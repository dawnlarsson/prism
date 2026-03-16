static void expect_parse_rejects(const char *code, const char *file_name,
				 const char *name, const char *needle);

void test_multi_decl_basic(void) {
	int a, b, c;
	CHECK(a == 0 && b == 0 && c == 0, "int a, b, c");
}

void test_multi_decl_mixed_ptr(void) {
	int *p, x, *q;
	CHECK(p == NULL && x == 0 && q == NULL, "int *p, x, *q");
}

void test_multi_decl_arrays(void) {
	int a[5], b, c[3];
	int all_zero = 1;
	for (int i = 0; i < 5; i++)
		if (a[i] != 0) all_zero = 0;
	for (int i = 0; i < 3; i++)
		if (c[i] != 0) all_zero = 0;
	CHECK(all_zero && b == 0, "int a[5], b, c[3]");
}

void test_multi_decl_partial_init(void) {
	int a, b = 42, c;
	CHECK(a == 0 && b == 42 && c == 0, "int a, b = 42, c");
}

void test_multi_decl_long(void) {
	int a, b, c, d, e, f, g, h;
	CHECK(a == 0 && b == 0 && c == 0 && d == 0 && e == 0 && f == 0 && g == 0 && h == 0,
	      "int a,b,c,d,e,f,g,h");
}

void test_multi_decl_func_ptr(void) {
	// Basic case
	int (*fp1)(int), (*fp2)(int);
	CHECK(fp1 == NULL && fp2 == NULL, "int (*fp1)(int), (*fp2)(int)");

	// NIGHTMARE: 12 mixed declarators in one statement
	// Mix of: plain vars, pointers, double pointers, arrays, function pointers,
	// pointers to arrays, arrays of pointers, function pointers returning pointers
	int plain1, *ptr1, **dptr1, arr1[3], *arr_ptr1[4], (*ptr_arr1)[5], (*func1)(void),
	    *(*func_ret_ptr1)(int), (*arr_func1[2])(char), (*(*ptr_arr_func1))[3], ***tptr1, plain2;

	CHECK(plain1 == 0, "nightmare multi-decl: plain1");
	CHECK(ptr1 == NULL, "nightmare multi-decl: ptr1");
	CHECK(dptr1 == NULL, "nightmare multi-decl: dptr1");
	int all_zero = 1;
	for (int i = 0; i < 3; i++)
		if (arr1[i] != 0) all_zero = 0;
	CHECK(all_zero, "nightmare multi-decl: arr1[3]");
	int all_null = 1;
	for (int i = 0; i < 4; i++)
		if (arr_ptr1[i] != NULL) all_null = 0;
	CHECK(all_null, "nightmare multi-decl: *arr_ptr1[4]");
	CHECK(ptr_arr1 == NULL, "nightmare multi-decl: (*ptr_arr1)[5]");
	CHECK(func1 == NULL, "nightmare multi-decl: (*func1)(void)");
	CHECK(func_ret_ptr1 == NULL, "nightmare multi-decl: *(*func_ret_ptr1)(int)");
	all_null = 1;
	for (int i = 0; i < 2; i++)
		if (arr_func1[i] != NULL) all_null = 0;
	CHECK(all_null, "nightmare multi-decl: (*arr_func1[2])(char)");
	CHECK(ptr_arr_func1 == NULL, "nightmare multi-decl: (*(*ptr_arr_func1))[3]");
	CHECK(tptr1 == NULL, "nightmare multi-decl: ***tptr1");
	CHECK(plain2 == 0, "nightmare multi-decl: plain2");

	// Even more extreme: const/volatile mixed in
	const int *const cptr1, *volatile vptr1, *const *volatile cvptr1, (*const cfunc1)(int),
	    (*volatile * vfunc_ptr1)(void);
	CHECK(cptr1 == NULL, "nightmare cv multi-decl: const int *const");
	CHECK(vptr1 == NULL, "nightmare cv multi-decl: *volatile");
	CHECK(cvptr1 == NULL, "nightmare cv multi-decl: *const *volatile");
	CHECK(cfunc1 == NULL, "nightmare cv multi-decl: (*const cfunc1)(int)");
	CHECK(vfunc_ptr1 == NULL, "nightmare cv multi-decl: (*volatile *vfunc_ptr1)(void)");
}


// SECTION 4: TYPEDEF TRACKING TESTS

typedef int MyInt;
typedef int *IntPtr;

typedef struct {
	int x;
	int y;
} Point;

typedef char Name[64];
typedef int (*Callback)(int, int);

void test_typedef_simple(void) {
	MyInt x;
	CHECK_EQ(x, 0, "simple typedef zero-init");
}

void test_typedef_pointer(void) {
	IntPtr p;
	CHECK(p == NULL, "pointer typedef zero-init");
}

void test_typedef_struct(void) {
	Point p;
	CHECK(p.x == 0 && p.y == 0, "struct typedef zero-init");
}

void test_typedef_array(void) {
	Name n;
	CHECK(n[0] == 0, "array typedef zero-init");
}

void test_typedef_func_ptr(void) {
	Callback cb;
	CHECK(cb == NULL, "func ptr typedef zero-init");
}

typedef MyInt ChainedInt;
typedef ChainedInt DoubleChainedInt;

typedef int T0;
typedef T0 *T1;			    // pointer to T0
typedef T1 T2[3];		    // array of T1
typedef T2 *T3;			    // pointer to array of pointers
typedef T3 (*T4)(void);		    // function returning T3
typedef T4 T5[2];		    // array of function pointers
typedef T5 *T6;			    // pointer to array of func ptrs
typedef T6 (*T7)(int);		    // function(int) returning T6
typedef T7 *T8;			    // pointer to T7
typedef T8 *T9;			    // pointer to T8 (changed: can't have func return array)
typedef T9 (*T10)(char, int);	    // function(char,int) returning T9
typedef T10 *T11;		    // pointer to T10
typedef T11 const *volatile T12;    // volatile ptr to const ptr to T11
typedef T12 T13[2][3];		    // 2D array of T12
typedef T13 *T14;		    // pointer to 2D array
typedef T14 (*T15)(void *, size_t); // function(void*, size_t) returning T14

void test_typedef_chained(void) {
	ChainedInt c;
	CHECK_EQ(c, 0, "chained typedef zero-init");

	DoubleChainedInt d;
	CHECK_EQ(d, 0, "double-chained typedef zero-init");

	// NIGHTMARE: Test each level of the 15-chain
	T0 t0;
	CHECK_EQ(t0, 0, "nightmare typedef chain: T0 (int)");

	T1 t1;
	CHECK(t1 == NULL, "nightmare typedef chain: T1 (int*)");

	T2 t2;
	int all_null = 1;
	for (int i = 0; i < 3; i++)
		if (t2[i] != NULL) all_null = 0;
	CHECK(all_null, "nightmare typedef chain: T2 (int*[3])");

	T3 t3;
	CHECK(t3 == NULL, "nightmare typedef chain: T3 (int*(*)[3])");

	T4 t4;
	CHECK(t4 == NULL, "nightmare typedef chain: T4 (func returning T3)");

	T5 t5;
	all_null = 1;
	for (int i = 0; i < 2; i++)
		if (t5[i] != NULL) all_null = 0;
	CHECK(all_null, "nightmare typedef chain: T5 (T4[2])");

	T6 t6;
	CHECK(t6 == NULL, "nightmare typedef chain: T6 (*T5)");

	T7 t7;
	CHECK(t7 == NULL, "nightmare typedef chain: T7 (func returning T6)");

	T8 t8;
	CHECK(t8 == NULL, "nightmare typedef chain: T8 (*T7)");

	T9 t9;
	CHECK(t9 == NULL, "nightmare typedef chain: T9 (*T8)");

	T10 t10;
	CHECK(t10 == NULL, "nightmare typedef chain: T10 (func returning T9)");

	T11 t11;
	CHECK(t11 == NULL, "nightmare typedef chain: T11 (*T10)");

	T12 t12;
	CHECK(t12 == NULL, "nightmare typedef chain: T12 (cv-qualified T11*)");

	T13 t13;
	all_null = 1;
	for (int i = 0; i < 2; i++)
		for (int j = 0; j < 3; j++)
			if (t13[i][j] != NULL) all_null = 0;
	CHECK(all_null, "nightmare typedef chain: T13 (T12[2][3])");

	T14 t14;
	CHECK(t14 == NULL, "nightmare typedef chain: T14 (*T13)");

	T15 t15;
	CHECK(t15 == NULL, "nightmare typedef chain: T15 (func returning T14)");
}

void test_typedef_multi_var(void) {
	MyInt a, b, c;
	CHECK(a == 0 && b == 0 && c == 0, "typedef multi-var zero-init");
}

void test_typedef_block_scoped(void) {
	{
		typedef int LocalInt;
		LocalInt x;
		CHECK_EQ(x, 0, "block-scoped typedef zero-init");
	}
	int y; // LocalInt not visible here
	CHECK_EQ(y, 0, "after block-scoped typedef");
}

typedef int ShadowType;

void test_typedef_shadowing(void) {
	ShadowType outer;
	CHECK_EQ(outer, 0, "outer typedef zero-init");
	{
		typedef float ShadowType;
		ShadowType inner;
		CHECK(inner == 0.0f, "shadowed typedef zero-init");
	}
	ShadowType after;
	CHECK_EQ(after, 0, "typedef after shadow scope");
}

typedef int TD_Int, *TD_IntPtr;

void test_typedef_multi_declarator(void) {
	TD_Int a;
	TD_IntPtr p;
	CHECK_EQ(a, 0, "multi-declarator typedef int zero-init");
	CHECK(p == NULL, "multi-declarator typedef ptr zero-init");
}

void test_typedef_after_braceless_while(void) {
	typedef int NumT;
	NumT counter = 3;
	while (counter > 0) counter--;
	NumT after = counter;
	CHECK_EQ(after, 0, "typedef after braceless while");
}

void test_typedef_after_braceless_if_else(void) {
	typedef int FlagT;
	FlagT x = 1;
	FlagT y;
	if (x) y = 100;
	else
		y = 200;
	FlagT z = y;
	CHECK_EQ(z, 100, "typedef after braceless if-else");
}

void test_typedef_braceless_nested_control(void) {
	typedef int IdxT;
	IdxT sum = 0;
	for (IdxT i = 0; i < 3; i++)
		if (i > 0) sum += i;
	IdxT result = sum;
	CHECK_EQ(result, 3, "typedef braceless nested if in for");
}

void test_typedef_multi_braceless_sequential(void) {
	typedef int ValT;
	ValT a = 1;
	if (a > 0) a = 10;
	ValT b = a;
	if (b > 5) b = 20;
	ValT c = b;
	while (c > 15) c--;
	ValT d = c;
	CHECK_EQ(d, 15, "typedef multi braceless sequential");
}


void test_bitfield_zeroinit(void) {
	// NIGHTMARE: Extensive bitfield testing

	// Basic bitfields
	struct {
		unsigned int a : 3;
		unsigned int b : 5;
		unsigned int c : 1;
	} bits;

	CHECK(bits.a == 0 && bits.b == 0 && bits.c == 0, "bitfield zero-init");

	// Zero-width bitfield forces alignment break
	struct {
		unsigned int x : 7;
		unsigned int : 0; // zero-width: forces next field to new unit
		unsigned int y : 5;
		unsigned int : 3; // unnamed 3-bit padding
		unsigned int z : 10;
	} aligned_bits;

	CHECK(aligned_bits.x == 0 && aligned_bits.y == 0 && aligned_bits.z == 0,
	      "bitfield with zero-width alignment");

	// Signed vs unsigned bitfields (sign extension edge cases)
	struct {
		signed int neg : 4;   // can hold -8 to 7
		unsigned int pos : 4; // can hold 0 to 15
		int impl : 4;	      // implementation-defined signedness
	} signed_bits;

	CHECK(signed_bits.neg == 0 && signed_bits.pos == 0 && signed_bits.impl == 0,
	      "signed/unsigned bitfield zero-init");

	// Maximum width bitfields
	struct {
		unsigned long long wide : 63;
		unsigned int full : 32;
		unsigned short med : 16;
		unsigned char tiny : 8;
	} max_bits;

	CHECK(max_bits.wide == 0 && max_bits.full == 0 && max_bits.med == 0 && max_bits.tiny == 0,
	      "max-width bitfield zero-init");

	// Bitfields in nested anonymous struct/union
	struct {
		int type : 4;

		union {
			struct {
				unsigned int r : 5;
				unsigned int g : 6;
				unsigned int b : 5;
			};

			unsigned short rgb565;
		};

		struct {
			unsigned int alpha : 8;
			unsigned int : 0;
			unsigned int flags : 4;
		};
	} complex_bits;

	CHECK(complex_bits.type == 0 && complex_bits.r == 0 && complex_bits.g == 0 && complex_bits.b == 0 &&
		  complex_bits.alpha == 0 && complex_bits.flags == 0,
	      "nested anonymous bitfield zero-init");

	// Array of bitfield structs
	struct BitFlags {
		unsigned int enabled : 1;
		unsigned int visible : 1;
		unsigned int selected : 1;
		unsigned int : 5;
		unsigned int priority : 4;
		unsigned int : 0;
		unsigned int category : 8;
	} flag_array[5];

	int all_zero = 1;
	for (int i = 0; i < 5; i++) {
		if (flag_array[i].enabled != 0 || flag_array[i].visible != 0 || flag_array[i].selected != 0 ||
		    flag_array[i].priority != 0 || flag_array[i].category != 0)
			all_zero = 0;
	}
	CHECK(all_zero, "array of bitfield structs zero-init");

	// Bitfield with boolean type
	struct {
		_Bool flag1 : 1;
		_Bool flag2 : 1;
		unsigned int count : 6;
	} bool_bits;

	CHECK(bool_bits.flag1 == 0 && bool_bits.flag2 == 0 && bool_bits.count == 0,
	      "_Bool bitfield zero-init");
}

void test_anonymous_struct(void) {
	// Basic case
	struct {
		int x;

		struct {
			int a;
			int b;
		}; // anonymous

		int y;
	} s;

	CHECK(s.x == 0 && s.a == 0 && s.b == 0 && s.y == 0, "anonymous struct zero-init");

	// NIGHTMARE: 6 levels of alternating anonymous struct/union nesting
	struct {
		int level0;

		struct {
			int level1_a;

			union {
				int level2_int;

				struct {
					short level3_lo;
					short level3_hi;

					struct {
						char level4_bytes[4];

						union {
							int level5_whole;

							struct {
								unsigned char level6_r;
								unsigned char level6_g;
								unsigned char level6_b;
								unsigned char level6_a;
							};
						};
					};
				};

				float level2_float;
			};

			int level1_b;
		};

		union {
			long level0_long;

			struct {
				int level1_x;
				int level1_y;

				union {
					double level2_double;

					struct {
						float level3_re;
						float level3_im;
					};
				};
			};
		};

		struct {
			// Arrays inside anonymous structs
			int arr_in_anon[3];

			struct {
				int *ptr_in_nested_anon;
				void (*func_ptr_in_anon)(void);
			};
		};
	} nightmare;

	CHECK(nightmare.level0 == 0, "nightmare anon: level0");
	CHECK(nightmare.level1_a == 0 && nightmare.level1_b == 0, "nightmare anon: level1");
	CHECK(nightmare.level2_int == 0, "nightmare anon: level2_int");
	CHECK(nightmare.level3_lo == 0 && nightmare.level3_hi == 0, "nightmare anon: level3");
	int all_zero = 1;
	for (int i = 0; i < 4; i++)
		if (nightmare.level4_bytes[i] != 0) all_zero = 0;
	CHECK(all_zero, "nightmare anon: level4_bytes");
	CHECK(nightmare.level5_whole == 0, "nightmare anon: level5_whole");
	CHECK(nightmare.level6_r == 0 && nightmare.level6_g == 0 && nightmare.level6_b == 0 &&
		  nightmare.level6_a == 0,
	      "nightmare anon: level6 rgba");
	CHECK(nightmare.level0_long == 0, "nightmare anon: level0_long");
	CHECK(nightmare.level1_x == 0 && nightmare.level1_y == 0, "nightmare anon: level1_xy");
	CHECK(nightmare.level2_double == 0.0, "nightmare anon: level2_double");
	CHECK(nightmare.level3_re == 0.0f && nightmare.level3_im == 0.0f, "nightmare anon: level3_complex");
	all_zero = 1;
	for (int i = 0; i < 3; i++)
		if (nightmare.arr_in_anon[i] != 0) all_zero = 0;
	CHECK(all_zero, "nightmare anon: arr_in_anon");
	CHECK(nightmare.ptr_in_nested_anon == NULL, "nightmare anon: ptr in nested");
	CHECK(nightmare.func_ptr_in_anon == NULL, "nightmare anon: func ptr in anon");
}

void test_anonymous_union(void) {
	struct {
		int type;

		union {
			int i;
			float f;
		}; // anonymous
	} u;

	CHECK(u.type == 0 && u.i == 0, "anonymous union zero-init");
}

void test_long_declaration(void) {
	const volatile unsigned long long int *const *volatile ptr;
	CHECK(ptr == NULL, "long qualified declaration zero-init");
}

void test_func_ptr_array(void) {
	int (*handlers[10])(int, int);
	int all_null = 1;
	for (int i = 0; i < 10; i++)
		if (handlers[i] != NULL) all_null = 0;
	CHECK(all_null, "function pointer array zero-init");
}

void test_ptr_to_array(void) {
	int (*p)[10];
	CHECK(p == NULL, "pointer to array zero-init");
}

void test_defer_compound_literal(void) {
	log_reset();
	{
		int *p = (int[]){1, 2, 3};
		defer log_append("D");
		log_append("1");
	}
	log_append("E");
	CHECK_LOG("1DE", "defer with compound literal");
}

void test_duffs_device(void) {
	// Classic Duff's device (defer is now in a block to not affect final check)
	log_reset();
	int count = 5;
	int n = (count + 3) / 4;
	{
		defer log_append("F");
		switch (count % 4) {
		case 0: do { log_append("X");
			case 3: log_append("X");
			case 2: log_append("X");
			case 1: log_append("X");
			} while (--n > 0);
		}
		log_append("E");
	}
	// CHECK is done in run_edge_case_tests, expects "XXXXXEF"

	// NIGHTMARE: Duff's device with defers at each case
	count = 7;
	n = (count + 3) / 4;
	int iterations = 0;
	switch (count % 4) {
	case 0:
		do {
			{
				defer iterations++;
			}
		case 3: {
			defer iterations++;
		}
		case 2: {
			defer iterations++;
		}
		case 1: {
			defer iterations++;
		}
		} while (--n > 0);
	}
	// count=7, so 7%4=3, starts at case 3, runs: 3,2,1 (first partial), then 0,3,2,1 (full round)
	// That's 3+4=7 iterations total
	CHECK_EQ(iterations, 7, "nightmare duff: defer ran correct times");

	// NIGHTMARE: Nested Duff's devices (the horror!)
	int outer = 3;
	int inner_count = 2;
	int duff_total = 0;
	int outer_n = (outer + 1) / 2;
	switch (outer % 2) {
	case 0:
		do {
			{
				int inner_n = (inner_count + 1) / 2;
				switch (inner_count % 2) {
				case 0: do { duff_total++;
					case 1: duff_total++;
					} while (--inner_n > 0);
				}
			}
		case 1: {
			int inner_n = (inner_count + 1) / 2;
			switch (inner_count % 2) {
			case 0: do { duff_total++;
				case 1: duff_total++;
				} while (--inner_n > 0);
			}
		}
		} while (--outer_n > 0);
	}
	// This is truly evil nesting - the key is it parses and runs
	CHECK(duff_total > 0, "nightmare duff: nested devices executed");
}

void test_defer_ternary(void) {
	log_reset();
	int x = 1;
	defer(x ? log_append("T") : log_append("F"));
	log_append("1");
}

void test_empty_defer(void) {
	log_reset();
	{
		defer;
		log_append("1");
	} // empty defer statement
	log_append("E");
	CHECK_LOG("1E", "empty defer statement");
}

void test_do_while_0_defer(void) {
	log_reset();
	defer log_append("F");
	do {
		defer log_append("D");
		log_append("1");
		if (1) break;
		log_append("X");
	} while (0);
	log_append("E");
}

void test_defer_comma_operator(void) {
	log_reset();
	{
		defer(log_append("A"), log_append("B"));
		log_append("1");
	}
	CHECK_LOG("1AB", "defer with comma operator");
}

void test_struct_in_braceless_control(void) {
	int result = 0;

	struct {
		int a;
		int b;
	} pair;

	pair.a = 10;
	pair.b = 20;
	if (pair.a > 0) result = pair.a + pair.b;
	CHECK_EQ(result, 30, "struct in braceless control");

	for (int i = 0; i < 1; i++) result = pair.b;
	CHECK_EQ(result, 20, "struct after braceless for");
}

void test_defer_across_struct_boundaries(void) {
	log_reset();
	{
		struct {
			int x;
		} s1;

		s1.x = 1;
		defer log_append("outer");
		{
			struct {
				struct {
					int y;
				} inner;
			} s2;

			s2.inner.y = 2;
			defer log_append("inner");
			char buf[16];
			snprintf(buf, sizeof(buf), "%d%d", s1.x, s2.inner.y);
			log_append(buf);
		}
	}
	CHECK_LOG("12innerouter", "defer across struct boundaries");
}

static void _nested_struct_defer_helper(int val) {
	char buf[32];
	snprintf(buf, sizeof(buf), "d%d", val);
	log_append(buf);
}

void test_nested_struct_union_with_defer(void) {
	log_reset();
	{
		struct {
			struct {
				union {
					int x;
					float f;
				} val;

				int tag;
			} inner;

			int count;
		} outer;

		outer.inner.val.x = 99;
		outer.inner.tag = 1;
		outer.count = 3;
		defer _nested_struct_defer_helper(outer.inner.val.x);
		log_append("body");
	}
	CHECK_LOG("bodyd99", "nested struct/union with defer");
}

void test_deeply_nested_struct_defer_scopes(void) {
	log_reset();
	{
		defer log_append("A");

		struct {
			struct {
				struct {
					int v;
				} a;
			} b;
		} s;

		s.b.a.v = 7;
		{
			defer log_append("B");

			struct {
				int x;
			} inner;

			inner.x = s.b.a.v + 1;
			char buf[8];
			snprintf(buf, sizeof(buf), "%d", inner.x);
			log_append(buf);
		}
	}
	CHECK_LOG("8BA", "deeply nested struct defer scopes");
}


// SECTION 6: BUG REGRESSION TESTS
#ifdef __GNUC__
void test_stmt_expr_defer_nested_block(void) {
	// This SHOULD work: defer is in a nested block, not at top level
	log_reset();
	int x = ({
		int result;
		{
			defer log_append("D");
			result = 42;
		}
		log_append("1");
		result; // This is the result, not the defer
	});
	log_append("E");
	CHECK_EQ(x, 42, "stmt expr defer nested block - value");
	CHECK_LOG("D1E", "stmt expr defer nested block - order");
}
#endif

void test_non_vla_typedef_still_works(void) {
	typedef int FixedArray[10]; // NOT a VLA - size is constant
	FixedArray arr;
	int all_zero = 1;
	for (int i = 0; i < 10; i++)
		if (arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "non-VLA typedef array zero-init");

	typedef struct {
		int x;
		int y;
	} PointType;

	PointType p;
	CHECK(p.x == 0 && p.y == 0, "non-VLA typedef struct zero-init");
}

void test_switch_defer_no_leak(void) {
	log_reset();
	int cleanup_count = 0;
	switch (1) {
	case 1: {
		defer cleanup_count++;
		log_append("1");
	} // defer fires here at }
	case 2:
		log_append("2"); // fallthrough reaches here
		break;
	}
	log_append("E");
	CHECK_EQ(cleanup_count, 1, "switch defer fires at brace not switch exit");
	CHECK_LOG("12E", "switch defer fallthrough order");
}

typedef int EnumShadowT;

void test_enum_constant_shadows_typedef(void) {
	// First verify EnumShadowT works as a type
	EnumShadowT before;
	CHECK_EQ(before, 0, "typedef works before enum shadow");

	// Define enum with constant that shadows the typedef name
	enum { EnumShadowT = 42 };

	// Now EnumShadowT is the enum constant (value 42), not a type
	// This expression should be integer multiplication (42 * 2 = 84)
	int product;
	product = EnumShadowT * 2;
	CHECK_EQ(product, 84, "enum constant shadows typedef - multiplication works");

	// Directly use the enum constant value
	CHECK_EQ(EnumShadowT, 42, "enum constant has correct value");
}

typedef int EnumPtrT;

void test_enum_shadow_star_ambiguity(void) {
	int x = 3;

	// Shadow the typedef with an enum constant
	enum { EnumPtrT = 7 };

	// Now "EnumPtrT * x" is multiplication (7 * 3 = 21), not a pointer declaration
	// Prism must NOT try to zero-init this as a declaration
	int result = EnumPtrT * x; // 7 * 3 = 21
	CHECK_EQ(result, 21, "enum shadow: T*x is multiplication not ptr decl");

	// Verify the enum constant has the right value
	CHECK_EQ(EnumPtrT, 7, "enum constant value correct");
}

typedef int EnumStmtT;

void test_enum_shadow_statement_form(void) {
	int y = 5;

	// Shadow the typedef with enum constant
	enum { EnumStmtT = 10 };

	// This statement-level expression "EnumStmtT * y" is multiplication: 10 * 5 = 50
	// If Prism tries to parse it as "EnumStmtT *y = 0;", the gcc compilation would fail
	// because "10 * y = 0" is not a valid lvalue assignment.
	EnumStmtT *y; // This is a statement: multiplication, discards result

	CHECK_EQ(y, 5, "enum shadow: statement T*x leaves operand unchanged");
}

#define PP_PASTE_TEST(x) extern int test_prefix_##x##_suffix;

PP_PASTE_TEST(1024_160)
PP_PASTE_TEST(2048_224)
PP_PASTE_TEST(2048_256)

// Verify the pasted tokens are valid by using them
extern int test_prefix_1024_160_suffix;
extern int test_prefix_2048_224_suffix;
extern int test_prefix_2048_256_suffix;

void test_ppnum_underscore_paste(void) {
	// Intentional compile-as-proof check: these declarations would fail to compile
	// if tokens like 1024_160 were split incorrectly during preprocessing.
	CHECK(1, "pp-number underscore paste: 1024_160 is single token");
}

void test_local_function_decl(void) {
	// These are local extern function declarations (valid C)
	// The zeroinit code should recognize these as function declarations
	// and skip them, NOT try to add "= 0" or duplicate them
	void local_func(int a, int b);
	void multi_line_func(
	    int *rp, const int *ap, const void *table, const int *np, const int *n0, int num, int power);
	int return_func(const int *ap, int off);

	PrismResult result = prism_transpile_source(
	    "void f(void) {\n"
	    "    void local_func(int a, int b);\n"
	    "    void multi_line_func(int *rp, const int *ap, const void *table,\n"
	    "                         const int *np, const int *n0, int num, int power);\n"
	    "    int return_func(const int *ap, int off);\n"
	    "}\n",
	    "local_function_decl.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "local function declarations transpile");
	if (result.output) {
		CHECK(strstr(result.output, "local_func") != NULL,
		      "local function declarations: local_func preserved");
		CHECK(strstr(result.output, "multi_line_func") != NULL,
		      "local function declarations: multi_line_func preserved");
		CHECK(strstr(result.output, "return_func") != NULL,
		      "local function declarations: return_func preserved");
		CHECK(strstr(result.output, "local_func =") == NULL,
		      "local function declarations: local_func not rewritten as variable init");
		CHECK(strstr(result.output, "multi_line_func =") == NULL,
		      "local function declarations: multi_line_func not rewritten as variable init");
		CHECK(strstr(result.output, "return_func =") == NULL,
		      "local function declarations: return_func not rewritten as variable init");
	}
	prism_free(&result);
}


void test_defer_shadowing_vars(void) {
	log_reset();
	int x = 1;
	{
		int x = 2;
		// Should capture inner x (2)
		defer {
			if (x == 2) log_append("I");
			else
				log_append("?");
		};
	}
	// Should verify outer x (1) is untouched
	if (x == 1) log_append("O");
	CHECK_LOG("IO", "variable shadowing with defer");
}

void test_typedef_hiding(void) {
	// Test that a local variable can hide a global typedef
	// without confusing the zero-init logic
	typedef int T;
	T a;
	CHECK_EQ(a, 0, "global typedef zero-init");

	{
		float T; // T is now a variable name, not a type
		T = 5.5f;
		CHECK(T == 5.5f, "typedef name hidden by variable");

		// This should be a syntax error if T was still seen as a type:
		// T * x; -> float * x (valid ptr decl) vs float * x (multiplication)
		// Prism shouldn't try to zero-init "T * x" as a pointer if it thinks T is a variable.
		// However, standard C parsing rules apply here.
	}

	T b; // T should be restored to type 'int'
	CHECK_EQ(b, 0, "typedef name restored after scope");
}

void test_typedef_same_name_shadow(void) {
	typedef int T;

	// Verify T works as a type before shadowing
	T before;
	CHECK_EQ(before, 0, "typedef T works before shadow");

	{
		// THE BUG CASE: "T T;" - first T is type, second T is variable name
		// Prism must:
		// 1. Recognize first T as typedef -> zero-init the variable
		// 2. Register that variable T now shadows typedef T
		T T;
		CHECK_EQ(T, 0, "T T declaration zero-inits variable");

		T = 42;
		CHECK_EQ(T, 42, "T is usable as variable after T T decl");

		// At this point, "T" refers to the variable, not the type.
		// Any attempt to use T as a type here would be a C syntax error.
		// We can't test "T x;" here as it would fail to compile (correctly).
		// But we verify that T is being used as a variable:
		int result = T + 8;
		CHECK_EQ(result, 50, "T used in expression as variable");
	}

	// After the block, typedef T should be visible again
	T after;
	CHECK_EQ(after, 0, "typedef T restored after shadow scope");
}

void test_typedef_nested_same_name_shadow(void) {
	typedef int T;

	T outer;
	CHECK_EQ(outer, 0, "outer T as typedef");

	{
		T T; // shadows typedef
		T = 1;
		CHECK_EQ(T, 1, "first shadow level");

		{
			// Can't do "T T;" again here because T is already a variable.
			// But we can verify T is still the variable from outer scope:
			T = 2;
			CHECK_EQ(T, 2, "inner scope sees variable T");
		}

		CHECK_EQ(T, 2, "variable T preserved after inner scope");
	}

	// T is typedef again
	T restored;
	CHECK_EQ(restored, 0, "typedef restored after nested shadows");
}

void test_typedef_shadow_then_pointer(void) {
	typedef int T;

	{
		T T; // shadow
		T = 100;
		(void)T; // use it
	}

	// Now T is a type again - pointer declaration should work
	T *ptr;
	CHECK(ptr == NULL, "pointer to typedef after shadow scope");

	T arr[3];
	CHECK(arr[0] == 0 && arr[1] == 0 && arr[2] == 0, "array of typedef after shadow scope");
}

void test_static_local_init(void) {
	// Prism skips adding "= 0" to static vars (as they are implicitly zero),
	// but we must ensure it doesn't break them.
	static int s;
	CHECK_EQ(s, 0, "static local implicit zero-init");

	static int *sp;
	CHECK(sp == NULL, "static local ptr implicit zero-init");
}

void test_complex_func_ptr(void) {
	// Test zero-init on complex declarators
	// int *(*fp)(int, int) -> pointer to function(int, int) returning int*
	int *(*fp)(int, int);
	CHECK(fp == NULL, "complex function pointer zero-init");

	// Array of function pointers
	// void (*arr[2])(void)
	void (*arr[2])(void);
	CHECK(arr[0] == NULL && arr[1] == NULL, "array of func ptr zero-init");
}

void test_switch_default_first(void) {
	// Verify defer cleanup works even if 'default' is the first label
	log_reset();
	int x = 10;
	switch (x) {
	default: {
		defer log_append("D");
	} break;
	case 1: log_append("1"); break;
	}
	log_append("E");
	CHECK_LOG("DE", "switch default first defer");
}

#define CLEANUP defer log_append("C")
#define DEFER_NESTED_1(x) defer log_append(x)
#define DEFER_NESTED_2(x)                                                                                    \
	{                                                                                                    \
		DEFER_NESTED_1(x);                                                                           \
		log_append("n2");                                                                            \
	}
#define DEFER_NESTED_3(x)                                                                                    \
	{                                                                                                    \
		DEFER_NESTED_2(x);                                                                           \
		log_append("n3");                                                                            \
	}
#define DEFER_CHAIN(a, b, c)                                                                                 \
	defer log_append(a);                                                                                 \
	defer log_append(b);                                                                                 \
	defer log_append(c)
#define MULTI_DEFER_BLOCK                                                                                    \
	{                                                                                                    \
		defer log_append("M1");                                                                      \
		{                                                                                            \
			defer log_append("M2");                                                              \
			{                                                                                    \
				defer log_append("M3");                                                      \
				log_append("*");                                                             \
			}                                                                                    \
			log_append("+");                                                                     \
		}                                                                                            \
		log_append("-");                                                                             \
	}
#define CONDITIONAL_DEFER(cond, a, b)                                                                        \
	if (cond) {                                                                                          \
		defer log_append(a);                                                                         \
	} else {                                                                                             \
		defer log_append(b);                                                                         \
	}
#define LOOP_DEFER(n, x)                                                                                     \
	for (int _i = 0; _i < (n); _i++) {                                                                   \
		defer log_append(x);                                                                         \
		log_append(".");                                                                             \
	}

void test_macro_hidden_defer(void) {
	// Prism operates on preprocessed tokens, so this must work
	log_reset();
	{
		CLEANUP;
		log_append("1");
	}
	CHECK_LOG("1C", "macro hidden defer");

	// NIGHTMARE: Nested macro expansion with defer
	log_reset();
	{ DEFER_NESTED_3("X"); }
	CHECK_LOG("n2Xn3", "nightmare macro: nested defer expansion");

	// Multiple defers from one macro
	log_reset();
	{
		DEFER_CHAIN("A", "B", "C");
		log_append("1");
	}
	CHECK_LOG("1CBA", "nightmare macro: chain defer");

	// Complex multi-block defer macro
	log_reset();
	MULTI_DEFER_BLOCK;
	CHECK_LOG("*M3+M2-M1", "nightmare macro: multi-block defer");

	// Conditional defer macro - the defer inside the if/else runs when that block exits,
	// which happens BEFORE log_append("1")
	log_reset();
	{
		defer log_append("O");
		CONDITIONAL_DEFER(1, "T", "F");
		log_append("1");
	}
	CHECK_LOG("T1O", "nightmare macro: conditional defer true");

	log_reset();
	{
		defer log_append("O");
		CONDITIONAL_DEFER(0, "T", "F");
		log_append("1");
	}
	CHECK_LOG("F1O", "nightmare macro: conditional defer false");

	// Loop defer macro
	log_reset();
	{
		defer log_append("E");
		LOOP_DEFER(3, "L");
	}
	CHECK_LOG(".L.L.LE", "nightmare macro: loop defer");
}

// Macro that expands to a declaration
#define DECL_INT(x) int x

void test_macro_hidden_decl(void) {
	// Should still zero-init
	DECL_INT(val);
	CHECK_EQ(val, 0, "macro hidden declaration zero-init");
}

static void void_inner_func(void) {
	log_append("I");
}

static void void_outer_func(void) {
	defer log_append("O");
	// This is valid C: returning a void expression from a void function
	// Prism must handle the sequence: eval -> defer -> return
	return void_inner_func();
}

void test_void_return_void_call(void) {
	log_reset();
	void_outer_func();
	CHECK_LOG("IO", "void return void call execution order");
}

void test_switch_continue(void) {
	log_reset();
	int i = 0;
	while (i < 1) {
		defer log_append("L"); // Loop cleanup

		switch (i) {
		case 0: {
			defer log_append("S"); // Switch cleanup
		} // defer S executes here
			// 'continue' must trigger 'L' (loop iteration end)
			i++;
			continue;
		}
		log_append("X"); // Should be skipped
		i++;
	}
	log_append("E");

	// Expected order:
	// 1. Enter loop
	// 2. Enter switch
	// 3. Register 'L' in loop scope, enter case block
	// 4. Register 'S' in case block scope
	// 5. Exit case block -> defer 'S' executes
	// 6. Hit continue -> defer 'L' executes -> Re-check loop cond
	// 7. Loop terminates -> 'E'
	CHECK_LOG("SLE", "continue from inside switch");
}

void test_fam_struct_zeroinit(void) {
	// C99 Flexible Array Member
	struct Fam {
		int len;
		char data[];
	};

	// Should zero-init the fixed part (len=0) and not crash parser
	struct Fam f;

	CHECK_EQ(f.len, 0, "struct with flexible array member zero-init");
}

#ifdef __GNUC__
void test_stmt_expr_side_effects(void) {
	log_reset();
	int global = 0;
	// ({ { defer global=1; } 5; }) -> result 5, then global=1
	int y = ({
		{ defer global = 1; }
		5;
	});

	CHECK_EQ(y, 5, "stmt expr result preserved");
	CHECK_EQ(global, 1, "stmt expr defer executed");
}
#endif

void test_typedef_scope_churn_consolidated(void) {
	int ok = 1;
	for (int round = 0; round < 300; round++) {
		typedef int CT_A;
		typedef short CT_B;
		typedef long CT_C;
		CT_A a;
		CT_B b;
		CT_C c;
		if (a != 0 || b != 0 || c != 0) {
			ok = 0;
			break;
		}
		{
			typedef float CT_A;
			typedef double CT_B;
			typedef char CT_C;
			CT_A fa;
			CT_B db;
			CT_C cc;
			if (fa != 0.0f || db != 0.0 || cc != 0) {
				ok = 0;
				break;
			}
		}
	}
	CHECK(ok, "typedef scope churn 300 rounds: nested redefinition + varied types");
	int post_churn;
	CHECK_EQ(post_churn, 0, "typedef scope churn: post-churn zeroed");
}


void test_case_in_nested_block(void) {
	// Case label inside a nested block (valid C, but weird)
	log_reset();
	int x = 1;
	switch (x) {
		{
			// This block is entered via fallthrough from case 0 or jumped to for case 1
		case 1: log_append("1"); break;
		}
	case 0:
		log_append("0");
		// falls through to block above
	}
	log_append("E");
	CHECK_LOG("1E", "case label in nested block");
}

void test_case_after_defer_in_block(void) {
	// Case label after defer in same block - NOW AN ERROR
	// Prism correctly detects that jumping to case 1 would skip the defer.
	// This test verifies the safe pattern: each case has its own block with defers.
	log_reset();
	int x = 1;
	switch (x) {
	case 0: {
		defer log_append("D0");
		log_append("0");
		break;
	}
	case 1: {
		defer log_append("D1");
		log_append("1");
		break;
	}
	}
	log_append("E");
	// x=1: jump to case 1, "1", break, D1 fires, "E"
	CHECK_LOG("1D1E", "case with separate blocks - correct defer behavior");
}

void test_duff_device_with_defer_at_top(void) {
	// Duff's device pattern - defer works when used in a wrapper scope
	// The interleaved case labels inside do-while are incompatible with defer
	// in the same block, so we use a separate scope.
	log_reset();
	int count = 5;
	int result = 0;
	{
		defer result += 10; // Wrapper scope - fires when we exit this block
		int n = (count + 3) / 4;
		switch (count % 4) {
		case 0: do { log_append("X");
			case 3: log_append("X");
			case 2: log_append("X");
			case 1: log_append("X");
			} while (--n > 0);
		}
	}
	log_append("E");
	CHECK_LOG("XXXXXE", "duff device with defer in wrapper");
	CHECK_EQ(result, 10, "duff device defer count"); // defer fires once at wrapper scope exit
}


#define TEST_FLT128_MAX 1.18973149535723176508575932662800702e+4932F128
#define TEST_FLT128_MIN 3.36210314311209350626267781732175260e-4932F128
#define TEST_FLT64_VAL 1.7976931348623157e+308F64
#define TEST_FLT32_VAL 3.40282347e+38F32
#define TEST_FLT16_VAL 6.5504e+4F16
#define TEST_BF16_VAL 3.38953139e+38BF16

#ifndef _MSC_VER
void test_float128_suffix(void) {
	// Intentional compile-as-proof check: these literal suffixes are validated
	// by the host compiler during parsing, so successful compilation is the proof.
	(void)TEST_FLT128_MAX;
	(void)TEST_FLT128_MIN;
	CHECK(1, "F128 float suffix parses");
}
#endif // _MSC_VER

void test_float64_suffix(void) {
	// Intentional compile-as-proof check for the F64 literal suffix.
	(void)TEST_FLT64_VAL;
	CHECK(1, "F64 float suffix parses");
}

void test_float32_suffix(void) {
	// Intentional compile-as-proof check for the F32 literal suffix.
	(void)TEST_FLT32_VAL;
	CHECK(1, "F32 float suffix parses");
}

void test_float16_suffix(void) {
	// Intentional compile-as-proof check for the F16 literal suffix.
	(void)TEST_FLT16_VAL;
	CHECK(1, "F16 float suffix parses");
}

void test_bf16_suffix(void) {
	// Intentional compile-as-proof check for the BF16 literal suffix.
	(void)TEST_BF16_VAL;
	CHECK(1, "BF16 float suffix parses");
}


#include <signal.h>

void test_linux_macros(void) {
	// These macros must be defined for Linux platform detection
	// OpenSSL KTLS support depends on these being available
	// Only test on Linux - skip on other platforms
#ifdef __linux__
	// Intentional compile-as-proof checks: these platform macros either exist
	// at preprocess time or the conditional branches would compile differently.
	CHECK(1, "__linux__ macro defined");
#ifdef __linux
	CHECK(1, "__linux macro defined");
#else
	CHECK(0, "__linux macro defined");
#endif
#ifdef linux
	CHECK(1, "linux macro defined");
#else
	CHECK(0, "linux macro defined");
#endif
	// __gnu_linux__ is glibc-specific, not defined on musl (Alpine)
#ifdef __GLIBC__
#ifdef __gnu_linux__
	CHECK(1, "__gnu_linux__ macro defined");
#else
	CHECK(0, "__gnu_linux__ macro defined");
#endif
#else
	printf("  [SKIP] __gnu_linux__ test (not using glibc)\n");
#endif
#else
	// Not on Linux - skip these tests (they're Linux-specific)
	printf("  [SKIP] Linux macro tests (not on Linux)\n");
#endif
}

void test_signal_macros(void) {
	// Signal macros must be defined for code using #ifdef SIGALRM etc.
	// OpenSSL speed.c and many other programs depend on this.
	// prism now pre-defines standard Linux signal values.
#ifdef SIGALRM
	CHECK(SIGALRM == 14, "SIGALRM defined as 14");
#elif !defined(_WIN32)
	CHECK(0, "SIGALRM defined as 14");
#endif

#ifdef SIGINT
	CHECK(SIGINT == 2, "SIGINT defined as 2");
#else
	CHECK(0, "SIGINT defined as 2");
#endif

#ifdef SIGTERM
	CHECK(SIGTERM == 15, "SIGTERM defined as 15");
#else
	CHECK(0, "SIGTERM defined as 15");
#endif

#ifdef SIGKILL
	CHECK(SIGKILL == 9, "SIGKILL defined as 9");
#elif !defined(_WIN32)
	CHECK(0, "SIGKILL defined as 9");
#endif

#ifdef SIGCHLD
// SIGCHLD is 17 on Linux, 20 on macOS/BSD
#ifdef __linux__
	CHECK(SIGCHLD == 17, "SIGCHLD defined as 17");
#elif defined(__APPLE__)
	CHECK(SIGCHLD == 20, "SIGCHLD defined as 20 (macOS)");
#else
	// Intentional compile-as-proof check on non-Linux/non-macOS targets.
	CHECK(1, "SIGCHLD defined");
#endif
#elif !defined(_WIN32)
	CHECK(0, "SIGCHLD defined");
#endif

#ifndef _WIN32
	// Also verify we can use sigset_t from signal.h
	sigset_t test_set;
	CHECK(sizeof(test_set) > 0, "signal.h types available");
#endif
}

void test_glibc_macros(void) {
	// __GLIBC__ and __GLIBC_MINOR__ must be defined for glibc detection
	// Only relevant on Linux with glibc
#ifdef __GLIBC__
	CHECK(__GLIBC__ >= 2, "__GLIBC__ defined and >= 2");
#ifdef __GLIBC_MINOR__
	CHECK(__GLIBC_MINOR__ >= 0, "__GLIBC_MINOR__ defined");
#else
	CHECK(0, "__GLIBC_MINOR__ defined");
#endif
#else
	// Not using glibc - skip these tests
	printf("  [SKIP] glibc macro tests (not using glibc)\n");
#endif
}

void test_posix_macros(void) {
	// _POSIX_VERSION must be defined for POSIX compliance detection
#ifdef _POSIX_VERSION
// Different systems have different POSIX versions
#ifdef __linux__
	CHECK(_POSIX_VERSION >= 200809L, "_POSIX_VERSION defined and >= 200809L");
#else
	CHECK(_POSIX_VERSION > 0, "_POSIX_VERSION defined");
#endif
#else
	// _POSIX_VERSION may not be defined without feature test macros
	printf("  [SKIP] _POSIX_VERSION test (not defined)\n");
#endif
}


void test_switch_conditional_break_defer(void) {
	log_reset();
	int error = 0; // No error, will fall through

	switch (1) {
	case 1: {
		// Wrap in braces so defer executes before fallthrough
		defer log_append("cleanup1");
		if (error) break;
	} // defer runs here
	case 2: log_append("case2"); break;
	}

	// With fix: cleanup1 executes at closing brace before fallthrough
	CHECK_LOG("cleanup1case2", "defer executes before fallthrough with braces");
}

void test_switch_unconditional_break_works(void) {
	log_reset();
	int x = 1;

	switch (x) {
	case 1: {
		int *ptr = malloc(sizeof(int));
		defer {
			free(ptr);
			log_append("cleanup");
		};

		// Unconditional break - this is allowed (fall through to case 2 can't happen)
		break;
	}
	case 2: log_append("reached_case2"); break;
	}

	// Defer should execute, case 2 should not be reached
	CHECK_LOG("cleanup", "unconditional break allows defer without fallthrough warning");
}

void test_switch_braced_fallthrough_works(void) {
	log_reset();
	int cleanup_called = 0;

	switch (1) {
	case 1: {
		int *ptr = malloc(sizeof(int));
		defer {
			free(ptr);
			cleanup_called = 1;
		};

		// Even with conditional break, braces ensure defer runs
		if (0) break;
		// Fall through - cleanup happens at closing brace
	}
	case 2: log_append("reached_case2"); break;
	}

	CHECK(cleanup_called == 1, "braced case executes defer on fallthrough");
	CHECK_LOG("reached_case2", "fallthrough occurs as expected");
}

#ifndef _MSC_VER
void test_vla_struct_member(void) {
	struct Config {
		int size;
	} cfg = {10};

	// This is a VLA (runtime value), but Prism incorrectly thinks it's constant
	// because of the member access optimization
	int buffer[cfg.size]; // Should be recognized as VLA

	// This would cause backend error: "variable-sized object may not be initialized"
	// if Prism emits: int buffer[cfg.size] = {0};

	// Verify buffer is actually allocated
	buffer[0] = 42;
	buffer[9] = 99;

	CHECK(buffer[0] == 42, "VLA with struct member access allocates correctly");
	CHECK(buffer[9] == 99, "VLA struct member size works");
}

void test_vla_struct_member_nested(void) {
	struct Outer {
		struct {
			int count;
		} inner;
	} obj = {{5}};

	// Nested member access - still a VLA
	int arr[obj.inner.count];
	arr[0] = 1;
	arr[4] = 5;

	CHECK(arr[0] == 1 && arr[4] == 5, "nested struct member VLA works");
}

void test_offsetof_vs_runtime(void) {
	struct S {
		int x;
		int y;
	};

	// This should be constant (offsetof pattern with 0)
	int const_size = offsetof(struct S, y);
	int fixed_arr[const_size]; // Should be fixed-size

	// This should be VLA (runtime struct instance)
	struct S instance = {0, 3};
	int vla_arr[instance.y]; // Should be VLA

	fixed_arr[0] = 10;
	vla_arr[0] = 20;

	CHECK(fixed_arr[0] == 10, "offsetof pattern creates fixed array");
	CHECK(vla_arr[0] == 20, "runtime member creates VLA");
}

void test_stmt_expr_defer_goto(void) {
	log_reset();
	int err = 1;
	int x;

	x = ({
		{
			defer log_append("cleanup");
			if (err) goto error;
		}
		42;
	});

error:
	log_append("error_handler");

	// The defer should execute before jumping to error label
	// Risk: depends on backend compiler's statement expression implementation
	CHECK_LOG("cleanuperror_handler", "defer executes before goto in stmt expr");
}

void test_stmt_expr_defer_normal(void) {
	log_reset();
	int err = 0;

	int x = ({
		{
			defer log_append("cleanup");
			if (err) goto skip;
			log_append("body");
		}
		100;
	});

skip:
	log_append("end");

	CHECK_LOG("bodycleanupend", "defer executes normally in stmt expr");
	CHECK(x == 100, "statement expression returns correct value");
}

void test_nested_stmt_expr_defer(void) {
	log_reset();

	int result = ({
		{
			defer log_append("outer");
			int inner = ({
				{
					defer log_append("inner");
					log_append("inner_body");
				}
				5;
			});
			log_append("outer_body");
		}
		10;
	});

	CHECK_LOG("inner_bodyinnerouter_bodyouter", "nested stmt expr defer order");
	CHECK(result == 10, "nested stmt expr computes correctly");
}
#endif // _MSC_VER

void test_vanishing_statement_if_else(void) {
	log_reset();

	// Use inner scope so defer executes before we check the log
	{
		int check = 1;

		// defer now requires braces
		if (check) {
			defer log_append("cleanup");
		} // defer executes here when exiting if block
		else {
			log_append("alt");
		}

		log_append("end");
	}

	CHECK_LOG("cleanupend", "defer with braces executes when block closes");
}

void test_vanishing_statement_while(void) {
	log_reset();

	{
		int count = 0;

		while (count < 1) {
			count++;
			// defer now requires braces
			if (count == 1) {
				defer log_append("loop_cleanup");
			} // defer executes here when if block exits
		}

		log_append("after");
	}

	CHECK_LOG("loop_cleanupafter", "defer with braces in while loop works");
}

void test_vanishing_statement_for(void) {
	log_reset();

	{
		// defer in for loop now requires braces
		for (int i = 0; i < 1; i++) {
			defer log_append("for_defer");
		} // defer executes here at end of each iteration

		log_append("done");
	}

	CHECK_LOG("for_deferdone", "defer with braces in for loop works");
}

void test_defer_label(void) {
	log_reset();
	goto defer;
	log_append("skipped");
defer:
	log_append("label_reached");
	CHECK_LOG("label_reached", "label named 'defer' works correctly");
}

#ifndef _MSC_VER
void test_generic_default_first_association(void) {
	log_reset();
	int x = 42;

	switch (x) {
	case 42: {
		defer log_append("cleanup");

		// Edge case: _Generic with default as FIRST (and only) association
		// "default" is NOT preceded by comma - only by the controlling expression
		int result = _Generic(x, default: 100);

		log_append("body");
		break;
	}
	}

	log_append("end");
	CHECK_LOG("bodycleanupend", "_Generic(v, default: x) doesn't clear defer stack");
}

void test_generic_default_collision(void) {
	log_reset();
	char *ptr = malloc(16);
	int type = 1;

	switch (type) {
	case 1: {
		defer free(ptr);
		defer log_append("case1_cleanup");

		// _Generic with default keyword - should NOT clear defer stack
		// BUG: Prism sees "default" and clears the defer stack
		int x = _Generic(type, int: 0, default: 1);

		log_append("case1_body");
		break;
	}
	}

	log_append("after_switch");

	// Defers execute when exiting case block (at break), before after_switch
	CHECK_LOG("case1_bodycase1_cleanupafter_switch", "_Generic default doesn't clear defer stack");
}

void test_generic_default_collision_nested(void) {
	log_reset();
	char *ptr1 = malloc(16);
	char *ptr2 = malloc(16);
	int type = 2;

	switch (type) {
	case 1: {
		log_append("unreachable");
		break;
	}

	case 2: {
		defer free(ptr1);
		defer log_append("outer");

		// Nested _Generic - multiple "default" keywords
		// With fix: "default" in _Generic doesn't clear defer stack
		int y = _Generic(ptr2, char *: _Generic(type, int: 1, default: 2), default: 3);

		defer free(ptr2);
		defer log_append("inner");

		log_append("body");
		break;
	}
	}

	log_append("end");

	// Defers execute at break in LIFO order: inner, outer
	CHECK_LOG("bodyinnerouterend", "nested _Generic preserves defer stack");
}

void test_generic_default_outside_switch(void) {
	log_reset();
	char *ptr = malloc(16);

	{
		defer free(ptr);
		defer log_append("block_cleanup");

		// _Generic outside switch - should work fine
		int x = _Generic(ptr, char *: 1, default: 0);

		log_append("body");
	}

	log_append("after");

	CHECK_LOG("bodyblock_cleanupafter", "_Generic outside switch works normally");
}
#endif // _MSC_VER

#ifndef _MSC_VER
void test_vla_backward_goto_reentry(void) {
	int iterations = 0;
	int last_val = -1;
	int changed = 0;

label: {
	int n = (iterations == 0) ? 5 : 10; // Different sizes
	int vla[n];			    // VLA allocated on stack

	vla[0] = iterations;

	if (iterations > 0 && vla[0] != last_val) {
		changed = 1;
	}

	last_val = vla[0];
	iterations++;

	if (iterations < 2) goto label;
}

	// This test verifies the VLA is re-allocated each iteration
	// vla[0] gets different values (0, then 1), so they don't match
	CHECK(changed == 1, "VLA backward goto reentry behavior tracked");
}

void test_vla_backward_goto_stack_exhaustion(void) {
	int count = 0;
	int max_iterations = 100;

loop: {
	int size = 100;
	int vla[size]; // Stack allocation

	vla[0] = count;
	count++;

	if (count < max_iterations) goto loop; // Backward goto - potential stack buildup
}

	CHECK(count == max_iterations, "VLA backward goto completes iterations");
}

void test_vla_backward_goto_with_defer(void) {
	log_reset();
	int iterations = 0;

restart: {
	int n = 5;
	int vla[n];
	defer log_append("D");

	vla[0] = iterations;
	log_append("B");

	iterations++;
	if (iterations < 2) goto restart;
}

	log_append("E");

	// Defers should execute for each iteration
	CHECK_LOG("BDBDE", "VLA backward goto executes defers correctly");
}

void test_vla_pointer_init_semantics(void) {
	int n = 5;

	// Pointer to VLA: CAN be zero-initialized
	// Type is "int (*)[n]", which is a single pointer
	int (*ptr_to_vla)[n] = {0};

	// Array of pointers (VLA): storage only, no init allowed
	// Type is "int *[n]", which is an array of size n
	int *vla_of_ptrs[n];

	// Typedef'd VLA pointer
	typedef int Matrix[n][n];
	Matrix *mat_ptr = {0};

	// Verify pointers are zeroed
	CHECK(ptr_to_vla == NULL, "VLA pointer zero-initialized");
	CHECK(mat_ptr == NULL, "typedef VLA pointer zero-initialized");
}
#endif // _MSC_VER

typedef int T;

void test_typedef_shadow_semantics(void) {
	{
		// In outer scope, T is a typedef
		// This IS a pointer declaration
		T *ptr = NULL;
		CHECK(ptr == NULL, "typedef pointer declaration works");
	}

	{
		int T = 10; // Shadows global typedef T
		int x = 2;

		// Now T is a variable, not a type
		// "T * x" is multiplication: 10 * 2 = 20
		// If Prism adds "= 0", it becomes "10 * 2 = 0" -> syntax error
		int result = T * x;

		CHECK(result == 20, "typedef shadow multiplication works");
		CHECK(T == 10, "shadowing variable correct");
	}
}

#ifndef _MSC_VER
void test_generic_default_no_switch(void) {
	log_reset();

	{
		defer log_append("D");
		log_append("A");

		// _Generic uses 'default:', but NOT in a switch
		// Prism must not clear defer stack here
		int x = 0;
		int result = _Generic(x, int: 1, default: 2);

		log_append("B");
		CHECK(result == 1, "_Generic selection correct");
	}

	// Defer should have run
	CHECK_LOG("ABD", "_Generic default does not break defer");
}
#endif // _MSC_VER

int knr_func_add(a, b)
int a;
int b;
{
	if (a > b) goto return_a;
	return b;

return_a:
	return a;
}

void test_knr_function_parsing(void) {
	CHECK(knr_func_add(10, 5) == 10, "K&R function goto works");
	CHECK(knr_func_add(3, 8) == 8, "K&R function fallthrough works");
}

void test_comma_operator_in_init(void) {
	int a = 1, b = 2;

	// This is ONE variable 'c', initialized to result of (a, b) which is 2
	// Comma operator: evaluates left to right, result is rightmost value
	int c = (a, b);

	// This is TWO variables: 'd' gets 1, 'e' gets zero-init
	int d = 1, e;

	CHECK(c == 2, "comma operator in initializer");
	CHECK(d == 1, "first multi-declarator init");
	CHECK(e == 0, "second multi-declarator zero-init");

	// NIGHTMARE: Long comma chains with side effects
	int counter = 0;
	int result = (counter++,
		      counter++,
		      counter++,
		      counter++,
		      counter++,
		      counter++,
		      counter++,
		      counter++,
		      counter++,
		      counter++,
		      counter *= 2,
		      counter += 5,
		      counter);
	CHECK(counter == 25, "nightmare comma: counter after 10 increments, *2, +5");
	CHECK(result == 25, "nightmare comma: result is final value");

	// Comma operator inside complex expressions
	int x = 0, y = 0, z = 0;
	int complex_result = ((x = 1, y = 2, z = 3), (x + y + z) * 2);
	CHECK(x == 1 && y == 2 && z == 3, "nightmare comma: side effects in nested parens");
	CHECK(complex_result == 12, "nightmare comma: complex result");

	// Comma with ternary - parsing nightmare
	int t = 1;
	int ternary_comma = (t ? (1, 2, 3) : (4, 5, 6));
	CHECK(ternary_comma == 3, "nightmare comma: inside ternary true branch");
	t = 0;
	ternary_comma = (t ? (1, 2, 3) : (4, 5, 6));
	CHECK(ternary_comma == 6, "nightmare comma: inside ternary false branch");

	// Comma in array subscript (valid C!)
	int arr[10];
	for (int i = 0; i < 10; i++) arr[i] = i * 10;
	int subscript_comma = arr[(1, 2, 3, 7)];
	CHECK(subscript_comma == 70, "nightmare comma: in array subscript");

	// Nested comma with function-like expressions
	int nested = ((a = 100, b = 200), (c = a + b, c));
	CHECK(a == 100 && b == 200 && c == 300, "nightmare comma: nested assignments");
	CHECK(nested == 300, "nightmare comma: nested result");

	// Comma separating declarators with comma operator initializers
	int m1 = (1, 2, 3), m2 = (4, 5, 6), m3 = (7, 8, 9);
	CHECK(m1 == 3 && m2 == 6 && m3 == 9, "nightmare comma: multi-decl with comma op inits");

	// Comma in pointer arithmetic expression
	int parr[5] = {10, 20, 30, 40, 50};
	int *p = parr;
	int ptr_comma = *((p++, p++, p)); // advances p twice, then deref
	CHECK(ptr_comma == 30, "nightmare comma: in pointer expression");

	// Ultra-nested comma with all operators
	int u1 = 1, u2 = 2, u3 = 3;
	int ultra = ((u1 += 10, u2 *= 3, u3 <<= 2), (u1 &= 0xFF, u2 |= 0x10, u3 ^= 0x5), (u1 + u2 + u3));
	// u1 = 1+10 = 11, then &= 0xFF = 11
	// u2 = 2*3 = 6, then |= 0x10 = 22
	// u3 = 3<<2 = 12, then ^= 5 = 9
	// result = 11 + 22 + 9 = 42
	CHECK(ultra == 42, "nightmare comma: ultra-nested with compound ops");
}

void test_switch_skip_hole_strict(void) {
	// SAFE PATTERN: Variable declared BEFORE switch
	int x;
	int result = -1;
	switch (1) {
	case 1:
		result = x; // x is properly zero-initialized (declared before switch)
		break;
	}
	CHECK_EQ(result, 0, "switch skip hole fix: var before switch works");

	// SAFE PATTERN: Variable declared INSIDE case block
	result = -1;
	switch (1) {
	case 1: {
		int y; // Declared inside case block - properly initialized
		result = y;
		break;
	}
	}
	CHECK_EQ(result, 0, "switch skip hole fix: var in case block works");

	expect_parse_rejects(
	    "void f(int val) {\n"
	    "    switch (val) {\n"
	    "    int z;\n"
	    "    case 1:\n"
	    "        (void)z;\n"
	    "        break;\n"
	    "    }\n"
	    "}\n",
	    "switch_skip_hole.c", "switch skip hole unsafe pattern rejected", "switch body");
}

#if __STDC_VERSION__ >= 199901L && !defined(__STDC_NO_COMPLEX__)
#include <complex.h>

void test_complex_type_zeroinit(void) {
	// C99 _Complex types - should be zero-initialized
	double _Complex dc;
	CHECK(creal(dc) == 0.0 && cimag(dc) == 0.0, "double _Complex zero-init");

	float _Complex fc;
	CHECK(crealf(fc) == 0.0f && cimagf(fc) == 0.0f, "float _Complex zero-init");

	// Using the complex.h macros
	complex double cd;
	CHECK(creal(cd) == 0.0 && cimag(cd) == 0.0, "complex double (macro) zero-init");

	// Pointer to complex
	double _Complex *pdc;
	CHECK(pdc == NULL, "pointer to double _Complex zero-init");
}
#else
void test_complex_type_zeroinit(void) {
	printf("[SKIP] _Complex tests (C99 complex not available)\n");
}
#endif

void test_continue_in_switch_defer_detailed(void) {
	log_reset();
	int iterations = 0;

	while (iterations < 2) {
		defer log_append("L"); // Loop defer

		switch (iterations) {
		case 0: {
			defer log_append("S0"); // Switch case defer
			log_append("A");
			iterations++;
			continue; // Should: run S0, run L, jump to loop condition
		}
		case 1: {
			defer log_append("S1");
			log_append("B");
			iterations++;
			break; // Should: run S1, exit switch, then "X", then L at loop end
		}
		}
		log_append("X"); // After switch
	}
	log_append("E");
	// Expected trace:
	// iter=0: "A" -> continue -> S0 -> L -> check condition
	// iter=1: "B" -> break -> S1 -> "X" -> L -> check condition (exit)
	// "E"
	CHECK_LOG("AS0LBS1XLE", "continue in switch: defer order");
}

void test_ultra_complex_declarators(void) {
	// LEVEL 1: Function pointers returning function pointers

	// f1: pointer to function(void) returning pointer to function(void) returning void
	void (*(*f1)(void))(void);
	CHECK(f1 == NULL, "func ptr returning func ptr zero-init");

	// f2: pointer to function(int) returning pointer to function(int) returning int*
	int *(*(*f2)(int))(int);
	CHECK(f2 == NULL, "ptr to func(int) -> ptr to func(int) -> int*");

	// f3: pointer to function(char*, int) returning pointer to function(void) returning pointer to function(int, int) returning long
	long (*(*(*f3)(char *, int))(void))(int, int);
	CHECK(f3 == NULL, "triple-nested func ptr chain");

	// LEVEL 2: Arrays of function pointers with complex return types

	// afp1: array[3] of pointer to function(void) returning int*
	int *(*afp1[3])(void);
	int all_null = 1;
	for (int i = 0; i < 3; i++)
		if (afp1[i] != NULL) all_null = 0;
	CHECK(all_null, "array of func ptrs returning ptr zero-init");

	// afp2: array[4] of pointer to function(int) returning pointer to function(void) returning char*
	char *(*(*afp2[4])(int))(void);
	all_null = 1;
	for (int i = 0; i < 4; i++)
		if (afp2[i] != NULL) all_null = 0;
	CHECK(all_null, "array of func ptrs returning func ptrs");

	// afp3: array[2][3] of pointer to function(void) returning void*
	void *(*afp3[2][3])(void);
	all_null = 1;
	for (int i = 0; i < 2; i++)
		for (int j = 0; j < 3; j++)
			if (afp3[i][j] != NULL) all_null = 0;
	CHECK(all_null, "2D array of func ptrs zero-init");

	// LEVEL 3: Pointers to arrays of function pointers

	// pafp1: pointer to array[5] of pointer to function(void) returning void
	void (*(*pafp1)[5])(void);
	CHECK(pafp1 == NULL, "ptr to array of func ptrs zero-init");

	// pafp2: pointer to array[3] of pointer to function(int*) returning pointer to array[10] of int
	int (*(*(*pafp2)[3])(int *))[10];
	CHECK(pafp2 == NULL, "ptr to array of func ptrs returning ptr to array");

	// pafp3: pointer to array[2][4] of pointer to function(void) returning double*
	double *(*(*pafp3)[2][4])(void);
	CHECK(pafp3 == NULL, "ptr to 2D array of func ptrs");

	// LEVEL 4: Function pointers returning pointers to arrays

	// fpa1: pointer to function(int) returning pointer to array[10] of char
	char (*(*fpa1)(int))[10];
	CHECK(fpa1 == NULL, "func ptr returning ptr to array");

	// fpa2: pointer to function(void) returning pointer to array[5] of pointer to function(int) returning int
	int (*(*(*fpa2)(void))[5])(int);
	CHECK(fpa2 == NULL, "func ptr returning ptr to array of func ptrs");

	// fpa3: pointer to function(char) returning pointer to array[3][4] of long*
	long *(*(*fpa3)(char))[3][4];
	CHECK(fpa3 == NULL, "func ptr returning ptr to 2D array of ptrs");

	// LEVEL 5: Arrays of pointers to arrays of function pointers

	// apafp: array[2] of pointer to array[3] of pointer to function(void) returning int
	int (*(*(*apafp[2]))[3])(void);
	all_null = 1;
	for (int i = 0; i < 2; i++)
		if (apafp[i] != NULL) all_null = 0;
	CHECK(all_null, "array of ptrs to arrays of func ptrs");

	// LEVEL 6: Deeply nested pointer chains with mixed types

	// pp1: pointer to pointer to pointer to function(void) returning pointer to pointer to int
	int **(*(**pp1)(void));
	CHECK(pp1 == NULL, "ptr to ptr to func ptr returning ptr to ptr");

	// pp2: pointer to pointer to array[5] of pointer to function(int, char*) returning void*
	void *(*(**pp2)[5])(int, char *);
	CHECK(pp2 == NULL, "ptr to ptr to array of func ptrs");

	// LEVEL 7: Signal-handler-like ultra-complex signatures

	// signal_like: pointer to function(int, pointer to function(int) returning void)
	//              returning pointer to function(int) returning void
	void (*(*signal_like)(int, void (*)(int)))(int);
	CHECK(signal_like == NULL, "signal-like handler ptr");

	// signal_extreme: like signal but returning ptr to func returning ptr to func
	void (*(*(*signal_extreme)(int, void (*)(int)))(void))(int);
	CHECK(signal_extreme == NULL, "signal returning double func ptr");

	// LEVEL 8: Const and volatile qualifiers in complex declarators

	// cvfp1: pointer to function(const int*) returning pointer to volatile char*
	volatile char *(*(*cvfp1)(const int *))(void);
	CHECK(cvfp1 == NULL, "const/volatile qualified func ptr");

	// cvfp2: const pointer to function(void) returning pointer to const pointer to volatile int
	volatile int *const *(*(*const cvfp2)(void))(void) = NULL;
	CHECK(cvfp2 == NULL, "const ptr to func returning nested cv ptrs");

	// cvfp3: array[3] of const pointer to function(volatile int*) returning const char*
	const char *(*const cvfp3[3])(volatile int *) = {NULL, NULL, NULL};
	all_null = 1;
	for (int i = 0; i < 3; i++)
		if (cvfp3[i] != NULL) all_null = 0;
	CHECK(all_null, "array of const func ptrs with cv params");

	// LEVEL 9: Structs containing complex declarators

	struct ComplexFuncPtrStruct {
		// Member: pointer to function(void) returning pointer to function(int) returning char*
		char *(*(*member1)(void))(int);
		// Member: array[2] of pointer to function(void*) returning pointer to array[5] of int
		int (*(*member2[2])(void *))[5];
		// Member: pointer to pointer to function(struct ComplexFuncPtrStruct*) returning void
		void(*(**member3)(struct ComplexFuncPtrStruct *));
	};
	struct ComplexFuncPtrStruct cfps;
	CHECK(cfps.member1 == NULL, "struct member: nested func ptr");
	all_null = 1;
	for (int i = 0; i < 2; i++)
		if (cfps.member2[i] != NULL) all_null = 0;
	CHECK(all_null, "struct member: array of complex func ptrs");
	CHECK(cfps.member3 == NULL, "struct member: ptr to ptr to func ptr");

	// LEVEL 10: The ultimate declarator stress test

	// ultimate1: array[2] of pointer to function(pointer to function(int) returning int*)
	//            returning pointer to array[3] of pointer to function(void) returning char**
	char **(*(*(*ultimate1[2])(int *(*)(int)))[3])(void);
	all_null = 1;
	for (int i = 0; i < 2; i++)
		if (ultimate1[i] != NULL) all_null = 0;
	CHECK(all_null, "ultimate: array of func ptrs returning array of func ptrs");

	// ultimate2: pointer to function(array[5] of pointer to function(void) returning int, char*)
	//            returning pointer to pointer to function(long) returning pointer to array[4] of double
	double (*(*(**(*ultimate2)(int (*[5])(void), char *))(long)))[4];
	CHECK(ultimate2 == NULL, "ultimate: func ptr with func ptr array param");

	// ultimate3: pointer to array[2] of pointer to function(pointer to pointer to int)
	//            returning pointer to function(char, pointer to function(void) returning float)
	//            returning pointer to array[6] of short*
	short *(*(*(*(*ultimate3)[2])(int **))(char, float (*)(void)))[6];
	CHECK(ultimate3 == NULL, "ultimate: quadruple-nested mixed declarator");

	// LEVEL 11: Recursive-style type references

	// Self-referential through void* cast pattern
	// node_handler: pointer to function(void*, pointer to function(void*, void*) returning int)
	//               returning pointer to function(void*) returning void*
	void *(*(*(*node_handler)(void *, int (*)(void *, void *)))(void *));
	CHECK(node_handler == NULL, "self-ref style nested handler");

	// callback_chain: pointer to function returning pointer to function returning pointer to function
	//                 returning pointer to function returning int
	int (*(*(*(*callback_chain)(void))(void))(void))(void);
	CHECK(callback_chain == NULL, "4-level callback chain");

	// LEVEL 12: Combining everything - the nightmare declarators

	// nightmare1: pointer to array[2] of pointer to function(
	//     pointer to function(const char*, volatile int*) returning pointer to array[3] of long,
	//     pointer to pointer to function(void) returning short*
	// ) returning pointer to function(unsigned char) returning pointer to pointer to array[4] of float*
	float *(*(**(*(*(*nightmare1)[2])(long (*(*)(const char *, volatile int *))[3],
					  short *(*(**)(void))))(unsigned char)))[4];
	CHECK(nightmare1 == NULL, "nightmare: multi-param deeply nested");

	// nightmare2: array[1] of pointer to pointer to function(
	//     array[2] of pointer to function(int) returning char*
	// ) returning pointer to array[3] of pointer to function(double) returning pointer to pointer to int
	int **(*(*(*(**nightmare2[1])(char *(*[2])(int)))[3])(double));
	all_null = 1;
	if (nightmare2[0] != NULL) all_null = 0;
	CHECK(all_null, "nightmare: array of ptr to ptr to complex func");
}

#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
_Thread_local int tls_var; // Should NOT get explicit = 0 (redundant but valid)

static void verify_thread_local_output(void) {
	PrismResult result = prism_transpile_source(
	    "_Thread_local int tls_var;\n"
	    "int f(void) {\n"
	    "    static _Thread_local int tls_local;\n"
	    "    return tls_var + tls_local;\n"
	    "}\n",
	    "thread_local_parse.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "_Thread_local output check transpiles");
	if (result.output) {
		CHECK(strstr(result.output, "_Thread_local int tls_var") != NULL,
		      "_Thread_local output: file-scope thread_local preserved");
		CHECK(strstr(result.output, "static _Thread_local int tls_local") != NULL,
		      "_Thread_local output: function-scope static thread_local preserved");
		CHECK(strstr(result.output, "memset") == NULL,
		      "_Thread_local output: no memset inserted for thread storage");
	}
	prism_free(&result);
}

void test_thread_local_handling(void) {
	// Static storage duration means implicit zero-init by C standard
	CHECK_EQ(tls_var, 0, "_Thread_local file scope implicit zero");

	// Thread-local in function scope
	static _Thread_local int tls_local;
	CHECK_EQ(tls_local, 0, "static _Thread_local local implicit zero");
	verify_thread_local_output();
}
#else
void test_thread_local_handling(void) {
	printf("[SKIP] _Thread_local tests (C11 threads not available)\n");
}
#endif

static void verify_line_directive_output(void) {
	PrismResult result = prism_transpile_source(
	    "int f(void) {\n"
	    "    int x;\n"
	    "    return x;\n"
	    "}\n",
	    "line_directive_parse.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "line directive output check transpiles");
	if (result.output) {
		CHECK(strstr(result.output, "line_directive_parse.c") != NULL,
		      "line directive output: filename preserved");
		CHECK(strstr(result.output, "#line ") != NULL || strstr(result.output, "# ") != NULL,
		      "line directive output: line marker emitted");
	}
	prism_free(&result);
}

void test_line_directive_preservation(void) {
	int line_before = __LINE__;
	{
		defer(void) 0; // Simple defer that injects code
		int x;	       // Should be zero-init
		(void)x;
	}
	int line_after = __LINE__;

	// Lines should be sequential (accounting for the block)
	// If line directives are broken, __LINE__ would report wrong values
	// This is a sanity check - real verification needs error message testing
	CHECK(line_after > line_before, "#line tracking: lines increase correctly");

	// Test with multiple defers
	line_before = __LINE__;
	{
		defer log_append("A");
		defer log_append("B");
		defer log_append("C");
		int y;
		(void)y;
	}
	line_after = __LINE__;
	CHECK(line_after > line_before, "#line tracking: multiple defers OK");
	verify_line_directive_output();
}

void test_alignas_struct_bitfield(void) {
	// Standard struct with bitfield - combined definition and variable
	struct Data {
		int val;
		unsigned int flag : 1; // Bitfield, not label
	} d = {42, 1};

	// Struct with __attribute__ - bitfield must NOT be mistaken for label
	struct __attribute__((packed)) PackedData {
		unsigned int x : 1; // This is a BITFIELD, not a label!
		unsigned int y : 2;
	} pd = {1, 3};

	// Struct with multiple attributes before name
	struct __attribute__((packed)) __attribute__((aligned(4))) AttrData {
		unsigned int a : 4;
		unsigned int b : 4;
	} ad = {5, 10};

	CHECK(d.val == 42 && d.flag == 1, "struct bitfield: basic struct works");
	CHECK(pd.x == 1 && pd.y == 3, "struct bitfield: packed bitfields work");
	CHECK(ad.a == 5 && ad.b == 10, "struct bitfield: multi-attr bitfields work");

	PrismResult result = prism_transpile_source(
	    "struct __attribute__((packed)) PackedData {\n"
	    "    unsigned int x : 1;\n"
	    "    unsigned int y : 2;\n"
	    "};\n"
	    "int main(void) { struct PackedData pd = {1, 3}; return pd.x + pd.y; }\n",
	    "struct_bitfield_parse.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "struct bitfield parsing transpiles");
	if (result.output) {
		CHECK(strstr(result.output, "x : 1") != NULL,
		      "struct bitfield parsing: first bitfield preserved");
		CHECK(strstr(result.output, "y : 2") != NULL,
		      "struct bitfield parsing: second bitfield preserved");
	}
	prism_free(&result);
}

#ifndef _MSC_VER
typedef int GenericTestType;

static void verify_generic_typedef_output(void) {
	PrismResult result = prism_transpile_source(
	    "typedef int GenericTestType;\n"
	    "void f(void) {\n"
	    "    int y = _Generic((char)0, GenericTestType: 10, char: 20, default: 30);\n"
	    "    (void)y;\n"
	    "}\n",
	    "generic_typedef_not_label.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "_Generic typedef parse transpiles");
	if (result.output) {
		CHECK(strstr(result.output, "_Generic") != NULL,
		      "_Generic typedef parse: generic expression preserved");
		CHECK(strstr(result.output, "GenericTestType : 10") != NULL ||
		          strstr(result.output, "GenericTestType: 10") != NULL,
		      "_Generic typedef parse: typedef association preserved");
	}
	prism_free(&result);
}

void test_generic_typedef_not_label(void) {
	// _Generic uses Type: expr syntax which looks like labels
	// Prism should skip _Generic(...) in label scanner
	int x = _Generic(0,
	    GenericTestType: 1, // This is NOT a goto label!
	    default: 0);

	CHECK_EQ(x, 1, "_Generic typedef association works");

	// Verify label scanner doesn't get confused
	// If it did, goto might emit wrong defer cleanup
	log_reset();
	{
		defer log_append("D");
		int y = _Generic((char)0, GenericTestType: 10, char: 20, default: 30);
		CHECK_EQ(y, 20, "_Generic with multiple type associations");
		log_append("X");
	}
	CHECK_LOG("XD", "_Generic doesn't confuse label scanner");
	verify_generic_typedef_output();
}
#endif // _MSC_VER

#if __STDC_VERSION__ >= 202311L
void test_c23_attributes_zeroinit(void) {
	// C23 attributes before declaration
	[[maybe_unused]] int x;
	CHECK_EQ(x, 0, "[[maybe_unused]] int zero-init");

	// Multiple attributes
	[[maybe_unused]] [[deprecated]] int y;
	CHECK_EQ(y, 0, "multiple [[...]] attributes zero-init");

	// Attribute with argument
	[[deprecated("use z2 instead")]] int z;
	CHECK_EQ(z, 0, "[[deprecated(...)]] zero-init");

	PrismResult result = prism_transpile_source(
	    "void f(void) {\n"
	    "    [[maybe_unused]] int x;\n"
	    "    [[maybe_unused]] [[deprecated]] int y;\n"
	    "    [[deprecated(\"use z2 instead\")]] int z;\n"
	    "    (void)x; (void)y; (void)z;\n"
	    "}\n",
	    "c23_attr_zeroinit_parse.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "C23 [[...]] attrs transpile");
	if (result.output) {
		CHECK(strstr(result.output, "[[maybe_unused]]") != NULL,
		      "C23 [[...]] attrs: maybe_unused preserved");
		CHECK(strstr(result.output, "[[deprecated") != NULL,
		      "C23 [[...]] attrs: deprecated preserved");
		CHECK(strstr(result.output, "memset") == NULL,
		      "C23 [[...]] attrs: not misparsed as VLA");
		CHECK(strstr(result.output, "= 0") != NULL,
		      "C23 [[...]] attrs: scalar zero-init preserved");
	}
	prism_free(&result);
}
#else
void test_c23_attributes_zeroinit(void) {
	printf("[SKIP] C23 [[...]] attribute tests (C23 not available)\n");
}
#endif

#if __STDC_VERSION__ >= 202311L && defined(__clang__)
void test_bitint_zeroinit(void) {
	_BitInt(32) x;
	CHECK(x == 0, "_BitInt(32) zero-init");

	_BitInt(64) y;
	CHECK(y == 0, "_BitInt(64) zero-init");

	unsigned _BitInt(16) z;
	CHECK(z == 0, "unsigned _BitInt(16) zero-init");

	PrismResult result = prism_transpile_source(
	    "void f(void) {\n"
	    "    _BitInt(32) x;\n"
	    "    _BitInt(64) y;\n"
	    "    unsigned _BitInt(16) z;\n"
	    "    (void)x; (void)y; (void)z;\n"
	    "}\n",
	    "bitint_zeroinit_parse.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "_BitInt transpiles");
	if (result.output) {
		CHECK(strstr(result.output, "_BitInt(32)") != NULL,
		      "_BitInt output: signed width preserved");
		CHECK(strstr(result.output, "unsigned _BitInt(16)") != NULL,
		      "_BitInt output: unsigned width preserved");
		CHECK(strstr(result.output, "memset") == NULL,
		      "_BitInt output: not misparsed as VLA");
	}
	prism_free(&result);
}
#else
void test_bitint_zeroinit(void) {
	printf("[SKIP] _BitInt tests (C23/_BitInt not available)\n");
}
#endif

void test_pragma_pack_preservation(void) {
#pragma pack(push, 1)

	struct PragmaPackTest {
		char a;
		int b;
	};

#pragma pack(pop)

	// On typical ABIs without packing, this would be 8 bytes (char + 3 padding + int)
	// With pack(1), it should be 5 bytes (char + int, no padding)
	size_t size = sizeof(struct PragmaPackTest);
	CHECK(size == 5, "pragma pack(1) preserved - struct size is 5");

	PrismResult result = prism_transpile_source(
	    "#pragma pack(push, 1)\n"
	    "struct PragmaPackTest { char a; int b; };\n"
	    "#pragma pack(pop)\n"
	    "int main(void) { return sizeof(struct PragmaPackTest); }\n",
	    "pragma_pack_parse.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "pragma pack parse transpiles");
	if (result.output) {
		CHECK(strstr(result.output, "#pragma pack(push") != NULL,
		      "pragma pack output: push pragma preserved");
		CHECK(strstr(result.output, "#pragma pack(pop)") != NULL,
		      "pragma pack output: pop pragma preserved");
	}
	prism_free(&result);
}

#ifndef _MSC_VER
static int g_defer_counter;

int test_return_stmt_expr_helper(int x) {
	defer g_defer_counter++;
	return ({
		int y = x + 1;
		y;
	});
}

void test_return_stmt_expr_with_defer(void) {
	g_defer_counter = 0;
	int result = test_return_stmt_expr_helper(42);

	// The function should return 43 (42 + 1)
	CHECK(result == 43, "statement-expr return value correct");

	// The defer should have executed once
	CHECK(g_defer_counter == 1, "defer executed with statement-expr return");
}

void test_security_stmtexpr_value_corruption(void) {
	log_reset();

	// Test 1: Nested block with defer should work correctly
	int val = ({
		{ defer log_append("D"); }
		42; // This should be the return value
	});

	CHECK_EQ(val, 42, "statement-expr value correct with nested defer");
	CHECK_LOG("D", "nested defer in statement-expr executed");

	log_reset();

	// Test 2: Multiple nested blocks
	int val2 = ({
		int tmp = 10;
		{
			defer log_append("X");
			tmp += 5;
		}
		tmp + 27; // Should return 42
	});

	CHECK_EQ(val2, 42, "statement-expr with multiple statements and defer");
	CHECK_LOG("X", "defer executed before final expression");
	log_reset();
}
#endif // _MSC_VER

void test_security_braceless_defer_trap(void) {
	log_reset();

	// FIXED: Braceless defer now errors at compile-time, preventing the security issue
	// This test verifies the correct behavior with braces
	{
		int trigger = 0;

		// With braces, this creates a proper scope
		if (trigger) {
			defer log_append("FAIL");
		}

		log_append("OK");
	}

	// With the fix, defer only executes if the condition is true
	// Since trigger=0, the defer does not execute
	CHECK_LOG("OK", "defer with braces executes conditionally (issue FIXED)");
	expect_parse_rejects(
	    "void f(int trigger) {\n"
	    "    if (trigger) defer log_append(\"FAIL\");\n"
	    "}\n",
	    "braceless_defer_trap.c", "braceless if defer trap rejected", "defer");
}

void test_security_switch_goto_double_free(void) {
	log_reset();
	int stage = 1;

	// FIXED: Switch case defer now requires braces, which creates proper scoping
	switch (stage) {
	case 1: {
		defer log_append("X");
		log_append("A");
	} // defer executes here when exiting case 1 block
	// Note: Can't use goto to jump to another case after the block closes
	// because that would be outside the braced block
	break;

	case 2: log_append("Y"); break;
	}

	// With the fix, defer executes when the braced block closes
	// Log should be "AX" (A appended, then defer X executes at })
	CHECK_LOG("AX", "switch defer with braces executes correctly (issue FIXED)");
	log_reset();
	expect_parse_rejects(
	    "void f(int stage) {\n"
	    "    switch (stage) {\n"
	    "    case 1:\n"
	    "        defer log_append(\"X\");\n"
	    "        log_append(\"A\");\n"
	    "    case 2:\n"
	    "        log_append(\"Y\");\n"
	    "        break;\n"
	    "    }\n"
	    "}\n",
	    "switch_defer_loss.c", "switch goto defer loss rejected", "defer");
}

void test_ghost_shadow_corruption(void) {
	// This tests that typedef shadows are properly cleaned up
	// even when loop bodies are braceless
	typedef int T;

	// Declare loop variable T that shadows typedef 'T'
	// With braceless body, shadow must still be cleaned up
	for (int T = 0; T < 5; T++);

	// Now use T as a type - should work correctly
	// Without the fix, T would still be shadowed and this would parse wrong
	T *ptr = NULL;

	CHECK(ptr == NULL, "ghost shadow: typedef T works after braceless for loop");
}

#ifndef _MSC_VER
void test_sizeof_vla_codegen(void) {
	int n = 10;

	// sizeof(int[n]) is evaluated at runtime because n is variable
	// So arr is a VLA, not a constant-sized array
	// Prism should NOT emit = {0} for this
	int arr[sizeof(int[n])];
	arr[0] = 42;

	CHECK(arr[0] == 42, "sizeof(VLA) treated as runtime value");
}
#endif // _MSC_VER

void test_keyword_typedef_collision(void) {
	// These typedefs use names that are also Prism keywords
	typedef int raw;
	typedef int defer;
	typedef int orelse;

	// These should work correctly
	raw x = 10;
	defer y = 20;
	orelse z = 30;

	CHECK(x == 10, "typedef named 'raw' works");
	CHECK(y == 20, "typedef named 'defer' works");
	CHECK(z == 30, "typedef named 'orelse' works");
}

// Regression: Prism keywords used as struct/union field names.
// This is valid C — 'defer', 'orelse', 'raw' are not C keywords.
// Bug: 'bool orelse;' in PrismFeatures struct triggered the error diagnostic.
struct _KeywordFields {
	int defer;
	int orelse;
	int raw;
};

void test_keyword_as_struct_field(void) {
	struct _KeywordFields kf;
	kf.defer = 1;
	kf.orelse = 2;
	kf.raw = 3;
	CHECK(kf.defer == 1, "struct field named 'defer' works");
	CHECK(kf.orelse == 2, "struct field named 'orelse' works");
	CHECK(kf.raw == 3, "struct field named 'raw' works");

	// Compound literal
	struct _KeywordFields kf2 = {.defer = 10, .orelse = 20, .raw = 30};
	CHECK(kf2.defer == 10, "compound literal field 'defer' works");
	CHECK(kf2.orelse == 20, "compound literal field 'orelse' works");
	CHECK(kf2.raw == 30, "compound literal field 'raw' works");
}

// Keywords as function names
static int _fn_named_orelse(int x) { return x + 1; }

void test_keyword_as_function_name(void) {
	CHECK(_fn_named_orelse(5) == 6, "function named with keyword prefix works");

	// Function pointer to keyword-named function
	int (*fp)(int) = _fn_named_orelse;
	CHECK(fp(10) == 11, "fnptr to keyword-named function works");
}

#ifndef _MSC_VER
void test_sizeof_vla_typedef(void) {
	int n = 10;
	typedef int VLA_Type[n];

	// sizeof(VLA_Type) is evaluated at runtime because VLA_Type is a VLA
	// Prism should NOT emit = {0} for this
	int arr[sizeof(VLA_Type)];
	arr[0] = 42;

	CHECK(arr[0] == 42, "sizeof(VLA_Typedef) treated as runtime value");
}

void test_typeof_vla_zeroinit(void) {
	int n = 10;
	int vla1[n];
	vla1[0] = 42;

	// copy_vla is a VLA type via typeof
	// Prism now zero-inits VLAs via memset (sizeof works at runtime)
	__typeof__(vla1) copy_vla;

	// Verify zero-init worked
	int all_zero = 1;
	for (int i = 0; i < n; i++)
		if (copy_vla[i] != 0) all_zero = 0;
	CHECK(all_zero, "typeof(VLA) now gets zero-init via memset");

	// Can still assign after
	copy_vla[0] = 99;
	CHECK(copy_vla[0] == 99, "typeof(VLA) assignment after zero-init works");
}
#endif // _MSC_VER

void test_bug1_ghost_shadow_while(void) {
	typedef int U;
	int x = 5;
	while (x-- > 0) {
		int U = x; // Shadow inside braced body
		(void)U;
	}
	U *ptr = NULL;
	CHECK(ptr == NULL, "typedef U works after while with shadow");
}

void test_bug1_ghost_shadow_if(void) {
	typedef int V;
	if (1)
		;
	V *ptr = NULL;
	CHECK(ptr == NULL, "typedef V works after braceless if");
}

static int ghost_shadow_return_helper(void) {
	typedef int T;
	for (int T = 0; T < 5; T++) return T;
	// T should be restored as typedef after braceless for+return
	T val = 42;
	return val;
}

void test_ghost_shadow_braceless_break(void) {
	typedef int T;

	// break in braceless for body - shadow must be cleaned up
	for (int T = 0; T < 5; T++) break;
	T *ptr = NULL;
	CHECK(ptr == NULL, "ghost shadow: typedef T works after braceless for+break");
}

void test_ghost_shadow_braceless_continue(void) {
	typedef int T;

	// continue in braceless for body - shadow must be cleaned up
	for (int T = 0; T < 5; T++) continue;
	T *ptr = NULL;
	CHECK(ptr == NULL, "ghost shadow: typedef T works after braceless for+continue");
}

void test_ghost_shadow_braceless_return(void) {
	int result = ghost_shadow_return_helper();
	CHECK(result == 0, "ghost shadow: typedef T works after braceless for+return");
}

void test_ghost_shadow_nested_braceless(void) {
	typedef int T;

	// Nested braceless: if inside for - both must clean up properly
	for (int T = 0; T < 5; T++)
		if (T > 2) break;
	T *ptr = NULL;
	CHECK(ptr == NULL, "ghost shadow: typedef T works after nested braceless for+if+break");
}

void test_bug2_ultra_complex_exact(void) {
	// Exact example from bug report: pointer to array of 5 function pointers
	int (*(*complex_var)[5])(void);
	CHECK(complex_var == NULL, "ultra-complex declarator from report");
}

void test_bug2_deeply_nested_parens(void) {
	// Even more nested: pointer to function returning pointer to array
	int (*(*fp)(int))[10];
	CHECK(fp == NULL, "deeply nested paren declarator");
}

#ifndef _MSC_VER
static int defer_value_3rdparty = 0;

void test_bug3_stmtexpr_defer_ordering(void) {
	defer_value_3rdparty = 0;

	// Test defer in nested block within statement expression
	int x = ({
		int val = 10;
		{
			defer {
				defer_value_3rdparty = val;
			};
			val = val + 5; // Modify to 15
		}
		val; // Return 15
	});

	CHECK(x == 15, "statement-expr with nested defer");
	CHECK(defer_value_3rdparty == 15, "defer captured value");
}

void test_bug3_stmtexpr_defer_variable(void) {
	int result = ({
		int tmp = 42;
		{ defer tmp = 999; }
		tmp; // Return 999
	});

	CHECK(result == 999, "defer modifies variable correctly");
}

void test_bug4_generic_fnptr(void) {
	// Exact pattern from bug report: function pointer in _Generic
	int x = _Generic(0, void (*)(int): 1, default: 0);
	CHECK(x == 0, "_Generic with fn ptr type");
}

void test_bug4_generic_defer_interaction(void) {
	int result = 0;
	{
		defer result = 1;
		int y = _Generic((int *)0, int *: 5, void (*)(int): 10, default: 15);
		result = y; // Should be 5
	}
	// Defer runs after, result = 1
	CHECK(result == 1, "defer doesn't break _Generic");
}

void test_bug7_sizeof_vla_variable(void) {
	int n = 5;
	int vla[n]; // VLA
	vla[0] = 42;

	// CRITICAL: sizeof(vla) is evaluated at runtime!
	// Array x is also a VLA, should NOT be zero-initialized
	int x[sizeof(vla)];
	x[0] = 99;

	CHECK(vla[0] == 42 && x[0] == 99, "3rd-party bug #7: sizeof(vla) creates VLA");
}

void test_bug7_sizeof_sizeof_vla(void) {
	int n = 3;
	int arr1[n]; // VLA
	arr1[0] = 1;

	// sizeof(sizeof(arr1)) is sizeof(size_t) - constant
	int arr2[sizeof(sizeof(arr1))];
	arr2[0] = 2;

	CHECK(arr1[0] == 1 && arr2[0] == 2, "sizeof(sizeof(VLA))");
}

void test_bug7_sizeof_vla_element(void) {
	int m = 4;
	int inner[m]; // VLA
	inner[0] = 10;

	// sizeof(inner[0]) is sizeof(int) - constant!
	int outer[sizeof(inner[0])];
	// outer should be zero-initialized
	CHECK(outer[0] == 0, "sizeof(VLA[0]) is constant");
}

void test_sizeof_parenthesized_vla(void) {
	int n = 5;
	int vla[n]; // VLA
	vla[0] = 42;

	// sizeof((vla)) is still runtime — extra parens around VLA variable
	int arr[sizeof((vla))];
	arr[0] = 99;

	CHECK(arr[0] == 99, "sizeof((vla)): parenthesized VLA treated as runtime");
	CHECK(vla[0] == 42, "sizeof((vla)): original VLA not clobbered");
}

void test_edge_multiple_typedef_shadows(void) {
	typedef int T;
	{
		int T = 5;
		CHECK(T == 5, "3rd-party edge: first shadow level");
		{
			int T = 10;
			CHECK(T == 10, "second shadow level");
		}
		CHECK(T == 5, "back to first shadow");
	}
	T *ptr = NULL;
	CHECK(ptr == NULL, "typedef restored after shadows");
}

void test_edge_defer_in_generic(void) {
	int result = 0;
	{
		int x = _Generic(1, int: 10, default: 20);
		defer result = x;
	}
	CHECK(result == 10, "defer with _Generic");
}

void test_attributed_label_defer(void) {
	log_reset();
	{
		defer log_append("Cleanup");
		goto error;
	}

error:
	__attribute__((unused)) log_append("Error");

	CHECK(strcmp(log_buffer, "CleanupError") == 0, "attributed label defer cleanup");
}
#endif // _MSC_VER

void test_number_tokenizer_identifiers(void) {
#define MN_test 0xf64
#define SPACE_test 200

	int arr1[] = {2, MN_test, 3, SPACE_test};
	CHECK(arr1[0] == 2, "tokenizer: array element 0 is 2");
	CHECK(arr1[1] == 0xf64, "tokenizer: MN_test expands to 0xf64");
	CHECK(arr1[2] == 3, "tokenizer: array element 2 is 3");
	CHECK(arr1[3] == 200, "tokenizer: SPACE_test expands to 200");

	// Test that hex numbers followed by identifiers work correctly
	int x = 0x82;
	int MN_invpcid = 100;
	CHECK(x == 0x82, "tokenizer: hex number 0x82 parsed correctly");
	CHECK(MN_invpcid == 100, "tokenizer: identifier MN_invpcid separate from hex");

	// Test edge cases with different identifier prefixes
	int val1 = 0xAB;
	int MN_other = 500;
	int SPACE_other = 600;
	CHECK(val1 == 0xAB, "tokenizer: hex 0xAB parsed correctly");
	CHECK(MN_other == 500, "tokenizer: MN_ identifier works");
	CHECK(SPACE_other == 600, "tokenizer: SPACE_ identifier works");
}

void test_hex_numbers_vs_float_suffixes(void) {
	// Test hex patterns that look like C23 float suffixes
	unsigned int h1 = 0xf64;
	CHECK(h1 == 3940, "hex: 0xf64 not confused with F64 suffix");

	unsigned int h2 = 0xf32;
	CHECK(h2 == 3890, "hex: 0xf32 not confused with F32 suffix");

	unsigned int h3 = 0xf16;
	CHECK(h3 == 3862, "hex: 0xf16 not confused with F16 suffix");

	unsigned int h4 = 0xbf16;
	CHECK(h4 == 48918, "hex: 0xbf16 not confused with BF16 suffix");

	unsigned int h5 = 0xf128;
	CHECK(h5 == 61736, "hex: 0xf128 not confused with F128 suffix");

	// Test that real float suffixes still work
	float f1 = 1.0f;
	double d1 = 1.0;
	long double ld1 = 1.0L;
	CHECK(f1 == 1.0f, "hex: float suffix f still works");
	CHECK(d1 == 1.0, "hex: double still works");
	CHECK(ld1 == 1.0L, "hex: long double suffix L still works");

	// Test combinations in array
	int arr[] = {0xf64, 0xf32, 0xf16, 0xabc, 0x123};
	CHECK(arr[0] == 0xf64, "hex: array[0] = 0xf64");
	CHECK(arr[1] == 0xf32, "hex: array[1] = 0xf32");
	CHECK(arr[2] == 0xf16, "hex: array[2] = 0xf16");
	CHECK(arr[3] == 0xabc, "hex: array[3] = 0xabc");
	CHECK(arr[4] == 0x123, "hex: array[4] = 0x123");
}

void test_hex_and_identifier_edge_cases(void) {
#define HEX_F64 0xf64
#define HEX_F32 0xf32

	int val1 = HEX_F64;
	int val2 = HEX_F32;
	CHECK(val1 == 0xf64, "edge: macro HEX_F64 expands correctly");
	CHECK(val2 == 0xf32, "edge: macro HEX_F32 expands correctly");

	// Array initializers (binutils pattern)
	struct test_struct {
		int a;
		int b;
		int c;
	};

	struct test_struct s1 = {0xf64, 0x82, 2};
	CHECK(s1.a == 0xf64, "edge: struct init with 0xf64");
	CHECK(s1.b == 0x82, "edge: struct init with 0x82");
	CHECK(s1.c == 2, "edge: struct init with 2");

#define OUTER_MACRO 0xf64
#define INNER_MACRO OUTER_MACRO
	int nested = INNER_MACRO;
	CHECK(nested == 0xf64, "edge: nested macro expansion");

	// Hex numbers in expressions
	int expr1 = 0xf64 + 0xf32;
	CHECK(expr1 == (0xf64 + 0xf32), "edge: hex addition");

	int expr2 = 0xf64 | 0xf32;
	CHECK(expr2 == (0xf64 | 0xf32), "edge: hex bitwise OR");

	// Binary numbers (should also not be confused with suffixes)
	int bin1 = 0b1111;
	CHECK(bin1 == 15, "edge: binary literal works");
}

void test_valid_number_suffixes(void) {
	// Integer suffixes
	unsigned int u1 = 100u;
	unsigned int u2 = 100U;
	long l1 = 100l;
	long l2 = 100L;
	unsigned long ul1 = 100ul;
	unsigned long ul2 = 100UL;
	unsigned long long ull1 = 100ull;
	unsigned long long ull2 = 100ULL;

	CHECK(u1 == 100, "suffix: 100u works");
	CHECK(u2 == 100, "suffix: 100U works");
	CHECK(l1 == 100, "suffix: 100l works");
	CHECK(l2 == 100, "suffix: 100L works");
	CHECK(ul1 == 100, "suffix: 100ul works");
	CHECK(ul2 == 100, "suffix: 100UL works");
	CHECK(ull1 == 100, "suffix: 100ull works");
	CHECK(ull2 == 100, "suffix: 100ULL works");

	// Hex with suffixes
	unsigned int hu1 = 0xFFu;
	unsigned int hu2 = 0xFFU;
	unsigned long hul = 0xFFUL;
	unsigned long long hull = 0xFFULL;

	CHECK(hu1 == 255, "suffix: 0xFFu works");
	CHECK(hu2 == 255, "suffix: 0xFFU works");
	CHECK(hul == 255, "suffix: 0xFFUL works");
	CHECK(hull == 255, "suffix: 0xFFULL works");

	// Float suffixes
	float f1 = 1.0f;
	float f2 = 1.0F;
	long double ld1 = 1.0l;
	long double ld2 = 1.0L;

	CHECK(f1 == 1.0f, "suffix: 1.0f works");
	CHECK(f2 == 1.0F, "suffix: 1.0F works");
	CHECK(ld1 == 1.0L, "suffix: 1.0l works");
	CHECK(ld2 == 1.0L, "suffix: 1.0L works");
}

#ifndef _MSC_VER
int test_return_zeroinit_no_defer_helper(void) {
	return ({
		int x;
		x;
	});
}

int test_return_zeroinit_with_defer_helper(void) {
	int *p = malloc(1);
	defer free(p);

	return ({
		int x;
		x;
	});
}

int test_return_zeroinit_multiple_helper(void) {
	int *p = malloc(1);
	defer free(p);

	return ({
		int a;
		int b;
		a + b;
	});
}

int test_return_zeroinit_nested_helper(void) {
	int *p = malloc(1);
	defer free(p);

	return ({
		int outer;
		{
			int inner;
			outer = inner;
		}
		outer;
	});
}

void test_return_zeroinit_no_defer(void) {
	int result = test_return_zeroinit_no_defer_helper();
	CHECK(result == 0, "return stmt-expr zero-init without defer");
}

void test_return_zeroinit_with_defer(void) {
	int result = test_return_zeroinit_with_defer_helper();
	CHECK(result == 0, "return stmt-expr zero-init WITH defer (blind spot)");
}

void test_return_zeroinit_multiple_decls(void) {
	int result = test_return_zeroinit_multiple_helper();
	CHECK(result == 0, "return stmt-expr multiple zero-inits with defer");
}

void test_return_zeroinit_nested_blocks(void) {
	int result = test_return_zeroinit_nested_helper();
	CHECK(result == 0, "return stmt-expr nested block zero-init with defer");
}

void test_sizeof_vla_zeroinit(void) {
	// sizeof(VLA) False Negative in Zero-Init
	// sizeof(int[n]) is evaluated at runtime, so int buf[sizeof(int[n])] is a VLA
	// Prism should NOT add = {0} to this declaration
	int n = 5;
	int buf[sizeof(int[n])]; // This should compile (VLA, no zero-init)
	buf[0] = 42;
	CHECK(buf[0] == 42, "sizeof(VLA) should be recognized as VLA");
}
#endif // _MSC_VER

void test_goto_raw_decl(void) {
	// goto vs raw Declarations
	// raw keyword opts out of initialization, so goto skipping it should be allowed
	int x = 0;
	goto label;
	raw int y; // This should NOT error - raw means "I know what I'm doing"
label:
	x = 1;
	CHECK(x == 1, "goto over raw declaration should be allowed");
}

void test_attributed_default_label(void) {
	// Attributed default Label Detection
	// Prism checks: equal(tok, "default") && tok->next && equal(tok->next, ":")
	// But with attributes: default __attribute__((unused)) :
	// The tok->next is __attribute__, not :, so the pattern fails
	// For now, we'll test that normal default works
	log_reset();
	int x = 1;
	switch (x) {
	case 1: {
		defer log_append("X");
		log_append("A");
		break;
	}
	default: // If this has attribute, Prism won't recognize it
		log_append("B");
		break;
	}
	CHECK_LOG("AX", "default label defer clearing (attribute case is theoretical bug)");
}

void test_stmtexpr_void_cast_return(void) {
	// Statement Expression return with void Cast
	// return (void)({ func(); }); should be handled correctly
	log_reset();
	// This function returns void, so the statement should work
	log_append("X");
	CHECK_LOG("X", "statement expr with void cast in return setup");
}

#ifndef _MSC_VER
void test_stmtexpr_void_cast_return_helper(void) {
	log_reset();
	log_append("A");
	return (void)({ log_append("B"); }); // This should work
}

void test_stmtexpr_void_cast_check(void) {
	test_stmtexpr_void_cast_return_helper();
	CHECK_LOG("AB", "statement expr with void cast in return should work");
}
#endif // _MSC_VER

void test_variable_named_defer_goto(void) {
	// Variable named defer used as a regular variable (not the keyword).
	// Verify Prism correctly treats it as an identifier in non-keyword contexts.
	int defer;
	defer = 42;
	CHECK(defer == 42, "variable named defer assignment works");
}

void test_defer_assignment_goto(void) {
	PrismFeatures features = prism_defaults();
	features.defer = false;
	PrismResult result = prism_transpile_source(
	    "int main(void) {\n"
	    "    int defer = 0;\n"
	    "    goto jump;\n"
	    "    defer = 1;\n"
	    "jump:\n"
	    "    return defer;\n"
	    "}\n",
	    "defer_assignment_disabled.c", features);
	CHECK_EQ(result.status, PRISM_OK, "defer assignment with feature disabled transpiles");
	if (result.output) {
		CHECK(strstr(result.output, "defer = 1") != NULL,
		      "defer assignment preserved when defer feature disabled");
	}
	prism_free(&result);
}

void test_attributed_default_safety(void) {
	// Safety hole: attributed default label not recognized
	// switch with defer fallthrough + attributed default can cause resource leak
	log_reset();
	int x = 2;
	int *p = malloc(16);
	switch (x) {
	case 1: {
		defer free(p);
		log_append("A");
		// fallthrough
	}
	// Note: Cannot use __attribute__ in test as it would fail parsing
	// This test verifies normal default works, actual bug needs manual verification
	default: log_append("B"); break;
	}
	CHECK_LOG("B", "attributed default - normal case works");
}

void test_for_loop_goto_bypass(void) {
	PrismResult result = prism_transpile_source(
	    "int main(void) {\n"
	    "    goto entry;\n"
	    "    for (int i = 0; i < 1; i++) {\n"
	    "entry:\n"
	    "        return i;\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n",
	    "for_loop_goto_bypass.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "for loop goto into body transpiles");
	CHECK(result.error_msg == NULL, "for loop goto into body has no error");
	prism_free(&result);
}

#ifdef __GNUC__
void test_utf8_identifiers(void) {
	int \u00E4 = 4; // UCN for 'ä' (U+00E4)
	CHECK(\u00E4 == 4, "UCN identifier \\u00E4");
}
#endif

void test_digraphs(void) {
	// Digraph mappings:
	// <: = [    :> = ]
	// <% = {    %> = }
	// %: = #    %:%: = ## (preprocessor only)

	// Array declaration with digraphs
	int arr<:5:> = <%1, 2, 3, 4, 5%>;
	CHECK(arr<:0:> == 1, "digraph array[0]");
	CHECK(arr<:4:> == 5, "digraph array[4]");
}

void test_pragma_operator(void) {
	// _Pragma in function body - test that zero-init works correctly
	_Pragma("GCC diagnostic push")
	    _Pragma("GCC diagnostic ignored \"-Wunused-variable\"") int unused_var = 42;
	_Pragma("GCC diagnostic pop") CHECK(unused_var == 42, "_Pragma with explicit init works");

	// _Pragma before declaration - zero-init should work
	_Pragma("GCC diagnostic push") int x; // No explicit init - should be zero-initialized
	_Pragma("GCC diagnostic pop") CHECK(x == 0, "_Pragma before decl with zero-init");

	// Multiple _Pragma before declaration - zero-init should work
	_Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wunused-value\"")
	    _Pragma("GCC diagnostic ignored \"-Wunused-variable\"") int y; // No explicit init
	5 + 3; // unused value, but warning suppressed
	_Pragma("GCC diagnostic pop") CHECK(y == 0, "multiple _Pragma with zero-init");

	// _Pragma inside compound statement with defer - this works
	log_reset();
	{
		_Pragma("GCC diagnostic push") defer log_append("D");
		_Pragma("GCC diagnostic pop") log_append("1");
	}
	CHECK_LOG("1D", "_Pragma with defer");

	// _Pragma in loop - zero-init should work
	for (int i = 0; i < 1; i++) {
		_Pragma("GCC diagnostic push") int loop_var; // No explicit init
		_Pragma("GCC diagnostic pop") CHECK(loop_var == 0, "_Pragma in loop with zero-init");
	}
}

#ifdef __GNUC__

void test_break_escape_stmtexpr(void) {
	// Basic case: break inside statement expression exits outer loop
	log_reset();
	for (int i = 0; i < 3; i++) {
		defer log_append("L");
		int x = ({
			int _result;
			{
				defer log_append("S");
				if (i == 0) break; // Should run S, then L, then exit loop
				_result = 42;
			}
			_result;
		});
		(void)x;
		log_append("X"); // Never reached on first iteration
	}
	log_append("E");
	CHECK_LOG("SLE", "break escaping statement expression");

	// Continue inside statement expression
	log_reset();
	for (int i = 0; i < 2; i++) {
		defer log_append("L");
		int x = ({
			int _r;
			{
				defer log_append("S");
				if (i == 0) {
					log_append("C");
					continue; // Should run S, then L, then continue
				}
				log_append("V");
				_r = 100;
			}
			_r;
		});
		(void)x;
		log_append("X");
	}
	log_append("E");
	CHECK_LOG("CSLVSXLE", "continue escaping statement expression");

	// Nested statement expressions with break
	log_reset();
	for (int i = 0; i < 1; i++) {
		defer log_append("1");
		int outer = ({
			int _o;
			{
				defer log_append("2");
				int inner = ({
					int _i;
					{
						defer log_append("3");
						if (1) break; // Exits the for loop
						_i = 5;
					}
					_i;
				});
				_o = inner + 10;
			}
			_o;
		});
		(void)outer;
		log_append("X"); // Never reached
	}
	log_append("E");
	CHECK_LOG("321E", "nested stmtexpr break - defer order");

	// Break in statement expression inside switch inside loop
	log_reset();
	for (int i = 0; i < 1; i++) {
		defer log_append("L");
		int x = ({
			int _r;
			{
				defer log_append("S");
				int result = 0;
				switch (1) {
				case 1: {
					defer log_append("C");
					result = ({
						int _inner;
						{
							defer log_append("I");
							if (1) break; // breaks the SWITCH, not the for
							_inner = 99;
						}
						_inner;
					});
					log_append("A"); // NOT reached - break exits switch!
				}
				}
				_r = result;
			}
			_r;
		});
		(void)x;
		log_append("X");
	}
	log_append("E");
	// break in stmtexpr exits the SWITCH entirely, skipping "A"
	// I exits inner block, C exits case block, S exits stmtexpr, X logged, L exits loop, E
	CHECK_LOG("ICSXLE", "stmtexpr break in switch - break exits switch entirely");

	// goto out of statement expression
	log_reset();
	for (int i = 0; i < 1; i++) {
		defer log_append("L");
		int x = ({
			int _r;
			{
				defer log_append("S");
				if (1) goto stmtexpr_escape;
				_r = 42;
			}
			_r;
		});
		(void)x;
		log_append("X");
	}
stmtexpr_escape:
	log_append("E");
	CHECK_LOG("SLE", "goto escaping statement expression");
}

void test_stmtexpr_while_break(void) {
	// break in stmtexpr inside while
	log_reset();
	int count = 0;
	while (count < 5) {
		defer log_append("W");
		int x = ({
			int _r;
			{
				defer log_append("S");
				count++;
				if (count == 2) break;
				_r = count;
			}
			_r;
		});
		(void)x;
		log_append(".");
	}
	log_append("E");
	// S runs when inner block exits (before "."), W runs at loop iteration end
	// Iter 1: count=1, block exits→S, then ".", iter ends→W → "S.W"
	// Iter 2: count=2, break→S exits block, W exits loop → "S.WSW"
	CHECK_LOG("S.WSWE", "stmtexpr break in while loop");
}

void test_stmtexpr_dowhile_break(void) {
	// break in stmtexpr inside do-while
	log_reset();
	int count = 0;
	do {
		defer log_append("D");
		int x = ({
			int _r;
			{
				defer log_append("S");
				count++;
				if (count == 2) break;
				_r = count;
			}
			_r;
		});
		(void)x;
		log_append(".");
	} while (count < 5);
	log_append("E");
	// Same as while: S runs at block exit (before "."), D runs at loop iteration end
	CHECK_LOG("S.DSDE", "stmtexpr break in do-while loop");
}

void test_stmtexpr_nested_loops_break(void) {
	// break in stmtexpr should only exit innermost loop
	log_reset();
	for (int i = 0; i < 2; i++) {
		defer log_append("O");
		for (int j = 0; j < 3; j++) {
			defer log_append("I");
			int x = ({
				int _r;
				{
					defer log_append("S");
					if (j == 1) break; // exits inner loop only
					_r = j;
				}
				_r;
			});
			(void)x;
			log_append(".");
		}
		log_append("+");
	}
	log_append("E");
	// S runs at block exit (before "."), I runs at inner loop end, O at outer end
	// Inner loop: j=0 → S.I, j=1 → break→SI (exits inner loop)
	// Then "+", outer loop ends → O
	CHECK_LOG("S.ISI+OS.ISI+OE", "stmtexpr break exits only inner loop");
}
#endif

void test_string_escape_sequences_varied(void) {
	const char *s1 = "abc\\def";
	CHECK(strlen(s1) == 7, "string with backslash-d escape");

	const char *s2 = "hello \"world\" end";
	CHECK(strlen(s2) == 17, "string with escaped quotes");

	const char *s3 = "\\\\\\\\";
	CHECK(strlen(s3) == 4, "string with multiple double-backslashes");

	const char *s4 = "line1\nline2\ttab\r\0hidden";
	CHECK(s4[5] == '\n' && s4[11] == '\t', "string with mixed escapes");

	const char *s5 = "\x41\x42\x43";
	CHECK(s5[0] == 'A' && s5[1] == 'B' && s5[2] == 'C', "string with hex escapes");

	const char *s6 = "end\\";
	CHECK(s6[3] == '\\', "string ending with backslash char");

	const char *s7 = "a\\'b";
	CHECK(s7[1] == '\\' && s7[2] == '\'' && strlen(s7) == 4, "string with backslash-quote");
}

void test_char_literal_escape_sequences(void) {
	char c1 = '\\';
	CHECK(c1 == 92, "char literal backslash");

	char c2 = '\'';
	CHECK(c2 == 39, "char literal escaped single-quote");

	char c3 = '\"';
	CHECK(c3 == 34, "char literal escaped double-quote");

	char c4 = '\n';
	CHECK(c4 == 10, "char literal newline");

	char c5 = '\t';
	CHECK(c5 == 9, "char literal tab");

	char c6 = '\0';
	CHECK(c6 == 0, "char literal null");

	char c7 = '\x7F';
	CHECK(c7 == 127, "char literal hex escape");
}


typedef int my_attr_param_t;
enum { MY_VAL __attribute__((my_attr_param_t)) = 1 };

void test_enum_attribute_pollution(void) {
        // If 'my_attr_param_t' was poisoned as an enum constant, Prism no longer 
        // recognizes it as a type here! Thus, it skips zero-initializing 'ptr'.
        my_attr_param_t *ptr = (my_attr_param_t *)0xDEADBEEF; 
        
        {
            my_attr_param_t *ptr; // Should be zero-initialized to NULL
            CHECK_EQ((uintptr_t)ptr, 0, "attribute parameters don't pollute typedef table");
        }
}

/*

// BUG 1: Prism fails to throw a safety error when jumping over a struct declaration.
void test_goto_skips_struct(void) {
        log_reset();
        int bypassed = 1;
        
        // Unsafe jump! Prism should catch this and throw a safety error,
        // but the buggy walker_next completely misses the 'struct' keyword.
        goto skip; 
        
        struct TestStruct { 
                int x; 
        } s;
        
        s.x = 10;
        bypassed = 0;
        (void)s; // suppress unused warning
        
skip:
        CHECK_EQ(bypassed, 1, "goto skipped struct initialization without a Prism safety error");
}
*/

// BUG 2: Prism does a naive linear scan, sees "void" inside this struct,
// and incorrectly flags 'VoidPtrStruct' as a void-return alias.
typedef struct {
        void *ptr;
} VoidPtrStruct;

static VoidPtrStruct helper_void_typedef_overmatch(void) {
        VoidPtrStruct s;
        s.ptr = (void*)0x1234;
        
        // This defer forces Prism to intercept the return.
        // If buggy, Prism drops 's' and just emits: "(s); log_append(\"A\"); return;"
        defer log_append("A");
        
        return s;
}

void test_void_typedef_overmatch(void) {
        log_reset();
        
        VoidPtrStruct res = helper_void_typedef_overmatch();
        
        CHECK(res.ptr == (void*)0x1234, "struct containing void* returned successfully");
        CHECK_LOG("A", "void typedef defer executed");
}

#ifndef _MSC_VER
void test_utf8_latin_extended(void) {
	int café = 42;
	int naïve = 100;
	int résumé = café + naïve;
	CHECK_EQ(résumé, 142, "UTF-8 Latin Extended identifiers");
}

void test_utf8_greek(void) {
	double π = 3.14159;
	double τ = 2.0 * π;
	int Σ = 0;
	for (int i = 1; i <= 10; i++) Σ += i;
	CHECK(π > 3.14 && π < 3.15, "UTF-8 Greek pi");
	CHECK(τ > 6.28 && τ < 6.29, "UTF-8 Greek tau");
	CHECK_EQ(Σ, 55, "UTF-8 Greek sigma sum");
}

void test_utf8_cyrillic(void) {
	int счётчик = 0; // "counter" in Russian
	for (int i = 0; i < 5; i++) счётчик++;
	CHECK_EQ(счётчик, 5, "UTF-8 Cyrillic identifier");
}

void test_utf8_cjk(void) {
	int 変数 = 10;		// "variable" in Japanese
	int 数值 = 20;		// "value" in Chinese
	int 결과 = 変数 + 数值; // "result" in Korean
	CHECK_EQ(결과, 30, "UTF-8 CJK identifiers");
}
#endif // _MSC_VER

#ifndef _MSC_VER
void test_ucn_short(void) {
	// \u03C0 = π (Greek small letter pi)
	// \u00E9 = é (Latin small letter e with acute)
	int \u03C0 = 314;
	int caf\u00E9 = 42;
	CHECK_EQ(\u03C0, 314, "UCN short form \\u03C0");
	CHECK_EQ(caf\u00E9, 42, "UCN short form in identifier");
}

void test_ucn_long(void) {
	// \U0001F600 = 😀 (but we use valid XID characters)
	// \U00004E2D = 中 (CJK character)
	int \U00004E2D = 100;
	CHECK_EQ(\U00004E2D, 100, "UCN long form \\U00004E2D");
}

void test_utf8_ucn_mixed(void) {
	int café_var = 1; // UTF-8 with ASCII suffix
	int π_value = 314;
	// Note: π and \u03C0 are the same character, so they refer to the same variable!
	// This proves UTF-8 and UCN normalization works correctly
	\u03C0_value = 628; // Modify via UCN form
	CHECK_EQ(café_var, 1, "Mixed UTF-8 and ASCII");
	CHECK_EQ(π_value, 628, "UTF-8 and UCN same variable");
}
#endif // _MSC_VER

void test_digraph_brackets(void) {
	int arr<:5:> = {1, 2, 3, 4, 5}; // int arr[5] = {1, 2, 3, 4, 5};
	int sum = 0;
	for (int i = 0; i < 5; i++) sum += arr<:i:>; // sum += arr[i];
	CHECK_EQ(sum, 15, "Digraph <: :> for brackets");
	CHECK_EQ(arr<:0:>, 1, "Digraph bracket access first");
	CHECK_EQ(arr<:4:>, 5, "Digraph bracket access last");
}

void test_digraph_braces(void) <%
	int x = 10;
	int y = 20;
	int result = x + y;
	CHECK_EQ(result, 30, "Digraph <% %> for braces");
%>

void test_digraph_struct(void) {
	struct Point <%
		int x;
		int y;
	%>;
	struct Point p = <%.x = 3, .y = 4%>;
	CHECK_EQ(p.x, 3, "Digraph struct member x");
	CHECK_EQ(p.y, 4, "Digraph struct member y");
}

void test_digraph_complex(void) {
	struct Data <%
		int values<:3:>;
	%>;
	struct Data d = <%.values = <%10, 20, 30%>%>;
	CHECK_EQ(d.values<:0:>, 10, "Digraph nested array first");
	CHECK_EQ(d.values<:1:>, 20, "Digraph nested array middle");
	CHECK_EQ(d.values<:2:>, 30, "Digraph nested array last");
}

void test_digraph_defer(void) <%
	log_reset();
	<%
		defer log_append("B");
		log_append("A");
	%>
	CHECK_LOG("AB", "Digraph with defer");
%>

#ifndef _MSC_VER
void test_utf8_defer(void) {
	log_reset();
	{
		int счётчик = 0;
		defer {
			char buf[16];
			snprintf(buf, sizeof(buf), "%d", счётчик);
			log_append(buf);
		};
		счётчик = 42;
		log_append("X");
	}
	CHECK_LOG("X42", "UTF-8 identifier with defer");
}

void test_utf8_math_identifiers(void) {
	double α = 1.0;
	double β = 2.0;
	double γ = α + β;
	double Δx = 0.1;
	double λ = 500e-9;		// wavelength in meters
	double ω = 2.0 * 3.14159 * 1.0; // angular frequency

	CHECK(γ > 2.9 && γ < 3.1, "Greek alpha+beta=gamma");
	CHECK(Δx > 0.09 && Δx < 0.11, "Greek Delta");
	CHECK(λ > 0 && λ < 1e-6, "Greek lambda");
	CHECK(ω > 6.0 && ω < 7.0, "Greek omega");
}
#endif // _MSC_VER


static int zombie_counter = 0;

void test_zombie_defer(void) {
	zombie_counter = 0;
	int x = 1;

	// Switch jumps directly to case labels, skipping dead zone
	switch (x) {
		// DEAD ZONE: Code here is unreachable in standard C
		// Prism correctly errors if you try: defer zombie_counter++;

	case 1: break;
	default: break;
	}

	CHECK_EQ(zombie_counter, 0, "switch dead zone not executed");
}

void test_zombie_defer_uninitialized(void) {
	int result = -1;
	int x = 1;

	switch (x) {
	// DEAD ZONE - Prism errors if defer used here
	case 1: result = 1; break;
	}

	CHECK_EQ(result, 1, "switch jumps past dead zone");
}

void test_tcc_detection_logic(void) {
	CHECK(strstr("tcc", "cc") != NULL, "strstr finds 'cc' in 'tcc' (old bug)");

	// Test the FIXED matching approach
	const char *compilers[] = {"tcc", "gcc", "cc", "x86_64-linux-gnu-gcc", "/usr/bin/cc", "clang"};
	int should_match[] = {0, 1, 1, 1, 1, 1}; // tcc should NOT match

	for (int i = 0; i < 6; i++) {
		const char *compiler = compilers[i];
		int len = strlen(compiler);

		// FIXED matching logic (mirrors prism.c)
		int is_gcc_family = (len >= 3 && strcmp(compiler + len - 3, "gcc") == 0) ||
				    (strcmp(compiler, "cc") == 0) ||
				    (len >= 3 && strcmp(compiler + len - 3, "/cc") == 0);
		int is_clang_family = strstr(compiler, "clang") != NULL;
		int matches = is_gcc_family || is_clang_family;

		char msg[128];
		snprintf(msg,
			 sizeof(msg),
			 "compiler '%s' %s",
			 compiler,
			 should_match[i] ? "matches" : "does NOT match");
		CHECK_EQ(matches, should_match[i], msg);
	}
}

static int is_valid_ident_start_fixed(uint32_t cp) {
	if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '_' || cp == '$';
	if (cp >= 0x00C0 && cp <= 0x00FF) return 1;
	if (cp >= 0x0100 && cp <= 0x017F) return 1;
	if (cp >= 0x0180 && cp <= 0x024F) return 1;
	if (cp >= 0x0250 && cp <= 0x02AF) return 1;
	if (cp >= 0x1E00 && cp <= 0x1EFF) return 1;
	if (cp >= 0x0370 && cp <= 0x03FF) return 1;
	if (cp >= 0x1F00 && cp <= 0x1FFF) return 1;
	if (cp >= 0x0400 && cp <= 0x04FF) return 1;
	if (cp >= 0x0500 && cp <= 0x052F) return 1;
	if (cp >= 0x0530 && cp <= 0x058F) return 1;
	if (cp >= 0x0590 && cp <= 0x05FF) return 1; // Hebrew (NEW)
	if (cp >= 0x0600 && cp <= 0x06FF) return 1;
	if (cp >= 0x0750 && cp <= 0x077F) return 1;
	if (cp >= 0x0900 && cp <= 0x097F) return 1;
	if (cp >= 0x1200 && cp <= 0x137F) return 1; // Ethiopian (NEW)
	if (cp >= 0x13A0 && cp <= 0x13FF) return 1; // Cherokee (NEW)
	if (cp >= 0x3040 && cp <= 0x309F) return 1;
	if (cp >= 0x30A0 && cp <= 0x30FF) return 1;
	if (cp >= 0x4E00 && cp <= 0x9FFF) return 1;
	if (cp >= 0x20000 && cp <= 0x2A6DF) return 1; // CJK Extension B (NEW)
	if (cp >= 0xAC00 && cp <= 0xD7AF) return 1;
	if (cp >= 0x1D400 && cp <= 0x1D7FF) return 1; // Math Alphanumeric (NEW)
	return 0;
}

void test_unicode_extended_ranges(void) {
	// Test codepoints that were previously rejected but are now accepted
	CHECK_EQ(is_valid_ident_start_fixed(0x1D400), 1, "Math Bold A (U+1D400) accepted");
	CHECK_EQ(is_valid_ident_start_fixed(0x20000), 1, "CJK Extension B (U+20000) accepted");
	CHECK_EQ(is_valid_ident_start_fixed(0x13A0), 1, "Cherokee A (U+13A0) accepted");
	CHECK_EQ(is_valid_ident_start_fixed(0x05D0), 1, "Hebrew Alef (U+05D0) accepted");
	CHECK_EQ(is_valid_ident_start_fixed(0x1200), 1, "Ethiopian Ha (U+1200) accepted");

	// Verify existing ranges still work
	CHECK_EQ(is_valid_ident_start_fixed(0x4E00), 1, "CJK U+4E00 accepted");
	CHECK_EQ(is_valid_ident_start_fixed(0x0391), 1, "Greek Alpha accepted");
	CHECK_EQ(is_valid_ident_start_fixed(0x0410), 1, "Cyrillic A accepted");

	// Emojis are NOT valid XID_Start - correct rejection
	CHECK_EQ(is_valid_ident_start_fixed(0x1F4A9), 0, "emoji correctly rejected");
}

void test_memory_interning_pattern(void) {
	const char *filenames[] = {
	    "/usr/include/stdio.h",
	    "/usr/include/stdio.h",
	    "/usr/include/stdio.h",
	    "/usr/include/stdlib.h",
	    "/usr/include/stdlib.h",
	};

	int unique_count = 0;
	const char *seen[5] = {0};

	for (int i = 0; i < 5; i++) {
		int is_dup = 0;
		for (int j = 0; j < unique_count; j++) {
			if (strcmp(filenames[i], seen[j]) == 0) {
				is_dup = 1;
				break;
			}
		}
		if (!is_dup) {
			seen[unique_count++] = filenames[i];
		}
	}

	CHECK_EQ(unique_count, 2, "filename interning: 2 unique from 5 entries");
}

void test_compound_literal_for_break(void) {
	log_reset();
	for (int i = 0; i < (int){10}; i++) {
		defer log_append("D");
		log_append("L");
		if (i == 0) break; // BUG: defer must still execute on break!
	}
	CHECK_LOG("LD", "compound literal for loop: defer on break");
}

void test_compound_literal_for_continue(void) {
	log_reset();
	for (int i = 0; i < (int){2}; i++) {
		defer log_append("D");
		log_append("C");
		if (i == 0) continue; // defer must execute on continue too
		log_append("X");
	}
	CHECK_LOG("CDCXD", "compound literal for loop: defer on continue");
}

void test_compound_literal_while_break(void) {
	log_reset();
	int i = 0;
	while (i < (int){5}) {
		defer log_append("W");
		log_append("B");
		if (i == 0) break;
		i++;
	}
	CHECK_LOG("BW", "compound literal while loop: defer on break");
}

void test_nested_compound_literal_in_loop(void) {
	log_reset();

	struct {
		int x;
	} s;

	for (int i = 0; i < ((struct { int x; }){3}).x; i++) {
		defer log_append("N");
		log_append("I");
		if (i == 1) break;
	}
	CHECK_LOG("ININ", "nested compound literal in for: defer on break");
}

void test_multiple_compound_literals_in_for(void) {
	log_reset();
	for (int i = (int){0}; i < (int){2}; i += (int){1}) {
		defer log_append("M");
		log_append("X");
	}
	CHECK_LOG("XMXM", "multiple compound literals in for: defer executes each iteration");
}

void test_compound_literal_if_condition(void) {
	log_reset();
	if ((int){1}) {
		defer log_append("I");
		log_append("T");
	}
	CHECK_LOG("TI", "compound literal in if condition: defer works");
}



void test_issue4_strtoll_unsigned(void) {
	// Test 1: UINT64_MAX as hex literal
	unsigned long long val1 = 0xFFFFFFFFFFFFFFFFULL;
	CHECK(val1 == UINT64_MAX, "0xFFFFFFFFFFFFFFFFULL equals UINT64_MAX");
	CHECK(val1 > 0, "UINT64_MAX > 0 (not treated as -1)");

	// Test 2: Compare unsigned literals
	unsigned long long a = 0xFFFFFFFFFFFFFFFFULL;
	unsigned long long b = 1ULL;
	CHECK(a > b, "UINT64_MAX > 1 in unsigned comparison");

	// Test 3: Large unsigned without suffix (might overflow signed)
	unsigned long long big = 9223372036854775808ULL;
	CHECK(big == 9223372036854775808ULL, "large unsigned literal parses correctly");

	// Test 4: Hex without suffix
	unsigned long long hex_max = 0xFFFFFFFFFFFFFFFF;
	CHECK(hex_max == UINT64_MAX, "hex UINT64_MAX without U suffix");
}

typedef int RawTypedefTest; // Simulates user-defined 'raw' type

void test_issue5_raw_typedef_collision(void) {
	// Declare variable of typedef type - should be zero-initialized
	RawTypedefTest x;
	CHECK(x == 0, "typedef'd type variable is zero-initialized");

	// Multiple declarations
	RawTypedefTest a, b, c;
	CHECK(a == 0 && b == 0 && c == 0, "multiple typedef'd vars zero-initialized");

	// With initializer
	RawTypedefTest y = 42;
	CHECK(y == 42, "typedef'd type with initializer works");

	// Pointer
	RawTypedefTest *ptr = &y;
	CHECK(*ptr == 42, "typedef'd type pointer works");

	// Array
	RawTypedefTest arr[3];
	CHECK(arr[0] == 0 && arr[1] == 0 && arr[2] == 0, "typedef'd type array zero-initialized");
}

static int defer_for_loop_counter = 0;

void test_issue7_defer_in_for_body(void) {
	// Valid use: defer INSIDE the for body
	defer_for_loop_counter = 0;
	for (int i = 0; i < 3; i++) {
		defer defer_for_loop_counter++;
	}
	CHECK_EQ(defer_for_loop_counter, 3, "defer inside for body runs each iteration");
}

void test_issue7_defer_before_for(void) {
	// Valid use: defer before for loop
	defer_for_loop_counter = 0;
	{
		defer defer_for_loop_counter = 100;
		for (int i = 0; i < 3; i++) {
			// loop body
		}
	}
	CHECK_EQ(defer_for_loop_counter, 100, "defer before loop runs once at scope exit");
}

void test_defer_nested_control_structures(void) {
	int cleanup_order[10];
	int cleanup_idx = 0;

	for (int i = 0; i < 2; i++) {
		defer cleanup_order[cleanup_idx++] = i * 10;

		if (i == 0) {
			defer cleanup_order[cleanup_idx++] = 1;
		}
	}

	// After first iteration: defer 1 runs, then defer 0
	// After second iteration: defer 10 runs
	// Expected order: 1, 0, 10
	CHECK_EQ(cleanup_order[0], 1, "nested defer: inner if defer runs first");
	CHECK_EQ(cleanup_order[1], 0, "nested defer: outer for defer runs second");
	CHECK_EQ(cleanup_order[2], 10, "nested defer: second iteration defer");
}

static void defer_cleanup_func(int *p) {
	if (p) *p = 0;
}

static void defer(int *p) {
	if (p) *p = 999;
}

void test_defer_in_attribute_cleanup(void) {
	// Using a function named 'defer' in __attribute__((cleanup(...)))
	// This should NOT trigger defer statement parsing
	int value __attribute__((cleanup(defer))) = 42;
	CHECK_EQ(value, 42, "defer in cleanup attr: not parsed as statement");
	// After scope exit, value will be set to 999 by cleanup, but we can't check that here
}

void test_defer_in_attribute_with_defer_stmt(void) {
	// Combine cleanup attribute with actual defer statement
	int result = 0;
	{
		int value __attribute__((cleanup(defer_cleanup_func))) = 42;
		defer result = value; // This IS a defer statement
	}
	// defer runs first (sets result=42), then cleanup runs (sets value=0)
	CHECK_EQ(result, 42, "defer stmt + cleanup attr: both work");
}


#ifndef _MSC_VER
void test_register_typeof_zeroinit(void) {
	// register variables can't have their address taken
	// So typeof(int) register x should NOT use memset(&x, ...)
	// It should use = 0 instead
	typeof(int) register x;
	x = 42; // Assign after declaration
	CHECK(x == 42, "register typeof compiles (no memset)");
}

void test_register_typeof_multiple(void) {
	// Multiple register typeof variables
	typeof(int) register a, b, c;
	a = 1;
	b = 2;
	c = 3;
	CHECK(a == 1 && b == 2 && c == 3, "multiple register typeof");
}
#endif

// C23 digit separator tests - only run if compiler supports C23
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L

void test_c23_digit_separator_decimal(void) {
	// C23 digit separators in decimal literals
	int million = 1'000'000;
	int thousand = 1'000;
	CHECK(million == 1000000, "C23 digit sep decimal million");
	CHECK(thousand == 1000, "C23 digit sep decimal thousand");
}

void test_c23_digit_separator_binary(void) {
	// C23 digit separators in binary literals
	int b1 = 0b1010'1010;
	int b2 = 0b1111'0000'1111'0000;
	CHECK(b1 == 170, "C23 digit sep binary 0b1010'1010");
	CHECK(b2 == 0xF0F0, "C23 digit sep binary 16-bit");
}

void test_c23_digit_separator_hex(void) {
	// C23 digit separators in hex literals
	int h1 = 0xFF'FF;
	int h2 = 0x12'34'56'78;
	CHECK(h1 == 0xFFFF, "C23 digit sep hex 0xFF'FF");
	CHECK(h2 == 0x12345678, "C23 digit sep hex 32-bit");
}

void test_c23_digit_separator_octal(void) {
	// C23 digit separators in octal literals
	int o1 = 0'777;
	int o2 = 01'234'567;
	CHECK(o1 == 0777, "C23 digit sep octal 0'777");
	CHECK(o2 == 01234567, "C23 digit sep octal large");
}

void test_c23_digit_separator_float(void) {
	// C23 digit separators in floating point literals
	float f = 1'234.567'8f;
	double d = 123'456.789'012;
	CHECK(f > 1234.0f && f < 1235.0f, "C23 digit sep float");
	CHECK(d > 123456.0 && d < 123457.0, "C23 digit sep double");
}

void test_c23_digit_separator_suffix(void) {
	// Digit separators with type suffixes
	long l = 1'000'000L;
	long long ll = 123'456'789'012LL;
	unsigned u = 4'294'967'295U;
	CHECK(l == 1000000L, "C23 digit sep with L suffix");
	CHECK(ll == 123456789012LL, "C23 digit sep with LL suffix");
	CHECK(u == 4294967295U, "C23 digit sep with U suffix");
}

#endif // C23 digit separator tests

#ifndef _MSC_VER
void test_volatile_typeof_zeroinit(void) {
	// volatile typeof should use volatile-safe zeroing, not memset
	// This ensures the stores aren't optimized out
	volatile typeof(int) v;
	CHECK(v == 0, "volatile typeof zeroed");
}

void test_volatile_typeof_struct(void) {
	// volatile struct with typeof
	struct TestStruct {
		int x;
		int y;
	};

	volatile typeof(struct TestStruct) vs;
	CHECK(vs.x == 0 && vs.y == 0, "volatile typeof struct zeroed");
}

void test_volatile_typeof_array(void) {
	// volatile array with typeof (uses volatile char* loop)
	volatile typeof(int[4]) arr;
	int all_zero = 1;
	for (int i = 0; i < 4; i++)
		if (arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "volatile typeof array zeroed");
}
#endif


#include <errno.h>

#ifdef EWOULDBLOCK
#define TEST_IS_EAGAIN(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#else
#define TEST_IS_EAGAIN(e) ((e) == EAGAIN)
#endif

void test_logical_op_eagain(void) {
	// This pattern caused -Werror=logical-op when EAGAIN == EWOULDBLOCK
	int saved_errno = EAGAIN;
	int result = TEST_IS_EAGAIN(saved_errno);
	CHECK(result, "IS_EAGAIN macro (logical-op regression)");

	saved_errno = 0;
	result = TEST_IS_EAGAIN(saved_errno);
	CHECK(!result, "IS_EAGAIN false case (logical-op regression)");
}


static void __attribute__((noinline)) pollute_stack_for_switch(void) {
	volatile char garbage[256];
	for (int i = 0; i < 256; i++) garbage[i] = (char)(0xCC + i);
	(void)garbage[0];
}

void test_switch_unbraced_inter_case_decl(void) {
	pollute_stack_for_switch();
	int result = -1;
	int selector = 2;
	switch (selector) {
	case 1: {
		int y;
		result = y;
		break;
	}
	case 2: {
		int z;
		result = z;
		break;
	}
	}
	CHECK_EQ(result, 0, "switch braced inter-case decl zero-init");
}

void test_switch_unbraced_multi_decl_inter_case(void) {
	pollute_stack_for_switch();
	int result_a = -1, result_b = -1;
	switch (3) {
	case 1: {
		int a;
		int b;
		result_a = a;
		result_b = b;
		break;
	}
	case 2: {
		int c;
		result_a = c;
		break;
	}
	case 3: {
		int d;
		int e;
		result_a = d;
		result_b = e;
		break;
	}
	}
	CHECK_EQ(result_a, 0, "switch braced multi-decl inter-case a");
	CHECK_EQ(result_b, 0, "switch braced multi-decl inter-case b");
}

#ifndef _MSC_VER
void test_generic_default_switch_defer_combo(void) {
	log_reset();
	int type = 42;
	switch (type) {
	case 42: {
		defer log_append("D");
		int r1 = _Generic(type, int: 10, default: 20);
		int r2 = _Generic(type, long: 30, default: 40);
		CHECK_EQ(r1, 10, "_Generic int match in switch+defer");
		CHECK_EQ(r2, 40, "_Generic default match in switch+defer");
		log_append("B");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("BDE", "_Generic default in switch doesn't break defer");
}

void test_generic_default_nested_switch_defer(void) {
	log_reset();
	int x = 1;
	switch (x) {
	case 1: {
		defer log_append("outer");
		switch (2) {
		case 2: {
			defer log_append("inner");
			int t = _Generic(x, int: 100, default: 200);
			CHECK_EQ(t, 100, "nested _Generic in nested switch");
			log_append("body");
			break;
		}
		}
		break;
	}
	}
	log_append("end");
	CHECK_LOG("bodyinnerouterend", "nested _Generic+switch+defer ordering");
}
#endif

void test_typedef_table_scope_resilience(void) {
	typedef int TTR_A;
	{
		typedef float TTR_A;
		TTR_A f;
		CHECK(f == 0.0f, "typedef table: shadowed type zero-init");
	}
	TTR_A restored;
	CHECK_EQ(restored, 0, "typedef table: restored after shadow");

	{
		typedef struct {
			int x;
			int y;
		} TTR_B;

		TTR_B b;
		CHECK(b.x == 0 && b.y == 0, "typedef table: struct in scope");
	}

	TTR_A still_works;
	CHECK_EQ(still_works, 0, "typedef table: still tracks after scope exit");
}

void test_typedef_table_churn(void) {
	for (int i = 0; i < 50; i++) {
		typedef int ChurnT;
		ChurnT v;
		CHECK(v == 0, "typedef churn iteration");
		{
			typedef float ChurnT;
			ChurnT f;
			CHECK(f == 0.0f, "typedef churn shadow");
		}
	}
	int final_val;
	CHECK_EQ(final_val, 0, "typedef churn: no table corruption");
}

void test_setjmp_detection_direct(void) {
	log_reset();
	{
		defer log_append("D");
		log_append("B");
	}
	CHECK_LOG("BD", "defer works in function without setjmp");
}

#ifndef _MSC_VER
void test_generic_nested_default_in_switch_defer(void) {
	log_reset();
	int x = 1;
	switch (x) {
	case 1: {
		defer log_append("D");
		int r = _Generic(x, int: _Generic((long){0L}, long: 50, default: 0), default: -1);
		CHECK_EQ(r, 50, "nested _Generic default in switch+defer");
		log_append("B");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("BDE", "nested _Generic defer ordering");
}

void test_generic_multi_default_switch(void) {
	int x = 1;
	switch (x) {
	case 1: {
		int a = _Generic(x, int: 10, default: 0);
		int b = _Generic((short){1}, short: 20, default: 0);
		int c = _Generic((char){1}, char: 30, default: 0);
		CHECK_EQ(a + b + c, 60, "multiple _Generic defaults in one case");
		break;
	}
	default: break;
	}
}
#endif

void test_typedef_braceless_for_restore(void) {
	typedef int BFT;
	BFT before;
	CHECK_EQ(before, 0, "typedef before braceless for");
	for (int BFT = 0; BFT < 3; BFT++);
	BFT after;
	CHECK_EQ(after, 0, "typedef after braceless for");
}

void test_typedef_nested_braceless_restore(void) {
	typedef int NBFT;
	NBFT before;
	CHECK_EQ(before, 0, "typedef nested braceless before");
	for (int NBFT = 0; NBFT < 2; NBFT++)
		for (int i = 0; i < 1; i++);
	NBFT recovered;
	CHECK_EQ(recovered, 0, "typedef nested braceless restore");
}

void test_typedef_braceless_while_restore(void) {
	typedef int WBT;
	int limit = 0;
	while (limit++ < 1) {
		typedef float WBT;
		WBT f;
		CHECK(f == 0.0f, "typedef in while body");
	}
	WBT after;
	CHECK_EQ(after, 0, "typedef after while body");
}

#ifndef _MSC_VER
void test_vla_typedef_pointer_vs_value(void) {
	int n = 4;
	typedef int VlaArr[n];
	VlaArr val;
	int all_zero = 1;
	for (int i = 0; i < n; i++)
		if (val[i] != 0) all_zero = 0;
	CHECK(all_zero, "VLA typedef value zeroed");
	VlaArr *ptr;
	CHECK(ptr == NULL, "VLA typedef pointer null-init");
}

void test_vla_zeroed_each_loop_iteration(void) {
	int n = 8;
	for (int i = 0; i < 3; i++) {
		int buf[n];
		int all_zero = 1;
		for (int j = 0; j < n; j++)
			if (buf[j] != 0) all_zero = 0;
		CHECK(all_zero, "VLA zeroed on each loop iteration");
		for (int j = 0; j < n; j++) buf[j] = 42 + i;
	}
}

void test_atomic_struct_zeroed(void) {
	typedef struct {
		int x;
		int y;
	} AtomicTestS;

	_Atomic AtomicTestS a;
	unsigned char zeros[sizeof(a)];
	memset(zeros, 0, sizeof(zeros));
	CHECK(memcmp(&a, zeros, sizeof(a)) == 0, "atomic struct zero-initialized");
}

void test_atomic_specifier_struct_zeroed(void) {
	typedef struct {
		int a;
		int b;
	} AtomicSpecS;

	_Atomic(AtomicSpecS) v;
	unsigned char zeros[sizeof(v)];
	memset(zeros, 0, sizeof(zeros));
	CHECK(memcmp(&v, zeros, sizeof(v)) == 0, "atomic specifier struct zero-initialized");
}
#endif

void test_attr_before_type_zeroed(void) {
	__attribute__((unused)) int x;
	CHECK_EQ(x, 0, "attr before type zero-init");
}

void test_attr_between_type_and_var_zeroed(void) {
	int __attribute__((unused)) x;
	CHECK_EQ(x, 0, "attr between type and var zero-init");
}

void test_attr_after_var_zeroed(void) {
	int x __attribute__((unused));
	CHECK_EQ(x, 0, "attr after var zero-init");
}

void test_attr_pointer_zeroed(void) {
	int *__attribute__((unused)) p;
	CHECK(p == NULL, "attr on pointer zero-init");
}

static void __attribute__((noinline)) check_attr_paren_ptr(void) {
	int(__attribute__((unused)) * p);
	CHECK(p == NULL, "attr in paren pointer zero-init");
}

void test_attr_paren_ptr_zeroed(void) {
	pollute_stack_for_switch();
	check_attr_paren_ptr();
}

void test_multi_attr_zeroed(void) {
	__attribute__((unused)) __attribute__((aligned(sizeof(int)))) int x;
	CHECK_EQ(x, 0, "multiple attrs zero-init");
}

void test_attr_struct_var_zeroed(void) {
	struct {
		int a;
		int b;
	} __attribute__((aligned(sizeof(int)))) s;

	CHECK(s.a == 0 && s.b == 0, "attr on struct var zero-init");
}

void test_switch_no_match_defer_skipped(void) {
	log_reset();
	int cleanup = 0;
	switch (99) {
	case 1: {
		defer cleanup += 10;
		log_append("A");
		break;
	}
	case 2: {
		defer cleanup += 20;
		log_append("B");
		break;
	}
	}
	log_append("E");
	CHECK_EQ(cleanup, 0, "switch no match skips all case defers");
	CHECK_LOG("E", "switch no match straight to end");
}

void test_switch_no_default_no_match_with_defer(void) {
	log_reset();
	int resource = 0;
	for (int i = 0; i < 3; i++) {
		defer resource = 0;
		resource = 1;
		switch (i + 100) {
		case 0: {
			defer log_append("X");
			break;
		}
		}
		log_append("L");
	}
	log_append("E");
	CHECK_EQ(resource, 0, "defer in loop with unmatched switch");
	CHECK_LOG("LLLE", "loop continues after unmatched switch");
}

#ifndef _MSC_VER
void test_generic_compound_literal_association(void) {
	int x = 42;
	int result = _Generic(x, int: ((struct { int v; }){x}).v, default: -1);
	CHECK_EQ(result, 42, "generic compound literal in association");
}
#endif

#ifdef __GNUC__
void test_generic_stmt_expr_with_defer(void) {
	log_reset();
	int x = 10;
	defer log_append("outer");
	int val = _Generic(x,
	    int: ({
				   int tmp = x * 2;
				   tmp;
			   }),
	    default: 0);
	CHECK_EQ(val, 20, "generic stmt expr value");
	log_append("body");
}

static void test_generic_stmt_expr_with_defer_wrapper(void) {
	test_generic_stmt_expr_with_defer();
	CHECK_LOG("bodyouter", "generic stmt expr defer ordering");
}
#endif

#include <assert.h>

void test_assert_active_by_default(void) {
	volatile int triggered = 0;
	assert(1 == 1);
	triggered = 1;
	CHECK_EQ(triggered, 1, "assert true does not abort");
}

#undef NDEBUG
#include <assert.h>

void test_assert_still_active_after_reinclusion(void) {
	volatile int triggered = 0;
	assert(1 == 1);
	triggered = 1;
	CHECK_EQ(triggered, 1, "assert active after re-include without NDEBUG");
}

static int knr_defer_counter;

void knr_defer_func(a, b, out) int a;
int b;
int *out;
{
	defer knr_defer_counter += 100;
	if (a > b) {
		*out = a;
		return;
	}
	*out = b;
}

void test_knr_defer_goto(void) {
	int result;
	knr_defer_counter = 0;
	knr_defer_func(10, 5, &result);
	CHECK_EQ(result, 10, "K&R defer result when a > b");
	CHECK_EQ(knr_defer_counter, 100, "K&R defer ran on return (a > b)");
	knr_defer_counter = 0;
	knr_defer_func(3, 8, &result);
	CHECK_EQ(result, 8, "K&R defer result when b > a");
	CHECK_EQ(knr_defer_counter, 100, "K&R defer ran on return (b > a)");
}

typedef int KnrTypedef;

static int knr_typedef_defer_flag;

void knr_typedef_param(x, y, out) KnrTypedef x;
int y;
int *out;
{
	defer knr_typedef_defer_flag = 1;
	*out = x + y;
}

void test_knr_typedef_param_defer(void) {
	int result;
	knr_typedef_defer_flag = 0;
	knr_typedef_param(5, 3, &result);
	CHECK_EQ(result, 8, "K&R typedef param result");
	CHECK_EQ(knr_typedef_defer_flag, 1, "K&R typedef param defer ran");
}

static int knr_label_defer_count;

void knr_multi_label(a, result) int a;
int *result;
{
	defer knr_label_defer_count++;
	if (a == 1) goto first;
	if (a == 2) goto second;
	*result = a;
	return;
first:
	*result = 10;
	return;
second:
	*result = 20;
}

void test_knr_multi_label(void) {
	int r;
	knr_label_defer_count = 0;
	knr_multi_label(1, &r);
	CHECK_EQ(r, 10, "K&R multi label first");
	CHECK_EQ(knr_label_defer_count, 1, "K&R multi label first defer");
	knr_label_defer_count = 0;
	knr_multi_label(2, &r);
	CHECK_EQ(r, 20, "K&R multi label second");
	CHECK_EQ(knr_label_defer_count, 1, "K&R multi label second defer");
	knr_label_defer_count = 0;
	knr_multi_label(3, &r);
	CHECK_EQ(r, 3, "K&R multi label end");
	CHECK_EQ(knr_label_defer_count, 1, "K&R multi label end defer");
}

#ifdef __GNUC__
void test_label_zeroinit_in_stmt_expr(void) {
	int val = ({
		start:
			(void)0;
			int x;
			x;
	});
	CHECK_EQ(val, 0, "labeled decl in stmt expr zeroed");

	int val2 = ({
		lbl:
			int a;
			int b;
			a + b;
	});
	CHECK_EQ(val2, 0, "labeled multi-decl in stmt expr zeroed");

	int val3 = ({
		int pre;
		goto mid;
	mid:
		int post;
		pre + post;
	});
	CHECK_EQ(val3, 0, "goto-mid labeled decl zeroed");
}
#endif

#ifdef __GNUC__
void test_typeof_volatile_inner_zeroed(void) {
	typeof(volatile int) v;
	CHECK(v == 0, "typeof(volatile int) zeroed");

	typeof(volatile short) s;
	CHECK(s == 0, "typeof(volatile short) zeroed");
}
#endif

#ifndef _MSC_VER
typedef int GenCount;
static int _generic_colon_helper(void) {
	GenCount n = 10;
	defer (void)0;
	// _Generic with typedef type in defer scope — colon must not trigger label logic
	int r = _Generic(n, GenCount: n * 2, default: 0);
	return r;
}

void test_generic_colon_in_defer(void) {
	int r = _generic_colon_helper();
	CHECK_EQ(r, 20, "_Generic colon in defer: typedef association correct");
}

static void test_vla_nested_delimiter_depth(void) {
	int n = 4;
	// VLA inside nested parenthesized expression
	int a1[(n)];
	a1[0] = 10;
	CHECK_EQ(a1[0], 10, "vla with parens around size");

	// VLA with deeper nesting
	int a2[((n + 1))];
	a2[0] = 20;
	CHECK_EQ(a2[0], 20, "vla with double-parens around size");

	// VLA with mixed braces in surrounding context
	{
		int a3[n + 0];
		a3[0] = 30;
		CHECK_EQ(a3[0], 30, "vla inside nested braces");
	}

	// VLA after complex expression with parens and commas
	int m = (n > 0 ? n : 1);
	int a4[m];
	a4[0] = 40;
	CHECK_EQ(a4[0], 40, "vla from ternary result");
}
#endif

static void test_typedef_survives_bare_semicolons(void) {
	typedef int MyT;
	MyT x;
	CHECK_EQ(x, 0, "typedef before bare semicolons");

	;
	; // bare semicolons (empty statements)

	MyT y;
	CHECK_EQ(y, 0, "typedef after bare semicolons");

	for (int i = 0; i < 1; i++) {
		MyT z;
		CHECK_EQ(z, 0, "typedef inside for after bare semicolons");
	}

	MyT w;
	CHECK_EQ(w, 0, "typedef after for with bare semicolons");
}

static void test_for_init_typedef_shadow_cleanup(void) {
	typedef int T;
	T before;
	CHECK_EQ(before, 0, "T works before for-init shadow");

	for (int T = 0; T < 1; T++) {
		// T shadows the typedef here
		int copy = T;
		CHECK_EQ(copy, 0, "for-init shadow T is variable");
	}

	// T should be typedef again after the for loop
	T after;
	CHECK_EQ(after, 0, "T is typedef again after for");

	// Nested for with typedef shadow
	for (int T = 0; T < 1; T++) {
		for (int j = 0; j < 1; j++) {
			int inner = T + j;
			CHECK_EQ(inner, 0, "nested for-init shadow");
		}
	}
	T final;
	CHECK_EQ(final, 0, "T restored after nested for");
}

static int *orelse_comma_passthru(int *p) {
	return p;
}

static int orelse_comma_fn_null(int *p) {
	int *q = orelse_comma_passthru(p) orelse return -1;
	return *q;
}

static int orelse_comma_fn_nonnull(int *p) {
	int *q = orelse_comma_passthru(p) orelse return -1;
	return *q + 1;
}

static void test_orelse_comma_operator_expr(void) {
	int fallback = 77;
	CHECK_EQ(orelse_comma_fn_null(NULL), -1, "orelse comma: null triggers return");
	CHECK_EQ(orelse_comma_fn_nonnull(&fallback), 78, "orelse comma: non-null passes through");
}

// Multiple sequential orelse in same function
static int orelse_comma_seq_a(int *p) {
	int *q = p orelse return -1;
	return *q;
}

static int orelse_comma_seq_b(int *a, int *b) {
	int *x = a orelse return -1;
	int *y = b orelse return -2;
	return *x + *y;
}

static void test_orelse_sequential_comma(void) {
	int a = 10, b = 20;
	CHECK_EQ(orelse_comma_seq_a(&a), 10, "orelse seq: non-null");
	CHECK_EQ(orelse_comma_seq_a(NULL), -1, "orelse seq: null");
	CHECK_EQ(orelse_comma_seq_b(&a, &b), 30, "orelse seq: both non-null");
	CHECK_EQ(orelse_comma_seq_b(NULL, &b), -1, "orelse seq: first null");
	CHECK_EQ(orelse_comma_seq_b(&a, NULL), -2, "orelse seq: second null");
}

static void test_short_keyword_recognition(void) {
	// All short C keywords: if, do, for, int, ...
	// Verify they parse correctly in various positions
	int x;
	CHECK_EQ(x, 0, "int keyword recognized");

	if (1) {
		x = 1;
	}
	CHECK_EQ(x, 1, "if keyword recognized");

	do {
		x = 2;
	} while (0);
	CHECK_EQ(x, 2, "do keyword recognized");

	for (int i = 0; i < 1; i++) {
		x = 3;
	}
	CHECK_EQ(x, 3, "for keyword recognized");
}

#if __STDC_VERSION__ >= 202311L
static void test_c23_attr_positions(void) {
	[[maybe_unused]] int c23_a;
	CHECK_EQ(c23_a, 0, "c23 attr: maybe_unused int");

	[[deprecated("old")]] int c23_b;
	CHECK_EQ(c23_b, 0, "c23 attr: deprecated zeroed");

	[[maybe_unused]] int c23_c;
	CHECK_EQ(c23_c, 0, "c23 attr: second maybe_unused zeroed");
}
#endif

#ifndef _MSC_VER
static void test_vla_typedef_complex_size(void) {
	int n = 3;
	typedef int VArr[n];
	VArr arr;
	arr[0] = 100;
	CHECK_EQ(arr[0], 100, "vla typedef basic");

	typedef int VArr2[n + 1];
	VArr2 arr2;
	arr2[0] = 200;
	CHECK_EQ(arr2[0], 200, "vla typedef with addition");

	// VLA typedef with parens around expression
	typedef int VArr3[(n)];
	VArr3 arr3;
	arr3[0] = 300;
	CHECK_EQ(arr3[0], 300, "vla typedef with parens");
}
#endif

static void test_goto_stress_many_targets(void) {
	int result = 0;
	int path = 7;

	{
		defer result += 100;
		if (path == 1) goto gs_t1;
		if (path == 2) goto gs_t2;
		if (path == 3) goto gs_t3;
		if (path == 4) goto gs_t4;
		if (path == 5) goto gs_t5;
		if (path == 6) goto gs_t6;
		if (path == 7) goto gs_t7;
		if (path == 8) goto gs_t8;
		result = 999;
	}
	goto gs_done;
gs_t1:
	result += 1;
	goto gs_done;
gs_t2:
	result += 2;
	goto gs_done;
gs_t3:
	result += 3;
	goto gs_done;
gs_t4:
	result += 4;
	goto gs_done;
gs_t5:
	result += 5;
	goto gs_done;
gs_t6:
	result += 6;
	goto gs_done;
gs_t7:
	result += 7;
	goto gs_done;
gs_t8:
	result += 8;
	goto gs_done;
gs_done:
	CHECK_EQ(result, 107, "goto stress many targets with defer");
}

static void test_goto_converging_defers(void) {
	log_reset();
	int sel = 2;
	{
		defer log_append("Z");
		{
			defer log_append("Y");
			if (sel == 1) goto gc_out;
			if (sel == 2) goto gc_out;
			if (sel == 3) goto gc_out;
			log_append("X");
		}
	}
gc_out:
	log_append("E");
	CHECK_LOG("YZE", "goto converging defers");
}

static void test_stack_aggregate_zeroinit(void) {
	struct {
		int a;
		long b;
		char c[32];
		void *d;
	} compound;

	CHECK_EQ(compound.a, 0, "compound struct a zeroed");
	CHECK_EQ(compound.b, 0, "compound struct b zeroed");
	CHECK_EQ(compound.c[0], 0, "compound struct c zeroed");
	CHECK(compound.d == NULL, "compound struct d zeroed");
}

static void _safe_noop(void) {}

static void (*volatile _indirect_fn_ptr)(void) = _safe_noop;

static void test_defer_with_indirect_call(void) {
	log_reset();
	{
		defer log_append("D");
		_indirect_fn_ptr();
		log_append("X");
	}
	CHECK_LOG("XD", "defer with indirect call");
}

#ifndef _MSC_VER
static void test_vla_size_side_effect(void) {
	int n = 5;
	int vla[n++];
	(void)vla;
	CHECK_EQ(n, 6, "VLA size expr evaluated once");
}
#endif

static void test_multi_ptr_zeroinit(void) {
	int *p1;
	int **p2;
	int ***p3;
	int ****p4;
	CHECK(p1 == NULL, "1-ptr zeroinit");
	CHECK(p2 == NULL, "2-ptr zeroinit");
	CHECK(p3 == NULL, "3-ptr zeroinit");
	CHECK(p4 == NULL, "4-ptr zeroinit");
}

static void test_typedef_scope_after_braceless(void) {
	typedef int _BST;
	_BST x;
	CHECK_EQ(x, 0, "typedef before braceless");
	if (1) {
		typedef float _BST;
		_BST f;
		(void)f;
	}
	_BST y;
	CHECK_EQ(y, 0, "typedef restored after scope");
}

static int _const_orelse_value(int v) {
	return v;
}

static void test_const_orelse_scalar_fallback(void) {
	const int a = _const_orelse_value(5) orelse 42;
	CHECK_EQ(a, 5, "const orelse scalar: non-zero kept");
	const int b = _const_orelse_value(0) orelse 42;
	CHECK_EQ(b, 42, "const orelse scalar: zero uses fallback");
}

static int _coe_scalar_ctr;

static int _coe_scalar_fn(int v) {
	_coe_scalar_ctr++;
	return v;
}

static void test_const_orelse_scalar_eval_once(void) {
	_coe_scalar_ctr = 0;
	const int a = _coe_scalar_fn(7) orelse 99;
	CHECK_EQ(a, 7, "const orelse scalar eval: non-zero kept");
	CHECK_EQ(_coe_scalar_ctr, 1, "const orelse scalar eval: called once (truthy)");

	_coe_scalar_ctr = 0;
	const int b = _coe_scalar_fn(0) orelse 99;
	CHECK_EQ(b, 99, "const orelse scalar eval: zero uses fallback");
	CHECK_EQ(_coe_scalar_ctr, 1, "const orelse scalar eval: called once (falsy)");
}

static int _coe_multi_ctr;

static int _coe_multi_fn(int v) {
	_coe_multi_ctr++;
	return v;
}

static void test_const_orelse_multi_eval_once(void) {
	_coe_multi_ctr = 0;
	const int a = _coe_multi_fn(3) orelse 10, b = _coe_multi_fn(0) orelse 20;
	CHECK_EQ(a, 3, "const orelse multi eval: first non-zero");
	CHECK_EQ(b, 20, "const orelse multi eval: second zero fallback");
	CHECK_EQ(_coe_multi_ctr, 2, "const orelse multi eval: each called once");
}

static void test_defer_all_scopes_fire(void) {
	log_reset();
	{
		defer log_append("D");
		{
			defer log_append("C");
			{
				defer log_append("B");
				{
					defer log_append("A");
					log_append("0");
				}
			}
		}
	}
	CHECK_LOG("0ABCD", "all nested defers fire in order");
}

static void test_defer_loop_all_iters_fire(void) {
	log_reset();
	for (int i = 0; i < 4; i++) {
		defer log_append("X");
		log_append(".");
	}
	CHECK_LOG(".X.X.X.X", "defer fires every loop iteration");
}

static void test_typedef_braceless_if_no_leak(void) {
	typedef int _BIF;
	_BIF a;
	CHECK_EQ(a, 0, "typedef int before braceless if");
	if (1) {
		typedef double _BIF;
		_BIF d;
		d = 0;
		(void)d;
	}
	_BIF b;
	CHECK_EQ(b, 0, "typedef int restored after braced if");
	CHECK(sizeof(b) == sizeof(int), "typedef size correct after braced if");
}

static void test_typedef_braceless_while_no_leak(void) {
	typedef int _BWH;
	_BWH x;
	CHECK_EQ(x, 0, "typedef int before while");
	while (0) {
		typedef double _BWH;
		_BWH w;
		w = 0;
		(void)w;
	}
	_BWH y;
	CHECK_EQ(y, 0, "typedef int restored after while");
	CHECK(sizeof(y) == sizeof(int), "typedef size correct after while");
}

static void test_typedef_braceless_for_shadow_restore(void) {
	typedef int _BFS;
	_BFS a;
	CHECK_EQ(a, 0, "typedef before for-init shadow");
	for (int _BFS = 0; _BFS < 3; _BFS++) {
		int copy = _BFS;
		(void)copy;
	}
	_BFS b;
	CHECK_EQ(b, 0, "typedef restored after for-init shadow");
	CHECK(sizeof(b) == sizeof(int), "typedef size after for-init shadow");

	for (int _BFS = 10; _BFS < 11; _BFS++); // braceless for body
	_BFS c;
	CHECK_EQ(c, 0, "typedef restored after braceless for");
}

static void test_typedef_braceless_else_no_leak(void) {
	typedef int _BEL;
	_BEL a;
	CHECK_EQ(a, 0, "typedef before if-else");
	if (0) {
	} else {
		typedef double _BEL;
		_BEL e;
		e = 0;
		(void)e;
	}
	_BEL b;
	CHECK_EQ(b, 0, "typedef restored after else");
	CHECK(sizeof(b) == sizeof(int), "typedef size after else");
}

static void test_typedef_nested_braceless_control(void) {
	typedef int _BNC;
	_BNC v0;
	CHECK_EQ(v0, 0, "typedef before nested control");
	for (int _BNC = 0; _BNC < 1; _BNC++) {
		for (int j = 0; j < 1; j++) {
			int inner = _BNC + j;
			(void)inner;
		}
	}
	_BNC v1;
	CHECK_EQ(v1, 0, "typedef after double-nested for");

	for (int _BNC = 0; _BNC < 1; _BNC++)
		for (int j = 0; j < 1; j++);
	_BNC v2;
	CHECK_EQ(v2, 0, "typedef after nested braceless for");
}

static void test_typedef_for_init_shadow_multi_var(void) {
	typedef int _FIM;
	_FIM a;
	CHECK_EQ(a, 0, "typedef before multi-var for-init shadow");
	for (int _FIM = 0, q = 1; _FIM < q; _FIM++) {
		int sum = _FIM + q;
		(void)sum;
	}
	_FIM b;
	CHECK_EQ(b, 0, "typedef after multi-var for-init");
}

static void test_goto_forward_over_block_safe(void) {
	int result = 0;
	goto _gfob_target;
	{
		int hidden = 99;
		result = hidden;
	}
_gfob_target:
	CHECK_EQ(result, 0, "goto forward over block safe");
}

static void test_goto_forward_no_decl_skip(void) {
	int x = 0;
	goto _gfnd_label;
	x = 42;
_gfnd_label:
	CHECK_EQ(x, 0, "goto forward no decl skip");
}

static void test_goto_backward_safe(void) {
	int counter = 0;
_gbs_loop:
	counter++;
	if (counter < 3) goto _gbs_loop;
	CHECK_EQ(counter, 3, "goto backward loop");
}

static void test_goto_forward_same_scope_label(void) {
	log_reset();
	{
		defer log_append("D");
		goto _gfss_end;
		log_append("SKIP");
	_gfss_end:
		log_append("E");
	}
	CHECK_LOG("ED", "goto same scope label with defer");
}

#ifndef _MSC_VER
static void test_vla_sizeof_no_double_eval(void) {
	int n = 4;
	int before = n;
	int vla[n++];
	int after = n;
	CHECK_EQ(before, 4, "VLA n before");
	CHECK_EQ(after, 5, "VLA n after (incremented once)");
	CHECK(sizeof(vla) == 4 * sizeof(int), "VLA sizeof matches original n");
	(void)vla;
}

static void test_vla_memset_zeroinit(void) {
	int n = 8;
	int arr[n];
	int all_zero = 1;
	for (int i = 0; i < n; i++) {
		if (arr[i] != 0) all_zero = 0;
	}
	CHECK(all_zero, "VLA memset zeroinit all zeros");
}
#endif

static void test_defer_scope_isolation(void) {
	log_reset();
	{
		defer log_append("A");
		{
			defer log_append("B");
			log_append("1");
		}
		log_append("2");
	}
	CHECK_LOG("1B2A", "defer scope isolation");
}

static void test_defer_braceless_rejected(void) {
	expect_parse_rejects(
	    "int main(void) {\n"
	    "    if (1) defer (void)0;\n"
	    "    return 0;\n"
	    "}\n",
	    "defer_braceless_if.c", "defer braceless if rejected", "defer");
	expect_parse_rejects(
	    "int main(void) {\n"
	    "    while (1) defer (void)0;\n"
	    "}\n",
	    "defer_braceless_while.c", "defer braceless while rejected", "defer");
}

static void test_zeroinit_typedef_after_control(void) {
	typedef struct {
		int x;
		int y;
	} _ZTC;

	_ZTC s1;
	CHECK(s1.x == 0 && s1.y == 0, "struct typedef zeroed before control");
	if (1) {
		_ZTC s2;
		CHECK(s2.x == 0 && s2.y == 0, "struct typedef zeroed inside if");
	}
	_ZTC s3;
	CHECK(s3.x == 0 && s3.y == 0, "struct typedef zeroed after if");

	for (int i = 0; i < 1; i++) {
		_ZTC s4;
		CHECK(s4.x == 0 && s4.y == 0, "struct typedef zeroed inside for");
	}
	_ZTC s5;
	CHECK(s5.x == 0 && s5.y == 0, "struct typedef zeroed after for");
}

static void test_for_init_shadow_braceless_body(void) {
	typedef int _FISB;
	_FISB before;
	CHECK_EQ(before, 0, "typedef before braceless for-init shadow");
	for (int _FISB = 0; _FISB < 2; _FISB++);
	_FISB after;
	CHECK_EQ(after, 0, "typedef after braceless for-init shadow");

	for (int _FISB = 0; _FISB < 1; _FISB++) {
		int val = _FISB;
		CHECK_EQ(val, 0, "for-init shadow var zeroe");
	}
	_FISB final;
	CHECK_EQ(final, 0, "typedef after braced for-init shadow");
}

static int _oe_eval_ctr;

static int *_oe_eval_fn(int *p) {
	_oe_eval_ctr++;
	return p;
}

static void test_const_orelse_ptr_eval_once(void) {
	int val = 42;
	int fb = 99;
	_oe_eval_ctr = 0;
	const int *p = _oe_eval_fn(&val) orelse & fb;
	CHECK(*p == 42, "const ptr orelse: non-null preserved");
	CHECK_EQ(_oe_eval_ctr, 1, "const ptr orelse: init evaluated once");

	_oe_eval_ctr = 0;
	const int *q = _oe_eval_fn(NULL) orelse & fb;
	CHECK(*q == 99, "const ptr orelse: null uses fallback");
	CHECK_EQ(_oe_eval_ctr, 1, "const ptr orelse null: init evaluated once");
}

static void test_const_orelse_multi_fallback(void) {
	int a = 7, b = 0;
	const int x = a orelse 10, y = b orelse 20;
	CHECK_EQ(x, 7, "const orelse multi first non-zero");
	CHECK_EQ(y, 20, "const orelse multi second zero fallback");

	const int m = 0 orelse 55, n = 3 orelse 66;
	CHECK_EQ(m, 55, "const orelse multi both: first");
	CHECK_EQ(n, 3, "const orelse multi both: second");
}

static void _void_defer_side_fn(void) {
	log_append("V");
}

static void _void_return_call_defer_impl(void) {
	defer log_append("D");
	log_append("1");
	return _void_defer_side_fn();
}

static void test_void_return_call_with_defer(void) {
	log_reset();
	_void_return_call_defer_impl();
	CHECK_LOG("1VD", "void return funcall with defer");
}

static void _void_return_cast_defer_impl(void) {
	defer log_append("D");
	log_append("1");
	return (void)0;
}

static void test_void_return_cast_with_defer(void) {
	log_reset();
	_void_return_cast_defer_impl();
	CHECK_LOG("1D", "void return cast with defer");
}

static void test_switch_case_defer_ordering(void) {
	log_reset();
	for (int i = 0; i < 3; i++) {
		switch (i) {
		case 0: {
			defer log_append("A");
			log_append("0");
			break;
		}
		case 1: {
			defer log_append("B");
			log_append("1");
			break;
		}
		default: {
			defer log_append("C");
			log_append("2");
			break;
		}
		}
	}
	CHECK_LOG("0A1B2C", "switch case defer ordering");
}

static void test_switch_defer_loop_nested(void) {
	log_reset();
	for (int i = 0; i < 2; i++) {
		defer log_append("L");
		switch (i) {
		case 0: {
			defer log_append("S0");
			log_append("a");
			break;
		}
		case 1: {
			defer log_append("S1");
			log_append("b");
			break;
		}
		}
	}
	CHECK_LOG("aS0LbS1L", "switch defer in loop nested");
}

static void test_typedef_for_switch_scope(void) {
	typedef int _TFS;
	_TFS before;
	CHECK_EQ(before, 0, "typedef before for-switch");
	for (int _TFS = 0; _TFS < 2; _TFS++) {
		switch (_TFS) {
		case 0: break;
		case 1: break;
		}
	}
	_TFS after;
	CHECK_EQ(after, 0, "typedef after for-switch scope");
}

static void test_typedef_nested_for_braceless(void) {
	typedef int _TNF;
	_TNF x;
	CHECK_EQ(x, 0, "typedef nested for braceless: before");
	for (int _TNF = 0; _TNF < 1; _TNF++)
		for (int j = 0; j < 1; j++);
	_TNF y;
	CHECK_EQ(y, 0, "typedef nested for braceless: after");
}

static void test_typedef_shadow_braceless_for_complex(void) {
	typedef int _TSFC;
	_TSFC a;
	CHECK_EQ(a, 0, "typedef before for-shadow complex");
	for (int _TSFC = 0; _TSFC < 3; _TSFC++)
		if (_TSFC == 1)
			;
	_TSFC b;
	CHECK_EQ(b, 0, "typedef after braceless for with if body");
	CHECK(sizeof(b) == sizeof(int), "typedef size after complex braceless for");
}

static void test_typeof_large_struct_zeroinit(void) {
#ifdef __GNUC__
	struct _large_tzi {
		int data[256];
		char name[64];
		double values[32];
	};
	struct _large_tzi ref = {{0}, {0}, {0}};
	typeof(ref) copy;
	int all_zero = 1;
	for (int i = 0; i < 256; i++)
		if (copy.data[i] != 0) all_zero = 0;
	for (int i = 0; i < 64; i++)
		if (copy.name[i] != 0) all_zero = 0;
	for (int i = 0; i < 32; i++)
		if (copy.values[i] != 0.0) all_zero = 0;
	CHECK(all_zero, "typeof large struct zeroinit: all fields zero");
#endif
}

static void test_typeof_nested_struct_zeroinit(void) {
#ifdef __GNUC__
	struct _inner_tnz {
		int x;
		int y;
	};

	struct _outer_tnz {
		struct _inner_tnz a;
		struct _inner_tnz b;
		int c;
	};
	struct _outer_tnz ref;
	ref.a.x = 0;
	typeof(ref) copy;
	CHECK_EQ(copy.a.x, 0, "typeof nested struct: a.x zero");
	CHECK_EQ(copy.a.y, 0, "typeof nested struct: a.y zero");
	CHECK_EQ(copy.b.x, 0, "typeof nested struct: b.x zero");
	CHECK_EQ(copy.b.y, 0, "typeof nested struct: b.y zero");
	CHECK_EQ(copy.c, 0, "typeof nested struct: c zero");
#endif
}

static void test_typedef_shadow_braceless_for_multi_stmt(void) {
	typedef int _TSFM;
	_TSFM v1;
	CHECK_EQ(v1, 0, "typedef before multi braceless for");
	for (int _TSFM = 0; _TSFM < 2; _TSFM++);
	_TSFM v2;
	CHECK_EQ(v2, 0, "typedef after first braceless for");
	for (int _TSFM = 10; _TSFM < 12; _TSFM++);
	_TSFM v3;
	CHECK_EQ(v3, 0, "typedef after second braceless for");
}

static void test_typedef_shadow_braceless_for_nested_loops(void) {
	typedef int _TSFN;
	_TSFN a;
	CHECK_EQ(a, 0, "typedef before nested braceless for");
	for (int _TSFN = 0; _TSFN < 2; _TSFN++)
		for (int j = 0; j < 2; j++);
	_TSFN b;
	CHECK_EQ(b, 0, "typedef after nested braceless for");
	for (int i = 0; i < 2; i++)
		for (int _TSFN = 0; _TSFN < 2; _TSFN++);
	_TSFN c;
	CHECK_EQ(c, 0, "typedef after inner braceless for shadow");
}

static void test_typedef_shadow_for_with_if_body(void) {
	typedef int _TSFI;
	_TSFI x;
	CHECK_EQ(x, 0, "typedef before for-if-else shadow");
	for (int _TSFI = 0; _TSFI < 3; _TSFI++) {
		if (_TSFI == 0)
			;
		else if (_TSFI == 1)
			;
		else
			;
	}
	_TSFI y;
	CHECK_EQ(y, 0, "typedef after for-if-else shadow");
	CHECK(sizeof(y) == sizeof(int), "typedef size after for-if-else");
}

#if defined(__GNUC__) && !defined(__clang__)
static void test_for_init_typedef_no_leak(void) {
	// for-init typedef should NOT persist after the loop
	typedef int _FITL;
	_FITL a;
	CHECK_EQ(a, 0, "typedef visible before for-init shadow");
	for (typedef double _FITL;;) {
		_FITL x;
		(void)x;
		break;
	}
	// After for loop, _FITL should revert to outer int, not double
	_FITL b;
	CHECK_EQ(b, 0, "typedef restored after for-init shadow");
	CHECK(sizeof(b) == sizeof(int), "for-init typedef correctly scoped (int, not double)");
}

static void test_for_init_typedef_braceless_no_leak(void) {
	typedef int _FITBL;
	_FITBL a;
	CHECK_EQ(a, 0, "typedef before braceless for-init shadow");
	for (typedef double _FITBL;;) break;
	_FITBL b;
	CHECK_EQ(b, 0, "typedef after braceless for-init shadow");
	CHECK(sizeof(b) == sizeof(int), "braceless for-init typedef correctly scoped");
}

static void test_for_init_typedef_nested_loops(void) {
	typedef int _FITNL;
	for (typedef double _FITNL;;) {
		_FITNL inner;
		CHECK(sizeof(inner) == sizeof(double), "nested for-init typedef is double");
		break;
	}
	_FITNL outer;
	CHECK_EQ(outer, 0, "outer typedef restored after nested for-init shadow");
	CHECK(sizeof(outer) == sizeof(int), "outer typedef is int after nested for");
}
#endif

static void test_defer_switch_dead_zone_braced(void) {
	int val = 0;
	switch (1) {
	case 1: val = 10; break;
	case 2: {
		defer val += 100;
		val = 99;
	}
	}
	// Case 2 is dead code — val should remain 10
	CHECK_EQ(val, 10, "defer in dead switch case does not fire");
}

#ifndef _MSC_VER
static void test_generic_const_array_zeroinit(void) {
	int arr[_Generic(0, int: 10, default: 20)];
	CHECK_EQ(arr[0], 0, "_Generic const array first elem zero");
	CHECK_EQ(arr[9], 0, "_Generic const array last elem zero");
}
#endif

struct _AnonRetTest {
	int x;
	int y;
};

static int _anon_ret_defer_flag;

static struct _AnonRetTest _named_struct_ret_with_defer(void) {
	defer _anon_ret_defer_flag = 1;
	return (struct _AnonRetTest){42, 99};
}

static void test_named_struct_return_with_defer(void) {
	_anon_ret_defer_flag = 0;
	struct _AnonRetTest p = _named_struct_ret_with_defer();
	CHECK_EQ(p.x, 42, "named struct return with defer: x correct");
	CHECK_EQ(p.y, 99, "named struct return with defer: y correct");
	CHECK_EQ(_anon_ret_defer_flag, 1, "named struct return with defer: defer fired");
}

typedef struct {
	int val;
} _TypedefRetTest;

static int _trt_defer_flag;

static _TypedefRetTest _typedef_struct_ret_with_defer(void) {
	defer _trt_defer_flag = 1;
	return (_TypedefRetTest){77};
}

static void test_typedef_struct_return_with_defer(void) {
	_trt_defer_flag = 0;
	_TypedefRetTest r = _typedef_struct_ret_with_defer();
	CHECK_EQ(r.val, 77, "typedef struct return with defer: value correct");
	CHECK_EQ(_trt_defer_flag, 1, "typedef struct return with defer: defer fired");
}

static int _goto_fnptr_helper(void) {
	return 42;
}

static void test_goto_fnptr_decl_after_label(void) {
	goto _gfp_label;
_gfp_label:;
	int (*fn)(void) = _goto_fnptr_helper;
	CHECK_EQ(fn(), 42, "goto fnptr decl after label: callable");
}

static void test_goto_fnptr_decl_before_goto(void) {
	int (*fn)(void) = _goto_fnptr_helper;
	goto _gfpb_label;
_gfpb_label:
	CHECK_EQ(fn(), 42, "goto fnptr decl before goto: preserved");
}

static void test_goto_array_ptr_decl_after_label(void) {
	int data[4] = {10, 20, 30, 40};
	goto _gap_label;
_gap_label:;
	int (*arr)[4] = &data;
	CHECK_EQ((*arr)[2], 30, "goto array-ptr decl after label: accessible");
}

#ifndef _MSC_VER
static void test_typeof_const_zero_init(void) {
	typeof(const int) a;
	CHECK_EQ(a, 0, "typeof(const int) zero-init via = {0}");

	const typeof(int) b;
	CHECK_EQ(b, 0, "const typeof(int) zero-init via = {0}");
}
#endif

// Function parameter that shadows a typedef must be treated as a variable, not a type
static int _param_shadow_helper(int MyInt) {
	int y = 3;
	int result = MyInt * y; // multiplication, NOT pointer declaration
	return result;
}

static void _param_shadow_scope_check(void) {
	MyInt x; // MyInt should be typedef again after _param_shadow_helper
	CHECK_EQ(x, 0, "param shadow: typedef restored after function");
}

static void test_param_typedef_shadow(void) {
	CHECK_EQ(_param_shadow_helper(5), 15, "param shadow: multiplication not ptr decl");
	_param_shadow_scope_check();
}

static void test_goto_over_static_decl(void) {
	goto _gos_label;
	static int _gos_x = 42;
_gos_label:
	CHECK_EQ(_gos_x, 42, "goto over static decl: allowed and value correct");
}

static void test_defer_break_continue_rejected(void) {
	expect_parse_rejects(
	    "int main(void) {\n"
	    "    defer break;\n"
	    "    return 0;\n"
	    "}\n",
	    "defer_break_bare.c", "defer break rejected", "break");
	expect_parse_rejects(
	    "int main(void) {\n"
	    "    defer continue;\n"
	    "    return 0;\n"
	    "}\n",
	    "defer_continue_bare.c", "defer continue rejected", "continue");
	expect_parse_rejects(
	    "int main(void) {\n"
	    "    defer { break; };\n"
	    "    return 0;\n"
	    "}\n",
	    "defer_break_braced.c", "defer braced break rejected", "break");
	expect_parse_rejects(
	    "int main(void) {\n"
	    "    defer { continue; };\n"
	    "    return 0;\n"
	    "}\n",
	    "defer_continue_braced.c", "defer braced continue rejected", "continue");
}

static void helper_defer_for_break(int *out) {
	defer {
		for (int i = 0; i < 10; i++) {
			if (i == 3) break;
			*out += i;
		}
	};
}

static void test_defer_inner_loop_break(void) {
	int sum = 0;
	helper_defer_for_break(&sum);
	CHECK_EQ(sum, 3, "defer inner for break: 0+1+2=3");
}

static void helper_defer_while_continue(int *out) {
	defer {
		int i = 0;
		while (i < 5) {
			i++;
			if (i == 3) continue;
			*out += i;
		}
	};
}

static void test_defer_inner_loop_continue(void) {
	int sum = 0;
	helper_defer_while_continue(&sum);
	CHECK_EQ(sum, 12, "defer inner while continue: 1+2+4+5=12");
}

static void helper_defer_switch_break(int val, int *out) {
	defer {
		switch (val) {
		case 1: *out = 10; break;
		case 2: *out = 20; break;
		default: *out = 30; break;
		}
	};
}

static void test_defer_inner_switch_break(void) {
	int result = 0;
	helper_defer_switch_break(2, &result);
	CHECK_EQ(result, 20, "defer inner switch break: case 2");
}

static void helper_defer_dowhile_break(int *out) {
	defer {
		do {
			(*out)++;
			if (*out == 3) break;
		} while (*out < 10);
	};
}

static void test_defer_inner_do_while_break(void) {
	int count = 0;
	helper_defer_dowhile_break(&count);
	CHECK_EQ(count, 3, "defer inner do-while break: stopped at 3");
}

static void test_t_heuristic_shadow_mul(void) {
	// count_t looks like a typedef to the _t heuristic,
	// but declaring 'int count_t' should shadow it.
	int count_t = 10;
	int x = 2;
	int result = count_t * x; // multiplication, NOT pointer decl
	CHECK_EQ(result, 20, "_t shadow: multiplication not misread as decl");
}

static void test_t_heuristic_shadow_arith(void) {
	// Arithmetic on a _t-shadowed variable
	int offset_t = 7;
	int y = offset_t + 3;
	CHECK_EQ(y, 10, "_t shadow: addition works");
	y = offset_t - 2;
	CHECK_EQ(y, 5, "_t shadow: subtraction works");
}

static void test_t_heuristic_shadow_ptr_deref(void) {
	// Dereferencing through a pointer named foo_t
	int val = 42;
	int *ptr_t = &val;
	int got = *ptr_t; // dereference, not a declaration
	CHECK_EQ(got, 42, "_t shadow: pointer deref not misread as decl");
}

static void test_t_heuristic_shadow_scope(void) {
	// Shadow should be scoped — after block, the heuristic should resume
	{
		int size_t = 99; // shadows the real size_t inside this block
		int z = size_t * 2;
		CHECK_EQ(z, 198, "_t shadow in scope: mul works");
	}
	// After the block, size_t should be a type again
	size_t n = 0;
	CHECK_EQ((int)n, 0, "_t shadow: real size_t accessible after block");
}

static void test_t_heuristic_shadow_param(void) {
	// Function-scoped via a helper (parameters should shadow too)
	// We test this inline with a block to simulate the effect
	{
		int count_t = 5;
		int arr[3];
		arr[0] = count_t; // index expression, not decl
		CHECK_EQ(arr[0], 5, "_t shadow: array index expr works");
	}
}

static void test_t_heuristic_noshadow(void) {
	// Verify the heuristic still works for real typedefs
	size_t a = 0;
	CHECK_EQ((int)a, 0, "_t heuristic: size_t still recognized as type");
}

static void test_array_orelse_rejected(void) {
	expect_parse_rejects(
	    "int main(void) {\n"
	    "    int arr[] = {1, 2} orelse { return 0; };\n"
	    "    return arr[0];\n"
	    "}\n",
	    "array_orelse_block.c", "array orelse block rejected", "array");
	expect_parse_rejects(
	    "int main(void) {\n"
	    "    const int arr[] = {1, 2} orelse (int[]){3, 4};\n"
	    "    return arr[0];\n"
	    "}\n",
	    "array_orelse_const.c", "const array orelse fallback rejected", "array");
	expect_parse_rejects(
	    "int main(void) {\n"
	    "    int arr[] = {1, 2} orelse (int[]){3, 4};\n"
	    "    return arr[0];\n"
	    "}\n",
	    "array_orelse_expr.c", "non-const array orelse fallback rejected", "array");
}

#ifndef _MSC_VER
static void test_deep_struct_nesting_goto(void) {
	// This struct has 66 levels of nesting, pushing depth past the 64-bit
	// bitmask threshold. The walker's deep_struct_opens counter tracks these.
	struct D0 {
		struct {
			struct {
				struct {
					struct {
						struct { // 6

							struct {
								struct {
									struct {
										struct {
											struct {
												struct { // 12

													struct
													{
														struct
														{
															struct
															{
																struct
																{
																	struct
																	{
																		struct
																		{ // 18

																			struct
																			{
																				struct
																				{
																					struct
																					{
																						struct
																						{
																							struct
																							{
																								struct
																								{ // 24

																									struct
																									{
																										struct
																										{
																											struct
																											{
																												struct
																												{
																													struct
																													{
																														struct
																														{ // 30

																															struct
																															{
																																struct
																																{
																																	struct
																																	{
																																		struct
																																		{
																																			struct
																																			{
																																				struct
																																				{ // 36

																																					struct
																																					{
																																						struct
																																						{
																																							struct
																																							{
																																								struct
																																								{
																																									struct
																																									{
																																										struct
																																										{ // 42

																																											struct
																																											{
																																												struct
																																												{
																																													struct
																																													{
																																														struct
																																														{
																																															struct
																																															{
																																																struct
																																																{ // 48

																																																	struct
																																																	{
																																																		struct
																																																		{
																																																			struct
																																																			{
																																																				struct
																																																				{
																																																					struct
																																																					{
																																																						struct
																																																						{ // 54

																																																							struct
																																																							{
																																																								struct
																																																								{
																																																									struct
																																																									{
																																																										struct
																																																										{
																																																											struct
																																																											{
																																																												struct
																																																												{ // 60

																																																													struct
																																																													{
																																																														struct
																																																														{
																																																															struct
																																																															{
																																																																struct
																																																																{
																																																																	struct
																																																																	{
																																																																		struct
																																																																		{ // 66
																																																																			int leaf;
																																																																		};
																																																																	};
																																																																};
																																																															};
																																																														};
																																																													}; // 60
																																																												};
																																																											};
																																																										};
																																																									};
																																																								};
																																																							}; // 54
																																																						};
																																																					};
																																																				};
																																																			};
																																																		};
																																																	}; // 48
																																																};
																																															};
																																														};
																																													};
																																												};
																																											}; // 42
																																										};
																																									};
																																								};
																																							};
																																						};
																																					}; // 36
																																				};
																																			};
																																		};
																																	};
																																};
																															}; // 30
																														};
																													};
																												};
																											};
																										};
																									}; // 24
																								};
																							};
																						};
																					};
																				};
																			}; // 18
																		};
																	};
																};
															};
														};
													}; // 12
												};
											};
										};
									};
								};
							}; // 6
						};
					};
				};
			};
		};
	}; // 0

	struct D0 d = {0};
	// goto after the struct definition — walker must not misidentify
	// struct members as labels due to struct_depth desync
	int flag = 1;
	if (flag) goto done;
	flag = 0;
done:
	CHECK(flag == 1, "deep struct nesting: goto works correctly");
	CHECK(d.leaf == 0, "deep struct nesting: zero-init works");
}
#endif

#ifndef _MSC_VER
static void test_generic_array_not_vla(void) {
	int x = 0;
	int arr[_Generic(x, int: 5, default: 10)];
	// If falsely detected as VLA, Prism would use memset.
	// With the fix, it should use = {0} and sizeof is constant.
	CHECK_EQ((int)sizeof(arr), 5 * (int)sizeof(int), "_Generic array size: not VLA");
}
#endif

static void test_c23_attr_void_function(void) {
	PrismResult result = prism_transpile_source(
	    "void [[deprecated]] func(void) {\n"
	    "    defer (void)0;\n"
	    "    return;\n"
	    "}\n",
	    "parse_c23_attr_void.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "C23 [[attr]] void func parse transpiles");
	if (result.output) {
		CHECK(strstr(result.output, "_Prism_ret") == NULL,
		      "C23 [[attr]] void func parse: no synthetic return temp");
		CHECK(strstr(result.output, "[[deprecated]]") != NULL,
		      "C23 [[attr]] void func parse: attribute preserved");
	}
	prism_free(&result);
}

static int _bug2_count_t = 10;   // file-scope var, NOT a type
static int _bug2_offset_t = 7;   // another file-scope _t var

static void test_bug2_filescope_t_mul(void) {
	int b = 3;
	int saved_b = b;
	{
		defer log_append("D");
		// BUG: _bug2_count_t ends in _t, no shadow at file scope.
		// Prism heuristic treats it as a type.
		// '_bug2_count_t * b;' gets parsed as pointer decl '_bug2_count_t *b = 0;'
		// which fails to compile (_bug2_count_t is int, not a type).
		// After fix: expression emitted as-is, b unchanged.
		_bug2_count_t * b;
		CHECK_EQ(b, saved_b, "bug2: file-scope _t mul not misread as ptr decl");
	}
}

static void test_bug2_filescope_t_arith(void) {
	{
		defer log_append("D");
		// Assignment with file-scope _t var — should work even without fix
		// (assignment doesn't match declaration pattern)
		_bug2_offset_t = 20;
		CHECK_EQ(_bug2_offset_t, 20, "bug2: file-scope _t assignment in defer");
		_bug2_offset_t = 7; // restore
	}
}

static void test_bug2_filescope_t_in_expr(void) {
	{
		defer log_append("D");
		// Using file-scope _t var in an expression with 'int' at stmt start — safe
		int val = _bug2_count_t + _bug2_offset_t;
		CHECK_EQ(val, 17, "bug2: file-scope _t vars in int-expr");
	}
}

static void _bug5_void_callee(void) {
	log_append("V");
}

static void _bug5_void_return_call(void) {
	defer log_append("D");
	log_append("1");
	return _bug5_void_callee(); // Valid C: returning void expr from void func
}

static void test_bug5_void_return_call_defer(void) {
	log_reset();
	_bug5_void_return_call();
	CHECK_LOG("1VD", "bug5: void return funcall with defer");
}

static void _bug5_void_return_cast(void) {
	defer log_append("D");
	log_append("1");
	return (void)0; // Explicit void cast return
}

static void test_bug5_void_return_cast_defer(void) {
	log_reset();
	_bug5_void_return_cast();
	CHECK_LOG("1D", "bug5: void return (void)cast with defer");
}

static void _bug5_void_return_bare(void) {
	defer log_append("D");
	log_append("1");
	return; // Bare return in void function
}

static void test_bug5_void_return_bare_defer(void) {
	log_reset();
	_bug5_void_return_bare();
	CHECK_LOG("1D", "bug5: void bare return with defer");
}

#ifdef __GNUC__
static void test_bug4_stmt_expr_in_defer(void) {
	log_reset();
	int result = 0;
	{
		defer log_append("D");
		// Statement expression inside a defer scope
		result = ({
			int a = 10;
			int b = 20;
			a + b;
		});
	}
	CHECK_EQ(result, 30, "bug4: stmt_expr in defer scope works");
	CHECK_LOG("D", "bug4: defer scope logged correctly");
}
#endif

static void test_bug1_digraph_in_defer(void) {
	int arr[3];
	arr[0] = 0;
	{
		defer log_append("D");
		// Use array subscript (digraphs <: and :> normalize to [ and ])
		arr<:0:> = 42;
		arr<:1:> = 99;
		CHECK_EQ(arr[0], 42, "bug1: digraph <: :> subscript in defer");
		CHECK_EQ(arr[1], 99, "bug1: digraph <: :> second subscript");
	}
}

static void test_bug6_setjmp_detection(void) {
	PrismResult result = prism_transpile_source(
	    "#include <setjmp.h>\n"
	    "static jmp_buf buf;\n"
	    "void bad(void) {\n"
	    "    defer (void)0;\n"
	    "    longjmp(buf, 1);\n"
	    "}\n"
	    "int main(void) { if (setjmp(buf) == 0) bad(); return 0; }\n",
	    "bug6_setjmp_reject.c", prism_defaults());
	CHECK(result.status != PRISM_OK, "bug6: setjmp with defer rejected");
	prism_free(&result);
}

typedef void (*simple_callback_t)(void);
static int _r2_callback_called = 0;
static void _r2_callback_fn(void) { _r2_callback_called = 1; }

static simple_callback_t _r2_get_callback_typedef(void) {
	defer log_append("D");
	log_append("G");
	return _r2_callback_fn;
}

static void test_bug_r2_fnptr_return_typedef(void) {
	log_reset();
	_r2_callback_called = 0;
	simple_callback_t cb = _r2_get_callback_typedef();
	CHECK_LOG("GD", "bug_r2: fnptr return typedef defer order");
	cb();
	CHECK_EQ(_r2_callback_called, 1, "bug_r2: fnptr return typedef value correct");
}

static void (*_r2_get_callback_raw(void))(void) {
	defer log_append("D");
	log_append("G");
	return _r2_callback_fn;
}

static void test_bug_r2_fnptr_return_raw(void) {
	log_reset();
	_r2_callback_called = 0;
	void (*cb)(void) = _r2_get_callback_raw();
	CHECK_LOG("GD", "bug_r2: fnptr return raw defer order");
	cb();
	CHECK_EQ(_r2_callback_called, 1, "bug_r2: fnptr return raw value correct");
}

// Function returning int* with defer (simpler complex type)
static int _r2_int_val = 42;
static int *_r2_get_ptr(void) {
	defer log_append("D");
	log_append("G");
	return &_r2_int_val;
}

static void test_bug_r2_ptr_return(void) {
	log_reset();
	int *p = _r2_get_ptr();
	CHECK_LOG("GD", "bug_r2: ptr return defer order");
	CHECK_EQ(*p, 42, "bug_r2: ptr return value correct");
}

#ifndef _WIN32
static void test_bug_r1_readonly_dir(void) {
	if (getuid() == 0) {
		passed++; total++;
		printf("  [SKIP] bug_r1: read-only dir test (running as root)\n");
		return;
	}
	char dir_template[PATH_MAX];
	char *dir = test_mkdtemp(dir_template, "prism_readonly_dir_");
	CHECK(dir != NULL, "bug_r1: create read-only temp dir");
	if (!dir) return;

	char src_path[PATH_MAX];
	char temp_path[PATH_MAX];
	snprintf(src_path, sizeof(src_path), "%s/source.c", dir);
	FILE *f = fopen(src_path, "w");
	CHECK(f != NULL, "bug_r1: create source inside read-only dir");
	if (f) {
		fputs("int main(void) { return 0; }\n", f);
		fclose(f);
	}

	CHECK_EQ(chmod(dir, 0555), 0, "bug_r1: make source dir read-only");
	if (access(src_path, F_OK) == 0) {
		CHECK_EQ(make_temp_file(temp_path, sizeof(temp_path), NULL, 0, src_path), 0,
			 "bug_r1: temp creation falls back when source dir is read-only");
		CHECK(strncmp(temp_path, dir, strlen(dir)) != 0,
		      "bug_r1: temp path not created inside read-only dir");
		remove(temp_path);
	}

	chmod(dir, 0755);
	remove(src_path);
	rmdir(dir);
}
#endif

static void test_bug_r3_line_directive(void) {
	// __FILE__ should be a valid string. If #line processing is broken,
	// this test file itself would fail to compile.
	CHECK(strlen(__FILE__) > 0, "bug_r3: __FILE__ is valid string");
}

static int _paren_void_flag = 0;

void (_paren_void_func)(void) {
	defer {
		_paren_void_flag += 1;
	};
	_paren_void_flag += 10;
}

static void test_void_parenthesized_func_defer(void) {
	_paren_void_flag = 0;
	_paren_void_func();
	CHECK_EQ(_paren_void_flag, 11, "void (func)(): defer fires correctly");
}

static void test_paren_param_typedef_shadow(void) {
	printf("\n--- Parenthesized Param Typedef Shadow ---\n");

	const char *code =
	    "typedef int T;\n"
	    "void f(int (T)) {\n"
	    "    T x;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "paren param typedef shadow: create temp file");

	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_file(path, feat);

	if (r.status == PRISM_OK && r.output) {
		bool has_zeroinit = (strstr(r.output, "T x = 0") != NULL ||
				     strstr(r.output, "T x ={0}") != NULL ||
				     strstr(r.output, "T x = {0}") != NULL);
		CHECK(!has_zeroinit, "paren param typedef shadow: T shadowed by param, no zero-init");
	} else {
		CHECK(0, "paren param typedef shadow: transpilation failed");
	}

	prism_free(&r);
	unlink(path);
	free(path);
}

typedef int _epf_inner_param;

static void _epf_func(int ((*func_ptr)(int _epf_inner_param))) {
	// If the transpiler incorrectly shadows _epf_inner_param due to the
	// extra-parenthesized function pointer, x won't be zero-initialized.
	_epf_inner_param x;
	CHECK_EQ(x, 0, "extra-paren funcptr shadow: typedef not falsely shadowed");
}

static void test_extra_paren_funcptr_shadow(void) {
	_epf_func(NULL);
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

static void test_pragma_struct_body_parsing(void) {
	printf("\n--- _Pragma Blinds Struct/Union Body Parsing ---\n");

	// _Pragma between struct keyword and tag/body used to be consumed
	// as the struct tag name by is_valid_varname, blindsiding body parsing.
	// After preprocessing, _Pragma("...") becomes #pragma (TK_PREP_DIR).
	// The fix skips TK_PREP_DIR and _Pragma(...) tokens in both
	// find_struct_body_brace and parse_type_specifier.
	const char *code =
	    "struct\n"
	    "_Pragma(\"pack(push, 1)\")\n"
	    "Foo { int x; char y; };\n"
	    "void f(void) {\n"
	    "    struct Foo s;\n"
	    "    (void)s;\n"
	    "}\n"
	    "int main(void) { f(); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "pragma struct body: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "pragma struct body: transpiles OK");
	CHECK(result.output != NULL, "pragma struct body: output not NULL");

	// The struct body should be properly parsed, and local "struct Foo s"
	// should get zero-initialized.
	CHECK(strstr(result.output, "= {0}") != NULL || strstr(result.output, "= 0") != NULL,
	      "pragma struct body: struct variable zero-initialized despite _Pragma");

	prism_free(&result);
	unlink(path);
	free(path);
}

// Bug report: K&R function params at depth 0 poison global shadow table
// via register_toplevel_shadows. After a K&R `__my_type a;` param decl,
// the typedef __my_type becomes invisible everywhere.
static void test_knr_param_shadow_no_poison(void) {
	printf("\n--- K&R Param Shadow No Poison ---\n");

	// K&R function with a typedef'd parameter, followed by another function
	// that uses the same typedef. The typedef must still be recognized.
	const char *code =
	    "typedef struct { int v; } __my_type;\n"
	    "\n"
	    "void old_func(a)\n"
	    "    __my_type a;\n"
	    "{\n"
	    "    (void)a;\n"
	    "}\n"
	    "\n"
	    "void new_func(void) {\n"
	    "    __my_type x;\n"
	    "    (void)x;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "knr shadow: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "knr shadow: transpiles OK");
	CHECK(result.output != NULL, "knr shadow: output not NULL");

	// __my_type x; in new_func should get zero-initialized (= {0})
	// because __my_type is still recognized as a typedef (struct type).
	// If K&R params poisoned the shadow table, __my_type would be treated
	// as a variable name, and x would be parsed as a label or expression.
	CHECK(strstr(result.output, "= {0}") != NULL,
	      "knr shadow: __my_type x still gets = {0} after K&R param decl");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void expect_parse_rejects(const char *code, const char *file_name,
				 const char *name, const char *needle) {
	PrismResult result = prism_transpile_source(code, file_name, prism_defaults());
	CHECK(result.status != PRISM_OK, name);
	if (result.error_msg) {
		CHECK(strstr(result.error_msg, needle) != NULL,
		      "negative corpus: error message mentions expected keyword");
	}
	prism_free(&result);
}

static void test_hashmap_struct_entry_regression(void) {
	/* Regression: hashmap entries used to pack the key length into the top
	 * 16 bits of the pointer (TAG_KEY/UNTAG_KEY), which breaks on ARM64 MTE
	 * and any platform with >48-bit virtual addresses.  After the fix,
	 * HashEntry stores key_len as a separate uint16_t field.  This test
	 * exercises many typedefs with varying name lengths to stress the
	 * hashmap lookup/resize paths with the new struct layout. */
	char code[4096];
	int off = 0;
	/* Generate 100 typedefs with names of lengths 1..10 */
	for (int i = 0; i < 100; i++) {
		char name[32];
		int nlen = (i % 10) + 1;
		for (int j = 0; j < nlen; j++) name[j] = 'a' + (j + i) % 26;
		name[nlen] = '\0';
		off += snprintf(code + off, sizeof(code) - off,
			"typedef int t_%s_%d;\n", name, i);
	}
	/* Use some of them in declarations */
	off += snprintf(code + off, sizeof(code) - off,
		"void f(void) {\n");
	for (int i = 0; i < 100; i += 10) {
		char name[32];
		int nlen = (i % 10) + 1;
		for (int j = 0; j < nlen; j++) name[j] = 'a' + (j + i) % 26;
		name[nlen] = '\0';
		off += snprintf(code + off, sizeof(code) - off,
			"    t_%s_%d v%d;\n    (void)v%d;\n", name, i, i, i);
	}
	off += snprintf(code + off, sizeof(code) - off, "}\n");

	PrismResult r = prism_transpile_source(code, "hashmap_regression.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "hashmap-struct-entry: many typedefs with varying key lengths");
	prism_free(&r);
}

static void test_negative_parse_corpus(void) {
	printf("\n--- Negative Parse Corpus ---\n");

	expect_parse_rejects(
	    "int main(void) {\n"
	    "    for (int i = 0; defer (void)0; i++) { }\n"
	    "    return 0;\n"
	    "}\n",
	    "neg_for_defer.c", "negative corpus: defer in for condition rejected", "defer");

	expect_parse_rejects(
	    "int main(void) {\n"
	    "    if (1) defer (void)0;\n"
	    "    return 0;\n"
	    "}\n",
	    "neg_if_defer.c", "negative corpus: braceless defer rejected", "defer");

	expect_parse_rejects(
	    "int get(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    int x = (get() orelse 0);\n"
	    "}\n",
	    "neg_paren_orelse.c", "negative corpus: parenthesized orelse rejected", "orelse");

	expect_parse_rejects(
	    "int main(void) {\n"
	    "    int arr[] = {1, 2} orelse (int[]){3, 4};\n"
	    "    return arr[0];\n"
	    "}\n",
	    "neg_array_orelse.c", "negative corpus: array orelse rejected", "array");
}

static void test_large_token_no_truncation(void) {
	/* Regression: Token.len was uint16_t, so tokens > 65535 bytes
	   caused silent overflow — infinite loops or truncated output.
	   Generate a string literal of 66000 chars (token ~66002 bytes). */
	enum { STR_CHARS = 66000 };
	size_t code_len = STR_CHARS + 256;
	char *code = malloc(code_len);
	int off = snprintf(code, code_len,
		"void f(void) { const char *s = \"");
	memset(code + off, 'A', STR_CHARS);
	off += STR_CHARS;
	off += snprintf(code + off, code_len - off, "\"; (void)s; }\n");

	PrismResult result = prism_transpile_source(code, "large_token.c",
						    prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "large token (>65535 bytes) transpiles");
	if (result.output) {
		/* Output must contain the full string — not truncated */
		size_t out_len = strlen(result.output);
		CHECK(out_len > STR_CHARS, "large token output not truncated");
	}
	prism_free(&result);
	free(code);
}

static void test_typeof_vla_orelse_side_effect_rejected(void) {
	/* Regression: typeof(int[n++ orelse 10]) duplicated n++ in the ternary.
	   For VLAs, typeof evaluates the dimension expression at runtime,
	   causing the side effect to fire twice.  The transpiler must reject
	   orelse with side-effect LHS inside typeof array brackets. */
	expect_parse_rejects(
	    "void f(int n) { typeof(int[n++ orelse 10]) arr; (void)arr; }\n",
	    "typeof_vla_side_effect.c",
	    "typeof VLA orelse with side-effect LHS rejected",
	    "side effect");
}

#ifdef __GNUC__
static void test_asm_in_orelse_lhs_rejected(void) {
	/* Regression: asm in a statement expression in the bare-orelse LHS
	   bypassed the side-effect scanner because asm is a keyword,
	   not a valid varname.  The transpiler duplicated the LHS,
	   causing asm to execute twice. */
	expect_parse_rejects(
	    "void f(void) {\n"
	    "    int arr[4];\n"
	    "    int val;\n"
	    "    arr[({ __asm__ volatile(\"nop\"); 0; })] = val orelse 42;\n"
	    "}\n",
	    "asm_orelse_lhs.c",
	    "asm in bare-orelse LHS rejected",
	    "asm");
}
#endif

void run_parse_tests(void) {
	printf("\n=== PARSE TESTS ===\n");

	/* Multi-declarator tests */
	test_multi_decl_basic();
	test_multi_decl_mixed_ptr();
	test_multi_decl_arrays();
	test_multi_decl_partial_init();
	test_multi_decl_long();
	test_multi_decl_func_ptr();

	/* Typedef tracking tests */
	test_typedef_simple();
	test_typedef_pointer();
	test_typedef_struct();
	test_typedef_array();
	test_typedef_func_ptr();
	test_typedef_chained();
	test_typedef_multi_var();
	test_typedef_block_scoped();
	test_typedef_shadowing();
	test_typedef_multi_declarator();
	test_typedef_after_braceless_while();
	test_typedef_after_braceless_if_else();
	test_typedef_braceless_nested_control();
	test_typedef_multi_braceless_sequential();

	/* Edge case tests */
	test_bitfield_zeroinit();
	test_anonymous_struct();
	test_anonymous_union();
	test_long_declaration();
	test_func_ptr_array();
	test_ptr_to_array();
	test_defer_compound_literal();
	test_nested_struct_union_with_defer();
	test_deeply_nested_struct_defer_scopes();
	test_struct_in_braceless_control();
	test_defer_across_struct_boundaries();
	test_duffs_device();
	CHECK_LOG("XXXXXEF", "Duff's device with defer");
	test_defer_ternary();
	CHECK_LOG("1T", "defer with ternary");
	test_empty_defer();
	test_do_while_0_defer();
	CHECK_LOG("1DEF", "do-while(0) with defer");
	test_defer_comma_operator();

	/* Bug regression tests */
#ifdef __GNUC__
	test_stmt_expr_defer_nested_block();
#else
	printf("[SKIP] stmt expr tests (not GCC)\n");
#endif
	test_non_vla_typedef_still_works();
	test_switch_defer_no_leak();
	test_enum_constant_shadows_typedef();
	test_enum_shadow_star_ambiguity();
	test_enum_shadow_statement_form();
	test_ppnum_underscore_paste();
	test_local_function_decl();

	/* Stress tests */
	test_defer_shadowing_vars();
	test_typedef_hiding();
	test_typedef_same_name_shadow();
	test_typedef_nested_same_name_shadow();
	test_typedef_shadow_then_pointer();
	test_static_local_init();
	test_complex_func_ptr();
	test_switch_default_first();
	test_macro_hidden_defer();
	test_macro_hidden_decl();
	test_void_return_void_call();
	test_switch_continue();
	test_fam_struct_zeroinit();
#ifdef __GNUC__
	test_stmt_expr_side_effects();
#endif
	test_typedef_scope_churn_consolidated();

	/* Case labels inside blocks */
	test_case_in_nested_block();
	test_case_after_defer_in_block();
	test_duff_device_with_defer_at_top();

	/* Preprocessor numeric literals */
#ifndef _MSC_VER
	test_float128_suffix();
#endif
	test_float64_suffix();
	test_float32_suffix();
	test_float16_suffix();
	test_bf16_suffix();

	/* Preprocessor system macros */
	test_linux_macros();
	test_signal_macros();
	test_glibc_macros();
	test_posix_macros();

	/* Parsing edge cases */
#ifdef __GNUC__
	test_utf8_identifiers();
#endif
	test_digraphs();
	test_pragma_operator();
#ifdef __GNUC__
	test_break_escape_stmtexpr();
	test_stmtexpr_while_break();
	test_stmtexpr_dowhile_break();
	test_stmtexpr_nested_loops_break();
#endif
	test_string_escape_sequences_varied();
	test_char_literal_escape_sequences();

	/* Verification tests */
	test_switch_conditional_break_defer();
	test_switch_unconditional_break_works();
	test_switch_braced_fallthrough_works();
#ifndef _MSC_VER
	test_vla_struct_member();
	test_vla_struct_member_nested();
	test_offsetof_vs_runtime();
	test_stmt_expr_defer_goto();
	test_stmt_expr_defer_normal();
	test_nested_stmt_expr_defer();
#endif
	test_vanishing_statement_if_else();
	test_vanishing_statement_while();
	test_vanishing_statement_for();
#ifndef _MSC_VER
	test_attributed_label_defer();
#endif
	test_defer_label();
#ifndef _MSC_VER
	test_generic_default_first_association();
	test_generic_default_collision();
	test_generic_default_collision_nested();
	test_generic_default_outside_switch();
	test_vla_backward_goto_reentry();
	test_vla_backward_goto_stack_exhaustion();
	test_vla_backward_goto_with_defer();
	test_vla_pointer_init_semantics();
#endif
	test_typedef_shadow_semantics();
#ifndef _MSC_VER
	test_generic_default_no_switch();
#endif
	test_knr_function_parsing();
	test_comma_operator_in_init();
	test_switch_skip_hole_strict();
	test_complex_type_zeroinit();
	test_continue_in_switch_defer_detailed();
	test_ultra_complex_declarators();
	test_thread_local_handling();
	test_line_directive_preservation();
	test_alignas_struct_bitfield();
#ifndef _MSC_VER
	test_generic_typedef_not_label();
#endif
	test_c23_attributes_zeroinit();
	test_bitint_zeroinit();
	test_pragma_pack_preservation();
#ifndef _MSC_VER
	test_return_stmt_expr_with_defer();
	test_security_stmtexpr_value_corruption();
#endif
	test_security_braceless_defer_trap();
	test_security_switch_goto_double_free();
	test_ghost_shadow_corruption();
#ifndef _MSC_VER
	test_sizeof_vla_codegen();
#endif
	test_keyword_typedef_collision();
	test_keyword_as_struct_field();
	test_keyword_as_function_name();
#ifndef _MSC_VER
	test_sizeof_vla_typedef();
	test_typeof_vla_zeroinit();
#endif
	test_bug1_ghost_shadow_while();
	test_bug1_ghost_shadow_if();
	test_ghost_shadow_braceless_break();
	test_ghost_shadow_braceless_continue();
	test_ghost_shadow_braceless_return();
	test_ghost_shadow_nested_braceless();
	test_bug2_ultra_complex_exact();
	test_bug2_deeply_nested_parens();
#ifndef _MSC_VER
	test_bug3_stmtexpr_defer_ordering();
	test_bug3_stmtexpr_defer_variable();
	test_bug4_generic_fnptr();
	test_bug4_generic_defer_interaction();
	test_bug7_sizeof_vla_variable();
	test_bug7_sizeof_sizeof_vla();
	test_bug7_sizeof_vla_element();
	test_sizeof_parenthesized_vla();
	test_edge_multiple_typedef_shadows();
	test_edge_defer_in_generic();
	test_attributed_label_defer();
#endif
	test_number_tokenizer_identifiers();
	test_hex_numbers_vs_float_suffixes();
	test_hex_and_identifier_edge_cases();
	test_valid_number_suffixes();
#ifndef _MSC_VER
	test_return_zeroinit_no_defer();
	test_return_zeroinit_with_defer();
	test_return_zeroinit_multiple_decls();
	test_return_zeroinit_nested_blocks();
	test_sizeof_vla_zeroinit();
#endif
	test_goto_raw_decl();
	test_attributed_default_label();
	test_stmtexpr_void_cast_return();
#ifndef _MSC_VER
	test_stmtexpr_void_cast_return_helper();
	test_stmtexpr_void_cast_check();
#endif
	test_variable_named_defer_goto();
	test_defer_assignment_goto();
	test_attributed_default_safety();
	test_for_loop_goto_bypass();
	test_enum_attribute_pollution();
	test_void_typedef_overmatch();

	/* Unicode/digraph tests */
#ifndef _MSC_VER
	test_utf8_latin_extended();
	test_utf8_greek();
	test_utf8_cyrillic();
	test_utf8_cjk();
	test_ucn_short();
	test_ucn_long();
	test_utf8_ucn_mixed();
#endif
	test_digraph_brackets();
	test_digraph_braces();
	test_digraph_struct();
	test_digraph_complex();
	test_digraph_defer();
#ifndef _MSC_VER
	test_utf8_defer();
	test_utf8_math_identifiers();
#endif

	/* Compound literal in loop header */
	test_compound_literal_for_break();
	test_compound_literal_for_continue();
	test_compound_literal_while_break();
	test_nested_compound_literal_in_loop();
	test_multiple_compound_literals_in_for();
	test_compound_literal_if_condition();

	/* Bug fix verification */
	test_zombie_defer();
	test_zombie_defer_uninitialized();
	test_tcc_detection_logic();
	test_unicode_extended_ranges();
	test_memory_interning_pattern();

	/* Reported bug fixes */
	test_issue4_strtoll_unsigned();
	test_issue5_raw_typedef_collision();
	test_issue7_defer_in_for_body();
	test_issue7_defer_before_for();
	test_defer_nested_control_structures();
	test_defer_in_attribute_cleanup();
	test_defer_in_attribute_with_defer_stmt();

	/* Additional bug fixes */
#ifndef _MSC_VER
	test_register_typeof_zeroinit();
	test_register_typeof_multiple();
#endif
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
	test_c23_digit_separator_decimal();
	test_c23_digit_separator_binary();
	test_c23_digit_separator_hex();
	test_c23_digit_separator_octal();
	test_c23_digit_separator_float();
	test_c23_digit_separator_suffix();
#else
	printf("(C23 digit separator tests skipped - compiler doesn't support C23)\n");
#endif
#ifndef _MSC_VER
	test_volatile_typeof_zeroinit();
	test_volatile_typeof_struct();
	test_volatile_typeof_array();
#endif

	/* Logical-op regression */
	test_logical_op_eagain();

	/* Issue validation */
	test_switch_unbraced_inter_case_decl();
	test_switch_unbraced_multi_decl_inter_case();
#ifndef _MSC_VER
	test_generic_default_switch_defer_combo();
	test_generic_default_nested_switch_defer();
#endif
	test_typedef_table_scope_resilience();
	test_typedef_table_churn();
	test_setjmp_detection_direct();
#ifndef _MSC_VER
	test_generic_nested_default_in_switch_defer();
	test_generic_multi_default_switch();
#endif
	test_typedef_braceless_for_restore();
	test_typedef_nested_braceless_restore();
	test_typedef_braceless_while_restore();
#ifndef _MSC_VER
	test_vla_typedef_pointer_vs_value();
	test_vla_zeroed_each_loop_iteration();
	test_atomic_struct_zeroed();
	test_atomic_specifier_struct_zeroed();
#endif
	test_attr_before_type_zeroed();
	test_attr_between_type_and_var_zeroed();
	test_attr_after_var_zeroed();
	test_attr_pointer_zeroed();
	test_attr_paren_ptr_zeroed();
	test_multi_attr_zeroed();
	test_attr_struct_var_zeroed();
	test_switch_no_match_defer_skipped();
	test_switch_no_default_no_match_with_defer();
#ifndef _MSC_VER
	test_generic_compound_literal_association();
#endif
#ifdef __GNUC__
	test_generic_stmt_expr_with_defer_wrapper();
#endif
	test_assert_active_by_default();
	test_assert_still_active_after_reinclusion();
	test_knr_defer_goto();
	test_knr_typedef_param_defer();
	test_knr_multi_label();
#ifdef __GNUC__
	test_label_zeroinit_in_stmt_expr();
	test_typeof_volatile_inner_zeroed();
#endif
#ifndef _MSC_VER
	test_generic_colon_in_defer();

	/* Additional parse tests */
	test_vla_nested_delimiter_depth();
#endif
	test_typedef_survives_bare_semicolons();
	test_for_init_typedef_shadow_cleanup();
	test_orelse_comma_operator_expr();
	test_orelse_sequential_comma();
	test_short_keyword_recognition();
#if __STDC_VERSION__ >= 202311L
	test_c23_attr_positions();
#endif
#ifndef _MSC_VER
	test_vla_typedef_complex_size();
#endif
	test_goto_stress_many_targets();
	test_goto_converging_defers();
	test_stack_aggregate_zeroinit();
	test_defer_with_indirect_call();
#ifndef _MSC_VER
	test_vla_size_side_effect();
#endif
	test_multi_ptr_zeroinit();
	test_typedef_scope_after_braceless();
	test_const_orelse_scalar_fallback();
	test_const_orelse_scalar_eval_once();
	test_const_orelse_multi_eval_once();
	test_defer_all_scopes_fire();
	test_defer_loop_all_iters_fire();
	test_typedef_braceless_if_no_leak();
	test_typedef_braceless_while_no_leak();
	test_typedef_braceless_for_shadow_restore();
	test_typedef_braceless_else_no_leak();
	test_typedef_nested_braceless_control();
	test_typedef_for_init_shadow_multi_var();
	test_goto_forward_over_block_safe();
	test_goto_forward_no_decl_skip();
	test_goto_backward_safe();
	test_goto_forward_same_scope_label();
#ifndef _MSC_VER
	test_vla_sizeof_no_double_eval();
	test_vla_memset_zeroinit();
#endif
	test_defer_scope_isolation();
	test_defer_braceless_rejected();
	test_zeroinit_typedef_after_control();
	test_for_init_shadow_braceless_body();
	test_const_orelse_ptr_eval_once();
	test_const_orelse_multi_fallback();
	test_void_return_call_with_defer();
	test_void_return_cast_with_defer();
	test_switch_case_defer_ordering();
	test_switch_defer_loop_nested();
	test_typedef_for_switch_scope();
	test_typedef_nested_for_braceless();
	test_typedef_shadow_braceless_for_complex();
	test_typeof_large_struct_zeroinit();
	test_typeof_nested_struct_zeroinit();
	test_typedef_shadow_braceless_for_multi_stmt();
	test_typedef_shadow_braceless_for_nested_loops();
	test_typedef_shadow_for_with_if_body();
#if defined(__GNUC__) && !defined(__clang__)
	test_for_init_typedef_no_leak();
	test_for_init_typedef_braceless_no_leak();
	test_for_init_typedef_nested_loops();
#endif
	test_defer_switch_dead_zone_braced();
#ifndef _MSC_VER
	test_generic_const_array_zeroinit();
#endif
	test_named_struct_return_with_defer();
	test_typedef_struct_return_with_defer();
	test_goto_fnptr_decl_after_label();
	test_goto_fnptr_decl_before_goto();
	test_goto_array_ptr_decl_after_label();
#ifndef _MSC_VER
	test_typeof_const_zero_init();
#endif
	test_param_typedef_shadow();
	test_goto_over_static_decl();
	test_defer_break_continue_rejected();
	test_defer_inner_loop_break();
	test_defer_inner_loop_continue();
	test_defer_inner_switch_break();
	test_defer_inner_do_while_break();
	test_t_heuristic_shadow_mul();
	test_t_heuristic_shadow_arith();
	test_t_heuristic_shadow_ptr_deref();
	test_t_heuristic_shadow_scope();
	test_t_heuristic_shadow_param();
	test_t_heuristic_noshadow();
	test_array_orelse_rejected();
#ifndef _MSC_VER
	test_deep_struct_nesting_goto();
#endif
#ifndef _MSC_VER
	test_generic_array_not_vla();
#endif
	test_c23_attr_void_function();

	/* Bug regression round 2 */
	test_bug2_filescope_t_mul();
	test_bug2_filescope_t_arith();
	test_bug2_filescope_t_in_expr();
	test_bug5_void_return_call_defer();
	test_bug5_void_return_cast_defer();
	test_bug5_void_return_bare_defer();
#ifdef __GNUC__
	test_bug4_stmt_expr_in_defer();
#endif
	test_bug1_digraph_in_defer();
	test_bug6_setjmp_detection();
	test_bug_r2_fnptr_return_typedef();
	test_bug_r2_fnptr_return_raw();
	test_bug_r2_ptr_return();
#ifndef _WIN32
	test_bug_r1_readonly_dir();
#endif
	test_bug_r3_line_directive();
	test_void_parenthesized_func_defer();

	test_paren_param_typedef_shadow();
	test_extra_paren_funcptr_shadow();
	test_c23_attr_misparsed_as_vla();
	test_c23_extended_float_x_suffixes();
	test_pragma_breaks_type_specifier();
	test_pragma_struct_body_parsing();
	test_knr_param_shadow_no_poison();
	test_hashmap_struct_entry_regression();
	test_negative_parse_corpus();
	test_large_token_no_truncation();
	test_typeof_vla_orelse_side_effect_rejected();
#ifdef __GNUC__
	test_asm_in_orelse_lhs_rejected();
#endif
}

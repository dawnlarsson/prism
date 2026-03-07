void test_zeroinit_basic_types(void) {
	int i;
	CHECK_EQ(i, 0, "int zero-init");

	char c;
	CHECK_EQ(c, 0, "char zero-init");

	short s;
	CHECK_EQ(s, 0, "short zero-init");

	long l;
	CHECK(l == 0, "long zero-init");

	float f;
	CHECK(f == 0.0f, "float zero-init");

	double d;
	CHECK(d == 0.0, "double zero-init");

	unsigned int u;
	CHECK_EQ(u, 0, "unsigned int zero-init");

	long long ll;
	CHECK(ll == 0LL, "long long zero-init");

	_Bool b;
	CHECK(b == 0, "_Bool zero-init");

	unsigned char uc;
	CHECK_EQ(uc, 0, "unsigned char zero-init");

	signed char sc;
	CHECK_EQ(sc, 0, "signed char zero-init");

	enum { RED, GREEN, BLUE } color;

	CHECK_EQ(color, 0, "enum variable zero-init");

	size_t sz;
	CHECK(sz == 0, "size_t zero-init");
}

void test_zeroinit_pointers(void) {
	int *p;
	CHECK(p == NULL, "int pointer zero-init");

	char *s;
	CHECK(s == NULL, "char pointer zero-init");

	void *vp;
	CHECK(vp == NULL, "void pointer zero-init");

	int **pp;
	CHECK(pp == NULL, "double pointer zero-init");

	void (*fp)(void);
	CHECK(fp == NULL, "function pointer zero-init");
}

void test_zeroinit_arrays(void) {
	int arr[5];
	int all_zero = 1;
	for (int i = 0; i < 5; i++)
		if (arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "int array zero-init");

	char buf[64];
	CHECK(buf[0] == 0, "char array zero-init");

	int arr2d[3][3];
	all_zero = 1;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			if (arr2d[i][j] != 0) all_zero = 0;
	CHECK(all_zero, "2D array zero-init");

	int *ptrs[5];
	int all_null = 1;
	for (int i = 0; i < 5; i++)
		if (ptrs[i] != NULL) all_null = 0;
	CHECK(all_null, "pointer array zero-init");

	// NIGHTMARE: 5D array
	int arr5d[2][3][4][5][6];
	all_zero = 1;
	for (int a = 0; a < 2; a++)
		for (int b = 0; b < 3; b++)
			for (int c = 0; c < 4; c++)
				for (int d = 0; d < 5; d++)
					for (int e = 0; e < 6; e++)
						if (arr5d[a][b][c][d][e] != 0) all_zero = 0;
	CHECK(all_zero, "nightmare: 5D array zero-init");

	// NIGHTMARE: Array of structs containing arrays of function pointers
	struct {
		int id;
		void (*handlers[4])(int);

		struct {
			int (*transform)(int, int);
			int *data_ptr;
		} nested[2];
	} complex_arr[3];

	all_zero = 1;
	all_null = 1;
	for (int i = 0; i < 3; i++) {
		if (complex_arr[i].id != 0) all_zero = 0;
		for (int j = 0; j < 4; j++)
			if (complex_arr[i].handlers[j] != NULL) all_null = 0;
		for (int j = 0; j < 2; j++) {
			if (complex_arr[i].nested[j].transform != NULL) all_null = 0;
			if (complex_arr[i].nested[j].data_ptr != NULL) all_null = 0;
		}
	}
	CHECK(all_zero && all_null, "nightmare: array of complex structs zero-init");

	// NIGHTMARE: 3D array of pointers to function pointers
	int(*(*ptr_arr_3d[2][3][4])(void));
	all_null = 1;
	for (int a = 0; a < 2; a++)
		for (int b = 0; b < 3; b++)
			for (int c = 0; c < 4; c++)
				if (ptr_arr_3d[a][b][c] != NULL) all_null = 0;
	CHECK(all_null, "nightmare: 3D array of func ptr ptrs zero-init");

	// NIGHTMARE: Array with size from sizeof expression on a TYPE (compile-time constant)
	// Note: sizeof on a variable could be runtime if it's a VLA, so use a type instead
	int sized_arr[sizeof(struct {
		long long data[8];
		void *ptrs[4];
		char name[32];
	})];
	all_zero = 1;
	for (size_t i = 0; i < sizeof(sized_arr) / sizeof(sized_arr[0]); i++)
		if (sized_arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "nightmare: sizeof-sized array zero-init");

	// NIGHTMARE: Jagged-style: array of pointers to differently-sized arrays
	int (*jagged[5])[10];
	all_null = 1;
	for (int i = 0; i < 5; i++)
		if (jagged[i] != NULL) all_null = 0;
	CHECK(all_null, "nightmare: array of pointers to arrays zero-init");

	// NIGHTMARE: Array of unions containing arrays
	union {
		int ints[8];
		float floats[8];
		char bytes[32];

		struct {
			void *ptr;
			size_t len;
		} slice;
	} union_arr[4];

	all_zero = 1;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 8; j++)
			if (union_arr[i].ints[j] != 0) all_zero = 0;
	CHECK(all_zero, "nightmare: array of unions zero-init");
}

void test_zeroinit_structs(void) {
	struct {
		int a;
		char b;
		float c;
	} s;

	CHECK(s.a == 0 && s.b == 0 && s.c == 0.0f, "anonymous struct zero-init");

	struct Point {
		int x;
		int y;
	};
	struct Point p;
	CHECK(p.x == 0 && p.y == 0, "named struct zero-init");

	struct {
		int *ptr;
		int val;
	} sp;

	CHECK(sp.ptr == NULL && sp.val == 0, "struct with pointer zero-init");

	struct {
		int arr[4];
		int len;
	} sa;

	int all_zero = (sa.len == 0);
	for (int i = 0; i < 4; i++)
		if (sa.arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "struct with array zero-init");

	// NIGHTMARE: Deeply nested struct with every possible member type
	struct NightmareStruct {
		// Basic types
		char c;
		short s;
		int i;
		long l;
		long long ll;
		float f;
		double d;
		long double ld;

		// Unsigned variants
		unsigned char uc;
		unsigned short us;
		unsigned int ui;
		unsigned long ul;
		unsigned long long ull;

		// Pointers
		void *vp;
		int *ip;
		char **cpp;
		void ***vppp;

		// Function pointers
		int (*fp)(void);
		void *(*(*complex_fp)(int, char *))[5];

		// Arrays
		int arr[10];
		char str[32];
		int *ptr_arr[5];
		int (*arr_ptr)[10];

		// Nested struct
		struct {
			int x, y, z;

			struct {
				float r, g, b, a;
			} color;

			void (*callback)(void *);
		} nested;

		// Nested union
		union {
			int as_int;
			float as_float;
			char as_bytes[4];
		} variant;

		// Bitfields mixed in
		unsigned int flag1 : 1;
		unsigned int flag2 : 1;
		unsigned int reserved : 6;
		unsigned int value : 24;

		// More nested anonymous
		struct {
			union {
				struct {
					short lo;
					short hi;
				};

				int combined;
			};

			int (*handlers[3])(int);
		};
	};

	struct NightmareStruct nightmare;

	// Check all basic types
	CHECK(nightmare.c == 0 && nightmare.s == 0 && nightmare.i == 0 && nightmare.l == 0 &&
		  nightmare.ll == 0,
	      "nightmare struct: basic int types");
	CHECK(nightmare.f == 0.0f && nightmare.d == 0.0 && nightmare.ld == 0.0L,
	      "nightmare struct: float types");
	CHECK(nightmare.uc == 0 && nightmare.us == 0 && nightmare.ui == 0 && nightmare.ul == 0 &&
		  nightmare.ull == 0,
	      "nightmare struct: unsigned types");

	// Check pointers
	CHECK(nightmare.vp == NULL && nightmare.ip == NULL && nightmare.cpp == NULL && nightmare.vppp == NULL,
	      "nightmare struct: pointers");
	CHECK(nightmare.fp == NULL && nightmare.complex_fp == NULL, "nightmare struct: function pointers");
	CHECK(nightmare.arr_ptr == NULL, "nightmare struct: pointer to array");

	// Check arrays
	all_zero = 1;
	for (int j = 0; j < 10; j++)
		if (nightmare.arr[j] != 0) all_zero = 0;
	CHECK(all_zero, "nightmare struct: int array");
	CHECK(nightmare.str[0] == 0, "nightmare struct: char array");
	int all_null = 1;
	for (int j = 0; j < 5; j++)
		if (nightmare.ptr_arr[j] != NULL) all_null = 0;
	CHECK(all_null, "nightmare struct: pointer array");

	// Check nested struct
	CHECK(nightmare.nested.x == 0 && nightmare.nested.y == 0 && nightmare.nested.z == 0,
	      "nightmare struct: nested xyz");
	CHECK(nightmare.nested.color.r == 0.0f && nightmare.nested.color.g == 0.0f &&
		  nightmare.nested.color.b == 0.0f && nightmare.nested.color.a == 0.0f,
	      "nightmare struct: nested color");
	CHECK(nightmare.nested.callback == NULL, "nightmare struct: nested callback");

	// Check union
	CHECK(nightmare.variant.as_int == 0, "nightmare struct: union");

	// Check bitfields
	CHECK(nightmare.flag1 == 0 && nightmare.flag2 == 0 && nightmare.reserved == 0 && nightmare.value == 0,
	      "nightmare struct: bitfields");

	// Check anonymous nested
	CHECK(nightmare.lo == 0 && nightmare.hi == 0 && nightmare.combined == 0,
	      "nightmare struct: anonymous nested");
	all_null = 1;
	for (int j = 0; j < 3; j++)
		if (nightmare.handlers[j] != NULL) all_null = 0;
	CHECK(all_null, "nightmare struct: anonymous handlers array");
}

void test_zeroinit_qualifiers(void) {
	volatile int v;
	CHECK_EQ(v, 0, "volatile int zero-init");

	register int r;
	CHECK_EQ(r, 0, "register int zero-init");

	_Alignas(16) int aligned;
	CHECK_EQ(aligned, 0, "_Alignas zero-init");

	const int ci;
	CHECK(ci == 0, "const int zero-init");

	const char cc;
	CHECK(cc == 0, "const char zero-init");

	static int si;
	CHECK(si == 0, "static int zero-init");
}

void test_zeroinit_in_scopes(void) {
	{
		int x;
		CHECK_EQ(x, 0, "nested scope zero-init");
	}

	if (1) {
		int x;
		CHECK_EQ(x, 0, "if block zero-init");
	}

	for (int i = 0; i < 1; i++) {
		int x;
		CHECK_EQ(x, 0, "for loop zero-init");
	}

	int val = 1;
	switch (val) {
	case 1: {
		int x;
		CHECK_EQ(x, 0, "switch case zero-init");
		break;
	}
	}
}

void test_zeroinit_with_defer(void) {
	int result;
	{
		int x;
		defer result = x;
	}
	CHECK_EQ(result, 0, "zero-init with defer");
}

#ifdef __GNUC__
void test_zeroinit_typeof(void) {
	// typeof now gets zero-init via memset (fixed in v0.99.5+)
	// This works for scalars, arrays, and even VLAs

	// Test 1: typeof(type) - basic type
	typeof(int) a;
	CHECK_EQ(a, 0, "typeof(int) zero-init");

	// Test 2: typeof(expr) - expression
	double pi = 3.14159;
	typeof(pi) b;
	CHECK(b == 0.0, "typeof(expr) zero-init");

	// Test 3: __typeof__ variant (GCC extension)
	__typeof__(int) c;
	CHECK_EQ(c, 0, "__typeof__(int) zero-init");

	// Test 4: Multi-declarator typeof
	typeof(int) x, y, z;
	CHECK(x == 0 && y == 0 && z == 0, "typeof multi-decl zero-init");

	// Test 5: typeof with array element reference
	int arr[4] = {1, 2, 3, 4};
	typeof(arr[0]) elem;
	CHECK_EQ(elem, 0, "typeof(arr[0]) zero-init");

	// Test 6: typeof pointer (should use = 0, not memset)
	typeof(int) *ptr;
	CHECK(ptr == NULL, "typeof(int)* pointer zero-init");

	// Test 7: typeof with explicit init still works
	typeof(int) init = 42;
	CHECK_EQ(init, 42, "typeof with explicit init");
}

void test_typeof_zeroinit_all_basic_types(void) {
	// Every basic C type via typeof
	typeof(char) c;
	typeof(signed char) sc;
	typeof(unsigned char) uc;
	typeof(short) s;
	typeof(unsigned short) us;
	typeof(int) i;
	typeof(unsigned int) ui;
	typeof(long) l;
	typeof(unsigned long) ul;
	typeof(long long) ll;
	typeof(unsigned long long) ull;
	typeof(float) f;
	typeof(double) d;
	typeof(long double) ld;
	typeof(_Bool) b;

	CHECK(c == 0 && sc == 0 && uc == 0, "typeof char types zero-init");
	CHECK(s == 0 && us == 0, "typeof short types zero-init");
	CHECK(i == 0 && ui == 0, "typeof int types zero-init");
	CHECK(l == 0 && ul == 0, "typeof long types zero-init");
	CHECK(ll == 0 && ull == 0, "typeof long long types zero-init");
	CHECK(f == 0.0f && d == 0.0 && ld == 0.0L, "typeof float types zero-init");
	CHECK(b == 0, "typeof _Bool zero-init");
}

void test_typeof_zeroinit_structs(void) {
	// typeof with struct expressions
	struct {
		int x;
		int y;
		int z;
	} point = {10, 20, 30};

	typeof(point) pt;
	CHECK(pt.x == 0 && pt.y == 0 && pt.z == 0, "typeof(struct expr) zero-init");

	// typeof with nested struct
	struct {
		struct {
			int a;
			int b;
		} inner;

		int outer;
	} nested = {{1, 2}, 3};

	typeof(nested) n;
	CHECK(n.inner.a == 0 && n.inner.b == 0 && n.outer == 0, "typeof(nested struct) zero-init");

	// typeof with struct containing array
	struct {
		int arr[8];
		int count;
	} container = {{1, 2, 3, 4, 5, 6, 7, 8}, 8};

	typeof(container) cont;
	int all_zero = 1;
	for (int i = 0; i < 8; i++)
		if (cont.arr[i] != 0) all_zero = 0;
	CHECK(all_zero && cont.count == 0, "typeof(struct with array) zero-init");
}

void test_typeof_zeroinit_arrays(void) {
	// typeof with fixed-size arrays
	int arr10[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
	typeof(arr10) copy;
	int all_zero = 1;
	for (int i = 0; i < 10; i++)
		if (copy[i] != 0) all_zero = 0;
	CHECK(all_zero, "typeof(int[10]) array zero-init");

	// typeof with 2D array
	int arr2d[3][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
	typeof(arr2d) copy2d;
	all_zero = 1;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 4; j++)
			if (copy2d[i][j] != 0) all_zero = 0;
	CHECK(all_zero, "typeof(int[3][4]) 2D array zero-init");

	// typeof with char array (string)
	char str[32] = "hello world";
	typeof(str) buf;
	CHECK(buf[0] == 0, "typeof(char[32]) string buffer zero-init");
}

void test_typeof_zeroinit_qualifiers(void) {
	// typeof with const expression - use +0 to strip const
	const int ci = 42;
	typeof(ci + 0) mutable_copy; // ci+0 is int, not const int
	CHECK_EQ(mutable_copy, 0, "typeof(const expr + 0) zero-init");

	// typeof with volatile - use +0 to strip volatile
	volatile int vi = 100;
	typeof(vi + 0) vol_copy; // vi+0 is int, not volatile int
	CHECK_EQ(vol_copy, 0, "typeof(volatile expr + 0) zero-init");

	// typeof with restrict pointer - pointer arithmetic strips restrict
	int dummy = 5;
	int *restrict rp = &dummy;
	typeof(rp + 0) rp_copy; // rp+0 is int*, not int* restrict
	CHECK(rp_copy == NULL, "typeof(restrict ptr + 0) zero-init");
}

void test_typeof_zeroinit_complex_exprs(void) {
	// typeof with arithmetic expression
	int a = 10, b = 20;
	typeof(a + b) sum;
	CHECK_EQ(sum, 0, "typeof(a + b) zero-init");

	// typeof with ternary expression
	typeof(a > b ? a : b) max;
	CHECK_EQ(max, 0, "typeof(ternary) zero-init");

	// typeof with cast expression
	typeof((double)a) casted;
	CHECK(casted == 0.0, "typeof((double)a) zero-init");

	// typeof with sizeof expression (result is size_t)
	typeof(sizeof(int)) sz;
	CHECK(sz == 0, "typeof(sizeof) zero-init");

	// typeof with array subscript
	int arr[5] = {1, 2, 3, 4, 5};
	typeof(arr[2]) elem;
	CHECK_EQ(elem, 0, "typeof(arr[2]) zero-init");

	// typeof with struct member access
	struct {
		int val;
	} s = {99};

	typeof(s.val) member;
	CHECK_EQ(member, 0, "typeof(s.val) zero-init");

	// typeof with pointer dereference
	int x = 42;
	int *px = &x;
	typeof(*px) deref;
	CHECK_EQ(deref, 0, "typeof(*ptr) zero-init");
}

void test_typeof_zeroinit_vla(void) {
	// typeof with VLA - this is the tricky case!
	// memset works because sizeof(vla) is evaluated at runtime
	int n = 5;
	int vla[n];
	for (int i = 0; i < n; i++) vla[i] = i + 1;

	typeof(vla) vla_copy;
	int all_zero = 1;
	for (int i = 0; i < n; i++)
		if (vla_copy[i] != 0) all_zero = 0;
	CHECK(all_zero, "typeof(VLA) now gets zero-init via memset");

	// typeof with VLA in expression
	int m = 3;
	int vla2[m];
	typeof(vla2[0]) elem;
	CHECK_EQ(elem, 0, "typeof(VLA[0]) element zero-init");
}

void test_typeof_zeroinit_function_ptrs(void) {
	// typeof with function pointer
	int (*fp)(int, int) = NULL;
	typeof(fp) fp_copy; // Pointer - should get = 0
	CHECK(fp_copy == NULL, "typeof(func ptr) zero-init");

	// typeof with function type via expression
	extern int printf(const char *, ...);
	typeof(&printf) print_ptr; // Pointer - should get = 0
	CHECK(print_ptr == NULL, "typeof(&printf) zero-init");
}

void test_typeof_zeroinit_multi_decl_complex(void) {
	// Multi-declarator with mixed pointer/non-pointer
	typeof(int) a, *b, c, **d;
	CHECK_EQ(a, 0, "typeof multi-decl: a zero-init");
	CHECK(b == NULL, "typeof multi-decl: *b zero-init");
	CHECK_EQ(c, 0, "typeof multi-decl: c zero-init");
	CHECK(d == NULL, "typeof multi-decl: **d zero-init");

	// Multi-declarator with arrays
	typeof(int) arr1[3], arr2[5];
	int all_zero = 1;
	for (int i = 0; i < 3; i++)
		if (arr1[i] != 0) all_zero = 0;
	for (int i = 0; i < 5; i++)
		if (arr2[i] != 0) all_zero = 0;
	CHECK(all_zero, "typeof multi-decl arrays zero-init");
}

void test_typeof_zeroinit_in_scopes(void) {
	// typeof in nested blocks
	{
		{
			{
				typeof(int) deep;
				CHECK_EQ(deep, 0, "typeof in deep nested block");
			}
		}
	}

	// typeof in loop body
	for (int i = 0; i < 3; i++) {
		typeof(int) loop_var;
		CHECK_EQ(loop_var, 0, "typeof in for loop");
	}

	// typeof in if body
	if (1) {
		typeof(double) cond_var;
		CHECK(cond_var == 0.0, "typeof in if body");
	}

	// typeof in switch case
	int sel = 1;
	switch (sel) {
	case 1: {
		typeof(int) case_var;
		CHECK_EQ(case_var, 0, "typeof in switch case");
		break;
	}
	}
}

void test_typeof_zeroinit_with_defer(void) {
	// typeof with defer - make sure they interact correctly
	int cleanup_ran = 0;
	{
		typeof(int) val;
		defer cleanup_ran = 1;
		CHECK_EQ(val, 0, "typeof with defer: value zero-init");
	}
	CHECK_EQ(cleanup_ran, 1, "typeof with defer: defer ran");

	// typeof in defer expression
	int counter = 0;
	{
		typeof(counter) local;
		defer counter = local + 1; // Uses zero-init value
	}
	CHECK_EQ(counter, 1, "typeof in defer expression");
}

void test_typeof_zeroinit_unions(void) {
	// typeof with union
	union {
		int i;
		float f;
		char c[4];
	} u = {.i = 0x12345678};

	typeof(u) u_copy;
	CHECK(u_copy.i == 0, "typeof(union) zero-init");

	// typeof with anonymous union member
	struct {
		union {
			int a;
			float b;
		};

		int c;
	} mixed = {.a = 99, .c = 100};

	typeof(mixed) m_copy;
	CHECK(m_copy.a == 0 && m_copy.c == 0, "typeof(struct with anon union) zero-init");
}

void test_typeof_zeroinit_edge_cases(void) {
	// typeof of typeof
	int x = 42;
	typeof(x) y;
	typeof(y) z;
	CHECK_EQ(z, 0, "typeof(typeof(x)) zero-init");

	// typeof with parenthesized expression
	int val = 5;
	typeof((((val)))) paren;
	CHECK_EQ(paren, 0, "typeof((((val)))) zero-init");

	// typeof with comma operator (takes type of last)
	int a = 1, b = 2;
	typeof((a, b)) comma;
	CHECK_EQ(comma, 0, "typeof((a, b)) comma expr zero-init");

	// typeof with compound literal
	typeof((int){42}) compound;
	CHECK_EQ(compound, 0, "typeof(compound literal) zero-init");

	// typeof with _Alignof result
	typeof(_Alignof(double)) align_val;
	CHECK(align_val == 0, "typeof(_Alignof) zero-init");
}

void test_typeof_zeroinit_torture_stress(void) {
	// Stress test: many typeof declarations in sequence
	typeof(int) v0, v1, v2, v3, v4, v5, v6, v7, v8, v9;
	typeof(int) v10, v11, v12, v13, v14, v15, v16, v17, v18, v19;
	int all_zero = 1;
	if (v0 != 0 || v1 != 0 || v2 != 0 || v3 != 0 || v4 != 0) all_zero = 0;
	if (v5 != 0 || v6 != 0 || v7 != 0 || v8 != 0 || v9 != 0) all_zero = 0;
	if (v10 != 0 || v11 != 0 || v12 != 0 || v13 != 0 || v14 != 0) all_zero = 0;
	if (v15 != 0 || v16 != 0 || v17 != 0 || v18 != 0 || v19 != 0) all_zero = 0;
	CHECK(all_zero, "20 typeof vars in sequence all zero-init");

	// Large struct via typeof
	struct {
		int arr[100];
		double values[50];
		char buffer[256];
	} big = {0};

	big.arr[0] = 1; // Make it non-zero
	typeof(big) big_copy;
	all_zero = 1;
	for (int i = 0; i < 100; i++)
		if (big_copy.arr[i] != 0) all_zero = 0;
	for (int i = 0; i < 50; i++)
		if (big_copy.values[i] != 0.0) all_zero = 0;
	for (int i = 0; i < 256; i++)
		if (big_copy.buffer[i] != 0) all_zero = 0;
	CHECK(all_zero, "large struct via typeof all zero-init");
}

#endif

enum { TEST_ARRAY_SIZE = 10 };

void test_zeroinit_enum_array_size(void) {
	int arr[TEST_ARRAY_SIZE];
	int all_zero = 1;
	for (int i = 0; i < TEST_ARRAY_SIZE; i++)
		if (arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "enum constant array size zero-init");
}

void test_zeroinit_alignas_array(void) {
	_Alignas(32) int arr[8];
	int all_zero = 1;
	for (int i = 0; i < 8; i++)
		if (arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "_Alignas array zero-init");
}

void test_zeroinit_union(void) {
	union {
		int i;
		float f;
		char c[4];
	} u;

	CHECK_EQ(u.i, 0, "union zero-init");
}

void test_zeroinit_torture_declarators(void) {
	// Pointer to array of function pointers returning pointer to int
	int *(*(*pafp)[5])(void);
	CHECK(pafp == NULL, "torture: ptr->arr[5]->func()->ptr");

	// Array of pointers to functions returning arrays of pointers
	int *(*(*afpa[3])(int))[4];
	int all_null = 1;
	for (int i = 0; i < 3; i++)
		if (afpa[i] != NULL) all_null = 0;
	CHECK(all_null, "torture: arr[3]->func->arr[4]->ptr");

	// Triple pointer to const volatile int
	const volatile int ***cvipp;
	CHECK(cvipp == NULL, "torture: const volatile int***");

	// Function pointer returning function pointer
	int (*(*fp_fp)(int))(char);
	CHECK(fp_fp == NULL, "torture: func->func");

	// Pointer to array of arrays of function pointers
	void(*(*(*paafp)[3][4])(int, int));
	CHECK(paafp == NULL, "torture: ptr->arr[3][4]->func");

	// Array of pointers to pointers to function pointers
	int (**(*appfp[2][3]))(void);
	all_null = 1;
	for (int i = 0; i < 2; i++)
		for (int j = 0; j < 3; j++)
			if (appfp[i][j] != NULL) all_null = 0;
	CHECK(all_null, "torture: arr[2][3]->ptr->ptr->func");
}

void test_zeroinit_torture_attributes(void) {
	// __attribute__ before type
	__attribute__((unused)) int attr_before;
	CHECK_EQ(attr_before, 0, "torture: __attribute__ before type");

	// __attribute__ after type
	int __attribute__((unused)) attr_after;
	CHECK_EQ(attr_after, 0, "torture: __attribute__ after type");

	// __attribute__ after declarator
	int attr_decl __attribute__((unused));
	CHECK_EQ(attr_decl, 0, "torture: __attribute__ after declarator");

	// Multiple __attribute__
	__attribute__((unused)) __attribute__((aligned(8))) int multi_attr;
	CHECK_EQ(multi_attr, 0, "torture: multiple __attribute__");

	// __attribute__ with pointer
	int *__attribute__((unused)) attr_ptr;
	CHECK(attr_ptr == NULL, "torture: __attribute__ with pointer");

	// __attribute__ in multi-decl
	int __attribute__((unused)) ma1, __attribute__((unused)) ma2;
	CHECK(ma1 == 0 && ma2 == 0, "torture: __attribute__ multi-decl");

	// Aligned array
	__attribute__((aligned(64))) int aligned_arr[16];
	int all_zero = 1;
	for (int i = 0; i < 16; i++)
		if (aligned_arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "torture: aligned array");
}

void test_zeroinit_torture_partial_init(void) {
	// Alternating init/no-init in multi-decl
	int a, b = 1, c, d = 2, e, f = 3, g;
	CHECK(a == 0 && b == 1 && c == 0 && d == 2 && e == 0 && f == 3 && g == 0,
	      "torture: alternating init pattern");

	// First init, rest no-init
	int first = 99, second, third, fourth, fifth;
	CHECK(first == 99 && second == 0 && third == 0 && fourth == 0 && fifth == 0,
	      "torture: first init only");

	// Last init, rest no-init
	int p1, p2, p3, p4, p5 = 88;
	CHECK(p1 == 0 && p2 == 0 && p3 == 0 && p4 == 0 && p5 == 88, "torture: last init only");

	// Mixed pointers with partial init
	int *ptr1, *ptr2 = NULL, *ptr3, val1 = 7, val2, *ptr4;
	CHECK(ptr1 == NULL && ptr2 == NULL && ptr3 == NULL && val1 == 7 && val2 == 0 && ptr4 == NULL,
	      "torture: mixed ptr/val partial init");

	// Array with expression init next to uninit scalars
	int x, arr[3] = {1, 2, 3}, y;
	CHECK(x == 0 && arr[0] == 1 && arr[1] == 2 && arr[2] == 3 && y == 0,
	      "torture: uninit around array init");
}

#ifdef __GNUC__
void test_zeroinit_torture_stmt_expr(void) {
	// Declaration in statement expression
	int result = ({
		int inner; // Should be zero-init
		inner + 10;
	});
	CHECK_EQ(result, 10, "torture: zero-init in stmt expr");

	// Multiple declarations in statement expression
	int result2 = ({
		int a, b, c;
		a + b + c + 5;
	});
	CHECK_EQ(result2, 5, "torture: multi-decl in stmt expr");

	// Nested statement expressions
	int result3 = ({
		int outer;
		int inner_val = ({
			int inner;
			inner + 1;
		});
		outer + inner_val;
	});
	CHECK_EQ(result3, 1, "torture: nested stmt expr zero-init");

	// Statement expression in array size - GCC treats as VLA, so Prism
	// intentionally skips zero-init (VLAs can't have initializers in C).
	// Just verify it compiles without error.
	raw int arr[({
		enum { N = 5 };
		N;
	})];
	(void)arr; // Suppress unused warning

	PrismResult transpile_result = prism_transpile_source(
	    "void f(void) {\n"
	    "    int arr[({ enum { N = 5 }; N; })];\n"
	    "    (void)arr;\n"
	    "}\n",
	    "stmt_expr_array_size.c", prism_defaults());
	CHECK_EQ(transpile_result.status, PRISM_OK, "torture: stmt expr in array size transpiles");
	if (transpile_result.output) {
		CHECK(strstr(transpile_result.output, "int arr[({ enum { N = 5 }; N; })];") != NULL,
		      "torture: stmt expr array size preserved");
		CHECK(strstr(transpile_result.output, "memset(&arr, 0, sizeof(arr));") != NULL,
		      "torture: stmt expr array size gets VLA memset");
		CHECK(strstr(transpile_result.output, "arr = {0}") == NULL,
		      "torture: stmt expr array size avoids illegal brace init");
	}
	prism_free(&transpile_result);
}
#endif

void test_zeroinit_torture_deep_nesting(void) {
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
											{ // 11 levels deep
												int deep_var;
												CHECK_EQ(
												    deep_var,
												    0,
												    "torture:"
												    " 11 "
												    "levels "
												    "deep");
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

	// Variable in each nesting level
	int v0;
	{
		int v1;
		{
			int v2;
			{
				int v3;
				{
					int v4;
					{
						int v5;
						CHECK(v0 == 0 && v1 == 0 && v2 == 0 && v3 == 0 && v4 == 0 &&
							  v5 == 0,
						      "torture: var per nesting level");
					}
				}
			}
		}
	}

	// Declaration after nested block
	{
		{
			{
				{ int inner; }
			}
		}
	}
	int after_nested;
	CHECK_EQ(after_nested, 0, "torture: after deeply nested block");
}

void test_zeroinit_torture_bitfields(void) {
	struct {
		unsigned int a : 1;
		unsigned int b : 3;
		unsigned int c : 12;
		unsigned int d : 16;
	} bits;

	CHECK(bits.a == 0 && bits.b == 0 && bits.c == 0 && bits.d == 0, "torture: basic bit-fields");

	// Mixed bit-fields and regular fields
	struct {
		int regular1;
		unsigned int bf1 : 4;
		unsigned int bf2 : 4;
		int regular2;
		unsigned int bf3 : 8;
		int *ptr;
	} mixed_bf;

	CHECK(mixed_bf.regular1 == 0 && mixed_bf.bf1 == 0 && mixed_bf.bf2 == 0 && mixed_bf.regular2 == 0 &&
		  mixed_bf.bf3 == 0 && mixed_bf.ptr == NULL,
	      "torture: mixed bit-fields and regular");

	// Bit-field spanning all bits
	struct {
		unsigned long long full : 64;
	} full_bf;

	CHECK(full_bf.full == 0, "torture: 64-bit bit-field");
}

void test_zeroinit_torture_anonymous(void) {
	// Anonymous struct in struct
	struct {
		int before;

		struct {
			int x, y, z;
		}; // anonymous

		int after;
	} anon_struct;

	CHECK(anon_struct.before == 0 && anon_struct.x == 0 && anon_struct.y == 0 && anon_struct.z == 0 &&
		  anon_struct.after == 0,
	      "torture: anonymous struct");

	// Anonymous union in struct
	struct {
		int tag;

		union {
			int i;
			float f;
			char c[4];
		}; // anonymous
	} anon_union;

	CHECK(anon_union.tag == 0 && anon_union.i == 0, "torture: anonymous union");

	// Nested anonymous
	struct {
		union {
			struct {
				int a, b;
			};

			struct {
				float x, y;
			};
		};

		int z;
	} nested_anon;

	CHECK(nested_anon.a == 0 && nested_anon.b == 0 && nested_anon.z == 0, "torture: nested anonymous");
}

void test_zeroinit_torture_compound_literals(void) {
	// Declaration before compound literal use
	int before_cl;
	int *cl_ptr = (int[]){1, 2, 3};
	int after_cl;
	CHECK(before_cl == 0 && after_cl == 0, "torture: around compound literal");
	CHECK(cl_ptr[0] == 1 && cl_ptr[1] == 2 && cl_ptr[2] == 3, "torture: compound literal values");

	// Struct compound literal context - use named type to avoid mismatch
	typedef struct {
		int x, y;
	} Point;

	Point s_before;
	Point *sp = &(Point){10, 20};
	Point s_after;
	CHECK(s_before.x == 0 && s_before.y == 0, "torture: struct before CL");
	CHECK(s_after.x == 0 && s_after.y == 0, "torture: struct after CL");
	CHECK(sp->x == 10 && sp->y == 20, "torture: compound literal struct");
}

void test_zeroinit_torture_fam_adjacent(void) {
	// Can't have FAM itself uninitialized, but test structs around it
	struct HasFAM {
		int count;
		int data[];
	};

	struct {
		int x, y;
	} before_fam;
	struct HasFAM *fam_ptr; // Pointer to FAM struct

	struct {
		int a, b;
	} after_fam;

	CHECK(before_fam.x == 0 && before_fam.y == 0, "torture: before FAM pointer");
	CHECK(fam_ptr == NULL, "torture: FAM pointer");
	CHECK(after_fam.a == 0 && after_fam.b == 0, "torture: after FAM pointer");
}

void test_zeroinit_torture_long_multidecl(void) {
	int v00, v01, v02, v03, v04, v05, v06, v07, v08, v09, v10, v11, v12, v13, v14, v15, v16, v17, v18,
	    v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31;

	int all_zero = 1;
	if (v00 != 0 || v01 != 0 || v02 != 0 || v03 != 0) all_zero = 0;
	if (v04 != 0 || v05 != 0 || v06 != 0 || v07 != 0) all_zero = 0;
	if (v08 != 0 || v09 != 0 || v10 != 0 || v11 != 0) all_zero = 0;
	if (v12 != 0 || v13 != 0 || v14 != 0 || v15 != 0) all_zero = 0;
	if (v16 != 0 || v17 != 0 || v18 != 0 || v19 != 0) all_zero = 0;
	if (v20 != 0 || v21 != 0 || v22 != 0 || v23 != 0) all_zero = 0;
	if (v24 != 0 || v25 != 0 || v26 != 0 || v27 != 0) all_zero = 0;
	if (v28 != 0 || v29 != 0 || v30 != 0 || v31 != 0) all_zero = 0;
	CHECK(all_zero, "torture: 32-variable multi-decl");
}

void test_zeroinit_torture_control_flow(void) {
	// After if-else chain
	if (0) {
	} else if (0) {
	} else {
	}
	int after_if_chain;
	CHECK_EQ(after_if_chain, 0, "torture: after if-else chain");

	// Between switch cases (inside scope)
	int sel = 1;
	switch (sel) {
	case 0: break;
	case 1: {
		int in_case1;
		CHECK_EQ(in_case1, 0, "torture: in switch case");
		break;
	}
	default: break;
	}

	// After switch
	int after_switch;
	CHECK_EQ(after_switch, 0, "torture: after switch");

	// After for loop
	for (int i = 0; i < 1; i++) {}
	int after_for;
	CHECK_EQ(after_for, 0, "torture: after for loop");

	// After while loop
	int cond = 0;
	while (cond) {}
	int after_while;
	CHECK_EQ(after_while, 0, "torture: after while loop");

	// After do-while
	do {
	} while (0);
	int after_do;
	CHECK_EQ(after_do, 0, "torture: after do-while");
}

void test_zeroinit_torture_stress(void) {
	// 50 sequential declarations of different types
	char c1;
	char c2;
	char c3;
	char c4;
	char c5;
	short s1;
	short s2;
	short s3;
	short s4;
	short s5;
	int i1;
	int i2;
	int i3;
	int i4;
	int i5;
	long l1;
	long l2;
	long l3;
	long l4;
	long l5;
	float f1;
	float f2;
	float f3;
	float f4;
	float f5;
	double d1;
	double d2;
	double d3;
	double d4;
	double d5;
	int *p1;
	int *p2;
	int *p3;
	int *p4;
	int *p5;
	void *v1;
	void *v2;
	void *v3;
	void *v4;
	void *v5;
	char arr1[4];
	char arr2[4];
	char arr3[4];
	char arr4[4];
	char arr5[4];

	struct {
		int x;
	} st1;

	struct {
		int x;
	} st2;

	struct {
		int x;
	} st3;

	CHECK(c1 == 0 && c2 == 0 && c3 == 0 && c4 == 0 && c5 == 0, "torture stress: chars");
	CHECK(s1 == 0 && s2 == 0 && s3 == 0 && s4 == 0 && s5 == 0, "torture stress: shorts");
	CHECK(i1 == 0 && i2 == 0 && i3 == 0 && i4 == 0 && i5 == 0, "torture stress: ints");
	CHECK(l1 == 0 && l2 == 0 && l3 == 0 && l4 == 0 && l5 == 0, "torture stress: longs");
	CHECK(f1 == 0.0f && f2 == 0.0f && f3 == 0.0f && f4 == 0.0f && f5 == 0.0f, "torture stress: floats");
	CHECK(d1 == 0.0 && d2 == 0.0 && d3 == 0.0 && d4 == 0.0 && d5 == 0.0, "torture stress: doubles");
	CHECK(p1 == NULL && p2 == NULL && p3 == NULL && p4 == NULL && p5 == NULL, "torture stress: int ptrs");
	CHECK(v1 == NULL && v2 == NULL && v3 == NULL && v4 == NULL && v5 == NULL,
	      "torture stress: void ptrs");
	CHECK(arr1[0] == 0 && arr2[0] == 0 && arr3[0] == 0 && arr4[0] == 0 && arr5[0] == 0,
	      "torture stress: arrays");
	CHECK(st1.x == 0 && st2.x == 0 && st3.x == 0, "torture stress: structs");
}

void test_zeroinit_torture_with_defer(void) {
	int cleanup_order = 0;

	{
		int a, b, c; // All zero-init
		defer cleanup_order |= 1;
		int d, e, f; // All zero-init
		defer cleanup_order |= 2;
		int g, h, i; // All zero-init
		defer cleanup_order |= 4;

		CHECK(a == 0 && b == 0 && c == 0, "torture defer: a,b,c zero");
		CHECK(d == 0 && e == 0 && f == 0, "torture defer: d,e,f zero");
		CHECK(g == 0 && h == 0 && i == 0, "torture defer: g,h,i zero");
	}
	CHECK(cleanup_order == 7, "torture defer: all defers ran");

	// Zero-init in defer target scope
	int final_value = 0;
	{
		int uninit_in_defer_scope;
		defer final_value = uninit_in_defer_scope + 1;
	}
	CHECK_EQ(final_value, 1, "torture defer: zero-init used in defer");
}

#include <stdatomic.h>

void test_zeroinit_torture_atomic(void) {
	// _Atomic pointer to pointer
	_Atomic(int **) atomic_pp;
	int **pp_val = atomic_load(&atomic_pp);
	CHECK(pp_val == NULL, "torture: _Atomic(int**)");

	// _Atomic function pointer
	_Atomic(int (*)(void)) atomic_fp;
	int (*fp_val)(void) = atomic_load(&atomic_fp);
	CHECK(fp_val == NULL, "torture: _Atomic func ptr");

	// _Atomic in multi-decl
	_Atomic int a1, a2, a3;
	CHECK(atomic_load(&a1) == 0 && atomic_load(&a2) == 0 && atomic_load(&a3) == 0,
	      "torture: _Atomic multi-decl");
}


void test_ternary_zeroinit(void) {
        log_reset();
        int cond = 0;
        int my_val_t = 5; 
        int b = 2;
        
        // BUG: Prism sees ':' as a structural token (statement start).
        // It then sees 'my_val_t' (which triggers the typedef heuristic because of '_t')
        // followed by '* b'. It mistakenly rewrites this into a zero-initialized declaration:
        // cond ? 0 : my_val_t * b = 0;
        // This mutates 'b' to 0 and returns 0!
        int result = cond ? 0 : my_val_t * b;
        
        CHECK_EQ(result, 10, "ternary colon safely ignored without mutating variables");
        CHECK_EQ(b, 2, "variable 'b' was not mutated by rogue zero-init");
}

#define REPEAT_64(x) x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x
#define MASSIVE_STRING REPEAT_64(REPEAT_64("A")) // Creates a massive token

void test_buffer_boundary_hang(void) {
        log_reset();
        const char *huge = MASSIVE_STRING;
        CHECK(huge[0] == 'A', "transpiler survived massive buffer boundary flush");
}


typedef int GlobalEnumShadowType;

void test_enum_shadow_zeroinit(void) {
	// Enum constant shadows the global typedef
	enum { GlobalEnumShadowType = 5 };

	// Array size is compile-time constant (enum), so should be zero-initialized
	int arr[GlobalEnumShadowType];
	int sum = 0;
	for (int i = 0; i < GlobalEnumShadowType; i++) sum += arr[i];
	CHECK_EQ(sum, 0, "enum constant shadowing typedef: array zero-initialized");
}

int _ternary_result_global; // global forces bare assignment (not a declaration)

void test_ternary_bare_expr_in_defer(void) {
	int my_val_t = 5;
	int b = 2;

	// Bare assignment with ternary inside a defer scope.
	// Before fix: Prism rewrites to `my_val_t * b = 0`, mutating b.
	{
		defer (void)0;
		_ternary_result_global = 0 ? 0 : my_val_t * b;
	}
	CHECK_EQ(_ternary_result_global, 10, "bare ternary in defer: correct value");
	CHECK_EQ(b, 2, "bare ternary in defer: b not mutated");

	// Nested ternary: two colons in a bare assignment
	{
		defer (void)0;
		_ternary_result_global = 1 ? (0 ? 10 : my_val_t * b) : 99;
	}
	CHECK_EQ(_ternary_result_global, 10, "nested bare ternary in defer: correct");
	CHECK_EQ(b, 2, "nested bare ternary in defer: b still intact");
}

void test_nested_ternary_zeroinit(void) {
	// Declaration-based ternary tests (go through emit_expr_to_semicolon
	// which has its own ternary_depth tracking).
	int a_val_t = 3;
	int b = 4;
	int r1 = 1 ? (0 ? 10 : a_val_t * b) : 99;
	CHECK_EQ(r1, 12, "decl ternary: nested ternary correct");
	CHECK_EQ(b, 4, "decl ternary: b not mutated");
}

void test_inline_enum_const_array_size(void) {
	// Inline enum with explicit value, then use the constant as an array size
	enum { INLINE_SIZE = 8 } tag;
	int arr[INLINE_SIZE];

	// If the constant wasn't registered, Prism would think INLINE_SIZE is a
	// variable, treat arr as a VLA, and skip = {0}.
	int sum = 0;
	for (int i = 0; i < INLINE_SIZE; i++) sum += arr[i];
	CHECK_EQ(sum, 0, "inline enum const as array size: zero-initialized");
	CHECK_EQ((int)tag, 0, "inline enum var itself is zero-initialized");
}

void test_zeroinit_parenthesized_declarator(void) {
	int (x);
	CHECK_EQ(x, 0, "parenthesized declarator int (x) zero-init");
}

void test_zeroinit_parenthesized_array(void) {
	int (arr[5]);
	int all_zero = 1;
	for (int i = 0; i < 5; i++)
		if (arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "parenthesized array int (arr[5]) zero-init");
}

void test_parenthesized_vla_zeroinit(void) {
	int x = 5;
	int (arr)[x];
	int all_zero = 1;
	for (int i = 0; i < x; i++)
		if (arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "parenthesized VLA int (arr)[x] zero-init");
}

void test_typeof_fnptr_vla_param_zeroinit(void) {
#ifdef __GNUC__
	int x = 5;
	typeof( void (*)(int[x]) ) fn_ptr;
	CHECK(fn_ptr == NULL, "typeof fn-ptr with VLA param zero-init");
#endif
}

void test_typeof_register_fnptr_vla_param(void) {
#ifdef __GNUC__
	int x = 5;
	register typeof( void (*)(int[x]) ) fn_ptr;
	(void)sizeof(fn_ptr);

	PrismResult transpile_result = prism_transpile_source(
	    "void f(int x) {\n"
	    "    register typeof(void (*)(int[x])) fn_ptr;\n"
	    "    (void)sizeof(fn_ptr);\n"
	    "}\n",
	    "register_fnptr_vla.c", prism_defaults());
	CHECK_EQ(transpile_result.status, PRISM_OK, "register typeof fn-ptr with VLA param transpiles");
	if (transpile_result.output) {
		CHECK(strstr(transpile_result.output, "register typeof(void (*)(int[x])) fn_ptr = {0};") != NULL,
		      "register typeof fn-ptr with VLA param: brace zero-init preserved");
		CHECK(strstr(transpile_result.output, "memset(&fn_ptr") == NULL,
		      "register typeof fn-ptr with VLA param: no illegal register memset");
	}
	prism_free(&transpile_result);
#endif
}

void test_sizeof_inline_enum_not_vla(void) {
	int arr[ sizeof(enum { MY_CONST = 5 }) ];
	int all_zero = 1;
	for (unsigned i = 0; i < sizeof(arr)/sizeof(int); i++)
		if (arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof inline enum: not VLA");
}


void test_typeof_register_vla_bug(void) {
#ifdef __GNUC__
	int n = 10;
	register typeof(int[n]) val;
	(void)sizeof(val);

	PrismResult transpile_result = prism_transpile_source(
	    "void f(int n) {\n"
	    "    register typeof(int[n]) val;\n"
	    "    (void)sizeof(val);\n"
	    "}\n",
	    "register_typeof_vla.c", prism_defaults());
	CHECK_EQ(transpile_result.status, PRISM_OK, "register typeof VLA transpiles");
	if (transpile_result.output) {
		CHECK(strstr(transpile_result.output, "register typeof(int[n]) val;") != NULL,
		      "register typeof VLA: declaration preserved");
		CHECK(strstr(transpile_result.output, "memset(&val") == NULL,
		      "register typeof VLA: no illegal register memset");
		CHECK(strstr(transpile_result.output, "val = {0}") == NULL,
		      "register typeof VLA: no bogus brace init emitted");
	}
	prism_free(&transpile_result);
#endif
}

void test_atomic_register_struct_bug(void) {
#if !defined(__TINYC__)
	register _Atomic struct S_bug { int x, y; } s1;
	(void)s1;

	PrismResult transpile_result = prism_transpile_source(
	    "void f(void) {\n"
	    "    register _Atomic struct S_bug { int x, y; } s1;\n"
	    "    (void)s1;\n"
	    "}\n",
	    "register_atomic_struct.c", prism_defaults());
	CHECK_EQ(transpile_result.status, PRISM_OK, "register _Atomic struct transpiles");
	if (transpile_result.output) {
		CHECK(strstr(transpile_result.output, "register _Atomic struct S_bug { int x, y; } s1;") != NULL,
		      "register _Atomic struct: declaration preserved");
		CHECK(strstr(transpile_result.output, "memset(&s1") == NULL,
		      "register _Atomic struct: no illegal register memset");
		CHECK(strstr(transpile_result.output, "s1 = {0}") == NULL,
		      "register _Atomic struct: no bogus brace init emitted");
	}
	prism_free(&transpile_result);
#endif
}

void test_const_typeof_vla_bug(void) {
#ifdef __GNUC__
	int n = 10;
	int vla[n];
	const typeof(vla) val;
	(void)val;

	PrismResult transpile_result = prism_transpile_source(
	    "void f(int n) {\n"
	    "    int vla[n];\n"
	    "    const typeof(vla) val;\n"
	    "    (void)val;\n"
	    "}\n",
	    "const_typeof_vla.c", prism_defaults());
	CHECK_EQ(transpile_result.status, PRISM_OK, "const typeof VLA transpiles");
	if (transpile_result.output) {
		CHECK(strstr(transpile_result.output, "int vla[n]; memset(&vla, 0, sizeof(vla));") != NULL,
		      "const typeof VLA: VLA source gets memset");
		CHECK(strstr(transpile_result.output, "const typeof(vla) val; memset((void *)&val, 0, sizeof(val));") != NULL,
		      "const typeof VLA: const array gets casted memset");
		CHECK(strstr(transpile_result.output, "val = {0}") == NULL,
		      "const typeof VLA: no illegal brace init emitted");
	}
	prism_free(&transpile_result);
#endif
}

void test_const_typeof_atomic_struct_bug(void) {
#if !defined(__TINYC__)
	_Atomic struct S2_bug { int x, y; } s1;
	const typeof(s1) s2;
	(void)s1; (void)s2;

	PrismResult transpile_result = prism_transpile_source(
	    "void f(void) {\n"
	    "    _Atomic struct S2_bug { int x, y; } s1;\n"
	    "    const typeof(s1) s2;\n"
	    "    (void)s1; (void)s2;\n"
	    "}\n",
	    "const_typeof_atomic_struct.c", prism_defaults());
	CHECK_EQ(transpile_result.status, PRISM_OK, "const typeof _Atomic struct transpiles");
	if (transpile_result.output) {
		CHECK(strstr(transpile_result.output, "_Atomic struct S2_bug { int x, y; } s1; memset(&s1, 0, sizeof(s1));") != NULL,
		      "const typeof _Atomic struct: source object gets memset");
		CHECK(strstr(transpile_result.output, "const typeof(s1) s2; memset((void *)&s2, 0, sizeof(s2));") != NULL,
		      "const typeof _Atomic struct: const copy gets casted memset");
		CHECK(strstr(transpile_result.output, "s2 = {0}") == NULL,
		      "const typeof _Atomic struct: no bogus brace init emitted");
	}
	prism_free(&transpile_result);
#endif
}

static void test_typeof_memset_split_before_initializer(void) {
	printf("\n--- Typeof Memset Split Before Initializer ---\n");

	PrismResult transpile_result = prism_transpile_source(
	    "struct S { int x; };\n"
	    "void f(void) {\n"
	    "    typeof((struct S){1}) a, b = a;\n"
	    "    (void)b;\n"
	    "}\n",
	    "typeof_memset_split.c", prism_defaults());
	CHECK_EQ(transpile_result.status, PRISM_OK, "typeof memset split: transpiles");
	if (transpile_result.output) {
		CHECK(strstr(transpile_result.output,
			     "typeof((struct S){1}) a; memset(&a, 0, sizeof(a));") != NULL,
		      "typeof memset split: first declarator split and zeroed eagerly");
		CHECK(strstr(transpile_result.output, "typeof((struct S){1}) b = a;") != NULL,
		      "typeof memset split: preserved compound literal type for later declarator");
		CHECK(strstr(transpile_result.output, "b = a;") != NULL,
		      "typeof memset split: later initializer still reads prior variable");
		CHECK(strstr(transpile_result.output, "b = a; memset(&a, 0, sizeof(a));") == NULL,
		      "typeof memset split: no late memset after dependent initializer");
		CHECK(strstr(transpile_result.output, "typeof((struct S)) b = a;") == NULL,
		      "typeof memset split: no broken cast-like type re-emission");
	}
	prism_free(&transpile_result);
}

static void test_typeof_memset_queue_over_128(void) {
	printf("\n--- Typeof Memset Queue Over 128 ---\n");

	char *code = malloc(8192);
	CHECK(code != NULL, "typeof memset queue: allocate source buffer");
	if (!code) return;

	size_t off = 0;
	off += snprintf(code + off, 8192 - off, "void f(int n) {\n    typeof(int[n]) ");
	for (int i = 0; i < 129 && off < 8192; i++)
		off += snprintf(code + off, 8192 - off, "a%d%s", i, (i == 128) ? ";\n" : ", ");
	off += snprintf(code + off, 8192 - off, "    (void)a0;\n    (void)a128;\n}\n");

	PrismResult transpile_result = prism_transpile_source(
	    code, "typeof_memset_queue_129.c", prism_defaults());
	CHECK_EQ(transpile_result.status, PRISM_OK, "typeof memset queue: transpiles");
	if (transpile_result.output) {
		CHECK(strstr(transpile_result.output, "memset(&a127, 0, sizeof(a127));") != NULL,
		      "typeof memset queue: penultimate queued var still zeroed");
		CHECK(strstr(transpile_result.output, "memset(&a128, 0, sizeof(a128));") != NULL,
		      "typeof memset queue: 129th var still zeroed");
	}
	prism_free(&transpile_result);
	free(code);
}

static void test_sizeof_unparenthesized_not_vla(void) {
	// sizeof without parentheses should not trigger VLA detection.
	// If it does, the transpiler emits memset instead of = {0}.
	int my_var;
	int arr[sizeof my_var];
	int all_zero = 1;
	for (unsigned i = 0; i < sizeof my_var / sizeof(int); i++)
		if (arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof unparenthesized: array zero-initialized");
	CHECK(sizeof(arr) == sizeof(int) * sizeof my_var,
	      "sizeof unparenthesized: correct size");
}

static void test_sizeof_unary_prefix_not_vla(void) {
	// sizeof with unary +/- prefix should not trigger VLA detection.
	// e.g. int arr[sizeof -x] is NOT a VLA.
	int x = 42;
	int arr_neg[sizeof -x];
	int arr_pos[sizeof +x];
	int all_zero = 1;
	for (unsigned i = 0; i < sizeof arr_neg / sizeof(int); i++)
		if (arr_neg[i] != 0) all_zero = 0;
	for (unsigned i = 0; i < sizeof arr_pos / sizeof(int); i++)
		if (arr_pos[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof unary prefix: arrays zero-initialized");
	CHECK(sizeof(arr_neg) == sizeof(int) * sizeof(-x),
	      "sizeof unary prefix neg: correct size");
	CHECK(sizeof(arr_pos) == sizeof(int) * sizeof(+x),
	      "sizeof unary prefix pos: correct size");
}


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

static void test_vla_sizeof_pointer_deref(void) {
	printf("\n--- VLA sizeof Pointer Deref ---\n");

	const char *code =
	    "void f(int n, int (*vla_ptr)[n]) {\n"
	    "    int arr[sizeof(*vla_ptr) / sizeof(int)];\n"
	    "    arr[0] = 1;\n"
	    "    (void)arr;\n"
	    "}\n"
	    "int main(void) { f(5, 0); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "vla sizeof ptr: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "vla sizeof ptr: transpiles OK");
	CHECK(result.output != NULL, "vla sizeof ptr: output not NULL");

	// arr is a VLA (its size depends on sizeof(*vla_ptr) which is runtime-dependent).
	// It must use memset, not = {0}.
	CHECK(strstr(result.output, "memset") != NULL,
	      "vla sizeof ptr: VLA array should use memset (sizeof of VLA pointer deref is runtime)");
	CHECK(strstr(result.output, "= {0}") == NULL,
	      "vla sizeof ptr: should NOT use = {0} (would be compile error on VLA)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_vla_sizeof_paren_bracket(void) {
	printf("\n--- VLA sizeof Detection: Parenthesized Type Before Bracket ---\n");

	const char *code =
	    "#include <stdlib.h>\n"
	    "void f(int n) {\n"
	    "    int arr[sizeof(typeof(int)[n]) / sizeof(int)];\n"
	    "    arr[0] = 1;\n"
	    "    (void)arr;\n"
	    "}\n"
	    "int main(void) { f(5); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "vla sizeof paren: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "vla sizeof paren: transpiles OK");
	CHECK(result.output != NULL, "vla sizeof paren: output not NULL");

	// sizeof(typeof(int)[n]) is runtime-dependent — the array must use memset.
	CHECK(strstr(result.output, "memset") != NULL,
	      "vla sizeof paren: VLA array should use memset (sizeof(typeof(int)[n]) is runtime)");
	CHECK(strstr(result.output, "= {0}") == NULL,
	      "vla sizeof paren: should NOT use = {0} (would be compile error on VLA)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_paren_pointer_vla_masking(void) {
	printf("\n--- Paren-Pointer VLA Masking Triggers Illegal = {0} ---\n");

	const char *code =
	    "void f(int m, int n) {\n"
	    "    int (*p[m])[n];\n"
	    "    (void)p;\n"
	    "}\n"
	    "int main(void) { f(3, 4); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "paren VLA: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "paren VLA: transpiles OK");
	CHECK(result.output != NULL, "paren VLA: output not NULL");

	// int (*p[m])[n] is a VLA — must use memset, not = {0}
	CHECK(strstr(result.output, "memset") != NULL,
	      "paren VLA: VLA of pointers should use memset");
	CHECK(strstr(result.output, "= {0}") == NULL,
	      "paren VLA: should NOT use = {0} (would be compile error on VLA)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_func_returned_vla_sizeof(void) {
	printf("\n--- Function-Returned VLA Pointer sizeof Blind Spot ---\n");

	const char *code =
	    "#include <stdlib.h>\n"
	    "typedef int (*vla_ptr_t)[1];\n"
	    "vla_ptr_t get_vla(int n);\n"
	    "void f(int n) {\n"
	    "    int arr[sizeof(*get_vla(n)) / sizeof(int)];\n"
	    "    arr[0] = 1;\n"
	    "    (void)arr;\n"
	    "}\n"
	    "int main(void) { f(5); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "func VLA sizeof: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "func VLA sizeof: transpiles OK");
	CHECK(result.output != NULL, "func VLA sizeof: output not NULL");

	// sizeof(*get_vla(n)) might be runtime-dependent — must use memset
	CHECK(strstr(result.output, "memset") != NULL,
	      "func VLA sizeof: should use memset (sizeof of dereferenced func call with runtime arg)");
	CHECK(strstr(result.output, "= {0}") == NULL,
	      "func VLA sizeof: should NOT use = {0} (potentially VLA)");

	prism_free(&result);
	unlink(path);
	free(path);
}

void run_zeroinit_tests(void) {

	printf("\n=== ZERO-INIT TESTS ===\n");

	/* Basic zero-init */
	test_zeroinit_basic_types();
	test_zeroinit_pointers();
	test_zeroinit_arrays();
	test_zeroinit_structs();
	test_zeroinit_qualifiers();
	test_zeroinit_in_scopes();
	test_zeroinit_with_defer();
#ifdef __GNUC__
	test_zeroinit_typeof();
#endif
	test_zeroinit_enum_array_size();
	test_zeroinit_alignas_array();
	test_zeroinit_union();
	test_ternary_zeroinit();
	test_buffer_boundary_hang();

	/* Torture tests */
	test_zeroinit_torture_declarators();
	test_zeroinit_torture_attributes();
	test_zeroinit_torture_partial_init();
#ifdef __GNUC__
	test_zeroinit_torture_stmt_expr();
#endif
	test_zeroinit_torture_deep_nesting();
	test_zeroinit_torture_bitfields();
	test_zeroinit_torture_anonymous();
	test_zeroinit_torture_compound_literals();
	test_zeroinit_torture_fam_adjacent();
	test_zeroinit_torture_long_multidecl();
	test_zeroinit_torture_control_flow();
	test_zeroinit_torture_stress();
	test_zeroinit_torture_with_defer();
	test_zeroinit_torture_atomic();

	/* Typeof zero-init torture */
#ifdef __GNUC__
	test_typeof_zeroinit_all_basic_types();
	test_typeof_zeroinit_structs();
	test_typeof_zeroinit_arrays();
	test_typeof_zeroinit_qualifiers();
	test_typeof_zeroinit_complex_exprs();
	test_typeof_zeroinit_vla();
	test_typeof_zeroinit_function_ptrs();
	test_typeof_zeroinit_multi_decl_complex();
	test_typeof_zeroinit_in_scopes();
	test_typeof_zeroinit_with_defer();
	test_typeof_zeroinit_unions();
	test_typeof_zeroinit_edge_cases();
	test_typeof_zeroinit_torture_stress();
#endif

	/* Enum shadow */
	test_enum_shadow_zeroinit();
	test_ternary_bare_expr_in_defer();
	test_nested_ternary_zeroinit();
	test_inline_enum_const_array_size();

	test_zeroinit_parenthesized_declarator();
	test_zeroinit_parenthesized_array();

	test_parenthesized_vla_zeroinit();
	test_typeof_fnptr_vla_param_zeroinit();
	test_typeof_register_fnptr_vla_param();

	test_sizeof_inline_enum_not_vla();

	test_typeof_register_vla_bug();
	test_atomic_register_struct_bug();
	test_const_typeof_vla_bug();
	test_const_typeof_atomic_struct_bug();
	test_typeof_memset_split_before_initializer();
	test_typeof_memset_queue_over_128();

	test_sizeof_unparenthesized_not_vla();
	test_sizeof_unary_prefix_not_vla();
	test_vla_pessimization_inline_enum_in_sizeof();
	test_prism_p_temp_var_namespace_collision();
	test_vla_pointer_array_no_init();
	test_stmt_expr_initializer_features();
	test_vla_sizeof_pointer_deref();
	test_vla_sizeof_paren_bracket();
	test_paren_pointer_vla_masking();
	test_func_returned_vla_sizeof();
}

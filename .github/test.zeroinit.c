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

#ifndef _MSC_VER
	_Alignas(16) int aligned;
	CHECK_EQ(aligned, 0, "_Alignas zero-init");
#endif

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

#ifndef _MSC_VER
void test_zeroinit_alignas_array(void) {
	_Alignas(32) int arr[8];
	int all_zero = 1;
	for (int i = 0; i < 8; i++)
		if (arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "_Alignas array zero-init");
}
#endif

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

#ifndef _MSC_VER
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
#endif


void test_ternary_zeroinit(void) {
        log_reset();
        int cond = 0;
        int my_val_t = 5; 
        int b = 2;
        
        // Prism sees ':' as a structural token (statement start).
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

#ifndef _MSC_VER
void test_parenthesized_vla_zeroinit(void) {
	int x = 5;
	int (arr)[x];
	int all_zero = 1;
	for (int i = 0; i < x; i++)
		if (arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "parenthesized VLA int (arr)[x] zero-init");
}
#endif

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
	raw register typeof(int[n]) val;
	(void)sizeof(val);

	PrismResult transpile_result = prism_transpile_source(
	    "void f(int n) {\n"
	    "    register typeof(int[n]) val;\n"
	    "    (void)sizeof(val);\n"
	    "}\n",
	    "register_typeof_vla.c", prism_defaults());
	CHECK(transpile_result.status != PRISM_OK, "register typeof VLA: must be rejected");
	if (transpile_result.error_msg)
		CHECK(strstr(transpile_result.error_msg, "register") != NULL,
		      "register typeof VLA: error mentions register");
	prism_free(&transpile_result);
#endif
}

#ifndef _MSC_VER
void test_atomic_register_struct_bug(void) {
#if !defined(__TINYC__)
	/* register + _Atomic aggregate must be rejected, not silently uninitialized */
	PrismResult transpile_result = prism_transpile_source(
	    "void f(void) {\n"
	    "    register _Atomic struct S_bug { int x, y; } s1;\n"
	    "    (void)s1;\n"
	    "}\n",
	    "register_atomic_struct.c", prism_defaults());
	CHECK(transpile_result.status != PRISM_OK,
	      "register _Atomic struct: must be rejected");
	prism_free(&transpile_result);
#endif
}
#endif

void test_const_typeof_vla_bug(void) {
#ifdef __GNUC__
	int n = 10;
	int vla[n];
	raw const typeof(vla) val;
	(void)val;

	PrismResult transpile_result = prism_transpile_source(
	    "void f(int n) {\n"
	    "    int vla[n];\n"
	    "    const typeof(vla) val;\n"
	    "    (void)val;\n"
	    "}\n",
	    "const_typeof_vla.c", prism_defaults());
	CHECK(transpile_result.status != PRISM_OK,
	      "const typeof VLA: must be rejected (memset on const is UB)");
	if (transpile_result.error_msg)
		CHECK(strstr(transpile_result.error_msg, "const") != NULL,
		      "const typeof VLA: error mentions const");
	prism_free(&transpile_result);
#endif
}

#ifdef __GNUC__
void test_typeof_const_ptr_to_vla_ok(void) {
	/* Pointer-to-VLA typeof is variably-modified but not an object VLA —
	 * zero-init with = 0 must not trip the const + memset rejection. */
	PrismResult r = prism_transpile_source(
	    "void f(int n) {\n"
	    "    const typeof(int (*)[n]) ptr;\n"
	    "    (void)sizeof(ptr);\n"
	    "}\n",
	    "const_ptr_to_vla_ok.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	      "const typeof ptr-to-VLA: standard code transpiles");
	prism_free(&r);
}

void test_struct_tag_shadow_volatile_byte_loop(void) {
	PrismResult r = prism_transpile_source(
	    "struct HwReg { volatile int control; };\n"
	    "void f(void) {\n"
	    "    int HwReg = 0;\n"
	    "    struct W { struct HwReg reg; };\n"
	    "    typeof(struct W) config;\n"
	    "    (void)sizeof(config);\n"
	    "}\n",
	    "tag_shadow_volatile.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	      "struct tag shadow + volatile member: transpiles");
	if (r.output)
		CHECK(strstr(r.output, "volatile char *__prism_p_") != NULL,
		      "tag shadow: volatile-safe byte loop (not plain memset)");
	prism_free(&r);
}
#endif

void test_typedef_typeof_vla_zeroinit(void) {
#ifdef __GNUC__
	// typedef typeof(char[n]) should be recognized as VLA and get memset
	PrismResult r = prism_transpile_source(
	    "void f(int n) {\n"
	    "    typedef typeof(char[n]) dyn_buf_t;\n"
	    "    dyn_buf_t buf;\n"
	    "    (void)buf;\n"
	    "}\n",
	    "typedef_typeof_vla.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "typedef typeof(char[n]) transpiles");
	if (r.output) {
		CHECK(strstr(r.output, "memset") != NULL,
		      "typedef typeof VLA: gets memset (not brace init)");
		CHECK(strstr(r.output, "buf = {0}") == NULL,
		      "typedef typeof VLA: no illegal brace init");
	}
	prism_free(&r);
#endif
}

#ifndef _MSC_VER
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
		CHECK(strstr(transpile_result.output, "_Atomic struct S2_bug { int x, y; } s1; __builtin_memset(&s1, 0, sizeof(s1));") != NULL,
		      "const typeof _Atomic struct: source object gets memset");
		CHECK(strstr(transpile_result.output, "const typeof(s1) s2; __builtin_memset((void *)&s2, 0, sizeof(s2));") != NULL,
		      "const typeof _Atomic struct: const copy gets casted memset");
		CHECK(strstr(transpile_result.output, "s2 = {0}") == NULL,
		      "const typeof _Atomic struct: no bogus brace init emitted");
	}
	prism_free(&transpile_result);
#endif
}
#endif

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
			     "typeof((struct S){1}) a; __builtin_memset(&a, 0, sizeof(a));") != NULL ||
		      has_var_zeroing(transpile_result.output, "a"),
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

static void test_typeof_vla_split_double_eval(void) {
	printf("\n--- Typeof VLA Split Double Evaluation ---\n");

	/* typeof(int[side_effect()]) arr, *p = 0;
	 * Splitting re-emits the type specifier, causing the VLA dimension
	 * (with side effects) to be evaluated twice (ISO C11 §6.7.2.5).
	 * Prism must reject this with a clear error. */

	/* Case 1: typeof VLA + memset split (typeof_var_count > 0, next has init) */
	PrismResult r1 = prism_transpile_source(
	    "void f(void) {\n"
	    "    typeof(int[get_size()]) arr, *p = 0;\n"
	    "    (void)arr; (void)p;\n"
	    "}\n",
	    "typeof_vla_split1.c", prism_defaults());
	CHECK(r1.status != PRISM_OK, "typeof VLA memset split: must be rejected");
	if (r1.error_msg)
		CHECK(strstr(r1.error_msg, "variably") != NULL || strstr(r1.error_msg, "VLA") != NULL,
		      "typeof VLA memset split: error mentions variably-modified / VLA");
	prism_free(&r1);

	/* Case 2: typeof VLA + bracket orelse split */
	PrismResult r2 = prism_transpile_source(
	    "void f(void) {\n"
	    "    typeof(int[get_size()]) arr, b[3 orelse 1];\n"
	    "    (void)arr; (void)b;\n"
	    "}\n",
	    "typeof_vla_split2.c", prism_defaults());
	CHECK(r2.status != PRISM_OK, "typeof VLA bracket orelse split: must be rejected");
	if (r2.error_msg)
		CHECK(strstr(r2.error_msg, "variably") != NULL || strstr(r2.error_msg, "VLA") != NULL,
		      "typeof VLA bracket orelse split: error mentions variably-modified / VLA");
	prism_free(&r2);

	/* Case 3: non-VLA typeof must NOT be rejected (no side effects in type spec) */
	PrismResult r3 = prism_transpile_source(
	    "struct S { int x; };\n"
	    "void f(void) {\n"
	    "    typeof((struct S){1}) a, b = a;\n"
	    "    (void)b;\n"
	    "}\n",
	    "typeof_nonvla_split.c", prism_defaults());
	CHECK(r3.status == PRISM_OK, "non-VLA typeof split: must be accepted");
	prism_free(&r3);

	/* Case 4: plain VLA (no typeof) must NOT be rejected (type spec is just 'int') */
	PrismResult r4 = prism_transpile_source(
	    "void f(int n) {\n"
	    "    int arr[n], *p = 0;\n"
	    "    (void)arr; (void)p;\n"
	    "}\n",
	    "plain_vla_split.c", prism_defaults());
	CHECK(r4.status == PRISM_OK, "plain VLA split: must be accepted");
	prism_free(&r4);
}

static void test_vla_multi_decl_sequence_point(void) {
	printf("\n--- VLA multi-decl sequence point split ---\n");

	/* int arr[n], matrix[arr[0]][n]; — arr queued for memset but
	 * matrix's VLA dim arr[0] evaluated before the memset runs.
	 * Fix: should_split_multi_decl splits when next decl is VLA. */
	PrismResult r = prism_transpile_source(
		"void f(int n) {\n"
		"    int arr[n], matrix[arr[0]][n];\n"
		"    (void)arr; (void)matrix;\n"
		"}\n",
		"vla_seq.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "vla-seq: transpile succeeds");
	if (r.output) {
		/* arr must be on its own statement, memset'd before matrix */
		char *arr_decl = strstr(r.output, "int arr[n]");
		char *matrix_decl = strstr(r.output, "matrix[");
		CHECK(arr_decl != NULL, "vla-seq: arr declared");
		CHECK(matrix_decl != NULL, "vla-seq: matrix declared");
		CHECK(has_zeroing(r.output), "vla-seq: memset present");
		char *zero_call = strstr(r.output, "memset");
		if (!zero_call) zero_call = strstr(r.output, "__prism_p_");
		if (arr_decl && matrix_decl && zero_call)
			CHECK(zero_call < matrix_decl,
			      "vla-seq: memset(arr) before matrix declaration");
	}
	prism_free(&r);
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
		CHECK(strstr(transpile_result.output, "memset(&a127, 0, sizeof(a127));") != NULL ||
		      has_var_zeroing(transpile_result.output, "a127"),
		      "typeof memset queue: penultimate queued var still zeroed");
		CHECK(strstr(transpile_result.output, "memset(&a128, 0, sizeof(a128));") != NULL ||
		      has_var_zeroing(transpile_result.output, "a128"),
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

	// After fix: generated temps use reserved __prism_p_ prefix, not _prism_p_.
	// User's _prism_p_0 should coexist with generated __prism_p_0.
	CHECK(strstr(result.output, "__prism_p_") != NULL || strstr(result.output, "__prism_i_") != NULL,
	      "prism_p collision: transpiler should use reserved __prism_ prefix for volatile memset temps");
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
	CHECK(has_zeroing(result.output),
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
	CHECK(has_zeroing(result.output),
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
	CHECK(has_zeroing(result.output),
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
	CHECK(has_zeroing(result.output),
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
	CHECK(has_zeroing(result.output),
	      "func VLA sizeof: should use memset (sizeof of dereferenced func call with runtime arg)");
	CHECK(strstr(result.output, "= {0}") == NULL,
	      "func VLA sizeof: should NOT use = {0} (potentially VLA)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_vla_typedef_struct_tag_memset(void) {
	/* struct S { vlax buf; }; struct S s; emitted = {0} instead of memset
	 * because struct_body_contains_vla only scanned inline bodies.
	 * When struct S is referenced by tag without a body, the VLA info was lost. */

	/* Separate definition and variable declaration */
	const char *code1 =
	    "void f(int n) {\n"
	    "    typedef int vlax[n];\n"
	    "    struct S { vlax buf; };\n"
	    "    struct S s;\n"
	    "}\n";
	PrismResult r1 = prism_transpile_source(code1, "vla_td_struct.c", prism_defaults());
	CHECK(r1.status == PRISM_OK && r1.output,
	      "vla-typedef-struct-tag: transpiles OK");
	if (r1.output) {
		CHECK(has_zeroing(r1.output),
		      "vla-typedef-struct-tag: separate decl uses memset");
		CHECK(!strstr(r1.output, "struct S s = {0}"),
		      "vla-typedef-struct-tag: no = {0} for VLA struct");
	}
	prism_free(&r1);

	/* Inline definition (should still work too) */
	const char *code2 =
	    "void g(int n) {\n"
	    "    typedef int vlax[n];\n"
	    "    struct S { vlax buf; } s;\n"
	    "}\n";
	PrismResult r2 = prism_transpile_source(code2, "vla_td_inline.c", prism_defaults());
	CHECK(r2.status == PRISM_OK && r2.output,
	      "vla-typedef-struct-inline: transpiles OK");
	if (r2.output) {
		CHECK(has_zeroing(r2.output),
		      "vla-typedef-struct-inline: inline decl uses memset");
	}
	prism_free(&r2);
}

static void test_c23_if_initializer_zeroinit_dropped(void) {
	/* FIX: Prism now treats if/switch opening parens like for() parens
	 * so that C23 initializer declarations get proper zero-initialization.
	 * Scalars get = 0, VLAs get an error (same as for-init). */

	/* --- VLA inside if-initializer: rejected (can't inject memset) --- */
	const char *vla_code =
	    "void f(int n) {\n"
	    "    if (int arr[n]; n > 0) { (void)arr; }\n"
	    "}\n";
	PrismResult r1 = prism_transpile_source(vla_code, "c23_if_vla.c", prism_defaults());
	CHECK(r1.status != PRISM_OK,
	      "c23-if-vla-zeroinit: VLA in if-initializer must be rejected");
	prism_free(&r1);

	/* --- Scalar inside if-initializer: gets = 0 --- */
	const char *scalar_code =
	    "void g(void) {\n"
	    "    if (int x; 1) { (void)x; }\n"
	    "}\n";
	PrismResult r2 = prism_transpile_source(scalar_code, "c23_if_scalar.c", prism_defaults());
	CHECK(r2.status == PRISM_OK && r2.output,
	      "c23-if-scalar-zeroinit: if-initializer scalar compiles");
	if (r2.output) {
		CHECK(strstr(r2.output, "int x = 0") || strstr(r2.output, "int x =0"),
		      "c23-if-scalar-zeroinit: scalar in if-initializer gets = 0");
	}
	prism_free(&r2);

	/* --- Scalar inside switch-initializer: gets = 0 --- */
	const char *switch_code =
	    "void h(void) {\n"
	    "    switch (int x; x) { default: (void)x; break; }\n"
	    "}\n";
	PrismResult r3 = prism_transpile_source(switch_code, "c23_switch_scalar.c", prism_defaults());
	CHECK(r3.status == PRISM_OK && r3.output,
	      "c23-switch-scalar-zeroinit: switch-initializer scalar compiles");
	if (r3.output) {
		CHECK(strstr(r3.output, "int x = 0") || strstr(r3.output, "int x =0"),
		      "c23-switch-scalar-zeroinit: scalar in switch-initializer gets = 0");
	}
	prism_free(&r3);
}

void test_static_vars_no_redundant_zeroinit(void) {
	/* C guarantees static and _Thread_local variables are zero-initialized
	   by the loader/runtime.  Prism must NOT emit = 0, = {0}, or memset
	   for such variables — especially memset, which would re-zero the
	   variable on every function call, destroying static semantics. */

	/* 1. static int — must NOT get '= 0' */
	PrismResult r1 = prism_transpile_source(
	    "void f(void) {\n"
	    "    static int x;\n"
	    "    (void)x;\n"
	    "}\n",
	    "static_int_no_zeroinit.c", prism_defaults());
	CHECK(r1.status == PRISM_OK && r1.output, "static-int: transpiles OK");
	if (r1.output) {
		CHECK(!strstr(r1.output, "= 0") && !strstr(r1.output, "={0}") &&
		      !strstr(r1.output, "= {0}") && !strstr(r1.output, "memset"),
		      "static-int: no redundant zero-init emitted");
	}
	prism_free(&r1);

	/* 2. static array — must NOT get '= {0}' */
	PrismResult r2 = prism_transpile_source(
	    "void f(void) {\n"
	    "    static char buf[10];\n"
	    "    (void)buf;\n"
	    "}\n",
	    "static_array_no_zeroinit.c", prism_defaults());
	CHECK(r2.status == PRISM_OK && r2.output, "static-array: transpiles OK");
	if (r2.output) {
		CHECK(!strstr(r2.output, "= {0}") && !strstr(r2.output, "={0}") &&
		      !strstr(r2.output, "memset"),
		      "static-array: no redundant zero-init emitted");
	}
	prism_free(&r2);

#ifdef __GNUC__
	/* 3. CRITICAL: static typeof(int) — must NOT get memset (would reset
	   the variable on every function call, breaking static semantics) */
	PrismResult r3 = prism_transpile_source(
	    "void f(void) {\n"
	    "    static typeof(int) x;\n"
	    "    (void)x;\n"
	    "}\n",
	    "static_typeof_nozero.c", prism_defaults());
	CHECK(r3.status == PRISM_OK && r3.output, "static-typeof: transpiles OK");
	if (r3.output) {
		CHECK(!strstr(r3.output, "__builtin_memset"),
		      "static-typeof: no memset on static variable (would reset on every call)");
		CHECK(!strstr(r3.output, "= 0") && !strstr(r3.output, "= {0}"),
		      "static-typeof: no redundant zero-init emitted");
	}
	prism_free(&r3);
#endif
}

void test_register_atomic_aggregate_must_error(void) {
#ifndef _MSC_VER
	/* register + _Atomic aggregate cannot be safely zero-initialized:
	   = {0} is illegal on _Atomic, memset is illegal on register.
	   The transpiler must reject this with a hard error instead of
	   silently leaving the variable uninitialized. */
	PrismResult r = prism_transpile_source(
	    "void f(void) {\n"
	    "    register _Atomic struct { int a; } x;\n"
	    "    (void)x;\n"
	    "}\n",
	    "register_atomic_aggregate_error.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "register _Atomic aggregate: must be rejected (not silently uninitialized)");
	prism_free(&r);
#endif
}

#ifndef _MSC_VER
void test_gnu_thread_storage_class(void) {
	/* __thread is a GNU storage class specifier equivalent to _Thread_local.
	   It survives preprocessing verbatim (unlike sigsetjmp→__sigsetjmp).
	   Prism must recognise __thread as TT_STORAGE so that:
	   1. file-scope __thread variables are not redundantly zero-initialized
	   2. static __thread in function scope does not get zero-init
	   3. goto-over-__thread is exempt from CFG errors (static storage) */

	/* __thread at file scope — must NOT get zero-init */
	PrismResult r1 = prism_transpile_source(
	    "__thread int x;\n"
	    "int main(void) { return x; }\n",
	    "gnu_thread_file_scope.c", prism_defaults());
	CHECK(r1.status == PRISM_OK && r1.output, "gnu-__thread-file-scope: transpiles OK");
	if (r1.output) {
		CHECK(!strstr(r1.output, "= 0") && !strstr(r1.output, "= {0}") &&
		      !strstr(r1.output, "memset"),
		      "gnu-__thread-file-scope: no redundant zero-init");
	}
	prism_free(&r1);

	/* static __thread in function scope — must NOT get zero-init */
	PrismResult r2 = prism_transpile_source(
	    "void f(void) {\n"
	    "    static __thread int y;\n"
	    "    (void)y;\n"
	    "}\n",
	    "gnu_thread_static_func.c", prism_defaults());
	CHECK(r2.status == PRISM_OK && r2.output, "gnu-__thread-static-func: transpiles OK");
	if (r2.output) {
		CHECK(!strstr(r2.output, "= 0") && !strstr(r2.output, "= {0}") &&
		      !strstr(r2.output, "memset"),
		      "gnu-__thread-static-func: no redundant zero-init");
	}
	prism_free(&r2);
}
#endif

// computed goto + zeroinit declarations bypasses initialization.
// The CFG verifier only checked computed_goto + F_DEFER, not F_ZEROINIT.
// A computed goto can jump past `int x = 0;` leaving x with stack garbage.
static void test_computed_goto_zeroinit_bypass(void) {
	PrismResult r = prism_transpile_source(
	    "void f(int choice) {\n"
	    "    void *table[] = {&&a, &&b};\n"
	    "    goto *table[choice];\n"
	    "a:;\n"
	    "    int x;\n"
	    "    x = 42;\n"
	    "    return;\n"
	    "b:\n"
	    "    return;\n"
	    "}\n",
	    "computed_goto_zeroinit.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "computed-goto-zeroinit: must reject (bypass zero-init)");
	if (r.status != PRISM_OK && r.error_msg)
		CHECK(strstr(r.error_msg, "computed goto") != NULL,
		      "computed-goto-zeroinit: error mentions computed goto");
	prism_free(&r);
}

// C23 if/switch init-statement shadow scope expires
// too early.  p1_scan_init_shadows is called with scope_close_idx =
// tok_idx(is_close) — the position of the ')' that closes the init-statement
// paren.  This means the shadow only covers tokens up to and including ')';
// everything inside the if-body has a token index AFTER that position, so the
// shadow is invisible there.
//
// Contrast with the for-loop init path which calls skip_one_stmt(body_start)
// to extend scope_close_idx past the entire loop body.
//
// Symptom: if a C23 if-init variable shadows a typedef name in the outer scope,
// the shadow does NOT suppress the typedef inside the if-body.  Declarations
// that use the typedef name inside the body are incorrectly processed by
// try_zero_init_decl, applying zeroinit to 'T x;' when T should be opaque
// (it is a variable in scope, not a type — any 'T x;' in the body is a
// C language error that Prism should preserve verbatim, not silently zeroinit).
//
//   typedef int T;
//   void fn(int c) { if (int T = 0; c) { T x; } }
//
// emits 'T x = {0}' (zeroinit applied despite shadow)
// CORRECT: shadow suppresses the typedef → Prism sees 'T x;' as a non-decl
//          and emits it verbatim (downstream C compiler will reject it as expected)
static void test_c23_if_init_shadow_underscopes_body(void) {
	const char *src =
	    "typedef int T;\n"
	    "void fn(int c) { if (int T = 0; c) { T x; (void)x; } }\n";
	PrismResult r = prism_transpile_source(src, "c23_if_init_shadow.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
		 "c23-if-init-shadow: transpiles OK");
	if (r.output) {
		/* shadow expires at ')' so T is still a typedef inside the body.
		 * try_zero_init_decl fires on 'T x' and emits 'T x = ...' (zeroinit).
		 * Correct: shadow covers body → T is a variable → 'T x;' is verbatim. */
		CHECK(strstr(r.output, "T x = ") == NULL,
		      "c23-if-init-shadow: 'T x' in body must not get zeroinit "
		      "— C23 if-init shadow scope_close_idx is set to tok_idx(is_close) "
		      "which is the ')' position, NOT the end of the body; the shadow "
		      "is invisible to all tokens inside the if-body, so the outer "
		      "typedef T leaks back in and try_zero_init_decl incorrectly "
		      "zeroinits 'T x;' (produces 'T x = {0}' or 'T x = 0')");
	}
	prism_free(&r);

	/* Same bug with switch init-statement */
	const char *src2 =
	    "typedef int T;\n"
	    "void fn(int c) { switch (int T = 0; c) { case 0: { T x; (void)x; } break; } }\n";
	PrismResult r2 = prism_transpile_source(src2, "c23_switch_init_shadow.c", prism_defaults());
	CHECK_EQ(r2.status, PRISM_OK,
		 "c23-switch-init-shadow: transpiles OK");
	if (r2.output) {
		CHECK(strstr(r2.output, "T x = ") == NULL,
		      "c23-switch-init-shadow: 'T x' in body must not get zeroinit "
		      "(same scope_close_idx truncation as if-init variant)");
	}
	prism_free(&r2);
}

// C23 if-init shadow scope amputated before else branch.
// skip_one_stmt(body_start_is) is called on the '{' of the true-branch body,
// which matches to its '}' and stops — it never looks for an else clause.
// This causes scope_close_idx to end at the true-branch '}', so the init
// shadow expires before the else block.  Any typedef or variable declared in
// the init-statement becomes invisible in the else branch, violating C23
// §6.8.4.1 which requires the scope to extend through both branches.
static void test_c23_if_init_shadow_else_scope(void) {
	/* Variable declared in if-init shadows outer typedef.
	 * The shadow must cover both the true-branch AND the else-branch. */
	const char *src =
	    "typedef int T;\n"
	    "void fn(int c) {\n"
	    "    if (int T = 0; c) {\n"
	    "        T x; (void)x;\n"
	    "    } else {\n"
	    "        T y; (void)y;\n"
	    "    }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(src, "c23_if_else_scope.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
		 "c23-if-init-else-scope: transpiles OK");
	if (r.output) {
		/* True branch: shadow covers body, T is a variable → no zeroinit */
		CHECK(strstr(r.output, "T x = ") == NULL,
		      "c23-if-init-else-scope: true branch must not zeroinit "
		      "(shadow covers if-body)");
		/* Else branch: shadow must ALSO cover else body → no zeroinit.
		 * skip_one_stmt on '{' returns at '}' of true-branch,
		 * scope_close_idx does not extend to else → shadow expires →
		 * outer typedef T leaks back → zeroinit fires on 'T y;'. */
		CHECK(strstr(r.output, "T y = ") == NULL,
		      "c23-if-init-else-scope: else branch must not zeroinit "
		      "— if-init shadow scope_close_idx ends at true-branch '}'; "
		      "skip_one_stmt called on '{' matches to '}' and never "
		      "peeks ahead for 'else'; shadow is invisible in else body");
	}
	prism_free(&r);

	/* Same bug with switch (no else, but verify it doesn't regress) */
}

// typedef void func_t(int); func_t my_func; — parse_declarator sees
// my_func as a plain variable (no trailing parens). process_declarators applies
// zero-init (= {0} or memset), which is invalid for function declarations.
// Function POINTER typedefs must remain correctly zeroed.
static void test_func_typedef_zeroinit(void) {
	// Case 1: void function typedef
	{
		const char *code =
		    "typedef void func_t(int);\n"
		    "void test(void) {\n"
		    "    func_t my_func;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "fntd1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "func-typedef-zeroinit: transpile succeeds");
		if (r.output) {
			CHECK(strstr(r.output, "= {0}") == NULL && strstr(r.output, "= 0") == NULL
			      && strstr(r.output, "memset") == NULL,
			      "func-typedef-zeroinit: function declaration must not be zero-initialized");
		}
		prism_free(&r);
	}
	// Case 2: int-returning function typedef
	{
		const char *code =
		    "typedef int getter_t(void);\n"
		    "void test(void) {\n"
		    "    getter_t get_value;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "fntd2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "func-typedef-int: transpile succeeds");
		if (r.output) {
			CHECK(strstr(r.output, "= {0}") == NULL && strstr(r.output, "= 0") == NULL
			      && strstr(r.output, "memset") == NULL,
			      "func-typedef-int: function declaration must not be zero-initialized");
		}
		prism_free(&r);
	}
	// Case 3: function POINTER typedef must STILL be zeroed (regression guard)
	{
		const char *code =
		    "typedef int (*fn_ptr_t)(void);\n"
		    "void test(void) {\n"
		    "    fn_ptr_t fp;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "fntd3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "func-ptr-typedef: transpile succeeds");
		if (r.output) {
			CHECK(strstr(r.output, "= {0}") != NULL || strstr(r.output, "= 0") != NULL,
			      "func-ptr-typedef: function pointer must still be zero-initialized");
		}
		prism_free(&r);
	}
}

// BUG_AUDIT_A5: parenthesized function typedef `typedef int (FuncType)(int)`
// was not detected as is_func because the paren-wrapped declarator made
// decl.end non-NULL, skipping the `(` check. Fix: added else-branch after
// the !decl.end check that inspects the token after decl.end for `(`.
static void test_paren_func_typedef_zeroinit(void) {
	printf("\n--- parenthesized function typedef zeroinit (A5) ---\n");
	{
		const char *code =
		    "typedef int (FuncType)(int);\n"
		    "void test(void) {\n"
		    "    FuncType my_func;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "a5_pft.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "paren func typedef transpiles (A5)");
		if (r.output) {
			CHECK(strstr(r.output, "= {0}") == NULL && strstr(r.output, "= 0") == NULL
			      && strstr(r.output, "memset") == NULL,
			      "paren func typedef must NOT be zero-initialized (A5)");
		}
		prism_free(&r);
	}
	// Control: paren'd non-function typedef must still be zeroed
	{
		const char *code =
		    "typedef int (IntType);\n"
		    "void test(void) {\n"
		    "    IntType x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "a5_pnt.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "paren non-func typedef transpiles (A5)");
		if (r.output) {
			CHECK(strstr(r.output, "= 0") != NULL || strstr(r.output, "= {0}") != NULL,
			      "paren non-func typedef must still be zero-initialized (A5 control)");
		}
		prism_free(&r);
	}
}

// ternary in case label vs skip_one_stmt shadow scope.
// skip_one_stmt's case/default scanner confuses the ternary `:` with the
// label `:` in `case 1 ? 2 : 3:`, overshooting the for-loop body boundary.
// The for-init shadow extends past the loop, hiding the typedef and
// bypassing zeroinit for `T *p;` (treated as expression `T * p;`).
static void test_ternary_case_label_shadow_leak(void) {
	printf("\n--- ternary in case label shadow leak ---\n");

	// For-init variable `T` scoped to the for loop.  After the loop,
	// `T` is the outer typedef, so `T *p;` is a pointer declaration
	// and must be zero-initialized.
	const char *code =
	    "typedef int T;\n"
	    "void f(int c) {\n"
	    "    for (T T = 0; T < 1; T++)\n"
	    "        switch(c) case 1 ? 2 : 3: {\n"
	    "            break;\n"
	    "        }\n"
	    "    T *p;\n"
	    "    (void)p;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "ternary_case_shadow.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "ternary-case-shadow: transpile succeeds");
	if (r.output) {
		CHECK(strstr(r.output, "T *p = 0") != NULL ||
		      strstr(r.output, "T * p = 0") != NULL,
		      "ternary-case-shadow: T *p must be zero-initialized (not treated as expression)");
	}
	prism_free(&r);
}

// typeof(func) alias; must not emit __builtin_memset.
// has_typeof in needs_memset forces memset for all typeof-typed locals,
// but function declarations cannot be memset'd — runtime segfault/linker error.
static void test_typeof_func_decl_memset(void) {
	printf("\n--- typeof function declaration memset trap ---\n");

	// Case 1: typeof(void_func) alias; — must not memset a function.
	{
		const char *code =
		    "void my_func(int x) { (void)x; }\n"
		    "void wrapper(void) {\n"
		    "    typeof(my_func) another_func;\n"
		    "    (void)another_func;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "typeof_fn_decl.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typeof-func-memset: typeof(func) declaration transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "__builtin_memset") == NULL,
			      "typeof-func-memset: must not emit memset for function declaration");
		}
		prism_free(&r);
	}

	// Case 2: typeof(int_func) alias; — same for non-void return type.
	{
		const char *code =
		    "int getter(void) { return 42; }\n"
		    "void wrapper(void) {\n"
		    "    typeof(getter) another_getter;\n"
		    "    (void)another_getter;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "typeof_fn_decl2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typeof-func-memset-int: typeof(func) declaration transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "__builtin_memset") == NULL,
			      "typeof-func-memset-int: must not emit memset for function declaration");
		}
		prism_free(&r);
	}

	// Regression guard: typeof(struct_var) must still be zero-initialized.
	{
		const char *code =
		    "struct S { int x; int y; };\n"
		    "struct S global_s;\n"
		    "void wrapper(void) {\n"
		    "    typeof(global_s) local_s;\n"
		    "    (void)local_s;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "typeof_struct_zi.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typeof-struct-zeroinit: typeof(struct) still zero-initialized");
		if (r.output) {
			// Should have some form of zeroinit (= {0} or memset)
			CHECK(strstr(r.output, "= {0}") != NULL || has_zeroing(r.output),
			      "typeof-struct-zeroinit: typeof(struct) var gets zero-init");
		}
		prism_free(&r);
	}
}

// Trailing attributes (C23 [[...]] or GNU __attribute__) after the
// parameter list of a void-returning function must not blind the
// is_typeof_func_type backward walk.  Without the fix, walk_back_skip_attrs
// was not used: the scanner hit ']' or the TT_ATTR keyword and aborted,
// causing typeof(func) to be misclassified as a variable and zero-
// initialized — a fatal compiler error on the backend.
static void test_typeof_void_func_trailing_attr(void) {
	printf("\n--- typeof void func trailing attr ---\n");
	// C23 trailing attribute
	{
		const char *code =
		    "void handler(void) [[gnu::cold]] {\n"
		    "    (void)0;\n"
		    "}\n"
		    "void setup(void) {\n"
		    "    typeof(handler) fwd;\n"
		    "    (void)fwd;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "typeof_c23trail.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typeof-trailing-attr-c23: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "__builtin_memset") == NULL && strstr(r.output, "= {0}") == NULL,
			      "typeof-trailing-attr-c23: must not zero-init function declaration");
		}
		prism_free(&r);
	}
	// GNU trailing attribute
	{
		const char *code =
		    "void handler(void) __attribute__((cold)) {\n"
		    "    (void)0;\n"
		    "}\n"
		    "void setup(void) {\n"
		    "    typeof(handler) fwd;\n"
		    "    (void)fwd;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "typeof_gnutrail.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typeof-trailing-attr-gnu: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "__builtin_memset") == NULL && strstr(r.output, "= {0}") == NULL,
			      "typeof-trailing-attr-gnu: must not zero-init function declaration");
		}
		prism_free(&r);
	}
}

// pointer-to-array declarators like int (*p)[4] were treated as
// aggregates because is_array was set (the [4] describes the pointed-to
// type, not the variable itself). This caused = {0} instead of = 0.
static void test_ptr_to_array_scalar_zeroinit(void) {
	printf("\n--- Pointer-to-array scalar zero-init ---\n");

	// int (*p)[4] is a pointer → should get = 0, not = {0}
	{
		const char *code =
		    "void f(void) {\n"
		    "    int (*p)[4];\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "pta1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "ptr-to-array: transpile OK");
		if (r.output) {
			CHECK(strstr(r.output, "(*p)[4] = 0") != NULL,
			      "ptr-to-array: must use = 0 (scalar pointer)");
			CHECK(strstr(r.output, "= {0}") == NULL,
			      "ptr-to-array: must NOT use = {0}");
		}
		prism_free(&r);
	}

	// int *arr[4] is an array → should get = {0}
	{
		const char *code =
		    "void f(void) {\n"
		    "    int *arr[4];\n"
		    "    (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "pta2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "array-of-ptrs: transpile OK");
		if (r.output)
			CHECK(strstr(r.output, "= {0}") != NULL,
			      "array-of-ptrs: must use = {0}");
		prism_free(&r);
	}

	// int (*(r[4])) — array with extra parens → should get = {0}
	{
		const char *code =
		    "void f(void) {\n"
		    "    int (*(r[4]));\n"
		    "    (void)r;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "pta3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "paren-array: transpile OK");
		if (r.output)
			CHECK(strstr(r.output, "= {0}") != NULL,
			      "paren-array: must use = {0} (still an array)");
		prism_free(&r);
	}
}

// stmt-expr inside control-flow conditions (if/while/for) must
// get zero-init processing.  Previously, handle_open_brace treated the
// stmt-expr `{` as a compound literal inside ctrl parens, bypassing all
// block-level processing (zero-init, raw, orelse, defer).
static void test_stmt_expr_zeroinit_in_ctrl_cond(void) {
	printf("\n--- stmt-expr zero-init in ctrl conditions ---\n");

	// if condition
	{
		const char *code =
		    "void f(void) {\n"
		    "    if (({int x; x=42; x;})) { (void)0; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t59a.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "transpile succeeds");
		if (r.output)
			CHECK(strstr(r.output, "int x = 0") != NULL,
			      "int x inside stmt-expr in if() must be zero-initialized");
		prism_free(&r);
	}

	// while condition
	{
		const char *code =
		    "void f(void) {\n"
		    "    while (({int x; x=0; x;})) { break; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t59b.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "transpile succeeds");
		if (r.output)
			CHECK(strstr(r.output, "int x = 0") != NULL,
			      "int x inside stmt-expr in while() must be zero-initialized");
		prism_free(&r);
	}

	// for condition
	{
		const char *code =
		    "void f(void) {\n"
		    "    for (int i = 0; ({int x; x=(i<3); x;}); i++) { (void)i; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t59c.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "transpile succeeds");
		if (r.output)
			CHECK(strstr(r.output, "int x = 0") != NULL,
			      "int x inside stmt-expr in for() condition must be zero-initialized");
		prism_free(&r);
	}

	// if-body after stmt-expr condition must still work normally
	{
		const char *code =
		    "void f(void) {\n"
		    "    if (({int x; x=1; x;})) {\n"
		    "        int y;\n"
		    "        (void)y;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t59d.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "transpile succeeds");
		if (r.output) {
			CHECK(strstr(r.output, "int x = 0") != NULL,
			      "stmt-expr var zero-initialized");
			CHECK(strstr(r.output, "int y = 0") != NULL,
			      "if-body var also zero-initialized");
		}
		prism_free(&r);
	}

	// compound literal in ctrl parens must NOT be treated as stmt-expr
	{
		const char *code =
		    "#include <string.h>\n"
		    "typedef struct { int x; } P;\n"
		    "void f(void) {\n"
		    "    P p;\n"
		    "    if (memcmp(&p, &(P){1}, sizeof(P)) == 0) { (void)0; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t59e.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "compound literal in if() still transpiles correctly");
		prism_free(&r);
	}
}

// typeof(external_function) emits __builtin_memset on a function
// declaration, corrupting the .text segment at runtime.
static void test_typeof_extern_func_memset(void) {
	printf("\n--- typeof external function memset corruption ---\n");

	// Sub-test 1: typeof(printf) — external function, forward-declared only.
	{
		const char *code =
		    "int printf(const char *, ...);\n"
		    "void f(void) {\n"
		    "    typeof(printf) my_func;\n"
		    "    (void)my_func;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t69a.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "__builtin_memset") == NULL &&
			      strstr(r.output, "= {0}") == NULL,
			      "typeof(printf) must NOT emit zero-init");
		}
		prism_free(&r);
	}

	// Sub-test 2: typeof(extern_struct_var) — external variable, not a function.
	// Must STILL get zero-init.
	{
		const char *code =
		    "typedef struct { int x; } S;\n"
		    "extern S global;\n"
		    "void f(void) {\n"
		    "    typeof(global) copy;\n"
		    "    (void)copy;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t69b.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "__builtin_memset") != NULL || has_zeroing(r.output),
			      "typeof(extern_struct_var) must still get zero-init");
		}
		prism_free(&r);
	}

	// Sub-test 3: typeof(defined_func) — defined function (func_meta hit).
	// Must not emit memset (regression guard for existing logic).
	{
		const char *code =
		    "void helper(void) {}\n"
		    "void f(void) {\n"
		    "    typeof(helper) my_func;\n"
		    "    (void)my_func;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t69c.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "__builtin_memset") == NULL,
			      "typeof(defined_func) must not emit memset (regression)");
		}
		prism_free(&r);
	}

	// Sub-test 4: typeof(func_ptr_var) — function pointer variable.
	// Function pointer IS a pointer — should NOT get typeof memset.
	{
		const char *code =
		    "void (*callback)(int);\n"
		    "void f(void) {\n"
		    "    typeof(callback) local_cb;\n"
		    "    (void)local_cb;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t69d.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "transpiles OK");
		prism_free(&r);
	}
}

// _Static_assert/sizeof at file scope confuses typeof func-type scanner.
// The scanner sees `ident(` inside sizeof(ident(...)) and falsely flags it as
// a function declaration, bypassing zero-init for function pointer variables.
static void test_typeof_funcptr_static_assert_bypass(void) {
	printf("\n--- typeof funcptr _Static_assert scanner bypass ---\n");
	// Sub-test 1: _Static_assert(sizeof(funcptr_var(args))) at file scope
	// must not trick the scanner into treating funcptr_var as a function.
	{
		const char *code =
		    "int (*fake_func)(int);\n"
		    "_Static_assert(sizeof(fake_func(1)) > 0, \"ok\");\n"
		    "void f(void) {\n"
		    "    typeof(fake_func) target;\n"
		    "    (void)target;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "funcptr_sa.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "funcptr-static-assert: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "__builtin_memset") != NULL || has_zeroing(r.output),
			      "funcptr-static-assert: typeof(funcptr) must get zero-init");
		}
		prism_free(&r);
	}
	// Sub-test 2: actual function (not pointer) must still be detected.
	{
		const char *code =
		    "int real_func(int);\n"
		    "_Static_assert(sizeof(real_func(1)) > 0, \"ok\");\n"
		    "void f(void) {\n"
		    "    typeof(real_func) alias;\n"
		    "    (void)alias;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "func_sa.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "func-static-assert: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "__builtin_memset") == NULL,
			      "func-static-assert: typeof(real_func) must NOT emit zero-init");
		}
		prism_free(&r);
	}
}

// #pragma inside a statement expression processed by walk_balanced
// (e.g., inside array dimensions) clears at_stmt_start, skipping zero-init.
static void test_stmt_expr_pragma_zeroinit_bypass(void) {
	// Sub-test 1: pragma before declaration inside stmt-expr in array dim
	{
		const char *code =
		    "void f(void) {\n"
		    "    int arr[({ \n"
		    "#pragma GCC diagnostic push\n"
		    "        int x;\n"
		    "        x + 1;\n"
		    "    })];\n"
		    "    (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t70a.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "int x = 0") != NULL,
			      "pragma in walk_balanced stmt-expr must not bypass zero-init");
		}
		prism_free(&r);
	}

	// Sub-test 2: #line directive (also TK_PREP_DIR) before declaration
	{
		const char *code =
		    "void f(void) {\n"
		    "    int arr[({ \n"
		    "#line 42 \"test.c\"\n"
		    "        int y;\n"
		    "        y + 1;\n"
		    "    })];\n"
		    "    (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t70b.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "int y = 0") != NULL,
			      "#line in walk_balanced stmt-expr must not bypass zero-init");
		}
		prism_free(&r);
	}
}

static void test_asm_goto_zeroinit_rejected(void) {
	/* asm goto hides jump targets inside the assembly string.
	   Prism's token walker skips asm parameters entirely and never
	   extracts the labels, so no P1K_GOTO is recorded.  The CFG
	   verifier was blind to asm goto jumps, allowing zeroinit'd
	   declarations (VLA and non-VLA) to be bypassed.
	   Fix: p1_verify_cfg checks TT_ASM on body_open alongside
	   has_computed_goto. */
	{
		/* asm goto + VLA */
		const char *code =
		    "void f(int n) {\n"
		    "    if (n) asm goto(\"jmp %l[out]\" : : : : out);\n"
		    "    int vla[n];\n"
		    "    return;\n"
		    "out: vla[0] = 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "asm_goto_vla.c",
						       prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "asm goto + VLA must be rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "asm goto") != NULL,
			      "asm goto + VLA error must mention asm goto");
		prism_free(&r);
	}
	{
		/* asm goto + non-VLA zeroinit */
		const char *code =
		    "void f(int n) {\n"
		    "    if (n) asm goto(\"jmp %l[out]\" : : : : out);\n"
		    "    int x;\n"
		    "    return;\n"
		    "out: x = 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "asm_goto_zi.c",
						       prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "asm goto + zeroinit must be rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "asm goto") != NULL,
			      "asm goto + zeroinit error must mention asm goto");
		prism_free(&r);
	}
	{
		/* Regular asm (no goto) + zeroinit must still be allowed */
		const char *code =
		    "void f(void) {\n"
		    "    int x;\n"
		    "    asm volatile(\"nop\");\n"
		    "    x = 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "asm_nogo.c",
						       prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "regular asm (no goto) with zeroinit must be allowed");
		prism_free(&r);
	}
}

// CFG verifier P1K_CASE body_close_idx desync —
// for-init declarations inside switch should not be rejected.
static void test_switch_for_init_not_rejected(void) {
	/* Braceless for-init inside switch case must be accepted:
	   the variable is scoped to the for loop, not the switch body. */
	{
		PrismResult r = prism_transpile_source(
		    "void f(int state) {\n"
		    "    switch (state) {\n"
		    "    case 0:\n"
		    "        for (int i = 0; i < 5; i++)\n"
		    "            (void)i;\n"
		    "    case 1:\n"
		    "        break;\n"
		    "    }\n"
		    "}\n",
		    "switch_for_init.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "switch-for-init: braceless for-init in case must be accepted");
		prism_free(&r);
	}
	/* Genuine unbraced declaration in switch body must still be rejected. */
	{
		PrismResult r = prism_transpile_source(
		    "void f(int state) {\n"
		    "    switch (state) {\n"
		    "    case 0:\n"
		    "        ;\n"
		    "        int x;\n"
		    "        (void)x;\n"
		    "    case 1:\n"
		    "        break;\n"
		    "    }\n"
		    "}\n",
		    "switch_bare_decl.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "switch-bare-decl: unbraced decl in switch must still be rejected");
		prism_free(&r);
	}
}

// skip_one_stmt didn't handle user labels (L: stmt), causing it to
// over-consume tokens past the labeled statement, extending variable scopes
// and triggering false CFG verifier rejections.
static void test_skip_one_stmt_label_parsing(void) {
	printf("\n--- skip_one_stmt Label Parsing ---\n");

	// Label before braceless if body in for loop
	{
		PrismResult r = prism_transpile_source(
		    "void test(void) {\n"
		    "    goto SAFE_TARGET;\n"
		    "    for (int secret = 0; secret < 1; secret++)\n"
		    "        L: if (1) { }\n"
		    "    SAFE_TARGET: ;\n"
		    "    int safe_var = 1;\n"
		    "    (void)safe_var;\n"
		    "}\n",
		    "label_skip.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "label-skip: goto past labeled braceless for body must not be rejected");
		prism_free(&r);
	}

	// Multiple chained labels
	{
		PrismResult r = prism_transpile_source(
		    "void test(void) {\n"
		    "    goto END;\n"
		    "    for (int x = 0; x < 1; x++)\n"
		    "        A: B: C: if (1) { }\n"
		    "    END: ;\n"
		    "}\n",
		    "chain_label_skip.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "chain-label-skip: chained labels before braceless body must work");
		prism_free(&r);
	}

	// Label before braced block in while loop
	{
		PrismResult r = prism_transpile_source(
		    "void test(void) {\n"
		    "    goto DONE;\n"
		    "    while (0)\n"
		    "        BODY: { }\n"
		    "    DONE: ;\n"
		    "}\n",
		    "label_while_skip.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "label-while-skip: label before braced while body must work");
		prism_free(&r);
	}
}

// emit_type_range stmt-expr bypass in struct bodies
static void test_struct_body_stmt_expr_features(void) {
	printf("\n--- Struct Body Statement Expression Features ---\n");

	// Test 1: zeroinit inside struct body statement expression (array size)
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    struct S { int x[({ int vla[n]; (void)vla; 1; })]; } obj;\n"
		    "    (void)obj;\n"
		    "}\n",
		    "struct_se_zeroinit.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "struct-se-zeroinit: transpiles OK");
		CHECK(r.output != NULL, "struct-se-zeroinit: output not NULL");
		CHECK(strstr(r.output, "memset") != NULL,
		      "struct-se-zeroinit: VLA inside struct stmt-expr must get memset");
		prism_free(&r);
	}

	// Test 2: defer inside struct body statement expression
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    struct S { int x[({ { defer (void)0; } 1; })]; } obj;\n"
		    "    (void)obj;\n"
		    "}\n",
		    "struct_se_dfr.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "struct-se-defer: transpiles OK");
		CHECK(r.output != NULL, "struct-se-defer: output not NULL");
		// Check that 'defer' keyword doesn't appear verbatim (skip #line directives)
		{
			const char *p = r.output;
			bool found_defer = false;
			while ((p = strstr(p, "defer")) != NULL) {
				// Skip occurrences inside #line directives (filenames)
				const char *line_start = p;
				while (line_start > r.output && line_start[-1] != '\n') line_start--;
				if (line_start[0] != '#') { found_defer = true; break; }
				p += 5;
			}
			CHECK(!found_defer,
			      "struct-se-defer: defer inside struct stmt-expr must be processed");
		}
		prism_free(&r);
	}

	// Test 3: raw keyword inside struct body statement expression
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    struct S { int x[({ raw int y = 1; y; })]; } obj;\n"
		    "    (void)obj;\n"
		    "}\n",
		    "struct_se_raw.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "struct-se-raw: transpiles OK");
		CHECK(r.output != NULL, "struct-se-raw: output not NULL");
		// 'raw' keyword should be stripped (not emitted verbatim)
		CHECK(strstr(r.output, "raw int y") == NULL,
		      "struct-se-raw: raw keyword inside struct stmt-expr must be stripped");
		// But the declaration itself should remain
		CHECK(strstr(r.output, "int y = 1") != NULL,
		      "struct-se-raw: declaration preserved after raw stripping");
		prism_free(&r);
	}
}

// block-scope function prototype typeof memset.
// A block-scope forward declaration like 'int add(int, int);' inside a
// function body was not recorded in p1_func_proto_map (brace_depth > 0),
// so typeof(add) triggered a spurious __builtin_memset on a function type.
static void test_typeof_block_scope_func_proto(void) {
	printf("\n--- block-scope function proto typeof memset ---\n");
	const char *code =
	    "void f(void) {\n"
	    "    int add(int, int);\n"
	    "    typeof(add) my_func;\n"
	    "    (void)my_func;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "t93.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "__builtin_memset") == NULL &&
		      strstr(r.output, "= {0}") == NULL,
		      "typeof(block_scope_func) must NOT emit zero-init");
	}
	prism_free(&r);
}

// array_size_is_vla exponential blowup on deeply nested sizeof.
// sizeof(int[sizeof(int[...])]) inside an array dimension caused O(2^N)
// re-scanning because the sizeof path didn't skip past matched brackets
// after recursion.  Also has a depth>256 guard.
// sizeof(param_array) is pointer-size (constant), not VLA.
// Parameter arrays decay to pointers; first dimension skipped for VLA check.
static void test_vla_param_decay_sizeof(void) {
	printf("\n--- VLA param decay sizeof ---\n");
	// sizeof(arr) where arr is a decayed array parameter should be constant.
	// No goto — just verify it gets brace-init (fixed-size), not memset (VLA).
	const char *code =
	    "void f(int n, int arr[n]) {\n"
	    "    int local[sizeof(arr)];\n"
	    "    (void)local;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "t96.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "sizeof(param_array) is not VLA");
	// Should use = {0} (fixed-size), NOT memset (VLA).
	if (r.output) {
		CHECK(strstr(r.output, "= {0}") != NULL || strstr(r.output, "={0}") != NULL,
		      "fixed-size array gets brace init, not memset");
	}
	prism_free(&r);
}

static void test_array_size_is_vla_depth_guard(void) {
	printf("\n--- array_size_is_vla depth guard ---\n");
	// Build sizeof(int[sizeof(int[sizeof(int[...1...])])]) with 300 levels
	// to trigger the depth > 256 guard.
	char buf[8192];
	int pos = 0;
	pos += snprintf(buf + pos, sizeof(buf) - pos, "void f(void) { int arr[");
	for (int i = 0; i < 300 && pos < 6000; i++)
		pos += snprintf(buf + pos, sizeof(buf) - pos, "sizeof(int[");
	pos += snprintf(buf + pos, sizeof(buf) - pos, "1");
	for (int i = 0; i < 300 && pos < 7500; i++)
		pos += snprintf(buf + pos, sizeof(buf) - pos, "])");
	pos += snprintf(buf + pos, sizeof(buf) - pos, "]; (void)arr; }");
	PrismResult r = prism_transpile_source(buf, "t95.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "depth > 256 rejected");
	if (r.error_msg)
		CHECK(strstr(r.error_msg, "nesting depth") != NULL,
		      "error mentions nesting depth");
	prism_free(&r);
}

#ifdef __GNUC__
static void test_extension_zeroinit(void) {
	// Scalar
	__extension__ int ext_i;
	CHECK_EQ(ext_i, 0, "__extension__ int zero-init");

	// Struct
	__extension__ struct { int a; int b; } ext_s;
	CHECK_EQ(ext_s.a, 0, "__extension__ struct.a zero-init");
	CHECK_EQ(ext_s.b, 0, "__extension__ struct.b zero-init");

	// Pointer
	__extension__ void *ext_p;
	CHECK(ext_p == NULL, "__extension__ pointer zero-init");

	// Array
	__extension__ int ext_arr[4];
	int ext_ok = 1;
	for (int i = 0; i < 4; i++)
		if (ext_arr[i] != 0) ext_ok = 0;
	CHECK(ext_ok, "__extension__ array zero-init");

	// Transpile-level: verify = {0} or memset is present
	PrismResult r = prism_transpile_source(
		"void f(void) { __extension__ int x; }\n",
		"ext_zi.c", prism_defaults());
	CHECK(r.status == PRISM_OK, "__extension__ zeroinit: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "= 0") != NULL || has_zeroing(r.output),
		      "__extension__ zeroinit: output has initialization");
	}
	prism_free(&r);

	// Chained __extension__
	__extension__ __extension__ int ext_chain;
	CHECK_EQ(ext_chain, 0, "chained __extension__ zero-init");
}

// p1_scan_init_shadows placed P1_IS_DECL on the __extension__
// prefix token instead of the type keyword.  Pass 2's fast gate
// skipped the TT_INLINE token to find an unannotated type → zeroinit
// silently dropped for for/if/switch init declarations.
static void test_extension_forinit_zeroinit(void) {
	printf("\n--- __extension__ for-init zero-init ---\n");

	// Basic: __extension__ int in for-init (GCC only — Clang rejects
	// __extension__ in for-init but Prism processes the preprocessed
	// output, which is always GCC-flavored when running under GCC).
	// Transpile-level only to avoid Clang compile errors.

	// Transpile-level: verify = 0 is present for __extension__ in for-init
	{
		PrismResult r = prism_transpile_source(
			"void f(void) { for(__extension__ int i; i < 10; i++) {} }\n",
			"ext_fi.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "ext for-init: transpiles OK");
		if (r.output)
			CHECK(strstr(r.output, "i = 0") != NULL,
			      "ext for-init: output has = 0");
		prism_free(&r);
	}

	// Multi-declarator: __extension__ int a, b in for-init
	{
		PrismResult r = prism_transpile_source(
			"void f(void) { for(__extension__ int a, b; a+b < 10; a++) {} }\n",
			"ext_fi_m.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "ext for-init multi-decl: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "a = 0") != NULL,
			      "ext for-init multi-decl: a has = 0");
			CHECK(strstr(r.output, "b = 0") != NULL,
			      "ext for-init multi-decl: b has = 0");
		}
		prism_free(&r);
	}

	// Chained __extension__ in for-init
	{
		PrismResult r = prism_transpile_source(
			"void f(void) { for(__extension__ __extension__ int i; i < 10; i++) {} }\n",
			"ext_fi2.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "ext chained for-init: transpiles OK");
		if (r.output)
			CHECK(strstr(r.output, "i = 0") != NULL,
			      "ext chained for-init: output has = 0");
		prism_free(&r);
	}
}
#endif

static void test_const_vla_memset_ub(void) {
	printf("\n--- Const VLA/typeof memset UB rejection ---\n");

	/* const typeof(int[len]) buf; — memset on const is UB (C11 §6.7.3p6) */
	PrismResult r1 = prism_transpile_source(
	    "void f(int len) {\n"
	    "    const typeof(int[len]) secure_buffer;\n"
	    "    (void)secure_buffer;\n"
	    "}\n",
	    "const_vla1.c", prism_defaults());
	CHECK(r1.status != PRISM_OK, "const typeof(VLA) memset: must be rejected");
	if (r1.error_msg)
		CHECK(strstr(r1.error_msg, "const") != NULL,
		      "const typeof(VLA) memset: error mentions const");
	prism_free(&r1);

	/* const int buf[n]; — VLA needs memset, but const forbids it */
	PrismResult r2 = prism_transpile_source(
	    "void f(int n) {\n"
	    "    const int buf[n];\n"
	    "    (void)buf;\n"
	    "}\n",
	    "const_vla2.c", prism_defaults());
	CHECK(r2.status != PRISM_OK, "const int VLA memset: must be rejected");
	if (r2.error_msg)
		CHECK(strstr(r2.error_msg, "const") != NULL,
		      "const int VLA memset: error mentions const");
	prism_free(&r2);

	/* raw const int buf[n]; — raw opts out, should be accepted */
	PrismResult r3 = prism_transpile_source(
	    "void f(int n) {\n"
	    "    raw const int buf[n];\n"
	    "    (void)buf;\n"
	    "}\n",
	    "const_vla_raw.c", prism_defaults());
	CHECK(r3.status == PRISM_OK, "raw const VLA: accepted (raw opts out)");
	prism_free(&r3);

	/* Non-VLA const — normal = {0}, should work */
	PrismResult r4 = prism_transpile_source(
	    "void f(void) {\n"
	    "    const int x;\n"
	    "    (void)x;\n"
	    "}\n",
	    "const_nonvla.c", prism_defaults());
	CHECK(r4.status == PRISM_OK, "const non-VLA: accepted (= 0)");
	prism_free(&r4);
}

static void test_register_vla_bypass(void) {
	printf("\n--- Register VLA zero-init rejection ---\n");

	/* register int buf[n]; — can't memset (no address), can't = {0} (VLA) */
	PrismResult r1 = prism_transpile_source(
	    "void f(int n) {\n"
	    "    register int buffer[n];\n"
	    "    (void)buffer;\n"
	    "}\n",
	    "reg_vla1.c", prism_defaults());
	CHECK(r1.status != PRISM_OK, "register VLA: must be rejected");
	if (r1.error_msg)
		CHECK(strstr(r1.error_msg, "register") != NULL,
		      "register VLA: error mentions register");
	prism_free(&r1);

	/* raw register int buf[n]; — raw opts out, should be accepted */
	PrismResult r2 = prism_transpile_source(
	    "void f(int n) {\n"
	    "    raw register int buffer[n];\n"
	    "    (void)buffer;\n"
	    "}\n",
	    "reg_vla_raw.c", prism_defaults());
	CHECK(r2.status == PRISM_OK, "raw register VLA: accepted (raw opts out)");
	prism_free(&r2);

	/* register int x; — non-VLA register, normal = 0 */
	PrismResult r3 = prism_transpile_source(
	    "void f(void) {\n"
	    "    register int x;\n"
	    "    (void)x;\n"
	    "}\n",
	    "reg_nonvla.c", prism_defaults());
	CHECK(r3.status == PRISM_OK, "register non-VLA: accepted (= 0)");
	prism_free(&r3);
}

/* Volatile member detection: memset on structs with volatile fields is UB
 * (ISO C11 §6.7.3p6). Must emit byte loop (__prism_p_) not __builtin_memset. */
static void test_volatile_member_memset(void) {
	printf("\n--- Volatile Member Memset Detection ---\n");

	/* 1. Inline struct with volatile field + VLA */
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    struct { volatile int status; int data; } buf[n];\n"
		    "    (void)buf;\n"
		    "}\n",
		    "vol_inline.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "vol inline struct VLA: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__prism_p_") != NULL,
			      "vol inline struct VLA: byte loop (not memset)");
			CHECK(strstr(r.output, "__builtin_memset") == NULL,
			      "vol inline struct VLA: no memset");
		}
		prism_free(&r);
	}

	/* 2. Typedef of struct with volatile field + VLA */
	{
		PrismResult r = prism_transpile_source(
		    "typedef struct { volatile int status; int data; } MMIO;\n"
		    "void f(int n) {\n"
		    "    MMIO regs[n];\n"
		    "    (void)regs;\n"
		    "}\n",
		    "vol_typedef.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "vol typedef VLA: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__prism_p_") != NULL,
			      "vol typedef VLA: byte loop (not memset)");
			CHECK(strstr(r.output, "__builtin_memset") == NULL,
			      "vol typedef VLA: no memset");
		}
		prism_free(&r);
	}

	/* 3. Struct tag reference (body defined earlier) + VLA */
	{
		PrismResult r = prism_transpile_source(
		    "struct MMIO_Reg { volatile int status; int data; };\n"
		    "void f(int n) {\n"
		    "    struct MMIO_Reg regs[n];\n"
		    "    (void)regs;\n"
		    "}\n",
		    "vol_tag.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "vol tag ref VLA: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__prism_p_") != NULL,
			      "vol tag ref VLA: byte loop (not memset)");
			CHECK(strstr(r.output, "__builtin_memset") == NULL,
			      "vol tag ref VLA: no memset");
		}
		prism_free(&r);
	}

	/* 4. typeof(struct_with_volatile_member) */
	{
		PrismResult r = prism_transpile_source(
		    "typedef struct { volatile int flags; } HwReg;\n"
		    "void f(void) {\n"
		    "    HwReg base;\n"
		    "    typeof(base) shadow;\n"
		    "    (void)base; (void)shadow;\n"
		    "}\n",
		    "vol_typeof.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "vol typeof: transpiles");
		if (r.output) {
			/* typeof path uses memset — shadow is aggregate, not VLA.
			 * Only VLA/typeof/_Atomic aggregates go through emit_typeof_memsets.
			 * typeof(base) where base is plain aggregate uses = {0}. */
			CHECK(strstr(r.output, "= {0}") != NULL || strstr(r.output, "__prism_p_") != NULL,
			      "vol typeof: some zeroing present");
		}
		prism_free(&r);
	}

	/* 5. Nested struct with volatile field deep inside + VLA */
	{
		PrismResult r = prism_transpile_source(
		    "struct Inner { volatile int ctl; };\n"
		    "struct Outer { struct Inner hw; int pad; };\n"
		    "void f(int n) {\n"
		    "    struct Outer devs[n];\n"
		    "    (void)devs;\n"
		    "}\n",
		    "vol_nested.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "vol nested VLA: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__prism_p_") != NULL,
			      "vol nested VLA: byte loop (not memset)");
			CHECK(strstr(r.output, "__builtin_memset") == NULL,
			      "vol nested VLA: no memset");
		}
		prism_free(&r);
	}

	/* 6. Volatile typedef member (typedef volatile int vint; struct { vint x; }) */
	{
		PrismResult r = prism_transpile_source(
		    "typedef volatile int vint;\n"
		    "void f(int n) {\n"
		    "    struct { vint status; } regs[n];\n"
		    "    (void)regs;\n"
		    "}\n",
		    "vol_tdef_member.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "vol typedef-member VLA: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__prism_p_") != NULL,
			      "vol typedef-member VLA: byte loop (not memset)");
			CHECK(strstr(r.output, "__builtin_memset") == NULL,
			      "vol typedef-member VLA: no memset");
		}
		prism_free(&r);
	}

	/* 7. Non-volatile struct should STILL use memset (no false positive).
	 * On MSVC, the byte loop is always used (no __builtin_memset),
	 * but it should NOT have the "volatile char *" qualifier prefix. */
	{
		PrismResult r = prism_transpile_source(
		    "struct Plain { int x; int y; };\n"
		    "void f(int n) {\n"
		    "    struct Plain arr[n];\n"
		    "    (void)arr;\n"
		    "}\n",
		    "nonvol.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "non-volatile VLA: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "memset") != NULL || strstr(r.output, "__prism_p_") != NULL,
			      "non-volatile VLA: has zeroing");
			CHECK(strstr(r.output, "volatile char") == NULL,
			      "non-volatile VLA: no volatile qualifier (no false positive)");
		}
		prism_free(&r);
	}

	/* 8. typeof with volatile typeof struct — typeof(HwReg) VLA */
	{
		PrismResult r = prism_transpile_source(
		    "typedef struct { volatile int sr; } HwReg;\n"
		    "void f(int n) {\n"
		    "    typeof(HwReg) regs[n];\n"
		    "    (void)regs;\n"
		    "}\n",
		    "vol_typeof_vla.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "vol typeof VLA: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__prism_p_") != NULL,
			      "vol typeof VLA: byte loop (not memset)");
			CHECK(strstr(r.output, "__builtin_memset") == NULL,
			      "vol typeof VLA: no memset");
		}
		prism_free(&r);
	}

	/* 9. Namespace collision: ordinary variable shadows struct tag name.
	 * ISO C11 §6.2.3: tags and ordinary identifiers are separate namespaces.
	 * A local variable named MMIO must NOT hide struct MMIO's volatile fields. */
	{
		PrismResult r = prism_transpile_source(
		    "struct MMIO { volatile int status; int data; };\n"
		    "void f(int n) {\n"
		    "    int MMIO = 1;\n"
		    "    struct MMIO buffer[n];\n"
		    "    (void)MMIO; (void)buffer;\n"
		    "}\n",
		    "vol_ns.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "vol namespace shadow: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__prism_p_") != NULL,
			      "vol namespace shadow: byte loop (not memset)");
			CHECK(strstr(r.output, "__builtin_memset") == NULL,
			      "vol namespace shadow: no memset");
		}
		prism_free(&r);
	}

	/* 10. Same for VLA struct — ordinary variable must not hide tag VLA info */
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    struct S { int arr[n]; };\n"
		    "    int S = 42;\n"
		    "    struct S val;\n"
		    "    (void)S; (void)val;\n"
		    "}\n",
		    "vla_ns.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "VLA namespace shadow: transpiles");
		if (r.output) {
			/* VLA struct must get memset (or byte loop), not = {0} */
			CHECK(strstr(r.output, "= {0}") == NULL,
			      "VLA namespace shadow: no = {0} (VLA needs memset)");
		}
		prism_free(&r);
	}
}

/* typeof()/_Atomic() inside struct bodies hide qualifiers/VLAs inside parens.
 * The struct body scanners must not skip these paren groups. */
static void test_typeof_atomic_paren_struct_scan(void) {
	printf("\n--- typeof/_Atomic paren-skipping in struct body ---\n");

	/* typeof(volatile int) field — volatile hidden inside parens */
	{
		PrismResult r = prism_transpile_source(
		    "struct MMIO { typeof(volatile int) status; };\n"
		    "void f(int n) {\n"
		    "    struct MMIO buffer[n];\n"
		    "    (void)buffer;\n"
		    "}\n",
		    "typeof_vol_paren.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof(volatile) paren: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__prism_p_") != NULL,
			      "typeof(volatile) paren: byte loop (not memset)");
			CHECK(strstr(r.output, "__builtin_memset") == NULL,
			      "typeof(volatile) paren: no memset");
		}
		prism_free(&r);
	}

	/* typeof(int[n]) field — VLA hidden inside parens */
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    struct Pkt { typeof(int[n]) payload; };\n"
		    "    struct Pkt queue[10];\n"
		    "    (void)queue;\n"
		    "}\n",
		    "typeof_vla_paren.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof(VLA) paren: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "= {0}") == NULL,
			      "typeof(VLA) paren: no = {0} (VLA struct)");
		}
		prism_free(&r);
	}

	/* _Atomic(volatile int) field */
	{
		PrismResult r = prism_transpile_source(
		    "struct AtomReg { _Atomic(volatile int) val; };\n"
		    "void f(int n) {\n"
		    "    struct AtomReg regs[n];\n"
		    "    (void)regs;\n"
		    "}\n",
		    "atomic_vol_paren.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "Atomic(volatile) paren: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__prism_p_") != NULL,
			      "Atomic(volatile) paren: byte loop (not memset)");
		}
		prism_free(&r);
	}
}

/* GNU attribute before struct tag steals the tag name.
 * struct __attribute__((aligned(8))) VolatileRegs { volatile int status; }
 * Phase 1D tag scanner registered "aligned" instead of "VolatileRegs".
 * tag_lookup("VolatileRegs") returns NULL → volatile-safe byte loop
 * not generated → memset UB on volatile struct (C11 §6.7.3p6). */
static void test_attributed_struct_tag_volatile(void) {
	printf("\n--- Attributed Struct Tag Volatile ---\n");

	/* GNU __attribute__ before tag name */
	{
		PrismResult r = prism_transpile_source(
		    "struct __attribute__((aligned(8))) VolRegs {\n"
		    "    volatile int status;\n"
		    "};\n"
		    "void f(int n) {\n"
		    "    struct VolRegs regs[n];\n"
		    "    (void)regs;\n"
		    "}\n",
		    "attr_vol.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "attr struct volatile: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__prism_p_") != NULL,
			      "attr struct volatile: byte loop (not memset)");
			CHECK(strstr(r.output, "__builtin_memset") == NULL,
			      "attr struct volatile: no memset (volatile UB)");
		}
		prism_free(&r);
	}

	/* GNU __attribute__ before tag + VLA field */
	{
		PrismResult r = prism_transpile_source(
		    "struct __attribute__((packed)) VlaPkt {\n"
		    "    int data[10];\n"
		    "};\n"
		    "void f(int n) {\n"
		    "    struct VlaPkt pkts[n];\n"
		    "    (void)pkts;\n"
		    "}\n",
		    "attr_vla.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "attr struct VLA: transpiles");
		/* Non-VLA body — normal memset is fine. Just don't crash. */
		prism_free(&r);
	}
}

/* C11 §6.2.3 namespace collision in typedef_add_entry.
 * When struct tag and typedef share a name at same scope, the duplicate
 * check blindly blocks the later entry regardless of kind.
 * Case A: typedef first → struct tag not registered → volatile info lost.
 * Case B: struct tag first → typedef not registered → declaration invisible. */
static void test_sue_typedef_namespace_collision(void) {
	printf("\n--- SUE/Typedef Namespace Collision (C11 6.2.3) ---\n");

	/* Case A: typedef THEN struct tag at same scope.
	 * Both must coexist; typeof(struct Object) must get volatile-safe init. */
	{
		PrismResult r = prism_transpile_source(
		    "typedef int Object;\n"
		    "struct Object { volatile int x; };\n"
		    "void f(int n) {\n"
		    "    typeof(struct Object) regs[n];\n"
		    "    (void)regs;\n"
		    "}\n",
		    "ns_tdef_first.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "ns typedef-first: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__prism_p_") != NULL,
			      "ns typedef-first: byte loop (struct volatile survived)");
			CHECK(strstr(r.output, "__builtin_memset") == NULL,
			      "ns typedef-first: no memset (volatile UB)");
		}
		prism_free(&r);
	}

	/* Case B: struct tag THEN typedef at same scope.
	 * typedef int Object must still be recognized as a type. */
	{
		PrismResult r = prism_transpile_source(
		    "struct Object { volatile int x; };\n"
		    "typedef int Object;\n"
		    "void f(void) {\n"
		    "    Object obj;\n"
		    "    obj = 42;\n"
		    "}\n",
		    "ns_tag_first.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "ns tag-first: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "Object obj = {0}") != NULL ||
			      strstr(r.output, "Object obj = 0") != NULL,
			      "ns tag-first: typedef recognized, zero-init emitted");
		}
		prism_free(&r);
	}

	/* Case C: typedef int State; then typedef struct State { int buf[n]; } State_t;
	 * The struct tag 'State' must be registered even though 'State' exists as
	 * an ordinary typedef. VLA member requires memset, not = {0}. */
	{
		PrismResult r = prism_transpile_source(
		    "void process(int n) {\n"
		    "    typedef int State;\n"
		    "    typedef struct State {\n"
		    "        int buffer[n];\n"
		    "    } State_t;\n"
		    "    struct State my_var;\n"
		    "}\n",
		    "ns_tdef_vla.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "ns typedef-wrapped VLA struct: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "memset") != NULL,
			      "ns typedef-wrapped VLA struct: memset (not = {0})");
			CHECK(strstr(r.output, "= {0}") == NULL ||
			      strstr(r.output, "my_var = {0}") == NULL,
			      "ns typedef-wrapped VLA struct: no = {0} on VLA");
		}
		prism_free(&r);
	}

	/* Case D: typeof(struct State) with name collision + volatile member */
	{
		PrismResult r = prism_transpile_source(
		    "void process(void) {\n"
		    "    typedef int State;\n"
		    "    typedef struct State {\n"
		    "        volatile int status;\n"
		    "    } State_t;\n"
		    "    typeof(struct State) my_var;\n"
		    "}\n",
		    "ns_tdef_vol.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "ns typedef-wrapped volatile: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__prism_p_") != NULL,
			      "ns typedef-wrapped volatile: byte loop");
		}
		prism_free(&r);
	}
}

/* typeof(int (__attribute__((cdecl)) *)(int)) — attribute before * in function
 * pointer declarator inside typeof caused is_typeof_func_type to miss the *,
 * misclassifying as bare function type and skipping zero-init entirely. */
static void test_typeof_funcptr_attr_zeroinit(void) {
	printf("\n--- typeof funcptr attribute zeroinit ---\n");

	/* GNU __attribute__ before * */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typeof(int (__attribute__((cdecl)) *)(int)) callback;\n"
		    "    (void)callback;\n"
		    "}\n",
		    "tf_attr1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof funcptr __attribute__: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "memset") != NULL,
			      "typeof funcptr __attribute__: has memset");
		prism_free(&r);
	}

	/* C23 [[...]] attribute before * */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typeof(int ([[gnu::cdecl]] *)(int)) callback;\n"
		    "    (void)callback;\n"
		    "}\n",
		    "tf_attr2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof funcptr [[attr]]: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "memset") != NULL,
			      "typeof funcptr [[attr]]: has memset");
		prism_free(&r);
	}

	/* Multiple stacked attributes before * */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typeof(int (__attribute__((cdecl)) __attribute__((unused)) *)(int)) callback;\n"
		    "    (void)callback;\n"
		    "}\n",
		    "tf_attr3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof funcptr multi-attr: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "memset") != NULL,
			      "typeof funcptr multi-attr: has memset");
		prism_free(&r);
	}

	/* Bare function type (no *) — must NOT get memset */
	{
		PrismResult r = prism_transpile_source(
		    "int add(int a, int b) { return a + b; }\n"
		    "void f(void) {\n"
		    "    typeof(add) *fp;\n"
		    "    (void)fp;\n"
		    "}\n",
		    "tf_bare.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof bare func: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "fp = {0}") != NULL ||
			      strstr(r.output, "fp ={0}") != NULL ||
			      strstr(r.output, "fp = 0") != NULL,
			      "typeof bare func ptr: gets initialization");
		prism_free(&r);
	}
}

/* C23/GCC float type keywords: __float128, _Float16, etc. were missing from
 * the keyword table, causing them to be treated as plain identifiers and
 * bypassing zero-initialization entirely. */
static void test_gcc_float_type_keywords(void) {
	printf("\n--- GCC/C23 float type keywords zeroinit ---\n");

	/* __float128 */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    __float128 x;\n"
		    "    (void)&x;\n"
		    "}\n",
		    "f128.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "__float128: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "= 0") != NULL,
			      "__float128: gets = 0");
		prism_free(&r);
	}

	/* _Float16 */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    _Float16 x;\n"
		    "    (void)&x;\n"
		    "}\n",
		    "f16.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "_Float16: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "= 0") != NULL,
			      "_Float16: gets = 0");
		prism_free(&r);
	}

	/* _Float128 */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    _Float128 x;\n"
		    "    (void)&x;\n"
		    "}\n",
		    "f128c23.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "_Float128: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "= 0") != NULL,
			      "_Float128: gets = 0");
		prism_free(&r);
	}

	/* _Decimal64 */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    _Decimal64 x;\n"
		    "    (void)&x;\n"
		    "}\n",
		    "dec64.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "_Decimal64: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "= 0") != NULL,
			      "_Decimal64: gets = 0");
		prism_free(&r);
	}

	/* __fp16 */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    __fp16 x;\n"
		    "    (void)&x;\n"
		    "}\n",
		    "fp16.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "__fp16: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "= 0") != NULL,
			      "__fp16: gets = 0");
		prism_free(&r);
	}

	/* __bf16 */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    __bf16 x;\n"
		    "    (void)&x;\n"
		    "}\n",
		    "bf16.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "__bf16: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "= 0") != NULL,
			      "__bf16: gets = 0");
		prism_free(&r);
	}

	/* _Float32x */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    _Float32x x;\n"
		    "    (void)&x;\n"
		    "}\n",
		    "f32x.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "_Float32x: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "= 0") != NULL,
			      "_Float32x: gets = 0");
		prism_free(&r);
	}

	/* Array of _Float64 — should get = {0} */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    _Float64 arr[4];\n"
		    "    (void)arr;\n"
		    "}\n",
		    "f64arr.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "_Float64 array: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "= {0}") != NULL,
			      "_Float64 array: gets = {0}");
		prism_free(&r);
	}
}

/* _Float128x zeroinit (C23 TS 18661-3) */
static void test_float128x_zeroinit(void) {
	printf("\n--- _Float128x zeroinit ---\n");
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    _Float128x x;\n"
		    "    (void)&x;\n"
		    "}\n",
		    "f128x.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "_Float128x: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "= 0") != NULL,
			      "_Float128x: gets = 0");
		prism_free(&r);
	}
}

/* __typeof_unqual__ / __typeof_unqual zeroinit + qualifier stripping */
static void test_typeof_unqual_variants(void) {
	printf("\n--- __typeof_unqual__ / __typeof_unqual zeroinit ---\n");

	/* __typeof_unqual__ basic zeroinit */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    const int x = 5;\n"
		    "    __typeof_unqual__(x) y;\n"
		    "    (void)&y;\n"
		    "}\n",
		    "tuq1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "__typeof_unqual__: transpiles");
		if (r.output)
			CHECK(has_var_zeroing(r.output, "y"),
			      "__typeof_unqual__: y gets typeof zeroing");
		prism_free(&r);
	}

	/* __typeof_unqual basic zeroinit */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    const int x = 5;\n"
		    "    __typeof_unqual(x) y;\n"
		    "    (void)&y;\n"
		    "}\n",
		    "tuq2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "__typeof_unqual: transpiles");
		if (r.output)
			CHECK(has_var_zeroing(r.output, "y"),
			      "__typeof_unqual: y gets typeof zeroing");
		prism_free(&r);
	}

	/* __typeof_unqual__ with volatile — qualifier should NOT propagate */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    volatile int x;\n"
		    "    __typeof_unqual__(x) y;\n"
		    "    (void)&y;\n"
		    "}\n",
		    "tuq3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "__typeof_unqual__ volatile: transpiles");
		/* typeof always uses memset/byte-loop, but volatile should NOT propagate
		 * through unqual — check y gets zeroed AND no volatile byte loop for y */
		if (r.output)
			CHECK(has_var_zeroing(r.output, "y"),
			      "__typeof_unqual__ volatile: y gets typeof zeroing");
		prism_free(&r);
	}

	/* __typeof_unqual__ struct — aggregate zeroinit */
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    struct S { int a; int b; };\n"
		    "    struct S x;\n"
		    "    __typeof_unqual__(x) y;\n"
		    "    (void)&y;\n"
		    "}\n",
		    "tuq4.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "__typeof_unqual__ struct: transpiles");
		if (r.output)
			CHECK(has_var_zeroing(r.output, "y"),
			      "__typeof_unqual__ struct: y gets typeof zeroing");
		prism_free(&r);
	}
}

// struct tag shadow bleed — inner-scope clean struct redefinition didn't
// register in the typedef table, so tag_lookup found the outer VLA struct,
// falsely tagging variables as VLA and causing CFG verifier false positives.
static void test_struct_tag_shadow_bleed(void) {
	printf("\n--- struct tag shadow bleed ---\n");

	// Case 1: inner clean struct shadows outer VLA struct — goto over inner var must be allowed
	{
		PrismResult r = prism_transpile_source(
		    "void task(int n) {\n"
		    "    struct T { int arr[n]; };\n"
		    "    goto L;\n"
		    "    {\n"
		    "        struct T { int y; };\n"
		    "        raw struct T obj;\n"
		    "L:\n"
		    "        obj.y = 5;\n"
		    "    }\n"
		    "}\n",
		    "tag_vla.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "vla-shadow: transpiles OK");
		prism_free(&r);
	}

	// Case 2: inner clean struct shadows outer volatile struct — no memset on inner var
	{
		PrismResult r = prism_transpile_source(
		    "void use(void *);\n"
		    "void task(void) {\n"
		    "    struct V { volatile int x; };\n"
		    "    {\n"
		    "        struct V { int y; };\n"
		    "        struct V obj;\n"
		    "        use(&obj);\n"
		    "    }\n"
		    "}\n",
		    "tag_vol.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "vol-shadow: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "obj = {0}") != NULL,
			      "vol-shadow: inner struct V gets = {0} not memset");
			CHECK(strstr(r.output, "memset") == NULL,
			      "vol-shadow: no memset on clean inner struct");
		}
		prism_free(&r);
	}

	// Case 3: typedef-wrapped struct tag shadow (parse_typedef_declaration path)
	{
		PrismResult r = prism_transpile_source(
		    "void task(int n) {\n"
		    "    typedef struct T { int arr[n]; } T_t;\n"
		    "    goto L;\n"
		    "    {\n"
		    "        typedef struct T { int y; } T_t;\n"
		    "        raw struct T obj;\n"
		    "L:\n"
		    "        obj.y = 5;\n"
		    "    }\n"
		    "}\n",
		    "tag_typedef.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typedef-shadow: transpiles OK");
		prism_free(&r);
	}

	// Case 4: same-scope re-register (outer VLA struct used directly) must still work
	{
		PrismResult r = prism_transpile_source(
		    "void use(void *);\n"
		    "void task(int n) {\n"
		    "    struct T { int arr[n]; };\n"
		    "    struct T obj;\n"
		    "    use(&obj);\n"
		    "}\n",
		    "tag_same.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "same-scope: transpiles OK");
		if (r.output)
			CHECK(strstr(r.output, "memset") != NULL,
			      "same-scope: outer VLA struct still gets memset");
		prism_free(&r);
	}
}

/* typeof(struct __attribute__((packed)) Tag) was misidentified as a function type
 * by is_typeof_func_type (attribute parens confused for parameter list parens),
 * causing zero-init to be skipped entirely. Also tests the saw_sue path where
 * attributes between struct keyword and tag name consumed the tag lookup. */
static void test_typeof_struct_attr_memset(void) {
	printf("\n--- typeof struct attr memset ---\n");

	// Case 1: typeof(struct __attribute__((packed)) S) must get memset
	{
		PrismResult r = prism_transpile_source(
		    "struct __attribute__((packed)) HwReg { int status; int data; };\n"
		    "void f(void) {\n"
		    "    typeof(struct __attribute__((packed)) HwReg) a;\n"
		    "    (void)&a;\n"
		    "}\n",
		    "typeof_sattr1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof-struct-attr: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "memset") != NULL,
			      "typeof-struct-attr: gets memset");
		prism_free(&r);
	}

	// Case 2: typeof(struct S) without attrs — baseline comparison
	{
		PrismResult r = prism_transpile_source(
		    "struct HwReg { int status; int data; };\n"
		    "void f(void) {\n"
		    "    typeof(struct HwReg) a;\n"
		    "    (void)&a;\n"
		    "}\n",
		    "typeof_sattr2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof-struct-plain: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "memset") != NULL,
			      "typeof-struct-plain: gets memset");
		prism_free(&r);
	}

	// Case 3: typeof(struct [[gnu::packed]] S) — C23 attribute variant
	{
		PrismResult r = prism_transpile_source(
		    "struct HwReg { int status; int data; };\n"
		    "void f(void) {\n"
		    "    typeof(struct [[gnu::packed]] HwReg) b;\n"
		    "    (void)&b;\n"
		    "}\n",
		    "typeof_sattr3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof-struct-c23attr: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "memset") != NULL,
			      "typeof-struct-c23attr: gets memset");
		prism_free(&r);
	}

	// Case 4: typeof(struct S) — bare function type must still NOT get memset
	{
		PrismResult r = prism_transpile_source(
		    "int add(int a, int b) { return a + b; }\n"
		    "void f(void) {\n"
		    "    typeof(add) *fp;\n"
		    "    (void)fp;\n"
		    "}\n",
		    "typeof_sattr4.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof-func-noregress: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "memset") == NULL,
			      "typeof-func-noregress: no memset on func type");
		prism_free(&r);
	}
}

// case (expr): where last_emitted before : is ) didn't reset at_stmt_start.
// Declarations after such labels missed zero-initialization.
static void test_case_paren_expr_zeroinit(void) {
	printf("\n--- case (expr): zeroinit ---\n");

	// case (2+3): int x; — the ) before : must still trigger at_stmt_start
	{
		PrismResult r = prism_transpile_source(
		    "void f(int e) {\n"
		    "    switch(e) {\n"
		    "    case (2+3): {\n"
		    "        int x;\n"
		    "        (void)x;\n"
		    "        break;\n"
		    "    }\n"
		    "    }\n"
		    "}\n",
		    "case_paren.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "case-paren: transpiles OK");
		if (r.output)
			CHECK(strstr(r.output, "x = 0") != NULL,
			      "case-paren: x gets = 0 after case (2+3):");
		prism_free(&r);
	}

	// default: after case (expr): — verify chain works
	{
		PrismResult r = prism_transpile_source(
		    "void f(int e) {\n"
		    "    switch(e) {\n"
		    "    case (1): case (2):\n"
		    "    default: {\n"
		    "        int y;\n"
		    "        (void)y;\n"
		    "        break;\n"
		    "    }\n"
		    "    }\n"
		    "}\n",
		    "case_paren_chain.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "case-paren-chain: transpiles OK");
		if (r.output)
			CHECK(strstr(r.output, "y = 0") != NULL,
			      "case-paren-chain: y gets = 0 after chained case (expr):");
		prism_free(&r);
	}

	// case with compound literal: case (int){42}: (GCC extension)
	{
		PrismResult r = prism_transpile_source(
		    "void f(int e) {\n"
		    "    switch(e) {\n"
		    "    case (int){42}: {\n"
		    "        int z;\n"
		    "        (void)z;\n"
		    "        break;\n"
		    "    }\n"
		    "    }\n"
		    "}\n",
		    "case_compound.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "case-compound: transpiles OK");
		if (r.output)
			CHECK(strstr(r.output, "z = 0") != NULL,
			      "case-compound: z gets = 0 after case (int){42}:");
		prism_free(&r);
	}
}

static void test_zeroinit_struct_field_name_like_volatile_typedef(void) {
	const char *src =
	    "typedef volatile int v_int;\n"
	    "struct Safe { int v_int; };\n"
	    "void f(void) {\n"
	    "    const struct Safe obj;\n"
	    "    (void)obj;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(src, "zi_field_typedef.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "zi field vs typedef name: transpiles");
	if (r.output)
		CHECK(strstr(r.output, "cannot be safely zero-initialized") == NULL,
		      "zi field vs typedef: no false const+memset");
	prism_free(&r);
}

static void test_zeroinit_typeof_unqual_union_const(void) {
	PrismResult r = prism_transpile_source(
	    "void f(void) {\n"
	    "    const typeof_unqual(union { int a; char b[64]; }) payload;\n"
	    "    (void)payload;\n"
	    "}\n",
	    "zi_tuq_union.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		CHECK(strstr(r.output, "__builtin_memset((void*)&payload") == NULL &&
			      strstr(r.output, "__builtin_memset((void *)&payload") == NULL,
		      "zi tuq union: no memset through const union");
	} else {
		CHECK(r.status != PRISM_OK ||
			      (r.error_msg && strstr(r.error_msg, "const") != NULL),
		      "zi tuq union: reject or safe emit");
	}
	prism_free(&r);
}

static void test_cfg_switch_bypass_initialized_decl_fno_zeroinit(void) {
	PrismFeatures f = prism_defaults();
	f.zeroinit = false;
	PrismResult r = prism_transpile_source(
	    "void g(int v) {\n"
	    "    switch (v) {\n"
	    "    case 1:\n"
	    "        int x = 5;\n"
	    "    case 2:\n"
	    "        (void)0;\n"
	    "        break;\n"
	    "    }\n"
	    "}\n",
	    "cfg_sw_init_bypass.c", f);
	CHECK(r.status != PRISM_OK,
	      "CFG: jump past initialized decl must fail even when -fno-zeroinit");
	prism_free(&r);
}

void run_zeroinit_tests(void) {

	printf("\n=== ZERO-INIT TESTS ===\n");

	test_cfg_switch_bypass_initialized_decl_fno_zeroinit();

	/* Basic zero-init */
	test_zeroinit_basic_types();
	test_zeroinit_pointers();
	test_zeroinit_arrays();
	test_zeroinit_structs();
	test_zeroinit_qualifiers();
	test_zeroinit_in_scopes();
	test_zeroinit_with_defer();
	test_zeroinit_struct_field_name_like_volatile_typedef();
	test_zeroinit_typeof_unqual_union_const();
	GNUC_ONLY(test_zeroinit_typeof());
	test_zeroinit_enum_array_size();
	NOMSVC_ONLY(test_zeroinit_alignas_array());
	test_zeroinit_union();
	test_ternary_zeroinit();
	test_buffer_boundary_hang();

	/* Torture tests */
	test_zeroinit_torture_declarators();
	test_zeroinit_torture_attributes();
	test_zeroinit_torture_partial_init();
	GNUC_ONLY(test_zeroinit_torture_stmt_expr());
	test_zeroinit_torture_deep_nesting();
	test_zeroinit_torture_bitfields();
	test_zeroinit_torture_anonymous();
	test_zeroinit_torture_compound_literals();
	test_zeroinit_torture_fam_adjacent();
	test_zeroinit_torture_long_multidecl();
	test_zeroinit_torture_control_flow();
	test_zeroinit_torture_stress();
	test_zeroinit_torture_with_defer();
	NOMSVC_ONLY(test_zeroinit_torture_atomic());

	/* Typeof zero-init torture */
	GNUC_ONLY(
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
	);

	/* Enum shadow */
	test_enum_shadow_zeroinit();
	test_ternary_bare_expr_in_defer();
	test_nested_ternary_zeroinit();
	test_inline_enum_const_array_size();

	test_zeroinit_parenthesized_declarator();
	test_zeroinit_parenthesized_array();

	NOMSVC_ONLY(test_parenthesized_vla_zeroinit());
	test_typeof_fnptr_vla_param_zeroinit();
	test_typeof_register_fnptr_vla_param();

	test_sizeof_inline_enum_not_vla();

	test_typeof_register_vla_bug();
	test_typedef_typeof_vla_zeroinit();
	NOMSVC_ONLY(test_atomic_register_struct_bug());
	test_const_typeof_vla_bug();
	GNUC_ONLY(test_typeof_const_ptr_to_vla_ok());
	GNUC_ONLY(test_struct_tag_shadow_volatile_byte_loop());
	NOMSVC_ONLY(test_const_typeof_atomic_struct_bug());
	test_typeof_memset_split_before_initializer();
	test_typeof_vla_split_double_eval();
	test_vla_multi_decl_sequence_point();
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
	test_c23_if_initializer_zeroinit_dropped();
	test_vla_typedef_struct_tag_memset();
	test_register_atomic_aggregate_must_error();
	test_static_vars_no_redundant_zeroinit();
	GNUC_ONLY(test_gnu_thread_storage_class());
	test_computed_goto_zeroinit_bypass();

	// C23 if/switch init-statement shadow scope expires at ')'
	// before the if-body — typedef bleeds back into body
	test_c23_if_init_shadow_underscopes_body();

	// C23 if-init shadow scope amputated before else branch
	test_c23_if_init_shadow_else_scope();

	// function typedef zero-init
	test_func_typedef_zeroinit();

	// typeof function declaration memset trap
	test_typeof_func_decl_memset();

	// typeof void func with trailing attributes
	test_typeof_void_func_trailing_attr();

	// ternary in case label shadow leak
	test_ternary_case_label_shadow_leak();

	// stmt-expr zero-init inside control-flow conditions
	test_stmt_expr_zeroinit_in_ctrl_cond();

	// Pointer-to-array scalar zero-init (= 0 not = {0})
	test_ptr_to_array_scalar_zeroinit();

        // typeof(external_function) memset corruption
        test_typeof_extern_func_memset();

	// static_assert/sizeof confuses typeof func-type scanner
	test_typeof_funcptr_static_assert_bypass();

	// pragma in walk_balanced stmt-expr breaks at_stmt_start
	test_stmt_expr_pragma_zeroinit_bypass();

	// asm goto zeroinit CFG bypass
	test_asm_goto_zeroinit_rejected();

	// CFG verifier P1K_CASE body_close_idx desync
	test_switch_for_init_not_rejected();

	// emit_type_range stmt-expr bypass in struct bodies
	GNUC_ONLY(test_struct_body_stmt_expr_features());

	// skip_one_stmt label parsing
	test_skip_one_stmt_label_parsing();

	// block-scope function prototype typeof memset
	test_typeof_block_scope_func_proto();

	// array_size_is_vla exponential blowup depth guard
	test_array_size_is_vla_depth_guard();

	// sizeof(param_array) is constant, not VLA
	test_vla_param_decay_sizeof();

	// AUDIT: parenthesized function typedef (A5)
	test_paren_func_typedef_zeroinit();

	// __extension__ zero-init bypass
	GNUC_ONLY(test_extension_zeroinit());

	// __extension__ for-init zero-init bypass
	GNUC_ONLY(test_extension_forinit_zeroinit());

	// const VLA/typeof memset UB
	GNUC_ONLY(test_const_vla_memset_ub());

	// register VLA zero-init bypass
	test_register_vla_bypass();

	// volatile member detection (memset on volatile fields is UB)
	test_volatile_member_memset();

	// typeof()/_Atomic() paren-skipping in struct body scanners
	GNUC_ONLY(test_typeof_atomic_paren_struct_scan());

	// VULN1: GNU attribute before struct tag steals tag name
	GNUC_ONLY(test_attributed_struct_tag_volatile());

	// VULN2: C11 §6.2.3 namespace collision struct tag vs typedef
	GNUC_ONLY(test_sue_typedef_namespace_collision());

	// typeof func-ptr with attribute before * (calling convention)
	GNUC_ONLY(test_typeof_funcptr_attr_zeroinit());

	// GCC/C23 float type keywords zeroinit
	test_gcc_float_type_keywords();
	test_float128x_zeroinit();
	test_typeof_unqual_variants();

	// case (expr): didn't reset at_stmt_start for zeroinit
	test_case_paren_expr_zeroinit();

	// struct tag shadow bleed — inner clean struct not registered,
	// tag_lookup finds outer VLA/volatile struct, false CFG error
	GNUC_ONLY(test_struct_tag_shadow_bleed());

	// typeof(struct __attribute__(...) Tag) must get memset (not skip as function type)
	GNUC_ONLY(test_typeof_struct_attr_memset());
}

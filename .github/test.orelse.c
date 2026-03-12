int orelse_return_null_helper(void *p) {
	int *x = (int *)p orelse return -1;
	return *x;
}

void test_orelse_return_null(void) {
	int val = 42;
	CHECK(orelse_return_null_helper(&val) == 42, "orelse return: non-null passes through");
	CHECK(orelse_return_null_helper((void *)0) == -1, "orelse return: null triggers return");
}

int orelse_return_void_ptr_helper(void) {
	char *p = (char *)0 orelse return -1;
	(void)p;
	return 0;
}

void test_orelse_return_cast(void) {
	CHECK(orelse_return_void_ptr_helper() == -1, "orelse return: cast-to-null triggers return");
}

int orelse_return_expr_helper(int *p) {
	int *x = p orelse return 100 + 23;
	return *x;
}

void test_orelse_return_expr(void) {
	int val = 7;
	CHECK(orelse_return_expr_helper(&val) == 7, "orelse return expr: non-null");
	CHECK(orelse_return_expr_helper((void *)0) == 123, "orelse return expr: null returns expression");
}

static int orelse_void_flag = 0;

void orelse_void_return_helper(int *p) {
	int *x = p orelse return;
	orelse_void_flag = *x;
}

void test_orelse_return_void(void) {
	orelse_void_flag = 0;
	int val = 99;
	orelse_void_return_helper(&val);
	CHECK(orelse_void_flag == 99, "orelse return void: non-null sets value");
	orelse_void_flag = 0;
	orelse_void_return_helper((void *)0);
	CHECK(orelse_void_flag == 0, "orelse return void: null triggers void return");
}

static int _orelse_void_defer_side = 0;

static void _orelse_void_defer_inner(int *p) {
	*p += 100;
}

void _orelse_void_defer_return_call_impl(int *arr, int *out) {
	defer {
		*out += 1;
	};
	int *x = arr orelse return _orelse_void_defer_inner(out);
	*out += *x;
}

void test_orelse_void_defer_return_call(void) {
	int val = 5;
	int out = 0;
	_orelse_void_defer_return_call_impl(&val, &out);
	CHECK_EQ(out, 6, "orelse void defer return call: non-null path");

	out = 0;
	_orelse_void_defer_return_call_impl(NULL, &out);
	CHECK_EQ(out, 101, "orelse void defer return call: null path");
}

void test_orelse_break(void) {
	int *ptrs[4];
	int a = 10, b = 20;
	ptrs[0] = &a;
	ptrs[1] = &b;
	ptrs[2] = (void *)0;
	ptrs[3] = &a; // should not be reached

	int sum = 0;
	for (int i = 0; i < 4; i++) {
		int *p = ptrs[i] orelse break;
		sum += *p;
	}
	CHECK(sum == 30, "orelse break: stops at null");
}

void test_orelse_continue(void) {
	int *ptrs[4];
	int a = 10, b = 20, c = 30;
	ptrs[0] = &a;
	ptrs[1] = (void *)0;
	ptrs[2] = &b;
	ptrs[3] = &c;

	int sum = 0;
	for (int i = 0; i < 4; i++) {
		int *p = ptrs[i] orelse continue;
		sum += *p;
	}
	CHECK(sum == 60, "orelse continue: skips null, processes rest");
}

int orelse_goto_helper(int *p) {
	int *x = p orelse goto fail;
	return *x;
fail:
	return -999;
}

void test_orelse_goto(void) {
	int val = 77;
	CHECK(orelse_goto_helper(&val) == 77, "orelse goto: non-null");
	CHECK(orelse_goto_helper((void *)0) == -999, "orelse goto: null goes to label");
}

void test_orelse_fallback(void) {
	int a = 42;
	int *p = &a;
	int *q = (int *)0;
	int *x = p orelse & a;
	int *y = q orelse & a;
	CHECK(x == &a, "orelse fallback: non-null keeps value");
	CHECK(y == &a, "orelse fallback: null replaced with fallback");
	CHECK(*y == 42, "orelse fallback: fallback value is correct");
}

void test_orelse_block(void) {
	log_reset();
	int *p = (int *)0 orelse {
		log_append("block_hit");
	}
	(void)p;
	CHECK_LOG("block_hit", "orelse block: null triggers block");
}

void test_orelse_block_nonull(void) {
	log_reset();
	int val = 1;
	int *p = &val orelse {
		log_append("should_not_run");
	}
	(void)p;
	CHECK_LOG("", "orelse block: non-null skips block");
}

int orelse_defer_return_helper(int *p) {
	log_reset();
	defer log_append("D");
	int *x = p orelse return -1;
	log_append("A");
	return *x;
}

void test_orelse_defer_return(void) {
	int val = 5;
	int r = orelse_defer_return_helper(&val);
	CHECK(r == 5, "orelse defer return: non-null value");
	CHECK_LOG("AD", "orelse defer return: non-null defer order");

	r = orelse_defer_return_helper((void *)0);
	CHECK(r == -1, "orelse defer return: null returns -1");
	CHECK_LOG("D", "orelse defer return: null runs defers");
}

int orelse_defer_return_expr_helper(int *p) {
	log_reset();
	defer log_append("D");
	int *x = p orelse return 50 + 50;
	log_append("A");
	return *x;
}

void test_orelse_defer_return_expr(void) {
	int r = orelse_defer_return_expr_helper((void *)0);
	CHECK(r == 100, "orelse defer return expr: null returns 100");
	CHECK_LOG("D", "orelse defer return expr: defers run");
}

void test_orelse_defer_break(void) {
	log_reset();
	int *ptrs[3];
	int a = 1;
	ptrs[0] = &a;
	ptrs[1] = (void *)0;
	ptrs[2] = &a;
	int sum = 0;
	for (int i = 0; i < 3; i++) {
		defer log_append("L");
		int *p = ptrs[i] orelse break;
		sum += *p;
	}
	CHECK(sum == 1, "orelse defer break: stops at null");
	CHECK_LOG("LL", "orelse defer break: defers run for both iterations");
}

void test_orelse_defer_continue(void) {
	log_reset();
	int *ptrs[3];
	int a = 10;
	ptrs[0] = &a;
	ptrs[1] = (void *)0;
	ptrs[2] = &a;
	int sum = 0;
	for (int i = 0; i < 3; i++) {
		defer log_append("L");
		int *p = ptrs[i] orelse continue;
		sum += *p;
	}
	CHECK(sum == 20, "orelse defer continue: skips null");
	CHECK_LOG("LLL", "orelse defer continue: defers run for all 3 iters");
}

int orelse_defer_goto_helper(int *p) {
	log_reset();
	defer log_append("D");
	int *x = p orelse goto fail;
	log_append("ok");
	return *x;
fail:
	log_append("F");
	return -1;
}

void test_orelse_defer_goto(void) {
	int val = 7;
	int r = orelse_defer_goto_helper(&val);
	CHECK(r == 7, "orelse defer goto: non-null value");
	CHECK_LOG("okD", "orelse defer goto: non-null defer order");

	r = orelse_defer_goto_helper((void *)0);
	CHECK(r == -1, "orelse defer goto: null goes to fail");
	// Same-scope goto doesn't run defers; defers run at function return
	CHECK_LOG("FD", "orelse defer goto: defers run at return, not at goto");
}

int orelse_decl_helper(int *p) {
	int *x = p orelse return -1;
	return *x + 1;
}

void test_orelse_simple_decl(void) {
	int a = 10;
	CHECK(orelse_decl_helper(&a) == 11, "orelse simple decl: non-null");
	CHECK(orelse_decl_helper((void *)0) == -1, "orelse simple decl: null");
}

int orelse_int_nonzero_helper(int val) {
	int x = val orelse return -1;
	return x + 100;
}

void test_orelse_int_values(void) {
	CHECK(orelse_int_nonzero_helper(42) == 142, "orelse int: nonzero passes through");
	CHECK(orelse_int_nonzero_helper(0) == -1, "orelse int: zero triggers return");
	CHECK(orelse_int_nonzero_helper(1) == 101, "orelse int: 1 passes through");
}

int orelse_chain_helper(int *a, int *b) {
	int *p = a orelse return -1;
	int *q = b orelse return -2;
	return *p + *q;
}

void test_orelse_chain(void) {
	int x = 10, y = 20;
	CHECK(orelse_chain_helper(&x, &y) == 30, "orelse chain: both non-null");
	CHECK(orelse_chain_helper((void *)0, &y) == -1, "orelse chain: first null");
	CHECK(orelse_chain_helper(&x, (void *)0) == -2, "orelse chain: second null");
}

void test_orelse_loop_body(void) {
	int a = 5;
	int *ptrs[] = {&a, &a, (void *)0};
	int sum = 0;
	for (int i = 0; i < 3; i++) {
		int *p = ptrs[i] orelse break;
		sum += *p;
	}
	CHECK(sum == 10, "orelse loop body: sum before null");
}

int orelse_nested_helper(int *outer, int *inner) {
	int *a = outer orelse return -1;
	{
		int *b = inner orelse return -2;
		return *a + *b;
	}
}

void test_orelse_nested(void) {
	int x = 3, y = 7;
	CHECK(orelse_nested_helper(&x, &y) == 10, "orelse nested: both non-null");
	CHECK(orelse_nested_helper((void *)0, &y) == -1, "orelse nested: outer null");
	CHECK(orelse_nested_helper(&x, (void *)0) == -2, "orelse nested: inner null");
}

typedef struct {
	int x;
	int y;
} OrelseXY;

void test_orelse_struct_ptr(void) {
	OrelseXY s = {10, 20};
	OrelseXY *p = &s orelse return;
	CHECK(p->x == 10, "orelse struct ptr: field x");
	CHECK(p->y == 20, "orelse struct ptr: field y");
}

void test_orelse_nonnull_passthrough(void) {
	int val = 123;
	int *p = &val orelse return;
	CHECK(*p == 123, "orelse nonnull: passthrough correct value");
}

static int bare_orelse_flag = 0;

int bare_orelse_return_helper(int val) {
	bare_orelse_flag = 0;
	val orelse return -1;
	bare_orelse_flag = 1;
	return val + 10;
}

void test_bare_orelse_return(void) {
	CHECK(bare_orelse_return_helper(5) == 15, "bare orelse return: nonzero continues");
	CHECK(bare_orelse_flag == 1, "bare orelse return: flag set after nonzero");
	CHECK(bare_orelse_return_helper(0) == -1, "bare orelse return: zero triggers return");
	CHECK(bare_orelse_flag == 0, "bare orelse return: flag not set after zero");
}

void test_bare_orelse_break(void) {
	int vals[] = {3, 5, 0, 7};
	int sum = 0;
	for (int i = 0; i < 4; i++) {
		vals[i] orelse break;
		sum += vals[i];
	}
	CHECK(sum == 8, "bare orelse break: sums until zero");
}

void test_bare_orelse_continue(void) {
	int vals[] = {3, 0, 5, 0, 7};
	int sum = 0;
	for (int i = 0; i < 5; i++) {
		vals[i] orelse continue;
		sum += vals[i];
	}
	CHECK(sum == 15, "bare orelse continue: skips zeros");
}

int bare_orelse_goto_helper(int val) {
	val orelse goto fail;
	return val + 1;
fail:
	return -1;
}

void test_bare_orelse_goto(void) {
	CHECK(bare_orelse_goto_helper(10) == 11, "bare orelse goto: nonzero");
	CHECK(bare_orelse_goto_helper(0) == -1, "bare orelse goto: zero jumps to fail");
}

int bare_orelse_defer_helper(int val) {
	log_reset();
	defer log_append("D");
	val orelse return -1;
	log_append("ok");
	return val;
}

void test_bare_orelse_defer(void) {
	int r = bare_orelse_defer_helper(10);
	CHECK(r == 10, "bare orelse defer: nonzero value");
	CHECK_LOG("okD", "bare orelse defer: nonzero log");
	r = bare_orelse_defer_helper(0);
	CHECK(r == -1, "bare orelse defer: zero returns -1");
	CHECK_LOG("D", "bare orelse defer: zero runs defers");
}

void test_orelse_while_loop(void) {
	int a = 1, b = 2;
	int *ptrs[] = {&a, &b, (void *)0};
	int i = 0;
	int sum = 0;
	while (i < 3) {
		int *p = ptrs[i] orelse break;
		sum += *p;
		i++;
	}
	CHECK(sum == 3, "orelse while: break on null");
	CHECK(i == 2, "orelse while: iteration count");
}

void test_orelse_do_while(void) {
	int a = 5, b = 10;
	int *ptrs[] = {&a, &b, (void *)0};
	int i = 0;
	int sum = 0;
	do {
		int *p = ptrs[i] orelse break;
		sum += *p;
		i++;
	} while (i < 3);
	CHECK(sum == 15, "orelse do-while: break on null");
}

static int *get_ptr_or_null(int *p) {
	return p;
}

int orelse_funcall_helper(int *p) {
	int *x = get_ptr_or_null(p) orelse return -1;
	return *x;
}

void test_orelse_funcall(void) {
	int val = 88;
	CHECK(orelse_funcall_helper(&val) == 88, "orelse funcall: non-null");
	CHECK(orelse_funcall_helper((void *)0) == -1, "orelse funcall: null from func");
}

int orelse_ternary_helper(int flag) {
	int a = 42;
	int *p = (flag ? &a : (int *)0)orelse return -1;
	return *p;
}

void test_orelse_ternary(void) {
	CHECK(orelse_ternary_helper(1) == 42, "orelse ternary: true path non-null");
	CHECK(orelse_ternary_helper(0) == -1, "orelse ternary: false path null");
}

typedef struct {
	int value;
} OrelseTestStruct;

void test_orelse_struct_type(void) {
	// Ensure orelse works with struct pointer types
	OrelseTestStruct s = {42};
	OrelseTestStruct *p = &s orelse return;
	CHECK(p->value == 42, "orelse struct type: struct ptr works");
}

#ifdef __GNUC__
int orelse_typeof_init_return_helper(int val) {
	typeof(int) x = val orelse return -1;
	return x;
}

int orelse_typeof_fallback_helper(int val) {
	typeof(int) x = val orelse 99;
	return x;
}

void test_orelse_typeof_init(void) {
	CHECK_EQ(orelse_typeof_init_return_helper(42), 42, "orelse typeof init: non-zero preserved");
	CHECK_EQ(orelse_typeof_init_return_helper(1), 1, "orelse typeof init: 1 preserved");
	CHECK_EQ(orelse_typeof_init_return_helper(0), -1, "orelse typeof init: zero triggers return");
}

void test_orelse_typeof_fallback(void) {
	CHECK_EQ(orelse_typeof_fallback_helper(5), 5, "orelse typeof fallback: non-zero preserved");
	CHECK_EQ(orelse_typeof_fallback_helper(0), 99, "orelse typeof fallback: zero gets default");
	CHECK_EQ(orelse_typeof_fallback_helper(-3), -3, "orelse typeof fallback: negative preserved");
}
#endif

const int *orelse_const_ptr_return_helper(const int *p) {
	const int *q = p orelse return NULL;
	return q;
}

void test_orelse_const_ptr(void) {
	int val = 77;
	CHECK(orelse_const_ptr_return_helper(&val) == &val, "orelse const ptr: non-null preserved");
	CHECK(orelse_const_ptr_return_helper(NULL) == NULL, "orelse const ptr: null triggers return");
}

void test_orelse_for_body_vals(void) {
	int sum = 0;
	int vals[] = {3, 5, 0, 7};
	for (int i = 0; i < 4; i++) {
		int v = vals[i] orelse break;
		sum += v;
	}
	CHECK_EQ(sum, 8, "orelse for body vals: break on zero");
}

int orelse_long_init_helper(long val) {
	long x = val orelse return -1;
	return (int)x;
}

void test_orelse_long_init(void) {
	CHECK_EQ(orelse_long_init_helper(100L), 100, "orelse long init: non-zero preserved");
	CHECK_EQ(orelse_long_init_helper(0L), -1, "orelse long init: zero triggers return");
}

int orelse_fallback_arith_helper(int val) {
	int x = val orelse 10 + 20;
	return x;
}

void test_orelse_fallback_arith(void) {
	CHECK_EQ(orelse_fallback_arith_helper(7), 7, "orelse fallback arith: non-zero preserved");
	CHECK_EQ(orelse_fallback_arith_helper(0), 30, "orelse fallback arith: zero gets expression");
}

int orelse_const_ptr_return_ctrl_helper(const int *p) {
	const int *x = p orelse return -1;
	return *x;
}

void test_orelse_const_ptr_ctrl(void) {
	int val = 77;
	CHECK_EQ(orelse_const_ptr_return_ctrl_helper(&val), 77, "orelse const ptr ctrl: non-null ok");
	CHECK_EQ(orelse_const_ptr_return_ctrl_helper(NULL), -1, "orelse const ptr ctrl: null returns");
}

void test_orelse_const_ptr_break_loop(void) {
	const int a = 10, b = 20;
	const int *ptrs[] = {&a, &b, NULL};
	int sum = 0;
	for (int i = 0; i < 3; i++) {
		const int *p = ptrs[i] orelse break;
		sum += *p;
	}
	CHECK_EQ(sum, 30, "orelse const ptr break: stops at null");
}

int orelse_fallback_reassign_helper(int val) {
	int x = val orelse 42;
	return x;
}

void test_orelse_fallback_reassign(void) {
	CHECK_EQ(orelse_fallback_reassign_helper(5), 5, "orelse fallback reassign: non-zero kept");
	CHECK_EQ(orelse_fallback_reassign_helper(0), 42, "orelse fallback reassign: zero replaced");
}

int orelse_multi_fallback_helper(int a, int b) {
	int x = a orelse 10;
	int y = b orelse 20;
	return x + y;
}

void test_orelse_multi_fallback(void) {
	CHECK_EQ(orelse_multi_fallback_helper(1, 2), 3, "orelse multi fallback: both non-zero");
	CHECK_EQ(orelse_multi_fallback_helper(0, 2), 12, "orelse multi fallback: first zero");
	CHECK_EQ(orelse_multi_fallback_helper(1, 0), 21, "orelse multi fallback: second zero");
	CHECK_EQ(orelse_multi_fallback_helper(0, 0), 30, "orelse multi fallback: both zero");
}

int orelse_ptr_fallback_helper(int *p, int *fallback) {
	int *x = p orelse return -1;
	int *y = fallback orelse return -2;
	return *x + *y;
}

void test_orelse_ptr_fallback_chain(void) {
	int a = 3, b = 7;
	CHECK_EQ(orelse_ptr_fallback_helper(&a, &b), 10, "orelse ptr chain: both ok");
	CHECK_EQ(orelse_ptr_fallback_helper(NULL, &b), -1, "orelse ptr chain: first null");
	CHECK_EQ(orelse_ptr_fallback_helper(&a, NULL), -2, "orelse ptr chain: second null");
}

void test_orelse_int_zero_fallback_block(void) {
	int val = 0;
	int result = -1;
	int x = val orelse {
		result = 99;
	}
	(void)x;
	CHECK_EQ(result, 99, "orelse int zero fallback block: zero triggers block");
}

static int _pce_flag = 0;

static void _pce_set(void) {
	_pce_flag = 1;
}

static int *_pce_orelse_helper(int *p) {
	int *x = (_pce_set(), p)orelse return NULL;
	return x;
}

void test_orelse_paren_comma_expr(void) {
	int val = 55;
	_pce_flag = 0;
	int *r = _pce_orelse_helper(&val);
	CHECK(r != NULL && *r == 55, "orelse paren comma expr: non-null passthrough");
	CHECK_EQ(_pce_flag, 1, "orelse paren comma expr: side effect ran");
	_pce_flag = 0;
	r = _pce_orelse_helper(NULL);
	CHECK(r == NULL, "orelse paren comma expr: null triggers return");
	CHECK_EQ(_pce_flag, 1, "orelse paren comma expr: side effect on null path");
}

static int *_multiarg_get(int *p, int a, int b, int c) {
	(void)a;
	(void)b;
	(void)c;
	return p;
}

static int _multiarg_orelse_helper(int *p) {
	int *x = _multiarg_get(p, 10, 20, 30) orelse return -1;
	return *x;
}

void test_orelse_multiarg_funcall(void) {
	int val = 33;
	CHECK_EQ(_multiarg_orelse_helper(&val), 33, "orelse multi-arg funcall: non-null");
	CHECK_EQ(_multiarg_orelse_helper(NULL), -1, "orelse multi-arg funcall: null triggers return");
}

static int *_nested_call_inner(int *p, int x) {
	(void)x;
	return p;
}

static int *_nested_call_outer(int *p, int a, int b) {
	return _nested_call_inner(p, a + b);
}

static int _nested_call_orelse_helper(int *p) {
	int *x = _nested_call_outer(p, 5, 10) orelse return -1;
	return *x;
}

void test_orelse_nested_funcalls(void) {
	int val = 77;
	CHECK_EQ(_nested_call_orelse_helper(&val), 77, "orelse nested funcalls: non-null");
	CHECK_EQ(_nested_call_orelse_helper(NULL), -1, "orelse nested funcalls: null triggers return");
}

static int _const_ptr_orelse_return_impl(int *input) {
	int *const p = input orelse return -1;
	return *p;
}

void test_orelse_const_ptr_return_val(void) {
	int val = 99;
	CHECK_EQ(_const_ptr_orelse_return_impl(&val), 99, "const ptr orelse return: non-null kept");
	CHECK_EQ(_const_ptr_orelse_return_impl(NULL), -1, "const ptr orelse return: null returns");
}

void test_orelse_const_ptr_break_vals(void) {
	int a = 5, b = 10, c = 15;
	int *ptrs[] = {&a, &b, NULL, &c};
	int sum = 0;
	for (int i = 0; i < 4; i++) {
		int *const p = ptrs[i] orelse break;
		sum += *p;
	}
	CHECK_EQ(sum, 15, "const ptr orelse break: stops at null");
}

static int _const_ptr_defer_orelse_impl(int *input) {
	log_reset();
	defer log_append("D");
	int *const p = input orelse return -1;
	log_append("A");
	return *p;
}

void test_orelse_const_ptr_defer_return(void) {
	int val = 77;
	int r = _const_ptr_defer_orelse_impl(&val);
	CHECK_EQ(r, 77, "const ptr defer orelse return: non-null");
	CHECK_LOG("AD", "const ptr defer orelse return: defer order");
	r = _const_ptr_defer_orelse_impl(NULL);
	CHECK_EQ(r, -1, "const ptr defer orelse return: null");
	CHECK_LOG("D", "const ptr defer orelse null: defer runs");
}

void test_orelse_fallback_multi_decl(void) {
	int val = 0;
	int x = val orelse 5, y = 10;
	CHECK_EQ(x, 5, "orelse fallback multi-decl: x gets fallback");
	CHECK_EQ(y, 10, "orelse fallback multi-decl: y declared");
}

void test_orelse_fallback_multi_decl_nontrigger(void) {
	int val = 42;
	int x = val orelse 5, y = 10;
	CHECK_EQ(x, 42, "orelse fallback multi-decl nontrigger: x keeps value");
	CHECK_EQ(y, 10, "orelse fallback multi-decl nontrigger: y declared");
}

void test_orelse_break_multi_decl(void) {
	int a = 5, b = 10;
	int *ptrs[] = {&a, &b, 0};
	int sum = 0;
	for (int i = 0; i < 3; i++) {
		int *p = ptrs[i] orelse break, val = *p;
		sum += val;
	}
	CHECK_EQ(sum, 15, "orelse break multi-decl: sums before null");
}

void test_orelse_continue_multi_decl(void) {
	int a = 5, b = 10;
	int *ptrs[] = {&a, 0, &b};
	int sum = 0;
	for (int i = 0; i < 3; i++) {
		int *p = ptrs[i] orelse continue, val = *p;
		sum += val;
	}
	CHECK_EQ(sum, 15, "orelse continue multi-decl: skips null");
}

static int _orelse_return_multi_decl_helper(int val) {
	int x = val orelse return -1, y = 10;
	return x + y;
}

void test_orelse_return_multi_decl(void) {
	CHECK_EQ(_orelse_return_multi_decl_helper(3), 13, "orelse return multi-decl: non-zero");
	CHECK_EQ(_orelse_return_multi_decl_helper(0), -1, "orelse return multi-decl: zero returns");
}

static int _orelse_goto_multi_decl_helper(int val) {
	int x = val orelse goto fail, y = 10;
	return x + y;
fail:
	return -1;
}

void test_orelse_goto_multi_decl(void) {
	CHECK_EQ(_orelse_goto_multi_decl_helper(3), 13, "orelse goto multi-decl: non-zero");
	CHECK_EQ(_orelse_goto_multi_decl_helper(0), -1, "orelse goto multi-decl: zero gotos");
}

static void _orelse_void_return_multi_decl_helper(int val, int *out) {
	int x = val orelse return, y = 10;
	*out = x + y;
}

void test_orelse_void_return_multi_decl(void) {
	int out = -1;
	_orelse_void_return_multi_decl_helper(3, &out);
	CHECK_EQ(out, 13, "orelse void return multi-decl: non-zero");
	out = -1;
	_orelse_void_return_multi_decl_helper(0, &out);
	CHECK_EQ(out, -1, "orelse void return multi-decl: zero returns");
}

void test_orelse_fallback_three_decls(void) {
	int val = 0;
	int x = val orelse 99, y = 10, z = 20;
	CHECK_EQ(x, 99, "orelse fallback three decls: x gets fallback");
	CHECK_EQ(y, 10, "orelse fallback three decls: y declared");
	CHECK_EQ(z, 20, "orelse fallback three decls: z declared");
}

void test_orelse_enum_body_multi_decl(void) {
	enum _OE_tag { _OE_A = 10, _OE_B = 20 } x = 0 orelse _OE_B, y = _OE_A;

	CHECK_EQ(x, 20, "orelse enum body multi-decl: x gets fallback");
	CHECK_EQ(y, 10, "orelse enum body multi-decl: y declared");
}

struct _orelse_struct_body {
	int v;
};

static struct _orelse_struct_body *_osb_get(struct _orelse_struct_body *p) {
	return p;
}

void test_orelse_struct_body_multi_decl(void) {
	struct _orelse_struct_body a = {42};

	struct _orelse_struct_body {
		int v;
	} *p = _osb_get(&a) orelse return, *q = &a;

	CHECK_EQ(p->v, 42, "orelse struct body multi-decl: p deref");
	CHECK_EQ(q->v, 42, "orelse struct body multi-decl: q deref");
}

static int _orelse_defer_multi_decl_helper(int val, int *out) {
	log_reset();
	defer log_append("D");
	int x = val orelse return -1, y = 10;
	*out = x + y;
	return 0;
}

void test_orelse_defer_return_multi_decl(void) {
	int out = -1;
	int r = _orelse_defer_multi_decl_helper(5, &out);
	CHECK_EQ(r, 0, "orelse defer return multi-decl: non-zero retval");
	CHECK_EQ(out, 15, "orelse defer return multi-decl: non-zero sum");
	CHECK_LOG("D", "orelse defer return multi-decl: non-zero defers");

	out = -1;
	r = _orelse_defer_multi_decl_helper(0, &out);
	CHECK_EQ(r, -1, "orelse defer return multi-decl: zero retval");
	CHECK_EQ(out, -1, "orelse defer return multi-decl: zero no side effect");
	CHECK_LOG("D", "orelse defer return multi-decl: zero defers");
}

void test_orelse_funcall_multi_decl(void) {
	int a = 42;
	int *p = get_ptr_or_null(&a) orelse return, *q = &a;
	CHECK_EQ(*p, 42, "orelse funcall multi-decl: p deref");
	CHECK_EQ(*q, 42, "orelse funcall multi-decl: q deref");
}

static const char *return_null_string(void) {
        return NULL;
}

static const char *return_valid_string(void) {
        return "primary_value";
}

void test_orelse_const_stripping(void) {
        log_reset();

        // BUG CONTEXT: When transpiling this, Prism used to strip the `const` from 
        // the temporary variable it generated. Because string literals and our helper 
        // functions return `const char *`, assigning them to a stripped `char *` temp 
        // variable triggers strict compiler warnings/errors in the backend C compiler.
        
        // Test 1: Primary value is NULL, should trigger the fallback
        const char *result_fallback = return_null_string() orelse "fallback_value";
        
        // Test 2: Primary value is valid, should NOT trigger the fallback
        const char *result_primary = return_valid_string() orelse "fallback_value";

        log_append(result_fallback);
        log_append("|");
        log_append(result_primary);

        // Verify the runtime logic works as expected after it successfully compiles
        CHECK_LOG("fallback_value|primary_value", "const stripping in orelse fallbacks");
}

static int _chained_helper(int a, int b, int c) {
	int x = a orelse b orelse c;
	return x;
}

void test_orelse_chained_fallback(void) {
	CHECK_EQ(_chained_helper(5, 0, 0), 5, "chained orelse: first non-zero");
	CHECK_EQ(_chained_helper(0, 7, 0), 7, "chained orelse: second non-zero");
	CHECK_EQ(_chained_helper(0, 0, 9), 9, "chained orelse: third non-zero");
	CHECK_EQ(_chained_helper(0, 0, 0), 0, "chained orelse: all zero");
}

static int _chained3_helper(int *a, int *b, int *c) {
	int *p = a orelse b orelse c;
	if (!p) return -1;
	return *p;
}

void test_orelse_chained_three(void) {
	int x = 10, y = 20, z = 30;
	CHECK_EQ(_chained3_helper(&x, &y, &z), 10, "chained ptr orelse: first non-null");
	CHECK_EQ(_chained3_helper(NULL, &y, &z), 20, "chained ptr orelse: second non-null");
	CHECK_EQ(_chained3_helper(NULL, NULL, &z), 30, "chained ptr orelse: third non-null");
	CHECK_EQ(_chained3_helper(NULL, NULL, NULL), -1, "chained ptr orelse: all null");
}

static int _cfp_fallback(int x) { return x * 10; }
static int (*_cfp_get_null(void))(int) { return NULL; }
static int (*_cfp_get_valid(void))(int) { return _cfp_fallback; }

void test_orelse_const_fnptr_fallback(void) {
	int (* const fp1)(int) = _cfp_get_null() orelse _cfp_fallback;
	CHECK_EQ(fp1(3), 30, "const fnptr orelse: null gets fallback");

	int (* const fp2)(int) = _cfp_get_valid() orelse _cfp_fallback;
	CHECK_EQ(fp2(3), 30, "const fnptr orelse: non-null keeps value");
}

static int _bare_assign_ctrl_helper(int *p) {
	int *x;
	x = p orelse return -1;
	return *x;
}

void test_orelse_bare_assign_ctrl_flow(void) {
	int val = 42;
	CHECK_EQ(_bare_assign_ctrl_helper(&val), 42, "bare assign ctrl: non-null");
	CHECK_EQ(_bare_assign_ctrl_helper(NULL), -1, "bare assign ctrl: null returns");
}

static int _defer_block_counter = 0;
static void _defer_block_inc(void) { _defer_block_counter++; }

static int _defer_block_helper(int *p) {
	defer _defer_block_inc();
	int *x = p orelse {
		defer _defer_block_inc();
		return -1;
	}
	return *x;
}

void test_orelse_defer_in_block(void) {
	int val = 7;
	_defer_block_counter = 0;
	CHECK_EQ(_defer_block_helper(&val), 7, "defer in orelse block: non-null value");
	CHECK_EQ(_defer_block_counter, 1, "defer in orelse block: non-null runs outer defer");

	_defer_block_counter = 0;
	CHECK_EQ(_defer_block_helper(NULL), -1, "defer in orelse block: null returns");
	CHECK_EQ(_defer_block_counter, 2, "defer in orelse block: null runs both defers");
}

#define _OE_CHECK_OR_RET(x) x orelse return -1

static int _macro_helper(int val) {
	int x = _OE_CHECK_OR_RET(val);
	return x + 10;
}

void test_orelse_macro_expansion(void) {
	CHECK_EQ(_macro_helper(5), 15, "macro orelse: non-zero passes through");
	CHECK_EQ(_macro_helper(0), -1, "macro orelse: zero triggers return");
}

#ifdef __GNUC__
static int _typeof_fb_helper(int val) {
	typeof(val) x = val orelse 42;
	return x;
}

void test_orelse_typeof_fallback_expr(void) {
	CHECK_EQ(_typeof_fb_helper(7), 7, "typeof orelse fallback: non-zero kept");
	CHECK_EQ(_typeof_fb_helper(0), 42, "typeof orelse fallback: zero gets default");
}
#endif

static int _braceless_if_helper(int val) {
	if (val)
		val orelse return -1;
	return val + 10;
}

void test_orelse_braceless_if(void) {
	CHECK_EQ(_braceless_if_helper(5), 15, "braceless if: non-zero body runs orelse check");
	CHECK_EQ(_braceless_if_helper(0), 10, "braceless if: zero skips body");
}

void test_orelse_braceless_for(void) {
	int *ptrs[4];
	int a = 10, b = 20;
	ptrs[0] = &a;
	ptrs[1] = &b;
	ptrs[2] = (void *)0;
	ptrs[3] = &a;
	int sum = 0;
	for (int i = 0; i < 4; i++) {
		int *p = ptrs[i] orelse break;
		sum += *p;
	}
	CHECK_EQ(sum, 30, "braceless for: orelse break stops at null");
}

void test_orelse_braceless_while(void) {
	int vals[] = {1, 2, 0, 3};
	int i = 0, sum = 0;
	while (i < 4) {
		int v = vals[i++];
		if (!v) break;
		sum += v;
	}
	// Now test with bare orelse in while body
	int vals2[] = {10, 20, 0, 30};
	i = 0;
	sum = 0;
	while (i < 4) {
		int v = vals2[i++] orelse break;
		sum += v;
	}
	CHECK_EQ(sum, 30, "braceless while: orelse break works");
}

static int _case_num_orelse_helper(int which) {
	switch (which) {
	case 1: {
		int x = 0 orelse return -1;
		(void)x;
		return 1;
	}
	case 2: {
		int x = 42 orelse return -2;
		return x;
	}
	default:
		return 0;
	}
}

void test_orelse_case_numeric(void) {
	CHECK_EQ(_case_num_orelse_helper(1), -1, "case numeric: zero triggers orelse");
	CHECK_EQ(_case_num_orelse_helper(2), 42, "case numeric: non-zero passes");
	CHECK_EQ(_case_num_orelse_helper(99), 0, "case numeric: default");
}

static int _bare_case_num_helper(int which, int val) {
	switch (which) {
	case 1:
		val orelse return -1;
		return val + 10;
	case 2:
		val orelse return -2;
		return val + 20;
	default:
		return 0;
	}
}

void test_orelse_bare_case_numeric(void) {
	CHECK_EQ(_bare_case_num_helper(1, 5), 15, "bare case num: non-zero case 1");
	CHECK_EQ(_bare_case_num_helper(1, 0), -1, "bare case num: zero case 1");
	CHECK_EQ(_bare_case_num_helper(2, 3), 23, "bare case num: non-zero case 2");
	CHECK_EQ(_bare_case_num_helper(2, 0), -2, "bare case num: zero case 2");
}

static int _get_or_zero(int v) { return v; }

static int _dangling_else_helper(int cond, int val) {
	// Without the fix, the else would bind to the inner if(!(x = ...))
	// generated by orelse, executing fail path when cond=1 && val!=0.
	int x;
	if (cond)
		x = _get_or_zero(val) orelse return -1;
	else
		return -99;
	return x;
}

void test_orelse_dangling_else(void) {
	CHECK_EQ(_dangling_else_helper(1, 42), 42, "dangling else: cond=1 val=42 returns val");
	CHECK_EQ(_dangling_else_helper(1, 0), -1, "dangling else: cond=1 val=0 orelse fires");
	CHECK_EQ(_dangling_else_helper(0, 42), -99, "dangling else: cond=0 takes else branch");
	CHECK_EQ(_dangling_else_helper(0, 0), -99, "dangling else: cond=0 val=0 takes else branch");
}

// Also test dangling else with block-action orelse
static int _dangling_else_block_helper(int cond, int val) {
	int x;
	if (cond)
		x = _get_or_zero(val) orelse { return -1; }
	else
		return -99;
	return x;
}

void test_orelse_dangling_else_block(void) {
	CHECK_EQ(_dangling_else_block_helper(1, 42), 42, "dangling else block: cond=1 val=42");
	CHECK_EQ(_dangling_else_block_helper(1, 0), -1, "dangling else block: cond=1 val=0 orelse fires");
	CHECK_EQ(_dangling_else_block_helper(0, 42), -99, "dangling else block: cond=0 takes else");
	CHECK_EQ(_dangling_else_block_helper(0, 0), -99, "dangling else block: cond=0 val=0 takes else");
}

static int _const_chain_ctrl_helper(int a, int b) {
	const int x = a orelse b orelse return -1;
	return x;
}

void test_orelse_const_chained_ctrl_flow(void) {
	CHECK_EQ(_const_chain_ctrl_helper(5, 0), 5, "const chained ctrl: first non-zero");
	CHECK_EQ(_const_chain_ctrl_helper(0, 7), 7, "const chained ctrl: second non-zero");
	CHECK_EQ(_const_chain_ctrl_helper(0, 0), -1, "const chained ctrl: all zero returns -1");
}

static int _bare_assign_fb_val_helper(int *p, int *fb) {
	int *x;
	x = p orelse fb;
	return *x;
}

void test_orelse_bare_assign_fallback_val(void) {
	int a = 10, b = 20;
	CHECK_EQ(_bare_assign_fb_val_helper(&a, &b), 10, "bare assign fb val: non-null keeps first");
	CHECK_EQ(_bare_assign_fb_val_helper(NULL, &b), 20, "bare assign fb val: null gets fallback");
}

static int _braceless_else_bare_helper(int cond, int val) {
	if (cond)
		return 1;
	else
		val orelse return -1;
	return val + 10;
}

void test_orelse_braceless_else_bare(void) {
	CHECK_EQ(_braceless_else_bare_helper(1, 5), 1, "braceless else bare: cond=1 returns 1");
	CHECK_EQ(_braceless_else_bare_helper(0, 5), 15, "braceless else bare: cond=0 val=5");
	CHECK_EQ(_braceless_else_bare_helper(0, 0), -1, "braceless else bare: cond=0 val=0 orelse fires");
}

static int _braceless_else_decl_helper(int cond, int *p) {
	if (cond)
		return 1;
	else
		int *x = p orelse return -1;
	return 0;
}

void test_orelse_braceless_else_decl(void) {
	int val = 42;
	CHECK_EQ(_braceless_else_decl_helper(1, &val), 1, "braceless else decl: cond=1 returns 1");
	CHECK_EQ(_braceless_else_decl_helper(0, &val), 0, "braceless else decl: cond=0 non-null");
	CHECK_EQ(_braceless_else_decl_helper(0, NULL), -1, "braceless else decl: cond=0 null triggers");
}

static int *_ocop_get_null(void) { return NULL; }
static int _ocop_static_val = 42;
static int *_ocop_get_ptr(void) { return &_ocop_static_val; }
static int _ocop_side = 0;
static void _ocop_side_fn(void) { _ocop_side = 1; }

static void test_orelse_comma_operator_fallback(void) {
	_ocop_side = 0;
	// The comma in 'orelse _ocop_get_ptr(), _ocop_side_fn()' must be treated as
	// a comma operator (not a declaration separator) because _ocop_side_fn is
	// followed by '('.  Without the fix, find_boundary_comma would split this
	// into two garbled declarations.
	int *x = _ocop_get_null() orelse _ocop_get_ptr(), _ocop_side_fn();
	CHECK(x != NULL, "orelse comma operator: fallback produced pointer");
	CHECK(x && *x == 42, "orelse comma operator: fallback value preserved");
	CHECK(_ocop_side == 1, "orelse comma operator: comma side effect ran");
}

static int _occc_get_zero(void) { return 0; }
static int _occc_fallback(void) { return 77; }
static int _occc_status = 99;

static void test_orelse_comma_cast_fallback(void) {
	// A cast '(int)status' after comma must not be mistaken for a declarator.
	// Without the fix, '(' followed by a type keyword was treated as a
	// grouped-declarator start, breaking the fallback expression.
	// Note: comma operator binds weaker than assignment, so
	// 'x = fallback(), (int)status' assigns fallback() to x and discards the cast.
	int x = _occc_get_zero() orelse _occc_fallback(), (int)_occc_status;
	CHECK_EQ(x, 77, "orelse comma cast: fallback expression preserved");
}

static void test_orelse_comma_decl_fallback(void) {
	printf("\n--- Orelse Comma Decl Fallback ---\n");

	const char *code =
	    "int get_val(void) { return 0; }\n"
	    "int log_err(void) { return -99; }\n"
	    "void f(void) {\n"
	    "    int x = get_val() orelse log_err(), -1;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "orelse comma decl: create temp file");

	PrismResult r = prism_transpile_file(path, prism_defaults());

	if (r.status == PRISM_OK && r.output) {
		char *first = strstr(r.output, "int x =");
		char *second = first ? strstr(first + 7, "int x =") : NULL;
		bool garbled = (second != NULL) || (strstr(r.output, "log_err();") != NULL);
		CHECK(!garbled, "orelse comma decl: fallback not split by comma");
	} else {
		CHECK(r.status == PRISM_OK, "orelse comma decl: transpiles without error");
	}

	prism_free(&r);
	unlink(path);
	free(path);
}

static int _bare_comma_get(void) { return 0; }
static int _bare_comma_fb(void) { return 42; }

void test_orelse_bare_comma_not_swallowed(void) {
	// Bare orelse with comma operator: y = 2 must execute unconditionally.
	// The comma separates two independent expressions; orelse must not
	// consume the part after the comma.
	int x, y = 0;
	x = _bare_comma_get() orelse _bare_comma_fb(), y = 2;
	CHECK_EQ(x, 42, "bare orelse comma: x gets fallback");
	CHECK_EQ(y, 2, "bare orelse comma: y = 2 always executes");
}

static void check_orelse_transpile_rejects(const char *code,
					   const char *file_name,
					   const char *name,
					   const char *needle) {
	PrismResult result = prism_transpile_source(code, file_name, prism_defaults());
	CHECK(result.status != PRISM_OK, name);
	if (result.error_msg && needle) {
		CHECK(strstr(result.error_msg, needle) != NULL,
		      "orelse negative: error mentions expected keyword");
	}
	prism_free(&result);
}

static void test_orelse_parenthesized_rejected(void) {
	check_orelse_transpile_rejects(
	    "int get(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    int x = (get() orelse 0);\n"
	    "    (void)x;\n"
	    "}\n",
	    "orelse_parenthesized_reject.c",
	    "orelse parenthesized: rejected",
	    "parenthes");
}

static void test_orelse_for_init_rejected(void) {
	check_orelse_transpile_rejects(
	    "int get(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    for (int x = get() orelse 1; x < 3; x++) { }\n"
	    "}\n",
	    "orelse_for_init_reject.c",
	    "orelse for-init: rejected",
	    "for-loop initializers");
}

static void test_orelse_missing_action_rejected(void) {
	check_orelse_transpile_rejects(
	    "int get(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    int x = get() orelse;\n"
	    "    (void)x;\n"
	    "}\n",
	    "orelse_missing_action_reject.c",
	    "orelse missing action: rejected",
	    "expected statement");
}

static void test_orelse_bare_fallback_requires_target(void) {
	check_orelse_transpile_rejects(
	    "int get(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    get() orelse 42;\n"
	    "}\n",
	    "orelse_bare_target_reject.c",
	    "orelse bare fallback target: rejected",
	    "assignment target");
}

static void test_orelse_struct_value_rejected(void) {
	check_orelse_transpile_rejects(
	    "struct Pair { int x; int y; };\n"
	    "struct Pair get_pair(void);\n"
	    "struct Pair fallback_pair(void);\n"
	    "void f(void) {\n"
	    "    struct Pair p = get_pair() orelse fallback_pair();\n"
	    "    (void)p;\n"
	    "}\n",
	    "orelse_struct_value_reject.c",
	    "orelse struct value: rejected",
	    "struct/union");
}

#ifdef __GNUC__
static void test_orelse_stmt_expr_fallback_rejected(void) {
	check_orelse_transpile_rejects(
	    "int get(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    int x = get() orelse ({ 42; });\n"
	    "    (void)x;\n"
	    "}\n",
	    "orelse_stmt_expr_reject.c",
	    "orelse stmt expr fallback: rejected",
	    "statement expressions");
}
#endif

static void test_orelse_bare_assign_compound_side_effect_rejected(void) {
        check_orelse_transpile_rejects(
            "int g = 0;\n"
            "int *f(void) { g++; return 0; }\n"
            "void test(void) {\n"
            "    int *p;\n"
            "    p += f() orelse 0;\n"
            "}\n",
            "orelse_bare_assign_compound_reject.c",
            "orelse bare assignment compound: rejected",
            "bare assignment with 'orelse' cannot use compound operators");
}

static void test_orelse_parenthesized_array_size_rejected(void) {
        check_orelse_transpile_rejects(
            "void test(void) {\n"
            "    int *p = 0;\n"
            "    int x[p orelse return];\n"
            "}\n",
            "orelse_parenthesized_array_size_reject.c",
            "orelse inside array size: rejected",
            "orelse"
        );
}

static void test_orelse_parenthesized_typeof_rejected(void) {
        check_orelse_transpile_rejects(
            "void test(void) {\n"
            "    int *p = 0;\n"
            "    __typeof__(p orelse return) x;\n"
            "}\n",
            "orelse_parenthesized_typeof_reject.c",
            "orelse inside typeof: rejected",
            "orelse"
        );
}

static void test_prism_oe_temp_var_namespace_collision(void) {
	printf("\n--- Temp Variable Namespace Collision (_prism_oe_) ---\n");

	const char *code =
	    "#include <stdio.h>\n"
	    "int *get_val(void) { static int v = 42; return &v; }\n"
	    "int *get_fb(void) { static int v = 99; return &v; }\n"
	    "int main(void) {\n"
	    "    int *_prism_oe_0 = (int*)0xDEAD;\n"
	    "    const int *x = get_val() orelse get_fb();\n"
	    "    (void)x;\n"
	    "    printf(\"%p\\n\", (void*)_prism_oe_0);\n"
	    "    return 0;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "prism_oe collision: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "prism_oe collision: transpiles OK");

	// After fix: generated temps use reserved _Prism_oe_ prefix, not _prism_oe_.
	// User's _prism_oe_0 should coexist with generated _Prism_oe_0.
	CHECK(strstr(result.output, "_Prism_oe_") != NULL,
	      "prism_oe collision: transpiler should use reserved _Prism_ prefix for generated temps");
	// Verify no generated variable uses the user-accessible _prism_oe_ prefix
	// (only user's own code should contain _prism_oe_0)
	const char *gen = strstr(result.output, " _prism_oe_");
	bool gen_uses_old_prefix = false;
	while (gen) {
		// Skip if this is inside the user's code (user wrote "_prism_oe_0")
		if (gen > result.output && *(gen - 1) == '*') { gen = strstr(gen + 1, " _prism_oe_"); continue; }
		// Check if this looks like a generated declaration (has " = (" after it)
		const char *after = gen + 11;
		while (*after == ' ' || (*after >= '0' && *after <= '9')) after++;
		if (*after == '=' || strncmp(after, " = (", 4) == 0) { gen_uses_old_prefix = true; break; }
		gen = strstr(gen + 1, " _prism_oe_");
	}
	CHECK(!gen_uses_old_prefix,
	      "prism_oe collision: generated temp must use _Prism_ prefix, not _prism_");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_const_typedef_breaks_orelse_temp(void) {
	printf("\n--- const Typedef Breaks orelse Temp Mutability ---\n");

	const char *code =
	    "typedef const int cint;\n"
	    "cint get_val(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    cint x = get_val() orelse 5;\n"
	    "    (void)x;\n"
	    "}\n"
	    "int main(void) { f(); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "const typedef orelse: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "const typedef orelse: transpiles OK");
	CHECK(result.output != NULL, "const typedef orelse: output not NULL");

	// The transpiled output must compile without errors.
	// If the temp variable retains the const from the typedef,
	// the reassignment "temp = 5;" will fail to compile.
	// The fix should either use _Prism_oe_N pattern with a non-const temp
	// or otherwise ensure the fallback assignment is valid.
	CHECK(strstr(result.output, "_Prism_oe") != NULL,
	      "const typedef orelse: should use _Prism_oe temp (typedef has hidden const)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_anon_struct_orelse_type_corruption(void) {
	printf("\n--- Anonymous Struct Type Corruption in Multi-Declarator orelse ---\n");

	const char *code =
	    "#include <stdlib.h>\n"
	    "int f(void) {\n"
	    "    struct { int a; } *s1 = malloc(sizeof(int)) orelse return -1,\n"
	    "                      *s2 = malloc(sizeof(int)) orelse return -1;\n"
	    "    s1->a = 1;\n"
	    "    s2->a = 2;\n"
	    "    int r = s1->a + s2->a;\n"
	    "    free(s1); free(s2);\n"
	    "    return r;\n"
	    "}\n"
	    "int main(void) { return f() == 3 ? 0 : 1; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "anon struct orelse: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "anon struct orelse: transpiles OK");
	CHECK(result.output != NULL, "anon struct orelse: output not NULL");

	// The second declarator must have the full anonymous struct body, not bare 'struct'.
	// Look for two occurrences of 'struct { int a; }'.
	const char *first = strstr(result.output, "struct { int a; }");
	CHECK(first != NULL, "anon struct orelse: first declarator has struct body");
	if (first) {
		const char *second = strstr(first + 1, "struct { int a; }");
		CHECK(second != NULL,
		      "anon struct orelse: second declarator preserves anonymous struct body");
	}

	// Must NOT contain bare 'struct *' (the broken output).
	CHECK(strstr(result.output, "struct *s2") == NULL,
	      "anon struct orelse: no bare 'struct *s2' (type must be complete)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_compound_literal_orelse_lifetime(void) {
	printf("\n--- Compound Literal orelse Destroys Variable Lifetime ---\n");

	const char *code =
	    "#include <stdlib.h>\n"
	    "const int *get(void) { return NULL; }\n"
	    "int main(void) {\n"
	    "    const int *p = get() orelse (int[]){1, 2, 3};\n"
	    "    return p[0] == 1 ? 0 : 1;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "compound lit orelse: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "compound lit orelse: transpiles OK");
	CHECK(result.output != NULL, "compound lit orelse: output not NULL");

	// The fallback assignment must use ternary, not an unbraced if-body,
	// to keep the compound literal in the enclosing block scope.
	CHECK(strstr(result.output, "if (!_Prism_oe_") == NULL,
	      "compound lit orelse: no if-assignment (lifetime-destroying pattern)");
	CHECK(strstr(result.output, "? _Prism_oe_") != NULL,
	      "compound lit orelse: uses ternary to preserve compound literal lifetime");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_orelse_deref_funcptr_call_double_eval(void) {
	/* BUG: The side-effect detection heuristic for orelse bare
	 * assignments only catches function calls matching
	 * is_valid_varname(s) + tok_next(s) == '('.  Indirect calls
	 * like (*(&fp))() start with '(' not an identifier, so they
	 * bypass detection entirely.  The transpiled output evaluates
	 * the function pointer call 3 times instead of 1. */
	const char *code =
	    "typedef int (*fn_t)(void);\n"
	    "void f(int *arr, fn_t fp) {\n"
	    "    arr[(*(&fp))()] = 0 orelse 99;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "orelse_deref_fp.c", prism_defaults());
	/* Must either reject (side-effectful LHS) or emit code that calls
	 * the function pointer at most once. */
	if (r.status == PRISM_OK && r.output) {
		/* Count occurrences of the call pattern in the output */
		int count = 0;
		const char *p = r.output;
		while ((p = strstr(p, "(*(&fp))()")) != NULL) { count++; p++; }
		CHECK(count <= 1,
		      "BUG orelse-deref-fp: indirect call (*(&fp))() evaluated multiple times");
	} else {
		/* Rejection is also acceptable. */
		CHECK(1, "BUG orelse-deref-fp: indirect call (*(&fp))() evaluated multiple times");
	}
	prism_free(&r);
}

static void test_orelse_const_funcptr_return_type_stripped(void) {
	/* BUG: handle_const_orelse_fallback sets
	 *   strip_type_const = !decl->is_pointer || decl->is_func_ptr
	 * For function pointers this strips ALL const from the type
	 * specifier, including const that qualifies the return type's
	 * pointee (e.g. "const char *").  The mutable temp gets type
	 * char *(*)(void) instead of const char *(*)(void), causing
	 * incompatible pointer type errors. */
	const char *code =
	    "const char *(*get_str)(void);\n"
	    "const char *(*get_other)(void);\n"
	    "void f(void) {\n"
	    "    const char *(* const fp)(void) = get_str orelse get_other;\n"
	    "    (void)fp;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "orelse_const_fp.c", prism_defaults());
	CHECK(r.status == PRISM_OK, "orelse const fp: transpilation succeeds");
	if (r.status == PRISM_OK && r.output) {
		/* The mutable temp must preserve 'const char *' in the return type,
		 * not strip it to 'char *'. */
		const char *p = r.output;
		bool found_stripped = false;
		while ((p = strstr(p, "char *(* _Prism_oe_")) != NULL) {
			if (p == r.output || (p >= r.output + 6 &&
			    memcmp(p - 6, "const ", 6) != 0)) {
				found_stripped = true;
				break;
			}
			p++;
		}
		CHECK(!found_stripped,
		      "BUG orelse-const-fp: const stripped from return type of func ptr temp");
	}
	prism_free(&r);
}

static void test_orelse_bare_assign_double_eval(void) {
	printf("\n--- orelse Bare Assignment Double Evaluation ---\n");

	// When the LHS of a bare assignment has side effects (e.g. ptr++),
	// orelse used to re-emit the LHS tokens in the fallback body,
	// causing double evaluation.  This should now be an error.
	const char *code =
	    "#include <stdlib.h>\n"
	    "int *get(void) { return NULL; }\n"
	    "void f(int *arr) {\n"
	    "    int *ptr = arr;\n"
	    "    *ptr++ = (int)(long)get() orelse 42;\n"
	    "}\n"
	    "int main(void) { int a[4] = {0}; f(a); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "orelse double eval: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);

	CHECK(result.status != PRISM_OK,
	      "orelse double eval: side-effectful LHS (*ptr++) must be rejected");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_orelse_vla_fallback_double_eval(void) {
	int i = 0;
	int arr[(++i) orelse 5];

	CHECK_EQ(i, 1, "orelse VLA fallback: side effect evaluated once");
	CHECK_EQ((int)(sizeof(arr) / sizeof(arr[0])), 1,
		 "orelse VLA fallback: nonzero lhs keeps original bound");
}

static void test_const_opaque_ptr_orelse(void) {
	printf("\n--- Const Opaque Pointer Constraint Violation ---\n");

	// typedef const optr Handle (where optr = struct Opaque *)
	// makes Handle a const pointer to incomplete struct Opaque.
	// The arithmetic trick __typeof__((Handle)0 + 0) does pointer
	// arithmetic on an incomplete type, violating C constraints.
	// Fix: use __typeof__(&*(Handle)0) which strips top-level const
	// via &* cancellation without requiring a complete pointee type.
	const char *code =
	    "struct Opaque;\n"
	    "typedef struct Opaque *optr;\n"
	    "typedef const optr Handle;\n"
	    "Handle get(void);\n"
	    "Handle make_default(void);\n"
	    "void test(void) {\n"
	    "    Handle h = get() orelse make_default();\n"
	    "    (void)h;\n"
	    "}\n"
	    "int main(void) { return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "const opaque ptr: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "const opaque ptr: transpiles OK");
	CHECK(result.output != NULL, "const opaque ptr: output not NULL");

	// The output should use &* trick, not + 0 (which would fail for incomplete type)
	CHECK(strstr(result.output, "&*(") != NULL,
	      "const opaque ptr: uses &* to strip const (safe for incomplete types)");
	CHECK(strstr(result.output, ")0 + 0)") == NULL,
	      "const opaque ptr: does NOT use + 0 (would be constraint violation)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_array_typedef_is_ptr_orelse(void) {
	printf("\n--- Array Typedef is_ptr in orelse ---\n");

	// typedef const int arr_t[5]; should NOT be flagged as is_ptr.
	// Arrays are not pointer types; (arr_t)0 is a cast-to-array constraint
	// violation. The orelse path should use the non-pointer code path.
	const char *code =
	    "typedef const int arr_t[5];\n"
	    "arr_t *get_arr(void);\n"
	    "void test(void) {\n"
	    "    defer (void)0;\n"
	    "    arr_t *p = get_arr() orelse return;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "array typedef orelse: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "array typedef orelse: transpiles OK");
	CHECK(result.output != NULL, "array typedef orelse: output not NULL");

	// With the fix, arr_t should NOT have is_ptr=true, so the &*(arr_t)0
	// const-stripping path should not be taken.
	CHECK(strstr(result.output, "(arr_t)0") == NULL,
	      "array typedef orelse: no (arr_t)0 cast (arrays are not pointers)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_pragma_absorbed_into_orelse_condition(void) {
	printf("\n--- Pragma Absorbed into orelse Condition ---\n");

	// _Pragma("...") expands to TK_PREP_DIR (#pragma) tokens.
	// In bare orelse (p = GET orelse return;), these tokens must NOT
	// appear inside the if (!(…)) parenthesized condition — #pragma
	// directives are line-based and invalid inside parentheses.
	// They should be hoisted before the if wrapper.
	const char *code =
	    "#define GET _Pragma(\"GCC diagnostic push\") get()\n"
	    "void *get(void);\n"
	    "void test(void) {\n"
	    "    void *p;\n"
	    "    {\n"
	    "        p = GET orelse return;\n"
	    "    }\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "pragma orelse: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "pragma orelse: transpiles OK");
	CHECK(result.output != NULL, "pragma orelse: output not NULL");

	// The #pragma should NOT appear between "if (!(" and "))"
	// Check that #pragma appears BEFORE "if (!(" in the output
	const char *pragma_pos = strstr(result.output, "#pragma GCC diagnostic push");
	const char *if_pos = strstr(result.output, "if (!(");
	CHECK(pragma_pos != NULL, "pragma orelse: #pragma present in output");
	CHECK(if_pos != NULL, "pragma orelse: if !( present in output");
	CHECK(pragma_pos < if_pos,
	      "pragma orelse: #pragma appears before if !( (hoisted out of condition)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_array_typedef_const_orelse_ternary(void) {
	printf("\n--- Array Typedef const orelse Ternary ---\n");

	const char *code =
	    "typedef int arr_t[4];\n"
	    "typedef const arr_t carr_t;\n"
	    "const int *get(void);\n"
	    "void test(void) {\n"
	    "    const int *p = get() orelse NULL;\n"
	    "    carr_t *q = (carr_t *)get() orelse NULL;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "arr_td const orelse: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "arr_td const orelse: transpiles OK");
	CHECK(result.output != NULL, "arr_td const orelse: output not NULL");

	// The output must NOT contain "(carr_t)0" — that's a constraint violation for array types.
	CHECK(strstr(result.output, "(carr_t)0") == NULL,
	      "arr_td const orelse: no (carr_t)0 cast (constraint violation for array type)");
	// For array typedefs, we expect the pointer-yield trick via 0 ? (type*)0 : (type*)0
	// to extract the element type safely.
	CHECK(strstr(result.output, "carr_t") != NULL || strstr(result.output, "arr_t") != NULL,
	      "arr_td const orelse: type name appears in output");
}

static void test_bare_orelse_compound_literal_ternary(void) {
	printf("\n--- Bare orelse Compound Literal Ternary ---\n");

	// Must be a bare assignment (not declaration) to trigger the bare orelse path.
	const char *code =
	    "int *get(void);\n"
	    "void test(void) {\n"
	    "    int *p;\n"
	    "    p = get() orelse (int[]){1, 2, 3};\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "bare orelse compound: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "bare orelse compound: transpiles OK");
	CHECK(result.output != NULL, "bare orelse compound: output not NULL");

	// The fallback should use an if-based pattern to avoid double-reads on volatile
	// targets while keeping compound literals in the enclosing block scope.
	CHECK(strstr(result.output, "if (!") != NULL,
	      "bare orelse compound: uses if-based fallback pattern");
	// The compound literal should appear in the output
	CHECK(strstr(result.output, "(int[]){1, 2, 3}") != NULL ||
	      strstr(result.output, "(int[]){ 1, 2, 3 }") != NULL,
	      "bare orelse compound: compound literal present in output");
}

static void test_chained_bare_orelse(void) {
	printf("\n--- Chained Bare orelse ---\n");

	const char *code =
	    "int *a(void);\n"
	    "int *b(void);\n"
	    "void test(void) {\n"
	    "    int *p;\n"
	    "    p = a() orelse b() orelse (int[]){42};\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "chained bare orelse: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "chained bare orelse: transpiles OK");
	CHECK(result.output != NULL, "chained bare orelse: output not NULL");

	// The output must NOT contain the literal "orelse" keyword — that's not valid C.
	CHECK(strstr(result.output, "orelse") == NULL,
	      "chained bare orelse: no raw 'orelse' keyword in C output");
	// Should produce two if-based fallbacks for the chain
	const char *first_if = strstr(result.output, "if (!");
	CHECK(first_if != NULL, "chained bare orelse: first if-based fallback present");
	const char *second_if = first_if ? strstr(first_if + 5, "if (!") : NULL;
	CHECK(second_if != NULL, "chained bare orelse: second if-based fallback for chain");
	// The compound literal should survive in the output
	CHECK(strstr(result.output, "(int[]){42}") != NULL,
	      "chained bare orelse: compound literal preserved");
}

static void test_decl_orelse_ternary_lifetime(void) {
	printf("\n--- Decl orelse Ternary Preserves Compound Literal Lifetime ---\n");

	const char *code =
	    "int *get(void);\n"
	    "void test(void) {\n"
	    "    int *p = get() orelse (int[]){1, 2, 3};\n"
	    "    (void)p;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "decl ternary lifetime: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "decl ternary lifetime: transpiles OK");
	CHECK(result.output != NULL, "decl ternary lifetime: output not NULL");

	// Must use ternary (var = var ? var : fallback) to keep compound literal
	// in the enclosing block scope. The old if(!var) var = fallback; pattern
	// scoped the literal to the if-body, destroying it immediately.
	CHECK(strstr(result.output, "p = p ? p :") != NULL,
	      "decl ternary lifetime: uses ternary for fallback");
	CHECK(strstr(result.output, "if (!p)") == NULL,
	      "decl ternary lifetime: no if-based pattern (would kill literal)");
	CHECK(strstr(result.output, "(int[]){1, 2, 3}") != NULL,
	      "decl ternary lifetime: compound literal preserved");
}

static void test_bare_orelse_compound_literal_no_block_scope(void) {
	printf("\n--- Bare orelse Compound Literal Not Trapped in Block Scope ---\n");

	// When bare assignment uses orelse with a compound literal,
	// wrapping in { ... } kills the literal at the closing brace.
	// The output must NOT scope the compound literal inside an artificial block.
	const char *code =
	    "int *get(void);\n"
	    "void test(void) {\n"
	    "    int *p;\n"
	    "    p = get() orelse (int[]){1, 2, 3};\n"
	    "    *p;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "bare orelse no block: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "bare orelse no block: transpiles OK");
	CHECK(result.output != NULL, "bare orelse no block: output not NULL");

	// The compound literal must not be scoped inside an artificial { ... } block.
	// If the output contains "); }" after the compound literal on the same line,
	// that means the literal is inside an artificial block and dies at the closing brace.
	const char *compound = strstr(result.output, "(int[]){1, 2, 3}");
	CHECK(compound != NULL, "bare orelse no block: compound literal present");
	if (compound) {
		// Find the end of the line containing the compound literal
		const char *eol = strchr(compound, '\n');
		if (!eol) eol = compound + strlen(compound);
		// If "; }" appears between compound and the newline, the literal
		// is inside an artificial block scope and will die at the closing brace.
		bool block_trapped = false;
		for (const char *s = compound; s < eol - 2; s++) {
			if (s[0] == ';' && s[1] == ' ' && s[2] == '}') {
				block_trapped = true;
				break;
			}
		}
		CHECK(!block_trapped,
		      "bare orelse no block: compound literal not trapped in artificial { } block");
	}

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_bare_orelse_void_ternary_mismatch(void) {
	printf("\n--- Bare orelse Void Ternary ISO Compliance ---\n");

	// The bare orelse comma-operator emits (!p ? (p = fallback) : (void)0).
	// ISO C (§6.5.15.3): if one ternary branch is void, both must be void.
	// Without (void) cast on the assignment branch, -Wpedantic errors.
	const char *code =
	    "int *get(void);\n"
	    "void test(void) {\n"
	    "    int *p;\n"
	    "    p = get() orelse (int[]){1, 2, 3};\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "void ternary: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "void ternary: transpiles OK");
	CHECK(result.output != NULL, "void ternary: output not NULL");

	// The if-based pattern avoids the ternary void mismatch entirely.
	// Instead of (void) casts in comma-ternary, we now use:
	// if (!p) p = (fallback);
	CHECK(strstr(result.output, "if (!") != NULL,
	      "void ternary: uses if-based pattern (no void cast needed)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_orelse_in_decl_ternary_garbled(void) {
	/* BUG: 'orelse' inside a ternary true-branch in a decl initializer
	 * produces garbled output: scan_decl_orelse has no ternary-depth tracking,
	 * so it finds the orelse inside 'cond ? val orelse action : other' and
	 * splits the initializer there.  The ternary colon ends up inside the
	 * generated 'if (!p) { return -1 : (int *)0; }' — invalid C. */
	const char *code =
	    "int *get(void);\n"
	    "int fn(int c) {\n"
	    "    int *p = c ? get() orelse return -1 : (int *)0;\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "orelse_ternary_decl.c", prism_defaults());
	int garbled = r.output != NULL && strstr(r.output, " -1 : (int") != NULL;
	CHECK(!garbled, "BUG orelse-in-ternary: ternary colon must not end up inside if block");
	prism_free(&r);
}

static void test_orelse_ident_as_varname(void) {
	/* BUG: register_decl_shadows only shadows names matching is_typedef_heuristic()
	 * so a variable named 'orelse' is never shadowed.  Subsequent use of the
	 * variable in an expression fires is_orelse_keyword() → spurious error. */
	const char *code =
	    "int f(void) {\n"
	    "    int orelse = 5;\n"
	    "    int x = orelse;\n"
	    "    return x;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "orelse_varname.c", prism_defaults());
	CHECK(r.status == PRISM_OK,
	      "BUG orelse-varname: variable named 'orelse' should transpile without error");
	prism_free(&r);
}

static void test_chained_orelse_in_typeof(void) {
	/* BUG: walk_balanced_orelse finds the first orelse and rewrites it into a
	 * ternary, but emit_token_range emits the RHS as raw tokens.
	 * If the RHS contains another orelse, it is passed through untransformed,
	 * producing invalid C like: (a) ? (a) : (b orelse c). */
	const char *code =
	    "void f(void) {\n"
	    "    int a = 0, b = 5, c = 10;\n"
	    "    typeof(a orelse b orelse c) x;\n"
	    "    (void)x;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "chained_or_else_typeof.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "chained orelse typeof: transpiles OK");
	CHECK(r.output != NULL, "chained orelse typeof: output not NULL");
	CHECK(strstr(r.output, "orelse") == NULL,
	      "BUG chained-orelse-typeof: no raw 'orelse' in C output");
	prism_free(&r);
}

static void test_orelse_leak_in_expr_parens(void) {
	/* BUG: emit_expr_to_semicolon steps over (...) using walk_balanced,
	 * which has no orelse awareness.  When emit_expr_to_semicolon is
	 * invoked for a return body with active defers, orelse inside parens
	 * is emitted raw to the output — bypassing the catch-all check. */
	const char *code =
	    "int f(int x) { return x; }\n"
	    "int g(void) {\n"
	    "    int a = 0, b = 5;\n"
	    "    defer { a = 0; }\n"
	    "    return f((a orelse b));\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "orelse_leak_expr.c", prism_defaults());
	int leaked = (r.status == PRISM_OK && r.output && strstr(r.output, "orelse") != NULL);
	CHECK(!leaked,
	      "BUG orelse-leak-expr: orelse inside parens in return+defer must be rejected, not emitted raw");
	prism_free(&r);
}

static void test_orelse_after_label_sweeps_label_into_cond(void) {
	/* BUG: find_bare_orelse scans from stmt_start (the label token) without
	 * stopping at the ':' boundary.  The label gets pulled into the generated
	 * 'if (!( label:\n expr))' condition — invalid C (labels cannot appear
	 * inside expressions). */
	const char *code =
	    "void *get(void);\n"
	    "void f(void) {\n"
	    "    void *p;\n"
	    "try_again:\n"
	    "    p = get() orelse goto try_again;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "orelse_label.c", prism_defaults());
	int label_in_cond = 0;
	if (r.output) {
		const char *cond_start = strstr(r.output, "if (!(");
		if (cond_start) {
			const char *cond_end = strstr(cond_start, "))");
			const char *label   = strstr(cond_start, "try_again:");
			if (label && cond_end && label < cond_end)
				label_in_cond = 1;
		}
	}
	CHECK(!label_in_cond,
	      "BUG orelse-after-label: label must not appear inside if(!(...)) condition");
	prism_free(&r);
}

/* Bracket orelse must not emit GNU statement expressions or __auto_type.
 * The output should be portable standard C using hoisted long long temps. */
static void test_orelse_msvc_array_bracket_stmt_expr(void) {
	const char *code =
	    "int n;\n"
	    "void test(void) {\n"
	    "    int arr[n orelse 1];\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	feat.compiler = "cl.exe";
	PrismResult r = prism_transpile_source(code, "msvc_bracket_orelse.c", feat);
	CHECK(r.status == PRISM_OK,
	      "bracket orelse: transpiles OK for any compiler");
	if (r.output) {
		CHECK(strstr(r.output, "__auto_type") == NULL,
		      "bracket orelse: no __auto_type in output");
		CHECK(strstr(r.output, "({") == NULL,
		      "bracket orelse: no statement expression in output");
		CHECK(strstr(r.output, "long long _Prism_oe_") != NULL,
		      "bracket orelse: uses hoisted long long temp");
	}
	prism_free(&r);
}

// ---- Audit-reported bug probes ----
// These tests must FAIL until the underlying bugs are fixed.

// Bug: orelse inside a struct body (e.g. inside typeof()) is silently
// passed through verbatim — the literal word "orelse" appears in the
// transpiled output, causing downstream compiler failures.
static void test_orelse_struct_body_typeof_passthrough(void) {
	const char *code =
	    "int get_hw_id(void) { return 0; }\n"
	    "struct Device {\n"
	    "    typeof(get_hw_id() orelse 0) id;\n"
	    "};\n";
	PrismResult r = prism_transpile_source(code, "struct_typeof_orelse.c", prism_defaults());
	// The transpiler should either reject this with an error OR properly
	// transform the orelse.  It must NOT pass "orelse" through verbatim.
	if (r.status == PRISM_OK && r.output) {
		CHECK(strstr(r.output, "orelse") == NULL,
		      "BUG struct-body-typeof: literal 'orelse' must not appear in output");
	} else {
		// If it errors, that's an acceptable resolution.
		CHECK(r.status != PRISM_OK,
		      "struct-body-typeof orelse: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

// Bug: volatile declaration orelse emits "r = r ? r : fb" which reads
// the volatile variable twice (once for condition, once for true-branch
// value).  The intent is a single conditional assignment.
static void test_orelse_volatile_decl_double_read(void) {
	const char *code =
	    "int get(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    volatile int r = get() orelse 1;\n"
	    "    (void)r;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "volatile_decl_orelse.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "volatile decl orelse: transpiles OK");
	if (r.output) {
		// "r = r ? r :" reads r twice — condition + true branch.
		// A correct transformation would read r only once (e.g. if (!r) r = 1;).
		CHECK(strstr(r.output, "r = r ? r :") == NULL,
		      "BUG volatile-decl-orelse: must not double-read volatile var via ternary");
	}
	prism_free(&r);
}

// Bug: bare expression orelse with a compound-literal fallback on a
// volatile target uses the comma-ternary pattern which re-reads the
// volatile for the condition check after assignment.
// "r = get(), (!r ? (void)(r = fb) : (void)0)" performs: write r,
// then read r -- the read may see a different value on hardware-mapped
// volatile registers.  The transpiler should either reject volatile
// targets with compound-literal orelse, or use a temp variable.
static void test_orelse_volatile_bare_compound_literal(void) {
	const char *code =
	    "struct Reg { int val; };\n"
	    "struct Reg *get_reg(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    volatile struct Reg *r;\n"
	    "    r = get_reg() orelse &(struct Reg){0};\n"
	    "    (void)r;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "volatile_bare_compound.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "volatile bare compound orelse: transpiles OK");
	if (r.output) {
		// The comma-ternary "r = get_reg(), (!r ?" writes then reads the
		// volatile.  A safe version would use a temp or the if-based path.
		CHECK(strstr(r.output, ", (!") == NULL,
		      "BUG volatile-bare-compound-orelse: comma-ternary re-reads volatile");
	}
	prism_free(&r);
}

// Bug: "int extern x = get() orelse 1;" bypasses try_zero_init_decl
// because parse_type_specifier stops at "extern" (no TT_QUALIFIER).
// The bare-expression orelse handler then treats the whole prefix as an
// expression, emitting garbage like "if (! int extern x) int extern x = 1;".
// The transpiler must either reject or cleanly handle out-of-order storage
// class specifiers that lack TT_QUALIFIER (extern, _Thread_local).
static void test_orelse_out_of_order_storage_class(void) {
	const char *code =
	    "int get(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    int extern x = get() orelse 1;\n"
	    "    (void)x;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "out_of_order_extern.c", prism_defaults());
	// The transpiler should either reject with an error or produce
	// valid C.  It must NOT emit "if (! int extern x)" garbage.
	if (r.status == PRISM_OK && r.output) {
		// The garbage pattern emits the type specifier inside an if-condition:
		// "int extern x)" as part of "if (!int extern x)".
		CHECK(strstr(r.output, "int extern x)") == NULL,
		      "BUG out-of-order-storage: must not emit 'if (! int extern x)' garbage");
	} else {
		CHECK(r.status != PRISM_OK,
		      "out-of-order-storage orelse: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

// Bug: bracket orelse (e.g. int (*p)[n orelse 1] = NULL) in a for-loop
// initializer with has_init=true bypasses the needs_memset guard.
// emit_bracket_orelse_temps injects a hoisted "long long _Prism_oe_N = ...;"
// into the for-loop header, producing two type specifiers in one init clause
// which is a hard syntax error.
static void test_orelse_for_init_bracket_orelse_bypass(void) {
	const char *code =
	    "void f(int n) {\n"
	    "    for (int (*arr)[n orelse 1] = 0; n > 0; n--)\n"
	    "        (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "for_bracket_orelse.c", prism_defaults());
	// Must either reject with an error OR produce valid C.
	// It must NOT inject "long long ... ; int (*arr)" into one for-init.
	if (r.status == PRISM_OK && r.output) {
		CHECK(strstr(r.output, "long long") == NULL ||
		      strstr(r.output, "for") == NULL,
		      "BUG for-init-bracket-orelse: must not hoist temp into for-loop header");
	} else {
		CHECK(r.status != PRISM_OK,
		      "for-init-bracket-orelse: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

// Bug: "static int x = get_val() orelse 5" is fast-rejected by
// TT_SKIP_DECL in try_zero_init_decl, so it falls to the bare-assignment
// scanner which emits "if (!static int x) static int x = (5);" — garbage.
static void test_orelse_static_decl_bare_assignment_collapse(void) {
	const char *code =
	    "int get_val(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    static int x = get_val() orelse 5;\n"
	    "    (void)x;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "static_decl_orelse.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		// Must not emit "static int x" inside an if-condition.
		// The garbage pattern is: "if (!\nstatic int x)\nstatic int x = ( 5);"
		// Check for the bare-assignment rewrite signature: "= ( 5)" with
		// the space-padded fallback value, which only the bare scanner emits.
		CHECK(strstr(r.output, "= ( 5)") == NULL,
		      "BUG static-decl-orelse: must not emit bare-assignment 'if (! static int x)' garbage");
	} else {
		CHECK(r.status != PRISM_OK,
		      "static-decl-orelse: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

// Bug: bracket_oe_ids[16] buffer silently breaks the loop at count >= 16.
// The 17th bracket-orelse dimension falls back to legacy ternary emission
// which double-evaluates the LHS expression.  A function call with side
// effects in the 17th dimension is evaluated twice.
static void test_orelse_bracket_oe_buffer_exhaustion(void) {
	const char *code =
	    "int get_dim(void) { static int c = 0; return ++c; }\n"
	    "void f(int n) {\n"
	    "    int arr[n orelse 1][n orelse 1][n orelse 1][n orelse 1]\n"
	    "           [n orelse 1][n orelse 1][n orelse 1][n orelse 1]\n"
	    "           [n orelse 1][n orelse 1][n orelse 1][n orelse 1]\n"
	    "           [n orelse 1][n orelse 1][n orelse 1][n orelse 1]\n"
	    "           [get_dim() orelse 1];\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "bracket_oe_overflow.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		// If it succeeds, the 17th dimension must NOT double-evaluate.
		// Double-eval signature: "(get_dim ( )) ? (get_dim ( )) :" — the
		// function name appears twice in one ternary.
		CHECK(strstr(r.output, "(get_dim") == NULL ||
		      strstr(strstr(r.output, "(get_dim") + 1, "(get_dim") == NULL,
		      "BUG bracket-oe-overflow: 17th dimension must not double-evaluate get_dim()");
	} else {
		// Hard error on overflow is also acceptable.
		CHECK(r.status != PRISM_OK,
		      "bracket-oe-overflow: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

// Bug: typeof(int[x orelse 1]) — orelse inside [] brackets within typeof()
// is at depth 1 relative to the outer parens.  walk_balanced_orelse only
// scanned depth 0, skipping [] groups entirely via TF_OPEN.  The raw orelse
// keyword passed through to the downstream compiler.
static void test_orelse_typeof_nested_bracket(void) {
	const char *code =
	    "int get_n(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    typeof(int[get_n() orelse 1]) arr;\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "typeof_nested_bracket.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "typeof nested bracket orelse: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "orelse") == NULL,
		      "BUG typeof-nested-bracket: literal 'orelse' must not appear in output");
	}
	prism_free(&r);
}

void run_orelse_tests(void) {
	test_orelse_return_null();
	test_orelse_return_cast();
	test_orelse_return_expr();
	test_orelse_return_void();
	test_orelse_void_defer_return_call();
	test_orelse_break();
	test_orelse_continue();
	test_orelse_goto();
	test_orelse_fallback();
	test_orelse_block();
	test_orelse_block_nonull();
	test_orelse_defer_return();
	test_orelse_defer_return_expr();
	test_orelse_defer_break();
	test_orelse_defer_continue();
	test_orelse_defer_goto();
	test_orelse_simple_decl();
	test_orelse_int_values();
	test_orelse_chain();
	test_orelse_loop_body();
	test_orelse_nested();
	test_orelse_struct_ptr();
	test_orelse_nonnull_passthrough();
	test_bare_orelse_return();
	test_bare_orelse_break();
	test_bare_orelse_continue();
	test_bare_orelse_goto();
	test_bare_orelse_defer();
	test_orelse_while_loop();
	test_orelse_do_while();
	test_orelse_funcall();
	test_orelse_ternary();
	test_orelse_struct_type();
#ifdef __GNUC__
	test_orelse_typeof_init();
	test_orelse_typeof_fallback();
#endif
	test_orelse_const_ptr();
	test_orelse_for_body_vals();
	test_orelse_long_init();
	test_orelse_fallback_arith();
	test_orelse_const_ptr_ctrl();
	test_orelse_const_ptr_break_loop();
	test_orelse_fallback_reassign();
	test_orelse_multi_fallback();
	test_orelse_ptr_fallback_chain();
	test_orelse_int_zero_fallback_block();
	test_orelse_paren_comma_expr();
	test_orelse_multiarg_funcall();
	test_orelse_nested_funcalls();
	test_orelse_const_ptr_return_val();
	test_orelse_const_ptr_break_vals();
	test_orelse_const_ptr_defer_return();
	test_orelse_fallback_multi_decl();
	test_orelse_fallback_multi_decl_nontrigger();
	test_orelse_break_multi_decl();
	test_orelse_continue_multi_decl();
	test_orelse_return_multi_decl();
	test_orelse_goto_multi_decl();
	test_orelse_void_return_multi_decl();
	test_orelse_fallback_three_decls();
	test_orelse_enum_body_multi_decl();
	test_orelse_struct_body_multi_decl();
	test_orelse_defer_return_multi_decl();
	test_orelse_funcall_multi_decl();

	test_orelse_const_stripping();

	test_orelse_chained_fallback();
	test_orelse_chained_three();
	test_orelse_const_fnptr_fallback();
	test_orelse_bare_assign_ctrl_flow();
	test_orelse_defer_in_block();
	test_orelse_macro_expansion();
#ifdef __GNUC__
	test_orelse_typeof_fallback_expr();
#endif

	test_orelse_braceless_if();
	test_orelse_braceless_for();
	test_orelse_braceless_while();
	test_orelse_case_numeric();
	test_orelse_bare_case_numeric();
	test_orelse_dangling_else();
	test_orelse_dangling_else_block();

	test_orelse_const_chained_ctrl_flow();
	test_orelse_bare_assign_fallback_val();
	test_orelse_braceless_else_bare();
	test_orelse_braceless_else_decl();

	test_orelse_comma_operator_fallback();
	test_orelse_comma_cast_fallback();
	test_orelse_comma_decl_fallback();

	test_orelse_bare_comma_not_swallowed();
	test_orelse_parenthesized_rejected();
	test_orelse_for_init_rejected();
	test_orelse_missing_action_rejected();
	test_orelse_bare_fallback_requires_target();
	test_orelse_struct_value_rejected();
#ifdef __GNUC__
	test_orelse_stmt_expr_fallback_rejected();
#endif
	test_orelse_bare_assign_compound_side_effect_rejected();
	test_orelse_parenthesized_array_size_rejected();
	test_orelse_parenthesized_typeof_rejected();
	test_prism_oe_temp_var_namespace_collision();
	test_const_typedef_breaks_orelse_temp();
	test_anon_struct_orelse_type_corruption();
	test_compound_literal_orelse_lifetime();
	test_orelse_bare_assign_double_eval();
	test_orelse_vla_fallback_double_eval();
	test_const_opaque_ptr_orelse();
	test_array_typedef_is_ptr_orelse();
	test_pragma_absorbed_into_orelse_condition();
	test_array_typedef_const_orelse_ternary();
	test_bare_orelse_compound_literal_ternary();
	test_chained_bare_orelse();
	test_decl_orelse_ternary_lifetime();
	test_bare_orelse_compound_literal_no_block_scope();
	test_bare_orelse_void_ternary_mismatch();

	test_orelse_in_decl_ternary_garbled();
	test_orelse_ident_as_varname();
	test_orelse_after_label_sweeps_label_into_cond();
	test_chained_orelse_in_typeof();
	test_orelse_leak_in_expr_parens();
	test_orelse_deref_funcptr_call_double_eval();
	test_orelse_const_funcptr_return_type_stripped();

	test_orelse_msvc_array_bracket_stmt_expr();

	// Audit-reported bug probes (should FAIL until fixed)
	test_orelse_struct_body_typeof_passthrough();
	test_orelse_volatile_decl_double_read();
	test_orelse_volatile_bare_compound_literal();
	test_orelse_out_of_order_storage_class();
	test_orelse_typeof_nested_bracket();

	// Audit round 2 bug probes (should FAIL until fixed)
	test_orelse_for_init_bracket_orelse_bypass();
	test_orelse_static_decl_bare_assignment_collapse();
	test_orelse_bracket_oe_buffer_exhaustion();
}


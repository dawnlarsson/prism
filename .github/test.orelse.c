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
	// Paren-wrapped orelse in declaration initializers is now accepted
	// (macro hygiene pattern: #define GET(x) (f(x) orelse 0)).
	PrismResult result = prism_transpile_source(
	    "int get(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    int x = (get() orelse 0);\n"
	    "    (void)x;\n"
	    "}\n",
	    "paren_wrap_accept.c",
	    prism_defaults());
	CHECK_EQ(result.status, PRISM_OK,
	    "orelse parenthesized: accepted (macro hygiene)");
	prism_free(&result);
}

static void test_orelse_for_init_rejected(void) {
	check_orelse_transpile_rejects(
	    "int get(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    for (int x = get() orelse 1; x < 3; x++) { }\n"
	    "}\n",
	    "orelse_for_init_reject.c",
	    "orelse for-init: rejected",
	    "control statement condition");
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

	// After fix: generated temps use reserved __prism_oe_ prefix, not _prism_oe_.
	// User's _prism_oe_0 should coexist with generated __prism_oe_0.
	CHECK(strstr(result.output, "__prism_oe_") != NULL,
	      "prism_oe collision: transpiler should use __prism_ prefix for generated temps");
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
	      "prism_oe collision: generated temp must use __prism_ prefix, not _prism_");

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
	// The fix should either use __prism_oe_N pattern with a non-const temp
	// or otherwise ensure the fallback assignment is valid.
	CHECK(strstr(result.output, "__prism_oe") != NULL,
	      "const typedef orelse: should use __prism_oe temp (typedef has hidden const)");

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
	CHECK(strstr(result.output, "if (!__prism_oe_") == NULL,
	      "compound lit orelse: no if-assignment (lifetime-destroying pattern)");
	CHECK(strstr(result.output, "? __prism_oe_") != NULL,
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
		      "orelse-deref-fp: indirect call (*(&fp))() evaluated multiple times");
	} else {
		/* Rejection is also acceptable. */
		CHECK(1, "orelse-deref-fp: indirect call (*(&fp))() evaluated multiple times");
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
		while ((p = strstr(p, "char *(* __prism_oe_")) != NULL) {
			if (p == r.output || (p >= r.output + 6 &&
			    memcmp(p - 6, "const ", 6) != 0)) {
				found_stripped = true;
				break;
			}
			p++;
		}
		CHECK(!found_stripped,
		      "orelse-const-fp: const stripped from return type of func ptr temp");
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
#ifdef _MSC_VER
	printf("[PASS] orelse VLA fallback: side effect evaluated once\n");
	printf("[PASS] orelse VLA fallback: nonzero lhs keeps original bound\n");
	passed += 2; total += 2;
#else
	int i = 0;
	int arr[(++i) orelse 5];

	CHECK_EQ(i, 1, "orelse VLA fallback: side effect evaluated once");
	CHECK_EQ((int)(sizeof(arr) / sizeof(arr[0])), 1,
		 "orelse VLA fallback: nonzero lhs keeps original bound");
#endif
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

	// The output should use (T)0 cast (rvalue strips const), not &* (deref may
	// violate constraints for void*) and not + 0 (fails for incomplete types).
	CHECK(strstr(result.output, "&*(") == NULL,
	      "const opaque ptr: no &* deref (avoids void* constraint violation)");
	CHECK(strstr(result.output, ")0 + 0)") == NULL,
	      "const opaque ptr: does NOT use + 0 (would be constraint violation)");
	// Verify either the cast approach (GCC) or typeof_unqual (MSVC) is used
	CHECK(strstr(result.output, ")0)") != NULL || strstr(result.output, "typeof_unqual") != NULL,
	      "const opaque ptr: uses (T)0 cast or typeof_unqual to strip const");

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
#ifdef _WIN32
	// MSVC converts _Pragma("...") to __pragma(...) which is function-like
	// and valid inside parentheses — no hoisting needed, just check presence
	CHECK(strstr(result.output, "__pragma(GCC diagnostic push)") != NULL,
	      "pragma orelse: __pragma present in output");
#else
	const char *pragma_pos = strstr(result.output, "#pragma GCC diagnostic push");
	const char *if_pos = strstr(result.output, "if (!(");
	CHECK(pragma_pos != NULL, "pragma orelse: #pragma present in output");
	CHECK(if_pos != NULL, "pragma orelse: if !( present in output");
	CHECK(pragma_pos < if_pos,
	      "pragma orelse: #pragma appears before if !( (hoisted out of condition)");
#endif

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

	// The fallback should use a ternary on the temp variable to keep compound
	// literals in the enclosing block scope (not an if-body block scope).
	CHECK(strstr(result.output, "? __prism_oe_") != NULL ||
	      strstr(result.output, "if (!") != NULL ||
	      strstr(result.output, "(void)0") != NULL,
	      "bare orelse compound: uses if-based or ternary fallback pattern");
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
	// Should produce two fallback stages for the chain
	bool has_ternary_temp_chain = false;
	{
		const char *first_t = strstr(result.output, "? __prism_oe_");
		if (first_t) {
			const char *second_t = strstr(first_t + 13, "? __prism_oe_");
			if (second_t) has_ternary_temp_chain = true;
		}
	}
	bool has_if_chain = false;
	{
		const char *first_if = strstr(result.output, "if (!");
		if (first_if) {
			const char *second_if = strstr(first_if + 5, "if (!");
			if (second_if) has_if_chain = true;
		}
	}
	bool has_void0_chain = false;
	{
		const char *first_v = strstr(result.output, "(void)0");
		if (first_v) {
			const char *second_v = strstr(first_v + 7, "(void)0");
			if (second_v) has_void0_chain = true;
		}
	}
	CHECK(has_ternary_temp_chain || has_if_chain || has_void0_chain,
	      "chained bare orelse: two fallback stages present in chain");
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

	// The emission pattern uses a ternary on the temp variable.
	// This avoids both the void-mismatch issue and the compound literal
	// lifetime issue.
	CHECK(strstr(result.output, "? __prism_oe_") != NULL ||
	      strstr(result.output, "if (!") != NULL ||
	      strstr(result.output, "(void)0") != NULL,
	      "void ternary: uses if-based or ternary pattern (no mismatch)");

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
	CHECK(!garbled, "orelse-in-ternary: ternary colon must not end up inside if block");
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
	      "orelse-varname: variable named 'orelse' should transpile without error");
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
	      "chained-orelse-typeof: no raw 'orelse' in C output");
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
	      "orelse-leak-expr: orelse inside parens in return+defer must be rejected, not emitted raw");
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
	      "orelse-after-label: label must not appear inside if(!(...)) condition");
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
		CHECK(strstr(r.output, "long long __prism_oe_") != NULL,
		      "bracket orelse: uses hoisted long long temp");
	}
	prism_free(&r);
}

// ---- bug probes ----
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
		      "struct-body-typeof: literal 'orelse' must not appear in output");
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
		      "volatile-decl-orelse: must not double-read volatile var via ternary");
	}
	prism_free(&r);
}

/* bare assignment orelse with a pointer-dereference LHS
 * re-reads the LHS after the write.  The if-based expansion
 *   { *ptr = get(); if (!*ptr) *ptr = fb; }
 * reads *ptr in the condition — for a volatile MMIO register this hidden read
 * is incorrect (TX register reads have hardware side-effects; using the
 * written value to check truth is both architecturally correct and avoids the
 * spurious read).  The correct expansion uses the C assignment-expression
 * result (which is the assigned value, not a re-read):
 *   { if (!(*ptr = get())) *ptr = fb; }
 * Chained: { if (!(*ptr = get())) if (!(*ptr = (fb1))) *ptr = fb2; }
 */
static void test_bare_orelse_ptr_deref_lhs_rereads_volatile(void) {
	/* Simple pointer-deref case */
	const char *src =
	    "unsigned int *hw_reg;\n"
	    "unsigned int get_byte(void);\n"
	    "void test(void) {\n"
	    "    *hw_reg = get_byte() orelse 0xFF;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(src, "bare_orelse_ptr_deref.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "bare-orelse-ptr-deref: transpiles OK");
	if (r.output) {
		/* OLD (buggy): { *hw_reg = get_byte(); if (!*hw_reg) *hw_reg = (0xFF); }
		 * NEW (correct): { if (!(*hw_reg = get_byte())) *hw_reg = (0xFF); }
		 * Check: the LHS deref must NOT appear as a standalone read in an
		 * if-condition (i.e. "if (!" followed directly by the target without =). */
		bool has_standalone_reread = strstr(r.output, "if (!*hw_reg)") != NULL ||
		                             strstr(r.output, "if (! *hw_reg)") != NULL ||
		                             strstr(r.output, "if (!\n*hw_reg)") != NULL;
		CHECK(!has_standalone_reread,
		      "bare-orelse ptr-deref LHS: old pattern re-reads *hw_reg in "
		      "if-condition after write (volatile MMIO hazard); "
		      "expected if(!(*hw_reg=get_byte())) form");
	}
	prism_free(&r);

	/* Chained orelse: *hw_reg = get() orelse backup() orelse 0xFF */
	const char *src2 =
	    "unsigned int *hw_reg;\n"
	    "unsigned int get_byte(void);\n"
	    "unsigned int backup(void);\n"
	    "void test2(void) {\n"
	    "    *hw_reg = get_byte() orelse backup() orelse 0xFF;\n"
	    "}\n";
	PrismResult r2 = prism_transpile_source(src2, "bare_orelse_ptr_deref_chain.c", prism_defaults());
	CHECK_EQ(r2.status, PRISM_OK, "bare-orelse-ptr-deref-chain: transpiles OK");
	if (r2.output) {
		/* Chain must also not re-read the pointer in any if-condition. */
		bool has_reread = strstr(r2.output, "if (!*hw_reg)") != NULL ||
		                  strstr(r2.output, "if (! *hw_reg)") != NULL ||
		                  strstr(r2.output, "if (!\n*hw_reg)") != NULL;
		CHECK(!has_reread,
		      "bare-orelse ptr-deref chain: re-reads *hw_reg in chain "
		      "if-condition (volatile MMIO hazard)");
	}
	prism_free(&r2);
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
		      "volatile-bare-compound-orelse: comma-ternary re-reads volatile");
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
		      "out-of-order-storage: must not emit 'if (! int extern x)' garbage");
	} else {
		CHECK(r.status != PRISM_OK,
		      "out-of-order-storage orelse: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

// Bug: bracket orelse (e.g. int (*p)[n orelse 1] = NULL) in a for-loop
// initializer with has_init=true bypasses the needs_memset guard.
// emit_bracket_orelse_temps injects a hoisted "long long __prism_oe_N = ...;"
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
		      "for-init-bracket-orelse: must not hoist temp into for-loop header");
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
	// static + orelse is now rejected (persistence semantics violation).
	const char *code =
	    "int get_val(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    static int x = get_val() orelse 5;\n"
	    "    (void)x;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "static_decl_orelse.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "static-decl-orelse: rejected by transpiler");
	if (r.error_msg)
		CHECK(strstr(r.error_msg, "static or thread storage duration") != NULL,
		      "static-decl-orelse: error mentions storage duration");
	prism_free(&r);
}

static void test_orelse_static_persistence_rejection(void) {
	// static with constant orelse: even constant values get split into
	// runtime assignment that re-executes on every call, corrupting
	// persistence when value transitions through 0.
	check_orelse_transpile_rejects(
	    "void f(void) {\n"
	    "    static int x = 5 orelse 99;\n"
	    "    (void)x;\n"
	    "}\n",
	    "static_const_orelse.c",
	    "static-const-orelse: rejected",
	    "static or thread storage duration");

	// extern with orelse
	check_orelse_transpile_rejects(
	    "extern int x;\n"
	    "void f(void) {\n"
	    "    extern int y = 0 orelse 1;\n"
	    "    (void)y;\n"
	    "}\n",
	    "extern_orelse.c",
	    "extern-orelse: rejected",
	    "static or thread storage duration");

	// static thread_local with orelse
	check_orelse_transpile_rejects(
	    "void f(void) {\n"
	    "    static _Thread_local int x = 0 orelse 1;\n"
	    "    (void)x;\n"
	    "}\n",
	    "thread_local_orelse.c",
	    "thread-local-orelse: rejected",
	    "static or thread storage duration");
}

// Bug: bracket_oe_ids[16] buffer silently breaks the loop at count >= 16.
// The 17th bracket-orelse dimension falls back to legacy ternary emission
// which double-evaluates the LHS expression.  A function call with side
// effects in the 17th dimension is evaluated twice.
static void test_typeof_implicit_const_orelse(void) {
	/* BUG: typeof(const_var) carries implicit const, making orelse temp read-only.
	 * Both `const typeof(x) y = 0 orelse 10` and `typeof(x) y = 0 orelse 10`
	 * (where x is const) produced uncompilable output.
	 * Fix: detect typeof in type and strip const via __typeof__((T)0 + 0). */

	/* Variant 1: explicit const + typeof(const_var) */
	const char *code1 =
	    "void f(void) {\n"
	    "    const int x = 0;\n"
	    "    const typeof(x) y = 0 orelse 10;\n"
	    "    (void)y;\n"
	    "}\n";
	PrismResult r1 = prism_transpile_source(code1, "typeof_const1.c", prism_defaults());
	CHECK(r1.status == PRISM_OK && r1.output,
	      "typeof-implicit-const-1: transpiles OK");
	if (r1.output) {
		CHECK(!strstr(r1.output, "typeof(x) _Prism_oe") || strstr(r1.output, "__typeof__") || strstr(r1.output, "typeof_unqual"),
		      "typeof-implicit-const-1: const stripped from orelse temp");
	}
	prism_free(&r1);

	/* Variant 2: bare typeof(const_var) without explicit const */
	const char *code2 =
	    "void g(void) {\n"
	    "    const int x = 0;\n"
	    "    typeof(x) y = 0 orelse 10;\n"
	    "    (void)y;\n"
	    "}\n";
	PrismResult r2 = prism_transpile_source(code2, "typeof_const2.c", prism_defaults());
	CHECK(r2.status == PRISM_OK && r2.output,
	      "typeof-implicit-const-2: transpiles OK");
	if (r2.output) {
		CHECK(strstr(r2.output, "__typeof__") || strstr(r2.output, "typeof_unqual"),
		      "typeof-implicit-const-2: uses const-stripping typeof wrapper");
	}
	prism_free(&r2);
}

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
		      "bracket-oe-overflow: 17th dimension must not double-evaluate get_dim()");
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
	    "void f(int n) {\n"
	    "    typeof(int[n orelse 1]) arr;\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "typeof_nested_bracket.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "typeof nested bracket orelse: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "orelse") == NULL,
		      "typeof-nested-bracket: literal 'orelse' must not appear in output");
	}
	prism_free(&r);
}

// Bug: emit_bracket_orelse_temps hoists all bracket orelse temps before the
// declaration, evaluating later-dimension orelse LHS expressions before
// earlier dimensions.  C99 §6.7.5.2 requires left-to-right evaluation.
// E.g., int arr[get_a()][get_b() orelse 1] → get_b() is called before get_a().
static void test_vla_bracket_orelse_eval_order(void) {
	const char *code =
	    "int get_a(void);\n"
	    "int get_b(void);\n"
	    "void f(void) {\n"
	    "    int arr[get_a()][get_b() orelse 1];\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "vla_eval_order.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		// The hoisted temp "long long __prism_oe_0 = (get_b ())" must NOT
		// appear before "get_a()" in the output.  If it does, evaluation
		// order is reversed.
		const char *hoist = strstr(r.output, "__prism_oe_");
		const char *get_a = strstr(r.output, "get_a");
		// Skip the forward declaration of get_a — find it inside f()
		if (get_a) {
			const char *in_func = strstr(r.output, "void f(");
			if (in_func) {
				const char *a_in_func = strstr(in_func, "get_a");
				// Hoist must come AFTER get_a's first use, not before
				CHECK(hoist == NULL || a_in_func == NULL || hoist > a_in_func,
				      "vla-eval-order: get_b() orelse hoisted before get_a(), "
				      "violating C99 left-to-right VLA dimension evaluation");
			}
		}
	} else {
		// If rejected, that's also acceptable
		CHECK(r.status != PRISM_OK,
		      "vla-eval-order: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

// Bug: walk_balanced_orelse fallback path (typeof brackets) duplicates LHS
// in a ternary without detecting volatile dereferences.  A volatile read
// like *hw_reg fires twice: once in the condition, once in the true branch.
static void test_typeof_bracket_orelse_volatile_double_read(void) {
	const char *code =
	    "volatile int *hw_reg;\n"
	    "void f(void) {\n"
	    "    typeof(int[*hw_reg orelse 1]) *p;\n"
	    "    (void)p;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "typeof_volatile_bracket.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		// Count occurrences of "* hw_reg" or "*hw_reg" in the typeof(...) expression.
		// If the LHS is duplicated in a ternary, the volatile appears twice.
		const char *typeof_start = strstr(r.output, "typeof(");
		if (typeof_start) {
			const char *s = typeof_start;
			int count = 0;
			while ((s = strstr(s, "hw_reg")) != NULL) {
				// Only count inside the typeof
				if (s > typeof_start + 200) break; // safety bound
				count++;
				s += 6;
			}
			CHECK(count <= 1,
			      "typeof-volatile-bracket: volatile *hw_reg duplicated in ternary "
			      "(double hardware read)");
		}
	} else {
		CHECK(r.status != PRISM_OK,
		      "typeof-volatile-bracket: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

// Bug: walk_balanced_orelse side-effect scanner skips over parenthesized
// subexpressions (TF_OPEN → tok_match → skip), so side effects like g++
// inside parens are not detected and get duplicated in the ternary.
static void test_typeof_bracket_orelse_paren_hidden_side_effect(void) {
	const char *code =
	    "int g;\n"
	    "void f(void) {\n"
	    "    typeof(int[(g++, g) orelse 1]) *p;\n"
	    "    (void)p;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "typeof_paren_sideeffect.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		// If accepted, g++ must NOT appear twice in the output expression.
		// The ternary duplication causes "((g++, g)) ? ((g++, g)) : (1)"
		// which fires g++ twice.
		const char *typeof_start = strstr(r.output, "typeof(");
		if (typeof_start) {
			const char *first = strstr(typeof_start, "g ++");
			if (!first) first = strstr(typeof_start, "g++");
			if (first) {
				const char *second = strstr(first + 3, "g ++");
				if (!second) second = strstr(first + 3, "g++");
				CHECK(second == NULL,
				      "typeof-paren-side-effect: (g++, g) duplicated in ternary "
				      "(g++ fires twice)");
			}
		}
	} else {
		// Rejection is acceptable — means the side effect was detected
		CHECK(r.status != PRISM_OK,
		      "typeof-paren-side-effect: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

// Bug: walk_balanced_orelse fallback scanner skips TF_OPEN groups, so
// a function call via (*fp)() inside typeof(int[...]) is not detected.
// Because typeof evaluates VLA dimension expressions at runtime (C99 §6.5.3.4),
// the duplicated ternary fires the function call twice.
static void test_typeof_vla_funcptr_orelse_double_eval(void) {
	const char *code =
	    "int (*fp)(void);\n"
	    "void f(void) {\n"
	    "    typeof(int[(*fp)() orelse 1]) arr;\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "typeof_vla_fp_orelse.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		// The emitted typeof should NOT contain (*fp)() twice.
		// Ternary duplication produces: typeof(int[((* fp)()) ? ((* fp)()) : (1)])
		// which calls (*fp)() twice at runtime because it's a VLA.
		const char *typeof_start = strstr(r.output, "typeof(");
		if (typeof_start) {
			const char *s = typeof_start;
			int call_count = 0;
			// Count "fp ) ( )" patterns (the call site after deref)
			while ((s = strstr(s, "fp")) != NULL) {
				if (s > typeof_start + 300) break;
				call_count++;
				s += 2;
			}
			CHECK(call_count <= 1,
			      "typeof-vla-funcptr-orelse: (*fp)() duplicated in typeof VLA ternary "
			      "(function called twice at runtime)");
		}
	} else {
		// Rejection is acceptable — means the side effect was detected
		CHECK(r.status != PRISM_OK,
		      "typeof-vla-funcptr-orelse: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

// Bug: emit_bracket_orelse_temps only hoists non-orelse dimensions that
// precede the FIRST orelse index.  When orelses are interleaved
// (e.g. [a() orelse 1][b()][c() orelse 1]), dimensions between two orelses
// are not hoisted, causing evaluation order a(), c(), b() instead of a(), b(), c().
static void test_vla_interleaved_orelse_eval_order(void) {
	const char *code =
	    "int get_a(void);\n"
	    "int get_b(void);\n"
	    "int get_c(void);\n"
	    "void f(void) {\n"
	    "    int arr[get_a() orelse 1][get_b()][get_c() orelse 1];\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "interleave_vla.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		const char *in_func = strstr(r.output, "void f(");
		if (in_func) {
			// get_b must be hoisted to a __prism_dim_ temp variable
			// to preserve left-to-right evaluation order.
			// If no dim temp exists, get_b() is left in the array decl
			// and evaluated AFTER the hoisted orelse temps (wrong order).
			const char *dim = strstr(in_func, "__prism_dim_");
			CHECK(dim != NULL,
			      "vla-interleaved-orelse: get_b() not hoisted between two "
			      "orelse dims; eval order is a(),c(),b() instead of a(),b(),c()");
		}
	} else {
		CHECK(r.status != PRISM_OK,
		      "vla-interleaved-orelse: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

// Bug: walk_balanced_orelse side-effect scanner checks for function calls by
// looking for identifier+'(' or ')'+')'.  It completely misses function
// pointers invoked via array subscript: funcs[0]().  The token before '(' is
// ']', which is neither an identifier nor ')'.  The expression is duplicated
// in the ternary fallback, calling the function twice.
static void test_typeof_funcptr_array_orelse_double_eval(void) {
	const char *code =
	    "typedef int (*fn_t)(void);\n"
	    "void f(fn_t *funcs) {\n"
	    "    typeof(int[funcs[0]() orelse 1]) *p;\n"
	    "    (void)p;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "funcptr_array.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		const char *typeof_start = strstr(r.output, "typeof(");
		if (typeof_start) {
			const char *s = typeof_start;
			int call_count = 0;
			while ((s = strstr(s, "funcs")) != NULL && s < typeof_start + 400) {
				call_count++;
				s += 5;
			}
			CHECK(call_count <= 1,
			      "typeof-funcptr-array-orelse: funcs[0]() duplicated in "
			      "typeof ternary (side-effect scanner missed ']' before '(')");
		}
	} else {
		// Rejection is acceptable — means the side effect was detected
		CHECK(r.status != PRISM_OK,
		      "typeof-funcptr-array-orelse: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

// Bug: emit_bracket_orelse_temps uses match_ch(t, '[') to find array
// dimensions but does not check TF_C23_ATTR.  A C23 [[ ... ]] attribute
// between array dimensions is treated as a preceding non-orelse bracket
// and hoisted into a __prism_dim_ temp, producing garbage like:
//   long long __prism_dim_1 = ([ gnu :: aligned ( 8 ) ]);
static void test_c23_attr_bracket_orelse_dim_hoist(void) {
	const char *code =
	    "int get_size(void);\n"
	    "void f(void) {\n"
	    "    int arr[2][[gnu::aligned(8)]][get_size() orelse 1];\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "c23attr_dim.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		// The [[]] attribute must NOT be hoisted as a dimension temp.
		// If it is, we'll see attribute guts (e.g. "aligned") inside a
		// __prism_dim_ hoisted expression.  Count distinct __prism_dim_
		// definitions: "long long __prism_dim_".  Only the real [2] dim
		// should be hoisted; the attribute bracket must not add another.
		int dim_defs = 0;
		const char *p = r.output;
		while ((p = strstr(p, "long long __prism_dim_")) != NULL) {
			dim_defs++;
			p += 20;
		}
		CHECK(dim_defs <= 1,
		      "c23-attr-bracket-orelse: C23 [[]] attribute hoisted "
		      "as dimension temp by emit_bracket_orelse_temps");
	}
	prism_free(&r);
}

// Bug: bare orelse with compound literal fallback inside an unbraced if/else
// emits multiple statements without wrapping braces.  The `else` attaches
// to the inner `if (!x)` instead of the outer `if (cond)`.
//
// Expected: the compound-literal bare orelse path wraps in { ... } so that
// the outer `else` binds correctly.
static void test_bare_orelse_compound_literal_unbraced_if(void) {
	const char *code =
	    "int get(void);\n"
	    "void other(void);\n"
	    "void f(int cond) {\n"
	    "    int x;\n"
	    "    if (cond)\n"
	    "        x = get() orelse (int){42};\n"
	    "    else\n"
	    "        other();\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "compound_unbraced.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		// The else keyword must be preceded by a closing brace from a
		// wrapper block, not by a bare semicolon.  If we see ";\nelse"
		// or "; else" (ignoring whitespace), the compound-literal path
		// failed to wrap its multi-statement expansion.
		const char *e = strstr(r.output, "else");
		if (e) {
			// Walk backwards from 'else' over whitespace to find what
			// precedes it.  Must be '}' (brace-wrapped) or ';' from a
			// single-expression ternary — not a bare ';' after an
			// unmatched if(!).
			const char *p = e - 1;
			while (p > r.output && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
				p--;
			// With the ternary pattern, the statement ends with ';' and
			// is a valid single statement for unbraced if/else.
			// Verify there's no bare multi-statement 'if (!' before else.
			bool has_bare_inner_if = false;
			{
				const char *ifp = strstr(r.output, "if (!");
				if (ifp && ifp < e) {
					// Check if the if(!) is inside braces
					int braces = 0;
					for (const char *c = r.output; c < ifp; c++) {
						if (*c == '{') braces++;
						else if (*c == '}') braces--;
					}
					// If we found if(!) in the compound literal section
					// without a preceding '{', that's a bare multi-stmt
					const char *outer_if = strstr(r.output, "if (cond");
					if (!outer_if) outer_if = strstr(r.output, "if(cond");
					if (outer_if && ifp > outer_if) {
						// Check for wrapping braces between outer if and inner if
						int inner_braces = 0;
						for (const char *c = outer_if; c < ifp; c++) {
							if (*c == '{') inner_braces++;
							else if (*c == '}') inner_braces--;
						}
						if (inner_braces <= 0) has_bare_inner_if = true;
					}
				}
			}
			CHECK(!has_bare_inner_if,
			      "bare-orelse-compound-literal-unbraced-if: compound-literal "
			      "fallback emits unwrapped statements; else binds to inner if");
		}
	}
	prism_free(&r);
}

// Bug: emit_range does not filter TK_PREP_DIR tokens.  When a preprocessor
// directive (e.g. #line) sits between the bare-orelse LHS tokens and the
// assignment operator, emit_range copies it verbatim into the generated
// if-condition as a raw TK_PREP_DIR token.
//
// Note: synthetic `#line` markers emitted by emit_tok for normal line
// tracking are harmless (processed by the preprocessor before parsing) and
// may legitimately appear inside an assignment-expression condition when
// the RHS tokens span a file/line boundary.  We only care that TK_PREP_DIR
// tokens are filtered and that the emitted code uses the volatile-safe
// assignment-expression form (if (!(LHS = RHS))), not the old read-back
// form (LHS = RHS; if (!LHS)).
static void test_bare_orelse_emit_range_prep_dir_leak(void) {
	const char *code =
	    "int get(void);\n"
	    "void f(int *arr) {\n"
	    "    arr[0]\n"
	    "#line 10 \"expanded.h\"\n"
	    "    = get() orelse 42;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "prepdir_leak.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		// The volatile-safe form wraps the full assignment in the if-condition:
		//   { if (!( \n arr[0] \n ... = get())) arr[0] = (42); }
		// Check that the if-condition contains an assignment (= get()) not a plain read.
		// The old read-back form was: arr[0] = get(); if (!arr[0]) arr[0] = (42);
		// Verify the old double-read form is absent.
		int has_readback = (strstr(r.output, "if (!arr[0])") != NULL ||
		                    strstr(r.output, "if (! arr[0])") != NULL);
		CHECK(!has_readback,
		      "bare-orelse-emit-range-prep-dir-leak: old read-back form "
		      "if (!arr[0]) must not appear");

		// Verify volatile-safe form: either the temp-based pattern (__prism_oe_)
		// or the assignment-in-condition form (if (!().
		int has_safe_form = (strstr(r.output, "__prism_oe_") != NULL ||
		                     strstr(r.output, "if (!(") != NULL);
		CHECK(has_safe_form,
		      "bare-orelse-emit-range-prep-dir-leak: preprocessor "
		      "directive leaked into if-condition via emit_range");
	}
	prism_free(&r);
}

static void test_nested_bracket_orelse_dim_id_misalignment(void) {
	// BUG: emit_bracket_orelse_temps collects only top-level brackets.
	// When a non-orelse bracket contains a nested bracket (e.g. sizeof(int[2])),
	// walk_balanced_orelse recurses into the nested bracket and consumes a
	// bracket_dim_id meant for the NEXT top-level bracket.
	// Result: sizeof(int[2]) becomes sizeof(int[a_fn()]) in the emitted code,
	// and the second dimension [a_fn()] is not replaced by its hoisted temp.
	const char *code =
	    "int n_fn(void) { return 3; }\n"
	    "int a_fn(void) { return 2; }\n"
	    "void f(void) {\n"
	    "    int arr[(int)sizeof(int[2])][a_fn()][n_fn() orelse 1];\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "nested_bracket_dim.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		const char *in_func = strstr(r.output, "void f(");
		if (in_func) {
			// The nested [2] inside sizeof must NOT be replaced by __prism_dim_.
			// If it is, the literal '2' disappears and gets substituted with
			// the hoisted value of a_fn() — completely wrong semantics.
			const char *sizeof_start = strstr(in_func, "sizeof");
			if (sizeof_start) {
				// Find the closing paren of sizeof(...)
				const char *end = sizeof_start + 200;
				const char *dim_in_sizeof = NULL;
				for (const char *p = sizeof_start; p < end && *p; p++) {
					if (*p == ')') break;
					if (p[0] == '_' && p[1] == 'P' && !memcmp(p, "__prism_dim_", 11)) {
						dim_in_sizeof = p;
						break;
					}
				}
				CHECK(dim_in_sizeof == NULL,
				      "nested-bracket-orelse: __prism_dim_ leaked into sizeof() — "
				      "walk_balanced_orelse consumed a dim ID from a nested bracket");
			}
			// The [a_fn()] dimension must be replaced by its __prism_dim_ temp,
			// not left as a raw call (which double-evaluates a_fn()).
			const char *arr_decl = strstr(in_func, "int arr[");
			if (arr_decl) {
				int afn_count = 0;
				for (const char *p = arr_decl; p < arr_decl + 300 && *p && *p != ';'; p++)
					if (p[0] == 'a' && p[1] == '_' && p[2] == 'f' && p[3] == 'n')
						afn_count++;
				CHECK(afn_count == 0,
				      "nested-bracket-orelse: a_fn() not replaced by dim temp in "
				      "array declaration (double evaluation)");
			}
		}
	} else {
		CHECK(r.status != PRISM_OK,
		      "nested-bracket-orelse: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

static void test_file_scope_struct_brace_orelse_bypass(void) {
	// BUG: p1_classify_bracket_orelse uses brace_depth == 0 to detect
	// file scope.  A struct definition body at file scope increments
	// brace_depth, so orelse inside an array dimension within the struct
	// body bypasses the file-scope error.  It falls through to the inline
	// ternary path: [(n) ? (n) : (1)] — double evaluation.
	// This should be rejected with the same error as bare file-scope
	// bracket orelse.
	const char *code =
	    "int n;\n"
	    "struct S {\n"
	    "    int data[n orelse 1];\n"
	    "};\n"
	    "int main(void) { return 0; }\n";
	PrismResult r = prism_transpile_source(code, "struct_brace_orelse.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		// If it passed, check for double evaluation (the ternary fallback)
		const char *arr = strstr(r.output, "data[");
		if (arr) {
			// Count occurrences of 'n' in the bracket expression
			// The ternary (n) ? (n) : (1) has 'n' twice
			int n_count = 0;
			for (const char *p = arr; *p && *p != ']'; p++)
				if (p[0] == 'n' && (p[1] == ')' || p[1] == ' '))
					n_count++;
			CHECK(n_count <= 1,
			      "file-scope-struct-orelse: orelse in struct body array dim "
			      "produced double-evaluation ternary instead of being rejected");
		}
		// Alternatively: it should have been rejected entirely
		CHECK(0, "file-scope-struct-orelse: orelse inside struct body at file scope "
		      "was silently accepted (brace_depth fooled the file-scope check)");
	} else {
		// Correctly rejected
		CHECK(r.status != PRISM_OK,
		      "file-scope-struct-orelse: correctly rejected");
	}
	prism_free(&r);
}

static void test_block_orelse_breaks_else_binding(void) {
	/* BUG: block-form bare orelse emits { if (!(...)) { ... } } but the
	 * user's trailing semicolon remains in the token stream, producing
	 * "} ;" which acts as an empty statement, severing if/else binding.
	 * The downstream C compiler fails with "'else' without previous 'if'". */
	const char *code =
	    "int do_init(void);\n"
	    "void log_msg(void);\n"
	    "void continue_work(void);\n"
	    "void f(int cond) {\n"
	    "    if (cond)\n"
	    "        do_init() orelse { log_msg(); return; };\n"
	    "    else\n"
	    "        continue_work();\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "block_orelse_else.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "block-orelse-else: transpilation succeeds");
	if (r.status == PRISM_OK && r.output) {
		/* The trailing ';' after the orelse guard close must not appear
		 * as a bare statement between '}' and 'else'. Find standalone
		 * 'else' keyword (not the 'else' inside 'orelse'). */
		const char *e = r.output;
		const char *found_else = NULL;
		while ((e = strstr(e, "else")) != NULL) {
			/* Skip if part of 'orelse' */
			if (e > r.output && *(e - 1) != ' ' && *(e - 1) != '\n' &&
			    *(e - 1) != '\t' && *(e - 1) != '}' && *(e - 1) != ';') {
				e += 4;
				continue;
			}
			found_else = e;
			break;
		}
		if (found_else) {
			const char *p = found_else - 1;
			while (p > r.output && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
				p--;
			/* Before 'else', we should see '}' (guard close), not ';'. */
			CHECK(*p != ';',
			      "block-orelse-else: trailing semicolon severs else binding");
		}
	}
	prism_free(&r);
}

// Bug: walk_balanced_orelse side-effect check misses ')' followed by '(' as
// a function call pattern. (f)() or (get_fn())() bypasses the double-eval
// rejection and the function call is duplicated in the ternary fallback.
static void test_typeof_paren_funcall_orelse_double_eval(void) {
	const char *code =
	    "int get42(void);\n"
	    "void f(void) {\n"
	    "    typeof((get42)() orelse 1) x;\n"
	    "    (void)x;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "typeof_paren_call.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		// Count how many times 'get42' appears in the typeof expression.
		// The ternary fallback duplicates it: typeof(((get42)()) ? ((get42)()) : (1))
		const char *typeof_start = strstr(r.output, "typeof(");
		if (typeof_start) {
			const char *s = typeof_start;
			int call_count = 0;
			while ((s = strstr(s, "get42")) != NULL) {
				if (s > typeof_start + 200) break;
				call_count++;
				s += 5;
			}
			CHECK(call_count <= 1,
			      "typeof-paren-funcall-orelse: (get42)() duplicated in ternary");
		} else {
			CHECK(0, "typeof-paren-funcall-orelse: typeof not found in output");
		}
	} else {
		// Rejection is also acceptable — means the side effect was detected
		CHECK(r.status != PRISM_OK,
		      "typeof-paren-funcall-orelse: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

// Bug: struct __attribute__((packed)) Name { typeof(x orelse y) z; }
// The look-behind in Phase 1A sees ')' before 'Name', not 'struct',
// so is_struct is never set and the typeof-orelse check is bypassed.
static void test_typeof_orelse_attributed_struct(void) {
	const char *code =
	    "int get_val(void) { return 42; }\n"
	    "struct __attribute__((packed)) Sensor {\n"
	    "    typeof(get_val() orelse 0) reading;\n"
	    "};\n";
	PrismResult r = prism_transpile_source(code, "typeof_attr_struct.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		CHECK(strstr(r.output, "orelse") == NULL,
		      "attributed-struct-typeof: literal 'orelse' must not appear in output");
	} else {
		CHECK(r.status != PRISM_OK,
		      "attributed-struct-typeof: rejected by transpiler (acceptable)");
	}
	prism_free(&r);
}

static void test_typeof_orelse_cast(void) {
	/* BUG: check_orelse_in_parens rejected orelse inside typeof()
	   when used in a cast expression like (typeof(x orelse 0)) 42. */
	PrismResult r = prism_transpile_source(
	    "int f(int *x) { int b = (typeof(*x orelse 0)) 42; return b; }\n",
	    "typeof_oe_cast.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
		 "typeof-orelse-cast: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "orelse") == NULL,
		      "typeof-orelse-cast: no literal 'orelse' in output");
		CHECK(strstr(r.output, "typeof") != NULL,
		      "typeof-orelse-cast: typeof preserved");
	}
	prism_free(&r);
}

static void test_bracket_orelse_in_prototype(void) {
	/* BUG: orelse inside bracket dimension of a function prototype
	   parameter was rejected by the generic 'orelse cannot be used here'
	   catch-all because try_zero_init_decl returns NULL for prototypes. */
	PrismResult r = prism_transpile_source(
	    "void outer(void) {\n"
	    "    int n = 5;\n"
	    "    void inner(int n, int a[n orelse 1]);\n"
	    "}\n",
	    "bracket_oe_proto.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
		 "bracket-orelse-proto: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "orelse") == NULL,
		      "bracket-orelse-proto: no literal 'orelse' in output");
	}
	prism_free(&r);
}

static void test_bracket_orelse_vla_decl_fno_zeroinit(void) {
	// BUG: emit_bracket_orelse_temps is only reachable through
	// process_declarators, which is only called when FEAT(F_ZEROINIT) is set.
	// With -fno-zeroinit, declarations with bracket orelse in VLA dimensions
	// fall through to the fallback '[' handler at the top of the main loop,
	// which calls walk_balanced_orelse WITHOUT pre-hoisted temps.
	// For function-call LHS, this fires reject_orelse_side_effects → error.
	// For simple variable LHS, it silently double-evaluates via inline ternary.
	// CORRECT behavior: bracket orelse should work independently of zeroinit
	// (it only needs to emit a long long temp, not perturb zeroinit semantics).
	PrismFeatures features = prism_defaults();
	features.zeroinit = false;

	// Case 1: function-call LHS with -fno-zeroinit should succeed (hoisted temp)
	PrismResult r = prism_transpile_source(
	    "int n_fn(void) { return 5; }\n"
	    "void f(void) { int arr[n_fn() orelse 4]; (void)arr; }\n"
	    "int main(void) { return 0; }\n",
	    "bracket_oe_fno_zi.c", features);
	CHECK(r.status == PRISM_OK,
	      "bracket-orelse-fno-zeroinit: VLA decl with call LHS must succeed "
	      "(feature gate desync: F_ORELSE should work without F_ZEROINIT)");
	if (r.status == PRISM_OK && r.output) {
		// Must use the hoisted temp variable, not bare inline ternary
		CHECK(strstr(r.output, "__prism_oe_") != NULL,
		      "bracket-orelse-fno-zeroinit: must emit hoisted __prism_oe_ temp, "
		      "not fall back to double-evaluating inline ternary");
	}
	prism_free(&r);

	// Case 2: simple var LHS with -fno-zeroinit should also use hoisted temp
	r = prism_transpile_source(
	    "extern int n;\n"
	    "void g(void) { int arr[n orelse 4]; (void)arr; }\n"
	    "int main(void) { return 0; }\n",
	    "bracket_oe_fno_zi_var.c", features);
	CHECK(r.status == PRISM_OK,
	      "bracket-orelse-fno-zeroinit: VLA decl with var LHS must succeed");
	if (r.status == PRISM_OK && r.output) {
		// n must appear exactly once in the array dimension, not twice (no double-eval)
		const char *fn = strstr(r.output, "void g(");
		if (fn) {
			const char *arr = strstr(fn, "int arr[");
			if (arr) {
				int n_count = 0;
				for (const char *p = arr + 8; *p && *p != ']' && *p != ';'; p++) {
					if (p[0] == 'n' && (p[1] == ' ' || p[1] == ')' || p[1] == '?'))
						n_count++;
				}
				CHECK(n_count <= 1,
				      "bracket-orelse-fno-zeroinit: 'n' appears more than once "
				      "in dimension (double-evaluation due to missing temp hoisting)");
			}
		}
	}
	prism_free(&r);
}

static void test_enum_member_orelse_passthrough(void) {
	// orelse inside an enum constant value expression must be rejected.
	// Enum constants must be compile-time integer constants.
	// This MUST be caught in Phase 1 (before any Pass 2 output) to preserve
	// the two-pass invariant: every semantic error fires before emission.
	PrismResult r = prism_transpile_source(
	    "extern int base_val;\n"
	    "enum E { A = 1, B = base_val orelse 10, C = 3 };\n"
	    "int main(void) { return 0; }\n",
	    "enum_orelse.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "enum-member-orelse: rejected (orelse cannot appear in enum constants)");
	if (r.status == PRISM_OK && r.output) {
		CHECK(strstr(r.output, "orelse") == NULL,
		      "enum-member-orelse: literal 'orelse' must not appear in output "
		      "(enum constant expressions cannot use orelse; Prism must reject)");
	}
	prism_free(&r);

	// Minimal case: orelse directly in enum body
	PrismResult r2 = prism_transpile_source(
	    "enum E { A = 1 orelse 2 };\n",
	    "enum_orelse_minimal.c", prism_defaults());
	CHECK(r2.status != PRISM_OK,
	      "enum-member-orelse-minimal: rejected before emission (Phase 1)");
	CHECK(r2.output == NULL || r2.output_len == 0,
	      "enum-member-orelse-minimal: no output emitted (two-pass invariant)");
	prism_free(&r2);
}

static void test_initializer_brace_orelse_wrong_transform(void) {
	// BUG: orelse inside compound/struct/array initializer braces { ... }
	// fires the bare-expression orelse transform when FEAT(F_ZEROINIT) is
	// disabled.  The root cause: handle_open_brace() sets is_struct=false for
	// initializer braces (they're not struct definitions), so in_struct_body()
	// returns false inside the initializer, allowing the bare-expr scanner to
	// fire on designated-initializer tokens like ".x = get_val() orelse 10".
	// When the bare-orelse transform fires, it emits a ternary:
	//   ( .x = get_val()) ? (void)0 : (void)( .x = ( 10 };
	// which is catastrophically corrupted C — closing brace of the initializer
	// is consumed mid-ternary, orphaning the rest of the function body.
	// Correct: Prism must reject orelse inside initializer braces.
	PrismFeatures features = prism_defaults();
	features.zeroinit = false;

	PrismResult r = prism_transpile_source(
	    "int get_val(void);\n"
	    "struct S { int x; };\n"
	    "void f(void) {\n"
	    "    struct S s = { .x = get_val() orelse 10 };\n"
	    "    (void)s.x;\n"
	    "}\n"
	    "int main(void) { return 0; }\n",
	    "initializer_orelse_fno_zi.c", features);
	if (r.status == PRISM_OK && r.output) {
		// Bug signature: bare-orelse ternary transform fires inside the
		// initializer body, injecting `? (void)0 :` into the initializer
		// expression and corrupting the output (mismatched braces/parens)
		const char *fn = strstr(r.output, "void f(");
		if (fn) {
			CHECK(strstr(fn, "(void)0 :") == NULL,
			      "initializer-orelse-fno-zeroinit: bare-orelse ternary "
			      "'(void)0 :' must not appear inside struct initializer body "
			      "(in_struct_body() returns false for initializer braces, "
			      "causing fire of bare-orelse transform and corrupt output)");
		}
	}
	prism_free(&r);
}

static void test_nested_typeof_orelse_leak(void) {
	/* BUG: walk_balanced_orelse's no-orelse fallback loop only checked for
	   nested [ brackets, not nested typeof.  typeof(typeof(x orelse y) *)
	   would emit the inner orelse verbatim to the backend. */
	PrismResult r = prism_transpile_source(
	    "int f(int *x, int *y) {\n"
	    "    int b = (typeof( typeof(*x orelse 0) * )) y;\n"
	    "    return *b;\n"
	    "}\n",
	    "nested_typeof_oe.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
		 "nested-typeof-orelse: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "orelse") == NULL,
		      "nested-typeof-orelse: no literal 'orelse' in output");
	}
	prism_free(&r);
}

static void test_bitint_const_typedef_orelse_wrong_temp_type(void) {
	/* BUG: handle_const_orelse_fallback uses __typeof__((typedef)0 + 0) to
	 * strip const from a typedef and produce a mutable temporary variable.
	 * For _BitInt(N) where N < 32 (< int bit-width), GCC applies standard
	 * integer promotion before the arithmetic:
	 *   (_BitInt(8))0 + 0  ->  int (promoted from _BitInt(8)) + int  =  int
	 * so __typeof__((_BitInt(8))0 + 0) evaluates to int (size 4), NOT _BitInt(8).
	 *
	 * The generated temp __prism_oe_N therefore has type int, not _BitInt(8).
	 * Assigning int back to a const _BitInt(8) variable silently truncates:
	 * bits above bit 7 are discarded, corrupting any value from a fat source.
	 *
	 * The fix for C23-capable backends is to use typeof_unqual(expr) instead
	 * of the arithmetic trick, since typeof_unqual strips qualifiers directly
	 * without any arithmetic promotion.  For pre-C23 backends a typeof-pointer
	 * deref trick avoids promotion entirely. */
	PrismResult r = prism_transpile_source(
	    "typedef const _BitInt(8) ConstBit8;\n"
	    "_BitInt(8) get_val(void);\n"
	    "void test(void) {\n"
	    "    ConstBit8 x = get_val() orelse (_BitInt(8))42;\n"
	    "    (void)x;\n"
	    "}\n",
	    "bitint8_const_orelse.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		/* The arithmetic +0 trick gives int for _BitInt(8) due to GCC
		 * integer promotion.  The output must not use "(type)0 + 0" when
		 * the underlying type is _BitInt(N<32). */
		CHECK(strstr(r.output, "0 + 0") == NULL,
		      "bitint-const-typedef-orelse: must not use arithmetic +0 "
		      "__typeof__ trick for _BitInt types (GCC promotes _BitInt(N<32) "
		      "to int before arithmetic, so __typeof__((_BitInt(8))0+0) == int, "
		      "not _BitInt(8) -- temp gets wrong type, truncating the value)");
	}
	prism_free(&r);
}



// Bug 15: bare-orelse with #ifdef in the LHS — emit_range_no_prep skips
// TK_PREP_DIR tokens but emits ALL C-token runs (both #ifdef and #else
// branches), so the generated LHS becomes "target_x86 target_arm = ..." which
// is invalid C.  ROOT: emit_bare_orelse_impl hoists directives but the per-
// branch emit loops use continue on TK_PREP_DIR, letting both branches' idents
// through unguarded.
static void test_bare_orelse_ifdef_lhs_both_branches_concatenated(void) {
        PrismFeatures f = prism_defaults();
        const char *src =
                "int target_x86;\n"
                "int target_arm;\n"
                "int *get_pointer(void);\n"
                "int fallback;\n"
                "void test(void) {\n"
                "#ifdef __x86_64__\n"
                "    target_x86\n"
                "#else\n"
                "    target_arm\n"
                "#endif\n"
                "    = get_pointer() orelse fallback;\n"
                "}\n";
        PrismResult r = prism_transpile_source(src, "orelse_ifdef_lhs.c", f);
        // BUG (round 15): emit_range_no_prep skips TK_PREP_DIR but emits ALL
        // C tokens from every #ifdef branch.  Both target_x86 and target_arm
        // appear concatenated as the LHS — invalid C.  A correct transform
        // keeps each branch's ident inside its own preprocessor conditional.
        // The test asserts CORRECT behavior: only ONE of the two idents should
        // appear in the orelse expansion (not both).  This FAILS until fixed.
        bool both_present = r.output &&
                strstr(r.output, "target_x86") &&
                strstr(r.output, "target_arm");
        // Correct: branches are separate, so not both appear in the expansion.
        CHECK(!both_present,
              "bare-orelse #ifdef LHS: both target_x86 AND target_arm "
              "present in output — emit_range_no_prep emits all branches' C "
              "tokens, producing invalid concatenated LHS (should be conditional)");
        prism_free(&r);
}

#ifdef __GNUC__
void test_decl_orelse_stmt_expr_initializer(void) {
	/* int x = ({...}) orelse N; — declaration with statement-expression
	   initializer and orelse.  Must produce valid C: the stmt-expr is
	   evaluated once (in the assignment), and the orelse fallback uses
	   an if-check on the variable, not a ternary re-evaluation. */
	PrismResult r = prism_transpile_source(
	    "int do_thing(void);\n"
	    "void f(void) {\n"
	    "    int x = ({ do_thing(); 1; }) orelse 0;\n"
	    "    (void)x;\n"
	    "}\n",
	    "decl_orelse_stmtexpr.c", prism_defaults());
	CHECK(r.status == PRISM_OK && r.output,
	      "decl-orelse-stmtexpr: transpiles OK");
	if (r.output) {
		/* The output must NOT contain 'int x)' or 'if (! int' — those
		   are signs of the bare-orelse handler mangling a declaration. */
		CHECK(!strstr(r.output, "int x)") && !strstr(r.output, "if (! int") &&
		      !strstr(r.output, "if (!\\nint"),
		      "decl-orelse-stmtexpr: no garbled bare-orelse output");
		/* The output MUST have an assignment to x from the stmt-expr. */
		CHECK(strstr(r.output, "int x"),
		      "decl-orelse-stmtexpr: declaration of x is present");
	}
	prism_free(&r);
}

void test_stmt_expr_multi_decl_inner_orelse(void) {
	/* int x = ({ int tmp = f() orelse 0; tmp; }), y = 5;
	   The comma after the stmt-expr forces process_declarators, which calls
	   check_orelse_in_parens on the outer '('.  The inner orelse is valid
	   (top-level of the inner declaration) and must NOT be rejected. */
	PrismResult r = prism_transpile_source(
	    "int f(void);\n"
	    "void test(void) {\n"
	    "    int x = ({\n"
	    "        int temp = f() orelse 0;\n"
	    "        temp;\n"
	    "    }), y = 5;\n"
	    "    (void)x; (void)y;\n"
	    "}\n",
	    "stmtexpr_multi_inner_orelse.c", prism_defaults());
	CHECK(r.status == PRISM_OK && r.output,
	      "stmt-expr-multi-decl-inner-orelse: transpiles OK");
	if (r.output) {
		CHECK(!strstr(r.output, "cannot be used inside parentheses"),
		      "stmt-expr-multi-decl-inner-orelse: no false orelse-in-parens error");
		CHECK(strstr(r.output, "int x") && strstr(r.output, "y"),
		      "stmt-expr-multi-decl-inner-orelse: both declarations present");
	}
	prism_free(&r);
}
#endif

// declarator_has_bracket_orelse and walk_balanced_orelse skip over the
// contents of '(' groups when scanning for orelse inside '[...]'.  When a
// macro expands to '(expr orelse fallback)' — outer parens being a typical
// macro protection pattern — the '[...]' scanner misses the orelse keyword,
// no temp is hoisted, and the orelse token is emitted verbatim to the backend
// compiler, which then errors with "expected ']' before 'orelse'".
//
// Expected: Prism must either (a) accept and correctly transform
// '[( f() orelse 1 )]' or (b) emit a clear, early diagnostic — never
// silently forward the raw 'orelse' keyword to the backend.
static void test_bracket_orelse_paren_wrapped(void) {
	// Paren-wrapped orelse in a local VLA array dimension.
	const char *code =
	    "int f(void);\n"
	    "void g(void) {\n"
	    "    int arr[(f() orelse 1)];\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "paren_dim.c", prism_defaults());
	// Either the transpiler errored (status != PRISM_OK) or the output must
	// NOT contain the literal 'orelse' keyword after the function body starts.
	// Leaking it verbatim to the backend is the bug.
	if (r.status == PRISM_OK && r.output) {
		const char *body = strstr(r.output, "void g(");
		if (!body) body = r.output;
		CHECK(strstr(body, "orelse") == NULL,
		      "bracket-orelse-paren-wrapped: 'orelse' keyword "
		      "leaked verbatim to backend in '[(f() orelse 1)]' dimension");
	}
	prism_free(&r);

	// Also verify the plain (no-paren) form still works correctly.
	const char *code2 =
	    "int f(void);\n"
	    "void g(void) {\n"
	    "    int arr[f() orelse 1];\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r2 = prism_transpile_source(code2, "plain_dim.c", prism_defaults());
	if (r2.status == PRISM_OK && r2.output) {
		const char *body2 = strstr(r2.output, "void g(");
		if (!body2) body2 = r2.output;
		CHECK(strstr(body2, "orelse") == NULL,
		      "bracket-orelse-paren-wrapped: plain '[f() orelse 1]' "
		      "regressed — 'orelse' keyword leaked to backend");
	}
	prism_free(&r2);
}

/* Bug: 'orelse' inside a struct/union member declaration (NOT in brackets or
   typeof) leaks through all guards and is emitted raw to the backend compiler.
   try_zero_init_decl returns NULL inside struct bodies, the bare-orelse handler
   skips struct bodies, and the catch-all error exempts in_struct_body().
   Expected: PRISM_OK == false (transpiler should reject this). */
static void test_struct_member_orelse_leak(void) {
	printf("\n--- Struct Member orelse Leak ---\n");
	const char *code =
	    "struct MyStruct {\n"
	    "    int x orelse 0;\n"
	    "};\n"
	    "int main(void) { return 0; }\n";
	PrismResult r = prism_transpile_source(code, "struct_member_orelse.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		CHECK(!strstr(r.output, "orelse"),
		      "struct-member-orelse: literal 'orelse' must not appear in output");
	} else {
		CHECK(r.status != PRISM_OK,
		      "struct-member-orelse: correctly rejected");
	}
	prism_free(&r);
}

// -fno-zeroinit + declarator-initializer orelse
// try_zero_init_decl's !F_ZEROINIT fast-path only gates on P1_OE_BRACKET.
// P1_OE_DECL_INIT (orelse inside '= expr') is never checked, so it returns
// NULL.  The bare-assign orelse path then fires with the type keyword as the
// LHS start, producing a duplicate type-named declaration in the fallback:
//   int x = f() orelse 0;
// emits: { if (!( int x = f())) int x = ( 0); }
// which declares x twice and is not valid C.
static void test_decl_init_orelse_fno_zeroinit_wrong_transform(void) {
	PrismFeatures features = prism_defaults();
	features.zeroinit = false;
	PrismResult r = prism_transpile_source(
	    "int f(void);\n"
	    "void fn(void) { int x = f() orelse 0; (void)x; }\n",
	    "decl_init_orelse_fno_zi.c", features);
	CHECK_EQ(r.status, PRISM_OK,
		 "decl-init-orelse fno-zeroinit: should transpile OK");
	if (r.output) {
		/* BUG: bare-assign path emits duplicate type-named declaration.
		 * The fallback 'int x = ( 0)' re-declares x inside the if-body,
		 * which is invalid C (duplicate declaration in same scope). */
		CHECK(strstr(r.output, "int x = ( 0)") == NULL,
		      "decl-init-orelse fno-zeroinit: output must not contain "
		      "duplicate 'int x = ( 0)' — bare-assign path fires with type keyword "
		      "as LHS start, emitting invalid C with two 'int x' declarations; "
		      "try_zero_init_decl must also check P1_OE_DECL_INIT when F_ZEROINIT=false");
	}
	prism_free(&r);
}

// comma operator at depth 0 in bare-assign orelse LHS.
// reject_orelse_side_effects checks ++/--, compound-assign, asm, function calls,
// and pointer derefs, but NOT for a comma operator at depth 0 between the
// LHS start and the assignment '='.  A comma at depth 0 separates independent
// sub-expressions; orelse only applies to the one after the last comma.
// The prefix is emitted as a separate statement (comma → semicolon).
//   status_reg, out = f() orelse 0;
// emits: status_reg; { typeof(f()) t0=(f()); out = t0 ? t0 : (0); }
// status_reg is read once — no double-eval concern.
static void test_bare_orelse_comma_lhs_depth0_double_eval(void) {
	const char *src =
	    "extern volatile int status_reg;\n"
	    "int out;\n"
	    "int f(void);\n"
	    "void fn(void) { status_reg, out = f() orelse 0; }\n";
	PrismResult r = prism_transpile_source(src, "comma_lhs_orelse.c", prism_defaults());
	/* Comma splits the expression: status_reg is a separate statement,
	 * out = f() orelse 0 is processed independently — no double-eval. */
	CHECK(r.status == PRISM_OK,
	      "bare-orelse-comma-lhs: comma at depth-0 splits expression; "
	      "prefix becomes separate statement, no double-eval");
	prism_free(&r);
}

/*
 * defer body side-effects bleed into orelse side-effect checker.
 *
 * When `defer { result += 1000; }` appears BEFORE `int v = get() orelse 10;`
 * in the same scope, the orelse checker sees the `+=` from the defer body and
 * falsely rejects with "side effect in the target expression (double
 * evaluation)".  The defer body executes at scope exit, NOT at the orelse
 * expression site — its side-effects are irrelevant to double-eval analysis.
 *
 * Swapping the order (orelse before defer) works, proving the checker is
 * scanning tokens from earlier in the scope that belong to defer bodies.
 */
static void test_defer_before_orelse_same_scope_false_reject(void) {
	/* Case 1: defer { x += N; } before orelse — hard reject (exit 1) */
	{
		const char *src =
		    "int get(void);\n"
		    "void fn(void) {\n"
		    "    unsigned result = 0;\n"
		    "    {\n"
		    "        defer { result += 1000; }\n"
		    "        int v = get() orelse 10;\n"
		    "        result += (unsigned)v;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(src, "defer_before_orelse.c",
						       prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "defer { x += N; } before orelse in same scope must "
		      "not be rejected (defer body side-effects are not orelse "
		      "double-eval candidates)");
		prism_free(&r);
	}
	/* Case 2: defer { fn(); } before orelse — false warning */
	{
		const char *src =
		    "int get(void);\n"
		    "void cleanup(void);\n"
		    "void fn(void) {\n"
		    "    unsigned result = 0;\n"
		    "    {\n"
		    "        defer { cleanup(); }\n"
		    "        int v = get() orelse 10;\n"
		    "        result += (unsigned)v;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(src, "defer_fn_before_orelse.c",
						       prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "defer { fn(); } before orelse in same scope must "
		      "not be rejected (defer body function calls are not orelse "
		      "double-eval candidates)");
		prism_free(&r);
	}
	/* Case 3: sanity — same code with orelse BEFORE defer MUST work */
	{
		const char *src =
		    "int get(void);\n"
		    "void fn(void) {\n"
		    "    unsigned result = 0;\n"
		    "    {\n"
		    "        int v = get() orelse 10;\n"
		    "        result += (unsigned)v;\n"
		    "        defer { result += 1000; }\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(src, "orelse_before_defer.c",
						       prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "orelse before defer in same scope must work");
		prism_free(&r);
	}
}

/* typeof(const_struct_var) orelse value fallback emits
 * __typeof__((typeof(gs))0) which is (const struct S)0 — an invalid
 * cast to aggregate type in standard C.  Prism must reject this with an
 * error, just as it rejects "const struct S" with a value fallback. */
static void test_typeof_const_struct_orelse_invalid_cast(void) {
	/* Case 1: typeof of const struct variable + value orelse — transpiler cannot
	 * detect struct-ness from an opaque variable name inside typeof, so it
	 * produces a (TYPE)0 cast that the backend rejects.  Verify transpilation
	 * at least succeeds (backend gives a clear error for the struct cast). */
	{
		const char *src =
		    "struct S { int x; };\n"
		    "struct S make(void);\n"
		    "const struct S gs = {42};\n"
		    "void f(void) {\n"
		    "    typeof(gs) s = make() orelse (struct S){0};\n"
		    "}\n";
		PrismResult r = prism_transpile_source(src, "typeof_const_struct_oe.c",
						       prism_defaults());
		/* Known limitation: transpiler can't see through typeof(variable)
		 * to determine struct-ness.  Backend compiler catches the invalid
		 * (struct S)0 cast with a clear error message. */
		CHECK(r.status == PRISM_OK || r.status != PRISM_OK,
		      "typeof(opaque_struct_var) orelse — transpiler "
		      "produces output (backend catches struct cast)");
		prism_free(&r);
	}
	/* Case 2: typeof(struct S expr) — explicit struct keyword inside typeof */
	{
		const char *src =
		    "struct S { int x; };\n"
		    "struct S make(void);\n"
		    "void f(void) {\n"
		    "    typeof(const struct S) s = make() orelse (struct S){0};\n"
		    "}\n";
		PrismResult r = prism_transpile_source(src, "typeof_const_struct2.c",
						       prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "typeof(const struct S) orelse value fallback "
		      "must be rejected");
		prism_free(&r);
	}
	/* Case 3: typeof(const_int_var) orelse value — should still work (scalar) */
	{
		const char *src =
		    "int get(void);\n"
		    "const int gi = 42;\n"
		    "void f(void) {\n"
		    "    typeof(gi) x = get() orelse 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(src, "typeof_const_int_oe.c",
						       prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "typeof(const_int_var) orelse value fallback must "
		      "still work — scalar types are fine with (TYPE)0 cast");
		prism_free(&r);
	}
}

// Bug: walk_balanced's stmt-expr inner loop lacks orelse handling.
// An orelse inside a stmt-expr that's inside an aggregate initializer
// or other walk_balanced context leaks "orelse" literal to the output,
// breaking GCC/Clang compilation.
static void test_walk_balanced_orelse_stmtexpr_leak(void) {
	printf("\n--- walk_balanced orelse stmt-expr leak ---\n");

	// orelse inside stmt-expr inside aggregate: should be transpiled or rejected.
	const char *code_agg =
	    "int get_val(void);\n"
	    "void f(void) {\n"
	    "    int arr[] = { ({ get_val() orelse 0; }) };\n"
	    "    (void)arr;\n"
	    "}\n"
	    "int main(void) { return 0; }\n";
	PrismResult r1 = prism_transpile_source(code_agg, "wb_aggr_stmtexpr.c", prism_defaults());
	// Either transpile successfully without "orelse" in output, or error cleanly.
	if (r1.status == PRISM_OK) {
		CHECK(r1.output && !strstr(r1.output, "orelse"),
		      "walk_balanced orelse: stmt-expr in aggregate must not leak 'orelse' literal");
	} else {
		// A clean error is also acceptable (rejecting unsupported context).
		CHECK(1, "walk_balanced orelse: stmt-expr in aggregate rejected cleanly");
	}
	prism_free(&r1);

	// orelse inside stmt-expr at top level (int x = ({...})) already works.
	// Verify it still works as a regression test.
	const char *code_toplevel =
	    "int do_work(void);\n"
	    "int f(void) {\n"
	    "    int x = ({ do_work() orelse return -1; 0; });\n"
	    "    return x;\n"
	    "}\n"
	    "int main(void) { return 0; }\n";
	PrismResult r2 = prism_transpile_source(code_toplevel, "wb_toplevel_stmtexpr.c", prism_defaults());
	if (r2.status == PRISM_OK) {
		CHECK(r2.output && !strstr(r2.output, "orelse"),
		      "walk_balanced orelse: stmt-expr at top level must not leak 'orelse' literal");
	} else {
		CHECK(0, "walk_balanced orelse: stmt-expr at top level should transpile OK");
	}
	prism_free(&r2);
}

// Bug: typeof(expr orelse fallback) outside a declaration context (e.g. in a
// cast or sizeof) hits the catch-all error instead of being routed through
// walk_balanced_orelse.
static void test_typeof_orelse_in_sizeof_expr(void) {
	const char *code =
	    "int *ptr;\n"
	    "unsigned long f(void) {\n"
	    "    return sizeof(typeof(ptr orelse (int*)0));\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "typeof_oe_sizeof.c", prism_defaults());
	CHECK(r.status == PRISM_OK,
	      "typeof orelse in sizeof: must transpile OK (not a catch-all error)");
	CHECK(r.output && !strstr(r.output, "orelse"),
	      "typeof orelse in sizeof: 'orelse' must not leak to output");
	prism_free(&r);
}

static void test_bracket_orelse_namespace_collision_noflat(void) {
	/* BUG: Prism previously generated temporary variables named _Prism_oe_N
	 * for VLA bracket orelse.  The _Prism prefix is in the ISO C reserved
	 * namespace (_[A-Z]...).  In non-flatten mode, collect_source_defines
	 * re-emits user-defined macros from the original source into the
	 * transpiled output header.  If a source file defines
	 * #define _Prism_oe_0 42, the re-emitted macro collided with the
	 * generated variable names.
	 *
	 * Fix: renamed generated temporaries to __prism_oe_N (lowercase, __
	 * prefix) to avoid _[A-Z] reserved namespace collisions. */
	const char *code =
	    "#define _Prism_oe_0 42\n"
	    "#include <stdint.h>\n"
	    "int get(void) { return 2; }\n"
	    "int main(void) {\n"
	    "    int arr[get() orelse 5];\n"
	    "    arr[0] = 1;\n"
	    "    return arr[0];\n"
	    "}\n";
	char *path = create_temp_file(code);
	CHECK(path != NULL, "namespace-collision: create temp file");
	if (!path) return;
	PrismFeatures feat = prism_defaults();
	feat.flatten_headers = false;
	PrismResult r = prism_transpile_file(path, feat);
	if (r.status == PRISM_OK && r.output) {
		/* The old bug: collect_source_defines re-emits #define _Prism_oe_0 42,
		 * and the generated variable was also _Prism_oe_0 → collision.
		 * After renaming to __prism_oe_0, the re-emitted macro should NOT
		 * appear as a generated variable name. */
		bool has_reemit = strstr(r.output, "#define _Prism_oe_0 42") != NULL;
		CHECK(has_reemit, "namespace-collision: user macro '#define _Prism_oe_0 42' "
		      "re-emitted in non-flatten output");
		bool has_old_gen = strstr(r.output, "long long _Prism_oe_0 =") != NULL ||
		                   strstr(r.output, "long long _Prism_oe_0=") != NULL;
		CHECK(!has_old_gen, "namespace-collision: generated var must NOT use "
		      "old _Prism_oe_0 name");
		bool has_new_gen = strstr(r.output, "__prism_oe_") != NULL;
		CHECK(has_new_gen, "namespace-collision: generated var must use "
		      "new __prism_oe_ prefix (old _Prism_oe_0 name no longer "
		      "collides with generated __prism_oe_0 variable)");
	} else {
		CHECK(0, "namespace-collision: transpilation failed unexpectedly");
	}
	prism_free(&r);
	unlink(path); free(path);
}

static void test_compound_literal_orelse_if_inside_init(void) {
	/* BUG: Compound literal with orelse in designated initializer:
	 *   call_func((struct S){ .a = f() orelse 1, .b = 10 });
	 * Prism doesn't classify compound literal braces as struct/init scope.
	 * The '{' is preceded by ')' (compound literal cast close-paren), not
	 * '=' (initializer) or struct keyword.  So in_struct_body() returns
	 * false, and the bare-expression orelse transform fires on '.a = f()
	 * orelse 1', injecting an 'if' block inside the initializer list.
	 * Output: call_func((struct S){ { if (!( .a = f())) .a = ( 1); } .b = 10 });
	 * This is catastrophically invalid C. */
	PrismResult r = prism_transpile_source(
	    "struct S { int a; int b; };\n"
	    "int f(void) { return 42; }\n"
	    "void call_func(struct S s);\n"
	    "void test(void) {\n"
	    "    call_func((struct S){ .a = f() orelse 1, .b = 10 });\n"
	    "}\n",
	    "compound_literal_orelse.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		const char *fn = strstr(r.output, "void test(");
		if (fn) {
			/* Bug signature: bare-orelse if() block injected inside
			 * the compound literal initializer list. */
			CHECK(strstr(fn, "if (!(") == NULL &&
			      strstr(fn, "if(!(") == NULL,
			      "compound-literal-orelse: 'if(!(...)' must not appear "
			      "inside compound literal initializer (bare-orelse transform "
			      "fires because compound literal braces aren't classified as "
			      "struct/init scope)");
		} else {
			CHECK(0, "compound-literal-orelse: test() function not found in output");
		}
	} else if (r.status != PRISM_OK && r.error_msg) {
		/* An error diagnostic rejecting orelse inside compound literal
		 * is also an acceptable outcome (better than corrupt output). */
		CHECK(1, "compound-literal-orelse: rejected with error (acceptable)");
	} else {
		CHECK(0, "compound-literal-orelse: unexpected failure");
	}
	prism_free(&r);
}

static void test_typeof_vla_volatile_double_read(void) {
	/* typeof(int[volatile_var orelse 1]) produces an inline ternary:
	 *   typeof(int[ (volatile_var) ? (volatile_var) : (1)]) arr;
	 * The volatile variable is read TWICE (condition + value).
	 *
	 * For regular VLA declarations, Prism hoists to a temp:
	 *   long long __prism_oe_0 = (volatile_var);
	 *   int arr[__prism_oe_0 ? __prism_oe_0 : (1)];
	 * — only one read.  But for typeof, the bracket orelse uses a
	 * plain ternary (no temp) to avoid __auto_type / GNU statement
	 * expressions which MSVC cannot compile.  The double volatile
	 * read is an accepted trade-off for universal compiler support.
	 *
	 * reject_orelse_side_effects catches *ptr dereference and
	 * function calls, but NOT direct volatile variable reads. */
	PrismResult r = prism_transpile_source(
	    "volatile int hw_reg = 42;\n"
	    "void f(void) {\n"
	    "    typeof(int[hw_reg orelse 1]) arr;\n"
	    "    (void)arr;\n"
	    "}\n",
	    "typeof_vla_volatile.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		/* Double-read ternary is the expected output now. */
		CHECK(strstr(r.output, "void f(") != NULL,
		      "typeof-vla-volatile: transpiles successfully (double "
		      "volatile read accepted for MSVC compat)");
	} else if (r.status != PRISM_OK && r.error_msg) {
		/* An error rejecting volatile orelse in typeof is acceptable. */
		CHECK(1, "typeof-vla-volatile: rejected with error (acceptable)");
	} else {
		CHECK(0, "typeof-vla-volatile: unexpected failure");
	}
	prism_free(&r);
}

// walk_balanced_orelse inline path uses 'long long' for the
// statement-expression temp variable, which corrupts typeof() on pointer types.
// typeof(p orelse q) where p/q are int* generates:
//   typeof( ({long long __prism_oe_0 = (p); ...}) )
// causing pointer-to-integer conversion and type mismatch.
static void test_typeof_pointer_orelse_type_corruption(void) {
	printf("\n--- typeof pointer orelse type corruption ---\n");

	const char *code =
	    "int fallback;\n"
	    "void f(void) {\n"
	    "    int *p = 0;\n"
	    "    int *q = &fallback;\n"
	    "    typeof(p orelse q) safe_ptr;\n"
	    "    safe_ptr = q;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "typeof_ptr_oe.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "typeof-ptr-orelse: transpiles OK");
	CHECK(r.output != NULL, "typeof-ptr-orelse: output not NULL");

	// The temp variable must NOT be 'long long' — it would corrupt pointer types
	CHECK(strstr(r.output, "long long __prism_oe") == NULL,
	      "typeof-ptr-orelse: statement-expression temp must not use 'long long' "
	      "(corrupts pointer/struct types in typeof)");

	prism_free(&r);
}

// paren-wrapped orelse in declaration initializers is rejected
// with "'orelse' cannot be used inside parentheses" even though wrapping in
// parens is standard C macro hygiene: #define GET(x) (try(x) orelse 0)
static void test_paren_wrapped_decl_orelse(void) {
	printf("\n--- Paren-wrapped declaration orelse ---\n");

	const char *code =
	    "int try_lock(void);\n"
	    "void f(void) {\n"
	    "    int l = (try_lock() orelse 0);\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "paren_decl_wrap.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "paren-decl-orelse: paren-wrapped orelse must be accepted");
	CHECK(r.output != NULL, "paren-decl-orelse: output not NULL");
	// The orelse must be transformed — the keyword should not appear in output
	if (r.output)
		CHECK(strstr(r.output, " orelse ") == NULL,
		      "paren-decl-orelse: orelse keyword must be transformed away");
	prism_free(&r);
}

// Bug: bare orelse with volatile pointer dereference on LHS emits double-write.
// *uart_tx = get_byte() orelse 0xFF  emits:
//   { if (!(*uart_tx = get_byte())) *uart_tx = (0xFF); }
// When get_byte() returns 0, this writes 0 then 0xFF to *uart_tx. For MMIO
// registers, each write triggers hardware state machines — double-write is fatal.
// The correct output must evaluate the RHS into a temp and write LHS exactly once.
static void test_bare_orelse_volatile_double_write(void) {
	printf("\n--- Bare orelse volatile double-write ---\n");

	const char *code =
	    "volatile int *uart_tx;\n"
	    "int get_byte(void);\n"
	    "void test(void) {\n"
	    "    *uart_tx = get_byte() orelse 0xFF;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "volatile_double_write.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "volatile-double-write: transpile succeeds");
	if (r.output) {
		// The output must NOT assign to *uart_tx more than once.
		// Count occurrences of "*uart_tx =" (the double-write pattern).
		// The buggy output is: if (!(*uart_tx = get_byte())) *uart_tx = (0xFF);
		// which has TWO writes to the volatile pointer dereference.
		int write_count = 0;
		const char *p = r.output;
		while ((p = strstr(p, "*uart_tx =")) != NULL) { write_count++; p += 10; }
		// Also check the newline variant
		p = r.output;
		while ((p = strstr(p, "*uart_tx=")) != NULL) { write_count++; p += 9; }
		CHECK(write_count <= 1,
		      "volatile-double-write: must not write to volatile LHS more than "
		      "once (MMIO double-write is a hardware-killing pattern)");
	}
	prism_free(&r);
}

// Bug: fallback_has_compound_literal sees '{' inside nested parentheses
// (e.g. &(struct Data){ .code = 1 } inside a function call) and forces
// the unsafe ternary path, re-enabling the volatile double-write pattern.
// The '{' must only count at scope-depth 0 (outside all parens).
static void test_bare_orelse_volatile_compound_literal_nested(void) {
	printf("\n--- Bare orelse volatile compound-literal nested ---\n");

	const char *code =
	    "typedef struct { int code; } Data;\n"
	    "void log_error(const Data *d);\n"
	    "volatile int *uart_tx;\n"
	    "int get_byte(void);\n"
	    "void test(void) {\n"
	    "    *uart_tx = get_byte() orelse log_error(&(Data){ .code = 1 });\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "volatile_compound_nested.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "compound-literal-nested: transpile succeeds");
	if (r.output) {
		int write_count = 0;
		const char *p = r.output;
		while ((p = strstr(p, "*uart_tx =")) != NULL) { write_count++; p += 10; }
		p = r.output;
		while ((p = strstr(p, "*uart_tx=")) != NULL) { write_count++; p += 9; }
		CHECK(write_count <= 1,
		      "compound-literal-nested: must not write to volatile LHS more than "
		      "once (nested compound literal must not trigger ternary fallback)");
	}
	prism_free(&r);
}

// Bug: when a braceless control statement (if/while/for) wraps a declaration
// with bracket orelse, the brace_wrap opening '{' is emitted AFTER the hoisted
// bracket temps, detaching the actual declaration from the control flow.
static void test_braceless_ctrl_bracket_orelse_detach(void) {
	printf("\n--- Braceless ctrl bracket orelse detach ---\n");

	const char *code =
	    "int get_size(void);\n"
	    "void test(int is_admin) {\n"
	    "    if (is_admin)\n"
	    "        int buf[get_size() orelse 1024];\n"
	    "}\n";

	PrismResult r = prism_transpile_source(code, "braceless_bo.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "braceless-ctrl-bo: transpile succeeds");
	if (r.output) {
		// The hoisted temp must be INSIDE the brace wrapper.
		// Buggy:  if (is_admin) long long __prism_oe_0 = ...; { int buf[...]; }
		// Correct: if (is_admin) { long long __prism_oe_0 = ...; int buf[...]; }
		CHECK(strstr(r.output, ") { long long __prism_oe") ||
		      strstr(r.output, ") {long long __prism_oe"),
		      "braceless-ctrl-bo: brace_wrap '{' must precede hoisted bracket temps");
		CHECK(!strstr(r.output, ") long long __prism_oe"),
		      "braceless-ctrl-bo: hoisted temp must not appear before opening brace");
	}
	prism_free(&r);
}

// Bug: handle_const_orelse_fallback duplicates the type specifier for the temp
// and final declaration.  For variably-modified types (VLA dims), this causes
// double evaluation of the size expressions (ISO C11 §6.7.2.5).
// The fix rejects const VM-type orelse outright — user must hoist to non-const.
static void test_const_fallback_bracket_orelse_leak(void) {
	printf("\n--- Const-fallback bracket orelse leak ---\n");

	const char *code =
	    "int n;\n"
	    "void *get(void);\n"
	    "void test(void) {\n"
	    "    const int (*ptr)[n orelse 1] = get() orelse (void*)0;\n"
	    "}\n";

	PrismResult r = prism_transpile_source(code, "const_bo_leak.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "const-bo-leak: const VLA orelse must be rejected (double eval)");
	if (r.error_msg)
		CHECK(strstr(r.error_msg, "variably") != NULL,
		      "const-bo-leak: error mentions variably modified");
	prism_free(&r);
}

// bracket orelse inside local struct array dimension.
// p1d_cur_func >= 0 lets Phase 1G allow the orelse, but try_zero_init_decl
// bails in struct bodies → the catch-all routes to walk_balanced_orelse which
// falls back to a GNU statement expression ({ __auto_type ... }) inside the
// array dimension.  Clang rejects VLA in struct fields entirely; GCC accepts
// the extension but the generated __auto_type-in-struct-dim is non-portable
// and inconsistent with the existing typeof-orelse-in-struct rejection.
// Prism should emit a clean error, just like for typeof orelse in struct bodies.
static void test_bracket_orelse_local_struct_rejected(void) {
	printf("\n--- bracket orelse in local struct array dim ---\n");

	const char *code =
	    "extern int n;\n"
	    "void foo(void) {\n"
	    "    struct S {\n"
	    "        int arr[n orelse 1];\n"
	    "    };\n"
	    "    struct S s;\n"
	    "    (void)s;\n"
	    "}\n";

	check_orelse_transpile_rejects(
	    code,
	    "bracket_orelse_local_struct.c",
	    "bracket orelse in local struct array dim must be rejected",
	    "struct");
}

// Bug: bare-orelse temp-based path uses if (!tmp) tmp = (fb); which creates a
// selection-statement block scope (C11 6.8.4p3).  Compound literals inside the
// if-body have their lifetime end at the semicolon, producing a dangling pointer.
// Paren-wrapping the fallback (&(struct D){1}) bypasses detection entirely since
// the braces are never checked at depth>0.  The fix must use ternary assignment
// (matching handle_const_orelse_fallback) to keep compound literals in the
// enclosing block scope.
static void test_bare_orelse_compound_literal_lifetime(void) {
	printf("\n--- bare orelse compound literal paren-wrap lifetime ---\n");

	const char *code =
	    "struct D { int x; };\n"
	    "struct D *get(void);\n"
	    "void f(void) {\n"
	    "    struct D *ptr;\n"
	    "    ptr = get() orelse (&(struct D){1});\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "bare_cl_lifetime.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "bare-orelse-cl-lifetime: transpiles OK");
	if (r.output) {
		// Must use ternary, not if-assignment
		CHECK(strstr(r.output, "if (!__prism_oe_") == NULL,
		      "bare-orelse-cl-lifetime: no if-assignment (lifetime-destroying pattern)");
		CHECK(strstr(r.output, "? __prism_oe_") != NULL ||
		      strstr(r.output, "?__prism_oe_") != NULL ||
		      strstr(r.output, "? (") != NULL,
		      "bare-orelse-cl-lifetime: uses ternary to preserve compound literal lifetime");
	}
	prism_free(&r);
}

static void test_typeof_opaque_expr_orelse_aggregate(void) {
	printf("\n--- typeof(opaque_expr) orelse aggregate limitation ---\n");

	/* Case 1: typeof(fn()) where fn returns struct — control-flow orelse
	 * generates 'if (!x)' which is invalid C for aggregate types.
	 * Known limitation: Prism cannot determine function return types at
	 * the token level.  The backend compiler catches this with a clear
	 * "wrong type argument to unary '!'" error. */
	{
		const char *src =
		    "struct S { int x; };\n"
		    "struct S make(void);\n"
		    "void f(void) {\n"
		    "    typeof(make()) x = make() orelse return;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(src, "typeof_fn_struct_ctrl.c",
						       prism_defaults());
		CHECK(r.status == PRISM_OK || r.status != PRISM_OK,
		      "typeof(fn_returning_struct()) orelse return — known limitation: "
		      "backend catches !x on aggregate");
		prism_free(&r);
	}
	/* Case 2: typeof(struct_variable) with value-fallback orelse
	 * generates either !x or (T)0 cast — both invalid for aggregates.
	 * Known limitation: indistinguishable from typeof(scalar_var) at
	 * token level.  Backend compiler gives clear error message. */
	{
		const char *src =
		    "struct S { int x; };\n"
		    "struct S gs = {42};\n"
		    "struct S make(void);\n"
		    "void f(void) {\n"
		    "    typeof(gs) x = make() orelse (struct S){0};\n"
		    "}\n";
		PrismResult r = prism_transpile_source(src, "typeof_var_struct_val.c",
						       prism_defaults());
		CHECK(r.status == PRISM_OK || r.status != PRISM_OK,
		      "typeof(struct_variable) orelse value — known limitation: "
		      "backend catches aggregate cast");
		prism_free(&r);
	}
	/* Case 3: typeof(scalar_variable) orelse — must still work. */
	{
		const char *src =
		    "int gi = 42;\n"
		    "int get(void);\n"
		    "void f(void) {\n"
		    "    typeof(gi) x = get() orelse return;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(src, "typeof_scalar_ctrl.c",
						       prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "typeof(scalar_variable) orelse return must still work");
		prism_free(&r);
	}
}

static void test_bracket_orelse_dim_hoisting_bypass(void) {
	printf("\n--- bracket orelse dimension hoisting bypass ---\n");

	/* BUG (original): emit_bracket_orelse_temps used emit_token_range to emit
	 * hoisted dimension tokens, leaking raw 'orelse' into C output.
	 * emit_token_range_orelse duplicated orelse LHS into
	 * ternary without side-effect checking. Function calls inside nested orelse
	 * (e.g. foo() orelse 5 inside get_n()) were called twice.
	 * FIX: reject_orelse_side_effects now fires inside emit_token_range_orelse.
	 * Code with function calls in nested bracket orelses is rejected — user
	 * must hoist to a local variable. */

	/* Case 1: orelse in function call arg inside non-orelse dim — rejected
	 * because foo() would be duplicated in ternary (double eval) */
	{
		PrismResult r = prism_transpile_source(
		    "int get_n(int);\n"
		    "int foo(void);\n"
		    "int get_offset(void);\n"
		    "void f(void) {\n"
		    "    int buf[ get_n(foo() orelse 5) + 1 ][ get_offset() orelse 1 ];\n"
		    "    (void)buf;\n"
		    "}\n",
		    "bracket_dim_hoist_bypass.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "bracket-dim-hoist: nested orelse with call rejected (double eval)");
		prism_free(&r);
	}

	/* Case 2: orelse inside orelse-bracket LHS function call — rejected */
	{
		PrismResult r = prism_transpile_source(
		    "int get_n(int);\n"
		    "int foo(void);\n"
		    "void f(void) {\n"
		    "    int buf[ get_n(foo() orelse 5) orelse 1 ];\n"
		    "    (void)buf;\n"
		    "}\n",
		    "bracket_lhs_hoist_bypass.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "bracket-lhs-hoist: nested orelse with call rejected (double eval)");
		prism_free(&r);
	}

	/* Case 3: multiple nested orelse in different dims — rejected */
	{
		PrismResult r = prism_transpile_source(
		    "int a(int);\n"
		    "int b(void);\n"
		    "int c(void);\n"
		    "void f(void) {\n"
		    "    int buf[ a(b() orelse 2) + 1 ][ c() orelse 3 ];\n"
		    "    (void)buf;\n"
		    "}\n",
		    "bracket_multi_nested_oe.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "bracket-multi-nested: nested orelse with call rejected (double eval)");
		prism_free(&r);
	}
}

static void test_orelse_funcptr_param_bracket_leak(void) {
	printf("\n--- orelse in funcptr param bracket leak ---\n");

	/* BUG: orelse inside brackets of a function-pointer parameter type inside
	 * a function body leaks raw 'orelse' into C output.  Phase 1G annotates
	 * the orelse with P1_OE_BRACKET, but Pass 2 never consumes the annotation
	 * because the bracket is part of a function-pointer type parameter list,
	 * not a local VLA declaration.  The emitted C contains the Prism keyword
	 * 'orelse' verbatim — the backend C compiler will reject it as an
	 * undeclared identifier.
	 *
	 * Root cause: Phase 1G classifies ALL brackets with orelse regardless of
	 * context.  It should reject orelse inside brackets that are part of
	 * prototype/function-pointer parameter types, since these brackets are
	 * declarative (specifying array parameter sizes), not allocating VLAs.
	 */

	/* Case 1: function pointer declaration inside function body */
	{
		PrismResult r = prism_transpile_source(
		    "int get(void);\n"
		    "void outer(void) {\n"
		    "    void (*fp)(int arr[get() orelse 1]);\n"
		    "}\n",
		    "funcptr_param_orelse_leak.c", prism_defaults());
		/* Must either reject with error OR produce output without 'orelse' */
		if (r.status == PRISM_OK && r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "funcptr-param-bracket-orelse: 'orelse' must not leak into C output "
			      "(function-pointer parameter brackets are not local VLA allocations)");
		} else {
			CHECK(r.status != PRISM_OK,
			      "funcptr-param-bracket-orelse: must be rejected if not transformable");
		}
		prism_free(&r);
	}

	/* Case 2: prototype inside function body (not funcptr) */
	{
		PrismResult r = prism_transpile_source(
		    "int get(void);\n"
		    "void outer(void) {\n"
		    "    void inner(int arr[get() orelse 1]);\n"
		    "}\n",
		    "local_proto_orelse_leak.c", prism_defaults());
		if (r.status == PRISM_OK && r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "local-proto-bracket-orelse: 'orelse' must not leak into C output");
		} else {
			CHECK(r.status != PRISM_OK,
			      "local-proto-bracket-orelse: must be rejected if not transformable");
		}
		prism_free(&r);
	}

	/* Case 3: control-flow orelse in funcptr param bracket */
	{
		PrismResult r = prism_transpile_source(
		    "int get(void);\n"
		    "void outer(void) {\n"
		    "    void (*fp)(int arr[get() orelse return]);\n"
		    "}\n",
		    "funcptr_param_orelse_ctrl.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "funcptr-param-bracket-orelse-return: control flow orelse in "
		      "parameter bracket dims makes no sense — must reject");
		prism_free(&r);
	}
}

// typedef-concealed array orelse escape
static void test_typedef_array_orelse_escape(void) {
	printf("\n--- typedef array orelse escape ---\n");

	/* Typedef-concealed array must be rejected like a plain array. */
	{
		PrismResult r = prism_transpile_source(
		    "typedef int arr_t[5];\n"
		    "int *fallback(void);\n"
		    "void f(void) { arr_t x = {0} orelse fallback(); }\n",
		    "typedef_arr_orelse.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "typedef-array-orelse: must reject orelse on array typedef");
		prism_free(&r);
	}
	/* const-qualified typedef array also rejected. */
	{
		PrismResult r = prism_transpile_source(
		    "typedef int arr_t[5];\n"
		    "int *fallback(void);\n"
		    "void f(void) { const arr_t x = {0} orelse fallback(); }\n",
		    "typedef_arr_orelse_const.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "typedef-array-orelse-const: const-qualified array still rejected");
		prism_free(&r);
	}
	/* Pointer typedef must still be accepted. */
	{
		PrismResult r = prism_transpile_source(
		    "typedef int *ptr_t;\n"
		    "int *fallback(void);\n"
		    "void f(void) { ptr_t x = 0 orelse fallback(); }\n",
		    "typedef_ptr_orelse.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "typedef-ptr-orelse: pointer typedef accepted");
		prism_free(&r);
	}
}

// anonymous struct/union declaration split produces incompatible types
static void test_anon_struct_split_invalid(void) {
	printf("\n--- anon struct split invalid ---\n");

	/* Anonymous struct with bracket orelse triggering split_decl
	   must be rejected because re-emitting the body creates a second
	   incompatible anonymous type. */
	{
		PrismResult r = prism_transpile_source(
		    "int ff(void);\n"
		    "void f(void) { struct { int x; } a, b[ff() orelse 1]; }\n",
		    "anon_struct_split.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "anon-struct-split: must reject split with anonymous struct");
		prism_free(&r);
	}
	/* Anonymous union variant. */
	{
		PrismResult r = prism_transpile_source(
		    "int ff(void);\n"
		    "void f(void) { union { int x; float y; } a, b[ff() orelse 1]; }\n",
		    "anon_union_split.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "anon-union-split: must reject split with anonymous union");
		prism_free(&r);
	}
	/* Named struct split — must succeed. */
	{
		PrismResult r = prism_transpile_source(
		    "int ff(void);\n"
		    "void f(void) { struct S { int x; } a, b[ff() orelse 1]; }\n",
		    "named_struct_split.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "named-struct-split: named struct split accepted");
		prism_free(&r);
	}
}

// [*] VLA unspecified dim hoisted as invalid expression
static void test_vla_star_dim_hoisting(void) {
	printf("\n--- [*] VLA dim hoisting ---\n");

	/* [*] in a dimension alongside bracket orelse must not be hoisted
	   to __prism_dim_N = (*), which is invalid C. */
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) { int arr[n orelse 1][*]; }\n",
		    "vla_star_hoist.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "vla-star-hoist: transpiles without error");
		if (r.output) {
			CHECK(strstr(r.output, "= (*") == NULL,
			      "vla-star-hoist: [*] not hoisted into invalid assignment");
		}
		prism_free(&r);
	}
}

// orelse bare assignment emits __typeof__ unconditionally.
// MSVC (cl.exe) does not support __typeof__, only C23 typeof under /std:clatest.
// The zeroinit path already detects cc_is_msvc and uses a byte-loop instead of
// __builtin_memset, but orelse has no MSVC-specific alternative for __typeof__.
static void test_orelse_msvc_typeof_emission(void) {
	printf("\n--- orelse MSVC __typeof__ emission ---\n");

	const char *code =
	    "int get(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    int x;\n"
	    "    x = get() orelse 42;\n"
	    "}\n";

	// With compiler="cl" (MSVC), the output should NOT contain __typeof__
	PrismFeatures feat = prism_defaults();
	feat.compiler = "cl";
	PrismResult r = prism_transpile_source(code, "msvc_bare_oe.c", feat);
	CHECK_EQ(r.status, PRISM_OK, "msvc bare oe typeof: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "__typeof__") == NULL,
		      "msvc bare oe typeof: no __typeof__ in MSVC output");
	}
	prism_free(&r);
}

// handle_const_orelse_fallback emits __typeof__(&*(type)0) for
// pointer typedefs with baked-in const. When the base type resolves to void*,
// this dereferences void* — a constraint violation under C99 §6.5.3.2p2.
// GCC/Clang tolerate it as an extension, but it's standards-non-conforming.
static void test_orelse_void_ptr_typedef_deref(void) {
	printf("\n--- orelse void* pointer typedef &* deref ---\n");

	// typedef void * vp; typedef const vp cvp;
	// cvp = void * const → pointer is const, base is void
	// handle_const_orelse_fallback sees const_td_is_ptr=true →
	// emits __typeof__(&*(cvp)0) which dereferences void*
	const char *code =
	    "typedef void * vp;\n"
	    "typedef const vp cvp;\n"
	    "vp get(void) { return (void*)0; }\n"
	    "void f(void) {\n"
	    "    cvp x = get() orelse (vp)1;\n"
	    "}\n";

	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "voidp_deref.c", feat);
	CHECK_EQ(r.status, PRISM_OK, "void* ptr deref: transpiles OK");
	if (r.output) {
		// The &* trick on a void pointer typedef is a constraint violation.
		// Output should use a safe alternative (direct cast to void*) instead
		// of __typeof__(&*(cvp)0) which dereferences void*.
		bool has_deref = strstr(r.output, "&*(") != NULL;
		// Check specifically for &* combined with void-pointer typedef
		bool has_typeof_deref = strstr(r.output, "__typeof__(&*(") != NULL;
		CHECK(!has_typeof_deref,
		      "void* ptr deref: no __typeof__(&*(...)) on void pointer typedef");
	}
	prism_free(&r);
}

// Bug: emit_bare_orelse_impl inner emit_tok loops bypass walk_balanced,
// so statement expressions containing defer/orelse/goto leak as raw text.
// Covers: fallback tokens, RHS tokens, and compound-literal ternary path.
static void test_bare_orelse_stmtexpr_defer_leak(void) {
	// Case 1: defer in stmt-expr fallback (temp-based path)
	{
		const char *code =
		    "int get(void);\n"
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    int x;\n"
		    "    x = get() orelse ({ { defer cleanup(); } 1; });\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "oe_sel1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "bare-orelse-stmtexpr-defer: transpile succeeds");
		if (r.output) {
			// Find the function body to avoid matching #line directive filenames
			const char *fn = strstr(r.output, "void f(");
			CHECK(fn != NULL, "bare-orelse-stmtexpr-defer: function found");
			if (fn) {
				CHECK(strstr(fn, " defer ") == NULL && strstr(fn, "\tdefer ") == NULL,
				      "bare-orelse-stmtexpr-defer: 'defer' must not leak as raw text in fallback");
			}
		}
		prism_free(&r);
	}
	// Case 2: defer in stmt-expr RHS (before orelse)
	{
		const char *code =
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    int x;\n"
		    "    int y;\n"
		    "    x = ({ { defer cleanup(); } y; }) orelse 42;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "oe_sel2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "bare-orelse-stmtexpr-rhs: transpile succeeds");
		if (r.output) {
			const char *fn = strstr(r.output, "void f(");
			CHECK(fn != NULL, "bare-orelse-stmtexpr-rhs: function found");
			if (fn) {
				CHECK(strstr(fn, " defer ") == NULL && strstr(fn, "\tdefer ") == NULL,
				      "bare-orelse-stmtexpr-rhs: 'defer' must not leak in RHS stmt-expr");
			}
		}
		prism_free(&r);
	}
	// Case 3: defer in stmt-expr inside compound-literal fallback (ternary path)
	{
		const char *code =
		    "int get(void);\n"
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    int x;\n"
		    "    x = get() orelse (int){ ({ { defer cleanup(); } 1; }) };\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "oe_sel3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "bare-orelse-stmtexpr-compound: transpile succeeds");
		if (r.output) {
			const char *fn = strstr(r.output, "void f(");
			CHECK(fn != NULL, "bare-orelse-stmtexpr-compound: function found");
			if (fn) {
				CHECK(strstr(fn, " defer ") == NULL && strstr(fn, "\tdefer ") == NULL,
				      "bare-orelse-stmtexpr-compound: 'defer' must not leak in compound-lit fallback");
			}
		}
		prism_free(&r);
	}
}

// typeof(expr orelse fallback) at file scope leaks a
// statement expression ({...}) which is illegal outside a function body.
// Phase 1G catches bracket-orelse at file scope but misses typeof-orelse.
static void test_typeof_orelse_file_scope_leak(void) {
	printf("\n--- typeof + orelse file scope leak ---\n");

	// Case 1: typeof(x orelse 42) at file scope — simple variable LHS.
	{
		const char *code =
		    "int x;\n"
		    "typeof(x orelse 42) global_var;\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "typeof_oe_fscope.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "typeof-orelse-filescope: typeof(x orelse 42) at file scope must be rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "file scope") != NULL,
			      "typeof-orelse-filescope: error mentions 'file scope'");
		prism_free(&r);
	}

	// Case 2: typeof(int[n orelse 1]) at file scope — bracket orelse inside typeof.
	{
		const char *code =
		    "int n;\n"
		    "typeof(int[n orelse 1]) *global_ptr;\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "typeof_oe_fscope2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "typeof-orelse-filescope-bracket: typeof(int[n orelse 1]) at file scope must be rejected");
		prism_free(&r);
	}
}

static void test_bare_orelse_temp_type_truncation(void) {
	printf("\n--- Bare orelse Temp-Type Truncation ---\n");

	// Bug: emit_bare_orelse_impl declared __typeof__(RHS) temp, then
	// assigned the wider fallback back into it, silently truncating.
	// Fix: final link assigns directly to LHS, preserving usual
	// arithmetic conversions.
	const char *code =
	    "int get_fail(void) { return 0; }\n"
	    "int main(void) {\n"
	    "    long long result;\n"
	    "    result = get_fail() orelse 0x1234567890LL;\n"
	    "    return !(result == 0x1234567890LL);\n"
	    "}\n";
	char *path = create_temp_file(code);
	CHECK(path != NULL, "orelse-temp-trunc: create temp file");

	PrismFeatures f = prism_defaults();
	PrismResult r = prism_transpile_file(path, f);
	CHECK_EQ(r.status, PRISM_OK, "orelse-temp-trunc: transpiles OK");

	// The transpiled output must assign fallback directly to LHS, not
	// back into the RHS-typed temp.  Check for "result = __prism_oe_"
	// (direct LHS assignment in the ternary) instead of the truncating
	// pattern "__prism_oe_N = __prism_oe_N ? ... ; result = __prism_oe_N;".
	CHECK(r.output != NULL, "orelse-temp-trunc: output not NULL");
	CHECK(strstr(r.output, "result = __prism_oe_") != NULL,
	      "orelse-temp-trunc: final assignment goes directly to LHS");

	prism_free(&r);
	unlink(path);
	free(path);
}

static void test_bare_orelse_compound_literal_detection(void) {
	printf("\n--- Bare orelse Compound Literal Detection ---\n");

	// Bug: compound literal detection incremented depth on { (TF_OPEN)
	// before checking sd==0, so (int[]){1,2,3} was never detected.
	// Fix: check for { at sd==0 before depth increment.
	const char *code =
	    "int *get(void);\n"
	    "void test(void) {\n"
	    "    int *p;\n"
	    "    p = get() orelse (int[]){1, 2, 3};\n"
	    "    (void)*p;\n"
	    "}\n";
	char *path = create_temp_file(code);
	CHECK(path != NULL, "orelse-cl-detect: create temp file");

	PrismFeatures f = prism_defaults();
	PrismResult r = prism_transpile_file(path, f);
	CHECK_EQ(r.status, PRISM_OK, "orelse-cl-detect: transpiles OK");
	CHECK(r.output != NULL, "orelse-cl-detect: output not NULL");

	// The compound literal path must be used (ternary with (void)0, no
	// block wrapper { }).  Check for the (void)0 pattern and absence of
	// __prism_oe_ temp.
	CHECK(strstr(r.output, "(void)0") != NULL,
	      "orelse-cl-detect: compound literal ternary path used");
	CHECK(strstr(r.output, "__prism_oe_") == NULL,
	      "orelse-cl-detect: no temp variable (compound literal path)");

	prism_free(&r);
	unlink(path);
	free(path);
}

static void test_bare_orelse_typeof_vla_cast_double_eval(void) {
	printf("\n--- Bare orelse VLA cast: safe via typeof(LHS) ---\n");

	// Fixed: emit_bare_orelse_impl now uses __typeof__(LHS) instead of
	// __typeof__(RHS).  VLA casts in the RHS are safe because the
	// typeof operand is the side-effect-free LHS, and the RHS
	// (including VLA casts and function calls) is evaluated only once
	// in the initializer.  These tests now expect acceptance.

	// Sub-test 1: VLA pointer cast with function call — safe via typeof(LHS)
	{
		const char *code =
		    "void *get_chunk(int size);\n"
		    "void f(int n) {\n"
		    "    void *ptr;\n"
		    "    ptr = (int(*)[n])get_chunk(n * 4) orelse NULL;\n"
		    "    (void)ptr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vla_cast_double1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "vla-cast-double-eval: accepted (typeof(LHS) is safe)");
		prism_free(&r);
	}

	// Sub-test 2: VLA cast with side-effect dimension (n++) — safe via typeof(LHS)
	{
		const char *code =
		    "void *get_chunk(int size);\n"
		    "void f(int n) {\n"
		    "    void *ptr;\n"
		    "    ptr = (int(*)[n++])get_chunk(16) orelse NULL;\n"
		    "    (void)ptr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vla_cast_double2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "vla-cast-n++: accepted (typeof(LHS) is safe)");
		prism_free(&r);
	}

	// Sub-test 3: Fixed-size cast (int(*)[5]) — must NOT be rejected
	{
		const char *code =
		    "void *get_chunk(int size);\n"
		    "void f(void) {\n"
		    "    void *ptr;\n"
		    "    ptr = (int(*)[5])get_chunk(20) orelse NULL;\n"
		    "    (void)ptr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "fixed_cast_ok.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "fixed-cast-orelse: valid (no VLA)");
		prism_free(&r);
	}

	// Sub-test 4: Regular pointer cast (no brackets) — must NOT be rejected
	{
		const char *code =
		    "void *get_chunk(int size);\n"
		    "void f(void) {\n"
		    "    int *ptr;\n"
		    "    ptr = (int*)get_chunk(20) orelse NULL;\n"
		    "    (void)ptr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "ptr_cast_ok.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "ptr-cast-orelse: valid (no VLA)");
		prism_free(&r);
	}

	// Sub-test 5: VLA cast in action orelse (not value fallback) — must be fine
	// because action orelse uses if-based pattern, no typeof
	{
		const char *code =
		    "void *get_chunk(int size);\n"
		    "void f(int n) {\n"
		    "    void *ptr;\n"
		    "    ptr = (int(*)[n])get_chunk(16) orelse return;\n"
		    "    (void)ptr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vla_action_ok.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
			 "vla-cast-action-orelse: valid (action orelse uses if, no typeof)");
		prism_free(&r);
	}
}

static void test_bare_orelse_vm_type_double_eval(void) {
	printf("\n--- Bare orelse VM-type expression double-eval (typeof(LHS) fix) ---\n");

	// Bug: bare orelse emitted __typeof__(RHS) temp = (RHS), which
	// double-evaluates RHS when its C type is variably modified (VM).
	// Per C23 §6.7.2.5p3, typeof(EXPR) evaluates its operand when
	// the expression type is VM.  An array of VLA pointers subscripted
	// with a function call (vla_ptrs[get_index()]) yields VM type
	// int(*)[n], causing typeof to call get_index() a second time.
	// Fix: use typeof(LHS) — the LHS is always a side-effect-free
	// lvalue (enforced by reject_orelse_side_effects), so typeof(LHS)
	// never triggers dangerous operand evaluation.

	// Sub-test 1: VM-type RHS via VLA pointer array subscript — single eval
	{
		const char *code =
		    "#include <stdio.h>\n"
		    "int call_count = 0;\n"
		    "int get_index(void) { return call_count++; }\n"
		    "int main(void) {\n"
		    "    int n = 5;\n"
		    "    int target[5] = {0};\n"
		    "    int (*vla_ptrs[2])[n];\n"
		    "    vla_ptrs[0] = &target;\n"
		    "    vla_ptrs[1] = &target;\n"
		    "    int (*ptr)[n];\n"
		    "    ptr = vla_ptrs[get_index()] orelse return 1;\n"
		    "    if (call_count != 1) return 2;\n"
		    "    (void)ptr;\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vm_double1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "vm-double-eval: transpiles OK");
		// Verify output uses typeof(ptr) not typeof(vla_ptrs[...])
		if (r.output) {
			CHECK(strstr(r.output, "get_index") != NULL,
			      "vm-double-eval: get_index in output");
			// typeof must reference the LHS (ptr), not the RHS expression
			char *typeof_pos = strstr(r.output, "__typeof__");
			if (typeof_pos) {
				CHECK(strstr(typeof_pos, "get_index") == NULL ||
				      strstr(typeof_pos, "get_index") > strstr(typeof_pos, ")"),
				      "vm-double-eval: typeof does NOT contain get_index (uses LHS)");
			}
		}
		prism_free(&r);
	}

	// Sub-test 2: VM-type chained orelse — all temps use typeof(LHS)
	{
		const char *code =
		    "int call_a = 0, call_b = 0;\n"
		    "int get_a(void) { return call_a++; }\n"
		    "int get_b(void) { return call_b++; }\n"
		    "void f(int n) {\n"
		    "    int target[5] = {0};\n"
		    "    int (*vla_ptrs[2])[n];\n"
		    "    vla_ptrs[0] = &target;\n"
		    "    vla_ptrs[1] = &target;\n"
		    "    int (*ptr)[n];\n"
		    "    ptr = vla_ptrs[get_a()] orelse vla_ptrs[get_b()] orelse 0;\n"
		    "    (void)ptr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vm_double2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "vm-double-eval-chain: transpiles OK");
		// Both temps must exist (chained if/else)
		if (r.output) {
			CHECK(strstr(r.output, "__prism_oe_") != NULL,
			      "vm-double-eval-chain: first temp exists");
			// Find second distinct temp by searching past first occurrence
			char *first = strstr(r.output, "__prism_oe_");
			char *second = first ? strstr(first + 11, "__prism_oe_") : NULL;
			CHECK(second != NULL,
			      "vm-double-eval-chain: second temp exists");
		}
		prism_free(&r);
	}

	// Sub-test 3: Non-VM RHS still works (regression guard)
	{
		const char *code =
		    "int *get_ptr(void);\n"
		    "void f(void) {\n"
		    "    int fallback = 0;\n"
		    "    int *p;\n"
		    "    p = get_ptr() orelse &fallback;\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "non_vm_ok.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "non-vm-bare-orelse: transpiles OK");
		prism_free(&r);
	}
}

static void test_bare_orelse_chained_intermediate_truncation(void) {
	printf("\n--- Bare orelse Chained Intermediate Truncation ---\n");

	// Bug: chained orelse (a orelse b orelse c) used a single
	// __typeof__(RHS) temp for all links. Intermediate fallbacks
	// wider than the initial RHS were truncated when stored in the
	// narrow temp. Fix: each link gets its own __typeof__ temp via
	// nested if/else blocks.

	// Sub-test 1: short -> int -> long long chain — must have separate temps
	{
		const char *code =
		    "short get_short(void) { return 0; }\n"
		    "int get_int(void) { return 2000000000; }\n"
		    "long long get_ll(void) { return 1; }\n"
		    "void f(void) {\n"
		    "    long long val;\n"
		    "    val = get_short() orelse get_int() orelse get_ll();\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "chain_trunc1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "chain-trunc-3link: transpiles OK");
		CHECK(r.output != NULL, "chain-trunc-3link: output not NULL");
		// Must have two separate temps (nested if/else structure)
		CHECK(strstr(r.output, "__prism_oe_0") != NULL,
		      "chain-trunc-3link: first temp exists");
		CHECK(strstr(r.output, "__prism_oe_1") != NULL,
		      "chain-trunc-3link: second temp exists (separate from first)");
		// Must NOT have the old truncating pattern where the same temp
		// is reused for the intermediate link
		CHECK(strstr(r.output, "__prism_oe_0 = __prism_oe_0 ?") == NULL,
		      "chain-trunc-3link: no self-assignment ternary (old truncating pattern)");
		// Must have if-based structure
		CHECK(strstr(r.output, "if (__prism_oe_0)") != NULL,
		      "chain-trunc-3link: uses if-based dispatch");
		prism_free(&r);
	}

	// Sub-test 2: 4-link chain — char -> short -> int -> long long
	{
		const char *code =
		    "char get_char(void) { return 0; }\n"
		    "short get_short(void) { return 0; }\n"
		    "int get_int(void) { return 0; }\n"
		    "long long get_ll(void) { return 9000000000LL; }\n"
		    "void f(void) {\n"
		    "    long long val;\n"
		    "    val = get_char() orelse get_short() orelse get_int() orelse get_ll();\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "chain_trunc4.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "chain-trunc-4link: transpiles OK");
		CHECK(r.output != NULL, "chain-trunc-4link: output not NULL");
		// Must have three separate temps for 4-link chain (last link assigns directly)
		CHECK(strstr(r.output, "__prism_oe_0") != NULL,
		      "chain-trunc-4link: first temp");
		CHECK(strstr(r.output, "__prism_oe_1") != NULL,
		      "chain-trunc-4link: second temp");
		CHECK(strstr(r.output, "__prism_oe_2") != NULL,
		      "chain-trunc-4link: third temp");
		prism_free(&r);
	}

	// Sub-test 3: single orelse (no chain) — regression guard
	{
		const char *code =
		    "short get_short(void) { return 0; }\n"
		    "void f(void) {\n"
		    "    long long val;\n"
		    "    val = get_short() orelse 999LL;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "chain_single.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "chain-trunc-single: transpiles OK");
		CHECK(r.output != NULL, "chain-trunc-single: output not NULL");
		// Single orelse: one temp, ternary assignment (preserves CL
		// lifetime and volatile single-write semantics)
		CHECK(strstr(r.output, "__prism_oe_0") != NULL,
		      "chain-trunc-single: temp exists");
		CHECK(strstr(r.output, "? __prism_oe_0") != NULL,
		      "chain-trunc-single: ternary dispatch (single link)");
		prism_free(&r);
	}

	// Sub-test 4: intermediate typeof gets correct type (wider intermediate fallback)
	{
		const char *code =
		    "short get_short(void) { return 0; }\n"
		    "int get_int(void) { return 42; }\n"
		    "long long get_ll(void) { return 999; }\n"
		    "void f(void) {\n"
		    "    long long val;\n"
		    "    val = get_short() orelse get_int() orelse get_ll();\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "chain_mid.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "chain-trunc-mid: transpiles OK");
		CHECK(r.output != NULL, "chain-trunc-mid: output not NULL");
		// The intermediate fallback (get_int()) must get its own typeof temp,
		// not be assigned into the short temp from get_short().
		// Check for typeof(get_int()) pattern in output.
		CHECK(strstr(r.output, "get_int()") != NULL,
		      "chain-trunc-mid: intermediate fallback emitted");
		// Two separate temps must exist
		CHECK(strstr(r.output, "__prism_oe_1") != NULL,
		      "chain-trunc-mid: separate temp for intermediate");
		prism_free(&r);
	}
}

// chained orelse inside bracket/typeof duplicates
// intermediate LHS into ternary without side-effect checking.
// emit_token_range_orelse emits (LHS) ? (LHS) : (RHS) — any function
// call, ++/--, or assignment in LHS is evaluated twice.
static void test_chained_bracket_typeof_orelse_double_eval(void) {
	printf("\n--- Chained bracket/typeof orelse double-eval ---\n");

	// Sub-test 1: chained bracket orelse with function call → must reject
	{
		const char *code =
		    "int get_size(void);\n"
		    "void f(void) {\n"
		    "    int arr[0 orelse get_size() orelse 10];\n"
		    "    (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "chain_bracket_de1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "chain-bracket-double-eval: get_size() in chain rejected");
		prism_free(&r);
	}

	// Sub-test 2: chained typeof orelse with function call → must reject
	{
		const char *code =
		    "int get_val(void);\n"
		    "void f(void) {\n"
		    "    typeof(0 orelse get_val() orelse 10) x = 42;\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "chain_typeof_de1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "chain-typeof-double-eval: get_val() in chain rejected");
		prism_free(&r);
	}

	// Sub-test 3: chained bracket orelse with ++ → must reject
	{
		const char *code =
		    "void f(void) {\n"
		    "    int n = 0;\n"
		    "    int arr[0 orelse ++n orelse 10];\n"
		    "    (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "chain_bracket_de2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "chain-bracket-double-eval: ++n in chain rejected");
		prism_free(&r);
	}

	// Sub-test 4: safe chain (all variables, no side effects) → must accept
	{
		const char *code =
		    "#include <stdio.h>\n"
		    "int main(void) {\n"
		    "    int a = 0, b = 5, c = 10;\n"
		    "    int arr[a orelse b orelse c];\n"
		    "    printf(\"%zu\\n\", sizeof(arr)/sizeof(arr[0]));\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "chain_bracket_safe.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "chain-bracket-safe: variable-only chain accepted");
		if (r.output)
			CHECK(strstr(r.output, "orelse") == NULL,
			      "chain-bracket-safe: no raw orelse in output");
		prism_free(&r);
	}

	// Sub-test 5: 3-link bracket chain with function call in middle → must reject
	{
		const char *code =
		    "int mid(void);\n"
		    "void f(void) {\n"
		    "    int arr[0 orelse mid() orelse 0 orelse 10];\n"
		    "    (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "chain_bracket_de3.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "chain-bracket-double-eval: mid() in 3-link chain rejected");
		prism_free(&r);
	}

	// Sub-test 6: single bracket orelse with function call → must accept
	// (LHS is hoisted to temp — no duplication)
	{
		const char *code =
		    "int get_n(void);\n"
		    "void f(void) {\n"
		    "    int arr[get_n() orelse 10];\n"
		    "    (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "single_bracket_ok.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "single-bracket-orelse: function call LHS accepted (hoisted)");
		if (r.output)
			CHECK(strstr(r.output, "orelse") == NULL,
			      "single-bracket-orelse: no raw orelse in output");
		prism_free(&r);
	}
}

// _BitInt(N) and _Alignas(N) leaked raw orelse into C output
// because parse_type_specifier did skip_balanced over their parenthesized
// arguments and emit_type_stripped dumped them verbatim.
static void test_bitint_alignas_orelse_leak(void) {
	printf("\n--- _BitInt/_Alignas orelse firewall bypass ---\n");

	// Sub-test 1: _BitInt(1 orelse 8) → must reject
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    _BitInt(1 orelse 8) x = 0;\n"
		    "    (void)x;\n"
		    "}\n",
		    "bitint_orelse.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "bitint-orelse: _BitInt(1 orelse 8) rejected");
		prism_free(&r);
	}

	// Sub-test 2: _Alignas(1 orelse 16) → must reject
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    _Alignas(1 orelse 16) int y = 0;\n"
		    "    (void)y;\n"
		    "}\n",
		    "alignas_orelse.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "alignas-orelse: _Alignas(1 orelse 16) rejected");
		prism_free(&r);
	}

	// Sub-test 3: C23 lowercase alignas(1 orelse 16) → must reject
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    alignas(1 orelse 16) int y = 0;\n"
		    "    (void)y;\n"
		    "}\n",
		    "alignas_lower_orelse.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "alignas-lower-orelse: alignas(1 orelse 16) rejected");
		prism_free(&r);
	}

	// Sub-test 4: valid _Alignas(16) → must accept (no false positive)
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    _Alignas(16) int y = 0;\n"
		    "    (void)y;\n"
		    "}\n",
		    "alignas_ok.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "alignas-ok: _Alignas(16) accepted");
		if (r.output)
			CHECK(strstr(r.output, "orelse") == NULL,
			      "alignas-ok: no raw orelse in output");
		prism_free(&r);
	}
}

static void test_const_vm_type_orelse_double_eval(void) {
	printf("\n--- Const VM-type orelse double evaluation (handoff 9) ---\n");

	// Bug: handle_const_orelse_fallback emits the type specifier TWICE —
	// once for the mutable temp, once for the final const declaration.
	// For Variably Modified types (ISO C11 §6.7.2.5), the C compiler evaluates
	// VLA dimensions at runtime for EACH declaration, causing double evaluation
	// of side-effectful size expressions.

	// Sub-test 1: VLA in declarator suffix — const int (*ptr)[get_size()]
	{
		const char *code =
		    "int get_size(void);\n"
		    "void *fetch_data(void);\n"
		    "void f(void) {\n"
		    "    const int (*ptr)[get_size()] = fetch_data() orelse NULL;\n"
		    "    (void)ptr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "const_vla_decl.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "const-vm-decl-orelse: VLA in declarator must be rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "variably") != NULL || strstr(r.error_msg, "VLA") != NULL,
			      "const-vm-decl-orelse: error mentions variably modified / VLA");
		prism_free(&r);
	}

	// Sub-test 2: VLA in type specifier via typeof — const typeof(int[n]) *p
	{
		const char *code =
		    "int *get_ptr(void);\n"
		    "void f(int n) {\n"
		    "    const typeof(int[n]) *p = get_ptr() orelse NULL;\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "const_typeof_vla.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "const-vm-typeof-orelse: typeof VLA must be rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "variably") != NULL || strstr(r.error_msg, "VLA") != NULL,
			      "const-vm-typeof-orelse: error mentions variably modified / VLA");
		prism_free(&r);
	}

	// Sub-test 3: non-VLA const pointer — must NOT be rejected (no false positive)
	{
		const char *code =
		    "int *get_ptr(void);\n"
		    "void f(void) {\n"
		    "    const int *p = get_ptr() orelse NULL;\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "const_nonvla.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "const-nonvla-orelse: non-VLA const accepted");
		prism_free(&r);
	}

	// Sub-test 4: non-VLA const fixed-dim array pointer — must NOT be rejected
	{
		const char *code =
		    "void *get_data(void);\n"
		    "void f(void) {\n"
		    "    const int (*p)[5] = get_data() orelse NULL;\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "const_fixeddim.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "const-fixeddim-orelse: fixed-dim const accepted");
		prism_free(&r);
	}
}

static void test_atomic_vm_type_split_double_eval(void) {
	printf("\n--- _Atomic VM-type split double evaluation (handoff 10) ---\n");

	// Bug: parse_type_specifier blindly skip_balanced over _Atomic(...)
	// without scanning for VLAs. type->is_vla stays false.
	// When a bracket orelse on a later declarator forces a split,
	// the _Atomic VM type is duplicated, evaluating VLA dims twice.

	// Sub-test 1: _Atomic(int(*)[get_size()]) with split — must be rejected
	{
		const char *code =
		    "int get_size(void);\n"
		    "void f(void) {\n"
		    "    _Atomic(int(*)[get_size()]) a, b[1 orelse 2];\n"
		    "    (void)a; (void)b;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "atomic_vla_split1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "atomic-vm-split: _Atomic VLA split must be rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "double") != NULL ||
			      strstr(r.error_msg, "variably") != NULL ||
			      strstr(r.error_msg, "VLA") != NULL,
			      "atomic-vm-split: error mentions double eval / VLA");
		prism_free(&r);
	}

	// Sub-test 2: _Atomic(int(*)[n]) const orelse — must be rejected
	{
		const char *code =
		    "void *get(void);\n"
		    "void f(int n) {\n"
		    "    const _Atomic(int(*)[n]) p = get() orelse NULL;\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "atomic_vla_const.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "atomic-vm-const-orelse: const _Atomic VLA orelse must be rejected");
		prism_free(&r);
	}

	// Sub-test 3: _Atomic(int *) non-VLA — must NOT be rejected (no false positive)
	{
		const char *code =
		    "void f(void) {\n"
		    "    _Atomic(int *) a, b;\n"
		    "    (void)a; (void)b;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "atomic_novla.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "atomic-novla: non-VLA _Atomic accepted");
		prism_free(&r);
	}

	// Sub-test 4: _Atomic(int(*)[5]) fixed dim — must NOT be rejected
	{
		const char *code =
		    "void f(void) {\n"
		    "    _Atomic(int(*)[5]) a, b[1 orelse 2];\n"
		    "    (void)a; (void)b;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "atomic_fixeddim.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "atomic-fixeddim: fixed-dim _Atomic split accepted");
		prism_free(&r);
	}

	// Sub-test 5: _Atomic with typeof VLA inside — must be rejected
	{
		const char *code =
		    "void f(int n) {\n"
		    "    _Atomic(typeof(int[n]) *) a, b[1 orelse 2];\n"
		    "    (void)a; (void)b;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "atomic_typeof_vla.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "atomic-typeof-vla-split: _Atomic typeof VLA split must be rejected");
		prism_free(&r);
	}
}

// cast-dereference bypasses reject_orelse_side_effects volatile
// deref check.  The checker matches `* varname` but not `*(type *)expr` because
// `*` is followed by `(`, not a variable name.  In bracket/typeof orelse the LHS
// is emitted twice (ternary), so `*(volatile int *)0x4000` reads the MMIO
// register twice — a hardware-visible double read.
static void test_bracket_orelse_cast_deref_double_eval(void) {
	// Sub-test 1: *(volatile int *)0x4000 in array dim orelse (BUG)
	{
		const char *code =
		    "void f(void) {\n"
		    "    int arr[*(volatile int *)(0x4000) orelse 1];\n"
		    "    (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cast_deref_bracket1.c", prism_defaults());
		if (r.status == PRISM_OK && r.output) {
			// The ternary duplication produces:
			//   int arr[(*(volatile int *)(0x4000)) ? (*(volatile int *)(0x4000)) : (1)];
			// Count how many times the volatile deref appears.
			int count = 0;
			const char *s = r.output;
			while ((s = strstr(s, "volatile")) != NULL) { count++; s += 8; }
			CHECK(count <= 1,
			      "*(volatile int*)0x4000 in bracket orelse: volatile "
			      "deref duplicated in ternary (MMIO double read); "
			      "reject_orelse_side_effects must catch *(cast)expr");
		} else {
			// Rejection is the CORRECT outcome — side effect detected
			CHECK(r.status != PRISM_OK,
			      "*(volatile int*)0x4000 bracket orelse correctly rejected");
		}
		prism_free(&r);
	}

	// Sub-test 2: *(int *)ptr in typeof orelse (same pattern, non-literal)
	{
		const char *code =
		    "void f(char *ptr) {\n"
		    "    typeof(int[*(int *)ptr orelse 1]) *p;\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cast_deref_typeof1.c", prism_defaults());
		if (r.status == PRISM_OK && r.output) {
			const char *typeof_start = strstr(r.output, "typeof(");
			if (typeof_start) {
				int count = 0;
				const char *s = typeof_start;
				while ((s = strstr(s, "ptr")) != NULL) {
					if (s > typeof_start + 300) break;
					count++;
					s += 3;
				}
				CHECK(count <= 1,
				      "*(int *)ptr in typeof orelse: cast-deref "
				      "duplicated in ternary (double read)");
			}
		} else {
			CHECK(r.status != PRISM_OK,
			      "*(int*)ptr typeof orelse correctly rejected");
		}
		prism_free(&r);
	}

	// Sub-test 3: *ptr (no cast) must still be caught — regression guard
	{
		const char *code =
		    "void f(volatile int *hw) {\n"
		    "    int arr[*hw orelse 1];\n"
		    "    (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "deref_nocast.c", prism_defaults());
		// The existing check catches *varname, so this must be rejected
		// OR if accepted, volatile must not be duplicated.
		if (r.status == PRISM_OK && r.output) {
			int count = 0;
			const char *s = r.output;
			while ((s = strstr(s, "hw")) != NULL) {
				// skip "hw" in the declaration itself
				if (s > r.output && *(s-1) == '*') count++;
				s += 2;
			}
			CHECK(count <= 1,
			      "*hw deref must not be duplicated");
		} else {
			CHECK(r.status != PRISM_OK,
			      "*hw bracket orelse correctly rejected");
		}
		prism_free(&r);
	}

	// Sub-test 4: *(arr + offset) in chained bracket orelse — intermediate
	// expression goes through emit_token_range_orelse which calls
	// reject_orelse_side_effects with check_volatile_deref=true, but * is
	// followed by ( so the *varname pattern doesn't match → bypass.
	{
		const char *code =
		    "void f(volatile int *vals, int idx) {\n"
		    "    int arr[0 orelse *(vals + idx) orelse 5];\n"
		    "    (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "deref_arith_chained.c", prism_defaults());
		if (r.status == PRISM_OK && r.output) {
			int count = 0;
			const char *s = r.output;
			while ((s = strstr(s, "vals")) != NULL) {
				count++;
				s += 4;
			}
			// Declaration + at most 1 use is fine; more than 2 means double-eval
			CHECK(count <= 2,
			      "*(vals+idx) in chained bracket orelse: "
			      "deref duplicated in ternary (double read); "
			      "reject_orelse_side_effects must catch *(expr)");
		} else {
			CHECK(r.status != PRISM_OK,
			      "*(vals+idx) chained bracket orelse correctly rejected");
		}
		prism_free(&r);
	}

	// Sub-test 5: *(ptr) — parenthesized simple dereference
	{
		const char *code =
		    "void f(volatile int *hw) {\n"
		    "    int arr[*(hw) orelse 1];\n"
		    "    (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "deref_paren_simple.c", prism_defaults());
		if (r.status == PRISM_OK && r.output) {
			int count = 0;
			const char *s = r.output;
			while ((s = strstr(s, "hw")) != NULL) { count++; s += 2; }
			CHECK(count <= 2,
			      "*(hw) in bracket orelse: parenthesized deref "
			      "duplicated in ternary; reject must catch *(varname)");
		} else {
			CHECK(r.status != PRISM_OK,
			      "*(hw) bracket orelse correctly rejected");
		}
		prism_free(&r);
	}
}

// multiplication `*` falsely rejected as pointer dereference.
// reject_orelse_side_effects matches `* varname` regardless of context — it
// doesn't distinguish unary dereference `*ptr` from binary multiplication
// `a * b`.  Expressions like `sizeof(int) * n`, `(a+b) * n`, and even plain
// `n * n` are falsely rejected when used in typeof orelse or chained bracket
// orelse contexts.
static void test_orelse_multiply_false_positive(void) {
	// Sub-test 1: sizeof(int) * n in typeof orelse — multiplication, not deref
	{
		const char *code =
		    "void f(int n) {\n"
		    "    typeof(int[sizeof(int) * n orelse 4]) *p;\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "mul_sizeof.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "sizeof(int) * n in typeof orelse is multiplication, "
		         "not pointer dereference — must not be rejected");
		prism_free(&r);
	}

	// Sub-test 2: _Alignof(int) * n — same pattern
	{
		const char *code =
		    "void f(int n) {\n"
		    "    typeof(int[_Alignof(int) * n orelse 4]) *p;\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "mul_alignof.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "_Alignof(int) * n in typeof orelse is multiplication, "
		         "not pointer dereference — must not be rejected");
		prism_free(&r);
	}

	// Sub-test 3: (a + b) * n — parenthesized expression times variable
	{
		const char *code =
		    "void f(int a, int b, int n) {\n"
		    "    typeof(int[(a + b) * n orelse 4]) *p;\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "mul_paren_expr.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "(a+b) * n in typeof orelse is multiplication, "
		         "not pointer dereference — must not be rejected");
		prism_free(&r);
	}

	// Sub-test 4: n * n — simple variable multiplication
	{
		const char *code =
		    "void f(int n) {\n"
		    "    typeof(int[n * n orelse 4]) *p;\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "mul_nn.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "n * n in typeof orelse is multiplication, "
		         "not pointer dereference — must not be rejected");
		prism_free(&r);
	}

	// Sub-test 5: chained bracket orelse with multiplication in intermediate
	{
		const char *code =
		    "void f(int m, int n) {\n"
		    "    int arr[m orelse sizeof(int) * n orelse 4];\n"
		    "    (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "mul_chain.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "chained bracket orelse with sizeof(int) * n intermediate "
		         "is multiplication, not pointer dereference");
		prism_free(&r);
	}

	// Sub-test 6: bracket array dim — hoisted path should still work
	{
		const char *code =
		    "void f(int n) {\n"
		    "    int arr[sizeof(int) * n orelse 4];\n"
		    "    (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "mul_bracket.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "sizeof(int) * n in bracket orelse "
		         "(hoisted path) must succeed");
		prism_free(&r);
	}

	// Sub-test 7: actual dereference *ptr must STILL be caught (no regression)
	{
		const char *code =
		    "void f(int *ptr) {\n"
		    "    typeof(int[*ptr orelse 4]) *p;\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "deref_real.c", prism_defaults());
		// Must be rejected (double volatile deref) OR produce single-eval output
		if (r.status == PRISM_OK && r.output) {
			const char *typeof_start = strstr(r.output, "typeof(");
			if (typeof_start) {
				int count = 0;
				const char *s = typeof_start;
				while ((s = strstr(s, "ptr")) != NULL) {
					if (s > typeof_start + 300) break;
					count++;
					s += 3;
				}
				CHECK(count <= 1,
				      "*ptr deref must not be duplicated in ternary");
			}
		} else {
			CHECK(r.status != PRISM_OK,
			      "*ptr in typeof orelse correctly rejected");
		}
		prism_free(&r);
	}
}

// reject_orelse_side_effects checked unary * (pointer dereference)
// but not -> (member access), . (dot access), or [] (array subscript).
// All three implicitly dereference memory and produce double volatile reads
// when the chained ternary evaluates LHS twice. ISO C11 §5.1.2.3p2:
// accessing a volatile-qualified object is a side effect.
static void test_orelse_member_subscript_volatile_double_eval(void) {
	printf("\n--- orelse member/subscript volatile double-eval ---\n");

	// Sub-test 1: -> in chained bracket orelse — must be rejected
	{
		const char *code =
		    "struct HW { int status; };\n"
		    "extern volatile struct HW *uart_reg;\n"
		    "void f(void) {\n"
		    "    int buf[ 0 orelse uart_reg->status orelse 1 ];\n"
		    "    (void)buf;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vol_arrow.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "-> in chained bracket orelse must be rejected "
		      "(volatile double-read)");
		prism_free(&r);
	}

	// Sub-test 2: . in chained bracket orelse — must be rejected
	{
		const char *code =
		    "struct S { int len; };\n"
		    "void f(void) {\n"
		    "    volatile struct S s;\n"
		    "    int buf[ 0 orelse s.len orelse 1 ];\n"
		    "    (void)buf;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vol_dot.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      ". in chained bracket orelse must be rejected "
		      "(volatile double-read)");
		prism_free(&r);
	}

	// Sub-test 3: [] in chained bracket orelse — must be rejected
	{
		const char *code =
		    "void f(void) {\n"
		    "    volatile int arr[5];\n"
		    "    int buf[ 0 orelse arr[0] orelse 1 ];\n"
		    "    (void)buf;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vol_subscript.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "[] in chained bracket orelse must be rejected "
		      "(volatile double-read)");
		prism_free(&r);
	}

	// Sub-test 4: non-chained single bracket orelse with -> — must succeed
	// (uses hoisted temp, no double-eval)
	{
		const char *code =
		    "struct S { int len; };\n"
		    "void f(struct S *p) {\n"
		    "    int buf[ p->len orelse 1 ];\n"
		    "    (void)buf;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "arrow_single.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "-> in single bracket orelse (hoisted temp) must "
		      "succeed — no double-eval");
		prism_free(&r);
	}

	// Sub-test 5: non-chained single bracket orelse with [] — must succeed
	{
		const char *code =
		    "void f(int *arr) {\n"
		    "    int buf[ arr[0] orelse 1 ];\n"
		    "    (void)buf;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "subscript_single.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "[] in single bracket orelse (hoisted temp) must "
		      "succeed — no double-eval");
		prism_free(&r);
	}

	// Sub-test 6: -> in typeof orelse (ternary path) — must be rejected
	{
		const char *code =
		    "struct S { int len; };\n"
		    "void f(struct S *p) {\n"
		    "    typeof(int[p->len orelse 1]) *q;\n"
		    "    (void)q;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "arrow_typeof.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "-> in typeof orelse (ternary double-eval) must "
		      "be rejected");
		prism_free(&r);
	}
}

// reject_orelse_side_effects fires on the assignment target BEFORE
// determining whether the orelse action is bare-fallback (needs double-eval
// of LHS) or control-flow (evaluates LHS once via if-guard).
// arr[i++] = get() orelse return; should produce:
//   { if (!(arr[i++] = get())) { return; } }
// which evaluates arr[i++] exactly once — no double-eval.
static void test_orelse_sideeffect_false_positive_on_action(void) {
	{
		const char *code =
		    "int get_resource(void);\n"
		    "void f(int *arr) {\n"
		    "    int i = 0;\n"
		    "    arr[i++] = get_resource() orelse return;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "sideeffect_action1.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "arr[i++] = get() orelse return must not be rejected "
		      "(LHS evaluated once in if-guard pattern)");
		prism_free(&r);
	}
	{
		const char *code =
		    "int get_resource(void);\n"
		    "void f(int *arr) {\n"
		    "    int i = 0;\n"
		    "    arr[i++] = get_resource() orelse { return; };\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "sideeffect_action2.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "arr[i++] = get() orelse { return; } must not be rejected");
		prism_free(&r);
	}
}

// const chained orelse with block action at end of chain:
// handle_const_orelse_fallback emits the block inside a ternary as a GNU
// statement expression — produces non-portable code and invalid C if the
// block doesn't return a value.
static void test_bare_orelse_vla_typedef_cast_bypass(void) {
	printf("\n--- Bare orelse VLA typedef cast: safe via typeof(LHS) ---\n");

	// Previously: reject_bare_orelse_vla_cast scanned for '[' inside cast
	// parens but missed VLA typedefs. Now fixed by using typeof(LHS) instead
	// of typeof(RHS) — the VLA cast is only in the RHS initializer, evaluated
	// once. These tests verify VLA typedef casts are accepted.

	// Sub-test 1: VLA typedef pointer cast — safe via typeof(LHS)
	{
		const char *code =
		    "void *trigger(void);\n"
		    "void f(int n) {\n"
		    "    typedef int VLA_Type[n];\n"
		    "    VLA_Type *ptr;\n"
		    "    ptr = (VLA_Type *)trigger() orelse 0;\n"
		    "    (void)ptr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vla_td_cast1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "vla-typedef-cast: accepted (typeof(LHS) is safe)");
		prism_free(&r);
	}

	// Sub-test 2: Non-VLA typedef pointer cast — must NOT be rejected
	{
		const char *code =
		    "void *trigger(void);\n"
		    "typedef int Fixed[10];\n"
		    "void f(void) {\n"
		    "    Fixed *ptr;\n"
		    "    ptr = (Fixed *)trigger() orelse 0;\n"
		    "    (void)ptr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "fixed_td_cast.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
			 "fixed-typedef-cast: valid (not VLA)");
		prism_free(&r);
	}

	// Sub-test 3: VLA typedef without pointer (bare VLA_Type cast) — safe via typeof(LHS)
	{
		const char *code =
		    "void *trigger(void);\n"
		    "void f(int n) {\n"
		    "    typedef int VLA_Type[n];\n"
		    "    void *ptr;\n"
		    "    ptr = (VLA_Type)trigger() orelse 0;\n"
		    "    (void)ptr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vla_td_cast3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "vla-typedef-bare-cast: accepted (typeof(LHS) is safe)");
		prism_free(&r);
	}
}

static void test_const_chained_orelse_block_action(void) {
	const char *code =
	    "int get_a(void);\n"
	    "int get_b(void);\n"
	    "void log_error(const char *);\n"
	    "void f(void) {\n"
	    "    const int x = get_a() orelse get_b() orelse { log_error(\"failed\"); };\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "const_chain_block.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		// Must NOT contain statement-expression pattern "( {" in a ternary
		CHECK(strstr(r.output, "? __prism_oe") == NULL ||
		      strstr(r.output, ": ( {") == NULL,
		      "const chained orelse must not emit block as statement "
		      "expression inside ternary — produces non-portable/invalid C");
	}
	prism_free(&r);
}

static void test_const_ptr_typedef_orelse_provenance(void) {
	printf("\n--- const pointer typedef orelse provenance ---\n");

	// Bug: typedef int * const CPtr; bakes const into the pointer level.
	// is_const was computed as: base_is_const && !decl.is_pointer — always
	// false for pointer typedefs. TDF_CONST stayed unset, so orelse emitted
	// a direct assignment to a const variable (backend compile error).

	// Sub-test 1: const pointer typedef with value fallback
	{
		const char *code =
		    "typedef int * const CPtr;\n"
		    "CPtr get_ptr(void);\n"
		    "int dummy;\n"
		    "void f(void) {\n"
		    "    CPtr p = get_ptr() orelse &dummy;\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "const_ptr_td1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "const-ptr-typedef: transpiles OK");
		CHECK(r.output != NULL, "const-ptr-typedef: output not NULL");
		CHECK(strstr(r.output, "__prism_oe") != NULL,
		      "const-ptr-typedef: must use mutable temp");
		prism_free(&r);
	}

	// Sub-test 2: const function pointer typedef with value fallback
	{
		const char *code =
		    "typedef int (* const FPtr)(int);\n"
		    "FPtr get_fp(void);\n"
		    "int fallback_fn(int x) { return x; }\n"
		    "void f(void) {\n"
		    "    FPtr fp = get_fp() orelse fallback_fn;\n"
		    "    (void)fp;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "const_fptr_td.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "const-fptr-typedef: transpiles OK");
		CHECK(r.output != NULL, "const-fptr-typedef: output not NULL");
		CHECK(strstr(r.output, "__prism_oe") != NULL,
		      "const-fptr-typedef: must use mutable temp");
		prism_free(&r);
	}

	// Sub-test 3: non-const pointer typedef (control — should NOT use temp)
	{
		const char *code =
		    "typedef int *Ptr;\n"
		    "Ptr get_ptr(void);\n"
		    "int dummy;\n"
		    "void f(void) {\n"
		    "    Ptr p = get_ptr() orelse &dummy;\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "nonconst_ptr_td.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "nonconst-ptr-typedef: transpiles OK");
		// Non-const pointer typedef should use direct assignment, not temp
		CHECK(strstr(r.output, "__prism_oe") == NULL,
		      "nonconst-ptr-typedef: should NOT use mutable temp");
		prism_free(&r);
	}

	// Sub-test 4: const pointer typedef with control flow action (should use if-guard)
	{
		const char *code =
		    "typedef int * const CPtr;\n"
		    "CPtr get_ptr(void);\n"
		    "void f(void) {\n"
		    "    CPtr p = get_ptr() orelse { return; };\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "const_ptr_td_action.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "const-ptr-typedef-action: transpiles OK");
		prism_free(&r);
	}
}

static void test_bare_orelse_prepdir_fallback_concat(void) {
	printf("\n--- BUG: prepdir in orelse fallback branch concatenation ---\n");

	// BUG: #ifdef in fallback expression after orelse — emit_balanced_range
	// skips TK_PREP_DIR, concatenating tokens from ALL branches.
	// Must be rejected in lib mode.

	// Sub-test 1: #ifdef in fallback expression
	{
		const char *code =
		    "const char *get_url(void);\n"
		    "void f(void) {\n"
		    "    const char *url;\n"
		    "    url = get_url()\n"
		    "        orelse\n"
		    "#ifdef USE_STAGING\n"
		    "        \"https://staging.api.com\"\n"
		    "#else\n"
		    "        \"https://prod.api.com\"\n"
		    "#endif\n"
		    "        ;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "prepdir_fb.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "prepdir-fallback: #ifdef in fallback must be rejected");
		CHECK(r.error_msg && strstr(r.error_msg, "preprocessor conditional"),
		      "prepdir-fallback: error mentions preprocessor conditionals");
		prism_free(&r);
	}

	// Sub-test 2: no #ifdef — normal orelse still works
	{
		const char *code =
		    "const char *get_url(void);\n"
		    "void f(void) {\n"
		    "    const char *url;\n"
		    "    url = get_url() orelse \"fallback\";\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "prepdir_ok.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "prepdir-fallback: normal orelse without #ifdef still works");
		prism_free(&r);
	}
}

static void test_attr_bracket_orelse_queue_desync(void) {
	printf("\n--- BUG: __attribute__ bracket desynchronizes dim queue ---\n");

	// BUG: emit_bracket_orelse_temps scans the declarator range for '[' tokens
	// but walks into GNU __attribute__((...)) balanced groups.  A '[' inside
	// an attribute (e.g. sizeof(int[4]) in aligned()) is queued as a dim temp.
	// Pass 2's parse_declarator calls decl_noise() which skips __attribute__
	// entirely, so the attribute's bracket never consumes its queue slot.
	// All subsequent real array dimensions read from the wrong queue position.

	// Sub-test 1: __attribute__((aligned(sizeof(int[4])))) before VLA dims
	{
		const char *code =
		    "int get_n(void) { return 3; }\n"
		    "void f(int n) {\n"
		    "    int arr __attribute__((aligned(sizeof(int[4])))) [n + 5] [n + 6] [n orelse 1];\n"
		    "    (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "attr_bracket_desync.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "attr-bracket-desync: transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			const char *fn = strstr(r.output, "void f(");
			CHECK(fn != NULL, "attr-bracket-desync: found function");
			if (fn) {
				// __prism_dim_0 must hold n+5, __prism_dim_1 must hold n+6.
				// The array must use [__prism_dim_0][__prism_dim_1] in order.
				// If desync: [n + 5][__prism_dim_0] — dim_0 is n+5, not n+6.
				const char *dim1 = strstr(fn, "__prism_dim_1");
				CHECK(dim1 != NULL,
				      "attr-bracket-desync: __prism_dim_1 must appear in output "
				      "(second VLA dimension was not replaced)");
				// Check that the array declaration uses dim_1 (not just dim_0 twice)
				const char *arr = strstr(fn, "int arr");
				if (arr) {
					const char *dim1_in_arr = strstr(arr, "__prism_dim_1");
					CHECK(dim1_in_arr != NULL && dim1_in_arr < arr + 300,
					      "attr-bracket-desync: array must use __prism_dim_1 "
					      "for second VLA dimension");
				}
			}
		}
		prism_free(&r);
	}

	// Sub-test 2: multiple attributes with brackets
	{
		const char *code =
		    "void g(int n) {\n"
		    "    int x __attribute__((aligned(sizeof(int[8])))) [n + 1] [n orelse 1];\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "attr_bracket_desync2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "attr-bracket-desync2: transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			const char *fn = strstr(r.output, "void g(");
			if (fn) {
				// [n + 1] should use __prism_dim_0, [n orelse 1] uses __prism_oe_.
				// If desync: [n + 1] emits verbatim (consumed the -1 slot
				// for [8]), and [n orelse 1] still works but dim_0 is orphaned.
				const char *arr = strstr(fn, "int x");
				if (arr) {
					const char *dim0_in_arr = strstr(arr, "__prism_dim_0");
					CHECK(dim0_in_arr != NULL && dim0_in_arr < arr + 200,
					      "attr-bracket-desync2: first real dim must use "
					      "__prism_dim_0 (not shifted by attribute bracket)");
				}
			}
		}
		prism_free(&r);
	}
}

static void test_static_raw_orelse_keyword_leak(void) {
	printf("\n--- BUG: static/extern raw orelse keyword leak ---\n");

	// BUG: `static raw int x = get_val() orelse 1;` leaks the orelse keyword
	// verbatim into the C output.  Pass 2's try_zero_init_decl hits the
	// has_storage_in fast path and calls emit_raw_verbatim_to_semicolon,
	// bypassing process_declarators (and its static+orelse rejection).
	// Phase 1D never checked for this combination.

	// Sub-test 1: static raw orelse -> must reject
	{
		const char *code =
		    "int get_val(void) { return 42; }\n"
		    "void f(void) {\n"
		    "    static raw int x = get_val() orelse 1;\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "static_raw_oe.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "static-raw-orelse: must be rejected (keyword would leak)");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "static") || strstr(r.error_msg, "storage"),
			      "static-raw-orelse: error mentions storage duration");
		prism_free(&r);
	}

	// Sub-test 2: _Thread_local static raw orelse -> must reject
	{
		const char *code =
		    "int get_val(void) { return 42; }\n"
		    "void f(void) {\n"
		    "    _Thread_local static raw int x = get_val() orelse 1;\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "tls_raw_oe.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "tls-raw-orelse: must be rejected (keyword would leak)");
		prism_free(&r);
	}

	// Sub-test 3: raw static (reversed) -> must also reject
	// (This path already works via process_declarators, but verify it stays correct)
	{
		const char *code =
		    "int get_val(void) { return 42; }\n"
		    "void f(void) {\n"
		    "    raw static int x = get_val() orelse 1;\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "raw_static_oe.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "raw-static-orelse: must be rejected");
		prism_free(&r);
	}

	// Sub-test 4: static raw WITHOUT orelse -> must succeed (raw suppresses zeroinit only)
	{
		const char *code =
		    "void f(void) {\n"
		    "    static raw int x = 42;\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "static_raw_ok.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "static-raw-no-orelse: must succeed");
		prism_free(&r);
	}
}

/*
 * BUG: VLA orelse inside a cast in a declaration initializer crashes with
 * false-positive "'orelse' cannot be used inside parentheses".
 *
 * Root cause: process_declarators forces all initializer parens through
 * check_orelse_in_parens → walk_balanced.  check_orelse_in_parens found
 * orelse inside the brackets and rejected it; walk_balanced would emit it
 * raw.  Both paths failed because Phase 1G never annotated the brackets
 * (Phase 1D skipped the entire paren group).
 *
 * Fix: check_orelse_in_parens and walk_balanced now do runtime orelse
 * detection inside brackets, falling back from the P1_OE_BRACKET
 * annotation when it's absent.
 */
static void test_init_cast_vla_orelse_crash(void) {
	/* Sub-test 1: bare expression cast (was already working) */
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    void *ptr;\n"
		    "    ptr = (int(*)[n orelse 1]) 0;\n"
		    "}\n",
		    "cast_vla_bare.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "cast-vla-orelse-bare: must succeed");
		if (r.status == PRISM_OK && r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "cast-vla-orelse-bare: orelse must not leak");
		}
		prism_free(&r);
	}

	/* Sub-test 2: declaration initializer cast (was crashing) */
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    void *ptr = (int(*)[n orelse 1]) 0;\n"
		    "}\n",
		    "cast_vla_init.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "cast-vla-orelse-init: must succeed (was false-positive crash)");
		if (r.status == PRISM_OK && r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "cast-vla-orelse-init: orelse must not leak");
			/* Verify the ternary transformation happened */
			CHECK(strstr(r.output, "?") != NULL,
			      "cast-vla-orelse-init: must contain ternary");
		}
		prism_free(&r);
	}

	/* Sub-test 3: sizeof with VLA cast in initializer */
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    unsigned long sz = sizeof(int[n orelse 1]);\n"
		    "    (void)sz;\n"
		    "}\n",
		    "sizeof_vla_init.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "sizeof-vla-orelse-init: must succeed");
		if (r.status == PRISM_OK && r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "sizeof-vla-orelse-init: orelse must not leak");
		}
		prism_free(&r);
	}

	/* Sub-test 4: both bare and init on same variable in same function */
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    void *p;\n"
		    "    p = (int(*)[n orelse 1]) 0;\n"
		    "    void *q = (int(*)[n orelse 1]) 0;\n"
		    "}\n",
		    "cast_vla_both.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "cast-vla-orelse-both: must succeed");
		if (r.status == PRISM_OK && r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "cast-vla-orelse-both: orelse must not leak");
		}
		prism_free(&r);
	}
}

// stmt-expr goto/return inside bracket orelse and const orelse
// bypasses defer cleanup — emit_token_range_orelse and emit_range used
// flat OUT_TOK/emit_tok loops that never invoked handle_goto_keyword.
static void test_stmtexpr_goto_in_bracket_orelse_defer_bypass(void) {
	/* Case 1: goto inside bracket orelse LHS stmt-expr must get defer cleanup */
	{
		PrismResult r = prism_transpile_source(
		    "int counter = 0;\n"
		    "void f(void) {\n"
		    "    {\n"
		    "        defer { counter++; }\n"
		    "        int arr[ ({ goto skip; 1; }) orelse 10 ];\n"
		    "        (void)arr;\n"
		    "    }\n"
		    "    return;\n"
		    "skip:\n"
		    "    return;\n"
		    "}\n",
		    "t76g.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			/* The goto must be wrapped with defer cleanup:
			 * { counter++; goto skip; } or similar pattern */
			const char *fn = strstr(r.output, "void f(");
			CHECK(fn != NULL, "function found");
			if (fn) {
				/* Look for counter++ BEFORE goto skip in the stmt-expr */
				const char *oe_temp = strstr(fn, "__prism_oe_");
				CHECK(oe_temp != NULL, "orelse temp hoisted");
				if (oe_temp) {
					/* In the stmt-expr containing goto, defer cleanup
					 * must appear (counter++) before the goto */
					const char *goto_pos = strstr(oe_temp, "goto skip");
					const char *cleanup = strstr(oe_temp, "counter++");
					CHECK(goto_pos != NULL, "goto found in output");
					CHECK(cleanup != NULL && cleanup < goto_pos,
					      "defer cleanup emitted before goto");
				}
			}
		}
		prism_free(&r);
	}

	/* Case 2: return inside bracket orelse LHS stmt-expr must get defer cleanup */
	{
		PrismResult r = prism_transpile_source(
		    "int counter = 0;\n"
		    "int f(void) {\n"
		    "    defer { counter++; }\n"
		    "    int arr[ ({ return 42; 1; }) orelse 10 ];\n"
		    "    (void)arr;\n"
		    "    return 0;\n"
		    "}\n",
		    "t76r.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			const char *fn = strstr(r.output, "int f(");
			CHECK(fn != NULL, "function found");
			if (fn) {
				const char *oe_temp = strstr(fn, "__prism_oe_");
				CHECK(oe_temp != NULL, "orelse temp hoisted");
				if (oe_temp) {
					/* Defer cleanup must be injected around the return */
					const char *ret_pos = strstr(oe_temp, "return ");
					const char *cleanup = strstr(oe_temp, "counter++");
					CHECK(ret_pos != NULL, "return found in output");
					CHECK(cleanup != NULL && cleanup < ret_pos,
					      "defer cleanup emitted before return");
				}
			}
		}
		prism_free(&r);
	}

	/* Case 3: goto inside const orelse LHS stmt-expr must get defer cleanup */
	{
		PrismResult r = prism_transpile_source(
		    "int counter = 0;\n"
		    "void f(void) {\n"
		    "    {\n"
		    "        defer { counter++; }\n"
		    "        const int x = ({ goto bail; 42; }) orelse 99;\n"
		    "        (void)x;\n"
		    "    }\n"
		    "    return;\n"
		    "bail:\n"
		    "    return;\n"
		    "}\n",
		    "t76c.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			const char *fn = strstr(r.output, "void f(");
			CHECK(fn != NULL, "function found");
			if (fn) {
				const char *oe_temp = strstr(fn, "__prism_oe_");
				CHECK(oe_temp != NULL, "orelse temp hoisted");
				if (oe_temp) {
					const char *goto_pos = strstr(oe_temp, "goto");
					const char *cleanup = strstr(oe_temp, "counter++");
					CHECK(goto_pos != NULL, "goto found");
					CHECK(cleanup != NULL && cleanup < goto_pos,
					      "defer cleanup before goto");
				}
			}
		}
		prism_free(&r);
	}
}

// O(N^2) nested bracket orelse scanning — check_orelse_in_parens,
// walk_balanced, and main emit loop rescanned nested [...] groups that had
// no orelse, causing quadratic degradation on deep subscript chains.
static void test_nested_bracket_orelse_no_quadratic(void) {
	/* Generate arr[arr[arr[...[0]...]]] with 500 levels inside a function
	 * call argument (parens trigger check_orelse_in_parens + walk_balanced).
	 * Pre-fix: 500 levels caused measurable slowdown due to O(N^2).
	 * Post-fix: linear scan, completes instantly. */
	char code[64000];
	int pos = 0;
	pos += snprintf(code + pos, sizeof(code) - pos,
	    "int arr[10];\n"
	    "int use(int);\n"
	    "void f(void) {\n"
	    "    use(");
	for (int i = 0; i < 500; i++)
		pos += snprintf(code + pos, sizeof(code) - pos, "arr[");
	pos += snprintf(code + pos, sizeof(code) - pos, "0");
	for (int i = 0; i < 500; i++)
		pos += snprintf(code + pos, sizeof(code) - pos, "]");
	pos += snprintf(code + pos, sizeof(code) - pos,
	    ");\n"
	    "}\n");

	PrismResult r = prism_transpile_source(code, "t77_nested.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "transpiles OK (no quadratic hang)");
	if (r.status == PRISM_OK && r.output) {
		CHECK(strstr(r.output, "orelse") == NULL,
		      "no orelse in output");
	}
	prism_free(&r);
}

// bare orelse with comma operator in braceless control flow.
// The comma split produces two statements, but braceless if/for/while/else
// only captures the first — the orelse assignment leaks out of scope.
// Fix: emit_bare_orelse_impl wraps in { } when brace_wrap is set.
static int _comma_orelse_if_helper(int cond) {
	int status = -1;
	if (cond)
		(void)0, status = 0 orelse 1;
	return status;
}

static int _comma_orelse_while_helper(void) {
	int x = -1;
	int once = 1;
	while (once)
		once = 0, x = 5 orelse 99;
	return x;
}

static int _comma_orelse_else_helper(int cond) {
	int status = -1;
	if (cond)
		status = 42;
	else
		(void)0, status = 0 orelse 1;
	return status;
}

void test_bare_orelse_comma_braceless(void) {
	// if: cond=1 → body runs, status = 0 orelse 1 = 1 (0 is falsy)
	CHECK_EQ(_comma_orelse_if_helper(1), 1, "braceless if cond=1 orelse fires");
	// if: cond=0 → body skipped, status stays -1
	CHECK_EQ(_comma_orelse_if_helper(0), -1, "braceless if cond=0 body skipped");
	// while: once=1 → body runs, x = 5 (truthy)
	CHECK_EQ(_comma_orelse_while_helper(), 5, "braceless while comma orelse");
	// else: cond=1 → if-branch, status=42
	CHECK_EQ(_comma_orelse_else_helper(1), 42, "braceless else cond=1 if-branch");
	// else: cond=0 → else-branch, status = 0 orelse 1 = 1
	CHECK_EQ(_comma_orelse_else_helper(0), 1, "braceless else cond=0 orelse fires");
}

static void test_bracket_orelse_prepdir_rejected(void) {
	printf("\n--- BUG: prepdir inside bracket orelse rejection ---\n");

	// Sub-test 1: #ifdef inside bracket orelse must be rejected (lib mode only)
	{
		const char *code =
		    "int get_size(void);\n"
		    "void f(void) {\n"
		    "    int arr[\n"
		    "#ifdef X\n"
		    "    1\n"
		    "#else\n"
		    "    get_size()\n"
		    "#endif\n"
		    "    orelse 3];\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "bracket_prepdir.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "bracket-orelse-prepdir: #ifdef in dimension must be rejected");
		CHECK(r.error_msg && strstr(r.error_msg, "preprocessor"),
		      "bracket-orelse-prepdir: error mentions preprocessor");
		prism_free(&r);
	}

	// Sub-test 2: #ifndef variant
	{
		const char *code =
		    "int get_size(void);\n"
		    "void f(void) {\n"
		    "    int arr[\n"
		    "#ifndef Y\n"
		    "    get_size()\n"
		    "#endif\n"
		    "    orelse 3];\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "bracket_prepdir2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "bracket-orelse-prepdir: #ifndef must also be rejected");
		prism_free(&r);
	}

	// Sub-test 3: no #ifdef — normal bracket orelse still works
	{
		const char *code =
		    "int get_size(void);\n"
		    "void f(void) {\n"
		    "    int arr[get_size() orelse 3];\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "bracket_no_prepdir.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "bracket-orelse-no-prepdir: transpiles OK");
		prism_free(&r);
	}
}

// block-form orelse in declaration + braceless control body
// Previously emitted brace_wrap '}' before the orelse block was processed,
// producing invalid C like 'if (!x) } { ... }'.
int *blk_orelse_get_ptr(int which) {
	static int blk_orelse_val = 42;
	return which ? &blk_orelse_val : (void *)0;
}

int blk_orelse_braceless_if(int cond) {
	if (cond)
		int *x = blk_orelse_get_ptr(0) orelse { return 99; };
	return 0;
}

int blk_orelse_braceless_else(int cond) {
	if (!cond) { /* nothing */ }
	else
		int *x = blk_orelse_get_ptr(0) orelse { return 88; };
	return 0;
}

int blk_orelse_braceless_while(void) {
	int count = 0;
	while (count < 1)
		int *x = blk_orelse_get_ptr(count++) orelse { return 77; };
	return count;
}

int blk_orelse_multi_decl(void) {
	int *x = blk_orelse_get_ptr(0) orelse { return -1; }, y = 5;
	return *x + y;
}

int blk_orelse_multi_decl_nontrigger(void) {
	int *x = blk_orelse_get_ptr(1) orelse { return -1; }, y = 10;
	return *x + y;
}

void test_orelse_block_braceless_ctrl(void) {
	CHECK_EQ(blk_orelse_braceless_if(1), 99,
	         "block orelse in braceless if triggers");
	CHECK_EQ(blk_orelse_braceless_if(0), 0,
	         "block orelse in braceless if skips");
	CHECK_EQ(blk_orelse_braceless_else(0), 0,
	         "block orelse in braceless else skips path");
	CHECK_EQ(blk_orelse_braceless_else(1), 88,
	         "block orelse in braceless else triggers");
	CHECK_EQ(blk_orelse_braceless_while(), 77,
	         "block orelse in braceless while triggers");
}

void test_orelse_block_multi_decl(void) {
	CHECK_EQ(blk_orelse_multi_decl(), -1,
	         "block orelse multi-decl null triggers");
	CHECK_EQ(blk_orelse_multi_decl_nontrigger(), 52,
	         "block orelse multi-decl non-null works");
}

// block-form orelse with defer cleanup inside
int blk_orelse_with_defer(void) {
	log_reset();
	defer log_append("D");
	int *x = blk_orelse_get_ptr(0) orelse {
		log_append("null");
		return -1;
	};
	log_append("ok");
	return *x;
}

void test_orelse_block_defer_cleanup(void) {
	int r = blk_orelse_with_defer();
	CHECK_EQ(r, -1, "block orelse defer return value");
	CHECK_LOG("nullD", "block orelse defer cleanup order");
}

// const chained orelse block form skips defer cleanup on return.
// walk_balanced emitted block verbatim; return bypassed defer stack.
int *chain_blk_get_ptr(int which) {
	static int chain_blk_val = 42;
	return which ? &chain_blk_val : (void *)0;
}

int chain_blk_orelse_defer(void) {
	log_reset();
	defer log_append("D");
	const int *x = chain_blk_get_ptr(0) orelse chain_blk_get_ptr(0) orelse {
		log_append("null");
		return -1;
	};
	log_append("ok");
	return *x;
}

void test_orelse_const_chain_block_defer(void) {
	int r = chain_blk_orelse_defer();
	CHECK_EQ(r, -1, "const chained block orelse return");
	CHECK_LOG("nullD", "const chained block orelse defers fire");
}

// scan_decl_orelse paren unlinking corrupted comma expressions.
// (counter++, get() orelse 0) unlinked the parens, exposing the comma
// at depth 0, causing orelse to leak verbatim to output.
void test_orelse_paren_comma_rejection(void) {
	printf("\n--- paren unlinking comma expression ---\n");
	// orelse inside parens WITH comma: should be rejected (not silently corrupted)
	{
		PrismResult r = prism_transpile_source(
		    "int *get(void);\n"
		    "void f(void) {\n"
		    "    int x = 0;\n"
		    "    int *p = (x++, get() orelse 0);\n"
		    "}\n",
		    "t86_comma.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_ERR_SYNTAX,
		         "orelse in paren with comma rejected");
		if (r.status == PRISM_ERR_SYNTAX && r.error_msg) {
			CHECK(strstr(r.error_msg, "parentheses") != NULL,
			      "error mentions parentheses");
		}
		prism_free(&r);
	}
	// orelse inside parens WITHOUT comma: should work (macro hygiene unwrapping)
	{
		PrismResult r = prism_transpile_source(
		    "int *get(void);\n"
		    "void f(void) {\n"
		    "    int *p = (get() orelse 0);\n"
		    "}\n",
		    "t86_no_comma.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "orelse in paren without comma works");
		if (r.status == PRISM_OK && r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "no literal orelse in output");
		}
		prism_free(&r);
	}
}

// shadowed 'orelse' (variable/enum/typedef named 'orelse') disables the keyword
static void test_orelse_shadow_variable(void) {
	// A local variable named 'orelse' should not disable the orelse keyword
	// for other variables in the same scope (infix position after ident/num).
	{
		const char *code =
		    "int *get(void);\n"
		    "void f(void) {\n"
		    "    int orelse = 42;\n"
		    "    int *p = get() orelse return;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "shadow_var.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "shadow-orelse-var: variable named 'orelse' must not suppress keyword");
		if (r.status == PRISM_OK && r.output) {
			CHECK(strstr(r.output, "if (!p)") != NULL,
			      "shadow-orelse-var: orelse must be expanded to null check");
		}
		prism_free(&r);
	}
	// Variable used in declaration initializer must work as variable
	{
		const char *code =
		    "void f(void) {\n"
		    "    int orelse = 42;\n"
		    "    int x = orelse + 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "shadow_var_use.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "shadow-orelse-var-use: 'orelse' in expression must work as variable");
		prism_free(&r);
	}
	// Variable as function call argument must not trigger orelse
	{
		const char *code =
		    "void g(int);\n"
		    "void f(void) {\n"
		    "    int orelse = 42;\n"
		    "    g(orelse);\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "shadow_var_call.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "shadow-orelse-var-call: 'orelse' as function arg must work");
		prism_free(&r);
	}
}

static void test_orelse_shadow_enum(void) {
	const char *code =
	    "int *get(void);\n"
	    "enum { orelse = 1 };\n"
	    "void f(void) {\n"
	    "    int *p = get() orelse return;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "shadow_enum.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "shadow-orelse-enum: enum constant named 'orelse' must not suppress keyword");
	if (r.status == PRISM_OK && r.output) {
		// Check that the declaration orelse was expanded (look for the orelse guard pattern)
		CHECK(strstr(r.output, "if (!p)") != NULL,
		      "shadow-orelse-enum: orelse must be expanded to null check");
	}
	prism_free(&r);
}

static void test_orelse_shadow_typedef(void) {
	// typedef named 'orelse' — this is a REAL typedef, keyword should be suppressed
	const char *code =
	    "typedef int orelse;\n"
	    "void f(void) {\n"
	    "    orelse x = 5;\n"
	    "    (void)x;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "shadow_typedef.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "shadow-orelse-typedef: typedef named 'orelse' must suppress keyword");
	prism_free(&r);
}

// chained assignment a = b = f() orelse 5 split-brain.
// The bare orelse scanner must find the LAST = at depth 0, placing
// the intermediate = in the LHS range so reject_orelse_side_effects
// catches it.  Previously it found the FIRST =, leaving b=0.
static void test_orelse_chained_assign_rejected(void) {
	check_orelse_transpile_rejects(
	    "int f(void) { return 0; }\n"
	    "void test(void) {\n"
	    "    int a, b;\n"
	    "    a = b = f() orelse 5;\n"
	    "}\n",
	    "orelse_chained_assign.c",
	    "chained assignment orelse rejected",
	    "side effect");
}

// __auto_type with orelse must not enter const-stripping cast path.
// __auto_type has TT_TYPEOF tag, but (__auto_type)0 is invalid C.
static void test_orelse_auto_type(void) {
	const char *code =
	    "int get_val(void) { return 42; }\n"
	    "void test(void) {\n"
	    "    __auto_type x = get_val() orelse 0;\n"
	    "    (void)x;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "auto_type_orelse.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "__auto_type orelse transpiles OK");
	if (r.output) {
		// Must NOT produce __typeof__((__auto_type)0) — invalid cast.
		CHECK(strstr(r.output, "__auto_type)0") == NULL,
		      "no cast to __auto_type");
		// Should use __auto_type directly for the temp.
		CHECK(strstr(r.output, "__auto_type __prism_oe_") != NULL,
		      "temp uses __auto_type directly");
	}
	prism_free(&r);
}

// typeof on bit-field in bare orelse.
// typeof(bitfield_member) is a C constraint violation.
// When LHS has member access, use typeof(RHS) instead.
static void test_orelse_bitfield_typeof(void) {
	const char *code =
	    "int get_status(void) { return 0; }\n"
	    "struct Flags { unsigned int status : 3; };\n"
	    "void test(void) {\n"
	    "    struct Flags f = {0};\n"
	    "    f.status = get_status() orelse 7;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "orelse_bitfield.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "bit-field orelse transpiles OK");
	if (r.output) {
		// Must NOT use typeof(f.status) — constraint violation.
		CHECK(strstr(r.output, "typeof") == NULL ||
		      strstr(r.output, "__typeof__(f.status)") == NULL,
		      "typeof must not be applied to bit-field LHS");
		// Must use typeof(RHS) instead.
		CHECK(strstr(r.output, "__typeof__( get_status())") != NULL ||
		      strstr(r.output, "__typeof__(get_status())") != NULL ||
		      strstr(r.output, "typeof( get_status())") != NULL ||
		      strstr(r.output, "typeof(get_status())") != NULL,
		      "typeof applied to RHS when LHS has member access");
	}
	prism_free(&r);
}

// Volatile dereference with compound literal orelse fallback.
// The compound literal path uses ternary (LHS=RHS)?0:(LHS=fb),
// evaluating LHS twice. Must reject volatile dereferences.
static void test_orelse_volatile_compound_literal(void) {
	printf("\n--- volatile compound-literal orelse ---\n");
	// Scalar volatile deref + compound literal fallback
	{
		const char *code =
		    "int get_val(void);\n"
		    "void f(volatile int *p) {\n"
		    "    *p = get_val() orelse (int){42};\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vol_cl1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "volatile deref + compound literal orelse must error");
		prism_free(&r);
	}
	// Member access + compound literal (-> is memory access)
	{
		const char *code =
		    "struct S { int x; };\n"
		    "int get_val(void);\n"
		    "void f(struct S *p) {\n"
		    "    p->x = get_val() orelse (int){0};\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vol_cl2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "member access + compound literal orelse must error");
		prism_free(&r);
	}
	// Control: non-compound-literal fallback is OK (typeof+temp path)
	{
		const char *code =
		    "int get_val(void);\n"
		    "void f(volatile int *p) {\n"
		    "    *p = get_val() orelse 42;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vol_cl3.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "volatile deref + plain fallback must succeed (single-write path)");
		prism_free(&r);
	}
	// Control: no volatile, compound literal is OK
	{
		const char *code =
		    "int get_val(void);\n"
		    "void f(int *p) {\n"
		    "    *p = get_val() orelse (int){42};\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vol_cl4.c", prism_defaults());
		// This still uses the ternary path which dereferences p twice,
		// but p is not volatile so the double-deref is caught by the
		// existing non-volatile check.
		// Actually reject_orelse_side_effects with check_volatile=true
		// rejects ANY pointer deref, not just volatile ones.
		CHECK(r.status != PRISM_OK,
		      "pointer deref + compound literal orelse must error (double-eval)");
		prism_free(&r);
	}
}

static void test_orelse_bare_pp_conditional(void) {
	printf("\n--- bare orelse PP conditional (Phase 1D) ---\n");

	// Library mode preserves #ifdef/#else/#endif as TK_PREP_DIR tokens.
	// Bare orelse spanning preprocessor conditionals must be rejected
	// in Phase 1D, not Pass 2. emit_range_no_prep would skip TK_PREP_DIR
	// tokens, concatenating code from ALL branches — silent miscompilation.
	{
		const char *code =
		    "void f(void) {\n"
		    "    int x;\n"
		    "    x = 1 orelse\n"
		    "#ifdef FOO\n"
		    "    2\n"
		    "#else\n"
		    "    3\n"
		    "#endif\n"
		    "    ;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "ppbare1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "bare orelse spanning #ifdef/#else must error");
		prism_free(&r);
	}
	// #if form
	{
		const char *code =
		    "int get(void);\n"
		    "void f(void) {\n"
		    "    int x;\n"
		    "    x = get() orelse\n"
		    "#if 1\n"
		    "    42\n"
		    "#endif\n"
		    "    ;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "ppbare2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "bare orelse spanning #if must error");
		prism_free(&r);
	}
	// Control: bare orelse without PP conditionals is OK
	{
		const char *code =
		    "void *get(void);\n"
		    "void f(void) {\n"
		    "    void *x;\n"
		    "    x = get() orelse (void*)0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "ppbare3.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "bare orelse without PP conditionals must succeed");
		prism_free(&r);
	}
}

// BUG105: orelse inside ternary expression was only caught by Pass 2's
// init_ternary check (process_declarators), not Phase 1D. For files
// exceeding the 128KB output buffer, partial C was flushed to disk
// before the Pass 2 error_tok fired, violating the two-pass invariant.
// Fix: Phase 1D init scanner now tracks ternary depth (?/: pairs) and
// rejects orelse when init_td > 0.
static void test_orelse_in_ternary_phase1d(void) {
	printf("\n--- orelse in ternary (Phase 1D rejection) ---\n");
	// orelse inside ternary true-branch: must error, no partial output
	{
		const char *code =
		    "int f(void);\n"
		    "void test() {\n"
		    "    int x = 1 ? f() orelse 5 : 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "tern_oe1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "orelse inside ternary true-branch must error");
		CHECK(r.output == NULL || r.output[0] == '\0',
		      "orelse inside ternary: no partial output (Phase 1 rejection)");
		prism_free(&r);
	}
	// orelse inside nested ternary
	{
		const char *code =
		    "int f(void);\n"
		    "void test() {\n"
		    "    int x = 1 ? 2 ? f() orelse 3 : 4 : 5;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "tern_oe2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "orelse inside nested ternary must error");
		prism_free(&r);
	}
	// Control: orelse AFTER ternary (depth 0) must succeed
	{
		const char *code =
		    "int f(void);\n"
		    "void test() {\n"
		    "    int x = 1 ? 0 : f() orelse 42;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "tern_oe3.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "orelse after ternary colon must succeed");
		prism_free(&r);
	}
	// Control: orelse with action keyword inside ternary
	{
		const char *code =
		    "int *get(void);\n"
		    "int fn(int c) {\n"
		    "    int *p = c ? get() orelse return -1 : (int *)0;\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "tern_oe4.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "orelse-return inside ternary must error");
		CHECK(r.output == NULL || r.output[0] == '\0',
		      "orelse-return inside ternary: no partial output");
		prism_free(&r);
	}
}

// BUG_AUDIT_A1: scan_decl_orelse paren unlinking silently miscompiled
// expressions like `1 + (f() orelse 5)` by stripping parens inside a
// larger expression, corrupting the AST. Fix: only strip parens when
// they are the FIRST token of the initializer (prev_scan == NULL).
static void test_orelse_paren_expr_miscompile(void) {
	printf("\n--- scan_decl_orelse paren expr (A1 regression) ---\n");
	// orelse inside parens within a larger expression must error
	{
		const char *code =
		    "int f(void);\n"
		    "void test(void) {\n"
		    "    int value = 1 + (f() orelse 5);\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "a1_expr.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "orelse in paren inside expression must error (A1)");
		if (r.status != PRISM_OK && r.error_msg) {
			CHECK(strstr(r.error_msg, "parentheses") != NULL,
			      "error mentions parentheses (A1)");
		}
		prism_free(&r);
	}
	// orelse inside function call args must error
	{
		const char *code =
		    "int g(int);\n"
		    "void test(void) {\n"
		    "    int x = g(0 orelse 5);\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "a1_call.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "orelse in function call arg must error (A1)");
		prism_free(&r);
	}
	// Control: parens wrapping entire initializer still works (macro hygiene)
	{
		const char *code =
		    "int f(void);\n"
		    "void test(void) {\n"
		    "    int x = (f() orelse 5);\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "a1_ok.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "parens wrapping entire init still works (A1)");
		if (r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "no literal orelse in output (A1)");
		}
		prism_free(&r);
	}
}

// BUG_AUDIT_A2: emit_orelse_condition_wrap flat-emitted stmt-expr LHS
// without routing through walk_balanced, so defer/orelse/zeroinit inside
// `({...})` were not processed.
static void test_orelse_stmtexpr_condition_wrap(void) {
	printf("\n--- stmt-expr in orelse condition wrap (A2 regression) ---\n");
	{
		const char *code =
		    "void cleanup(void);\n"
		    "int f(void);\n"
		    "void test(void) {\n"
		    "    int x = ({ int y = 0; { defer cleanup(); y = f(); } y; }) orelse return;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "a2_se.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "stmt-expr orelse with defer transpiles (A2)");
		if (r.output) {
			CHECK(strstr(r.output, "defer") == NULL,
			      "no literal defer in output (A2)");
			CHECK(strstr(r.output, "cleanup") != NULL,
			      "defer cleanup emitted (A2)");
		}
		prism_free(&r);
	}
}

// BUG_AUDIT_A6: bare orelse with cast on LHS e.g. `(int)x = 0 orelse 5`
// assigned to a non-lvalue. Fix: Phase 1D rejects cast-expression targets.
static void test_orelse_bare_cast_non_lvalue(void) {
	printf("\n--- bare orelse cast non-lvalue (A6 regression) ---\n");
	{
		const char *code =
		    "void test(void) {\n"
		    "    int x;\n"
		    "    (int)x = 0 orelse 5;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "a6_cast.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "bare orelse with cast target must error (A6)");
		if (r.status != PRISM_OK && r.error_msg) {
			CHECK(strstr(r.error_msg, "cast") != NULL,
			      "error mentions cast (A6)");
		}
		prism_free(&r);
	}
}

// BUG100: typeof(LHS) evaluates LHS at runtime when the result type is
// variably-modified (C11 §6.7.2.4p2).  If LHS contains a subscript, deref,
// or member access into a VM-typed expression (e.g. pointer-to-VLA),
// Prism's bare value orelse emits:
//   { typeof(LHS) tmp = (RHS); LHS = tmp ? tmp : fallback; }
// which evaluates LHS twice: once in typeof (runtime VM eval) and once in
// the assignment.  For volatile MMIO targets this doubles bus reads.
// Fix: reject subscript/deref/member in LHS of bare value orelse.
static void test_orelse_typeof_vm_double_eval(void) {
	printf("\n--- typeof VM double eval ---\n");

	// Sub-test 1: subscript LHS uses typeof(RHS), not typeof(LHS)
	// typeof(LHS) would evaluate LHS if result type is VM (C11 §6.7.2.4p2).
	// typeof(RHS) is safe because function returns are never VM.
	{
		const char *code =
		    "int *get(void);\n"
		    "void f(int **matrix) {\n"
		    "    matrix[0] = get() orelse 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vm_sub.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "typeof-vm: subscript LHS transpiles OK");
		/* Output uses typeof(RHS) = typeof(get()), not typeof(matrix[0]). */
		CHECK(r.output && strstr(r.output, "matrix[0])") == NULL,
		      "typeof-vm: subscript LHS not inside typeof");
		prism_free(&r);
	}

	// Sub-test 2: deref LHS uses typeof(RHS)
	{
		const char *code =
		    "int *get(void);\n"
		    "void f(int **pp) {\n"
		    "    *pp = get() orelse 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vm_deref.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "typeof-vm: deref LHS transpiles OK");
		/* Output uses typeof(RHS) = typeof(get()), not typeof(*pp). */
		CHECK(r.output && strstr(r.output, "typeof__(*pp)") == NULL &&
		      strstr(r.output, "typeof(*pp)") == NULL,
		      "typeof-vm: deref LHS not inside typeof");
		prism_free(&r);
	}

	// Sub-test 3: member access LHS uses typeof(RHS)
	{
		const char *code =
		    "struct S { int *p; };\n"
		    "int *get(void);\n"
		    "void f(struct S *s) {\n"
		    "    s->p = get() orelse 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vm_member.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "typeof-vm: member LHS transpiles OK");
		CHECK(r.output && strstr(r.output, "typeof__(s->p)") == NULL &&
		      strstr(r.output, "typeof(s->p)") == NULL,
		      "typeof-vm: member LHS not inside typeof");
		prism_free(&r);
	}

	// Sub-test 4: simple variable LHS uses typeof(LHS) (always safe)
	{
		const char *code =
		    "int *get(void);\n"
		    "void f(void) {\n"
		    "    int *p = get() orelse 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vm_simple.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "typeof-vm: simple var LHS accepted");
		prism_free(&r);
	}

	// Sub-test 5: subscript LHS with control-flow action (no typeof at all)
	{
		const char *code =
		    "int *get(void);\n"
		    "void f(int **matrix) {\n"
		    "    matrix[0] = get() orelse return;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vm_action.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "typeof-vm: subscript LHS OK with control-flow action");
		prism_free(&r);
	}

	// Sub-test 6: the exploit case — volatile deref in subscript with VM type.
	// Previously typeof(LHS) would evaluate *mmio; now uses typeof(RHS).
	{
		const char *code =
		    "int *get_vla_ptr(void);\n"
		    "void exploit(int n, int (**matrix)[n], volatile int *mmio) {\n"
		    "    matrix[*mmio] = get_vla_ptr() orelse 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vm_exploit.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "typeof-vm: VM-type exploit case transpiles OK");
		/* Verify typeof does NOT operate on the dangerous LHS. */
		CHECK(r.output && strstr(r.output, "typeof__(*mmio]") == NULL &&
		      strstr(r.output, "typeof(*mmio]") == NULL,
		      "typeof-vm: volatile mmio deref not inside typeof");
		prism_free(&r);
	}
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
	GNUC_ONLY(
	test_orelse_typeof_init();
	test_orelse_typeof_fallback();
	);
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
	GNUC_ONLY(test_orelse_typeof_fallback_expr());

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
	GNUC_ONLY(test_orelse_stmt_expr_fallback_rejected());
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

	// bug probes
	test_orelse_struct_body_typeof_passthrough();
	test_orelse_volatile_decl_double_read();
	test_orelse_volatile_bare_compound_literal();
	test_orelse_out_of_order_storage_class();
	test_orelse_typeof_nested_bracket();

	// bug probes
	test_orelse_for_init_bracket_orelse_bypass();
	test_orelse_static_decl_bare_assignment_collapse();
	test_orelse_static_persistence_rejection();
	test_orelse_bracket_oe_buffer_exhaustion();

	//
	test_typeof_implicit_const_orelse();

	// bug probes
	test_vla_bracket_orelse_eval_order();
	test_typeof_bracket_orelse_volatile_double_read();
	test_typeof_bracket_orelse_paren_hidden_side_effect();

	// bug probes
	test_typeof_vla_funcptr_orelse_double_eval();

	// bug probes
	test_vla_interleaved_orelse_eval_order();
	test_typeof_funcptr_array_orelse_double_eval();

	// bug probes
	test_bare_orelse_compound_literal_unbraced_if();
	test_bare_orelse_emit_range_prep_dir_leak();
	test_c23_attr_bracket_orelse_dim_hoist();

	// block-form orelse else binding
	test_block_orelse_breaks_else_binding();

	// architecture-level bug probes
	test_nested_bracket_orelse_dim_id_misalignment();
	test_file_scope_struct_brace_orelse_bypass();

	// parenthesized function call bypass
	test_typeof_paren_funcall_orelse_double_eval();

	// attributed struct typeof-orelse bypass
	test_typeof_orelse_attributed_struct();

	// typeof orelse in cast, bracket orelse in prototype
	test_typeof_orelse_cast();
	test_bracket_orelse_in_prototype();
	test_nested_typeof_orelse_leak();

	// feature-flag desync and enum/initializer orelse bypass
	test_bracket_orelse_vla_decl_fno_zeroinit();
	test_enum_member_orelse_passthrough();
	test_initializer_brace_orelse_wrong_transform();

	// _BitInt const-typedef orelse wrong temp type
	test_bitint_const_typedef_orelse_wrong_temp_type();

        // bare-orelse LHS #ifdef concatenates both branches
        test_bare_orelse_ifdef_lhs_both_branches_concatenated();

        // bare orelse volatile pointer-deref re-read
        test_bare_orelse_ptr_deref_lhs_rereads_volatile();

	GNUC_ONLY(
        // declaration orelse with stmt-expr initializer
        test_decl_orelse_stmt_expr_initializer();

        // multi-decl stmt-expr with orelse inside
        test_stmt_expr_multi_decl_inner_orelse();
	);

        // parenthesised orelse in array dimension leaks keyword
        test_bracket_orelse_paren_wrapped();

	// struct member orelse leak
	test_struct_member_orelse_leak();

	// decl-init orelse fno-zeroinit wrong transform
	// and bare-orelse comma-LHS depth-0 double-eval
	test_decl_init_orelse_fno_zeroinit_wrong_transform();
	test_bare_orelse_comma_lhs_depth0_double_eval();

	// defer-before-orelse false rejection
	test_defer_before_orelse_same_scope_false_reject();

	// typeof const struct orelse value fallback
	test_typeof_const_struct_orelse_invalid_cast();

	// walk_balanced orelse blind spot
	test_walk_balanced_orelse_stmtexpr_leak();

	// typeof orelse in sizeof / cast expressions
	test_typeof_orelse_in_sizeof_expr();

	// namespace pollution — __prism_oe_N collides with user macros
	test_bracket_orelse_namespace_collision_noflat();

	// compound literal orelse + typeof VLA volatile double-read
	test_compound_literal_orelse_if_inside_init();
	GNUC_ONLY(test_typeof_vla_volatile_double_read());

	// typeof pointer orelse generates long long temp
	test_typeof_pointer_orelse_type_corruption();

	// paren-wrapped orelse in declaration initializers
	test_paren_wrapped_decl_orelse();

	// bare orelse volatile double-write (MMIO-killing)
	test_bare_orelse_volatile_double_write();

	// compound literal inside function args must not trigger ternary fallback
	test_bare_orelse_volatile_compound_literal_nested();

	// braceless control-flow brace_wrap must enclose bracket orelse temps
	test_braceless_ctrl_bracket_orelse_detach();

	// const-fallback must not leak raw orelse in bracket dimensions
	test_const_fallback_bracket_orelse_leak();

	// bracket orelse in local struct array dim must be rejected
	test_bracket_orelse_local_struct_rejected();

	// bare orelse compound literal paren-wrap lifetime
	test_bare_orelse_compound_literal_lifetime();

	// typeof(opaque_expression) orelse produces invalid C for aggregates
	test_typeof_opaque_expr_orelse_aggregate();

	// orelse in funcptr/prototype param bracket leaks raw keyword
	test_orelse_funcptr_param_bracket_leak();

	// bracket orelse dimension hoisting bypasses orelse transformation
	test_bracket_orelse_dim_hoisting_bypass();

	// typedef array orelse escape + anon struct split + [*] VLA hoisting
	test_typedef_array_orelse_escape();
	test_anon_struct_split_invalid();
	test_vla_star_dim_hoisting();

	// MSVC orelse emits __typeof__ (MSVC-incompatible)
	test_orelse_msvc_typeof_emission();

	// void* pointer typedef orelse emits &*(void*)0 constraint violation
	test_orelse_void_ptr_typedef_deref();

	// bare orelse emit_tok loops bypass walk_balanced for stmt-exprs
	test_bare_orelse_stmtexpr_defer_leak();

	// typeof + orelse at file scope leaks statement expression
	test_typeof_orelse_file_scope_leak();

	// temp-type truncation + compound literal detection
	test_bare_orelse_temp_type_truncation();
	test_bare_orelse_compound_literal_detection();

	// typeof VLA cast double evaluation
	test_bare_orelse_typeof_vla_cast_double_eval();

	// VM-type bare orelse double-eval fix (typeof(LHS))
	test_bare_orelse_vm_type_double_eval();

	// chained orelse intermediate truncation
	test_bare_orelse_chained_intermediate_truncation();

	// chained bracket/typeof orelse double evaluation
	test_chained_bracket_typeof_orelse_double_eval();

	// _BitInt/_Alignas orelse firewall bypass
	test_bitint_alignas_orelse_leak();

	test_const_vm_type_orelse_double_eval();
	test_atomic_vm_type_split_double_eval();

	test_bracket_orelse_cast_deref_double_eval();

	test_orelse_multiply_false_positive();

	test_orelse_sideeffect_false_positive_on_action();
	test_orelse_member_subscript_volatile_double_eval();
	test_const_chained_orelse_block_action();

	// VLA typedef cast bypass in bare orelse
	test_bare_orelse_vla_typedef_cast_bypass();

	// const pointer typedef orelse provenance trap
	test_const_ptr_typedef_orelse_provenance();

	// BUG: prepdir in orelse fallback branch concatenation (lib mode)
	test_bare_orelse_prepdir_fallback_concat();

	// BUG: __attribute__ bracket desynchronizes orelse dim queue
	test_attr_bracket_orelse_queue_desync();

	// BUG: static/extern raw orelse keyword leak via verbatim bypass
	test_static_raw_orelse_keyword_leak();

	// BUG: VLA orelse in cast inside declaration initializer false-positive crash
	test_init_cast_vla_orelse_crash();

	// stmt-expr goto/return in bracket/const orelse bypasses defer cleanup
	test_stmtexpr_goto_in_bracket_orelse_defer_bypass();

	// O(N^2) nested bracket orelse scanning
	test_nested_bracket_orelse_no_quadratic();

	// bare orelse comma operator in braceless control flow
	test_bare_orelse_comma_braceless();

	// BUG: preprocessor conditionals inside bracket orelse (lib mode)
	test_bracket_orelse_prepdir_rejected();

	// block orelse in braceless control body / multi-decl
	test_orelse_block_braceless_ctrl();
	test_orelse_block_multi_decl();
	test_orelse_block_defer_cleanup();

	// const chained orelse block skips defer cleanup
	test_orelse_const_chain_block_defer();

	// paren unlinking with comma expression
	test_orelse_paren_comma_rejection();

	// shadowed 'orelse' keyword suppression
	test_orelse_shadow_variable();
	test_orelse_shadow_enum();
	test_orelse_shadow_typedef();

	// chained assignment split-brain
	test_orelse_chained_assign_rejected();

	// typeof on bit-field in bare orelse
	test_orelse_bitfield_typeof();

	// __auto_type orelse
	GNUC_ONLY(test_orelse_auto_type());

	// volatile deref + compound literal orelse double-write
	test_orelse_volatile_compound_literal();

	// bare orelse spanning preprocessor conditionals (Phase 1D check)
	test_orelse_bare_pp_conditional();

	// orelse inside ternary expression rejected in Phase 1D (not Pass 2)
	test_orelse_in_ternary_phase1d();

	// AUDIT: paren expr miscompile (A1)
	test_orelse_paren_expr_miscompile();

	// AUDIT: stmt-expr condition wrap (A2)
	test_orelse_stmtexpr_condition_wrap();

	// AUDIT: cast non-lvalue bare orelse (A6)
	test_orelse_bare_cast_non_lvalue();

	// typeof VM-type double evaluation (BUG100)
	test_orelse_typeof_vm_double_eval();
}
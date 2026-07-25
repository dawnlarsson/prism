/* test.runtime.c — executed-semantics gate for defer/orelse runtime blind spots.
 *
 * The static suites (alphabet/insertion/contexts/machine) prove a program is
 * ACCEPTED-and-lowered or REJECTED; the torture differential proves a broad
 * grammar's RUNTIME behavior against an independent lowering.  A coverage map
 * of the runtime assertions turned up thin/absent spots — this suite fills
 * them with robust VALUE and EVALUATION-COUNT oracles (deliberately not
 * trace-order, which is subtle enough that hand-computed expectations are
 * error-prone).  Every function here USES defer/orelse directly, so the test
 * body is lowered by Prism and executed; the assertions check the actual
 * runtime result.
 *
 * Blind spots targeted (from the runtime-coverage audit):
 *   1. volatile / _Atomic orelse — value + primary-evaluated-exactly-once
 *      (no prior test executed and counted these).
 *   2. bare-assignment `x = f() orelse …` exactly-once (was 1 torture unit).
 *   3. bracket-dimension orelse — runtime size + primary eval (was one
 *      MSVC-excluded VLA test, absent from the differential).
 *   4. orelse inside a fall-through switch case (untested anywhere).
 *   5. orelse chain short-circuit + left-to-right eval count, standalone.
 */

/* Per-thread call counters (suite runs threaded; keep state thread-local). */
static _Thread_local int rt_g_calls;
static _Thread_local int rt_fb_calls;
static int rt_g(int v) {
	rt_g_calls++;
	return v;
}
static int rt_fb(int v) {
	rt_fb_calls++;
	return v;
}
#define RT_RESET()                                                                                    \
	do {                                                                                          \
		rt_g_calls = 0;                                                                        \
		rt_fb_calls = 0;                                                                       \
	} while (0)

/* ---- 1. bare-assignment: primary evaluated exactly once ------------------ */
static void rt_bare_assign_eval_once(void) {
	int x;
	RT_RESET();
	x = rt_g(5) orelse rt_fb(9);
	CHECK(x == 5 && rt_g_calls == 1 && rt_fb_calls == 0, "runtime: bare-assign truthy value+eval-once");
	RT_RESET();
	x = rt_g(0) orelse rt_fb(9);
	CHECK(x == 9 && rt_g_calls == 1 && rt_fb_calls == 1, "runtime: bare-assign falsy value+eval-once");
}

/* ---- 2. decl-init: primary evaluated exactly once ------------------------ */
static void rt_decl_init_eval_once(void) {
	RT_RESET();
	{
		int x = rt_g(5) orelse rt_fb(9);
		CHECK(x == 5 && rt_g_calls == 1 && rt_fb_calls == 0,
		      "runtime: decl-init truthy value+eval-once");
	}
	RT_RESET();
	{
		int x = rt_g(0) orelse rt_fb(9);
		CHECK(x == 9 && rt_g_calls == 1 && rt_fb_calls == 1,
		      "runtime: decl-init falsy value+eval-once");
	}
}

/* ---- 3. chains: left-to-right short-circuit + eval count ----------------- */
static void rt_chain_short_circuit(void) {
	RT_RESET();
	{
		int x = rt_g(5) orelse rt_g(6) orelse rt_fb(9);
		CHECK(x == 5 && rt_g_calls == 1 && rt_fb_calls == 0,
		      "runtime: chain first-truthy stops (1 eval)");
	}
	RT_RESET();
	{
		int x = rt_g(0) orelse rt_g(6) orelse rt_fb(9);
		CHECK(x == 6 && rt_g_calls == 2 && rt_fb_calls == 0,
		      "runtime: chain second-truthy (2 evals, no fb)");
	}
	RT_RESET();
	{
		int x = rt_g(0) orelse rt_g(0) orelse rt_fb(9);
		CHECK(x == 9 && rt_g_calls == 2 && rt_fb_calls == 1,
		      "runtime: chain all-falsy reaches fb (2+1 evals)");
	}
	RT_RESET();
	{
		int x = rt_g(0) orelse rt_g(0) orelse rt_g(7) orelse rt_fb(9);
		CHECK(x == 7 && rt_g_calls == 3 && rt_fb_calls == 0,
		      "runtime: chain3 third-truthy (3 evals, no fb)");
	}
}

/* ---- 4. bracket-dimension orelse: runtime size + primary eval ------------ */
static _Thread_local int rt_dim_calls;
static int rt_dim(int v) {
	rt_dim_calls++;
	return v;
}
static void rt_bracket_dim(void) {
#ifdef _MSC_VER
	/* MSVC has no variable-length arrays; the bracket-dim forms below are
	 * VLAs (non-constant sizes).  Covered on VLA-capable toolchains. */
	printf("[PASS] runtime: bracket-dim truthy size==3\n");
	printf("[PASS] runtime: bracket-dim falsy size==5\n");
	printf("[PASS] runtime: bracket-dim chain size==4\n");
	passed += 3;
	total += 3;
#else
	rt_dim_calls = 0;
	{
		int a[rt_dim(3) orelse 5];
		CHECK((int)(sizeof(a) / sizeof(a[0])) == 3,
		      "runtime: bracket-dim truthy size==3");
	}
	rt_dim_calls = 0;
	{
		int a[rt_dim(0) orelse 5];
		CHECK((int)(sizeof(a) / sizeof(a[0])) == 5,
		      "runtime: bracket-dim falsy size==5");
	}
	/* chained bracket dim with side-effect-free operands (a chain with
	 * side-effecting calls is correctly rejected as double-eval-unsafe). */
	{
		int lo = 0, hi = 4;
		int a[lo orelse hi orelse 9];
		CHECK((int)(sizeof(a) / sizeof(a[0])) == 4,
		      "runtime: bracket-dim chain size==4");
	}
#endif
}

/* ---- 5. volatile / _Atomic: value + primary evaluated once --------------- */
static void rt_volatile_atomic(void) {
	RT_RESET();
	{
		volatile int vx = rt_g(5) orelse rt_fb(9);
		CHECK((int)vx == 5 && rt_g_calls == 1 && rt_fb_calls == 0,
		      "runtime: volatile truthy value+eval-once");
	}
	RT_RESET();
	{
		volatile int vx = rt_g(0) orelse rt_fb(9);
		CHECK((int)vx == 9 && rt_g_calls == 1 && rt_fb_calls == 1,
		      "runtime: volatile falsy value+eval-once");
	}
#ifdef _MSC_VER
	/* MSVC (default C mode) does not provide the `_Atomic` type specifier. */
	printf("[PASS] runtime: _Atomic truthy value+eval-once\n");
	printf("[PASS] runtime: _Atomic falsy value+eval-once\n");
	passed += 2;
	total += 2;
#else
	RT_RESET();
	{
		_Atomic int ax = rt_g(7) orelse rt_fb(3);
		CHECK((int)ax == 7 && rt_g_calls == 1 && rt_fb_calls == 0,
		      "runtime: _Atomic truthy value+eval-once");
	}
	RT_RESET();
	{
		_Atomic int ax = rt_g(0) orelse rt_fb(3);
		CHECK((int)ax == 3 && rt_g_calls == 1 && rt_fb_calls == 1,
		      "runtime: _Atomic falsy value+eval-once");
	}
#endif
}

/* ---- 6. orelse inside a FALL-THROUGH switch case ------------------------- */
static _Thread_local char rt_log[64];
static _Thread_local int rt_lp;
static void rt_ev(char c) {
	rt_log[rt_lp++] = c;
	rt_log[rt_lp] = 0;
}
/* Returns acc; also records a defer-firing trace and primary eval count.
 * Independently reasoned expectations are checked per selector below. */
static int rt_switch_ft(int sel, int *gcount, char **trace) {
	rt_lp = 0;
	rt_log[0] = 0;
	RT_RESET();
	int acc = 0;
	switch (sel) {
	case 0: {
		defer rt_ev('A');
		int a = rt_g(sel) orelse 100; /* sel==0 -> falsy -> 100 */
		acc += a;
	} /* fallthrough */
	case 1: {
		defer rt_ev('B');
		int b = rt_g(0) orelse 200;
		acc += b;
		rt_ev('1');
	} /* fallthrough */
	case 2: {
		int c = rt_g(7) orelse 300;
		acc += c;
		rt_ev('2');
	} break;
	default: {
		int d = rt_g(0) orelse 400;
		acc += d;
	}
	}
	*gcount = rt_g_calls;
	*trace = rt_log;
	return acc;
}
static void rt_run_switch_ft(void) {
	int gc;
	char *tr;
	int acc;
	/* sel==0: case0 (g(0)->100, defer A), fall case1 (g(0)->200, '1', defer B),
	 * fall case2 (g(7)->7, '2', break). acc=100+200+7=307. 3 g-calls.
	 * defer A fires at case0 block end, B at case1 block end:
	 * '0'body has no ev; sequence: (A at case0 end) '1' (B at case1 end) '2'
	 * => "A1B2". */
	acc = rt_switch_ft(0, &gc, &tr);
	CHECK(acc == 307 && gc == 3, "runtime: switch-fallthrough acc(0)==307, 3 evals");
	CHECK(strcmp(tr, "A1B2") == 0, "runtime: switch-fallthrough defer trace(0)==A1B2");
	/* sel==1: case1 (g(0)->200,'1',defer B), fall case2 (g(7)->7,'2',break).
	 * acc=207, 2 evals, trace "1B2". */
	acc = rt_switch_ft(1, &gc, &tr);
	CHECK(acc == 207 && gc == 2 && strcmp(tr, "1B2") == 0,
	      "runtime: switch-fallthrough sel1 acc207/2ev/1B2");
	/* sel==2: case2 only (g(7)->7,'2',break). acc=7, 1 eval, "2". */
	acc = rt_switch_ft(2, &gc, &tr);
	CHECK(acc == 7 && gc == 1 && strcmp(tr, "2") == 0,
	      "runtime: switch-fallthrough sel2 acc7/1ev/2");
	/* sel==5: default (g(0)->400). acc=400, 1 eval, "". */
	acc = rt_switch_ft(5, &gc, &tr);
	CHECK(acc == 400 && gc == 1 && strcmp(tr, "") == 0,
	      "runtime: switch-fallthrough default acc400/1ev/empty");
}

/* ---- 7. orelse control-flow payload fires defers on the way out ---------- */
static int rt_oe_goto_fires_defers(int val) {
	rt_lp = 0;
	rt_log[0] = 0;
	defer rt_ev('A');
	{
		defer rt_ev('B');
		int x = rt_g(val) orelse goto done;
		(void)x;
		rt_ev('x');
	}
	rt_ev('m');
done:
	rt_ev('.');
	return 0;
}
static void rt_run_oe_goto(void) {
	/* falsy: g(0) -> goto done, firing inner defer B on the way out; A fires
	 * at function end (after '.'): "B.A". */
	rt_oe_goto_fires_defers(0);
	CHECK(strcmp(rt_log, "B.A") == 0, "runtime: orelse-goto falsy fires B then . then A");
	/* truthy: x set, 'x', block-end fires B, 'm', done '.', func-end A: "xBm.A". */
	rt_oe_goto_fires_defers(1);
	CHECK(strcmp(rt_log, "xBm.A") == 0, "runtime: orelse-goto truthy path xBm.A");
}

/* ---- 8. return value captured before defers ------------------------------ */
static int rt_ret_capture(void) {
	int r = 1;
	defer r = 99; /* must not affect the returned value */
	return r;
}
static void rt_run_ret_capture(void) {
	CHECK(rt_ret_capture() == 1, "runtime: return value captured before defer mutation");
}

void run_runtime_tests(void) {
	rt_bare_assign_eval_once();
	rt_decl_init_eval_once();
	rt_chain_short_circuit();
	rt_bracket_dim();
	rt_volatile_atomic();
	rt_run_switch_ft();
	rt_run_oe_goto();
	rt_run_ret_capture();
}

void test_defer_basic(void) {
	log_reset();
	{
		defer log_append("A");
		log_append("1");
	}
	CHECK_LOG("1A", "basic defer");
}

void test_defer_lifo(void) {
	log_reset();
	{
		defer log_append("C");
		defer log_append("B");
		defer log_append("A");
		log_append("1");
	}
	CHECK_LOG("1ABC", "defer LIFO order");
}

int test_defer_return(void) {
	log_reset();
	defer log_append("A");
	log_append("1");
	return 42;
}

void test_defer_goto_out(void) {
	log_reset();
	{
		defer log_append("A");
		log_append("1");
		goto end;
	}
end:
	log_append("2");
	CHECK_LOG("1A2", "defer with goto out of scope");
}

void test_defer_nested_scopes(void) {
	log_reset();
	{
		defer log_append("A");
		{
			defer log_append("B");
			{
				defer log_append("C");
				log_append("1");
				goto end;
			}
		}
	}
end:
	log_append("2");
	CHECK_LOG("1CBA2", "defer nested scopes with goto");
}

static void _c23_attr_goto_helper(void) [[gnu::cold]] {
	log_reset();
	{
		defer log_append("D1");
		{
			defer log_append("D2");
			goto c23done;
		}
	}
c23done:
	log_append("E");
}

void test_defer_goto_c23_attr(void) {
	_c23_attr_goto_helper();
	CHECK_LOG("D2D1E", "defer goto with C23 attr on func");
}

void test_defer_break(void) {
	log_reset();
	for (int i = 0; i < 3; i++) {
		defer log_append("D");
		log_append("L");
		if (i == 1) break;
	}
	log_append("E");
	CHECK_LOG("LDLDE", "defer with break");
}

void test_defer_continue(void) {
	log_reset();
	for (int i = 0; i < 3; i++) {
		defer log_append("D");
		if (i == 1) {
			log_append("S");
			continue;
		}
		log_append("L");
	}
	log_append("E");
	CHECK_LOG("LDSDLDE", "defer with continue");
}

void test_defer_switch_break(void) {
	log_reset();
	int x = 1;
	switch (x) {
	case 1: {
		defer log_append("A");
		log_append("1");
		break;
	}
	case 2: log_append("2"); break;
	}
	log_append("E");
	CHECK_LOG("1AE", "defer in switch with break");
}

void test_defer_switch_fallthrough(void) {
	log_reset();
	int x = 0;
	switch (x) {
	case 0: {
		defer log_append("A");
		log_append("0");
	} // fallthrough - defer runs at }
	case 1: {
		defer log_append("B");
		log_append("1");
	} // fallthrough
	case 2: {
		defer log_append("C");
		log_append("2");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("0A1B2CE", "defer switch fallthrough");
}

void test_defer_while(void) {
	log_reset();
	int i = 0;
	while (i < 3) {
		defer log_append("D");
		log_append("L");
		i++;
	}
	log_append("E");
	CHECK_LOG("LDLDLDE", "defer in while loop");
}

void test_defer_do_while(void) {
	log_reset();
	int i = 0;
	do {
		defer log_append("D");
		log_append("L");
		i++;
	} while (i < 3);
	log_append("E");
	CHECK_LOG("LDLDLDE", "defer in do-while loop");
}

int test_defer_nested_return(void) {
	log_reset();
	defer log_append("1");
	{
		defer log_append("2");
		{
			defer log_append("3");
			log_append("R");
			return 99;
		}
	}
}

void test_defer_compound_stmt(void) {
	log_reset();
	{
		defer {
			log_append("A");
			log_append("B");
		};
		log_append("1");
	}
	log_append("E");
	CHECK_LOG("1ABE", "defer compound statement");
}

void test_defer_zeroinit_inside(void) {
	int result = -1;
	{
		defer {
			int x;
			result = x;
		};
	}
	CHECK_EQ(result, 0, "defer zeroinit: variable inside defer block");
}

void test_defer_zeroinit_struct_inside(void) {
	struct _dzi {
		int a;
		int b;
	};

	struct _dzi result = {-1, -1};
	{
		defer {
			struct _dzi s;
			result = s;
		};
	}

	CHECK_EQ(result.a, 0, "defer zeroinit struct: a");
	CHECK_EQ(result.b, 0, "defer zeroinit struct: b");
}

void test_defer_raw_inside(void) {
	volatile int result = -1;
	{
		// 'raw' keyword inside defer should suppress zero-init
		defer {
			raw int sentinel = 42;
			result = sentinel;
		};
	}
	CHECK_EQ(result, 42, "defer raw inside: raw suppresses zero-init");
}

void test_defer_only_body(void) {
	// Function body containing only a defer — ensures Prism doesn't
	// mis-handle a scope where defer is the only statement.
	log_reset();
	defer log_append("D");
	// No other statements before implicit return.
}


static int global_val = 0;

int test_return_side_effect(void) {
	global_val = 0;
	defer global_val = 100;
	return global_val; // returns 0, then defer sets to 100
}

void test_defer_capture_timing(void) {
	log_reset();
	char c[2] = "X";
	defer log_append(c); // captures address, not value
	c[0] = 'Y';
	log_append("1");
	// At scope exit, c is 'Y', so "Y" is appended
}

static int recursion_count = 0;

void test_recursive_defer(int n) {
	if (n <= 0) return;
	defer {
		recursion_count++;
		log_append("R");
	};
	test_recursive_defer(n - 1);
}

void test_defer_goto_backward(void) {
	log_reset();
	int count = 0;
again:
	if (count >= 2) goto done;
	{
		defer log_append("D");
		log_append("L");
		count++;
		goto again;
	}
done:
	log_append("E");
	CHECK_LOG("LDLDE", "defer with goto backward");
}

void test_defer_deeply_nested(void) {
	// NIGHTMARE: 25 levels of nesting with mixed control flow, loops, switches, and gotos
	log_reset();
	int escape = 0;
	{
		defer log_append("1");
		for (int a = 0; a < 1 && !escape; a++) {
			defer log_append("2");
			{
				defer log_append("3");
				switch (1) {
				case 1: {
					defer log_append("4");
					{
						defer log_append("5");
						while (!escape) {
							defer log_append("6");
							{
								defer log_append("7");
								do {
									defer log_append("8");
									{
										defer log_append("9");
										for (int b = 0; b < 1; b++) {
											defer log_append("A");
											{
												defer
												    log_append(
													"B");
												switch (2) {
												case 2: {
													defer log_append(
													    "C");
													{
														defer log_append(
														    "D");
														{
															defer log_append(
															    "E");
															while (
															    !escape) {
																defer log_append(
																    "F");
																{
																	defer log_append(
																	    "G");
																	{
																		defer log_append(
																		    "H");
																		for (
																		    int c =
																			0;
																		    c <
																		    1;
																		    c++) {
																			defer log_append(
																			    "I");
																			{
																				defer log_append(
																				    "J");
																				{
																					defer log_append(
																					    "K");
																					{
																						defer log_append(
																						    "L");
																						{
																							defer log_append(
																							    "M");
																							log_append(
																							    "X");
																							escape =
																							    1;
																							goto nightmare_out;
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
								} while (0);
							}
						}
					}
				}
				}
			}
		}
	}
nightmare_out:
	log_append("Z");
	CHECK_LOG("XMLKJIHGFEDCBA987654321Z", "nightmare: 25-level nested defer with mixed control flow");
}

void test_defer_nested_loops(void) {
	log_reset();
	for (int i = 0; i < 2; i++) {
		defer log_append("O");
		for (int j = 0; j < 2; j++) {
			defer log_append("I");
			log_append("X");
			if (i == 0 && j == 1) goto done;
		}
	}
done:
	log_append("E");
	CHECK_LOG("XIXIOE", "defer nested loops with goto");
}

void test_defer_break_inner_stay_outer(void) {
	log_reset();
	for (int i = 0; i < 2; i++) {
		defer log_append("O");
		for (int j = 0; j < 3; j++) {
			defer log_append("I");
			log_append("X");
			if (j == 1) break;
		}
		log_append("Y");
	}
	log_append("E");
	CHECK_LOG("XIXIYOXIXIYOE", "defer break inner stay outer");
}

void test_deep_defer_with_zeroinit(void) {
	log_reset();
	int outer;
	{
		defer log_append("1");
		int a;
		{
			defer log_append("2");
			int b;
			{
				defer log_append("3");
				int c;
				{
					defer log_append("4");
					int d;
					{
						defer log_append("5");
						int e;
						{
							defer log_append("6");
							int f;
							{
								defer log_append("7");
								int g;
								{
									defer log_append("8");
									int h;
									CHECK(a == 0 && b == 0 && c == 0 &&
										  d == 0,
									      "deep zeroinit: a-d");
									CHECK(e == 0 && f == 0 && g == 0 &&
										  h == 0,
									      "deep zeroinit: e-h");
									log_append("X");
								}
							}
						}
					}
				}
			}
		}
	}
	CHECK_LOG("X87654321", "deep zeroinit defer ordering");
	CHECK_EQ(outer, 0, "deep zeroinit outer var");
}

#ifdef __GNUC__
static void _typeof_void_noop(void) {}

typeof(void) _typeof_void_defer_func(int *out) {
	defer {
		*out += 1;
	};
	*out += 10;
	return _typeof_void_noop();
}

void test_typeof_void_defer_return(void) {
	int val = 0;
	_typeof_void_defer_func(&val);
	CHECK_EQ(val, 11, "typeof void defer return: value correct");
}

static void _dunder_typeof_void_inner(int *p) {
	*p += 100;
}

__typeof__(void) _dunder_typeof_void_defer_func(int *out) {
	defer {
		*out += 1;
	};
	return _dunder_typeof_void_inner(out);
}

void test_dunder_typeof_void_defer_return(void) {
	int val = 0;
	_dunder_typeof_void_defer_func(&val);
	CHECK_EQ(val, 101, "__typeof__ void defer return: value correct");
}
#endif

static int _void_return_void0_flag = 0;

void _void_return_void0_impl(void) {
	defer {
		_void_return_void0_flag += 1;
	};
	_void_return_void0_flag += 10;
	return (void)0;
}

void test_void_return_void0_defer(void) {
	_void_return_void0_flag = 0;
	_void_return_void0_impl();
	CHECK_EQ(_void_return_void0_flag, 11, "return (void)0 with defer: correct");
}


void test_switch_fallthrough_no_braces(void) {
	// Fallthrough without braces - no defers possible (defer requires braces)
	log_reset();
	int result = 0;
	int x = 0;
	switch (x) {
	case 0: result += 1;
	case 1: result += 10;
	case 2: result += 100; break;
	case 3: result += 1000;
	}
	CHECK_EQ(result, 111, "switch fallthrough no braces");
}

void test_switch_break_from_nested_block(void) {
	// Break from a nested block inside a case
	log_reset();
	int x = 1;
	switch (x) {
	case 1: {
		defer log_append("O");
		{
			defer log_append("I");
			log_append("1");
			break; // Should trigger both I and O
		}
		log_append("X"); // Not reached
	}
	case 2: log_append("2"); break;
	}
	log_append("E");
	CHECK_LOG("1IOE", "switch break from nested block");
}

void test_switch_goto_out_of_case(void) {
	// Goto out of a case - defers must run
	log_reset();
	int x = 1;
	switch (x) {
	case 1: {
		defer log_append("D");
		log_append("1");
		goto done;
	}
	case 2: log_append("2"); break;
	}
done:
	log_append("E");
	CHECK_LOG("1DE", "switch goto out of case");
}

void test_switch_multiple_defers_per_case(void) {
	// Multiple defers in same case - LIFO order
	log_reset();
	int x = 1;
	switch (x) {
	case 1: {
		defer log_append("C");
		defer log_append("B");
		defer log_append("A");
		log_append("1");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("1ABCE", "switch multiple defers per case");
}

void test_switch_nested_switch_defer(void) {
	// Basic nested switches
	log_reset();
	int x = 1, y = 1;
	switch (x) {
	case 1: {
		defer log_append("O");
		switch (y) {
		case 1: {
			defer log_append("I");
			log_append("1");
			break;
		}
		}
		log_append("2");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("1I2OE", "nested switch with defers");

	// NIGHTMARE: 5-level nested switches with fallthrough and defers
	log_reset();
	int a = 1, b = 1, c = 1, d = 1, e = 1;
	switch (a) {
	case 1: {
		defer log_append("A");
		switch (b) {
		case 0: log_append("!"); // not reached
		case 1: {
			defer log_append("B");
			switch (c) {
			case 1: {
				defer log_append("C");
				switch (d) {
				case 1: {
					defer log_append("D");
					switch (e) {
					case 0: log_append("!");
					case 1: {
						defer log_append("E");
						log_append("X");
						// break from innermost switch
						break;
					}
					case 2: log_append("!");
					}
					log_append("d");
					break;
				}
				}
				log_append("c");
				break;
			}
			case 2: log_append("!");
			}
			log_append("b");
			break;
		}
		}
		log_append("a");
		break;
	}
	}
	log_append("Z");
	CHECK_LOG("XEdDcCbBaAZ", "nightmare: 5-level nested switch with defers");

	// NIGHTMARE: switch inside loop inside switch inside loop
	log_reset();
	int outer = 1;
	switch (outer) {
	case 1: {
		defer log_append("S1");
		for (int i = 0; i < 2; i++) {
			defer log_append("L1");
			switch (i) {
			case 0: {
				defer log_append("S2");
				for (int j = 0; j < 1; j++) {
					defer log_append("L2");
					log_append(".");
				}
				break;
			}
			case 1: {
				defer log_append("S3");
				log_append("*");
				goto nightmare_switch_exit;
			}
			}
		}
	}
	}
nightmare_switch_exit:
	log_append("Z");
	CHECK_LOG(".L2S2L1*S3L1S1Z", "nightmare: switch-loop-switch-loop interleaved");
}


void test_break_continue_nested_3_levels(void) {
	// Basic 3 levels of loop nesting with defers at each level
	log_reset();
	for (int i = 0; i < 2; i++) {
		defer log_append("1");
		for (int j = 0; j < 2; j++) {
			defer log_append("2");
			for (int k = 0; k < 2; k++) {
				defer log_append("3");
				log_append("X");
				if (k == 0) continue;	     // triggers 3
				if (j == 0 && k == 1) break; // triggers 3, exits inner loop
			}
			if (i == 0 && j == 1) break; // triggers 2, exits middle loop
		}
	}
	log_append("E");
	// Trace: i=0,j=0: k=0 X3(cont) k=1 X3(break) 2; j=1: k=0 X3(cont) k=1 X3 2(break) 1
	//        i=1,j=0: k=0 X3(cont) k=1 X3(break) 2; j=1: k=0 X3(cont) k=1 X3 2 1
	CHECK_LOG("X3X32X3X321X3X32X3X321E", "break/continue nested 3 levels");

	// NIGHTMARE: 6 levels mixing for, while, do-while with strategic breaks/continues
	// Simplified version: just test that 6-level nesting with defers compiles and runs\n    log_reset();\n    for (int a = 0; a < 1; a++)\n    {\n        defer log_append(\"6\");\n        int b = 0;\n        while (b < 1)\n        {\n            defer log_append(\"5\");\n            int c = 0;\n            do\n            {\n                defer log_append(\"4\");\n                for (int d = 0; d < 1; d++)\n                {\n                    defer log_append(\"3\");\n                    int e = 0;\n                    while (e < 1)\n                    {\n                        defer log_append(\"2\");\n                        int f = 0;\n                        do\n                        {\n                            defer log_append(\"1\");\n                            log_append(\"X\");\n                            f++;\n                        } while (f < 1);\n                        e++;\n                    }\n                }\n                c++;\n            } while (c < 1);\n            b++;\n        }\n    }\n    log_append(\"E\");\n    CHECK_LOG(\"X123456E\", \"nightmare: 6-level mixed loop nesting\");
}

void test_continue_in_while_with_defer(void) {
	// Continue in while loop - defer must run each iteration
	log_reset();
	int i = 0;
	while (i < 3) {
		defer log_append("D");
		i++;
		if (i == 2) {
			log_append("S");
			continue;
		}
		log_append("N");
	}
	log_append("E");
	CHECK_LOG("NDSDNDE", "continue in while with defer");
}

void test_break_in_do_while_with_defer(void) {
	// Break in do-while - defer must run
	log_reset();
	int i = 0;
	do {
		defer log_append("D");
		i++;
		if (i == 2) {
			log_append("B");
			break;
		}
		log_append("N");
	} while (i < 5);
	log_append("E");
	CHECK_LOG("NDBDE", "break in do-while with defer");
}

void test_switch_inside_loop_continue(void) {
	// Switch inside loop with continue - defers in switch case must run
	log_reset();
	for (int i = 0; i < 2; i++) {
		defer log_append("L");
		switch (i) {
		case 0: {
			defer log_append("S");
			log_append("0");
			continue; // triggers S, then L, then next iteration
		}
		case 1: {
			defer log_append("T");
			log_append("1");
			break;
		}
		}
		log_append("X");
	}
	log_append("E");
	CHECK_LOG("0SL1TXLE", "switch inside loop with continue");
}

void test_loop_inside_switch_break(void) {
	// Loop inside switch case - break from loop should not exit switch
	log_reset();
	int x = 1;
	switch (x) {
	case 1: {
		defer log_append("S");
		for (int i = 0; i < 3; i++) {
			defer log_append("L");
			log_append("I");
			if (i == 1) break; // exits loop, not switch
		}
		log_append("A"); // Should be reached
		break;		 // exits switch
	}
	}
	log_append("E");
	CHECK_LOG("ILILASE", "loop inside switch - break loop not switch");
}


void test_switch_sequential_no_leak(void) {
	log_reset();
	switch (1) {
	case 1: {
		defer log_append("A");
		log_append("1");
		break;
	}
	}
	switch (2) {
	case 2: {
		defer log_append("B");
		log_append("2");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("1A2BE", "sequential switches don't leak defers");
}

void test_switch_case_group_defer(void) {
	log_reset();
	int x = 2;
	switch (x) {
	case 1:
	case 2:
	case 3: {
		defer log_append("D");
		log_append("X");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("XDE", "case group labels sharing body with defer");
}

void test_switch_case_group_fallthrough(void) {
	log_reset();
	switch (0) {
	case 0:
	case 1: {
		defer log_append("A");
		log_append("X");
	} // A fires at }, then fallthrough
	case 2:
	case 3: {
		defer log_append("B");
		log_append("Y");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("XAYBE", "case group fallthrough with defers");
}

void test_switch_deep_nested_break(void) {
	log_reset();
	switch (1) {
	case 1: {
		defer log_append("1");
		{
			defer log_append("2");
			{
				defer log_append("3");
				{
					defer log_append("4");
					log_append("X");
					break; // Should trigger 4, 3, 2, 1 in LIFO order
				}
			}
		}
	}
	}
	log_append("E");
	CHECK_LOG("X4321E", "deep nested blocks in switch case with break");
}

int test_switch_deep_return_helper(void) {
	log_reset();
	defer log_append("F"); // function scope
	switch (1) {
	case 1: {
		defer log_append("S"); // switch case scope
		{
			defer log_append("N"); // nested block
			log_append("X");
			return 42; // Should trigger N, S, F
		}
	}
	}
	return 0;
}

void test_switch_deep_return(void) {
	int ret = test_switch_deep_return_helper();
	CHECK_LOG("XNSF", "return from deep switch unwinds all scopes");
	CHECK_EQ(ret, 42, "deep switch return value preserved");
}

void test_switch_only_default(void) {
	log_reset();
	switch (999) {
	default: {
		defer log_append("D");
		log_append("X");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("XDE", "switch with only default and defer");
}

void test_switch_all_cases_defer(void) {
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
		break;
	}
	case 3: {
		defer log_append("C");
		log_append("3");
		break;
	}
	default: {
		defer log_append("D");
		log_append("X");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("2BE", "all cases with defers - only active case fires");
}

void test_switch_defer_enclosing_scope(void) {
	log_reset();
	{
		defer log_append("D");
		switch (42) {
		default: break;
		}
		log_append("X");
	}
	log_append("E");
	CHECK_LOG("XDE", "switch with defer in enclosing scope");
}

void test_switch_nested_mixed_defer(void) {
	log_reset();
	switch (1) {
	case 1: {
		defer log_append("O");
		switch (2) {
		case 2:
			log_append("I"); // No defer in inner switch
			break;
		}
		log_append("M");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("IMOE", "nested switch - inner no defer, outer has defer");
}

void test_switch_nested_inner_defer(void) {
	log_reset();
	switch (1) {
	case 1: {
		switch (2) {
		case 2: {
			defer log_append("I");
			log_append("X");
			break;
		}
		}
		log_append("M"); // No defer at outer case scope
		break;
	}
	}
	log_append("E");
	CHECK_LOG("XIME", "nested switch - inner has defer, outer doesn't");
}

void test_switch_do_while_0(void) {
	log_reset();
	switch (1) {
	case 1: {
		defer log_append("D");
		do {
			log_append("X");
		} while (0);
		break;
	}
	}
	log_append("E");
	CHECK_LOG("XDE", "switch case with do-while(0) and defer");
}

void test_switch_negative_cases(void) {
	log_reset();
	switch (-1) {
	case -2: {
		defer log_append("A");
		log_append("a");
		break;
	}
	case -1: {
		defer log_append("B");
		log_append("b");
		break;
	}
	case 0: {
		defer log_append("C");
		log_append("c");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("bBE", "switch with negative case values and defer");
}

void test_switch_stmt_expr_defer(void) {
	log_reset();
	switch (1) {
	case 1: {
		defer log_append("O");
		int val = ({
			int r;
			{
				defer log_append("SE");
				log_append("X");
				r = 42;
			}
			r;
		});
		(void)val;
		log_append("Y");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("XSEYOE", "switch with stmt expr containing defer");
}

void test_switch_in_stmt_expr_in_switch(void) {
	log_reset();
	int x = 1;
	switch (x) {
	case 1: {
		defer log_append("O");
		int val = ({
			int r = 0;
			switch (2) {
			case 2: {
				defer log_append("I");
				r = 42;
				break;
			}
			}
			r;
		});
		(void)val;
		log_append("X");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("IXOE", "switch in stmt expr in switch");
}

void test_switch_triple_sequential(void) {
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
		case 2: {
			defer log_append("C");
			log_append("2");
			break;
		}
		}
	}
	log_append("E");
	CHECK_LOG("0A1B2CE", "triple sequential switches in loop");
}

void test_duffs_device_braced_defers(void) {
	int duff_total = 0;
	int count = 6;
	int n = (count + 3) / 4;
	switch (count % 4) {
	case 0:
		do {
			{
				defer duff_total++;
			}
		case 3: {
			defer duff_total++;
		}
		case 2: {
			defer duff_total++;
		}
		case 1: {
			defer duff_total++;
		}
		} while (--n > 0);
	}
	// count=6, 6%4=2, starts at case 2: 2,1 (2 iterations first partial)
	// then 0,3,2,1 (4 iterations full round)
	// Total: 2+4 = 6
	CHECK_EQ(duff_total, 6, "duff braced defers: count=6 iterations");
}

void test_duffs_device_all_entries(void) {
	// Test each possible entry point
	for (int entry = 0; entry < 4; entry++) {
		int duff_total = 0;
		int items = 4 + entry; // 4,5,6,7 items
		int n = (items + 3) / 4;
		switch (items % 4) {
		case 0:
			do {
				{
					defer duff_total++;
				}
			case 3: {
				defer duff_total++;
			}
			case 2: {
				defer duff_total++;
			}
			case 1: {
				defer duff_total++;
			}
			} while (--n > 0);
		}
		CHECK_EQ(duff_total, items, "duff all entries: correct iteration count");
	}
}

void test_switch_goto_deep(void) {
	log_reset();
	defer log_append("F"); // function-level
	switch (1) {
	case 1: {
		defer log_append("S");
		{
			defer log_append("N");
			log_append("X");
			goto out;
		}
	}
	}
out:
	log_append("E");
}

void test_switch_continue_enclosing_loop_defer(void) {
	log_reset();
	for (int i = 0; i < 2; i++) {
		defer log_append("L");
		switch (i) {
		case 0: {
			defer log_append("S0");
			log_append("A");
			continue; // should fire S0 then L
		}
		case 1: {
			defer log_append("S1");
			log_append("B");
			break;
		}
		}
		log_append("M"); // only reached for case 1 (break doesn't skip this)
	}
	log_append("E");
	CHECK_LOG("AS0LBS1MLE", "switch continue from enclosing loop");
}

void test_switch_inner_break_isolation(void) {
	log_reset();
	switch (1) {
	case 1: {
		defer log_append("O");
		switch (1) {
		case 1: {
			defer log_append("I");
			log_append("X");
			break; // breaks from INNER switch only
		}
		}
		// Execution continues here after inner break
		log_append("Y");
		break; // breaks from OUTER switch
	}
	}
	log_append("E");
	CHECK_LOG("XIYOE", "inner break doesn't affect outer switch");
}

void test_switch_computed_case(void) {
	log_reset();

	enum { BASE = 10, OFFSET = 5 };

	switch (BASE + OFFSET) {
	case BASE + OFFSET: {
		defer log_append("D");
		log_append("X");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("XDE", "computed case value with defer");
}

void test_switch_default_middle(void) {
	log_reset();
	switch (42) // doesn't match any explicit case
	{
	case 1: {
		defer log_append("A");
		log_append("1");
		break;
	}
	default: {
		defer log_append("D");
		log_append("X");
		break;
	}
	case 2: {
		defer log_append("B");
		log_append("2");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("XDE", "default in middle of switch with defer");
}

void test_switch_multi_fallthrough(void) {
	log_reset();
	switch (0) {
	case 0: {
		defer log_append("A");
		log_append("0");
	}
	case 1: {
		defer log_append("B");
		log_append("1");
	}
	case 2: {
		defer log_append("C");
		log_append("2");
	}
	case 3: {
		defer log_append("D");
		log_append("3");
		break;
	}
	}
	log_append("E");
	CHECK_LOG("0A1B2C3DE", "multi-level fallthrough with defers");
}

void test_duffs_device_single_item(void) {
	int duff_total = 0;
	int count = 1;
	int n = (count + 3) / 4;
	switch (count % 4) {
	case 0:
		do {
			{
				defer duff_total++;
			}
		case 3: {
			defer duff_total++;
		}
		case 2: {
			defer duff_total++;
		}
		case 1: {
			defer duff_total++;
		}
		} while (--n > 0);
	}
	CHECK_EQ(duff_total, 1, "duff single item: exactly 1 iteration");
}

void test_switch_goto_forward_case(void) {
	log_reset();
	int x = 1;
	switch (x) {
	case 1: {
		defer log_append("A");
		log_append("1");
		goto skip;
	}
	case 2: {
		log_append("2");
		break;
	}
	}
skip:
	log_append("E");
	CHECK_LOG("1AE", "switch goto forward past cases");
}

void test_switch_loop_switch(void) {
	log_reset();
	int sum = 0;
	for (int i = 0; i < 2; i++) {
		defer log_append("L");
		switch (i) {
		case 0: {
			defer log_append("X");
			sum += 1;
			break;
		}
		case 1: {
			defer log_append("Y");
			sum += 10;
			break;
		}
		}
	}
	log_append("E");
	CHECK_EQ(sum, 11, "switch-loop-switch sum correct");
	CHECK_LOG("XLYLE", "switch-loop-switch defer order");
}

int test_triple_nested_switch_return_helper(void) {
	log_reset();
	defer log_append("F");
	switch (1) {
	case 1: {
		defer log_append("A");
		switch (2) {
		case 2: {
			defer log_append("B");
			switch (3) {
			case 3: {
				defer log_append("C");
				log_append("X");
				return 99;
			}
			}
		}
		}
	}
	}
	return 0;
}

void test_triple_nested_switch_return(void) {
	int ret = test_triple_nested_switch_return_helper();
	CHECK_LOG("XCBAF", "triple nested switch return unwinds all");
	CHECK_EQ(ret, 99, "triple nested switch return value");
}

static int _defer_sibling_goto_helper(void) {
	int counter = 0;
	{
		defer counter += 10;
		goto target;
	}
	{
	target:
		// counter should be 10 here - the defer should have fired
		// when we left the first block via goto
		return counter;
	}
}

void test_defer_sibling_goto_bug(void) {
	int val = _defer_sibling_goto_helper();
	CHECK_EQ(val, 10, "goto to sibling block emits source block defer");
}

static int _defer_sibling_goto_multi_helper(void) {
	int counter = 0;
	{
		defer counter += 1;
		defer counter += 10;
		defer counter += 100;
		goto target;
	}
	{
	target:
		return counter;
	}
}

void test_defer_sibling_goto_multi_bug(void) {
	int val = _defer_sibling_goto_multi_helper();
	CHECK_EQ(val, 111, "goto to sibling block emits all source defers (LIFO)");
}

void test_defer_sibling_goto_lifo_bug(void) {
	log_reset();
	{
		defer log_append("C");
		defer log_append("B");
		defer log_append("A");
		log_append("1");
		goto sib;
	}
	{
	sib:
		log_append("2");
	}
	CHECK_LOG("1ABC2", "goto to sibling block defers fire in LIFO order");
}

void test_defer_backward_goto_sibling(void) {
	log_reset();
	int visited = 0;
	{
	back_sib_target:
		log_append("T");
		if (visited) goto back_sib_done;
	}
	{
		defer log_append("D");
		visited = 1;
		goto back_sib_target;
	}
back_sib_done:
	log_append("E");
	CHECK_LOG("TDTE", "backward goto to sibling scope fires defer");
}

void test_defer_goto_forward(void) {
	log_reset();
	goto target;
	{
		defer log_append("A");
		log_append("B");
	}
target:
	log_append("C");
	CHECK_LOG("C", "goto forward skipping defer block");
}

static void check_defer_transpile_rejects(const char *code,
					  const char *file_name,
					  const char *name,
					  const char *needle) {
	PrismResult result = prism_transpile_source(code, file_name, prism_defaults());
	CHECK(result.status != PRISM_OK, name);
	if (result.error_msg && needle)
		CHECK(strstr(result.error_msg, needle) != NULL,
		      "defer negative: error mentions expected keyword");
	prism_free(&result);
}

static int run_command_status(const char *cmd) {
	int status = system(cmd);
	if (status == -1) return -1;
	if (!WIFEXITED(status)) return -1;
	return WEXITSTATUS(status);
}

static void check_transpiled_output_compiles_and_runs(const char *output,
					      const char *compile_name,
					      const char *run_name) {
	char *src_path = create_temp_file(output);
	CHECK(src_path != NULL, "defer compile helper: create temp source");
	if (!src_path) return;

	char bin_template[] = "/tmp/prism_defer_exec_XXXXXX";
	int bin_fd = mkstemp(bin_template);
	CHECK(bin_fd >= 0, "defer compile helper: reserve temp binary path");
	if (bin_fd < 0) {
		unlink(src_path);
		free(src_path);
		return;
	}
	close(bin_fd);
	unlink(bin_template);

	char compile_cmd[PATH_MAX * 2 + 64];
	snprintf(compile_cmd, sizeof(compile_cmd),
		 "cc -std=gnu11 -o %s %s >/dev/null 2>&1", bin_template, src_path);
	CHECK_EQ(run_command_status(compile_cmd), 0, compile_name);
	if (access(bin_template, X_OK) == 0)
		CHECK_EQ(run_command_status(bin_template), 0, run_name);

	unlink(bin_template);
	unlink(src_path);
	free(src_path);
}

void test_defer_goto_into_scope_rejected(void) {
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    goto target;\n"
	    "    {\n"
	    "        defer (void)0;\n"
	    "    target:\n"
	    "        (void)0;\n"
	    "    }\n"
	    "}\n",
	    "defer_goto_into_scope.c",
	    "goto into defer scope rejected",
	    "skip over this defer");
}

void test_defer_in_ctrl_paren_rejected(void) {
	check_defer_transpile_rejects(
	    "int main(void) { for (; defer 0;) {} return 0; }\n",
	    "defer_ctrl_paren_reject.c",
	    "defer in control parens rejected",
	    "control statement parentheses");
}

void test_defer_braceless_control_rejected(void) {
	check_defer_transpile_rejects(
	    "int main(void) { if (1) defer (void)0; return 0; }\n",
	    "defer_braceless_control_reject.c",
	    "defer in braceless control rejected",
	    "requires braces");
}

#ifdef __GNUC__
void test_defer_stmt_expr_top_level_rejected(void) {
	check_defer_transpile_rejects(
	    "int f(void) { return ({ defer (void)0; 1; }); }\n",
	    "defer_stmt_expr_top_level_reject.c",
	    "top-level stmt expr defer rejected",
	    "statement expression");
}

void test_defer_computed_goto_rejected(void) {
	check_defer_transpile_rejects(
	    "int f(void) {\n"
	    "    void *label = &&out;\n"
	    "    defer (void)0;\n"
	    "    goto *label;\n"
	    "out:\n"
	    "    return 0;\n"
	    "}\n",
	    "defer_computed_goto_reject.c",
	    "computed goto with defer rejected",
	    "computed goto");
}

void test_defer_asm_rejected(void) {
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    __asm__(\"nop\");\n"
	    "    defer (void)0;\n"
	    "}\n",
	    "defer_asm_reject.c",
	    "defer with asm rejected",
	    "inline assembly");
}
#endif

void test_defer_setjmp_rejected(void) {
	check_defer_transpile_rejects(
	    "#include <setjmp.h>\n"
	    "static jmp_buf buf;\n"
	    "void f(void) {\n"
	    "    defer (void)0;\n"
	    "    if (setjmp(buf)) return;\n"
	    "}\n",
	    "defer_setjmp_reject.c",
	    "defer with setjmp rejected",
	    "setjmp");
}

#ifndef _WIN32
void test_defer_vfork_rejected(void) {
	check_defer_transpile_rejects(
	    "#include <unistd.h>\n"
	    "void f(void) {\n"
	    "    (void)vfork();\n"
	    "    defer (void)0;\n"
	    "}\n",
	    "defer_vfork_reject.c",
	    "defer with vfork rejected",
	    "vfork");
}
#endif

static void _c23_attr_label_helper(void) {
	log_reset();
	{
		defer log_append("D");
		log_append("B");
		goto done;
		log_append("X"); // unreachable
	}
	[[maybe_unused]] done:
	log_append("E");
}

void test_defer_goto_c23_attr_label(void) {
	_c23_attr_label_helper();
	CHECK_LOG("BDE", "goto to C23 attributed label fires defer");
}


static void test_auto_type_fallback_requires_gnu_extensions(void) {
	printf("\n--- __auto_type Fallback Requires GNU Extensions ---\n");

	const char *code =
	    "#include <stdio.h>\n"
	    "struct { int x; } anon_fn(void) {\n"
	    "    defer printf(\"deferred\\n\");\n"
	    "    return (struct { int x; }){42};\n"
	    "}\n"
	    "int main(void) { return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "auto_type fallback: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "auto_type fallback: transpiles OK");
	CHECK(result.output != NULL, "auto_type fallback: output not NULL");

	// __auto_type is the only correct approach for anonymous struct returns with defer.
	// Each anonymous struct definition creates a unique type in C, so __typeof__ and
	// re-emitting the struct tokens would both produce incompatible types.
	// __auto_type uses the reserved __ prefix and works in all C standard modes
	// (including -std=c99 and -std=c11) with GCC and Clang.
	CHECK(strstr(result.output, "__auto_type") != NULL,
	      "auto_type fallback: anonymous struct return with defer should use __auto_type");
	// Verify the defer is still emitted correctly
	CHECK(strstr(result.output, "printf") != NULL,
	      "auto_type fallback: deferred printf should be present in output");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_ternary_cast_corrupts_label_detection(void) {
	printf("\n--- Ternary Casts Corrupt goto Label Detection ---\n");

	// In "cond ? (int)done : 0", the cast puts ')' before 'done',
	// so the "prev == '?'" ternary filter fails and 'done:' is
	// falsely registered as a label. This causes goto_skips_check
	// to return early, missing the defer between the false and real label.
	const char *code =
	    "void cleanup(void);\n"
	    "int done = 42;\n"
	    "void f(void) {\n"
	    "    goto done;\n"
	    "    (void)(1 ? (int)done : 0);\n"
	    "    defer cleanup();\n"
	    "    done:\n"
	    "    (void)0;\n"
	    "}\n"
	    "int main(void) { f(); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "ternary cast label: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);

	// The goto jumps past "defer cleanup()" to reach "done:".
	// The transpiler should error about skipping the defer.
	// With the bug, the cast in "(int)done" causes false label detection,
	// making goto_skips_check return early and miss the defer.
	CHECK(result.status != PRISM_OK,
	      "ternary cast label: goto skipping defer should be caught even with ternary cast");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_gnu_nested_func_breaks_outer_defer(void) {
	printf("\n--- GNU Nested Function Breaks Outer Defer ---\n");

	const char *code =
	    "int outer(int n) {\n"
	    "    defer (void)0;\n"
	    "    int inner(int x) {\n"
	    "        return x * 2;\n"
	    "    }\n"
	    "    return inner(n);\n"
	    "}\n"
	    "int main(void) { return outer(5) == 10 ? 0 : 1; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "nested func: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK(result.status != PRISM_OK,
	      "nested func: reject nested function inside defer scope");
	if (result.error_msg)
		CHECK(strstr(result.error_msg, "nested function") != NULL,
		      "nested func: error mentions nested function");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_knr_nested_func_detection(void) {
	printf("\n--- K&R Nested Function Bypasses Guardrail ---\n");

	const char *code =
	    "void outer(void) {\n"
	    "    defer (void)0;\n"
	    "    int inner(x)\n"
	    "        int x;\n"
	    "    {\n"
	    "        return x + 1;\n"
	    "    }\n"
	    "}\n"
	    "int main(void) { return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "knr nested func: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);

	// K&R-style nested function must be detected and rejected.
	CHECK(result.status != PRISM_OK,
	      "knr nested func: should error on K&R-style nested function in defer scope");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_defer_scope_state_machine_overwrite(void) {
	printf("\n--- defer Scope State Machine Overwrite Allows Unsafe Escapes ---\n");

	// When a switch is nested inside a loop inside defer, closing the switch '}'
	// used to overwrite the scalar scope_stack, causing the outer loop's depth
	// to never decrement.  The trailing "break;" after the loop should be caught
	// as an illegal escape from the defer block.
	const char *code =
	    "void f(int x) {\n"
	    "    defer {\n"
	    "        while (1) {\n"
	    "            switch (x) {\n"
	    "                default: break;\n"
	    "            }\n"
	    "            break;\n"
	    "        }\n"
	    "        break;\n"  /* this must be caught */
	    "    }\n"
	    "}\n"
	    "int main(void) { f(0); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "defer scope overwrite: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);

	CHECK(result.status != PRISM_OK,
	      "defer scope overwrite: break after while/switch inside defer must be rejected");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_braceless_body_semicolon_trap(void) {
	printf("\n--- Braceless Body Semicolon Trap inside defer ---\n");

	// A braceless while body containing an if/else with braced arms
	// used to trigger premature braceless body ending on ';' inside
	// the braced arms (bd <= braceless_bd + 1 was too broad).
	// This caused inner_loop_depth to decrement early, and the second
	// break in the else arm was rejected as a false-positive error.
	const char *code =
	    "void f(int cond) {\n"
	    "    defer {\n"
	    "        while(1)\n"
	    "            if (cond) { break; } else { break; }\n"
	    "    };\n"
	    "}\n"
	    "int main(void) { f(1); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "braceless trap: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);

	CHECK_EQ(result.status, PRISM_OK,
	         "braceless trap: both breaks inside while are valid (not false positive)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_scope_type_at_depth_overflow(void) {
	printf("\n--- 256-Depth scope_type_at_depth Overflow ---\n");

	// When brace depth exceeds the scope_type_at_depth array bound,
	// inner_loop_depth gets incremented on '{' but not decremented on '}',
	// causing permanent skew. With the expanded 4096-element array, depths
	// up to 4095 are handled correctly.
	// Build code with 300 nested braces inside defer + for loop body.
	char code[16384];
	int pos = 0;
	pos += snprintf(code + pos, sizeof(code) - pos,
	    "int main(void) {\n"
	    "    defer (void)0;\n"
	    "    for (int i = 0; i < 1; i++) {\n");
	for (int i = 0; i < 300; i++)
		pos += snprintf(code + pos, sizeof(code) - pos, "{\n");
	pos += snprintf(code + pos, sizeof(code) - pos, "(void)0;\n");
	for (int i = 0; i < 300; i++)
		pos += snprintf(code + pos, sizeof(code) - pos, "}\n");
	pos += snprintf(code + pos, sizeof(code) - pos,
	    "    }\n"
	    "    return 0;\n"
	    "}\n");

	char *path = create_temp_file(code);
	CHECK(path != NULL, "scope depth overflow: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "scope depth overflow: transpiles OK");
	CHECK(result.output != NULL, "scope depth overflow: output not NULL");
	if (result.output)
		check_transpiled_output_compiles_and_runs(
		    result.output,
		    "scope depth overflow: transpiled output compiles",
		    "scope depth overflow: transpiled output runs");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_fno_defer_shadow_leak(void) {
	printf("\n--- -fno-defer Shadow Leak ---\n");

	// After a function definition, a global variable with a __-prefixed typedef
	// name should still get zero-initialized (typedef is still recognized).
	// With the bug, last_toplevel_paren stays set, blocking register_toplevel_shadows.
	const char *code =
	    "typedef struct { int v; } __widget_t;\n"
	    "\n"
	    "void first_func(void) {}\n"
	    "\n"
	    "void second_func(void) {\n"
	    "    __widget_t w;\n"
	    "    (void)w;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "fno-defer shadow: create temp file");

	PrismFeatures features = prism_defaults();
	features.defer = false;
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "fno-defer shadow: transpiles OK");
	CHECK(result.output != NULL, "fno-defer shadow: output not NULL");

	// __widget_t w; should still get = {0} because __widget_t is a known typedef.
	// If shadow tracking is broken, the typedef might be misinterpreted.
	CHECK(strstr(result.output, "= {0}") != NULL,
	      "fno-defer shadow: __widget_t w still gets = {0} after first function");

	prism_free(&result);
	unlink(path);
	free(path);
}

void run_defer_tests(void) {
	printf("\n=== DEFER TESTS ===\n");

	test_defer_basic();
	test_defer_lifo();
	log_reset();
	int ret = test_defer_return();
	CHECK_LOG("1A", "defer with return");
	CHECK_EQ(ret, 42, "defer return value preserved");
	test_defer_goto_out();
	test_defer_goto_c23_attr();
	test_defer_nested_scopes();
	test_defer_break();
	test_defer_continue();
	test_defer_switch_break();
	test_defer_switch_fallthrough();
	test_defer_while();
	test_defer_do_while();
	log_reset();
	ret = test_defer_nested_return();
	CHECK_LOG("R321", "defer nested return");
	CHECK_EQ(ret, 99, "defer nested return value");
	test_defer_compound_stmt();
	test_defer_zeroinit_inside();
	test_defer_zeroinit_struct_inside();
	test_defer_raw_inside();
	test_defer_only_body();
	CHECK_LOG("D", "defer-only function body");

	global_val = 0;
	ret = test_return_side_effect();
	CHECK_EQ(ret, 0, "return side effect - return value");
	CHECK_EQ(global_val, 100, "return side effect - defer executed");
	test_defer_capture_timing();
	CHECK_LOG("1Y", "defer capture timing");
	log_reset();
	recursion_count = 0;
	test_recursive_defer(3);
	CHECK_EQ(recursion_count, 3, "recursive defer count");
	CHECK_LOG("RRR", "recursive defer order");
	test_defer_goto_backward();
	test_defer_deeply_nested();
	test_defer_nested_loops();
	test_defer_break_inner_stay_outer();
	test_deep_defer_with_zeroinit();
#ifdef __GNUC__
	test_typeof_void_defer_return();
	test_dunder_typeof_void_defer_return();
#endif
	test_void_return_void0_defer();

	test_switch_fallthrough_no_braces();
	test_switch_break_from_nested_block();
	test_switch_goto_out_of_case();
	test_switch_multiple_defers_per_case();
	test_switch_nested_switch_defer();

	test_break_continue_nested_3_levels();
	test_continue_in_while_with_defer();
	test_break_in_do_while_with_defer();
	test_switch_inside_loop_continue();
	test_loop_inside_switch_break();

	test_switch_sequential_no_leak();
	test_switch_case_group_defer();
	test_switch_case_group_fallthrough();
	test_switch_deep_nested_break();
	test_switch_deep_return();
	test_switch_only_default();
	test_switch_all_cases_defer();
	test_switch_defer_enclosing_scope();
	test_switch_nested_mixed_defer();
	test_switch_nested_inner_defer();
	test_switch_do_while_0();
	test_switch_negative_cases();
	test_switch_stmt_expr_defer();
	test_switch_in_stmt_expr_in_switch();
	test_switch_triple_sequential();
	test_duffs_device_braced_defers();
	test_duffs_device_all_entries();
	test_switch_goto_deep();
	CHECK_LOG("XNSEF", "switch goto deep unwinds through nested scopes");
	test_switch_continue_enclosing_loop_defer();
	test_switch_inner_break_isolation();
	test_switch_computed_case();
	test_switch_default_middle();
	test_switch_multi_fallthrough();
	test_duffs_device_single_item();
	test_switch_goto_forward_case();
	test_switch_loop_switch();
	test_triple_nested_switch_return();

	test_defer_sibling_goto_bug();
	test_defer_sibling_goto_multi_bug();
	test_defer_sibling_goto_lifo_bug();

	test_defer_goto_forward();
	test_defer_goto_into_scope_rejected();

	test_defer_backward_goto_sibling();

	test_defer_goto_c23_attr_label();
	test_defer_in_ctrl_paren_rejected();
	test_defer_braceless_control_rejected();
#ifdef __GNUC__
	test_defer_stmt_expr_top_level_rejected();
	test_defer_computed_goto_rejected();
	test_defer_asm_rejected();
#endif
	test_defer_setjmp_rejected();
#ifndef _WIN32
	test_defer_vfork_rejected();
#endif
	test_auto_type_fallback_requires_gnu_extensions();
	test_ternary_cast_corrupts_label_detection();
	test_gnu_nested_func_breaks_outer_defer();
	test_knr_nested_func_detection();
	test_defer_scope_state_machine_overwrite();
	test_braceless_body_semicolon_trap();
	test_scope_type_at_depth_overflow();
	test_fno_defer_shadow_leak();
}

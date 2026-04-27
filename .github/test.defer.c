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

#ifndef _MSC_VER
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
#endif

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

#ifndef _MSC_VER
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
#endif

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

void test_defer_sibling_goto(void) {
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

void test_defer_sibling_goto_multi(void) {
	int val = _defer_sibling_goto_multi_helper();
	CHECK_EQ(val, 111, "goto to sibling block emits all source defers (LIFO)");
}

void test_defer_sibling_goto_lifo(void) {
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

void test_defer_cross_branch_goto_nested(void) {
	log_reset();
	int i = 0;
	{ // outer sibling 1
		defer log_append("A");
		{ // nested
			defer log_append("B");
			if (i == 0) { // deeply nested
				i++;
				goto L_cross;
			}
		}
	}
	{ // outer sibling 2
		L_cross:
		log_append("T");
	}
	CHECK_LOG("BAT", "cross-branch goto from nested scope fires all ancestor defers");
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

static void test_defer_stmt_expr_nested_block_last_stmt_corrupts(void) {
	/* a nested block containing defer is the LAST
	 * statement of a GNU statement expression.  handle_close_brace emits
	 * defers in LIFO order after the last C expression, placing the defer
	 * call after the intended return value.  For void defers (free/cleanup)
	 * this produces "void value not ignored" compile errors.  For non-void
	 * defers (counter++, x--) it silently assigns the wrong value:
	 *
	 *   int f(void) { return ({ { defer cleanup(); work(); } }); }
	 *   → emits: return ({ { work(); cleanup(); } });
	 *   → f() returns void (cleanup result), NOT work() result.
	 *
	 * Since recovering the last expression requires a full expression parser
	 * (which Prism intentionally omits), Prism must reject this pattern.
	 */
	check_defer_transpile_rejects(
	    "void cleanup(void);\n"
	    "int work(void);\n"
	    "int f(void) {\n"
	    "    return ({\n"
	    "        {\n"
	    "            defer cleanup();\n"
	    "            work();\n"
	    "        }\n"
	    "    });\n"
	    "}\n",
	    "defer_stmt_expr_nested_last.c",
	    "defer in nested block that is last stmt of stmt_expr corrupts value",
	    "statement expression");
}

void test_defer_stmt_expr_return_bypass(void) {
	// A return inside a GNU statement expression inside a defer body should
	// be rejected — it bypasses remaining defers just like a bare return.
	check_defer_transpile_rejects(
	    "int f(void) {\n"
	    "    defer {\n"
	    "        int status = ({\n"
	    "            if (1) return -1;\n"
	    "            0;\n"
	    "        });\n"
	    "        (void)status;\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n",
	    "defer_stmt_expr_return_bypass.c",
	    "defer stmt-expr return bypass rejected",
	    "return");
}

void test_defer_stmt_expr_goto_bypass(void) {
	// A goto inside a GNU statement expression inside a defer body should
	// also be rejected.
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    defer {\n"
	    "        int status = ({\n"
	    "            if (1) goto bail;\n"
	    "            0;\n"
	    "        });\n"
	    "        (void)status;\n"
	    "    }\n"
	    "    bail: (void)0;\n"
	    "}\n",
	    "defer_stmt_expr_goto_bypass.c",
	    "defer stmt-expr goto bypass rejected",
	    "goto");
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

static void test_computed_goto_forward_into_deferred_scope(void) {
	/* computed goto (goto *ptr) forwards into a scope PAST its defer and
	 * zeroinit entries.  Phase 1 CFG verifier (p1_verify_cfg) only records a
	 * P1K_GOTO entry when tok_next(goto_tok) is identifier-like.  For a
	 * computed goto the next token is '*', so the goto is NEVER inserted into
	 * the P1 CFG array — it is completely invisible to CFG analysis.
	 *
	 * Phase 2 handle_goto_keyword() checks has_active_defers() only at the
	 * GOTO SITE (no defers are live there), not whether the TARGET label sits
	 * inside a scope whose defer/decl precede it.  So goto *ptr silently flies
	 * past both the defer statement and the zero-init, producing broken C with
	 * uninitialized reads and a missed cleanup — no rejection, no warning.
	 */
	check_defer_transpile_rejects(
	    "void noop(int *p);\n"
	    "void exploit(void) {\n"
	    "    void *target = &&inside;\n"
	    "    goto *target;\n"
	    "    {\n"
	    "        int secret;\n"
	    "        defer noop(&secret);\n"
	    "    inside:\n"
	    "        noop(&secret);\n"
	    "    }\n"
	    "}\n",
	    "computed_goto_forward_defer.c",
	    "computed-goto-forward-defer: goto *ptr into scope past defer "
	    "must be rejected (CFG verifier blind to computed gotos)",
	    "computed goto");
}

void test_defer_asm_rejected(void) {
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    __asm__ goto(\"\" : : : : lbl);\n"
	    "    defer (void)0;\n"
	    "lbl:;\n"
	    "}\n",
	    "defer_asm_reject.c",
	    "defer with asm goto rejected",
	    "asm goto");
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

// glibc expands sigsetjmp to __sigsetjmp after preprocessing.
// Prism must reject defer in functions calling these internal variants.
void test_defer_glibc_sigsetjmp_rejected(void) {
	// __sigsetjmp: glibc internal for sigsetjmp
	check_defer_transpile_rejects(
	    "int __sigsetjmp(void *env, int savesigs);\n"
	    "void f(void) {\n"
	    "    defer (void)0;\n"
	    "    __sigsetjmp((void*)0, 0);\n"
	    "}\n",
	    "defer_glibc_sigsetjmp.c",
	    "defer with __sigsetjmp rejected",
	    "setjmp");

	// __siglongjmp: glibc internal for siglongjmp
	check_defer_transpile_rejects(
	    "void __siglongjmp(void *env, int val);\n"
	    "void f(void) {\n"
	    "    defer (void)0;\n"
	    "    __siglongjmp((void*)0, 1);\n"
	    "}\n",
	    "defer_glibc_siglongjmp.c",
	    "defer with __siglongjmp rejected",
	    "setjmp");

	// __setjmp: BSD/musl internal
	check_defer_transpile_rejects(
	    "int __setjmp(void *env);\n"
	    "void f(void) {\n"
	    "    defer (void)0;\n"
	    "    __setjmp((void*)0);\n"
	    "}\n",
	    "defer_glibc___setjmp.c",
	    "defer with __setjmp rejected",
	    "setjmp");

	// __longjmp: glibc/musl internal
	check_defer_transpile_rejects(
	    "void __longjmp(void *env, int val);\n"
	    "void f(void) {\n"
	    "    defer (void)0;\n"
	    "    __longjmp((void*)0, 1);\n"
	    "}\n",
	    "defer_glibc___longjmp.c",
	    "defer with __longjmp rejected",
	    "setjmp");

	// __longjmp_chk: glibc fortified variant
	check_defer_transpile_rejects(
	    "void __longjmp_chk(void *env, int val);\n"
	    "void f(void) {\n"
	    "    defer (void)0;\n"
	    "    __longjmp_chk((void*)0, 1);\n"
	    "}\n",
	    "defer_glibc___longjmp_chk.c",
	    "defer with __longjmp_chk rejected",
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

#ifndef _MSC_VER
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
#endif


static void test_auto_type_fallback_requires_gnu_extensions(void) {
	printf("\n--- Unresolvable Return Type With Defer Rejected ---\n");

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
	// Anonymous struct return types with defer cannot be captured portably.
	// The transpiler now rejects this for all compilers (no __auto_type fallback).
	CHECK(result.status != PRISM_OK,
	      "anonymous struct return with defer: rejected (use a named type)");
	if (result.error_msg)
		CHECK(strstr(result.error_msg, "unresolvable return type") != NULL,
		      "error message mentions unresolvable return type");

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

static void test_extern_decl_not_nested_func(void) {
	printf("\n--- Extern Decl Not Misidentified as Nested Function ---\n");

	// extern forward declarations with attributes inside function bodies
	// were falsely detected as GNU nested function definitions when a switch
	// statement followed. The K&R params fallback scan walked past the ;
	// through subsequent code to the switch body {, and is_knr_params returned
	// true because it found a ; in between. Fix: nested function detection
	// skips declarations with storage class specifiers (extern/static/etc).
	{
		const char *src =
		    "void f(int argc) {\n"
		    "    extern void g(void) __attribute__((visibility(\"hidden\")));\n"
		    "    switch (argc) { case 0: break; }\n"
		    "}\n"
		    "int main(void) { return 0; }\n";

		PrismFeatures feat = prism_defaults();
		PrismResult r = prism_transpile_source(src, "extern_attr.c", feat);
		CHECK(r.status == PRISM_OK,
		      "extern decl + attr + switch: should not be nested func error");
		prism_free(&r);
	}
	// Variant: __asm__
	{
		const char *src =
		    "void f(int argc) {\n"
		    "    extern void g(void) __asm__(\"__g\");\n"
		    "    switch (argc) { case 0: break; }\n"
		    "}\n"
		    "int main(void) { return 0; }\n";

		PrismFeatures feat = prism_defaults();
		PrismResult r = prism_transpile_source(src, "extern_asm.c", feat);
		CHECK(r.status == PRISM_OK,
		      "extern decl + asm + switch: should not be nested func error");
		prism_free(&r);
	}
	// Variant: C23 attribute
	{
		const char *src =
		    "void f(int argc) {\n"
		    "    extern void g(void) [[gnu::visibility(\"hidden\")]];\n"
		    "    switch (argc) { case 0: break; }\n"
		    "}\n"
		    "int main(void) { return 0; }\n";

		PrismFeatures feat = prism_defaults();
		PrismResult r = prism_transpile_source(src, "extern_c23.c", feat);
		CHECK(r.status == PRISM_OK,
		      "extern decl + C23 attr + switch: should not be nested func error");
		prism_free(&r);
	}
	// Variant: glibc makecontext.c pattern (two extern decls + for+switch)
	{
		const char *src =
		    "typedef struct ucontext_t { int x; } ucontext_t;\n"
		    "void f(ucontext_t *ucp, void (*func)(void), int argc) {\n"
		    "    extern void __start_context(void) __attribute__((visibility(\"hidden\")));\n"
		    "    extern void __push(ucontext_t *) __attribute__((visibility(\"hidden\")));\n"
		    "    int i;\n"
		    "    for (i = 0; i < argc; ++i)\n"
		    "        switch (i) {\n"
		    "        case 0: break;\n"
		    "        default: break;\n"
		    "        }\n"
		    "}\n"
		    "int main(void) { return 0; }\n";

		PrismFeatures feat = prism_defaults();
		PrismResult r = prism_transpile_source(src, "glibc_makecontext.c", feat);
		CHECK(r.status == PRISM_OK,
		      "glibc makecontext pattern: should not be nested func error");
		prism_free(&r);
	}
	// Verify actual nested functions still detected
	{
		const char *src =
		    "int outer(void) {\n"
		    "    defer (void)0;\n"
		    "    void inner(void) { }\n"
		    "}\n";

		PrismFeatures feat = prism_defaults();
		PrismResult r = prism_transpile_source(src, "real_nested.c", feat);
		CHECK(r.status != PRISM_OK,
		      "actual nested func: still detected");
		prism_free(&r);
	}
}

static void test_stmt_expr_in_ctrl_cond_label(void) {
	printf("\n--- Stmt-Expr in Ctrl Condition + Label Body ---\n");

	// when a GNU statement expression ({...}) appeared inside an if()
	// condition, the balanced-group scan aborted and tokens were processed
	// individually. The closing ')' of the condition never triggered
	// at_stmt_start=true, so a label on the next line (the if body) was
	// invisible to Phase 1D. Forward gotos to that label failed with
	// "goto target label 'X' not found in scope".
	// Triggered by glibc's pthread_mutex_unlock.c where atomic_load_relaxed
	// expands to ({...}) inside the if() condition.

	// Case 1: stmt-expr in if() + label as body
	{
		const char *src =
		    "int f(int x) {\n"
		    "    int newowner = 0;\n"
		    "    switch (x) {\n"
		    "    case 1:\n"
		    "        goto target;\n"
		    "    case 2:\n"
		    "        if (({ int __v = x; __v; }) == 3)\n"
		    "        target:\n"
		    "            newowner = 99;\n"
		    "        break;\n"
		    "    }\n"
		    "    return newowner;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismFeatures feat = prism_defaults();
		feat.zeroinit = false;
		PrismResult r = prism_transpile_source(src, "stmtexpr_label1.c", feat);
		CHECK(r.status == PRISM_OK,
		      "stmt-expr in if cond + label body: should not error");
		prism_free(&r);
	}
	// Case 2: glibc-like pattern (THREAD_GETMEM → stmt-expr)
	{
		const char *src =
		    "typedef struct { struct { int lock; int owner; int kind; int count; } __data; } mutex_t;\n"
		    "static int unlock(mutex_t *m, int decr) {\n"
		    "    int newowner = 0;\n"
		    "    switch (m->__data.kind) {\n"
		    "    case 10:\n"
		    "        if ((m->__data.lock & 0x3fffffff)\n"
		    "            == ({ int __v = m->__data.owner; __v; }))\n"
		    "        { goto pi_notrecoverable; }\n"
		    "        goto continue_pi_robust;\n"
		    "    case 20:\n"
		    "        if (({ int __k = m->__data.kind; __k; } & 16) != 0\n"
		    "            && m->__data.owner == 3)\n"
		    "        pi_notrecoverable:\n"
		    "            newowner = 4;\n"
		    "        if (({ int __k = m->__data.kind; __k; } & 16) != 0) {\n"
		    "        continue_pi_robust:\n"
		    "            m->__data.count = 0;\n"
		    "        }\n"
		    "        break;\n"
		    "    }\n"
		    "    return newowner;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismFeatures feat = prism_defaults();
		feat.zeroinit = false;
		PrismResult r = prism_transpile_source(src, "stmtexpr_label2.c", feat);
		CHECK(r.status == PRISM_OK,
		      "glibc THREAD_GETMEM pattern: should not error");
		prism_free(&r);
	}
	// Case 3: while + stmt-expr + label
	{
		const char *src =
		    "void f(int x) {\n"
		    "    goto L;\n"
		    "    while (({ int v = x; v; }) > 0)\n"
		    "    L:\n"
		    "        x--;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismFeatures feat = prism_defaults();
		feat.zeroinit = false;
		PrismResult r = prism_transpile_source(src, "stmtexpr_label3.c", feat);
		CHECK(r.status == PRISM_OK,
		      "while + stmt-expr + label: should not error");
		prism_free(&r);
	}
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
	// Build code with 200 nested braces inside defer + for loop body.
	// Keep under clang's default -fbracket-depth=256 after transpilation.
	char code[16384];
	int pos = 0;
	pos += snprintf(code + pos, sizeof(code) - pos,
	    "int main(void) {\n"
	    "    defer (void)0;\n"
	    "    for (int i = 0; i < 1; i++) {\n");
	for (int i = 0; i < 200; i++)
		pos += snprintf(code + pos, sizeof(code) - pos, "{\n");
	pos += snprintf(code + pos, sizeof(code) - pos, "(void)0;\n");
	for (int i = 0; i < 200; i++)
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
#ifndef _WIN32
	if (result.output)
		check_transpiled_output_compiles_and_runs(
		    result.output,
		    "scope depth overflow: transpiled output compiles",
		    "scope depth overflow: transpiled output runs");
#endif

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_defer_void_parens_return(void) {
        printf("\n--- Void function return with parens ---\n");

        const char *code =
            "#include <stdio.h>\n"
            "void void_helper(void) {}\n"
            "void (func_with_parens)(void) {\n"
            "    defer printf(\"test\\n\");\n"
            "    return void_helper();\n"
            "}\n"
            "int main() {\n"
            "    func_with_parens();\n"
            "    return 0;\n"
            "}\n";

        char *path = create_temp_file(code);
        PrismResult result = prism_transpile_file(path, prism_defaults());
        CHECK_EQ(result.status, PRISM_OK, "void with parens: transpiles OK");

        if (result.output) {
#ifndef _WIN32
                check_transpiled_output_compiles_and_runs(
                    result.output,
                    "void with parens: compiles without void deduction failure",
                    "void with parens: runs successfully");
#endif
        }
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

static void test_defer_in_comma_expr_rejected(void) {
        check_defer_transpile_rejects(
            "int main(void) { int x = (defer (void)0, 1); return x; }\n",
            "defer_in_comma_reject.c",
            "defer in comma expression rejected",
            "cannot be at top level"); 
}

static void test_defer_varname_return_value_dropped(void) {
	/* register_decl_shadows only shadows names matching
	 * is_typedef_heuristic(), so 'int defer = 5;' does not register 'defer'
	 * as a shadow.  In the return statement, handle_defer_keyword fires,
	 * consuming 'defer' as the start of a deferred statement and emitting
	 * 'return ;' — the return value is silently lost. */
	const char *code =
	    "int f(void) {\n"
	    "    int defer = 5;\n"
	    "    return defer;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_varname_return.c", prism_defaults());
	/* If the bug is present, output contains 'return ;' */
	CHECK(r.output == NULL || strstr(r.output, "return ;") == NULL,
	      "defer-varname: 'return defer;' must not become 'return ;'");
	prism_free(&r);
}

static void test_defer_label_duplication_rejected(void) {
	/* validate_defer_statement allows labeled statements inside
	 * defer blocks.  When the defer body is copy-pasted to each exit
	 * point (return, fall-through), any labels inside the block are
	 * duplicated, causing the downstream C compiler to hard-fail with
	 * "duplicate label" errors. */
	const char *code =
	    "int f(int x) {\n"
	    "    defer { my_label: x++; }\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_label_dup.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		/* If transpilation succeeded, the label must not appear more than once. */
		const char *first = strstr(r.output, "my_label:");
		const char *second = first ? strstr(first + 1, "my_label:") : NULL;
		CHECK(second == NULL,
		      "defer-label-dup: label inside defer pasted to multiple exit points");
	} else {
		/* Rejection is also acceptable (label banned inside defer). */
		CHECK(1, "defer-label-dup: label inside defer pasted to multiple exit points");
	}
	prism_free(&r);
}

static void test_defer_soft_keyword_labels_rejected(void) {
	check_defer_transpile_rejects(
	    "int f(int x) {\n"
	    "    defer { raw: x++; }\n"
	    "    if (x) return x;\n"
	    "    return x + 1;\n"
	    "}\n",
	    "defer_raw_label.c",
	    "defer soft label: raw rejected",
	    "labels inside defer blocks");
	check_defer_transpile_rejects(
	    "int f(int x) {\n"
	    "    defer { defer: x++; }\n"
	    "    if (x) return x;\n"
	    "    return x + 1;\n"
	    "}\n",
	    "defer_defer_label.c",
	    "defer soft label: defer rejected",
	    "labels inside defer blocks");
	check_defer_transpile_rejects(
	    "int f(int x) {\n"
	    "    defer { orelse: x++; }\n"
	    "    if (x) return x;\n"
	    "    return x + 1;\n"
	    "}\n",
	    "defer_orelse_label.c",
	    "defer soft label: orelse rejected",
	    "labels inside defer blocks");
}

static void test_defer_soft_keyword_control_condition_rejected(void) {
	check_defer_transpile_rejects(
	    "int f(int x) {\n"
	    "    int raw = x;\n"
	    "    if (defer raw) return 1;\n"
	    "    return 0;\n"
	    "}\n",
	    "defer_raw_condition.c",
	    "defer soft condition: raw operand rejected",
	    "defer cannot appear inside control statement parentheses");
	check_defer_transpile_rejects(
	    "int f(int x) {\n"
	    "    int bool = x;\n"
	    "    if (defer bool) return 1;\n"
	    "    return 0;\n"
	    "}\n",
	    "defer_bool_condition.c",
	    "defer soft condition: bool operand rejected",
	    "defer cannot appear inside control statement parentheses");
}

static void test_defer_variable_shadowing_binds_wrong_scope(void) {
	/* FIX: Prism copy-pastes defer token streams to each exit point.
	 * When a captured variable is shadowed in an inner scope, the pasted
	 * defer body would bind to the wrong variable (UAF / double-free).
	 * Prism now rejects the shadowing declaration at compile time. */
	const char *code =
	    "void tracked_free(void *p);\n"
	    "int f(void) {\n"
	    "    void *ptr = (void*)111;\n"
	    "    defer tracked_free(ptr);\n"
	    "    {\n"
	    "        void *ptr = (void*)222;\n"
	    "        return 0;\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_shadow_uaf.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "defer-shadow-uaf: shadowing a defer-captured variable must be rejected");
	CHECK(r.error_msg && strstr(r.error_msg, "shadows"),
	      "defer-shadow-uaf: error message mentions 'shadows'");
	prism_free(&r);
}

static void test_defer_safe_shadow_no_exit(void) {
	/* Safe shadow: inner scope redeclares a defer-captured variable but
	 * never exits the outer scope (no return/goto/break/continue).
	 * The inner variable goes out of scope at }, and the defer at end-of-
	 * outer-block correctly binds to the outer variable.  This must compile. */
	const char *code =
	    "void use(int x);\n"
	    "void f(void) {\n"
	    "    int count = 0;\n"
	    "    defer use(count);\n"
	    "    for (int i = 0; i < 10; i++) {\n"
	    "        int count = i * 2;\n"
	    "        use(count);\n"
	    "    }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_safe_shadow.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "defer-safe-shadow: safe shadow without control-flow exit must compile");
	prism_free(&r);
}

static void test_defer_shadow_struct_member_false_positive(void) {
	/* check_defer_var_shadow matches identifiers by string alone,
	 * without checking if the token follows a member-access operator
	 * (. or ->).  If a defer body contains device->x (a struct member),
	 * declaring 'int x' in an inner scope falsely triggers a shadow
	 * conflict even though the defer never captures a standalone 'x'. */
	const char *code =
	    "typedef struct { int x; } Device;\n"
	    "void *alloc(int);\n"
	    "void release(void *);\n"
	    "void f(void) {\n"
	    "    Device *dev = alloc(sizeof(Device));\n"
	    "    dev->x = 42;\n"
	    "    defer { dev->x = 0; release(dev); }\n"
	    "    {\n"
	    "        int x = 10;\n"
	    "        (void)x;\n"
	    "        return;\n"
	    "    }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_shadow_member.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "defer-shadow-member: struct member in defer must not trigger false shadow");
	prism_free(&r);
}

static void test_defer_shadow_overflow_silent_drop(void) {
	/* register_decl_shadow silently drops entry #257 when the static
	 * defer_shadows[256] array is full.  If a break/continue only unwinds
	 * defers that the dropped shadow refers to, check_defer_shadow_at_exit
	 * cannot see the conflict and the transpiled code silently operates on
	 * the wrong (shadowed) variable.
	 *
	 * Repro: 256 outer defers capture v0..v255; one loop defer captures
	 * v256.  An inner scope shadows all 257 variables and breaks.  The
	 * break unwinds only the loop defer (v256), but shadow entry #257
	 * was dropped so the error is missed. */
	char buf[65536];
	int off = 0;
	off += snprintf(buf + off, sizeof(buf) - off, "void test(void) {\n");
	for (int i = 0; i < 256; i++)
		off += snprintf(buf + off, sizeof(buf) - off,
		    "  int v%d = 0; defer v%d++;\n", i, i);
	off += snprintf(buf + off, sizeof(buf) - off,
	    "  int v256 = 0;\n"
	    "  for (int _i = 0; _i < 1; _i++) {\n"
	    "    defer v256++;\n"
	    "    {\n");
	for (int i = 0; i < 256; i++)
		off += snprintf(buf + off, sizeof(buf) - off,
		    "      int v%d = 1;\n", i);
	off += snprintf(buf + off, sizeof(buf) - off,
	    "      int v256 = 1;\n"
	    "      break;\n"
	    "    }\n"
	    "  }\n"
	    "}\n");
	PrismResult r = prism_transpile_source(buf, "defer_shadow_overflow.c",
	                                       prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "defer-shadow-overflow: 257th shadow must not be silently dropped");
	CHECK(r.error_msg && strstr(r.error_msg, "shadow"),
	      "defer-shadow-overflow: error message mentions 'shadow'");
	prism_free(&r);
}

// Regression: ctx->ret_counter is shared between __prism_ret_N (return-with-defer
// temp) and __prism_oe_N (orelse hoist temp). emit_return_body emitted
// `__prism_ret_<N> = (...)` with the counter value BEFORE emit_all_defers(),
// then `return __prism_ret_<N>` AFTER the defer body had already bumped the
// counter via a bare orelse inside the deferred body. Result: the return
// referenced an undeclared identifier, producing invalid C.
static void test_defer_body_orelse_counter_desync(void) {
	PrismResult r = prism_transpile_source(
	    "int main(void) {\n"
	    "    int x = 0;\n"
	    "    defer x = 5 orelse 3;\n"
	    "    return 0;\n"
	    "}\n",
	    "defer_orelse_counter.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "defer-orelse-counter: transpile succeeds");
	if (r.output) {
		// The bug emits `return __prism_ret_1;` while only
		// `__prism_ret_0 = ...` is declared. The output must not
		// reference any __prism_ret_<N> identifier that was not
		// first declared with ` = `.
		const char *p = r.output;
		while ((p = strstr(p, "return __prism_ret_")) != NULL) {
			const char *digits = p + strlen("return __prism_ret_");
			int n = 0, saw = 0;
			while (*digits >= '0' && *digits <= '9') {
				n = n * 10 + (*digits - '0'); digits++; saw = 1;
			}
			CHECK(saw, "defer-orelse-counter: return reference parses");
			char needle[64];
			snprintf(needle, sizeof(needle),
			         "__prism_ret_%d = ", n);
			CHECK(strstr(r.output, needle) != NULL,
			      "defer-orelse-counter: each `return __prism_ret_N` has a matching `__prism_ret_N = ` declaration");
			p = digits;
		}
	}
	prism_free(&r);
}

// Regression: Phase 1D walks braceless defer bodies in the `!at_stmt_start`
// branch, so the type keyword inside `defer int t = 0 orelse c;` never gets
// P1_IS_DECL annotated. Pass 2's try_zero_init_decl bails at the P1_IS_DECL
// gate, so emit_deferred_orelse takes over and treats `int t` as the bare-
// orelse LHS, emitting `__typeof__(int t) __prism_oe_N = ...` — a syntax
// error in C. The block-form variant works because Phase 1D enters the
// block via handle_open_brace and processes the declaration normally.
// Pass 1 rejects the braceless-with-orelse-init case with a clear message
// pointing the user at `defer { ... }`.
static void test_defer_braceless_decl_orelse_rejected(void) {
	PrismResult r = prism_transpile_source(
	    "int main(void) {\n"
	    "    int c = 0;\n"
	    "    defer int t = 0 orelse c;\n"
	    "    return 0;\n"
	    "}\n",
	    "defer_braceless_decl_orelse.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "defer-braceless-decl-orelse: must reject at Phase 1");
	CHECK(r.error_msg && strstr(r.error_msg, "wrap the defer body in braces"),
	      "defer-braceless-decl-orelse: error message points to braces");
	prism_free(&r);

	// Block form must still work.
	r = prism_transpile_source(
	    "int main(void) {\n"
	    "    int c = 0;\n"
	    "    defer { int t = 0 orelse c; (void)t; }\n"
	    "    return 0;\n"
	    "}\n",
	    "defer_block_decl_orelse.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "defer-block-decl-orelse: block form accepted");
	if (r.output) {
		CHECK(strstr(r.output, "__typeof__( int t)") == NULL &&
		      strstr(r.output, "__typeof__(int t)") == NULL,
		      "defer-block-decl-orelse: no invalid typeof(decl) emitted");
	}
	prism_free(&r);
}

// Regression: emit_orelse_action's block path consumes the trailing `;`,
// then emit_deferred_orelse advances past the defer body's end boundary.
// emit_statements' `tok != end` loop then overshoots, spilling user tokens
// (the statements following the defer) into the emitted defer body.
static void test_defer_body_orelse_block_overshoot(void) {
	PrismResult r = prism_transpile_source(
	    "#include <stdio.h>\n"
	    "int main(void) {\n"
	    "    defer 0 orelse { (void)0; };\n"
	    "    return 0;\n"
	    "}\n",
	    "defer_orelse_block_overshoot.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "defer-orelse-overshoot: transpile succeeds");
	if (r.output) {
		// Count top-level `return __prism_ret_N` references; must
		// have a matching `__prism_ret_N = ` declaration.
		const char *p = r.output;
		while ((p = strstr(p, "return __prism_ret_")) != NULL) {
			const char *digits = p + strlen("return __prism_ret_");
			int n = 0, saw = 0;
			while (*digits >= '0' && *digits <= '9') {
				n = n * 10 + (*digits - '0'); digits++; saw = 1;
			}
			CHECK(saw, "defer-orelse-overshoot: return reference parses");
			char needle[64];
			snprintf(needle, sizeof(needle), "__prism_ret_%d = ", n);
			CHECK(strstr(r.output, needle) != NULL,
			      "defer-orelse-overshoot: each return has matching declaration");
			p = digits;
		}
	}
	prism_free(&r);
}

static void test_defer_body_bare_orelse_return_not_rejected(void) {
	/* validate_defer_statement calls skip_to_semicolon for statements
	 * that don't start with a keyword (like 'get() orelse return;'), so the
	 * 'return' inside the bare-orelse action is never seen.
	 * emit_deferred_range also has no bare-orelse processing.
	 * Result: 'get() orelse return;' is emitted verbatim into the deferred
	 * block — it is not valid C and is not rejected by the transpiler. */
	check_defer_transpile_rejects(
	    "int *get(void);\n"
	    "void f(void) {\n"
	    "    defer {\n"
	    "        get() orelse return;\n"
	    "    };\n"
	    "}\n",
	    "defer_bare_orelse_return.c",
	    "defer-bare-orelse: 'return' inside defer via orelse must be rejected",
	    NULL);
}

static void test_defer_paren_wrapped_orelse_smuggling(void) {
	printf("\n--- defer paren-wrapped orelse smuggling ---\n");
	// (0 orelse return) — Phase 1F must recurse into parens
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    defer {\n"
	    "        int status = (0 orelse return);\n"
	    "    };\n"
	    "}\n",
	    "defer_poe1.c",
	    "defer-paren-orelse: return in parens must be rejected",
	    "return");
	// nested parens: ((0 orelse goto L))
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    defer {\n"
	    "        int x = ((0 orelse goto done));\n"
	    "    };\n"
	    "    done: ;\n"
	    "}\n",
	    "defer_poe2.c",
	    "defer-paren-orelse: goto in nested parens must be rejected",
	    NULL);
	// bracket-wrapped: arr[0 orelse return]
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    int arr[2];\n"
	    "    defer {\n"
	    "        int x = arr[0 orelse return];\n"
	    "    };\n"
	    "}\n",
	    "defer_poe3.c",
	    "defer-bracket-orelse: return in brackets must be rejected",
	    "return");
	// paren-wrapped orelse block: (0 orelse { return; })
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    defer {\n"
	    "        int status = (0 orelse { return; });\n"
	    "    };\n"
	    "}\n",
	    "defer_poe4.c",
	    "defer-paren-orelse-block: return in orelse block in parens must be rejected",
	    "return");
}

#ifndef _WIN32
static void test_defer_paren_vfork_bypass(void) {
	// (vfork)() is equivalent to vfork() but bypasses the strict `vfork(` check
	const char *code =
	    "typedef int pid_t;\n"
	    "pid_t vfork(void);\n"
	    "void cleanup(void);\n"
	    "int f(void) {\n"
	    "    defer { cleanup(); }\n"
	    "    pid_t pid = (vfork)();\n"
	    "    return pid;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_paren_vfork.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "defer-paren-vfork: (vfork)() must be rejected like vfork()");
	prism_free(&r);
}

static void test_defer_vfork_funcptr_bypass(void) {
	// Intra-TU vfork aliasing: vfork returned via function pointer.
	// The aggressive token-level taint now catches any vfork reference
	// in a function body, so get_trigger is tainted and main inherits
	// the taint via transitive propagation.
	const char *code =
	    "typedef int pid_t;\n"
	    "pid_t vfork(void);\n"
	    "void cleanup(void);\n"
	    "pid_t (*get_trigger(void))(void) { return vfork; }\n"
	    "int main(void) {\n"
	    "    pid_t (*trigger)(void) = get_trigger();\n"
	    "    defer { cleanup(); }\n"
	    "    (void)trigger;\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_vfork_fptr.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "defer-vfork-funcptr: vfork via function pointer must be rejected");
	prism_free(&r);
}

static void test_defer_vfork_local_alias_bypass(void) {
	// Direct intra-function aliasing: fp = vfork; fp();
	// This was the original attack vector that bypassed the old
	// call-site-only (vfork followed by '(') detection.
	const char *code =
	    "typedef int pid_t;\n"
	    "pid_t vfork(void);\n"
	    "void cleanup(void);\n"
	    "void test(void) {\n"
	    "    defer { cleanup(); }\n"
	    "    pid_t (*stealth)(void) = vfork;\n"
	    "    if (stealth() == 0) return;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_vfork_alias.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "defer-vfork-local-alias: fp=vfork;fp() must be rejected");
	prism_free(&r);
}


static void test_defer_vfork_reference_false_positive(void) {
	// get_vfork_ptr returns a pointer to vfork but never calls it.
	// main() calls get_vfork_ptr() and uses defer — this is safe in
	// theory, but the token-level taint scanner taints any function
	// whose body mentions vfork (to block intra-TU fp aliasing attacks
	// like `fp = vfork; fp();`).  Transitive propagation then taints
	// main().  This is an accepted false positive.
	const char *code =
	    "typedef int pid_t;\n"
	    "pid_t vfork(void);\n"
	    "void cleanup(void) {}\n"
	    "pid_t (*get_vfork_ptr(void))(void) { return vfork; }\n"
	    "int main(void) {\n"
	    "    pid_t (*fp)(void) = get_vfork_ptr();\n"
	    "    defer { cleanup(); }\n"
	    "    (void)fp;\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_vfork_ref_fp.c", prism_defaults());
	// Expected: rejected.  The aggressive vfork taint catches bare vfork
	// references to prevent function-pointer aliasing bypasses.
	CHECK(r.status != PRISM_OK,
	      "defer-vfork-ref-false-positive: vfork reference must taint transitively");
	prism_free(&r);
}
#endif

void test_defer_backward_goto_into_scope_rejected(void) {
	// Backward goto into a scope with defer should be rejected.
	// Without this check, the defer cleanup fires on re-entry via goto,
	// even though the defer's setup (e.g. malloc) was skipped → double-free.
	check_defer_transpile_rejects(
	    "void do_work(int *p);\n"
	    "void *malloc(unsigned long);\n"
	    "void free(void *);\n"
	    "void f(void) {\n"
	    "    {\n"
	    "        int *p = malloc(10);\n"
	    "        defer free(p);\n"
	    "    target:\n"
	    "        do_work(p);\n"
	    "    }\n"
	    "    goto target;\n"
	    "}\n",
	    "defer_backward_goto_into_scope.c",
	    "backward goto into defer scope rejected",
	    "skip over this defer");
}

void test_defer_shadow_in_for_init(void) {
	// A variable declared in a for-init that shadows a name captured
	// by an outer defer must be rejected when the for body contains an exit.
	check_defer_transpile_rejects(
	    "void foo(int);\n"
	    "void f(void) {\n"
	    "    int x = 1;\n"
	    "    defer foo(x);\n"
	    "    for (int x = 0; x < 10; x++) {\n"
	    "        if (x == 3) return;\n"
	    "    }\n"
	    "}\n",
	    "defer_shadow_for_init.c",
	    "defer shadow in for-init rejected",
	    "shadows");
}

static void test_cfg_hash_table_saturation_bypass(void) {
	// p1_verify_cfg's label hash table is capped at 8192 slots.
	// When a function has >8192 labels, late-inserted labels are silently
	// dropped (open-addressing probe exhausts without finding empty slot).
	// A backward goto to a dropped label gets li=-1, misclassified as
	// forward, queued in fwd[], never resolved → CFG check bypassed.
	//
	// Baseline: backward goto from outer scope into inner scope with
	// defer is rejected.  With 9000+ filler labels, the same pattern
	// silently passes because 'target' is dropped from the hash.

	// Build source: 9000 filler labels, then { defer ...; target: }, then goto
	int n = 9000;
	size_t cap = (size_t)n * 40 + 512;
	char *code = malloc(cap);
	if (!code) { CHECK(0, "cfg-hash-sat: malloc"); return; }
	int pos = 0;
	pos += snprintf(code + pos, cap - pos, "#include <stdio.h>\n");
	pos += snprintf(code + pos, cap - pos, "void f(int x) {\n");
	for (int i = 1; i <= n; i++)
		pos += snprintf(code + pos, cap - pos, "filler_%d: (void)0;\n", i);
	pos += snprintf(code + pos, cap - pos,
		"{\n"
		"  defer printf(\"inner cleanup\");\n"
		"  target: (void)0;\n"
		"}\n");
	for (int i = 1; i <= n; i++)
		pos += snprintf(code + pos, cap - pos, "goto filler_%d;\n", i);
	pos += snprintf(code + pos, cap - pos,
		"if (x) goto target;\n"
		"}\n"
		"int main(void) { return 0; }\n");

	PrismResult r = prism_transpile_source(code, "cfg_hash_sat.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "cfg-hash-saturation: backward goto into defer scope must be "
	      "rejected even with >8192 labels (hash table overflow)");
	if (r.status != PRISM_OK && r.error_msg)
		CHECK(strstr(r.error_msg, "skip over this defer") != NULL,
		      "cfg-hash-saturation: error mentions defer skip");
	prism_free(&r);
	free(code);
}

static void test_defer_fnptr_return_type_overwrite(void) {
	/* capture_function_return_type is called on every type token at
	   block_depth 0. For void (*signal_fn(int, void(*func)(int)))(int),
	   the 'void' in the func parameter re-triggers capture and overwrites
	   the correct return type with a broken one (no suffix). The defer
	   return-value temp then gets the wrong type, producing invalid C. */
	PrismResult r = prism_transpile_source(
	    "void cleanup(void);\n"
	    "void (*signal_fn(int sig, void (*func)(int)))(int) {\n"
	    "    defer cleanup();\n"
	    "    return func;\n"
	    "}\n",
	    "defer_fnptr_return_type.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
		 "defer fnptr return type: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "typedef") != NULL,
		      "defer fnptr return type: generates typedef for complex return");
		CHECK(strstr(r.output, "__prism_ret_t_") != NULL,
		      "defer fnptr return type: uses named typedef");
	}
	prism_free(&r);
}

static void test_braceless_switch_defer_false_positive(void) {
	/* braceless switch (no braces around body) had no P1K_SWITCH entry,
	   so case/default labels saw sw_sid=0, causing ALL preceding defers in
	   the function to be flagged as "skipped by switch fallthrough". */
	PrismResult r = prism_transpile_source(
	    "void cleanup(void);\n"
	    "void f(int x) { defer cleanup(); switch (x) default: break; }\n",
	    "braceless_switch_defer.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
		 "braceless-switch-defer: no false positive error");
	prism_free(&r);

	/* Sequential defer + braceless switch with case */
	r = prism_transpile_source(
	    "void cleanup(void);\n"
	    "void g(int x) {\n"
	    "    defer cleanup();\n"
	    "    switch (x) case 1: x++;\n"
	    "}\n",
	    "braceless_switch_case_defer.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
		 "braceless-switch-case-defer: no false positive error");
	prism_free(&r);
}

// Braceless inner switch's case label triggers handle_case_default
// which calls find_switch_scope → finds enclosing BRACED switch (the only
// one with a SCOPE_BLOCK), and incorrectly resets the defer stack for that
// outer switch, silently dropping defers registered between the outer case
// and the braceless inner switch.
static void test_braceless_switch_defer_drop(void) {
	PrismResult r = prism_transpile_source(
	    "void cleanup(void);\n"
	    "void f(int x, int y) {\n"
	    "    switch (x) {\n"
	    "        case 1:\n"
	    "            {\n"
	    "                defer cleanup();\n"
	    "                switch (y) case 2: y++;\n"
	    "            }\n"
	    "            break;\n"
	    "        case 3:\n"
	    "            break;\n"
	    "    }\n"
	    "}\n",
	    "braceless_switch_defer_drop.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
		 "braceless-switch-defer-drop: transpiles OK");
	CHECK(r.output && strstr(r.output, "cleanup()") != NULL,
	      "braceless-switch-defer-drop: defer cleanup() not silently dropped");
	prism_free(&r);
}

// check_defer_var_shadow does a flat scan of defer body tokens without
// tracking brace depth. A variable declared inside an inner { } block within
// the defer body matches the outer declaration name, producing a false positive.
static void test_defer_shadow_inner_block_false_positive(void) {
	printf("\n--- Defer shadow inner-block false positive ---\n");

	// The defer body has { int tmp = 1; } in an inner block.
	// Declaring tmp in a subsequent scope should NOT be flagged as a shadow
	// because the defer-internal tmp is local and can't conflict.
	const char *code =
	    "void f(void) {\n"
	    "    defer {\n"
	    "        { int tmp = 1; (void)tmp; }\n"
	    "    }\n"
	    "    {\n"
	    "        int tmp = 2;\n"
	    "        (void)tmp;\n"
	    "        return;\n"
	    "    }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_shadow_inner_block.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "defer-shadow-inner-block: inner { int tmp; } in defer must not flag outer tmp");
	prism_free(&r);

	// Verify that a real shadow (tmp used at the top level of the defer body)
	// is still caught.
	const char *code_real =
	    "int g(void);\n"
	    "void f2(void) {\n"
	    "    int tmp = g();\n"
	    "    defer { (void)tmp; }\n"
	    "    {\n"
	    "        int tmp = 99;\n"
	    "        (void)tmp;\n"
	    "        return;\n"
	    "    }\n"
	    "}\n";
	PrismResult r2 = prism_transpile_source(code_real, "defer_shadow_real.c", prism_defaults());
	CHECK(r2.status != PRISM_OK,
	      "defer-shadow-real: top-level tmp in defer must still be caught");
	prism_free(&r2);
}

// in_generic() pierces SCOPE_BLOCK — defer leaks raw
static void test_defer_in_generic_stmt_expr(void) {
	printf("\n--- Defer inside _Generic stmt-expr ---\n");

	// defer inside a block within a stmt-expr inside _Generic must be
	// transformed, not leaked raw.  in_generic() used to scan the full
	// scope stack, piercing SCOPE_BLOCK boundaries and suppressing
	// handle_defer_keyword dispatch.
	const char *code =
	    "void cleanup(void);\n"
	    "int bar(int);\n"
	    "void foo(void) {\n"
	    "    bar(_Generic(1, int: ({ int r; { defer cleanup(); r = 42; } r; })));\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "generic_stmtexpr.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "defer inside _Generic stmt-expr must not be rejected");
	// The output must NOT contain the raw 'defer' keyword.
	CHECK(!strstr(r.output, "defer"),
	      "defer keyword must not leak raw into output inside _Generic stmt-expr");
	// cleanup() must appear in the output (emitted by defer expansion).
	CHECK(strstr(r.output, "cleanup()"),
	      "cleanup() must be emitted by defer expansion inside _Generic stmt-expr");
	prism_free(&r);
}

static void test_defer_user_noreturn_no_warning(void) {
	/* Prism detects hardcoded noreturn functions (abort, exit, _Exit,
	 * quick_exit, thrd_exit, __builtin_trap, __builtin_unreachable) via
	 * TT_NORETURN_FN and warns when they're called with active defers.
	 * But user-defined noreturn functions declared with _Noreturn,
	 * __attribute__((noreturn)), or [[noreturn]] are NOT recognized.
	 * If a developer writes:
	 *   _Noreturn void my_panic(const char *msg);
	 *   void f(void) { defer cleanup(); my_panic("fatal"); }
	 * Prism silently accepts this without warning, even though defers will
	 * never run.  This is dangerous in safety-critical / kernel code where
	 * defer is relied upon for resource cleanup.
	 *
	 * The test captures stderr from lib-mode transpilation and verifies
	 * that calling a _Noreturn-declared function with active defers
	 * produces the same warning as calling abort(). */

	/* Baseline: abort() with active defer DOES warn */
	const char *code_abort =
	    "void cleanup(void);\n"
	    "void f(void) {\n"
	    "    defer cleanup();\n"
	    "    abort();\n"
	    "}\n";
	int pipefd[2];
	pipe(pipefd);
	int saved = dup(STDERR_FILENO);
	dup2(pipefd[1], STDERR_FILENO);
	PrismResult r1 = prism_transpile_source(code_abort, "abort_warn.c",
						prism_defaults());
	fflush(stderr);
	dup2(saved, STDERR_FILENO);
	close(saved);
	close(pipefd[1]);
	char buf[4096];
	int n = read(pipefd[0], buf, sizeof(buf) - 1);
	buf[n > 0 ? n : 0] = '\0';
	close(pipefd[0]);
	CHECK(r1.status == PRISM_OK, "noreturn-warn: abort transpile succeeds");
	CHECK(n > 0 && strstr(buf, "warning") != NULL,
	      "noreturn-warn: abort() with defer produces warning (baseline)");
	prism_free(&r1);

	/* Bug case: _Noreturn function with active defer gets NO warning */
	const char *code_noreturn =
	    "_Noreturn void my_panic(void);\n"
	    "void cleanup(void);\n"
	    "void f(void) {\n"
	    "    defer cleanup();\n"
	    "    my_panic();\n"
	    "}\n";
	pipe(pipefd);
	saved = dup(STDERR_FILENO);
	dup2(pipefd[1], STDERR_FILENO);
	PrismResult r2 = prism_transpile_source(code_noreturn, "noreturn_warn.c",
						prism_defaults());
	fflush(stderr);
	dup2(saved, STDERR_FILENO);
	close(saved);
	close(pipefd[1]);
	n = read(pipefd[0], buf, sizeof(buf) - 1);
	buf[n > 0 ? n : 0] = '\0';
	close(pipefd[0]);
	CHECK(r2.status == PRISM_OK, "noreturn-warn: _Noreturn transpile succeeds");
	CHECK(n > 0 && strstr(buf, "warning") != NULL,
	      "noreturn-warn: _Noreturn user function with active defer must "
	      "warn ('_Noreturn void my_panic()' is not in hardcoded "
	      "TT_NORETURN_FN list; Phase 1 body scanner ignores _Noreturn / "
	      "__attribute__((noreturn)) specifiers on declarations)");
	prism_free(&r2);
}

// match_ch(prev, '*') misclassifies pointer dereference as
// a local declaration inside nested defer blocks, bypassing shadow detection.
static void test_defer_shadow_deref_bypass(void) {
	printf("\n--- Defer shadow * dereference bypass ---\n");

	const char *code =
	    "int *get_lock(void);\n"
	    "int *get_dummy(void);\n"
	    "int f(int condition) {\n"
	    "    int *target_lock = get_lock();\n"
	    "    defer {\n"
	    "        {\n"
	    "            *target_lock = 0;\n"
	    "        }\n"
	    "    }\n"
	    "    if (condition) {\n"
	    "        int *target_lock = get_dummy();\n"
	    "        return *target_lock;\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_shadow_deref.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "defer-shadow-deref: *target_lock dereference must not suppress shadow detection");
	CHECK(r.error_msg && strstr(r.error_msg, "shadows"),
	      "defer-shadow-deref: error message mentions 'shadows'");
	prism_free(&r);
}

// casts like (struct X *) inside nested defer blocks set
// in_decl=true which persists past ')', causing subsequent identifier uses
// to be misclassified as local declarations — shadow goes undetected.
static void test_defer_shadow_cast_bypass(void) {
	printf("\n--- Defer shadow cast bypass ---\n");

	// The defer body uses 'ptr' only at brace_depth > 1, after a cast.
	// (struct Node *) sets in_decl=true, which persists past ')' and
	// causes 'ptr' to be misclassified as a local declaration.
	const char *code =
	    "struct Node;\n"
	    "void release(struct Node *p);\n"
	    "int f(void) {\n"
	    "    struct Node *ptr = 0;\n"
	    "    defer {\n"
	    "        if (1) {\n"
	    "            release((struct Node *)ptr);\n"
	    "        }\n"
	    "    }\n"
	    "    {\n"
	    "        struct Node *ptr = 0;\n"
	    "        return 1;\n"
	    "    }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_shadow_cast.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "defer-shadow-cast: cast (struct Node *) must not blind shadow detection");
	CHECK(r.error_msg && strstr(r.error_msg, "shadows"),
	      "defer-shadow-cast: error message mentions 'shadows'");
	prism_free(&r);
}

// comma-separated declarators in nested defer blocks cause
// false-positive shadow errors (prev is ',' which isn't a type keyword).
static void test_defer_shadow_comma_decl_false_positive(void) {
	printf("\n--- Defer shadow comma-decl false positive ---\n");

	// The defer body locally declares 'int a = 1, b = 2;' in a nested block.
	// 'b' at the top level is used only inside that declaration.
	// A later 'int b = 99; return;' must NOT trigger a shadow error
	// because the defer's 'b' is local to the inner block.
	const char *code =
	    "void use(int x);\n"
	    "void f(void) {\n"
	    "    int b = 10;\n"
	    "    defer {\n"
	    "        {\n"
	    "            int a = 1, b = 2;\n"
	    "            use(a + b);\n"
	    "        }\n"
	    "    }\n"
	    "    {\n"
	    "        int b = 99;\n"
	    "        return;\n"
	    "    }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_shadow_comma.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "defer-shadow-comma: 'int a, b' in nested defer block must not false-positive");
	prism_free(&r);
}

// brace_depth > 1 skip in check_defer_var_shadow causes
// captured variables used only inside nested blocks of the defer body to
// be missed, allowing silent shadowing + wrong-variable binding.
static void test_defer_shadow_depth_bypass(void) {
	printf("\n--- Defer shadow depth > 1 bypass ---\n");

	// The defer body uses 'resource' only inside if (1) { ... } — at
	// brace_depth 2.  The old brace_depth > 1 skip misses this, so
	// an inner-scope shadow + return silently binds the wrong variable.
	const char *code =
	    "void release(int *p);\n"
	    "int f(void) {\n"
	    "    int resource = 1;\n"
	    "    defer {\n"
	    "        if (1) {\n"
	    "            release(&resource);\n"
	    "        }\n"
	    "    }\n"
	    "    {\n"
	    "        int resource = 99;\n"
	    "        return 0;\n"
	    "    }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_shadow_depth.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "defer-shadow-depth: captured var at brace_depth > 1 must be detected");
	CHECK(r.error_msg && strstr(r.error_msg, "shadows"),
	      "defer-shadow-depth: error message mentions 'shadows'");
	prism_free(&r);
}

static void test_defer_shadow_enum_bypass(void) {
	printf("\n--- Defer shadow enum constant bypass ---\n");

	// Enum constants declared inside an inner scope shadow a variable
	// captured in a defer body.  check_defer_var_shadow is only called
	// from process_declarators (variable declarations), never from
	// parse_enum_constants, so the enum constant is invisible to it.
	const char *code =
	    "void cleanup(int v);\n"
	    "int f(int condition) {\n"
	    "    int status = 42;\n"
	    "    defer cleanup(status);\n"
	    "    if (condition) {\n"
	    "        enum { status = -12 };\n"
	    "        return 0;\n"
	    "    }\n"
	    "    return 1;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_shadow_enum.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "defer-shadow-enum: enum constant must not silently shadow captured var");
	CHECK(r.error_msg && strstr(r.error_msg, "shadows"),
	      "defer-shadow-enum: error message mentions 'shadows'");
	prism_free(&r);
}

static void test_defer_shadow_typedef_bypass(void) {
	printf("\n--- Defer shadow typedef bypass ---\n");

	// A local typedef with the same name as a variable captured in a
	// defer body silently changes sizeof(x) from sizeof(int) to
	// sizeof(double).  parse_typedef_declaration never calls
	// check_defer_var_shadow.
	const char *code =
	    "void log_size(int sz);\n"
	    "int f(void) {\n"
	    "    int x = 42;\n"
	    "    defer log_size(sizeof(x));\n"
	    "    {\n"
	    "        typedef double x;\n"
	    "        x val;\n"
	    "        (void)val;\n"
	    "        return 0;\n"
	    "    }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_shadow_typedef.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "defer-shadow-typedef: typedef must not silently shadow captured var");
	CHECK(r.error_msg && strstr(r.error_msg, "shadows"),
	      "defer-shadow-typedef: error message mentions 'shadows'");
	prism_free(&r);
}

// The check for "defer at end of stmt-expr" only fires when the next
// non-noise token after the inner block's closing '}' is the outer '}' of
// the stmt-expr. An empty statement (;) or label between the two braces
// defeats the detection. The defer then emits after the last expression in
// the inner block, overwriting the stmt-expr's return value with the defer's
// result type (void → compile error, non-void → silent wrong value).
static void test_defer_stmt_expr_empty_stmt_bypass(void) {
	printf("\n--- defer stmt-expr empty-statement bypass ---\n");

	// Variant 1: empty statement between inner } and outer }
	{
		const char *code =
		    "void cleanup(void);\n"
		    "int work(void);\n"
		    "void test(void) {\n"
		    "    int x = ({\n"
		    "        int r = work();\n"
		    "        { defer cleanup(); r; }\n"
		    "        ;\n"
		    "    });\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "stmtexpr_empty_bypass.c", prism_defaults());
		// Must either reject with error or produce output where cleanup()
		// does NOT appear after "r;" in the last block (overwriting return value).
		if (r.status == PRISM_OK && r.output) {
			// If it compiled, the defer must NOT have been pasted after the
			// expression r inside the inner block. Search for pattern:
			// "r; cleanup()" or "r;\ncleanup()" which indicates corruption.
			char *inner = r.output ? strstr(r.output, "r;") : NULL;
			bool corrupted = false;
			if (inner) {
				// Check if cleanup() follows r; within the same block
				char *after_r = inner + 2;
				while (*after_r == ' ' || *after_r == '\n' || *after_r == '\t') after_r++;
				if (strncmp(after_r, "cleanup()", 9) == 0)
					corrupted = true;
			}
			CHECK(!corrupted,
			      "stmtexpr-empty-bypass: defer must not paste cleanup() after "
			      "the return expression (void overwrite corrupts stmt-expr value)");
		} else {
			CHECK(r.status != PRISM_OK,
			      "stmtexpr-empty-bypass: rejection is acceptable");
		}
		prism_free(&r);
	}

	// Variant 2: label between inner } and outer }
	{
		const char *code =
		    "void cleanup(void);\n"
		    "int work(void);\n"
		    "void test(void) {\n"
		    "    int x = ({\n"
		    "        int r = work();\n"
		    "        { defer cleanup(); r; }\n"
		    "    end_expr:\n"
		    "    });\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "stmtexpr_label_bypass.c", prism_defaults());
		if (r.status == PRISM_OK && r.output) {
			char *inner = r.output ? strstr(r.output, "r;") : NULL;
			bool corrupted = false;
			if (inner) {
				char *after_r = inner + 2;
				while (*after_r == ' ' || *after_r == '\n' || *after_r == '\t') after_r++;
				if (strncmp(after_r, "cleanup()", 9) == 0)
					corrupted = true;
			}
			CHECK(!corrupted,
			      "stmtexpr-label-bypass: defer must not paste cleanup() after "
			      "the return expression (void overwrite corrupts stmt-expr value)");
		} else {
			CHECK(r.status != PRISM_OK,
			      "stmtexpr-label-bypass: rejection is acceptable");
		}
		prism_free(&r);
	}
}

// typeof(x) inside a nested block in a defer body sets in_decl = true,
// then when 'x' is encountered inside the typeof parens, it's treated as a
// local declaration target. This poisons decl_depth, causing a later
// 'int x = 5;' in the same block to be skipped by the shadow checker.
static void test_defer_typeof_shadow_bypass(void) {
	printf("\n--- Defer typeof Shadow Bypass ---\n");

	// typeof(x) in nested defer block must not poison shadow detection for x
	const char *code =
	    "int test(void) {\n"
	    "    int x = 10;\n"
	    "    defer {\n"
	    "        {\n"
	    "            typeof(x) tmp = x;\n"
	    "            (void)tmp;\n"
	    "        }\n"
	    "    }\n"
	    "    {\n"
	    "        int x = 5;\n"
	    "        if (x > 3) return x;\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n"
	    "int main(void) { test(); return 0; }\n";

	PrismResult r = prism_transpile_source(code, "typeof_shadow.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "typeof(x) in defer must not blind shadow detection for 'x'");
	prism_free(&r);

	// Also test __typeof__ variant
	const char *code2 =
	    "int test(void) {\n"
	    "    int x = 10;\n"
	    "    defer {\n"
	    "        {\n"
	    "            __typeof__(x) tmp = x;\n"
	    "            (void)tmp;\n"
	    "        }\n"
	    "    }\n"
	    "    {\n"
	    "        int x = 5;\n"
	    "        if (x > 3) return x;\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n"
	    "int main(void) { test(); return 0; }\n";

	PrismResult r2 = prism_transpile_source(code2, "dunder_typeof_shadow.c", prism_defaults());
	CHECK(r2.status != PRISM_OK,
	      "__typeof__(x) in defer must not blind shadow detection for 'x'");
	prism_free(&r2);
}

// check_enum_typedef_defer_shadow's comma-scan loop inside enum body
// does not skip over balanced groups (parens). An enum constant with a
// complex initializer like STATE_A = (0, x) has a comma inside parens;
// the scanner stops at that inner comma, then the next iteration picks up
// the identifier 'x' as if it were a new enum constant name, causing a
// false-positive defer shadow error.
static void test_defer_enum_initializer_comma(void) {
	printf("\n--- Enum Initializer Comma ---\n");

	// x appears inside enum initializer parens, NOT as an enum constant name
	const char *code =
	    "int test(void) {\n"
	    "    int x = 10;\n"
	    "    defer { (void)x; }\n"
	    "    {\n"
	    "        enum {\n"
	    "            HW_INIT = (0, 1),\n"
	    "            HW_READY\n"
	    "        };\n"
	    "        return HW_READY;\n"
	    "    }\n"
	    "}\n"
	    "int main(void) { return test(); }\n";

	PrismResult r = prism_transpile_source(code, "enum_comma.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "enum initializer with comma in parens must not hallucinate shadow");
	prism_free(&r);

	// Harder case: identifier inside parens matches the deferred variable
	const char *code2 =
	    "int test(void) {\n"
	    "    int x = 10;\n"
	    "    defer { (void)x; }\n"
	    "    {\n"
	    "        enum {\n"
	    "            HW_INIT = (0, x),\n"
	    "            HW_READY\n"
	    "        };\n"
	    "        return HW_READY;\n"
	    "    }\n"
	    "}\n"
	    "int main(void) { return test(); }\n";

	PrismResult r2 = prism_transpile_source(code2, "enum_comma2.c", prism_defaults());
	CHECK_EQ(r2.status, PRISM_OK,
	         "identifier inside enum initializer parens must not trigger false shadow");
	prism_free(&r2);
}

// double-nested block defer in stmt-expr bypasses detection.
// The check at handle_close_brace only looks at scope_depth-2 for is_stmt_expr.
// A double-nested block places the stmt_expr scope at depth-3, so the check
// doesn't fire.  The defer cleanup call (void) becomes the last expression in
// the statement expression → "void value not ignored as it ought to be".
static void test_defer_stmt_expr_double_nested_block_bypass(void) {
	printf("\n--- defer stmt-expr double-nested block bypass ---\n");

	const char *code =
	    "void cleanup(void);\n"
	    "int work(void);\n"
	    "int f(void) {\n"
	    "    return ({\n"
	    "        int y = 1;\n"
	    "        {\n"
	    "            {\n"
	    "                defer cleanup();\n"
	    "                y = work();\n"
	    "            }\n"
	    "        }\n"
	    "    });\n"
	    "}\n";
	check_defer_transpile_rejects(
	    code,
	    "defer_stmt_expr_double_nested.c",
	    "defer in double-nested block that is last stmt of stmt_expr must be rejected",
	    "statement expression");
}

static void test_c23_attr_computed_goto_defer_bypass(void) {
	printf("\n--- C23 attributed computed goto defer bypass ---\n");

	/* goto [[gnu::nomerge]] *ptr; bypasses computed goto detection in
	 * BOTH Phase 1D (has_computed_goto flag) and Pass 2 (handle_goto_keyword).
	 *
	 * Phase 1D checks:  tok_next(goto) == '*'  → match_ch fails on '['
	 * Pass 2 checks:    tok_next(goto) == '*'  → match_ch fails on '['
	 *
	 * Result: has_computed_goto is never set, so the CFG verifier does not
	 * reject the function.  In Pass 2, handle_goto_keyword falls through to
	 * identifier-goto handling, sees '[', and emits 'goto' without checking
	 * has_active_defers().  Active defers are NOT emitted before the jump.
	 *
	 * The C23 attribute [[...]] between goto and * is a valid C23 statement
	 * attribute.  GCC and Clang accept goto [[attr]] *ptr; as a computed
	 * goto with a statement attribute.
	 */

	/* Case 1: normal computed goto + defer → correctly rejected */
	check_defer_transpile_rejects(
	    "void cleanup(int *p) { (void)p; }\n"
	    "void f(void) {\n"
	    "    int x = 0;\n"
	    "    defer cleanup(&x);\n"
	    "    void *target = &&done;\n"
	    "    goto *target;\n"
	    "done:\n"
	    "    return;\n"
	    "}\n",
	    "c23_attr_goto_baseline.c",
	    "c23-attr-goto-baseline: plain 'goto *target' with defer is rejected",
	    "computed goto");

	/* Case 2: C23 attributed computed goto + defer → MUST ALSO be rejected */
	check_defer_transpile_rejects(
	    "void cleanup(int *p) { (void)p; }\n"
	    "void f(void) {\n"
	    "    int x = 0;\n"
	    "    defer cleanup(&x);\n"
	    "    void *target = &&done;\n"
	    "    goto [[gnu::nomerge]] *target;\n"
	    "done:\n"
	    "    return;\n"
	    "}\n",
	    "c23_attr_goto_bypass.c",
	    "c23-attr-goto-bypass: 'goto [[attr]] *target' with defer must be "
	    "rejected (C23 attribute must not blind computed goto detection)",
	    "computed goto");

	/* Case 3: C23 attributed computed goto WITHOUT defer and WITHOUT zeroinit
	 * → should succeed (no defers to protect, no zeroinit concern) */
	{
		PrismFeatures feat = prism_defaults();
		feat.defer = false;
		feat.zeroinit = false;
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    void *target = &&done;\n"
		    "    goto [[gnu::nomerge]] *target;\n"
		    "done:\n"
		    "    return;\n"
		    "}\n",
		    "c23_attr_goto_no_defer.c", feat);
		CHECK(r.status == PRISM_OK,
		      "c23-attr-goto-no-defer: attributed computed goto without defer/zeroinit must succeed");
		prism_free(&r);
	}

	/* Case 4: C23 attributed plain goto + defer → should succeed
	 * (not computed, normal label goto is fine with defer) */
	{
		PrismFeatures feat = prism_defaults();
		feat.warn_safety = false;
		PrismResult r = prism_transpile_source(
		    "void cleanup(int *p) { (void)p; }\n"
		    "void f(void) {\n"
		    "    int x = 0;\n"
		    "    defer cleanup(&x);\n"
		    "    goto [[gnu::nomerge]] done;\n"
		    "done:\n"
		    "    return;\n"
		    "}\n",
		    "c23_attr_goto_label.c", feat);
		CHECK(r.status == PRISM_OK,
		      "c23-attr-goto-label: attributed label goto with defer must succeed");
		prism_free(&r);
	}
}

/* validate_defer_statement's catch-all loop only detected GNU statement
 * expressions ({...}) when '(' was immediately followed by '{'.  A stmt-expr
 * nested inside function arguments, array indices, _Generic, or arithmetic
 * (e.g. printf("%d", ({return;0;}))) was invisible because the outer '('
 * group was skipped as a balanced unit.  This allowed smuggling return/goto/
 * break/continue into defer bodies, silently bypassing resource cleanup.
 *
 * Root cause: the TF_OPEN handler and catch-all both checked
 *     match_ch(tok_next(s), '{')
 * which is false when the stmt-expr is not the first thing inside the parens.
 *
 * Fix: flat-scan inside () and [] groups for the '(' '{' two-token signature,
 * which catches stmt-exprs at arbitrary nesting depth.
 */
static void test_defer_stmt_expr_smuggled_in_args(void) {
	printf("\n--- defer stmt-expr smuggled in function args ---\n");

	/* Case 1: return smuggled via function argument */
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    defer {\n"
	    "        printf(\"%d\\n\", ({ return; 0; }));\n"
	    "    }\n"
	    "}\n",
	    "defer_stmtexpr_funcarg.c",
	    "defer stmt-expr in funcarg: return must be rejected",
	    "return");

	/* Case 2: goto smuggled via array index */
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    int arr[10];\n"
	    "    defer arr[({ goto skip; 0; })] = 1;\n"
	    "    skip: ;\n"
	    "}\n",
	    "defer_stmtexpr_arrayidx.c",
	    "defer stmt-expr in array index: goto must be rejected",
	    "goto");

	/* Case 3: break smuggled via _Generic */
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    for (int i = 0; i < 10; i++) {\n"
	    "        defer _Generic(({ break; 0; }), int: 0);\n"
	    "    }\n"
	    "}\n",
	    "defer_stmtexpr_generic.c",
	    "defer stmt-expr in _Generic: break must be rejected",
	    "break");

	/* Case 4: return smuggled via deeply nested arithmetic */
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    defer {\n"
	    "        int x = (1 + (2 * ({ return; 3; })));\n"
	    "    }\n"
	    "}\n",
	    "defer_stmtexpr_nested_arith.c",
	    "defer stmt-expr nested in arithmetic: return must be rejected",
	    "return");

	/* Case 5: continue smuggled via cast expression */
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    while (1) {\n"
	    "        defer (void)({ continue; 0; });\n"
	    "    }\n"
	    "}\n",
	    "defer_stmtexpr_cast.c",
	    "defer stmt-expr in cast: continue must be rejected",
	    "continue");

	/* Case 6: return smuggled via comma expr in parens */
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    defer {\n"
	    "        int x = (0, ({ return; 1; }));\n"
	    "        (void)x;\n"
	    "    }\n"
	    "}\n",
	    "defer_stmtexpr_comma.c",
	    "defer stmt-expr in comma expr: return must be rejected",
	    "return");

	/* Control: direct stmt-expr still caught */
	check_defer_transpile_rejects(
	    "void f(void) {\n"
	    "    defer ({ return; 0; });\n"
	    "}\n",
	    "defer_stmtexpr_direct.c",
	    "defer direct stmt-expr: return still rejected",
	    "return");
}

// emit_expr_to_semicolon bypasses walk_balanced keyword dispatcher
// for statement expressions in return statements when outer defers are active.
// defer/orelse/goto inside ({...}) in a return expr leak as raw text → compile error.
static void test_stmt_expr_return_defer_bypass(void) {
	printf("\n--- stmt-expr return defer bypass ---\n");

	// Case 1: defer inside stmt-expr in return with outer defer active.
	// emit_return_body → emit_expr_to_semicolon sees ({ and falls to naive
	// brace-depth tracking, completely bypassing handle_defer_keyword.
	// The defer keyword leaks as raw C text → backend compiler error.
	{
		const char *code =
		    "void outer_cleanup(void) {}\n"
		    "void inner_cleanup(void) {}\n"
		    "int f(void) {\n"
		    "    defer outer_cleanup();\n"
		    "    return ({\n"
		    "        int a = 1;\n"
		    "        { defer inner_cleanup(); a = 5; }\n"
		    "        a;\n"
		    "    });\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "se_ret_dfr.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "stmtexpr return defer: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "defer") == NULL,
			      "stmtexpr return defer: no raw 'defer' keyword in output");
			// inner_cleanup should appear AFTER a = 5 (deferred to scope exit).
			// Search within the f() function body (after "int f") to avoid
			// matching the function definition "void inner_cleanup(void) {}"
			// which appears earlier in the output.
			const char *fn = strstr(r.output, "int f");
			const char *a5 = fn ? strstr(fn, "a = 5") : NULL;
			const char *ic = a5 ? strstr(a5, "inner_cleanup") : NULL;
			CHECK(a5 != NULL && ic != NULL && ic > a5,
			      "stmtexpr return defer: inner_cleanup deferred after a = 5");
		}
		prism_free(&r);
	}

	// Case 2: bare orelse inside stmt-expr in return with outer defer active.
	// Same bypass: emit_expr_to_semicolon doesn't dispatch orelse.
	{
		const char *code =
		    "void cleanup(void) {}\n"
		    "int get(void) { return 0; }\n"
		    "int f(void) {\n"
		    "    defer cleanup();\n"
		    "    return ({\n"
		    "        int x;\n"
		    "        x = get() orelse 42;\n"
		    "        x;\n"
		    "    });\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "se_ret_oe.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "stmtexpr return orelse: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "stmtexpr return orelse: no raw 'orelse' keyword in output");
		}
		prism_free(&r);
	}

	// Case 3: goto inside stmt-expr in return with outer defer active.
	// Same bypass: emit_expr_to_semicolon doesn't dispatch handle_goto_keyword.
	{
		const char *code =
		    "void cleanup(void) {}\n"
		    "int f(void) {\n"
		    "    defer cleanup();\n"
		    "    return ({\n"
		    "        int x = 10;\n"
		    "        goto end;\n"
		    "        x = 20;\n"
		    "        end:\n"
		    "        x;\n"
		    "    });\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "se_ret_gt.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "stmtexpr return goto: transpiles OK");
		if (r.output) {
			// goto should be processed by handle_goto_keyword, not leaked raw.
			// A properly processed goto would have skip-checks applied.
			// For now, just verify no raw 'defer' leaks and output compiles.
			CHECK(strstr(r.output, "defer") == NULL,
			      "stmtexpr return goto: no raw 'defer' in output");
		}
		prism_free(&r);
	}
}

// compound literal ctrl_state desync in braceless control flow.
// nested stmt-exprs in defer must not cause exponential blowup.
// defer_scan_hidden_stmt_exprs must skip past validated ({...}) groups.
static void test_defer_nested_stmt_expr_perf(void) {
	printf("\n--- nested stmt-expr in defer performance ---\n");

	// Generate deeply nested statement-expression code inside a defer block.
	// Before the fix, depth 30 caused the transpiler to hang (>30s).
	// After the fix, depth 200 completes in <0.1s.
	const char *code =
	    "int g;\n"
	    "void f(void) {\n"
	    "    defer { g = ({g = ({g = ({g = ({g = ({g = ({g = ({g = ({g = ({g = ({\n"
	    "        g = ({g = ({g = ({g = ({g = ({g = ({g = ({g = ({g = ({g = ({\n"
	    "        0;\n"
	    "    });});});});});});});});});});\n"
	    "    });});});});});});});});});});\n"
	    "    ; };\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "fractal_defer.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "nested-stmt-expr-perf: depth-20 nested ({...}) in defer must not hang");
	prism_free(&r);
}

// After `for (...) (struct S){i};`, the `(struct S)` parens trigger
// track_ctrl_paren_close → parens_just_closed = true.  handle_open_brace
// then treats `{` as the control body brace (clears ctrl_state.pending).
// end_statement_after_semicolon never fires → for-init shadow not cleaned.
static void test_compound_literal_ctrl_state_desync(void) {
	printf("\n--- compound literal ctrl_state desync ---\n");

	// Case 1: braceless for + compound literal body + defer shadow.
	// The for-init `int i` shadows `i` captured by defer.
	// After the for-loop, shadow must be cleaned up properly.
	{
		const char *code =
		    "struct S { int x; };\n"
		    "void cleanup(int *p);\n"
		    "void f(void) {\n"
		    "    int i = 99;\n"
		    "    defer cleanup(&i);\n"
		    "    for (int i = 0; i < 1; i++)\n"
		    "        (struct S){i};\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cl_desync.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "compound-lit-ctrl-desync: braceless for + compound literal body transpiles OK");
		prism_free(&r);
	}

	// Case 2: braceless for + compound literal body + defer shadow + explicit return.
	// If the shadow leaks, check_defer_shadow_at_exit may false-positive.
	{
		const char *code =
		    "struct S { int x; };\n"
		    "void cleanup(int *p);\n"
		    "void f(void) {\n"
		    "    int i = 99;\n"
		    "    defer cleanup(&i);\n"
		    "    for (int i = 0; i < 1; i++)\n"
		    "        (struct S){i};\n"
		    "    return;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cl_desync_ret.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "compound-lit-ctrl-desync: return after for + compound literal must not false-positive");
		prism_free(&r);
	}

	// Case 3: braceless while + compound literal (cast-style).
	{
		const char *code =
		    "void cleanup(int *p);\n"
		    "void f(void) {\n"
		    "    int x = 1;\n"
		    "    defer cleanup(&x);\n"
		    "    while (x)\n"
		    "        (int){x};\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cl_desync_while.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "compound-lit-ctrl-desync: braceless while + compound literal transpiles OK");
		prism_free(&r);
	}

	// Case 4: simple compound literal in braceless if — scope underflow.
	// handle_open_brace bypassed scope_push (P1_SCOPE_INIT), but
	// handle_close_brace required in_ctrl_paren() to match — asymmetric.
	{
		const char *code =
		    "void f(int c) {\n"
		    "    if (c)\n"
		    "        (void)(int){0};\n"
		    "    defer { }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cl_braceless_if.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "compound-lit-ctrl-desync: braceless if + (int){0} + defer OK");
		prism_free(&r);
	}

	// Case 5: struct compound literal in braceless if — ctrl_state leak.
	// Struct body ';' triggered end_statement_after_semicolon → ctrl_reset(),
	// leaving stale SCOPE_CTRL_PAREN on the scope stack.
	{
		const char *code =
		    "#include <stdio.h>\n"
		    "void f(int c) {\n"
		    "    if (c)\n"
		    "        (void)(struct {int x;}){0};\n"
		    "    defer { printf(\"ok\\n\"); }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cl_struct_braceless.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "compound-lit-ctrl-desync: braceless if + struct compound literal + defer OK");
		prism_free(&r);
	}

	// Case 6: double compound literal in braceless if body.
	{
		const char *code =
		    "void f(int c) {\n"
		    "    if (c)\n"
		    "        (void)(int){0};\n"
		    "    if (c)\n"
		    "        (void)(int){0};\n"
		    "    defer { }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cl_double.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "compound-lit-ctrl-desync: double braceless if + compound literal OK");
		prism_free(&r);
	}

	// Case 7: compound literal in braceless while body.
	{
		const char *code =
		    "void f(int c) {\n"
		    "    while (c)\n"
		    "        (void)(int){0};\n"
		    "    defer { }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cl_while.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "compound-lit-ctrl-desync: braceless while + compound literal OK");
		prism_free(&r);
	}

	// Case 8: compound literal array init in braceless for body.
	{
		const char *code =
		    "void f(int c) {\n"
		    "    for (;c;)\n"
		    "        (void)(int[]){1, 2, 3};\n"
		    "    defer { }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cl_for_arr.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "compound-lit-ctrl-desync: braceless for + array compound literal OK");
		prism_free(&r);
	}

	// Case 9: emit_ctrl_condition stale parens_just_closed.
	// else→for(int i=0;...) inside orelse block body via emit_statements
	// would wrap for-init in { } due to leaked brace_wrap flag.
	{
		const char *code =
		    "int f(int a) {\n"
		    "    int x = 0;\n"
		    "    x = a orelse {\n"
		    "        if (a)\n"
		    "            ;\n"
		    "        else\n"
		    "            for (int i = 0; i < 10; i++)\n"
		    "                x++;\n"
		    "    };\n"
		    "    return x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cl_ctrl_cond.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "emit_ctrl_condition: else→for init-decl not wrongly brace-wrapped");
		if (r.output) {
			CHECK(!strstr(r.output, "for ( {int"),
			      "emit_ctrl_condition: for-init must not have spurious braces");
		}
		prism_free(&r);
	}
}

#ifdef __GNUC__
static void test_defer_shadow_stmt_expr_bypass(void) {
	printf("\n--- Defer shadow stmt-expr init bypass ---\n");

	// try_zero_init_decl returns NULL for single-declarator
	// statement-expression initializers (int x = ({...});).  This bypasses
	// process_declarators and thus check_defer_var_shadow.  The inner
	// 'target' silently hijacks the name captured by the outer defer.
	const char *code =
	    "void cleanup(int *p);\n"
	    "int f(void) {\n"
	    "    int target = 1;\n"
	    "    defer cleanup(&target);\n"
	    "    {\n"
	    "        int target = ({ 3; });\n"
	    "        (void)target;\n"
	    "        return 0;\n"
	    "    }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_shadow_stmtexpr.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "defer-shadow-stmt-expr: stmt-expr init must not bypass shadow check");
	CHECK(r.error_msg && strstr(r.error_msg, "shadows"),
	      "defer-shadow-stmt-expr: error message mentions 'shadows'");
	prism_free(&r);
}
#endif

// orelse break inside a loop inside a defer body — Phase 1F correctly
// permits break (in_loop context), but emit_deferred_orelse's non-bare path
// only handles '{' blocks and errors on control-flow keywords like break.
static void test_defer_orelse_break_in_loop(void) {
	const char *code =
	    "int get_val(int);\n"
	    "void f(int *arr, int n) {\n"
	    "    defer {\n"
	    "        for (int i = 0; i < n; i++) {\n"
	    "            arr[i] = get_val(i) orelse break;\n"
	    "        }\n"
	    "    };\n"
	    "    arr[0] = 1;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "defer_orelse_break.c", prism_defaults());
	CHECK(r.status == PRISM_OK,
	      "orelse break inside defer body loop must not be rejected");
	prism_free(&r);
}

// orelse block-form in defer body emits verbatim — bypasses zero-init,
// raw-stripping, and nested orelse processing.
static void test_defer_orelse_block_zeroinit_bypass(void) {
	{
		const char *code =
		    "int get_handle(void);\n"
		    "void use(int);\n"
		    "void f(void) {\n"
		    "    defer {\n"
		    "        get_handle() orelse {\n"
		    "            int cleanup_buf;\n"
		    "            use(cleanup_buf);\n"
		    "        };\n"
		    "    };\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "defer_orelse_block_zi.c", prism_defaults());
		if (r.status == PRISM_OK && r.output) {
			bool has_zeroinit = strstr(r.output, "cleanup_buf = 0") != NULL ||
					    strstr(r.output, "memset") != NULL;
			CHECK(has_zeroinit,
			      "defer orelse block must apply zero-init to decls");
		} else {
			CHECK(r.status == PRISM_OK,
			      "defer orelse block transpilation must succeed");
		}
		prism_free(&r);
	}
	{
		const char *code =
		    "int get_handle(void);\n"
		    "void use(int);\n"
		    "void f(void) {\n"
		    "    defer {\n"
		    "        get_handle() orelse {\n"
		    "            raw int raw_val;\n"
		    "            use(raw_val);\n"
		    "        };\n"
		    "    };\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "defer_orelse_block_raw.c", prism_defaults());
		if (r.status == PRISM_OK && r.output) {
			// Check for "raw int" or "raw " preceding a type keyword — the
			// raw keyword itself should be stripped.  The variable name
			// "raw_val" legitimately contains "raw" so check specifically.
			CHECK(strstr(r.output, "raw int") == NULL,
			      "defer orelse block must strip raw keyword");
		} else {
			CHECK(r.status == PRISM_OK,
			      "defer orelse block transpilation must succeed");
		}
		prism_free(&r);
	}
}

// emit_goto_defer was missing the p1_goto_exits depth adjustment.
// When goto in orelse crossed sibling scopes, defer cleanup was skipped.
static void test_orelse_goto_defer_sibling_scope(void) {
	printf("\n--- orelse goto defer sibling scope ---\n");

	// goto in orelse crossing sibling scope with defer
	{
		const char *code =
		    "void cleanup_a(void);\n"
		    "int get_value(void);\n"
		    "void f(void) {\n"
		    "    {\n"
		    "        defer cleanup_a();\n"
		    "        int x = get_value() orelse goto end;\n"
		    "        (void)x;\n"
		    "    }\n"
		    "    { end: (void)0; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t60a.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "transpile succeeds");
		if (r.output) {
			// The goto in orelse must emit cleanup_a() before the goto
			CHECK(strstr(r.output, "cleanup_a(); goto end") != NULL ||
			      strstr(r.output, "cleanup_a();\ngoto end") != NULL,
			      "goto in orelse must emit defer cleanup before jumping to sibling scope");
		}
		prism_free(&r);
	}

	// const orelse goto crossing sibling scope
	{
		const char *code =
		    "void cleanup_b(void);\n"
		    "int get_value(void);\n"
		    "void f(void) {\n"
		    "    {\n"
		    "        defer cleanup_b();\n"
		    "        const int x = get_value() orelse goto done;\n"
		    "        (void)x;\n"
		    "    }\n"
		    "    { done: (void)0; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t60b.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "transpile succeeds");
		if (r.output) {
			CHECK(strstr(r.output, "cleanup_b(); goto done") != NULL ||
			      strstr(r.output, "cleanup_b();\ngoto done") != NULL,
			      "const orelse goto must emit defer cleanup for sibling scope");
		}
		prism_free(&r);
	}
}

// Phase 1D defer detection false positives on variables/members named "defer"
static void test_defer_name_false_positive(void) {
	printf("\n--- defer name false positive ---\n");

	// variable named "defer" with goto — must NOT get phantom "goto skips defer"
	{
		const char *code =
		    "void f(void) {\n"
		    "    int defer = 42;\n"
		    "    goto skip;\n"
		    "    defer;\n"
		    "skip:\n"
		    "    (void)defer;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t61a.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "variable named 'defer' must not create phantom P1K_DEFER");
		prism_free(&r);
	}

	// struct member ".defer" with goto — must NOT get phantom "goto skips defer"
	{
		const char *code =
		    "struct Cfg { int defer; int val; };\n"
		    "void f(void) {\n"
		    "    struct Cfg c = {0};\n"
		    "    int x = 0;\n"
		    "    goto skip;\n"
		    "    x = c.defer + 1;\n"
		    "skip:\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t61b.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "struct member '.defer' must not create phantom P1K_DEFER");
		prism_free(&r);
	}

	// arrow member "->defer" with goto
	{
		const char *code =
		    "struct Kf { int defer; };\n"
		    "void f(void) {\n"
		    "    struct Kf k = {0};\n"
		    "    struct Kf *p = &k;\n"
		    "    int x = 0;\n"
		    "    goto skip;\n"
		    "    x = p->defer + 1;\n"
		    "skip:\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t61c.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "arrow member '->defer' must not create phantom P1K_DEFER");
		prism_free(&r);
	}

	// enum constant named "defer" with goto
	{
		const char *code =
		    "enum Actions { defer = 1, cancel = 2 };\n"
		    "void f(void) {\n"
		    "    int x = 0;\n"
		    "    goto skip;\n"
		    "    x = defer;\n"
		    "skip:\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t61d.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "enum constant named 'defer' must not create phantom P1K_DEFER");
		prism_free(&r);
	}

	// member "defer" in setjmp function — must NOT false-trigger taint error
	{
		const char *code =
		    "#include <setjmp.h>\n"
		    "struct Kf { int defer; };\n"
		    "void f(void) {\n"
		    "    jmp_buf buf;\n"
		    "    struct Kf kf = {0};\n"
		    "    if (setjmp(buf)) return;\n"
		    "    int x = kf.defer + 5;\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t61e.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "'.defer' in setjmp function must not trigger taint error");
		prism_free(&r);
	}
}

// check_defer_var_shadow state machine doesn't reset in_decl on '='.
// Variables on the RHS of an assignment inside a defer body are misclassified
// as local declarations, hiding captured references from the shadow tracker.
static void test_defer_shadow_rhs_assignment_bypass(void) {
	printf("\n--- defer shadow RHS assignment bypass ---\n");

	// Sub-test 1: ptr on RHS of 'void *local = ptr;' in defer body.
	// Outer block redeclares ptr — must be caught as shadow.
	{
		const char *code =
		    "void tracked_free(void *p);\n"
		    "void *my_alloc(int n);\n"
		    "int f(void) {\n"
		    "    void *ptr = my_alloc(1024);\n"
		    "    defer {\n"
		    "        {\n"
		    "            void *local = ptr;\n"
		    "            tracked_free(local);\n"
		    "        }\n"
		    "    }\n"
		    "    {\n"
		    "        void *ptr = my_alloc(16);\n"
		    "        (void)ptr;\n"
		    "        return 0;\n"
		    "    }\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t62a.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "ptr on RHS of assignment in defer body must be detected as captured");
		CHECK(r.error_msg && strstr(r.error_msg, "shadows"),
		      "error message mentions 'shadows'");
		prism_free(&r);
	}

	// Sub-test 2: multi-declarator 'int a = 1, b = 2;' in defer body.
	// 'b' is locally declared — outer 'int b' must NOT trigger shadow.
	{
		const char *code =
		    "void use(int x);\n"
		    "void f(void) {\n"
		    "    int b = 10;\n"
		    "    defer {\n"
		    "        {\n"
		    "            int a = 1, b = 2;\n"
		    "            use(a + b);\n"
		    "        }\n"
		    "    }\n"
		    "    {\n"
		    "        int b = 99;\n"
		    "        return;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t62b.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "'int a = 1, b = 2;' in nested defer block must not false-positive");
		prism_free(&r);
	}

	// Sub-test 3: ptr used in bare expression (not assignment) in defer body.
	// This always worked but ensures the fix doesn't regress.
	{
		const char *code =
		    "void tracked_free(void *p);\n"
		    "void *my_alloc(int n);\n"
		    "int f(void) {\n"
		    "    void *ptr = my_alloc(64);\n"
		    "    defer tracked_free(ptr);\n"
		    "    {\n"
		    "        void *ptr = my_alloc(16);\n"
		    "        (void)ptr;\n"
		    "        return 0;\n"
		    "    }\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t62c.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "bare ptr ref in defer body must still be caught");
		prism_free(&r);
	}
}

// C23 attribute [[__noreturn__]] and [[gnu::__noreturn__]] are not
// recognized by the tokenizer's noreturn scanner.  The scanner only checks
// "noreturn" and "_Noreturn" inside [[...]], missing the double-underscore
// GNU convention.  Functions with these attributes are silently accepted
// when called with active defers, bypassing the noreturn warning.
static void test_c23_attr_noreturn_dunder_blindspot(void) {
	printf("\n--- C23 attr [[__noreturn__]] blind spot ---\n");

	// Sub-test 1: [[__noreturn__]] (non-namespaced, double-underscore)
	{
		const char *code =
		    "[[__noreturn__]] void my_panic(void);\n"
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    defer cleanup();\n"
		    "    my_panic();\n"
		    "}\n";
		int pipefd[2];
		pipe(pipefd);
		int saved = dup(STDERR_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		PrismResult r = prism_transpile_source(code, "t64a.c", prism_defaults());
		fflush(stderr);
		dup2(saved, STDERR_FILENO);
		close(saved);
		close(pipefd[1]);
		char buf[4096];
		int n = read(pipefd[0], buf, sizeof(buf) - 1);
		buf[n > 0 ? n : 0] = '\0';
		close(pipefd[0]);
		CHECK(r.status == PRISM_OK, "transpile succeeds");
		CHECK(n > 0 && strstr(buf, "warning") != NULL,
		      "[[__noreturn__]] must produce defer-bypass warning");
		prism_free(&r);
	}

	// Sub-test 2: [[gnu::__noreturn__]] (namespaced double-underscore)
	{
		const char *code =
		    "[[gnu::__noreturn__]] void my_fatal(void);\n"
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    defer cleanup();\n"
		    "    my_fatal();\n"
		    "}\n";
		int pipefd[2];
		pipe(pipefd);
		int saved = dup(STDERR_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		PrismResult r = prism_transpile_source(code, "t64b.c", prism_defaults());
		fflush(stderr);
		dup2(saved, STDERR_FILENO);
		close(saved);
		close(pipefd[1]);
		char buf[4096];
		int n = read(pipefd[0], buf, sizeof(buf) - 1);
		buf[n > 0 ? n : 0] = '\0';
		close(pipefd[0]);
		CHECK(r.status == PRISM_OK, "transpile succeeds");
		CHECK(n > 0 && strstr(buf, "warning") != NULL,
		      "[[gnu::__noreturn__]] must produce defer-bypass warning");
		prism_free(&r);
	}

	// Sub-test 3: [[noreturn]] (standard form) — must still work (regression guard)
	{
		const char *code =
		    "[[noreturn]] void my_exit(void);\n"
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    defer cleanup();\n"
		    "    my_exit();\n"
		    "}\n";
		int pipefd[2];
		pipe(pipefd);
		int saved = dup(STDERR_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		PrismResult r = prism_transpile_source(code, "t64c.c", prism_defaults());
		fflush(stderr);
		dup2(saved, STDERR_FILENO);
		close(saved);
		close(pipefd[1]);
		char buf[4096];
		int n = read(pipefd[0], buf, sizeof(buf) - 1);
		buf[n > 0 ? n : 0] = '\0';
		close(pipefd[0]);
		CHECK(r.status == PRISM_OK, "transpile succeeds");
		CHECK(n > 0 && strstr(buf, "warning") != NULL,
		      "[[noreturn]] must still produce warning (regression)");
		prism_free(&r);
	}
}

// GNU __attribute__((cold, __noreturn__)) list truncation.
// The scanner only checked the first attribute after (( — missed noreturn
// when preceded by other attributes in a comma-separated list.
// Also covers __declspec(__noreturn__) blind spot.
static void test_gnu_attr_noreturn_list_blindspot(void) {
	printf("\n--- GNU attr list noreturn blind spot ---\n");

	// Sub-test 1: __attribute__((cold, __noreturn__)) — noreturn not first
	{
		const char *code =
		    "__attribute__((cold, __noreturn__)) void my_panic(void);\n"
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    defer cleanup();\n"
		    "    my_panic();\n"
		    "}\n";
		int pipefd[2];
		pipe(pipefd);
		int saved = dup(STDERR_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		PrismResult r = prism_transpile_source(code, "t66a.c", prism_defaults());
		fflush(stderr);
		dup2(saved, STDERR_FILENO);
		close(saved);
		close(pipefd[1]);
		char buf[4096];
		int n = read(pipefd[0], buf, sizeof(buf) - 1);
		buf[n > 0 ? n : 0] = '\0';
		close(pipefd[0]);
		CHECK(r.status == PRISM_OK, "transpile succeeds");
		CHECK(n > 0 && strstr(buf, "warning") != NULL,
		      "__attribute__((cold, __noreturn__)) must warn");
		prism_free(&r);
	}

	// Sub-test 2: __attribute__((always_inline, noreturn)) — keyword form not first
	{
		const char *code =
		    "__attribute__((always_inline, noreturn)) void my_abort(void);\n"
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    defer cleanup();\n"
		    "    my_abort();\n"
		    "}\n";
		int pipefd[2];
		pipe(pipefd);
		int saved = dup(STDERR_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		PrismResult r = prism_transpile_source(code, "t66b.c", prism_defaults());
		fflush(stderr);
		dup2(saved, STDERR_FILENO);
		close(saved);
		close(pipefd[1]);
		char buf[4096];
		int n = read(pipefd[0], buf, sizeof(buf) - 1);
		buf[n > 0 ? n : 0] = '\0';
		close(pipefd[0]);
		CHECK(r.status == PRISM_OK, "transpile succeeds");
		CHECK(n > 0 && strstr(buf, "warning") != NULL,
		      "__attribute__((always_inline, noreturn)) must warn");
		prism_free(&r);
	}

	// Sub-test 3: __attribute__((noreturn, cold)) — noreturn first (regression)
	{
		const char *code =
		    "__attribute__((noreturn, cold)) void my_exit(void);\n"
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    defer cleanup();\n"
		    "    my_exit();\n"
		    "}\n";
		int pipefd[2];
		pipe(pipefd);
		int saved = dup(STDERR_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		PrismResult r = prism_transpile_source(code, "t66c.c", prism_defaults());
		fflush(stderr);
		dup2(saved, STDERR_FILENO);
		close(saved);
		close(pipefd[1]);
		char buf[4096];
		int n = read(pipefd[0], buf, sizeof(buf) - 1);
		buf[n > 0 ? n : 0] = '\0';
		close(pipefd[0]);
		CHECK(r.status == PRISM_OK, "transpile succeeds");
		CHECK(n > 0 && strstr(buf, "warning") != NULL,
		      "__attribute__((noreturn, cold)) must still warn");
		prism_free(&r);
	}

	// Sub-test 4: __declspec(__noreturn__) — MSVC dunder form
	{
		const char *code =
		    "__declspec(__noreturn__) void my_fatal(void);\n"
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    defer cleanup();\n"
		    "    my_fatal();\n"
		    "}\n";
		int pipefd[2];
		pipe(pipefd);
		int saved = dup(STDERR_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		PrismResult r = prism_transpile_source(code, "t66d.c", prism_defaults());
		fflush(stderr);
		dup2(saved, STDERR_FILENO);
		close(saved);
		close(pipefd[1]);
		char buf[4096];
		int n = read(pipefd[0], buf, sizeof(buf) - 1);
		buf[n > 0 ? n : 0] = '\0';
		close(pipefd[0]);
		CHECK(r.status == PRISM_OK, "transpile succeeds");
		CHECK(n > 0 && strstr(buf, "warning") != NULL,
		      "__declspec(__noreturn__) must produce warning");
		prism_free(&r);
	}
}

static void test_defer_shadow_brace_init_comma_bypass(void) {
	printf("\n--- brace-initializer comma bypass in defer shadow check ---\n");

	// Sub-test 1: brace initializer at depth > 1 referencing captured var.
	// The comma inside { NULL, ptr } must NOT re-enter declaration context.
	{
		const char *code =
		    "void *malloc(unsigned long);\n"
		    "void free(void *);\n"
		    "void f(void) {\n"
		    "    int *ptr = (int*)malloc(1024);\n"
		    "    defer {\n"
		    "        {\n"
		    "            void *arr[] = { (void*)0, ptr };\n"
		    "            free(arr[1]);\n"
		    "        }\n"
		    "    }\n"
		    "    {\n"
		    "        int *ptr = (int*)malloc(16);\n"
		    "        (void)ptr;\n"
		    "        return;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t67a.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "ptr in brace-init at depth > 1 must be detected as captured");
		CHECK(r.error_msg && strstr(r.error_msg, "shadows"),
		      "error message mentions 'shadows'");
		prism_free(&r);
	}

	// Sub-test 2: multi-declarator comma at same depth must STILL work.
	// 'int dummy = 0, ptr = 1;' locally declares ptr — no shadow error.
	{
		const char *code =
		    "void *malloc(unsigned long);\n"
		    "void free(void *);\n"
		    "void f(void) {\n"
		    "    int *ptr = (int*)malloc(1024);\n"
		    "    defer {\n"
		    "        {\n"
		    "            int dummy = 0, ptr = 1;\n"
		    "            (void)dummy; (void)ptr;\n"
		    "        }\n"
		    "    }\n"
		    "    {\n"
		    "        int *ptr = (int*)malloc(16);\n"
		    "        (void)ptr;\n"
		    "        return;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t67b.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "multi-decl comma at same depth must still work (ptr locally declared)");
		prism_free(&r);
	}

	// Sub-test 3: nested struct initializer with captured var inside values.
	{
		const char *code =
		    "void *malloc(unsigned long);\n"
		    "void free(void *);\n"
		    "typedef struct { void *p; int n; } Pair;\n"
		    "void f(void) {\n"
		    "    void *ptr = malloc(1024);\n"
		    "    defer {\n"
		    "        {\n"
		    "            Pair pairs[] = { { ptr, 1 }, { (void*)0, 0 } };\n"
		    "            free(pairs[0].p);\n"
		    "        }\n"
		    "    }\n"
		    "    {\n"
		    "        void *ptr = malloc(16);\n"
		    "        (void)ptr;\n"
		    "        return;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t67c.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "ptr in nested struct init must be detected as captured");
		CHECK(r.error_msg && strstr(r.error_msg, "shadows"),
		      "error message mentions 'shadows'");
		prism_free(&r);
	}
}

// (noreturn_fn)() with active defers skips warning because
// the TT_NORETURN_FN lookahead required tok_next to be '('.
static void test_paren_noreturn_warning_bypass(void) {
	// Sub-test 1: (fatal_panic)() should produce the same warning as fatal_panic()
	{
		const char *code =
		    "_Noreturn void fatal_panic(void);\n"
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    defer cleanup();\n"
		    "    (fatal_panic)();\n"
		    "}\n";
		int pipefd[2];
		pipe(pipefd);
		int saved = dup(STDERR_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		PrismResult r = prism_transpile_source(code, "t71a.c", prism_defaults());
		fflush(stderr);
		dup2(saved, STDERR_FILENO);
		close(saved);
		close(pipefd[1]);
		char buf[4096];
		int n = read(pipefd[0], buf, sizeof(buf) - 1);
		buf[n > 0 ? n : 0] = '\0';
		close(pipefd[0]);
		CHECK(r.status == PRISM_OK, "transpiles OK");
		CHECK(n > 0 && strstr(buf, "warning") != NULL,
		      "(fatal_panic)() must warn about defers not running");
		prism_free(&r);
	}

	// Sub-test 2: control case — fatal_panic() (no parens) still warns
	{
		const char *code =
		    "_Noreturn void fatal_panic(void);\n"
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    defer cleanup();\n"
		    "    fatal_panic();\n"
		    "}\n";
		int pipefd[2];
		pipe(pipefd);
		int saved = dup(STDERR_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		PrismResult r = prism_transpile_source(code, "t71b.c", prism_defaults());
		fflush(stderr);
		dup2(saved, STDERR_FILENO);
		close(saved);
		close(pipefd[1]);
		char buf[4096];
		int n = read(pipefd[0], buf, sizeof(buf) - 1);
		buf[n > 0 ? n : 0] = '\0';
		close(pipefd[0]);
		CHECK(r.status == PRISM_OK, "transpiles OK");
		CHECK(n > 0 && strstr(buf, "warning") != NULL,
		      "fatal_panic() must still warn (regression guard)");
		prism_free(&r);
	}
}

// (setjmp_wrapper)() bypasses transitive taint propagation because
// the ident( check requires tok_next to be '(' — ')' from the parens fails.
static void test_paren_setjmp_wrapper_taint_bypass(void) {
	// Sub-test 1: (my_setjmp_wrapper)() must taint the caller
	{
		const char *code =
		    "#include <setjmp.h>\n"
		    "void my_setjmp_wrapper(jmp_buf buf) {\n"
		    "    setjmp(buf);\n"
		    "}\n"
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    jmp_buf buf;\n"
		    "    defer cleanup();\n"
		    "    (my_setjmp_wrapper)(buf);\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t71c.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "defer in function calling (setjmp_wrapper)() must be rejected");
		prism_free(&r);
	}

	// Sub-test 2: control — my_setjmp_wrapper() (no parens) still errors
	{
		const char *code =
		    "#include <setjmp.h>\n"
		    "void my_setjmp_wrapper(jmp_buf buf) {\n"
		    "    setjmp(buf);\n"
		    "}\n"
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    jmp_buf buf;\n"
		    "    defer cleanup();\n"
		    "    my_setjmp_wrapper(buf);\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t71d.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "defer in function calling setjmp_wrapper() must be rejected (regression)");
		prism_free(&r);
	}
}

// typedef enum { name = 0 } wrapping bypasses defer shadow check.
// parse_type_specifier skips the enum body with skip_balanced, so
// check_enum_typedef_defer_shadow's TT_TYPEDEF path never sees the constants.
static void test_defer_shadow_typedef_enum_bypass(void) {
	// Sub-test 1: typedef enum { ptr = 0 } must trigger shadow error
	{
		const char *code =
		    "void cleanup(int *p);\n"
		    "int *alloc(void);\n"
		    "void f(void) {\n"
		    "    int *ptr = alloc();\n"
		    "    defer cleanup(ptr);\n"
		    "    {\n"
		    "        typedef enum { ptr = 0 } MyEnum;\n"
		    "        return;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t72a.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "typedef enum constant must not silently shadow defer-captured var");
		CHECK(r.error_msg && strstr(r.error_msg, "shadows"),
		      "error message mentions 'shadows'");
		prism_free(&r);
	}

	// Sub-test 2: control — bare enum (no typedef) still catches shadow
	{
		const char *code =
		    "void cleanup(int *p);\n"
		    "int *alloc(void);\n"
		    "void f(void) {\n"
		    "    int *ptr = alloc();\n"
		    "    defer cleanup(ptr);\n"
		    "    {\n"
		    "        enum { ptr = 0 };\n"
		    "        return;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t72b.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "bare enum constant shadow must still be caught (regression)");
		prism_free(&r);
	}
}

// validate_defer_statement case/default scanner lacks ternary depth
// tracking: case EXPR ? X : Y: allows the ternary ':' to be misidentified
// as the case label colon, causing the real body (after the label ':') to
// escape Phase 1F validation. return/goto/break silently emitted.
static void test_defer_case_ternary_desync(void) {
	printf("\n--- case ternary desync in validate_defer_statement ---\n");

	// Sub-test 1: return after ternary case label must be caught
	{
		const char *code =
		    "void f(int x) {\n"
		    "    defer {\n"
		    "        switch (x) {\n"
		    "            case 1 ? 0 : 0:\n"
		    "                return;\n"
		    "        }\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t73a.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "return after ternary case label must be caught by Phase 1F");
		CHECK(r.error_msg && strstr(r.error_msg, "return"),
		      "error mentions 'return'");
		prism_free(&r);
	}

	// Sub-test 2: goto after ternary case must also be caught
	{
		const char *code =
		    "void f(int x) {\n"
		    "    defer {\n"
		    "        switch (x) {\n"
		    "            case (1 ? 2 : 3):\n"
		    "                goto end;\n"
		    "        }\n"
		    "    }\n"
		    "    end: ;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t73b.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "goto after parenthesized ternary case must be caught");
		prism_free(&r);
	}

	// Sub-test 3: valid ternary in case without control flow — must succeed
	{
		const char *code =
		    "#include <stdio.h>\n"
		    "void f(int x) {\n"
		    "    defer {\n"
		    "        switch (x) {\n"
		    "            case 1 ? 42 : 0:\n"
		    "                printf(\"ok\\n\");\n"
		    "                break;\n"
		    "        }\n"
		    "    }\n"
		    "}\n"
		    "int main(void) { f(42); return 0; }\n";
		PrismResult r = prism_transpile_source(code, "t73c.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "valid ternary case without control flow must succeed");
		prism_free(&r);
	}

	// Sub-test 4: nested ternary in case — both colons consumed before label
	{
		const char *code =
		    "void f(int x) {\n"
		    "    defer {\n"
		    "        switch (x) {\n"
		    "            case (1 ? (2 ? 3 : 4) : 5):\n"
		    "                return;\n"
		    "        }\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t73d.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "return after nested ternary case must be caught");
		prism_free(&r);
	}
}

// check_defer_var_shadow false positive on for-init declarations.
// for(int x = ...) inside a defer body at paren_depth > 0 was treated as a
// captured reference instead of a local declaration, causing spurious shadow
// errors when an outer variable with the same name was declared after the defer.
static void test_defer_same_block_shadow_hijack(void) {
	printf("\n--- same-block defer shadow hijack ---\n");

	// Sub-test 1: same-block shadow — must be rejected (silent miscompilation)
	{
		const char *code =
		    "void use(int x);\n"
		    "int global_x = 1;\n"
		    "void f(void) {\n"
		    "    defer use(global_x);\n"
		    "    int global_x = 2;\n"
		    "    (void)global_x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t75a.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "same-block defer shadow must be rejected");
		CHECK(r.error_msg && strstr(r.error_msg, "shadows"),
		      "error mentions 'shadows'");
		prism_free(&r);
	}

	// Sub-test 2: same-block shadow with explicit return — must be rejected
	{
		const char *code =
		    "void use(int x);\n"
		    "int global_x = 1;\n"
		    "int f(void) {\n"
		    "    defer use(global_x);\n"
		    "    int global_x = 2;\n"
		    "    (void)global_x;\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t75b.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "same-block shadow with return must be rejected");
		prism_free(&r);
	}

	// Sub-test 3: inner-block shadow (no exit) — must be ACCEPTED
	// Inner variable goes out of scope at } before outer defer fires.
	{
		const char *code =
		    "void use(int x);\n"
		    "int global_x = 1;\n"
		    "void f(void) {\n"
		    "    defer use(global_x);\n"
		    "    {\n"
		    "        int global_x = 2;\n"
		    "        (void)global_x;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t75c.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "inner-block shadow without exit must be accepted");
		prism_free(&r);
	}

	// Sub-test 4: same-block block-form defer — must be rejected
	{
		const char *code =
		    "void use(int x);\n"
		    "int global_x = 1;\n"
		    "void f(void) {\n"
		    "    defer { use(global_x); }\n"
		    "    int global_x = 2;\n"
		    "    (void)global_x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t75d.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "same-block block-form defer shadow must be rejected");
		prism_free(&r);
	}
}

static void test_defer_shadow_for_init_false_positive(void) {
	printf("\n--- defer shadow for-init false positive ---\n");

	// Sub-test 1: for-init in defer body — must NOT false-positive
	{
		const char *code =
		    "void use(int x);\n"
		    "int f(void) {\n"
		    "    defer {\n"
		    "        for (int x = 0; x < 3; x++) {\n"
		    "            use(x);\n"
		    "        }\n"
		    "    }\n"
		    "    { int x = 99; (void)x; return 0; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t74a.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "for-init in defer body must not false-positive on shadow");
		prism_free(&r);
	}

	// Sub-test 2: braceless for body — must NOT false-positive
	{
		const char *code =
		    "void use(int x);\n"
		    "int f(void) {\n"
		    "    defer {\n"
		    "        for (int x = 0; x < 3; x++)\n"
		    "            use(x);\n"
		    "    }\n"
		    "    { int x = 99; (void)x; return 0; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t74b.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "braceless for body must not false-positive");
		prism_free(&r);
	}

	// Sub-test 3: nested for with same name — must NOT false-positive
	{
		const char *code =
		    "void use(int x);\n"
		    "int f(void) {\n"
		    "    defer {\n"
		    "        for (int x = 0; x < 3; x++) {\n"
		    "            for (int x = 0; x < 5; x++) {\n"
		    "                use(x);\n"
		    "            }\n"
		    "        }\n"
		    "    }\n"
		    "    { int x = 99; (void)x; return 0; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t74c.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "nested for-init same name must not false-positive");
		prism_free(&r);
	}

	// Sub-test 4: genuine capture AFTER for loop — must still be caught
	{
		const char *code =
		    "void use(int x);\n"
		    "int f(void) {\n"
		    "    int x = 42;\n"
		    "    defer {\n"
		    "        for (int x = 0; x < 3; x++) {\n"
		    "            use(x);\n"
		    "        }\n"
		    "        use(x);\n"
		    "    }\n"
		    "    { int x = 99; (void)x; return 0; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t74d.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "genuine capture after for loop must still be caught");
		CHECK(r.error_msg && strstr(r.error_msg, "shadows"),
		      "error mentions 'shadows'");
		prism_free(&r);
	}

	// Sub-test 5: genuine capture INSIDE for body — must still be caught
	{
		const char *code =
		    "void use(int x);\n"
		    "int f(void) {\n"
		    "    int y = 42;\n"
		    "    defer {\n"
		    "        for (int x = 0; x < 3; x++) {\n"
		    "            use(y);\n"
		    "        }\n"
		    "    }\n"
		    "    { int y = 99; (void)y; return 0; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t74e.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "genuine capture inside for body must still be caught");
		CHECK(r.error_msg && strstr(r.error_msg, "shadows"),
		      "error mentions 'shadows'");
		prism_free(&r);
	}
}

// Regression: a user-defined wrapper around exit/abort/_Exit/etc. (all
// TT_NORETURN_FN but NOT TT_SPECIAL_FN) used to propagate TT_NORETURN_FN
// onto the caller's body via wrapper_taint, triggering a hard "defer cannot
// be used in functions that call vfork()" error on every caller of the
// wrapper. Direct calls to exit/abort only produced a warning at the call
// site. Inconsistent. Fix: wrapper_taint now mirrors the direct-body scan —
// only TT_SPECIAL_FN callees taint the body (vfork → TT_NORETURN_FN,
// setjmp/longjmp/pthread_exit → TT_SPECIAL_FN). Bare TT_NORETURN_FN
// callees do not taint; the existing Pass 2 per-call-site warning covers
// them.
static void test_defer_wrapper_of_exit_not_rejected(void) {
	printf("\n--- defer + user wrapper of exit ---\n");

	// Wrapper of exit — must NOT hard-error.  Pass 2 should still warn
	// at the call site (`die` is tagged TT_NORETURN_FN because die's
	// declaration carries __attribute__((noreturn))).
	const char *code =
	    "void exit(int);\n"
	    "void cleanup(void);\n"
	    "__attribute__((noreturn)) void die(int code) { exit(code); }\n"
	    "int main(int argc, char **argv) {\n"
	    "    defer cleanup();\n"
	    "    if (argc > 1) die(1);\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "wrap_exit.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "wrapper-of-exit: defer must not be rejected when function calls "
	         "a user-defined wrapper of exit/abort");
	prism_free(&r);

	// Wrapper of abort — same story.
	code =
	    "void abort(void);\n"
	    "void cleanup(void);\n"
	    "void bail(void) { abort(); }\n"
	    "void f(void) { defer cleanup(); bail(); }\n";
	r = prism_transpile_source(code, "wrap_abort.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "wrapper-of-abort: defer must not be rejected through wrapper");
	prism_free(&r);

	// Wrapper of vfork — must STILL hard-error (this is the genuine
	// UB case: vfork's child shares the parent's memory).
	code =
	    "int vfork(void);\n"
	    "void cleanup(void);\n"
	    "int my_vfork(void) { return vfork(); }\n"
	    "void f(void) { defer cleanup(); my_vfork(); }\n";
	r = prism_transpile_source(code, "wrap_vfork.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "wrapper-of-vfork: vfork wrapper STILL rejects defer (UB: child "
	      "shares parent memory, defer cleanup would double-run)");
	CHECK(r.error_msg && strstr(r.error_msg, "vfork"),
	      "wrapper-of-vfork: error message mentions vfork");
	prism_free(&r);

	// Wrapper of setjmp — must also still hard-error.
	code =
	    "typedef int jmp_buf[20];\n"
	    "int setjmp(jmp_buf);\n"
	    "jmp_buf env;\n"
	    "void cleanup(void);\n"
	    "int my_setjmp(void) { return setjmp(env); }\n"
	    "void f(void) { defer cleanup(); my_setjmp(); }\n";
	r = prism_transpile_source(code, "wrap_setjmp.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "wrapper-of-setjmp: setjmp wrapper STILL rejects defer");
	prism_free(&r);
}

static void test_defer_vfork_member_no_reject(void) {
	printf("\n--- vfork member namespace pollution ---\n");
	// struct member named 'vfork' falsely taints the function body,
	// causing "defer cannot be used in functions that call vfork()" error.
	// The vfork body scan in parse.c tagged TT_NORETURN_FN on the body '{'
	// when it found 'vfork' without checking for member-access predecessor.

	// Sub-test 1: dot member access
	{
		const char *code =
		    "struct os { int (*vfork)(void); };\n"
		    "int dummy(void) { return 0; }\n"
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    struct os os = { .vfork = dummy };\n"
		    "    defer cleanup();\n"
		    "    os.vfork();\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vfork_member_dot.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "vfork-member: os.vfork() must not reject defer");
		prism_free(&r);
	}

	// Sub-test 2: arrow member access
	{
		const char *code =
		    "struct os { int (*vfork)(void); };\n"
		    "void cleanup(void);\n"
		    "void f(struct os *p) {\n"
		    "    defer cleanup();\n"
		    "    p->vfork();\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vfork_member_arrow.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "vfork-member: p->vfork() must not reject defer");
		prism_free(&r);
	}
}

static void test_defer_shadow_braceless_for_ifelse(void) {
	printf("\n--- braceless for-body if/else shadow false positive ---\n");

	// Sub-test 1: if/else inside braceless for body
	// The semicolon after the if-branch must NOT clear for_name_hid.
	{
		const char *code =
		    "void use(int);\n"
		    "void f(void) {\n"
		    "    defer {\n"
		    "        for (int x = 0; x < 10; x++)\n"
		    "            if (x % 2 == 0)\n"
		    "                use(x);\n"
		    "            else\n"
		    "                use(x);\n"
		    "    }\n"
		    "    int x = 42;\n"
		    "    use(x);\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "braceless_ifelse.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "braceless-for-ifelse: must not false-positive on for-init shadow");
		prism_free(&r);
	}

	// Sub-test 2: do/while inside braceless for body
	// The } of the do-body must NOT clear for_name_hid before the while condition.
	{
		const char *code =
		    "void use(int);\n"
		    "void f(void) {\n"
		    "    defer {\n"
		    "        for (int x = 0; x < 10; x++)\n"
		    "            do { use(x); } while (x < 5);\n"
		    "    }\n"
		    "    int x = 42;\n"
		    "    use(x);\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "braceless_dowhile.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "braceless-for-dowhile: must not false-positive on for-init shadow");
		prism_free(&r);
	}

	// Sub-test 3: genuine shadow AFTER braceless for body must still be caught
	{
		const char *code =
		    "void use(int);\n"
		    "void f(void) {\n"
		    "    defer {\n"
		    "        for (int x = 0; x < 10; x++)\n"
		    "            use(x);\n"
		    "        use(x);\n"
		    "    }\n"
		    "    int x = 42;\n"
		    "    use(x);\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "braceless_genuine.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "braceless-for-genuine: genuine shadow after for body must be caught");
		prism_free(&r);
	}

	// Sub-test 4: nested for with if/else — both for-inits hide the name
	{
		const char *code =
		    "void use(int);\n"
		    "void f(void) {\n"
		    "    defer {\n"
		    "        for (int x = 0; x < 3; x++)\n"
		    "            for (int y = 0; y < 3; y++)\n"
		    "                if (y % 2)\n"
		    "                    use(x);\n"
		    "                else\n"
		    "                    use(x);\n"
		    "    }\n"
		    "    int x = 42;\n"
		    "    use(x);\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "nested_for_ifelse.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "nested-for-ifelse: must not false-positive");
		prism_free(&r);
	}
}

// scope_depth/block_depth type confusion in emit_defers_ex.
// When non-SCOPE_BLOCK entries (SCOPE_CTRL_PAREN) are on the scope_stack,
// the scope_stack index `d` diverges from block_depth. The old code compared
// `d < stop_depth` where stop_depth is in block_depth units, causing
// over-unwinding of defers for same-scope gotos inside stmt-exprs.
static void test_scope_depth_overunwind(void) {
	/* goto L is a forward jump WITHIN the same block inside a stmt-expr
	 * that lives inside if(...). The SCOPE_CTRL_PAREN shifts scope indices.
	 * The defer must fire exactly once (at scope exit), not at the goto. */
	{
		PrismResult r = prism_transpile_source(
		    "int counter = 0;\n"
		    "void f(void) {\n"
		    "    if ( ({\n"
		    "        {\n"
		    "            defer { counter++; }\n"
		    "            goto L;\n"
		    "            L: ;\n"
		    "        }\n"
		    "        42;\n"
		    "    }) ) {\n"
		    "        (void)0;\n"
		    "    }\n"
		    "}\n",
		    "t78a.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			/* The goto should NOT be wrapped with defer cleanup
			 * because it stays within the same scope. Count
			 * occurrences of "counter++" — should be exactly 1
			 * (the scope-exit defer), not 2 (goto + scope-exit). */
			const char *fn = strstr(r.output, "void f(");
			CHECK(fn != NULL, "function found");
			if (fn) {
				int count = 0;
				const char *p = fn;
				while ((p = strstr(p, "counter++")) != NULL) {
					count++;
					p += 9;
				}
				CHECK_EQ(count, 1,
				         "defer fires once (not over-unwound)");
			}
		}
		prism_free(&r);
	}
	/* Backward same-scope goto with SCOPE_CTRL_PAREN on stack */
	{
		PrismResult r = prism_transpile_source(
		    "int counter = 0;\n"
		    "void f(void) {\n"
		    "    if ( ({\n"
		    "        {\n"
		    "            defer { counter++; }\n"
		    "            L: ;\n"
		    "            static int j = 0;\n"
		    "            if (!j) { j = 1; goto L; }\n"
		    "        }\n"
		    "        42;\n"
		    "    }) ) {\n"
		    "        (void)0;\n"
		    "    }\n"
		    "}\n",
		    "t78b.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			const char *fn = strstr(r.output, "void f(");
			CHECK(fn != NULL, "function found");
			if (fn) {
				int count = 0;
				const char *p = fn;
				while ((p = strstr(p, "counter++")) != NULL) {
					count++;
					p += 9;
				}
				CHECK_EQ(count, 1,
				         "defer fires once (not over-unwound)");
			}
		}
		prism_free(&r);
	}
}

// emit_range_no_prep flat-loops tokens without routing stmt-exprs
// through walk_balanced. A goto inside a stmt-expr in an orelse LHS
// bypasses defer cleanup.
// Uses block-form orelse (if-guard, single LHS eval) — bare value fallback
// would correctly reject the goto via reject_orelse_side_effects since
// the ternary expansion evaluates LHS twice.
static void test_emit_range_no_prep_stmtexpr_defer(void) {
	PrismResult r = prism_transpile_source(
	    "int counter = 0;\n"
	    "void f(void) {\n"
	    "    int target = 0;\n"
	    "    {\n"
	    "        defer { counter++; }\n"
	    "        *({ goto skip; &target; }) = 1 orelse { return; };\n"
	    "        (void)target;\n"
	    "    }\n"
	    "    return;\n"
	    "skip:\n"
	    "    return;\n"
	    "}\n",
	    "t79.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "transpiles OK");
	if (r.status == PRISM_OK && r.output) {
		/* The goto in the LHS stmt-expr must have defer cleanup.
		 * Find "goto skip" and check that "counter++" appears before it */
		const char *fn = strstr(r.output, "void f(");
		CHECK(fn != NULL, "function found");
		if (fn) {
			const char *gs = strstr(fn, "goto skip");
			const char *cleanup = strstr(fn, "counter++");
			CHECK(gs != NULL, "goto found");
			CHECK(cleanup != NULL && cleanup < gs,
			      "defer cleanup before goto");
		}
	}
	prism_free(&r);
}

// emit_deferred_range did not track control-flow keywords,
// so at_stmt_start was false for braceless if/else/for/while/do bodies
// inside defer blocks.  orelse was emitted verbatim; zero-init was skipped.
static void test_defer_braceless_ctrl_orelse(void) {
	printf("\n--- braceless ctrl-flow orelse/zeroinit in defer ---\n");
	// orelse in braceless if body inside defer
	{
		PrismResult r = prism_transpile_source(
		    "int f(void) {\n"
		    "    int result = -1;\n"
		    "    int zero = 0;\n"
		    "    {\n"
		    "        defer {\n"
		    "            if (1)\n"
		    "                result = *(&zero) orelse 99;\n"
		    "        }\n"
		    "    }\n"
		    "    return result;\n"
		    "}\n",
		    "t81_if.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "no literal 'orelse' in output");
			CHECK(strstr(r.output, "__prism_oe_") != NULL,
			      "orelse temp generated");
		}
		prism_free(&r);
	}
	// orelse in else body inside defer
	{
		PrismResult r = prism_transpile_source(
		    "int f(void) {\n"
		    "    int result = -1;\n"
		    "    int zero = 0;\n"
		    "    {\n"
		    "        defer {\n"
		    "            if (0) result = 0;\n"
		    "            else result = *(&zero) orelse 77;\n"
		    "        }\n"
		    "    }\n"
		    "    return result;\n"
		    "}\n",
		    "t81_else.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "no literal 'orelse' in output");
		}
		prism_free(&r);
	}
	// orelse in braceless for body inside defer
	{
		PrismResult r = prism_transpile_source(
		    "int f(void) {\n"
		    "    int result = -1;\n"
		    "    int zero = 0;\n"
		    "    {\n"
		    "        defer {\n"
		    "            for (int i = 0; i < 1; i++)\n"
		    "                result = *(&zero) orelse 55;\n"
		    "        }\n"
		    "    }\n"
		    "    return result;\n"
		    "}\n",
		    "t81_for.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "no literal 'orelse' in output");
		}
		prism_free(&r);
	}
	// orelse in braceless do body inside defer
	{
		PrismResult r = prism_transpile_source(
		    "int f(void) {\n"
		    "    int result = -1;\n"
		    "    int zero = 0;\n"
		    "    {\n"
		    "        defer {\n"
		    "            do\n"
		    "                result = *(&zero) orelse 11;\n"
		    "            while (0);\n"
		    "        }\n"
		    "    }\n"
		    "    return result;\n"
		    "}\n",
		    "t81_do.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "no literal 'orelse' in output");
		}
		prism_free(&r);
	}
}

// shadowed 'defer' (variable/typedef named 'defer') suppresses block form
static void test_defer_shadow_variable_block(void) {
	// A local variable named 'defer' should not disable block form 'defer {}'
	const char *code =
	    "int log_count;\n"
	    "void f(void) {\n"
	    "    int defer = 5;\n"
	    "    (void)defer;\n"
	    "    defer { log_count++; }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "shadow_defer_var.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "shadow-defer-var: variable named 'defer' must not suppress block form");
	if (r.status == PRISM_OK && r.output) {
		CHECK(strstr(r.output, "log_count++") != NULL,
		      "shadow-defer-var: defer body must be emitted");
	}
	prism_free(&r);
}

static void test_defer_shadow_typedef_block(void) {
	// A typedef named 'defer' should not disable block form 'defer {}'
	const char *code =
	    "typedef int defer;\n"
	    "int log_count;\n"
	    "void f(void) {\n"
	    "    defer { log_count++; }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "shadow_defer_typedef.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "shadow-defer-typedef: typedef named 'defer' must not suppress block form");
	if (r.status == PRISM_OK && r.output) {
		CHECK(strstr(r.output, "log_count++") != NULL,
		      "shadow-defer-typedef: defer body must be emitted");
	}
	prism_free(&r);
}

// check_defer_var_shadow false positive inside GNU statement expressions.
// The scanner's paren_depth==0 guard prevents it from recognizing declarations
// inside ({...}), so macro temporaries are falsely flagged as outer-scope captures.
static void test_defer_shadow_stmt_expr_local_false_positive(void) {
	printf("\n--- Defer shadow stmt-expr local false positive ---\n");
	// Stmt-expr macro temp inside defer: _tmp_a is local to the stmt-expr,
	// re-declaring _tmp_a in the outer scope must not trigger a false shadow error.
	{
		const char *code =
		    "int main() {\n"
		    "    int out = 0;\n"
		    "    defer out = ({ typeof(10) _tmp_a = (10); typeof(20) _tmp_b = (20);\n"
		    "                   _tmp_a > _tmp_b ? _tmp_a : _tmp_b; });\n"
		    "    int _tmp_a = 5;\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "shadow_se_fp.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "defer-shadow-se-fp: stmt-expr local must not trigger false shadow");
		prism_free(&r);
	}
	// Nested stmt-exprs: inner locals must not leak to outer scope
	{
		const char *code =
		    "int main() {\n"
		    "    int x = 0;\n"
		    "    defer x = ({ int inner = ({ int deep = 1; deep; }); inner + 1; });\n"
		    "    int deep = 99;\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "shadow_se_nested.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "defer-shadow-se-nested: nested stmt-expr locals must not trigger shadow");
		prism_free(&r);
	}
	// Stmt-expr local that IS also used outside: must still catch real shadows
	{
		const char *code =
		    "void cleanup(int *p);\n"
		    "int f(void) {\n"
		    "    int val = 1;\n"
		    "    defer cleanup(&val);\n"
		    "    {\n"
		    "        int val = 2;\n"
		    "        return val;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "shadow_se_real.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "defer-shadow-se-real: real shadow in inner block must still be caught");
		prism_free(&r);
	}
}

// braceless inner switch caused find_switch_scope to leak to outer switch,
// resetting defer_count and silently wiping registered defers.
static void test_braceless_switch_defer_wipe(void) {
	printf("\n--- Braceless Switch Defer Wipe ---\n");

	// Defer in outer switch must survive a braceless inner switch
	{
		PrismResult r = prism_transpile_source(
		    "int result = 0;\n"
		    "void test(int outer, int inner) {\n"
		    "    switch (outer) {\n"
		    "        case 0: {\n"
		    "            defer { result = 42; }\n"
		    "            switch (inner)\n"
		    "                if (inner == 1) {\n"
		    "                    case 1:\n"
		    "                        break;\n"
		    "                }\n"
		    "        }\n"
		    "    }\n"
		    "}\n",
		    "braceless_sw_defer.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "braceless-sw-defer: transpiles OK");
		CHECK(r.output != NULL, "braceless-sw-defer: output not NULL");
		// The defer body must appear in the output (not be wiped)
		CHECK(strstr(r.output, "result = 42") != NULL,
		      "braceless-sw-defer: defer must not be wiped by braceless inner switch");
		prism_free(&r);
	}
}

// Noreturn in braceless if(0) inside defer: __builtin_unreachable must not leak
static void test_defer_braceless_noreturn_unreachable_leak(void) {
	printf("\n--- unreachable in braceless ctrl-flow defer ---\n");
	// noreturn call in braceless if(0) inside defer body:
	// __builtin_unreachable() must NOT appear outside the if-body
	PrismResult r = prism_transpile_source(
	    "_Noreturn void die(void);\n"
	    "void f(void) {\n"
	    "    defer {\n"
	    "        if (0)\n"
	    "            die();\n"
	    "    }\n"
	    "}\n",
	    "t82_braceless.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "transpiles OK");
	if (r.status == PRISM_OK && r.output) {
		const char *fn = strstr(r.output, "void f(");
		CHECK(fn != NULL, "function found");
		if (fn)
			CHECK(strstr(fn, "__builtin_unreachable") == NULL &&
			      strstr(fn, "__assume") == NULL,
			      "no unreachable leak outside braceless if");
	}
	prism_free(&r);

	// Top-level noreturn in defer body SHOULD still get unreachable
	r = prism_transpile_source(
	    "_Noreturn void die(void);\n"
	    "void f(void) {\n"
	    "    defer { die(); }\n"
	    "}\n",
	    "t82_toplevel.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "transpiles OK");
	if (r.status == PRISM_OK && r.output) {
		CHECK(strstr(r.output, "__builtin_unreachable") != NULL ||
		      strstr(r.output, "__assume") != NULL,
		      "unreachable injected at top level");
	}
	prism_free(&r);
}

// Braceless ctrl-flow inside defer: multi-stmt expansion must be brace-wrapped
static void test_defer_braceless_ctrl_brace_wrap(void) {
	printf("\n--- braceless ctrl-flow brace_wrap in defer ---\n");
	// typeof zeroinit in braceless if inside defer: memset must be inside braces
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    defer {\n"
		    "        if (1)\n"
		    "            typeof(int) x;\n"
		    "        (void)0;\n"
		    "    }\n"
		    "}\n",
		    "t85_typeof_if.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			CHECK(strstr(r.output, "if (1) {") != NULL ||
			      strstr(r.output, "if (1){") != NULL,
			      "braceless if body wrapped in braces");
			CHECK(has_zeroing(r.output),
			      "memset generated");
		}
		prism_free(&r);
	}
	// typeof zeroinit in braceless for inside defer
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    defer {\n"
		    "        for (int i = 0; i < 1; i++)\n"
		    "            typeof(int) x;\n"
		    "    }\n"
		    "}\n",
		    "t85_typeof_for.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			const char *fn = strstr(r.output, "void f(");
			CHECK(fn != NULL, "function found");
			if (fn) {
				CHECK(has_zeroing(fn),
				      "memset generated");
			}
		}
		prism_free(&r);
	}
	// typeof zeroinit in braceless else inside defer
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    defer {\n"
		    "        if (0) (void)0;\n"
		    "        else\n"
		    "            typeof(int) x;\n"
		    "    }\n"
		    "}\n",
		    "t85_typeof_else.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			CHECK(has_zeroing(r.output),
			      "memset generated");
		}
		prism_free(&r);
	}
	// orelse decl in braceless if inside defer: multi-stmt must be wrapped
	{
		PrismResult r = prism_transpile_source(
		    "int *get_null(void);\n"
		    "void f(void) {\n"
		    "    defer {\n"
		    "        if (1)\n"
		    "            int *p = get_null() orelse 0;\n"
		    "    }\n"
		    "}\n",
		    "t85_orelse_if.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			const char *fn = strstr(r.output, "void f(");
			CHECK(fn != NULL, "function found");
			if (fn) {
				CHECK(strstr(fn, "orelse") == NULL,
				      "no literal orelse");
				CHECK(strstr(fn, "if (1) {") != NULL ||
				      strstr(fn, "if (1){") != NULL,
				      "braceless body wrapped in braces");
			}
		}
		prism_free(&r);
	}
}

// Orelse block body: ctrl-flow keyword tracking for braceless sub-bodies
static void test_orelse_block_braceless_ctrl_zeroinit(void) {
	printf("\n--- ctrl-flow in orelse block body ---\n");
	// typeof zeroinit in braceless if inside orelse block
	{
		PrismResult r = prism_transpile_source(
		    "int *get_null(void);\n"
		    "void f(void) {\n"
		    "    int *p = get_null() orelse {\n"
		    "        if (1)\n"
		    "            typeof(int) x;\n"
		    "        return;\n"
		    "    };\n"
		    "}\n",
		    "t85b_typeof_if.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			const char *fn = strstr(r.output, "void f(");
			CHECK(fn != NULL, "function found");
			if (fn) {
				CHECK(has_zeroing(fn),
				      "memset generated for typeof");
			}
		}
		prism_free(&r);
	}
	// orelse in braceless while inside orelse block
	{
		PrismResult r = prism_transpile_source(
		    "int f(int *p);\n"
		    "void g(void) {\n"
		    "    int *q = (int*)(0) orelse {\n"
		    "        while (0)\n"
		    "            typeof(int) y;\n"
		    "        return;\n"
		    "    };\n"
		    "    (void)q;\n"
		    "}\n",
		    "t85b_while.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "transpiles OK");
		if (r.status == PRISM_OK && r.output) {
			const char *fn = strstr(r.output, "void g(");
			CHECK(fn != NULL, "function found");
			if (fn) {
				CHECK(has_zeroing(fn),
				      "memset generated");
			}
		}
		prism_free(&r);
	}
}

// Vfork taint propagation through function pointer chains
static void test_defer_vfork_taint_funcptr_chain(void) {
	printf("\n--- Taint propagation: non-wrapper fp chain ---\n");
	const char *code =
	    "#include <unistd.h>\n"
	    "void f0(void) { vfork(); }\n"
	    "void f1(void) { void (*fp)(void) = f0; fp(); }\n"
	    "void f2(void) { void (*fp)(void) = f1; fp(); }\n"
	    "void f3(void) {\n"
	    "    defer { (void)0; }\n"
	    "    f2();\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "taint_fp_chain.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "taint-fp-chain: deep non-wrapper vfork chain must be rejected");
	CHECK(r.error_msg && strstr(r.error_msg, "vfork"),
	      "taint-fp-chain: error mentions vfork");
	prism_free(&r);
}

// C23 [[...]] attribute on return type must not be consumed into typedef
static void test_defer_c23_attr_return_type_typedef(void) {
	const char *code =
	    "void cleanup(void);\n"
	    "int (*get_array(void))[3] [[gnu::noinline]] {\n"
	    "    static int arr[3] = {1, 2, 3};\n"
	    "    defer cleanup();\n"
	    "    return &arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "c23_attr_ret.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "c23-attr-ret: transpiles OK");
	CHECK(r.output != NULL, "c23-attr-ret: output not NULL");
	const char *td = r.output ? strstr(r.output, "typedef") : NULL;
	CHECK(td != NULL, "c23-attr-ret: must emit typedef");
	const char *td_semi = td ? strchr(td, ';') : NULL;
	bool has_attr = false;
	if (td && td_semi) {
		for (const char *p = td; p < td_semi; p++) {
			if (memcmp(p, "noinline", 8) == 0) { has_attr = true; break; }
		}
	}
	CHECK(!has_attr,
	      "c23-attr-ret: typedef must not contain [[gnu::noinline]]");
	prism_free(&r);
}

// Orelse in stmt-expr inside ctrl-flow condition inside defer body
static void test_defer_orelse_in_stmtexpr_ctrl_condition(void) {
	printf("\n--- orelse in stmt-expr ctrl-flow condition in defer ---\n");
	{
		const char *code =
		    "int get(void);\n"
		    "void f(void) {\n"
		    "    defer {\n"
		    "        if (({ int x = get() orelse 1; x; })) {\n"
		    "            (void)0;\n"
		    "        }\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "dr_stmtexpr1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "stmt-expr-in-defer-if: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "stmt-expr-in-defer-if: no raw orelse in output");
		}
		prism_free(&r);
	}
	// while variant
	{
		const char *code =
		    "int get(void);\n"
		    "void f(void) {\n"
		    "    defer {\n"
		    "        while (({ int x = get() orelse 0; x; })) {\n"
		    "            break;\n"
		    "        }\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "dr_stmtexpr2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "stmt-expr-in-defer-while: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "stmt-expr-in-defer-while: no raw orelse in output");
		}
		prism_free(&r);
	}
	// orelse in stmt-expr in ctrl-flow inside orelse block body
	{
		const char *code =
		    "int get(void);\n"
		    "int f(void) {\n"
		    "    int result = get() orelse {\n"
		    "        if (({ int x = get() orelse 1; x; })) {\n"
		    "            return 1;\n"
		    "        }\n"
		    "        return 0;\n"
		    "    };\n"
		    "    return result;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "bb_stmtexpr1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "stmt-expr-in-orelse-block-if: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "stmt-expr-in-orelse-block-if: no raw orelse in output");
		}
		prism_free(&r);
	}
}

// Zeroinit in for-init declarations inside defer/orelse block bodies
static void test_defer_zeroinit_for_init_ctrl_flow(void) {
	printf("\n--- zeroinit in ctrl-flow conditions inside defer/orelse ---\n");
	// for-init inside defer
	{
		const char *code =
		    "void f(void) {\n"
		    "    defer {\n"
		    "        for (int uninit_var;;) {\n"
		    "            break;\n"
		    "        }\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "zi_forinit_defer.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "zeroinit-for-defer: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "uninit_var = 0") != NULL,
			      "zeroinit-for-defer: for-init var zero-initialized");
		}
		prism_free(&r);
	}
	// for-init inside orelse block body
	{
		const char *code =
		    "int get(void);\n"
		    "int f(void) {\n"
		    "    int result = get() orelse {\n"
		    "        for (int uninit_var;;) {\n"
		    "            break;\n"
		    "        }\n"
		    "        return 0;\n"
		    "    };\n"
		    "    return result;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "zi_forinit_orelse.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "zeroinit-for-orelse-block: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "uninit_var = 0") != NULL,
			      "zeroinit-for-orelse-block: for-init var zero-initialized");
		}
		prism_free(&r);
	}
	// while condition with stmt-expr containing zeroinit
	{
		const char *code =
		    "void f(void) {\n"
		    "    defer {\n"
		    "        while (({ int x = 0; x; })) {\n"
		    "            break;\n"
		    "        }\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "zi_while_defer.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "zeroinit-while-stmtexpr: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "zeroinit-while-stmtexpr: no raw orelse leaked");
		}
		prism_free(&r);
	}
}

// Initializer braces must not inflate block_depth (SCOPE_INIT tracking)
static void test_defer_scope_init_block_depth_desync(void) {
	printf("\n--- SCOPE_INIT block_depth desync ---\n");
	// goto inside stmt-expr in initializer brace, with active defer
	{
		PrismFeatures opts = prism_defaults();
		opts.zeroinit = false;
		const char *code =
		    "int flag;\n"
		    "int f(void) {\n"
		    "    {\n"
		    "        defer flag = 1;\n"
		    "        int x = { ({ goto L; 0; }) };\n"
		    "    }\n"
		    "L:\n"
		    "    return flag;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "scope_init1.c", opts);
		CHECK_EQ(r.status, PRISM_OK, "scope-init-desync: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "flag = 1") != NULL,
			      "scope-init-desync: defer cleanup emitted before goto");
		}
		prism_free(&r);
	}
	// Same test with zeroinit ON — must also work (regression guard)
	{
		const char *code =
		    "int flag;\n"
		    "int f(void) {\n"
		    "    {\n"
		    "        defer flag = 1;\n"
		    "        int x = { ({ goto L; 0; }) };\n"
		    "    }\n"
		    "L:\n"
		    "    return flag;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "scope_init2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "scope-init-zeroinit-on: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "flag = 1") != NULL,
			      "scope-init-zeroinit-on: defer cleanup emitted before goto");
		}
		prism_free(&r);
	}
	// Nested initializer braces — multiple levels of = { { ... } }
	{
		PrismFeatures opts = prism_defaults();
		opts.zeroinit = false;
		const char *code =
		    "typedef struct { int a[2]; } S;\n"
		    "int flag;\n"
		    "int f(void) {\n"
		    "    {\n"
		    "        defer flag = 1;\n"
		    "        S s = { { ({ goto L; 0; }), 0 } };\n"
		    "    }\n"
		    "L:\n"
		    "    return flag;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "scope_init3.c", opts);
		CHECK_EQ(r.status, PRISM_OK, "scope-init-nested: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "flag = 1") != NULL,
			      "scope-init-nested: defer cleanup with nested init braces");
		}
		prism_free(&r);
	}
}

static void test_ghost_enum_defer_shadow(void) {
	// Ghost enum in sizeof: enum constant shadows a defer-captured variable
	const char *code_sizeof =
		"int secret;\n"
		"void f(void) {\n"
		"    defer (void)secret;\n"
		"    int x = sizeof(enum { secret = 0 });\n"
		"}\n";
	PrismResult r1 = prism_transpile_source(code_sizeof, "ghost_sz.c", prism_defaults());
	CHECK(r1.status != PRISM_OK,
	      "ghost enum defer shadow (sizeof): must be rejected");
	CHECK(r1.error_msg && strstr(r1.error_msg, "shadows"),
	      "ghost enum defer shadow (sizeof): error mentions shadows");
	prism_free(&r1);

	// Ghost enum in cast
	const char *code_cast =
		"int key;\n"
		"void f(void) {\n"
		"    defer (void)key;\n"
		"    int x = (enum { key = 0 })0;\n"
		"}\n";
	PrismResult r2 = prism_transpile_source(code_cast, "ghost_cast.c", prism_defaults());
	CHECK(r2.status != PRISM_OK,
	      "ghost enum defer shadow (cast): must be rejected");
	prism_free(&r2);

	// Ghost enum in typeof
	const char *code_typeof =
		"int val;\n"
		"void f(void) {\n"
		"    defer (void)val;\n"
		"    typeof(enum { val = 0 }) x;\n"
		"}\n";
	PrismResult r3 = prism_transpile_source(code_typeof, "ghost_to.c", prism_defaults());
	CHECK(r3.status != PRISM_OK,
	      "ghost enum defer shadow (typeof): must be rejected");
	prism_free(&r3);

	// Statement-start enum: already caught (sanity check)
	const char *code_stmt =
		"int val;\n"
		"void f(void) {\n"
		"    defer (void)val;\n"
		"    enum { val = 0 };\n"
		"}\n";
	PrismResult r4 = prism_transpile_source(code_stmt, "ghost_stmt.c", prism_defaults());
	CHECK(r4.status != PRISM_OK,
	      "ghost enum defer shadow (stmt): must be rejected");
	prism_free(&r4);

	// No shadow — must still compile fine
	const char *code_ok =
		"int val;\n"
		"void f(void) {\n"
		"    defer (void)val;\n"
		"    int x = sizeof(enum { unrelated = 0 });\n"
		"}\n";
	PrismResult r5 = prism_transpile_source(code_ok, "ghost_ok.c", prism_defaults());
	CHECK(r5.status == PRISM_OK,
	      "ghost enum no shadow: must compile OK");
	prism_free(&r5);
}

static void test_defer_stmtexpr_ctrl_head_escape(void) {
	// return in stmt-expr inside if-condition in defer
	const char *code_if_return =
		"#include <stdio.h>\n"
		"int main(void) {\n"
		"    defer {\n"
		"        if ( ({ return 1; 1; }) ) printf(\"x\\n\");\n"
		"    }\n"
		"    return 0;\n"
		"}\n";
	PrismResult r1 = prism_transpile_source(code_if_return, "dse_if.c", prism_defaults());
	CHECK(r1.status != PRISM_OK,
	      "defer stmt-expr in if-condition (return): must be rejected");
	CHECK(r1.error_msg && strstr(r1.error_msg, "return"),
	      "defer stmt-expr in if-condition: error mentions return");
	prism_free(&r1);

	// goto in stmt-expr inside if-condition in defer
	const char *code_if_goto =
		"#include <stdio.h>\n"
		"int main(void) {\n"
		"    defer {\n"
		"        if ( ({ goto bail; 1; }) ) printf(\"x\\n\");\n"
		"    }\n"
		"    return 0;\n"
		"bail: return 1;\n"
		"}\n";
	PrismResult r2 = prism_transpile_source(code_if_goto, "dse_go.c", prism_defaults());
	CHECK(r2.status != PRISM_OK,
	      "defer stmt-expr in if-condition (goto): must be rejected");
	CHECK(r2.error_msg && strstr(r2.error_msg, "goto"),
	      "defer stmt-expr in if-condition: error mentions goto");
	prism_free(&r2);

	// return in stmt-expr inside while-condition in defer
	const char *code_while =
		"#include <stdio.h>\n"
		"int main(void) {\n"
		"    defer {\n"
		"        while ( ({ return 1; 0; }) ) printf(\"x\\n\");\n"
		"    }\n"
		"    return 0;\n"
		"}\n";
	PrismResult r3 = prism_transpile_source(code_while, "dse_wh.c", prism_defaults());
	CHECK(r3.status != PRISM_OK,
	      "defer stmt-expr in while-condition: must be rejected");
	prism_free(&r3);

	// return in stmt-expr inside switch-condition in defer
	const char *code_switch =
		"#include <stdio.h>\n"
		"int main(void) {\n"
		"    defer {\n"
		"        switch ( ({ return 1; 0; }) ) { default: break; }\n"
		"    }\n"
		"    return 0;\n"
		"}\n";
	PrismResult r4 = prism_transpile_source(code_switch, "dse_sw.c", prism_defaults());
	CHECK(r4.status != PRISM_OK,
	      "defer stmt-expr in switch-condition: must be rejected");
	prism_free(&r4);

	// return in stmt-expr inside do-while condition in defer
	const char *code_do =
		"#include <stdio.h>\n"
		"int main(void) {\n"
		"    defer {\n"
		"        do { printf(\"x\\n\"); } while ( ({ return 1; 0; }) );\n"
		"    }\n"
		"    return 0;\n"
		"}\n";
	PrismResult r5 = prism_transpile_source(code_do, "dse_do.c", prism_defaults());
	CHECK(r5.status != PRISM_OK,
	      "defer stmt-expr in do-while condition: must be rejected");
	prism_free(&r5);

	// return in stmt-expr inside for-condition in defer
	const char *code_for =
		"#include <stdio.h>\n"
		"int main(void) {\n"
		"    defer {\n"
		"        for (int i = ({ return 1; 0; }); i < 1; i++) printf(\"x\\n\");\n"
		"    }\n"
		"    return 0;\n"
		"}\n";
	PrismResult r6 = prism_transpile_source(code_for, "dse_for.c", prism_defaults());
	CHECK(r6.status != PRISM_OK,
	      "defer stmt-expr in for-condition: must be rejected");
	prism_free(&r6);

	// Valid: stmt-expr in condition WITHOUT control flow must still work
	const char *code_ok =
		"#include <stdio.h>\n"
		"int main(void) {\n"
		"    defer {\n"
		"        if ( ({ int x = 1; x; }) ) printf(\"valid\\n\");\n"
		"    }\n"
		"    return 0;\n"
		"}\n";
	PrismResult r7 = prism_transpile_source(code_ok, "dse_ok.c", prism_defaults());
	CHECK(r7.status == PRISM_OK,
	      "defer stmt-expr in condition (no ctrl flow): must compile OK");
	prism_free(&r7);
}

// emit_deferred_range has no scope tracking — struct/union/enum
// field definitions inside defer blocks got = 0 zero-init injected.
static void test_defer_sue_body_zeroinit(void) {
	printf("\n--- struct/union/enum in defer body ---\n");

	// Struct type-only definition inside defer block
	{
		const char *code =
			"void use(void *p);\n"
			"void f(void) {\n"
			"    defer {\n"
			"        struct S { int x; char buf[32]; };\n"
			"        struct S s;\n"
			"        use(&s);\n"
			"    }\n"
			"    return;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "sue_struct.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "struct in defer: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "int x;") != NULL,
			      "struct in defer: field has no zero-init");
			CHECK(strstr(r.output, "int x = 0") == NULL,
			      "struct in defer: no field = 0 injection");
		}
		prism_free(&r);
	}

	// Union inside defer block
	{
		const char *code =
			"void use(void *p);\n"
			"void f(void) {\n"
			"    defer {\n"
			"        union U { int i; float f; };\n"
			"        union U u;\n"
			"        use(&u);\n"
			"    }\n"
			"    return;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "sue_union.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "union in defer: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "int i = 0") == NULL,
			      "union in defer: no field = 0 injection");
		}
		prism_free(&r);
	}

	// Enum inside defer block
	{
		const char *code =
			"void use(void *p);\n"
			"void f(void) {\n"
			"    defer {\n"
			"        enum Color { RED, GREEN, BLUE };\n"
			"        enum Color c;\n"
			"        use(&c);\n"
			"    }\n"
			"    return;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "sue_enum.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "enum in defer: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "RED = 0") == NULL,
			      "enum in defer: no constant = 0 injection");
		}
		prism_free(&r);
	}

	// Struct with variable declarator inside defer
	{
		const char *code =
			"void use(void *p);\n"
			"void f(void) {\n"
			"    defer {\n"
			"        struct P { int x; int y; } p;\n"
			"        use(&p);\n"
			"    }\n"
			"    return;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "sue_var.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "struct+var in defer: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "int x = 0") == NULL,
			      "struct+var in defer: no field = 0 injection");
			// Variable 's' SHOULD get zero-init
			CHECK(strstr(r.output, "p = {0}") != NULL,
			      "struct+var in defer: variable gets zero-init");
		}
		prism_free(&r);
	}

	// Nested struct inside defer
	{
		const char *code =
			"void use(void *p);\n"
			"void f(void) {\n"
			"    defer {\n"
			"        struct Outer {\n"
			"            struct Inner { int a; int b; } inner;\n"
			"            int c;\n"
			"        };\n"
			"        struct Outer o;\n"
			"        use(&o);\n"
			"    }\n"
			"    return;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "sue_nest.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "nested struct in defer: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "int a = 0") == NULL,
			      "nested struct in defer: no inner field = 0");
			CHECK(strstr(r.output, "int c = 0") == NULL,
			      "nested struct in defer: no outer field = 0");
		}
		prism_free(&r);
	}

	// raw keyword inside struct body in defer must be stripped
	{
		const char *code =
			"void f(void) {\n"
			"    defer {\n"
			"        struct R { raw int magic; int len; };\n"
			"    }\n"
			"    return;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "sue_rw.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "raw in struct defer: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "raw") == NULL,
			      "raw in struct defer: raw keyword stripped");
			CHECK(strstr(r.output, "int magic;") != NULL,
			      "raw in struct defer: field preserved without raw");
		}
		prism_free(&r);
	}

	// raw keyword inside prefixed struct body in defer
	{
		const char *code =
			"void use(void *p);\n"
			"void f(void) {\n"
			"    defer {\n"
			"        __extension__ struct X { raw int a; int b; };\n"
			"        struct X v;\n"
			"        use(&v);\n"
			"    }\n"
			"    return;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "sue_ext_rw.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "raw in prefixed struct defer: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "raw") == NULL,
			      "raw in prefixed struct defer: raw keyword stripped");
		}
		prism_free(&r);
	}
}

// declaration prefixes (__extension__, [[...]], _Pragma, volatile, etc.)
// before struct/union/enum consumed at_stmt_start, bypassing the SUE body
// verbatim-emit guard. Fields got zero-init injected.
static void test_defer_sue_prefix_bypass(void) {
	printf("\n--- SUE prefix bypass in defer body ---\n");

	// __extension__ struct type-only
	{
		const char *code =
			"void use(void *p);\n"
			"void f(void) {\n"
			"    defer {\n"
			"        __extension__ struct E { int x; int y; };\n"
			"        struct E v;\n"
			"        use(&v);\n"
			"    }\n"
			"    return;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "sue_ext.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "__extension__ struct in defer: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "int x = 0") == NULL,
			      "__extension__ struct in defer: no field = 0");
			CHECK(strstr(r.output, "int y = 0") == NULL,
			      "__extension__ struct in defer: no field y = 0");
		}
		prism_free(&r);
	}

	// __extension__ union type-only
	{
		const char *code =
			"void use(void *p);\n"
			"void f(void) {\n"
			"    defer {\n"
			"        __extension__ union U { int i; float f; };\n"
			"        union U v;\n"
			"        use(&v);\n"
			"    }\n"
			"    return;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "sue_ext_u.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "__extension__ union in defer: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "int i = 0") == NULL,
			      "__extension__ union in defer: no field = 0");
		}
		prism_free(&r);
	}

	// C23 [[...]] attribute + struct type-only
	{
		const char *code =
			"void use(void *p);\n"
			"void f(void) {\n"
			"    defer {\n"
			"        [[gnu::packed]] struct A { int a; int b; };\n"
			"        struct A v;\n"
			"        use(&v);\n"
			"    }\n"
			"    return;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "sue_c23.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "C23 attr struct in defer: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "int a = 0") == NULL,
			      "C23 attr struct in defer: no field = 0");
		}
		prism_free(&r);
	}

	// volatile struct type-only
	{
		const char *code =
			"void use(void *p);\n"
			"void f(void) {\n"
			"    defer {\n"
			"        volatile struct V { int x; int y; };\n"
			"        struct V v;\n"
			"        use(&v);\n"
			"    }\n"
			"    return;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "sue_vol.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "volatile struct in defer: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "int x = 0") == NULL,
			      "volatile struct in defer: no field = 0");
		}
		prism_free(&r);
	}

	// __extension__ struct with variable (should still zero-init the variable)
	{
		const char *code =
			"void use(void *p);\n"
			"void f(void) {\n"
			"    defer {\n"
			"        __extension__ struct { int a; int b; } ev;\n"
			"        use(&ev);\n"
			"    }\n"
			"    return;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "sue_ext_v.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "__extension__ struct+var in defer: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "int a = 0") == NULL,
			      "__extension__ struct+var in defer: no field = 0");
			CHECK(strstr(r.output, "ev = {0}") != NULL,
			      "__extension__ struct+var in defer: var gets zero-init");
		}
		prism_free(&r);
	}
}

static void test_braceless_shadow_scope_leak(void) {
	printf("\n--- braceless body shadow scope leak ---\n");

	// Case 1: typedef shadow from braceless body shouldn't poison rest of function.
	// Zero-init must still work after the braceless body ends.
	{
		const char *code =
			"typedef int Arr5[5];\n"
			"void f(int bypass) {\n"
			"    if (bypass)\n"
			"        int Arr5;\n"
			"    Arr5 buf;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "shd_lk1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "shadow scope leak case 1: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "buf = {0}") != NULL,
			      "shadow scope leak case 1: typedef recovered, gets = {0}");
		}
		prism_free(&r);
	}

	// Case 2: VLA typedef + goto-over-VLA must be caught despite braceless shadow.
	{
		const char *code =
			"void f(int n, int bypass) {\n"
			"    typedef int VT[n];\n"
			"    if (bypass)\n"
			"        int VT;\n"
			"    goto L;\n"
			"    VT vla;\n"
			"L:\n"
			"    return;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "shd_lk2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "shadow scope leak case 2: goto over VLA rejected");
		prism_free(&r);
	}

	// Case 3: while braceless body shadow also properly scoped.
	{
		const char *code =
			"typedef int Arr3[3];\n"
			"void f(int c) {\n"
			"    while (c)\n"
			"        int Arr3;\n"
			"    Arr3 buf;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "shd_lk3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "shadow scope leak case 3: while body OK");
		if (r.output) {
			CHECK(strstr(r.output, "buf = {0}") != NULL,
			      "shadow scope leak case 3: typedef recovered after while");
		}
		prism_free(&r);
	}

	// Case 4: VLA variable entry in braceless body doesn't leak.
	{
		const char *code =
			"void f(int n, int bypass) {\n"
			"    if (bypass)\n"
			"        int vla[n];\n"
			"    goto L;\n"
			"    int x;\n"
			"L:\n"
			"    return;\n"
			"}\n";
		PrismResult r = prism_transpile_source(code, "shd_lk4.c", prism_defaults());
		// The goto over 'int x' (non-VLA, has init via zeroinit) should be caught.
		// But crucially, the VLA in the braceless body must NOT poison the scope.
		CHECK(r.status != PRISM_OK,
		      "shadow scope leak case 4: goto over initialized decl rejected");
		prism_free(&r);
	}
}

/* emit_statements `:` label handler was gated on mode != EMIT_DEFER_BODY,
 * preventing case/default labels from resetting at_stmt_start in defer bodies.
 * Bare orelse after case labels leaked verbatim to C output. */
static void test_defer_case_label_orelse_leak(void) {
	printf("\n--- Defer case label orelse leak ---\n");

	/* Bare orelse after case label in defer body */
	{
		PrismResult r = prism_transpile_source(
		    "int get_val(void);\n"
		    "void f(int e) {\n"
		    "    defer {\n"
		    "        int result;\n"
		    "        switch(e) {\n"
		    "        case 1: {\n"
		    "            result = get_val() orelse 5;\n"
		    "            break;\n"
		    "        }\n"
		    "        default: {\n"
		    "            result = 0;\n"
		    "            break;\n"
		    "        }\n"
		    "        }\n"
		    "        (void)result;\n"
		    "    };\n"
		    "}\n",
		    "defer_case_oe.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "defer case orelse: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "defer case orelse: no orelse keyword leak");
			CHECK(strstr(r.output, "__prism_oe_") != NULL ||
			      strstr(r.output, "if (") != NULL,
			      "defer case orelse: orelse expanded");
		}
		prism_free(&r);
	}

	/* Default label with orelse in defer body */
	{
		PrismResult r = prism_transpile_source(
		    "int get_val(void);\n"
		    "void f(int e) {\n"
		    "    defer {\n"
		    "        int result;\n"
		    "        switch(e) {\n"
		    "        default: {\n"
		    "            result = get_val() orelse -1;\n"
		    "            break;\n"
		    "        }\n"
		    "        }\n"
		    "        (void)result;\n"
		    "    };\n"
		    "}\n",
		    "defer_def_oe.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "defer default orelse: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "defer default orelse: no orelse keyword leak");
		}
		prism_free(&r);
	}
}

// defer_body_refs_name bd>1 missed top-level locals in block defer.
// `defer { int x = 1; }` followed by `int x = 2;` falsely flagged as shadow
// because the scanner didn't recognize top-level (bd==1) declarations as local.
static void test_defer_shadow_toplevel_local_false_positive(void) {
	printf("\n--- Defer shadow top-level local false positive ---\n");

	// Top-level local in defer body must not cause false-positive shadow.
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    defer { int x = 1; (void)x; }\n"
		    "    { int x = 2; (void)x; return; }\n"
		    "}\n",
		    "defer_toplevel_local.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "defer-toplevel-local: top-level int x in defer is local, no shadow");
		prism_free(&r);
	}

	// Real shadow at top level must still be caught.
	{
		PrismResult r = prism_transpile_source(
		    "int get(void);\n"
		    "void f(void) {\n"
		    "    int x = get();\n"
		    "    defer { (void)x; }\n"
		    "    { int x = 99; (void)x; return; }\n"
		    "}\n",
		    "defer_toplevel_real.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "defer-toplevel-real: top-level x reference in defer still caught");
		prism_free(&r);
	}

	// Multiple locals at top level in block defer.
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    defer {\n"
		    "        int a = 1, b = 2;\n"
		    "        (void)a; (void)b;\n"
		    "    }\n"
		    "    { int a = 10; int b = 20; (void)a; (void)b; return; }\n"
		    "}\n",
		    "defer_toplevel_multi.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "defer-toplevel-multi: multiple locals in defer not false positive");
		prism_free(&r);
	}
}

// skip_to_semicolon in validate_defer_statement escaped past } when
// a semicolon was missing inside a defer block, causing Phase 1F to validate
// the entire rest of the file as if inside the defer.
static void test_defer_validator_escape_missing_semicolon(void) {
	printf("\n--- Defer validator escape: missing semicolon ---\n");

	// Missing semicolon in defer block must NOT cause valid return in
	// a later function to be falsely rejected as "return inside defer".
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(void);\n"
		    "void task_a(void) {\n"
		    "    defer {\n"
		    "        int x = 5\n"  // missing ;
		    "    };\n"
		    "    return;\n"       // Phase 1F would falsely reject this
		    "}\n"
		    "void task_b(void) {\n"
		    "    return;\n"       // Also falsely rejected before the fix
		    "}\n",
		    "defer_escape.c", prism_defaults());
		// This should NOT produce "return inside defer block" error.
		// The missing ; is a C syntax error (backend handles it), not
		// a Prism structural error.
		bool false_reject = (r.status != PRISM_OK && r.error_msg &&
		    strstr(r.error_msg, "inside defer"));
		CHECK(!false_reject,
		      "defer-escape: missing ; must not cause false 'return inside defer'");
		prism_free(&r);
	}

	// Nested defer in second function must not be falsely rejected.
	{
		PrismResult r = prism_transpile_source(
		    "void task_a(void) {\n"
		    "    defer {\n"
		    "        int x = 5\n"  // missing ;
		    "    };\n"
		    "}\n"
		    "void f(void);\n"
		    "void task_b(void) {\n"
		    "    defer f();\n"     // valid defer — was falsely flagged
		    "}\n",
		    "defer_escape2.c", prism_defaults());
		bool false_reject = (r.status != PRISM_OK && r.error_msg &&
		    strstr(r.error_msg, "inside defer"));
		CHECK(!false_reject,
		      "defer-escape2: missing ; must not cause false error in other function");
		prism_free(&r);
	}
}

// Phase 1D enum defer shadow check unconditionally hard-errored for
// enclosing-scope shadows, even when no control-flow exit occurs while
// the enum constant is live. C11 §6.2.1p4: enum constants have block scope.
static void test_enum_defer_shadow_false_positive(void) {
	printf("\n--- enum defer shadow false positive ---\n");

	// Case 1: inner-scope enum shadows defer-captured name, no return — safe
	{
		PrismResult r = prism_transpile_source(
		    "void close_stream(void);\n"
		    "void process_data(void) {\n"
		    "    int mode = 0;\n"
		    "    defer { if (mode == 0) close_stream(); }\n"
		    "    if (1) {\n"
		    "        enum { mode = 1 };\n"
		    "        (void)mode;\n"
		    "    }\n"
		    "}\n",
		    "enum_safe.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "enum-shadow-safe: transpiles OK");
		prism_free(&r);
	}

	// Case 2: inner-scope enum shadows with return — must be caught
	{
		PrismResult r = prism_transpile_source(
		    "void close_stream(void);\n"
		    "void process_data(void) {\n"
		    "    int mode = 0;\n"
		    "    defer { if (mode == 0) close_stream(); }\n"
		    "    if (1) {\n"
		    "        enum { mode = 1 };\n"
		    "        (void)mode;\n"
		    "        return;\n"
		    "    }\n"
		    "}\n",
		    "enum_ret.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "enum-shadow-return: detected");
		prism_free(&r);
	}

	// Case 3: same-scope enum shadow — always fatal (unconditional)
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(int);\n"
		    "void test(void) {\n"
		    "    int handle = 42;\n"
		    "    defer cleanup(handle);\n"
		    "    enum { handle = 99 };\n"
		    "    (void)handle;\n"
		    "}\n",
		    "enum_same.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "enum-shadow-same-scope: detected");
		prism_free(&r);
	}

	// Case 4: sizeof(enum {...}) expression-context shadow — safe without return
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(int);\n"
		    "void test(void) {\n"
		    "    int handle = 42;\n"
		    "    defer cleanup(handle);\n"
		    "    if (1) {\n"
		    "        int x = sizeof(enum { handle = 1 });\n"
		    "        (void)x;\n"
		    "    }\n"
		    "}\n",
		    "enum_sizeof_safe.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "sizeof-enum-shadow-safe: transpiles OK");
		prism_free(&r);
	}

	// Case 5: sizeof(enum {...}) with return after — must be caught
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(int);\n"
		    "void test(void) {\n"
		    "    int handle = 42;\n"
		    "    defer cleanup(handle);\n"
		    "    if (1) {\n"
		    "        int x = sizeof(enum { handle = 1 });\n"
		    "        (void)x;\n"
		    "        return;\n"
		    "    }\n"
		    "}\n",
		    "enum_sizeof_ret.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "sizeof-enum-shadow-return: detected");
		prism_free(&r);
	}

	// Case 6: enum in array dimension (walk_balanced_orelse path) with return
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(int);\n"
		    "void test(void) {\n"
		    "    int handle = 42;\n"
		    "    defer cleanup(handle);\n"
		    "    if (1) {\n"
		    "        int arr[sizeof(enum { handle = 1 })];\n"
		    "        (void)arr;\n"
		    "        return;\n"
		    "    }\n"
		    "}\n",
		    "enum_arrdim_ret.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "enum-in-array-dim-return: detected");
		prism_free(&r);
	}

	// Case 7: enum in array dimension without return — safe
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(int);\n"
		    "void test(void) {\n"
		    "    int handle = 42;\n"
		    "    defer cleanup(handle);\n"
		    "    if (1) {\n"
		    "        int arr[sizeof(enum { handle = 1 })];\n"
		    "        (void)arr;\n"
		    "    }\n"
		    "}\n",
		    "enum_arrdim_safe.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "enum-in-array-dim-safe: transpiles OK");
		prism_free(&r);
	}
}

// chained orelse in defer body — premature break; in scanner
// stopped after first orelse, allowing control-flow to leak.
static void test_defer_chained_orelse_control_flow_leak(void) {
	printf("\n--- chained orelse defer control-flow leak ---\n");

	// Case 1: double chain — benign orelse hides return
	{
		PrismResult r = prism_transpile_source(
		    "int f(void);\n"
		    "void g(void) {\n"
		    "    defer { int x = (f() orelse 0 orelse return); };\n"
		    "}\n",
		    "chain_oe_ret.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "chained orelse return in defer must be rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "return") != NULL,
			      "error message mentions 'return'");
		prism_free(&r);
	}

	// Case 2: triple chain — goto hidden behind two benign orelses
	{
		PrismResult r = prism_transpile_source(
		    "int f(void);\n"
		    "void g(void) {\n"
		    "    defer { int x = (f() orelse 1 orelse 2 orelse goto done); };\n"
		    "done:;\n"
		    "}\n",
		    "chain_oe_goto.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "triple-chained orelse goto in defer must be rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "goto") != NULL,
			      "error message mentions 'goto'");
		prism_free(&r);
	}

	// Case 3: nested parens — double-nested paren evasion
	{
		PrismResult r = prism_transpile_source(
		    "int f(void);\n"
		    "void g(void) {\n"
		    "    defer { int x = ((f() orelse 0 orelse return)); };\n"
		    "}\n",
		    "chain_oe_paren.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "nested-paren chained orelse return in defer must be rejected");
		prism_free(&r);
	}

	// Case 4: chained orelse with block-form action
	{
		PrismResult r = prism_transpile_source(
		    "int f(void);\n"
		    "void g(void) {\n"
		    "    defer { int x = (f() orelse 0 orelse { return; }); };\n"
		    "}\n",
		    "chain_oe_block.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "chained orelse block return in defer must be rejected");
		prism_free(&r);
	}

	// Case 5: bare (non-paren) chained orelse in defer body
	{
		PrismResult r = prism_transpile_source(
		    "int f(void);\n"
		    "void use(int);\n"
		    "void g(void) {\n"
		    "    defer {\n"
		    "        int x = f() orelse 0 orelse return;\n"
		    "    };\n"
		    "}\n",
		    "chain_oe_bare.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "bare chained orelse return in defer must be rejected");
		prism_free(&r);
	}

	// Case 6: bracket orelse chain in defer — should be caught
	{
		PrismResult r = prism_transpile_source(
		    "int f(void);\n"
		    "void g(void) {\n"
		    "    defer {\n"
		    "        int arr[f() orelse 0 orelse break];\n"
		    "        (void)arr;\n"
		    "    };\n"
		    "}\n",
		    "chain_oe_bracket.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "bracket chained orelse break in defer must be rejected");
		prism_free(&r);
	}

	// Case 7: struct field named 'orelse' — no false positive via member access
	{
		PrismResult r = prism_transpile_source(
		    "typedef struct { int orelse; } S;\n"
		    "void cleanup(int);\n"
		    "void g(S s) {\n"
		    "    defer cleanup(s.orelse);\n"
		    "}\n",
		    "chain_oe_member.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "struct field named 'orelse' in defer must not false-positive");
		prism_free(&r);
	}

	// Case 8: single orelse (no chain) still caught
	{
		PrismResult r = prism_transpile_source(
		    "int f(void);\n"
		    "void g(void) {\n"
		    "    defer { int x = (f() orelse return); };\n"
		    "}\n",
		    "single_oe_ret.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "single orelse return in defer must still be rejected");
		prism_free(&r);
	}
}

// Enum body inside defer block: enum constants at brace_depth+1 caused
// defer_body_populate_captures to lose track of in_decl state, producing
// false shadow errors when a subsequent variable reused an enum constant name.
static void test_defer_body_enum_constant_shadow(void) {
	printf("\n--- defer body enum constant shadow ---\n");

	// Case 1: basic enum inside defer body, reuse constant name after
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    defer cleanup();\n"
		    "    defer { enum { A = 1, B = 2 }; };\n"
		    "    int B = 5;\n"
		    "    (void)B;\n"
		    "}\n",
		    "enum_body1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "enum-body-basic: no false shadow");
		prism_free(&r);
	}

	// Case 2: tagged enum inside defer body
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    defer cleanup();\n"
		    "    defer { enum Color { R, G, B }; };\n"
		    "    int G = 10;\n"
		    "    (void)G;\n"
		    "}\n",
		    "enum_body2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "enum-body-tagged: no false shadow");
		prism_free(&r);
	}

	// Case 3: attributed enum inside defer body (GCC)
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    defer cleanup();\n"
		    "    defer { enum __attribute__((packed)) E { X, Y, Z }; };\n"
		    "    int Y = 7;\n"
		    "    (void)Y;\n"
		    "}\n",
		    "enum_body3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "enum-body-attributed: no false shadow");
		prism_free(&r);
	}

	// Case 4: real shadow must still be caught — variable (not enum const)
	// captured by defer, then shadowed in same scope
	{
		PrismResult r = prism_transpile_source(
		    "void use(int);\n"
		    "void f(void) {\n"
		    "    int val = 1;\n"
		    "    defer use(val);\n"
		    "    int val = 2;\n"
		    "    (void)val;\n"
		    "}\n",
		    "enum_body4.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "real-shadow: still detected");
		prism_free(&r);
	}
}

static void test_defer_fno_orelse_paren_leak(void) {
	printf("\n--- -fno-orelse defer paren leak ---\n");
	PrismFeatures f = prism_defaults();
	f.orelse = false;

	// sizeof(defer cleanup()) — must be rejected even with -fno-orelse
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(void);\n"
		    "int main(void) { int x = sizeof(defer cleanup()); return x; }\n",
		    "fno_oe_sizeof.c", f);
		CHECK(r.status != PRISM_OK,
		      "defer in sizeof rejected with -fno-orelse");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "parenthesized") != NULL,
			      "-fno-orelse defer sizeof error mentions parenthesized");
		prism_free(&r);
	}

	// (defer cleanup(), 42) — must be rejected even with -fno-orelse
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(void);\n"
		    "int main(void) { int x = (defer cleanup(), 42); return x; }\n",
		    "fno_oe_comma.c", f);
		CHECK(r.status != PRISM_OK,
		      "defer in paren-comma rejected with -fno-orelse");
		prism_free(&r);
	}

	// init context: int x = f(defer cleanup()) with -fno-orelse
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(void);\n"
		    "int f(int);\n"
		    "int main(void) { int x = f(defer cleanup()); return x; }\n",
		    "fno_oe_call.c", f);
		CHECK(r.status != PRISM_OK,
		      "defer in call-arg rejected with -fno-orelse");
		prism_free(&r);
	}
}

static void test_defer_break_while_condition_stmt_expr(void) {
	PrismResult r = prism_transpile_source(
	    "#include <stdio.h>\n"
	    "void f(void) {\n"
	    "  defer { putchar('Z'); }\n"
	    "  while (({ if (1) break; 1; })) { putchar('B'); break; }\n"
	    "}\n",
	    "def_br_wcond.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
		 "defer + break inside stmt-expr in while(cond): must transpile "
		 "(ctrl-paren loop boundary for defer)");
	prism_free(&r);
}

static void test_defer_capture_shadow_stack_restore(void) {
	printf("\n--- defer_body_populate_captures shadow restore ---\n");
	const char *code = "void g(void) {\n"
			   "    {\n"
			   "        int x = 4;\n"
			   "        defer {\n"
			   "            int x = 1;\n"
			   "            { int x = 2; (void)x; }\n"
			   "            x = 3;\n"
			   "        }\n"
			   "        (void)x;\n"
			   "    }\n"
			   "    int x = 9;\n"
			   "    (void)x;\n"
			   "}\n";
	PrismResult r = prism_transpile_source(code, "defer_cap.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
		 "defer nested x shadow: capture scan must restore outer binding");
	prism_free(&r);
}

void run_defer_tests(void) {
	printf("\n=== DEFER TESTS ===\n");
        test_defer_in_comma_expr_rejected();

	test_defer_basic();
	test_defer_lifo();
	log_reset();
	int ret = test_defer_return();
	CHECK_LOG("1A", "defer with return");
	CHECK_EQ(ret, 42, "defer return value preserved");
	test_defer_goto_out();
	NOMSVC_ONLY(test_defer_goto_c23_attr());
	test_defer_nested_scopes();
	test_defer_break();
	test_defer_continue();
	test_defer_switch_break();
	test_defer_switch_fallthrough();
	test_defer_while();
	test_defer_break_while_condition_stmt_expr();
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
	GNUC_ONLY(
	test_typeof_void_defer_return();
	test_dunder_typeof_void_defer_return();
	);
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
	NOMSVC_ONLY(
	test_switch_stmt_expr_defer();
	test_switch_in_stmt_expr_in_switch();
	);
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

	test_defer_sibling_goto();
	test_defer_sibling_goto_multi();
	test_defer_sibling_goto_lifo();
	test_defer_cross_branch_goto_nested();

	test_defer_goto_forward();
	test_defer_goto_into_scope_rejected();
	test_defer_backward_goto_into_scope_rejected();
	test_defer_shadow_in_for_init();

	test_defer_backward_goto_sibling();

	NOMSVC_ONLY(test_defer_goto_c23_attr_label());
	test_defer_in_ctrl_paren_rejected();
	test_defer_braceless_control_rejected();
	GNUC_ONLY(
	test_defer_stmt_expr_top_level_rejected();
	test_defer_stmt_expr_nested_block_last_stmt_corrupts();
	test_defer_stmt_expr_return_bypass();
	test_defer_stmt_expr_goto_bypass();
	test_defer_computed_goto_rejected();
	// computed-goto forward-into-scope CFG bypass
	test_computed_goto_forward_into_deferred_scope();
	test_defer_asm_rejected();
	);
	test_defer_setjmp_rejected();
	test_defer_glibc_sigsetjmp_rejected();
	UNIX_ONLY(test_defer_vfork_rejected());
	test_auto_type_fallback_requires_gnu_extensions();
	test_ternary_cast_corrupts_label_detection();
	test_gnu_nested_func_breaks_outer_defer();
	test_knr_nested_func_detection();
	test_extern_decl_not_nested_func();
	test_stmt_expr_in_ctrl_cond_label();
	test_defer_scope_state_machine_overwrite();
	test_braceless_body_semicolon_trap();
	test_scope_type_at_depth_overflow();
	test_fno_defer_shadow_leak();
        test_defer_void_parens_return();
	test_defer_varname_return_value_dropped();
	test_defer_body_orelse_counter_desync();
	test_defer_body_orelse_block_overshoot();
	test_defer_braceless_decl_orelse_rejected();
	test_defer_body_bare_orelse_return_not_rejected();
	test_defer_paren_wrapped_orelse_smuggling();
	test_defer_label_duplication_rejected();
	test_defer_soft_keyword_labels_rejected();
	test_defer_soft_keyword_control_condition_rejected();
	test_defer_variable_shadowing_binds_wrong_scope();
	test_defer_safe_shadow_no_exit();
	test_defer_shadow_struct_member_false_positive();
	test_defer_shadow_overflow_silent_drop();
	UNIX_ONLY(
	test_defer_paren_vfork_bypass();
	test_defer_vfork_funcptr_bypass();
	test_defer_vfork_reference_false_positive();
	test_defer_vfork_local_alias_bypass();
	);
	test_defer_fnptr_return_type_overwrite();

	// hash table saturation CFG bypass
	test_cfg_hash_table_saturation_bypass();

	// braceless switch false positive
	test_braceless_switch_defer_false_positive();

	// braceless switch inside braced switch drops defers
	test_braceless_switch_defer_drop();

	// defer shadow inner-block false positive
	test_defer_shadow_inner_block_false_positive();

	// in_generic() scope leak
	test_defer_in_generic_stmt_expr();

	// user-defined _Noreturn function with defer gets no warning
	UNIX_ONLY(test_defer_user_noreturn_no_warning());

	// brace_depth > 1 skip bypasses captured vars in nested blocks
	test_defer_shadow_depth_bypass();

	// * dereference bypass + comma multi-decl false positive
	test_defer_shadow_deref_bypass();
	test_defer_shadow_comma_decl_false_positive();

	// cast blinds shadow checker
	test_defer_shadow_cast_bypass();

	// enum constant + typedef shadow defer captures
	test_defer_shadow_enum_bypass();
	test_defer_shadow_typedef_bypass();

	// empty statement / label between blocks defeats stmt-expr defer check
	test_defer_stmt_expr_empty_stmt_bypass();

	// typeof(x) inside nested defer block poisons shadow detection
	test_defer_typeof_shadow_bypass();

	// enum initializer with parens-wrapped comma hallucinates shadow
	test_defer_enum_initializer_comma();

	// double-nested block in stmt-expr bypasses scope_depth-2 check
	test_defer_stmt_expr_double_nested_block_bypass();

	// C23 attribute on computed goto bypasses CFG + defer checks
	test_c23_attr_computed_goto_defer_bypass();

	// stmt-expr smuggled inside function args/array indices/casts
	test_defer_stmt_expr_smuggled_in_args();

	// emit_expr_to_semicolon stmt-expr keyword bypass
	test_stmt_expr_return_defer_bypass();

	// compound literal ctrl_state desync shadow leak
	test_compound_literal_ctrl_state_desync();

	// nested stmt-expr in defer exponential blowup
	test_defer_nested_stmt_expr_perf();

	// stmt-expr init bypasses defer shadow check
	GNUC_ONLY(test_defer_shadow_stmt_expr_bypass());

	// orelse break inside defer body loop
	test_defer_orelse_break_in_loop();

	// orelse block-form in defer bypasses zero-init/raw
	test_defer_orelse_block_zeroinit_bypass();

	// goto in orelse missing p1_goto_exits defer cleanup
	test_orelse_goto_defer_sibling_scope();

	// Phase 1D defer detection false positives on variables/members named "defer"
	test_defer_name_false_positive();

	// defer shadow RHS assignment bypass (check_defer_var_shadow state desync)
	test_defer_shadow_rhs_assignment_bypass();

	// [[__noreturn__]] / [[gnu::__noreturn__]] C23 attr blind spot
	UNIX_ONLY(test_c23_attr_noreturn_dunder_blindspot());

	// __attribute__((cold, __noreturn__)) list truncation + __declspec
	UNIX_ONLY(test_gnu_attr_noreturn_list_blindspot());

	// brace-initializer comma bypass in check_defer_var_shadow
	test_defer_shadow_brace_init_comma_bypass();

	// parenthesized noreturn call / wrapper call bypasses warning and taint
	UNIX_ONLY(
	test_paren_noreturn_warning_bypass();
	test_paren_setjmp_wrapper_taint_bypass();
	);

	// typedef enum wrapping bypasses defer shadow check
	test_defer_shadow_typedef_enum_bypass();

	// case ternary ?: desync in validate_defer_statement
	test_defer_case_ternary_desync();

	// for-init declaration in defer body false positive shadow
	test_defer_shadow_for_init_false_positive();

	// same-block defer shadow hijack (silent miscompilation)
	test_defer_same_block_shadow_hijack();

	// vfork member namespace: os.vfork() / p->vfork() must not be rejected
	test_defer_vfork_member_no_reject();
	test_defer_wrapper_of_exit_not_rejected();

	// braceless for-body if/else and do/while prematurely clears for_name_hid
	test_defer_shadow_braceless_for_ifelse();

	// scope_depth/block_depth type confusion over-unwinds defers
	test_scope_depth_overunwind();

	// emit_range_no_prep missing stmt-expr dispatch
	test_emit_range_no_prep_stmtexpr_defer();

	// braceless ctrl-flow in defer body — orelse/zeroinit not processed
	test_defer_braceless_ctrl_orelse();

	test_defer_braceless_noreturn_unreachable_leak();
	test_defer_braceless_ctrl_brace_wrap();
	test_orelse_block_braceless_ctrl_zeroinit();
	UNIX_ONLY(test_defer_vfork_taint_funcptr_chain());

	// shadowed 'defer' keyword suppression
	test_defer_shadow_variable_block();
	test_defer_shadow_typedef_block();

	// stmt-expr local false positive in defer shadow scanner
	test_defer_shadow_stmt_expr_local_false_positive();

	test_defer_c23_attr_return_type_typedef();
	test_defer_orelse_in_stmtexpr_ctrl_condition();
	test_defer_zeroinit_for_init_ctrl_flow();
	test_defer_scope_init_block_depth_desync();

        // ghost enum in sizeof/cast/typeof must be caught by defer shadow check
        test_ghost_enum_defer_shadow();

	// stmt-expr in control-flow condition heads inside defer must be scanned
	test_defer_stmtexpr_ctrl_head_escape();

	// struct/union/enum body zero-init injection in defer blocks
	test_defer_sue_body_zeroinit();

	// prefix bypass: __extension__/[[...]]/volatile before SUE in defer
	test_defer_sue_prefix_bypass();

	// braceless body shadow scope leak (typedef poisoning)
	test_braceless_shadow_scope_leak();

        // case/default labels in defer body didn't reset at_stmt_start
        test_defer_case_label_orelse_leak();

	// defer_body_refs_name bd>1 missed top-level local declarations
	test_defer_shadow_toplevel_local_false_positive();

	// skip_to_semicolon escaped past } on missing semicolon
	test_defer_validator_escape_missing_semicolon();

	// Phase 1D enum defer shadow hard error on enclosing-scope shadows
	GNUC_ONLY(test_enum_defer_shadow_false_positive());

	// chained orelse in defer body leaked control-flow past first orelse
	test_defer_chained_orelse_control_flow_leak();

	// -fno-orelse must not disable defer-in-parens validation
	test_defer_fno_orelse_paren_leak();

	// enum body inside defer: constants at brace_depth+1 false shadow
	GNUC_ONLY(test_defer_body_enum_constant_shadow());

	test_defer_capture_shadow_stack_restore();
}

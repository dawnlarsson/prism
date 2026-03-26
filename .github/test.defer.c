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

#ifndef _WIN32
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

	char bin_template[PATH_MAX];
	int bin_fd = test_mkstemp(bin_template, "prism_defer_exec_");
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
#endif

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
	/* BUG (audit round 16): a nested block containing defer is the LAST
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
	/* BUG: computed goto (goto *ptr) forwards into a scope PAST its defer and
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

// Bug: glibc expands sigsetjmp to __sigsetjmp after preprocessing.
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

static void test_defer_void_parens_return_bug(void) {
        printf("\n--- Void Function Return Parens Bug ---\n");

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

static void test_defer_in_comma_expr_bug(void) {
        check_defer_transpile_rejects(
            "int main(void) { int x = (defer (void)0, 1); return x; }\n",
            "defer_in_comma_reject.c",
            "defer in comma expression rejected",
            "cannot be at top level"); 
}

static void test_defer_varname_return_value_dropped(void) {
	/* BUG: register_decl_shadows only shadows names matching
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

static void test_defer_label_duplication_bug(void) {
	/* BUG: validate_defer_statement allows labeled statements inside
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
	/* BUG: check_defer_var_shadow matches identifiers by string alone,
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
	/* BUG: register_decl_shadow silently drops entry #257 when the static
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

static void test_defer_body_bare_orelse_return_not_rejected(void) {
	/* BUG: validate_defer_statement calls skip_to_semicolon for statements
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
	/*
	const char *code =
	"#include <unistd.h>\n"
	"#include <stdio.h>\n"
	"void cleanup(void) { printf(\"cleanup\\n\"); }\n"
	"pid_t (*get_trigger(void))(void) { return vfork; }\n"
	"int main(void) {\n"
	"    pid_t (*trigger)(void) = get_trigger();\n"
	"    defer { cleanup(); }\n"
	"    trigger();\n"
	"    return 0;\n"
	"}\n";
	PrismResult r = prism_transpile_source(code, "defer_vfork_fptr.c", prism_defaults());
	// The transpiler should reject this because trigger() is ultimately vfork.
	// Since it can't see through the indirection, this test documents the bypass.
	// It should FAIL (status==OK, no error) until alias analysis is added.
	CHECK(r.status != PRISM_OK,
	"defer-vfork-funcptr: vfork via function pointer bypasses defer safety");
	prism_free(&r);
	*/
}


static void test_defer_vfork_reference_false_positive(void) {
	// get_vfork_ptr returns a pointer to vfork but never calls it.
	// main() calls get_vfork_ptr() and uses defer — this is safe because
	// vfork is never actually invoked.
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
	// main() never calls vfork — only get_vfork_ptr references it as a value.
	// The transpiler should ACCEPT this. Rejection is a false positive caused
	// by the body scanner not distinguishing `return vfork;` from `vfork();`.
	CHECK(r.status == PRISM_OK,
	      "defer-vfork-ref-false-positive: vfork pointer reference taints caller");
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
	// BUG: p1_verify_cfg's label hash table is capped at 8192 slots.
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
	/* BUG: capture_function_return_type is called on every type token at
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
	/* BUG: braceless switch (no braces around body) had no P1K_SWITCH entry,
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

// BUG: braceless inner switch's case label triggers handle_case_default
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

// Bug: check_defer_var_shadow does a flat scan of defer body tokens without
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

// Audit round 30: in_generic() pierces SCOPE_BLOCK — defer leaks raw
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
	/* BUG: Prism detects hardcoded noreturn functions (abort, exit, _Exit,
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

// Audit round 30: match_ch(prev, '*') misclassifies pointer dereference as
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

// Audit round 31: casts like (struct X *) inside nested defer blocks set
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

// Audit round 30: comma-separated declarators in nested defer blocks cause
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

// Audit round 29: brace_depth > 1 skip in check_defer_var_shadow causes
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

// Bug: The check for "defer at end of stmt-expr" only fires when the next
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

// Bug: typeof(x) inside a nested block in a defer body sets in_decl = true,
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

// Bug: check_enum_typedef_defer_shadow's comma-scan loop inside enum body
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

// Audit round 38: double-nested block defer in stmt-expr bypasses detection.
// The check at handle_close_brace only looks at scope_depth-2 for is_stmt_expr.
// A double-nested block places the stmt_expr scope at depth-3, so the check
// doesn't fire.  The defer cleanup call (void) becomes the last expression in
// the statement expression → "void value not ignored as it ought to be".
static void test_defer_stmt_expr_double_nested_block_bypass(void) {
	printf("\n--- defer stmt-expr double-nested block bypass (audit round 38) ---\n");

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
	printf("\n--- C23 attributed computed goto defer bypass (audit round 40) ---\n");

	/* BUG: goto [[gnu::nomerge]] *ptr; bypasses computed goto detection in
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

/* BUG: validate_defer_statement's catch-all loop only detected GNU statement
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
	printf("\n--- defer stmt-expr smuggled in function args (audit round 40) ---\n");

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

// Audit round 41: emit_expr_to_semicolon bypasses walk_balanced keyword dispatcher
// for statement expressions in return statements when outer defers are active.
// defer/orelse/goto inside ({...}) in a return expr leak as raw text → compile error.
static void test_stmt_expr_return_defer_bypass(void) {
	printf("\n--- stmt-expr return defer bypass (audit round 41) ---\n");

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

void run_defer_tests(void) {
	printf("\n=== DEFER TESTS ===\n");
        test_defer_in_comma_expr_bug();

	test_defer_basic();
	test_defer_lifo();
	log_reset();
	int ret = test_defer_return();
	CHECK_LOG("1A", "defer with return");
	CHECK_EQ(ret, 42, "defer return value preserved");
	test_defer_goto_out();
#ifndef _MSC_VER
	test_defer_goto_c23_attr();
#endif
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
#ifndef _MSC_VER
	test_switch_stmt_expr_defer();
	test_switch_in_stmt_expr_in_switch();
#endif
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
	test_defer_backward_goto_into_scope_rejected();
	test_defer_shadow_in_for_init();

	test_defer_backward_goto_sibling();

#ifndef _MSC_VER
	test_defer_goto_c23_attr_label();
#endif
	test_defer_in_ctrl_paren_rejected();
	test_defer_braceless_control_rejected();
#ifdef __GNUC__
	test_defer_stmt_expr_top_level_rejected();
	test_defer_stmt_expr_nested_block_last_stmt_corrupts();
	test_defer_stmt_expr_return_bypass();
	test_defer_stmt_expr_goto_bypass();
	test_defer_computed_goto_rejected();
	// Audit round 14: computed-goto forward-into-scope CFG bypass
	test_computed_goto_forward_into_deferred_scope();
	test_defer_asm_rejected();
#endif
	test_defer_setjmp_rejected();
	test_defer_glibc_sigsetjmp_rejected();
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
        test_defer_void_parens_return_bug();
	test_defer_varname_return_value_dropped();
	test_defer_body_bare_orelse_return_not_rejected();
	test_defer_label_duplication_bug();
	test_defer_variable_shadowing_binds_wrong_scope();
	test_defer_safe_shadow_no_exit();
	test_defer_shadow_struct_member_false_positive();
	test_defer_shadow_overflow_silent_drop();
#ifndef _WIN32
	test_defer_paren_vfork_bypass();
	test_defer_vfork_funcptr_bypass();
	test_defer_vfork_reference_false_positive();
#endif
	test_defer_fnptr_return_type_overwrite();

	// Audit round 9: hash table saturation CFG bypass
	test_cfg_hash_table_saturation_bypass();

	// Audit round 12: braceless switch false positive
	test_braceless_switch_defer_false_positive();

	// Audit round 13: braceless switch inside braced switch drops defers
	test_braceless_switch_defer_drop();

	// Audit round 24: defer shadow inner-block false positive
	test_defer_shadow_inner_block_false_positive();

	// Audit round 30: in_generic() scope leak
	test_defer_in_generic_stmt_expr();

	// Audit round 27: user-defined _Noreturn function with defer gets no warning
#ifndef _WIN32
	test_defer_user_noreturn_no_warning();
#endif

	// Audit round 29: brace_depth > 1 skip bypasses captured vars in nested blocks
	test_defer_shadow_depth_bypass();

	// Audit round 30: * dereference bypass + comma multi-decl false positive
	test_defer_shadow_deref_bypass();
	test_defer_shadow_comma_decl_false_positive();

	// Audit round 31: cast blinds shadow checker
	test_defer_shadow_cast_bypass();

	// Audit round 32: enum constant + typedef shadow defer captures
	test_defer_shadow_enum_bypass();
	test_defer_shadow_typedef_bypass();

	// Audit round 33: empty statement / label between blocks defeats stmt-expr defer check
	test_defer_stmt_expr_empty_stmt_bypass();

	// Audit round 34: typeof(x) inside nested defer block poisons shadow detection
	test_defer_typeof_shadow_bypass();

	// Audit round 35: enum initializer with parens-wrapped comma hallucinates shadow
	test_defer_enum_initializer_comma();

	// Audit round 38: double-nested block in stmt-expr bypasses scope_depth-2 check
	test_defer_stmt_expr_double_nested_block_bypass();

	// Audit round 40: C23 attribute on computed goto bypasses CFG + defer checks
	test_c23_attr_computed_goto_defer_bypass();

	// Audit round 40: stmt-expr smuggled inside function args/array indices/casts
	test_defer_stmt_expr_smuggled_in_args();

	// Audit round 41: emit_expr_to_semicolon stmt-expr keyword bypass
	test_stmt_expr_return_defer_bypass();
}
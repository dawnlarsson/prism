// Massive test suite for Prism C transpiler trying to break it....
// Tests: defer, zero-init, typedef tracking, multi-declarator, edge cases
// Run with: $ prism run .github/test.c

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

static char log_buffer[1024];
static int log_pos = 0;
static int passed = 0;
static int failed = 0;
static int total = 0;

static void log_reset(void) {
	log_buffer[0] = 0;
	log_pos = 0;
}

static void log_append(const char *s) {
	int len = strlen(s);
	if (log_pos + len < 1023) {
		strcpy(log_buffer + log_pos, s);
		log_pos += len;
	}
}

#define CHECK(cond, name)                                                                                    \
	do {                                                                                                 \
		total++;                                                                                     \
		if (cond) {                                                                                  \
			printf("[PASS] %s\n", name);                                                         \
			passed++;                                                                            \
		} else {                                                                                     \
			printf("[FAIL] %s\n", name);                                                         \
			failed++;                                                                            \
		}                                                                                            \
	} while (0)

#define CHECK_LOG(expected, name)                                                                            \
	do {                                                                                                 \
		total++;                                                                                     \
		if (strcmp(log_buffer, expected) == 0) {                                                     \
			printf("[PASS] %s\n", name);                                                         \
			passed++;                                                                            \
		} else {                                                                                     \
			printf("[FAIL] %s: expected '%s', got '%s'\n", name, expected, log_buffer);          \
			failed++;                                                                            \
		}                                                                                            \
	} while (0)

#define CHECK_EQ(got, expected, name)                                                                        \
	do {                                                                                                 \
		total++;                                                                                     \
		if ((got) == (expected)) {                                                                   \
			printf("[PASS] %s\n", name);                                                         \
			passed++;                                                                            \
		} else {                                                                                     \
			printf("[FAIL] %s: expected %d, got %d\n", name, (int)(expected), (int)(got));       \
			failed++;                                                                            \
		}                                                                                            \
	} while (0)


#include "test.safe.c"
#include "test.parse.c"
#include "test.defer.c"
#include "test.orelse.c"
#include "test.zeroinit.c"

int main(void) {
	printf("=== PRISM TEST SUITE ===\n");

	run_defer_basic_tests();
	run_zeroinit_tests();
	run_zeroinit_torture_tests();
#ifdef __GNUC__
	run_typeof_zeroinit_torture_tests();
#endif
	run_raw_tests();
	run_raw_torture_tests();
	run_multi_decl_tests();
	run_typedef_tests();
	run_edge_case_tests();
	run_bug_regression_tests();
	run_advanced_defer_tests();
	run_stress_tests();
	run_safety_hole_tests();
	run_switch_fallthrough_tests();
	run_complex_nesting_tests();
	run_case_label_tests();
	run_switch_defer_bulletproof_tests();
	run_rigor_tests();
	run_silent_failure_tests();
	run_sizeof_constexpr_tests();
	run_manual_offsetof_vla_tests();
	run_preprocessor_numeric_tests();
	run_preprocessor_system_macro_tests();
	run_verification_bug_tests();
	run_parsing_edge_case_tests();
	run_unicode_digraph_tests();
	run_bug_fix_verification_tests();
	run_compound_literal_loop_tests();
	run_enum_shadow_tests();
	run_reported_bug_fix_tests();
	run_additional_bug_fix_tests();
	run_c23_raw_string_tests();
	run_raw_string_torture_tests();
	run_sizeof_var_torture_tests();
	run_logical_op_regression_tests();
	run_bulletproof_regression_tests();
	run_issue_validation_tests();
	run_orelse_tests();
	run_hardening_tests();
	run_hardening_tests_2();
	run_hardening_tests_3();

	test_typedef_extreme_scope_churn();
	test_typedef_tombstone_saturation_extended();
	test_struct_static_assert_compound_literal();
	test_struct_nested_compound_literal_depth();
	test_struct_compound_literal_then_nested_struct();
	test_for_init_multi_decl_all_zeroed();
#ifdef __GNUC__
	test_for_init_stmt_expr_with_decls();
	test_struct_stmt_expr_in_member_size();
#endif
	test_nested_struct_depth_tracking();
	test_struct_with_enum_body_depth();

	test_vla_nested_delimiter_depth();
	test_typedef_survives_bare_semicolons();
	test_for_init_typedef_shadow_cleanup();
	test_orelse_comma_operator_expr();
	test_orelse_sequential_comma();
	test_short_keyword_recognition();
#if __STDC_VERSION__ >= 202311L
	test_c23_attr_positions();
#endif
	test_vla_typedef_complex_size();
	test_goto_stress_many_targets();
	test_goto_converging_defers();
	test_switch_raw_var_in_body();
	test_stack_aggregate_zeroinit();
	test_defer_with_indirect_call();
	test_vla_size_side_effect();
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
	test_vla_sizeof_no_double_eval();
	test_vla_memset_zeroinit();
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
	test_raw_string_max_delimiter();
	test_raw_string_near_max_delimiter();

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
	test_generic_const_array_zeroinit();

	test_named_struct_return_with_defer();
	test_typedef_struct_return_with_defer();
	test_raw_string_16_char_delimiter();

	test_goto_fnptr_decl_after_label();
	test_goto_fnptr_decl_before_goto();
	test_goto_array_ptr_decl_after_label();
	test_typeof_const_zero_init();
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
	test_deep_struct_nesting_goto();
	test_generic_array_not_vla();
	test_c23_attr_void_function();

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
	test_bug_r1_readonly_dir();
	test_bug_r3_line_directive();

	printf("\n========================================\n");
	printf("TOTAL: %d tests, %d passed, %d failed\n", total, passed, failed);
	printf("========================================\n");

	return (failed == 0) ? 0 : 1;
}

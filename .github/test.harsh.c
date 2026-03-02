static void test_harsh_vla_sizeof_side_effect(void) {
    int x = 0;
    int sz = (int)sizeof(int[++x]);
    CHECK(x == 1, "VLA sizeof side effect evaluated exactly once");
}

static void test_harsh_stmt_expr_in_vla(void) {
    int x = 0;
    int sz = (int)sizeof(int[({ x++; 5; })]);
    CHECK(x == 1, "stmt expr in VLA sizeof evaluated exactly once");
}

static void test_harsh_goto_escape_sizeof_defer(void) {
    int x = 0, flags = 0;
    int sz = 0;
    {
        defer x++;
        sz = (int)sizeof(int[({ goto out; { defer flags++; } 5; })]);
    }
out:
    CHECK(x == 1 && flags == 0, "goto escapes sizeof and runs outer defer");
}

#define BRUTAL_DEFER(y) defer y++; defer y++; defer y++; defer y++;

static void test_harsh_macro_defer_eval(void) {
    int count = 0;
    {
        BRUTAL_DEFER(count);
    }
    CHECK(count == 4, "macro expanded defers all fire");
}

void run_harsh_review_tests(void) {
    test_harsh_vla_sizeof_side_effect();
    test_harsh_stmt_expr_in_vla();
    test_harsh_goto_escape_sizeof_defer();
    test_harsh_macro_defer_eval();
}

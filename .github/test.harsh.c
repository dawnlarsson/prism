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

static void test_harsh_generic_decl_noise(void) {
    PrismResult r = prism_transpile_source(
        "#include <wchar.h>\n"
        "#define _GL_EXTERN_C_FUNC\n"
        "#define _GL_FUNCDECL_SYS_NAME(func) (func)\n"
        "#define _GL_FUNCDECL_SYS_1(func, rettype, parameters, ...) \\\n"
        "  _GL_EXTERN_C_FUNC __VA_ARGS__ rettype _GL_FUNCDECL_SYS_NAME(func) parameters\n"
        "#define _GL_FUNCDECL_SYS(func, rettype, parameters, ...) \\\n"
        "  _GL_FUNCDECL_SYS_1(func, rettype, parameters, __VA_ARGS__)\n"
        "#define _GL_ARG_NONNULL(args) __attribute__((nonnull args))\n"
        "#define _GL_ATTRIBUTE_NOTHROW __attribute__((nothrow))\n"
        "#define __glibc_const_generic(PTR,CTYPE,CALL) \\\n"
        "  _Generic(0 ? (PTR) : (void *)1, const void *: (CTYPE)(CALL), default: CALL)\n"
        "#define wmemchr(S,C,N) \\\n"
        "  __glibc_const_generic((S), const wchar_t *, wmemchr(S, C, N))\n"
        "_GL_FUNCDECL_SYS (wmemchr, wchar_t *,\n"
        "                  (const wchar_t *s, wchar_t c, size_t n),\n"
        "                  _GL_ARG_NONNULL ((1)))\n"
        "                  _GL_ATTRIBUTE_NOTHROW;\n",
        "harsh_generic_decl_noise.c", prism_defaults());

    CHECK(r.status == PRISM_OK, "harsh generic decl noise: transpiles OK");
    if (r.output) {
        CHECK(strstr(r.output, "wchar_t *_Generic") == NULL,
              "harsh generic decl noise: no genericized declarator");
    }
    prism_free(&r);
}

static void test_harsh_error_recovery_barrage(void) {
    const char *bad[] = {
        "int main(void) { for (; defer 0;) {} return 0; }\n",
        "int main(void) { if (1) defer (void)0; return 0; }\n",
        "int get(void) { return 0; }\nvoid f(void) { int x = (get() orelse 0); }\n",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        PrismResult r = prism_transpile_source(bad[i], "harsh_bad_case.c", prism_defaults());
        char name[80];
        snprintf(name, sizeof(name), "harsh error barrage: bad case %zu rejected", i + 1);
        CHECK(r.status != PRISM_OK, name);
        prism_free(&r);
    }

    PrismResult ok = prism_transpile_source("int main(void) { int x; return x; }\n",
                                            "harsh_recovery_ok.c", prism_defaults());
    CHECK(ok.status == PRISM_OK, "harsh error barrage: valid case still works after errors");
    prism_free(&ok);
}

void run_harsh_review_tests(void) {
    test_harsh_vla_sizeof_side_effect();
    test_harsh_stmt_expr_in_vla();
    test_harsh_goto_escape_sizeof_defer();
    test_harsh_macro_defer_eval();
    test_harsh_generic_decl_noise();
    test_harsh_error_recovery_barrage();
}

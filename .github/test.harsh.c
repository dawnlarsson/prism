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

static void test_harsh_orelse_typedef_union_value_rejected(void) {
    PrismResult r = prism_transpile_source(
        "typedef union { int x; long y; } U;\n"
        "U make(void) { U u = { .x = 1 }; return u; }\n"
        "int main(void) { U u = make() orelse return 1; return u.x; }\n",
        "harsh_orelse_typedef_union_value.c", prism_defaults());

    CHECK(r.status != PRISM_OK,
          "harsh orelse typedef union value: rejected before emitting invalid scalar test");
    prism_free(&r);
}

static void test_harsh_many_labels_goto_safety(void) {
    size_t cap = 32768;
    char *src = malloc(cap);
    size_t len = 0;

    CHECK(src != NULL, "harsh many labels: allocate source buffer");
    if (!src) return;

    len += snprintf(src + len, cap - len, "int f(void) { goto L129; defer (void)0;\n");
    for (int i = 0; i < 130; i++)
        len += snprintf(src + len, cap - len, "L%d: ;\n", i);
    len += snprintf(src + len, cap - len, "return 0; }\n");

    PrismResult r = prism_transpile_source(src, "harsh_many_labels.c", prism_defaults());
    CHECK(r.status != PRISM_OK,
          "harsh many labels: goto safety still enforced past 128 labels");
    prism_free(&r);
    free(src);
}

static void test_harsh_delimiter_overflow(void) {
    const int depth = 4100;
    size_t cap = (size_t)depth * 2 + 128;
    char *src = malloc(cap);
    size_t len = 0;

    CHECK(src != NULL, "harsh delimiter overflow: allocate source buffer");
    if (!src) return;

    len += snprintf(src + len, cap - len, "int f(void) { int x = ");
    for (int i = 0; i < depth; i++) src[len++] = '(';
    src[len++] = '0';
    for (int i = 0; i < depth; i++) src[len++] = ')';
    len += snprintf(src + len, cap - len, "; return x; }\n");

    PrismResult r = prism_transpile_source(src, "harsh_delim_overflow.c", prism_defaults());
    CHECK(r.status != PRISM_OK,
          "harsh delimiter overflow: excessive nesting rejected cleanly");
    prism_free(&r);
    free(src);
}

static void test_harsh_many_special_wrappers(void) {
    size_t cap = 32768;
    char *src = malloc(cap);
    size_t len = 0;

    CHECK(src != NULL, "harsh wrapper propagation: allocate source buffer");
    if (!src) return;

    len += snprintf(src + len, cap - len, "int setjmp(void *); void *jb;\n");
    for (int i = 0; i < 40; i++)
        len += snprintf(src + len, cap - len, "void w%d(void) { setjmp(jb); }\n", i);
    len += snprintf(src + len,
                    cap - len,
                    "int f(void) { defer (void)0; w39(); return 0; }\n");

    PrismResult r = prism_transpile_source(src, "harsh_many_wrappers.c", prism_defaults());
    CHECK(r.status != PRISM_OK,
          "harsh wrapper propagation: defer still blocked past 32 wrappers");
    prism_free(&r);
    free(src);
}

static void test_harsh_defer_control_scope_leaks(void) {
    const char *bad[] = {
        "int f(void) { { defer { for (;;) { break; } break; }; return 0; } }\n",
        "int f(void) { { defer { for (;;) { break; } continue; }; return 0; } }\n",
        "int f(int x) { { defer { switch (x) { default: break; } break; }; return 0; } }\n",
        "int f(void) { { defer { do { continue; } while (0); continue; }; return 0; } }\n",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        PrismResult r = prism_transpile_source(bad[i], "harsh_defer_scope_leak.c", prism_defaults());
        char name[96];
        snprintf(name,
                 sizeof(name),
                 "harsh defer scope leak: bad case %zu rejected",
                 i + 1);
        CHECK(r.status != PRISM_OK, name);
        prism_free(&r);
    }
}

static void test_harsh_multihop_special_wrapper_propagation(void) {
    PrismResult r = prism_transpile_source(
        "int setjmp(void *);\n"
        "void *jb;\n"
        "void w0(void) { setjmp(jb); }\n"
        "void w1(void) { w0(); }\n"
        "void w2(void) { w1(); }\n"
        "int f(void) { defer (void)0; w2(); return 0; }\n",
        "harsh_multihop_wrapper.c", prism_defaults());

    CHECK(r.status != PRISM_OK,
          "harsh multihop wrapper propagation: defer blocked through wrapper chain");
    prism_free(&r);

    r = prism_transpile_source(
        "int vfork(void);\n"
        "void w0(void) { vfork(); }\n"
        "void w1(void) { w0(); }\n"
        "void w2(void) { w1(); }\n"
        "int f(void) { defer (void)0; w2(); return 0; }\n",
        "harsh_multihop_vfork_wrapper.c", prism_defaults());

    CHECK(r.status != PRISM_OK,
          "harsh multihop wrapper propagation: defer blocked through vfork wrapper chain");
    prism_free(&r);
}

static void test_harsh_mismatched_delimiters_rejected(void) {
    const char *bad[] = {
        "int f(void) { int x = (1]; return x; }\n",
        "int f(void) { defer { int x = (1]; }; return 0; }\n",
        "int f(void) { int x = (1; return x; }\n",
        "int f(void) { ] return 0; }\n",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        PrismResult r = prism_transpile_source(bad[i], "harsh_mismatched_delims.c", prism_defaults());
        char name[96];
        snprintf(name,
                 sizeof(name),
                 "harsh mismatched delimiters: bad case %zu rejected",
                 i + 1);
        CHECK(r.status != PRISM_OK, name);
        prism_free(&r);
    }
}

void run_harsh_review_tests(void) {
    test_harsh_vla_sizeof_side_effect();
    test_harsh_stmt_expr_in_vla();
    test_harsh_goto_escape_sizeof_defer();
    test_harsh_macro_defer_eval();
    test_harsh_generic_decl_noise();
    test_harsh_orelse_typedef_union_value_rejected();
    test_harsh_error_recovery_barrage();
    test_harsh_many_labels_goto_safety();
    test_harsh_delimiter_overflow();
    test_harsh_many_special_wrappers();
    test_harsh_defer_control_scope_leaks();
    test_harsh_multihop_special_wrapper_propagation();
    test_harsh_mismatched_delimiters_rejected();
}

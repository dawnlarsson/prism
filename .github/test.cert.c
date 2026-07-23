// CERT: certification roadmap — flag cubes, cross-feature slices, -fno-safety hooks.
// Run: prism run .github/test.c -- cert   or   PRISM_SUITE_ONLY=cert prism run .github/test.c

static PrismFeatures cert_zd_or(void) {
	PrismFeatures f = prism_defaults();
	f.zeroinit = false;
	/* defer + orelse stay default true */
	return f;
}

// CERT:defer CERT:orelse — zeroinit off; defer + orelse in same function must transpile.
static void cert_flag_cube_defer_orelse_decl(void) {
	PrismFeatures f = cert_zd_or();
	const char *code =
	    "#include <stdio.h>\n"
	    "int main(void) {\n"
	    "    int seed = 0;\n"
	    "    {\n"
	    "        defer printf(\"D\");\n"
	    "        int v = (seed orelse 7);\n"
	    "        (void)v;\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "cert_cube1.c", f);
	CHECK_EQ(r.status, PRISM_OK, "cert flag cube: defer + orelse decl transpiles");
	CHECK(r.output != NULL, "cert flag cube: output");
	if (r.output) {
		CHECK(strstr(r.output, " orelse ") == NULL,
		      "cert flag cube: orelse keyword not left in emission");
	}
	UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
		      r.output, "cert flag cube: compile", "cert flag cube: run"));
	prism_free(&r);
}

// CERT:defer CERT:orelse — two defers; orelse between them (zeroinit off). Exercises ordering + lowering outside defer body.
static void cert_flag_cube_defer_pair_orelse_between(void) {
	PrismFeatures f = cert_zd_or();
	const char *code =
	    "#include <stdio.h>\n"
	    "int main(void) {\n"
	    "    int z = 0;\n"
	    "    defer printf(\"B\");\n"
	    "    int y = (z orelse 3);\n"
	    "    defer printf(\"A\");\n"
	    "    (void)y;\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "cert_cube2.c", f);
	CHECK_EQ(r.status, PRISM_OK, "cert flag cube: dual defer + mid orelse transpiles");
	if (r.output) {
		CHECK(strstr(r.output, " orelse ") == NULL,
		      "cert flag cube: orelse lowered (mid-stmt)");
	}
	UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
		      r.output, "cert dual defer: compile", "cert dual defer: run"));
	prism_free(&r);
}

// CERT:orelse — multi-declarator split with zeroinit off (pipeline still sound).
static void cert_flag_cube_multi_decl_orelse(void) {
	PrismFeatures f = cert_zd_or();
	const char *code =
	    "int main(void) {\n"
	    "    int a = 0 orelse 1, b = 3;\n"
	    "    (void)a;\n"
	    "    (void)b;\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "cert_cube3.c", f);
	CHECK_EQ(r.status, PRISM_OK, "cert flag cube: multi-decl orelse + zeroinit off");
	if (r.output)
		CHECK(strstr(r.output, " orelse ") == NULL,
		      "cert flag cube: multi-decl orelse lowered");
	UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
		      r.output, "cert multi-decl: compile", "cert multi-decl: run"));
	prism_free(&r);
}

// CERT:defer CERT:zeroinit — -fno-safety: switch unbraced decl still OK with defer in scope.
static void cert_nosafety_defer_switch_unbraced(void) {
	PrismFeatures f = prism_defaults();
	f.warn_safety = true;
	const char *code =
	    "void f(int x) {\n"
	    "    defer { (void)0; }\n"
	    "    switch (x) {\n"
	    "        int y;\n"
	    "    case 0:\n"
	    "        y = 1;\n"
	    "        break;\n"
	    "    default:\n"
	    "        break;\n"
	    "    }\n"
	    "}\n"
	    "int main(void) { f(0); return 0; }\n";
	PrismResult r = prism_transpile_source(code, "cert_nosafe1.c", f);
	CHECK_EQ(r.status, PRISM_OK,
		 "cert -fno-safety: switch unbraced decl + defer transpiles");
	UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
		      r.output, "cert nosafe+defer: compile", "cert nosafe+defer: run"));
	prism_free(&r);
}

// CERT:bounds CERT:orelse — declarator `[n orelse k]` + default bounds + zeroinit (memset on VLA path).
static void cert_bounds_bracket_orelse_dim(void) {
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	const char *code =
	    "int main(void) {\n"
	    "    int n = 3;\n"
	    "    int arr[n orelse 4];\n"
	    "    (void)arr;\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "cert_bbo.c", f);
	CHECK_EQ(r.status, PRISM_OK,
		 "cert bounds+bracket orelse dim: transpiles with default bounds");
	if (r.output)
		CHECK(strstr(r.output, " orelse ") == NULL,
		      "cert bounds+bracket orelse dim: declarator orelse lowered");
	UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
		      r.output, "cert bracket dim orelse: compile",
		      "cert bracket dim orelse: run"));
	prism_free(&r);
}

// CERT: Gap — F_ZEROINIT off + orelse decl inside defer body must not emit
// `__typeof__(int t)` (bare-orelse mis-lowering). Prefer process_declarators.
static void cert_zeroinit_off_orelse_in_defer_body(void) {
	PrismFeatures f = cert_zd_or();
	const char *code =
	    "static int get(void) { return 1; }\n"
	    "int main(void) {\n"
	    "    defer {\n"
	    "        int t = get() orelse 0;\n"
	    "        (void)t;\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "cert_zi_off_oe_defer.c", f);
	CHECK_EQ(r.status, PRISM_OK,
		 "cert zi-off+orelse-in-defer: transpiles");
	CHECK(r.output != NULL, "cert zi-off+orelse-in-defer: output");
	if (r.output) {
		CHECK(strstr(r.output, " orelse ") == NULL,
		      "cert zi-off+orelse-in-defer: orelse lowered");
		CHECK(strstr(r.output, "__typeof__(\nint t)") == NULL &&
			  strstr(r.output, "__typeof__(int t)") == NULL,
		      "cert zi-off+orelse-in-defer: no invalid typeof(decl)");
	}
	UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
		      r.output, "cert zi-off+orelse-in-defer: compile",
		      "cert zi-off+orelse-in-defer: run"));
	prism_free(&r);
}

// CERT:zeroinit CERT:autostatic — zeroinit off + auto-static: const literal array still hoisted per SPEC.
static void cert_autostatic_zeroinit_off(void) {
	PrismFeatures f = cert_zd_or();
	f.auto_static = true;
	f.bounds_check = false;
	const char *code =
	    "int main(void) {\n"
	    "    const int k[2] = {1, 2};\n"
	    "    (void)k;\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "cert_as_zi.c", f);
	CHECK_EQ(r.status, PRISM_OK,
		 "cert auto-static + zeroinit off: const array transpiles");
	if (r.output)
		CHECK(strstr(r.output, "static") != NULL,
		      "cert auto-static + zeroinit off: static injected");
	UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
		      r.output, "cert autostatic offzi: compile",
		      "cert autostatic offzi: run"));
	prism_free(&r);
}

/* Feature-flag cube: {zeroinit, defer, orelse, bounds, auto_static} × mini corpus.
 * Transpile always; emit oracles per toggle; compile-run only when Prism keywords
 * the snippet uses are enabled (otherwise keywords leak as invalid C). */
enum {
	CERT_F_DEFER = 1u << 0,
	CERT_F_ORELSE = 1u << 1,
	CERT_F_ZEROINIT = 1u << 2,
	CERT_F_AUTOS = 1u << 3,
	CERT_F_BOUNDS = 1u << 4,
};

static bool cert_out_has_orelse_kw(const char *out) {
	const char *p = out;
	while (p && (p = strstr(p, "orelse")) != NULL) {
		char before = (p == out) ? '\0' : p[-1];
		char after = p[6];
		int border_b = !((before >= 'a' && before <= 'z') ||
				 (before >= 'A' && before <= 'Z') ||
				 (before >= '0' && before <= '9') || before == '_');
		int border_a = !((after >= 'a' && after <= 'z') ||
				 (after >= 'A' && after <= 'Z') ||
				 (after >= '0' && after <= '9') || after == '_');
		if (border_b && border_a) return true;
		p += 6;
	}
	return false;
}

static bool cert_out_has_defer_kw(const char *out) {
	const char *p = out;
	while (p && (p = strstr(p, "defer")) != NULL) {
		char before = (p == out) ? '\0' : p[-1];
		char after = p[5];
		int border_b = !((before >= 'a' && before <= 'z') ||
				 (before >= 'A' && before <= 'Z') ||
				 (before >= '0' && before <= '9') || before == '_');
		int border_a = !((after >= 'a' && after <= 'z') ||
				 (after >= 'A' && after <= 'Z') ||
				 (after >= '0' && after <= '9') || after == '_');
		if (border_b && border_a) return true;
		p += 5;
	}
	return false;
}

/* auto-static injects `static` immediately before the const array decl; the
 * keyword may be separated by whitespace/newline from `const int k`. */
static bool cert_out_has_autostatic_k(const char *out) {
	const char *p = out;
	while (p && (p = strstr(p, "const int k")) != NULL) {
		const char *q = p;
		while (q > out &&
		       (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\n' || q[-1] == '\r'))
			q--;
		if (q - out >= 6 && memcmp(q - 6, "static", 6) == 0) {
			char before = (q - 6 == out) ? '\0' : q[-7];
			int border = !((before >= 'a' && before <= 'z') ||
				       (before >= 'A' && before <= 'Z') ||
				       (before >= '0' && before <= '9') || before == '_');
			if (border) return true;
		}
		p += 11;
	}
	return false;
}

static void cert_feature_flag_cube(void) {
	static const struct {
		const char *tag;
		const char *code;
		unsigned uses; /* Prism keywords / shapes present in source */
	} snips[] = {
	    {"plain", "int main(void) { return 0; }\n", 0},
	    {"orelse",
	     "int main(void) {\n"
	     "    int z = 0;\n"
	     "    int y = z orelse 3;\n"
	     "    (void)y;\n"
	     "    return 0;\n"
	     "}\n",
	     CERT_F_ORELSE},
	    {"defer",
	     "int main(void) {\n"
	     "    { defer (void)0; }\n"
	     "    return 0;\n"
	     "}\n",
	     CERT_F_DEFER},
	    {"zi_arr",
	     "int main(void) {\n"
	     "    int x[3];\n"
	     "    (void)x;\n"
	     "    return 0;\n"
	     "}\n",
	     CERT_F_ZEROINIT},
	    {"autostat",
	     "int main(void) {\n"
	     "    const int k[2] = {1, 2};\n"
	     "    (void)k;\n"
	     "    return 0;\n"
	     "}\n",
	     CERT_F_AUTOS},
	    {"bchk",
	     "int main(void) {\n"
	     "    int n = 3;\n"
	     "    int a[n orelse 4];\n"
	     "    a[0] = 1;\n"
	     "    (void)a;\n"
	     "    return 0;\n"
	     "}\n",
	     /* Bounds oracle only meaningful once orelse lowers the dim. */
	     CERT_F_ORELSE | CERT_F_BOUNDS},
	    {"cross",
	     "int main(void) {\n"
	     "    defer (void)0;\n"
	     "    int z = 0;\n"
	     "    int y = z orelse 1;\n"
	     "    int x[2];\n"
	     "    const int k[2] = {1, 2};\n"
	     "    (void)y; (void)x; (void)k;\n"
	     "    return 0;\n"
	     "}\n",
	     CERT_F_DEFER | CERT_F_ORELSE | CERT_F_ZEROINIT | CERT_F_AUTOS},
	    {"oe_in_defer",
	     "static int get(void) { return 1; }\n"
	     "int main(void) {\n"
	     "    defer {\n"
	     "        int t = get() orelse 0;\n"
	     "        (void)t;\n"
	     "    }\n"
	     "    return 0;\n"
	     "}\n",
	     CERT_F_DEFER | CERT_F_ORELSE},
	    {"multi_oe",
	     "int main(void) {\n"
	     "    int a = 0 orelse 1, b = 3;\n"
	     "    (void)a; (void)b;\n"
	     "    return 0;\n"
	     "}\n",
	     CERT_F_ORELSE},
	    {"vla_zi",
	     "int main(void) {\n"
	     "    int n = 3;\n"
	     "    int a[n];\n"
	     "    (void)a;\n"
	     "    return 0;\n"
	     "}\n",
	     CERT_F_ZEROINIT},
	    {"goto_defer",
	     "int main(void) {\n"
	     "    defer (void)0;\n"
	     "    if (0) goto done;\n"
	     "done:\n"
	     "    return 0;\n"
	     "}\n",
	     CERT_F_DEFER},
	};

	printf("\n--- CERT feature-flag cube ---\n");
	int ok = 0, fail = 0;
	char name[160];
	char fname[64];

	for (int zi = 0; zi < 2; zi++) {
		for (int df = 0; df < 2; df++) {
			for (int oe = 0; oe < 2; oe++) {
				for (int bb = 0; bb < 2; bb++) {
					for (int as = 0; as < 2; as++) {
						PrismFeatures f = prism_defaults();
						f.zeroinit = zi != 0;
						f.defer = df != 0;
						f.orelse = oe != 0;
						f.bounds_check = bb != 0;
						f.auto_static = as != 0;
						/* Keep other defaults; line_directives off shrinks noise. */
						f.line_directives = false;

						for (size_t si = 0;
						     si < sizeof(snips) / sizeof(snips[0]); si++) {
							snprintf(fname, sizeof(fname),
								 "cert_cube_%s_%d%d%d%d%d.c",
								 snips[si].tag, zi, df, oe, bb, as);
							snprintf(name, sizeof(name),
								 "cert cube %s zi=%d df=%d oe=%d "
								 "bb=%d as=%d",
								 snips[si].tag, zi, df, oe, bb, as);

							PrismResult r = prism_transpile_source(
							    snips[si].code, fname, f);
							CHECK_EQ(r.status, PRISM_OK, name);
							if (r.status != PRISM_OK) {
								fail++;
								if (r.error_msg)
									printf("         error: %s\n",
									       r.error_msg);
								prism_free(&r);
								continue;
							}

							int cell_ok = 1;
							unsigned uses = snips[si].uses;

							if (uses & CERT_F_ORELSE) {
								int leaked = cert_out_has_orelse_kw(
								    r.output);
								if (oe) {
									snprintf(name, sizeof(name),
										 "cert cube no-oe-leak "
										 "%s oe=1",
										 snips[si].tag);
									CHECK(!leaked, name);
									if (leaked) cell_ok = 0;
								} else {
									snprintf(name, sizeof(name),
										 "cert cube oe-passthru "
										 "%s oe=0",
										 snips[si].tag);
									CHECK(leaked, name);
									if (!leaked) cell_ok = 0;
								}
							}

							if (uses & CERT_F_DEFER) {
								int leaked =
								    cert_out_has_defer_kw(r.output);
								if (df) {
									snprintf(name, sizeof(name),
										 "cert cube no-df-leak "
										 "%s df=1",
										 snips[si].tag);
									CHECK(!leaked, name);
									if (leaked) cell_ok = 0;
								} else {
									snprintf(name, sizeof(name),
										 "cert cube df-passthru "
										 "%s df=0",
										 snips[si].tag);
									CHECK(leaked, name);
									if (!leaked) cell_ok = 0;
								}
							}

							if (uses & CERT_F_ZEROINIT) {
								/* Fixed arrays → `= {0}`; VLAs → memset. */
								int zeroed =
								    has_zeroing(r.output) ||
								    strstr(r.output, "= {0}") !=
									NULL ||
								    strstr(r.output, "={0}") != NULL;
								snprintf(name, sizeof(name),
									 "cert cube zeroinit %s zi=%d",
									 snips[si].tag, zi);
								if (zi) {
									CHECK(zeroed, name);
									if (!zeroed) cell_ok = 0;
								} else {
									CHECK(!zeroed, name);
									if (zeroed) cell_ok = 0;
								}
							}

							if (uses & CERT_F_AUTOS) {
								int hoisted =
								    cert_out_has_autostatic_k(r.output);
								snprintf(name, sizeof(name),
									 "cert cube autostatic %s as=%d",
									 snips[si].tag, as);
								if (as) {
									CHECK(hoisted, name);
									if (!hoisted) cell_ok = 0;
								} else {
									CHECK(!hoisted, name);
									if (hoisted) cell_ok = 0;
								}
							}

							if ((uses & CERT_F_BOUNDS) && oe) {
								int has_bchk =
								    strstr(r.output,
									   "__prism_bchk(") != NULL;
								snprintf(name, sizeof(name),
									 "cert cube bounds %s bb=%d",
									 snips[si].tag, bb);
								if (bb) {
									CHECK(has_bchk, name);
									if (!has_bchk) cell_ok = 0;
								} else {
									CHECK(!has_bchk, name);
									if (has_bchk) cell_ok = 0;
								}
							}

							/* Compile-run once per snippet at full features. */
							if (zi && df && oe && bb && as) {
								UNIX_ONLY(
								    check_transpiled_output_compiles_and_runs(
									r.output, name, name));
							}

							if (cell_ok) ok++;
							else
								fail++;
							prism_free(&r);
						}
					}
				}
			}
		}
	}

	printf("--- cert cube summary: %d ok, %d fail (32×%zu cells) ---\n", ok, fail,
	       sizeof(snips) / sizeof(snips[0]));

	/* Secondary flags (not full 2^N): warn_safety / auto_unreachable / line_directives
	 * / flatten on a fixed mini corpus + reject corpus. */
	printf("\n--- CERT secondary flags + reject corpus ---\n");
	{
		static const char *sec_code =
		    "int main(void) {\n"
		    "    defer (void)0;\n"
		    "    int z = 0;\n"
		    "    int y = z orelse 1;\n"
		    "    int x[2];\n"
		    "    (void)y; (void)x;\n"
		    "    return 0;\n"
		    "}\n";
		for (int ws = 0; ws < 2; ws++) {
			for (int au = 0; au < 2; au++) {
				for (int ld = 0; ld < 2; ld++) {
					for (int fl = 0; fl < 2; fl++) {
						PrismFeatures f = prism_defaults();
						f.warn_safety = ws != 0;
						f.auto_unreachable = au != 0;
						f.line_directives = ld != 0;
						f.flatten_headers = fl != 0;
						snprintf(name, sizeof(name),
							 "cert sec ws=%d au=%d ld=%d fl=%d", ws, au,
							 ld, fl);
						PrismResult r = prism_transpile_source(
						    sec_code, "cert_sec.c", f);
						CHECK_EQ(r.status, PRISM_OK, name);
						if (r.status == PRISM_OK && r.output) {
							CHECK(!cert_out_has_orelse_kw(r.output),
							      name);
							CHECK(!cert_out_has_defer_kw(r.output),
							      name);
							if (!ld)
								CHECK(strstr(r.output, "#line") ==
									  NULL,
								      name);
						}
						prism_free(&r);
					}
				}
			}
		}

		static const struct {
			const char *code;
			const char *label;
		} rejects[] = {
		    {"int g(void); void f(void){ if (g() orelse 0){} }\n", "orelse in cond"},
		    {"int g(void); int f(int c){ return c ? g() orelse 0 : 1; }\n",
		     "orelse in ternary"},
		    {"void f(int c){ switch(c){ int x; case 0: break; } }\n",
		     "switch unbraced decl"},
		};
		for (size_t i = 0; i < sizeof(rejects) / sizeof(rejects[0]); i++) {
			snprintf(name, sizeof(name), "cert reject: %s", rejects[i].label);
			PrismResult r =
			    prism_transpile_source(rejects[i].code, "cert_rej.c", prism_defaults());
			CHECK(r.status != PRISM_OK, name);
			prism_free(&r);
		}
		/* Same switch under warn_safety must accept. */
		{
			PrismFeatures f = prism_defaults();
			f.warn_safety = true;
			PrismResult r = prism_transpile_source(
			    "void f(int c){ switch(c){ int x; case 0: (void)x; break; } }\n"
			    "int main(void){ f(0); return 0; }\n",
			    "cert_ws.c", f);
			CHECK_EQ(r.status, PRISM_OK, "cert warn_safety: switch unbraced OK");
			UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
			    r.output, "cert warn_safety compile", "cert warn_safety run"));
			prism_free(&r);
		}
	}
}

/* #8 Selective compile-run oracles — shape-critical emits with real cc+exec,
 * not more strstr. Prefer semantic exit codes over snapshot greps. */
static PrismFeatures cro_feat(int kind) {
	/* 0=defaults, 1=no zi, 2=bounds, 3=autostatic, 4=full safety+bounds+as */
	PrismFeatures f = prism_defaults();
	switch (kind) {
	case 1:
		f.zeroinit = false;
		break;
	case 2:
		f.bounds_check = true;
		break;
	case 3:
		f.auto_static = true;
		break;
	case 4:
		f.bounds_check = true;
		f.auto_static = true;
		break;
	default:
		break;
	}
	return f;
}

static void cert_selective_compile_run_oracles(void) {
	printf("\n--- CERT selective compile-run oracles ---\n");
	static const struct {
		const char *tag;
		int feat; /* cro_feat kind */
		const char *src;
	} cases[] = {
	    {"zi-scalar", 0,
	     "int main(void) { int x; return x == 0 ? 0 : 1; }\n"},
	    {"zi-struct", 0,
	     "struct S { int a; int b; };\n"
	     "int main(void) { struct S s; return (s.a | s.b) == 0 ? 0 : 1; }\n"},
	    {"zi-array", 0,
	     "int main(void) { int a[4]; return (a[0]|a[1]|a[2]|a[3]) == 0 ? 0 : 1; }\n"},
	    {"orelse-zero", 0,
	     "int main(void) { int z = 0; int y = z orelse 7; return y == 7 ? 0 : 1; }\n"},
	    {"orelse-nonzero", 0,
	     "int main(void) { int z = 3; int y = z orelse 7; return y == 3 ? 0 : 1; }\n"},
	    {"orelse-ptr", 0,
	     "int main(void) {\n"
	     "  int x = 42; int *p = (void *)0; int *q = p orelse &x;\n"
	     "  return (q && *q == 42) ? 0 : 1;\n"
	     "}\n"},
	    {"defer-scope", 0,
	     "int g;\n"
	     "int main(void) { g = 0; { defer g = 1; } return g == 1 ? 0 : 1; }\n"},
	    {"defer-lifo", 0,
	     "#include <string.h>\n"
	     "char b[16];\n"
	     "static void app(char c) {\n"
	     "  unsigned n = 0; while (b[n]) n++;\n"
	     "  b[n] = c; b[n + 1] = 0;\n"
	     "}\n"
	     "int main(void) {\n"
	     "  b[0] = 0;\n"
	     "  { defer app('A'); defer app('B'); app('1'); }\n"
	     "  return strcmp(b, \"1BA\") == 0 ? 0 : 1;\n"
	     "}\n"},
	    {"defer-nested", 0,
	     "#include <string.h>\n"
	     "char b[16];\n"
	     "static void app(char c) {\n"
	     "  unsigned n = 0; while (b[n]) n++;\n"
	     "  b[n] = c; b[n + 1] = 0;\n"
	     "}\n"
	     "int main(void) {\n"
	     "  b[0] = 0;\n"
	     "  { defer app('A'); { defer app('B'); { defer app('C'); app('1'); } } }\n"
	     "  return strcmp(b, \"1CBA\") == 0 ? 0 : 1;\n"
	     "}\n"},
	    {"defer-return-sidefx", 0,
	     "int g;\n"
	     "static void f(void) { defer g = 9; }\n"
	     "int main(void) { g = 0; f(); return g == 9 ? 0 : 1; }\n"},
	    {"defer-orelse", 0,
	     "int main(void) {\n"
	     "  int z = 0; int y = 0;\n"
	     "  { defer y = 1; int v = z orelse 5; if (v != 5) return 2; }\n"
	     "  return y == 1 ? 0 : 1;\n"
	     "}\n"},
	    {"multi-decl-orelse", 1,
	     "int main(void) {\n"
	     "  int a = 0 orelse 1, b = 3;\n"
	     "  return (a == 1 && b == 3) ? 0 : 1;\n"
	     "}\n"},
	    {"orelse-in-defer-nozi", 1,
	     "int main(void) {\n"
	     "  int out = 0;\n"
	     "  { defer { int z = 0; out = z orelse 4; } }\n"
	     "  return out == 4 ? 0 : 1;\n"
	     "}\n"},
	    {"bounds-ok", 2,
	     "int main(void) {\n"
	     "  int a[4] = {1, 2, 3, 4}; int i = 2;\n"
	     "  return a[i] == 3 ? 0 : 1;\n"
	     "}\n"},
	    {"autostatic-local", 3,
	     "int main(void) {\n"
	     "  int x; x = 0;\n"
	     "  return x == 0 ? 0 : 1;\n"
	     "}\n"},
	    {"stmt-expr-orelse", 0,
	     "int main(void) {\n"
	     "  int z = 0;\n"
	     "  int y = ({ int t = z orelse 8; t; });\n"
	     "  return y == 8 ? 0 : 1;\n"
	     "}\n"},
	    {"full-mix", 4,
	     "int main(void) {\n"
	     "  int a[3]; int z = 0; int y = 0;\n"
	     "  { defer y = 1; int v = z orelse 2; (void)a[1]; if (v != 2) return 3; }\n"
	     "  return (y == 1 && a[0] == 0) ? 0 : 1;\n"
	     "}\n"},
	    {"goto-nested-defer", 0,
	     "#include <string.h>\n"
	     "char b[16];\n"
	     "static void app(char c) {\n"
	     "  unsigned n = 0; while (b[n]) n++;\n"
	     "  b[n] = c; b[n + 1] = 0;\n"
	     "}\n"
	     "int main(void) {\n"
	     "  b[0] = 0;\n"
	     "  { defer app('A'); { defer app('B'); app('1'); goto out; } }\n"
	     "out:\n"
	     "  app('2');\n"
	     "  return strcmp(b, \"1BA2\") == 0 ? 0 : 1;\n"
	     "}\n"},
	    {"break-defer", 0,
	     "#include <string.h>\n"
	     "char b[16];\n"
	     "static void app(char c) {\n"
	     "  unsigned n = 0; while (b[n]) n++;\n"
	     "  b[n] = c; b[n + 1] = 0;\n"
	     "}\n"
	     "int main(void) {\n"
	     "  b[0] = 0;\n"
	     "  for (;;) { defer app('A'); app('1'); break; }\n"
	     "  app('2');\n"
	     "  return strcmp(b, \"1A2\") == 0 ? 0 : 1;\n"
	     "}\n"},
	    {"switch-defer", 0,
	     "#include <string.h>\n"
	     "char b[16];\n"
	     "static void app(char c) {\n"
	     "  unsigned n = 0; while (b[n]) n++;\n"
	     "  b[n] = c; b[n + 1] = 0;\n"
	     "}\n"
	     "int main(void) {\n"
	     "  b[0] = 0;\n"
	     "  switch (1) { default: { defer app('A'); app('1'); break; } }\n"
	     "  app('2');\n"
	     "  return strcmp(b, \"1A2\") == 0 ? 0 : 1;\n"
	     "}\n"},
	};

	int ok = 0, fail = 0;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char name[96];
		snprintf(name, sizeof(name), "cert cro %s", cases[i].tag);
		PrismResult r =
		    prism_transpile_source(cases[i].src, "cert_cro.c", cro_feat(cases[i].feat));
		CHECK_EQ(r.status, PRISM_OK, name);
		if (r.status != PRISM_OK || !r.output) {
			fail++;
			prism_free(&r);
			continue;
		}
#ifndef _WIN32
		check_transpiled_output_compiles_and_runs(r.output, name, name);
		/* check_* already bumps pass/fail; count cell as ok if we got here
		 * after OK transpile (run failures still recorded by CHECK). */
		ok++;
#else
		(void)name;
		ok++;
#endif
		prism_free(&r);
	}
	printf("--- cert cro summary: %d cells transpiled (%zu total) ---\n", ok,
	       sizeof(cases) / sizeof(cases[0]));
	(void)fail;
}

void run_cert_tests(void) {
	printf("\n=== CERT (roadmap) TESTS ===\n");
	cert_flag_cube_defer_orelse_decl();
	cert_flag_cube_defer_pair_orelse_between();
	cert_flag_cube_multi_decl_orelse();
	cert_nosafety_defer_switch_unbraced();
	cert_bounds_bracket_orelse_dim();
	cert_zeroinit_off_orelse_in_defer_body();
	cert_autostatic_zeroinit_off();
	cert_feature_flag_cube();
	cert_selective_compile_run_oracles();
}

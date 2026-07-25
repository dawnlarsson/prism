/*
 * T1′ transform/annotation completeness — procedural generation.
 *
 * Hand-picked cells cannot close transform/annotation products. This suite
 * locks small *axis alphabets* and sweeps their Cartesian products with
 * machine-decidable oracles (same aggregation style as contexts/insertion):
 * many cells → a handful of CHECKs + first-failure breadcrumb.
 *
 * Suites:
 *   completeness       — closed certificate (products that must stay green)
 *   completeness_open  — correct oracles for still-open classes (hunt4+)
 *
 * Enable open: PRISM_COMPLETENESS_OPEN=1 or PRISM_SUITE_ONLY=completeness_open
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ── helpers ─────────────────────────────────────────────────────────── */

static int cm_kw(const char *hay, const char *kw) {
	if (!hay || !kw) return 0;
	size_t n = strlen(kw);
	for (const char *p = hay; (p = strstr(p, kw)) != NULL; p++) {
		char a = p == hay ? ' ' : p[-1], b = p[n];
		int al = (a >= 'a' && a <= 'z') || (a >= 'A' && a <= 'Z') ||
		         (a >= '0' && a <= '9') || a == '_';
		int bl = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
		         (b >= '0' && b <= '9') || b == '_';
		if (!al && !bl) return 1;
	}
	return 0;
}

static PrismResult cm_tx(const char *src) {
	return prism_transpile_source(src, "completeness.c", prism_defaults());
}

static PrismResult cm_txf(const char *src, PrismFeatures f) {
	return prism_transpile_source(src, "completeness.c", f);
}

static int cm_ok(const PrismResult *r) { return r && r->status == PRISM_OK; }
static int cm_err(const PrismResult *r) { return r && r->status != PRISM_OK; }

typedef struct {
	long cells, bad;
	char first[256];
} CmStats;

static void cm_note(CmStats *st, const char *fmt, ...) {
	st->bad++;
	if (st->first[0]) return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(st->first, sizeof(st->first), fmt, ap);
	va_end(ap);
}

static void cm_report(const char *tier, CmStats *st) {
	char name[384];
	snprintf(name, sizeof(name),
		 "completeness[%s]: %ld cells, %ld bad%s%s", tier, st->cells, st->bad,
		 st->bad ? " -- " : "", st->bad ? st->first : "");
	CHECK(st->bad == 0, name);
}

/* Forward decls for tiers defined later in this file. */
static void cm_gen_soft_ident(void);
static void cm_gen_orelse_actions(void);
static void cm_gen_zeroinit_dense(void);
static void cm_gen_cast_subscript(void);
static void cm_gen_name_semantics(void);
static void cm_gen_attr_multidecl(void);
static void cm_gen_raw_suppress(void);
static void cm_gen_linemarker(void);
static void cm_gen_illformed(void);
static void cm_gen_generic_decl(void);
static void cm_gen_bounds_bypass(void);
static void cm_gen_bounds_multidim(void);
static void cm_gen_bounds_filescope(void);
static void cm_gen_cast_correct(void);
static void cm_gen_goto_assign_first(void);
static void cm_gen_computed_goto(void);
static void cm_gen_orelse_reject_dense(void);
static void cm_gen_defer_expr_splice(void);
static void cm_gen_raw_feature_matrix(void);
static void cm_gen_ctrl_paren_defer(void);
static void cm_gen_bounds_cast_oob(void);
static void cm_gen_switch_case_bypass(void);
static void cm_gen_stmt_expr_defer_tail(void);
static void cm_gen_multi_raw_prefix(void);
static void cm_gen_atomic_zi_init(void);
static void cm_gen_defer_braceless_decl(void);
static void cm_gen_orelse_storage(void);
static void cm_gen_nested_defer(void);
static void cm_gen_defer_cf_body(void);
static void cm_gen_goto_vla_cross(void);
static void cm_gen_bounds_ptr_arith(void);
static void cm_gen_attr_ctrlflow(void);
static void cm_gen_case_orelse(void);
static void cm_gen_shadow_after_defer(void);
static void cm_gen_bounds_bypass_dense(void);
static void cm_gen_unreach_sites(void);
static void cm_gen_autostatic_sites(void);
static void cm_gen_zeroinit_sites(void);
static void cm_gen_orelse_expr_ctx(void);
static void cm_gen_taint_defer(void);
static void cm_gen_label_defer_loop(void);
static void cm_gen_orelse_type_junk_dense(void);
static void cm_gen_generic_assoc(void);
static void cm_gen_comma_orelse_split(void);
static void cm_gen_braceless_ctrl_defer(void);
static void cm_gen_cast_orelse_junk(void);
static void cm_gen_nested_typeof_orelse(void);
static void cm_gen_compound_lit_orelse(void);
static void cm_gen_designator_orelse(void);
static void cm_gen_bitint_orelse(void);
static void cm_gen_atomic_orelse(void);
static void cm_gen_pp_span_orelse(void);
static void cm_gen_empty_orelse_action(void);
static void cm_gen_dim_orelse_uneval(void);
static void cm_gen_bounds_param_shadow(void);

/* ═══════════════════════════════════════════════════════════════════════
 * CLOSED generative sweeps
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Axis: defer_form × stmt_wrap.
 * Oracle: accepted ⇒ zero surviving `defer` keywords.
 */
static void cm_gen_stmt_defer(void) {
	static const char *forms[] = {
		"defer { }",
		"defer cleanup();",
		"defer { cleanup(); }",
		"defer { int t = 0; (void)t; cleanup(); }",
		"defer cleanup(); defer { }",
		"defer { cleanup(); } defer cleanup();",
		"defer { cleanup(); cleanup(); }",
		"defer { if (1) cleanup(); }",
		"defer { for (int i=0;i<1;i++) cleanup(); }",
		"defer { while (0) cleanup(); }",
		"defer { switch (0) { default: cleanup(); break; } }",
		"defer { { cleanup(); } }",
	};
	static const char *wraps[] = {
		"void cleanup(void); void f(void){ %s }",
		"void cleanup(void); void f(void){ { %s } }",
		"void cleanup(void); void f(void){ {{ %s }} }",
		"void cleanup(void); void f(void){ if (1) { %s } }",
		"void cleanup(void); void f(void){ if (0) {} else { %s } }",
		"void cleanup(void); void f(void){ if (1) { if (1) { %s } } }",
		"void cleanup(void); void f(void){ for (;;) { %s break; } }",
		"void cleanup(void); void f(void){ for (int i=0;i<1;i++) { %s } }",
		"void cleanup(void); void f(void){ while (0) { %s } }",
		"void cleanup(void); void f(void){ do { %s } while (0); }",
		"void cleanup(void); void f(int x){ switch (x) { default: { %s } break; } }",
		"void cleanup(void); void f(int x){ switch (x) { case 0: { %s } break; default: break; } }",
		"void cleanup(void); void f(void){ L: { %s } }",
		"void cleanup(void); int f(void){ %s return 0; }",
		"void cleanup(void); void f(void){ defer { } %s }",
		"void cleanup(void); void f(void){ %s defer cleanup(); }",
	};
	CmStats st = {0};
	char src[1024];
	for (size_t f = 0; f < sizeof(forms) / sizeof(forms[0]); f++) {
		for (size_t w = 0; w < sizeof(wraps) / sizeof(wraps[0]); w++) {
			snprintf(src, sizeof(src), wraps[w], forms[f]);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_ok(&r) || !r.output || cm_kw(r.output, "defer"))
				cm_note(&st, "stmt-defer form=%zu wrap=%zu", f, w);
			prism_free(&r);
		}
	}
	cm_report("gen/stmt-defer", &st);
}

/*
 * Axis: orelse_payload × site_kind.
 * Sites: decl-top (must lower), paren (must reject), mid-empty (must reject),
 * return-expr (must reject), case-label (reject or lower).
 */
static void cm_gen_orelse_sites(void) {
	static const char *primaries[] = {
		"g()", "rd()", "p", "g() orelse g()", "gp()",
	};
	static const char *fallbacks[] = { "0", "1", "5", "g()" };
	CmStats st = {0};
	char payload[160], src[1024];

	for (size_t p = 0; p < sizeof(primaries) / sizeof(primaries[0]); p++) {
		for (size_t f = 0; f < sizeof(fallbacks) / sizeof(fallbacks[0]); f++) {
			snprintf(payload, sizeof(payload), "%s orelse %s", primaries[p], fallbacks[f]);
			int ptr = !strcmp(primaries[p], "p") || !strcmp(primaries[p], "gp()");
			if (ptr)
				snprintf(src, sizeof(src),
					 "int g(void); int rd(void); int *gp(void);\n"
					 "int main(void){ int *p = gp(); int *x = %s; return x?1:0; }\n",
					 payload);
			else
				snprintf(src, sizeof(src),
					 "int g(void); int rd(void); int *gp(void);\n"
					 "int main(void){ int *p = gp(); int x = %s; return x; }\n",
					 payload);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_ok(&r) || !r.output || cm_kw(r.output, "orelse"))
				cm_note(&st, "decl-orelse p=%zu f=%zu", p, f);
			prism_free(&r);
		}
	}

	static const char *asg_prim[] = { "g()", "rd()", "x" };
	static const char *asg_fb[] = { "0", "1", "g()", "rd()" };
	for (size_t p = 0; p < 3; p++)
		for (size_t f = 0; f < 4; f++) {
			snprintf(src, sizeof(src),
				 "int g(void); int rd(void);\n"
				 "int main(void){ int x = 0; x = %s orelse %s; return x; }\n",
				 asg_prim[p], asg_fb[f]);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_ok(&r) || !r.output || cm_kw(r.output, "orelse"))
				cm_note(&st, "asg-orelse p=%zu f=%zu", p, f);
			prism_free(&r);
		}

	static const char *multis[] = {
		"int *g(void); int main(void){ int *a = g() orelse 0, *b = 0; return (a||b)?1:0; }",
		"int *g(void); int main(void){ int *a = 0, *b = g() orelse 0; return (a||b)?1:0; }",
		"int g(void); int main(void){ int a = g() orelse 0, b = 1; return a+b; }",
		"int g(void); int main(void){ int a = 0, b = g() orelse 1; return a+b; }",
		"int g(void); int main(void){ int a = g() orelse 0, b = g() orelse 1; return a+b; }",
		"_Atomic int rd(void); int main(void){ const _Atomic int x = rd() orelse 5; return (int)x; }",
		"int rd(void); int main(void){ const volatile int x = rd() orelse 5; return x; }",
		"int *g(void); int main(void){ int *_Nonnull p = g() orelse 0; return p?1:0; }",
	};
	for (size_t i = 0; i < sizeof(multis) / sizeof(multis[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(multis[i]);
		if (!cm_ok(&r) || !r.output || cm_kw(r.output, "orelse"))
			cm_note(&st, "multi/qual orelse %zu", i);
		prism_free(&r);
	}

	for (size_t p = 0; p < 5; p++) {
		snprintf(src, sizeof(src),
			 "int g(void); void f(void){ int x; (void)(g() orelse %zu); (void)x; }\n", p);
		st.cells++;
		PrismResult r = cm_tx(src);
		if (!cm_err(&r)) cm_note(&st, "paren-orelse must reject p=%zu", p);
		prism_free(&r);
	}

	static const char *rej[] = {
		"int main(void){ int x=0; x = x orelse orelse 1; return x; }",
		"int main(void){ int b=0; return 0, b = 0 orelse 5; }",
		"void f(int x){ switch(x){ case (x orelse 1): break; } }",
		"void f(void){ int x = (g() orelse 0); } int g(void);",
		"int main(void){ int x = g() orelse orelse 0; return x; } int g(void);",
	};
	for (size_t i = 0; i < sizeof(rej) / sizeof(rej[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(rej[i]);
		if (!(cm_err(&r) || (r.output && !cm_kw(r.output, "orelse"))))
			cm_note(&st, "orelse-reject probe %zu", i);
		prism_free(&r);
	}

	static const char *probes[] = {
		"int main(void){ int x=4; int r; r = sizeof defer; x; return r; }",
		"int main(void){ int x=0; int r; r = ! defer; x; return r; }",
		"int main(void){ int x=0; int r; r = + defer; x; return r; }",
		"int main(void){ int x=0; int r; r = ~ defer; x; return r; }",
	};
	static const char *keep[] = { "sizeof defer", "! defer", "+ defer", "~ defer" };
	for (size_t i = 0; i < 4; i++) {
		st.cells++;
		PrismResult r = cm_tx(probes[i]);
		if (!cm_ok(&r) || !r.output || !strstr(r.output, keep[i]))
			cm_note(&st, "defer-ident splice probe %zu", i);
		prism_free(&r);
	}

	cm_report("gen/orelse-sites", &st);
}

static void cm_gen_autostatic(void) {
	static const char *consts[] = { "const ", "" };
	static const char *stores[] = { "", "static ", "extern " };
	static const char *inits[] = {
		" = {1,2,3}",
		" = {0}",
		"", /* no init */
		" = g()", /* runtime — ineligible */
	};
	static const char *ranks[] = {
		"int a[3]",
		"int a[2][2]",
		"char a[]", /* needs string init — special-cased */
		"int *a",   /* pointer — ineligible */
	};
	static const char *prefs[] = {
		"",
		"[[maybe_unused]] ",
		"__attribute__((cleanup(dtor))) ",
	};

	CmStats st = {0};
	char decl[256], src[1024];
	PrismFeatures feat = prism_defaults();
	feat.bounds_check = false;

	for (size_t c = 0; c < 2; c++)
		for (size_t s = 0; s < 3; s++)
			for (size_t i = 0; i < 4; i++)
				for (size_t r = 0; r < 4; r++)
					for (size_t p = 0; p < 3; p++) {
						/* skip combinatorially invalid */
						if (r == 2 && i != 0 && i != 1)
							continue; /* char a[] needs brace/string */
						if (r == 2) {
							/* use string init form instead */
							if (i > 1) continue;
						}
						if (s == 2 && i == 0)
							continue; /* extern + init is odd */
						if (p == 2 && s != 0)
							continue; /* cleanup+static storage messy */

						int is_const = (c == 0);
						int has_store = (s != 0);
						int literal_init = (i == 0 || i == 1);
						int runtime_init = (i == 3);
						int is_array = (r <= 2);
						int is_ptr = (r == 3);
						int has_prefix = (p != 0);
						int already_static = (s == 1);

						/* SPEC eligibility (local auto-static inject) */
						int eligible = is_const && !has_store &&
								literal_init && is_array &&
								!is_ptr && !has_prefix &&
								!runtime_init;

						if (r == 2)
							snprintf(decl, sizeof(decl), "%s%schar a[] = \"hi\"",
								 prefs[p], consts[c]);
						else if (r == 1 && (i == 0))
							snprintf(decl, sizeof(decl),
								 "%s%s%sint a[2][2] = {{1,2},{3,4}}",
								 prefs[p], stores[s], consts[c]);
						else if (r == 1)
							snprintf(decl, sizeof(decl), "%s%s%sint a[2][2]%s",
								 prefs[p], stores[s], consts[c],
								 inits[i]);
						else
							snprintf(decl, sizeof(decl), "%s%s%s%s%s",
								 prefs[p], stores[s], consts[c],
								 ranks[r], inits[i]);

						if (p == 2)
							snprintf(src, sizeof(src),
								 "void dtor(void*); int g(void);\n"
								 "void f(void){ %s; (void)a; }\n",
								 decl);
						else
							snprintf(src, sizeof(src),
								 "int g(void);\n"
								 "void f(void){ %s; (void)a; }\n",
								 decl);

						st.cells++;
						PrismResult res = cm_txf(src, feat);
						if (!cm_ok(&res)) {
							/* some ineligible combos may hard-error
							 * (extern, cleanup+const zi, …) — OK */
							prism_free(&res);
							continue;
						}
						int has_static =
							res.output && strstr(res.output, "static") != NULL;
						int expect_static = eligible || already_static;
						if (p == 1) {
							/* [[attr]] skip: must not inject before attr */
							if (strstr(res.output, "static  [[") ||
							    strstr(res.output, "static [["))
								cm_note(&st, "as [[attr]] prefixed c=%zu i=%zu r=%zu",
									c, i, r);
						} else if (p == 2) {
							if (strstr(res.output, "static __attribute__((cleanup") ||
							    strstr(res.output, "static  __attribute__((cleanup"))
								cm_note(&st, "as cleanup prefixed c=%zu r=%zu", c, r);
						} else if (expect_static != has_static && eligible) {
							cm_note(&st,
								"as miss inject c=%zu s=%zu i=%zu r=%zu p=%zu",
								c, s, i, r, p);
						} else if (!expect_static && has_static && !already_static &&
							   eligible == 0 && is_const && literal_init &&
							   is_array && !has_prefix && !has_store) {
							/* should have been eligible — axis bug */
							cm_note(&st, "as unexpected static c=%zu r=%zu", c, r);
						} else if (!eligible && !already_static && has_static &&
							   !has_store) {
							cm_note(&st,
								"as false inject c=%zu s=%zu i=%zu r=%zu p=%zu",
								c, s, i, r, p);
						}
						prism_free(&res);
					}

	/* feature-off gate */
	{
		PrismFeatures off = feat;
		off.auto_static = false;
		st.cells++;
		PrismResult r =
			cm_txf("void f(void){ const int k[3]={1,2,3}; (void)k; }", off);
		if (!cm_ok(&r) || (r.output && strstr(r.output, "static")))
			cm_note(&st, "as feature-off still injects");
		prism_free(&r);
	}

	/*
	 * Special-form alphabet — residual SPEC §6.9 edges that don't fit the
	 * rectangular factor grid. Still procedural: one table, one oracle
	 * (inject / no-inject / no-double-static). Absorbs former test.autostatic.c.
	 */
	{
		struct {
			const char *src;
			int want; /* 1 inject, 0 no static, 2 already-static (no double) */
			const char *tag;
		} specials[] = {
			/* positive extras */
			{ "void f(void){ const char * const names[2]={\"a\",\"b\"}; (void)names; }", 1,
			  "ptr-const-arr" },
			{ "void f(void){ const int a[4]={[0]=10,[3]=40}; (void)a; }", 1, "desig" },
			{ "void f(void){ const int s[3]={-1,+2,-3}; (void)s; }", 1, "signs" },
			{ "enum E { E0=7 }; void f(void){ const int a[2]={E0,E0}; (void)a; }", 1,
			  "enum-init" },
			{ "enum E { E0=1 }; void f(void){ const int a[2]={[E0]=3}; (void)a; }", 1,
			  "enum-desig" },
			{ "void f(void){ const char a[4]=\"hi\"; (void)a; }", 1, "char-arr" },
			{ "void f(void){ const int a[]={1,2,3}; (void)a; }", 1, "inferred" },
			{ "void f(void){ const int a[2][2][2]={{{{1}}}}; (void)a; }", 1, "3d" },
			{ "void f(void){ const int * const a[2]={0,0}; (void)a; }", 1,
			  "const-ptr-const" },
			/* negative extras */
			{ "void f(void){ static const int k[2]={1,2}; (void)k; }", 2, "already" },
			{ "int get(void); void f(void){ const int k[2]={get(),2}; (void)k; }", 0,
			  "call-init" },
			{ "void f(int x){ const int k[2]={x,2}; (void)k; }", 0, "var-init" },
			{ "const int k[3]={1,2,3};", 0, "file-scope" },
			{ "void f(void){ int k[3]={1,2,3}; (void)k; }", 0, "non-const" },
			{ "void f(void){ const int a[2]={1,2}, b[2]={3,4}; (void)a;(void)b; }", 0,
			  "multi" },
			{ "volatile int r(void); void f(void){ const int a[2]={1,2}, b=r(); (void)a;(void)b; }",
			  0, "multi-rt" },
			{ "void f(void){ const int k[3]; (void)k; }", 0, "no-init" },
			{ "void f(void){ raw const int k[3]={1,2,3}; (void)k; }", 0, "raw" },
			{ "void f(void){ extern const int k[2]; (void)k; }", 0, "extern" },
			{ "void f(void){ const int k[2]={(int)1,2}; (void)k; }", 0, "cast-init" },
			{ "void f(void){ int n=2; const int k[n]; (void)k; }", 0, "vla" },
			{ "void f(void){ int n=2; const typeof(int[n]) k; (void)k; }", 0,
			  "typeof-vla" },
			{ "struct S{int x;}; void f(void){ const struct S a[1]={{.x=1}}; (void)a; }",
			  1, "struct-desig" },
			{ "void f(void){ const int k = 1; (void)k; }", 0, "plain-scalar" },
			{ "void f(void){ for (const int k[2]={1,2}; 0; ) (void)k; }", 0, "for-init" },
			{ "void f(void){ auto const int k[2]={1,2}; (void)k; }", 0, "auto-sc" },
			{ "void f(void){ constexpr int k[2]={1,2}; (void)k; }", 0, "constexpr" },
			{ "void f(void){ _Thread_local const int k[2]={1,2}; (void)k; }", 0,
			  "thread-local" },
			{ "void f(void){ volatile const int k[2]={1,2}; (void)k; }", 0, "vol-const" },
			{ "void f(void){ const volatile int k[2]={1,2}; (void)k; }", 0, "const-vol" },
			{ "struct S{volatile int x;}; void f(void){ const struct S a[1]={{1}}; (void)a; }",
			  0, "vol-member" },
			{ "typedef volatile int vi; void f(void){ const vi a[2]={1,2}; (void)a; }", 0,
			  "typedef-vol" },
			{ "struct S{volatile int x;}; typedef struct S T; void f(void){ const T a[1]={{1}}; (void)a; }",
			  0, "typedef-vol-member" },
			{ "typedef volatile int vi; typedef vi v2; void f(void){ const v2 a[2]={1,2}; (void)a; }",
			  0, "double-typedef-vol" },
			{ "struct S{struct{volatile int x;} m;}; void f(void){ const struct S a[1]={{{1}}}; (void)a; }",
			  0, "nested-vol-member" },
			{ "void f(void){ const int *a[2]={0,0}; (void)a; }", 0, "const-ptr-arr" },
			{ "void f(void){ const int (*a)[2] = 0; (void)a; }", 0, "ptr-to-arr" },
			{ "typedef int F(void); void f(void){ const F *a[2]={0,0}; (void)a; }", 0,
			  "func-ptr-arr" },
			{ "void f(void){ int i=3; const int k[2]={i--,2}; (void)k; }", 0, "decrement" },
			{ "void f(void){ const int k[2]={({1;}),2}; (void)k; }", 0, "stmtexpr" },
		};
		for (size_t i = 0; i < sizeof(specials) / sizeof(specials[0]); i++) {
			st.cells++;
			PrismResult r = cm_txf(specials[i].src, feat);
			if (!cm_ok(&r) || !r.output) {
				/* VLA/typeof-VLA const may reject under zi — acceptable no-inject */
				if (specials[i].want == 0) {
					prism_free(&r);
					continue;
				}
				cm_note(&st, "as special[%s] status", specials[i].tag);
				prism_free(&r);
				continue;
			}
			int has = strstr(r.output, "static") != NULL;
			if (specials[i].want == 1 && !has)
				cm_note(&st, "as special[%s] miss inject", specials[i].tag);
			else if (specials[i].want == 0 && has &&
				 !strstr(specials[i].src, "static "))
				cm_note(&st, "as special[%s] false inject", specials[i].tag);
			else if (specials[i].want == 2 && strstr(r.output, "static static"))
				cm_note(&st, "as special[%s] double static", specials[i].tag);
			prism_free(&r);
		}
	}

	/* zeroinit-off must not suppress auto-static (feature isolation) */
	{
		PrismFeatures iso = feat;
		iso.zeroinit = false;
		iso.orelse = false;
		st.cells++;
		PrismResult r =
			cm_txf("void f(void){ const int arr[3]={1,2,3}; (void)arr; }", iso);
		if (!cm_ok(&r) || !r.output || !strstr(r.output, "static"))
			cm_note(&st, "as features-isolated miss inject");
		prism_free(&r);
	}
	cm_report("gen/autostatic", &st);
}

/*
 * Goto × obstacle product.
 * Axes: jump_kind × obstacle.
 * Closed subset: cells whose current behavior matches SPEC.
 * (Gap cells live in the open sweep.)
 */
typedef enum {
	CM_JMP_SIBLING_SKIP = 0, /* goto L; { … } L: */
	CM_JMP_INTO_BLOCK,	 /* goto L; { … L: } */
	CM_JMP_BACK_EDGE,	 /* { … L: } goto L; */
	CM_JMP_N
} CmJmp;

typedef enum {
	CM_OBS_NONE = 0,
	CM_OBS_DEFER_EMPTY,
	CM_OBS_DEFER_CALL,
	CM_OBS_PLAIN_DECL,
	CM_OBS_TWO_DECL,
	CM_OBS_FOR_INIT,
	CM_OBS_WHILE_BODY,
	CM_OBS_IF_INIT,
	CM_OBS_SWITCH_INIT,
	CM_OBS_N
} CmObs;

static int cm_goto_closed_expect_err(CmJmp j, CmObs o) {
	/* Locked green behavior today */
	if (j == CM_JMP_INTO_BLOCK && (o == CM_OBS_DEFER_EMPTY || o == CM_OBS_DEFER_CALL)) return 1;
	if (j == CM_JMP_BACK_EDGE && (o == CM_OBS_DEFER_EMPTY || o == CM_OBS_DEFER_CALL)) return 1;
	if (j == CM_JMP_INTO_BLOCK && (o == CM_OBS_PLAIN_DECL || o == CM_OBS_TWO_DECL)) return 1;
	if (j == CM_JMP_INTO_BLOCK && o == CM_OBS_FOR_INIT) return 1;
	if (j == CM_JMP_SIBLING_SKIP && o == CM_OBS_NONE) return 0;
	if (j == CM_JMP_SIBLING_SKIP && (o == CM_OBS_PLAIN_DECL || o == CM_OBS_TWO_DECL || o == CM_OBS_WHILE_BODY))
		return 0;
	if (j == CM_JMP_SIBLING_SKIP && (o == CM_OBS_DEFER_EMPTY || o == CM_OBS_DEFER_CALL))
		return -2; /* allowed: never enters the defer scope (labeled FSM idiom) */
	if (o == CM_OBS_IF_INIT || o == CM_OBS_SWITCH_INIT) {
		/* Into-block over C23 if/switch-init decl must reject. */
		if (j == CM_JMP_INTO_BLOCK) return 1;
		return -2;
	}
	if (j == CM_JMP_INTO_BLOCK && o == CM_OBS_NONE) return 0;
	if (j == CM_JMP_INTO_BLOCK && o == CM_OBS_WHILE_BODY) return 0;
	if (j == CM_JMP_BACK_EDGE && o == CM_OBS_NONE) return 0;
	if (j == CM_JMP_BACK_EDGE && (o == CM_OBS_PLAIN_DECL || o == CM_OBS_TWO_DECL))
		return 1;
	if (j == CM_JMP_BACK_EDGE && o == CM_OBS_WHILE_BODY)
		return 0; /* while body decls are exited before the back-edge label */
	return -2; /* skip / N/A */
}

static void cm_build_goto(char *dst, size_t n, CmJmp j, CmObs o) {
	const char *body_none = "";
	const char *body_defer = "defer { } ";
	const char *body_defer_call = "defer cleanup(); ";
	const char *body_decl = "int x=1; (void)x; ";
	const char *body_two = "int x=1; int y=2; (void)x; (void)y; ";
	const char *body_while = "while(0){ int w=1; (void)w; } ";
	const char *obs = body_none;
	if (o == CM_OBS_DEFER_EMPTY) obs = body_defer;
	if (o == CM_OBS_DEFER_CALL) obs = body_defer_call;
	if (o == CM_OBS_PLAIN_DECL) obs = body_decl;
	if (o == CM_OBS_TWO_DECL) obs = body_two;
	if (o == CM_OBS_WHILE_BODY) obs = body_while;

	if (o == CM_OBS_DEFER_CALL) {
		if (j == CM_JMP_SIBLING_SKIP)
			snprintf(dst, n, "void cleanup(void); void f(void){ goto L; { %s} L: ; }", obs);
		else if (j == CM_JMP_INTO_BLOCK)
			snprintf(dst, n, "void cleanup(void); void f(void){ goto L; { %sL: ; } }", obs);
		else
			snprintf(dst, n, "void cleanup(void); void f(void){ { %sL: ; } goto L; }", obs);
		return;
	}

	if (o == CM_OBS_FOR_INIT) {
		if (j == CM_JMP_INTO_BLOCK)
			snprintf(dst, n,
				 "void f(void){ goto L; for (int x=1; x<2; x++) { L: ; (void)x; } }");
		else if (j == CM_JMP_SIBLING_SKIP)
			snprintf(dst, n,
				 "void f(void){ goto L; for (int x=1; x<2; x++) { (void)x; } L: ; }");
		else
			snprintf(dst, n,
				 "void f(void){ for (int x=1; x<2; x++) { L: ; (void)x; } goto L; }");
		return;
	}
	if (o == CM_OBS_IF_INIT) {
		snprintf(dst, n,
			 "void f(void){ goto L; if (int x=1) { L: ; (void)x; } }");
		return;
	}
	if (o == CM_OBS_SWITCH_INIT) {
		snprintf(dst, n,
			 "void f(void){ goto L; switch (int x=1) { default: L: ; (void)x; break; } }");
		return;
	}

	if (j == CM_JMP_SIBLING_SKIP)
		snprintf(dst, n, "void f(void){ goto L; { %s} L: ; }", obs);
	else if (j == CM_JMP_INTO_BLOCK)
		snprintf(dst, n, "void f(void){ goto L; { %sL: ; } }", obs);
	else
		snprintf(dst, n, "void f(void){ { %sL: ; } goto L; }", obs);
}

static void cm_gen_goto_closed(void) {
	CmStats st = {0};
	char src[512];
	for (int j = 0; j < CM_JMP_N; j++) {
		for (int o = 0; o < CM_OBS_N; o++) {
			int exp = cm_goto_closed_expect_err((CmJmp)j, (CmObs)o);
			if (exp < 0) continue; /* open or N/A */
			cm_build_goto(src, sizeof(src), (CmJmp)j, (CmObs)o);
			st.cells++;
			PrismResult r = cm_tx(src);
			int got_err = cm_err(&r);
			if (exp == 1 && !got_err)
				cm_note(&st, "goto closed miss-reject j=%d o=%d", j, o);
			if (exp == 0 && got_err)
				cm_note(&st, "goto closed false-reject j=%d o=%d", j, o);
			prism_free(&r);
		}
	}
	cm_report("gen/goto-closed", &st);
}

/*
 * Feature polarity: {defer,orelse} ∈ {0,1}² × probe_set.
 * Oracle: enabled feature still checked; disabled keyword is identifier.
 */
static void cm_gen_polarity(void) {
	CmStats st = {0};
	for (int d = 0; d < 2; d++) {
		for (int o = 0; o < 2; o++) {
			PrismFeatures f = prism_defaults();
			f.defer = d;
			f.orelse = o;

			/* identifier gates */
			{
				st.cells++;
				PrismFeatures id = f;
				char src[128];
				snprintf(src, sizeof(src),
					 "int f(void){ int orelse = 3; return orelse; }");
				PrismResult r = cm_txf(src, id);
				if (!o) {
					if (!cm_ok(&r) || !r.output || !cm_kw(r.output, "orelse"))
						cm_note(&st, "polarity orelse-off ident d=%d", d);
				}
				prism_free(&r);
			}
			{
				st.cells++;
				char src[128];
				snprintf(src, sizeof(src),
					 "int f(void){ int defer = 3; return defer; }");
				PrismResult r = cm_txf(src, f);
				if (!d) {
					if (!cm_ok(&r) || !r.output || !cm_kw(r.output, "defer"))
						cm_note(&st, "polarity defer-off ident o=%d", o);
				}
				prism_free(&r);
			}

			/* cross-check: enabled side still rejects misuse */
			if (d) {
				st.cells++;
				PrismResult r = cm_txf("void f(void){ (void)(defer { }); }", f);
				if (!cm_err(&r))
					cm_note(&st, "polarity defer-on miss-reject o=%d", o);
				prism_free(&r);
			}
			if (o) {
				st.cells++;
				PrismResult r = cm_txf(
					"int main(void){ int x=0; x = x orelse orelse 1; return x; }",
					f);
				if (!cm_err(&r))
					cm_note(&st, "polarity orelse-on miss-reject d=%d", d);
				prism_free(&r);
			}
		}
	}
	cm_report("gen/polarity", &st);
}

/* Hunt3 shape locks — unique emission oracles (not just accept/reject). */
static void cm_gen_hunt3_seed(void) {
	CmStats st = {0};

	{
		st.cells++;
		PrismResult r = cm_tx("int main(void){ typeof(_Atomic(int)) x; return (int)x; }");
		if (!cm_ok(&r) || !r.output ||
		    !(strstr(r.output, "memset") || strstr(r.output, "__prism_p_") ||
		      strstr(r.output, "= 0") || strstr(r.output, "= {0}")))
			cm_note(&st, "typeof-atomic missing zi");
		prism_free(&r);
	}
	{
		st.cells++;
		PrismResult r = cm_tx(
			"int *g(void); int main(void){ int *_Nonnull p = g() orelse 0; return p?1:0; }");
		if (!cm_ok(&r) || !r.output || !strstr(r.output, "_Nonnull p") ||
		    !strstr(r.output, "p ="))
			cm_note(&st, "_Nonnull scoped into if/else");
		prism_free(&r);
	}
	{
		st.cells++;
		PrismResult r = cm_tx(
			"_Atomic int rd(void); int main(void){ const _Atomic int x = rd() orelse 5; return (int)x; }");
		if (!cm_ok(&r) || !r.output || !strstr(r.output, "if (!__prism_oe_") ||
		    strstr(r.output, "? __prism_oe_"))
			cm_note(&st, "const-atomic orelse not if-assign");
		prism_free(&r);
	}
	{
		st.cells++;
		PrismResult r = cm_tx(
			"int rd(void); int main(void){ const volatile int x = rd() orelse 5; return x; }");
		if (!cm_ok(&r) || !r.output || !strstr(r.output, "if (!__prism_oe_"))
			cm_note(&st, "const-volatile orelse not if-assign");
		prism_free(&r);
	}

	struct {
		const char *src;
		int want_err;
	} seeds[] = {
		{ "int main(void){ goto L; int x; L: x = x + 1; return x & 255; }", 1 },
		{ "struct S { int a; int b; }; int main(void){ for (_Atomic struct S s; ; ) return s.a; }",
		  1 },
		{ "struct P { struct {} e; int x; }; int main(void){ for (register struct P p; ; ) return p.x!=0; }",
		  1 },
		{ "enum E : _Atomic(int) { orelse = 1 }; int main(void){ return orelse; }", 0 },
		{ "union U { int a; float b; }; struct S { const union U u; int x; };\n"
		  "int main(void){ struct S s; return s.x; }",
		  0 },
		{ "static int defer(void){ return 3; }\n"
		  "int main(void){ int (*t[])(void)={ defer }; return t[0]()==3?0:1; }",
		  0 },
		{ "typedef const int cint; int main(void){ cint arr[8]={1,2,3,4,5,6,7,8}; return arr[0]; }",
		  0 },
		{ "int main(void){ const typeof(int[8]) arr={1,2,3,4,5,6,7,8}; return arr[0]; }", 0 },
		{ "int main(void){ const bool arr[2]={true,false}; return arr[0]; }", 0 },
	};
	for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(seeds[i].src);
		if (seeds[i].want_err ? !cm_err(&r) : !cm_ok(&r))
			cm_note(&st, "hunt3 seed %zu", i);
		if (!seeds[i].want_err && cm_ok(&r) && r.output &&
		    strstr(seeds[i].src, "cint arr") &&
		    !(strstr(r.output, "static") && strstr(r.output, "cint arr")))
			cm_note(&st, "typedef-const miss promote");
		if (!seeds[i].want_err && cm_ok(&r) && r.output &&
		    strstr(seeds[i].src, "typeof(int[8])") &&
		    !(strstr(r.output, "static") && strstr(r.output, "typeof")))
			cm_note(&st, "typeof-arr miss promote");
		if (!seeds[i].want_err && cm_ok(&r) && r.output &&
		    strstr(seeds[i].src, "true,false") &&
		    !(strstr(r.output, "static") && strstr(r.output, "true")))
			cm_note(&st, "bool-literal miss promote");
		prism_free(&r);
	}
	{
		PrismFeatures f = prism_defaults();
		f.flatten_headers = false;
		st.cells++;
		PrismResult r = cm_txf("%: 1 \"hunt3_dg.i\"\n"
				      "%: 1 \"/usr/include/stdio.h\" 1 3\n"
				      "extern int printf(const char *, ...);\n"
				      "%: 2 \"hunt3_dg.i\" 2\n"
				      "int main(void){ int x; return x; }\n",
				      f);
		if (!cm_ok(&r) || !r.output || !strstr(r.output, "main") ||
		    strstr(r.output, "printf"))
			cm_note(&st, "hunt3 digraph flatten");
		prism_free(&r);
	}
	cm_report("gen/hunt3-seed", &st);
}

/* ── helpers for unreachable / bounds ───────────────────────────────── */
static int cm_has_unreach(const char *o) {
	return o && (strstr(o, "__builtin_unreachable") || strstr(o, "__assume(0)"));
}
/* True wrap — not the always-emitted static helper when bounds is on. */
static int cm_has_bchk_wrap(const char *o) {
	return o && strstr(o, "[__prism_bchk") != NULL;
}

/*
 * Auto-unreachable product: attr_spelling × call_site × polarity.
 * Closed oracle matches SPEC conservatism: inject after stmt-level
 * `ident(...);` to a known-noreturn callee; never in sizeof/ternary/
 * for-init/paren-name/member/shadow.
 * Absorbs the bulk of former test.autounreach.c (~60 hand fns → product).
 */
static void cm_gen_autounreach(void) {
	static const char *attrs[] = {
		"_Noreturn void die(void)",
		"[[noreturn]] void die(void)",
		"__attribute__((noreturn)) void die(void)",
		"__attribute__((__noreturn__)) void die(void)",
		"_Noreturn __attribute__((noreturn)) void die(void)",
		"static _Noreturn inline void die(void)",
		"_Noreturn void die(int)",
		"[[noreturn]] [[deprecated]] void die(void)",
	};
	/* Positive sites: %s = attr decl line; call is die(...); as statement */
	static const char *pos_sites[] = {
		"%s; void f(void){ die(0); int __m=1; (void)__m; }",
		"%s; void f(void){ { die(0); } int __m=1; (void)__m; }",
		"%s; void f(void){ {{ die(0); }} int __m=1; (void)__m; }",
		"%s; void f(void){ L: die(0); int __m=1; (void)__m; }",
		"%s; void f(void){ M: { die(0); } int __m=1; (void)__m; }",
		"%s; void f(int x){ switch(x){ case 0: die(0); break; default: break; } }",
		"%s; void f(int x){ switch(x){ default: die(0); break; } }",
		"%s; void f(void){ do { die(0); } while(0); }",
		"%s; void f(void){ for(;;){ die(0); } }",
		"%s; void f(void){ for(int i=0;i<1;i++){ die(0); } }",
		"%s; void f(void){ while(1){ die(0); } }",
		"%s; void f(void){ defer { die(0); } }",
		"%s; void f(void){ if(1) { die(0); } int __m=1; (void)__m; }",
		"%s; void f(void){ if(0) {} else { die(0); } int __m=1; (void)__m; }",
		"%s; void f(void){ if(1) { if(1) { die(0); } } }",
	};
	/* Negative sites: must NOT inject */
	static const char *neg_sites[] = {
		"%s; void f(void){ (die)(0); }",
		"%s; void cleanup(void); void f(void){ (cleanup(), die(0)); }",
		"%s; int f(int x){ x ? die(0) : (void)0; return x; }",
		"%s; void f(void){ for(die(0);;){} }",
		"%s; void f(void){ if(die(0)){} }",
		"%s; void f(void){ while(die(0)){} }",
		"%s; void f(void){ (void)sizeof(die(0)); }",
		"%s; void f(void){ int die = 0; (void)die; }",
		"%s; struct S { int die; }; void f(void){ struct S s={0}; (void)s.die; }",
		"%s; struct S { int die; }; void f(void){ struct S *p=0; if(p) (void)p->die; }",
		/* braceless then/else: conservative — no inject (stmt is not bare ident(;)) */
		"%s; void f(void){ if(1) die(0); int __m=1; (void)__m; }",
		"%s; void f(void){ if(0); else die(0); int __m=1; (void)__m; }",
	};
	/* Library-name × stmt call (declared locally, not #include) */
	static const char *lib_names[] = {
		"exit", "abort", "quick_exit", "_Exit",
	};

	CmStats st = {0};
	char src[1024];

	for (size_t a = 0; a < sizeof(attrs) / sizeof(attrs[0]); a++) {
		for (size_t s = 0; s < sizeof(pos_sites) / sizeof(pos_sites[0]); s++) {
			/* die(int) vs die(void) — pick matching call */
			const char *site = pos_sites[s];
			char sitebuf[512];
			if (strstr(attrs[a], "die(int)"))
				snprintf(sitebuf, sizeof(sitebuf), "%s", site);
			else {
				/* rewrite die(0) → die() for void callees */
				const char *p = site;
				char *o = sitebuf;
				char *end = sitebuf + sizeof(sitebuf) - 1;
				while (*p && o < end) {
					if (!strncmp(p, "die(0)", 6)) {
						memcpy(o, "die()", 5);
						o += 5;
						p += 6;
					} else
						*o++ = *p++;
				}
				*o = 0;
				site = sitebuf;
			}
			snprintf(src, sizeof(src), site, attrs[a]);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_ok(&r) || !r.output || !cm_has_unreach(r.output))
				cm_note(&st, "aur pos attr=%zu site=%zu", a, s);
			prism_free(&r);
		}
		for (size_t s = 0; s < sizeof(neg_sites) / sizeof(neg_sites[0]); s++) {
			const char *site = neg_sites[s];
			char sitebuf[512];
			if (!strstr(attrs[a], "die(int)")) {
				const char *p = site;
				char *o = sitebuf;
				char *end = sitebuf + sizeof(sitebuf) - 1;
				while (*p && o < end) {
					if (!strncmp(p, "die(0)", 6)) {
						memcpy(o, "die()", 5);
						o += 5;
						p += 6;
					} else
						*o++ = *p++;
				}
				*o = 0;
				site = sitebuf;
			}
			snprintf(src, sizeof(src), site, attrs[a]);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_ok(&r)) {
				/* some neg forms may reject — OK if no false unreach needed */
				prism_free(&r);
				continue;
			}
			if (r.output && cm_has_unreach(r.output))
				cm_note(&st, "aur neg false+ attr=%zu site=%zu", a, s);
			prism_free(&r);
		}
	}

	for (size_t n = 0; n < sizeof(lib_names) / sizeof(lib_names[0]); n++) {
		snprintf(src, sizeof(src),
			 "void %s(int); void f(void){ %s(1); int __m=1; (void)__m; }",
			 lib_names[n], lib_names[n]);
		st.cells++;
		PrismResult r = cm_tx(src);
		if (!cm_ok(&r) || !r.output || !cm_has_unreach(r.output))
			cm_note(&st, "aur lib-name %s", lib_names[n]);
		prism_free(&r);
	}

	/* feature-off */
	{
		PrismFeatures off = prism_defaults();
		off.auto_unreachable = false;
		st.cells++;
		PrismResult r = cm_txf("_Noreturn void die(void); void f(void){ die(); }", off);
		if (!cm_ok(&r) || (r.output && cm_has_unreach(r.output)))
			cm_note(&st, "aur feature-off still injects");
		prism_free(&r);
	}

	/* multi-decl prefix attr: both names (open gap also tracks; closed
	 * asserts at least the form transpiles — full both-unreach is open) */
	{
		st.cells++;
		PrismResult r = cm_tx(
			"_Noreturn void a(void), b(void); void a(void){} void b(void){}\n"
			"void f(void){ a(); b(); }\n");
		if (!cm_ok(&r)) cm_note(&st, "aur multidecl status");
		prism_free(&r);
	}

	/* Residuals that are not a rectangular product (former autounreach pins). */
	{
		st.cells++;
		PrismResult r = cm_tx(
			"void f(void){\n"
			"  defer (void)0;\n"
			"  if (0) asm __volatile__ goto(\"\" : : : : L);\n"
			"L: return;\n"
			"}\n");
		if (!cm_err(&r)) cm_note(&st, "aur asm-volatile-goto+defer must reject");
		prism_free(&r);
	}
	{
		st.cells++;
		PrismResult r = cm_tx(
			"void f(void (*_Nonnull _Noreturn p)(void)) { (void)p; }\n");
		if (!cm_ok(&r) || (r.output && cm_has_unreach(r.output)))
			cm_note(&st, "aur param-noreturn must not mark function");
		prism_free(&r);
	}

	cm_report("gen/autounreach", &st);
}

/*
 * Bounds wrap product: array_kind × index × context.
 * Eval contexts must wrap with __prism_bchk; uneval must not.
 */
static void cm_gen_bounds_wrap(void) {
	static const char *arrays[] = {
		"int a[8]",
		"char a[16]",
		"short a[10]",
		"long a[6]",
		"int a[4][4]",
		"int a[2][3][4]",
		"raw int a[8]",
		"int defer[8]",
		"int orelse[8]",
		"unsigned a[8]",
		"double a[4]",
	};
	static const char *idxs[] = { "0", "1", "2", "3", "7", "i", "i+1", "i*2" };
	static const struct {
		const char *tmpl;
		int want_bchk;
		const char *tag;
	} ctxs[] = {
		{ "void f(void){ %s; int i=1; (void)(%s); }", 1, "eval-stmt" },
		{ "void f(void){ %s; int i=1; int x = %s; (void)x; }", 1, "eval-init" },
		{ "int g(int); void f(void){ %s; int i=1; g(%s); }", 1, "eval-arg" },
		{ "int f(void){ %s; int i=1; return %s; }", 1, "eval-return" },
		{ "void f(void){ %s; int i=1; int x = 0; x = %s; (void)x; }", 1, "eval-assign" },
		{ "void f(void){ %s; int i=1; int x = (%s, 0); (void)x; }", 1, "eval-comma" },
		{ "void f(void){ %s; int i=1; if (%s) {} }", 1, "eval-cond" },
		{ "void f(void){ %s; int i=1; while (%s) break; }", 1, "eval-while" },
		{ "void f(void){ %s; int i=1; (void)sizeof(%s); }", 0, "uneval-sizeof" },
		{ "void f(void){ %s; int i=1; typeof(%s) *p = 0; (void)p; }", 0, "uneval-typeof" },
		{ "void f(void){ %s; int i=1; (void)_Alignof(typeof(%s)); }", 0, "uneval-alignof" },
	};

	CmStats st = {0};
	char decl[128], sub[64], src[768];
	PrismFeatures feat = prism_defaults();

	for (size_t a = 0; a < sizeof(arrays) / sizeof(arrays[0]); a++) {
		for (size_t i = 0; i < sizeof(idxs) / sizeof(idxs[0]); i++) {
			for (size_t c = 0; c < sizeof(ctxs) / sizeof(ctxs[0]); c++) {
				const char *var = "a";
				if (strstr(arrays[a], "defer[")) var = "defer";
				else if (strstr(arrays[a], "orelse[")) var = "orelse";

				if (strstr(arrays[a], "raw int"))
					snprintf(decl, sizeof(decl), "raw int a[8]");
				else if (strstr(arrays[a], "defer["))
					snprintf(decl, sizeof(decl), "int defer[8]");
				else if (strstr(arrays[a], "orelse["))
					snprintf(decl, sizeof(decl), "int orelse[8]");
				else
					snprintf(decl, sizeof(decl), "%s", arrays[a]);

				/* Collapse multi-dim to a scalar element access for typed eval ctxs. */
				if (strstr(arrays[a], "[2][3][4]"))
					snprintf(sub, sizeof(sub), "%s[%s][0][0]", var, idxs[i]);
				else if (strstr(arrays[a], "]["))
					snprintf(sub, sizeof(sub), "%s[%s][0]", var, idxs[i]);
				else
					snprintf(sub, sizeof(sub), "%s[%s]", var, idxs[i]);

				snprintf(src, sizeof(src), ctxs[c].tmpl, decl, sub);
				st.cells++;
				PrismResult r = cm_txf(src, feat);
				if (!cm_ok(&r) || !r.output) {
					/* soft-kw array names as decl can be fine; skip hard fail */
					if (cm_err(&r) && (strstr(decl, "defer") || strstr(decl, "orelse"))) {
						prism_free(&r);
						continue;
					}
					cm_note(&st, "bounds status %s a=%zu i=%zu", ctxs[c].tag, a, i);
					prism_free(&r);
					continue;
				}
				int is_raw = strstr(decl, "raw ") != NULL;
				int want = ctxs[c].want_bchk && !is_raw;
				int has = cm_has_bchk_wrap(r.output);
				if (want && !has)
					cm_note(&st, "bounds miss wrap %s a=%zu", ctxs[c].tag, a);
				if (!want && has && !is_raw)
					cm_note(&st, "bounds false wrap %s a=%zu", ctxs[c].tag, a);
				if (is_raw && has)
					cm_note(&st, "bounds raw still wraps a=%zu ctx=%s", a,
						ctxs[c].tag);
				prism_free(&r);
			}
		}
	}

	/* feature-off */
	{
		PrismFeatures off = feat;
		off.bounds_check = false;
		st.cells++;
		PrismResult r = cm_txf("void f(void){ int a[8]; (void)a[3]; }", off);
		if (!cm_ok(&r) || (r.output && cm_has_bchk_wrap(r.output)))
			cm_note(&st, "bounds feature-off still wraps");
		prism_free(&r);
	}
	cm_report("gen/bounds-wrap", &st);
}

/*
 * Zero-init shape product: type × storage-ish × init_state.
 * Oracle: uninit local object types get memset/{0}; ineligible reject or skip.
 */
static void cm_gen_zeroinit(void) {
	static const char *types[] = {
		"int",
		"char",
		"double",
		"int *",
		"struct { int x; }",
		"union { int x; float y; }",
		"int[4]",
		"_Atomic(int)",
		"typeof(int)",
	};
	static const char *inits[] = {
		"",	     /* uninit — want zi */
		" = {0}",    /* user init — typically no extra zi */
		" = 0",
	};

	CmStats st = {0};
	char src[768], decl[256];
	PrismFeatures feat = prism_defaults();
	feat.bounds_check = false;

	for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
		for (size_t i = 0; i < sizeof(inits) / sizeof(inits[0]); i++) {
			int is_arr = strstr(types[t], "[") != NULL;
			int is_agg = strstr(types[t], "struct") || strstr(types[t], "union") ||
				     is_arr;
			if (is_arr)
				snprintf(decl, sizeof(decl), "int x[4]%s", inits[i]);
			else if (strstr(types[t], "struct"))
				snprintf(decl, sizeof(decl), "struct { int x; } s%s",
					 inits[i][0] ? inits[i] : "");
			else if (strstr(types[t], "union"))
				snprintf(decl, sizeof(decl), "union { int x; float y; } u%s",
					 inits[i][0] ? inits[i] : "");
			else
				snprintf(decl, sizeof(decl), "%s x%s", types[t], inits[i]);

			snprintf(src, sizeof(src), "void f(void){ %s; (void)sizeof(char); }\n",
				 decl);
			st.cells++;
			PrismResult r = cm_txf(src, feat);
			if (!cm_ok(&r)) {
				/* some _Atomic/union uninit may reject — record only unexpected */
				prism_free(&r);
				continue;
			}
			int uninit = (inits[i][0] == '\0');
			int has_zi = r.output && (strstr(r.output, "memset") ||
						  strstr(r.output, "__prism_p_") ||
						  strstr(r.output, "= {0}") ||
						  strstr(r.output, "= 0"));
			if (uninit && !has_zi && !strstr(types[t], "_Atomic"))
				cm_note(&st, "zi miss t=%zu", t);
			(void)is_agg;
			prism_free(&r);
		}
	}

	/* feature-off */
	{
		PrismFeatures off = feat;
		off.zeroinit = false;
		st.cells++;
		PrismResult r = cm_txf("void f(void){ int x; (void)x; }", off);
		if (!cm_ok(&r)) cm_note(&st, "zi-off status");
		else if (r.output && (strstr(r.output, "memset") || strstr(r.output, "__prism_p_")))
			cm_note(&st, "zi-off still memsets");
		prism_free(&r);
	}
	cm_report("gen/zeroinit", &st);
}

/*
 * Defer × exit-mode × nest-depth — compact CFG product complementary to
 * machine suite (which exhausts abstract stacks). Here: concrete source
 * shapes must lower with no keyword leak.
 */
static void cm_gen_defer_cfg(void) {
	static const char *exits[] = {
		"return;",
		"break;",
		"continue;",
		"goto L;",
		"", /* fallthrough */
	};
	static const char *nests[] = {
		"void cleanup(void); void f(void){ %s %s }",
		"void cleanup(void); void f(void){ { %s %s } }",
		"void cleanup(void); void f(void){ {{ %s %s }} }",
		"void cleanup(void); void f(void){ for(int i=0;i<1;i++){ %s %s } }",
		"void cleanup(void); void f(void){ while(0){ %s %s } }",
		"void cleanup(void); void f(void){ switch(0){ default: { %s %s } break; } }",
		"void cleanup(void); void f(void){ do { %s %s } while(0); }",
		"void cleanup(void); void f(void){ if(1){ %s %s } }",
	};
	static const char *defers[] = {
		"defer cleanup();",
		"defer { cleanup(); }",
		"defer { } defer cleanup();",
		"defer { cleanup(); cleanup(); }",
		"defer cleanup(); defer { }",
	};

	CmStats st = {0};
	char src[1024];

	for (size_t d = 0; d < sizeof(defers) / sizeof(defers[0]); d++) {
		for (size_t n = 0; n < sizeof(nests) / sizeof(nests[0]); n++) {
			for (size_t e = 0; e < sizeof(exits) / sizeof(exits[0]); e++) {
				/* break/continue only valid in loop/switch nests */
				if ((!strcmp(exits[e], "break;") || !strcmp(exits[e], "continue;")) &&
				    !(n == 3 || n == 4 || n == 5 || n == 6))
					continue;
				if (!strcmp(exits[e], "continue;") && n == 5)
					continue; /* switch */
				if (!strcmp(exits[e], "goto L;"))
					snprintf(src, sizeof(src), nests[n], defers[d], "goto L; L:;");
				else
					snprintf(src, sizeof(src), nests[n], defers[d], exits[e]);
				st.cells++;
				PrismResult r = cm_tx(src);
				if (!cm_ok(&r) || !r.output || cm_kw(r.output, "defer"))
					cm_note(&st, "defer-cfg d=%zu n=%zu e=%zu", d, n, e);
				prism_free(&r);
			}
		}
	}
	cm_report("gen/defer-cfg", &st);
}

/*
 * Feature-flag cube (subset of cert, denser transforms): toggle each of
 * {defer, orelse, bounds, auto_static, auto_unreachable, zeroinit} on a
 * fixed snippet pack — leak/inject invariants.
 */
static void cm_gen_feature_cube(void) {
	static const char *snips[] = {
		"void cleanup(void); void f(void){ defer cleanup(); }",
		"void cleanup(void); void f(void){ defer { cleanup(); } }",
		"int *g(void); void f(void){ int *p = g() orelse 0; (void)p; }",
		"int g(void); void f(void){ int x = g() orelse 1; (void)x; }",
		"void f(void){ int a[4]; (void)a[1]; }",
		"void f(void){ int a[2][2]; (void)a[0][1]; }",
		"void f(void){ const int k[3]={1,2,3}; (void)k; }",
		"void f(void){ const char m[]=\"hi\"; (void)m; }",
		"_Noreturn void die(void); void f(void){ die(); }",
		"[[noreturn]] void die(void); void f(void){ die(); }",
		"void f(void){ int x; (void)x; }",
		"void f(void){ struct { int a; } s; (void)s; }",
		"void f(void){ raw { int x; (void)x; } }",
		"void f(void){ int *p=0; (void)(p); }",
	};
	CmStats st = {0};
	/* 2^6 = 64 feature masks × 6 snips = 384 cells */
	for (unsigned mask = 0; mask < 64; mask++) {
		PrismFeatures f = prism_defaults();
		f.defer = mask & 1;
		f.orelse = (mask >> 1) & 1;
		f.bounds_check = (mask >> 2) & 1;
		f.auto_static = (mask >> 3) & 1;
		f.auto_unreachable = (mask >> 4) & 1;
		f.zeroinit = (mask >> 5) & 1;
		for (size_t s = 0; s < sizeof(snips) / sizeof(snips[0]); s++) {
			st.cells++;
			PrismResult r = cm_txf(snips[s], f);
			if (!r.output && r.status == PRISM_OK) {
				cm_note(&st, "cube null out mask=%u s=%zu", mask, s);
				prism_free(&r);
				continue;
			}
			/* leak invariants when feature ON and accept */
			if (cm_ok(&r) && r.output) {
				if ((mask & 1) && strstr(snips[s], "defer") &&
				    cm_kw(r.output, "defer"))
					cm_note(&st, "cube defer leak mask=%u", mask);
				if (((mask >> 1) & 1) && strstr(snips[s], "orelse") &&
				    cm_kw(r.output, "orelse"))
					cm_note(&st, "cube orelse leak mask=%u", mask);
				if (!((mask >> 2) & 1) && strstr(snips[s], "a[1]") &&
				    cm_has_bchk_wrap(r.output))
					cm_note(&st, "cube bounds off leak mask=%u", mask);
				if (!((mask >> 3) & 1) && strstr(snips[s], "const int k") &&
				    strstr(r.output, "static const int"))
					cm_note(&st, "cube as off leak mask=%u", mask);
				if (!((mask >> 4) & 1) && strstr(snips[s], "die") &&
				    cm_has_unreach(r.output))
					cm_note(&st, "cube aur off leak mask=%u", mask);
			}
			prism_free(&r);
		}
	}
	cm_report("gen/feature-cube", &st);
}

/*
 * Historical golf pins — unique emission shapes (absorbs test.golf.c).
 */
static void cm_gen_golf_pins(void) {
	CmStats st = {0};
	{
		st.cells++;
		PrismResult r = cm_tx(
			"void f(int n, int arr[n][n]){ int local[sizeof(arr)]; (void)local; }\n");
		if (!cm_ok(&r) || !r.output || strstr(r.output, "memset"))
			cm_note(&st, "golf: param multidim sizeof must not memset");
		prism_free(&r);
	}
	{
		st.cells++;
		PrismResult r = cm_tx(
			"static int g[5];\n"
			"int (*f(void))[5] __attribute__((unused)) {\n"
			"  defer { }\n"
			"  return &g;\n"
			"}\n");
		if (!cm_ok(&r) || !r.output) {
			cm_note(&st, "golf: complex return+attr status");
		} else {
			const char *td = strstr(r.output, "typedef");
			if (!td)
				cm_note(&st, "golf: missing typedef");
			else {
				const char *semi = strchr(td, ';');
				if (!semi)
					cm_note(&st, "golf: typedef no semi");
				else {
					size_t len = (size_t)(semi - td);
					char *buf = malloc(len + 1);
					memcpy(buf, td, len);
					buf[len] = 0;
					if (strstr(buf, "__attribute__"))
						cm_note(&st, "golf: attr leaked into typedef");
					free(buf);
				}
			}
		}
		prism_free(&r);
	}
	cm_report("gen/golf-pins", &st);
}

/*
 * Reject alphabet — only forms that already reject on main (closed).
 * Open gaps (while-init, for-raw, …) stay in completeness_open.
 * Axes: stray-control × site, misuse paren, goto×obstacle known-reject.
 */
static void cm_gen_reject_alphabet(void) {
	static const char *file_scope[] = {
		"return 0;",
		"break;",
		"continue;",
		"case 1: ;",
		"default: ;",
		"goto L;\nL: ;",
		"case 0: break;",
		"default: break;",
	};
	static const char *fn_stray[] = {
		"case 0: break;",
		"default: break;",
		"case 1: ;",
		"default: ;",
	};
	static const char *fixed_reject[] = {
		"int main(void){ for (; defer 0;) {} return 0; }",
		"int main(void){ if (1) defer (void)0; return 0; }",
		"void f(void){ (void)(defer { }); }",
		"void f(void){ (void)(defer cleanup()); } void cleanup(void);",
		"void f(void){ int *p=0; (void)(p orelse 0); }",
		"void f(void){ int x=0; (void)(x orelse 1); }",
		"int main(void){ int x=0; x = x orelse orelse 1; return x; }",
		"int main(void){ int b=0; return 0, b = 0 orelse 5; }",
		"int main(void){ int x = g() orelse orelse 0; return x; } int g(void);",
		"void f(void){ goto L; { defer { } L: ; } }",
		"void f(void){ { defer { } L: ; } goto L; }",
		"void f(void){ goto L; { defer cleanup(); L: ; } } void cleanup(void);",
		"void f(void){ goto L; for (int x=1; x<2; x++) { L: ; (void)x; } }",
		"void f(void){ goto L; { int x=1; (void)x; L: ; } }",
		"void f(void){ goto L; { int x=1; int y=2; (void)x; L: ; } }",
		"void f(void){ for (;defer 0;) {} }",
		"typedef union U { int x; } U; void f(void){ U u = u orelse u; (void)u; }",
		"void f(int x){ switch(x){ case (x orelse 1): break; } }",
		"int main(void){ int x=0; x = x orelse orelse orelse 1; return x; }",
	};

	CmStats st = {0};
	char src[384];

	for (size_t i = 0; i < sizeof(file_scope) / sizeof(file_scope[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(file_scope[i]);
		if (!cm_err(&r))
			cm_note(&st, "reject file-scope i=%zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(fn_stray) / sizeof(fn_stray[0]); i++) {
		snprintf(src, sizeof(src), "void f(void){ %s }", fn_stray[i]);
		st.cells++;
		PrismResult r = cm_tx(src);
		if (!cm_err(&r))
			cm_note(&st, "reject fn-stray i=%zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(fixed_reject) / sizeof(fixed_reject[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(fixed_reject[i]);
		if (!cm_err(&r))
			cm_note(&st, "reject fixed i=%zu", i);
		prism_free(&r);
	}
	cm_report("gen/reject-alphabet", &st);
}

void run_completeness_tests(void) {
	printf("\n=== COMPLETENESS (T1′ generative closed) ===\n");
	cm_gen_stmt_defer();
	cm_gen_orelse_sites();
	cm_gen_autostatic();
	cm_gen_goto_closed();
	cm_gen_polarity();
	cm_gen_hunt3_seed();
	cm_gen_autounreach();
	cm_gen_bounds_wrap();
	cm_gen_zeroinit();
	cm_gen_defer_cfg();
	cm_gen_feature_cube();
	cm_gen_golf_pins();
	cm_gen_reject_alphabet();
	cm_gen_soft_ident();
	cm_gen_orelse_actions();
	cm_gen_zeroinit_dense();
	/* Promoted from open + new hunters (SPEC-correct oracles). */
	cm_gen_cast_subscript();
	cm_gen_name_semantics();
	cm_gen_attr_multidecl();
	cm_gen_raw_suppress();
	cm_gen_linemarker();
	cm_gen_illformed();
	cm_gen_generic_decl();
	cm_gen_bounds_bypass();
	cm_gen_bounds_multidim();
	cm_gen_bounds_filescope();
	cm_gen_cast_correct();
	cm_gen_goto_assign_first();
	cm_gen_computed_goto();
	cm_gen_orelse_reject_dense();
	cm_gen_defer_expr_splice();
	cm_gen_raw_feature_matrix();
	cm_gen_ctrl_paren_defer();
	cm_gen_bounds_cast_oob();
	cm_gen_switch_case_bypass();
	cm_gen_stmt_expr_defer_tail();
	cm_gen_multi_raw_prefix();
	cm_gen_atomic_zi_init();
	cm_gen_defer_braceless_decl();
	cm_gen_orelse_storage();
	cm_gen_nested_defer();
	cm_gen_defer_cf_body();
	cm_gen_goto_vla_cross();
	cm_gen_bounds_ptr_arith();
	cm_gen_attr_ctrlflow();
	cm_gen_case_orelse();
	cm_gen_shadow_after_defer();
	cm_gen_bounds_bypass_dense();
	cm_gen_unreach_sites();
	cm_gen_autostatic_sites();
	cm_gen_zeroinit_sites();
	cm_gen_orelse_expr_ctx();
	cm_gen_taint_defer();
	cm_gen_label_defer_loop();
	cm_gen_orelse_type_junk_dense();
	cm_gen_generic_assoc();
	cm_gen_comma_orelse_split();
	cm_gen_braceless_ctrl_defer();
	cm_gen_cast_orelse_junk();
	cm_gen_nested_typeof_orelse();
	cm_gen_compound_lit_orelse();
	cm_gen_designator_orelse();
	cm_gen_bitint_orelse();
	cm_gen_atomic_orelse();
	cm_gen_pp_span_orelse();
	cm_gen_empty_orelse_action();
	cm_gen_dim_orelse_uneval();
	cm_gen_bounds_param_shadow();
}

/*
 * Soft-keyword × namespace role product: raw/defer/orelse as ordinary names.
 * Oracle: accept (no false reject). Complements contexts ident-namespaces
 * with denser role coverage including labels/params/enum.
 */
static void cm_gen_soft_ident(void) {
	static const char *kws[] = { "raw", "defer", "orelse" };
	static const char *tmpls[] = {
		"int f(void){ int %s = 9; return %s; }",
		"int f(void){ int %s[3] = {1,2,3}; return %s[0]; }",
		"int f(void){ int %s[2][2] = {{1,2},{3,4}}; return %s[0][1]; }",
		"struct S { int %s; }; int f(void){ struct S s; s.%s = 3; return s.%s; }",
		"struct S { int x; int %s; }; int f(void){ struct S s; s.%s = 3; return s.%s; }",
		"typedef int %s; int f(void){ %s v = 5; return v; }",
		"typedef int %s; int f(void){ %s a, b; a = b = 1; return a+b; }",
		"static int %s(int a){ return a; } int f(void){ return %s(7); }",
		"int f(void){ goto %s; %s: return 1; }",
		"int f(void){ int x=0; goto %s; %s: return x; }",
		"int f(int %s){ return %s; }",
		"int f(int %s, int y){ return %s + y; }",
		"enum E { %s = 4 }; int f(void){ return %s; }",
		"enum E { A, %s, C }; int f(void){ return %s; }",
		"int f(void){ int %s = 1; return (%s) + ((%s) ? 1 : 0); }",
		"union U { int %s; }; int f(void){ union U u; u.%s = 2; return u.%s; }",
		"int f(void){ int *%s = 0; return %s ? 1 : 0; }",
		"int f(void){ typeof(int) %s = 3; return %s; }",
	};
	CmStats st = {0};
	char src[512];
	for (size_t k = 0; k < 3; k++) {
		for (size_t t = 0; t < sizeof(tmpls) / sizeof(tmpls[0]); t++) {
			const char *w = kws[k];
			snprintf(src, sizeof(src), tmpls[t], w, w, w, w);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_ok(&r))
				cm_note(&st, "soft-ident false-reject kw=%s tmpl=%zu", w, t);
			prism_free(&r);
		}
	}
	cm_report("gen/soft-ident", &st);
}

/*
 * Orelse action alphabet × primary form — decl-top only (closed).
 * Actions: value, return, break, continue, goto, block.
 */
static void cm_gen_orelse_actions(void) {
	static const char *primaries[] = {
		"g()",
		"p",
		"g() orelse g()",
		"rd()",
		"gp()",
		"g() orelse rd()",
	};
	static const char *actions[] = {
		"0",
		"1",
		"5",
		"g()",
		"return 0",
		"return 1",
		"{ (void)0; }",
		"{ return 0; }",
	};
	static const char *loop_actions[] = {
		"break",
		"continue",
		"return 0",
		"{ break; }",
	};

	CmStats st = {0};
	char src[768];

	for (size_t p = 0; p < sizeof(primaries) / sizeof(primaries[0]); p++) {
		int ptr = !strcmp(primaries[p], "p") || !strcmp(primaries[p], "gp()");
		const char *pre = "int g(void); int rd(void); int *gp(void);\n";
		for (size_t a = 0; a < sizeof(actions) / sizeof(actions[0]); a++) {
			if (strstr(actions[a], "return") || strstr(actions[a], "{")) {
				snprintf(src, sizeof(src),
					 "%sint f(void){ int *p = gp(); %s = %s orelse %s; return 0; }\n",
					 pre, ptr ? "int *x" : "int x", primaries[p], actions[a]);
			} else if (ptr) {
				const char *fb = actions[a];
				if (strstr(fb, "g(") || strstr(fb, "rd(")) fb = "0";
				snprintf(src, sizeof(src),
					 "%sint f(void){ int *p = gp(); int *x = %s orelse (%s); return x?1:0; }\n",
					 pre, primaries[p], fb);
			} else {
				snprintf(src, sizeof(src),
					 "%sint f(void){ int *p = gp(); int x = %s orelse %s; return x; }\n",
					 pre, primaries[p], actions[a]);
			}
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_ok(&r) || !r.output || cm_kw(r.output, "orelse"))
				cm_note(&st, "orelse-action p=%zu a=%zu", p, a);
			prism_free(&r);
		}
		for (size_t a = 0; a < sizeof(loop_actions) / sizeof(loop_actions[0]); a++) {
			snprintf(src, sizeof(src),
				 "%sint f(void){ int *p = gp(); for(;;){ %s = %s orelse %s; (void)0; }\n"
				 "  return 0; }\n",
				 pre, ptr ? "int *x" : "int x", primaries[p], loop_actions[a]);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_ok(&r) || !r.output || cm_kw(r.output, "orelse"))
				cm_note(&st, "orelse-loop-action p=%zu a=%zu", p, a);
			prism_free(&r);
		}
	}
	cm_report("gen/orelse-actions", &st);
}

static void cm_gen_zeroinit_dense(void) {
	static const char *ranks[] = {
		"int x",
		"char x",
		"short x",
		"long x",
		"long long x",
		"float x",
		"double x",
		"int *x",
		"char *x",
		"int x[4]",
		"int x[8]",
		"int x[2][2]",
		"int x[2][2][2]",
		"struct { int a; } x",
		"struct { int a; int b; } x",
		"union { int a; char b; } x",
		"typeof(int) x",
		"typeof(int[4]) x",
		"_Alignas(8) int x",
		"_Alignas(16) char x",
	};
	static const char *quals[] = { "", "const ", "volatile ", "register ", "static " };
	static const char *inits[] = { "", " = {0}", " = 0", " = {1}" };

	CmStats st = {0};
	char decl[256], src[768];
	PrismFeatures feat = prism_defaults();
	feat.bounds_check = false;

	for (size_t r = 0; r < sizeof(ranks) / sizeof(ranks[0]); r++) {
		for (size_t q = 0; q < sizeof(quals) / sizeof(quals[0]); q++) {
			for (size_t i = 0; i < sizeof(inits) / sizeof(inits[0]); i++) {
				/* skip ill-typed combos */
				if (strstr(ranks[r], "[") && !strcmp(inits[i], " = 0"))
					continue;
				if (strstr(ranks[r], "struct") && !strcmp(inits[i], " = 0"))
					continue;
				if (strstr(ranks[r], "union") && !strcmp(inits[i], " = 0"))
					continue;
				if (!strcmp(inits[i], " = {1}") &&
				    !strstr(ranks[r], "[") && !strstr(ranks[r], "struct") &&
				    !strstr(ranks[r], "union") && !strstr(ranks[r], "typeof(int["))
					continue; /* scalar brace-init with non-zero is noisy */
				if (strstr(quals[q], "const") && inits[i][0] == '\0' &&
				    (strstr(ranks[r], "union") || strstr(ranks[r], "[")))
					continue; /* const VLA/union uninit often rejects */

				snprintf(decl, sizeof(decl), "%s%s%s", quals[q], ranks[r], inits[i]);
				snprintf(src, sizeof(src), "void f(void){ %s; (void)sizeof(char); }\n",
					 decl);
				st.cells++;
				PrismResult res = cm_txf(src, feat);
				if (!cm_ok(&res)) {
					prism_free(&res);
					continue; /* reject OK for some const/register combos */
				}
				int uninit = (inits[i][0] == '\0');
				int has_zi = res.output && (strstr(res.output, "memset") ||
							    strstr(res.output, "__prism_p_") ||
							    strstr(res.output, "= {0}") ||
							    strstr(res.output, "= 0"));
				if (uninit && !has_zi && !strstr(quals[q], "const") &&
				    !strstr(quals[q], "register") &&
				    !strstr(quals[q], "static"))
					cm_note(&st, "zi-dense miss r=%zu q=%zu", r, q);
				prism_free(&res);
			}
		}
	}
	cm_report("gen/zeroinit-dense", &st);
}

/* ═══════════════════════════════════════════════════════════════════════
 * OPEN generative sweeps — correct oracles; red until fixed
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Cast × base × index — OOB decided by host sizeof arithmetic.
 * Oracle: OOB ⇒ reject OR emitted check must not use naive
 * sizeof(arr)/sizeof(arr[0]) when cast_elem != base_elem.
 */
static void cm_gen_cast_subscript(void) {
	struct {
		const char *ctype;
		size_t sz;
		int n;
		const char *var;
		const char *zero_init;
	} bases[] = {
		{ "int", sizeof(int), 2, "a", "{0,0}" },
		{ "int", sizeof(int), 4, "a", "{0}" },
		{ "unsigned char", sizeof(unsigned char), 8, "b", "{0}" },
		{ "short", sizeof(short), 4, "s", "{0}" },
		{ "long", sizeof(long), 2, "L", "{0}" },
		{ "char", sizeof(char), 16, "c", "{0}" },
	};
	struct {
		const char *cast;
		size_t sz;
	} casts[] = {
		{ "double *", sizeof(double) },
		{ "int *", sizeof(int) },
		{ "char *", sizeof(char) },
		{ "short *", sizeof(short) },
		{ "long *", sizeof(long) },
		{ "float *", sizeof(float) },
	};
	static const int idxs[] = { 0, 1, 2, 3, 4, 7, 8, 15, 16 };

	CmStats st = {0};
	char src[512], pat1[72], pat2[72];

	for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
		for (size_t c = 0; c < sizeof(casts) / sizeof(casts[0]); c++) {
			size_t bytes = (size_t)bases[b].n * bases[b].sz;
			size_t max_i = casts[c].sz ? bytes / casts[c].sz : 0;
			int cast_differs = casts[c].sz != bases[b].sz;
			for (size_t k = 0; k < sizeof(idxs) / sizeof(idxs[0]); k++) {
				int idx = idxs[k];
				if ((size_t)idx > max_i + 2 && idx > 4)
					continue; /* trim sparse tail */
				int oob = (size_t)idx >= max_i ||
					  ((size_t)idx + 1) * casts[c].sz > bytes;
				snprintf(src, sizeof(src),
					 "int main(void){ %s %s[%d]=%s; return (int)((%s)%s)[%d]; }\n",
					 bases[b].ctype, bases[b].var, bases[b].n,
					 bases[b].zero_init, casts[c].cast, bases[b].var, idx);
				st.cells++;
				PrismResult r = cm_tx(src);
				if (!oob) {
					if (!cm_ok(&r))
						cm_note(&st, "cast-sub false-rej %s→%s [%d]",
							bases[b].ctype, casts[c].cast, idx);
				} else if (cast_differs) {
					snprintf(pat1, sizeof(pat1), "sizeof(%s) / sizeof(%s[0])",
						 bases[b].var, bases[b].var);
					snprintf(pat2, sizeof(pat2), "sizeof(%s)/sizeof(%s[0])",
						 bases[b].var, bases[b].var);
					int naive = r.output && (strstr(r.output, pat1) ||
								 strstr(r.output, pat2));
					int wrapped = r.output && strstr(r.output, "[__prism_bchk");
					int cast_sz = r.output && (strstr(r.output, "sizeof(*") ||
								   strstr(r.output, "sizeof((*"));
					if (!cm_err(&r) && naive)
						cm_note(&st, "cast-sub naive %s→%s [%d]",
							bases[b].ctype, casts[c].cast, idx);
					else if (!cm_err(&r) && wrapped && !cast_sz)
						cm_note(&st, "cast-sub wrap w/o cast-sizeof %s→%s [%d]",
							bases[b].ctype, casts[c].cast, idx);
				}
				prism_free(&r);
			}
		}
	}
	cm_report("gen/cast-subscript", &st);
}

/*
 * Name × role product for noreturn / taint spelling.
 * Roles: user TU function, struct member, call-through-user.
 */
static void cm_gen_name_semantics(void) {
	static const char *names[] = {
		"exit", "abort", "quick_exit", "_Exit", "_exit", "setjmp", "vfork", "longjmp",
	};
	CmStats st = {0};
	char src[640];

	for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
		for (size_t role = 0; role < 2; role++) {
			if (role == 0) {
				snprintf(src, sizeof(src),
					 "int %s(void){ return 0; }\n"
					 "int main(void){ return %s(); }\n",
					 names[n], names[n]);
			} else {
				snprintf(src, sizeof(src),
					 "struct S { int %s; };\n"
					 "int main(void){ struct S s={0}; return s.%s; }\n",
					 names[n], names[n]);
			}
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_ok(&r) ||
			    (r.output && strstr(r.output, "__builtin_unreachable")))
				cm_note(&st, "name-sem %s role=%zu", names[n], role);
			prism_free(&r);
		}
	}

	/* taint × defer: user setjmp/vfork must not false-taint */
	static const char *taints[] = { "setjmp", "vfork" };
	for (size_t i = 0; i < 2; i++) {
		snprintf(src, sizeof(src),
			 "int %s(void){ return 0; }\n"
			 "void f(void){ defer { } (void)%s(); }\n",
			 taints[i], taints[i]);
		st.cells++;
		PrismResult r = cm_tx(src);
		if (!cm_ok(&r)) cm_note(&st, "name-sem taint+defer %s", taints[i]);
		prism_free(&r);
	}
	cm_report("gen/name-semantics", &st);
}

/* Attr spelling × return-type × multi-decl → both names get unreach (or reject). */
static void cm_gen_attr_multidecl(void) {
	static const char *attrs[] = {
		"_Noreturn",
		"[[noreturn]]",
		"__attribute__((noreturn))",
		"__attribute__((__noreturn__))",
	};
	static const char *rets[] = { "void", "int" };
	CmStats st = {0};
	char src[768];

	for (size_t a = 0; a < sizeof(attrs) / sizeof(attrs[0]); a++) {
		for (size_t r = 0; r < 2; r++) {
			if (!strcmp(rets[r], "void"))
				snprintf(src, sizeof(src),
					 "%s void a(void), b(void);\n"
					 "void a(void){}\nvoid b(void){}\n"
					 "int main(void){ a(); b(); return 0; }\n",
					 attrs[a]);
			else
				snprintf(src, sizeof(src),
					 "%s int a(void), b(void);\n"
					 "int a(void){ for(;;){} }\n"
					 "int b(void){ for(;;){} }\n"
					 "int main(void){ a(); b(); return 0; }\n",
					 attrs[a]);
			st.cells++;
			PrismResult res = cm_tx(src);
			if (cm_err(&res)) {
				prism_free(&res);
				continue; /* reject-all OK */
			}
			int n_unreach = 0;
			if (res.output) {
				for (const char *p = res.output;
				     (p = strstr(p, "__builtin_unreachable")) != NULL; p++)
					n_unreach++;
				/* MSVC target emits `__assume(0)` instead. */
				for (const char *p = res.output; (p = strstr(p, "__assume(0)")) != NULL;
				     p++)
					n_unreach++;
			}
			if (n_unreach < 2)
				cm_note(&st, "multidecl attr=%zu ret=%s unreach=%d", a, rets[r],
					n_unreach);
			prism_free(&res);
		}
	}
	cm_report("gen/attr-multidecl", &st);
}

/* Goto gap cells: every (j,o) with expect_err == -1 must reject. */
static void cm_gen_goto_open(void) {
	CmStats st = {0};
	char src[512];
	for (int j = 0; j < CM_JMP_N; j++) {
		for (int o = 0; o < CM_OBS_N; o++) {
			if (cm_goto_closed_expect_err((CmJmp)j, (CmObs)o) != -1)
				continue;
			cm_build_goto(src, sizeof(src), (CmJmp)j, (CmObs)o);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_err(&r))
				cm_note(&st, "goto-open miss-reject j=%d o=%d", j, o);
			prism_free(&r);
		}
	}
	cm_report("gen/goto-open", &st);
}

/*
 * raw{} × feature-payload product.
 * Oracle: inside raw, defer/orelse keywords survive; auto-static does not inject.
 */
static void cm_gen_raw_suppress(void) {
	static const char *payloads[] = {
		"defer cleanup();",
		"defer { }",
		"defer { cleanup(); }",
		"int *q = p orelse 0; (void)q;",
		"int *q = p orelse p orelse 0; (void)q;",
		"const int k[3]={1,2,3}; (void)k;",
		"const char msg[]=\"hi\"; (void)msg;",
		"int a[4]; (void)a[1];",
		"defer { } int *q = p orelse 0; (void)q; const int k[2]={1,2}; (void)k;",
		"_Noreturn void die(void); die();",
	};
	static const int keep_defer[] = { 1, 1, 1, 0, 0, 0, 0, 0, 1, 0 };
	static const int keep_orelse[] = { 0, 0, 0, 1, 1, 0, 0, 0, 1, 0 };
	static const int no_static[] = { 0, 0, 0, 0, 0, 1, 1, 0, 1, 0 };
	static const int no_bchk[] = { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0 };
	static const int no_unreach[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

	static const char *forms[] = {
		"void cleanup(void); int *p;\nvoid f(void){ p=0; raw { %s } }\n",
		"void cleanup(void); int *p;\nvoid f(void){ p=0; raw {{ %s }} }\n",
		"void cleanup(void); int *p;\nvoid f(void){ p=0; if(1) raw { %s } }\n",
	};

	CmStats st = {0};
	char src[1200];
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;

	for (size_t form = 0; form < sizeof(forms) / sizeof(forms[0]); form++) {
		for (size_t p = 0; p < sizeof(payloads) / sizeof(payloads[0]); p++) {
			snprintf(src, sizeof(src), forms[form], payloads[p]);
			st.cells++;
			PrismResult r = cm_txf(src, f);
			if (!cm_ok(&r) || !r.output) {
				cm_note(&st, "raw form=%zu p=%zu reject", form, p);
				prism_free(&r);
				continue;
			}
			if (keep_defer[p] && !cm_kw(r.output, "defer"))
				cm_note(&st, "raw lowered defer form=%zu p=%zu", form, p);
			else if (keep_orelse[p] && !cm_kw(r.output, "orelse"))
				cm_note(&st, "raw lowered orelse form=%zu p=%zu", form, p);
			else if (no_static[p] && strstr(r.output, "static const"))
				cm_note(&st, "raw auto-static form=%zu p=%zu", form, p);
			else if (no_bchk[p] && cm_has_bchk_wrap(r.output))
				cm_note(&st, "raw bounds wrap form=%zu p=%zu", form, p);
			else if (no_unreach[p] && cm_has_unreach(r.output))
				cm_note(&st, "raw unreach inject form=%zu p=%zu", form, p);
			prism_free(&r);
		}
	}
	cm_report("gen/raw-suppress", &st);
}

/*
 * Line-marker flag sequences: generate from flag atoms.
 * Oracle under no-flatten: user_fn must survive in output.
 */
static void cm_gen_linemarker(void) {
	static const char *seqs[] = {
		"# 1 \"sys.h\" 3\n",
		"# 1 \"sys.h\" 3\n# 2 \"user.c\" 2\n",
		"%: 1 \"sys.h\" 3\n",
		"# 1 \"a.h\" 1 3\n",
		"# 1 \"sys.h\" 3\n# 1 \"user.c\"\n",
		"# 1 \"sys.h\" 1\n# 1 \"sys.h\" 3\n# 2 \"user.c\" 2\n",
		"%: 1 \"sys.h\" 1 3\n%: 2 \"user.c\" 2\n",
		"# 1 \"sys.h\" 3 4\n",
	};
	CmStats st = {0};
	char src[512];
	PrismFeatures f = prism_defaults();
	f.flatten_headers = false;

	for (size_t i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
		snprintf(src, sizeof(src), "%sint user_fn(void){ return 42; }\n", seqs[i]);
		st.cells++;
		PrismResult r = cm_txf(src, f);
		if (!cm_ok(&r) || !r.output || !strstr(r.output, "user_fn"))
			cm_note(&st, "lineflag seq %zu dropped TU", i);
		prism_free(&r);
	}
	cm_report("gen/linemarker", &st);
}

/* Ill-formed control alphabet — every form must reject. */
static void cm_gen_illformed(void) {
	static const char *forms[] = {
		"void f(void){ while (int x=1){ (void)x; break; } }",
		"void f(void){ for (raw { int x=1; }; 0; ){} }",
		"void f(void){ for (raw int x = 1; 0; ){} }",
		"void f(void){ do int x=1; while(0); (void)x; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(forms[i]);
		if (!cm_err(&r)) cm_note(&st, "illformed accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/illformed", &st);
}

/*
 * Decl-context _Generic × association count — must accept cleanly.
 */
static void cm_gen_generic_decl(void) {
	static const char *forms[] = {
		"int x = _Generic(0, int: 1, default: 0);",
		"int x = _Generic(0, int: 1, long: 2, default: 0);",
		"const int x = _Generic(0, int: 1, default: 0);",
		"int *p = 0; int x = _Generic(p, int*: 1, default: 0);",
	};
	CmStats st = {0};
	char src[384];
	for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++) {
		snprintf(src, sizeof(src), "%s\nint main(void){ return x; }\n", forms[i]);
		st.cells++;
		PrismResult r = cm_tx(src);
		if (!cm_ok(&r)) cm_note(&st, "generic-decl %zu", i);
		prism_free(&r);
	}
	cm_report("gen/generic-decl", &st);
}


/* ── New closed hunters (expand until red) ───────────────────────────── */

static void cm_gen_bounds_bypass(void) {
	static const char *must_err[] = {
		"void f(void){ int a[8]; int i=1; (void)i[a]; }",
		"void f(void){ int a[8]; (void)0[a]; }",
		"void f(void){ int a[8]; int i=1; (void)(i)[a]; }",
		"void f(void){ int a[8]; (void)*(a+3); }",
		"void f(void){ int a[8]; (void)*(&a[0]+2); }",
		"void f(void){ int a[4][4]; (void)*(a[1]+2); }",
		"void f(void){ int a[8]; int *p=a; (void)p[a-a]; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "bounds-bypass accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/bounds-bypass", &st);
}

static void cm_gen_bounds_multidim(void) {
	static const char *srcs[] = {
		"void f(void){ int a[4][4]; (void)a[1][2]; }",
		"void f(void){ int a[2][3][4]; (void)a[1][2][3]; }",
		"void f(void){ char a[8][8]; (void)a[0][7]; }",
		"void f(void){ int a[3][5]; int i=1,j=2; (void)a[i][j]; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(srcs)/sizeof(srcs[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(srcs[i]);
		if (!cm_ok(&r) || !r.output || !cm_has_bchk_wrap(r.output))
			cm_note(&st, "multidim miss wrap %zu", i);
		/* count wraps — want at least 2 for 2D */
		int n = 0;
		if (r.output)
			for (const char *p = r.output; (p = strstr(p, "[__prism_bchk")) != NULL; p++)
				n++;
		if (i < 3 && n < 2)
			cm_note(&st, "multidim shallow wraps %zu n=%d", i, n);
		prism_free(&r);
	}
	cm_report("gen/bounds-multidim", &st);
}

static void cm_gen_bounds_filescope(void) {
	static const char *srcs[] = {
		"int g[8]; void f(void){ (void)g[3]; }",
		"static int g[4]; void f(void){ (void)g[1]; }",
		"char g[16]; void f(void){ (void)g[0]; }",
		"int g[2][2]; void f(void){ (void)g[1][0]; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(srcs)/sizeof(srcs[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(srcs[i]);
		if (!cm_ok(&r) || !r.output || !cm_has_bchk_wrap(r.output))
			cm_note(&st, "filescope miss wrap %zu", i);
		prism_free(&r);
	}
	cm_report("gen/bounds-filescope", &st);
}

static void cm_gen_cast_correct(void) {
	/* In-range casted subscripts must wrap with cast element sizeof. */
	static const char *srcs[] = {
		"int main(void){ int a[4]={0}; return (int)((char*)a)[0]; }",
		"int main(void){ int a[4]={0}; return (int)((char*)a)[3]; }",
		"int main(void){ int a[2]={0}; return (int)((short*)a)[0]; }",
		"int main(void){ char a[16]={0}; return (int)((int*)a)[0]; }",
		"int main(void){ int a[8]={0}; return (int)((double*)a)[0]; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(srcs)/sizeof(srcs[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(srcs[i]);
		if (!cm_ok(&r) || !r.output)
			cm_note(&st, "cast-correct status %zu", i);
		else if (!strstr(r.output, "[__prism_bchk") ||
			 !(strstr(r.output, "sizeof(*") || strstr(r.output, "sizeof((*")))
			cm_note(&st, "cast-correct miss cast-bound %zu", i);
		else if (strstr(r.output, "sizeof(a)/sizeof(a[0])") ||
			 strstr(r.output, "sizeof(a) / sizeof(a[0])"))
			cm_note(&st, "cast-correct naive %zu", i);
		prism_free(&r);
	}
	cm_report("gen/cast-correct", &st);
}

static void cm_gen_goto_assign_first(void) {
	/* `x = x + 1` is NOT a dominating first assignment — must still reject. */
	static const char *must_err[] = {
		"void f(void){ goto L; { int x; L: x = x + 1; (void)x; } }",
		"void f(void){ goto L; { int x; L: x += 1; (void)x; } }",
		"void f(void){ goto L; { int x; L: x++; (void)x; } }",
		"void f(void){ goto L; { int x; L: ++x; (void)x; } }",
		"void f(void){ goto L; { int x; L: x = x; (void)x; } }",
	};
	static const char *must_ok[] = {
		"void f(void){ goto L; { int x; L: x = 1; (void)x; } }",
		"void f(void){ goto L; { int x; L: x = 0; (void)x; } }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "assign-first false-ok %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "assign-first false-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/goto-assign-first", &st);
}

static void cm_gen_computed_goto(void) {
	static const char *must_err[] = {
		"void f(void){ void *p=&&L; defer { } goto *p; L: ; }",
		"void f(void){ void *p=&&L; int x; goto *p; L: (void)x; }",
		"void f(void){ void *t[]={&&L}; defer cleanup(); goto *t[0]; L: ; } void cleanup(void);",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "computed-goto accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/computed-goto", &st);
}

static void cm_gen_orelse_reject_dense(void) {
	static const char *must_err[] = {
		"int main(void){ int x=0; x = x orelse orelse 1; return x; }",
		"int main(void){ int x=0; x = x orelse orelse orelse 1; return x; }",
		"int g(void); int main(void){ return g() orelse 1; }",
		"int g(void); int main(void){ return (g() orelse 1); }",
		"void f(int x){ switch(x){ case (x orelse 1): break; } }",
		"void f(int x){ switch(x){ case 1 ... (x orelse 2): break; } }",
		"int main(void){ int b=0; return 0, b = 0 orelse 5; }",
		"void f(void){ int *p=0; (void)(p orelse 0); }",
		"void f(void){ (void)(defer { }); }",
		"int main(void){ for(;defer 0;){} return 0; }",
		"int main(void){ while(defer 0){} return 0; }",
		"int main(void){ if(1) defer (void)0; return 0; }",
		"void f(void){ int a[0 orelse 1]; (void)a; }", /* may accept if ICE */
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		/* index 12: [0 orelse 1] is ICE — may accept+lower */
		if (i == 12) {
			if (!(cm_err(&r) || (r.output && !cm_kw(r.output, "orelse"))))
				cm_note(&st, "orelse-reject leak %zu", i);
		} else if (!cm_err(&r))
			cm_note(&st, "orelse-reject accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/orelse-reject-dense", &st);
}

static void cm_gen_defer_expr_splice(void) {
	/* Expression-position undeclared defer must stay identifier (not absorb next stmt). */
	static const struct { const char *src; const char *keep; } cells[] = {
		{ "int main(void){ int x=4; int r; r = sizeof defer; x; return r; }", "sizeof defer" },
		{ "int main(void){ int x=0; int r; r = ! defer; x; return r; }", "! defer" },
		{ "int main(void){ int x=0; int r; r = ~ defer; x; return r; }", "~ defer" },
		{ "int main(void){ int x=0; int r; r = + defer; x; return r; }", "+ defer" },
		{ "int main(void){ int x=0; int *p; p = & defer; x; (void)p; return x; }", "& defer" },
		{ "int main(void){ int x=0; int r; r = (int) defer; x; return r; }", "defer" },
		{ "int main(void){ int x=0; int r; r = 1 ? 2 : defer; x; return r; }", "defer" },
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(cells)/sizeof(cells[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(cells[i].src);
		if (!cm_ok(&r) || !r.output || !strstr(r.output, cells[i].keep))
			cm_note(&st, "defer-splice %zu", i);
		/* Must not absorb following `x` into the sizeof/unary. */
		if (r.output && strstr(r.output, "sizeof x"))
			cm_note(&st, "defer-splice absorbed %zu", i);
		prism_free(&r);
	}
	cm_report("gen/defer-expr-splice", &st);
}

static void cm_gen_raw_feature_matrix(void) {
	/* denser raw × payload × nesting */
	static const char *payloads[] = {
		"defer cleanup();",
		"int *q = p orelse 0; (void)q;",
		"const int k[2]={1,2}; (void)k;",
		"int a[4]; (void)a[1];",
		"_Noreturn void die(void); die();",
		"defer { } int *q = p orelse 0; (void)q;",
	};
	static const char *forms[] = {
		"void cleanup(void); int *p;\nvoid f(void){ p=0; raw { %s } }\n",
		"void cleanup(void); int *p;\nvoid f(void){ p=0; raw {{ %s }} }\n",
		"void cleanup(void); int *p;\nvoid f(void){ p=0; if(1) raw { %s } }\n",
		"void cleanup(void); int *p;\nvoid f(void){ p=0; for(int i=0;i<1;i++) raw { %s } }\n",
		"void cleanup(void); int *p;\nvoid f(void){ p=0; switch(0){ default: raw { %s } break; } }\n",
	};
	CmStats st = {0};
	char src[1400];
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	for (size_t form = 0; form < sizeof(forms)/sizeof(forms[0]); form++) {
		for (size_t p = 0; p < sizeof(payloads)/sizeof(payloads[0]); p++) {
			snprintf(src, sizeof(src), forms[form], payloads[p]);
			st.cells++;
			PrismResult r = cm_txf(src, f);
			if (!cm_ok(&r) || !r.output) {
				cm_note(&st, "raw-matrix reject f=%zu p=%zu", form, p);
				prism_free(&r);
				continue;
			}
			if (strstr(payloads[p], "defer") && !cm_kw(r.output, "defer"))
				cm_note(&st, "raw-matrix defer-lowered f=%zu p=%zu", form, p);
			if (strstr(payloads[p], "orelse") && !cm_kw(r.output, "orelse"))
				cm_note(&st, "raw-matrix orelse-lowered f=%zu p=%zu", form, p);
			if (strstr(payloads[p], "const int") &&
			    (strstr(r.output, "static const int k") ||
			     strstr(r.output, "static  const int k")))
				cm_note(&st, "raw-matrix auto-static f=%zu p=%zu", form, p);
			if (strstr(payloads[p], "a[1]") && cm_has_bchk_wrap(r.output))
				cm_note(&st, "raw-matrix bounds f=%zu p=%zu", form, p);
			if (strstr(payloads[p], "die") && strstr(r.output, "__builtin_unreachable"))
				cm_note(&st, "raw-matrix unreach f=%zu p=%zu", form, p);
			prism_free(&r);
		}
	}
	cm_report("gen/raw-feature-matrix", &st);
}


static void cm_gen_ctrl_paren_defer(void) {
	/* defer in any control paren form must reject — including defer <lit>. */
	static const char *must_err[] = {
		"int main(void){ while(defer 0){} return 0; }",
		"int main(void){ while(defer 1){} return 0; }",
		"int main(void){ if(defer 0){} return 0; }",
		"int main(void){ if(defer 1) (void)0; return 0; }",
		"int main(void){ switch(defer 0){ default: break; } return 0; }",
		"int main(void){ for(;defer 0;){} return 0; }",
		"int main(void){ for(;(defer 0);){} return 0; }",
		"int main(void){ while((defer)){} return 0; }",
		"int main(void){ do {} while(defer 0); return 0; }",
	};
	static const char *must_ok[] = {
		"int main(void){ int defer=0; while(defer){} return 0; }",
		"int main(void){ struct S{int defer;}; struct S s={0}; while(s.defer){} return 0; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "ctrl-paren-defer accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "ctrl-paren-defer false-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/ctrl-paren-defer", &st);
}

static void cm_gen_bounds_cast_oob(void) {
	/* OOB casted subscript: must reject OR wrap with cast-sized bound (never naive). */
	struct { const char *src; int oob; } cells[] = {
		{ "int main(void){ int a[2]={0}; return (int)((double*)a)[0]; }", 0 },
		{ "int main(void){ int a[2]={0}; return (int)((double*)a)[1]; }", 1 },
		{ "int main(void){ int a[1]={0}; return (int)((char*)a)[0]; }", 0 },
		{ "int main(void){ int a[1]={0}; return (int)((char*)a)[4]; }", 1 },
		{ "int main(void){ char a[3]={0}; return (int)((int*)a)[0]; }", 0 },
		{ "int main(void){ char a[3]={0}; return (int)((int*)a)[1]; }", 1 },
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(cells)/sizeof(cells[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(cells[i].src);
		int naive = r.output && (strstr(r.output, "sizeof(a)/sizeof(a[0])") ||
					 strstr(r.output, "sizeof(a) / sizeof(a[0])"));
		if (cells[i].oob) {
			if (!cm_err(&r) && naive)
				cm_note(&st, "cast-oob naive %zu", i);
		} else {
			if (!cm_ok(&r)) cm_note(&st, "cast-oob false-rej %zu", i);
			else if (naive) cm_note(&st, "cast-oob inrange naive %zu", i);
		}
		prism_free(&r);
	}
	cm_report("gen/bounds-cast-oob", &st);
}

static void cm_gen_switch_case_bypass(void) {
	static const char *must_err[] = {
		"void f(int c){ switch(c){ case 0: { int x; case 1: (void)x; break; } } }",
		"void f(int c){ switch(c){ default: { defer { } case 1: break; } } }",
		"void f(int c){ switch(c){ case 0: { int x=1; case 1: (void)x; } } }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "switch-bypass accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/switch-case-bypass", &st);
}

static void cm_gen_stmt_expr_defer_tail(void) {
	/* SPEC C7: top-level defer in ({…}) rejected; C8: defer-block as last stmt rejected.
	 * Allowed: defer wrapped in nested block, with a following value stmt. */
	static const char *must_err[] = {
		"int f(void){ return ({ defer (void)0; }); }",
		"int f(void){ return ({ int x=1; defer (void)0; }); }",
		"int f(void){ return ({ defer { } }); }",
		"int f(void){ return ({ defer (void)0; 1; }); }",
		"int f(void){ return ({ int x=1; defer (void)x; x; }); }",
		"int f(void){ return ({ int r=1; { defer (void)0; } }); }",
		"int f(void){ return ({ { defer (void)0; } }); }",
	};
	static const char *must_ok[] = {
		"int f(void){ return ({ int r; { defer (void)0; r = 1; } r; }); }",
		"int f(void){ return ({ int x=1; { defer (void)x; } x; }); }",
		"int f(void){ return ({ { defer { } } 0; }); }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "stmt-expr-defer accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "stmt-expr-defer false-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/stmt-expr-defer-tail", &st);
}

static void cm_gen_multi_raw_prefix(void) {
	static const char *srcs[] = {
		"void f(void){ raw raw int x; (void)x; }",
		"void f(void){ raw raw int a[4]; (void)a[1]; }",
		"void f(int n){ for(raw raw int a[n];;){ (void)a; break; } }",
		"void f(void){ int a, raw b; (void)a; (void)b; }",
		"void f(void){ int a, raw b[4]; (void)a; (void)b[1]; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(srcs)/sizeof(srcs[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(srcs[i]);
		if (!cm_ok(&r) || !r.output)
			cm_note(&st, "multi-raw status %zu", i);
		else if (cm_kw(r.output, "raw") && i < 3)
			cm_note(&st, "multi-raw leak %zu", i);
		prism_free(&r);
	}
	cm_report("gen/multi-raw-prefix", &st);
}

static void cm_gen_atomic_zi_init(void) {
	static const char *must_err[] = {
		"void f(void){ for(_Atomic int x[2];;){ (void)x; break; } }",
		"void f(void){ if(_Atomic struct { int a; } s) (void)s; }",
	};
	static const char *must_ok[] = {
		"void f(void){ _Atomic int x; (void)x; }",
		"void f(void){ for(int i=0;i<1;i++){ _Atomic int x; (void)x; } }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "atomic-zi-init accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "atomic-zi-init false-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/atomic-zi-init", &st);
}


static void cm_gen_defer_braceless_decl(void) {
	/* SPEC C11: braceless defer of a declaration must reject. */
	static const char *must_err[] = {
		"void f(void){ defer int x; }",
		"void f(void){ defer int x = 1; }",
		"void f(void){ defer int x = 1 orelse 0; }",
		"void f(void){ defer const int x = 1; }",
		"void f(void){ defer static int x; }",
		"void f(void){ defer _Atomic int x; }",
		"void f(void){ defer struct S { int a; } s; }",
		"void f(void){ defer typedef int T; }",
	};
	static const char *must_ok[] = {
		"void f(void){ defer { int x; (void)x; } }",
		"void f(void){ defer { int x = 1; (void)x; } }",
		"void f(void){ defer { int *p = 0 orelse 0; (void)p; } }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "braceless-decl accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "braceless-decl false-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/defer-braceless-decl", &st);
}

static void cm_gen_orelse_storage(void) {
	/* Decl-init orelse with static/extern/TLS/constexpr storage must reject.
	 * Bare assignment orelse (even to static) is OK. Local decl-init is OK. */
	static const char *must_err[] = {
		"void f(void){ static int *p = 0 orelse 0; (void)p; }",
		"int *gp = 0 orelse 0;",
		"void f(void){ _Thread_local int *p = 0 orelse 0; (void)p; }",
		"void f(void){ constexpr int *p = 0 orelse 0; (void)p; }",
		"void f(void){ extern int *p = 0 orelse 0; (void)p; }",
		"static int *gp2 = 0 orelse 0;",
	};
	static const char *must_ok[] = {
		"void f(void){ int *p = 0 orelse 0; (void)p; }",
		"void f(void){ static int *p; p = 0; int *q = p orelse 0; (void)q; }",
		"void f(void){ int *p; p = 0 orelse 0; (void)p; }",
		"void f(void){ static int *p; p = 0 orelse 0; (void)p; }",
		"void f(void){ extern int *p; p = 0; int *q = p orelse 0; (void)q; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "orelse-storage accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "orelse-storage false-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/orelse-storage", &st);
}

static void cm_gen_nested_defer(void) {
	static const char *must_err[] = {
		"void f(void){ defer { defer (void)0; } }",
		"void f(void){ defer defer (void)0; }",
		"void f(void){ defer { if(1) defer (void)0; } }",
		"void f(void){ defer { { defer (void)0; } } }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "nested-defer accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/nested-defer", &st);
}

static void cm_gen_defer_cf_body(void) {
	static const char *must_err[] = {
		"void f(void){ defer { return; } }",
		"void f(void){ defer return; }",
		"void f(void){ defer { goto L; } L: ; }",
		"void f(void){ defer goto L; L: ; }",
		"void f(void){ for(;;){ defer { break; } } }",
		"void f(void){ for(;;){ defer break; } }",
		"void f(void){ for(;;){ defer { continue; } } }",
		"void f(void){ for(;;){ defer continue; } }",
	};
	static const char *must_ok[] = {
		"void f(void){ defer { for(;;) break; } }",
		"void f(void){ defer { for(;;) continue; } }",
		"void f(void){ defer { switch(0){ default: break; } } }",
		"void f(void){ defer { while(0) break; } }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "defer-cf accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "defer-cf false-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/defer-cf-body", &st);
}

static void cm_gen_goto_vla_cross(void) {
	static const char *must_err[] = {
		"void f(int n){ goto L; int a[n]; L: (void)a; }",
		"void f(int n){ goto L; { int a[n]; L: (void)a; } }",
		"void f(int n){ int a[n]; goto L; int b[n]; L: (void)a; (void)b; }",
	};
	static const char *must_ok[] = {
		"void f(int n){ int a[n]; goto L; L: (void)a; }",
		"void f(int n){ { int a[n]; (void)a; } goto L; L: ; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "goto-vla accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "goto-vla false-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/goto-vla-cross", &st);
}

static void cm_gen_bounds_ptr_arith(void) {
	/* must_wrap: 1 wrap, 0 accept-no-wrap, -1 reject (ptr-arith / commutative bypass) */
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	struct { const char *src; int expect; } cells[] = {
		{ "int main(void){ int a[4]={0}; return a[1]; }", 1 },
		{ "int main(void){ int a[4]={0}; int *p=a; return p[1]; }", 0 },
		{ "int main(void){ int a[4]={0}; return *(a+1); }", -1 },
		{ "int main(void){ int a[4]={0}; return 1[a]; }", -1 },
		{ "int main(void){ int a[2][3]={{0}}; return a[1][2]; }", 1 },
		{ "int main(void){ char a[8]={0}; return a[7]; }", 1 },
		{ "int main(void){ int a[4]={0}; return (a+0)[1]; }", -1 },
		{ "int main(void){ int a[4]={0}; return (+a)[1]; }", -1 },
		{ "int main(void){ int a[4]={0}; return *(a+0+1); }", -1 },
		{ "int a[3]; int main(void){ return a[1]; }", 1 },
		{ "int main(void){ struct { int a[4]; } s; return s.a[1]; }", 0 },
		{ "int main(void){ return (int[3]){1,2,3}[1]; }", 0 },
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(cells)/sizeof(cells[0]); i++) {
		st.cells++;
		PrismResult r = cm_txf(cells[i].src, f);
		int wrap = r.output && cm_has_bchk_wrap(r.output);
		if (cells[i].expect < 0) {
			if (!cm_err(&r)) cm_note(&st, "bounds-ptr miss-rej %zu", i);
		} else if (!cm_ok(&r)) {
			cm_note(&st, "bounds-ptr status %zu", i);
		} else if (cells[i].expect && !wrap) {
			cm_note(&st, "bounds-ptr miss-wrap %zu", i);
		} else if (!cells[i].expect && wrap) {
			cm_note(&st, "bounds-ptr false-wrap %zu", i);
		}
		prism_free(&r);
	}
	cm_report("gen/bounds-ptr-arith", &st);
}

static void cm_gen_attr_ctrlflow(void) {
	static const char *must_err[] = {
		"int a __attribute__((aligned(({ goto L; 8; })))); void f(void){ L: ; }",
		"[[gnu::aligned(({ return; 8; }))]] int a; void f(void){}",
		"void f(void){ int x __attribute__((aligned(({ defer (void)0; 8; }))); (void)x; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "attr-ctrl accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/attr-ctrlflow", &st);
}

static void cm_gen_case_orelse(void) {
	static const char *must_err[] = {
		"void f(int c){ switch(c){ case 0 orelse 1: break; } }",
		"void f(int c){ switch(c){ case (0 orelse 1): break; } }",
		"void f(int c){ switch(c){ case 1 + (0 orelse 1): break; } }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "case-orelse accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/case-orelse", &st);
}

static void cm_gen_shadow_after_defer(void) {
	/* SPEC C9: same-block shadow after defer rejects; nested-block shadow OK. */
	static const char *must_err[] = {
		"void use(int*); void f(void){ int x=1; defer use(&x); int x; }",
		"void use(char*); void f(void){ char *p=0; defer use(p); char *p; }",
		"void use(int*); void f(void){ int x=1; defer use(&x); int x, y; (void)y; }",
	};
	static const char *must_ok[] = {
		"void use(int*); void f(void){ int x=1; defer use(&x); { int y; (void)y; } }",
		"void use(int*); void f(void){ { int x=1; defer use(&x); } int x; (void)x; }",
		"void use(int*); void f(void){ int x=1; { defer use(&x); } int x; (void)x; }",
		"void use(int*); void f(void){ int x=1; defer use(&x); { int x; (void)x; } }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "shadow-defer accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "shadow-defer false-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/shadow-after-defer", &st);
}


static void cm_gen_bounds_bypass_dense(void) {
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	/* All must reject under bounds-check (SPEC commutative / ptr-arith). */
	static const char *must_err[] = {
		"int main(void){ int a[4]={0}; return 1[a]; }",
		"int main(void){ int a[4]={0}; return (1)[a]; }",
		"int main(void){ int a[4]={0}; return (+a)[1]; }",
		"int main(void){ int a[4]={0}; return (a+0)[1]; }",
		"int main(void){ int a[4]={0}; return (a+1)[0]; }",
		"int main(void){ int a[4]={0}; return *(a+1); }",
		"int main(void){ int a[4]={0}; return *(a-(-1)); }",
		"int main(void){ int a[4]={0}; return *(&a[0]+1); }",
		"int main(void){ int a[4]={0}; int i=1; return i[a]; }",
		"int main(void){ int a[4]={0}; return (0,1)[a]; }",
		"int main(void){ int a[2][3]={{0}}; return *(a[0]+1); }",
		"int main(void){ int a[4]={0}; return (*&a)[1]; }",
	};
	static const char *must_ok_wrap[] = {
		"int main(void){ int a[4]={0}; return a[1]; }",
		"int main(void){ int a[4]={0}; return (a)[1]; }",
		"int main(void){ int a[4]={0}; return a[(0,1)]; }",
		"int main(void){ int a[4]={0}; return (0,a)[1]; }",
		"int main(void){ int a[2][3]={{0}}; return a[1][0]; }",
		"int main(void){ int a[4]={0}; return a[a[0]]; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_txf(must_err[i], f);
		if (!cm_err(&r)) cm_note(&st, "byp-dense accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok_wrap)/sizeof(must_ok_wrap[0]); i++) {
		st.cells++;
		PrismResult r = cm_txf(must_ok_wrap[i], f);
		if (!cm_ok(&r) || !r.output || !cm_has_bchk_wrap(r.output))
			cm_note(&st, "byp-dense wrap %zu", i);
		prism_free(&r);
	}
	cm_report("gen/bounds-bypass-dense", &st);
}

static void cm_gen_unreach_sites(void) {
	static const char *must_inject[] = {
		"_Noreturn void die(void); void f(void){ die(); }",
		"_Noreturn void die(void); void f(void){ die(); int x; (void)x; }",
		"_Noreturn void die(void); int f(void){ die(); return 0; }",
		"void die(void) __attribute__((noreturn)); void f(void){ die(); }",
		"_Noreturn void die(void); void f(void){ if(1){ die(); } }",
	};
	static const char *must_not[] = {
		"void die(void); void f(void){ die(); }",
		"_Noreturn void die(void); void f(void){ sizeof(die()); }",
		"_Noreturn void die(void); void f(void){ if(0) die(); else (void)0; }",
		"_Noreturn void die(void); void f(void){ (void)(0 ? die() : 0); }",
		"_Noreturn void die(void); void f(void){ raw { die(); } }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_inject)/sizeof(must_inject[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_inject[i]);
		if (!cm_ok(&r) || !r.output || !cm_has_unreach(r.output))
			cm_note(&st, "unreach miss %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_not)/sizeof(must_not[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_not[i]);
		if (!cm_ok(&r)) cm_note(&st, "unreach false-rej %zu", i);
		else if (r.output && cm_has_unreach(r.output))
			cm_note(&st, "unreach false-inj %zu", i);
		prism_free(&r);
	}
	cm_report("gen/unreach-sites", &st);
}

static void cm_gen_autostatic_sites(void) {
	static const char *must_static[] = {
		"void f(void){ const int k[3]={1,2,3}; (void)k; }",
		"void f(void){ const int k[]={1,2,3}; (void)k; }",
		"void f(void){ const char s[]=\"hi\"; (void)s; }",
		"void f(void){ const char s[3]=\"hi\"; (void)s; }",
	};
	static const char *must_not[] = {
		"void f(void){ int k[3]={1,2,3}; (void)k; }",
		"void f(void){ const int k[3]; (void)k; }",
		"void f(void){ volatile const int k[3]={1,2,3}; (void)k; }",
		"void f(void){ raw { const int k[3]={1,2,3}; (void)k; } }",
		"void f(void){ const int *k = (int[]){1,2,3}; (void)k; }",
	};
	/* const VLA needs memset → hard reject (not an auto-static question). */
	static const char *must_err[] = {
		"void f(int n){ const int k[n]; (void)k; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_static)/sizeof(must_static[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_static[i]);
		if (!cm_ok(&r) || !r.output || !strstr(r.output, "static"))
			cm_note(&st, "autostatic miss %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_not)/sizeof(must_not[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_not[i]);
		if (!cm_ok(&r)) cm_note(&st, "autostatic false-rej %zu", i);
		else if (r.output && strstr(r.output, "static const"))
			cm_note(&st, "autostatic false-inj %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "autostatic miss-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/autostatic-sites", &st);
}

static void cm_gen_zeroinit_sites(void) {
	static const char *must_zi[] = {
		"void f(void){ int x; (void)x; }",
		"void f(void){ int x, y; (void)x; (void)y; }",
		"void f(void){ struct { int a; } s; (void)s; }",
		"void f(void){ int a[4]; (void)a; }",
		"void f(void){ union { int a; char b; } u; (void)u; }",
		"void f(void){ L: int x; (void)x; }",
	};
	static const char *must_not_eq[] = {
		"void f(void){ int x = 1; (void)x; }",
		"void f(void){ static int x; (void)x; }",
		"void f(void){ extern int x; (void)x; }",
		"int x; void f(void){ (void)x; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_zi)/sizeof(must_zi[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_zi[i]);
		if (!cm_ok(&r) || !r.output) cm_note(&st, "zi status %zu", i);
		else if (!strstr(r.output, "= 0") && !strstr(r.output, "={0}") &&
			 !strstr(r.output, "= {0}") && !strstr(r.output, "memset"))
			cm_note(&st, "zi miss %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_not_eq)/sizeof(must_not_eq[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_not_eq[i]);
		if (!cm_ok(&r)) cm_note(&st, "zi false-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/zeroinit-sites", &st);
}

static void cm_gen_orelse_expr_ctx(void) {
	/* Type-junk `T orelse …` inside uneval/generic parens must reject (no leak).
	 * Bracket-dim orelse and decl-init orelse remain valid. */
	static const char *must_err[] = {
		"void f(void){ int n = sizeof(0 orelse 1); (void)n; }",
		"void f(void){ int n = sizeof(int orelse 0); (void)n; }",
		"void f(void){ int n = _Alignof(int orelse 0); (void)n; }",
		"void f(void){ int n = alignof(int orelse 0); (void)n; }",
		"void f(void){ typeof(int orelse 0) x; (void)x; }",
		"void f(void){ _Static_assert(int orelse 0, \"\"); }",
		"void f(void){ int n = _Generic(int orelse 0, default: 1); (void)n; }",
		"void f(int *p){ for(;*p orelse 0;) break; }",
		"void f(int *p){ while(*p orelse 0) break; }",
		"void f(int *p){ if(*p orelse 0){} }",
		"void f(int c){ switch(c orelse 0){ default: break; } }",
		"void f(void){ return 0 orelse 1; }",
	};
	static const char *must_ok[] = {
		"void f(void){ int *p = 0 orelse 0; (void)p; }",
		"void f(void){ int *p; p = 0 orelse 0; (void)p; }",
		"void f(void){ int a[4]; int *p = &a[0 orelse 0]; (void)p; }",
		"void f(void){ typeof(int *) p = 0 orelse 0; (void)p; }",
		"void f(void){ int a[0 orelse 1]; (void)a; }",
		"void f(void){ int *p=0; typeof(p orelse (int*)0) x; (void)x; }",
		"typedef int orelse; void f(void){ int n = sizeof(orelse); (void)n; }",
		"void f(void){ int orelse = 1; int n = sizeof(orelse); (void)n; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "orelse-ctx accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "orelse-ctx false-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/orelse-expr-ctx", &st);
}

static void cm_gen_taint_defer(void) {
	static const char *must_err[] = {
		"#include <setjmp.h>\nvoid f(void){ jmp_buf b; setjmp(b); defer (void)0; }",
		"#include <setjmp.h>\nvoid f(void){ defer (void)0; jmp_buf b; setjmp(b); }",
		"#include <setjmp.h>\nvoid f(void){ void *p = setjmp; defer (void)0; (void)p; }",
		"void f(void){ defer (void)0; asm goto(\"\"::::L); L:; }",
		"void f(void){ void *p; defer (void)0; goto *p; }",
	};
	static const char *must_ok[] = {
		"void f(void){ defer (void)0; }",
		"#include <setjmp.h>\nvoid f(void){ jmp_buf b; setjmp(b); }",
		"void g(void); void f(void){ defer g(); }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "taint-defer accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "taint-defer false-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/taint-defer", &st);
}

static void cm_gen_label_defer_loop(void) {
	/* Reject loop-over-defer and skip-over-defer; allow forward goto after defer. */
	static const char *must_err[] = {
		"void f(void){ L: defer (void)0; goto L; }",
		"void f(void){ L: ; defer (void)0; goto L; }",
		"void f(void){ L: defer (void)0; { goto L; } }",
		"void f(void){ L: ; goto L2; defer (void)0; L2: ; }",
		"void f(void){ goto L2; defer (void)0; L2: ; }",
	};
	static const char *must_ok[] = {
		"void f(void){ defer (void)0; goto L; L: ; }",
		"void f(void){ defer (void)0; L: goto L; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "label-defer accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "label-defer false-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/label-defer-loop", &st);
}



static void cm_gen_orelse_type_junk_dense(void) {
	static const char *intros[] = {
		"sizeof", "_Alignof", "alignof", "typeof", "_Generic",
	};
	static const char *inners[] = {
		"int orelse 0",
		"unsigned orelse 0",
		"const int orelse 0",
		"struct S orelse 0",
		"enum E orelse 0",
		"int * orelse 0",
	};
	CmStats st = {0};
	char src[512];
	for (size_t i = 0; i < sizeof(intros)/sizeof(intros[0]); i++) {
		for (size_t j = 0; j < sizeof(inners)/sizeof(inners[0]); j++) {
			if (intros[i][0] == '_' && intros[i][1] == 'G')
				snprintf(src, sizeof(src),
					 "void f(void){ int n = _Generic(%s, default: 1); (void)n; }",
					 inners[j]);
			else if (intros[i][0] == 't')
				snprintf(src, sizeof(src),
					 "void f(void){ %s(%s) x; (void)x; }", intros[i], inners[j]);
			else
				snprintf(src, sizeof(src),
					 "void f(void){ int n = (int)%s(%s); (void)n; }",
					 intros[i], inners[j]);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_err(&r)) cm_note(&st, "type-junk accept %s/%zu", intros[i], j);
			prism_free(&r);
		}
	}
	/* Positive: typedef/var named orelse in uneval */
	static const char *ok[] = {
		"typedef int orelse; void f(void){ int n = sizeof(orelse); (void)n; }",
		"typedef int orelse; void f(void){ int n = _Alignof(orelse); (void)n; }",
		"void f(void){ int orelse=1; int n = sizeof(orelse); (void)n; }",
		"void f(void){ int *p=0; typeof(p orelse 0) x; (void)x; }",
	};
	for (size_t i = 0; i < sizeof(ok)/sizeof(ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "type-junk false-rej %zu", i);
		/* ok[0..2] intentionally retain identifier `orelse` (typedef/var).
		 * ok[3] must lower the operator (no keyword leak). */
		else if (i == 3 && r.output && cm_kw(r.output, "orelse"))
			cm_note(&st, "type-junk leak ok %zu", i);
		prism_free(&r);
	}
	cm_report("gen/orelse-type-junk-dense", &st);
}

static void cm_gen_generic_assoc(void) {
	static const char *must_ok[] = {
		"int f(int x){ return _Generic(x, int: 1, default: 0); }",
		"int f(void){ return _Generic(0, int: 1, default: 0); }",
		"int f(int *p){ return _Generic(p, int *: 1, default: 0); }",
	};
	static const char *must_err[] = {
		"int f(void){ return _Generic(0 orelse 1, default: 0); }",
		"int f(void){ return _Generic(int orelse 0, default: 0); }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "generic false-rej %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "generic accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/generic-assoc", &st);
}

static void cm_gen_comma_orelse_split(void) {
	static const char *must_ok[] = {
		"void f(void){ int *a = 0, *b = 0 orelse 0; (void)a; (void)b; }",
		"void f(void){ int a = 0, b = 1 orelse 2; (void)a; (void)b; }",
		"void f(void){ int *a = 0 orelse 0, *b = 0 orelse 0; (void)a; (void)b; }",
	};
	static const char *must_err[] = {
		"void f(void){ static int *a = 0, *b = 0 orelse 0; (void)a; (void)b; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r) || (r.output && cm_kw(r.output, "orelse")))
			cm_note(&st, "comma-oe status %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "comma-oe accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/comma-orelse-split", &st);
}

static void cm_gen_braceless_ctrl_defer(void) {
	/* Braceless defer under if/while/for/switch has no block scope — reject.
	 * Braced bodies are fine. */
	static const char *must_err[] = {
		"void g(void); void f(void){ if(1) defer g(); }",
		"void g(void); void f(void){ if(1) defer g(); else defer g(); }",
		"void g(void); void f(void){ while(0) defer g(); }",
		"void g(void); void f(void){ for(;;) defer g(); }",
		"void f(void){ if(1) defer int x; }",
		"void f(void){ if(1) defer return; }",
	};
	static const char *must_ok[] = {
		"void g(void); void f(void){ if(1){ defer g(); } }",
		"void g(void); void f(void){ while(0){ defer g(); } }",
		"void g(void); void f(void){ for(;;){ defer g(); break; } }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "braceless-ctrl-defer accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r) || (r.output && cm_kw(r.output, "defer")))
			cm_note(&st, "braceless-ctrl-defer ok %zu", i);
		prism_free(&r);
	}
	cm_report("gen/braceless-ctrl-defer", &st);
}



static void cm_gen_cast_orelse_junk(void) {
	static const char *must_err[] = {
		"void f(void){ int x = (int orelse 0)1; (void)x; }",
		"void f(void){ int x = (int * orelse 0)0; (void)x; }",
		"void f(void){ int x = (sizeof(int orelse 0)); (void)x; }",
	};
	static const char *must_ok[] = {
		"void f(void){ int *p = (int *)0; p = p orelse 0; (void)p; }",
		"void f(void){ int *p = ((int *)0) orelse 0; (void)p; }",
		"void f(void){ int *p = (int *)0 orelse 0; (void)p; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "cast-oe accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r) || (r.output && cm_kw(r.output, "orelse")))
			cm_note(&st, "cast-oe ok %zu", i);
		prism_free(&r);
	}
	cm_report("gen/cast-orelse-junk", &st);
}

static void cm_gen_nested_typeof_orelse(void) {
	static const char *must_ok[] = {
		"void f(void){ int *p=0; typeof(typeof(p orelse (int*)0)) x; (void)x; }",
		"void f(void){ int *p=0; int n = sizeof(typeof(p orelse (int*)0)); (void)n; }",
		"void f(void){ int *p=0; typeof(p orelse p orelse (int*)0) x; (void)x; }",
	};
	static const char *must_err[] = {
		"void f(void){ typeof(typeof(int orelse 0)) x; (void)x; }",
		"void f(void){ int n = sizeof(typeof(int orelse 0)); (void)n; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r) || (r.output && cm_kw(r.output, "orelse")))
			cm_note(&st, "nested-typeof ok %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "nested-typeof accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/nested-typeof-orelse", &st);
}

static void cm_gen_compound_lit_orelse(void) {
	/* Deref/member LHS + compound-literal fallback must reject (double-write /
	 * dangling). Plain `p = … orelse &(T){…}` is intentionally allowed. */
	static const char *must_err[] = {
		"void f(void){ int *p; *p = 0 orelse (int){1}; (void)*p; }",
		"void f(void){ int **pp; *pp = 0 orelse &(int){1}; (void)pp; }",
		"void f(void){ struct S { int x; } *s; s->x = 0 orelse (int){1}; }",
	};
	static const char *must_ok[] = {
		"void f(void){ int *p = 0 orelse (int*)0; (void)p; }",
		"void f(void){ int x; int *p = &x; p = p orelse &x; (void)p; }",
		"void f(void){ int *p; p = 0 orelse { p = 0; }; (void)p; }",
		"void f(void){ int *p; p = &(int){0} orelse 0; (void)p; }",
		"void f(void){ int *p; p = 0 orelse &(int){0}; (void)p; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "cl-oe accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r) || (r.output && cm_kw(r.output, "orelse")))
			cm_note(&st, "cl-oe ok %zu", i);
		prism_free(&r);
	}
	cm_report("gen/compound-lit-orelse", &st);
}

static void cm_gen_designator_orelse(void) {
	static const char *must_ok[] = {
		"void f(void){ int a[4] = { [0 orelse 0] = 1 }; (void)a; }",
		"void f(void){ int a[4] = { [1 orelse 2] = 3, [0] = 4 }; (void)a; }",
	};
	static const char *must_err[] = {
		"void f(void){ int a[4] = { [0] = 1 orelse 2 }; (void)a; }",
		"void f(void){ struct { int x; } s = { .x = 1 orelse 2 }; (void)s; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r) || (r.output && cm_kw(r.output, "orelse")))
			cm_note(&st, "desig-oe ok %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "desig-oe accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/designator-orelse", &st);
}



static void cm_gen_bitint_orelse(void) {
	static const char *must_err[] = {
		"void f(void){ int n = sizeof(_BitInt(8) orelse 0); (void)n; }",
		"void f(void){ typeof(_BitInt(8) orelse 0) x; (void)x; }",
		"void f(void){ int n = _Alignof(_BitInt(16) * orelse 0); (void)n; }",
	};
	static const char *must_ok[] = {
		"void f(void){ _BitInt(8) x = 1; (void)x; }",
		"void f(void){ typeof(_BitInt(8)) x; (void)x; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "bitint-oe accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r)) cm_note(&st, "bitint-oe false-rej %zu", i);
		prism_free(&r);
	}
	cm_report("gen/bitint-orelse", &st);
}

static void cm_gen_atomic_orelse(void) {
	static const char *must_err[] = {
		"void f(void){ int n = sizeof(_Atomic int orelse 0); (void)n; }",
		"void f(void){ typeof(_Atomic int orelse 0) x; (void)x; }",
		"void f(void){ _Atomic int *p = 0; *p = 0 orelse (int){1}; }",
	};
	static const char *must_ok[] = {
		"void f(void){ _Atomic int x; (void)x; }",
		"void f(void){ _Atomic int *p = 0 orelse 0; (void)p; }",
		"void f(void){ _Atomic int x = 0; x = 1 orelse 2; (void)x; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "atomic-oe accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r) || (r.output && cm_kw(r.output, "orelse")))
			cm_note(&st, "atomic-oe ok %zu", i);
		prism_free(&r);
	}
	cm_report("gen/atomic-orelse", &st);
}

static void cm_gen_pp_span_orelse(void) {
	static const char *must_err[] = {
		"void f(void){\n#ifdef X\nint *p = 0\n#else\nint *p = 0\n#endif\n orelse 0; (void)p; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "pp-span accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/pp-span-orelse", &st);
}

static void cm_gen_empty_orelse_action(void) {
	static const char *must_err[] = {
		"void f(void){ int *p = 0 orelse ; (void)p; }",
		"void f(void){ int *p; p = 0 orelse ; }",
		"void f(void){ int *p = 0 orelse , 0; (void)p; }",
		"void f(void){ int *p = 0 orelse 0 orelse ; (void)p; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "empty-oe accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/empty-orelse-action", &st);
}

/* Array-dimension orelse: uneval operands must reject (not drop sizeof);
 * typedef/file-scope dims must not leak; static/extern/TLS dims reject;
 * _Atomic(T[n orelse …]) must lower; dim-level sizeof(T) orelse stays ok. */
static void cm_gen_dim_orelse_uneval(void) {
	static const char *must_err[] = {
		"void f(void){ int a[sizeof(0 orelse 1)]; (void)a; }",
		"void f(void){ int a[sizeof(int orelse 0)]; (void)a; }",
		"void f(void){ int a[_Alignof(int orelse 0)]; (void)a; }",
		"typedef int T[sizeof(int orelse 0)];",
		"typedef int T[0 orelse 1];",
		"int g[sizeof(int orelse 0)];",
		"void f(void){ static int a[0 orelse 1]; (void)a; }",
		"void f(int n){ static int a[n orelse 1]; (void)a; }",
		"void f(void){ extern int a[0 orelse 1]; }",
		"void f(void){ _Thread_local int a[0 orelse 1]; (void)a; }",
		"void f(void){ constexpr int a[0 orelse 1]; (void)a; }",
		"void f(void){ static int (*p)[0 orelse 1]; (void)p; }",
		"void f(void){ static _Atomic(int[0 orelse 1]) *p; (void)p; }",
		"struct S { int a[sizeof(int orelse 0)]; };",
		"void f(void){ int a[sizeof(typeof(int orelse 0))]; (void)a; }",
		"void f(void){ int a[int orelse 1]; (void)a; }",
		"void f(void){ int a[_BitInt(8) orelse 1]; (void)a; }",
		"void f(void){ int a[_Alignas(8) int orelse 1]; (void)a; }",
		"void f(void){ int a[defer 1]; (void)a; }",
		"void f(void){ int x = int orelse 1; (void)x; }",
		"void f(void){ int x = _BitInt(8) orelse 1; (void)x; }",
		"void f(void){ typeof(_Atomic(int) orelse 0) x; (void)x; }",
		"void f(void){ int a[sizeof(_Atomic(int) orelse 0)]; (void)a; }",
		"void f(void){ int x = _Atomic(int) orelse 0; (void)x; }",
		"void f(void){ typeof(sizeof(0 orelse 1)) x; (void)x; }",
		"void f(void){ typeof(typeof(_Atomic(int) orelse 0)) x; (void)x; }",
		"void f(void){ typeof(_Alignof(int orelse 0)) x; (void)x; }",
		/* Typedef / funcptr param dims — must reject (not ternary-lower). */
		"typedef int (*F)(int a[1 orelse 2]);",
		"typedef int F(int a[1 orelse 2]);",
		"typedef int (*F)(int a[sizeof(0 orelse 1)]);",
		"void f(void){ typedef int (*F)(int a[0 orelse 1]); (void)sizeof(F*); }",
		"void f(int (*fp)(int a[0 orelse 1]));",
		"void f(void){ void (*fp)(int a[0 orelse 1]); (void)fp; }",
	};
	static const char *must_ok[] = {
		"void f(void){ int a[sizeof(int) orelse 1]; (void)a; }",
		"void f(int n){ int a[n orelse 1]; (void)a; }",
		"void f(int n){ typedef int T[n orelse 1]; T a; (void)a; }",
		"void f(int n){ typeof(int[n orelse 1]) *p; (void)p; }",
		"void f(int n){ _Atomic(int[n orelse 1]) *p; (void)p; }",
		"void f(void){ _Atomic(int[0 orelse 1]) *p; (void)p; }",
		"void f(int n){ int (*p)[n orelse 1]; (void)p; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_err)/sizeof(must_err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_err[i]);
		if (!cm_err(&r)) cm_note(&st, "dim-oe-uneval accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_ok)/sizeof(must_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_ok[i]);
		if (!cm_ok(&r) || (r.output && cm_kw(r.output, "orelse")))
			cm_note(&st, "dim-oe-uneval ok %zu", i);
		prism_free(&r);
	}
	cm_report("gen/dim-orelse-uneval", &st);
}

/* Param / local name that hides a file-scope array must not wrap against the
 * outer sizeof — array params, pointer params, and K&R. */
static void cm_gen_bounds_param_shadow(void) {
	static const char *no_wrap[] = {
		"int g[10]; int f(int g[20]){return g[5];}",
		"int g[5]={0}; int f(g) int g[10]; {return g[3];}",
		"int g[10]; void f(int *g){ (void)g[3]; }",
		"int g[10]; void f(int g){ (void)sizeof(g); }",
	};
	static const char *must_wrap[] = {
		"int g[10]; void f(void){ (void)g[3]; }",
		"int g[10]; void f(void){ int g[5]; (void)g[3]; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(no_wrap) / sizeof(no_wrap[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(no_wrap[i]);
		if (!cm_ok(&r) || (r.output && cm_has_bchk_wrap(r.output)))
			cm_note(&st, "param-shadow wrap %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_wrap) / sizeof(must_wrap[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_wrap[i]);
		if (!cm_ok(&r) || !r.output || !cm_has_bchk_wrap(r.output))
			cm_note(&st, "param-shadow miss-wrap %zu", i);
		prism_free(&r);
	}
	cm_report("gen/bounds-param-shadow", &st);
}


void run_completeness_open_tests(void) {
	const char *only = getenv("PRISM_SUITE_ONLY");
	int force = getenv("PRISM_COMPLETENESS_OPEN") != NULL;
	int selected = only && strcmp(only, "completeness_open") == 0;
	if (!force && !selected) {
		printf("\n=== COMPLETENESS_OPEN skipped "
		       "(set PRISM_COMPLETENESS_OPEN=1 or PRISM_SUITE_ONLY=completeness_open) ===\n");
		return;
	}
	printf("\n=== COMPLETENESS_OPEN (T1′ generative open) ===\n");
	/* Residual / aspirational cells — red means a product bug to fix. */
	cm_gen_goto_open();
}

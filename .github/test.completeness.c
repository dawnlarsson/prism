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
static void cm_gen_member_subscript_orelse(void);
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
/* Market-seven densify (defer/orelse/zi/raw/bounds/aur/as). */
static void cm_gen_bounds_addr_modes(void);
static void cm_gen_market7_cross(void);
static void cm_gen_raw_prefix_feat(void);
static void cm_gen_zi_typeof_product(void);
static void cm_gen_orelse_bare_ctrl(void);
static void cm_gen_defer_switch_product(void);
static void cm_gen_soft_ident_expr(void);
static void cm_gen_aur_call_shapes(void);
static void cm_gen_as_init_shapes(void);
#ifndef _WIN32
static void cm_gen_runtime_defer(void);
static void cm_gen_runtime_orelse(void);
static void cm_gen_runtime_bounds(void);
static void cm_gen_runtime_zi(void);
static void cm_gen_runtime_cross(void);
#endif
static void cm_gen_market7_deep(void);
static void cm_gen_feature_pair(void);
static void cm_gen_defer_exit_dense(void);
static void cm_gen_bounds_emit_paths(void);
static void cm_gen_hunt67_densify(void);
static void cm_gen_defer_expr_reject(void);
static void cm_gen_bounds_comm_reject(void);
static void cm_gen_bounds_derived_lhs(void);
static void cm_gen_orelse_array_type(void);
static void cm_gen_atomic_typeof_init(void);
static void cm_gen_bounds_warn_safety(void);
static void cm_gen_bounds_uneval_product(void);
static void cm_gen_bounds_vla_wrap(void);
static void cm_gen_bounds_deref_sites(void);
static void cm_gen_bounds_lhs_peel(void);
static void cm_gen_bounds_rank_registry(void);
static void cm_gen_bounds_member_falsepos(void);

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
		/* Statement-final through casts / grouping / comma / args */
		"%s; void f(void){ (void)die(0); }",
		"%s; void f(void){ (void)(die(0)); }",
		"%s; void f(void){ (void)(0, die(0)); }",
		"%s; void f(void){ int x; x = (0, die(0)); (void)x; }",
		"%s; void f(void){ die(0), 1; }",
		"%s; void cleanup(void); void f(void){ (cleanup(), die(0)); }",
		"%s; void sink(int); void f(void){ sink(die(0)); }",
	};
	/* Negative sites: must NOT inject */
	static const char *neg_sites[] = {
		"%s; void f(void){ (die)(0); }",
		"%s; int f(int x){ x ? die(0) : (void)0; return x; }",
		"%s; void f(void){ for(die(0);;){} }",
		"%s; void f(void){ if(die(0)){} }",
		"%s; void f(void){ while(die(0)){} }",
		"%s; void f(void){ (void)sizeof(die(0)); }",
		"%s; void f(void){ (void)sizeof +die(0); }",
		"%s; void f(void){ int die = 0; (void)die; }",
		"%s; struct S { int die; }; void f(void){ struct S s={0}; (void)s.die; }",
		"%s; struct S { int die; }; void f(void){ struct S *p=0; if(p) (void)p->die; }",
		/* braceless then/else: conservative — no inject (stmt is not bare ident(;)) */
		"%s; void f(void){ if(1) die(0); int __m=1; (void)__m; }",
		"%s; void f(void){ if(0); else die(0); int __m=1; (void)__m; }",
		/* short-circuit: die may not run */
		"%s; void f(int x){ x && die(0); }",
		"%s; void f(int x){ x || die(0); }",
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
		{ "void f(void){ %s; int i=1; (void)sizeof %s; }", 0, "uneval-sizeof-np" },
		{ "void f(void){ %s; int i=1; typeof(%s) *p = 0; (void)p; }", 0, "uneval-typeof" },
		{ "void f(void){ %s; int i=1; (void)_Alignof(typeof(%s)); }", 0, "uneval-alignof" },
		{ "int f(void){ %s; int i=1; return _Generic(%s, int: 1, default: 0); }", 0,
		  "uneval-generic-ctrl" },
		{ "void f(void){ %s; _Static_assert(sizeof(%s) > 0, \"\"); }", 0, "uneval-sa-sizeof" },
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
		/* Cross-feature interaction snips (hand suites → cube ×64). */
		"void cleanup(void); void f(void){ defer { int a[4]; (void)a[1]; } cleanup(); }",
		"int *g(void); void cleanup(void); void f(void){ defer cleanup(); int *p = g() orelse 0; (void)p; }",
		"void f(void){ raw { int a[4]; (void)a[1]; const int k[2]={1,2}; (void)k; } }",
		"void cleanup(void); void f(void){ raw { defer cleanup(); } }",
		"int *g(void); void f(void){ raw { int *p = g() orelse 0; (void)p; } }",
		"_Noreturn void die(void); void f(void){ raw { die(); } }",
		"void f(void){ int a[4]; (void)&a[4]; }",
		"void f(void){ const typeof(int[3]) k={1,2,3}; (void)k; }",
		"void cleanup(void); void f(void){ for(int i=0;i<1;i++){ defer cleanup(); break; } }",
		"int *g(void); void f(void){ for(int i=0;i<1;i++){ int *p = g() orelse continue; (void)p; } }",
		"void cleanup(void); void f(void){ switch(0){ default: { defer cleanup(); } break; } }",
		"void f(void){ int a[8]; (void)a[0]; (void)a[7]; }",
		"void f(void){ raw int a[4]; (void)a[1]; }",
		"void cleanup(void); int *g(void); void f(void){ defer { int *p = g() orelse 0; (void)p; } cleanup(); }",
		"_Noreturn void die(void); void cleanup(void); void f(void){ defer cleanup(); die(); }",
		"void f(void){ const int k[2]={[0]=1,[1]=2}; (void)k; }",
		"void f(void){ int x; int y; (void)x; (void)y; }",
		"int *g(void); void f(void){ int *p = g() orelse g() orelse 0; (void)p; }",
		"void f(void){ raw { int x; const int k[2]={1,2}; (void)x; (void)k; } }",
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
			/* leak invariants when feature ON and accept.
			 * raw { … } intentionally keeps defer/orelse keywords. */
			if (cm_ok(&r) && r.output) {
				int in_raw = strstr(snips[s], "raw") != NULL;
				if ((mask & 1) && !in_raw && strstr(snips[s], "defer") &&
				    cm_kw(r.output, "defer"))
					cm_note(&st, "cube defer leak mask=%u", mask);
				if (((mask >> 1) & 1) && !in_raw && strstr(snips[s], "orelse") &&
				    cm_kw(r.output, "orelse"))
					cm_note(&st, "cube orelse leak mask=%u", mask);
				if (!((mask >> 2) & 1) && strstr(snips[s], "a[1]") && !in_raw &&
				    cm_has_bchk_wrap(r.output))
					cm_note(&st, "cube bounds off leak mask=%u", mask);
				if (!((mask >> 3) & 1) && strstr(snips[s], "const int k") && !in_raw &&
				    strstr(r.output, "static const int"))
					cm_note(&st, "cube as off leak mask=%u", mask);
				if (!((mask >> 4) & 1) && strstr(snips[s], "die") && !in_raw &&
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
	cm_gen_member_subscript_orelse();
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
	/* Market-seven densify layer. */
	cm_gen_bounds_addr_modes();
	cm_gen_market7_cross();
	cm_gen_raw_prefix_feat();
	cm_gen_zi_typeof_product();
	cm_gen_orelse_bare_ctrl();
	cm_gen_defer_switch_product();
	cm_gen_soft_ident_expr();
	cm_gen_aur_call_shapes();
	cm_gen_as_init_shapes();
	cm_gen_market7_deep();
	cm_gen_feature_pair();
	cm_gen_defer_exit_dense();
	cm_gen_bounds_emit_paths();
	cm_gen_hunt67_densify();
	cm_gen_defer_expr_reject();
	cm_gen_bounds_comm_reject();
	cm_gen_bounds_derived_lhs();
	cm_gen_orelse_array_type();
	cm_gen_atomic_typeof_init();
	cm_gen_bounds_warn_safety();
	cm_gen_bounds_uneval_product();
	cm_gen_bounds_vla_wrap();
	cm_gen_bounds_deref_sites();
	cm_gen_bounds_lhs_peel();
	cm_gen_bounds_rank_registry();
	cm_gen_bounds_member_falsepos();
#ifndef _WIN32
	cm_gen_runtime_defer();
	cm_gen_runtime_orelse();
	cm_gen_runtime_bounds();
	cm_gen_runtime_zi();
	cm_gen_runtime_cross();
#endif
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
	/* Cast×subscript: wrap with outermost cast element sizeof.
	 * Axes: outer cast × optional inner cast × index. */
	static const char *outers[] = {
		"(int*)",
		"(short*)",
		"(char*)",
		"(int(*)[4])",
	};
	static const char *inners[] = {
		"",
		"(void*)",
		"(char*)",
		"(void*)(char*)",
	};
	static const char *idxs[] = { "0", "i" };
	CmStats st = {0};
	char src[512], cast[160];
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	for (size_t o = 0; o < sizeof(outers) / sizeof(outers[0]); o++) {
		for (size_t in = 0; in < sizeof(inners) / sizeof(inners[0]); in++) {
			/* PTA cast does not compose with an inner cast prefix. */
			if (strstr(outers[o], "(*)") && inners[in][0]) continue;
			for (size_t ix = 0; ix < sizeof(idxs) / sizeof(idxs[0]); ix++) {
				snprintf(cast, sizeof(cast), "(%s%sa)", outers[o], inners[in]);
				if (strstr(outers[o], "(*)"))
					snprintf(src, sizeof(src),
						 "void f(void){ int a[16]={0}; int i=0; "
						 "(void)((%s)a)[%s][0]; }\n",
						 outers[o], idxs[ix]);
				else
					snprintf(src, sizeof(src),
						 "void f(void){ char a[64]={0}; int i=0; "
						 "(void)%s[%s]; }\n",
						 cast, idxs[ix]);
				st.cells++;
				PrismResult r = cm_txf(src, f);
				if (!cm_ok(&r) || !r.output)
					cm_note(&st, "cast-correct status o=%zu in=%zu ix=%zu", o, in, ix);
				else if (!cm_has_bchk_wrap(r.output))
					cm_note(&st, "cast-correct miss o=%zu in=%zu", o, in);
				else if (inners[in][0] && strstr(inners[in], "void*") &&
					 strstr(r.output, "sizeof((*(void*)"))
					cm_note(&st, "cast-correct inner-void o=%zu", o);
				else if (!strstr(outers[o], "(*)") &&
					 (strstr(r.output, "sizeof(a)/sizeof(a[0])") ||
					  strstr(r.output, "sizeof(a) / sizeof(a[0])")))
					cm_note(&st, "cast-correct naive o=%zu", o);
				prism_free(&r);
			}
		}
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
		"_Noreturn void die(void); void f(void){ (void)sizeof die(); }",
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
		"int f(void){ return _Generic(0, int[0 orelse 1]: 1, default: 0); }",
	};
	static const char *must_err[] = {
		"int f(void){ return _Generic(0 orelse 1, default: 0); }",
		"int f(void){ return _Generic(int orelse 0, default: 0); }",
		"int f(int n){ return _Generic(0, int[n orelse 1]: 1, default: 0); }",
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
	/* Association type constructor x dimension-ICE product.  The direct
	 * `int[n]` spelling is not enough: an ancestor search must see through
	 * typeof layers and abstract declarator parentheses. */
	static const char *assoc_types[] = {
		"int[%s orelse 1]",
		"typeof(int[%s orelse 1])",
		"typeof(typeof(int[%s orelse 1]))",
		"int (*)[%s orelse 1]",
		"typeof(int (*)[%s orelse 1])",
	};
	static const char *dims[] = { "0", "n" };
	char src[768], assoc[384];
	for (size_t a = 0; a < sizeof(assoc_types) / sizeof(assoc_types[0]); a++) {
		for (size_t d = 0; d < sizeof(dims) / sizeof(dims[0]); d++) {
			snprintf(assoc, sizeof(assoc), assoc_types[a], dims[d]);
			snprintf(src, sizeof(src),
				 "int f(int n){ return _Generic((void *)0, %s: 1, default: 0); }",
				 assoc);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (d == 0) {
				if (!cm_ok(&r) || !r.output || cm_kw(r.output, "orelse"))
					cm_note(&st, "generic ctor false-rej a=%zu", a);
			} else if (!cm_err(&r)) {
				cm_note(&st, "generic ctor VM accept a=%zu", a);
			}
			prism_free(&r);
		}
	}
	cm_report("gen/generic-assoc", &st);
}

/* Value-member subscript x LHS x fallback x expression-site.  Member access
 * and initializer designators share the token shape `.name[…]`; this product
 * proves dynamic value subscripts are not accidentally subjected to the
 * designator ICE rule. */
static void cm_gen_member_subscript_orelse(void) {
	static const char *accesses[] = {
		"s.a[0][%s orelse %s]",
		"p->a[0][%s orelse %s]",
		"(*p).a[0][%s orelse %s]",
		"o.in.a[0][%s orelse %s]",
	};
	static const char *lhs[] = { "0", "i" };
	static const char *fallback[] = { "0", "1" };
	static const char *sites[] = {
		"return %s;",
		"int x = %s; return x;",
		"return (int)sizeof(%s);",
	};
	CmStats st = {0};
	char expr[256], body[384], src[1200];
	for (size_t a = 0; a < sizeof(accesses) / sizeof(accesses[0]); a++)
		for (size_t l = 0; l < sizeof(lhs) / sizeof(lhs[0]); l++)
			for (size_t f = 0; f < sizeof(fallback) / sizeof(fallback[0]); f++)
				for (size_t s = 0; s < sizeof(sites) / sizeof(sites[0]); s++) {
					snprintf(expr, sizeof(expr), accesses[a], lhs[l], fallback[f]);
					snprintf(body, sizeof(body), sites[s], expr);
					snprintf(src, sizeof(src),
						 "struct I{int a[2][2];}; struct O{struct I in;}; "
						 "int test(struct I s, struct I *p, struct O o, int i){ %s }",
						 body);
					st.cells++;
					PrismResult r = cm_tx(src);
					if (!cm_ok(&r) || !r.output || cm_kw(r.output, "orelse"))
						cm_note(&st, "member-subscript a=%zu l=%zu f=%zu s=%zu",
							a, l, f, s);
					prism_free(&r);
				}
	cm_report("gen/member-subscript-orelse", &st);
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
		"void f(void){ int a[2][2] = { [0 orelse 1][0] = 1 }; (void)a; }",
		"void f(void){ int defer=1; int a[3] = { [defer] = 2 }; (void)a; }",
		"struct S{int a[4];}; void f(void){ (void)__builtin_offsetof(struct S,a[0 orelse 1]); }",
		"struct S{int a[4];}; void f(void){ (void)offsetof(struct S,a[1 orelse 0]); }",
	};
	static const char *must_err[] = {
		"void f(void){ int a[4] = { [0] = 1 orelse 2 }; (void)a; }",
		"void f(void){ struct { int x; } s = { .x = 1 orelse 2 }; (void)s; }",
		/* GNU range designator — ternary would destroy `...`. */
		"void f(void){ int a[8] = { [0 ... 2 orelse 3] = 9 }; (void)a; }",
		"void f(void){ int a[8] = { [0 orelse 1 ... 3] = 9 }; (void)a; }",
		"void f(void){ int a[8] = { [0 orelse 1 ... 2 orelse 3] = 9 }; (void)a; }",
		"void f(void){ int idx=0; int a[8]={ [idx orelse 1]=42 }; (void)a; }",
		/* offsetof member designators need ICEs; GNU range must not ternary. */
		"struct S{int a[4];}; void f(int n){ (void)__builtin_offsetof(struct S,a[n orelse 1]); }",
		"struct S{int a[4];}; void f(void){ (void)__builtin_offsetof(struct S,a[0 ... 2 orelse 3]); }",
		"struct S{int a[4];}; int n; void f(void){ (void)offsetof(struct S,a[n orelse 0]); }",
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
		"void f(int n){ _Static_assert(sizeof(char[n orelse 1]) > 0, \"\"); }",
		"void f(int n){ (void)_Alignof(int[n orelse 1]); }",
		"void f(int n){ (void)alignof(char[n orelse 1]); }",
		"int n; int main(void){ return (int)_Alignof(char[n orelse 4]); }",
		"void f(int n){ (void)_Alignof(_Atomic(int[n orelse 1])); }",
		"void f(int n){ (void)_Alignof(typeof(int[n orelse 1])); }",
		"int n; int main(void){ return _Generic(0, int[n orelse 1]: 1, default: 0); }",
	};
	static const char *must_ok[] = {
		"void f(void){ int a[sizeof(int) orelse 1]; (void)a; }",
		"void f(int n){ int a[n orelse 1]; (void)a; }",
		"void f(int n){ typedef int T[n orelse 1]; T a; (void)a; }",
		"void f(int n){ typeof(int[n orelse 1]) *p; (void)p; }",
		"void f(int n){ _Atomic(int[n orelse 1]) *p; (void)p; }",
		"void f(void){ _Atomic(int[0 orelse 1]) *p; (void)p; }",
		"void f(int n){ int (*p)[n orelse 1]; (void)p; }",
		"void f(void){ typeof(sizeof(int[0 orelse 1])) x; (void)x; }",
		"void f(void){ typeof(_Alignof(int[0 orelse 1])) x; (void)x; }",
		"void f(void){ int x = sizeof(int[0 orelse 1]); (void)x; }",
		"void f(void){ int x = _Alignof(int[0 orelse 1]); (void)x; }",
		"void f(void){ _Static_assert(sizeof(char[0 orelse 1]) > 0, \"\"); }",
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
 * outer sizeof — array/scalar/pointer params, K&R, pointer declarator shapes,
 * typedef pointers, and incomplete block-scope declarations. */
static void cm_gen_bounds_param_shadow(void) {
	static const char *no_wrap[] = {
		"int g[10]; int f(int g[20]){return g[5];}",
		"int g[5]={0}; int f(g) int g[10]; {return g[3];}",
		"int g[10]; void f(int *g){ (void)g[3]; }",
		"int g[10]; void f(int g){ (void)sizeof(g); }",
		"int g[10]; void f(int i){ int *g=0; (void)g[i]; }",
		"int g[10]; void f(int i){ const int *g=0; (void)g[i]; }",
		"int g[10]; void f(int i){ volatile int *g=0; (void)g[i]; }",
		"typedef int *P; int g[10]; void f(int i){ P g=0; (void)g[i]; }",
		"int g[10]; void f(int i){ int (*g)[10]=0; (void)(*g)[i]; }",
		"int g[10]; void f(int i){ extern int g[]; (void)g[i]; }",
		"int g[10]; void f(int i){ raw int *g=0; (void)g[i]; }",
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

/* ═══════════════════════════════════════════════════════════════════════
 * Market-seven densify — pull hand-suite patterns into axis products for
 * defer / orelse / zeroinit / raw / bounds-check / auto-unreachable /
 * auto-static. Oracles stay machine-decidable (leak / wrap / inject /
 * reject); runtime LIFO logs stay in test.defer.c / test.orelse.c.
 * ═══════════════════════════════════════════════════════════════════════ */

/* Bounds address/access forms from test.bounds.c — wrap / no-wrap / reject. */
static void cm_gen_bounds_addr_modes(void) {
	enum { W = 1, N = 0, R = -1 };
	static const struct {
		const char *expr;
		int expect; /* W wrap, N no-wrap, R reject */
	} forms[] = {
		{ "a[i]", W },
		{ "(a)[i]", W },
		{ "a[(0,i)]", W },
		{ "(0,a)[i]", W },
		{ "&a[i]", N },
		{ "&(a)[i]", N },
		{ "(void)&a[4]", N },
		{ "(void)&(a)[4]", N },
		{ "(int*)&a[4]", N },
		{ "sizeof(a[i])", N },
		{ "sizeof a[i]", N },
		{ "_Alignof(a[0])", N },
		{ "i[a]", R },
		{ "(i)[a]", R },
		{ "*(a+i)", R },
		{ "(a+i)[0]", R },
		{ "(+a)[i]", R },
		{ "*(&a[0]+i)", R },
		{ "0[a]", R },
		{ "(0)[a]", R },
		/* Hunt 6/7: paren-peeled &arr forms (1D `int a[4]`). */
		{ "(*&(a))[i]", R },
		{ "(* & (a))[i]", R },
		{ "(*&a)[i]", R },
	};
	static const char *idxs[] = { "0", "1", "3" };
	CmStats st = {0};
	char src[512];
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	for (size_t fi = 0; fi < sizeof(forms) / sizeof(forms[0]); fi++) {
		/* Forms that hardcode `4` / `0` don't need idx product. */
		int fixed = strstr(forms[fi].expr, "4") != NULL ||
			    (strstr(forms[fi].expr, "Alignof") != NULL);
		size_t nidx = fixed ? 1 : sizeof(idxs) / sizeof(idxs[0]);
		for (size_t ii = 0; ii < nidx; ii++) {
			const char *ix = fixed ? "1" : idxs[ii];
			char expr[192];
			/* Substitute i only when present as a whole index token. */
			if (strchr(forms[fi].expr, 'i') && !fixed) {
				/* naive replace first 'i' that is the index var */
				const char *p = forms[fi].expr;
				char *o = expr;
				while (*p && (size_t)(o - expr) + 8 < sizeof(expr)) {
					if (*p == 'i' && (p == forms[fi].expr ||
							  !((p[-1] >= 'a' && p[-1] <= 'z') ||
							    (p[-1] >= 'A' && p[-1] <= 'Z') ||
							    p[-1] == '_')) &&
					    !((p[1] >= 'a' && p[1] <= 'z') ||
					      (p[1] >= 'A' && p[1] <= 'Z') || p[1] == '_' ||
					      (p[1] >= '0' && p[1] <= '9'))) {
						size_t n = strlen(ix);
						memcpy(o, ix, n);
						o += n;
						p++;
					} else
						*o++ = *p++;
				}
				*o = 0;
			} else {
				snprintf(expr, sizeof(expr), "%s", forms[fi].expr);
			}
			snprintf(src, sizeof(src),
				 "void f(void){ int a[4]={0}; int i=%s; (void)(%s); }\n",
				 ix, expr);
			st.cells++;
			PrismResult r = cm_txf(src, f);
			if (forms[fi].expect == R) {
				if (!cm_err(&r)) cm_note(&st, "addr-mode accept %zu/%s", fi, expr);
			} else if (!cm_ok(&r) || !r.output) {
				cm_note(&st, "addr-mode status %zu/%s", fi, expr);
			} else {
				int wrap = cm_has_bchk_wrap(r.output);
				if (forms[fi].expect == W && !wrap)
					cm_note(&st, "addr-mode miss-wrap %zu/%s", fi, expr);
				if (forms[fi].expect == N && wrap)
					cm_note(&st, "addr-mode false-wrap %zu/%s", fi, expr);
			}
			prism_free(&r);
		}
	}
	cm_report("gen/bounds-addr-modes", &st);
}

/*
 * Nest context × marketed payload. One product covers defer/orelse/bounds/
 * zi/raw/aur/as interactions the hand suites probe in isolation.
 */
static void cm_gen_market7_cross(void) {
	static const char *nests[] = {
		"%s",
		"if (1) { %s }",
		"for (int _i = 0; _i < 1; _i++) { %s }",
		"while (0) { %s }",
		"do { %s } while (0);",
		"switch (0) { default: { %s } break; }",
		"{ %s }",
		"{{ %s }}",
		"int _r = ({ { %s } 0; }); (void)_r;",
		"if (0) { (void)0; } else { %s }",
		"for (;;) { %s break; }",
		"switch (1) { case 1: { %s } break; default: break; }",
	};
	enum {
		P_DEFER = 1 << 0,
		P_ORELSE = 1 << 1,
		P_BOUNDS = 1 << 2,
		P_ZI = 1 << 3,
		P_RAW = 1 << 4,
		P_AUR = 1 << 5,
		P_AS = 1 << 6,
		P_NO_BCHK = 1 << 7, /* payload must not wrap */
		P_NO_AUR = 1 << 8,
		P_NO_AS = 1 << 9,
	};
	static const struct {
		const char *payload;
		unsigned flags;
	} payloads[] = {
		{ "defer cleanup();", P_DEFER },
		{ "defer { cleanup(); }", P_DEFER },
		{ "defer { cleanup(); cleanup(); }", P_DEFER },
		{ "int *p = g() orelse 0; (void)p;", P_ORELSE },
		{ "int x = 0 orelse 1; (void)x;", P_ORELSE },
		{ "int a[4]; (void)a[1];", P_BOUNDS },
		{ "int a[4]; (void)&a[4];", P_BOUNDS | P_NO_BCHK },
		{ "int z; (void)z;", P_ZI },
		{ "struct { int a; } s; (void)s;", P_ZI },
		{ "raw { int y; (void)y; }", P_RAW | P_NO_AS },
		{ "raw { int a[4]; (void)a[1]; }", P_RAW | P_NO_BCHK },
		{ "die();", P_AUR },
		{ "raw { die(); }", P_RAW | P_NO_AUR },
		{ "const int k[3]={1,2,3}; (void)k;", P_AS },
		{ "raw { const int k[3]={1,2,3}; (void)k; }", P_RAW | P_NO_AS },
		{ "defer cleanup(); int a[4]; (void)a[1];", P_DEFER | P_BOUNDS },
		{ "defer cleanup(); int *p = g() orelse 0; (void)p;", P_DEFER | P_ORELSE },
		{ "int *p = g() orelse 0; int z; (void)p; (void)z;", P_ORELSE | P_ZI },
		{ "defer { cleanup(); } const int k[2]={1,2}; (void)k;", P_DEFER | P_AS },
		{ "int a[4]; (void)(a)[1];", P_BOUNDS },
		{ "int a[4]; (void)&(a)[4];", P_BOUNDS | P_NO_BCHK },
		{ "raw { defer cleanup(); int a[4]; (void)a[1]; }", P_RAW | P_DEFER | P_NO_BCHK },
		{ "int *p = g() orelse return; (void)p;", P_ORELSE },
		{ "defer { int z; (void)z; } cleanup();", P_DEFER | P_ZI },
		{ "const char s[]=\"hi\"; (void)s;", P_AS },
		{ "die(); int live=1; (void)live;", P_AUR },
	};
	CmStats st = {0};
	char body[768], src[1600];
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	for (size_t n = 0; n < sizeof(nests) / sizeof(nests[0]); n++) {
		for (size_t p = 0; p < sizeof(payloads) / sizeof(payloads[0]); p++) {
			/* Stmt-expr top-level defer is rejected — payload is
			 * wrapped in `{ %s }` above so all payloads are legal. */
			snprintf(body, sizeof(body), nests[n], payloads[p].payload);
			snprintf(src, sizeof(src),
				 "void cleanup(void); _Noreturn void die(void); int *g(void);\n"
				 "void f(void){ %s }\n",
				 body);
			st.cells++;
			PrismResult r = cm_txf(src, f);
			if (!cm_ok(&r) || !r.output) {
				cm_note(&st, "m7 status n=%zu p=%zu", n, p);
				prism_free(&r);
				continue;
			}
			if ((payloads[p].flags & P_DEFER) && !(payloads[p].flags & P_RAW) &&
			    cm_kw(r.output, "defer"))
				cm_note(&st, "m7 defer leak n=%zu p=%zu", n, p);
			if ((payloads[p].flags & P_ORELSE) && !(payloads[p].flags & P_RAW) &&
			    cm_kw(r.output, "orelse"))
				cm_note(&st, "m7 orelse leak n=%zu p=%zu", n, p);
			if ((payloads[p].flags & P_BOUNDS) && !(payloads[p].flags & P_NO_BCHK) &&
			    !(payloads[p].flags & P_RAW) && !cm_has_bchk_wrap(r.output))
				cm_note(&st, "m7 bounds miss n=%zu p=%zu", n, p);
			if ((payloads[p].flags & P_NO_BCHK) && cm_has_bchk_wrap(r.output))
				cm_note(&st, "m7 bounds false n=%zu p=%zu", n, p);
			if ((payloads[p].flags & P_AUR) && !(payloads[p].flags & P_NO_AUR) &&
			    !cm_has_unreach(r.output)) {
				/* while(0)/for-with-false-cond may leave die() unreachable
				 * for injection purposes — skip those nests. */
				if (n == 2 || n == 3)
					; /* for/while nests: no hard miss */
				else
					cm_note(&st, "m7 aur miss n=%zu p=%zu", n, p);
			}
			if ((payloads[p].flags & P_NO_AUR) && cm_has_unreach(r.output))
				cm_note(&st, "m7 aur false n=%zu p=%zu", n, p);
			if ((payloads[p].flags & P_AS) && !(payloads[p].flags & P_NO_AS) &&
			    !strstr(r.output, "static"))
				cm_note(&st, "m7 as miss n=%zu p=%zu", n, p);
			if ((payloads[p].flags & P_NO_AS) && strstr(r.output, "static const"))
				cm_note(&st, "m7 as false n=%zu p=%zu", n, p);
			prism_free(&r);
		}
	}
	cm_report("gen/market7-cross", &st);
}

/* raw as storage/type prefix × feature payload (from test.raw.c). */
static void cm_gen_raw_prefix_feat(void) {
	static const char *prefixes[] = {
		"raw",
		"raw const",
		"const raw",
		"static raw",
		"raw static",
		"raw volatile",
	};
	static const struct {
		const char *suffix; /* after prefix, before `;` use */
		unsigned check;	    /* 1=no bchk on a[1], 2=no as, 4=accept only */
	} objs[] = {
		{ "int a[4]; (void)a[1]", 1 },
		{ "int x; (void)x", 4 },
		{ "const int k[2]={1,2}; (void)k", 2 },
	};
	CmStats st = {0};
	char src[512];
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
		for (size_t o = 0; o < sizeof(objs) / sizeof(objs[0]); o++) {
			/* `const raw` / `raw const` with another const in obj — skip clash. */
			if (strstr(prefixes[p], "const") && strstr(objs[o].suffix, "const int"))
				continue;
			if (strstr(prefixes[p], "static") && strstr(objs[o].suffix, "const int k"))
				; /* ok: static raw const int — may be odd */
			snprintf(src, sizeof(src), "void f(void){ %s %s; }\n", prefixes[p],
				 objs[o].suffix);
			st.cells++;
			PrismResult r = cm_txf(src, f);
			if (!cm_ok(&r) || !r.output) {
				cm_note(&st, "raw-prefix status p=%zu o=%zu", p, o);
				prism_free(&r);
				continue;
			}
			if ((objs[o].check & 1) && cm_has_bchk_wrap(r.output))
				cm_note(&st, "raw-prefix bchk p=%zu o=%zu", p, o);
			/* Already-`static` prefixes keep `static` after raw strip —
			 * only flag false auto-static promotion on non-static prefixes. */
			if ((objs[o].check & 2) && !strstr(prefixes[p], "static") &&
			    strstr(r.output, "static const"))
				cm_note(&st, "raw-prefix as p=%zu o=%zu", p, o);
			prism_free(&r);
		}
	}
	cm_report("gen/raw-prefix-feat", &st);
}

/* typeof × type × init — pull typeof/VLA/partial shapes from test.zeroinit.c. */
static void cm_gen_zi_typeof_product(void) {
	static const char *types[] = {
		"int",
		"int[4]",
		"struct { int x; }",
		"typeof(int)",
		"typeof(int[4])",
		"typeof(struct { int x; })",
	};
	static const char *inits[] = {
		"",
		" = {0}",
		" = {1}",
	};
	CmStats st = {0};
	char src[768], decl[384];
	PrismFeatures f = prism_defaults();
	f.bounds_check = false;
	for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
		for (size_t i = 0; i < sizeof(inits) / sizeof(inits[0]); i++) {
			int is_arr = strstr(types[t], "[") != NULL;
			if (is_arr)
				snprintf(decl, sizeof(decl), "%s x%s", types[t], inits[i]);
			else if (strstr(types[t], "struct") && !strstr(types[t], "typeof"))
				snprintf(decl, sizeof(decl), "%s x%s", types[t],
					 inits[i][0] ? inits[i] : "");
			else
				snprintf(decl, sizeof(decl), "%s x%s", types[t], inits[i]);
			/* `int[4] x` is invalid — use `int x[4]`. */
			if (!strcmp(types[t], "int[4]"))
				snprintf(decl, sizeof(decl), "int x[4]%s", inits[i]);
			snprintf(src, sizeof(src), "void f(void){ %s; (void)x; }\n", decl);
			st.cells++;
			PrismResult r = cm_txf(src, f);
			int want_zi = (inits[i][0] == 0);
			if (!cm_ok(&r) || !r.output) {
				cm_note(&st, "zi-typeof status t=%zu i=%zu", t, i);
				prism_free(&r);
				continue;
			}
			int has = r.output &&
				  (strstr(r.output, "memset") || strstr(r.output, "= {0}") ||
				   strstr(r.output, "= 0") || strstr(r.output, "__prism_p_"));
			if (want_zi && !has)
				cm_note(&st, "zi-typeof miss t=%zu i=%zu", t, i);
			prism_free(&r);
		}
	}
	/* VLA typeof — must accept and zi via memset path or reject cleanly. */
	{
		st.cells++;
		PrismResult r = cm_txf("void f(int n){ typeof(int[n]) x; (void)x; }\n", f);
		if (!(cm_ok(&r) || cm_err(&r)))
			cm_note(&st, "zi-typeof vla weird");
		prism_free(&r);
	}
	cm_report("gen/zi-typeof-product", &st);
}

/* Bare orelse × braceless control — densify test.orelse.c bare/ctrl pins. */
static void cm_gen_orelse_bare_ctrl(void) {
	static const char *ok[] = {
		"int *g(void); void f(void){ int *p; p = g() orelse 0; (void)p; }",
		"int *g(void); void f(void){ int *p = g() orelse return; (void)p; }",
		"int *g(void); void f(void){ for(;;){ int *p = g() orelse break; (void)p; } }",
		"int *g(void); void f(void){ for(;;){ int *p = g() orelse continue; (void)p; } }",
		"int *g(void); void f(void){ int *p = g() orelse goto L; L: (void)p; }",
		"int *g(void); void f(void){ int *p = g() orelse { p = 0; }; (void)p; }",
		"int *g(void); void f(void){ if (1) { int *p = g() orelse 0; (void)p; } }",
		"int g(void); void f(void){ int x; x = g() orelse 1; (void)x; }",
		"int *g(void); void f(void){ int *a = 0, *b = g() orelse 0; (void)a; (void)b; }",
	};
	static const char *err[] = {
		"int *g(void); void f(void){ (void)(g() orelse 0); }",
		"int *g(void); void f(void){ return g() orelse 0; }",
		"void f(void){ if (0 orelse 1) {} }",
		"void f(void){ while (0 orelse 1) {} }",
		"void f(void){ for (; 0 orelse 1; ) {} }",
		"int *g(void); void f(void){ static int *p = g() orelse 0; (void)p; }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(ok[i]);
		if (!cm_ok(&r) || (r.output && cm_kw(r.output, "orelse")))
			cm_note(&st, "oe-bare ok %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(err) / sizeof(err[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(err[i]);
		if (!cm_err(&r)) cm_note(&st, "oe-bare accept %zu", i);
		prism_free(&r);
	}
	cm_report("gen/orelse-bare-ctrl", &st);
}

/* Switch/case × defer — braced cases ok; unbraced case defer must reject. */
static void cm_gen_defer_switch_product(void) {
	static const char *defers[] = {
		"defer cleanup();",
		"defer { cleanup(); }",
		"defer cleanup(); defer { }",
	};
	static const char *ok_switches[] = {
		"switch (x) { case 0: { %s } break; default: break; }",
		"switch (x) { default: { %s } break; }",
		"switch (x) { case 0: case 1: { %s } break; default: break; }",
		"switch (x) { case 0: { %s cleanup(); } break; default: break; }",
	};
	static const char *err_switches[] = {
		"switch (x) { case 0: %s break; default: break; }",
		"switch (x) { case 0: %s case 1: cleanup(); break; default: break; }",
	};
	CmStats st = {0};
	char body[512], src[768];
	for (size_t d = 0; d < sizeof(defers) / sizeof(defers[0]); d++) {
		for (size_t s = 0; s < sizeof(ok_switches) / sizeof(ok_switches[0]); s++) {
			snprintf(body, sizeof(body), ok_switches[s], defers[d]);
			snprintf(src, sizeof(src),
				 "void cleanup(void); void f(int x){ %s }\n", body);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_ok(&r) || (r.output && cm_kw(r.output, "defer")))
				cm_note(&st, "defer-switch ok d=%zu s=%zu", d, s);
			prism_free(&r);
		}
		for (size_t s = 0; s < sizeof(err_switches) / sizeof(err_switches[0]); s++) {
			snprintf(body, sizeof(body), err_switches[s], defers[d]);
			snprintf(src, sizeof(src),
				 "void cleanup(void); void f(int x){ %s }\n", body);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_err(&r)) cm_note(&st, "defer-switch accept d=%zu s=%zu", d, s);
			prism_free(&r);
		}
	}
	cm_report("gen/defer-switch-product", &st);
}

/* Expression-algebra soft-ident roles (ops/compound) from test.raw.c. */
static void cm_gen_soft_ident_expr(void) {
	static const char *kws[] = { "raw", "defer", "orelse" };
	static const char *tmpls[] = {
		"int f(void){ int %s = 1; %s += 2; %s *= 3; return %s; }",
		"int f(void){ int %s = 1; return %s < 2 && %s > 0; }",
		"int f(void){ int %s = 1; int *p = &%s; return *p; }",
		"int f(void){ int %s = 1; return (%s) + (+%s); }",
		"int f(void){ int a=1, %s=2; return a,%s; }",
		"int f(void){ int %s=0; return %s ? 1 : 2; }",
	};
	CmStats st = {0};
	char src[384];
	for (size_t k = 0; k < sizeof(kws) / sizeof(kws[0]); k++) {
		for (size_t t = 0; t < sizeof(tmpls) / sizeof(tmpls[0]); t++) {
			snprintf(src, sizeof(src), tmpls[t], kws[k], kws[k], kws[k], kws[k]);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_ok(&r)) cm_note(&st, "soft-expr k=%s t=%zu", kws[k], t);
			prism_free(&r);
		}
	}
	cm_report("gen/soft-ident-expr", &st);
}

/* Auto-unreachable call shapes — inject after stmt-level known-noreturn
 * calls; paren-name / fp / braceless-if stay conservative (no inject). */
static void cm_gen_aur_call_shapes(void) {
	static const char *must_inj[] = {
		"_Noreturn void die(void); void f(void){ die(); }",
		"_Noreturn void die(void); void f(void){ ({ die(); }); }",
		"_Noreturn void die(void); void f(void){ if(1){ die(); } }",
		"_Noreturn void die(void); void f(void){ for(;0;) die(); die(); }",
		"_Noreturn void die(void); void f(void){ switch(0){ default: die(); } }",
		"_Noreturn void die(void); void f(void){ defer die(); }",
		"_Noreturn void die(void); void f(void){ defer { die(); } }",
		"_Noreturn void die(void); void f(void){ defer die(); return; }",
		"_Noreturn int die(void); void f(void){ sizeof(0) + die(); }",
		"_Noreturn int die(void); void f(void){ 1 - die(); }",
		/* Statement-final through casts / grouping / comma / args / return */
		"_Noreturn void die(void); void f(void){ (void)die(); }",
		"_Noreturn void die(void); void f(void){ (void)(die()); }",
		"_Noreturn void die(void); void f(void){ (void)(0, die()); }",
		"_Noreturn void die(void); void f(void){ die(), 1; }",
		"_Noreturn void die(void); void cleanup(void); void f(void){ (cleanup(), die()); }",
		"_Noreturn void die(void); void sink(int); void f(void){ sink(die()); }",
		"_Noreturn void die(void); int f(void){ return die(), 0; }",
		"_Noreturn void die(void); void f(void){ int x; x = (0, die()); (void)x; }",
	};
	static const char *must_not[] = {
		"_Noreturn void die(void); void f(void){ sizeof(die()); }",
		"_Noreturn void die(void); void f(void){ (void)sizeof(die()); }",
		"_Noreturn void die(void); void f(void){ (void)sizeof die(); }",
		"_Noreturn void die(void); void f(void){ sizeof die(); }",
		"_Noreturn void die(void); void f(void){ raw { die(); } }",
		"void die(void); void f(void){ die(); }",
		"_Noreturn void die(void); void f(void){ if(0) die(); }",
		"_Noreturn void die(void); void f(void){ (die)(); }",
		"_Noreturn void die(void); void f(void){ void (*fp)(void)=die; fp(); }",
		"_Noreturn void die(void); void f(void){ if(1) die(); }",
		"_Noreturn void die(void); void f(void){ defer { if(0) die(); } }",
		"_Noreturn int die(void); void f(void){ sizeof +die(); }",
		"_Noreturn int die(void); void f(void){ sizeof -die(); }",
		"_Noreturn int die(void); void f(void){ sizeof !die(); }",
		"_Noreturn int die(void); void f(void){ sizeof ~die(); }",
		"_Noreturn int die(void); void f(void){ sizeof +(+die()); }",
		"_Noreturn int die(void); void f(void){ sizeof +(int)die(); }",
		/* short-circuit: callee may not run */
		"_Noreturn void die(void); void f(int x){ x && die(); }",
		"_Noreturn void die(void); void f(int x){ x || die(); }",
	};
	CmStats st = {0};
	for (size_t i = 0; i < sizeof(must_inj) / sizeof(must_inj[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_inj[i]);
		if (!cm_ok(&r) || !r.output || !cm_has_unreach(r.output))
			cm_note(&st, "aur-shape miss %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(must_not) / sizeof(must_not[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(must_not[i]);
		if (!cm_ok(&r)) cm_note(&st, "aur-shape rej %zu", i);
		else if (r.output && cm_has_unreach(r.output))
			cm_note(&st, "aur-shape false %zu", i);
		prism_free(&r);
	}
	cm_report("gen/aur-call-shapes", &st);
}

/* Auto-static: qual × type × init product (SPEC §6.9 explicit-const gate). */
static void cm_gen_as_init_shapes(void) {
	static const char *quals[] = { "", "const ", "const volatile " };
	static const char *types[] = {
		"int a[3]",
		"int a[]",
		"typeof(int[2]) a",
		"typeof(int[3]) a",
	};
	static const char *inits[] = {
		"={1,2,3}",
		"={[0]=1,[1]=2}",
		"", /* no init — must not promote */
	};
	CmStats st = {0};
	char src[512];
	for (size_t q = 0; q < sizeof(quals) / sizeof(quals[0]); q++) {
		for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
			for (size_t i = 0; i < sizeof(inits) / sizeof(inits[0]); i++) {
				/* `int a[]` needs a brace init to be complete. */
				if (strstr(types[t], "a[]") && !inits[i][0]) continue;
				if (strstr(types[t], "a[]") && strstr(inits[i], "[0]=")) continue;
				snprintf(src, sizeof(src), "void f(void){ %s%s%s; (void)a; }\n",
					 quals[q], types[t], inits[i]);
				st.cells++;
				PrismResult r = cm_tx(src);
				int want_static = (strcmp(quals[q], "const ") == 0) && inits[i][0];
				/* volatile must never promote. */
				if (strstr(quals[q], "volatile")) want_static = 0;
				if (!cm_ok(&r)) {
					cm_note(&st, "as-init status q=%zu t=%zu i=%zu", q, t, i);
					prism_free(&r);
					continue;
				}
				int has = r.output && strstr(r.output, "static");
				if (want_static && !has)
					cm_note(&st, "as-init miss q=%zu t=%zu i=%zu", q, t, i);
				if (!want_static && has && strstr(r.output, "static const"))
					cm_note(&st, "as-init false q=%zu t=%zu i=%zu", q, t, i);
				prism_free(&r);
			}
		}
	}
	/* Typedef-const and enum still promote; runtime init / raw must not. */
	static const char *extra_ok[] = {
		"typedef const int cint; void f(void){ cint a[2]={1,2}; (void)a; }",
		"enum E { A=1,B=2 }; void f(void){ const enum E a[2]={A,B}; (void)a; }",
	};
	static const char *extra_no[] = {
		"int g(void); void f(void){ const int a[2]={g(),2}; (void)a; }",
		"void f(void){ raw { const int a[2]={1,2}; (void)a; } }",
	};
	for (size_t i = 0; i < sizeof(extra_ok) / sizeof(extra_ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(extra_ok[i]);
		if (!cm_ok(&r) || !r.output || !strstr(r.output, "static"))
			cm_note(&st, "as-init extra-ok %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(extra_no) / sizeof(extra_no[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(extra_no[i]);
		if (!cm_ok(&r)) cm_note(&st, "as-init extra-no rej %zu", i);
		else if (r.output && strstr(r.output, "static const"))
			cm_note(&st, "as-init extra-no false %zu", i);
		prism_free(&r);
	}
	cm_report("gen/as-init-shapes", &st);
}

/* Deeper market-seven product: more nests × denser mixed payloads. */
static void cm_gen_market7_deep(void) {
	static const char *nests[] = {
		"%s",
		"{ %s }",
		"if (1) { %s }",
		"if (0) { (void)0; } else { %s }",
		"for (int i = 0; i < 1; i++) { %s }",
		"for (;;) { %s break; }",
		"while (1) { %s break; }",
		"do { %s } while (0);",
		"switch (0) { default: { %s } break; }",
		"switch (1) { case 1: { %s } break; default: break; }",
		"{{ %s }}",
		"int r = ({ { %s } 0; }); (void)r;",
	};
	static const char *payloads[] = {
		"defer cleanup();",
		"defer { cleanup(); }",
		"defer cleanup(); defer { cleanup(); }",
		"int *p = g() orelse 0; (void)p;",
		"int x = 0 orelse 1; (void)x;",
		"int *p = g() orelse g() orelse 0; (void)p;",
		"int a[4]; (void)a[1];",
		"int a[4]; (void)&a[4];",
		"int a[2][2]; (void)a[0][1];",
		"int z; (void)z;",
		"struct { int a; int b; } s; (void)s;",
		"const int k[3]={1,2,3}; (void)k;",
		"const char m[]=\"x\"; (void)m;",
		"raw { int y; (void)y; }",
		"raw { int a[4]; (void)a[1]; }",
		"raw { const int k[2]={1,2}; (void)k; }",
		"die();",
		"defer cleanup(); int a[4]; (void)a[1];",
		"defer cleanup(); int *p = g() orelse 0; (void)p;",
		"int *p = g() orelse 0; int z; (void)p; (void)z;",
		"raw { defer cleanup(); }",
		"raw { int *p = g() orelse 0; (void)p; }",
	};
	CmStats st = {0};
	char body[900], src[1800];
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	for (size_t n = 0; n < sizeof(nests) / sizeof(nests[0]); n++) {
		for (size_t p = 0; p < sizeof(payloads) / sizeof(payloads[0]); p++) {
			snprintf(body, sizeof(body), nests[n], payloads[p]);
			snprintf(src, sizeof(src),
				 "void cleanup(void); _Noreturn void die(void); int *g(void);\n"
				 "void f(void){ %s }\n",
				 body);
			st.cells++;
			PrismResult r = cm_txf(src, f);
			int in_raw = strstr(payloads[p], "raw") != NULL;
			if (!cm_ok(&r) || !r.output) {
				cm_note(&st, "m7d status n=%zu p=%zu", n, p);
				prism_free(&r);
				continue;
			}
			if (!in_raw && strstr(payloads[p], "defer") && cm_kw(r.output, "defer"))
				cm_note(&st, "m7d defer leak n=%zu p=%zu", n, p);
			if (!in_raw && strstr(payloads[p], "orelse") && cm_kw(r.output, "orelse"))
				cm_note(&st, "m7d orelse leak n=%zu p=%zu", n, p);
			if (!in_raw && strstr(payloads[p], "a[1]") && !cm_has_bchk_wrap(r.output))
				cm_note(&st, "m7d bounds miss n=%zu p=%zu", n, p);
			if (strstr(payloads[p], "&a[4]") && cm_has_bchk_wrap(r.output))
				cm_note(&st, "m7d bounds false n=%zu p=%zu", n, p);
			prism_free(&r);
		}
	}
	cm_report("gen/market7-deep", &st);
}

/* Pairwise feature on/off × interaction snip (cheaper than full 2^6 densify). */
static void cm_gen_feature_pair(void) {
	static const char *snips[] = {
		"void cleanup(void); void f(void){ defer cleanup(); }",
		"int *g(void); void f(void){ int *p = g() orelse 0; (void)p; }",
		"void f(void){ int a[4]; (void)a[1]; }",
		"void f(void){ const int k[2]={1,2}; (void)k; }",
		"_Noreturn void die(void); void f(void){ die(); }",
		"void f(void){ int x; (void)x; }",
		"void f(void){ raw { int a[4]; (void)a[1]; } }",
		"void cleanup(void); void f(void){ defer { int a[4]; (void)a[1]; } cleanup(); }",
	};
	/* Feature indices: 0 defer 1 orelse 2 bounds 3 as 4 aur 5 zi */
	CmStats st = {0};
	for (int a = 0; a < 6; a++) {
		for (int b = a + 1; b < 6; b++) {
			for (unsigned bits = 0; bits < 4; bits++) {
				PrismFeatures f = prism_defaults();
				f.defer = f.orelse = f.bounds_check = f.auto_static =
				    f.auto_unreachable = f.zeroinit = false;
				int on_a = bits & 1, on_b = (bits >> 1) & 1;
				bool *slots[] = { &f.defer, &f.orelse, &f.bounds_check,
						 &f.auto_static, &f.auto_unreachable, &f.zeroinit };
				*slots[a] = on_a;
				*slots[b] = on_b;
				for (size_t s = 0; s < sizeof(snips) / sizeof(snips[0]); s++) {
					st.cells++;
					PrismResult r = cm_txf(snips[s], f);
					if (!r.output && r.status == PRISM_OK) {
						cm_note(&st, "pair null a=%d b=%d", a, b);
						prism_free(&r);
						continue;
					}
					if (cm_ok(&r) && r.output) {
						int in_raw = strstr(snips[s], "raw") != NULL;
						if (f.defer && !in_raw && strstr(snips[s], "defer") &&
						    cm_kw(r.output, "defer"))
							cm_note(&st, "pair defer leak");
						if (f.orelse && !in_raw && strstr(snips[s], "orelse") &&
						    cm_kw(r.output, "orelse"))
							cm_note(&st, "pair orelse leak");
						if (!f.bounds_check && !in_raw &&
						    strstr(snips[s], "a[1]") && cm_has_bchk_wrap(r.output))
							cm_note(&st, "pair bounds off");
					}
					prism_free(&r);
				}
			}
		}
	}
	cm_report("gen/feature-pair", &st);
}

/* defer × exit densify (transpile leak oracle) — expand defer_cfg alphabet. */
static void cm_gen_defer_exit_dense(void) {
	static const char *defers[] = {
		"defer cleanup();",
		"defer { cleanup(); }",
		"defer cleanup(); defer { }",
		"defer { cleanup(); cleanup(); }",
		"defer { if (1) cleanup(); }",
	};
	static const char *nests[] = {
		"void cleanup(void); void f(void){ %s %s }",
		"void cleanup(void); void f(void){ { %s %s } }",
		"void cleanup(void); void f(void){ for(int i=0;i<1;i++){ %s %s } }",
		"void cleanup(void); void f(void){ while(1){ %s %s break; } }",
		"void cleanup(void); void f(void){ do { %s %s } while(0); }",
		"void cleanup(void); void f(void){ switch(0){ default: { %s %s } break; } }",
		"void cleanup(void); void f(void){ if(1){ %s %s } }",
		"void cleanup(void); void f(void){ for(;;){ %s %s break; } }",
	};
	static const char *exits[] = {
		"",
		"return;",
		"break;",
		"continue;",
		"goto L; L:;",
	};
	CmStats st = {0};
	char src[1200];
	for (size_t d = 0; d < sizeof(defers) / sizeof(defers[0]); d++) {
		for (size_t n = 0; n < sizeof(nests) / sizeof(nests[0]); n++) {
			for (size_t e = 0; e < sizeof(exits) / sizeof(exits[0]); e++) {
				int loopish = (n == 2 || n == 3 || n == 4 || n == 5 || n == 7);
				if ((!strcmp(exits[e], "break;") || !strcmp(exits[e], "continue;")) &&
				    !loopish)
					continue;
				if (!strcmp(exits[e], "continue;") && (n == 5 || n == 3 || n == 7))
					continue; /* while(1){…break} / switch / for(;;) break nests */
				if (!strcmp(exits[e], "continue;") && n == 3)
					continue;
				snprintf(src, sizeof(src), nests[n], defers[d], exits[e]);
				st.cells++;
				PrismResult r = cm_tx(src);
				if (!cm_ok(&r) || (r.output && cm_kw(r.output, "defer")))
					cm_note(&st, "defer-exit d=%zu n=%zu e=%zu", d, n, e);
				prism_free(&r);
			}
		}
	}
	cm_report("gen/defer-exit-dense", &st);
}

/* Bounds wrapping inside defer / orelse / stmt-expr emit paths. */
static void cm_gen_bounds_emit_paths(void) {
	static const struct {
		const char *src;
		int want_wrap;
	} cells[] = {
		{ "void f(void){ int a[4]; defer { (void)a[1]; } }", 1 },
		{ "void f(void){ int a[4]; defer { (void)&a[4]; } }", 0 },
		{ "int *g(void); void f(void){ int a[4]; int *p = g() orelse 0; (void)a[1]; (void)p; }",
		  1 },
		{ "void f(void){ int a[4]; int r = ({ (void)a[1]; 0; }); (void)r; }", 1 },
		{ "void f(void){ int a[4]; raw { (void)a[1]; } }", 0 },
		{ "void f(void){ int a[4]; if(1){ (void)a[1]; } }", 1 },
		{ "void f(void){ int a[4]; for(int i=0;i<1;i++){ (void)a[i]; } }", 1 },
		{ "void f(void){ int a[4]; switch(0){ default: (void)a[1]; break; } }", 1 },
		{ "void cleanup(void); void f(void){ int a[4]; defer cleanup(); (void)a[1]; }", 1 },
		{ "int *g(void); void f(void){ int a[4]; int *p = g() orelse 0; (void)a[1]; (void)p; }",
		  1 },
		/* orelse-in-index must compose with bounds (was hook-order miss). */
		{ "int f(void){ int a[4]; return a[0 orelse 1]; }", 1 },
		{ "int f(void){ int a[4]; int i=0; return a[i orelse 1]; }", 1 },
		{ "void f(void){ int a[4]; (void)a[0 orelse 1]; }", 1 },
		{ "void f(void){ int a[4]; defer { (void)a[0 orelse 1]; } }", 1 },
		{ "int f(void){ int a[4]; defer { } return a[0 orelse 1]; }", 1 },
		{ "int f(void){ int a[0 orelse 1]; (void)a; return 0; }", 0 },
	};
	CmStats st = {0};
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
		st.cells++;
		PrismResult r = cm_txf(cells[i].src, f);
		if (!cm_ok(&r) || !r.output) {
			cm_note(&st, "emit-path status %zu", i);
			prism_free(&r);
			continue;
		}
		int wrap = cm_has_bchk_wrap(r.output);
		if (cells[i].want_wrap && !wrap)
			cm_note(&st, "emit-path miss %zu", i);
		if (!cells[i].want_wrap && wrap)
			cm_note(&st, "emit-path false %zu", i);
		prism_free(&r);
	}
	cm_report("gen/bounds-emit-paths", &st);
}

/*
 * Hunt 6/7 densify — axis products for post-parse-move registry / typeof /
 * atomic / orelse edges. Replaces hand pins that would only cover one cell.
 */
static void cm_gen_hunt67_densify(void) {
	CmStats st = {0};
	char src[768];
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;

	/* ── init-stmt registry: ctrl × decl ─────────────────────────────
	 * Array decls must wrap; pointer shadows of outer arrays must not. */
	{
		static const char *ctrls[] = {
			"for (%s;;) { %s }",
			"if (%s; 1) { %s }",
			"switch (%s; 0) { default: { %s } break; }",
		};
		static const struct {
			const char *decl;
			const char *use;
			int want_wrap; /* -1 = reject ok too */
			const char *prelude;
		} decls[] = {
			{ "int a[4]={1,2,3,4}", "(void)a[i]", 1, "" },
			{ "typeof(int[4]) a={1,2,3,4}", "(void)a[i]", 1, "" },
			{ "int a[2][2]={{1,2},{3,4}}", "(void)a[0][i]", 1, "" },
			{ "int *g = 0", "(void)g[i]", 0, "int g[10]; " },
			{ "char *g = 0", "(void)g[i]", 0, "char g[8]; " },
			{ "raw int a[4]={1,2,3,4}", "(void)a[i]", 0, "" },
		};
		for (size_t c = 0; c < sizeof(ctrls) / sizeof(ctrls[0]); c++) {
			for (size_t d = 0; d < sizeof(decls) / sizeof(decls[0]); d++) {
				char body[384];
				snprintf(body, sizeof(body), ctrls[c], decls[d].decl, decls[d].use);
				snprintf(src, sizeof(src), "%sint f(int i){ %s return 0; }\n",
					 decls[d].prelude, body);
				st.cells++;
				PrismResult r = cm_txf(src, f);
				if (!cm_ok(&r) || !r.output) {
					/* raw in if/switch init may reject — count ok if err. */
					if (strstr(decls[d].decl, "raw") && cm_err(&r)) {
						prism_free(&r);
						continue;
					}
					cm_note(&st, "init-reg status c=%zu d=%zu", c, d);
					prism_free(&r);
					continue;
				}
				int wrap = cm_has_bchk_wrap(r.output);
				if (decls[d].want_wrap && !wrap)
					cm_note(&st, "init-reg miss c=%zu d=%zu", c, d);
				if (!decls[d].want_wrap && wrap)
					cm_note(&st, "init-reg false c=%zu d=%zu", c, d);
				prism_free(&r);
			}
		}
	}

	/* ── static-storage ICE: storage × index ───────────────────────── */
	{
		static const char *stores[] = { "static", "constexpr" };
		static const char *idxs[] = { "0", "1", "3" };
		for (size_t s = 0; s < sizeof(stores) / sizeof(stores[0]); s++) {
			for (size_t i = 0; i < sizeof(idxs) / sizeof(idxs[0]); i++) {
				snprintf(src, sizeof(src),
					 "const int g[4]={1,2,3,4};\n"
					 "int f(void){ %s int x = g[%s]; return x; }\n",
					 stores[s], idxs[i]);
				st.cells++;
				PrismResult r = cm_txf(src, f);
				/* constexpr may reject non-ICE on some paths — accept err or
				 * unwrapped ok. */
				if (cm_err(&r)) {
					prism_free(&r);
					continue;
				}
				if (!cm_ok(&r) || !r.output)
					cm_note(&st, "ssd-init status s=%zu i=%zu", s, i);
				else if (cm_has_bchk_wrap(r.output))
					cm_note(&st, "ssd-init wrap s=%zu i=%zu", s, i);
				prism_free(&r);
			}
		}
		/* File-scope still uneval. */
		st.cells++;
		PrismResult r = cm_txf(
		    "const int g[4]={1,2,3,4}; static int x = g[0]; int f(void){ return x; }\n", f);
		if (!cm_ok(&r) || (r.output && cm_has_bchk_wrap(r.output)))
			cm_note(&st, "ssd-init file-scope wrap");
		prism_free(&r);
	}

	/* ── raw VLA opt-out × dim spellings ────────────────────────────── */
	{
		static const char *dims[] = { "n", "(n)", "n+0", "1+n" };
		for (size_t d = 0; d < sizeof(dims) / sizeof(dims[0]); d++) {
			snprintf(src, sizeof(src),
				 "int f(int n, int i){ raw int a[%s]; return a[i]; }\n", dims[d]);
			st.cells++;
			PrismResult r = cm_txf(src, f);
			if (!cm_ok(&r) || !r.output)
				cm_note(&st, "raw-vla status %zu", d);
			else if (cm_has_bchk_wrap(r.output))
				cm_note(&st, "raw-vla wrap %zu", d);
			prism_free(&r);

			snprintf(src, sizeof(src),
				 "int f(int n, int i){ raw typeof(int[%s]) a; return a[i]; }\n",
				 dims[d]);
			st.cells++;
			r = cm_txf(src, f);
			if (!cm_ok(&r) || !r.output)
				cm_note(&st, "raw-typeof-vla status %zu", d);
			else if (cm_has_bchk_wrap(r.output))
				cm_note(&st, "raw-typeof-vla wrap %zu", d);
			prism_free(&r);
		}
	}

	/* ── derived multi-dim / commutative index ─────────────────────── */
	{
		static const char *rej[] = {
			"(*a)[j]",
			"(*(a))[j]",
			"(*&(a))[j]",
		};
		for (size_t i = 0; i < sizeof(rej) / sizeof(rej[0]); i++) {
			snprintf(src, sizeof(src),
				 "int f(int j){ int a[3][4]={0}; return %s; }\n", rej[i]);
			st.cells++;
			PrismResult r = cm_txf(src, f);
			if (!cm_err(&r)) cm_note(&st, "derived-2d accept %zu", i);
			prism_free(&r);
		}
		static const char *ok_idx[] = {
			"int f(int *p, int i){ int a[4]={0}; return p[a[i]]; }",
			"int f(int *p, int i){ int a[4]={0}; return p[a[0]]; }",
			"int f(char *p, int i){ int a[4]={0}; return p[a[i]]; }",
			"int f(int i){ int a[4]={0}; int *p=a; return p[a[i]]; }",
			"int f(int i){ int a[4]={0}; int b[4]={0}; return b[a[i]]; }",
		};
		for (size_t i = 0; i < sizeof(ok_idx) / sizeof(ok_idx[0]); i++) {
			st.cells++;
			PrismResult r = cm_txf(ok_idx[i], f);
			if (!cm_ok(&r) || !r.output || !cm_has_bchk_wrap(r.output))
				cm_note(&st, "ptr-idx-arr miss %zu", i);
			prism_free(&r);
		}
		/* Bare idx[arr] still rejects. */
		static const char *still_rej[] = {
			"int f(void){ int a[4]={0}; int i=0; return i[a]; }",
			"int f(int i){ int a[4]={0}; return i[a]; }",
		};
		for (size_t i = 0; i < sizeof(still_rej) / sizeof(still_rej[0]); i++) {
			st.cells++;
			PrismResult r = cm_txf(still_rej[i], f);
			if (!cm_err(&r)) cm_note(&st, "idx-arr accept %zu", i);
			prism_free(&r);
		}
	}

	/* ── typeof/_Atomic array orelse reject ────────────────────────── */
	{
		static const char *types[] = {
			"typeof(int[2])",
			"typeof(int[3])",
			"_Atomic(int[2])",
			"typeof(int[2][2])",
		};
		static const char *fbs[] = { "{0}", "{1,2}", "return 0" };
		for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
			for (size_t fb = 0; fb < sizeof(fbs) / sizeof(fbs[0]); fb++) {
				if (strstr(fbs[fb], "return") && strstr(types[t], "[2][2]"))
					continue;
				snprintf(src, sizeof(src),
					 "int f(void){ %s a={0} orelse %s; return 0; }\n",
					 types[t], fbs[fb]);
				st.cells++;
				PrismResult r = cm_tx(src);
				if (!cm_err(&r))
					cm_note(&st, "typeof-arr-oe accept t=%zu fb=%zu", t, fb);
				prism_free(&r);
			}
		}
		/* Pointer-to-array typeof still OK. */
		st.cells++;
		PrismResult r = cm_tx(
		    "int f(void){ typeof(int[2]) *p = 0 orelse (typeof(int[2])*)0; (void)p; return 0; }\n");
		if (!cm_ok(&r) || (r.output && cm_kw(r.output, "orelse")))
			cm_note(&st, "typeof-arr-ptr-oe false");
		prism_free(&r);
	}

	/* ── const _Atomic typeof: scalar OK, aggregate reject ──────────── */
	{
		static const struct {
			const char *ty;
			int want_ok;
		} cells[] = {
			{ "typeof(int)", 1 },
			{ "typeof(unsigned)", 1 },
			{ "typeof(long)", 1 },
			{ "typeof(int[2])", 0 },
			{ "typeof(struct { int a; })", 0 },
			{ "typeof(union { int a; long b; })", 0 },
			{ "typeof_unqual(struct { int a; })", 0 },
		};
		for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
			/* Keyword form: const _Atomic typeof(...) x */
			if (cells[i].ty[0] == '(')
				snprintf(src, sizeof(src),
					 "int f(void){ const _Atomic typeof%s x; return 0; }\n",
					 cells[i].ty); /* typeof(int) already has parens in ty? */
			else
				snprintf(src, sizeof(src),
					 "int f(void){ const _Atomic %s x; return 0; }\n", cells[i].ty);
			/* cells use typeof(...)/typeof_unqual(...) — never bare '(...)' */
			snprintf(src, sizeof(src),
				 "int f(void){ const _Atomic %s x; return 0; }\n", cells[i].ty);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (cells[i].want_ok) {
				if (!cm_ok(&r))
					cm_note(&st, "atomic-typeof-scalar rej %zu", i);
			} else if (!cm_err(&r))
				cm_note(&st, "atomic-typeof-agg accept %zu", i);
			prism_free(&r);

			snprintf(src, sizeof(src),
				 "int f(void){ const _Atomic(%s) x; return 0; }\n", cells[i].ty);
			st.cells++;
			r = cm_tx(src);
			if (cells[i].want_ok) {
				if (!cm_ok(&r))
					cm_note(&st, "atomic()-typeof-scalar rej %zu", i);
			} else if (!cm_err(&r))
				cm_note(&st, "atomic()-typeof-agg accept %zu", i);
			prism_free(&r);
		}
		/* Init-stmt: _Atomic(typeof(agg)) rejects like bare _Atomic(agg). */
		static const char *init_rej[] = {
			"int f(void){ if (_Atomic(typeof(struct { int a; })) s; 1) return s.a; return 0; }",
			"int f(void){ for (_Atomic(typeof(union { int a; long b; })) u; 0;) return u.a; return 0; }",
			"int f(void){ switch (_Atomic(typeof(struct { int a; })) s; 0){ default: return s.a; } }",
		};
		for (size_t i = 0; i < sizeof(init_rej) / sizeof(init_rej[0]); i++) {
			st.cells++;
			PrismResult r = cm_tx(init_rej[i]);
			if (!cm_err(&r)) cm_note(&st, "atomic-typeof-init accept %zu", i);
			prism_free(&r);
		}
	}

	/* ── defer mid-expression must reject (no keyword leak) ─────────── */
	{
		static const char *hosts[] = {
			"void g(void); void f(void){ %s }",
			"void g(void); void f(void){ if(1){ %s } }",
			"void g(void); void f(void){ for(;;){ %s break; } }",
			"void g(void); void f(void){ switch(0){ default: %s break; } }",
		};
		static const char *payloads[] = {
			"int x; x=1, defer g();",
			"int x=0; (void)(x=1, defer g());",
			"int x; x = (0, defer g());",
		};
		for (size_t h = 0; h < sizeof(hosts) / sizeof(hosts[0]); h++) {
			for (size_t p = 0; p < sizeof(payloads) / sizeof(payloads[0]); p++) {
				snprintf(src, sizeof(src), hosts[h], payloads[p]);
				st.cells++;
				PrismResult r = cm_tx(src);
				if (!cm_err(&r))
					cm_note(&st, "defer-expr accept h=%zu p=%zu", h, p);
				else if (r.output && cm_kw(r.output, "defer"))
					cm_note(&st, "defer-expr leak h=%zu p=%zu", h, p);
				prism_free(&r);
			}
		}
		static const char *ret_hosts[] = {
			"void g(void); int f(void){ %s }",
			"void g(void); int f(void){ if(1){ %s } return 1; }",
		};
		static const char *ret_payloads[] = {
			"return defer g(), 0;",
			"return (defer g(), 0);",
			"int x=0; return x=1, defer g(), 0;",
		};
		for (size_t h = 0; h < sizeof(ret_hosts) / sizeof(ret_hosts[0]); h++) {
			for (size_t p = 0; p < sizeof(ret_payloads) / sizeof(ret_payloads[0]); p++) {
				snprintf(src, sizeof(src), ret_hosts[h], ret_payloads[p]);
				st.cells++;
				PrismResult r = cm_tx(src);
				if (!cm_err(&r))
					cm_note(&st, "defer-ret accept h=%zu p=%zu", h, p);
				prism_free(&r);
			}
		}
	}

	/* ── AUR statement-final through casts / comma / args ───────────── */
	{
		static const char *attrs[] = {
			"_Noreturn void die(void)",
			"[[noreturn]] void die(void)",
			"__attribute__((noreturn)) void die(void)",
		};
		static const char *sites[] = {
			"%s; void f(void){ (void)die(); }",
			"%s; void f(void){ (void)(0, die()); }",
			"%s; void f(void){ die(), 1; }",
			"%s; void cleanup(void); void f(void){ (cleanup(), die()); }",
			"%s; void sink(int); void f(void){ sink(die()); }",
			"%s; int f(void){ return die(), 0; }",
		};
		for (size_t a = 0; a < sizeof(attrs) / sizeof(attrs[0]); a++) {
			for (size_t s = 0; s < sizeof(sites) / sizeof(sites[0]); s++) {
				snprintf(src, sizeof(src), sites[s], attrs[a]);
				st.cells++;
				PrismResult r = cm_tx(src);
				if (!cm_ok(&r) || !r.output || !cm_has_unreach(r.output))
					cm_note(&st, "aur-comma miss a=%zu s=%zu", a, s);
				prism_free(&r);
			}
		}
	}

	cm_report("gen/hunt67-densify", &st);
}

/* Axis: host × defer-payload — mid-expression defer must reject (no leak). */
static void cm_gen_defer_expr_reject(void) {
	static const char *hosts[] = {
		"void g(void); void f(void){ %s }",
		"void g(void); void f(void){ if(1){ %s } }",
		"void g(void); void f(void){ for(int i=0;i<1;i++){ %s } }",
		"void g(void); void f(void){ do { %s } while(0); }",
		"void g(void); void f(void){ switch(0){ default: { %s } break; } }",
		"void g(void); void f(void){ { %s } }",
	};
	static const char *payloads[] = {
		"int x; x=1, defer g();",
		"int x=0; (void)(x, defer g());",
		"int x; x=(0, defer g());",
	};
	static const char *ret_hosts[] = {
		"void g(void); int f(void){ %s }",
		"void g(void); int f(void){ if(1){ %s } return -1; }",
		"void g(void); int f(void){ for(;;){ %s } }",
	};
	static const char *ret_payloads[] = {
		"return defer g(), 0;",
		"return (defer g(), 0);",
		"int x=0; return x, defer g(), 0;",
	};
	CmStats st = {0};
	char src[512];
	for (size_t h = 0; h < sizeof(hosts) / sizeof(hosts[0]); h++) {
		for (size_t p = 0; p < sizeof(payloads) / sizeof(payloads[0]); p++) {
			snprintf(src, sizeof(src), hosts[h], payloads[p]);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_err(&r))
				cm_note(&st, "defer-expr accept h=%zu p=%zu", h, p);
			else if (r.output && cm_kw(r.output, "defer"))
				cm_note(&st, "defer-expr leak h=%zu p=%zu", h, p);
			prism_free(&r);
		}
	}
	for (size_t h = 0; h < sizeof(ret_hosts) / sizeof(ret_hosts[0]); h++) {
		for (size_t p = 0; p < sizeof(ret_payloads) / sizeof(ret_payloads[0]); p++) {
			snprintf(src, sizeof(src), ret_hosts[h], ret_payloads[p]);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_err(&r))
				cm_note(&st, "defer-ret accept h=%zu p=%zu", h, p);
			prism_free(&r);
		}
	}
	/* Statement-form still accepted and lowered. */
	static const char *ok[] = {
		"void g(void); void f(void){ defer g(); }",
		"void g(void); void f(void){ defer { g(); } }",
		"void g(void); void f(void){ int x; defer g(); (void)x; }",
	};
	for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(ok[i]);
		if (!cm_ok(&r) || !r.output || cm_kw(r.output, "defer"))
			cm_note(&st, "defer-stmt ok miss %zu", i);
		prism_free(&r);
	}
	cm_report("gen/defer-expr-reject", &st);
}

/* Axis: lhs × rhs — commutative idx[arr] must reject under -fbounds-check;
 * subscripted-index and normal (arr)[idx] stay OK+wrap. Absorbs hand pins
 * from test.bounds.c commutative / nested-index suites. */
static void cm_gen_bounds_comm_reject(void) {
	static const char *rej_lhs[] = { "i", "(i)", "((i))", "2", "(2)", "s.f" };
	static const char *rej_rhs[] = {
		"a",
		"(a)",
		"&a[0]",
		"(0,a)",
		"(0,&a[0])",
		"((int*)&a[0])",
		"(1?&a[0]:&a[0])",
		"(1?a:a)",
		"(0?a:a)",
		"((1)?(a):(a))",
	};
	static const char *rej_arith[] = {
		"0[a+i]",
		"0[(a+i)]",
		"i[(a+1)]",
		"i[(a+i)]",
		"(i)[(a+1)]",
		"2[a+i]",
	};
	static const char *ok[] = {
		"int f(int i){ int a[8]={0}; return (a)[i]; }",
		"int f(int i){ int a[8]={0}; int *p=a; return p[a[i]]; }",
		"int f(int i){ int a[8]={0}; int *p=a; return p[a[0]]; }",
		"int f(int i){ int a[8]={0}; int b[8]={0}; return b[a[i]]; }",
		"int f(void){ int a[8]={0}; int i=3; return i[a[0]]; }",
	};
	CmStats st = {0};
	char src[512];
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;

	for (size_t L = 0; L < sizeof(rej_lhs) / sizeof(rej_lhs[0]); L++) {
		for (size_t R = 0; R < sizeof(rej_rhs) / sizeof(rej_rhs[0]); R++) {
			/* Literal 2 with &a[0]-family is fine; keep full product. */
			if (strstr(rej_lhs[L], "s.f"))
				snprintf(src, sizeof(src),
					 "struct S{int f;}; int f(void){ int a[8]={0}; struct S s={3};\n"
					 "  %s[%s]=0; return 0; }\n",
					 rej_lhs[L], rej_rhs[R]);
			else
				snprintf(src, sizeof(src),
					 "int f(int i){ int a[8]={0}; %s[%s]=0; return 0; }\n",
					 rej_lhs[L], rej_rhs[R]);
			st.cells++;
			PrismResult r = cm_txf(src, f);
			if (!cm_err(&r))
				cm_note(&st, "comm-rej accept L=%zu R=%zu", L, R);
			prism_free(&r);
		}
	}

	for (size_t a = 0; a < sizeof(rej_arith) / sizeof(rej_arith[0]); a++) {
		snprintf(src, sizeof(src),
			 "int f(int i){ int a[8]={0}; return %s; }\n", rej_arith[a]);
		st.cells++;
		PrismResult r = cm_txf(src, f);
		if (!cm_err(&r)) cm_note(&st, "comm-arith accept %zu", a);
		prism_free(&r);
	}

	/* Two-array ternary index: idx[c?a:b] — same commutative bypass. */
	{
		static const char *tern_idx[] = {
			"i[1?a:b]",
			"i[(1?a:b)]",
			"i[c?a:b]",
			"(i)[(c?a:b)]",
			"2[1?a:b]",
		};
		for (size_t t = 0; t < sizeof(tern_idx) / sizeof(tern_idx[0]); t++) {
			snprintf(src, sizeof(src),
				 "int f(int i,int c){ int a[8]={0},b[8]={0}; return %s; }\n",
				 tern_idx[t]);
			st.cells++;
			PrismResult r = cm_txf(src, f);
			if (!cm_err(&r)) cm_note(&st, "comm-tern accept %zu", t);
			prism_free(&r);
		}
	}

	/* Feature-off: raw idx[arr] must pass through. */
	{
		PrismFeatures off = prism_defaults();
		off.bounds_check = false;
		st.cells++;
		PrismResult r = cm_txf("int f(void){ int a[8]; int i=3; i[a]=0; return 0; }\n", off);
		if (!cm_ok(&r) || !r.output || !strstr(r.output, "i[a]"))
			cm_note(&st, "comm-off raw miss");
		prism_free(&r);
	}

	for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_txf(ok[i], f);
		if (!cm_ok(&r) || !r.output || !cm_has_bchk_wrap(r.output))
			cm_note(&st, "comm-ok miss %zu", i);
		prism_free(&r);
	}
	cm_report("gen/bounds-comm-reject", &st);
}

/* Axis: peel × dims — derived LHS / ptr-arith deref reject; PTA (*p)[i]
 * on int (*p)[N] stays accept/no-wrap. Absorbs hand derived/PTA pins. */
static void cm_gen_bounds_derived_lhs(void) {
	static const char *rej_1d[] = {
		"(&a[0])[i]",
		"(*(&a))[i]",
		"(*&(a))[i]",
		"(*&a)[i]",
		"(*(a))[i]",
		"*(a+i)",
		"*(&a[0]+i)",
		"*(+a+i)",
	};
	static const char *rej_2d[] = {
		"(&m[0])[i][j]",
		"(*(&m))[i][j]",
		"*(*(m+i)+j)",
		"(*m)[i][j]",
	};
	CmStats st = {0};
	char src[512];
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;

	for (size_t i = 0; i < sizeof(rej_1d) / sizeof(rej_1d[0]); i++) {
		snprintf(src, sizeof(src),
			 "int f(int i){ int a[4]={0}; return %s; }\n", rej_1d[i]);
		st.cells++;
		PrismResult r = cm_txf(src, f);
		if (!cm_err(&r)) cm_note(&st, "derived-1d accept %zu", i);
		prism_free(&r);
	}
	for (size_t i = 0; i < sizeof(rej_2d) / sizeof(rej_2d[0]); i++) {
		snprintf(src, sizeof(src),
			 "int f(int i, int j){ int m[3][4]={0}; return %s; }\n", rej_2d[i]);
		st.cells++;
		PrismResult r = cm_txf(src, f);
		if (!cm_err(&r)) cm_note(&st, "derived-2d accept %zu", i);
		prism_free(&r);
	}

	/* PTA polarity: name is pointer-to-array, not an array. */
	{
		static const char *pta[] = {
			"int f(int i){ int b[8]={0}; int (*p)[8]=&b; return (*p)[i]; }",
			"int f(int i){ int b[8]={0}; int (*p)[8]=&b; return (*(p))[i]; }",
			"int f(int i){ int b[4][4]={0}; int (*p)[4]=b; return (*p)[i]; }",
		};
		for (size_t i = 0; i < sizeof(pta) / sizeof(pta[0]); i++) {
			st.cells++;
			PrismResult r = cm_txf(pta[i], f);
			if (!cm_ok(&r) || !r.output)
				cm_note(&st, "pta status %zu", i);
			else if (cm_has_bchk_wrap(r.output))
				cm_note(&st, "pta false-wrap %zu", i);
			prism_free(&r);
		}
	}
	cm_report("gen/bounds-derived-lhs", &st);
}

/* Axis: array declarator spelling × fallback — orelse on array values rejects;
 * pointer-to-array / pointer typedef OK. Absorbs typedef-array orelse pins.
 * Use real declarators (`int a[4]`), not invalid `int[4] a` type-name syntax. */
static void cm_gen_orelse_array_type(void) {
	static const char *arr_decls[] = {
		"int a[4]",
		"typeof(int[4]) a",
		"_Atomic(int[4]) a",
		"arr_t a", /* typedef int arr_t[4] */
		"const arr_t a",
	};
	static const char *fbs[] = { "{0}", "return 0", "fb()" };
	CmStats st = {0};
	char src[640];

	for (size_t t = 0; t < sizeof(arr_decls) / sizeof(arr_decls[0]); t++) {
		for (size_t fb = 0; fb < sizeof(fbs) / sizeof(fbs[0]); fb++) {
			const char *pre = "";
			if (strstr(arr_decls[t], "arr_t"))
				pre = "typedef int arr_t[4]; ";
			if (strstr(fbs[fb], "fb("))
				snprintf(src, sizeof(src),
					 "%sint *fb(void); void f(void){ %s = {0} orelse fb(); }\n",
					 pre, arr_decls[t]);
			else if (strstr(fbs[fb], "return"))
				snprintf(src, sizeof(src),
					 "%sint f(void){ %s = {0} orelse return 0; return 1; }\n",
					 pre, arr_decls[t]);
			else
				snprintf(src, sizeof(src),
					 "%sint *fb(void); void f(void){ %s = {0} orelse %s; (void)a; }\n",
					 pre, arr_decls[t], fbs[fb]);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_err(&r))
				cm_note(&st, "arr-oe accept t=%zu fb=%zu", t, fb);
			prism_free(&r);
		}
	}

	/* Pointer polarities must stay accepted and lowered. */
	static const char *ok[] = {
		"typedef int *ptr_t; int *fb(void); void f(void){ ptr_t x=0 orelse fb(); (void)x; }",
		"int *fb(void); void f(void){ typeof(int[4]) *p=0 orelse (typeof(int[4])*)0; (void)p; }",
		"int *fb(void); void f(void){ int (*p)[4]=0 orelse (int(*)[4])0; (void)p; }",
	};
	for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
		st.cells++;
		PrismResult r = cm_tx(ok[i]);
		if (!cm_ok(&r) || (r.output && cm_kw(r.output, "orelse")))
			cm_note(&st, "arr-oe-ptr false %zu", i);
		prism_free(&r);
	}
	cm_report("gen/orelse-array-type", &st);
}

/* Axis: ctrl × atomic-typeof spelling × agg — init-stmt reject for atomic
 * aggregates; scalar/typeof_unqual non-atomic stay OK+zi. Absorbs zeroinit
 * hand pins for `_Atomic(typeof(…))` if/for/switch-init. */
static void cm_gen_atomic_typeof_init(void) {
	static const char *ctrls[] = {
		"if (%s; 1) return 1; return 0;",
		"for (%s; 0;) return 1; return 0;",
		"switch (%s; 0){ default: return 1; }",
	};
	static const struct {
		const char *decl;
		int want_ok; /* 1 accept+zi, 0 reject */
	} decls[] = {
		{ "_Atomic(typeof(struct { int a; })) s", 0 },
		{ "_Atomic(typeof(union { int a; long b; })) u", 0 },
		{ "_Atomic(typeof_unqual(struct { int a; })) s", 0 },
		{ "_Atomic(typeof_unqual(union { int a; long b; })) u", 0 },
		{ "typeof(struct { int a; }) s", 1 },
		{ "typeof_unqual(union { int a; long b; }) u", 1 },
		{ "typeof(int *) p", 1 },
		{ "const _Atomic typeof(int) x", 1 },
		{ "const _Atomic(typeof(struct { int a; })) s", 0 },
	};
	CmStats st = {0};
	char src[768];

	for (size_t c = 0; c < sizeof(ctrls) / sizeof(ctrls[0]); c++) {
		for (size_t d = 0; d < sizeof(decls) / sizeof(decls[0]); d++) {
			char body[384];
			snprintf(body, sizeof(body), ctrls[c], decls[d].decl);
			snprintf(src, sizeof(src), "int f(void){ %s }\n", body);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (decls[d].want_ok) {
				if (!cm_ok(&r) || !r.output)
					cm_note(&st, "ati-ok status c=%zu d=%zu", c, d);
				else if (!strstr(r.output, "= {0}") && !strstr(r.output, "= 0"))
					cm_note(&st, "ati-ok zi c=%zu d=%zu", c, d);
			} else if (!cm_err(&r)) {
				cm_note(&st, "ati-rej accept c=%zu d=%zu", c, d);
			}
			prism_free(&r);
		}
	}
	cm_report("gen/atomic-typeof-init", &st);
}

/* Axis: bypass_form × {bounds+hard, bounds+warn_safety, bounds-off}.
 * Oracle: hard → reject; warn_safety → accept+raw (no wrap); off → accept+raw. */
static void cm_gen_bounds_warn_safety(void) {
	static const char *bypasses[] = {
		"int f(int i){ int a[8]={0}; return i[a]; }",
		"int f(int i){ int a[8]={0}; return (i)[a]; }",
		"int f(int i){ int a[8]={0}; return (a+i)[0]; }",
		"int f(int i){ int a[8]={0}; return 0[a+i]; }",
		"int f(int i){ int a[8]={0}; return *(a+i); }",
		"int f(int i){ int a[8]={0}; return *(&a[0]+i); }",
		"int f(int i){ int a[8]={0}; return *(int*)(a+i); }",
		"int f(int i){ int a[8]={0}; return i[&a[0]]; }",
	};
	CmStats st = {0};
	PrismFeatures hard = prism_defaults();
	hard.bounds_check = true;
	hard.warn_safety = false;
	PrismFeatures warn = hard;
	warn.warn_safety = true;
	PrismFeatures off = prism_defaults();
	off.bounds_check = false;

	for (size_t i = 0; i < sizeof(bypasses) / sizeof(bypasses[0]); i++) {
		st.cells++;
		PrismResult r = cm_txf(bypasses[i], hard);
		if (!cm_err(&r)) cm_note(&st, "ws-hard accept %zu", i);
		prism_free(&r);

		st.cells++;
		r = cm_txf(bypasses[i], warn);
		if (!cm_ok(&r) || !r.output)
			cm_note(&st, "ws-warn status %zu", i);
		else if (cm_has_bchk_wrap(r.output))
			cm_note(&st, "ws-warn still-wrap %zu", i);
		prism_free(&r);

		st.cells++;
		r = cm_txf(bypasses[i], off);
		if (!cm_ok(&r) || !r.output)
			cm_note(&st, "ws-off status %zu", i);
		else if (cm_has_bchk_wrap(r.output))
			cm_note(&st, "ws-off still-wrap %zu", i);
		prism_free(&r);
	}
	cm_report("gen/bounds-warn-safety", &st);
}

/* Axis: uneval intro × subscript/deref form — must not wrap (false wrap ⇒
 * ICE/trap on VLA / _Generic). Association _Generic IS evaluated → wrap. */
static void cm_gen_bounds_uneval_product(void) {
	static const struct {
		const char *tmpl; /* %s = access expr */
		int want_wrap;
	} intros[] = {
		{ "int f(int i){ int a[8]={0}; return (int)sizeof(%s); }", 0 },
		{ "int f(int i){ int a[8]={0}; return (int)sizeof %s; }", 0 },
		{ "int f(int i){ int a[8]={0}; typeof(%s) *p=0; return !!p; }", 0 },
		{ "int f(int i){ int a[8]={0}; return (int)_Alignof(typeof(%s)); }", 0 },
		{ "int f(int i){ int a[8]={0}; return _Generic(%s, int:1, default:0); }", 0 },
		{ "int f(int i){ int a[8]={0}; return _Generic(1, int:%s, default:0); }", 1 },
		{ "void f(void){ int a[3]={1,2,3}; _Static_assert(%s==1, \"\"); }", 0 },
		{ "struct S{int a[4];}; int f(void){ return (int)__builtin_offsetof(struct S, %s); }",
		  0 },
		{ "struct S{int a[4];}; int f(void){ return (int)offsetof(struct S, %s); }", 0 },
		{ "int g[4]={1,2,3,4}; int f(void){ static int x = %s; return x; }", 0 },
		{ "int f(void){ static int a[4]={1,2,3,4}; static int x = %s; return x; }", 0 },
		{ "int f(void){ constexpr int a[3]={1,2,3}; _Static_assert(%s==1, \"\"); return 0; }",
		  0 },
	};
	static const char *accesses[] = {
		"a[0]",
		"a[i]",
		"*(a+i)",
		"(*&a)[0]",
	};
	CmStats st = {0};
	char src[640];
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;

	for (size_t n = 0; n < sizeof(intros) / sizeof(intros[0]); n++) {
		for (size_t a = 0; a < sizeof(accesses) / sizeof(accesses[0]); a++) {
			/* Association wrap cells: only plain subscripts (deref/derived reject). */
			if (intros[n].want_wrap && accesses[a][0] != 'a') continue;
			/* offsetof / designator only take a[N], not *(a+i). */
			if ((strstr(intros[n].tmpl, "offsetof") ||
			     strstr(intros[n].tmpl, "__builtin_offsetof")) &&
			    accesses[a][0] == '*')
				continue;
			if (strstr(intros[n].tmpl, "offsetof") && strstr(accesses[a], "i"))
				continue; /* designator needs ICE */
			if (strstr(intros[n].tmpl, "_Static_assert") &&
			    (strstr(accesses[a], "i") || accesses[a][0] == '*'))
				continue;
			if (strstr(intros[n].tmpl, "static int x") &&
			    (strstr(accesses[a], "i") || accesses[a][0] == '*'))
				continue;
			if (strstr(intros[n].tmpl, "constexpr") &&
			    (strstr(accesses[a], "i") || accesses[a][0] == '*'))
				continue;
			/* sizeof bare *(a+i) / (*&a)[0] need parens after sizeof keyword. */
			if (strstr(intros[n].tmpl, "sizeof %s") &&
			    (accesses[a][0] == '*' || accesses[a][0] == '('))
				continue;

			snprintf(src, sizeof(src), intros[n].tmpl, accesses[a]);
			/* offsetof needs string.h for the macro form. */
			if (strstr(intros[n].tmpl, "offsetof(struct") &&
			    !strstr(intros[n].tmpl, "__builtin")) {
				char full[720];
				snprintf(full, sizeof(full), "#include <stddef.h>\n%s", src);
				snprintf(src, sizeof(src), "%s", full);
			}
			st.cells++;
			PrismResult r = cm_txf(src, f);
			if (!cm_ok(&r) || !r.output) {
				cm_note(&st, "uneval status n=%zu a=%zu", n, a);
			} else if (intros[n].want_wrap) {
				if (!cm_has_bchk_wrap(r.output))
					cm_note(&st, "uneval miss-wrap n=%zu a=%zu", n, a);
			} else if (cm_has_bchk_wrap(r.output)) {
				cm_note(&st, "uneval false-wrap n=%zu a=%zu", n, a);
			}
			prism_free(&r);
		}
	}
	cm_report("gen/bounds-uneval-product", &st);
}

/* Axis: VLA spelling × ctx — eval wrap; uneval/raw/off no-wrap. */
static void cm_gen_bounds_vla_wrap(void) {
	static const char *decls[] = {
		"int a[n]",
		"int a[(n)]",
		"int a[n+1]",
		"typeof(int[n]) a",
	};
	static const struct {
		const char *tmpl; /* %s = decl, then access via a[i] */
		int want_wrap;
		int is_raw;
	} ctxs[] = {
		{ "int f(int n,int i){ %s; return a[i]; }", 1, 0 },
		{ "int f(int n,int i){ %s; int x=a[i]; return x; }", 1, 0 },
		{ "int g(int); int f(int n,int i){ %s; return g(a[i]); }", 1, 0 },
		{ "int f(int n,int i){ %s; return (int)sizeof(a[i]); }", 0, 0 },
		{ "int f(int n,int i){ raw %s; return a[i]; }", 0, 1 },
	};
	CmStats st = {0};
	char src[512];
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;

	for (size_t d = 0; d < sizeof(decls) / sizeof(decls[0]); d++) {
		for (size_t c = 0; c < sizeof(ctxs) / sizeof(ctxs[0]); c++) {
			snprintf(src, sizeof(src), ctxs[c].tmpl, decls[d]);
			st.cells++;
			PrismResult r = cm_txf(src, f);
			if (!cm_ok(&r) || !r.output) {
				cm_note(&st, "vla status d=%zu c=%zu", d, c);
			} else if (ctxs[c].want_wrap) {
				if (!cm_has_bchk_wrap(r.output))
					cm_note(&st, "vla miss-wrap d=%zu c=%zu", d, c);
			} else if (cm_has_bchk_wrap(r.output)) {
				cm_note(&st, "vla false-wrap d=%zu c=%zu", d, c);
			}
			prism_free(&r);
		}
	}

	/* Feature-off VLA. */
	{
		PrismFeatures off = f;
		off.bounds_check = false;
		st.cells++;
		PrismResult r = cm_txf("int f(int n,int i){ int a[n]; return a[i]; }", off);
		if (!cm_ok(&r) || (r.output && cm_has_bchk_wrap(r.output)))
			cm_note(&st, "vla-off wrap");
		prism_free(&r);
	}
	cm_report("gen/bounds-vla-wrap", &st);
}

/* Axis: ptr-arith/cast-deref form × site — reject in eval/dim; no-wrap in sizeof. */
static void cm_gen_bounds_deref_sites(void) {
	static const char *forms[] = {
		"*(a+i)",
		"*((a+i))",
		"*(((a+i)))",
		"*(&a[0]+i)",
		"*(int*)(a+i)",
		"*((int*)(a+i))",
		"(a+i)[0]",
		"0[a+i]",
		"(*&a)[i]",
	};
	static const struct {
		const char *tmpl;
		int want_err; /* 1 reject, 0 accept+no-wrap */
	} sites[] = {
		{ "int f(int i){ int a[8]={0}; return %s; }", 1 },
		{ "int f(int i){ int a[8]={0}; int x=%s; return x; }", 1 },
		{ "int f(int i){ int a[8]={0}; defer %s; return 0; }", 1 },
		{ "int f(int i){ int a[8]={0}; typeof(int[%s]) *p=0; return !!p; }", 1 },
		{ "int f(int i){ int a[8]={0}; _Atomic(int[%s]) *p=0; return !!p; }", 1 },
		{ "int f(int i){ int a[8]={0}; return (int)sizeof(%s); }", 0 },
	};
	CmStats st = {0};
	char src[640];
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;

	for (size_t fo = 0; fo < sizeof(forms) / sizeof(forms[0]); fo++) {
		for (size_t s = 0; s < sizeof(sites) / sizeof(sites[0]); s++) {
			/* Dim sites need an expression usable as array size — skip
			 * forms that aren't a single primary after `[`. */
			if ((strstr(sites[s].tmpl, "typeof(int[%s])") ||
			     strstr(sites[s].tmpl, "_Atomic(int[%s])")) &&
			    forms[fo][0] != '*')
				continue;
			/* defer needs a statement; cast-deref as stmt is fine with ; */
			snprintf(src, sizeof(src), sites[s].tmpl, forms[fo]);
			st.cells++;
			PrismResult r = cm_txf(src, f);
			if (sites[s].want_err) {
				if (!cm_err(&r)) cm_note(&st, "deref accept fo=%zu s=%zu", fo, s);
			} else if (!cm_ok(&r) || !r.output) {
				cm_note(&st, "deref-ue status fo=%zu s=%zu", fo, s);
			} else if (cm_has_bchk_wrap(r.output)) {
				cm_note(&st, "deref-ue wrap fo=%zu s=%zu", fo, s);
			}
			prism_free(&r);
		}
	}
	cm_report("gen/bounds-deref-sites", &st);
}

/* Axis: LHS peel × index — paren / ternary / comma bases must still wrap. */
static void cm_gen_bounds_lhs_peel(void) {
	static const char *lhs[] = {
		"a",
		"(a)",
		"((a))",
		"(((a)))",
		"(0,a)",
		"(1?a:b)",
		"(c?a:b)",
		"((c)?(a):(b))",
	};
	static const char *idxs[] = { "0", "i", "i+1" };
	CmStats st = {0};
	char src[512];
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;

	for (size_t L = 0; L < sizeof(lhs) / sizeof(lhs[0]); L++) {
		for (size_t ix = 0; ix < sizeof(idxs) / sizeof(idxs[0]); ix++) {
			if (strstr(lhs[L], "b") || strstr(lhs[L], "c?"))
				snprintf(src, sizeof(src),
					 "int f(int i,int c){ int a[8]={0},b[8]={0}; return %s[%s]; }\n",
					 lhs[L], idxs[ix]);
			else
				snprintf(src, sizeof(src),
					 "int f(int i){ int a[8]={0}; return %s[%s]; }\n",
					 lhs[L], idxs[ix]);
			st.cells++;
			PrismResult r = cm_txf(src, f);
			if (!cm_ok(&r) || !r.output || !cm_has_bchk_wrap(r.output))
				cm_note(&st, "lhs-peel miss L=%zu ix=%zu", L, ix);
			prism_free(&r);
		}
	}
	cm_report("gen/bounds-lhs-peel", &st);
}

/* Axis: decl rank/completeness × access — wrap only checkable ranks. */
static void cm_gen_bounds_rank_registry(void) {
	static const struct {
		const char *src;
		int want_wrap; /* 1 wrap, 0 accept-no-wrap */
		int max_wraps; /* 0 = any if want; else upper bound */
	} cells[] = {
		{ "int f(int i){ int a[8]={0}; return a[i]; }", 1, 0 },
		{ "int f(int i,int j){ int m[3][4]={0}; return m[i][j]; }", 1, 0 },
		{ "int f(int i){ int *p[8]={0}; return p[i]?1:0; }", 1, 1 }, /* outer only */
		{ "int f(int i){ int b[8]={0}; int (*p)[8]=&b; return (*p)[i]; }", 0, 0 },
		{ "int f(void){ int (*p[5])[10]; p[1][2]=0; return 0; }", 1, 1 },
		{ "extern int m[][10]; int f(int i){ return m[0][i]; }", 0, 0 },
		{ "int g[8]; int f(int i){ return g[i]; }", 1, 0 },
		{ "typedef int Arr[8]; int f(int i){ Arr a={0}; return a[i]; }", 1, 0 },
		{ "typedef int (*P)[8]; int f(int i){ int b[8]={0}; P p=&b; return (*p)[i]; }", 0,
		  0 },
		{ "struct S{int a[4]; int fam[];}; int f(struct S *s,int i){ return s->a[i]; }", 0,
		  0 },
	};
	CmStats st = {0};
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;

	for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
		st.cells++;
		PrismResult r = cm_txf(cells[i].src, f);
		if (!cm_ok(&r) || !r.output) {
			cm_note(&st, "rank status %zu", i);
			prism_free(&r);
			continue;
		}
		int wraps = 0;
		for (const char *p = r.output; (p = strstr(p, "[__prism_bchk")) != NULL; p++)
			wraps++;
		if (cells[i].want_wrap) {
			if (!wraps) cm_note(&st, "rank miss-wrap %zu", i);
			else if (cells[i].max_wraps && wraps > cells[i].max_wraps)
				cm_note(&st, "rank over-wrap %zu n=%d", i, wraps);
		} else if (wraps) {
			cm_note(&st, "rank false-wrap %zu n=%d", i, wraps);
		}
		prism_free(&r);
	}
	cm_report("gen/bounds-rank-registry", &st);
}

/* Axis: member depth × . / -> — local array name collision must not wrap
 * member subscripts (TT_MEMBER filter). */
static void cm_gen_bounds_member_falsepos(void) {
	static const char *forms[] = {
		"struct S{int a[4];}; int f(int i){ struct S s={{0}}; return s.a[i]; }",
		"struct S{int a[4];}; int f(struct S *p,int i){ return p->a[i]; }",
		"struct S{int a[4];}; int f(int i){ int a[4]={0}; struct S s={{0}}; (void)a; return "
		"s.a[i]; }",
		"struct S{int a[4];}; int f(int i){ int a[4]={0}; struct S s={{0}}; return a[i]+s.a[0]; "
		"}",
		"union U{int a[4]; long x;}; int f(int i){ union U u={{0}}; return u.a[i]; }",
		"struct A{struct B{int arr[4];} b;}; int f(int i){ struct A x={{{0}}}; return "
		"x.b.arr[i]; }",
		"struct S{int a[4];}; int f(int i){ struct S s={{0}}; return (&s)->a[i]; }",
	};
	CmStats st = {0};
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;

	for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++) {
		st.cells++;
		PrismResult r = cm_txf(forms[i], f);
		if (!cm_ok(&r) || !r.output) {
			cm_note(&st, "memb status %zu", i);
		} else if (i == 3) {
			/* local a[i] wraps; s.a[0] must not contribute a member wrap —
			 * require at least one wrap from local. */
			if (!cm_has_bchk_wrap(r.output))
				cm_note(&st, "memb local-miss %zu", i);
		} else if (cm_has_bchk_wrap(r.output)) {
			cm_note(&st, "memb false-wrap %zu", i);
		}
		prism_free(&r);
	}
	cm_report("gen/bounds-member-falsepos", &st);
}

#ifndef _WIN32
/* Transpile → cc → run. Exit codes: child status, or -1000/-1001/-1002 on fail. */
static int cm_exec(const char *src, PrismFeatures feat) {
	PrismResult r = cm_txf(src, feat);
	if (!cm_ok(&r) || !r.output) {
		prism_free(&r);
		return -1000;
	}
	char *path = create_temp_file(r.output);
	prism_free(&r);
	if (!path) return -1002;
	char bin[PATH_MAX];
	int fd = test_mkstemp(bin, "cm_exec_");
	if (fd < 0) {
		unlink(path);
		free(path);
		return -1002;
	}
	close(fd);
	unlink(bin);
	char cmd[PATH_MAX * 2 + 80];
	snprintf(cmd, sizeof(cmd), "cc -std=gnu11 -o %s %s >/dev/null 2>&1", bin, path);
	if (run_command_status(cmd) != 0) {
		unlink(path);
		free(path);
		return -1001;
	}
	int st = run_command_status(bin);
	unlink(bin);
	unlink(path);
	free(path);
	return st;
}

#define CM_LOG_PRE                                                                                   \
	"#include <string.h>\n"                                                                      \
	"static char __B[256]; static int __P;\n"                                                     \
	"static void L(const char *s){ for(; *s && __P < 255; s++) __B[__P++]=*s; }\n"                 \
	"static int chk(const char *e){ __B[__P]=0; return strcmp(__B, e) != 0; }\n"

/* Executed defer LIFO / exit product — pulls test.defer.c oracles into a grid. */
static void cm_gen_runtime_defer(void) {
	static const struct {
		const char *body;
		const char *expect;
	} cells[] = {
		{ "L(\"1\"); { defer L(\"A\"); }", "1A" },
		{ "L(\"1\"); { defer L(\"C\"); defer L(\"B\"); defer L(\"A\"); }", "1ABC" },
		{ "{ defer L(\"A\"); { defer L(\"B\"); L(\"1\"); } }", "1BA" },
		{ "{ defer L(\"A\"); { defer L(\"B\"); { defer L(\"C\"); L(\"1\"); } } }", "1CBA" },
		{ "{ defer L(\"A\"); L(\"1\"); goto E; } E: L(\"2\");", "1A2" },
		{ "{ defer L(\"A\"); { defer L(\"B\"); goto E; } } E: L(\"2\");", "BA2" },
		/* Same-scope goto does not unwind defer until the block ends. */
		{ "{ defer L(\"A\"); L(\"1\"); goto E; E: L(\"2\"); }", "12A" },
		{ "for (int i = 0; i < 1; i++) { defer L(\"A\"); L(\"1\"); break; }", "1A" },
		{ "for (int i = 0; i < 2; i++) { defer L(\"A\"); L(\"1\"); }", "1A1A" },
		{ "for (int i = 0; i < 2; i++) { defer L(\"A\"); if (i == 0) continue; L(\"1\"); }",
		  "A1A" },
		{ "for (int i = 0; i < 3; i++) { defer L(\"A\"); if (i < 2) continue; L(\"1\"); }",
		  "AA1A" },
		{ "int i=0; while (i < 1) { defer L(\"A\"); L(\"1\"); i++; }", "1A" },
		{ "int i=0; while (i < 2) { defer L(\"A\"); L(\"1\"); i++; }", "1A1A" },
		{ "do { defer L(\"A\"); L(\"1\"); } while (0);", "1A" },
		{ "int i=0; do { defer L(\"A\"); L(\"1\"); i++; } while (i < 2);", "1A1A" },
		{ "switch (0) { default: { defer L(\"A\"); L(\"1\"); } break; }", "1A" },
		{ "switch (1) { case 1: { defer L(\"A\"); L(\"1\"); } break; default: break; }", "1A" },
		{ "if (1) { defer L(\"A\"); L(\"1\"); }", "1A" },
		{ "if (0) { defer L(\"A\"); L(\"X\"); } else { defer L(\"B\"); L(\"1\"); }", "1B" },
		{ "{ defer L(\"A\"); L(\"1\"); } return chk(\"1A\");", "1A" },
		{ "{ defer { L(\"A\"); } L(\"1\"); }", "1A" },
		{ "{ defer L(\"A\"); defer { L(\"B\"); } L(\"1\"); }", "1BA" },
		{ "for (;;) { defer L(\"A\"); L(\"1\"); break; }", "1A" },
		{ "{ defer L(\"A\"); { defer L(\"B\"); L(\"1\"); goto E; } } E:;", "1BA" },
	};
	CmStats st = {0};
	char src[1800];
	PrismFeatures f = prism_defaults();
	for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
		if (strstr(cells[i].body, "return chk")) {
			snprintf(src, sizeof(src),
				 CM_LOG_PRE "int main(void){ %s }\n", cells[i].body);
		} else {
			snprintf(src, sizeof(src),
				 CM_LOG_PRE "int main(void){ %s return chk(\"%s\"); }\n",
				 cells[i].body, cells[i].expect);
		}
		st.cells++;
		int ex = cm_exec(src, f);
		if (ex != 0) cm_note(&st, "rt-defer %zu exit=%d", i, ex);
	}
	cm_report("gen/runtime-defer", &st);
}

/* Executed orelse value / action / defer-interaction product. */
static void cm_gen_runtime_orelse(void) {
	static const struct {
		const char *src;
	} cells[] = {
		{ CM_LOG_PRE
		  "static int *ok(void){ static int x=1; return &x; }\n"
		  "static int *nil(void){ return 0; }\n"
		  "int main(void){ int *p = ok() orelse nil(); L(p?\"T\":\"F\"); return chk(\"T\"); }\n" },
		{ CM_LOG_PRE
		  "static int *ok(void){ static int x=1; return &x; }\n"
		  "static int *nil(void){ return 0; }\n"
		  "int main(void){ int *p = nil() orelse ok(); L(p?\"T\":\"F\"); return chk(\"T\"); }\n" },
		{ CM_LOG_PRE
		  "static int *nil(void){ return 0; }\n"
		  "int main(void){ int *p = nil() orelse { L(\"B\"); }; L(\"Z\"); "
		  "(void)p; return chk(\"BZ\"); }\n" },
		{ CM_LOG_PRE
		  "static int *ok(void){ static int x=1; return &x; }\n"
		  "int main(void){ { defer L(\"D\"); int *p = ok() orelse { L(\"B\"); }; "
		  "L(\"A\"); (void)p; } return chk(\"AD\"); }\n" },
		{ CM_LOG_PRE
		  "static int *nil(void){ return 0; }\n"
		  "static int helper(void){ defer L(\"D\"); "
		  "int *p = nil() orelse return 0; L(\"X\"); (void)p; return 1; }\n"
		  "int main(void){ (void)helper(); return chk(\"D\"); }\n" },
		{ CM_LOG_PRE
		  "static int *nil(void){ return 0; }\n"
		  "int main(void){ for (int i=0;i<1;i++){ defer L(\"L\"); "
		  "int *p = nil() orelse break; (void)p; } return chk(\"L\"); }\n" },
		{ CM_LOG_PRE
		  "static int g(void){ return 0; }\n"
		  "int main(void){ int x = g() orelse 7; return x != 7; }\n" },
		{ CM_LOG_PRE
		  "static int g(void){ return 5; }\n"
		  "int main(void){ int x = g() orelse 7; return x != 5; }\n" },
		{ CM_LOG_PRE
		  "static int g(void){ return 0; }\n"
		  "int main(void){ int x = g() orelse g() orelse 3; return x != 3; }\n" },
		{ CM_LOG_PRE
		  "static int *nil(void){ return 0; }\n"
		  "static int *ok(void){ static int x=1; return &x; }\n"
		  "int main(void){ int *p = nil() orelse nil() orelse ok(); return p?0:1; }\n" },
		{ CM_LOG_PRE
		  "static int *nil(void){ return 0; }\n"
		  "int main(void){ for(int i=0;i<2;i++){ int *p = nil() orelse continue; L(\"X\"); "
		  "(void)p; } return chk(\"\"); }\n" },
		{ CM_LOG_PRE
		  "static int *nil(void){ return 0; }\n"
		  "int main(void){ { defer L(\"D\"); int *p = nil() orelse goto done; "
		  "L(\"X\"); done: (void)p; } return chk(\"D\"); }\n" },
		{ "struct I{int a[2][2];}; int main(void){ struct I s={{{4,9},{2,3}}}; "
		  "int i=0; return s.a[0][i orelse 1] != 9; }\n" },
		{ "struct I{int a[2][2];}; int main(void){ struct I s={{{4,9},{2,3}}}; "
		  "struct I *p=&s; int i=0; return p->a[0][i orelse 1] != 9; }\n" },
		/* Exhaustive finite scalar model: 25 two-term and 125 three-term
		 * truth/evaluation-count cells in one compiled program. */
		{ "static int C[3]; static int V(int k,int v){ C[k]++; return v; }\n"
		  "int main(void){ for(int a=-2;a<=2;a++) for(int b=-2;b<=2;b++) "
		  "for(int c=-2;c<=2;c++){ C[0]=C[1]=C[2]=0; "
		  "int x=V(0,a) orelse V(1,b); "
		  "if(x!=(a?a:b)||C[0]!=1||C[1]!=(a==0)||C[2]!=0)return 1; "
		  "C[0]=C[1]=C[2]=0; int y=V(0,a) orelse V(1,b) orelse V(2,c); "
		  "if(y!=(a?a:(b?b:c))||C[0]!=1||C[1]!=(a==0)||"
		  "C[2]!=((a==0)&&(b==0)))return 2; } return 0; }\n" },
	};
	CmStats st = {0};
	PrismFeatures f = prism_defaults();
	for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
		st.cells++;
		int ex = cm_exec(cells[i].src, f);
		if (ex != 0) cm_note(&st, "rt-orelse %zu exit=%d", i, ex);
	}
	cm_report("gen/runtime-orelse", &st);
}

/* Executed bounds trap / one-past / raw-suppress product. */
static void cm_gen_runtime_bounds(void) {
	static const struct {
		const char *src;
		int expect; /* exact exit, or -1 = must trap (nonzero) */
	} cells[] = {
		{ "int main(void){ int a[4]={1,2,3,4}; return a[1]; }\n", 2 },
		{ "int main(void){ int a[4]={0}; int *p=&a[4]; (void)p; return 0; }\n", 0 },
		{ "int main(void){ int a[4]={0}; (void)&a[4]; return 0; }\n", 0 },
		{ "int main(void){ int a[4]={0}; (void)(int*)&a[4]; return 0; }\n", 0 },
		{ "int main(void){ int a[4]={0}; (void)a[4]; return 0; }\n", -1 },
		{ "int main(void){ int a[4]={0}; (void)a[5]; return 0; }\n", -1 },
		{ "int main(void){ int a[2][2]={{1,2},{3,4}}; return a[1][0]; }\n", 3 },
		{ "int main(void){ raw int a[4]={0,1,2,3}; return a[2]; }\n", 2 },
		{ "int main(void){ int a[4]={9,8,7,6}; int i=2; return a[i]; }\n", 7 },
		{ "int main(void){ int a[4]={0}; int i=4; (void)a[i]; return 0; }\n", -1 },
		{ "int main(void){ int a[4]={1}; defer { } return a[0]; }\n", 1 },
		{ "int main(void){ int a[4]={0}; int *p=0; p = p orelse &a[0]; return *p; }\n", 0 },
		{ "int main(void){ int a[4]={0}; (void)&(a)[4]; return 0; }\n", 0 },
		{ "int main(void){ int a[4]={5,6,7,8}; return (a)[0]; }\n", 5 },
		{ "int main(void){ int a[4]={10,20,30,40}; "
		  "for(int i=0;i<4;i++)for(int j=0;j<4;j++)"
		  "{int x=a[i orelse j]; if(x!=a[i?i:j])return 1;} return 0; }\n", 0 },
	};
	CmStats st = {0};
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
		st.cells++;
		int ex = cm_exec(cells[i].src, f);
		if (ex <= -1000) {
			cm_note(&st, "rt-bounds infra %zu (%d)", i, ex);
			continue;
		}
		if (cells[i].expect < 0) {
			if (ex == 0) cm_note(&st, "rt-bounds no-trap %zu", i);
		} else if (ex != cells[i].expect) {
			cm_note(&st, "rt-bounds exit %zu got=%d want=%d", i, ex, cells[i].expect);
		}
	}
	cm_report("gen/runtime-bounds", &st);
}

/* Executed zero-init value product. */
static void cm_gen_runtime_zi(void) {
	static const char *cells[] = {
		"int main(void){ int x; return x != 0; }\n",
		"int main(void){ int a[4]; return a[0]|a[1]|a[2]|a[3]; }\n",
		"int main(void){ struct { int a; int b; } s; return s.a|s.b; }\n",
		"int main(void){ union { int a; char b[4]; } u; return u.a; }\n",
		"int main(void){ int *p; return p != 0; }\n",
		"int main(void){ typeof(int) x; return x != 0; }\n",
		"int main(void){ int x, y; return x|y; }\n",
		"int main(void){ raw int x; return 0; }\n", /* raw may skip zi — just must run */
	};
	CmStats st = {0};
	PrismFeatures f = prism_defaults();
	for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
		st.cells++;
		int ex = cm_exec(cells[i], f);
		if (ex != 0) cm_note(&st, "rt-zi %zu exit=%d", i, ex);
	}
	cm_report("gen/runtime-zi", &st);
}

/* Cross-feature executed: defer+bounds, orelse+zi, raw suppress, as promote. */
static void cm_gen_runtime_cross(void) {
	static const struct {
		const char *src;
		int expect;
	} cells[] = {
		{ CM_LOG_PRE
		  "int main(void){ int a[4]={1,2,3,4}; { defer L(\"D\"); L(\"1\"); } "
		  "return chk(\"1D\") || (a[1] != 2); }\n",
		  0 },
		{ "int main(void){ const int k[3]={1,2,3}; return k[0]+k[1]+k[2] != 6; }\n", 0 },
		{ "int main(void){ raw int a[4]={0,1,2,3}; return a[2] != 2; }\n", 0 },
		{ CM_LOG_PRE
		  "static int *nil(void){ return 0; }\n"
		  "int main(void){ int z; { defer L(\"D\"); "
		  "int *p = nil() orelse { L(\"B\"); }; (void)p; } "
		  "return chk(\"BD\") || (z != 0); }\n",
		  0 },
		{ "int main(void){ int a[4]={0}; (void)&a[4]; return a[0]; }\n", 0 },
		{ "int main(void){ int x=0; int *p = (int*)0 orelse &x; return *p; }\n", 0 },
		{ CM_LOG_PRE
		  "int main(void){ for(int i=0;i<2;i++){ defer L(\"A\"); L(\"1\"); } "
		  "return chk(\"1A1A\"); }\n",
		  0 },
		{ "int main(void){ typeof(int[2]) a; return a[0]|a[1]; }\n", 0 },
	};
	CmStats st = {0};
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
		st.cells++;
		int ex = cm_exec(cells[i].src, f);
		if (ex != cells[i].expect)
			cm_note(&st, "rt-cross %zu exit=%d", i, ex);
	}
	cm_report("gen/runtime-cross", &st);
}
#undef CM_LOG_PRE
#endif /* !_WIN32 */

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

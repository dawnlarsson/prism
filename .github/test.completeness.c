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
 * Both run unconditionally — no env gate.
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
static void cm_gen_ice_stmt_expr(void);
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
		/* Absorbed from test.cert.c: the roadmap's flag-cube / cross-feature
		 * slices.  They were 7 hand-written functions asserting exactly what
		 * this product asserts; as rows they sweep all 128 masks instead of
		 * the single mask each function pinned. */
		"int main(void){ int seed=0; { defer (void)0; int v = (seed orelse 7); (void)v; } return 0; }",
		"int main(void){ int z=0; defer (void)0; int y = (z orelse 3); defer (void)1; (void)y; return 0; }",
		"int main(void){ int a = 0 orelse 1, b = 3; (void)a; (void)b; return 0; }",
		"void f(int x){ defer { (void)0; } switch (x) { int y; case 0: y = 1; break; default: break; } }",
		"int main(void){ int n = 3; int arr[n orelse 4]; (void)arr; return 0; }",
		"static int get(void){ return 1; } int main(void){ defer { int t = get() orelse 0; (void)t; } return 0; }",
		"int main(void){ const int k[2] = {1, 2}; (void)k; return 0; }",
		"int main(void){ int n = 3; int a[n]; (void)a; return 0; }",
		"int main(void){ defer (void)0; int z=0; int y = z orelse 1; int x[2]; "
		"const int k[2]={1,2}; (void)y; (void)x; (void)k; return 0; }",
	};
	CmStats st = {0};
	/* 2^7 = 128 feature masks (warn_safety is the 7th, absorbed from the cert
	 * suite's -fno-safety hooks) × snips. */
	for (unsigned mask = 0; mask < 128; mask++) {
		PrismFeatures f = prism_defaults();
		f.defer = mask & 1;
		f.orelse = (mask >> 1) & 1;
		f.bounds_check = (mask >> 2) & 1;
		f.auto_static = (mask >> 3) & 1;
		f.auto_unreachable = (mask >> 4) & 1;
		f.zeroinit = (mask >> 5) & 1;
		f.warn_safety = (mask >> 6) & 1;
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

#ifndef _WIN32
/* Compiling byte-identical output twice proves nothing and costs a process
 * spawn, so the executed tiers dedupe on the emitted C.  Feature masks that
 * are irrelevant to a given program collapse onto the same output, which is
 * most of the cube. */
static unsigned long cm_run_seen[512];
static int cm_run_seen_n;

static int cm_already_ran(const char *out) {
	unsigned long h = 5381;
	for (const unsigned char *p = (const unsigned char *)out; *p; p++) h = h * 33u + *p;
	for (int i = 0; i < cm_run_seen_n; i++)
		if (cm_run_seen[i] == h) return 1;
	if (cm_run_seen_n < (int)(sizeof(cm_run_seen) / sizeof(cm_run_seen[0])))
		cm_run_seen[cm_run_seen_n++] = h;
	return 0;
}

/*
 * Executed feature-cube slice (absorbs test.cert.c's compile-and-run oracles).
 *
 * The cube above is a static oracle: it checks that a disabled feature leaves
 * no trace and an enabled one leaves no keyword.  It cannot see "lowered to C
 * that does not compile" or "compiles but returns nonzero".  This tier runs the
 * emitted C for a product of runnable programs × feature configurations, which
 * is the only oracle in the tree that catches both.
 */
static void cm_gen_cert_compile_run(void) {
	/* Runnable programs: each returns 0 iff its semantics held.  `needs` names
	 * the features the program's EXPECTED RESULT depends on — reading an
	 * uninitialized array only yields zeros with zeroinit on, and a
	 * declaration jumped over by a switch label is a hard error unless safety
	 * is downgraded.  Cells a config cannot satisfy are gated out and counted
	 * separately, so a tier that skips everything cannot read as coverage. */
#define CR_NEED_ZI 1u
#define CR_NEED_NOSAFETY 2u
	static const struct {
		unsigned needs;
		const char *code;
	} progs[] = {
	    {0,
	     "int main(void){ int seed = 0; int hit = 0;\n"
	     "  { defer hit = 1; int v = (seed orelse 7); if (v != 7) return 1; }\n"
	     "  return hit ? 0 : 2; }\n"},
	    {0,
	     "int main(void){ int z = 0; int n = 0;\n"
	     "  { defer n += 1; int y = (z orelse 3); defer n += 2; if (y != 3) return 1; }\n"
	     "  return n == 3 ? 0 : 2; }\n"},
	    {0, "int main(void){ int a = 0 orelse 1, b = 3; return (a == 1 && b == 3) ? 0 : 1; }\n"},
	    {0,
	     "int main(void){ int n = 3; int arr[n orelse 4]; arr[0] = 7;\n"
	     "  return arr[0] == 7 ? 0 : 1; }\n"},
	    {0,
	     "static int get(void){ return 1; }\n"
	     "int main(void){ int seen = 0; { defer { int t = get() orelse 0; seen = t; } }\n"
	     "  return seen == 1 ? 0 : 1; }\n"},
	    {0,
	     "int main(void){ const int k[2] = {1, 2}; return (k[0] == 1 && k[1] == 2) ? 0 : 1; }\n"},
	    {CR_NEED_ZI,
	     "int main(void){ int x[3]; for (int i = 0; i < 3; i++) if (x[i]) return 1; return 0; }\n"},
	    {CR_NEED_NOSAFETY,
	     "void f(int x){ defer { (void)0; } switch (x) { int y; case 0: y = 1; (void)y; break;\n"
	     "  default: break; } }\n"
	     "int main(void){ f(0); f(1); return 0; }\n"},
	};
	/* Feature configurations the cert roadmap named as mandatory couplings. */
	static const struct {
		const char *tag;
		int zeroinit, auto_static, bounds_check, warn_safety;
	} cfg[] = {
	    {"defaults", 1, 1, 1, 0},
	    {"zi-off", 0, 1, 1, 0},
	    {"as-off", 1, 0, 1, 0},
	    {"bchk-off", 1, 1, 0, 0},
	    {"nosafety", 1, 1, 1, 1},
	    {"zi-off+nosafety", 0, 1, 1, 1},
	};
	CmStats st = {0};
	long gated = 0, dedup = 0;
	for (size_t c = 0; c < sizeof(cfg) / sizeof(cfg[0]); c++) {
		PrismFeatures f = prism_defaults();
		f.zeroinit = cfg[c].zeroinit;
		f.auto_static = cfg[c].auto_static;
		f.bounds_check = cfg[c].bounds_check;
		f.warn_safety = cfg[c].warn_safety;
		for (size_t p = 0; p < sizeof(progs) / sizeof(progs[0]); p++) {
			unsigned need = progs[p].needs;
			if (((need & CR_NEED_ZI) && !cfg[c].zeroinit) ||
			    ((need & CR_NEED_NOSAFETY) && !cfg[c].warn_safety)) {
				gated++;
				continue;
			}
			st.cells++;
			PrismResult r = cm_txf(progs[p].code, f);
			if (!cm_ok(&r) || !r.output) {
				cm_note(&st, "cert-run reject cfg=%s prog=%zu", cfg[c].tag, p);
				prism_free(&r);
				continue;
			}
			if (cm_kw(r.output, "defer") || cm_kw(r.output, "orelse"))
				cm_note(&st, "cert-run keyword leak cfg=%s prog=%zu", cfg[c].tag, p);
			if (!cm_already_ran(r.output)) {
				char cname[128], rname[128];
				snprintf(cname, sizeof(cname),
					 "cert-run[%s/%zu]: emitted C compiles", cfg[c].tag, p);
				snprintf(rname, sizeof(rname),
					 "cert-run[%s/%zu]: emitted C returns 0", cfg[c].tag, p);
				check_transpiled_output_compiles_and_runs(r.output, cname, rname);
			} else {
				dedup++;
			}
			prism_free(&r);
		}
	}
	char name[256];
	snprintf(name, sizeof(name),
		 "completeness[gen/cert-compile-run]: %ld cells, %ld bad, %ld gated-out, "
		 "%ld dedup%s%s",
		 st.cells, st.bad, gated, dedup, st.bad ? " -- " : "", st.bad ? st.first : "");
	CHECK(st.bad == 0, name);
#undef CR_NEED_ZI
#undef CR_NEED_NOSAFETY
}
#endif /* !_WIN32 */

/*
 * `raw` keyword product (absorbs test.raw.c).
 *
 * SPEC Part II "The `raw` Keyword" is 3 constraints + 5 semantic clauses, and
 * every one of them is a product.  No oracle here names an expected emission —
 * that was test.raw.c's weakness (296 substring CHECKs that could not see a
 * wrong-but-plausible lowering):
 *
 *   O1 TOKEN     accepted output contains no `raw` word           (Semantics 1)
 *   O2 EQUIV     an all-`raw` declaration emits identically to the same
 *                declaration with the `raw` tokens deleted and zeroinit OFF.
 *                That IS the definition of "suppresses zero-initialization",
 *                stated without naming a single memset       (Semantics 1,2,4)
 *   O3 SPLIT     `T a, raw b;` suppresses exactly as much as `T a; raw T b;`
 *                — per-declarator form and split form must agree on the number
 *                of zeroing constructs emitted                    (Semantics 3)
 *   O4 COMPILES  the emitted C is accepted by the backend compiler
 *   O5 IDENT     `raw` bound as an ordinary identifier survives, and the
 *                emitted program still computes the right answer at runtime
 *                (the disambiguation torture, promoted from inspect to execute)
 */

#define RAW_PRE "struct RS { int a; int b[2]; }; typedef int rs_t;\n"

/* Count emitted zeroing constructs (all three lowering strategies). */
static int cm_zero_marks(const char *s) {
	int n = 0;
	for (const char *p = s; (p = strstr(p, "memset")) != NULL; p += 6) n++;
	for (const char *p = s; (p = strstr(p, "= {0}")) != NULL; p += 5) n++;
	for (const char *p = s; (p = strstr(p, "__prism_p_")) != NULL; p += 10) n++;
	return n;
}

/* Delete every whole-word `raw` token (and one following space) from src. */
static void cm_strip_raw(const char *src, char *out, size_t cap) {
	size_t o = 0;
	for (const char *p = src; *p && o + 1 < cap;) {
		int lb = (p == src) || (!isalnum((unsigned char)p[-1]) && p[-1] != '_');
		if (lb && strncmp(p, "raw", 3) == 0 && !isalnum((unsigned char)p[3]) && p[3] != '_') {
			p += 3;
			while (*p == ' ') p++;
			continue;
		}
		out[o++] = *p++;
	}
	out[o] = '\0';
}

static void cm_gen_raw_product(void) {
	/* Declaration shapes: type specifier + declarator.  `n` is a parameter,
	 * so the VLA row is the only variably-modified cell and nothing else in
	 * the TU is a zero-init candidate — which is what makes O2 exact. */
	static const struct {
		const char *ty, *decl;
	} shapes[] = {
	    {"int", "v"},        {"int", "v[4]"},      {"int", "v[2][3]"},
	    {"int *", "v"},      {"char", "v[8]"},     {"long long", "v"},
	    {"double", "v"},     {"struct RS", "v"},   {"struct RS", "v[2]"},
	    {"rs_t", "v"},       {"rs_t", "v[3]"},     {"int", "v[n]"},
	    {"unsigned", "v"},   {"int *", "v[2]"},    {"struct RS *", "v"},
	};
	/* Storage/qualifier prefixes that may sit alongside `raw`. */
	static const char *stor[] = {"", "static ", "register ", "const "};
	/* Placements: %1$s storage, %2$s type, %3$s declarator.  Every one of
	 * these is a legal position per the SPEC grammar; ill-formed positions
	 * (`int raw v;`) are deliberately absent — asserting a verdict there
	 * would be a hand-written expectation, which is what this replaces. */
	static const char *place[] = {
	    "%sraw %s %s;",                              /* raw before type      */
	    "raw %s%s %s;",                              /* raw before storage   */
	    "raw raw %s%s %s;",                          /* consecutive absorb   */
	    /* Consecutive raws with NOISE between them — comments and both
	     * attribute spellings.  emit_noise_between_raws has a balanced-group
	     * arm that only these reach (12 lines the A/B coverage diff showed
	     * the deleted test.raw.c was the sole reader of). */
	    "raw /*n*/ raw %s%s %s;",
	    "raw __attribute__((unused)) raw %s%s %s;",
	    "raw [[maybe_unused]] raw %s%s %s;",
	    "__attribute__((unused)) %sraw %s %s;",      /* GNU attr + raw       */
	    "[[maybe_unused]] %sraw %s %s;",             /* C23 attr + raw       */
	};
	/* Planting sites.  A one-site product is exactly the shape that misses
	 * this project's signature bug class ("the context nobody enumerated"),
	 * and the deleted test.raw.c pinned defects at file scope, in struct
	 * bodies, in cast/return-type neighbourhoods and after labels.  The
	 * differential oracle transfers to every site unchanged, so sites are a
	 * table, not new logic. */
	static const struct {
		const char *name, *tmpl;
		int allows_vm; /* variably-modified declarations legal here? */
	} sites[] = {
	    {"block", "void rawf(int n) { (void)n; %s (void)sizeof(v); }\n", 1},
	    {"inner-block", "void rawf(int n) { (void)n; { %s (void)sizeof(v); } }\n", 1},
	    {"file-scope", "%s\nvoid rawf(int n) { (void)n; (void)sizeof(v); }\n", 0},
	    {"struct-body", "struct SM { %s };\nvoid rawf(int n) { (void)n; }\n", 0},
	    {"switch-body",
	     "void rawf(int n) { switch (n) { default: { %s (void)sizeof(v); } } }\n", 1},
	    {"after-label",
	     "void rawf(int n) { (void)n; goto L; L: { %s (void)sizeof(v); } }\n", 1},
	    {"nonvoid-return",
	     "int rawf(int n) { (void)n; %s (void)sizeof(v); return 0; }\n", 1},
	    {"pp-adjacent",
	     "#define RAWM 1\nvoid rawf(int n) { (void)n; %s (void)sizeof(v); (void)RAWM; }\n", 1},
	    /* Return-type neighbourhood: emit_ret_type_tokens / emit_type_range
	     * each carry a raw-strip arm nothing else reaches. */
	    {"before-fn-def", "%s\nint rawg(void) { return 0; }\nvoid rawf(int n) { (void)n; }\n", 0},
	    /* Statement-level braces after the declaration drive
	     * emit_expr_to_semicolon's brace-depth tracking. */
	    {"brace-init-neighbour",
	     "void rawf(int n) { (void)n; %s (void)sizeof(v); int bi[2] = {1, 2}; (void)bi; }\n", 1},
	};
	CmStats st = {0};
	long gated = 0;
	char src[1400], stripped[1400], decl[256];
	for (size_t si = 0; si < sizeof(sites) / sizeof(sites[0]); si++)
	for (size_t pl = 0; pl < sizeof(place) / sizeof(place[0]); pl++)
		for (size_t sh = 0; sh < sizeof(shapes) / sizeof(shapes[0]); sh++)
			for (size_t so = 0; so < sizeof(stor) / sizeof(stor[0]); so++) {
				/* register arrays and const-without-init are not the
				 * subject under test; skip rather than pin a verdict. */
				int is_reg = strcmp(stor[so], "register ") == 0;
				int is_const = strcmp(stor[so], "const ") == 0;
				int is_vla = strstr(shapes[sh].decl, "[n]") != NULL;
				if ((is_reg || is_const) && strchr(shapes[sh].decl, '['))
					{ gated++; continue; }
				if (is_reg && is_vla) { gated++; continue; }
				if (strcmp(place[pl], "raw %s%s %s;") == 0 && !stor[so][0])
					{ gated++; continue; } /* duplicate of placement 0 */

				if (!sites[si].allows_vm && is_vla) { gated++; continue; }
				/* struct members take no storage class */
				if (strcmp(sites[si].name, "struct-body") == 0 && stor[so][0] &&
				    strcmp(stor[so], "const ") != 0) { gated++; continue; }
				if (strcmp(sites[si].name, "file-scope") == 0 &&
				    strcmp(stor[so], "register ") == 0) { gated++; continue; }

				snprintf(decl, sizeof(decl), place[pl], stor[so], shapes[sh].ty,
					 shapes[sh].decl);
				char body[900];
				snprintf(body, sizeof(body), sites[si].tmpl, decl);
				snprintf(src, sizeof(src), RAW_PRE "%s", body);
				st.cells++;

				/* The oracle is SYMMETRIC: whatever prism decides about the
				 * raw form, it must decide identically about the same source
				 * with `raw` deleted and zeroinit off.  That keeps every site
				 * honest without this suite guessing a verdict — a rejection
				 * is only a finding when the two pipelines disagree. */
				cm_strip_raw(src, stripped, sizeof(stripped));
				PrismFeatures zoff = prism_defaults();
				zoff.zeroinit = false;
				PrismResult r = cm_tx(src);
				PrismResult ref = cm_txf(stripped, zoff);
				if (cm_ok(&r) != cm_ok(&ref)) {
					cm_note(&st, "raw accept/reject disagreement [%s] %s",
						sites[si].name, decl);
				} else if (cm_ok(&r)) {
					if (!r.output || cm_kw(r.output, "raw")) /* O1 */
						cm_note(&st, "raw token survives [%s] %s",
							sites[si].name, decl);
					else if (!ref.output || !cx_norm_equal(r.output, ref.output))
						cm_note(&st, /* O2 */
							"raw != zeroinit-off equivalence [%s] %s",
							sites[si].name, decl);
				}
				prism_free(&ref);
				prism_free(&r);
			}

	/* O3: per-declarator `raw` suppresses exactly the declarators it marks. */
	for (size_t sh = 0; sh < sizeof(shapes) / sizeof(shapes[0]); sh++) {
		if (strstr(shapes[sh].decl, "[n]")) { gated++; continue; } /* VM split */
		char mixed[1024], split[1024];
		snprintf(mixed, sizeof(mixed),
			 RAW_PRE "void rawf(int n) { (void)n; %s a, raw %s; "
				 "(void)sizeof(a); (void)sizeof(v); }\n",
			 shapes[sh].ty, shapes[sh].decl);
		snprintf(split, sizeof(split),
			 RAW_PRE "void rawf(int n) { (void)n; %s a; raw %s %s; "
				 "(void)sizeof(a); (void)sizeof(v); }\n",
			 shapes[sh].ty, shapes[sh].ty, shapes[sh].decl);
		st.cells++;
		PrismResult rm = cm_tx(mixed), rs = cm_tx(split);
		if (!cm_ok(&rm) || !rm.output || !cm_ok(&rs) || !rs.output) {
			/* A rejected split is a documented multi-declarator constraint,
			 * not a raw defect — only flag a one-sided rejection. */
			if (cm_ok(&rm) != cm_ok(&rs))
				cm_note(&st, "raw split/mixed accept disagreement: %s %s",
					shapes[sh].ty, shapes[sh].decl);
		} else if (cm_zero_marks(rm.output) != cm_zero_marks(rs.output)) {
			cm_note(&st, "raw per-declarator != split (%d vs %d): %s %s",
				cm_zero_marks(rm.output), cm_zero_marks(rs.output),
				shapes[sh].ty, shapes[sh].decl);
		}
		prism_free(&rm);
		prism_free(&rs);
	}

	/* O4: `raw` on a FUNCTION DEFINITION.  Every placement above ends in `;`,
	 * so the product structurally could not generate this — and the A/B
	 * coverage diff showed the try_strip_raw arms in emit_type_range and
	 * emit_ret_type_tokens had no other reader once test.raw.c was retired.
	 * Same differential oracle: with every `raw` deleted and zeroinit off, the
	 * emission must be identical. */
	{
		static const struct {
			const char *open, *close, *ret, *decls;
		} fns[] = {
		    {"int ", "(void)", "1", ""},
		    {"int *", "(void)", "rp_a", "static int rp_a[4];"},
		    {"int (*", "(void))[4]", "&rp_a", "static int rp_a[4];"},
		    {"void ", "(void)", "", ""},
		    {"rp_t ", "(void)", "(rp_t){0}", "typedef struct { int x; } rp_t;"},
		};
		static const char *bodies[] = {
		    "", "rp_h();", "defer rp_h();", "defer { rp_h(); } rp_h();",
		    "int lv; (void)lv;", "raw int lv; (void)lv;",
		};
		/* Leading specifier prefixes, paired with the SAME prefix minus its
		 * `raw` tokens.  A function is not an object, so `raw` on a function
		 * definition must be a pure no-op — that is the property, and it is
		 * checked at identical features.  (Comparing against zeroinit-off, as
		 * the declaration tiers do, would be wrong here: it would also
		 * suppress zero-init of the body's own locals.) */
		static const struct {
			const char *with, *without;
		} pre[] = {
		    {"raw ", ""},
		    {"raw raw ", ""},
		    {"__attribute__((unused)) raw ", "__attribute__((unused)) "},
		    {"raw static ", "static "},
		    {"static raw ", "static "},
		    {"raw inline ", "inline "},
		    {"[[maybe_unused]] raw ", "[[maybe_unused]] "},
		};
		for (size_t f = 0; f < sizeof(fns) / sizeof(fns[0]); f++)
			for (size_t b = 0; b < sizeof(bodies) / sizeof(bodies[0]); b++)
				for (size_t q = 0; q < sizeof(pre) / sizeof(pre[0]); q++) {
					char fsrc[1024], fref[1024];
					const char *fmt = "void rp_h(void);\n%s\n%s%srp_fn%s { %s %s%s%s }\n";
					snprintf(fsrc, sizeof(fsrc), fmt, fns[f].decls, pre[q].with,
						 fns[f].open, fns[f].close, bodies[b],
						 fns[f].ret[0] ? "return " : "", fns[f].ret,
						 fns[f].ret[0] ? ";" : "");
					snprintf(fref, sizeof(fref), fmt, fns[f].decls, pre[q].without,
						 fns[f].open, fns[f].close, bodies[b],
						 fns[f].ret[0] ? "return " : "", fns[f].ret,
						 fns[f].ret[0] ? ";" : "");
					st.cells++;
					PrismResult ra = cm_tx(fsrc);
					PrismResult rb = cm_tx(fref);
					if (cm_ok(&ra) != cm_ok(&rb))
						cm_note(&st,
							"raw-fn accept/reject disagreement f=%zu b=%zu q=%zu",
							f, b, q);
					else if (cm_ok(&ra)) {
						if (!ra.output || cm_kw(ra.output, "raw"))
							cm_note(&st, "raw-fn token survives f=%zu b=%zu q=%zu",
								f, b, q);
						else if (!rb.output ||
							 !cx_norm_equal(ra.output, rb.output))
							cm_note(&st,
								"raw on a function definition is not a no-op f=%zu b=%zu q=%zu",
								f, b, q);
					}
					prism_free(&ra);
					prism_free(&rb);
				}
	}

	char name[320];
	snprintf(name, sizeof(name), "completeness[gen/raw-product]: %ld cells, %ld bad, %ld gated-out%s%s",
		 st.cells, st.bad, gated, st.bad ? " -- " : "", st.bad ? st.first : "");
	CHECK(st.bad == 0, name);
}

/*
 * `raw` as an ordinary identifier, executed (absorbs test.raw.c's
 * disambiguation torture).  Each program returns 0 iff its arithmetic held, so
 * the oracle is the program's own exit status — not a substring of the output.
 */
/*
 * defer x complex return type x return-expression shape.
 *
 * emit_ret_type synthesizes a `typedef` for a return type that has a suffix
 * (an array- or function-returning function), so that the defer lowering can
 * name the captured return value.  The A/B coverage diff after retiring
 * test.raw.c / test.cert.c showed those synthesis lines had exactly one reader
 * in the whole tree, and it was a single hand-written pin
 * (test_raw_return_type_defer_leak).  A product is the durable form.
 *
 * Oracle: accept, no keyword survives, and the emitted C compiles — a bad
 * typedef synthesis produces C the backend rejects, which is precisely what a
 * substring assertion cannot see.
 */
#ifndef _WIN32
/* The C compiler every cell shells out to.
 *
 * These were hardcoded to `cc`, which pins the whole executed half of the
 * suite to whatever the platform calls its default compiler. That hides an
 * entire class of defect: Prism emits C, and C that only one compiler accepts
 * is still a Prism bug. Honouring $CC lets the same 1,000 executed cells be
 * replayed against clang, a cross toolchain, or an older gcc, with no source
 * change. shell_word_ok keeps a hostile $CC out of the command line. */
static const char *cm_cc(void) {
	const char *cc = getenv("CC");
	return shell_word_ok(cc) ? cc : "cc";
}

static int cm_cc_accepts(const char *code);
#endif

static void cm_gen_defer_ret_shape(void) {
	static const struct {
		const char *proto_open, *proto_close, *ret_expr, *decls;
	} rets[] = {
	    {"int (*", "(void))[4]", "&drs_a", "static int drs_a[4];"},
	    {"int (*", "(void))[2][3]", "&drs_b", "static int drs_b[2][3];"},
	    {"void (*", "(void))(int)", "drs_f", "static void drs_f(int x){ (void)x; }"},
	    {"int *", "(void)", "drs_a", "static int drs_a[4];"},
	    {"int ", "(void)", "7", ""},
	    {"drs_t ", "(void)", "(drs_t){0}", "typedef struct { int x; } drs_t;"},
	    {"struct DRS ", "(void)", "(struct DRS){0}", "struct DRS { int x; };"},
	    /* Inline struct definition in the return type: SPEC makes this a hard
	     * error when a defer is present ("unresolvable return type"). Kept as
	     * a row precisely so the relational invariant below covers it. */
	    {"struct DRS2 { int x; } ", "(void)", "(struct DRS2){0}", ""},
	};
	static const char *exprs[] = {
	    "%s", "(%s)", "((%s))", "drs_id(%s)", "(1 ? (%s) : (%s))",
	};
	/* Defer bodies. Index 0 is the no-defer control. */
	static const char *defers[] = {
	    "", "defer drs_h();", "defer { drs_h(); }", "defer drs_h(); defer drs_h();",
	    "{ defer drs_h(); } defer drs_h();",
	};
#define DRS_NDEF ((int)(sizeof(defers) / sizeof(defers[0])))

	CmStats st = {0};
	char src[1200], expr[256];
	for (size_t r = 0; r < sizeof(rets) / sizeof(rets[0]); r++)
		for (size_t e = 0; e < sizeof(exprs) / sizeof(exprs[0]); e++) {
			snprintf(expr, sizeof(expr), exprs[e], rets[r].ret_expr, rets[r].ret_expr);
			int accept[DRS_NDEF], diag[DRS_NDEF];
			for (int d = 0; d < DRS_NDEF; d++) {
				snprintf(src, sizeof(src),
					 "void drs_h(void);\n%s\n"
					 "#define drs_id(x) (x)\n"
					 "%sdrs_fn%s { %s return %s; }\n",
					 rets[r].decls, rets[r].proto_open, rets[r].proto_close,
					 defers[d], expr);
				st.cells++;
				PrismResult res = cm_tx(src);
				accept[d] = cm_ok(&res) && res.output != NULL;
				diag[d] = (!cm_ok(&res)) && res.error_msg && res.error_msg[0];
				if (accept[d]) {
					if (cm_kw(res.output, "defer"))
						cm_note(&st, "defer-ret keyword leak r=%zu e=%zu d=%d",
							r, e, d);
#ifndef _WIN32
					else if (!cm_cc_accepts(res.output))
						cm_note(&st,
							"defer-ret emitted C rejected by cc r=%zu e=%zu d=%d",
							r, e, d);
#endif
				}
				prism_free(&res);
			}
			/* THE INVARIANT (relational — no hand-written verdicts):
			 * whether a defer can be lowered in this function is a property
			 * of the RETURN TYPE, not of the defer body.  So every non-empty
			 * defer body must reach the same accept/reject decision; and when
			 * they reject, the no-defer control must accept (proving the
			 * program is otherwise well-formed) and a diagnostic must be
			 * present (never a silent drop). */
			for (int d = 2; d < DRS_NDEF; d++)
				if (accept[d] != accept[1])
					cm_note(&st,
						"defer-ret verdict depends on defer BODY r=%zu e=%zu "
						"(d1=%d d%d=%d)",
						r, e, accept[1], d, accept[d]);
			if (!accept[1]) {
				if (!accept[0])
					cm_note(&st, "defer-ret control (no defer) rejected r=%zu e=%zu",
						r, e);
				if (!diag[1])
					cm_note(&st, "defer-ret reject without diagnostic r=%zu e=%zu", r,
						e);
			}
		}
#undef DRS_NDEF
	cm_report("gen/defer-ret-shape", &st);
}

#ifndef _WIN32
/*
 * Raw string literals — R"delim(...)delim" — the tokenizer feature that shares
 * a prefix with the `raw` keyword and nothing else (absorbs test.raw.c's
 * ~25 raw_string_* pins, including the 15/16-character delimiter boundary that
 * probes a fixed tokenizer buffer bound).
 *
 * Oracle: the emitted program compares the raw literal against a conventionally
 * escaped literal built independently by this generator, and returns 0 iff they
 * are equal.  A tokenizer that mis-scans the delimiter either fails to compile
 * or returns nonzero — no substring inspection involved.
 */
/* ── gen/source-defines ───────────────────────────────────────────────
 *
 * `collect_source_defines` re-emits the `#define`s it scrapes out of the
 * original source, because prism's output is post-`cc -E` and the directives
 * are otherwise gone. Getting that wrong is silent and serious in both
 * directions: scraping a `#define` that was never active resurrects
 * commented-out configuration into the build (test.api.c pins one such case
 * for `_FILE_OFFSET_BITS`), and dropping a live one changes what later
 * translation units see.
 *
 * The function is a five-state line scanner — continuation, block comment,
 * block comment opened *between* `#` and the directive name, multi-line raw
 * string, and `#if` nesting with branch accumulation — and it was 1.4% covered.
 * Hand-picked cells cannot close a five-state product, so this sweeps
 * context × directive spelling and asks one question of every cell.
 *
 * ORACLE — no expected strings, and no guess about any individual cell.
 *
 *     prism must preserve whether a macro is defined, wherever the plain
 *     preprocessor's own output preserves it.
 *
 * Three measurements per cell, all from the platform's C preprocessor with an
 * `#error` probe appended (definedness is read from the exit status, so no
 * output has to be captured or parsed):
 *
 *     ref  definedness at the end of the ORIGINAL source
 *     cc   definedness after re-preprocessing `cc -E original`
 *     got  definedness after re-preprocessing prism's output
 *
 * The `cc` leg is what keeps the tier honest. `cc -E` is not idempotent for
 * every input: given
 *
 *     int pad = 0; \
 *     #define X 1
 *
 * the backslash splices those into one logical line, so `X` is never defined —
 * but `cc -E` emits the `#define` as passthrough text on its own line, and its
 * own output re-preprocesses to *defined*. A tier that compared `got` against
 * `ref` alone would report that as a prism bug. It is not; prism inherits it.
 * So a cell where `got == cc` but both differ from `ref` is counted as
 * `inherited`, reported and not failed.
 *
 * Comparing against `cc` alone would be equally wrong in the other direction:
 * `cc -E` drops every `#define` it consumes, and re-emitting them is the whole
 * point of collect_source_defines, so prism is *supposed* to differ there.
 *
 * The oracle also survives guarded re-emission: when a define sits inside
 * `#ifdef __APPLE__`, prism re-emits the guard around it and re-preprocessing
 * gives the same answer on this platform as the original did. */

#define CSD_MACRO "CSD_T"

/* 1 = macro defined at end of `code`, 0 = not defined, -1 = infrastructure. */
static int csd_defined(const char *code) {
	static const char *probe = "\n#ifndef " CSD_MACRO "\n#error csd_undefined\n#endif\n";
	size_t n = strlen(code) + strlen(probe) + 1;
	char *buf = malloc(n);
	if (!buf) return -1;
	snprintf(buf, n, "%s%s", code, probe);
	char *path = create_temp_file(buf);
	free(buf);
	if (!path) return -1;
	char cmd[PATH_MAX + 96];
	/* -w: GCC rejects clang's -Wno-everything, and a rejected flag would make
	 * every cell read as "undefined" and the tier vacuously green. */
	snprintf(cmd, sizeof(cmd), "%s -std=gnu11 -E -w %s >/dev/null 2>&1", cm_cc(), path);
	int rc = run_command_status(cmd);
	unlink(path);
	free(path);
	if (rc < 0) return -1;
	return rc == 0 ? 1 : 0;
}

/* Is `code` valid C at all? `cc -E` succeeding proves nothing: the
 * preprocessor accepts text the compiler rejects, and a cell built from such
 * text is not a legitimate input to judge prism against. The splice contexts
 * below produce exactly that - `int pad = 0; \\` followed by `#define X 1`
 * splices into an expression statement containing `#`, which gcc reports as
 * "stray '#' in program". prism rejecting it is correct. */
static int csd_compiles(const char *code) {
	char *path = create_temp_file(code);
	if (!path) return -1;
	char cmd[PATH_MAX + 96];
	snprintf(cmd, sizeof(cmd), "%s -std=gnu11 -fsyntax-only -w %s >/dev/null 2>&1", cm_cc(), path);
	int rc = run_command_status(cmd);
	unlink(path);
	free(path);
	if (rc < 0) return -1;
	return rc == 0 ? 1 : 0;
}

/* Definedness after `cc -E` has had the source once and its output is fed back
 * in. Isolates the preprocessor's own non-idempotence from prism's behaviour. */
static int csd_cc_roundtrip(const char *code) {
	char *in = create_temp_file(code);
	if (!in) return -1;
	char *out = create_temp_file("");
	if (!out) {
		unlink(in);
		free(in);
		return -1;
	}
	char cmd[2 * PATH_MAX + 96];
	snprintf(cmd, sizeof(cmd), "%s -std=gnu11 -E -w %s -o %s 2>/dev/null", cm_cc(), in, out);
	int rc = run_command_status(cmd);
	unlink(in);
	free(in);
	if (rc != 0) {
		unlink(out);
		free(out);
		return -1;
	}
	FILE *f = fopen(out, "r");
	if (!f) {
		unlink(out);
		free(out);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = (n >= 0) ? malloc((size_t)n + 1) : NULL;
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	if (buf) buf[n] = '\0';
	fclose(f);
	unlink(out);
	free(out);
	if (!buf) return -1;
	int r = csd_defined(buf);
	free(buf);
	return r;
}

static void cm_gen_source_defines(void) {
	/* Where the directive sits. %s is the directive spelling. Each entry
	 * drives one state of the scanner; the trailing group exercises states
	 * that only arise from a *previous* line's parse. */
	static const char *contexts[] = {
	    "%s\n",                                              /* active code           */
	    "/* %s */\n",                                        /* inside one-line block */
	    "/* opening\n%s\n*/\n",                              /* inside multi-line     */
	    "/* opening\n * starred\n%s\n*/\n",                  /* starred continuation  */
	    "#if 0\n%s\n#endif\n",                               /* dead branch           */
	    "#if 1\n%s\n#endif\n",                               /* live branch           */
	    "#ifdef CSD_ABSENT\n%s\n#endif\n",                   /* ifdef false           */
	    "#ifndef CSD_ABSENT\n%s\n#endif\n",                  /* ifndef true           */
	    "#if 0\n#else\n%s\n#endif\n",                        /* else branch, live     */
	    "#if 1\n#else\n%s\n#endif\n",                        /* else branch, dead     */
	    "#if 0\n#elif 1\n%s\n#endif\n",                      /* elif branch, live     */
	    "#if 1\n#if 1\n%s\n#endif\n#endif\n",                /* nested, both live     */
	    "#if 1\n#if 0\n%s\n#endif\n#endif\n",                /* nested, inner dead    */
	    "#if 0\n#if 1\n%s\n#endif\n#endif\n",                /* nested, outer dead    */
	    "// %s\n",                                           /* line comment          */
	    "int csd_pad = 0; \\\n%s\n",                         /* spliced onto prev line*/
	    "#if defined(CSD_ABSENT) && \\\n    defined(CSD_X)\n%s\n#endif\n", /* multi-line cond */
	    "#define CSD_OTHER 1\n%s\n",                         /* after another define  */
	    "/* c */ %s\n",                                      /* comment then directive*/
	    "#include <stddef.h>\n%s\n",                         /* AFTER a top-level include */
	    "#if 0\n#include <stddef.h>\n#endif\n%s\n",          /* after a dead include   */
	};
	/* Index of the context whose define sits after a top-level #include.
	 * collect_source_defines deliberately stops there, because the collected
	 * defines are emitted above the re-emitted includes and hoisting a later
	 * define would change what it means. Counted, not failed. */
	const size_t post_include_ctx = sizeof(contexts) / sizeof(contexts[0]) - 2;
	/* How the directive is spelled. All of these define CSD_T. */
	static const char *spellings[] = {
	    "#define " CSD_MACRO " 1",
	    "#  define " CSD_MACRO " 1",
	    "#\tdefine " CSD_MACRO " 1",
	    "#define " CSD_MACRO,
	    "#define " CSD_MACRO "(a) (a)",
	    "#define " CSD_MACRO " 1 /* trailing */",
	    "#define " CSD_MACRO " (1 + \\\n                   1)",
	};

	CmStats st = {0};
	long infra = 0, skipped = 0, inherited = 0, hoist_boundary = 0;
	char src[2048], directive[256];

	for (size_t c = 0; c < sizeof(contexts) / sizeof(contexts[0]); c++)
		for (size_t d = 0; d < sizeof(spellings) / sizeof(spellings[0]); d++) {
			/* A directive whose own value continues onto the next line
			 * cannot be planted inside a one-line comment or a splice
			 * without changing which construct is under test. */
			bool multiline_dir = strstr(spellings[d], "\\\n") != NULL;
			bool one_line_ctx = strstr(contexts[c], "// %s") || strstr(contexts[c], "/* %s */");
			if (multiline_dir && one_line_ctx) {
				skipped++;
				continue;
			}
			snprintf(directive, sizeof(directive), "%s", spellings[d]);
			int len = snprintf(src, sizeof(src), contexts[c], directive);
			if (len < 0 || (size_t)len >= sizeof(src)) {
				skipped++;
				continue;
			}
			/* Give every cell a body so the output is a real TU. */
			size_t sl = strlen(src);
			snprintf(src + sl, sizeof(src) - sl, "int csd_main(void) { return 0; }\n");

			/* Only legitimate inputs are judged. */
			int valid = csd_compiles(src);
			if (valid < 0) {
				infra++;
				continue;
			}
			if (!valid) {
				skipped++;
				continue;
			}
			int ref = csd_defined(src);
			if (ref < 0) {
				infra++;
				continue;
			}
			/* What the bare preprocessor's own output preserves. */
			int cc_ref = csd_cc_roundtrip(src);
			if (cc_ref < 0) {
				infra++;
				continue;
			}

			char *path = create_temp_file(src);
			if (!path) {
				infra++;
				continue;
			}
			PrismFeatures feat = prism_defaults();
			feat.flatten_headers = false; /* the collect_source_defines path */
			PrismResult r = prism_transpile_file(path, feat);
			unlink(path);
			free(path);

			if (r.status != PRISM_OK || !r.output) {
				/* The input compiles, so a rejection is a finding. */
				cm_note(&st, "ctx%zu/spell%zu: prism rejected a TU that cc compiles", c, d);
				st.cells++;
				prism_free(&r);
				continue;
			}
			int got = csd_defined(r.output);
			prism_free(&r);
			st.cells++;
			if (got < 0) {
				infra++;
				continue;
			}
			if (got == ref) continue;
			if (c == post_include_ctx) {
				/* Documented hoist boundary, not a defect. */
				hoist_boundary++;
				continue;
			}
			if (got == cc_ref) {
				/* cc -E's own output already disagrees with the
				 * source the same way; prism only passed it on. */
				inherited++;
				continue;
			}
			cm_note(&st,
				"ctx%zu/spell%zu: " CSD_MACRO " defined=%d in source, %d after cc -E, "
				"%d after transpile",
				c, d, ref, cc_ref, got);
		}

	char name[384];
	snprintf(name, sizeof(name),
		 "completeness[gen/source-defines]: %ld cells, %ld bad, %ld inherited-from-cc, "
		 "%ld past-hoist-boundary, %ld skipped, %ld infra%s%s",
		 st.cells, st.bad, inherited, hoist_boundary, skipped, infra,
		 st.bad ? " -- " : "", st.bad ? st.first : "");
	CHECK(st.bad == 0, name);
	/* An all-infrastructure run must not read as coverage. */
	CHECK(st.cells > 0 && infra < st.cells, "completeness[gen/source-defines]: cells actually ran");
}

#undef CSD_MACRO

static void cm_gen_raw_string(void) {
	/* Contents chosen to break naive scanners: embedded quotes, backslashes,
	 * a false ')' + partial-delimiter ending, newlines, and the empty body. */
	static const char *contents[] = {
	    "",
	    "x",
	    "hello raw",
	    "with \"quotes\" inside",
	    "back\\slash and \\n literal",
	    "parens ( ) inside",
	    "false ending )D( still going",
	    "C:\\path\\to\\file",
	    "line1\nline2\nline3",
	    "{\"json\": [1, 2, 3]}",
	    "^[a-z]+\\d*$",
	    ")",
	    "))",
	};
	/* Delimiter lengths sweep the boundary: 0..16 (16 is the documented max). */
	static const char *delims[] = {
	    "", "D", "AB", "ABC", "ABCDEFGHIJKLMN",   /* 14 */
	    "ABCDEFGHIJKLMNO",                        /* 15 */
	    "ABCDEFGHIJKLMNOP",                       /* 16 */
	    "1234567890ABCDEF",                       /* 16, digits+letters */
	};
	CmStats st = {0};
	char raw_lit[512], esc_lit[512], src[1600];
	for (size_t d = 0; d < sizeof(delims) / sizeof(delims[0]); d++)
		for (size_t c = 0; c < sizeof(contents) / sizeof(contents[0]); c++) {
			const char *body = contents[c];
			/* A raw literal cannot contain its own ")delim" terminator. */
			char term[32];
			snprintf(term, sizeof(term), ")%s\"", delims[d]);
			if (strstr(body, term)) continue;

			snprintf(raw_lit, sizeof(raw_lit), "R\"%s(%s)%s\"", delims[d], body,
				 delims[d]);
			/* Conventional escaping of the same bytes, built here so the
			 * comparison never consults the tokenizer under test. */
			size_t o = 0;
			esc_lit[o++] = '"';
			for (const char *p = body; *p && o + 5 < sizeof(esc_lit); p++) {
				if (*p == '"' || *p == '\\') {
					esc_lit[o++] = '\\';
					esc_lit[o++] = *p;
				} else if (*p == '\n') {
					esc_lit[o++] = '\\';
					esc_lit[o++] = 'n';
				} else {
					esc_lit[o++] = *p;
				}
			}
			esc_lit[o++] = '"';
			esc_lit[o] = '\0';

			snprintf(src, sizeof(src),
				 "int rs_cmp(const char *a, const char *b){ while (*a && *a == *b)"
				 " { a++; b++; } return *a - *b; }\n"
				 "int main(void) { const char *r = %s; const char *e = %s;\n"
				 "  return rs_cmp(r, e) == 0 ? 0 : 1; }\n",
				 raw_lit, esc_lit);
			st.cells++;
			PrismResult r = cm_tx(src);
			if (!cm_ok(&r) || !r.output) {
				cm_note(&st, "raw-string reject d=%zu c=%zu", d, c);
				prism_free(&r);
				continue;
			}
#ifndef _WIN32
			/* Prism recognises R"delim(...)delim" and hands it to the
			 * backend unchanged, so whether the emitted C compiles is a
			 * question about the backend, not about Prism. Raw string
			 * literals are a GNU extension in C: accepted by
			 * `gcc -std=gnu11`, rejected by `gcc -std=c11`, and rejected
			 * by clang in every mode tried. Asserting unconditionally
			 * here bakes a silent GCC dependency into 104 checks, which
			 * is what `CC=clang` exposed. Same discipline as
			 * gen/passthrough-equivalence: if the compiler will not take
			 * the construct, the cell says nothing about Prism. */
			if (!cm_already_ran(r.output) && cc_supports_raw_strings()) {
				char cn[160], rn[160];
				snprintf(cn, sizeof(cn), "raw-string[d%zu/c%zu]: emitted C compiles",
					 d, c);
				snprintf(rn, sizeof(rn),
					 "raw-string[d%zu/c%zu]: raw literal == escaped literal", d, c);
				check_transpiled_output_compiles_and_runs(r.output, cn, rn);
			}
#endif
			prism_free(&r);
		}
	cm_report("gen/raw-string", &st);
}

static void cm_gen_raw_identifier(void) {
	static const char *bodies[] = {
	    "int raw = 5; raw += 2; return raw == 7 ? 0 : 1;",
	    "int raw = 3; return raw * 2 == 6 ? 0 : 1;",
	    "int raw = 6; return (raw & 4) == 4 ? 0 : 1;",
	    "int raw = 1; return (raw || 0) ? 0 : 1;",
	    "int raw = 0; raw++; ++raw; return raw == 2 ? 0 : 1;",
	    "int raw[3] = {1,2,3}; return raw[2] == 3 ? 0 : 1;",
	    "struct { int raw; } s; s.raw = 9; return s.raw == 9 ? 0 : 1;",
	    "int raw = 4; return raw > 3 ? 0 : 1;",
	    "int raw = 2, other = 3; return raw + other == 5 ? 0 : 1;",
	    "int raw = 1; return raw ? 0 : 1;",
	    "int raw = 8; return (int)sizeof(raw) == (int)sizeof(int) ? 0 : 1;",
	    "int raw = 7; int *p = &raw; return *p == 7 ? 0 : 1;",
	    "int raw = 5; switch (raw) { case 5: return 0; default: return 1; }",
	    "int raw = 0; for (raw = 0; raw < 3; raw++) {} return raw == 3 ? 0 : 1;",
	    "int raw = 2; return (raw << 1) == 4 ? 0 : 1;",
	};
	CmStats st = {0};
	char src[512];
	for (size_t b = 0; b < sizeof(bodies) / sizeof(bodies[0]); b++) {
		snprintf(src, sizeof(src), "int main(void) { %s }\n", bodies[b]);
		st.cells++;
		PrismResult r = cm_tx(src);
		if (!cm_ok(&r) || !r.output) {
			cm_note(&st, "raw-ident reject b=%zu", b);
			prism_free(&r);
			continue;
		}
		char cname[160], rname[160];
		snprintf(cname, sizeof(cname), "raw-ident[%zu]: emitted C compiles", b);
		snprintf(rname, sizeof(rname), "raw-ident[%zu]: emitted C returns 0", b);
		check_transpiled_output_compiles_and_runs(r.output, cname, rname);
		prism_free(&r);
	}
	cm_report("gen/raw-identifier", &st);
}
#endif /* !_WIN32 */

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
	cm_gen_raw_product();
	cm_gen_defer_ret_shape();
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
	cm_gen_ice_stmt_expr();
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
/* ── compile oracle ───────────────────────────────────────────────────
 * Every other static hunter in this file checks output with a SUBSTRING
 * oracle (cm_has_bchk_wrap / cm_kw). That is structurally blind to the
 * recurring "accepted, but lowered to illegal C" class — illegal VLAs,
 * static VLAs, ternary-in-designator, memset-on-const, if(!array). Handing
 * the output to the backend is the only oracle that sees it.
 *
 * Returns 1 = cc accepts, 0 = cc rejects, -1 = infrastructure failure
 * (never conflate -1 with 0; a broken temp dir must not read as green).
 *
 * Uses run_command_status (posix_spawn + spawn mutex), never system(3):
 * this suite runs on a pthread beside ~20 others and fork-based spawning
 * from the multithreaded harness caused intermittent SIGBUS on macOS arm64.
 * Uses -w rather than -Wno-everything, which is clang-only (GCC CI would
 * reject the flag and turn the whole tier vacuous). */
static int cm_cc_accepts(const char *code) {
	char *path = create_temp_file(code);
	if (!path) return -1;
	char cmd[PATH_MAX + 96];
	snprintf(cmd, sizeof(cmd), "%s -std=gnu11 -fsyntax-only -w %s >/dev/null 2>&1", cm_cc(), path);
	int rc = run_command_status(cmd);
	unlink(path);
	free(path);
	if (rc < 0) return -1;
	return rc == 0 ? 1 : 0;
}

/* ── trap-aware execution ─────────────────────────────────────────────
 *
 * `cm_exec` reports a child's exit status through `run_command_status`, which
 * ends at `prism_spawn_wait`, which collapses every !WIFEXITED outcome to -1.
 * A program killed by a signal and a harness that failed to spawn anything are
 * therefore the same value. That is fine for tiers whose programs are meant to
 * exit cleanly, and useless for bounds checking, where trapping IS the
 * behaviour under test. It is why the bounds tiers had 15 executed cells
 * against 1,851 static ones.
 *
 * `cm_exec_trap` spawns the binary directly, with no intervening shell, and
 * keeps the raw wait status. No shell also means no 128+N remapping, so a
 * program that genuinely exits 133 stays distinguishable from one killed by
 * signal 5.
 *
 * Which signal a trap raises is a per-target detail, not a prism guarantee:
 * `__builtin_trap` lowers to `brk #1000` on aarch64 (SIGTRAP) and `ud2` on
 * x86-64 (SIGILL), and a target without a trap instruction may route through
 * abort (SIGABRT). The oracle therefore asks only whether the child died by
 * signal, and the tiers pair every trap cell with an in-bounds cell that must
 * exit 0 - which is what keeps "died by signal" from being satisfiable by an
 * unrelated crash. */
#define CM_X_TRAPPED (-3000) /* died by any signal */
#define CM_X_INFRA (-1000)   /* transpile failed */
#define CM_X_CCFAIL (-1001)  /* emitted C did not compile */
#define CM_X_TEMP (-1002)    /* could not create temp file */

static int cm_exec_trap(const char *src, PrismFeatures feat) {
	PrismResult r = cm_txf(src, feat);
	if (!cm_ok(&r) || !r.output) {
		prism_free(&r);
		return CM_X_INFRA;
	}
	char *path = create_temp_file(r.output);
	prism_free(&r);
	if (!path) return CM_X_TEMP;

	char bin[PATH_MAX];
	int fd = test_mkstemp(bin, "cm_trap_");
	if (fd < 0) {
		unlink(path);
		free(path);
		return CM_X_TEMP;
	}
	close(fd);
	unlink(bin);

	char cmd[PATH_MAX * 2 + 80];
	snprintf(cmd, sizeof(cmd), "%s -std=gnu11 -w -o %s %s >/dev/null 2>&1", cm_cc(), bin, path);
	if (run_command_status(cmd) != 0) {
		unlink(path);
		free(path);
		return CM_X_CCFAIL;
	}

	char *argv[] = {bin, NULL};
	int status = 0;
	int rc = prism_spawn_wait_raw(argv, "/dev/null", "/dev/null", &status);
	unlink(bin);
	unlink(path);
	free(path);

	if (rc != 0) return CM_X_TEMP;
	if (WIFSIGNALED(status)) return CM_X_TRAPPED;
	if (WIFEXITED(status)) return WEXITSTATUS(status);
	return CM_X_TEMP;
}

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
	snprintf(cmd, sizeof(cmd), "%s -std=gnu11 -o %s %s >/dev/null 2>&1", cm_cc(), bin, path);
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

/* ── gen/runtime-defer-product ────────────────────────────────────────
 *
 * The two existing executed cross-feature tiers (gen/runtime-defer,
 * gen/runtime-cross) are hand-written cells with the expected trace typed in
 * as a literal. That bounds what they can catch to what someone thought to
 * write down, and defer's hard part is exactly the combinatorial bit: which
 * scopes unwind, in what order, when a scope is left by something other than
 * falling off the end.
 *
 * This sweeps scope kind × inner defer count × outer defer count × exit path
 * and, for each cell, does two independent things with one structural model:
 *
 *   1. emits the C program, and
 *   2. simulates it to derive the expected trace.
 *
 * The oracle is the simulation, not a string a human chose. If prism unwinds
 * the wrong scopes, runs defers in the wrong order, or skips an iteration's
 * defers on `continue`, the emitted program's own trace disagrees and it exits
 * non-zero. Zero-init and an in-range bounds access ride along in every cell so
 * a cross-feature regression shows up here too rather than only in isolation.
 *
 * Traces use one char per event: 's' scope start, 'b' body, 'e' after the
 * construct, 'A'/'B' outer defers, 'a'/'b'... inner defers. Defers are LIFO, so
 * registering a then b yields "ba". */

enum { DP_MAX_D = 2 };

/* Inner construct kinds. */
enum { DPK_BLOCK, DPK_LOOP, DPK_SWITCH, DPK_IF, DPK_N };
/* How the inner scope is left. */
enum { DPX_FALL, DPX_BREAK, DPX_CONTINUE, DPX_RETURN, DPX_GOTO, DPX_N };

static const char *dp_kind_name(int k) {
	return k == DPK_BLOCK ? "block" : k == DPK_LOOP ? "loop" : k == DPK_SWITCH ? "switch" : "if";
}
static const char *dp_exit_name(int x) {
	return x == DPX_FALL	 ? "fall"
	       : x == DPX_BREAK	 ? "break"
	       : x == DPX_CONTINUE ? "continue"
	       : x == DPX_RETURN	 ? "return"
				 : "goto";
}

/* `break` needs a loop or a switch; `continue` needs a loop. */
static int dp_valid(int kind, int exit) {
	if (exit == DPX_BREAK) return kind == DPK_LOOP || kind == DPK_SWITCH;
	if (exit == DPX_CONTINUE) return kind == DPK_LOOP;
	return 1;
}

/* Append the inner defers in LIFO order: registered a,b -> runs b,a. */
static void dp_unwind_inner(char *t, size_t cap, int ndef) {
	size_t n = strlen(t);
	for (int i = ndef - 1; i >= 0 && n + 1 < cap; i--) t[n++] = (char)('a' + i);
	t[n] = '\0';
}
static void dp_unwind_outer(char *t, size_t cap, int ndef) {
	size_t n = strlen(t);
	for (int i = ndef - 1; i >= 0 && n + 1 < cap; i--) t[n++] = (char)('A' + i);
	t[n] = '\0';
}
static void dp_put(char *t, size_t cap, char c) {
	size_t n = strlen(t);
	if (n + 1 < cap) {
		t[n] = c;
		t[n + 1] = '\0';
	}
}

/* Derive the trace the program must produce. This is the oracle. */
static void dp_expect(char *t, size_t cap, int kind, int nin, int nout, int exit) {
	t[0] = '\0';
	dp_put(t, cap, 's');
	int iters = (kind == DPK_LOOP) ? 2 : 1;
	for (int it = 0; it < iters; it++) {
		dp_put(t, cap, 'b');
		dp_unwind_inner(t, cap, nin); /* scope left, whichever way */
		if (exit == DPX_RETURN || exit == DPX_GOTO) {
			/* leaves the construct and skips the trailing marker */
			dp_unwind_outer(t, cap, nout);
			return;
		}
		if (exit == DPX_BREAK) break;
		/* DPX_FALL and DPX_CONTINUE both proceed to the next iteration */
	}
	dp_put(t, cap, 'e');
	dp_unwind_outer(t, cap, nout);
}

/* Emit the program described by the same model. */
static void dp_emit(char *src, size_t cap, int kind, int nin, int nout, int exit) {
	char outer[128] = "", inner[128] = "", body[512], ctrl[640];
	for (int i = 0; i < nout; i++) {
		char one[32];
		snprintf(one, sizeof(one), "defer L(\"%c\"); ", (char)('A' + i));
		strncat(outer, one, sizeof(outer) - strlen(outer) - 1);
	}
	for (int i = 0; i < nin; i++) {
		char one[32];
		snprintf(one, sizeof(one), "defer L(\"%c\"); ", (char)('a' + i));
		strncat(inner, one, sizeof(inner) - strlen(inner) - 1);
	}
	const char *ex = exit == DPX_FALL	      ? ""
			 : exit == DPX_BREAK   ? "break;"
			 : exit == DPX_CONTINUE ? "continue;"
			 : exit == DPX_RETURN  ? "return 0;"
					      : "goto done;";
	/* Zero-init and an in-range bounds access ride along in every cell.
	 * `zi` must be 0 and `arr[2]` must be 3; either failing sets the flag. */
	snprintf(body, sizeof(body),
		 "%sint zi; int arr[4] = {1,2,3,4}; if (zi != 0 || arr[2] != 3) __bad = 1; L(\"b\"); %s",
		 inner, ex);
	switch (kind) {
	case DPK_LOOP:
		snprintf(ctrl, sizeof(ctrl), "for (int i = 0; i < 2; i++) { %s }", body);
		break;
	case DPK_SWITCH:
		snprintf(ctrl, sizeof(ctrl), "switch (1) { case 1: { %s } }", body);
		break;
	case DPK_IF:
		snprintf(ctrl, sizeof(ctrl), "if (1) { %s }", body);
		break;
	default:
		snprintf(ctrl, sizeof(ctrl), "{ %s }", body);
		break;
	}
	snprintf(src, cap,
		 CM_LOG_PRE "static int __bad;\n"
			    "static int f(void) { %sL(\"s\"); %s L(\"e\"); done: ; return 0; }\n"
			    "int main(void){ f(); return __bad || chk(\"%s\"); }\n",
		 outer, ctrl, "%s");
}

static void cm_gen_runtime_defer_product(void) {
	CmStats st = {0};
	long infra = 0;
	char src[2048], tmpl[2048], expect[64];

	for (int kind = 0; kind < DPK_N; kind++)
		for (int exit = 0; exit < DPX_N; exit++) {
			if (!dp_valid(kind, exit)) continue;
			for (int nin = 0; nin <= DP_MAX_D; nin++)
				for (int nout = 0; nout <= DP_MAX_D; nout++) {
					dp_expect(expect, sizeof(expect), kind, nin, nout, exit);
					dp_emit(tmpl, sizeof(tmpl), kind, nin, nout, exit);
					snprintf(src, sizeof(src), tmpl, expect);
					st.cells++;
					PrismFeatures f = prism_defaults();
					f.bounds_check = true;
					int ex = cm_exec(src, f);
					if (ex <= -1000) {
						infra++;
						cm_note(&st,
							"%s/%s in=%d out=%d: transpile or compile "
							"failed (%d)",
							dp_kind_name(kind), dp_exit_name(exit), nin,
							nout, ex);
						continue;
					}
					if (ex != 0)
						cm_note(&st, "%s/%s in=%d out=%d: expected trace %s",
							dp_kind_name(kind), dp_exit_name(exit), nin,
							nout, expect);
				}
		}

	char name[384];
	snprintf(name, sizeof(name),
		 "completeness[gen/runtime-defer-product]: %ld cells, %ld bad, %ld infra%s%s",
		 st.cells, st.bad, infra, st.bad ? " -- " : "", st.bad ? st.first : "");
	CHECK(st.bad == 0, name);
	CHECK(st.cells > 0, "completeness[gen/runtime-defer-product]: cells actually ran");
}

/* ── gen/runtime-orelse-product ───────────────────────────────────────
 *
 * `orelse` lowers to a ternary over its left-hand side, so the LHS appears
 * twice in the naive expansion and has to be hoisted into a temporary. That
 * makes double-evaluation the feature's signature defect — it is why
 * reject_orelse_side_effects exists at all — and it is invisible to any
 * substring oracle, because the emitted C looks perfectly reasonable while
 * calling the function twice.
 *
 * Every cell therefore *counts* evaluations at runtime rather than inspecting
 * the output:
 *
 *   the LHS must be evaluated exactly once, always;
 *   the fallback must be evaluated exactly once when the LHS is falsy and
 *   never when it is truthy;
 *   the resulting value must be the LHS when truthy and the fallback when not.
 *
 * All three expectations are derived from the cell's own axes, not typed in.
 * The sweep is LHS truthiness × declaration form × fallback shape × site, so a
 * hoist that is correct at statement level but wrong inside a loop or a switch
 * case — this project's recurring shape — shows up as a wrong count rather
 * than as output that merely looks odd. */

enum { OPT_TRUTHY_N = 2 };
/* Where the orelse sits. */
enum { OPS_BLOCK, OPS_LOOP, OPS_SWITCH, OPS_IF, OPS_N };
/* How the value is bound. */
enum { OPF_DECL, OPF_ASSIGN, OPF_N };
/* What the fallback is. */
enum { OPB_CONST, OPB_CALL, OPB_EXPR, OPB_N };

static const char *op_site_name(int s) {
	return s == OPS_BLOCK ? "block" : s == OPS_LOOP ? "loop" : s == OPS_SWITCH ? "switch" : "if";
}
static const char *op_bind_name(int f) { return f == OPF_DECL ? "decl" : "assign"; }
static const char *op_fb_name(int b) {
	return b == OPB_CONST ? "const" : b == OPB_CALL ? "call" : "expr";
}

/* The fallback's value and whether evaluating it calls the counted helper. */
static int op_fb_value(int b) { return b == OPB_CONST ? 11 : b == OPB_CALL ? 5 : 9; }
static int op_fb_counts(int b) { return b == OPB_CALL; }

static void cm_gen_orelse_product(void) {
	CmStats st = {0};
	long infra = 0;
	char src[2048];

	for (int truthy = 0; truthy < OPT_TRUTHY_N; truthy++)
		for (int site = 0; site < OPS_N; site++)
			for (int bind = 0; bind < OPF_N; bind++)
				for (int fb = 0; fb < OPB_N; fb++) {
					int lhs_val = truthy ? 7 : 0;
					const char *fb_txt = fb == OPB_CONST  ? "11"
							     : fb == OPB_CALL ? "fbc()"
									      : "(4 + 5)";
					/* A loop body runs twice, so every count doubles. */
					int iters = (site == OPS_LOOP) ? 2 : 1;
					int exp_val = truthy ? lhs_val : op_fb_value(fb);
					int exp_lc = 1 * iters;
					int exp_rc = (!truthy && op_fb_counts(fb)) ? iters : 0;

					char stmt[256];
					if (bind == OPF_DECL)
						snprintf(stmt, sizeof(stmt),
							 "int x = src() orelse %s; __val = x;", fb_txt);
					else
						snprintf(stmt, sizeof(stmt),
							 "int x; x = src() orelse %s; __val = x;",
							 fb_txt);

					char ctrl[512];
					switch (site) {
					case OPS_LOOP:
						snprintf(ctrl, sizeof(ctrl),
							 "for (int i = 0; i < 2; i++) { %s }", stmt);
						break;
					case OPS_SWITCH:
						snprintf(ctrl, sizeof(ctrl),
							 "switch (1) { case 1: { %s } }", stmt);
						break;
					case OPS_IF:
						snprintf(ctrl, sizeof(ctrl), "if (1) { %s }", stmt);
						break;
					default:
						snprintf(ctrl, sizeof(ctrl), "{ %s }", stmt);
						break;
					}

					snprintf(src, sizeof(src),
						 "static int __lc, __rc, __val;\n"
						 "static int src(void){ __lc++; return %d; }\n"
						 "static int fbc(void){ __rc++; return 5; }\n"
						 "static void t(void){ %s }\n"
						 "int main(void){ t(); return !(__val == %d && __lc == "
						 "%d && __rc == %d); }\n",
						 lhs_val, ctrl, exp_val, exp_lc, exp_rc);

					st.cells++;
					PrismFeatures f = prism_defaults();
					int ex = cm_exec(src, f);
					if (ex <= -1000) {
						infra++;
						cm_note(&st,
							"%s/%s/%s truthy=%d: transpile or compile "
							"failed (%d)",
							op_site_name(site), op_bind_name(bind),
							op_fb_name(fb), truthy, ex);
						continue;
					}
					if (ex != 0)
						cm_note(&st,
							"%s/%s/%s truthy=%d: expected val=%d "
							"lhs_evals=%d fb_evals=%d",
							op_site_name(site), op_bind_name(bind),
							op_fb_name(fb), truthy, exp_val, exp_lc,
							exp_rc);
				}

	char name[384];
	snprintf(name, sizeof(name),
		 "completeness[gen/runtime-orelse-product]: %ld cells, %ld bad, %ld infra%s%s",
		 st.cells, st.bad, infra, st.bad ? " -- " : "", st.bad ? st.first : "");
	CHECK(st.bad == 0, name);
	CHECK(st.cells > 0, "completeness[gen/runtime-orelse-product]: cells actually ran");
}

/* ── gen/runtime-orelse-defer ─────────────────────────────────────────
 *
 * The SPEC promise for an orelse action is that "all active defers run, just
 * like a normal return". That sentence puts the two heaviest machines in the
 * transpiler on the same statement: orelse has to hoist its left-hand side
 * into a temporary, and defer has to unwind the right set of scopes for
 * whichever control-flow edge the action takes.
 *
 * Neither existing executed tier crosses them. gen/runtime-defer sweeps defer
 * exits with no orelse; gen/runtime-orelse-product sweeps orelse values with no
 * defers; gen/runtime-cross has two hand-written cells that happen to use both.
 *
 * Each cell here plants defers at two nesting levels, an orelse whose action is
 * return / break / continue / goto, and a left-hand side that is falsy (action
 * fires) or truthy (it does not). The expected trace is simulated from the same
 * model that emits the program, so nothing is asserted by hand:
 *
 *   'p' reached the orelse, 'q' survived it, 'e' left the construct,
 *   'a','b' inner defers, 'A','B' outer defers — LIFO within a scope.
 *
 * A wrong answer here means either a scope was unwound that should not have
 * been, or one that should have been was skipped. */

enum { OD_MAX_D = 2 };
enum { ODS_BLOCK, ODS_LOOP, ODS_SWITCH, ODS_IF, ODS_N };
enum { ODA_RETURN, ODA_BREAK, ODA_CONTINUE, ODA_GOTO, ODA_N };

static const char *od_site_name(int s) {
	return s == ODS_BLOCK ? "block" : s == ODS_LOOP ? "loop" : s == ODS_SWITCH ? "switch" : "if";
}
static const char *od_act_name(int a) {
	return a == ODA_RETURN ? "return" : a == ODA_BREAK ? "break" : a == ODA_CONTINUE ? "continue" : "goto";
}
static int od_valid(int site, int act) {
	if (act == ODA_BREAK) return site == ODS_LOOP || site == ODS_SWITCH;
	if (act == ODA_CONTINUE) return site == ODS_LOOP;
	return 1;
}

static void od_put(char *t, size_t cap, char c) {
	size_t n = strlen(t);
	if (n + 1 < cap) {
		t[n] = c;
		t[n + 1] = '\0';
	}
}
static void od_unwind(char *t, size_t cap, int n, char base) {
	size_t k = strlen(t);
	for (int i = n - 1; i >= 0 && k + 1 < cap; i--) t[k++] = (char)(base + i);
	t[k] = '\0';
}

/* Simulate the cell. This is the oracle. */
static void od_expect(char *t, size_t cap, int site, int act, int nin, int nout, int truthy) {
	t[0] = '\0';
	od_put(t, cap, 's');
	int iters = (site == ODS_LOOP) ? 2 : 1;
	for (int it = 0; it < iters; it++) {
		od_put(t, cap, 'p');
		if (!truthy) {
			/* the action fires: the inner scope is left right here */
			od_unwind(t, cap, nin, 'a');
			if (act == ODA_RETURN || act == ODA_GOTO) {
				od_unwind(t, cap, nout, 'A');
				return; /* skips the trailing marker */
			}
			if (act == ODA_BREAK) break;
			continue; /* ODA_CONTINUE: next iteration */
		}
		/* truthy: the action does not fire, the scope ends normally */
		od_put(t, cap, 'q');
		od_unwind(t, cap, nin, 'a');
	}
	od_put(t, cap, 'e');
	od_unwind(t, cap, nout, 'A');
}

static void cm_gen_orelse_defer(void) {
	CmStats st = {0};
	long infra = 0;
	char src[2048], outer[128], inner[128], stmt[256], ctrl[640], expect[64];

	for (int site = 0; site < ODS_N; site++)
		for (int act = 0; act < ODA_N; act++) {
			if (!od_valid(site, act)) continue;
			for (int truthy = 0; truthy < 2; truthy++)
				for (int nin = 0; nin <= OD_MAX_D; nin++)
					for (int nout = 0; nout <= OD_MAX_D; nout++) {
						od_expect(expect, sizeof(expect), site, act, nin,
							  nout, truthy);
						outer[0] = inner[0] = '\0';
						for (int i = 0; i < nout; i++) {
							char one[32];
							snprintf(one, sizeof(one),
								 "defer L(\"%c\"); ", (char)('A' + i));
							strncat(outer, one,
								sizeof(outer) - strlen(outer) - 1);
						}
						for (int i = 0; i < nin; i++) {
							char one[32];
							snprintf(one, sizeof(one),
								 "defer L(\"%c\"); ", (char)('a' + i));
							strncat(inner, one,
								sizeof(inner) - strlen(inner) - 1);
						}
						const char *action =
						    act == ODA_RETURN     ? "return"
						    : act == ODA_BREAK    ? "break"
						    : act == ODA_CONTINUE ? "continue"
									  : "goto done";
						snprintf(stmt, sizeof(stmt),
							 "%sL(\"p\"); int x = src() orelse %s; (void)x; "
							 "L(\"q\");",
							 inner, action);
						switch (site) {
						case ODS_LOOP:
							snprintf(ctrl, sizeof(ctrl),
								 "for (int i = 0; i < 2; i++) { %s }",
								 stmt);
							break;
						case ODS_SWITCH:
							snprintf(ctrl, sizeof(ctrl),
								 "switch (1) { case 1: { %s } }", stmt);
							break;
						case ODS_IF:
							snprintf(ctrl, sizeof(ctrl), "if (1) { %s }",
								 stmt);
							break;
						default:
							snprintf(ctrl, sizeof(ctrl), "{ %s }", stmt);
							break;
						}
						snprintf(src, sizeof(src),
							 CM_LOG_PRE
							 "static int src(void){ return %d; }\n"
							 "static void t(void){ %sL(\"s\"); %s L(\"e\"); "
							 "done: ; }\n"
							 "int main(void){ t(); return chk(\"%s\"); }\n",
							 truthy ? 7 : 0, outer, ctrl, expect);

						st.cells++;
						PrismFeatures f = prism_defaults();
						int ex = cm_exec(src, f);
						if (ex <= -1000) {
							infra++;
							cm_note(&st,
								"%s/%s truthy=%d in=%d out=%d: "
								"transpile or compile failed (%d)",
								od_site_name(site), od_act_name(act),
								truthy, nin, nout, ex);
							continue;
						}
						if (ex != 0)
							cm_note(&st,
								"%s/%s truthy=%d in=%d out=%d: expected "
								"trace %s",
								od_site_name(site), od_act_name(act),
								truthy, nin, nout, expect);
					}
		}

	char name[384];
	snprintf(name, sizeof(name),
		 "completeness[gen/runtime-orelse-defer]: %ld cells, %ld bad, %ld infra%s%s", st.cells,
		 st.bad, infra, st.bad ? " -- " : "", st.bad ? st.first : "");
	CHECK(st.bad == 0, name);
	CHECK(st.cells > 0, "completeness[gen/runtime-orelse-defer]: cells actually ran");
}

/* ── gen/passthrough-equivalence ──────────────────────────────────────
 *
 * The headline claim is "drop-in overlay: use CC=prism in any build system".
 * That is a statement about code using *none* of prism's features, which is
 * the overwhelming majority of every real translation unit it will ever see.
 * Zero-init, bounds-check and auto-static are on by default, so prism rewrites
 * such files anyway: every declaration is a zero-init site, every subscript a
 * bounds site, every const array a promotion candidate.
 *
 * Nothing else in the tree tests that. The executed tiers all run programs
 * written to exercise defer/orelse/raw and self-check their own semantics, and
 * cm_cc_accepts only asks whether prism's output *compiles*. A transform that
 * changed what ordinary C computes while still emitting compilable output
 * would pass every existing oracle in the suite.
 *
 * ORACLE — differential against the unmodified compiler:
 *
 *     for well-defined C using no prism feature, compiling and running the
 *     ORIGINAL and compiling and running PRISM'S OUTPUT must agree exactly.
 *
 * Both legs use the same compiler on the same program, so nothing is asserted
 * about what the answer should be — only that prism did not change it. Every
 * cell is gated on the original building and running first, so a malformed
 * template is reported as infrastructure and never as a prism defect.
 *
 * Programs must be free of undefined behaviour: zero-init legitimately changes
 * the result of an uninitialised read, which is the feature working. Every
 * local here is initialised before use. */

/* Compile and run `code` with no prism involvement. Exit status, or <= -1000. */
static int cm_run_plain(const char *code) {
	char *path = create_temp_file(code);
	if (!path) return -1002;
	char bin[PATH_MAX];
	int fd = test_mkstemp(bin, "cm_plain_");
	if (fd < 0) {
		unlink(path);
		free(path);
		return -1002;
	}
	close(fd);
	unlink(bin);
	char cmd[PATH_MAX * 2 + 80];
	snprintf(cmd, sizeof(cmd), "%s -std=gnu11 -w -o %s %s >/dev/null 2>&1", cm_cc(), bin, path);
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

static void cm_gen_passthrough(void) {
	/* `pre` is emitted at file scope, `body` inside main, which must leave the
	 * answer in `r`. Recursion, function pointers and struct-by-value need a
	 * real function definition, and ISO C has no nested functions. */
	static const struct {
		const char *pre;
		const char *body;
	} cells[] = {
	    /* scalars and declarations */
	    {"", "int a = 5, b = 7; r = a + b;"},
	    {"", "int a = 40; { int a = 2; r = a; } r += 1;"},
	    {"", "long a = 100000L; r = (int)(a % 97);"},
	    {"", "unsigned u = 250u; r = (int)(u & 0x3f);"},
	    {"", "char c = 'A'; r = c - 'A' + 9;"},
	    {"", "double d = 2.5; r = (int)(d * 4);"},
	    {"", "const int k = 12; r = k;"},
	    {"", "static int s = 33; r = s;"},
	    {"", "volatile int v = 21; r = v;"},
	    {"", "_Bool f1 = 1; r = f1 ? 27 : 0;"},
	    /* arrays and subscripts — the bounds-check surface */
	    {"", "int a[4] = {1,2,3,4}; r = a[0]+a[1]+a[2]+a[3];"},
	    {"", "int a[4] = {1,2,3,4}; int i = 2; r = a[i] * 3;"},
	    {"", "int a[2][3] = {{1,2,3},{4,5,6}}; r = a[1][2] + a[0][0];"},
	    {"", "int a[5] = {0}; for (int i = 0; i < 5; i++) a[i] = i*i; r = a[4];"},
	    {"", "int a[4] = {9,8,7,6}; int *p = a; r = p[2] + *(p+1);"},
	    {"", "char s[8] = \"abcd\"; r = (int)s[3];"},
	    {"", "int a[4] = {[2] = 11}; r = a[2] + a[0];"},
	    /* const arrays — the auto-static promotion surface */
	    {"", "const int k[4] = {2,4,6,8}; r = k[0]+k[3];"},
	    {"", "const char msg[6] = \"hello\"; r = (int)msg[1];"},
	    {"", "static const int t[3] = {9,9,9}; r = t[0]+t[1];"},
	    /* VLAs — zero-init lowers these to a runtime memset */
	    {"", "int n = 4; int v[n]; for (int i=0;i<n;i++) v[i]=i+1; r = v[3]*5;"},
	    {"", "int n = 3; int v[n][2]; for(int i=0;i<n;i++){v[i][0]=i;v[i][1]=i*2;} r = v[2][1]*7;"},
	    /* structs and unions */
	    {"", "struct S { int x, y; }; struct S s = {3,4}; r = s.x * s.y;"},
	    {"", "struct S { int x; struct { int y; } in; }; struct S s = {1,{2}}; r = s.x + s.in.y * 10;"},
	    {"", "union U { int i; char c[4]; }; union U u; u.i = 0; u.c[0] = 7; r = u.c[0];"},
	    {"", "struct S { int a[3]; }; struct S s = {{5,6,7}}; r = s.a[1];"},
	    {"", "struct S { int x, y; }; struct S s = {.y = 8, .x = 1}; r = s.y - s.x;"},
	    {"", "struct S { int a, b; }; struct S arr[3] = {{1,2},{3,4},{5,6}}; r = arr[2].a + arr[0].b;"},
	    {"", "struct B { unsigned a : 3; unsigned b : 5; }; struct B x = {5, 9}; r = (int)(x.a + x.b);"},
	    {"", "struct S { int x; union { int a; int b; }; }; struct S s = {0}; s.a = 12; r = s.a;"},
	    /* pointers and declarator shapes */
	    {"", "int x = 17; int *p = &x; r = *p;"},
	    {"", "int x = 3; int *p = &x; int **q = &p; r = **q * 5;"},
	    {"", "int a[4] = {1,2,3,4}; int *p = a + 3; r = (int)(p - a) + *p;"},
	    {"", "int *const p = (int[1]){13}; r = *p;"},
	    {"", "int a[2] = {1,2}; int (*pa)[2] = &a; r = (*pa)[1] * 15;"},
	    {"", "int a[3] = {1,2,3}; int *p = (int[3]){7,8,9}; r = p[1] + a[0];"},
	    /* control flow */
	    {"", "int n = 0; for (int i = 0; i < 6; i++) n += i; r = n;"},
	    {"", "int n = 0, i = 0; while (i < 5) { n += 2; i++; } r = n;"},
	    {"", "int n = 0, i = 0; do { n += 3; i++; } while (i < 4); r = n;"},
	    {"", "int x = 2; switch (x) { case 1: r = 1; break; case 2: r = 22; break; default: r = 3; }"},
	    {"", "int x = 5; if (x > 3) r = 11; else r = 22;"},
	    {"", "int n = 0; for (int i=0;i<10;i++){ if (i==3) continue; if (i==7) break; n++; } r = n;"},
	    {"", "int n = 0; for (int i=0;i<3;i++) for (int j=0;j<3;j++) n++; r = n;"},
	    {"", "int i = 0, n = 0; goto mid; top: n += 5; mid: i++; if (i < 3) goto top; r = n;"},
	    /* typedef, enum, expressions */
	    {"", "typedef int myint; myint m = 14; r = m;"},
	    {"", "enum E { A = 2, B = 5 }; enum E e = B; r = (int)e * 3;"},
	    {"", "typedef struct { int a, b; } P; P p = {2,3}; r = p.a * p.b + 1;"},
	    {"", "typedef int arr3[3]; arr3 a = {4,5,6}; r = a[2] * 4;"},
	    {"", "int a = 1; typeof(a) b = 30; r = b;"},
	    {"", "int a = 6, b = 3; r = (a << 1) + (b >> 1) + (a % b) + (a / b);"},
	    {"", "int a = 1, b = 0; r = (a && !b) ? 31 : 0;"},
	    {"", "int a = 5; a += 3; a *= 2; a -= 4; r = a;"},
	    {"", "int a = 0; r = (a++, a += 9, a);"},
	    {"", "int a[3] = {1,2,3}; r = (int)(sizeof a / sizeof a[0]) * 7;"},
	    {"", "int a = 65; r = (int)(char)a - 40;"},
	    {"", "unsigned long long big = 1ULL << 40; r = (int)(big >> 38);"},
	    {"", "char *strs[3] = {\"aa\",\"bb\",\"cc\"}; r = (int)strs[1][0];"},
	    /* functions: recursion, by-value structs, function pointers */
	    {"static int fact(int n){ return n <= 1 ? 1 : n * fact(n-1); }", "r = fact(4);"},
	    {"struct S { int a, b; }; static int sv(struct S s){ return s.a + s.b; }",
	     "struct S q = {6,7}; r = sv(q);"},
	    {"static int add(int a, int b){ return a+b; }", "int (*fp)(int,int) = add; r = fp(20,3);"},
	    {"static int g1(void){return 1;} static int g2(void){return 2;}",
	     "int (*t[2])(void) = {g1,g2}; r = t[0]() + t[1]() * 10;"},
	    {"static int sum(int *a, int n){ int s=0; for(int i=0;i<n;i++) s+=a[i]; return s; }",
	     "int a[4] = {1,2,3,4}; r = sum(a, 4) * 4;"},
	    /* file-scope declarations */
	    {"static int gcount = 17;", "r = gcount;"},
	    {"static int gtab[4] = {1,2,3,4};", "r = gtab[2] + gtab[3];"},
	    {"static const char *gs = \"xyz\";", "r = (int)gs[2];"},
	    {"struct G { int a; int b[2]; }; static struct G gg = {1,{2,3}};", "r = gg.b[1] * 9;"},
	    {"static const int gk[3] = {5,10,15};", "r = gk[1] + gk[0];"},
	    /* preprocessor: every one of these is expanded by cc -E before Prism
	       sees a token, so a divergence means Prism disturbed the
	       preprocessed stream rather than mis-expanding anything */
	    {"#define K 23", "r = K;"},
	    {"#define ADD(a,b) ((a)+(b))", "r = ADD(20, 4);"},
	    {"#define SQ(x) ((x)*(x))", "int n = 5; r = SQ(n) - 100;"},
	    {"#define STR(x) #x", "const char *t = STR(abc); r = (int)t[1];"},
	    {"#define CAT(a,b) a##b", "int xy = 41; r = CAT(x,y);"},
	    {"#define VA(...) sum(0, __VA_ARGS__, -1)\nstatic int sum(int f, ...){(void)f;return 42;}",
	     "r = VA(1,2,3);"},
	    {"#define ON 1\n#if ON\n#define PICK 15\n#else\n#define PICK 99\n#endif", "r = PICK;"},
	    {"#define A0 7\n#define A1 A0\n#define A2 A1", "r = A2 * 3;"},
	    {"#define TMP 5\n#undef TMP\n#define TMP 9", "r = TMP;"},
	    {"#define EMPTY", "EMPTY r = 26; EMPTY"},

	    /* integer conversion and promotion: the corners where a stray cast or
	       a lost qualifier changes the answer without changing the shape */
	    {"", "signed char c = -3; r = (int)(c + 40);"},
	    {"", "unsigned char uc = 250; r = (int)(uc / 4);"},
	    {"", "short sh = -1; unsigned short ush = (unsigned short)sh; r = (int)(ush >> 9);"},
	    {"", "unsigned u = 3; int i = -1; r = ((long long)i < (long long)u) ? 12 : 34;"},
	    {"", "int i = -8; r = (int)((unsigned)i >> 28);"},
	    {"", "long long v = -5; r = (int)((v % 3) + 20);"},
	    {"", "unsigned u = 0u; u--; r = (int)(u & 0x1f);"},
	    {"", "int a = 1; r = (int)(sizeof(a + 1L) == sizeof(long) ? 19 : 3);"},
	    {"", "unsigned x = 0xFFu; r = (int)(x ^ 0xF0u);"},
	    {"", "int n = 1; r = (n << 5) | (n << 1);"},

	    /* floating point: same compiler and flags on both sides, so any
	       difference is Prism reshaping the expression */
	    {"", "float f = 1.5f; double d = 2.25; r = (int)(f * d * 8);"},
	    {"", "double d = 10.0/4.0; r = (int)(d * 10);"},
	    {"", "double d = -3.7; r = (int)(-d) + 20;"},
	    {"", "long double ld = 2.5L; r = (int)(ld * 10);"},

	    /* functions: by-value aggregates, varargs, pointers to functions,
	       and a static local that must persist across calls */
	    {"struct P { int a, b; };\nstatic int usep(struct P p){ return p.a * p.b; }",
	     "struct P p = {6,7}; r = usep(p);"},
	    {"struct P { int a, b; };\nstatic struct P mk(int v){ struct P p; p.a = v; p.b = v+1; return p; }",
	     "struct P p = mk(5); r = p.a + p.b;"},
	    {"#include <stdarg.h>\nstatic int vsum(int n, ...){ va_list ap; va_start(ap, n); int t=0;"
	     " for(int i=0;i<n;i++) t += va_arg(ap,int); va_end(ap); return t; }",
	     "r = vsum(4, 1, 2, 3, 4);"},
	    {"static int bump(void){ static int c = 10; c += 5; return c; }",
	     "bump(); bump(); r = bump();"},
	    {"static int f1(int x){return x+1;}\nstatic int f2(int x){return x*2;}",
	     "int (*tab[2])(int) = {f1, f2}; r = tab[0](9) + tab[1](6);"},
	    {"static int deep(int n){ return n ? deep(n-1) + 2 : 1; }", "r = deep(8);"},
	    {"static int apply(int (*g)(int), int v){ return g(v); }\nstatic int trip(int x){return x*3;}",
	     "r = apply(trip, 9);"},

	    /* aggregates Prism has to walk without rewriting */
	    {"struct FAM { int n; int d[]; };", "struct FAM *q = (struct FAM*)(int[4]){3,7,8,9};"
	     " r = q->d[0] + q->d[2];"},
	    {"", "struct S { int x; } s = (struct S){44}; r = s.x;"},
	    {"", "int *p = (int[3]){1,2,3}; r = p[0] + p[2] * 10;"},
	    {"struct N { struct { int a; struct { int b; } in; } o; };",
	     "struct N n = {{1,{2}}}; r = n.o.a + n.o.in.b * 25;"},
	    {"", "struct A { int v[2]; }; struct A a[2] = {{{1,2}},{{3,4}}}; r = a[1].v[0] + a[0].v[1];"},
	    {"union U { double d; long long l; };", "union U u; u.l = 0; u.d = 1.0; r = (u.l != 0) ? 37 : 1;"},
	    {"", "struct Q { char c; int i; double d; }; r = (int)(sizeof(struct Q) >= 16 ? 29 : 2);"},

	    /* qualifiers, alignment, and the sizeof/offsetof family */
	    {"#include <stddef.h>\nstruct O { char a; int b; };",
	     "r = (int)offsetof(struct O, b) + 20;"},
	    {"", "r = (int)_Alignof(double) + 18;"},
	    {"", "_Alignas(16) int a[4] = {1,2,3,4}; r = a[3] * 6;"},
	    {"static int rd(const int *restrict p){ return *p; }", "int x = 47; r = rd(&x);"},
	    {"static inline int inl(int x){ return x + 4; }", "r = inl(21);"},
	    {"", "volatile int v = 0; for (int i=0;i<5;i++) v += 3; r = v;"},

	    /* _Generic and statement expressions: both are erased or rewritten by
	       Prism's own machinery elsewhere, so passthrough must leave them be */
	    {"", "int i = 0; r = _Generic(i, int: 24, double: 1, default: 2);"},
	    {"", "double d = 0; r = _Generic(d, int: 1, double: 28, default: 2);"},
	    {"", "int a = 3; r = _Generic(&a, int*: 31, int: 1, default: 2);"},
	    {"", "r = ({ int t = 6; t * 7; });"},
	    {"", "int n = 2; r = ({ int acc = 0; for (int i=0;i<n;i++) acc += 10; acc + 5; });"},

	    /* control flow shapes that move the instruction pointer sideways */
	    {"", "int n = 0; int i = 0; sw: switch (i) { case 0: n += 1; case 1: n += 2; case 2: n += 4; }"
	     " if (++i < 3) goto sw; r = n;"},
	    {"", "int n = 0; for (int i=0;i<8;i++) { if (i&1) continue; if (i>5) break; n += i; } r = n + 20;"},
	    {"", "int i = 0, n = 0; while (1) { n++; if (n > 6) break; if (i++ > 100) break; } r = n * 3;"},
	    {"", "int n = 0; { { { n = 13; } } } r = n * 2;"},
	    {"", "int c = 0; for (int i=0;i<3;i++){ for (int j=0;j<3;j++){ if (j==2) goto out; c++; } } out: r = c + 30;"},

	    /* string and memory library calls: Prism must not disturb the
	       declarations these pull in from system headers */
	    {"#include <string.h>", "const char *t = \"prism\"; r = (int)strlen(t) * 8;"},
	    {"#include <string.h>", "char b[8]; memset(b, 0, sizeof b); memcpy(b, \"ab\", 2); r = b[0] + b[1];"},
	    {"#include <string.h>", "r = strcmp(\"abc\", \"abc\") == 0 ? 46 : 1;"},
	    {"#include <stdio.h>", "char b[16]; int k = snprintf(b, sizeof b, \"%d\", 407); r = k * 15 + b[0] - '0';"},

	    /* Adversarial syntax. Generators emit well-formed C; obfuscated and
	       code-golf C does not, and it is the shape most likely to break a
	       parser that carries its own keywords. Nothing here is exotic to a
	       compiler, but all of it is hostile to a tokenizer. */
	    {"static int kr(c) int c; { return c + 5; }", "r = kr(6);"},
	    {"static int implicit(void){ return 12; }", "r = implicit();"},
	    {"", "int a=1,b=2,c=3; r = ((a?b:c),(a,b,c), a<b ? b<c ? 13 : 1 : 2);"},
	    {"", "int n=8; char s[9]; char *d=s; switch(n%8){case 0: do{*d++='x'; case 7: *d++='x';"
	     " case 6: *d++='x'; case 5: *d++='x'; case 4: *d++='x'; case 3: *d++='x';"
	     " case 2: *d++='x'; case 1: *d++='x';}while(--n>0);} r = (int)(d - s) + 6;"},
	    {"", "int a<:3:> = <%1,2,3%>; r = a<:2:> + 12;"},   /* digraphs */
	    {"static int leaf(int x){return x;}\nstatic int (*pick(int y))(int){return y?leaf:leaf;}",
	     "r = pick(1)(17);"},
	    {"typedef int A3[3];\nstatic A3 *give(void){ static A3 a = {1,2,18}; return &a; }",
	     "A3 *(*t[1])(void) = {give}; r = (*t[0]())[2];"},
	    {"#include <stdarg.h>\nstatic int vs(int n, ...){ va_list a, b; va_start(a,n); va_copy(b,a);"
	     " int t=0; for(int i=0;i<n;i++) t += va_arg(b,int); va_end(b); va_end(a); return t; }",
	     "r = vs(3, 6, 6, 7);"},
	    {"", "r = ({ int a=1; ({ int b=2; ({ int c=17; a+b+c; }); }); });"},
	    {"static int boom(void){ return *(int*)0; }",
	     "r = (sizeof boom() == sizeof(int)) ? 20 : 1;"},   /* unevaluated call */
	    {"#define M1(x) M2(x)\n#define M2(x) M3(x)\n#define M3(x) ((x)+7)", "r = M1(M1(M1(0)));"},
	    {"", "int i=0; { goto tail; tail: ; } r = i + 21;"},

	    /* Prism's own keywords, in every position C allows an identifier.
	       Each one is a live grammar hazard: a soft keyword that stops being
	       soft in the wrong context silently changes the program. */
	    /* A #define of a dialect keyword stands that keyword down for the TU,
	       so the hoisted define does the substitution and the library path
	       matches what cc -E gives the driver. */
	    {"#define defer 7", "r = defer * 3 + 1;"},
	    {"#define orelse +", "r = 11 orelse 12;"},
	    {"#define raw const", "raw int k = 24; r = k;"},
	    {"struct KW { int defer, orelse, raw; };",
	     "struct KW s = {10, 8, 3}; r = s.defer + s.orelse + s.raw;"},
	    {"", "goto defer; defer: r = 25;"},
	    {"typedef int defer;", "defer x = 26; r = x;"},
	    {"", "int deferred = 13, my_defer = 14; r = deferred + my_defer;"},
	    {"", "const char *s = \"orelse defer raw\"; /* defer orelse raw */ r = (int)s[0] - 84;"},

	    /* Dense floating-point with a string-literal subscript, the shape the
	       spinning-donut program is built from. The subscript is on a literal
	       rather than a tracked array, which is exactly the case the
	       bounds-check matcher has to leave alone. */
	    {"#include <string.h>\n"
	     "static float sq(float x){ while(x>6.2832f)x-=6.2832f; while(x<0)x+=6.2832f;"
	     " float t=x-3.1416f, t2=t*t; return -(t*(1-t2/6+t2*t2/120)); }\n"
	     "static float cq(float x){ return sq(x+1.5708f); }\n"
	     "static int donut(void){ float A=1,B=1,i,j,z[1760]; char b[1760];"
	     " memset(b,32,1760); memset(z,0,sizeof z);"
	     " for(j=0;j<6.28f;j+=0.07f) for(i=0;i<6.28f;i+=0.02f){"
	     " float c=sq(i),d=cq(j),e=sq(A),f=sq(j),g=cq(A),h=d+2,"
	     " D=1/(c*h*e+f*g+5),l=cq(i),m=cq(B),n=sq(B),t=c*h*g-f*e;"
	     " int x=40+30*D*(l*h*m-t*n),y=12+15*D*(l*h*n+t*m),o=x+80*y,"
	     " N=8*((f*e-c*d*g)*m-c*d*e-f*g-l*d*n);"
	     " if(22>y&&y>0&&x>0&&80>x&&D>z[o]){z[o]=D;b[o]=\".,-~:;=!*#$@\"[N>0?N:0];}}"
	     " int s=0; for(int q=0;q<1760;q++) s+=b[q]; return s; }",
	     "r = donut() % 97;"},
	};

	CmStats st = {0};
	long infra = 0;
	char src[1400];

	for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
		snprintf(src, sizeof(src), "%s\nint main(void){ int r = 0; %s return r & 0x7f; }\n",
			 cells[i].pre, cells[i].body);
		/* Gate: if the original does not build and run, the cell says
		 * nothing about prism. */
		int want = cm_run_plain(src);
		if (want <= -1000) {
			infra++;
			cm_note(&st, "cell %zu: baseline failed to build or run (%d): %s", i, want,
				cells[i].body);
			continue;
		}
		st.cells++;
		int got = cm_exec(src, prism_defaults());
		if (got <= -1000) {
			cm_note(&st, "cell %zu: prism output failed to build or run (%d): %s", i, got,
				cells[i].body);
			continue;
		}
		if (got != want)
			cm_note(&st, "cell %zu: plain C returns %d, through prism returns %d: %s", i,
				want, got, cells[i].body);
	}

	char name[384];
	snprintf(name, sizeof(name),
		 "completeness[gen/passthrough-equivalence]: %ld cells, %ld bad, %ld infra%s%s",
		 st.cells, st.bad, infra, st.bad ? " -- " : "", st.bad ? st.first : "");
	CHECK(st.bad == 0, name);
	CHECK(st.cells > 0, "completeness[gen/passthrough-equivalence]: cells actually ran");
}

/* ── gen/runtime-defer-depth ──────────────────────────────────────────
 *
 * gen/runtime-defer-product sweeps exits at ONE nesting level. The cases the
 * SPEC calls out as hard are all multi-level:
 *
 *   "Nested loops: break/continue unwind the correct scope"
 *
 * `break` leaves the nearest enclosing loop *or switch*, stepping over any
 * plain blocks between and running their defers on the way. `continue` leaves
 * the nearest enclosing loop, stepping over blocks *and* switches. `goto` and
 * `return` leave everything. Those are three different answers to "which
 * defers run", and a transpiler carrying one scope stack for all of them gets
 * some subset wrong.
 *
 * ORACLE — a conservation law, not a predicted trace.
 *
 *     every scope that is entered runs its defer exactly once on the way out.
 *
 * So the program counts entries and firings per level and asserts they balance.
 * That is universally true for every kind/exit combination, holds under loops
 * without having to predict iteration counts, and needs no model of C's
 * break/continue targeting — which matters, because a simulator that got the
 * targeting wrong in the same way the transpiler did would agree with it and
 * report green.
 *
 * It is also the property that defer exists to provide: an unbalanced count is
 * a leaked cleanup (fired too few) or a double free (fired too many), which are
 * exactly the bugs the feature is meant to make impossible.
 *
 * A second, independent check rides along: defers must fire strictly
 * inner-to-outer, so the last firing of an inner level always precedes the
 * enclosing level's. */

enum { DDK_BLOCK, DDK_LOOP, DDK_SWITCH, DDK_N };
enum { DDX_FALL, DDX_RETURN, DDX_BREAK, DDX_CONTINUE, DDX_GOTO, DDX_N };
enum { DD_LEVELS = 3 };

static const char *dd_kind_ch(int k) { return k == DDK_BLOCK ? "b" : k == DDK_LOOP ? "l" : "s"; }
static const char *dd_exit_name(int x) {
	return x == DDX_FALL	     ? "fall"
	       : x == DDX_RETURN     ? "return"
	       : x == DDX_BREAK	     ? "break"
	       : x == DDX_CONTINUE   ? "continue"
				     : "goto";
}

/* `break` needs an enclosing loop or switch, `continue` an enclosing loop.
 * Without one the program would not compile, so those cells are not cells. */
static int dd_legal(const int *kind, int exit) {
	if (exit == DDX_BREAK) {
		for (int i = 0; i < DD_LEVELS; i++)
			if (kind[i] == DDK_LOOP || kind[i] == DDK_SWITCH) return 1;
		return 0;
	}
	if (exit == DDX_CONTINUE) {
		for (int i = 0; i < DD_LEVELS; i++)
			if (kind[i] == DDK_LOOP) return 1;
		return 0;
	}
	return 1;
}

static void cm_gen_defer_depth(void) {
	CmStats st = {0};
	long infra = 0, skipped = 0;
	char src[2400], open_s[DD_LEVELS][160], close_s[DD_LEVELS][16], nest[1200];

	for (int k0 = 0; k0 < DDK_N; k0++)
		for (int k1 = 0; k1 < DDK_N; k1++)
			for (int k2 = 0; k2 < DDK_N; k2++)
				for (int exit = 0; exit < DDX_N; exit++) {
					int kind[DD_LEVELS] = {k0, k1, k2};
					if (!dd_legal(kind, exit)) {
						skipped++;
						continue;
					}
					for (int i = 0; i < DD_LEVELS; i++) {
						if (kind[i] == DDK_LOOP)
							snprintf(open_s[i], sizeof(open_s[i]),
								 "for (int i%d = 0; i%d < 2; i%d++) {", i,
								 i, i);
						else if (kind[i] == DDK_SWITCH)
							snprintf(open_s[i], sizeof(open_s[i]),
								 "switch (1) { case 1: {");
						else
							snprintf(open_s[i], sizeof(open_s[i]), "{");
						snprintf(close_s[i], sizeof(close_s[i]), "%s",
							 kind[i] == DDK_SWITCH ? "} }" : "}");
					}
					const char *act = exit == DDX_FALL	 ? ""
							  : exit == DDX_RETURN	 ? "return;"
							  : exit == DDX_BREAK	 ? "break;"
							  : exit == DDX_CONTINUE ? "continue;"
										 : "goto done;";
					snprintf(nest, sizeof(nest),
						 "%s ent[0]++; defer d0(); "
						 "%s ent[1]++; defer d1(); "
						 "%s ent[2]++; defer d2(); %s "
						 "%s %s %s",
						 open_s[0], open_s[1], open_s[2], act, close_s[2],
						 close_s[1], close_s[0]);
					snprintf(
					    src, sizeof(src),
					    "static int ent[3], fir[3], ord[3], seq;\n"
					    "static void d0(void){ fir[0]++; ord[0] = ++seq; }\n"
					    "static void d1(void){ fir[1]++; ord[1] = ++seq; }\n"
					    "static void d2(void){ fir[2]++; ord[2] = ++seq; }\n"
					    "static void t(void){ %s done: ; }\n"
					    "int main(void){ t();\n"
					    "  for (int i = 0; i < 3; i++) if (fir[i] != ent[i]) return 10 + i;\n"
					    "  if (ent[2] && ent[1] && ord[2] > ord[1]) return 20;\n"
					    "  if (ent[1] && ent[0] && ord[1] > ord[0]) return 21;\n"
					    "  if (!ent[0]) return 30;\n"
					    "  return 0; }\n",
					    nest);
					st.cells++;
					PrismFeatures f = prism_defaults();
					int ex = cm_exec(src, f);
					if (ex <= -1000) {
						infra++;
						cm_note(&st, "%s%s%s/%s: build failed (%d)",
							dd_kind_ch(k0), dd_kind_ch(k1),
							dd_kind_ch(k2), dd_exit_name(exit), ex);
						continue;
					}
					if (ex != 0) {
						const char *why =
						    ex >= 10 && ex <= 12  ? "defer count != scope entries"
						    : ex == 20 || ex == 21 ? "defers fired outer-before-inner"
						    : ex == 30		   ? "outermost scope never entered"
									   : "unexpected exit";
						cm_note(&st, "%s%s%s/%s: %s (exit=%d)", dd_kind_ch(k0),
							dd_kind_ch(k1), dd_kind_ch(k2),
							dd_exit_name(exit), why, ex);
					}
				}

	char name[384];
	snprintf(name, sizeof(name),
		 "completeness[gen/runtime-defer-depth]: %ld cells, %ld bad, %ld skipped, %ld infra%s%s",
		 st.cells, st.bad, skipped, infra, st.bad ? " -- " : "", st.bad ? st.first : "");
	CHECK(st.bad == 0, name);
	CHECK(st.cells > 0, "completeness[gen/runtime-defer-depth]: cells actually ran");
}

/* ── gen/runtime-defer-edges ──────────────────────────────────────────
 *
 * Two cases the README names as handled, neither with a procedural tier:
 *
 *   "Statement expressions ({ ... }): defers fire at inner scope, not outer"
 *   "switch fallthrough: defers don't double-fire between cases"
 *
 * Both are places where the scope a defer belongs to is not the one the braces
 * suggest. A switch body is a single scope shared by every case label, so
 * falling from one case into the next registers a second defer in that same
 * scope; a transpiler treating each `case` as a boundary either fires the
 * first early or fires it twice. A statement expression is an expression
 * containing a block, so its defers must fire while the enclosing expression
 * is still being evaluated.
 *
 * prism restricts both shapes on purpose, and building the tier surfaced that:
 *
 *   defer in an unbraced switch case  -> "requires braces"
 *   defer at the top of a stmt-expr   -> "wrap in a block"
 *
 * Neither is a defect — an unbraced case lets a later label jump past the
 * registration, and a top-level defer in a statement expression has no
 * unambiguous firing point relative to the value. So the tier sweeps the legal
 * forms for behaviour AND asserts the two rejections, which keeps a
 * restriction from silently lapsing later.
 *
 * ORACLE for the legal forms is the conservation law from
 * gen/runtime-defer-depth, with registration counted rather than assumed:
 *
 *     a defer fires exactly once per time control actually reached it.
 *
 * `reg[i]++` sits immediately before each `defer`, so entering a switch at
 * case 1 leaves reg[0] at zero and the law then *requires* d0 never to fire —
 * testing both directions without predicting which cases an entry visits. */

enum { DE_ENTRY_N = 3 };
enum { DEB_UNBRACED, DEB_BRACED, DEB_N };
enum { DE_BRK_NONE, DE_BRK_LAST, DE_BRK_EACH, DE_BRK_N };

static const char *de_brk_name(int b) {
	return b == DE_BRK_NONE ? "no-break" : b == DE_BRK_LAST ? "break-last" : "break-each";
}

static void cm_gen_defer_edges(void) {
	static const char *pre =
	    "static int reg[4], fir[4];\n"
	    "static void d0(void){ fir[0]++; }\n"
	    "static void d1(void){ fir[1]++; }\n"
	    "static void d2(void){ fir[2]++; }\n"
	    "static void d3(void){ fir[3]++; }\n";
	static const char *balance =
	    "  for (int i = 0; i < 4; i++) if (fir[i] != reg[i]) return 10 + i;\n";

	CmStats st = {0};
	long infra = 0;
	char src[2400], cases[900];

	/* ── switch: fallthrough, entry point, break placement ───────── */
	for (int entry = 0; entry < DE_ENTRY_N; entry++)
		for (int braced = 0; braced < DEB_N; braced++)
			for (int brk = 0; brk < DE_BRK_N; brk++) {
				cases[0] = '\0';
				for (int c = 0; c < 3; c++) {
					char one[300];
					const char *tail =
					    (brk == DE_BRK_EACH || (brk == DE_BRK_LAST && c == 2))
						? "break;"
						: "";
					if (braced == DEB_BRACED)
						snprintf(one, sizeof(one),
							 "case %d: { reg[%d]++; defer d%d(); %s } ", c, c,
							 c, tail);
					else
						snprintf(one, sizeof(one),
							 "case %d: reg[%d]++; defer d%d(); %s ", c, c, c,
							 tail);
					strncat(cases, one, sizeof(cases) - strlen(cases) - 1);
				}
				snprintf(src, sizeof(src),
					 "%s"
					 "static void t(int n){ reg[3]++; defer d3(); switch (n) { %s } }\n"
					 "int main(void){ t(%d);\n%s"
					 "  if (fir[3] != 1) return 20;\n"
					 "  return 0; }\n",
					 pre, cases, entry, balance);
				st.cells++;
				if (braced == DEB_UNBRACED) {
					PrismResult r = cm_txf(src, prism_defaults());
					if (cm_ok(&r))
						cm_note(&st,
							"switch entry=%d unbraced %s: accepted a defer "
							"in an unbraced case",
							entry, de_brk_name(brk));
					prism_free(&r);
					continue;
				}
				int ex = cm_exec(src, prism_defaults());
				if (ex <= -1000) {
					infra++;
					cm_note(&st, "switch entry=%d braced %s: build failed (%d)",
						entry, de_brk_name(brk), ex);
					continue;
				}
				if (ex != 0)
					cm_note(&st,
						"switch entry=%d braced %s: defer count != times "
						"reached (exit=%d)",
						entry, de_brk_name(brk), ex);
			}

	/* ── statement expressions, legal (block-wrapped) forms ──────── */
	{
		static const char *sites[] = {
		    "int v = ({ { reg[0]++; defer d0(); } 7; }); if (fir[0] != 1) rc = 40; (void)v;",
		    "int v = 0; v = ({ { reg[0]++; defer d0(); } 7; }); if (fir[0] != 1) rc = 41; (void)v;",
		    "int v = 1 + ({ { reg[0]++; defer d0(); } 7; }); if (fir[0] != 1) rc = 42; (void)v;",
		    "if (({ { reg[0]++; defer d0(); } 1; })) { if (fir[0] != 1) rc = 43; }",
		    "int v = ({ { reg[0]++; defer d0(); } ({ { reg[1]++; defer d1(); } 3; }); }); "
		    "if (fir[0] != 1 || fir[1] != 1) rc = 44; (void)v;",
		    "for (int i = 0; i < 2; i++) { int v = ({ { reg[0]++; defer d0(); } i; }); (void)v; } "
		    "if (fir[0] != 2) rc = 45;",
		    "int a[2] = {0,0}; a[({ { reg[0]++; defer d0(); } 1; })] = 5; "
		    "if (fir[0] != 1 || a[1] != 5) rc = 46;",
		    /* the defer must have fired before the value is taken */
		    "int v = ({ int w; { reg[0]++; defer d0(); w = 6; } if (fir[0] != 1) rc = 47; w; }); "
		    "if (v != 6) rc = 48;",
		    /* a block nested inside the statement expression's block */
		    "int v = ({ { reg[0]++; defer d0(); { reg[1]++; defer d1(); } } 9; }); "
		    "if (fir[0] != 1 || fir[1] != 1) rc = 49; (void)v;",
		};
		for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
			snprintf(src, sizeof(src),
				 "%s"
				 "static int t(void){ int rc = 0; reg[3]++; defer d3(); %s return rc; }\n"
				 "int main(void){ int rc = t(); if (rc) return rc;\n%s"
				 "  return 0; }\n",
				 pre, sites[i], balance);
			st.cells++;
			int ex = cm_exec(src, prism_defaults());
			if (ex <= -1000) {
				infra++;
				cm_note(&st, "stmt-expr %zu: build failed (%d)", i, ex);
				continue;
			}
			if (ex != 0)
				cm_note(&st,
					"stmt-expr %zu: defer did not fire at the inner scope "
					"(exit=%d)",
					i, ex);
		}
	}

	/* ── the two restrictions must keep holding ──────────────────── */
	{
		static const char *illegal[] = {
		    "int v = ({ reg[0]++; defer d0(); 7; }); (void)v;",
		    "int v = ({ defer d0(); 7; }); (void)v;",
		    "int v = 1 + ({ reg[0]++; defer d0(); 7; }); (void)v;",
		};
		for (size_t i = 0; i < sizeof(illegal) / sizeof(illegal[0]); i++) {
			snprintf(src, sizeof(src),
				 "%s"
				 "static int t(void){ int rc = 0; %s return rc; }\n"
				 "int main(void){ return t(); }\n",
				 pre, illegal[i]);
			st.cells++;
			PrismResult r = cm_txf(src, prism_defaults());
			if (cm_ok(&r))
				cm_note(&st,
					"stmt-expr illegal %zu: accepted a defer at the top level "
					"of a statement expression",
					i);
			prism_free(&r);
		}
	}

	char name[384];
	snprintf(name, sizeof(name),
		 "completeness[gen/runtime-defer-edges]: %ld cells, %ld bad, %ld infra%s%s", st.cells,
		 st.bad, infra, st.bad ? " -- " : "", st.bad ? st.first : "");
	CHECK(st.bad == 0, name);
	CHECK(st.cells > 0, "completeness[gen/runtime-defer-edges]: cells actually ran");
}

/* ── gen/runtime-defer-wide ───────────────────────────────────────────
 *
 * Closes the axes the other defer tiers leave open:
 *
 *   depth 4        (defer-depth stops at 3)
 *   do/while       (only `for` was swept)
 *   3 defers/scope (only 0-2)
 *   goto backward  (only forward, to a label past the construct)
 *   recursion      (defers of several live frames)
 *   multi-return   (returns at different depths in one function)
 *
 * ORACLE is the conservation law from gen/runtime-defer-depth — every scope
 * entered runs its defer exactly once — for the same reason: it needs no model
 * of C's break/continue targeting, so a simulator that got the targeting wrong
 * the same way the transpiler did cannot make the tier agree with a bug.
 * Registration is counted (`reg[i]++` immediately before each `defer`) so a
 * scope that is never reached must not fire, and one reached twice must fire
 * twice.
 *
 * The kind vectors are a representative list rather than the full 4^4 product:
 * 256 patterns x 5 exits at two process spawns each would dominate the suite's
 * wall clock, and the combinations that matter are the ones where the scope a
 * `break` or `continue` targets is NOT the innermost — blocks and switches
 * interposed between the exit and its loop. Those are all present below. */

enum { DW_LVL = 4 };

static void cm_gen_defer_wide(void) {
	/* 'b' block, 'f' for, 'd' do-while, 's' switch, 'i' if. Chosen so every
	 * pattern puts something non-targetable between an exit and its loop. */
	static const char *pats[] = {
	    "bbbb", "ffff", "dddd", "ssss", "bfbf", "fbfb", "fsbf", "sfsb",
	    "fbsd", "dbsf", "ifbf", "fifi", "sbdb", "dsfb", "bdsf", "fdbs",
	};
	static const int ndef[] = {1, 3};
	enum { DWX_FALL, DWX_RETURN, DWX_BREAK, DWX_CONTINUE, DWX_GOTO, DWX_N };
	static const char *acts[] = {"", "return;", "break;", "continue;", "goto done;"};
	static const char *xname[] = {"fall", "return", "break", "continue", "goto"};

	CmStats st = {0};
	long infra = 0, skipped = 0;
	char src[4096], nest[3000], pre[600], chk[400];

	for (size_t p = 0; p < sizeof(pats) / sizeof(pats[0]); p++)
		for (size_t nd = 0; nd < sizeof(ndef) / sizeof(ndef[0]); nd++)
			for (int x = 0; x < DWX_N; x++) {
				const char *pat = pats[p];
				/* `break` needs a loop or switch somewhere, `continue` a loop. */
				int has_loop = 0, has_brk_target = 0;
				for (int i = 0; i < DW_LVL; i++) {
					if (pat[i] == 'f' || pat[i] == 'd') has_loop = 1;
					if (pat[i] == 'f' || pat[i] == 'd' || pat[i] == 's')
						has_brk_target = 1;
				}
				if (x == DWX_BREAK && !has_brk_target) {
					skipped++;
					continue;
				}
				if (x == DWX_CONTINUE && !has_loop) {
					skipped++;
					continue;
				}
				int n = ndef[nd];
				/* counters + one cleanup fn per (level, slot) */
				int slots = DW_LVL * n;
				pre[0] = '\0';
				chk[0] = '\0';
				{
					char one[128];
					snprintf(one, sizeof(one),
						 "static int reg[%d], fir[%d];\n", slots, slots);
					strncat(pre, one, sizeof(pre) - strlen(pre) - 1);
					for (int k = 0; k < slots; k++) {
						snprintf(one, sizeof(one),
							 "static void c%d(void){ fir[%d]++; }\n", k, k);
						strncat(pre, one, sizeof(pre) - strlen(pre) - 1);
					}
					snprintf(chk, sizeof(chk),
						 "  for (int k = 0; k < %d; k++) if (fir[k] != reg[k]) "
						 "return 10 + k;\n",
						 slots);
				}
				nest[0] = '\0';
				for (int i = 0; i < DW_LVL; i++) {
					char open[192];
					switch (pat[i]) {
					case 'f':
						snprintf(open, sizeof(open),
							 "for (int i%d = 0; i%d < 2; i%d++) { ", i, i, i);
						break;
					case 'd': snprintf(open, sizeof(open), "do { "); break;
					case 's':
						snprintf(open, sizeof(open), "switch (1) { case 1: { ");
						break;
					case 'i': snprintf(open, sizeof(open), "if (1) { "); break;
					default: snprintf(open, sizeof(open), "{ "); break;
					}
					strncat(nest, open, sizeof(nest) - strlen(nest) - 1);
					for (int k = 0; k < n; k++) {
						char one[96];
						int slot = i * n + k;
						snprintf(one, sizeof(one), "reg[%d]++; defer c%d(); ",
							 slot, slot);
						strncat(nest, one, sizeof(nest) - strlen(nest) - 1);
					}
				}
				strncat(nest, acts[x], sizeof(nest) - strlen(nest) - 1);
				for (int i = DW_LVL - 1; i >= 0; i--) {
					const char *close = pat[i] == 's'	? " } } "
							    : pat[i] == 'd' ? " } while (0); "
									    : " } ";
					strncat(nest, close, sizeof(nest) - strlen(nest) - 1);
				}
				snprintf(src, sizeof(src),
					 "%s"
					 "static void t(void){ %s done: ; }\n"
					 "int main(void){ t();\n%s  return 0; }\n",
					 pre, nest, chk);
				st.cells++;
				int ex = cm_exec(src, prism_defaults());
				if (ex <= -1000) {
					infra++;
					cm_note(&st, "%s n=%d %s: build failed (%d)", pat, n,
						xname[x], ex);
					continue;
				}
				if (ex != 0)
					cm_note(&st,
						"%s n=%d %s: defer count != scope entries (exit=%d)",
						pat, n, xname[x], ex);
			}

	/* ── shapes that are not a nesting product ───────────────────── */
	{
		static const char *fixed[] = {
		    /* goto backward over a scope containing defers */
		    "static int reg[2], fir[2];\n"
		    "static void c0(void){ fir[0]++; }\n"
		    "static void t(void){ int n = 0; again: { reg[0]++; defer c0(); n++; } "
		    "if (n < 3) goto again; }\n"
		    "int main(void){ t(); return fir[0] != reg[0]; }\n",
		    /* recursion: several live frames each with defers */
		    "static int reg[2], fir[2];\n"
		    "static void c0(void){ fir[0]++; }\n"
		    "static void r(int n){ reg[0]++; defer c0(); if (n > 0) r(n - 1); }\n"
		    "int main(void){ r(5); return fir[0] != reg[0] || reg[0] != 6; }\n",
		    /* returns at three different depths in one function */
		    "static int reg[3], fir[3];\n"
		    "static void c0(void){ fir[0]++; }\n"
		    "static void c1(void){ fir[1]++; }\n"
		    "static void c2(void){ fir[2]++; }\n"
		    "static void t(int k){ reg[0]++; defer c0(); if (k == 0) return; "
		    "{ reg[1]++; defer c1(); if (k == 1) return; "
		    "{ reg[2]++; defer c2(); return; } } }\n"
		    "int main(void){ for (int k = 0; k < 3; k++) t(k);\n"
		    "  for (int i = 0; i < 3; i++) if (fir[i] != reg[i]) return 10 + i;\n"
		    "  return !(reg[0] == 3 && reg[1] == 2 && reg[2] == 1); }\n",
		    /* many defers in one scope */
		    "static int reg[1], fir[1];\n"
		    "static void c0(void){ fir[0]++; }\n"
		    "static void t(void){ for (int i = 0; i < 8; i++) { reg[0]++; defer c0(); } }\n"
		    "int main(void){ t(); return fir[0] != reg[0] || reg[0] != 8; }\n",
		    /* defer in a do-while that runs several iterations */
		    "static int reg[1], fir[1];\n"
		    "static void c0(void){ fir[0]++; }\n"
		    "static void t(void){ int i = 0; do { reg[0]++; defer c0(); i++; } while (i < 4); }\n"
		    "int main(void){ t(); return fir[0] != reg[0] || reg[0] != 4; }\n",
		    /* defer inside a while whose body exits early */
		    "static int reg[1], fir[1];\n"
		    "static void c0(void){ fir[0]++; }\n"
		    "static void t(void){ int i = 0; while (i < 9) { reg[0]++; defer c0(); i++; "
		    "if (i == 3) break; } }\n"
		    "int main(void){ t(); return fir[0] != reg[0] || reg[0] != 3; }\n",
		};
		for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
			st.cells++;
			int ex = cm_exec(fixed[i], prism_defaults());
			if (ex <= -1000) {
				infra++;
				cm_note(&st, "shape %zu: build failed (%d)", i, ex);
				continue;
			}
			if (ex != 0) cm_note(&st, "shape %zu: exit=%d", i, ex);
		}
	}

	char name[384];
	snprintf(name, sizeof(name),
		 "completeness[gen/runtime-defer-wide]: %ld cells, %ld bad, %ld skipped, %ld infra%s%s",
		 st.cells, st.bad, skipped, infra, st.bad ? " -- " : "", st.bad ? st.first : "");
	CHECK(st.bad == 0, name);
	CHECK(st.cells > 0, "completeness[gen/runtime-defer-wide]: cells actually ran");
}

/* ── gen/defer-reject-product ─────────────────────────────────────────
 *
 * prism enforces 19 restrictions on where a defer may appear. Each had exactly
 * one hand-written test, which proves the rule fires for the shape somebody
 * wrote down and says nothing about its siblings. The failure mode that
 * matters is a rule that holds in a function body and lapses inside a switch
 * case, or holds at depth 1 and lapses at depth 2 — a shape where the
 * transpiler accepts what it believes it rejects and lowers it to something
 * with no defined meaning.
 *
 * This crosses every restriction with every enclosing context it can still
 * apply in. A restriction about placement within a function (nested defer,
 * control flow inside a defer body, labels, statics) must hold no matter how
 * deeply the defer itself is nested, so each fragment is planted in a plain
 * body, a block, a loop, a braced switch case, an if, and two nested blocks.
 *
 * ORACLE: prism must reject. An acceptance is reported with the context that
 * produced it, because the interesting part of such a finding is always which
 * placement let it through. Cells whose *baseline* is rejected for an
 * unrelated reason cannot exist here — these fragments are invalid prism, not
 * invalid C, so there is nothing to gate on. */

static void cm_gen_defer_reject(void) {
	/* Fragments that must never be accepted, wherever they sit. */
	static const struct {
		const char *name;
		const char *frag;
	} rules[] = {
	    {"nested-defer", "defer { defer f(); }"},
	    {"nested-defer-block", "defer { { defer f(); } }"},
	    {"nested-defer-loop", "defer { for (int q = 0; q < 2; q++) { defer f(); } }"},
	    {"return-in-defer", "defer { f(); return; }"},
	    {"goto-in-defer", "defer { f(); goto zz; } zz: ;"},
	    {"label-in-defer", "defer { zq: f(); goto zq; }"},
	    {"static-in-defer", "defer { static int sq; sq++; f(); }"},
	    {"stmt-expr-top", "int vq = ({ defer f(); 1; }); (void)vq;"},
	    {"paren-top", "(defer f());"},
	    {"static-assert", "_Static_assert(sizeof(({ int q; defer f(); q = 4; q; })) == 4, \"x\");"},
	    {"missing-semi", "defer f()"},
	};
	/* Enclosing contexts. %s is the fragment. */
	static const struct {
		const char *name;
		const char *wrap;
	} ctxs[] = {
	    {"body", "%s"},
	    {"block", "{ %s }"},
	    {"loop", "for (int zi = 0; zi < 2; zi++) { %s }"},
	    {"switch", "switch (1) { case 1: { %s } }"},
	    {"if", "if (1) { %s }"},
	    {"nested2", "{ { %s } }"},
	    {"while", "while (1) { %s break; }"},
	    {"do", "do { %s } while (0);"},
	};

	CmStats st = {0};
	char src[1600], inner[900];

	for (size_t r = 0; r < sizeof(rules) / sizeof(rules[0]); r++)
		for (size_t c = 0; c < sizeof(ctxs) / sizeof(ctxs[0]); c++) {
			/* `break` in the while wrapper would be inside a defer body for
			 * the control-flow rules, changing which rule is under test. */
			if (strstr(ctxs[c].wrap, "break;") && strstr(rules[r].frag, "defer {")) continue;
			snprintf(inner, sizeof(inner), ctxs[c].wrap, rules[r].frag);
			snprintf(src, sizeof(src), "void f(void);\nvoid t(void){ %s }\n", inner);
			st.cells++;
			PrismResult res = cm_txf(src, prism_defaults());
			if (cm_ok(&res))
				cm_note(&st, "%s accepted in context '%s'", rules[r].name, ctxs[c].name);
			prism_free(&res);
		}

	/* Restrictions that are about the function, not the statement's nesting. */
	{
		static const struct {
			const char *name;
			const char *src;
		} whole[] = {
		    {"file-scope", "void f(void);\ndefer f();\n"},
		    {"braceless-if", "void f(void);\nvoid t(int c){ if (c) defer f(); }\n"},
		    {"braceless-for",
		     "void f(void);\nvoid t(void){ for (int i = 0; i < 2; i++) defer f(); }\n"},
		    {"braceless-while", "void f(void);\nvoid t(void){ while (1) defer f(); }\n"},
		    {"unbraced-switch-case",
		     "void f(void);\nvoid t(int n){ switch (n) { case 0: defer f(); break; } }\n"},
		    {"computed-goto",
		     "void f(void);\nvoid t(void){ void *p = &&L; defer f(); goto *p; L: ; }\n"},
		    {"asm-goto",
		     "void f(void);\nvoid t(void){ defer f(); asm goto (\"\" ::: : L); L: ; }\n"},
		    {"setjmp", "#include <setjmp.h>\nvoid f(void);\nstatic jmp_buf b;\n"
			       "void t(void){ defer f(); setjmp(b); }\n"},
		    {"nested-function",
		     "void f(void);\nvoid t(void){ defer f(); void inner(void){ } inner(); }\n"},
		    {"goto-skips-defer", "void f(void);\nvoid t(void){ goto L; defer f(); L: ; }\n"},
		    {"goto-loops-defer",
		     "void f(void);\nvoid t(void){ int i=0; L: defer f(); i++; if (i<2) goto L; }\n"},
		    {"goto-skips-defer-nested",
		     "void f(void);\nvoid t(void){ { goto L; defer f(); L: ; } }\n"},
		    {"goto-skips-defer-loop",
		     "void f(void);\nvoid t(void){ for(int i=0;i<2;i++){ goto L; defer f(); L: ; } }\n"},
		};
		for (size_t i = 0; i < sizeof(whole) / sizeof(whole[0]); i++) {
			st.cells++;
			PrismResult res = cm_txf(whole[i].src, prism_defaults());
			if (cm_ok(&res)) cm_note(&st, "%s accepted", whole[i].name);
			prism_free(&res);
		}
	}

	char name[384];
	snprintf(name, sizeof(name), "completeness[gen/defer-reject-product]: %ld cells, %ld bad%s%s",
		 st.cells, st.bad, st.bad ? " -- " : "", st.bad ? st.first : "");
	CHECK(st.bad == 0, name);
	CHECK(st.cells > 0, "completeness[gen/defer-reject-product]: cells actually ran");
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
/* ── ICE-context × injected-statement compile oracle ──────────────────
 * A GNU statement expression may appear in EVERY integer-constant-expression
 * context, and Clang/GCC fold it when its value is constant. Any Prism
 * transform that injects a *statement* into such a stmt-expr destroys that
 * constness and turns legal C into C the backend rejects.
 *
 * Seed defect this tier found (zero-init's __builtin_memset path; now fixed):
 *     void f(int n){ struct S { int x[({ int vla[n]; (void)vla; 1; })]; } obj; }
 * compiles as plain C, but the emitted memset made the member a VLA-in-struct.
 * test.zeroinit.c's test_struct_body_stmt_expr_features had LOCKED that shape
 * with a strstr(output,"memset") oracle — the substring matched the *enclosing*
 * object's memset while the output did not compile. Exactly the blind spot this
 * tier exists to remove. compute_decl_zero_kind now drops the zero-init for a
 * declaration inside a stmt-expr in an ICE context (p1_decl_in_ice_stmt_expr).
 *
 * Oracle is self-gating; no hand-derived expectations:
 *     plain-C baseline compiles AND prism accepts  =>  output must compile
 * A conservative Prism *reject* is always acceptable. */
static void cm_gen_ice_stmt_expr(void) {
	/* Declarations placed inside the statement expression. The zero-init
	 * lowering differs per shape: memset (VLA/union/typeof-array/empty
	 * aggregate) vs an initializer (scalar, fixed array). Only the
	 * memset path emits a *call*, which is what breaks constant folding. */
	static const char *payload[] = {
		"int vla[n];",		     /* VLA           -> memset */
		"union U{int a; long b;} u;", /* union         -> memset */
		"typeof(int[4]) ta;",	     /* typeof array  -> memset */
		"struct E1{} e1;",	     /* empty agg     -> memset */
		"int fx[4];",		     /* array         -> = {0}  */
		"int s;",		     /* scalar        -> = 0    */
		"struct T{int a,b;} t;",
		"_Atomic int ai;",
		"int m[2][2];",
		"const char *cs = \"lit\";", /* auto-static promotion */
		"int q[4]; q[0]=1;",	     /* bounds-check wrap */
		/* Nested statement expression: the inner declaration is just as
		 * unable to carry a memset as the outer one. */
		"int vo[n]; ({ int vi[n]; (void)vi; 0; });",
		"union UO{int a; long b;} uo; ({ union UI{int a; long b;} ui; (void)ui; 0; });",
	};
	/* %s takes the statement expression; each template is a whole TU. */
	static const char *ctx[] = {
		"void f(int n){ struct S { int x[%s]; } obj; (void)obj; (void)n; }\n",
		"void f(int n){ struct O { struct I { int y[%s]; } i; } o; (void)o; (void)n; }\n",
		"void f(int n){ struct B { int b : %s; } bb; (void)bb; (void)n; }\n",
		"void f(int n){ enum E { AA = %s }; (void)AA; (void)n; }\n",
		"void f(int n){ static int arr[%s]; (void)arr; (void)n; }\n",
		"void f(int n){ static int gv = %s; (void)gv; (void)n; }\n",
		"void f(int n){ int a[4] = { [%s] = 7 }; (void)a; (void)n; }\n",
		"void f(int n){ int *p = (int[%s]){0}; (void)p; (void)n; }\n",
		"void f(int n){ _Static_assert(%s, \"m\"); (void)n; }\n",
		"void f(int n){ int arr[%s]; (void)arr; (void)n; }\n",
		"void f(int n){ unsigned long z = sizeof(int[%s]); (void)z; (void)n; }\n",
	};
	static const struct {
		const char *name;
		int bounds, zi, as_aur;
	} cfg[] = {
		{ "default", 0, 1, 0 },
		{ "bounds", 1, 1, 0 },
		{ "allon", 1, 1, 1 },
		{ "nozi", 1, 0, 1 },
	};

	CmStats st = {0};
	long skipped = 0, infra = 0;

	for (size_t c = 0; c < sizeof(ctx) / sizeof(ctx[0]); c++) {
		for (size_t p = 0; p < sizeof(payload) / sizeof(payload[0]); p++) {
			char se[512], src[1024];
			snprintf(se, sizeof(se), "({ %s 1; })", payload[p]);
			snprintf(src, sizeof(src), ctx[c], se);

			/* Gate: only cells whose plain-C baseline is legal can
			 * accuse Prism of breaking legality. */
			int base = cm_cc_accepts(src);
			if (base < 0) { infra++; continue; }
			if (base == 0) { skipped++; continue; }

			for (size_t k = 0; k < sizeof(cfg) / sizeof(cfg[0]); k++) {
				PrismFeatures f = prism_defaults();
				f.bounds_check = cfg[k].bounds != 0;
				f.zeroinit = cfg[k].zi != 0;
				f.auto_static = cfg[k].as_aur != 0;
				f.auto_unreachable = cfg[k].as_aur != 0;

				st.cells++;
				PrismResult r = cm_txf(src, f);
				if (!cm_ok(&r) || !r.output) {
					prism_free(&r); /* conservative reject: OK */
					continue;
				}
				int got = cm_cc_accepts(r.output);
				if (got < 0) infra++;
				else if (got == 0)
					cm_note(&st, "ctx=%zu payload=%zu cfg=%s: accepted but "
						     "output is illegal C",
						c, p, cfg[k].name);
				prism_free(&r);
			}
		}
	}

	/* Report skips explicitly: stmt-expr-as-ICE is a fold-if-possible
	 * extension, so a compiler that folds less would skip most cells. A
	 * tier that silently skipped everything must not read as coverage. */
	char name[384];
	snprintf(name, sizeof(name),
		 "completeness[gen/ice-stmt-expr]: %ld cells, %ld bad, %ld gated-out, "
		 "%ld infra%s%s",
		 st.cells, st.bad, skipped, infra, st.bad ? " -- " : "", st.bad ? st.first : "");
	CHECK(st.bad == 0, name);
}

#undef CM_LOG_PRE
#endif /* !_WIN32 */

/* Formerly the env-gated `completeness_open` suite.  There is no staging area
 * any more: a tier is either in the default run or it does not exist.  Red here
 * means a product bug to fix, not a tier to gate off. */
/* Everything from here to the matching #endif is POSIX-only, and is called
 * only from the #ifndef _WIN32 arm of run_completeness_open_tests. These tiers
 * fork and inspect wait status, create temp directory trees, symlink, and run
 * `install` against $PREFIX; MSVC has none of that. The guard has to wrap the
 * DEFINITIONS and not just the calls, because the CM_X_* result codes live
 * beside cm_exec_trap inside the same guard: leaving the bodies visible on
 * Windows compiles them against macros that were never defined, which is
 * exactly how this broke. */
#ifndef _WIN32

/* ── executed bounds-check boundary ───────────────────────────────────
 *
 * Every cell below was measured against the shipping binary before being
 * written down, because a tier whose expectations came from reading the spec
 * tests the spec, not the compiler. Three outcome classes, and the tier is a
 * statement about where the feature's edge sits:
 *
 *   WRAP      in-bounds runs clean, one-past dies by signal.
 *   REJECT    prism refuses at transpile time. `*(a+i)` and `i[a]` are
 *             equivalent to subscripts under C 6.5.2.1 but bypass the
 *             last-emitted matcher, so they fail closed with a diagnostic
 *             rather than silently emitting an unchecked access (SPEC 987-988).
 *   UNCHECKED both arms exit 0. Recording these is the point: a decayed
 *             parameter has no size to check against, and if prism ever
 *             learns to wrap ptr-to-array, this cell goes red and someone
 *             reclassifies it on purpose instead of never noticing.
 *
 * Pairing each trap cell with an in-bounds cell is what keeps the oracle
 * honest. "Died by signal" on its own would be satisfied by any crash. */
#define CM_B_WRAP 0
#define CM_B_REJECT 1
#define CM_B_UNCHECKED 2

static void cm_gen_runtime_bounds_trap(void) {
	static const struct {
		const char *name;
		const char *prog; /* %d receives the index */
		int n;		  /* length: in-bounds is n-1, one-past is n */
		int cls;
	} cells[] = {
	    {"local-int", "int main(void){int a[4]={0};volatile int i=%d;return a[i];}", 4, CM_B_WRAP},
	    {"local-char", "int main(void){char a[4]={0};volatile int i=%d;return a[i];}", 4, CM_B_WRAP},
	    {"local-double",
	     "int main(void){double a[4]={0};volatile int i=%d;return (int)a[i];}", 4, CM_B_WRAP},
	    {"local-struct",
	     "struct S{int x,y;};int main(void){struct S a[4]={{0,0}};volatile int i=%d;return "
	     "a[i].x;}",
	     4, CM_B_WRAP},
	    {"local-ptr-elem",
	     "int main(void){int*a[4]={0};volatile int i=%d;return a[i]!=0;}", 4, CM_B_WRAP},
	    {"local-2d-inner",
	     "int main(void){int a[2][4]={{0}};volatile int i=%d;return a[1][i];}", 4, CM_B_WRAP},
	    {"local-2d-outer",
	     "int main(void){int a[4][2]={{0}};volatile int i=%d;return a[i][1];}", 4, CM_B_WRAP},
	    {"static-local",
	     "int main(void){static int a[4];volatile int i=%d;return a[i];}", 4, CM_B_WRAP},
	    {"file-scope", "int a[4];int main(void){volatile int i=%d;return a[i];}", 4, CM_B_WRAP},
	    {"vla",
	     "int main(void){volatile int n=4;int a[n];for(int k=0;k<n;k++)a[k]=0;volatile int "
	     "i=%d;return a[i];}",
	     4, CM_B_WRAP},
	    {"len-1", "int main(void){int a[1]={0};volatile int i=%d;return a[i];}", 1, CM_B_WRAP},
	    {"nested-block",
	     "int main(void){volatile int i=%d;{int a[4]={0};return a[i];}}", 4, CM_B_WRAP},
	    {"loop-body",
	     "int main(void){int r=0;for(int q=0;q<1;q++){int a[4]={0};volatile int "
	     "i=%d;r=a[i];}return r;}",
	     4, CM_B_WRAP},

	    {"ptr-arith-deref",
	     "int main(void){int a[4]={0};volatile int i=%d;return *(a+i);}", 4, CM_B_REJECT},
	    {"commutative-idx",
	     "int main(void){int a[4]={0};volatile int i=%d;return i[a];}", 4, CM_B_REJECT},
	    {"addr-first-elem",
	     "int main(void){int a[4]={0};volatile int i=%d;return *(&a[0]+i);}", 4, CM_B_REJECT},

	    {"decayed-param",
	     "int g(int a[4],volatile int i){return a[i];}int main(void){int b[4]={0};volatile int "
	     "i=%d;return g(b,i);}",
	     4, CM_B_UNCHECKED},
	    {"static-param",
	     "int g(int a[static 4],volatile int i){return a[i];}int main(void){int "
	     "b[4]={0};volatile int i=%d;return g(b,i);}",
	     4, CM_B_UNCHECKED},
	    {"ptr-to-array",
	     "int main(void){int a[4]={0};int(*p)[4]=&a;volatile int i=%d;return (*p)[i];}", 4,
	     CM_B_UNCHECKED},
	};

	CmStats st = {0};
	PrismFeatures f = prism_defaults();
	char src[900];

	for (size_t c = 0; c < sizeof(cells) / sizeof(cells[0]); c++) {
		for (int arm = 0; arm < 2; arm++) { /* 0 = in-bounds, 1 = one-past */
			int idx = arm ? cells[c].n : cells[c].n - 1;
			snprintf(src, sizeof(src), cells[c].prog, idx);
			int rc = cm_exec_trap(src, f);
			st.cells++;

			if (cells[c].cls == CM_B_REJECT) {
				if (rc != CM_X_INFRA)
					cm_note(&st, "%s idx=%d: expected transpile reject, got %d",
						cells[c].name, idx, rc);
				continue;
			}
			if (rc == CM_X_CCFAIL || rc == CM_X_TEMP || rc == CM_X_INFRA) {
				cm_note(&st, "%s idx=%d: infra %d", cells[c].name, idx, rc);
				continue;
			}
			int want_trap = (arm == 1 && cells[c].cls == CM_B_WRAP);
			if (want_trap && rc != CM_X_TRAPPED)
				cm_note(&st, "%s: one-past idx=%d did not trap (rc %d)",
					cells[c].name, idx, rc);
			else if (!want_trap && rc == CM_X_TRAPPED)
				cm_note(&st, "%s: idx=%d trapped but should not", cells[c].name, idx);
		}
	}
	cm_report("gen/runtime-bounds-trap", &st);
}

/* ── executed zero-init ───────────────────────────────────────────────
 *
 * The emitted memset is easy to see in the output text and impossible to judge
 * there: a substring oracle cannot tell a memset that covers the object from
 * one that stops short of the trailing padding, misses a union's largest
 * member, or skips a bitfield storage unit. Each cell dirties the stack, then
 * declares the aggregate, then reads every byte back through an unsigned char
 * pointer and returns the count that were not zero. Expected exit status is 0.
 *
 * `dirty()` writes 0xAA over a stack region larger than anything declared here
 * and is marked noinline so the frame is actually reused. Without it the cells
 * would pass on a stack that happened to already be zero, which is the usual
 * way a zero-init test turns vacuous. */
#define CM_ZI_PRE                                                                                    \
	"__attribute__((noinline)) static void dirty(void){volatile unsigned char "                  \
	"j[512];for(int k=0;k<512;k++)j[k]=0xAA;(void)j;}\n"                                         \
	"static int nz(const void*p,unsigned long n){const unsigned char*b=p;int "                   \
	"k=0;for(unsigned long q=0;q<n;q++)if(b[q])k++;return k;}\n"

static void cm_gen_runtime_zeroinit(void) {
	static const struct {
		const char *name;
		const char *types; /* file-scope type declarations */
		const char *decl;  /* the object under test, named v */
	} cells[] = {
	    {"struct-padding", "struct P{char a;int b;char c;};", "struct P v;"},
	    {"struct-nested", "struct I{char a;long b;};struct N{char x;struct I i;char y;};",
	     "struct N v;"},
	    {"union-mixed", "union U{char s[7];int n;double d;};", "union U v;"},
	    {"bitfield", "struct B{unsigned x:3;unsigned y:5;int tail;};", "struct B v;"},
	    {"bitfield-straddle", "struct B2{unsigned a:1;unsigned b:31;unsigned c:2;};",
	     "struct B2 v;"},
	    {"array-of-struct", "struct S{char a;int b;};", "struct S v[3];"},
	    {"array-2d", "", "int v[3][4];"},
	    {"ptr-member", "struct PM{char a;void*p;char b;};", "struct PM v;"},
	    /* `long double` is the one scalar in C with padding bytes, and Prism
	     * zeroes scalars by assignment (`long double v = 0;`) while it zeroes
	     * aggregates with a whole-object initializer. Where the type fills its
	     * storage, as with aarch64's 128-bit quad, the two are the same. Where
	     * it does not, as with x86-64's 80-bit x87 value in a 16-byte slot, the
	     * assignment writes the value and leaves the remaining bytes holding
	     * whatever was on the stack. __LDBL_MANT_DIG__ tells the two apart at
	     * compile time: 113 for quad, 64 for x87 extended.
	     *
	     * The value is checked unconditionally because that is what zero-init
	     * promises. The all-bytes check is applied only where no padding
	     * exists, so this records the boundary rather than asserting past it.
	     * Worth a decision though: those bytes are stale stack, readable via
	     * memcmp or a byte-wise hash, and closing that is the point of the
	     * feature. See the known gap in RELEASE_NOTES_1.1.6. */
	    {"long-double-value", "", "long double v; return v != 0.0L;"},
	    {"long-double-bytes", "",
	     "long double v;\n"
	     "#if defined(__LDBL_MANT_DIG__) && __LDBL_MANT_DIG__ == 64\n"
	     "\t(void)v; return 0; /* x87 extended: 10 value bytes in 16, padding not covered */\n"
	     "#endif\n"},
	    {"scalar-int", "", "int v;"},
	    {"scalar-ptr", "", "char *v;"},
	    {"char-array", "", "char v[13];"},
	    {"struct-in-union", "struct SU{char a;int b;};union UU{struct SU s;char c;};",
	     "union UU v;"},
	    {"array-of-union", "union AU{int n;char c[9];};", "union AU v[2];"},
	    {"anon-member", "struct AM{char a;struct{int x;char y;};long z;};", "struct AM v;"},
	    {"enum-member", "enum E{E0,E1};struct EM{char a;enum E e;char b;};", "struct EM v;"},
	};

	CmStats st = {0};
	PrismFeatures f = prism_defaults();
	char src[1200];

	for (size_t c = 0; c < sizeof(cells) / sizeof(cells[0]); c++) {
		snprintf(src, sizeof(src),
			 "%s%s\nint main(void){dirty();%s return nz(&v,sizeof v);}\n", CM_ZI_PRE,
			 cells[c].types, cells[c].decl);
		st.cells++;
		int rc = cm_exec_trap(src, f);
		if (rc != 0)
			cm_note(&st, "%s: %d bytes not zeroed (rc %d)", cells[c].name,
				rc > 0 ? rc : 0, rc);
	}
	cm_report("gen/runtime-zeroinit", &st);
}

/* ── executed auto-static ─────────────────────────────────────────────
 *
 * Auto-static had 539 static cells and none that ran anything, which left the
 * half that matters untested. The existing tiers check that prism REFUSES to
 * promote in the seventeen cases SPEC 6.9 lists. Nothing checked that when it
 * does promote, the program still computes what it computed before.
 *
 * The oracle is differential: build each program twice, once with auto-static
 * on and once with -fno-auto-static, and require the same exit status. Each
 * program folds the array's contents into that status, so a promotion that
 * corrupted an initializer, dropped an element, or moved the wrong object
 * changes the answer.
 *
 * Address identity is deliberately never compared. SPEC 6.9 documents it as
 * the one legitimate difference between the two modes (C11 6.5.9p6 does not
 * guarantee distinct addresses for objects with non-overlapping lifetimes), so
 * a cell that compared &v across recursion depths would fail by design and
 * would be reported as a prism bug by whoever saw it next. */
static void cm_gen_runtime_autostatic(void) {
	/* `promotes` is measured against the shipping binary, not read off the
	 * spec, and both zeroes below are prism declining correctly:
	 *   volatile-member  SPEC 6.9 condition 5, a volatile field must not
	 *                    reach .rodata or volatile access is defeated.
	 *   const-ptr-array  the array is `const int *const`, which 6.9 allows,
	 *                    but the initializer is {&a,&b}: address constants,
	 *                    not the literal tokens the promotion requires.
	 * Recording them as cells rather than deleting them means a future change
	 * that starts promoting either one turns this tier red and gets looked at,
	 * instead of silently moving a documented boundary. */
	static const struct {
		const char *name;
		const char *prog;
		int promotes;
	} cells[] = {
	    {"sbox-sum",
	     "int main(void){const unsigned char t[8]={1,2,3,4,5,6,7,8};int s=0;for(int "
	     "k=0;k<8;k++)s+=t[k];return s%%251;}", 1},
	    {"nested-init",
	     "int main(void){const int t[2][3]={{1,2,3},{4,5,6}};int s=0;for(int k=0;k<2;k++)for(int "
	     "j=0;j<3;j++)s+=t[k][j];return s;}", 1},
	    {"designated",
	     "int main(void){const int t[6]={[1]=7,[4]=9};int s=0;for(int "
	     "k=0;k<6;k++)s+=t[k];return s;}", 1},
	    {"partial-init",
	     "int main(void){const int t[5]={1,2};int s=0;for(int k=0;k<5;k++)s+=t[k];return s;}", 1},
	    {"string-array",
	     "int main(void){const char t[]=\"abc\";int s=0;for(int "
	     "k=0;k<3;k++)s+=t[k];return s%%251;}", 1},
	    {"enum-init",
	     "enum{A=3,B=5};int main(void){const int t[2]={A,B};return t[0]+t[1];}", 1},
	    {"recursive-frame",
	     "static int rec(int d){const int t[4]={1,2,3,4};int s=0;for(int "
	     "k=0;k<4;k++)s+=t[k];return d?s+rec(d-1):s;}int main(void){return rec(3)%%251;}", 1},
	    {"const-ptr-array",
	     "int main(void){static const int a=1,b=2;const int*const t[2]={&a,&b};return "
	     "*t[0]+*t[1];}", 0},
	    {"struct-array",
	     "struct S{int x,y;};int main(void){const struct S t[2]={{1,2},{3,4}};return "
	     "t[0].x+t[0].y+t[1].x+t[1].y;}", 1},
	    {"volatile-member",
	     "struct V{volatile int x;int y;};int main(void){const struct V "
	     "t[2]={{1,2},{3,4}};return t[0].y+t[1].y;}", 0},
	    {"typedef-const",
	     "typedef const int ci;int main(void){ci t[4]={2,4,6,8};int s=0;for(int "
	     "k=0;k<4;k++)s+=t[k];return s;}", 1},
	    {"loop-local",
	     "int main(void){int s=0;for(int q=0;q<3;q++){const int t[3]={1,2,3};s+=t[q];}return s;}", 1},
	    {"shadowed",
	     "int main(void){const int t[2]={9,9};{const int t[2]={1,2};return t[0]+t[1]+0*t[1];}}", 1},
	    {"negative-vals",
	     "int main(void){const int t[4]={-1,-2,3,4};int s=0;for(int "
	     "k=0;k<4;k++)s+=t[k];return s+10;}", 1},
	    {"wide-elems",
	     "int main(void){const long long t[3]={100000LL,200000LL,300000LL};long long s=0;for(int "
	     "k=0;k<3;k++)s+=t[k];return (int)(s%%251);}", 1},
	};

	CmStats st = {0};
	PrismFeatures on = prism_defaults();
	PrismFeatures off = prism_defaults();
	off.auto_static = false;

	for (size_t c = 0; c < sizeof(cells) / sizeof(cells[0]); c++) {
		char src[1200];
		snprintf(src, sizeof(src), cells[c].prog);
		st.cells++;

		int a = cm_exec_trap(src, on);
		int b = cm_exec_trap(src, off);
		if (a < 0 || b < 0) {
			cm_note(&st, "%s: infra (on %d, off %d)", cells[c].name, a, b);
			continue;
		}
		if (a != b) {
			cm_note(&st, "%s: auto-static changed behaviour (on %d, off %d)",
				cells[c].name, a, b);
			continue;
		}
		/* Guard against the differential passing because nothing was
		 * promoted in either build: at least one cell must actually
		 * differ in emitted text, or this tier proves nothing. */
		/* Behavioural agreement is necessary but not sufficient: two builds
		 * that emitted the same text would agree trivially. Check the text
		 * moved exactly where promotion was measured to fire. */
		PrismResult ra = cm_txf(src, on), rb = cm_txf(src, off);
		if (!cm_ok(&ra) || !cm_ok(&rb) || !ra.output || !rb.output) {
			cm_note(&st, "%s: re-transpile failed", cells[c].name);
		} else {
			int moved = strcmp(ra.output, rb.output) != 0;
			if (moved != cells[c].promotes)
				cm_note(&st, "%s: promotion %s but expected %s", cells[c].name,
					moved ? "fired" : "did not fire",
					cells[c].promotes ? "to fire" : "not to");
		}
		prism_free(&ra);
		prism_free(&rb);
	}
	cm_report("gen/runtime-autostatic", &st);
}

/* Read a whole file into a NUL-terminated heap buffer, or NULL. */
static char *cm_slurp(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	size_t cap = 8192, len = 0;
	char *b = malloc(cap);
	if (!b) {
		fclose(f);
		return NULL;
	}
	size_t n;
	while ((n = fread(b + len, 1, cap - len - 1, f)) > 0) {
		len += n;
		if (len + 1 >= cap) {
			size_t nc = cap * 2;
			char *nb = realloc(b, nc);
			if (!nb) break;
			b = nb;
			cap = nc;
		}
	}
	b[len] = '\0';
	fclose(f);
	return b;
}

/* ── CLI feature-flag polarity ────────────────────────────────────────
 *
 * Through 1.1.5 the flag table was asymmetric: -fbounds-check and
 * -fflatten-headers had positive forms, the other eight features accepted only
 * -fno-X. A build inheriting -fno-zeroinit from a parent makefile had no way to
 * turn it back on for one target, and -fzeroinit fell through to the C
 * compiler, which rejected it as an unrecognized option naming a flag the user
 * believed was Prism's.
 *
 * Both directions are checked here. Every feature must round-trip -fno-X then
 * -fX back to its default, and the near-miss GCC flags must still reach CC
 * untouched: -fdefer-pop shares a prefix with -fdefer, -fno-strict-aliasing
 * takes the "no-" branch, and matching either one would break real builds. */
static void cm_gen_flag_polarity(void) {
	static const char *feats[] = {"defer",	  "zeroinit",	     "orelse",
				      "line-directives", "safety",	     "flatten-headers",
				      "auto-unreachable", "auto-static",     "bounds-check",
				      "link-pragma"};
	/* Real compiler flags Prism must NOT claim, including two chosen to
	 * collide with the matcher if it ever loosens to a prefix test. */
	static const char *foreign[] = {"-fdefer-pop", "-fno-strict-aliasing", "-fPIC",
					"-fno-omit-frame-pointer", "-fshort-enums"};

	CmStats st = {0};
	char cmd[PATH_MAX * 2 + 256], src[PATH_MAX], bin[PATH_MAX], dirbuf[PATH_MAX];
	char *dir = test_mkdtemp(dirbuf, "cm_flagpol_");
	if (!dir) {
		cm_note(&st, "could not create temp dir");
		cm_report("gen/flag-polarity", &st);
		return;
	}
	snprintf(bin, sizeof(bin), "%s/prism_flagpol", dir);
	if (!build_test_prism_binary(bin, "flag-polarity: build prism binary")) {
		cm_report("gen/flag-polarity", &st);
		return;
	}
	snprintf(src, sizeof(src), "%s/f.c", dir);
	FILE *f = fopen(src, "w");
	if (!f) {
		cm_note(&st, "could not write temp source");
		cm_report("gen/flag-polarity", &st);
		return;
	}
	fputs("int main(void){int v;return v;}\n", f);
	fclose(f);

	for (size_t i = 0; i < sizeof(feats) / sizeof(feats[0]); i++)
		for (int pol = 0; pol < 3; pol++) {
			const char *form = pol == 0 ? "-f%s" : (pol == 1 ? "-fno-%s" : "-fno-%s -f%s");
			char flag[96];
			if (pol == 2)
				snprintf(flag, sizeof(flag), "-fno-%s -f%s", feats[i], feats[i]);
			else
				snprintf(flag, sizeof(flag), form, feats[i]);
			snprintf(cmd, sizeof(cmd),
				 "'%s' -w -fsyntax-only %s '%s' >/dev/null 2>&1", bin, flag,
				 src);
			st.cells++;
			if (run_command_status(cmd) != 0)
				cm_note(&st, "%s: prism rejected its own flag", flag);
		}

	/* Exit status cannot answer this half. A flag Prism wrongly consumes is
	 * simply dropped, the compile still succeeds, and the check passes while
	 * the user's -fdefer-pop silently stops taking effect. Ask
	 * --prism-verbose what was actually handed to CC instead. Verified by
	 * mutation: loosening the `defer` match to a 5-byte prefix leaves the
	 * status-based version green and turns this one red. */
	char vout[PATH_MAX];
	snprintf(vout, sizeof(vout), "%s/verbose.txt", dir);
	for (size_t i = 0; i < sizeof(foreign) / sizeof(foreign[0]); i++) {
		snprintf(cmd, sizeof(cmd),
			 "'%s' --prism-verbose -w -fsyntax-only %s '%s' >'%s' 2>&1", bin,
			 foreign[i], src, vout);
		st.cells++;
		run_command_status(cmd);
		char *seen = cm_slurp(vout);
		if (!seen || !strstr(seen, foreign[i]))
			cm_note(&st, "%s: consumed by prism, never reaches CC", foreign[i]);
		free(seen);
	}
	/* The mirror: every flag Prism owns must be absent from the CC line, or
	 * the C compiler sees an option it does not know. */
	for (size_t i = 0; i < sizeof(feats) / sizeof(feats[0]); i++) {
		char own[64];
		snprintf(own, sizeof(own), "-f%s", feats[i]);
		snprintf(cmd, sizeof(cmd),
			 "'%s' --prism-verbose -w -fsyntax-only %s '%s' >'%s' 2>&1", bin, own,
			 src, vout);
		st.cells++;
		run_command_status(cmd);
		char *seen = cm_slurp(vout);
		if (seen && strstr(seen, own))
			cm_note(&st, "%s: leaked through to CC", own);
		free(seen);
	}
	unlink(vout);
	unlink(src);
	remove(bin);
	remove(dir);
	cm_report("gen/flag-polarity", &st);
}

/* ── driver surfaces that no test executed ────────────────────────────
 *
 * `install`, `--prism-verify` and @file response files were three of the four
 * areas behind the 1,318-line zero-coverage figure in the 1.1.5 notes: real
 * shipping code that every release went out without running once. They are
 * awkward to test, which is why they were skipped, and each has a way in:
 *
 *   install         honours $PREFIX, so it can be pointed at a temp dir
 *                   instead of /usr/local/bin.
 *   --prism-verify  re-transpiles its own output and demands a fixed point;
 *                   the check below feeds it every feature at once so a
 *                   keyword leaking through any of them would be caught.
 *   @file           parsed by Prism itself before CC ever sees it, and the
 *                   1.1.5 fix for backslash-eating on Windows had no
 *                   regression test at all.
 *
 * Every case asserts an outcome, not merely absence of a crash: the installed
 * binary is executed and must transpile, and the malformed inputs must fail
 * rather than be quietly accepted. */
static void cm_gen_driver_surfaces(void) {
	CmStats st = {0};
	char dirbuf[PATH_MAX], bin[PATH_MAX], src[PATH_MAX], rsp[PATH_MAX], cmd[PATH_MAX * 3];
	char *dir = test_mkdtemp(dirbuf, "cm_driver_");
	if (!dir) {
		cm_note(&st, "could not create temp dir");
		cm_report("gen/driver-surfaces", &st);
		return;
	}
	snprintf(bin, sizeof(bin), "%s/prism_drv", dir);
	if (!build_test_prism_binary(bin, "driver-surfaces: build prism binary")) {
		cm_report("gen/driver-surfaces", &st);
		return;
	}

	/* One source using defer, orelse, zero-init, bounds-check and auto-static
	 * together, so --prism-verify has every feature to re-check at once. */
	snprintf(src, sizeof(src), "%s/all.c", dir);
	FILE *f = fopen(src, "w");
	if (!f) {
		cm_note(&st, "could not write source");
		cm_report("gen/driver-surfaces", &st);
		return;
	}
	fputs("static void sink(int x){(void)x;}\n"
	      "int run(int n){\n"
	      "  const int tbl[4]={1,2,3,4};\n"
	      "  int acc;\n"
	      "  defer sink(acc);\n"
	      "  int v = n orelse 1;\n"
	      "  acc = tbl[v & 3];\n"
	      "  return acc;\n"
	      "}\n"
	      "int main(void){return run(2)==3?0:1;}\n",
	      f);
	fclose(f);

	struct {
		const char *what;
		const char *fmt;
		int want_zero; /* 1 = must succeed, 0 = must fail */
	} runs[] = {
	    {"verify-flag", "'%s' --prism-verify -w -fsyntax-only '%s'", 1},
	    {"verify-env", "PRISM_VERIFY=1 '%s' -w -fsyntax-only '%s'", 1},
	    {"verify-no-safety", "'%s' --prism-verify -fno-safety -w -fsyntax-only '%s'", 1},
	    {"verify-no-line-directives",
	     "'%s' --prism-verify -fno-line-directives -w -fsyntax-only '%s'", 1},
	    {"verify-flatten", "'%s' --prism-verify -fflatten-headers -w -fsyntax-only '%s'", 1},
	};
	for (size_t i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
		snprintf(cmd, sizeof(cmd), runs[i].fmt, bin, src);
		strncat(cmd, " >/dev/null 2>&1", sizeof(cmd) - strlen(cmd) - 1);
		st.cells++;
		int rc = run_command_status(cmd);
		if ((rc == 0) != (runs[i].want_zero != 0))
			cm_note(&st, "%s: rc %d", runs[i].what, rc);
	}

	/* Response files. Prism expands @file itself; a mistake here either drops
	 * arguments silently or, as on Windows through 1.1.4, eats path
	 * separators. */
	struct {
		const char *what;
		const char *body;
		int want_zero;
	} rsps[] = {
	    {"rsp-basic", "-w\n-fsyntax-only\n", 1},
	    {"rsp-crlf", "-w\r\n-fsyntax-only\r\n", 1},
	    {"rsp-blank-lines", "-w\n\n\n-fsyntax-only\n\n", 1},
	    {"rsp-trailing-space", "-w \n-fsyntax-only \n", 1},
	    {"rsp-no-final-newline", "-w\n-fsyntax-only", 1},
	    {"rsp-prism-flag", "-w\n-fsyntax-only\n-fno-zeroinit\n", 1},
	    {"rsp-space-separated", "-w -fsyntax-only\n", 1},
	};
	for (size_t i = 0; i < sizeof(rsps) / sizeof(rsps[0]); i++) {
		snprintf(rsp, sizeof(rsp), "%s/a%zu.rsp", dir, i);
		FILE *rf = fopen(rsp, "wb");
		if (!rf) continue;
		fputs(rsps[i].body, rf);
		fclose(rf);
		snprintf(cmd, sizeof(cmd), "'%s' @'%s' '%s' >/dev/null 2>&1", bin, rsp, src);
		st.cells++;
		int rc = run_command_status(cmd);
		if ((rc == 0) != (rsps[i].want_zero != 0))
			cm_note(&st, "%s: rc %d", rsps[i].what, rc);
		unlink(rsp);
	}

	/* A response file naming a path that does not exist must fail loudly.
	 * Silently continuing would let a build drop half its arguments. */
	snprintf(cmd, sizeof(cmd), "'%s' @'%s/does_not_exist.rsp' '%s' >/dev/null 2>&1", bin, dir,
		 src);
	st.cells++;
	if (run_command_status(cmd) == 0)
		cm_note(&st, "missing response file was accepted");

	/* A path containing a space must survive expansion. */
	{
		char spdir[PATH_MAX], spsrc[PATH_MAX];
		snprintf(spdir, sizeof(spdir), "%s/sp ace", dir);
		mkdir(spdir, 0755);
		snprintf(spsrc, sizeof(spsrc), "%s/x.c", spdir);
		FILE *sf = fopen(spsrc, "w");
		if (sf) {
			fputs("int main(void){int v;return v;}\n", sf);
			fclose(sf);
			snprintf(rsp, sizeof(rsp), "%s/sp.rsp", dir);
			FILE *rf = fopen(rsp, "w");
			if (rf) {
				fprintf(rf, "-w\n-fsyntax-only\n\"%s\"\n", spsrc);
				fclose(rf);
				snprintf(cmd, sizeof(cmd), "'%s' @'%s' >/dev/null 2>&1", bin, rsp);
				st.cells++;
				if (run_command_status(cmd) != 0)
					cm_note(&st, "quoted path with a space was not expanded");
			}
		}
	}

	/* install, redirected away from /usr/local/bin by $PREFIX. The installed
	 * copy is then run, because a corrupted or truncated write would still
	 * leave a file at the right path. */
	{
		char prefix[PATH_MAX], installed[PATH_MAX];
		snprintf(prefix, sizeof(prefix), "%s/pfx", dir);
		snprintf(installed, sizeof(installed), "%s/bin/prism", prefix);

		snprintf(cmd, sizeof(cmd), "PREFIX='%s' '%s' install >/dev/null 2>&1", prefix, bin);
		st.cells++;
		if (run_command_status(cmd) != 0) {
			cm_note(&st, "install failed");
		} else {
			st.cells++;
			if (access(installed, X_OK) != 0) {
				cm_note(&st, "install produced no executable at %s", installed);
			} else {
				snprintf(cmd, sizeof(cmd), "'%s' -w -fsyntax-only '%s' >/dev/null 2>&1",
					 installed, src);
				st.cells++;
				if (run_command_status(cmd) != 0)
					cm_note(&st, "installed binary cannot transpile");
				/* Installing again must be recognised as a no-op rather
				 * than a copy of the running file onto itself, which is
				 * ETXTBSY on Linux. Each spelling below names the same
				 * inode as the destination but writes it differently, so
				 * a textual comparison passes the first and fails the
				 * rest. Verified by mutation: reverting the check to
				 * strcmp leaves the plain absolute case green and turns
				 * the other three red. */
				char alt[PATH_MAX], link[PATH_MAX];
				snprintf(link, sizeof(link), "%s/linked_prism", dir);
				if (symlink(installed, link) != 0) link[0] = '\0';
				const char *spellings[4];
				int nsp = 0;
				spellings[nsp++] = installed;
				snprintf(alt, sizeof(alt), "%s/bin/../bin/prism", prefix);
				spellings[nsp++] = alt;
				if (link[0]) spellings[nsp++] = link;
				for (int k = 0; k < nsp; k++) {
					snprintf(cmd, sizeof(cmd),
						 "PREFIX='%s' '%s' install 2>/dev/null | "
						 "grep -q 'Already installed'",
						 prefix, spellings[k]);
					st.cells++;
					if (run_command_status(cmd) != 0)
						cm_note(&st,
							"reinstall via %s was not recognised as "
							"already installed",
							spellings[k]);
					st.cells++;
					if (access(installed, X_OK) != 0) {
						cm_note(&st, "reinstall via %s destroyed the binary",
							spellings[k]);
						break;
					}
				}
				if (link[0]) unlink(link);
			}
		}
	}

	cm_report("gen/driver-surfaces", &st);
}

/* ── library API as an embedder sees it ───────────────────────────────
 *
 * The suite calls prism_transpile_source thousands of times through cm_txf,
 * so the entry point is far from cold, but nothing checked the properties an
 * embedder actually depends on: that one call cannot influence the next.
 * 1.1.5 moved 39 fields out of PParseContext into a prism.c-owned PrismState,
 * and the arena, the source-defines array and several counters live across
 * calls, so cross-call contamination is the failure mode worth hunting.
 *
 * Three shapes of it are checked. Transpiling A, then something else, then A
 * again must reproduce A byte for byte. A call that FAILS must not poison the
 * next one, which is the sharper case: an error unwinds by longjmp, and 1.1.5
 * fixed exactly this class when in_defer_emit stayed set after a
 * pparse_error skipped its restore. And a feature configuration must give the
 * same answer whichever order the configurations are tried in. */
static void cm_gen_lib_api(void) {
	static const char *A =
	    "static void s(int x){(void)x;}\n"
	    "int fa(int n){const int t[4]={1,2,3,4};int acc;defer s(acc);"
	    "int v=n orelse 1;acc=t[v&3];return acc;}\n";
	static const char *B =
	    "int fb(int n){int m[2][3];raw int r;int q=n orelse 2;(void)r;return m[q&1][q%3];}\n";

	/* Each must be rejected, and each takes a different diagnostic path out. */
	static const struct {
		const char *name;
		const char *src;
	} bad[] = {
	    {"return-in-defer", "int f(void){defer { return 1; } return 0;}\n"},
	    {"ptr-arith-deref", "int f(void){int a[4];volatile int i=0;return *(a+i);}\n"},
	    {"defer-missing-semi", "void g(void);int f(void){defer g()\nreturn 0;}\n"},
	    {"commutative-subscript", "int f(void){int a[4];volatile int i=0;return i[a];}\n"},
	    {"file-scope-defer", "defer f();\nint main(void){return 0;}\n"},
	};

	CmStats st = {0};
	PrismFeatures d = prism_defaults();

	char *base = NULL;
	PrismResult r0 = cm_txf(A, d);
	if (cm_ok(&r0) && r0.output) base = strdup(r0.output);
	prism_free(&r0);
	if (!base) {
		cm_note(&st, "baseline transpile failed");
		cm_report("gen/lib-api", &st);
		return;
	}

	/* An unrelated transpile between two identical ones must not change the
	 * second. */
	for (int rep = 0; rep < 3; rep++) {
		PrismResult rb = cm_txf(B, d);
		prism_free(&rb);
		PrismResult ra = cm_txf(A, d);
		st.cells++;
		if (!cm_ok(&ra) || !ra.output || strcmp(ra.output, base) != 0)
			cm_note(&st, "output drifted after an unrelated transpile (rep %d)", rep);
		prism_free(&ra);
	}

	/* A rejected input must leave nothing behind. */
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		PrismResult rbad = cm_txf(bad[i].src, d);
		st.cells++;
		if (cm_ok(&rbad)) cm_note(&st, "%s: accepted, expected a diagnostic", bad[i].name);
		/* A failed result must carry a message, or the embedder has nothing
		 * to show its user. */
		st.cells++;
		if (!cm_ok(&rbad) && (!rbad.error_msg || !rbad.error_msg[0]))
			cm_note(&st, "%s: rejected with an empty error message", bad[i].name);
		/* An embedder reports the location, so it has to be populated. */
		st.cells++;
		if (!cm_ok(&rbad) && rbad.error_line <= 0)
			cm_note(&st, "%s: rejected with error_line %d", bad[i].name,
				rbad.error_line);
		prism_free(&rbad);

		PrismResult after = cm_txf(A, d);
		st.cells++;
		if (!cm_ok(&after) || !after.output || strcmp(after.output, base) != 0)
			cm_note(&st, "%s: contaminated the following transpile", bad[i].name);
		prism_free(&after);
	}

	/* Feature configurations, each tried in both directions. A configuration
	 * that only works when it runs first is a state bug, not a feature. */
	PrismFeatures cfg[6];
	const char *cname[6] = {"defaults",   "no-defer",  "no-orelse",
				"no-zeroinit", "no-bounds", "no-auto-static"};
	for (int i = 0; i < 6; i++) cfg[i] = prism_defaults();
	cfg[1].defer = false;
	cfg[2].orelse = false;
	cfg[3].zeroinit = false;
	cfg[4].bounds_check = false;
	cfg[5].auto_static = false;

	char *firstrun[6] = {0};
	for (int i = 0; i < 6; i++) {
		PrismResult r = cm_txf(A, cfg[i]);
		if (cm_ok(&r) && r.output) firstrun[i] = strdup(r.output);
		prism_free(&r);
	}
	for (int pass = 0; pass < 2; pass++)
		for (int k = 0; k < 6; k++) {
			int i = pass ? 5 - k : k; /* forwards, then backwards */
			PrismResult r = cm_txf(A, cfg[i]);
			st.cells++;
			if (!firstrun[i] || !cm_ok(&r) || !r.output ||
			    strcmp(r.output, firstrun[i]) != 0)
				cm_note(&st, "config %s drifted on pass %d", cname[i], pass);
			prism_free(&r);
		}
	for (int i = 0; i < 6; i++) free(firstrun[i]);

	/* Soft keywords. Every Prism keyword is a legal C identifier, and code
	 * that predates Prism is full of variables called `raw`. Declaring one
	 * must suppress the keyword meaning for that name. */
	static const struct {
		const char *name;
		const char *src;
		const char *must_contain;
	} soft[] = {
	    {"defer-as-var", "void f(void){int defer=5;defer;(void)defer;}\n", "defer"},
	    {"orelse-as-var", "void f(void){int orelse=5;(void)orelse;}\n", "orelse"},
	    {"raw-as-var", "void f(void){int raw=5;(void)raw;}\n", "raw"},
	    {"defer-as-param", "void f(int defer){(void)defer;}\n", "defer"},
	    {"raw-as-member", "struct S{int raw;};int f(struct S s){return s.raw;}\n", "raw"},
	};
	for (size_t i = 0; i < sizeof(soft) / sizeof(soft[0]); i++) {
		PrismResult r = cm_txf(soft[i].src, d);
		st.cells++;
		if (!cm_ok(&r) || !r.output || !strstr(r.output, soft[i].must_contain))
			cm_note(&st, "%s: identifier did not survive", soft[i].name);
		prism_free(&r);
	}

	/* `defer;` with no declaration in scope is accepted as an empty defer:
	 * the keyword is consumed and an empty statement is scheduled at scope
	 * exit, which is why the call below is emitted ahead of it. Nothing is
	 * deferred. Pinned rather than endorsed. Prism rejects `defer g()` for a
	 * missing semicolon, so accepting a defer with no body at all is the
	 * inconsistent case; if that is ever tightened to a diagnostic, this cell
	 * is where it shows up. */
	{
		PrismResult r = cm_txf("void g(void);void f(void){defer; g();}\n", d);
		st.cells++;
		if (!cm_ok(&r)) {
			cm_note(&st, "bare `defer;` now rejected: intended? update this cell");
		} else if (r.output) {
			const char *g = strstr(r.output, "g();");
			const char *tail = g ? strstr(g, ";") : NULL;
			if (!g || !tail)
				cm_note(&st, "bare `defer;` no longer lowers to an empty defer");
		}
		prism_free(&r);
	}

	free(base);
	cm_report("gen/lib-api", &st);
}

#endif /* !_WIN32: POSIX-only executed tiers */

/* ── zero-init leaves no reachable byte, per type ─────────────────────
 *
 * `long double` is the one scalar in C with padding bytes, and on x86 Prism's
 * `long double v = 0;` writes the 10-byte x87 value and leaves the rest of the
 * 16-byte slot holding stack. This sweeps the family rather than that one type.
 *
 * The first version of this tier tried to decide "does this type have padding"
 * at runtime by writing zero through a `T*` and comparing two poisoned objects.
 * That was wrong, and CI caught it: a store through a pointer does not lower
 * the same way as a declaration with an initializer, so on x86 the probe
 * reported no padding for the very type that has it. Modelling the construct
 * indirectly was the mistake. The measurement is direct now:
 *
 *   nz_prism  bytes left nonzero after Prism zero-initializes the object
 *   nz_memset bytes left nonzero after an explicit memset of the same object
 *
 * `nz_memset` must be 0 by definition; it is measured anyway, because if it
 * ever is not, the harness is broken rather than Prism. `nz_prism` is what the
 * feature promises. Types where Prism currently falls short carry `gap = 1`,
 * which turns the cell into a recorded boundary instead of a failure, so the
 * shortfall stays visible and the tier goes red if it spreads to a type not on
 * the list or disappears from one that is. */
#define CM_ZP_PRE                                                                                    \
	"#include <string.h>\n"                                                                      \
	"__attribute__((noinline)) static void dirty(void){volatile unsigned char "                  \
	"j[512];for(int k=0;k<512;k++)j[k]=0xAA;(void)j;}\n"                                          \
	"static int nz(const void*p,unsigned long n){const unsigned char*b=p;int "                    \
	"k=0;for(unsigned long q=0;q<n;q++)if(b[q])k++;return k;}\n"

static void cm_gen_zeroinit_padding(void) {
	static const struct {
		const char *name;
		const char *types;
		int gap; /* 1 = Prism is known not to cover every byte of this type */
	} cells[] = {
	    /* The x87 family. sizeof is 16 (or 12 on 32-bit x86), the value is 10
	     * bytes, and a zero initializer writes only the value. Elsewhere
	     * `long double` is a 128-bit quad with no padding and these pass with
	     * gap unused, which is why the flag records rather than asserts. */
	    {"long-double", "typedef long double T;", 1},
	    {"complex-long-double", "typedef long double _Complex T;", 1},
	    {"volatile-long-double", "typedef volatile long double T;", 1},
	    {"ld-array", "typedef long double T[3];", 1},
	    {"ld-2d-array", "typedef long double T[2][2];", 1},
	    {"ld-in-struct", "typedef struct { long double d; } T;", 1},
	    {"ld-with-tail", "typedef struct { long double d; char c; } T;", 1},
	    {"ld-union", "typedef union { long double d; char c[4]; } T;", 1},
	    {"ld-nested", "typedef struct { char a; struct { long double d; } in; } T;", 1},
	    /* Everything else must be covered completely, everywhere. */
	    {"double", "typedef double T;", 0},
	    {"float", "typedef float T;", 0},
	    {"bool", "typedef _Bool T;", 0},
	    {"char", "typedef char T;", 0},
	    {"short", "typedef short T;", 0},
	    {"int", "typedef int T;", 0},
	    {"long", "typedef long T;", 0},
	    {"long-long", "typedef long long T;", 0},
	    {"void-ptr", "typedef void *T;", 0},
	    {"fn-ptr", "typedef void (*T)(void);", 0},
	    {"complex-double", "typedef double _Complex T;", 0},
	    {"struct-padding", "typedef struct { char a; int b; char c; } T;", 0},
	    {"bitfield", "typedef struct { unsigned x : 3; unsigned y : 5; int t; } T;", 0},
	    {"array-of-struct", "typedef struct { char a; int b; } T[3];", 0},
	    {"anon-member", "typedef struct { char a; struct { int x; char y; }; long z; } T;", 0},
	};

	CmStats st = {0};
	PrismFeatures f = prism_defaults();
	long recorded = 0;
	char first_gap[96] = {0};
	char src[1800];

	for (size_t c = 0; c < sizeof(cells) / sizeof(cells[0]); c++) {
		/* Two separate noinline frames so each declaration meets a stack the
		 * same dirty() just filled with 0xAA. Exit status: bit 0 = Prism left
		 * a nonzero byte, bit 1 = an explicit memset did too, which would
		 * mean the measurement itself is untrustworthy. */
		snprintf(src, sizeof(src),
			 "%s%s\n"
			 "__attribute__((noinline)) static int via_prism(void){ dirty(); T v;"
			 " return nz(&v, sizeof(T)) != 0; }\n"
			 "__attribute__((noinline)) static int via_memset(void){ dirty(); T v;"
			 " memset(&v, 0, sizeof(T)); return nz(&v, sizeof(T)) != 0; }\n"
			 "int main(void){ return via_prism() | (via_memset() << 1); }\n",
			 CM_ZP_PRE, cells[c].types);

		st.cells++;
		int rc = cm_exec_trap(src, f);
		if (rc < 0) {
			cm_note(&st, "%s: infra %d", cells[c].name, rc);
			continue;
		}
		if (rc & 2) {
			cm_note(&st, "%s: explicit memset left a nonzero byte, harness is wrong",
				cells[c].name);
			continue;
		}
		if (!(rc & 1)) continue; /* fully covered */
		if (!cells[c].gap) {
			cm_note(&st, "%s: zero-init left a nonzero byte", cells[c].name);
			continue;
		}
		recorded++;
		if (!first_gap[0]) snprintf(first_gap, sizeof(first_gap), "%s", cells[c].name);
	}

	char name[288];
	snprintf(name, sizeof(name),
		 "completeness[gen/zeroinit-padding]: %ld cells, %ld bad, %ld known-gap%s%s%s %s",
		 st.cells, st.bad, recorded, recorded ? " (first: " : "", recorded ? first_gap : "",
		 recorded ? ")" : "", st.bad ? st.first : "");
	CHECK(st.bad == 0, name);
}

/* ── preprocessed-output cache ────────────────────────────────────────
 *
 * Thirteen functions and roughly 536 lines shipped in 1.1.5, on by default,
 * with no test of any kind: not one reference to pp_cache, PRISM_NO_PP_CACHE,
 * --prism-cache-info or --prism-cache-clear anywhere in the suite. It is the
 * highest-risk code in the tree because its failure mode is the only silent
 * one. Every other defect announces itself with a diagnostic, a wrong exit
 * status or a crash. A stale cache entry produces a clean build of source the
 * user already changed, with correct #line directives and no complaint.
 *
 * One oracle covers all of it: whatever the cache does, transpiling with it
 * enabled must be byte-identical to transpiling with PRISM_NO_PP_CACHE=1. Any
 * staleness, any key collision, any missed dependency shows up as a diff. The
 * cells are sequences of edits, because a cache can only go wrong on the
 * second visit; a single cold transpile proves nothing.
 *
 * PRISM_PP_CACHE_DIR points the whole tier at a private directory, so it can
 * neither pollute the developer's cache nor be perturbed by it. */
static void cm_gen_pp_cache(void) {
	CmStats st = {0};
	char dirbuf[PATH_MAX], bin[PATH_MAX], cache[PATH_MAX], cmd[PATH_MAX * 4];
	char src[PATH_MAX], hdr[PATH_MAX], hdr2[PATH_MAX], warm[PATH_MAX], cold[PATH_MAX];

	char *dir = test_mkdtemp(dirbuf, "cm_ppcache_");
	if (!dir) {
		cm_note(&st, "could not create temp dir");
		cm_report("gen/pp-cache", &st);
		return;
	}
	snprintf(bin, sizeof(bin), "%s/prism_ppc", dir);
	if (!build_test_prism_binary(bin, "pp-cache: build prism binary")) {
		cm_report("gen/pp-cache", &st);
		return;
	}
	snprintf(cache, sizeof(cache), "%s/ppcache", dir);
	snprintf(src, sizeof(src), "%s/m.c", dir);
	snprintf(hdr, sizeof(hdr), "%s/a.h", dir);
	snprintf(hdr2, sizeof(hdr2), "%s/b.h", dir);
	snprintf(warm, sizeof(warm), "%s/warm.c", dir);
	snprintf(cold, sizeof(cold), "%s/cold.c", dir);

	/* Write `text` to `path`. */
#define CM_PPC_WRITE(path, text)                                                                     \
	do {                                                                                         \
		FILE *wf = fopen((path), "w");                                                        \
		if (wf) {                                                                            \
			fputs((text), wf);                                                           \
			fclose(wf);                                                                  \
		}                                                                                    \
	} while (0)

	/* Transpile once with the cache and once without, and require the two to
	 * agree byte for byte. `extra` carries any flags the scenario needs. */
#define CM_PPC_CHECK(extra, label)                                                                   \
	do {                                                                                         \
		st.cells++;                                                                          \
		snprintf(cmd, sizeof(cmd),                                                           \
			 "PRISM_PP_CACHE_DIR='%s' '%s' %s transpile '%s' >'%s' 2>/dev/null", cache,   \
			 bin, (extra), src, warm);                                                   \
		int rc_w = run_command_status(cmd);                                                  \
		snprintf(cmd, sizeof(cmd),                                                           \
			 "PRISM_NO_PP_CACHE=1 PRISM_PP_CACHE_DIR='%s' '%s' %s transpile '%s' >'%s' "  \
			 "2>/dev/null",                                                              \
			 cache, bin, (extra), src, cold);                                            \
		int rc_c = run_command_status(cmd);                                                  \
		if (rc_w != rc_c) {                                                                  \
			cm_note(&st, "%s: exit differs, cached %d uncached %d", (label), rc_w,        \
				rc_c);                                                               \
		} else {                                                                             \
			char *a = cm_slurp(warm), *b = cm_slurp(cold);                               \
			if (!a || !b)                                                                \
				cm_note(&st, "%s: could not read output", (label));                  \
			else if (strcmp(a, b) != 0)                                                  \
				cm_note(&st, "%s: cached output differs from uncached", (label));    \
			free(a);                                                                     \
			free(b);                                                                     \
		}                                                                                    \
	} while (0)

	CM_PPC_WRITE(src, "#include \"a.h\"\nint main(void){ return VALUE; }\n");
	CM_PPC_WRITE(hdr, "#define VALUE 1\n");
	CM_PPC_CHECK("", "cold");
	CM_PPC_CHECK("", "warm-unchanged");

	/* A transitive header changes. The dependency list is recovered from the
	 * `# N "file"` linemarkers in the preprocessed text, not from a .d file,
	 * so this is the path most likely to miss an edit. */
	CM_PPC_WRITE(hdr, "#define VALUE 2\n");
	CM_PPC_CHECK("", "header-edited");

	/* Same length, different content: a size-only staleness check passes this
	 * and serves the old text. */
	CM_PPC_WRITE(hdr, "#define VALUE 3\n");
	CM_PPC_CHECK("", "header-same-length");

	/* Several rewrites inside one second. Filesystems with coarse timestamps
	 * cannot distinguish these by mtime alone. */
	CM_PPC_WRITE(hdr, "#define VALUE 4\n");
	CM_PPC_CHECK("", "same-second-1");
	CM_PPC_WRITE(hdr, "#define VALUE 5\n");
	CM_PPC_CHECK("", "same-second-2");
	CM_PPC_WRITE(hdr, "#define VALUE 6\n");
	CM_PPC_CHECK("", "same-second-3");

	/* Growing and shrinking the header shifts every line number after it. */
	CM_PPC_WRITE(hdr, "\n\n\n#define VALUE 7\n");
	CM_PPC_CHECK("", "header-grown");
	CM_PPC_WRITE(hdr, "#define VALUE 8\n");
	CM_PPC_CHECK("", "header-shrunk");

	/* Nesting: the edited file is two levels down. */
	CM_PPC_WRITE(hdr, "#include \"b.h\"\n#define VALUE INNER\n");
	CM_PPC_WRITE(hdr2, "#define INNER 9\n");
	CM_PPC_CHECK("", "nested-cold");
	CM_PPC_WRITE(hdr2, "#define INNER 10\n");
	CM_PPC_CHECK("", "nested-inner-edited");

	/* The source itself changes while its headers do not. */
	CM_PPC_WRITE(src, "#include \"a.h\"\nint main(void){ return VALUE + 1; }\n");
	CM_PPC_CHECK("", "source-edited");

	/* Flags are part of the key. Same files, different -D, must not collide. */
	CM_PPC_WRITE(src, "#include \"a.h\"\n#ifdef PICK\nint main(void){return 1;}\n#else\nint "
			  "main(void){return 2;}\n#endif\n");
	CM_PPC_CHECK("", "flags-none");
	CM_PPC_CHECK("-DPICK=1", "flags-D-added");
	CM_PPC_CHECK("", "flags-D-removed");
	CM_PPC_CHECK("-DPICK=2", "flags-D-changed");

	/* Two directories holding a same-named header: only the include path
	 * distinguishes them, so a key that ignores -I serves the wrong one. */
	{
		char d1[PATH_MAX], d2[PATH_MAX], p1[PATH_MAX], p2[PATH_MAX], inc[PATH_MAX * 2];
		snprintf(d1, sizeof(d1), "%s/i1", dir);
		snprintf(d2, sizeof(d2), "%s/i2", dir);
		mkdir(d1, 0755);
		mkdir(d2, 0755);
		snprintf(p1, sizeof(p1), "%s/same.h", d1);
		snprintf(p2, sizeof(p2), "%s/same.h", d2);
		CM_PPC_WRITE(p1, "#define WHICH 100\n");
		CM_PPC_WRITE(p2, "#define WHICH 200\n");
		CM_PPC_WRITE(src, "#include \"same.h\"\nint main(void){ return WHICH % 7; }\n");
		snprintf(inc, sizeof(inc), "-I'%s'", d1);
		CM_PPC_CHECK(inc, "include-path-1");
		snprintf(inc, sizeof(inc), "-I'%s'", d2);
		CM_PPC_CHECK(inc, "include-path-2");
		snprintf(inc, sizeof(inc), "-I'%s'", d1);
		CM_PPC_CHECK(inc, "include-path-back-to-1");
	}

	/* A header that disappears and returns with different content. */
	CM_PPC_WRITE(src, "#include \"a.h\"\nint main(void){ return VALUE; }\n");
	CM_PPC_WRITE(hdr, "#define VALUE 11\n");
	CM_PPC_CHECK("", "recreate-before");
	unlink(hdr);
	CM_PPC_WRITE(hdr, "#define VALUE 12\n");
	CM_PPC_CHECK("", "recreate-after");

	/* Build-time date macros must never be served from cache, because the
	 * answer changes without any input changing. */
	CM_PPC_WRITE(src, "static const char *d = __DA" "TE__;\nint main(void){ return d[0]==0; }\n");
	CM_PPC_CHECK("", "date-macro");
	CM_PPC_WRITE(src, "static const char *t = __TI" "ME__;\nint main(void){ return t[0]==0; }\n");
	CM_PPC_CHECK("", "time-macro");

	/* --prism-cache-info and --prism-cache-clear are user-facing and were
	 * never run. Clearing must leave the cache usable, not merely empty. */
	snprintf(cmd, sizeof(cmd), "PRISM_PP_CACHE_DIR='%s' '%s' --prism-cache-info >/dev/null 2>&1",
		 cache, bin);
	st.cells++;
	if (run_command_status(cmd) != 0) cm_note(&st, "--prism-cache-info failed");
	snprintf(cmd, sizeof(cmd),
		 "PRISM_PP_CACHE_DIR='%s' '%s' --prism-cache-clear >/dev/null 2>&1", cache, bin);
	st.cells++;
	if (run_command_status(cmd) != 0) cm_note(&st, "--prism-cache-clear failed");
	CM_PPC_WRITE(src, "#include \"a.h\"\nint main(void){ return VALUE; }\n");
	CM_PPC_WRITE(hdr, "#define VALUE 13\n");
	CM_PPC_CHECK("", "after-cache-clear");
	CM_PPC_WRITE(hdr, "#define VALUE 14\n");
	CM_PPC_CHECK("", "after-clear-then-edit");

#undef CM_PPC_WRITE
#undef CM_PPC_CHECK
	cm_report("gen/pp-cache", &st);
}

void run_completeness_open_tests(void) {
	printf("\n=== COMPLETENESS EXECUTED TIERS ===\n");
	/* The compile-and-run oracles live on this second suite thread rather
	 * than inside run_completeness_tests: each cell spawns cc and then the
	 * built binary, so they are latency-bound, not CPU-bound, and running
	 * them beside the static product halves the suite's wall clock. */
	cm_gen_goto_open();
#ifndef _WIN32
	cm_gen_cert_compile_run();
	cm_gen_raw_identifier();
	cm_gen_raw_string();
	cm_gen_source_defines();
	cm_gen_runtime_defer_product();
	cm_gen_orelse_product();
	cm_gen_orelse_defer();
	cm_gen_passthrough();
	cm_gen_defer_depth();
	cm_gen_defer_edges();
	cm_gen_defer_wide();
	cm_gen_defer_reject();
	cm_gen_runtime_bounds_trap();
	cm_gen_runtime_zeroinit();
	cm_gen_runtime_autostatic();
	cm_gen_flag_polarity();
	cm_gen_driver_surfaces();
	cm_gen_lib_api();
	cm_gen_zeroinit_padding();
	cm_gen_pp_cache();
#endif
}

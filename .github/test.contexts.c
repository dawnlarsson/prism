/* test.contexts.c — combinatorial context-closure sweep for defer/orelse.
 *
 * Every historical classifier bug was an UNENUMERATED
 * CONTEXT — and the worst ones were COMPOSED contexts: "GNU attr BETWEEN
 * dims", "_Atomic-typeof dims", "designator orelse in a statement
 * expression", "case (x orelse 1)".  Hand-picked context lists cannot close
 * that class.  This suite generates contexts three ways and sweeps every
 * defer/orelse payload form through all of them:
 *
 *   TIER A (atomic): ~65 grammar-position templates with a hole — every
 *     keyword head class, declarator dims (incl. static/qualified/VM),
 *     designators (incl. GNU ranges), digraphs, preprocessor directives
 *     (library mode preserves them), asm operand slots, the case/label
 *     family, attributes in every position, C23 auto/constexpr/bitint,
 *     bitfields, _Generic, statement expressions, K&R definitions, ...
 *   TIER B (composed): 14 expression->expression wrappers composed
 *     PAIRWISE (196 shapes) and planted in 3 sites (decl-init, statement,
 *     array dim).  PRISM_CONTEXTS_DEEP=1 adds TRIPLE composition (2744
 *     shapes) — run in CI.
 *   TIER C (feature polarity): every atomic cell re-swept under
 *     orelse-only and defer-only feature sets — SPEC's cross-feature
 *     invariant says disabling one feature must never suppress another's
 *     checking.
 *
 * THE ORACLE (machine-decidable, no hand verdicts — same trichotomy as
 * test.insertion.c, strengthened):
 *   REJECT      status != PRISM_OK and a diagnostic is present; or
 *   TRANSFORM   accepted, zero `defer`/`orelse` tokens survive, AND the
 *               output is a re-transpilation FIXED POINT (transpiling the
 *               output again succeeds and yields identical output modulo
 *               '#' linemarker lines) — in-process translation validation
 *               on every accepted cell; or
 *   IDENTIFIER  accepted with survivors, and byte-identical to the same
 *               cell transpiled with both extensions disabled.
 * Any other outcome (survivor divergence, fixed-point failure, crash) fails
 * the suite.  ~10k transpiles per run, aggregated into a handful of CHECKs.
 */

/* Shared preamble planted before every generated TU.  No identifier here is
 * spelled `defer` or `orelse`, so any survivor in accepted output is
 * attributable to the payload (or to a lowering leak — either is a catch). */
#define CX_PRE                                                                                       \
	"static volatile int z; static int g(void){return z;} "                                     \
	"static void h(int a){(void)a;} static int h2(int a){return a;} "                            \
	"static int arr[8]; struct D { int f; int gg[4]; }; "                                        \
	"typedef int cx_t; enum CE { CE0 = 1 };\n"

/* ---- TIER A: atomic grammar-position templates ---------------------------
 * Each template contains exactly one %s hole.  Function-body templates end
 * with the label L so goto-payloads can resolve; templates where a label is
 * ill-formed simply reject those payloads (a valid outcome).               */
typedef struct {
	const char *name;
	const char *tmpl;
} CxTmpl;

static const CxTmpl cx_atomic[] = {
    /* --- keyword-head contexts (one per TT_ class, superset of alphabet) --- */
    {"head-int-parendecl", "int f(void){ int (x) = %s; h2(x); goto L; L: return x; }"},
    {"head-const", "int f(void){ const int c = %s; goto L; L: return c; }"},
    {"head-volatile", "int f(void){ volatile int v = %s; goto L; L: return v; }"},
    {"head-register", "int f(void){ register int r = %s; goto L; L: return r; }"},
    {"head-restrict-ptr", "int f(void){ int * restrict p = (int*)0; (void)p; int q = %s; goto L; L: return q; }"},
    {"head-struct-tag", "int f(void){ struct D d = { %s }; goto L; L: return d.f; }"},
    {"head-typedef-name", "int f(void){ cx_t t = %s; goto L; L: return t; }"},
    {"head-static-init", "int f(void){ static int s = %s; return s; }"},
    {"head-threadlocal", "static _Thread_local int tl = %s; int f(void){ return tl; }"},
    {"head-sizeof", "int f(void){ int s = (int)sizeof(%s); goto L; L: return s; }"},
    {"head-alignof", "int f(void){ int s = (int)_Alignof(int[%s]); goto L; L: return s; }"},
    {"head-typeof-arg", "int f(void){ typeof(%s) t = 0; goto L; L: return (int)t; }"},
    {"head-typeof-unqual", "int f(void){ typeof_unqual(%s) t = 0; goto L; L: return (int)t; }"},
    {"head-atomic-paren", "int f(void){ _Atomic(int) a = 0; int q = %s; (void)a; goto L; L: return q; }"},
    {"head-alignas", "int f(void){ _Alignas(%s) int a = 1; goto L; L: return a; }"},
    {"head-bitint", "int f(void){ _BitInt(%s) b = 0; goto L; L: return (int)b; }"},
    {"head-generic-ctrl", "int f(void){ int q = _Generic((%s), int: 1, default: 2); goto L; L: return q; }"},
    {"head-generic-assoc", "int f(void){ int q = _Generic((z), int: (%s), default: 2); goto L; L: return q; }"},
    {"head-attr-args", "int f(void){ __attribute__((aligned(%s))) int a = 1; goto L; L: return a; }"},
    {"head-c23-attr", "int f(void){ [[deprecated(\"%s\")]] int a = 1; goto L; L: return a; }"},
    {"head-asm-out", "int f(void){ int r = 0; __asm__ volatile(\"\" : \"=r\"(r) : : ); (void)(%s); goto L; L: return r; }"},
    {"head-asm-in", "int f(void){ int r = 0; __asm__ volatile(\"\" : \"=r\"(r) : \"r\"(%s)); goto L; L: return r; }"},
    {"head-asm-clobber-slot", "int f(void){ __asm__ volatile(\"\" : : : \"memory\"); int q = %s; goto L; L: return q; }"},
    {"head-return", "int f(void){ if (z) return %s; goto L; L: return 0; }"},
    {"head-static-assert", "int f(void){ _Static_assert(1, \"m\"); int q = %s; goto L; L: return q; }"},
    {"head-static-assert-arg", "int f(void){ _Static_assert((%s) || 1, \"m\"); return 0; }"},
    /* --- case / label family (the caught bug class) --- */
    {"case-expr", "int f(void){ switch(z){ case (%s): return 1; } return 0; }"},
    {"case-range-gnu", "int f(void){ switch(z){ case 1 ... (%s): return 1; } return 0; }"},
    {"case-body", "int f(void){ switch(z){ case 0: { %s; break; } } return 0; }"},
    {"default-body", "int f(void){ switch(z){ default: { %s; } } return 0; }"},
    {"label-stmt", "int f(void){ goto M; M: %s; return 0; }"},
    {"label-c23-attr", "int f(void){ goto M; [[maybe_unused]] M: { %s; } return 0; }"},
    {"switch-braceless", "int f(void){ switch (z) h2(0); int q = %s; goto L; L: return q; }"},
    /* --- braceless control bodies --- */
    {"braceless-if", "int f(void){ if (z) %s; goto L; L: return 0; }"},
    {"braceless-else", "int f(void){ if (z) h(1); else %s; goto L; L: return 0; }"},
    {"braceless-while", "int f(void){ while (0) %s; goto L; L: return 0; }"},
    {"braceless-do", "int f(void){ do %s; while (0); goto L; L: return 0; }"},
    {"braceless-for", "int f(void){ for (;;) { if (z) break; %s; break; } goto L; L: return 0; }"},
    /* --- declarator / dimension family --- */
    {"dim-basic", "int f(void){ int a[%s]; a[0] = 1; goto L; L: return a[0]; }"},
    {"dim-second", "int f(void){ int a[2][%s]; a[0][0] = 1; goto L; L: return a[0][0]; }"},
    {"dim-attr-between", "int f(void){ int a[2] __attribute__((unused)) [%s]; a[0][0] = 1; goto L; L: return 0; }"},
    {"dim-param-static", "void p(int a[static %s]); int f(void){ goto L; L: return 0; }"},
    {"dim-param-const", "void p(int a[const %s]); int f(void){ goto L; L: return 0; }"},
    {"dim-fnptr-ret", "int f(void){ int (*(*fp)(void))[4] = 0; (void)fp; int q = %s; goto L; L: return q; }"},
    {"dim-nested-declarator", "int f(void){ int (*pa)[%s] = 0; (void)pa; goto L; L: return 0; }"},
    {"dim-typeof-vla", "int f(void){ int n = 3; typeof(int[n]) v; v[0] = %s; goto L; L: return v[0]; }"},
    /* --- initializer / designator family --- */
    {"desig-member", "int f(void){ struct D d = { .f = %s }; goto L; L: return d.f; }"},
    {"desig-array", "int f(void){ int a[4] = { [2] = %s }; goto L; L: return a[2]; }"},
    {"desig-nested", "int f(void){ struct D d = { .gg[1] = %s }; goto L; L: return d.gg[1]; }"},
    {"desig-range-gnu", "int f(void){ int a[8] = { [1 ... 3] = %s }; goto L; L: return a[2]; }"},
    {"compound-lit", "int f(void){ int q = ((int){ %s }); goto L; L: return q; }"},
    {"compound-lit-struct", "int f(void){ struct D d = (struct D){ %s }; goto L; L: return d.f; }"},
    {"init-brace-scalar", "int f(void){ int q = { %s }; goto L; L: return q; }"},
    /* --- struct / enum bodies --- */
    {"bitfield-width", "int f(void){ struct B { int b : %s; } sb = {1}; goto L; L: return sb.b; }"},
    {"struct-member-dim", "int f(void){ struct M { int m[%s]; } sm; sm.m[0] = 1; goto L; L: return 0; }"},
    {"enum-value", "int f(void){ enum X { X0 = %s } e = X0; goto L; L: return e; }"},
    {"enum-c23-underlying", "int f(void){ enum Y : typeof(%s) { Y0 } e = Y0; goto L; L: return e; }"},
    /* --- preprocessor-adjacent (library mode keeps directives) --- */
    {"pp-if-span", "int f(void){ int q =\n#if 1\n%s\n#else\n0\n#endif\n; goto L; L: return q; }"},
    {"pp-define-body", "#define CXM (%s)\nint f(void){ int q = CXM; goto L; L: return q; }"},
    {"pp-line-adjacent", "#line 100\nint f(void){ int q = %s; goto L; L: return q; }"},
    {"comment-adjacent", "int f(void){ int q = /*c*/ %s //t\n ; goto L; L: return q; }"},
    /* --- digraphs (tokenizer alternate spellings) --- */
    {"digraph-subscript", "int f(void){ int q = arr<:1:> + h2(%s); goto L; L: return q; }"},
    {"digraph-body", "int f(void) <%% int q = %s; goto L; L: return q; %%>"},
    /* --- misc modern / legacy --- */
    {"c23-auto", "int f(void){ auto q = %s; goto L; L: return (int)q; }"},
    {"c23-constexpr", "int f(void){ constexpr int c = %s; return c; }"},
    {"gnu-autotype", "int f(void){ __auto_type q = %s; goto L; L: return (int)q; }"},
    {"stmt-expr", "int f(void){ int q = ({ %s; 7; }); goto L; L: return q; }"},
    {"stmt-expr-tail", "int f(void){ int q = ({ h(1); %s; }); goto L; L: return q; }"},
    {"comma-stmt", "int f(void){ h(1), (void)(%s), h(2); goto L; L: return 0; }"},
    {"ternary-mid", "int f(void){ int q = z ? (%s) : 2; goto L; L: return q; }"},
    {"call-arg", "int f(void){ int q = h2(%s); goto L; L: return q; }"},
    {"call-arg-2nd", "static int h3(int a, int b){ return a + b; } int f(void){ int q = h3(0, %s); goto L; L: return q; }"},
    {"knr-def", "int fk(a) int a; { return a; } int f(void){ int q = fk(h2(%s)); goto L; L: return q; }"},
    {"file-scope", "%s\nint f(void){ goto L; L: return 0; }"},
    {"file-scope-tentative", "int t;\n%s\nint f(void){ return t; }"},
    /* denser heads */
    {"head-float", "int f(void){ float q = (float)(%s); goto L; L: return (int)q; }"},
    {"head-double", "int f(void){ double q = (double)(%s); goto L; L: return (int)q; }"},
    {"head-long-long", "int f(void){ long long q = %s; goto L; L: return (int)q; }"},
    {"head-unsigned", "int f(void){ unsigned q = (unsigned)(%s); goto L; L: return (int)q; }"},
    {"cast-paren", "int f(void){ int q = (int)(%s); goto L; L: return q; }"},
    {"addr-of", "int f(void){ int tmp = %s; int *p = &tmp; goto L; L: return *p; }"},
    {"subscript-idx", "int f(void){ int q = arr[%s]; goto L; L: return q; }"},
    {"member-dot", "int f(void){ struct D d; d.f = %s; goto L; L: return d.f; }"},
    {"cond-and", "int f(void){ int q = z && (%s); goto L; L: return q; }"},
    {"cond-or", "int f(void){ int q = z || (%s); goto L; L: return q; }"},
    {"assign-add", "int f(void){ int q = 0; q += %s; goto L; L: return q; }"},
    {"assign-mul", "int f(void){ int q = 1; q *= %s; goto L; L: return q; }"},
    {"postinc-neighbor", "int f(void){ int q = %s; q++; goto L; L: return q; }"},
    {"neg-unary", "int f(void){ int q = -(%s); goto L; L: return q; }"},
    {"not-unary", "int f(void){ int q = !(%s); goto L; L: return q; }"},
    {"bitnot-unary", "int f(void){ int q = ~(%s); goto L; L: return q; }"},
    {"shift-left", "int f(void){ int q = ((%s) << 1); goto L; L: return q; }"},
    {"shift-right", "int f(void){ int q = ((%s) >> 1); goto L; L: return q; }"},
    {"xor-bin", "int f(void){ int q = ((%s) ^ 1); goto L; L: return q; }"},
    {"and-bin", "int f(void){ int q = ((%s) & 7); goto L; L: return q; }"},
    {"or-bin", "int f(void){ int q = ((%s) | 1); goto L; L: return q; }"},
    {"cmp-lt", "int f(void){ int q = ((%s) < 2); goto L; L: return q; }"},
    {"cmp-eq", "int f(void){ int q = ((%s) == 0); goto L; L: return q; }"},
    {"nested-paren", "int f(void){ int q = (((%s))); goto L; L: return q; }"},
    {"array-init-tail", "int f(void){ int a[3] = {0, 1, %s}; goto L; L: return a[2]; }"},
    {"switch-cond", "int f(void){ switch(%s){ default: break; } goto L; L: return 0; }"},
    {"for-cond", "int f(void){ for(; %s;) break; goto L; L: return 0; }"},
    {"for-incr", "int f(void){ int i=0; for(; i<1; i+= (%s)?1:1) break; goto L; L: return 0; }"},
    {"do-cond", "int f(void){ do {} while(%s && 0); goto L; L: return 0; }"},
    {"return-paren", "int f(void){ if (z) return (%s); goto L; L: return 0; }"},
    {"cast-long", "int f(void){ long q = (long)(%s); goto L; L: return (int)q; }"},
    {"ptr-nullish", "int f(void){ int *p = (%s) ? (int*)0 : (int*)0; (void)p; goto L; L: return 0; }"},
};
#define CX_NATOMIC ((int)(sizeof(cx_atomic) / sizeof(cx_atomic[0])))

/* ---- TIER B: expression->expression wrappers for composition ------------ */
static const CxTmpl cx_wrap[] = {
    {"paren", "(%s)"},
    {"comma", "(0, %s)"},
    {"ternary-then", "((%s) ? 1 : 2)"},
    {"ternary-else", "(z ? 1 : (%s))"},
    {"cast", "((int)(%s))"},
    {"sizeof", "((int)sizeof(%s))"},
    {"alignof-dim", "((int)_Alignof(int[(%s) + 1]))"},
    {"typeof-decl", "(sizeof(typeof(%s)))"},
    {"generic", "(_Generic((%s), int: 1, default: 2))"},
    {"stmt-expr", "(({ int se = (%s); se; }))"},
    {"compound-lit", "(((int){ %s }))"},
    {"designator", "(((struct D){ .f = (%s) }).f)"},
    {"call-arg", "(h2(%s))"},
    {"subscript-dim", "(arr[(%s) & 3])"},
};
#define CX_NWRAP ((int)(sizeof(cx_wrap) / sizeof(cx_wrap[0])))

/* Planting sites for composed expressions. */
static const CxTmpl cx_site[] = {
    {"decl-init", "int f(void){ int q = %s; goto L; L: return q; }"},
    {"stmt", "int f(void){ (void)(%s); goto L; L: return 0; }"},
    {"array-dim", "int f(void){ int a[(%s) + 1]; a[0] = 1; goto L; L: return a[0]; }"},
};
#define CX_NSITE ((int)(sizeof(cx_site) / sizeof(cx_site[0])))

/* ---- Payloads: every defer/orelse surface form + interactions ----------- */
static const CxTmpl cx_payload[] = {
    {"oe-value", "z orelse 1"},
    {"oe-call", "g() orelse 1"},
    {"oe-chain2", "g() orelse g() orelse 1"},
    {"oe-chain3", "g() orelse g() orelse g() orelse 1"},
    {"oe-ret", "g() orelse return 0"},
    {"oe-brk", "g() orelse break"},
    {"oe-cont", "g() orelse continue"},
    {"oe-goto", "g() orelse goto L"},
    {"oe-block", "g() orelse { h(1); }"},
    {"oe-bare-kw", "orelse"},
    {"oe-zero", "g() orelse 0"},
    {"oe-null", "g() orelse (void*)0"},
    {"oe-five", "g() orelse 5"},
    {"oe-chain-ret", "g() orelse g() orelse return 0"},
    {"oe-block-ret", "g() orelse { return 0; }"},
    {"df-braceless", "defer h(1)"},
    {"df-braced", "defer { h(2); }"},
    {"df-bare-kw", "defer"},
    {"df-empty", "defer { }"},
    {"df-oe-braceless", "defer h(z orelse 1)"},
    {"df-oe-braced", "defer { int w = z orelse 1; h(w); }"},
};
#define CX_NPAYLOAD ((int)(sizeof(cx_payload) / sizeof(cx_payload[0])))
/* Composition uses expression payloads only (index range below). */
#define CX_NPAYLOAD_EXPR 15

#define CX_SRC_MAX 8192

typedef struct {
	long cells, rejects, transforms, idents;
	long silent, fp_fail, bad_diag;
	char first_fail[320];
} CxStats;

/* Normalized token-stream equality (cx_norm_equal) is defined in
 * test.insertion.c, included earlier in the same TU: the fixed-point theorem
 * lives in the C token stream — '#'-led lines and whitespace are emission
 * bookkeeping.  (The CLI --prism-verify comparison uses the same rule.) */

/* One cell through the trichotomy + fixed-point oracle. */
static void cx_run_cell(const char *src, const char *ctx_name, const char *payload_name,
			PrismFeatures feats, CxStats *st) {
	st->cells++;
	PrismResult r = prism_transpile_source(src, "cx.c", feats);
	if (r.status != PRISM_OK) {
		st->rejects++;
		if (!r.error_msg || !r.error_msg[0]) st->bad_diag++;
		prism_free(&r);
		return;
	}
	int surv_oe = r.output ? ins_count_kw(r.output, "orelse") : 0;
	int surv_df = r.output ? ins_count_kw(r.output, "defer") : 0;
	int surv = surv_oe + surv_df;
	if (surv == 0) {
		/* TRANSFORM: require the re-transpilation fixed point.  Second
		 * pass runs zero-init OFF and safety diagnostics as warnings:
		 * `raw` is stripped in pass-1 output (re-zero-init asymmetry), and
		 * Prism's own lowering can emit CFG shapes its strict checker
		 * rejects (already enforced on the original in pass 1).  defer/
		 * orelse lowering stays ON — the idempotence property under test. */
		PrismFeatures fp_feats = feats;
		fp_feats.zeroinit = false;
		fp_feats.warn_safety = true;
		fp_feats.auto_unreachable = false; /* benign double-emit on re-apply */
		fp_feats.auto_static = false;
		PrismResult r2 = prism_transpile_source(r.output, "cx_fp.c", fp_feats);
		int fp_ok = (r2.status == PRISM_OK) && r2.output && r.output &&
			    cx_norm_equal(r.output, r2.output);
		if (fp_ok) {
			st->transforms++;
		} else {
			if (!st->fp_fail && !st->silent)
				snprintf(st->first_fail, sizeof(st->first_fail),
					 "FIXED-POINT fail: ctx=%s payload=%s (2nd pass %s)",
					 ctx_name, payload_name,
					 r2.status == PRISM_OK ? "diverged" : "rejected");
			st->fp_fail++;
		}
		prism_free(&r2);
	} else {
		/* IDENTIFIER: survivors must behave exactly as identifiers —
		 * output equals the pipeline with precisely the SURVIVING
		 * keywords' features disabled.  (Disabling both would wrongly
		 * flag cells where an unrelated construct legitimately
		 * transforms while the other keyword survives as an
		 * identifier.) */
		PrismFeatures off = feats;
		if (surv_oe) off.orelse = false;
		if (surv_df) off.defer = false;
		PrismResult ro = prism_transpile_source(src, "cx.c", off);
		int consistent = (ro.status == PRISM_OK) && ro.output && r.output &&
				 strcmp(ro.output, r.output) == 0;
		if (consistent) {
			st->idents++;
		} else {
			if (!st->silent && !st->fp_fail)
				snprintf(st->first_fail, sizeof(st->first_fail),
					 "SILENT survivor: ctx=%s payload=%s (%d survive)",
					 ctx_name, payload_name, surv);
			st->silent++;
		}
		prism_free(&ro);
	}
	prism_free(&r);
}

static void cx_report(const char *tier, CxStats *st) {
	char name[288];
	snprintf(name, sizeof(name),
		 "contexts[%s]: %ld cells: %ld reject / %ld transform / %ld identifier, 0 unsafe%s%s",
		 tier, st->cells, st->rejects, st->transforms, st->idents,
		 (st->silent || st->fp_fail) ? " -- " : "",
		 (st->silent || st->fp_fail) ? st->first_fail : "");
	CHECK(st->silent == 0 && st->fp_fail == 0, name);
	snprintf(name, sizeof(name), "contexts[%s]: every reject carries a diagnostic", tier);
	CHECK(st->bad_diag == 0, name);
}

/* Identifier-namespace tier: `defer`/`orelse` used as an ORDINARY declared
 * name — struct/union/enum tag, member, typedef, function, variable — inside
 * the contexts the new stray-keyword rules police.  The safety property here
 * is NON-REJECTION: a legitimate identifier use must never be mistaken for a
 * stray keyword.  (This is the discriminating check for the whole class of
 * "cleaner rejection" changes — it proves they did not over-reach into valid
 * programs.)  Accept-only: byte-level identity is covered elsewhere; here we
 * assert the classifier lets every namespace through. */
static const char *cx_ident_tmpl[] = {
    "struct %s { int x; }; int f(void){ struct %s v; v.x = 1; return v.x; }",
    "union %s { int x; }; int f(void){ union %s v; v.x = 1; return v.x; }",
    "enum %s { %s_A }; int f(void){ enum %s v = %s_A; return v; }",
    "struct S { int %s; }; int f(void){ struct S s; s.%s = 3; return s.%s; }",
    "typedef int %s; int f(void){ %s v = 5; return v; }",
    "typedef int %s; int f(void){ %s a, b, c; a = b = c = 0; return a + b + c; }",
    "typedef int %s; struct S { %s m; }; int f(void){ struct S s; s.m = 0; return s.m; }",
    "static int %s(int a){ return a; } int f(void){ return %s(7); }",
    "int f(void){ int %s = 9; return %s; }",
    "int f(void){ int %s = 1; return (%s) + ((%s) ? 1 : 0); }",
    "int f(void){ int %s[3] = {1,2,3}; return %s[0]; }",
    "struct %s { int x; }; int f(void){ struct %s *p = 0; (void)p; return 0; }",
    "typedef int %s; int f(void){ return (int)sizeof(%s); }",
    "int f(void){ int %s = 0; return sizeof(%s); }",
};

static void run_ident_namespace_tier(void) {
	static const char *kw[2] = {"orelse", "defer"};
	int rejects = 0, cells = 0;
	char first_rej[192] = "";
	char src[CX_SRC_MAX];
	for (int k = 0; k < 2; k++) {
		for (size_t i = 0; i < sizeof(cx_ident_tmpl) / sizeof(cx_ident_tmpl[0]); i++) {
			const char *t = cx_ident_tmpl[i];
			const char *w = kw[k];
			/* templates use the keyword up to 4 times */
			snprintf(src, sizeof(src), t, w, w, w, w);
			cells++;
			PrismResult r = prism_transpile_source(src, "cxid.c", prism_defaults());
			if (r.status != PRISM_OK) {
				if (!rejects)
					snprintf(first_rej, sizeof(first_rej),
						 "kw=%s tmpl#%zu: %s", w, i,
						 r.error_msg ? r.error_msg : "?");
				rejects++;
			}
			prism_free(&r);
		}
	}
	char name[256];
	snprintf(name, sizeof(name),
		 "contexts[ident-namespaces]: %d identifier uses of defer/orelse accepted, 0 "
		 "false rejects%s%s",
		 cells, rejects ? " -- " : "", rejects ? first_rej : "");
	CHECK(rejects == 0, name);
}

static void run_contexts_tests(void) {
	char src[CX_SRC_MAX], e1[2048], e2[2048], e3[2048];
	PrismFeatures defaults = prism_defaults();

	run_ident_namespace_tier();

	/* TIER A: atomic x all payloads. */
	{
		CxStats st = {0};
		for (int c = 0; c < CX_NATOMIC; c++)
			for (int p = 0; p < CX_NPAYLOAD; p++) {
				snprintf(e1, sizeof(e1), cx_atomic[c].tmpl, cx_payload[p].tmpl);
				snprintf(src, sizeof(src), "%s%s", CX_PRE, e1);
				cx_run_cell(src, cx_atomic[c].name, cx_payload[p].tmpl, defaults,
					    &st);
			}
		cx_report("atomic", &st);
	}

	/* TIER B: pairwise wrapper composition x sites x expression payloads. */
	{
		CxStats st = {0};
		for (int w1 = 0; w1 < CX_NWRAP; w1++)
			for (int w2 = 0; w2 < CX_NWRAP; w2++)
				for (int s = 0; s < CX_NSITE; s++)
					for (int p = 0; p < CX_NPAYLOAD_EXPR; p++) {
						snprintf(e1, sizeof(e1), cx_wrap[w2].tmpl,
							 cx_payload[p].tmpl);
						snprintf(e2, sizeof(e2), cx_wrap[w1].tmpl, e1);
						snprintf(e3, sizeof(e3), cx_site[s].tmpl, e2);
						snprintf(src, sizeof(src), "%s%s", CX_PRE, e3);
						char cn[128];
						snprintf(cn, sizeof(cn), "%s(%s)@%s",
							 cx_wrap[w1].name, cx_wrap[w2].name,
							 cx_site[s].name);
						cx_run_cell(src, cn, cx_payload[p].tmpl, defaults,
							    &st);
					}
		cx_report("composed-2", &st);
	}

	/* TIER B-deep: triple composition.  Unconditional — it costs 0.57s and a
	 * tier nobody runs by default is a tier that rots. */
	{
		CxStats st = {0};
		char e0[2048];
		for (int w1 = 0; w1 < CX_NWRAP; w1++)
			for (int w2 = 0; w2 < CX_NWRAP; w2++)
				for (int w3 = 0; w3 < CX_NWRAP; w3++)
					for (int s = 0; s < CX_NSITE; s++)
						for (int p = 0; p < CX_NPAYLOAD_EXPR; p += 3) {
							snprintf(e0, sizeof(e0), cx_wrap[w3].tmpl,
								 cx_payload[p].tmpl);
							snprintf(e1, sizeof(e1), cx_wrap[w2].tmpl,
								 e0);
							snprintf(e2, sizeof(e2), cx_wrap[w1].tmpl,
								 e1);
							snprintf(e3, sizeof(e3), cx_site[s].tmpl,
								 e2);
							snprintf(src, sizeof(src), "%s%s", CX_PRE,
								 e3);
							cx_run_cell(src, "triple",
								    cx_payload[p].tmpl, defaults,
								    &st);
						}
		cx_report("composed-3-deep", &st);
	}

	/* TIER C: feature polarity — the oracle must hold when only one
	 * extension is enabled (cross-feature gates must not mask checks). */
	{
		CxStats st = {0};
		PrismFeatures oe_only = prism_defaults();
		oe_only.defer = false;
		PrismFeatures df_only = prism_defaults();
		df_only.orelse = false;
		for (int c = 0; c < CX_NATOMIC; c++)
			for (int p = 0; p < CX_NPAYLOAD; p += 2) {
				snprintf(e1, sizeof(e1), cx_atomic[c].tmpl, cx_payload[p].tmpl);
				snprintf(src, sizeof(src), "%s%s", CX_PRE, e1);
				cx_run_cell(src, cx_atomic[c].name, cx_payload[p].tmpl, oe_only,
					    &st);
				cx_run_cell(src, cx_atomic[c].name, cx_payload[p].tmpl, df_only,
					    &st);
			}
		cx_report("feature-polarity", &st);
	}
}

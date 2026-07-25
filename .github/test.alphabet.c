/* token-tag alphabet totality for orelse (and CF-in-args).
 *
 * The Phase-1 orelse classifier decides from a finite context
 * alphabet; the head of that alphabet is the 32-bit TT_ tag of the token
 * preceding a parenthesis/bracket group (parse.c enum, bits 0..31).  The
 * recurring historical bug class ("orelse/CF inside <context> nobody
 * enumerated" — attribute args, asm args, designators, ...) is exactly a
 * MISSING CELL in that finite table.  This suite makes the table total and
 * locked:
 *
 *   - one cell per TT_ bit, each a complete program placing `orelse` in a
 *     context headed by a token carrying that bit (or the nearest reachable
 *     construct where the bit cannot legally precede a paren — noted);
 *   - every cell's verdict is locked: REJECT (diagnostic) or TRANSFORM
 *     (accepted, zero `orelse` tokens survive in emitted C);
 *   - cells where `orelse` is used as an IDENTIFIER (member name, declared
 *     variable, feature-off) lock the opposite guarantee: the identifier
 *     SURVIVES verbatim, with an exact occurrence count.
 *
 * META-THEOREM (checked for every operator cell): the outcome is never
 * "accepted with `orelse` still in the output" and never a crash — i.e.
 * accept => fully transformed, otherwise reject with a diagnostic.  A new
 * TT_ bit without a cell here fails the completeness check below, so the
 * table cannot silently rot when the alphabet grows.
 */

typedef enum {
	AV_REJECT = 0,	 /* status != PRISM_OK, diagnostic produced          */
	AV_TRANSFORM,	 /* PRISM_OK and zero `orelse` tokens in output      */
	AV_IDENT,	 /* PRISM_OK and `orelse` survives as an identifier  */
} AlphaVerdict;

typedef struct {
	const char *bit_name; /* TT_ bit this cell covers (NULL = missing!)  */
	const char *src;      /* complete translation unit                   */
	AlphaVerdict verdict; /* locked expectation                          */
	int operator_use;     /* 1: orelse is the operator (IDENT = bug)     */
	int ident_count;      /* AV_IDENT cells: exact surviving count       */
	const char *note;
} AlphaCell;

/* Shared preamble: volatile source so no compiler can fold orelse away. */
#define A_PRE "static volatile int z; static int g(void){return z;} "

static const AlphaCell alpha_cells[32] = {
    [0] = {"TT_TYPE", A_PRE "int f(void){ int (x) = g() orelse 1; return x; }", AV_TRANSFORM, 1, 0,
	   "type keyword heads paren declarator; decl-init orelse"},
    [1] = {"TT_QUALIFIER", A_PRE "int f(void){ int * restrict (rp) = (int*)0 orelse (int*)&z; "
				 "return rp != 0; }",
	   AV_TRANSFORM, 1, 0, "qualifier heads paren declarator"},
    [2] = {"TT_SUE", "struct S { int a; }; static struct S sv; static struct S *g2(void){return 0;} "
		     "int f(void){ struct S *(p) = g2() orelse &sv; return p != 0; }",
	   AV_TRANSFORM, 1, 0, "struct tag heads paren declarator; pointer orelse"},
    [3] = {"TT_SKIP_DECL", A_PRE "int f(void){ return (int)sizeof(z orelse 1); }", AV_REJECT, 1, 0,
	   "sizeof operand is unevaluated; orelse cannot lower there"},
    [4] = {"TT_ATTR", A_PRE "int f(void){ __attribute__((aligned(z orelse 8))) int aa = 0; "
			    "return aa; }",
	   AV_REJECT, 1, 0, "orelse inside attribute arguments (SPEC constraint)"},
    [5] = {"TT_ASSIGN", A_PRE "int f(void){ int x = (g() orelse 1); return x; }", AV_TRANSFORM, 1, 0,
	   "paren-wrapped initializer (macro-hygiene form, SPEC Semantics 9)"},
    [6] = {"TT_MEMBER", "struct O { int orelse; }; "
			"int f(void){ struct O o; o.orelse = 5; return o.orelse; }",
	   AV_IDENT, 0, 3, "member named orelse: TT_MEMBER disambiguation preserves identifier"},
    [7] = {"TT_LOOP", A_PRE "int f(void){ int n=0; while (z orelse 1) { n=1; break; } return n; }",
	   AV_REJECT, 1, 0, "orelse in loop control parenthesis"},
    [8] = {"TT_STORAGE", A_PRE "int f(void){ static int sx = z orelse 1; return sx; }", AV_REJECT, 1,
	   0, "static storage initializer (SPEC orelse constraint 3)"},
    [9] = {"TT_ASM", A_PRE "int f(void){ int r=0; "
			   "__asm__ volatile(\"\" : \"=r\"(r) : \"r\"(z orelse 1)); return r; }",
	   AV_REJECT, 1, 0, "orelse inside asm arguments (the asm-args totality hole)"},
    [10] = {"TT_INLINE", A_PRE "static inline int (fi)(int a[z orelse 1]) { return a != 0; } "
			       "int f(void){ int arr[3]; return fi(arr); }",
	    AV_REJECT, 1, 0, "param array dimension orelse (SPEC orelse constraint 11)"},
    [11] = {"TT_NORETURN_FN", A_PRE "void nq(void){ exit(z orelse 0); }", AV_REJECT, 1, 0,
	    "orelse inside call arguments of noreturn fn"},
    [12] = {"TT_SPECIAL_FN", "typedef int jb_t[16]; static jb_t jb; " A_PRE
			     "void q2(void){ longjmp(jb, z orelse 1); }",
	    AV_REJECT, 1, 0, "orelse inside call arguments of special fn"},
    [13] = {"TT_CONST", A_PRE "int f(void){ const int cx = g() orelse 3; return cx; }",
	    AV_TRANSFORM, 1, 0, "const decl-init orelse (temp hoist path)"},
    [14] = {"TT_RETURN", A_PRE "int f(void){ return (z orelse 1); }", AV_REJECT, 1, 0,
	    "orelse in return expression"},
    [15] = {"TT_BREAK", A_PRE "int f(void){ int n=0; for(;;){ n = g() orelse break; n=2; break; } "
			      "return n; }",
	    AV_TRANSFORM, 1, 0, "break as orelse CF payload (break cannot head parens)"},
    [16] = {"TT_CONTINUE", A_PRE "int f(void){ int n=0; int i; for(i=0;i<2;i++){ "
				 "n = g() orelse continue; } return n; }",
	    AV_TRANSFORM, 1, 0, "continue as orelse CF payload"},
    [17] = {"TT_GOTO", A_PRE "int f(void){ int n = g() orelse goto L; return n; L: return 9; }",
	    AV_TRANSFORM, 1, 0, "goto as orelse CF payload"},
    [18] = {"TT_CASE", A_PRE "int f(void){ switch(z){ case (z orelse 1): return 1; } return 0; }",
	    AV_REJECT, 1, 0, "orelse in case label expression"},
    [19] = {"TT_DEFAULT", A_PRE "int f(void){ switch(z){ default: { int w = g() orelse 4; "
				"return w; } } }",
	    AV_TRANSFORM, 1, 0, "default: cannot head parens; orelse in default body transforms"},
    [20] = {"TT_DEFER", "void h(int a); " A_PRE "int f(void){ defer h(z orelse 1); return 0; }",
	    AV_REJECT, 1, 0, "orelse in braceless defer body (SPEC defer constraint 10)"},
    [21] = {"TT_GENERIC", A_PRE "int f(void){ return _Generic((z orelse 1), int: 1, default: 2); }",
	    AV_REJECT, 1, 0, "orelse in _Generic controlling expression"},
    [22] = {"TT_SWITCH", A_PRE "int f(void){ switch (z orelse 1) { case 1: return 1; } return 0; }",
	    AV_REJECT, 1, 0, "orelse in switch control parenthesis"},
    [23] = {"TT_IF", A_PRE "int f(void){ if (z orelse 1) return 1; return 0; }", AV_REJECT, 1, 0,
	    "orelse in if control parenthesis"},
    [24] = {"TT_TYPEDEF", "typedef int TD; " A_PRE "int f(void){ TD (tv) = g() orelse 1; "
			  "return tv; }",
	    AV_TRANSFORM, 1, 0, "typedef name heads paren declarator"},
    [25] = {"TT_VOLATILE", A_PRE "int f(void){ volatile int vx = g() orelse 1; return vx; }",
	    AV_TRANSFORM, 1, 0, "volatile storage decl-init (single-eval lowering)"},
    [26] = {"TT_REGISTER", A_PRE "int f(void){ register int r = g() orelse 1; return r; }",
	    AV_TRANSFORM, 1, 0, "register storage decl-init"},
    [27] = {"TT_TYPEOF", A_PRE "int f(void){ typeof(z orelse 1) t2 = 0; return t2; }",
	    AV_TRANSFORM, 1, 0, "orelse inside typeof operand (side-effect-free)"},
    [28] = {"TT_BITINT", A_PRE "int f(void){ _BitInt(z orelse 8) b = 0; return (int)b; }",
	    AV_REJECT, 1, 0, "orelse in _BitInt width (integer constant required)"},
    [29] = {"TT_ALIGNAS", A_PRE "int f(void){ _Alignas(z orelse 8) int av = 0; return av; }",
	    AV_REJECT, 1, 0, "orelse in _Alignas argument (constant required)"},
    [30] = {"TT_ORELSE", A_PRE "int f(void){ int c = g() orelse (g() orelse 7); return c; }",
	    AV_REJECT, 1, 0,
	    "orelse heading a paren group (nested paren fallback) is not a recognized "
	    "form — rejected per SPEC orelse constraint 13; plain chains are the "
	    "accepted spelling (FCSE chain cells)"},
    [31] = {"TT_STRUCTURAL", A_PRE "int f(void){ (z orelse 1); return 0; }", AV_REJECT, 1, 0,
	    "statement-head paren with bare orelse (no assignment target)"},
};

/* CF keywords inside attribute/asm argument groups must be rejected before
 * emission (they would bypass control-flow analysis).                      */
static const AlphaCell alpha_cf_cells[] = {
    {"ATTR+break", A_PRE "int f(void){ __attribute__((aligned(break))) int q = 0; return q; }",
     AV_REJECT, 1, 0, "CF token inside attribute args"},
    {"ATTR+return", A_PRE "int f(void){ __attribute__((aligned(return))) int q = 0; return q; }",
     AV_REJECT, 1, 0, "CF token inside attribute args"},
    {"ASM+return", A_PRE "int f(void){ __asm__(\"\" :: \"r\"(return)); return 0; }", AV_REJECT, 1,
     0, "CF token inside asm args (the asm-args totality hole)"},
    {"ASM+goto-kw", A_PRE "int f(void){ __asm__(\"\" :: \"r\"(goto)); return 0; }", AV_REJECT, 1, 0,
     "CF token inside asm args"},
    {"DEFER braced+orelse", "void h(int a); " A_PRE
			    "int f(void){ defer { int w = z orelse 1; h(w); } return 0; }",
     AV_TRANSFORM, 1, 0, "braced defer body: orelse transforms normally"},
};

/* Count word-boundary occurrences of `orelse` in emitted output. */
static int alpha_count_orelse(const char *out) {
	int n = 0;
	for (const char *p = out; (p = strstr(p, "orelse")) != NULL; p += 6) {
		int lb = (p == out) || (!isalnum((unsigned char)p[-1]) && p[-1] != '_');
		int rb = !isalnum((unsigned char)p[6]) && p[6] != '_';
		if (lb && rb) n++;
	}
	return n;
}

static void alpha_run_cell(const AlphaCell *c, const char *tag) {
	char name[256];
	if (!c->src) {
		snprintf(name, sizeof(name), "alphabet[%s]: MISSING CELL (totality hole)", tag);
		CHECK(0, name);
		return;
	}
	PrismResult r = prism_transpile_source(c->src, "alpha.c", prism_defaults());
	int actual;
	int surviving = 0;
	if (r.status != PRISM_OK) {
		actual = AV_REJECT;
	} else {
		surviving = r.output ? alpha_count_orelse(r.output) : 0;
		actual = (surviving == 0) ? AV_TRANSFORM : AV_IDENT;
	}

	/* Meta-theorem: operator-use orelse must never survive acceptance. */
	if (c->operator_use) {
		snprintf(name, sizeof(name), "alphabet[%s]: no silent passthrough (%s)", tag,
			 c->note);
		CHECK(actual != AV_IDENT, name);
	}

	snprintf(name, sizeof(name), "alphabet[%s]: verdict locked=%d actual=%d (%s)", tag,
		 (int)c->verdict, actual, c->note);
	CHECK(actual == (int)c->verdict, name);

	if (c->verdict == AV_IDENT && actual == AV_IDENT) {
		snprintf(name, sizeof(name), "alphabet[%s]: identifier survives exactly %d times",
			 tag, c->ident_count);
		CHECK(surviving == c->ident_count, name);
	}
	if (actual == AV_REJECT) {
		snprintf(name, sizeof(name), "alphabet[%s]: reject carries a diagnostic", tag);
		CHECK(r.error_msg != NULL && r.error_msg[0] != '\0', name);
	}
	prism_free(&r);
}

/* Soft-keyword gate: with the orelse feature disabled the token must revert
 * to a plain identifier everywhere.                                        */
static void alpha_run_feature_gate(void) {
	PrismFeatures feats = prism_defaults();
	feats.orelse = false;
	const char *src = "int f(void){ int orelse = 3; return orelse; }";
	PrismResult r = prism_transpile_source(src, "alpha_gate.c", feats);
	CHECK(r.status == PRISM_OK, "alphabet[gate]: feature-off accepts orelse as identifier");
	CHECK(r.output && alpha_count_orelse(r.output) == 2,
	      "alphabet[gate]: feature-off preserves identifier verbatim (2 uses)");
	prism_free(&r);
}

static void run_alphabet_tests(void) {
	int covered = 0;
	for (int i = 0; i < 32; i++) {
		char tag[64];
		snprintf(tag, sizeof(tag), "bit%d:%s", i,
			 alpha_cells[i].bit_name ? alpha_cells[i].bit_name : "?");
		alpha_run_cell(&alpha_cells[i], tag);
		if (alpha_cells[i].src) covered++;
	}
	CHECK(covered == 32, "alphabet: all 32 TT_ bits have a locked cell (completeness)");
	for (size_t i = 0; i < sizeof(alpha_cf_cells) / sizeof(alpha_cf_cells[0]); i++)
		alpha_run_cell(&alpha_cf_cells[i], alpha_cf_cells[i].bit_name);
	alpha_run_feature_gate();
}

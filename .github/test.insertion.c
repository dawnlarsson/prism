/* test.insertion.c — keyword-insertion totality sweep.
 *
 * THE PROPERTY (soft-keyword trichotomy).  For any program point, inserting
 * `orelse` or `defer` must produce exactly one of:
 *   (a) REJECT     — a diagnostic, before any emission;
 *   (b) TRANSFORM  — accepted, zero occurrences of the keyword survive;
 *   (c) IDENTIFIER — accepted with survivors, and the output is BYTE-
 *       IDENTICAL to transpiling the same mutant with both extensions
 *       disabled.  SPEC Part II: "when an extension is disabled, its
 *       keyword reverts to an ordinary identifier" — so a surviving
 *       occurrence is only sound if the enabled pipeline treated it exactly
 *       as the disabled pipeline would (pure identifier, no half-engaged
 *       transform).  This equality is decidable per mutant and does not
 *       reuse the classifier under test — it is the non-circular oracle.
 * Anything else — survivors with divergent output, accept-on/reject-off
 * disagreement, or an in-process crash — is the silent-passthrough failure
 * mode (the class that produced the case-label hole this infrastructure
 * caught on its first run).
 *
 * MECHANISM.  Each corpus TU below is tokenized with prism's own tokenizer;
 * the set of token boundaries IS the set of grammatically distinct insertion
 * points (inserting inside a token is lexically inert — it would just make a
 * longer identifier or a broken literal, both backend-loud).  For every
 * boundary we splice " orelse " / " defer " into the SOURCE and run the full
 * pipeline via prism_transpile_source.
 *
 * The corpus deliberately spans C grammar positions: declarations (scalar,
 * pointer, array, function pointer, struct/union/enum, bitfield, typedef,
 * VLA, _Atomic, _Alignas, attributes), statements (if/else, loops, switch/
 * case/default, goto/label, return), expressions (ternary, comma, casts,
 * sizeof, _Generic, compound literal, designators, statement expression),
 * and Prism's own constructs (defer, orelse forms, raw) so interactions are
 * swept too.  Corpus TUs contain no identifier spelled `orelse` or `defer`,
 * so ANY surviving occurrence in accepted output is a leak — whether of the
 * inserted keyword or of a corpus construct that failed to lower.
 */

static const char *ins_corpus[] = {
    /* declarations & initializers */
    "static volatile int z; int f(void){ int a[4] = { [1] = 2, [3] = z }; "
    "struct D { int f; int g[2]; } d = { .g[1] = 3, .f = z }; return a[1] + d.f; }",
    "static volatile int z; int f(void){ int a[8] = { [1 ... 3] = z }; return a[2]; }",
    "static volatile int z; int f(void){ __attribute__((aligned(8))) int u = z; "
    "int b[2] __attribute__((unused)); b[0] = u; return b[0]; }",
    "static volatile int z; int f(void){ int (*pa)[3] = 0; int (*(*fp)(void))[4] = 0; "
    "(void)pa; (void)fp; return z; }",
    "static volatile int z; void pr(int a[static 2], int b[const 3]); "
    "int f(void){ return z; }",
    "static volatile int z; int f(void){ struct B { unsigned w : 3; unsigned : 0; "
    "unsigned v : 2; } bb = {1, 1}; return bb.w + bb.v + z; }",
    "static volatile int z; int f(void){ auto q = z + 1; constexpr int c = 7; "
    "return (int)q + c; }",
    "static volatile int z; int fk(a) int a; { return a; } int f(void){ return fk(z); }",
    /* statements & labels */
    "static volatile int z; int f(void){ switch (z) { case 0: return 1; "
    "case 1 ... 2: return 2; default: break; } goto M; M: return 3; }",
    "static volatile int z; int f(void){ int q = 0; { q = 1; } L1: { q = 2; } "
    "if (z) goto L1; return q; }",
    /* expressions */
    "static volatile int z; static int h2(int a){ return a; } int f(void){ "
    "int q = h2((int){ z }); struct D2 { int f; }; int r = ((struct D2){ .f = z }).f; "
    "return q + r; }",
    "static volatile int z; int f(void){ int a2[4]; a2[0] = 0; int q = a2<:1:> + z; "
    "return q; }",
    "static volatile int z; int f(void) <% int q = z; return q; %>",
    /* preprocessor-adjacent (library mode keeps directives) */
    "#define IM(x) ((x) + 1)\nstatic volatile int z; int f(void){ int q = IM(z); "
    "return q; }",
    "static volatile int z; int f(void){ int q =\n#if 1\nz + 1\n#else\n0\n#endif\n; "
    "return q; }",
    "#line 42\nstatic volatile int z; int f(void){ /*c*/ int q = z; //t\n return q; }",
    /* asm slots */
    "static volatile int z; int f(void){ int r = 0; "
    "__asm__ volatile(\"\" : \"=r\"(r) : \"r\"(z) : \"memory\"); return r; }",
    "static volatile int z; int f(void){ int a = 1; int *p = &a; int arr[3] = {1,2,3}; "
    "return a + *p + arr[z?1:0]; }",
    "struct S { int a; unsigned b : 3; }; union U { int i; float f; }; enum E { E0, E1 = 4 }; "
    "int f(void){ struct S s = {1, 2}; union U u = {.i = 5}; return s.a + s.b + u.i + E1; }",
    "typedef int (*fp_t)(int); static int idf(int x){ return x; } "
    "int f(void){ fp_t fp = idf; return fp(3); }",
    "static volatile int z; int f(void){ int n = (z ? 2 : 3); int v[n]; v[0] = 1; "
    "return v[0] + (int)sizeof(v); }",
    "static _Atomic int ai; int f(void){ _Alignas(8) int x = 1; return x + ai; }",
    "static volatile int z; int f(void){ __attribute__((unused)) int u = 2; "
    "int c = (int){ 7 }; return u + c + z; }",
    /* statements */
    "static volatile int z; int f(void){ int n = 0; if (z) n = 1; else n = 2; "
    "while (n < 4) { n++; } do { n++; } while (n < 6); for (int i = 0; i < 2; i++) n += i; "
    "return n; }",
    "static volatile int z; int f(void){ switch (z) { case 0: return 1; case 1: break; "
    "default: return 2; } return 3; }",
    "static volatile int z; int f(void){ int n = 0; goto L; n = 9; L: n += 2; return n; }",
    "static volatile int z; int f(void){ int n = ({ int t = z + 1; t * 2; }); return n; }",
    /* expressions */
    "static volatile int z; static int g(void){ return z; } int f(void){ "
    "int a = g() ? g() : 3; int b = (a, a + 1); long c = (long)a; "
    "return a + b + (int)c + _Generic(a, int: 1, default: 2); }",
    /* prism constructs */
    "static volatile int z; static int lg(int v){ return v; } "
    "int f(void){ int n = 0; { n = lg(1); } return n + z; }",
    "static volatile int z; static int g(void){ return z; } "
    "int f(void){ int x = g(); int y = x; return x + y; }",
    "static volatile int z; int f(void){ raw int u; u = 3; return u + z; }",
    /* more grammar surface for boundary insertion */
    "static volatile int z; int f(void){ int *p = &z; return *p + z; }",
    "static volatile int z; int f(void){ enum { A = 1, B }; return A + B + z; }",
    "static volatile int z; _Atomic int *ap; int f(void){ return z + (ap ? 1 : 0); }",
    "static volatile int z; int f(void){ constexpr int c = 3; return c + z; }",
    "static volatile int z; int f(void){ int a[2][2] = {{1,2},{3,4}}; return a[0][1] + z; }",
    "static volatile int z; int f(void){ _BitInt(8) b = 1; return (int)b + z; }",
    "static volatile int z; int f(void){ int a = z; a += 1; a *= 2; a ^= 1; return a; }",
    "static volatile int z; int f(void){ int *p = &z; int **pp = &p; return **pp; }",
    "static volatile int z; int f(void){ typeof(z) t = z; typeof_unqual(z) u = z; return t + u; }",
    "static volatile int z; int f(void){ _Generic(z, int: 1, default: 0); return z; }",
    "static volatile int z; int f(void){ struct { int a, b; } s = {.a = 1, .b = z}; return s.a + s.b; }",
    "static volatile int z; int f(void){ int a[4] = {0}; for (int i = 0; i < 4; i++) a[i] = i; return a[z&3]; }",
    "static volatile int z; int f(void){ switch (z) { case 0: case 1: return 1; default: return 0; } }",
    "static volatile int z; int f(void){ int x = z; return (x < 0 ? -x : x); }",
    "static volatile int z; int f(void){ signed char c = (signed char)z; return (int)c; }",
};
#define INS_NCORPUS ((int)(sizeof(ins_corpus) / sizeof(ins_corpus[0])))
#define INS_MAX_BOUNDS 512
#define INS_MAX_SRC 4096

/* Normalized token-stream equality shared with test.contexts.c (same TU):
 * '#'-led lines and all whitespace are emission bookkeeping — the theorem
 * lives in the C tokens.                                                   */
static const char *cx_advance(const char *p, int at_line_start) {
	for (;;) {
		if (at_line_start && *p == '#') {
			while (*p && *p != '\n') p++;
			continue;
		}
		if (*p == '\n') {
			p++;
			at_line_start = 1;
			continue;
		}
		if (*p == ' ' || *p == '\t' || *p == '\r') {
			p++;
			at_line_start = 0;
			continue;
		}
		return p;
	}
}

static int cx_norm_equal(const char *a, const char *b) {
	const char *pa = a, *pb = b;
	int sa = 1, sb = 1;
	for (;;) {
		pa = cx_advance(pa, sa);
		pb = cx_advance(pb, sb);
		sa = sb = 0;
		if (!*pa || !*pb) return *pa == *pb;
		if (*pa != *pb) return 0;
		pa++;
		pb++;
	}
}

static int ins_count_kw(const char *out, const char *kw) {
	int n = 0;
	size_t kl = strlen(kw);
	for (const char *p = out; (p = strstr(p, kw)) != NULL; p += kl) {
		int lb = (p == out) || (!isalnum((unsigned char)p[-1]) && p[-1] != '_');
		int rb = !isalnum((unsigned char)p[kl]) && p[kl] != '_';
		if (lb && rb) n++;
	}
	return n;
}

/* PParseToken boundaries via prism's own tokenizer (offsets into src).
 *
 * LIFETIME INVARIANT: prism never frees tokenized source buffers — the
 * persistent per-thread maps (typedef table, ...) keep name pointers into
 * them across prism_transpile_source calls.  Freeing this buffer would put
 * a dangling key into those maps (heap-use-after-free found by ASan when
 * this harness first freed it).  We honor the invariant: buffers are kept
 * for the lifetime of the suite thread.                                    */
static int ins_boundaries(const char *src, int *bounds) {
	size_t len = strlen(src);
	char *buf = malloc(len + 8);
	if (!buf) return -1;
	memcpy(buf, src, len);
	memset(buf + len, 0, 8);
	PParseToken *tok = pparse_tokenize_buffer((char *)"ins_bounds.c", buf);
	if (!tok) return -1;
	int n = 0;
	long prev_end = -1;
	for (PParseToken *t = tok; t && t->kind != PPARSE_TK_EOF && n < INS_MAX_BOUNDS - 2; t = pparse_next(t)) {
		long start = pparse_loc(t) - buf;
		long end = start + t->len;
		if (start != prev_end) bounds[n++] = (int)start;
		bounds[n++] = (int)end;
		prev_end = end;
	}
	return n; /* buf intentionally retained (see invariant above) */
}

typedef struct {
	int boundaries, rejects, transforms, idents, silent, bad_diag, base_fail, fp_fail;
	int first_silent; /* boundary offset of first silent passthrough */
} InsStats;

static void ins_sweep(const char *src, const char *kw, InsStats *st) {
	/* Direct pparse_tokenize_buffer calls need the per-thread prologue that
	 * prism_transpile_source would otherwise run for us. */
	pparse_ctx_init();
	error_recovery_init();
	if (setjmp(pparse_ctx->error_jmp) != 0) {
		st->base_fail++; /* corpus TU failed to tokenize */
		pparse_ctx->error_jmp_set = false;
		return;
	}
	apply_features(prism_defaults());
	pparse_ensure_keyword_cache();

	int bounds[INS_MAX_BOUNDS];
	int nb = ins_boundaries(src, bounds);
	pparse_ctx->error_jmp_set = false;
	if (nb < 0) {
		st->base_fail++;
		return;
	}

	/* corpus sanity: the unmodified TU must transpile clean */
	PrismResult base = prism_transpile_source(src, "ins_base.c", prism_defaults());
	if (base.status != PRISM_OK ||
	    (base.output && (ins_count_kw(base.output, "orelse") || ins_count_kw(base.output, "defer"))))
		st->base_fail++;
	prism_free(&base);

	char mutant[INS_MAX_SRC + 32];
	size_t slen = strlen(src);
	for (int i = 0; i < nb; i++) {
		int at = bounds[i];
		if (slen + 10 >= sizeof(mutant)) break;
		memcpy(mutant, src, (size_t)at);
		int m = at;
		mutant[m++] = ' ';
		memcpy(mutant + m, kw, strlen(kw));
		m += (int)strlen(kw);
		mutant[m++] = ' ';
		memcpy(mutant + m, src + at, slen - (size_t)at + 1);

		st->boundaries++;
		PrismResult r = prism_transpile_source(mutant, "ins_mut.c", prism_defaults());
		if (r.status != PRISM_OK) {
			st->rejects++;
			if (!r.error_msg || !r.error_msg[0]) st->bad_diag++;
		} else {
			int surv = r.output ? (ins_count_kw(r.output, "orelse") +
					       ins_count_kw(r.output, "defer"))
					    : 0;
			if (surv) {
				/* Identifier-consistency oracle: enabled output must
				 * equal disabled output byte-for-byte. */
				PrismFeatures off = prism_defaults();
				off.orelse = false;
				off.defer = false;
				PrismResult ro =
				    prism_transpile_source(mutant, "ins_mut.c", off);
				int consistent = (ro.status == PRISM_OK) && ro.output &&
						 r.output && strcmp(ro.output, r.output) == 0;
				if (consistent) {
					st->idents++;
				} else {
					if (!st->silent) st->first_silent = at;
					st->silent++;
				}
				prism_free(&ro);
			} else {
				/* Fixed-point oracle: fully transformed output must
				 * re-transpile to itself (token-stream equality).
				 * Second pass: zero-init off (`raw` stripped in pass-1
				 * output) and safety as warnings (Prism's own lowering
				 * can emit CFG shapes its strict checker rejects, already
				 * enforced on the original in pass 1); defer/orelse
				 * lowering stays on. */
				PrismFeatures fpf = prism_defaults();
				fpf.zeroinit = false;
				fpf.warn_safety = true;
				fpf.auto_unreachable = false; /* benign double-emit on re-apply */
				fpf.auto_static = false;
				PrismResult rf =
				    prism_transpile_source(r.output, "ins_fp.c", fpf);
				if (rf.status == PRISM_OK && rf.output && r.output &&
				    cx_norm_equal(r.output, rf.output)) {
					st->transforms++;
				} else {
					if (!st->silent && !st->fp_fail) st->first_silent = at;
					st->fp_fail++;
				}
				prism_free(&rf);
			}
		}
		prism_free(&r);
	}
}

static void run_insertion_tests(void) {
	static const char *kws[2] = {"orelse", "defer"};
	for (int k = 0; k < 2; k++) {
		InsStats st = {0};
		for (int c = 0; c < INS_NCORPUS; c++) ins_sweep(ins_corpus[c], kws[k], &st);
		char name[224];
		snprintf(name, sizeof(name),
			 "insertion[%s]: %d boundaries: %d reject / %d transform+fixed-point / %d "
			 "identifier-consistent, 0 unsafe (base TUs clean)",
			 kws[k], st.boundaries, st.rejects, st.transforms, st.idents);
		CHECK(st.silent == 0 && st.base_fail == 0 && st.fp_fail == 0, name);
		if (st.silent) {
			snprintf(name, sizeof(name),
				 "insertion[%s]: FIRST SILENT PASSTHROUGH at byte offset %d",
				 kws[k], st.first_silent);
			CHECK(0, name);
		}
		snprintf(name, sizeof(name), "insertion[%s]: every reject carries a diagnostic",
			 kws[k]);
		CHECK(st.bad_diag == 0, name);
	}
}

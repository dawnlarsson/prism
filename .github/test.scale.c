/*
 * Scale and order-independence differentials.
 *
 * Every other static suite transpiles a single small TU (one function, a
 * char[512] source). The 1.1.5 performance work introduced scale-dependent
 * lookup structures with numeric thresholds that such a TU never reaches:
 *
 *   - binding timelines are built once a name's chain reaches length 8
 *     (pparse_td_max_chain_seen / pparse_ba_max_chain_seen >= 8, parse.c)
 *   - timelines are rebuilt every PPARSE_TL_REBUILD_CADENCE (256) new entries
 *   - lookups walk the "added since last build" prefix, then binary-search
 *   - a previous-cover link skips an entire dead, disjoint scope in one step
 *   - the bounds-array registry is a separate table from the typedef chain
 *
 * A resolution bug in any of those is invisible below the threshold and
 * silent above it — the same shape as the uint16 scope-cap truncation that
 * dropped the scope tree past 32,768 scopes with no diagnostic.
 *
 * Both oracles are self-checking; neither needs a hand-derived expectation.
 *
 *   A. isolation:  a chunk transpiled ALONE must emit the same function body
 *                  as the same chunk inside a TU of M chunks. Any threshold,
 *                  rebuild, eviction, or cross-scope-leak bug breaks this.
 *   B. order:      a probe TU transpiled before and after N unrelated
 *                  transpiles in the same process must emit identical output.
 *                  Targets arena reuse and persistent map state.
 */

/* Per-TU scaffolding counters (__prism_ret_0, hoist temps, …) are TU-global by
 * design, so the Nth function of a bulk TU legitimately differs from the same
 * function alone. Normalize the trailing counter; everything else must match. */
static void sc_normalize_temps(char *s) {
	char *w = s;
	for (char *p = s; *p;) {
		if (strncmp(p, "__prism_", 8) == 0) {
			char *id = p;
			while (*id && ((*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') ||
				       (*id >= '0' && *id <= '9') || *id == '_'))
				id++;
			char *e = id;
			while (e > p && e[-1] >= '0' && e[-1] <= '9') e--;
			if (e > p && e[-1] == '_' && e < id) {
				memmove(w, p, (size_t)(e - p));
				w += (e - p);
				*w++ = 'N';
			} else {
				memmove(w, p, (size_t)(id - p));
				w += (id - p);
			}
			p = id;
			continue;
		}
		*w++ = *p++;
	}
	*w = 0;
}

/* Extract "f<idx>( … )" through its matching close brace. malloc'd, or NULL. */
static char *sc_extract_fn(const char *out, int idx) {
	char pat[64];
	snprintf(pat, sizeof(pat), "f%d(", idx);
	const char *hit = NULL;
	for (const char *p = out; (p = strstr(p, pat)) != NULL; p++) {
		char a = (p == out) ? ' ' : p[-1];
		int al = (a >= 'a' && a <= 'z') || (a >= 'A' && a <= 'Z') ||
			 (a >= '0' && a <= '9') || a == '_';
		if (!al) { hit = p; break; }
	}
	if (!hit) return NULL;
	const char *brace = strchr(hit, '{');
	if (!brace) return NULL;
	int depth = 0;
	const char *q = brace;
	for (; *q; q++) {
		if (*q == '{') depth++;
		else if (*q == '}' && --depth == 0) { q++; break; }
	}
	size_t n = (size_t)(q - hit);
	char *s = malloc(n + 1);
	if (!s) return NULL;
	memcpy(s, hit, n);
	s[n] = 0;
	sc_normalize_temps(s);
	return s;
}

/* ── chunk alphabet ───────────────────────────────────────────────────
 * Each emits exactly one function f<idx> and is independent of its
 * neighbours. Names are deliberately REUSED across chunks so a single name
 * accumulates a long binding chain. */
typedef void (*ScChunk)(char *buf, size_t cap, int idx);

/* One array name `a` with differing extents in every chunk. */
static void sc_ck_samename(char *b, size_t c, int i) {
	snprintf(b, c, "int f%d(int i){ int a[%d]={0}; return a[i]; }\n", i, 4 + (i % 13));
}

/* Pointer shadow alternating with array shadow on the same name — the
 * shape behind the "param named like a file-scope array" wrap bugs. */
static void sc_ck_ptr_shadow(char *b, size_t c, int i) {
	if (i % 2)
		snprintf(b, c, "int f%d(int i){ int *a=0; return a?i:0; }\n", i);
	else
		snprintf(b, c, "int f%d(int i){ int a[%d]={0}; return a[i]; }\n", i, 4 + (i % 7));
}

/* Same typedef name redefined per function scope. */
static void sc_ck_typedef_chain(char *b, size_t c, int i) {
	snprintf(b, c, "int f%d(int i){ typedef int T[%d]; T a={0}; return a[i]; }\n", i,
		 4 + (i % 11));
}

/* defer + zero-init + orelse + bounds in one body. */
static void sc_ck_market_mix(char *b, size_t c, int i) {
	snprintf(b, c,
		 "int g%d(void); int f%d(int i){ int a[%d]={0}; struct S{int x,y;} s; "
		 "defer a[0]=1; int v = g%d() orelse %d; return a[i]+v+s.x; }\n",
		 i, i, 4 + (i % 9), i, i + 1);
}

/* typeof aliasing of a tracked array (TDF_ARRAY through the registry). */
static void sc_ck_typeof_alias(char *b, size_t c, int i) {
	snprintf(b, c, "int f%d(int i){ int a[%d]={0}; typeof(a) b; b[0]=0; return b[i]+a[i]; }\n",
		 i, 4 + (i % 5));
}

/* File-scope array alternately shadowed by a local array / local pointer. */
static void sc_ck_filescope_shadow(char *b, size_t c, int i) {
	if (i % 3 == 0)
		snprintf(b, c, "int f%d(int i){ return garr[i]; }\n", i);
	else if (i % 3 == 1)
		snprintf(b, c, "int f%d(int i){ int garr[%d]={0}; return garr[i]; }\n", i,
			 4 + (i % 6));
	else
		snprintf(b, c, "int f%d(int i){ int *garr=0; return garr?i:0; }\n", i);
}

/* Nested same-name scopes — exercises the previous-cover skip link. */
static void sc_ck_deep_shadow(char *b, size_t c, int i) {
	int d = 3 + (i % 6);
	size_t len = (size_t)snprintf(b, c, "int f%d(int i){ int r=0;", i);
	for (int k = 0; k < d; k++)
		len += (size_t)snprintf(b + len, c - len, " { int a[%d]={0}; r+=a[i];", 4 + k);
	for (int k = 0; k < d; k++)
		len += (size_t)snprintf(b + len, c - len, " }");
	snprintf(b + len, c - len, " return r; }\n");
}

/* Multidimensional arrays sharing a name — array_rank in the registry. */
static void sc_ck_multidim(char *b, size_t c, int i) {
	snprintf(b, c, "int f%d(int i,int j){ int a[%d][%d]={{0}}; return a[i][j]; }\n", i,
		 2 + (i % 5), 3 + (i % 4));
}

/* auto-static + auto-unreachable + defer together. */
static void sc_ck_as_aur(char *b, size_t c, int i) {
	snprintf(b, c,
		 "_Noreturn void die%d(void); int f%d(int i){ const char *s=\"k%d\"; "
		 "int a[%d]={0}; defer a[0]=1; if(i) die%d(); return a[i]+(int)s[0]; }\n",
		 i, i, i, 4 + (i % 7), i);
}

/* struct/union tags reusing one name across every chunk. */
static void sc_ck_tag_reuse(char *b, size_t c, int i) {
	snprintf(b, c,
		 "int f%d(int i){ struct S{int a[%d]; int x;} s; union U{int a[%d]; long z;} u; "
		 "return s.a[i]+u.a[i]+s.x; }\n",
		 i, 2 + (i % 6), 2 + (i % 3));
}

/* VLA on a shared name — VLA_VAR registry + memset path. */
static void sc_ck_vla(char *b, size_t c, int i) {
	snprintf(b, c, "int f%d(int n,int i){ if(n<1)n=1; int a[n]; a[0]=0; return a[i]+%d; }\n", i,
		 i);
}

static const struct {
	const char *name;
	ScChunk fn;
	const char *prologue; /* file-scope preamble, emitted once */
} SC_CHUNKS[] = {
	{ "samename", sc_ck_samename, NULL },
	{ "ptr-shadow", sc_ck_ptr_shadow, NULL },
	{ "typedef-chain", sc_ck_typedef_chain, NULL },
	{ "market-mix", sc_ck_market_mix, NULL },
	{ "typeof-alias", sc_ck_typeof_alias, NULL },
	{ "filescope-shadow", sc_ck_filescope_shadow, "int garr[16];\n" },
	{ "deep-shadow", sc_ck_deep_shadow, NULL },
	{ "multidim", sc_ck_multidim, NULL },
	{ "as-aur", sc_ck_as_aur, NULL },
	{ "tag-reuse", sc_ck_tag_reuse, NULL },
	{ "vla", sc_ck_vla, NULL },
};

static PrismFeatures sc_features(int bounds_on) {
	PrismFeatures f = prism_defaults();
	f.line_directives = false;
	f.flatten_headers = false;
	f.bounds_check = bounds_on != 0;
	return f;
}

/* ── A. chunk-in-isolation == chunk-in-large-TU ───────────────────────── */
static void sc_isolation(int M, int bounds_on) {
	for (size_t k = 0; k < sizeof(SC_CHUNKS) / sizeof(SC_CHUNKS[0]); k++) {
		PrismFeatures f = sc_features(bounds_on);
		size_t cap = (size_t)M * 512 + 8192;
		char *bulk = malloc(cap);
		if (!bulk) return;
		size_t len = 0;
		if (SC_CHUNKS[k].prologue)
			len += (size_t)snprintf(bulk + len, cap - len, "%s", SC_CHUNKS[k].prologue);
		for (int i = 0; i < M; i++) {
			char one[512];
			SC_CHUNKS[k].fn(one, sizeof(one), i);
			len += (size_t)snprintf(bulk + len, cap - len, "%s", one);
		}

		char label[256];
		PrismResult rb = prism_transpile_source(bulk, "sc_bulk.c", f);
		if (rb.status != PRISM_OK || !rb.output) {
			snprintf(label, sizeof(label), "scale[%s M=%d bchk=%d]: bulk TU rejected",
				 SC_CHUNKS[k].name, M, bounds_on);
			CHECK(0, label);
			prism_free(&rb);
			free(bulk);
			continue;
		}

		int bad = 0;
		char first[256] = {0};
		for (int i = 0; i < M && bad < 2; i++) {
			/* Sample the head (chain still short), the tail (past every
			 * rebuild threshold) and a coprime stride between them. */
			if (M > 40 && !(i < 10 || i >= M - 10 || i % 37 == 0)) continue;

			char one[512], solo[1024];
			SC_CHUNKS[k].fn(one, sizeof(one), i);
			snprintf(solo, sizeof(solo), "%s%s",
				 SC_CHUNKS[k].prologue ? SC_CHUNKS[k].prologue : "", one);

			PrismResult rs = prism_transpile_source(solo, "sc_solo.c", f);
			if (rs.status != PRISM_OK || !rs.output) {
				if (!bad++) snprintf(first, sizeof(first), "solo #%d rejected", i);
				prism_free(&rs);
				continue;
			}
			char *sa = sc_extract_fn(rs.output, i);
			char *sb = sc_extract_fn(rb.output, i);
			if (!sa || !sb) {
				if (!bad++)
					snprintf(first, sizeof(first),
						 "#%d not found (solo=%d bulk=%d)", i, sa != NULL,
						 sb != NULL);
			} else if (strcmp(sa, sb) != 0) {
				if (!bad++)
					snprintf(first, sizeof(first), "#%d diverges: solo<%.60s> "
								       "bulk<%.60s>",
						 i, sa, sb);
			}
			free(sa);
			free(sb);
			prism_free(&rs);
		}
		snprintf(label, sizeof(label), "scale[%s M=%d bchk=%d]: isolation%s%s",
			 SC_CHUNKS[k].name, M, bounds_on, bad ? " -- " : " stable", bad ? first : "");
		CHECK(bad == 0, label);
		prism_free(&rb);
		free(bulk);
	}
}

/* ── B. transpile-order independence ──────────────────────────────────── */
static void sc_order(int N) {
	static const char *probe =
	    "int gp(void); int probe(int i){ int a[8]={0}; typedef int T[4]; T t={0}; "
	    "defer a[0]=1; int v = gp() orelse 3; return a[i]+t[i]+v; }\n";
	PrismFeatures f = sc_features(1);
	char label[160];

	PrismResult r1 = prism_transpile_source(probe, "sc_probe.c", f);
	if (r1.status != PRISM_OK || !r1.output) {
		snprintf(label, sizeof(label), "scale[order N=%d]: probe rejected", N);
		CHECK(0, label);
		prism_free(&r1);
		return;
	}
	char *before = strdup(r1.output);
	prism_free(&r1);
	if (!before) return;

	for (int i = 0; i < N; i++) {
		char noise[512], src[1024];
		size_t nk = sizeof(SC_CHUNKS) / sizeof(SC_CHUNKS[0]);
		SC_CHUNKS[(size_t)i % nk].fn(noise, sizeof(noise), i);
		snprintf(src, sizeof(src), "int garr[16];\n%s", noise);
		PrismResult rn = prism_transpile_source(src, "sc_noise.c", f);
		prism_free(&rn);
	}

	PrismResult r2 = prism_transpile_source(probe, "sc_probe.c", f);
	if (r2.status != PRISM_OK || !r2.output) {
		snprintf(label, sizeof(label), "scale[order N=%d]: probe rejected after churn", N);
		CHECK(0, label);
		prism_free(&r2);
		free(before);
		return;
	}
	snprintf(label, sizeof(label), "scale[order N=%d]: output stable across process state", N);
	CHECK(strcmp(before, r2.output) == 0, label);
	prism_free(&r2);
	free(before);
}

void run_scale_tests(void) {
	printf("\n=== SCALE / ORDER DIFFERENTIALS ===\n");

	/* Straddle each documented threshold: chain length 8, the 256-entry
	 * rebuild cadence, and well past both. */
	static const int SCALES[] = { 4, 8, 9, 63, 255, 256, 257, 300, 600 };
	for (int bounds_on = 0; bounds_on <= 1; bounds_on++)
		for (size_t s = 0; s < sizeof(SCALES) / sizeof(SCALES[0]); s++)
			sc_isolation(SCALES[s], bounds_on);

	sc_order(5);
	sc_order(100);
}

/* test.machine.c — exhaustive verification of the REAL defer_walk state
 * machine against the declarative model in defer_model.h.
 *
 * What this proves (and how it composes with the other artifacts):
 *
 *   1. EQUIVALENCE  For every abstract scope stack up to depth DM_DEPTH
 *      (5, the saturating depth) over the full 14-symbol per-level alphabet, and every
 *      exit mode (+ every stop depth for DEFER_TO_DEPTH), the emission
 *      sequence produced by prism.c's defer_walk equals dm_expected() —
 *      the semantics written independently from SPEC.md Part II.
 *   2. PROPERTIES   The same emissions satisfy the mode-independent safety
 *      properties P1-P5 (exactly-once, LIFO, all-or-none per scope,
 *      suffix-closure, mode stop-scope) — checked directly on the output,
 *      not via the model, so a bug mirrored in both still cannot hide.
 *   3. DRY-RUN PARITY  has_defers_for(mode) == (emission nonempty) for
 *      every case — the gate Pass 2 uses to decide whether to emit.
 *   4. SHADOW DRAIN  The lazy defer-shadow error fires exactly per its
 *      min_defer_idx window and mode gate, including the documented
 *      in_defer_emit longjmp fragility (asserted, then repaired).
 *   5. COVERAGE CERTIFICATE  Every (mode x level-symbol x emitted?) pair
 *      and every TO_DEPTH stop relation is exercised; the counts are
 *      locked. Combined with defer_walk's loop body being depth-invariant
 *      (see defer_model.h header), bounded exhaustion + induction covers
 *      unbounded depth: this is the small-model argument, made checkable.
 *
 * The harness drives the real machinery in-TU: it builds Pass-2 scope_stack
 * / defer_stack states with the same scope_push_kind/defer_add calls Pass 2
 * uses, points DeferEntry bodies at real tokens from pparse_tokenize_buffer, runs
 * defer_walk, and reads back out_buf.  No mocks.
 */

#include "defer_model.h"

/* Depth 5 is the saturating depth: it adds zero new transition pairs over
 * depth 4 (see the small-model lemma in PROOFS.md), so it is the smallest
 * depth that *demonstrates* saturation rather than assuming it.  It costs
 * 2.2s, so it runs unconditionally — no env knob, no CI-only tier. */
#define DM_DEPTH_DEFAULT 5

static PParseToken *dm_marker_stmt[DM_MAX_IDS]; /* `mK` ident token   */
static PParseToken *dm_marker_end[DM_MAX_IDS];  /* the trailing `;`   */
static char *dm_marker_buf;		  /* tokenizer source (must outlive tokens) */
static FILE *dm_sink;			  /* out_fp flush safety */

static int dm_setup_markers(void) {
	char src[2048];
	int pos = 0;
	pos += snprintf(src + pos, sizeof(src) - pos, "void dm_f(void){");
	for (int i = 0; i < DM_MAX_IDS; i++)
		pos += snprintf(src + pos, sizeof(src) - pos, "m%d();", i);
	pos += snprintf(src + pos, sizeof(src) - pos, "}");

	size_t len = strlen(src);
	dm_marker_buf = malloc(len + 8);
	if (!dm_marker_buf) return 0;
	memcpy(dm_marker_buf, src, len);
	memset(dm_marker_buf + len, 0, 8);

	PParseToken *tok = pparse_tokenize_buffer((char *)"dm_markers.c", dm_marker_buf);
	if (!tok) return 0;

	int found = 0;
	for (PParseToken *t = tok; t && t->kind != PPARSE_TK_EOF; t = pparse_next(t)) {
		if (t->kind != PPARSE_TK_IDENT || t->len < 2) continue;
		char *p = pparse_loc(t);
		if (p[0] != 'm' || p[1] < '0' || p[1] > '9') continue;
		int id = atoi(p + 1);
		if (id < 0 || id >= DM_MAX_IDS) continue;
		PParseToken *e = t;
		while (e && !pparse_match_ch(e, ';')) e = pparse_next(e);
		if (!e) return 0;
		dm_marker_stmt[id] = t;
		dm_marker_end[id] = e; /* exclusive bound, same as Pass 2 braceless defer */
		found++;
	}
	return found == DM_MAX_IDS;
}

/* Per-level alphabet encoding: c in [0,14).
 * c < 9: block symbol c/3 (BLOCK, BLOCK_LOOP, BLOCK_SWITCH), ndef = c%3.
 * c >= 9: non-block symbol DM_CTRL_LOOP + (c-9), ndef = 0.                 */
#define DM_NCHOICE 14
static DmScope dm_decode(int c) {
	DmScope s;
	if (c < 9) {
		s.sym = (unsigned char)(DM_BLOCK + c / 3);
		s.ndef = (unsigned char)(c % 3);
	} else {
		s.sym = (unsigned char)(DM_CTRL_LOOP + (c - 9));
		s.ndef = 0;
	}
	return s;
}

static ScopeKind dm_real_kind(int sym) {
	switch (sym) {
	case DM_BLOCK:
	case DM_BLOCK_LOOP:
	case DM_BLOCK_SWITCH: return SCOPE_BLOCK;
	case DM_CTRL_LOOP:
	case DM_CTRL_SWITCH:
	case DM_CTRL_PLAIN: return SCOPE_CTRL_PAREN;
	case DM_FOR_LOOP: return SCOPE_FOR_PAREN;
	default: return SCOPE_GENERIC; /* DM_TRANSPARENT */
	}
}

static DeferEmitMode dm_real_mode(int mode) {
	switch (mode) {
	case DM_M_SCOPE: return DEFER_SCOPE;
	case DM_M_ALL: return DEFER_ALL;
	case DM_M_BREAK: return DEFER_BREAK;
	case DM_M_CONTINUE: return DEFER_CONTINUE;
	default: return DEFER_TO_DEPTH;
	}
}

/* Build the real Pass-2 stacks for an abstract stack, using the same entry
 * points Pass 2 uses.  `kind_override` (or -1) substitutes the real kind of
 * DM_TRANSPARENT levels so the GENERIC/TERNARY/INIT collapse can be checked. */
static void dm_build_real(const DmScope *st, int n, int kind_override) {
	int id = 0;
	for (int i = 0; i < n; i++) {
		ScopeKind k = dm_real_kind(st[i].sym);
		if (st[i].sym == DM_TRANSPARENT && kind_override >= 0) k = (ScopeKind)kind_override;
		scope_push_kind(k);
		ScopeNode *s = &scope_stack[pparse_ctx->scope_depth - 1];
		s->is_loop = (st[i].sym == DM_BLOCK_LOOP || st[i].sym == DM_CTRL_LOOP ||
			      st[i].sym == DM_FOR_LOOP);
		s->is_switch = (st[i].sym == DM_BLOCK_SWITCH || st[i].sym == DM_CTRL_SWITCH);
		for (int j = 0; j < st[i].ndef; j++, id++)
			defer_add(dm_marker_stmt[id], dm_marker_stmt[id], dm_marker_end[id]);
	}
}

static void dm_teardown_real(void) {
	pparse_ctx->scope_depth = 0;
	pparse_ctx->block_depth = 0;
	defer_count = 0;
	out_buf_pos = 0;
}

/* Parse marker ids out of out_buf in emission order. */
static int dm_parse_emissions(int *ids) {
	int n = 0;
	for (int i = 0; i + 1 < out_buf_pos && n < DM_MAX_IDS; i++) {
		if (out_buf[i] != 'm') continue;
		if (out_buf[i + 1] < '0' || out_buf[i + 1] > '9') continue;
		if (i > 0 && ((out_buf[i - 1] >= 'a' && out_buf[i - 1] <= 'z') ||
			      (out_buf[i - 1] >= '0' && out_buf[i - 1] <= '9')))
			continue;
		ids[n++] = atoi(out_buf + i + 1);
		while (i + 1 < out_buf_pos && out_buf[i + 1] >= '0' && out_buf[i + 1] <= '9') i++;
	}
	return n;
}

/* One drive of the real machine.  Returns emissions via ids/&n; returns the
 * dry-run answer.                                                          */
static bool dm_drive(const DmScope *st, int nsc, int mode, int stop_depth, int kind_override,
		     int *ids, int *n) {
	dm_build_real(st, nsc, kind_override);
	bool dry = defer_walk(dm_real_mode(mode), stop_depth, true);
	out_buf_pos = 0;
	defer_walk(dm_real_mode(mode), stop_depth, false);
	*n = dm_parse_emissions(ids);
	dm_teardown_real();
	return dry;
}

static void dm_format_stack(const DmScope *st, int n, char *buf, size_t cap) {
	static const char *sn[] = {"B", "Bl", "Bs", "Cl", "Cs", "Cp", "Fl", "T"};
	int pos = 0;
	buf[0] = '\0';
	for (int i = 0; i < n && pos + 8 < (int)cap; i++)
		pos += snprintf(buf + pos, cap - pos, "%s%d ", sn[st[i].sym], st[i].ndef);
}

/* Coverage certificate: (mode x level-choice x scope-fully-emitted?) plus
 * TO_DEPTH stop relations.  Locked below for the default depth.            */
static unsigned char dm_cov[DM_NMODE][DM_NCHOICE][2];
static unsigned char dm_cov_rel[3]; /* stop<blocks, stop==blocks, only-check */

static void dm_record_cov(const DmScope *st, int nsc, int mode, int stop_depth, const int *ids,
			  int n, const int *choices) {
	for (int i = 0; i < nsc; i++) {
		int emitted = 0;
		if (dm_is_block(st[i].sym) && st[i].ndef > 0) {
			int base = dm_base(st, i), cnt = 0;
			for (int k = 0; k < n; k++)
				if (ids[k] >= base && ids[k] < base + st[i].ndef) cnt++;
			emitted = (cnt == st[i].ndef);
		}
		dm_cov[mode][choices[i]][emitted] = 1;
	}
	if (mode == DM_M_TO_DEPTH) {
		int tb = dm_total_blocks(st, nsc);
		dm_cov_rel[stop_depth < tb ? 0 : (stop_depth == tb ? 1 : 2)] = 1;
	}
}

typedef struct {
	long drives, mismatches, prop_fails, dry_fails;
	char first_fail[256];
} DmStats;

static void dm_check_one(const DmScope *st, int nsc, int mode, int stop_depth, const int *choices,
			 DmStats *stats) {
	int exp[DM_MAX_IDS], got[DM_MAX_IDS], got_n;
	int exp_n = dm_expected(st, nsc, mode, stop_depth, exp);
	bool dry = dm_drive(st, nsc, mode, stop_depth, -1, got, &got_n);
	stats->drives++;

	int ok = (got_n == exp_n);
	for (int i = 0; ok && i < got_n; i++) ok = (got[i] == exp[i]);
	if (!ok) {
		if (stats->mismatches == 0) {
			char sbuf[128];
			dm_format_stack(st, nsc, sbuf, sizeof(sbuf));
			snprintf(stats->first_fail, sizeof(stats->first_fail),
				 "stack[%s] mode=%d stop=%d: expected %d ids, got %d", sbuf, mode,
				 stop_depth, exp_n, got_n);
		}
		stats->mismatches++;
	}
	int total = dm_base(st, nsc);
	if (!dm_prop_exactly_once(got, got_n, total) || !dm_prop_lifo(got, got_n) ||
	    !dm_prop_scope_all_or_none(st, nsc, got, got_n) ||
	    !dm_prop_suffix_closed(st, nsc, got, got_n) ||
	    !dm_prop_mode_stop(st, nsc, mode, stop_depth, got, got_n))
		stats->prop_fails++;
	if (dry != (exp_n > 0)) stats->dry_fails++;
	dm_record_cov(st, nsc, mode, stop_depth, got, got_n, choices);
}

static void dm_run_exhaustive(int max_depth) {
	DmStats stats = {0};
	DmScope st[DM_MAX_DEPTH];
	int choices[DM_MAX_DEPTH];

	for (int depth = 0; depth <= max_depth; depth++) {
		long count = 1;
		for (int i = 0; i < depth; i++) count *= DM_NCHOICE;
		for (long enc = 0; enc < count; enc++) {
			long e = enc;
			for (int i = 0; i < depth; i++) {
				choices[i] = (int)(e % DM_NCHOICE);
				st[i] = dm_decode(choices[i]);
				e /= DM_NCHOICE;
			}
			if (depth > 0 && dm_is_block(st[depth - 1].sym))
				dm_check_one(st, depth, DM_M_SCOPE, 0, choices, &stats);
			dm_check_one(st, depth, DM_M_ALL, 0, choices, &stats);
			dm_check_one(st, depth, DM_M_BREAK, 0, choices, &stats);
			dm_check_one(st, depth, DM_M_CONTINUE, 0, choices, &stats);
			/* sd == tb+1 is unreachable from Pass 2 (a goto target cannot
			 * be deeper than the current stack) — driven anyway to lock
			 * the defensive no-op. */
			int tb = dm_total_blocks(st, depth);
			for (int sd = 0; sd <= tb + 1; sd++)
				dm_check_one(st, depth, DM_M_TO_DEPTH, sd, choices, &stats);
		}
	}

	char name[192];
	snprintf(name, sizeof(name),
		 "machine: exhaustive depth<=%d: %ld drives, %ld mismatches (%s)", max_depth,
		 stats.drives, stats.mismatches, stats.mismatches ? stats.first_fail : "model == code");
	CHECK(stats.mismatches == 0, name);
	snprintf(name, sizeof(name), "machine: properties P1-P5 hold on all %ld emissions",
		 stats.drives);
	CHECK(stats.prop_fails == 0, name);
	snprintf(name, sizeof(name), "machine: dry-run parity on all %ld drives", stats.drives);
	CHECK(stats.dry_fails == 0, name);

	/* Coverage certificate.  Locked for the default depth: every reachable
	 * (mode x level-symbol x emitted?) pair occurs, every TO_DEPTH stop
	 * relation occurs.  The exact reachable-pair count is locked so a new
	 * mode/symbol/depth-dependent branch cannot appear silently.          */
	int cov = 0;
	for (int m = 0; m < DM_NMODE; m++)
		for (int c = 0; c < DM_NCHOICE; c++)
			for (int e = 0; e < 2; e++) cov += dm_cov[m][c][e];
	int rel = dm_cov_rel[0] + dm_cov_rel[1] + dm_cov_rel[2];
	snprintf(name, sizeof(name), "machine: transition coverage locked (%d pairs, %d rels)", cov,
		 rel);
	CHECK(cov == 94 && rel == 3, name);
}

/* GENERIC / TERNARY / INIT must be indistinguishable to the walk (the model
 * collapses them into DM_TRANSPARENT — validate that collapse on real code). */
static void dm_run_transparent_collapse(void) {
	DmScope st[2];
	st[0].sym = DM_BLOCK;
	st[0].ndef = 2;
	st[1].sym = DM_TRANSPARENT;
	st[1].ndef = 0;
	ScopeKind kinds[3] = {SCOPE_GENERIC, SCOPE_TERNARY, SCOPE_INIT};
	int ref[DM_MAX_IDS], ref_n = -1, ok = 1;
	for (int m = 0; m < DM_NMODE; m++) {
		if (m == DM_M_SCOPE) continue; /* top not a block */
		for (int k = 0; k < 3; k++) {
			int ids[DM_MAX_IDS], n;
			dm_drive(st, 2, m, 0, kinds[k], ids, &n);
			if (k == 0) {
				ref_n = n;
				memcpy(ref, ids, sizeof(int) * (size_t)n);
			} else {
				if (n != ref_n) ok = 0;
				for (int i = 0; ok && i < n; i++)
					if (ids[i] != ref[i]) ok = 0;
			}
		}
	}
	CHECK(ok, "machine: GENERIC/TERNARY/INIT collapse identically (transparent lemma)");
}

/* Shadow lazy-drain semantics (defer_walk lines ~1320): the queued shadow
 * error fires iff mode != DEFER_SCOPE and the shadow's defer_idx lies in
 * [min_defer_idx, defer_count) of the walk that just emitted.              */
static void dm_run_shadow_tests(void) {
	static char shadow_name[] = "dm_shadow_var";

	/* S1: shadow in window, CF mode -> error fires (longjmp).  Verified
	 * finding: the shadow error path is longjmp-clean — defer_walk restores
	 * in_defer_emit (line ~1319) BEFORE the shadow check (line ~1320), so
	 * the guard is already false when pparse_error_tok jumps.  The repair in
	 * reset_transpiler_state (line ~510) covers the OTHER error source:
	 * PRISM_DEBUG shells firing inside emit_deferred_range mid-walk.      */
	{
		DmScope st[1] = {{DM_BLOCK, 1}};
		dm_build_real(st, 1, -1);
		defer_shadow_count = 1;
		pparse_VEC_ENSURE_REALLOC(defer_shadows, 1, defer_shadow_cap, 8);
		defer_shadows[0] = (DeferShadow){shadow_name, (int)sizeof(shadow_name) - 1, 1,
						 dm_marker_stmt[0], 0};
		int caught = 0;
		error_recovery_init();
		if (setjmp(pparse_ctx->error_jmp) != 0) {
			caught = 1;
		} else {
			out_buf_pos = 0;
			defer_walk(DEFER_ALL, 0, false);
		}
		CHECK(caught == 1, "machine: shadow in window fires on CF exit");
		CHECK(in_defer_emit == false,
		      "machine: shadow error longjmp is in_defer_emit-clean (restore precedes check)");
		in_defer_emit = false; /* belt-and-braces for the next drive */
		defer_shadow_count = 0;
		dm_teardown_real();
		error_recovery_init();
	}

	/* S2: same state, DEFER_SCOPE -> gate excludes scope exits, no error. */
	{
		DmScope st[1] = {{DM_BLOCK, 1}};
		dm_build_real(st, 1, -1);
		defer_shadow_count = 1;
		defer_shadows[0] = (DeferShadow){shadow_name, (int)sizeof(shadow_name) - 1, 1,
						 dm_marker_stmt[0], 0};
		int caught = 0;
		error_recovery_init();
		if (setjmp(pparse_ctx->error_jmp) != 0) {
			caught = 1;
		} else {
			out_buf_pos = 0;
			defer_walk(DEFER_SCOPE, 0, false);
		}
		CHECK(caught == 0, "machine: shadow does NOT fire on plain scope exit");
		defer_shadow_count = 0;
		dm_teardown_real();
		error_recovery_init();
	}

	/* S3: shadow outside the min_defer_idx window -> no error.  Stack:
	 * outer block owns defer id0, inner loop block owns none; BREAK stops
	 * at the inner loop block, so min_defer_idx == 1 and id0 is outside.  */
	{
		DmScope st[2] = {{DM_BLOCK, 1}, {DM_BLOCK_LOOP, 0}};
		dm_build_real(st, 2, -1);
		defer_shadow_count = 1;
		defer_shadows[0] = (DeferShadow){shadow_name, (int)sizeof(shadow_name) - 1, 1,
						 dm_marker_stmt[0], 0};
		int caught = 0;
		error_recovery_init();
		if (setjmp(pparse_ctx->error_jmp) != 0) {
			caught = 1;
		} else {
			out_buf_pos = 0;
			defer_walk(DEFER_BREAK, 0, false);
		}
		CHECK(caught == 0, "machine: shadow outside emitted window stays quiet");
		defer_shadow_count = 0;
		dm_teardown_real();
		error_recovery_init();
	}

	/* S4: dry runs never fire shadow errors (they emit nothing). */
	{
		DmScope st[1] = {{DM_BLOCK, 1}};
		dm_build_real(st, 1, -1);
		defer_shadow_count = 1;
		defer_shadows[0] = (DeferShadow){shadow_name, (int)sizeof(shadow_name) - 1, 1,
						 dm_marker_stmt[0], 0};
		int caught = 0;
		error_recovery_init();
		if (setjmp(pparse_ctx->error_jmp) != 0) {
			caught = 1;
		} else {
			bool has = defer_walk(DEFER_ALL, 0, true);
			CHECK(has == true, "machine: dry-run sees pending defers");
		}
		CHECK(caught == 0, "machine: dry-run never fires shadow errors");
		defer_shadow_count = 0;
		dm_teardown_real();
		error_recovery_init();
	}
}

static void run_machine_tests(void) {
	pparse_ctx_init();
	error_recovery_init();
	if (setjmp(pparse_ctx->error_jmp) != 0) {
		CHECK(0, "machine: unexpected pparse_error_tok during setup");
		return;
	}
	apply_features(prism_defaults());
	pparse_ensure_keyword_cache();

	if (!dm_setup_markers()) {
		CHECK(0, "machine: marker tokenization");
		return;
	}
	reset_transpiler_state();
	dm_sink = fopen("/dev/null", "w");
	FILE *saved_fp = out_fp;
	if (dm_sink) out_fp = dm_sink;

	dm_run_exhaustive(DM_DEPTH_DEFAULT);
	dm_run_transparent_collapse();
	dm_run_shadow_tests();

	out_fp = saved_fp;
	if (dm_sink) fclose(dm_sink);
	pparse_ctx->error_jmp_set = false;
}

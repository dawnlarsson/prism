/* declarative model of Prism defer unwinding semantics.
 *
 *   - .github/test.machine.c   drives the REAL `defer_walk` in prism.c over
 *     every abstract scope stack up to a bounded depth and asserts emission
 *     order equals dm_expected() plus the independent properties below.
 *   - .github/cbmc_defer.c     model-checks dm_expected() symbolically
 *     (nondeterministic stacks/modes) against the same properties with CBMC.
 *
 * Everything here is written FROM SPEC.md Part II ("The defer Statement",
 * Semantics 2-7) not from prism.c. Where the two disagree, the suite fails
 * and the divergence itself is the finding.
 *
 * Abstraction argument (why bounded exhaustion is a proof, not a sample):
 * defer_walk's loop body is invariant in the depth index. per iteration it
 * reads only the current scope descriptor (kind, is_loop, is_switch,
 * defer_start_idx) and carried state (current_defer, curr_bd, min_defer_idx).
 * Hence its behavior is a function of the SEQUENCE of abstract descriptors
 * drawn from the finite alphabet below, and every distinct
 * (mode x descriptor x carried-state-class) transition already occurs in some
 * stack of depth <= 3. Verifying all stacks of depth <= DM_DEPTH (default 4)
 * therefore covers every transition of the machine plus at least one extra
 * composition step; correctness for unbounded depth follows by induction on
 * the stack (each iteration's postcondition is the next one's precondition).
 * test.machine.c additionally records and locks the transition-coverage count
 * so a future code change that adds a depth-dependent branch breaks the gate.
 *
 * Pure C, no dependencies, fixed bounds, no allocation, CBMC-friendly.
 */
#ifndef PRISM_DEFER_MODEL_H
#define PRISM_DEFER_MODEL_H

/* ---- Abstract scope-descriptor alphabet ---------------------------------
 * One symbol per Pass-2 scope_stack state distinguishable by defer_walk.
 * SCOPE_INIT / SCOPE_GENERIC / SCOPE_TERNARY are all "transparent" to the
 * walk (not SCOPE_BLOCK, never a break/continue stopper) and collapse into
 * DM_TRANSPARENT; test.machine.c drives DM_TRANSPARENT as SCOPE_GENERIC and
 * relies on this collapse, which is itself checked by a dedicated case that
 * drives all three kinds and asserts identical emissions. */
enum {
	DM_BLOCK = 0,	 /* SCOPE_BLOCK, plain compound statement            */
	DM_BLOCK_LOOP,	 /* SCOPE_BLOCK, loop body   (is_loop set)           */
	DM_BLOCK_SWITCH, /* SCOPE_BLOCK, switch body (is_switch set)         */
	DM_CTRL_LOOP,	 /* SCOPE_CTRL_PAREN of while(...)   (is_loop)       */
	DM_CTRL_SWITCH,	 /* SCOPE_CTRL_PAREN of switch(...)  (is_switch)     */
	DM_CTRL_PLAIN,	 /* SCOPE_CTRL_PAREN of if(...)                      */
	DM_FOR_LOOP,	 /* SCOPE_FOR_PAREN of for(...)      (is_loop)       */
	DM_TRANSPARENT,	 /* SCOPE_GENERIC / SCOPE_TERNARY / SCOPE_INIT       */
	DM_NSYM
};

/* Exit kinds, mirroring DeferEmitMode 1:1 (order differs; mapped in the
 * harness).  Semantics per SPEC Part II defer Semantics 2-4 + 7:           */
enum {
	DM_M_SCOPE = 0, /* normal flow reaches the innermost `}`             */
	DM_M_ALL,	/* return: every registered defer                    */
	DM_M_BREAK,	/* break: up to+incl innermost loop-or-switch block  */
	DM_M_CONTINUE,	/* continue: up to+incl innermost loop block         */
	DM_M_TO_DEPTH,	/* goto / labeled break/continue: exit all blocks    */
			/* strictly deeper than block-depth `stop_depth`     */
	DM_NMODE
};

#define DM_MAX_DEPTH 8	/* max scopes in an abstract stack                  */
#define DM_MAX_NDEF 3	/* max defers registered per block scope            */
#define DM_MAX_IDS (DM_MAX_DEPTH * DM_MAX_NDEF)

typedef struct {
	unsigned char sym;  /* DM_* symbol                                   */
	unsigned char ndef; /* defers registered in this scope (blocks only) */
} DmScope;

static inline int dm_is_block(int sym) {
	return sym == DM_BLOCK || sym == DM_BLOCK_LOOP || sym == DM_BLOCK_SWITCH;
}
/* A break unwinds until the innermost loop-or-switch BLOCK (SPEC: "a break
 * statement leaving a loop or switch").  A loop/switch control paren met
 * first also terminates the unwind: a break inside a GNU statement
 * expression in `while (cond)` exits only scopes inside the condition —
 * scopes outside the paren are not exited at that point.                   */
static inline int dm_break_stops_at_paren(int sym) {
	return sym == DM_CTRL_LOOP || sym == DM_CTRL_SWITCH || sym == DM_FOR_LOOP;
}
static inline int dm_break_stops_at_block(int sym) {
	return sym == DM_BLOCK_LOOP || sym == DM_BLOCK_SWITCH;
}
static inline int dm_continue_stops_at_paren(int sym) {
	return sym == DM_CTRL_LOOP || sym == DM_FOR_LOOP;
}
static inline int dm_continue_stops_at_block(int sym) {
	return sym == DM_BLOCK_LOOP;
}

/* Total number of BLOCK scopes in the stack == Pass 2's ctx->block_depth.  */
static inline int dm_total_blocks(const DmScope *st, int n) {
	int b = 0;
	for (int i = 0; i < n; i++)
		if (dm_is_block(st[i].sym)) b++;
	return b;
}

/* Global defer ids in registration order: scope i's defers occupy
 * [dm_base(st,i), dm_base(st,i)+st[i].ndef), outermost scope registered
 * first — exactly the order Pass 2 pushes onto defer_stack.               */
static inline int dm_base(const DmScope *st, int i) {
	int b = 0;
	for (int j = 0; j < i; j++) b += st[j].ndef;
	return b;
}

/* dm_expected — the declarative semantics.
 *
 * Emits into out[] the global defer ids that SPEC requires to run for the
 * given exit, in the required order:  scopes innermost-to-outermost (SPEC
 * defer Semantics 3), LIFO within each scope (Semantics 2), stopping per
 * exit kind (Semantics 2-4).  Returns the count.
 *
 * Precondition for DM_M_SCOPE: the innermost scope is a block (Pass 2 only
 * performs scope-exit emission at a closing `}`).
 * DM_M_BREAK/DM_M_CONTINUE with no stopper in the stack unwind every block;
 * that state is unreachable from valid C (break outside a loop) — the
 * harness still drives it and locks the defensive behavior.                */
static int dm_expected(const DmScope *st, int n, int mode, int stop_depth, int *out) {
	int out_n = 0;
	int curr_bd = dm_total_blocks(st, n);
	for (int i = n - 1; i >= 0; i--) {
		int sym = st[i].sym;
		if (!dm_is_block(sym)) {
			if (mode == DM_M_BREAK && dm_break_stops_at_paren(sym)) break;
			if (mode == DM_M_CONTINUE && dm_continue_stops_at_paren(sym)) break;
			continue; /* transparent to the unwind */
		}
		if (mode == DM_M_TO_DEPTH && curr_bd <= stop_depth) break;
		/* Emit this block's defers, LIFO (descending global id). */
		int base = dm_base(st, i);
		for (int k = st[i].ndef - 1; k >= 0; k--) out[out_n++] = base + k;
		curr_bd--;
		if (mode == DM_M_SCOPE) break;
		if (mode == DM_M_BREAK && dm_break_stops_at_block(sym)) break;
		if (mode == DM_M_CONTINUE && dm_continue_stops_at_block(sym)) break;
	}
	return out_n;
}

/* ---- Independent properties ---------------------------------------------
 * Checked on an emission sequence WITHOUT reference to dm_expected's
 * derivation, so a bug replicated identically in both dm_expected and the
 * implementation would still have to satisfy these to escape.              */

/* P1: exactly-once — no defer id appears twice, and no id outside the
 * registered range appears at all.                                         */
static int dm_prop_exactly_once(const int *ids, int n, int total_registered) {
	unsigned char seen[DM_MAX_IDS] = {0};
	for (int i = 0; i < n; i++) {
		if (ids[i] < 0 || ids[i] >= total_registered) return 0;
		if (seen[ids[i]]) return 0;
		seen[ids[i]] = 1;
	}
	return 1;
}

/* P2: global LIFO — ids strictly decrease.  Because ids are assigned in
 * registration order and scopes unwind innermost-first, SPEC's "LIFO within
 * each scope, innermost to outermost" is equivalent to a strictly
 * decreasing global sequence.                                              */
static int dm_prop_lifo(const int *ids, int n) {
	for (int i = 1; i < n; i++)
		if (ids[i] >= ids[i - 1]) return 0;
	return 1;
}

/* P3: per-scope contiguity — for every scope, either all of its defers ran
 * or none did (an exit never runs half a scope's cleanup).                 */
static int dm_prop_scope_all_or_none(const DmScope *st, int nsc, const int *ids, int n) {
	for (int i = 0; i < nsc; i++) {
		int base = dm_base(st, i), cnt = 0;
		for (int k = 0; k < n; k++)
			if (ids[k] >= base && ids[k] < base + st[i].ndef) cnt++;
		if (cnt != 0 && cnt != st[i].ndef) return 0;
	}
	return 1;
}

/* P4: downward closure — the set of unwound scopes is a contiguous suffix
 * of the block stack: if a block's defers ran, every block NESTED INSIDE it
 * also ran its defers.  (Control cannot leave an outer scope while staying
 * in an inner one.)                                                        */
static int dm_prop_suffix_closed(const DmScope *st, int nsc, const int *ids, int n) {
	int deepest_unemitted = -1; /* innermost block index with ndef>0 not emitted */
	for (int i = nsc - 1; i >= 0; i--) {
		if (!dm_is_block(st[i].sym) || st[i].ndef == 0) continue;
		int base = dm_base(st, i), ran = 0;
		for (int k = 0; k < n; k++)
			if (ids[k] >= base && ids[k] < base + st[i].ndef) ran = 1;
		if (!ran && deepest_unemitted < 0) deepest_unemitted = i;
		if (ran && deepest_unemitted >= 0 && i < deepest_unemitted) return 0;
	}
	return 1;
}

/* P5: mode-specific stop-scope correctness, stated as a reachability rule
 * rather than by re-running the walk:
 *   DM_M_ALL       — every registered defer ran.
 *   DM_M_SCOPE     — exactly the innermost block's defers ran.
 *   DM_M_BREAK     — no defer outside (shallower than) the innermost
 *                    break-stopper ran; defers inside it all ran.
 *   DM_M_CONTINUE  — same with the continue-stopper.
 *   DM_M_TO_DEPTH  — a block's defers ran iff its block-depth (1-based
 *                    count from outermost) exceeds stop_depth, up to
 *                    paren-stopper truncation (none for goto: Pass 2 only
 *                    emits DEFER_TO_DEPTH at statement level).             */
static int dm_prop_mode_stop(const DmScope *st, int nsc, int mode, int stop_depth,
			     const int *ids, int n) {
	int total = dm_base(st, nsc);
	if (mode == DM_M_ALL) {
		int cnt = 0;
		for (int k = 0; k < n; k++) cnt++;
		return cnt == total;
	}
	if (mode == DM_M_SCOPE) {
		for (int i = nsc - 1; i >= 0; i--) {
			if (!dm_is_block(st[i].sym)) continue;
			int base = dm_base(st, i);
			if (n != st[i].ndef) return 0;
			for (int k = 0; k < n; k++)
				if (ids[k] < base || ids[k] >= base + st[i].ndef) return 0;
			return 1;
		}
		return n == 0;
	}
	if (mode == DM_M_BREAK || mode == DM_M_CONTINUE) {
		/* Find the stopper scanning inward-out; defers registered at or
		 * outside it must not run. */
		for (int i = nsc - 1; i >= 0; i--) {
			int sym = st[i].sym;
			int stop_paren = (mode == DM_M_BREAK) ? dm_break_stops_at_paren(sym)
							      : dm_continue_stops_at_paren(sym);
			int stop_block = (mode == DM_M_BREAK) ? dm_break_stops_at_block(sym)
							      : dm_continue_stops_at_block(sym);
			if (stop_paren) { /* nothing at or below index i runs */
				for (int k = 0; k < n; k++)
					if (ids[k] < dm_base(st, i + 1)) return 0;
				return 1;
			}
			if (dm_is_block(sym) && stop_block) {
				/* everything from this block inward runs, nothing outside */
				int base = dm_base(st, i);
				int inward = dm_base(st, nsc) - base;
				if (n != inward) return 0;
				for (int k = 0; k < n; k++)
					if (ids[k] < base) return 0;
				return 1;
			}
		}
		return n == total; /* no stopper: defensive full unwind */
	}
	if (mode == DM_M_TO_DEPTH) {
		int bd = 0;
		for (int i = 0; i < nsc; i++) {
			if (!dm_is_block(st[i].sym)) continue;
			bd++; /* this block sits at block-depth bd */
			int base = dm_base(st, i), ran = 0, cnt = 0;
			for (int k = 0; k < n; k++)
				if (ids[k] >= base && ids[k] < base + st[i].ndef) cnt++;
			ran = (cnt == st[i].ndef && st[i].ndef > 0);
			int should = bd > stop_depth;
			if (st[i].ndef > 0 && should != ran) return 0;
			if (!should && cnt != 0) return 0;
		}
		return 1;
	}
	return 0;
}

#endif /* PRISM_DEFER_MODEL_H */

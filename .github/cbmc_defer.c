/* cbmc_defer.c — symbolic model-check of the declarative defer semantics.
 *
 * Proof chain (three legs, each mechanical):
 *   1. THIS FILE: CBMC proves the model (defer_model.h dm_expected) satisfies
 *      properties P1-P5 for ALL stacks up to DM_MAX_DEPTH with SYMBOLIC
 *      ndef counts (0..DM_MAX_NDEF) and symbolic stop depths — no sampling.
 *   2. test.machine.c proves the REAL prism.c defer_walk is emission-
 *      equivalent to the model on the full bounded alphabet (exhaustive).
 *   3. defer_model.h's header states the depth-uniformity induction that
 *      lifts both to unbounded depth (validated empirically: transition
 *      coverage saturates at depth 3; depth 4 and 5 add zero new pairs).
 *
 * Run under CBMC (CI):
 *   cbmc --unwind 40 --unwinding-assertions .github/cbmc_defer.c
 *
 * Without CBMC this file still compiles as a plain exhaustive checker over
 * the same space (bounded ndef <= 2 like the suite, depth <= 5):
 *   cc -O2 -o /tmp/cbmc_defer_cc .github/cbmc_defer.c && /tmp/cbmc_defer_cc
 */
#include "defer_model.h"

#ifdef __CPROVER__

unsigned char nondet_uchar(void);
unsigned int nondet_uint(void);

int main(void) {
	DmScope st[DM_MAX_DEPTH];
	unsigned int n = nondet_uint();
	__CPROVER_assume(n <= DM_MAX_DEPTH);

	for (unsigned int i = 0; i < n; i++) {
		st[i].sym = nondet_uchar();
		st[i].ndef = nondet_uchar();
		__CPROVER_assume(st[i].sym < DM_NSYM);
		__CPROVER_assume(st[i].ndef <= DM_MAX_NDEF);
		__CPROVER_assume(dm_is_block(st[i].sym) || st[i].ndef == 0);
	}

	unsigned int mode = nondet_uint();
	__CPROVER_assume(mode < DM_NMODE);
	/* DM_M_SCOPE precondition: Pass 2 emits scope exits only at `}`. */
	__CPROVER_assume(mode != DM_M_SCOPE || (n > 0 && dm_is_block(st[n - 1].sym)));

	unsigned int stop = nondet_uint();
	__CPROVER_assume(stop <= (unsigned int)dm_total_blocks(st, (int)n) + 1u);

	int out[DM_MAX_IDS];
	int out_n = dm_expected(st, (int)n, (int)mode, (int)stop, out);
	int total = dm_base(st, (int)n);

	__CPROVER_assert(out_n <= total, "emission count bounded by registrations");
	__CPROVER_assert(dm_prop_exactly_once(out, out_n, total), "P1 exactly-once");
	__CPROVER_assert(dm_prop_lifo(out, out_n), "P2 LIFO");
	__CPROVER_assert(dm_prop_scope_all_or_none(st, (int)n, out, out_n), "P3 all-or-none");
	__CPROVER_assert(dm_prop_suffix_closed(st, (int)n, out, out_n), "P4 suffix-closed");
	__CPROVER_assert(dm_prop_mode_stop(st, (int)n, (int)mode, (int)stop, out, out_n),
			 "P5 mode stop-scope");
	return 0;
}

#else /* plain-cc fallback: exhaustive over the suite alphabet */

#include <stdio.h>
#include <stdlib.h>

#define FB_NCHOICE 14
static DmScope fb_decode(int c) {
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

int main(void) {
	long checked = 0, failed = 0;
	DmScope st[DM_MAX_DEPTH];
	for (int depth = 0; depth <= 5; depth++) {
		long count = 1;
		for (int i = 0; i < depth; i++) count *= FB_NCHOICE;
		for (long enc = 0; enc < count; enc++) {
			long e = enc;
			for (int i = 0; i < depth; i++) {
				st[i] = fb_decode((int)(e % FB_NCHOICE));
				e /= FB_NCHOICE;
			}
			int tb = dm_total_blocks(st, depth);
			for (int mode = 0; mode < DM_NMODE; mode++) {
				if (mode == DM_M_SCOPE &&
				    !(depth > 0 && dm_is_block(st[depth - 1].sym)))
					continue;
				int max_stop = (mode == DM_M_TO_DEPTH) ? tb + 1 : 0;
				for (int stop = 0; stop <= max_stop; stop++) {
					int out[DM_MAX_IDS];
					int out_n = dm_expected(st, depth, mode, stop, out);
					int total = dm_base(st, depth);
					checked++;
					if (!dm_prop_exactly_once(out, out_n, total) ||
					    !dm_prop_lifo(out, out_n) ||
					    !dm_prop_scope_all_or_none(st, depth, out, out_n) ||
					    !dm_prop_suffix_closed(st, depth, out, out_n) ||
					    !dm_prop_mode_stop(st, depth, mode, stop, out, out_n))
						failed++;
				}
			}
		}
	}
	printf("cbmc_defer fallback: %ld cases, %ld property failures\n", checked, failed);
	return failed ? 1 : 0;
}

#endif

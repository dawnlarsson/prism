# Prism Verification Architecture: defer / orelse never fail, never misfire

**Companion:** [`SPEC.md`](SPEC.md) (Part II is the semantic contract), [`CERTIFICATION.md`](CERTIFICATION.md).

This document states the correctness claims as theorems, names the artifact that
discharges each one, and gives the arguments that lift bounded, mechanical checks
to unbounded programs. Every artifact runs in CI; none of this is aspirational.

---

## The theorems

**T1: Parse totality.** For every input, prism terminates and either emits
output or raises a diagnostic. Never a crash, never a hang, never a silent drop.

**T2: Classifier totality.** Every `defer`/`orelse` token, in every syntactic
position C's grammar can put it, is either (a) lowered by a recognized emission
template, (b) rejected with a diagnostic, or (c) treated as an ordinary
identifier *exactly* as the feature-disabled pipeline would treat it.
"Accepted, with the keyword surviving in operator position" is excluded.

**T3: Semantic preservation.** For every accepted program: each `defer` body
runs exactly once per dynamic exit of its scope, LIFO within a scope,
innermost-scope-first, stopping at the SPEC-mandated boundary for each exit
kind; each `orelse` fires iff the tested value is falsy, with single
evaluation guarantees per SPEC Part II.

---

## The artifacts and what each one proves

### 1. `defer_model.h`: the declarative model
A ~200-line, dependency-free statement of SPEC Part II's defer semantics
(`dm_expected`) plus five *independent* safety properties checked directly on
emission sequences, not via the model: P1 exactly-once, P2 global LIFO,
P3 per-scope all-or-none, P4 suffix-closure (unwound scopes form a contiguous
inner suffix), P5 mode stop-scope correctness. Written from the SPEC, not from
prism.c. Divergence between the two is a finding, whichever side is wrong.

### 2. `test.machine.c`: exhaustive machine equivalence (suite: `machine`)
Drives the **real** `defer_walk` in-TU (same `scope_push_kind` / `defer_add`
entry points Pass 2 uses, real tokens, real output buffer) over **every**
abstract scope stack up to depth 4 (the full 14-symbol per-level alphabet of
scope kinds × loop/switch flags × defer counts) under every exit mode and
every `DEFER_TO_DEPTH` stop depth (including the unreachable-defensive
`stop == blocks+1`). 337,787 drives per run assert:
- emission order == `dm_expected` (model ≡ code),
- P1–P5 hold on the raw emissions,
- dry-run parity (`has_defers_for` == emission nonempty), the gate Pass 2
  uses before emitting,
- the shadow lazy-drain window and its mode gate (including the verified
  finding that the shadow error longjmp is `in_defer_emit`-clean because the
  restore at defer_walk's exit precedes the shadow check),
- GENERIC/TERNARY/INIT scopes are walk-indistinguishable (the transparent
  collapse the model relies on).

**The small-model lemma.** `defer_walk`'s loop body is invariant in the depth
index: each iteration reads only the current scope descriptor plus three
carried integers. Its behavior is therefore a finite-state transduction over
the descriptor alphabet, and every distinct (mode × descriptor ×
carried-state-class) transition occurs by depth 3. The transition-coverage
certificate (locked at 94 pairs / 3 stop relations) is *saturated*: depth 5
(5,101,371 drives, run in CI) adds **zero** new pairs. Bounded exhaustion at
depth ≥ 4 therefore covers every transition of the machine, and correctness
for unbounded depth follows by induction on the stack: each iteration's
postcondition is the next one's precondition. A future depth-dependent branch
in `defer_walk` breaks the locked count.

### 3. `cbmc_defer.c`: symbolic model check (CI job: `formal`)
CBMC proves P1–P5 over the model for **nondeterministic** stacks up to depth 8
with *symbolic* defer counts and stop depths, no enumeration gaps. The same
file compiles standalone as an exhaustive checker (5,101,371 cases) on hosts
without CBMC. Proof chain: CBMC certifies the model; `test.machine.c`
certifies model ≡ real code on the full bounded alphabet; the small-model
lemma lifts both.

### 4. `test.alphabet.c`: tag-alphabet totality for T2 (suite: `alphabet`)
The Phase-1 classifier decides from a finite context alphabet whose head is
the 32-bit `TT_` tag of the token preceding a paren/bracket group. The
historical bug class, "orelse/CF inside *context nobody enumerated*"
(attribute args, asm args, designators, …), is exactly a missing cell in
that finite table. This suite locks **one verdict cell per TT_ bit** (all 32,
completeness-checked so a new bit cannot appear without a cell), plus CF-in-
attr/asm cells, the member-access and typedef identifier-preservation cells,
and the feature-off soft-keyword gate. The meta-theorem checked per operator
cell: *accepted ⇒ zero surviving `orelse` tokens*.

> **It works:** on its first run this suite found a live totality hole:
> `case (z orelse 1):` was accepted and emitted verbatim (the backend saw the
> raw `orelse` token). Fixed in the prescan case-label scan, which now
> inspects paren-group interiors between `case` and `:`; the cell locks the
> reject. CF keywords in case parens are backend-loud by construction (C
> keywords cannot parse as expressions), so passthrough there is safe.

### 5. `test.insertion.c`: every-boundary insertion sweep for T2 (suite: `insertion`)
For a corpus of ~40 TUs spanning C grammar positions (declarations,
designators incl. GNU ranges, bitfields, attributes, K&R defs, digraphs,
preprocessor directives, asm slots, statement expressions, …), `orelse` and
`defer` are spliced at **every token boundary** (boundaries enumerated by
prism's own tokenizer, ~4,200 mutants per run) and the full pipeline runs on
each mutant. Locked trichotomy per mutant:
- REJECT with a diagnostic, or
- TRANSFORM with zero surviving keywords **and a re-transpilation fixed
  point** (transpiling the output again yields identical tokens), or
- IDENTIFIER: output **byte-identical** to the same mutant transpiled with
  both extensions disabled, SPEC's "when disabled, the keyword reverts to an
  ordinary identifier", used as a non-circular oracle (it never consults the
  classifier under test).
Anything else (divergent survivors, a non-idempotent transform, accept/reject
disagreement between the two pipelines, or an in-process crash) fails the
suite.

### 5b. `test.contexts.c`: combinatorial context-closure sweep for T2/T3 (suite: `contexts`)
The historical bug class was not just unenumerated atomic positions but
unenumerated **composed** contexts ("attr between dims", "_Atomic-typeof
dims", "designator orelse in a statement expression"). Hand-picked lists
cannot close that. This suite generates contexts three ways and sweeps every
defer/orelse payload form through all of them:
- **atomic** (~70 grammar-position templates with a hole: every keyword head
  class, declarator dims with static/qualifiers, designators incl. GNU
  ranges, digraphs, preprocessor directives, asm operand slots, the
  case/label family, attributes in every position, C23 auto/constexpr/bitint,
  bitfields, _Generic, K&R defs, …) × every payload;
- **composed-2**: 14 expression→expression wrappers composed **pairwise**
  (196 shapes) planted in 3 sites; `PRISM_CONTEXTS_DEEP=1` adds
  **composed-3** triple composition (run in CI, ~33k cells);
- **feature-polarity**: every atomic cell re-swept under orelse-only and
  defer-only feature sets (SPEC's cross-feature invariant: disabling one
  extension must never suppress the other's checking).
Oracle per cell: the same REJECT / TRANSFORM+fixed-point / IDENTIFIER
trichotomy as the insertion suite. ~45k transpiles per full run.

> **It works:** this pair found and drove fixes for a dozen live defects:
> case-label orelse passthrough; `orelse`-position bounds-check omission in
> copied ranges; non-idempotent bounds re-wrapping and VLA re-`memset`;
> file-scope / parameter-dimension / `_Static_assert` / declarator-interior
> stray-`defer` token-mangling; empty bracket-orelse fallback; decl-init
> `orelse` spanning `#if`; and a **silent miscompile** where a declaration as
> a *braceless* defer body duplicated every following statement into the
> cleanup paste (the pre-existing accept-test only checked a substring and
> missed it, now a hard reject with the braced form required).

### 6. `--prism-verify` / `PRISM_VERIFY=1`: translation validation (T2, per compile)
After emitting, prism re-runs its **entire pipeline** on the emitted C. The
certificate has two arms, both sound and both demonstrated to fire:
- **re-transpile succeeds**: output #1 must be valid C that re-survives every
  Phase-1 constraint and the CFG verifier. A leaked keyword that pass 2's
  Phase-1 *rejects* (e.g. `case (x orelse 1)`) fails here (hard error).
- **keyword count is invariant**: prism only ever *removes* operator-position
  `defer`/`orelse` (it lowers them) and never introduces one, so the whole-word
  count can only drop on re-transpile. A strict decrease means pass 2 lowered a
  keyword pass 1 *left behind*, an operator leak. Equal counts prove every
  surviving `orelse`/`defer` word is a stable ordinary identifier.

Deliberately **not** byte-equality: `prism(prism(x))` is not byte-equal to
`prism(x)` under header-flattening (pass 2 re-wraps the pragma preamble,
renumbers `#line`, relocates the one-time `__prism_bchk` helper). That
scaffolding is unrelated to defer/orelse; byte-level lowering-idempotence is
certified instead by the in-process oracles (§5/§5b) on controlled inputs. The
second pass disables the additive transforms (zero-init, auto-unreachable,
auto-static) (each re-applies with a benign asymmetry) and downgrades safety
diagnostics to warnings (Prism's own lowering can emit CFG shapes its strict
checker rejects; the original already passed in pass 1). CI runs the full
14,000+-test suite under `PRISM_VERIFY=1`, and prism verifies its **own**
13k-line self-host transpilation.

### 6b. Identifier-namespace non-regression (suite: `contexts`, tier `ident-namespaces`)
The "cleaner rejection" fixes (stray-`defer`/`orelse` in declarator, argument,
dimension, `sizeof`/`_Static_assert` contexts) must never mistake a legitimate
identifier for a stray keyword. This tier uses `defer`/`orelse` as a declared
name in every namespace (struct/union/enum tag, member, typedef, function,
variable) inside those policed contexts and asserts **non-rejection** (28
uses, 0 false rejects). It is the discriminating check that the rejection
tightening did not over-reach into valid programs.

### 7. Termination watchdogs: T1 (PRISM_DEBUG builds)
The Phase-1 prescan and Pass-2 walks carry debug-only progress watchdogs
(budget `256·tokens + 65536` outer iterations). A cursor-stall bug becomes a
diagnostic instead of a hang. Release builds compile them out. Crash-freedom
remains fuzz-covered (csmith differential + AFL++ with ASan/UBSan,
`fuzz.sh`); the insertion and alphabet suites additionally run the pipeline
in-process, where any crash fails the suite. The machine and insertion
harnesses run ASan-clean.

### 8. `gen_torture.py` exhaustive tier: end-to-end T3 (suite: `torture`)
Beyond the seeded random "storm" units, the generator now emits a **complete
product**: wrapper {none, block, for, while, do, switch} × outer defers
{0,1,2} × inner defers {0,1,2} × exit {end, return, break, continue,
orelse-decl, orelse-return, orelse-break, orelse-continue}, 342 units, each
emitted twice from one AST (Prism source + an independent plain-C reference
lowering with its own scope walker) and executed with LIFO/trace/return/
eval-parity assertions. This carries the machine-level exhaustion through the
full concrete pipeline: parse → classify → emit → compile → run.

### 9. `test.runtime.c`: executed-semantics blind spots (suite: `runtime`)
Targets the runtime-semantic gaps a coverage audit found thin or absent, with
robust VALUE and EVALUATION-COUNT oracles (deliberately not trace-order, whose
hand-computed expectations are error-prone): bare-assignment and decl-init
primary-evaluated-exactly-once; chain left-to-right short-circuit with exact
eval counts; bracket-dimension runtime size + primary eval; **volatile /
`_Atomic` orelse value + primary-once** (previously zero executed coverage:
the single-eval lowering `if(!x){x=fb;}` reads the qualified object once);
`orelse` inside a **fall-through switch case** (untested elsewhere); orelse
control-flow payload firing pending defers on the way out; and return-value
captured before defer mutation. 23 executed assertions.

**Differential fuzzing (offline bug-hunt, not a committed suite).** Two
harnesses back this suite's design: a *structural* stress generator (deep
random defer/orelse/control-flow nesting, checked for emitted-UB under
ASan+UBSan, emitted-invalid-C, and re-transpilation fixed point, 800 seeds,
0 anomalies) and an *orelse value-differential* generator that emits each
program twice, once with `orelse` and once lowered to the provably-correct
`({int _t=(A); _t?_t:(B);})` stmt-expr form, then compares runtime value and
eval count (800 seeds, 0 divergences). Both are reproducible from
`.github/` scratch scripts; the durable coverage they validated lives in this
suite and in `gen_torture.py`.

---

## How the pieces compose

```
        SPEC Part II  ──written into──►  defer_model.h (dm_expected, P1–P5)
                                              │
              CBMC (symbolic, nondet) ────────┤ certifies model
                                              │
   test.machine.c (exhaustive, real code) ────┤ certifies model ≡ defer_walk
                                              │
        small-model lemma + induction ────────┘ lifts to unbounded depth
                                              │
   torture exhaustive tier ─── end-to-end product through parse/emit/run
   alphabet + insertion  ───── classifier totality over finite context alphabet
   --prism-verify ──────────── per-compile fixed-point certificate (all passes)
   watchdogs + fuzz ────────── termination and crash-freedom backstops
```

## Trust base (what is *not* proven here)

The backend C compiler; the system preprocessor; cross-TU escapes
(`longjmp` through opaque function pointers, SPEC UB items 3); struct
padding semantics (SPEC Known Limitations 1); and the documented UB items in
SPEC Part II. `p1_verify_cfg` refuses (hard error) the constructs it cannot
verify (computed goto, asm goto) rather than guessing.

## Running everything locally

```sh
cc -O2 -o /tmp/prism prism.c && python3 .github/gen_torture.py > .github/test.torture.c
/tmp/prism run .github/test.c                          # full suite (13k+ tests)
PRISM_VERIFY=1 /tmp/prism run .github/test.c           # + translation validation
PRISM_MACHINE_DEPTH=5 PRISM_SUITE_ONLY=machine /tmp/prism run .github/test.c   # deep machine
cc -O2 -o /tmp/cbmc_cc .github/cbmc_defer.c && /tmp/cbmc_cc                    # model, exhaustive
cbmc --unwind 40 --unwinding-assertions .github/cbmc_defer.c                   # model, symbolic
cc -DPRISM_DEBUG -o /tmp/prism_dbg prism.c && /tmp/prism_dbg run .github/test.c  # debug gate
```

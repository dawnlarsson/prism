# Prism Transpiler Specification

**Version:** 0.120.0
**Status:** Implemented — every item in this document corresponds to behavior that exists in the codebase and is exercised by the test suite (3075 tests + self-host stage1==stage2).

This document describes what the transpiler **does**, not what it aspires to do.

---

## 1. Overview

Prism is a source-to-source C transpiler. It reads preprocessed C, transforms it, and emits standard C. It adds three language features — `defer`, `orelse`, and automatic zero-initialization — and enforces safety rules at transpile time. It is a single compilation unit (`prism.c` includes `parse.c`; `windows.c` is a native Windows shim).

The transpiler operates in two passes:

1. **Pass 1** — Full semantic analysis over all tokens at all depths. Builds immutable data structures (scope tree, symbol table, shadow table, per-function CFG arrays). Raises all errors. No output is emitted.
2. **Pass 2** — Code generation. Reads Pass 1's immutable artifacts. Emits transformed C. No type table mutations, no safety checks, no speculative token walking. If Pass 2 starts, it runs to completion.

**Key invariant:** Every semantic error is raised before Pass 2 emits its first byte.

---

## 2. Tokenizer (Pass 0)

Defined in `parse.c`. Produces a flat pool of `Token` structs.

### Token structure (20 bytes, hot path)

| Field | Type | Description |
|---|---|---|
| `tag` | `uint32_t` | Bitmask of `TT_*` keyword/semantic tags |
| `next_idx` | `uint32_t` | Pool index of next token (0 = NULL) |
| `match_idx` | `uint32_t` | Pool index of matching delimiter (0 = none) |
| `len` | `uint32_t` | Byte length of the token text |
| `kind` | `uint8_t` | `TK_IDENT`, `TK_KEYWORD`, `TK_PUNCT`, `TK_STR`, `TK_NUM`, `TK_PREP_DIR`, `TK_EOF` |
| `flags` | `uint8_t` | `TF_AT_BOL`, `TF_HAS_SPACE`, `TF_IS_FLOAT`, `TF_OPEN`, `TF_CLOSE`, `TF_C23_ATTR`, `TF_RAW`, `TF_SIZEOF` |

Cold path data (`TokenCold`, separate array): `loc_offset`, `line_no` (21-bit), `file_idx` (11-bit).

### Delimiter matching

Every `(`, `[`, `{` gets a `match_idx` pointing to its closing pair. Every `)`, `]`, `}` points back. This is computed during tokenization and is used pervasively in both passes.

### Keyword tagging

Identifiers and keywords are tagged with `TT_*` bitmask flags during tokenization. 32 tag bits are defined:

`TT_TYPE`, `TT_QUALIFIER`, `TT_SUE` (struct/union/enum), `TT_SKIP_DECL`, `TT_ATTR`, `TT_ASSIGN`, `TT_MEMBER`, `TT_LOOP`, `TT_STORAGE`, `TT_ASM`, `TT_INLINE`, `TT_NORETURN_FN`, `TT_SPECIAL_FN`, `TT_CONST`, `TT_RETURN`, `TT_BREAK`, `TT_CONTINUE`, `TT_GOTO`, `TT_CASE`, `TT_DEFAULT`, `TT_DEFER`, `TT_GENERIC`, `TT_SWITCH`, `TT_IF`, `TT_TYPEDEF`, `TT_VOLATILE`, `TT_REGISTER`, `TT_TYPEOF`, `TT_BITINT`, `TT_ALIGNAS`, `TT_ORELSE`, `TT_STRUCTURAL`.

### Taint graph

The tokenizer builds per-function taint flags (`has_setjmp`, `has_vfork`, `has_asm`) stored on the opening `{` token's tag bits. These are later transferred to `FuncMeta`.

---

## 3. Pass 1 — Semantic Analysis

Pass 1 is a series of phases executed sequentially. Each phase augments the data structures built by prior phases.

### 3.0 Infrastructure

#### pass1_ann (parallel annotation array)

A `uint8_t` array parallel to the token pool. Indexed by token pool index via the `tok_ann(tok)` macro. Allocated alongside the token pool (realloc'd in `token_pool_ensure`). Freed by `tokenizer_teardown(true)`.

Flag definitions:

| Flag | Bit | Meaning |
|---|---|---|
| `P1_NEEDS_BRACE_WRAP` | 0 | Braceless control body needs `{` `}` injection |
| `P1_SCOPE_LOOP` | 1 | This `{` opens a loop body |
| `P1_SCOPE_SWITCH` | 2 | This `{` opens a switch body |
| `P1_SCOPE_CONDITIONAL` | 3 | This `{` opens a conditional body |
| `P1_OE_BRACKET` | 4 | `orelse` inside array dimension brackets `[…]` |
| `P1_OE_DECL_INIT` | 5 | `orelse` inside a declaration initializer |

**Cache discipline:** Pass 2 only reads `pass1_ann` when a token's tag matches `TT_STRUCTURAL`, `TT_IF|TT_LOOP|TT_SWITCH`, or `TT_ORELSE`. The fast path (~70–80% of tokens) never touches the array.

#### Scope tree

Flat array indexed by `scope_id`. One entry per `{` in the translation unit.

```
ScopeInfo {
    parent_id      : uint16_t    — enclosing scope (0 = file scope)
    open_tok_idx   : uint32_t    — token index of '{'
    close_tok_idx  : uint32_t    — token index of matching '}'
    is_struct      : bool :1     — struct/union/enum body
    is_loop        : bool :1
    is_switch      : bool :1
    is_func_body   : bool :1     — depth-1 '{' of a function definition
    is_stmt_expr   : bool :1     — GNU statement expression ({...})
    is_conditional : bool :1
}
```

Ancestor check (`scope_is_ancestor_or_self`): O(depth) walk up `parent_id` chain, typically < 10 hops.

#### Per-function metadata

```
FuncMeta {
    body_open              : Token*
    ret_type_start         : Token*
    ret_type_end           : Token*
    ret_type_suffix_start  : Token*
    ret_type_suffix_end    : Token*
    returns_void           : bool
    has_setjmp             : bool
    has_vfork              : bool
    has_asm                : bool
    entry_start            : int     — start index into p1_entries[]
    entry_count            : int     — count of P1FuncEntry items
}
```

Lookup during Pass 2: linear scan by `body_open` pointer (typically < 100 functions per TU).

#### Per-function entry array

```
P1FuncEntry {
    kind         : enum { P1K_LABEL, P1K_GOTO, P1K_DEFER, P1K_DECL, P1K_SWITCH, P1K_CASE }
    scope_id     : uint16_t
    token_index  : uint32_t
    tok          : Token*
    union {
        label  { name, len }                     — for P1K_LABEL, P1K_GOTO
        decl   { has_init, is_vla, has_raw }     — for P1K_DECL
        kase   { switch_scope_id }               — for P1K_CASE
    }
}
```

All functions share one global `p1_entries[]` array. Each `FuncMeta` indexes into it via `entry_start` / `entry_count`.

#### Shadow table

```
P1ShadowEntry {
    name        : char*
    len         : int
    scope_id    : uint16_t
    token_index : uint32_t
    prev_index  : int         — chain to previous shadow for same name (-1 = none)
}
```

Stored in a flat array with a hashmap keyed on name. Supports multiple shadows per name (chained via `prev_index`).

#### Allocation policy

| Array | Allocation | Freed by |
|---|---|---|
| `pass1_ann[]` | `realloc` | `tokenizer_teardown(true)` |
| `scope_tree[]` | `ARENA_ENSURE_CAP` | `arena_reset()` |
| `func_meta[]` | `ARENA_ENSURE_CAP` | `arena_reset()` |
| `p1_entries[]` | `ARENA_ENSURE_CAP` | `arena_reset()` |
| `p1_shadow_entries[]` | `ARENA_ENSURE_CAP` | `arena_reset()` |
| `p1_shadow_map.buckets` | `malloc` | `prism_thread_cleanup()` |

All arena-allocated structures are automatically reclaimed by `prism_reset()` → `arena_reset()` on any `error_tok()` that `longjmp`s out.

---

### 3.1 Phase 1A — Scope Tree Construction

**Function:** `p1_build_scope_tree`

Walks all tokens. On every `{`, assigns a new `scope_id`, records `parent_id` from a local stack, and classifies the scope by examining the preceding token pattern:

| Pattern | Classification |
|---|---|
| `do {` | `is_loop` |
| `) {` where `(` preceded by `for`/`while` | `is_loop` |
| `) {` where `(` preceded by `switch` | `is_switch` |
| `) {` where `(` preceded by `if` | `is_conditional` |
| `else {` | `is_conditional` |
| `({` | `is_stmt_expr` |
| `struct`/`union`/`enum` body | `is_struct` |
| File-scope `{` preceded by `)` (function def) | `is_func_body` |

Writes `P1_SCOPE_LOOP`, `P1_SCOPE_SWITCH`, `P1_SCOPE_CONDITIONAL` flags to `pass1_ann` on the `{` token.

---

### 3.2 Phase 1B — Full-Depth Type Registration

**Executed inside:** `p1_full_depth_prescan`

Walks all tokens at all depths. Registers every `typedef`, `enum` constant, and VLA tag into the typedef table using `typedef_add_entry`.

Each `TypedefEntry` records:

- `name`, `len` — identifier text
- `scope_depth` — numeric depth
- `scope_open_idx`, `scope_close_idx` — token index range of the enclosing scope (set from `td_scope_open` / `td_scope_close` thread-locals, which are updated as Phase 1 enters/exits scopes)
- `token_index` — pool index of the declaration
- `is_vla`, `is_void`, `is_const`, `is_ptr`, `is_array`, `is_aggregate` — type property flags
- `is_shadow`, `is_enum_const`, `is_vla_var` — entry kind flags
- `prev_index` — chain to previous entry for the same name

**After Phase 1 completes, the typedef table is immutable.** No `typedef_add_entry` calls occur in Pass 2.

#### Typedef lookup (range-based scoping)

`typedef_lookup(tok)` walks the chain for a name and returns the first entry where:

```
entry.token_index <= tok_idx(tok) AND
tok_idx(tok) >= entry.scope_open_idx AND
tok_idx(tok) < entry.scope_close_idx
```

This range-based check eliminates runtime scope unwinding. There is no `active_shadow_idx` cache — the range check is sufficient.

#### typedef_flags query

A single lookup returns a bitmask: `TDF_TYPEDEF`, `TDF_VLA`, `TDF_VOID`, `TDF_ENUM_CONST`, `TDF_CONST`, `TDF_PTR`, `TDF_ARRAY`, `TDF_AGGREGATE`. Convenience macros (`is_known_typedef`, `is_vla_typedef`, etc.) test individual bits.

---

### 3.3 Phase 1C — Shadow Registration

**Executed inside:** `p1_full_depth_prescan`

For every variable declaration at every depth, if the declared name collides with a typedef, a `P1ShadowEntry` is recorded via `p1_add_shadow`. Shadows also create a `TDK_SHADOW` entry in the typedef table itself (with `is_shadow = true`, `scope_open_idx`, `scope_close_idx`), so that `typedef_lookup` returns the shadow entry when the token is within the shadow's scope range.

**Temporal ordering:** Shadows are token-order-dependent. A variable named `T` declared at token index 500 only shadows the typedef `T` for lookups at index ≥ 500 within the shadow's scope range.

**Function parameter scope:** When Phase 1 encounters a function body `{` (is_func_body), parameter declarations from the preceding `(…)` are registered as shadows scoped to the function body. For forward declarations (prototypes ending with `;`), parameter shadows are registered scoped to the prototype's parameter list range.

---

### 3.4 Phase 1D — Per-Function CFG Collection

**Executed inside:** `p1_full_depth_prescan`

For each function body, collects `P1FuncEntry` items into the global `p1_entries[]` array:

| Kind | What is recorded |
|---|---|
| `P1K_LABEL` | Label name, scope_id, token_index |
| `P1K_GOTO` | Target label name, scope_id, token_index |
| `P1K_DEFER` | scope_id, token_index |
| `P1K_DECL` | scope_id, token_index, `has_init`, `is_vla`, `has_raw` |
| `P1K_SWITCH` | scope_id, token_index |
| `P1K_CASE` | scope_id, token_index, `switch_scope_id` |

**Detection sites:** Labels and gotos are detected both at statement-start positions and inside braceless control flow (e.g., `if (c) goto L;`).

**VLA tracking:** `is_vla` is set for Variable Length Arrays. Jumping past a VLA is always dangerous regardless of `has_init`, because it bypasses implicit stack allocation.

**has_raw:** Declarations marked `raw` set `has_raw = true`. The CFG verifier skips these for forward goto checks (skipping an uninitialized `raw` variable is safe).

**FuncMeta linkage:** Each `FuncMeta` records `entry_start` and `entry_count`, indexing into `p1_entries[]`.

---

### 3.5 Phase 1E — Function Return Type Capture

**Executed inside:** `p1_full_depth_prescan`

At each file-scope function body `{`, captures the return type token range and flags into `FuncMeta`:

- `ret_type_start` / `ret_type_end` — token range for the return type
- `ret_type_suffix_start` / `ret_type_suffix_end` — for complex declarators (function pointers, arrays)
- `returns_void` — whether the function returns `void`
- `has_setjmp`, `has_vfork`, `has_asm` — transferred from the taint flags on the `{` token's tag

Pass 2 reads `FuncMeta` at function body entry.

---

### 3.6 Phase 1F — Defer Validation

**Executed inside:** `p1_full_depth_prescan`

Calls `validate_defer_statement` for every `defer` keyword encountered. This function is pure — it only calls `error_tok` on violations, no mutations.

**Critical rule:** Always called with `in_loop=false`, `in_switch=false`. Defer bodies create their own control-flow context; `break`/`continue` inside a defer body must not affect the enclosing loop/switch.

Rejected patterns inside defer bodies:
- `return`
- `goto`
- `break` / `continue` (since `in_loop=false, in_switch=false`, these always error)
- Recurses into GNU statement expressions `({…})` — `return`/`goto`/`break`/`continue` inside a stmt-expr in a defer body is rejected

**Forbidden function contexts:** Defer in functions that use `setjmp`/`longjmp`, `vfork`, or inline assembly is rejected via `FuncMeta.has_setjmp`/`.has_vfork`/`.has_asm`.

---

### 3.7 Phase 1G — Braceless Control Pre-Tagging

**Functions:** `p1_check_braceless`, `p1_scan_body_stmt`

When Phase 1 sees `if`/`for`/`while`/`do`/`else`/`switch`, it skips the `(…)` condition and checks the body. If the body is not preceded by `{`, a recursive descent scanner (`p1_scan_body_stmt`) walks the braceless statement. If any Prism keyword (`defer`, `orelse`, `raw`) or uninitialized declaration appears in the braceless body, `P1_NEEDS_BRACE_WRAP` is set on the control keyword token.

**Braceless body boundary parsing** (recursive descent on keyword tags):

- `{` → braced body, skip to matching `}` via `match_idx`
- `if` → parse `(…)`, recurse for then-body, check for `else`, recurse for else-body
- `for`/`while` → parse `(…)`, recurse for body
- `do` → recurse for body, consume `while (…)` and `;`
- `switch` → parse `(…)`, recurse for body
- Otherwise → scan to `;` with balanced delimiters

Pass 2 checks the flag at the `{`/`;` insertion point and emits wrapping braces.

---

### 3.8 Phase 1H — Orelse Pre-Classification

**Function:** `p1_classify_bracket_orelse`

Walks all tokens looking for `[…]` pairs containing `orelse`. Marks them with `P1_OE_BRACKET` on `pass1_ann`.

**File-scope guard:** If `P1_OE_BRACKET` occurs at brace_depth == 0 (file scope), a hard error is raised. Bracket orelse requires hoisting a temp variable statement, which is illegal at file scope (e.g., `void process(int buf[1 orelse 2]);` at depth 0).

**Declaration-init classification:** `P1_OE_DECL_INIT` is set during initializer scanning in Phase 1C+1D when a declaration's initializer contains `orelse`.

**Invalid context rejection (Phase 1):** Orelse inside `struct` bodies, inside `typeof`, inside certain invalid contexts is caught and errored.

**Bare-expression orelse** (`OE_BARE_ASSIGN`, `OE_BARE_ACTION`, `OE_BARE_COMPOUND`) is classified at emit time in Pass 2. This is safe because the symbol table is immutable.

---

## 4. Phase 2A — CFG Verification

**Function:** `p1_verify_cfg`

Runs after all Phase 1 sub-phases complete. For each function's `P1FuncEntry[]` array, performs an O(N) linear sweep with label hash table and monotonic watermark arrays.

### Algorithm

1. Build a label hash table (open-addressing, power-of-2) mapping label names to their entry index.

2. Maintain watermark arrays `wm_defer[]` and `wm_decl[]` indexed by scope_id. As the sweep encounters `P1K_DEFER` and `P1K_DECL` entries, it records their position in the entry array.

3. **Forward goto:** When a `P1K_GOTO` is encountered and its target label has not yet been seen, add it to a pending forward-goto list. When the target `P1K_LABEL` is reached, resolve by checking all `P1K_DEFER` and `P1K_DECL` entries between the goto and label positions. If any dangerous entry's scope is an ancestor-or-self of the label's scope, error.

4. **Backward goto:** When a `P1K_GOTO` targets a label already seen, check entries in `[0, wm_defer[label_index])` range. The scope-close check uses `label->token_index` (not the goto's token_index) because the goto jumps into the scope containing the label.

5. **Switch/case:** On `P1K_SWITCH`, snapshot the current watermarks. On `P1K_CASE`, compare current state against the switch's snapshot. Defers in ancestor-or-self scopes that were not active at the switch entry → error (defer skipped by fallthrough). Zero-initialized declarations in nested blocks → error (case bypasses initialization).

### Checked violations

| Violation | Severity |
|---|---|
| Forward goto skips over `defer` | Error (or warning with `-fno-safety`) |
| Forward goto skips over uninitialized declaration | Error (or warning with `-fno-safety`) |
| Forward goto skips over VLA declaration | Error (always, regardless of `-fno-safety`) |
| Backward goto enters scope containing `defer` | Error (or warning) |
| Backward goto enters scope containing uninitialized declaration | Error (or warning) |
| Switch/case skips `defer` via fallthrough | Error (or warning) |
| Switch/case bypasses zero-initialized declaration in nested block | Error (or warning) |

Forward gotos past `raw`-marked declarations are skipped (safe — no initialization to bypass).

### Complexity

O(N) per function where N is the number of `P1FuncEntry` items, plus O(depth) for `scope_is_ancestor_or_self` checks.

---

## 5. Pass 2 — Code Generation

A sequential token walk. Emits transformed C.

### Fast path

Tokens with no `tag` bits set and `!at_stmt_start` (~70–80% of tokens) are emitted directly via `emit_tok`. This path never reads `pass1_ann`.

### Slow path dispatch

Tokens with tag bits or at statement boundaries are dispatched to handlers:

| Handler | Trigger | Action |
|---|---|---|
| `handle_goto_keyword` | `TT_GOTO` | Emit defer cleanup (LIFO unwinding to target label depth), emit `goto`. Safety checks are in Phase 2A. |
| `handle_case_default` | `TT_CASE` / `TT_DEFAULT` | Reset defer stack to switch scope level. Error checks are in Phase 2A. |
| `handle_open_brace` | `{` | Push scope. Read `P1_SCOPE_*` from `pass1_ann` for classification. Handle compound-literal-in-ctrl-paren, orelse guard, stmt_expr detection. |
| `handle_close_brace` | `}` | Pop scopes, emit defers (LIFO), handle orelse guard close (consume trailing `;` to prevent dangling-else). |
| `try_zero_init_decl` | Statement start, type keyword/typedef | Parse declaration, insert `= {0}` or `= 0` or `memset` call. |
| `scan_labels_in_function` | Function body `{` | Build per-function label table for defer cleanup depth calculation in `handle_goto_keyword`. |
| Orelse handlers | `TT_ORELSE` | Multiple handlers for bracket, decl-init, block, bare-assign, bare-action, bare-compound forms. |
| FuncMeta lookup | Function body `{` | Read `FuncMeta[next_func_idx]` for return type and taint flags. |

### State maintained in Pass 2

| State | Purpose |
|---|---|
| Scope stack (push/pop) | Tracks current scope nesting, drives defer cleanup emission timing |
| Defer stack | `defer_add` at keyword, `emit_defers` at `}`/return/break/continue/goto |
| `at_stmt_start` | Inherently sequential — gates declaration detection |
| `ret_counter` | Monotonic counter for generating unique defer-cleanup label names |
| `block_depth` | Current block nesting depth |
| Line directive state | Last emitted line number for `#line` directive deduplication |
| `CtrlState` | Braceless control flow tracking: `pending`, `pending_for_paren`, `parens_just_closed`, `pending_orelse_guard`, `brace_depth` |

### Defer emission

Defers execute in LIFO order at scope exit — whether via `return`, `break`, `continue`, `goto`, or reaching `}`.

For `goto`: the handler looks up the target label's scope depth, then emits defers from the current scope level down to the target's level.

For `return`: emits all defers from the current scope to function scope. Uses `ret_counter` to generate unique labels for cleanup blocks.

`emit_deferred_range` handles defer bodies, including bare orelse and raw stripping within deferred code. `emit_deferred_bare_orelse` handles the case where a bare orelse expression appears inside a defer body.

### Defer-variable shadow checking

`check_defer_var_shadow` detects when a newly-declared variable name appears in an active defer body — this would silently capture the wrong variable at cleanup time.

---

## 6. Language Features

### 6.1 defer

**Syntax:** `defer <statement>;` or `defer { <block> }`

**Semantics:** Registers a cleanup action that executes when the enclosing scope exits. Multiple defers execute in LIFO order (last registered runs first).

**Scope exit triggers:** `}`, `return`, `break`, `continue`, `goto`.

**Statement expression interaction:** Defers inside `({…})` fire at the inner scope boundary, not the outer.

**Switch fallthrough:** Defers between cases are reset — they don't double-fire on fallthrough (verified by Phase 2A).

**Forbidden contexts:**
- Functions using `setjmp`/`longjmp` — error
- Functions using `vfork` — error (including `(vfork)()` paren-wrapped pattern)
- Functions using inline `asm` — error
- `return`, `goto` inside defer body — error
- `break`, `continue` inside defer body — error (defer body is not a loop/switch context)
- Computed goto (`goto *ptr`) with active defers — error

**Feature flag:** `-fno-defer` disables.

### 6.2 orelse

**Syntax:** `<expr> orelse <action>`

**Semantics:** Evaluates the expression. If the result is falsy (`!value` is true), executes the action. Works with any scalar type where `!value` is meaningful (pointers, integers, floats).

**Limitation:** Does not support struct/union values (no whole-struct zero check). Struct/union pointers work. Typedeffed structs pass through Prism and fail at the backend compiler.

**Forms:**

| Form | Example | Transformation |
|---|---|---|
| Control flow | `x = f() orelse return -1;` | `x = f(); if (!x) return -1;` (with defer cleanup) |
| Block | `x = f() orelse { log(); return -1; }` | `x = f(); if (!x) { log(); return -1; }` |
| Fallback value | `x = f() orelse "default"` | `x = f(); if (!x) x = "default";` |
| Bare expression | `do_init() orelse return -1;` | `if (!(do_init())) return -1;` |
| Bare assignment | `x = f() orelse return -1;` | `x = f(); if (!x) return -1;` |
| Bracket (array dim) | `int buf[n orelse 1]` | Temp variable hoisted before declaration |
| Decl-init | `int x = f() orelse 0;` | Expanded with temp and null check |

**Side-effect protection:** Bracket orelse in VLA/typeof contexts rejects expressions with side effects (`++`, `--`, `=`, volatile reads, function calls) to prevent double evaluation.

**File-scope guard:** Bracket orelse at file scope (brace_depth == 0) is a hard error — hoisting a temp variable requires a statement context.

**Invalid contexts:** Orelse inside struct bodies, typeof (in some forms), ternary, for-init control parens — errored in Pass 1.

**Feature flag:** `-fno-orelse` disables.

### 6.3 Zero-Initialization

**Semantics:** All local variable declarations without an explicit initializer get one:

| Declaration | Transformation |
|---|---|
| Scalar (`int x;`) | `int x = 0;` |
| Pointer (`char *p;`) | `char *p = 0;` |
| Aggregate (`struct S s;`) | `struct S s = {0};` |
| Array (`int arr[10];`) | `int arr[10] = {0};` |
| VLA (`int arr[n];`) | `int arr[n]; memset(arr, 0, sizeof(arr));` |
| Typedef-hidden VLA (`T x;` where T is VLA) | `T x; memset(x, 0, sizeof(x));` |

**Scope:** Only inside function bodies (`block_depth > 0`). Not at file scope. Not inside struct/union/enum definitions.

**Typedef awareness:** Uses the immutable typedef table to distinguish `size_t x;` (declaration → initialize) from `size_t * x;` (could be expression → don't touch). Tracks `is_aggregate` to handle struct typedefs that need `= {0}` instead of `= 0`.

**register _Atomic aggregate:** Rejected with an error — `_Atomic struct` with `register` storage class and zero-init would require a non-trivial store sequence.

**Feature flag:** `-fno-zeroinit` disables.

### 6.4 raw

**Syntax:** `raw <type> <name>;`

**Semantics:** Opts out of zero-initialization for a specific variable. The `raw` keyword is stripped from the output. The resulting declaration is emitted without an initializer.

**Safety interaction:** `raw`-marked variables can be safely jumped over by `goto` — the CFG verifier skips them in forward goto checks.

**Scope:** Works at block scope, file scope, and in struct bodies (stripped silently).

**Defer body:** `raw` is also stripped inside deferred code (`emit_deferred_range`).

### 6.5 Computed goto

`goto *<expr>` is supported. If active defers are present when a computed goto is executed, a hard error is raised (defer cleanup cannot be determined for an indirect jump target).

### 6.6 _Generic

`_Generic` expressions receive special handling: `case`/`default` inside `_Generic` association lists are not treated as switch cases (the `in_generic()` scope tracking prevents `handle_case_default` from firing inside `_Generic`).

### 6.7 Statement expressions

GNU statement expressions `({…})` are supported. They get their own scope in the scope tree (`is_stmt_expr = true`). Defers, declarations, and zero-initialization work correctly inside them. `walk_balanced` detects `({` patterns inside balanced groups and processes inner blocks with `try_zero_init_decl` + raw stripping.

---

## 7. Error Handling

### error_tok

`noreturn void error_tok(Token *tok, const char *fmt, ...)` — prints an error message with file/line/column context and calls `exit(1)`.

### warn_tok

`void warn_tok(Token *tok, const char *fmt, ...)` — prints a warning. Suppressed in library mode.

### -fno-safety

When `warn_safety` is enabled (`-fno-safety`), CFG violations (goto skipping defers/declarations, switch/case bypassing defers) are downgraded from errors to warnings. VLA skip violations remain hard errors regardless.

### longjmp error recovery (library mode)

In `PRISM_LIB_MODE`, `error_tok` triggers `longjmp(ctx->error_jmp)` instead of `exit(1)`. All arena-allocated structures are reclaimed by `arena_reset()`. `pass1_ann` survives arena resets (same lifecycle as token pool).

---

## 8. CLI Modes

| Mode | Command | Action |
|---|---|---|
| Compile (default) | `prism src.c -o out` | Transpile + pipe to backend CC |
| Run | `prism run src.c` | Compile to temp executable → execute |
| Transpile | `prism transpile src.c` | Emit transformed C to stdout |
| Install | `prism install` | Install binary to `/usr/local/bin/prism` |
| Passthrough | `prism -v` (no source files) | Forward all args to backend CC |
| Help | `prism --help` | Print usage |
| Version | `prism --version` | Print version + CC version |

### Feature flags

| Flag | Effect |
|---|---|
| `-fno-defer` | Disable defer |
| `-fno-zeroinit` | Disable zero-initialization |
| `-fno-orelse` | Disable orelse |
| `-fno-line-directives` | Disable #line directives |
| `-fno-safety` | Downgrade safety errors to warnings |
| `-fflatten-headers` | Flatten system headers into single output |
| `-fno-flatten-headers` | Disable header flattening |
| `--prism-cc=<compiler>` | Use specific compiler backend |
| `--prism-verbose` | Show commands being executed |

All other flags pass through to the backend compiler.

### Multi-file handling

Multiple `.c` files are each transpiled independently and passed to CC. Assembly (`.s`, `.S`), C++ (`.cc`, `.cpp`, `.cxx`), Objective-C (`.m`, `.mm`) files pass through untouched. C++ files trigger automatic `g++`/`clang++` backend selection.

### Compiler detection

`cc_is_clang` probes `<CC> --version` for "clang" when the basename doesn't match — handles Termux/FreeBSD/some Linux where `cc` or `gcc` symlinks to clang. Detects the backend to avoid passing unsupported flags (e.g., `-fpreprocessed` is clang-only).

### -x language handling

`compile_sources` extracts the user's `-x <lang>` from cc_args and uses it as the pipe language instead of hardcoded `"c"`.

---

## 9. Library Mode

Compile with `-DPRISM_LIB_MODE` to produce a library (excludes CLI `main()`).

**API:**

```c
PrismFeatures prism_defaults(void);
PrismResult   prism_transpile_file(const char *path, PrismFeatures features);
void          prism_free(PrismResult *r);
```

`PrismFeatures` struct fields: `compiler`, `include_paths`, `defines`, `compiler_flags`, `force_includes` (with respective counts), plus boolean feature flags (`defer`, `zeroinit`, `line_directives`, `warn_safety`, `flatten_headers`, `orelse`).

`PrismResult` returns status (`PRISM_OK`, `PRISM_ERR_SYNTAX`, `PRISM_ERR_SEMANTIC`, `PRISM_ERR_IO`) and the transpiled source.

Error recovery uses `setjmp`/`longjmp` — `error_tok` longjmps out, arena is reset, context pointers are NULLed.

---

## 10. Line Directives

Pass 2 emits `#line` directives so that compiler errors from the backend CC point to the original source file and line number, not the transpiled output. Directive emission is deduplicated (only emitted when the line number changes). Disabled with `-fno-line-directives`.

---

## 11. Self-Hosting

Prism is fully self-hosting:

```
Stage 0: cc -O2 -o prism_stage0 prism.c              (host compiler)
Stage 1: ./prism_stage0 prism.c -o prism_stage1       (Prism compiles itself)
Stage 2: ./prism_stage1 prism.c -o prism_stage2       (self-built Prism compiles itself)
```

The transpiled C output of stage1 and stage2 is identical (verified by the CI pipeline). Binary differences between stage1 and stage2 on macOS are due to Mach-O `LC_UUID` metadata, not code differences.

### CI Matrix

Linux x86_64, macOS x86_64/arm64, Windows build-only, Linux arm64, Linux riscv64.

---

## 12. Files

| File | Description |
|---|---|
| `prism.c` | Main transpiler — all Pass 1 phases, Pass 2 code generation, CLI |
| `parse.c` | Tokenizer, arena allocator, HashMap, `fast_hash`, `error_tok` / `warn_tok` |
| `windows.c` | Native Windows shim (used from `parse.c` for platform-specific I/O) |

`prism.c` includes `parse.c` via `#include`. Single compilation unit — no separate linking step.

---

## 13. Architectural Boundaries

### What Pass 2 does NOT do

Pass 2 is a near-pure code generator. It does not:

- Mutate the typedef table (`typedef_add_entry` is never called)
- Unwind the typedef scope (`typedef_pop_scope` does not exist)
- Register shadows (`register_decl_shadows`, `register_param_shadows`, `register_toplevel_shadows` do not exist)
- Walk the token stream for CFG safety checks (no `goto_skips_check`, no `backward_goto_skips_defer`, no `backward_goto_skips_decl`)
- Validate defer bodies (`validate_defer_statement` runs only in Phase 1F)
- Track return type state machines (`FuncMeta` provides return type data at function body entry)
- Register ghost enums during emit (`emit_tok` does not call `parse_enum_constants`)
- Read `CtrlState.scope_flags` (scope classification comes from `P1_SCOPE_*` in `pass1_ann`)

### What stays in Pass 2

These are inherently runtime and cannot move to Pass 1:

- **Scope stack push/pop** — driven by `{`/`}` during emit; timing drives defer cleanup
- **Defer stack** — `defer_add` at keyword, `emit_defers` at `}`/return/break/continue/goto
- **CtrlState** — braceless control-flow brace injection: `pending`, `pending_for_paren`, `parens_just_closed`, `pending_orelse_guard`, `brace_depth`. Tracking is inherently sequential and cannot move to Pass 1 without new infrastructure.
- **`at_stmt_start`** — inherently sequential
- **`ret_counter`** — monotonic during emit
- **Line directive / whitespace emission** — tied to output position
- **Bare-expression orelse classification** — requires expression boundary parsing; safe because symbol table is immutable
- **`scan_labels_in_function`** — builds per-function label table for `handle_goto_keyword` defer cleanup depth calculation (the Phase 1D arrays serve CFG verification, not defer emission)

---

## 14. Invariants

1. **Immutable symbol table:** After Phase 1B completes, the typedef table is frozen. Pass 2 performs zero mutations to the typedef table, scope tree, `pass1_ann`, or `func_meta`.
2. **All errors before emission:** Every `error_tok` call from semantic analysis fires during Pass 1 or Phase 2A. Pass 2 contains no safety-check `error_tok` calls.
3. **O(N) CFG verification:** `p1_verify_cfg` is guaranteed linear in the number of P1FuncEntry items per function. No O(N²) pairwise scans.
4. **Delimiter matching completeness:** Every `(`, `[`, `{` has a `match_idx`. Every `)`, `]`, `}` points back. No unmatched delimiters survive tokenization.
5. **Self-host fixed point:** Stage 1 and Stage 2 transpiled C output is identical.
6. **Arena safety:** All arena-allocated Pass 1 structures are reclaimed on `longjmp` error recovery. No dangling pointers after `arena_reset()`.

# Prism Transpiler Specification

**Version:** 0.120.0
**Status:** Implemented — every item in this document corresponds to behavior that exists in the codebase and is exercised by the test suite (3344+ tests + self-host stage1==stage2).

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

GNU extension `__auto_type` is tagged `TT_TYPE | TT_TYPEOF` so that declaration detection and goto-over-decl checking treat it identically to `typeof`/`__typeof__`.

### Taint graph

The tokenizer builds per-function taint flags (`has_setjmp`, `has_vfork`, `has_asm`) stored on the opening `{` token's tag bits (`TT_SPECIAL_FN`, `TT_NORETURN_FN`, `TT_ASM`). These remain on the token and are read directly via `func_meta[idx].body_open->tag` — they are **not** transferred to `FuncMeta` fields.

---

## 3. Pass 1 — Semantic Analysis

Pass 1 is a series of phases executed sequentially. Each phase augments the data structures built by prior phases.

### 3.0 Infrastructure

#### pass1_ann (parallel annotation array)

A `uint8_t` array parallel to the token pool. Indexed by token pool index via the `tok_ann(tok)` macro. Allocated alongside the token pool (realloc'd in `token_pool_ensure`). Freed by `tokenizer_teardown(true)`. Zeroed by `tokenizer_teardown(false)` (library-mode reuse) via `memset(p1_ann, 0, token_count)` before the pool index is reset — this prevents stale `|=`-applied flags (`P1_IS_DECL`, `P1_OE_BRACKET`, `P1_OE_DECL_INIT`) from leaking across consecutive `prism_transpile_*()` calls.

Flag definitions:

| Flag | Bit | Meaning |
|---|---|---|
| `P1_SCOPE_LOOP` | 1 | This `{` opens a loop body |
| `P1_SCOPE_SWITCH` | 2 | This `{` opens a switch body |
| `P1_SCOPE_CONDITIONAL` | 3 | This `{` opens a conditional body |
| `P1_OE_BRACKET` | 4 | `orelse` inside array dimension brackets `[…]` |
| `P1_OE_DECL_INIT` | 5 | `orelse` inside a declaration initializer |
| `P1_IS_DECL` | 6 | Phase 1D: token starts a variable declaration |

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
    is_init        : bool :1     — initializer brace: = { ... } (not a compound statement)
    is_enum        : bool :1     — set when is_struct=true and the keyword is 'enum'
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
    has_computed_goto      : bool     — function contains computed goto (*ptr)
    entry_start            : int      — start index into p1_entries[]
    entry_count            : int      — count of P1FuncEntry items
    defer_name_bloom       : uint64_t — FNV-1a bloom filter of identifiers in defer bodies
}
```

**Taint flags:** `has_setjmp`/`has_vfork`/`has_asm` are NOT stored in `FuncMeta`. They are tag bits (`TT_SPECIAL_FN`, `TT_NORETURN_FN`, `TT_ASM`) on the function body's opening `{` token, set by the tokenizer during taint-graph construction. Checked at defer-allocation time via `func_meta[idx].body_open->tag`.

Lookup during Pass 2: linear scan by `body_open` pointer (typically < 100 functions per TU).

#### Per-function entry array

```
P1FuncEntry {
    kind         : enum { P1K_LABEL, P1K_GOTO, P1K_DEFER, P1K_DECL, P1K_SWITCH, P1K_CASE }
    scope_id     : uint16_t
    token_index  : uint32_t
    tok          : Token*
    union {
        label  { name, len }                                      — for P1K_LABEL, P1K_GOTO
        decl   { has_init, is_vla, has_raw, is_static_storage }    — for P1K_DECL
        kase   { switch_scope_id }                                 — for P1K_CASE
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
| `typeof_vars[]` | `ARENA_ENSURE_CAP` (ctx, reused) | `arena_reset()` |
| `bracket_oe_ids[]` | `ARENA_ENSURE_CAP` (ctx, reused) | `arena_reset()` |

All arena-allocated structures are automatically reclaimed by `prism_reset()` → `arena_reset()` on any `error_tok()` that `longjmp`s out. `arena_alloc_uninit` has an overflow guard: `size > SIZE_MAX - (ARENA_ALIGN - 1)` is rejected before the alignment addition, preventing wrap-around to a tiny allocation.

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
| `struct`/`union`/`enum` keyword (or tag name preceded by `TT_SUE`) `{` | `is_struct` |
| `) {` where `(` preceded by `__attribute__`/`TT_ATTR` — walk further back past balanced parens and attributes to find the real keyword | Inherits classification from the real keyword (`is_loop`, `is_switch`, etc., or `is_struct` when `TT_SUE` found) |
| Named struct with attributes: `struct __attribute__((packed)) Name {` | `is_struct` (look-behind skips balanced parens and `TT_ATTR` tokens) |
| File-scope `{` preceded by `)` (function def) | `is_func_body` |
| File-scope `{` preceded by `]` (array-returning function, e.g., `int (*fn(void))[5] {`) or `;` (K&R function def) | `is_func_body` |
| `= {` (initializer brace) | `is_init` (also inherits `is_init` from parent scope for nested initializers) |
| `enum` keyword (or tag name preceded by enum `TT_SUE`) `{` | `is_struct` + `is_enum` |

**C23 attribute backward walk:** When walking backward to find `prev`, the loop skips `]]` C23 attributes by checking `tok_match(])->flags & TF_C23_ATTR` (the `TF_C23_ATTR` flag is set on the opening `[`, not the closing `]`). This ensures `void f(void) [[gnu::cold]] {` correctly finds `)` as `prev` and classifies it as `is_func_body`.

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

**Function parameter scope:** When Phase 1 encounters a function body `{` (is_func_body), parameter declarations from the preceding `(…)` are registered as shadows scoped to the function body. For forward declarations (prototypes ending with `;`), parameter shadows are registered scoped to the prototype's parameter list range. **Unnamed function pointer parameter safety:** `p1_register_param_shadows` only scans the first nested `(` group per parameter for declarator names (e.g., `(*cb)` in `int (*cb)(int a, int b)`). After scanning one `(` group, subsequent `(` groups (which contain inner parameter type lists) are skipped. This prevents phantom shadow registration from inner parameter names of unnamed function pointer parameters (e.g., `int (*)(int a, int b)` must not shadow `b` for the outer function scope).

**Attribute skipping during parameter list discovery:** The backward walk from `{` (or `;` for prototypes) to find the parameter list `(…)` skips GNU `__attribute__((...))` (detected via `TT_ATTR` tag on the preceding keyword) and C23 `[[...]]` attributes (detected via `TF_C23_ATTR` flag on the matching `[`). This ensures `void f(int T) __attribute__((noinline)) {` and `void f(int T) [[gnu::cold]] {` correctly identify the parameter list.

**For-init scope:** Variables declared in `for`-init (e.g., `for (int T = 0; …)`) are shadowed for the entire loop body (through the closing `}` or braceless statement end), not just through the `)`. This matches C99 §6.8.5p3: the for-init declaration's scope extends to the end of the loop body.

**Braceless body detection:** The for-init body end is determined by `skip_one_stmt()`, a recursive helper that correctly handles all C statement forms: braced `{…}`, `if (…) stmt [else stmt]`, `for`/`while`/`switch (…) stmt`, `do stmt while (…);`, and simple `expr;`. This prevents if-else branches from prematurely terminating the shadow scope (e.g., `for (int T=0;…) if(c) x=T; else T*x;` — the else branch is within T's shadow scope).

**Struct/union body scan (typedef declarations):** When a `typedef` contains a `struct`/`union` body (`typedef struct { … } name;`), Phase 1 scans the body for field names that shadow typedefs. A token `m` followed by `;`, `,`, `:`, `[`, or `=` where `m` is a known typedef triggers a shadow. **Anonymous bitfield exemption:** When the next token is `:` (bitfield width), a backward walk checks whether a type specifier (any token beyond qualifiers and attributes) precedes `m`. If no type precedes `m`, then `m` is the type specifier itself in an anonymous bitfield (`T : width;`), not a field name — no shadow is registered. This prevents typedef poisoning from patterns like `uint32_t : 5;` in hardware register definitions.

---

### 3.4 Phase 1D — Per-Function CFG Collection

**Executed inside:** `p1_full_depth_prescan`

For each function body, collects `P1FuncEntry` items into the global `p1_entries[]` array:

| Kind | What is recorded |
|---|---|
| `P1K_LABEL` | Label name, scope_id, token_index |
| `P1K_GOTO` | Target label name, scope_id, token_index |
| `P1K_DEFER` | scope_id, token_index |
| `P1K_DECL` | scope_id, token_index, `has_init`, `is_vla`, `has_raw`, `is_static_storage` |
| `P1K_SWITCH` | scope_id, token_index (braced switches use Phase 1A scope_id; braceless switches use a synthetic scope_id beyond `scope_tree_count`) |
| `P1K_CASE` | scope_id, token_index, `switch_scope_id` |

**Detection sites:** Labels and gotos are detected both at statement-start positions and inside braceless control flow (e.g., `if (c) goto L;`).

**For-init / if-switch-init declarations:** `p1_scan_init_shadows` also allocates `P1K_DECL` entries for variables declared in C99 for-init clauses (`for (int i = 0; ...)`) and C23 if/switch initializers (`if (int x = f(); x > 0)`). The scope_id assigned to these entries is the **body scope** (the `{` following the `)` of the control statement), not the enclosing scope. This ensures the CFG verifier catches gotos that jump *into* the loop/if/switch body past the init declaration, while still allowing gotos that jump *over* the entire control statement (where the variable is no longer in scope). For braceless bodies (no `{`), the entry uses the enclosing scope's `scope_id` (cur_sid) with a `body_close_idx` field set to the braceless body's end position (from `skip_one_stmt`). The CFG verifier uses `body_close_idx` when non-zero to correctly bound the variable's lifetime, allowing gotos that jump *over* the entire statement while catching gotos that jump *into* the braceless body. Consecutive `raw` keywords (e.g. from macro expansion like `raw raw int arr[n]`) are handled by a `while` loop, matching the pattern in `try_zero_init_decl`.

**VLA tracking:** `is_vla` on `P1FuncEntry.decl` is set when either the base type is a VLA typedef (`type.is_vla`) or the declaration itself has variable-length array dimensions (`decl.is_vla`). This covers both `typedef int T[n]; T x;` and direct `int x[n];` forms. Jumping past a VLA is always dangerous regardless of `has_init` or `has_raw`, because it bypasses implicit stack allocation.

**typeof VLA detection:** Inside `parse_type_specifier`, VLA array dimensions inside `typeof(...)` are detected by scanning for `[` preceded by a type keyword, typedef, `]`, or `*`, followed by a runtime-variable size (`array_size_is_vla`). Parenthesized types like `typeof((int[n]))` are detected at all paren depths. However, function-pointer parameter lists are skipped: a `(` preceded by `)` signals a function parameter list (e.g., `typeof(void(*)(int[n]))`), and `[n]` inside such lists describes parameter dimensions, not the outer type. This prevents false VLA flags on function pointers — important because `register` storage class variables cannot have their address taken, making memset-based zero-init illegal for them.

**has_raw:** Declarations marked `raw` set `has_raw = true`. In multi-declarator statements (`raw int x, y;`), `p1d_saw_raw` is reset on commas — only the first declarator receives `has_raw = true` (matching Pass 2's behavior). The CFG verifier skips `has_raw` declarations for goto checks, **except VLAs** — `raw` on a VLA does not exempt it.

**is_static_storage:** Set when the declaration has `static`, `extern`, `_Thread_local`, `thread_local`, or `__thread` (GNU extension) storage class. The prescan tracks this via `p1d_saw_static` (set when skipping `TT_STORAGE` tokens before the type specifier, and also checked via `type.has_static` / `type.has_extern` from `parse_type_specifier`). The CFG verifier exempts static-storage declarations from goto/switch checks because their initialization occurs at program/thread startup, not at the declaration point.

**CFG verifier declaration check:** The verifier flags ANY declaration jumped over by goto or case fallthrough, whether user-initialized (`int x = 5;`) or Prism-initialized (`int x;`). Jumping over a user-initialized variable leaves it indeterminate — the same vulnerability as jumping over a zero-initialized one. Exempt: `has_raw` (user opted out), `is_static_storage` (initialization not at declaration point). VLAs are always flagged regardless of other flags.

**FuncMeta linkage:** Each `FuncMeta` records `entry_start` and `entry_count`, indexing into `p1_entries[]`.

---

### 3.5 Phase 1E — Function Return Type Capture

**Executed inside:** `p1_full_depth_prescan`

At each file-scope function body `{`, captures the return type token range and flags into `FuncMeta`:

- `ret_type_start` / `ret_type_end` — token range for the return type
- `ret_type_suffix_start` / `ret_type_suffix_end` — for complex declarators (function pointers, arrays)
- `returns_void` — whether the function returns `void`

Taint flags (`TT_SPECIAL_FN`, `TT_NORETURN_FN`, `TT_ASM`) remain on the `body_open` token's tag and are read directly — they are not copied into FuncMeta fields.

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

**Forbidden function contexts:** Defer in functions that use `setjmp`/`longjmp`, `vfork`, or inline assembly is rejected via `TT_SPECIAL_FN`/`TT_NORETURN_FN`/`TT_ASM` tag bits on the function body's opening `{` token (checked as `func_meta[idx].body_open->tag`). The `TT_SPECIAL_FN` taint covers all standard and implementation-internal variants that survive preprocessing: `setjmp`, `_setjmp`, `__setjmp`, `__sigsetjmp`, `longjmp`, `_longjmp`, `__longjmp`, `__siglongjmp`, `__longjmp_chk`, `sigsetjmp`, `siglongjmp`, `__builtin_setjmp`, `__builtin_longjmp`, `__builtin_setjmp_receive`, `pthread_exit`, `savectx`. Detection is token-name-based: even bare references like `void (*fp)(jmp_buf, int) = longjmp;` taint the function because the `longjmp` token retains its `TT_SPECIAL_FN` tag. The only undetectable bypass is a function pointer passed from another translation unit (cross-TU indirect call), which is inherent to single-TU static analysis.

---

### 3.7 Phase 1G — Orelse Pre-Classification

**Function:** inline in `p1_full_depth_prescan`

Walks all tokens looking for `[…]` pairs containing `orelse`. Marks them with `P1_OE_BRACKET` on `pass1_ann`.

**Control-flow rejection (Phase 1G):** When an `orelse` token is found inside `[…]`, Phase 1G peeks at the next token. If it is a control-flow keyword (`return`, `break`, `continue`, `goto`) or `{` (block form), a hard error is raised immediately — before Pass 2 emits any output. Pass 2's `walk_balanced_orelse` retains the same checks as defense-in-depth assertions (unreachable by design).

**File-scope guard:** If `P1_OE_BRACKET` occurs outside any function body (`p1d_cur_func < 0`), a hard error is raised. Bracket orelse requires hoisting a temp variable statement, which is illegal outside a function body (e.g., `void process(int buf[1 orelse 2]);` at file scope).

**Declaration-init classification:** `P1_OE_DECL_INIT` is set during initializer scanning in Phase 1C+1D when a declaration's initializer contains `orelse`.

**Invalid context rejection (Phase 1):** Orelse inside `struct`/`union` bodies, inside `enum` bodies (enum constants must be compile-time integer constants), inside `typeof`, inside certain invalid contexts is caught and errored.

**Bare-expression orelse** (`OE_BARE_ASSIGN`, `OE_BARE_ACTION`, `OE_BARE_COMPOUND`) is classified at emit time in Pass 2. This is safe because the symbol table is immutable.

---

## 4. Phase 2A — CFG Verification

**Function:** `p1_verify_cfg`

Runs after all Phase 1 sub-phases complete. Gated by `F_DEFER | F_ZEROINIT` — skipped entirely when neither feature is enabled. For each function's `P1FuncEntry[]` array, performs an O(N) linear sweep with label hash table and monotonic watermark arrays.

### Algorithm

1. Build a label hash table (open-addressing, power-of-2) mapping label names to their entry index. During insertion, duplicate labels (same name appearing twice in the same function) are detected and rejected with a hard error.

2. Maintain watermark arrays `wm_defer[]` and `wm_decl[]` indexed by entry array position. As the sweep encounters `P1K_DEFER` and `P1K_DECL` entries, it appends them to `defer_list`/`decl_list` and records the current list lengths in the watermark arrays. Separate switch watermark arrays (`sw_defer_wm[]`, `sw_decl_wm[]`) are indexed by `scope_id` for O(1) lookup from case entries.

3. **Forward goto:** When a `P1K_GOTO` is encountered and its target label has not yet been seen, add it to a pending forward-goto list. When the target `P1K_LABEL` is reached, resolve by checking all `P1K_DEFER` and `P1K_DECL` entries between the goto and label positions. If any dangerous entry's scope is an ancestor-or-self of the label's scope, error.

4. **Backward goto:** When a `P1K_GOTO` targets a label already seen, check entries in `[0, wm_defer[label_index])` range. The scope-close check uses `label->token_index` (not the goto's token_index) because the goto jumps into the scope containing the label.

5. **Switch/case:** On `P1K_SWITCH`, snapshot the current watermarks. On `P1K_CASE`, compare current state against the switch's snapshot. Defers in ancestor-or-self scopes that were not active at the switch entry → error (defer skipped by fallthrough). Declarations in nested blocks (regardless of whether user-initialized or Prism-initialized) → error (case bypasses initialization), unless the declaration has `raw` or static storage duration.

6. **Statement-expression boundary:** Gotos into a GNU statement expression are rejected. When resolving a forward or backward goto, if the target label is inside a `is_stmt_expr` scope and the goto is not within that same statement expression (checked via `scope_stmt_expr_ancestor`), a hard error is raised. Gotos *out of* a statement expression are allowed (GCC/Clang support this and Prism's defer+goto idioms rely on it).

### Checked violations

| Violation | Severity |
|---|---|
| Forward goto skips over `defer` | Error (or warning with `-fno-safety`) |
| Forward goto skips over variable declaration | Error (or warning with `-fno-safety`) |
| Forward goto skips over VLA declaration | **Always error** (VLA skip is UB regardless of `-fno-safety`) |
| Backward goto enters scope containing `defer` | Error (or warning with `-fno-safety`) |
| Backward goto enters scope containing variable declaration | Error (or warning) |
| Backward goto enters scope containing VLA declaration | **Always error** (VLA skip is UB regardless of `-fno-safety`) |
| Switch/case skips `defer` via fallthrough | Error (or warning) |
| Switch/case bypasses variable declaration in nested block | Error (or warning) |

Forward gotos past `raw`-marked declarations are skipped (safe — user explicitly opted out of initialization safety), **except VLAs** — `raw` on a VLA does not exempt it because jumping past a VLA bypasses implicit stack allocation regardless of initialization. Declarations with static storage duration (`static`, `extern`, `_Thread_local`, `thread_local`, `__thread`) are also exempt — their initialization occurs at program/thread startup, not at the declaration point, so jumping past them does not leave the variable indeterminate.

### Complexity

O(N) per function where N is the number of `P1FuncEntry` items, plus O(depth) for `scope_is_ancestor_or_self` checks.

**Arena management:** Per-function temporary allocations (label hash table, watermark arrays, forward-goto list) are reclaimed after each function via `arena_mark`/`arena_restore`, preventing O(total_entries) memory accumulation across all functions in a translation unit.

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
| `handle_case_default` | `TT_CASE` / `TT_DEFAULT` | Bail out early if `ctrl_state.pending && parens_just_closed` (braceless switch body — no `SCOPE_BLOCK` was pushed, so `find_switch_scope()` would leak to an enclosing braced switch). Otherwise, reset defer stack to switch scope level. Error checks are in Phase 2A. |
| `handle_open_brace` | `{` | Push scope. Read `P1_SCOPE_*` from `pass1_ann` for classification. Handle compound-literal-in-ctrl-paren, orelse guard, stmt_expr detection. |
| `handle_close_brace` | `}` | Pop scopes, emit defers (LIFO), handle orelse guard close (consume trailing `;` to prevent dangling-else). |
| `try_zero_init_decl` | Statement start, type keyword/typedef | Parse declaration, insert `= {0}` or `= 0` or `memset` call. |
| `p1_label_find` | `TT_GOTO` dispatch | Query Phase 1D `p1_entries` array for label scope depth; no separate label table needed. |
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

`goto_scope_exits` counts brace-depth exits between a goto and its target label by scanning the token stream and counting only `{` / `}` tokens. Parentheses `(` `)` and brackets `[` `]` are intentionally excluded \u2014 they delimit expressions, not scopes. Counting them would miscalculate the scope-exit count for any goto whose token range happens to contain unmatched paren or bracket tokens (e.g. due to macro-expanded constructs). The call is guarded by `target_depth >= ctx->block_depth`, so it only fires when a same-depth or shallower label is the target.

For `return`: emits all defers from the current scope to function scope. Uses `ret_counter` to generate unique labels for cleanup blocks.

`emit_deferred_range` handles defer bodies, including bare orelse and raw stripping within deferred code. `emit_deferred_bare_orelse` handles the case where a bare orelse expression appears inside a defer body.

### Defer-variable shadow checking

`check_defer_var_shadow` detects when a newly-declared variable name appears in an active defer body — this would silently capture the wrong variable at cleanup time. Uses `FuncMeta.defer_name_bloom` (a 64-bit FNV-1a bloom filter of all identifier names in defer bodies) for an O(1) fast-reject before the O(N×M) body walk — eliminates the walk in the common case when no name matches. The body walk tracks brace depth: identifiers inside nested `{ }` blocks within the defer body (depth > 1) are skipped, since they are local declarations that cannot conflict with outer-scope variables. This avoids false positives for patterns like `defer { { int tmp = 1; } }` where the inner `tmp` is purely internal to the defer body.

`check_enum_typedef_defer_shadow` extends this protection to enum constants and typedef names. Called from the main Pass 2 loop at statement-start for enum definitions (`enum { name = val, ... }`) and typedef declarations (`typedef type name`), which bypass `process_declarators` and would otherwise evade shadow detection. Each introduced name is checked against active defer bodies via `check_defer_var_shadow`.

---

## 6. Language Features

### 6.1 defer

**Syntax:** `defer <statement>;` or `defer { <block> }`

**Semantics:** Registers a cleanup action that executes when the enclosing scope exits. Multiple defers execute in LIFO order (last registered runs first).

**Scope exit triggers:** `}`, `return`, `break`, `continue`, `goto`.

**Statement expression interaction:** Defers inside `({…})` fire at the inner scope boundary, not the outer.  `defer` at the direct top level of a statement expression is rejected; it must be wrapped in a nested block.  Additionally, if that nested block is the **last statement** of the statement expression, the defer emission would overwrite the expression's return value, so Prism rejects this pattern too — the defer block must not be the last statement (add a final expression after it).

**Switch fallthrough:** Defers between cases are reset — they don't double-fire on fallthrough (verified by Phase 2A).

**Forbidden contexts:**
- Functions using `setjmp`/`longjmp` — error
- Functions using `vfork` — error (including `(vfork)()` paren-wrapped pattern)
- Functions using inline `asm` — error
- `return`, `goto` inside defer body — error
- `break`, `continue` inside defer body — error (defer body is not a loop/switch context)
- Computed goto (`goto *ptr`) with active defers or zero-initialized declarations — error
- `defer` at the top level of a GNU statement expression — error (use a nested block)
- A block containing `defer` as the **last statement** of a GNU statement expression — error. `handle_close_brace` emits defers after the last expression, overwriting the expression's return value (void defers → compile error; non-void defers → silent wrong-value assignment). Without an expression parser there is no safe transformation; the user must place the defer block before the final expression rather than as the last statement.

**Feature flag:** `-fno-defer` disables.

### 6.2 orelse

**Syntax:** `<expr> orelse <action>`

**Semantics:** Evaluates the expression. If the result is falsy (`!value` is true), executes the action. Works with any scalar type where `!value` is meaningful (pointers, integers, floats).

**Limitation:** Does not support struct/union values (no whole-struct zero check). Struct/union pointers work. Typedeffed structs pass through Prism and fail at the backend compiler. `typeof(const struct S)` and `typeof(aggregate_typedef)` are detected by `parse_type_specifier` (sets `is_struct` when `TT_SUE` or `TDF_AGGREGATE` appears inside the `typeof` expression) and rejected for value-fallback orelse. Opaque `typeof(variable)` where the variable's type is an aggregate cannot be detected at the token level — the backend compiler reports a clear error (`used type ... where arithmetic type is required`).

**Forms:**

| Form | Example | Transformation |
|---|---|---|
| Control flow | `x = f() orelse return -1;` | `{ if (!(x = f())) { return -1; } }` (with defer cleanup) |
| Block | `x = f() orelse { log(); return -1; }` | `{ if (!(x = f())) { log(); return -1; } }` |
| Fallback value | `x = f() orelse 0` | `{ if (!(x = f())) x = (0); }` |
| Bare expression | `do_init() orelse return -1;` | `{ if (!(do_init())) { return -1; } }` |
| Bracket (array dim) | `int buf[n orelse 1]` | Temp variable hoisted before declaration |
| Paren-wrapped bracket | `int buf[(n orelse 1)]` | Outer parens stripped; same hoisting as plain bracket form |
| Decl-init | `int x = f() orelse 0;` | Expanded with temp and null check |
| Stmt-expr decl-init | `int x = ({...}) orelse 0;` | `int x = ({...}); x = x ? x : 0;` |

**Paren-wrapped bracket orelse:** `int arr[(f() orelse 1)]` — the outer `(...)` is a common macro-protection pattern (e.g. `#define DIM (f() orelse 1)`). When the parentheses span the *entire* bracket content, Prism strips them and applies the standard hoisting transformation. Deeper wrapping or partial wrapping (e.g. `[(f() orelse 1) + 2]`) is caught as a hard error with a diagnostic suggesting removal of the outer parens.

**Volatile safety:** All forms that include an assignment in the condition use the C assignment-expression value — `(LHS = expr)` yields `expr`'s value without re-reading `LHS`. This makes bare orelse safe for volatile pointer-dereference targets such as MMIO registers: `*uart_tx = get_byte() orelse 0xFF` emits `{ if (!(*uart_tx = get_byte())) *uart_tx = (0xFF); }` with no hidden re-read of the register. Compound-literal fallbacks use a `(LHS = RHS) ? (void)0 : (void)(LHS = (fb))` ternary (same safety property).

**Side-effect protection:** Bracket orelse in VLA/typeof contexts rejects expressions with side effects (`++`, `--`, `=`, volatile reads, function calls) to prevent double evaluation. Function-call detection recognizes both `ident(` and `)(`  (parenthesized call) patterns.

**Constant dimension optimization:** When hoisting preceding bracket dimensions to preserve C99 left-to-right evaluation order, single numeric literal dimensions (e.g., `[5]`) are left in-place rather than hoisted to a temp variable — they have no side effects and cannot be reordered.

**File-scope guard:** Bracket orelse at file scope (brace_depth == 0) is a hard error — hoisting a temp variable requires a statement context.

**Invalid contexts:** Detected at two stages:

- **Phase 1 (early rejection):** Bracket orelse at file scope, bracket orelse with control-flow actions (return/goto/break/continue) or block form, orelse inside enum bodies (compile-time constant context), typeof-orelse inside struct/union bodies. The typeof-in-struct check runs during `p1_full_depth_prescan` using the scope tree's `is_struct` flag; the enum check uses `is_enum` on the scope tree.
- **Pass 2 catch-all:** Any `orelse` token that survives to the main emit loop without being consumed by a handler (bracket, decl-init, bare, typeof, walk_balanced) is **unconditionally** rejected with a hard error. This catches orelse in struct/union member declarations, ternary contexts, for-init control parens, and any other unsupported position. No context exemptions — bracket/typeof orelse is fully consumed before reaching the catch-all, so it never fires on valid uses.
- **Pass 2 typeof dispatch:** `typeof(expr orelse fallback)` outside declaration contexts (e.g., inside `sizeof()`, casts) is caught by a `TT_TYPEOF` handler in the main emit loop that routes through `walk_balanced_orelse`, ensuring the inner orelse is transformed before the catch-all fires.

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

**Storage class exclusions:** Variables with `static`, `extern`, `_Thread_local`, or `thread_local` storage class are **not** zero-initialized. C guarantees these are zero-initialized by the loader (static/extern) or runtime (_Thread_local/thread_local). Emitting `= 0` for static locals would move them from `.bss` to `.data`; emitting `memset` would re-zero them on every function entry, breaking static semantics.

**Typedef awareness:** Uses the immutable typedef table to distinguish `size_t x;` (declaration → initialize) from `size_t * x;` (could be expression → don't touch). Tracks `is_aggregate` to handle struct typedefs that need `= {0}` instead of `= 0`.

**register _Atomic aggregate:** Rejected with an error — `_Atomic struct` with `register` storage class and zero-init would require a non-trivial store sequence.

**Feature flag:** `-fno-zeroinit` disables.

### 6.4 raw

**Syntax:** `raw <type> <name>;`

**Semantics:** Opts out of zero-initialization for a specific variable. The `raw` keyword is stripped from the output. The resulting declaration is emitted without an initializer. Consecutive `raw` keywords (e.g. `raw raw int x;` from macro expansion) are handled gracefully: `is_raw_declaration_context()` recognizes `TF_RAW` as a valid successor, and each stripping site (`try_zero_init_decl`, `walk_balanced` stmt-start, file-scope loop) skips all consecutive `raw` tokens before the type.

**Multi-declarator scope:** `raw` applies only to the **first** declarator in a comma-separated declaration. In `raw int x, y;`, only `x` opts out of zero-initialization; `y` is still zero-initialized. Both Phase 1D (`p1d_saw_raw` reset on `,`) and Pass 2 (`is_raw` reset on `,`) enforce this.

**Safety interaction:** `raw`-marked variables can be safely jumped over by `goto` — the CFG verifier skips them in forward goto checks, **except VLAs** where `raw` does not exempt the declaration.

**Scope:** Works at block scope, file scope, and in struct bodies (stripped silently).

**Defer body:** `raw` is also stripped inside deferred code (`emit_deferred_range`).

### 6.5 Computed goto

`goto *<expr>` is supported. `FuncMeta.has_computed_goto` is set during Phase 1D when `goto *` is detected. During Phase 2A (`p1_verify_cfg`), if a function contains both a computed goto and either (a) any `P1K_DEFER` entries (when `F_DEFER` is enabled) or (b) any non-`raw`, non-static-storage `P1K_DECL` entries (when `F_ZEROINIT` is enabled), a hard error is raised — the jump target cannot be verified at compile time, so defer cleanup or zero-initialization could be bypassed.

### 6.6 _Generic

`_Generic` expressions receive special handling: `case`/`default` inside `_Generic` association lists are not treated as switch cases (the `in_generic()` scope tracking prevents `handle_case_default` from firing inside `_Generic`).

### 6.7 Statement expressions

GNU statement expressions `({…})` are supported. They get their own scope in the scope tree (`is_stmt_expr = true`). Declarations and zero-initialization work correctly inside them. `walk_balanced` detects `({` patterns (including when called directly on a stmt-expr `(`) and processes inner blocks with full keyword dispatch (defer, goto, return/break/continue), `try_zero_init_decl`, raw stripping, and orelse transformation (via `emit_deferred_orelse` at stmt-start with a catch-all `error_tok` for unprocessed orelse tokens, preventing literal "orelse" from leaking to the C output). The inner loop's `itag` variable uses `uint32_t` (matching `Token.tag`'s width) to avoid truncation of high-bit tags like `TT_DEFER` (1<<20), `TT_GOTO` (1<<17), and `TT_ORELSE` (1<<30). `walk_balanced_orelse` (used for array dimensions when `F_ORELSE` is enabled) also detects `({` patterns in its no-orelse emit path and routes them through `walk_balanced` for full transpilation.

**Defer constraint:** A block containing `defer` must not be the last statement of the statement expression (the defer emission would overwrite the expression's return value). Prism detects this at `}` close time: if the parent scope is `is_stmt_expr` and the next non-noise token after `}` is `}` (the stmt-expr close), it emits a hard error pointing to the `defer` keyword.  The user must place the defer block before the final expression: `int fd = ({ int r; { defer cleanup(); r = work(); } r; });` or restructure without a statement expression.

**Multi-declarator stmt-expr:** `int x = ({ int tmp = f() orelse 0; tmp; }), y = 5;` — when the comma after a stmt-expr initializer forces `process_declarators`, `check_orelse_in_parens` skips stmt-expr boundaries (bails early for `({` patterns), and `walk_balanced` processes the inner content with full scope handling.

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

---

## 7.1 Implementation Limits

| Limit | Value | Error |
|---|---|---|
| Scope count (scope_id) | 65,534 | `scope tree: too many scopes (>65534)` |
| Brace nesting depth | 4,096 | `brace nesting depth exceeds 4096` |
| Switch nesting depth | 256 | `switch nesting depth exceeds 256` |
| Braceless switch synthetic scopes | Limited by remaining scope_id range | `too many scopes + braceless switches (>65535)` |

These limits are enforced with hard errors. Exceeding any limit halts transpilation.

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

### Non-flatten define re-emission

When `-fno-flatten-headers` is active, Prism runs `cc -E` which consumes in-file `#define` directives. The transpiled output reconstructs `#include` directives but must also re-emit the user's `#define`s that appeared before the first `#include`. `collect_source_defines` scans the original source file (raw text, not tokens) and extracts non-function-like `#define NAME VALUE` directives, both unconditional and conditional. These are re-emitted as `#ifndef NAME` / `#define NAME VALUE` / `#endif` guards by `emit_consumed_defines` before any `#include` directives.

**Conditional guard extraction:** Defines inside `#ifdef`/`#ifndef`/`#if`/`#elif`/`#else` blocks are extracted along with their enclosing preprocessor guard text. A condition stack (max depth 32) tracks each nesting level's opening directive (e.g., `#ifdef __APPLE__\n`) and current branch directive (e.g., `#else\n`). When a `#define` is found inside conditional blocks, `emit_consumed_defines` wraps it in the reconstructed guard (concatenation of all active condition stack entries' opening/branch text) followed by matching `#endif` lines. This preserves platform-gated defines like `#ifdef __APPLE__ / #define _DARWIN_C_SOURCE / #endif`.

The scanner handles: multi-line block comments (tracked via `in_block_comment`), line continuations (`\` at end of line), inline block comments between `#` and the directive name, `#ifdef`/`#ifndef`/`#if`/`#elif`/`#else`/`#endif` nesting with condition stack tracking, multi-line continuation values, and `#define`s that follow block comment closings on the same line.

---

## 9. Library Mode

Compile with `-DPRISM_LIB_MODE` to produce a library (excludes CLI `main()`).

**API:**

```c
PrismFeatures prism_defaults(void);
PrismResult   prism_transpile_file(const char *path, PrismFeatures features);
PrismResult   prism_transpile_source(const char *source, const char *filename,
                                      PrismFeatures features);
void          prism_free(PrismResult *r);
void          prism_reset(void);
void          prism_thread_cleanup(void);
```

`prism_transpile_source` transpiles already-preprocessed source text without invoking `cc -E`. Useful for IDE integrations that preprocess separately.

`prism_reset` reclaims all arena-allocated structures. Called automatically on error recovery.

`prism_thread_cleanup` frees thread-local hash table buckets. Must be called before a thread exits to avoid leaks in long-lived host processes.

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
- **`p1_label_find`** — queries the immutable Phase 1D `p1_entries` array for label scope depth; `handle_goto_keyword` reads CFG artifacts directly with no separate label scan

---

## 14. Invariants

1. **Immutable symbol table:** After Phase 1B completes, the typedef table is frozen. Pass 2 performs zero mutations to the typedef table, scope tree, `pass1_ann`, or `func_meta`.
2. **All errors before emission:** Every user-triggerable `error_tok` call from semantic analysis fires during Pass 1 or Phase 2A, before Pass 2 emits its first byte. Pass 2 contains defensive `error_tok` calls in `process_declarators`, `emit_bare_orelse_impl`, `handle_defer_keyword`, etc. that serve as unreachable-by-design assertions — they guard against internal inconsistencies that would indicate a Pass 1 gap, not against user input that should have been caught earlier.
3. **O(N) CFG verification:** `p1_verify_cfg` is guaranteed linear in the number of P1FuncEntry items per function. No O(N²) pairwise scans.
4. **Delimiter matching completeness:** Every `(`, `[`, `{` has a `match_idx`. Every `)`, `]`, `}` points back. No unmatched delimiters survive tokenization.
5. **Self-host fixed point:** Stage 1 and Stage 2 transpiled C output is identical.
6. **Arena safety:** All arena-allocated Pass 1 structures are reclaimed on `longjmp` error recovery. No dangling pointers after `arena_reset()`.
7. **Signal cleanup safety:** `signal_temps_clear()` zeroes the **entire** buffer (`memset`, `PATH_MAX` bytes) of every registered path slot before resetting the counter. `signal_cleanup_handler` skips entries where `signal_temps[i][0] == '\0'`. This eliminates the TOCTOU race between `signal_temps_register`'s CAS (counter increment) and `memcpy` (path write): if a signal arrives during `memcpy`, partially written data is followed by zeroes (from the prior `memset`), so the handler sees at worst a truncated — but never fabricated — path. Previous behavior (zeroing only byte 0) left stale path data in bytes 1..N, which could reconstruct a prior cycle's path if `memcpy` was interrupted after writing just the first byte.

---

## 15. Known Limitations and Caveats

1. **Struct padding bytes and `= {0}`:** Prism emits `= {0}` for aggregate zero-initialization. The C standard guarantees all *members* are zeroed but does not mandate that *padding bytes* between members are zeroed. In practice, GCC and Clang emit `memset`/`bzero` for `= {0}` at all optimization levels, zeroing the entire aggregate including padding. However, code that copies raw struct bytes across trust boundaries (e.g., `copy_to_user` in kernel contexts) should use explicit `memset(&s, 0, sizeof(s))` via `raw` + manual initialization to guarantee no padding infoleak. This is a C language limitation, not a Prism deficiency.

2. **Indirect call taint bypass (cross-TU):** The `setjmp`/`longjmp`/`vfork` taint detection is token-name-based: any appearance of a tainted identifier (even as a bare reference like `= longjmp`) taints the enclosing function. However, a function pointer to `longjmp` passed from another translation unit (e.g., via a `void (*)(jmp_buf, int)` parameter) is undetectable by single-TU static analysis. This is an inherent limitation shared with all C static analyzers that operate on individual translation units.

3. **Bitfield zero-initialization:** Bitfield member declarations (`int x : 4;`) inside struct/union bodies are not individually zero-initialized — `try_zero_init_decl` correctly skips when `in_struct_body()` is true (bitfield syntax `int x : 4 = 0;` is invalid C). Bitfields are zeroed implicitly when the parent struct variable receives `= {0}`. This is working as designed.
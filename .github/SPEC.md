# Prism Transpiler Specification

**Version:** 1.1.0
**Status:** Implemented — every item in this document corresponds to behavior that exists in the codebase and is exercised by the test suite (5216+ tests + self-host stage1==stage2).

This document describes what the transpiler **does**, not what it aspires to do. It is organized in two parts: **Part I** covers the transpiler's architecture, internal processing model, and implementation details. **Part II** provides a formal language specification for Prism's extensions to C, described in terms of the C abstract machine independently of any implementation strategy.

---

# Part I: Transpiler Architecture & Internals

## 1. Overview

Prism is a source-to-source C transpiler. It reads preprocessed C, transforms it, and emits standard C. It adds three language features — `defer`, `orelse`, and automatic zero-initialization — and enforces safety rules at transpile time. It is a single compilation unit (`prism.c` includes `parse.c`; `windows.c` is a native Windows shim).

**Standards compatibility:** Prism accepts C99, C11, and C23 input and emits standard C compatible with GCC, Clang, and MSVC. All standard C type specifiers, qualifiers, storage classes, attributes, and control-flow constructs are recognized and passed through correctly. C23 features including `typeof_unqual`, `constexpr`, `auto` type inference, `_BitInt(N)`, `[[...]]` attributes, `alignas`/`alignof`, `static_assert`, fixed-underlying-type enums (`enum E : int { ... }`), labeled declarations, and if/switch initializers are supported. GCC extensions `__typeof_unqual__` and `__typeof_unqual` are recognized as `TT_TYPE | TT_TYPEOF` and handled identically to C23 `typeof_unqual`. C23 TS 18661 extended float types (`_Float16`, `_Float32`, `_Float64`, `_Float128`, `_Float32x`, `_Float64x`, `_Float128x`) and decimal float types (`_Decimal32`, `_Decimal64`, `_Decimal128`) are registered as `TT_TYPE` keywords, ensuring zero-initialization and declaration detection work correctly. GCC-specific float types (`__float128`, `__float80`, `__fp16`, `__bf16`) are similarly registered. Do note Prism IS NOT officially certified in any way.

Note: Prism is thoroughly empirically tested (self-hosting and 5216+ test cases) but **is not formally verified.** It is designed to compile standard-compliant code, but **may not catch every obscure constraint violation** defined by the ISO C standard.

The transpiler operates in two passes:

1. **Pass 1** — Full semantic analysis over all tokens at all depths. Builds immutable data structures (scope tree, symbol table, shadow table, per-function CFG arrays). Raises all errors. No output is emitted.
2. **Pass 2** — Code generation. Reads Pass 1's immutable artifacts. Emits transformed C. No type table mutations, no safety checks, no speculative token walking. If Pass 2 starts, it runs to completion.

**Key invariant:** Every semantic error is raised before Pass 2 emits its first byte.

---

## 2. Tokenizer (Pass 0)

Defined in `parse.c`. Produces a flat pool of `Token` structs.

### Token structure (24 bytes, hot path)

| Field | Type | Description |
|---|---|---|
| `tag` | `uint32_t` | Bitmask of `TT_*` keyword/semantic tags |
| `next_idx` | `uint32_t` | Pool index of next token (0 = NULL) |
| `match_idx` | `uint32_t` | Pool index of matching delimiter (0 = none) |
| `len` | `uint32_t` | Byte length of the token text |
| `kind` | `uint8_t` | `TK_IDENT`, `TK_KEYWORD`, `TK_PUNCT`, `TK_STR`, `TK_NUM`, `TK_PREP_DIR`, `TK_EOF` |
| `flags` | `uint8_t` | `TF_AT_BOL`, `TF_HAS_SPACE`, `TF_IS_FLOAT`, `TF_OPEN`, `TF_CLOSE`, `TF_C23_ATTR`, `TF_RAW`, `TF_SIZEOF` (also set on `__builtin_offsetof` and `offsetof`) |
| `ann` | `uint16_t` | Pass 1 annotation flags (`P1_SCOPE_*`, `P1_OE_*`, `P1_IS_DECL`, `P1_REJECTED`). Zeroed by `new_token()` on allocation. |
| `ch0` | `uint8_t` | First source byte — avoids `tok_loc()` indirection in hot paths |

Cold path data (`TokenCold`, separate array): `loc_offset`, `line_no` (18-bit), `file_idx` (14-bit).

### Delimiter matching

Every `(`, `[`, `{` gets a `match_idx` pointing to its closing pair. Every `)`, `]`, `}` points back. This is computed during tokenization and is used pervasively in both passes.

### Keyword tagging

Identifiers and keywords are tagged with `TT_*` bitmask flags during tokenization. 32 tag bits are defined:

`TT_TYPE`, `TT_QUALIFIER`, `TT_SUE` (struct/union/enum), `TT_SKIP_DECL`, `TT_ATTR`, `TT_ASSIGN`, `TT_MEMBER`, `TT_LOOP`, `TT_STORAGE`, `TT_ASM`, `TT_INLINE`, `TT_NORETURN_FN`, `TT_SPECIAL_FN`, `TT_CONST`, `TT_RETURN`, `TT_BREAK`, `TT_CONTINUE`, `TT_GOTO`, `TT_CASE`, `TT_DEFAULT`, `TT_DEFER`, `TT_GENERIC`, `TT_SWITCH`, `TT_IF`, `TT_TYPEDEF`, `TT_VOLATILE`, `TT_REGISTER`, `TT_TYPEOF`, `TT_BITINT`, `TT_ALIGNAS`, `TT_ORELSE`, `TT_STRUCTURAL`.

GNU extension `__auto_type` is tagged `TT_TYPE | TT_TYPEOF` so that declaration detection and goto-over-decl checking treat it identically to `typeof`/`__typeof__`. The `typeof_unqual` family (`typeof_unqual`, `__typeof_unqual__`, `__typeof_unqual`) is tagged `TT_TYPE | TT_TYPEOF`; `parse_type_specifier` detects the unqual variant via `tok->len >= 13` (all three lengths — 13, 15, 17 — exceed the longest non-unqual typeof variant `__auto_type` at 11) and skips the inner qualifier propagation scan, since `typeof_unqual` strips qualifiers by definition.

### Taint graph

The tokenizer builds per-function taint flags (`has_setjmp`, `has_vfork`, `has_asm_goto`) stored on the opening `{` token's tag bits (`TT_SPECIAL_FN`, `TT_NORETURN_FN`, `TT_ASM`). These remain on the token and are read directly via `func_meta[idx].body_open->tag` — they are **not** transferred to `FuncMeta` fields. The `TT_ASM` taint fires only for `asm goto` — the tokenizer scans between the `asm` keyword and `(` for a `TT_GOTO` token. Regular `asm` (volatile, inline) is safe and does not taint.

**Noreturn attribute scanning:** After tokenization, the tokenizer scans all tokens for noreturn annotations and tags every occurrence of the function name with `TT_NORETURN_FN`. Three attribute syntaxes are recognized:

- **`_Noreturn` / `noreturn` keyword:** Bare keyword check (`equal(t, "_Noreturn") || equal(t, "noreturn")`).
- **C23 `[[noreturn]]` / `[[_Noreturn]]` / `[[__noreturn__]]`:** When a `[` token with `TF_C23_ATTR` is found, all `TK_IDENT` tokens between `[[` and `]]` are scanned for `noreturn`, `_Noreturn`, or `__noreturn__`. This handles namespaced forms (`[[gnu::noreturn]]`, `[[gnu::__noreturn__]]`) because the loop skips non-matching identifiers.
- **GNU `__attribute__((noreturn))` / `__attribute__((__noreturn__))` / `__attribute__((cold, noreturn))`:** All `TK_IDENT` tokens between the inner `((` and matching `)` are scanned for `noreturn` or `__noreturn__`. This handles comma-separated attribute lists where noreturn is not the first attribute.
- **MSVC `__declspec(noreturn)` / `__declspec(__noreturn__)`:** All `TK_IDENT` tokens between `(` and matching `)` are scanned for `noreturn` or `__noreturn__`.

### C23 extended float suffix normalization

`emit_tok_special` normalizes C23 extended float suffixes on numeric literals to standard C suffixes: `f16`/`bf16` → `f` (`float`), `f32` → `f` (`float`), `f32x` → (none, `double`), `f64` → (none, `double`), `f64x` → `L` (`long double`), `f128`/`f128x` → `L` (`long double`). When an `f128` or `f128x` suffix is downcast to `long double`, a warning is emitted: `"C23 _Float128 literal truncated to long double; precision may be lost on platforms where long double is 80-bit"` — on x86 extended precision, `long double` is 80-bit rather than IEEE 754 binary128.

---

## 3. Pass 1 — Semantic Analysis

Pass 1 is a series of phases executed sequentially. Each phase augments the data structures built by prior phases.

### 3.0 Infrastructure

#### Token annotation field (`ann`)

The `uint8_t ann` field in the Token struct stores Pass 1 annotation flags. Accessed via the `tok_ann(tok)` macro (expands to `(tok)->ann`). Zeroed by `new_token()` on allocation — this prevents stale `|=`-applied flags (`P1_IS_DECL`, `P1_OE_BRACKET`, `P1_OE_DECL_INIT`) from leaking across consecutive `prism_transpile_*()` calls when the token pool is reused.

Flag definitions:

| Flag | Bit | Meaning |
|---|---|---|
| `P1_IS_TYPEDEF` | 0 | Token resolves to a real typedef at this position (baked by Phase 1 annotation pass) |
| `P1_SCOPE_LOOP` | 1 | This `{` opens a loop body |
| `P1_SCOPE_SWITCH` | 2 | This `{` opens a switch body |
| `P1_HAS_ENTRY` | 3 | Token has any typedef-table entry (typedef, enum constant, shadow, or VLA variable) |
| `P1_OE_BRACKET` | 4 | `orelse` inside array dimension brackets `[…]` |
| `P1_OE_DECL_INIT` | 5 | `orelse` inside a declaration initializer |
| `P1_IS_DECL` | 6 | Phase 1D: token starts a variable declaration |
| `P1_SCOPE_INIT` | 7 | This `{` opens an initializer (compound literal, `= {...}`) |

**Cache discipline:** Pass 2 only reads `tok->ann` when a token's tag matches `TT_STRUCTURAL`, `TT_IF|TT_LOOP|TT_SWITCH`, or `TT_ORELSE`. The fast path (~70–80% of tokens) never reads the annotation.

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

#### Pass 2 scope stack

```
ScopeKind enum:
    SCOPE_BLOCK       — { ... } block scope
    SCOPE_INIT        — = { ... } initializer brace (does NOT increment block_depth)
    SCOPE_FOR_PAREN   — for( ... ) — first ';' ends init, not stmt
    SCOPE_CTRL_PAREN  — if/while/switch( ... )
    SCOPE_GENERIC     — _Generic( ... )
    SCOPE_TERNARY     — ? ... : — popped on matching ':'
```

```
ScopeNode {
    defer_start_idx    : int       — index into defer stack at scope entry
    kind               : uint8_t   — ScopeKind value
    is_loop            : bool :1
    is_switch          : bool :1
    is_struct          : bool :1
    is_stmt_expr       : bool :1   — GNU statement expression ({...})
    is_ctrl_se         : bool :1   — stmt-expr inside ctrl parens (ctrl_state saved on ctrl_save_stack)
}
```

The `scope_stack[]` is distinct from `scope_tree[]` (Phase 1A). The scope tree is a flat immutable array indexed by `scope_id`; the scope stack is a mutable LIFO stack driven by `{`/`}` during Pass 2 emission.

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
    defer_name_set         : HashMap  — exact set of captured names (union of all defer bodies)
    defer_body_captures    : HashMap  — tok_idx(body) → HashMap* (per-body capture sets)
    label_hash             : int*     — open-addressing hash table: name → entry index (-1=empty)
    label_hash_mask        : int      — power-of-2 mask for label_hash probing
}
```

**Taint flags:** `has_setjmp`/`has_vfork`/`has_asm_goto` are NOT stored in `FuncMeta`. They are tag bits (`TT_SPECIAL_FN`, `TT_NORETURN_FN`, `TT_ASM`) on the function body's opening `{` token, set by the tokenizer during taint-graph construction. Checked at defer-allocation time via `func_meta[idx].body_open->tag`.

Lookup during Pass 2: linear scan by `body_open` pointer (typically < 100 functions per TU).

#### Per-function entry array

```
P1FuncEntry {
    kind         : enum { P1K_LABEL, P1K_GOTO, P1K_DEFER, P1K_DECL, P1K_SWITCH, P1K_CASE }
    scope_id     : uint16_t
    token_index  : uint32_t
    tok          : Token*
    union {
        label  { name, len, exits }                                — for P1K_LABEL, P1K_GOTO (exits: pre-computed scope exits for P1K_GOTO)
        decl   { has_init, is_vla, has_raw, is_static_storage,
                 body_close_idx }                                  — for P1K_DECL (body_close_idx: braceless body end position, 0 = not braceless)
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
| `Token.ann` | Inline in token pool | `tokenizer_teardown(true)` (frees token pool) |
| `scope_tree[]` | `ARENA_ENSURE_CAP` | `arena_reset()` |
| `func_meta[]` | `ARENA_ENSURE_CAP` | `arena_reset()` |
| `p1_entries[]` | `ARENA_ENSURE_CAP` | `arena_reset()` |
| `p1_shadow_entries[]` | `ARENA_ENSURE_CAP` | `arena_reset()` |
| `p1_shadow_map.buckets` | `arena_alloc` | `arena_reset()` |
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

Writes `P1_SCOPE_LOOP`, `P1_SCOPE_SWITCH`, `P1_SCOPE_INIT` flags to the `{` token's `ann` field.

---

### 3.2 Phase 1B — Full-Depth Type Registration

**Executed inside:** `p1_full_depth_prescan`

Walks all tokens at all depths. Registers every `typedef`, `enum` constant, and VLA tag into the typedef table using `typedef_add_entry`.

Each `TypedefEntry` records:

- `name`, `len` — identifier text
- `scope_depth` — numeric depth
- `scope_open_idx`, `scope_close_idx` — token index range of the enclosing scope (set from `td_scope_open` / `td_scope_close` thread-locals, which are updated as Phase 1 enters/exits scopes)
- `token_index` — pool index of the declaration
- `is_vla`, `is_void`, `is_const`, `is_volatile`, `is_ptr`, `is_array`, `is_aggregate`, `has_volatile_member` — type property flags
- `is_shadow`, `is_enum_const`, `is_vla_var`, `is_struct_tag` — entry kind flags. `is_struct_tag` entries (`TDK_STRUCT_TAG`) are unconditionally registered for all struct/union tags with bodies — including clean structs with no VLA or volatile members. This ensures inner-scope struct redefinitions correctly shadow outer tags (C11 §6.2.1p4), preventing `tag_lookup` from falling through to stale outer-scope VLA/volatile metadata and causing false CFG verifier errors. Registration occurs at two sites: Phase 1A's `TT_SUE` handler (bare struct defs) and `parse_typedef_declaration` (typedef-wrapped struct defs). Clean entries have `is_vla=false, has_volatile_member=false`. They do NOT receive `P1_IS_TYPEDEF` annotation, preserving `struct Foo` parsing (the tag is not an ordinary typedef name per ISO C11 §6.2.3). Tag name extraction in Phase 1D uses `skip_noise()` to skip GNU `__attribute__` and C23 `[[...]]` attributes between the struct/union keyword and the tag name, and skips `TT_QUALIFIER` tokens
- `is_func` — set when the typedef resolves to a function type (used to suppress zero-init memset on function types). Detection works for both `typedef int FuncType(int)` (parse_declarator returns `end=NULL`, check for `(` after name) and `typedef int (FuncType)(int)` (parse_declarator sets `is_func_ptr=true` without `paren_pointer` — function type, not function pointer). **Chained typedef propagation:** `parse_typedef_declaration` propagates `is_func` through typedef chains via `base_is_func`: when the base type specifier contains a function typedef (`is_func_typedef`), the derived typedef inherits `is_func = true` (guarded by `!decl.is_pointer && !decl.is_array && !decl.is_func_ptr`). Without this, `typedef func_t alias; alias f;` would lose the function-type property and Prism would emit `= {0}` or `memset` on a function symbol — a fatal constraint violation (ISO C11 §6.5.3.4p1 forbids `sizeof` on function types).
- `is_param` — set for function parameter shadow entries (registered by `p1_register_param_shadows`). Used by `array_size_is_vla` to distinguish VLA parameters (which decay to pointers, making `sizeof(param)` constant) from VLA locals.
- `prev_index` — chain to previous entry for the same name

**After Phase 1 completes, the typedef table is immutable.** No `typedef_add_entry` calls occur in Pass 2.

#### Typedef lookup (range-based scoping)

`typedef_lookup(tok)` walks the chain for a name and returns the first **non-struct-tag** entry in scope. When no ordinary entry matches, it falls back to the first matching struct tag entry. This preference ordering enforces ISO C11 §6.2.3 namespace separation: when both `typedef int Foo;` and `struct Foo { ... };` coexist at the same scope, `typedef_lookup` returns the typedef entry (with `TDF_TYPEDEF`), not the struct tag. When only a struct tag exists (e.g. `Inner` inside `struct Outer { struct Inner hw; }`), the struct tag is returned as fallback, preserving `TDF_HAS_VOL_MEMBER` and `TDF_VLA` access via `typedef_flags`.

The duplicate-detection guard in `typedef_add_entry` compares entry kind (`prev->is_struct_tag == (kind == TDK_STRUCT_TAG)`) in addition to scope depth and range — a `TDK_TYPEDEF` and `TDK_STRUCT_TAG` with the same name at the same scope are NOT duplicates (separate C11 namespaces).

Scope check for each candidate entry:

```
entry.token_index <= tok_idx(tok) AND
tok_idx(tok) >= entry.scope_open_idx AND
tok_idx(tok) < entry.scope_close_idx
```

This range-based check eliminates runtime scope unwinding. There is no `active_shadow_idx` cache — the range check is sufficient.

#### tag_lookup (C namespace isolation)

`tag_lookup(tok)` walks the same `prev_index` hash chain as `typedef_lookup` but filters for `e->is_struct_tag` entries only. This enforces ISO C11 §6.2.3 namespace separation: struct/union/enum tags exist in a different namespace from ordinary identifiers. When `parse_type_specifier` encounters `struct Foo`, it calls `tag_lookup(sue_tag)` instead of `typedef_flags(sue_tag)` to read `is_vla` and `has_volatile_member` directly from the tag entry. This prevents an ordinary variable named `Foo` (which creates a `TDK_SHADOW` in the unified hash chain) from hiding the struct tag's VLA/volatile properties. The `typeof(struct Tag)` handler in `parse_type_specifier` also uses `tag_lookup` (via `saw_sue` tracking) to resolve struct tag volatile/VLA info when the tag name also exists as an ordinary typedef.

#### typedef_flags query

A single lookup returns a bitmask: `TDF_TYPEDEF`, `TDF_VLA`, `TDF_VOID`, `TDF_ENUM_CONST`, `TDF_CONST`, `TDF_PTR`, `TDF_ARRAY`, `TDF_AGGREGATE`, `TDF_VOLATILE`, `TDF_FUNC`, `TDF_PARAM`, `TDF_HAS_VOL_MEMBER`. Convenience macros (`is_known_typedef`, `is_vla_typedef`, `is_volatile_typedef`, `has_volatile_member_typedef`, etc.) test individual bits.

---

### 3.3 Phase 1C — Shadow Registration

**Executed inside:** `p1_full_depth_prescan`

For every variable declaration at every depth, if the declared name collides with a typedef, a `P1ShadowEntry` is recorded via `p1_register_shadow`. Shadows also create a `TDK_SHADOW` entry in the typedef table itself (with `is_shadow = true`, `scope_open_idx`, `scope_close_idx`), so that `typedef_lookup` returns the shadow entry when the token is within the shadow's scope range. Shadow registration triggers when the declared name matches any of: `TT_DEFER`, `TT_ORELSE` (keyword shadowing), `TT_NORETURN_FN`, `TT_SPECIAL_FN` (noreturn/setjmp function name shadowing), or an existing typedef/enum-constant/function-prototype entry in the typedef table (type shadowing).

**Temporal ordering:** Shadows are token-order-dependent. A variable named `T` declared at token index 500 only shadows the typedef `T` for lookups at index ≥ 500 within the shadow's scope range.

**Function parameter scope:** When Phase 1 encounters a function body `{` (is_func_body), parameter declarations from the preceding `(…)` are registered as shadows scoped to the function body. For forward declarations (prototypes ending with `;`), parameter shadows are registered scoped to the prototype's parameter list range. **Unnamed function pointer parameter safety:** `p1_register_param_shadows` only scans the first nested `(` group per parameter for declarator names (e.g., `(*cb)` in `int (*cb)(int a, int b)`). After scanning one `(` group, subsequent `(` groups (which contain inner parameter type lists) are skipped. This prevents phantom shadow registration from inner parameter names of unnamed function pointer parameters (e.g., `int (*)(int a, int b)` must not shadow `b` for the outer function scope).

**Attribute skipping during parameter list discovery:** The backward walk from `{` (or `;` for prototypes) to find the parameter list `(…)` skips GNU `__attribute__((...))` (detected via `TT_ATTR` tag on the preceding keyword) and C23 `[[...]]` attributes (detected via `TF_C23_ATTR` flag on the matching `[`). This ensures `void f(int T) __attribute__((noinline)) {` and `void f(int T) [[gnu::cold]] {` correctly identify the parameter list.

**For-init scope:** Variables declared in `for`-init (e.g., `for (int T = 0; …)`) are shadowed for the entire loop body (through the closing `}` or braceless statement end), not just through the `)`. This matches C99 §6.8.5p3: the for-init declaration's scope extends to the end of the loop body.

**Braceless body detection:** The for-init body end is determined by `skip_one_stmt()`, an iterative helper that correctly handles all C statement forms: braced `{…}`, `if (…) stmt [else stmt]`, `for`/`while`/`switch (…) stmt`, `do stmt while (…);`, `case EXPR:` / `default:` labels, and simple `expr;`. The `case` label scanner tracks ternary depth (`?` increments, `:` decrements when depth > 0) to distinguish ternary `:` from label `:` in expressions like `case 1 ? 2 : 3:`. Without this, a ternary `:` is misidentified as the label terminator, causing the scanner to overshoot and corrupt shadow scope boundaries. This prevents if-else branches from prematurely terminating the shadow scope (e.g., `for (int T=0;…) if(c) x=T; else T*x;` — the else branch is within T's shadow scope). **Cache correctness:** The O(1) amortization cache (`skip_cache`) stores trail entries during traversal and backfills them when a statement end is reached. When unwinding an `if`-body and entering the `else` branch, the trail must be flushed (cached with the true-branch's end value) and reset before processing the else. Without this, tokens inside the true-branch are poisoned with the else-branch's end value, causing subsequent cache queries on those tokens to return the wrong statement boundary, producing false-positive CFG errors.

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

**Attribute control-flow rejection:** After `skip_noise()` advances past GNU `__attribute__((...))`, C23 `[[...]]`, or `_Pragma(...)` blocks, the prescan scans the skipped token range for control-flow keywords (`goto`, `return`, `break`, `continue`, `defer`) and `orelse`. If found, a hard error is emitted. Without this, a `goto` inside a statement expression in an attribute argument (e.g., `__attribute__((aligned( ({ goto L; 8; }) )))`) would be invisible to the CFG verifier — no `P1K_GOTO` entry would be created, and the jump-over-VLA safety check would not fire. Pass 2's `emit_range_ex` correctly routes `({...})` through `walk_balanced` → `emit_statements(EMIT_NORMAL)`, which handles defer cleanup at the goto site; however, Phase 1D's blindness would leave the static CFG graph incomplete. The `defer` and `orelse` checks use `typedef_lookup` to avoid rejecting variables named `defer` or `orelse`. The `orelse` check is also performed independently in the `!at_stmt_start` handler for mid-declarator and trailing attributes — when a balanced attribute group (GNU `__attribute__((...))` or C23 `[[...]]`) is encountered, the prescan scans its interior for `TT_ORELSE` tokens (with the same `typedef_lookup` shadow guard).

**For-init / if-switch-init declarations:** `p1_scan_init_shadows` also allocates `P1K_DECL` entries for variables declared in C99 for-init clauses (`for (int i = 0; ...)`) and C23 if/switch initializers (`if (int x = f(); x > 0)`). The scope_id assigned to these entries is the **body scope** (the `{` following the `)` of the control statement), not the enclosing scope. This ensures the CFG verifier catches gotos that jump *into* the loop/if/switch body past the init declaration, while still allowing gotos that jump *over* the entire control statement (where the variable is no longer in scope). For braceless bodies (no `{`), the entry uses the enclosing scope's `scope_id` (cur_sid) with a `body_close_idx` field set to the braceless body's end position (from `skip_one_stmt`). The CFG verifier uses `body_close_idx` when non-zero to correctly bound the variable's lifetime, allowing gotos that jump *over* the entire statement while catching gotos that jump *into* the braceless body. Consecutive `raw` keywords (e.g. from macro expansion like `raw raw int arr[n]`) are handled by a `while` loop, matching the pattern in `try_zero_init_decl`. **For-init typedef routing:** When the first token after `(` (and any `raw` prefix) has the `TT_TYPEDEF` tag, `p1_scan_init_shadows` delegates to `parse_typedef_declaration` with `td_scope_open`/`td_scope_close` set to the enclosing control statement's scope bounds. Enum constants within the typedef body are registered via `parse_enum_constants`. Without this routing, `typedef` in for-init/if-init is silently dropped — the guard condition only checks `TT_TYPE|TT_QUALIFIER|TT_SUE|TT_TYPEOF|TT_BITINT` and `is_known_typedef()`, none of which match the `TT_TYPEDEF` keyword tag. The consequence is type system collapse: variables declared using the for-init typedef are not recognized as typed declarations, bypassing both zero-initialization and the CFG verifier's goto-over-VLA safety checks.

**Braceless-body declarations:** Declarations that constitute the body of a braceless control statement (e.g., `if (1) L: int x;` using C23 labeled declarations) receive `body_close_idx` from `skip_one_stmt` on the type token. Phase 1D detects this via `p1d_ctrl_pending` — when the flag is true at declaration time (labels don't reset it), the declaration is bounded to the braceless body's semicolon. Case/default labels DO reset `p1d_ctrl_pending` via `P1D_STMT_RESET()` — they are label prefixes within a switch body, not braceless control-flow constructs. Without this reset, `switch(0) case 0: int x;` would erroneously set `body_close_idx` on `x`, artificially shrinking its lifetime in the CFG verifier and allowing gotos to bypass the declaration. Without this, the declaration would inherit the enclosing function scope's lifetime, causing the CFG verifier to falsely reject forward gotos past already-dead declarations. Note: backward gotos to a label *before* a VLA declarator are legal per C11 §6.2.1p7 (scope begins after declarator completion), so the label is outside the VLA's scope and no checking is needed for the backward direction.

**Braceless-body typedef scope narrowing:** When a `typedef` is encountered inside a braceless control-flow body (`p1d_ctrl_pending` is true), Phase 1D temporarily narrows `td_scope_close` to the braceless body's statement boundary (computed by `skip_one_stmt_impl`) before calling `parse_typedef_declaration`. This prevents the typedef from polluting the enclosing scope — without it, `if (1) typedef float T;` would register `T` with the parent block's `td_scope_close`, masking any keyword or type named `T` for the remainder of the block. The original `td_scope_close` is restored immediately after registration.

**Braceless-body variable shadow/VLA scope narrowing:** `p1d_probe_declaration` applies the same `TD_SCOPE_SAVE()` / `td_scope_close = braceless_close_idx` / `TD_SCOPE_RESTORE()` pattern when `p1d_ctrl_pending` is true. This narrows both `TDK_SHADOW` entries (via `p1_register_shadow`) and `TDK_VLA_VAR` entries (via `typedef_add_vla_var`) to the braceless statement boundary. Without this, a variable declared in a braceless body that shadows a typedef (e.g., `if (c) int MyTypedef;`) would poison the typedef name for the remainder of the enclosing function — `is_known_typedef` would return false, causing the CFG verifier to miss VLA declarations and the zero-init engine to skip initialization. Enum constants (`TDK_ENUM_CONST`) and struct tags (`TDK_STRUCT_TAG`) are deliberately NOT narrowed — per C11 §6.2.1p4, their scope extends to the end of the enclosing block, not the braceless statement.

**VLA tracking:** `is_vla` on `P1FuncEntry.decl` is set when either the base type is a VLA typedef (`type.is_vla`) or the declaration itself has variable-length array dimensions (`decl.is_vla`). This covers both `typedef int T[n]; T x;` and direct `int x[n];` forms. Jumping past a VLA is always dangerous regardless of `has_init` or `has_raw`, because it bypasses implicit stack allocation.

**typeof VLA detection:** Inside `parse_type_specifier`, VLA array dimensions inside `typeof(...)` are detected by scanning for `[` preceded by a valid type-context predecessor, followed by a runtime-variable size (`array_size_is_vla`). The predecessor check is consolidated in the `is_array_bracket_predecessor` helper, which accepts: type keywords, known typedefs, type qualifiers (`TT_QUALIFIER` — covers `const`, `volatile`, `restrict`, `_Atomic`), `]` (multi-dim), `*` (pointer), or `)` (paren-pointer grouping). Parenthesized types like `typeof((int[n]))` are detected at all paren depths. However, function-pointer parameter lists are skipped: a `(` preceded by `)` signals a function parameter list (e.g., `typeof(void(*)(int[n]))`), and `[n]` inside such lists describes parameter dimensions, not the outer type. This prevents false VLA flags on function pointers — important because `register` storage class variables cannot have their address taken, making memset-based zero-init illegal for them. The same `is_array_bracket_predecessor` check is used in the `_Atomic(...)` VLA scanning loop and in the `sizeof(...)` branch of `array_size_is_vla` (which detects VLA types inside sizeof expressions like `sizeof(int const [n])`). **sizeof VLA param dereference detection:** VLA function parameters (marked `TDF_PARAM`) decay to pointers, so `sizeof(param)` is always `sizeof(pointer)` — constant. The verifier only flags a VLA param inside `sizeof` as producing a VLA-sized type when the param is dereferenced or used in an expression that accesses VLA dimensions. The adjacency check examines both the preceding and following tokens for: `*` (explicit dereference), `[` (subscript — including the commutative form `1[param]` where `[` is the *preceding* token), `+`/`-` (pointer arithmetic — catches `*(1 + param)`, `*(param - 0)` etc.), and `*` as a following token (pointer-scaled multiplication). This ensures that `sizeof(param)` alone correctly evaluates to pointer size (constant) while `sizeof(param[i])`, `sizeof(1[param])`, `sizeof(*(1 + param))`, and all equivalent ISO C expressions that derive a VLA-sized type are correctly flagged. False positives (e.g., flagging `sizeof(&param[0])` which produces a constant pointer size) only result in stricter goto checks; false negatives would cause stack corruption.

**has_raw:** Declarations marked `raw` set `has_raw = true`. In multi-declarator statements (`raw int x, y;`), `p1d_saw_raw` persists across commas — **all** declarators receive `has_raw = true` (matching Pass 2's behavior, where `is_raw` is also never reset on commas). The CFG verifier skips `has_raw` declarations for goto checks, **except VLAs** — `raw` on a VLA does not exempt it.

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

**K&R function fallback:** K&R-style parameter declarations (e.g., `void f(a) int a; {`) reset `file_scope_stmt_start` at each `;`. If the initial `capture_function_return_type(file_scope_stmt_start)` fails (returns 0), Phase 1E walks backward from `{` past K&R parameter declarations (`;`-separated) to find the actual parameter list `)`, then backward from that `(` to find the real declaration start, and retries capture.

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
- Recurses into `orelse { … }` blocks — control-flow keywords inside orelse action blocks in defer bodies are validated

**Paren-aware orelse scanning:** The orelse keyword scanner inside `validate_defer_statement` recurses into balanced `(…)` and `[…]` groups via `defer_scan_orelse_in_group` instead of blindly skipping them with `tok_match`. This is critical because Pass 2's `scan_decl_orelse` strips outer parentheses from initializers (macro-hygiene pattern), making `defer { int x = (0 orelse return); }` reachable at runtime. Without recursion, the `return` inside parens was invisible to Phase 1F. `{` groups are still skipped (statement expressions are handled separately by `defer_scan_hidden_stmt_exprs`).

**Chain-aware scanning:** Both `defer_scan_orelse_in_group` and the inline orelse scanner in `validate_defer_statement` scan ALL orelse instances in their range — they do not stop after finding the first orelse. This is critical because chained orelse like `(1 orelse 0 orelse return)` or bare `int x = f() orelse 0 orelse goto err;` in defer bodies hides the dangerous control-flow keyword behind a benign intermediate orelse. Each scanner uses `Token *prev` tracking with `orelse_shadow_is_kw(prev)` positional disambiguation to avoid false positives on variables named `orelse`. After validating each orelse's action (via `validate_defer_control_flow` for keywords, or recursive `validate_defer_statement` for block-form `orelse { … }`), the scanner advances past the action tokens and continues scanning for additional orelse instances.

**Control-flow condition head scanning:** `skip_defer_control_head` (used to advance past the `(…)` condition of `if`/`while`/`for`/`switch`/`do-while`) calls `defer_scan_hidden_stmt_exprs` on the condition parens before skipping past them. This ensures that statement expressions inside control-flow conditions (e.g., `if ( ({ return 1; 1; }) )`) are scanned for forbidden control-flow keywords. Without this, `return`/`goto`/`break`/`continue` hidden inside a statement expression in a condition head would bypass Phase 1F validation entirely.

**Case/default label scanning:** When `validate_defer_statement` encounters a `case` or `default` label, it scans forward to the terminating `:` using ternary depth tracking (`?` increments, `:` at `td > 0` decrements, `:` at `td == 0` terminates). This correctly handles `case 1 ? 2 : 3:` — matching the pattern used by Phase 1D and `skip_one_stmt`. The scan also stops at bare `}` tokens (`TF_CLOSE && ch0 == '}'`) — same boundary guard as the orelse and statement-expression scan loops — preventing overshoot into the outer scope on malformed case labels with missing `:`.

**`skip_to_semicolon` boundary hardening:** `skip_to_semicolon(tok, end)` accepts an optional `end` boundary pointer and stops at bare `}` tokens (`TF_CLOSE && ch0 == '}'`) in addition to `;` and `EOF`. Without the `}` stop, a missing semicolon inside `defer { int x = 5 }` caused the scanner to walk past the closing `}` into the outer scope, making `validate_defer_statement`'s `tok != end` exact pointer test silently overshoot — any `return` in the enclosing function was then falsely flagged as "return inside defer block". Three defense layers: (1) `skip_to_semicolon` stops at bare `}`, (2) `validate_defer_statement`'s block loop uses `tok_idx(tok) < end_idx` as overshoot guard, (3) the orelse and statement-expression scan loops inside `validate_defer_statement` also check for `}` boundary escape. All callers outside defer validation pass `NULL` for `end` (no boundary).

**Forbidden function contexts:** Defer in functions that use `setjmp`/`longjmp`, `vfork`, or `asm goto` (computed labels inside assembly) is rejected via `TT_SPECIAL_FN`/`TT_NORETURN_FN`/`TT_ASM` tag bits on the function body's opening `{` token (checked as `func_meta[idx].body_open->tag`). The `TT_SPECIAL_FN` taint covers all standard and implementation-internal variants that survive preprocessing: `setjmp`, `_setjmp`, `__setjmp`, `__sigsetjmp`, `longjmp`, `_longjmp`, `__longjmp`, `__siglongjmp`, `__longjmp_chk`, `sigsetjmp`, `siglongjmp`, `__builtin_setjmp`, `__builtin_longjmp`, `__builtin_setjmp_receive`, `pthread_exit`, `savectx`. Detection is token-name-based: even bare references like `void (*fp)(jmp_buf, int) = longjmp;` taint the function because the `longjmp` token retains its `TT_SPECIAL_FN` tag. The only undetectable bypass is a function pointer passed from another translation unit (cross-TU indirect call), which is inherent to single-TU static analysis.

---

### 3.7 Phase 1G — Orelse Pre-Classification

**Function:** inline in `p1_full_depth_prescan`

Walks all tokens looking for `[…]` pairs containing `orelse`. Marks them with `P1_OE_BRACKET` on the token's `ann` field.

**Control-flow rejection (Phase 1G):** When an `orelse` token is found inside `[…]`, Phase 1G peeks at the next token. If it is a control-flow keyword (`return`, `break`, `continue`, `goto`) or `{` (block form), a hard error is raised immediately — before Pass 2 emits any output. Pass 2's `walk_balanced_orelse` retains the same checks as defense-in-depth assertions (unreachable by design).

**Chain-aware side-effect rejection (Phase 1G + Phase 1D):** When two or more `orelse` tokens appear at depth 0 inside the same `[…]` (chained bracket orelse), Phase 1G checks the tokens between consecutive depth-0 orelses for side effects via `reject_orelse_side_effects`. Similarly, when chained orelse appears inside `typeof(…)`, Phase 1D's typeof handler scans ALL orelse instances (not just the first), validating each intermediate LHS range via `reject_orelse_side_effects(se_start, orelse)` where `se_start` advances past each orelse found. This prevents the ternary duplication in `emit_token_range_orelse` from evaluating function calls, `++`/`--`, assignments, control-flow keywords (`goto`/`return`/`break`/`continue`/`defer`), member access (`->`, `.`), or array subscripts (`[]`) twice (the intermediate LHS is emitted as `(LHS) ? (LHS) : (RHS)`). Pass 2's `emit_token_range_orelse` retains the same check as defense-in-depth for chains at nested depths and inside `typeof` contexts.

**File-scope guard:** If `P1_OE_BRACKET` occurs outside any function body (`p1d_cur_func < 0`), a hard error is raised. Bracket orelse requires hoisting a temp variable statement, which is illegal outside a function body (e.g., `void process(int buf[1 orelse 2]);` at file scope).

**Declaration-init classification:** `P1_OE_DECL_INIT` is set during initializer scanning in Phase 1C+1D when a declaration's initializer contains `orelse`.

**Invalid context rejection (Phase 1):** Orelse inside `struct`/`union` bodies, inside `enum` bodies (enum constants must be compile-time integer constants), inside `_BitInt(N)` or `_Alignas(N)` (compile-time constant expression contexts), inside `typeof`, inside certain invalid contexts is caught and errored. The `_BitInt`/`_Alignas` check fires inside `parse_type_specifier` (called from Phase 1D prescan); `emit_type_stripped` retains a defense-in-depth route through `walk_balanced_orelse` for these keywords.

**Bare-expression orelse** (`OE_BARE_ASSIGN`, `OE_BARE_ACTION`, `OE_BARE_COMPOUND`) is classified at emit time in Pass 2. This is safe because the symbol table is immutable.

---

## 4. Phase 2A — CFG Verification

**Function:** `p1_verify_cfg`

Runs after all Phase 1 sub-phases complete. Gated by `F_DEFER | F_ZEROINIT` — when neither feature is enabled, only functions containing VLA declarations are verified (all others are skipped). For each function's `P1FuncEntry[]` array, performs an O(N) linear sweep with label hash table and monotonic watermark arrays. The label hash table is allocated before `arena_mark` so it persists in `FuncMeta` for O(1) label lookup in Pass 2; only the watermark arrays and forward-goto list are reclaimed via `arena_restore`.

### Algorithm

1. Build a label hash table (open-addressing, power-of-2) mapping label names to their entry index. During insertion, duplicate labels (same name appearing twice in the same function) are detected and rejected with a hard error.

2. Maintain watermark arrays `wm_defer[]` and `wm_decl[]` indexed by entry array position. As the sweep encounters `P1K_DEFER` and `P1K_DECL` entries, it appends them to `defer_list`/`decl_list` and records the current list lengths in the watermark arrays. Separate switch watermark arrays (`sw_defer_wm[]`, `sw_decl_wm[]`) are indexed by `scope_id` for O(1) lookup from case entries.

3. **Forward goto:** When a `P1K_GOTO` is encountered and its target label has not yet been seen, add it to a forward-goto hash table (separate-chaining, keyed by label name). When the target `P1K_LABEL` is reached, resolve by looking up the hash chain for O(1) amortized resolution, then checking all `P1K_DEFER` and `P1K_DECL` entries between the goto and label positions. If any dangerous entry's scope is an ancestor-or-self of the label's scope, error. Scope exits are pre-computed via `scope_block_exits` (O(depth) LCA tree walk) and stored in `P1FuncEntry.label.exits` for O(1) retrieval in Pass 2.

4. **Backward goto:** When a `P1K_GOTO` targets a label already seen, check entries in `[0, wm_defer[label_index])` / `[0, wm_decl[label_index])` range. The scope-close check uses `label->token_index` (not the goto's token_index) because the goto jumps into the scope containing the label.

5. **Switch/case:** On `P1K_SWITCH`, snapshot the current watermarks. On `P1K_CASE`, compare current state against the switch's snapshot. Defers in ancestor-or-self scopes that were not active at the switch entry → error (defer skipped by fallthrough). Declarations in nested blocks (regardless of whether user-initialized or Prism-initialized) → error (case bypasses initialization), unless the declaration has `raw` or static storage duration.

6. **Statement-expression boundary:** Gotos into a GNU statement expression are rejected. When resolving a forward or backward goto, if the target label is inside a `is_stmt_expr` scope and the goto is not within that same statement expression (checked via `scope_stmt_expr_ancestor`), a hard error is raised. Gotos *out of* a statement expression are allowed (GCC/Clang support this and Prism's defer+goto idioms rely on it).

7. **Computed goto / asm goto:** If `has_computed_goto` is set or the function body's opening `{` has `TT_ASM` (asm goto taint), and the function has `P1K_DEFER` entries (when `F_DEFER`) or VLA `P1K_DECL` entries or non-`raw`, non-static-storage `P1K_DECL` entries (when `F_ZEROINIT`), hard error — the jump target cannot be verified at compile time. `asm goto` labels are inside the assembly string and cannot be extracted by the token walker, making CFG verification impossible.

8. **Unresolvable return type:** If the function has `P1K_DEFER` entries and does not return `void` but has no captured return type (`ret_type_start == NULL`), hard error. Anonymous struct return types (e.g., `struct { int x; } f() { defer ...; }`) cannot be spelled in a `typedef` for the defer cleanup temp variable. The user must use a named struct or typedef.

### Checked violations

| Violation | Severity |
|---|---|
| Forward goto skips over `defer` | Error (or warning with `-fno-safety`) |
| Forward goto skips over variable declaration | Error (or warning with `-fno-safety`) |
| Forward goto skips over VLA declaration | **Always error** (VLA skip is UB regardless of `-fno-safety`) |
| Backward goto enters scope containing `defer` | Error (or warning with `-fno-safety`) |
| Backward goto enters scope containing variable declaration | Error (or warning) |
| Backward goto enters scope containing VLA declaration | **Always error** (VLA skip is UB regardless of `-fno-safety`) |
| Backward goto loops over `defer` | **Always error** |
| Switch/case skips `defer` via fallthrough | Error (or warning) |
| Switch/case bypasses variable declaration in nested block | Error (or warning) |

**Backward goto loop-over-defer detection:** After checking backward goto entries-into-scope, the verifier detects backward gotos that loop over `defer` statements. A defer between the label and the goto (token-position-wise) is looped over if the goto does not exit the defer's scope — i.e. the defer's scope is an ancestor-or-self of BOTH the label's scope and the goto's scope. The check uses `scope_is_ancestor_or_self(d->scope_id, ents[li].scope_id)` and `scope_is_ancestor_or_self(d->scope_id, g->scope_id)` to cover all nesting combinations: label in nested block (`{ L: ; } defer f(); goto L;`), goto in nested block (`L: ; defer f(); { goto L; }`), both nested, and same-scope. Defers in child or sibling scopes of the goto are correctly allowed because the goto exits or never enters those scopes, and `emit_goto_defer` fires their cleanup.

Forward gotos past `raw`-marked declarations are skipped (safe — user explicitly opted out of initialization safety), **except VLAs** — `raw` on a VLA does not exempt it because jumping past a VLA bypasses implicit stack allocation regardless of initialization. Declarations with static storage duration (`static`, `extern`, `_Thread_local`, `thread_local`, `__thread`) are also exempt — their initialization occurs at program/thread startup, not at the declaration point, so jumping past them does not leave the variable indeterminate.

### Complexity

O(N) per function where N is the number of `P1FuncEntry` items, plus O(depth) for `scope_is_ancestor_or_self` checks.

**Arena management:** Per-function temporary allocations (watermark arrays, forward-goto list) are reclaimed after each function via `arena_mark`/`arena_restore`, preventing O(total_entries) memory accumulation across all functions in a translation unit. The label hash table is allocated **before** `arena_mark` and persists in `FuncMeta.label_hash` for O(1) label lookup in Pass 2.

---

## 5. Pass 2 — Code Generation

A sequential token walk. Emits transformed C.

### Fast path

Tokens with no `tag` bits set and `!at_stmt_start` (~70–80% of tokens) are emitted directly via `emit_tok`. This path never reads `tok->ann`.

### Slow path dispatch

Tokens with tag bits or at statement boundaries are dispatched to handlers:

| Handler | Trigger | Action |
|---|---|---|
| `handle_goto_keyword` | `TT_GOTO` | Emit defer cleanup (LIFO unwinding to target label depth), emit `goto`. Safety checks are in Phase 2A. |
| `handle_open_brace` | `{` | Push scope. Read `P1_SCOPE_*` from `tok->ann` for classification. Handle compound-literal-in-ctrl-paren and in braceless body (`P1_SCOPE_INIT` bypass: no scope push, increment `ctrl_state.brace_depth` instead), stmt_expr detection. |
| `handle_close_brace` | `}` | Pop scopes, emit defers (LIFO), restore ctrl_state for stmt-expr inside ctrl parens. Compound-literal bypass: when `ctrl_state.pending && brace_depth > 0`, decrement `brace_depth` (matches open-brace bypass symmetrically — no scope to pop). |
| `try_zero_init_decl` | Statement start, type keyword/typedef | Parse declaration, insert `= {0}` or `= 0` or `memset` call. |
| `p1_label_find` | `TT_GOTO` dispatch | O(1) label lookup via `FuncMeta.label_hash` (persisted from Phase 2A); falls back to linear scan if no hash. |
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
| `CtrlState` | Braceless control flow tracking: `pending`, `pending_for_paren`, `parens_just_closed`, `brace_depth`. Set for `TT_IF`, `TT_SWITCH`, and `TT_LOOP` keywords — `TT_LOOP` is gated by `FEAT(F_DEFER \| F_ZEROINIT)` (fires when *either* feature is enabled), since braceless-body declarations after `while`/`do` need `brace_wrap` for zero-init. |

### Defer emission

Defers execute in LIFO order at scope exit — whether via `return`, `break`, `continue`, `goto`, or reaching `}`.

For `goto`: the handler looks up the target label's scope depth, then emits defers from the current scope level down to the target's level.

Scope exits between a goto and its target label are pre-computed during Phase 2A via `scope_block_exits` (an O(depth) LCA tree walk on the scope tree) and stored in `P1FuncEntry.label.exits`. Pass 2 retrieves these via `p1_goto_exits` using a monotonic cursor through the goto entries (O(1) amortized).

For `return`: emits all defers from the current scope to function scope. Uses `ret_counter` to generate unique labels for cleanup blocks.

`emit_deferred_range` handles defer bodies by delegating to `emit_statements(start, end, EMIT_DEFER_BODY)`, which provides bare orelse, raw stripping, zero-init, control-flow keyword tracking, and SUE body handling within deferred code. The colon (`:`) label handler in `emit_statements` fires in both `EMIT_NORMAL` and `EMIT_DEFER_BODY` modes — it resets `at_stmt_start = true` after case/default labels so that subsequent orelse and zero-init processing is correctly triggered. The handler uses `pending_case_colon` to support complex case label expressions like `case (2+3):`, `case (FOO):`, or `case (int){42}:` where `last_emitted` before the `:` is `)` rather than an identifier or number — without this flag, the `)` would not match the `is_identifier_like || TK_NUM` check and `at_stmt_start` would not be reset, causing subsequent declarations to miss zero-initialization. The flag is set when `TT_CASE | TT_DEFAULT` is encountered and consumed (or cleared) at the `:` handler. Both `emit_statements` and `transpile_tokens` maintain independent `pending_case_colon` flags. Phase 1F bans user labels (`ident:`) in defer bodies (hard error: "labels inside defer blocks produce duplicate labels"), so only case/default labels reach this handler in defer mode.

`emit_deferred_orelse` (called from `try_process_stmt_token` inside `walk_balanced`'s stmt-expr processing and `emit_statements`) delegates action emission to `emit_orelse_action`, which routes blocks through `emit_orelse_block_body` (→ `handle_open_brace` → `emit_statements(EMIT_NORMAL)` → `handle_close_brace`, the full transpilation engine) and control-flow keywords through `emit_return_body` / `emit_break_continue_defer` / `emit_goto_defer`. This ensures that return/goto/break/continue inside orelse actions that appear in nested contexts (e.g., orelse-with-return inside another orelse block body) correctly unwind all applicable defers, and that defer keywords inside orelse action blocks are fully processed rather than leaked to the backend compiler.

### Defer-variable shadow checking

`check_defer_var_shadow` detects when a newly-declared variable name appears in an active defer body — this would silently capture the wrong variable at cleanup time. Uses a two-tier HashMap architecture for O(1) amortized lookups:

1. **`FuncMeta.defer_name_set`** — union of all captured names across all defer bodies in the function. Used for an O(1) fast-reject: if the name isn't captured by ANY defer, skip immediately.
2. **`FuncMeta.defer_body_captures`** — per-defer-body capture sets. Maps `tok_idx(body)` → `HashMap*` where each inner HashMap contains the exact set of names captured by that specific defer body. Used for O(1) per-body lookups via `defer_body_has_capture(func_idx, body, name, nlen)`.

Both HashMaps are populated during Phase 1D by `defer_body_populate_captures(body, body_end, out, body_captures_map)`, a single-pass O(M) scanner that computes the exact set of captured (free) variables for each defer body. A captured variable is one referenced in the defer body but not locally declared within it at the reference's scope depth or deeper. The scanner maintains per-name state in a local HashMap (`name → decl_depth`), a scope-exit stack for resetting local declarations when `}` closes a brace depth, and for-init scope tracking for C99 `for(TYPE name = ...)` declarations. Block-level context detection uses a per-block base paren depth array (`block_base_pd[bd]`): each `{` records the current paren depth as its block's baseline, and declarations are recognized only when `pd == block_base_pd[bd]` — the paren depth matches the block's entry depth. This correctly handles GNU statement expressions nested inside arbitrary parenthesized expressions (e.g., `(1 + ({ int x = 1; x; }))`) where the stmt-expr's block opens at `pd > 0`. For single-statement defers (`defer f();`), `body` points directly to the first statement token (no `{`), so `bd` stays 0 and the local-declaration logic correctly doesn't fire — single-statement defers have no block scope for local declarations.

This architecture replaced the previous O(N×M) design where `defer_name_set` contained a superset (all identifiers, not just captures) and a per-name O(M) linear scanner `defer_body_refs_name` was called for each variable that passed the superset filter. With generated code (N = thousands of variables sharing names with M-token defer bodies), the old design produced quadratic slowdown. The new design is O(N + Σ M_i) total: one O(M) pass per defer body during Phase 1D, then O(1) per variable per defer body during shadow checking.

The scan covers two ranges: enclosing-scope defers `[0, blk->defer_start_idx)` and same-block defers `[blk->defer_start_idx, defer_count)`. Enclosing-scope matches register a `DeferShadow` entry for deferred checking at control-flow exits (`check_defer_shadow_at_exit`). Same-block matches produce an immediate error — the shadowing variable is unconditionally live when the defer fires at block end, so no control-flow analysis is needed. For-init declarations are exempt from the same-block immediate error (they use deferred `DeferShadow` checking instead) because their implicit scope ends at the loop boundary. A token-index range guard (`var_idx ∈ [defer.stmt, defer.end)`) prevents false positives from declarations inside defer bodies matching against their own body (e.g., `defer { char buf[16]; ... }`).

`check_enum_typedef_defer_shadow` extends this protection to enum constants and typedef names. Called from the main Pass 2 loop at statement-start for enum definitions (`enum { name = val, ... }`) and typedef declarations (`typedef type name`), which bypass `process_declarators` and would otherwise evade shadow detection. Each introduced name is checked against active defer bodies via `check_defer_var_shadow`.

**Ghost enum in mid-expression contexts:** Enum definitions inside `sizeof(…)`, casts `(enum { … })expr`, `typeof(…)`, and array dimensions `[sizeof(enum { … })]` bypass the statement-start `check_enum_typedef_defer_shadow` call. Detection is split across two passes:

- **Phase 1D (same-block only):** `p1_check_enum_body_defer_shadow` checks enum constants against same-scope defers and produces an immediate hard error — the shadow is unconditionally live when the defer fires at block end. This is called from `p1d_scan_balanced_group` for enums inside balanced `(…)` and `[…]` groups, and from the direct `at_stmt_start` enum-definition handler.

- **Pass 2 (enclosing-scope):** Enclosing-scope enum shadows are deferred to `check_defer_var_shadow`, which queues a `DeferShadow` entry checked only at control-flow exits (`check_defer_shadow_at_exit`). This correctly allows inner-scope enum constants that shadow defer-captured names when no control-flow exit is reachable while the shadow is live. Three Pass 2 hooks cover expression-context enums: (1) `walk_balanced` emits `check_enum_body_defer_shadow` for `is_enum_kw(t)` with a body brace when `defer_count > 0`; (2) `walk_balanced_orelse` has the same hook in its no-orelse flat emit loop for array dimension enums; (3) the main loop's statement-start `check_enum_typedef_defer_shadow` covers top-level enum definitions.

**Control-flow paren keyword detection:** `p1d_scan_balanced_group` also detects `defer` and `orelse` keywords inside control-flow condition parentheses (`if(…)`, `while(…)`, `for(…)`, `switch(…)`). When the preceding saved token has `TT_IF | TT_LOOP | TT_SWITCH`, any `defer` or `orelse` at statement-expression depth zero (`se_depth == 0`) is rejected with a hard error. The `se_depth` tracker (not raw `inner_depth`) is used so that keywords inside GNU statement expressions within the condition (e.g., `if (({ int x = f() orelse 0; x; }))`) are correctly allowed — the stmt-expr provides its own block scope. `se_depth` is maintained via a 64-entry `se_close_stack`: on `({` entry, the matching `}` is pushed and `se_depth` increments; on reaching that `}`, the stack pops and `se_depth` decrements.

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
- Functions using `vfork` — error (any appearance of the `vfork` identifier in the function body, including bare references like `fp = vfork`)
- Functions using `asm goto` — error (regular `asm` is safe)
- `return`, `goto` inside defer body — error
- `break`, `continue` inside defer body — error (defer body is not a loop/switch context)
- Computed goto (`goto *ptr`) with active defers or zero-initialized declarations — error
- Functions with unresolvable return type (anonymous struct return) when using `defer` — error
- `defer` at the top level of a GNU statement expression — error (use a nested block)
- A block containing `defer` as the **last statement** of a GNU statement expression — error. `handle_close_brace` emits defers after the last expression, overwriting the expression's return value (void defers → compile error; non-void defers → silent wrong-value assignment). Without an expression parser there is no safe transformation; the user must place the defer block before the final expression rather than as the last statement.

**Feature flag:** `-fno-defer` disables.

### 6.2 orelse

**Syntax:** `<expr> orelse <action>`

**Semantics:** Evaluates the expression. If the result is falsy (`!value` is true), executes the action. Works with any scalar type where `!value` is meaningful (pointers, integers, floats).

**Limitation:** Does not support struct/union values (no whole-struct zero check). Struct/union pointers work. Typedeffed structs pass through Prism and fail at the backend compiler. `typeof(const struct S)` and `typeof(aggregate_typedef)` are detected by `parse_type_specifier` (sets `is_struct` when `TT_SUE` or `TDF_AGGREGATE` appears inside the `typeof` expression) and rejected for value-fallback orelse. Opaque `typeof(expression)` — where the expression is a variable name (e.g. `typeof(my_var)`) or a function call (e.g. `typeof(make())`) whose type is an aggregate — cannot be detected at the token level. The backend compiler reports a clear error (`used type ... where arithmetic type is required` or `wrong type argument to unary '!'`).

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
| Paren-wrapped decl-init | `int x = (f() orelse 0);` | Outer parens stripped when they are the first init token (macro hygiene) |
| Stmt-expr decl-init | `int x = ({...}) orelse 0;` | `int x = ({...}); x = x ? x : 0;` |

**Paren-wrapped bracket orelse:** `int arr[(f() orelse 1)]` — the outer `(...)` is a common macro-protection pattern (e.g. `#define DIM (f() orelse 1)`). When the parentheses span the *entire* bracket content, Prism strips them and applies the standard hoisting transformation. Deeper wrapping or partial wrapping (e.g. `[(f() orelse 1) + 2]`) is caught as a hard error with a diagnostic suggesting removal of the outer parens.

**Paren-wrapped decl-init orelse:** `int x = (f() orelse 0);` — similar macro-hygiene pattern for declaration initializers. `scan_decl_orelse` strips the outer parentheses **only** when they are the first token of the initializer (i.e., the `(` immediately follows `=`). When the parens appear mid-expression (e.g., `int x = 1 + (f() orelse 5)`), the orelse is inside a sub-expression where paren-stripping would corrupt the AST. Phase 1D enforces this via `init_is_first`: the paren-spanning-to-end check (`P1_OE_DECL_INIT` tagging) only fires on the first paren encountered during the initializer walk.

**Volatile safety:** All forms that include an assignment in the condition use the C assignment-expression value — `(LHS = expr)` yields `expr`'s value without re-reading `LHS`. This makes bare orelse safe for volatile pointer-dereference targets such as MMIO registers: `*uart_tx = get_byte() orelse 0xFF` emits `{ if (!(*uart_tx = get_byte())) *uart_tx = (0xFF); }` with no hidden re-read of the register. Compound-literal fallbacks use a `(LHS = RHS) ? (void)0 : (void)(LHS = (fb))` ternary which evaluates the LHS address expression twice — Phase 1D (primary) and Pass 2 (defense-in-depth) reject pointer dereference (`*`), member access (`->`, `.`), and array subscript (`[]`) in the LHS when the fallback contains a compound literal, since the ternary path would produce double memory access (double bus transactions for volatile MMIO registers). For non-compound-literal bare-assignment orelse with value fallback, `emit_bare_orelse_impl` hoists the RHS into a temp variable using `typeof(LHS) temp = (RHS)`. The temp is typed using `typeof(LHS)` — not `typeof(RHS)` — to avoid double evaluation when the RHS expression yields a variably-modified (VM) type (C23 §6.7.2.5p3: `typeof(EXPR)` evaluates its operand when the expression type is VM). When the LHS has indirection (`*`, `[]`, `.`, `->`), `typeof(LHS)` itself may yield a VM type, so `typeof(RHS)` is used instead — function return types are never VM (C11 §6.7.6.3p1). However, `typeof(RHS)` physically emits the RHS tokens twice at compile time (once inside the `typeof()` operator and once for the assignment initialization). When the RHS expression type is VM, `typeof(RHS)` evaluates its operand at runtime (C11 §6.7.2.4p2), causing double evaluation of any side effects embedded in the RHS. Since Prism cannot determine VM-ness at the token level (it would require full type inference), the RHS is conservatively scanned for side effects via `reject_orelse_side_effects` when `lhs_has_indirection` is true. This rejects `++`, `--`, assignment operators (`=`, `+=`, etc.), function calls (`ident(` and `)(` patterns), depth-0 comma operators, and control-flow keywords (`goto`/`return`/`break`/`continue`/`defer`). **Exception:** when the *entire* RHS is a strictly bare function call with no preceding cast — e.g. `*pp = get_ptr() orelse 0` — the check is bypassed, because function return types are never VM (C11 §6.7.6.3p1), so `typeof(f(...))` never evaluates `f(...)` at runtime. Cast expressions are **not** skipped: a cast like `(int (*)[n])` can introduce a VM type (ISO C11 §6.7.5.2: pointer to VLA is VM), causing `typeof()` to evaluate the operand at runtime and triggering double evaluation of the cast's sub-expression. For example, `*pp = (int (*)[n]) get_ptr() orelse 0` is rejected because the cast changes the expression type to a VM pointer-to-VLA, even though `get_ptr()` itself returns `void *` (not VM). Users must hoist the cast result to a variable first: `int (*tmp)[n] = (int (*)[n]) get_ptr(); *pp = tmp orelse 0;`. Function calls *embedded* in larger expressions (e.g. `*pp = arrays[get_idx()] orelse 0`) are also rejected because the enclosing subscript/dereference may yield a VM type. The control-flow keyword check also prevents Prism state-machine corruption: `emit_balanced_range` traverses the tokens at compile time, so control-flow keywords in statement expressions would be processed twice, corrupting the defer stack (double registration → double-free) or desynchronizing `goto_entry_cursor`. The LHS is guaranteed side-effect-free by `reject_orelse_side_effects`, so `typeof(LHS)` is always safe. Both single-link and chained orelse use if/else blocks with per-link `typeof(LHS)` temps: `{ typeof(LHS) t0=(a); if(t0){ LHS=t0; }else{ LHS=(fb); } }`. This ensures each assignment to LHS undergoes independent simple-assignment conversions (ISO C §6.5.16.1), avoiding the usual arithmetic conversions that a conditional operator (§6.5.15) would force between the temp and fallback operands. Chained example: `{ typeof(LHS) t0=(a); if(t0){LHS=t0;}else{ typeof(LHS) t1=(b); if(t1){ LHS=t1; }else{ LHS=(c); } } }`.

**Side-effect protection:** Bracket orelse in VLA/typeof contexts rejects expressions with side effects (`++`, `--`, `=`, volatile reads via `*`/`->`/`.`/`[]`, function calls, control-flow keywords `goto`/`return`/`break`/`continue`/`defer`) to prevent double evaluation. Control-flow keywords inside statement expressions in duplicated ranges would desynchronize `goto_entry_cursor` (goto) and the defer unwinding stack (return/break/continue), or double defer registration (defer). Block-form orelse (`orelse { … }`) uses the if-guard pattern (single LHS evaluation) and is not subject to this restriction. Function-call detection recognizes both `ident(` and `)(`  (parenthesized call) patterns. **Chained bracket orelse** (e.g. `int arr[0 orelse get_size() orelse 10]`) is subject to an additional chain-aware side-effect check: Phase 1G scans depth-0 orelses and calls `reject_orelse_side_effects` on the range between consecutive depth-0 orelses (the intermediate LHS that would be duplicated in the ternary); Pass 2's `emit_token_range_orelse` retains the same check as defense-in-depth for chains at nested depths and inside `typeof` contexts.

**Constant dimension optimization:** When hoisting preceding bracket dimensions to preserve C99 left-to-right evaluation order, single numeric literal dimensions (e.g., `[5]`) are left in-place rather than hoisted to a temp variable — they have no side effects and cannot be reordered.

**File-scope guard:** Bracket orelse at file scope (brace_depth == 0) is a hard error — hoisting a temp variable requires a statement context.

**Invalid contexts:** Detected at two stages:

- **Phase 1 (early rejection):** Bracket orelse at file scope, bracket orelse with control-flow actions (return/goto/break/continue) or block form, orelse inside enum bodies (compile-time constant context), typeof-orelse inside struct/union bodies, empty orelse action (`orelse ;` — caught by `p1d_validate_bare_orelse` before the missing-target check), anonymous struct/union multi-declarator splits (when bracket orelse or typeof/VLA memset would force type re-emission), bare orelse with cast-expression assignment target (e.g. `(int)x = 0 orelse 5;` — the LHS is not a modifiable lvalue). The typeof-in-struct check runs during `p1_full_depth_prescan` using the scope tree's `is_struct` flag; the enum check uses `is_enum` on the scope tree.
- **Pass 2 (static/thread storage rejection):** `orelse` in the initializer of a `static`, `extern`, or `_Thread_local`/`thread_local` variable is rejected with a hard error. The orelse transformation splits the declaration into a runtime assignment that re-executes on every function entry, which destroys C's persistence semantics (C11 §6.7.9p4). The check runs in `process_declarators` immediately after `scan_decl_orelse` returns, before any code is emitted.
- **Pass 2 (C23 `constexpr` rejection):** `orelse` in the initializer of a `constexpr` variable is rejected with a hard error. `constexpr` mandates a compile-time constant initializer (ISO C23 §6.7.1p14); orelse produces runtime fallback code incompatible with constant evaluation. `constexpr` also implies `const`, so `has_effective_const_qual` returns true when `has_constexpr` is set — ensuring the const orelse path is never bypassed. The check runs alongside the static/extern guard in `process_declarators`.
- **Pass 2 catch-all:** Any `orelse` token that survives to the main emit loop without being consumed by a handler (bracket, decl-init, bare, typeof, walk_balanced) is **unconditionally** rejected with a hard error. This catches orelse in struct/union member declarations, ternary contexts, for-init control parens, and any other unsupported position. No context exemptions — bracket/typeof orelse is fully consumed before reaching the catch-all, so it never fires on valid uses.
- **Pass 2 typeof dispatch:** `typeof(expr orelse fallback)` outside declaration contexts (e.g., inside `sizeof()`, casts) is caught by `TT_TYPEOF` checks that route through `try_typeof_orelse` → `walk_balanced_orelse`, ensuring the inner orelse is transformed to a ternary before any catch-all fires. The check is present in: (1) the main Pass 2 emit loop, (2) `walk_balanced`'s inner loop, (3) `emit_statements` (both EMIT_NORMAL and EMIT_DEFER_BODY modes), (4) `emit_orelse_fallback_value`, (5) `emit_bare_orelse_impl`'s inline fallback loops (compound-literal ternary path and last-link if/else path), (6) `emit_raw_verbatim_to_semicolon` (raw declaration bail-out), and as defense-in-depth in (7) `emit_range_ex` (covers `emit_range`/`emit_range_no_prep`/`emit_balanced_range`), (8) `emit_expr_to_stop`, and (9) `emit_expr_to_semicolon`. Sites (7)–(9) are defense-in-depth only — balanced groups containing typeof are always inside `(...)` which enters `walk_balanced` before reaching the flat emit path.

**Feature flag:** `-fno-orelse` disables.

**Keyword shadow disambiguation:** When the identifier `orelse` is shadowed by a variable, enum constant, or typedef name (`TT_ORELSE` tag co-exists with a typedef-table entry), helper functions disambiguate using positional context:

- `is_orelse_kw_shadow(tok)` — returns true when `tok` has `TT_ORELSE` AND does **not** have a typedef-table entry (or the entry is a shadow). Used only in two remaining call sites (Phase 1 prescan helpers that handle orelse inside `#if` conditionals).
- `is_orelse_kw(tok)` — returns true when `tok` has `TT_ORELSE` but has no typedef-table entry at all (strict: any typedef suppresses). Not used in Pass 2 orelse detection — all active call sites use the positional path instead.
- `orelse_shadow_is_kw(prev)` — given the token *before* the ambiguous `orelse`, returns true when context indicates keyword usage: preceded by an expression-ending token (`)`, `]`, identifiers, numbers, `}`, postfix `++`/`--`). Returns false for type keywords (`TT_TYPE`), qualifiers (`TT_QUALIFIER`), storage classes (`TT_STORAGE`), struct/union/enum (`TT_SUE`), `typeof`/`_BitInt`/`_Alignas`, `inline`, attributes (`TT_ATTR`), and all non-expression-ending tokens. Note: the `TT_MEMBER` check (`.`/`->` before `orelse`) is performed by the **callers** — `orelse_shadow_is_kw` itself only inspects the predecessor token's expression-ending nature.
- `is_orelse_keyword(tok)` — unified entry point for Pass 2: returns true if `tok` is an `orelse` keyword. First checks `TT_ORELSE` tag, then rejects `TT_MEMBER` predecessor (`last_emitted`). For **all** typedef-table entries (both real typedefs and shadows), delegates to `orelse_shadow_is_kw(last_emitted)` for positional disambiguation. Tokens with no typedef entry are treated as unambiguous keywords.

**Distributed positional checks:** Most orelse detection sites do **not** call `is_orelse_keyword` (which requires `last_emitted`). Instead, they perform inline positional disambiguation using the same pattern: check `(tok->tag & TT_ORELSE)`, then `typedef_lookup(tok)`, then `orelse_shadow_is_kw(prev)` with a locally-tracked predecessor token. This applies to:

- `find_bare_orelse` — scans for bare orelse at depth 0, tracks `prev` through the scan loop, uses `!te || orelse_shadow_is_kw(prev)`.
- `scan_bracket_orelse` — scans `[...]` bracket contents for orelse, tracks `prev` at depth 0 and `prev_p` inside spanning parens, uses positional check at both levels.
- `emit_token_range_orelse` — walks balanced groups for orelse transformation, tracks `prev`, uses `!te || orelse_shadow_is_kw(prev)`.
- `check_orelse_in_parens` — validates orelse inside `(...)`, tracks `pi` (previous inner), uses `!te || orelse_shadow_is_kw(pi)`.
- `scan_decl_orelse` — scans declaration initializers, tracks `prev_scan`, uses `!te || orelse_shadow_is_kw(prev_scan)`.
- `p1d_classify_bracket_orelse` — Phase 1D bracket orelse classification in `[...]` dimensions, tracks `prev_bracket`, uses `te_b && !orelse_shadow_is_kw(prev_bracket)` to skip non-keyword matches.
- `p1d_scan_init_orelse` — Phase 1D initializer orelse scan, handles both outer level (`prev_init_tok` with `is_expr_ending_brace`) and inner spanning-paren level (`prev_inner` with `is_expr_ending`). Uses `typedef_lookup` + positional for real typedefs and shadows uniformly.
- Phase 1D typeof rejection — orelse inside `typeof(...)`, tracks `prev_typeof`, uses `!te_s || orelse_shadow_is_kw(prev_typeof)`. Chain-aware: the side-effect check scans ALL chained orelses (not just the first), validating each intermediate LHS range via `reject_orelse_side_effects(se_start, orelse)` where `se_start` advances past each orelse.

All sites also check `TT_MEMBER` on the predecessor where applicable (either inline or via `is_orelse_keyword`'s built-in check).

### 6.3 Zero-Initialization

**Semantics:** All local variable declarations without an explicit initializer get one:

| Declaration | Transformation |
|---|---|
| Scalar (`int x;`) | `int x = 0;` |
| Pointer (`char *p;`) | `char *p = 0;` |
| Aggregate (`struct S s;`) | `struct S s = {0};` |
| Union (`union U u;`) | `union U u; memset(&u, 0, sizeof(u));` |
| Array (`int arr[10];`) | `int arr[10] = {0};` |
| VLA (`int arr[n];`) | `int arr[n]; memset(arr, 0, sizeof(arr));` |
| Typedef-hidden VLA (`T x;` where T is VLA) | `T x; memset(x, 0, sizeof(x));` |

**Scope:** Only inside function bodies (`block_depth > 0`). Not at file scope. Not inside struct/union/enum definitions.

**Declaration prefix handling:** `try_zero_init_decl` skips storage class specifiers (`static`, `extern`, etc.), `inline`, and `__extension__` to reach the actual type keyword. The P1_IS_DECL fast gate probes past these prefixes (`TT_STORAGE | TT_INLINE | TT_SKIP_DECL`) to find the Phase 1D annotation on the type-start token, ensuring `__extension__ int x;` and `__extension__ struct S s;` receive zero-initialization correctly. Phase 1D's prescan also skips `__extension__` (tagged `TT_INLINE`) at statement start before type parsing. **For-init prefix handling:** `p1_scan_init_shadows` places the `P1_IS_DECL` annotation on the type-start token, not the first token of the declaration. When `__extension__` or storage-class specifiers precede the type keyword in a for-init declaration (e.g., `for (__extension__ int i = 0; ...)`), the annotation placement loop skips `TT_STORAGE | TT_INLINE` tokens before annotating. Without this skip, `P1_IS_DECL` would land on the `__extension__` token, and Pass 2's fast gate (which probes past `TT_STORAGE | TT_INLINE | TT_SKIP_DECL` to find the annotation) would never see it — silently dropping zero-initialization for the for-init variable. Similarly, `p1_scan_init_shadows` skips `TT_INLINE`-tagged tokens before checking the `TT_TYPEDEF` tag dispatch, so that `for (__extension__ typedef int T; ...)` correctly routes to `parse_typedef_declaration` instead of falling through to the variable declaration path.

**Storage class exclusions:** Variables with `static`, `extern`, `_Thread_local`, or `thread_local` storage class are **not** zero-initialized. C guarantees these are zero-initialized by the loader (static/extern) or runtime (_Thread_local/thread_local). Emitting `= 0` for static locals would move them from `.bss` to `.data`; emitting `memset` would re-zero them on every function entry, breaking static semantics.

**Typedef awareness:** Uses the immutable typedef table to distinguish `size_t x;` (declaration → initialize) from `size_t * x;` (could be expression → don't touch). Tracks `is_aggregate` to handle struct typedefs that need `= {0}` instead of `= 0`.

**Union memset:** C11 §6.7.9p21 ("all elements that are not initialized explicitly … are initialized implicitly the same as objects that have static storage duration") applies only to aggregates (C11 §6.2.5p21: arrays and structures). Unions are NOT aggregates — `union U u = {0}` initializes only the first named member, leaving remaining bytes indeterminate. GCC-15 empirically confirms this: bytes beyond the first member are not zeroed. Prism routes all union types (direct `union U u;` and typedef'd `BigU u;` where `BigU` is a union typedef) through `__builtin_memset` instead of `= {0}`. Detection: `TypeSpecResult.is_union` is set when `parse_type_specifier` encounters a `TT_SUE` token with `ch0 == 'u'`, and propagated through the typedef table via `TDF_UNION`. In `process_declarators`, `is_union_type = type->is_union && !decl.is_pointer` feeds into `needs_memset`. Phase 1D mirrors this via `p1d_is_union_type` in `any_would_memset`.

**register _Atomic aggregate:** Rejected with an error — `_Atomic struct` with `register` storage class and zero-init would require a non-trivial store sequence.

**register union rejection:** `register union U u;` is rejected with a hard error — `register` forbids address-taking (ISO C11 §6.7.1p6) making `memset` impossible, and `= {0}` only initializes the first named member (C11 §6.7.9p17), leaving remaining bytes indeterminate. The user must remove `register` or use `raw` to opt out. Phase 1D mirrors this rejection. `_Atomic(union U)` propagates `is_union` through the `_Atomic(type)` handler in `parse_type_specifier` — both direct `_Atomic(union U)` and `_Atomic(UTypedef)` where `UTypedef` is a union typedef carry the `is_union` flag (queried via `TDF_UNION`).

**const VLA rejection:** `const`-qualified VLA declarations requiring `typeof`/VLA memset are rejected with a hard error — modifying a `const`-defined object is undefined behavior (ISO C11 §6.7.3p6). The check evaluates `explicit_const = (type->has_const && !decl.is_func_ptr && !decl.is_pointer) || decl.is_const`, then scans const-qualified typedefs in the type specifier range when `explicit_const` is false and the declarator is not a pointer. The `!decl.is_pointer` guard is critical for ISO C type composition: in `const char *arr[n]`, the `const` qualifies the pointee (`char`), not the pointer elements — so `memset`ting the pointer array is legal. Without this guard, Prism fatally rejects standard-compliant code. Conversely, `int *const arr[n]` (const pointer, not const pointee) correctly has `decl.is_const = true`, triggering the rejection. The same `!decl.is_pointer` guard applies in `has_effective_const_qual` (used by the orelse const-fallback path): `const int *p = get() orelse 0` routes through the normal mutable-pointer ternary path, not the const-orelse temp-variable path. Non-VLA `const typeof` (e.g., `typeof(const int) a;`) is allowed because compilers optimize the memset to a compile-time initializer. **Typedef const propagation:** `parse_typedef_declaration` records `is_const` on `TypedefEntry` for base-const non-pointer types (e.g., `typedef const int CI`). Array typedefs preserve this: `typedef const int Arr[n]` sets `is_const = true` on the entry, so `Arr buf;` is correctly rejected when `buf` requires memset. Previously, `is_const` was deliberately stripped from array typedefs (`&& !decl.is_array`), allowing `memset` on `const`-qualified VLA array typedefs — undefined behavior. Chained typedefs also propagate: `typedef const int CI; typedef CI Arr[n];` — the second typedef's type specifier contains `CI` which carries `TDF_CONST`, propagating through the const-typedef scan. **Volatile batching performance note:** The `typeof_vars` batching mechanism passes `type->has_volatile` from the base type specifier to `emit_typeof_memsets`, which forces a byte-by-byte initialization loop (since `memset` drops `volatile`). **Typedef volatile propagation:** `parse_typedef_declaration` records `is_volatile` on `TypedefEntry` (query flag `TDF_VOLATILE`, macro `is_volatile_typedef`) for base-volatile non-pointer types, mirroring the const mechanism. In `process_declarators`, the type specifier range is scanned for volatile typedefs; when found (and the declarator is not a pointer or function pointer), `type->has_volatile` is set, routing the VLA through the byte loop in `emit_typeof_memsets`. Without this, `typedef volatile int VArr[n]; VArr buf;` uses `memset`, which drops the `volatile` qualifier — the compiler may then elide or reorder the initialization, violating the programmer's intent for memory-mapped or concurrent access patterns. For pointer-to-volatile declarations like `volatile int *arr[n]`, the `volatile` qualifies the pointee, not the pointer array elements — `memset` would be safe. However, because `typeof_vars` stores only variable names (not per-declarator type metadata), there is no per-variable volatile discrimination. A mixed declaration `volatile int a[n], *b[n];` forces the byte loop on both. This is a performance-only issue (correctness is preserved: the byte loop is a strict superset of `memset` behavior). **Volatile member propagation:** Structs/unions with volatile-qualified fields (direct or nested) require the byte loop even when the type specifier itself has no `volatile` keyword. `TypeSpecResult.has_volatile_member` (separate from `has_volatile`) signals this condition. Detection: (1) `struct_body_contains_volatile()` scans struct/union bodies for `TT_VOLATILE` tokens and identifiers carrying `TDF_VOLATILE` or `TDF_HAS_VOL_MEMBER`; (2) `TDK_STRUCT_TAG` entries in the typedef table carry `has_volatile_member`, queried via `tag_lookup()` for tag-only references (`struct Foo buf[n]`); (3) typedef entries propagate `has_volatile_member` through `TDF_HAS_VOL_MEMBER`. In `process_declarators`, `type->has_volatile_member` (from `parse_type_specifier`) and `has_volatile_member_typedef()` scans are merged into `type->has_volatile` for the memset vs byte-loop decision. **Struct body scanner paren-awareness:** `struct_body_contains_vla()` and `struct_body_contains_volatile()` track a `prev` token. When scanning encounters `(` preceded by a `TT_TYPEOF`-tagged token or an `_Atomic` token (`(TT_QUALIFIER | TT_TYPE)` both set), the scanner steps *inside* the parens instead of skipping them. This ensures `typeof(int[n])` fields are detected as VLA and `typeof(volatile int)` fields are detected as volatile. Without this, type operators inside struct bodies would be invisible to the property scanners, causing `= {0}` on VLA structs (compile error) or `memset` on volatile structs (UB per ISO C11 §6.7.3p6).

**register VLA rejection:** `register` VLA declarations without an explicit initializer (and without `raw`) are rejected with a hard error — `register` forbids address-taking (ISO C11 §6.7.1p6), making `memset` impossible, and VLAs cannot use `= {0}` initializer syntax. The user must remove `register` or use `raw` to opt out.

**Function-type exclusion:** `typeof(func_name)` where `func_name` is a function (not a function pointer) produces a function type — emitting `memset` on it writes to the `.text` segment (SIGSEGV). `process_declarators` detects function types via three mechanisms: (1) `is_func_typedef` scan of the type specifier for typedef'd function types; (2) `func_meta` scan matching the identifier inside `typeof(…)` against defined functions; (3) forward-declaration scan of `token_pool` at brace depth 0, detecting `ident(` patterns for functions only visible via forward declaration (not in `func_meta`). All three require a single bare identifier inside the `typeof(…)` parens. When detected, `is_func_type` is set and `needs_memset` evaluates to false. **(4) Function type signatures:** `typeof(int(int))`, `typeof(void(void))`, etc. — when the inner tokens start with a type keyword and contain a `(` whose first inner token is not `*`, this is a function parameter list, indicating a function type. If the first `(` is followed by `*`, it is a pointer grouping (`typeof(void(*)(int))`), indicating a function pointer type — `is_func_type` returns false and zero-init proceeds normally. The `*` check uses `skip_noise(tok_next(fs))` to skip GNU `__attribute__((...))` and C23 `[[...]]` attributes between the opening `(` and `*` — without this, `typeof(int (__attribute__((cdecl)) *)(int))` would be misclassified as a function type instead of a function pointer, suppressing memset. **Nested typeof skip:** The function type signature scan skips `typeof`/`typeof_unqual`/`__typeof_unqual__` keywords and their argument parentheses — `typeof(typeof(int[n]))` contains a nested `typeof(int[n])` whose `(` is a typeof argument paren, not a function parameter list. Without this skip, the inner `(` would be misidentified as a function parameter list, incorrectly classifying the VLA-preserving typeof as a function type and suppressing the required `memset`.

**Feature flag:** `-fno-zeroinit` disables.

### 6.4 raw

**Syntax:** `raw <type> <name>;`

**Semantics:** Opts out of zero-initialization for a specific variable. The `raw` keyword is stripped from the output. The resulting declaration is emitted without an initializer. Consecutive `raw` keywords (e.g. `raw raw int x;` from macro expansion) are handled gracefully: `is_raw_declaration_context()` recognizes `TF_RAW` as a valid successor, and each stripping site (`try_zero_init_decl`, `walk_balanced`, `emit_type_range`, `emit_token_range`, file-scope loop) skips all consecutive `raw` tokens before the type. Inside `walk_balanced`, **all** tokens (not just statement-start positions) are emitted through `emit_tok_checked`, which strips `raw` from any position — this covers cast expressions like `[(raw int)x]` inside bracket subscripts and other non-statement contexts.

**Multi-declarator scope:** `raw` applies to **all** declarators when used as a prefix (`raw int x, y;`). Both `x` and `y` opt out. **Per-declarator raw** is also supported: in `int a, raw b;`, only `b` opts out while `a` gets zero-initialized. Both Phase 1D (`p1_skip_decl_raw`) and Pass 2 (`process_declarators`) detect `TF_RAW` after comma and set `decl_is_raw` per-declarator. Both detection paths probe past GNU `__attribute__`, C23 `[[...]]` attributes, and `_Pragma` directives via `skip_noise` to find `raw` hidden behind them (e.g., `int a, [[maybe_unused]] raw b[n];`). This ensures the CFG verifier and emission pass see identical declarator boundaries — without the probe, C23 attributes before `raw` cause Phase 1D to treat `raw` as the variable name, dropping subsequent VLA declarators from analysis and silently disabling goto-over-VLA safety checks. At file scope and struct body, the raw stripping catch-all uses `last_emitted` comma detection to route `raw` tokens from the fast path to the slow path for stripping.

**Safety interaction:** `raw`-marked variables can be safely jumped over by `goto` — the CFG verifier skips them in forward goto checks, **except VLAs** where `raw` does not exempt the declaration.

**Scope:** Works at block scope, file scope, and in struct bodies (stripped silently).

**Defer body:** `raw` is also stripped inside deferred code (`emit_deferred_range` → `emit_statements(EMIT_DEFER_BODY)`). The `emit_deferred_range` SUE body verbatim-emit loop also uses `try_strip_raw` for struct/union/enum definitions inside defer.

**Return type synthesis:** When a non-void function contains `defer`, Pass 2 synthesizes a `typedef` and temp variable from the captured return type via `emit_ret_type` → `emit_token_range`. `emit_token_range` strips `raw` via `try_strip_raw` on each token, preventing `raw` from leaking into the synthesized `typedef __prism_ret_TYPE_N ...` declaration (e.g., `raw int f() { defer ...; }` correctly emits `typedef int __prism_ret_...` rather than `typedef raw int __prism_ret_...`).

**Type emission:** `emit_type_range` (used by `process_declarators` for typeof-based memset, const-orelse continuation types, and multi-declarator split re-emission) strips `raw` via `try_strip_raw` in its fallthrough path. This covers struct/union bodies at depth > 0 — e.g., `struct S { raw int x; } s;` at block scope correctly strips `raw` from the field declaration when re-emitting the type for zero-initialization.

### 6.5 Computed goto

`goto *<expr>` is supported. `FuncMeta.has_computed_goto` is set during Phase 1D when `goto *` is detected. `asm goto` is detected by the tokenizer which sets `TT_ASM` on the function body's opening `{` token. During Phase 2A (`p1_verify_cfg`), if a function contains either a computed goto or asm goto alongside (a) any `P1K_DEFER` entries (when `F_DEFER` is enabled) or (b) any VLA `P1K_DECL` entries or (c) any non-`raw`, non-static-storage `P1K_DECL` entries (when `F_ZEROINIT` is enabled), a hard error is raised — the jump target cannot be verified at compile time, so defer cleanup or zero-initialization could be bypassed.

### 6.6 _Generic

`_Generic` expressions pass through as standard C without transformation. Prism tracks `_Generic` scope depth via `SCOPE_GENERIC` to prevent internal keyword processing (defer, orelse, case/default, label detection, zero-init) from firing inside `_Generic` association lists. The `in_generic()` function walks the scope stack up to the first `SCOPE_BLOCK` to detect when emission is inside a `_Generic(...)` context.

### 6.7 Statement expressions

GNU statement expressions `({…})` are supported. They get their own scope in the scope tree (`is_stmt_expr = true`). Declarations and zero-initialization work correctly inside them. `walk_balanced` detects `({` patterns (including when called directly on a stmt-expr `(`) and processes inner blocks with full keyword dispatch (defer, goto, return/break/continue), `try_zero_init_decl`, raw stripping, and orelse transformation (via `emit_deferred_orelse` at stmt-start with a catch-all `error_tok` for unprocessed orelse tokens, preventing literal "orelse" from leaking to the C output). The `emit_block_body` function (called from `walk_balanced` for stmt-expr processing) uses `uint32_t` for its `itag` variable (matching `Token.tag`'s width) to avoid truncation of high-bit tags like `TT_DEFER` (1<<20), `TT_GOTO` (1<<17), and `TT_ORELSE` (1<<30). `walk_balanced_orelse` (used for array dimensions when `F_ORELSE` is enabled) also detects `({` patterns in its no-orelse emit path and routes them through `walk_balanced` for full transpilation. Additionally, `walk_balanced_orelse` checks for enum definitions with bodies (`is_enum_kw(t)` + `find_struct_body_brace`) in its flat emit loop when `defer_count > 0`, calling `check_enum_body_defer_shadow` to queue enclosing-scope shadow entries — this covers array dimension patterns like `int arr[sizeof(enum { handle = 1 })]`. `emit_orelse_condition_wrap` (which emits the LHS of an orelse in certain `({...}) orelse` forms) routes through `emit_range_no_prep` instead of flat `emit_tok` loops, ensuring statement expressions on the LHS receive full `walk_balanced` processing for defer/orelse/zeroinit. `walk_balanced` saves and restores `ctrl_state` on stmt-expr entry/exit to prevent stmt-expr content from corrupting the outer braceless control-flow tracking.

**Defer constraint:** A block containing `defer` must not be the last statement of the statement expression (the defer emission would overwrite the expression's return value). Prism detects this during Phase 1D's prescan walk via `p1_check_defer_stmt_expr_chain`, which walks the scope tree parent chain from the defer's scope, checking if only trivial tokens (`;`, labels, preprocessor directives, attributes with balanced parens) separate each scope close. If the chain reaches a `is_stmt_expr` scope, a hard error is raised before Pass 2 begins. Pass 2's `handle_close_brace` retains the same check as defense-in-depth (fires at `}` close time when the parent scope is `is_stmt_expr` and the next non-noise token after `}` is `}`).  The user must place the defer block before the final expression: `int fd = ({ int r; { defer cleanup(); r = work(); } r; });` or restructure without a statement expression.

**Multi-declarator stmt-expr:** `int x = ({ int tmp = f() orelse 0; tmp; }), y = 5;` — when the comma after a stmt-expr initializer forces `process_declarators`, `check_orelse_in_parens` skips stmt-expr boundaries (bails early for `({` patterns), and `walk_balanced` processes the inner content with full scope handling.

**typeof_var reentrancy:** `process_declarators` queues VLA/typeof memsets in the shared `typeof_vars[]` array, tracked by `typeof_var_count`. When a statement expression in an array dimension (e.g., `int buf[({ int inner[n]; 32; })]`) triggers a nested `process_declarators` invocation, the inner call must not clobber the outer's queued memsets. Each invocation saves `typeof_var_base = typeof_var_count` on entry and passes it to `flush_typeof_memsets`, which only flushes entries at indices `≥ base` — preserving the outer invocation's entries below the watermark.

**Multi-declarator split trigger:** `should_split_multi_decl` forces a statement split at a comma when there are pending `typeof` memsets (`typeof_var_count > 0`) and the next declarator either has an explicit initializer (`has_init`) or is itself a VLA (`is_vla`) — the latter prevents VLA dimension evaluation before a preceding declarator's memset has run (e.g., `int arr[n], matrix[arr[0]][n];` where `arr[0]` must not be evaluated before `memset(&arr, ...)`). Bracket orelse on the next declarator also triggers a split.

**Multi-declarator VM-type split restriction:** When `process_declarators` must split a multi-declarator statement (due to the triggers above), it re-emits the type specifier for the new declaration. If the type specifier contains a variably-modified type — `typeof(expr)` with VLA dimensions (`has_typeof && is_vla`) or `_Atomic(type)` with VLA dimensions (`has_atomic && is_vla`) — the VLA dimension expression would be evaluated a second time at runtime by the backend compiler (ISO C11 §6.7.2.5 — VM type specifiers are evaluated when the declaration is reached). This causes double evaluation of side effects (function calls, `++`, etc.) in the VLA dimension. Prism rejects such splits with a hard error: the user must declare each variable on a separate line. The guard condition is `(type->has_typeof || type->has_atomic) && type->is_vla`. The `_Atomic(...)` VLA detection was added because `parse_type_specifier` now scans `_Atomic(...)` contents for VLA array dimensions (same pattern as the `typeof(...)` scan), including the `)` predecessor for parenthesized pointer types like `_Atomic(int(*)[n])`. This also covers the orelse `stop_comma` continuation paths (const orelse fallback, orelse action). Anonymous struct/union splits are separately rejected because re-emitting the body produces two incompatible anonymous types. **Two-Pass Invariant for split detection:** Phase 1D's `p1d_check_multi_decl_constraints` must perfectly simulate Pass 2's split logic. The `split` predicate includes three triggers: (1) `current_decl_has_orelse` — the current declarator's initializer contains an `orelse` keyword, which unconditionally forces a split via `process_init_orelse_hit` in Pass 2; (2) `any_would_memset` — a preceding declarator requires a typeof/VLA memset and the next declarator has an initializer; (3) bracket orelse on the next declarator. The `any_would_memset` condition must exactly mirror Pass 2's `needs_memset`: `type.has_typeof || (type.has_atomic && is_aggregate) || type.is_vla || decl.is_vla`, where `is_aggregate = (decl.is_array && (!decl.paren_pointer || decl.paren_array)) || ((type.is_struct || type.is_typedef) && !decl.is_pointer)`. Previously, Phase 1D used a simplified `type.has_atomic && type.is_struct` check that missed `_Atomic` arrays and `_Atomic` typedef aggregates, causing it to approve multi-declarator statements that Pass 2 would split — violating the Two-Pass Invariant. Without the first trigger, Phase 1D would approve VM-type and anonymous struct multi-declarators where the current declarator's orelse forces a split invisible to the static analyzer, causing Pass 2 to crash mid-emission (violating the Two-Pass Invariant). The `any_would_memset` arm also checks `nd.is_vla` (next declarator has VLA dimensions), matching Pass 2's `should_split_multi_decl` which splits when `typeof_var_count > 0 && (next_decl.has_init || next_decl.is_vla)` — without the VLA check, a `typeof(int[n]) arr, buf[get_n()]` declaration would bypass Phase 1D and crash in Pass 2.

**Const orelse VM-type restriction:** `handle_const_orelse_fallback` emits the type specifier twice — once for the mutable temporary and once for the final `const` declaration. When the type is variably-modified (`type->is_vla || decl.is_vla`), this forces the C compiler to evaluate VLA size expressions twice at runtime (ISO C11 §6.7.2.5). Prism rejects const-qualified VM-type orelse with a hard error: the user must hoist the value to a non-const variable first. This covers both VLA dimensions in the declarator suffix (e.g. `const int (*p)[get_size()]`) and in the type specifier via typeof (e.g. `const typeof(int[n]) *p`).

**Type specifier control-flow keyword ban:** `scan_paren_for_vla` (the shared scanner for `typeof(...)` and `_Atomic(...)` contents) rejects `defer`, `goto`, `return`, `break`, and `continue` keywords inside the parenthesized type expression with a hard error. These keywords interact with Prism's transpilation state (defer registration, goto-crossing analysis, unreachable injection) during emission. Because type specifiers may be emitted multiple times — const orelse fallback emits the type twice, multi-declarator splits re-emit the type for continuation declarations — control-flow keywords inside them would be processed repeatedly, causing double defer registration, duplicate goto-crossing checks, or corrupted scope state. The ban fires early in Phase 1 (during `parse_type_specifier`) before any emission occurs, covering all downstream duplication paths uniformly. Under `-fno-safety`, this error is downgraded to a warning (suppressed in library mode), allowing code such as glibc's `INLINE_SYSCALL` macros that expand `__typeof__(({...return...}))` to pass through.

### 6.8 Auto-Unreachable (`-fno-auto-unreachable`, default on)

**Semantics:** After emitting a call to a function classified as noreturn, Prism injects `__builtin_unreachable();` (or `__assume(0);` for MSVC). This propagates Prism's transitive noreturn knowledge into the backend compiler, enabling tail-call optimization, dead code elimination, and register pressure relief.

**Detection:** A function is noreturn if declared with `_Noreturn`, `noreturn`, `[[noreturn]]`, `__attribute__((noreturn))`, `__declspec(noreturn)`, or is a known builtin (`exit`, `abort`, `_Exit`, `_exit`, `quick_exit`, `__builtin_trap`, `__builtin_unreachable`, `thrd_exit`). All occurrences of the function name are tagged `TT_NORETURN_FN` during tokenization.

**Injection rules:**
1. The noreturn identifier must be followed by `(` (a call, not a pointer reference)
2. The matching `)` must be followed by `;` (statement-level call, not a subexpression)
3. Must be inside a function body (`block_depth > 0`)
4. Must NOT be in a braceless control body (would create a multi-statement body without braces)
5. The `__builtin_unreachable();` is emitted immediately after the `;`
6. The predecessor token must NOT be a type keyword, qualifier, storage class, `inline`, `struct`/`union`/`enum`, `*`, or member operator — these indicate a forward declaration (`void abort(void);`) or struct field, not a call. `try_detect_noreturn_call` performs this backward check to avoid injecting `__builtin_unreachable()` after declaration prototypes
7. The identifier must NOT be shadowed by a local variable or parameter. `try_detect_noreturn_call` queries `typedef_lookup(tok)` — if a `TDK_SHADOW` entry exists, the call is to a local variable (e.g., a function pointer parameter named `exit`), not the global noreturn function, so injection is suppressed. Shadow registration for `TT_NORETURN_FN` and `TT_SPECIAL_FN` identifiers is handled by Phase 1C's three shadow registration sites (parameters, for-init declarations, general declarations)

**Disable:** `-fno-auto-unreachable` or `features.auto_unreachable = false` in library mode.

### 6.9 Auto-Static Constant Arrays (`-fno-auto-static`, default on)

#### Problem

A common C pattern declares local `const` arrays initialized with compile-time constants — lookup tables, hash round constants, dispatch tables, error strings:

```c
void sha256_transform(uint8_t *data) {
    const uint32_t K[64] = { 0x428a2f98, 0x71374491, /* ... 62 more ... */ };
    round(data, K);
}
```

The C standard (C11 §6.2.4p5) gives `K` *automatic storage duration* — the compiler allocates 256 bytes on the stack and copies the constant data from `.rodata` on **every call**. This hidden O(N) `memcpy` is expensive (measured at ~5% of total runtime in SHA-256 on x86-64). Compilers are reluctant to optimize it away because `K` is passed to an opaque function — aliasing analysis cannot prove `round()` won't cast away `const` and mutate it (this would be UB per C11 §6.7.3p6, but compilers must be conservative).

The fix is trivial — add `static` — but programmers routinely forget, and the omission is invisible (no warning, no error, just slower code).

#### Semantics

Auto-static automatically injects `static` before local `const` array declarations whose initializer is provably compile-time constant. The array is placed in `.rodata` once, eliminating the per-call stack copy.

This is semantics-preserving: mutating a `const` object is undefined behavior (C11 §6.7.3p6: "If an attempt is made to modify an object defined with a const-qualified type through use of an lvalue with non-const-qualified type, the behavior is undefined"), so sharing a single read-only instance across stack frames is indistinguishable from per-call copies.

#### Observable difference

The only observable change is **address identity**: without `static`, recursive calls produce distinct `&arr` values (separate stack frames); with `static`, all calls share one address. C does not guarantee distinct addresses for objects with non-overlapping lifetimes (C11 §6.5.9p6 footnote), and no known real-world code relies on address-identity of `const` local arrays across recursion depths.

#### ISO C conformance

| Standard rule | Status |
|---|---|
| C11 §6.7.1p2: at most one storage-class specifier per declaration | Enforced — skip when `auto`, `static`, `extern`, or `register` already present |
| C11 §6.7.9p4: static storage duration requires constant initializers | Satisfied — `is_const_literal_init` rejects anything except numeric/string literals, sign operators, designators, and enum constants (which are integer constant expressions per C11 §6.6p6) |
| C11 §6.7.6.2p2: VLAs cannot have static storage duration | Enforced — `!decl.is_vla && !type->is_vla` |
| C11 §6.8.5p3: for-init clause prohibits `static` | Enforced — `!in_ctrl_paren()` |
| C23 §6.8.4.1: if/switch-init clause (same restriction) | Enforced — same `!in_ctrl_paren()` check |
| C11 §6.7.3p6: `const` object mutation is UB | Relied upon — the safety argument |
| C11 §6.7.3p7: `volatile` accesses have side effects | Enforced — skip when `volatile` qualifier present, when struct/union has volatile members, or when volatile is hidden behind a typedef |
| C11 §6.7.5.1: pointer-to-const vs const-array distinction | Enforced — `const int *arr[3]` (mutable array of const-pointers) skipped; only arrays where the array itself is immutable qualify |

#### Eligibility criteria (all must hold)

1. Block scope (`block_depth > 0`) — file-scope arrays already have static storage
2. Not inside a control-flow condition (`!in_ctrl_paren()`) — `static` is illegal in for-init and C23 if/switch-init clauses (C11 §6.8.5p3)
3. Explicit `const` qualifier on the type (`type->has_const`)
4. No `volatile` qualifier (`!type->has_volatile`) — changing storage duration of a `volatile` object may change abstract-machine behavior (C11 §6.7.3p7: volatile accesses are side effects)
5. No `volatile` members in the struct/union type (`!type->has_volatile_member`) — placing a struct with a `volatile` field into `.rodata` defeats volatile access semantics
6. No volatile hidden in typedefs — scans type tokens with `is_volatile_typedef()` and `has_volatile_member_typedef()` to catch `typedef volatile int vint; const vint arr[3]` and `typedef struct { volatile int x; } VS; const VS arr[1]`
7. Declarator is an array (has `[...]` dimensions)
8. When declarator includes a pointer (`*`), requires declarator-level `const` (`decl.is_const`) — `const int *arr[3]` has mutable array elements (the `const` qualifies the pointed-to type, not the array); only `const int * const arr[3]` (where the pointers themselves are const) qualifies
9. Not a VLA (`!decl.is_vla && !type->is_vla`) — VLAs cannot be `static` (C11 §6.7.6.2p2)
10. Has a brace-enclosed initializer (`= { ... }`)
11. No existing storage-class specifier: not `static`, `extern`, `register`, `auto` (C11 §6.7.1p2 prohibits multiple storage-class specifiers), `constexpr` (C23 storage-class specifier), or `_Thread_local`/`thread_local`/`__thread` (thread-local storage class)
12. Not `raw` (user opted out of Prism transformations)
13. No `orelse` on the declaration
14. No GNU `__attribute__` or C23 `[[...]]` on the declarator (between variable name and `=`) — conservatively excluded because semantic attributes like `cleanup()` would silently break when storage duration changes to static
15. Single declarator — multi-declarator `const int a[2] = {1,2}, b[2] = {3,4};` is excluded because promoting only one declarator to `static` while leaving the other `auto` would require splitting the declaration, which violates the principle of minimal transformation
16. Every token inside `{ ... }` is one of: numeric literal, string literal, sign operator (`+`/`-`), brace/bracket/comma/dot/equals punctuation, an identifier preceded by `.` (struct field designator), or an identifier that is a registered enum constant
17. Independent of `-fzeroinit` — auto-static runs through the `process_declarators` pipeline; the gate in `try_zero_init_decl` allows entry when `FEAT(F_AUTO_STATIC)` even if `FEAT(F_ZEROINIT)` is off

#### Initializer scan

`is_const_literal_init(Token *eq)` performs a single forward pass over the brace-enclosed tokens. It is conservative — it rejects any token that *could* produce a non-constant value, including:

- Function calls (`ident(`)
- Variable references (any `TK_IDENT` not preceded by `.` and not a registered enum constant)
- Cast expressions (`(type)value` — the `(` is rejected)
- Address-of / dereference (`&`, `*`)
- Ternary operator (`?`, `:`)
- `sizeof`, `_Alignof`, `offsetof`
- Bitwise/logical operators (`&`, `|`, `^`, `~`, `!`, `<<`, `>>`)
- Comparison / equality operators (`==` — rejected via `len == 1` check on `=`)
- Compound literals (`(type){...}` — the `(` is rejected)
- Multi-character operators sharing `ch0` with sign/designator punctuation: `++`, `--`, `+=`, `-=`, `->` (rejected via `len == 1` check on `+`/`-`), `...` (rejected via `len == 1` check on `.` — prevents runtime variable bypass through `prev_was_dot` designator exemption)

This conservative scan means some valid constant expressions (e.g., `{1 << 3}`, `{sizeof(int)}`) are not promoted. This is a deliberate design choice: false negatives (missed optimization) are safe; false positives (promoting a runtime-dependent init to static) would produce an ISO constraint violation.

#### Injection point

`OUT_LIT("static")` is emitted immediately before `emit_type_range` in `process_declarators` Step 3 (first declarator type emission). The existing `const` token carries `TF_HAS_SPACE`, providing the space between `static` and `const`.

#### Impact

Eliminates hidden `memcpy` calls for:
- Cryptographic round constants (SHA-256 K[64], AES S-box[256])
- Parser lookup/dispatch tables
- Unicode category tables, CRC polynomial tables
- Error message string arrays, enum-to-string maps
- State machine transition tables

#### Disable

`-fno-auto-static` on the command line, or `features.auto_static = false` in library mode.

---

## 7. Error Handling

### error_tok

`noreturn void error_tok(Token *tok, const char *fmt, ...)` — prints an error message with file/line/column context and calls `exit(1)`.

### warn_tok

`void warn_tok(Token *tok, const char *fmt, ...)` — prints a warning. Suppressed in library mode.

### -fno-safety

When `warn_safety` is enabled (`-fno-safety`), CFG violations (goto skipping defers/declarations, switch/case bypassing defers) are downgraded from errors to warnings. VLA skip violations remain hard errors regardless.

### longjmp error recovery (library mode)

In `PRISM_LIB_MODE`, `error_tok` triggers `longjmp(ctx->error_jmp)` instead of `exit(1)`. All arena-allocated structures are reclaimed by `arena_reset()`. Token annotations (`ann` field) survive arena resets (same lifecycle as token pool).

---

---

## 7.1 Implementation Limits

| Limit | Value | Error |
|---|---|---|
| Scope count (scope_id) | 65,534 | `scope tree: too many scopes (>65534)` |
| Braceless control flow nesting depth | 4,096 | `braceless control flow nesting depth exceeds 4096` |
| Array dimension nesting depth | 256 | `array dimension nesting depth exceeds 256` |
| Braceless switch synthetic scopes | Limited by remaining scope_id range | `too many scopes + braceless switches (>65535)` |
| Pointer depth in declarator | 1,024 | Warning (zero-init skipped, not a hard error) |
| Parenthesization depth in declarator | 1,024 | Warning (declarator parse bails out) |

These limits are enforced with hard errors. Exceeding any limit halts transpilation. The pointer depth and parenthesization depth limits produce warnings and skip zero-initialization rather than halting.

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
| `-fno-auto-unreachable` | Disable auto-unreachable injection after noreturn calls |
| `-fno-auto-static` | Disable auto-static promotion of const arrays with literal inits |
| `--prism-cc=<compiler>` | Use specific compiler backend |
| `--prism-verbose` | Show commands being executed |

All other flags pass through to the backend compiler.

### Multi-file handling

Multiple `.c` files are each transpiled independently and passed to CC. Assembly (`.s`, `.S`), C++ (`.cc`, `.cpp`, `.cxx`), Objective-C (`.m`, `.mm`) files pass through untouched. C++ files trigger automatic `g++`/`clang++` backend selection.

### Compiler detection

`cc_is_clang` probes `<CC> --version` for "clang" when the basename doesn't match — handles Termux/FreeBSD/some Linux where `cc` or `gcc` symlinks to clang. Detects the backend to avoid passing unsupported flags (e.g., `-fpreprocessed` is GCC-only — not passed to clang).

### -x language handling

`compile_sources` extracts the user's `-x <lang>` from cc_args and uses it as the pipe language instead of hardcoded `"c"`.

### Non-flatten define re-emission

When `-fno-flatten-headers` is active, Prism runs `cc -E` which consumes in-file `#define` directives. The transpiled output reconstructs `#include` directives but must also re-emit the user's `#define`s that appeared before the first `#include`. `collect_source_defines` scans the original source file (raw text, not tokens) and extracts non-function-like `#define NAME VALUE` directives, both unconditional and conditional. These are re-emitted as `#ifndef NAME` / `#define NAME VALUE` / `#endif` guards by `emit_consumed_defines` before any `#include` directives.

**Conditional guard extraction:** Defines inside `#ifdef`/`#ifndef`/`#if`/`#elif`/`#else` blocks are extracted along with their enclosing preprocessor guard text. A dynamically grown condition stack tracks each nesting level's opening directive (e.g., `#ifdef __APPLE__\n`) and current branch directive (e.g., `#else\n`). When a `#define` is found inside conditional blocks, `emit_consumed_defines` wraps it in the reconstructed guard (concatenation of all active condition stack entries' opening/branch text) followed by matching `#endif` lines. This preserves platform-gated defines like `#ifdef __APPLE__ / #define _DARWIN_C_SOURCE / #endif`.

The scanner handles: multi-line block comments (tracked via `in_block_comment`), mid-line block comment opens on non-directive and directive lines (detected via `has_unclosed_block_comment` which tracks string/char literal context to avoid false positives on `/*` inside string literals), line continuations (`\` at end of line), inline block comments between `#` and the directive name, `#ifdef`/`#ifndef`/`#if`/`#elif`/`#else`/`#endif` nesting with condition stack tracking, multi-line continuation values, and `#define`s that follow block comment closings on the same line.

### System header diagnostic suppression

When `-fflatten-headers` is active, Prism wraps flattened system header content in diagnostic suppression pragmas to prevent warnings from third-party headers polluting the build output. For GCC/Clang: `#pragma GCC diagnostic push` with 10 specific `-W` suppressions (`-Wredundant-decls`, `-Wstrict-prototypes`, `-Wold-style-definition`, `-Wpedantic`, `-Wunused-function`, `-Wunused-parameter`, `-Wunused-variable`, `-Wcast-qual`, `-Wsign-conversion`, `-Wconversion`), closed by `#pragma GCC diagnostic pop`. For MSVC: `#pragma warning(push, 0)` / `#pragma warning(pop)`. Emitted by `emit_system_header_diag_push` / `emit_system_header_diag_pop`.

### POSIX/GNU feature macro injection

During preprocessing (`cc -E`), Prism injects `-D_POSIX_C_SOURCE=200809L` and `-D_GNU_SOURCE` unless the user provides their own (via `-D` flags or `PrismFeatures.defines`). In non-flatten mode, corresponding `#ifndef` / `#define` / `#endif` guards are also emitted into the transpiled output. This ensures POSIX and GNU extensions are available by default for standard library headers.

### Backend warning suppression

In compile and run modes, Prism injects warning suppression flags into the backend compiler invocation via `add_warn_suppress` to prevent spurious warnings from Prism-generated code patterns (e.g., unused variables from hoisted temps, implicit fallthrough from orelse expansion). For GCC/Clang: `-Wno-type-limits`, `-Wno-cast-align`, `-Wno-implicit-fallthrough`, `-Wno-unused-function`, `-Wno-unused-variable`, `-Wno-unused-parameter`, plus `-Wno-unknown-warning-option` (clang) or `-Wno-logical-op` (GCC). For MSVC: `/wd4100`, `/wd4189`, `/wd4244`, `/wd4267`, `/wd4068`. These are NOT injected in transpile mode (stdout output only).

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

`PrismFeatures` struct fields: `compiler`, `include_paths`, `defines`, `compiler_flags`, `force_includes` (with respective counts), plus boolean feature flags (`defer`, `zeroinit`, `line_directives`, `warn_safety`, `flatten_headers`, `orelse`, `auto_unreachable`).

`PrismResult` returns status (`PRISM_OK`, `PRISM_ERR_SYNTAX`, `PRISM_ERR_SEMANTIC`, `PRISM_ERR_IO`) and the transpiled source. `PRISM_ERR_SEMANTIC` is defined but currently unused — all errors route through `PRISM_ERR_SYNTAX`.

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

The transpiled C output of stage1 and stage2 is identical. Binary differences between stage1 and stage2 on macOS are due to Mach-O `LC_UUID` metadata, not code differences.

### CI Matrix

Linux x86_64, macOS x86_64/arm64, Windows build-only, Linux arm64, Linux riscv64.

---

## 12. Files

| File | Description |
|---|---|
| `prism.c` | Main transpiler — all Pass 1 phases, Pass 2 code generation, CLI |
| `parse.c` | Tokenizer, arena allocator, HashMap, `fast_hash`, `error_tok`/`warn_tok`, type/declaration parsing (`parse_type_specifier`, `parse_declarator`, `parse_typedef_declaration`, `parse_enum_constants`), statement skipping (`skip_one_stmt`, `skip_to_semicolon`), defer validation (`validate_defer_statement`), scope utilities (`scope_block_exits`, `scope_is_ancestor_or_self`), noreturn detection (`try_detect_noreturn_call`), VLA analysis (`array_size_is_vla`, `is_array_bracket_predecessor`), raw/const helpers (`is_raw_declaration_context`, `has_effective_const_qual`) |
| `windows.c` | Native Windows shim (used from `parse.c` for platform-specific I/O) |

`prism.c` includes `parse.c` via `#include`. Single compilation unit — no separate linking step.

---

## 13. Architectural Boundaries

### What Pass 2 does NOT do

Pass 2 is a near-pure code generator. It does not:

- Mutate the typedef table (`typedef_add_entry` is never called)
- Unwind the typedef scope (`typedef_pop_scope` does not exist)
- Register shadows (`p1_register_shadow`, `p1_register_param_shadows` are Phase 1 only)
- Walk the token stream for CFG safety checks (no `goto_skips_check`, no `backward_goto_skips_defer`, no `backward_goto_skips_decl`)
- Validate defer bodies (`validate_defer_statement` runs only in Phase 1F)
- Track return type state machines (`FuncMeta` provides return type data at function body entry)
- Register ghost enums during emit (`emit_tok` does not call `parse_enum_constants`)

### What stays in Pass 2

These are inherently runtime and cannot move to Pass 1:

- **Scope stack push/pop** — driven by `{`/`}` during emit; timing drives defer cleanup
- **Defer stack** — `defer_add` at keyword, `emit_defers` at `}`/return/break/continue/goto
- **CtrlState** — braceless control-flow brace injection: `pending`, `pending_for_paren`, `parens_just_closed`, `brace_depth`. Tracking is inherently sequential and cannot move to Pass 1 without new infrastructure. `end_statement_after_semicolon` resets `ctrl_state` only when `!in_ctrl_paren() && !in_struct_body()` — struct/enum/init body semicolons must not clear the braceless-body tracking state (prevents stale `SCOPE_CTRL_PAREN` leak from compound literals like `(struct {int x;}){0}` in braceless bodies). `emit_ctrl_condition` clears `parens_just_closed` before processing condition tokens to prevent stale brace-wrap from outer braceless contexts (e.g. `else for(int i=0;...)`).
- **`at_stmt_start`** — inherently sequential
- **`ret_counter`** — monotonic during emit
- **Line directive / whitespace emission** — tied to output position
- **Bare-expression orelse classification** — requires expression boundary parsing; safe because symbol table is immutable
- **`p1_label_find`** — O(1) label lookup via `FuncMeta.label_hash` (open-addressing hash table persisted from Phase 2A); `handle_goto_keyword` reads pre-computed scope exits via `p1_goto_exits`

---

## 14. Invariants

1. **Immutable symbol table:** After Phase 1B completes, the typedef table is frozen. Pass 2 performs zero mutations to the typedef table, scope tree, token annotations (`ann`), or `func_meta`.
2. **All errors before emission:** Every user-triggerable `error_tok` call from semantic analysis fires during Pass 1 or Phase 2A, before Pass 2 emits its first byte, with one documented exception: `check_defer_shadow_at_exit` requires runtime scope-stack state that only exists during Pass 2 control-flow exit emission. Same-block defer shadows (variable declared after a defer in the same scope) are detected immediately at declaration time in `check_defer_var_shadow` and do not rely on `check_defer_shadow_at_exit`. Pass 2 contains additional defensive `error_tok` calls in `process_declarators`, `emit_bare_orelse_impl`, `emit_orelse_action`, etc. that serve as unreachable-by-design assertions — they guard against internal inconsistencies that would indicate a Pass 1 gap, not against user input that should have been caught earlier. Phase 1D detects: array orelse, struct value orelse, compound-assign bare orelse, bare orelse without assignment target, empty orelse action (`orelse ;`), anonymous struct/union multi-declarator split, VLA-type cast in bare orelse RHS, volatile dereference with compound-literal fallback, and preprocessor conditional spanning. The preprocessor boundary check (rejecting bare orelse assignments that span `#ifdef`/`#ifndef`/`#if`/`#elif`/`#else`/`#endif`) is the Phase 1D primary; the copy in `emit_bare_orelse_impl` is defense-in-depth. This check is unreachable in CLI mode (input arrives from `cc -E` with conditionals already resolved) but reachable in Library Mode (`prism_transpile_source`), where raw un-preprocessed source preserves conditional directive tokens.
3. **O(N) CFG verification:** `p1_verify_cfg` is guaranteed linear in the number of P1FuncEntry items per function. No O(N²) pairwise scans.
4. **Delimiter matching completeness:** Every `(`, `[`, `{` has a `match_idx`. Every `)`, `]`, `}` points back. No unmatched delimiters survive tokenization.
5. **Self-host fixed point:** Stage 1 and Stage 2 transpiled C output is identical.
6. **Arena safety:** All arena-allocated Pass 1 structures are reclaimed on `longjmp` error recovery. No dangling pointers after `arena_reset()`.
7. **Signal cleanup safety:** `signal_temps_clear()` zeroes the **entire** buffer (`memset`, `PATH_MAX` bytes) of every registered path slot before resetting the counter. `signal_cleanup_handler` checks both `signal_temps_ready_load(i)` (atomic acquire fence on the ready flag) and `signal_temps[i][0] != '\0'` before unlinking. This eliminates the TOCTOU race between `signal_temps_register`'s CAS (counter increment) and `memcpy` (path write): if a signal arrives during `memcpy`, partially written data is followed by zeroes (from the prior `memset`), so the handler sees at worst a truncated — but never fabricated — path. Previous behavior (zeroing only byte 0) left stale path data in bytes 1..N, which could reconstruct a prior cycle's path if `memcpy` was interrupted after writing just the first byte.

---

## 15. Known Limitations and Caveats

1. **Struct padding bytes and `= {0}`:** Prism emits `= {0}` for aggregate zero-initialization. The C standard guarantees all *members* are zeroed but does not mandate that *padding bytes* between members are zeroed. In practice, GCC and Clang emit `memset`/`bzero` for `= {0}` at all optimization levels, zeroing the entire aggregate including padding. However, code that copies raw struct bytes across trust boundaries (e.g., `copy_to_user` in kernel contexts) should use explicit `memset(&s, 0, sizeof(s))` via `raw` + manual initialization to guarantee no padding infoleak. This is a C language limitation, not a Prism deficiency.

2. **Indirect call taint bypass (cross-TU):** The `setjmp`/`longjmp`/`vfork` taint detection is token-name-based: any appearance of a tainted identifier (even as a bare reference like `= longjmp` or `fp = vfork`) taints the enclosing function, and taint propagates transitively to callers. However, a function pointer to `longjmp` passed from another translation unit (e.g., via a `void (*)(jmp_buf, int)` parameter) is undetectable by single-TU static analysis. A function that merely returns or stores `vfork` as a value will also taint its callers even if the pointer is never invoked — this is an accepted false positive that closes intra-TU aliasing attacks. This is an inherent limitation shared with all C static analyzers that operate on individual translation units.

3. **Bitfield zero-initialization:** Bitfield member declarations (`int x : 4;`) inside struct/union bodies are not individually zero-initialized — `try_zero_init_decl` correctly skips when `in_struct_body()` is true (bitfield syntax `int x : 4 = 0;` is invalid C). Bitfields are zeroed implicitly when the parent struct variable receives `= {0}`. This is working as designed.

---

# Part II: Formal Language Specification

This part defines the syntax, constraints, and semantics of Prism's language extensions to C. All descriptions are framed in terms of the C abstract machine as defined by ISO/IEC 9899. No reference is made to implementation strategies, internal data structures, or transpilation passes.

Unless otherwise stated, all standard C terms (*block scope*, *scalar type*, *lvalue*, *side effect*, *object*, *storage duration*) carry their ISO C definitions.

A *conforming Prism program* is a strictly conforming C program extended with the constructs specified herein. A diagnostic is required for every constraint violation listed below. A conforming implementation may provide mechanisms to independently enable or disable each extension; when an extension is disabled, its keyword reverts to an ordinary identifier.

---

## The `defer` Statement

### Syntax

```
defer-statement:
    defer statement
    defer compound-statement
```

The token `defer` is a keyword. A *defer-statement* is a block-scoped statement that registers its operand for deferred execution.

### Constraints

1. A *defer-statement* shall appear only at block scope within the body of a function definition.

2. The defer body establishes an independent control-flow context. The following constraints apply to statements within the defer body:
   - `return` — prohibited unconditionally. A defer body shall not return from the enclosing function.
   - `goto` — prohibited unconditionally.
   - `break` — permitted only when targeting a `switch`, `for`, `while`, or `do` statement that is itself within the defer body.
   - `continue` — permitted only when targeting a `for`, `while`, or `do` statement that is itself within the defer body.

3. Nested `defer` (a *defer-statement* within a defer body) is a constraint violation.

4. User-defined labels within a defer body are a constraint violation. The defer body may be duplicated at multiple scope exit points; labels would produce duplicates.

5. A *defer-statement* shall not appear in a function whose body contains any identifier from the following categories, whether invoked, referenced, or assigned to a pointer:

   **Non-local jump:** `setjmp`, `_setjmp`, `__setjmp`, `sigsetjmp`, `__sigsetjmp`, `savectx`, `__builtin_setjmp`, `__builtin_setjmp_receive`

   **Non-local jump restoration:** `longjmp`, `_longjmp`, `__longjmp`, `siglongjmp`, `__siglongjmp`, `__longjmp_chk`, `__builtin_longjmp`

   **Process creation:** `vfork`

   **Thread termination:** `pthread_exit`

6. A *defer-statement* shall not appear in a function that uses computed goto (`goto *`*expression*) or `asm goto` (extended inline assembly with goto labels).

7. A *defer-statement* shall not appear at the direct top level of a GNU statement expression `({ ... })`. It must be wrapped in an inner block.

8. A compound statement containing a *defer-statement* shall not be the final statement of a GNU statement expression. The deferred actions would overwrite the expression's result value.

9. A variable declaration that introduces a name referenced by an active defer body in the same block scope is a constraint violation (same-block shadow rule).

### Semantics

1. Execution of a *defer-statement* registers the defer body for later execution. The defer body is **not** executed at the point of the defer-statement.

2. When control leaves a block scope, all defer bodies registered in that scope execute in reverse order of registration (last-in, first-out). Control may leave a scope through:
   - Normal flow reaching the closing `}`.
   - A `return` statement.
   - A `break` statement leaving a loop or switch.
   - A `continue` statement advancing to the next loop iteration.
   - A `goto` statement transferring control to a label outside the scope.

3. For nested scopes, defer bodies are unwound from innermost to outermost. A `return` at depth *N* causes defers at depths *N*, *N*−1, …, 1 to fire, LIFO within each scope, proceeding outward.

4. A `goto` crossing scope boundaries causes defers in all exited scopes to fire, LIFO within each scope, from innermost to the target scope (exclusive).

5. Defers inside a GNU statement expression `({ ... })` fire at the statement expression's scope boundary, not at the enclosing function scope.

6. The return value of a `return` statement is fully evaluated before any defers execute. The sequence is: evaluate return expression → save result → execute defers (LIFO, innermost to outermost) → transfer result to caller.

7. Between consecutive `case` or `default` labels in a `switch` body, defers are unwound consistently with C's fallthrough semantics.

---

## The `orelse` Operator

### Syntax

```
orelse-expression:
    assignment-expression orelse fallback-action

fallback-action:
    expression
    compound-statement
    return expression_opt ;
    break ;
    continue ;
    goto identifier ;

bracket-orelse:
    expression orelse expression      (within array dimension [ ])
```

The token `orelse` is a keyword. It introduces a conditional fallback that executes when the left operand evaluates to a *falsy* value (one for which `!value` is true under C's unary `!` operator).

### Forms

| Form | Syntax | Description |
|---|---|---|
| Assignment with value | `LHS = expr orelse fallback;` | Assign `expr` to `LHS`; if falsy, reassign `fallback` |
| Assignment with action | `LHS = expr orelse action;` | Assign `expr` to `LHS`; if falsy, execute control-flow action |
| Assignment with block | `LHS = expr orelse { ... }` | Assign `expr` to `LHS`; if falsy, execute block |
| Bare expression | `expr orelse action;` | Evaluate `expr`; if falsy, execute action |
| Declaration initializer | `type name = expr orelse fallback;` | Declare and initialize; fallback if falsy |
| Bracket (array dim) | `type arr[dim orelse fallback]` | Use `dim` as array size; if falsy, use `fallback` |

### Constraints

1. The left operand of `orelse` shall have scalar type (integer, floating-point, or pointer). Struct and union *values* are not permitted. Pointers to struct or union types are permitted.

2. An `orelse` shall not appear at file scope (outside any function body).

3. An `orelse` shall not appear in the initializer of a variable with `static`, `extern`, `_Thread_local`, or `thread_local` storage duration. The orelse transformation produces runtime code; re-executing it on each function entry would violate C's static persistence guarantees (ISO C11 §6.7.9¶4). An `orelse` shall also not appear in the initializer of a `constexpr` variable, which requires a compile-time constant initializer (ISO C23 §6.7.1).

4. An `orelse` shall not appear inside an `enum` body. Enum values require integer constant expressions.

5. A *bracket-orelse* shall not use a control-flow action (`return`, `goto`, `break`, `continue`) or compound statement as its right operand.

6. A *bracket-orelse* at file scope is a constraint violation — no statement context exists for the required temporary variable.

7. In contexts where the array dimension expression may be evaluated more than once (VLA sizes, `typeof` operands), neither operand of a *bracket-orelse* shall contain side effects. Prohibited side effects include:
   - Increment/decrement operators (`++`, `--`)
   - Assignment operators (`=`, `+=`, `-=`, `*=`, `/=`, `%=`, `<<=`, `>>=`, `&=`, `^=`, `|=`)
   - Function calls (direct or through function pointers)
   - Volatile reads through indirection (`*`, `->`, `.`, `[]`)
   - Inline assembly (`asm`)
   - Comma operator at the top level of the operand

8. A bare `orelse` (no assignment target) with a value fallback requires a modifiable lvalue on the left side of `=`. A cast-expression target (e.g., `(int)x = expr orelse fallback`) is a constraint violation.

9. When the fallback is a compound literal and the assignment target involves indirection (`*`, `->`, `.`, `[]`), the `orelse` is a constraint violation. The compound-literal code path evaluates the target address expression twice, producing undefined behavior for volatile objects. **Compound literal detection** scans the fallback range for `{` at any parenthesis depth: a `{` preceded by `)` (the cast pattern `(type){`) is recognized as a compound literal even inside parentheses (e.g., `(&((struct S){0}))`), because such compound literals would be scoped to the else block (C11 §6.5.2.5p5) and produce dangling pointers. Compound literals inside function call arguments are also detected and use the ternary path — token-level analysis cannot determine whether a function returns the compound literal's address, so the conservative approach avoids dangling pointers in all cases (e.g., `identity_ptr(&(struct Config){.mode=1})` where the function returns its argument).

10. An `orelse` in a function parameter array dimension is a constraint violation in both prototypes and definitions. For prototypes (e.g., `void f(int arr[n orelse 1]);`), parameter arrays decay to pointers and the dimension is never evaluated at runtime. For definitions (e.g., `void f(int arr[n orelse 1]) { ... }`), the ternary expansion would evaluate the dimension expression twice — undefined behavior for volatile expressions (C11 §6.7.3p7) — and temporary variables cannot be hoisted outside the function signature.

11. An `orelse` shall not appear inside a `struct` or `union` member declaration (including within a `typeof` context in that declaration).

12. Any `orelse` token that does not match one of the recognized forms listed above is a constraint violation.

### Semantics

1. **Assignment with value:** `LHS = expr orelse fallback;` — The expression `expr` is evaluated and assigned to `LHS`. If `!(LHS)` is true after the assignment, `LHS` is assigned the value of `fallback`. If `!(LHS)` is false, `fallback` is not evaluated. The initial assignment `LHS = expr` occurs exactly once.

2. **Assignment with action:** `LHS = expr orelse action;` — The expression `expr` is evaluated and assigned to `LHS`. If `!(LHS)` is true, all applicable defers in scopes being exited by `action` fire (LIFO), then `action` executes. If `!(LHS)` is false, execution continues normally.

3. **Block form:** `LHS = expr orelse { ... }` — As assignment-with-action, but the compound statement body executes. Defer, orelse, and zero-initialization within the block are processed normally.

4. **Bare expression:** `expr orelse action;` — The expression `expr` is evaluated. If `!(expr)` is true, `action` executes with applicable defer cleanup. Otherwise, execution continues.

5. **Declaration initializer:** `type name = expr orelse fallback;` — The variable `name` is declared. `expr` is evaluated and assigned to `name`. If `!(name)` is true, `name` is reassigned `fallback` (or the control-flow action executes).

6. **Bracket form:** `type arr[dim orelse fallback]` — The expression `dim` is evaluated. If `!(dim)` is true, `fallback` is used as the array dimension. Semantically equivalent to `dim ? dim : fallback`, subject to the side-effect constraints above.

7. **Chained orelse:** `expr orelse a orelse b` — Evaluated left to right. If `expr` is falsy, `a` is evaluated. If `a` is also falsy, `b` is used. Each intermediate value is tested independently.

8. **Volatile safety:** In assignment forms, the truthiness test uses the value produced by the C assignment expression `(LHS = expr)`, not a re-read of `LHS`. For volatile-qualified lvalues, this guarantees exactly one write when the result is truthy, and no hidden reads.

9. **Paren-wrapped forms:** `int x = (f() orelse 0);` and `int arr[(dim orelse 1)]` — Parentheses spanning the entire initializer or entire bracket content are recognized as macro-hygiene wrappers and are permitted.

### Result Type

The *orelse-expression* is not a single C operator; it is a source-level directive that expands to standard C statements. Consequently, its "result type" is defined indirectly by the generated code:

1. **Value-fallback forms** (assignment and declaration-initializer): The left operand is assigned to the target lvalue via a standard C assignment expression. The right operand (fallback) is assigned to the same lvalue via a separate assignment if the condition fails. Both assignments undergo the usual implicit conversions for simple assignment (ISO C §6.5.16.1). The type of each assignment is the type of the lvalue after lvalue conversion. No additional conversions between the left and right operands are performed — they are independently converted to the target type.

2. **Bracket form:** The expansion uses a conditional expression `dim ? dim : fallback`. The result type is determined by the usual arithmetic conversions applied to the second and third operands of the conditional (ISO C §6.5.15¶5).

3. **Control-flow forms** (`return`, `break`, `continue`, `goto`, compound statement): The right operand is a statement, not a value. The orelse construct does not yield a value; the expansion is purely control flow. No type conversion applies.

---

## Automatic Zero-Initialization

### Constraints

1. Automatic zero-initialization applies to objects with automatic storage duration (block-scope, no `static`, `extern`, `_Thread_local`, or `thread_local` qualifier) that lack an explicit initializer and are not marked with the `raw` keyword.

2. Objects with `static`, `extern`, or thread storage duration are excluded. The C standard already guarantees zero-initialization for these.

3. Declarations inside `struct`, `union`, or `enum` definitions are excluded.

4. A `register`-qualified variable-length array (VLA) without an explicit initializer and without `raw` is a constraint violation. `register` prohibits address-taking (ISO C11 §6.7.1¶6), making `memset` impossible, and VLAs cannot use `= {0}` syntax.

5. A `const`-qualified VLA declaration requiring runtime zero-initialization is a constraint violation. Modifying a `const`-defined object is undefined behavior (ISO C11 §6.7.3¶6).

6. A `register`-qualified `_Atomic` aggregate without an explicit initializer is a constraint violation.

7. A function using computed goto (`goto *`*expression*) or `asm goto` that contains zero-initialized declarations is a constraint violation. The jump target cannot be verified at compile time; initialization could be bypassed.

### Semantics

1. **Scalar types** (integers, floating-point, pointers, `_Bool`, `_Complex`, `_BitInt`) receive `= 0`.

2. **Aggregate types** (structs, unions, fixed-size arrays) receive `= {0}`. Per ISO C §6.7.9¶21, if there are fewer initializers in a brace-enclosed list than there are elements or members of an aggregate, the remainder of the aggregate is initialized implicitly the same as objects that have static storage duration — that is, to zero for arithmetic types and null for pointers. Padding bytes between members are not required to be zeroed by the standard (though major implementations produce `memset`/`bzero` for `= {0}` in practice).

3. **Variable-length arrays** receive `memset(arr, 0, sizeof(arr))` immediately after the declaration.

4. **Typedef-hidden VLAs** — objects whose type resolves through typedefs to a VLA type — receive `memset` treatment.

5. **Function types** — a variable declared via `typeof(func_name)` where the operand names a function (not a function pointer) is excluded from zero-initialization.

6. Initialization is applied at the point of declaration, preserving C's sequential left-to-right evaluation order for multi-declarator statements.

---

## The `raw` Keyword

### Syntax

`raw` is an *initialization-suppression specifier*. It extends the C grammar at two levels:

**As a declaration specifier** (ISO C §6.7 *declaration-specifiers*), `raw` may appear alongside storage-class specifiers, type qualifiers, and type specifiers. When used in this position, it applies to all declarators in the declaration:

```
declaration-specifiers:
    raw declaration-specifiers_opt          (extension)
    storage-class-specifier declaration-specifiers_opt
    type-specifier declaration-specifiers_opt
    type-qualifier declaration-specifiers_opt
    ...
```

`raw` shall precede the first type specifier. `int raw x;` is ill-formed.

**As an init-declarator modifier** (ISO C §6.7 *init-declarator-list*), `raw` may appear immediately before a declarator in a comma-separated init-declarator list. When used in this position, it applies only to the immediately following declarator:

```
init-declarator:
    raw_opt declarator                      (extension)
    raw_opt declarator = initializer        (extension)
```

This allows mixed declarations: `int a, raw b;` — `a` receives automatic zero-initialization, `b` does not.

### Constraints

1. `raw` shall appear either before the declaration specifiers of a declaration, or immediately before a declarator in an init-declarator list (optionally preceded by attributes). It shall not appear inside a type specifier, abstract declarator, or expression.

2. `raw` does not affect VLA lifetime or scope. A `raw`-marked VLA is still subject to `goto`-over-VLA safety checks (see §Undefined Behavior, item 3).

3. `raw` is not a type qualifier and does not participate in type composition. It does not affect the type of the declared object.

### Semantics

1. A `raw`-marked declaration is not automatically zero-initialized. The `raw` token is consumed and does not appear in the generated C output.

2. As a declaration specifier (`raw int a, b;`), `raw` applies to all declarators in the init-declarator list.

3. As an init-declarator modifier (`int a, raw b;`), `raw` applies only to the immediately following declarator.

4. Consecutive `raw` tokens (e.g., from macro expansion: `raw raw int x;`) are silently absorbed.

5. At file scope and inside struct/union bodies, `raw` is silently stripped — no zero-initialization applies in these contexts.

---

## Undefined Behavior

The following actions result in undefined behavior. A conforming implementation is not required to diagnose these conditions.

1. **Jump over `raw` VLA:** If a `goto` statement or `switch` case transfer crosses the declaration of a `raw`-marked object with variably modified type, the behavior is undefined. The `raw` keyword suppresses zero-initialization but does not suppress the VLA's stack allocation; the backend compiler may generate an allocation instruction at the point of declaration, and jumping past it produces an object with indeterminate size or an invalid stack frame. (Prism diagnoses this as a constraint violation when possible; however, computed goto targets are unresolvable at compile time, making exhaustive detection infeasible.)

2. **Return expression modifying deferred state:** If the expression of a `return` statement modifies an object that is subsequently read by an active `defer` body in the same function, the value read by the defer body is the value *after* the modification. The return expression is fully evaluated and its result captured before any defers execute (see *defer* Semantics §6). However, if the return expression and a defer body both modify the same object through a volatile-qualified lvalue, the number and ordering of volatile accesses is unspecified — the implementation may introduce a temporary variable for the return value, altering the total volatile access count.

3. **`longjmp` past defers:** If `longjmp` (or any non-local jump mechanism) transfers control out of a scope that has active `defer` registrations, the defers do not execute. Prism prohibits `defer` in functions containing `setjmp`/`longjmp` identifiers (see *defer* Constraint §5), but indirect calls through function pointers obtained from other translation units are undetectable by single-TU analysis.

4. **VLA size expressions evaluated twice:** For `const`-qualified or variably-modified type specifiers involving `typeof(expr)` — where `expr` contains VLA dimensions — multi-declarator statement splits or const-orelse expansions may cause the VLA size expression to be evaluated more than once. If the size expression has side effects, the behavior is undefined. (Prism diagnoses known cases as constraint violations, but expressions with undetectable side effects — e.g., a function call through an opaque pointer — may escape static analysis.)

5. **Padding bytes in zero-initialized aggregates:** Automatic zero-initialization of aggregate types uses `= {0}`, which the C standard specifies initializes all members and sub-objects to zero but does not mandate that padding bytes between members are zeroed (ISO C §6.7.9¶21). Code that relies on padding bytes being zero (e.g., byte-level comparison, raw-byte serialization, copying across trust boundaries) has implementation-defined behavior. For guaranteed all-bytes-zero, the programmer should use `raw` and explicit `memset`.
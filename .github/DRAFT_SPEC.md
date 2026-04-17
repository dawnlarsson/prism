# Prism Draft Specification

**Status:** Ideas under consideration. Nothing here is implemented. Items may be adopted, modified, or rejected. Once implemented, they move to SPEC.md.

---

## 1. Defer 2.0: Channels & Goto-Patch Emission

**This is not a separate flag — it's the next evolution of `-fdefer`.**

### Problem 1: Error-path vs success-path cleanup

The most common `goto cleanup` pattern in C isn't "undo everything" — it's "undo on error, keep on success." A function allocates resources progressively, and on failure it must release the ones already acquired, but on success it returns ownership to the caller. Current `defer` fires on ALL exit paths — there's no way to say "only on error."

### Problem 2: Inline emission bloats hot paths

Current defer inlines the cleanup body at every `return`/`goto`/`break`/scope-exit site. With 5 defers and 10 returns, that's 50 inlined cleanup statements — cold code polluting the hot path, bloating the instruction cache, and inflating binary size.

```c
// Current emission — cleanup duplicated at every exit:
if (bad) { free(buf); fclose(f); return -1; }     // inlined
if (worse) { free(buf); fclose(f); return -2; }   // inlined again
{ fclose(f); return buf; }                          // inlined again
```

### Solution: Layered goto-patch at end of function

Defer 2.0 changes the emission strategy. Instead of inlining cleanup at every exit point, each exit becomes a single `goto` into a layered cleanup chain at the bottom of the function. This is exactly the pattern that expert kernel developers write by hand — Prism automates it.

### Syntax

| Construct | Meaning |
|---|---|
| `defer stmt;` | Always-fire, goto-patch emission (cold, end-of-function) |
| `defer(name) stmt;` | Channel-tagged, goto-patch, fires only on `return(name:)` |
| `defer_inline stmt;` | Always-fire, inlined at every exit point (for tiny one-liners) |
| `defer_inline(name) stmt;` | Channel-tagged, inlined at every exit point |
| `return expr;` | Fires only untagged defers |
| `return(name:) expr;` | Fires untagged defers AND `name`-tagged defers |
| `defer_flush;` | Fire and consume all active always-defers now (inlined) |
| `defer_flush(name);` | Fire and consume all active `name`-channel defers now (inlined) |

The colon in `return(name:)` disambiguates from `return(expr)`. The tokenizer sees `return` + `(` + identifier + `:` + `)` — unambiguous, no conflict with standard C.

### Example: source

```c
int *parse_file(const char *path) {
    FILE *f = fopen(path, "r");
    defer fclose(f);

    int *buf = malloc(100);
    defer(err) free(buf);

    if (!f)  return(err:) NULL;
    if (bad) return(err:) NULL;

    return buf;
}
```

### Example: emitted code (defer 2.0 goto-patch)

```c
int *parse_file(const char *path) {
    FILE *f = fopen(path, "r");
    int *buf = malloc(100);

    int *__prism_rv;

    if (!f)  { __prism_rv = NULL; goto __prism_err_2; }
    if (bad) { __prism_rv = NULL; goto __prism_err_2; }

    __prism_rv = buf;
    goto __prism_defer_1;

    // — cold cleanup patch (end of function) —
__prism_err_2:              // error channel entry point
    free(buf);              // defer(err) — LIFO
__prism_defer_1:            // always-defer entry point
    fclose(f);              // defer — LIFO
    return __prism_rv;
}
```

### What this achieves

**Hot path optimization:** The `if (!f)` and `if (bad)` branches contain a single `goto` — no cleanup code. The branch predictor marks them as not-taken. The instruction cache stays warm on the success path.

**Cold code consolidation:** All cleanup lives in one place at the end of the function. Binary size drops because cleanup statements appear once, not once per exit point.

**Natural channel layering:** The goto-patch is a cascade — error-channel defers sit above always-defers. A `return(err:)` enters at the top (fires both), a plain `return` enters below (fires only always-defers). No flag variables, no conditionals in the cleanup chain.

### Emission rules

1. **Return value capture:** Each `return expr;` becomes `__prism_rv = expr; goto __prism_defer_N;` where the entry point depends on which channels fire
2. **Void functions:** No `__prism_rv`, just `goto __prism_defer_N;` and bare `return;` at the end
3. **LIFO ordering:** Within each tier, defers execute in reverse declaration order (same as current)
4. **Cross-channel interleaving:** If `defer` and `defer(err)` interleave, the cleanup chain preserves declaration order across tiers:
   ```c
   defer A;           // always
   defer(err) B;      // error
   defer C;           // always
   defer(err) D;      // error
   
   // Goto-patch (LIFO):
   __prism_err:
       D;             // defer(err) — most recent
       C;             // defer — always (interleaved)
       B;             // defer(err)
   __prism_defer:
       A;             // defer — always (only the non-err defers below the lowest err)
       return __prism_rv;
   ```
5. **Scope nesting:** Defers inside nested `{ }` blocks create sub-chains that fire when that scope's `}` is reached (same as current), using local goto labels

### Backward compatibility

Existing `defer` code with no channels works identically — just with better emission. `defer` in 2.0 defaults to goto-patch. To get the old inline behavior, use `defer_inline`. The return value of every exit path is unchanged.

### Emission strategy summary

| Keyword | At `return` / scope exit | At `defer_flush` |
|---|---|---|
| `defer` | goto-patch (cold, end-of-function) | inlined at flush site |
| `defer_inline` | inlined at every exit point | inlined at flush site |
| `defer(name)` | goto-patch at `return(name:)` | inlined at `defer_flush(name)` |
| `defer_inline(name)` | inlined at `return(name:)` | inlined at `defer_flush(name)` |

### `defer_inline`: Inlined at exit points

The goto-patch model trades a `goto` + label round-trip for code deduplication. For tiny cleanup statements, the `goto` overhead is worse than just inlining the statement. `defer_inline` opts into the old emission model — cleanup is duplicated at every exit point.

```c
void locked_operation(mutex_t *m) {
    mutex_lock(m);
    defer_inline mutex_unlock(m);   // one instruction — cheaper to inline than to goto

    if (bad) return;                // emits: { mutex_unlock(m); return; }
    if (worse) return;              // emits: { mutex_unlock(m); return; }
    do_work();
}                                   // emits: mutex_unlock(m);
```

**When to use `defer_inline` vs `defer`:**
- `defer_inline` — single-expression cleanup (unlock, flag clear, counter decrement)
- `defer` — multi-statement cleanup, function calls, anything that bloats if duplicated

`defer_inline` supports channels: `defer_inline(err) flags &= ~IN_PROGRESS;`

The choice is purely an emission optimization — semantics are identical to `defer`.

### `defer_flush`: Explicit fire-and-consume

Sometimes you need to fire defers without returning — mid-function cleanup, resource recycling, or transitioning between phases. `defer_flush` fires and consumes pending defers at any point in the function body.

`defer_flush` is always **inlined at the call site** (not goto-patched). The developer explicitly asked to run cleanup here — this is wanted hot-path work, not cold error handling. Inlining keeps the end-of-function goto-patch clean and one-directional.

#### Syntax

| Construct | Meaning |
|---|---|
| `defer_flush;` | Fire and consume all active always-defers (LIFO), inline |
| `defer_flush(name);` | Fire and consume all active `name`-channel defers (LIFO), inline |

**"Consume" means the defers are removed from the defer stack after firing.** They will not fire again at `return` or scope exit. This is "flush the cleanup queue now."

#### Usage: resource recycling

```c
void process_files(const char **paths, int n) {
    for (int i = 0; i < n; i++) {
        FILE *f = fopen(paths[i], "r");
        defer fclose(f);

        char *buf = malloc(4096);
        defer free(buf);

        process(f, buf);

        defer_flush;            // fires: free(buf), fclose(f) — LIFO, inlined here
        // buf and f are now cleaned up, loop continues fresh
    }
    // no defers pending here — all consumed by defer_flush
}
```

#### Usage: phase transition with channels

```c
void pipeline(void) {
    int *scratch = malloc(1024);
    defer(setup) free(scratch);

    int *result = malloc(2048);
    defer(err) free(result);

    if (!init(scratch, result)) return(err:) ;   // goto-patch: free(result), free(scratch)

    // Setup phase complete — release setup resources, keep result
    defer_flush(setup);         // fires: free(scratch) only, inlined here
    // scratch is freed, result survives

    // ... use result ...
    return;                     // fires: nothing (no remaining always-defers)
}
```

#### Emitted code

```c
// Source:
FILE *f = fopen(path, "r");
defer fclose(f);
char *buf = malloc(4096);
defer free(buf);
process(f, buf);
defer_flush;

// Emitted (inlined at call site):
FILE *f = fopen(path, "r");
char *buf = malloc(4096);
process(f, buf);
{ free(buf); fclose(f); }       // inlined, LIFO
// defers consumed — return has nothing to fire
```

No goto, no labels, no resume point. The cleanup is inlined directly because the developer explicitly requested it. The end-of-function goto-patch stays clean — only `return`-triggered defers generate goto jumps.

#### Semantics

1. **Consume model:** Fired defers are removed. Subsequent `return` only fires defers registered *after* the `defer_flush` call.
2. **LIFO order:** Same as normal defer — most recently registered fires first.
3. **Scope-aware:** `defer_flush` only fires defers in the current scope and its parents, same as a `return` would.
4. **Channel-specific:** `defer_flush(name)` fires only defers tagged with `name`. Always-defers are NOT fired by `defer_flush(name)` — only `defer_flush` (no argument) fires always-defers.
5. **No-op safety:** `defer_flush` with no pending defers is a no-op (no error, no warning).
6. **Cannot appear in defer bodies:** `defer_flush` inside a `defer` body is a compile-time error (prevents infinite recursion in the cleanup chain).

#### Why `defer_flush` inlines and `return` goto-patches

`return`-triggered defers are almost always error paths — cold code that shouldn't pollute the icache. The goto-patch keeps them out of the hot path.

`defer_flush` is an explicit developer action — "I want cleanup to happen here, now." This is hot-path code by intent. Inlining it avoids the goto round-trip overhead and keeps the end-of-function cleanup patch purely one-directional (no backward jumps to resume points).

#### Why not just use a `{ }` scope?

Scoped defers already fire at `}`. But scope-based cleanup has two problems:
- It forces you to nest code inside extra `{ }` blocks, increasing indentation depth
- You can't selectively fire a channel — scope exit fires ALL defers in that scope

`defer_flush` gives explicit, flat, channel-aware control.

### Open questions

**Interleaved channel ordering:** When `defer` and `defer(err)` interleave, do error-channel defers fire in strict LIFO relative to all defers (preserving declaration order), or do they form a separate LIFO chain? The interleaved model is correct for resource cleanup (resources depend on allocation order), but the goto-patch cascade becomes more complex — it may require a conditional flag per entry rather than a simple label cascade.

**Scope-exit defers:** Current defer emits cleanup at `}` for nested scopes. The goto-patch model works naturally for function-level returns but scope-exit defers (not `return`, just leaving a `{ }`) may still need inline emission or a per-scope sub-patch.

**Return type inference:** `__prism_rv` needs a type. For functions with explicit return types, this is straightforward. For functions returning structs, the temp declaration needs `typeof` or the explicit struct type from the function signature.

---

## 2. Taint Qualifiers (`-ftaint`)

**Problem:** Direct dereference of untrusted pointers is a systemic bug class across C codebases — not just the Linux kernel (`__user`), but network daemons (recv buffers), embedded systems (MMIO registers), database engines (mmap'd pages), sandboxes (guest memory), and IPC (shared memory). Today, catching these requires either a separate static analysis tool (Sparse) or runtime instrumentation (ASAN). Most projects use neither.

**Design:** User-defined taint qualifiers via pragma, with compile-time enforcement of dereference safety.

### Declaration

```c
#pragma prism taint untrusted
#pragma prism taint mmio
#pragma prism taint guest
```

Or via CLI:

```
prism -ftaint=untrusted,mmio,guest
```

### Usage

```c
void handle_request(untrusted char *buf, size_t len) {
    char c = *buf;              // ERROR: direct dereference of 'untrusted' pointer
    char c = buf[0];            // ERROR: subscript dereference of 'untrusted' pointer
    char *p = buf;              // ERROR: taint stripped without boundary function
    untrusted char *q = buf;    // OK: taint preserved

    char local[256];
    safe_copy(local, buf, len); // OK: passed to function (not dereferenced)
    char c = local[0];          // OK: local is not tainted
}
```

### Core rule: taint can never be silently stripped

The enforcement model is **error-on-strip**, not dataflow tracking. No branch analysis, no assignment tracking, no CFG:

```c
untrusted char *buf;

*buf;                        // ERROR: dereference of tainted pointer
buf[0];                      // ERROR: subscript of tainted pointer
buf->field;                  // ERROR: member access on tainted pointer

char *p = buf;               // ERROR: stripping taint qualifier
untrusted char *q = buf;     // OK: taint preserved

safe_copy(local, buf, len);  // OK: passed as function argument (boundary crossing)
```

The assignment `char *p = buf;` is the error — not the later `*p`. This eliminates the aliasing bypass (developer strips annotation and checker goes blind) without requiring any branch or dataflow analysis.

### What about branches?

```c
untrusted char *buf;
char *p = default_ptr;
if (cond) { p = buf; }   // ERROR fires HERE: taint stripped at assignment
char c = *p;              // irrelevant — already caught above
```

No CFG needed. The error fires at the assignment site, not the dereference site. This is simpler and catches the alias bypass that a dereference-only check misses.

### Scope

Pure lexical enforcement within a function body. Does NOT track through:
- Function return values
- Cross-translation-unit propagation  
- `void *` casts (explicit unsafe boundary — developer's responsibility)

This is a **syntactic taint linter**, not a provenance tracker.

### Emitted output

The taint qualifier is stripped from emitted C. Optionally emitted as:
- `#define untrusted /* taint:untrusted */` (documentation)
- `__attribute__((address_space(N)))` (Sparse compatibility)

### Architecture fit

Extends existing infrastructure:
- Variable scanning: typedef table already tracks declarations per-scope with taint flags
- Taint check: same pattern as noreturn taint tracking — tag at declaration, check at use
- Dereference detection: `*ident`, `ident[`, `ident->` are trivial token patterns
- Assignment check: declaration scanner already sees `type *name = expr` — check if expr is tainted and type is not

### Real-world applicability

| Domain | Taint name | Protects against |
|---|---|---|
| Kernel | `__user` | User pointer dereference → privilege escalation |
| Kernel | `__iomem` | MMIO direct access → bus error / race |
| Network servers | `untrusted` | Recv buffer direct parse → injection |
| Embedded/RTOS | `mmio` | Register direct access → hardware fault |
| Databases | `mapped` | Direct mmap access → locking bypass |
| Sandboxes | `guest` | Guest memory access → sandbox escape |
| IPC | `foreign` | Shared memory direct use → TOCTOU |

---

## 3. Built-in `min` / `max` / `clamp` (`-fminmax`)

**Problem:** The standard C `#define min(x, y) ((x) < (y) ? (x) : (y))` evaluates arguments twice. Side effects (++, function calls) cause double-evaluation bugs. The Linux kernel's safe `min()` macro is 50+ lines of `_Generic`/`typeof`/statement-expression soup. Every C project either has this bug or has its own ugly workaround.

**Design:** Recognize `min(a, b)`, `max(a, b)`, and `clamp(val, lo, hi)` as built-in function-like identifiers with strict side-effect rejection.

### Usage

```c
int x = min(a, b);              // emits: ((a) < (b) ? (a) : (b))
int y = max(f(), g());          // ERROR: arguments have side effects
int z = clamp(val, 0, 255);     // emits: ((val) < (0) ? (0) : (val) > (255) ? (255) : (val))
int w = min(a++, b);            // ERROR: argument has side effects
```

### Side-effect detection

Reuses `reject_orelse_side_effects` scanner. Flags:
- Increment/decrement (`++`, `--`)
- Assignment (`=`, `+=`, etc.)
- Function calls (`ident(`)
- Volatile dereference

On rejection: hard error with actionable message:
```
error: arguments to min() have side effects; hoist to a temporary:
    int tmp = f(); int x = min(tmp, b);
```

### Why not auto-hoist?

Auto-hoisting into temps (via statement expressions or pre-statement declarations) breaks short-circuit evaluation. In `if (cond && min(f(), g()))`, pre-hoisting evaluates `f()` and `g()` unconditionally. Statement expressions fix this but are GNU-only (no MSVC). Strict rejection is the safe choice.

### Namespace collision

If the source already `#define`s `min`/`max`/`clamp`, Prism defers to the user's macro (same as `defer`/`orelse` — check typedef table, skip if user-defined).

---

## 4. Compiler Attribute Normalization (`-fnormalize-attrs`)

**Problem:** GCC, Clang, MSVC, and C23 all use different syntax for the same compiler attributes. Cross-platform C projects litter their headers with `#ifdef` chains.

**Design:** Write the canonical form, Prism emits the right syntax for the target compiler.

### Candidates

| Canonical | GCC/Clang | MSVC | C23 |
|---|---|---|---|
| `[[noreturn]]` | `__attribute__((noreturn))` | `__declspec(noreturn)` | `[[noreturn]]` |
| `[[deprecated]]` | `__attribute__((deprecated))` | `__declspec(deprecated)` | `[[deprecated]]` |
| `[[fallthrough]]` | `__attribute__((fallthrough))` | n/a | `[[fallthrough]]` |
| `[[maybe_unused]]` | `__attribute__((unused))` | n/a | `[[maybe_unused]]` |
| `[[nodiscard]]` | `__attribute__((warn_unused_result))` | `_Check_return_` | `[[nodiscard]]` |

### Status: Low priority

Prism already handles `_Noreturn` / `[[noreturn]]` / `__attribute__((noreturn))` / `__declspec(noreturn)` for its own noreturn analysis. Generalizing to all attributes is straightforward but low impact — the `#ifdef` boilerplate is annoying but not dangerous.

---

## 5. `sizeof` Array Parameter Decay Check (`-fsizeof-decay`)

**Problem:** When an array is passed as a function parameter, it decays to a pointer. `sizeof(arr)` then returns the pointer size, not the array size — a silent, catastrophic bug that every C beginner hits and many experienced developers still miss. GCC has `-Wsizeof-array-argument` but it's not in `-Wall`.

**Design:** Hard error when `sizeof` is applied to a parameter that was declared with array syntax.

### Detection

```c
void process(int arr[], size_t n) {
    // Phase 1 sees: parameter 'arr' declared as 'int arr[]' (array syntax)
    size_t len = sizeof(arr);              // ERROR: sizeof on decayed array parameter 'arr'
    size_t elem = sizeof(arr) / sizeof(arr[0]); // ERROR: same
}

void ok(int *arr, size_t n) {
    size_t len = sizeof(arr);  // OK: declared as pointer, developer knows what they're getting
}
```

### Rules

1. In function parameter list, if an identifier is declared with `[]` or `[N]` syntax, tag it as "array-declared parameter"
2. In the function body, if `sizeof(ident)` or `sizeof ident` appears where `ident` is tagged, emit error:
   ```
   error: sizeof() on array parameter 'arr' returns pointer size, not array size;
          use an explicit size parameter instead
   ```
3. `sizeof(arr[0])` (element size) is allowed — the subscript dereference produces the element type, not the array
4. `sizeof(*arr)` is allowed — same reason

### Architecture fit

- Parameter scanning: Phase 1 already walks function parameter lists for type parsing
- Token check: `sizeof` + `(` + tagged-ident + `)` is a trivial pattern
- Zero false positives: the C standard guarantees array parameters decay to pointers

### Why this matters

This is possibly the most common C bug that compilers don't warn about loudly enough. Stack Overflow has thousands of questions about it. It causes buffer overflows, truncated reads, and wrong-size allocations — all silently.

---

## 6. Mandatory Control-Flow Braces (`-fmandate-braces`)

**Problem:** Braceless `if`/`for`/`while` bodies are the root cause of the Apple `goto fail` vulnerability (CVE-2014-1266). A developer adds a second indented statement expecting it to belong to the `if`, but it executes unconditionally.

```c
if (condition)
    check_something();
    do_critical_thing();   // always executes — indentation is a lie
```

**Design:** When enabled, any braceless control-flow body is a hard error.

### Implementation

Prism already tracks braceless control flow via `ctrl_state.pending` (set by `TT_IF`, `TT_LOOP`, `TT_SWITCH`). When the flag is enabled and `ctrl_state.pending` is true, the next non-noise token must be `{`. If it isn't:

```
error: braceless control flow is forbidden (-fmandate-braces);
       wrap statement in { }
```

### Exceptions

- `else if` chains: `else if (x) {` is allowed (the `if` immediately follows `else`)
- Single-line macros that expand to `{ ... }` aren't visible post-preprocessing — but the flag is opt-in, so projects that hit false positives from macros can disable it

### Architecture fit

`ctrl_state.pending` already exists in Pass 2. The check is a 4-line gate on the existing code path that injects braces for braceless bodies.

---

## 7. Strict Implicit Fallthrough Ban (`-fno-fallthrough`)

**Problem:** Missing `break` in `switch` cases causes silent execution bleed-through. This is one of the most common C bugs — CWE-484 (Omitted Break Statement in Switch). GCC/Clang have `-Wimplicit-fallthrough` but it's not universally in `-Wall`, and MSVC lacks it entirely.

```c
switch (state) {
    case INIT:
        start_engine();
        // forgot break — falls through silently
    case RUNNING:
        update_engine();   // executes when state == INIT too
        break;
}
```

**Design:** When enabled, every `case`/`default` label must be preceded by a terminating statement (`break`, `return`, `continue`, `goto`, `_Noreturn` function call) or be an empty fallthrough (`case X: case Y:`). The C23 `[[fallthrough]]` attribute explicitly opts into intentional fallthrough.

### Detection

Phase 1D already tracks `P1K_CASE` and `P1K_DEFAULT`. On encountering a new `case`/`default`:

1. Scan backward from the `:` to find the previous statement's terminator
2. Skip over nested `{ }` blocks when scanning (a `return` inside a nested block within the case counts)
3. If no terminator found and the case is non-empty (has statement-producing tokens), error:
   ```
   error: implicit fallthrough from 'case INIT' to 'case RUNNING' (-fno-fallthrough);
          add 'break;' or '[[fallthrough]];' if intentional
   ```

### Allowed patterns

```c
case 1: case 2:         // OK: empty fallthrough (grouping cases)
    handle_both();
    break;

case 3:
    handle_three();
    [[fallthrough]];     // OK: explicit annotation
case 4:
    handle_four();
    break;

case 5:
    return;              // OK: return terminates

case 6: {
    if (x) return;
    break;               // OK: break inside nested block
}
```

### Architecture fit

Phase 1D tracks case/default positions. The backward scan for terminators is the same kind of look-behind Prism already does for defer shadow checking and label resolution. The `[[fallthrough]]` attribute detection reuses existing C23 attribute recognition.

---

## 8. Forward-Only `goto` Enforcement (`-fstrict-goto`)

**Problem:** Backward `goto` creates unstructured loops that defeat human comprehension, static analysis, and code review. In modern C, `goto` is considered acceptable only for forward jumps to cleanup labels. Backward `goto` is spaghetti code — use a real loop construct.

```c
retry:
    result = try_operation();
    if (result == RETRY)
        goto retry;        // ERROR: backward goto — use while/for loop
```

**Design:** When enabled, any `goto` that jumps to a label appearing earlier in the function is a hard error.

### Implementation

The CFG verifier (`p1_verify_cfg`) already computes the topological direction of every `goto` relative to its label. A backward `goto` is one where the label index `li` satisfies `li < goto_index`. When the flag is enabled:

```
error: backward goto to 'retry' is forbidden (-fstrict-goto);
       use a loop construct (while, for, do-while)
```

### What remains allowed

```c
    if (init_failed)
        goto cleanup;      // OK: forward goto to cleanup

    // ... normal code ...

cleanup:
    free(resources);
    return -1;
```

### Architecture fit

Zero new scanning logic required. The label direction check already exists in `p1_verify_cfg` for VLA scope validation. The flag simply converts a "this is a backward goto" fact that Prism already knows into a hard error.

---

## 10. Bounds Checking — Future Tiers

Tiers 1 and 2 (fixed-size local arrays and VLAs) have shipped as `-fbounds-check`; see **SPEC.md §6.10**. The remaining work is tracked here.

### Tier 3: Function parameters (annotation)

Array parameters decay to pointers — the size is lost. The developer annotates the bound:

```c
// Source:
void fill(int arr[bounds(n)], size_t n) {
    for (size_t i = 0; i < n; i++)
        arr[i] = 0;
    arr[n] = 0;  // caught at runtime
}

// Emitted (annotation stripped):
void fill(int *arr, size_t n) {
    for (size_t i = 0; i < n; i++)
        arr[__prism_bchk(i, n)] = 0;
    arr[__prism_bchk(n, n)] = 0;  // TRAP
}
```

The `bounds(expr)` annotation lives inside the array brackets — valid declarator position, stripped from emitted C. The `expr` is any expression visible at the function scope (typically a size parameter).

C99 `[static N]` is also recognized for constant bounds:
```c
void process(int arr[static 10]) {
    arr[9] = 1;   // OK
    arr[10] = 1;  // TRAP
}
```

Tier 3 requires per-parameter tracking for the `bounds(expr)` size expression:

```
BoundsParamEntry {
    Token *name;           // parameter identifier token
    Token *size_start;     // first token of bounds(expr)
    Token *size_end;       // last token of bounds(expr)
    uint32_t scope_open;   // function body '{' token index
    uint32_t scope_close;  // function body '}' token index
    uint8_t ndim;          // number of dimensions
    bool is_param : 1;     // always true for this table
}
```

Registration happens in `process_declarators` when a parameter has `bounds(...)` annotation. Lookup happens in the Pass 2 emit loop when emitting `TK_IDENT` + `[`.

### Further future tiers

- **Inner dimensions of multi-dim subscripts:** `m[i][j]` currently only checks the outer `i`. Inner `j` is not wrapped because `last_emitted` at the inner `[` is `]`, not an identifier. A future pass could re-walk the preceding subscript chain to derive the inner dimension's `sizeof` ratio.
- **Pointer arithmetic:** `*(arr + i)` → check `i` against arr's bounds. Requires recognizing `arr + expr` as a bounds-relevant pattern.
- **Struct field arrays via pointer:** `p->data[i]` where `p` is a pointer to a struct with a known-size `data` field. Requires struct definition scanning. Local struct instances (`pkt.data[i]`) are already covered once Tier 1 is extended to follow struct-member access.
- **Return value annotation:** `int *get_buffer(size_t *out_len) bounds(*out_len)` — annotate that the returned pointer has bounds tied to an output parameter.
- **Propagated bounds:** When `int *p = arr;` is detected, carry arr's bounds to `p` within the same scope. Limited dataflow, but catches the most common alias pattern.

### Why not `slice(T)` fat pointers?

An alternative design (inspired by Rust's `&[T]`) would introduce a `slice(T)` type that bundles a pointer with its length — a "fat pointer" that carries bounds across function boundaries.

This is a non-starter for Prism:
- **ABI break:** `slice(int)` is a struct, not an `int *`. Every function signature changes. Every existing C API needs a wrapper.
- **New type system:** Slice types need their own rules for assignment, comparison, arithmetic. Prism is a transpiler, not a language.
- **Conversion ceremony:** Every interaction with legacy code requires explicit `slice_from(ptr, len)` / `slice_ptr(s)` conversion — the exact ceremony Rust developers complain about.

The `bounds(n)` annotation is superior: the function signature stays `int *arr` in emitted C. The ABI doesn't change. Existing C code calls the function without modification. Only the function body gets bounds checks injected.

---

## 12. Orelse Postcondition Injection

**Problem:** After `int *p = malloc(100) orelse default_buf;`, the developer knows `p` is guaranteed non-null (orelse provides a fallback). But the backend compiler doesn't — it sees a ternary expression and can't prove the result is non-null. Every subsequent `if (!p)` check and null-pointer sanitizer branch is wasted.

**Design:** After orelse expansion, inject `__builtin_assume(result != 0)` to communicate the postcondition to the backend.

### Usage

```c
// Source:
int *p = malloc(100) orelse (int *)fallback_buf;

// Current emission (simplified):
int *p = (malloc(100)) ? (malloc(100)) : ((int *)fallback_buf);
// (actual emission uses a temp to avoid double-eval)

// With postcondition:
int *__prism_tmp = malloc(100);
int *p = __prism_tmp ? __prism_tmp : (int *)fallback_buf;
__builtin_assume(p != ((void*)0));
```

### What the backend gains

- All subsequent `if (p == NULL)` checks in the same scope are dead-code-eliminated
- Null-pointer sanitizer instrumentation on `*p` is removed
- The compiler can avoid null-check register pressure — it knows `p` is live and valid

### Scope

Only inject when the orelse fallback is a non-null expression:
- Literal value: `orelse 0` → `__builtin_assume(result != 0)` (for integers)
- Address expression: `orelse buf` → `__builtin_assume(result != NULL)` (for pointers)
- Block: `orelse { return -1; }` → no postcondition (control flow, not a value)

Skip injection when the fallback could itself be null/zero (e.g., `orelse other_ptr` where `other_ptr` could be null).

### Why only Prism can do this

The compiler sees a ternary — it doesn't know the developer's intent was "guarantee a valid fallback." Prism understands the `orelse` contract: "if the LHS evaluates to a falsy value, substitute the RHS." If the RHS is a non-zero constant or known-valid address, the result is guaranteed non-zero.

### Architecture fit

- Orelse expansion already happens in Pass 2
- The postcondition is emitted immediately after the declaration, same emit point
- `__builtin_assume` is GCC/Clang; MSVC uses `__assume` — Prism already handles this split
- Zero overhead: `__builtin_assume` generates no code, it's purely an optimizer hint

---

## 13. Const-to-Literal VLA Demotion

**Problem:** In C (unlike C++), `const int N = 10;` does not create a constant expression — `N` is a variable with a `const` qualifier. Using it as an array dimension creates a VLA, which forces the compiler to dedicate the frame pointer register, emit `alloca`-style allocation, and generate VLA cleanup code. This is a well-known C/C++ gap that bites every C developer who writes:

```c
void process(void) {
    const int N = 10;
    int arr[N];           // VLA in C, fixed array in C++
}
```

**Design:** When an array dimension is a single identifier that resolves to a `const`-qualified local initialized with a compile-time constant literal, substitute the literal value at the array declaration.

### Usage

```c
// Source:
void process(void) {
    const int N = 10;
    int arr[N];
}

// Emitted:
void process(void) {
    const int N = 10;
    int arr[10];          // fixed array — no VLA overhead
}
```

### Detection rules

1. Array dimension is a single `TK_IDENT` (no operators, no function calls)
2. Identifier resolves in the typedef table to a local variable with `is_const = true`
3. The variable's initializer is a single `TK_NUM` literal (no expressions, no identifiers)
4. The variable is declared in the same scope or an enclosing scope

On match: substitute the `TK_NUM` value for the `TK_IDENT` in the dimension.

### What this eliminates

- Frame pointer register dedication (frees a GPR for the hot path)
- `alloca`-style runtime allocation code
- VLA cleanup code at scope exit
- GCC's `-Wvla` warning (the emitted code has no VLA)
- Interaction with `-fno-vla` flag (the emitted code is legal)

### Edge cases

- `const int N = sizeof(something);` — NOT demoted (`sizeof` is a compile-time constant but involves a non-literal expression; the compiler handles this fine)
- `const int N = 10 + 5;` — NOT demoted (expression, not a single literal). Conservative is correct.
- `const int N = -1;` — NOT demoted (negative array size is a constraint violation)
- `const int N = 0;` — NOT demoted (zero-length array is a constraint violation or GNU extension)
- `static const int N = 10;` — NOT demoted (static storage is visible across the TU; technically safe but overly broad)

### Architecture fit

- The typedef table already tracks `is_const` for local declarations
- `array_size_is_vla` already identifies the dimension token — if it's a single `TK_IDENT`, look it up
- The substitution is a single token replacement in the emit loop
- Conservative rules ensure zero false positives

---

15. Out-of-Line Assembly Extraction (-fnaked-asm)
Problem: Standard C inline assembly (__asm__ volatile (...)) is the least portable feature in the C ecosystem. It blinds the compiler's optimizer, requires complex register constraint boilerplate, and fragments codebases between AT&T syntax (GCC/Clang) and Intel syntax (MSVC). Maintaining separate assembly files and C header definitions to bypass this is a massive boilerplate tax.

Design: Introduce a naked_asm block. Prism extracts the raw assembly strings, automatically generates a parallel assembly file with the correct directives for the target compiler's native assembler (GNU AS or MASM), and replaces the block in the C output with a clean extern prototype.

Usage
C
// User's pure C file (main.c):
#include <stdio.h>

naked_asm void my_custom_dispatcher(void) {
    "add spl, 8\n"
    "jmp qword ptr [rsp]\n"
}

int main(void) {
    my_custom_dispatcher();
    return 0;
}
Prism's Dual Output
1. Emitted C File (main.c.tmp):
Prism completely strips the assembly block and replaces it with a standard ISO C forward declaration. This guarantees 100% portability to any C compiler. The backend compiler treats it as an opaque external function, preventing it from flushing registers to RAM or panicking about clobbers.

C
#include <stdio.h>

extern void my_custom_dispatcher(void);

int main(void) {
    my_custom_dispatcher();
    return 0;
}
2. Synthesized Assembly File (Backend-Aware):
Prism uses its existing compiler detection (cc_is_msvc, cc_is_clang) to wrap the exact strings the user wrote in the correct native assembler directives.

If targeting GCC / Clang / TCC (Generates prism_extracted.S):

Code snippet
#if defined(__APPLE__)
    #define SYM(x) _##x
#else
    #define SYM(x) x
#endif

.intel_syntax noprefix
.text
.globl SYM(my_custom_dispatcher)
SYM(my_custom_dispatcher):
    add spl, 8
    jmp qword ptr [rsp]
If targeting MSVC (Generates prism_extracted.asm):

Code snippet
PUBLIC my_custom_dispatcher
.code
my_custom_dispatcher PROC
    add spl, 8
    jmp qword ptr [rsp]
my_custom_dispatcher ENDP
END
Architecture Fit
Detection: In Pass 2, when the naked_asm keyword is encountered, Prism parses the function signature, then loops through the { ... } block capturing all TK_STR (string literal) tokens.

Dual-Stream Emission: Prism introduces an asm_fp alongside the standard out_fp. The function signature is emitted to out_fp as extern ... ;, and the strings are stripped of their quotes and written directly to asm_fp.

The Pipeline: Prism's compile_sources already knows how to invoke the backend compiler with multiple files. If asm_fp has content, Prism dumps it to a temporary .S or .asm file and appends it to the compile_argv array.

Universal Build Routing: * On GCC/Clang, gcc main.c prism_extracted.S delegates to GNU AS.

On MSVC, Prism automatically invokes ml64.exe (MASM) on the .asm file to produce an .obj, then passes that object file to cl.exe alongside the C source.

Core Directives Maintained
This respects Prism's core architectural constraint: We do not parse expressions. Prism doesn't need an x86 opcode table. It treats the assembly exactly as it treats C: as raw tokens to be macro-structurally reorganized and forwarded to the appropriate backend tool.

Impact
The developer gets the absolute raw power and zero-overhead of bare-metal Intel assembly, but the developer experience is as seamless as writing a standard C function in a single file.

No .h header files to keep in sync.

100% portability across any C compiler (GCC, Clang, MSVC, TCC), because the C compiler only ever sees ISO C.

The agonizing assembler directive fragmentation (.globl vs PUBLIC, .text vs .code) is completely absorbed by the transpiler.

---
## Priority Assessment

| Feature | Bug severity | Arch fit | Effort | Priority |
|---|---|---|---|---|
| Defer 2.0 (channels + goto-patch) | High (resource leaks + icache bloat) | High (extends existing defer infra) | Medium | **1** |
| Bounds checking | Critical (CWE-787/125, #1 exploit class) | High (declaration scanner + emit loop) | Medium | **2** |
| Taint qualifiers | Critical (security) | High (extends existing taint infra) | Medium | **3** |
| min/max/clamp | Medium (double-eval bugs) | High (reuses orelse scanner) | Low | **4** |
| sizeof decay check | Medium (silent wrong results) | High (trivial token pattern) | Very low | **5** |
| Mandatory braces | Medium (CVE-2014-1266 class) | High (ctrl_state exists) | Very low | **6** |
| Fallthrough ban | Medium (CWE-484 class) | High (Phase 1D case tracking) | Low | **7** |
| Forward-only goto | Low (code quality) | High (CFG verifier has it) | Near zero | **8** |
| Attribute normalization | None (convenience) | High (trivial) | Low | **9** |
| Auto-static const arrays | High (eliminates hidden memcpy) | Very high (trivial token scan) | Low | **10** |
| Unreachable after noreturn | ~~Medium~~ | ~~Very high~~ | ~~Near zero~~ | **DONE** |
| Orelse postcondition | Low (missed optimizations) | Very high (orelse semantics) | Near zero | **12** |
| Const-to-literal VLA demotion | Medium (wastes frame pointer GPR) | Very high (typedef table lookup) | Very low | **13** |

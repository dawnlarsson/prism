![Prism Banner](https://github.com/user-attachments/assets/051187c2-decd-497e-9beb-b74031eb84ed)

![License](https://img.shields.io/badge/license-Apache_2.0-blue) ![Language](https://img.shields.io/badge/language-C-lightgrey) ![Tests](https://img.shields.io/badge/tests-3377_passing-brightgreen) ![Zero deps](https://img.shields.io/badge/dependencies-0-brightgreen)

## Robust C by default
**A dialect of C with `defer`, `orelse`, and automatic zero-initialization.**

Prism is a lightweight and very fast transpiler that makes C safer without changing how you write it.

- **3377 tests** — edge cases, control flow, nightmares, trying hard to break Prism
- **Building Real C** — OpenSSL, SQLite, Bash, GNU Coreutils, Make, Curl
- **Two-pass transpiler** — full semantic analysis before a single byte is emitted
- **Opt-out features** — Disable parts of the transpiler, like zero-init, with CLI flags
- **Drop-in overlay** — Use `CC=prism` in any build system — GCC-compatible flags pass through automatically
- **Single Repo** — zero dependencies, easy to audit, only need a C compiler

Prism is a proper transpiler, not a preprocessor macro.
* **Track Types:** Pass 1 walks every token at every depth, registering every `typedef`, `enum` constant, parameter shadow, and VLA tag into an immutable symbol table — no heuristics, no suffix guessing. If it wasn't declared, it's not a type
* **Respect Scope:** A full scope tree maps every `{`/`}` pair with parent links and classification (loop, switch, conditional, function body, statement expression). `defer` fires exactly when it should — no state machines in the emitter
* **Detect Errors Early:** A CFG verifier checks every `goto`→label and `switch`→`case` pair against defers and declarations **before** code generation starts. If your code is unsafe, Prism errors before writing a single byte

## Quick Start

### Linux / macOS
```sh
cc prism.c -flto -s -O3 -o prism && ./prism install
```

### Windows (MSVC)
Open a **Developer Command Prompt** (or run `vcvars64.bat`) and build:
```sh
cl /Fe:prism.exe prism.c /O2 /D_CRT_SECURE_NO_WARNINGS /nologo
```
Requires Visual Studio Build Tools with the **Desktop development with C++** workload.

## Built with Prism
Real codebases written using Prism as the compiler.

### [Terminal File Explorer](https://github.com/dawnlarsson/explore)
[<img width="1113" height="412" alt="image 105 (2)" src="https://github.com/user-attachments/assets/88c9e447-7153-4ad0-ab7e-fd9fedbb3f88" />](https://github.com/dawnlarsson/explore)

### [Refract - Linux Kernel & Distro](https://github.com/dawnlarsson/refract)

## Defer

**The problem:** C requires manual cleanup at every exit point. Each new resource adds cleanup to *every* error path. Miss one and you leak.

```c
// Standard C — cleanup grows with every new resource
int compile(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char *src = read_file(f);
    if (!src) {
        fclose(f); 
        return -1; 
    }

    Token *tok = tokenize(src);
    if (!tok) { 
        free(src); 
        fclose(f); 
        return -1; 
    }

    Node *ast = parse(tok);
    if (!ast) { 
        token_free(tok); 
        free(src); 
        fclose(f); 
        return -1; 
    }

    int result = emit(ast);
    node_free(ast);    // remember all four
    token_free(tok);   // in the right order
    free(src);         // or you leak
    fclose(f);         // every single time
    return result;
}
```

**With Prism:** Write cleanup once. It runs on every exit.

```c
int compile(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    defer fclose(f);

    char *src = read_file(f);
    if (!src) return -1;           // fclose runs
    defer free(src);

    Token *tok = tokenize(src);
    if (!tok) return -1;           // free, fclose run
    defer token_free(tok);

    Node *ast = parse(tok);
    if (!ast) return -1;           // token_free, free, fclose run
    defer node_free(ast);

    return emit(ast);              // all four, reverse order
}
```
It is better, but we can take it further, see `orelse` section.


Defers execute in **LIFO order** (last defer runs first) at scope exit — whether via `return`, `break`, `continue`, `goto`, or reaching `}`.

**Edge cases handled:**
- Statement expressions `({ ... })` — defers fire at inner scope, not outer
- `switch` fallthrough — defers don't double-fire between cases  
- Nested loops — `break`/`continue` unwind the correct scope
- Computed goto — `goto *ptr` with active defers is a hard error

**Forbidden patterns:** Functions using `setjmp`/`longjmp`/`pthread_exit`, `vfork`, or inline assembly are rejected to prevent resource leaks from non-local jumps.

**Opt-out:** `prism -fno-defer src.c`

## Zero-Init

**The problem:** Uninitialized reads are the #1 source of C vulnerabilities. Compilers don't require initialization, and `-Wall` only catches obvious cases.

```c
// Standard C — compiles fine, undefined behavior at runtime
int sum_positive(int *arr, int n) {
    int total;  // uninitialized — could be anything
    for (int i = 0; i < n; i++)
        if (arr[i] > 0) total += arr[i];
    return total;  // UB: total was never set if no positives
}
```

**With Prism:** All locals start at zero. The above code just works.

```c
void example() {
    int x;                            // 0
    char *ptr;                        // NULL  
    int arr[10];                      // {0, 0, ...}
    struct { int a; float b; } s;     // {0, 0.0}
}
```

**Typedef tracking:** Before code generation, Pass 1 walks the entire preprocessed token stream at all depths — not just file scope — to build a complete, immutable symbol table of every `typedef`, `enum` constant, parameter shadow, and VLA tag. This is deterministic — `size_t`, `pthread_mutex_t`, and every other typedef from system headers are resolved by name lookup, not pattern matching. This distinguishes `size_t x;` (declaration → initialize) from `size_t * x;` (expression → don't touch).

**VLA support:** Variable-length arrays get `memset` at runtime.

**Opt-out:** `prism -fno-zeroinit src.c` or per-variable with `raw`.

## Raw

The `raw` keyword opts out of zero-initialization for a specific variable.

```c
void example() {
    raw int x;             // Uninitialized
    raw char buf[65536];   // No memset overhead
    raw struct large data; // Skip zeroing
}
```

**When to use:**
- Large buffers that will be immediately overwritten (`read()`, `recv()`)
- Performance-critical inner loops where zeroing is measurable overhead
- Interfacing with APIs that fully initialize the data

**Safety interaction:** Variables marked `raw` can be safely jumped over by `goto` — since they're not initialized anyway, skipping them isn't undefined behavior. Exception: `raw` on a VLA does not exempt it from the goto check, because jumping past a VLA bypasses implicit stack allocation regardless of initialization.

```c
void allowed() {
    goto skip;
    raw int x;  // OK: raw opts out of initialization
skip:
    return;
}
```

## orelse

The `orelse` keyword handles failure inline — check a value and bail in one line.

`defer` solved the cleanup problem, but notice the function still has a repetitive pattern: call, null-check, bail. Four times. `orelse` collapses each check-and-bail into the declaration itself:

```c
int compile(const char *path) {
    FILE *f = fopen(path, "r") orelse return -1;
    defer fclose(f);

    char *src = read_file(f) orelse return -1;
    defer free(src);

    Token *tok = tokenize(src) orelse return -1;
    defer token_free(tok);

    Node *ast = parse(tok) orelse return -1;
    defer node_free(ast);

    return emit(ast);
}
```

Same function, three versions: **32 lines → 19 lines → 15 lines.** No cleanup bugs, no null-check boilerplate.

`orelse` checks if the initialized value is falsy (null pointer, zero). If so, the action fires. All active defers run — just like a normal `return`.

### Forms

**Control flow** — return, break, continue, goto:
```c
int *p = get_ptr() orelse return -1;
int *q = next()    orelse break;
int *r = try_it()  orelse continue;
int *s = find()    orelse goto cleanup;
```

**Block** — run arbitrary code on failure:
```c
FILE *f = fopen(path, "r") orelse {
    log_error("failed to open %s", path);
    return -1;
}
```

**Fallback value** — substitute a default:
```c
char *name = get_name() orelse "unknown";
```

**Bare expression** — check without assignment:
```c
do_init() orelse return -1;
```

### Works with any falsy value

`orelse` isn't limited to pointers — it works with any type where `!value` is meaningful:

```c
int fd = open(path, O_RDONLY) orelse return -1;  // 0 is falsy
size_t n = read_data(fd, buf) orelse break;      // 0 bytes = done
```

### Limitation: struct/union values

`orelse` does not **currently** support struct or union **values** — it is a compile error:

```c
struct Vec2 { int x, y; };

struct Vec2 v = make_vec2() orelse return -1;  // Error
```

The reason: `orelse` works by testing `!value`, which is well-defined for scalars and pointers but not for structs. A whole-struct zero check would require `memcmp`, which can give false negatives due to padding bytes.

Struct and union **pointers** work fine:

```c
struct Vec2 *p = get_vec2() orelse return -1;  // OK — pointer is scalar
```

> **Note:** This protection only works for explicit `struct` / `union` keywords.
> If the struct is hidden behind a `typedef`, Prism has no way to detect it
> (it does not have a full type-checker). The code will be emitted and the
> backend C compiler will report a less helpful error such as
> *"used struct type value where scalar is required"*.
>
> ```c
> typedef struct { int x, y; } Vec2;
> Vec2 v = make_vec2() orelse return -1;  // Passes Prism, fails at CC
> ```

**Opt-out:** `prism -fno-orelse src.c`

## Safety Enforcement
Prism acts as a static analysis tool, turning common C pitfalls into compile-time errors — all before a single byte of output is emitted.

### No Uninitialized Jumps
Standard C allows `goto` to skip variable initialization, leading to undefined behavior. Prism's CFG verifier checks every `goto`→label pair in an O(N) linear sweep:

```c
// THIS WILL FAIL TO COMPILE
void unsafe() {
    goto skip;
    int x; // Prism guarantees x is zero-initialized
skip:
    printf("%d", x);
}
// Error: goto 'skip' would skip over variable declaration 'x'
```

The same analysis covers `switch`/`case` — jumping from one case into a nested block that has zero-initialized declarations or active defers is rejected:

```c
void bad_switch(int n) {
    switch (n) {
        case 1: {
            defer cleanup();
            // ...
        }
        case 2:  // Error: defer skipped by switch fallthrough
            break;
    }
}
```

### Defer in Forbidden Contexts

Prism rejects `defer` in functions that use non-local control flow:

```c
void bad() {
    jmp_buf buf;
    defer cleanup();  // Error: defer forbidden with setjmp
    if (setjmp(buf)) return;
}
```

This prevents resource leaks when `longjmp` bypasses defer cleanup.

### Downgrade to Warnings

Use `-fno-safety` to turn safety errors into warnings (for gradual adoption):

```sh
prism -fno-safety legacy.c  # Compiles with warnings instead of errors
```

## Multi-File & Passthrough

Prism handles real-world build scenarios:

```sh
# Multiple source files
prism main.c utils.c -o app

# Mix with assembly
prism main.c boot.s -o kernel

# C++ files pass through untouched (uses g++/clang++ automatically)
prism main.c helper.cpp -o mixed
```

**Passthrough files:** `.s`, `.S` (assembly), `.cc`, `.cpp`, `.cxx`, `.mm` (C++), `.m` (Objective-C) are passed directly to the compiler without transpilation.

## Error Reporting

Prism emits `#line` directives so compiler errors point to your original source, not the transpiled output:

```
main.c:42:5: error: use of undeclared identifier 'foo'
```

Not:
```
/tmp/prism_xyz.c:1847:5: error: use of undeclared identifier 'foo'
```

**Disable:** `prism -fno-line-directives src.c` (useful for debugging transpiler output)

## CLI

Prism uses a GCC-compatible interface — most flags pass through to the backend compiler.

```sh
Prism v0.120.0 - Robust C transpiler

Usage: prism [options] source.c... [-o output]

Commands:
  run <src.c>           Transpile, compile, and run
  transpile <src.c>     Output transpiled C to stdout
  install [src.c...]    Install prism to /usr/local/bin/prism

Prism Flags (consumed, not passed to CC):
  -fno-defer            Disable defer
  -fno-zeroinit         Disable zero-initialization
  -fno-orelse           Disable orelse keyword
  -fno-line-directives  Disable #line directives
  -fno-safety           Safety checks warn instead of error
  -fflatten-headers     Flatten headers into single output
  -fno-flatten-headers  Disable header flattening
  --prism-cc=<compiler> Use specific compiler
  --prism-verbose       Show commands

All other flags are passed through to CC.

Examples:
  prism foo.c -o foo               Compile (GCC-compatible)
  prism run foo.c                  Compile and run
  prism transpile foo.c            Output transpiled C
  prism -O2 -Wall foo.c -o foo     With optimization
  CC=clang prism foo.c             Use clang as backend

Apache 2.0 license (c) Dawn Larsson 2026
https://github.com/dawnlarsson/prism
```

### Drop-in Compiler Overlay

Prism can replace `gcc` or `clang` in any build system:

```sh
# Instead of:
CC=gcc make

# Use:
CC=prism make
```

All standard compiler flags (`-O2`, `-Wall`, `-I`, `-L`, `-l`, etc.) pass through automatically to the backend compiler.

## Library Mode

Prism can be compiled as a library for embedding in other tools:

```sh
# Compile as library (excludes CLI)
cc -DPRISM_LIB_MODE -c prism.c -o prism.o
```

API:
```c
PrismFeatures prism_defaults(void);
PrismResult   prism_transpile_file(const char *path, PrismFeatures features);
void          prism_free(PrismResult *r);
```

## Architecture

Prism processes C in two passes. Pass 1 performs full semantic analysis and catches all user-triggerable errors. Pass 2 is a near-pure code generator that reads Pass 1's immutable artifacts — no type table mutations, no speculative token walking. Defensive assertions in Pass 2 guard against internal consistency violations but are not reachable from valid or invalid user input.

| Phase | What it does |
|---|---|
| **Pass 0** — Tokenizer | Tokenize, delimiter-match, keyword-tag, build setjmp/vfork/asm taint graph per function |
| **Pass 1A** — Scope Tree | Walk all tokens, assign scope IDs, build parent chain, classify each `{` (loop/switch/conditional/function/struct) |
| **Pass 1B** — Type Registration | Full-depth `typedef`, `enum`, VLA tag registration at all scopes — symbol table frozen after this point |
| **Pass 1C** — Shadow Table | Record every variable that shadows a typedef, with scope ID and token index for temporally-correct lookup |
| **Pass 1D** — CFG Collection | Per-function arrays of labels, gotos, defers, declarations, switch/case entries |
| **Pass 1E** — Return Type Capture | Record each function's return type range and void/setjmp/vfork/asm flags |
| **Pass 1F** — Defer Validation | Reject forbidden patterns inside defer bodies (return, goto, break, continue, nested stmt-expr) |
| **Pass 1G** — Orelse Pre-Classification | Classify orelse in brackets and declaration initializers; reject in enum bodies and at file scope |
| **Phase 2A** — CFG Verification | O(N) snapshot-and-sweep: verify every goto→label and switch→case pair against defers and declarations |
| **Pass 2** — Code Generation | Emit transformed C. Reads immutable scope tree, typedef table, shadow table. No type mutations, no safety checks |

**Key invariant:** Every semantic error is raised before Pass 2 emits its first byte. If code generation starts, it runs to completion.

see `.github/SPEC.md` for full breakdown.

## Get in touch

available for consulting work, (design, branding, engineering / software)

→ dawn@dawn.day · [dawning.dev](https://dawning.dev)

# Repo
Apache 2.0 license (c) Dawn Larsson 2026

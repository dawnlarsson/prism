![Prism Banner](https://github.com/user-attachments/assets/051187c2-decd-497e-9beb-b74031eb84ed)

## Robust C by default
**A dialect of C with `defer`, `orelse`, and automatic zero-initialization.**

Prism is a lightweight and very fast transpiler that makes C safer without changing how you write it.

- **1354 tests** — edge cases, control flow, nightmares, trying hard to break Prism
- **Building Real C** — OpenSSL, SQLite, Bash, GNU Coreutils, Make, Curl
- **Proper transpiler** — tracks typedefs, respects scope, catches unsafe patterns
- **Opt-out features** Disable parts of the transpiler, like zero-init, with CLI flags
- **Drop-in overlay** Use `CC=prism` in any build system — GCC-compatible flags pass through automatically
- **Single Repo** — 7k lines, zero dependencies, easy to audit

Prism is a proper transpiler, not a preprocessor macro.
* **Track Types:** It parses `typedef`s to distinguish pointer declarations from multiplication (the "lexer hack"), ensuring correct zero-initialization.
* **Respect Scope:** It understands braces `{}`, statement expressions `({ ... })`, and switch-case fallthrough, ensuring `defer` fires exactly when it should.
* **Detect Errors:** It catches unsafe patterns (like jumping into a scope with `goto`) before they become runtime bugs.

## Quick Start

### Linux / macOS
```sh
cc prism.c -flto -s -O3 -o /tmp/prism && /tmp/prism install && rm /tmp/prism
```

### Windows (MSVC)
Open a **Developer Command Prompt** (or run `vcvars64.bat`) and build:
```sh
cl /Fe:prism.exe prism.c /O2 /D_CRT_SECURE_NO_WARNINGS /nologo
```
Requires Visual Studio Build Tools with the **Desktop development with C++** workload.

> **Note:** Prism builds and runs natively on Windows. The transpiler output is GCC/Clang-first — features like `__auto_type` in `defer` return values are not yet MSVC-compatible. Use `CC=gcc` or `CC=clang` as the backend compiler on Windows for full transpiled-code support.

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

**With Prism:** Write cleanup once. It runs on every exit. It is better, but we can take it further, see `orelse` section.

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

Defers execute in **LIFO order** (last defer runs first) at scope exit — whether via `return`, `break`, `continue`, `goto`, or reaching `}`.

**Edge cases handled:**
- Statement expressions `({ ... })` — defers fire at inner scope, not outer
- `switch` fallthrough — defers don't double-fire between cases  
- Nested loops — `break`/`continue` unwind the correct scope

**Forbidden patterns:** Functions using `setjmp`/`longjmp`, `vfork`, or inline assembly are rejected to prevent resource leaks from non-local jumps.

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

**Typedef tracking:** Prism parses headers to recognize `size_t`, `uint8_t`, `FILE *`, `pthread_mutex_t`, etc. This distinguishes `size_t x;` (declaration → initialize) from `size_t * x;` (expression → don't touch).

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

**Safety interaction:** Variables marked `raw` can be safely jumped over by `goto` — since they're not initialized anyway, skipping them isn't undefined behavior.

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

## Safety Enforcement
Prism acts as a static analysis tool, turning common C pitfalls into compile-time errors.

### No Uninitialized Jumps
Standard C allows `goto` to skip variable initialization, leading to undefined behavior. Prism performs a single-pass dominator analysis to forbid this:

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
Prism v0.112.0 - Robust C transpiler

Usage: prism [options] source.c... [-o output]

GCC-Compatible Options:
  -c                    Compile only, dont link
  -o <file>             Output file
  -O0/-O1/-O2/-O3/-Os   Optimization level (passed to CC)
  -g                    Debug info (passed to CC)
  -W...                 Warnings (passed to CC)
  -I/-D/-U/-L/-l        Include/define/lib flags (passed to CC)
  -std=...              Language standard (passed to CC)

Prism Options:
  -fno-defer            Disable defer feature
  -fno-zeroinit         Disable zero-initialization
  -fno-line-directives  Disable #line directives in output
  -fflatten-headers     Flatten included headers into single output file
  -fno-flatten-headers  Disable header flattening
  -fno-safety           Safety checks warn instead of error
  --prism-cc=<compiler> Use specific compiler (default: $CC or cc)
  --prism-verbose       Show transpile and compile commands

Commands:
  run <src.c>           Transpile, compile, and execute
  transpile <src.c>     Output transpiled C to stdout
  install               Install prism to /usr/local/bin/prism
  --help, -h            Show this help
  --version, -v         Show version

Environment:
  CC                    C compiler to use (default: cc)
  PRISM_CC              Override CC for prism specifically

Examples:
  prism foo.c                      Compile to a.out (GCC-compatible)
  prism foo.c -o foo               Compile to 'foo'
  prism run foo.c                  Compile and run immediately
  prism transpile foo.c            Output transpiled C
  prism transpile foo.c -o out.c   Transpile to file
  prism -c foo.c -o foo.o          Compile to object file
  prism -O2 -Wall foo.c -o foo     With optimization and warnings
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

**Note:** Library mode is currently limited and **not thread-safe**. The transpiler uses global state internally. Call from a single thread only, or protect with a mutex.

# Repo
Apache 2.0 license (c) Dawn Larsson 2026

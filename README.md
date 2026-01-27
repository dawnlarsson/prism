![prism_banner](https://github.com/user-attachments/assets/97c303d0-0d85-4262-8fb3-663a33ce00cd)

### **Robust C by default.**

Prism is a lightweight, self-contained transpiler that brings modern language features to standard C. It functions as a build tool with zero dependencies, allowing you to write safer code without the overhead of complex build systems or heavy frameworks, or macro hacks.

* **Stability First:** Focused on foundational correctness, testing, and verification of edge cases.
* **Opt out dialect features** Disable parts of the transpiler, like zero-init, with CLI flags.
* **Drop-in overlay:** Use `CC=prism` in any build system — GCC-compatible flags pass through automatically.

Prism is a propper transpiler, not a preprocessor macro.
* **Track Types:** It parses `typedef`s to distinguish pointer declarations from multiplication (the "lexer hack"), ensuring correct zero-initialization.
* **Respect Scope:** It understands braces `{}`, statement expressions `({ ... })`, and switch-case fallthrough, ensuring `defer` fires exactly when it should.
* **Detect Errors:** It catches unsafe patterns (like jumping into a scope with `goto`) before they become runtime bugs.

### Status
Prism can parse & preprocess **large complex C projects** like `Bash`, `Dash`.

All validation & edge tests pass, but,
There is some edge cases still to be caught in the parsing, **huge speedups** and runtime memory savings to be had.

<br/>

build & install
```c
cc prism.c -flto -s -O3 -o /tmp/prism && /tmp/prism install && rm /tmp/prism
```

## Defer
Scope-based resource management. Statements execute in LIFO order upon scope exit, including `return`, `break`, `continue`, and `goto`.

`defer` is robust against complex control flow. It correctly injects cleanup code before:
* `return` (including void and value returns)
* `break` / `continue` (handles loops and switches correctly)
* `goto` (unwinds scopes properly)

```c
void example() {
    FILE *f = fopen("file.txt", "r");
    defer fclose(f);

    void *mem = malloc(1024);
    defer free(mem);

    if (!f || !mem) return; // Cleanups run automatically
}
```

*Note: Defer is explicitly forbidden in functions using `setjmp`/`longjmp` or computed gotos to prevent resource leaks.*

**Opt-out:** `prism -fno-defer src.c`

## Zero-Init
Local variables are automatically zero-initialized.

```c
void example() {
    int x;           // x = 0
    char *ptr;       // ptr = NULL
    int arr[10];     // all elements = 0
    struct { int a; float b; } s;  // s = {0}
    
    typedef struct { int x, y; } Point;
    Point p;         // p = {0}
}
```

Works with: primitives, pointers, arrays, structs, unions, enums, and user-defined typedefs (including from headers like `uint8_t`, `size_t`, `FILE*`, `pthread_mutex_t`, etc).

**Opt-out:** `prism -fno-zeroinit src.c`

## Raw
Skip zero-initialization for a specific variable using the `raw` keyword.

```c
void example() {
    raw int x;           // x is uninitialized
    raw char buffer[4096]; // buffer is uninitialized (faster for large arrays)
}
```

Useful when you need to avoid the overhead of zero-initialization for performance-critical code or large buffers that will be immediately overwritten.

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

## CLI

Prism uses a GCC-compatible interface — most flags pass through to the backend compiler.

```sh
Prism v0.73.0 - Robust C transpiler

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

# parse.c
C tokenizer and preprocessor based from chibicc (MIT),
single file, few fixes/changes ect.

# Repo
Apache 2.0 license (c) Dawn Larsson 2026
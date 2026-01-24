![prism_banner](https://github.com/user-attachments/assets/97c303d0-0d85-4262-8fb3-663a33ce00cd)

### **Robust C by default.**

Prism is a lightweight, self-contained transpiler that brings modern language features to standard C. It functions as a build tool with zero dependencies, allowing you to write safer code without the overhead of complex build systems or heavy frameworks, or macro hacks.

* **Stability First:** Focused on foundational correctness, testing, and verification of edge cases.
* **Opt out dialect features** Disable parts of the transpiler, like zero-init, with CLI flags.
* **Build tool** Work-in-progress build tool features—no need for Makefiles for simpler C programs (with support for larger setups planned for the future).

Prism is a propper transpiler, not a preprocessor macro.
* **Track Types:** It parses `typedef`s to distinguish pointer declarations from multiplication (the "lexer hack"), ensuring correct zero-initialization.
* **Respect Scope:** It understands braces `{}`, statement expressions `({ ... })`, and switch-case fallthrough, ensuring `defer` fires exactly when it should.
* **Detect Errors:** It catches unsafe patterns (like jumping into a scope with `goto`) before they become runtime bugs.

<br/>

build & install
```c
cc prism.c -flto -s -O3 -o /tmp/prism && /tmp/prism install && rm /tmp/prism
```

## Defer
Scope-based resource management. Statements execute in LIFO order upon scope exit, including `return`, `break`, `continue`, and `goto`.

```c
void example() {
    FILE *f = fopen("file.txt", "r");
    defer fclose(f);

    void *mem = malloc(1024);
    defer free(mem);

    if (!f || !mem) return; // Cleanups run automatically
}
```

**Opt-out** `prism src.c no-defer`

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

**Opt-out** `prism src.c no-zeroinit`

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

**No Uninitialized Jumps**
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

### Differentiate "Defer" from Macros
Many C users assume `defer` is just a `for` loop macro hack (which breaks `break`/`continue`). You need to explicitly state that yours is robust against control flow.

### Control Flow Integrity
Prism's `defer` is robust against complex control flow. It correctly injects cleanup code before:
* `return` (including void and value returns)
* `break` / `continue` (handles loops and switches correctly)
* `goto` (unwinds scopes properly)

*Note: Defer is explicitly forbidden in functions using `setjmp`/`longjmp` or computed gotos to prevent resource leaks.*

## CLI

```sh
Prism v0.42.0

Usage: prism [options] src.c [output] [args]

Options:
  install               Install prism as a global cli tool
  build                 Build only, dont run
  transpile             Transpile only, output to stdout or file
  debug/release/small   Optimization mode
  arm/x86               Architecture (default: native)
  32/64                 Word size (default: 64)
  linux/windows/macos   Platform (default: native)
  no-defer              Disable defer feature
  no-zeroinit           Disable zero-initialization

Examples:
  prism src.c                   Run src.c
  prism build src.c             Build src
  prism build src.c out         Build to 'out'
  prism build arm src.c         Build for arm64 linux
  prism transpile src.c         Transpile to stdout
  prism transpile src.c out.c   Transpile to out.c
  prism no-defer src.c          Run without defer

Apache 2.0 license (c) Dawn Larsson 2026
https://github.com/dawnlarsson/prism 

```

# parse.c
C tokenizer and preprocessor based from chibicc (MIT),
single file, few fixes/changes ect.

# Repo
Apache 2.0 license (c) Dawn Larsson 2026

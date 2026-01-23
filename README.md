# Prism
**Robust C by default.**

Prism is a lightweight, self-contained transpiler that brings modern language features to standard C. It functions as a build tool with zero dependencies, allowing you to write safer code without the overhead of complex build systems or heavy frameworks, or macro hacks.

* **Stability First:** Focused on foundational correctness, testing, and verification of edge cases.
* **Opt out dialect features** Disable parts of the transpiler, like zero-init, with CLI flags.
* **Build tool** Work-in-progress build tool features—no need for Makefiles for simpler C programs (with support for larger setups planned for the future).
 
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
}
```

Works with: primitives, pointers, arrays, structs, unions, enums.

**Limitation:** Typedef'd types currently require explicit initialization.

**Opt-out** `prism src.c no-zeroinit`

## CLI

```sh
Prism v0.33.0

Usage: prism [options] src.c [output] [args]

Options:
  install               Install prism as a global cli tool
  build                 Build only, don't run
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

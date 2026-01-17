# Prism
C transpiled with some sugar

build & install
```c
cc prism.c -flto -s -O3 -o /tmp/prism && /tmp/prism install && rm /tmp/prism
```

```sh
Prism v0.14.0
Usage: prism [options] src.c [output] [args]

Options:
  build          Build only, don't run
  transpile      Transpile only, output to stdout or file
  debug/release/small  Optimization mode
  arm/x86        Architecture (default: native)
  32/64          Word size (default: 64)
  linux/windows/macos  Platform (default: native)

Examples:
  prism src.c              Run src.c
  prism build src.c        Build src
  prism build src.c out    Build to 'out'
  prism build arm src.c    Build for arm64 linux
  prism transpile src.c    Transpile to stdout
  prism transpile src.c out.c  Transpile to out.c

Prism extensions:
  defer stmt;    Execute stmt when scope exits

install
```

# parse.c
C tokenizer and preprocessor based from chibicc (MIT),
single file, few fixes/changes ect.

# Repo
Apache 2.0 license
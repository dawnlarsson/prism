#!/usr/bin/env bash
# .github/fuzz.sh — Differential + coverage-guided fuzzer for the Prism C transpiler
# Arch Linux only.
#
# Commands:
#   sudo bash .github/fuzz.sh --install  Install all required packages
#        bash .github/fuzz.sh --build    Compile Prism variants + AFL++ harness
#        bash .github/fuzz.sh --diff [N] csmith differential fuzzer (default: 2000 iters)
#        bash .github/fuzz.sh --afl      AFL++ persistent-mode crash finder (Ctrl-C to stop)
#        bash .github/fuzz.sh --cov      Generate lcov HTML coverage report
#        bash .github/fuzz.sh --all      --build + --diff 10000
#
# Differential strategy:
#   csmith generates random, deterministic (zero-UB) C99 programs with a printed checksum.
#   Path A: gcc -O0  original.c  → run → capture checksum.
#   Path B: prism transpile original.c | gcc -O0 - → run → capture checksum.
#   Divergence = semantic bug.  A sanitized prism_san binary runs in parallel to catch
#   internal crashes/UB that wouldn't affect output comparison (ASan + UBSan).
#   All failing inputs are saved to fuzz_crashes/ with full reproduction artifacts.
#
# AFL++ strategy:
#   A persistent-mode harness links prism as PRISM_LIB_MODE with afl-clang-lto.
#   ASan + UBSan enabled.  Seeds: all .github/test*.c files + minimal handcrafted seeds.
#   JOBS/2 parallel AFL++ instances are launched; primary uses -M, secondaries use -S.
#
# C coverage:
#   csmith covers: all integer types, pointers/pointer-to-pointer, structs, unions,
#   bitfields, multi-dimensional arrays, function pointers, const/volatile, typedef
#   chains, enums, cast expressions, signed/unsigned arithmetic, and virtually every
#   C99/C11 grammar production.  Since csmith output is preprocessed before reaching
#   prism, it also exercises all of prism's pass-through and zero-init transformation
#   paths.  AFL++ adds mutation-driven coverage of prism-specific constructs
#   (defer, orelse, raw) via evolution of the seed corpus.
#
# Environment overrides:
#   DIFF_N=<n>       Iteration count for --diff (default: 2000)
#   EXEC_TIMEOUT=<s> Per-program timeout in seconds (default: 5)
#   JOBS=<n>         AFL++ parallel instances (default: nproc/2, min 1)

set -Euo pipefail

# ─── paths ────────────────────────────────────────────────────────────────────
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO/.github/.fuzz_build"
CRASHES="$REPO/.github/fuzz_crashes"
SEEDS="$BUILD/seeds"

# ─── config ───────────────────────────────────────────────────────────────────
DIFF_N="${DIFF_N:-2000}"
EXEC_TIMEOUT="${EXEC_TIMEOUT:-5}"
DIFF_JOBS="${DIFF_JOBS:-$(nproc)}"
JOBS="${JOBS:-$(( $(nproc) / 2 < 1 ? 1 : $(nproc) / 2 ))}"

# csmith flags: --no-argc removes argv dependency → deterministic output.
# The rest maximise grammar surface area while keeping programs safe.
CSMITH_FLAGS=(
    --no-argc
    --max-funcs 12
    --max-pointer-depth 4
    --max-block-depth 5
    --max-array-dim 3
    --max-struct-fields 8
    --max-union-fields 4
    --unions
    --bitfields
    --packed-struct
    --arrays
    --volatiles
    --volatile-pointers
)

# ─── colours ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[1;33m'
CYN='\033[0;36m'
NC='\033[0m'
info()  { printf "${GRN}[fuzz]${NC} %s\n" "$*"; }
warn()  { printf "${YLW}[warn]${NC} %s\n" "$*"; }
die()   { printf "${RED}[fail]${NC} %s\n" "$*" >&2; exit 1; }

# ─── helpers ──────────────────────────────────────────────────────────────────
require() { command -v "$1" &>/dev/null || die "Required: $1  (run --install first)"; }

find_csmith_include() {
    # Prefer the path csmith itself reports, then fall back to a filesystem search.
    local p
    p=$(csmith --csmith-include-path 2>/dev/null) && { echo "$p"; return 0; }
    p=$(find /usr /usr/local -name "csmith.h" 2>/dev/null | head -1)
    [[ -n $p ]] && { dirname "$p"; return 0; }
    return 1
}

build_csmith_from_source() {
    info "csmith not found in repos — building from source..."
    require git; require cmake
    local tmp; tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' RETURN
    git clone --depth=1 https://github.com/csmith-project/csmith "$tmp/csmith"
    cmake -S "$tmp/csmith" -B "$tmp/csmith/build" \
          -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON -Wno-dev
    cmake --build   "$tmp/csmith/build" -j"$(nproc)"
    cmake --install "$tmp/csmith/build"
    info "csmith installed → $(which csmith)"
}

# ─── --install ────────────────────────────────────────────────────────────────
cmd_install() {
    [[ $EUID -eq 0 ]] || die "Run --install as root:  sudo bash .github/fuzz.sh --install"
    info "Syncing and installing packages..."
    pacman -Sy --needed --noconfirm \
        gcc clang llvm lld \
        afl++ \
        lcov \
        cmake git \
        diffutils coreutils

    if ! command -v csmith &>/dev/null; then
        build_csmith_from_source
    else
        info "csmith already installed: $(which csmith)"
    fi
    info "All dependencies installed.  Next: bash .github/fuzz.sh --build"
}

# ─── --build ──────────────────────────────────────────────────────────────────
cmd_build() {
    require cc; require clang
    mkdir -p "$BUILD" "$CRASHES" "$SEEDS"

    # 1. Release binary — used as the transpiler in the diff path.
    info "Building prism (release)..."
    cc -O2 -o "$BUILD/prism" "$REPO/prism.c"

    # 2. Sanitized binary — runs in parallel during diff to catch internal crashes/UB.
    info "Building prism (asan+ubsan)..."
    clang -O1 -g \
        -fsanitize=address,undefined \
        -fno-omit-frame-pointer \
        -fno-sanitize-recover=all \
        -o "$BUILD/prism_san" "$REPO/prism.c"

    # 3. Coverage binary — instrumented with gcov for --cov reports.
    info "Building prism (gcov)..."
    gcc -O0 -g --coverage -o "$BUILD/prism_cov" "$REPO/prism.c"
    # Store gcov data files alongside the binary.
    ln -sf "$REPO/prism.c" "$BUILD/prism.c.src" 2>/dev/null || true

    # 4. AFL++ persistent-mode harness.
    # Prefer afl-clang-fast (no LTO plugin → no LLVM ABI mismatch issues on Arch).
    # Fall back to afl-clang-lto only if fast is absent.
    local afl_cc=""
    command -v afl-clang-fast &>/dev/null && afl_cc=afl-clang-fast
    command -v afl-clang-lto  &>/dev/null && [[ -z $afl_cc ]] && afl_cc=afl-clang-lto
    if [[ -z $afl_cc ]]; then
        warn "afl-clang-lto / afl-clang-fast not found — AFL++ harness skipped"
        warn "(run --install to get afl++)"
    else
        info "Building AFL++ harness with $afl_cc..."

        # Symlink sources into BUILD so relative #include "prism.c" / "parse.c" resolve.
        ln -sf "$REPO/prism.c" "$BUILD/prism.c"
        ln -sf "$REPO/parse.c" "$BUILD/parse.c"
        [[ -f "$REPO/windows.c" ]] && ln -sf "$REPO/windows.c" "$BUILD/windows.c"

        # The harness calls prism_transpile_source() in persistent mode.
        # prism_transpile_source() calls prism_ctx_init() internally on each invocation,
        # so no explicit setup call is needed here.
        cat > "$BUILD/fuzz_harness.c" << 'HARNESS_EOF'
#define PRISM_LIB_MODE
#include "prism.c"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

__AFL_FUZZ_INIT();

int main(void)
{
#ifdef __AFL_HAVE_MANUAL_CONTROL
    __AFL_INIT();
#endif
    unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;
    while (__AFL_LOOP(100000)) {
        size_t len = __AFL_FUZZ_TESTCASE_LEN;
        if (len == 0 || len > 256 * 1024)
            continue;
        char *src = malloc(len + 1);
        if (!src) continue;
        memcpy(src, buf, len);
        src[len] = '\0';
        PrismResult r = prism_transpile_source(src, "fuzz.c", prism_defaults());
        prism_free(&r);
        free(src);
        prism_thread_cleanup();
    }
    return 0;
}
HARNESS_EOF

        (
            cd "$BUILD"
            # AFL_LLVM_INSTRUMENT=NATIVE uses clang's built-in SanitizerCoverage
            # (-fsanitize-coverage=trace-pc-guard) rather than loading an external
            # .so plugin, which is broken on Arch due to LLVM ABI mismatches.
            AFL_CC=clang AFL_LLVM_INSTRUMENT=NATIVE "$afl_cc" -O1 -g \
                -fsanitize=address,undefined \
                -fno-omit-frame-pointer \
                -fno-sanitize-recover=all \
                -o fuzz_prism fuzz_harness.c
        ) || { warn "AFL++ harness compile failed — check output above"; return; }
        info "AFL++ harness → $BUILD/fuzz_prism"
    fi

    # 5. Seed corpus.
    info "Preparing seed corpus..."
    for f in "$REPO/.github"/test*.c; do
        [[ -f $f ]] && cp "$f" "$SEEDS/" 2>/dev/null || true
    done
    # Minimal handcrafted seeds that exercise prism-specific constructs not in csmith output.
    printf 'int main(void){defer{} return 0;}\n'             > "$SEEDS/seed_defer.c"
    printf 'int main(void){int x = get() orelse 0;}\n'       > "$SEEDS/seed_orelse.c"
    printf 'int main(void){raw int x = 0; return x;}\n'      > "$SEEDS/seed_raw.c"
    printf 'int main(void){int x; return x;}\n'              > "$SEEDS/seed_zeroinit.c"
    printf '#include <stdio.h>\nint main(void){return 0;}\n' > "$SEEDS/seed_minimal.c"
    info "Seed corpus: $(ls "$SEEDS" | wc -l) files → $SEEDS"

    info "Build complete."
    printf "  ${CYN}%-12s${NC}%s\n" "release:"   "$BUILD/prism"
    printf "  ${CYN}%-12s${NC}%s\n" "sanitized:" "$BUILD/prism_san"
    printf "  ${CYN}%-12s${NC}%s\n" "coverage:"  "$BUILD/prism_cov"
    [[ -f "$BUILD/fuzz_prism" ]] && \
        printf "  ${CYN}%-12s${NC}%s\n" "afl harness:" "$BUILD/fuzz_prism"
}

# ─── --diff worker (runs in a background subshell) ───────────────────────────
_diff_worker() {
    local wid="$1" wstart="$2" wend="$3" td="$4" csmith_inc="$5"
    local wtd="$td/w${wid}"
    mkdir -p "$wtd"

    local pass=0 n_diff=0 n_crash=0 n_reject=0 n_codegen=0 total=0

    for (( i = wstart; i <= wend; i++ )); do

        local input="$wtd/in_${i}.c"
        local transpiled="$wtd/tx_${i}.c"
        local native_bin="$wtd/nb_${i}"
        local prism_bin="$wtd/pb_${i}"
        local native_out="$wtd/nb_${i}.out"
        local prism_out="$wtd/pb_${i}.out"
        local san_log="$wtd/san_${i}.txt"
        local fail_type=""

        # ── Generate a random deterministic C program ────────────────────────
        if ! csmith "${CSMITH_FLAGS[@]}" -o "$input" 2>/dev/null; then continue; fi
        total=$(( total + 1 ))

        # ── Path A: native gcc compile + run ─────────────────────────────────
        if ! gcc -O0 -w -I"$csmith_inc" "$input" -o "$native_bin" 2>/dev/null; then
            total=$(( total - 1 )); continue
        fi
        if ! timeout "$EXEC_TIMEOUT" "$native_bin" > "$native_out" 2>&1; then
            total=$(( total - 1 )); continue
        fi

        # ── Path B: prism transpile → gcc compile + run ──────────────────────
        local prism_status=0
        "$BUILD/prism" transpile \
            -fno-line-directives -I"$csmith_inc" "$input" \
            > "$transpiled" 2>/dev/null || prism_status=$?

        if (( prism_status != 0 )); then
            fail_type="REJECT"; n_reject=$(( n_reject + 1 ))
        else
            if ! gcc -O0 -w "$transpiled" -o "$prism_bin" 2>/dev/null; then
                fail_type="CODEGEN"; n_codegen=$(( n_codegen + 1 ))
            else
                timeout "$EXEC_TIMEOUT" "$prism_bin" > "$prism_out" 2>&1 || true
                if ! diff -q "$native_out" "$prism_out" &>/dev/null; then
                    fail_type="DIFF"; n_diff=$(( n_diff + 1 ))
                fi
            fi
        fi

        # ── Sanitized run (ASan + UBSan) ─────────────────────────────────────
        ASAN_OPTIONS="detect_leaks=0" \
        UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
            "$BUILD/prism_san" transpile \
            -fno-line-directives -I"$csmith_inc" "$input" \
            > /dev/null 2>"$san_log" || true
        if grep -qE 'runtime error|AddressSanitizer|heap-buffer-overflow|SIGSEGV|Killed' \
                "$san_log" 2>/dev/null; then
            if [[ -z $fail_type ]]; then
                fail_type="CRASH"; n_crash=$(( n_crash + 1 ))
            else
                fail_type="${fail_type}+CRASH"; n_crash=$(( n_crash + 1 ))
            fi
        fi

        # ── Save artifacts ───────────────────────────────────────────────────
        if [[ -n $fail_type ]]; then
            local cdir="$CRASHES/$(printf '%05d' "$i")_${fail_type}"
            mkdir -p "$cdir"
            cp "$input" "$cdir/input.c"
            [[ -f $transpiled ]] && cp "$transpiled" "$cdir/transpiled.c"
            [[ -f $native_out ]] && cp "$native_out" "$cdir/native.out"
            [[ -f $prism_out  ]] && cp "$prism_out"  "$cdir/prism.out"
            [[ -f $san_log    ]] && cp "$san_log"    "$cdir/sanitizer.txt"
            if [[ -f $native_out && -f $prism_out ]]; then
                diff "$native_out" "$prism_out" > "$cdir/output_diff.txt" 2>&1 || true
            fi
            printf "  [w%02d] ${RED}[%-14s]${NC} → %s\n" "$wid" "$fail_type" "$cdir"
        else
            pass=$(( pass + 1 ))
        fi

    done

    # Write tally for parent to aggregate: "pass diff crash reject codegen total"
    echo "$pass $n_diff $n_crash $n_reject $n_codegen $total" > "$td/w${wid}.result"
}

# ─── --diff ───────────────────────────────────────────────────────────────────
cmd_diff() {
    local n="${1:-$DIFF_N}"
    local diff_jobs="${DIFF_JOBS:-$(nproc)}"
    require csmith
    require gcc
    [[ -f "$BUILD/prism"     ]] || die "Binaries missing — run --build first"
    [[ -f "$BUILD/prism_san" ]] || die "Binaries missing — run --build first"

    local csmith_inc
    csmith_inc=$(find_csmith_include) \
        || die "csmith.h not found. Is csmith installed? (run --install)"
    info "csmith include path: $csmith_inc"

    local td; td=$(mktemp -d)
    # Use ${td:-} so the EXIT trap is safe if td is not yet set (e.g. mktemp
    # failed) or after cmd_diff returns and td goes out of bash's global scope.
    trap 'rm -rf "${td:-}"' EXIT
    mkdir -p "$CRASHES"

    # Shard N iterations across workers (ceiling division per worker).
    local per=$(( (n + diff_jobs - 1) / diff_jobs ))
    local pids=() nw=0
    for (( w = 0; w < diff_jobs; w++ )); do
        local ws=$(( w * per + 1 ))
        local we=$(( (w + 1) * per ))
        (( we > n )) && we=$n
        (( ws > n )) && break
        _diff_worker "$w" "$ws" "$we" "$td" "$csmith_inc" &
        pids+=($!)
        nw=$(( nw + 1 ))
    done

    info "Differential fuzzer: $n iterations across $nw worker(s), timeout ${EXEC_TIMEOUT}s/program"
    info "Workers running — failures will print as they are found..."

    for pid in "${pids[@]}"; do wait "$pid" || true; done

    # Aggregate results from all workers.
    local pass=0 n_diff=0 n_crash=0 n_reject=0 n_codegen=0 total=0
    for (( w = 0; w < nw; w++ )); do
        local rf="$td/w${w}.result"
        [[ -f $rf ]] || continue
        local p d c r g t
        read -r p d c r g t < "$rf"
        pass=$(( pass + p ))         ; n_diff=$(( n_diff + d ))
        n_crash=$(( n_crash + c ))   ; n_reject=$(( n_reject + r ))
        n_codegen=$(( n_codegen + g)); total=$(( total + t ))
    done

    local total_bugs=$(( n_diff + n_crash + n_reject + n_codegen ))
    printf "\n"
    info "Differential fuzzing complete."
    printf "  Total programs tested : %d\n"  "$total"
    printf "  ${GRN}Pass${NC}                  : %d\n"  "$pass"
    printf "  ${RED}DIFF  (output mismatch)${NC}: %d\n"  "$n_diff"
    printf "  ${RED}CRASH (sanitizer hit)  ${NC}: %d\n"  "$n_crash"
    printf "  ${RED}REJECT (false reject)  ${NC}: %d\n"  "$n_reject"
    printf "  ${RED}CODEGEN (bad C emitted)${NC}: %d\n"  "$n_codegen"
    printf "\n"
    if (( total_bugs == 0 )); then
        info "Clean run — no bugs found in $total programs."
    else
        warn "$total_bugs bug(s) found. Artifacts saved to: $CRASHES/"
    fi
}

# ─── --deep worker (multi-compiler, multi-opt, round-trip) ───────────────────
_deep_worker() {
    local wid="$1" wstart="$2" wend="$3" td="$4" csmith_inc="$5"
    local wtd="$td/w${wid}"
    mkdir -p "$wtd"

    local has_clang=0
    command -v clang &>/dev/null && has_clang=1

    local pass=0 total=0
    local n_diff=0 n_crash=0 n_reject=0 n_codegen=0
    local n_diff_clang=0 n_diff_opt=0 n_diff_rt=0 n_codegen_clang=0

    for (( i = wstart; i <= wend; i++ )); do
        local input="$wtd/in_${i}.c"
        local transpiled="$wtd/tx_${i}.c"
        local fail_type=""

        if ! csmith "${CSMITH_FLAGS[@]}" -o "$input" 2>/dev/null; then continue; fi
        total=$(( total + 1 ))

        # ── Path A: native gcc -O0 baseline ──────────────────────────────────
        local native_bin="$wtd/nb_${i}" native_out="$wtd/nb_${i}.out"
        if ! gcc -O0 -w -I"$csmith_inc" "$input" -o "$native_bin" 2>/dev/null; then
            total=$(( total - 1 )); continue; fi
        if ! timeout "$EXEC_TIMEOUT" "$native_bin" > "$native_out" 2>&1; then
            total=$(( total - 1 )); continue; fi

        # ── Transpile ────────────────────────────────────────────────────────
        local prism_status=0
        "$BUILD/prism" transpile \
            -fno-line-directives -I"$csmith_inc" "$input" \
            > "$transpiled" 2>/dev/null || prism_status=$?

        if (( prism_status != 0 )); then
            fail_type="REJECT"; n_reject=$(( n_reject + 1 ))
        else
            # ── Path B1: gcc -O0 (standard diff) ────────────────────────────
            local pb1="$wtd/pb1_${i}" po1="$wtd/pb1_${i}.out"
            if ! gcc -O0 -w "$transpiled" -o "$pb1" 2>/dev/null; then
                fail_type="CODEGEN"; n_codegen=$(( n_codegen + 1 ))
            else
                timeout "$EXEC_TIMEOUT" "$pb1" > "$po1" 2>&1 || true
                if ! diff -q "$native_out" "$po1" &>/dev/null; then
                    fail_type="DIFF"; n_diff=$(( n_diff + 1 ))
                fi
            fi

            # ── Path B2: clang -O0 (re-transpile with clang preprocessor) ────
            if (( has_clang )) && [[ -z $fail_type ]]; then
                local tx_clang="$wtd/tx_clang_${i}.c"
                local pb2="$wtd/pb2_${i}" po2="$wtd/pb2_${i}.out"
                if CC=clang "$BUILD/prism" transpile \
                    -fno-line-directives -I"$csmith_inc" "$input" \
                    > "$tx_clang" 2>/dev/null; then
                    if ! clang -O0 -w "$tx_clang" -o "$pb2" 2>/dev/null; then
                        fail_type="CODEGEN_CLANG"; n_codegen_clang=$(( n_codegen_clang + 1 ))
                    else
                        timeout "$EXEC_TIMEOUT" "$pb2" > "$po2" 2>&1 || true
                        if ! diff -q "$native_out" "$po2" &>/dev/null; then
                            fail_type="DIFF_CLANG"; n_diff_clang=$(( n_diff_clang + 1 ))
                        fi
                    fi
                fi
            fi

            # ── Path B3: gcc -O2 (exposes latent UB in transpiled output) ───
            if [[ -z $fail_type ]]; then
                local pb3="$wtd/pb3_${i}" po3="$wtd/pb3_${i}.out"
                if gcc -O2 -w "$transpiled" -o "$pb3" 2>/dev/null; then
                    if timeout "$EXEC_TIMEOUT" "$pb3" > "$po3" 2>&1; then
                        if ! diff -q "$native_out" "$po3" &>/dev/null; then
                            fail_type="DIFF_OPT"; n_diff_opt=$(( n_diff_opt + 1 ))
                        fi
                    fi
                fi
            fi

            # ── Path B4: round-trip — transpile(transpile(X)) ───────────────
            if [[ -z $fail_type ]]; then
                local rt="$wtd/rt_${i}.c" rtb="$wtd/rt_${i}" rto="$wtd/rt_${i}.out"
                if "$BUILD/prism" transpile -fno-line-directives "$transpiled" \
                    > "$rt" 2>/dev/null; then
                    if gcc -O0 -w "$rt" -o "$rtb" 2>/dev/null; then
                        timeout "$EXEC_TIMEOUT" "$rtb" > "$rto" 2>&1 || true
                        if ! diff -q "$native_out" "$rto" &>/dev/null; then
                            fail_type="DIFF_RT"; n_diff_rt=$(( n_diff_rt + 1 ))
                        fi
                    fi
                fi
            fi
        fi

        # ── Sanitized run ────────────────────────────────────────────────────
        local san_log="$wtd/san_${i}.txt"
        ASAN_OPTIONS="detect_leaks=0" \
        UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
            "$BUILD/prism_san" transpile \
            -fno-line-directives -I"$csmith_inc" "$input" \
            > /dev/null 2>"$san_log" || true
        if grep -qE 'runtime error|AddressSanitizer|heap-buffer-overflow|SIGSEGV|Killed' \
                "$san_log" 2>/dev/null; then
            fail_type="${fail_type:+${fail_type}+}CRASH"; n_crash=$(( n_crash + 1 ))
        fi

        # ── Save artifacts ───────────────────────────────────────────────────
        if [[ -n $fail_type ]]; then
            local cdir="$CRASHES/deep_$(printf '%05d' "$i")_${fail_type}"
            mkdir -p "$cdir"
            cp "$input" "$cdir/input.c"
            [[ -f $transpiled       ]] && cp "$transpiled"       "$cdir/transpiled.c"
            [[ -f "$wtd/rt_${i}.c"  ]] && cp "$wtd/rt_${i}.c"   "$cdir/roundtrip.c"
            [[ -f $native_out       ]] && cp "$native_out"       "$cdir/native.out"
            [[ -f "$wtd/pb1_${i}.out" ]] && cp "$wtd/pb1_${i}.out" "$cdir/gcc_O0.out"
            [[ -f "$wtd/pb2_${i}.out" ]] && cp "$wtd/pb2_${i}.out" "$cdir/clang_O0.out"
            [[ -f "$wtd/pb3_${i}.out" ]] && cp "$wtd/pb3_${i}.out" "$cdir/gcc_O2.out"
            [[ -f "$wtd/rt_${i}.out"  ]] && cp "$wtd/rt_${i}.out"  "$cdir/roundtrip.out"
            [[ -f $san_log          ]] && cp "$san_log"          "$cdir/sanitizer.txt"
            printf "  [w%02d] ${RED}[%-20s]${NC} → %s\n" "$wid" "$fail_type" "$cdir"
        else
            pass=$(( pass + 1 ))
        fi
    done

    printf "%d %d %d %d %d %d %d %d %d %d" \
        "$pass" "$n_diff" "$n_crash" "$n_reject" "$n_codegen" \
        "$n_diff_clang" "$n_diff_opt" "$n_diff_rt" "$n_codegen_clang" "$total" \
        > "$td/w${wid}.result"
}

# ─── --deep ──────────────────────────────────────────────────────────────────
cmd_deep() {
    local n="${1:-$DIFF_N}"
    local diff_jobs="${DIFF_JOBS:-$(nproc)}"
    require csmith; require gcc
    [[ -f "$BUILD/prism"     ]] || die "Binaries missing — run --build first"
    [[ -f "$BUILD/prism_san" ]] || die "Binaries missing — run --build first"

    local csmith_inc
    csmith_inc=$(find_csmith_include) \
        || die "csmith.h not found. Is csmith installed? (run --install)"
    info "csmith include path: $csmith_inc"
    info "Deep differential: multi-compiler, multi-opt, round-trip"
    command -v clang &>/dev/null \
        && info "  clang: $(clang --version | head -1)" \
        || warn "  clang not found — skipping clang paths"

    local td; td=$(mktemp -d)
    trap 'rm -rf "${td:-}"' EXIT
    mkdir -p "$CRASHES"

    local per=$(( (n + diff_jobs - 1) / diff_jobs ))
    local pids=() nw=0
    for (( w = 0; w < diff_jobs; w++ )); do
        local ws=$(( w * per + 1 )) we=$(( (w + 1) * per ))
        (( we > n )) && we=$n; (( ws > n )) && break
        _deep_worker "$w" "$ws" "$we" "$td" "$csmith_inc" &
        pids+=($!); nw=$(( nw + 1 ))
    done

    info "Deep differential: $n iterations across $nw worker(s), timeout ${EXEC_TIMEOUT}s/program"
    info "Workers running — failures will print as they are found..."
    for pid in "${pids[@]}"; do wait "$pid" || true; done

    # Aggregate.
    local pass=0 n_diff=0 n_crash=0 n_reject=0 n_codegen=0
    local n_diff_clang=0 n_diff_opt=0 n_diff_rt=0 n_codegen_clang=0 total=0
    for (( w = 0; w < nw; w++ )); do
        local rf="$td/w${w}.result"
        [[ -f $rf ]] || continue
        local p d c r g dc dop drt gc t
        read -r p d c r g dc dop drt gc t < "$rf"
        pass=$((pass+p));  n_diff=$((n_diff+d));  n_crash=$((n_crash+c))
        n_reject=$((n_reject+r));  n_codegen=$((n_codegen+g))
        n_diff_clang=$((n_diff_clang+dc));  n_diff_opt=$((n_diff_opt+dop))
        n_diff_rt=$((n_diff_rt+drt));  n_codegen_clang=$((n_codegen_clang+gc))
        total=$((total+t))
    done

    local total_bugs=$(( n_diff + n_crash + n_reject + n_codegen + \
                         n_diff_clang + n_diff_opt + n_diff_rt + n_codegen_clang ))
    printf "\n"
    info "Deep differential fuzzing complete."
    printf "  Total programs tested     : %d\n"  "$total"
    printf "  ${GRN}Pass${NC}                      : %d\n"  "$pass"
    printf "  ${RED}DIFF  (gcc -O0 mismatch)  ${NC}: %d\n"  "$n_diff"
    printf "  ${RED}DIFF  (clang -O0 mismatch)${NC}: %d\n"  "$n_diff_clang"
    printf "  ${RED}DIFF  (gcc -O2 mismatch)  ${NC}: %d\n"  "$n_diff_opt"
    printf "  ${RED}DIFF  (round-trip)        ${NC}: %d\n"  "$n_diff_rt"
    printf "  ${RED}CRASH (sanitizer hit)     ${NC}: %d\n"  "$n_crash"
    printf "  ${RED}REJECT (false reject)     ${NC}: %d\n"  "$n_reject"
    printf "  ${RED}CODEGEN (gcc rejects)     ${NC}: %d\n"  "$n_codegen"
    printf "  ${RED}CODEGEN (clang rejects)   ${NC}: %d\n"  "$n_codegen_clang"
    printf "\n"
    if (( total_bugs == 0 )); then
        info "Clean run — no bugs found in $total programs."
    else
        warn "$total_bugs bug(s) found. Artifacts saved to: $CRASHES/"
    fi
}

# ─── Prism-extension template generators ─────────────────────────────────────
# Each _gen_*_pair function writes prism_src.c (uses extensions) and ref_src.c
# (plain C with identical semantics) into a directory.  The fuzzer transpiles
# the prism source, compiles both, runs both, and diffs the checksum output.

_gen_orelse_pair() {
    local seed=$1 dir=$2
    local nfuncs=$(( seed % 4 + 2 ))    # 2-5 getter functions
    local nexprs=$(( seed % 6 + 3 ))    # 3-8 orelse expressions

    local prism_body="" ref_body=""
    for (( e = 0; e < nexprs; e++ )); do
        local fn=$(( (seed + e * 3) % nfuncs ))
        local fb=$(( (seed + e * 13) % 100 + 1 ))
        prism_body+="    int v${e} = get_${fn}() orelse ${fb};"$'\n'
        prism_body+="    checksum += (unsigned)v${e};"$'\n'
        ref_body+="    int _t${e} = get_${fn}(); int v${e} = _t${e} ? _t${e} : ${fb};"$'\n'
        ref_body+="    checksum += (unsigned)v${e};"$'\n'
    done

    local header=""
    header+='#include <stdio.h>'$'\n'
    for (( f = 0; f < nfuncs; f++ )); do
        local rv=$(( (seed / (f + 1) + f * 7) % 20 ))
        header+="int get_${f}(void) { return ${rv}; }"$'\n'
    done

    local footer='    printf("checksum = %lX\n", checksum);'$'\n'
    footer+='    return 0;'$'\n'
    footer+='}'$'\n'

    printf '%s%s\n%s%s' "$header" 'int main(void) {' \
        '    unsigned long checksum = 0;'$'\n'"$prism_body" "$footer" \
        > "$dir/prism_src.c"
    printf '%s%s\n%s%s' "$header" 'int main(void) {' \
        '    unsigned long checksum = 0;'$'\n'"$ref_body" "$footer" \
        > "$dir/ref_src.c"
}

_gen_defer_flat_pair() {
    local seed=$1 dir=$2
    local ndefers=$(( seed % 6 + 2 ))   # 2-7 defers in one scope

    {
        echo '#include <stdio.h>'
        echo 'int main(void) {'
        printf '    int log[%d]; int idx = 0;\n' "$ndefers"
        echo '    {'
        for (( d = 0; d < ndefers; d++ )); do
            printf '        defer { log[idx++] = %d; }\n' "$d"
        done
        echo '    }'
        echo '    unsigned checksum = 0;'
        printf '    for (int i = 0; i < %d; i++) checksum = checksum * 31u + (unsigned)log[i];\n' "$ndefers"
        echo '    printf("checksum = %X\n", checksum);'
        echo '    return 0;'
        echo '}'
    } > "$dir/prism_src.c"

    {
        echo '#include <stdio.h>'
        echo 'int main(void) {'
        printf '    int log[%d]; int idx = 0;\n' "$ndefers"
        echo '    {'
        for (( d = ndefers - 1; d >= 0; d-- )); do
            printf '        log[idx++] = %d;\n' "$d"
        done
        echo '    }'
        echo '    unsigned checksum = 0;'
        printf '    for (int i = 0; i < %d; i++) checksum = checksum * 31u + (unsigned)log[i];\n' "$ndefers"
        echo '    printf("checksum = %X\n", checksum);'
        echo '    return 0;'
        echo '}'
    } > "$dir/ref_src.c"
}

_gen_defer_nested_pair() {
    local seed=$1 dir=$2
    local n_outer=$(( seed % 3 + 1 ))   # 1-3 outer defers
    local n_inner=$(( seed % 3 + 1 ))   # 1-3 inner defers
    local total=$(( n_outer + n_inner ))

    {
        echo '#include <stdio.h>'
        echo 'int main(void) {'
        printf '    int log[%d]; int idx = 0;\n' "$total"
        echo '    {'
        for (( d = 0; d < n_outer; d++ )); do
            printf '        defer { log[idx++] = %d; }\n' "$d"
        done
        echo '        {'
        for (( d = 0; d < n_inner; d++ )); do
            printf '            defer { log[idx++] = %d; }\n' "$(( n_outer + d ))"
        done
        echo '        }'
        echo '    }'
        echo '    unsigned checksum = 0;'
        printf '    for (int i = 0; i < %d; i++) checksum = checksum * 31u + (unsigned)log[i];\n' "$total"
        echo '    printf("checksum = %X\n", checksum);'
        echo '    return 0;'
        echo '}'
    } > "$dir/prism_src.c"

    {
        echo '#include <stdio.h>'
        echo 'int main(void) {'
        printf '    int log[%d]; int idx = 0;\n' "$total"
        echo '    {'
        echo '        {'
        # Inner defers fire first (LIFO within inner scope)
        for (( d = n_inner - 1; d >= 0; d-- )); do
            printf '            log[idx++] = %d;\n' "$(( n_outer + d ))"
        done
        echo '        }'
        # Then outer defers fire (LIFO within outer scope)
        for (( d = n_outer - 1; d >= 0; d-- )); do
            printf '        log[idx++] = %d;\n' "$d"
        done
        echo '    }'
        echo '    unsigned checksum = 0;'
        printf '    for (int i = 0; i < %d; i++) checksum = checksum * 31u + (unsigned)log[i];\n' "$total"
        echo '    printf("checksum = %X\n", checksum);'
        echo '    return 0;'
        echo '}'
    } > "$dir/ref_src.c"
}

_gen_defer_return_pair() {
    local seed=$1 dir=$2
    local ndefers=$(( seed % 4 + 2 ))    # 2-5 defers
    local retafter=$(( seed % ndefers ))  # early return after this defer

    {
        echo '#include <stdio.h>'
        echo 'int counter = 0;'
        echo 'int helper(void) {'
        for (( d = 0; d < ndefers; d++ )); do
            local amt=$(( (seed + d * 7) % 50 + 1 ))
            printf '    defer { counter += %d; }\n' "$amt"
            if (( d == retafter )); then
                echo '    return counter;'
            fi
        done
        echo '    return counter;'
        echo '}'
        echo 'int main(void) {'
        echo '    int r = helper();'
        echo '    printf("checksum = %X %X\n", (unsigned)r, (unsigned)counter);'
        echo '    return 0;'
        echo '}'
    } > "$dir/prism_src.c"

    # Reference: return captures value BEFORE defers fire, then defers 0..retafter
    # fire in reverse.
    {
        echo '#include <stdio.h>'
        echo 'int counter = 0;'
        echo 'int helper(void) {'
        echo '    int _retval = counter;'
        for (( d = retafter; d >= 0; d-- )); do
            local amt=$(( (seed + d * 7) % 50 + 1 ))
            printf '    counter += %d;\n' "$amt"
        done
        echo '    return _retval;'
        echo '}'
        echo 'int main(void) {'
        echo '    int r = helper();'
        echo '    printf("checksum = %X %X\n", (unsigned)r, (unsigned)counter);'
        echo '    return 0;'
        echo '}'
    } > "$dir/ref_src.c"
}

_gen_mixed_pair() {
    local seed=$1 dir=$2
    # Function with defer cleanup + orelse value acquisition
    local nvals=$(( seed % 3 + 2 ))   # 2-4 orelse values

    local orelse_body_p="" orelse_body_r=""
    for (( e = 0; e < nvals; e++ )); do
        local rv=$(( (seed + e * 11) % 15 ))
        local fb=$(( (seed + e * 7)  % 50 + 1 ))
        orelse_body_p+="        int v${e} = get_${e}() orelse ${fb};"$'\n'
        orelse_body_p+="        result += (unsigned)v${e};"$'\n'
        orelse_body_r+="        { int _t = get_${e}(); int v${e} = _t ? _t : ${fb}; result += (unsigned)v${e}; }"$'\n'
    done

    local getters=""
    for (( e = 0; e < nvals; e++ )); do
        local rv=$(( (seed + e * 11) % 15 ))
        getters+="int get_${e}(void) { return ${rv}; }"$'\n'
    done

    {
        echo '#include <stdio.h>'
        printf '%s' "$getters"
        echo 'int main(void) {'
        echo '    unsigned result = 0;'
        echo '    {'
        printf '%s' "$orelse_body_p"
        echo '        defer { result += 2000; }'
        echo '        defer { result += 1000; }'
        echo '    }'
        echo '    printf("checksum = %X\n", result);'
        echo '    return 0;'
        echo '}'
    } > "$dir/prism_src.c"

    {
        echo '#include <stdio.h>'
        printf '%s' "$getters"
        echo 'int main(void) {'
        echo '    unsigned result = 0;'
        echo '    {'
        printf '%s' "$orelse_body_r"
        echo '        result += 1000;'   # defer 1 fires first (LIFO)
        echo '        result += 2000;'   # defer 0 fires second
        echo '    }'
        echo '    printf("checksum = %X\n", result);'
        echo '    return 0;'
        echo '}'
    } > "$dir/ref_src.c"
}

# ─── --prism worker ──────────────────────────────────────────────────────────
_prism_worker() {
    local wid="$1" wstart="$2" wend="$3" td="$4"
    local wtd="$td/w${wid}"
    mkdir -p "$wtd"

    local pass=0 total=0 n_diff=0 n_crash=0 n_reject=0 n_codegen=0
    local templates=(orelse defer_flat defer_nested defer_return mixed)
    local ntemplates=${#templates[@]}

    for (( i = wstart; i <= wend; i++ )); do
        local idir="$wtd/$i"
        mkdir -p "$idir"
        total=$(( total + 1 ))

        local tpl_idx=$(( i % ntemplates ))
        local tpl="${templates[$tpl_idx]}"
        local fail_type=""

        # Generate program pair
        case "$tpl" in
            orelse)       _gen_orelse_pair       "$i" "$idir" ;;
            defer_flat)   _gen_defer_flat_pair   "$i" "$idir" ;;
            defer_nested) _gen_defer_nested_pair "$i" "$idir" ;;
            defer_return) _gen_defer_return_pair "$i" "$idir" ;;
            mixed)        _gen_mixed_pair        "$i" "$idir" ;;
        esac

        # Compile reference
        local ref_bin="$idir/ref" ref_out="$idir/ref.out"
        if ! gcc -O0 -w "$idir/ref_src.c" -o "$ref_bin" 2>/dev/null; then
            total=$(( total - 1 )); continue
        fi
        if ! timeout "$EXEC_TIMEOUT" "$ref_bin" > "$ref_out" 2>&1; then
            total=$(( total - 1 )); continue
        fi

        # Transpile prism source
        local transpiled="$idir/transpiled.c"
        local prism_status=0
        "$BUILD/prism" transpile -fno-line-directives "$idir/prism_src.c" \
            > "$transpiled" 2>/dev/null || prism_status=$?

        if (( prism_status != 0 )); then
            fail_type="REJECT"; n_reject=$(( n_reject + 1 ))
        else
            local prism_bin="$idir/prism_bin" prism_out="$idir/prism.out"
            if ! gcc -O0 -w "$transpiled" -o "$prism_bin" 2>/dev/null; then
                fail_type="CODEGEN"; n_codegen=$(( n_codegen + 1 ))
            else
                timeout "$EXEC_TIMEOUT" "$prism_bin" > "$prism_out" 2>&1 || true
                if ! diff -q "$ref_out" "$prism_out" &>/dev/null; then
                    fail_type="DIFF"; n_diff=$(( n_diff + 1 ))
                fi
            fi
        fi

        # Sanitized run
        local san_log="$idir/sanitizer.txt"
        ASAN_OPTIONS="detect_leaks=0" \
        UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
            "$BUILD/prism_san" transpile -fno-line-directives "$idir/prism_src.c" \
            > /dev/null 2>"$san_log" || true
        if grep -qE 'runtime error|AddressSanitizer|heap-buffer-overflow|SIGSEGV|Killed' \
                "$san_log" 2>/dev/null; then
            fail_type="${fail_type:+${fail_type}+}CRASH"; n_crash=$(( n_crash + 1 ))
        fi

        if [[ -n $fail_type ]]; then
            local cdir="$CRASHES/prism_${tpl}_$(printf '%05d' "$i")_${fail_type}"
            mkdir -p "$cdir"
            cp "$idir/prism_src.c"  "$cdir/"
            cp "$idir/ref_src.c"    "$cdir/"
            [[ -f $transpiled       ]] && cp "$transpiled"    "$cdir/"
            [[ -f "$idir/ref.out"   ]] && cp "$idir/ref.out"  "$cdir/"
            [[ -f "$idir/prism.out" ]] && cp "$idir/prism.out" "$cdir/"
            [[ -f $san_log          ]] && cp "$san_log"        "$cdir/"
            if [[ -f "$idir/ref.out" && -f "$idir/prism.out" ]]; then
                diff "$idir/ref.out" "$idir/prism.out" > "$cdir/output_diff.txt" 2>&1 || true
            fi
            printf "  [w%02d] ${RED}[%-13s %-14s]${NC} → %s\n" "$wid" "$tpl" "$fail_type" "$cdir"
        else
            pass=$(( pass + 1 ))
        fi
    done

    echo "$pass $n_diff $n_crash $n_reject $n_codegen $total" > "$td/w${wid}.result"
}

# ─── --prism ─────────────────────────────────────────────────────────────────
cmd_prism() {
    local n="${1:-1000}"
    local diff_jobs="${DIFF_JOBS:-$(nproc)}"
    require gcc
    [[ -f "$BUILD/prism"     ]] || die "Binaries missing — run --build first"
    [[ -f "$BUILD/prism_san" ]] || die "Binaries missing — run --build first"

    info "Prism-extension template fuzzer: $n iterations"
    info "  Templates: orelse, defer (flat/nested/return), mixed (defer+orelse)"

    local td; td=$(mktemp -d)
    trap 'rm -rf "${td:-}"' EXIT
    mkdir -p "$CRASHES"

    local per=$(( (n + diff_jobs - 1) / diff_jobs ))
    local pids=() nw=0
    for (( w = 0; w < diff_jobs; w++ )); do
        local ws=$(( w * per + 1 )) we=$(( (w + 1) * per ))
        (( we > n )) && we=$n; (( ws > n )) && break
        _prism_worker "$w" "$ws" "$we" "$td" &
        pids+=($!); nw=$(( nw + 1 ))
    done

    info "Running $nw worker(s), timeout ${EXEC_TIMEOUT}s/program..."
    for pid in "${pids[@]}"; do wait "$pid" || true; done

    local pass=0 n_diff=0 n_crash=0 n_reject=0 n_codegen=0 total=0
    for (( w = 0; w < nw; w++ )); do
        local rf="$td/w${w}.result"
        [[ -f $rf ]] || continue
        local p d c r g t
        read -r p d c r g t < "$rf"
        pass=$((pass+p)); n_diff=$((n_diff+d)); n_crash=$((n_crash+c))
        n_reject=$((n_reject+r)); n_codegen=$((n_codegen+g)); total=$((total+t))
    done

    local total_bugs=$(( n_diff + n_crash + n_reject + n_codegen ))
    printf "\n"
    info "Prism-extension fuzzing complete."
    printf "  Total programs tested : %d\n"  "$total"
    printf "  ${GRN}Pass${NC}                  : %d\n"  "$pass"
    printf "  ${RED}DIFF  (output mismatch)${NC}: %d\n"  "$n_diff"
    printf "  ${RED}CRASH (sanitizer hit)  ${NC}: %d\n"  "$n_crash"
    printf "  ${RED}REJECT (false reject)  ${NC}: %d\n"  "$n_reject"
    printf "  ${RED}CODEGEN (bad C emitted)${NC}: %d\n"  "$n_codegen"
    printf "\n"
    if (( total_bugs == 0 )); then
        info "Clean run — no bugs found in $total programs."
    else
        warn "$total_bugs bug(s) found. Artifacts saved to: $CRASHES/"
    fi
}

# ─── --cov ────────────────────────────────────────────────────────────────────
cmd_cov() {
    require lcov
    require genhtml
    [[ -f "$BUILD/prism_cov" ]] || die "Coverage binary missing — run --build first"
    [[ -d "$SEEDS"           ]] || die "Seed corpus missing — run --build first"

    local csmith_inc=""
    find_csmith_include &>/dev/null && csmith_inc=$(find_csmith_include) || true

    info "Resetting gcov counters..."
    lcov --zerocounters --directory "$BUILD" --quiet 2>/dev/null || true

    info "Running coverage binary against seed corpus ($(ls "$SEEDS" | wc -l) seeds)..."
    for f in "$SEEDS"/*.c; do
        [[ -f $f ]] || continue
        GCOV_PREFIX="$BUILD" \
            "$BUILD/prism_cov" transpile -fno-line-directives \
            ${csmith_inc:+-I"$csmith_inc"} "$f" \
            > /dev/null 2>&1 || true
    done

    info "Collecting coverage data..."
    lcov --capture --directory "$BUILD" \
         --output-file "$BUILD/prism.info" \
         --include "*/prism.c" --include "*/parse.c" \
         --quiet 2>/dev/null

    local report_dir="$BUILD/cov_report"
    mkdir -p "$report_dir"
    genhtml "$BUILD/prism.info" \
            --output-directory "$report_dir" \
            --title "Prism Fuzzer Coverage" \
            --legend --branch-coverage \
            --quiet

    info "Coverage report → $report_dir/index.html"
    lcov --summary "$BUILD/prism.info" 2>&1 \
        | grep -E "lines\.|functions\.|branches\." || true
}

# ─── --afl ────────────────────────────────────────────────────────────────────
cmd_afl() {
    require afl-fuzz
    [[ -f "$BUILD/fuzz_prism" ]] || die "AFL++ harness missing — run --build first"
    [[ -d "$SEEDS"            ]] || die "Seed corpus missing — run --build first"

    # Recommended AFL++ system tuning (best-effort; warn if it fails).
    if [[ -w /proc/sys/kernel/core_pattern ]]; then
        echo core > /proc/sys/kernel/core_pattern
    else
        warn "Cannot set core_pattern (may need root). Performance may degrade."
    fi

    local afl_out="$BUILD/afl_output"
    mkdir -p "$afl_out"

    info "AFL++ persistent-mode fuzzer: $JOBS instance(s)"
    info "Seed corpus : $SEEDS"
    info "Output dir  : $afl_out"
    info "Crashes     : $afl_out/fuzzer*/crashes/"
    info "Press Ctrl-C to stop."
    printf "\n"

    local common_env=(
        AFL_SKIP_CPUFREQ=1
        AFL_AUTORESUME=1
        ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
    )

    if (( JOBS == 1 )); then
        env "${common_env[@]}" \
            afl-fuzz -i "$SEEDS" -o "$afl_out" -t 5000 -m none \
            -- "$BUILD/fuzz_prism"
    else
        # Primary instance (-M) runs deterministic mutations; secondaries run havoc.
        env "${common_env[@]}" \
            afl-fuzz -i "$SEEDS" -o "$afl_out" -t 5000 -m none \
            -M fuzzer01 -- "$BUILD/fuzz_prism" &

        for (( j = 2; j <= JOBS; j++ )); do
            env "${common_env[@]}" \
                afl-fuzz -i "$SEEDS" -o "$afl_out" -t 5000 -m none \
                -S "$(printf 'fuzzer%02d' "$j")" -- "$BUILD/fuzz_prism" &
        done
        wait
    fi

    # Print crash summary after AFL++ exits.
    local ncr; ncr=$(find "$afl_out" -path "*/crashes/id:*" 2>/dev/null | wc -l)
    printf "\n"
    info "AFL++ session ended.  Unique crashes found: $ncr"
    if (( ncr > 0 )); then
        find "$afl_out" -name "crashes" -type d | while read -r d; do
            info "  $(ls "$d" | grep -c '^id:') crash(es) in $d"
        done
    fi
}

# ─── usage ────────────────────────────────────────────────────────────────────
usage() {
    cat << 'EOF'

Usage:  [sudo] bash .github/fuzz.sh COMMAND [ARG]

Commands:
  --install         Install all required packages via pacman + build csmith
                    (must run as root)
  --build           Compile Prism: release, asan+ubsan, gcov, and AFL++ harness
  --diff [N]        Differential csmith fuzzer — N iterations (default: 2000)
  --deep [N]        Deep differential: multi-compiler (gcc+clang), multi-opt
                    (-O0+-O2), round-trip idempotency (default: 2000 iterations)
  --prism [N]       Prism-extension template fuzzer — generates programs using
                    defer/orelse with known-correct C references (default: 1000)
  --afl             AFL++ persistent-mode crash finder (Ctrl-C to stop)
  --cov             Generate lcov HTML coverage report (opens as cov_report/index.html)
  --all             --build + --diff 10000 + --deep 5000 + --prism 2000

Environment:
  DIFF_N=<n>        Override default iteration count             (default: 2000)
  DIFF_JOBS=<n>     Parallel workers for --diff/--deep/--prism   (default: nproc)
  EXEC_TIMEOUT=<s>  Per-program execution timeout in seconds     (default: 5)
  JOBS=<n>          AFL++ parallel instances                      (default: nproc/2)

Failure categories saved to fuzz_crashes/:
  DIFF           — prism-transpiled binary produced different output than native gcc
  DIFF_CLANG     — transpiled output diverges under clang (--deep only)
  DIFF_OPT       — transpiled output diverges at -O2 (latent UB) (--deep only)
  DIFF_RT        — round-trip transpile(transpile(X)) diverges (--deep only)
  CRASH          — ASan/UBSan triggered inside prism itself
  REJECT         — prism rejected a valid program (false negative)
  CODEGEN        — prism emitted C that gcc refuses to compile
  CODEGEN_CLANG  — prism emitted C that clang refuses to compile (--deep only)

EOF
}

# ─── dispatch ─────────────────────────────────────────────────────────────────
[[ $# -eq 0 ]] && { usage; exit 0; }

case "$1" in
    --install)  cmd_install                                          ;;
    --build)    cmd_build                                            ;;
    --diff)     cmd_diff "${2:-}"                                    ;;
    --deep)     cmd_deep "${2:-}"                                    ;;
    --prism)    cmd_prism "${2:-}"                                   ;;
    --afl)      cmd_afl                                              ;;
    --cov)      cmd_cov                                              ;;
    --all)      cmd_build; cmd_diff 10000; cmd_deep 5000; cmd_prism 2000 ;;
    --help|-h)  usage                                                ;;
    *)          die "Unknown command: $1  (try --help)"              ;;
esac

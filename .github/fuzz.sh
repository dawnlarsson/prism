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
    --max-funcs 8
    --max-pointer-depth 3
    --max-block-depth 4
    --unions
    --bitfields
    --arrays
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
  --afl             AFL++ persistent-mode crash finder (Ctrl-C to stop)
  --cov             Generate lcov HTML coverage report (opens as cov_report/index.html)
  --all             --build then --diff 10000

Environment:
  DIFF_N=<n>        Override default iteration count             (default: 2000)
  DIFF_JOBS=<n>     Parallel workers for --diff                  (default: nproc)
  EXEC_TIMEOUT=<s>  Per-program execution timeout in seconds     (default: 5)
  JOBS=<n>          AFL++ parallel instances                      (default: nproc/2)

Failure categories saved to fuzz_crashes/:
  DIFF       — prism-transpiled binary produced different output than native gcc
  CRASH      — ASan/UBSan triggered inside prism itself
  REJECT     — prism rejected a valid csmith program (false negative)
  CODEGEN    — prism emitted C that gcc refuses to compile

EOF
}

# ─── dispatch ─────────────────────────────────────────────────────────────────
[[ $# -eq 0 ]] && { usage; exit 0; }

case "$1" in
    --install)  cmd_install                ;;
    --build)    cmd_build                  ;;
    --diff)     cmd_diff "${2:-}"          ;;
    --afl)      cmd_afl                    ;;
    --cov)      cmd_cov                    ;;
    --all)      cmd_build; cmd_diff 10000  ;;
    --help|-h)  usage                      ;;
    *)          die "Unknown command: $1  (try --help)" ;;
esac

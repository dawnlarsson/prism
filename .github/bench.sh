#!/usr/bin/env bash
# ============================================================================
# Prism Benchmark Suite
# Compares current HEAD against the 1.0 release (8928ee1)
#
# Usage:  bash .github/bench.sh [iterations]
#         Default: 20 iterations per test
# ============================================================================
set -euo pipefail

ITERATIONS="${1:-20}"
PRISM_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_DIR="$PRISM_DIR/.github/bench_tmp"
V10_COMMIT="8928ee1"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

cleanup() {
    rm -rf "$BENCH_DIR"
}
trap cleanup EXIT

mkdir -p "$BENCH_DIR"

# --------------------------------------------------------------------------
# Generate stress-test inputs
# --------------------------------------------------------------------------
generate_stress_files() {
    # 1. Heavy defer nesting
    cat > "$BENCH_DIR/stress_defer.c" << 'CEOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int counter = 0;

void deep_defer(int depth) {
    if (depth <= 0) return;
    {
        defer counter++;
        {
            defer counter++;
            {
                defer counter++;
                deep_defer(depth - 1);
            }
        }
    }
}

int main(void) {
    // Many defer scopes
    for (int i = 0; i < 100; i++) {
        defer counter++;
        for (int j = 0; j < 10; j++) {
            defer counter += 2;
            if (j % 2 == 0) {
                defer counter += 3;
            }
        }
    }

    // Nested defer chains
    for (int round = 0; round < 50; round++) {
        {
            defer counter++;
            {
                defer counter++;
                {
                    defer counter++;
                    {
                        defer counter++;
                        {
                            defer counter++;
                        }
                    }
                }
            }
        }
    }

    // Defer with control flow
    for (int i = 0; i < 200; i++) {
        defer counter++;
        if (i % 3 == 0) continue;
        if (i % 7 == 0) break;
    }

    // Switch + defer
    for (int i = 0; i < 100; i++) {
        switch (i % 5) {
        case 0: { defer counter++; } break;
        case 1: { defer counter += 2; } break;
        case 2: { defer counter += 3; } break;
        default: { defer counter += 4; } break;
        }
    }

    printf("counter = %d\n", counter);
    return 0;
}
CEOF

    # 2. Heavy zero-init
    cat > "$BENCH_DIR/stress_zeroinit.c" << 'CEOF'
#include <stdio.h>
#include <string.h>

typedef struct { int x; int y; int z; } Point;
typedef struct { double matrix[4][4]; } Transform;
typedef struct { char name[64]; int id; float scores[10]; } Record;

int main(void) {
    // Lots of uninitialized locals (should be zero-init'd)
    int a;
    int b;
    int c;
    int d;
    int e;
    long la;
    long lb;
    float fa;
    float fb;
    double da;
    double db;
    char buf[256];
    char buf2[1024];
    int arr[100];
    int arr2[200];
    Point p1;
    Point p2;
    Point p3;
    Transform t1;
    Transform t2;
    Record r1;
    Record r2;
    Record r3;

    // Structs in loops
    for (int i = 0; i < 100; i++) {
        Point p;
        Transform t;
        Record r;
        int local_arr[50];
        char local_buf[128];
        (void)p; (void)t; (void)r; (void)local_arr; (void)local_buf;
    }

    // Pointer declarations
    int *ip;
    char *cp;
    void *vp;
    Point *pp;
    int **ipp;

    // Multi-declarator
    int x1, x2, x3, x4, x5;
    float f1, f2, f3, f4;
    char c1, c2, c3, c4, c5, c6, c7, c8;

    // VLA-like (constant size known at runtime scope)
    for (int n = 10; n <= 50; n += 10) {
        int vla[n];
        memset(vla, 0, sizeof(vla));
        (void)vla;
    }

    printf("a=%d b=%d c=%d d=%d e=%d\n", a, b, c, d, e);
    printf("la=%ld lb=%ld\n", la, lb);
    printf("fa=%f fb=%f\n", fa, fb);
    printf("da=%f db=%f\n", da, db);
    printf("buf[0]=%d arr[0]=%d\n", buf[0], arr[0]);
    printf("p1.x=%d t1.matrix[0][0]=%f\n", p1.x, t1.matrix[0][0]);
    printf("r1.id=%d\n", r1.id);
    (void)b; (void)c; (void)d; (void)e;
    (void)lb; (void)fb; (void)db;
    (void)buf2; (void)arr2; (void)p2; (void)p3; (void)t2;
    (void)r2; (void)r3;
    (void)ip; (void)cp; (void)vp; (void)pp; (void)ipp;
    (void)x1; (void)x2; (void)x3; (void)x4; (void)x5;
    (void)f1; (void)f2; (void)f3; (void)f4;
    (void)c1; (void)c2; (void)c3; (void)c4;
    (void)c5; (void)c6; (void)c7; (void)c8;
    return 0;
}
CEOF

    # 3. Large realistic file with mixed features
    {
        echo '#include <stdio.h>'
        echo '#include <stdlib.h>'
        echo '#include <string.h>'
        echo ''
        echo 'typedef struct Node { int val; struct Node *next; } Node;'
        echo 'typedef struct { char name[32]; int scores[10]; double avg; } Student;'
        echo ''

        # Generate many functions with mixed defer + zeroinit
        for i in $(seq 1 200); do
            cat << FUNC
void func_${i}(int n) {
    int result;
    char buffer[64];
    Student s;
    defer (void)0;
    for (int i = 0; i < n; i++) {
        int tmp;
        defer (void)0;
        if (i % 2 == 0) {
            defer (void)0;
            tmp = i * 2;
        }
        result += tmp;
    }
    snprintf(buffer, sizeof(buffer), "func_${i}: %d", result);
    s.avg = (double)result;
    (void)s; (void)buffer;
}

FUNC
        done

        echo 'int main(void) {'
        for i in $(seq 1 200); do
            echo "    func_${i}(10);"
        done
        echo '    printf("done\\n");'
        echo '    return 0;'
        echo '}'
    } > "$BENCH_DIR/stress_mixed.c"

    # 4. Many typedefs and type-heavy code
    {
        echo '#include <stdio.h>'
        echo '#include <stdint.h>'
        echo ''
        for i in $(seq 1 100); do
            echo "typedef struct { int f1; int f2; int f3; float f4; double f5; char name[32]; } Type_${i};"
        done
        echo ''
        echo 'int main(void) {'
        for i in $(seq 1 100); do
            echo "    Type_${i} v${i};"
            echo "    v${i}.f1 = ${i};"
            echo "    (void)v${i};"
        done
        echo '    printf("types done\\n");'
        echo '    return 0;'
        echo '}'
    } > "$BENCH_DIR/stress_types.c"

    echo "Generated stress test files in $BENCH_DIR"
    wc -l "$BENCH_DIR"/stress_*.c
}

# --------------------------------------------------------------------------
# Timing helpers
# --------------------------------------------------------------------------

# Run a command N times, collect wall-clock times (ms)
bench_command() {
    local label="$1"
    shift
    local cmd=("$@")
    local times=()
    local total=0

    for ((i = 0; i < ITERATIONS; i++)); do
        local start end elapsed
        start=$(date +%s%N)
        "${cmd[@]}" > /dev/null 2>&1
        end=$(date +%s%N)
        elapsed=$(( (end - start) / 1000000 ))  # nanoseconds -> ms
        times+=("$elapsed")
        total=$((total + elapsed))
    done

    # Sort and compute stats
    IFS=$'\n' sorted=($(sort -n <<< "${times[*]}")); unset IFS
    local count=${#sorted[@]}
    local min=${sorted[0]}
    local max=${sorted[$((count - 1))]}
    local avg=$((total / count))
    local median=${sorted[$((count / 2))]}
    local p95=${sorted[$(( (count * 95) / 100 ))]}

    printf "  %-35s  min=%4dms  avg=%4dms  median=%4dms  p95=%4dms  max=%4dms\n" \
        "$label" "$min" "$avg" "$median" "$p95" "$max"

    # Return median for comparison
    echo "$median" > "$BENCH_DIR/.last_median"
}

# --------------------------------------------------------------------------
# Build
# --------------------------------------------------------------------------
build_current() {
    echo -e "${CYAN}Building current HEAD...${RESET}"
    cd "$PRISM_DIR"
    rm -f "$BENCH_DIR/prism_current"
    cc -O2 -o "$BENCH_DIR/prism_current" prism.c 2>/dev/null
    echo "  Binary: $BENCH_DIR/prism_current"
}

build_v10() {
    echo -e "${CYAN}Building 1.0 ($V10_COMMIT)...${RESET}"
    cd "$PRISM_DIR"

    # Extract 1.0 source to temp dir
    mkdir -p "$BENCH_DIR/v10_src"
    git show "$V10_COMMIT:prism.c" > "$BENCH_DIR/v10_src/prism.c"
    git show "$V10_COMMIT:parse.c" > "$BENCH_DIR/v10_src/parse.c"

    rm -f "$BENCH_DIR/prism_v10"
    cc -O2 -o "$BENCH_DIR/prism_v10" "$BENCH_DIR/v10_src/prism.c" 2>/dev/null
    echo "  Binary: $BENCH_DIR/prism_v10"
}

# --------------------------------------------------------------------------
# Get version string
# --------------------------------------------------------------------------
get_version() {
    local binary="$1"
    "$binary" --version 2>/dev/null | head -1 || echo "unknown"
}

# --------------------------------------------------------------------------
# Binary size comparison
# --------------------------------------------------------------------------
compare_binary_size() {
    echo ""
    echo -e "${BOLD}=== BINARY SIZE ===${RESET}"
    local size_cur size_v10
    size_cur=$(stat -c%s "$BENCH_DIR/prism_current" 2>/dev/null || stat -f%z "$BENCH_DIR/prism_current")
    size_v10=$(stat -c%s "$BENCH_DIR/prism_v10" 2>/dev/null || stat -f%z "$BENCH_DIR/prism_v10")
    local diff_bytes=$((size_cur - size_v10))
    local diff_pct
    if [ "$size_v10" -gt 0 ]; then
        diff_pct=$(echo "scale=1; ($diff_bytes * 100) / $size_v10" | bc)
    else
        diff_pct="N/A"
    fi
    printf "  %-20s %s bytes\n" "1.0:" "$size_v10"
    printf "  %-20s %s bytes\n" "Current:" "$size_cur"
    printf "  %-20s %s bytes (%s%%)\n" "Delta:" "$diff_bytes" "$diff_pct"
}

# --------------------------------------------------------------------------
# Source size comparison
# --------------------------------------------------------------------------
compare_source_size() {
    echo ""
    echo -e "${BOLD}=== SOURCE SIZE ===${RESET}"
    local cur_prism cur_parse v10_prism v10_parse
    cur_prism=$(wc -l < "$PRISM_DIR/prism.c")
    cur_parse=$(wc -l < "$PRISM_DIR/parse.c")
    v10_prism=$(git show "$V10_COMMIT:prism.c" | wc -l)
    v10_parse=$(git show "$V10_COMMIT:parse.c" | wc -l)
    local cur_total=$((cur_prism + cur_parse))
    local v10_total=$((v10_prism + v10_parse))
    local delta=$((cur_total - v10_total))
    printf "  %-20s prism.c: %5d  parse.c: %5d  total: %5d lines\n" "1.0:" "$v10_prism" "$v10_parse" "$v10_total"
    printf "  %-20s prism.c: %5d  parse.c: %5d  total: %5d lines\n" "Current:" "$cur_prism" "$cur_parse" "$cur_total"
    printf "  %-20s %+d lines (%s%%)\n" "Delta:" "$delta" "$(echo "scale=1; ($delta * 100) / $v10_total" | bc)"
}

# --------------------------------------------------------------------------
# Run benchmark suite for a given binary
# --------------------------------------------------------------------------
run_suite() {
    local label="$1"
    local binary="$2"
    local version
    version=$(get_version "$binary")

    echo ""
    echo -e "${BOLD}=== $label ($version) ===${RESET}"

    # Phase 1: Transpile-only (emit to /dev/null)
    echo -e "${YELLOW}[Transpile Only]${RESET}"
    bench_command "test.c (emit)"           "$binary" transpile .github/test.c -o /dev/null
    local test_median; test_median=$(cat "$BENCH_DIR/.last_median")

    bench_command "stress_defer.c (emit)"   "$binary" transpile "$BENCH_DIR/stress_defer.c" -o /dev/null
    local defer_median; defer_median=$(cat "$BENCH_DIR/.last_median")

    bench_command "stress_zeroinit.c (emit)" "$binary" transpile "$BENCH_DIR/stress_zeroinit.c" -o /dev/null
    local zeroinit_median; zeroinit_median=$(cat "$BENCH_DIR/.last_median")

    bench_command "stress_mixed.c (emit)"   "$binary" transpile "$BENCH_DIR/stress_mixed.c" -o /dev/null
    local mixed_median; mixed_median=$(cat "$BENCH_DIR/.last_median")

    bench_command "stress_types.c (emit)"   "$binary" transpile "$BENCH_DIR/stress_types.c" -o /dev/null
    local types_median; types_median=$(cat "$BENCH_DIR/.last_median")

    # Phase 2: Full pipeline (transpile + compile + link)
    echo -e "${YELLOW}[Full Pipeline: transpile + compile]${RESET}"
    bench_command "test.c (compile)"        "$binary" "$BENCH_DIR/stress_defer.c" -o "$BENCH_DIR/bench_out"
    local compile_median; compile_median=$(cat "$BENCH_DIR/.last_median")

    bench_command "stress_mixed.c (compile)" "$binary" "$BENCH_DIR/stress_mixed.c" -o "$BENCH_DIR/bench_out"
    local compile_mixed_median; compile_mixed_median=$(cat "$BENCH_DIR/.last_median")

    # Phase 3: Full pipeline including run
    echo -e "${YELLOW}[Full Pipeline: transpile + compile + run]${RESET}"
    bench_command "stress_defer.c (run)"    "$binary" run "$BENCH_DIR/stress_defer.c"
    local run_defer_median; run_defer_median=$(cat "$BENCH_DIR/.last_median")

    # Save results for comparison
    echo "$test_median $defer_median $zeroinit_median $mixed_median $types_median $compile_median $compile_mixed_median $run_defer_median" > "$BENCH_DIR/.results_${label// /_}"
}

# --------------------------------------------------------------------------
# Comparison table
# --------------------------------------------------------------------------
print_comparison() {
    echo ""
    echo -e "${BOLD}=== PERFORMANCE COMPARISON (median ms, lower is better) ===${RESET}"
    echo ""

    local v10_results cur_results
    read -r -a v10_results < "$BENCH_DIR/.results_1.0"
    read -r -a cur_results < "$BENCH_DIR/.results_Current"

    local labels=(
        "test.c transpile"
        "stress_defer.c transpile"
        "stress_zeroinit.c transpile"
        "stress_mixed.c transpile"
        "stress_types.c transpile"
        "stress_defer.c compile"
        "stress_mixed.c compile"
        "stress_defer.c run"
    )

    printf "  ${BOLD}%-35s  %8s  %8s  %8s  %7s${RESET}\n" "Benchmark" "1.0" "Current" "Delta" "Change"
    printf "  %-35s  %8s  %8s  %8s  %7s\n" "---" "---" "---" "---" "---"

    for i in "${!labels[@]}"; do
        local v10="${v10_results[$i]}"
        local cur="${cur_results[$i]}"
        local delta=$((cur - v10))
        local pct
        if [ "$v10" -gt 0 ]; then
            pct=$(echo "scale=1; ($delta * 100) / $v10" | bc)
        else
            pct="N/A"
        fi

        local color="$RESET"
        if [ "$delta" -lt 0 ]; then
            color="$GREEN"
        elif [ "$delta" -gt 0 ]; then
            color="$RED"
        fi

        printf "  %-35s  %6dms  %6dms  ${color}%+5dms  %+6s%%${RESET}\n" \
            "${labels[$i]}" "$v10" "$cur" "$delta" "$pct"
    done
}

# --------------------------------------------------------------------------
# Profile: measure where time goes in the transpile pipeline
# --------------------------------------------------------------------------
profile_transpile() {
    local binary="$1"
    local input="$2"
    echo ""
    echo -e "${BOLD}=== PIPELINE BREAKDOWN: $input ===${RESET}"
    echo "  (Single run, approximate breakdown using strace)"

    if ! command -v strace &>/dev/null; then
        echo "  strace not available — skipping pipeline breakdown"
        return
    fi

    # Use strace to time syscalls and find the fork (preprocessor)
    local trace_file="$BENCH_DIR/strace_out"
    strace -f -T -e trace=clone,clone3,execve,write -o "$trace_file" \
        "$binary" transpile "$input" -o /dev/null 2>/dev/null || true

    # Count clones (forks for preprocessor)
    local forks preproc_execs
    forks=$(grep -c 'clone\|clone3' "$trace_file" 2>/dev/null || echo 0)
    preproc_execs=$(grep -c 'execve.*cc\|execve.*gcc\|execve.*clang' "$trace_file" 2>/dev/null || echo 0)
    local total_writes
    total_writes=$(grep -c '^[0-9].*write(' "$trace_file" 2>/dev/null || echo 0)

    printf "  fork/clone calls:  %d\n" "$forks"
    printf "  cc execve calls:   %d\n" "$preproc_execs"
    printf "  write() calls:     %d\n" "$total_writes"
    rm -f "$trace_file"
}

# --------------------------------------------------------------------------
# Perf stat (if available)
# --------------------------------------------------------------------------
perf_stat_run() {
    local label="$1"
    local binary="$2"
    shift 2

    if ! command -v perf &>/dev/null; then
        return
    fi

    echo ""
    echo -e "${BOLD}=== PERF STAT: $label ===${RESET}"
    perf stat -r "$ITERATIONS" "$binary" "$@" -o /dev/null 2>&1 | \
        grep -E 'task-clock|instructions|cycles|cache-misses|page-faults|seconds' || true
}

# --------------------------------------------------------------------------
# Memory usage (peak RSS)
# --------------------------------------------------------------------------
measure_memory() {
    local label="$1"
    local binary="$2"
    shift 2

    echo ""
    echo -e "${BOLD}=== PEAK MEMORY: $label ===${RESET}"

    if command -v /usr/bin/time &>/dev/null; then
        local mem
        mem=$(/usr/bin/time -v "$binary" "$@" -o /dev/null 2>&1 | grep "Maximum resident" | awk '{print $NF}')
        printf "  Peak RSS: %s KB\n" "$mem"
    else
        echo "  /usr/bin/time not available — skipping memory measurement"
    fi
}

# ============================================================================
# MAIN
# ============================================================================
echo -e "${BOLD}╔══════════════════════════════════════════╗${RESET}"
echo -e "${BOLD}║       PRISM BENCHMARK SUITE              ║${RESET}"
echo -e "${BOLD}║  Iterations: $ITERATIONS per test                 ║${RESET}"
echo -e "${BOLD}╚══════════════════════════════════════════╝${RESET}"
echo ""

generate_stress_files
build_current
build_v10

compare_source_size
compare_binary_size

run_suite "1.0"     "$BENCH_DIR/prism_v10"
run_suite "Current" "$BENCH_DIR/prism_current"

print_comparison

# Additional profiling for current version
profile_transpile "$BENCH_DIR/prism_current" "$BENCH_DIR/stress_mixed.c"

# Memory usage comparison
measure_memory "1.0 stress_mixed.c"     "$BENCH_DIR/prism_v10"     transpile "$BENCH_DIR/stress_mixed.c"
measure_memory "Current stress_mixed.c" "$BENCH_DIR/prism_current" transpile "$BENCH_DIR/stress_mixed.c"

# Perf stat if available
perf_stat_run "Current transpile test.c" "$BENCH_DIR/prism_current" transpile .github/test.c

echo ""
echo -e "${BOLD}=== BENCHMARK COMPLETE ===${RESET}"

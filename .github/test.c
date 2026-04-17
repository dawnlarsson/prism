// Massive test suite for Prism C transpiler trying to break it....
// Tests: defer, zero-init, typedef tracking, multi-declarator, edge cases, library API
// Run with: $ prism run .github/test.c

#define PRISM_LIB_MODE
#include "../prism.c"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#ifndef _WIN32
#include <sys/resource.h>
#include <pthread.h>
#include <sys/time.h>
#include <spawn.h>
#include <fcntl.h>
extern char **environ;
#endif

// Per-thread log buffer (used by defer tests to verify execution order)
// and per-thread test counters for parallel suite execution.
static _Thread_local char log_buffer[1024];
static _Thread_local int log_pos = 0;
static _Thread_local int passed = 0;
static _Thread_local int failed = 0;
static _Thread_local int total = 0;

static void log_reset(void) {
	log_buffer[0] = 0;
	log_pos = 0;
}

static void log_append(const char *s) {
	int len = strlen(s);
	if (log_pos + len < 1023) {
		strcpy(log_buffer + log_pos, s);
		log_pos += len;
	}
}

#define CHECK(cond, name)                                                                                    \
	do {                                                                                                 \
		total++;                                                                                     \
		if (cond) {                                                                                  \
			printf("[PASS] %s\n", name);                                                         \
			passed++;                                                                            \
		} else {                                                                                     \
			printf("[FAIL] %s\n", name);                                                         \
			failed++;                                                                            \
		}                                                                                            \
	} while (0)

#define CHECK_LOG(expected, name)                                                                            \
	do {                                                                                                 \
		total++;                                                                                     \
		if (strcmp(log_buffer, expected) == 0) {                                                     \
			printf("[PASS] %s\n", name);                                                         \
			passed++;                                                                            \
		} else {                                                                                     \
			printf("[FAIL] %s: expected '%s', got '%s'\n", name, expected, log_buffer);          \
			failed++;                                                                            \
		}                                                                                            \
	} while (0)

#define CHECK_EQ(got, expected, name)                                                                        \
	do {                                                                                                 \
		int _got = (got), _exp = (expected);                                                         \
		total++;                                                                                     \
		if (_got == _exp) {                                                                          \
			printf("[PASS] %s\n", name);                                                         \
			passed++;                                                                            \
		} else {                                                                                     \
			printf("[FAIL] %s: expected %d, got %d\n", name, _exp, _got);                        \
			failed++;                                                                            \
		}                                                                                            \
	} while (0)

// Platform-conditional test execution.
// Use in runner functions to replace #ifdef/#endif boilerplate:
//   GNUC_ONLY(test_foo());           // GCC + Clang only
//   UNIX_ONLY(test_bar());           // non-Windows only
//   NOMSVC_ONLY(test_baz());         // non-MSVC only
#ifdef __GNUC__
#define GNUC_ONLY(...) __VA_ARGS__
#else
#define GNUC_ONLY(...)
#endif

#ifndef _WIN32
#define UNIX_ONLY(...) __VA_ARGS__
#else
#define UNIX_ONLY(...)
#endif

#ifndef _MSC_VER
#define NOMSVC_ONLY(...) __VA_ARGS__
#else
#define NOMSVC_ONLY(...)
#endif

// Detect whether transpiler output uses memset (GCC) or byte loop (MSVC).
// MSVC has no __builtin_memset; prism uses __prism_p_ byte loops on MSVC.
static bool has_zeroing(const char *output) {
	return strstr(output, "memset") != NULL || strstr(output, "__prism_p_") != NULL;
}
static bool has_var_zeroing(const char *output, const char *var) {
	char pattern[128];
	snprintf(pattern, sizeof(pattern), "memset(&%s", var);
	if (strstr(output, pattern)) return true;
	snprintf(pattern, sizeof(pattern), "char *)&%s;", var);
	return strstr(output, pattern) != NULL;
}

static const char *test_tmp_dir(void) {
	static _Thread_local char buf[PATH_MAX];
	const char *t = getenv("TMPDIR");
#ifdef _WIN32
	if (!t || !*t) t = getenv("TEMP");
	if (!t || !*t) t = getenv("TMP");
	if (!t || !*t) t = ".";
	size_t len = strlen(t);
	char sep = '\\';
	snprintf(buf, sizeof(buf), "%s%s", t, (t[len - 1] == '\\' || t[len - 1] == '/') ? "" : "\\");
#else
	if (!t || !*t) t = "/tmp";
	size_t len = strlen(t);
	snprintf(buf, sizeof(buf), "%s%s", t, (t[len - 1] == '/') ? "" : "/");
#endif
	return buf;
}

static char *create_temp_file(const char *content) {
	char *path = malloc(PATH_MAX);
	snprintf(path, PATH_MAX, "%sprism_test_XXXXXX.c", test_tmp_dir());
	int fd = mkstemps(path, 2);
	if (fd < 0) {
		free(path);
		return NULL;
	}
	write(fd, content, strlen(content));
	close(fd);
	return path;
}

// mkdtemp helper that respects $TMPDIR. Writes into caller's buffer.
// Usage: char tmpdir[PATH_MAX]; test_mkdtemp(tmpdir, "prism_foo_");
static char *test_mkdtemp(char *buf, const char *prefix) {
	snprintf(buf, PATH_MAX, "%s%sXXXXXX", test_tmp_dir(), prefix);
	return mkdtemp(buf);
}

// mkstemp helper that respects $TMPDIR. Writes into caller's buffer.
static int test_mkstemp(char *buf, const char *prefix) {
	snprintf(buf, PATH_MAX, "%s%sXXXXXX", test_tmp_dir(), prefix);
	return mkstemp(buf);
}

static long get_memory_usage_kb(void) {
#ifdef __linux__
	FILE *f = fopen("/proc/self/status", "r");
	if (!f) return 0;
	char line[256];
	long vmrss = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "VmRSS:", 6) == 0) {
			sscanf(line + 6, "%ld", &vmrss);
			break;
		}
	}
	fclose(f);
	return vmrss;
#else
	return 0;
#endif
}

static bool is_emulated(void) {
#ifdef __linux__
	if (getenv("PRISM_EMULATED")) return true;
#if defined(__aarch64__) || defined(__arm__) || defined(__riscv) || \
    defined(__s390x__) || defined(__mips__) || defined(__powerpc__)
	/* User-mode QEMU: host cpuinfo leaks x86 fields into non-x86 process */
	FILE *f = fopen("/proc/cpuinfo", "r");
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "vendor_id", 9) == 0 ||
			    strncmp(line, "model name", 10) == 0) {
				fclose(f);
				return true;
			}
		}
		fclose(f);
	}
	/* Docker QEMU: /.dockerenv exists when running inside a container */
	if (access("/.dockerenv", F_OK) == 0)
		return true;
	/* System-mode QEMU: device tree advertises the virtual machine */
	f = fopen("/sys/firmware/devicetree/base/compatible", "rb");
	if (f) {
		char buf[256];
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = 0;
		for (size_t i = 0; i < n; i++)
			if (buf[i] == 0) buf[i] = ' ';
		if (strstr(buf, "qemu") || strstr(buf, "virt"))
			return true;
	}
#endif
#endif
	return false;
}

#ifndef _WIN32
// Serialization mutex shared by ALL test-harness process spawns.
// Held ONLY across the posix_spawnp call itself; waitpid runs outside the
// critical section so spawned processes execute concurrently and parent
// threads continue working in parallel.
//
// Why this exists: the previous harness used fork()+execvp() and system(3).
// fork() from a multithreaded process duplicates only the calling thread
// but leaves the rest of the process's shared state (malloc heap, libc
// pthread bookkeeping, dispatch queues) in whatever mid-transaction state
// the other threads happened to be in when the fork syscall was issued.
// On macOS arm64 this produced intermittent SIGBUS crashes in the parent
// (wild pointers observed in tokenize(), _platform_strstr(), etc.) because
// libc internal structures were observed half-written. posix_spawn is an
// atomic kernel operation on macOS (darwin_spawn) that does not fork the
// calling process — no mid-transaction state is ever visible to the child
// or disturbed in the parent. The mutex here is defense-in-depth so that
// no two spawns overlap even on platforms where posix_spawn might fall
// back to a fork-based implementation.
static pthread_mutex_t g_spawn_mtx = PTHREAD_MUTEX_INITIALIZER;

// Core spawn-and-wait helper. argv must be a NULL-terminated array, argv[0]
// is looked up via $PATH by posix_spawnp. If stdout_path/stderr_path are
// non-NULL, the child's stdout/stderr is redirected to those files (created
// O_WRONLY|O_CREAT|O_TRUNC, mode 0644).  Returns the child's exit status
// (>=0, 127 if exec failed inside posix_spawn), -1 on infrastructure error.
static int prism_spawn_wait(char *const argv[], const char *stdout_path,
			    const char *stderr_path) {
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	if (stdout_path)
		posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, stdout_path,
						 O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (stderr_path)
		posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, stderr_path,
						 O_WRONLY | O_CREAT | O_TRUNC, 0644);

	pid_t pid = 0;
	pthread_mutex_lock(&g_spawn_mtx);
	int err = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
	pthread_mutex_unlock(&g_spawn_mtx);
	posix_spawn_file_actions_destroy(&fa);
	if (err != 0) { errno = err; return -1; }

	int status = 0;
	while (waitpid(pid, &status, 0) == -1)
		if (errno != EINTR) return -1;
	if (!WIFEXITED(status)) return -1;
	return WEXITSTATUS(status);
}

// Run an arbitrary shell command via /bin/sh -c. Preserves shell semantics
// (redirections, pipes, globbing) that callers embed in `cmd`.
static int run_command_status(const char *cmd) {
	char *argv[] = {(char *)"/bin/sh", (char *)"-c", (char *)cmd, NULL};
	return prism_spawn_wait(argv, NULL, NULL);
}

static void check_transpiled_output_compiles_and_runs(const char *output,
					      const char *compile_name,
					      const char *run_name) {
	char *src_path = create_temp_file(output);
	CHECK(src_path != NULL, "compile helper: create temp source");
	if (!src_path) return;

	char bin_template[PATH_MAX];
	int bin_fd = test_mkstemp(bin_template, "prism_defer_exec_");
	CHECK(bin_fd >= 0, "compile helper: reserve temp binary path");
	if (bin_fd < 0) {
		unlink(src_path);
		free(src_path);
		return;
	}
	close(bin_fd);
	unlink(bin_template);

	char compile_cmd[PATH_MAX * 2 + 64];
	snprintf(compile_cmd, sizeof(compile_cmd),
		 "cc -std=gnu11 -o %s %s >/dev/null 2>&1", bin_template, src_path);
	CHECK_EQ(run_command_status(compile_cmd), 0, compile_name);
	if (access(bin_template, X_OK) == 0)
		CHECK_EQ(run_command_status(bin_template), 0, run_name);

	unlink(bin_template);
	unlink(src_path);
	free(src_path);
}
#endif

#include "test.windows.c"

#include "test.safe.c"
#include "test.raw.c"
#include "test.parse.c"
#include "test.defer.c"
#include "test.orelse.c"
#include "test.zeroinit.c"
#include "test.harsh.c"
#include "test.api.c"
#include "test.golf.c"
#include "test.autostatic.c"
#include "test.autounreach.c"
#include "test.spec.c"
#include "test.bounds.c"

typedef struct {
	const char *name;
	void (*func)(void);
	int passed, failed, total;
	double elapsed;
} TestSuite;

#ifndef _WIN32
static void *suite_thread(void *arg) {
	TestSuite *s = (TestSuite *)arg;
	struct timeval st0, st1;
	gettimeofday(&st0, NULL);
	passed = failed = total = 0;
	s->func();
	s->passed = passed;
	s->failed = failed;
	s->total = total;
	gettimeofday(&st1, NULL);
	s->elapsed = (st1.tv_sec - st0.tv_sec) + (st1.tv_usec - st0.tv_usec) / 1e6;
	prism_thread_cleanup();
	return NULL;
}
#endif

int main(void) {
	TestSuite suites[] = {
		{"windows",  run_windows_tests},
		{"safe",     run_safe_tests},
		{"raw",      run_raw_tests},
		{"defer",    run_defer_tests},
		{"zeroinit", run_zeroinit_tests},
		{"parse",    run_parse_tests},
		{"orelse",   run_orelse_tests},
		{"harsh",    run_harsh_review_tests},
		{"api_1",    run_api_tests_1},
		{"api_2",    run_api_tests_2},
		{"api_3",    run_api_tests_3},
		{"api_4",    run_api_tests_4},
		{"golf",     run_golf_tests},
		{"autostatic", run_autostatic_tests},
		{"autounreach", run_auto_unreachable_tests},
		{"spec",     run_spec_tests},
		{"bounds",   run_bounds_check_tests},
	};
	int n = sizeof(suites) / sizeof(suites[0]);

	printf("=== PRISM TEST SUITE (%d suites, parallel) ===\n", n);

#ifndef _WIN32
	struct timeval t0, t1;
	gettimeofday(&t0, NULL);

	pthread_t threads[sizeof(suites) / sizeof(suites[0])];
	// Force an 8 MiB stack per worker.  musl's default pthread stack is
	// 128 KiB, which the recursive parser + deep test inputs overflow (seen
	// as SIGSEGV/SIGBUS on Alpine x86_64 and arm64 CI at random suites).
	// glibc (8 MiB) and macOS (512 KiB) normally survive; pin a known-good
	// value everywhere so the test harness is not libc-dependent.
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
	for (int i = 0; i < n; i++)
		pthread_create(&threads[i], &attr, suite_thread, &suites[i]);
	pthread_attr_destroy(&attr);
	for (int i = 0; i < n; i++)
		pthread_join(threads[i], NULL);

	gettimeofday(&t1, NULL);
	double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
#else
	// Windows: run serially
	for (int i = 0; i < n; i++) {
		passed = failed = total = 0;
		suites[i].func();
		suites[i].passed = passed;
		suites[i].failed = failed;
		suites[i].total = total;
	}
	double elapsed = 0;
#endif

	int total_pass = 0, total_fail = 0, total_tests = 0;
	for (int i = 0; i < n; i++) {
		total_pass += suites[i].passed;
		total_fail += suites[i].failed;
		total_tests += suites[i].total;
	}

	printf("\n--- Suite Timing ---\n");
	for (int i = 0; i < n; i++)
		printf("  %-10s %4d tests  %5.2fs\n", suites[i].name, suites[i].total, suites[i].elapsed);

	printf("\n========================================\n");
	printf("TOTAL: %d tests, %d passed, %d failed", total_tests, total_pass, total_fail);
#ifndef _WIN32
	printf(" (%.2fs wall)\n", elapsed);
#else
	printf("\n");
#endif
	printf("========================================\n");

	return (total_fail == 0) ? 0 : 1;
}

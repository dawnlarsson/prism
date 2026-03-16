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
#endif

static char log_buffer[1024];
static int log_pos = 0;
static int passed = 0;
static int failed = 0;
static int total = 0;

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
		total++;                                                                                     \
		if ((got) == (expected)) {                                                                   \
			printf("[PASS] %s\n", name);                                                         \
			passed++;                                                                            \
		} else {                                                                                     \
			printf("[FAIL] %s: expected %d, got %d\n", name, (int)(expected), (int)(got));       \
			failed++;                                                                            \
		}                                                                                            \
	} while (0)

static const char *test_tmp_dir(void) {
	static char buf[PATH_MAX];
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


#include "test.safe.c"
#include "test.raw.c"
#include "test.parse.c"
#include "test.defer.c"
#include "test.orelse.c"
#include "test.zeroinit.c"
#include "test.harsh.c"
#include "test.api.c"

int main(void) {
	printf("=== PRISM TEST SUITE ===\n");

	run_safe_tests();
	run_raw_tests();
	run_defer_tests();
	run_zeroinit_tests();
	run_parse_tests();
	run_orelse_tests();
	run_harsh_review_tests();
	run_api_tests();

	printf("\n========================================\n");
	printf("TOTAL: %d tests, %d passed, %d failed\n", total, passed, failed);
	printf("========================================\n");

	return (failed == 0) ? 0 : 1;
}

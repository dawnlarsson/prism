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
#include <sys/resource.h>

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

static char *create_temp_file(const char *content) {
	char *path = malloc(64);
	snprintf(path, 64, "/tmp/prism_test_XXXXXX.c");
	int fd = mkstemps(path, 2);
	if (fd < 0) {
		free(path);
		return NULL;
	}
	write(fd, content, strlen(content));
	close(fd);
	return path;
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
#include "test.bugs.c"
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
	run_bug_report_tests();
	run_api_tests();

	printf("\n========================================\n");
	printf("TOTAL: %d tests, %d passed, %d failed\n", total, passed, failed);
	printf("========================================\n");

	return (failed == 0) ? 0 : 1;
}

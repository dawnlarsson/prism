// Massive test suite for Prism C transpiler trying to break it....
// Tests: defer, zero-init, typedef tracking, multi-declarator, edge cases
// Run with: $ prism run .github/test.c

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

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


#include "test.safe.c"
#include "test.raw.c"
#include "test.parse.c"
#include "test.defer.c"
#include "test.orelse.c"
#include "test.zeroinit.c"
#include "test.harsh.c"

int main(void) {
	printf("=== PRISM TEST SUITE ===\n");

	run_safe_tests();
	run_raw_tests();
	run_defer_tests();
	run_zeroinit_tests();
	run_parse_tests();
	run_orelse_tests();
	run_harsh_review_tests();

	printf("\n========================================\n");
	printf("TOTAL: %d tests, %d passed, %d failed\n", total, passed, failed);
	printf("========================================\n");

	return (failed == 0) ? 0 : 1;
}

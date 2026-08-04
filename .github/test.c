/*
 * Prism's entire executable test suite is data below. There is one case
 * generator (recipe_run): it expands zero to four axes with an odometer,
 * renders the selected values into a source template, and sends every cell
 * through the same oracle pipeline.  New coverage is a row, never a new test
 * function.
 */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Test-only allocation fault injection. Including the libc declarations
 * before these macros keeps system prototypes intact; only direct allocation
 * calls in the embedded Prism implementation are redirected. */
#ifdef _MSC_VER
#define TEST_THREAD_LOCAL __declspec(thread)
#else
#define TEST_THREAD_LOCAL _Thread_local
#endif

static TEST_THREAD_LOCAL long fault_alloc_at;
static TEST_THREAD_LOCAL long fault_alloc_calls;
static TEST_THREAD_LOCAL int fault_alloc_fired;
static TEST_THREAD_LOCAL const char *fault_alloc_file;
static TEST_THREAD_LOCAL int fault_alloc_line;
static TEST_THREAD_LOCAL int fault_realloc_must_move;
#define FAULT_LIVE_CAP 4096
static TEST_THREAD_LOCAL void *fault_live[FAULT_LIVE_CAP];
static TEST_THREAD_LOCAL const char *fault_live_file[FAULT_LIVE_CAP];
static TEST_THREAD_LOCAL int fault_live_line[FAULT_LIVE_CAP];
static TEST_THREAD_LOCAL uint64_t fault_live_id[FAULT_LIVE_CAP];
static TEST_THREAD_LOCAL size_t fault_live_size[FAULT_LIVE_CAP];
static TEST_THREAD_LOCAL size_t fault_live_count;
static TEST_THREAD_LOCAL int fault_live_overflow;
static TEST_THREAD_LOCAL uint64_t fault_live_next_id;

static size_t fault_live_find(void *ptr) {
	for (size_t i = 0; i < fault_live_count; i++)
		if (fault_live[i] == ptr) return i;
	return SIZE_MAX;
}

static void fault_live_add(void *ptr, size_t size, const char *file, int line) {
	if (!ptr) return;
	if (fault_live_count == FAULT_LIVE_CAP) {
		fault_live_overflow = 1;
		return;
	}
	fault_live[fault_live_count] = ptr;
	fault_live_file[fault_live_count] = file;
	fault_live_line[fault_live_count] = line;
	fault_live_id[fault_live_count] = ++fault_live_next_id;
	fault_live_size[fault_live_count] = size;
	fault_live_count++;
}

static void fault_live_remove_at(size_t i) {
	if (i == SIZE_MAX) return;
	fault_live_count--;
	fault_live[i] = fault_live[fault_live_count];
	fault_live_file[i] = fault_live_file[fault_live_count];
	fault_live_line[i] = fault_live_line[fault_live_count];
	fault_live_id[i] = fault_live_id[fault_live_count];
	fault_live_size[i] = fault_live_size[fault_live_count];
}

static int fault_alloc_should_fail(const char *file, int line) {
	if (fault_alloc_at <= 0) return 0;
	fault_alloc_calls++;
	if (fault_alloc_calls != fault_alloc_at) return 0;
	fault_alloc_fired = 1;
	fault_alloc_file = file;
	fault_alloc_line = line;
	return 1;
}

static void *fault_malloc(size_t size, const char *file, int line) {
	if (fault_alloc_should_fail(file, line)) return NULL;
	void *ptr = malloc(size);
	fault_live_add(ptr, size, file, line);
	return ptr;
}

static void *fault_calloc(size_t count, size_t size, const char *file, int line) {
	if (fault_alloc_should_fail(file, line)) return NULL;
	void *ptr = calloc(count, size);
	fault_live_add(ptr, count * size, file, line);
	return ptr;
}

static void *fault_realloc(void *ptr, size_t size, const char *file, int line) {
	if (fault_alloc_should_fail(file, line)) return NULL;
	size_t tracked = fault_live_find(ptr);
	void *resized;
	/* Exercise the legal worst case for ownership: every tracked realloc moves.
	 * Allocating before freeing also guarantees a distinct address, catching
	 * code that only works when a platform happens to grow a block in place. */
	if (fault_realloc_must_move && ptr && size && tracked != SIZE_MAX) {
		resized = malloc(size);
		if (resized) {
			size_t copy = fault_live_size[tracked] < size ? fault_live_size[tracked] : size;
			memcpy(resized, ptr, copy);
			free(ptr);
		}
	} else {
		resized = realloc(ptr, size);
	}
	if (!resized) {
		if (size == 0) fault_live_remove_at(tracked);
		return NULL;
	}
	if (tracked == SIZE_MAX) fault_live_add(resized, size, file, line);
	else {
		fault_live[tracked] = resized;
		fault_live_file[tracked] = file;
		fault_live_line[tracked] = line;
		fault_live_size[tracked] = size;
	}
	return resized;
}

static void fault_free(void *ptr) {
	fault_live_remove_at(fault_live_find(ptr));
	free(ptr);
}

#define malloc(size) fault_malloc((size), __FILE__, __LINE__)
#define calloc(count, size) fault_calloc((count), (size), __FILE__, __LINE__)
#define realloc(ptr, size) fault_realloc((ptr), (size), __FILE__, __LINE__)
#define free(ptr) fault_free(ptr)
#define PRISM_LIB_MODE
#include "../prism.c"
#undef free
#undef realloc
#undef calloc
#undef malloc

#include <stdarg.h>

#ifndef _WIN32
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

/* Test code allocates outside the Prism wrappers, but it also receives blocks
 * allocated inside Prism from direct helper calls. Route every deallocation
 * through the tracker: untracked test allocations pass through unchanged,
 * while Prism-owned blocks cannot leave stale address/size records behind. */
#define free(ptr) fault_free(ptr)

#define N(a) (sizeof(a) / sizeof((a)[0]))

enum {
	FB_DEFER = 1u << 0,
	FB_ZERO = 1u << 1,
	FB_LINE = 1u << 2,
	FB_WARN = 1u << 3,
	FB_FLAT = 1u << 4,
	FB_ORELSE = 1u << 5,
	FB_AUR = 1u << 6,
	FB_AS = 1u << 7,
	FB_BOUNDS = 1u << 8,
};

enum {
	CAP_NONE = 0,
	CAP_POSIX = 1u << 0,
	CAP_VLA = 1u << 1,
	CAP_GNU = 1u << 2,
	CAP_WINDOWS = 1u << 3,
};

enum { AF_NONE, AF_INCLUDE, AF_DEFINE, AF_CFLAG, AF_FORCE, AF_COMPILER };

enum {
	O_OK = 1u << 0,
	O_REJECT = 1u << 1,
	O_DIAG = 1u << 2,
	O_NO_EXT = 1u << 3,
	O_FIXED = 1u << 4,
	O_COMPILE = 1u << 5,
	O_RUN = 1u << 6,
	O_TRICHOTOMY = 1u << 7,
	O_OUTPUT_EQ_OFF = 1u << 8,
	O_FILE = 1u << 9,
	O_NULL_SOURCE = 1u << 10,
	O_LIFECYCLE = 1u << 11,
	O_CLI = 1u << 12,
	O_ANY_STATUS = 1u << 13,
	O_DRIVER_TRANSPILE = 1u << 14,
	O_VERIFY = 1u << 15,
	O_INPUT_PATH = 1u << 16,
	O_REPLAY = 1u << 17,
	O_INTERNAL = 1u << 18,
	O_TRAP = 1u << 19,
	O_REFERENCE_RUN = 1u << 20,
	O_ALLOC_FAIL = 1u << 21,
	/* A generated 2^7 power set.  Unlike the broad trichotomy grammar
	 * product, every cell here has a concrete parse, emission, fixed-point,
	 * and (where a host compiler is available) execution contract for the
	 * five user-facing transformations. */
	O_FEATURE_MATRIX = 1u << 22,
};

typedef struct {
	const char *tag;
	const char *text;
	unsigned set_features;
	unsigned clear_features;
	unsigned oracle;
	int expected_status_plus_one;
	int api_field;
	const char **api_values;
	int api_count;
	const char *api_compiler;
} AxisValue;

typedef struct {
	const char *name;
	const AxisValue *values;
	size_t count;
} Axis;

typedef struct {
	const char *id;
	const char *source;
	const char *preamble;
	const Axis *axes[4];
	unsigned oracle;
	unsigned set_features;
	unsigned clear_features;
	unsigned requires;
	const char *must_have;
	const char *must_not_have;
	const char *diagnostic;
	int expected_exit;
	const char *sequence;
	const char *const *argv;
	int argc;
	int cli_mode;
	int cli_action;
	int cli_sources;
	int cli_cc_args;
} Recipe;

typedef struct {
	long cells;
	long passed;
	long failed;
	long skipped;
	long infra; /* spawns the machine refused, not verdicts */
	char first[768];
} Stats;

static void fail(Stats *st, const Recipe *r, const AxisValue *sel[4], const char *fmt, ...) {
	st->failed++;
	char msg[768];
	int n = snprintf(msg, sizeof(msg), "%s", r->id);
	for (int i = 0; i < 4 && r->axes[i]; i++)
		n += snprintf(msg + n, sizeof(msg) - (size_t)n, "/%s=%s",
			      r->axes[i]->name, sel[i]->tag);
	n += snprintf(msg + n, sizeof(msg) - (size_t)n, ": ");
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg + n, sizeof(msg) - (size_t)n, fmt, ap);
	va_end(ap);
	if (!st->first[0]) snprintf(st->first, sizeof(st->first), "%s", msg);
	if (getenv("PRISM_RECIPE_VERBOSE")) fprintf(stderr, "FAIL: %s\n", msg);
}

static unsigned feature_bits(PrismFeatures f) {
	return (f.defer ? FB_DEFER : 0) | (f.zeroinit ? FB_ZERO : 0) |
	       (f.line_directives ? FB_LINE : 0) | (f.warn_safety ? FB_WARN : 0) |
	       (f.flatten_headers ? FB_FLAT : 0) | (f.orelse ? FB_ORELSE : 0) |
	       (f.auto_unreachable ? FB_AUR : 0) | (f.auto_static ? FB_AS : 0) |
	       (f.bounds_check ? FB_BOUNDS : 0);
}

static PrismFeatures patch_features(PrismFeatures f, unsigned set, unsigned clear) {
	unsigned b = (feature_bits(f) | set) & ~clear;
	f.defer = (b & FB_DEFER) != 0;
	f.zeroinit = (b & FB_ZERO) != 0;
	f.line_directives = (b & FB_LINE) != 0;
	f.warn_safety = (b & FB_WARN) != 0;
	f.flatten_headers = (b & FB_FLAT) != 0;
	f.orelse = (b & FB_ORELSE) != 0;
	f.auto_unreachable = (b & FB_AUR) != 0;
	f.auto_static = (b & FB_AS) != 0;
	f.bounds_check = (b & FB_BOUNDS) != 0;
	f.quiet = true;
	return f;
}

static PrismFeatures patch_api_features(PrismFeatures f, const AxisValue *v) {
	if (!v) return f;
	switch (v->api_field) {
	case AF_INCLUDE: f.include_paths = v->api_values; f.include_count = v->api_count; break;
	case AF_DEFINE: f.defines = v->api_values; f.define_count = v->api_count; break;
	case AF_CFLAG: f.compiler_flags = v->api_values; f.compiler_flags_count = v->api_count; break;
	case AF_FORCE: f.force_includes = v->api_values; f.force_include_count = v->api_count; break;
	case AF_COMPILER: f.compiler = v->api_compiler; break;
	default: break;
	}
	return f;
}

static unsigned capabilities(void) {
	unsigned c = 0;
#ifndef _WIN32
	c |= CAP_POSIX | CAP_VLA;
#else
	c |= CAP_WINDOWS;
#endif
#if defined(__GNUC__) || defined(__clang__)
	c |= CAP_GNU;
#endif
	return c;
}

static int render_into(char *out, size_t cap, size_t *n, const char *src,
		       const AxisValue *sel[4], long cell, int depth) {
	if (depth > 8) return 0;
	for (const char *p = src ? src : ""; *p;) {
		if (!strncmp(p, "@cell@", 6)) {
			int z = snprintf(out + *n, cap - *n, "%ld", cell);
			if (z < 0 || *n + (size_t)z >= cap) return 0;
			*n += (size_t)z;
			p += 6;
			continue;
		}
		if (p[0] == '@' && p[1] >= '0' && p[1] <= '3' && p[2] == '@') {
			int hit = p[1] - '0';
			const char *value = sel[hit] && sel[hit]->text ? sel[hit]->text : "";
			if (!render_into(out, cap, n, value, sel, cell, depth + 1)) return 0;
			p += 3;
			continue;
		}
		if (*n + 1 >= cap) return 0;
		out[(*n)++] = *p++;
	}
	return 1;
}

static char *render(const Recipe *r, const AxisValue *sel[4], long cell) {
	const size_t cap = 131072;
	char *out = malloc(cap);
	if (!out) return NULL;
	size_t n = 0;
	if (!render_into(out, cap, &n, r->preamble, sel, cell, 0) ||
	    !render_into(out, cap, &n, r->source, sel, cell, 0)) {
		free(out);
		return NULL;
	}
	out[n] = 0;
	return out;
}

static int is_ident(int c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '_';
}

static int count_kw(const char *s, const char *kw) {
	if (!s) return 0;
	int n = 0;
	size_t z = strlen(kw);
	for (const char *p = s; (p = strstr(p, kw)) != NULL; p++)
		if ((p == s || !is_ident((unsigned char)p[-1])) && !is_ident((unsigned char)p[z])) n++;
	return n;
}

static char *normalize(const char *s) {
	if (!s) return NULL;
	char *o = malloc(strlen(s) + 1);
	if (!o) return NULL;
	size_t n = 0;
	int bol = 1;
	for (const char *p = s; *p;) {
		if (bol && *p == '#') {
			while (*p && *p != '\n') p++;
			bol = 1;
			continue;
		}
		if (*p == '\n') { bol = 1; p++; continue; }
		if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\f') { p++; continue; }
		bol = 0;
		o[n++] = *p++;
	}
	o[n] = 0;
	return o;
}

static int normalized_equal(const char *a, const char *b) {
	char *na = normalize(a), *nb = normalize(b);
	int ok = na && nb && !strcmp(na, nb);
	free(na);
	free(nb);
	return ok;
}

static void fault_alloc_disable(void) {
	fault_alloc_at = 0;
	fault_alloc_calls = 0;
	fault_alloc_fired = 0;
	fault_alloc_file = NULL;
	fault_alloc_line = 0;
}

static void fault_alloc_enable(long fail_at) {
	fault_alloc_calls = 0;
	fault_alloc_fired = 0;
	fault_alloc_file = NULL;
	fault_alloc_line = 0;
	fault_alloc_at = fail_at;
}

static TEST_THREAD_LOCAL uint64_t fault_live_baseline_id[FAULT_LIVE_CAP];
static TEST_THREAD_LOCAL size_t fault_live_baseline_count;

static int fault_alloc_tracker_snapshot(char *why, size_t why_cap) {
	if (fault_live_overflow) {
		snprintf(why, why_cap, "allocation tracker overflowed");
		return 0;
	}
	fault_live_baseline_count = fault_live_count;
	memcpy(fault_live_baseline_id, fault_live_id, fault_live_count * sizeof(uint64_t));
	return 1;
}

static int fault_alloc_tracker_matches(char *why, size_t why_cap, const char *phase) {
	const char *file = "-";
	int line = 0;
	int has_new = fault_live_overflow;
	for (size_t i = 0; i < fault_live_count; i++) {
		int expected = 0;
		for (size_t j = 0; j < fault_live_baseline_count; j++)
			if (fault_live_id[i] == fault_live_baseline_id[j]) expected = 1;
		if (!expected) {
			file = fault_live_file[i];
			line = fault_live_line[i];
			has_new = 1;
			break;
		}
	}
	if (!has_new) return 1;
	snprintf(why, why_cap, "%s changed tracked allocations: %zu -> %zu, first new site %s:%d%s",
		 phase, fault_live_baseline_count, fault_live_count, file, line,
		 fault_live_overflow ? " (tracker overflowed)" : "");
	return 0;
}

static int prism_result_shape_ok(const PrismResult *r) {
	if (r->status < PRISM_OK || r->status > PRISM_ERR_IO) return 0;
	if (r->status == PRISM_OK)
		return r->output && !r->error_msg && r->output_len == strlen(r->output);
	return !r->output && r->output_len == 0 && r->error_msg && r->error_msg[0];
}

static int prism_result_equal(const PrismResult *a, const PrismResult *b) {
	if (a->status != b->status || a->error_line != b->error_line || a->error_col != b->error_col) return 0;
	if (a->status == PRISM_OK)
		return a->output && b->output && a->output_len == b->output_len && !strcmp(a->output, b->output);
	return a->error_msg && b->error_msg && !strcmp(a->error_msg, b->error_msg);
}

static PrismResult alloc_fault_transpile(const char *input, int file_api, PrismFeatures f) {
	return file_api ? prism_transpile_file(input, f)
			: prism_transpile_source(input, "alloc-fault.c", f);
}

static int run_alloc_failure_sweep(const char *src, int file_api, int expect_ok, int expect_reject,
				   PrismFeatures f, char *why, size_t why_cap) {
	int ok = 0;
	const char *input = src;
	const char *site_file = NULL;
	char path[256] = "";
	long allocation_count = 0;
	long i = 0;
	int fired = 0;
	int site_line = 0;
	int tracker_baseline_set = 0;
	PrismResult base = {0}, measured = {0}, failed = {0}, recovery = {0};
#ifndef _WIN32
	if (file_api) {
		static unsigned serial;
		snprintf(path, sizeof(path), "/tmp/prism_alloc_fault_%ld_%u.c", (long)getpid(), ++serial);
		FILE *fp = fopen(path, "wb");
		if (!fp) {
			snprintf(why, why_cap, "could not create file-API input");
			return 0;
		}
		size_t src_len = strlen(src);
		size_t written = fwrite(src, 1, src_len, fp);
		int close_status = fclose(fp);
		if (written != src_len || close_status != 0) {
			remove(path);
			snprintf(why, why_cap, "could not write file-API input");
			return 0;
		}
		input = path;
	}
#else
	if (file_api) {
		snprintf(why, why_cap, "file allocation sweep is POSIX-only");
		return 0;
	}
#endif
	fault_realloc_must_move = 1;
	fault_alloc_disable();
	prism_thread_cleanup();
	pparse_ctx_init();
	base = alloc_fault_transpile(input, file_api, f);
	if (!prism_result_shape_ok(&base) || (expect_ok && base.status != PRISM_OK) ||
	    (expect_reject && base.status == PRISM_OK)) {
		snprintf(why, why_cap, "baseline failed: status=%d diag=%s", base.status,
			 base.error_msg ? base.error_msg : "-");
		goto done;
	}

	prism_thread_cleanup();
	if (!fault_alloc_tracker_snapshot(why, why_cap)) goto done;
	tracker_baseline_set = 1;
	pparse_ctx_init();
	fault_alloc_enable(LONG_MAX);
	measured = alloc_fault_transpile(input, file_api, f);
	allocation_count = fault_alloc_calls;
	fault_alloc_disable();
	if (!prism_result_shape_ok(&measured) || !prism_result_equal(&base, &measured)) {
		snprintf(why, why_cap, "allocation-count pass changed output: status=%d", measured.status);
		goto done;
	}
	prism_free(&measured);
	prism_thread_cleanup();
	if (!fault_alloc_tracker_matches(why, why_cap, "allocation-count cleanup")) goto done;
	if (allocation_count <= 0) {
		snprintf(why, why_cap, "source reached no injected allocation sites");
		goto done;
	}

	for (i = 1; i <= allocation_count; i++) {
		prism_thread_cleanup();
		pparse_ctx_init();
		fault_alloc_enable(i);
		failed = alloc_fault_transpile(input, file_api, f);
		fired = fault_alloc_fired;
		site_file = fault_alloc_file;
		site_line = fault_alloc_line;
		fault_alloc_disable();
		if (!fired) {
			snprintf(why, why_cap, "allocation %ld/%ld was not reached", i, allocation_count);
			goto done;
		}
		if (!prism_result_shape_ok(&failed)) {
			snprintf(why, why_cap, "allocation %ld/%ld at %s:%d returned invalid result: status=%d output=%p "
				 "len=%zu diag=%s",
				 i, allocation_count, site_file, site_line, failed.status, (void *)failed.output, failed.output_len,
				 failed.error_msg ? failed.error_msg : "-");
			goto done;
		}
		if (failed.status == PRISM_OK &&
		    (base.status != PRISM_OK || !prism_result_equal(&base, &failed))) {
			snprintf(why, why_cap, "allocation %ld/%ld at %s:%d succeeded with changed output", i,
				 allocation_count, site_file, site_line);
			goto done;
		}
		prism_free(&failed);

		recovery = alloc_fault_transpile(input, file_api, f);
		if (!prism_result_shape_ok(&recovery) || !prism_result_equal(&base, &recovery)) {
			snprintf(why, why_cap, "allocation %ld/%ld at %s:%d poisoned next call: status=%d diag=%s", i,
				 allocation_count, site_file, site_line, recovery.status,
				 recovery.error_msg ? recovery.error_msg : "-");
			goto done;
		}
		prism_free(&recovery);
		prism_thread_cleanup();
		if (!fault_alloc_tracker_matches(why, why_cap, "fault recovery cleanup")) {
			char phase[160];
			snprintf(phase, sizeof(phase), "allocation %ld/%ld at %s:%d cleanup", i,
				 allocation_count, site_file, site_line);
			fault_alloc_tracker_matches(why, why_cap, phase);
			goto done;
		}
	}
	ok = 1;

done:
	fault_alloc_disable();
	prism_free(&recovery);
	prism_free(&failed);
	prism_free(&measured);
	prism_free(&base);
	prism_thread_cleanup();
	if (tracker_baseline_set && !fault_alloc_tracker_matches(why, why_cap, "final cleanup")) {
		char tracker_why[256];
		fault_alloc_tracker_matches(tracker_why, sizeof(tracker_why), "final cleanup");
		if (ok) {
			ok = 0;
			snprintf(why, why_cap, "%s", tracker_why);
		}
	}
	if (path[0]) remove(path);
	fault_realloc_must_move = 0;
	return ok;
}

static int terms_present(const char *out, const char *terms, int present) {
	if (!terms || !*terms) return 1;
	for (const char *p = terms; *p;) {
		const char *e = strchr(p, '|');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		char term[256];
		if (n >= sizeof(term)) return 0;
		memcpy(term, p, n);
		term[n] = 0;
		int found = out && strstr(out, term) != NULL;
		if (found != present) return 0;
		p = e ? e + 1 : p + n;
	}
	return 1;
}

/* Both counters are read by the summary in main(), which is compiled on every
 * platform, so they live outside the POSIX guard even though only POSIX code
 * ever increments them. Declaring them beside their writers instead cost a
 * Windows build. */

/* A `system()` that fails because the machine would not start a process says
 * nothing about the code under test. A laptop briefly short of memory turned
 * five such refusals into five reported failures -- and reverting every
 * candidate fix afterwards reproduced nothing, because the tree was never the
 * variable. fork returning ENOMEM shows up as rc == -1; a shell that cannot
 * exec exits 127. Retry those, and count what needed retrying so a run that
 * fought the machine says so out loud instead of blaming the compiler. */
static long infra_retries;
/* Sub-second-identity shapes that the host clock was too slow to construct. */
static long slow_clock_skips;
/* Spawn-refusal shapes this host would not let the suite construct. */
static long spawn_refusal_skips;
/* Runs that could not find the suite source to lint its action letters. */
static long action_lint_skips;

/* A machine that will not start a process is not a verdict on prism, even when
 * the refusal is relayed through prism rather than around it. Fix 28 covered the
 * harness's own `system()` calls and the driver transpile; a library call whose
 * internal preprocessor spawn was refused still arrived here as PRISM_ERR_IO and
 * was reported as a failure. Prism now names that case in the message, so it can
 * be told apart from the preprocessor having run and disagreed, and retried. */
static int prism_result_is_spawn_refusal(const PrismResult *r) {
	return r->status == PRISM_ERR_IO && r->error_msg &&
	       strstr(r->error_msg, "could not start the C preprocessor") != NULL;
}


#ifndef _WIN32
static const char *backend_cc(void) {
	const char *cc = getenv("CC");
	if (!cc || !*cc || strpbrk(cc, " \t\"'`$;&|<>()[]{}*?!#~\n")) return "cc";
	return cc;
}

static int run_shell_command(const char *cmd) {
	int rc = -1;
	for (int attempt = 0; attempt < 4; attempt++) {
		rc = system(cmd);
		if (rc != -1 && !(WIFEXITED(rc) && WEXITSTATUS(rc) == 127)) return rc;
		infra_retries++;
		usleep(20000 * (unsigned)(attempt + 1));
	}
	return rc;
}

static int compile_output(const char *out, int run) {
	static unsigned serial;
	char src[256], bin[256], cmd[1024];
	unsigned id = ++serial;
	snprintf(src, sizeof(src), "/tmp/prism_recipe_%ld_%u.c", (long)getpid(), id);
	snprintf(bin, sizeof(bin), "/tmp/prism_recipe_%ld_%u.bin", (long)getpid(), id);
	FILE *fp = fopen(src, "w");
	if (!fp) return -1000;
	fwrite(out, 1, strlen(out), fp);
	fclose(fp);
	snprintf(cmd, sizeof(cmd), "%s -w -std=gnu11 %s %s -o %s >/dev/null 2>&1",
		 backend_cc(), run ? "" : "-fsyntax-only", src, bin);
	int rc = run_shell_command(cmd);
	if (rc != 0) {
		unlink(src);
		unlink(bin);
		return -1001;
	}
	if (!run) {
		unlink(src);
		unlink(bin);
		return 0;
	}
	/* Redirect the shell's own signal diagnostic as well as the program's
	 * streams; bounds-trap cells intentionally terminate by signal. */
	snprintf(cmd, sizeof(cmd), "{ %s; } >/dev/null 2>&1", bin);
	rc = system(cmd);
	unlink(src);
	unlink(bin);
	if (rc == -1) return -1002;
	if (WIFSIGNALED(rc)) return 128 + WTERMSIG(rc);
	if (!WIFEXITED(rc)) return -1002;
	return WEXITSTATUS(rc);
}

/* `transpile()` preprocesses with the backend's selected dialect.  A direct
 * system-header fallback can therefore contain dialect-specific declarations
 * from that backend (GCC 16's C23 `nullptr_t`, for example).  Keep the broad
 * corpus on its explicit GNU11 baseline, but validate driver output using the
 * same default dialect that produced the preprocessor stream. */
static int compile_output_backend_default(const char *out) {
	static unsigned serial;
	char src[256], cmd[1024];
	unsigned id = ++serial;
	snprintf(src, sizeof(src), "/tmp/prism_recipe_default_%ld_%u.c", (long)getpid(), id);
	FILE *fp = fopen(src, "w");
	if (!fp) return -1000;
	int wrote = fwrite(out, 1, strlen(out), fp) == strlen(out);
	if (fclose(fp) != 0) wrote = 0;
	if (!wrote) {
		unlink(src);
		return -1000;
	}
	snprintf(cmd, sizeof(cmd), "%s -w -fsyntax-only %s >/dev/null 2>&1", backend_cc(), src);
	int rc = run_shell_command(cmd);
	unlink(src);
	if (rc == -1 || !WIFEXITED(rc)) return -1001;
	return WEXITSTATUS(rc);
}

/* A #line filename is still tokenized in translation phase 1. Exercise that
 * path explicitly because normal compiles usually leave trigraphs disabled. */
static int compile_output_trigraphs(const char *out) {
	static unsigned serial;
	char src[256], cmd[1024];
	unsigned id = ++serial;
	snprintf(src, sizeof(src), "/tmp/prism_recipe_trigraph_%ld_%u.c", (long)getpid(), id);
	FILE *fp = fopen(src, "w");
	if (!fp) return -1000;
	int wrote = fwrite(out, 1, strlen(out), fp) == strlen(out);
	if (fclose(fp) != 0) wrote = 0;
	if (!wrote) {
		unlink(src);
		return -1000;
	}
	snprintf(cmd, sizeof(cmd), "%s -w -std=gnu11 -trigraphs -fsyntax-only %s >/dev/null 2>&1",
		 backend_cc(), src);
	int rc = run_shell_command(cmd);
	unlink(src);
	if (rc == -1 || !WIFEXITED(rc)) return -1001;
	return WEXITSTATUS(rc);
}

static int run_driver_transpile(const Recipe *r, const char *src, PrismFeatures f) {
	static unsigned serial;
	char in[256], out[256];
	unsigned id = ++serial;
	if (r->oracle & O_INPUT_PATH)
		snprintf(in, sizeof(in), "%s", src);
	else
		snprintf(in, sizeof(in), "/tmp/prism_driver_%ld_%u.c", (long)getpid(), id);
	snprintf(out, sizeof(out), "/tmp/prism_driver_%ld_%u.out.c", (long)getpid(), id);
	FILE *fp = NULL;
	if (!(r->oracle & O_INPUT_PATH)) {
		fp = fopen(in, "wb");
		if (!fp) return 0;
		fwrite(src, 1, strlen(src), fp);
		fclose(fp);
	}
	pparse_ctx_init();
	apply_features(f);
	prism_verify_mode = (r->oracle & O_VERIFY) != 0;
	int ok = transpile(in, out);
	/* transpile() returns 0 without a diagnostic when it could not spawn the
	 * preprocessor, which is indistinguishable here from prism rejecting the
	 * input. A real rejection repeats; a refused spawn usually does not. */
	for (int attempt = 0; !ok && attempt < 3; attempt++) {
		infra_retries++;
		usleep(20000 * (unsigned)(attempt + 1));
		ok = transpile(in, out);
	}
	prism_verify_mode = false;
	char *emitted = NULL;
	if (ok) {
		fp = fopen(out, "rb");
		if (fp) {
			fseek(fp, 0, SEEK_END);
			long z = ftell(fp);
			fseek(fp, 0, SEEK_SET);
			if (z >= 0) {
				emitted = malloc((size_t)z + 1);
				if (emitted) {
					fread(emitted, 1, (size_t)z, fp);
					emitted[z] = 0;
				}
			}
			fclose(fp);
		}
		ok = emitted && terms_present(emitted, r->must_have, 1) &&
		     terms_present(emitted, r->must_not_have, 0);
		if (ok && (r->oracle & O_COMPILE))
			ok = compile_output_backend_default(emitted) == 0;
	}
	free(emitted);
	if (!(r->oracle & O_INPUT_PATH)) unlink(in);
	unlink(out);
	prism_reset();
	return ok;
}

/* Keep the oversized-write length opaque to GCC's optimizer. The public
 * out_str dispatch is what this test covers, but folding its deliberately
 * over-buffer constant through two nested helpers produces a false-positive
 * -Wstringop-overflow warning even though the slow branch writes directly to
 * the FILE. GCC 16 can constprop-clone a noinline function, hence the volatile
 * read as well as the cold boundary. */
static PRISM_COLD void exercise_out_str_dispatch(const char *s, int len) {
	volatile int opaque_len = len;
	out_str(s, opaque_len);
}

/* mkdir that tolerates an existing directory; the recipe owns its own tree. */
static int pp_mkdir_p(const char *path) {
	if (mkdir(path, 0700) == 0) return 1;
	return errno == EEXIST;
}

/* Cross a wall-clock second boundary. The cache refuses to record a file whose
 * mtime has no sub-second part and is younger than two seconds, and comparing
 * identities needs the rewrite to be distinguishable from the original. */
static void pp_wait_for_next_second(void) {
	time_t edge = time(NULL);
	while (time(NULL) == edge) usleep(1000);
}

static int write_text_file(const char *path, const char *text) {
	FILE *fp = fopen(path, "wb");
	if (!fp) return 0;
	int ok = fputs(text, fp) >= 0;
	if (fclose(fp) != 0) ok = 0;
	return ok;
}

/* The internal actions drive prism through environment variables, and several
 * set one without putting it back: a full run leaves PRISM_PP_CACHE_DIR
 * pointing at a directory these actions have already deleted, and
 * PRISM_PP_CACHE_MAX_MB pinned at 1. Every later recipe then runs against a
 * broken 1 MB cache, which is invisible in the default order but makes recipe
 * results depend on what ran before them -- so a filtered or sharded run
 * disagrees with a full one. Snapshot the whole set here and restore it on the
 * way out, whatever the individual actions do in between. */
static const char *const INTERNAL_ENV_VARS[] = {
	"PRISM_PP_CACHE_DIR", "PRISM_PP_CACHE_MAX_MB", "PRISM_PP_CACHE_MAX_DAYS",
	"PRISM_NO_PP_CACHE", "CPATH", "DEPENDENCIES_OUTPUT", "SUNPRO_DEPENDENCIES",
	"SOURCE_DATE_EPOCH",
};

static int run_internal(const Recipe *r, char *failed_action) {
	int ok = 1;
	char *saved_env[N(INTERNAL_ENV_VARS)];
	for (size_t v = 0; v < N(INTERNAL_ENV_VARS); v++) {
		const char *cur = getenv(INTERNAL_ENV_VARS[v]);
		saved_env[v] = cur ? strdup(cur) : NULL;
	}
	for (const char *p = r->sequence; p && *p; p++) {
		int before = ok;
		if (*p == 'K') {
			char cache[PATH_MAX], entry[PATH_MAX], marker[PATH_MAX];
			const char *tmp = get_tmp_dir();
			snprintf(cache, sizeof cache, "/tmp/prism_recipe_cache_%ld", (long)getpid());
			ok = ok && tmp && *tmp && setenv("PRISM_PP_CACHE_DIR", cache, 1) == 0 &&
			     setenv("PRISM_PP_CACHE_MAX_MB", "1", 1) == 0;
			const char *dir = pp_cache_dir();
			ok = ok && dir && !strcmp(dir, cache);
			for (int i = 0; ok && i < 2; i++) {
				snprintf(entry, sizeof entry, "%s/seed%d.pp", dir, i);
				FILE *fp = fopen(entry, "wb");
				if (!fp) { ok = 0; break; }
				char block[4096];
				memset(block, 'A' + i, sizeof block);
				for (int n = 0; n < 175; n++)
					if (fwrite(block, 1, sizeof block, fp) != sizeof block) ok = 0;
				if (fclose(fp) != 0) ok = 0;
			}
			/* Exercise the user-facing cache reports without adding noise to the
			 * recipe protocol's single summary line. */
			int saved = dup(STDOUT_FILENO);
			FILE *sink = tmpfile();
			if (saved < 0 || !sink || fflush(stdout) != 0 || dup2(fileno(sink), STDOUT_FILENO) < 0)
				ok = 0;
			else {
				ok = ok && pp_cache_info() == 0;
				pp_cache_prune();
				ok = ok && pp_cache_clear() == 0;
				fflush(stdout);
				if (dup2(saved, STDOUT_FILENO) < 0) ok = 0;
			}
			if (saved >= 0) close(saved);
			if (sink) fclose(sink);
			snprintf(marker, sizeof marker, "%s/%s", dir, PP_PRUNE_MARKER);
			unlink(marker);
		} else if (*p == 'C') {
			/* A warm cache result must equal the cold preprocessor output, and a
			 * dependency identity change must invalidate it immediately. */
			char src[256], hdr[256];
			snprintf(src, sizeof src, "/tmp/prism_recipe_cache_src_%ld.c", (long)getpid());
			snprintf(hdr, sizeof hdr, "/tmp/prism_recipe_cache_hdr_%ld.h", (long)getpid());
			FILE *fp = fopen(hdr, "wb");
			if (!fp) { ok = 0; continue; }
			fputs("#define RECIPE_CACHE_VALUE 17\n", fp);
			fclose(fp);
			fp = fopen(src, "wb");
			if (!fp) { unlink(hdr); ok = 0; continue; }
			fprintf(fp, "#include \"%s\"\nint recipe_cache_value=RECIPE_CACHE_VALUE;\n", hdr);
			fclose(fp);
			pparse_ctx_init();
			apply_features(prism_defaults());
			char *cold = preprocess_with_cc(src);
			prism_reset();
			pparse_ctx_init();
			apply_features(prism_defaults());
			char *warm = preprocess_with_cc(src);
			prism_reset();
			ok = ok && cold && warm && !strcmp(cold, warm) && strstr(warm, "17");
			free(cold);
			free(warm);

			fp = fopen(hdr, "wb");
			if (!fp) ok = 0;
			else {
				fputs("#define RECIPE_CACHE_VALUE 2301\n", fp);
				fclose(fp);
				pparse_ctx_init();
				apply_features(prism_defaults());
				char *changed = preprocess_with_cc(src);
				ok = ok && changed && strstr(changed, "2301") && !strstr(changed, "=17;");
				free(changed);
				prism_reset();
			}
			unlink(src);
			unlink(hdr);
		} else if (*p == 's') {
			/* Action letters are dispatched by an if/else chain, so a letter
			 * used twice silently disables the later branch: the first match
			 * wins and the second test stops running without anything saying
			 * so. That happened here -- four actions added in 1.1.7 shadowed
			 * four existing ones, and the only symptom was tests quietly not
			 * executing. A switch would make it a compile error, but its cases
			 * would capture the `break`s the surrounding loop owns, so the
			 * check is done on the source instead.
			 *
			 * The suite is always invoked with its own source path, so that
			 * file is normally at hand; where it is not, this records a skip
			 * rather than passing. */
			static const char *const candidates[] = {".github/test.c", "test.c",
								 "../.github/test.c"};
			FILE *tf = NULL;
			for (size_t i = 0; i < N(candidates) && !tf; i++)
				tf = fopen(candidates[i], "r");
			if (!tf) {
				action_lint_skips++;
			} else {
				char line[1024];
				int seen[256] = {0};
				int dupes = 0, in_chain = 0;
				char first_dupe = 0;
				/* Scoped to run_internal: other dispatchers spell their own
				 * letters the same way and are allowed to reuse them. */
				while (fgets(line, sizeof line, tf)) {
					const char *m;
					if (strstr(line, "run_internal(const Recipe")) {
						in_chain = 1;
						continue;
					}
					if (!in_chain) continue;
					if (strncmp(line, "static ", 7) == 0) break;
					m = strstr(line, "(*p == '");
					if (!m) continue;
					m += 8;
					if (!m[0] || m[1] != '\'') continue;
					if (seen[(unsigned char)m[0]]++) {
						if (!dupes) first_dupe = m[0];
						dupes++;
					}
				}
				fclose(tf);
				if (dupes) {
					fprintf(stderr,
						"action letter '%c' handled more than once "
						"(%d duplicate branch(es)); the later one is dead\n",
						first_dupe, dupes);
					ok = 0;
				}
			}
		} else if (*p == 'p') {
			/* A define that follows a header and is named by a `#pragma` has
			 * to be emitted *after* that header, not hoisted to the top with
			 * the rest. Hoisting it would let it shadow or pre-empt what the
			 * header sets up, and no compile-or-run oracle can see the
			 * difference here: GCC and Clang never expand pragma operands, so
			 * sizeof is identical either way. Position is the whole property,
			 * so position is what this asserts -- the define must land after
			 * the last line marker naming the system header. */
			/* The header is written here rather than borrowed from libc:
			 * a system header's own content varies enough between hosts to
			 * decide the assertion by itself. <limits.h> contributes no line
			 * marker at all under GCC 11, and <stddef.h> under GCC 16 defines
			 * nullptr_t, which stops compiling once the flattened output is
			 * handed back with -std=gnu11. Neither says anything about prism. */
			char src[PATH_MAX], hdrp[PATH_MAX];
			ok = ok && snprintf(src, sizeof src, "/tmp/prism_recipe_pragmapos_%ld.c",
					    (long)getpid()) > 0;
			ok = ok && snprintf(hdrp, sizeof hdrp,
					    "/tmp/prism_recipe_pragmapos_%ld.h",
					    (long)getpid()) > 0;
			char body[PATH_MAX + 256];
			ok = ok && snprintf(body, sizeof body,
					    "#include \"%s\"\n"
					    "#define PRISM_PK_POS 1\n"
					    "#pragma pack(push, PRISM_PK_POS)\n"
					    "struct prism_pk_pos { char c; int x; };\n"
					    "#pragma pack(pop)\n"
					    "int prism_pk_pos_f(void){ "
					    "return (int)sizeof(struct prism_pk_pos) + "
					    "(int)sizeof(prism_pk_pos_marker); }\n",
					    hdrp) > 0;
			if (ok && write_text_file(hdrp, "typedef int prism_pk_pos_marker;\n") &&
			    write_text_file(src, body)) {
				PrismResult r = prism_transpile_file(src, prism_defaults());
				ok = ok && r.status == PRISM_OK && r.output;
				if (ok) {
					const char *def = strstr(r.output, "#define PRISM_PK_POS 1");
					const char *hdr = NULL, *scan = r.output;
					const char *prag = strstr(r.output,
								  "#pragma pack(push, PRISM_PK_POS)");
					const char *base = strrchr(hdrp, '/');
					base = base ? base + 1 : hdrp;
					while ((scan = strstr(scan, base)) != NULL) {
						hdr = scan;
						scan += strlen(base);
					}
					/* Present, after the header, before the pragma that
					 * needs it, and on a line of its own. */
					ok = ok && def && hdr && prag && def > hdr && def < prag &&
					     def > r.output && def[-1] == '\n';
					ok = ok && compile_output(r.output, 0) == 0;
				}
				prism_free(&r);
			}
			unlink(hdrp);
			unlink(src);
		} else if (*p == 'r') {
			/* The other half of action Q. Q covers a refusal the harness sees
			 * directly; this covers one relayed through prism, which is what a
			 * library call does when its own preprocessor spawn is refused.
			 * Until prism named that case, it arrived as a plain PRISM_ERR_IO
			 * and every cell using the file API turned a memory-starved
			 * machine into a prism failure -- observed on a Mac with 14.9 GB
			 * paged out, which exhausted the retry budget outright.
			 *
			 * The refusal is provoked for real rather than simulated: a child
			 * lowers RLIMIT_NPROC below its current process count, so the fork
			 * behind posix_spawnp fails with EAGAIN. It runs in a child
			 * because the limit is per-process and the suite is threaded. A
			 * host that will not let the limit bite -- some containers ignore
			 * it -- is recorded as a skip, not a pass. */
			char src[PATH_MAX];
			ok = ok && snprintf(src, sizeof src, "/tmp/prism_recipe_refusal_%ld.c",
					    (long)getpid()) > 0;
			if (ok && write_text_file(src, "int prism_refusal_f(void){ return 0; }\n")) {
				int pipefd[2];
				ok = ok && pipe(pipefd) == 0;
				if (ok) {
					pid_t kid = fork();
					if (kid == 0) {
						struct rlimit rl;
						char verdict = 'S'; /* skipped */
						close(pipefd[0]);
						if (getrlimit(RLIMIT_NPROC, &rl) == 0) {
							rl.rlim_cur = 1;
							if (setrlimit(RLIMIT_NPROC, &rl) == 0) {
								PrismResult r = prism_transpile_file(
								    src, prism_defaults());
								/* Which errno the limit produces is the
								 * host's choice: Linux fork reports EAGAIN,
								 * but a sandbox that denies the call outright
								 * reports EPERM, which is not transient and
								 * must not be retried. Assert only what this
								 * cell is about -- that a refusal prism did
								 * record is named in the message it returns. */
								if (r.status == PRISM_OK || !prism_spawn_refused)
									verdict = 'S';
								else
									verdict = prism_result_is_spawn_refusal(&r)
										      ? 'Y'
										      : 'N';
								prism_free(&r);
							}
						}
						ssize_t unused = write(pipefd[1], &verdict, 1);
						(void)unused;
						close(pipefd[1]);
						_exit(0);
					}
					close(pipefd[1]);
					if (kid < 0) {
						ok = 0;
					} else {
						char verdict = 0;
						ssize_t n = read(pipefd[0], &verdict, 1);
						int status = 0;
						waitpid(kid, &status, 0);
						if (n != 1 || verdict == 'S')
							spawn_refusal_skips++;
						else
							ok = ok && verdict == 'Y';
					}
					close(pipefd[0]);
				}
			}
			unlink(src);
		} else if (*p == 'q') {
			/* The harness has to tell a machine that refused to start a
			 * process from a compiler that ran and said no. Conflating the two
			 * is what turned five transient spawn refusals on a loaded laptop
			 * into five reported prism failures, and it sent the subsequent
			 * bisect chasing code changes that were never the variable.
			 * Refusal is rc == -1 (fork) or exit 127 (shell could not exec);
			 * anything else is a verdict and must be believed first time. */
			char script[PATH_MAX], cmd[PATH_MAX + 8];
			long before = infra_retries;
			int rc;
			ok = ok && snprintf(script, sizeof script,
					    "/tmp/prism_recipe_spawnq_%ld.sh", (long)getpid()) > 0;
			if (ok) {
				char counter[PATH_MAX + 4];
				snprintf(counter, sizeof counter, "%s.n", script);
				unlink(counter);
			}
			/* Refused twice, then fine: must be absorbed, and counted. */
			ok = ok && write_text_file(script,
						   "n=$(cat \"$0.n\" 2>/dev/null || echo 0)\n"
						   "n=$((n+1)); echo $n > \"$0.n\"\n"
						   "[ \"$n\" -le 2 ] && exit 127\n"
						   "exit 0\n");
			ok = ok && snprintf(cmd, sizeof cmd, "sh %s", script) > 0;
			if (ok) {
				rc = run_shell_command(cmd);
				ok = ok && rc == 0 && infra_retries == before + 2;
			}
			/* A real non-zero verdict is returned as-is, never retried. */
			before = infra_retries;
			ok = ok && write_text_file(script, "exit 3\n");
			if (ok) {
				rc = run_shell_command(cmd);
				ok = ok && WIFEXITED(rc) && WEXITSTATUS(rc) == 3 &&
				     infra_retries == before;
			}
			{
				char counter[PATH_MAX + 4];
				snprintf(counter, sizeof counter, "%s.n", script);
				unlink(counter);
			}
			unlink(script);
		} else if (*p == 'h') {
			/* Header-bearing cache entries. The three ways a recorded header
			 * can stop being the file the next preprocess would pick: it is
			 * edited, something shadows it earlier in the search path, or a
			 * symlink on the way to it is retargeted. The first is caught by
			 * the header's own identity, the second by the search directories'
			 * identities, the third by re-resolving the spelling. A stale hit
			 * here compiles the wrong code silently, so all three are asserted
			 * against real files rather than a constructed entry. */
			char root[PATH_MAX], inc_a[PATH_MAX], inc_b[PATH_MAX];
			char src[PATH_MAX], hdr_a[PATH_MAX], hdr_b[PATH_MAX];
			char real_a[PATH_MAX], real_b[PATH_MAX], link[PATH_MAX], lsrc[PATH_MAX];
			int paths_ok =
			    snprintf(root, sizeof root, "/tmp/prism_recipe_hdrcache_%ld", (long)getpid()) > 0 &&
			    snprintf(inc_a, sizeof inc_a, "%s/a", root) > 0 &&
			    snprintf(inc_b, sizeof inc_b, "%s/b", root) > 0 &&
			    snprintf(src, sizeof src, "%s/m.c", root) > 0 &&
			    snprintf(hdr_a, sizeof hdr_a, "%s/h.h", inc_a) > 0 &&
			    snprintf(hdr_b, sizeof hdr_b, "%s/h.h", inc_b) > 0 &&
			    snprintf(real_a, sizeof real_a, "%s/ra.h", root) > 0 &&
			    snprintf(real_b, sizeof real_b, "%s/rb.h", root) > 0 &&
			    snprintf(link, sizeof link, "%s/l.h", root) > 0 &&
			    snprintf(lsrc, sizeof lsrc, "%s/ls.c", root) > 0;
			ok = ok && paths_ok && pp_mkdir_p(root) && pp_mkdir_p(inc_a) && pp_mkdir_p(inc_b);
			if (ok) {
				const char *inc[] = {inc_a, inc_b};
				PrismFeatures f = prism_defaults();
				f.flatten_headers = false;
				f.include_paths = inc;
				f.include_count = 2;
				ok = ok && write_text_file(hdr_b, "#define HDRCACHE 41\n") &&
				     write_text_file(src, "#include <h.h>\nint hv = HDRCACHE;\n");
				/* prime, then a reuse that must produce the same text */
				PrismResult r1 = prism_transpile_file(src, f);
				PrismResult r2 = prism_transpile_file(src, f);
				ok = ok && r1.status == PRISM_OK && r2.status == PRISM_OK && r1.output && r2.output &&
				     strstr(r1.output, "41") && !strcmp(r1.output, r2.output);
				prism_free(&r1);
				prism_free(&r2);
				/* edited header */
				pp_wait_for_next_second();
				ok = ok && write_text_file(hdr_b, "#define HDRCACHE 42\n");
				PrismResult r3 = prism_transpile_file(src, f);
				ok = ok && r3.status == PRISM_OK && r3.output && strstr(r3.output, "42");
				prism_free(&r3);
				/* a shadowing header appears earlier in the search path */
				pp_wait_for_next_second();
				ok = ok && write_text_file(hdr_a, "#define HDRCACHE 43\n");
				PrismResult r4 = prism_transpile_file(src, f);
				ok = ok && r4.status == PRISM_OK && r4.output && strstr(r4.output, "43");
				prism_free(&r4);
				/* a symlink on the path to the header is retargeted */
				PrismFeatures g = prism_defaults();
				g.flatten_headers = false;
				ok = ok && write_text_file(real_a, "#define LINKED 51\n") &&
				     write_text_file(real_b, "#define LINKED 52\n");
				if (ok) {
					char inc_line[PATH_MAX + 32];
					snprintf(inc_line, sizeof inc_line, "#include \"%s\"\nint lv = LINKED;\n", link);
					unlink(link);
					ok = ok && symlink(real_a, link) == 0 && write_text_file(lsrc, inc_line);
					PrismResult s1 = prism_transpile_file(lsrc, g);
					PrismResult s2 = prism_transpile_file(lsrc, g);
					ok = ok && s1.status == PRISM_OK && s2.status == PRISM_OK && s1.output &&
					     s2.output && strstr(s1.output, "51") && !strcmp(s1.output, s2.output);
					prism_free(&s1);
					prism_free(&s2);
					pp_wait_for_next_second();
					unlink(link);
					ok = ok && symlink(real_b, link) == 0;
					PrismResult s3 = prism_transpile_file(lsrc, g);
					ok = ok && s3.status == PRISM_OK && s3.output && strstr(s3.output, "52");
					prism_free(&s3);
				}
			}
			unlink(hdr_a);
			unlink(hdr_b);
			unlink(src);
			unlink(real_a);
			unlink(real_b);
			unlink(link);
			unlink(lsrc);
			rmdir(inc_a);
			rmdir(inc_b);
			rmdir(root);
		} else if (*p == 'X') {
			/* pp_cache_dir() resolves PRISM_PP_CACHE_DIR once per thread and
			 * caches it, so this action cannot redirect itself at a private
			 * directory: it gets whatever the first cache user in this process
			 * resolved. In the default order that is internal/platform's
			 * per-pid directory and the rmdir is a real assertion. Run on its
			 * own -- filtered, or in a shard that does not contain
			 * internal/platform -- it resolves to the developer's actual cache
			 * instead, where removing the directory is both rude and a race
			 * against any other process using it. Sweep either way; only
			 * assert the removal on a directory the suite owns. */
			const char *dir = pp_cache_dir();
			long removed = 0;
			char marker[PATH_MAX];
			int suite_owned = dir && strstr(dir, "prism_recipe_") != NULL;
			ok = ok && dir != NULL;
			if (ok && suite_owned) {
				pp_each_entry(pp_clear_cb, &removed);
				snprintf(marker, sizeof marker, "%s/%s", dir, PP_PRUNE_MARKER);
				unlink(marker);
				ok = ok && rmdir(dir) == 0;
			}
			unsetenv("PRISM_PP_CACHE_DIR");
			unsetenv("PRISM_PP_CACHE_MAX_MB");
		} else if (*p == 'S') {
			char path[64], too_long[PATH_MAX + 1];
			signal_temps_clear();
			memset(too_long, 'x', sizeof too_long - 1);
			too_long[sizeof too_long - 1] = 0;
			signal_temps_register(too_long);
			for (int i = 0; i < SIGNAL_TEMPS_MAX; i++) {
				snprintf(path, sizeof path, "/tmp/prism_recipe_signal_%d", i);
				signal_temps_register(path);
			}
			signal_temps_unregister(NULL);
			signal_temps_unregister("/tmp/not-registered");
			signal_temps_unregister("/tmp/prism_recipe_signal_17");
			signal_temps_register("/tmp/prism_recipe_signal_reused");
			ok = ok && signal_temps_load() == SIGNAL_TEMPS_MAX;
			signal_temps_clear();
		} else if (*p == 'P') {
			char *yes[] = {"sh", "-c", "exit 0", NULL};
			char *no[] = {"sh", "-c", "exit 7", NULL};
			char *sig[] = {"sh", "-c", "kill -TERM $$", NULL};
			char *quiet[] = {"sh", "-c", "echo hidden >&2", NULL};
			ok = ok && run_command(yes) == 0 && run_command(no) == 7 &&
			     run_command(sig) == 143 && run_command_quiet(quiet) == 0;
		} else if (*p == 'O') {
			out_fp = tmpfile();
			if (!out_fp) { ok = 0; continue; }
			out_buf_pos = 0;
			out_total_flushed = 0;
			use_linemarkers = false;
			out_line(7, "a\\b\"c.c", false);
			use_linemarkers = true;
			out_line(9, "system.h", true);
			char *wide = malloc(OUT_BUF_SIZE + 17);
			if (!wide) { out_close(); ok = 0; continue; }
			memset(wide, 'w', OUT_BUF_SIZE + 16);
			wide[OUT_BUF_SIZE + 16] = 0;
			exercise_out_str_dispatch(wide, OUT_BUF_SIZE + 16);
			free(wide);
			out_close();
			ok = ok && out_total_flushed > 0;
		} else if (*p == 'W') {
			/* fclose can succeed after an unbuffered fwrite has already set the
			 * stream error indicator. Verify out_close preserves that earlier error. */
#if defined(__linux__)
			FILE *full = fopen("/dev/full", "w");
			if (!full || setvbuf(full, NULL, _IONBF, 0) != 0) {
				if (full) fclose(full);
				ok = 0;
			} else {
				out_fp = full;
				out_buf_pos = 0;
				out_str("x", 1);
				ok = ok && !out_close();
			}
#endif
		} else if (*p == 'D') {
			ConsumedDef *defs = NULL;
			bool undef_gnu = false;
			int n = 0, cap = 0;
			fault_realloc_must_move = 1;
			emit_consumed_def_upsert(&defs, &n, &cap, "", true, &undef_gnu);
			for (int i = 0; i < 20; i++) {
				char spec[32];
				snprintf(spec, sizeof spec, "RECIPE_%d=%d", i, i);
				emit_consumed_def_upsert(&defs, &n, &cap, spec, true, &undef_gnu);
			}
			emit_consumed_def_upsert(&defs, &n, &cap, "RECIPE_3", false, &undef_gnu);
			emit_consumed_def_upsert(&defs, &n, &cap, "_GNU_SOURCE", false, &undef_gnu);
			ok = ok && n == 21 && !defs[3].on && undef_gnu;
			free_consumed_defs(defs, n);
			fault_realloc_must_move = 0;
		} else if (*p == 'A') {
			/* The stdin preprocessor path must always carry an explicit language.
			 * A user-provided `-x none` means extension guessing, which is impossible
			 * for `-`, so it is normalized to C instead of being passed through. */
			const char *prefix_define[] = {"_GNU_SOURCE_EXTRA=1"};
			PrismFeatures prefix = prism_defaults();
			prefix.defines = prefix_define;
			prefix.define_count = N(prefix_define);
			apply_features(prefix);
			ok = ok && !api_mentions_macro("_GNU_SOURCE", 11);
			const char *xnone[] = {"-x", "none"};
			PrismFeatures f = prism_defaults();
			f.compiler_flags = xnone;
			f.compiler_flags_count = N(xnone);
			pparse_ctx_init();
			apply_features(f);
			const char **args = alloc_argv(32);
			int argc = 0;
			char *cc_dup = NULL;
			build_pp_argv(args, &argc, "-", &cc_dup);
			int saw_x_c = 0;
			for (int i = 0; i + 1 < argc; i++)
				if (!strcmp(args[i], "-x") && !strcmp(args[i + 1], "c")) saw_x_c++;
			ok = ok && argc > 0 && !strcmp(args[argc - 1], "-") && saw_x_c == 1;
			free(cc_dup);
			free((void *)args);
			prism_reset();

			f = prism_defaults();
			pparse_ctx_init();
			apply_features(f);
			args = alloc_argv(32);
			argc = 0;
			cc_dup = NULL;
			build_pp_argv(args, &argc, "-", &cc_dup);
			saw_x_c = 0;
			for (int i = 0; i + 1 < argc; i++)
				if (!strcmp(args[i], "-x") && !strcmp(args[i + 1], "c")) saw_x_c++;
			ok = ok && argc > 0 && !strcmp(args[argc - 1], "-") && saw_x_c == 1;
			free(cc_dup);
			free((void *)args);
			prism_reset();
		} else if (*p == 'F') {
			/* Exercise the raw-source #define collector independently of the host
			 * preprocessor so lexical noise cannot hide a scanner regression. */
			static const struct {
				const char *source;
				const char *want;
				int count;
			} scans[] = {
				{"#define A (1 /* same line */ + 2)\n", "A=(1   + 2)", 1},
				{"#define B (1 /* across\n*/ + 3)\n", "B=(1  + 3)", 1},
				{"#define C 4 /* across\n*/   \n", "C=4", 1},
				{"#define D 5 /* never closes", "D=5", 1},
				{"const char*s=R\"tag(\n#define BAD 1\n)tag\";\n#define E 6\n", "E=6", 1},
				{"const char*s=u8R\"(\n#define BAD 1\n)\";\n#define F 7\n", "F=7", 1},
				{"# /* split\n*/ define G 8\n", "G=8", 1},
				{"%:define H 9\n", "H=9", 1},
				{"?" "?=define I 10\n", "I=10", 1},
				{"ordinary tokens \\\ncontinued\n#define J 11\n", "J=11", 1},
				{"#if 1\n#else\n#define K 12\n#endif\n", "K=12", 1},
			};
			for (size_t i = 0; ok && i < N(scans); i++) {
				char path[256];
				snprintf(path, sizeof path, "/tmp/prism_recipe_scan_%ld_%zu.c", (long)getpid(), i);
				FILE *fp = fopen(path, "wb");
				if (!fp) { ok = 0; break; }
				fwrite(scans[i].source, 1, strlen(scans[i].source), fp);
				fclose(fp);
				pparse_ctx_init();
				PrismFeatures f = prism_defaults();
				f.flatten_headers = false;
				apply_features(f);
				collect_source_defines(path);
				PRISM_STATE();
				ok = _ps->source_define_count == scans[i].count &&
				     _ps->source_defines && !strcmp(_ps->source_defines[0].text, scans[i].want);
				if (!ok && getenv("PRISM_INTERNAL_TRACE"))
					fprintf(stderr, "scan %zu: count=%d value=%s want=%s\n", i,
						_ps->source_define_count,
						_ps->source_define_count ? _ps->source_defines[0].text : "-", scans[i].want);
				prism_reset();
				unlink(path);
			}
			if (ok) {
				char path[256];
				snprintf(path, sizeof path, "/tmp/prism_recipe_scan_deep_%ld.c", (long)getpid());
				FILE *fp = fopen(path, "wb");
				if (!fp) ok = 0;
				else {
					for (int i = 0; i < 40; i++) fputs("#if 1\n", fp);
					fputs("#define DEEP 41\n", fp);
					for (int i = 0; i < 40; i++) fputs("#endif\n", fp);
					fclose(fp);
					pparse_ctx_init();
					PrismFeatures f = prism_defaults();
					f.flatten_headers = false;
					apply_features(f);
					collect_source_defines(path);
					PRISM_STATE();
					ok = _ps->source_define_count == 1 &&
					     !strcmp(_ps->source_defines[0].text, "DEEP=41") &&
					     _ps->source_defines[0].guard &&
					     _ps->source_defines[0].guard_depth == 40 &&
					     count_kw(_ps->source_defines[0].guard, "if") == 40;
					prism_reset();
					unlink(path);
				}
			}
		} else if (*p == 'J') {
			/* `.i` inputs bypass the host preprocessor. Verify that extensions are
			 * still lowered, and reject binary/UTF-16-looking input explicitly. */
			char in[256], out[256];
			snprintf(in, sizeof in, "/tmp/prism_recipe_%ld.i", (long)getpid());
			snprintf(out, sizeof out, "/tmp/prism_recipe_%ld.i.out.c", (long)getpid());
			FILE *fp = fopen(in, "wb");
			const char good[] = "int g(void){return 0;}int f(void){int x=g() orelse 7;return x;}\n";
			if (!fp) { ok = 0; continue; }
			fwrite(good, 1, sizeof good - 1, fp);
			fclose(fp);
			pparse_ctx_init();
			apply_features(prism_defaults());
			ok = ok && transpile(in, out);
			char *emitted = read_file_padded(out);
			ok = ok && emitted && !strstr(emitted, "orelse") && compile_output(emitted, 0) == 0;
			free(emitted);
			prism_reset();

			int saved_err = dup(STDERR_FILENO);
			FILE *err_sink = tmpfile();
			if (saved_err < 0 || !err_sink || fflush(stderr) != 0 ||
			    dup2(fileno(err_sink), STDERR_FILENO) < 0)
				ok = 0;
			const unsigned char nul_bytes[] = {'i','n','t',' ','x',';',0,'x'};
			fp = fopen(in, "wb");
			if (!fp) ok = 0;
			else {
				fwrite(nul_bytes, 1, sizeof nul_bytes, fp);
				fclose(fp);
				pparse_ctx_init();
				apply_features(prism_defaults());
				char *bad = preprocess_with_cc(in);
				ok = ok && !bad;
				free(bad);
				prism_reset();
			}
			const unsigned char bom[] = {0xff, 0xfe};
			fp = fopen(in, "wb");
			if (!fp) ok = 0;
			else {
				fwrite(bom, 1, sizeof bom, fp);
				fclose(fp);
				pparse_ctx_init();
				apply_features(prism_defaults());
				char *bad = preprocess_with_cc(in);
				ok = ok && !bad;
				free(bad);
				prism_reset();
			}
			fflush(stderr);
			if (saved_err >= 0 && dup2(saved_err, STDERR_FILENO) < 0) ok = 0;
			if (saved_err >= 0) close(saved_err);
			if (err_sink) fclose(err_sink);
			unlink(in);
			unlink(out);
		} else if (*p == 'T') {
			char in[256];
			char stdout_path[] = "/dev/stdout";
			snprintf(in, sizeof in, "/tmp/prism_recipe_stdout_%ld.c", (long)getpid());
			FILE *fp = fopen(in, "wb");
			const char source[] = "int g(void);int f(void){int x=g() orelse 3;return x;}\n";
			if (!fp) { ok = 0; continue; }
			fwrite(source, 1, sizeof source - 1, fp);
			fclose(fp);
			int saved = dup(STDOUT_FILENO);
			FILE *sink = tmpfile();
			if (saved < 0 || !sink || fflush(stdout) != 0 || dup2(fileno(sink), STDOUT_FILENO) < 0) {
				ok = 0;
			} else {
				pparse_ctx_init();
				apply_features(prism_defaults());
				ok = ok && output_path_is_stdout("-") && output_path_is_stdout("/dev/fd/1") &&
				     transpile(in, stdout_path);
				prism_reset();
				fflush(stdout);
				/* transpile_to_stdout writes through a separately opened
				 * /dev/stdout descriptor. musl does not update this FILE's
				 * cached position from that descriptor, so synchronize it
				 * with the shared underlying file before asking for its size. */
				if (fseek(sink, 0, SEEK_END) != 0) ok = 0;
				long z = ftell(sink);
				if (z >= 0) rewind(sink);
				char *emitted = z >= 0 ? malloc((size_t)z + 1) : NULL;
				if (emitted) {
					fread(emitted, 1, (size_t)z, sink);
					emitted[z] = 0;
				}
				ok = ok && emitted && z > 0 && !strstr(emitted, "orelse") &&
				     compile_output(emitted, 0) == 0;
				free(emitted);
				if (dup2(saved, STDOUT_FILENO) < 0) ok = 0;
			}
			if (saved >= 0) close(saved);
			if (sink) fclose(sink);
			unlink(in);
		} else if (*p == 'R') {
			/* Shrinking an arena allocation is a legal no-op: callers may retain
			 * the original capacity, but the helper must never copy past the new
			 * size or underflow its zero-fill length. */
			PParseArena arena = {0};
			unsigned char *old = pparse_arena_alloc(&arena, 16);
			for (unsigned i = 0; i < 16; i++) old[i] = (unsigned char)(i + 1);
			unsigned char *shrunk = pparse_arena_realloc(&arena, old, 16, 7);
			unsigned char *fresh = pparse_arena_realloc(&arena, NULL, 16, 7);
			ok = ok && shrunk == old && !memcmp(shrunk, "\1\2\3\4\5\6\7", 7) && fresh != NULL;
			for (PParseArenaBlock *block = arena.head; block;) {
				PParseArenaBlock *next = block->next;
				free(block);
				block = next;
			}
		} else if (*p == 'Q') {
			/* Public reset is valid both before initialization and after an
			 * explicit thread cleanup; the following API call must reinitialize
			 * normally. */
			prism_thread_cleanup();
			prism_reset();
			PrismResult again = prism_transpile_source("int main(void){return 0;}",
								    "reset.c", prism_defaults());
			ok = ok && again.status == PRISM_OK && again.output && again.output_len > 0;
			prism_free(&again);
		} else if (*p == 'E') {
			/* A first library call must report an allocation failure instead of
			 * terminating the embedding process when its TLS parser context cannot
			 * be created. Even diagnostic allocation failure must return a freeable
			 * static fallback, and the next call must recover. */
			fault_alloc_enable(1);
			PrismResult fallback = prism_transpile_source(NULL, "oom-message.c", prism_defaults());
			int fallback_fired = fault_alloc_fired;
			fault_alloc_disable();
			ok = ok && fallback_fired && prism_result_shape_ok(&fallback) &&
			     fallback.status == PRISM_ERR_IO && fallback.error_msg == prism_oom_error;
			prism_free(&fallback);
			prism_thread_cleanup();
			fault_alloc_enable(1);
			PrismResult oom = prism_transpile_source("int x;", "first-oom.c", prism_defaults());
			int fired = fault_alloc_fired;
			fault_alloc_disable();
			ok = ok && fired && prism_result_shape_ok(&oom) && oom.status == PRISM_ERR_IO;
			prism_free(&oom);
			PrismResult recovery = prism_transpile_source("int x;", "first-oom.c", prism_defaults());
			ok = ok && recovery.status == PRISM_OK && recovery.output;
			prism_free(&recovery);
		} else if (*p == 'V') {
			/* File/API validation: reject ignored compiler inputs, bound public
			 * feature arrays, clean soft errors, and safely quote line markers. */
			PrismFeatures f = prism_defaults();
			prism_free(NULL);
			PrismResult dir = prism_transpile_file(get_tmp_dir(), f);
			ok = ok && prism_result_shape_ok(&dir) && dir.status == PRISM_ERR_IO;
			prism_free(&dir);

			char txt[256], lower_i[256], upper_i[256], bad[256];
			snprintf(txt, sizeof txt, "/tmp/prism_recipe_%ld.txt", (long)getpid());
			FILE *fp = fopen(txt, "wb");
			if (!fp) ok = 0;
			else {
				fputs("int txt_input;\n", fp);
				fclose(fp);
				PrismResult text = prism_transpile_file(txt, f);
				ok = ok && prism_result_shape_ok(&text) && text.status == PRISM_ERR_IO;
				prism_free(&text);
			}
			unlink(txt);
			snprintf(lower_i, sizeof lower_i, "/tmp/prism_recipe_%ld.i", (long)getpid());
			fp = fopen(lower_i, "wb");
			if (!fp) ok = 0;
			else {
				fputs("int lower_i_input;\n", fp);
				fclose(fp);
				PrismResult lower = prism_transpile_file(lower_i, f);
				ok = ok && prism_result_shape_ok(&lower) && lower.status == PRISM_OK && lower.output;
				prism_free(&lower);
			}
			unlink(lower_i);
			snprintf(upper_i, sizeof upper_i, "/tmp/prism_recipe_%ld.I", (long)getpid());
			fp = fopen(upper_i, "wb");
			if (!fp) ok = 0;
			else {
				fputs("int upper_i_input;\n", fp);
				fclose(fp);
				PrismResult upper = prism_transpile_file(upper_i, f);
				ok = ok && prism_result_shape_ok(&upper) && upper.status == PRISM_ERR_IO;
				prism_free(&upper);
			}
			unlink(upper_i);

			PrismResult marker = prism_transpile_source(
			    "int value;\n", "safe.c\"\n#error injected\r\t\001\177\"tail", f);
			ok = ok && prism_result_shape_ok(&marker) && marker.status == PRISM_OK &&
			     strstr(marker.output, "\\\"") && strstr(marker.output, "\\n") &&
			     strstr(marker.output, "\\r") && strstr(marker.output, "\\t") &&
			     strstr(marker.output, "\\001") && strstr(marker.output, "\\177") &&
			     compile_output(marker.output, 0) == 0;
			prism_free(&marker);
			/* Build the filename bytewise so this test remains safe if the test
			 * driver itself enables trigraphs. The emitted marker must not contain
			 * a raw trigraph before its closing quote. */
			char tri_name[] = {'s','a','f','e','?','?','/','"','t','a','i','l','.','c',0};
			char tri_escaped[] = {'\\','?','\\','?','/',0};
			PrismResult trigraph = prism_transpile_source("int trigraph_value;\n", tri_name, f);
			ok = ok && prism_result_shape_ok(&trigraph) && trigraph.status == PRISM_OK &&
			     strstr(trigraph.output, tri_escaped) && compile_output_trigraphs(trigraph.output) == 0;
			prism_free(&trigraph);

			const char *one_value[] = {"-w"};
			f = prism_defaults();
			f.flatten_headers = false;
			f.compiler_flags = one_value;
			f.compiler_flags_count = INT_MAX;
			PrismResult count = prism_transpile_source("int count_input;", "count.c", f);
			ok = ok && prism_result_shape_ok(&count) && count.status == PRISM_ERR_IO;
			prism_free(&count);
			f = prism_defaults();
			f.include_paths = one_value;
			f.include_count = INT_MAX;
			PrismResult include_count = prism_transpile_source("int count_input;", "count.c", f);
			ok = ok && prism_result_shape_ok(&include_count) && include_count.status == PRISM_ERR_IO;
			prism_free(&include_count);
			f = prism_defaults();
			f.defines = one_value;
			f.define_count = INT_MAX;
			PrismResult define_count = prism_transpile_source("int count_input;", "count.c", f);
			ok = ok && prism_result_shape_ok(&define_count) && define_count.status == PRISM_ERR_IO;
			prism_free(&define_count);
			f = prism_defaults();
			f.force_includes = one_value;
			f.force_include_count = INT_MAX;
			PrismResult force_count = prism_transpile_source("int count_input;", "count.c", f);
			ok = ok && prism_result_shape_ok(&force_count) && force_count.status == PRISM_ERR_IO;
			prism_free(&force_count);

			snprintf(bad, sizeof bad, "/tmp/prism_recipe_%ld.c", (long)getpid());
			fp = fopen(bad, "wb");
			if (!fp) ok = 0;
			else {
				fputs("#define KEPT 1\n#error intentional\n", fp);
				fclose(fp);
				f = prism_defaults();
				f.flatten_headers = false;
				int saved_err = dup(STDERR_FILENO);
				FILE *err_sink = tmpfile();
				if (saved_err < 0 || !err_sink || fflush(stderr) != 0 ||
				    dup2(fileno(err_sink), STDERR_FILENO) < 0) {
					ok = 0;
				} else {
					PrismResult failed = prism_transpile_file(bad, f);
					fflush(stderr);
					if (dup2(saved_err, STDERR_FILENO) < 0) ok = 0;
					PRISM_STATE();
					ok = ok && prism_result_shape_ok(&failed) && failed.status == PRISM_ERR_IO &&
					     _ps->source_defines == NULL && _ps->source_define_count == 0;
					prism_free(&failed);
				}
				if (saved_err >= 0) close(saved_err);
				if (err_sink) fclose(err_sink);
			}
			unlink(bad);
		} else if (*p == 'M') {
			/* Cache limits are caller-controlled; saturated values must never
			 * overflow their byte/second conversions. K established an isolated
			 * cache root before this action runs. */
			ok = ok && setenv("PRISM_PP_CACHE_MAX_MB", "9223372036854775807", 1) == 0 &&
			     setenv("PRISM_PP_CACHE_MAX_DAYS", "9223372036854775807", 1) == 0;
			pp_cache_prune();
			/* Specs files and dependency output are compiler side effects, not
			 * preprocessor stdout. Both must bypass an otherwise valid cache hit. */
			char *specs_joined[] = {"cc", "-specs=/tmp/prism-mutable.specs", NULL};
			char *specs_split[] = {"cc", "-specs", "/tmp/prism-mutable.specs", NULL};
			char *dep_argv[] = {"cc", "-MMD", "-MF", "/tmp/prism-side-effect.d", NULL};
			char *empty_argv[] = {"cc", "", NULL};
			PRISM_STATE();
			int saved_dep_count = _ps->dep_flags_count;
			_ps->dep_flags_count = 0;
			ok = ok && pp_argv_disables_cache(specs_joined, 2) &&
			     pp_argv_disables_cache(specs_split, 3) && pp_argv_disables_cache(dep_argv, 4) &&
			     !pp_argv_disables_cache(empty_argv, 2);
			const char *old_dep_env = getenv("DEPENDENCIES_OUTPUT");
			const char *old_sun_dep_env = getenv("SUNPRO_DEPENDENCIES");
			char *saved_dep_env = old_dep_env ? strdup(old_dep_env) : NULL;
			char *saved_sun_dep_env = old_sun_dep_env ? strdup(old_sun_dep_env) : NULL;
			ok = ok && (!old_dep_env || saved_dep_env) && (!old_sun_dep_env || saved_sun_dep_env) &&
			     setenv("DEPENDENCIES_OUTPUT", "/tmp/prism-cache-deps", 1) == 0 &&
			     pp_argv_disables_cache((char *[]){"cc", NULL}, 1) &&
			     unsetenv("DEPENDENCIES_OUTPUT") == 0 &&
			     setenv("SUNPRO_DEPENDENCIES", "/tmp/prism-cache-sun-deps", 1) == 0 &&
			     pp_argv_disables_cache((char *[]){"cc", NULL}, 1);
			if (saved_dep_env) setenv("DEPENDENCIES_OUTPUT", saved_dep_env, 1);
			else unsetenv("DEPENDENCIES_OUTPUT");
			if (saved_sun_dep_env) setenv("SUNPRO_DEPENDENCIES", saved_sun_dep_env, 1);
			else unsetenv("SUNPRO_DEPENDENCIES");
			free(saved_dep_env);
			free(saved_sun_dep_env);
			_ps->dep_flags_count = 1;
			ok = ok && pp_argv_disables_cache((char *[]){"cc", NULL}, 1);
			_ps->dep_flags_count = saved_dep_count;
			/* A corrupt cache entry must be a harmless miss when its declared payload
			 * is larger than the file that carries it. On 32-bit builds this value
			 * additionally exceeds size_t, covering both allocation guards. */
			PPKey corrupt = {0x123456789abcdef0ULL, 0x0fedcba987654321ULL};
			char corrupt_path[PATH_MAX] = {0};
			if (!pp_cache_path(&corrupt, corrupt_path, sizeof corrupt_path)) {
				ok = 0;
			} else {
				FILE *fp = fopen(corrupt_path, "wb");
				if (!fp) {
					ok = 0;
				} else {
					int wrote = fputs(PP_CACHE_MAGIC, fp) >= 0 &&
						    fputs("deps 0\ndirs 0\npayload 9223372036854775807\n", fp) >= 0;
					if (fclose(fp) != 0) wrote = 0;
					ok = ok && wrote;
				}
				char *payload = pp_cache_load(&corrupt);
				ok = ok && payload == NULL;
				free(payload);
				fp = fopen(corrupt_path, "wb");
				if (!fp) {
					ok = 0;
				} else {
					int wrote = fputs(PP_CACHE_MAGIC, fp) >= 0 && fputs("deps 1\ninvalid dependency\ninvalid dependency\ndirs 0\npayload 0\n", fp) >= 0;
					if (fclose(fp) != 0) wrote = 0;
					ok = ok && wrote;
				}
					payload = pp_cache_load(&corrupt);
					ok = ok && payload == NULL;
					free(payload);
					unlink(corrupt_path);
				}
				/* A same-length flip must not turn into silently altered generated C.
				 * Exercise the payload checksum and the exact-EOF validation together. */
				PPKey checked = {0x0badf00d12345678ULL, 0x87654321deadbeefULL};
				char checked_path[PATH_MAX] = {0};
				const char checked_payload[] = "int cache_checksum_value;\n";
				PPKey checked_sum = pp_payload_checksum(checked_payload, sizeof checked_payload - 1);
				if (!pp_cache_path(&checked, checked_path, sizeof checked_path)) {
					ok = 0;
				} else {
					FILE *fp = fopen(checked_path, "wb");
					int wrote = fp && fputs(PP_CACHE_MAGIC, fp) >= 0 && fputs("deps 0\ndirs 0\n", fp) >= 0 &&
						fprintf(fp, "payload %zu %016llx %016llx\n", sizeof checked_payload - 1,
							(unsigned long long)checked_sum.a, (unsigned long long)checked_sum.b) > 0 &&
						fwrite(checked_payload, 1, sizeof checked_payload - 1, fp) == sizeof checked_payload - 1;
					if (fp && fclose(fp) != 0) wrote = 0;
					char *payload = pp_cache_load(&checked);
					ok = ok && wrote && payload && !strcmp(payload, checked_payload);
					free(payload);
					fp = fopen(checked_path, "r+b");
					int flipped = fp && fseek(fp, -1, SEEK_END) == 0 && fputc('X', fp) != EOF;
					if (fp && fclose(fp) != 0) flipped = 0;
					payload = pp_cache_load(&checked);
					ok = ok && flipped && payload == NULL;
					free(payload);
					unlink(checked_path);
				}
				/* mkstemp-style cache publishing gives concurrent same-process stores
				 * independent temporary names instead of one pid-only collision path. */
				PPKey temp_key = {0x1111111111111111ULL, 0x2222222222222222ULL};
				char cache_path[PATH_MAX] = {0}, temp_one[PATH_MAX] = {0}, temp_two[PATH_MAX] = {0};
				int one_fd = -1, two_fd = -1;
				if (!pp_cache_path(&temp_key, cache_path, sizeof cache_path) ||
				    (one_fd = pp_cache_open_temp(cache_path, temp_one, sizeof temp_one)) < 0 ||
				    (two_fd = pp_cache_open_temp(cache_path, temp_two, sizeof temp_two)) < 0)
					ok = 0;
				else
					ok = ok && strcmp(temp_one, temp_two) != 0;
				if (one_fd >= 0) close(one_fd);
				if (two_fd >= 0) close(two_fd);
				if (temp_one[0]) unlink(temp_one);
				if (temp_two[0]) unlink(temp_two);
				setenv("PRISM_PP_CACHE_MAX_MB", "1", 1);
			unsetenv("PRISM_PP_CACHE_MAX_DAYS");
		} else if (*p == 'H') {
			/* build_clean_environ must snapshot the host environment per spawn.
			 * CPATH changes between calls are both a functional requirement and a
			 * guard against retaining C runtime-owned environment pointers. */
			char root[PATH_MAX], first[PATH_MAX], second[PATH_MAX];
			char one_h[PATH_MAX], two_h[PATH_MAX], one_c[PATH_MAX], two_c[PATH_MAX];
			const char *old_cpath = getenv("CPATH");
			char *saved_cpath = old_cpath ? strdup(old_cpath) : NULL;
			int paths_ok = (!old_cpath || saved_cpath) &&
				       snprintf(root, sizeof root, "/tmp/prism_recipe_env_%ld", (long)getpid()) > 0 &&
				       snprintf(first, sizeof first, "%s/a", root) > 0 &&
				       snprintf(second, sizeof second, "%s/replacement_include_path", root) > 0 &&
				       snprintf(one_h, sizeof one_h, "%s/prism_env_before.h", first) > 0 &&
				       snprintf(two_h, sizeof two_h, "%s/prism_env_after.h", second) > 0 &&
				       snprintf(one_c, sizeof one_c, "%s/one.c", root) > 0 &&
				       snprintf(two_c, sizeof two_c, "%s/two.c", root) > 0;
			if (!paths_ok || mkdir(root, 0700) != 0 || mkdir(first, 0700) != 0 ||
			    mkdir(second, 0700) != 0 || !write_text_file(one_h, "#define PRISM_ENV_BEFORE 17\n") ||
			    !write_text_file(two_h, "#define PRISM_ENV_AFTER 23\n") ||
			    !write_text_file(one_c, "#include <prism_env_before.h>\nint env_before=PRISM_ENV_BEFORE;\n") ||
			    !write_text_file(two_c, "#include <prism_env_after.h>\nint env_after=PRISM_ENV_AFTER;\n")) {
				ok = 0;
			} else {
				PrismFeatures f = prism_defaults();
				ok = ok && setenv("CPATH", first, 1) == 0;
				PrismResult before = prism_transpile_file(one_c, f);
				ok = ok && before.status == PRISM_OK && before.output;
				prism_free(&before);
				ok = ok && setenv("CPATH", second, 1) == 0;
				PrismResult after = prism_transpile_file(two_c, f);
				ok = ok && after.status == PRISM_OK && after.output;
				prism_free(&after);
			}
			if (saved_cpath) setenv("CPATH", saved_cpath, 1);
			else unsetenv("CPATH");
			free(saved_cpath);
			unlink(one_c);
			unlink(two_c);
			unlink(one_h);
			unlink(two_h);
			rmdir(first);
			rmdir(second);
			rmdir(root);
		} else if (*p == 'Y') {
			/* Cache keys and validation must account for cwd-sensitive flags, raw
			 * response files, volatile header macros, and opaque custom compilers. */
			char old_cwd[PATH_MAX] = {0}, root[PATH_MAX] = {0}, first[PATH_MAX] = {0}, second[PATH_MAX] = {0};
			char src[PATH_MAX] = {0}, first_h[PATH_MAX] = {0}, second_h[PATH_MAX] = {0};
			char rsp[PATH_MAX] = {0}, rsp_arg[PATH_MAX + 2] = {0}, rsp_src[PATH_MAX] = {0};
			char time_h[PATH_MAX] = {0}, time_src[PATH_MAX] = {0}, wrapper[PATH_MAX] = {0};
			char config[PATH_MAX] = {0}, wrap_src[PATH_MAX] = {0}, dep[PATH_MAX] = {0}, dep_src[PATH_MAX] = {0};
			const char *old_epoch = getenv("SOURCE_DATE_EPOCH");
			char *saved_epoch = old_epoch ? strdup(old_epoch) : NULL;
			int have_cwd = getcwd(old_cwd, sizeof old_cwd) != NULL;
			int paths_ok = have_cwd && (!old_epoch || saved_epoch) &&
				snprintf(root, sizeof root, "/tmp/prism_recipe_cache_more_%ld", (long)getpid()) > 0 &&
				snprintf(first, sizeof first, "%s/a", root) > 0 &&
				snprintf(second, sizeof second, "%s/b", root) > 0 &&
				snprintf(src, sizeof src, "%s/main.c", root) > 0 &&
				snprintf(first_h, sizeof first_h, "%s/choice.h", first) > 0 &&
				snprintf(second_h, sizeof second_h, "%s/choice.h", second) > 0 &&
				snprintf(rsp, sizeof rsp, "%s/flags.rsp", root) > 0 &&
				snprintf(rsp_arg, sizeof rsp_arg, "@%s", rsp) > 0 &&
				snprintf(rsp_src, sizeof rsp_src, "%s/rsp.c", root) > 0 &&
				snprintf(time_h, sizeof time_h, "%s/volatile.h", root) > 0 &&
				snprintf(time_src, sizeof time_src, "%s/time.c", root) > 0 &&
				snprintf(dep, sizeof dep, "%s/side-effect.d", root) > 0 &&
				snprintf(dep_src, sizeof dep_src, "%s/side-effect.c", root) > 0 &&
				snprintf(wrapper, sizeof wrapper, "%s/wrapcc", root) > 0 &&
				snprintf(config, sizeof config, "%s/config", root) > 0 &&
				snprintf(wrap_src, sizeof wrap_src, "%s/wrap.c", root) > 0;
			if (!paths_ok || mkdir(root, 0700) != 0 || mkdir(first, 0700) != 0 ||
			    mkdir(second, 0700) != 0 || !write_text_file(first_h, "#define PRISM_CACHE_CWD 111\n") ||
			    !write_text_file(second_h, "#define PRISM_CACHE_CWD 222\n") ||
			    !write_text_file(src, "#include <choice.h>\nint cwd_value=PRISM_CACHE_CWD;\n")) {
				ok = 0;
			} else {
				const char *dot[] = {"."};
				PrismFeatures f = prism_defaults();
				f.flatten_headers = false;
				f.include_paths = dot;
				f.include_count = N(dot);
				PrismResult from_first = {0}, from_second = {0};
				if (chdir(first) != 0) ok = 0;
				else from_first = prism_transpile_file(src, f);
				if (chdir(second) != 0) ok = 0;
				else from_second = prism_transpile_file(src, f);
				ok = ok && from_first.status == PRISM_OK && from_first.output &&
				     from_second.status == PRISM_OK && from_second.output &&
				     strstr(from_first.output, "111") && strstr(from_second.output, "222");
				prism_free(&from_first);
				prism_free(&from_second);

				const char *no_markers[] = {"-P"};
				f.compiler_flags = no_markers;
				f.compiler_flags_count = N(no_markers);
				PrismResult without_markers_before = prism_transpile_file(src, f);
				ok = ok && without_markers_before.status == PRISM_OK && without_markers_before.output &&
				     strstr(without_markers_before.output, "222");
				prism_free(&without_markers_before);
				ok = ok && write_text_file(second_h, "#define PRISM_CACHE_CWD 333\n");
				PrismResult without_markers_after = prism_transpile_file(src, f);
				ok = ok && without_markers_after.status == PRISM_OK && without_markers_after.output &&
				     strstr(without_markers_after.output, "333");
				prism_free(&without_markers_after);

				ok = ok && write_text_file(rsp, "-DPRISM_RSP_VALUE=111\n") &&
				     write_text_file(rsp_src, "int rsp_value=PRISM_RSP_VALUE;\n");
				const char *response[] = {rsp_arg};
				f = prism_defaults();
				f.flatten_headers = false;
				f.compiler_flags = response;
				f.compiler_flags_count = N(response);
				PrismResult rsp_before = prism_transpile_file(rsp_src, f);
				ok = ok && rsp_before.status == PRISM_OK && rsp_before.output && strstr(rsp_before.output, "111");
				prism_free(&rsp_before);
				ok = ok && write_text_file(rsp, "-DPRISM_RSP_VALUE=333\n");
				PrismResult rsp_after = prism_transpile_file(rsp_src, f);
				ok = ok && rsp_after.status == PRISM_OK && rsp_after.output && strstr(rsp_after.output, "333");
				prism_free(&rsp_after);

				/* A hit can replay preprocessor stdout, but not the .d side effect.
				 * Remove it between identical calls to prove dependency mode bypasses
				 * the cache for public API compiler flags as well as CLI flags. */
				const char *dep_flags[] = {"-MMD", "-MF", dep};
				f = prism_defaults();
				f.flatten_headers = false;
				f.compiler_flags = dep_flags;
				f.compiler_flags_count = N(dep_flags);
				ok = ok && write_text_file(dep_src, "int prism_dep_side_effect;\n");
				PrismResult dep_before = prism_transpile_file(dep_src, f);
				int first_dep = access(dep, F_OK) == 0;
				unlink(dep);
				PrismResult dep_after = prism_transpile_file(dep_src, f);
				int second_dep = access(dep, F_OK) == 0;
				ok = ok && dep_before.status == PRISM_OK && dep_before.output && first_dep &&
				     dep_after.status == PRISM_OK && dep_after.output && second_dep;
				prism_free(&dep_before);
				prism_free(&dep_after);

				unsetenv("SOURCE_DATE_EPOCH");
				ok = ok && write_text_file(time_h, "#define PRISM_HEADER_TIME __TIME__\n") &&
				     write_text_file(time_src,
						     "#include \"volatile.h\"\nconst char *header_time=PRISM_HEADER_TIME;\n");
				f = prism_defaults();
				f.flatten_headers = false;
				PrismResult time_before = prism_transpile_file(time_src, f);
				sleep(2);
				PrismResult time_after = prism_transpile_file(time_src, f);
				ok = ok && time_before.status == PRISM_OK && time_before.output &&
				     time_after.status == PRISM_OK && time_after.output &&
				     strcmp(time_before.output, time_after.output) != 0;
				prism_free(&time_before);
				prism_free(&time_after);

				FILE *script = fopen(wrapper, "wb");
				int script_ok = script &&
					fprintf(script, "#!/bin/sh\nexec cc -DPRISM_WRAP_VALUE=$(cat %s) \"$@\"\n", config) >= 0;
				if (script && fclose(script) != 0) script_ok = 0;
				if (!script_ok || chmod(wrapper, 0700) != 0 || !write_text_file(config, "111\n") ||
				    !write_text_file(wrap_src, "int wrap_value=PRISM_WRAP_VALUE;\n")) {
					ok = 0;
				} else {
					f = prism_defaults();
					f.flatten_headers = false;
					f.compiler = wrapper;
					PrismResult wrap_before = prism_transpile_file(wrap_src, f);
					ok = ok && wrap_before.status == PRISM_OK && wrap_before.output && strstr(wrap_before.output, "111");
					prism_free(&wrap_before);
					ok = ok && write_text_file(config, "333\n");
					PrismResult wrap_after = prism_transpile_file(wrap_src, f);
					ok = ok && wrap_after.status == PRISM_OK && wrap_after.output && strstr(wrap_after.output, "333");
					prism_free(&wrap_after);
				}
			}
			if (have_cwd && chdir(old_cwd) != 0) ok = 0;
			if (saved_epoch) setenv("SOURCE_DATE_EPOCH", saved_epoch, 1);
			else unsetenv("SOURCE_DATE_EPOCH");
			free(saved_epoch);
			unlink(src);
			unlink(first_h);
			unlink(second_h);
			unlink(rsp);
			unlink(rsp_src);
			unlink(time_h);
			unlink(time_src);
			unlink(dep);
			unlink(dep_src);
			unlink(wrapper);
			unlink(config);
			unlink(wrap_src);
			rmdir(first);
			rmdir(second);
			rmdir(root);
		} else if (*p == 'Z') {
			/* A same-size rewrite can restore its exact mtime. ctime seconds alone
			 * are not enough when both writes land in one second, so lock that old
			 * stale-hit shape in with an explicit sub-second regression. */
			char src[256] = {0}, hdr[256] = {0};
			int names_ok = snprintf(src, sizeof src, "/tmp/prism_recipe_cache_stat_%ld.c", (long)getpid()) > 0 &&
				       snprintf(hdr, sizeof hdr, "/tmp/prism_recipe_cache_stat_%ld.h", (long)getpid()) > 0;
			int within_one_second = 0;
			struct timeval fixed_time[2] = {{123456789, 123456}, {123456789, 123456}};
			for (int attempt = 0; names_ok && ok && attempt < 6 && !within_one_second; attempt++) {
				time_t edge = time(NULL);
				while (time(NULL) == edge) usleep(1000);
				if (!write_text_file(hdr, "#define PRISM_CACHE_STAT 111\n") || utimes(hdr, fixed_time) != 0) {
					ok = 0;
					break;
				}
				/* The generated include spelling needs this process's suffix. */
				FILE *source = fopen(src, "wb");
				int source_ok = source &&
					fprintf(source, "#include \"%s\"\nint stat_value=PRISM_CACHE_STAT;\n", hdr) >= 0;
				if (source && fclose(source) != 0) source_ok = 0;
				if (!source_ok) {
					ok = 0;
					break;
				}
				PrismFeatures f = prism_defaults();
				f.flatten_headers = false;
				PrismResult before = prism_transpile_file(src, f);
				struct stat old_id;
				int got_old = stat(hdr, &old_id) == 0;
				int rewrote = write_text_file(hdr, "#define PRISM_CACHE_STAT 222\n") &&
					      utimes(hdr, fixed_time) == 0;
				struct stat new_id;
				int got_new = stat(hdr, &new_id) == 0;
				PrismResult after = rewrote ? prism_transpile_file(src, f) : (PrismResult){0};
				if (got_old && got_new && old_id.st_size == new_id.st_size &&
				    old_id.st_mtime == new_id.st_mtime && old_id.st_ctime == new_id.st_ctime) {
					within_one_second = 1;
					ok = ok && before.status == PRISM_OK && before.output && strstr(before.output, "111") &&
					     rewrote && after.status == PRISM_OK && after.output && strstr(after.output, "222");
				}
				prism_free(&before);
				prism_free(&after);
			}
			/* The window that has to land inside one clock second spans two
			 * preprocessor spawns. On a slow host that is not flaky, it is
			 * impossible: CI runs riscv64 under qemu, where this was the only
			 * red job and failed every time. Try hard enough for a fast but
			 * loaded machine, then record that the shape could not be built
			 * here instead of reporting it as a prism defect. Never a silent
			 * pass -- the skip is counted and printed, and on any host quick
			 * enough to construct it the assertion above still runs. */
			ok = ok && names_ok;
			if (ok && !within_one_second) slow_clock_skips++;
			unlink(src);
			unlink(hdr);
		} else if (*p == 'G') {
			/* A directive between stripped raw prefixes must keep its own line:
			 * the following raw is elided and cannot supply emit_tok's usual BOL. */
			PrismResult raw_directive = prism_transpile_source(
			    "void raw_directive_test(void){raw\n#pragma GCC diagnostic push\nraw int x;(void)x;}\n",
			    "raw-directive.c", prism_defaults());
			ok = ok && raw_directive.status == PRISM_OK && raw_directive.output &&
			     strstr(raw_directive.output, "#pragma GCC diagnostic push\n") &&
			     compile_output(raw_directive.output, 0) == 0;
			prism_free(&raw_directive);
		} else if (*p == 'I') {
			/* Compiler command strings are argv, not shell fragments: embedded
			 * quotes and backslash escapes must remain inside one argument. */
			const char *argv[8] = {0};
			int argc = 0;
			char *dup = NULL;
			cc_split_into_argv(argv, &argc,
					   "cc -DNUM=7 -DSTR=\"a b\" -DSINGLE='c d' -DESC=a\\ b", &dup);
			int split_ok = dup && argc == 5 && !strcmp(argv[0], "cc") &&
			     !strcmp(argv[1], "-DNUM=7") && !strcmp(argv[2], "-DSTR=a b") &&
			     !strcmp(argv[3], "-DSINGLE=c d") && !strcmp(argv[4], "-DESC=a b");
			ok = ok && split_ok;
			free(dup);
			PrismFeatures f = prism_defaults();
			f.flatten_headers = false;
			f.compiler = "cc -DPRISM_CC_NUM=7 -DPRISM_CC_STR=\"a b\"";
			char input[256];
			snprintf(input, sizeof input, "/tmp/prism_recipe_compiler_command_%ld.c", (long)getpid());
			int wrote = write_text_file(
			    input, "#ifndef PRISM_CC_NUM\n#error missing compiler define\n#endif\nint compiler_command_value=PRISM_CC_NUM;\n");
			PrismResult command = wrote ? prism_transpile_file(input, f) : (PrismResult){0};
			int command_ok = command.status == PRISM_OK && command.output &&
			     strstr(command.output, "compiler_command_value") && strstr(command.output, "7") &&
			     compile_output(command.output, 0) == 0;
			if ((!split_ok || !command_ok) && getenv("PRISM_INTERNAL_TRACE"))
				fprintf(stderr, "compiler command split=%d argc=%d command=%d status=%d\n", split_ok,
					argc, command_ok, command.status);
			ok = ok && command_ok;
			prism_free(&command);
			unlink(input);
		} else if (*p == 'N') {
			PrismResult z = prism_transpile_file(NULL, prism_defaults());
			ok = ok && z.status == PRISM_ERR_IO && z.error_msg && strstr(z.error_msg, "NULL");
			prism_free(&z);
		} else if (*p == 'L') {
			/* `--prism-emit=<file>` must never truncate its input, including
			 * a distinct hard-link spelling.  The CLI's one-file restriction is
			 * kept in a small helper so this library-mode harness can exercise it. */
			char input[256], alias[256], header[256], guard[256], failing[256];
			const char source[] = "int prism_emit_input_value=17;\n";
			snprintf(input, sizeof input, "/tmp/prism_recipe_emit_%ld.c", (long)getpid());
			snprintf(alias, sizeof alias, "/tmp/prism_recipe_emit_alias_%ld.c", (long)getpid());
			snprintf(header, sizeof header, "/tmp/prism_recipe_emit_header_%ld.h", (long)getpid());
			snprintf(guard, sizeof guard, "/tmp/prism_recipe_emit_guard_%ld.c", (long)getpid());
			snprintf(failing, sizeof failing, "/tmp/prism_recipe_emit_failing_%ld.c", (long)getpid());
			unlink(input);
			unlink(alias);
			unlink(header);
			unlink(guard);
			unlink(failing);
			if (!write_text_file(input, source) || link(input, alias) != 0) {
				ok = 0;
			} else {
				int direct_rejected = !transpile(input, input) && errno == EINVAL;
				char *after_direct = read_file_padded(input);
				int alias_rejected = !transpile(input, alias) && errno == EINVAL;
				char *after_alias = read_file_padded(input);
				Cli one = {.output = "out.c", .source_count = 1};
				Cli many = {.output = "out.c", .source_count = 2};
				ok = ok && direct_rejected && alias_rejected && after_direct && after_alias &&
					!strcmp(after_direct, source) && !strcmp(after_alias, source) &&
					cli_emit_output_has_one_source(&one) && !cli_emit_output_has_one_source(&many);
				free(after_direct);
				free(after_alias);
			}
			/* Publishing only after translation keeps an include usable even when
			 * it is deliberately selected as the destination, and preserves an
			 * old destination when preprocessing rejects the input. */
			FILE *inc = fopen(header, "wb");
			int include_ok = inc && fprintf(inc, "#define PRISM_EMIT_HEADER_VALUE 41\n") > 0;
			if (inc && fclose(inc) != 0) include_ok = 0;
			FILE *source_fp = fopen(input, "wb");
			if (source_fp && fprintf(source_fp, "#include \"%s\"\nint prism_emit_header_value=PRISM_EMIT_HEADER_VALUE;\n", header) < 0)
				include_ok = 0;
			if (source_fp && fclose(source_fp) != 0) include_ok = 0;
			if (!source_fp) include_ok = 0;
			pparse_ctx_init();
			apply_features(prism_defaults());
			int header_output_ok = include_ok && transpile(input, header);
			char *header_output = read_file_padded(header);
			ok = ok && header_output_ok && header_output && strstr(header_output, "prism_emit_header_value") &&
			     strstr(header_output, "41") && compile_output(header_output, 0) == 0;
			free(header_output);
			prism_reset();

			int guard_ok = write_text_file(guard, "prism emit sentinel\n") &&
			       write_text_file(failing, "#error prism emit must not publish on preprocess failure\n");
			int saved_err = dup(STDERR_FILENO);
			FILE *err_sink = tmpfile();
			if (saved_err < 0 || !err_sink || fflush(stderr) != 0 ||
			    dup2(fileno(err_sink), STDERR_FILENO) < 0)
				guard_ok = 0;
			else {
				pparse_ctx_init();
				apply_features(prism_defaults());
				guard_ok = guard_ok && !transpile(failing, guard);
				prism_reset();
				fflush(stderr);
				if (dup2(saved_err, STDERR_FILENO) < 0) guard_ok = 0;
			}
			if (saved_err >= 0) close(saved_err);
			if (err_sink) fclose(err_sink);
			char *guard_after = read_file_padded(guard);
			ok = ok && guard_ok && guard_after && !strcmp(guard_after, "prism emit sentinel\n");
			free(guard_after);
			unlink(alias);
			unlink(input);
			unlink(header);
			unlink(guard);
			unlink(failing);
		} else if (*p == 'B') {
			/* Response files are expanded only while Prism owns argv. Exercise the
			 * boundary modes, operand splicing, BOMs, nested depth, and a pipe-backed
			 * response file so eager expansion cannot regress any of them. */
			char root[PATH_MAX] = {0}, normal[PATH_MAX] = {0}, outer[PATH_MAX] = {0};
			char operand[PATH_MAX] = {0}, xlang[PATH_MAX] = {0}, dep[PATH_MAX] = {0};
			char bom[PATH_MAX] = {0};
			char normal_arg[PATH_MAX + 2] = {0}, outer_arg[PATH_MAX + 2] = {0};
			char operand_arg[PATH_MAX + 2] = {0}, xlang_arg[PATH_MAX + 2] = {0};
			char dep_arg[PATH_MAX + 2] = {0}, bom_arg[PATH_MAX + 2] = {0};
			int setup = snprintf(root, sizeof root, "/tmp/prism_recipe_rsp_%ld", (long)getpid()) > 0 &&
			    snprintf(normal, sizeof normal, "%s/normal.rsp", root) > 0 &&
			    snprintf(outer, sizeof outer, "%s/outer.rsp", root) > 0 &&
			    snprintf(operand, sizeof operand, "%s/operand.rsp", root) > 0 &&
			    snprintf(xlang, sizeof xlang, "%s/xlang.rsp", root) > 0 &&
			    snprintf(dep, sizeof dep, "%s/dep.rsp", root) > 0 &&
			    snprintf(bom, sizeof bom, "%s/bom.rsp", root) > 0 && mkdir(root, 0700) == 0;
			if (setup) {
				snprintf(normal_arg, sizeof normal_arg, "@%s", normal);
				snprintf(outer_arg, sizeof outer_arg, "@%s", outer);
				snprintf(operand_arg, sizeof operand_arg, "@%s", operand);
				snprintf(xlang_arg, sizeof xlang_arg, "@%s", xlang);
				snprintf(dep_arg, sizeof dep_arg, "@%s", dep);
				snprintf(bom_arg, sizeof bom_arg, "@%s", bom);
				char outer_text[PATH_MAX + 64];
				setup = write_text_file(normal, "-DPRISM_RSP_NORMAL=88 x.c\n") &&
				    snprintf(outer_text, sizeof outer_text, "run x.c -- %s\n", normal_arg) > 0 &&
				    write_text_file(outer, outer_text) &&
				    write_text_file(operand, "rsp-output.o x.c\n") &&
				    write_text_file(xlang, "c rsp-extensionless\n") &&
				    write_text_file(dep, "rsp-output.d x.c\n");
				FILE *fp = fopen(bom, "wb");
				const unsigned char bom_text[] = {0xef, 0xbb, 0xbf, '-', 'D', 'P', 'R', 'I', 'S', 'M',
								  '_', 'B', 'O', 'M', '=', '1', ' ', 'x', '.', 'c', '\n'};
				if (!fp || fwrite(bom_text, 1, sizeof bom_text, fp) != sizeof bom_text || fclose(fp) != 0)
					setup = 0;
			}
			if (!setup) {
				ok = 0;
			} else {
				char *run_after_separator[] = {"prism", "run", "x.c", "--", normal_arg};
				Cli c = cli_parse(N(run_after_separator), run_after_separator);
				ok = ok && c.mode == CLI_RUN && c.source_count == 1 && c.prog_arg_count == 1 &&
				     !strcmp(c.prog_args[0], normal_arg);
				cli_free(&c);

				char *check_args[] = {"prism", "check", "tool", "x.c", normal_arg};
				c = cli_parse(N(check_args), check_args);
				ok = ok && c.mode == CLI_CHECK && c.source_count == 1 && c.check_arg_count == 2 &&
				     !strcmp(c.check_args[1], normal_arg);
				cli_free(&c);

				char *nested_separator[] = {"prism", outer_arg, "later.c"};
				c = cli_parse(N(nested_separator), nested_separator);
				ok = ok && c.mode == CLI_RUN && c.source_count == 1 && c.prog_arg_count == 2 &&
				     !strcmp(c.prog_args[0], normal_arg) && !strcmp(c.prog_args[1], "later.c");
				cli_free(&c);

				char *normal_args[] = {"prism", normal_arg};
				c = cli_parse(N(normal_args), normal_args);
				ok = ok && c.mode == CLI_DEFAULT && c.source_count == 1 && c.cc_arg_count == 1 &&
				     !strcmp(c.cc_args[0], "-DPRISM_RSP_NORMAL=88");
				cli_free(&c);

				/* `-f…` remains GNU's namespace on POSIX.  Only the explicit
				 * uppercase MSVC dash forms `-Fe` and `-Fo` are output switches. */
				char *gnu_f_args[] = {"prism", "-fexceptions", "x.c"};
				c = cli_parse(N(gnu_f_args), gnu_f_args);
				ok = ok && c.source_count == 1 && c.cc_arg_count == 1 && !c.output &&
				     !strcmp(c.cc_args[0], "-fexceptions");
				cli_free(&c);
				ok = ok && !msvc_flag_takes_arg("/") && !msvc_output_flag_kind("/");

				char *output_args[] = {"prism", "-o", operand_arg};
				c = cli_parse(N(output_args), output_args);
				ok = ok && c.source_count == 1 && c.output && !strcmp(c.output, "rsp-output.o");
				cli_free(&c);

				char *include_args[] = {"prism", "-I", operand_arg};
				c = cli_parse(N(include_args), include_args);
				ok = ok && c.source_count == 1 && c.cc_arg_count == 2 &&
				     !strcmp(c.cc_args[0], "-I") && !strcmp(c.cc_args[1], "rsp-output.o");
				cli_free(&c);

				char *x_args[] = {"prism", "-x", xlang_arg};
				c = cli_parse(N(x_args), x_args);
				ok = ok && c.source_count == 1 && c.cc_arg_count == 2 &&
				     !strcmp(c.sources[0], "rsp-extensionless");
				cli_free(&c);

				char *dep_args[] = {"prism", "-MF", dep_arg};
				c = cli_parse(N(dep_args), dep_args);
				ok = ok && c.source_count == 1 && c.dep_arg_count == 2 &&
				     !strcmp(c.dep_args[1], "rsp-output.d");
				cli_free(&c);

				char *bom_args[] = {"prism", bom_arg};
				c = cli_parse(N(bom_args), bom_args);
				ok = ok && c.source_count == 1 && c.cc_arg_count == 1 &&
				     !strcmp(c.cc_args[0], "-DPRISM_BOM=1");
				cli_free(&c);

				/* A directory is readable enough for fopen on some systems but not a
				 * response stream. Keep it literal instead of silently deleting it. */
				char directory_arg[PATH_MAX + 2];
				snprintf(directory_arg, sizeof directory_arg, "@%s", root);
				char *directory_args[] = {"prism", directory_arg};
				c = cli_parse(N(directory_args), directory_args);
				ok = ok && c.source_count == 0 && c.cc_arg_count == 1 &&
				     !strcmp(c.cc_args[0], directory_arg);
				cli_free(&c);

				char depth_paths[RSP_MAX_DEPTH + 1][PATH_MAX];
				int depth_ok = 1;
				for (int i = 0; i <= RSP_MAX_DEPTH; i++) {
					if (snprintf(depth_paths[i], sizeof depth_paths[i], "%s/depth-%d.rsp", root, i) <= 0)
						depth_ok = 0;
				}
				for (int i = 0; depth_ok && i <= RSP_MAX_DEPTH; i++) {
					char text[PATH_MAX + 4];
					if (i == RSP_MAX_DEPTH) snprintf(text, sizeof text, "x.c\n");
					else snprintf(text, sizeof text, "@%s\n", depth_paths[i + 1]);
					if (!write_text_file(depth_paths[i], text)) depth_ok = 0;
				}
				char depth_arg[PATH_MAX + 2];
				snprintf(depth_arg, sizeof depth_arg, "@%s", depth_paths[0]);
				if (depth_ok) {
					char *depth_args[] = {"prism", depth_arg};
					c = cli_parse(N(depth_args), depth_args);
					depth_ok = c.source_count == 1 && !strcmp(c.sources[0], "x.c");
					cli_free(&c);
				}
				ok = ok && depth_ok;
				for (int i = 0; i <= RSP_MAX_DEPTH; i++) unlink(depth_paths[i]);

				int pipefd[2];
				char pipe_arg[64];
				int pipe_ok = pipe(pipefd) == 0;
				const char pipe_text[] = "-DPRISM_RSP_PIPE=1 x.c\n";
				if (pipe_ok && (write(pipefd[1], pipe_text, sizeof pipe_text - 1) != sizeof pipe_text - 1 ||
						close(pipefd[1]) != 0)) {
					close(pipefd[0]);
					pipe_ok = 0;
				}
				if (pipe_ok) {
					snprintf(pipe_arg, sizeof pipe_arg, "@/dev/fd/%d", pipefd[0]);
					char *pipe_args[] = {"prism", pipe_arg};
					c = cli_parse(N(pipe_args), pipe_args);
					pipe_ok = c.source_count == 1 && c.cc_arg_count == 1 &&
						!strcmp(c.cc_args[0], "-DPRISM_RSP_PIPE=1");
					cli_free(&c);
					close(pipefd[0]);
				}
				ok = ok && pipe_ok;
			}
			unlink(normal);
			unlink(outer);
			unlink(operand);
			unlink(xlang);
			unlink(dep);
			unlink(bom);
			rmdir(root);
		} else if (*p == 'b') {
			/* Non-flatten mode used to hoist direct system headers and selected
			 * definitions. A synthetic -isystem path that looks like a Clang
			 * resource header exercises the old SYS_SKIP/re-emission path: pragma
			 * order and a post-include undef must both survive exactly. */
			char root[PATH_MAX] = {0}, lib[PATH_MAX] = {0}, clang_dir[PATH_MAX] = {0};
			char fixture[PATH_MAX] = {0}, inc[PATH_MAX] = {0};
			char first[PATH_MAX] = {0}, second[PATH_MAX] = {0}, source[PATH_MAX] = {0};
			int setup = snprintf(root, sizeof root, "/tmp/prism_recipe_system_order_%ld", (long)getpid()) > 0 &&
			    snprintf(lib, sizeof lib, "%s/lib", root) > 0 &&
			    snprintf(clang_dir, sizeof clang_dir, "%s/clang", lib) > 0 &&
			    snprintf(fixture, sizeof fixture, "%s/fixture", clang_dir) > 0 &&
			    snprintf(inc, sizeof inc, "%s/include", fixture) > 0 &&
			    snprintf(first, sizeof first, "%s/prism_system_first.h", inc) > 0 &&
			    snprintf(second, sizeof second, "%s/prism_system_second.h", inc) > 0 &&
			    snprintf(source, sizeof source, "%s/main.c", root) > 0 && mkdir(root, 0700) == 0 &&
			    mkdir(lib, 0700) == 0 && mkdir(clang_dir, 0700) == 0 && mkdir(fixture, 0700) == 0 &&
			    mkdir(inc, 0700) == 0;
			if (setup) {
				setup = write_text_file(first, "struct prism_system_header_pack { char c; int i; };\n") &&
				    write_text_file(second,
						    "#ifdef PRISM_SYSTEM_SWITCH\n"
						    "enum { prism_system_second_saw_switch = 1 };\n"
						    "#else\n"
						    "enum { prism_system_second_saw_switch = 0 };\n"
						    "#endif\n") &&
				    write_text_file(source,
						    "#define PRISM_SYSTEM_SWITCH 1\n"
						    "#pragma pack(push, 1)\n"
						    "#include <prism_system_first.h>\n"
						    "#pragma pack(pop)\n"
						    "#undef PRISM_SYSTEM_SWITCH\n"
						    "#include <prism_system_second.h>\n"
						    "_Static_assert(prism_system_second_saw_switch == 0, \"undef ordering\");\n"
						    "_Static_assert(sizeof(struct prism_system_header_pack) == 5, \"pragma ordering\");\n"
						    "int prism_system_header_order_value;\n");
			}
			if (!setup) {
				ok = 0;
			} else {
				const char *flags[] = {"-isystem", inc};
				PrismFeatures f = prism_defaults();
				f.flatten_headers = false;
				f.compiler_flags = flags;
				f.compiler_flags_count = N(flags);
				PrismResult result = prism_transpile_file(source, f);
				PPARSE_CTX();
				ok = ok && result.status == PRISM_OK && result.output &&
				     compile_output(result.output, 0) == 0 &&
				     !(_pc->features & PPARSE_F_FLATTEN);
				prism_free(&result);
			}
			unlink(first);
			unlink(second);
			unlink(source);
			rmdir(inc);
			rmdir(fixture);
			rmdir(clang_dir);
			rmdir(lib);
			rmdir(root);
		}
		if (before && !ok && failed_action && !*failed_action) *failed_action = *p;
		if (getenv("PRISM_INTERNAL_TRACE"))
			fprintf(stderr, "internal %c: %s%s\n", *p, ok ? "ok" : "failed",
				before ? "" : " (earlier failure)");
	}
	for (size_t v = 0; v < N(INTERNAL_ENV_VARS); v++) {
		if (saved_env[v]) setenv(INTERNAL_ENV_VARS[v], saved_env[v], 1);
		else unsetenv(INTERNAL_ENV_VARS[v]);
		free(saved_env[v]);
	}
	return ok;
}
#endif

#ifdef _WIN32
static int run_windows_internal(const Recipe *r, char *failed_action) {
	int ok = 1;
	for (const char *p = r->sequence; p && *p; p++) {
		int before = ok;
		if (*p == 'U') {
			/* The native Unicode environment must round-trip through Prism's UTF-8
			 * spawn snapshot and back into CreateProcessW's UTF-16 block. */
			const wchar_t name[] = L"PRISM_TEST_UNICODE_ENV";
			const wchar_t value[] = L"\u03c0\u4e2d";
			const wchar_t want[] = L"PRISM_TEST_UNICODE_ENV=\u03c0\u4e2d";
			SetLastError(ERROR_SUCCESS);
			DWORD saved_len = GetEnvironmentVariableW(name, NULL, 0);
			DWORD saved_error = GetLastError();
			wchar_t *saved = saved_len ? malloc((size_t)saved_len * sizeof(*saved)) : NULL;
			int had_saved = saved_len != 0 || saved_error == ERROR_SUCCESS;
			if ((saved_len && (!saved || GetEnvironmentVariableW(name, saved, saved_len) == 0)) ||
			    !SetEnvironmentVariableW(name, value)) {
				ok = 0;
			} else {
				char **env = build_clean_environ();
				wchar_t *block = win32_build_env_block(env);
				const char want_utf8[] = "\xcf\x80\xe4\xb8\xad";
				const char *split[4] = {0};
				int split_n = 0;
				char *split_dup = NULL;
				cc_split_into_argv(split, &split_n,
						   "C:\\Users\\O'Brien\\cl.exe -IC:\\O'Brien\\include", &split_dup);
				int found = 0;
				for (wchar_t *entry = block; block && *entry; ) {
					if (wcscmp(entry, want) == 0) found = 1;
					while (*entry) entry++;
					entry++;
				}
				ok = ok && env && block && found && prism_getenv("PRISM_TEST_UNICODE_ENV") &&
				     !strcmp(prism_getenv("PRISM_TEST_UNICODE_ENV"), want_utf8) && split_dup &&
				     split_n == 2 && !strcmp(split[0], "C:\\Users\\O'Brien\\cl.exe") &&
				     !strcmp(split[1], "-IC:\\O'Brien\\include");
				free(split_dup);
				free(block);
				free(env);
			}
			if (had_saved) SetEnvironmentVariableW(name, saved ? saved : L"");
			else SetEnvironmentVariableW(name, NULL);
			free(saved);

			/* MSVC options are parsed before source classification. In particular,
			 * /Fo is not /c, and slash-option operands must never become Prism TUs. */
			char *outputs[] = {"prism", "/c", "/Fo:", "obj.obj", "/Fe:", "app.exe", "main.c"};
			Cli cli = cli_parse(N(outputs), outputs);
			int cli_ok = cli.compile_only && cli.source_count == 1 &&
			     !strcmp(cli.sources[0], "main.c") && cli.msvc_obj_output &&
			     !strcmp(cli.msvc_obj_output, "obj.obj") && cli.msvc_exe_output &&
			     !strcmp(cli.msvc_exe_output, "app.exe") && cli.output && !strcmp(cli.output, "obj.obj");
			cli_free(&cli);
			char *force[] = {"prism", "/FI", "force.c", "main.c"};
			cli = cli_parse(N(force), force);
			cli_ok = cli_ok && cli.source_count == 1 && !strcmp(cli.sources[0], "main.c") &&
				 cli.cc_arg_count == 2 && !strcmp(cli.cc_args[0], "/FI") && !strcmp(cli.cc_args[1], "force.c");
			cli_free(&cli);
			char *define[] = {"prism", "/D", "NAME=foo.c", "main.c"};
			cli = cli_parse(N(define), define);
			cli_ok = cli_ok && cli.source_count == 1 && !strcmp(cli.sources[0], "main.c") &&
				 cli.cc_arg_count == 2 && !strcmp(cli.cc_args[0], "/D") && !strcmp(cli.cc_args[1], "NAME=foo.c");
			cli_free(&cli);
			char *preprocess[] = {"prism", "/E", "main.c"};
			cli = cli_parse(N(preprocess), preprocess);
			/* Preprocess passthrough deliberately leaves the input in the
			 * compiler argument list. It is not a Prism source to transpile. */
			cli_ok = cli_ok && cli.passthrough && cli.source_count == 0 &&
				 cli.cc_arg_count == 1 && !strcmp(cli.cc_args[0], "main.c");
			cli_free(&cli);
			char *stdin_source[] = {"prism", "-"};
			cli = cli_parse(N(stdin_source), stdin_source);
			cli_ok = cli_ok && cli.source_count == 1 && !strcmp(cli.sources[0], "-");
			cli_free(&cli);

			/* cl accepts UTF-16 response files. Also lock its Windows quote rule:
			 * two slashes before a quote yield one literal slash and close quotes. */
			char rsp_path[PATH_MAX], rsp_arg[PATH_MAX + 2];
			snprintf(rsp_path, sizeof rsp_path, "%sprism_recipe_utf16_rsp_%lu.rsp", get_tmp_dir(),
				 (unsigned long)GetCurrentProcessId());
			FILE *rsp = fopen(rsp_path, "wb");
			const char utf16_text[] = "-DPRISM_UTF16=1 x.c\n";
			int rsp_ok = rsp && fputc(0xff, rsp) != EOF && fputc(0xfe, rsp) != EOF;
			for (size_t i = 0; rsp_ok && i < sizeof utf16_text - 1; i++)
				rsp_ok = fputc((unsigned char)utf16_text[i], rsp) != EOF && fputc(0, rsp) != EOF;
			if (rsp && fclose(rsp) != 0) rsp_ok = 0;
			snprintf(rsp_arg, sizeof rsp_arg, "@%s", rsp_path);
			if (rsp_ok) {
				char *rsp_argv[] = {"prism", rsp_arg};
				cli = cli_parse(N(rsp_argv), rsp_argv);
				rsp_ok = cli.source_count == 1 && cli.cc_arg_count == 1 &&
					 !strcmp(cli.sources[0], "x.c") && !strcmp(cli.cc_args[0], "-DPRISM_UTF16=1");
				cli_free(&cli);
			}
			unlink(rsp_path);
			char **tokens = NULL, **owned_tokens = NULL;
			int token_n = 0, token_cap = 0, owned_n = 0, owned_cap = 0;
			int quote_ok = rsp_tokenize_buf("-DMSG=\"C:\\tmp\\\\\" \"C:\\Users\\O'Brien\\x.c\"",
							  strlen("-DMSG=\"C:\\tmp\\\\\" \"C:\\Users\\O'Brien\\x.c\""),
							  &tokens, &token_n, &token_cap, &owned_tokens, &owned_n, &owned_cap) == 0 &&
				 token_n == 2 && !strcmp(tokens[0], "-DMSG=C:\\tmp\\") &&
				 !strcmp(tokens[1], "C:\\Users\\O'Brien\\x.c");
			for (int i = 0; i < owned_n; i++) free(owned_tokens[i]);
			free(owned_tokens);
			free(tokens);
			ok = ok && cli_ok && rsp_ok && quote_ok;
		}
		if (before && !ok && failed_action && !*failed_action) *failed_action = *p;
	}
	return ok;
}
#endif

static int run_lifecycle(const Recipe *r, Stats *st, const AxisValue *sel[4]) {
	const char *clean = r->source;
	PrismFeatures f = patch_features(prism_defaults(), r->set_features, r->clear_features);
	PrismResult base = prism_transpile_source(clean, "lifecycle.c", f);
	char *want = base.output ? strdup(base.output) : NULL;
	int ok = base.status == PRISM_OK && want;
	prism_free(&base);
	for (const char *p = r->sequence; ok && p && *p; p++) {
		if (*p == 'R') prism_reset();
		else if (*p == 'C') prism_thread_cleanup();
		else if (*p == 'I') pparse_ctx_init();
		else if (*p == 'F') prism_free(NULL);
		else if (*p == 'E') {
			PrismResult x = prism_transpile_source("defer (void)0;", "bad.c", f);
			ok = x.status != PRISM_OK;
			prism_free(&x);
		} else if (*p == 'T') {
			PrismResult x = prism_transpile_source(clean, "lifecycle.c", f);
			ok = x.status == PRISM_OK && x.output && !strcmp(x.output, want);
			prism_free(&x);
		}
	}
	free(want);
	if (!ok) fail(st, r, sel, "lifecycle sequence %s diverged", r->sequence);
	return ok;
}

static int run_cli(const Recipe *r, Stats *st, const AxisValue *sel[4]) {
	char *argv[32];
	char rsp_path[256] = "";
	if (r->argc <= 0 || r->argc > 32) {
		fail(st, r, sel, "invalid argv size");
		return 0;
	}
	for (int i = 0; i < r->argc; i++) argv[i] = (char *)r->argv[i];
#ifndef _WIN32
	char rsp_arg[272];
	if (r->source) {
		snprintf(rsp_path, sizeof(rsp_path), "/tmp/prism_recipe_%ld.rsp", (long)getpid());
		FILE *fp = fopen(rsp_path, "wb");
		if (!fp) {
			fail(st, r, sel, "response file creation failed");
			return 0;
		}
		fwrite(r->source, 1, strlen(r->source), fp);
		fclose(fp);
		snprintf(rsp_arg, sizeof(rsp_arg), "@%s", rsp_path);
		for (int i = 0; i < r->argc; i++)
			if (!strcmp(argv[i], "@rsp@")) argv[i] = rsp_arg;
	}
#endif
	Cli c = cli_parse(r->argc, argv);
	int ok = c.mode == r->cli_mode && c.action == r->cli_action;
	if (r->cli_sources >= 0) ok = ok && c.source_count == r->cli_sources;
	if (r->cli_cc_args >= 0) ok = ok && c.cc_arg_count == r->cli_cc_args;
	unsigned got = feature_bits(c.features);
	ok = ok && (got & r->set_features) == r->set_features && (got & r->clear_features) == 0;
	if (!terms_present(c.output, r->must_have, 1)) ok = 0;
	if (!ok)
		fail(st, r, sel,
		     "CLI parse mismatch mode=%d action=%d sources=%d cc_args=%d features=0x%x output=%s",
		     c.mode, c.action, c.source_count, c.cc_arg_count, got,
		     c.output ? c.output : "-");
	cli_free(&c);
#ifndef _WIN32
	if (rsp_path[0]) unlink(rsp_path);
#endif
	return ok;
}

static void run_source_cell(const Recipe *r, const AxisValue *sel[4], long cell, Stats *st) {
	unsigned set = r->set_features, clear = r->clear_features;
	unsigned oracle = r->oracle;
	int expected_status_plus_one = 0;
	for (int i = 0; i < 4 && r->axes[i]; i++) {
		set |= sel[i]->set_features;
		clear |= sel[i]->clear_features;
		if (sel[i]->oracle) oracle = sel[i]->oracle;
		if (sel[i]->expected_status_plus_one)
			expected_status_plus_one = sel[i]->expected_status_plus_one;
	}
	PrismFeatures f = patch_features(prism_defaults(), set, clear);
	for (int i = 0; i < 4 && r->axes[i]; i++) f = patch_api_features(f, sel[i]);
	char *src = (oracle & O_NULL_SOURCE) ? NULL : render(r, sel, cell);
	if (!(oracle & O_NULL_SOURCE) && !src) {
		fail(st, r, sel, "source rendering failed");
		return;
	}
	if (oracle & O_ALLOC_FAIL) {
		char why[512] = "allocation fault sweep failed";
		if (!run_alloc_failure_sweep(src, (oracle & O_FILE) != 0, (oracle & O_REJECT) == 0,
					     (oracle & O_REJECT) != 0, f, why, sizeof(why)))
			fail(st, r, sel, "%s", why);
		free(src);
		return;
	}
	if (oracle & O_DRIVER_TRANSPILE) {
#ifndef _WIN32
		int ok = run_driver_transpile(r, src, f);
		if (!ok) fail(st, r, sel, "driver transpile failed source=%.260s", src);
#else
		st->skipped++;
#endif
		free(src);
		return;
	}

	PrismResult x;
	if (oracle & O_FILE) {
#ifndef _WIN32
		char path[256];
		snprintf(path, sizeof(path), "/tmp/prism_recipe_file_%ld_%ld.c", (long)getpid(), cell);
		FILE *fp = fopen(path, "w");
		if (!fp) {
			free(src);
			fail(st, r, sel, "temp source open failed");
			return;
		}
		fwrite(src, 1, strlen(src), fp);
		fclose(fp);
		x = prism_transpile_file(path, f);
		for (int retry = 0; retry < 4 && prism_result_is_spawn_refusal(&x); retry++) {
			prism_free(&x);
			infra_retries++;
			x = prism_transpile_file(path, f);
		}
		unlink(path);
#else
		st->skipped++;
		free(src);
		return;
#endif
	} else {
		x = prism_transpile_source(src, "recipe.c", f);
		for (int retry = 0; retry < 4 && prism_result_is_spawn_refusal(&x); retry++) {
			prism_free(&x);
			infra_retries++;
			x = prism_transpile_source(src, "recipe.c", f);
		}
	}
	int result_shape_ok = x.status == PRISM_OK
		? x.output && x.output_len == strlen(x.output)
		: !x.output && x.output_len == 0;
	if (expected_status_plus_one && x.status != expected_status_plus_one - 1) {
		fail(st, r, sel, "status changed: expected=%d got=%d source=%.260s",
		     expected_status_plus_one - 1, x.status, src ? src : "<null>");
		prism_free(&x);
		free(src);
		return;
	}

	int replay_ok = 1;
	if (oracle & O_REPLAY) {
		PrismResult y = prism_transpile_source(src, "recipe.c", f);
		replay_ok = x.status == y.status;
		if (replay_ok && x.output && y.output) replay_ok = !strcmp(x.output, y.output);
		else if (replay_ok) replay_ok = !x.output && !y.output;
		if (replay_ok && x.error_msg && y.error_msg) replay_ok = !strcmp(x.error_msg, y.error_msg);
		else if (replay_ok) replay_ok = !x.error_msg && !y.error_msg;
		prism_free(&y);
	}
	/* Replay is an independent determinism assertion, not a replacement for
	 * the cell's semantic oracle.  Keeping it orthogonal lets the historical
	 * corpus lock status/output/diagnostics while successful cells still pass
	 * through the totality and fixed-point checks below. */
	int ok = result_shape_ok;
	if (oracle & O_ANY_STATUS) {
		ok = x.status >= PRISM_OK && x.status <= PRISM_ERR_IO;
		/* A locked non-success corpus result must remain an explained
		 * diagnostic, not merely the same numeric status. */
		if (expected_status_plus_one && x.status != PRISM_OK)
			ok = ok && x.error_msg && x.error_msg[0];
	} else if (oracle & O_FEATURE_MATRIX) {
		/* The matrix source is purposefully valid C for every feature
		 * configuration.  Check the transformation contracts by name, so a
		 * successful parse without the requested rewrite cannot count as
		 * coverage.  The defer/orelse snippets occur only in their enabled
		 * cells; both must be completely eliminated from emitted C. */
		unsigned bits = feature_bits(f);
		ok = x.status == PRISM_OK && x.output;
		if (ok && (bits & FB_DEFER)) ok = count_kw(x.output, "defer") == 0;
		if (ok && (bits & FB_ORELSE)) ok = count_kw(x.output, "orelse") == 0;
		if (ok)
			ok = terms_present(x.output, "__prism_matrix_zero = 0", (bits & FB_ZERO) != 0);
		if (ok)
			ok = terms_present(x.output, "__prism_bchk", (bits & FB_BOUNDS) != 0);
		if (ok)
			ok = terms_present(x.output, "static \nconst int __prism_matrix_static",
					   (bits & FB_AS) != 0);
		if (ok) {
			int has_auto_unreachable = strstr(x.output, "__builtin_unreachable") != NULL ||
						   strstr(x.output, "__assume(0)") != NULL;
			ok = has_auto_unreachable == ((bits & FB_AUR) != 0);
		}
		if (ok) ok = terms_present(x.output, "#line", (bits & FB_LINE) != 0);
		/* Every configuration must also reparse its generated C with every
		 * transformation disabled.  The feature-specific runtime products
		 * cover execution; keeping this 512-cell parser product in-process
		 * makes it cheap enough to run on every edit. */
		if (ok) {
			PrismFeatures fp = f;
			fp.defer = fp.orelse = fp.zeroinit = fp.bounds_check = false;
			fp.auto_static = fp.auto_unreachable = false;
			fp.warn_safety = true;
			PrismResult y = prism_transpile_source(x.output, "matrix-fixed.c", fp);
			ok = y.status == PRISM_OK && y.output && normalized_equal(x.output, y.output);
			prism_free(&y);
		}
#ifndef _WIN32
		/* The matrix's main function checks the deferred marker, fallback,
		 * bounds access, and promoted table result.  Run it after checking the
		 * emitted spelling so every POSIX configuration has a semantic oracle. */
		if (ok) ok = compile_output(x.output, 1) == 0;
#endif
	} else if (oracle & O_TRICHOTOMY) {
		if (x.status != PRISM_OK) {
			ok = x.error_msg && x.error_msg[0];
		} else if (!x.output) {
			ok = 0;
		} else {
			int oe = count_kw(x.output, "orelse"), df = count_kw(x.output, "defer");
			PrismFeatures fp = f;
			fp.zeroinit = false;
			fp.bounds_check = false;
			fp.auto_static = false;
			fp.auto_unreachable = false;
			fp.warn_safety = true;
			PrismResult y = prism_transpile_source(x.output, "fixed.c", fp);
			ok = y.status == PRISM_OK && y.output && normalized_equal(x.output, y.output);
			prism_free(&y);
			/* A non-idempotent survivor may be an ordinary soft-keyword
			 * identifier. In that case the feature-disabled pipeline must
			 * produce exactly the same output. */
			if (!ok && (oe || df)) {
				PrismFeatures off = f;
				if (oe) off.orelse = false;
				if (df) off.defer = false;
				y = prism_transpile_source(src, "recipe.c", off);
				ok = y.status == PRISM_OK && y.output && !strcmp(x.output, y.output);
				prism_free(&y);
			}
		}
	} else {
		if (oracle & O_OK) ok = x.status == PRISM_OK && x.output;
		if (oracle & O_REJECT) ok = x.status != PRISM_OK;
		if ((oracle & O_DIAG) && (!x.error_msg || !x.error_msg[0])) ok = 0;
		if (r->diagnostic && (!x.error_msg || !strstr(x.error_msg, r->diagnostic))) ok = 0;
		if ((oracle & O_NO_EXT) && x.output &&
		    (count_kw(x.output, "defer") || count_kw(x.output, "orelse") || count_kw(x.output, "raw")))
			ok = 0;
		if (!terms_present(x.output, r->must_have, 1)) ok = 0;
		if (!terms_present(x.output, r->must_not_have, 0)) ok = 0;
		if ((oracle & O_FIXED) && x.status == PRISM_OK && x.output) {
			PrismFeatures fp = f;
			fp.zeroinit = fp.auto_static = fp.auto_unreachable = fp.bounds_check = false;
			fp.warn_safety = true;
			PrismResult y = prism_transpile_source(x.output, "fixed.c", fp);
			ok = ok && y.status == PRISM_OK && y.output && normalized_equal(x.output, y.output);
			prism_free(&y);
		}
		if ((oracle & O_OUTPUT_EQ_OFF) && x.status == PRISM_OK && x.output) {
			PrismFeatures off = f;
			off.defer = off.orelse = false;
			PrismResult y = prism_transpile_source(src, "recipe.c", off);
			ok = ok && y.status == PRISM_OK && y.output && !strcmp(x.output, y.output);
			prism_free(&y);
		}
#ifndef _WIN32
		if ((oracle & O_COMPILE) && x.status == PRISM_OK && x.output)
			ok = ok && compile_output(x.output, 0) == 0;
		if ((oracle & O_RUN) && x.status == PRISM_OK && x.output)
			ok = ok && compile_output(x.output, 1) == r->expected_exit;
		if ((oracle & O_REFERENCE_RUN) && x.status == PRISM_OK && x.output) {
			int reference_rc = compile_output(src, 1);
			int prism_rc = compile_output(x.output, 1);
			ok = ok && reference_rc >= 0 && prism_rc == reference_rc;
		}
		if ((oracle & O_TRAP) && x.status == PRISM_OK && x.output) {
			int rc = compile_output(x.output, 1);
			ok = ok && rc > 0 && rc < 256;
		}
#endif
	}
	ok = ok && replay_ok;
	if (!ok)
		fail(st, r, sel, "status=%d diag=%s source=%.260s", x.status,
		     x.error_msg ? x.error_msg : "-", src ? src : "<null>");
	prism_free(&x);
	free(src);
}

static void recipe_run(const Recipe *recipes, size_t count, Stats *st) {
	const char *filter = getenv("PRISM_RECIPE_FILTER");
	/* Optional focused-shard selector.  Unlike PRISM_RECIPE_FILTER this matches
	 * an axis value tag, so a large product can be bisected without changing
	 * generator code or weakening its unfiltered gate. */
	const char *axis_filter = getenv("PRISM_AXIS_FILTER");
	/* Optional shard selector, "i/n": run only the recipes whose index is
	 * congruent to i modulo n. Recipes are independent and every temp path
	 * already carries the pid, so n processes cover the gate between them with
	 * no thread pool and no shared state -- 24.9s becomes 3.1s at n=32 on a
	 * 32-core host. Interleaving rather than blocking keeps adjacent expensive
	 * families (the four feature-matrix layouts) in different shards. Splitting
	 * this way is only sound because run_internal now restores the environment
	 * it borrows; before that, results depended on what had run earlier.
	 * CI runs with the variable unset. */
	long shard_i = 0, shard_n = 1;
	{
		const char *sh = getenv("PRISM_RECIPE_SHARD");
		if (sh && *sh) {
			char *end = NULL;
			long a = strtol(sh, &end, 10);
			if (end && *end == '/' && a >= 0) {
				long b = strtol(end + 1, NULL, 10);
				if (b > 0 && a < b) {
					shard_i = a;
					shard_n = b;
				}
			}
		}
	}
	for (size_t ri = 0; ri < count; ri++) {
		if (shard_n > 1 && (long)(ri % (size_t)shard_n) != shard_i) continue;
		const Recipe *r = &recipes[ri];
		if (filter && *filter && !strstr(r->id, filter)) continue;
		if ((r->requires & capabilities()) != r->requires) {
			st->skipped++;
			continue;
		}
		if (r->oracle & O_CLI) {
			const AxisValue *none[4] = {0};
			st->cells++;
			if (run_cli(r, st, none)) st->passed++;
			continue;
		}
		if (r->oracle & O_INTERNAL) {
			const AxisValue *none[4] = {0};
			char failed_action = 0;
			st->cells++;
#ifndef _WIN32
			if (run_internal(r, &failed_action)) st->passed++;
			else fail(st, r, none, "internal action %c failed in sequence %s",
				  failed_action ? failed_action : '?', r->sequence);
#else
			if ((r->requires & CAP_WINDOWS) && run_windows_internal(r, &failed_action)) st->passed++;
			else if (r->requires & CAP_WINDOWS)
				fail(st, r, none, "internal action %c failed in sequence %s",
				     failed_action ? failed_action : '?', r->sequence);
			else
				st->skipped++;
#endif
			continue;
		}
		if (r->oracle & O_LIFECYCLE) {
			const AxisValue *none[4] = {0};
			st->cells++;
			if (run_lifecycle(r, st, none)) st->passed++;
			continue;
		}
		size_t dims = 0, idx[4] = {0}, sizes[4] = {1, 1, 1, 1};
		while (dims < 4 && r->axes[dims]) {
			sizes[dims] = r->axes[dims]->count;
			dims++;
		}
		int done = 0;
		while (!done) {
			const AxisValue *sel[4] = {0};
			bool selected = !axis_filter || !*axis_filter;
			for (size_t i = 0; i < dims; i++) {
				sel[i] = &r->axes[i]->values[idx[i]];
				if (axis_filter && *axis_filter && strstr(sel[i]->tag, axis_filter)) selected = true;
			}
			if (selected) {
				long before = st->failed, skipped_before = st->skipped;
				st->cells++;
				run_source_cell(r, sel, st->cells, st);
				if (st->failed == before && st->skipped == skipped_before) st->passed++;
			}
			if (!dims) break;
			for (size_t i = dims; i-- > 0;) {
				if (++idx[i] < sizes[i]) break;
				idx[i] = 0;
				if (i == 0) done = 1;
			}
		}
	}
}

/* ---- Shared axes ----------------------------------------------------- */

static const AxisValue expr_values[] = {
	{"oe-call", "g() orelse 7", 0, 0},
	{"oe-ident", "x orelse 7", 0, 0},
	{"oe-chain", "g() orelse h() orelse 9", 0, 0},
	{"oe-ret", "g() orelse return 0", 0, 0},
	{"oe-goto", "g() orelse goto L", 0, 0},
	{"oe-block", "g() orelse { x++; }", 0, 0},
	{"oe-paren", "(g() orelse 7)", 0, 0},
	{"oe-comma", "(x, g()) orelse 7", 0, 0},
	{"oe-member", "s.v orelse 7", 0, 0},
	{"oe-subscript", "a[i] orelse 7", 0, 0},
	{"oe-deref", "*p orelse 7", 0, 0},
	{"oe-assign", "x = g() orelse 7", 0, 0},
	{"defer-ident", "defer", 0, 0},
	{"orelse-ident", "orelse", 0, 0},
};
static const Axis ax_expr = {"payload", expr_values, N(expr_values)};

static const AxisValue expr_wrappers[] = {
	{"plain", "@0@", 0, 0}, {"paren", "(@0@)", 0, 0},
	{"comma", "(0,@0@)", 0, 0}, {"ternary-then", "(i?@0@:1)", 0, 0},
	{"ternary-else", "(i?1:@0@)", 0, 0}, {"cast", "((int)(@0@))", 0, 0},
	{"neg", "(-(@0@))", 0, 0}, {"not", "(!(@0@))", 0, 0},
	{"binary", "((@0@)+1)", 0, 0}, {"call", "q(@0@)", 0, 0},
	{"subscript", "a[(@0@)&7]", 0, 0},
	{"stmt-expr", "({int t=(@0@);t;})", 0, 0},
};
static const Axis ax_expr_wrap = {"wrapper", expr_wrappers, N(expr_wrappers)};

static const AxisValue expr_contexts[] = {
	{"return", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};L:return @1@;}", 0, 0},
	{"init", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};int y=@1@;L:return y;}", 0, 0},
	{"assign", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};x=@1@;L:return x;}", 0, 0},
	{"arg", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};L:return q(@1@);}", 0, 0},
	{"if", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};if(@1@)x++;L:return x;}", 0, 0},
	{"while", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};while(@1@)break;L:return x;}", 0, 0},
	{"switch", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};switch(@1@){default:break;}L:return x;}", 0, 0},
	{"ternary", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};int y=i?@1@:1;L:return y;}", 0, 0},
	{"comma", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};int y=(0,@1@);L:return y;}", 0, 0},
	{"paren", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};int y=(@1@);L:return y;}", 0, 0},
	{"cast", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};int y=(int)(@1@);L:return y;}", 0, 0},
	{"sizeof", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};int y=sizeof(@1@);L:return y;}", 0, 0},
	{"typeof", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};typeof(@1@)y=0;L:return !!y;}", 0, 0},
	{"generic", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};int y=_Generic(@1@,int:1,default:0);L:return y;}", 0, 0},
	{"array-dim", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};int z[(@1@)+1];L:return sizeof(z);}", 0, 0},
	{"stmt-expr", "int q(int);int f(int i){int x=0,a[8]={0},*p=a;struct{int v;}s={0};int y=(@1@);L:return y;}", 0, 0},
};
static const Axis ax_expr_ctx = {"context", expr_contexts, N(expr_contexts)};

static const AxisValue stmt_values[] = {
	{"defer-call", "defer clean();", 0, 0},
	{"defer-block", "defer { clean(); }", 0, 0},
	{"defer-empty", "defer { }", 0, 0},
	{"defer-nested", "defer { if(x) clean(); }", 0, 0},
	{"defer-oe", "defer cleanv(g() orelse 1);", 0, 0},
	{"oe-assign", "x = g() orelse 1;", 0, 0},
	{"oe-decl", "int y = g() orelse 1;(void)y;", 0, 0},
	{"raw-decl", "raw int y;(void)y;", 0, 0},
	{"raw-block", "raw { int y;(void)y; }", 0, 0},
};
static const Axis ax_stmt = {"payload", stmt_values, N(stmt_values)};

static const AxisValue stmt_contexts[] = {
	{"top", "void clean(void);void cleanv(int);int g(void);void f(int x){ @0@ }", 0, 0},
	{"block", "void clean(void);void cleanv(int);int g(void);void f(int x){{{ @0@ }}}", 0, 0},
	{"if-brace", "void clean(void);void cleanv(int);int g(void);void f(int x){if(x){ @0@ }}", 0, 0},
	{"else-brace", "void clean(void);void cleanv(int);int g(void);void f(int x){if(x){}else{ @0@ }}", 0, 0},
	{"while", "void clean(void);void cleanv(int);int g(void);void f(int x){while(x){ @0@ break;}}", 0, 0},
	{"for", "void clean(void);void cleanv(int);int g(void);void f(int x){for(;x;){ @0@ break;}}", 0, 0},
	{"do", "void clean(void);void cleanv(int);int g(void);void f(int x){do{ @0@ }while(0);}", 0, 0},
	{"switch", "void clean(void);void cleanv(int);int g(void);void f(int x){switch(x){case 0:{ @0@ }break;}}", 0, 0},
	{"label", "void clean(void);void cleanv(int);int g(void);void f(int x){L:{ @0@ }if(x)goto L;}", 0, 0},
	{"stmt-expr", "void clean(void);void cleanv(int);int g(void);void f(int x){(void)({ @0@ 0;});}", 0, 0},
};
static const Axis ax_stmt_ctx = {"context", stmt_contexts, N(stmt_contexts)};

static const AxisValue decl_values[] = {
	{"int", "int x;", 0, 0}, {"ptr", "int *p;", 0, 0},
	{"array", "int a[8];", 0, 0}, {"multi", "int a,b,*p;", 0, 0},
	{"struct", "struct S{int a;long b;}s;", 0, 0},
	{"union", "union U{int a;long b;}u;", 0, 0},
	{"atomic", "_Atomic int a;", 0, 0}, {"volatile", "volatile int v;", 0, 0},
	{"const", "const int c;", 0, 0}, {"typeof", "int q=1;typeof(q)x;", 0, 0},
	{"vla", "int n=3;int a[n];", 0, 0},
	{"paren-ptr", "int (*p);", 0, 0}, {"array-ptr", "int (*p)[4];", 0, 0},
	{"func-ptr", "int (*fp)(int);", 0, 0},
	{"attr-before", "__attribute__((unused)) int x;", 0, 0},
	{"attr-mid", "int __attribute__((unused)) x;", 0, 0},
	{"attr-after", "int x __attribute__((unused));", 0, 0},
	{"raw-int", "raw int x;", 0, 0}, {"raw-array", "raw int a[4];", 0, 0},
	{"static", "static int x;", 0, 0}, {"extern", "extern int x;", 0, 0},
	{"register", "register int x;", 0, 0},
};
static const Axis ax_decl = {"decl", decl_values, N(decl_values)};

static const AxisValue decl_contexts[] = {
	{"function", "void f(void){ @0@ (void)0; }", 0, 0},
	{"nested", "void f(void){{{ @0@ (void)0; }}}", 0, 0},
	{"loop", "void f(int c){while(c){ @0@ break;}}", 0, 0},
	{"switch", "void f(int c){switch(c){case 0:{ @0@ }break;}}", 0, 0},
};
static const Axis ax_decl_ctx = {"context", decl_contexts, N(decl_contexts)};

static const AxisValue bounds_values[] = {
	{"read", "a[i]", 0, 0}, {"write", "a[i]=1", 0, 0},
	{"multi", "m[i][j]", 0, 0}, {"deref", "*(a+i)", 0, 0},
	{"reverse", "i[a]", 0, 0}, {"addr", "&a[8]", 0, 0},
	{"paren-addr", "*&a[i]", 0, 0}, {"post-and", "x++ & a[i]", 0, 0},
	{"compound-and", "((int){5}) & a[i]", 0, 0},
};
static const Axis ax_bounds = {"expr", bounds_values, N(bounds_values)};

static const AxisValue bounds_contexts[] = {
	{"return", "int f(int i,int j){int a[8]={0},m[4][5]={{0}},x=1;return (@0@);}", 0, 0},
	{"assign", "int f(int i,int j){int a[8]={0},m[4][5]={{0}},x=1;x=(@0@);return x;}", 0, 0},
	{"if", "int f(int i,int j){int a[8]={0},m[4][5]={{0}},x=1;if(@0@)x++;return x;}", 0, 0},
	{"while", "int f(int i,int j){int a[8]={0},m[4][5]={{0}},x=1;while(@0@)break;return x;}", 0, 0},
	{"sizeof", "int f(int i,int j){int a[8]={0},m[4][5]={{0}},x=1;return sizeof(@0@);}", 0, 0},
	{"typeof", "int f(int i,int j){int a[8]={0},m[4][5]={{0}},x=1;typeof(@0@)y=0;return !!y;}", 0, 0},
	{"generic", "int f(int i,int j){int a[8]={0},m[4][5]={{0}},x=1;return _Generic(@0@,int:1,default:0);}", 0, 0},
	{"stmt", "void f(int i,int j){int a[8]={0},m[4][5]={{0}},x=1;(void)(@0@);}", 0, 0},
};
static const Axis ax_bounds_ctx = {"context", bounds_contexts, N(bounds_contexts)};

static const AxisValue feature_values[] = {
	{"default", "", 0, 0},
	{"no-defer", "", 0, FB_DEFER}, {"no-orelse", "", 0, FB_ORELSE},
	{"no-zero", "", 0, FB_ZERO}, {"no-bounds", "", 0, FB_BOUNDS},
	{"only-defer", "", FB_DEFER, FB_ORELSE | FB_ZERO | FB_BOUNDS | FB_AS | FB_AUR},
	{"only-orelse", "", FB_ORELSE, FB_DEFER | FB_ZERO | FB_BOUNDS | FB_AS | FB_AUR},
	{"minimal", "", 0, FB_DEFER | FB_ORELSE | FB_ZERO | FB_BOUNDS | FB_AS | FB_AUR},
	{"safety-warn", "", FB_WARN, 0}, {"bounds", "", FB_BOUNDS, 0},
	{"auto-static", "", FB_AS, 0}, {"auto-unreach", "", FB_AUR, 0},
	{"no-flatten", "", 0, FB_FLAT},
	{"library-shape", "", FB_WARN, FB_FLAT | FB_LINE},
};
static const Axis ax_features = {"features", feature_values, N(feature_values)};

/* The ordinary feature axis deliberately focuses on useful public profiles.
 * This family is different: it enumerates every 2^7 state of the five
 * language transformations plus line directives and auto-unreachable. Each
 * configuration is replayed in four statement/control layouts. Its source
 * remains ISO-C when `defer` or `orelse` are disabled, so every one of the
 * resulting 512 cells has a strict success/emission/fixed-point oracle—and,
 * on POSIX, a runtime oracle—rather than counting an expected syntax error
 * as coverage. */
#define FM_DEFER_0 ""
#define FM_DEFER_1 "defer __prism_matrix_mark();"
#define FM_ORELSE_0 "int __prism_matrix_value=9;"
#define FM_ORELSE_1 "int __prism_matrix_value=__prism_matrix_source() orelse 9;"
#define FM_LOG_0 "0"
#define FM_LOG_1 "1"
#define FM_HEAD \
	"static int __prism_matrix_log;" \
	"void abort(void);" \
	"static int __prism_matrix_dead(void){abort();return 0;}" \
	"static int __prism_matrix_source(void){return 0;}" \
	"static void __prism_matrix_mark(void){__prism_matrix_log++;}" \
	"static int __prism_matrix_worker(void){"
#define FM_BODY(d, o) \
	"int __prism_matrix_zero;" \
	"int __prism_matrix_bounds[2]={4,5};" \
	"const int __prism_matrix_static[2]={7,8};" \
	"volatile int __prism_matrix_index=1;" \
	FM_DEFER_ ## d FM_ORELSE_ ## o \
	"return __prism_matrix_bounds[__prism_matrix_index]+" \
	"__prism_matrix_static[0]+__prism_matrix_value;"
#define FM_CONTEXT_0(body) body
#define FM_CONTEXT_1(body) "{" body "}"
#define FM_CONTEXT_2(body) "if(1){" body "}return 0;"
#define FM_CONTEXT_3(body) "switch(0){default:{" body "}}return 0;"
#define FM_CAT_INNER(a, b) a ## b
#define FM_CAT(a, b) FM_CAT_INNER(a, b)
#define FM_TAIL(d) \
	"}" \
	"int main(void){return __prism_matrix_worker()==21&&__prism_matrix_log==" FM_LOG_ ## d "?0:1;}"
#define FM_SOURCE(d, o, c) FM_HEAD FM_CAT(FM_CONTEXT_, c)(FM_BODY(d, o)) FM_TAIL(d)
#define FM_SET(d, o, z, b, s) \
	((d) ? FB_DEFER : 0) | ((o) ? FB_ORELSE : 0) | ((z) ? FB_ZERO : 0) | \
	((b) ? FB_BOUNDS : 0) | ((s) ? FB_AS : 0)
#define FM_CLEAR(d, o, z, b, s) \
	((d) ? 0 : FB_DEFER) | ((o) ? 0 : FB_ORELSE) | ((z) ? 0 : FB_ZERO) | \
	((b) ? 0 : FB_BOUNDS) | ((s) ? 0 : FB_AS)
#define FM_CELL(c, d, o, z, b, s) \
	{.tag="ctx" #c "-d" #d "-o" #o "-z" #z "-b" #b "-s" #s, .text=FM_SOURCE(d, o, c), \
	 .set_features=FM_SET(d, o, z, b, s), .clear_features=FM_CLEAR(d, o, z, b, s)}
#define FM_ROWS(c) \
	FM_CELL(c, 0, 0, 0, 0, 0), FM_CELL(c, 0, 0, 0, 0, 1), \
	FM_CELL(c, 0, 0, 0, 1, 0), FM_CELL(c, 0, 0, 0, 1, 1), \
	FM_CELL(c, 0, 0, 1, 0, 0), FM_CELL(c, 0, 0, 1, 0, 1), \
	FM_CELL(c, 0, 0, 1, 1, 0), FM_CELL(c, 0, 0, 1, 1, 1), \
	FM_CELL(c, 0, 1, 0, 0, 0), FM_CELL(c, 0, 1, 0, 0, 1), \
	FM_CELL(c, 0, 1, 0, 1, 0), FM_CELL(c, 0, 1, 0, 1, 1), \
	FM_CELL(c, 0, 1, 1, 0, 0), FM_CELL(c, 0, 1, 1, 0, 1), \
	FM_CELL(c, 0, 1, 1, 1, 0), FM_CELL(c, 0, 1, 1, 1, 1), \
	FM_CELL(c, 1, 0, 0, 0, 0), FM_CELL(c, 1, 0, 0, 0, 1), \
	FM_CELL(c, 1, 0, 0, 1, 0), FM_CELL(c, 1, 0, 0, 1, 1), \
	FM_CELL(c, 1, 0, 1, 0, 0), FM_CELL(c, 1, 0, 1, 0, 1), \
	FM_CELL(c, 1, 0, 1, 1, 0), FM_CELL(c, 1, 0, 1, 1, 1), \
	FM_CELL(c, 1, 1, 0, 0, 0), FM_CELL(c, 1, 1, 0, 0, 1), \
	FM_CELL(c, 1, 1, 0, 1, 0), FM_CELL(c, 1, 1, 0, 1, 1), \
	FM_CELL(c, 1, 1, 1, 0, 0), FM_CELL(c, 1, 1, 1, 0, 1), \
	FM_CELL(c, 1, 1, 1, 1, 0), FM_CELL(c, 1, 1, 1, 1, 1)
static const AxisValue feature_matrix_top_values[] = {FM_ROWS(0)};
static const AxisValue feature_matrix_block_values[] = {FM_ROWS(1)};
static const AxisValue feature_matrix_if_values[] = {FM_ROWS(2)};
static const AxisValue feature_matrix_switch_values[] = {FM_ROWS(3)};
static const Axis ax_feature_matrix_top = {"configuration", feature_matrix_top_values, N(feature_matrix_top_values)};
static const Axis ax_feature_matrix_block = {"configuration", feature_matrix_block_values, N(feature_matrix_block_values)};
static const Axis ax_feature_matrix_if = {"configuration", feature_matrix_if_values, N(feature_matrix_if_values)};
static const Axis ax_feature_matrix_switch = {"configuration", feature_matrix_switch_values, N(feature_matrix_switch_values)};

static const AxisValue feature_matrix_emission_values[] = {
	{"auto-unreachable-off/line-off", "", 0, FB_AUR | FB_LINE},
	{"auto-unreachable-off/line-on", "", FB_LINE, FB_AUR},
	{"auto-unreachable-on/line-off", "", FB_AUR, FB_LINE},
	{"auto-unreachable-on/line-on", "", FB_AUR | FB_LINE, 0},
};
static const Axis ax_feature_matrix_emission = {
	"emission", feature_matrix_emission_values, N(feature_matrix_emission_values)
};
#undef FM_ROWS
#undef FM_CELL
#undef FM_CLEAR
#undef FM_SET
#undef FM_SOURCE
#undef FM_TAIL
#undef FM_CAT
#undef FM_CAT_INNER
#undef FM_CONTEXT_3
#undef FM_CONTEXT_2
#undef FM_CONTEXT_1
#undef FM_CONTEXT_0
#undef FM_BODY
#undef FM_HEAD
#undef FM_LOG_1
#undef FM_LOG_0
#undef FM_ORELSE_1
#undef FM_ORELSE_0
#undef FM_DEFER_1
#undef FM_DEFER_0

/* Every unsupported array-address form has two public contracts: strict
 * safety mode must stop before producing C, while -fno-safety must preserve
 * valid host C and leave the unverifiable access uninstrumented. Keep both
 * outcomes generated from the same hazard alphabet. */
static const AxisValue safety_bounds_hazard_values[] = {
	{"deref-add", "int f(int i){int a[4]={0};return *(a+i);}", 0, 0},
	{"subscript-add", "int f(int i){int a[4]={0};return (a+i)[0];}", 0, 0},
	{"reverse-subscript", "int f(int i){int a[4]={0};return i[a];}", 0, 0},
	{"conditional-base",
	 "int f(int choose,int i){int a[4]={0},b[2]={0};return (choose?a:b)[i];}", 0, 0},
};
static const Axis ax_safety_bounds_hazard = {
	"hazard", safety_bounds_hazard_values, N(safety_bounds_hazard_values)
};

/* Library-facing count/pointer states and preprocessor argv shapes.  The
 * generic feature patcher above applies these directly to PrismFeatures. */
static const char *api_include_ok[] = {"."};
static const char *api_define_ok[] = {"RECIPE_DEF=7", "RECIPE_TWO=9"};
static const char *api_cflag_ok[] = {
	"-DRECIPE_FLAG=11", "-URECIPE_GONE", "-D", "RECIPE_SPLIT=13",
	"-D", "_GNU_SOURCE", "-D_DARWIN_C_SOURCE",
	"-c", "-S", "/c", "/Fo", "discard.o", "/Fejoined",
	"-o", "discard.o", "-save-temps", "sibling.c", "-include", "/dev/null",
};
static const char *api_force_ok[] = {"/dev/null"};
static const char *api_order_defines[] = {"RECIPE_ORDER=1", "RECIPE_ORDER=2"};
static const char *api_order_cflags[] = {"-DRECIPE_ORDER=1", "-DRECIPE_ORDER=2"};
static const char *api_null[] = {NULL};
static const char *api_include_mixed[] = {NULL, "."};
static const char *api_define_mixed[] = {NULL, "RECIPE_DEF=7"};
static const char *api_cflag_mixed[] = {NULL, "sibling.c", "-DRECIPE_FLAG=11"};
static const char *api_force_mixed[] = {NULL, "/dev/null"};
static const AxisValue api_feature_values[] = {
	{.tag="include/both", .oracle=O_FILE|O_OK, .api_field=AF_INCLUDE, .api_values=api_include_ok, .api_count=1},
	{.tag="include/count-only", .oracle=O_FILE|O_OK, .api_field=AF_INCLUDE, .api_count=3},
	{.tag="include/pointer-only", .oracle=O_FILE|O_OK, .api_field=AF_INCLUDE, .api_values=api_include_ok},
	{.tag="include/negative-count", .oracle=O_FILE|O_OK, .api_field=AF_INCLUDE, .api_values=api_include_ok, .api_count=(-2147483647 - 1)},
	{.tag="include/null-entry", .oracle=O_FILE|O_OK, .api_field=AF_INCLUDE, .api_values=api_null, .api_count=1},
	{.tag="include/mixed-null", .oracle=O_FILE|O_OK, .api_field=AF_INCLUDE, .api_values=api_include_mixed, .api_count=N(api_include_mixed)},
	{.tag="define/both", .oracle=O_FILE|O_OK, .clear_features=FB_FLAT, .api_field=AF_DEFINE, .api_values=api_define_ok, .api_count=2},
	{.tag="define/count-only", .oracle=O_FILE|O_OK, .api_field=AF_DEFINE, .api_count=3},
	{.tag="define/pointer-only", .oracle=O_FILE|O_OK, .api_field=AF_DEFINE, .api_values=api_define_ok},
	{.tag="define/negative-count", .oracle=O_FILE|O_OK, .api_field=AF_DEFINE, .api_values=api_define_ok, .api_count=(-2147483647 - 1)},
	{.tag="define/null-entry", .oracle=O_FILE|O_OK, .clear_features=FB_FLAT, .api_field=AF_DEFINE, .api_values=api_null, .api_count=1},
	{.tag="define/mixed-null", .oracle=O_FILE|O_OK, .clear_features=FB_FLAT, .api_field=AF_DEFINE, .api_values=api_define_mixed, .api_count=N(api_define_mixed)},
	{.tag="cflag/all-shapes", .oracle=O_FILE|O_OK, .clear_features=FB_FLAT, .api_field=AF_CFLAG, .api_values=api_cflag_ok, .api_count=N(api_cflag_ok)},
	{.tag="cflag/count-only", .oracle=O_FILE|O_OK, .api_field=AF_CFLAG, .api_count=3},
	{.tag="cflag/pointer-only", .oracle=O_FILE|O_OK, .api_field=AF_CFLAG, .api_values=api_cflag_ok},
	{.tag="cflag/negative-count", .oracle=O_FILE|O_OK, .api_field=AF_CFLAG, .api_values=api_cflag_ok, .api_count=(-2147483647 - 1)},
	{.tag="cflag/null-entry", .oracle=O_FILE|O_OK, .clear_features=FB_FLAT, .api_field=AF_CFLAG, .api_values=api_null, .api_count=1},
	{.tag="cflag/mixed-null", .oracle=O_FILE|O_OK, .clear_features=FB_FLAT, .api_field=AF_CFLAG, .api_values=api_cflag_mixed, .api_count=N(api_cflag_mixed)},
	{.tag="force/both", .oracle=O_FILE|O_OK, .api_field=AF_FORCE, .api_values=api_force_ok, .api_count=1},
	{.tag="force/count-only", .oracle=O_FILE|O_OK, .api_field=AF_FORCE, .api_count=3},
	{.tag="force/pointer-only", .oracle=O_FILE|O_OK, .api_field=AF_FORCE, .api_values=api_force_ok},
	{.tag="force/negative-count", .oracle=O_FILE|O_OK, .api_field=AF_FORCE, .api_values=api_force_ok, .api_count=(-2147483647 - 1)},
	{.tag="force/null-entry", .oracle=O_FILE|O_OK, .api_field=AF_FORCE, .api_values=api_null, .api_count=1},
	{.tag="force/mixed-null", .oracle=O_FILE|O_OK, .api_field=AF_FORCE, .api_values=api_force_mixed, .api_count=N(api_force_mixed)},
	{.tag="compiler/default", .oracle=O_FILE|O_OK, .api_field=AF_COMPILER},
	{.tag="compiler/argv", .oracle=O_FILE|O_OK, .api_field=AF_COMPILER, .api_compiler="cc -std=gnu11"},
	{.tag="compiler/failing", .api_field=AF_COMPILER, .api_compiler="false"},
};
static const Axis ax_api_features = {"state", api_feature_values, N(api_feature_values)};

static const AxisValue api_order_values[] = {
	{.tag="defines", .text="", .api_field=AF_DEFINE, .api_values=api_order_defines,
	 .api_count=N(api_order_defines)},
	{.tag="compiler-flags", .text="", .api_field=AF_CFLAG, .api_values=api_order_cflags,
	 .api_count=N(api_order_cflags)},
};
static const Axis ax_api_order = {"source", api_order_values, N(api_order_values)};

static const AxisValue msvc_target_values[] = {
	{.tag="cl", .api_field=AF_COMPILER, .api_compiler="cl"},
};
static const Axis ax_msvc_target = {"compiler", msvc_target_values, N(msvc_target_values)};

/* Whole translation units used through prism_transpile_file.  This is the
 * preprocessor/driver-facing alphabet: every row is crossed with the same
 * feature axis, so adding one lexical or declaration form exercises every
 * supported feature configuration automatically. */
static const AxisValue file_values[] = {
	{"plain", "int f(void){int x;return x;}\n", 0, 0},
	{"define", "#define V 7\nint f(void){int x=V;return x;}\n", 0, 0},
	{"system-includes", "#include <assert.h>\n#include <stddef.h>\nint f(void){assert(1);return (int)sizeof(size_t);}\n", 0, 0},
	{"define-cont", "#define SUM(a,b) \\" "\n ((a)+(b))\nint f(void){return SUM(2,3);}\n", 0, 0},
	{"conditional", "#if 1\nint f(void){return 1;}\n#else\nint f(void){return 0;}\n#endif\n", 0, 0},
	{"line-comments", "// opener { defer orelse\nint f(void){int x;return x;} // tail }\n", 0, 0},
	{"block-comments", "/* { defer orelse */ int f(void){int x;/* } */return x;}\n", 0, 0},
	{"string-noise", "const char*s=\"{ defer orelse } // /*\";int f(void){return s[0];}\n", 0, 0},
	{"char-noise", "int f(void){int x='}';return x=='{';}\n", 0, 0},
	{"enum", "enum E{A=1,B=A+2,C};int f(void){enum E e=B;return e;}\n", 0, 0},
	{"enum-typed", "enum E:unsigned{A=1,B=2};int f(void){enum E e=B;return e;}\n", 0, 0},
	{"typedef-shadow", "typedef int T;int f(void){T x;{int T=2;T*x;}T y;return x+y;}\n", 0, 0},
	{"tag-shadow", "struct S{volatile int x;};int f(int S){typeof(struct S)x;(void)x;return S;}\n", 0, 0},
	{"multidecl", "struct S{int x;};int f(void){int a,b=2,*p;struct S x,y;return a+b+!!p+x.x+y.x;}\n", 0, 0},
	{"attributes", "__attribute__((noreturn,cold))void die(void);int f(int x){[[maybe_unused]]int y;if(x)die();return y;}\n", 0, 0},
	{"noreturn", "_Noreturn void die(void);int f(int x){int y;if(x)die();return y;}\n", 0, 0},
	{"noreturn-ptr", "_Noreturn void die(void);int f(void){typeof(die)*p=die;(void)p;return 0;}\n", 0, 0},
	{"knr", "int add(a,b)int a;int b;{int x;return a+b+x;}\n", 0, 0},
	{"generic", "int f(int x){return _Generic((x),int:x+1,default:0);}\n", 0, 0},
	{"generic-assoc", "int f(int x){return _Generic((x),int:(x orelse 3),default:0);}\n", 0, 0},
	{"compound", "struct S{int a[3];};int f(void){struct S s=(struct S){.a={[1]=4}};return s.a[1];}\n", 0, 0},
	{"range-designator", "int f(void){int a[8]={[1 ... 4]=3};return a[2];}\n", 0, 0},
	{"stmt-expr", "int f(void){int x=({int y;defer(void)0;y;});return x;}\n", 0, 0},
	{"labels", "int f(int x){int y;goto L;{int z;L:y=x+z;}return y;}\n", 0, 0},
	{"computed-goto", "int f(int x){static void*p[]={&&A,&&B};goto*p[x&1];A:return 1;B:return 2;}\n", 0, 0},
	{"switch", "int f(int x){switch(x){case 0:{int y;defer(void)0;return y;}case 1 ... 3:return 2;default:return 4;}}\n", 0, 0},
	{"loops", "int f(int n){int x;for(int i=0;i<n;i++){defer x++;if(i&1)continue;if(i==4)break;}return x;}\n", 0, 0},
	{"do-while", "int f(int n){int x;do{defer x++;if(n--)continue;}while(n>0);return x;}\n", 0, 0},
	{"orelse-const", "int f(int x){int a=x orelse 7;int b=x orelse 0;return a+b;}\n", 0, 0},
	{"orelse-const-decl", "int g(void);int f(void){const int x=g() orelse 7;return x;}\n", 0, 0},
	{"orelse-volatile", "int g(void);int f(void){volatile int x=g() orelse 7;return x;}\n", 0, 0},
	{"orelse-atomic", "int g(void);int f(void){_Atomic int x=g() orelse 7;return x;}\n", 0, 0},
	{"orelse-multidecl", "int g(void);int f(void){int a=g() orelse 1,b=g() orelse 2,*p;return a+b+!!p;}\n", 0, 0},
	{"orelse-actions", "int g(void);int f(void){defer(void)0;int x=g() orelse return 1;x=g() orelse goto L;L:return x;}\n", 0, 0},
	{"orelse-block", "int g(void);int f(void){int x=g() orelse {return 3;};return x;}\n", 0, 0},
	{"orelse-bracket", "int g(void);int f(void){int a[g() orelse 4];return sizeof(a);}\n", 0, 0},
	{"atomic", "struct S{int x;};int f(void){_Atomic int x;_Atomic(struct S)y;return x+y.x;}\n", 0, 0},
	{"typeof-vla", "int f(int n){int a[n];typeof(a)b;return sizeof(b);}\n", 0, 0},
	{"funcptr-vla-param", "int f(int n){raw typeof(void(*)(int[n]))p;p=0;return !!p;}\n", 0, 0},
	{"array-funcptr", "int f(int n){typeof(void(*[n])(int))a;(void)a;return n;}\n", 0, 0},
	{"raw-product", "raw static const int a=1;void f(void){raw int x;raw {int y;(void)y;}(void)x;}\n", 0, 0},
	{"raw-pragma", "#pragma pack(push,1)\nraw struct S{char c;int x;};\n#pragma pack(pop)\n", 0, 0},
	{"bounds", "struct S{int a[4];};int f(int i){int a[8],m[3][5];struct S s;return a[i]+m[i][i]+s.a[i];}\n", 0, 0},
	{"bounds-shadows", "int a[9];int f(int i){int x=a[i];{int*a=0;x+=a[i];}{int a[3];x+=a[i];}return x+a[i];}\n", 0, 0},
	{"soft-ident", "struct defer{int orelse;};int defer(int);int f(void){struct defer x;int orelse=defer(1);return x.orelse+orelse;}\n", 0, 0},
	{"digraph", "int f(int i)<%int a<:4:>={<%1%>};return a<:i:>;%>\n", 0, 0},
	{"asm", "int f(int x){int y;__asm__ volatile(\"\":\"=r\"(y):\"r\"(x):\"memory\");return y;}\n", 0, 0},
	{"bitint", "int f(void){_BitInt(17)x;return (int)x;}\n", 0, 0},
	{"constexpr", "int f(void){constexpr int N=4;int a[N];return sizeof(a);}\n", 0, 0},
	{"static-literals", "int f(void){const int a[4]={1,2};const char s[]=\"hi\";const struct{int x;}v={3};return a[0]+s[0]+v.x;}\n", 0, 0},
	{"ice-contexts", "int g(void);enum E{A=(g() orelse 2)};struct S{int b:(g() orelse 3);};_Static_assert((g() orelse 1),\"x\");int f(int x){switch(x){case (g() orelse 4):return 1;}return 0;}\n", 0, 0},
	{"attrs-many", "int f(int n __attribute__((unused))){__attribute__((unused)) int a[4] __attribute__((aligned(16)));int(*__attribute__((unused))p)[n]=0;return !!p;}\n", 0, 0},
	{"label-defer", "void c(void);int f(int x){defer c();if(x)goto L;{defer c();L:return x;}}\n", 0, 0},
};
static const Axis ax_files = {"source", file_values, N(file_values)};

static const AxisValue driver_values[] = {
	{"plain", "int f(void){int x;return x;}\n", 0, 0},
	{"define", "#define V 9\nint f(void){return V;}\n", 0, 0},
	{"conditional", "#ifdef X\n#define V 1\n#else\n#define V 2\n#endif\nint f(void){return V;}\n", 0, 0},
	{"comment", "/* multiline\n * comment with #define X and defer\n */\nint f(void){int x;return x;}\n", 0, 0},
	{"string", "const char*s=\"/* #define X defer */\";int f(void){return s[0];}\n", 0, 0},
	{"enum", "enum E{A=1,B=2};int f(void){enum E e=B;return e;}\n", 0, 0},
	{"attribute", "_Noreturn void die(void);int f(int x){int y;if(x)die();return y;}\n", 0, 0},
	{"defer", "void c(void);int f(void){int x;defer c();return x;}\n", 0, 0},
	{"orelse", "int g(void);int f(void){const int x=g() orelse 7;return x;}\n", 0, 0},
	{"raw", "#pragma pack(push,1)\nraw struct S{char c;int x;};\n#pragma pack(pop)\n", 0, 0},
	{"bounds", "int f(int i){int a[8]={0};return a[i];}\n", 0, 0},
	{"define-comment-inline", "#define V (1 /* kept apart */ + 2)\nint f(void){return V;}\n", 0, 0},
	{"define-comment-multiline", "#define V (1 /* across\n*/ + 3)\nint f(void){return V;}\n", 0, 0},
	{"hash-comment-directive", "# /* across\n*/ define V 4\nint f(void){return V;}\n", 0, 0},
	{"digraph-define", "%:define V 5\nint f(void){return V;}\n", 0, 0},
	{"trigraph-define", "?" "?=define V 6\nint f(void){return V;}\n", 0, 0},
	{"conditional-nested", "#if 1\n#if 0\n#define V 1\n#elif 1\n#define V 7\n#else\n#define V 2\n#endif\n#endif\nint f(void){return V;}\n", 0, 0},
	{"define-string-comment-noise", "#define V \"/* not a comment */\"\nconst char*f(void){return V;}\n", 0, 0},
	{"comment-splice-before-define", "/* open \\\nstill comment */\n#define V 8\nint f(void){return V;}\n", 0, 0},
	{"switch-depth-65",
	 "#define S1 switch(x){default:\n#define S2 S1 S1\n#define S4 S2 S2\n"
	 "#define S8 S4 S4\n#define S16 S8 S8\n#define S32 S16 S16\n"
	 "#define E1 }\n#define E2 E1 E1\n#define E4 E2 E2\n#define E8 E4 E4\n"
	 "#define E16 E8 E8\n#define E32 E16 E16\n"
	 "int f(int x){S32 S32 S1 (void)x;E32 E32 E1 return x;}\n", 0, 0},
};
static const Axis ax_driver = {"source", driver_values, N(driver_values)};

static const AxisValue path_values[] = {
	{"self", ".github/test.c", 0, 0},
	{"prism", "prism.c", 0, 0},
	{"verify-smoke", ".github/verify_smoke.c", 0, 0},
	{"defer-proof", ".github/cbmc_defer.c", 0, 0},
};
static const Axis ax_paths = {"path", path_values, N(path_values)};

static const AxisValue passthrough_values[] = {
	{"promotions", "int main(void){unsigned char a=250,b=10;return (int)(a+b)==260?0:1;}", 0, 0},
	{"floating", "int main(void){long double x=0.25L,y=0.5L;return x+y==0.75L?0:1;}", 0, 0},
	{"macros", "#define F(x) ((x)*(x))\n#define CAT(a,b) a##b\nint main(void){int CAT(v,1)=F(4);return v1==16?0:1;}", 0, 0},
	{"struct-value", "struct S{int x,y;};static struct S add(struct S a,struct S b){return(struct S){a.x+b.x,a.y+b.y};}int main(void){struct S s=add((struct S){1,2},(struct S){3,4});return s.x==4&&s.y==6?0:1;}", 0, 0},
	{"static-local", "static int f(void){static int n;n++;return n;}int main(void){return f()==1&&f()==2?0:1;}", 0, 0},
	{"function-pointer", "static int a(int x){return x+2;}static int b(int x){return x*3;}int main(void){int(*f[2])(int)={a,b};return f[0](4)==6&&f[1](4)==12?0:1;}", 0, 0},
	{"flex-array", "#include <stdlib.h>\nstruct S{int n;int a[];};int main(void){struct S*s=malloc(sizeof(*s)+3*sizeof(int));if(!s)return 2;s->n=3;s->a[2]=7;int r=s->n==3&&s->a[2]==7?0:1;free(s);return r;}", 0, 0},
	{"offsetof", "#include <stddef.h>\nstruct S{char c;long x;};int main(void){return offsetof(struct S,x)>=sizeof(char)?0:1;}", 0, 0},
	{"alignment", "int main(void){_Alignas(32) char x;return ((unsigned long)&x%_Alignof(x))==0?0:1;}", 0, 0},
	{"restrict", "static void add(int n,int*restrict a,const int*restrict b){for(int i=0;i<n;i++)a[i]+=b[i];}int main(void){int a[3]={1,2,3},b[3]={4,5,6};add(3,a,b);return a[2]==9?0:1;}", 0, 0},
	{"inline", "static inline int twice(int x){return x*2;}int main(void){return twice(9)==18?0:1;}", 0, 0},
	{"generic", "#define K(x) _Generic((x),int:1,long:2,default:3)\nint main(void){return K(0)==1&&K(0L)==2&&K(0.0)==3?0:1;}", 0, 0},
	{"goto-loops", "int main(void){int n=0;for(int i=0;i<3;i++)for(int j=0;j<3;j++){n++;if(n==5)goto done;}done:return n==5?0:1;}", 0, 0},
	{"switch-goto", "int main(void){int x=2,n=0;again:switch(x){case 2:n+=2;x=1;goto again;case 1:n+=3;break;}return n==5?0:1;}", 0, 0},
	{"strings", "#include <string.h>\nint main(void){char s[8]=\"prism\";return strlen(s)==5&&memcmp(s,\"prism\",5)==0?0:1;}", 0, 0},
	{"varargs", "#include <stdarg.h>\nstatic int sum(int n,...){va_list a;va_start(a,n);int s=0;for(int i=0;i<n;i++)s+=va_arg(a,int);va_end(a);return s;}int main(void){return sum(4,1,2,3,4)==10?0:1;}", 0, 0},
	{"compound-literal", "struct P{int x,y;};int main(void){struct P*p=&(struct P){7,9};return p->x+p->y==16?0:1;}", 0, 0},
	{"soft-defer-operators", "int main(void){int defer=1,x=3;x+=defer;x-=defer;x*=defer;x/=defer;x%=defer;x|=defer;x&=defer;x^=defer;x<<=defer;x>>=defer;if(x==defer||x!=defer)defer++;return (x&&defer)?0:1;}", 0, 0},
	{"union", "union U{unsigned n;unsigned char b[4];};int main(void){union U u={.n=0};u.b[0]=1;return u.b[0]==1?0:1;}", 0, 0},
	{"last-array-element", "int main(void){int a[4]={1,2,3,4};volatile int i=3;return a[i]==4?0:1;}", 0, 0},
	{"recursion", "static int fib(int n){return n<2?n:fib(n-1)+fib(n-2);}int main(void){return fib(10)==55?0:1;}", 0, 0},
};
static const Axis ax_passthrough = {
	"program", passthrough_values, N(passthrough_values)
};

/* Executed products retained from the retired generative suite.  These are
 * deliberately axes, not callbacks: every selected cell is still rendered
 * and judged by recipe_run's generic compile/execute oracle. */
static const AxisValue runtime_truth_values[] = {
	{"falsy", "0", 0, 0},
	{"truthy", "7", 0, 0},
};
static const Axis ax_runtime_truth = {"truth", runtime_truth_values, N(runtime_truth_values)};

/* C translation phase 2 joins a backslash-newline before Prism recognizes
 * soft keywords. Cross every extension spelling with LF/CRLF and the two
 * possible phase-2 backslash origins (literal and trigraph): a lexer that
 * sees physical fragments instead of the joined token misses the rewrite. */
static const AxisValue runtime_spliced_keyword_values[] = {
	{"defer-lf",
	 "static int calls;static void mark(void){calls++;}int main(void){de\\\nfer mark();return calls==0?0:1;}",
	 0, 0},
	{"defer-crlf",
	 "static int calls;static void mark(void){calls++;}int main(void){de\\\r\nfer mark();return calls==0?0:1;}",
	 0, 0},
	/* Translation phase 1 converts ??/ to backslash before phase 2 removes
	 * the resulting newline splice.  Split the spelling across C string
	 * literals so compiling this test source cannot consume the trigraph. */
	{"defer-trigraph-lf",
	 "static int calls;static void mark(void){calls++;}int main(void){de?" "?/\nfer mark();return calls==0?0:1;}",
	 0, 0},
	{"defer-trigraph-crlf",
	 "static int calls;static void mark(void){calls++;}int main(void){de?" "?/\r\nfer mark();return calls==0?0:1;}",
	 0, 0},
	{"orelse-lf",
	 "static int src(void){return 0;}int main(void){int x=src() or\\\nelse 7;return x==7?0:1;}",
	 0, 0},
	{"orelse-crlf",
	 "static int src(void){return 0;}int main(void){int x=src() or\\\r\nelse 7;return x==7?0:1;}",
	 0, 0},
	{"orelse-trigraph-lf",
	 "static int src(void){return 0;}int main(void){int x=src() or?" "?/\nelse 7;return x==7?0:1;}",
	 0, 0},
	{"orelse-trigraph-crlf",
	 "static int src(void){return 0;}int main(void){int x=src() or?" "?/\r\nelse 7;return x==7?0:1;}",
	 0, 0},
	{"raw-lf", "int main(void){raw\\\n int ignored;return 0;}", 0, 0},
	{"raw-crlf", "int main(void){raw\\\r\n int ignored;return 0;}", 0, 0},
	{"raw-trigraph-lf", "int main(void){raw?" "?/\n int ignored;return 0;}", 0, 0},
	{"raw-trigraph-crlf", "int main(void){raw?" "?/\r\n int ignored;return 0;}", 0, 0},
	/* Exercise every other phase-1 spelling as parser punctuation, plus the
	 * trigraph directive introducer.  Each pair is split to keep this host C
	 * source from translating it before the generated recipe sees it. */
	{"trigraph-punctuation",
	 "?" "?=define TRI 7\nint main(void)" "?" "?<int a" "?" "?(2" "?" "?)="
	 "?" "?<1,2" "?" "?>;return(a" "?" "?(0" "?" "?)" "?" "?'a" "?" "?(1"
	 "?" "?))==3&&" "?" "?-0==-1&&(0" "?" "?!1)&&TRI==7?0:1;" "?" "?>",
	 0, 0},
};
static const Axis ax_runtime_spliced_keyword = {
	"keyword", runtime_spliced_keyword_values, N(runtime_spliced_keyword_values)
};

static const AxisValue runtime_orelse_fallback_values[] = {
	{"constant", "11", 0, 0},
	{"call", "(__need_fb=1,fbc())", 0, 0},
	{"expression", "(__need_fb=0,5+6)", 0, 0},
};
static const Axis ax_runtime_orelse_fallback = {
	"fallback", runtime_orelse_fallback_values, N(runtime_orelse_fallback_values)
};

static const AxisValue runtime_orelse_bind_values[] = {
	{"declaration", "int x=src() orelse @2@;__value=x;", 0, 0},
	{"assignment", "int x;x=src() orelse @2@;__value=x;", 0, 0},
};
static const Axis ax_runtime_orelse_bind = {
	"binding", runtime_orelse_bind_values, N(runtime_orelse_bind_values)
};

static const AxisValue runtime_orelse_site_values[] = {
	{"block", "{@1@}", 0, 0},
	{"loop", "for(int i=0;i<1;i++){@1@}", 0, 0},
	{"switch", "switch(1){case 1:{@1@}break;}", 0, 0},
	{"if", "if(1){@1@}", 0, 0},
};
static const Axis ax_runtime_orelse_site = {
	"site", runtime_orelse_site_values, N(runtime_orelse_site_values)
};

static const AxisValue runtime_chain_values[] = {
	{"first-truthy", "int main(void){int x=g(5) orelse g(6) orelse fb(9);return x==5&&gc==1&&fc==0?0:1;}", 0, 0},
	{"second-truthy", "int main(void){int x=g(0) orelse g(6) orelse fb(9);return x==6&&gc==2&&fc==0?0:1;}", 0, 0},
	{"all-falsy", "int main(void){int x=g(0) orelse g(0) orelse fb(9);return x==9&&gc==2&&fc==1?0:1;}", 0, 0},
	{"third-truthy", "int main(void){int x=g(0) orelse g(0) orelse g(7) orelse fb(9);return x==7&&gc==3&&fc==0?0:1;}", 0, 0},
};
static const Axis ax_runtime_chain = {"path", runtime_chain_values, N(runtime_chain_values)};

static const AxisValue runtime_qualified_values[] = {
	{"volatile", "volatile", 0, 0},
	{"atomic", "_Atomic", 0, 0},
};
static const Axis ax_runtime_qualified = {
	"qualifier", runtime_qualified_values, N(runtime_qualified_values)
};

static const AxisValue runtime_switch_values[] = {
	{"case-0", "int main(void){return run(0)==307&&calls==3&&same(\"A1B2\")?0:1;}", 0, 0},
	{"case-1", "int main(void){return run(1)==207&&calls==2&&same(\"1B2\")?0:1;}", 0, 0},
	{"case-2", "int main(void){return run(2)==7&&calls==1&&same(\"2\")?0:1;}", 0, 0},
	{"default", "int main(void){return run(5)==400&&calls==1&&same(\"\")?0:1;}", 0, 0},
};
static const Axis ax_runtime_switch = {
	"selector", runtime_switch_values, N(runtime_switch_values)
};

static const AxisValue runtime_orelse_defer_values[] = {
	{"return",
	 "static int run(void){defer ev('A');{defer ev('a');ev('p');int x=src() orelse return 9;(void)x;ev('q');}ev('z');return 3;}"
	 "int main(void){int r=run();return @0@?(r==3&&same(\"pqazA\")?0:1):(r==9&&same(\"paA\")?0:2);}",
	 0, 0},
	{"goto",
	 "static int run(void){defer ev('A');{defer ev('a');ev('p');int x=src() orelse goto L;(void)x;ev('q');}ev('z');L:ev('l');return 3;}"
	 "int main(void){int r=run();return @0@?(r==3&&same(\"pqazlA\")?0:1):(r==3&&same(\"palA\")?0:2);}",
	 0, 0},
	{"break",
	 "static int run(void){defer ev('A');for(int i=0;i<1;i++){defer ev('a');ev('p');int x=src() orelse break;(void)x;ev('q');}ev('z');return 3;}"
	 "int main(void){int r=run();return r==3&&(@0@?same(\"pqazA\"):same(\"pazA\"))?0:1;}",
	 0, 0},
	{"continue",
	 "static int run(void){defer ev('A');for(int i=0;i<1;i++){defer ev('a');ev('p');int x=src() orelse continue;(void)x;ev('q');}ev('z');return 3;}"
	 "int main(void){int r=run();return r==3&&(@0@?same(\"pqazA\"):same(\"pazA\"))?0:1;}",
	 0, 0},
	{"switch-break",
	 "static int run(void){defer ev('A');switch(0){case 0:{defer ev('a');ev('p');int x=src() orelse break;(void)x;ev('q');}ev('x');}ev('z');return 3;}"
	 "int main(void){int r=run();return r==3&&(@0@?same(\"pqaxzA\"):same(\"pazA\"))?0:1;}",
	 0, 0},
};
static const Axis ax_runtime_orelse_defer = {
	"action", runtime_orelse_defer_values, N(runtime_orelse_defer_values)
};

/* Bare-assignment orelse has a separate lowering from declarations.  Keep the
 * lvalue shapes runtime-checked: the generated branches must assign exactly
 * once, including an lvalue whose evaluation has a side effect.  The member
 * RHS cases also cover the bit-field typeof promotion path. */
static const AxisValue runtime_bare_orelse_lvalue_values[] = {
	{"ident",
	 "int main(void){int x=0;x=src() orelse fb();return x==(@0@?@0@:9)&&gc==1&&fc==((!@0@)?1:0)?0:1;}",
	 0, 0},
	{"deref",
	 "int main(void){int x=0,*p=&x;*p=src() orelse fb();return x==(@0@?@0@:9)&&gc==1&&fc==((!@0@)?1:0)?0:1;}",
	 0, 0},
	{"subscript",
	 "int main(void){int a[2]={0};a[1]=src() orelse fb();return a[1]==(@0@?@0@:9)&&gc==1&&fc==((!@0@)?1:0)?0:1;}",
	 0, 0},
	{"subscript-side-effect",
	 "int main(void){int a[2]={0},i=0;a[i++]=src() orelse fb();return a[0]+i;}",
	 0, 0, O_REJECT | O_DIAG},
	{"member",
	 "struct S{int v;};int main(void){struct S s={0};s.v=src() orelse fb();return s.v==(@0@?@0@:9)&&gc==1&&fc==((!@0@)?1:0)?0:1;}",
	 0, 0},
	{"pointer-member",
	 "struct S{int v;};int main(void){struct S s={0},*p=&s;p->v=src() orelse fb();return s.v==(@0@?@0@:9)&&gc==1&&fc==((!@0@)?1:0)?0:1;}",
	 0, 0},
	{"bitfield-lhs",
	 "struct B{unsigned v:4;};int main(void){struct B b={0};b.v=src() orelse fb();return b.v==(@0@?@0@:9)&&gc==1&&fc==((!@0@)?1:0)?0:1;}",
	 0, 0},
	{"bitfield-rhs",
	 "struct B{unsigned v:4;};int main(void){struct B b={0};int x=0;b.v=src();x=b.v orelse fb();return x==(@0@?@0@:9)&&gc==1&&fc==((!@0@)?1:0)?0:1;}",
	 0, 0},
};
static const Axis ax_runtime_bare_orelse_lvalue = {
	"lvalue", runtime_bare_orelse_lvalue_values, N(runtime_bare_orelse_lvalue_values)
};

/* The declaration action matrix above exercises the same exits, but not the
 * bare-assignment lowering that owns temporary types, fallback chains, and
 * the action handoff.  Verify each action preserves exact defer unwinding. */
static const AxisValue runtime_bare_orelse_defer_values[] = {
	{"return",
	 "static int run(void){defer ev('A');{defer ev('a');ev('p');int x=0;x=src() orelse return 9;(void)x;ev('q');}ev('z');return 3;}"
	 "int main(void){int r=run();return @0@?(r==3&&same(\"pqazA\")?0:1):(r==9&&same(\"paA\")?0:2);}",
	 0, 0},
	{"goto",
	 "static int run(void){defer ev('A');{defer ev('a');ev('p');int x=0;x=src() orelse goto L;(void)x;ev('q');}ev('z');L:ev('l');return 3;}"
	 "int main(void){int r=run();return @0@?(r==3&&same(\"pqazlA\")?0:1):(r==3&&same(\"palA\")?0:2);}",
	 0, 0},
	{"break",
	 "static int run(void){defer ev('A');for(int i=0;i<1;i++){defer ev('a');ev('p');int x=0;x=src() orelse break;(void)x;ev('q');}ev('z');return 3;}"
	 "int main(void){int r=run();return r==3&&(@0@?same(\"pqazA\"):same(\"pazA\"))?0:1;}",
	 0, 0},
	{"continue",
	 "static int run(void){defer ev('A');for(int i=0;i<1;i++){defer ev('a');ev('p');int x=0;x=src() orelse continue;(void)x;ev('q');}ev('z');return 3;}"
	 "int main(void){int r=run();return r==3&&(@0@?same(\"pqazA\"):same(\"pazA\"))?0:1;}",
	 0, 0},
	{"block-return",
	 "static int run(void){defer ev('A');{defer ev('a');ev('p');int x=0;x=src() orelse {ev('f');return 9;};(void)x;ev('q');}ev('z');return 3;}"
	 "int main(void){int r=run();return @0@?(r==3&&same(\"pqazA\")?0:1):(r==9&&same(\"pfaA\")?0:2);}",
	 0, 0},
};
static const Axis ax_runtime_bare_orelse_defer = {
	"action", runtime_bare_orelse_defer_values, N(runtime_bare_orelse_defer_values)
};

/* A braceless defer can hold any complete control statement.  These shapes
 * make Phase 1 record through an else arm, a final brace, or a do-while tail
 * instead of stopping at the first semicolon in the nested statement. */
static const AxisValue runtime_defer_control_body_values[] = {
	{"if-else",
	 "static int calls;static int src(void){return @0@;}"
	 "static int run(void){defer if(src())calls++;else calls+=2;return 0;}"
	 "int main(void){run();return calls==(@0@?1:2)?0:1;}",
	 0, 0},
	{"if-else-braced",
	 "static int calls;static int src(void){return @0@;}"
	 "static int run(void){defer if(src()){calls++;}else{calls+=2;}return 0;}"
	 "int main(void){run();return calls==(@0@?1:2)?0:1;}",
	 0, 0},
	{"while-braced",
	 "static int calls,n;static int run(void){defer while(n--){calls++;}return 0;}"
	 "int main(void){n=@0@?1:2;run();return calls==(@0@?1:2)?0:1;}",
	 0, 0},
	{"for-braced",
	 "static int calls;static int src(void){return @0@;}"
	 "static int run(void){defer for(int i=0;i<(src()?1:2);i++){calls++;}return 0;}"
	 "int main(void){run();return calls==(@0@?1:2)?0:1;}",
	 0, 0},
	{"do-while",
	 "static int calls,probes;static int src(void){probes++;return @0@;}"
	 "static int run(void){defer do{calls++;}while(src()&&0);return 0;}"
	 "int main(void){run();return calls==1&&probes==1?0:1;}",
	 0, 0},
	{"switch",
	 "static int calls;static int src(void){return @0@;}"
	 "static int run(void){defer switch(src()){case 0:{calls+=2;break;}default:{calls++;break;}}return 0;}"
	 "int main(void){run();return calls==(@0@?1:2)?0:1;}",
	 0, 0},
};
static const Axis ax_runtime_defer_control_body = {
	"control", runtime_defer_control_body_values, N(runtime_defer_control_body_values)
};

/* `emit_deferred_range` has its own statement-dispatch path.  Exercise all
 * three orelse lowerings from inside a braced deferred body, on both truth
 * paths, and require a fixed point as well as execution. */
static const AxisValue runtime_deferred_orelse_body_values[] = {
	{"declaration",
	 "static int gc,fc;static int src(void){gc++;return @0@;}static int fb(void){fc++;return 9;}"
	 "static int run(void){int got=0;{defer {int value=src() orelse fb();got=value;}}return got;}"
	 "int main(void){int got=run();return got==(@0@?@0@:9)&&gc==1&&fc==(@0@?0:1)?0:1;}",
	 0, 0},
	{"bare-assignment",
	 "static int gc,fc;static int src(void){gc++;return @0@;}static int fb(void){fc++;return 9;}"
	 "static int run(void){int got=0;{defer {got=src() orelse fb();}}return got;}"
	 "int main(void){int got=run();return got==(@0@?@0@:9)&&gc==1&&fc==(@0@?0:1)?0:1;}",
	 0, 0},
	{"bracket-vla",
	 "static int run(int n){int got=0;{defer {int value[n orelse 3];got=(int)(sizeof(value)/sizeof(value[0]));}}return got;}"
	 "int main(void){return run(@0@)==(@0@?@0@:3)?0:1;}",
	 0, 0},
};
static const Axis ax_runtime_deferred_orelse_body = {
	"lowering", runtime_deferred_orelse_body_values, N(runtime_deferred_orelse_body_values)
};

static const AxisValue runtime_zero_values[] = {
	{"scalar", "int main(void){dirty();int v;return nz(&v,sizeof v);}", 0, 0},
	{"pointer", "int main(void){dirty();char *v;return nz(&v,sizeof v);}", 0, 0},
	{"long-double", "int main(void){dirty();long double v;return nz(&v,sizeof v);}", 0, 0},
	{"long-double-typedef", "typedef long double LD;int main(void){dirty();LD v;return nz(&v,sizeof v);}", 0, 0},
	{"long-double-typeof-typedef",
	 "typedef typeof((long double)0) LD;static volatile int poison;"
	 "__attribute__((noinline))static int check(void){LD v;volatile unsigned char*p=(void*)&v;"
	 "if(poison){for(unsigned long i=0;i<sizeof v;i++)p[i]=0xAA;return 0;}"
	 "for(unsigned long i=0;i<sizeof v;i++)if(p[i])return 1;return 0;}"
	 "int main(void){poison=1;check();poison=0;return check();}", 0, 0},
	{"long-double-typeof-typedef-chain",
	 "typedef typeof((long double)0) Base;typedef Base LD;static volatile int poison;"
	 "__attribute__((noinline))static int check(void){LD v;volatile unsigned char*p=(void*)&v;"
	 "if(poison){for(unsigned long i=0;i<sizeof v;i++)p[i]=0xAA;return 0;}"
	 "for(unsigned long i=0;i<sizeof v;i++)if(p[i])return 1;return 0;}"
	 "int main(void){poison=1;check();poison=0;return check();}", 0, 0},
	{"long-double-typeof-type-typedef",
	 "typedef typeof(long double) LD;static volatile int poison;"
	 "__attribute__((noinline))static int check(void){LD v;volatile unsigned char*p=(void*)&v;"
	 "if(poison){for(unsigned long i=0;i<sizeof v;i++)p[i]=0xAA;return 0;}"
	 "for(unsigned long i=0;i<sizeof v;i++)if(p[i])return 1;return 0;}"
	 "int main(void){poison=1;check();poison=0;return check();}", 0, 0},
	{"long-double-complex", "int main(void){dirty();_Complex long double v;return nz(&v,sizeof v);}", 0, 0},
	{"long-double-volatile", "int main(void){dirty();volatile long double v;return nz((const void*)&v,sizeof v);}", 0, 0},
	{"char-array", "int main(void){dirty();char v[13];return nz(&v,sizeof v);}", 0, 0},
	{"array-2d", "int main(void){dirty();int v[3][4];return nz(&v,sizeof v);}", 0, 0},
	{"struct-padding", "struct P{char a;int b;char c;};int main(void){dirty();struct P v;return nz(&v,sizeof v);}", 0, 0},
	{"struct-nested", "struct I{char a;long b;};struct S{char x;struct I i;char y;};int main(void){dirty();struct S v;return nz(&v,sizeof v);}", 0, 0},
	{"union", "union U{char s[7];int n;double d;};int main(void){dirty();union U v;return nz(&v,sizeof v);}", 0, 0},
	{"bitfield", "struct B{unsigned x:3;unsigned y:5;int tail;};int main(void){dirty();struct B v;return nz(&v,sizeof v);}", 0, 0},
	{"bitfield-straddle", "struct B{unsigned a:1;unsigned b:31;unsigned c:2;};int main(void){dirty();struct B v;return nz(&v,sizeof v);}", 0, 0},
	{"array-struct", "struct S{char a;int b;};int main(void){dirty();struct S v[3];return nz(&v,sizeof v);}", 0, 0},
	{"pointer-member", "struct S{char a;void*p;char b;};int main(void){dirty();struct S v;return nz(&v,sizeof v);}", 0, 0},
	{"struct-union", "struct S{char a;int b;};union U{struct S s;char c;};int main(void){dirty();union U v;return nz(&v,sizeof v);}", 0, 0},
	{"array-union", "union U{int n;char c[9];};int main(void){dirty();union U v[2];return nz(&v,sizeof v);}", 0, 0},
	{"anonymous-member", "struct S{char a;struct{int x;char y;};long z;};int main(void){dirty();struct S v;return nz(&v,sizeof v);}", 0, 0},
	{"enum-member", "enum E{E0,E1};struct S{char a;enum E e;char b;};int main(void){dirty();struct S v;return nz(&v,sizeof v);}", 0, 0},
	{"vla", "int main(int n,char**v){(void)v;dirty();n+=3;int a[n];return nz(&a,sizeof a);}", 0, 0},
	{"typeof-vla", "int main(int n,char**v){(void)v;dirty();n+=3;int a[n];typeof(a)b;return nz(&b,sizeof b);}", 0, 0},
};
static const Axis ax_runtime_zero = {"shape", runtime_zero_values, N(runtime_zero_values)};

/* The broad zero-init product above focuses on object representations.  These
 * axes cross the lowering strategies with every block form that owns a local
 * declaration, so a state-machine regression cannot hide behind a happy-path
 * function body.  All declarations use `v`, allowing the scope wrapper to
 * apply the same byte-level runtime oracle. */
static const AxisValue runtime_zero_fixed_values[] = {
	{"array", "int v[3][4];", 0, 0},
	{"struct", "struct { char a; int b; char c; } v;", 0, 0},
	{"union", "union { char c[7]; long n; double d; } v;", 0, 0},
	{"array-struct", "struct { char a; int b; } v[3];", 0, 0},
};
static const Axis ax_runtime_zero_fixed = {
	"shape", runtime_zero_fixed_values, N(runtime_zero_fixed_values)
};

static const AxisValue runtime_zero_scalar_values[] = {
	{"int", "int v;", 0, 0},
	{"pointer", "void *v;", 0, 0},
	{"volatile", "volatile int v;", 0, 0},
};
static const Axis ax_runtime_zero_scalar = {
	"shape", runtime_zero_scalar_values, N(runtime_zero_scalar_values)
};

static const AxisValue runtime_zero_vla_values[] = {
	{"vla", "int v[n][3];", 0, 0},
	{"typeof-vla", "typeof(int[n][3]) v;", 0, 0},
};
static const Axis ax_runtime_zero_vla = {
	"shape", runtime_zero_vla_values, N(runtime_zero_vla_values)
};

static const AxisValue runtime_zero_scope_values[] = {
	{"function", "int main(void){int n=3;dirty();@0@return nz(&v,sizeof v);}", 0, 0},
	{"nested", "int main(void){int n=3;dirty();{@0@return nz(&v,sizeof v);}}", 0, 0},
	{"if-body", "int main(void){int n=3;if(1){dirty();@0@return nz(&v,sizeof v);}return 1;}", 0, 0},
	{"while-body", "int main(void){int n=3;int once=1;while(once--){dirty();@0@return nz(&v,sizeof v);}return 1;}", 0, 0},
	{"for-body", "int main(void){int n=3;for(int once=0;once==0;once++){dirty();@0@return nz(&v,sizeof v);}return 1;}", 0, 0},
	{"switch-case", "int main(void){int n=3;switch(0){case 0:{dirty();@0@return nz(&v,sizeof v);}default:return 1;}}", 0, 0},
	{"stmt-expr", "int main(void){int n=3;return ({dirty();@0@nz(&v,sizeof v);});}", 0, 0},
};
static const Axis ax_runtime_zero_scope = {
	"scope", runtime_zero_scope_values, N(runtime_zero_scope_values)
};

/* Declarations in a control initializer have no preceding statement where a
 * delayed memset could be inserted.  Cross scalar and array declarators here
 * so the recipe must choose initializer lowering, including typedef and
 * parenthesized-array classification. */
static const AxisValue runtime_zero_control_init_values[] = {
	{"scalar",
	 "int main(void){int pass=0;for(int v;pass++==0;){if(v)return 1;}return 0;}", 0, 0},
	{"array",
	 "int main(void){int pass=0;for(int v[3];pass++==0;){for(int i=0;i<3;i++)if(v[i])return 1;}return 0;}",
	 0, 0},
	{"parenthesized-array",
	 "int main(void){int pass=0;for(int (v)[3];pass++==0;){for(int i=0;i<3;i++)if(v[i])return 1;}return 0;}",
	 0, 0},
	{"typedef-array",
	 "typedef int Row[3];int main(void){int pass=0;for(Row v;pass++==0;){for(int i=0;i<3;i++)if(v[i])return 1;}return 0;}",
	 0, 0},
};
static const Axis ax_runtime_zero_control_init = {
	"declarator", runtime_zero_control_init_values, N(runtime_zero_control_init_values)
};

static const AxisValue defer_ident_next_op_values[] = {
	{"lt", "<", 0, 0}, {"gt", ">", 0, 0}, {"mod", "%", 0, 0},
	{"xor", "^", 0, 0}, {"or", "|", 0, 0}, {"div", "/", 0, 0},
	{"le", "<=", 0, 0}, {"shl", "<<", 0, 0}, {"ge", ">=", 0, 0},
	{"shr", ">>", 0, 0}, {"eq", "==", 0, 0}, {"ne", "!=", 0, 0},
	{"and", "&&", 0, 0}, {"lor", "||", 0, 0},
};
static const Axis ax_defer_ident_next_op = {
	"operator", defer_ident_next_op_values, N(defer_ident_next_op_values)
};

static const AxisValue defer_ident_prev_op_values[] = {
	{"plus", "+", 0, 0}, {"minus", "-", 0, 0}, {"div", "/", 0, 0},
	{"mod", "%", 0, 0}, {"or", "|", 0, 0}, {"xor", "^", 0, 0},
	{"lt", "<", 0, 0}, {"gt", ">", 0, 0}, {"shl", "<<", 0, 0},
	{"shr", ">>", 0, 0}, {"eq", "==", 0, 0}, {"ne", "!=", 0, 0},
	{"and", "&&", 0, 0}, {"lor", "||", 0, 0},
};
static const Axis ax_defer_ident_prev_op = {
	"operator", defer_ident_prev_op_values, N(defer_ident_prev_op_values)
};

static const AxisValue defer_ident_shape_values[] = {
	{"scalar", "int f(void){int defer=2;return defer;}", 0, 0},
	{"parameter", "int f(int defer){return defer;}", 0, 0},
	{"postfix", "int f(void){int defer=0;defer++;defer--;return defer;}", 0, 0},
	{"subscript", "int f(void){int defer[2]={1,2};return defer[1];}", 0, 0},
	{"member", "struct S{int defer;};int f(struct S s){return s.defer;}", 0, 0},
	{"pointer-member", "struct S{int defer;};int f(struct S*p){return p->defer;}", 0, 0},
	{"label", "int f(void){goto defer;defer:return 0;}", 0, 0},
	{"function-empty-stmt", "int defer(void);void f(void){defer();}", 0, 0},
	{"function-arg-stmt", "void defer(int);void f(void){defer(1);}", 0, 0},
	{"function-pointer-param", "void f(void(*defer)(int)){defer(1);}", 0, 0},
	{"function-pointer-typedef-param", "typedef void(*F)(int);void f(F defer){defer(1);}", 0, 0},
	{"function-pointer-local", "void g(int);void f(void){void(*defer)(int)=g;defer(1);}", 0, 0},
	{"function-pointer-knr-param", "void f(defer)void(*defer)(int);{defer(1);}", 0, 0},
};
static const Axis ax_defer_ident_shape = {
	"shape", defer_ident_shape_values, N(defer_ident_shape_values)
};

static const AxisValue orelse_ident_shape_values[] = {
	{"scalar", "int f(void){int orelse=2;return orelse;}", 0, 0},
	{"parameter", "int f(int orelse){return orelse;}", 0, 0},
	{"postfix", "int f(void){int orelse=0;orelse++;orelse--;return orelse;}", 0, 0},
	{"subscript", "int f(void){int orelse[2]={1,2};return orelse[1];}", 0, 0},
	{"member", "struct S{int orelse;};int f(struct S s){return s.orelse;}", 0, 0},
	{"pointer-member", "struct S{int orelse;};int f(struct S*p){return p->orelse;}", 0, 0},
	{"label", "int f(void){goto orelse;orelse:return 0;}", 0, 0},
	{"function-empty-stmt", "int orelse(void);void f(void){orelse();}", 0, 0},
	{"function-arg-stmt", "void orelse(int);void f(void){orelse(1);}", 0, 0},
	{"function-pointer-param", "void f(void(*orelse)(int)){orelse(1);}", 0, 0},
	{"function-pointer-typedef-param", "typedef void(*F)(int);void f(F orelse){orelse(1);}", 0, 0},
	{"function-pointer-local", "void g(int);void f(void){void(*orelse)(int)=g;orelse(1);}", 0, 0},
	{"function-pointer-knr-param", "void f(orelse)void(*orelse)(int);{orelse(1);}", 0, 0},
};
static const Axis ax_orelse_ident_shape = {
	"shape", orelse_ident_shape_values, N(orelse_ident_shape_values)
};

static const AxisValue runtime_bounds_oob_values[] = {
	{"read", "return a[i];", 0, 0},
	{"write", "a[i]=1;return 0;", 0, 0},
	{"reverse", "return i[a];", 0, 0, O_REJECT | O_DIAG},
	{"paren", "return (a)[i];", 0, 0},
	{"redundant-paren", "return ((a))[i];", 0, 0},
	{"cast", "return (((int *)a))[i];", 0, 0},
	{"cancel-address", "return *&a[i];", 0, 0},
	{"cancel-address-paren", "return *(&a[i]);", 0, 0},
	{"row", "return m[i][0];", 0, 0},
	{"column", "return m[0][i];", 0, 0},
	{"orelse-index", "return a[0 orelse i];", 0, 0},
	{"stmt-expr", "return ({a[i];});", 0, 0},
	{"binary-and-postfix", "int x=1;return x++ & a[i];", 0, 0},
	{"binary-and-compound", "return ((int){1}) & a[i];", 0, 0},
	{"binary-and-paren", "int x=1;return (x) & a[i];", 0, 0},
};
static const Axis ax_runtime_bounds_oob = {
	"access", runtime_bounds_oob_values, N(runtime_bounds_oob_values)
};

static const AxisValue runtime_bounds_ok_values[] = {
	{"2d", "int main(void){int m[2][3]={{1,2,3},{4,5,6}};return m[1][2]==6?0:1;}", 0, 0},
	{"vla", "int main(int n,char**v){(void)v;n+=2;int a[n];for(int i=0;i<n;i++)a[i]=i;return a[n-1]==n-1?0:1;}", 0, 0},
	{"tracked-pointer", "int main(void){int a[4]={1,2,3,4};int*p=a;return p[2]==3?0:1;}", 0, 0},
	{"untracked-pointer", "int main(void){int a[4]={1,2,3,4};int*p=a;volatile int i=3;return p[i]==4?0:1;}", 0, 0},
	{"defer-index", "int main(void){int a[5]={0};int i=2;defer{(void)a[i];}return 0;}", 0, 0},
	{"orelse-action-index", "int main(void){int a[5]={0};int i=1;int*p=0;p=p orelse{(void)a[i];return 0;};return 1;}", 0, 0},
	{"orelse-in-index", "int main(void){int a[4]={1,2,3,4};int z=0;return a[z orelse 2]==3?0:1;}", 0, 0},
	{"stmt-expr-index", "int main(void){int a[5]={9,8,7,6,5};int i=0;int v=({a[i];});return v==9?0:1;}", 0, 0},
	{"one-past-deep-paren", "int main(void){int a[4];volatile int i=4;return &((a[i]))==a+4?0:1;}", 0, 0},
	{"one-past-if-body", "int main(void){int a[4];volatile int i=4;if(1)&a[i];return 0;}", 0, 0},
	{"one-past-while-body", "int main(void){int a[4];volatile int i=4;while(0)&a[i];return &a[i]==a+4?0:1;}", 0, 0},
	{"one-past-switch-body", "int main(void){int a[4];volatile int i=4;switch(0){default:&a[i];}return 0;}", 0, 0},
};
static const Axis ax_runtime_bounds_ok = {
	"access", runtime_bounds_ok_values, N(runtime_bounds_ok_values)
};

/* Runtime trap cells must reach every tracked binding kind, not merely the
 * ordinary automatic local in runtime_bounds_oob_values.  Keep the indexed
 * expression and its spelling independent: the odometer then tests each
 * parser path (plain, parens, address cancellation, orelse, stmt-expr) for
 * automatic, block-static, file-static and typedef-hidden arrays. */
static const AxisValue runtime_bounds_base_values[] = {
	{"local", "a", 0, 0},
	{"block-static", "s", 0, 0},
	{"file-static", "g", 0, 0},
	{"typedef", "r", 0, 0},
};
static const Axis ax_runtime_bounds_base = {
	"binding", runtime_bounds_base_values, N(runtime_bounds_base_values)
};

static const AxisValue runtime_bounds_access_values[] = {
	{"read", "return @0@[idx];", 0, 0},
	{"write", "@0@[idx]=1;return 0;", 0, 0},
	{"paren", "return (@0@)[idx];", 0, 0},
	{"cancel-address", "return *&@0@[idx];", 0, 0},
	{"orelse-index", "return @0@[0 orelse idx];", 0, 0},
	{"stmt-expr", "return ({@0@[idx];});", 0, 0},
};
static const Axis ax_runtime_bounds_access = {
	"access", runtime_bounds_access_values, N(runtime_bounds_access_values)
};

static const AxisValue runtime_bounds_matrix_base_values[] = {
	{"local", "m", 0, 0},
	{"block-static", "sm", 0, 0},
	{"file-static", "gm", 0, 0},
	{"typedef", "tm", 0, 0},
};
static const Axis ax_runtime_bounds_matrix_base = {
	"binding", runtime_bounds_matrix_base_values, N(runtime_bounds_matrix_base_values)
};

static const AxisValue runtime_bounds_matrix_access_values[] = {
	{"row-read", "return @0@[idx][0];", 0, 0},
	{"column-read", "return @0@[0][idx];", 0, 0},
	{"row-write", "@0@[idx][0]=1;return 0;", 0, 0},
	{"column-write", "@0@[0][idx]=1;return 0;", 0, 0},
	{"row-orelse", "return @0@[0 orelse idx][0];", 0, 0},
	{"column-orelse", "return @0@[0][0 orelse idx];", 0, 0},
	{"cancel-address", "return *&@0@[0][idx];", 0, 0},
};
static const Axis ax_runtime_bounds_matrix_access = {
	"access", runtime_bounds_matrix_access_values, N(runtime_bounds_matrix_access_values)
};

/* Type-derived arrays take a different declaration/binding path from an
 * ordinary or typedef-spelled array.  Keep both VLA ranks live: zero-init
 * must choose memset while bounds emits the matching dynamic sizeof ratio. */
static const AxisValue runtime_bounds_typeof_vla_values[] = {
	{"outer", "return a[i][0];", 0, 0},
	{"inner", "return a[0][i];", 0, 0},
};
static const Axis ax_runtime_bounds_typeof_vla = {
	"rank", runtime_bounds_typeof_vla_values, N(runtime_bounds_typeof_vla_values)
};

static const AxisValue runtime_autostatic_values[] = {
	{"flat", "int main(void){const int a[4]={1,2,3,4};return a[0]+a[3]==5?0:1;}", 0, 0},
	{"nested", "int main(void){const int a[2][3]={{1,2,3},{4,5,6}};return a[0][2]+a[1][0]==7?0:1;}", 0, 0},
	{"designated", "int main(void){const int a[6]={[1]=7,[4]=9};return a[0]==0&&a[1]==7&&a[4]==9?0:1;}", 0, 0},
	{"partial", "int main(void){const int a[5]={1,2};return a[0]==1&&a[1]==2&&a[4]==0?0:1;}", 0, 0},
	{"string", "int main(void){const char a[]=\"abc\";return a[0]=='a'&&a[3]==0?0:1;}", 0, 0},
	{"enum", "enum{A=3,B=5};int main(void){const int a[2]={A,B};return a[0]+a[1]==8?0:1;}", 0, 0},
	{"recursive", "static int f(int n){const int a[4]={1,2,3,4};int s=a[0]+a[1]+a[2]+a[3];return n?s+f(n-1):s;}int main(void){return f(3)==40?0:1;}", 0, 0},
	{"struct", "struct S{int x,y;};int main(void){const struct S a[2]={{1,2},{3,4}};return a[0].x+a[1].y==5?0:1;}", 0, 0},
	{"typedef", "typedef const int CI;int main(void){CI a[4]={2,4,6,8};return a[0]+a[3]==10?0:1;}", 0, 0},
	{"loop", "int main(void){int s=0;for(int q=0;q<3;q++){const int a[3]={1,2,3};s+=a[q];}return s==6?0:1;}", 0, 0},
};
static const Axis ax_runtime_autostatic = {
	"shape", runtime_autostatic_values, N(runtime_autostatic_values)
};

#define AF_BLOCK_1 "{int x;}"
#define AF_BLOCK_2 AF_BLOCK_1 AF_BLOCK_1
#define AF_BLOCK_4 AF_BLOCK_2 AF_BLOCK_2
#define AF_BLOCK_8 AF_BLOCK_4 AF_BLOCK_4
#define AF_BLOCK_16 AF_BLOCK_8 AF_BLOCK_8
#define AF_BLOCK_32 AF_BLOCK_16 AF_BLOCK_16
#define AF_BLOCK_64 AF_BLOCK_32 AF_BLOCK_32
#define AF_BLOCK_128 AF_BLOCK_64 AF_BLOCK_64
#define AF_BLOCK_256 AF_BLOCK_128 AF_BLOCK_128
#define AF_BLOCK_512 AF_BLOCK_256 AF_BLOCK_256
#define AF_BLOCK_1024 AF_BLOCK_512 AF_BLOCK_512
static const char alloc_fault_large_source[] = "void f(void){" AF_BLOCK_1024 "}";
#undef AF_BLOCK_1024
#undef AF_BLOCK_512
#undef AF_BLOCK_256
#undef AF_BLOCK_128
#undef AF_BLOCK_64
#undef AF_BLOCK_32
#undef AF_BLOCK_16
#undef AF_BLOCK_8
#undef AF_BLOCK_4
#undef AF_BLOCK_2
#undef AF_BLOCK_1

#define AF_IF_1 "#if 1\n"
#define AF_IF_2 AF_IF_1 AF_IF_1
#define AF_IF_4 AF_IF_2 AF_IF_2
#define AF_IF_8 AF_IF_4 AF_IF_4
#define AF_IF_16 AF_IF_8 AF_IF_8
#define AF_IF_32 AF_IF_16 AF_IF_16
#define AF_ENDIF_1 "#endif\n"
#define AF_ENDIF_2 AF_ENDIF_1 AF_ENDIF_1
#define AF_ENDIF_4 AF_ENDIF_2 AF_ENDIF_2
#define AF_ENDIF_8 AF_ENDIF_4 AF_ENDIF_4
#define AF_ENDIF_16 AF_ENDIF_8 AF_ENDIF_8
#define AF_ENDIF_32 AF_ENDIF_16 AF_ENDIF_16
static const char alloc_fault_cond_growth_source[] =
	AF_IF_32 AF_IF_2 "#define V 7\n" AF_ENDIF_2 AF_ENDIF_32
	"int f(void){return V;}\n";
#undef AF_ENDIF_32
#undef AF_ENDIF_16
#undef AF_ENDIF_8
#undef AF_ENDIF_4
#undef AF_ENDIF_2
#undef AF_ENDIF_1
#undef AF_IF_32
#undef AF_IF_16
#undef AF_IF_8
#undef AF_IF_4
#undef AF_IF_2
#undef AF_IF_1

#define AF_TEXT_64 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define AF_TEXT_128 AF_TEXT_64 AF_TEXT_64
#define AF_TEXT_256 AF_TEXT_128 AF_TEXT_128
#define AF_TEXT_512 AF_TEXT_256 AF_TEXT_256
static const char alloc_fault_long_define_source[] =
	"#define V \"" AF_TEXT_256 "\" \\\n\"" AF_TEXT_512 "\"\n"
	"const char *f(void){return V;}\n";
#undef AF_TEXT_512
#undef AF_TEXT_256
#undef AF_TEXT_128
#undef AF_TEXT_64

static const char alloc_fault_taint_graph_source[] =
	"void exit(int);"
	"void leaf(void){exit(1);}"
	"void middle(void){leaf();}"
	"void top(void){middle();}";

static const AxisValue alloc_fault_values[] = {
	{"basic", "int f(void){int x;return x;}", 0, 0},
	{"types",
	 "typedef typeof((long double)0) LD;typedef int*volatile VP;"
	 "void f(const int source,VP p){typeof(source)x;typeof_unqual(source)y;LD z;"
	 "(void)x;(void)y;(void)z;(void)p;}",
	 0, 0},
	{"control",
	 "int g(void);void c(int);int f(int n){int a[4];defer c(n);int x=g() orelse 3;"
	 "if(n)return a[n&3]+x;return x;}",
	 0, 0},
	{"source-defines", "#define V 7\n#if V\nint f(void){int x;return x+V;}\n#endif\n", 0, FB_FLAT},
	{"line-markers", "# 42 \"virtual-input.c\"\nint f(void){int x;return x;}\n", 0, 0},
	{"taint-graph", alloc_fault_taint_graph_source, 0, 0},
	{"token-growth", alloc_fault_large_source, 0, 0},
};
static const Axis ax_alloc_fault = {"source", alloc_fault_values, N(alloc_fault_values)};

static const AxisValue alloc_fault_reject_values[] = {
	{"syntax", "void f(void){if(1){", 0, 0},
	{"defer-control", "void f(void){defer return;}", 0, 0},
	{"goto-vla", "void f(int n){goto L;int a[n];L:(void)a;}", 0, 0},
	{"const-atomic", "void f(void){_Atomic int source;const typeof(source) copy;(void)copy;}", 0, 0},
};
static const Axis ax_alloc_fault_reject = {
	"source", alloc_fault_reject_values, N(alloc_fault_reject_values)
};

static const AxisValue alloc_fault_file_values[] = {
	{"basic", "int f(void){int x;return x;}", 0, 0},
	{"defines", "#define V 7\nint f(void){int x;return x+V;}\n", 0, FB_FLAT},
	{"conditional",
	 "#if 1\n#define V 7\n#elif 0\n#define V 8\n#else\n#define V 9\n#endif\n"
	 "int f(void){return V;}\n",
	 0, FB_FLAT},
	{"define-growth",
	 "#define A0 0\n#define A1 1\n#define A2 2\n#define A3 3\n#define A4 4\n#define A5 5\n"
	 "#define A6 6\n#define A7 7\n#define A8 8\n#define A9 9\n#define A10 10\n#define A11 11\n"
	 "int f(void){return A0+A5+A11;}\n",
	 0, FB_FLAT},
	{"continued-growth", alloc_fault_long_define_source, 0, FB_FLAT},
	{"comment-splice", "#define V 1 /* open\n*/ + 2\nint f(void){return V;}\n", 0, FB_FLAT},
	{"conditional-growth", alloc_fault_cond_growth_source, 0, FB_FLAT},
	{"taint-graph", alloc_fault_taint_graph_source, 0, 0},
	{.tag="api-include", .text="int f(void){return 0;}", .api_field=AF_INCLUDE,
	 .api_values=api_include_ok, .api_count=N(api_include_ok)},
	{.tag="api-defines", .text="#ifdef RECIPE_DEF\nint v=RECIPE_DEF;\n#endif\n",
	 .clear_features=FB_FLAT, .api_field=AF_DEFINE, .api_values=api_define_ok,
	 .api_count=N(api_define_ok)},
	{.tag="api-cflags", .text="int f(void){return RECIPE_FLAG;}", .api_field=AF_CFLAG,
	 .api_values=api_cflag_ok, .api_count=N(api_cflag_ok)},
	{.tag="api-force-include", .text="int f(void){return 0;}", .api_field=AF_FORCE,
	 .api_values=api_force_ok, .api_count=N(api_force_ok)},
	{.tag="api-compiler", .text="int f(void){return 0;}", .api_field=AF_COMPILER,
	 .api_compiler="cc -std=gnu11"},
};
static const Axis ax_alloc_fault_file = {"source", alloc_fault_file_values, N(alloc_fault_file_values)};


/* A defer-bearing `return` needs a temporary of the function's return type, so
 * a complex return declarator has to be re-synthesized as a typedef. Suffixes
 * that follow the declarator's outer `)` belong to the returned type: `[N]` for
 * a returned array pointer and `(params)` for a returned function pointer. The
 * parameter list used to be dropped, so `int (*f(void))(int)` produced
 * `typedef int (*T);` and the initializer and return became incompatible-pointer
 * errors on GCC 16 / Clang 22 (warnings on older compilers, hence O_COMPILE
 * alone is not the whole oracle -- see the exact/ fragments below). */
static const AxisValue defer_return_declarator_values[] = {
	{"plain", "int f(void){defer (void)0;return 0;}", 0, 0},
	{"pointer", "int *f(void){defer (void)0;return 0;}", 0, 0},
	{"pointer2", "int **f(void){defer (void)0;return 0;}", 0, 0},
	{"array-ptr", "static int a[4];int (*f(void))[4]{defer (void)0;return &a;}", 0, 0},
	{"array-ptr-2d", "static int a[4][3];int (*f(void))[4][3]{defer (void)0;return &a;}", 0, 0},
	{"array-ptr-ptr", "static int (*p)[4];int (**f(void))[4]{defer (void)0;return &p;}", 0, 0},
	{"fnptr", "static int g(int a){return a;}int (*f(void))(int){defer (void)0;return g;}", 0, 0},
	{"fnptr-void", "static int g(void){return 0;}int (*f(void))(void){defer (void)0;return g;}", 0, 0},
	{"fnptr-two-params",
	 "static int g(int a,char *b){(void)b;return a;}int (*f(void))(int,char *){defer (void)0;return g;}",
	 0, 0},
	{"fnptr-ptr", "static int (*p)(int);int (**f(void))(int){defer (void)0;return &p;}", 0, 0},
	{"fnptr-returning-ptr",
	 "static int *g(int a){(void)a;return 0;}int *(*f(void))(int){defer (void)0;return g;}", 0, 0},
	{"fnptr-of-fnptr",
	 "static int i2(int a){return a;}static int (*g(char c))(int){(void)c;return i2;}"
	 "int (*(*f(void))(char))(int){defer (void)0;return g;}",
	 0, 0},
	{"array-of-fnptr",
	 "static int (*t[4])(int);int (*(*f(void))[4])(int){defer (void)0;return &t;}", 0, 0},
	{"fnptr-with-fnptr-param",
	 "static int g(int (*h)(int)){return h?1:0;}"
	 "int (*f(void))(int (*)(int)){defer (void)0;return g;}",
	 0, 0},
	{"struct-value", "struct S{int a;};static struct S s;struct S f(void){defer (void)0;return s;}", 0, 0},
	{"typedef-value", "struct S{int a;};typedef struct S ST;static ST s;ST f(void){defer (void)0;return s;}", 0, 0},
	{"qualified-pointer", "const char *f(void){defer (void)0;return \"x\";}", 0, 0},
};
static const Axis ax_defer_return_declarator = {
	"declarator", defer_return_declarator_values, N(defer_return_declarator_values)
};

/* Synthetic output -- a spliced `{`, a copied defer body -- does not pass
 * through emit_tok, so it never picks up emit_tok's beginning-of-line newline.
 * Landing it on a surviving `#pragma` line puts the statement inside the
 * directive, and a conforming preprocessor discards trailing tokens there: the
 * cleanup, and in the return case the `return` itself, vanished with no
 * diagnostic. Line directives are what normally forced the break, so every cell
 * here must run with FB_LINE cleared. Each program's exit status is the
 * oracle: a dropped splice changes it. */
static const AxisValue defer_after_directive_values[] = {
	{"scope-exit",
	 "static int calls;static void mark(void){calls++;}"
	 "static void f(void){defer mark();\n#pragma pack(1)\n}"
	 "int main(void){f();return calls==1?0:1;}",
	 0, 0},
	{"return",
	 "static int calls;static void mark(void){calls++;}"
	 "static int f(void){defer mark();\n#pragma pack(1)\nreturn 7;}"
	 "int main(void){int v=f();return (v==7&&calls==1)?0:1;}",
	 0, 0},
	{"break",
	 "static int calls;static void mark(void){calls++;}"
	 "static void f(void){for(int i=0;i<1;i++){defer mark();\n#pragma pack(1)\nbreak;}}"
	 "int main(void){f();return calls==1?0:1;}",
	 0, 0},
	{"continue",
	 "static int calls;static void mark(void){calls++;}"
	 "static void f(void){for(int i=0;i<2;i++){defer mark();\n#pragma pack(1)\ncontinue;}}"
	 "int main(void){f();return calls==2?0:1;}",
	 0, 0},
	{"goto",
	 "static int calls;static void mark(void){calls++;}"
	 "static void f(void){{defer mark();\n#pragma pack(1)\ngoto out;}out:;}"
	 "int main(void){f();return calls==1?0:1;}",
	 0, 0},
};
static const Axis ax_defer_after_directive = {
	"exit", defer_after_directive_values, N(defer_after_directive_values)
};

#include "test.recipes.inc"

/* ---- Recipes -------------------------------------------------------- */

#define PRE "static volatile int z;static int g(void){return z;}static int h(void){return z;}\n"

static const char *const av_help[] = {"prism", "--help"};
static const char *const av_version[] = {"prism", "--version"};
static const char *const av_run[] = {"prism", "run", "x.c", "--", "a", "b"};
static const char *const av_emit[] = {"prism", "--prism-emit", "x.c"};
static const char *const av_emit_file[] = {"prism", "--prism-emit=o.c", "x.c"};
static const char *const av_check[] = {"prism", "check", "tool", "x.c"};
static const char *const av_flags[] = {"prism", "-fno-defer", "-fno-orelse", "-fno-zeroinit", "x.c"};
static const char *const av_bounds[] = {"prism", "-fbounds-check", "x.c"};
static const char *const av_compile[] = {"prism", "-c", "x.c", "-o", "x.o"};
static const char *const av_assemble[] = {"prism", "-S", "x.c", "-o", "x.s"};
static const char *const av_verbose[] = {"prism", "--prism-verbose", "x.c"};
static const char *const av_verify[] = {"prism", "--prism-verify", "x.c"};
static const char *const av_rsp[] = {"prism", "@rsp@"};
static const char *const av_install[] = {"prism", "install"};
static const char *const av_dash[] = {"prism", "-"};
static const char *const av_i[] = {"prism", "x.i"};
static const char *const av_msvc[] = {"prism", "/c", "/Fe:app", "/Foobj.o", "x.c"};
static const char *const av_msvc_sep[] = {"prism", "/Fe", "app", "/Fo", "obj.o", "x.c"};
static const char *const av_pass_e[] = {"prism", "-E", "x.c"};
static const char *const av_pass_m[] = {"prism", "-M", "x.c"};
static const char *const av_pass_mm[] = {"prism", "-MM", "x.c"};
static const char *const av_cc[] = {"prism", "--prism-cc=clang", "x.c"};
static const char *const av_prof[] = {"prism", "--prism-prof", "x.c"};
static const char *const av_short_help[] = {"prism", "-h"};
static const char *const av_emit_joined[] = {"prism", "--prism-emit=out.c", "x.c"};
static const char *const av_output_joined[] = {"prism", "-oout", "x.c"};
static const char *const av_features_on[] = {"prism", "-fdefer", "-fzeroinit", "-forelse", "-fline-directives", "-fflatten-headers", "-fauto-unreachable", "-fauto-static", "-fbounds-check", "x.c"};
static const char *const av_features_more[] = {"prism", "-fno-safety", "-fno-link-pragma", "x.c"};
static const char *const av_dep[] = {"prism", "-MD", "-MF", "x.d", "x.c"};
static const char *const av_x_c[] = {"prism", "-x", "c", "source"};
static const char *const av_x_header[] = {"prism", "-xc-header", "source"};
static const char *const av_x_cpp[] = {"prism", "-xcpp-output", "source"};
static const char *const av_x_none[] = {"prism", "-x", "none", "source"};
static const char *const av_arg_flags[] = {"prism", "-I", ".", "-include", "/dev/null", "x.c"};
static const char *const av_run_sep[] = {"prism", "run", "x.c", "--", "--flag"};

static const Recipe recipes[] = {
	{"internal/platform", NULL, NULL, {0}, O_INTERNAL, 0, 0, CAP_POSIX,
	 NULL, NULL, NULL, 0, "KCSPODAFJTRMNYZWGILBH"},
	{"internal/system-header-ordering", NULL, NULL, {0}, O_INTERNAL, 0, 0, CAP_POSIX,
	 NULL, NULL, NULL, 0, "b"},
	{"internal/api-reset", NULL, NULL, {0}, O_INTERNAL, 0, 0, 0, NULL, NULL, NULL, 0, "Q"},
	{"internal/api-validation", NULL, NULL, {0}, O_INTERNAL, 0, 0, CAP_POSIX, NULL, NULL, NULL, 0, "V"},
	{"internal/api-first-oom", NULL, NULL, {0}, O_INTERNAL, 0, 0, 0, NULL, NULL, NULL, 0, "E"},
	{"internal/clean-environ", NULL, NULL, {0}, O_INTERNAL, 0, 0, CAP_POSIX, NULL, NULL, NULL, 0, "H"},
	{"internal/windows-unicode-environ", NULL, NULL, {0}, O_INTERNAL, 0, 0, CAP_WINDOWS,
	 NULL, NULL, NULL, 0, "U"},
	{"fault/alloc-source-api", "@0@", NULL, {&ax_alloc_fault}, O_ALLOC_FAIL, 0, 0, 0},
	{"fault/alloc-source-reject", "@0@", NULL, {&ax_alloc_fault_reject}, O_ALLOC_FAIL | O_REJECT,
	 0, 0, 0},
	{"fault/alloc-file-api", "@0@", NULL, {&ax_alloc_fault_file}, O_ALLOC_FAIL | O_FILE,
	 0, 0, CAP_POSIX},
	{"api/msvc-first-call-prologue", "int f(void){int a[1];return a[0];}", NULL,
	 {&ax_msvc_target}, O_OK, 0, FB_LINE, 0,
	 "#pragma warning(push, 0)|#pragma warning(pop)", "#pragma GCC diagnostic"},
	{"contexts/expression", "@2@", PRE, {&ax_expr, &ax_expr_wrap, &ax_expr_ctx, &ax_features}, O_TRICHOTOMY, 0, FB_LINE, 0},
	{"contexts/statement", "@1@", NULL, {&ax_stmt, &ax_stmt_ctx, &ax_features}, O_TRICHOTOMY, 0, FB_LINE, 0},
	{"declarations/product", "@1@", NULL, {&ax_decl, &ax_decl_ctx, &ax_features}, O_OK | O_FIXED, 0, FB_LINE, 0},
	{"features/product", "int g(void);void c(void);int f(int i){int a[8];const int table[2]={1,2};defer c();int x=g() orelse 2;raw int y;return a[i]+table[0]+x+y;}",
	 NULL, {&ax_features}, O_TRICHOTOMY, FB_BOUNDS | FB_AS | FB_AUR, FB_LINE, 0},
	{"features/power-set/top", "@0@", NULL,
	 {&ax_feature_matrix_top, &ax_feature_matrix_emission}, O_FEATURE_MATRIX, 0, 0, 0},
	{"features/power-set/block", "@0@", NULL,
	 {&ax_feature_matrix_block, &ax_feature_matrix_emission}, O_FEATURE_MATRIX, 0, 0, 0},
	{"features/power-set/if", "@0@", NULL,
	 {&ax_feature_matrix_if, &ax_feature_matrix_emission}, O_FEATURE_MATRIX, 0, 0, 0},
	{"features/power-set/switch", "@0@", NULL,
	 {&ax_feature_matrix_switch, &ax_feature_matrix_emission}, O_FEATURE_MATRIX, 0, 0, 0},
	{"api/feature-struct", "#ifdef RECIPE_DEF\nint v=RECIPE_DEF;\n#else\nint v;\n#endif\n",
	 NULL, {&ax_api_features}, O_FILE | O_ANY_STATUS, 0, FB_LINE, CAP_POSIX},
	{"api/define-last-wins", "@0@int f(void){return RECIPE_ORDER;}", NULL,
	 {&ax_api_order}, O_FILE | O_OK, 0, FB_FLAT | FB_LINE, CAP_POSIX,
	 "#define RECIPE_ORDER 2", "#define RECIPE_ORDER 1"},
	{"files/product", "@0@", NULL, {&ax_files, &ax_features}, O_FILE | O_TRICHOTOMY,
	 0, FB_LINE, CAP_POSIX},
	{"driver/transpile", "@0@", NULL, {&ax_driver, &ax_features},
	 O_DRIVER_TRANSPILE, 0, FB_LINE, CAP_POSIX},
	{"driver/verify", "@0@", NULL, {&ax_driver},
	 O_DRIVER_TRANSPILE | O_VERIFY, 0, FB_LINE | FB_ZERO, CAP_POSIX},
	{"driver/paths", "@0@", NULL, {&ax_paths},
	 O_DRIVER_TRANSPILE | O_INPUT_PATH, 0, FB_LINE, CAP_POSIX},
	{"driver/function-macro-preserve",
	 "#define SUM(a,b) ((a)+(b))\n#include <stddef.h>\nint f(void){return SUM(2,3);}\n", NULL, {0},
	 O_DRIVER_TRANSPILE | O_COMPILE, 0, FB_FLAT | FB_LINE, CAP_POSIX,
	 "return ((2)+(3))", "#ifndef SUM"},
	{"runtime/passthrough-equivalence", "@0@", NULL, {&ax_passthrough},
	 O_OK | O_OUTPUT_EQ_OFF | O_REFERENCE_RUN, 0, FB_LINE, CAP_POSIX},
	{"runtime/spliced-extension-keywords", "@0@", NULL, {&ax_runtime_spliced_keyword},
	 O_OK | O_RUN | O_FIXED | O_NO_EXT, 0, 0, CAP_POSIX},
	{"runtime/spliced-line-marker",
	 "static int calls;static void mark(void){calls++;}int main(void){de\\\nfer mark();return calls==0?0:1;}",
	 NULL, {0}, O_OK | O_RUN | O_FIXED | O_NO_EXT, 0, 0, CAP_POSIX,
	 "#line 2 \"recipe.c\""},
	{"corpus/retired-regressions", "@0@", NULL, {&ax_corpus},
	 O_ANY_STATUS | O_REPLAY, 0, FB_LINE, 0},
	{"bounds/product", "@1@", NULL, {&ax_bounds, &ax_bounds_ctx}, O_TRICHOTOMY, FB_BOUNDS, FB_LINE, 0},
	{"safety/bounds-strict", "@0@", NULL, {&ax_safety_bounds_hazard},
	 O_REJECT | O_DIAG, FB_BOUNDS, FB_LINE, 0},
	{"safety/bounds-warning", "@0@", NULL, {&ax_safety_bounds_hazard},
	 O_OK | O_FIXED | O_COMPILE, FB_BOUNDS | FB_WARN, FB_LINE, CAP_POSIX},
	{"runtime/orelse-product",
	 "static int __lhs,__rhs,__value,__need_fb;static int src(void){__lhs++;return @0@;}"
	 "static int fbc(void){__rhs++;return 11;}static void test(void){@3@}"
	 "int main(void){test();int truth=(@0@)!=0;return __value==(truth?7:11)&&__lhs==1&&"
	 "__rhs==((!truth&&__need_fb)?1:0)?0:1;}",
	 NULL, {&ax_runtime_truth, &ax_runtime_orelse_bind, &ax_runtime_orelse_fallback,
	        &ax_runtime_orelse_site},
	 O_OK | O_RUN, 0, FB_LINE, CAP_POSIX},
	{"runtime/orelse-defer-product", "@1@",
	 "static char logv[32];static int logn;static void ev(char c){logv[logn++]=c;}"
	 "static int same(const char*s){int i=0;while(s[i]&&logv[i]==s[i])i++;return !s[i]&&!logv[i];}"
	 "static int src(void){return @0@;}",
	 {&ax_runtime_truth, &ax_runtime_orelse_defer}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX},
	{"runtime/bare-orelse-lvalue-product", "@1@",
	 "static int gc,fc;static int src(void){gc++;return @0@;}static int fb(void){fc++;return 9;}",
	 {&ax_runtime_truth, &ax_runtime_bare_orelse_lvalue}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX},
	{"runtime/bare-orelse-defer-product", "@1@",
	 "static char logv[32];static int logn;static void ev(char c){logv[logn++]=c;}"
	 "static int same(const char*s){int i=0;while(s[i]&&logv[i]==s[i])i++;return !s[i]&&!logv[i];}"
	 "static int src(void){return @0@;}",
	 {&ax_runtime_truth, &ax_runtime_bare_orelse_defer}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX},
	{"runtime/defer-control-body-product", "@1@", NULL,
	 {&ax_runtime_truth, &ax_runtime_defer_control_body}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX},
	{"runtime/deferred-orelse-body-product", "@1@", NULL,
	 {&ax_runtime_truth, &ax_runtime_deferred_orelse_body}, O_OK | O_RUN | O_FIXED,
	 0, FB_LINE, CAP_POSIX | CAP_VLA},
	{"runtime/orelse-chain-matrix", "@0@",
	 "static int gc,fc;static int g(int v){gc++;return v;}static int fb(int v){fc++;return v;}",
	 {&ax_runtime_chain}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX},
	{"runtime/defer-function-pointer-shadow",
	 "static int calls;static void hit(int x){calls+=x;}"
	 "static void invoke(void(*defer)(int)){defer(2);}"
	 "int main(void){void(*defer)(int)=hit;defer(1);invoke(defer);return calls==3?0:1;}",
	 NULL, {0}, O_OK | O_RUN | O_OUTPUT_EQ_OFF, 0, FB_LINE, CAP_POSIX},
	{"runtime/orelse-qualified-product",
	 "static int gc,fc;static int g(int v){gc++;return v;}static int fb(int v){fc++;return v;}"
	 "int main(void){@0@ int x=g(@1@) orelse fb(9);return "
	 "(x==(@1@?@1@:9)&&gc==1&&fc==(@1@?0:1))?0:1;}",
	 NULL, {&ax_runtime_qualified, &ax_runtime_truth}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX},
	{"runtime/switch-fallthrough-matrix", "@0@",
	 "static char logv[8];static int lp,calls;static void ev(char c){logv[lp++]=c;logv[lp]=0;}"
	 "static int same(const char*s){int i=0;while(s[i]&&logv[i]==s[i])i++;return !s[i]&&!logv[i];}"
	 "static int g(int v){calls++;return v;}static int run(int sel){lp=0;logv[0]=0;calls=0;int acc=0;"
	 "switch(sel){case 0:{defer ev('A');int a=g(sel) orelse 100;acc+=a;}"
	 "case 1:{defer ev('B');int b=g(0) orelse 200;acc+=b;ev('1');}"
	 "case 2:{int c=g(7) orelse 300;acc+=c;ev('2');}break;"
	 "default:{int d=g(0) orelse 400;acc+=d;}}return acc;}",
	 {&ax_runtime_switch}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX},
	{"runtime/zeroinit-product", "@0@",
	 "__attribute__((noinline))static void dirty(void){volatile unsigned char j[512];"
	 "for(int k=0;k<512;k++)j[k]=0xAA;(void)j;}"
	 "static int nz(const void*p,unsigned long n){const unsigned char*b=p;int k=0;"
	 "for(unsigned long q=0;q<n;q++)if(b[q])k++;return k;}",
	 {&ax_runtime_zero}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX | CAP_VLA},
	{"runtime/zeroinit-fixed-scope-matrix", "@1@",
	 "__attribute__((noinline))static void dirty(void){volatile unsigned char j[512];"
	 "for(int k=0;k<512;k++)j[k]=0xAA;(void)j;}"
	 "static int nz(const void*p,unsigned long n){const unsigned char*b=p;int k=0;"
	 "for(unsigned long q=0;q<n;q++)if(b[q])k++;return k;}",
	 {&ax_runtime_zero_fixed, &ax_runtime_zero_scope}, O_OK | O_RUN | O_FIXED,
	 0, FB_LINE, CAP_POSIX, "= {0}"},
	{"runtime/zeroinit-scalar-scope-matrix", "@1@",
	 "__attribute__((noinline))static void dirty(void){volatile unsigned char j[512];"
	 "for(int k=0;k<512;k++)j[k]=0xAA;(void)j;}"
	 "static int nz(const void*p,unsigned long n){const unsigned char*b=p;int k=0;"
	 "for(unsigned long q=0;q<n;q++)if(b[q])k++;return k;}",
	 {&ax_runtime_zero_scalar, &ax_runtime_zero_scope}, O_OK | O_RUN | O_FIXED,
	 0, FB_LINE, CAP_POSIX, "v = 0"},
	{"runtime/zeroinit-vla-scope-matrix", "@1@",
	 "__attribute__((noinline))static void dirty(void){volatile unsigned char j[512];"
	 "for(int k=0;k<512;k++)j[k]=0xAA;(void)j;}"
	 "static int nz(const void*p,unsigned long n){const unsigned char*b=p;int k=0;"
	 "for(unsigned long q=0;q<n;q++)if(b[q])k++;return k;}",
	 {&ax_runtime_zero_vla, &ax_runtime_zero_scope}, O_OK | O_RUN | O_FIXED,
	 0, FB_LINE, CAP_POSIX | CAP_VLA, "__builtin_memset"},
	{"runtime/zeroinit-control-init-product", "@0@", NULL,
	 {&ax_runtime_zero_control_init}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX},
	{"differential/defer-identifier-next-operator", "int f(int defer){return defer @0@ 1;}", NULL,
	 {&ax_defer_ident_next_op}, O_OK | O_OUTPUT_EQ_OFF | O_COMPILE, 0, FB_LINE, CAP_POSIX},
	{"differential/defer-identifier-prev-operator", "int f(int defer){return 1 @0@ defer;}", NULL,
	 {&ax_defer_ident_prev_op}, O_OK | O_OUTPUT_EQ_OFF | O_COMPILE, 0, FB_LINE, CAP_POSIX},
	{"differential/defer-identifier-shapes", "@0@", NULL,
	 {&ax_defer_ident_shape}, O_OK | O_OUTPUT_EQ_OFF | O_COMPILE, 0, FB_LINE, CAP_POSIX},
	{"differential/orelse-identifier-next-operator", "int f(int orelse){return orelse @0@ 1;}", NULL,
	 {&ax_defer_ident_next_op}, O_OK | O_OUTPUT_EQ_OFF | O_COMPILE, 0, FB_LINE, CAP_POSIX},
	{"differential/orelse-identifier-prev-operator", "int f(int orelse){return 1 @0@ orelse;}", NULL,
	 {&ax_defer_ident_prev_op}, O_OK | O_OUTPUT_EQ_OFF | O_COMPILE, 0, FB_LINE, CAP_POSIX},
	{"differential/orelse-identifier-shapes", "@0@", NULL,
	 {&ax_orelse_ident_shape}, O_OK | O_OUTPUT_EQ_OFF | O_COMPILE, 0, FB_LINE, CAP_POSIX},
	{"runtime/bounds-traps", "int main(void){int a[4]={0},m[4][4]={{0}};volatile unsigned long i=4;@0@}",
	 NULL, {&ax_runtime_bounds_oob}, O_OK | O_TRAP, FB_BOUNDS, FB_LINE, CAP_POSIX},
	{"runtime/bounds-vla-trap",
	 "int main(int n,char**v){(void)v;n+=2;int a[n];volatile int i=n+1;return a[i];}",
	 NULL, {0}, O_OK | O_TRAP, FB_BOUNDS, FB_LINE, CAP_POSIX | CAP_VLA},
	{"runtime/bounds-inbounds-product", "@0@", NULL, {&ax_runtime_bounds_ok}, O_OK | O_RUN,
	 FB_BOUNDS, FB_LINE, CAP_POSIX | CAP_VLA},
	{"runtime/bounds-binding-trap-matrix",
	 "static int g[4]={0};typedef int Row[4];int main(void){int a[4]={0};"
	 "static int s[4]={0};Row r={0};volatile unsigned long idx=4;@1@}",
	 NULL, {&ax_runtime_bounds_base, &ax_runtime_bounds_access}, O_OK | O_TRAP | O_FIXED,
	 FB_BOUNDS, FB_LINE, CAP_POSIX, "__prism_bchk"},
	{"runtime/bounds-rank-trap-matrix",
	 "static int gm[4][3]={{0}};typedef int Matrix[4][3];int main(void){"
	 "int m[4][3]={{0}};static int sm[4][3]={{0}};Matrix tm={{0}};"
	 "volatile unsigned long idx=4;@1@}",
	 NULL, {&ax_runtime_bounds_matrix_base, &ax_runtime_bounds_matrix_access},
	 O_OK | O_TRAP | O_FIXED, FB_BOUNDS, FB_LINE, CAP_POSIX, "__prism_bchk"},
	{"runtime/bounds-typeof-vla-rank-traps",
	 "int main(void){int n=2;typeof(int[n][n]) a;volatile int i=n;@0@}", NULL,
	 {&ax_runtime_bounds_typeof_vla}, O_OK | O_TRAP | O_FIXED, FB_BOUNDS, FB_LINE,
	 CAP_POSIX | CAP_VLA, "__builtin_memset|__prism_bchk"},
	{"runtime/auto-static-product", "@0@", NULL, {&ax_runtime_autostatic}, O_OK | O_RUN,
	 FB_AS, FB_LINE, CAP_POSIX},

	/* Auto-static has a semantic runtime product above; these rows lock down
	 * the actual injection and its conservative eligibility boundary. */
	{"exact/auto-static-literal-without-zero",
	 "void f(void){const int values[2]={1,2};(void)values;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, FB_AS, FB_ZERO | FB_LINE, CAP_POSIX,
	 "static const int values[2]", NULL},
	{"exact/auto-static-string",
	 "void f(void){const char message[]=\"ok\";(void)message;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, FB_AS, FB_LINE, CAP_POSIX,
	 "static const char message[]", NULL},
	{"exact/auto-static-enum-designator",
	 "enum{A=1};void f(void){const int table[3]={[A]=7,[2]=9};(void)table;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, FB_AS, FB_LINE, CAP_POSIX,
	 "static const int table[3]", NULL},
	{"exact/auto-static-declarator-const-pointer-array",
	 "void f(void){int * const ptrs[2]={0,0};(void)ptrs;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, FB_AS, FB_LINE, CAP_POSIX,
	 "static int * const ptrs[2]", NULL},
	{"exact/auto-static-disabled",
	 "void f(void){const int values[2]={1,2};(void)values;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, 0, FB_AS | FB_LINE, CAP_POSIX,
	 NULL, "static const int values[2]"},
	{"exact/auto-static-skip-multiple-declarators",
	 "void f(void){const int left[2]={1,2},right[2]={3,4};(void)left;(void)right;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, FB_AS, FB_LINE, CAP_POSIX,
	 NULL, "static const int left[2]|static const int right[2]"},
	{"exact/auto-static-skip-pointer-to-const",
	 "void f(void){const int *values[2]={0,0};(void)values;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, FB_AS, FB_LINE, CAP_POSIX,
	 NULL, "static const int *values[2]"},
	{"exact/auto-static-skip-raw",
	 "void f(void){raw const int values[2]={1,2};(void)values;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, FB_AS, FB_LINE, CAP_POSIX,
	 NULL, "static const int values[2]"},
	{"exact/auto-static-skip-auto-storage",
	 "void f(void){auto const int values[2]={1,2};(void)values;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, FB_AS, FB_LINE, CAP_POSIX,
	 NULL, "static auto const int values[2]"},
	{"exact/auto-static-skip-vla",
	 "void f(int n){const int values[n];(void)values;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, FB_AS, FB_ZERO | FB_BOUNDS | FB_LINE, CAP_POSIX | CAP_VLA,
	 NULL, "static const int values[n]"},
	{"exact/auto-static-skip-volatile-typedef",
	 "typedef volatile int VI;void f(void){const VI values[2]={1,2};(void)values;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, FB_AS, FB_LINE, CAP_POSIX,
	 NULL, "static const VI values[2]"},
	{"exact/auto-static-skip-runtime-initializer",
	 "int value(void);void f(void){const int values[2]={value(),2};(void)values;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, FB_AS, FB_LINE, CAP_POSIX,
	 NULL, "static const int values[2]"},
	{"exact/auto-static-skip-declarator-attribute",
	 "void f(void){const int values[2] __attribute__((unused))={1,2};(void)values;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, FB_AS, FB_LINE, CAP_POSIX,
	 NULL, "static const int values[2]"},
	{"exact/auto-static-skip-control-init",
	 "void f(void){for(const int values[2]={1,2};;){(void)values;break;}}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, FB_AS, FB_LINE, CAP_POSIX,
	 NULL, "static const int values[2]"},
	{"exact/auto-static-skip-mutable-typeof-array",
	 "void f(void){typeof(int[2]) values={1,2};(void)values;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE, FB_AS, FB_LINE, CAP_POSIX,
	 NULL, "static typeof(int[2]) values"},
	/* In C99/C11 `true` and `false` are ordinary identifiers.  They may be
	 * parameters or locals, so lexical spelling alone must never certify a
	 * static initializer.  Registered enum constants with the same spellings
	 * remain integer constant expressions and must still be promoted. */
	{"runtime/auto-static-skip-shadowed-bool-identifiers",
	 "static int f(int true,int false){const int values[2]={true,false};return values[0]*10+values[1];}"
	 "int main(void){return f(4,7)==47?0:1;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE | O_RUN, FB_AS, FB_LINE, CAP_POSIX,
	 NULL, "static const int values[2]"},
	{"runtime/auto-static-enum-bool-identifiers",
	 "enum{true=4,false=7};int main(void){const int values[2]={true,false};"
	 "return values[0]*10+values[1]==47?0:1;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE | O_RUN, FB_AS, FB_LINE, CAP_POSIX,
	 "static const int values[2]", NULL},
	{"runtime/auto-static-enum-bool-identifiers-disabled",
	 "enum{true=4,false=7};int main(void){const int values[2]={true,false};"
	 "return values[0]*10+values[1]==47?0:1;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE | O_RUN, 0, FB_AS | FB_LINE, CAP_POSIX,
	 NULL, "static const int values[2]"},

	{"exact/defer-lifo", "void p(char);int main(void){defer p('A');{defer p('B');p('x');}return 0;}", NULL,
	 {0}, O_OK | O_NO_EXT | O_FIXED, 0, FB_LINE, 0, "p('A')|p('B')", NULL},
	/* Capture discovery precedes normal orelse/raw annotation.  The extension
	 * spellings inside a deferred declaration must not enter defer_name_set;
	 * otherwise the unrelated locals below are rejected as false shadows. */
	{"runtime/defer-capture-ignores-orelse-operator",
	 "static int result;static int g(void){return 0;}static void use(int x){result=x;}"
	 "static void f(void){defer {int y=g() orelse 7;use(y);}int orelse=5;use(orelse);}"
	 "int main(void){f();return result==7?0:1;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE | O_RUN, 0, FB_LINE, CAP_POSIX},
	{"runtime/defer-capture-ignores-raw-prefix",
	 "static int result;static void f(void){defer {raw int y=3;result=y;}int raw=5;result=raw;}"
	 "int main(void){f();return result==3?0:1;}", NULL, {0},
	 O_OK | O_FIXED | O_COMPILE | O_RUN, 0, FB_LINE, CAP_POSIX},
	/* The positional exclusion must not discard an ordinary parameter named
	 * orelse: it is genuinely captured, so a nested shadow on return remains
	 * unsafe and must be diagnosed. */
	{"reject/defer-capture-soft-orelse-identifier",
	 "void use(int);void f(int orelse){defer {use(orelse);}{int orelse=1;return;}}", NULL, {0},
	 O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "shadows"},
	{"reject/orelse-in-braceless-defer", "void cleanv(int);int g(void);void f(void){defer cleanv(g() orelse 1);}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0},
	{"reject/declaration-in-braceless-defer", "void f(void){defer int x=1;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "declaration as a braceless defer body"},
	{"reject/break-in-braced-defer", "void f(void){for(;;){defer {break;}break;}}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "break"},
	{"reject/continue-in-braced-defer", "void f(void){for(;;){defer {continue;}break;}}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "continue"},
	{"exact/zeroinit-scalar", "void f(void){int x;(void)x;}", NULL, {0}, O_OK, 0, FB_LINE, 0,
	 "x = 0", NULL},
	{"exact/zeroinit-array", "void f(void){int a[8];(void)a;}", NULL, {0}, O_OK, 0, FB_LINE, 0,
	 "a[8] = {0}", NULL},
	{"exact/raw-suppresses-zero", "void f(void){raw int x;(void)x;}", NULL, {0}, O_OK | O_NO_EXT,
	 0, FB_LINE, 0, NULL, "memset|x = 0"},
	{"exact/bounds-wrap", "int f(int i){int a[8]={0};return a[i];}", NULL, {0}, O_OK,
	 FB_BOUNDS, FB_LINE, 0, "__prism_bchk", NULL},
	{"exact/bounds-address", "int f(void){int a[8]={0};return &a[8]!=0;}", NULL, {0}, O_OK,
	 FB_BOUNDS, FB_LINE, 0, "&a[8]", "&a[__prism_bchk"},
	{"exact/raw-after-pragma", "#pragma pack(push,1)\nraw struct S{char c;int x;};\n#pragma pack(pop)\n", NULL,
	 {0}, O_OK | O_NO_EXT | O_COMPILE, 0, FB_LINE, CAP_POSIX},
	{"exact/raw-block-orelse", "int g(void);void f(void){raw {int x=g() orelse 2;(void)x;}}", NULL,
	 {0}, O_OK, 0, FB_LINE, 0, "g() orelse 2", NULL},
	{"exact/constexpr-array", "void f(void){constexpr int N=4;int a[N];(void)a;}", NULL,
	 {0}, O_OK, 0, FB_LINE, 0, NULL, "memset"},
	{"exact/const-typeof-scalar-brace-init",
	 "void f(void){const typeof(int) x;(void)x;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", "memset"},
	{"exact/const-typeof-union-brace-init",
	 "union U{int i;double d;};void f(void){const typeof(union U) x;(void)x;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", "memset"},
	{"exact/const-union-brace-init",
	 "union U{int i;double d;};void f(void){const union U x;(void)x;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", "memset"},
	{"exact/const-typeof-pointer-to-vla-brace-init",
	 "void f(int n){const typeof(int (*)[n]) p;(void)sizeof(p);}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX | CAP_VLA, "= {0}", "memset"},
	{"exact/const-typeof-array-brace-init",
	 "void f(void){__typeof__(int *const[5]) a,b;(void)a;(void)b;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", "memset"},
	{"exact/long-double-full-object-zero",
	 "void f(void){long double x;(void)x;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "__builtin_memset", "x = 0"},
	{"exact/long-double-typedef-full-object-zero",
	 "typedef long double LD;void f(void){LD x;(void)x;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "__builtin_memset", "x = 0"},
	{"exact/typeof-typedef-preserves-zero-strategy",
	 "typedef typeof((long double)0) Base;typedef Base LD;void f(void){LD x;(void)x;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "__builtin_memset", "x = {0}"},
	{"reject/const-long-double-auto-zero",
	 "void f(void){const long double x;(void)x;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "const"},
	{"reject/const-typeof-long-double-binding",
	 "void f(void){long double source;const typeof(source) copy;(void)source;(void)copy;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "const"},
	{"exact/typeof-sizeof-atomic-is-not-atomic",
	 "void f(void){_Atomic int source;const typeof(sizeof(source)) n;(void)source;(void)n;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", NULL},
	{"exact/typeof-const-binding-brace-init",
	 "void f(void){const int source=1;typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", "memset"},
	{"exact/typeof-parenthesized-const-binding-brace-init",
	 "void f(void){const int source=1;typeof(((source))) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", "memset"},
	{"exact/typeof-const-parameter-brace-init",
	 "void f(const int source){typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", "memset"},
	{"exact/typeof-const-pointer-parameter-brace-init",
	 "void f(int *const source){typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", "memset"},
	{"exact/typeof-pointer-to-const-parameter-not-const",
	 "void f(const int *source){typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "memset", "= {0}"},
	{"exact/typeof-array-parameter-element-const-not-pointer-const",
	 "void f(const int source[2]){typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "memset", "= {0}"},
	{"exact/typeof-array-parameter-pointer-const-brace-init",
	 "void f(int source[const 2]){typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", "memset"},
	{"exact/typeof-volatile-pointer-binding-uses-volatile-zero",
	 "void f(void){int *volatile source=0;typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "volatile char", "__builtin_memset"},
	{"exact/typeof-volatile-pointer-parameter-uses-volatile-zero",
	 "void f(int *volatile source){typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "volatile char", "__builtin_memset"},
	{"exact/typeof-volatile-pointer-typedef-uses-volatile-zero",
	 "typedef int *volatile VP;void f(void){VP source=0;typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "volatile char", "__builtin_memset"},
	{"exact/typeof-inner-volatile-pointer-is-not-top-level",
	 "void f(int *volatile *source){typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "__builtin_memset", "volatile char"},
	{"exact/typeof-knr-const-parameter-brace-init",
	 "void f(source)const int source;{typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", "memset"},
	{"exact/typeof-knr-volatile-pointer-parameter",
	 "void f(source)int *volatile source;{typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "volatile char", "__builtin_memset"},
	{"exact/typeof-knr-pointer-to-const-not-const",
	 "void f(source)const int *source;{typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "__builtin_memset", "= {0}"},
	{"exact/typeof-unqual-drops-const-binding",
	 "void f(void){const int source=1;typeof_unqual(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_FIXED, 0, FB_LINE, 0, "sizeof(copy)", "= {0}"},
	{"exact/typeof-sizeof-const-is-not-const",
	 "void f(void){const int source=1;typeof(sizeof(source)) n;(void)n;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "memset", "= {0}"},
	{"exact/typeof-sizeof-atomic-type-is-not-atomic",
	 "void f(void){const typeof(sizeof(_Atomic int)) n;(void)n;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", "memset"},
	{"exact/typeof-alignof-atomic-type-is-not-atomic",
	 "void f(void){const typeof(_Alignof(_Atomic int)) n;(void)n;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", "memset"},
	{"exact/typeof-sizeof-const-type-is-not-inherently-const",
	 "void f(void){typeof(sizeof(const int)) n;(void)n;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "memset", "= {0}"},
	{"exact/typeof-real-const-deref-remains-const",
	 "void f(void){typeof(*(const int*)0) n;(void)n;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", "memset"},
	{"reject/const-atomic-typeof-scalar-memset",
	 "void f(void){const _Atomic typeof(int) x;(void)x;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "const"},
	{"reject/const-atomic-typeof-expression-memset",
	 "struct S{int a,b;};void f(void){struct S s;const _Atomic(typeof(s)) c;(void)s;(void)c;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "const"},
	{"reject/const-typeof-atomic-binding-memset",
	 "void f(void){_Atomic int source;const typeof(source) copy;(void)source;(void)copy;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "const"},
	{"reject/const-typeof-atomic-pointer-binding",
	 "void f(void){int *_Atomic source=0;const typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "const"},
	{"reject/const-typeof-atomic-pointer-parameter",
	 "void f(int *_Atomic source){const typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "const"},
	{"reject/const-typeof-atomic-pointer-typedef",
	 "typedef int *_Atomic AP;void f(void){AP source=0;const typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "const"},
	{"exact/const-typeof-pointer-to-atomic-is-not-atomic",
	 "void f(_Atomic int *source){const typeof(source) copy;(void)copy;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "= {0}", "memset"},
	{"exact/constexpr-dimension-not-vla",
	 "int f(void){goto L;raw constexpr int N=4;raw int a[N];L:return 0;}", NULL,
	 {0}, O_OK | O_FIXED, 0, FB_LINE, 0},
	{"reject/constexpr-param-shadow-is-vla",
	 "constexpr int N=4;void f(int N){goto L;int a[N];L:(void)a;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_ZERO | FB_LINE, 0, NULL, NULL, "VLA"},
	{"exact/namespace-tag-shadow", "struct R{volatile int x;};void f(int R){typeof(struct R)x;(void)x;(void)R;}", NULL,
	 {0}, O_OK, 0, FB_LINE, 0, NULL, "__builtin_memset"},
	{"exact/do-tail-defer", "void c(void);int f(void){return ({do{defer c();}while(0);1;});}", NULL,
	 {0}, O_OK | O_NO_EXT | O_FIXED, 0, FB_LINE, CAP_GNU},
	{"exact/orelse-mid-chain", "int *g(void);void f(int n){int a[n];int *p=0; p=g() orelse &a[0] orelse 0;}", NULL,
	 {0}, O_OK | O_NO_EXT | O_FIXED, 0, FB_LINE, CAP_VLA},
	{"exact/pp-straddle", "int g(void);void f(void){int x=\n#if 1\ng() orelse 1\n#else\n2\n#endif\n;(void)x;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "preprocessor"},
	{"reject/defer-inside-unresolved-pp-conditional",
	 "void c(void);void f(void){\n#ifdef ENABLE_CLEANUP\ndefer c();\n#endif\n}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "preprocessor conditional"},
	{"reject/defer-inside-digraph-pp-conditional",
	 "void c(void);void f(void){\n%:ifdef ENABLE_CLEANUP\ndefer c();\n%:endif\n}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "preprocessor conditional"},
	{"exact/defer-after-closed-pp-conditional",
	 "void c(void);\n#ifdef UNUSED\nint x;\n#endif\nvoid f(void){defer c();}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "c();", NULL},
	{"exact/pp-conditional-inside-defer-body",
	 "void c(void);void f(void){defer {\n#ifdef ENABLE_CLEANUP\nc();\n#endif\n}}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "#ifdef ENABLE_CLEANUP", NULL},
	{"exact/raw-block-bracket-orelse", "int g(void);void f(void){raw {int a[g() orelse 4];(void)a;}}", NULL,
	 {0}, O_OK, 0, FB_LINE, 0, "g() orelse 4", "__prism_oe_|__prism_dim_"},
	{"exact/repeated-raw-prefix", "void f(void){raw raw int x;(void)x;}", NULL,
	 {0}, O_OK | O_NO_EXT | O_FIXED, 0, FB_LINE, 0},
	{"exact/pp-fallback-tail", "int g(void);void f(void){int x=g() orelse\n#if 1\n7\n#else\n9\n#endif\n;(void)x;}", NULL,
	 {0}, O_OK | O_NO_EXT | O_COMPILE, 0, FB_LINE, CAP_POSIX, "#endif\n;", "#endif;"},
	{"exact/user-setjmp-definition", "int setjmp(void*p){(void)p;return 0;}void c(void);void f(void){defer c();(void)setjmp(0);}", NULL,
	 {0}, O_OK | O_NO_EXT, 0, FB_LINE, 0},
	{"exact/user-exit-definition", "int exit(void){return 7;}int f(void){return exit();}", NULL,
	 {0}, O_OK, FB_AUR, FB_LINE, 0, NULL, "__builtin_unreachable"},
	{"exact/generic-decl-plain",
	 "typedef unsigned long Z;extern _Generic((char*)0,char*:memchr,default:memchr)(const void*,int,Z);", NULL,
	 {0}, O_OK | O_FIXED, 0, FB_LINE, 0, NULL, "_Generic"},
	{"exact/generic-decl-paren",
	 "typedef unsigned long Z;extern _Generic((char*)0,char*:(memchr),default:(memchr))(const void*,int,Z);", NULL,
	 {0}, O_OK | O_FIXED, 0, FB_LINE, 0, NULL, "_Generic"},
	{"exact/generic-decl-associated-params",
	 "typedef unsigned long Z;extern _Generic((char*)0,char*:memchr(const void*,int,Z),default:memchr(const void*,int,Z));", NULL,
	 {0}, O_OK | O_FIXED, 0, FB_LINE, 0, NULL, "_Generic"},
	{"exact/generic-decl-cast-wrapped",
	 "typedef unsigned long Z;extern _Generic((char*)0,char*:(const char *)(memchr)(const void*,int,Z),default:(const char *)(memchr)(const void*,int,Z));", NULL,
	 {0}, O_OK | O_FIXED, 0, FB_LINE, 0, NULL, "_Generic"},
	{"reject/zeroinit-vla-for-init", "void f(int n){for(int v[n];0;)(void)v;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, CAP_VLA, NULL, NULL,
	 "VLA in for/if/switch init-statement"},
	{"exact/generic-decl-distinct-targets",
	 "typedef unsigned long Z;extern _Generic((char*)0,char*:memchr,default:other)(const void*,int,Z);", NULL,
	 {0}, O_OK | O_FIXED, 0, FB_LINE, 0, "_Generic", NULL},
	{"exact/generic-decl-distinct-associated-params",
	 "typedef unsigned long Z;extern _Generic((char*)0,char*:memchr(const void*,int,Z),default:memchr(void*,int,Z));", NULL,
	 {0}, O_OK | O_FIXED, 0, FB_LINE, 0, "_Generic", NULL},
	{"exact/generic-decl-mixed-call-and-name-targets",
	 "typedef unsigned long Z;extern _Generic((char*)0,char*:memchr(const void*,int,Z),default:memchr)(const void*,int,Z);", NULL,
	 {0}, O_OK | O_FIXED, 0, FB_LINE, 0, "_Generic", NULL},
	{"exact/generic-decl-nonidentifier-target",
	 "typedef unsigned long Z;extern _Generic((char*)0,char*:memchr,default:(void*)0)(const void*,int,Z);", NULL,
	 {0}, O_OK | O_FIXED, 0, FB_LINE, 0, "_Generic", NULL},
	{"exact/typedef-named-raw-prefix", "typedef int raw;void f(void){raw raw x;(void)x;}", NULL,
	 {0}, O_OK | O_FIXED | O_COMPILE, 0, FB_LINE, CAP_POSIX, NULL, "raw raw"},
	{"exact/typedef-named-raw-before-builtin", "typedef int raw;void f(void){raw int x;(void)x;}", NULL,
	 {0}, O_OK | O_FIXED | O_COMPILE, 0, FB_LINE, CAP_POSIX, NULL, "raw int"},
	{"exact/soft-defer-after-wide-operators",
	 "int f(int x){return (x<<defer)+(x>>defer)+(x&&defer)+(x||defer)+(x==defer)+(x!=defer);}", NULL,
	 {0}, O_OK | O_FIXED, 0, FB_LINE, 0, "defer", NULL},
	{"exact/soft-defer-postfix-unbound", "void f(void){defer++;defer--;}", NULL,
	 {0}, O_OK | O_FIXED, 0, FB_LINE, 0, "defer++|defer--", NULL},
	{"exact/typedef-soft-bitfield-members",
	 "typedef struct S{unsigned defer:1;unsigned orelse:1;}S;int f(S*p){return p->defer+p->orelse;}", NULL,
	 {0}, O_OK | O_FIXED | O_COMPILE, 0, FB_LINE, CAP_POSIX},
	{"exact/canonical-zero-memset",
	 "void f(void){int a[4];__builtin_memset(&a,0,sizeof(a));(void)a;}", NULL,
	 {0}, O_OK | O_FIXED | O_COMPILE, 0, FB_LINE, CAP_POSIX},
	{"exact/canonical-zero-vla-memset",
	 "void f(int n){int a[n];__builtin_memset(&a,0,sizeof(a));(void)a;}", NULL,
	 {0}, O_OK | O_FIXED | O_COMPILE, 0, FB_LINE, CAP_POSIX | CAP_VLA},
	{"reject/orelse-prototype-dimension", "void f(int a[(0 orelse 2)]);", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "prototype"},
	{"exact/bounds-cancel-address-paren", "int f(int i){int a[4]={0};return *(&a[i]);}", NULL,
	 {0}, O_OK, FB_BOUNDS, FB_LINE, 0, "__prism_bchk", NULL},
	{"exact/bounds-address-inner-paren", "int*f(int i){static int a[4];return &(a[i]);}", NULL,
	 {0}, O_OK, FB_BOUNDS, FB_LINE, 0, "&(a[i])", "__prism_bchk(("},
	{"runtime/bounds-address-inner-paren-one-past",
	 "int main(void){int a[4];volatile unsigned long i=4;return &(a[i])==a+4?0:1;}", NULL,
	 {0}, O_OK | O_RUN, FB_BOUNDS, FB_LINE, CAP_POSIX},
	{"exact/bounds-control-deref-add", "void f(int c,int i){int a[4]={0};if(c)*(a+i)=0;}", NULL,
	 {0}, O_REJECT | O_DIAG, FB_BOUNDS, FB_LINE, 0, NULL, NULL, "pointer-arithmetic"},
	{"exact/bounds-control-deref-add-warn", "void f(int c,int i){int a[4]={0};if(c)*(a+i)=0;}", NULL,
	 {0}, O_OK, FB_BOUNDS | FB_WARN, FB_LINE, 0, NULL, "__prism_bchk(("},
	{"exact/bounds-unevaluated-nested-paren-deref-add",
	 "int f(int i){int a[4];return sizeof((*(a+i)));}", NULL,
	 {0}, O_OK | O_COMPILE | O_FIXED, FB_BOUNDS, FB_LINE, CAP_POSIX},
	{"exact/bounds-unevaluated-comma-deref-add",
	 "int f(int i){int a[4];return sizeof((0,*(a+i)));}", NULL,
	 {0}, O_OK | O_COMPILE | O_FIXED, FB_BOUNDS, FB_LINE, CAP_POSIX},
	{"exact/bounds-generic-control-comma-deref-add",
	 "int f(int i){int a[4];return _Generic((0,*(a+i)),int:1);}", NULL,
	 {0}, O_OK | O_COMPILE | O_FIXED, FB_BOUNDS, FB_LINE, CAP_POSIX},
	{"exact/bounds-typeof-comma-deref-add",
	 "int f(int i){int a[4];typeof((0,*(a+i)))x=0;return x;}", NULL,
	 {0}, O_OK | O_COMPILE | O_FIXED, FB_BOUNDS, FB_LINE, CAP_POSIX},
	{"reject/bounds-evaluated-nested-paren-deref-add",
	 "int f(int i){int a[4];return ((0,*(a+i)));}", NULL,
	 {0}, O_REJECT | O_DIAG, FB_BOUNDS, FB_LINE, 0, NULL, NULL, "pointer-arithmetic"},
	{"reject/bounds-generic-association-deref-add-is-evaluated",
	 "int f(int i){int a[4];return _Generic(0,int:*(a+i),default:0);}", NULL,
	 {0}, O_REJECT | O_DIAG, FB_BOUNDS, FB_LINE, 0, NULL, NULL, "pointer-arithmetic"},
	{"exact/bounds-parenthesized-reverse-address", "int*f(int i){int a[4]={0};return &(i[a]);}", NULL,
	 {0}, O_REJECT | O_DIAG, FB_BOUNDS, FB_LINE, 0},
	/* Keep the self-hosting lookup shape covered: recovering a tracked outer
	 * array through a parenthesized member array is valid and must not be
	 * rejected as a non-identifier base. */
	{"runtime/bounds-parenthesized-member-array-base",
	 "typedef struct{char name[8];}Entry;int main(void){Entry entries[2]={{\"alpha\"},{\"beta\"}};"
	 "return (entries[1].name)[0]=='b'?0:1;}", NULL,
	 {0}, O_OK | O_RUN | O_FIXED, FB_BOUNDS, FB_LINE, CAP_POSIX, "__prism_bchk"},
	{"reject/bounds-conditional-array-base",
	 "int f(int choose,int i){int a[4]={0},b[2]={0};return (choose?a:b)[i];}", NULL,
	 {0}, O_REJECT | O_DIAG, FB_BOUNDS, FB_LINE, 0, NULL, NULL,
	 "conditional subscript base derived from tracked array"},
	{"exact/special-wrapper-transitive",
	 "#include <setjmp.h>\nstatic void inner(jmp_buf p){setjmp(p);}"
	 "static void outer(jmp_buf p){int touched=1;inner(p);(void)touched;}"
	 "void c(void);void f(jmp_buf p){defer c();outer(p);}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "setjmp"},
	{"reject/special-wrapper-transitive-cast",
	 "#include <setjmp.h>\nstatic void inner(jmp_buf p){setjmp(p);}"
	 "static void outer(jmp_buf p){int touched=1;(void)inner(p);(void)touched;}"
	 "void c(void);void f(jmp_buf p){defer c();outer(p);}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "setjmp"},
	{"exact/special-shadowed-cast-not-call",
	 "#include <setjmp.h>\nstatic void hazardous(jmp_buf p){setjmp(p);}"
	 "void c(void);void innocent(jmp_buf p){int hazardous=1;(void)hazardous;"
	 "defer c();(void)p;}", NULL,
	 {0}, O_OK, 0, FB_LINE, 0},

	{"reject/defer-filescope", "defer (void)0;", NULL, {0}, O_REJECT | O_DIAG, 0, FB_LINE},
	/* A second close brace reaches emitter recovery after the function scope has
	 * been popped. This must be a normal syntax error, never scope_stack[-1]. */
	{"reject/stray-close-brace", "int f(void){}}", NULL, {0}, O_REJECT | O_DIAG, 0, FB_LINE},
	{"reject/fuzz-file-scope-orelse",
	 "orelse****\372*/*\377/*\377* \372  **\372*/\337", NULL, {0}, O_ANY_STATUS | O_REPLAY,
	 FB_DEFER | FB_ORELSE | FB_BOUNDS | FB_AUR, FB_ZERO | FB_LINE | FB_AS | FB_WARN},
	{"reject/defer-return", "void f(void){defer return;}", NULL, {0}, O_REJECT | O_DIAG, 0, FB_LINE},
	{"reject/defer-break", "void f(void){defer break;}", NULL, {0}, O_REJECT | O_DIAG, 0, FB_LINE},
	{"reject/defer-continue", "void f(void){defer continue;}", NULL, {0}, O_REJECT | O_DIAG, 0, FB_LINE},
	{"reject/orelse-filescope", "int x=0 orelse 1;", NULL, {0}, O_REJECT | O_DIAG, 0, FB_LINE},
	{"reject/orelse-sideeffect", "int k(void);void f(int n){int a[n];int(*v[2])[n]={&a,&a};int(*p)[n]=0;*p=0 orelse v[k()] orelse 0;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, CAP_VLA},
	{"reject/goto-vla", "void f(int n){goto L;int a[n];L:(void)0;}", NULL, {0}, O_REJECT | O_DIAG, 0, FB_LINE, CAP_VLA},
	{"reject/null-source", NULL, NULL, {0}, O_NULL_SOURCE | O_ANY_STATUS, 0, 0},

	{"compile/basic", "int main(void){int x;return x;}", NULL, {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX},
	{"compile/features", "int g(void);void c(void);int main(void){int a[4];defer c();int x=g() orelse 2;return a[0]+x;}", NULL,
	 {0}, O_OK | O_COMPILE | O_NO_EXT, FB_BOUNDS, FB_LINE, CAP_POSIX},
	{"file/api", "int main(void){int x;return x;}", NULL, {0}, O_FILE | O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX},

	{"runtime/orelse-truthy", "static int n;static int g(void){n++;return 5;}static int h(void){n+=10;return 9;}int main(void){int x=g() orelse h();return x==5&&n==1?0:1;}", NULL,
	 {0}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX, NULL, NULL, NULL, 0},
	{"runtime/orelse-falsy", "static int n;static int g(void){n++;return 0;}static int h(void){n+=10;return 9;}int main(void){int x=g() orelse h();return x==9&&n==11?0:1;}", NULL,
	 {0}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX, NULL, NULL, NULL, 0},
	{"runtime/orelse-chain", "static int n;static int g(int x){n++;return x;}int main(void){int x=g(0) orelse g(7) orelse g(9);return x==7&&n==2?0:1;}", NULL,
	 {0}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX, NULL, NULL, NULL, 0},
	{"runtime/defer-return", "static int n;static void c(void){n++;}static int f(void){defer c();return 7;}int main(void){int x=f();return x==7&&n==1?0:1;}", NULL,
	 {0}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX, NULL, NULL, NULL, 0},
	{"runtime/defer-lifo", "static int n;static void a(void){n=n*10+1;}static void b(void){n=n*10+2;}int main(void){{defer a();defer b();}return n==21?0:1;}", NULL,
	 {0}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX, NULL, NULL, NULL, 0},
	{"runtime/defer-break", "static int n;static void c(void){n++;}int main(void){for(;;){{defer c();break;}}return n==1?0:1;}", NULL,
	 {0}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX, NULL, NULL, NULL, 0},
	{"runtime/defer-continue", "static int n;static void c(void){n++;}int main(void){for(int i=0;i<3;i++){{defer c();continue;}}return n==3?0:1;}", NULL,
	 {0}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX, NULL, NULL, NULL, 0},
	{"runtime/defer-goto", "static int n;static void c(void){n++;}int main(void){{defer c();goto L;}L:return n==1?0:1;}", NULL,
	 {0}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX, NULL, NULL, NULL, 0},
	{"runtime/vla-zero", "static void dirty(void){volatile unsigned char x[4096];for(int i=0;i<4096;i++)x[i]=255;}int main(int c,char**v){(void)v;dirty();int n=c+3;int a[n];for(int i=0;i<n;i++)if(a[i])return 1;return 0;}", NULL,
	 {0}, O_OK | O_RUN, 0, FB_LINE, CAP_POSIX | CAP_VLA, NULL, NULL, NULL, 0},
	{"runtime/bounds-in", "int main(void){int a[4]={1,2,3,4};volatile int i=3;return a[i]==4?0:1;}", NULL,
	 {0}, O_OK | O_RUN, FB_BOUNDS, FB_LINE, CAP_POSIX, NULL, NULL, NULL, 0},
	{.id="runtime/orelse-multidecl-defer-return",
	 .source="static int logv;static int g(int x){return x;}static int choose(int a,int b){return a+b-18;}static void clean(void){logv++;}static int f(int v){defer clean();int x=g(v) orelse return choose(17,18),y=9;return x+y;}int main(void){logv=0;if(f(0)!=17||logv!=1)return 1;logv=0;return f(3)==12&&logv==1?0:2;}",
	 .oracle=O_OK|O_RUN, .clear_features=FB_LINE, .requires=CAP_POSIX, .expected_exit=0},
	{.id="runtime/bounds-shadow-timeline",
	 .source="int main(void){volatile int i=1;int sum=0;int a[2]={1,2};sum+=a[i];{int a[2]={2,3};sum+=a[i];{int a[2]={3,4};sum+=a[i];{int a[2]={4,5};sum+=a[i];{int a[2]={5,6};sum+=a[i];{int a[2]={6,7};sum+=a[i];{int a[2]={7,8};sum+=a[i];{int a[2]={8,9};sum+=a[i];{int a[2]={9,10};sum+=a[i];sum+=a[i];}sum+=a[i];}sum+=a[i];}sum+=a[i];}sum+=a[i];}sum+=a[i];}sum+=a[i];}sum+=a[i];}sum+=a[i];return sum==108?0:1;}",
	 .oracle=O_OK|O_RUN, .set_features=FB_BOUNDS, .clear_features=FB_LINE, .requires=CAP_POSIX, .expected_exit=0},
	{.id="runtime/volatile-atomic-once",
	 .source="static int g_calls,fb_calls;static int g(int v){g_calls++;return v;}static int fb(int v){fb_calls++;return v;}int main(void){volatile int a=g(0) orelse fb(9);if(a!=9||g_calls!=1||fb_calls!=1)return 1;g_calls=fb_calls=0;_Atomic int b=g(7) orelse fb(3);return b==7&&g_calls==1&&fb_calls==0?0:2;}",
	 .oracle=O_OK|O_RUN, .clear_features=FB_LINE, .requires=CAP_POSIX, .expected_exit=0},
	{.id="runtime/bracket-dimension",
	 .source="static int calls;static int dim(int v){calls++;return v;}int main(void){int t[dim(3) orelse 5];if(sizeof(t)/sizeof(t[0])!=3||calls!=1)return 1;calls=0;int a[dim(0) orelse 5];if(sizeof(a)/sizeof(a[0])!=5||calls!=1)return 2;int lo=0,hi=4;int b[lo orelse hi orelse 9];return sizeof(b)/sizeof(b[0])==4?0:3;}",
	 .oracle=O_OK|O_RUN, .clear_features=FB_LINE, .requires=CAP_POSIX|CAP_VLA, .expected_exit=0},
	{.id="runtime/switch-fallthrough",
	 .source="static char logv[8];static int lp;static void ev(char c){logv[lp++]=c;}int main(void){int sel=0,acc=0;switch(sel){case 0:{defer ev('A');int a=sel orelse 100;acc+=a;}case 1:{defer ev('B');int b=0 orelse 200;acc+=b;ev('1');}case 2:{int c=7 orelse 300;acc+=c;ev('2');break;}}return acc==307&&logv[0]=='A'&&logv[1]=='1'&&logv[2]=='B'&&logv[3]=='2'?0:1;}",
	 .oracle=O_OK|O_RUN, .clear_features=FB_LINE, .requires=CAP_POSIX, .expected_exit=0},
	{.id="runtime/orelse-goto-cleanup",
	 .source="static char l[8];static int p;static void e(char c){l[p++]=c;}static int g(int v){return v;}static void f(int v){p=0;defer e('A');{defer e('B');int x=g(v) orelse goto done;(void)x;e('x');}e('m');done:e('.');}int main(void){f(0);if(l[0]!='B'||l[1]!='.'||l[2]!='A')return 1;f(1);return l[0]=='x'&&l[1]=='B'&&l[2]=='m'&&l[3]=='.'&&l[4]=='A'?0:2;}",
	 .oracle=O_OK|O_RUN, .clear_features=FB_LINE, .requires=CAP_POSIX, .expected_exit=0},
	{.id="runtime/return-capture",
	 .source="static int f(void){int r=1;defer r=99;return r;}int main(void){return f()==1?0:1;}",
	 .oracle=O_OK|O_RUN, .clear_features=FB_LINE, .requires=CAP_POSIX, .expected_exit=0},
	{.id="runtime/zeroinit-stack",
	 .source="static void poison(void){volatile unsigned char x[4096];for(int i=0;i<4096;i++)x[i]=255;}int main(void){poison();int x;int a[32];if(x)return 1;for(int i=0;i<32;i++)if(a[i])return 2;return 0;}",
	 .oracle=O_OK|O_RUN, .clear_features=FB_LINE, .requires=CAP_POSIX, .expected_exit=0},

	{"lifecycle/reset", "int f(void){int x;defer(void)0;return x;}", NULL, {0}, O_LIFECYCLE,
	 0, FB_LINE, 0, NULL, NULL, NULL, 0, "TRTRT"},
	{"lifecycle/error", "int f(void){int x;defer(void)0;return x;}", NULL, {0}, O_LIFECYCLE,
	 0, FB_LINE, 0, NULL, NULL, NULL, 0, "TETET"},
	{"lifecycle/cleanup", "int f(void){int x;defer(void)0;return x;}", NULL, {0}, O_LIFECYCLE,
	 0, FB_LINE, 0, NULL, NULL, NULL, 0, "TCITCIT"},
	{"lifecycle/null-free", "int f(void){return 0;}", NULL, {0}, O_LIFECYCLE,
	 0, FB_LINE, 0, NULL, NULL, NULL, 0, "FT"},

	{"cli/help", NULL, NULL, {0}, O_CLI, 0, 0, 0, NULL, NULL, NULL, 0, NULL, av_help, 2,
	 CLI_DEFAULT, CLI_ACT_HELP, 0, 0},
	{"cli/version", NULL, NULL, {0}, O_CLI, 0, 0, 0, NULL, NULL, NULL, 0, NULL, av_version, 2,
	 CLI_DEFAULT, CLI_ACT_VERSION, 0, 0},
	{"cli/run", NULL, NULL, {0}, O_CLI, 0, 0, 0, NULL, NULL, NULL, 0, NULL, av_run, 6,
	 CLI_RUN, CLI_ACT_NONE, 1, 0},
	{"cli/emit", NULL, NULL, {0}, O_CLI, 0, 0, 0, NULL, NULL, NULL, 0, NULL, av_emit, 3,
	 CLI_EMIT, CLI_ACT_NONE, 1, 0},
	{"cli/emit-file", NULL, NULL, {0}, O_CLI, 0, 0, 0, "o.c", NULL, NULL, 0, NULL, av_emit_file, 3,
	 CLI_EMIT, CLI_ACT_NONE, 1, 0},
	{"cli/check", NULL, NULL, {0}, O_CLI, 0, 0, 0, NULL, NULL, NULL, 0, NULL, av_check, 4,
	 CLI_CHECK, CLI_ACT_NONE, 1, 0},
	{"cli/feature-off", NULL, NULL, {0}, O_CLI, 0, FB_DEFER | FB_ORELSE | FB_ZERO, 0,
	 NULL, NULL, NULL, 0, NULL, av_flags, 5, CLI_DEFAULT, CLI_ACT_NONE, 1, 0},
	{"cli/bounds", NULL, NULL, {0}, O_CLI, FB_BOUNDS, 0, 0, NULL, NULL, NULL, 0, NULL,
	 av_bounds, 3, CLI_DEFAULT, CLI_ACT_NONE, 1, 0},
	{"cli/compile", NULL, NULL, {0}, O_CLI, 0, 0, 0, NULL, NULL, NULL, 0, NULL,
	 av_compile, 5, CLI_DEFAULT, CLI_ACT_NONE, 1, 1},
	{"cli/assemble", NULL, NULL, {0}, O_CLI, 0, 0, 0, NULL, NULL, NULL, 0, NULL,
	 av_assemble, 5, CLI_DEFAULT, CLI_ACT_NONE, 1, 1},
	{"cli/verbose", NULL, NULL, {0}, O_CLI, 0, 0, 0, NULL, NULL, NULL, 0, NULL,
	 av_verbose, 3, CLI_DEFAULT, CLI_ACT_NONE, 1, 0},
	{"cli/verify", NULL, NULL, {0}, O_CLI, 0, 0, 0, NULL, NULL, NULL, 0, NULL,
	 av_verify, 3, CLI_DEFAULT, CLI_ACT_NONE, 1, 0},
	{.id="cli/install", .oracle=O_CLI, .argv=av_install, .argc=N(av_install), .cli_mode=CLI_INSTALL, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1},
	{.id="cli/stdin", .oracle=O_CLI, .argv=av_dash, .argc=N(av_dash), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1},
	{.id="cli/preprocessed", .oracle=O_CLI, .argv=av_i, .argc=N(av_i), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1},
	{.id="cli/msvc-joined", .oracle=O_CLI, .argv=av_msvc, .argc=N(av_msvc), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1},
	{.id="cli/msvc-separated", .oracle=O_CLI, .argv=av_msvc_sep, .argc=N(av_msvc_sep), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1},
	{.id="cli/passthrough-E", .oracle=O_CLI, .argv=av_pass_e, .argc=N(av_pass_e), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1},
	{.id="cli/passthrough-M", .oracle=O_CLI, .argv=av_pass_m, .argc=N(av_pass_m), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1},
	{.id="cli/passthrough-MM", .oracle=O_CLI, .argv=av_pass_mm, .argc=N(av_pass_mm), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1},
	{.id="cli/compiler", .oracle=O_CLI, .argv=av_cc, .argc=N(av_cc), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1},
	{.id="cli/profile", .oracle=O_CLI, .argv=av_prof, .argc=N(av_prof), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1},
	{.id="cli/short-help", .oracle=O_CLI, .argv=av_short_help, .argc=N(av_short_help), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_HELP, .cli_sources=-1, .cli_cc_args=-1},
	{.id="cli/emit-joined", .oracle=O_CLI, .argv=av_emit_joined, .argc=N(av_emit_joined), .cli_mode=CLI_EMIT, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1, .must_have="out.c"},
	{.id="cli/output-joined", .oracle=O_CLI, .argv=av_output_joined, .argc=N(av_output_joined), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1, .must_have="out"},
	{.id="cli/features-on", .oracle=O_CLI, .set_features=FB_DEFER|FB_ZERO|FB_LINE|FB_FLAT|FB_ORELSE|FB_AUR|FB_AS|FB_BOUNDS, .argv=av_features_on, .argc=N(av_features_on), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1},
	{.id="cli/features-more", .oracle=O_CLI, .argv=av_features_more, .argc=N(av_features_more), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1},
	{.id="cli/dependencies", .oracle=O_CLI, .argv=av_dep, .argc=N(av_dep), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=-1, .cli_cc_args=-1},
	{.id="cli/x-c", .oracle=O_CLI, .argv=av_x_c, .argc=N(av_x_c), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=1, .cli_cc_args=-1},
	{.id="cli/x-header", .oracle=O_CLI, .argv=av_x_header, .argc=N(av_x_header), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=1, .cli_cc_args=-1},
	{.id="cli/x-cpp", .oracle=O_CLI, .argv=av_x_cpp, .argc=N(av_x_cpp), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=1, .cli_cc_args=-1},
	{.id="cli/x-none", .oracle=O_CLI, .argv=av_x_none, .argc=N(av_x_none), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=0, .cli_cc_args=-1},
	{.id="cli/arg-flags", .oracle=O_CLI, .argv=av_arg_flags, .argc=N(av_arg_flags), .cli_mode=CLI_DEFAULT, .cli_action=CLI_ACT_NONE, .cli_sources=1, .cli_cc_args=-1},
	{.id="cli/run-separator", .oracle=O_CLI, .argv=av_run_sep, .argc=N(av_run_sep), .cli_mode=CLI_RUN, .cli_action=CLI_ACT_NONE, .cli_sources=1, .cli_cc_args=-1},
	{.id="cli/rsp-basic", .source="-fbounds-check x.c\n", .oracle=O_CLI,
	 .requires=CAP_POSIX, .argv=av_rsp, .argc=2, .cli_mode=CLI_DEFAULT,
	 .cli_action=CLI_ACT_NONE, .cli_sources=1, .cli_cc_args=0, .set_features=FB_BOUNDS},
	{.id="cli/rsp-double-quote", .source="-DNAME=\"a b\" \"x.c\"\n", .oracle=O_CLI,
	 .requires=CAP_POSIX, .argv=av_rsp, .argc=2, .cli_mode=CLI_DEFAULT,
	 .cli_action=CLI_ACT_NONE, .cli_sources=1, .cli_cc_args=1},
	{.id="cli/rsp-single-quote", .source="-DNAME='a b' 'x.c'\n", .oracle=O_CLI,
	 .requires=CAP_POSIX, .argv=av_rsp, .argc=2, .cli_mode=CLI_DEFAULT,
	 .cli_action=CLI_ACT_NONE, .cli_sources=1, .cli_cc_args=1},
	{.id="cli/rsp-escaped", .source="-DNAME=a\\ b x.c\r\n", .oracle=O_CLI,
	 .requires=CAP_POSIX, .argv=av_rsp, .argc=2, .cli_mode=CLI_DEFAULT,
	 .cli_action=CLI_ACT_NONE, .cli_sources=1, .cli_cc_args=1},
	/* --- 1.1.7 regressions: defer return declarators, directive-line
	 * splices, wrap-paren orelse actions, and GNU label declarations. --- */
	{"runtime/defer-return-declarator", "@0@", NULL, {&ax_defer_return_declarator},
	 O_OK | O_FIXED | O_COMPILE | O_NO_EXT, 0, FB_LINE, CAP_POSIX},
	/* Compiler-independent half of the same contract: the synthesized typedef
	 * must carry the returned function type's parameter list. The source
	 * spelling has no spaces inside its parentheses, so these fragments can
	 * only come from the typedef. */
	{"exact/defer-return-fnptr-params",
	 "static int g(int a){return a;}int (*f(void))(int){defer (void)0;return g;}", NULL,
	 {0}, O_OK, 0, FB_LINE, 0, ") ( int );", NULL},
	{"exact/defer-return-fnptr-two-params",
	 "static int g(int a,char *b){(void)b;return a;}"
	 "int (*f(void))(int,char *){defer (void)0;return g;}", NULL,
	 {0}, O_OK, 0, FB_LINE, 0, ") ( int , char * );", NULL},
	{"exact/defer-return-fnptr-of-fnptr",
	 "static int i2(int a){return a;}static int (*g(char c))(int){(void)c;return i2;}"
	 "int (*(*f(void))(char))(int){defer (void)0;return g;}", NULL,
	 {0}, O_OK, 0, FB_LINE, 0, ") ( char ) ) ( int );", NULL},
	{"exact/defer-return-array-ptr-dim",
	 "static int a[4];int (*f(void))[4]{defer (void)0;return &a;}", NULL,
	 {0}, O_OK, 0, FB_LINE, 0, ") [ 4 ];", NULL},
	/* Without a terminating `;` the return value expression used to swallow
	 * the rest of the translation unit into the temporary's initializer. */
	{"reject/return-value-missing-semicolon",
	 "void g(void);int f(void){defer g();return 0}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "missing ';' after return value"},
	{"runtime/defer-after-directive", "@0@", NULL, {&ax_defer_after_directive},
	 O_OK | O_RUN | O_FIXED | O_NO_EXT, 0, FB_LINE, CAP_POSIX},
	/* Macro-hygiene wrap parens around a declaration initializer are stripped
	 * before emission, so an empty action inside them reaches Pass 2 as the
	 * bare form the unwrapped scan already rejects, and lowered to `x ? x :;`. */
	{"reject/orelse-empty-action-wrapped",
	 "int g(void);void f(void){int z=(g() orelse);(void)z;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "expected statement after 'orelse'"},
	{"reject/orelse-empty-action-wrapped-chain",
	 "int g(void);int h(void);void f(void){int z=(g() orelse h() orelse);(void)z;}", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "expected statement after 'orelse'"},
	/* Positive control: the wrap-paren value form still lowers. */
	{"exact/orelse-wrapped-value",
	 "int g(void);void f(void){int z=(g() orelse 5);(void)z;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "z = z ? z : 5", NULL},
	{"exact/orelse-wrapped-value-chain",
	 "int g(void);int h(void);void f(void){int z=(g() orelse h() orelse 5);(void)z;}", NULL,
	 {0}, O_OK | O_COMPILE, 0, FB_LINE, CAP_POSIX, "z = z ? z : 5", NULL},
	/* An unterminated GNU label declaration emitted its tokens and only then
	 * reported "not handled", so the caller emitted them a second time and
	 * everything to EOF was duplicated. Duplication breaks the fixed point. */
	{"exact/gnu-label-decl-unterminated",
	 "int f(void){\n__label__ L\n}\n", NULL,
	 {0}, O_OK | O_FIXED, 0, FB_LINE, 0},
	{"runtime/gnu-label-decl-scoped",
	 "int main(void){int t=0;{__label__ again;int i=0;again:if(++i<3)goto again;t+=i;}"
	 "{__label__ again;int j=0;again:if(++j<5)goto again;t+=j;}return t==8?0:1;}", NULL,
	 {0}, O_OK | O_RUN | O_FIXED | O_NO_EXT, 0, FB_LINE, CAP_GNU | CAP_POSIX},
	/* The retired `collect_source_defines` continuation tests asserted that a
	 * `#define` whose value spans backslash-continued physical lines is
	 * re-emitted with every chunk joined -- the original defect lost the name
	 * to a getline realloc and dropped later chunks. The corpus capture of
	 * those tests (api-214, api-215) kept only the status, which is not what
	 * they checked, so the joining contract was uncovered. Restore it against
	 * the re-emission path directly: no include, so `emit_consumed_defines`
	 * always runs. */
	{"exact/define-continuation-two-line",
	 "#define FOB \\\n    64\nint f(void){return FOB;}", NULL,
	 {0}, O_OK | O_FILE | O_COMPILE, 0, FB_FLAT | FB_LINE, CAP_POSIX,
	 "#define FOB 64", NULL},
	{"exact/define-continuation-multi-line",
	 "#define MULTI_VAL (1 \\\n    | 2 \\\n    | 4)\nint f(void){return MULTI_VAL;}", NULL,
	 {0}, O_OK | O_FILE | O_COMPILE, 0, FB_FLAT | FB_LINE, CAP_POSIX,
	 "#define MULTI_VAL (1 | 2 | 4)", NULL},
	{"exact/define-continuation-tab-indent",
	 "#define TABBED (1 \\\n\t| 2)\nint f(void){return TABBED;}", NULL,
	 {0}, O_OK | O_FILE | O_COMPILE, 0, FB_FLAT | FB_LINE, CAP_POSIX,
	 "#define TABBED (1 | 2)", NULL},
	/* Symmetric with the return-value walk: an orelse fallback with no
	 * terminating `;` copied the rest of the block into `LHS = ( ... )`. */
	{"reject/orelse-fallback-missing-semicolon",
	 "int g(void); void f(void){ int x; x = g() orelse 5 }", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "missing ';' after orelse fallback value"},
	/* A `__label__` declaration is a statement. Phase 1D handed on the
	 * `__label__` token itself as the previous token, so a `defer` directly
	 * after one looked like expression context and was rejected -- valid GNU C
	 * plus valid Prism, refused. */
	{"runtime/defer-after-gnu-label-decl",
	 "static int calls;static void mark(void){calls++;}"
	 "static int g(int c){__label__ retry;defer mark();int n=0;"
	 "retry:if(++n<3)goto retry;return c?n:-n;}"
	 "int main(void){int v=g(1);return (v==3&&calls==1)?0:1;}", NULL,
	 {0}, O_OK | O_RUN | O_FIXED | O_NO_EXT, 0, FB_LINE, CAP_GNU | CAP_POSIX},
	{"exact/defer-after-gnu-label-decl-multi",
	 "void k(void);void f(void){__label__ L, M; defer k(); L:M:;}", NULL,
	 {0}, O_OK | O_NO_EXT, 0, FB_LINE, 0, "k();", NULL},
	/* A GNU label declaration carries no type tag, so the braceless-body
	 * declaration check never saw it and the scan emitted a stray `}`. */
	{"reject/gnu-label-decl-as-braceless-defer-body",
	 "void k(void);void f(void){ defer __label__ L, k(); }", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL,
	 "'__label__' declaration as a braceless defer body"},
	/* Translation phase 1 belongs to whoever preprocesses. `cc -E` already
	 * applied the backend's trigraph policy -- off by default -- so re-running
	 * it over the preprocessed stream rewrote literals the compiler had
	 * deliberately kept. The file API is the path that preprocesses, so the
	 * oracle has to be O_FILE; the executable check compares against the
	 * length the backend itself would produce. */
	{"runtime/trigraph-policy-follows-backend",
	 /* Split every `??!` across two string literals: concatenation happens in
	  * translation phase 6, long after phase 1, so the trigraph is never formed
	  * while compiling this file. Spelled literally it warns under Clang and is
	  * silently rewritten to `what|` by any compiler with trigraphs enabled,
	  * which would corrupt the very data this cell exists to check. */
	 "int main(void){ const char *s = \"what?" "?!\"; return (sizeof(\"what?" "?!\")-1==7 && s[4]=='?')?0:1; }",
	 NULL, {0}, O_OK | O_FILE | O_RUN | O_NO_EXT, 0, FB_LINE, CAP_POSIX},
	/* Raw source has no other front end, so Prism still owns phases 1 and 2
	 * there: the spliced-keyword product above must keep working. */
	{"runtime/trigraph-splice-still-applies-to-raw-source",
	 "static int calls;static void mark(void){calls++;}"
	 "int main(void){de?" "?/\nfer mark();return calls==0?0:1;}", NULL,
	 {0}, O_OK | O_RUN | O_FIXED | O_NO_EXT, 0, FB_LINE, CAP_POSIX},
	/* Phase 1 can leave the bare-orelse recipe bit set with no recorded `=`.
	 * Every emission below walked from that pointer, so `pparse_next(NULL)`
	 * dereferenced a null token -- caught by UBSan in the fuzz target. */
	{"reject/bare-orelse-without-assignment",
	 "int main(void){ int a[5]={0}; int i=2; free orelse { (void)a[i]; } return 0; }", NULL,
	 {0}, O_ANY_STATUS | O_REPLAY, 0, FB_LINE, 0},
	/* Extension elimination is now enforced at emission rather than assumed.
	 * Each of these used to be accepted, with the keyword copied verbatim into
	 * the generated C: the backend then failed somewhere unrelated, or -- for
	 * the defer case -- silently compiled a duplicated translation unit. All
	 * three inputs are rejected by a C compiler on their own merits. */
	{"reject/defer-not-lowered-unterminated-body",
	 "void task_a(void){ defer { int x = 5 }; }\nvoid f(void);\nvoid task_b(void){ defer f(); }\n", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "was not lowered"},
	{"reject/orelse-not-lowered-attribute-before-if-paren",
	 "int *get(void);int main(void){int *p;if __attribute__((hot)) (1) p = get() orelse 0;(void)p;return 0;}",
	 NULL, {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0, NULL, NULL, "was not lowered"},
	/* The three constructs that legitimately emit the spelling must keep
	 * working: a shadowing identifier, a raw suppress block, and the keyword's
	 * own feature switched off. */
	{"exact/raw-block-keeps-defer-verbatim",
	 "void c(void);void f(void){ raw { defer c(); } }", NULL,
	 {0}, O_OK, 0, FB_LINE, 0, "defer c();", NULL},
	{"exact/no-orelse-keeps-orelse-verbatim",
	 "int g(void);int f(void){const int x=g() orelse 7;return x;}", NULL,
	 {0}, O_OK, 0, FB_ORELSE | FB_LINE, 0, "orelse 7", NULL},
	{"exact/no-defer-keeps-defer-verbatim",
	 "void c(void);int f(void){int x=0;defer c();return x;}", NULL,
	 {0}, O_OK, 0, FB_DEFER | FB_LINE, 0, "defer c();", NULL},
	{"runtime/shadowed-defer-identifier-still-emitted",
	 "typedef int defer;int main(void){defer d=7;return d==7?0:1;}", NULL,
	 {0}, O_OK | O_RUN | O_FIXED, 0, FB_LINE, CAP_POSIX, "defer d", NULL},
	/* `int (*p)[4]` carries its extent in the type, so the inner subscript is
	 * checkable even though `p` is a pointer. `*p` and `p[0]` denote the same
	 * object, so both spellings bound against `sizeof(p[0])/sizeof(p[0][0])`.
	 * The outer subscript must stay unchecked: nothing says how many arrays
	 * `p` points at. */
	{"runtime/bounds-ptr-to-array-inbounds",
	 "int main(void){int a[4]={10,20,30,40};int (*p)[4]=&a;volatile int i=3;"
	 "return (p[0][i]==40 && (*p)[i]==40 && (*(p))[i]==40)?0:1;}", NULL,
	 {0}, O_OK | O_RUN | O_FIXED | O_COMPILE, FB_BOUNDS, FB_LINE, CAP_POSIX},
	{"runtime/bounds-ptr-to-array-traps-subscript",
	 "int main(void){int a[4]={0};int (*p)[4]=&a;volatile int i=4;return p[0][i];}", NULL,
	 {0}, O_OK | O_TRAP, FB_BOUNDS, FB_LINE, CAP_POSIX},
	{"runtime/bounds-ptr-to-array-traps-deref",
	 "int main(void){int a[4]={0};int (*p)[4]=&a;volatile int i=4;return (*p)[i];}", NULL,
	 {0}, O_OK | O_TRAP, FB_BOUNDS, FB_LINE, CAP_POSIX},
	{"exact/bounds-ptr-to-array-bound-shape",
	 "int f(int i){int a[4]={0};int (*p)[4]=&a;return p[0][i];}", NULL,
	 {0}, O_OK, FB_BOUNDS, FB_LINE, 0, "sizeof(p[0])/sizeof(p[0][0])", NULL},
	{"exact/bounds-ptr-to-array-deref-bound-shape",
	 "int f(int i){int a[4]={0};int (*p)[4]=&a;return (*p)[i];}", NULL,
	 {0}, O_OK, FB_BOUNDS, FB_LINE, 0, "sizeof(p[0])/sizeof(p[0][0])", NULL},
	/* The pointer hop itself has no extent, so `p[i]` must not be wrapped --
	 * `sizeof(p)/sizeof(p[0])` would be 0 and trap on every access. */
	{"exact/bounds-ptr-to-array-outer-unchecked",
	 "int f(int i){int a[4]={0};int (*p)[4]=&a;return p[i][0];}", NULL,
	 {0}, O_OK, FB_BOUNDS, FB_LINE, 0, NULL, "sizeof(p)/sizeof(p[0])"},
	/* C11 6.7.6.3p7: `int a[static N]` promises the argument points at at least
	 * N elements. That promise is the only extent an array parameter has -- it
	 * decays to a pointer, so a sizeof ratio would measure the pointer. The
	 * bound is the literal N. */
	{"runtime/bounds-static-param-inbounds",
	 "static int f(int a[static 4],int i){return a[i];}"
	 "int main(void){int a[6]={1,2,3,4,5,6};volatile int i=3;return f(a,i)==4?0:1;}", NULL,
	 {0}, O_OK | O_RUN | O_FIXED | O_COMPILE, FB_BOUNDS, FB_LINE, CAP_POSIX},
	{"runtime/bounds-static-param-traps",
	 "static int f(int a[static 4],int i){return a[i];}"
	 "int main(void){int a[6]={0};volatile int i=4;return f(a,i);}", NULL,
	 {0}, O_OK | O_TRAP, FB_BOUNDS, FB_LINE, CAP_POSIX},
	{"exact/bounds-static-param-literal-extent",
	 "int f(int a[static 4],int i){return a[i];}", NULL,
	 {0}, O_OK, FB_BOUNDS, FB_LINE, 0, "(__prism_bchk_size_t)(4)", "sizeof(a)/sizeof(a[0])"},
	{"exact/bounds-static-param-qualified",
	 "int f(int a[static const 4],int i){return a[i];}", NULL,
	 {0}, O_OK, FB_BOUNDS, FB_LINE, 0, "(__prism_bchk_size_t)(4)", NULL},
	/* An array parameter without the promise has no extent at all and must stay
	 * unwrapped: `sizeof(a)/sizeof(a[0])` would measure the pointer. */
	{"exact/bounds-plain-array-param-unchecked",
	 "int f(int a[4],int i){return a[i];}", NULL,
	 {0}, O_OK, FB_BOUNDS, FB_LINE, 0, NULL, "a[__prism_bchk"},
	{"exact/bounds-pointer-param-unchecked",
	 "int f(int *a,int i){return a[i];}", NULL,
	 {0}, O_OK, FB_BOUNDS, FB_LINE, 0, NULL, "a[__prism_bchk"},
	/* `-fno-orelse` means the word is an ordinary identifier. Every rejection
	 * in the initializer scanner is about how the *operator* may be spelled, so
	 * with the feature off none of them applies -- an undeclared identifier is
	 * the backend's to complain about, in its own words. Diagnosing it here
	 * also stopped Prism reparsing its own output whenever an input used the
	 * name as a plain identifier. */
	{"exact/no-orelse-identifier-passes-through",
	 "int f(int w){ int j = w || orelse; return j; }", NULL,
	 {0}, O_OK, 0, FB_ORELSE | FB_LINE, 0, "w || orelse", NULL},
	{"runtime/no-orelse-declared-identifier",
	 "int orelse = 7;int main(void){int j = 0 || orelse;return j==1?0:1;}", NULL,
	 {0}, O_OK | O_RUN | O_FIXED, 0, FB_ORELSE | FB_LINE, CAP_POSIX},
	/* With the feature on, the operator's placement rules still hold. */
	{"reject/orelse-operator-still-diagnosed",
	 "int g(void);void f(void){ int x = int orelse 1; (void)x; }", NULL,
	 {0}, O_REJECT | O_DIAG, 0, FB_LINE, 0},
	/* No preprocessor expands a `#pragma`'s operands, so a macro named there is
	 * the one define flattening cannot drop: the emitted pragma still says
	 * `PRISM_PK`, and without the define a backend that does expand operands at
	 * compile time sees a malformed pragma where the source had a working one.
	 * A define that follows a header cannot be hoisted above it -- it may be
	 * completing or shadowing what the header set up -- so it is emitted in
	 * place, immediately before the pragma that needs it. That splice lands
	 * between ordinary tokens, which is why the after-include cells carry
	 * O_COMPILE: the first version of it glued `#define` onto the tail of the
	 * preceding line and every one of these stopped compiling. */
	{.id = "pragma/define-before-include-survives",
	 .source = "#define PRISM_PK 1\n"
		   "#pragma pack(push, PRISM_PK)\n"
		   "struct prism_pk_s { char c; int x; };\n"
		   "#pragma pack(pop)\n"
		   "int f(void){ return (int)sizeof(struct prism_pk_s); }",
	 .oracle = O_OK | O_FILE | O_COMPILE, .requires = CAP_POSIX,
	 .must_have = "#define PRISM_PK 1"},
	{.id = "pragma/define-after-include-survives",
	 .source = "#include <limits.h>\n"
		   "#define PRISM_PK 1\n"
		   "#pragma pack(push, PRISM_PK)\n"
		   "struct prism_pk_s { char c; int x; };\n"
		   "#pragma pack(pop)\n"
		   "int f(void){ return (int)sizeof(struct prism_pk_s); }",
	 .oracle = O_OK | O_FILE | O_COMPILE, .requires = CAP_POSIX,
	 .must_have = "#define PRISM_PK 1"},
	{.id = "pragma/define-after-include-two-pragmas",
	 .source = "#include <limits.h>\n"
		   "#define PRISM_PK 1\n"
		   "#pragma pack(push, PRISM_PK)\n"
		   "struct prism_pk_s { char c; int x; };\n"
		   "#pragma pack(pop)\n"
		   "#pragma pack(push, PRISM_PK)\n"
		   "struct prism_pk_t { char c; double d; };\n"
		   "#pragma pack(pop)\n"
		   "int f(void){ return (int)(sizeof(struct prism_pk_s) + "
		   "sizeof(struct prism_pk_t)); }",
	 .oracle = O_OK | O_FILE | O_COMPILE, .requires = CAP_POSIX,
	 .must_have = "#define PRISM_PK 1"},
	/* The narrowness is the point: flattening still drops every source define
	 * no surviving pragma names. Without this cell the fix above could be
	 * "hoist everything" and nothing would notice. */
	{.id = "pragma/unreferenced-define-still-dropped",
	 .source = "#include <limits.h>\n"
		   "#define PRISM_UNREFERENCED_PK 1\n"
		   "#pragma pack(push, 1)\n"
		   "struct prism_pk_s { char c; int x; };\n"
		   "#pragma pack(pop)\n"
		   "int f(void){ return (int)sizeof(struct prism_pk_s); }",
	 .oracle = O_OK | O_FILE | O_COMPILE, .requires = CAP_POSIX,
	 .must_not_have = "PRISM_UNREFERENCED_PK"},
	{"internal/action-letters-unique", NULL, NULL, {0}, O_INTERNAL, 0, 0, CAP_POSIX,
	 NULL, NULL, NULL, 0, "s"},
	{"internal/header-cache", NULL, NULL, {0}, O_INTERNAL, 0, 0, CAP_POSIX,
	 NULL, NULL, NULL, 0, "h"},
	{"internal/pragma-define-position", NULL, NULL, {0}, O_INTERNAL, 0, 0, CAP_POSIX,
	 NULL, NULL, NULL, 0, "p"},
	{"internal/spawn-refusal-relayed", NULL, NULL, {0}, O_INTERNAL, 0, 0, CAP_POSIX,
	 NULL, NULL, NULL, 0, "r"},
	{"internal/spawn-classification", NULL, NULL, {0}, O_INTERNAL, 0, 0, CAP_POSIX,
	 NULL, NULL, NULL, 0, "q"},
	{"internal/cache-cleanup", NULL, NULL, {0}, O_INTERNAL, 0, 0, CAP_POSIX,
	 NULL, NULL, NULL, 0, "X"},
};

int main(void) {
	Stats st = {0};
	recipe_run(recipes, N(recipes), &st);
	printf("PRISM RECIPES: %ld cells, %ld passed, %ld failed, %ld skipped\n",
	       st.cells, st.passed, st.failed, st.skipped);
	/* Surfaced deliberately: a run that had to retry spawns was fighting the
	 * machine, and any failure it reports should be read in that light. */
	if (slow_clock_skips)
		fprintf(stderr,
			"NOTE: %ld sub-second file-identity check(s) skipped: this host could not "
			"write, preprocess and rewrite inside one clock second\n",
			slow_clock_skips);
	if (action_lint_skips)
		fprintf(stderr,
			"NOTE: %ld action-letter lint(s) skipped: the suite source was not "
			"found relative to the working directory\n",
			action_lint_skips);
	if (spawn_refusal_skips)
		fprintf(stderr,
			"NOTE: %ld spawn-refusal check(s) skipped: this host would not let the "
			"suite make a spawn fail\n",
			spawn_refusal_skips);
	if (infra_retries)
		fprintf(stderr,
			"NOTE: %ld process spawn(s) were refused by the machine and retried "
			"(low memory or process limits); this is not a verdict on prism\n",
			infra_retries);
	if (st.failed) fprintf(stderr, "FIRST FAILURE: %s\n", st.first);
	prism_thread_cleanup();
	return st.failed ? 1 : 0;
}

/*
 * Prism's entire executable test suite is data below. There is one case
 * generator (recipe_run): it expands zero to four axes with an odometer,
 * renders the selected values into a source template, and sends every cell
 * through the same oracle pipeline.  New coverage is a row, never a new test
 * function.
 */
#define PRISM_LIB_MODE
#include "../prism.c"

#include <stdarg.h>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

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

#ifndef _WIN32
static const char *backend_cc(void) {
	const char *cc = getenv("CC");
	if (!cc || !*cc || strpbrk(cc, " \t\"'`$;&|<>()[]{}*?!#~\n")) return "cc";
	return cc;
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
	int rc = system(cmd);
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
		if (ok && (r->oracle & O_COMPILE)) ok = compile_output(emitted, 0) == 0;
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

static int run_internal(const Recipe *r, char *failed_action) {
	int ok = 1;
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
		} else if (*p == 'X') {
			const char *dir = pp_cache_dir();
			long removed = 0;
			char marker[PATH_MAX];
			pp_each_entry(pp_clear_cb, &removed);
			snprintf(marker, sizeof marker, "%s/%s", dir, PP_PRUNE_MARKER);
			unlink(marker);
			ok = ok && rmdir(dir) == 0;
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
		} else if (*p == 'D') {
			char **names = NULL;
			bool *enabled = NULL, undef_gnu = false;
			int n = 0, cap = 0;
			emit_consumed_def_upsert(&names, &enabled, &n, &cap, "", true, &undef_gnu);
			for (int i = 0; i < 20; i++) {
				char spec[32];
				snprintf(spec, sizeof spec, "RECIPE_%d=%d", i, i);
				emit_consumed_def_upsert(&names, &enabled, &n, &cap, spec, true, &undef_gnu);
			}
			emit_consumed_def_upsert(&names, &enabled, &n, &cap, "RECIPE_3", false, &undef_gnu);
			emit_consumed_def_upsert(&names, &enabled, &n, &cap, "_GNU_SOURCE", false, &undef_gnu);
			ok = ok && n == 21 && !enabled[3] && undef_gnu;
			for (int i = 0; i < n; i++) free(names[i]);
			free(names);
			free(enabled);
		} else if (*p == 'A') {
			/* The stdin preprocessor path must always carry an explicit language.
			 * A user-provided `-x none` means extension guessing, which is impossible
			 * for `-`, so it is normalized to C instead of being passed through. */
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
				     _ps->source_defines && !strcmp(_ps->source_defines[0], scans[i].want);
				if (!ok && getenv("PRISM_INTERNAL_TRACE"))
					fprintf(stderr, "scan %zu: count=%d value=%s want=%s\n", i,
						_ps->source_define_count,
						_ps->source_define_count ? _ps->source_defines[0] : "-", scans[i].want);
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
					     !strcmp(_ps->source_defines[0], "DEEP=41") &&
					     _ps->source_define_guards && _ps->source_define_guards[0] &&
					     count_kw(_ps->source_define_guards[0], "if") == 40;
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
				ok = ok && transpile_to_stdout(in);
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
		} else if (*p == 'N') {
			PrismResult z = prism_transpile_file(NULL, prism_defaults());
			ok = ok && z.status == PRISM_ERR_IO && z.error_msg && strstr(z.error_msg, "NULL");
			prism_free(&z);
		}
		if (before && !ok && failed_action && !*failed_action) *failed_action = *p;
		if (getenv("PRISM_INTERNAL_TRACE"))
			fprintf(stderr, "internal %c: %s%s\n", *p, ok ? "ok" : "failed",
				before ? "" : " (earlier failure)");
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
		unlink(path);
#else
		st->skipped++;
		free(src);
		return;
#endif
	} else {
		x = prism_transpile_source(src, "recipe.c", f);
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
	for (size_t ri = 0; ri < count; ri++) {
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
			for (size_t i = 0; i < dims; i++) sel[i] = &r->axes[i]->values[idx[i]];
			long before = st->failed, skipped_before = st->skipped;
			st->cells++;
			run_source_cell(r, sel, st->cells, st);
			if (st->failed == before && st->skipped == skipped_before) st->passed++;
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
	 NULL, NULL, NULL, 0, "KCSPODAFJTN"},
	{"api/msvc-first-call-prologue", "int f(void){int a[1];return a[0];}", NULL,
	 {&ax_msvc_target}, O_OK, 0, FB_LINE, 0,
	 "#pragma warning(push, 0)|#pragma warning(pop)", "#pragma GCC diagnostic"},
	{"contexts/expression", "@2@", PRE, {&ax_expr, &ax_expr_wrap, &ax_expr_ctx, &ax_features}, O_TRICHOTOMY, 0, FB_LINE, 0},
	{"contexts/statement", "@1@", NULL, {&ax_stmt, &ax_stmt_ctx, &ax_features}, O_TRICHOTOMY, 0, FB_LINE, 0},
	{"declarations/product", "@1@", NULL, {&ax_decl, &ax_decl_ctx, &ax_features}, O_OK | O_FIXED, 0, FB_LINE, 0},
	{"features/product", "int g(void);void c(void);int f(int i){int a[8];defer c();int x=g() orelse 2;raw int y;return a[i]+x+y;}",
	 NULL, {&ax_features}, O_TRICHOTOMY, FB_BOUNDS | FB_AS | FB_AUR, FB_LINE, 0},
	{"api/feature-struct", "#ifdef RECIPE_DEF\nint v=RECIPE_DEF;\n#else\nint v;\n#endif\n",
	 NULL, {&ax_api_features}, O_FILE | O_ANY_STATUS, 0, FB_LINE, CAP_POSIX},
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
	 "#ifndef SUM\n|#define SUM(a,b) ((a)+(b))", "#ifndef SUM(a,b)"},
	{"runtime/passthrough-equivalence", "@0@", NULL, {&ax_passthrough},
	 O_OK | O_OUTPUT_EQ_OFF | O_REFERENCE_RUN, 0, FB_LINE, CAP_POSIX},
	{"corpus/retired-regressions", "@0@", NULL, {&ax_corpus},
	 O_ANY_STATUS | O_REPLAY, 0, FB_LINE, 0},
	{"bounds/product", "@1@", NULL, {&ax_bounds, &ax_bounds_ctx}, O_TRICHOTOMY, FB_BOUNDS, FB_LINE, 0},
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
	{"runtime/auto-static-product", "@0@", NULL, {&ax_runtime_autostatic}, O_OK | O_RUN,
	 FB_AS, FB_LINE, CAP_POSIX},

	{"exact/defer-lifo", "void p(char);int main(void){defer p('A');{defer p('B');p('x');}return 0;}", NULL,
	 {0}, O_OK | O_NO_EXT | O_FIXED, 0, FB_LINE, 0, "p('A')|p('B')", NULL},
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
	{"internal/cache-cleanup", NULL, NULL, {0}, O_INTERNAL, 0, 0, CAP_POSIX,
	 NULL, NULL, NULL, 0, "X"},
};

int main(void) {
	Stats st = {0};
	recipe_run(recipes, N(recipes), &st);
	printf("PRISM RECIPES: %ld cells, %ld passed, %ld failed, %ld skipped\n",
	       st.cells, st.passed, st.failed, st.skipped);
	if (st.failed) fprintf(stderr, "FIRST FAILURE: %s\n", st.first);
	prism_thread_cleanup();
	return st.failed ? 1 : 0;
}

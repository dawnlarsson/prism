#define PRISM_VERSION "1.1.6"

#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define INSTALL_PATH "/usr/local/bin/prism"
#define PRISM_DEFAULT_CC "cc"
#define EXE_SUFFIX ""
#define TMPDIR_ENVVAR "TMPDIR"
#define TMPDIR_FALLBACK "/tmp/"
#define FIND_EXE_CMD "which -a prism 2>/dev/null || command -v prism 2>/dev/null"
#endif

#ifdef PRISM_LIB_MODE
#define PRISM_API
#else
#define PRISM_API PRISM_MAYBE_UNUSED static
#endif

/* parse.c is self-contained: it includes its own headers and needs nothing
 * declared above this point. Keep it that way — it is consumed STB-style,
 * as a single file dropped into a host TU. */
#include "parse.c"

static char **build_clean_environ(void);
static const char *path_basename(const char *path);
static void signal_temps_register(const char *path);

static int run_command(char **argv);
static int run_command_quiet(char **argv);

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif
#ifndef _WIN32
#include <dirent.h> /* preprocessor-cache eviction sweep */
#endif

/* Driver and emitter state. These fields used to live in PParseContext, but
 * parse.c never read them - they were there only because prism.c needed
 * somewhere to put CLI flags, #line tracking, emit scratch and output state.
 * parse.c owns C-language logic; this owns everything that is merely how prism
 * drives it.
 *
 * Thread-local storage rather than a heap pointer: nothing here needs a
 * lifetime, and one _tlv_get_addr per function is what PPARSE_CTX already
 * costs. Zero-initialised by definition, so reset_transpiler_state only has to
 * clear what carries between translation units. */
typedef struct {
	int error_col;
	const char *extra_compiler;
	const char **extra_compiler_flags;
	int extra_compiler_flags_count;
	const char **extra_include_paths;
	int extra_include_count;
	const char **extra_defines;
	int extra_define_count;
	const char **extra_force_includes;
	int extra_force_include_count;
	const char **dep_flags; // -Wp,-MMD, -MD, -MF etc. (preprocessor-only)
	int dep_flags_count;
	int aggregate_member_nest; // `{` after struct/union/enum kw (expr/type contexts without scope push)
	int scope_depth;
	int block_depth;
	bool at_stmt_start;
	bool last_system_header;
	int last_line_no;
	char *last_filename;
	int system_include_count;
	int raw_block_depth; /* Pass 2: nest depth inside `raw { ... }` suppress blocks */
	unsigned long long ret_counter;
	unsigned *bracket_oe_ids;	   // Pre-assigned temp IDs for bracket orelse hoisting (dynamic)
	int bracket_oe_count;		   // Count of hoisted bracket orelse temps
	int bracket_oe_cap;		   // Capacity of bracket_oe_ids array
	int bracket_oe_next;		   // Next temp to consume during emit
	PParseToken **typeof_vars;
	int typeof_var_count;
	int typeof_var_cap;
	unsigned *bracket_dim_ids; // Temp IDs for non-orelse brackets (0 = not hoisted)
	int bracket_dim_count;	   // Count of pre-hoisted dimension temps
	int bracket_dim_cap;
	int bracket_dim_next; // Next dim temp to consume during emit
	char **source_defines;	     // Array of "NAME=VALUE" or "NAME" strings (malloc'd)
	char **source_define_guards; // Parallel: NULL (unconditional) or condition guard text (malloc'd)
	int source_define_count;
	int source_define_cap;
	char *active_membuf; // open_memstream buffer; freed on longjmp recovery
} PrismState;

static PRISM_THREAD_LOCAL PrismState prism_state_storage;
#define PRISM_STATE() PrismState *const _ps = &prism_state_storage
#define emit_scope_depth (prism_state_storage.scope_depth)
#define emit_block_depth (prism_state_storage.block_depth)
#define emit_at_stmt_start (prism_state_storage.at_stmt_start)

#define OUT_BUF_SIZE (128 * 1024)

#define emit_defers(mode) emit_defers_ex(mode, 0)
#define emit_all_defers() emit_defers(DEFER_ALL)
#define emit_goto_defers(depth) emit_defers_ex(DEFER_TO_DEPTH, depth)
#define has_active_defers() has_defers_for(DEFER_ALL, 0)
#define control_flow_has_defers(include_switch)                                                              \
	has_defers_for((include_switch) ? DEFER_BREAK : DEFER_CONTINUE, 0)
#define goto_has_defers(depth) has_defers_for(DEFER_TO_DEPTH, depth)
typedef struct {
	const char *compiler;
	const char **include_paths;  // -I paths
	const char **defines;	     // -D macros
	const char **compiler_flags; // Additional flags (-std=c99, -m32, etc.)
	const char **force_includes; // -include files
	int include_count;
	int define_count;
	int compiler_flags_count;
	int force_include_count;
	bool defer;
	bool zeroinit;
	bool line_directives;
	bool warn_safety;
	bool quiet; /* suppress Prism's own warnings (not the backend's) */
	bool flatten_headers;
	bool orelse;
	bool auto_unreachable;
	bool auto_static;
	bool bounds_check;
} PrismFeatures;

typedef enum {
	PRISM_OK = 0,
	PRISM_ERR_SYNTAX,
	PRISM_ERR_SEMANTIC,
	PRISM_ERR_IO,
} PrismStatus;

typedef struct {
	char *output;	 // transpiled C (caller frees with prism_free)
	char *error_msg; // pparse_error message (NULL on success)
	size_t output_len;
	int error_line;
	int error_col;
	PrismStatus status;
} PrismResult;

typedef enum {
	DEFER_SCOPE,	// DEFER_SCOPE=current only
	DEFER_ALL,	// DEFER_ALL=all scopes
	DEFER_BREAK,	// DEFER_BREAK=stop at loop/switch,
	DEFER_CONTINUE, // DEFER_CONTINUE=stop at loop,
	DEFER_TO_DEPTH	// DEFER_TO_DEPTH=stop at given depth (for goto)
} DeferEmitMode;

typedef struct {
	PParseToken *stmt, *end, *defer_kw;
} DeferEntry;

typedef enum {
	SCOPE_BLOCK,	  // { ... } block scope
	SCOPE_INIT,	  // = { ... } initializer brace (no block_depth increment)
	SCOPE_FOR_PAREN,  // for( ... ) — first ';' ends init, not stmt
	SCOPE_CTRL_PAREN, // if/while/switch( ... )
	SCOPE_GENERIC,	  // _Generic( ... )
	SCOPE_TERNARY,	  // ? ... : — popped on matching ':'
} ScopeKind;

static inline bool is_brace_scope(ScopeKind k) {
	return k == SCOPE_BLOCK || k == SCOPE_INIT;
}

typedef struct {
	int defer_start_idx;
	int saved_defer_shadow_count; // stmt-expr: shadow count at open time (restore
				      // on close)
	uint8_t kind;
	bool is_loop : 1;
	bool is_switch : 1;
	bool is_struct : 1;
	bool is_stmt_expr : 1;
	bool is_ctrl_se : 1; // stmt-expr inside ctrl parens (ctrl_state saved on ctrl_save_stack)
} ScopeNode;

typedef enum { CLI_DEFAULT, CLI_RUN, CLI_EMIT, CLI_INSTALL, CLI_CHECK } CliMode;

typedef enum { CLI_ACT_NONE, CLI_ACT_HELP, CLI_ACT_VERSION } CliAction;

typedef struct {
	PrismFeatures features;
	const char **sources;
	const char **cc_args;
	const char **dep_args;	// dependency-generation flags (routed to preprocessor only)
	const char **prog_args; // args passed to the compiled binary in `run` mode
				// (after `--`)
	const char *output;
	const char *cc;
	char **rsp_owned; // heap tokens from @file expansion (freed by cli_free)
	char **rsp_argv;  // expanded argv array; elements point into rsp_owned
	const char *check_tool; // `prism check <tool>`: analyzer executable
	char **check_args;	// tool args verbatim (sources substituted at spawn)
	int check_arg_count, check_arg_cap;
	int source_count, source_cap;
	int cc_arg_count, cc_arg_cap;
	int dep_arg_count, dep_arg_cap;
	int prog_arg_count, prog_arg_cap;
	int rsp_owned_count;
	CliMode mode;
	CliAction action;
	bool verbose;
	bool profile;
	bool verify; // --prism-verify: re-transpile emitted C, require fixed point
	bool compile_only;
	bool assemble_only; // -S: synthesize .s like -c synthesizes .o
	bool passthrough;
	bool no_link_pragma; // -fno-link-pragma: suppress #pragma link libs
	/* Language from `-x` that applied to the first Prism source (GCC: -x
	 * binds subsequent inputs until the next -x). NULL → default "c". */
	const char *source_x_lang;
	/* Index in cc_args of that `-x` / `-xLANG` (or -1). Stripped when
	 * building the stdin pipe so it is not double-applied. */
	int source_x_arg_idx;
} Cli;

extern char **environ;
static char **cached_clean_env = NULL;
static volatile sig_atomic_t signal_temp_registered = 0;
static char signal_temp_path[PATH_MAX];

#define SIGNAL_TEMPS_MAX 256
static char signal_temps[SIGNAL_TEMPS_MAX][PATH_MAX];
static volatile sig_atomic_t signal_temps_ready[SIGNAL_TEMPS_MAX];
static volatile sig_atomic_t signal_temps_count = 0;

#ifndef signal_temp_store // Windows: defined in windows.c
#define signal_temp_store(val) __atomic_store_n(&signal_temp_registered, (val), __ATOMIC_RELEASE)
#define signal_temp_load() __atomic_load_n(&signal_temp_registered, __ATOMIC_ACQUIRE)
#define signal_temps_store(val) __atomic_store_n(&signal_temps_count, (val), __ATOMIC_RELEASE)
#define signal_temps_load() __atomic_load_n(&signal_temps_count, __ATOMIC_ACQUIRE)
#define signal_temps_cas(expected, desired)                                                                  \
	__atomic_compare_exchange_n(                                                                         \
	    &signal_temps_count, (expected), (desired), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
#define cached_env_load() __atomic_load_n(&cached_clean_env, __ATOMIC_ACQUIRE)
#define cached_env_store(val) __atomic_store_n(&cached_clean_env, (val), __ATOMIC_RELEASE)
#define signal_temps_ready_store(idx, val)                                                                   \
	__atomic_store_n(&signal_temps_ready[(idx)], (val), __ATOMIC_RELEASE)
#define signal_temps_ready_load(idx) __atomic_load_n(&signal_temps_ready[(idx)], __ATOMIC_ACQUIRE)
#define signal_temps_ready_cas(idx, expected, desired)                                                       \
	__atomic_compare_exchange_n(                                                                         \
	    &signal_temps_ready[(idx)], (expected), (desired), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
#endif

static void signal_temps_register(const char *path) {
	size_t len = strlen(path);
	if (len >= PATH_MAX) return;
	sig_atomic_t n;
	do {
		n = signal_temps_load();
		if (n >= SIGNAL_TEMPS_MAX) {
			for (int i = 0; i < SIGNAL_TEMPS_MAX; i++) {
				sig_atomic_t expected = 0;
				if (signal_temps_ready_cas(i, &expected, 1)) {
					memcpy(signal_temps[i], path, len + 1);
					return;
				}
			}
			fprintf(stderr,
				"prism: warning: temp file tracking full (%d); "
				"'%s' won't be cleaned on signal\n",
				SIGNAL_TEMPS_MAX,
				path);
			return;
		}
	} while (!signal_temps_cas(&n, n + 1));
	memcpy(signal_temps[n], path, len + 1);
	signal_temps_ready_store(n, 1);
}

static void signal_temps_clear(void) {
	sig_atomic_t n = signal_temps_load();
	for (int i = 0; i < n; i++) {
		signal_temps_ready_store(i, 0);
		memset(signal_temps[i], 0, PATH_MAX);
	}
	signal_temps_store(0);
}

PRISM_MAYBE_UNUSED static void signal_temps_unregister(const char *path) {
	if (!path) return;
	sig_atomic_t n = signal_temps_load();
	for (int i = 0; i < n; i++) {
		if (signal_temps_ready_load(i) && strcmp(signal_temps[i], path) == 0) {
			memset(signal_temps[i], 0, PATH_MAX);
			signal_temps_ready_store(i, 0);
			return;
		}
	}
}

static PRISM_THREAD_LOCAL char **system_include_list; // Ordered list of includes
static PRISM_THREAD_LOCAL int system_include_capacity = 0;

static PRISM_THREAD_LOCAL FILE *out_fp;
static PRISM_THREAD_LOCAL PParseToken *last_emitted = NULL;

static PRISM_THREAD_LOCAL char out_buf[OUT_BUF_SIZE];
static PRISM_THREAD_LOCAL int out_buf_pos = 0;
static PRISM_THREAD_LOCAL int64_t out_total_flushed = 0;
static PRISM_THREAD_LOCAL bool use_linemarkers = false; // true = GCC linemarker "# N", false = C99 "#line N"

typedef struct {
	bool pending;
	bool pending_for_paren;
	bool parens_just_closed;
	int brace_depth;
	uint8_t pending_paren_kw; // next '(' from control keyword: 0=none, 1=loop,
				  // 2=switch
} CtrlState;

static PRISM_THREAD_LOCAL ScopeNode *scope_stack = NULL;
static PRISM_THREAD_LOCAL int scope_stack_cap = 0;
static PRISM_THREAD_LOCAL DeferEntry *defer_stack = NULL;
static PRISM_THREAD_LOCAL int defer_stack_cap = 0;
static PRISM_THREAD_LOCAL int defer_count = 0;
static PRISM_THREAD_LOCAL bool in_defer_emit = false; // recursion guard for emit_defers_ex
static PRISM_THREAD_LOCAL CtrlState ctrl_state;
static PRISM_THREAD_LOCAL CtrlState *ctrl_save_stack =
    NULL; // saved ctrl_state for stmt-expr inside ctrl parens
static PRISM_THREAD_LOCAL int ctrl_save_depth = 0;
static PRISM_THREAD_LOCAL int ctrl_save_cap = 0;
static PRISM_THREAD_LOCAL int current_func_idx = -1;  // Index into func_meta[] for the function being emitted
static PRISM_THREAD_LOCAL bool is_msvc_cached;	      // cached target_is_msvc(), set in transpile_tokens
static PRISM_THREAD_LOCAL bool prism_profile = false; // --prism-prof: emit phase timing to stderr
/* --prism-verify / PRISM_VERIFY: translation validation.  After emitting,
 * re-run the full pipeline on the emitted C and require a fixed point
 * (byte-identical modulo preprocessor linemarkers).  Any operator-position
 * defer/orelse that leaked into the output would transform or reject on the
 * second pass; a well-formed output re-verifies through every Phase 1 + CFG
 * check.  Generalizes the self-host stage1==stage2 invariant to every
 * user compile. */
static PRISM_THREAD_LOCAL bool prism_verify_mode = false;
static PRISM_THREAD_LOCAL bool prism_in_verify = false;
typedef struct {
	char *name;
	int len;
	int block_depth; // scope depth where the shadowing variable was declared
	PParseToken *var_tok;	 // for pparse_error reporting
	int defer_idx;	 // which defer it conflicts with
} DeferShadow;

static PRISM_THREAD_LOCAL DeferShadow *defer_shadows = NULL;
static PRISM_THREAD_LOCAL int defer_shadow_count = 0;
static PRISM_THREAD_LOCAL int defer_shadow_cap = 0;

// MSVC /D define buffers (dynamically allocated, freed in prism_thread_cleanup)
static PRISM_THREAD_LOCAL char **pp_define_bufs = NULL;
static PRISM_THREAD_LOCAL int pp_define_bufs_cap = 0;

static PParseToken *emit_expr_to_semicolon(PParseToken *tok);
static PParseToken *
emit_orelse_action(PParseToken *tok, PParseToken *var_name, bool single_eval_lhs, PParseToken *stop_comma);
static PParseToken *emit_return_body(PParseToken *tok, PParseToken *stop, bool active_known);
static PParseToken *try_zero_init_decl(PParseToken *tok);
static PParseToken *walk_balanced(PParseToken *tok);
static PParseToken *walk_balanced_orelse(PParseToken *tok);
static PParseToken *try_bounds_checks(PParseToken *t);

/* typeof(expr orelse val) -> typeof(ternary). Seven copies existed; six were
 * identical and one differed only by omitting the PPARSE_TT_TYPEOF fast path,
 * which try_typeof_orelse tests again itself. The PPARSE_F_ORELSE test is load
 * bearing - try_typeof_orelse does not check the feature. */
#define EMIT_TRY_TYPEOF_ORELSE(t)                                                                    \
	if (pparse_feat(PPARSE_F_ORELSE) && ((t)->tag & PPARSE_TT_TYPEOF)) {                         \
		PParseToken *_n = try_typeof_orelse(t);                                              \
		if (_n) {                                                                            \
			(t) = _n;                                                                    \
			continue;                                                                    \
		}                                                                                    \
	}

/* Emit a statement-expression group verbatim and resume after it. */
#define EMIT_SKIP_STMT_EXPR(t)                                                                       \
	if (pparse_is_stmt_expr_open(t) && pparse_pair(_pc, t)) {                                    \
		walk_balanced(t);                                                                    \
		(t) = pparse_next(_pc, pparse_pair(_pc, t));                                         \
		continue;                                                                            \
	}

/* Bounds-check handler, shared by every emit walker.
 *
 * Ten copies of this existed in three spellings: with and without the
 * PPARSE_F_BOUNDS_CHECK fast path, and with or without a `bc != t` test. The
 * fast path only skips the call - try_bounds_checks returns NULL when the
 * feature is off either way - and `bc != t` is dead, because both
 * try_bounds_check_subscript and try_bounds_check_deref_add return either NULL
 * or a token past the construct, never the token they were handed.
 *
 * Expands to a statement that advances `t` and continues the enclosing walk
 * when a bounds rewrite fired, so it composes with the other handlers. */
#define EMIT_TRY_BOUNDS(t)                                                                           \
	if (__builtin_expect(pparse_feat(PPARSE_F_BOUNDS_CHECK), 0)) {                               \
		PParseToken *_bc = try_bounds_checks(t);                                             \
		if (_bc) {                                                                           \
			(t) = _bc;                                                                   \
			continue;                                                                    \
		}                                                                                    \
	}

static PParseToken *try_typeof_orelse(PParseToken *tok);
static PParseToken *try_bracket_orelse(PParseToken *tok);
static void emit_token_range_orelse(PParseToken *start, PParseToken *end);
static void emit_token_range_nested(PParseToken *start, PParseToken *end, bool with_orelse);
static PParseToken *handle_sue_body(PParseToken *tok);
static void emit_noise_between_raws(PParseToken *first_raw, PParseToken *last_raw);
static inline PParseToken *try_strip_raw(PParseToken *t);
static inline void out_char(char c);
static inline void out_str(const char *s, int len);
#define OUT_TOK(t) out_str(pparse_loc(_pc, t), (t)->len)
#define skip_balanced(tok, o, c) pparse_skip_balanced_group(tok)
static bool cc_is_msvc(const char *cc);
static inline void ctrl_reset(void);

static inline double prism_now_ms(void) {
#ifdef _WIN32
	static PRISM_THREAD_LOCAL LARGE_INTEGER freq = {0};
	LARGE_INTEGER now;
	if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static void reset_transpiler_state(void) {
	PRISM_STATE();
	emit_scope_depth = emit_block_depth = 0;
	_ps->aggregate_member_nest = 0;
	_ps->last_line_no = 0;
	_ps->ret_counter = 0;
	_ps->last_filename = NULL;
	_ps->last_system_header = false;
	emit_at_stmt_start = true;
	ctrl_reset();
	ctrl_save_depth = 0;
	last_emitted = NULL;
	current_func_idx = -1;
	in_defer_emit = false; /* must clear: pparse_error longjmp can skip defer_walk restore */
	defer_count = 0;
	defer_shadow_count = 0;
	_ps->bracket_oe_ids = _ps->bracket_dim_ids = NULL;
	_ps->bracket_oe_count = _ps->bracket_oe_cap = _ps->bracket_oe_next = 0;
	_ps->bracket_dim_count = _ps->bracket_dim_cap = _ps->bracket_dim_next = 0;
	_ps->typeof_vars = NULL;
	_ps->typeof_var_count = _ps->typeof_var_cap = 0;
}

PRISM_API PrismFeatures prism_defaults(void) {
	return (PrismFeatures){.defer = true,
			       .zeroinit = true,
			       .line_directives = true,
			       .flatten_headers = true,
			       .orelse = true,
			       .auto_unreachable = true,
			       .auto_static = true,
			       .bounds_check = true};
}

static uint32_t features_to_bits(PrismFeatures f) {
	return (f.defer ? PPARSE_F_DEFER : 0) | (f.zeroinit ? PPARSE_F_ZEROINIT : 0) |
	       (f.line_directives ? PPARSE_F_LINE_DIR : 0) | (f.warn_safety ? PPARSE_F_WARN_SAFETY : 0) |
	       (f.quiet ? PPARSE_F_QUIET : 0) |
	       (f.flatten_headers ? PPARSE_F_FLATTEN : 0) | (f.orelse ? PPARSE_F_ORELSE : 0) |
	       (f.auto_unreachable ? PPARSE_F_AUTO_UNREACHABLE : 0) | (f.auto_static ? PPARSE_F_AUTO_STATIC : 0) |
	       (f.bounds_check ? PPARSE_F_BOUNDS_CHECK : 0);
}

static const char *get_tmp_dir(void) {
	static PRISM_THREAD_LOCAL char buf[PATH_MAX];
#ifdef _WIN32
	const wchar_t *wt = _wgetenv(L"TEMP");
	if (!wt || !*wt) wt = _wgetenv(L"TMP");
	if (!wt || !*wt) return TMPDIR_FALLBACK;
	int ulen = WideCharToMultiByte(CP_UTF8, 0, wt, -1, buf, PATH_MAX - 2, NULL, NULL);
	if (ulen <= 0) return TMPDIR_FALLBACK;
	size_t len = strlen(buf);
	if (len > 0 && buf[len - 1] != '/' && buf[len - 1] != '\\') {
		buf[len] = '/';
		buf[len + 1] = '\0';
	}
	return buf;
#else
	const char *t = getenv(TMPDIR_ENVVAR);
#ifdef TMPDIR_ENVVAR_ALT
	if (!t || !*t) t = getenv(TMPDIR_ENVVAR_ALT);
#endif
	if (!t || !*t) return TMPDIR_FALLBACK;
	size_t len = strlen(t);
	snprintf(buf, sizeof(buf), "%s%s", t, (t[len - 1] == '/' || t[len - 1] == '\\') ? "" : "/");
	return buf;
#endif
}

static bool dir_has_write_bits(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0) return true;
	if (!S_ISDIR(st.st_mode)) return false;
	return (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) != 0;
}

static void out_flush(void) {
	if (out_buf_pos > 0) {
		fwrite(out_buf, 1, out_buf_pos, out_fp);
		out_total_flushed += out_buf_pos;
		out_buf_pos = 0;
	}
}

static PRISM_COLD void out_str_slow(const char *s, int len) {
	if (len >= OUT_BUF_SIZE) {
		out_flush();
		fwrite(s, 1, len, out_fp);
		out_total_flushed += len;
		return;
	}
	out_flush();
	prism_memcpy_runtime_sized(out_buf + out_buf_pos, s, (size_t)(len));
	out_buf_pos += len;
}

static inline PRISM_ALWAYS_INLINE void out_char(char c) {
	if (pparse_PRISM_UNLIKELY(out_buf_pos >= OUT_BUF_SIZE)) out_flush();
	out_buf[out_buf_pos++] = c;
}

static inline PRISM_ALWAYS_INLINE void out_str(const char *s, int len) {
	if (pparse_PRISM_UNLIKELY(len <= 0)) return;
	if (pparse_PRISM_UNLIKELY(out_buf_pos + len >= OUT_BUF_SIZE)) {
		out_str_slow(s, len);
		return;
	}
	prism_memcpy_runtime_sized(out_buf + out_buf_pos, s, (size_t)(len));
	out_buf_pos += len;
}

/* Compile-time string literal: exact-width inline store, no size ladder.
 * (Was gated on PRISM_MEM_OUT_LIT_STATIC, part of the removed mem.c kit; the
 * out_str fallback is what every build actually used.) */
#define OUT_LIT(s) out_str((s), (int)sizeof(s) - 1)

// Check if the effective compiler is MSVC, falling back to PRISM_DEFAULT_CC
static inline bool target_is_msvc(void) {
	return is_msvc_cached;
}

#define EMIT_UNREACHABLE()                                                                                   \
	do {                                                                                                 \
		if (target_is_msvc()) OUT_LIT(" __assume(0);");                                              \
		else                                                                                         \
			OUT_LIT(" __builtin_unreachable();");                                                \
	} while (0)

// Emit __typeof__ (GNU) or typeof (C23/MSVC).
// MSVC does not support __typeof__; C23 typeof is available under /std:clatest.
static inline void emit_typeof_keyword(void) {
	if (target_is_msvc()) OUT_LIT("typeof");
	else
		OUT_LIT("__typeof__");
}

static void out_close(void) {
	if (out_fp) {
		out_flush();
		fclose(out_fp);
		out_fp = NULL;
	}
}

static void out_uint(unsigned long long v) {
	char buf[24], *p = buf + sizeof(buf);
	do {
		*--p = '0' + v % 10;
	} while (v /= 10);
	out_str(p, buf + sizeof(buf) - p);
}

static void out_quoted_path(const char *file) {
	for (const char *p = file; *p; p++) {
		char c = *p;
		if (c == '\\') c = '/'; // normalize backslashes (MSVC C4129, harmless on POSIX)
		if (c == '"' || c == '\\') out_char('\\');
		out_char(c);
	}
}

static void out_line(int line_no, const char *file, bool is_system) {
	if (use_linemarkers) OUT_LIT("# ");
	else
		OUT_LIT("#line ");
	out_uint(line_no);
	OUT_LIT(" \"");
	out_quoted_path(file);
	if (use_linemarkers && is_system) OUT_LIT("\" 3\n");
	else
		OUT_LIT("\"\n");
}

static void collect_system_includes(void) {
	PRISM_STATE();
	PPARSE_CTX();
	PParseHashMap include_map = {0};
	for (int i = 0; i < _pc->input_file_count; i++) {
		PParseFile *f = _pc->input_files[i];
		/* Only re-emit headers the TU included directly — nested system
		 * headers (e.g. bits/libc-header-start.h) pparse_error if included alone. */
		if (!f->is_system || !f->is_direct_system_include || !f->name) continue;
		const char *base = path_basename(f->name);
		/* Compiler-injected; not a user #include. */
		if (!strcmp(base, "stdc-predef.h")) continue;
		if (strcmp(base, "assert.h")) {
			int len = (int)strlen(f->name);
			if (pparse_hashmap_get(&include_map, f->name, len)) continue;
			pparse_hashmap_put(&include_map, f->name, len, (void *)1);
		}
		PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena,
				 system_include_list,
				 _ps->system_include_count + 1,
				 system_include_capacity,
				 32,
				 char *);
		system_include_list[_ps->system_include_count++] = f->name;
	}
}

static void emit_system_header_diag_push(void) {
	if (target_is_msvc()) {
		OUT_LIT("#pragma warning(push, 0)\n");
		return;
	}
	OUT_LIT("#pragma GCC diagnostic push\n"
		"#pragma GCC diagnostic ignored \"-Wredundant-decls\"\n"
		"#pragma GCC diagnostic ignored \"-Wstrict-prototypes\"\n"
		"#pragma GCC diagnostic ignored \"-Wold-style-definition\"\n"
		"#pragma GCC diagnostic ignored \"-Wpedantic\"\n"
		"#pragma GCC diagnostic ignored \"-Wunused-function\"\n"
		"#pragma GCC diagnostic ignored \"-Wunused-parameter\"\n"
		"#pragma GCC diagnostic ignored \"-Wunused-variable\"\n"
		"#pragma GCC diagnostic ignored \"-Wcast-qual\"\n"
		"#pragma GCC diagnostic ignored \"-Wsign-conversion\"\n"
		"#pragma GCC diagnostic ignored \"-Wconversion\"\n");
}

static void emit_system_header_diag_pop(void) {
	if (target_is_msvc()) {
		OUT_LIT("#pragma warning(pop)\n");
		return;
	}
	OUT_LIT("#pragma GCC diagnostic pop\n");
}

static void emit_define_guarded(const char *def) {
	const char *eq = strchr(def, '=');
	int name_len = eq ? (int)(eq - def) : (int)strlen(def);
	if (name_len <= 0) return;
	OUT_LIT("#ifndef ");
	out_str(def, name_len);
	OUT_LIT("\n#define ");
	out_str(def, name_len);
	if (eq) {
		OUT_LIT(" ");
		out_str(eq + 1, strlen(eq + 1));
	}
	OUT_LIT("\n#endif\n");
}

static void emit_consumed_def_upsert(char ***names, bool **on, int *n, int *cap,
				       const char *spec, bool defined, bool *undef_gnu) {
	const char *eq = strchr(spec, '=');
	int nlen = eq ? (int)(eq - spec) : (int)strlen(spec);
	if (nlen <= 0) return;
	if (!defined && nlen == 11 && prism_memeq_static(spec, "_GNU_SOURCE", 11) && undef_gnu) *undef_gnu = true;
	for (int i = 0; i < *n; i++) {
		if ((int)strlen((*names)[i]) == nlen && prism_memeq_runtime_sized((*names)[i], spec, (uint32_t)nlen)) {
			(*on)[i] = defined;
			return;
		}
	}
	if (*n >= *cap) {
		*cap = *cap ? *cap * 2 : 16;
		*names = realloc(*names, (size_t)*cap * sizeof(char *));
		*on = realloc(*on, (size_t)*cap * sizeof(bool));
		if (!*names || !*on) pparse_error("out of memory");
	}
	(*names)[*n] = malloc((size_t)nlen + 1);
	if (!(*names)[*n]) pparse_error("out of memory");
	memcpy((*names)[*n], spec, (size_t)nlen);
	(*names)[*n][nlen] = '\0';
	(*on)[*n] = defined;
	(*n)++;
}

static void emit_consumed_defines(void) {
	PRISM_STATE();
	bool any = _ps->extra_define_count > 0 || _ps->source_define_count > 0;
	bool user_undef_gnu = false;
	for (int i = 0; i < _ps->extra_compiler_flags_count; i++) {
		const char *f = _ps->extra_compiler_flags[i];
		if (f[0] == '-' && (f[1] == 'D' || f[1] == 'U')) {
			any = true;
			break;
		}
	}

	if (!any && _ps->system_include_count == 0) return;

	char **def_names = NULL;
	bool *def_on = NULL;
	int def_n = 0, def_cap = 0;

	for (int i = 0; i < _ps->extra_define_count; i++)
		emit_consumed_def_upsert(&def_names, &def_on, &def_n, &def_cap, _ps->extra_defines[i], true,
					 &user_undef_gnu);
	for (int i = 0; i < _ps->extra_compiler_flags_count; i++) {
		const char *f = _ps->extra_compiler_flags[i];
		if (f[0] != '-' || (f[1] != 'D' && f[1] != 'U')) continue;
		bool defined = (f[1] == 'D');
		const char *spec = f[2] ? f + 2 : NULL;
		if (!spec && i + 1 < _ps->extra_compiler_flags_count) spec = _ps->extra_compiler_flags[++i];
		if (spec)
			emit_consumed_def_upsert(&def_names, &def_on, &def_n, &def_cap, spec, defined,
						 &user_undef_gnu);
	}

	for (int i = 0; i < def_n; i++) {
		if (def_on[i]) {
			const char *emit = def_names[i];
			for (int j = 0; j < _ps->extra_define_count; j++) {
				const char *s = _ps->extra_defines[j];
				int nlen = (int)strlen(def_names[i]);
				if (!strncmp(s, def_names[i], (size_t)nlen) &&
				    (s[nlen] == '\0' || s[nlen] == '=')) {
					emit = s;
					break;
				}
			}
			for (int j = 0; j < _ps->extra_compiler_flags_count; j++) {
				const char *f = _ps->extra_compiler_flags[j];
				const char *s = NULL;
				if (f[0] == '-' && f[1] == 'D')
					s = f[2] ? f + 2
						 : (j + 1 < _ps->extra_compiler_flags_count
							? _ps->extra_compiler_flags[j + 1]
							: NULL);
				if (!s) continue;
				int nlen = (int)strlen(def_names[i]);
				if (!strncmp(s, def_names[i], (size_t)nlen) &&
				    (s[nlen] == '\0' || s[nlen] == '=')) {
					emit = s;
					break;
				}
			}
			emit_define_guarded(emit);
		} else {
			OUT_LIT("#ifdef ");
			out_str(def_names[i], strlen(def_names[i]));
			OUT_LIT("\n#undef ");
			out_str(def_names[i], strlen(def_names[i]));
			OUT_LIT("\n#endif\n");
		}
		free(def_names[i]);
	}
	free(def_names);
	free(def_on);

	for (int i = 0; i < _ps->source_define_count; i++) {
		const char *guard = _ps->source_define_guards ? _ps->source_define_guards[i] : NULL;
		if (guard) out_str(guard, strlen(guard));
		emit_define_guarded(_ps->source_defines[i]);
		if (guard) {
			int depth = 0;
			const char *gp = guard;
			while (*gp) {
				if (*gp == '#') {
					const char *d = gp + 1;
					while (*d == ' ' || *d == '\t') d++;
					if (d[0] == 'i' && d[1] == 'f') depth++;
				}
				while (*gp && *gp != '\n') gp++;
				if (*gp) gp++;
			}
			for (int d = 0; d < depth; d++) OUT_LIT("#endif\n");
		}
	}

	OUT_LIT("#if !defined(_WIN32)\n"
		"#ifndef _POSIX_C_SOURCE\n#define _POSIX_C_SOURCE 200809L\n#endif\n");
	if (!user_undef_gnu) OUT_LIT("#ifndef _GNU_SOURCE\n#define _GNU_SOURCE\n#endif\n");
	OUT_LIT("#ifdef __APPLE__\n#ifndef _DARWIN_C_SOURCE\n#define "
		"_DARWIN_C_SOURCE\n#endif\n#endif\n"
		"#endif\n\n");
}

static void emit_system_includes(void) {
	PRISM_STATE();
	emit_consumed_defines();
	if (_ps->system_include_count == 0) return;
	emit_system_header_diag_push();
	for (int i = 0; i < _ps->system_include_count; i++) {
		OUT_LIT("#include \"");
		out_quoted_path(system_include_list[i]);
		OUT_LIT("\"\n");
	}

	emit_system_header_diag_pop();
	out_char('\n');
}

static void system_includes_reset(void) {
	PRISM_STATE();
	system_include_list = NULL;
	_ps->system_include_count = 0;
	system_include_capacity = 0;
}

static inline void ctrl_reset(void) {
	ctrl_state = (CtrlState){0};
}

static inline ScopeNode *scope_block_top(void) {
	for (int i = emit_scope_depth - 1; i >= 0; i--)
		if (is_brace_scope(scope_stack[i].kind)) return &scope_stack[i];
	return NULL;
}

static inline bool in_for_init(void) {
	return emit_scope_depth > 0 && scope_stack[emit_scope_depth - 1].kind == SCOPE_FOR_PAREN;
}

static inline bool in_ctrl_paren(void) {
	for (int i = emit_scope_depth - 1; i >= 0; i--) {
		ScopeKind k = scope_stack[i].kind;
		if (is_brace_scope(k)) return false;
		if (k == SCOPE_FOR_PAREN || k == SCOPE_CTRL_PAREN) return true;
	}
	return false;
}

static inline bool in_struct_body(void) {
	for (int i = emit_scope_depth - 1; i >= 0; i--) {
		if (scope_stack[i].is_stmt_expr) return false;
		if (is_brace_scope(scope_stack[i].kind) && scope_stack[i].is_struct) return true;
	}
	return false;
}

static inline bool in_generic(void) {
	for (int i = emit_scope_depth - 1; i >= 0; i--) {
		if (scope_stack[i].kind == SCOPE_GENERIC) return true;
		if (is_brace_scope(scope_stack[i].kind)) return false;
	}
	return false;
}

static void end_statement_after_semicolon(void) {
	emit_at_stmt_start = true;
	if (ctrl_state.pending && !in_ctrl_paren() && !in_struct_body()) {
		while (defer_shadow_count > 0 &&
		       defer_shadows[defer_shadow_count - 1].block_depth > emit_block_depth)
			defer_shadow_count--;
		ctrl_reset();
	}
}

static void scope_push_kind(ScopeKind kind) {
	pparse_VEC_ENSURE_REALLOC(scope_stack, emit_scope_depth + 1, scope_stack_cap, 64);
	ScopeNode *s = &scope_stack[emit_scope_depth];
	*s = (ScopeNode){.kind = kind};
	s->defer_start_idx = defer_count;
	if (kind == SCOPE_BLOCK) emit_block_depth++;
	emit_scope_depth++;
}

static void scope_pop(void) {
	if (emit_scope_depth > 0) {
		emit_scope_depth--;
		ScopeNode *s = &scope_stack[emit_scope_depth];
		if (s->kind == SCOPE_BLOCK) {
			while (defer_shadow_count > 0 &&
			       defer_shadows[defer_shadow_count - 1].block_depth >= emit_block_depth)
				defer_shadow_count--;
			// Stmt-expr scopes must not clear shadows from enclosing scopes.
			if (s->is_stmt_expr && defer_shadow_count < s->saved_defer_shadow_count)
				defer_shadow_count = s->saved_defer_shadow_count;
			emit_block_depth--;
		}
	}
}

static void defer_add(PParseToken *defer_keyword, PParseToken *start, PParseToken *end) {
	/* Phase 1 p1d_validate_defer owns the common file-scope reject; this
	 * guard is UNCONDITIONAL (not PRISM_DEBUG-only) because a defer that
	 * reaches Pass 2 at depth 0 through an unclassified context (e.g. a
	 * file-scope initializer) would otherwise register silently and its
	 * body would never be emitted — a token-dropping miscompile.  One
	 * comparison per defer statement; found by the contexts suite. */
	if (emit_block_depth <= 0) pparse_error_tok(start, "defer outside of any scope");
	pparse_VEC_ENSURE_REALLOC(defer_stack, defer_count + 1, defer_stack_cap, 16);
	defer_stack[defer_count++] = (DeferEntry){start, end, defer_keyword};
}

static inline bool emit_newline_before_decl_after_stmt_boundary(PParseToken *prev, PParseToken *tok) {
	PRISM_STATE();
	PPARSE_CTX();
	if (!prev || !tok) return false;
	/* Cheapest, most selective test first: only a `;`/`}` predecessor can
	 * trigger this. Rejecting here avoids in_struct_body()'s scope-stack walk
	 * on the ~95% of tokens that don't follow a statement boundary. */
	if (!(pparse_match_ch(prev, ';') || pparse_match_ch(prev, '}'))) return false;
	// Member declarations in struct/union/enum bodies use `;` between specifiers;
	if (_ps->aggregate_member_nest > 0) return false;
	if (in_struct_body()) return false;
	if (pparse_at_bol(tok)) return false;
	if (pparse_ann(tok) & P1_IS_DECL) return true;
	return (tok->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_STORAGE | PPARSE_TT_TYPEDEF | PPARSE_TT_INLINE |
			    PPARSE_TT_TYPEOF | PPARSE_TT_ALIGNAS | PPARSE_TT_BITINT)) ||
	       pparse_is_type_keyword(tok) || pparse_is_known_typedef(tok) || pparse_is_c23_attr(tok) ||
	       ((tok->tag & PPARSE_TT_ATTR) && pparse_next(_pc, tok) && pparse_match_ch(pparse_next(_pc, tok), '('));
}

static PRISM_HOT void emit_tok(PParseToken *tok) {
	PRISM_STATE();
	PPARSE_CTX();
	uint32_t feat = _pc->features;
	if (__builtin_expect(!(feat & PPARSE_F_FLATTEN) && (tok->flags & PPARSE_TF_SYS_SKIP), 0)) return;
	PParseFile *f = (tok->file_idx < (uint32_t)_pc->input_file_count) ? _pc->input_files[tok->file_idx]
								  : _pc->current_file;
	char *loc = pparse_loc(_pc, tok);
	PParseToken *prev_emitted = last_emitted;
	bool need_line = false;
	char *tok_fname = NULL;
	int line_no = 0;
	if (feat & PPARSE_F_LINE_DIR) {
		line_no = tok->line_no;
		tok_fname = f->name;
		need_line = (_ps->last_filename != tok_fname) || (f->is_system != _ps->last_system_header) ||
			    (line_no != _ps->last_line_no && line_no != _ps->last_line_no + 1);
	}

	if (pparse_at_bol(tok) || need_line || emit_newline_before_decl_after_stmt_boundary(last_emitted, tok))
		out_char('\n');
	else if ((tok->flags & PPARSE_TF_HAS_SPACE) || pparse_needs_space(last_emitted, tok))
		out_char(' ');
	if (need_line) {
		out_line(line_no, tok_fname, f->is_system);
		_ps->last_filename = tok_fname;
		_ps->last_system_header = f->is_system;
	}

	_ps->last_line_no = line_no;
	if (__builtin_expect(tok->kind == PPARSE_TK_PREP_DIR, 0)) {
		if (!pparse_at_bol(tok)) out_char('\n');
		if ((feat & PPARSE_F_FLATTEN) && tok->len >= 8 && loc[0] == '#') {
			const char *p = loc + 1;
			const char *end = loc + tok->len;
			while (p < end && (*p == ' ' || *p == '\t')) p++;
			if (end - p >= 7 && prism_memeq_static(p, "define", 6) && (p[6] == ' ' || p[6] == '\t')) {
				p += 6;
				while (p < end && (*p == ' ' || *p == '\t')) p++;
				const char *name = p;
				while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
						   (*p >= '0' && *p <= '9') || *p == '_'))
					p++;
				if (p > name) {
					OUT_LIT("#undef ");
					out_str(name, (int)(p - name));
					out_char('\n');
				}
			}
		}
		out_str(loc, tok->len);
		last_emitted = tok;
		return;
	}

	out_str(loc, tok->len);
	last_emitted = tok;
	if (prev_emitted && pparse_match_ch(tok, '{') && (prev_emitted->tag & PPARSE_TT_SUE)) _ps->aggregate_member_nest++;
	else if (pparse_match_ch(tok, '}') && _ps->aggregate_member_nest > 0)
		_ps->aggregate_member_nest--;
}

static inline void reject_defer_in_expr_context(PParseToken *t);

static inline PParseToken *emit_tok_checked(PParseToken *t) {
	PParseToken *r = try_strip_raw(t);
	if (r) return r;
	emit_tok(t);
	return NULL;
}

static inline PParseToken *emit_advance(PParseToken *t) {
	PPARSE_CTX();
	PParseToken *r = emit_tok_checked(t);
	return r ? r : pparse_next(_pc, t);
}

static void emit_ll_temp(void (*emit_id)(unsigned), unsigned id) {
	OUT_LIT(" long long");
	emit_id(id);
	OUT_LIT(" = (");
}

static PParseToken *emit_c23_attr(PParseToken *t) {
	PPARSE_CTX();
	PParseToken *bclose = pparse_pair(_pc, t);
	t = emit_advance(t);
	emit_token_range_nested(t, bclose, false);
	emit_tok(bclose);
	return pparse_next(_pc, bclose);
}

static void emit_orelse_ternary(PParseToken *lhs_start, PParseToken *orelse, PParseToken *rhs_start, PParseToken *rhs_end) {
	OUT_LIT(" (");
	emit_token_range_orelse(lhs_start, orelse);
	OUT_LIT(") ? (");
	emit_token_range_orelse(lhs_start, orelse);
	OUT_LIT(") : (");
	emit_token_range_orelse(rhs_start, rhs_end);
	OUT_LIT(")");
}

static void emit_prism_dim(unsigned dim) {
	OUT_LIT(" __prism_dim_");
	out_uint(dim);
}

static void emit_prism_oe(unsigned oe) {
	OUT_LIT(" __prism_oe_");
	out_uint(oe);
}

static inline PParseToken *emit_gnu_label_decl(PParseToken *tok) {
	PPARSE_CTX();
	if (!emit_at_stmt_start || !pparse_is_gnu_label_decl_head(tok)) return NULL;
	PPARSE_FOR_TAIL(t, tok) {
		emit_tok(t);
		if (pparse_match_ch(t, ';')) {
			end_statement_after_semicolon();
			return pparse_next(_pc, t);
		}
	}
	return NULL;
}

#define ER_SKIP_PREP 1 // Skip PPARSE_TK_PREP_DIR tokens
#define ER_BALANCED 2  // Use walk_balanced for paren/bracket groups (not just stmt-expr)

static void emit_range_ex(PParseToken *start, PParseToken *end, int flags) {
	PPARSE_CTX();
	PParseToken *t = start;
	while (t && t != end && t->kind != PPARSE_TK_EOF) {
		if ((flags & ER_SKIP_PREP) && t->kind == PPARSE_TK_PREP_DIR) {
			t = pparse_next(_pc, t);
			continue;
		}
		if ((flags & ER_BALANCED) && (t->flags & PPARSE_TF_OPEN) && pparse_match_set(t, pparse_CH('(') | pparse_CH('['))) {
			walk_balanced(t);
			t = pparse_next(_pc, pparse_pair(_pc, t));
			continue;
		}
		// C23 [[...]]: Phase 1D rejects 'orelse' inside attribute arguments;
		if ((t->flags & PPARSE_TF_C23_ATTR) && pparse_pair(_pc, t)) {
			t = emit_c23_attr(t);
			continue;
		}
		EMIT_SKIP_STMT_EXPR(t)
		// Defense-in-depth: typeof(expr orelse val) → typeof(ternary)
		EMIT_TRY_TYPEOF_ORELSE(t)
		/* Subscripts in copied ranges (orelse single-eval LHS duplicates,
		 * fallback values, return bodies) get the same -fbounds-check
		 * wrapping as the main loop — `v[i] = g() orelse 1;` must not
		 * store through a tracked array unchecked.  try_bounds_checks
		 * carries its own applicability and idempotence guards.  Found
		 * by the contexts suite's fixed-point oracle. */
		EMIT_TRY_BOUNDS(t)
		t = emit_advance(t);
	}
}

#define emit_range(start, end) emit_range_ex(start, end, 0)
#define emit_range_no_prep(start, end) emit_range_ex(start, end, ER_SKIP_PREP)
#define emit_balanced_range(start, end) emit_range_ex(start, end, ER_SKIP_PREP | ER_BALANCED)

static PParseToken *emit_bare_orelse_impl(PParseToken *t, PParseToken *end, bool comma_term, bool brace_wrap);
static PParseToken *emit_deferred_orelse(PParseToken *t, PParseToken *end);
static void emit_deferred_range(PParseToken *start, PParseToken *end);

static bool defer_walk(DeferEmitMode mode, int stop_depth, bool dry_run) {
	PPARSE_CTX();
	if (emit_block_depth <= 0) return false;
	if (!dry_run && in_defer_emit) return false;
	bool saved_in_defer = in_defer_emit;
	if (!dry_run) in_defer_emit = true;
	int current_defer = defer_count - 1;
	int curr_bd = emit_block_depth;
	bool found = false;
	int min_defer_idx = defer_count;
	for (int d = emit_scope_depth - 1; d >= 0; d--) {
		ScopeKind sk = scope_stack[d].kind;
		if (sk != SCOPE_BLOCK) {
			/* break/continue inside GNU stmt-expr in e.g. while(cond) sees the loop's
       * body block only after `{` — the matching while/switch '(' is
       * CTRL/FOR_PAREN. Stop defer unwinding here without emitting outer
       * scopes' defers. */
			if (mode == DEFER_BREAK && (sk == SCOPE_CTRL_PAREN || sk == SCOPE_FOR_PAREN) &&
			    (scope_stack[d].is_loop || scope_stack[d].is_switch))
				break;
			if (mode == DEFER_CONTINUE && (sk == SCOPE_CTRL_PAREN || sk == SCOPE_FOR_PAREN) &&
			    scope_stack[d].is_loop)
				break;
			continue;
		}
		if (mode == DEFER_TO_DEPTH && curr_bd <= stop_depth) break;
		ScopeNode *scope = &scope_stack[d];
		if (dry_run) {
			if (defer_count > scope->defer_start_idx) {
				found = true;
				break;
			}
		} else {
			if (scope->defer_start_idx < min_defer_idx) min_defer_idx = scope->defer_start_idx;
			for (int i = current_defer; i >= scope->defer_start_idx; i--) {
				out_char(' ');
				emit_deferred_range(defer_stack[i].stmt, defer_stack[i].end);
				out_char(';');
				/* Braceless `defer die();` stores the body as
				 * `[die, ';')` and synthesizes `;` here — so
				 * emit_statements never sees the semicolon that
				 * normally triggers auto-unreachable. Re-detect
				 * a top-level noreturn call on the body start.
				 * Braced bodies / `defer if (0) die();` already
				 * handle (or correctly suppress) injection. */
				if (pparse_feat(PPARSE_F_AUTO_UNREACHABLE) && defer_stack[i].stmt &&
				    !pparse_match_ch(defer_stack[i].stmt, '{')) {
					PParseToken *nr = pparse_try_detect_noreturn_call(defer_stack[i].stmt);
					if (nr) EMIT_UNREACHABLE();
				}
			}
			current_defer = scope->defer_start_idx - 1;
		}
		curr_bd--;
		if (mode == DEFER_SCOPE) break;
		if (mode == DEFER_BREAK && (scope->is_loop || scope->is_switch)) break;
		if (mode == DEFER_CONTINUE && scope->is_loop) break;
	}
	if (!dry_run) {
		in_defer_emit = saved_in_defer;
		if (mode != DEFER_SCOPE && defer_shadow_count > 0 && min_defer_idx < defer_count) {
			for (int si = 0; si < defer_shadow_count; si++) {
				DeferShadow *sh = &defer_shadows[si];
				if (sh->defer_idx >= min_defer_idx && sh->defer_idx < defer_count)
					pparse_error_tok(sh->var_tok,
						  "variable '%.*s' shadows a name captured by defer "
						  "in an enclosing scope and a control-flow exit "
						  "(return/goto/break/continue) would paste the "
						  "defer while the shadow is live",
						  sh->len,
						  sh->name);
			}
		}
	}
	return found;
}

static inline void emit_defers_ex(DeferEmitMode mode, int stop_depth) {
	defer_walk(mode, stop_depth, false);
}

static inline bool has_defers_for(DeferEmitMode mode, int stop_depth) {
	return defer_walk(mode, stop_depth, true);
}

static void check_defer_var_shadow(PParseToken *var_name) {
	PPARSE_CTX();
	if (!pparse_feat(PPARSE_F_DEFER) || defer_count == 0) return;
	ScopeNode *blk = scope_block_top();
	if (!blk) return;
	int outer_defer_end = blk->defer_start_idx;
	if (in_for_init() && outer_defer_end <= 0) outer_defer_end = defer_count;
	int same_block_start = blk->defer_start_idx;
	if (outer_defer_end <= 0 && same_block_start >= defer_count) return;
	char *name = pparse_loc(_pc, var_name);
	int nlen = var_name->len;
	if (current_func_idx >= 0 && !pparse_hashmap_get(&func_meta[current_func_idx].defer_name_set, name, nlen))
		return;
	for (int i = 0; i < defer_count; i++) {
		if (i >= outer_defer_end && i < same_block_start) continue;
		uint32_t var_idx = pparse_idx(_pc, var_name);
		uint32_t stmt_idx = pparse_idx(_pc, defer_stack[i].stmt);
		uint32_t end_idx = defer_stack[i].end ? pparse_idx(_pc, defer_stack[i].end) : UINT32_MAX;
		if (var_idx >= stmt_idx && var_idx < end_idx) continue;
		if (!p1_defer_has_capture(defer_stack[i].defer_kw, name, nlen)) continue;
		if (defer_shadow_count >= defer_shadow_cap) {
			int new_cap = defer_shadow_cap ? defer_shadow_cap * 2 : 16;
			void *tmp = realloc(defer_shadows, new_cap * sizeof(*defer_shadows));
			if (!tmp) pparse_error("out of memory");
			defer_shadows = tmp;
			defer_shadow_cap = new_cap;
		}
		defer_shadows[defer_shadow_count++] = (DeferShadow){
		    .name = name,
		    .len = nlen,
		    .block_depth = emit_block_depth + (in_for_init() ? 1 : 0),
		    .var_tok = var_name,
		    .defer_idx = i,
		};
		return;
	}
}

// Called from Pass 2 at statement-start for enum definitions and typedef
// declarations
static void enum_pass2_shadow_cb(PParseToken *t, void *ud) {
	(void)ud;
	check_defer_var_shadow(t);
}

static void check_enum_body_defer_shadow(PParseToken *brace) {
	pparse_for_each_enum_constant(brace, enum_pass2_shadow_cb, NULL);
}

static void check_enum_typedef_defer_shadow(PParseToken *tok) {
	PPARSE_CTX();
	if (!pparse_feat(PPARSE_F_DEFER) || defer_count == 0 || emit_block_depth <= 0) return;
	if (tok->tag & PPARSE_TT_SUE) {
		if (!pparse_is_enum_kw(tok)) return;
		PParseToken *brace = pparse_find_struct_body_brace(tok);
		if (brace) check_enum_body_defer_shadow(brace);
		return;
	}

	if (tok->tag & PPARSE_TT_TYPEDEF) {
		PParseToken *type_start = pparse_next(_pc, tok);
		if (!type_start) return;
		PParseTypeSpec type = pparse_type_specifier(type_start);
		if (!type.saw_type) return;
		PPARSE_FOR_RANGE(s, type_start, type.end) {
			if (pparse_is_enum_kw(s)) {
				PParseToken *brace = pparse_find_struct_body_brace(s);
				if (brace) check_enum_body_defer_shadow(brace);
			}
		}
		PParseToken *t = type.end;
		while (t && t->kind != PPARSE_TK_EOF && !pparse_match_ch(t, ';')) {
			PParseDecl decl = pparse_declarator(t);
			if (decl.var_name) check_defer_var_shadow(decl.var_name);
			if (!decl.end) break;
			t = decl.end;
			if (pparse_match_ch(t, ',')) t = pparse_next(_pc, t);
			else
				break;
		}
		return;
	}
}

static inline bool is_orelse_keyword(PParseToken *tok) {
	/* Phase 1 sets P1_IS_ORELSE_KW; Pass 2 only runs after annotation. */
	return (pparse_ann(tok) & P1_IS_ORELSE_KW) != 0;
}

static inline bool raw_token_is_sue_tag_name(PParseToken *t) {
	PPARSE_CTX();
	if (!t || !(t->flags & PPARSE_TF_RAW)) return false;
	PParseToken *prev = pparse_walk_back(pparse_idx(_pc, t), PPARSE_WB_PAST_NOISE);
	return (prev && (prev->tag & PPARSE_TT_SUE)) || pparse_token_ends_sue_type_specifier(t);
}

static void emit_type_range(PParseToken *start, PParseToken *end, bool strip_const, bool strip_sue_body) {
	PPARSE_CTX();
	int raw_depth = 0;
	for (PParseToken *t = start; t && t != end && t->kind != PPARSE_TK_EOF;) {
		if (strip_const && (t->tag & PPARSE_TT_CONST)) {
			t = pparse_next(_pc, t);
			continue;
		}
		EMIT_SKIP_STMT_EXPR(t)
		if (pparse_match_ch(t, '{')) raw_depth++;
		if (pparse_match_ch(t, '}')) raw_depth--;
		if (raw_depth == 0 && (t->flags & PPARSE_TF_RAW) && !pparse_is_known_typedef(t) &&
		    !raw_token_is_sue_tag_name(t)) {
			PParseToken *after = pparse_skip_noise(_pc, pparse_next(_pc, t));
			PParseToken *last = t;
			pparse_SKIP_RAW(after, last);
			emit_noise_between_raws(t, last);
			t = pparse_next(_pc, last);
			continue;
		}
		if (pparse_feat(PPARSE_F_ORELSE) && pparse_next(_pc, t) && pparse_match_ch(pparse_next(_pc, t), '(') &&
		    ((t->tag & (PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS)) ||
		     ((t->tag & PPARSE_TT_TYPE) && pparse_equal(t, "_Atomic")))) {
			emit_tok(t);
			t = pparse_next(_pc, t);
			t = walk_balanced_orelse(t);
			continue;
		}
		EMIT_TRY_BOUNDS(t)
		if (strip_sue_body && pparse_match_ch(t, '{')) {
			PParseToken *kw = NULL;
			for (PParseToken *s = start; s != t; s = pparse_next(_pc, s))
				if (s->tag & PPARSE_TT_SUE) kw = s;
			bool keep = false;
			if (kw && !pparse_is_enum_kw(kw)) {
				PPARSE_FOR_RANGE(u, pparse_next(_pc, kw), t) {
					if (pparse_match_ch(u, ')') && pparse_pair(_pc, u) &&
					    pparse_loc(_pc, pparse_pair(_pc, u)) < pparse_loc(_pc, kw)) {
						keep = true;
						break;
					}
				}
				if (!keep) {
					keep = true;
					PPARSE_FOR_RANGE(u, pparse_next(_pc, kw), t) {
						if ((u->flags & PPARSE_TF_OPEN) &&
						    (pparse_match_ch(u, '(') || (u->flags & PPARSE_TF_C23_ATTR))) {
							u = pparse_pair(_pc, u);
							if (!u || u == t) break;
							continue;
						}
						if (pparse_is_valid_varname(u) &&
						    !(u->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_ATTR | PPARSE_TT_TYPEOF))) {
							keep = false;
							break;
						}
					}
				}
			}
			if (!keep) {
				raw_depth--;
				t = pparse_skip_balanced_group(t);
				if (t == end) break;
				continue;
			}
		}
		{
			PParseToken *r = try_strip_raw(t);
			if (r) {
				t = r;
				continue;
			}
		}
		t = emit_advance(t);
	}
}

static PParseToken *emit_expr_to_stop(PParseToken *tok, PParseToken *stop, bool check_orelse) {
	PPARSE_CTX();
	while (tok->kind != PPARSE_TK_EOF) {
		if (tok->flags & PPARSE_TF_OPEN) {
			tok = walk_balanced(tok);
			continue;
		}
		if (pparse_match_ch(tok, ';') || (stop && tok == stop)) break;
		if (check_orelse && is_orelse_keyword(tok)) break;
		EMIT_TRY_TYPEOF_ORELSE(tok)
		reject_defer_in_expr_context(tok);
		tok = emit_advance(tok);
	}
	return tok;
}

static PParseToken *try_strip_raw_slow(PParseToken *t) {
	PPARSE_CTX();
	if (raw_token_is_sue_tag_name(t)) return NULL;
	/* Force multiplication in subscripts: arr[raw * x] — never strip `raw`. */
	if (pparse_raw_after_subscript_open_bracket(t)) return NULL;
	PParseToken *after = pparse_skip_noise(_pc, pparse_next(_pc, t));
	/* Variable / enum-constant shadows named `raw` are identifiers. */
	if (pparse_identifier_binding_kind(t) == PPARSE_BINDING_VALUE) return NULL;
	/* Typedef named `raw` is the type in `raw x` / `raw *p`. Strip only when
	 * it is a keyword prefix before another type (`raw raw x`, `raw int x`). */
	if (pparse_is_known_typedef(t)) {
		if (!(after && (pparse_is_type_keyword(after) || pparse_is_known_typedef(after) ||
				(after->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_STORAGE | PPARSE_TT_INLINE |
					       PPARSE_TT_TYPEDEF)) ||
				(after->flags & PPARSE_TF_RAW))))
			return NULL;
	} else if (t->td_entry) {
		return NULL;
	}
	if (pparse_is_raw_strip_context(t, after)) {
		PParseToken *last = t;
		pparse_SKIP_RAW(after, last);
		emit_noise_between_raws(t, last);
		return pparse_next(_pc, last);
	}
	return NULL;
}

static inline PParseToken *try_strip_raw(PParseToken *t) {
	if (__builtin_expect(!(t->flags & PPARSE_TF_RAW), 1)) return NULL;
	return try_strip_raw_slow(t);
}

static void emit_ret_type_tokens(PParseToken *start, PParseToken *end) {
	PPARSE_CTX();
	bool first = true;
	for (PParseToken *t = start; t && t != end && t->kind != PPARSE_TK_EOF;) {
		if (t->flags & PPARSE_TF_MS_CC) {
			t = pparse_next(_pc, t);
			continue;
		}
		if ((t->tag & PPARSE_TT_ATTR) || pparse_is_c23_attr(t) || t->kind == PPARSE_TK_PREP_DIR) {
			PParseToken *n = pparse_skip_noise(_pc, t);
			t = (n == t) ? pparse_next(_pc, t) : n;
			continue;
		}
		if (!first) out_char(' ');
		first = false;
		PParseToken *r = try_strip_raw(t);
		if (r) {
			t = r;
			continue;
		}
		OUT_TOK(t);
		t = pparse_next(_pc, t);
	}
}

static PParseToken *handle_open_brace(PParseToken *tok);
static PParseToken *handle_close_brace(PParseToken *tok);
static PParseToken *handle_defer_keyword(PParseToken *tok);
static PParseToken *handle_control_exit_defer(PParseToken *tok);
static PParseToken *handle_goto_keyword(PParseToken *tok);
static PParseToken *try_handle_defer_flow_kw(PParseToken *tok);
static void arm_ctrl_pending_from_tag(PParseToken *tok, uint32_t tag);
static inline PParseToken *try_process_stmt_token(PParseToken *t, PParseToken *end, PParseToken **unreachable_tok);
static inline void track_generic_token(PParseToken *tok);
static inline void track_ctrl_paren_open(void);
static inline void track_ctrl_paren_close(void);
static inline void track_ctrl_semicolon(void);
static PParseToken *emit_ctrl_condition(PParseToken *t, PParseToken **unreachable_tok);
static PParseToken *skip_stmt_prefixes(PParseToken *tok);
static PParseToken *emit_through(PParseToken *from, PParseToken *to);

typedef enum {
	EMIT_NORMAL = 0,
	EMIT_DEFER_BODY = 1,
} EmitMode;

static PParseToken *try_orelse_expr_rewrites(PParseToken *tok) {
	PPARSE_CTX();
	if (!pparse_feat(PPARSE_F_ORELSE)) return NULL;
	PParseToken *next = try_typeof_orelse(tok);
	if (next) return next;
	return try_bracket_orelse(tok);
}

enum { COLON_REQUIRE_BLOCK = 1 };

/* True if `prev` can end a label name before ':' — identifier, or the closing
 * delimiter of attrs between the name and colon (`done [[x]]:` /
 * `__attribute__`). */
static inline bool label_name_predecessor(PParseToken *prev) {
	PPARSE_CTX();
	if (!prev) return false;
	if (pparse_is_identifier_like(prev) || prev->kind == PPARSE_TK_NUM) return true;
	if (pparse_match_ch(prev, ']') && pparse_pair(_pc, prev) && (pparse_pair(_pc, prev)->flags & PPARSE_TF_C23_ATTR)) return true;
	if (pparse_match_ch(prev, ')') && pparse_pair(_pc, prev)) {
		PParseToken *before = pparse_walk_back(pparse_idx(_pc, pparse_pair(_pc, prev)), PPARSE_WB_ATTR_NOISE);
		return before && (before->tag & PPARSE_TT_ATTR);
	}
	return false;
}

/* Returns true if colon was consumed as a label/case terminator (caller should
 * continue). */
static bool consume_stmt_colon(PParseToken **tok_p, int *ternary_depth, bool *pending_case_colon, unsigned flags) {
	PParseToken *tok = *tok_p;
	if (!pparse_match_ch(tok, ':')) return false;
	if (*ternary_depth > 0) {
		(*ternary_depth)--;
		*pending_case_colon = false;
		return false;
	}
	if (!in_generic() && last_emitted && (label_name_predecessor(last_emitted) || *pending_case_colon) &&
	    !in_struct_body() && (!(flags & COLON_REQUIRE_BLOCK) || emit_block_depth > 0)) {
		*pending_case_colon = false;
		*tok_p = emit_advance(tok);
		emit_at_stmt_start = true;
		return true;
	}
	*pending_case_colon = false;
	return false;
}

/* Decl-context only: fold glibc C23 `_Generic(..., default: name(params))`
 * back to `(name)(params)`. Expression `_Generic` keeps SCOPE_GENERIC. */
static PParseToken *emit_generic_open(PParseToken *tok) {
	PPARSE_CTX();
	if (last_emitted &&
	    (pparse_match_ch(last_emitted, '*') || pparse_match_ch(last_emitted, ')') ||
	     (last_emitted->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_SKIP_DECL | PPARSE_TT_ATTR | PPARSE_TT_INLINE |
				   PPARSE_TT_STORAGE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT)) ||
	     pparse_is_known_typedef(last_emitted))) {
		PParseToken *name = NULL, *params_open = NULL, *params_close = NULL, *after = NULL;
		if (pparse_generic_decl_rewrite_target(tok, &name, &params_open, &params_close, &after)) {
			out_char('(');
			OUT_TOK(name);
			out_char(')');
			emit_range(params_open, pparse_next(_pc, params_close));
			PParseToken *gen_close = pparse_pair(_pc, pparse_next(_pc, tok));
			PParseToken *scan_start = gen_close;
			PParseToken *after_gen = pparse_skip_noise(_pc, pparse_next(_pc, gen_close));
			if (after_gen == params_open) scan_start = params_close;
			PPARSE_FOR_RANGE(a, pparse_next(_pc, scan_start), after) {
				emit_tok(a);
				last_emitted = a;
			}
			last_emitted = params_close ? params_close : name;
			emit_at_stmt_start = false;
			return after;
		}
	}
	tok = emit_advance(tok);
	if (tok && pparse_match_ch(tok, '(')) {
		scope_push_kind(SCOPE_GENERIC);
		tok = emit_advance(tok);
	}
	emit_at_stmt_start = false;
	return tok;
}

static PParseToken *emit_statements(PParseToken *tok, PParseToken *end, EmitMode mode) {
	PPARSE_CTX();
	PParseToken *unreachable_tok = NULL;
	int ternary_depth = 0;
	bool dr_braceless_body = false;
	bool pending_case_colon = false;
	while (tok && tok != end && tok->kind != PPARSE_TK_EOF) {
		if (pparse_match_ch(tok, '{')) {
			if (mode == EMIT_DEFER_BODY) {
				tok = emit_advance(tok);
				emit_at_stmt_start = true;
				dr_braceless_body = false;
				if (ctrl_state.pending) ctrl_reset();
			} else {
				tok = handle_open_brace(tok);
			}
			continue;
		}

		if (pparse_match_ch(tok, '}')) {
			if (mode == EMIT_DEFER_BODY) {
				tok = emit_advance(tok);
				emit_at_stmt_start = true;
				dr_braceless_body = false;
				if (ctrl_state.pending) ctrl_reset();
			} else {
				tok = handle_close_brace(tok);
			}
			continue;
		}

		if (pparse_match_ch(tok, ';')) {
			if (mode != EMIT_DEFER_BODY) end_statement_after_semicolon();
			bool is_ur = (tok == unreachable_tok);
			if (mode == EMIT_DEFER_BODY && dr_braceless_body) is_ur = false;
			tok = emit_advance(tok);
			if (is_ur) {
				EMIT_UNREACHABLE();
				unreachable_tok = NULL;
			}
			if (mode == EMIT_DEFER_BODY) {
				emit_at_stmt_start = true;
				dr_braceless_body = false;
				if (ctrl_state.pending) ctrl_reset();
			}
			continue;
		}

		if (pparse_match_ch(tok, '?')) ternary_depth++;
		if (consume_stmt_colon(&tok, &ternary_depth, &pending_case_colon, 0)) continue;

		if (__builtin_expect(tok->kind == PPARSE_TK_PREP_DIR, 0)) {
			tok = emit_advance(tok);
			continue;
		}

		{
			PParseToken *next = emit_gnu_label_decl(tok);
			if (next) {
				tok = next;
				continue;
			}
		}

		if ((tok->tag & PPARSE_TT_GENERIC) && !in_generic()) {
			tok = emit_generic_open(tok);
			continue;
		}

		if (mode != EMIT_DEFER_BODY) {
			PParseToken *next = try_handle_defer_flow_kw(tok);
			if (next) {
				tok = next;
				continue;
			}
		}

		if (emit_at_stmt_start && mode != EMIT_DEFER_BODY) check_enum_typedef_defer_shadow(tok);
		// Core statement dispatch: zeroinit, orelse, raw stripping, noreturn
		{
			PParseToken *next = try_process_stmt_token(tok, end, &unreachable_tok);
			if (next) {
				tok = next;
				continue;
			}
		}

		if (emit_at_stmt_start) {
			if (mode == EMIT_DEFER_BODY) {
				PParseToken *probe = tok;
				while (probe && probe->kind != PPARSE_TK_EOF) {
					PParseToken *sn = pparse_skip_noise(_pc, probe);
					if (sn != probe) {
						probe = sn;
						continue;
					}
					if (probe->tag &
					    (PPARSE_TT_INLINE | PPARSE_TT_TYPEDEF | PPARSE_TT_STORAGE | PPARSE_TT_QUALIFIER)) {
						probe = pparse_next(_pc, probe);
						if (probe && pparse_match_ch(probe, '(') && pparse_pair(_pc, probe))
							probe = pparse_next(_pc, pparse_pair(_pc, probe));
						continue;
					}
					break;
				}
				if (probe && (probe->tag & PPARSE_TT_SUE) && !pparse_is_known_typedef(probe)) {
					PParseToken *brace = pparse_find_struct_body_brace(probe);
					if (brace && pparse_pair(_pc, brace)) {
						PParseToken *close = pparse_pair(_pc, brace);
						while (tok && tok != end && tok->kind != PPARSE_TK_EOF) {
							PParseToken *cur = tok;
							tok = emit_advance(tok);
							if (cur == close) break;
						}
						emit_at_stmt_start = false;
						continue;
					}
				}
			} else if ((tok->tag & PPARSE_TT_SUE) && !pparse_is_known_typedef(tok)) {
				PParseToken *next = handle_sue_body(tok);
				if (next) {
					tok = next;
					continue;
				}
			}
		}

		if (emit_at_stmt_start && (tok->tag & (PPARSE_TT_IF | PPARSE_TT_LOOP | PPARSE_TT_SWITCH)) &&
		    !pparse_is_known_typedef(tok)) {
			if (pparse_is_else_or_do(tok)) {
				tok = emit_advance(tok);
				ctrl_state.pending = true;
				ctrl_state.parens_just_closed = true;
				emit_at_stmt_start = true;
				if (mode == EMIT_DEFER_BODY) dr_braceless_body = true;
				continue;
			}
			PParseToken *kw = tok;
			tok = emit_advance(tok);
			/* Emit attrs/_Pragma between keyword and condition '('. */
			while (tok && tok != end) {
				PParseToken *after = pparse_skip_noise(_pc, tok);
				if (after == tok) break;
				tok = emit_through(tok, after);
			}
			if (tok && pparse_match_ch(tok, '(') && pparse_pair(_pc, tok)) {
				/* Arm + push SCOPE_CTRL/FOR_PAREN so break/continue inside
         * condition stmt-exprs stop here (don't paste outer defers).
         * Main Pass 2 loop gets this via arm_ctrl + track_common. */
				arm_ctrl_pending_from_tag(kw, kw->tag);
				ctrl_state.parens_just_closed = false;
				tok = emit_ctrl_condition(tok, &unreachable_tok);
				emit_at_stmt_start = true;
				ctrl_state.pending = true;
				ctrl_state.parens_just_closed = true;
				if (mode == EMIT_DEFER_BODY) dr_braceless_body = true;
				continue;
			}
			continue;
		}

		track_generic_token(tok);
		/* Bounds before bracket-orelse: `a[i orelse 0]` must wrap the
		 * ternary index. try_bounds_check_subscript already lowers
		 * P1_OE_BRACKET indexes inside __prism_bchk; orelse-only
		 * brackets (declarator dims, uneval, non-arrays) still fall
		 * through to try_orelse_expr_rewrites. */
		EMIT_TRY_BOUNDS(tok)
		{
			PParseToken *next = try_orelse_expr_rewrites(tok);
			if (next) {
				tok = next;
				continue;
			}
		}

		if (tok->tag & (PPARSE_TT_CASE | PPARSE_TT_DEFAULT)) pending_case_colon = true;
		emit_at_stmt_start = false;
		tok = emit_advance(tok);
	}
	return tok;
}

static inline PParseToken *emit_stmt_expr(PParseToken *t) {
	PPARSE_CTX();
	emit_tok(t); // '('
	PParseToken *se_end = pparse_pair(_pc, t);
	PParseToken *inner = pparse_next(_pc, t); // '{'
	bool saved_ss = emit_at_stmt_start;
	CtrlState saved_ctrl = ctrl_state;
	inner = emit_statements(inner, se_end, EMIT_NORMAL);
	emit_at_stmt_start = saved_ss;
	ctrl_state = saved_ctrl;
	if (se_end) {
		emit_tok(se_end);
		return pparse_next(_pc, se_end);
	}
	return inner;
}

static PParseToken *emit_ctrl_condition(PParseToken *t, PParseToken **unreachable_tok) {
	PPARSE_CTX();
	PParseToken *close_p = pparse_pair(_pc, t);
	if (!close_p) {
		emit_tok(t);
		return pparse_next(_pc, t);
	}
	track_ctrl_paren_open();
	t = emit_advance(t); // emit '('
	emit_at_stmt_start = true;
	while (t && t != close_p && t->kind != PPARSE_TK_EOF) {
		PParseToken *next = try_process_stmt_token(t, close_p, unreachable_tok);
		if (next) {
			t = next;
			continue;
		}
		if ((t->flags & PPARSE_TF_OPEN) && pparse_is_stmt_expr_open(t) && pparse_pair(_pc, t)) {
			t = emit_stmt_expr(t);
			continue;
		}
		if ((t->flags & PPARSE_TF_OPEN) && pparse_pair(_pc, t)) {
			t = walk_balanced(t);
			emit_at_stmt_start = false;
			continue;
		}
		if (pparse_match_ch(t, ';')) {
			t = emit_advance(t);
			track_ctrl_semicolon();
			emit_at_stmt_start = true;
			continue;
		}
		EMIT_TRY_BOUNDS(t)
		emit_at_stmt_start = false;
		t = emit_advance(t);
	}
	if (t == close_p) {
		t = emit_advance(t);
	} // emit ')'
	track_ctrl_paren_close();
	return t;
}

static PParseToken *try_typeof_orelse(PParseToken *tok) {
	PPARSE_CTX();
	if (!(tok->tag & PPARSE_TT_TYPEOF) || !pparse_next(_pc, tok) || !pparse_match_ch(pparse_next(_pc, tok), '(') ||
	    !pparse_pair(_pc, pparse_next(_pc, tok)))
		return NULL;
	PParseToken *paren = pparse_next(_pc, tok);
	if (!(pparse_ann(paren) & P1_OE_BRACKET)) return NULL;
	emit_tok(tok);
	return walk_balanced_orelse(paren);
}

static PParseToken *try_bracket_orelse(PParseToken *tok) {
	PPARSE_CTX();
	if (!pparse_match_ch(tok, '[') || !(tok->flags & PPARSE_TF_OPEN) || !pparse_pair(_pc, tok)) return NULL;
	if (tok->flags & PPARSE_TF_C23_ATTR) return NULL;
	if (pparse_ann(tok) & P1_OE_BRACKET) return walk_balanced_orelse(tok);
	return NULL;
}

#define BOUNDS_COMM_DIAG(t, msg)                                                                             \
	do {                                                                                                 \
		if (pparse_feat(PPARSE_F_WARN_SAFETY)) {                                                                   \
			pparse_warn_tok((t), msg);                                                                  \
			return NULL;                                                                         \
		}                                                                                            \
		pparse_error_tok((t), msg);                                                                         \
	} while (0)

static PParseToken *try_bounds_check_deref_add(PParseToken *tok);

static PParseToken *try_bounds_check_subscript(PParseToken *tok) {
	PPARSE_CTX();
	if (!pparse_feat(PPARSE_F_BOUNDS_CHECK)) return NULL;
	if (!pparse_match_ch(tok, '[') || !(tok->flags & PPARSE_TF_OPEN) || !pparse_pair(_pc, tok)) return NULL;
	if (tok->flags & PPARSE_TF_C23_ATTR) return NULL;
	if (pparse_ann(tok) & (P1_DECL_BRACKET | P1_UNEVAL_BRACKET)) return NULL;
	/* Idempotence: an index already wrapped by a previous Prism pass
	 * (`a[__prism_bchk(...)]` — e.g. -save-temps .i re-compiles or
	 * --prism-verify's second pass) must not be wrapped again.  The
	 * __prism_ namespace is reserved, so this cannot fire on user code. */
	{
		PParseToken *idx0 = pparse_next(_pc, tok);
		if (idx0 && idx0->kind == PPARSE_TK_IDENT && idx0->len == 12 &&
		    prism_memeq_static(pparse_loc(_pc, idx0), "__prism_bchk", 12))
			return NULL;
	}
	// ALWAYS declarator dimensions per C11 6.7.2.1, never expression
	// brackets never get tagged P1_DECL_BRACKET via parse_declarator.
	if (in_struct_body()) return NULL;
	if (!last_emitted) return NULL;
	uint32_t ti = pparse_idx(_pc, tok);
	PParseToken *rp_prev = (ti >= 1) ? &pparse_token_pool[ti - 1] : NULL;
	if (rp_prev) {
		PParseToken *rp = rp_prev;
		if (pparse_match_ch(rp, ')') && (rp->flags & PPARSE_TF_CLOSE) && pparse_pair(_pc, rp)) {
			PParseToken *op = pparse_pair(_pc, rp);
			if (pparse_bounds_paren_derives_array(op)) BOUNDS_COMM_DIAG(tok, PPARSE_ERR_BOUNDS_DERIVED_SUB);
			/* `(a+i)[0]` must not wrap index 0 against sizeof(a) — the
			 * base is already offset. Same unverifiable shape as `*(a+i)`. */
			if (pparse_bounds_paren_has_array_arithmetic(op)) {
				BOUNDS_COMM_DIAG(tok, PPARSE_ERR_BOUNDS_PTR_ARITH_SUB);
			}
		}
	}

	// ISO C treats `(i)[b]` the same as `i[b]` (commutative subscript).
	if (rp_prev) {
		PParseToken *rp = rp_prev;
		if (pparse_match_ch(rp, ')') && pparse_pair(_pc, rp)) {
			PParseToken *op = pparse_pair(_pc, rp);
			PParseToken *idx = pparse_next(_pc, op);
			if (pparse_is_value_name_token(idx) && pparse_next(_pc, idx) == rp) {
				PParseToken *rb_close = pparse_pair(_pc, tok);
				PParseToken *inner = pparse_next(_pc, tok);
				if (rb_close && inner && pparse_next(_pc, inner) == rb_close &&
				    pparse_bounds_is_tracked_array(inner) && !pparse_bounds_is_tracked_array(idx))
					BOUNDS_COMM_DIAG(tok, PPARSE_ERR_BOUNDS_COMM_IDX_ARR);
			}
		}
	}

	// Commutative-subscript bypass check: ISO C defines `idx[arr]` as
	{
		PParseToken *rb_close = pparse_pair(_pc, tok);
		PParseToken *inner_first = pparse_next(_pc, tok);
		/* Parenthesized `idx[(&arr[0])]` and bare `idx[&arr[0]]` both derive
		 * an unchecked pointer; require rewrite to `arr[idx]`. */
		if (inner_first && rb_close && pparse_bounds_span_derives_array(inner_first, rb_close))
			BOUNDS_COMM_DIAG(tok, PPARSE_ERR_BOUNDS_COMM_DERIVED);
	}

	bool comma_resolved = false;
	{
		PParseToken *close_scan = pparse_pair(_pc, tok);
		PParseToken *inner = pparse_next(_pc, tok);
		PParseToken *iclose = close_scan;
		/* ISO `idx[(e1, arr)]` == `idx[arr]`; unwrap comma tail so the
     * main path sees the array ident.  If the last operand is the
     * tracked array, this is not a commutative *bypass* (it is
     * `arr` with index `idx` in disguise) — skip hard-pparse_error diags. */
		if (pparse_match_ch(inner, '(') && (inner->flags & PPARSE_TF_OPEN)) {
			PParseToken *pclose = pparse_pair(_pc, inner);
			if (pclose) {
				PParseToken *lastop = pparse_last_comma_operand(inner, pclose);
				if (lastop && lastop != pparse_next(_pc, inner) && pparse_next(_pc, lastop) == pclose) {
					inner = lastop;
					iclose = pclose;
					if (pparse_bounds_is_tracked_array(lastop)) comma_resolved = true;
				}
			}
		}
		if (!comma_resolved) {
			/* If last_emitted is a struct/union field access (preceded by
       * `.`/`->`), we do not know the field's type from the typedef
       * table, so we cannot assert a commutative bypass. Skip both
       * branches — the final brute-scan guard below is gated by the
       * same `memb` flag and handles this case correctly. */
			bool le_is_member = pparse_idx(_pc, last_emitted) >= 1 &&
					    (pparse_token_pool[pparse_idx(_pc, last_emitted) - 1].tag & PPARSE_TT_MEMBER);
			if (!le_is_member) {
				while (inner != iclose && pparse_match_ch(inner, '(') && (inner->flags & PPARSE_TF_OPEN) &&
				       pparse_pair(_pc, inner) && pparse_next(_pc, pparse_pair(_pc, inner)) == iclose) {
					iclose = pparse_pair(_pc, inner);
					inner = pparse_next(_pc, inner);
				}
				if (inner != iclose && pparse_next(_pc, inner) == iclose &&
				    pparse_bounds_is_tracked_array(inner)) {
					PParseToken *le = pparse_bounds_peel_paren_ident(last_emitted);
					if (!pparse_bounds_is_tracked_array(le) && !pparse_bounds_expr_base_is_pointer(le))
						BOUNDS_COMM_DIAG(tok, PPARSE_ERR_BOUNDS_COMM_IDX_ARR);
				}
				/* `ptr[arr[i]]` / `i[arr[j]]`: array name is itself
				 * subscripted — SPEC §6.10: not a commutative bypass;
				 * the inner `[` gets its own recursive wrap. */
			} // !le_is_member
		} // !comma_resolved
	}
	if (comma_resolved) BOUNDS_COMM_DIAG(tok, PPARSE_ERR_BOUNDS_COMMA_OP);
	{
		PParseToken *rb = pparse_pair(_pc, tok);
		/* `(i)[…]` / `(2)[…]`: last_emitted is `)`, peel to the index
		 * primary so ternary/arith hidden-array scans still fire. */
		PParseToken *le = pparse_bounds_peel_index_lhs(last_emitted);
		if (rb && (pparse_is_value_name_token(le) || le->kind == PPARSE_TK_NUM || last_emitted->kind == PPARSE_TK_NUM)) {
			bool memb = pparse_idx(_pc, le) >= 1 && (pparse_token_pool[pparse_idx(_pc, le) - 1].tag & PPARSE_TT_MEMBER);
			bool left_ok_scan = !memb && le->kind != PPARSE_TK_NUM && !pparse_is_known_typedef(le) &&
					    pparse_bounds_is_tracked_array(le);
			PParseToken *hit = left_ok_scan ? NULL : pparse_bounds_find_tracked_array(pparse_next(_pc, tok), rb);
			while (hit) {
				PParseToken *nx = pparse_next(_pc, hit);
				if (!(nx && pparse_match_ch(nx, '[') && (nx->flags & PPARSE_TF_OPEN))) break;
				hit = pparse_bounds_find_tracked_array(pparse_next(_pc, nx), rb);
			}
			if (hit) BOUNDS_COMM_DIAG(tok, PPARSE_ERR_BOUNDS_COMM_SCAN);
			/* `0[(a+i)]` — array arith in index paren */
			PParseToken *inner0 = pparse_next(_pc, tok);
			if (!left_ok_scan && inner0 && pparse_match_ch(inner0, '(') && (inner0->flags & PPARSE_TF_OPEN) &&
			    pparse_pair(_pc, inner0) && pparse_next(_pc, pparse_pair(_pc, inner0)) == rb &&
			    pparse_bounds_paren_has_array_arithmetic(inner0)) {
				BOUNDS_COMM_DIAG(tok, PPARSE_ERR_BOUNDS_PTR_ARITH_SUB);
			}
		}
	}
	PParseToken *name_tok = last_emitted;
	if (pparse_idx(_pc, tok) >= 1) {
		PParseToken *pool_prev = &pparse_token_pool[pparse_idx(_pc, tok) - 1];
		if (pool_prev != last_emitted &&
		    (pparse_match_ch(pool_prev, ']') || pparse_match_ch(pool_prev, ')') || pparse_is_value_name_token(pool_prev)))
			name_tok = pool_prev;
	}
	int dim_depth = 0;
	while (1) {
		if (pparse_match_ch(name_tok, ')') && pparse_pair(_pc, name_tok)) {
			PParseToken *open = pparse_pair(_pc, name_tok);
			if (pparse_idx(_pc, open) >= 1) {
				PParseToken *bp = &pparse_token_pool[pparse_idx(_pc, open) - 1];
				if (pparse_is_value_name_token(bp) || bp->kind == PPARSE_TK_NUM || pparse_match_ch(bp, ')') ||
				    pparse_match_ch(bp, ']'))
					break;
			}
			PParseToken *inner = pparse_next(_pc, open);
			if (!inner) break;
			if (pparse_is_value_name_token(inner) && pparse_next(_pc, inner) == name_tok) {
				name_tok = inner;
				break;
			}
			if (pparse_match_ch(inner, '(') && pparse_pair(_pc, inner) &&
			    pparse_next(_pc, pparse_pair(_pc, inner)) == name_tok) {
				name_tok = pparse_pair(_pc, inner);
				continue;
			}
			if (pparse_idx(_pc, name_tok) >= 1) {
				PParseToken *inner_last = &pparse_token_pool[pparse_idx(_pc, name_tok) - 1];
				if (pparse_match_ch(inner_last, ']')) {
					name_tok = inner_last;
					continue;
				}
			}
			break;
		}
		if (pparse_match_ch(name_tok, ']') && pparse_pair(_pc, name_tok)) {
			PParseToken *open_br = pparse_pair(_pc, name_tok);
			if (pparse_ann(open_br) & (P1_DECL_BRACKET | P1_UNEVAL_BRACKET)) break;
			if (open_br->flags & PPARSE_TF_C23_ATTR) break;
			if (pparse_idx(_pc, open_br) < 1) break;
			PParseToken *before = &pparse_token_pool[pparse_idx(_pc, open_br) - 1];
			if (!(pparse_is_value_name_token(before) || pparse_match_ch(before, ')') || pparse_match_ch(before, ']')))
				break;
			name_tok = before;
			dim_depth++;
			continue;
		}
		break;
	}
	if (!pparse_is_value_name_token(name_tok) && last_emitted) {
		PParseToken *probe = last_emitted;
		while (probe && pparse_match_ch(probe, ')') && pparse_pair(_pc, probe)) {
			PParseToken *op = pparse_pair(_pc, probe);
			if (pparse_bounds_paren_has_array_arithmetic(op)) {
				BOUNDS_COMM_DIAG(tok, PPARSE_ERR_BOUNDS_PTR_ARITH_SUB);
			}
			PParseToken *hit = pparse_bounds_find_tracked_array(pparse_next(_pc, op), probe);
			if (hit && pparse_bounds_is_tracked_array(hit)) {
				name_tok = hit;
				break;
			}
			if (pparse_idx(_pc, op) < 1) break;
			probe = &pparse_token_pool[pparse_idx(_pc, op) - 1];
		}
	}
	if (!pparse_is_value_name_token(name_tok)) return NULL;
	if (pparse_is_known_typedef(name_tok)) return NULL;
	if (pparse_idx(_pc, name_tok) >= 1 && (pparse_token_pool[pparse_idx(_pc, name_tok) - 1].tag & PPARSE_TT_MEMBER)) return NULL;
	if (pparse_idx(_pc, name_tok) >= 1) {
		PParseToken *pv = &pparse_token_pool[pparse_idx(_pc, name_tok) - 1];
		if (pv->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF)) return NULL;
		if (pparse_match_ch(pv, '*') && !(pv->flags & PPARSE_TF_OPEN) && pparse_idx(_pc, pv) >= 1) {
			PParseToken *pp = &pparse_token_pool[pparse_idx(_pc, pv) - 1];
			bool is_decl_star = (pp->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF)) ||
					    (pparse_match_ch(pp, '*') && !(pp->flags & PPARSE_TF_OPEN)) ||
					    (pparse_match_ch(pp, '(') && (pp->flags & PPARSE_TF_OPEN));
			if (is_decl_star) return NULL;
		}
	}
	PParseArrayBindingInfo array = pparse_array_binding_info(name_tok);
	if (!array.tracked) return NULL;
	if (array.rank > 0 && array.rank != PPARSE_ARRAY_RANK_WRAP_ALL && dim_depth >= array.rank) return NULL;
	if (pparse_idx(_pc, name_tok) >= 1) {
		PParseToken *operand_start = name_tok;
		PParseToken *operand_end = name_tok;
		PParseToken *prev = &pparse_token_pool[pparse_idx(_pc, operand_start) - 1];
		while (pparse_match_ch(prev, '(') && (prev->flags & PPARSE_TF_OPEN) && pparse_pair(_pc, prev)) {
			PParseToken *rp = pparse_pair(_pc, prev);
			if (!rp || pparse_next(_pc, prev) != operand_start || pparse_next(_pc, operand_end) != rp) break;
			operand_start = prev;
			operand_end = rp;
			if (pparse_idx(_pc, prev) < 1) {
				prev = NULL;
				break;
			}
			prev = &pparse_token_pool[pparse_idx(_pc, prev) - 1];
		}
		if (prev && pparse_match_ch(prev, '&') && !(prev->flags & PPARSE_TF_OPEN)) {
			bool unary = true;
			if (pparse_idx(_pc, prev) >= 1) {
				PParseToken *pp = &pparse_token_pool[pparse_idx(_pc, prev) - 1];
				/* `)` after a cast type-name `(void)` / `(int *)` still
				 * leaves `&` unary — `(void)&a[n]` is one-past-legal.
				 * Expression `)` (`(x)&a[i]`) is binary `&`. */
				if (pparse_is_value_name_token(pp) || pp->kind == PPARSE_TK_NUM || pp->kind == PPARSE_TK_STR ||
				    pparse_match_ch(pp, ']') ||
				    (pparse_match_ch(pp, ')') && !pparse_close_paren_ends_cast_type_name(pp)))
					unary = false;
			}
			if (unary) return NULL;
		}
	}

	PParseToken *arr_tok = name_tok;
	PParseToken *close = pparse_pair(_pc, tok);
	/* Pointer-cast then subscript: bound with cast element size via
	 * sizeof(arr)/sizeof(*(T *)arr), not sizeof(arr)/sizeof(arr[0]).
	 * Walk outward so the outermost cast wins: `((int*)(void*)a)[i]` must
	 * use `*(int*)…`, not the inner `*(void*)`. Also accept pointer-to-array
	 * casts `(T(*)[N])` whose last token before `)` is `]`. */
	PParseToken *cast_close = NULL; /* ')' of outermost (T *) / (T(*)[N]) */
	{
		PParseToken *probe = arr_tok;
		for (int peel = 0; peel < 8 && pparse_idx(_pc, probe) >= 1; peel++) {
			PParseToken *prev = &pparse_token_pool[pparse_idx(_pc, probe) - 1];
			if (pparse_match_ch(prev, ')') && pparse_pair(_pc, prev)) {
				PParseToken *open = pparse_pair(_pc, prev);
				PParseToken *last = pparse_idx(_pc, prev) >= 1 ? &pparse_token_pool[pparse_idx(_pc, prev) - 1] : NULL;
				if (last && (pparse_match_ch(last, '*') || pparse_match_ch(last, ']'))) {
					PParseToken *fi = pparse_skip_noise(_pc, pparse_next(_pc, open));
					bool looks_cast =
					    fi && (pparse_is_type_keyword(fi) ||
						   (fi->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF)) ||
						   pparse_is_known_typedef(fi));
					if (looks_cast) {
						cast_close = prev;
						probe = open;
						continue;
					}
				}
				if (pparse_next(_pc, open) == probe && pparse_next(_pc, probe) == prev) {
					probe = open;
					continue;
				}
			}
			break;
		}
	}
	OUT_LIT("[__prism_bchk((__prism_bchk_size_t)(");
	if (pparse_feat(PPARSE_F_ORELSE) && (pparse_ann(tok) & P1_OE_BRACKET))
		emit_token_range_orelse(pparse_next(_pc, tok), close);
	else {
		for (PParseToken *t = pparse_next(_pc, tok); t != close && t->kind != PPARSE_TK_EOF;) {
			if ((t->flags & PPARSE_TF_OPEN) && pparse_is_stmt_expr_open(t)) {
				t = emit_stmt_expr(t);
				continue;
			}
			EMIT_TRY_BOUNDS(t)
			if ((t->flags & PPARSE_TF_OPEN) && pparse_pair(_pc, t)) {
				t = walk_balanced(t);
				continue;
			}
			t = emit_advance(t);
		}
	}
	OUT_LIT("), sizeof(");
	out_str(pparse_loc(_pc, arr_tok), arr_tok->len);
	for (int d = 0; d < dim_depth; d++) OUT_LIT("[0]");
	OUT_LIT(")/sizeof(");
	if (cast_close) {
		/* sizeof(*(T *)arr) — cast_close is ')' of (T *) before arr. */
		PParseToken *cast_open = pparse_pair(_pc, cast_close);
		OUT_LIT("(*");
		PPARSE_FOR_RANGE(ct, cast_open, arr_tok)
			emit_tok(ct);
		out_str(pparse_loc(_pc, arr_tok), arr_tok->len);
		OUT_LIT(")");
	} else {
		out_str(pparse_loc(_pc, arr_tok), arr_tok->len);
		for (int d = 0; d <= dim_depth; d++) OUT_LIT("[0]");
	}
	OUT_LIT("))]");
	return pparse_next(_pc, close);
}

static PParseToken *try_bounds_check_deref_add(PParseToken *tok) {
	PPARSE_CTX();
	if (!pparse_feat(PPARSE_F_BOUNDS_CHECK)) return NULL;
	if (!pparse_match_ch(tok, '*') || (tok->flags & PPARSE_TF_OPEN)) return NULL;
	if (pparse_token_is_in_unevaluated_operand(tok)) return NULL;
	if (pparse_idx(_pc, tok) >= 1) {
		PParseToken *prev = &pparse_token_pool[pparse_idx(_pc, tok) - 1];
		if (prev->kind == PPARSE_TK_NUM || prev->kind == PPARSE_TK_STR) return NULL;
		if (prev->kind == PPARSE_TK_IDENT && !(prev->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF)) &&
		    !(prev->tag & (PPARSE_TT_RETURN | PPARSE_TT_GOTO | PPARSE_TT_DEFER))) return NULL;
		if (pparse_match_ch(prev, ']')) return NULL;
		if (pparse_match_ch(prev, ')') && (prev->flags & PPARSE_TF_CLOSE)) {
			PParseToken *om = pparse_pair(_pc, prev);
			PParseToken *fi = om ? pparse_next(_pc, om) : NULL;
			bool looks_cast = fi && (pparse_is_type_keyword(fi) ||
						 (fi->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF)));
			if (!looks_cast) return NULL;
		}
	}

	PParseToken *op = pparse_next(_pc, tok);
	if (!op || !pparse_match_ch(op, '(') || !(op->flags & PPARSE_TF_OPEN) || !pparse_pair(_pc, op)) return NULL;
	/* Peel casts: `*(T*)(a+i)` / `*(int *)(a+i)` — otherwise the cast
	 * group hides the additive paren and the check never fires. */
	for (;;) {
		PParseToken *cast_close = pparse_pair(_pc, op);
		PParseToken *fi = pparse_next(_pc, op);
		bool looks_cast =
		    fi && (pparse_is_type_keyword(fi) || (fi->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF)));
		if (!looks_cast) break;
		PParseToken *after = pparse_next(_pc, cast_close);
		if (!after || !pparse_match_ch(after, '(') || !(after->flags & PPARSE_TF_OPEN) || !pparse_pair(_pc, after))
			return NULL;
		op = after;
	}
	PParseToken *cp = pparse_pair(_pc, op);
	PParseToken *lhs = pparse_next(_pc, op);
	if (!lhs || lhs == cp) return NULL;
	/* Peel redundant outer paren layers: `*((a + 1))` → scan `a + 1`.
	 * Without this, the inner `(a + 1)` is skipped as a nested group and
	 * the unverifiable pointer arithmetic bypasses -fbounds-check. */
	PParseToken *scan_end = cp;
	while (lhs && pparse_match_ch(lhs, '(') && (lhs->flags & PPARSE_TF_OPEN) && pparse_pair(_pc, lhs)) {
		PParseToken *inner_cp = pparse_pair(_pc, lhs);
		if (!inner_cp || pparse_next(_pc, inner_cp) != scan_end) break;
		lhs = pparse_next(_pc, lhs);
		scan_end = inner_cp;
		if (!lhs || lhs == scan_end) return NULL;
	}
	/* Peel a leading cast inside the grouping: `*((T*)(a+i))` — the cast
	 * open is not a redundant whole-body wrap, so the loop above stops on
	 * it and would otherwise skip both `(T*)` and `(a+i)` as nested groups. */
	for (;;) {
		if (!lhs || !pparse_match_ch(lhs, '(') || !(lhs->flags & PPARSE_TF_OPEN) || !pparse_pair(_pc, lhs)) break;
		PParseToken *cast_close = pparse_pair(_pc, lhs);
		PParseToken *fi = pparse_next(_pc, lhs);
		bool looks_cast =
		    fi && (pparse_is_type_keyword(fi) || (fi->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF)));
		if (!looks_cast) break;
		PParseToken *after = pparse_next(_pc, cast_close);
		if (!after || !pparse_match_ch(after, '(') || !(after->flags & PPARSE_TF_OPEN) || !pparse_pair(_pc, after))
			return NULL;
		PParseToken *add_close = pparse_pair(_pc, after);
		if (!add_close || pparse_next(_pc, add_close) != scan_end) break;
		lhs = pparse_next(_pc, after);
		scan_end = add_close;
		if (!lhs || lhs == scan_end) return NULL;
	}
	// Must contain a top-level `+` or `-` operator inside the parens.
	bool has_addsub = false;
	PPARSE_FOR_RANGE(t, lhs, scan_end) {
		PPARSE_SKIP_GROUP_ON_CLOSE(t)
		if ((pparse_match_ch(t, '+') || pparse_match_ch(t, '-')) && !(t->flags & PPARSE_TF_OPEN)) {
			has_addsub = true;
			break;
		}
	}
	if (!has_addsub) return NULL;
	// Must reference a tracked array name somewhere in the parens.
	PParseToken *hit = pparse_bounds_find_tracked_array(lhs, scan_end);
	if (!hit) return NULL;
	BOUNDS_COMM_DIAG(tok, PPARSE_ERR_BOUNDS_PTR_ARITH_DEREF);
	return NULL;
}

static inline PParseToken *try_bounds_checks(PParseToken *t) {
	/* Only '[' (subscript) and '*' (pointer-arith deref) can be bounds sites;
	 * cheap ch0 gate avoids two call+pparse_feat+pparse_match_ch per ordinary token. */
	if (t->ch0 != '[' && t->ch0 != '*') return NULL;
	PParseToken *n = try_bounds_check_subscript(t);
	return n ? n : try_bounds_check_deref_add(t);
}

static inline void reject_defer_in_expr_context(PParseToken *t) {
	PPARSE_CTX();
	/* Release+debug: mid-expression `defer` (e.g. `x=1, defer g();` /
	 * `return defer g(), 0;`) must not leak the keyword to the C backend.
	 * Statement-form defer is consumed by handle_defer_keyword and never
	 * reaches emit_tok. */
	if (__builtin_expect(pparse_feat(PPARSE_F_DEFER) && (t->tag & PPARSE_TT_DEFER), 0) && !pparse_is_known_function_call(t) &&
	    !(last_emitted && (last_emitted->tag & PPARSE_TT_MEMBER)) &&
	    (!pparse_token_has_binding(t) || pparse_match_ch(pparse_next(_pc, t), '{')) && pparse_next(_pc, t) &&
	    (pparse_is_identifier_like(pparse_next(_pc, t)) || pparse_match_ch(pparse_next(_pc, t), '{')))
		pparse_error_tok(t,
			  "'defer' cannot be used in expression context "
			  "(comma expressions, return values, array dimensions, etc.)");
}

static inline bool walk_balanced_tail(PParseToken **tp) {
	PPARSE_CTX();
	PParseToken *t = *tp;
	if (pparse_feat(PPARSE_F_DEFER) && defer_count > 0 && pparse_is_enum_kw(t)) {
		PParseToken *brace = pparse_find_struct_body_brace(t);
		if (brace) check_enum_body_defer_shadow(brace);
	}
	reject_defer_in_expr_context(t);
	*tp = emit_advance(t);
	return true;
}

/* Emit a balanced group. The non-emitting form was identical to
 * pparse_skip_balanced_group, so it is gone and callers use that. */
static PRISM_HOT PParseToken *walk_balanced(PParseToken *tok) {
	PPARSE_CTX();
	PParseToken *end = pparse_pair(_pc, tok);
	if (!end) return pparse_next(_pc, tok);
	{
		for (PParseToken *t = tok; t != pparse_next(_pc, end) && t->kind != PPARSE_TK_EOF;) {
			if ((t->flags & PPARSE_TF_OPEN) && pparse_is_stmt_expr_open(t)) {
				t = emit_stmt_expr(t);
				continue;
			}
			EMIT_TRY_BOUNDS(t)
			if (pparse_feat(PPARSE_F_ORELSE) && (t->flags & PPARSE_TF_OPEN) && pparse_match_ch(t, '[') && pparse_pair(_pc, t)) {
				// C23 [[ ... ]]: verbatim interior; Phase 1D rejects orelse
				// inside attrs. Skip FIFO bracket-orelse path (same as before).
				if (t->flags & PPARSE_TF_C23_ATTR) {
					t = emit_c23_attr(t);
					continue;
				}
				{
					PParseToken *next = try_bracket_orelse(t);
					if (next) {
						t = next;
						continue;
					}
				}
				// No orelse — emit bracket contents with raw stripping
				PParseToken *bclose = pparse_pair(_pc, t);
				while (t != bclose) {
					if ((t->flags & PPARSE_TF_OPEN) && pparse_is_stmt_expr_open(t)) {
						t = emit_stmt_expr(t);
						continue;
					}
					t = emit_advance(t);
				}
				t = emit_advance(t);
				continue;
			}
			EMIT_TRY_TYPEOF_ORELSE(t)
			// Enum-body shadow queue + defense-in-depth defer rejection
			walk_balanced_tail(&t);
		}
	}
	return pparse_next(_pc, end);
}

static void emit_token_range_nested(PParseToken *start, PParseToken *end, bool with_orelse) {
	PPARSE_CTX();
	for (PParseToken *t = start; t && t != end && t->kind != PPARSE_TK_EOF;) {
		EMIT_SKIP_STMT_EXPR(t)
		/* Subscripts inside orelse-lowered ranges (single-eval LHS copies,
		 * fallback values) must get the same -fbounds-check wrapping as
		 * the main emission loop — otherwise `v[i] = g() orelse 1;`
		 * stores through a tracked array unchecked.  Found by the
		 * contexts suite's fixed-point oracle. */
		EMIT_TRY_BOUNDS(t)
		if ((t->flags & PPARSE_TF_OPEN) && (pparse_match_ch(t, '(') || pparse_match_ch(t, '['))) {
			PParseToken *close = pparse_pair(_pc, t);
			if (close && close != end) {
				emit_tok(t);
				if (with_orelse) emit_token_range_orelse(pparse_next(_pc, t), close);
				else
					emit_token_range_nested(pparse_next(_pc, t), close, false);
				emit_tok(close);
				t = pparse_next(_pc, close);
				continue;
			}
		}
		reject_defer_in_expr_context(t);
		t = emit_advance(t);
	}
}

/* First P1_IS_ORELSE_KW token in [start, end) at group depth 0 (jumps
 * balanced groups, skips prep dirs); NULL if none. */
static PParseToken *find_ann_orelse(PParseToken *start, PParseToken *end) {
	PPARSE_CTX();
	PPARSE_FOR_RANGE(s, start, end) {
		if (s->kind == PPARSE_TK_PREP_DIR) continue;
		PPARSE_SKIP_GROUP_ON_CLOSE(s)
		if (pparse_ann(s) & P1_IS_ORELSE_KW) return s;
	}
	return NULL;
}

static void emit_token_range_orelse(PParseToken *start, PParseToken *end) {
	PPARSE_CTX();
	PParseToken *orelse = find_ann_orelse(start, end);
	if (!orelse) {
		emit_token_range_nested(start, end, true);
		return;
	}
	emit_orelse_ternary(start, orelse, pparse_next(_pc, orelse), end);
}

static PParseToken *scan_bracket_orelse(PParseToken *open, PParseToken *close, PParseToken **paren_open_out) {
	PPARSE_CTX();
	PPARSE_FOR_RANGE(t, pparse_next(_pc, open), close) {
		if (t->kind == PPARSE_TK_PREP_DIR) continue;
		if (t->flags & PPARSE_TF_OPEN) {
			PParseToken *pp = pparse_pair(_pc, t);
			/* `( … orelse … )` ending flush at `]` — macro-hygiene wrap. */
			if (pparse_match_ch(open, '[') && pparse_match_ch(t, '(') && pp && pparse_next(_pc, pp) == close) {
				PParseToken *hit = find_ann_orelse(pparse_next(_pc, t), pp);
				if (hit) {
					if (paren_open_out) *paren_open_out = t;
					return hit;
				}
			}
			t = pp;
			continue;
		}
		if (pparse_ann(t) & P1_IS_ORELSE_KW) return t;
	}
	return NULL;
}

typedef struct {
	int oe_count, oe_next, dim_count, dim_next;
	unsigned *oe_ids, *dim_ids; // arena-allocated snapshots (unbounded)
} BOFrame;

static void bo_snapshot_ids(unsigned **dst, const unsigned *src, int n) {
	PPARSE_CTX();
	if (n <= 0) {
		*dst = NULL;
		return;
	}
	*dst = pparse_arena_alloc(&_pc->main_arena, (size_t)n * sizeof(unsigned));
	memcpy(*dst, src, (size_t)n * sizeof(unsigned));
}

static inline void emit_type_with_pragma_prelude(
    PParseToken *pragma_start, PParseToken *type_start, PParseToken *type_end, PParseToken *raw_tok, bool is_split) {
	PRISM_STATE();
	PPARSE_CTX();
	int saved_oe = _ps->bracket_oe_count, saved_dim = _ps->bracket_dim_count;
	_ps->bracket_oe_count = 0;
	_ps->bracket_dim_count = 0;
	/* The prelude emits everything before the type range (attributes, #pragma
	 * noise).  `raw_tok` bounds it only when the raw token actually PRECEDES
	 * the type range — `__attribute__((x)) raw int v;`.  When raw sits inside
	 * the specifiers — `__attribute__((x)) const raw int v;` — those
	 * specifiers belong to emit_type_range, and using raw_tok as the bound
	 * emitted them twice: `const const int v;`, invalid C that Phase 1
	 * accepted.  (Found by the generative raw product's raw-vs-zeroinit-off
	 * equivalence oracle; `static`/`extern` were unaffected because they take
	 * the has-storage verbatim path.)
	 *
	 * Order is decided by a bounded walk, not by comparing token addresses or
	 * pparse_idx: the stream is not contiguous across PPARSE_TF_LINK_JUMP
	 * hops, so index comparison silently misorders tokens from different pool
	 * segments. */
	PParseToken *prelude_end = type_start;
	if (raw_tok) {
		int budget = 64;
		for (PParseToken *p = pragma_start; p && p != type_start && budget-- > 0;
		     p = pparse_next(_pc, p)) {
			if (p == raw_tok) {
				prelude_end = raw_tok;
				break;
			}
		}
	}
	if (pragma_start != prelude_end) emit_range(pragma_start, prelude_end);
	emit_type_range(type_start, type_end, false, is_split);
	_ps->bracket_oe_count = saved_oe;
	_ps->bracket_dim_count = saved_dim;
}

static void bo_restore_queue(unsigned **dst, int *dst_cap, const unsigned *src, int count) {
	PPARSE_CTX();
	if (count <= 0) return;
	if (*dst_cap < count) {
		int old_cap = *dst_cap;
		size_t nc = pparse_vec_grow_cap((size_t)old_cap, (size_t)count, 16);
		*dst = pparse_arena_realloc(&_pc->main_arena,
				     *dst,
				     (size_t)old_cap * sizeof(unsigned),
				     nc * sizeof(unsigned));
		*dst_cap = (int)nc;
	}
	memcpy(*dst, src, (size_t)count * sizeof(unsigned));
}

static void bo_restore(BOFrame *f) {
	PRISM_STATE();
	_ps->bracket_oe_count = f->oe_count;
	_ps->bracket_oe_next = f->oe_next;
	_ps->bracket_dim_count = f->dim_count;
	_ps->bracket_dim_next = f->dim_next;
	bo_restore_queue(&_ps->bracket_oe_ids, &_ps->bracket_oe_cap, f->oe_ids, f->oe_count);
	bo_restore_queue(&_ps->bracket_dim_ids, &_ps->bracket_dim_cap, f->dim_ids, f->dim_count);
}

// are also hoisted to preserve C99 left-to-right VLA evaluation order.
static void emit_bracket_orelse_temps(PParseToken *start, PParseToken *end) {
	PRISM_STATE();
	PPARSE_CTX();
	_ps->bracket_oe_count = 0;
	_ps->bracket_oe_next = 0;
	_ps->bracket_dim_count = 0;
	_ps->bracket_dim_next = 0;

	// Phase 1: collect all brackets and identify which have orelse.
	typedef struct {
		PParseToken *open;
		PParseToken *close;
		PParseToken *orelse;
		PParseToken *paren_open;
	} BracketInfo;

	PParseArenaMark bracket_mark = pparse_arena_mark(&_pc->main_arena);
	int bracket_cap = 16;
	BracketInfo *brackets = pparse_arena_alloc_uninit(&_pc->main_arena, bracket_cap * sizeof(BracketInfo));
	int bracket_count = 0;
	bool any_orelse = false;
	PPARSE_FOR_RANGE(t, start, end) {
		if (t->tag & PPARSE_TT_ATTR) {
			PParseToken *a = pparse_next(_pc, t);
			if (a && pparse_match_ch(a, '(') && pparse_pair(_pc, a)) t = pparse_pair(_pc, a);
			continue;
		}
		if (!pparse_match_ch(t, '[')) continue;
		if (t->flags & PPARSE_TF_C23_ATTR) {
			t = pparse_pair(_pc, t);
			continue;
		}
		PParseToken *close = pparse_pair(_pc, t);
		if (!close) continue;
		PPARSE_ARENA_ENSURE_CAP(
		    &_pc->main_arena, brackets, bracket_count + 1, bracket_cap, 16, BracketInfo);
		PParseToken *paren_open_found = NULL;
		PParseToken *orelse_found = scan_bracket_orelse(t, close, &paren_open_found);
		brackets[bracket_count++] = (BracketInfo){t, close, orelse_found, paren_open_found};
		if (orelse_found) any_orelse = true;
		t = close;
	}

	if (!any_orelse) {
		pparse_arena_restore(&_pc->main_arena, bracket_mark);
		return;
	}

	for (int i = 0; i < bracket_count; i++) {
		if (brackets[i].orelse) {
			// Orelse bracket: hoist LHS
			PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena,
					 _ps->bracket_oe_ids,
					 _ps->bracket_oe_count,
					 _ps->bracket_oe_cap,
					 16,
					 unsigned);
			unsigned oe = _ps->ret_counter++;
			_ps->bracket_oe_ids[_ps->bracket_oe_count++] = oe;
			emit_ll_temp(emit_prism_oe, oe);
			emit_token_range_orelse(brackets[i].paren_open ? pparse_next(_pc, brackets[i].paren_open)
								       : pparse_next(_pc, brackets[i].open),
						brackets[i].orelse);
			OUT_LIT(");");
		} else {
			// hoist to preserve left-to-right VLA evaluation order.
			PParseToken *dim_start = pparse_next(_pc, brackets[i].open);
			PParseToken *dim_end = brackets[i].close;
			PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena,
					 _ps->bracket_dim_ids,
					 _ps->bracket_dim_count,
					 _ps->bracket_dim_cap,
					 16,
					 unsigned);
			if (dim_start == dim_end ||
			    (pparse_next(_pc, dim_start) == dim_end &&
			     (dim_start->kind == PPARSE_TK_NUM || pparse_match_ch(dim_start, '*')))) {
				_ps->bracket_dim_ids[_ps->bracket_dim_count++] = (unsigned)-1;
				continue;
			}
			unsigned dim = _ps->ret_counter++;
			_ps->bracket_dim_ids[_ps->bracket_dim_count++] = dim;
			emit_ll_temp(emit_prism_dim, dim);
			emit_token_range_orelse(dim_start, dim_end);
			OUT_LIT(");");
		}
	}
	pparse_arena_restore(&_pc->main_arena, bracket_mark);
}

static PParseToken *walk_balanced_orelse(PParseToken *tok) {
	PRISM_STATE();
	PPARSE_CTX();
	PParseToken *end = pparse_pair(_pc, tok);
	if (!end) {
		emit_tok(tok);
		return pparse_next(_pc, tok);
	}
	PParseToken *paren_open = NULL;
	PParseToken *orelse_found = scan_bracket_orelse(tok, end, &paren_open);
	if (!orelse_found) {
		if (pparse_match_ch(tok, '[') && _ps->bracket_dim_next < _ps->bracket_dim_count) {
			unsigned dim = _ps->bracket_dim_ids[_ps->bracket_dim_next++];
			if (dim != (unsigned)-1) {
				emit_tok(tok); // emit [
				emit_prism_dim(dim);
				emit_tok(end); // emit ]
				return pparse_next(_pc, end);
			}
		}
		for (PParseToken *t = tok; t != pparse_next(_pc, end) && t->kind != PPARSE_TK_EOF;) {
			if (t != tok && t != end && (pparse_match_ch(t, '(') || pparse_match_ch(t, '[')) &&
			    (t->flags & PPARSE_TF_OPEN) && pparse_pair(_pc, t) && !pparse_is_stmt_expr_open(t)) {
				if (pparse_match_ch(t, '[') &&
				    _ps->bracket_dim_next < _ps->bracket_dim_count) {
					unsigned dim = _ps->bracket_dim_ids[_ps->bracket_dim_next++];
					if (dim != (unsigned)-1) {
						emit_tok(t);
						emit_prism_dim(dim);
						emit_tok(pparse_pair(_pc, t));
						t = pparse_next(_pc, pparse_pair(_pc, t));
						continue;
					}
				}
				t = walk_balanced_orelse(t);
				continue;
			}
			if (t != tok && t != end && pparse_feat(PPARSE_F_ORELSE) && (t->tag & PPARSE_TT_TYPEOF) && pparse_next(_pc, t) &&
			    pparse_match_ch(pparse_next(_pc, t), '(')) {
				emit_tok(t);	 // typeof keyword
				t = pparse_next(_pc, t); // (
				t = walk_balanced_orelse(t);
				continue;
			}
			// handles zero-init, defer, goto, raw stripping, etc.
			if (t != tok && t != end && (t->flags & PPARSE_TF_OPEN) && pparse_is_stmt_expr_open(t)) {
				t = walk_balanced(t);
				continue;
			}
			EMIT_TRY_BOUNDS(t)
			walk_balanced_tail(&t);
		}
		return pparse_next(_pc, end);
	}
	PParseToken *lhs_start = paren_open ? pparse_next(_pc, paren_open) : pparse_next(_pc, tok);
	PParseToken *rhs_start = pparse_next(_pc, orelse_found);
	PParseToken *rhs_end = paren_open ? pparse_pair(_pc, paren_open) : end;
	bool is_bracket = pparse_match_ch(tok, '[');
	emit_tok(tok); // emit [ or (
	if (is_bracket && _ps->bracket_oe_next < _ps->bracket_oe_count) {
		unsigned oe = _ps->bracket_oe_ids[_ps->bracket_oe_next++];
		emit_prism_oe(oe);
		OUT_LIT(" ?");
		emit_prism_oe(oe);
		OUT_LIT(" : (");
		emit_token_range_orelse(rhs_start, rhs_end);
		OUT_LIT(")");
	} else {
		emit_orelse_ternary(lhs_start, orelse_found, rhs_start, rhs_end);
	}
	emit_tok(end); // emit ] or )
	return pparse_next(_pc, end);
}

static void emit_declarator(PParseToken *tok, PParseToken *end) {
	PPARSE_CTX();
	while (tok && tok != end && tok->kind != PPARSE_TK_EOF) {
		/* Attribute interiors are not declarator dimensions: keep them opaque
		 * so brackets in aligned(sizeof(int[8])) cannot consume dim IDs. */
		if (tok->tag & PPARSE_TT_ATTR) {
			emit_tok(tok);
			tok = pparse_next(_pc, tok);
			if (tok && pparse_match_ch(tok, '(')) tok = walk_balanced(tok);
			continue;
		}
		if (pparse_is_c23_attr(tok)) {
			tok = emit_c23_attr(tok);
			continue;
		}
		if (pparse_match_ch(tok, '[') && (pparse_ann(tok) & P1_DECL_BRACKET)) {
			tok = walk_balanced_orelse(tok);
			continue;
		}
		if ((tok->flags & PPARSE_TF_OPEN) && pparse_pair(_pc, tok)) {
			PParseToken *close = pparse_pair(_pc, tok);
			if (pparse_match_ch(tok, '(')) {
				emit_tok(tok);
				emit_declarator(pparse_next(_pc, tok), close);
				emit_tok(close);
				tok = pparse_next(_pc, close);
			} else
				tok = walk_balanced(tok);
			continue;
		}
		tok = emit_advance(tok);
	}
}

static void emit_noise_between_raws(PParseToken *first_raw, PParseToken *last_raw) {
	PPARSE_CTX();
	if (first_raw == last_raw) return;
	PPARSE_FOR_RANGE(t, pparse_next(_pc, first_raw), last_raw) {
		if (t->flags & PPARSE_TF_RAW) continue;
		if ((t->flags & PPARSE_TF_OPEN) && pparse_pair(_pc, t)) {
			// Balanced group (C23 attr [[...]], GNU attr((...))): walk & emit
			PParseToken *m = pparse_pair(_pc, t);
			for (PParseToken *u = t; u && u != pparse_next(_pc, m) && u->kind != PPARSE_TK_EOF;) {
				if ((u->flags & PPARSE_TF_OPEN) && pparse_is_stmt_expr_open(u)) {
					u = emit_stmt_expr(u);
					continue;
				}
				emit_tok(u);
				u = pparse_next(_pc, u);
			}
			t = m;
		} else
			emit_tok(t);
	}
}

static PParseToken *emit_raw_verbatim_to_semicolon(PParseToken *tok) {
	PPARSE_CTX();
	while (tok && tok->kind != PPARSE_TK_EOF && !pparse_match_ch(tok, ';')) {
		if (tok->flags & PPARSE_TF_OPEN) tok = walk_balanced(tok);
		else if (pparse_feat(PPARSE_F_ORELSE) && (tok->tag & PPARSE_TT_TYPEOF)) {
			PParseToken *next = try_typeof_orelse(tok);
			if (next) {
				tok = next;
				continue;
			}
			tok = emit_advance(tok);
		} else {
			tok = emit_advance(tok);
		}
	}
	if (tok && pparse_match_ch(tok, ';')) {
		tok = emit_advance(tok);
	}
	return tok;
}

/* Idempotence: when the statements immediately following this declaration
 * already contain Prism's own canonical zeroing for `var` (a prior pass's
 * output re-entering the pipeline — -save-temps .i recompiles, library-mode
 * round trips, --prism-verify's second pass), do not zero again.  Only the
 * exact canonical shapes are recognized:
 *   __builtin_memset([(void *)]&var, 0, sizeof(var));
 *   { [volatile] char *__prism_p_N = ...; for (...) __prism_p_N[...] = 0; }
 * A user's hand-written identical memset is also skipped — semantically
 * equivalent (the object is fully zeroed either way).  Partial or nonzero
 * user memsets do not match and zero-init still applies.                   */
static bool prism_memset_follows(PParseToken *var) {
	PPARSE_CTX();
	if (!var) return false;
	/* Find the terminating ';' of the declaration statement. */
	PParseToken *t = var;
	for (int guard = 0; t && t->kind != PPARSE_TK_EOF && guard < 4096; guard++) {
		if ((t->flags & PPARSE_TF_OPEN) && pparse_pair(_pc, t)) {
			t = pparse_next(_pc, pparse_pair(_pc, t));
			continue;
		}
		if (pparse_match_ch(t, ';')) break;
		if (pparse_match_ch(t, '}')) return false;
		t = pparse_next(_pc, t);
	}
	if (!t || !pparse_match_ch(t, ';')) return false;
	t = pparse_next(_pc, t);
	/* Scan across a small run of canonical zeroing statements (earlier
	 * declarators' memsets precede this var's in multi-decl splits). */
	for (int stmts = 0; t && t->kind != PPARSE_TK_EOF && stmts < 8; stmts++) {
		if (t->kind == PPARSE_TK_IDENT && t->len == 16 && prism_memeq_static(pparse_loc(_pc, t), "__builtin_memset", 16)) {
			PParseToken *p = pparse_next(_pc, t);
			if (!p || !pparse_match_ch(p, '(') || !pparse_pair(_pc, p)) return false;
			PParseToken *close = pparse_pair(_pc, p);
			PParseToken *a = pparse_next(_pc, p);
			/* optional (void *) cast */
			if (a && pparse_match_ch(a, '(') && pparse_pair(_pc, a)) a = pparse_next(_pc, pparse_pair(_pc, a));
			if (a && pparse_match_ch(a, '&')) a = pparse_next(_pc, a);
			if (a && a->kind == PPARSE_TK_IDENT && a->len == var->len &&
			    prism_memeq_runtime_sized(pparse_loc(_pc, a), pparse_loc(_pc, var), var->len))
				return true;
			t = pparse_next(_pc, close);
			if (t && pparse_match_ch(t, ';')) t = pparse_next(_pc, t);
			continue;
		}
		if (pparse_match_ch(t, '{') && pparse_pair(_pc, t)) {
			/* candidate byte-loop block: require a __prism_p_ ident and
			 * `& var` in its opening tokens */
			PParseToken *close = pparse_pair(_pc, t);
			bool has_p = false, has_var = false;
			int k = 0;
			for (PParseToken *s = pparse_next(_pc, t); s && s != close && k < 16; s = pparse_next(_pc, s), k++) {
				if (s->kind == PPARSE_TK_IDENT && s->len >= 10 &&
				    prism_memeq_static(pparse_loc(_pc, s), "__prism_p_", 10))
					has_p = true;
				if (pparse_match_ch(s, '&') && pparse_next(_pc, s) &&
				    pparse_next(_pc, s)->kind == PPARSE_TK_IDENT &&
				    pparse_next(_pc, s)->len == var->len &&
				    prism_memeq_runtime_sized(pparse_loc(_pc, pparse_next(_pc, s)), pparse_loc(_pc, var), var->len))
					has_var = true;
			}
			if (!has_p) return false;
			if (has_var) return true;
			t = pparse_next(_pc, close);
			continue;
		}
		return false;
	}
	return false;
}

static void emit_typeof_memsets(PParseToken **vars, int count, bool has_volatile, bool has_const) {
	PRISM_STATE();
	PPARSE_CTX();
	const char *vol = has_volatile ? "volatile " : "";
	int vol_len = has_volatile ? 9 : 0;
	bool use_loop = has_volatile || target_is_msvc();
	for (int i = 0; i < count; i++) {
		if (prism_memset_follows(vars[i])) continue;
		// Byte loop for volatile (memset drops volatile) and MSVC (no
		// __builtin_memset).
		if (use_loop) {
			OUT_LIT(" { ");
			out_str(vol, vol_len);
			OUT_LIT("char *__prism_p_");
			out_uint(_ps->ret_counter);
			OUT_LIT(" = (");
			out_str(vol, vol_len);
			if (has_const) OUT_LIT("char *)(void *)&");
			else
				OUT_LIT("char *)&");
			OUT_TOK(vars[i]);
			OUT_LIT("; for (unsigned long long __prism_i_");
			out_uint(_ps->ret_counter);
			OUT_LIT(" = 0; __prism_i_");
			out_uint(_ps->ret_counter);
			OUT_LIT(" < sizeof(");
			OUT_TOK(vars[i]);
			OUT_LIT("); __prism_i_");
			out_uint(_ps->ret_counter);
			OUT_LIT("++) __prism_p_");
			out_uint(_ps->ret_counter);
			OUT_LIT("[__prism_i_");
			out_uint(_ps->ret_counter);
			OUT_LIT("] = 0; }");
			_ps->ret_counter++;
		} else {
			if (has_const) OUT_LIT(" __builtin_memset((void *)&");
			else
				OUT_LIT(" __builtin_memset(&");
			OUT_TOK(vars[i]);
			OUT_LIT(", 0, sizeof(");
			OUT_TOK(vars[i]);
			OUT_LIT("));");
		}
	}
}

static PParseToken *emit_break_continue_defer(PParseToken *tok, bool active_known) {
	PPARSE_CTX();
	bool is_break = tok->tag & PPARSE_TT_BREAK;
	PParseToken *kw = tok;
	PParseToken *after = pparse_next(_pc, tok);
	/* C23/GNU labeled break/continue: `break outer;` / `continue outer;`.
	 * Must preserve the label and unwind with DEFER_TO_DEPTH (like goto),
	 * not DEFER_BREAK/CONTINUE — those only reach the innermost loop. */
	PParseToken *label = NULL;
	if (after && pparse_is_identifier_like(after)) label = after;

	if (label && pparse_feat(PPARSE_F_DEFER)) {
		P1LabelResult info = p1_label_find(label, current_func_idx);
		int td = info.tok ? info.scope_depth : 0;
		if (active_known || goto_has_defers(td)) emit_goto_defers(td);
	} else if (pparse_feat(PPARSE_F_DEFER) && (active_known || control_flow_has_defers(is_break))) {
		emit_defers(is_break ? DEFER_BREAK : DEFER_CONTINUE);
	}
	out_char(' ');
	OUT_TOK(kw);
	if (label) {
		out_char(' ');
		OUT_TOK(label);
		tok = pparse_next(_pc, label);
	} else {
		tok = after;
	}
	out_char(';');
	if (pparse_match_ch(tok, ';')) tok = pparse_next(_pc, tok);
	return tok;
}

static PParseToken *emit_goto_defer(PParseToken *tok) {
	PPARSE_CTX();
	PParseToken *goto_tok = tok;
	tok = pparse_next(_pc, tok);
	if (pparse_feat(PPARSE_F_DEFER) && pparse_is_identifier_like(tok)) {
		int td = emit_block_depth - (int)goto_tok->parse_data;
		if (td < 0) td = 0;
		if (goto_has_defers(td)) emit_goto_defers(td);
	}
	OUT_LIT(" goto ");
	if (pparse_is_identifier_like(tok)) {
		OUT_TOK(tok);
		tok = pparse_next(_pc, tok);
	}
	out_char(';');
	if (pparse_match_ch(tok, ';')) tok = pparse_next(_pc, tok);
	return tok;
}

static PParseToken *emit_orelse_fallback_value(PParseToken *tok, PParseToken *stop_comma, PParseToken **chain_next) {
	PPARSE_CTX();
	*chain_next = NULL;
	while (tok->kind != PPARSE_TK_EOF) {
		EMIT_TRY_TYPEOF_ORELSE(tok)
		if (tok->flags & PPARSE_TF_OPEN) {
			tok = walk_balanced(tok);
			continue;
		}
		if (pparse_match_ch(tok, ';') || (stop_comma && tok == stop_comma)) break;
		if (is_orelse_keyword(tok)) {
			*chain_next = pparse_next(_pc, tok);
			return tok;
		}
		tok = emit_advance(tok);
	}
	return tok;
}

static PParseToken *emit_orelse_block_body(PParseToken *tok) {
	PPARSE_CTX();
	PParseToken *blk_close = pparse_pair(_pc, tok);
	/* Tokenizer delimiter-matching completeness (§14.4) guarantees a match. */
#ifdef PRISM_DEBUG
	if (!blk_close) pparse_error_tok(tok, "unterminated orelse block");
#endif
	CtrlState saved_ctrl = ctrl_state;
	ctrl_reset();
	tok = handle_open_brace(tok);
	tok = emit_statements(tok, blk_close, EMIT_NORMAL);
	tok = handle_close_brace(tok);
	ctrl_state = saved_ctrl;
	ctrl_state.pending = false;
	ctrl_state.parens_just_closed = false;
	end_statement_after_semicolon();
	return tok;
}

// const + fallback orelse: roll back speculative output, re-emit with temp
// variable. MSVC-compatible: instead of "const T x = val ?: fallback;",
static PParseToken *handle_const_orelse_fallback(PParseToken *tok,
					   PParseToken *orelse_tok,
					   PParseToken *val_start,
					   PParseToken *decl_start,
					   PParseDecl *decl,
					   PParseToken *type_start,
					   PParseTypeSpec *type,
					   PParseToken *pragma_start,
					   PParseToken *stop_comma) {
	PRISM_STATE();
	PPARSE_CTX();
	unsigned oe_id = _ps->ret_counter++;
	// Function pointers: return-type const lives in the type specifier and must
	// be preserved;
	bool strip_type_const = !decl->is_pointer && !decl->is_func_ptr;
	bool has_const_typedef = false;
	if (strip_type_const) {
		for (PParseToken *t = type_start; t != type->end; t = pparse_next(_pc, t)) {
			if (pparse_is_const_typedef(t)) {
				has_const_typedef = true;
				break;
			}
			if ((t->tag & PPARSE_TT_TYPEOF) && t->len != 11) {
				has_const_typedef = true;
				break;
			}
		}
	}

#define EMIT_PRAGMA_PRELUDE()                                                                                \
	do {                                                                                                 \
		if (pragma_start != type_start) emit_range(pragma_start, type_start);                        \
	} while (0)
	if (has_const_typedef) {
		if (target_is_msvc()) {
			// MSVC's typeof does not strip const from cast-to-rvalue,
			EMIT_PRAGMA_PRELUDE();
			OUT_LIT(" typeof_unqual(");
			emit_type_range(type_start, type->end, strip_type_const, true);
			out_char(')');
		} else {
			EMIT_PRAGMA_PRELUDE();
			out_char(' ');
			emit_typeof_keyword();
			OUT_LIT("((");
			emit_type_range(type_start, type->end, strip_type_const, true);
			OUT_LIT(")0)");
		}
	} else {
		EMIT_PRAGMA_PRELUDE();
		emit_type_range(type_start, type->end, strip_type_const, true);
	}
	for (PParseToken *t = decl_start; t != decl->var_name; t = pparse_next(_pc, t)) {
		if (t->tag & PPARSE_TT_CONST) continue;
		emit_tok(t);
	}

	OUT_LIT(" __prism_oe_");
	out_uint(oe_id);
	{
		PParseToken *t = pparse_next(_pc, decl->var_name);
		while (t && t != decl->end && t->kind != PPARSE_TK_EOF) {
			if (pparse_match_ch(t, '[') && !(t->flags & PPARSE_TF_C23_ATTR)) {
				t = walk_balanced_orelse(t);
			} else {
				emit_tok(t);
				t = pparse_next(_pc, t);
			}
		}
	}
	OUT_LIT(" = (");
	emit_range(val_start, orelse_tok);
	OUT_LIT(");");
	for (;;) {
		if ((tok->tag & (PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE | PPARSE_TT_GOTO)) || pparse_match_ch(tok, '{')) {
			OUT_LIT(" if (!__prism_oe_");
			out_uint(oe_id);
			OUT_LIT(")");
			tok = emit_orelse_action(tok, NULL, false, stop_comma);
			break;
		}

		/* volatile/_Atomic temps must not use `t = t ? t : fb` (double load). */
		if (type->has_volatile || type->has_atomic) {
			OUT_LIT(" if (!__prism_oe_");
			out_uint(oe_id);
			OUT_LIT(") __prism_oe_");
			out_uint(oe_id);
			OUT_LIT(" = (");
			PParseToken *chain_next;
			tok = emit_orelse_fallback_value(tok, stop_comma, &chain_next);
			OUT_LIT(");");
			if (!chain_next) break;
			tok = chain_next;
			continue;
		}

		emit_prism_oe(oe_id);
		OUT_LIT(" =");
		emit_prism_oe(oe_id);
		OUT_LIT(" ?");
		emit_prism_oe(oe_id);
		OUT_LIT(" : (");
		PParseToken *chain_next;
		tok = emit_orelse_fallback_value(tok, stop_comma, &chain_next);
		OUT_LIT(");");
		if (!chain_next) break;
		tok = chain_next;
	}

	EMIT_PRAGMA_PRELUDE();
#undef EMIT_PRAGMA_PRELUDE
	emit_type_range(type_start, type->end, false, false);
	emit_declarator(decl_start, decl->end);
	OUT_LIT(" = __prism_oe_");
	out_uint(oe_id);
	out_char(';');
	return tok;
}

typedef struct {
	PParseToken *orelse_tok;
	bool is_const_fallback;
} OrelseInitInfo;

static inline void flush_typeof_memsets(PParseToken **vars, int *count, PParseTypeSpec *type, int base) {
	if (*count > base) {
		emit_typeof_memsets(&vars[base], *count - base, type->has_volatile, type->has_const);
		*count = base;
	}
}

static OrelseInitInfo
scan_decl_orelse(PParseToken *decl_end, PParseToken *type_start, PParseTypeSpec *type, PParseDecl *decl) {
	PPARSE_CTX();
	OrelseInitInfo info = {0};
	if (!decl->has_init || !pparse_feat(PPARSE_F_ORELSE)) return info;
	/* Macro-hygiene parens wrapping the whole initializer were unlinked by
	 * Phase 1 (p1d_scan_init_orelse), so a decl-init orelse keyword always
	 * sits at group depth 0 here — this walk only locates the annotation. */
	int pd = 0;
	PPARSE_FOR_TAIL(scan, pparse_next(_pc, decl_end)) {
		if (scan->flags & PPARSE_TF_OPEN) {
			pd++;
			continue;
		}
		if (scan->flags & PPARSE_TF_CLOSE) {
			pd--;
			continue;
		}
		if (pd) continue;
		if (pparse_match_ch(scan, ',') || pparse_match_ch(scan, ';')) break;
		if (pparse_ann(scan) & P1_IS_ORELSE_KW) {
			info.orelse_tok = scan;
			break;
		}
	}

	if (info.orelse_tok) {
		bool is_fallback = is_orelse_value_fallback(pparse_next(_pc, info.orelse_tok));
		info.is_const_fallback = pparse_has_effective_const_qual(type_start, type, decl) && is_fallback;
	}

	return info;
}

typedef struct {
	PParseToken *tok;
	PParseToken *unreachable_tok;
	bool hit_orelse;
} InitWalkResult;

static InitWalkResult emit_decl_init_walk(PParseToken *tok) {
	PRISM_STATE();
	PPARSE_CTX();
	InitWalkResult r = {tok, NULL, false};
	int init_ternary = 0;
	while (r.tok->kind != PPARSE_TK_EOF) {
		if (r.tok->flags & PPARSE_TF_OPEN) {
			r.tok = walk_balanced(r.tok);
			continue;
		}
		if (pparse_match_ch(r.tok, ',') || pparse_match_ch(r.tok, ';')) break;
		if (pparse_match_ch(r.tok, '?')) {
			init_ternary++;
			emit_tok(r.tok);
			r.tok = pparse_next(_pc, r.tok);
			continue;
		}
		if (pparse_match_ch(r.tok, ':') && init_ternary > 0) {
			init_ternary--;
			emit_tok(r.tok);
			r.tok = pparse_next(_pc, r.tok);
			continue;
		}
		if (pparse_feat(PPARSE_F_ORELSE) && is_orelse_keyword(r.tok)) {
			/* raw { … } suppress region — keep orelse as a soft keyword. */
			if (_ps->raw_block_depth > 0) {
				r.tok = emit_advance(r.tok);
				continue;
			}
			r.hit_orelse = true;
			break;
		}
		if (pparse_feat(PPARSE_F_AUTO_UNREACHABLE) && !in_ctrl_paren()) {
			PParseToken *nr = pparse_try_detect_noreturn_call(r.tok);
			if (nr) r.unreachable_tok = nr;
		}
		EMIT_TRY_BOUNDS(r.tok)
		r.tok = emit_advance(r.tok);
	}
	return r;
}

static bool finish_decl_orelse_hit(PParseToken **tok_p, PParseToken *tok, PParseToken *stop_comma, bool brace_wrap) {
	PPARSE_CTX();
	if (stop_comma && pparse_match_ch(tok, ',')) {
		*tok_p = pparse_next(_pc, tok);
		return true;
	}
	if (brace_wrap) OUT_LIT(" }");
	*tok_p = tok;
	return false;
}

static bool process_init_orelse_hit(PParseToken **tok_p,
				    PParseDecl *decl,
				    PParseTypeSpec *type,
				    bool brace_wrap,
				    int typeof_var_base) {
	PRISM_STATE();
	PPARSE_CTX();
	PParseToken *tok = *tok_p;
	out_char(';');
	flush_typeof_memsets(_ps->typeof_vars, &_ps->typeof_var_count, type, typeof_var_base);
	tok = pparse_next(_pc, tok); // skip 'orelse'
	PParseToken *stop_comma = pparse_find_boundary_comma(tok);
	tok = emit_orelse_action(tok,
				 decl->var_name,
				 type->has_volatile || type->has_atomic,
				 stop_comma);
	return finish_decl_orelse_hit(tok_p, tok, stop_comma, brace_wrap);
}

static bool process_const_orelse_decl(PParseToken **tok_p,
				      PParseToken *orelse_tok,
				      PParseToken *decl_start,
				      PParseDecl *decl,
				      PParseToken *type_start,
				      PParseTypeSpec *type,
				      PParseToken *pragma_start,
				      bool brace_wrap,
				      int typeof_var_base) {
	PRISM_STATE();
	PPARSE_CTX();
	PParseToken *val_start = pparse_next(_pc, decl->end); // First value token after '='
	PParseToken *tok = pparse_next(_pc, orelse_tok);	// skip 'orelse'
	PParseToken *stop_comma = pparse_find_boundary_comma(tok);
	tok = handle_const_orelse_fallback(
	    tok, orelse_tok, val_start, decl_start, decl, type_start, type, pragma_start, stop_comma);
	flush_typeof_memsets(_ps->typeof_vars, &_ps->typeof_var_count, type, typeof_var_base);
	if (pparse_match_ch(tok, ';')) tok = pparse_next(_pc, tok);
	end_statement_after_semicolon();
	return finish_decl_orelse_hit(tok_p, tok, stop_comma, brace_wrap);
}

static PParseToken *process_declarators(PParseToken *tok,
				  PParseTypeSpec *type,
				  bool is_raw,
				  PParseToken *type_start,
				  PParseToken *pragma_start,
				  PParseToken *raw_tok,
				  bool brace_wrap) {
	PRISM_STATE();
	PPARSE_CTX();
	int typeof_var_base = _ps->typeof_var_count; // Save for reentrancy (stmt-expr in array dims)
	bool first_decl = true;
	bool need_type_emit = false; // Set after orelse comma — deferred to after next lookahead
	while (tok && tok->kind != PPARSE_TK_EOF) {
		PParseToken *decl_start = tok;
		bool decl_is_raw = is_raw;
		PParseToken *raw_probe = pparse_skip_noise(_pc, tok);
		PParseToken *after = pparse_raw_decl_strip_after(raw_probe);
		if (after) {
			emit_declarator(tok, raw_probe);
			PParseToken *last_raw = raw_probe;
			pparse_SKIP_RAW(after, last_raw);
			emit_noise_between_raws(raw_probe, last_raw);
			decl_start = pparse_next(_pc, last_raw);
			tok = after;
			decl_is_raw = true;
		}

		PParseDecl decl = pparse_declarator(tok);
		if (!decl.end || !decl.var_name) {
			if (!first_decl) {
				if (need_type_emit) {
					emit_type_range(type_start, type->end, false, true);
				}
				goto emit_raw_bail;
			}
			return NULL;
		}

		OrelseInitInfo orelse_info = scan_decl_orelse(decl.end, type_start, type, &decl);
		bool is_const_orelse_fallback = orelse_info.is_const_fallback;
		// Step 2b: Pre-hoist bracket orelse temps (before type emission)
		bool has_bo = pparse_feat(PPARSE_F_ORELSE) && pparse_declarator_has_bracket_orelse(decl_start, decl.end);
		bool brace_opened = false;
		BOFrame bo_frame;
		if (has_bo) {
			if (first_decl && brace_wrap) {
				OUT_LIT(" {");
				brace_opened = true;
			}
			bo_frame.oe_count = _ps->bracket_oe_count;
			bo_frame.oe_next = _ps->bracket_oe_next;
			bo_frame.dim_count = _ps->bracket_dim_count;
			bo_frame.dim_next = _ps->bracket_dim_next;
			bo_snapshot_ids(&bo_frame.oe_ids, _ps->bracket_oe_ids, bo_frame.oe_count);
			bo_snapshot_ids(&bo_frame.dim_ids, _ps->bracket_dim_ids, bo_frame.dim_count);
			emit_bracket_orelse_temps(decl_start, decl.end);
		}

		if (need_type_emit) {
			if (!is_const_orelse_fallback)
				emit_type_with_pragma_prelude(
				    pragma_start, type_start, type->end, raw_tok, true);
			need_type_emit = false;
		}

		if (first_decl) {
			if (brace_wrap && !brace_opened) OUT_LIT(" {");
			if (!is_const_orelse_fallback) {
				if (pparse_feat(PPARSE_F_AUTO_STATIC) && _ps->raw_block_depth == 0 && emit_block_depth > 0 &&
				    !in_ctrl_paren() &&
				    /* SPEC §6.9 criterion 3: explicit const — do not use
				     * pparse_has_effective_const_qual (typeof is treated as const for
				     * orelse temps, which wrongly auto-static'd mutable
				     * typeof(int[N]) arrays). */
				    pparse_decl_has_explicit_const(type_start, type, &decl) &&
				    !type->has_volatile && !type->has_volatile_member &&
				    !type->has_static && !type->has_extern && !type->has_register &&
				    !type->has_auto && !type->has_constexpr && !type->has_thread_local &&
				    (decl.is_array || type->is_array) &&
				    (!decl.is_pointer || decl.is_const) && decl.has_init &&
				    !decl.is_vla && !type->is_vla && !decl_is_raw &&
				    !orelse_info.orelse_tok) {
					bool has_volatile_td = false;
					PPARSE_FOR_RANGE(tv, type_start, type->end)
						if (pparse_is_volatile_typedef(tv) || pparse_has_volatile_member_typedef(tv)) {
							has_volatile_td = true;
							break;
						}
					if (!has_volatile_td &&
					    !pparse_range_has_attribute(
						pragma_start ? pragma_start : type_start, type->end, PPARSE_TT_ASM) &&
					    !pparse_range_has_attribute(pparse_next(_pc, decl.var_name), decl.end, 0)) {
						PParseToken *eq = decl.end; // '=' token
						PParseToken *init = pparse_next(_pc, eq);
						bool lit_ok = false;
						if (init && pparse_match_ch(init, '{') && pparse_pair(_pc, init) &&
						    pparse_match_ch(pparse_next(_pc, pparse_pair(_pc, init)), ';') &&
						    pparse_is_const_literal_initializer(eq))
							lit_ok = true;
						else if (init && init->kind == PPARSE_TK_STR &&
							 pparse_is_const_literal_initializer(eq))
							lit_ok = true;
						if (lit_ok) OUT_LIT("static ");
					}
				}
				emit_type_with_pragma_prelude(
				    pragma_start, type_start, type->end, raw_tok, false);
			}
			first_decl = false;
		}

		uint32_t recipe = pparse_ann(decl.var_name);
		PParseDeclShape shape;
		uint8_t zero_kind;
		if (recipe & P1_DECL_RECIPE) {
			uint8_t bits = (uint8_t)(recipe >> P1_DECL_SHAPE_SHIFT);
			shape = (PParseDeclShape){.effective_vla = (bits & P1DS_EFF_VLA) != 0,
						 .is_aggregate = (bits & P1DS_AGG) != 0,
						 .is_union_type = (bits & P1DS_UNION) != 0};
			zero_kind = (uint8_t)((recipe >> P1_DECL_ZERO_SHIFT) & 3);
		} else {
			/* No P1K_DECL: file scope, func type, or aggregate member. */
			shape = pparse_classify_decl_shape(type_start, type, &decl);
			/* No P1K_DECL recipe means no recorded scope; a stmt-expr
			 * ICE context cannot arise on this path. */
			zero_kind = compute_decl_zero_kind(&shape, type_start, type, &decl,
							   decl.has_init, decl_is_raw,
							   type->has_static, PPARSE_SID_UNKNOWN);
#ifdef PRISM_DEBUG
			if (current_func_idx >= 0 && decl.var_name && !shape.is_func_type &&
			    !p1_token_in_nested_function(decl.var_name))
				pparse_error_tok(decl.var_name, "internal: missing P1K_DECL recipe");
#endif
		}
		bool effective_vla = shape.effective_vla;
		bool needs_memset = (zero_kind == P1Z_MEMSET);
		if (needs_memset && !type->has_volatile && !decl.is_func_ptr && !decl.is_pointer) {
			PPARSE_FOR_RANGE(tv, type_start, type->end)
				if (pparse_is_volatile_typedef(tv) || pparse_has_volatile_member_typedef(tv)) {
					type->has_volatile = true;
					break;
				}
		}
		if (needs_memset && !type->has_volatile && type->has_volatile_member)
			type->has_volatile = true;
		if (is_const_orelse_fallback && _ps->raw_block_depth == 0) {
			if (process_const_orelse_decl(&tok,
						      orelse_info.orelse_tok,
						      decl_start,
						      &decl,
						      type_start,
						      type,
						      pragma_start,
						      brace_wrap,
						      typeof_var_base)) {
				if (has_bo) bo_restore(&bo_frame);
				need_type_emit = true;
				continue;
			}
			if (has_bo) bo_restore(&bo_frame);
			return tok;
		}

		emit_declarator(decl_start, decl.end);
		if (has_bo) bo_restore(&bo_frame);
		tok = decl.end;
		bool init_stmt_ctx = in_for_init() || in_ctrl_paren();
		uint8_t emit_zk = zero_kind;
		if (emit_zk == P1Z_MEMSET && init_stmt_ctx && !effective_vla) {
			/* for/if init cannot emit memset. Demoting to ={0} is illegal for
			 * _Atomic aggregates (and some empty/FAM shapes on strict compilers). */
			if ((type->has_atomic && shape.is_aggregate) ||
			    (type_start && pparse_type_brace_zero_unsafe(type_start, type->end, 0)))
				pparse_error_tok(decl.var_name,
					  "aggregate requiring memset cannot be zero-initialized in a "
					  "for/if/switch init-statement; move the declaration before the "
					  "statement");
			emit_zk = P1Z_AGG;
		}
		if (emit_zk == P1Z_SCALAR || emit_zk == P1Z_AGG) {
			if (emit_zk == P1Z_AGG) OUT_LIT(" = {0}");
			else
				OUT_LIT(" = 0");
		}

		if (emit_zk == P1Z_MEMSET) {
			bool real_for_init = in_for_init() && emit_scope_depth > 0 &&
					     scope_stack[emit_scope_depth - 1].is_loop;
			if (!(init_stmt_ctx && effective_vla && !real_for_init)) {
				PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena,
						 _ps->typeof_vars,
						 _ps->typeof_var_count + 1,
						 _ps->typeof_var_cap,
						 16,
						 PParseToken *);
				_ps->typeof_vars[_ps->typeof_var_count++] = decl.var_name;
			}
		}

		PParseToken *pd_unreachable_tok = NULL;
		if (decl.has_init) {
			InitWalkResult iw = emit_decl_init_walk(tok);
			tok = iw.tok;
			pd_unreachable_tok = iw.unreachable_tok;
			if (iw.hit_orelse) {
				if (process_init_orelse_hit(
					&tok, &decl, type, brace_wrap, typeof_var_base)) {
					need_type_emit = true;
					continue;
				}
				return tok;
			}
		}

		if (!brace_wrap) check_defer_var_shadow(decl.var_name);
		if (pparse_match_ch(tok, ';')) {
			bool is_ur = (tok == pd_unreachable_tok);
			emit_tok(tok);
			flush_typeof_memsets(_ps->typeof_vars, &_ps->typeof_var_count, type, typeof_var_base);
			if (is_ur) EMIT_UNREACHABLE();
			if (brace_wrap) OUT_LIT(" }");
			return pparse_next(_pc, tok);
		} else if (pparse_match_ch(tok, ',')) {
			PParseToken *next_decl_tok = pparse_next(_pc, tok);
			bool split_decl = false;
			if (!in_for_init()) {
				PParseDecl next_decl = pparse_declarator(next_decl_tok);
				split_decl = next_decl.end &&
					     ((_ps->typeof_var_count > 0 && next_decl.var_name &&
					       (next_decl.has_init || next_decl.is_vla)) ||
					      (pparse_feat(PPARSE_F_ORELSE) &&
					       pparse_declarator_has_bracket_orelse(next_decl_tok, next_decl.end)));
			}
			if (split_decl) {
				out_char(';');
				flush_typeof_memsets(
				    _ps->typeof_vars, &_ps->typeof_var_count, type, typeof_var_base);
				tok = next_decl_tok;
				need_type_emit = true;
				continue;
			}

			emit_tok(tok);
			tok = next_decl_tok;
		} else {
			if (!first_decl) goto emit_raw_bail;
			return NULL;
		}
	}
	return NULL;

emit_raw_bail:
	while (tok && tok->kind != PPARSE_TK_EOF && !pparse_match_ch(tok, ';')) {
		if ((tok->flags & PPARSE_TF_OPEN) && pparse_is_stmt_expr_open(tok)) {
			tok = emit_stmt_expr(tok);
			continue;
		}
		tok = emit_advance(tok);
	}
	if (tok && pparse_match_ch(tok, ';')) {
		tok = emit_advance(tok);
	}
	flush_typeof_memsets(_ps->typeof_vars, &_ps->typeof_var_count, type, typeof_var_base);
	if (brace_wrap) OUT_LIT(" }");
	return tok;
}

static PParseToken *try_zero_init_decl(PParseToken *tok) {
	PPARSE_CTX();
	if (emit_block_depth <= 0 || in_struct_body()) return NULL;
	if (!pparse_feat(PPARSE_F_ZEROINIT) && !pparse_feat(PPARSE_F_ORELSE) && !pparse_feat(PPARSE_F_AUTO_STATIC)) return NULL;
	if (!pparse_feat(PPARSE_F_ZEROINIT) && !pparse_feat(PPARSE_F_AUTO_STATIC)) {
		bool has_bo = false;
		PPARSE_FOR_TAIL(s, tok) {
			if (pparse_match_ch(s, ';') || pparse_match_ch(s, '{')) break;
			if (pparse_ann(s) & (P1_OE_BRACKET | P1_OE_DECL_INIT)) {
				has_bo = true;
				break;
			}
		}
		if (!has_bo) return NULL;
	}

	if (tok->kind >= PPARSE_TK_STR) // Fast reject: strings, numbers, prep directives,
				 // EOF can't start a declaration
		return NULL;
	ScopeNode *_bt = scope_block_top();
	bool in_switch_scope_unbraced = _bt && _bt->is_switch;
	PParseToken *warn_loc = tok;
	PParseToken *pragma_start = tok;
	tok = pparse_skip_noise(_pc, tok);
	PParseToken *start = tok;
	bool is_raw = false;
	PParseToken *raw_tok = NULL;
	PParseToken *raw_last = NULL; // last raw in chain (for deferred emit_noise_between_raws)
	if ((tok->flags & PPARSE_TF_RAW) && !pparse_is_known_typedef(tok)) {
		PParseToken *after_raw = pparse_skip_noise(_pc, pparse_next(_pc, tok));
		if (pparse_is_raw_declaration_context(tok, after_raw)) {
			is_raw = true;
			raw_tok = tok;
			PParseToken *last_raw = tok;
			pparse_SKIP_RAW(after_raw, last_raw);
			raw_last = last_raw;
			start = pparse_next(_pc, last_raw);
			tok = after_raw;
			if (pragma_start == raw_tok) pragma_start = start;
			warn_loc = after_raw;
		}
	}

	if (!is_raw) {
		PParseToken *probe = start;
		while (probe && probe->kind != PPARSE_TK_EOF) {
			pparse_SKIP_NOISE_CONTINUE(probe);
			if (probe->tag & PPARSE_TT_QUALIFIER) {
				probe = pparse_next(_pc, probe);
				continue;
			}
			if ((probe->tag & (PPARSE_TT_STORAGE | PPARSE_TT_TYPEDEF))) {
				probe = pparse_next(_pc, probe);
				continue;
			}
			break;
		}
		if (probe && (probe->flags & PPARSE_TF_RAW) && !pparse_is_known_typedef(probe)) {
			PParseToken *after_raw = pparse_skip_noise(_pc, pparse_next(_pc, probe));
			if (pparse_is_raw_declaration_context(probe, after_raw)) {
				PParseToken *last_raw = probe;
				pparse_SKIP_RAW(after_raw, last_raw);
				if (pparse_has_storage_in(pragma_start, last_raw)) {
					emit_range(pragma_start, probe);
					emit_noise_between_raws(probe, last_raw);
					return emit_raw_verbatim_to_semicolon(pparse_next(_pc, last_raw));
				}
				is_raw = true;
				raw_tok = probe;
			}
		}
	}

	pparse_ASSERT_NOT_NOISE(tok);
	if ((tok->tag & PPARSE_TT_SKIP_DECL) && !(tok->tag & PPARSE_TT_STORAGE)) // Control flow, etc. (not storage class)
	{
		if (is_raw) {
			return emit_raw_verbatim_to_semicolon(start);
		}
		return NULL;
	}

	if (!(tok->tag & PPARSE_TT_DECL_START) && !pparse_is_known_typedef(tok)) return NULL;
	// Phase 1 fast gate: skip past storage/inline prefix to reach the
	// type-start token that Phase 1D annotated with P1_IS_DECL.
	{
		PParseToken *ann = tok;
		while (ann && ann->kind != PPARSE_TK_EOF) {
			pparse_SKIP_NOISE_CONTINUE(ann);
			if (ann->tag & (PPARSE_TT_STORAGE | PPARSE_TT_INLINE | PPARSE_TT_SKIP_DECL)) {
				ann = pparse_next(_pc, ann);
				continue;
			}
			break;
		}
		if (ann && !(pparse_ann(ann) & P1_IS_DECL)) return NULL;
	}

	PParseTypeSpec type = pparse_type_specifier(tok);
	if (!type.saw_type) return NULL;
	// pparse_type_specifier now detects embedded raw (e.g. const raw int)
	if (type.has_raw && !is_raw) {
		is_raw = true;
		PPARSE_FOR_RANGE(r, start, type.end)
			if ((r->flags & PPARSE_TF_RAW) && !pparse_is_known_typedef(r)) {
				raw_tok = r;
				break;
			}
	}

	{
		PParseDecl probe = pparse_declarator(type.end);
		if (!probe.var_name || !probe.end) return NULL;
		if (pparse_match_ch(probe.end, '=')) {
			PParseToken *aeq = pparse_next(_pc, probe.end);
			if (aeq && pparse_is_stmt_expr_open(aeq) && pparse_pair(_pc, aeq)) {
				PParseToken *se_close = pparse_pair(_pc, aeq);
				PParseToken *after_se = pparse_next(_pc, se_close);
				bool is_orelse =
				    after_se && (after_se->tag & PPARSE_TT_ORELSE) && !(se_close->tag & PPARSE_TT_MEMBER);
				if (is_orelse && pparse_token_has_binding(after_se) &&
				    !pparse_orelse_shadow_is_kw(se_close))
					is_orelse = false;
				if (!after_se || (!pparse_match_ch(after_se, ',') && !is_orelse)) {
					if (!(ctrl_state.pending && ctrl_state.parens_just_closed))
						check_defer_var_shadow(probe.var_name);
					return NULL;
				}
			}
		}
	}

	if (pparse_feat(PPARSE_F_ZEROINIT) && in_switch_scope_unbraced && !is_raw && !in_for_init()) {
		// Mirror Phase 1D's narrower gate: only fire on the
		PParseDecl _peek = pparse_declarator(type.end);
		bool _has_init = _peek.var_name && _peek.end && pparse_match_ch(_peek.end, '=');
		bool _has_explicit_intent = _has_init || type.is_typedef || type.is_struct || type.is_enum ||
					    type.has_static || type.has_extern || type.has_thread_local ||
					    type.has_register || type.has_atomic || type.has_constexpr ||
					    type.has_alignas;
		if (!_has_explicit_intent)
			SAFETY_DIAG(warn_loc,
				    "variable declaration directly in switch body without braces "
				    "(zero-init may be skipped by case labels); wrap in braces "
				    "or use 'raw'");
	}

	bool brace_wrap = ctrl_state.pending && ctrl_state.parens_just_closed;
	if (brace_wrap) ctrl_reset();
	if (is_raw && raw_tok && raw_last) emit_noise_between_raws(raw_tok, raw_last);
	return process_declarators(type.end, &type, is_raw, start, pragma_start, raw_tok, brace_wrap);
}

static PParseToken *emit_expr_to_semicolon(PParseToken *tok) {
	PPARSE_CTX();
	int brace_depth = 0;
	int ternary_depth = 0;
	bool expr_at_stmt_start = false;
	while (tok->kind != PPARSE_TK_EOF) {
		if ((pparse_match_ch(tok, '(') || pparse_match_ch(tok, '[')) && pparse_pair(_pc, tok)) {
			tok = walk_balanced(tok);
			expr_at_stmt_start = false;
			continue;
		}
		if (pparse_match_ch(tok, '{')) {
			brace_depth++;
			expr_at_stmt_start = true;
		} else if (pparse_match_ch(tok, '}'))
			brace_depth--;
		else if (brace_depth == 0 && pparse_match_ch(tok, ';'))
			break;
		else if (pparse_match_ch(tok, '?'))
			ternary_depth++;
		if (expr_at_stmt_start && pparse_feat(PPARSE_F_ZEROINIT)) {
			PParseToken *next = try_zero_init_decl(tok);
			if (next) {
				tok = next;
				expr_at_stmt_start = true;
				continue;
			}
			expr_at_stmt_start = false;
		}

		EMIT_TRY_TYPEOF_ORELSE(tok)
		reject_defer_in_expr_context(tok);
		{
			PParseToken *r = emit_tok_checked(tok);
			if (r) {
				tok = r;
				continue;
			}
		}

		if (pparse_match_ch(tok, ';') || pparse_match_ch(tok, '{') || pparse_match_ch(tok, '}')) expr_at_stmt_start = true;
		else if (pparse_match_ch(tok, ':') && ternary_depth > 0) {
			ternary_depth--;
			expr_at_stmt_start = false;
		} else if (pparse_match_ch(tok, ':') && ternary_depth <= 0)
			expr_at_stmt_start = true;
		else
			expr_at_stmt_start = false;
		tok = pparse_next(_pc, tok);
	}
	return tok;
}

static PParseToken *try_handle_defer_flow_kw(PParseToken *tok) {
	PPARSE_CTX();
	uint32_t tag = tok->tag;
	if (!tag) return NULL;
	if ((tag & PPARSE_TT_DEFER) && !in_generic()) {
		PParseToken *next = handle_defer_keyword(tok);
		if (next) return next;
		/* Statement-shaped `defer` that handle_defer_keyword declined
		 * (mid-expression after comma / in a return value, etc.): reject
		 * rather than leaking the keyword. Identifier uses of a shadowed
		 * name fall through — reject_defer_in_expr_context allows them. */
		reject_defer_in_expr_context(tok);
	}
	if (pparse_feat(PPARSE_F_DEFER) && (tag & (PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE))) {
		PParseToken *next = handle_control_exit_defer(tok);
		if (next) return next;
	}
	if ((tag & PPARSE_TT_GOTO) && pparse_feat(PPARSE_F_DEFER | PPARSE_F_ZEROINIT)) {
		PParseToken *next = handle_goto_keyword(tok);
		if (next) return next;
	}
	return NULL;
}

static void arm_ctrl_pending_from_tag(PParseToken *tok, uint32_t tag) {
	PPARSE_CTX();
	if (tag & PPARSE_TT_LOOP) {
		ctrl_state.pending_paren_kw = 1;
		if (pparse_is_do_kw(tok)) {
			ctrl_state.parens_just_closed = true;
			/* Match else / emit_statements: body is a statement. */
			emit_at_stmt_start = true;
		}
		if (pparse_feat(PPARSE_F_DEFER | PPARSE_F_ZEROINIT)) {
			ctrl_state.pending = true;
			if (!pparse_is_do_kw(tok)) ctrl_state.parens_just_closed = false;
		}
		if (tok->ch0 == 'f' && pparse_feat(PPARSE_F_DEFER | PPARSE_F_ZEROINIT)) {
			ctrl_state.pending = true;
			ctrl_state.pending_for_paren = true;
		}
	}
	if (tag & PPARSE_TT_SWITCH) ctrl_state.pending_paren_kw = 2;
	if (pparse_feat(PPARSE_F_DEFER) && (tag & PPARSE_TT_SWITCH)) ctrl_state.pending = true;
	if ((tag & PPARSE_TT_SWITCH) && pparse_feat(PPARSE_F_DEFER | PPARSE_F_ZEROINIT)) {
		ctrl_state.pending = true;
		ctrl_state.pending_for_paren = true;
		ctrl_state.parens_just_closed = false;
	}
	if (tag & PPARSE_TT_IF) {
		ctrl_state.pending = true;
		if (pparse_is_else_kw(tok)) {
			ctrl_state.parens_just_closed = true;
			emit_at_stmt_start = true;
		} else {
			ctrl_state.parens_just_closed = false;
			if (pparse_feat(PPARSE_F_DEFER | PPARSE_F_ZEROINIT)) ctrl_state.pending_for_paren = true;
		}
	}
}

static PParseToken *handle_defer_keyword(PParseToken *tok) {
	PPARSE_CTX();
	if (!pparse_feat(PPARSE_F_DEFER) || !(pparse_ann(tok) & P1_IS_DEFER_KW)) return NULL;
	if (in_struct_body() || (pparse_ann(tok) & P1_IN_ATTR_ARGS)) return NULL;
	bool in_sw = false;
	ScopeNode *block = NULL;
	for (int d = emit_scope_depth - 1; d >= 0; d--) {
		if (!block && is_brace_scope(scope_stack[d].kind)) block = &scope_stack[d];
		if (scope_stack[d].kind != SCOPE_BLOCK) continue;
		in_sw = scope_stack[d].is_switch;
		break;
	}
	bool in_cp = in_ctrl_paren();
	/* Keep in release: Phase 1 covers common paths, but control-paren /
	 * braceless forms still rely on this emission-time gate. */
	reject_defer_context(tok,
			     in_cp,
			     ctrl_state.pending && !in_cp,
			     block && block->is_stmt_expr,
			     in_sw);

	PParseToken *defer_keyword = tok;
	tok = pparse_skip_noise(_pc, pparse_next(_pc, tok));
	PParseToken *stmt_start = tok;
	if (pparse_match_ch(stmt_start, '{') && pparse_pair(_pc, stmt_start)) {
		PParseToken *close = pparse_pair(_pc, stmt_start);
		PParseToken *after = pparse_next(_pc, close);
		PParseToken *stmt_end = after; // exclusive boundary — emits up to but not including
		defer_add(defer_keyword, stmt_start, stmt_end);
		tok = after;
		{
			if (tok && pparse_match_ch(tok, ';')) tok = pparse_next(_pc, tok);
			end_statement_after_semicolon();
		}
		return tok;
	}

	PParseToken *stmt_end = pparse_skip_to_semicolon(tok, NULL);
	reject_defer_unterminated(defer_keyword, stmt_start, stmt_end);
	defer_add(defer_keyword, stmt_start, stmt_end);
	tok = (stmt_end->kind != PPARSE_TK_EOF) ? pparse_next(_pc, stmt_end) : stmt_end;
	end_statement_after_semicolon();
	return tok;
}

static PParseToken *emit_return_body(PParseToken *tok, PParseToken *stop, bool active_known) {
	PRISM_STATE();
	PPARSE_CTX();
	bool active = active_known || (pparse_feat(PPARSE_F_DEFER) && has_active_defers());
	bool is_empty = pparse_match_ch(tok, ';') || (stop && tok == stop);
	if (active) {
		if (is_empty) {
			emit_all_defers();
			OUT_LIT(" return;");
		} else {
			bool is_void = (current_func_idx >= 0 && func_meta[current_func_idx].returns_void) ||
				       (pparse_match_ch(tok, '(') && pparse_next(_pc, tok) &&
					pparse_equal(pparse_next(_pc, tok), "void") &&
					pparse_next(_pc, pparse_next(_pc, tok)) &&
					pparse_match_ch(pparse_next(_pc, pparse_next(_pc, tok)), ')'));
			unsigned ret_id = _ps->ret_counter++;
			if (!is_void) {
				out_char(' ');
				FuncMeta *fm = current_func_idx >= 0 ? &func_meta[current_func_idx] : NULL;
				if (fm && fm->ret_type_start && fm->ret_type_end) {
					if (fm->ret_type_suffix_start) {
						OUT_LIT("typedef ");
						emit_ret_type_tokens(fm->ret_type_start, fm->ret_type_end);
						OUT_LIT(" __prism_ret_t_");
						out_uint(_ps->ret_counter);
						for (PParseToken *t = fm->ret_type_suffix_start;
						     t && t != fm->ret_type_suffix_end && t->kind != PPARSE_TK_EOF;
						     t = pparse_next(_pc, t)) {
							out_char(' ');
							OUT_TOK(t);
						}
						OUT_LIT("; __prism_ret_t_");
						out_uint(_ps->ret_counter);
					} else
						emit_ret_type_tokens(fm->ret_type_start, fm->ret_type_end);
				} else {
					pparse_error("defer in function with unresolvable return type; "
					      "use a named struct or typedef");
				}
				OUT_LIT(" __prism_ret_");
				out_uint(ret_id);
				OUT_LIT(" = (");
			} else
				OUT_LIT(" (");
			if (stop) tok = emit_expr_to_stop(tok, stop, false);
			else
				tok = emit_expr_to_semicolon(tok);
			OUT_LIT(");");
			emit_all_defers();
			if (!is_void) {
				OUT_LIT(" return __prism_ret_");
				out_uint(ret_id);
			} else
				OUT_LIT(" return");
			out_char(';');
		}
	} else {
		OUT_LIT(" return");
		if (!is_empty) {
			out_char(' ');
			if (stop) tok = emit_expr_to_stop(tok, stop, false);
			else
				tok = emit_expr_to_semicolon(tok);
		}
		out_char(';');
	}

	if (pparse_match_ch(tok, ';')) tok = pparse_next(_pc, tok);
	return tok;
}

static void emit_if_not_var(PParseToken *var_name, bool open_brace) {
	PPARSE_CTX();
	if (var_name) {
		OUT_LIT(" if (!");
		OUT_TOK(var_name);
		out_char(')');
		if (open_brace) OUT_LIT(" {");
	} else if (open_brace) {
		OUT_LIT(" {");
	}
}

static PParseToken *
emit_orelse_action(PParseToken *tok, PParseToken *var_name, bool single_eval_lhs, PParseToken *stop_comma) {
	PPARSE_CTX();
	if (pparse_match_ch(tok, '{')) {
		emit_if_not_var(var_name, false);
		tok = emit_orelse_block_body(tok);
		if (tok && pparse_match_ch(tok, ';')) tok = pparse_next(_pc, tok);
		return tok;
	}

	if (tok->tag & (PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE | PPARSE_TT_GOTO)) {
		uint64_t tag = tok->tag;
		if (tag & PPARSE_TT_RETURN) tok = pparse_next(_pc, tok);
		emit_if_not_var(var_name, true);
		if (tag & PPARSE_TT_RETURN) tok = emit_return_body(tok, stop_comma, false);
		else if (tag & (PPARSE_TT_BREAK | PPARSE_TT_CONTINUE))
			tok = emit_break_continue_defer(tok, false);
		else
			tok = emit_goto_defer(tok);
		OUT_LIT(" }");
		end_statement_after_semicolon();
		return tok;
	}

	if (single_eval_lhs) {
		// `&(T){init}` in the if-substatement has statement scope (C11 §6.8.4.1p2);
		PParseToken *probe = tok;
		if (pparse_match_ch(probe, '&')) {
			PParseToken *lp = pparse_next(_pc, probe);
			if (pparse_match_ch(lp, '(') && (lp->flags & PPARSE_TF_OPEN)) {
				PParseToken *rp = pparse_pair(_pc, lp);
				if (rp) {
					PParseToken *br = pparse_next(_pc, rp);
					if (pparse_match_ch(br, '{') && (br->flags & PPARSE_TF_OPEN))
						pparse_error_tok(
						    probe,
						    "volatile/atomic pointer: address of compound literal in "
						    "'orelse' outlives the literal; use block form "
						    "`orelse { ... }` with a named object, or a "
						    "non-literal fallback");
				}
			}
		}
		emit_if_not_var(var_name, true);
		OUT_LIT(" ");
		OUT_TOK(var_name);
		OUT_LIT(" =");
	} else {
		out_char(' ');
		OUT_TOK(var_name);
		OUT_LIT(" = ");
		OUT_TOK(var_name);
		OUT_LIT(" ? ");
		OUT_TOK(var_name);
		OUT_LIT(" :");
	}
	PParseToken *chain_next;
	tok = emit_orelse_fallback_value(tok, stop_comma, &chain_next);
	if (chain_next) {
		if (single_eval_lhs) OUT_LIT("; }");
		else
			out_char(';');
		return emit_orelse_action(chain_next, var_name, single_eval_lhs, stop_comma);
	}
	if (single_eval_lhs) OUT_LIT("; }");
	else
		out_char(';');
	if (pparse_match_ch(tok, ';')) tok = pparse_next(_pc, tok);
	end_statement_after_semicolon();
	return tok;
}

static PParseToken *handle_control_exit_defer(PParseToken *tok) {
	PPARSE_CTX();
	if (tok->tag & PPARSE_TT_RETURN) {
		if (!has_active_defers()) return NULL;
		tok = pparse_next(_pc, tok);
		OUT_LIT(" {");
		tok = emit_return_body(tok, NULL, true);
		OUT_LIT(" }");
	} else {
		bool is_break = tok->tag & PPARSE_TT_BREAK;
		PParseToken *after = pparse_next(_pc, tok);
		bool has_label = after && pparse_is_identifier_like(after);
		bool need = false;
		if (has_label && pparse_feat(PPARSE_F_DEFER)) {
			/* Outer-loop defers are invisible to DEFER_BREAK dry-run
			 * (stops at innermost loop); use goto-style depth check. */
			P1LabelResult info = p1_label_find(after, current_func_idx);
			int td = info.tok ? info.scope_depth : 0;
			need = goto_has_defers(td);
		} else {
			need = control_flow_has_defers(is_break);
		}
		if (!need) return NULL;
		OUT_LIT(" {");
		tok = emit_break_continue_defer(tok, true);
		OUT_LIT(" }");
	}
	end_statement_after_semicolon();
	return tok;
}

static PParseToken *handle_goto_keyword(PParseToken *tok) {
	PPARSE_CTX();
	PParseToken *goto_tok = tok;
	tok = pparse_next(_pc, tok);
	if (pparse_feat(PPARSE_F_DEFER)) {
		// Skip C23 attributes between goto and target: goto [[attr]] *ptr;
		PParseToken *after_attrs = pparse_skip_noise(_pc, tok);
		if (pparse_match_ch(after_attrs, '*')) {
			emit_tok(goto_tok);
			while (tok != after_attrs) {
				tok = emit_advance(tok);
			}
			return tok;
		}

		if (pparse_is_identifier_like(after_attrs)) {
			int target_depth = emit_block_depth - (int)goto_tok->parse_data;
			if (target_depth < 0) target_depth = 0;

			if (goto_has_defers(target_depth)) {
				OUT_LIT(" {");
				emit_goto_defers(target_depth);
				OUT_LIT(" goto");
				// Emit any C23 attributes
				while (tok != after_attrs) {
					tok = emit_advance(tok);
				}
				tok = emit_advance(tok);
				if (pparse_match_ch(tok, ';')) {
					tok = emit_advance(tok);
				}
				OUT_LIT(" }");
				end_statement_after_semicolon();
				return tok;
			}
		}
		emit_tok(goto_tok);
		return tok;
	}

	emit_tok(goto_tok);
	return tok;
}

static PParseToken *handle_sue_body(PParseToken *tok) {
	PParseToken *brace = pparse_find_struct_body_brace(tok);
	if (!brace) return NULL;
	emit_range(tok, brace);
	return handle_open_brace(brace);
}

static PParseToken *handle_open_brace(PParseToken *tok) {
	PRISM_STATE();
	PPARSE_CTX();
	bool did_push = false;
	if (ctrl_state.pending &&
	    (in_ctrl_paren() || !ctrl_state.parens_just_closed || (pparse_ann(tok) & P1_SCOPE_INIT))) {
		if (last_emitted && pparse_match_ch(last_emitted, '(')) {
			pparse_VEC_ENSURE_REALLOC(ctrl_save_stack, ctrl_save_depth + 1, ctrl_save_cap, 16);
			ctrl_save_stack[ctrl_save_depth++] = ctrl_state;
			did_push = true;
		} else {
			emit_tok(tok);
			ctrl_state.brace_depth++;
			return pparse_next(_pc, tok);
		}
	}
	ctrl_state.pending = false;
	ctrl_state.pending_for_paren = false;
	ctrl_state.parens_just_closed = false;
	uint32_t ann = pparse_ann(tok);
	uint16_t sid = pparse_scope_id(tok);
	PParseScopeInfo *si = sid ? &pparse_scope_tree[sid] : NULL;
	bool is_init_scope = ann & P1_SCOPE_INIT;
	tok = emit_advance(tok);
	scope_push_kind(is_init_scope ? SCOPE_INIT : SCOPE_BLOCK);
	ScopeNode *s = &scope_stack[emit_scope_depth - 1];
	s->is_loop = ann & P1_SCOPE_LOOP;
	s->is_switch = ann & P1_SCOPE_SWITCH;
	s->is_struct = is_init_scope || (si && si->is_struct);
	s->is_stmt_expr = si && si->is_stmt_expr;
	if (did_push) s->is_ctrl_se = true;

	if (s->is_stmt_expr) s->saved_defer_shadow_count = defer_shadow_count;
	if (ann & P1_RAW_BLOCK) _ps->raw_block_depth++;
	emit_at_stmt_start = true;
	return tok;
}

static PParseToken *handle_close_brace(PParseToken *tok) {
	PRISM_STATE();
	PPARSE_CTX();
	if (ctrl_state.pending && ctrl_state.brace_depth > 0) {
		ctrl_state.brace_depth--;
		emit_tok(tok);
		return pparse_next(_pc, tok);
	}
	while (emit_scope_depth > 0 && !is_brace_scope(scope_stack[emit_scope_depth - 1].kind)) scope_pop();
	PParseToken *open = pparse_pair(_pc, tok);
	if (open && (pparse_ann(open) & P1_RAW_BLOCK) && _ps->raw_block_depth > 0)
		_ps->raw_block_depth--;
	if (pparse_feat(PPARSE_F_DEFER) && emit_scope_depth > 0) {
		ScopeNode *s = &scope_stack[emit_scope_depth - 1];
		if (defer_count > s->defer_start_idx) {
			emit_defers(DEFER_SCOPE);
			defer_count = s->defer_start_idx;
		}
	}

	bool restore_ctrl = emit_scope_depth > 0 && scope_stack[emit_scope_depth - 1].is_ctrl_se;
	bool closing_non_stmt_brace = emit_scope_depth > 0 && scope_stack[emit_scope_depth - 1].is_struct &&
				      !scope_stack[emit_scope_depth - 1].is_stmt_expr;
	scope_pop();
	if (restore_ctrl && ctrl_save_depth > 0) ctrl_state = ctrl_save_stack[--ctrl_save_depth];
	emit_tok(tok);
	tok = pparse_next(_pc, tok);
	emit_at_stmt_start = !closing_non_stmt_brace;
	return tok;
}

static char **build_clean_environ(void) {
	char **env = cached_env_load();
	if (env) return env;
	int n = 0;
	for (char **e = environ; *e; e++) n++;
	env = malloc((n + 1) * sizeof(char *));
	if (!env) return NULL;
	int j = 0;
	for (char **e = environ; *e; e++) {
#ifdef _WIN32
		if (_strnicmp(*e, "CC=", 3) != 0 && _strnicmp(*e, "PRISM_CC=", 9) != 0) env[j++] = *e;
#else
		if (strncmp(*e, "CC=", 3) != 0 && strncmp(*e, "PRISM_CC=", 9) != 0) env[j++] = *e;
#endif
	}
	env[j] = NULL;
	cached_env_store(env);
	return env;
}

static int wait_for_child(pid_t pid) {
	int status;
	while (waitpid(pid, &status, 0) == -1) {
		if (errno == EINTR) continue;
		perror("waitpid");
		return -1;
	}
	if (WIFEXITED(status)) return WEXITSTATUS(status);
	if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
	return -1;
}

static int spawn_command(char **argv, bool quiet_stderr) {
	char **env = build_clean_environ();
	if (!env) return -1;
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_t *actions_ptr = NULL;
	int devnull = -1;
	if (quiet_stderr) {
		posix_spawn_file_actions_init(&actions);
		devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) posix_spawn_file_actions_adddup2(&actions, devnull, STDERR_FILENO);
		actions_ptr = &actions;
	}

	pid_t pid;
	int err = posix_spawnp(&pid, argv[0], actions_ptr, NULL, argv, env);
	if (actions_ptr) posix_spawn_file_actions_destroy(actions_ptr);
	if (devnull >= 0) close(devnull);
	if (err) {
		fprintf(stderr, "posix_spawnp: %s: %s\n", argv[0], strerror(err));
		return -1;
	}
	return wait_for_child(pid);
}

#ifndef _WIN32
static int run_command(char **argv) {
	return spawn_command(argv, false);
}

static int run_command_quiet(char **argv) {
	return spawn_command(argv, true);
}
#endif

static int
make_temp_file(char *buf, size_t bufsize, const char *prefix, int suffix_len, const char *source_adjacent) {
	int n;
	if (source_adjacent) {
		const char *slash = strrchr(source_adjacent, '/');
		const char *bslash = strrchr(source_adjacent, '\\');
		char dir_path[PATH_MAX];
		if (bslash && (!slash || bslash > slash)) slash = bslash;
		if (slash) {
			int dir_len = (int)(slash - source_adjacent);
			if ((size_t)dir_len >= sizeof(dir_path)) return -1;
			memcpy(dir_path, source_adjacent, (size_t)dir_len);
			dir_path[dir_len] = '\0';
		} else
			strcpy(dir_path, ".");
		if (dir_has_write_bits(dir_path)) {
			if (slash) {
				int dir_len = (int)(slash - source_adjacent);
				n = snprintf(
				    buf, bufsize, "%.*s/.%s.XXXXXX.c", dir_len, source_adjacent, slash + 1);
			} else
				n = snprintf(buf, bufsize, ".%s.XXXXXX.c", source_adjacent);
			suffix_len = 2;
			if (n >= 0 && (size_t)n < bufsize) {
				int fd = mkstemps(buf, suffix_len);
				if (fd >= 0) return fd;
			}
		}
		const char *base = slash ? slash + 1 : source_adjacent;
		n = snprintf(buf, bufsize, "%s.%s.XXXXXX.c", get_tmp_dir(), base);
		suffix_len = 2;
	} else
		n = snprintf(buf, bufsize, "%s%s", get_tmp_dir(), prefix ? prefix : "prism_tmp");
	if (n < 0 || (size_t)n >= bufsize) return -1;
	return suffix_len > 0 ? mkstemps(buf, suffix_len) : mkstemp(buf);
}

static int make_temp_file_registered(
    char *buf, size_t bufsize, const char *prefix, int suffix_len, const char *source_adjacent) {
	sigset_t mask, oldmask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigprocmask(SIG_BLOCK, &mask, &oldmask);
	int fd = make_temp_file(buf, bufsize, prefix, suffix_len, source_adjacent);
	if (fd >= 0) signal_temps_register(buf);
	sigprocmask(SIG_SETMASK, &oldmask, NULL);
	return fd;
}

static const char *path_basename(const char *path) {
	const char *base = path;
	for (const char *p = path; *p; p++) {
		if (*p == '/' || *p == '\\') base = p + 1;
	}
	return base;
}

static const char **alloc_argv(int count) {
	const char **args = calloc((size_t)count, sizeof(*args));
	if (!args) pparse_error("out of memory");
	return args;
}

static const char *cc_next_token(const char *p, const char **start, int *len, bool terminate) {
	while (*p == ' ' || *p == '\t') p++;
	if (!*p) {
		*start = p;
		*len = 0;
		return p;
	}
	if (*p == '"' || *p == '\'') {
		char q = *p++;
		*start = p;
		while (*p && *p != q) p++;
		*len = (int)(p - *start);
		if (*p) {
			if (terminate) *(char *)p = '\0';
			p++;
		}
	} else {
		*start = p;
		while (*p && *p != ' ' && *p != '\t') p++;
		*len = (int)(p - *start);
		if (terminate && *p) *(char *)p++ = '\0';
	}
	return p;
}

static const char *cc_executable(const char *cc) {
	if (!cc || !*cc) return cc;
	/* Prefer the whole string when it is an unquoted path with spaces. */
	{
		const char *trim = cc;
		while (*trim == ' ' || *trim == '\t') trim++;
		size_t tlen = strlen(trim);
		while (tlen > 0 && (trim[tlen - 1] == ' ' || trim[tlen - 1] == '\t')) tlen--;
		bool has_space = false, has_quote = false;
		for (size_t i = 0; i < tlen; i++) {
			if (trim[i] == ' ' || trim[i] == '\t') has_space = true;
			if (trim[i] == '"' || trim[i] == '\'') has_quote = true;
		}
		if (has_space && !has_quote && tlen > 0 && tlen < PATH_MAX) {
			static PRISM_THREAD_LOCAL char whole[PATH_MAX];
			memcpy(whole, trim, tlen);
			whole[tlen] = '\0';
#ifdef _WIN32
			if (_access(whole, 0) == 0) return whole;
#else
			if (access(whole, X_OK) == 0 || access(whole, F_OK) == 0) return whole;
#endif
		}
	}
	const char *start;
	int len;
	cc_next_token(cc, &start, &len, false);
	if (len == 0) return cc;
	static PRISM_THREAD_LOCAL char buf[PATH_MAX];
	if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;
	memcpy(buf, start, len);
	buf[len] = '\0';
	return buf;
}

static void cc_split_into_argv(const char **args, int *argc, const char *cc, char **out_dup) {
	if (out_dup) *out_dup = NULL;
	if (!cc || !*cc) return;
	char *dup = strdup(cc);
	if (!dup) {
		args[(*argc)++] = cc;
		return;
	}
	/* Unquoted paths with spaces (`CC=/path with spaces/cc`) must stay one
	 * argv entry when they name a real executable — splitting yields ENOENT. */
	{
		char *trim = dup;
		while (*trim == ' ' || *trim == '\t') trim++;
		size_t tlen = strlen(trim);
		while (tlen > 0 && (trim[tlen - 1] == ' ' || trim[tlen - 1] == '\t')) trim[--tlen] = '\0';
		bool has_space = false, has_quote = false;
		for (char *q = trim; *q; q++) {
			if (*q == ' ' || *q == '\t') has_space = true;
			if (*q == '"' || *q == '\'') has_quote = true;
		}
		if (has_space && !has_quote) {
#ifdef _WIN32
			int ok = _access(trim, 0) == 0;
#else
			int ok = access(trim, X_OK) == 0 || access(trim, F_OK) == 0;
#endif
			if (ok) {
				if (trim != dup) memmove(dup, trim, tlen + 1);
				args[(*argc)++] = dup;
				if (out_dup) *out_dup = dup;
				return;
			}
		}
	}
	const char *p = dup;
	while (*p) {
		const char *start;
		int len;
		p = cc_next_token(p, &start, &len, true);
		if (len > 0) args[(*argc)++] = start;
	}
	if (out_dup) *out_dup = dup;
}

static int cc_extra_arg_count(const char *cc) {
	if (!cc || !*cc) return 0;
	/* Match cc_split_into_argv: whole-string executable → no extra tokens. */
	{
		const char *trim = cc;
		while (*trim == ' ' || *trim == '\t') trim++;
		size_t tlen = strlen(trim);
		while (tlen > 0 && (trim[tlen - 1] == ' ' || trim[tlen - 1] == '\t')) tlen--;
		bool has_space = false, has_quote = false;
		for (size_t i = 0; i < tlen; i++) {
			if (trim[i] == ' ' || trim[i] == '\t') has_space = true;
			if (trim[i] == '"' || trim[i] == '\'') has_quote = true;
		}
		if (has_space && !has_quote) {
			char tmp[PATH_MAX];
			if (tlen < sizeof(tmp)) {
				memcpy(tmp, trim, tlen);
				tmp[tlen] = '\0';
#ifdef _WIN32
				if (_access(tmp, 0) == 0) return 0;
#else
				if (access(tmp, X_OK) == 0 || access(tmp, F_OK) == 0) return 0;
#endif
			}
		}
	}
	const char *start;
	int len;
	const char *p = cc_next_token(cc, &start, &len, false); // skip first token
	int count = 0;
	while (*p) {
		p = cc_next_token(p, &start, &len, false);
		if (len > 0) count++;
	}
	return count;
}

#ifndef _WIN32
static bool cc_is_msvc(const char *cc) {
	if (!cc || !*cc) return false;
	const char *exe = cc_executable(cc);
	const char *base = path_basename(exe);
	return (strcasecmp(base, "cl") == 0 || strcasecmp(base, "cl.exe") == 0);
}
#endif

static bool is_pp_skip_input_arg(const char *f);
static bool cc_flag_takes_arg(const char *a);
static bool cli_has_cxx_passthrough(const Cli *cli);
static const char *cxx_driver_for_cc(const char *cc);

static void build_pp_argv(const char **args, int *argc, const char *input_file, char **out_cc_dup) {
	PRISM_STATE();
	PPARSE_CTX();
	const char *cc = _ps->extra_compiler ? _ps->extra_compiler : PRISM_DEFAULT_CC;
	bool msvc = cc_is_msvc(cc);
	cc_split_into_argv(args, argc, cc, out_cc_dup);
	if (msvc) {
		args[(*argc)++] = "/E"; // preprocess to stdout
		args[(*argc)++] = "/nologo";
	} else {
		args[(*argc)++] = "-E";
		args[(*argc)++] = "-w";
	}

	for (int i = 0; i < _ps->extra_compiler_flags_count; i++) {
		const char *f = _ps->extra_compiler_flags[i];
		if (msvc) {
			// MSVC cl: /c compile-only, /Fo<path>|/Fo <path>, /Fe likewise.
			if (strcmp(f, "/c") == 0 || strcmp(f, "-c") == 0) continue;
			if (strncmp(f, "/Fo", 3) == 0 || strncmp(f, "/Fe", 3) == 0) {
				if ((strcmp(f, "/Fo") == 0 || strcmp(f, "/Fe") == 0) &&
				    i + 1 < _ps->extra_compiler_flags_count)
					i++; /* skip separate operand (may be /abs/path.obj) */
				continue;
			}
			if (strcmp(f, "/link") == 0) break;
			/* Skip other TUs/objects — including absolute paths (/tmp/x.obj). */
			if (is_pp_skip_input_arg(f)) {
				if (i > 0 && cc_flag_takes_arg(_ps->extra_compiler_flags[i - 1]))
					args[(*argc)++] = f;
				continue;
			}
		} else {
			if (strcmp(f, "-c") == 0 || strcmp(f, "-S") == 0) continue;
			/* Accidental MSVC slash-flags on a Unix driver. */
			if (strcmp(f, "/c") == 0) continue;
			if (strncmp(f, "/Fo", 3) == 0 || strncmp(f, "/Fe", 3) == 0) {
				if ((strcmp(f, "/Fo") == 0 || strcmp(f, "/Fe") == 0) &&
				    i + 1 < _ps->extra_compiler_flags_count)
					i++;
				continue;
			}
			if (strcmp(f, "-o") == 0) {
				i++;
				continue;
			}
			if (f[0] == '-' && f[1] == 'o' && f[2] != '\0') continue;
			/* -save-temps with -E writes confusing sidecar files; drop it. */
			if (!strcmp(f, "-save-temps") || !strncmp(f, "-save-temps=", 12) ||
			    !strcmp(f, "--save-temps") || !strncmp(f, "--save-temps=", 13))
				continue;
			/* Skip sibling TUs/objects, but not operands of -D/-include/-I/… */
			if (f[0] != '-' && is_pp_skip_input_arg(f)) {
				if (i > 0 && cc_flag_takes_arg(_ps->extra_compiler_flags[i - 1]))
					args[(*argc)++] = f;
				continue;
			}
		}
		args[(*argc)++] = f;
	}

	for (int i = 0; i < _ps->dep_flags_count; i++) args[(*argc)++] = _ps->dep_flags[i];
	for (int i = 0; i < _ps->extra_include_count; i++) {
		args[(*argc)++] = msvc ? "/I" : "-I";
		args[(*argc)++] = _ps->extra_include_paths[i];
	}

	if (msvc) {
		// MSVC: /D concatenated with macro
		int needed =
		    _ps->extra_define_count + 3; // +3 for __PRISM__, __PRISM_DEFER__, __PRISM_ZEROINIT__
		if (needed > pp_define_bufs_cap) {
			int old_cap = pp_define_bufs_cap;
			pp_define_bufs_cap = needed > 64 ? needed : 64;
			pp_define_bufs = realloc(pp_define_bufs, pp_define_bufs_cap * sizeof(char *));
			if (!pp_define_bufs) pparse_error("out of memory");
			for (int i = old_cap; i < pp_define_bufs_cap; i++) pp_define_bufs[i] = NULL;
		}
		int buf_idx = 0;
		for (int i = 0; i < _ps->extra_define_count; i++) {
			int len = snprintf(NULL, 0, "/D%s", _ps->extra_defines[i]) + 1;
			pp_define_bufs[buf_idx] = realloc(pp_define_bufs[buf_idx], len);
			if (!pp_define_bufs[buf_idx]) pparse_error("out of memory");
			snprintf(pp_define_bufs[buf_idx], len, "/D%s", _ps->extra_defines[i]);
			args[(*argc)++] = pp_define_bufs[buf_idx++];
		}
#define MSVC_DEFINE(str)                                                                                     \
	do {                                                                                                 \
		const char *_s = (str);                                                                      \
		int _l = (int)strlen(_s) + 1;                                                                \
		pp_define_bufs[buf_idx] = realloc(pp_define_bufs[buf_idx], _l);                              \
		if (!pp_define_bufs[buf_idx]) pparse_error("out of memory");                                        \
		memcpy(pp_define_bufs[buf_idx], _s, _l);                                                     \
		args[(*argc)++] = pp_define_bufs[buf_idx++];                                                 \
	} while (0)
		MSVC_DEFINE("/D__PRISM__=1");
		if (pparse_feat(PPARSE_F_DEFER)) MSVC_DEFINE("/D__PRISM_DEFER__=1");
		if (pparse_feat(PPARSE_F_ZEROINIT)) MSVC_DEFINE("/D__PRISM_ZEROINIT__=1");
#undef MSVC_DEFINE
	} else {
		for (int i = 0; i < _ps->extra_define_count; i++) {
			args[(*argc)++] = "-D";
			args[(*argc)++] = _ps->extra_defines[i];
		}
		args[(*argc)++] = "-D__PRISM__=1";
		if (pparse_feat(PPARSE_F_DEFER)) args[(*argc)++] = "-D__PRISM_DEFER__=1";
		if (pparse_feat(PPARSE_F_ZEROINIT)) args[(*argc)++] = "-D__PRISM_ZEROINIT__=1";
	}

	// Add GNU feature test macro on non-Windows, non-MSVC so POSIX/GNU
#ifndef _WIN32
	if (!msvc) {
		bool user_has_gnu = false;
		for (int i = 0; i < _ps->extra_define_count; i++) {
			if (strncmp(_ps->extra_defines[i], "_GNU_SOURCE", 11) == 0) user_has_gnu = true;
		}
		for (int i = 0; i < _ps->extra_compiler_flags_count; i++) {
			const char *f = _ps->extra_compiler_flags[i];
			if (strncmp(f, "-D_GNU_SOURCE", 13) == 0 || strncmp(f, "-U_GNU_SOURCE", 13) == 0)
				user_has_gnu = true;
			/* Split `-U _GNU_SOURCE` / `-D _GNU_SOURCE`. */
			if ((strcmp(f, "-U") == 0 || strcmp(f, "-D") == 0) &&
			    i + 1 < _ps->extra_compiler_flags_count &&
			    strncmp(_ps->extra_compiler_flags[i + 1], "_GNU_SOURCE", 11) == 0)
				user_has_gnu = true;
		}
		if (!user_has_gnu) args[(*argc)++] = "-D_GNU_SOURCE";
#ifdef __APPLE__
		{
			bool user_has_darwin = false;
			for (int i = 0; i < _ps->extra_define_count; i++)
				if (strncmp(_ps->extra_defines[i], "_DARWIN_C_SOURCE", 16) == 0)
					user_has_darwin = true;
			for (int i = 0; i < _ps->extra_compiler_flags_count; i++) {
				const char *f = _ps->extra_compiler_flags[i];
				if (strncmp(f, "-D_DARWIN_C_SOURCE", 18) == 0 ||
				    strncmp(f, "-U_DARWIN_C_SOURCE", 18) == 0)
					user_has_darwin = true;
			}
			if (!user_has_darwin) args[(*argc)++] = "-D_DARWIN_C_SOURCE";
		}
#endif
	}
#endif

	for (int i = 0; i < _ps->extra_force_include_count; i++) {
		args[(*argc)++] = msvc ? "/FI" : "-include";
		args[(*argc)++] = _ps->extra_force_includes[i];
	}

	/* Clang rejects `cc -E -` without `-x`. Map `-x none` (extension guessing)
	 * to `-x c` for stdin, and inject `-x c` when the user omitted `-x`. */
	if (!msvc && input_file && !strcmp(input_file, "-")) {
		bool has_x = false;
		for (int i = 0; i < *argc - 1; i++) {
			if (!strcmp(args[i], "-x")) {
				has_x = true;
				if (!strcmp(args[i + 1], "none")) args[i + 1] = "c";
				break;
			}
		}
		if (!has_x) {
			args[(*argc)++] = "-x";
			args[(*argc)++] = "c";
		}
	}

	args[(*argc)++] = input_file;
	args[*argc] = NULL;
}

static char *make_dir_line(const char *p, int len) {
	char *s = malloc(1 + len + 2);
	if (s) {
		s[0] = '#';
		memcpy(s + 1, p, len);
		s[1 + len] = '\n';
		s[2 + len] = '\0';
	}
	return s;
}

static inline void free_source_defines(void) {
	PRISM_STATE();
	for (int i = 0; i < _ps->source_define_count; i++) {
		free(_ps->source_defines[i]);
		free(_ps->source_define_guards[i]);
	}
	_ps->source_defines = NULL;
	_ps->source_define_guards = NULL;
	_ps->source_define_count = 0;
	_ps->source_define_cap = 0;
}

static bool has_unclosed_block_comment(const char *p, char **raw_delim_out) {
	if (raw_delim_out) *raw_delim_out = NULL;
	bool in_str = false, in_chr = false;
	for (; *p && *p != '\n'; p++) {
		if (in_str) {
			if (*p == '\\' && p[1]) p++;
			else if (*p == '"')
				in_str = false;
			continue;
		}
		if (in_chr) {
			if (*p == '\\' && p[1]) p++;
			else if (*p == '\'')
				in_chr = false;
			continue;
		}
		if (*p == 'R' && p[1] == '"') {
			const char *q = p + 2;
			const char *dstart = q;
			while (*q && *q != '(' && *q != ')' && *q != '\\' && *q != ' ' && *q != '\t' &&
			       *q != '\n' && (q - dstart) < 17)
				q++;
			if (*q == '(') {
				int dlen = (int)(q - dstart);
				const char *content = q + 1;
				for (const char *r = content; *r && *r != '\n'; r++) {
					if (*r == ')' && (dlen == 0 || strncmp(r + 1, dstart, dlen) == 0) &&
					    r[1 + dlen] == '"') {
						p = r + 1 + dlen; // skip to closing "
						goto raw_closed;
					}
				}
				if (raw_delim_out) {
					*raw_delim_out = malloc(dlen + 1);
					if (*raw_delim_out) {
						memcpy(*raw_delim_out, dstart, dlen);
						(*raw_delim_out)[dlen] = '\0';
					}
				}
				return false;
			raw_closed:;
				continue;
			}
		} else if ((*p == 'u' || *p == 'U' || *p == 'L') && !in_str && !in_chr) {
			const char *rp = p;
			if (*rp == 'u' && rp[1] == '8') rp += 2;
			else
				rp++;
			if (*rp == 'R' && rp[1] == '"') {
				p = rp - 1;
				continue;
			} // will hit R" on next iter
		}
		if (*p == '"') {
			in_str = true;
			continue;
		}
		if (*p == '\'') {
			in_chr = true;
			continue;
		}
		if (p[0] == '/' && p[1] == '/') return false;
		if (p[0] == '/' && p[1] == '*') {
			const char *close = strstr(p + 2, "*/");
			if (!close) return true;
			p = close + 1;
		}
	}
	return false;
}

/* Does `p` leave us inside an unterminated block comment or raw string?
 *
 * Written out four times in collect_source_defines, once per place a line can
 * be examined. has_unclosed_block_comment hands back a malloc'd delimiter for
 * the raw-string case, so the branch that does not take ownership has to free
 * it - the part that is easiest to drop when copying. */
#define PP_SCAN_LINE_OPENERS(p)                                                                      \
	do {                                                                                         \
		char *_rd = NULL;                                                                    \
		if (has_unclosed_block_comment((p), &_rd)) {                                         \
			in_block_comment = true;                                                     \
		} else if (_rd && cond_depth == 0) {                                                 \
			in_raw_string = true;                                                        \
			raw_delim = _rd;                                                             \
			raw_delim_len = (int)strlen(_rd);                                            \
		} else                                                                               \
			free(_rd);                                                                   \
	} while (0)

static void collect_source_defines(const char *input_file) {
	PRISM_STATE();
	PPARSE_CTX();
	free_source_defines();
	if (!input_file || pparse_feat(PPARSE_F_FLATTEN)) return;
	FILE *f = fopen(input_file, "r");
	if (!f) return;
	char *line = NULL;
	size_t line_cap = 0;
	bool in_continuation = false;
	bool in_block_comment = false;
	bool in_hash_block_comment = false; // block comment started between # and directive name
	bool in_raw_string = false;	    // inside a multi-line raw string literal
	char *raw_delim = NULL;		    // delimiter for the current raw string (malloc'd)
	int raw_delim_len = 0;
	int cond_depth = 0; // #if/#ifdef/#ifndef nesting depth

	typedef struct {
		char *opening;	// e.g. "#ifdef __APPLE__\n"
		char *branches; // accumulated "#else\n" or "#elif EXPR\n" text (NULL
				// initially)
		int branches_len;
		int branches_cap;
		bool extractable; // false if opening/branch had continuation (multi-line
				  // expr)
	} CondStackEntry;

	int cond_stack_cap = 32;
	CondStackEntry *cond_stack = calloc(cond_stack_cap, sizeof(CondStackEntry));
	if (!cond_stack) {
		free(line);
		fclose(f);
		return;
	}
	while (getline(&line, &line_cap, f) >= 0) {
		char *p = line;
		char *line_end;
		bool dir_has_continuation;
		char *dir_text_end;
		int dir_text_len;
		if (in_block_comment) {
			char *end = strstr(line, "*/");
			if (!end) continue;
			in_block_comment = false;
			p = end + 2;
			while (*p == ' ' || *p == '\t') p++;
			if (in_hash_block_comment) {
				in_hash_block_comment = false;
				goto parse_directive;
			}
			if (*p != '#') continue;
			goto have_hash;
		}
		if (in_raw_string) {
			for (char *r = line; *r && *r != '\n'; r++) {
				if (*r == ')' &&
				    (raw_delim_len == 0 || strncmp(r + 1, raw_delim, raw_delim_len) == 0) &&
				    r[1 + raw_delim_len] == '"') {
					in_raw_string = false;
					free(raw_delim);
					raw_delim = NULL;
					raw_delim_len = 0;
					p = r + 2 + raw_delim_len;
					goto after_raw_string_close;
				}
			}
			continue; // entire line is inside raw string
		after_raw_string_close:
			while (*p == ' ' || *p == '\t') p++;
			if (*p == '\n' || *p == '\0') continue;
			if (*p == '#') goto have_hash;
			{
				PP_SCAN_LINE_OPENERS(p);
			}
			continue;
		}
		if (in_continuation) {
			char *end = line + strlen(line);
			while (end > line && (end[-1] == '\n' || end[-1] == '\r')) end--;
			in_continuation = (end > line && end[-1] == '\\');
			continue;
		}
		while (*p == ' ' || *p == '\t') p++;
		/* Digraph `%:` and trigraph `??=` are `#` — collect before PP. */
		if (p[0] == '%' && p[1] == ':') {
			p += 2;
			goto parse_directive;
		}
		if (p[0] == '?' && p[1] == '?' && p[2] == '=') {
			p += 3;
			goto parse_directive;
		}
		if (*p != '#') {
			if (*p == '\n' || *p == '\0' || (p[0] == '/' && p[1] == '/')) goto check_continuation;
			if (p[0] == '/' && p[1] == '*') {
				char *close = strstr(p + 2, "*/");
				if (!close) {
					in_block_comment = true;
					goto check_continuation;
				}
				p = close + 2;
				while (*p == ' ' || *p == '\t') p++;
				if (*p != '#') {
					PP_SCAN_LINE_OPENERS(p);
					goto check_continuation;
				}
				goto have_hash;
			}
			{
				PP_SCAN_LINE_OPENERS(p);
			}
			goto check_continuation;
		}
	have_hash:
		p++; // skip '#'
		while (*p == ' ' || *p == '\t' || (p[0] == '/' && p[1] == '*')) {
			if (p[0] == '/' && p[1] == '*') {
				char *end = strstr(p + 2, "*/");
				if (!end) {
					in_block_comment = true;
					in_hash_block_comment = true;
					goto check_continuation;
				} // unterminated block comment
				p = end + 2;
			} else
				p++;
		}
	parse_directive:;
		line_end = p + strlen(p);
		while (line_end > p && (line_end[-1] == '\n' || line_end[-1] == '\r')) line_end--;
		dir_has_continuation = (line_end > p && line_end[-1] == '\\');
		dir_text_end = line_end;
		if (dir_has_continuation) dir_text_end--;
		while (dir_text_end > p && (dir_text_end[-1] == ' ' || dir_text_end[-1] == '\t'))
			dir_text_end--;
		dir_text_len = (int)(dir_text_end - p);
		if (strncmp(p, "ifdef", 5) == 0 || strncmp(p, "ifndef", 6) == 0 ||
		    (strncmp(p, "if", 2) == 0 && (p[2] == ' ' || p[2] == '\t' || p[2] == '('))) {
			if (cond_depth >= cond_stack_cap) {
				int old = cond_stack_cap;
				size_t nc = pparse_vec_grow_cap((size_t)old, (size_t)cond_depth + 1, 32);
				CondStackEntry *ns = realloc(cond_stack, nc * sizeof(CondStackEntry));
				if (ns) {
					memset(ns + old, 0, (nc - (size_t)old) * sizeof(CondStackEntry));
					cond_stack = ns;
					cond_stack_cap = (int)nc;
				}
			}
			if (cond_depth < cond_stack_cap) {
				memset(&cond_stack[cond_depth], 0, sizeof(cond_stack[0]));
				cond_stack[cond_depth].opening = make_dir_line(p, dir_text_len);
				cond_stack[cond_depth].extractable = !dir_has_continuation;
			}
			cond_depth++;
			goto check_continuation;
		}
		if (strncmp(p, "endif", 5) == 0) {
			if (cond_depth > 0) {
				cond_depth--;
				if (cond_depth < cond_stack_cap) {
					free(cond_stack[cond_depth].opening);
					free(cond_stack[cond_depth].branches);
					cond_stack[cond_depth].opening = NULL;
					cond_stack[cond_depth].branches = NULL;
				}
			}
			goto check_continuation;
		}
		if (strncmp(p, "else", 4) == 0 || strncmp(p, "elif", 4) == 0) {
			if (cond_depth > 0 && cond_depth <= cond_stack_cap) {
				int d = cond_depth - 1;
				int blen = 1 + dir_text_len + 1; // "#" + text + "\n"
				int need = cond_stack[d].branches_len + blen + 1;
				if (need > cond_stack[d].branches_cap) {
					int nc = need * 2;
					char *nb = realloc(cond_stack[d].branches, nc);
					if (nb) {
						cond_stack[d].branches = nb;
						cond_stack[d].branches_cap = nc;
					}
				}
				if (cond_stack[d].branches && cond_stack[d].branches_cap >= need) {
					char *dst = cond_stack[d].branches + cond_stack[d].branches_len;
					dst[0] = '#';
					memcpy(dst + 1, p, dir_text_len);
					dst[1 + dir_text_len] = '\n';
					cond_stack[d].branches_len += blen;
					cond_stack[d].branches[cond_stack[d].branches_len] = '\0';
				}
				if (dir_has_continuation) cond_stack[d].extractable = false;
			}
			goto check_continuation;
		}

		/* Stop collecting at the first top-level #include.
		 *
		 * emit_consumed_defines writes the collected defines ABOVE the
		 * re-emitted includes, so anything defined after an include in the
		 * source cannot be hoisted without changing what it means - the
		 * macro may deliberately override something the header defined, or
		 * depend on it. Collecting only the prefix before the first include
		 * is what makes the hoist sound. Conditional includes do not stop
		 * the scan: they are re-emitted with their guards intact. */
		if (strncmp(p, "include", 7) == 0 && cond_depth == 0) break;
		if (cond_depth > cond_stack_cap) goto check_continuation;
		if (cond_depth > 0) {
			bool can_extract = true;
			for (int d = 0; d < cond_depth; d++) {
				if (!cond_stack[d].extractable || !cond_stack[d].opening) {
					can_extract = false;
					break;
				}
			}
			if (!can_extract) goto check_continuation;
		}

		if (strncmp(p, "define", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
			p += 6;
			while (*p == ' ' || *p == '\t') p++;
			char *name_start = p;
			while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '(') p++;
			int name_len = (int)(p - name_start);
			if (name_len <= 0) goto check_continuation;
			/* Function-like macro: the parameter list is part of the
			 * spelling and has to travel with the name, because the
			 * collected form is a -D spec and `-D'F(a)=(a)'` is how a
			 * function-like macro is written there.
			 *
			 * `(` must be adjacent to the name: `#define F (x)` is
			 * object-like with value `(x)`, and the scan above already
			 * stopped at the space in that case. Parameter lists cannot
			 * nest, so the first `)` closes the list. A list continued
			 * onto the next line is still skipped, as before.
			 *
			 * Dropping these silently was the bug: object-like defines
			 * survived a transpile and function-like ones did not. */
			if (*p == '(') {
				char *params_end = strchr(p, ')');
				if (!params_end) goto check_continuation;
				p = params_end + 1;
				name_len = (int)(p - name_start);
			}
			char *saved_name = malloc(name_len + 1);
			if (!saved_name) goto check_continuation;
			memcpy(saved_name, name_start, name_len);
			saved_name[name_len] = '\0';
			while (*p == ' ' || *p == '\t') p++;
			char *val_start = p;
			char *end = val_start + strlen(val_start);
			while (end > val_start &&
			       (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
				end--;
			bool has_continuation = (end > val_start && end[-1] == '\\');
			if (has_continuation) end--;
			bool prev_had_ws = (end > val_start && (end[-1] == ' ' || end[-1] == '\t'));
			while (end > val_start && (end[-1] == ' ' || end[-1] == '\t')) end--;
			int val_len = (int)(end - val_start);
			char *full_val = NULL;
			int full_val_len = 0;
			if (has_continuation) {
				size_t cap = (val_len > 0 ? val_len : 0) + 256;
				full_val = malloc(cap);
				if (!full_val) {
					free(saved_name);
					goto check_continuation;
				}
				if (val_len > 0) {
					memcpy(full_val, val_start, val_len);
					full_val_len = val_len;
				}
				while (getline(&line, &line_cap, f) >= 0) {
					char *lp = line;
					bool cur_leading_ws = (*lp == ' ' || *lp == '\t');
					while (*lp == ' ' || *lp == '\t') lp++;
					char *le = lp + strlen(lp);
					while (le > lp && (le[-1] == '\n' || le[-1] == '\r' ||
							   le[-1] == ' ' || le[-1] == '\t'))
						le--;
					bool more = (le > lp && le[-1] == '\\');
					if (more) le--;
					bool cur_trailing_ws = (le > lp && (le[-1] == ' ' || le[-1] == '\t'));
					while (le > lp && (le[-1] == ' ' || le[-1] == '\t')) le--;
					int chunk = (int)(le - lp);
					if (chunk > 0) {
						size_t need = full_val_len + 1 + chunk + 1;
						if (need > cap) {
							cap = need * 2;
							char *tmp = realloc(full_val, cap);
							if (!tmp) {
								free(full_val);
								free(saved_name);
								goto check_continuation;
							}
							full_val = tmp;
						}
						if (full_val_len > 0 && (prev_had_ws || cur_leading_ws))
							full_val[full_val_len++] = ' ';
						memcpy(full_val + full_val_len, lp, chunk);
						full_val_len += chunk;
					}
					prev_had_ws = cur_trailing_ws;
					if (!more) break;
				}
				if (full_val) {
					val_start = full_val;
					val_len = full_val_len;
				}
			}

			{
				// Work on a mutable copy if we don't have one already
				char *val = full_val;
				int vlen = val ? full_val_len : val_len;
				if (!val && vlen > 0) {
					val = malloc(vlen + 1);
					if (!val) {
						free(saved_name);
						goto check_continuation;
					}
					memcpy(val, val_start, vlen);
					val[vlen] = '\0';
				}
				bool modified = false;
				if (val) {
					for (int vi = 0; vi < vlen - 1; vi++) {
						if (val[vi] == '"' || val[vi] == '\'') {
							char q = val[vi++];
							while (vi < vlen && val[vi] != q) {
								if (val[vi] == '\\' && vi + 1 < vlen) vi++;
								vi++;
							}
							continue;
						}
						if (val[vi] == '/' && val[vi + 1] == '/') break;
						if (val[vi] == '/' && val[vi + 1] == '*') {
							int cs = vi; // comment start
							vi += 2;
							char *close = NULL;
							for (int ci = vi; ci < vlen - 1; ci++) {
								if (val[ci] == '*' && val[ci + 1] == '/') {
									close = val + ci;
									break;
								}
							}
							if (close) {
								int ce = (int)(close - val) + 2;
								memmove(val + cs + 1, val + ce, vlen - ce);
								vlen -= (ce - cs - 1);
								val[cs] = ' ';
								val[vlen] = '\0';
								vi = cs; // rescan from space
								modified = true;
							} else {
								bool found_close = false;
								while (getline(&line, &line_cap, f) >= 0) {
									char *ce = strstr(line, "*/");
									if (ce) {
										char *rest = ce + 2;
										while (*rest == ' ' ||
										       *rest == '\t')
											rest++;
										char *re =
										    rest + strlen(rest);
										while (re > rest &&
										       (re[-1] == '\n' ||
											re[-1] == '\r' ||
											re[-1] == ' ' ||
											re[-1] == '\t'))
											re--;
										int rlen = (int)(re - rest);
										if (rlen > 0) {
											char *nv = realloc(
											    val,
											    cs + 1 + rlen +
												1);
											if (nv) {
												if (val ==
												    full_val)
													full_val =
													    nv;
												val = nv;
												val[cs] = ' ';
												memcpy(
												    val + cs +
													1,
												    rest,
												    rlen);
												vlen = cs +
												       1 +
												       rlen;
												val[vlen] =
												    '\0';
											}
										} else {
											vlen = cs;
											while (
											    vlen > 0 &&
											    (val[vlen - 1] ==
												 ' ' ||
											     val[vlen - 1] ==
												 '\t'))
												vlen--;
											val[vlen] = '\0';
										}
										found_close = true;
										modified = true;
										break;
									}
								}
								if (!found_close) {
									vlen = cs;
									while (vlen > 0 &&
									       (val[vlen - 1] == ' ' ||
										val[vlen - 1] == '\t'))
										vlen--;
									val[vlen] = '\0';
									modified = true;
								}
								vi = vlen; // done
							}
						}
					}
				}
				if (val != full_val) free(full_val);
				if (modified || val != full_val) {
					full_val = val;
					full_val_len = vlen;
					val_start = full_val;
					val_len = full_val_len;
				}
			}

			int total = name_len + (val_len > 0 ? 1 + val_len : 0) + 1;
			char *def = malloc(total);
			if (!def) {
				free(full_val);
				free(saved_name);
				goto check_continuation;
			}
			memcpy(def, saved_name, name_len);
			free(saved_name);
			if (val_len > 0) {
				def[name_len] = '=';
				memcpy(def + name_len + 1, val_start, val_len);
				def[name_len + 1 + val_len] = '\0';
			} else
				def[name_len] = '\0';
			free(full_val);
			char *guard = NULL;
			if (cond_depth > 0) {
				int glen = 0;
				for (int d = 0; d < cond_depth; d++) {
					glen += (int)strlen(cond_stack[d].opening);
					if (cond_stack[d].branches) glen += cond_stack[d].branches_len;
				}
				guard = malloc(glen + 1);
				if (guard) {
					int pos = 0;
					for (int d = 0; d < cond_depth; d++) {
						int olen = (int)strlen(cond_stack[d].opening);
						memcpy(guard + pos, cond_stack[d].opening, olen);
						pos += olen;
						if (cond_stack[d].branches) {
							memcpy(guard + pos,
							       cond_stack[d].branches,
							       cond_stack[d].branches_len);
							pos += cond_stack[d].branches_len;
						}
					}
					guard[pos] = '\0';
				}
			}

			int old_cap = _ps->source_define_cap;
			PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena,
					 _ps->source_defines,
					 _ps->source_define_count,
					 _ps->source_define_cap,
					 8,
					 char *);
			if ((int)_ps->source_define_cap != old_cap)
				_ps->source_define_guards =
				    pparse_arena_realloc(&_pc->main_arena,
						  _ps->source_define_guards,
						  sizeof(char *) * old_cap,
						  sizeof(char *) * _ps->source_define_cap);
			_ps->source_define_guards[_ps->source_define_count] = guard;
			_ps->source_defines[_ps->source_define_count++] = def;
		}
	check_continuation: {
		char *end = line + strlen(line);
		while (end > line && (end[-1] == '\n' || end[-1] == '\r')) end--;
		in_continuation = (end > line && end[-1] == '\\');
		if (!in_continuation && !in_block_comment && !in_raw_string) {
			PP_SCAN_LINE_OPENERS(line);
		}
	}
	}
	for (int d = 0; d < cond_depth && d < cond_stack_cap; d++) {
		free(cond_stack[d].opening);
		free(cond_stack[d].branches);
	}
	free(cond_stack);
	free(raw_delim);
	free(line);
	fclose(f);
}

/* Read `path` whole into a malloc'd buffer with `pad` trailing NUL bytes.
 * Callers want different padding - the tokenizer needs 8 for its SWAR scan,
 * plain text consumers need 1 - which is the only way the two copies of this
 * function differed. Returns NULL on any failure. */
static char *read_file_bytes(const char *path, size_t pad) {
	FILE *f = fopen(path, "rb");
	long sz = 0;
	char *buf = NULL;
	if (!f) return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)sz + pad);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	memset(buf + (size_t)sz, 0, pad);
	return buf;
}

#define read_file_padded(path) read_file_bytes((path), 8)

static bool input_is_dot_i(const char *path) {
	size_t n = path ? strlen(path) : 0;
	return n >= 2 && path[n - 2] == '.' && (path[n - 1] == 'i' || path[n - 1] == 'I');
}

/* ---- Preprocessed-output cache ----------------------------------------
 *
 * `cc -E` is 39-99% of prism's wall time (8.19 of 8.28 ms on a no-include file,
 * 103.8 of 262.8 ms on test.c), so every transpile-phase optimisation is
 * competing for a small slice. Caching the preprocessor's output removes the
 * spawn entirely whenever nothing it read has changed.
 *
 * Key    = 128-bit hash of the exact argv, the resolved compiler's identity,
 *          and the include-affecting environment.
 * Valid  = every file that contributed to the output still has the same size
 *          and mtime. The dependency list is recovered from the `# N "file"`
 *          linemarkers in the output itself, so no -MD sidecar is needed.
 *
 * A stale hit produces a silently wrong build, so every uncertainty resolves to
 * "miss": unparsable header, missing dep, coarse timestamp, or a source using
 * one of the build-time date macros (whose expansion is not a function of the
 * inputs - see pp_text_has_time_macro). Set PRISM_NO_PP_CACHE=1 to disable.
 */
#define PP_CACHE_MAGIC "PRISMPPC2\n"

/* Sub-second mtime, where the platform exposes it. POSIX.1-2008 requires
 * st_mtime to be a macro for st_mtim.tv_sec, which is the portable probe for
 * the timespec fields. Without nanoseconds a file rewritten inside the same
 * second it was recorded is indistinguishable from an unchanged one, so
 * pp_cache_store refuses to record entries that recent. */
#if defined(_WIN32)
#define PP_MTIME_NSEC(st) 0LL
#elif defined(__APPLE__)
#define PP_MTIME_NSEC(st) ((long long)(st).st_mtimespec.tv_nsec)
#elif defined(st_mtime)
#define PP_MTIME_NSEC(st) ((long long)(st).st_mtim.tv_nsec)
#else
#define PP_MTIME_NSEC(st) 0LL
#endif

#ifdef _WIN32
#define PP_PATHLIST_SEP ';'
#else
#define PP_PATHLIST_SEP ':'
#endif

typedef struct {
	uint64_t a, b;
} PPKey;

/* Identity of one dependency. ctime catches metadata-only replacements
 * (`cp -p`, checkout of a same-size file) that leave size and mtime intact. */
typedef struct {
	long long size, mtime_sec, mtime_nsec, ctime_sec;
} PPStat;

static bool pp_stat_id(const char *path, PPStat *o) {
	struct stat st;
	if (stat(path, &st) != 0) return false;
	o->size = (long long)st.st_size;
	o->mtime_sec = (long long)st.st_mtime;
	o->mtime_nsec = PP_MTIME_NSEC(st);
	o->ctime_sec = (long long)st.st_ctime;
	return true;
}

static void ppk_feed(PPKey *k, const void *p, size_t n) {
	const unsigned char *s = (const unsigned char *)p;
	for (size_t i = 0; i < n; i++) {
		k->a = (k->a ^ s[i]) * 0x100000001b3ULL;
		k->b = (k->b + s[i] + 1) * 0x9e3779b97f4a7c15ULL;
		k->b ^= k->b >> 29;
	}
	/* Length-feed so {"ab","c"} and {"a","bc"} cannot collide. */
	k->a ^= n * 0xff51afd7ed558ccdULL;
}

static void ppk_feed_str(PPKey *k, const char *s) { ppk_feed(k, s ? s : "", s ? strlen(s) : 0); }

/* Environment variables the preprocessor consults for header search. A change
 * here changes the output without changing any file prism can stat. */
static const char *const pp_cache_env_keys[] = {
    "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
    "SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "SOURCE_DATE_EPOCH", "PRISM_CC",
};

static bool pp_is_dir_sep(char c) {
#ifdef _WIN32
	return c == '/' || c == '\\';
#else
	return c == '/';
#endif
}

/* Resolve `name` through PATH and fold the binary's identity into the key, so
 * upgrading or switching compilers invalidates every entry. */
static void ppk_feed_compiler(PPKey *k, const char *name) {
	PPStat id;
	bool has_sep = false;
	for (const char *c = name; *c; c++)
		if (pp_is_dir_sep(*c)) has_sep = true;
	if (has_sep) {
		if (pp_stat_id(name, &id)) ppk_feed(k, &id, sizeof id);
		return;
	}
	const char *path = getenv("PATH");
	if (!path) return;
	size_t nlen = strlen(name);
	for (const char *p = path; *p;) {
		const char *sep = strchr(p, PP_PATHLIST_SEP);
		size_t dlen = sep ? (size_t)(sep - p) : strlen(p);
		if (dlen && dlen + nlen + 8 < PATH_MAX) {
			char buf[PATH_MAX];
			memcpy(buf, p, dlen);
			buf[dlen] = '/';
			memcpy(buf + dlen + 1, name, nlen + 1);
			if (pp_stat_id(buf, &id)) {
				ppk_feed_str(k, buf);
				ppk_feed(k, &id, sizeof id);
				return;
			}
#ifdef _WIN32
			/* PATH entries on Windows omit the extension. */
			memcpy(buf + dlen + 1 + nlen, ".exe", 5);
			if (pp_stat_id(buf, &id)) {
				ppk_feed_str(k, buf);
				ppk_feed(k, &id, sizeof id);
				return;
			}
#endif
		}
		if (!sep) break;
		p = sep + 1;
	}
}

/* snprintf truncation is never benign here: a clipped path can name a different
 * entry than the key describes, which is exactly the collision the 128-bit key
 * exists to prevent. Every path build goes through this and fails closed. */
static bool pp_pathf(char *out, size_t cap, const char *fmt, ...) {
	va_list ap;
	int n = 0;
	va_start(ap, fmt);
	n = vsnprintf(out, cap, fmt, ap);
	va_end(ap);
	return n >= 0 && (size_t)n < cap;
}

/* Cache root, or NULL if it cannot be composed or created. Callers treat NULL
 * as "cache unavailable" and fall back to running the preprocessor. */
static const char *pp_cache_dir(void) {
	static PRISM_THREAD_LOCAL char dir[PATH_MAX];
	static PRISM_THREAD_LOCAL int state; /* 0 unknown, 1 ready, -1 unusable */
	const char *base = NULL;
	const char *xdg = NULL;
	const char *home = NULL;
	bool ok = false;
	char *s = NULL;

	if (state) return state > 0 ? dir : NULL;
	state = -1;

	base = getenv("PRISM_PP_CACHE_DIR");
	if (base && *base) {
		ok = pp_pathf(dir, sizeof dir, "%s", base);
	} else {
		xdg = getenv("XDG_CACHE_HOME");
		home = getenv("HOME");
#ifdef _WIN32
		if (!home || !*home) home = getenv("LOCALAPPDATA");
#endif
		if (xdg && *xdg)
			ok = pp_pathf(dir, sizeof dir, "%s/prism-pp", xdg);
		else if (home && *home)
			ok = pp_pathf(dir, sizeof dir, "%s/.cache/prism-pp", home);
		else
			ok = pp_pathf(dir, sizeof dir, "%sprism-pp", get_tmp_dir());
	}
	/* Leave room for "/<32 hex>.pp.<pid>.tmp" appended by callers. */
	if (!ok || strlen(dir) + 64 >= sizeof dir) {
		dir[0] = '\0';
		return NULL;
	}
	/* Parents may not exist; create each level, ignoring EEXIST. */
	for (s = dir + 1; *s; s++) {
		char save = *s;
		if (!pp_is_dir_sep(*s)) continue;
		*s = '\0';
		mkdir(dir, 0700);
		*s = save;
	}
	mkdir(dir, 0700);
	state = 1;
	return dir;
}

/* A source expanding one of the build-time date macros does not produce output
 * that is a pure function of its inputs, so it must never be cached.
 *
 * The names are assembled from fragments rather than written whole. The scan is
 * a plain substring match, so spelling them literally anywhere in this file -
 * including in a comment - would make prism.c match itself and exclude prism
 * from its own cache. Hence also the circumlocution in these comments. */
static bool pp_text_has_time_macro(const char *buf, size_t len) {
	static const char *const m[] = {"__DA"
					"TE__",
					"__TI"
					"ME__",
					"__TIMES"
					"TAMP__"};
	for (int i = 0; i < 3; i++) {
		size_t ml = strlen(m[i]);
		if (len < ml) continue;
		for (size_t j = 0; j + ml <= len; j++)
			if (buf[j] == '_' && memcmp(buf + j, m[i], ml) == 0) return true;
	}
	return false;
}

/* Decode one `# <line> "<path>"` linemarker; returns the unescaped path length
 * written to `out`, or 0 if the line is not a usable marker. */
static size_t pp_marker_path(const char *l, const char *end, char *out, size_t outcap) {
	if (l >= end || *l != '#') return 0;
	l++;
	while (l < end && (*l == ' ' || *l == '\t')) l++;
	if (l >= end || *l < '0' || *l > '9') return 0;
	while (l < end && *l >= '0' && *l <= '9') l++;
	while (l < end && (*l == ' ' || *l == '\t')) l++;
	if (l >= end || *l != '"') return 0;
	l++;
	size_t n = 0;
	while (l < end && *l != '"') {
		char c = *l++;
		if (c == '\\' && l < end) c = *l++;
		if (n + 1 >= outcap) return 0;
		out[n++] = c;
	}
	if (l >= end || *l != '"') return 0;
	out[n] = '\0';
	/* `<built-in>`, `<command-line>` and `<stdin>` are not files. */
	return (n && out[0] != '<') ? n : 0;
}

static bool pp_cache_key(PPKey *k, char **argv, int argc, const char *input_file) {
	char abs[PATH_MAX];
	*k = (PPKey){0xcbf29ce484222325ULL, 0x9e3779b97f4a7c15ULL};
	ppk_feed_str(k, PP_CACHE_MAGIC);
	for (int i = 0; i < argc; i++) {
		if (!argv[i]) return false;
		ppk_feed_str(k, argv[i]);
	}
	if (argc > 0) ppk_feed_compiler(k, argv[0]);
	for (size_t i = 0; i < sizeof pp_cache_env_keys / sizeof *pp_cache_env_keys; i++) {
		ppk_feed_str(k, pp_cache_env_keys[i]);
		ppk_feed_str(k, getenv(pp_cache_env_keys[i]));
	}
	if (realpath(input_file, abs)) ppk_feed_str(k, abs);
	return true;
}

static bool pp_cache_path(const PPKey *k, char *out, size_t cap) {
	const char *dir = pp_cache_dir();
	if (!dir) return false;
	return pp_pathf(out, cap, "%s/%016llx%016llx.pp", dir, (unsigned long long)k->a,
			(unsigned long long)k->b);
}

/* Portable atomic publish. POSIX rename replaces; Win32 needs the explicit flag. */
static bool pp_replace_file(const char *tmp, const char *dst) {
#ifdef _WIN32
	return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING) != 0;
#else
	return rename(tmp, dst) == 0;
#endif
}

/* Return the cached payload if every recorded dependency is unchanged.
 *
 * All declarations precede the first `goto`: prism rejects a jump that skips an
 * initialised declaration, and this file is compiled by prism when self-hosting. */
static char *pp_cache_load(const PPKey *k) {
	char path[PATH_MAX];
	char line[PATH_MAX + 128];
	char *out = NULL;
	FILE *f = NULL;
	long ndeps = 0;
	long long plen = 0;
	long i = 0;

	if (!pp_cache_path(k, path, sizeof path)) return NULL;
	f = fopen(path, "rb");
	if (!f) return NULL;

	if (!fgets(line, sizeof line, f) || strcmp(line, PP_CACHE_MAGIC) != 0) goto done;
	if (!fgets(line, sizeof line, f) || sscanf(line, "deps %ld", &ndeps) != 1) goto done;
	if (ndeps < 0 || ndeps > 65536) goto done;

	for (i = 0; i < ndeps; i++) {
		PPStat rec, now;
		int off = 0;
		char *p = NULL;
		size_t pl = 0;
		if (!fgets(line, sizeof line, f)) goto done;
		if (sscanf(line, "%lld %lld %lld %lld %n", &rec.size, &rec.mtime_sec, &rec.mtime_nsec,
			   &rec.ctime_sec, &off) < 4 ||
		    off <= 0)
			goto done;
		p = line + off;
		pl = strlen(p);
		while (pl && (p[pl - 1] == '\n' || p[pl - 1] == '\r')) p[--pl] = '\0';
		if (!pl || !pp_stat_id(p, &now)) goto done;
		if (now.size != rec.size || now.mtime_sec != rec.mtime_sec ||
		    now.mtime_nsec != rec.mtime_nsec || now.ctime_sec != rec.ctime_sec)
			goto done;
	}

	if (!fgets(line, sizeof line, f) || sscanf(line, "payload %lld", &plen) != 1) goto done;
	if (plen < 0) goto done;
	out = malloc((size_t)plen + 8);
	if (!out) goto done;
	if (fread(out, 1, (size_t)plen, f) != (size_t)plen) {
		free(out);
		out = NULL;
		goto done;
	}
	memset(out + plen, 0, 8);
done:
	fclose(f);
	return out;
}

/* ---- eviction ---------------------------------------------------------
 *
 * Bounded by total size and age. Scanning on every store would cost more than
 * the cache saves, so a marker file rate-limits the sweep to once an hour. */
#define PP_PRUNE_MARKER ".prune"

typedef struct {
	long long mtime, size;
	char name[64];
} PPEntry;

static long long pp_env_ll(const char *name, long long dflt) {
	const char *v = getenv(name);
	long long r = 0;
	if (!v || !*v) return dflt;
	r = strtoll(v, NULL, 10);
	return r > 0 ? r : dflt;
}

static int pp_entry_cmp(const void *a, const void *b) {
	long long x = ((const PPEntry *)a)->mtime, y = ((const PPEntry *)b)->mtime;
	return (x > y) - (x < y);
}

/* Invoke `cb` for every `*.pp` entry in the cache directory. */
static void pp_each_entry(void (*cb)(const char *dir, const char *name, void *ud), void *ud) {
	const char *dir = pp_cache_dir();
	if (!dir) return;
#ifdef _WIN32
	char glob[PATH_MAX];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	snprintf(glob, sizeof glob, "%s\\*.pp", dir);
	h = FindFirstFileA(glob, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) cb(dir, fd.cFileName, ud);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *d = opendir(dir);
	struct dirent *e;
	if (!d) return;
	while ((e = readdir(d)) != NULL) {
		size_t n = strlen(e->d_name);
		if (n > 3 && strcmp(e->d_name + n - 3, ".pp") == 0) cb(dir, e->d_name, ud);
	}
	closedir(d);
#endif
}

typedef struct {
	PPEntry *v;
	int n, cap;
	long long total;
} PPScan;

static void pp_collect_cb(const char *dir, const char *name, void *ud) {
	PPScan *s = (PPScan *)ud;
	char full[PATH_MAX];
	PPStat id;
	if (strlen(name) >= sizeof s->v[0].name) return;
	if (!pp_pathf(full, sizeof full, "%s/%s", dir, name)) return;
	if (!pp_stat_id(full, &id)) return;
	if (s->n == s->cap) {
		int nc = s->cap ? s->cap * 2 : 128;
		PPEntry *nv = realloc(s->v, (size_t)nc * sizeof *nv);
		if (!nv) return;
		s->v = nv;
		s->cap = nc;
	}
	s->v[s->n].mtime = id.mtime_sec;
	s->v[s->n].size = id.size;
	snprintf(s->v[s->n].name, sizeof s->v[s->n].name, "%s", name);
	s->n++;
	s->total += id.size;
}

static void pp_cache_prune(void) {
	const char *dir = pp_cache_dir();
	char marker[PATH_MAX], full[PATH_MAX];
	long long max_bytes = pp_env_ll("PRISM_PP_CACHE_MAX_MB", 1024) * 1024 * 1024;
	long long max_age = pp_env_ll("PRISM_PP_CACHE_MAX_DAYS", 14) * 24 * 3600;
	long long now = (long long)time(NULL);
	PPStat mst;
	PPScan s;
	FILE *mf = NULL;
	int i = 0;

	if (!dir) return;
	if (!pp_pathf(marker, sizeof marker, "%s/%s", dir, PP_PRUNE_MARKER)) return;
	if (pp_stat_id(marker, &mst) && now - mst.mtime_sec < 3600) return;
	mf = fopen(marker, "wb");
	if (mf) fclose(mf);

	memset(&s, 0, sizeof s);
	pp_each_entry(pp_collect_cb, &s);
	if (!s.v) return;

	for (i = 0; i < s.n; i++) {
		if (now - s.v[i].mtime <= max_age) continue;
		if (!pp_pathf(full, sizeof full, "%s/%s", dir, s.v[i].name)) continue;
		if (remove(full) == 0) {
			s.total -= s.v[i].size;
			s.v[i].size = -1; /* already gone */
		}
	}
	if (s.total > max_bytes) {
		/* Oldest first, down to 80% of the cap so this does not run every hour. */
		qsort(s.v, (size_t)s.n, sizeof *s.v, pp_entry_cmp);
		for (i = 0; i < s.n && s.total > max_bytes * 4 / 5; i++) {
			if (s.v[i].size < 0) continue;
			if (!pp_pathf(full, sizeof full, "%s/%s", dir, s.v[i].name)) continue;
			if (remove(full) == 0) s.total -= s.v[i].size;
		}
	}
	free(s.v);
}

static void pp_cache_store(const PPKey *k, const char *input_file, const char *payload, size_t len) {
	enum { MAX_DEPS = 4096 };
	char path[PATH_MAX], tmp[PATH_MAX], abs[PATH_MAX];
	char **deps = NULL;
	int ndeps = 0, i = 0;
	long long now = (long long)time(NULL);
	const char *end = payload + len;
	const char *l = NULL;
	FILE *f = NULL;
	bool ok = true;

	deps = calloc(MAX_DEPS, sizeof *deps);
	if (!deps) return;
	if (!realpath(input_file, abs)) goto out;
	deps[ndeps++] = strdup(abs);

	/* The dependency set is recovered from the output's own linemarkers, so no
	 * -MD sidecar has to be produced or kept in sync. */
	for (l = payload; l < end;) {
		char buf[PATH_MAX], can[PATH_MAX];
		const char *nl = NULL;
		size_t n = pp_marker_path(l, end, buf, sizeof buf);
		if (n) {
			int seen = 0;
			/* Linemarkers carry the path as cc saw it, which is often relative
			 * to the invoking directory. Validating a relative path would stat
			 * whatever sits at that name in a future run's cwd - a different
			 * file with the same name would then validate as unchanged. Canonical
			 * paths only; if a dep cannot be resolved, do not cache at all. */
			if (!realpath(buf, can)) goto out;
			for (i = 0; i < ndeps; i++)
				if (strcmp(deps[i], can) == 0) {
					seen = 1;
					break;
				}
			if (!seen) {
				if (ndeps >= MAX_DEPS) goto out;
				deps[ndeps++] = strdup(can);
			}
		}
		nl = memchr(l, '\n', (size_t)(end - l));
		if (!nl) break;
		l = nl + 1;
	}
	for (i = 0; i < ndeps; i++)
		if (!deps[i]) goto out;

	if (!pp_cache_path(k, path, sizeof path)) goto out;
	if (!pp_pathf(tmp, sizeof tmp, "%s.%ld.tmp", path, (long)getpid())) goto out;
	f = fopen(tmp, "wb");
	if (!f) goto out;
	ok = fputs(PP_CACHE_MAGIC, f) >= 0 && fprintf(f, "deps %d\n", ndeps) > 0;
	for (i = 0; ok && i < ndeps; i++) {
		PPStat id;
		if (!pp_stat_id(deps[i], &id)) {
			ok = false;
			break;
		}
		/* Without sub-second resolution, a file written in the second we are
		 * recording could change again and still compare equal. Skip the entry;
		 * the next build a second later records it safely. */
		if (id.mtime_nsec == 0 && now - id.mtime_sec < 2) {
			ok = false;
			break;
		}
		ok = fprintf(f, "%lld %lld %lld %lld %s\n", id.size, id.mtime_sec, id.mtime_nsec,
			     id.ctime_sec, deps[i]) > 0;
	}
	if (ok) ok = fprintf(f, "payload %llu\n", (unsigned long long)len) > 0;
	if (ok) ok = fwrite(payload, 1, len, f) == len;
	if (fclose(f) != 0) ok = false;
	if (!ok || !pp_replace_file(tmp, path)) remove(tmp);
	if (ok) pp_cache_prune();
out:
	for (i = 0; i < ndeps; i++) free(deps[i]);
	free(deps);
}

static bool pp_cache_enabled(void) {
	const char *v = getenv("PRISM_NO_PP_CACHE");
	return !(v && *v && strcmp(v, "0") != 0);
}

/* Decided before the spawn, not at store time: a source that expands a date
 * macro can never be cached, so probing and storing are both wasted work. The
 * scan is deliberately conservative - it matches the name anywhere, including
 * in comments and string literals. A false positive costs one cache miss; a
 * false negative would be a silently stale build. */
static bool pp_source_is_cacheable(const char *input_file) {
	struct stat st;
	FILE *f = NULL;
	char *b = NULL;
	size_t got = 0;
	bool ok = false;

	if (stat(input_file, &st) != 0) return false;
	/* Not off_t: that name is POSIX and MSVC only exposes it under
	 * _CRT_DECLARE_NONSTDC_NAMES. st_size is an integer type on every target. */
	if (st.st_size <= 0 || (long long)st.st_size > (long long)(64 << 20)) return false;
	f = fopen(input_file, "rb");
	if (!f) return false;
	b = malloc((size_t)st.st_size);
	if (!b) {
		fclose(f);
		return false;
	}
	got = fread(b, 1, (size_t)st.st_size, f);
	fclose(f);
	ok = !pp_text_has_time_macro(b, got);
	free(b);
	return ok;
}

/* `--prism-cache-clear` / `--prism-cache-info` support. */
static void pp_clear_cb(const char *dir, const char *name, void *ud) {
	char full[PATH_MAX];
	long *n = (long *)ud;
	if (!pp_pathf(full, sizeof full, "%s/%s", dir, name)) return;
	if (remove(full) == 0) (*n)++;
}

static void pp_info_cb(const char *dir, const char *name, void *ud) {
	char full[PATH_MAX];
	PPScan *s = (PPScan *)ud;
	PPStat id;
	(void)name;
	if (!pp_pathf(full, sizeof full, "%s/%s", dir, name)) return;
	if (!pp_stat_id(full, &id)) return;
	s->n++;
	s->total += id.size;
}

static int pp_cache_clear(void) {
	long n = 0;
	pp_each_entry(pp_clear_cb, &n);
	printf("prism: cleared %ld cached preprocessor %s from %s\n", n, n == 1 ? "entry" : "entries",
	       pp_cache_dir());
	return 0;
}

static int pp_cache_info(void) {
	PPScan s;
	memset(&s, 0, sizeof s);
	pp_each_entry(pp_info_cb, &s);
	printf("prism preprocessor cache\n  dir      %s\n  entries  %d\n  size     %.1f MB\n"
	       "  limits   %lld MB / %lld days  (PRISM_PP_CACHE_MAX_MB, PRISM_PP_CACHE_MAX_DAYS)\n"
	       "  status   %s\n",
	       pp_cache_dir(), s.n, s.total / (1024.0 * 1024.0), pp_env_ll("PRISM_PP_CACHE_MAX_MB", 1024),
	       pp_env_ll("PRISM_PP_CACHE_MAX_DAYS", 14), pp_cache_enabled() ? "enabled" : "disabled (PRISM_NO_PP_CACHE)");
	return 0;
}

static char *preprocess_with_cc(const char *input_file) {
	PRISM_STATE();
	collect_source_defines(input_file);
	/* `.i` is already preprocessed. GCC's `cc -E file.i` emits nothing, so
	 * re-running the preprocessor would drop the whole TU (including orelse). */
	if (input_is_dot_i(input_file)) {
		char *buf = read_file_padded(input_file);
		if (!buf) {
			fprintf(stderr, "pparse_error: cannot read preprocessed input: %s\n", input_file);
			return NULL;
		}
		size_t len = strlen(buf);
		FILE *f = fopen(input_file, "rb");
		long sz = -1;
		if (f) {
			if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
			fclose(f);
		}
		if (sz > 0 && (size_t)sz != len) {
			fprintf(stderr,
				"pparse_error: preprocessed input '%s' contains embedded null bytes\n",
				input_file);
			free(buf);
			return NULL;
		}
		if (sz >= 2) {
			unsigned char b0 = (unsigned char)buf[0], b1 = (unsigned char)buf[1];
			if ((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF)) {
				fprintf(stderr,
					"pparse_error: preprocessed input '%s' looks like UTF-16 (BOM); "
					"re-save as UTF-8/ASCII .i or pass the original .c\n",
					input_file);
				free(buf);
				return NULL;
			}
		}
		return buf;
	}
	const char *pp_cc = _ps->extra_compiler ? _ps->extra_compiler : PRISM_DEFAULT_CC;
	int argcap = 16 + cc_extra_arg_count(pp_cc) + _ps->extra_compiler_flags_count + _ps->dep_flags_count +
		     _ps->extra_include_count * 2 + _ps->extra_define_count * 2 +
		     _ps->extra_force_include_count * 2;
	const char **args = alloc_argv(argcap);
	int argc = 0;
	char *cc_dup = NULL;
	build_pp_argv(args, &argc, input_file, &cc_dup);
	char **argv = (char **)args;

	/* Cache probe. The key covers argv, the compiler binary and the include
	 * environment; validity is checked against every file the cached output
	 * was built from. A hit skips the spawn entirely. */
	PPKey key;
	bool cacheable = pp_cache_enabled() && pp_source_is_cacheable(input_file) &&
			 pp_cache_key(&key, argv, argc, input_file);
	if (cacheable) {
		char *hit = pp_cache_load(&key);
		if (hit) {
			if (prism_profile) fprintf(stderr, "[prism-prof] pp-cache=hit\n");
			free(cc_dup);
			free((void *)args);
			return hit;
		}
	}

	char *buf = NULL;
	char *result = NULL;
	int read_fd = -1;
	pid_t pid = 0;
	bool rerun_for_stderr = false;
	int pipefd[2];
	if (pipe(pipefd) == -1) {
		perror("pipe");
		free(cc_dup);
		free((void *)args);
		return NULL;
	}
	read_fd = pipefd[0];
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addclose(&fa, pipefd[0]);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&fa, pipefd[1]);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY | O_TRUNC, 0644);
	char **env = build_clean_environ();
	int err = posix_spawnp(&pid, argv[0], &fa, NULL, argv, env);
	posix_spawn_file_actions_destroy(&fa);
	close(pipefd[1]);
	if (err) {
		fprintf(stderr, "posix_spawnp: %s\n", strerror(err));
		pid = 0; // not spawned — pid is undefined on pparse_error
		goto cleanup;
	}

	{
		size_t cap = 8192, len = 0;
		buf = malloc(cap);
		if (!buf) goto cleanup;
		ssize_t n;
		while ((n = read(read_fd, buf + len, cap - len - 1)) > 0 || (n == -1 && errno == EINTR)) {
			if (n == -1) continue;
			len += (size_t)n;
			if (len + 1 >= cap) {
				cap = pparse_vec_grow_cap(cap, len + 2, 8192);
				char *tmp = realloc(buf, cap);
				if (!tmp) goto cleanup;
				buf = tmp;
			}
		}
		close(read_fd);
		read_fd = -1;
		buf[len] = '\0';
		if (strlen(buf) < len) {
			fprintf(stderr, "pparse_error: preprocessor output contains null bytes\n");
			goto cleanup;
		}

		char *fitted = realloc(buf, len + 8);
		if (!fitted) goto cleanup;
		buf = fitted;
		memset(buf + len, 0, 8);
	}

	{
		int status;
		while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
		pid = 0; // waited
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			rerun_for_stderr = true;
			goto cleanup;
		}
	}

	result = buf;
	buf = NULL; // ownership transferred
	if (cacheable) {
		if (prism_profile) fprintf(stderr, "[prism-prof] pp-cache=miss\n");
		pp_cache_store(&key, input_file, result, strlen(result));
	}

cleanup:
	free(buf);
	if (read_fd >= 0) close(read_fd);
	if (pid > 0) waitpid(pid, NULL, 0);
	if (rerun_for_stderr) {
		/* Re-run so cc diagnostics hit stderr. Must discard stdout —
		 * argv is `cc -E …`, so an unredirected rerun dumps the full
		 * preprocessed translation unit onto the user's stdout. */
		posix_spawn_file_actions_t fa2;
		posix_spawn_file_actions_init(&fa2);
		posix_spawn_file_actions_addopen(
		    &fa2, STDOUT_FILENO, "/dev/null", O_WRONLY | O_TRUNC, 0644);
		char **env2 = build_clean_environ();
		pid_t pid2 = 0;
		int err2 = env2 ? posix_spawnp(&pid2, argv[0], &fa2, NULL, argv, env2) : -1;
		posix_spawn_file_actions_destroy(&fa2);
		if (err2)
			fprintf(stderr, "posix_spawnp: %s\n", strerror(err2));
		else
			wait_for_child(pid2);
	}
	free(cc_dup);
	free((void *)args);
	return result;
}

// --- Pass 2: Transpilation Engine ---

static inline void track_ctrl_paren_open(void) {
	ScopeKind k;
	if (ctrl_state.pending_for_paren) {
		k = SCOPE_FOR_PAREN;
		ctrl_state.pending_for_paren = false;
	} else
		k = SCOPE_CTRL_PAREN;
	ctrl_state.parens_just_closed = false;
	scope_push_kind(k);
	ScopeNode *sn = &scope_stack[emit_scope_depth - 1];
	uint8_t pk = ctrl_state.pending_paren_kw;
	ctrl_state.pending_paren_kw = 0;
	if (pk == 1) sn->is_loop = true;
	else if (pk == 2)
		sn->is_switch = true;
	emit_at_stmt_start = (k == SCOPE_FOR_PAREN);
}

static inline void track_ctrl_paren_close(void) {
	// ctrl_state.pending is still live) must not leak parens_just_closed.
	if (emit_scope_depth == 0) return;
	ScopeKind k = scope_stack[emit_scope_depth - 1].kind;
	if (k != SCOPE_FOR_PAREN && k != SCOPE_CTRL_PAREN) return;
	scope_pop();
	ctrl_state.parens_just_closed = true;
	emit_at_stmt_start = true;
}

static inline void track_ctrl_semicolon(void) {
	if (in_for_init()) {
		/* for/switch/if C23 init: FOR_PAREN → CTRL_PAREN after first ';'.
     * Preserve is_loop/is_switch so break/continue in the condition
     * (or for-increment) still stop at this paren for defer_walk. */
		bool was_loop = scope_stack[emit_scope_depth - 1].is_loop;
		bool was_switch = scope_stack[emit_scope_depth - 1].is_switch;
		scope_pop();
		scope_push_kind(SCOPE_CTRL_PAREN);
		scope_stack[emit_scope_depth - 1].is_loop = was_loop;
		scope_stack[emit_scope_depth - 1].is_switch = was_switch;
	} else if (!in_ctrl_paren())
		ctrl_reset();
}

static PRISM_ALWAYS_INLINE inline void track_generic_token(PParseToken *tok) {
	if (tok->len != 1) return;
	char c = tok->ch0;
	if (c != '(' && c != ')') return;
	if (!in_generic()) return;
	if (c == '(') scope_push_kind(SCOPE_GENERIC);
	else if (emit_scope_depth > 0 && scope_stack[emit_scope_depth - 1].kind == SCOPE_GENERIC)
		scope_pop();
}

static PRISM_ALWAYS_INLINE inline void track_common_token_state(PParseToken *tok) {
	if (__builtin_expect(ctrl_state.pending && tok->len == 1, 0)) {
		char c = tok->ch0;
		if (!in_generic()) {
			if (c == '(') {
				/* Entire `__attribute__((...))` group is opaque — including
         * the inner `((` and nested attribute argument parens. After the
         * control condition has closed, a leading '(' is a
         * parenthesized braceless body / expression — not another
         * condition paren. Nested if/while still arm pending_for_paren
         * / pending_paren_kw (or clear parens_just_closed) first. */
				if (!(pparse_ann(tok) & P1_IN_ATTR_ARGS) &&
				    !(ctrl_state.parens_just_closed && !ctrl_state.pending_for_paren &&
				      !ctrl_state.pending_paren_kw))
					track_ctrl_paren_open();
			} else if (c == ')') {
				if (!(pparse_ann(tok) & P1_IN_ATTR_ARGS)) track_ctrl_paren_close();
			}
		}
	}
	track_generic_token(tok);
}

/* Skip (no emit) statement prefixes: attrs/_Pragma and one-or-more labels. */
static PParseToken *skip_stmt_prefixes(PParseToken *tok) {
	PPARSE_CTX();
	for (;;) {
		PParseToken *after = pparse_skip_noise(_pc, tok);
		if (after != tok) {
			tok = after;
			continue;
		}
		if (pparse_is_identifier_like(tok)) {
			PParseToken *colon = pparse_skip_noise(_pc, pparse_next(_pc, tok));
			if (colon && pparse_match_ch(colon, ':') &&
			    !(pparse_next(_pc, colon) && pparse_match_ch(pparse_next(_pc, colon), ':'))) {
				tok = pparse_next(_pc, colon);
				continue;
			}
		}
		break;
	}
	return tok;
}

/* Emit tokens in [from, to). */
static PParseToken *emit_through(PParseToken *from, PParseToken *to) {
	while (from && from != to && from->kind != PPARSE_TK_EOF) from = emit_advance(from);
	return from;
}

static bool orelse_has_chain(PParseToken *start, bool comma_term) {
	PPARSE_CTX();
	int pd = 0;
	for (PParseToken *p = start; p->kind != PPARSE_TK_EOF; p = pparse_next(_pc, p)) {
		if (p->flags & PPARSE_TF_OPEN) pd++;
		else if (p->flags & PPARSE_TF_CLOSE)
			pd--;
		else if (pd == 0 && (pparse_match_ch(p, ';') || (comma_term && pparse_match_ch(p, ','))))
			break;
		if (pd == 0 && (pparse_ann(p) & P1_IS_ORELSE_KW)) return true;
	}
	return false;
}

static bool bare_is_stmt_end(PParseToken *s, bool comma_term) {
	return pparse_match_ch(s, ';') || (comma_term && pparse_match_ch(s, ','));
}

static PParseToken *
bare_emit_fallback_expr(PParseToken *t, bool comma_term, PParseToken *lhs, PParseToken *eq, bool restart_on_orelse) {
	PPARSE_CTX();
	int fd = 0;
	if (restart_on_orelse) {
		if (orelse_has_chain(t, comma_term)) OUT_LIT("(");
		else
			OUT_LIT("(void)(");
		emit_range_no_prep(lhs, eq);
		OUT_LIT(" = (");
	}
	while (t->kind != PPARSE_TK_EOF) {
		EMIT_TRY_TYPEOF_ORELSE(t)
		if ((t->flags & PPARSE_TF_OPEN) && (pparse_match_ch(t, '(') || pparse_match_ch(t, '['))) {
			t = walk_balanced(t);
			continue;
		}
		if (t->flags & PPARSE_TF_OPEN) fd++;
		else if (t->flags & PPARSE_TF_CLOSE)
			fd--;
		else if (fd == 0 && bare_is_stmt_end(t, comma_term))
			break;
		if (restart_on_orelse && fd == 0 && is_orelse_keyword(t)) {
			OUT_LIT(")) ? (void)0 : ");
			t = pparse_next(_pc, t);
			if (orelse_has_chain(t, comma_term)) OUT_LIT("(");
			else
				OUT_LIT("(void)(");
			emit_range_no_prep(lhs, eq);
			OUT_LIT(" = (");
			continue;
		}
		t = emit_advance(t);
	}
	return t;
}

static PParseToken *emit_bare_orelse_impl(PParseToken *t, PParseToken *end, bool comma_term, bool brace_wrap) {
	PRISM_STATE();
	PPARSE_CTX();
	PParseToken *orelse_tok = find_bare_orelse(t);
	if (!orelse_tok || (end && pparse_loc(_pc, orelse_tok) >= pparse_loc(_pc, end))) return NULL;

	PParseExprTopLevel top = pparse_scan_expr_top_level(t, orelse_tok);
	PParseToken *last_comma = comma_term ? top.last_comma : NULL;
	PParseToken *post_comma_t = last_comma ? pparse_next(_pc, last_comma) : t;
	PParseToken *bare_lhs_start = post_comma_t;
	PParseToken *bare_assign_eq = comma_term ? top.segment_assignment : top.last_assignment;
	if (bare_assign_eq && !pparse_match_ch(bare_assign_eq, '='))
		pparse_error_tok(bare_assign_eq,
			  "bare assignment with 'orelse' cannot use compound operators "
			  "(e.g. +=, -=); use a plain '=' assignment");

	PParseToken *after_orelse = pparse_next(_pc, orelse_tok);
	bool is_bare_fallback = bare_assign_eq && is_orelse_value_fallback(after_orelse);

	if (!is_bare_fallback) return NULL; // caller handles non-bare fallback

	if (brace_wrap) OUT_LIT(" {");
	if (last_comma) {
		emit_range(t, last_comma);
		out_char(';');
		t = post_comma_t;
	}

	// Hoist preprocessor directives before the wrapper
	for (PParseToken *s = t; s != orelse_tok; s = pparse_next(_pc, s)) {
		if (s->kind == PPARSE_TK_PREP_DIR) {
			emit_tok(s);
			out_char('\n');
			_ps->last_line_no++;
		}
	}
	bool fallback_has_compound_literal = false;
	int fallback_depth = 0;
	PParseToken *fallback_prev = NULL;
	PPARSE_FOR_TAIL(s, after_orelse) {
		if (pparse_match_ch(s, '{') &&
		    (fallback_depth == 0 || (fallback_prev && pparse_match_ch(fallback_prev, ')')))) {
			fallback_has_compound_literal = true;
			break;
		}
		if (s->flags & PPARSE_TF_OPEN) fallback_depth++;
		else if (s->flags & PPARSE_TF_CLOSE)
			fallback_depth--;
		else if (fallback_depth == 0 && bare_is_stmt_end(s, comma_term))
			break;
		fallback_prev = s;
	}

	out_char(' ');
	// a constraint violation (C23 §6.7.2.5p2).
	// function return types are never VM (C11 §6.7.6.3p1).
	bool lhs_has_indirection = bare_assign_eq &&
				   bare_lhs_has_indirection(bare_lhs_start, bare_assign_eq);
	if (fallback_has_compound_literal) {
		OUT_LIT("(");
		emit_balanced_range(bare_lhs_start, orelse_tok);
		OUT_LIT(") ? (void)0 : ");
		t = bare_emit_fallback_expr(after_orelse, comma_term, bare_lhs_start, bare_assign_eq, true);
		OUT_LIT("));");
	} else {
		// (C23 §6.7.2.5p2), covered by the . / -> check.
		unsigned oe_id = _ps->ret_counter++;
		bool rhs_has_member = false;
		PPARSE_FOR_RANGE(s, pparse_next(_pc, bare_assign_eq), orelse_tok)
			if (s->tag & PPARSE_TT_MEMBER) {
				rhs_has_member = true;
				break;
			}
		OUT_LIT("{ ");
		emit_typeof_keyword();
		out_char('(');
		if (lhs_has_indirection) {
			/* Bit-field members are not valid typeof operands; `+ 0`
			 * forces integer promotion. Skip for plain idents/calls. */
			if (rhs_has_member) OUT_LIT("(");
			emit_balanced_range(pparse_next(_pc, bare_assign_eq), orelse_tok);
			if (rhs_has_member) OUT_LIT(")+0");
		} else
			emit_range_no_prep(bare_lhs_start, bare_assign_eq);
		OUT_LIT(") __prism_oe_");
		out_uint(oe_id);
		OUT_LIT(" = (");
		emit_balanced_range(pparse_next(_pc, bare_assign_eq), orelse_tok);
		OUT_LIT(");");
		t = after_orelse;
		{
			int nest = 0;
			while (true) {
				bool is_last = !orelse_has_chain(t, comma_term);
				OUT_LIT(" if (__prism_oe_");
				out_uint(oe_id);
				OUT_LIT(") { ");
				emit_range_no_prep(bare_lhs_start, bare_assign_eq);
				OUT_LIT(" = __prism_oe_");
				out_uint(oe_id);
				OUT_LIT("; } else { ");
				if (is_last) {
					/* Chain ending in return/goto/break/continue/{...}
					 * must use action lowering — wrapping `return` in
					 * `(...)` is a hard backend pparse_error. */
					if ((t->tag & (PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE | PPARSE_TT_GOTO)) ||
					    pparse_match_ch(t, '{')) {
						t = emit_orelse_action(t, NULL, false, NULL);
						OUT_LIT(" }");
						break;
					}
					emit_range_no_prep(bare_lhs_start, bare_assign_eq);
					OUT_LIT(" = (");
					t = bare_emit_fallback_expr(
					    t, comma_term, bare_lhs_start, bare_assign_eq, false);
					OUT_LIT("); }");
					break;
				}
				nest++;
				PParseToken *fb_start = t;
				PParseToken *fb_orelse = NULL;
				{
					int fd = 0;
					for (PParseToken *s = t; s->kind != PPARSE_TK_EOF; s = pparse_next(_pc, s)) {
						if ((s->flags & PPARSE_TF_OPEN) &&
						    (pparse_match_ch(s, '(') || pparse_match_ch(s, '['))) {
							s = pparse_pair(_pc, s);
							continue;
						}
						if (s->flags & PPARSE_TF_OPEN) fd++;
						else if (s->flags & PPARSE_TF_CLOSE)
							fd--;
						else if (fd == 0 && bare_is_stmt_end(s, comma_term))
							break;
						if (fd == 0 && (pparse_ann(s) & P1_IS_ORELSE_KW)) {
							fb_orelse = s;
							break;
						}
					}
				}
				oe_id = _ps->ret_counter++;
				emit_typeof_keyword();
				out_char('(');
				if (lhs_has_indirection) {
					bool mid_has_member = false;
					PPARSE_FOR_RANGE(s, fb_start, fb_orelse)
						if (s->tag & PPARSE_TT_MEMBER) {
							mid_has_member = true;
							break;
						}
					if (mid_has_member) OUT_LIT("(");
					emit_balanced_range(fb_start, fb_orelse);
					if (mid_has_member) OUT_LIT(")+0");
				} else
					emit_range_no_prep(bare_lhs_start, bare_assign_eq);
				OUT_LIT(") __prism_oe_");
				out_uint(oe_id);
				OUT_LIT(" = (");
				emit_balanced_range(fb_start, fb_orelse);
				OUT_LIT(");");
				t = pparse_next(_pc, fb_orelse);
			}
			for (int i = 0; i < nest; i++) OUT_LIT(" }");
		}
		OUT_LIT(" }");
	}
	if (bare_is_stmt_end(t, comma_term)) t = pparse_next(_pc, t);
	if (brace_wrap) OUT_LIT(" }");
	if (end && pparse_idx(_pc, t) > pparse_idx(_pc, end)) t = end;
	return t;
}

static PParseToken *emit_orelse_condition_wrap(PParseToken *t, PParseToken *orelse_tok) {
	PRISM_STATE();
	PPARSE_CTX();
	for (PParseToken *s = t; s != orelse_tok; s = pparse_next(_pc, s))
		if (s->kind == PPARSE_TK_PREP_DIR) {
			emit_tok(s);
			out_char('\n');
			_ps->last_line_no++;
		}
	OUT_LIT(" {");
	OUT_LIT(" if (!(");
	emit_range_no_prep(t, orelse_tok);
	OUT_LIT("))");
	return pparse_next(_pc, orelse_tok);
}

static PParseToken *emit_deferred_orelse(PParseToken *t, PParseToken *end) {
	PPARSE_CTX();
	PParseToken *body = skip_stmt_prefixes(t);
	PParseToken *orelse_tok = find_bare_orelse(body);
	if (!orelse_tok || (end && pparse_loc(_pc, orelse_tok) >= pparse_loc(_pc, end))) return NULL;
	t = emit_through(t, body);
	PParseToken *result = emit_bare_orelse_impl(t, end, false, false);
	if (result) return result;
	t = emit_orelse_condition_wrap(t, orelse_tok);
	t = emit_orelse_action(t, NULL, false, NULL);
	OUT_LIT(" }");
	if (pparse_match_ch(t, ';')) t = pparse_next(_pc, t);
	if (end && pparse_idx(_pc, t) > pparse_idx(_pc, end)) t = end;
	return t;
}

static inline PParseToken *try_process_stmt_token(PParseToken *t, PParseToken *end, PParseToken **unreachable_tok) {
	PPARSE_CTX();
	/* Match Pass 2 main loop: try_zero_init_decl also handles decl-init /
   * bracket orelse (and auto-static) when PPARSE_F_ZEROINIT is off. Gating on
   * PPARSE_F_ZEROINIT alone sent `int t = get() orelse 0;` inside defer bodies
   * through emit_deferred_orelse, which treated the type keyword as a
   * bare-orelse LHS and emitted invalid `__typeof__(int t)`. */
	if (emit_at_stmt_start) {
		PParseToken *next = try_zero_init_decl(t);
		if (next) return next;
	}
	if (emit_at_stmt_start && pparse_feat(PPARSE_F_ORELSE) && !(t->tag & PPARSE_TT_NON_EXPR_STMT)) {
		PParseToken *next = emit_deferred_orelse(t, end);
		if (next) {
			emit_at_stmt_start = true;
			return next;
		}
	}
	{
		PParseToken *r = try_strip_raw(t);
		if (r) return r;
	}
	if (pparse_feat(PPARSE_F_AUTO_UNREACHABLE) && !(ctrl_state.pending && ctrl_state.parens_just_closed)) {
		PParseToken *nr = pparse_try_detect_noreturn_call(t);
		if (nr && nr != end) *unreachable_tok = nr;
	}
	return NULL;
}

static void emit_deferred_range(PParseToken *start, PParseToken *end) {
	bool saved_stmt_start = emit_at_stmt_start;
	CtrlState saved_ctrl = ctrl_state;
	ctrl_reset();
	emit_at_stmt_start = true;
	emit_statements(start, end, EMIT_DEFER_BODY);
	emit_at_stmt_start = saved_stmt_start;
	ctrl_state = saved_ctrl;
}

// --- Pass 2: Main Transpilation Loop ---

static PRISM_HOT int transpile_tokens(PParseToken *tok, FILE *fp) {
	PRISM_STATE();
	PPARSE_CTX();
	out_fp = fp;
	out_buf_pos = 0;
	out_total_flushed = 0;
	reset_transpiler_state();
	if (pparse_feat(PPARSE_F_FLATTEN)) {
		emit_system_header_diag_push();
		out_char('\n');
	}

	system_includes_reset();
	const char *cc = _ps->extra_compiler ? _ps->extra_compiler : PRISM_DEFAULT_CC;
	is_msvc_cached = cc_is_msvc(cc);
	bool already_has_bchk = pparse_analyze(tok);
	if (!pparse_feat(PPARSE_F_FLATTEN)) {
		collect_system_includes();
		emit_system_includes();
	}

	// MSVC lacks __builtin_expect / __builtin_trap — fall back to __debugbreak +
	// abort. We do NOT #include <stddef.h> / <stdlib.h>: in flatten mode the
	// output is call site. MSVC gets `unsigned __int64` (matches LLP64 size_t on
	// x64).
	if (pparse_feat(PPARSE_F_BOUNDS_CHECK)) {
		if (!already_has_bchk && is_msvc_cached) {
			OUT_LIT("\n"
				"typedef unsigned __int64 __prism_bchk_size_t;\n"
				"void __cdecl abort(void);\n"
				"static __forceinline __prism_bchk_size_t "
				"__prism_bchk(__prism_bchk_size_t "
				"__i, __prism_bchk_size_t __n) {\n"
				"    if (__i >= __n) { __debugbreak(); abort(); }\n"
				"    return __i;\n"
				"}\n");
		} else if (!already_has_bchk) {
			/* C89-safe: no `inline` (an identifier under -std=c89). */
			OUT_LIT("\n"
				"typedef unsigned long long __prism_bchk_size_t;\n"
				"static __prism_bchk_size_t "
				"__prism_bchk(__prism_bchk_size_t __i, __prism_bchk_size_t __n) {\n"
				"    if (__builtin_expect(__i >= __n, 0)) __builtin_trap();\n"
				"    return __i;\n"
				"}\n");
		}
	}

	int next_func_idx = 0;
	int ternary_depth = 0;
	PParseToken *pending_unreachable_tok = NULL;
	bool pending_case_colon = false;
	const uint32_t feat = _pc->features;
#undef pparse_feat
#define pparse_feat(f) (feat & (f))
#ifdef PRISM_DEBUG
	/* Termination watchdog for the Pass 2 walk (see Phase 1 twin). */
	uint64_t p2_wd_steps = 0;
	const uint64_t p2_wd_budget = 256ull * (uint64_t)pparse_token_count + 65536ull;
#endif
	while (tok->kind != PPARSE_TK_EOF) {
#ifdef PRISM_DEBUG
		if (++p2_wd_steps > p2_wd_budget)
			pparse_error_tok(tok,
				  "internal: Pass 2 progress watchdog tripped "
				  "(possible non-termination); please report");
#endif
		/* Precomputed system-include predicate avoids a per-token cold-file lookup. */
		if (!pparse_feat(PPARSE_F_FLATTEN) && (tok->flags & PPARSE_TF_SYS_SKIP)) {
			if (next_func_idx < func_meta_count &&
			    func_meta[next_func_idx].body_open == tok)
				next_func_idx++;
			tok = pparse_next(_pc, tok);
			continue;
		}

		PParseToken *next;
		uint32_t tag = tok->tag;
		if (pparse_match_ch(tok, '?')) ternary_depth++;

#define DISPATCH(handler)                                                                                    \
	{                                                                                                    \
		next = handler(tok);                                                                         \
		if (next) {                                                                                  \
			tok = next;                                                                          \
			continue;                                                                            \
		}                                                                                            \
	}

		if (__builtin_expect(!tag && !emit_at_stmt_start, 1)) {
			if (__builtin_expect((tok->flags & PPARSE_TF_RAW) && !pparse_is_known_typedef(tok), 0))
				goto slow_path;
			if (__builtin_expect(pparse_feat(PPARSE_F_BOUNDS_CHECK) && _ps->raw_block_depth == 0 &&
						 tok->ch0 == '*',
					     0)) {
				PParseToken *bc_da = try_bounds_check_deref_add(tok);
				if (bc_da) {
					tok = bc_da;
					continue;
				}
			}
			track_common_token_state(tok);
			tok = emit_advance(tok);
			continue;
		}
	slow_path:

	{
		PParseToken *next = emit_gnu_label_decl(tok);
		if (next) {
			tok = next;
			continue;
		}
	}

		if (emit_at_stmt_start && !(tag & PPARSE_TT_STRUCTURAL) &&
		    (!ctrl_state.pending || in_for_init() || ctrl_state.parens_just_closed)) {
			/* The parser marks the brace of statement-form `raw { ... }`. */
			if ((tok->flags & PPARSE_TF_RAW)) {
				PParseToken *after = pparse_skip_noise(_pc, pparse_next(_pc, tok));
				if (after && (pparse_ann(after) & P1_RAW_BLOCK)) {
					tok = after;
					tag = tok->tag;
					/* Fall through: structural `{` handled below. */
					goto after_stmt_start_decl;
				}
			}
			next = try_zero_init_decl(tok);
			if (next) {
				tok = next;
				emit_at_stmt_start = true;
				continue;
			}

			check_enum_typedef_defer_shadow(tok);
			if (pparse_feat(PPARSE_F_ORELSE) && _ps->raw_block_depth == 0 && emit_block_depth > 0 &&
			    !in_struct_body() && !(tok->tag & (PPARSE_TT_NON_EXPR_STMT | PPARSE_TT_DEFER))) {
				PParseToken *body = skip_stmt_prefixes(tok);
				PParseToken *orelse_tok = find_bare_orelse(body);
				if (orelse_tok) {
					tok = emit_through(tok, body);

					bool brace_wrap = ctrl_state.pending && ctrl_state.parens_just_closed;
					if (brace_wrap) ctrl_reset();
					PParseToken *next = emit_bare_orelse_impl(tok, NULL, true, brace_wrap);
					if (next) {
						tok = next;
						end_statement_after_semicolon();
						continue;
					}

					tok = emit_orelse_condition_wrap(tok, orelse_tok);
					tok = emit_orelse_action(tok, NULL, false, NULL);
					OUT_LIT(" }");
					continue;
				}
			}
		}
	after_stmt_start_decl:
		emit_at_stmt_start = false;
		if (tag & PPARSE_TT_NORETURN_FN) {
			uint32_t ti = pparse_idx(_pc, tok);
			if (!(ti >= 1 && (pparse_token_pool[ti - 1].tag & PPARSE_TT_MEMBER))) {
				if (pparse_feat(PPARSE_F_DEFER) && has_active_defers() &&
				    !pparse_feat(PPARSE_F_QUIET))
					fprintf(
					    stderr,
					    "%s:%d: warning: '%.*s' referenced with active defers (defers "
					    "will not run if called)\n",
					    pparse_tok_file(tok)->name,
					    pparse_tok_line_no(tok),
					    tok->len,
					    pparse_loc(_pc, tok));
				if (pparse_feat(PPARSE_F_AUTO_UNREACHABLE) && _ps->raw_block_depth == 0 &&
				    emit_block_depth > 0 && !in_ctrl_paren() &&
				    !(ctrl_state.pending && ctrl_state.parens_just_closed)) {
					PParseToken *nr = pparse_try_detect_noreturn_call(tok);
					if (nr) pending_unreachable_tok = nr;
				}
			}
		}

		if (tag) {
			{
				if (_ps->raw_block_depth == 0) {
					PParseToken *n = try_handle_defer_flow_kw(tok);
					if (n) {
						tok = n;
						continue;
					}
				}
			}
			arm_ctrl_pending_from_tag(tok, tag);
			if ((tag & PPARSE_TT_GENERIC) && !in_generic()) {
				tok = emit_generic_open(tok);
				continue;
			}
		} // end if (tag)

		if (tag & (PPARSE_TT_CASE | PPARSE_TT_DEFAULT)) pending_case_colon = true;
		track_generic_token(tok);
		if (tag & PPARSE_TT_SUE) // struct/union/enum body
			DISPATCH(handle_sue_body);
		if (tag & PPARSE_TT_STRUCTURAL) {
			if (pparse_match_ch(tok, '{')) {
				if (emit_block_depth == 0) {
					if (pparse_feat(PPARSE_F_DEFER) && next_func_idx < func_meta_count &&
					    func_meta[next_func_idx].body_open == tok)
						current_func_idx = next_func_idx++;
				}
				tok = handle_open_brace(tok);
				continue;
			}
			if (pparse_match_ch(tok, '}')) {
				tok = handle_close_brace(tok);
				if (emit_block_depth == 0) current_func_idx = -1;
				continue;
			}
			char c = tok->ch0;
			if (c == ';') {
				if (in_ctrl_paren() || in_for_init()) track_ctrl_semicolon();
				else
					end_statement_after_semicolon();
				bool is_unreachable_target = (tok == pending_unreachable_tok);
				tok = emit_advance(tok);
				if (is_unreachable_target) {
					EMIT_UNREACHABLE();
					pending_unreachable_tok = NULL;
				}
				continue;
			}
			if (c == ':' && consume_stmt_colon(
					    &tok, &ternary_depth, &pending_case_colon, COLON_REQUIRE_BLOCK))
				continue;
		}

		if (__builtin_expect(tok->kind == PPARSE_TK_PREP_DIR, 0)) {
			tok = emit_advance(tok);
			emit_at_stmt_start = true;
			continue;
		}

		track_common_token_state(tok);
		/* Bounds before bracket-orelse — same ordering as emit_statements /
		 * walk_balanced. Otherwise `return a[i orelse 0]` lowers the
		 * index ternary but skips __prism_bchk (v1 hook-order bug). */
		if (_ps->raw_block_depth == 0) {
			EMIT_TRY_BOUNDS(tok)
			PParseToken *next = try_orelse_expr_rewrites(tok);
			if (next) {
				tok = next;
				continue;
			}
		}
		tok = emit_advance(tok);
	}

	if (pparse_feat(PPARSE_F_FLATTEN)) {
		out_char('\n');
		emit_system_header_diag_pop();
	}
#undef pparse_feat
#define pparse_feat(f) (_pc->features & (f))

	out_close();
	free_source_defines();
	pparse_tokenizer_teardown(false);
	return 1;
}

static PParseToken *preprocess_and_tokenize(char *input_file, double *pp_ms, double *tok_ms) {
	double t0 = prism_now_ms();
	char *pp_buf = preprocess_with_cc(input_file);
	double t1 = prism_now_ms();
	if (pp_ms) *pp_ms = t1 - t0;
	if (!pp_buf) {
		fprintf(stderr, "Preprocessing failed for: %s\n", input_file);
		return NULL;
	}
	double t2 = prism_now_ms();
	PParseToken *tok = pparse_tokenize_buffer(input_file, pp_buf);
	double t3 = prism_now_ms();
	if (tok_ms) *tok_ms = t3 - t2;
	if (!tok) {
		fprintf(stderr, "Failed to pparse_tokenize preprocessed output\n");
		pparse_tokenizer_teardown(false);
	}
	return tok;
}

static int transpile_to_fp(char *input_file, FILE *fp) {
	pparse_ensure_keyword_cache();
	double t0 = prism_now_ms();
	double pp_ms = 0.0, tok_ms = 0.0;
	PParseToken *tok = preprocess_and_tokenize(input_file, &pp_ms, &tok_ms);
	if (!tok) {
		fclose(fp);
		return 0;
	}

	double t1 = prism_now_ms();
	int ok = transpile_tokens(tok, fp);
	double t2 = prism_now_ms();
	if (prism_profile) {
		fprintf(stderr,
			"[prism-prof] file=%s preprocess=%.3fms pparse_tokenize=%.3fms "
			"transpile=%.3fms total=%.3fms\n",
			input_file,
			pp_ms,
			tok_ms,
			(t2 - t1),
			(t2 - t0));
	}
	return ok;
}

static int verify_transpiled_output(char *orig_input, char *out1_path);

static int transpile(char *input_file, char *output_file) {
	FILE *fp = fopen(output_file, "w");
	if (!fp) return 0;
	int ok = transpile_to_fp(input_file, fp);
	if (ok && prism_verify_mode && !prism_in_verify && strcmp(output_file, "/dev/stdout") != 0)
		ok = verify_transpiled_output(input_file, output_file);
	return ok;
}

static int transpile_to_stdout(char *input_file) {
#ifdef _WIN32
	char temp[PATH_MAX];
	int fd = make_temp_file_registered(temp, sizeof(temp), NULL, 0, input_file);
	if (fd < 0) return 0;
	FILE *wfp = fdopen(fd, "w");
	if (!wfp) {
		close(fd);
		return 0;
	}
	if (!transpile_to_fp(input_file, wfp)) {
		remove(temp);
		return 0;
	}
	FILE *f = fopen(temp, "r");
	if (f) {
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, stdout);
		fclose(f);
	}
	remove(temp);
	return 1;
#else
	return transpile(input_file, "/dev/stdout");
#endif
}

PRISM_API void prism_free(PrismResult *r) {
	free(r->output);
	free(r->error_msg);
	r->output = r->error_msg = NULL;
}

PRISM_API void prism_reset(void) {
	PRISM_STATE();
	pparse_reset();
	free_source_defines();
	pparse_tokenizer_teardown(false);
	emit_scope_depth = emit_block_depth = 0;
	_ps->aggregate_member_nest = 0;
	system_includes_reset();
	in_defer_emit = false;
	ctrl_reset();
	ctrl_save_depth = 0;
	current_func_idx = -1;
	if (out_fp) {
		out_flush();
		fclose(out_fp);
		out_fp = NULL;
	}
}

/* Count whole-word defer/orelse identifiers in one CODE pass; string literals,
 * char literals, and comments are skipped. The leak
 * check asks "did an operator-position keyword survive to the backend"; the
 * transpiler's own pparse_error-message strings ("'orelse' cannot be used …") and a
 * `{"defer", …}` keyword-table entry are not operator keywords and their count
 * is not guaranteed stable across re-preprocessing/header-flattening on every
 * platform (this counter previously counted them, producing a spurious Linux
 * "leak" on the self-referential test harness).  A single scanner tracks
 * string/char/comment state with escape handling. */
typedef struct { long orelse, defer; } VerifyKwCounts;

static VerifyKwCounts verify_kw_counts(const char *s) {
	VerifyKwCounts n = {0};
	for (const char *p = s; *p;) {
		if (*p == '"' || *p == '\'') {
			char q = *p++;
			while (*p && *p != q) {
				if (*p == '\\' && p[1]) p += 2;
				else
					p++;
			}
			if (*p) p++;
			continue;
		}
		if (p[0] == '/' && p[1] == '/') {
			while (*p && *p != '\n') p++;
			continue;
		}
		if (p[0] == '/' && p[1] == '*') {
			p += 2;
			while (*p && !(p[0] == '*' && p[1] == '/')) p++;
			if (*p) p += 2;
			continue;
		}
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9') || *p == '_') {
			const char *start = p++;
			while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			       (*p >= '0' && *p <= '9') || *p == '_')
				p++;
			size_t len = (size_t)(p - start);
			if (len == 6 && memcmp(start, "orelse", 6) == 0) n.orelse++;
			else if (len == 5 && memcmp(start, "defer", 5) == 0)
				n.defer++;
			continue;
		}
		p++;
	}
	return n;
}

/* --prism-verify: per-compile translation-validation certificate for the
 * defer/orelse safety theorem.
 *
 * After emitting output #1 for `orig_input`, the ENTIRE pipeline runs on
 * output #1 a second time (preprocess, pparse_tokenize, all Phase 1 analyses + CFG
 * verification, Pass 2).  The certificate requires:
 *   (1) the second pass SUCCEEDS — output #1 is valid C that survives every
 *       Phase-1 constraint and the CFG verifier as plain C.  A leaked
 *       operator-position keyword that produces invalid C, or any emitted
 *       construct that trips a Phase-1 gate, fails here.
 *   (2) NO operator keyword leaked — the count of whole-word `orelse` /
 *       `defer` tokens is IDENTICAL in output #1 and output #2.  Soundness:
 *       prism only ever REMOVES operator-position keywords (it lowers them);
 *       it never introduces one.  So count(#2) <= count(#1) always, and a
 *       strict decrease means pass 2 lowered a keyword that pass 1 left
 *       behind — i.e. output #1 leaked an operator `orelse`/`defer` to the
 *       backend.  Equal counts prove every surviving `orelse`/`defer` word
 *       is a stable ordinary identifier (a user variable/typedef/label),
 *       exactly SPEC's "when disabled the keyword reverts to an identifier".
 *
 * Why not byte-equality: `prism(prism(x))` is deliberately NOT byte-pparse_equal to
 * `prism(x)` under header-flattening — pass 2 re-wraps the diagnostic-pragma
 * preamble, renumbers `#line`, and relocates the one-time `__prism_bchk`
 * runtime helper.  That scaffolding asymmetry is unrelated to defer/orelse.
 * Byte-level lowering-idempotence of defer/orelse (and bounds) IS certified,
 * on controlled inputs where scaffolding is stable, by the in-process
 * contexts/insertion fixed-point oracles (test.contexts.c / test.insertion.c).
 *
 * The second pass disables the additive safety transforms (zero-init,
 * auto-unreachable, auto-static) — each re-applies to already-lowered code
 * with a benign asymmetry (raw-stripped re-zeroing, doubled unreachable
 * marker, reshaped-decl tracking) — and downgrades safety diagnostics to
 * warnings, because Prism's own lowering can emit CFG shapes its strict
 * checker rejects (e.g. `int x = v orelse goto L, y = 10;` → a goto
 * textually before an initialized decl); the user's ORIGINAL already passed
 * the strict check in pass 1.  defer + orelse stay ON so a leaked operator
 * keyword is lowered on pass 2 and detected by the count check.            */
static int verify_transpiled_output(char *orig_input, char *out1_path) {
	PRISM_STATE();
	PPARSE_CTX();
	prism_in_verify = true;
	const char **saved_dep = _ps->dep_flags;
	int saved_dep_n = _ps->dep_flags_count;
	_ps->dep_flags = NULL; /* never regenerate .d files for the verify pass */
	_ps->dep_flags_count = 0;
	prism_reset();

	char tmp2[PATH_MAX];
	int ok = 0;
	char diag[256] = "re-transpile of emitted C failed (output is not valid re-parseable C)";
	int fd = make_temp_file_registered(tmp2, sizeof(tmp2), NULL, 2, out1_path);
	if (fd >= 0) {
		close(fd);
		uint32_t saved_features = _pc->features;
		_pc->features &= ~(uint32_t)(PPARSE_F_ZEROINIT | PPARSE_F_AUTO_UNREACHABLE | PPARSE_F_AUTO_STATIC);
		_pc->features |= PPARSE_F_WARN_SAFETY;
		int retrans_ok = transpile(out1_path, tmp2);
		_pc->features = saved_features;
		if (retrans_ok) {
			char *o1 = read_file_bytes(out1_path, 1);
			char *o2 = read_file_bytes(tmp2, 1);
			if (o1 && o2) {
				VerifyKwCounts k1 = verify_kw_counts(o1), k2 = verify_kw_counts(o2);
				if (k2.orelse < k1.orelse || k2.defer < k1.defer) {
					snprintf(diag, sizeof(diag),
						 "operator keyword leaked to output: orelse %ld->%ld, "
						 "defer %ld->%ld (re-transpile lowered a keyword pass 1 "
						 "left behind)",
						 k1.orelse, k2.orelse, k1.defer, k2.defer);
				} else {
					ok = 1;
				}
			} else {
				snprintf(diag, sizeof(diag), "cannot reopen outputs for comparison");
			}
			free(o1);
			free(o2);
		}
		remove(tmp2);
	}

	prism_reset();
	_ps->dep_flags = saved_dep;
	_ps->dep_flags_count = saved_dep_n;
	prism_in_verify = false;

	if (!ok) {
		fprintf(stderr,
			"prism: --prism-verify FAILED for %s\n"
			"  %s\n"
			"  this indicates a transform defect; please report it.\n",
			orig_input, diag);
		return 0;
	}
	return 1;
}

PRISM_API void prism_thread_cleanup(void) {
	if (!pparse_ctx) return;
	if (out_fp) {
		out_flush();
		fclose(out_fp);
		out_fp = NULL;
	}
	out_buf_pos = 0;
	system_include_list = NULL;
	system_include_capacity = 0;
	last_emitted = NULL;
	use_linemarkers = false;
	defer_count = 0;
	defer_shadow_count = 0;
	free(scope_stack);
	scope_stack = NULL;
	scope_stack_cap = 0;
	free(defer_stack);
	defer_stack = NULL;
	defer_stack_cap = 0;
	free(defer_shadows);
	defer_shadows = NULL;
	defer_shadow_cap = 0;
	for (int i = 0; i < pp_define_bufs_cap; i++) free(pp_define_bufs[i]);
	free(pp_define_bufs);
	pp_define_bufs = NULL;
	pp_define_bufs_cap = 0;
	memset(&ctrl_state, 0, sizeof(ctrl_state));
	free(ctrl_save_stack);
	ctrl_save_stack = NULL;
	ctrl_save_cap = 0;
	ctrl_save_depth = 0;
	in_defer_emit = false;
	current_func_idx = -1;
	pparse_ctx_destroy();
}

static void apply_features(PrismFeatures features) {
	PRISM_STATE();
	PPARSE_CTX();
	_pc->features = features_to_bits(features);
	_ps->extra_compiler = features.compiler;
	_ps->extra_include_paths = features.include_paths;
	_ps->extra_include_count = features.include_count;
	_ps->extra_defines = features.defines;
	_ps->extra_define_count = features.define_count;
	_ps->extra_compiler_flags = features.compiler_flags;
	_ps->extra_compiler_flags_count = features.compiler_flags_count;
	_ps->extra_force_includes = features.force_includes;
	_ps->extra_force_include_count = features.force_include_count;
}

#ifdef PRISM_LIB_MODE
static void error_recovery_init(void) {
	PRISM_STATE();
	PPARSE_CTX();
	_pc->error_msg[0] = '\0';
	_pc->error_line = 0;
	_ps->error_col = 0;
	_pc->error_jmp_set = true;
}

static PrismResult error_recovery_result(void) {
	PRISM_STATE();
	PPARSE_CTX();
	_pc->error_jmp_set = false;
	PrismResult r = {.status = PRISM_ERR_SYNTAX,
			 .error_msg = strdup(_pc->error_msg[0] ? _pc->error_msg : "Unknown pparse_error"),
			 .error_line = _pc->error_line,
			 .error_col = _ps->error_col};
	// IMPORTANT: fclose must precede free — POSIX open_memstream only
	if (out_fp) {
		fclose(out_fp);
		out_fp = NULL;
	}
	if (_ps->active_membuf) {
		free(_ps->active_membuf);
		_ps->active_membuf = NULL;
	}
	prism_reset();
	return r;
}
#endif

static PrismResult transpile_to_result(PParseToken *tok) {
	PRISM_STATE();
	PrismResult result = {0};
	size_t memlen = 0;
#ifdef PRISM_LIB_MODE
	_ps->active_membuf = NULL;
	FILE *fp = open_memstream(&_ps->active_membuf, &memlen);
#else
	char *membuf = NULL;
	FILE *fp = open_memstream(&membuf, &memlen);
#endif
	if (!fp) {
		result.status = PRISM_ERR_IO;
		result.error_msg = strdup("open_memstream failed");
		prism_reset();
		return result;
	}
	if (transpile_tokens(tok, fp)) {
#ifdef PRISM_LIB_MODE
		result.output = _ps->active_membuf;
#else
		result.output = membuf;
#endif
		result.output_len = memlen;
		result.status = PRISM_OK;
	} else {
#ifdef PRISM_LIB_MODE
		free(_ps->active_membuf);
#else
		free(membuf);
#endif
		result.status = PRISM_ERR_SYNTAX;
		result.error_msg = strdup("Transpilation failed");
	}
#ifdef PRISM_LIB_MODE
	_ps->active_membuf = NULL;
#endif
	return result;
}

PRISM_API PrismResult prism_transpile_file(const char *input_file, PrismFeatures features) {
	pparse_ctx_init();
	/* After pparse_ctx_init(): caching before the context exists captures NULL. */
	PPARSE_CTX();
	PrismResult result = {0};

#ifdef PRISM_LIB_MODE
	error_recovery_init();
	if (setjmp(_pc->error_jmp) != 0) return error_recovery_result();
#endif

	apply_features(features);
	pparse_ensure_keyword_cache();
	PParseToken *tok;
	char *pp_buf = preprocess_with_cc((char *)input_file);
	if (!pp_buf) {
		result.status = PRISM_ERR_IO;
		result.error_msg = strdup("Preprocessing failed");
		goto cleanup;
	}

	tok = pparse_tokenize_buffer((char *)input_file, pp_buf);
	if (!tok) {
		result.status = PRISM_ERR_SYNTAX;
		result.error_msg = strdup("Failed to pparse_tokenize");
		pparse_tokenizer_teardown(false);
		goto cleanup;
	}

	result = transpile_to_result(tok);

cleanup:
#ifdef PRISM_LIB_MODE
	_pc->error_jmp_set = false;
#endif
	return result;
}

#ifdef PRISM_LIB_MODE
PRISM_API
PrismResult prism_transpile_source(const char *source, const char *filename, PrismFeatures features) {
	pparse_ctx_init();
	PPARSE_CTX();
	PrismResult result = {0};
	if (!source) {
		result.status = PRISM_ERR_IO;
		result.error_msg = strdup("source is NULL");
		return result;
	}

	const char *fname = filename ? filename : "<source>";
	error_recovery_init();
	if (setjmp(_pc->error_jmp) != 0) return error_recovery_result();
	apply_features(features);
	pparse_ensure_keyword_cache();
	PParseToken *tok;
	char *buf;
	size_t src_len = strlen(source);
	buf = malloc(src_len + 8);
	if (!buf) {
		result.status = PRISM_ERR_IO;
		result.error_msg = strdup("Out of memory");
		goto src_cleanup;
	}
	memcpy(buf, source, src_len);
	memset(buf + src_len, 0, 8);
	tok = pparse_tokenize_buffer((char *)fname, buf);
	if (!tok) {
		result.status = PRISM_ERR_SYNTAX;
		result.error_msg = strdup("Failed to pparse_tokenize");
		pparse_tokenizer_teardown(false);
		goto src_cleanup;
	}

	result = transpile_to_result(tok);

src_cleanup:
	_pc->error_jmp_set = false;
	return result;
}
#endif // PRISM_LIB_MODE

/* Grow *(arr) with sizeof(*arr) — portable, no __typeof__ (MSVC shim was
 * hardcoded to const char * and would under-allocate for any other element). */
#define CLI_PUSH(arr, cnt, cap, item)                                                                        \
	do {                                                                                                 \
		pparse_VEC_ENSURE_REALLOC((arr), (cnt) + 1, (cap), 16);                                             \
		(arr)[(cnt)++] = (item);                                                                     \
	} while (0)

static inline bool has_ext(const char *f, const char *ext) {
	size_t fl = strlen(f), el = strlen(ext);
	return fl >= el && !strcmp(f + fl - el, ext);
}

/* Args that must not be fed to `cc -E` of a Prism .c/.i unit: other TUs and
 * objects. Without this, `prism a.c b.cpp` preprocesses both and Prism parses
 * C++ as C (misccompile / bogus rejects). */
static bool is_pp_skip_input_arg(const char *f) {
	if (!f || !*f || f[0] == '-') return false;
	return has_ext(f, ".c") || has_ext(f, ".i") || has_ext(f, ".cpp") || has_ext(f, ".cc") ||
	       has_ext(f, ".cxx") || has_ext(f, ".C") || has_ext(f, ".m") || has_ext(f, ".mm") ||
	       has_ext(f, ".s") || has_ext(f, ".S") || has_ext(f, ".o") || has_ext(f, ".obj") ||
	       has_ext(f, ".a") || has_ext(f, ".lib") || has_ext(f, ".so") || has_ext(f, ".dylib") ||
	       has_ext(f, ".dll") || has_ext(f, ".exe");
}

static bool str_startswith(const char *s, const char *prefix) {
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool cc_flag_takes_arg(const char *a) {
	if (a[0] != '-' || !a[1]) return false;
	if (!a[2]) {
		switch (a[1]) {
		case 'c':
		case 'E':
		case 'S':
		case 'v':
		case 'w':
		case 's':
		case 'g':
		case 'H':
		case 'P':
		case 'p':
		case 'r':
		case 'C':
		case 'h':
		case 'Q':
		case 'O':
		case 'W':
		case 'M':
		case 'd': return false;
		default: return true;
		}
	}
	return !strcmp(a, "-include") || !strcmp(a, "-isystem") || !strcmp(a, "-idirafter") ||
	       !strcmp(a, "-imacros") || !strcmp(a, "-iquote") || !strcmp(a, "-iprefix") ||
	       !strcmp(a, "-iwithprefix") || !strcmp(a, "-iwithprefixbefore") || !strcmp(a, "-Xlinker") ||
	       !strcmp(a, "-Xpreprocessor") || !strcmp(a, "-Xassembler") || !strcmp(a, "-target") ||
	       !strcmp(a, "-arch");
}

static int dep_flag_kind(const char *a) {
	if (a[1] == 'M') {
		if (!strcmp(a, "-MD") || !strcmp(a, "-MMD") || !strcmp(a, "-MP")) return 1;
		if (!strcmp(a, "-MF") || !strcmp(a, "-MT") || !strcmp(a, "-MQ")) return 2;
	}
	if (a[1] == 'W' && a[2] == 'p' && a[3] == ',') {
		const char *v = a + 4;
		if (strstr(v, "-MD") || strstr(v, "-MMD") || strstr(v, "-MF") || strstr(v, "-MT") ||
		    strstr(v, "-MQ") || strstr(v, "-MP"))
			return 1;
	}
	return 0;
}

/* GCC-compatible @file response-file expansion. Without this, `prism @args.rsp`
 * where the rsp lists `.c` sources never sees those TUs — the backend gets raw
 * Prism syntax and misscompiles. Nesting depth is capped like the GCC driver. */
#define RSP_MAX_DEPTH 32

static bool rsp_push_dup(char ***out, int *count, int *cap, char ***owned, int *owned_count, int *owned_cap,
			 const char *s) {
	char *dup = strdup(s);
	if (!dup) return false;
	pparse_VEC_ENSURE_REALLOC(*owned, *owned_count + 1, *owned_cap, 16);
	(*owned)[(*owned_count)++] = dup;
	pparse_VEC_ENSURE_REALLOC(*out, *count + 1, *cap, 16);
	(*out)[(*count)++] = dup;
	return true;
}

static int rsp_expand_file(const char *path,
			   char ***out,
			   int *count,
			   int *cap,
			   char ***owned,
			   int *owned_count,
			   int *owned_cap,
			   int depth);

static int rsp_tokenize_buf(const char *text,
			    char ***out,
			    int *count,
			    int *cap,
			    char ***owned,
			    int *owned_count,
			    int *owned_cap,
			    int depth) {
	const char *p = text;
	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
		if (!*p) break;
		char *tokbuf = NULL;
		size_t n = 0, tcap = 0;
		/* libiberty buildargv semantics: backslash escapes the next char in
		 * every state; single/double quotes toggle anywhere in the token
		 * (`-DMSG="a b"` is ONE arg); a trailing lone backslash is dropped.
		 * On Windows a backslash is a PATH SEPARATOR, not an escape — only a
		 * backslash immediately before `"` is special (MS convention), so
		 * paths like C:\dir\app survive verbatim in a response file. */
		bool squote = false, dquote = false, bsquote = false;
		while (*p) {
			char c = *p;
			if (!bsquote && !squote && !dquote &&
			    (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'))
				break;
			bool emit = false;
			bool is_escape = c == '\\';
#ifdef _WIN32
			is_escape = is_escape && p[1] == '"';
#endif
			if (bsquote) {
				bsquote = false;
				emit = true;
			} else if (is_escape) {
				bsquote = true;
			} else if (squote) {
				if (c == '\'') squote = false;
				else
					emit = true;
			} else if (dquote) {
				if (c == '"') dquote = false;
				else
					emit = true;
			} else if (c == '\'') {
				squote = true;
			} else if (c == '"') {
				dquote = true;
			} else {
				emit = true;
			}
			if (emit) {
				if (n + 1 >= tcap) {
					tcap = tcap ? tcap * 2 : 64;
					char *nt = realloc(tokbuf, tcap);
					if (!nt) {
						free(tokbuf);
						return -1;
					}
					tokbuf = nt;
				}
				tokbuf[n++] = c;
			}
			p++;
		}
		if (!tokbuf) {
			tokbuf = strdup("");
			if (!tokbuf) return -1;
		} else {
			tokbuf[n] = '\0';
		}
		int nested_rc = 0;
		if (tokbuf[0] == '@' && tokbuf[1]) {
			nested_rc = rsp_expand_file(
			    tokbuf + 1, out, count, cap, owned, owned_count, owned_cap, depth + 1);
			/* Unreadable nested @file: GCC keeps the arg literally. */
			if (nested_rc > 0)
				nested_rc =
				    rsp_push_dup(out, count, cap, owned, owned_count, owned_cap, tokbuf)
					? 0
					: -1;
			free(tokbuf);
			if (nested_rc < 0) return -1;
		} else {
			bool ok = rsp_push_dup(out, count, cap, owned, owned_count, owned_cap, tokbuf);
			free(tokbuf);
			if (!ok) return -1;
		}
	}
	return 0;
}

static int rsp_expand_file(const char *path,
			   char ***out,
			   int *count,
			   int *cap,
			   char ***owned,
			   int *owned_count,
			   int *owned_cap,
			   int depth) {
	if (depth > RSP_MAX_DEPTH) {
		fprintf(stderr, "pparse_error: response file nesting too deep: %s\n", path);
		return -1;
	}
	/* GCC: an @file that does not exist or cannot be read is treated
	 * literally, not removed — return 1 so the caller keeps the raw arg. */
	FILE *f = fopen(path, "rb");
	if (!f) return 1;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return 1;
	}
	long sz = ftell(f);
	if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return 1;
	}
	char *buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return -1;
	}
	size_t got = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[got] = '\0';
	int rc = rsp_tokenize_buf(buf, out, count, cap, owned, owned_count, owned_cap, depth);
	free(buf);
	return rc;
}

/* Expand @file args into a flat argv. Caller owns *owned_out (each string + array).
 * *out_argv holds pointers into *owned_out (and must not outlive it). */
static char **cli_expand_response_files(int argc,
					char **argv,
					int *out_argc,
					char ***owned_out,
					int *owned_count_out) {
	char **out = NULL;
	char **owned = NULL;
	int count = 0, cap = 0, owned_count = 0, owned_cap = 0;
	for (int i = 0; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] == '@' && a[1] && i > 0) {
			int rc = rsp_expand_file(a + 1, &out, &count, &cap, &owned, &owned_count, &owned_cap, 0);
			/* rc > 0: unreadable @file — GCC keeps the arg literally. */
			if (rc > 0)
				rc = rsp_push_dup(&out, &count, &cap, &owned, &owned_count, &owned_cap, a) ? 0
													    : -1;
			if (rc < 0) {
				for (int j = 0; j < owned_count; j++) free(owned[j]);
				free(owned);
				free(out);
				*out_argc = 0;
				*owned_out = NULL;
				*owned_count_out = 0;
				return NULL;
			}
		} else if (!rsp_push_dup(&out, &count, &cap, &owned, &owned_count, &owned_cap, a)) {
			for (int j = 0; j < owned_count; j++) free(owned[j]);
			free(owned);
			free(out);
			*out_argc = 0;
			*owned_out = NULL;
			*owned_count_out = 0;
			return NULL;
		}
	}
	pparse_VEC_ENSURE_REALLOC(out, count + 1, cap, 16);
	out[count] = NULL;
	*out_argc = count;
	*owned_out = owned;
	*owned_count_out = owned_count;
	return out;
}

static bool prism_handles_x_lang(const char *lang) {
	if (!lang || !*lang) return false;
	return !strcmp(lang, "c") || !strcmp(lang, "cpp-output") || !strcmp(lang, "c-header");
}

static Cli cli_parse(int argc, char **argv) {
	Cli cli = {.features = prism_defaults(), .source_x_arg_idx = -1};
	char **owned = NULL;
	int owned_count = 0;
	int eargc = 0;
	char **eargv = cli_expand_response_files(argc, argv, &eargc, &owned, &owned_count);
	if (!eargv) {
		fprintf(stderr, "pparse_error: failed to expand response files\n");
		exit(1);
	}
	cli.rsp_owned = owned;
	cli.rsp_owned_count = owned_count;
	cli.rsp_argv = eargv;
	argc = eargc;
	argv = eargv;
	/* GCC: `-x LANG` applies to subsequent inputs until the next `-x`. */
	const char *cur_x_lang = NULL;
	int last_x_arg_idx = -1;
	for (int i = 1; i < argc; i++) {
		char *a = argv[i];
		/* check mode: first arg after `check` is the analyzer; everything
		 * after that belongs to the tool verbatim (including `--` and `-`
		 * flags — the tool owns its namespace). Source args are recorded
		 * in both lists and substituted with transpile temps at spawn. */
		if (cli.mode == CLI_CHECK) {
			if (!cli.check_tool) {
				cli.check_tool = a;
				continue;
			}
			if (a[0] != '-' && (has_ext(a, ".c") || has_ext(a, ".i") ||
					    prism_handles_x_lang(cur_x_lang)))
				CLI_PUSH(cli.sources, cli.source_count, cli.source_cap, a);
			CLI_PUSH(cli.check_args, cli.check_arg_count, cli.check_arg_cap, a);
			continue;
		}
		if (a[0] == '-' && a[1] == '-' && !a[2]) {
			for (int j = i + 1; j < argc; j++)
				CLI_PUSH(cli.prog_args, cli.prog_arg_count, cli.prog_arg_cap, argv[j]);
			break;
		}

#ifdef _WIN32
		if (a[0] != '-' && a[0] != '/') {
#else
		if (a[0] != '-' || !a[1]) {
#endif
			if (!strcmp(a, "run")) {
				cli.mode = CLI_RUN;
				continue;
			}
			if (!strcmp(a, "transpile")) {
				cli.mode = CLI_EMIT;
				continue;
			}
			if (!strcmp(a, "install")) {
				cli.mode = CLI_INSTALL;
				continue;
			}
			if (!strcmp(a, "check")) {
				cli.mode = CLI_CHECK;
				continue;
			}
			/* GCC/Clang: lone `-` means read the TU from stdin. */
			if (!strcmp(a, "-")) {
				if (cli.source_count == 0) {
					cli.source_x_lang = cur_x_lang;
					cli.source_x_arg_idx = last_x_arg_idx;
				}
				CLI_PUSH(cli.sources, cli.source_count, cli.source_cap, a);
				continue;
			}
			/* MSVC-style /c /Fe /Fo — honor on all hosts so they are
			 * not mistaken for input paths (`cc -E /c` → no such file). */
			if (a[0] == '/') {
				if (strcmp(a, "/c") == 0) {
					cli.compile_only = true;
					continue;
				}
				if (strncmp(a, "/Fe:", 4) == 0) {
					cli.output = a + 4;
					continue;
				}
				if (strncmp(a, "/Fe", 3) == 0 && a[3]) {
					cli.output = a + 3;
					continue;
				}
				if (strcmp(a, "/Fe") == 0 && i + 1 < argc) {
					cli.output = argv[++i];
					continue;
				}
				if (strncmp(a, "/Fo:", 4) == 0) {
					cli.output = a + 4;
					cli.compile_only = true;
					continue;
				}
				if (strncmp(a, "/Fo", 3) == 0 && a[3]) {
					cli.output = a + 3;
					cli.compile_only = true;
					continue;
				}
				if (strcmp(a, "/Fo") == 0 && i + 1 < argc) {
					cli.output = argv[++i];
					cli.compile_only = true;
					continue;
				}
			}
			/* `.c`/`.i`, or extensionless under `-x c` / `cpp-output`. */
			if (!cli.passthrough &&
			    (has_ext(a, ".c") || has_ext(a, ".i") || prism_handles_x_lang(cur_x_lang))) {
				if (cli.source_count == 0) {
					cli.source_x_lang = cur_x_lang;
					cli.source_x_arg_idx = last_x_arg_idx;
				}
				CLI_PUSH(cli.sources, cli.source_count, cli.source_cap, a);
				continue;
			}
			CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
			continue;
		}

#ifdef _WIN32
		// -- Remaining MSVC-style flags (start with /) — forwarded to cl --
		if (a[0] == '/') {
			CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
			continue;
		}
#endif

		if (a[1] == 'o') {
			cli.output = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
			continue;
		}

		if (a[1] == '-') {
			if (!strcmp(a, "--help")) {
				cli.action = CLI_ACT_HELP;
				return cli;
			}
			if (!strcmp(a, "--version")) {
				cli.action = CLI_ACT_VERSION;
				return cli;
			}
			if (str_startswith(a, "--prism-cc=")) {
				cli.cc = a + 11;
				continue;
			}
			if (!strcmp(a, "--prism-verbose")) {
				cli.verbose = true;
				continue;
			}
			if (!strcmp(a, "--prism-prof")) {
				cli.profile = true;
				continue;
			}
			/* Cache maintenance runs immediately and exits: there is no
			 * translation unit involved and nothing later in argv matters. */
			if (!strcmp(a, "--prism-cache-clear")) exit(pp_cache_clear());
			if (!strcmp(a, "--prism-cache-info")) exit(pp_cache_info());
			if (!strcmp(a, "--prism-verify")) {
				cli.verify = true;
				continue;
			}
			if (str_startswith(a, "--prism-emit=")) {
				cli.mode = CLI_EMIT;
				cli.output = a + 13;
				continue;
			}
			if (!strcmp(a, "--prism-emit")) {
				cli.mode = CLI_EMIT;
				continue;
			}
		} else if (a[1] == 'h' && !a[2]) {
			cli.action = CLI_ACT_HELP;
			return cli;
		} else if (a[1] == 'c' && !a[2]) {
			cli.compile_only = true;
		} else if (a[1] == 'S' && !a[2]) {
			cli.compile_only = true;
			cli.assemble_only = true;
		} else if (a[1] == 'E' && !a[2]) {
			cli.passthrough = true;
		} else if (!strcmp(a, "-M") || !strcmp(a, "-MM")) {
			/* Like -E: preprocess-only. Must not transpile via stdin `-`
			 * (backend would emit `-.o:` as the dep target). */
			cli.passthrough = true;
		} else if (a[1] == 'f') {
			/* Both polarities of every feature flag are accepted. GCC's
			 * convention is that -fno-X implies -fX exists, and a build that
			 * inherits -fno-zeroinit from a parent needs some way to turn it
			 * back on for one target. Through 1.1.5 only -fbounds-check and
			 * -fflatten-headers had a positive form; -fzeroinit, -fdefer,
			 * -forelse, -fauto-static and the rest fell through to the C
			 * compiler, which rejected them with "unrecognized command-line
			 * option" naming a flag the user reasonably believed was Prism's.
			 *
			 * Matching is exact after the optional "no-", so unrelated
			 * compiler flags (-fno-strict-aliasing, -fdefer-pop) still fall
			 * through to CC untouched. Note the two inversions: `safety`
			 * drives warn_safety, and `link-pragma` drives no_link_pragma, so
			 * both store the complement. */
			const char *fb = a + 2;
			bool on = true;
			if (!strncmp(fb, "no-", 3)) {
				on = false;
				fb += 3;
			}
			if (!strcmp(fb, "defer")) {
				cli.features.defer = on;
				continue;
			}
			if (!strcmp(fb, "zeroinit")) {
				cli.features.zeroinit = on;
				continue;
			}
			if (!strcmp(fb, "orelse")) {
				cli.features.orelse = on;
				continue;
			}
			if (!strcmp(fb, "line-directives")) {
				cli.features.line_directives = on;
				continue;
			}
			if (!strcmp(fb, "safety")) {
				cli.features.warn_safety = !on;
				continue;
			}
			if (!strcmp(fb, "flatten-headers")) {
				cli.features.flatten_headers = on;
				continue;
			}
			if (!strcmp(fb, "auto-unreachable")) {
				cli.features.auto_unreachable = on;
				continue;
			}
			if (!strcmp(fb, "auto-static")) {
				cli.features.auto_static = on;
				continue;
			}
			if (!strcmp(fb, "bounds-check")) {
				cli.features.bounds_check = on;
				continue;
			}
			if (!strcmp(fb, "link-pragma")) {
				cli.no_link_pragma = !on;
				continue;
			}
		} else {
			int dk = dep_flag_kind(a);
			if (dk) {
				CLI_PUSH(cli.dep_args, cli.dep_arg_count, cli.dep_arg_cap, a);
				if (dk == 2 && i + 1 < argc)
					CLI_PUSH(cli.dep_args, cli.dep_arg_count, cli.dep_arg_cap, argv[++i]);
				continue;
			}
		}

		/* Track `-x LANG` / `-xLANG` for source classification + pipe lang. */
		if (!strcmp(a, "-x") && i + 1 < argc) {
			last_x_arg_idx = cli.cc_arg_count;
			CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
			CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, argv[++i]);
			cur_x_lang = cli.cc_args[cli.cc_arg_count - 1];
			continue;
		}
		if (a[1] == 'x' && a[2] && a[2] != '-') {
			last_x_arg_idx = cli.cc_arg_count;
			CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
			cur_x_lang = a + 2;
			continue;
		}

		CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
		if (i + 1 < argc && cc_flag_takes_arg(a))
			CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, argv[++i]);
	}

	/* eargv / rsp_owned stay owned by Cli until cli_free — do not free here. */
	return cli;
}

static void cli_free(Cli *cli) {
	free(cli->sources);
	free(cli->cc_args);
	free(cli->dep_args);
	free(cli->prog_args);
	if (cli->rsp_owned) {
		for (int i = 0; i < cli->rsp_owned_count; i++) free(cli->rsp_owned[i]);
		free(cli->rsp_owned);
	}
	free(cli->rsp_argv);
	cli->rsp_argv = NULL;
	free(cli->check_args);
	cli->check_args = NULL;
	cli->check_arg_count = 0;
	cli->sources = NULL;
	cli->cc_args = NULL;
	cli->dep_args = NULL;
	cli->prog_args = NULL;
	cli->rsp_owned = NULL;
	cli->rsp_owned_count = 0;
}

#ifndef PRISM_LIB_MODE

static int transpile_and_compile(char *input_file, char **compile_argv, bool verbose) {
	if (verbose) {
		fprintf(stderr, "[prism] ");
		for (int i = 0; compile_argv[i]; i++) fprintf(stderr, "%s ", compile_argv[i]);
		fprintf(stderr, "\n");
	}

	/* --prism-verify: the fast path streams transpiled C straight into the
	 * backend's stdin, leaving nothing to verify.  Under verification we
	 * materialize the output first, check the re-transpilation fixed
	 * point, then feed the verified bytes to the backend. */
	if (prism_verify_mode && !prism_in_verify) {
		char vtmp[PATH_MAX];
		int vfd = make_temp_file_registered(vtmp, sizeof(vtmp), NULL, 2, input_file);
		if (vfd < 0) return -1;
		FILE *vfp = fdopen(vfd, "w");
		if (!vfp) {
			close(vfd);
			remove(vtmp);
			return -1;
		}
		if (!transpile_to_fp(input_file, vfp)) {
			remove(vtmp);
			return -1;
		}
		if (!verify_transpiled_output(input_file, vtmp)) {
			remove(vtmp);
			return -1;
		}
		int in_fd = open(vtmp, O_RDONLY);
		if (in_fd < 0) {
			remove(vtmp);
			return -1;
		}
		posix_spawn_file_actions_t vfa;
		posix_spawn_file_actions_init(&vfa);
		posix_spawn_file_actions_adddup2(&vfa, in_fd, STDIN_FILENO);
		posix_spawn_file_actions_addclose(&vfa, in_fd);
		char **venv = build_clean_environ();
		pid_t vpid;
		int verr = posix_spawnp(&vpid, compile_argv[0], &vfa, NULL, compile_argv, venv);
		posix_spawn_file_actions_destroy(&vfa);
		close(in_fd);
		if (verr) {
			fprintf(stderr, "posix_spawnp: %s: %s\n", compile_argv[0], strerror(verr));
			remove(vtmp);
			return -1;
		}
		int vrc = wait_for_child(vpid);
		remove(vtmp);
		return vrc;
	}

	double t0 = prism_now_ms();
	double pp_ms = 0.0, tok_ms = 0.0;
	PParseToken *tok = preprocess_and_tokenize(input_file, &pp_ms, &tok_ms);
	if (!tok) return -1;
	int pipefd[2];
	if (pipe(pipefd) == -1) {
		perror("pipe");
		pparse_tokenizer_teardown(false);
		return -1;
	}

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addclose(&fa, pipefd[1]);
	posix_spawn_file_actions_adddup2(&fa, pipefd[0], STDIN_FILENO);
	posix_spawn_file_actions_addclose(&fa, pipefd[0]);
	char **env = build_clean_environ();
	pid_t pid;
	int err = posix_spawnp(&pid, compile_argv[0], &fa, NULL, compile_argv, env);
	posix_spawn_file_actions_destroy(&fa);
	close(pipefd[0]);
	if (err) {
		fprintf(stderr, "posix_spawnp: %s: %s\n", compile_argv[0], strerror(err));
		close(pipefd[1]);
		pparse_tokenizer_teardown(false);
		return -1;
	}

	FILE *fp = fdopen(pipefd[1], "w");
	if (!fp) {
		close(pipefd[1]);
		pparse_tokenizer_teardown(false);
		waitpid(pid, NULL, 0);
		return -1;
	}

	double t1 = prism_now_ms();
	transpile_tokens(tok, fp);
	double t2 = prism_now_ms();
	int rc = wait_for_child(pid);
	double t3 = prism_now_ms();
	if (prism_profile) {
		fprintf(stderr,
			"[prism-prof] file=%s preprocess=%.3fms pparse_tokenize=%.3fms "
			"transpile=%.3fms "
			"cc_wait=%.3fms total=%.3fms\n",
			input_file,
			pp_ms,
			tok_ms,
			(t2 - t1),
			(t3 - t2),
			(t3 - t0));
	}
	return rc;
}

static noreturn void die(char *message) {
	fprintf(stderr, "%s\n", message);
	exit(1);
}

#ifndef _WIN32
static bool get_self_exe_path(char *buf, size_t bufsize) {
#if defined(__APPLE__)
	uint32_t sz = (uint32_t)bufsize;
	if (_NSGetExecutablePath(buf, &sz) == 0) {
		char temp[PATH_MAX];
		if (realpath(buf, temp)) {
			strncpy(buf, temp, bufsize - 1);
			buf[bufsize - 1] = '\0';
		}
		return true;
	}
#elif defined(__FreeBSD__) || defined(__DragonFly__)
	int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
	size_t len = bufsize;
	if (sysctl(mib, 4, buf, &len, NULL, 0) == 0) return true;
#else
	const char *links[] = {"/proc/self/exe", "/proc/curproc/exe", "/proc/self/path/a.out"};
	for (int i = 0; i < 3; i++) {
		ssize_t len = readlink(links[i], buf, bufsize - 1);
		if (len > 0) {
			buf[len] = '\0';
			return true;
		}
	}
#endif
	return false;
}

static const char *get_install_path(void) {
#ifndef _WIN32
	const char *prefix = getenv("PREFIX");
	if (prefix && *prefix) {
		static PRISM_THREAD_LOCAL char buf[PATH_MAX];
		snprintf(buf, sizeof(buf), "%s/bin/prism", prefix);
		return buf;
	}
#endif
	return INSTALL_PATH;
}

/* Create the directory holding `p`, including any missing parents.
 *
 * This used to be a single mkdir, which cannot create `$PREFIX/bin` unless
 * $PREFIX already exists. The failure was not reported as "could not create
 * the directory" either: the caller treats a false return as a permissions
 * problem and retries the whole install under sudo, so
 * `PREFIX=$HOME/.local prism install` prompted for a root password and then
 * failed, on a path the user could write to perfectly well. */
static bool ensure_install_dir(const char *p) {
	char dir[PATH_MAX];
	strncpy(dir, p, PATH_MAX - 1);
	dir[PATH_MAX - 1] = '\0';
	char *sep = strrchr(dir, '/');
	if (!sep) return true; /* bare filename: install into the cwd */
	*sep = '\0';
	if (!dir[0]) return true; /* p was "/name": root always exists */

	struct stat st;
	if (stat(dir, &st) == 0) return S_ISDIR(st.st_mode);

	/* Walk the chain, creating each missing component. Start at 1 so a
	 * leading '/' is never treated as an empty component to create. */
	for (char *q = dir + 1; *q; q++) {
		if (*q != '/') continue;
		*q = '\0';
		if (stat(dir, &st) != 0 && mkdir(dir, 0755) != 0 && errno != EEXIST) {
			*q = '/';
			return false;
		}
		*q = '/';
	}
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) return false;
	return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static void add_to_user_path(const char *dir) {
	(void)dir;
}
#endif

static void read_trimmed_line(char *buf, int bufsize, FILE *fp) {
	buf[0] = '\0';
	if (!fgets(buf, bufsize, fp)) return;
	size_t len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
	if (len > 0 && buf[len - 1] == '\r') buf[len - 1] = '\0';
}

static void check_path_shadow(const char *install_path) {
	const char *cmd = FIND_EXE_CMD;
	FILE *fp = popen(cmd, "r");
	if (!fp) return;
	char first_hit[PATH_MAX];
	read_trimmed_line(first_hit, sizeof(first_hit), fp);
	char resolved_hit[PATH_MAX], resolved_install[PATH_MAX];
#ifdef _WIN32
	char cwd[PATH_MAX];
	if (first_hit[0]) {
		wchar_t wcwd[PATH_MAX];
		bool got_cwd = false;
		if (_wgetcwd(wcwd, PATH_MAX)) {
			int ulen = WideCharToMultiByte(CP_UTF8, 0, wcwd, -1, cwd, PATH_MAX, NULL, NULL);
			got_cwd = (ulen > 0);
		}
		if (got_cwd) {
			char first_dir[PATH_MAX];
			strncpy(first_dir, first_hit, sizeof(first_dir) - 1);
			first_dir[sizeof(first_dir) - 1] = '\0';
			char *sep = strrchr(first_dir, '\\');
			if (!sep) sep = strrchr(first_dir, '/');
			if (sep) *sep = '\0';
			if (_stricmp(first_dir, cwd) == 0) {
				read_trimmed_line(first_hit, sizeof(first_hit), fp);
			}
		}
	}
	if (first_hit[0]) {
		UINT oem_cp = GetConsoleOutputCP();
		if (oem_cp && oem_cp != CP_UTF8) {
			wchar_t wide[PATH_MAX];
			int wlen = MultiByteToWideChar(oem_cp, 0, first_hit, -1, wide, PATH_MAX);
			if (wlen > 0) {
				char utf8[PATH_MAX];
				int ulen =
				    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, PATH_MAX, NULL, NULL);
				if (ulen > 0) memcpy(first_hit, utf8, (size_t)ulen);
			}
		}
	}
#endif
	pclose(fp);
	if (first_hit[0] && strcmp(first_hit, install_path) != 0) {
		char *rh = realpath(first_hit, resolved_hit);
		char *ri = realpath(install_path, resolved_install);
		if (rh && ri && strcmp(rh, ri) == 0) return; // Same file via symlink — no shadow
		fprintf(stderr,
			"[prism] Warning: '%s' shadows '%s' in your PATH.\n"
			"[prism] The newly installed version will NOT be used.\n"
			"[prism] Fix: remove or update '%s', or adjust your PATH.\n",
			first_hit,
			install_path,
			first_hit);
	}
}

/* Do two paths name the same file?
 *
 * The install self-check used to be strcmp, which only matches when the user
 * spells the destination exactly as argv[0]. Running the already-installed
 * copy through a symlink, a relative path, or any equivalent spelling slipped
 * past it and fell through to copying the file onto itself. On Linux that is
 * ETXTBSY, because the file is currently executing, so `prism install` from an
 * installed prism reported "Failed to install" and then escalated to sudo,
 * which also failed. Comparing device and inode answers the question the code
 * was actually asking; realpath alone would still miss hard links. */
static bool paths_are_same_file(const char *a, const char *b) {
#ifdef _WIN32
	char ra[PATH_MAX], rb[PATH_MAX];
	if (!_fullpath(ra, a, sizeof(ra)) || !_fullpath(rb, b, sizeof(rb)))
		return strcmp(a, b) == 0;
	for (char *p = ra; *p; p++)
		if (*p == '/') *p = '\\';
	for (char *p = rb; *p; p++)
		if (*p == '/') *p = '\\';
	return _stricmp(ra, rb) == 0;
#else
	struct stat sa, sb;
	if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return strcmp(a, b) == 0;
	return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#endif
}

static int install(char *self_path) {
	const char *install_path = get_install_path();
	printf("[prism] Installing to %s...\n", install_path);
	char resolved_path[PATH_MAX];
	struct stat st;
#ifdef _WIN32
	char old_path[PATH_MAX];
	old_path[0] = '\0';
#endif

	FILE *input = NULL;
	FILE *output = NULL;
	if (!ensure_install_dir(install_path)) goto use_sudo;
	if (stat(self_path, &st) != 0 && get_self_exe_path(resolved_path, sizeof(resolved_path)))
		self_path = resolved_path;
	if (paths_are_same_file(self_path, install_path)) {
		printf("[prism] Already installed at %s\n", install_path);
		return 0;
	}

	input = fopen(self_path, "rb");
	output = input ? fopen(install_path, "wb") : NULL;

#ifdef _WIN32
	if (input && !output && GetLastError() == ERROR_SHARING_VIOLATION) {
		snprintf(old_path, sizeof(old_path), "%s.old", install_path);
		remove(old_path); // remove any leftover from a previous update
		if (MoveFileA(install_path, old_path)) output = fopen(install_path, "wb");
		else
			old_path[0] = '\0'; // rename failed, will fall through to pparse_error
	}
#endif

	if (input && output) {
		char buffer[4096];
		size_t bytes;
		while ((bytes = fread(buffer, 1, 4096, input)) > 0) {
			if (fwrite(buffer, 1, bytes, output) != bytes) {
				fclose(input);
				fclose(output);
				goto use_sudo;
			}
		}
		fclose(input);
		fclose(output);
		chmod(install_path, 0755); // no-op on Windows (shimmed)
#ifdef _WIN32
		if (old_path[0]) {
			if (!remove(old_path)) {
				old_path[0] = '\0'; // successfully deleted
			} else {
				char temp_old[PATH_MAX];
				const char *tmp = get_tmp_dir();
				if (tmp && *tmp) {
					snprintf(temp_old,
						 sizeof(temp_old),
						 "%sprism_old_%u.exe",
						 tmp,
						 (unsigned)GetCurrentProcessId());
					if (MoveFileExA(old_path,
							temp_old,
							MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
						MoveFileExA(temp_old, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
						old_path[0] = '\0';
					}
				}
				if (old_path[0]) MoveFileExA(old_path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
			}
		}
#endif
		printf("[prism] Installed!\n");
		{
			char dir[PATH_MAX];
			strncpy(dir, install_path, PATH_MAX - 1);
			dir[PATH_MAX - 1] = '\0';
			char *sep = strrchr(dir, '/');
			char *bsep = strrchr(dir, '\\');
			if (bsep && (!sep || bsep > sep)) sep = bsep;
			if (sep) *sep = '\0';
			add_to_user_path(dir);
		}
		check_path_shadow(install_path);
		return 0;
	}

	if (input) fclose(input);
	if (output) fclose(output);

use_sudo:;
#ifdef _WIN32
	{
		DWORD err = GetLastError();
		fprintf(stderr, "[prism] Failed to install to %s (pparse_error %lu).\n", install_path, err);
		fprintf(stderr, "[prism] Try running as Administrator, or copy manually:\n");
		fprintf(stderr, "  copy \"%s\" \"%s\"\n", self_path, install_path);
	}
	return 1;
#else
	{
		const char *argv_cp[] = {"cp", self_path, install_path, NULL};
		if (run_command_quiet((char **)argv_cp) == 0) {
			const char *argv_chmod[] = {"chmod", "+x", install_path, NULL};
			run_command((char **)argv_chmod);
		} else {
			const char *escalate = NULL;
			const char *prefix = getenv("PREFIX");
			char sudo_path[PATH_MAX], doas_path[PATH_MAX];
			if (prefix && *prefix) {
				snprintf(sudo_path, sizeof(sudo_path), "%s/bin/sudo", prefix);
				snprintf(doas_path, sizeof(doas_path), "%s/bin/doas", prefix);
			} else
				sudo_path[0] = doas_path[0] = '\0';
			if (access("/usr/bin/sudo", X_OK) == 0 || access("/bin/sudo", X_OK) == 0 ||
			    (sudo_path[0] && access(sudo_path, X_OK) == 0))
				escalate = "sudo";
			else if (access("/usr/bin/doas", X_OK) == 0 || access("/bin/doas", X_OK) == 0 ||
				 (doas_path[0] && access(doas_path, X_OK) == 0))
				escalate = "doas";
			if (!escalate) {
				fprintf(stderr,
					"[prism] Permission denied and neither sudo nor doas found.\n"
					"  Install as root or copy manually:\n"
					"    cp %s %s && chmod +x %s\n",
					self_path,
					install_path,
					install_path);
				return 1;
			}

			const char *argv_rm[] = {escalate, "rm", "-f", install_path, NULL};
			run_command((char **)argv_rm);
			const char *argv_ecp[] = {escalate, "cp", self_path, install_path, NULL};
			if (run_command((char **)argv_ecp) != 0) {
				fprintf(stderr, "Failed to install\n");
				return 1;
			}

			const char *argv_chmod[] = {escalate, "chmod", "+x", install_path, NULL};
			run_command((char **)argv_chmod);
		}
	}
#endif

	printf("[prism] Installed!\n");
	check_path_shadow(install_path);
	return 0;
}

static bool is_prism_cc(const char *cc) {
	if (!cc || !*cc) return false;
	const char *exe = cc_executable(cc);
	const char *base = path_basename(exe);
	if (strncmp(base, "prism", 5) == 0) {
		char next = base[5];
		if (next == '\0' || next == ' ' || next == '.') return true;
	}
	return false;
}

static const char *get_real_cc(const char *cc) {
	if (!cc || !*cc || is_prism_cc(cc)) return PRISM_DEFAULT_CC;
	const char *exe = cc_executable(cc);
	if (!strpbrk(exe, "/\\")) return cc;
	char cc_real[PATH_MAX], self_real[PATH_MAX];
	if (get_self_exe_path(self_real, sizeof(self_real)) && realpath(exe, cc_real))
		if (strcmp(cc_real, self_real) == 0) return PRISM_DEFAULT_CC;
	return cc;
}

static int capture_first_line(char **argv, char *buf, size_t bufsize);

static bool cc_is_clang(const char *cc) {
#ifdef __APPLE__
	if (!cc || !*cc || strcmp(cc, "cc") == 0 || strcmp(cc, "gcc") == 0) return true;
#endif
	if (!cc || !*cc) return false;
	const char *exe = cc_executable(cc);
	const char *base = path_basename(exe);
	if (strncmp(base, "clang", 5) == 0) return true;
	char ver[256];
	char *probe_argv[] = {(char *)exe, "--version", NULL};
	if (capture_first_line(probe_argv, ver, sizeof(ver)) == 0) {
		for (char *p = ver; *p; p++) *p = (char)tolower((unsigned char)*p);
		if (strstr(ver, "clang")) return true;
	}
	return false;
}

#ifndef _WIN32
static ssize_t spawn_capture_stdout(char **argv, char *buf, size_t bufsize) {
	int pipefd[2];
	if (pipe(pipefd) != 0) return -1;
	char **env = build_clean_environ();
	if (!env) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&actions, pipefd[0]);
	int devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		posix_spawn_file_actions_adddup2(&actions, devnull, STDERR_FILENO);
		posix_spawn_file_actions_addclose(&actions, devnull);
	}
	pid_t pid;
	int err = posix_spawnp(&pid, argv[0], &actions, NULL, argv, env);
	posix_spawn_file_actions_destroy(&actions);
	close(pipefd[1]);
	if (devnull >= 0) close(devnull);
	if (err) {
		close(pipefd[0]);
		buf[0] = '\0';
		return -1;
	}
	size_t total = 0;
	while (total + 1 < bufsize) {
		ssize_t n = read(pipefd[0], buf + total, bufsize - 1 - total);
		if (n < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (n == 0) break;
		total += (size_t)n;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);
	buf[total] = '\0';
	return (ssize_t)total;
}

static int capture_first_line(char **argv, char *buf, size_t bufsize) {
	ssize_t n = spawn_capture_stdout(argv, buf, bufsize);
	if (n <= 0) {
		buf[0] = '\0';
		return -1;
	}
	char *nl = strchr(buf, '\n');
	if (nl) *nl = '\0';
	return 0;
}

static int capture_all_output(char **argv, char *buf, size_t bufsize) {
	ssize_t n = spawn_capture_stdout(argv, buf, bufsize);
	return (n > 0) ? 0 : -1;
}
#endif

static PRISM_COLD void print_help(void) {
	printf("Prism v%s - Robust C transpiler\n\n"
	       "Usage: prism [options] source.c... [-o output]\n"
	       "       prism [options] run src.c [-- prog_args...]\n\n"
	       "Commands:\n"
	       "  run <src.c> [-- args]  Transpile, compile, and run (args passed to "
	       "binary)\n"
	       "  transpile <src.c>      Output transpiled C to stdout\n"
	       "  check <tool> [args]    Run a static analyzer (cppcheck, clang-tidy, ...) on\n"
	       "                         transpiled sources; .c/.i args are swapped for analysis\n"
	       "                         artifacts, findings map to original lines via #line\n"
	       "  install [src.c...]     Install prism to %s\n\n"
	       "Prism Flags (consumed, not passed to CC):\n"
	       "  -fno-defer             Disable defer\n"
	       "  -fno-zeroinit          Disable zero-initialization\n"
	       "  -fno-orelse            Disable orelse keyword\n"
	       "  -fno-line-directives   Disable #line directives\n"
	       "  -fno-safety            Safety checks warn instead of error\n"
	       "  -fflatten-headers      Flatten headers into single output\n"
	       "  -fno-flatten-headers   Disable header flattening\n"
	       "  -fno-auto-unreachable  Disable __builtin_unreachable after "
	       "noreturn calls\n"
	       "  -fno-auto-static       Disable auto-static for const arrays with "
	       "literal inits\n"
	       "  -fno-bounds-check      Disable runtime bounds checks on "
	       "local, static and file-scope array subscripts\n"
	       "  -fno-link-pragma       Ignore #pragma link directives in source\n"
	       "  (each -fno-X above also accepts -fX to re-enable it)\n"
	       "  --prism-cc=<compiler>  Use specific compiler\n"
	       "  --prism-verbose        Show commands\n"
	       "  --prism-prof           Print per-phase timing breakdown\n"
	       "  --prism-verify         Translation validation: re-transpile emitted C,\n"
	       "                         require a fixed point (also: PRISM_VERIFY env)\n"
	       "  --prism-cache-info     Show the preprocessor cache location and size\n"
	       "  --prism-cache-clear    Delete all cached preprocessor output\n"
	       "  --prism-emit[=<file>]  Write transpiled C to stdout, or to <file>\n"
	       "  --                     Separator: remaining args are passed to the "
	       "binary in `run` mode\n\n"
	       "All other flags are passed through to CC.\n\n"
	       "Examples:\n"
	       "  prism foo.c -o foo                  Compile (GCC-compatible)\n"
	       "  prism run foo.c                     Compile and run\n"
	       "  prism -O2 run foo.c -- arg1 arg2    Compile and run with program "
	       "args\n"
	       "  prism transpile foo.c               Output transpiled C\n"
	       "  prism -O2 -Wall foo.c -o foo        With optimization\n"
	       "  CC=clang prism foo.c                Use clang as backend\n\n"
	       "Link Pragma (source-embedded linker flags):\n"
	       "  #pragma link <platform> <names...>\n"
	       "    platform: * | macos | macos_arm64 | macos_x86_64 | linux | "
	       "linux_arm64\n"
	       "              linux_x86_64 | linux_riscv64 | windows | "
	       "windows_x86_64 | windows_arm64\n"
	       "    name:     plain name (e.g. `Cocoa`, `m`): macOS => -framework, "
	       "else -l<name>\n"
	       "              or a literal flag starting with `-` (e.g. `-lm`, "
	       "`-framework Foo`)\n\n"
	       "Apache 2.0 license (c) Dawn Larsson 2026\n"
	       "https://github.com/dawnlarsson/prism\n",
	       PRISM_VERSION,
	       get_install_path());
}

static void add_warn_suppress(const char **args, int *argc, bool clang, bool msvc) {
	if (msvc) {
		args[(*argc)++] = "/wd4100";
		args[(*argc)++] = "/wd4189";
		args[(*argc)++] = "/wd4244";
		args[(*argc)++] = "/wd4267";
		args[(*argc)++] = "/wd4068";
		return;
	}
	static const char *w[] = {
	    "-Wno-type-limits",
	    "-Wno-cast-align",
	    "-Wno-implicit-fallthrough",
	    "-Wno-unused-function",
	    "-Wno-unused-variable",
	    "-Wno-unused-parameter",
	};
	for (int i = 0; i < (int)(sizeof(w) / sizeof(*w)); i++) args[(*argc)++] = w[i];
	if (clang) args[(*argc)++] = "-Wno-unknown-warning-option";
	else
		args[(*argc)++] = "-Wno-logical-op";
}

static void verbose_argv(char **args) {
	fprintf(stderr, "[prism]");
	for (int i = 0; args[i]; i++) fprintf(stderr, " %s", args[i]);
	fprintf(stderr, "\n");
}

typedef struct {
	const char *compiler;
	bool clang;
	bool msvc;
	const char *output;
	bool compile_only;
	bool optimize;
	bool suppress_warnings;
	bool use_preprocessed;
} TempCompilePlan;

static const char *cli_output_path(const Cli *cli, const char *temp_exe, bool msvc) {
	static PRISM_THREAD_LOCAL char defobj[PATH_MAX];
	if (cli->mode == CLI_RUN) return temp_exe;
	if (cli->output) return cli->output;
	if (cli->compile_only && cli->source_count == 1) {
		const char *base = path_basename(cli->sources[0]);
		snprintf(defobj, sizeof(defobj), "%s", base);
		char *dot = strrchr(defobj, '.');
		const char *ext = cli->assemble_only ? ".s" : (msvc ? ".obj" : ".o");
		if (dot) snprintf(dot, sizeof(defobj) - (size_t)(dot - defobj), "%s", ext);
		else {
			size_t n = strlen(defobj);
			snprintf(defobj + n, sizeof(defobj) - n, "%s", ext);
		}
		return defobj;
	}
	return NULL;
}

/* Default object/asm name for source path (basename + .o/.s/.obj). */
static void cli_default_unit_output(char *out, size_t outsz, const char *src, bool assemble_only,
				   bool msvc) {
	const char *base = path_basename(src);
	snprintf(out, outsz, "%s", base);
	char *dot = strrchr(out, '.');
	const char *ext = assemble_only ? ".s" : (msvc ? ".obj" : ".o");
	if (dot) snprintf(dot, outsz - (size_t)(dot - out), "%s", ext);
	else {
		size_t n = strlen(out);
		snprintf(out + n, outsz - n, "%s", ext);
	}
}

/* -MD/-MMD deps are generated during preprocess (`cc -E`), which strips `-o`.
 * Inject `-MT <output>` so the .d target matches the real link/object name. */
static void cli_inject_dep_mt_from_output(Cli *cli) {
	if (!cli->output || !cli->output[0]) return;
	bool has_md = false, has_mt = false;
	for (int i = 0; i < cli->dep_arg_count; i++) {
		const char *a = cli->dep_args[i];
		if (!strcmp(a, "-MD") || !strcmp(a, "-MMD")) has_md = true;
		if (!strcmp(a, "-MT") || !strcmp(a, "-MQ")) has_mt = true;
		if (a[0] == '-' && a[1] == 'W' && a[2] == 'p' && a[3] == ',') {
			const char *v = a + 4;
			if (strstr(v, "-MD") || strstr(v, "-MMD")) has_md = true;
			if (strstr(v, "-MT") || strstr(v, "-MQ")) has_mt = true;
		}
	}
	if (!has_md || has_mt) return;
	CLI_PUSH(cli->dep_args, cli->dep_arg_count, cli->dep_arg_cap, "-MT");
	CLI_PUSH(cli->dep_args, cli->dep_arg_count, cli->dep_arg_cap, cli->output);
}

static void argv_add_output(const char **args, int *argc, const char *out, bool msvc, bool compile_only) {
	if (!out) return;
	if (msvc) {
		static PRISM_THREAD_LOCAL char flag[PATH_MAX + 8]; // cl.exe: /Fe:exe or /Fo:obj
		if (compile_only) snprintf(flag, sizeof(flag), "/Fo:%s", out);
		else
			snprintf(flag, sizeof(flag), "/Fe:%s", out);
		args[(*argc)++] = flag;
	} else {
		args[(*argc)++] = "-o";
		args[(*argc)++] = out;
	}
}

/* How many argv slots to skip when forwarding cc_args to a backend compile of
 * already-preprocessed/transpiled input. -include/-imacros (/FI) are consumed
 * during Prism's preprocess; re-passing them injects the raw file again
 * (redefinitions + untranspiled orelse/defer). Also covers -Wp,-include,file
 * and -Xpreprocessor -include -Xpreprocessor file. */
static int cc_force_include_skip(const char *a) {
	if (!a) return 0;
	if (!strcmp(a, "-include") || !strcmp(a, "-imacros")) return 2;
	if (!strcmp(a, "/FI") || !strcmp(a, "-FI")) return 2;
	if ((!strncmp(a, "/FI", 3) || !strncmp(a, "-FI", 3)) && a[3]) return 1;
	if (!strncmp(a, "-Wp,-include,", 13) || !strncmp(a, "-Wp,-imacros,", 13)) return 1;
	return 0;
}

static int cc_backend_force_include_skip(const char **args, int i, int n) {
	int k = cc_force_include_skip(args[i]);
	if (k) return k;
	if (!strcmp(args[i], "-Xpreprocessor") && i + 3 < n &&
	    (!strcmp(args[i + 1], "-include") || !strcmp(args[i + 1], "-imacros")) &&
	    !strcmp(args[i + 2], "-Xpreprocessor"))
		return 4;
	return 0;
}

static void make_run_temp(char *buf, size_t size, CliMode mode) {
	buf[0] = '\0';
	if (mode != CLI_RUN) return;
	int suffix_len = (int)strlen(EXE_SUFFIX);
	snprintf(buf, size, "%sprism_run.XXXXXX%s", get_tmp_dir(), EXE_SUFFIX);
	int fd = suffix_len > 0 ? mkstemps(buf, suffix_len) : mkstemp(buf);
	if (fd >= 0) close(fd);
	else
		buf[0] = '\0';
}

static int passthrough_cc(const Cli *cli) {
	const char *compiler = get_real_cc(cli->cc);
	bool msvc = cc_is_msvc(compiler);
	/* Pure .cpp/.cc/… never hit compile_sources; still need g++/clang++ so
	 * libc++/libstdc++ link (else STL/iostream fail at link). */
	if (!msvc && cli_has_cxx_passthrough(cli)) compiler = cxx_driver_for_cc(compiler);
	int cc_extra = cc_extra_arg_count(compiler);
	const char **args = alloc_argv(cli->cc_arg_count + cli->dep_arg_count + cc_extra + 8);
	int argc = 0;
	char *cc_dup = NULL;
	cc_split_into_argv(args, &argc, compiler, &cc_dup);
	for (int i = 0; i < cli->dep_arg_count; i++) args[argc++] = cli->dep_args[i];
	for (int i = 0; i < cli->cc_arg_count; i++) args[argc++] = cli->cc_args[i];
	argv_add_output(args, &argc, cli->output, msvc, false);
	args[argc] = NULL;
	if (cli->verbose) verbose_argv((char **)args);
	int st = run_command((char **)args);
	free(cc_dup);
	free((void *)args);
	return st;
}

static void cleanup_temp_range(char **temps, int count) {
	for (int i = 0; i < count; i++) {
		remove(temps[i]);
#ifdef _WIN32
		// MSVC places .obj in the CWD using the input file's basename.
		{
			const char *base = temps[i];
			for (const char *p = temps[i]; *p; p++)
				if (*p == '/' || *p == '\\') base = p + 1;
			char obj_path[PATH_MAX];
			strncpy(obj_path, base, sizeof(obj_path) - 1);
			obj_path[sizeof(obj_path) - 1] = '\0';
			char *dot = strrchr(obj_path, '.');
			if (dot && (size_t)(dot - obj_path) < sizeof(obj_path) - 5) {
				strcpy(dot, ".obj");
				remove(obj_path);
			}
		}
#endif
		free(temps[i]);
	}
	free(temps);
	signal_temps_clear();
}

static bool compiler_is_cxx_driver(const char *cc) {
	if (!cc || !*cc) return false;
	const char *base = path_basename(cc_executable(cc));
	return strstr(base, "++") != NULL || strcmp(base, "c++") == 0;
}

/* g++ with leading `-x c` omits -lstdc++ (GCC 16+). clang++ / Apple's c++
 * already link the right C++ runtime — injecting -lc++/-lstdc++ only
 * duplicates the library and can fail under -Wl,-fatal_warnings. */
static void argv_add_cxx_stdlib(const char **args, int *argc, const char *compiler, bool clang) {
	if (clang) return;
	if (!compiler || !*compiler) return;
	const char *base = path_basename(cc_executable(compiler));
	/* `c++` drivers (libstdc++ or libc++) already pull the runtime. */
	if (strcmp(base, "c++") == 0) return;
	args[(*argc)++] = "-lstdc++";
}

static int run_temp_compile_plan(const Cli *cli, char **temps, int temp_count, const TempCompilePlan *plan) {
	int cc_extra = cc_extra_arg_count(plan->compiler);
	/* +2 per temp for repeated `-x c`, +4 for none/stdlib */
	const char **args = alloc_argv(temp_count * 3 + cli->cc_arg_count + cc_extra + 32);
	int argc = 0;
	char *cc_dup = NULL;
	cc_split_into_argv(args, &argc, plan->compiler, &cc_dup);
	if (plan->msvc) {
		args[argc++] = "/nologo";
		// Prism may emit typeof()/typeof_unqual() which require C23 mode on MSVC.
		args[argc++] = "/std:clatest";
	}
	if (plan->optimize) args[argc++] = plan->msvc ? "/O2" : "-O2";
	if (plan->use_preprocessed) args[argc++] = "-fpreprocessed";
	/* Multi-file `prism a.c b.c main.cpp` upgrades to g++/clang++ for libstdc++.
	 * GCC only applies `-x` to the next input, so repeat before every temp. */
	bool force_c_temps = !plan->msvc && compiler_is_cxx_driver(plan->compiler) && temp_count > 0;
	for (int i = 0; i < temp_count; i++) {
		if (force_c_temps) {
			args[argc++] = "-x";
			args[argc++] = "c";
		}
		args[argc++] = temps[i];
	}
	if (plan->use_preprocessed) args[argc++] = "-fno-preprocessed";
	if (force_c_temps) {
		args[argc++] = "-x";
		args[argc++] = "none";
	}
	for (int i = 0; i < cli->cc_arg_count; i++) {
		// Skip user's /std: flags on MSVC — we already injected /std:clatest.
		if (plan->msvc && strncmp(cli->cc_args[i], "/std:", 5) == 0) continue;
		int fi_skip = cc_backend_force_include_skip(cli->cc_args, i, cli->cc_arg_count);
		if (fi_skip) {
			i += fi_skip - 1;
			continue;
		}
		args[argc++] = cli->cc_args[i];
	}
	if (plan->suppress_warnings) add_warn_suppress(args, &argc, plan->clang, plan->msvc);
	argv_add_output(args, &argc, plan->output, plan->msvc, plan->compile_only);
	if (force_c_temps && !plan->compile_only) argv_add_cxx_stdlib(args, &argc, plan->compiler, plan->clang);
	args[argc] = NULL;
	if (cli->verbose) verbose_argv((char **)args);
	int status = run_command((char **)args);
	free(cc_dup);
	free((void *)args);
	return status;
}

static const char *resolve_install_compiler(const Cli *cli) {
#ifdef _WIN32
	const char *cc = get_real_cc(cli->cc ? cli->cc : get_env_utf8("PRISM_CC"));
	if (!cc || (strcmp(cc, "cc") == 0 && !cli->cc)) {
		cc = get_env_utf8("CC");
		if (cc) cc = get_real_cc(cc);
	}
#else
	const char *cc = get_real_cc(cli->cc ? cli->cc : getenv("PRISM_CC"));
	if (!cc || (strcmp(cc, "cc") == 0 && !cli->cc)) {
		cc = getenv("CC");
		if (cc) cc = get_real_cc(cc);
	}
#endif
	return cc ? cc : PRISM_DEFAULT_CC;
}

static char **transpile_sources_to_temps(const Cli *cli, bool use_lib_api) {
	char **temps = calloc(cli->source_count, sizeof(char *));
	if (!temps) die("Out of memory");
	signal_temps_clear();
	for (int i = 0; i < cli->source_count; i++) {
		if (use_lib_api) {
			temps[i] = malloc(PATH_MAX);
			if (!temps[i]) die("Out of memory");
			int fd = make_temp_file_registered(temps[i], PATH_MAX, NULL, 0, cli->sources[i]);
			if (fd < 0) die("Failed to create temp file");
			PrismResult result = prism_transpile_file(cli->sources[i], cli->features);
			if (result.status != PRISM_OK) {
				fprintf(stderr,
					"%s:%d:%d: pparse_error: %s\n",
					cli->sources[i],
					result.error_line,
					result.error_col,
					result.error_msg ? result.error_msg : "transpilation failed");
				prism_free(&result);
				close(fd);
				cleanup_temp_range(temps, i + 1);
				return NULL;
			}
			FILE *f = fdopen(fd, "w");
			if (!f) {
				prism_free(&result);
				close(fd);
				die("Failed to create temp file");
			}
			fwrite(result.output, 1, result.output_len, f);
			fclose(f);
			prism_free(&result);
		} else {
			temps[i] = malloc(512);
			if (!temps[i]) die("Out of memory");
			int fd = make_temp_file_registered(temps[i], 512, NULL, 0, cli->sources[i]);
			if (fd < 0) die("Failed to create temp file");
			if (cli->verbose)
				fprintf(stderr, "[prism] Transpiling %s -> %s\n", cli->sources[i], temps[i]);
			FILE *wfp = fdopen(fd, "w");
			if (!wfp) {
				close(fd);
				die("Failed to open temp file");
			}
			if (!transpile_to_fp((char *)cli->sources[i], wfp)) {
				cleanup_temp_range(temps, i + 1);
				return NULL;
			}
			if (prism_verify_mode && !prism_in_verify &&
			    !verify_transpiled_output((char *)cli->sources[i], temps[i])) {
				cleanup_temp_range(temps, i + 1);
				return NULL;
			}
		}
	}
	return temps;
}

static int install_from_source(Cli *cli) {
	char temp_bin[PATH_MAX];
	int suffix_len = (int)strlen(EXE_SUFFIX);
	snprintf(temp_bin, sizeof(temp_bin), "%sprism_inst_.XXXXXX%s", get_tmp_dir(), EXE_SUFFIX);
	int fd = suffix_len > 0 ? mkstemps(temp_bin, suffix_len) : mkstemp(temp_bin);
	if (fd < 0) die("Failed to create temp file");
	close(fd);
	const char *cc = resolve_install_compiler(cli);
	bool msvc = cc_is_msvc(cc);
	char **temps = transpile_sources_to_temps(cli, true);
	if (!temps) return 1;
	TempCompilePlan plan = {
	    .compiler = cc,
	    .msvc = msvc,
	    .output = temp_bin,
	    .optimize = true,
	    .suppress_warnings = true,
	};
	int status = run_temp_compile_plan(cli, temps, cli->source_count, &plan);
	cleanup_temp_range(temps, cli->source_count);
	if (status != 0) return 1;
	int result = install(temp_bin);
	remove(temp_bin);
	return result;
}

static const char *host_platform_tag(void) {
#if defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
	return "macos_arm64";
#elif defined(__x86_64__)
	return "macos_x86_64";
#else
	return "macos";
#endif
#elif defined(_WIN32) || defined(_WIN64)
#if defined(__aarch64__) || defined(_M_ARM64)
	return "windows_arm64";
#else
	return "windows_x86_64";
#endif
#elif defined(__linux__)
#if defined(__aarch64__)
	return "linux_arm64";
#elif defined(__x86_64__)
	return "linux_x86_64";
#elif defined(__riscv)
	return "linux_riscv64";
#else
	return "linux";
#endif
#else
	return "unknown";
#endif
}

static bool link_pragma_platform_matches(const char *platform, size_t plen, const char *host) {
	if (plen == 1 && platform[0] == '*') return true;
	size_t hlen = strlen(host);
	if (plen == hlen && prism_memeq_runtime_sized(platform, host, (uint32_t)hlen)) return true;
	const char *us = memchr(host, '_', hlen);
	if (us) {
		size_t os_len = (size_t)(us - host);
		if (plen == os_len && prism_memeq_runtime_sized(platform, host, (uint32_t)os_len)) return true;
	}
	return false;
}

static void collect_link_pragmas_file(const char *path, Cli *cli, const char *host, bool macos) {
	if (!path || cli->no_link_pragma) return;
	FILE *f = fopen(path, "r");
	if (!f) return;
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	while ((n = getline(&line, &cap, f)) != -1) {
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p != '#') continue;
		p++;
		while (*p == ' ' || *p == '\t') p++;
		if (strncmp(p, "pragma", 6) != 0) continue;
		p += 6;
		if (*p != ' ' && *p != '\t') continue;
		while (*p == ' ' || *p == '\t') p++;
		if (strncmp(p, "link", 4) != 0) continue;
		p += 4;
		if (*p != ' ' && *p != '\t') continue;
		while (*p == ' ' || *p == '\t') p++;
		char *plat = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
		size_t plen = (size_t)(p - plat);
		if (plen == 0) continue;
		bool match = link_pragma_platform_matches(plat, plen, host);
		while (*p) {
			while (*p == ' ' || *p == '\t') p++;
			if (!*p || *p == '\n' || *p == '\r') break;
			if (p[0] == '/' && (p[1] == '/' || p[1] == '*')) break;
			char *tok = p;
			while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
			size_t tlen = (size_t)(p - tok);
			if (!match || tlen == 0) continue;
			if (tok[0] == '-') {
				// the flag's value — we don't special-case it here;
				char *dup = malloc(tlen + 1);
				if (!dup) {
					free(line);
					fclose(f);
					die("out of memory");
				}
				memcpy(dup, tok, tlen);
				dup[tlen] = 0;
				CLI_PUSH(cli->cc_args, cli->cc_arg_count, cli->cc_arg_cap, dup);
			} else if (macos) {
				char *dup = malloc(tlen + 1);
				if (!dup) {
					free(line);
					fclose(f);
					die("out of memory");
				}
				memcpy(dup, tok, tlen);
				dup[tlen] = 0;
				CLI_PUSH(cli->cc_args, cli->cc_arg_count, cli->cc_arg_cap, "-framework");
				CLI_PUSH(cli->cc_args, cli->cc_arg_count, cli->cc_arg_cap, dup);
			} else {
				char *buf = malloc(tlen + 3);
				if (!buf) {
					free(line);
					fclose(f);
					die("out of memory");
				}
				buf[0] = '-';
				buf[1] = 'l';
				memcpy(buf + 2, tok, tlen);
				buf[tlen + 2] = 0;
				CLI_PUSH(cli->cc_args, cli->cc_arg_count, cli->cc_arg_cap, buf);
			}
		}
	}

	free(line);
	fclose(f);
}

static void collect_link_pragmas(Cli *cli) {
	if (cli->no_link_pragma || cli->source_count == 0) return;
	const char *host = host_platform_tag();
	bool macos = strncmp(host, "macos", 5) == 0;
	for (int i = 0; i < cli->source_count; i++)
		collect_link_pragmas_file(cli->sources[i], cli, host, macos);
}

static bool cli_has_cxx_passthrough(const Cli *cli) {
	for (int i = 0; i < cli->cc_arg_count; i++) {
		const char *a = cli->cc_args[i];
		if (!a || a[0] == '-') continue;
		if (has_ext(a, ".cpp") || has_ext(a, ".cc") || has_ext(a, ".cxx") || has_ext(a, ".C") ||
		    has_ext(a, ".mm"))
			return true;
	}
	return false;
}

/* SPEC: C++ passthrough triggers g++/clang++ so libstdc++/libc++ link.
 * Preserve cross prefixes: x86_64-w64-mingw32-gcc → x86_64-w64-mingw32-g++. */
static const char *cxx_driver_for_cc(const char *cc) {
	if (!cc || !*cc) return "g++";
	if (cc_is_msvc(cc)) return cc;
	static PRISM_THREAD_LOCAL char buf[PATH_MAX];
	const char *exe = cc_executable(cc);
	const char *base = path_basename(exe);
	if (strstr(base, "++") || strcmp(base, "c++") == 0) return cc;
	size_t dir_len = (size_t)(base - exe);
	if (strncmp(base, "clang", 5) == 0) {
		snprintf(buf, sizeof(buf), "%.*sclang++%s", (int)dir_len, exe, base + 5);
		return buf;
	}
	const char *gcc = strstr(base, "gcc");
	if (gcc) {
		snprintf(buf,
			 sizeof(buf),
			 "%.*s%.*sg++%s",
			 (int)dir_len,
			 exe,
			 (int)(gcc - base),
			 base,
			 gcc + 3);
		return buf;
	}
	if (strcmp(base, "cc") == 0) {
		if (cc_is_clang(cc)) {
			snprintf(buf, sizeof(buf), "%.*sclang++", (int)dir_len, exe);
			return buf;
		}
		snprintf(buf, sizeof(buf), "%.*sc++", (int)dir_len, exe);
		return buf;
	}
	if (cc_is_clang(cc)) return "clang++";
	return "g++";
}

static int compile_sources(Cli *cli) {
	PRISM_STATE();
	PPARSE_CTX();
	int status = 0;
	collect_link_pragmas(cli);
	_ps->extra_compiler_flags = cli->cc_args;
	_ps->extra_compiler_flags_count = cli->cc_arg_count;
	const char *compiler = get_real_cc(cli->cc);
	bool msvc = cc_is_msvc(compiler);
	if (!msvc && cli_has_cxx_passthrough(cli)) compiler = cxx_driver_for_cc(compiler);
	int cc_extra = cc_extra_arg_count(compiler);
	bool clang = cc_is_clang(compiler);
	char temp_exe[PATH_MAX];
	make_run_temp(temp_exe, sizeof(temp_exe), cli->mode);
	if (temp_exe[0]) {
		signal_temp_store(0);
		memcpy(signal_temp_path, temp_exe, sizeof(signal_temp_path));
		signal_temp_store(1);
	}

	use_linemarkers = pparse_feat(PPARSE_F_FLATTEN) && !clang && !msvc;
	/* Stdin pipe + -save-temps makes the backend invent `-.i` / `-.s` names
	 * (invalid args). Fall back to on-disk temps so save-temps keeps working. */
	bool save_temps = false;
	for (int i = 0; i < cli->cc_arg_count; i++) {
		const char *a = cli->cc_args[i];
		if (!strcmp(a, "-save-temps") || !strncmp(a, "-save-temps=", 12) ||
		    !strcmp(a, "--save-temps") || !strncmp(a, "--save-temps=", 13)) {
			save_temps = true;
			break;
		}
	}
	if (cli->source_count == 1 && !msvc && !save_temps) {
		/* Pipe language follows the `-x` that bound the Prism source
		 * (GCC positional rules). Do NOT steal a later `-x c++` meant
		 * for a passthrough .cpp — that used to compile C as C++. */
		const char *pipe_lang = cli->source_x_lang ? cli->source_x_lang : "c";
		int x_flag_idx = cli->source_x_arg_idx;
		/* GCC `-x none` restores extension-based guessing for subsequent files.
		 * Our pipe is stdin, so map `none` back to the TU's default language. */
		if (pipe_lang && !strcmp(pipe_lang, "none")) pipe_lang = "c";

		const char **args = alloc_argv(cli->cc_arg_count + cc_extra + 24);
		int argc = 0;
		char *cc_dup = NULL;
		cc_split_into_argv(args, &argc, compiler, &cc_dup);
		args[argc++] = "-x";
		args[argc++] = pipe_lang;
		if (pparse_feat(PPARSE_F_FLATTEN) && !clang) args[argc++] = "-fpreprocessed";
		args[argc++] = "-";
		bool need_x_none = false;
		for (int i = 0; i < cli->cc_arg_count; i++) {
			if (i == x_flag_idx) {
				if (!strcmp(cli->cc_args[i], "-x") && i + 1 < cli->cc_arg_count) i++;
				continue;
			}
			const char *a = cli->cc_args[i];
			/* Operands of -I/-include/-isystem/… are paths, not TUs.
			 * Treating them as inputs falsely cleared -fpreprocessed
			 * (split `-I /usr/include` misscompiled flatten mode). */
			if (a[0] == '-') {
				if (cc_flag_takes_arg(a) && i + 1 < cli->cc_arg_count) i++;
				continue;
			}
			need_x_none = true;
			break;
		}
		if (need_x_none) {
			/* -fpreprocessed must not apply to passthrough .cpp/.s/… */
			if (pparse_feat(PPARSE_F_FLATTEN) && !clang) args[argc++] = "-fno-preprocessed";
			args[argc++] = "-x";
			args[argc++] = "none";
		}
		for (int i = 0; i < cli->cc_arg_count; i++) {
			if (i == x_flag_idx) {
				if (!strcmp(cli->cc_args[i], "-x") && i + 1 < cli->cc_arg_count) i++;
				continue;
			}
			int fi_skip = cc_backend_force_include_skip(cli->cc_args, i, cli->cc_arg_count);
			if (fi_skip) {
				i += fi_skip - 1;
				continue;
			}
			args[argc++] = cli->cc_args[i];
		}
		add_warn_suppress(args, &argc, clang, false);
		argv_add_output(args, &argc, cli_output_path(cli, temp_exe, false), false, cli->compile_only);
		/* Leading `-x c` makes g++ drop -lstdc++; restore when linking C++. */
		if (need_x_none && compiler_is_cxx_driver(compiler) && !cli->compile_only)
			argv_add_cxx_stdlib(args, &argc, compiler, clang);
		args[argc] = NULL;
		if (cli->verbose) fprintf(stderr, "[prism] Transpiling %s (pipe → cc)\n", cli->sources[0]);
		status = transpile_and_compile((char *)cli->sources[0], (char **)args, cli->verbose);
		free(cc_dup);
		free((void *)args);
	} else {
		char **temps = transpile_sources_to_temps(cli, false);
		if (!temps) die("Transpilation failed");
		if (cli->compile_only && cli->output && cli->source_count > 1) {
			fprintf(stderr, "pparse_error: cannot specify -o when generating multiple output files\n");
			status = 1;
			cleanup_temp_range(temps, cli->source_count);
		} else if (cli->compile_only && !cli->output && cli->source_count > 1) {
			/* `cc -c a.c b.c` writes a.o b.o — not temp basenames. */
			status = 0;
			for (int i = 0; i < cli->source_count; i++) {
				char out[PATH_MAX];
				cli_default_unit_output(out, sizeof(out), cli->sources[i], cli->assemble_only,
							msvc);
				TempCompilePlan plan = {
				    .compiler = compiler,
				    .clang = clang,
				    .msvc = msvc,
				    .output = out,
				    .compile_only = true,
				    .suppress_warnings = true,
				    .use_preprocessed = pparse_feat(PPARSE_F_FLATTEN) && !clang && !msvc,
				};
				int st = run_temp_compile_plan(cli, &temps[i], 1, &plan);
				if (st != 0) {
					status = st;
					break;
				}
			}
			cleanup_temp_range(temps, cli->source_count);
		} else {
			TempCompilePlan plan = {
			    .compiler = compiler,
			    .clang = clang,
			    .msvc = msvc,
			    .output = cli_output_path(cli, temp_exe, msvc),
			    .compile_only = cli->compile_only,
			    .suppress_warnings = true,
			    .use_preprocessed = pparse_feat(PPARSE_F_FLATTEN) && !clang && !msvc,
			};
			status = run_temp_compile_plan(cli, temps, cli->source_count, &plan);
			cleanup_temp_range(temps, cli->source_count);
		}
	}

	if (status != 0) {
		if (temp_exe[0]) remove(temp_exe);
		signal_temp_store(0);
		return status;
	}

	if (cli->mode == CLI_RUN) {
		const char **run = alloc_argv(2 + cli->prog_arg_count);
		int rc = 0;
		run[rc++] = temp_exe;
		for (int i = 0; i < cli->prog_arg_count; i++) run[rc++] = cli->prog_args[i];
		run[rc] = NULL;
		if (cli->verbose) {
			fprintf(stderr, "[prism] Running %s", temp_exe);
			for (int i = 0; i < cli->prog_arg_count; i++)
				fprintf(stderr, " %s", cli->prog_args[i]);
			fprintf(stderr, "\n");
		}
		status = run_command((char **)run);
		free((void *)run);
		remove(temp_exe);
	}

	signal_temp_store(0);
	return status;
}

static void signal_cleanup_handler(int sig) {
#ifdef _WIN32
	if (out_fp) {
		fflush(out_fp);
		win32_real_fclose(out_fp);
		out_fp = NULL;
	}
	if (win32_memstream_fp) {
		win32_real_fclose(win32_memstream_fp);
		win32_memstream_fp = NULL;
	}
#endif
	if (signal_temp_load() && signal_temp_path[0]) unlink(signal_temp_path);
	int n = signal_temps_load();
	for (int i = 0; i < n; i++)
		if (signal_temps_ready_load(i) && signal_temps[i][0] != '\0') unlink(signal_temps[i]);
	signal(sig, SIG_DFL);
	raise(sig);
}

int main(int argc, char **argv) {
PRISM_STATE();
#ifdef _WIN32
	win32_utf8_argv(&argc, &argv);
#endif
	signal(SIGINT, signal_cleanup_handler);
	signal(SIGTERM, signal_cleanup_handler);
	signal(SIGPIPE,
	       SIG_IGN); // no-op on Windows (SIGPIPE defined but never raised)
	int status = 0;
	pparse_ctx_init();
	PPARSE_CTX();
	if (argc < 2) {
		print_help();
		return 0;
	}

	Cli cli = cli_parse(argc, argv);
	prism_profile = cli.profile;
	prism_verify_mode = cli.verify || getenv("PRISM_VERIFY") != NULL;
	if (cli.action == CLI_ACT_HELP) {
		print_help();
		cli_free(&cli);
		return 0;
	}
	if (cli.action == CLI_ACT_VERSION) {
		const char *real_cc = get_real_cc(cli.cc);
		char cc_out[4096];
		char *vargs[] = {(char *)real_cc, "--version", NULL};
		if (capture_all_output(vargs, cc_out, sizeof(cc_out)) == 0 && cc_out[0]) {
			char *nl = strchr(cc_out, '\n');
			if (nl) {
				*nl = '\0';
				printf("prism %s (%s)\n%s", PRISM_VERSION, cc_out, nl + 1);
				size_t tail = strlen(nl + 1);
				if (tail == 0 || (nl + 1)[tail - 1] != '\n') putchar('\n');
			} else {
				printf("prism %s (%s)\n", PRISM_VERSION, cc_out);
			}
		} else {
			printf("prism %s\n", PRISM_VERSION);
		}
		cli_free(&cli);
		return 0;
	}

	if (!cli.cc) {
#ifdef _WIN32
		char *env_cc = (char *)get_env_utf8("PRISM_CC");
		if (!env_cc || !*env_cc || is_prism_cc(env_cc)) {
			env_cc = (char *)get_env_utf8("CC");
			if (is_prism_cc(env_cc)) env_cc = NULL;
		}
#else
		char *env_cc = getenv("PRISM_CC");
		if (!env_cc || !*env_cc || is_prism_cc(env_cc)) {
			env_cc = getenv("CC");
			if (is_prism_cc(env_cc)) env_cc = NULL;
		}
#endif
		cli.cc = (env_cc && *env_cc) ? env_cc : PRISM_DEFAULT_CC;
	}

	/* Analysis profile for `prism check`: keep language semantics (defer/
	 * orelse/zeroinit) but shape the artifact for analyzers — #include lines
	 * intact and bare subscripts (the bounds wrapper hides constant indices
	 * from static analysis; the shipping build still gets both features). */
	if (cli.mode == CLI_CHECK) {
		cli.features.flatten_headers = false;
		cli.features.bounds_check = false;
		cli.features.line_directives = true;
	}
	_pc->features = features_to_bits(cli.features);
	_ps->extra_compiler = get_real_cc(cli.cc);
	_ps->extra_compiler_flags = cli.cc_args;
	_ps->extra_compiler_flags_count = cli.cc_arg_count;
	_ps->dep_flags = cli.dep_args;
	_ps->dep_flags_count = cli.dep_arg_count;
	cli_inject_dep_mt_from_output(&cli);
	_ps->dep_flags = cli.dep_args;
	_ps->dep_flags_count = cli.dep_arg_count;
	if (cli.mode == CLI_CHECK) {
		if (!cli.check_tool) die("check: no analyzer given (usage: prism check <tool> [args...])");
		char **temps = NULL;
		if (cli.source_count > 0) {
			temps = transpile_sources_to_temps(&cli, false);
			if (!temps) {
				cli_free(&cli);
				return 1;
			}
		}
		const char **targv = alloc_argv(2 + cli.check_arg_count);
		int tc = 0;
		targv[tc++] = cli.check_tool;
		for (int i = 0; i < cli.check_arg_count; i++) {
			const char *ta = cli.check_args[i];
			for (int k = 0; k < cli.source_count; k++)
				if (ta == cli.sources[k]) {
					ta = temps[k];
					break;
				}
			targv[tc++] = ta;
		}
		targv[tc] = NULL;
		if (cli.verbose) {
			fprintf(stderr, "[prism] check:");
			for (int i = 0; i < tc; i++) fprintf(stderr, " %s", targv[i]);
			fprintf(stderr, "\n");
		}
		status = run_command((char **)targv);
		free((void *)targv);
		if (temps) cleanup_temp_range(temps, cli.source_count);
	} else if (cli.mode == CLI_INSTALL)
		status = cli.source_count > 0 ? install_from_source(&cli) : install(argv[0]);
	else if (cli.mode == CLI_EMIT) {
		if (cli.source_count == 0) die("No source files specified");
		for (int i = 0; i < cli.source_count; i++) {
			if (cli.output) {
				if (cli.verbose)
					fprintf(stderr, "[prism] %s -> %s\n", cli.sources[i], cli.output);
				if (!transpile((char *)cli.sources[i], (char *)cli.output))
					die("Transpilation failed");
				continue;
			}
			if (!transpile_to_stdout((char *)cli.sources[i])) die("Transpilation failed");
		}
	} else if (cli.source_count == 0)
		status = passthrough_cc(&cli);
	else
		status = compile_sources(&cli);
	cli_free(&cli);
	return status;
}

#endif // PRISM_LIB_MODE

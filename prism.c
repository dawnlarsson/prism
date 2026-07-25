#define PRISM_VERSION "1.1.5"

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

static char **build_clean_environ(void);
static const char *path_basename(const char *path);
static void signal_temps_register(const char *path);

#include "parse.c"

static int run_command(char **argv);
static int run_command_quiet(char **argv);

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#define OUT_BUF_SIZE (128 * 1024)

#define is_raw(t) ((t)->flags & TF_RAW)

#define emit_defers(mode) emit_defers_ex(mode, 0)
#define emit_all_defers() emit_defers(DEFER_ALL)
#define emit_goto_defers(depth) emit_defers_ex(DEFER_TO_DEPTH, depth)
#define has_active_defers() has_defers_for(DEFER_ALL, 0)
#define control_flow_has_defers(include_switch)                                                              \
	has_defers_for((include_switch) ? DEFER_BREAK : DEFER_CONTINUE, 0)
#define goto_has_defers(depth) has_defers_for(DEFER_TO_DEPTH, depth)
#define typedef_add(name, len, depth, is_vla, is_void)                                                       \
	typedef_add_entry(name, len, depth, TDK_TYPEDEF, is_vla, is_void)
#define typedef_add_shadow(name, len, depth) typedef_add_entry(name, len, depth, TDK_SHADOW, false, false)
#define typedef_add_enum_const(name, len, depth)                                                             \
	typedef_add_entry(name, len, depth, TDK_ENUM_CONST, false, false)
#define typedef_add_vla_var(name, len, depth) typedef_add_entry(name, len, depth, TDK_VLA_VAR, true, false)
#define TYPEDEF_ADD_IDX(call, t)                                                                             \
	do {                                                                                                 \
		int _pre = typedef_table.count;                                                              \
		call;                                                                                        \
		if (typedef_table.count > _pre)                                                              \
			typedef_table.entries[typedef_table.count - 1].token_index = tok_idx(t);             \
	} while (0)

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
	char *error_msg; // error message (NULL on success)
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
	Token *stmt, *end, *defer_kw;
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

typedef struct {
	Token *body_open;	      // The '{' token
	Token *ret_type_start;	      // First token of return type
	Token *ret_type_end;	      // Function name token (exclusive)
	Token *ret_type_suffix_start; // For complex declarators: closing ')'
	Token *ret_type_suffix_end;   // Token after suffix (exclusive)
	bool returns_void;
	bool has_computed_goto;	     // Function contains a computed goto (*ptr)
	int entry_start;	     // Start index into p1_entries[] for this function
	int entry_count;	     // Number of P1FuncEntry items for this function
	HashMap defer_name_set;	     // Exact set of captured names (union of all defer bodies)
	HashMap defer_body_captures; // tok_idx(body) → HashMap* (per-body capture sets)
	int *label_hash;	     // Open-addressing hash table: name → entry index (-1=empty)
	int label_hash_mask;	     // Power-of-2 mask for label_hash probing
} FuncMeta;

typedef struct {
	char *name;
	int len;
	uint16_t scope_id;    // Scope where the shadow is declared
	uint32_t token_index; // Pool index of the shadowing declaration
	int prev_index;	      // Chain to previous shadow for same name (-1 = none)
} P1ShadowEntry;

// Phase 1D: per-function entry for labels, gotos, defers, decls, switches,
// cases.
typedef enum { P1K_LABEL, P1K_GOTO, P1K_DEFER, P1K_DECL, P1K_SWITCH, P1K_CASE } P1EntryKind;

/* Decl shape bits baked into P1K_DECL so Pass 2 need not re-classify. */
enum {
	P1DS_EFF_VLA = 1 << 0,
	P1DS_AGG = 1 << 1,
	P1DS_UNION = 1 << 2,
};

/* Zero-init emit recipe (non-init-stmt). Pass 2 may demote MEMSET→AGG brace
 * inside for/if/switch init statements. */
enum {
	P1Z_NONE = 0,
	P1Z_SCALAR = 1, /* emit " = 0" */
	P1Z_AGG = 2,	/* emit " = {0}" */
	P1Z_MEMSET = 3, /* queue delayed memset */
};

typedef struct {
	Token *tok;	      // 8 @ 0
	uint32_t token_index; // 4 @ 8  (tok_idx for sorting in Phase 2)
	uint16_t scope_id;    // 2 @ 12
	uint8_t kind;	      // 1 @ 14 (P1EntryKind value)
	uint8_t _pad;	      // 1 @ 15

	union { // 16 @ 16

		struct {
			char *name;
			int len;
			int exits;
		} label; // P1K_LABEL, P1K_GOTO

		struct {
			bool has_init;
			bool is_vla;
			bool has_raw;
			bool is_static_storage;
			uint8_t shape;	   // P1DS_* bits from classify_decl_shape
			uint8_t zero_kind; // P1Z_* emit recipe
			uint32_t body_close_idx;
		} decl; // P1K_DECL

		struct {
			uint16_t switch_scope_id;
		} kase; // P1K_CASE
	};
} P1FuncEntry; // 32 bytes — two entries per 64-byte cache line

#define func_meta ((FuncMeta *)ctx->p1_func_meta)
#define func_meta_count (ctx->p1_func_meta_count)
#define func_meta_cap (ctx->p1_func_meta_cap)
#define p1_shadows ((P1ShadowEntry *)ctx->p1_shadow_entries)
#define p1_shadow_count (ctx->p1_shadow_count)
#define p1_shadow_cap (ctx->p1_shadow_cap)
#define p1_shadow_map (ctx->p1_shadow_map)
#define p1_entries ((P1FuncEntry *)ctx->p1_func_entries)
#define p1_entry_count (ctx->p1_func_entry_count)
#define p1_entry_cap (ctx->p1_func_entry_cap)

static inline P1FuncEntry *p1_alloc(int knd, uint16_t sid, Token *t) {
	ARENA_ENSURE_CAP(
	    &ctx->main_arena, ctx->p1_func_entries, p1_entry_count, p1_entry_cap, 256, P1FuncEntry);
	P1FuncEntry *e = &p1_entries[p1_entry_count++];
	*e = (P1FuncEntry){.kind = knd, .scope_id = sid, .token_index = tok_idx(t), .tok = t};
	return e;
}

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
static PRISM_THREAD_LOCAL Token *last_emitted = NULL;

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
static PRISM_THREAD_LOCAL int goto_entry_cursor = 0;  // Cursor into entries[] for next P1K_GOTO lookup
static PRISM_THREAD_LOCAL bool p1_file_has_orelse;    // true if any TT_ORELSE token exists in token stream
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
static PRISM_THREAD_LOCAL HashMap p1_func_proto_map;  // file-scope ident followed by '(' → (void*)1

typedef struct {
	char *name;
	int len;
	int block_depth; // scope depth where the shadowing variable was declared
	Token *var_tok;	 // for error reporting
	int defer_idx;	 // which defer it conflicts with
} DeferShadow;

static PRISM_THREAD_LOCAL DeferShadow *defer_shadows = NULL;
static PRISM_THREAD_LOCAL int defer_shadow_count = 0;
static PRISM_THREAD_LOCAL int defer_shadow_cap = 0;

// MSVC /D define buffers (dynamically allocated, freed in prism_thread_cleanup)
static PRISM_THREAD_LOCAL char **pp_define_bufs = NULL;
static PRISM_THREAD_LOCAL int pp_define_bufs_cap = 0;

static Token *emit_expr_to_semicolon(Token *tok);
static Token *
emit_orelse_action(Token *tok, Token *var_name, bool has_const, bool single_eval_lhs, Token *stop_comma);
static Token *emit_return_body(Token *tok, Token *stop);
static Token *try_zero_init_decl(Token *tok);
static Token *find_bare_orelse(Token *tok);
static Token *decl_noise(Token *tok, bool emit);
static Token *walk_balanced(Token *tok, bool emit);
static Token *walk_balanced_orelse(Token *tok);
static Token *try_bounds_checks(Token *t);
static Token *try_typeof_orelse(Token *tok);
static Token *try_bracket_orelse(Token *tok);
static void emit_token_range_orelse(Token *start, Token *end);
static void emit_token_range_nested(Token *start, Token *end, bool with_orelse);
static inline bool is_orelse_value_fallback(Token *after_oe);
#ifdef PRISM_DEBUG
static void check_orelse_in_ctrl_paren(Token *open);
#endif
static Token *handle_sue_body(Token *tok);
static void emit_noise_between_raws(Token *first_raw, Token *last_raw);
static inline Token *try_strip_raw(Token *t);
static inline void out_char(char c);
static inline void out_str(const char *s, int len);
#define OUT_TOK(t) out_str(tok_loc(t), (t)->len)
#define skip_balanced(tok, o, c) skip_balanced_group(tok)
static bool cc_is_msvc(const char *cc);
static inline void ctrl_reset(void);

typedef struct {
	int scope_depth;
	Token *tok;
} P1LabelResult;

static P1LabelResult p1_label_find(Token *tok, int current_func_idx);

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

static inline void clear_func_ret_type(void) {
	ctx->func_ret_type_start = ctx->func_ret_type_end = NULL;
	ctx->func_ret_type_suffix_start = ctx->func_ret_type_suffix_end = NULL;
}

static TypedefEntry *p1_shadow_entry_for_token(Token *t) {
	for (int ix = typedef_get_index(tok_loc(t), t->len); ix >= 0;
	     ix = typedef_table.entries[ix].prev_index) {
		TypedefEntry *e = &typedef_table.entries[ix];
		if (e->token_index == tok_idx(t) && e->is_shadow) return e;
	}
	return NULL;
}

// Phase 1C: record a variable declaration that shadows a typedef name.
static void p1_register_shadow(Token *t, uint16_t scope_id, int brace_depth) {
	ARENA_ENSURE_CAP(
	    &ctx->main_arena, ctx->p1_shadow_entries, p1_shadow_count, p1_shadow_cap, 64, P1ShadowEntry);
	int new_idx = p1_shadow_count++;
	P1ShadowEntry *e = &p1_shadows[new_idx];
	e->name = tok_loc(t);
	e->len = t->len;
	e->scope_id = scope_id;
	e->token_index = tok_idx(t);
	void *prev_val = hashmap_get(&p1_shadow_map, tok_loc(t), t->len);
	e->prev_index = prev_val ? (int)(intptr_t)prev_val - 1 : -1;
	hashmap_put(&p1_shadow_map, tok_loc(t), t->len, (void *)(intptr_t)(new_idx + 1));
	TYPEDEF_ADD_IDX(typedef_add_shadow(tok_loc(t), t->len, brace_depth), t);
}

static void
p1_register_param_shadows(Token *open, Token *close, uint16_t scope_id, int brace_depth, bool check_vla);

static void reset_transpiler_state(void) {
	ctx->scope_depth = 0;
	ctx->block_depth = 0;
	ctx->aggregate_member_nest = 0;
	ctx->last_line_no = 0;
	ctx->ret_counter = 0;
	clear_func_ret_type();
	ctx->last_filename = NULL;
	ctx->last_system_header = false;
	ctx->at_stmt_start = true;
	ctrl_reset();
	ctrl_save_depth = 0;
	last_emitted = NULL;
	out_total_flushed = 0;
	current_func_idx = -1;
	goto_entry_cursor = 0;
	in_defer_emit = false; /* must clear: error longjmp can skip defer_walk restore */
	p1_typedef_annotated = false;
	p1_file_has_orelse = false;
	hashmap_discard(&p1_func_proto_map);
	defer_count = 0;
	defer_shadow_count = 0;
	ctx->bracket_oe_ids = NULL;
	ctx->bracket_oe_count = 0;
	ctx->bracket_oe_cap = 0;
	ctx->bracket_oe_next = 0;
	ctx->bracket_dim_ids = NULL;
	ctx->bracket_dim_count = 0;
	ctx->bracket_dim_cap = 0;
	ctx->bracket_dim_next = 0;
	ctx->typeof_vars = NULL;
	ctx->typeof_var_count = 0;
	ctx->typeof_var_cap = 0;
	ctx->p1_scope_tree = NULL;
	scope_tree_count = 0;
	scope_tree_cap = 0;
	ctx->p1_func_meta = NULL;
	func_meta_count = 0;
	func_meta_cap = 0;
	ctx->p1_shadow_entries = NULL;
	p1_shadow_count = 0;
	p1_shadow_cap = 0;
	hashmap_discard(&p1_shadow_map);
	ctx->p1_func_entries = NULL;
	p1_entry_count = 0;
	p1_entry_cap = 0;
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
	return (f.defer ? F_DEFER : 0) | (f.zeroinit ? F_ZEROINIT : 0) |
	       (f.line_directives ? F_LINE_DIR : 0) | (f.warn_safety ? F_WARN_SAFETY : 0) |
	       (f.flatten_headers ? F_FLATTEN : 0) | (f.orelse ? F_ORELSE : 0) |
	       (f.auto_unreachable ? F_AUTO_UNREACHABLE : 0) | (f.auto_static ? F_AUTO_STATIC : 0) |
	       (f.bounds_check ? F_BOUNDS_CHECK : 0);
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
	if (PRISM_UNLIKELY(out_buf_pos >= OUT_BUF_SIZE)) out_flush();
	out_buf[out_buf_pos++] = c;
}

static inline PRISM_ALWAYS_INLINE void out_str(const char *s, int len) {
	if (PRISM_UNLIKELY(len <= 0)) return;
	if (PRISM_UNLIKELY(out_buf_pos + len >= OUT_BUF_SIZE)) {
		out_str_slow(s, len);
		return;
	}
	prism_memcpy_runtime_sized(out_buf + out_buf_pos, s, (size_t)(len));
	out_buf_pos += len;
}

#if PRISM_MEM_OUT_LIT_STATIC
/* Compile-time string: exact load/store via prism_memcpy_static, no size ladder. */
#define OUT_LIT(s)                                                                                           \
	do {                                                                                                 \
		enum { _prism_lit_n = (int)sizeof(s) - 1 };                                                  \
		if (PRISM_UNLIKELY(out_buf_pos + _prism_lit_n >= OUT_BUF_SIZE))                               \
			out_str_slow((s), _prism_lit_n);                                                     \
		else {                                                                                       \
			prism_memcpy_static(out_buf + out_buf_pos, (s), sizeof(s) - 1);                       \
			out_buf_pos += _prism_lit_n;                                                         \
		}                                                                                            \
	} while (0)
#else
#define OUT_LIT(s) out_str((s), (int)sizeof(s) - 1)
#endif

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
#define TT_NON_EXPR_STMT                                                                                     \
	(TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO | TT_CASE | TT_DEFAULT | TT_IF | TT_LOOP | TT_SWITCH | \
	 TT_STORAGE | TT_TYPEDEF)

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
	HashMap include_map = {0};
	for (int i = 0; i < ctx->input_file_count; i++) {
		File *f = ctx->input_files[i];
		/* Only re-emit headers the TU included directly — nested system
		 * headers (e.g. bits/libc-header-start.h) error if included alone. */
		if (!f->is_system || !f->is_direct_system_include || !f->name) continue;
		const char *base = path_basename(f->name);
		/* Compiler-injected; not a user #include. */
		if (!strcmp(base, "stdc-predef.h")) continue;
		if (strcmp(base, "assert.h")) {
			int len = (int)strlen(f->name);
			if (hashmap_get(&include_map, f->name, len)) continue;
			hashmap_put(&include_map, f->name, len, (void *)1);
		}
		ARENA_ENSURE_CAP(&ctx->main_arena,
				 system_include_list,
				 ctx->system_include_count + 1,
				 system_include_capacity,
				 32,
				 char *);
		system_include_list[ctx->system_include_count++] = f->name;
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
		if (!*names || !*on) error("out of memory");
	}
	(*names)[*n] = malloc((size_t)nlen + 1);
	if (!(*names)[*n]) error("out of memory");
	memcpy((*names)[*n], spec, (size_t)nlen);
	(*names)[*n][nlen] = '\0';
	(*on)[*n] = defined;
	(*n)++;
}

static void emit_consumed_defines(void) {
	bool any = ctx->extra_define_count > 0 || ctx->source_define_count > 0;
	bool user_undef_gnu = false;
	for (int i = 0; i < ctx->extra_compiler_flags_count; i++) {
		const char *f = ctx->extra_compiler_flags[i];
		if (f[0] == '-' && (f[1] == 'D' || f[1] == 'U')) {
			any = true;
			break;
		}
	}

	if (!any && ctx->system_include_count == 0) return;

	char **def_names = NULL;
	bool *def_on = NULL;
	int def_n = 0, def_cap = 0;

	for (int i = 0; i < ctx->extra_define_count; i++)
		emit_consumed_def_upsert(&def_names, &def_on, &def_n, &def_cap, ctx->extra_defines[i], true,
					 &user_undef_gnu);
	for (int i = 0; i < ctx->extra_compiler_flags_count; i++) {
		const char *f = ctx->extra_compiler_flags[i];
		if (f[0] != '-' || (f[1] != 'D' && f[1] != 'U')) continue;
		bool defined = (f[1] == 'D');
		const char *spec = f[2] ? f + 2 : NULL;
		if (!spec && i + 1 < ctx->extra_compiler_flags_count) spec = ctx->extra_compiler_flags[++i];
		if (spec)
			emit_consumed_def_upsert(&def_names, &def_on, &def_n, &def_cap, spec, defined,
						 &user_undef_gnu);
	}

	for (int i = 0; i < def_n; i++) {
		if (def_on[i]) {
			const char *emit = def_names[i];
			for (int j = 0; j < ctx->extra_define_count; j++) {
				const char *s = ctx->extra_defines[j];
				int nlen = (int)strlen(def_names[i]);
				if (!strncmp(s, def_names[i], (size_t)nlen) &&
				    (s[nlen] == '\0' || s[nlen] == '=')) {
					emit = s;
					break;
				}
			}
			for (int j = 0; j < ctx->extra_compiler_flags_count; j++) {
				const char *f = ctx->extra_compiler_flags[j];
				const char *s = NULL;
				if (f[0] == '-' && f[1] == 'D')
					s = f[2] ? f + 2
						 : (j + 1 < ctx->extra_compiler_flags_count
							? ctx->extra_compiler_flags[j + 1]
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

	for (int i = 0; i < ctx->source_define_count; i++) {
		const char *guard = ctx->source_define_guards ? ctx->source_define_guards[i] : NULL;
		if (guard) out_str(guard, strlen(guard));
		emit_define_guarded(ctx->source_defines[i]);
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
	emit_consumed_defines();
	if (ctx->system_include_count == 0) return;
	emit_system_header_diag_push();
	for (int i = 0; i < ctx->system_include_count; i++) {
		OUT_LIT("#include \"");
		out_quoted_path(system_include_list[i]);
		OUT_LIT("\"\n");
	}

	emit_system_header_diag_pop();
	out_char('\n');
}

static void system_includes_reset(void) {
	system_include_list = NULL;
	ctx->system_include_count = 0;
	system_include_capacity = 0;
}

static inline void ctrl_reset(void) {
	ctrl_state = (CtrlState){0};
}

static inline ScopeNode *scope_block_top(void) {
	for (int i = ctx->scope_depth - 1; i >= 0; i--)
		if (is_brace_scope(scope_stack[i].kind)) return &scope_stack[i];
	return NULL;
}

static inline bool in_for_init(void) {
	return ctx->scope_depth > 0 && scope_stack[ctx->scope_depth - 1].kind == SCOPE_FOR_PAREN;
}

static inline bool in_ctrl_paren(void) {
	for (int i = ctx->scope_depth - 1; i >= 0; i--) {
		ScopeKind k = scope_stack[i].kind;
		if (is_brace_scope(k)) return false;
		if (k == SCOPE_FOR_PAREN || k == SCOPE_CTRL_PAREN) return true;
	}
	return false;
}

static inline bool in_struct_body(void) {
	for (int i = ctx->scope_depth - 1; i >= 0; i--) {
		if (scope_stack[i].is_stmt_expr) return false;
		if (is_brace_scope(scope_stack[i].kind) && scope_stack[i].is_struct) return true;
	}
	return false;
}

static inline bool in_generic(void) {
	for (int i = ctx->scope_depth - 1; i >= 0; i--) {
		if (scope_stack[i].kind == SCOPE_GENERIC) return true;
		if (is_brace_scope(scope_stack[i].kind)) return false;
	}
	return false;
}

static void end_statement_after_semicolon(void) {
	ctx->at_stmt_start = true;
	if (ctrl_state.pending && !in_ctrl_paren() && !in_struct_body()) {
		while (defer_shadow_count > 0 &&
		       defer_shadows[defer_shadow_count - 1].block_depth > ctx->block_depth)
			defer_shadow_count--;
		ctrl_reset();
	}
}

static void scope_push_kind(ScopeKind kind) {
	VEC_ENSURE_REALLOC(scope_stack, ctx->scope_depth + 1, scope_stack_cap, 256);
	ScopeNode *s = &scope_stack[ctx->scope_depth];
	*s = (ScopeNode){.kind = kind};
	s->defer_start_idx = defer_count;
	if (kind == SCOPE_BLOCK) ctx->block_depth++;
	ctx->scope_depth++;
}

static void scope_pop(void) {
	if (ctx->scope_depth > 0) {
		ctx->scope_depth--;
		ScopeNode *s = &scope_stack[ctx->scope_depth];
		if (s->kind == SCOPE_BLOCK) {
			while (defer_shadow_count > 0 &&
			       defer_shadows[defer_shadow_count - 1].block_depth >= ctx->block_depth)
				defer_shadow_count--;
			// Stmt-expr scopes must not clear shadows from enclosing scopes.
			if (s->is_stmt_expr && defer_shadow_count < s->saved_defer_shadow_count)
				defer_shadow_count = s->saved_defer_shadow_count;
			ctx->block_depth--;
		}
	}
}

static void defer_add(Token *defer_keyword, Token *start, Token *end) {
	/* Phase 1 p1d_validate_defer owns the common file-scope reject; this
	 * guard is UNCONDITIONAL (not PRISM_DEBUG-only) because a defer that
	 * reaches Pass 2 at depth 0 through an unclassified context (e.g. a
	 * file-scope initializer) would otherwise register silently and its
	 * body would never be emitted — a token-dropping miscompile.  One
	 * comparison per defer statement; found by the contexts suite. */
	if (ctx->block_depth <= 0) error_tok(start, "defer outside of any scope");
	VEC_ENSURE_REALLOC(defer_stack, defer_count + 1, defer_stack_cap, 64);
	defer_stack[defer_count++] = (DeferEntry){start, end, defer_keyword};
}

static inline bool emit_newline_before_decl_after_stmt_boundary(Token *prev, Token *tok) {
	if (!prev || !tok) return false;
	/* Cheapest, most selective test first: only a `;`/`}` predecessor can
	 * trigger this. Rejecting here avoids in_struct_body()'s scope-stack walk
	 * on the ~95% of tokens that don't follow a statement boundary. */
	if (!(match_ch(prev, ';') || match_ch(prev, '}'))) return false;
	// Member declarations in struct/union/enum bodies use `;` between specifiers;
	if (ctx->aggregate_member_nest > 0) return false;
	if (in_struct_body()) return false;
	if (tok_at_bol(tok)) return false;
	return (tok->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_STORAGE | TT_TYPEDEF | TT_INLINE |
			    TT_TYPEOF | TT_ALIGNAS | TT_BITINT)) ||
	       is_type_keyword(tok) || is_known_typedef(tok) || is_c23_attr(tok) ||
	       ((tok->tag & TT_ATTR) && tok_next(tok) && match_ch(tok_next(tok), '('));
}

static PRISM_HOT void emit_tok(Token *tok) {
	uint32_t feat = ctx->features;
	if (__builtin_expect(!(feat & F_FLATTEN) && (tok->flags & TF_SYS_SKIP), 0)) return;
	TokenCold *c = tok_cold(tok);
	File *f = (c->file_idx < (uint32_t)ctx->input_file_count) ? ctx->input_files[c->file_idx]
								  : ctx->current_file;
	char *loc = f->contents + c->loc_offset;
	Token *prev_emitted = last_emitted;
	bool need_line = false;
	char *tok_fname = NULL;
	int line_no = 0;
	if (feat & F_LINE_DIR) {
		line_no = c->line_no;
		tok_fname = f->name;
		need_line = (ctx->last_filename != tok_fname) || (f->is_system != ctx->last_system_header) ||
			    (line_no != ctx->last_line_no && line_no != ctx->last_line_no + 1);
	}

	if (tok_at_bol(tok) || need_line || emit_newline_before_decl_after_stmt_boundary(last_emitted, tok))
		out_char('\n');
	else if ((tok->flags & TF_HAS_SPACE) || needs_space(last_emitted, tok))
		out_char(' ');
	if (need_line) {
		out_line(line_no, tok_fname, f->is_system);
		ctx->last_filename = tok_fname;
		ctx->last_system_header = f->is_system;
	}

	ctx->last_line_no = line_no;
	if (__builtin_expect(tok->kind == TK_PREP_DIR, 0)) {
		if (!tok_at_bol(tok)) out_char('\n');
		if ((feat & F_FLATTEN) && tok->len >= 8 && loc[0] == '#') {
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
	if (prev_emitted && match_ch(tok, '{') && (prev_emitted->tag & TT_SUE)) ctx->aggregate_member_nest++;
	else if (match_ch(tok, '}') && ctx->aggregate_member_nest > 0)
		ctx->aggregate_member_nest--;
}

static inline Token *emit_tok_checked(Token *t) {
	Token *r = try_strip_raw(t);
	if (r) return r;
	emit_tok(t);
	return NULL;
}

static inline Token *emit_advance(Token *t) {
	Token *r = emit_tok_checked(t);
	return r ? r : tok_next(t);
}

static bool type_range_any(Token *start, Token *end, bool (*pred)(Token *)) {
	for (Token *t = start; t && t != end; t = tok_next(t))
		if (pred(t)) return true;
	return false;
}

static bool is_vol_or_vol_member_td(Token *t) {
	return is_volatile_typedef(t) || has_volatile_member_typedef(t);
}

static bool is_const_td(Token *t) {
	return is_const_typedef(t);
}

static void emit_ll_temp(void (*emit_id)(unsigned), unsigned id) {
	OUT_LIT(" long long");
	emit_id(id);
	OUT_LIT(" = (");
}

static Token *emit_c23_attr(Token *t) {
	Token *bclose = tok_match(t);
	t = emit_advance(t);
	emit_token_range_nested(t, bclose, false);
	emit_tok(bclose);
	return tok_next(bclose);
}

static void emit_orelse_ternary(Token *lhs_start, Token *orelse, Token *rhs_start, Token *rhs_end) {
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

static void emit_prism_oe_ternary_ref(unsigned oe) {
	emit_prism_oe(oe);
	OUT_LIT(" ?");
	emit_prism_oe(oe);
	OUT_LIT(" : (");
}

static void emit_prism_oe_chain_assign(unsigned oe) {
	emit_prism_oe(oe);
	OUT_LIT(" =");
	emit_prism_oe(oe);
	OUT_LIT(" ?");
	emit_prism_oe(oe);
	OUT_LIT(" : (");
}

static Token *find_depth0_token(Token *start, Token *end, bool want_comma) {
	Token *hit = NULL;
	int d = 0;
	for (Token *s = start; s != end; s = tok_next(s)) {
		if (s->flags & TF_OPEN) d++;
		else if (s->flags & TF_CLOSE)
			d--;
		else if (d == 0) {
			if (want_comma) {
				if (match_ch(s, ',')) hit = s;
			} else if (is_assignment_operator_token(s)) {
				if (!match_ch(s, '='))
					error_tok(
					    s,
					    "bare assignment with 'orelse' cannot use compound operators "
					    "(e.g. +=, -=); use a plain '=' assignment");
				hit = s;
			}
		}
	}
	return hit;
}

static Token *last_depth0_comma(Token *start, Token *end) {
	return find_depth0_token(start, end, true);
}

static Token *find_depth0_assign_eq(Token *start, Token *end) {
	return find_depth0_token(start, end, false);
}

static void for_each_enum_constant(Token *brace, void (*fn)(Token *, void *), void *ud) {
	Token *end = tok_match(brace);
	if (!end) return;
	for (Token *t = tok_next(brace); t && t != end && t->kind != TK_EOF;) {
		if (t->kind == TK_IDENT || t->kind == TK_KEYWORD) {
			fn(t, ud);
			while (t && t != end && t->kind != TK_EOF && !match_ch(t, ',')) {
				if ((t->flags & TF_OPEN) && tok_match(t)) {
					t = tok_next(tok_match(t));
					continue;
				}
				t = tok_next(t);
			}
			if (t && match_ch(t, ',')) t = tok_next(t);
		} else {
			t = tok_next(t);
		}
	}
}

static inline Token *emit_gnu_label_decl(Token *tok) {
	if (!ctx->at_stmt_start || !is_gnu_label_decl_head(tok)) return NULL;
	for (Token *t = tok; t && t->kind != TK_EOF; t = tok_next(t)) {
		emit_tok(t);
		if (match_ch(t, ';')) {
			end_statement_after_semicolon();
			return tok_next(t);
		}
	}
	return NULL;
}

#define ER_SKIP_PREP 1 // Skip TK_PREP_DIR tokens
#define ER_BALANCED 2  // Use walk_balanced for paren/bracket groups (not just stmt-expr)

static void emit_range_ex(Token *start, Token *end, int flags) {
	Token *t = start;
	while (t && t != end && t->kind != TK_EOF) {
		if ((flags & ER_SKIP_PREP) && t->kind == TK_PREP_DIR) {
			t = tok_next(t);
			continue;
		}
		if ((flags & ER_BALANCED) && (t->flags & TF_OPEN) && match_set(t, CH('(') | CH('['))) {
			walk_balanced(t, true);
			t = tok_next(tok_match(t));
			continue;
		}
		// C23 [[...]]: Phase 1D rejects 'orelse' inside attribute arguments;
		if ((t->flags & TF_C23_ATTR) && tok_match(t)) {
			t = emit_c23_attr(t);
			continue;
		}
		if (is_stmt_expr_open(t) && tok_match(t)) {
			walk_balanced(t, true);
			t = tok_next(tok_match(t));
			continue;
		}
		// Defense-in-depth: typeof(expr orelse val) → typeof(ternary)
		if (FEAT(F_ORELSE) && (t->tag & TT_TYPEOF)) {
			Token *next = try_typeof_orelse(t);
			if (next) {
				t = next;
				continue;
			}
		}
		/* Subscripts in copied ranges (orelse single-eval LHS duplicates,
		 * fallback values, return bodies) get the same -fbounds-check
		 * wrapping as the main loop — `v[i] = g() orelse 1;` must not
		 * store through a tracked array unchecked.  try_bounds_checks
		 * carries its own applicability and idempotence guards.  Found
		 * by the contexts suite's fixed-point oracle. */
		{
			Token *bc = try_bounds_checks(t);
			if (bc && bc != t) {
				t = bc;
				continue;
			}
		}
		t = emit_advance(t);
	}
}

#define emit_range(start, end) emit_range_ex(start, end, 0)
#define emit_range_no_prep(start, end) emit_range_ex(start, end, ER_SKIP_PREP)
#define emit_balanced_range(start, end) emit_range_ex(start, end, ER_SKIP_PREP | ER_BALANCED)

static Token *emit_bare_orelse_impl(Token *t, Token *end, bool comma_term, bool brace_wrap);
static Token *emit_deferred_orelse(Token *t, Token *end);
static void emit_deferred_range(Token *start, Token *end);

static bool defer_walk(DeferEmitMode mode, int stop_depth, bool dry_run) {
	if (ctx->block_depth <= 0) return false;
	if (!dry_run && in_defer_emit) return false;
	bool saved_in_defer = in_defer_emit;
	if (!dry_run) in_defer_emit = true;
	int current_defer = defer_count - 1;
	int curr_bd = ctx->block_depth;
	bool found = false;
	int min_defer_idx = defer_count;
	for (int d = ctx->scope_depth - 1; d >= 0; d--) {
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
				if (FEAT(F_AUTO_UNREACHABLE) && defer_stack[i].stmt &&
				    !match_ch(defer_stack[i].stmt, '{')) {
					Token *nr = try_detect_noreturn_call(defer_stack[i].stmt);
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
					error_tok(sh->var_tok,
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

// Phase 1 capture analysis: compute the exact set of externally-captured
static void
defer_body_populate_captures(Token *body, Token *body_end, HashMap *out, HashMap *body_captures_map) {
	HashMap *body_set = arena_alloc(&ctx->main_arena, sizeof(HashMap));
	*body_set = (HashMap){0};
	char tmp[16];
	int idx_len = snprintf(tmp, sizeof(tmp), "%u", tok_idx(body));
	char *idx_key = arena_alloc(&ctx->main_arena, idx_len + 1);
	memcpy(idx_key, tmp, idx_len + 1);
	hashmap_put(body_captures_map, idx_key, idx_len, body_set);
	HashMap local_decls = {0};

	typedef struct {
		char *name;
		int len;
		int depth;
		void *prev_val;
		bool had_binding;
	} ScopeDecl;

	int se_cap = 256, se_count = 0;
	ScopeDecl *se_stack = arena_alloc(&ctx->main_arena, se_cap * sizeof(ScopeDecl));
	HashMap for_scopes = {0};
	Token *prev = NULL;
	int bd = 0, pd = 0;
	int block_base_pd[256]; // base paren depth when each block scope opened
	block_base_pd[0] = 0;
	bool in_decl = false, was_in_decl = false;
	int decl_bd = 0, for_init_pd = -1;
	Token *for_header_open = NULL;
	int enum_bd = -1;	   // brace depth of enum body (-1 = not in enum)
	bool decl_is_enum = false; // current in_decl triggered by enum keyword

	for (Token *t = body; t && t != body_end && t->kind != TK_EOF; prev = t, t = tok_next(t)) {
		if (match_ch(t, '{')) {
			if (bd < 255) block_base_pd[bd + 1] = pd;
			if (in_decl && decl_is_enum && enum_bd < 0) enum_bd = bd + 1;
			bd++;
			continue;
		}
		if (match_ch(t, '}')) {
			bd--;
			if (enum_bd >= 0 && bd < enum_bd) enum_bd = -1;
			if (in_decl && bd < decl_bd) in_decl = false;
			while (se_count > 0 && se_stack[se_count - 1].depth > bd) {
				se_count--;
				ScopeDecl *sd = &se_stack[se_count];
				void *val = hashmap_get(&local_decls, sd->name, sd->len);
				if (val && val != (void *)1) {
					int stored = (int)((intptr_t)val - 2);
					if (stored == sd->depth) {
						if (sd->had_binding)
							hashmap_put(
							    &local_decls, sd->name, sd->len, sd->prev_val);
						else
							hashmap_remove(&local_decls, sd->name, sd->len);
					}
				}
			}
			continue;
		}
		if (match_set(t, CH('(') | CH('['))) {
			if (match_ch(t, '(') && prev && prev->kind == TK_KEYWORD &&
			    ((prev->tag & TT_LOOP) || (prev->tag & TT_IF) || (prev->tag & TT_SWITCH))) {
				for_init_pd = pd + 1;
				for_header_open = t;
			}
			pd++;
			continue;
		}
		if (match_set(t, CH(')') | CH(']'))) {
			pd--;
			if (for_init_pd >= 0 && pd < for_init_pd) for_init_pd = -1;
			continue;
		}
		if (match_ch(t, ';')) {
			in_decl = false;
			was_in_decl = false;
			decl_is_enum = false;
			if (for_init_pd >= 0 && pd == for_init_pd) for_init_pd = -1;
			continue;
		}
		int bpd = (bd > 0) ? block_base_pd[bd] : 0;
		if (pd == bpd && match_ch(t, '=')) {
			in_decl = false;
			continue;
		}
		if (pd == bpd && match_ch(t, ',') && was_in_decl &&
		    (bd == decl_bd || (enum_bd >= 0 && bd == enum_bd))) {
			in_decl = true;
			continue;
		}
		if (((bd > 0 && pd == bpd) || (for_init_pd >= 0 && pd == for_init_pd)) &&
		    (is_type_keyword(t) || (t->tag & (TT_QUALIFIER | TT_SUE | TT_STORAGE | TT_TYPEDEF)))) {
			if (!in_decl) decl_is_enum = false;
			in_decl = true;
			was_in_decl = true;
			decl_bd = bd;
			if ((t->tag & TT_SUE) && t->ch0 == 'e') decl_is_enum = true;
			continue;
		}
		if ((t->kind == TK_IDENT || t->kind == TK_KEYWORD) && !(prev && (prev->tag & TT_MEMBER))) {
			char *name = tok_loc(t);
			int nlen = t->len;
			void *val = hashmap_get(&local_decls, name, nlen);
			if (val == (void *)1) continue;
			void *fv = hashmap_get(&for_scopes, name, nlen);
			if (fv) {
				uint32_t fe = (uint32_t)(uintptr_t)fv;
				if (fe == 0 || tok_idx(t) <= fe) continue; // hidden by for-init
				hashmap_put(&for_scopes, name, nlen, NULL);
			}
			if (val) {
				int dd = (int)((intptr_t)val - 2);
				if (bd > 0 && dd >= 0 && bd >= dd) continue;
			}
			if (bd > 0 && in_decl && pd == bpd) {
				void *prev_val = hashmap_get(&local_decls, name, nlen);
				if (se_count >= se_cap) {
					int new_cap = se_cap * 2;
					ScopeDecl *ns =
					    arena_alloc(&ctx->main_arena, new_cap * sizeof(ScopeDecl));
					memcpy(ns, se_stack, se_count * sizeof(ScopeDecl));
					se_stack = ns;
					se_cap = new_cap;
				}
				se_stack[se_count++] =
				    (ScopeDecl){name, nlen, bd, prev_val, prev_val != NULL};
				hashmap_put(&local_decls, name, nlen, (void *)((intptr_t)(bd + 2)));
				continue;
			}
			if (for_init_pd >= 0 && in_decl) {
				uint32_t fbe = 0;
				if (for_header_open && tok_match(for_header_open)) {
					Token *end = skip_one_stmt(tok_next(tok_match(for_header_open)));
					fbe = end ? tok_idx(end) : 0;
				}
				hashmap_put(&for_scopes, name, nlen, (void *)(uintptr_t)fbe);
				continue;
			}
			hashmap_put(out, name, nlen, (void *)1);
			hashmap_put(body_set, name, nlen, (void *)1);
			hashmap_put(&local_decls, name, nlen, (void *)1);
		}
	}
}

static bool defer_body_has_capture(int func_idx, Token *body, const char *name, int nlen) {
	if (func_idx < 0) return false;
	char idx_buf[16];
	int idx_len = snprintf(idx_buf, sizeof(idx_buf), "%u", tok_idx(body));
	HashMap *body_set = hashmap_get(&func_meta[func_idx].defer_body_captures, idx_buf, idx_len);
	return body_set && hashmap_get(body_set, (char *)name, nlen);
}

static void check_defer_var_shadow(Token *var_name) {
	if (!FEAT(F_DEFER) || defer_count == 0) return;
	ScopeNode *blk = scope_block_top();
	if (!blk) return;
	int outer_defer_end = blk->defer_start_idx;
	if (in_for_init() && outer_defer_end <= 0) outer_defer_end = defer_count;
	int same_block_start = blk->defer_start_idx;
	if (outer_defer_end <= 0 && same_block_start >= defer_count) return;
	char *name = tok_loc(var_name);
	int nlen = var_name->len;
	if (current_func_idx >= 0 && !hashmap_get(&func_meta[current_func_idx].defer_name_set, name, nlen))
		return;
	for (int i = 0; i < defer_count; i++) {
		if (i >= outer_defer_end && i < same_block_start) continue;
		uint32_t var_idx = tok_idx(var_name);
		uint32_t stmt_idx = tok_idx(defer_stack[i].stmt);
		uint32_t end_idx = defer_stack[i].end ? tok_idx(defer_stack[i].end) : UINT32_MAX;
		if (var_idx >= stmt_idx && var_idx < end_idx) continue;
		if (!defer_body_has_capture(current_func_idx, defer_stack[i].stmt, name, nlen)) continue;
		if (defer_shadow_count >= defer_shadow_cap) {
			int new_cap = defer_shadow_cap ? defer_shadow_cap * 2 : 64;
			void *tmp = realloc(defer_shadows, new_cap * sizeof(*defer_shadows));
			if (!tmp) error("out of memory");
			defer_shadows = tmp;
			defer_shadow_cap = new_cap;
		}
		/* Phase 1 p1_check_defer_same_block_shadow owns the same-block reject. */
#ifdef PRISM_DEBUG
		if (i >= same_block_start && !in_for_init())
			error_tok(var_name, ERR_DEFER_SHADOW_SAME_SCOPE, nlen, name);
#endif
		defer_shadows[defer_shadow_count++] = (DeferShadow){
		    .name = name,
		    .len = nlen,
		    .block_depth = ctx->block_depth + (in_for_init() ? 1 : 0),
		    .var_tok = var_name,
		    .defer_idx = i,
		};
		return;
	}
}

// Called from Pass 2 at statement-start for enum definitions and typedef
// declarations
static void enum_pass2_shadow_cb(Token *t, void *ud) {
	(void)ud;
	check_defer_var_shadow(t);
}

static void check_enum_body_defer_shadow(Token *brace) {
	for_each_enum_constant(brace, enum_pass2_shadow_cb, NULL);
}

static void check_enum_typedef_defer_shadow(Token *tok) {
	if (!FEAT(F_DEFER) || defer_count == 0 || ctx->block_depth <= 0) return;
	if (tok->tag & TT_SUE) {
		if (!is_enum_kw(tok)) return;
		Token *brace = find_struct_body_brace(tok);
		if (brace) check_enum_body_defer_shadow(brace);
		return;
	}

	if (tok->tag & TT_TYPEDEF) {
		Token *type_start = tok_next(tok);
		if (!type_start) return;
		TypeSpecResult type = parse_type_specifier(type_start);
		if (!type.saw_type) return;
		for (Token *s = type_start; s && s != type.end; s = tok_next(s)) {
			if (is_enum_kw(s)) {
				Token *brace = find_struct_body_brace(s);
				if (brace) check_enum_body_defer_shadow(brace);
			}
		}
		Token *t = type.end;
		while (t && t->kind != TK_EOF && !match_ch(t, ';')) {
			DeclResult decl = parse_declarator(t, false);
			if (decl.var_name) check_defer_var_shadow(decl.var_name);
			if (!decl.end) break;
			t = decl.end;
			if (match_ch(t, ',')) t = tok_next(t);
			else
				break;
		}
		return;
	}
}

static inline bool is_known_function_call(Token *tok);
static inline bool is_empty_known_function_call(Token *tok);
static Token *function_declarator_param_open(Token *tok);

static inline bool is_defer_kw(Token *tok, Token *prev) {
	if (!(tok->tag & TT_DEFER)) return false;
	Token *nx = tok_next(tok);
	/* `defer()` empty-call spelling — not a statement keyword. */
	if (nx && match_ch(nx, '(') && is_empty_known_function_call(tok)) return false;
	/* Typedef *type* named `defer` stays an identifier except `defer {…}`
	 * (unambiguous keyword). Variable/param/enum-constant shadows
	 * (`is_shadow`) must still allow braceless `defer stmt;` — otherwise the
	 * keyword leaks to the backend. */
	TypedefEntry *te = typedef_lookup(tok);
	if (te && !te->is_shadow && !(nx && match_ch(nx, '{'))) return false;
	/* `defer {…}` cannot be a parameter name — skip the param-list walk. */
	if (!(nx && match_ch(nx, '{')) && function_declarator_param_open(tok)) return false;
	/* Type / declarator predecessor → identifier (mirror: `int defer`, `T *defer`). */
	if (prev && ((prev->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_TYPEDEF | TT_SUE | TT_TYPEOF |
				   TT_BITINT)) ||
		     is_known_typedef(prev) || match_ch(prev, '*')))
		return false;
	/* Label / member / &&label → identifier. */
	if (prev && ((prev->tag & (TT_GOTO | TT_MEMBER)) || is_gnu_label_decl_head(prev) ||
		     _equal_2(prev, "&&")))
		return false;
	/* Expression-position predecessor: `return defer`, `f(defer`, `x=defer`,
	 * `a[defer`, `sizeof defer`, `!defer`, cast `) defer`, ternary `: defer`.
	 * Real statements follow `;` `{` `}` / stmt-start (unary `+x` after boundary OK). */
	if (prev && (prev->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO | TT_ASSIGN)))
		return false;
	if (prev && is_sizeof_like(prev)) return false;
	if (prev && match_ch(prev, '(')) return false;
	if (prev && match_ch(prev, '[')) return false;
	if (prev && match_ch(prev, ',')) return false;
	/* Cast / call-close `) defer` is expression; `if (…)` / `while` / `for` /
	 * `switch` close is statement position (`if (1) defer …`). */
	if (prev && match_ch(prev, ')')) {
		Token *open = tok_match(prev);
		Token *before = open ? tok_walk_back(tok_idx(open), WB_PAST_NOISE) : NULL;
		if (!(before && (before->tag & (TT_IF | TT_LOOP | TT_SWITCH)))) return false;
	}
	/* Unary expr predecessors: `!defer` `~defer` `&defer` (address-of). */
	if (prev && prev->kind == TK_PUNCT && prev->len == 1) {
		unsigned char pc = (unsigned char)prev->ch0;
		if (pc == '!' || pc == '~' || pc == '&') return false;
	}
	/* Ternary `: defer` is expression; label/case `:` stays statement position. */
	if (prev && match_ch(prev, ':')) {
		int qd = 0;
		bool ternary = false;
		for (uint32_t pi = tok_idx(prev); pi > 0;) {
			pi--;
			Token *pt = &token_pool[pi];
			if (pt->kind == TK_PREP_DIR) continue;
			if ((pt->flags & TF_CLOSE) && tok_match(pt)) {
				pi = tok_idx(tok_match(pt));
				continue;
			}
			if (match_ch(pt, ';') || match_ch(pt, '{') || match_ch(pt, '}')) break;
			if (pt->tag & (TT_CASE | TT_DEFAULT)) break;
			if (match_ch(pt, '?') && qd == 0) {
				ternary = true;
				break;
			}
		}
		if (ternary) return false;
	}
	/* Infix binary op → identifier (`x + defer + y`). Unary `+`/`-` after a
	 * statement boundary still allows braceless `defer +x;`. */
	if (prev && prev->kind == TK_PUNCT) {
		unsigned char pc0 = (unsigned char)prev->ch0;
		if (prev->len == 1) {
			if (pc0 == '+' || pc0 == '-' || pc0 == '/' || pc0 == '%' || pc0 == '|' ||
			    pc0 == '^' || pc0 == '<' || pc0 == '>' || pc0 == '?' || pc0 == '=')
				return false;
		} else if (prev->len == 2) {
			char pc1 = tok_loc(prev)[1];
			if ((pc0 == '<' && (pc1 == '=' || pc1 == '<')) ||
			    (pc0 == '>' && (pc1 == '=' || pc1 == '>')) || (pc0 == '=' && pc1 == '=') ||
			    (pc0 == '!' && pc1 == '=') || (pc0 == '&' && pc1 == '&') ||
			    (pc0 == '|' && pc1 == '|') || (pc0 == '+' && pc1 == '=') ||
			    (pc0 == '-' && pc1 == '=') || (pc0 == '*' && pc1 == '=') ||
			    (pc0 == '/' && pc1 == '=') || (pc0 == '%' && pc1 == '=') ||
			    (pc0 == '&' && pc1 == '=') || (pc0 == '|' && pc1 == '=') ||
			    (pc0 == '^' && pc1 == '='))
				return false;
		} else if (prev->len == 3) {
			char *pl = tok_loc(prev);
			if ((pc0 == '<' && pl[1] == '<' && pl[2] == '=') ||
			    (pc0 == '>' && pl[1] == '>' && pl[2] == '='))
				return false;
		}
	}
	if (!nx || match_ch(nx, ':') || (nx->tag & TT_ASSIGN)) return false;
	/* `defer;` is an empty defer statement unless a variable/enum shadow owns
	 * the name — then it is an expression-statement of that identifier. */
	if (match_ch(nx, ';')) return !(te && (te->is_shadow || te->is_enum_const));
	/* Call-arg / asm-goto / brace-init: `f(defer)`, `{ defer }`, `{ defer, }`. */
	if (match_ch(nx, ')') || match_ch(nx, ']') || match_ch(nx, ',') || match_ch(nx, '}'))
		return false;
	/* Subscript / member on the name: `defer[i]`, `defer.x`, `defer->x`. */
	if (match_ch(nx, '[') || (nx->tag & TT_MEMBER)) return false;
	/* Postfix `defer++` / `defer--` vs braceless body `defer ++x;`. */
	if (nx->len == 2 && (nx->ch0 == '+' || nx->ch0 == '-') && tok_loc(nx)[1] == nx->ch0) {
		Token *after = tok_next(nx);
		if (!after || match_ch(after, ';') || match_ch(after, ')') || match_ch(after, ']') ||
		    match_ch(after, ',') || match_ch(after, ':') || (after->tag & TT_ASSIGN))
			return false;
	}
	/* Infix punctuators cannot start a statement: `for (; defer < 3; )`.
	 * Unary `&` `!` `*` `+` `-` `~` still can (`defer &x;`). */
	if (nx->kind == TK_PUNCT) {
		unsigned char c0 = (unsigned char)nx->ch0;
		if (nx->len == 1) {
			if (c0 == '<' || c0 == '>' || c0 == '?' || c0 == '%' || c0 == '^' || c0 == '|' ||
			    c0 == '/')
				return false;
		} else if (nx->len == 2) {
			char c1 = tok_loc(nx)[1];
			if ((c0 == '<' && (c1 == '=' || c1 == '<')) ||
			    (c0 == '>' && (c1 == '=' || c1 == '>')) || (c0 == '=' && c1 == '=') ||
			    (c0 == '!' && c1 == '=') || (c0 == '&' && c1 == '&') || (c0 == '|' && c1 == '|'))
				return false;
		}
	}
	return true;
}

static inline bool token_is_label_name(Token *tok) {
	Token *colon = tok ? skip_noise(tok_next(tok)) : NULL;
	return tok && is_identifier_like(tok) && colon && match_ch(colon, ':') &&
	       !(tok_next(colon) && match_ch(tok_next(colon), ':')) && !(tok->tag & (TT_CASE | TT_DEFAULT));
}

static inline bool token_can_name_function(Token *tok) {
	return tok && (tok->kind == TK_IDENT || (tok->tag & (TT_DEFER | TT_ORELSE)) || (tok->flags & TF_RAW));
}

static inline bool is_known_function_call(Token *tok) {
	if (!tok || !hashmap_get(&p1_func_proto_map, tok_loc(tok), tok->len)) return false;
	Token *next = skip_noise(tok_next(tok));
	return next && match_ch(next, '(');
}

static inline bool is_empty_known_function_call(Token *tok) {
	if (!is_known_function_call(tok)) return false;
	Token *open = skip_noise(tok_next(tok));
	return open && match_ch(open, '(') && tok_match(open) && tok_next(open) == tok_match(open);
}

static bool token_can_precede_function_name(Token *tok) {
	while (tok && (match_ch(tok, '*') || (tok->tag & (TT_QUALIFIER | TT_STORAGE | TT_INLINE))))
		tok = tok_walk_back(tok_idx(tok), WB_PAST_NOISE);
	return tok && ((tok->tag & (TT_TYPE | TT_SUE | TT_TYPEOF | TT_BITINT)) || is_known_typedef(tok));
}

static bool token_can_start_knr_param_decl(Token *tok) {
	return tok && ((tok->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_INLINE | TT_TYPEDEF | TT_SUE |
				    TT_TYPEOF | TT_BITINT)) ||
		       is_known_typedef(tok));
}

static bool paren_is_function_declarator_params(Token *open) {
	if (!open || !match_ch(open, '(') || !tok_match(open)) return false;
	Token *close = tok_match(open);
	Token *after = skip_asm_specifier_trail(tok_next(close));
	if (!after ||
	    !(match_ch(after, '{') || match_ch(after, ';') || match_ch(after, ',') || match_ch(after, '=') ||
	      match_ch(after, ')') || token_can_start_knr_param_decl(after)))
		return false;
	/* WB_ATTR_NOISE — not WB_PAST_NOISE — so `(*F)(…)` keeps the `)` as
	 * prev. JUMP_GROUPS would hop the paren-pointer group and land on the
	 * type keyword, making the )( branch dead and missing typedef/funcptr
	 * parameter dims (`typedef int (*F)(int a[0 orelse 1])`). */
	Token *prev = tok_walk_back(tok_idx(open), WB_ATTR_NOISE);
	if (prev && token_can_name_function(prev)) {
		Token *before = tok_walk_back(tok_idx(prev), WB_ATTR_NOISE);
		return token_can_precede_function_name(before);
	}
	if (prev && match_ch(prev, ')') && tok_match(prev)) {
		Token *before = tok_walk_back(tok_idx(tok_match(prev)), WB_ATTR_NOISE);
		return token_can_precede_function_name(before);
	}
	return false;
}

static Token *function_declarator_param_open(Token *tok) {
	if (!tok) return NULL;
	int depth = 0;
	for (uint32_t i = tok_idx(tok); i > 0; i--) {
		Token *t = &token_pool[i - 1];
		if (t->kind == TK_PREP_DIR) continue;
		if (t->flags & TF_CLOSE) {
			depth++;
			continue;
		}
		if (t->flags & TF_OPEN) {
			if (depth == 0 && match_ch(t, '('))
				return paren_is_function_declarator_params(t) ? t : NULL;
			if (depth > 0) depth--;
			continue;
		}
		if (depth == 0 && (match_ch(t, ';') || match_ch(t, '{') || match_ch(t, '}'))) break;
	}
	return NULL;
}

static inline bool orelse_kw_at(Token *t, Token *prev) {
	if (!(t->tag & TT_ORELSE) || (prev && (prev->tag & TT_MEMBER))) return false;
	if (is_known_function_call(t)) return false;
	/* Soft-keyword predecessor: may be a type specifier (`bool orelse = 0`)
	 * or a value (`_Float32 = _Float32 orelse 2`, `x = asm orelse 2`). */
	if (prev && is_soft_keyword_identifier(prev)) {
		if (soft_keyword_decl_name_boundary(t)) return false;
		if (orelse_is_label_or_goto_target(t, prev)) return false;
		return true;
	}
	TypedefEntry *te = typedef_lookup(t);
	/* Variable/param named orelse: operator only after expression-ending prev.
	 * prev==NULL at stmt-start `orelse;` is the identifier. */
	if (te && te->is_shadow) return orelse_shadow_is_kw(prev);
	/* Typedef named orelse: dual-use — type at stmt start, operator after expr. */
	if (te && !te->is_shadow) return prev && orelse_shadow_is_kw(prev);
	/* No typedef entry. Exclude type-specifier predecessors (`T orelse`,
	 * `_BitInt(N) orelse`, `int orelse(`) and `return orelse` (operand).
	 * Keep keyword after continue/break (TT_SKIP_DECL makes shadow_is_kw
	 * false — mid-chain needs the keyword) and after call `)`. */
	if (prev && ((prev->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_SUE | TT_TYPEOF | TT_BITINT |
				   TT_ALIGNAS | TT_INLINE | TT_ATTR | TT_RETURN)) ||
		     is_known_typedef(prev) || token_ends_sue_type_specifier(prev) ||
		     close_paren_ends_type_specifier_ctor(prev)))
		return false;
	if (!prev) return true;
	/* Prefer shadow_is_kw; also treat break/continue/goto as keyword predecessors
	 * for mid-chain (`… orelse continue orelse …`). */
	if (orelse_shadow_is_kw(prev)) return true;
	if (prev->tag & (TT_BREAK | TT_CONTINUE | TT_GOTO)) return true;
	return false;
}

static inline bool orelse_kw_at_bare(Token *t, Token *prev) {
	return orelse_kw_at(t, prev) && !orelse_is_label_or_goto_target(t, prev) &&
	       !(prev && token_ends_sue_type_specifier(prev));
}

/* Bracket path: require shadow-kw prev even when orelse is not a typedef. */
static inline bool orelse_kw_at_shadow(Token *t, Token *prev) {
	return (t->tag & TT_ORELSE) && !(prev && (prev->tag & TT_MEMBER)) && !is_known_function_call(t) &&
	       orelse_shadow_is_kw(prev);
}

static inline bool is_orelse_keyword(Token *tok) {
	/* Phase 1 sets P1_IS_ORELSE_KW; Pass 2 only runs after annotation. */
	return (tok_ann(tok) & P1_IS_ORELSE_KW) != 0;
}

static bool is_strictly_bare_call(Token *start, Token *end) {
	Token *t = skip_prep_dirs_until(start, end);
	if (!t || t == end || !is_valid_varname(t) || is_type_keyword(t)) return false;
	Token *fn = t;
	t = skip_prep_dirs_until(tok_next(fn), end);
	if (!t || t == end || !match_ch(t, '(') || !(t->flags & TF_OPEN)) return false;
	Token *close = tok_match(t);
	if (!close) return false;
	return skip_prep_dirs_until(tok_next(close), end) == end &&
	       hashmap_get(&p1_func_proto_map, tok_loc(fn), fn->len) != NULL;
}

static void reject_orelse_side_effects(Token *start,
				       Token *end,
				       const char *ctx_msg,
				       const char *advice,
				       bool check_asm,
				       bool check_volatile_deref,
				       bool check_indirect_call) {
	int pd = 0;
	Token *prev_tok = NULL;
	for (Token *s = start; s && s != end && s->kind != TK_EOF; s = tok_next(s)) {
		if (s->flags & TF_OPEN) {
			pd++;
			goto next_checks;
		}
		if (s->flags & TF_CLOSE) {
			pd--;
			goto next_checks;
		}
		if (pd == 0 && match_ch(s, ','))
			error_tok(s,
				  "%s with comma operator at top level (the "
				  "left-hand sub-expression before ',' is evaluated "
				  "twice — double evaluation of volatile reads or "
				  "other side effects) %s",
				  ctx_msg,
				  advice);
	next_checks:
		if (s->tag & (TT_GOTO | TT_RETURN | TT_BREAK | TT_CONTINUE | TT_DEFER))
			error_tok(s,
				  "%s containing control flow keywords "
				  "(cannot duplicate statement expressions with "
				  "goto/return/break/continue/defer) %s",
				  ctx_msg,
				  advice);
		if ((s->len == 2 &&
		     ((s->ch0 == '+' && tok_loc(s)[1] == '+') || (s->ch0 == '-' && tok_loc(s)[1] == '-'))) ||
		    is_assignment_operator_token(s))
			error_tok(s, "%s with side effect %s", ctx_msg, advice);
		if (check_asm && (s->tag & TT_ASM) && !is_soft_keyword_identifier(s))
			error_tok(s, "%s with inline asm %s", ctx_msg, advice);
		if ((is_valid_varname(s) && !is_type_keyword(s)) || match_ch(s, ']') || match_ch(s, ')')) {
			Token *after_s = tok_next(s);
			if (after_s && after_s != end && match_ch(after_s, '('))
				error_tok(s,
					  "%s with %s call %s",
					  ctx_msg,
					  check_indirect_call ? "a function" : "side effect",
					  advice);
		}
		if (check_indirect_call && match_ch(s, '(') && (s->flags & TF_OPEN) && tok_match(s) &&
		    tok_match(s) != end && tok_next(tok_match(s)) && tok_next(tok_match(s)) != end &&
		    match_ch(tok_next(tok_match(s)), '('))
			error_tok(s, "%s with an indirect call %s", ctx_msg, advice);
		if (check_volatile_deref && match_ch(s, '*') && tok_next(s) && tok_next(s) != end) {
			bool is_mul = false;
			if (prev_tok) {
				if (prev_tok->kind == TK_NUM || prev_tok->kind == TK_STR) is_mul = true;
				else if (prev_tok->kind == TK_IDENT && !is_type_keyword(prev_tok))
					is_mul = true;
				else if (match_ch(prev_tok, ']'))
					is_mul = true;
				else if (match_ch(prev_tok, ')') && (prev_tok->flags & TF_CLOSE)) {
					Token *om = tok_match(prev_tok);
					if (om) {
						Token *fi = tok_next(om);
						bool looks_cast =
						    fi && (is_type_keyword(fi) ||
							   (fi->tag & (TT_QUALIFIER | TT_SUE | TT_TYPEOF)));
						if (looks_cast) {
							uint32_t oi = tok_idx(om);
							if (oi >= 2 && token_pool[oi - 1].flags & TF_SIZEOF)
								is_mul = true;
						} else
							is_mul = true;
					}
				}
			}
			if (!is_mul) error_tok(s, "%s with pointer dereference %s", ctx_msg, advice);
		}
		if (check_volatile_deref && (s->tag & TT_MEMBER))
			error_tok(s, "%s with member access operator %s", ctx_msg, advice);
		if (check_volatile_deref && match_ch(s, '[') && (s->flags & TF_OPEN))
			error_tok(s, "%s with array subscript %s", ctx_msg, advice);
		// shadow/typedef table populated in Phase 1D.
		if (check_volatile_deref && is_valid_varname(s) && !is_type_keyword(s)) {
			unsigned tf = typedef_flags(s);
			if (tf & (TDF_VOLATILE | TDF_HAS_VOL_MEMBER))
				error_tok(s, "%s with volatile-qualified identifier %s", ctx_msg, advice);
			if (tf & TDF_ATOMIC)
				error_tok(s, "%s with atomic-qualified identifier %s", ctx_msg, advice);
		}
		prev_tok = s;
	}
}

static inline bool raw_token_is_sue_tag_name(Token *t) {
	if (!t || !(t->flags & TF_RAW)) return false;
	Token *prev = tok_walk_back(tok_idx(t), WB_PAST_NOISE);
	return (prev && (prev->tag & TT_SUE)) || token_ends_sue_type_specifier(t);
}

static void emit_type_range(Token *start, Token *end, bool strip_const, bool strip_sue_body) {
	int raw_depth = 0;
	for (Token *t = start; t && t != end && t->kind != TK_EOF;) {
		if (strip_const && (t->tag & TT_CONST)) {
			t = tok_next(t);
			continue;
		}
		if (is_stmt_expr_open(t) && tok_match(t)) {
			walk_balanced(t, true);
			t = tok_next(tok_match(t));
			continue;
		}
		if (match_ch(t, '{')) raw_depth++;
		if (match_ch(t, '}')) raw_depth--;
		if (raw_depth == 0 && (t->flags & TF_RAW) && !is_known_typedef(t) &&
		    !raw_token_is_sue_tag_name(t)) {
			Token *after = skip_noise(tok_next(t));
			Token *last = t;
			SKIP_RAW(after, last);
			emit_noise_between_raws(t, last);
			t = tok_next(last);
			continue;
		}
		if (FEAT(F_ORELSE) && tok_next(t) && match_ch(tok_next(t), '(') &&
		    ((t->tag & (TT_TYPEOF | TT_BITINT | TT_ALIGNAS)) ||
		     ((t->tag & TT_TYPE) && equal(t, "_Atomic")))) {
			emit_tok(t);
			t = tok_next(t);
			t = walk_balanced_orelse(t);
			continue;
		}
		if (__builtin_expect(FEAT(F_BOUNDS_CHECK), 0)) {
			Token *bc = try_bounds_checks(t);
			if (bc) {
				t = bc;
				continue;
			}
		}
		if (strip_sue_body && match_ch(t, '{')) {
			Token *kw = NULL;
			for (Token *s = start; s != t; s = tok_next(s))
				if (s->tag & TT_SUE) kw = s;
			bool keep = false;
			if (kw && !is_enum_kw(kw)) {
				for (Token *u = tok_next(kw); u && u != t; u = tok_next(u)) {
					if (match_ch(u, ')') && tok_match(u) &&
					    tok_loc(tok_match(u)) < tok_loc(kw)) {
						keep = true;
						break;
					}
				}
				if (!keep) {
					keep = true;
					for (Token *u = tok_next(kw); u && u != t; u = tok_next(u)) {
						if ((u->flags & TF_OPEN) &&
						    (match_ch(u, '(') || (u->flags & TF_C23_ATTR))) {
							u = tok_match(u);
							if (!u || u == t) break;
							continue;
						}
						if (is_valid_varname(u) &&
						    !(u->tag & (TT_QUALIFIER | TT_ATTR | TT_TYPEOF))) {
							keep = false;
							break;
						}
					}
				}
			}
			if (!keep) {
				raw_depth--;
				t = walk_balanced(t, false);
				if (t == end) break;
				continue;
			}
		}
		{
			Token *r = try_strip_raw(t);
			if (r) {
				t = r;
				continue;
			}
		}
		t = emit_advance(t);
	}
}

static Token *emit_expr_to_stop(Token *tok, Token *stop, bool check_orelse) {
	while (tok->kind != TK_EOF) {
		if (tok->flags & TF_OPEN) {
			tok = walk_balanced(tok, true);
			continue;
		}
		if (match_ch(tok, ';') || (stop && tok == stop)) break;
		if (check_orelse && is_orelse_keyword(tok)) break;
		if (FEAT(F_ORELSE) && (tok->tag & TT_TYPEOF)) {
			Token *next = try_typeof_orelse(tok);
			if (next) {
				tok = next;
				continue;
			}
		}
		tok = emit_advance(tok);
	}
	return tok;
}

static Token *try_strip_raw_slow(Token *t) {
	if (raw_token_is_sue_tag_name(t)) return NULL;
	/* Force multiplication in subscripts: arr[raw * x] — never strip `raw`. */
	if (raw_after_subscript_open_bracket(t)) return NULL;
	Token *after = skip_noise(tok_next(t));
	/* Variable / enum-constant shadows named `raw` are identifiers. */
	{
		TypedefEntry *te = typedef_lookup(t);
		if (te && (te->is_shadow || te->is_enum_const || te->is_vla_var)) return NULL;
	}
	/* Typedef named `raw` is the type in `raw x` / `raw *p`. Strip only when
	 * it is a keyword prefix before another type (`raw raw x`, `raw int x`). */
	if (is_known_typedef(t)) {
		if (!(after && (is_type_keyword(after) || is_known_typedef(after) ||
				(after->tag & (TT_QUALIFIER | TT_SUE | TT_STORAGE | TT_INLINE |
					       TT_TYPEDEF)) ||
				(after->flags & TF_RAW))))
			return NULL;
	} else if (p1_typedef_annotated && (tok_ann(t) & P1_HAS_ENTRY)) {
		return NULL;
	}
	if (is_raw_strip_context(t, after)) {
		Token *last = t;
		SKIP_RAW(after, last);
		emit_noise_between_raws(t, last);
		return tok_next(last);
	}
	return NULL;
}

static inline Token *try_strip_raw(Token *t) {
	if (__builtin_expect(!(t->flags & TF_RAW), 1)) return NULL;
	return try_strip_raw_slow(t);
}

/* MSVC / Windows calling-convention keywords between return type and name.
 * Not return-type material — must not be copied onto `__prism_ret_N`. */
static bool is_ms_calling_conv_kw(Token *tok) {
	return tok && (tok->flags & TF_MS_CC);
}

static Token *skip_func_attrs_and_cc(Token *tok) {
	/* skip_noise already eats TT_ATTR / C23 [[...]] / PREP_DIR. */
	while (tok && tok->kind != TK_EOF) {
		SKIP_NOISE_CONTINUE(tok);
		if (is_ms_calling_conv_kw(tok)) {
			tok = tok_next(tok);
			continue;
		}
		break;
	}
	return tok;
}

/* Pointers + pointer-qualifiers only. Do not call skip_noise here — it
 * swallows `__attribute__` / `__declspec`, which would extend the return-type
 * emit range through function attributes up to the declarator name. */
static Token *skip_ret_pointers(Token *tok, bool *is_void) {
	while (tok && tok->kind != TK_EOF) {
		if (tok->kind == TK_PREP_DIR) {
			tok = tok_next(tok);
			continue;
		}
		if (match_ch(tok, '*')) {
			tok = tok_next(tok);
			if (is_void) *is_void = false;
			continue;
		}
		if ((tok->tag & TT_QUALIFIER) && !(tok->tag & TT_ATTR) && !is_soft_keyword_identifier(tok)) {
			tok = tok_next(tok);
			if (is_void) *is_void = false;
			continue;
		}
		break;
	}
	return tok;
}

/* Exclusive end for emit_token_range: drop trailing function attrs / CC
 * keywords that parse_type_specifier may have consumed past the real type. */
static Token *ret_type_end_excluding_trailing_attrs(Token *start, Token *parsed_end) {
	Token *t = start;
	while (t && t != parsed_end && t->kind != TK_EOF) {
		if ((t->tag & TT_ATTR) || is_c23_attr(t) || is_ms_calling_conv_kw(t)) {
			Token *p = t;
			while (p && p != parsed_end) {
				if (p->tag & TT_ATTR) {
					Token *n = tok_next(p);
					if (n && n != parsed_end && match_ch(n, '(') && tok_match(n))
						p = tok_next(tok_match(n));
					else
						p = n;
					continue;
				}
				if (is_c23_attr(p) && tok_match(p)) {
					p = tok_next(tok_match(p));
					continue;
				}
				if (is_ms_calling_conv_kw(p)) {
					p = tok_next(p);
					continue;
				}
				return parsed_end;
			}
			return t;
		}
		if ((t->flags & TF_OPEN) && tok_match(t)) t = tok_next(tok_match(t));
		else
			t = tok_next(t);
	}
	return parsed_end;
}

static int capture_function_return_type(Token *tok) {
	while (tok && tok->kind != TK_EOF) {
		if (tok->tag & (TT_SKIP_DECL | TT_INLINE)) {
			tok = tok_next(tok);
			continue;
		}
		Token *next = skip_noise(tok);
		if (next == tok) break;
		tok = next;
	}
	if (!tok || tok->kind == TK_EOF) return 0;
	Token *type_start = tok;
	TypeSpecResult type = parse_type_specifier(tok);
	if (!type.saw_type) return 0;
	bool is_void = type.has_void;
	Token *trimmed = ret_type_end_excluding_trailing_attrs(type_start, type.end);
	/* If a concrete non-void type keyword appears before any void/typeof,
   * the function is not void — guards mis-set has_void when CC/attr
   * tokens confuse the type walk (MSVC `__cdecl` + defer → dropped value). */
	if (is_void) {
		for (Token *t = type_start; t && t != trimmed && t->kind != TK_EOF; t = tok_next(t)) {
			if (equal(t, "void") || is_void_typedef(t) || (t->tag & TT_TYPEOF)) break;
			if ((t->tag & TT_TYPE) && !equal(t, "void")) {
				is_void = false;
				break;
			}
		}
	}
	if (type.is_struct) {
		for (Token *t = type_start; t && t != trimmed && t->kind != TK_EOF; t = tok_next(t))
			if (match_ch(t, '{')) return 0;
	}

	/* Pointers belong in the return type; function attrs / calling-convention
   * keywords after the pointer chain do not. */
	tok = skip_ret_pointers(trimmed, &is_void);
	Token *ret_end = tok;
	tok = skip_func_attrs_and_cc(tok);
	if (tok && is_valid_varname(tok) && tok_next(tok) && match_ch(tok_next(tok), '(')) {
		if (is_void) return 1;
		ctx->func_ret_type_start = type_start;
		ctx->func_ret_type_end = ret_end;
		ctx->func_ret_type_suffix_start = ctx->func_ret_type_suffix_end = NULL;
		return 2;
	}

	if (tok && match_ch(tok, '(')) {
		Token *outer_open = tok;
		Token *inner = skip_ret_pointers(tok_next(tok), &is_void);
		inner = skip_func_attrs_and_cc(inner);
		while (inner && match_ch(inner, '(')) {
			inner = skip_ret_pointers(tok_next(inner), NULL);
			inner = skip_func_attrs_and_cc(inner);
		}
		if (inner && is_valid_varname(inner) && tok_next(inner)) {
			if (match_ch(tok_next(inner), '(')) {
				Token *after_params = skip_balanced(tok_next(inner), '(', ')');
				if (after_params && match_ch(after_params, ')')) {
					Token *decl_end = skip_balanced(outer_open, '(', ')');
					while (decl_end && (decl_end->flags & TF_OPEN) &&
					       !match_ch(decl_end, '{') && !match_ch(decl_end, '(') &&
					       !(decl_end->flags & TF_C23_ATTR))
						decl_end = walk_balanced(decl_end, false);
					if (is_void) return 1;
					ctx->func_ret_type_start = type_start;
					ctx->func_ret_type_end = inner;
					ctx->func_ret_type_suffix_start = after_params;
					ctx->func_ret_type_suffix_end = decl_end;
					return 2;
				}
			} else if (tok_next(inner) == tok_match(outer_open)) {
				Token *params = tok_next(tok_next(inner));
				if (params && match_ch(params, '(')) {
					if (is_void) return 1;
					ctx->func_ret_type_start = type_start;
					ctx->func_ret_type_end = inner;
					ctx->func_ret_type_suffix_start = ctx->func_ret_type_suffix_end =
					    NULL;
					return 2;
				}
			}
		}
	}
	return 0;
}

static void emit_ret_type_tokens(Token *start, Token *end) {
	bool first = true;
	for (Token *t = start; t && t != end && t->kind != TK_EOF;) {
		if (is_ms_calling_conv_kw(t)) {
			t = tok_next(t);
			continue;
		}
		if ((t->tag & TT_ATTR) || is_c23_attr(t) || t->kind == TK_PREP_DIR) {
			Token *n = skip_noise(t);
			t = (n == t) ? tok_next(t) : n;
			continue;
		}
		if (!first) out_char(' ');
		first = false;
		Token *r = try_strip_raw(t);
		if (r) {
			t = r;
			continue;
		}
		OUT_TOK(t);
		t = tok_next(t);
	}
}

static void emit_ret_type(void) {
	if (ctx->func_ret_type_start && ctx->func_ret_type_end) {
		if (ctx->func_ret_type_suffix_start) {
			OUT_LIT("typedef ");
			emit_ret_type_tokens(ctx->func_ret_type_start, ctx->func_ret_type_end);
			OUT_LIT(" __prism_ret_t_");
			out_uint(ctx->ret_counter);
			for (Token *t = ctx->func_ret_type_suffix_start;
			     t && t != ctx->func_ret_type_suffix_end && t->kind != TK_EOF;
			     t = tok_next(t)) {
				out_char(' ');
				OUT_TOK(t);
			}
			OUT_LIT("; __prism_ret_t_");
			out_uint(ctx->ret_counter);
		} else
			emit_ret_type_tokens(ctx->func_ret_type_start, ctx->func_ret_type_end);
	} else {
		error("defer in function with unresolvable return type; "
		      "use a named struct or typedef");
	}
}

static Token *decl_noise(Token *tok, bool emit) {
	if (!emit) return skip_noise(tok);
	while (tok && tok->kind != TK_EOF) {
		if (tok->tag & TT_ATTR) {
			emit_tok(tok);
			tok = tok_next(tok);
			if (tok && match_ch(tok, '(')) tok = walk_balanced(tok, true);
		} else if (is_c23_attr(tok)) {
			tok = walk_balanced(tok, true);
		} else if (tok->kind == TK_PREP_DIR) {
			emit_tok(tok);
			tok = tok_next(tok);
		} else
			break;
	}
	return tok;
}

static Token *handle_open_brace(Token *tok);
static Token *handle_close_brace(Token *tok);
static Token *handle_defer_keyword(Token *tok);
static Token *handle_control_exit_defer(Token *tok);
static Token *handle_goto_keyword(Token *tok);
static Token *try_handle_defer_flow_kw(Token *tok);
static void arm_ctrl_pending_from_tag(Token *tok, uint32_t tag);
static void end_statement_after_semicolon(void);
static inline Token *try_process_stmt_token(Token *t, Token *end, Token **unreachable_tok);
static inline void track_generic_token(Token *tok);
static inline void track_ctrl_paren_open(void);
static inline void track_ctrl_paren_close(void);
static inline void track_ctrl_semicolon(void);
static Token *emit_ctrl_condition(Token *t, Token **unreachable_tok);
static Token *try_bounds_checks(Token *t);
static Token *skip_stmt_prefixes(Token *tok);
static Token *emit_through(Token *from, Token *to);

typedef enum {
	EMIT_NORMAL = 0,
	EMIT_DEFER_BODY = 1,
} EmitMode;

static Token *try_orelse_expr_rewrites(Token *tok) {
	if (!FEAT(F_ORELSE)) return NULL;
	Token *next = try_typeof_orelse(tok);
	if (next) return next;
	return try_bracket_orelse(tok);
}

enum { COLON_REQUIRE_BLOCK = 1 };

/* True if `prev` can end a label name before ':' — identifier, or the closing
 * delimiter of attrs between the name and colon (`done [[x]]:` /
 * `__attribute__`). */
static inline bool label_name_predecessor(Token *prev) {
	if (!prev) return false;
	if (is_identifier_like(prev) || prev->kind == TK_NUM) return true;
	if (match_ch(prev, ']') && tok_match(prev) && (tok_match(prev)->flags & TF_C23_ATTR)) return true;
	if (match_ch(prev, ')') && tok_match(prev)) {
		Token *before = tok_walk_back(tok_idx(tok_match(prev)), WB_ATTR_NOISE);
		return before && (before->tag & TT_ATTR);
	}
	return false;
}

/* Returns true if colon was consumed as a label/case terminator (caller should
 * continue). */
static bool consume_stmt_colon(Token **tok_p, int *ternary_depth, bool *pending_case_colon, unsigned flags) {
	Token *tok = *tok_p;
	if (!match_ch(tok, ':')) return false;
	if (*ternary_depth > 0) {
		(*ternary_depth)--;
		*pending_case_colon = false;
		return false;
	}
	if (!in_generic() && last_emitted && (label_name_predecessor(last_emitted) || *pending_case_colon) &&
	    !in_struct_body() && (!(flags & COLON_REQUIRE_BLOCK) || ctx->block_depth > 0)) {
		*pending_case_colon = false;
		*tok_p = emit_advance(tok);
		ctx->at_stmt_start = true;
		return true;
	}
	*pending_case_colon = false;
	return false;
}

/* Decl-context only: fold glibc C23 `_Generic(..., default: name(params))`
 * back to `(name)(params)`. Expression `_Generic` keeps SCOPE_GENERIC. */
static Token *emit_generic_open(Token *tok) {
	if (last_emitted &&
	    (match_ch(last_emitted, '*') || match_ch(last_emitted, ')') ||
	     (last_emitted->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_SKIP_DECL | TT_ATTR | TT_INLINE |
				   TT_STORAGE | TT_TYPEOF | TT_BITINT)) ||
	     is_known_typedef(last_emitted))) {
		Token *name = NULL, *params_open = NULL, *params_close = NULL, *after = NULL;
		if (generic_decl_rewrite_target(tok, &name, &params_open, &params_close, &after)) {
			out_char('(');
			OUT_TOK(name);
			out_char(')');
			emit_range(params_open, tok_next(params_close));
			Token *gen_close = tok_match(tok_next(tok));
			Token *scan_start = gen_close;
			Token *after_gen = skip_noise(tok_next(gen_close));
			if (after_gen == params_open) scan_start = params_close;
			for (Token *a = tok_next(scan_start); a && a != after; a = tok_next(a)) {
				emit_tok(a);
				last_emitted = a;
			}
			last_emitted = params_close ? params_close : name;
			ctx->at_stmt_start = false;
			return after;
		}
	}
	tok = emit_advance(tok);
	if (tok && match_ch(tok, '(')) {
		scope_push_kind(SCOPE_GENERIC);
		tok = emit_advance(tok);
	}
	ctx->at_stmt_start = false;
	return tok;
}

static Token *emit_statements(Token *tok, Token *end, EmitMode mode) {
	Token *unreachable_tok = NULL;
	int ternary_depth = 0;
	bool dr_braceless_body = false;
	bool pending_case_colon = false;
	while (tok && tok != end && tok->kind != TK_EOF) {
		if (match_ch(tok, '{')) {
			if (mode == EMIT_DEFER_BODY) {
				tok = emit_advance(tok);
				ctx->at_stmt_start = true;
				dr_braceless_body = false;
				if (ctrl_state.pending) ctrl_reset();
			} else {
				tok = handle_open_brace(tok);
			}
			continue;
		}

		if (match_ch(tok, '}')) {
			if (mode == EMIT_DEFER_BODY) {
				tok = emit_advance(tok);
				ctx->at_stmt_start = true;
				dr_braceless_body = false;
				if (ctrl_state.pending) ctrl_reset();
			} else {
				tok = handle_close_brace(tok);
			}
			continue;
		}

		if (match_ch(tok, ';')) {
			if (mode != EMIT_DEFER_BODY) end_statement_after_semicolon();
			bool is_ur = (tok == unreachable_tok);
			if (mode == EMIT_DEFER_BODY && dr_braceless_body) is_ur = false;
			tok = emit_advance(tok);
			if (is_ur) {
				EMIT_UNREACHABLE();
				unreachable_tok = NULL;
			}
			if (mode == EMIT_DEFER_BODY) {
				ctx->at_stmt_start = true;
				dr_braceless_body = false;
				if (ctrl_state.pending) ctrl_reset();
			}
			continue;
		}

		if (match_ch(tok, '?')) ternary_depth++;
		if (consume_stmt_colon(&tok, &ternary_depth, &pending_case_colon, 0)) continue;

		if (__builtin_expect(tok->kind == TK_PREP_DIR, 0)) {
			tok = emit_advance(tok);
			continue;
		}

		{
			Token *next = emit_gnu_label_decl(tok);
			if (next) {
				tok = next;
				continue;
			}
		}

		if ((tok->tag & TT_GENERIC) && !in_generic()) {
			tok = emit_generic_open(tok);
			continue;
		}

		if (mode != EMIT_DEFER_BODY) {
			Token *next = try_handle_defer_flow_kw(tok);
			if (next) {
				tok = next;
				continue;
			}
		}

		if (ctx->at_stmt_start && mode != EMIT_DEFER_BODY) check_enum_typedef_defer_shadow(tok);
		// Core statement dispatch: zeroinit, orelse, raw stripping, noreturn
		{
			Token *next = try_process_stmt_token(tok, end, &unreachable_tok);
			if (next) {
				tok = next;
				continue;
			}
		}

		if (ctx->at_stmt_start) {
			if (mode == EMIT_DEFER_BODY) {
				Token *probe = tok;
				while (probe && probe->kind != TK_EOF) {
					Token *sn = skip_noise(probe);
					if (sn != probe) {
						probe = sn;
						continue;
					}
					if (probe->tag &
					    (TT_INLINE | TT_TYPEDEF | TT_STORAGE | TT_QUALIFIER)) {
						probe = tok_next(probe);
						if (probe && match_ch(probe, '(') && tok_match(probe))
							probe = tok_next(tok_match(probe));
						continue;
					}
					break;
				}
				if (probe && (probe->tag & TT_SUE) && !is_known_typedef(probe)) {
					Token *brace = find_struct_body_brace(probe);
					if (brace && tok_match(brace)) {
						Token *close = tok_match(brace);
						while (tok && tok != end && tok->kind != TK_EOF) {
							Token *cur = tok;
							tok = emit_advance(tok);
							if (cur == close) break;
						}
						ctx->at_stmt_start = false;
						continue;
					}
				}
			} else if ((tok->tag & TT_SUE) && !is_known_typedef(tok)) {
				Token *next = handle_sue_body(tok);
				if (next) {
					tok = next;
					continue;
				}
			}
		}

#ifdef PRISM_DEBUG
		if (__builtin_expect(FEAT(F_ORELSE) && is_orelse_keyword(tok), 0))
			error_tok(tok, ERR_ORELSE_STMT_LEVEL);
#endif
		if (ctx->at_stmt_start && (tok->tag & (TT_IF | TT_LOOP | TT_SWITCH)) &&
		    !is_known_typedef(tok)) {
			if (is_else_or_do(tok)) {
				tok = emit_advance(tok);
				ctrl_state.pending = true;
				ctrl_state.parens_just_closed = true;
				ctx->at_stmt_start = true;
				if (mode == EMIT_DEFER_BODY) dr_braceless_body = true;
				continue;
			}
			Token *kw = tok;
			tok = emit_advance(tok);
			/* Emit attrs/_Pragma between keyword and condition '('. */
			while (tok && tok != end) {
				Token *after = skip_noise(tok);
				if (after == tok) break;
				tok = emit_through(tok, after);
			}
			if (tok && match_ch(tok, '(') && tok_match(tok)) {
				/* Arm + push SCOPE_CTRL/FOR_PAREN so break/continue inside
         * condition stmt-exprs stop here (don't paste outer defers).
         * Main Pass 2 loop gets this via arm_ctrl + track_common. */
				arm_ctrl_pending_from_tag(kw, kw->tag);
				ctrl_state.parens_just_closed = false;
				tok = emit_ctrl_condition(tok, &unreachable_tok);
				ctx->at_stmt_start = true;
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
		if (__builtin_expect(FEAT(F_BOUNDS_CHECK), 0)) {
			Token *bc = try_bounds_checks(tok);
			if (bc) {
				tok = bc;
				continue;
			}
		}
		{
			Token *next = try_orelse_expr_rewrites(tok);
			if (next) {
				tok = next;
				continue;
			}
		}

		if (tok->tag & (TT_CASE | TT_DEFAULT)) pending_case_colon = true;
		ctx->at_stmt_start = false;
		tok = emit_advance(tok);
	}
	return tok;
}

static inline Token *emit_stmt_expr(Token *t) {
	emit_tok(t); // '('
	Token *se_end = tok_match(t);
	Token *inner = tok_next(t); // '{'
	bool saved_ss = ctx->at_stmt_start;
	CtrlState saved_ctrl = ctrl_state;
	inner = emit_statements(inner, se_end, EMIT_NORMAL);
	ctx->at_stmt_start = saved_ss;
	ctrl_state = saved_ctrl;
	if (se_end) {
		emit_tok(se_end);
		return tok_next(se_end);
	}
	return inner;
}

static Token *emit_ctrl_condition(Token *t, Token **unreachable_tok) {
	Token *close_p = tok_match(t);
	if (!close_p) {
		emit_tok(t);
		return tok_next(t);
	}
#ifdef PRISM_DEBUG
	if (FEAT(F_ORELSE)) check_orelse_in_ctrl_paren(t);
#endif
	track_ctrl_paren_open();
	t = emit_advance(t); // emit '('
	ctx->at_stmt_start = true;
	while (t && t != close_p && t->kind != TK_EOF) {
		Token *next = try_process_stmt_token(t, close_p, unreachable_tok);
		if (next) {
			t = next;
			continue;
		}
		if ((t->flags & TF_OPEN) && is_stmt_expr_open(t) && tok_match(t)) {
			t = emit_stmt_expr(t);
			continue;
		}
		if ((t->flags & TF_OPEN) && tok_match(t)) {
			t = walk_balanced(t, true);
			ctx->at_stmt_start = false;
			continue;
		}
		if (match_ch(t, ';')) {
			t = emit_advance(t);
			track_ctrl_semicolon();
			ctx->at_stmt_start = true;
			continue;
		}
		if (__builtin_expect(FEAT(F_BOUNDS_CHECK), 0)) {
			Token *bc = try_bounds_checks(t);
			if (bc) {
				t = bc;
				continue;
			}
		}
		ctx->at_stmt_start = false;
		t = emit_advance(t);
	}
	if (t == close_p) {
		t = emit_advance(t);
	} // emit ')'
	track_ctrl_paren_close();
	return t;
}

static Token *try_typeof_orelse(Token *tok) {
	if (!(tok->tag & TT_TYPEOF) || !tok_next(tok) || !match_ch(tok_next(tok), '(') ||
	    !tok_match(tok_next(tok)))
		return NULL;
	Token *paren = tok_next(tok);
	if (!(tok_ann(paren) & P1_OE_BRACKET)) return NULL;
	emit_tok(tok);
	return walk_balanced_orelse(paren);
}

static Token *try_bracket_orelse(Token *tok) {
	if (!match_ch(tok, '[') || !(tok->flags & TF_OPEN) || !tok_match(tok)) return NULL;
	if (tok->flags & TF_C23_ATTR) return NULL;
	if (tok_ann(tok) & P1_OE_BRACKET) return walk_balanced_orelse(tok);
	return NULL;
}

static void p1_tag_brackets_in_range(Token *open, Token *close) {
	for (Token *u = tok_next(open); u != close && u->kind != TK_EOF; u = tok_next(u)) {
		if (match_ch(u, '[') && (u->flags & TF_OPEN)) tok_ann(u) |= P1_UNEVAL_BRACKET;
	}
}

static Token *last_comma_operand(Token *open_paren, Token *close_paren) {
	Token *seg_start = tok_next(open_paren);
	for (Token *t = seg_start; t && t != close_paren; t = tok_next(t)) {
		if (t->flags & TF_OPEN) {
			t = tok_match(t);
			continue;
		}
		if (match_ch(t, ',')) seg_start = tok_next(t);
	}
	return seg_start;
}

static inline bool c_value_name_token(Token *t) {
	/* Soft type spellings (`_Float32`, `bool`, …) are valid declarator names
	 * after an established type; treat them as value names for bounds / postfix. */
	return t && is_valid_varname(t) &&
	       (!(t->tag & TT_TYPE) || is_soft_keyword_identifier(t)) && !is_known_typedef(t);
}

static void p1_tag_postfix_chain(Token *p) {
	while (p && p->kind != TK_EOF) {
		if ((match_ch(p, '[') || match_ch(p, '(')) && (p->flags & TF_OPEN) && tok_match(p)) {
			if (match_ch(p, '[')) tok_ann(p) |= P1_UNEVAL_BRACKET;
			Token *c = tok_match(p);
			p1_tag_brackets_in_range(p, c);
			p = tok_next(c);
			continue;
		}
		if (match_ch(p, '.') || equal(p, "->")) {
			p = tok_next(p);
			if (c_value_name_token(p)) p = tok_next(p);
			continue;
		}
		if (equal(p, "++") || equal(p, "--")) {
			p = tok_next(p);
			continue;
		}
		break;
	}
}

static inline bool is_uneval_operand_intro(Token *t) {
	return (t->flags & TF_SIZEOF) || (t->tag & TT_TYPEOF) ||
	       (t->kind == TK_IDENT && t->len == 18 && prism_memeq_static(tok_loc(t), "__builtin_offsetof", 18));
}

static void p1_mark_uneval_brackets(void) {
	int fs_cb = 0;
	bool fs_in_init = false;
	for (uint32_t i = 1; i < token_count; i++) {
		Token *t = &token_pool[i];
		if ((t->flags & TF_OPEN) && match_ch(t, '{')) fs_cb++;
		else if ((t->flags & TF_CLOSE) && match_ch(t, '}')) {
			if (fs_cb) fs_cb--;
		} else if (fs_cb == 0) {
			if (match_ch(t, '=')) fs_in_init = true;
			else if (match_ch(t, ';'))
				fs_in_init = false;
		}
		if (fs_in_init && (t->flags & TF_OPEN) && match_ch(t, '[')) tok_ann(t) |= P1_UNEVAL_BRACKET;
		// _Static_assert / static_assert predicate: must stay an integer
		if (t->flags & TF_STATIC_ASSERT) {
			Token *lp = tok_next(t);
			if (lp && match_ch(lp, '(') && (lp->flags & TF_OPEN) && tok_match(lp)) {
				Token *rp = tok_match(lp);
				if (rp) {
					p1_tag_brackets_in_range(lp, rp);
					i = tok_idx(rp);
				}
			}
			continue;
		}
		// _Generic(controlling_expr, type1: val1, ...): per C11 §6.5.1.1p3,
		if (t->tag & TT_GENERIC) {
			Token *lp = tok_next(t);
			if (lp && match_ch(lp, '(') && (lp->flags & TF_OPEN) && tok_match(lp)) {
				Token *close = tok_match(lp);
				int depth = 0;
				Token *u;
				for (u = tok_next(lp); u != close && u->kind != TK_EOF; u = tok_next(u)) {
					if (match_ch(u, '[') && (u->flags & TF_OPEN))
						tok_ann(u) |= P1_UNEVAL_BRACKET;
					if (u->flags & TF_OPEN) {
						depth++;
						continue;
					}
					if (u->flags & TF_CLOSE) {
						depth--;
						continue;
					}
					if (depth == 0 && match_ch(u, ',')) break;
				}
				i = tok_idx(u && u != close ? u : close);
			}
			continue;
		}
		bool is_uneval = is_uneval_operand_intro(t);
		if (!is_uneval) continue;
		Token *next = tok_next(t);
		if (!next) continue;
		if (match_ch(next, '(') && (next->flags & TF_OPEN)) {
			Token *close = tok_match(next);
			if (close) {
				p1_tag_brackets_in_range(next, close);
				p1_tag_postfix_chain(tok_next(close));
				i = tok_idx(close); /* nested uneval already covered */
			}
			continue;
		}
		Token *p = next;
		while (p && p->kind != TK_EOF) {
			if (match_set(p, CH('+') | CH('-') | CH('!') | CH('&') | CH('*')) ||
			    match_ch(p, '~') || equal(p, "++") || equal(p, "--")) {
				p = tok_next(p);
				continue;
			}
			break;
		}
		if (!c_value_name_token(p)) continue;
		p1_tag_postfix_chain(tok_next(p));
	}
}

static inline bool bounds_is_name_token(Token *t) {
	return c_value_name_token(t);
}

static bool is_tracked_array_name(Token *t) {
	if (!bounds_is_name_token(t)) return false;
	/* Param / enum shadows live in the typedef table; consult them before
	 * the bounds-array registry so `int g[10]; f(int g[20]){ g[i]; }` does
	 * not wrap the decayed parameter against the file-scope array. */
	TypedefEntry *te = typedef_lookup(t);
	if (te && (te->is_param || te->is_enum_const)) return false;
	BoundsArrayEntry *be = bounds_array_lookup(t);
	if (be && !be->is_param) return true;
	if (!te) return false;
	return te->is_array || te->is_vla_var;
}

static bool bounds_expr_base_is_pointer(Token *tok) {
	if (!bounds_is_name_token(tok)) return false;
	unsigned c0 = tok->ch0, tl = tok->len;
	if (!(typedef_table.bloom & (1ULL << ((c0 ^ tl) & 63)))) return false;
	int idx = typedef_get_index(tok_loc(tok), tok->len);
	uint32_t cur = tok_idx(tok);
	while (idx >= 0) {
		TypedefEntry *e = &typedef_table.entries[idx];
		if (e->token_index <= cur && cur >= e->scope_open_idx && cur < e->scope_close_idx &&
		    !e->is_struct_tag && e->is_ptr)
			return true;
		idx = e->prev_index;
	}
	return false;
}

static Token *bounds_peel_paren_ident(Token *le) {
	if (!match_ch(le, ')') || !tok_match(le)) return le;
	Token *open = tok_match(le);
	if (tok_idx(open) >= 1) {
		Token *before = &token_pool[tok_idx(open) - 1];
		if (bounds_is_name_token(before) || before->kind == TK_NUM || match_ch(before, ')') ||
		    match_ch(before, ']'))
			return le;
	}
	Token *ii = tok_next(open);
	if (bounds_is_name_token(ii) && tok_next(ii) == le) return ii;
	return le;
}

// are type-queries, not value uses, and cannot form a commutative bypass.
static Token *bounds_find_array_ident(Token *start, Token *end) {
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = tok_next(t)) {
		if ((t->flags & TF_OPEN) && match_ch(t, '(') && tok_idx(t) >= 1) {
			Token *pv = &token_pool[tok_idx(t) - 1];
			if (is_uneval_operand_intro(pv)) {
				Token *c = tok_match(t);
				if (c) {
					t = c;
					continue;
				}
			}
		}
		if (!bounds_is_name_token(t) || is_known_typedef(t)) continue;
		if (tok_idx(t) >= 1 && (token_pool[tok_idx(t) - 1].tag & TT_MEMBER)) continue;
		if (is_tracked_array_name(t)) return t;
	}
	return NULL;
}

// downgraded from hard error to warning. SAFETY_DIAG is the plain form;
#define SAFETY_DIAG(t, msg)                                                                                  \
	do {                                                                                                 \
		if (FEAT(F_WARN_SAFETY)) warn_tok((t), msg);                                                 \
		else                                                                                         \
			error_tok((t), msg);                                                                 \
	} while (0)
#define BOUNDS_COMM_DIAG(t, msg)                                                                             \
	do {                                                                                                 \
		if (FEAT(F_WARN_SAFETY)) {                                                                   \
			warn_tok((t), msg);                                                                  \
			return NULL;                                                                         \
		}                                                                                            \
		error_tok((t), msg);                                                                         \
	} while (0)

static Token *try_bounds_check_deref_add(Token *tok);
static Token *try_bounds_checks(Token *t);

static bool bounds_paren_derives_array(Token *open) {
	if (!open || !match_ch(open, '(') || !(open->flags & TF_OPEN)) return false;
	Token *close = tok_match(open);
	if (!close) return false;
	for (Token *t = tok_next(open); t && t != close && t->kind != TK_EOF; t = tok_next(t)) {
		if ((t->flags & TF_OPEN) && match_ch(t, '(') && tok_idx(t) >= 1) {
			Token *pv = &token_pool[tok_idx(t) - 1];
			if (is_uneval_operand_intro(pv)) {
				Token *c = tok_match(t);
				if (c) {
					t = c;
					continue;
				}
			}
		}
		if (match_ch(t, '&') && !(t->flags & TF_OPEN)) {
			Token *n = tok_next(t);
			if (n && n != close && bounds_is_name_token(n) && is_tracked_array_name(n))
				return true;
		}
	}
	return false;
}

/* `(a+i)` / `(a-i)` with tracked array — same unverifiable shape as `*(a+i)`. */
static bool bounds_paren_array_arith(Token *open) {
	if (!open || !match_ch(open, '(') || !(open->flags & TF_OPEN) || !tok_match(open)) return false;
	Token *close = tok_match(open);
	Token *scan_end = close;
	Token *lhs = tok_next(open);
	if (!lhs || lhs == close) return false;
	while (lhs && match_ch(lhs, '(') && (lhs->flags & TF_OPEN) && tok_match(lhs)) {
		Token *inner_cp = tok_match(lhs);
		if (!inner_cp || tok_next(inner_cp) != scan_end) break;
		lhs = tok_next(lhs);
		scan_end = inner_cp;
		if (!lhs || lhs == scan_end) return false;
	}
	bool has_addsub = false;
	for (Token *t = lhs; t && t != scan_end && t->kind != TK_EOF; t = tok_next(t)) {
		if ((t->flags & TF_OPEN) && tok_match(t)) {
			t = tok_match(t);
			continue;
		}
		if ((match_ch(t, '+') || match_ch(t, '-')) && !(t->flags & TF_OPEN)) {
			has_addsub = true;
			break;
		}
	}
	if (!has_addsub) return false;
	return bounds_find_array_ident(lhs, scan_end) != NULL;
}

static Token *try_bounds_check_subscript(Token *tok) {
	if (!FEAT(F_BOUNDS_CHECK)) return NULL;
	if (!match_ch(tok, '[') || !(tok->flags & TF_OPEN) || !tok_match(tok)) return NULL;
	if (tok->flags & TF_C23_ATTR) return NULL;
	if (tok_ann(tok) & (P1_DECL_BRACKET | P1_UNEVAL_BRACKET)) return NULL;
	/* Idempotence: an index already wrapped by a previous Prism pass
	 * (`a[__prism_bchk(...)]` — e.g. -save-temps .i re-compiles or
	 * --prism-verify's second pass) must not be wrapped again.  The
	 * __prism_ namespace is reserved, so this cannot fire on user code. */
	{
		Token *idx0 = tok_next(tok);
		if (idx0 && idx0->kind == TK_IDENT && idx0->len == 12 &&
		    prism_memeq_static(tok_loc(idx0), "__prism_bchk", 12))
			return NULL;
	}
	// ALWAYS declarator dimensions per C11 6.7.2.1, never expression
	// brackets never get tagged P1_DECL_BRACKET via parse_declarator.
	if (in_struct_body()) return NULL;
	if (!last_emitted) return NULL;
	uint32_t ti = tok_idx(tok);
	Token *rp_prev = (ti >= 1) ? &token_pool[ti - 1] : NULL;
	if (rp_prev) {
		Token *rp = rp_prev;
		if (match_ch(rp, ')') && (rp->flags & TF_CLOSE) && tok_match(rp)) {
			Token *op = tok_match(rp);
			if (bounds_paren_derives_array(op)) BOUNDS_COMM_DIAG(tok, ERR_BOUNDS_DERIVED_SUB);
			/* `(a+i)[0]` must not wrap index 0 against sizeof(a) — the
			 * base is already offset. Same unverifiable shape as `*(a+i)`. */
			if (bounds_paren_array_arith(op)) {
				if (FEAT(F_WARN_SAFETY))
					warn_tok(tok,
						 "-fbounds-check: pointer-arithmetic subscript with "
						 "tracked array base cannot be verified (rewrite as "
						 "array[index])");
				else
					error_tok(tok,
						  "-fbounds-check: pointer-arithmetic subscript with "
						  "tracked array base cannot be verified (rewrite as "
						  "array[index])");
				return NULL;
			}
		}
	}

	// ISO C treats `(i)[b]` the same as `i[b]` (commutative subscript).
	if (rp_prev) {
		Token *rp = rp_prev;
		if (match_ch(rp, ')') && tok_match(rp)) {
			Token *op = tok_match(rp);
			Token *idx = tok_next(op);
			if (bounds_is_name_token(idx) && tok_next(idx) == rp) {
				Token *rb_close = tok_match(tok);
				Token *inner = tok_next(tok);
				if (rb_close && inner && tok_next(inner) == rb_close &&
				    is_tracked_array_name(inner) && !is_tracked_array_name(idx))
					BOUNDS_COMM_DIAG(tok, ERR_BOUNDS_COMM_IDX_ARR);
			}
		}
	}

	// Commutative-subscript bypass check: ISO C defines `idx[arr]` as
	{
		Token *rb_close = tok_match(tok);
		Token *inner_first = tok_next(tok);
		if (inner_first && rb_close && match_ch(inner_first, '(') && (inner_first->flags & TF_OPEN) &&
		    tok_match(inner_first) && tok_next(tok_match(inner_first)) == rb_close &&
		    bounds_paren_derives_array(inner_first))
			BOUNDS_COMM_DIAG(tok, ERR_BOUNDS_COMM_DERIVED);
	}

	bool comma_resolved = false;
	{
		Token *close_scan = tok_match(tok);
		Token *inner = tok_next(tok);
		Token *iclose = close_scan;
		/* ISO `idx[(e1, arr)]` == `idx[arr]`; unwrap comma tail so the
     * main path sees the array ident.  If the last operand is the
     * tracked array, this is not a commutative *bypass* (it is
     * `arr` with index `idx` in disguise) — skip hard-error diags. */
		if (match_ch(inner, '(') && (inner->flags & TF_OPEN)) {
			Token *pclose = tok_match(inner);
			if (pclose) {
				Token *lastop = last_comma_operand(inner, pclose);
				if (lastop && lastop != tok_next(inner) && tok_next(lastop) == pclose) {
					inner = lastop;
					iclose = pclose;
					if (is_tracked_array_name(lastop)) comma_resolved = true;
				}
			}
		}
		if (!comma_resolved) {
			/* If last_emitted is a struct/union field access (preceded by
       * `.`/`->`), we do not know the field's type from the typedef
       * table, so we cannot assert a commutative bypass. Skip both
       * branches — the final brute-scan guard below is gated by the
       * same `memb` flag and handles this case correctly. */
			bool le_is_member = tok_idx(last_emitted) >= 1 &&
					    (token_pool[tok_idx(last_emitted) - 1].tag & TT_MEMBER);
			if (!le_is_member) {
				while (inner != iclose && match_ch(inner, '(') && (inner->flags & TF_OPEN) &&
				       tok_match(inner) && tok_next(tok_match(inner)) == iclose) {
					iclose = tok_match(inner);
					inner = tok_next(inner);
				}
				if (inner != iclose && tok_next(inner) == iclose &&
				    is_tracked_array_name(inner)) {
					Token *le = bounds_peel_paren_ident(last_emitted);
					if (!is_tracked_array_name(le) && !bounds_expr_base_is_pointer(le))
						BOUNDS_COMM_DIAG(tok, ERR_BOUNDS_COMM_IDX_ARR);
				} else if (inner != iclose && bounds_is_name_token(inner)) {
					Token *array_root = NULL;
					Token *scan = inner;
					while (scan && scan != iclose) {
						if (!bounds_is_name_token(scan)) break;
						Token *lb = tok_next(scan);
						if (!lb || !match_ch(lb, '[') || !(lb->flags & TF_OPEN) ||
						    !tok_match(lb))
							break;
						array_root = scan;
						Token *rb = tok_match(lb);
						if (!rb) break;
						scan = tok_next(rb);
					}
					if (scan == iclose && array_root &&
					    is_tracked_array_name(array_root)) {
						Token *le = bounds_peel_paren_ident(last_emitted);
						if (!is_tracked_array_name(le) &&
						    !bounds_expr_base_is_pointer(le))
							BOUNDS_COMM_DIAG(tok, ERR_BOUNDS_COMM_IDX_ARR);
					}
				}
			} // !le_is_member
		} // !comma_resolved
	}
	if (comma_resolved) BOUNDS_COMM_DIAG(tok, ERR_BOUNDS_COMMA_OP);
	{
		Token *rb = tok_match(tok);
		if (rb && (bounds_is_name_token(last_emitted) || last_emitted->kind == TK_NUM)) {
			bool memb = tok_idx(last_emitted) >= 1 &&
				    (token_pool[tok_idx(last_emitted) - 1].tag & TT_MEMBER);
			bool left_ok_scan = false;
			if (!memb && last_emitted->kind != TK_NUM && !is_known_typedef(last_emitted)) {
				TypedefEntry *tel = typedef_lookup(last_emitted);
				if (tel && (tel->is_param || tel->is_enum_const))
					left_ok_scan = false;
				else {
					BoundsArrayEntry *bel = bounds_array_lookup(last_emitted);
					if (bel && !bel->is_param)
						left_ok_scan = true;
					else
						left_ok_scan = tel && !tel->is_enum_const &&
							       (tel->is_array || tel->is_vla_var);
				}
			}
			Token *hit = left_ok_scan ? NULL : bounds_find_array_ident(tok_next(tok), rb);
			while (hit) {
				Token *nx = tok_next(hit);
				if (!(nx && match_ch(nx, '[') && (nx->flags & TF_OPEN))) break;
				hit = bounds_find_array_ident(tok_next(nx), rb);
			}
			if (hit) BOUNDS_COMM_DIAG(tok, ERR_BOUNDS_COMM_SCAN);
			/* `0[(a+i)]` — array arith in index paren */
			Token *inner0 = tok_next(tok);
			if (!left_ok_scan && inner0 && match_ch(inner0, '(') && (inner0->flags & TF_OPEN) &&
			    tok_match(inner0) && tok_next(tok_match(inner0)) == rb &&
			    bounds_paren_array_arith(inner0)) {
				if (FEAT(F_WARN_SAFETY))
					warn_tok(tok,
						 "-fbounds-check: pointer-arithmetic subscript with "
						 "tracked array base cannot be verified (rewrite as "
						 "array[index])");
				else
					error_tok(tok,
						  "-fbounds-check: pointer-arithmetic subscript with "
						  "tracked array base cannot be verified (rewrite as "
						  "array[index])");
				return NULL;
			}
		}
	}
	Token *name_tok = last_emitted;
	if (tok_idx(tok) >= 1) {
		Token *pool_prev = &token_pool[tok_idx(tok) - 1];
		if (pool_prev != last_emitted &&
		    (match_ch(pool_prev, ']') || match_ch(pool_prev, ')') || bounds_is_name_token(pool_prev)))
			name_tok = pool_prev;
	}
	int dim_depth = 0;
	while (1) {
		if (match_ch(name_tok, ')') && tok_match(name_tok)) {
			Token *open = tok_match(name_tok);
			if (tok_idx(open) >= 1) {
				Token *bp = &token_pool[tok_idx(open) - 1];
				if (bounds_is_name_token(bp) || bp->kind == TK_NUM || match_ch(bp, ')') ||
				    match_ch(bp, ']'))
					break;
			}
			Token *inner = tok_next(open);
			if (!inner) break;
			if (bounds_is_name_token(inner) && tok_next(inner) == name_tok) {
				name_tok = inner;
				break;
			}
			if (match_ch(inner, '(') && tok_match(inner) &&
			    tok_next(tok_match(inner)) == name_tok) {
				name_tok = tok_match(inner);
				continue;
			}
			if (tok_idx(name_tok) >= 1) {
				Token *inner_last = &token_pool[tok_idx(name_tok) - 1];
				if (match_ch(inner_last, ']')) {
					name_tok = inner_last;
					continue;
				}
			}
			break;
		}
		if (match_ch(name_tok, ']') && tok_match(name_tok)) {
			Token *open_br = tok_match(name_tok);
			if (tok_ann(open_br) & (P1_DECL_BRACKET | P1_UNEVAL_BRACKET)) break;
			if (open_br->flags & TF_C23_ATTR) break;
			if (tok_idx(open_br) < 1) break;
			Token *before = &token_pool[tok_idx(open_br) - 1];
			if (!(bounds_is_name_token(before) || match_ch(before, ')') || match_ch(before, ']')))
				break;
			name_tok = before;
			dim_depth++;
			continue;
		}
		break;
	}
	if (!bounds_is_name_token(name_tok) && last_emitted) {
		Token *probe = last_emitted;
		while (probe && match_ch(probe, ')') && tok_match(probe)) {
			Token *op = tok_match(probe);
			if (bounds_paren_array_arith(op)) {
				if (FEAT(F_WARN_SAFETY))
					warn_tok(tok,
						 "-fbounds-check: pointer-arithmetic subscript with "
						 "tracked array base cannot be verified (rewrite as "
						 "array[index])");
				else
					error_tok(tok,
						  "-fbounds-check: pointer-arithmetic subscript with "
						  "tracked array base cannot be verified (rewrite as "
						  "array[index])");
				return NULL;
			}
			Token *hit = bounds_find_array_ident(tok_next(op), probe);
			if (hit && is_tracked_array_name(hit)) {
				name_tok = hit;
				break;
			}
			if (tok_idx(op) < 1) break;
			probe = &token_pool[tok_idx(op) - 1];
		}
	}
	if (!bounds_is_name_token(name_tok)) return NULL;
	if (is_known_typedef(name_tok)) return NULL;
	if (tok_idx(name_tok) >= 1 && (token_pool[tok_idx(name_tok) - 1].tag & TT_MEMBER)) return NULL;
	if (tok_idx(name_tok) >= 1) {
		Token *pv = &token_pool[tok_idx(name_tok) - 1];
		if (pv->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF)) return NULL;
		if (match_ch(pv, '*') && !(pv->flags & TF_OPEN) && tok_idx(pv) >= 1) {
			Token *pp = &token_pool[tok_idx(pv) - 1];
			bool is_decl_star = (pp->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF)) ||
					    (match_ch(pp, '*') && !(pp->flags & TF_OPEN)) ||
					    (match_ch(pp, '(') && (pp->flags & TF_OPEN));
			if (is_decl_star) return NULL;
		}
	}
	BoundsArrayEntry *be = bounds_array_lookup(name_tok);
	TypedefEntry *te = typedef_lookup(name_tok);
	/* Decayed parameter names shadow outer array bindings — even when the
	 * bounds registry still sees the file-scope `g`. */
	if (te && (te->is_param || te->is_enum_const)) return NULL;
	if (be) {
		if (be->is_param) return NULL;
		if (be->array_rank > 0 && be->array_rank != ARRAY_RANK_WRAP_ALL &&
		    dim_depth >= be->array_rank)
			return NULL;
	} else {
		if (!te || (!te->is_array && !te->is_vla_var)) return NULL;
		if (te->array_rank > 0 && te->array_rank != ARRAY_RANK_WRAP_ALL && dim_depth >= te->array_rank)
			return NULL;
	}
	if (tok_idx(name_tok) >= 1) {
		Token *operand_start = name_tok;
		Token *operand_end = name_tok;
		Token *prev = &token_pool[tok_idx(operand_start) - 1];
		while (match_ch(prev, '(') && (prev->flags & TF_OPEN) && tok_match(prev)) {
			Token *rp = tok_match(prev);
			if (!rp || tok_next(prev) != operand_start || tok_next(operand_end) != rp) break;
			operand_start = prev;
			operand_end = rp;
			if (tok_idx(prev) < 1) {
				prev = NULL;
				break;
			}
			prev = &token_pool[tok_idx(prev) - 1];
		}
		if (prev && match_ch(prev, '&') && !(prev->flags & TF_OPEN)) {
			bool unary = true;
			if (tok_idx(prev) >= 1) {
				Token *pp = &token_pool[tok_idx(prev) - 1];
				/* `)` after a cast type-name `(void)` / `(int *)` still
				 * leaves `&` unary — `(void)&a[n]` is one-past-legal.
				 * Expression `)` (`(x)&a[i]`) is binary `&`. */
				if (bounds_is_name_token(pp) || pp->kind == TK_NUM || pp->kind == TK_STR ||
				    match_ch(pp, ']') ||
				    (match_ch(pp, ')') && !close_paren_ends_cast_type_name(pp)))
					unary = false;
			}
			if (unary) return NULL;
		}
	}

	Token *arr_tok = name_tok;
	Token *close = tok_match(tok);
	/* Pointer-cast then subscript: bound with cast element size via
	 * sizeof(arr)/sizeof(*(T *)arr), not sizeof(arr)/sizeof(arr[0]). */
	Token *cast_close = NULL; /* ')' of (T *) */
	{
		Token *probe = arr_tok;
		for (int peel = 0; peel < 4 && tok_idx(probe) >= 1; peel++) {
			Token *prev = &token_pool[tok_idx(probe) - 1];
			if (match_ch(prev, ')') && tok_match(prev)) {
				Token *open = tok_match(prev);
				Token *last = tok_idx(prev) >= 1 ? &token_pool[tok_idx(prev) - 1] : NULL;
				if (last && match_ch(last, '*')) {
					cast_close = prev;
					break;
				}
				if (tok_next(open) == probe && tok_next(probe) == prev) {
					probe = open;
					continue;
				}
			}
			break;
		}
	}
	OUT_LIT("[__prism_bchk((__prism_bchk_size_t)(");
	if (FEAT(F_ORELSE) && (tok_ann(tok) & P1_OE_BRACKET))
		emit_token_range_orelse(tok_next(tok), close);
	else {
		for (Token *t = tok_next(tok); t != close && t->kind != TK_EOF;) {
			if ((t->flags & TF_OPEN) && is_stmt_expr_open(t)) {
				t = emit_stmt_expr(t);
				continue;
			}
			Token *bc = try_bounds_checks(t);
			if (bc) {
				t = bc;
				continue;
			}
			if ((t->flags & TF_OPEN) && tok_match(t)) {
				t = walk_balanced(t, true);
				continue;
			}
			t = emit_advance(t);
		}
	}
	OUT_LIT("), sizeof(");
	out_str(tok_loc(arr_tok), arr_tok->len);
	for (int d = 0; d < dim_depth; d++) OUT_LIT("[0]");
	OUT_LIT(")/sizeof(");
	if (cast_close) {
		/* sizeof(*(T *)arr) — cast_close is ')' of (T *) before arr. */
		Token *cast_open = tok_match(cast_close);
		OUT_LIT("(*");
		for (Token *ct = cast_open; ct && ct != arr_tok; ct = tok_next(ct))
			emit_tok(ct);
		out_str(tok_loc(arr_tok), arr_tok->len);
		OUT_LIT(")");
	} else {
		out_str(tok_loc(arr_tok), arr_tok->len);
		for (int d = 0; d <= dim_depth; d++) OUT_LIT("[0]");
	}
	OUT_LIT("))]");
	return tok_next(close);
}

static bool bounds_star_in_uneval(Token *tok) {
	/* Skip sizeof/_Alignof/typeof/_Generic/_Static_assert operands — same
	 * policy as P1_UNEVAL_BRACKET for `a[i]`, but `*(a+i)` has no bracket. */
	int depth = 0;
	for (uint32_t i = tok_idx(tok); i > 0; i--) {
		Token *t = &token_pool[i - 1];
		if (t->flags & TF_CLOSE) {
			depth++;
			continue;
		}
		if (t->flags & TF_OPEN) {
			if (depth > 0) {
				depth--;
				continue;
			}
			Token *prev = (i >= 2) ? &token_pool[i - 2] : NULL;
			if (!prev) return false;
			if (is_uneval_operand_intro(prev) || (prev->tag & TT_GENERIC) ||
			    (prev->flags & TF_STATIC_ASSERT))
				return true;
			if (prev->kind == TK_IDENT &&
			    ((prev->len == 8 && prism_memeq_static(tok_loc(prev), "_Alignof", 8)) ||
			     (prev->len == 7 && prism_memeq_static(tok_loc(prev), "alignof", 7))))
				return true;
			return false;
		}
		if (depth != 0) continue;
		if (is_uneval_operand_intro(t) || (t->tag & TT_GENERIC) || (t->flags & TF_STATIC_ASSERT))
			return true;
		if (t->kind == TK_IDENT &&
		    ((t->len == 8 && prism_memeq_static(tok_loc(t), "_Alignof", 8)) ||
		     (t->len == 7 && prism_memeq_static(tok_loc(t), "alignof", 7))))
			return true;
		if (match_ch(t, ';') || match_ch(t, '{') || match_ch(t, '}') || match_ch(t, ','))
			return false;
	}
	return false;
}

static Token *try_bounds_check_deref_add(Token *tok) {
	if (!FEAT(F_BOUNDS_CHECK)) return NULL;
	if (!match_ch(tok, '*') || (tok->flags & TF_OPEN)) return NULL;
	if (bounds_star_in_uneval(tok)) return NULL;
	if (tok_idx(tok) >= 1) {
		Token *prev = &token_pool[tok_idx(tok) - 1];
		if (prev->kind == TK_NUM || prev->kind == TK_STR) return NULL;
		if (prev->kind == TK_IDENT && !(prev->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF)) &&
		    !(prev->tag & (TT_RETURN | TT_GOTO | TT_DEFER))) return NULL;
		if (match_ch(prev, ']')) return NULL;
		if (match_ch(prev, ')') && (prev->flags & TF_CLOSE)) {
			Token *om = tok_match(prev);
			Token *fi = om ? tok_next(om) : NULL;
			bool looks_cast = fi && (is_type_keyword(fi) ||
						 (fi->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF)));
			if (!looks_cast) return NULL;
		}
	}

	Token *op = tok_next(tok);
	if (!op || !match_ch(op, '(') || !(op->flags & TF_OPEN) || !tok_match(op)) return NULL;
	/* Peel casts: `*(T*)(a+i)` / `*(int *)(a+i)` — otherwise the cast
	 * group hides the additive paren and the check never fires. */
	for (;;) {
		Token *cast_close = tok_match(op);
		Token *fi = tok_next(op);
		bool looks_cast =
		    fi && (is_type_keyword(fi) || (fi->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF)));
		if (!looks_cast) break;
		Token *after = tok_next(cast_close);
		if (!after || !match_ch(after, '(') || !(after->flags & TF_OPEN) || !tok_match(after))
			return NULL;
		op = after;
	}
	Token *cp = tok_match(op);
	Token *lhs = tok_next(op);
	if (!lhs || lhs == cp) return NULL;
	/* Peel redundant outer paren layers: `*((a + 1))` → scan `a + 1`.
	 * Without this, the inner `(a + 1)` is skipped as a nested group and
	 * the unverifiable pointer arithmetic bypasses -fbounds-check. */
	Token *scan_end = cp;
	while (lhs && match_ch(lhs, '(') && (lhs->flags & TF_OPEN) && tok_match(lhs)) {
		Token *inner_cp = tok_match(lhs);
		if (!inner_cp || tok_next(inner_cp) != scan_end) break;
		lhs = tok_next(lhs);
		scan_end = inner_cp;
		if (!lhs || lhs == scan_end) return NULL;
	}
	// Must contain a top-level `+` or `-` operator inside the parens.
	bool has_addsub = false;
	for (Token *t = lhs; t && t != scan_end && t->kind != TK_EOF; t = tok_next(t)) {
		if ((t->flags & TF_OPEN) && tok_match(t)) {
			t = tok_match(t);
			continue;
		}
		if ((match_ch(t, '+') || match_ch(t, '-')) && !(t->flags & TF_OPEN)) {
			has_addsub = true;
			break;
		}
	}
	if (!has_addsub) return NULL;
	// Must reference a tracked array name somewhere in the parens.
	Token *hit = bounds_find_array_ident(lhs, scan_end);
	if (!hit) return NULL;
	if (FEAT(F_WARN_SAFETY)) {
		warn_tok(tok,
			 "-fbounds-check: pointer-arithmetic dereference with tracked "
			 "array base cannot be verified (rewrite as array[index])");
		return NULL;
	}
	error_tok(tok,
		  "-fbounds-check: pointer-arithmetic dereference with tracked "
		  "array base cannot be verified (rewrite as array[index])");
	return NULL;
}

static inline Token *try_bounds_checks(Token *t) {
	/* Only '[' (subscript) and '*' (pointer-arith deref) can be bounds sites;
	 * cheap ch0 gate avoids two call+FEAT+match_ch per ordinary token. */
	if (t->ch0 != '[' && t->ch0 != '*') return NULL;
	Token *n = try_bounds_check_subscript(t);
	return n ? n : try_bounds_check_deref_add(t);
}

static inline void reject_defer_in_expr_context(Token *t) {
#ifdef PRISM_DEBUG
	if (__builtin_expect(FEAT(F_DEFER) && (t->tag & TT_DEFER), 0) && !is_known_function_call(t) &&
	    !(last_emitted && (last_emitted->tag & TT_MEMBER)) &&
	    (!typedef_lookup(t) || match_ch(tok_next(t), '{')) && tok_next(t) &&
	    (is_identifier_like(tok_next(t)) || match_ch(tok_next(t), '{')))
		error_tok(t,
			  "'defer' cannot be used in expression context "
			  "(array dimensions, parenthesized expressions, etc.)");
#else
	(void)t;
#endif
}

static inline bool walk_balanced_tail(Token **tp) {
	Token *t = *tp;
	if (FEAT(F_DEFER) && defer_count > 0 && is_enum_kw(t)) {
		Token *brace = find_struct_body_brace(t);
		if (brace) check_enum_body_defer_shadow(brace);
	}
	reject_defer_in_expr_context(t);
	*tp = emit_advance(t);
	return true;
}

static PRISM_HOT Token *walk_balanced(Token *tok, bool emit) {
	Token *end = tok_match(tok);
	if (!end) return tok_next(tok);
	if (emit) {
		for (Token *t = tok; t != tok_next(end) && t->kind != TK_EOF;) {
			if ((t->flags & TF_OPEN) && is_stmt_expr_open(t)) {
				t = emit_stmt_expr(t);
				continue;
			}
			if (__builtin_expect(FEAT(F_BOUNDS_CHECK), 0)) {
				Token *bc = try_bounds_checks(t);
				if (bc) {
					t = bc;
					continue;
				}
			}
			if (FEAT(F_ORELSE) && (t->flags & TF_OPEN) && match_ch(t, '[') && tok_match(t)) {
				// C23 [[ ... ]]: verbatim interior; Phase 1D rejects orelse
				// inside attrs. Skip FIFO bracket-orelse path (same as before).
				if (t->flags & TF_C23_ATTR) {
					t = emit_c23_attr(t);
					continue;
				}
				{
					Token *next = try_bracket_orelse(t);
					if (next) {
						t = next;
						continue;
					}
				}
				// No orelse — emit bracket contents with raw stripping
				Token *bclose = tok_match(t);
				while (t != bclose) {
					if ((t->flags & TF_OPEN) && is_stmt_expr_open(t)) {
						t = emit_stmt_expr(t);
						continue;
					}
					t = emit_advance(t);
				}
				t = emit_advance(t);
				continue;
			}
			if (FEAT(F_ORELSE)) {
				Token *next = try_typeof_orelse(t);
				if (next) {
					t = next;
					continue;
				}
			}
			// Enum-body shadow queue + defense-in-depth defer rejection
			walk_balanced_tail(&t);
		}
	}
	return tok_next(end);
}

static void emit_token_range_nested(Token *start, Token *end, bool with_orelse) {
	for (Token *t = start; t && t != end && t->kind != TK_EOF;) {
		if (is_stmt_expr_open(t) && tok_match(t)) {
			walk_balanced(t, true);
			t = tok_next(tok_match(t));
			continue;
		}
		/* Subscripts inside orelse-lowered ranges (single-eval LHS copies,
		 * fallback values) must get the same -fbounds-check wrapping as
		 * the main emission loop — otherwise `v[i] = g() orelse 1;`
		 * stores through a tracked array unchecked.  Found by the
		 * contexts suite's fixed-point oracle. */
		{
			Token *bc = try_bounds_checks(t);
			if (bc && bc != t) {
				t = bc;
				continue;
			}
		}
		if ((t->flags & TF_OPEN) && (match_ch(t, '(') || match_ch(t, '['))) {
			Token *close = tok_match(t);
			if (close && close != end) {
				emit_tok(t);
				if (with_orelse) emit_token_range_orelse(tok_next(t), close);
				else
					emit_token_range_nested(tok_next(t), close, false);
				emit_tok(close);
				t = tok_next(close);
				continue;
			}
		}
		reject_defer_in_expr_context(t);
		t = emit_advance(t);
	}
}

/* First P1_IS_ORELSE_KW token in [start, end) at group depth 0 (jumps
 * balanced groups, skips prep dirs); NULL if none. */
static Token *find_ann_orelse(Token *start, Token *end) {
	for (Token *s = start; s && s != end && s->kind != TK_EOF; s = tok_next(s)) {
		if (s->kind == TK_PREP_DIR) continue;
		if (s->flags & TF_OPEN) {
			s = tok_match(s);
			continue;
		}
		if (tok_ann(s) & P1_IS_ORELSE_KW) return s;
	}
	return NULL;
}

static void emit_token_range_orelse(Token *start, Token *end) {
	Token *orelse = find_ann_orelse(start, end);
	if (!orelse) {
		emit_token_range_nested(start, end, true);
		return;
	}
#ifdef PRISM_DEBUG
	reject_orelse_side_effects(start,
				   orelse,
				   "'orelse' in array dimension / typeof",
				   "in a chained 'orelse' (would be evaluated twice); "
				   "hoist the expression to a variable first",
				   true,
				   true,
				   false);
#endif
	emit_orelse_ternary(start, orelse, tok_next(orelse), end);
}

static void validate_bracket_orelse(Token *oe) {
	Token *act = tok_next(oe);
	if (act && (act->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)))
		error_tok(oe,
			  "'orelse' with control flow cannot be used inside "
			  "array dimensions or typeof expressions");
	if (act && match_ch(act, '{'))
		error_tok(oe,
			  "'orelse' block form cannot be used inside "
			  "array dimensions or typeof expressions");
	/* Empty fallback (`[n orelse ]`): the ternary expansion would emit
	 * `n ? n : ()` — invalid C leaking to the backend.  The `;`/`,` empty
	 * cases are covered by the statement/init validators; the
	 * close-delimiter case belongs here.  Found by the insertion suite. */
	if (!act || (act->flags & TF_CLOSE) || match_ch(act, ';') || match_ch(act, ','))
		error_tok(oe, "expected expression after 'orelse' in array dimension");
}

static Token *scan_bracket_orelse(Token *open, Token *close, Token **paren_open_out) {
	for (Token *t = tok_next(open); t && t != close; t = tok_next(t)) {
		if (t->kind == TK_PREP_DIR) continue;
		if (t->flags & TF_OPEN) {
			Token *pp = tok_match(t);
			/* `( … orelse … )` ending flush at `]` — macro-hygiene wrap. */
			if (match_ch(open, '[') && match_ch(t, '(') && pp && tok_next(pp) == close) {
				Token *hit = find_ann_orelse(tok_next(t), pp);
				if (hit) {
					if (paren_open_out) *paren_open_out = t;
					return hit;
				}
			}
			t = pp;
			continue;
		}
		if (tok_ann(t) & P1_IS_ORELSE_KW) return t;
	}
	return NULL;
}

typedef struct {
	int oe_count, oe_next, dim_count, dim_next;
	unsigned *oe_ids, *dim_ids; // arena-allocated snapshots (unbounded)
} BOFrame;

static void bo_snapshot_ids(unsigned **dst, const unsigned *src, int n) {
	if (n <= 0) {
		*dst = NULL;
		return;
	}
	*dst = arena_alloc(&ctx->main_arena, (size_t)n * sizeof(unsigned));
	memcpy(*dst, src, (size_t)n * sizeof(unsigned));
}

static inline void emit_type_with_pragma_prelude(
    Token *pragma_start, Token *type_start, Token *type_end, Token *raw_tok, bool is_split) {
	int saved_oe = ctx->bracket_oe_count, saved_dim = ctx->bracket_dim_count;
	ctx->bracket_oe_count = 0;
	ctx->bracket_dim_count = 0;
	if (pragma_start != type_start) emit_range(pragma_start, raw_tok ? raw_tok : type_start);
	emit_type_range(type_start, type_end, false, is_split);
	ctx->bracket_oe_count = saved_oe;
	ctx->bracket_dim_count = saved_dim;
}

static void bo_restore_queue(unsigned **dst, int *dst_cap, const unsigned *src, int count) {
	if (count <= 0) return;
	if (*dst_cap < count) {
		int old_cap = *dst_cap;
		size_t nc = vec_grow_cap((size_t)old_cap, (size_t)count, 16);
		*dst = arena_realloc(&ctx->main_arena,
				     *dst,
				     (size_t)old_cap * sizeof(unsigned),
				     nc * sizeof(unsigned));
		*dst_cap = (int)nc;
	}
	memcpy(*dst, src, (size_t)count * sizeof(unsigned));
}

static void bo_restore(BOFrame *f) {
	ctx->bracket_oe_count = f->oe_count;
	ctx->bracket_oe_next = f->oe_next;
	ctx->bracket_dim_count = f->dim_count;
	ctx->bracket_dim_next = f->dim_next;
	bo_restore_queue(&ctx->bracket_oe_ids, &ctx->bracket_oe_cap, f->oe_ids, f->oe_count);
	bo_restore_queue(&ctx->bracket_dim_ids, &ctx->bracket_dim_cap, f->dim_ids, f->dim_count);
}

// are also hoisted to preserve C99 left-to-right VLA evaluation order.
static void emit_bracket_orelse_temps(Token *start, Token *end) {
	ctx->bracket_oe_count = 0;
	ctx->bracket_oe_next = 0;
	ctx->bracket_dim_count = 0;
	ctx->bracket_dim_next = 0;

	// Phase 1: collect all brackets and identify which have orelse.
	typedef struct {
		Token *open;
		Token *close;
		Token *orelse;
		Token *paren_open;
	} BracketInfo;

	ArenaMark bracket_mark = arena_mark(&ctx->main_arena);
	int bracket_cap = 16;
	BracketInfo *brackets = arena_alloc_uninit(&ctx->main_arena, bracket_cap * sizeof(BracketInfo));
	int bracket_count = 0;
	bool any_orelse = false;
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = tok_next(t)) {
		if (t->tag & TT_ATTR) {
			Token *a = tok_next(t);
			if (a && match_ch(a, '(') && tok_match(a)) t = tok_match(a);
			continue;
		}
		if (!match_ch(t, '[')) continue;
		if (t->flags & TF_C23_ATTR) {
			t = tok_match(t);
			continue;
		}
		Token *close = tok_match(t);
		if (!close) continue;
		ARENA_ENSURE_CAP(
		    &ctx->main_arena, brackets, bracket_count + 1, bracket_cap, 16, BracketInfo);
		Token *paren_open_found = NULL;
		Token *orelse_found = scan_bracket_orelse(t, close, &paren_open_found);
		brackets[bracket_count++] = (BracketInfo){t, close, orelse_found, paren_open_found};
		if (orelse_found) any_orelse = true;
		t = close;
	}

	if (!any_orelse) {
		arena_restore(&ctx->main_arena, bracket_mark);
		return;
	}

	for (int i = 0; i < bracket_count; i++) {
		if (brackets[i].orelse) {
			// Orelse bracket: hoist LHS
			ARENA_ENSURE_CAP(&ctx->main_arena,
					 ctx->bracket_oe_ids,
					 ctx->bracket_oe_count,
					 ctx->bracket_oe_cap,
					 16,
					 unsigned);
			unsigned oe = ctx->ret_counter++;
			ctx->bracket_oe_ids[ctx->bracket_oe_count++] = oe;
			emit_ll_temp(emit_prism_oe, oe);
			emit_token_range_orelse(brackets[i].paren_open ? tok_next(brackets[i].paren_open)
								       : tok_next(brackets[i].open),
						brackets[i].orelse);
			OUT_LIT(");");
		} else {
			// hoist to preserve left-to-right VLA evaluation order.
			Token *dim_start = tok_next(brackets[i].open);
			Token *dim_end = brackets[i].close;
			ARENA_ENSURE_CAP(&ctx->main_arena,
					 ctx->bracket_dim_ids,
					 ctx->bracket_dim_count,
					 ctx->bracket_dim_cap,
					 16,
					 unsigned);
			if (dim_start == dim_end ||
			    (tok_next(dim_start) == dim_end &&
			     (dim_start->kind == TK_NUM || match_ch(dim_start, '*')))) {
				ctx->bracket_dim_ids[ctx->bracket_dim_count++] = (unsigned)-1;
				continue;
			}
			unsigned dim = ctx->ret_counter++;
			ctx->bracket_dim_ids[ctx->bracket_dim_count++] = dim;
			emit_ll_temp(emit_prism_dim, dim);
			emit_token_range_orelse(dim_start, dim_end);
			OUT_LIT(");");
		}
	}
	arena_restore(&ctx->main_arena, bracket_mark);
}

static Token *walk_balanced_orelse(Token *tok) {
	Token *end = tok_match(tok);
	if (!end) {
		emit_tok(tok);
		return tok_next(tok);
	}
	Token *paren_open = NULL;
	Token *orelse_found = scan_bracket_orelse(tok, end, &paren_open);
	if (!orelse_found) {
		if (match_ch(tok, '[') && ctx->bracket_dim_next < ctx->bracket_dim_count) {
			unsigned dim = ctx->bracket_dim_ids[ctx->bracket_dim_next++];
			if (dim != (unsigned)-1) {
				emit_tok(tok); // emit [
				emit_prism_dim(dim);
				emit_tok(end); // emit ]
				return tok_next(end);
			}
		}
#ifdef PRISM_DEBUG
		Token *prev = NULL;
#endif
		for (Token *t = tok; t != tok_next(end) && t->kind != TK_EOF;) {
			if (t != tok && t != end && (match_ch(t, '(') || match_ch(t, '[')) &&
			    (t->flags & TF_OPEN) && tok_match(t) && !is_stmt_expr_open(t)) {
				if (match_ch(t, '[') &&
				    ctx->bracket_dim_next < ctx->bracket_dim_count) {
					unsigned dim = ctx->bracket_dim_ids[ctx->bracket_dim_next++];
					if (dim != (unsigned)-1) {
						emit_tok(t);
						emit_prism_dim(dim);
						emit_tok(tok_match(t));
						t = tok_next(tok_match(t));
						continue;
					}
				}
				t = walk_balanced_orelse(t);
				continue;
			}
			if (t != tok && t != end && FEAT(F_ORELSE) && (t->tag & TT_TYPEOF) && tok_next(t) &&
			    match_ch(tok_next(t), '(')) {
				emit_tok(t);	 // typeof keyword
				t = tok_next(t); // (
				t = walk_balanced_orelse(t);
				continue;
			}
			// handles zero-init, defer, goto, raw stripping, etc.
			if (t != tok && t != end && (t->flags & TF_OPEN) && is_stmt_expr_open(t)) {
				t = walk_balanced(t, true);
				continue;
			}
#ifdef PRISM_DEBUG
			/* Phase 1 rejects over-paren wraps; keep as assert. */
			if (is_orelse_kw_shadow(t) && orelse_shadow_is_kw(prev))
				error_tok(t,
					  "'orelse' inside array dimension could not be transformed; "
					  "if wrapped in outer parentheses, remove them: "
					  "use '[f() orelse 1]' not '[(f() orelse 1)]'");
			prev = t;
#endif
			if (__builtin_expect(FEAT(F_BOUNDS_CHECK), 0)) {
				Token *bc = try_bounds_checks(t);
				if (bc) {
					t = bc;
					continue;
				}
			}
			walk_balanced_tail(&t);
		}
		return tok_next(end);
	}
	// Control-flow / block-form rejected in Phase 1G.
#ifdef PRISM_DEBUG
	validate_bracket_orelse(orelse_found);
#endif
	Token *lhs_start = paren_open ? tok_next(paren_open) : tok_next(tok);
	Token *rhs_start = tok_next(orelse_found);
	Token *rhs_end = paren_open ? tok_match(paren_open) : end;
	bool is_bracket = match_ch(tok, '[');
	emit_tok(tok); // emit [ or (
	if (is_bracket && ctx->bracket_oe_next < ctx->bracket_oe_count) {
		unsigned oe = ctx->bracket_oe_ids[ctx->bracket_oe_next++];
		emit_prism_oe_ternary_ref(oe);
		emit_token_range_orelse(rhs_start, rhs_end);
		OUT_LIT(")");
	} else {
#ifdef PRISM_DEBUG
		reject_orelse_side_effects(lhs_start,
					   orelse_found,
					   is_bracket ? "'orelse' in array dimension / typeof"
						      : "'orelse' in typeof",
					   "in the LHS (would be evaluated twice); "
					   "hoist the expression to a variable first",
					   true,
					   is_bracket,
					   false);
#endif
		emit_orelse_ternary(lhs_start, orelse_found, rhs_start, rhs_end);
	}
	emit_tok(end); // emit ] or )
	return tok_next(end);
}

static inline void decl_emit(Token *t, bool emit) {
	if (emit) emit_tok(t);
}

static inline Token *decl_array_dims(Token *t, bool emit, bool *vla) {
	for (;;) {
		if (!t || t->kind == TK_EOF) return t;
		if (match_ch(t, '[')) {
			tok_ann(t) |= P1_DECL_BRACKET;
			/* C23 attributes between array dims: emit with inline orelse
			 * but skip the FIFO-consuming bracket orelse path. */
			if (t->flags & TF_C23_ATTR) {
				t = emit ? emit_c23_attr(t) : tok_next(tok_match(t));
				continue;
			}
			if (array_size_is_vla(t)) *vla = true;
			if (emit && FEAT(F_ORELSE)) t = walk_balanced_orelse(t);
			else
				t = walk_balanced(t, emit);
			continue;
		}
		/* GNU `__attribute__((...))` between dims — keep scanning so a later
		 * `[m orelse …]` stays on the FIFO path (otherwise r.end stops early,
		 * temps under-count, and leftover dims leak / corrupt after `= 0`). */
		if ((t->tag & TT_ATTR) && tok_next(t) && match_ch(tok_next(t), '(')) {
			Token *after = decl_noise(t, emit);
			if (after != t && match_ch(after, '[')) {
				t = after;
				continue;
			}
			return after;
		}
		return t;
	}
}

static DeclResult parse_declarator(Token *tok, bool emit) {
	DeclResult r = {.end = tok};
	bool is_vla = false;
	int ptr_depth = 0;

#define DECL_EAT_PTRS(extra_ptr_action)                                                                      \
	while (tok && tok->kind != TK_EOF) {                                                                 \
		Token *_n = decl_noise(tok, emit);                                                           \
		if (_n != tok) {                                                                             \
			tok = _n;                                                                            \
			continue;                                                                            \
		}                                                                                            \
		if (match_ch(tok, '*')) {                                                                    \
			r.is_pointer = true;                                                                 \
			r.is_const = false;                                                                  \
			extra_ptr_action;                                                                    \
			if (++ptr_depth > 1024) {                                                            \
				warn_tok(tok, "pointer depth exceeds 1024; zero-initialization skipped");    \
				r.end = NULL;                                                                \
				return r;                                                                    \
			}                                                                                    \
			decl_emit(tok, emit);                                                                \
			tok = tok_next(tok);                                                                 \
		} else if ((tok->tag & TT_QUALIFIER) &&                                                      \
			   !(is_soft_keyword_identifier(tok) && soft_keyword_decl_name_boundary(tok))) {     \
			if (r.is_pointer && (tok->tag & TT_CONST)) r.is_const = true;                        \
			decl_emit(tok, emit);                                                                \
			tok = tok_next(tok);                                                                 \
		} else                                                                                       \
			break;                                                                               \
	}

	DECL_EAT_PTRS((void)0)

	int nested_paren = 0;
	if (match_ch(tok, '(')) {
		Token *peek = skip_noise(tok_next(tok));
		if (!match_ch(peek, '*') && !match_ch(peek, '(') && !is_valid_varname(peek)) {
			r.end = NULL;
			return r;
		}
		decl_emit(tok, emit);
		tok = tok_next(tok);
		nested_paren = 1;
		r.has_paren = true;
		DECL_EAT_PTRS(r.paren_pointer = true)
		while (match_ch(tok, '(')) {
			if (++nested_paren > 1024) {
				warn_tok(tok, "parenthesization depth exceeds 1024");
				r.end = NULL;
				return r;
			}
			decl_emit(tok, emit);
			tok = tok_next(tok);
			DECL_EAT_PTRS(r.paren_pointer = true)
		}
	}
#undef DECL_EAT_PTRS

	if (!is_valid_varname(tok)) {
		r.end = NULL;
		return r;
	}
	r.var_name = tok;
	decl_emit(tok, emit);
	tok = tok_next(tok);
	tok = decl_noise(tok, emit);
	if (r.has_paren && match_ch(tok, '(')) r.is_func_decl = true;
	if (r.has_paren && match_ch(tok, '[')) {
		r.is_array = true;
		r.paren_array = true;
		tok = decl_array_dims(tok, emit, &is_vla);
	}
	while (r.has_paren && nested_paren > 0) {
		while (match_ch(tok, '(') || match_ch(tok, '[')) {
			if (match_ch(tok, '(')) {
				if (emit && FEAT(F_ORELSE)) tok = walk_balanced_orelse(tok);
				else
					tok = walk_balanced(tok, emit);
			} else {
				r.is_array = true;
				r.paren_array = true;
				tok = decl_array_dims(tok, emit, &is_vla);
			}
		}
		if (!match_ch(tok, ')')) {
			r.end = NULL;
			return r;
		}
		decl_emit(tok, emit);
		tok = tok_next(tok);
		nested_paren--;
	}

	if (match_ch(tok, '(')) {
		if (!r.has_paren) {
			r.end = NULL;
			return r;
		}
		r.is_func_ptr = true;
		if (emit && FEAT(F_ORELSE)) tok = walk_balanced_orelse(tok);
		else
			tok = walk_balanced(tok, emit);
	}

	if (match_ch(tok, '[')) {
		r.is_array = true;
		tok = decl_array_dims(tok, emit, &is_vla);
	}
	while (tok && tok->kind != TK_EOF) {
		Token *next = decl_noise(tok, emit);
		if (next != tok) {
			tok = next;
			continue;
		}
		if (tok->tag & TT_ASM) {
			decl_emit(tok, emit);
			tok = tok_next(tok);
			if (tok && match_ch(tok, '(')) tok = walk_balanced(tok, emit);
		} else
			break;
	}

	r.has_init = match_ch(tok, '=');
	r.is_vla = is_vla;
	r.end = tok;
	return r;
}

static void emit_noise_between_raws(Token *first_raw, Token *last_raw) {
	if (first_raw == last_raw) return;
	for (Token *t = tok_next(first_raw); t && t != last_raw && t->kind != TK_EOF; t = tok_next(t)) {
		if (t->flags & TF_RAW) continue;
		if ((t->flags & TF_OPEN) && tok_match(t)) {
			// Balanced group (C23 attr [[...]], GNU attr((...))): walk & emit
			Token *m = tok_match(t);
			for (Token *u = t; u && u != tok_next(m) && u->kind != TK_EOF;) {
				if ((u->flags & TF_OPEN) && is_stmt_expr_open(u)) {
					u = emit_stmt_expr(u);
					continue;
				}
				emit_tok(u);
				u = tok_next(u);
			}
			t = m;
		} else
			emit_tok(t);
	}
}

static Token *emit_raw_verbatim_to_semicolon(Token *tok) {
	while (tok && tok->kind != TK_EOF && !match_ch(tok, ';')) {
		if (tok->flags & TF_OPEN) tok = walk_balanced(tok, true);
		else if (FEAT(F_ORELSE) && (tok->tag & TT_TYPEOF)) {
			Token *next = try_typeof_orelse(tok);
			if (next) {
				tok = next;
				continue;
			}
			tok = emit_advance(tok);
		} else {
			tok = emit_advance(tok);
		}
	}
	if (tok && match_ch(tok, ';')) {
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
static bool prism_memset_follows(Token *var) {
	if (!var) return false;
	/* Find the terminating ';' of the declaration statement. */
	Token *t = var;
	for (int guard = 0; t && t->kind != TK_EOF && guard < 4096; guard++) {
		if ((t->flags & TF_OPEN) && tok_match(t)) {
			t = tok_next(tok_match(t));
			continue;
		}
		if (match_ch(t, ';')) break;
		if (match_ch(t, '}')) return false;
		t = tok_next(t);
	}
	if (!t || !match_ch(t, ';')) return false;
	t = tok_next(t);
	/* Scan across a small run of canonical zeroing statements (earlier
	 * declarators' memsets precede this var's in multi-decl splits). */
	for (int stmts = 0; t && t->kind != TK_EOF && stmts < 8; stmts++) {
		if (t->kind == TK_IDENT && t->len == 16 && prism_memeq_static(tok_loc(t), "__builtin_memset", 16)) {
			Token *p = tok_next(t);
			if (!p || !match_ch(p, '(') || !tok_match(p)) return false;
			Token *close = tok_match(p);
			Token *a = tok_next(p);
			/* optional (void *) cast */
			if (a && match_ch(a, '(') && tok_match(a)) a = tok_next(tok_match(a));
			if (a && match_ch(a, '&')) a = tok_next(a);
			if (a && a->kind == TK_IDENT && a->len == var->len &&
			    prism_memeq_runtime_sized(tok_loc(a), tok_loc(var), var->len))
				return true;
			t = tok_next(close);
			if (t && match_ch(t, ';')) t = tok_next(t);
			continue;
		}
		if (match_ch(t, '{') && tok_match(t)) {
			/* candidate byte-loop block: require a __prism_p_ ident and
			 * `& var` in its opening tokens */
			Token *close = tok_match(t);
			bool has_p = false, has_var = false;
			int k = 0;
			for (Token *s = tok_next(t); s && s != close && k < 16; s = tok_next(s), k++) {
				if (s->kind == TK_IDENT && s->len >= 10 &&
				    prism_memeq_static(tok_loc(s), "__prism_p_", 10))
					has_p = true;
				if (match_ch(s, '&') && tok_next(s) &&
				    tok_next(s)->kind == TK_IDENT &&
				    tok_next(s)->len == var->len &&
				    prism_memeq_runtime_sized(tok_loc(tok_next(s)), tok_loc(var), var->len))
					has_var = true;
			}
			if (!has_p) return false;
			if (has_var) return true;
			t = tok_next(close);
			continue;
		}
		return false;
	}
	return false;
}

static void emit_typeof_memsets(Token **vars, int count, bool has_volatile, bool has_const) {
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
			out_uint(ctx->ret_counter);
			OUT_LIT(" = (");
			out_str(vol, vol_len);
			if (has_const) OUT_LIT("char *)(void *)&");
			else
				OUT_LIT("char *)&");
			OUT_TOK(vars[i]);
			OUT_LIT("; for (unsigned long long __prism_i_");
			out_uint(ctx->ret_counter);
			OUT_LIT(" = 0; __prism_i_");
			out_uint(ctx->ret_counter);
			OUT_LIT(" < sizeof(");
			OUT_TOK(vars[i]);
			OUT_LIT("); __prism_i_");
			out_uint(ctx->ret_counter);
			OUT_LIT("++) __prism_p_");
			out_uint(ctx->ret_counter);
			OUT_LIT("[__prism_i_");
			out_uint(ctx->ret_counter);
			OUT_LIT("] = 0; }");
			ctx->ret_counter++;
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

static Token *emit_break_continue_defer(Token *tok) {
	bool is_break = tok->tag & TT_BREAK;
	Token *kw = tok;
	Token *after = tok_next(tok);
	/* C23/GNU labeled break/continue: `break outer;` / `continue outer;`.
	 * Must preserve the label and unwind with DEFER_TO_DEPTH (like goto),
	 * not DEFER_BREAK/CONTINUE — those only reach the innermost loop. */
	Token *label = NULL;
	if (after && is_identifier_like(after)) label = after;

	if (label && FEAT(F_DEFER)) {
		P1LabelResult info = p1_label_find(label, current_func_idx);
		int td = info.tok ? info.scope_depth : 0;
		if (goto_has_defers(td)) emit_goto_defers(td);
	} else if (FEAT(F_DEFER) && control_flow_has_defers(is_break)) {
		emit_defers(is_break ? DEFER_BREAK : DEFER_CONTINUE);
	}
	out_char(' ');
	OUT_TOK(kw);
	if (label) {
		out_char(' ');
		OUT_TOK(label);
		tok = tok_next(label);
	} else {
		tok = after;
	}
	out_char(';');
	if (match_ch(tok, ';')) tok = tok_next(tok);
	return tok;
}

static int p1_goto_exits(Token *goto_tok, int func_idx); // forward decl

static Token *emit_goto_defer(Token *tok) {
	Token *goto_tok = tok;
	tok = tok_next(tok);
	if (FEAT(F_DEFER) && is_identifier_like(tok)) {
		P1LabelResult info = p1_label_find(tok, current_func_idx);
		int td = info.tok ? info.scope_depth : ctx->block_depth;
		int exits = p1_goto_exits(goto_tok, current_func_idx);
		if (exits > 0) {
			td = ctx->block_depth - exits;
			if (td < 0) td = 0;
		}
		if (goto_has_defers(td)) emit_goto_defers(td);
	}
	OUT_LIT(" goto ");
	if (is_identifier_like(tok)) {
		OUT_TOK(tok);
		tok = tok_next(tok);
	}
	out_char(';');
	if (match_ch(tok, ';')) tok = tok_next(tok);
	return tok;
}

static Token *emit_orelse_fallback_value(Token *tok, Token *stop_comma, Token **chain_next) {
	*chain_next = NULL;
	while (tok->kind != TK_EOF) {
		if (FEAT(F_ORELSE) && (tok->tag & TT_TYPEOF)) {
			Token *next = try_typeof_orelse(tok);
			if (next) {
				tok = next;
				continue;
			}
		}
		if (tok->flags & TF_OPEN) {
			/* Phase 1 p1d_find_stmt_expr_fallback owns this reject. */
#ifdef PRISM_DEBUG
			if (is_stmt_expr_open(tok))
				error_tok(tok,
					  "GNU statement expressions in orelse fallback values are not "
					  "supported; use 'orelse { ... }' block form instead");
#endif
			tok = walk_balanced(tok, true);
			continue;
		}
		if (match_ch(tok, ';') || (stop_comma && tok == stop_comma)) break;
		if (is_orelse_keyword(tok)) {
			*chain_next = tok_next(tok);
			return tok;
		}
		tok = emit_advance(tok);
	}
	return tok;
}

static Token *emit_orelse_block_body(Token *tok) {
	Token *blk_close = tok_match(tok);
	/* Tokenizer delimiter-matching completeness (§14.4) guarantees a match. */
#ifdef PRISM_DEBUG
	if (!blk_close) error_tok(tok, "unterminated orelse block");
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
static Token *handle_const_orelse_fallback(Token *tok,
					   Token *orelse_tok,
					   Token *val_start,
					   Token *decl_start,
					   DeclResult *decl,
					   Token *type_start,
					   TypeSpecResult *type,
					   Token *pragma_start,
					   Token *stop_comma) {
	unsigned oe_id = ctx->ret_counter++;
	// Function pointers: return-type const lives in the type specifier and must
	// be preserved;
	bool strip_type_const = !decl->is_pointer && !decl->is_func_ptr;
	bool has_const_typedef = false;
	if (strip_type_const) {
		for (Token *t = type_start; t != type->end; t = tok_next(t)) {
			if (is_const_typedef(t)) {
				has_const_typedef = true;
				break;
			}
			if ((t->tag & TT_TYPEOF) && t->len != 11) {
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
	for (Token *t = decl_start; t != decl->var_name; t = tok_next(t)) {
		if (t->tag & TT_CONST) continue;
		emit_tok(t);
	}

	OUT_LIT(" __prism_oe_");
	out_uint(oe_id);
	{
		Token *t = tok_next(decl->var_name);
		while (t && t != decl->end && t->kind != TK_EOF) {
			if (match_ch(t, '[') && !(t->flags & TF_C23_ATTR)) {
				t = walk_balanced_orelse(t);
			} else {
				emit_tok(t);
				t = tok_next(t);
			}
		}
	}
	OUT_LIT(" = (");
	emit_range(val_start, orelse_tok);
	OUT_LIT(");");
	for (;;) {
		if ((tok->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) || match_ch(tok, '{')) {
			OUT_LIT(" if (!__prism_oe_");
			out_uint(oe_id);
			OUT_LIT(")");
			tok = emit_orelse_action(tok, NULL, false, false, stop_comma);
			break;
		}

		/* volatile/_Atomic temps must not use `t = t ? t : fb` (double load). */
		if (type->has_volatile || type->has_atomic) {
			OUT_LIT(" if (!__prism_oe_");
			out_uint(oe_id);
			OUT_LIT(") __prism_oe_");
			out_uint(oe_id);
			OUT_LIT(" = (");
			Token *chain_next;
			tok = emit_orelse_fallback_value(tok, stop_comma, &chain_next);
			OUT_LIT(");");
			if (!chain_next) break;
			tok = chain_next;
			continue;
		}

		emit_prism_oe_chain_assign(oe_id);
		Token *chain_next;
		tok = emit_orelse_fallback_value(tok, stop_comma, &chain_next);
		OUT_LIT(");");
		if (!chain_next) break;
		tok = chain_next;
	}

	EMIT_PRAGMA_PRELUDE();
#undef EMIT_PRAGMA_PRELUDE
	emit_type_range(type_start, type->end, false, false);
	parse_declarator(decl_start, true);
	OUT_LIT(" = __prism_oe_");
	out_uint(oe_id);
	out_char(';');
	return tok;
}

static Token *paren_scan_skip_nested(Token *t) {
	if ((t->flags & TF_OPEN) && match_ch(t, '[') && tok_match(t)) return tok_match(t);
	if ((t->tag & TT_TYPEOF) && tok_next(t) && match_ch(tok_next(t), '(') && tok_match(tok_next(t)))
		return tok_match(tok_next(t));
	if (is_stmt_expr_open(t) && tok_match(t)) return tok_match(t);
	return NULL;
}

/* `int orelse 0` / `int * orelse 0` inside sizeof/_Alignof/typeof/_Generic
 * parens: orelse_kw_at treats type-specifier predecessors as declarator names
 * (`int orelse = 0`), so the operator check misses them and the keyword leaks.
 * Param-list declarators return early via paren_is_function_declarator_params;
 * a typedef named orelse after a type (`unsigned orelse`) stays an identifier. */
static bool orelse_after_type_in_parens(Token *t, Token *prev) {
	if (!(t->tag & TT_ORELSE) || !prev || orelse_is_label_or_goto_target(t, prev)) return false;
	if (typedef_lookup(t) || is_known_typedef(t)) return false;
	Token *p = prev;
	/* Peel abstract-declarator pointer stars / trailing quals: `int *`,
	 * `int *const`, `char **`. Stop before binary `*` (expr * expr). */
	while (p && (match_ch(p, '*') || (p->tag & TT_QUALIFIER)))
		p = tok_walk_back(tok_idx(p), WB_PAST_NOISE);
	if (!p) return false;
	if (p->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_BITINT | TT_ALIGNAS | TT_TYPEOF)) return true;
	if (is_known_typedef(p) || token_ends_sue_type_specifier(p) ||
	    close_paren_ends_type_specifier_ctor(p))
		return true;
	return false;
}

static void check_paren_orelse_defer(Token *open, bool ctrl_cond) {
	if (!ctrl_cond && paren_is_function_declarator_params(open)) return;
	Token *close = tok_match(open);
	if (is_stmt_expr_open(open) && close) return;
	for (Token *pi = open, *t = tok_next(open); t != close; pi = t, t = tok_next(t)) {
		Token *skipped = paren_scan_skip_nested(t);
		if (skipped) {
			t = skipped;
			continue;
		}
		if (orelse_kw_at(t, pi) || (!ctrl_cond && orelse_after_type_in_parens(t, pi))) {
			if (orelse_is_label_or_goto_target(t, pi)) continue;
			if (tok_ann(t) & P1_OE_DECL_INIT) continue;
			if (ctrl_cond) {
				if (!is_orelse_value_fallback(tok_next(t)))
					error_tok(t,
						  "'orelse' with statement or block action cannot be used "
						  "in control-statement conditions (if/for/while/switch)");
			} else {
				error_tok(t,
					  "'orelse' cannot be used inside parentheses "
					  "(it must appear at the top level of a declaration)");
			}
		}
		/* Paren scan: do not use is_defer_kw — it treats prev=='(' as
		 * expression position (call args / declarators). Inside an
		 * already-open paren group, a statement-shaped defer is always
		 * illegal (`while ((defer …))`, `(defer (void)0, 1)`). */
		if (FEAT(F_DEFER) && (t->tag & TT_DEFER) && !typedef_lookup(t) &&
		    !(pi && (pi->tag & (TT_MEMBER | TT_GOTO))) &&
		    !(pi && is_gnu_label_decl_head(pi)) && !(pi && _equal_2(pi, "&&")) &&
		    tok_next(t) && !match_ch(tok_next(t), ':') &&
		    !(tok_next(t)->tag & TT_ASSIGN) &&
		    !match_ch(tok_next(t), ')') && !match_ch(tok_next(t), ',') &&
		    !match_ch(tok_next(t), ']'))
			error_tok(t, "defer cannot be at top level of a parenthesized expression");
	}
}

/* typeof(expr orelse fb) is allowed; typeof(int orelse 0) must still reject. */
static void check_typeof_paren_orelse_type_junk(Token *open) {
	if (!FEAT(F_ORELSE) || !match_ch(open, '(') || !tok_match(open)) return;
	Token *close = tok_match(open);
	for (Token *pi = open, *t = tok_next(open); t && t != close; pi = t, t = tok_next(t)) {
		Token *skipped = paren_scan_skip_nested(t);
		if (skipped) {
			t = skipped;
			continue;
		}
		if (orelse_after_type_in_parens(t, pi))
			error_tok(t,
				  "'orelse' cannot be used inside parentheses "
				  "(it must appear at the top level of a declaration)");
	}
}

static void check_orelse_in_parens(Token *open) {
	/* Several Pass-2 debug backstops are reached from generic balanced-group
	 * walks. Braces (initializer lists) and brackets have their own Phase-1
	 * classifiers; treating them as parentheses false-rejects ordinary
	 * `defer` identifiers in `{ defer }` and valid `[i orelse j]` indexes. */
	if (!open || !match_ch(open, '(')) return;
	check_paren_orelse_defer(open, false);
}

#ifdef PRISM_DEBUG
/* Phase 1 rejects orelse in ctrl-condition parens; Pass 2 assert only. */
static void check_orelse_in_ctrl_paren(Token *open) {
	check_paren_orelse_defer(open, true);
}
#endif

typedef struct {
	Token *orelse_tok;
	bool is_const_fallback;
} OrelseInitInfo;

typedef struct {
	Token *stop_comma;
	bool has_const_qual;
	bool is_struct_value;
} OrelseDeclTargetInfo;

#ifdef PRISM_DEBUG
/* Phase 1 rejects orelse in for-init via ctrl-paren scan; Pass 2 assert. */
static inline void reject_orelse_in_for_init(Token *tok) {
	if (in_for_init()) error_tok(tok, "orelse cannot be used in for-loop initializers");
}
#endif

static inline bool is_orelse_value_fallback(Token *after_oe) {
	return after_oe && !(after_oe->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) &&
	       !match_ch(after_oe, '{') && !match_ch(after_oe, ';');
}

/* LHS of `… = expr orelse …` uses *, [], or . / -> — Pass 2 may typeof(RHS). */
static bool bare_lhs_has_indirection(Token *lhs_start, Token *eq_tok) {
	for (Token *s = lhs_start; s && s != eq_tok; s = tok_next(s))
		if ((s->tag & TT_MEMBER) || match_ch(s, '*') ||
		    (match_ch(s, '[') && (s->flags & TF_OPEN)))
			return true;
	return false;
}

#ifdef PRISM_DEBUG
/* Phase 1 owns missing-action rejects (p1d_scan_init_orelse / p1d_validate_bare_orelse). */
static inline void require_orelse_action(Token *tok, Token *stop_comma) {
	if (match_ch(tok, ';') || (stop_comma && tok == stop_comma))
		error_tok(tok, "expected statement after 'orelse'");
}
#endif

static inline void flush_typeof_memsets(Token **vars, int *count, TypeSpecResult *type, int base) {
	if (*count > base) {
		emit_typeof_memsets(&vars[base], *count - base, type->has_volatile, type->has_const);
		*count = base;
	}
}

static OrelseDeclTargetInfo
analyze_decl_orelse_target(Token *tok, Token *type_start, TypeSpecResult *type, DeclResult *decl) {
	OrelseDeclTargetInfo info = {
	    .stop_comma = find_boundary_comma(tok),
	    .has_const_qual = has_effective_const_qual(type_start, type, decl),
	    .is_struct_value = type->is_struct && !type->is_enum && !decl->is_pointer && !decl->is_array,
	};
	(void)type_start;
	/* Array/struct shape rejects: Phase 1 reject_decl_orelse_value_shape.
	 * Missing-action rejects: Phase 1 p1d_scan_init_orelse. */
#ifdef PRISM_DEBUG
	require_orelse_action(tok, info.stop_comma);
#endif
	return info;
}

static OrelseInitInfo
scan_decl_orelse(Token *decl_end, Token *type_start, TypeSpecResult *type, DeclResult *decl) {
	OrelseInitInfo info = {0};
	if (!decl->has_init || !FEAT(F_ORELSE)) return info;
	/* Macro-hygiene parens wrapping the whole initializer were unlinked by
	 * Phase 1 (p1d_scan_init_orelse), so a decl-init orelse keyword always
	 * sits at group depth 0 here — this walk only locates the annotation. */
	int pd = 0;
	for (Token *scan = tok_next(decl_end); scan && scan->kind != TK_EOF; scan = tok_next(scan)) {
		if (scan->flags & TF_OPEN) {
#ifdef PRISM_DEBUG
			if (pd == 0) check_orelse_in_parens(scan);
#endif
			pd++;
			continue;
		}
		if (scan->flags & TF_CLOSE) {
			pd--;
			continue;
		}
		if (pd) continue;
		if (match_ch(scan, ',') || match_ch(scan, ';')) break;
		if (tok_ann(scan) & P1_IS_ORELSE_KW) {
			info.orelse_tok = scan;
			break;
		}
	}

	if (info.orelse_tok) {
		bool is_fallback = is_orelse_value_fallback(tok_next(info.orelse_tok));
		info.is_const_fallback = has_effective_const_qual(type_start, type, decl) && is_fallback;
	}

	return info;
}

// Detect whether a type specifier refers to a function type
static bool is_typeof_func_type(Token *type_start, TypeSpecResult *type, DeclResult *decl) {
	if (decl->is_pointer || decl->is_array || decl->is_func_ptr) return false;
	for (Token *ft = type_start; ft && ft != type->end; ft = tok_next(ft)) {
		if (is_func_typedef(ft)) return true;
	}
	char *typeof_ident_loc = NULL;
	int typeof_ident_len = 0;
	Token *typeof_inner = NULL;
	if (type->has_typeof) {
		for (Token *ft = type_start; ft && ft != type->end; ft = tok_next(ft)) {
			if (!(ft->tag & TT_TYPEOF)) continue;
			Token *paren = tok_next(ft);
			if (!paren || !match_ch(paren, '(') || !tok_match(paren)) break;
			Token *inner = tok_next(paren);
			Token *close = tok_match(paren);
			while (inner && inner != close && match_ch(inner, '(') && tok_match(inner)) {
				Token *inner_close = tok_match(inner);
				if (inner_close && tok_next(inner_close) == close) {
					inner = tok_next(inner);
					close = inner_close;
				} else
					break;
			}
			if (!inner || inner == close) break;
			if (tok_next(inner) == close && is_valid_varname(inner) &&
			    !(inner->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF))) {
				TypedefEntry *shadow = typedef_lookup(inner);
				if (shadow && shadow->is_shadow) {
					if (shadow->is_func) return true;
					break;
				}
			}
			if (inner->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF)) {
				for (Token *fs = inner; fs && fs != close; fs = tok_next(fs)) {
					if ((fs->tag & TT_TYPEOF) && tok_next(fs) &&
					    match_ch(tok_next(fs), '(') && tok_match(tok_next(fs))) {
						fs = tok_match(tok_next(fs));
						continue;
					}
					if ((fs->tag & (TT_BITINT | TT_ALIGNAS)) && tok_next(fs) &&
					    match_ch(tok_next(fs), '(') && tok_match(tok_next(fs))) {
						fs = tok_match(tok_next(fs));
						continue;
					}
					/* `_Atomic(T)` width/type parens are not a function type. */
					if ((fs->tag & TT_TYPE) && equal(fs, "_Atomic") && tok_next(fs) &&
					    match_ch(tok_next(fs), '(') && tok_match(tok_next(fs))) {
						fs = tok_match(tok_next(fs));
						continue;
					}
					if ((fs->tag & TT_ATTR) && tok_next(fs) &&
					    match_ch(tok_next(fs), '(') && tok_match(tok_next(fs))) {
						fs = tok_match(tok_next(fs));
						continue;
					}
					// Skip C23 [[...]] attributes
					if (is_c23_attr(fs) && tok_match(fs)) {
						fs = tok_match(fs);
						continue;
					}
					if (match_ch(fs, '[') && tok_match(fs)) {
						fs = tok_match(fs);
						continue;
					}
					if (match_ch(fs, '(')) {
						Token *after = skip_noise(tok_next(fs));
						if (after && match_ch(after, '*'))
							return false; // function pointer type
						return true; // function parameter list → function type
					}
				}
				break;
			}
			if (tok_next(inner) != close || !is_valid_varname(inner) ||
			    (inner->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF)))
				break;
			typeof_ident_loc = tok_loc(inner);
			typeof_ident_len = inner->len;
			typeof_inner = inner;
			{
				TypedefEntry *shadow = typedef_lookup(inner);
				if (shadow && shadow->is_shadow) break;
			}
			for (int fi = 0; fi < func_meta_count; fi++) {
				Token *fn = func_meta[fi].ret_type_end;
				if (!fn) {
					Token *bt = tok_walk_back(tok_idx(func_meta[fi].body_open) - 1,
								  WB_SKIP_ATTRS);
					if (bt && is_valid_varname(bt) &&
					    !(bt->tag & (TT_ATTR | TT_TYPE | TT_QUALIFIER | TT_SUE)))
						fn = bt;
				}
				if (fn && fn->len == inner->len &&
				    prism_memeq_runtime_sized(tok_loc(fn), tok_loc(inner), inner->len))
					return true;
			}
			break;
		}
	}
	if (typeof_ident_loc && typeof_inner) {
		TypedefEntry *shadow = typedef_lookup(typeof_inner);
		if (shadow && shadow->is_shadow) return shadow->is_func;
		return hashmap_get(&p1_func_proto_map, typeof_ident_loc, typeof_ident_len) != NULL;
	}
	return false;
}

typedef struct {
	bool effective_vla;
	bool is_aggregate;
	bool is_union_type;
	bool is_func_type;
} DeclShape;

static DeclShape classify_decl_shape(Token *type_start, TypeSpecResult *type, DeclResult *decl) {
	DeclShape s = {
	    .effective_vla = (decl->is_vla && (!decl->paren_pointer || decl->paren_array)) ||
			     (type->is_vla && !decl->is_pointer),
	    .is_aggregate = (decl->is_array && (!decl->paren_pointer || decl->paren_array)) ||
			    ((type->is_struct || type->is_typedef || type->is_array) && !decl->is_pointer),
	    .is_union_type = type->is_union && !decl->is_pointer,
	    .is_func_type = decl->is_func_decl || is_typeof_func_type(type_start, type, decl),
	};
	return s;
}

static bool decl_shape_needs_memset(const DeclShape *s,
				    TypeSpecResult *type,
				    DeclResult *decl,
				    bool has_init,
				    bool is_raw,
				    bool storage_static) {
	return FEAT(F_ZEROINIT) && !has_init && !is_raw && (!decl->is_pointer || decl->is_array) &&
	       !type->has_register && !storage_static && !type->has_extern && !s->is_func_type &&
	       (type->has_typeof || (type->has_atomic && s->is_aggregate) || s->effective_vla ||
		s->is_union_type);
}

static uint8_t decl_shape_to_bits(const DeclShape *s) {
	return (uint8_t)((s->effective_vla ? P1DS_EFF_VLA : 0) | (s->is_aggregate ? P1DS_AGG : 0) |
			 (s->is_union_type ? P1DS_UNION : 0));
}

static bool p1_type_brace_zero_unsafe(Token *type_start, Token *type_end, int depth);

/* Non-init-stmt zero-init recipe. Pass 2 demotes MEMSET→AGG inside init stmts. */
static uint8_t compute_decl_zero_kind(const DeclShape *s,
				      Token *type_start,
				      TypeSpecResult *type,
				      DeclResult *decl,
				      bool has_init,
				      bool is_raw,
				      bool storage_static) {
	if (!FEAT(F_ZEROINIT) || has_init || is_raw || storage_static || type->has_static ||
	    type->has_extern || s->is_func_type)
		return P1Z_NONE;
	if (decl_shape_needs_memset(s, type, decl, has_init, is_raw, storage_static)) return P1Z_MEMSET;
	if (s->effective_vla) return P1Z_NONE;
	/* Empty / zero-size-only / sole-FAM aggregates reject `= {0}` on clang/gcc.
	 * register cannot take the address for memset; const rejects write-via-memset
	 * (see reject_const_unavoidable_memset / reject_register_agg_zeroinit). */
	if (s->is_aggregate && type_start &&
	    p1_type_brace_zero_unsafe(type_start, type->end, 0)) {
		if (type->has_register) return P1Z_NONE;
		return P1Z_MEMSET;
	}
	if (s->is_aggregate || type->has_typeof || s->is_union_type || (type->has_atomic && s->is_aggregate))
		return P1Z_AGG;
	return P1Z_SCALAR;
}

/* Record a local P1K_DECL + zero-init recipe for Pass 2 emit. */
static P1FuncEntry *p1_record_local_decl(uint16_t sid,
					 Token *var,
					 Token *type_start,
					 const DeclShape *shape,
					 TypeSpecResult *type,
					 DeclResult *decl,
					 bool has_init,
					 bool is_raw,
					 bool storage_static,
					 uint32_t body_close_idx) {
	if (!sid || !var || shape->is_func_type) return NULL;
	P1FuncEntry *e = p1_alloc(P1K_DECL, sid, var);
	e->decl.has_init = has_init;
	e->decl.is_vla = type->is_vla || decl->is_vla;
	e->decl.has_raw = is_raw;
	e->decl.is_static_storage = storage_static;
	e->decl.body_close_idx = body_close_idx;
	e->decl.shape = decl_shape_to_bits(shape);
	e->decl.zero_kind =
	    compute_decl_zero_kind(shape, type_start, type, decl, has_init, is_raw, storage_static);
	return e;
}

static DeclShape decl_shape_from_bits(uint8_t bits) {
	return (DeclShape){.effective_vla = (bits & P1DS_EFF_VLA) != 0,
			   .is_aggregate = (bits & P1DS_AGG) != 0,
			   .is_union_type = (bits & P1DS_UNION) != 0,
			   .is_func_type = false};
}

static P1FuncEntry *p2_lookup_decl_entry(Token *var) {
	if (current_func_idx < 0 || !var) return NULL;
	FuncMeta *fm = &func_meta[current_func_idx];
	uint32_t want = tok_idx(var);
	int end = fm->entry_start + fm->entry_count;
	for (int i = fm->entry_start; i < end; i++) {
		if (p1_entries[i].kind == P1K_DECL && p1_entries[i].token_index == want)
			return &p1_entries[i];
	}
	return NULL;
}

#ifdef PRISM_DEBUG
/* Phase 1 deliberately excludes locals inside GNU nested functions from the
 * outer function's recipe/CFG table. Pass 2 still walks their declarations,
 * so a missing-recipe assertion must recognize that intentional omission. */
static bool p1_token_in_nested_function(Token *tok) {
	if (!tok) return false;
	uint32_t idx = tok_idx(tok);
	for (uint16_t sid = 1; sid < scope_tree_count; sid++) {
		ScopeInfo *s = &scope_tree[sid];
		if (s->is_func_body && s->parent_id != 0 && s->open_tok_idx < idx &&
		    idx < s->close_tok_idx)
			return true;
	}
	return false;
}
#endif

static bool decl_shape_explicit_const(Token *type_start, TypeSpecResult *type, DeclResult *decl) {
	bool excl = (type->has_const && !decl->is_func_ptr && !decl->is_pointer) || decl->is_const;
	if (!excl && !decl->is_func_ptr && !decl->is_pointer)
		excl = type_range_any(type_start, type->end, is_const_td);
	return excl;
}

static bool p1_type_has_const_subobject(Token *type_start, Token *type_end, int depth);

static Token *p1_sue_definition_body(Token *sue_kw) {
	Token *body = find_struct_body_brace(sue_kw);
	if (body) return body;

	Token *tag = skip_noise(tok_next(sue_kw));
	while (tag && (tag->tag & TT_QUALIFIER) && !is_soft_keyword_identifier(tag))
		tag = skip_noise(tok_next(tag));
	if (!tag || !is_valid_varname(tag)) return NULL;

	TypedefEntry *te = tag_lookup(tag);
	if (!te || !te->is_struct_tag || te->token_index >= token_count) return NULL;
	Token *definition_tag = &token_pool[te->token_index];
	Token *definition_kw = tok_walk_back(tok_idx(definition_tag), WB_PAST_NOISE);
	if (!definition_kw || !(definition_kw->tag & TT_SUE) || is_enum_kw(definition_kw)) return NULL;
	return find_struct_body_brace(definition_kw);
}

static Token *p1_typedef_type_start(TypedefEntry *te) {
	if (!te || te->token_index >= token_count) return NULL;
	uint32_t i = te->token_index;
	while (i > 0) {
		Token *prev = &token_pool[i - 1];
		if ((prev->flags & TF_CLOSE) && tok_match(prev)) {
			i = tok_idx(tok_match(prev));
			continue;
		}
		if (match_ch(prev, ';')) return NULL;
		if (prev->tag & TT_TYPEDEF) return skip_noise(tok_next(prev));
		i--;
	}
	return NULL;
}

/* True if `[dim]` is empty `[]` (FAM) or literal `[0]`. Both reject `= {0}`
 * when they are the sole member of an aggregate. */
static bool array_dim_is_empty_or_zero(Token *open_bracket) {
	Token *close = tok_match(open_bracket);
	if (!open_bracket || !close) return false;
	Token *t = skip_noise(tok_next(open_bracket));
	if (!t || t == close) return true; /* flexible array member `[]` */
	return t->kind == TK_NUM && t->len == 1 && t->ch0 == '0' && skip_noise(tok_next(t)) == close;
}

/* GNU empty structs / sole `[0]` / sole FAM arrays cannot take `= {0}` (clang:
 * "initializer for aggregate with no elements requires explicit braces").
 * Nested empty members poison parent `{0}` the same way. Route those to memset. */
static bool p1_sue_body_brace_zero_unsafe(Token *brace, int depth);

static bool p1_type_brace_zero_unsafe(Token *type_start, Token *type_end, int depth) {
	if (!type_start || depth > 16) return false;
	uint32_t range_lo = tok_idx(type_start);
	uint32_t range_hi = type_end ? tok_idx(type_end) : UINT32_MAX;
	/* Explicit cursor: after an *in-range* `{…}` land on the token after the
	 * closer. Never retarget the cursor onto a remote tag definition body.
	 * Never advance past type_end — pointer inequality would otherwise scan
	 * the rest of the TU and re-enter every subsequent aggregate. */
	for (Token *t = type_start; t && t != type_end;) {
		if (!(t->tag & TT_SUE) || is_enum_kw(t)) {
			t = tok_next(t);
			continue;
		}
		Token *body = find_struct_body_brace(t);
		if (!body) body = p1_sue_definition_body(t);
		if (body && p1_sue_body_brace_zero_unsafe(body, depth + 1)) return true;
		if (body && tok_match(body)) {
			uint32_t bi = tok_idx(body);
			if (bi >= range_lo && bi < range_hi) {
				Token *after = tok_next(tok_match(body));
				if (!after || tok_idx(after) >= range_hi) break;
				t = after;
				continue;
			}
		}
		t = tok_next(t);
	}
	for (Token *t = type_start; t && t != type_end;) {
		if ((t->flags & TF_OPEN) && match_ch(t, '{') && tok_match(t)) {
			uint32_t bi = tok_idx(t);
			if (bi >= range_lo && bi < range_hi) {
				Token *after = tok_next(tok_match(t));
				if (!after || tok_idx(after) >= range_hi) break;
				t = after;
				continue;
			}
		}
		if (!is_identifier_like(t) || !is_known_typedef(t)) {
			t = tok_next(t);
			continue;
		}
		TypedefEntry *te = typedef_lookup(t);
		if (!te || te->is_shadow || te->is_ptr) {
			t = tok_next(t);
			continue;
		}
		/* Defining occurrence of the typedef name — do not re-enter. */
		if (te->token_index < token_count && t == &token_pool[te->token_index]) {
			t = tok_next(t);
			continue;
		}
		Token *alias = p1_typedef_type_start(te);
		if (alias && te->token_index < token_count) {
			Token *name = &token_pool[te->token_index];
			if (p1_type_brace_zero_unsafe(alias, name, depth + 1)) return true;
		}
		t = tok_next(t);
	}
	return false;
}

static bool p1_sue_body_brace_zero_unsafe(Token *brace, int depth) {
	if (!brace || !tok_match(brace) || depth > 16) return false;
	/* Cached: same brace body is often re-checked via typedef/tag aliases. */
	if (tok_ann(brace) & P1_ZUNSAFE_KNOWN) return (tok_ann(brace) & P1_ZUNSAFE) != 0;

	Token *end = tok_match(brace);
	/* Fast prefilter: FAM / `[0]` / nested empty aggregates need `[` or a
	 * nested `struct`/`union`/`enum` (or a nested `{`). Ordinary
	 * `struct { int a; volatile int b; }` — the large.c stress shape —
	 * has neither, so skip the full re-parse. */
	{
		bool saw_any = false;
		bool needs_full = false;
		for (Token *t = tok_next(brace); t && t != end;) {
			if (t->kind == TK_PREP_DIR) {
				t = tok_next(t);
				continue;
			}
			if ((t->flags & TF_OPEN) && match_ch(t, '{') && tok_match(t)) {
				needs_full = true;
				saw_any = true;
				t = tok_next(tok_match(t));
				continue;
			}
			if (match_ch(t, '[') && !(t->flags & TF_C23_ATTR)) {
				needs_full = true;
				saw_any = true;
				t = tok_next(t);
				continue;
			}
			if (t->tag & TT_SUE) {
				needs_full = true;
				saw_any = true;
				t = tok_next(t);
				continue;
			}
			saw_any = true;
			t = tok_next(t);
		}
		if (!saw_any) {
			brace->ann |= (uint16_t)(P1_ZUNSAFE_KNOWN | P1_ZUNSAFE);
			return true; /* struct Empty {} */
		}
		if (!needs_full) {
			brace->ann |= (uint16_t)P1_ZUNSAFE_KNOWN;
			return false;
		}
	}

	bool saw_sized = false;
	bool saw_empty_or_zero = false;
	bool saw_any = false;
	for (Token *stmt = skip_noise(tok_next(brace)); stmt && stmt != end;) {
		if (stmt->kind == TK_PREP_DIR) {
			stmt = skip_noise(tok_next(stmt));
			continue;
		}
		if ((stmt->flags & TF_STATIC_ASSERT) || (stmt->tag & TT_SKIP_DECL)) {
			/* _Static_assert / static_assert inside the body */
			Token *n = skip_noise(tok_next(stmt));
			if (n && match_ch(n, '(') && tok_match(n))
				stmt = skip_noise(tok_next(tok_match(n)));
			else
				stmt = skip_noise(tok_next(stmt));
			continue;
		}
		TypeSpecResult member_type = parse_type_specifier(stmt);
		if (!member_type.saw_type || !member_type.end) {
			Token *next = stmt;
			while (next && next != end && !match_ch(next, ';')) {
				if ((next->flags & TF_OPEN) && tok_match(next))
					next = tok_next(tok_match(next));
				else
					next = tok_next(next);
			}
			stmt = next && match_ch(next, ';') ? skip_noise(tok_next(next)) : end;
			continue;
		}
		Token *decl_start = skip_noise(member_type.end);
		bool member_empty_type =
		    p1_type_brace_zero_unsafe(stmt, member_type.end, depth + 1);
		bool saw_decl = false;
		while (decl_start && decl_start != end && !match_ch(decl_start, ';')) {
			if (match_ch(decl_start, ':')) {
				/* anonymous bitfield — counts as a member */
				saw_any = true;
				saw_sized = true;
				break;
			}
			DeclResult member = parse_declarator(decl_start, false);
			if (!member.end) break;
			saw_decl = true;
			saw_any = true;
			bool zero_arr = false;
			if (member.is_array && !member.is_pointer) {
				for (Token *b = decl_start; b && b != member.end; b = tok_next(b)) {
					if (match_ch(b, '[') && !(b->flags & TF_C23_ATTR) &&
					    array_dim_is_empty_or_zero(b)) {
						zero_arr = true;
						break;
					}
					if ((b->flags & TF_OPEN) && tok_match(b) && !match_ch(b, '['))
						b = tok_match(b);
				}
			}
			if (member.is_pointer || member.is_func_ptr)
				saw_sized = true;
			else if (zero_arr || member_empty_type)
				saw_empty_or_zero = true;
			else
				saw_sized = true;

			Token *next = member.end;
			while (next && next != end && !match_ch(next, ',') && !match_ch(next, ';')) {
				if ((next->flags & TF_OPEN) && tok_match(next))
					next = tok_next(tok_match(next));
				else
					next = tok_next(next);
			}
			if (next && match_ch(next, ',')) {
				decl_start = skip_noise(tok_next(next));
				continue;
			}
			decl_start = next;
			break;
		}
		if (!saw_decl && member_empty_type) {
			saw_any = true;
			saw_empty_or_zero = true;
		}
		Token *next_stmt = decl_start;
		while (next_stmt && next_stmt != end && !match_ch(next_stmt, ';')) {
			if ((next_stmt->flags & TF_OPEN) && tok_match(next_stmt))
				next_stmt = tok_next(tok_match(next_stmt));
			else
				next_stmt = tok_next(next_stmt);
		}
		stmt = next_stmt && match_ch(next_stmt, ';') ? skip_noise(tok_next(next_stmt)) : end;
	}
	bool unsafe = !saw_any || !saw_sized || saw_empty_or_zero;
	brace->ann |= (uint16_t)P1_ZUNSAFE_KNOWN;
	if (unsafe) brace->ann |= (uint16_t)P1_ZUNSAFE;
	return unsafe;
}

static bool p1_aggregate_body_has_const_subobject(Token *brace, int depth) {
	if (!brace || !tok_match(brace) || depth > 32) return false;
	Token *end = tok_match(brace);
	for (Token *stmt = skip_noise(tok_next(brace)); stmt && stmt != end;) {
		if (stmt->kind == TK_PREP_DIR) {
			stmt = skip_noise(tok_next(stmt));
			continue;
		}

		TypeSpecResult member_type = parse_type_specifier(stmt);
		bool saw_declarator = false;
		if (member_type.saw_type && member_type.end) {
			Token *decl_start = skip_noise(member_type.end);
			while (decl_start && decl_start != end && !match_ch(decl_start, ';')) {
				DeclResult member = parse_declarator(decl_start, false);
				if (!member.end || !member.var_name) break;
				saw_declarator = true;
				if (decl_shape_explicit_const(stmt, &member_type, &member)) return true;
				if (!member.is_pointer && !member.is_func_ptr &&
				    p1_type_has_const_subobject(
					stmt, member_type.end, depth + 1))
					return true;

				Token *next = member.end;
				while (next && next != end && !match_ch(next, ',') &&
				       !match_ch(next, ';')) {
					if ((next->flags & TF_OPEN) && tok_match(next))
						next = tok_next(tok_match(next));
					else
						next = tok_next(next);
				}
				if (next && match_ch(next, ',')) {
					decl_start = skip_noise(tok_next(next));
					continue;
				}
				break;
			}
			if (!saw_declarator &&
			    p1_type_has_const_subobject(stmt, member_type.end, depth + 1))
				return true;
		}

		Token *next_stmt = stmt;
		while (next_stmt && next_stmt != end && !match_ch(next_stmt, ';')) {
			if ((next_stmt->flags & TF_OPEN) && tok_match(next_stmt))
				next_stmt = tok_next(tok_match(next_stmt));
			else
				next_stmt = tok_next(next_stmt);
		}
		stmt = next_stmt && next_stmt != end ? skip_noise(tok_next(next_stmt)) : end;
	}
	return false;
}

static bool p1_type_has_const_subobject(Token *type_start, Token *type_end, int depth) {
	if (!type_start || depth > 32) return false;
	for (Token *t = type_start; t && t != type_end; t = tok_next(t)) {
		if ((t->tag & TT_SUE) && !is_enum_kw(t)) {
			Token *body = p1_sue_definition_body(t);
			if (body && p1_aggregate_body_has_const_subobject(body, depth + 1)) return true;
			continue;
		}
		if (!is_known_typedef(t)) continue;
		TypedefEntry *te = typedef_lookup(t);
		if (!te || !te->is_aggregate || te->is_ptr) continue;
		Token *alias_type = p1_typedef_type_start(te);
		if (!alias_type) continue;
		/* Avoid re-entering the same typedef binding (self-alias cycles). */
		if (te->token_index < token_count && t == &token_pool[te->token_index]) continue;
		TypeSpecResult alias = parse_type_specifier(alias_type);
		if (alias.saw_type && alias.end &&
		    p1_type_has_const_subobject(alias_type, alias.end, depth + 1))
			return true;
	}
	return false;
}

static void reject_register_agg_zeroinit(
    Token *var, const DeclShape *s, TypeSpecResult *type, Token *type_start, bool has_init, bool is_raw) {
	if (!FEAT(F_ZEROINIT) || has_init || is_raw || !type->has_register) return;
	if (type->has_extern || type->has_static) return;
	if (type->has_atomic && s->is_aggregate) error_tok(var, ERR_REGISTER_ATOMIC_AGGREGATE);
	if (s->is_union_type) error_tok(var, ERR_REGISTER_UNION);
	/* Empty / sole-FAM aggregates need memset (cannot use `= {0}`); register
	 * forbids taking the address — reject rather than emit illegal memset. */
	if (s->is_aggregate && type_start && p1_type_brace_zero_unsafe(type_start, type->end, 0))
		error_tok(var, ERR_REGISTER_EMPTY_AGG);
}

static void reject_const_unavoidable_memset(Token *var,
					    const DeclShape *s,
					    TypeSpecResult *type,
					    Token *type_start,
					    DeclResult *decl,
					    bool has_init,
					    bool is_raw,
					    bool allow_init_stmt_scalar) {
	if (!FEAT(F_ZEROINIT) || has_init || is_raw) return;
	if (type->has_register || type->has_static || type->has_extern) return;
	bool brace_unsafe =
	    s->is_aggregate && type_start && p1_type_brace_zero_unsafe(type_start, type->end, 0);
	bool needs = (!decl->is_pointer || decl->is_array) &&
		     (type->has_typeof || (type->has_atomic && s->is_aggregate) || s->effective_vla ||
		      s->is_union_type || brace_unsafe);
	if (!needs) return;
	bool explicit_const = decl_shape_explicit_const(type_start, type, decl);
	bool has_const_subobject =
	    !explicit_const && s->is_aggregate &&
	    p1_type_has_const_subobject(type_start, type->end, 0);
	if (!explicit_const && !has_const_subobject) return;
	if (allow_init_stmt_scalar && !s->effective_vla) return;
	if (has_const_subobject)
		error_tok(var,
			  "aggregate containing a const-qualified subobject requires unavoidable "
			  "memset zero-initialization, which would modify a const object and cause "
			  "undefined behavior. Provide an explicit initializer or use 'raw' to opt out.");
	bool unavoidable = s->is_union_type || s->effective_vla || brace_unsafe ||
			   (type->has_atomic && (s->is_aggregate || type->has_typeof));
	if (!unavoidable) return;
	error_tok(var, ERR_CONST_UNAVOIDABLE_MEMSET);
}

static bool type_spec_is_anon_sue(Token *type_start, TypeSpecResult *type) {
	if (!type->is_struct || type->is_enum) return false;
	for (Token *t = type_start; t && t != type->end; t = tok_next(t)) {
		if (t->tag & TT_SUE) {
			Token *after = skip_noise(tok_next(t));
			return after && match_ch(after, '{');
		}
	}
	return false;
}

static void
reject_decl_orelse_storage(Token *static_tok, Token *constexpr_tok, TypeSpecResult *type, bool saw_static) {
	if (saw_static || type->has_static || type->has_extern)
		error_tok(static_tok, ERR_ORELSE_STATIC_THREAD);
	if (type->has_constexpr) error_tok(constexpr_tok ? constexpr_tok : static_tok, ERR_ORELSE_CONSTEXPR);
}

static void reject_decl_orelse_value_shape(Token *var_name,
					   Token *type_start,
					   TypeSpecResult *type,
					   DeclResult *decl) {
	bool base_is_array = decl->is_array && (!decl->paren_pointer || decl->paren_array);
	if (!base_is_array && !decl->is_pointer && !decl->paren_pointer) {
		for (Token *t = type_start; t && t != type->end; t = tok_next(t))
			if (is_array_typedef(t)) {
				base_is_array = true;
				break;
			}
	}
	if (base_is_array)
		error_tok(var_name, ERR_ORELSE_ARRAY_NEVER_NULL, var_name->len, tok_loc(var_name));
	if (type->is_struct && !type->is_enum && !decl->is_pointer && !decl->is_array)
		error_tok(var_name, ERR_ORELSE_STRUCT_VALUE);
}

/* Leading __attribute__/[[attr]]/asm before the type make `static ATTR …`
 * illegal or change cleanup semantics — skip auto-static in those cases. */
static bool range_has_attribute(Token *start, Token *end, uint32_t extra_tag) {
	for (Token *t = start; t && t != end; t = tok_next(t)) {
		if ((t->tag & (TT_ATTR | extra_tag)) || is_c23_attr(t)) return true;
		if (t->flags & TF_OPEN) {
			Token *m = tok_match(t);
			if (m) {
				t = m;
				continue;
			}
		}
	}
	return false;
}

static bool is_const_literal_init(Token *eq) {
	Token *t = tok_next(eq);
	if (!t) return false;
	/* Idiomatic `char msg[] = "…"` (and adjacent string concat). */
	if (t->kind == TK_STR) {
		Token *n = tok_next(t);
		while (n && n->kind == TK_STR) n = tok_next(n);
		return n && match_ch(n, ';');
	}
	if (!match_ch(t, '{')) return false;
	Token *close = tok_match(t);
	if (!close) return false;
	bool prev_was_dot = false;
	for (t = tok_next(t); t && t != close; t = tok_next(t)) {
		if (t->kind == TK_NUM || t->kind == TK_STR) {
			prev_was_dot = false;
			continue;
		}
		if (t->kind == TK_PUNCT) {
			char c = t->ch0;
			//   = blocks == (comparison; safe constant expr, but conservative reject)
			if (c == ',' || c == '{' || c == '}' || c == '[' || c == ']' ||
			    (c == '=' && t->len == 1) || (c == '.' && t->len == 1) ||
			    ((c == '+' || c == '-') && t->len == 1)) {
				prev_was_dot = (c == '.');
				continue;
			}
			return false;
		}
		if (t->kind == TK_IDENT &&
		    (prev_was_dot || is_known_enum_const(t) || equal(t, "true") || equal(t, "false"))) {
			prev_was_dot = false;
			continue;
		}
		return false;
	}
	return true;
}

typedef struct {
	Token *tok;
	Token *unreachable_tok;
	bool hit_orelse;
} InitWalkResult;

static InitWalkResult emit_decl_init_walk(Token *tok) {
	InitWalkResult r = {tok, NULL, false};
	int init_ternary = 0;
	while (r.tok->kind != TK_EOF) {
		if (r.tok->flags & TF_OPEN) {
			/* Phase 1 rejects orelse/defer in non-ctrl parens; Pass 2 assert. */
#ifdef PRISM_DEBUG
			if ((FEAT(F_ORELSE) || FEAT(F_DEFER)) &&
			    !(match_ch(r.tok, '[') && (tok_ann(r.tok) & P1_OE_BRACKET)))
				check_orelse_in_parens(r.tok);
#endif
			r.tok = walk_balanced(r.tok, true);
			continue;
		}
		if (match_ch(r.tok, ',') || match_ch(r.tok, ';')) break;
		if (match_ch(r.tok, '?')) {
			init_ternary++;
			emit_tok(r.tok);
			r.tok = tok_next(r.tok);
			continue;
		}
		if (match_ch(r.tok, ':') && init_ternary > 0) {
			init_ternary--;
			emit_tok(r.tok);
			r.tok = tok_next(r.tok);
			continue;
		}
		if (FEAT(F_ORELSE) && is_orelse_keyword(r.tok)) {
			/* raw { … } suppress region — keep orelse as a soft keyword. */
			if (ctx->raw_block_depth > 0) {
				r.tok = emit_advance(r.tok);
				continue;
			}
#ifdef PRISM_DEBUG
			if (init_ternary > 0) error_tok(r.tok, ERR_ORELSE_TERNARY);
#endif
			r.hit_orelse = true;
			break;
		}
		if (FEAT(F_AUTO_UNREACHABLE) && !in_ctrl_paren()) {
			Token *nr = try_detect_noreturn_call(r.tok);
			if (nr) r.unreachable_tok = nr;
		}
		if (__builtin_expect(FEAT(F_BOUNDS_CHECK), 0)) {
			Token *bc = try_bounds_checks(r.tok);
			if (bc) {
				r.tok = bc;
				continue;
			}
		}
		r.tok = emit_advance(r.tok);
	}
	return r;
}

static bool finish_decl_orelse_hit(Token **tok_p, Token *tok, OrelseDeclTargetInfo *target, bool brace_wrap) {
	if (target->stop_comma && match_ch(tok, ',')) {
		*tok_p = tok_next(tok);
		return true;
	}
	if (brace_wrap) OUT_LIT(" }");
	*tok_p = tok;
	return false;
}

static bool process_init_orelse_hit(Token **tok_p,
				    DeclResult *decl,
				    Token *type_start,
				    TypeSpecResult *type,
				    bool brace_wrap,
				    int typeof_var_base) {
	Token *tok = *tok_p;
#ifdef PRISM_DEBUG
	reject_orelse_in_for_init(tok);
#endif
	out_char(';');
	flush_typeof_memsets(ctx->typeof_vars, &ctx->typeof_var_count, type, typeof_var_base);
	tok = tok_next(tok); // skip 'orelse'
	OrelseDeclTargetInfo target = analyze_decl_orelse_target(tok, type_start, type, decl);
#ifdef PRISM_DEBUG
	if (target.is_struct_value) error_tok(decl->var_name, ERR_ORELSE_STRUCT_VALUE);
#endif
	tok = emit_orelse_action(tok,
				 decl->var_name,
				 target.has_const_qual,
				 type->has_volatile || type->has_atomic,
				 target.stop_comma);
	return finish_decl_orelse_hit(tok_p, tok, &target, brace_wrap);
}

static bool should_split_multi_decl(Token *next_decl_tok) {
	if (in_for_init()) return false;
	DeclResult next_decl = parse_declarator(next_decl_tok, false);
	if (!next_decl.end) return false;
	if (ctx->typeof_var_count > 0 && next_decl.var_name && (next_decl.has_init || next_decl.is_vla))
		return true;
	if (FEAT(F_ORELSE) && declarator_has_bracket_orelse(next_decl_tok, next_decl.end)) return true;
	return false;
}

#ifdef PRISM_DEBUG
static void validate_no_anon_struct_split(Token *next_decl_tok, Token *type_start, TypeSpecResult *type) {
	if (type_spec_is_anon_sue(type_start, type)) error_tok(next_decl_tok, ERR_BRACKET_OE_ANON_AGG);
}
#endif

static bool process_const_orelse_decl(Token **tok_p,
				      Token *orelse_tok,
				      Token *decl_start,
				      DeclResult *decl,
				      Token *type_start,
				      TypeSpecResult *type,
				      Token *pragma_start,
				      bool brace_wrap,
				      int typeof_var_base) {
	Token *val_start = tok_next(decl->end); // First value token after '='
	Token *tok = tok_next(orelse_tok);	// skip 'orelse'
	OrelseDeclTargetInfo target = analyze_decl_orelse_target(tok, type_start, type, decl);
	/* Phase 1 reject_decl_orelse_value_shape owns struct-value rejects. */
#ifdef PRISM_DEBUG
	if (target.is_struct_value)
		error_tok(orelse_tok,
			  "orelse value fallback on const/typeof aggregate "
			  "is not supported; use a control flow action "
			  "(return/break/goto), typeof_unqual, or an "
			  "explicit type name");
#endif
	/* Phase 1 p1d_validate_decl_orelse owns const+VM rejects (incl. typeof/_Atomic). */
#ifdef PRISM_DEBUG
	if (type->is_vla || decl->is_vla || type->type_vm) error_tok(orelse_tok, ERR_ORELSE_CONST_VM);
#endif
	tok = handle_const_orelse_fallback(
	    tok, orelse_tok, val_start, decl_start, decl, type_start, type, pragma_start, target.stop_comma);
	flush_typeof_memsets(ctx->typeof_vars, &ctx->typeof_var_count, type, typeof_var_base);
	if (match_ch(tok, ';')) tok = tok_next(tok);
	end_statement_after_semicolon();
	return finish_decl_orelse_hit(tok_p, tok, &target, brace_wrap);
}

static Token *process_declarators(Token *tok,
				  TypeSpecResult *type,
				  bool is_raw,
				  Token *type_start,
				  Token *pragma_start,
				  Token *raw_tok,
				  bool brace_wrap) {
	int typeof_var_base = ctx->typeof_var_count; // Save for reentrancy (stmt-expr in array dims)
	bool first_decl = true;
	bool need_type_emit = false; // Set after orelse comma — deferred to after next lookahead
	while (tok && tok->kind != TK_EOF) {
		Token *decl_start = tok;
		bool decl_is_raw = is_raw;
		Token *raw_probe = skip_noise(tok);
		Token *after = raw_decl_strip_after(raw_probe);
		if (after) {
			decl_noise(tok, true);
			Token *last_raw = raw_probe;
			SKIP_RAW(after, last_raw);
			emit_noise_between_raws(raw_probe, last_raw);
			decl_start = tok_next(last_raw);
			tok = after;
			decl_is_raw = true;
		}

		DeclResult decl = parse_declarator(tok, false);
		if (!decl.end || !decl.var_name) {
			if (!first_decl) {
				if (need_type_emit) {
					/* Phase 1 p1d_check_multi_decl_constraints owns VM splits. */
#ifdef PRISM_DEBUG
					if ((type->has_typeof || type->has_atomic) &&
					    (type->is_vla || type->type_vm))
						error_tok(tok, ERR_MULTIDECL_VM);
#endif
					emit_type_range(type_start, type->end, false, true);
				}
				goto emit_raw_bail;
			}
			return NULL;
		}

		OrelseInitInfo orelse_info = scan_decl_orelse(decl.end, type_start, type, &decl);
		bool is_const_orelse_fallback = orelse_info.is_const_fallback;
		// Static/extern/thread-local initializers must be constant expressions
		// (C11 §6.7.9p4). Orelse splits the declaration into a runtime
#ifdef PRISM_DEBUG
		if (orelse_info.orelse_tok)
			reject_decl_orelse_storage(
			    orelse_info.orelse_tok, orelse_info.orelse_tok, type, false);
#endif
		// Step 2b: Pre-hoist bracket orelse temps (before type emission)
		bool has_bo = FEAT(F_ORELSE) && declarator_has_bracket_orelse(decl_start, decl.end);
		bool brace_opened = false;
		BOFrame bo_frame;
		if (has_bo) {
			/* Phase 1: p1_scan_init_shadows (for/if/switch init) and the
			 * ctrl-paren orelse scan (while) own this reject. */
#ifdef PRISM_DEBUG
			if (in_ctrl_paren()) error_tok(decl_start, ERR_BRACKET_OE_VLA_INIT_STMT);
#endif
			if (first_decl && brace_wrap) {
				OUT_LIT(" {");
				brace_opened = true;
			}
			bo_frame.oe_count = ctx->bracket_oe_count;
			bo_frame.oe_next = ctx->bracket_oe_next;
			bo_frame.dim_count = ctx->bracket_dim_count;
			bo_frame.dim_next = ctx->bracket_dim_next;
			bo_snapshot_ids(&bo_frame.oe_ids, ctx->bracket_oe_ids, bo_frame.oe_count);
			bo_snapshot_ids(&bo_frame.dim_ids, ctx->bracket_dim_ids, bo_frame.dim_count);
			emit_bracket_orelse_temps(decl_start, decl.end);
		}

		if (need_type_emit) {
			// Splitting a typeof VLA re-emits the type specifier, causing
			// the VLA dimension to be evaluated twice (ISO C11 §6.7.2.5).
			// Phase 1 p1d_check_multi_decl_constraints owns this reject.
#ifdef PRISM_DEBUG
			if ((type->has_typeof || type->has_atomic) && (type->is_vla || type->type_vm))
				error_tok(decl_start, ERR_MULTIDECL_VM);
#endif
			if (!is_const_orelse_fallback)
				emit_type_with_pragma_prelude(
				    pragma_start, type_start, type->end, raw_tok, true);
			need_type_emit = false;
		}

		if (first_decl) {
			if (brace_wrap && !brace_opened) OUT_LIT(" {");
			if (!is_const_orelse_fallback) {
				if (FEAT(F_AUTO_STATIC) && ctx->raw_block_depth == 0 && ctx->block_depth > 0 &&
				    !in_ctrl_paren() &&
				    has_effective_const_qual(type_start, type, &decl) &&
				    !type->has_volatile && !type->has_volatile_member &&
				    !type->has_static && !type->has_extern && !type->has_register &&
				    !type->has_auto && !type->has_constexpr && !type->has_thread_local &&
				    (decl.is_array || type->is_array) &&
				    (!decl.is_pointer || decl.is_const) && decl.has_init &&
				    !decl.is_vla && !type->is_vla && !decl_is_raw &&
				    !orelse_info.orelse_tok) {
					if (!type_range_any(type_start, type->end, is_vol_or_vol_member_td) &&
					    !range_has_attribute(
						pragma_start ? pragma_start : type_start, type->end, TT_ASM) &&
					    !range_has_attribute(tok_next(decl.var_name), decl.end, 0)) {
						Token *eq = decl.end; // '=' token
						Token *init = tok_next(eq);
						bool lit_ok = false;
						if (init && match_ch(init, '{') && tok_match(init) &&
						    match_ch(tok_next(tok_match(init)), ';') &&
						    is_const_literal_init(eq))
							lit_ok = true;
						else if (init && init->kind == TK_STR &&
							 is_const_literal_init(eq))
							lit_ok = true;
						if (lit_ok) OUT_LIT("static ");
					}
				}
				emit_type_with_pragma_prelude(
				    pragma_start, type_start, type->end, raw_tok, false);
			}
			first_decl = false;
		}

		P1FuncEntry *p1e = p2_lookup_decl_entry(decl.var_name);
		DeclShape shape;
		uint8_t zero_kind;
		if (p1e) {
			shape = decl_shape_from_bits(p1e->decl.shape);
			zero_kind = p1e->decl.zero_kind;
		} else {
			/* No P1K_DECL: file scope, func type, or aggregate member. */
			shape = classify_decl_shape(type_start, type, &decl);
			zero_kind = compute_decl_zero_kind(
			    &shape, type_start, type, &decl, decl.has_init, decl_is_raw, type->has_static);
#ifdef PRISM_DEBUG
			if (current_func_idx >= 0 && decl.var_name && !shape.is_func_type &&
			    !p1_token_in_nested_function(decl.var_name))
				error_tok(decl.var_name, "internal: missing P1K_DECL recipe");
#endif
		}
		bool effective_vla = shape.effective_vla;
		bool needs_memset = (zero_kind == P1Z_MEMSET);
		if (needs_memset && !type->has_volatile && !decl.is_func_ptr && !decl.is_pointer) {
			for (Token *tv = type_start; tv && tv != type->end; tv = tok_next(tv))
				if (is_volatile_typedef(tv) || has_volatile_member_typedef(tv)) {
					type->has_volatile = true;
					break;
				}
		}
		if (needs_memset && !type->has_volatile && type->has_volatile_member)
			type->has_volatile = true;
		if (is_const_orelse_fallback && ctx->raw_block_depth == 0) {
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

		parse_declarator(decl_start, true);
		if (has_bo) bo_restore(&bo_frame);
		tok = decl.end;
		bool init_stmt_ctx = in_for_init() || in_ctrl_paren();
		uint8_t emit_zk = zero_kind;
		if (emit_zk == P1Z_MEMSET && init_stmt_ctx && !effective_vla) {
			/* for/if init cannot emit memset. Demoting to ={0} is illegal for
			 * _Atomic aggregates (and some empty/FAM shapes on strict compilers). */
			if ((type->has_atomic && shape.is_aggregate) ||
			    (type_start && p1_type_brace_zero_unsafe(type_start, type->end, 0)))
				error_tok(decl.var_name,
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
			bool real_for_init = in_for_init() && ctx->scope_depth > 0 &&
					     scope_stack[ctx->scope_depth - 1].is_loop;
			if (!(init_stmt_ctx && effective_vla && !real_for_init)) {
				ARENA_ENSURE_CAP(&ctx->main_arena,
						 ctx->typeof_vars,
						 ctx->typeof_var_count + 1,
						 ctx->typeof_var_cap,
						 128,
						 Token *);
				ctx->typeof_vars[ctx->typeof_var_count++] = decl.var_name;
			}
		}

		Token *pd_unreachable_tok = NULL;
		if (decl.has_init) {
			InitWalkResult iw = emit_decl_init_walk(tok);
			tok = iw.tok;
			pd_unreachable_tok = iw.unreachable_tok;
			if (iw.hit_orelse) {
				if (process_init_orelse_hit(
					&tok, &decl, type_start, type, brace_wrap, typeof_var_base)) {
					need_type_emit = true;
					continue;
				}
				return tok;
			}
		}

		if (!brace_wrap) check_defer_var_shadow(decl.var_name);
		if (match_ch(tok, ';')) {
			bool is_ur = (tok == pd_unreachable_tok);
			emit_tok(tok);
			flush_typeof_memsets(ctx->typeof_vars, &ctx->typeof_var_count, type, typeof_var_base);
			if (is_ur) EMIT_UNREACHABLE();
			if (brace_wrap) OUT_LIT(" }");
			return tok_next(tok);
		} else if (match_ch(tok, ',')) {
			Token *next_decl_tok = tok_next(tok);
			if (should_split_multi_decl(next_decl_tok)) {
				/* Phase 1 p1d_check_multi_decl_constraints owns anon-agg splits. */
#ifdef PRISM_DEBUG
				validate_no_anon_struct_split(next_decl_tok, type_start, type);
#endif
				out_char(';');
				flush_typeof_memsets(
				    ctx->typeof_vars, &ctx->typeof_var_count, type, typeof_var_base);
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
	while (tok && tok->kind != TK_EOF && !match_ch(tok, ';')) {
		if ((tok->flags & TF_OPEN) && is_stmt_expr_open(tok)) {
			tok = emit_stmt_expr(tok);
			continue;
		}
		tok = emit_advance(tok);
	}
	if (tok && match_ch(tok, ';')) {
		tok = emit_advance(tok);
	}
	flush_typeof_memsets(ctx->typeof_vars, &ctx->typeof_var_count, type, typeof_var_base);
	if (brace_wrap) OUT_LIT(" }");
	return tok;
}

static Token *try_zero_init_decl(Token *tok) {
	if (ctx->block_depth <= 0 || in_struct_body()) return NULL;
	if (!FEAT(F_ZEROINIT) && !FEAT(F_ORELSE) && !FEAT(F_AUTO_STATIC)) return NULL;
	if (!FEAT(F_ZEROINIT) && !FEAT(F_AUTO_STATIC)) {
		bool has_bo = false;
		for (Token *s = tok; s && s->kind != TK_EOF; s = tok_next(s)) {
			if (match_ch(s, ';') || match_ch(s, '{')) break;
			if (tok_ann(s) & (P1_OE_BRACKET | P1_OE_DECL_INIT)) {
				has_bo = true;
				break;
			}
		}
		if (!has_bo) return NULL;
	}

	if (tok->kind >= TK_STR) // Fast reject: strings, numbers, prep directives,
				 // EOF can't start a declaration
		return NULL;
	ScopeNode *_bt = scope_block_top();
	bool in_switch_scope_unbraced = _bt && _bt->is_switch;
	Token *warn_loc = tok;
	Token *pragma_start = tok;
	tok = skip_noise(tok);
	Token *start = tok;
	bool is_raw = false;
	Token *raw_tok = NULL;
	Token *raw_last = NULL; // last raw in chain (for deferred emit_noise_between_raws)
	if ((tok->flags & TF_RAW) && !is_known_typedef(tok)) {
		Token *after_raw = skip_noise(tok_next(tok));
		if (is_raw_declaration_context(tok, after_raw)) {
			is_raw = true;
			raw_tok = tok;
			Token *last_raw = tok;
			SKIP_RAW(after_raw, last_raw);
			raw_last = last_raw;
			start = tok_next(last_raw);
			tok = after_raw;
			if (pragma_start == raw_tok) pragma_start = start;
			warn_loc = after_raw;
		}
	}

	if (!is_raw) {
		Token *probe = start;
		while (probe && probe->kind != TK_EOF) {
			SKIP_NOISE_CONTINUE(probe);
			if (probe->tag & TT_QUALIFIER) {
				probe = tok_next(probe);
				continue;
			}
			if ((probe->tag & (TT_STORAGE | TT_TYPEDEF))) {
				probe = tok_next(probe);
				continue;
			}
			break;
		}
		if (probe && (probe->flags & TF_RAW) && !is_known_typedef(probe)) {
			Token *after_raw = skip_noise(tok_next(probe));
			if (is_raw_declaration_context(probe, after_raw)) {
				Token *last_raw = probe;
				SKIP_RAW(after_raw, last_raw);
				if (has_storage_in(pragma_start, last_raw)) {
					emit_range(pragma_start, probe);
					emit_noise_between_raws(probe, last_raw);
					return emit_raw_verbatim_to_semicolon(tok_next(last_raw));
				}
				is_raw = true;
				raw_tok = probe;
			}
		}
	}

	ASSERT_NOT_NOISE(tok);
	if ((tok->tag & TT_SKIP_DECL) && !(tok->tag & TT_STORAGE)) // Control flow, etc. (not storage class)
	{
		if (is_raw) {
			return emit_raw_verbatim_to_semicolon(start);
		}
		return NULL;
	}

	if (!(tok->tag & TT_DECL_START) && !is_known_typedef(tok)) return NULL;
	// Phase 1 fast gate: skip past storage/inline prefix to reach the
	// type-start token that Phase 1D annotated with P1_IS_DECL.
	{
		Token *ann = tok;
		while (ann && ann->kind != TK_EOF) {
			SKIP_NOISE_CONTINUE(ann);
			if (ann->tag & (TT_STORAGE | TT_INLINE | TT_SKIP_DECL)) {
				ann = tok_next(ann);
				continue;
			}
			break;
		}
		if (ann && !(tok_ann(ann) & P1_IS_DECL)) return NULL;
	}

	TypeSpecResult type = parse_type_specifier(tok);
	if (!type.saw_type) return NULL;
	// parse_type_specifier now detects embedded raw (e.g. const raw int)
	if (type.has_raw && !is_raw) {
		is_raw = true;
		for (Token *r = start; r && r != type.end; r = tok_next(r))
			if ((r->flags & TF_RAW) && !is_known_typedef(r)) {
				raw_tok = r;
				break;
			}
	}

	{
		DeclResult probe = parse_declarator(type.end, false);
		if (!probe.var_name || !probe.end) return NULL;
		if (match_ch(probe.end, '=')) {
			Token *aeq = tok_next(probe.end);
			if (aeq && is_stmt_expr_open(aeq) && tok_match(aeq)) {
				Token *se_close = tok_match(aeq);
				Token *after_se = tok_next(se_close);
				bool is_orelse =
				    after_se && (after_se->tag & TT_ORELSE) && !(se_close->tag & TT_MEMBER);
				if (is_orelse) {
					TypedefEntry *te = typedef_lookup(after_se);
					if (te && !orelse_shadow_is_kw(se_close)) is_orelse = false;
				}
				if (!after_se || (!match_ch(after_se, ',') && !is_orelse)) {
					if (!(ctrl_state.pending && ctrl_state.parens_just_closed))
						check_defer_var_shadow(probe.var_name);
					return NULL;
				}
			}
		}
	}

	if (FEAT(F_ZEROINIT) && in_switch_scope_unbraced && !is_raw && !in_for_init()) {
		// Mirror Phase 1D's narrower gate: only fire on the
		DeclResult _peek = parse_declarator(type.end, false);
		bool _has_init = _peek.var_name && _peek.end && match_ch(_peek.end, '=');
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

static Token *emit_expr_to_semicolon(Token *tok) {
	int brace_depth = 0;
	int ternary_depth = 0;
	bool expr_at_stmt_start = false;
	while (tok->kind != TK_EOF) {
		if ((match_ch(tok, '(') || match_ch(tok, '[')) && tok_match(tok)) {
			/* Annotated bracket orelse transforms via walk_balanced.
			 * Phase 1 owns paren orelse rejects (incl. stmt-start bodies). */
#ifdef PRISM_DEBUG
			if ((FEAT(F_ORELSE) || FEAT(F_DEFER)) &&
			    !(match_ch(tok, '[') && (tok_ann(tok) & P1_OE_BRACKET)))
				check_orelse_in_parens(tok);
#endif
			tok = walk_balanced(tok, true);
			expr_at_stmt_start = false;
			continue;
		}
		if (match_ch(tok, '{')) {
			brace_depth++;
			expr_at_stmt_start = true;
		} else if (match_ch(tok, '}'))
			brace_depth--;
		else if (brace_depth == 0 && match_ch(tok, ';'))
			break;
		else if (match_ch(tok, '?'))
			ternary_depth++;
		if (expr_at_stmt_start && FEAT(F_ZEROINIT)) {
			Token *next = try_zero_init_decl(tok);
			if (next) {
				tok = next;
				expr_at_stmt_start = true;
				continue;
			}
			expr_at_stmt_start = false;
		}

		if (FEAT(F_ORELSE) && (tok->tag & TT_TYPEOF)) {
			Token *next = try_typeof_orelse(tok);
			if (next) {
				tok = next;
				continue;
			}
		}
		{
			Token *r = emit_tok_checked(tok);
			if (r) {
				tok = r;
				continue;
			}
		}

		if (match_ch(tok, ';') || match_ch(tok, '{') || match_ch(tok, '}')) expr_at_stmt_start = true;
		else if (match_ch(tok, ':') && ternary_depth > 0) {
			ternary_depth--;
			expr_at_stmt_start = false;
		} else if (match_ch(tok, ':') && ternary_depth <= 0)
			expr_at_stmt_start = true;
		else
			expr_at_stmt_start = false;
		tok = tok_next(tok);
	}
	return tok;
}

static inline bool is_inside_attribute(Token *tok) {
	if (tok && (tok_ann(tok) & P1_IN_ATTR_ARGS)) return true;
	if (!last_emitted || (!match_ch(last_emitted, '(') && !match_ch(last_emitted, ','))) return false;
	for (Token *t = tok; t && t->kind != TK_EOF && !match_ch(t, ';') && !match_ch(t, '{');
	     t = tok_next(t)) {
		if (t->flags & TF_OPEN) {
			t = tok_match(t);
			continue;
		}
		if (match_ch(t, ')')) return true;
	}
	return false;
}

static void
reject_defer_context(Token *tok, bool ctrl_paren, bool ctrl_pending, bool in_stmt_expr, bool in_switch) {
	if (ctrl_paren) error_tok(tok, ERR_DEFER_CTRL_PAREN);
	if (ctrl_pending) error_tok(tok, ERR_DEFER_BRACELESS_CTRL);
	if (in_stmt_expr) error_tok(tok, ERR_DEFER_STMT_EXPR_TOP);
	if (in_switch) error_tok(tok, ERR_DEFER_SWITCH_BRACE);
}

static void reject_defer_fn_body(Token *tok, uint32_t body_tag) {
	static const struct {
		uint32_t tag;
		const char *msg;
	} tab[] = {
	    {TT_SPECIAL_FN,
	     "defer cannot be used in functions that call "
	     "setjmp/longjmp/pthread_exit"},
	    {TT_NORETURN_FN, "defer cannot be used in functions that call vfork()"},
	    {TT_ASM, "defer cannot be used in functions containing asm goto"},
	};

	for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
		if (body_tag & tab[i].tag) error_tok(tok, tab[i].msg);
}

static void reject_defer_unterminated(Token *tok, Token *body, Token *semi) {
	if (semi->kind == TK_EOF || !match_ch(semi, ';')) error_tok(tok, ERR_DEFER_UNTERMINATED);
	if (body && body->kind == TK_KEYWORD && (body->tag & (TT_NON_EXPR_STMT | TT_DEFER)))
		error_tok(tok, ERR_DEFER_MISSING_SEMI, body->len, tok_loc(body));
}

static Token *try_handle_defer_flow_kw(Token *tok) {
	uint32_t tag = tok->tag;
	if (!tag) return NULL;
	if ((tag & TT_DEFER) && !in_generic()) {
		Token *next = handle_defer_keyword(tok);
		if (next) return next;
	}
	if (FEAT(F_DEFER) && (tag & (TT_RETURN | TT_BREAK | TT_CONTINUE))) {
		Token *next = handle_control_exit_defer(tok);
		if (next) return next;
	}
	if ((tag & TT_GOTO) && FEAT(F_DEFER | F_ZEROINIT)) {
		Token *next = handle_goto_keyword(tok);
		if (next) return next;
	}
	return NULL;
}

static void arm_ctrl_pending_from_tag(Token *tok, uint32_t tag) {
	if (tag & TT_LOOP) {
		ctrl_state.pending_paren_kw = 1;
		if (is_do_kw(tok)) {
			ctrl_state.parens_just_closed = true;
			/* Match else / emit_statements: body is a statement. */
			ctx->at_stmt_start = true;
		}
		if (FEAT(F_DEFER | F_ZEROINIT)) {
			ctrl_state.pending = true;
			if (!is_do_kw(tok)) ctrl_state.parens_just_closed = false;
		}
		if (tok->ch0 == 'f' && FEAT(F_DEFER | F_ZEROINIT)) {
			ctrl_state.pending = true;
			ctrl_state.pending_for_paren = true;
		}
	}
	if (tag & TT_SWITCH) ctrl_state.pending_paren_kw = 2;
	if (FEAT(F_DEFER) && (tag & TT_SWITCH)) ctrl_state.pending = true;
	if ((tag & TT_SWITCH) && FEAT(F_DEFER | F_ZEROINIT)) {
		ctrl_state.pending = true;
		ctrl_state.pending_for_paren = true;
		ctrl_state.parens_just_closed = false;
	}
	if (tag & TT_IF) {
		ctrl_state.pending = true;
		if (is_else_kw(tok)) {
			ctrl_state.parens_just_closed = true;
			ctx->at_stmt_start = true;
		} else {
			ctrl_state.parens_just_closed = false;
			if (FEAT(F_DEFER | F_ZEROINIT)) ctrl_state.pending_for_paren = true;
		}
	}
}

static Token *handle_defer_keyword(Token *tok) {
	if (!FEAT(F_DEFER)) return NULL;
	/* Prefer Pass-1 classification; fall back when the bit was not set. */
	if (tok_ann(tok) & P1_IS_DEFER_KW) {
		/* ok */
	} else if (!is_defer_kw(tok, last_emitted)) {
		return NULL;
	}
	if (in_struct_body() || is_inside_attribute(tok)) return NULL;
	bool in_sw = false;
	for (int d = ctx->scope_depth - 1; d >= 0; d--) {
		if (scope_stack[d].kind != SCOPE_BLOCK) continue;
		in_sw = scope_stack[d].is_switch;
		break;
	}
	/* Keep in release: Phase 1 covers common paths, but control-paren /
	 * braceless forms still rely on this emission-time gate. */
	reject_defer_context(tok,
			     in_ctrl_paren(),
			     ctrl_state.pending && !in_ctrl_paren(),
			     scope_block_top() && scope_block_top()->is_stmt_expr,
			     in_sw);

	Token *defer_keyword = tok;
	tok = skip_noise(tok_next(tok));
	Token *stmt_start = tok;
	if (match_ch(stmt_start, '{') && tok_match(stmt_start)) {
		Token *close = tok_match(stmt_start);
		Token *after = tok_next(close);
		Token *stmt_end = after; // exclusive boundary — emits up to but not including
		defer_add(defer_keyword, stmt_start, stmt_end);
		tok = after;
		{
			if (tok && match_ch(tok, ';')) tok = tok_next(tok);
			end_statement_after_semicolon();
		}
		return tok;
	}

	Token *stmt_end = skip_to_semicolon(tok, NULL);
	reject_defer_unterminated(defer_keyword, stmt_start, stmt_end);
#ifdef PRISM_DEBUG
	/* Structural guard (twin of p1d_validate_defer's): never consume across
	 * the enclosing group's close token. */
	{
		int bd2 = 0;
		for (Token *s = stmt_start; s && s != stmt_end && s->kind != TK_EOF;
		     s = tok_next(s)) {
			if (s->flags & TF_OPEN) bd2++;
			else if (s->flags & TF_CLOSE) {
				bd2--;
				if (bd2 < 0)
					error_tok(defer_keyword,
						  "stray 'defer' in an expression position "
						  "(body would cross the enclosing ')' or ']')");
			}
		}
	}
#endif
	defer_add(defer_keyword, stmt_start, stmt_end);
	tok = (stmt_end->kind != TK_EOF) ? tok_next(stmt_end) : stmt_end;
	end_statement_after_semicolon();
	return tok;
}

static inline bool is_void_return(Token *tok) {
	bool returns_void = current_func_idx >= 0 && func_meta[current_func_idx].returns_void;
	return returns_void || (match_ch(tok, '(') && tok_next(tok) && equal(tok_next(tok), "void") &&
				tok_next(tok_next(tok)) && match_ch(tok_next(tok_next(tok)), ')'));
}

static Token *emit_return_body(Token *tok, Token *stop) {
	bool active = FEAT(F_DEFER) && has_active_defers();
	bool is_empty = match_ch(tok, ';') || (stop && tok == stop);
	if (active) {
		if (is_empty) {
			emit_all_defers();
			OUT_LIT(" return;");
		} else {
			bool is_void = is_void_return(tok);
			unsigned ret_id = ctx->ret_counter++;
			if (!is_void) {
				out_char(' ');
				emit_ret_type();
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

	if (match_ch(tok, ';')) tok = tok_next(tok);
	return tok;
}

static void emit_if_not_var(Token *var_name, bool open_brace) {
	if (var_name) {
		OUT_LIT(" if (!");
		OUT_TOK(var_name);
		out_char(')');
		if (open_brace) OUT_LIT(" {");
	} else if (open_brace) {
		OUT_LIT(" {");
	}
}

static Token *
emit_orelse_action(Token *tok, Token *var_name, bool has_const, bool single_eval_lhs, Token *stop_comma) {
	/* Phase 1 owns missing-action rejects: p1d_scan_init_orelse (decl init,
	 * incl. chains) and p1d_validate_bare_orelse (bare stmts, incl. chains). */
#ifdef PRISM_DEBUG
	require_orelse_action(tok, stop_comma);
#endif
	if (match_ch(tok, '{')) {
		emit_if_not_var(var_name, false);
		tok = emit_orelse_block_body(tok);
		if (tok && match_ch(tok, ';')) tok = tok_next(tok);
		return tok;
	}

	if (tok->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) {
		uint64_t tag = tok->tag;
		if (tag & TT_RETURN) tok = tok_next(tok);
		emit_if_not_var(var_name, true);
		if (tag & TT_RETURN) tok = emit_return_body(tok, stop_comma);
		else if (tag & (TT_BREAK | TT_CONTINUE))
			tok = emit_break_continue_defer(tok);
		else
			tok = emit_goto_defer(tok);
		OUT_LIT(" }");
		end_statement_after_semicolon();
		return tok;
	}

	/* Phase 1 p1d_validate_bare_orelse owns the no-target and const-LHS rejects. */
#ifdef PRISM_DEBUG
	if (!var_name) error_tok(tok, "orelse fallback requires an assignment target (use a declaration)");
	if (has_const) error_tok(tok, "orelse fallback cannot reassign a const-qualified variable");
#endif
	if (single_eval_lhs) {
		// `&(T){init}` in the if-substatement has statement scope (C11 §6.8.4.1p2);
		Token *probe = tok;
		if (match_ch(probe, '&')) {
			Token *lp = tok_next(probe);
			if (match_ch(lp, '(') && (lp->flags & TF_OPEN)) {
				Token *rp = tok_match(lp);
				if (rp) {
					Token *br = tok_next(rp);
					if (match_ch(br, '{') && (br->flags & TF_OPEN))
						error_tok(
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
	Token *chain_next;
	tok = emit_orelse_fallback_value(tok, stop_comma, &chain_next);
	if (chain_next) {
		if (single_eval_lhs) OUT_LIT("; }");
		else
			out_char(';');
		return emit_orelse_action(chain_next, var_name, has_const, single_eval_lhs, stop_comma);
	}
	if (single_eval_lhs) OUT_LIT("; }");
	else
		out_char(';');
	if (match_ch(tok, ';')) tok = tok_next(tok);
	end_statement_after_semicolon();
	return tok;
}

static Token *handle_control_exit_defer(Token *tok) {
	if (tok->tag & TT_RETURN) {
		if (!has_active_defers()) return NULL;
		tok = tok_next(tok);
		OUT_LIT(" {");
		tok = emit_return_body(tok, NULL);
		OUT_LIT(" }");
	} else {
		bool is_break = tok->tag & TT_BREAK;
		Token *after = tok_next(tok);
		bool has_label = after && is_identifier_like(after);
		bool need = false;
		if (has_label && FEAT(F_DEFER)) {
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
		tok = emit_break_continue_defer(tok);
		OUT_LIT(" }");
	}
	end_statement_after_semicolon();
	return tok;
}

// Find a label in the current function's Phase 1D P1FuncEntry array.
/* GNU __label__ stores names as "ident\\0scope_id" (mangled_len > ident len).
 * Match both plain labels and that prefix form. Prefer the deepest scope when
 * several __label__ bindings share a source name. */
static bool p1_label_matches_tok(P1FuncEntry *e, Token *tok) {
	if (e->kind != P1K_LABEL) return false;
	if ((uint32_t)e->label.len == tok->len && prism_memeq_runtime_sized(e->label.name, tok_loc(tok), tok->len)) return true;
	if ((uint32_t)e->label.len > tok->len && e->label.name[tok->len] == '\0' &&
	    prism_memeq_runtime_sized(e->label.name, tok_loc(tok), tok->len))
		return true;
	return false;
}

static P1LabelResult p1_label_find(Token *tok, int func_idx) {
	if (func_idx < 0 || func_idx >= func_meta_count) return (P1LabelResult){0, NULL};
	FuncMeta *fm = &func_meta[func_idx];
	P1FuncEntry *entries = &p1_entries[fm->entry_start];
	if (fm->label_hash) {
		uint32_t h = (uint32_t)fast_hash(tok_loc(tok), tok->len);
		for (int probe = 0; probe <= fm->label_hash_mask; probe++) {
			int slot = (h + probe) & fm->label_hash_mask;
			if (fm->label_hash[slot] < 0) break;
			P1FuncEntry *e = &entries[fm->label_hash[slot]];
			/* Exact-len hit only — mangled __label__ hashes differ. */
			if ((uint32_t)e->label.len == tok->len && p1_label_matches_tok(e, tok))
				return (P1LabelResult){scope_tree_depth(e->scope_id), e->tok};
		}
		/* Fall through: __label__ mangled names miss the unmangled hash. */
	}
	P1LabelResult best = {0, NULL};
	for (int i = 0; i < fm->entry_count; i++) {
		if (!p1_label_matches_tok(&entries[i], tok)) continue;
		int d = scope_tree_depth(entries[i].scope_id);
		if (!best.tok || d >= best.scope_depth)
			best = (P1LabelResult){d, entries[i].tok};
	}
	return best;
}

static int p1_goto_exits(Token *goto_tok, int func_idx) {
	if (func_idx < 0 || func_idx >= func_meta_count) return 0;
	FuncMeta *fm = &func_meta[func_idx];
	P1FuncEntry *entries = &p1_entries[fm->entry_start];
	uint32_t gt_idx = tok_idx(goto_tok);
	for (int i = goto_entry_cursor; i < fm->entry_count; i++) {
		if (entries[i].kind == P1K_GOTO && entries[i].token_index == gt_idx) {
			goto_entry_cursor = i + 1;
			return entries[i].label.exits;
		}
	}
	return 0;
}

static Token *handle_goto_keyword(Token *tok) {
	Token *goto_tok = tok;
	tok = tok_next(tok);
	if (FEAT(F_DEFER)) {
		// Skip C23 attributes between goto and target: goto [[attr]] *ptr;
		Token *after_attrs = skip_noise(tok);
		if (match_ch(after_attrs, '*')) {
			/* Phase 2A rejects computed goto + any defer in the function. */
#ifdef PRISM_DEBUG
			if (has_active_defers())
				error_tok(goto_tok,
					  "computed goto cannot be used with active defer statements");
#endif
			emit_tok(goto_tok);
			while (tok != after_attrs) {
				tok = emit_advance(tok);
			}
			return tok;
		}

		if (is_identifier_like(after_attrs)) {
			P1LabelResult info = p1_label_find(after_attrs, current_func_idx);
			int target_depth = info.tok ? info.scope_depth : ctx->block_depth;
			int exits = p1_goto_exits(goto_tok, current_func_idx);
			if (exits > 0) {
				target_depth = ctx->block_depth - exits;
				if (target_depth < 0) target_depth = 0;
			}

			if (goto_has_defers(target_depth)) {
				OUT_LIT(" {");
				emit_goto_defers(target_depth);
				OUT_LIT(" goto");
				// Emit any C23 attributes
				while (tok != after_attrs) {
					tok = emit_advance(tok);
				}
				tok = emit_advance(tok);
				if (match_ch(tok, ';')) {
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

static Token *handle_sue_body(Token *tok) {
	Token *brace = find_struct_body_brace(tok);
	if (!brace) return NULL;
	emit_range(tok, brace);
	emit_tok(brace);
	tok = tok_next(brace);
	scope_push_kind(SCOPE_BLOCK);
	scope_stack[ctx->scope_depth - 1].is_struct = true;
	ctx->at_stmt_start = true;
	return tok;
}

static Token *handle_open_brace(Token *tok) {
	bool did_push = false;
	if (ctrl_state.pending &&
	    (in_ctrl_paren() || !ctrl_state.parens_just_closed || (tok_ann(tok) & P1_SCOPE_INIT))) {
		if (last_emitted && match_ch(last_emitted, '(')) {
			VEC_ENSURE_REALLOC(ctrl_save_stack, ctrl_save_depth + 1, ctrl_save_cap, 16);
			ctrl_save_stack[ctrl_save_depth++] = ctrl_state;
			did_push = true;
		} else {
			emit_tok(tok);
			ctrl_state.brace_depth++;
			return tok_next(tok);
		}
	}
	ctrl_state.pending = false;
	ctrl_state.pending_for_paren = false;
	ctrl_state.parens_just_closed = false;
	bool is_stmt_expr = last_emitted && match_ch(last_emitted, '(');
	bool is_initializer = last_emitted && match_ch(last_emitted, '=');
	uint16_t ann = tok_ann(tok);
	bool is_init_scope = is_initializer || (ann & P1_SCOPE_INIT);
	Token *brace_tok = tok;
	Token *tok_before_brace = last_emitted;
	tok = emit_advance(tok);
	scope_push_kind(is_init_scope ? SCOPE_INIT : SCOPE_BLOCK);
	ScopeNode *s = &scope_stack[ctx->scope_depth - 1];
	s->is_loop = ann & P1_SCOPE_LOOP;
	s->is_switch = ann & P1_SCOPE_SWITCH;
	if (is_stmt_expr) s->is_stmt_expr = true;
	if (did_push) s->is_ctrl_se = true;
	if (is_init_scope) s->is_struct = true;
	/* Anonymous / keyword-headed aggregate: `union {`, `struct {`, `enum {`.
   * Phase 1 scope lookup may not associate every `{` token (e.g. inside
   * typeof(type)); without is_struct, in_struct_body() is false and the
   * decl-boundary newline heuristic splits member lists. */
	if (tok_before_brace && (tok_before_brace->tag & TT_SUE)) s->is_struct = true;
	// Phase 1A may have classified this as a struct-like scope
	if (!s->is_struct || !s->is_stmt_expr) {
		uint16_t sid = find_body_scope_id(brace_tok);
		if (sid) {
			if (!s->is_struct && scope_tree[sid].is_struct) s->is_struct = true;
			if (!s->is_stmt_expr && scope_tree[sid].is_stmt_expr) s->is_stmt_expr = true;
		}
	}

	if (s->is_stmt_expr) s->saved_defer_shadow_count = defer_shadow_count;
	if (ann & P1_RAW_BLOCK) ctx->raw_block_depth++;
	ctx->at_stmt_start = true;
	return tok;
}

static Token *handle_close_brace(Token *tok) {
	if (ctrl_state.pending && ctrl_state.brace_depth > 0) {
		ctrl_state.brace_depth--;
		emit_tok(tok);
		return tok_next(tok);
	}
	while (ctx->scope_depth > 0 && !is_brace_scope(scope_stack[ctx->scope_depth - 1].kind)) scope_pop();
	if (tok_match(tok) && (tok_ann(tok_match(tok)) & P1_RAW_BLOCK) && ctx->raw_block_depth > 0)
		ctx->raw_block_depth--;
	if (FEAT(F_DEFER) && ctx->scope_depth > 0) {
		ScopeNode *s = &scope_stack[ctx->scope_depth - 1];
		if (defer_count > s->defer_start_idx) {
			// Without an expression parser we cannot safely capture the
			// Phase 1 p1_check_defer_stmt_expr_chain owns this reject.
#ifdef PRISM_DEBUG
			{
				Token *nxt = skip_noise(tok_next(tok));
				for (int depth = ctx->scope_depth - 2; depth >= 0; depth--) {
					Token *probe = nxt;
					while (probe) {
						if (match_ch(probe, ';')) {
							probe = skip_noise(tok_next(probe));
							continue;
						}
						if (probe->kind == TK_IDENT || probe->kind == TK_KEYWORD) {
							Token *after = skip_noise(tok_next(probe));
							if (after && match_ch(after, ':')) {
								probe = skip_noise(tok_next(after));
								continue;
							}
						}
						break;
					}
					if (!probe || !match_ch(probe, '}'))
						break; // not a chain of closing braces
					if (scope_stack[depth].is_stmt_expr)
						error_tok(defer_stack[s->defer_start_idx].defer_kw,
							  ERR_DEFER_LAST_STMT_EXPR);
					nxt = skip_noise(tok_next(probe));
				}
			}
#endif
			emit_defers(DEFER_SCOPE);
			defer_count = s->defer_start_idx;
		}
	}

	bool restore_ctrl = ctx->scope_depth > 0 && scope_stack[ctx->scope_depth - 1].is_ctrl_se;
	bool closing_non_stmt_brace = ctx->scope_depth > 0 && scope_stack[ctx->scope_depth - 1].is_struct &&
				      !scope_stack[ctx->scope_depth - 1].is_stmt_expr;
	scope_pop();
	if (restore_ctrl && ctrl_save_depth > 0) ctrl_state = ctrl_save_stack[--ctrl_save_depth];
	emit_tok(tok);
	tok = tok_next(tok);
	ctx->at_stmt_start = !closing_non_stmt_brace;
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
	if (!args) error("out of memory");
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
	const char *cc = ctx->extra_compiler ? ctx->extra_compiler : PRISM_DEFAULT_CC;
	bool msvc = cc_is_msvc(cc);
	cc_split_into_argv(args, argc, cc, out_cc_dup);
	if (msvc) {
		args[(*argc)++] = "/E"; // preprocess to stdout
		args[(*argc)++] = "/nologo";
	} else {
		args[(*argc)++] = "-E";
		args[(*argc)++] = "-w";
	}

	for (int i = 0; i < ctx->extra_compiler_flags_count; i++) {
		const char *f = ctx->extra_compiler_flags[i];
		if (msvc) {
			// MSVC cl: /c compile-only, /Fo<path>|/Fo <path>, /Fe likewise.
			if (strcmp(f, "/c") == 0 || strcmp(f, "-c") == 0) continue;
			if (strncmp(f, "/Fo", 3) == 0 || strncmp(f, "/Fe", 3) == 0) {
				if ((strcmp(f, "/Fo") == 0 || strcmp(f, "/Fe") == 0) &&
				    i + 1 < ctx->extra_compiler_flags_count)
					i++; /* skip separate operand (may be /abs/path.obj) */
				continue;
			}
			if (strcmp(f, "/link") == 0) break;
			/* Skip other TUs/objects — including absolute paths (/tmp/x.obj). */
			if (is_pp_skip_input_arg(f)) {
				if (i > 0 && cc_flag_takes_arg(ctx->extra_compiler_flags[i - 1]))
					args[(*argc)++] = f;
				continue;
			}
		} else {
			if (strcmp(f, "-c") == 0 || strcmp(f, "-S") == 0) continue;
			/* Accidental MSVC slash-flags on a Unix driver. */
			if (strcmp(f, "/c") == 0) continue;
			if (strncmp(f, "/Fo", 3) == 0 || strncmp(f, "/Fe", 3) == 0) {
				if ((strcmp(f, "/Fo") == 0 || strcmp(f, "/Fe") == 0) &&
				    i + 1 < ctx->extra_compiler_flags_count)
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
				if (i > 0 && cc_flag_takes_arg(ctx->extra_compiler_flags[i - 1]))
					args[(*argc)++] = f;
				continue;
			}
		}
		args[(*argc)++] = f;
	}

	for (int i = 0; i < ctx->dep_flags_count; i++) args[(*argc)++] = ctx->dep_flags[i];
	for (int i = 0; i < ctx->extra_include_count; i++) {
		args[(*argc)++] = msvc ? "/I" : "-I";
		args[(*argc)++] = ctx->extra_include_paths[i];
	}

	if (msvc) {
		// MSVC: /D concatenated with macro
		int needed =
		    ctx->extra_define_count + 3; // +3 for __PRISM__, __PRISM_DEFER__, __PRISM_ZEROINIT__
		if (needed > pp_define_bufs_cap) {
			int old_cap = pp_define_bufs_cap;
			pp_define_bufs_cap = needed > 64 ? needed : 64;
			pp_define_bufs = realloc(pp_define_bufs, pp_define_bufs_cap * sizeof(char *));
			if (!pp_define_bufs) error("out of memory");
			for (int i = old_cap; i < pp_define_bufs_cap; i++) pp_define_bufs[i] = NULL;
		}
		int buf_idx = 0;
		for (int i = 0; i < ctx->extra_define_count; i++) {
			int len = snprintf(NULL, 0, "/D%s", ctx->extra_defines[i]) + 1;
			pp_define_bufs[buf_idx] = realloc(pp_define_bufs[buf_idx], len);
			if (!pp_define_bufs[buf_idx]) error("out of memory");
			snprintf(pp_define_bufs[buf_idx], len, "/D%s", ctx->extra_defines[i]);
			args[(*argc)++] = pp_define_bufs[buf_idx++];
		}
#define MSVC_DEFINE(str)                                                                                     \
	do {                                                                                                 \
		const char *_s = (str);                                                                      \
		int _l = (int)strlen(_s) + 1;                                                                \
		pp_define_bufs[buf_idx] = realloc(pp_define_bufs[buf_idx], _l);                              \
		if (!pp_define_bufs[buf_idx]) error("out of memory");                                        \
		memcpy(pp_define_bufs[buf_idx], _s, _l);                                                     \
		args[(*argc)++] = pp_define_bufs[buf_idx++];                                                 \
	} while (0)
		MSVC_DEFINE("/D__PRISM__=1");
		if (FEAT(F_DEFER)) MSVC_DEFINE("/D__PRISM_DEFER__=1");
		if (FEAT(F_ZEROINIT)) MSVC_DEFINE("/D__PRISM_ZEROINIT__=1");
#undef MSVC_DEFINE
	} else {
		for (int i = 0; i < ctx->extra_define_count; i++) {
			args[(*argc)++] = "-D";
			args[(*argc)++] = ctx->extra_defines[i];
		}
		args[(*argc)++] = "-D__PRISM__=1";
		if (FEAT(F_DEFER)) args[(*argc)++] = "-D__PRISM_DEFER__=1";
		if (FEAT(F_ZEROINIT)) args[(*argc)++] = "-D__PRISM_ZEROINIT__=1";
	}

	// Add GNU feature test macro on non-Windows, non-MSVC so POSIX/GNU
#ifndef _WIN32
	if (!msvc) {
		bool user_has_gnu = false;
		for (int i = 0; i < ctx->extra_define_count; i++) {
			if (strncmp(ctx->extra_defines[i], "_GNU_SOURCE", 11) == 0) user_has_gnu = true;
		}
		for (int i = 0; i < ctx->extra_compiler_flags_count; i++) {
			const char *f = ctx->extra_compiler_flags[i];
			if (strncmp(f, "-D_GNU_SOURCE", 13) == 0 || strncmp(f, "-U_GNU_SOURCE", 13) == 0)
				user_has_gnu = true;
			/* Split `-U _GNU_SOURCE` / `-D _GNU_SOURCE`. */
			if ((strcmp(f, "-U") == 0 || strcmp(f, "-D") == 0) &&
			    i + 1 < ctx->extra_compiler_flags_count &&
			    strncmp(ctx->extra_compiler_flags[i + 1], "_GNU_SOURCE", 11) == 0)
				user_has_gnu = true;
		}
		if (!user_has_gnu) args[(*argc)++] = "-D_GNU_SOURCE";
#ifdef __APPLE__
		{
			bool user_has_darwin = false;
			for (int i = 0; i < ctx->extra_define_count; i++)
				if (strncmp(ctx->extra_defines[i], "_DARWIN_C_SOURCE", 16) == 0)
					user_has_darwin = true;
			for (int i = 0; i < ctx->extra_compiler_flags_count; i++) {
				const char *f = ctx->extra_compiler_flags[i];
				if (strncmp(f, "-D_DARWIN_C_SOURCE", 18) == 0 ||
				    strncmp(f, "-U_DARWIN_C_SOURCE", 18) == 0)
					user_has_darwin = true;
			}
			if (!user_has_darwin) args[(*argc)++] = "-D_DARWIN_C_SOURCE";
		}
#endif
	}
#endif

	for (int i = 0; i < ctx->extra_force_include_count; i++) {
		args[(*argc)++] = msvc ? "/FI" : "-include";
		args[(*argc)++] = ctx->extra_force_includes[i];
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
	for (int i = 0; i < ctx->source_define_count; i++) {
		free(ctx->source_defines[i]);
		free(ctx->source_define_guards[i]);
	}
	ctx->source_defines = NULL;
	ctx->source_define_guards = NULL;
	ctx->source_define_count = 0;
	ctx->source_define_cap = 0;
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

static void collect_source_defines(const char *input_file) {
	free_source_defines();
	if (!input_file || FEAT(F_FLATTEN)) return;
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
				char *rd = NULL;
				if (has_unclosed_block_comment(p, &rd)) {
					in_block_comment = true;
				} else if (rd && cond_depth == 0) {
					in_raw_string = true;
					raw_delim = rd;
					raw_delim_len = (int)strlen(rd);
				} else
					free(rd);
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
					char *rd = NULL;
					if (has_unclosed_block_comment(p, &rd)) in_block_comment = true;
					else if (rd && cond_depth == 0) {
						in_raw_string = true;
						raw_delim = rd;
						raw_delim_len = (int)strlen(rd);
					} else
						free(rd);
					goto check_continuation;
				}
				goto have_hash;
			}
			{
				char *rd = NULL;
				if (has_unclosed_block_comment(p, &rd)) in_block_comment = true;
				else if (rd && cond_depth == 0) {
					in_raw_string = true;
					raw_delim = rd;
					raw_delim_len = (int)strlen(rd);
				} else
					free(rd);
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
				size_t nc = vec_grow_cap((size_t)old, (size_t)cond_depth + 1, 32);
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
			if (*p == '(') goto check_continuation;
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

			int old_cap = ctx->source_define_cap;
			ARENA_ENSURE_CAP(&ctx->main_arena,
					 ctx->source_defines,
					 ctx->source_define_count,
					 ctx->source_define_cap,
					 8,
					 char *);
			if ((int)ctx->source_define_cap != old_cap)
				ctx->source_define_guards =
				    arena_realloc(&ctx->main_arena,
						  ctx->source_define_guards,
						  sizeof(char *) * old_cap,
						  sizeof(char *) * ctx->source_define_cap);
			ctx->source_define_guards[ctx->source_define_count] = guard;
			ctx->source_defines[ctx->source_define_count++] = def;
		}
	check_continuation: {
		char *end = line + strlen(line);
		while (end > line && (end[-1] == '\n' || end[-1] == '\r')) end--;
		in_continuation = (end > line && end[-1] == '\\');
		if (!in_continuation && !in_block_comment && !in_raw_string) {
			char *rd = NULL;
			if (has_unclosed_block_comment(line, &rd)) in_block_comment = true;
			else if (rd && cond_depth == 0) {
				in_raw_string = true;
				raw_delim = rd;
				raw_delim_len = (int)strlen(rd);
			} else
				free(rd);
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

/* Read a source file into a malloc'd buffer with 8 trailing NUL bytes (tokenizer
 * SWAR padding). */
static char *read_file_padded(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long sz = ftell(f);
	if (sz < 0) {
		fclose(f);
		return NULL;
	}
	if (fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	char *buf = malloc((size_t)sz + 8);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	if (got != (size_t)sz) {
		free(buf);
		return NULL;
	}
	memset(buf + (size_t)sz, 0, 8);
	return buf;
}

static bool input_is_dot_i(const char *path) {
	size_t n = path ? strlen(path) : 0;
	return n >= 2 && path[n - 2] == '.' && (path[n - 1] == 'i' || path[n - 1] == 'I');
}

static char *preprocess_with_cc(const char *input_file) {
	collect_source_defines(input_file);
	/* `.i` is already preprocessed. GCC's `cc -E file.i` emits nothing, so
	 * re-running the preprocessor would drop the whole TU (including orelse). */
	if (input_is_dot_i(input_file)) {
		char *buf = read_file_padded(input_file);
		if (!buf) {
			fprintf(stderr, "error: cannot read preprocessed input: %s\n", input_file);
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
				"error: preprocessed input '%s' contains embedded null bytes\n",
				input_file);
			free(buf);
			return NULL;
		}
		if (sz >= 2) {
			unsigned char b0 = (unsigned char)buf[0], b1 = (unsigned char)buf[1];
			if ((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF)) {
				fprintf(stderr,
					"error: preprocessed input '%s' looks like UTF-16 (BOM); "
					"re-save as UTF-8/ASCII .i or pass the original .c\n",
					input_file);
				free(buf);
				return NULL;
			}
		}
		return buf;
	}
	const char *pp_cc = ctx->extra_compiler ? ctx->extra_compiler : PRISM_DEFAULT_CC;
	int argcap = 16 + cc_extra_arg_count(pp_cc) + ctx->extra_compiler_flags_count + ctx->dep_flags_count +
		     ctx->extra_include_count * 2 + ctx->extra_define_count * 2 +
		     ctx->extra_force_include_count * 2;
	const char **args = alloc_argv(argcap);
	int argc = 0;
	char *cc_dup = NULL;
	build_pp_argv(args, &argc, input_file, &cc_dup);
	char **argv = (char **)args;
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
		pid = 0; // not spawned — pid is undefined on error
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
				cap = vec_grow_cap(cap, len + 2, 8192);
				char *tmp = realloc(buf, cap);
				if (!tmp) goto cleanup;
				buf = tmp;
			}
		}
		close(read_fd);
		read_fd = -1;
		buf[len] = '\0';
		if (strlen(buf) < len) {
			fprintf(stderr, "error: preprocessor output contains null bytes\n");
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
	ScopeNode *sn = &scope_stack[ctx->scope_depth - 1];
	uint8_t pk = ctrl_state.pending_paren_kw;
	ctrl_state.pending_paren_kw = 0;
	if (pk == 1) sn->is_loop = true;
	else if (pk == 2)
		sn->is_switch = true;
	ctx->at_stmt_start = (k == SCOPE_FOR_PAREN);
}

static inline void track_ctrl_paren_close(void) {
	// ctrl_state.pending is still live) must not leak parens_just_closed.
	if (ctx->scope_depth == 0) return;
	ScopeKind k = scope_stack[ctx->scope_depth - 1].kind;
	if (k != SCOPE_FOR_PAREN && k != SCOPE_CTRL_PAREN) return;
	scope_pop();
	ctrl_state.parens_just_closed = true;
	ctx->at_stmt_start = true;
}

static inline void track_ctrl_semicolon(void) {
	if (in_for_init()) {
		/* for/switch/if C23 init: FOR_PAREN → CTRL_PAREN after first ';'.
     * Preserve is_loop/is_switch so break/continue in the condition
     * (or for-increment) still stop at this paren for defer_walk. */
		bool was_loop = scope_stack[ctx->scope_depth - 1].is_loop;
		bool was_switch = scope_stack[ctx->scope_depth - 1].is_switch;
		scope_pop();
		scope_push_kind(SCOPE_CTRL_PAREN);
		scope_stack[ctx->scope_depth - 1].is_loop = was_loop;
		scope_stack[ctx->scope_depth - 1].is_switch = was_switch;
	} else if (!in_ctrl_paren())
		ctrl_reset();
}

static PRISM_ALWAYS_INLINE inline void track_generic_token(Token *tok) {
	if (tok->len != 1) return;
	char c = tok->ch0;
	if (c != '(' && c != ')') return;
	if (!in_generic()) return;
	if (c == '(') scope_push_kind(SCOPE_GENERIC);
	else if (ctx->scope_depth > 0 && scope_stack[ctx->scope_depth - 1].kind == SCOPE_GENERIC)
		scope_pop();
}

/* Attribute-arg bits are filled by p1_annotate_pool before Pass 2 emission. */
static bool token_inside_gnu_attr_args(Token *tok) {
	return (tok_ann(tok) & P1_IN_ATTR_ARGS) != 0;
}

static PRISM_ALWAYS_INLINE inline void track_common_token_state(Token *tok) {
	if (__builtin_expect(ctrl_state.pending && tok->len == 1, 0)) {
		char c = tok->ch0;
		if (!in_generic()) {
			if (c == '(') {
				/* Entire `__attribute__((...))` group is opaque — including
         * the inner `((` and nested attribute argument parens. */
				if (token_inside_gnu_attr_args(tok))
					;
				/* After the control condition has closed, a leading '(' is a
         * parenthesized braceless body / expression — not another
         * condition paren. Nested if/while still arm pending_for_paren
         * / pending_paren_kw (or clear parens_just_closed) first. */
				else if (!(ctrl_state.parens_just_closed && !ctrl_state.pending_for_paren &&
					   !ctrl_state.pending_paren_kw))
					track_ctrl_paren_open();
			} else if (c == ')') {
				if (!token_inside_gnu_attr_args(tok)) track_ctrl_paren_close();
			}
		}
	}
	track_generic_token(tok);
}

/* Skip (no emit) statement prefixes: attrs/_Pragma and one-or-more labels. */
static Token *skip_stmt_prefixes(Token *tok) {
	for (;;) {
		Token *after = skip_noise(tok);
		if (after != tok) {
			tok = after;
			continue;
		}
		if (is_identifier_like(tok)) {
			Token *colon = skip_noise(tok_next(tok));
			if (colon && match_ch(colon, ':') &&
			    !(tok_next(colon) && match_ch(tok_next(colon), ':'))) {
				tok = tok_next(colon);
				continue;
			}
		}
		break;
	}
	return tok;
}

/* Emit tokens in [from, to). */
static Token *emit_through(Token *from, Token *to) {
	while (from && from != to && from->kind != TK_EOF) from = emit_advance(from);
	return from;
}

static Token *find_bare_orelse(Token *tok) {
	if (!p1_file_has_orelse) return NULL;
	Token *prev = NULL;
	int ternary = 0;
	for (Token *s = tok; s->kind != TK_EOF; s = tok_next(s)) {
		if (s->flags & TF_OPEN) {
			prev = tok_match(s);
			s = tok_match(s);
			continue;
		}
		if ((s->flags & TF_CLOSE) || match_ch(s, ';')) return NULL;
		if (match_ch(s, '?')) {
			ternary++;
			prev = s;
			continue;
		}
		if (match_ch(s, ':') && ternary > 0) {
			ternary--;
			prev = s;
			continue;
		}
		/* `return c ? g() orelse 0 : 1` — orelse inside `?:` must reject.
		 * Decl-init has its own ternary tracker; bare/expr stmts relied on
		 * find_bare_orelse skipping depth>0, which silently leaked. */
		if (ternary > 0) {
			bool is_oe = (tok_ann(s) & P1_IS_ORELSE_KW) != 0;
			if (!is_oe && !p1_typedef_annotated) is_oe = orelse_kw_at_bare(s, prev);
			if (is_oe && !(tok_ann(s) & (P1_OE_DECL_INIT | P1_OE_BRACKET))) {
#ifndef PRISM_DEBUG
				if (!p1_typedef_annotated)
#endif
					error_tok(s, ERR_ORELSE_TERNARY);
			}
			prev = s;
			continue;
		}
		if ((tok_ann(s) & P1_IS_ORELSE_KW) &&
		    !(tok_ann(s) & (P1_OE_DECL_INIT | P1_OE_BRACKET)))
			return s;
		/* During Phase 1 discovery, ann bits are not set yet. */
		if (!p1_typedef_annotated && orelse_kw_at_bare(s, prev)) return s;
		prev = s;
	}
	return NULL;
}

static bool orelse_has_chain(Token *start, bool comma_term) {
	int pd = 0;
	Token *prev = NULL;
	for (Token *p = start; p->kind != TK_EOF; p = tok_next(p)) {
		if (p->flags & TF_OPEN) pd++;
		else if (p->flags & TF_CLOSE)
			pd--;
		else if (pd == 0 && (match_ch(p, ';') || (comma_term && match_ch(p, ','))))
			break;
		if (pd == 0) {
			if (tok_ann(p) & P1_IS_ORELSE_KW) return true;
			if (!p1_typedef_annotated && orelse_kw_at_bare(p, prev)) return true;
			prev = p;
		}
	}
	return false;
}

static bool bare_is_stmt_end(Token *s, bool comma_term) {
	return match_ch(s, ';') || (comma_term && match_ch(s, ','));
}

/* Walk until stmt end / EOF. cb sees token before depth is updated.
 * cb return true → stop and return that token. */
static Token *bare_walk_depth0(Token *start,
			       bool comma_term,
			       bool (*cb)(Token *s, Token *prev, int sd, void *ud),
			       void *ud) {
	int sd = 0;
	Token *prev = NULL;
	for (Token *s = start; s && s->kind != TK_EOF; s = tok_next(s)) {
		if (cb && cb(s, prev, sd, ud)) return s;
		if (s->flags & TF_OPEN) sd++;
		else if (s->flags & TF_CLOSE)
			sd--;
		else if (sd == 0 && bare_is_stmt_end(s, comma_term))
			return s;
		prev = s;
	}
	return NULL;
}

#ifdef PRISM_DEBUG
/* Only the ERR_BARE_ORELSE_SPANS_PP debug assert walks with this callback. */
static bool bare_cb_pp_cond(Token *s, Token *prev, int sd, void *ud) {
	(void)prev;
	if (sd == 0 && is_pp_conditional(s)) {
		*(Token **)ud = s;
		return true;
	}
	return false;
}
#endif

static bool bare_cb_compound_lit(Token *s, Token *prev, int sd, void *ud) {
	if (match_ch(s, '{') && (sd == 0 || (prev && match_ch(prev, ')')))) {
		*(bool *)ud = true;
		return true;
	}
	return false;
}

static Token *
bare_emit_fallback_expr(Token *t, bool comma_term, Token *lhs, Token *eq, bool restart_on_orelse) {
	int fd = 0;
	if (restart_on_orelse) {
		if (orelse_has_chain(t, comma_term)) OUT_LIT("(");
		else
			OUT_LIT("(void)(");
		emit_range_no_prep(lhs, eq);
		OUT_LIT(" = (");
	}
	while (t->kind != TK_EOF) {
		if (FEAT(F_ORELSE) && (t->tag & TT_TYPEOF)) {
			Token *next = try_typeof_orelse(t);
			if (next) {
				t = next;
				continue;
			}
		}
		if ((t->flags & TF_OPEN) && (match_ch(t, '(') || match_ch(t, '['))) {
			t = walk_balanced(t, true);
			continue;
		}
		if (t->flags & TF_OPEN) fd++;
		else if (t->flags & TF_CLOSE)
			fd--;
		else if (fd == 0 && bare_is_stmt_end(t, comma_term))
			break;
		if (restart_on_orelse && fd == 0 && is_orelse_keyword(t)) {
			OUT_LIT(")) ? (void)0 : ");
			t = tok_next(t);
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

static void emit_bare_oe_if_temp(unsigned oe_id, Token *lhs, Token *eq) {
	OUT_LIT(" if (__prism_oe_");
	out_uint(oe_id);
	OUT_LIT(") { ");
	emit_range_no_prep(lhs, eq);
	OUT_LIT(" = __prism_oe_");
	out_uint(oe_id);
	OUT_LIT("; } else { ");
}

static Token *emit_bare_orelse_impl(Token *t, Token *end, bool comma_term, bool brace_wrap) {
	Token *orelse_tok = find_bare_orelse(t);
	if (!orelse_tok || (end && tok_loc(orelse_tok) >= tok_loc(end))) return NULL;
#ifdef PRISM_DEBUG
	if (is_orelse_keyword(t)) error_tok(t, "expected expression before 'orelse'");
#endif

	Token *last_comma = comma_term ? last_depth0_comma(t, orelse_tok) : NULL;
	Token *post_comma_t = last_comma ? tok_next(last_comma) : t;
	Token *bare_lhs_start = post_comma_t;
	Token *bare_assign_eq = find_depth0_assign_eq(post_comma_t, orelse_tok);

	Token *after_orelse = tok_next(orelse_tok);
	bool is_bare_fallback = bare_assign_eq && is_orelse_value_fallback(after_orelse);
	if (bare_assign_eq && is_bare_fallback) {
#ifdef PRISM_DEBUG
		reject_orelse_side_effects(bare_lhs_start,
					   bare_assign_eq,
					   "orelse fallback on assignment",
					   "in the target expression",
					   true,
					   false,
					   true);
#endif
	}

	if (!is_bare_fallback) return NULL; // caller handles non-bare fallback

	if (brace_wrap) OUT_LIT(" {");
	if (last_comma) {
		emit_range(t, last_comma);
		out_char(';');
		t = post_comma_t;
	}

	// We cannot statically evaluate which branch is active, so error here.
	if (bare_assign_eq) {
#ifdef PRISM_DEBUG
		Token *ppc = NULL;
		bare_walk_depth0(bare_lhs_start, comma_term, bare_cb_pp_cond, &ppc);
		if (ppc) error_tok(orelse_tok, ERR_BARE_ORELSE_SPANS_PP);
#endif
	}

	// Hoist preprocessor directives before the wrapper
	for (Token *s = t; s != orelse_tok; s = tok_next(s)) {
		if (s->kind == TK_PREP_DIR) {
			emit_tok(s);
			out_char('\n');
			ctx->last_line_no++;
		}
	}
	bool fallback_has_compound_literal = false;
	bare_walk_depth0(after_orelse, comma_term, bare_cb_compound_lit, &fallback_has_compound_literal);

	out_char(' ');
	// a constraint violation (C23 §6.7.2.5p2).
	// function return types are never VM (C11 §6.7.6.3p1).
	bool lhs_has_indirection = bare_assign_eq &&
				   bare_lhs_has_indirection(bare_lhs_start, bare_assign_eq);
	if (fallback_has_compound_literal) {
		if (bare_assign_eq) {
#ifdef PRISM_DEBUG
			reject_orelse_side_effects(bare_lhs_start,
						   bare_assign_eq,
						   "orelse compound-literal fallback on assignment",
						   "in the target expression (volatile double-write "
						   "with compound literal fallback); "
						   "use a temporary variable instead",
						   false,
						   true,
						   false);
#endif
		}
		OUT_LIT("(");
		emit_balanced_range(bare_lhs_start, orelse_tok);
		OUT_LIT(") ? (void)0 : ");
		t = bare_emit_fallback_expr(after_orelse, comma_term, bare_lhs_start, bare_assign_eq, true);
		OUT_LIT("));");
	} else {
		// (C23 §6.7.2.5p2), covered by the . / -> check.
		unsigned oe_id = ctx->ret_counter++;
		bool rhs_has_member = false;
		for (Token *s = tok_next(bare_assign_eq); s && s != orelse_tok; s = tok_next(s))
			if (s->tag & TT_MEMBER) {
				rhs_has_member = true;
				break;
			}
		OUT_LIT("{ ");
		emit_typeof_keyword();
		out_char('(');
		if (lhs_has_indirection) {
			/* Phase 1D owns RHS side-effect / ctrl-flow rejects. */
#ifdef PRISM_DEBUG
			Token *rhs_s = tok_next(bare_assign_eq);
			if (!is_strictly_bare_call(rhs_s, orelse_tok))
				reject_orelse_side_effects(
				    rhs_s,
				    orelse_tok,
				    "bare orelse with indirection in LHS",
				    "in the RHS (typeof(RHS) may evaluate for VM types "
				    "per C11 6.7.2.4p2; hoist to a variable)",
				    false,
				    false,
				    true);
			for (Token *ck = rhs_s; ck && ck != orelse_tok && ck->kind != TK_EOF;
			     ck = tok_next(ck))
				if (ck->tag & (TT_GOTO | TT_RETURN | TT_BREAK | TT_CONTINUE | TT_DEFER))
					error_tok(ck,
						  "bare assignment orelse with LHS indirection "
						  "cannot contain control flow keywords in the "
						  "right-hand expression (the expression is "
						  "duplicated inside typeof(), which would corrupt "
						  "transpiler control-flow tracking); hoist the "
						  "expression to a variable first");
#endif
			/* Bit-field members are not valid typeof operands; `+ 0`
			 * forces integer promotion. Skip for plain idents/calls. */
			if (rhs_has_member) OUT_LIT("(");
			emit_balanced_range(tok_next(bare_assign_eq), orelse_tok);
			if (rhs_has_member) OUT_LIT(")+0");
		} else
			emit_range_no_prep(bare_lhs_start, bare_assign_eq);
		OUT_LIT(") __prism_oe_");
		out_uint(oe_id);
		OUT_LIT(" = (");
		emit_balanced_range(tok_next(bare_assign_eq), orelse_tok);
		OUT_LIT(");");
		t = after_orelse;
		{
			int nest = 0;
			while (true) {
				bool is_last = !orelse_has_chain(t, comma_term);
				emit_bare_oe_if_temp(oe_id, bare_lhs_start, bare_assign_eq);
				if (is_last) {
					/* Chain ending in return/goto/break/continue/{...}
					 * must use action lowering — wrapping `return` in
					 * `(...)` is a hard backend error. */
					if ((t->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) ||
					    match_ch(t, '{')) {
						t = emit_orelse_action(t, NULL, false, false, NULL);
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
				Token *fb_start = t;
				Token *fb_orelse = NULL;
				{
					int fd = 0;
					Token *sprev = NULL;
					for (Token *s = t; s->kind != TK_EOF; s = tok_next(s)) {
						if ((s->flags & TF_OPEN) &&
						    (match_ch(s, '(') || match_ch(s, '['))) {
							sprev = tok_match(s);
							s = tok_match(s);
							continue;
						}
						if (s->flags & TF_OPEN) fd++;
						else if (s->flags & TF_CLOSE)
							fd--;
						else if (fd == 0 && bare_is_stmt_end(s, comma_term))
							break;
						if (fd == 0 && ((tok_ann(s) & P1_IS_ORELSE_KW) ||
								orelse_kw_at_bare(s, sprev))) {
							fb_orelse = s;
							break;
						}
						if (fd == 0) sprev = s;
					}
				}
				oe_id = ctx->ret_counter++;
				emit_typeof_keyword();
				out_char('(');
				if (lhs_has_indirection) {
					bool mid_has_member = false;
					for (Token *s = fb_start; s && s != fb_orelse; s = tok_next(s))
						if (s->tag & TT_MEMBER) {
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
				t = tok_next(fb_orelse);
			}
			for (int i = 0; i < nest; i++) OUT_LIT(" }");
		}
		OUT_LIT(" }");
	}
	if (bare_is_stmt_end(t, comma_term)) t = tok_next(t);
	if (brace_wrap) OUT_LIT(" }");
	if (end && tok_idx(t) > tok_idx(end)) t = end;
	return t;
}

static Token *emit_orelse_condition_wrap(Token *t, Token *orelse_tok) {
	for (Token *s = t; s != orelse_tok; s = tok_next(s))
		if (s->kind == TK_PREP_DIR) {
			emit_tok(s);
			out_char('\n');
			ctx->last_line_no++;
		}
	OUT_LIT(" {");
	OUT_LIT(" if (!(");
	emit_range_no_prep(t, orelse_tok);
	OUT_LIT("))");
	return tok_next(orelse_tok);
}

static Token *emit_deferred_orelse(Token *t, Token *end) {
	Token *body = skip_stmt_prefixes(t);
	Token *orelse_tok = find_bare_orelse(body);
	if (!orelse_tok || (end && tok_loc(orelse_tok) >= tok_loc(end))) return NULL;
	t = emit_through(t, body);
	Token *result = emit_bare_orelse_impl(t, end, false, false);
	if (result) return result;
	t = emit_orelse_condition_wrap(t, orelse_tok);
#ifdef PRISM_DEBUG
	if (match_ch(t, ';')) error_tok(t, "expected statement after 'orelse'");
#endif
	t = emit_orelse_action(t, NULL, false, false, NULL);
	OUT_LIT(" }");
	if (match_ch(t, ';')) t = tok_next(t);
	if (end && tok_idx(t) > tok_idx(end)) t = end;
	return t;
}

static inline Token *try_process_stmt_token(Token *t, Token *end, Token **unreachable_tok) {
	/* Match Pass 2 main loop: try_zero_init_decl also handles decl-init /
   * bracket orelse (and auto-static) when F_ZEROINIT is off. Gating on
   * F_ZEROINIT alone sent `int t = get() orelse 0;` inside defer bodies
   * through emit_deferred_orelse, which treated the type keyword as a
   * bare-orelse LHS and emitted invalid `__typeof__(int t)`. */
	if (ctx->at_stmt_start) {
		Token *next = try_zero_init_decl(t);
		if (next) return next;
	}
	if (ctx->at_stmt_start && FEAT(F_ORELSE) && !(t->tag & TT_NON_EXPR_STMT)) {
		Token *next = emit_deferred_orelse(t, end);
		if (next) {
			ctx->at_stmt_start = true;
			return next;
		}
	}
	{
		Token *r = try_strip_raw(t);
		if (r) return r;
	}
	if (FEAT(F_AUTO_UNREACHABLE) && !(ctrl_state.pending && ctrl_state.parens_just_closed)) {
		Token *nr = try_detect_noreturn_call(t);
		if (nr && nr != end) *unreachable_tok = nr;
	}
	return NULL;
}

static void emit_deferred_range(Token *start, Token *end) {
	bool saved_stmt_start = ctx->at_stmt_start;
	CtrlState saved_ctrl = ctrl_state;
	ctrl_reset();
	ctx->at_stmt_start = true;
	emit_statements(start, end, EMIT_DEFER_BODY);
	ctx->at_stmt_start = saved_stmt_start;
	ctrl_state = saved_ctrl;
}

// --- Phase 1: Static Analysis ---

// Phase 1A: walk all tokens, assign scope_ids, build scope_tree[] with parent
// links + flags.

static bool is_objc_ivar_brace(uint32_t brace_idx) {
	for (uint32_t i = brace_idx - 1; i > 0; i--) {
		Token *t = &token_pool[i];
		if (t->kind == TK_PREP_DIR) continue;
		if (t->kind == TK_IDENT && !t->tag) continue; // class name, protocol name
		if (match_ch(t, ':')) continue;		      // inheritance colon
		if (match_ch(t, '*')) continue;		      // pointer in type
		if (t->tag & (TT_QUALIFIER | TT_ATTR)) continue;
		if (match_ch(t, ')') && tok_match(t)) {
			i = tok_idx(tok_match(t));
			continue;
		}
		if (match_ch(t, ']') && tok_match(t) && (tok_match(t)->flags & TF_C23_ATTR)) {
			i = tok_idx(tok_match(t));
			continue;
		}
		if (match_ch(t, '>')) {
			int depth = 1;
			while (i > 1 && depth > 0) {
				i--;
				Token *inner = &token_pool[i];
				if (inner->kind == TK_PREP_DIR) continue;
				if (match_ch(inner, '>')) depth++;
				else if (match_ch(inner, '<'))
					depth--;
			}
			continue;
		}
		if (match_ch(t, '@')) {
			Token *kw = &token_pool[i + 1];
			if (kw->kind == TK_IDENT &&
			    ((kw->len == 9 && prism_memeq_static(tok_loc(kw), "interface", 9)) ||
			     (kw->len == 14 && prism_memeq_static(tok_loc(kw), "implementation", 14)) ||
			     (kw->len == 8 && prism_memeq_static(tok_loc(kw), "protocol", 8))))
				return true;
		}
		return false;
	}
	return false;
}

static void p1_build_scope_tree(Token *start) {
	// scope_id 0 is reserved for file scope (never stored in scope_tree[])
	scope_tree_count = 1; // start at 1; 0 = file scope sentinel
	scope_tree_cap = 0;
	ctx->p1_scope_tree = NULL;
	int p1a_stack_cap = 256;
	uint16_t *scope_stack_local = arena_alloc_uninit(&ctx->main_arena, p1a_stack_cap * sizeof(uint16_t));
	scope_stack_local[0] = 0; // file scope
	int depth = 0;
	for (Token *t = start; t && t->kind != TK_EOF; t = tok_next(t)) {
		if (t->tag & TT_ORELSE) p1_file_has_orelse = true;
		if (match_ch(t, '{')) {
			uint16_t sid = scope_tree_count;
			if (sid == UINT16_MAX) error_tok(t, "scope tree: too many scopes (>65534)");
			ARENA_ENSURE_CAP(&ctx->main_arena,
					 ctx->p1_scope_tree,
					 scope_tree_count,
					 scope_tree_cap,
					 256,
					 ScopeInfo);
			ScopeInfo *si = &scope_tree[sid];
			*si = (ScopeInfo){.parent_id = scope_stack_local[depth]};
			si->open_tok_idx = tok_idx(t);
			si->close_tok_idx = tok_match(t) ? tok_idx(tok_match(t)) : UINT32_MAX;
			uint32_t tidx = tok_idx(t);
			Token *prev = tok_walk_back(tidx - 1, WB_SKIP_NOISE);
			if (prev) {
				if (is_do_kw(prev)) {
					si->is_loop = true;
				} else if (match_ch(prev, ')') && tok_match(prev)) {
					Token *open_paren = tok_match(prev);
					Token *kw = tok_walk_back(tok_idx(open_paren) - 1, WB_SKIP_NOISE);
					if (kw && (kw->tag & TT_ATTR))
						kw = tok_walk_back(tok_idx(kw) - 1, WB_SKIP_ATTRS);
					if (kw) {
						if (kw->tag & TT_LOOP) si->is_loop = true;
						else if (kw->tag & TT_SWITCH)
							si->is_switch = true;
						else if (kw->tag & TT_IF)
							si->is_conditional = true;
						else if (kw->tag & TT_SUE) {
							si->is_struct = true;
							if (is_enum_kw(kw)) si->is_enum = true;
						}
					}
					if (depth == 0 && !si->is_loop && !si->is_switch &&
					    !si->is_conditional && !si->is_struct)
						si->is_func_body = true;
					/* GNU nested function `T name(...){` at block depth —
					 * not an initializer. `prev`/`open_paren` already skip
					 * post-declarator attrs (WB_SKIP_NOISE). Compound
					 * literals `(T){` fail paren_is_function_declarator_params
					 * and stay is_init; stmt-exprs are `({` not `){`. */
					if (depth > 0 && !si->is_loop && !si->is_switch &&
					    !si->is_conditional && !si->is_struct && !si->is_func_body) {
						if (paren_is_function_declarator_params(open_paren))
							si->is_func_body = true;
						else
							si->is_init = true;
					}
				} else if (is_else_kw(prev)) {
					si->is_conditional = true;
				} else if (prev->tag & TT_SUE) {
					si->is_struct = true;
					if (is_enum_kw(prev)) si->is_enum = true;
				} else if (prev->kind == TK_IDENT &&
					   !(prev->tag & (TT_TYPE | TT_QUALIFIER | TT_LOOP | TT_SWITCH |
							  TT_IF | TT_STORAGE))) {
					Token *sue = tok_walk_back(tok_idx(prev) - 1, WB_SKIP_ATTRS);
					if (sue && (sue->tag & TT_SUE)) {
						si->is_struct = true;
						if (is_enum_kw(sue)) si->is_enum = true;
					}
				} else if (is_c23_fixed_underlying_enum(prev)) {
					si->is_struct = true;
					si->is_enum = true;
				} else if (depth == 0 && (match_ch(prev, ']') || match_ch(prev, ';'))) {
					si->is_func_body = true;
				}
			}

			if (!si->is_struct && !si->is_loop && !si->is_switch && !si->is_conditional &&
			    is_objc_ivar_brace(tidx)) {
				si->is_struct = true;
				si->is_func_body = false;
				si->is_init = false;
			}

			if (prev && match_ch(prev, '(')) si->is_stmt_expr = true;
			if (!si->is_func_body && !si->is_loop && !si->is_switch && !si->is_conditional &&
			    !si->is_struct && !si->is_stmt_expr) {
				if (prev && match_ch(prev, '=')) {
					si->is_init = true;
				} else if (depth > 0 && scope_stack_local[depth] < scope_tree_count &&
					   scope_tree[scope_stack_local[depth]].is_init) {
					si->is_init = true;
				}
			}

			uint16_t ann = 0;
			if (si->is_loop) ann |= P1_SCOPE_LOOP;
			if (si->is_switch) ann |= P1_SCOPE_SWITCH;
			if (si->is_init) ann |= P1_SCOPE_INIT;
			token_pool[tidx].ann = ann;
			// typedefs, or goto targets — they don't need their own scope tree entry.
			bool reuse_parent =
			    (si->is_init && depth > 0 && scope_stack_local[depth] < scope_tree_count &&
			     scope_tree[scope_stack_local[depth]].is_init);
			if (!reuse_parent) scope_tree_count++;
			depth++;
			ARENA_ENSURE_CAP(&ctx->main_arena,
					 scope_stack_local,
					 depth + 1,
					 p1a_stack_cap,
					 256,
					 uint16_t);
			scope_stack_local[depth] = reuse_parent ? scope_stack_local[depth - 1] : sid;
			continue;
		}

		if (match_ch(t, '}')) {
			if (depth > 0) depth--;
			continue;
		}
	}
}

// Phase 1B: walk all tokens at all depths to build the complete typedef + enum
// table.

static void
p1_register_param_shadows(Token *open, Token *close, uint16_t scope_id, int brace_depth, bool check_vla) {
	for (Token *t = tok_next(open); t && t != close && t->kind != TK_EOF;) {
		Token *param_start = t;
		Token *last_ident = NULL;
		bool scanned_inner_paren = false;
		bool ident_from_inner = false;
		while (t && t != close && !match_ch(t, ',') && t->kind != TK_EOF) {
			if (t->flags & TF_OPEN) {
				if (!last_ident && !scanned_inner_paren && match_ch(t, '(') && tok_match(t))
					for (Token *s = tok_next(t); s != tok_match(t); s = tok_next(s)) {
						if (s->flags & TF_OPEN) {
							s = tok_match(s);
							continue;
						}
						if (is_valid_varname(s) &&
						    (!(s->tag & (TT_QUALIFIER | TT_TYPE | TT_SUE | TT_TYPEOF |
								 TT_ATTR)) ||
						     (s->tag & (TT_DEFER | TT_ORELSE)) ||
						     (s->flags & TF_RAW))) {
							last_ident = s;
							ident_from_inner = true;
						}
					}
				if (match_ch(t, '(')) scanned_inner_paren = true;
				t = tok_match(t) ? tok_next(tok_match(t)) : tok_next(t);
				continue;
			}
			if (is_valid_varname(t) &&
			    (!(t->tag & (TT_QUALIFIER | TT_TYPE | TT_SUE | TT_TYPEOF | TT_ATTR)) ||
			     (t->tag & (TT_DEFER | TT_ORELSE)) || (t->flags & TF_RAW))) {
				last_ident = t;
				ident_from_inner = false;
			}
			t = tok_next(t);
		}
		if (last_ident &&
		    (is_known_typedef(last_ident) || is_known_enum_const(last_ident) ||
		     (last_ident->tag & (TT_DEFER | TT_ORELSE | TT_NORETURN_FN | TT_SPECIAL_FN)) ||
		     (last_ident->flags & TF_RAW) ||
		     hashmap_get(&p1_func_proto_map, tok_loc(last_ident), last_ident->len)))
			p1_register_shadow(last_ident, scope_id, brace_depth);
		if (check_vla && last_ident) {
			bool skip_first = !ident_from_inner;
			Token *param_end = (t && match_ch(t, ',')) ? t : close;
			for (Token *s = param_start; s && s != param_end && s->kind != TK_EOF;
			     s = tok_next(s))
				if (match_ch(s, '[')) {
					if (skip_first) {
						skip_first = false;
						if (tok_match(s)) s = tok_match(s);
						continue;
					}
					if (array_size_is_vla(s)) {
						TYPEDEF_ADD_IDX(typedef_add_vla_var(tok_loc(last_ident),
										    last_ident->len,
										    brace_depth),
								last_ident);
						typedef_table.entries[typedef_table.count - 1].is_param =
						    true;
						break;
					}
				}
		}
		if (last_ident && scope_id < scope_tree_count) {
			Token *param_end = (t && match_ch(t, ',')) ? t : close;
			/* A plain built-in scalar param (`int x`, `char *p`) needs no
			 * is_param shadow — and registering one per function makes
			 * typedef_lookup O(n^2) when many functions share a name (`x`,`n`,
			 * `p`). Array/VLA params (sizeof decays), volatile params, and
			 * anything whose array-ness could hide in a typedef / K&R decl
			 * still need the entry, so skip ONLY definite built-in scalars. */
			bool param_has_bracket = false;
			for (Token *s = param_start; s && s != param_end && s->kind != TK_EOF; s = tok_next(s))
				if (match_ch(s, '[') && (s->flags & TF_OPEN) && !(s->flags & TF_C23_ATTR)) {
					param_has_bracket = true;
					break;
				}
			bool has_vol_qual = false;
			bool has_vol_member = false;
			bool has_atomic_qual = false;
			bool saw_star = false;
			bool saw_builtin_type = false, saw_typedef_or_sue = false;
			for (Token *s = param_start; s && s != param_end && s->kind != TK_EOF;
			     s = tok_next(s)) {
				if (s == last_ident) break;
				if (match_ch(s, '*')) saw_star = true;
				if (s->tag & (TT_SUE | TT_TYPEOF | TT_BITINT)) saw_typedef_or_sue = true;
				else if ((s->tag & TT_TYPE) && !is_known_typedef(s))
					saw_builtin_type = true;
				else if (is_known_typedef(s))
					saw_typedef_or_sue = true;
				if ((s->tag & (TT_QUALIFIER | TT_VOLATILE)) == (TT_QUALIFIER | TT_VOLATILE))
					has_vol_qual = true;
				if (s->len == 7 && s->ch0 == '_' && prism_memeq_static(tok_loc(s), "_Atomic", 7))
					has_atomic_qual = true;
				if (is_valid_varname(s) &&
				    !(s->tag & (TT_QUALIFIER | TT_TYPE | TT_SUE | TT_TYPEOF | TT_ATTR))) {
					unsigned tf = typedef_flags(s);
					if (tf & TDF_VOLATILE) has_vol_qual = true;
					if (tf & TDF_HAS_VOL_MEMBER) has_vol_member = true;
					if (tf & TDF_ATOMIC) has_atomic_qual = true;
				}
			}
			bool is_vol_param = (has_vol_qual || has_vol_member) && !saw_star;
			bool is_atomic_param = has_atomic_qual && !saw_star;
			/* The is_param shadow is only needed for ARRAY params (sizeof
			 * decays to a pointer; bounds-check must skip them) and volatile
			 * params. Skip it — avoiding one shadow per function, which makes
			 * typedef_lookup O(n^2) on shared names — for:
			 *  - pointer params (`S *s`, `int *p`): a pointer is never an array,
			 *    so no decay; this is the common real-code case (`Foo *self`).
			 *  - plain built-in scalars (`int x`).
			 * Kept (could hide array-ness): non-pointer typedef/SUE params
			 * (array typedef), non-pointer params with no type (K&R lists),
			 * bracketed params, volatile/atomic params.
			 * Also: a pointer/scalar param whose name hides a file-scope
			 * tracked array (`int g[10]; void f(int *g)`) must still get
			 * is_param so bounds-check does not wrap against the outer. */
			bool hides_outer_array = false;
			{
				BoundsArrayEntry *outer = bounds_array_lookup(last_ident);
				if (outer && !outer->is_param) hides_outer_array = true;
			}
			bool param_needs_shadow =
			    param_has_bracket || is_vol_param || is_atomic_param || hides_outer_array ||
			    (!saw_star && (saw_typedef_or_sue || !saw_builtin_type));
			bool param_plain_scalar = !param_needs_shadow;
			bool matched_ident = false;
			for (int ix = typedef_get_index(tok_loc(last_ident), last_ident->len); ix >= 0;
			     ix = typedef_table.entries[ix].prev_index) {
				TypedefEntry *ee = &typedef_table.entries[ix];
				if (ee->token_index == tok_idx(last_ident)) {
					ee->is_param = true;
					ee->is_array = false;
					matched_ident = true;
					break;
				}
			}
			if (!matched_ident && !param_plain_scalar) {
				TD_SCOPE_SAVE();
				if (scope_id > 0 && scope_id < scope_tree_count) {
					td_scope_open = scope_tree[scope_id].open_tok_idx;
					td_scope_close = scope_tree[scope_id].close_tok_idx;
				}
				int pre_ct = typedef_table.count;
				TYPEDEF_ADD_IDX(
				    typedef_add_shadow(tok_loc(last_ident), last_ident->len, brace_depth),
				    last_ident);
				TD_SCOPE_RESTORE();
				if (typedef_table.count > pre_ct) {
					typedef_table.entries[typedef_table.count - 1].is_param = true;
					typedef_table.entries[typedef_table.count - 1].is_array = false;
					if (is_vol_param) {
						typedef_table.entries[typedef_table.count - 1].is_volatile =
						    has_vol_qual;
						typedef_table.entries[typedef_table.count - 1]
						    .has_volatile_member = has_vol_member;
					}
					if (is_atomic_param)
						typedef_table.entries[typedef_table.count - 1].is_atomic =
						    true;
				}
			} else if (is_vol_param || is_atomic_param) {
				for (int ix = typedef_get_index(tok_loc(last_ident), last_ident->len);
				     ix >= 0;
				     ix = typedef_table.entries[ix].prev_index) {
					TypedefEntry *ee = &typedef_table.entries[ix];
					if (ee->token_index == tok_idx(last_ident)) {
						if (is_vol_param) {
							if (has_vol_qual) ee->is_volatile = true;
							if (has_vol_member) ee->has_volatile_member = true;
						}
						if (is_atomic_param) ee->is_atomic = true;
						break;
					}
				}
			}
		}
		if (t && match_ch(t, ',')) t = tok_next(t);
	}
}

static void p1_register_knr_param_vlas(Token *rparen, Token *lbrace, uint16_t sid, int brace_depth) {
	if (!rparen || !lbrace || sid == 0 || brace_depth <= 0 || sid >= scope_tree_count) return;
	if (!match_ch(rparen, ')') || !tok_match(rparen)) return;
	Token *id_list_open = tok_match(rparen);
	if (!id_list_open || !is_knr_params(tok_next(id_list_open), lbrace)) return;
	TD_SCOPE_SAVE();
	td_scope_open = scope_tree[sid].open_tok_idx;
	td_scope_close = scope_tree[sid].close_tok_idx;
	for (Token *stmt = tok_next(rparen); stmt && stmt != lbrace && stmt->kind != TK_EOF;) {
		stmt = skip_prep_dirs(stmt);
		stmt = skip_noise(stmt);
		if (!stmt || stmt == lbrace) break;
		if (!(stmt->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_BITINT | TT_STORAGE)) &&
		    !is_known_typedef(stmt)) {
			stmt = tok_next(stmt);
			continue;
		}
		TypeSpecResult ts = parse_type_specifier(stmt);
		if (!ts.saw_type) {
			stmt = tok_next(stmt);
			continue;
		}
		Token *semi = skip_to_semicolon(ts.end, NULL);
		if (!semi || semi->kind == TK_EOF || !match_ch(semi, ';')) break;
		for (Token *seg = ts.end; seg && seg != semi && seg != lbrace;) {
			DeclResult decl = parse_declarator(seg, false);
			if (!decl.var_name || !decl.end) break;
			bool skip_first = true;
			for (Token *s = seg; s && s != decl.end && s != semi && s->kind != TK_EOF;
			     s = tok_next(s)) {
				if (match_ch(s, '[') && (s->flags & TF_OPEN)) {
					if (skip_first) {
						skip_first = false;
						if (tok_match(s)) s = tok_match(s);
						continue;
					}
					if (array_size_is_vla(s)) {
						TYPEDEF_ADD_IDX(typedef_add_vla_var(tok_loc(decl.var_name),
										    decl.var_name->len,
										    brace_depth),
								decl.var_name);
						typedef_table.entries[typedef_table.count - 1].is_param =
						    true;
						break;
					}
				}
			}
			seg = decl.end;
			if (seg && match_ch(seg, ',')) seg = tok_next(seg);
			else
				break;
		}
		stmt = tok_next(semi);
	}
	TD_SCOPE_RESTORE();
}

// Phase 1D helper: check if array dimensions between start and end contain
// orelse (for-init runs before decl-probe classify, so classify on demand).
static void p1d_classify_bracket_orelse_ex(Token *tok, uint16_t cur_sid, int p1d_cur_func, bool hard_ctx,
					  bool allow_se_hoist);
static void p1d_classify_decl_dims(Token *start, Token *end, uint16_t cur_sid, int cur_func,
				  bool allow_se_hoist) {
	if (!FEAT(F_ORELSE)) return;
	for (Token *t = start; t && t != end;) {
		if (match_ch(t, '[') && tok_match(t) && !(t->flags & TF_C23_ATTR)) {
			if (!(tok_ann(t) & P1_OE_BRACKET))
				p1d_classify_bracket_orelse_ex(t,
							      cur_sid,
							      cur_func,
							      /*hard_ctx=*/true,
							      allow_se_hoist);
			t = tok_next(tok_match(t));
			continue;
		}
		/* Skip struct/enum bodies and stmt-exprs, but walk into `(...)`
		 * so dims inside typeof/_Atomic type-specifier parens are seen. */
		if (match_ch(t, '{') && tok_match(t)) {
			t = tok_next(tok_match(t));
			continue;
		}
		t = tok_next(t);
	}
}
static bool p1d_decl_has_bracket_orelse(Token *start, Token *end) {
	for (Token *t = start; t && t != end; t = tok_next(t)) {
		if (match_ch(t, '[') && tok_match(t) && !(t->flags & TF_C23_ATTR)) {
			if (!(tok_ann(t) & P1_OE_BRACKET))
				/* Annotate only — caller rejects the enclosing construct. */
				p1d_classify_bracket_orelse_ex(t, 0, -1, /*hard_ctx=*/false,
							      /*allow_se_hoist=*/true);
			if (tok_ann(t) & P1_OE_BRACKET) return true;
		}
	}
	return false;
}

static void p1_scan_init_shadows(Token *open,
				 Token *init_end,
				 uint32_t scope_close_idx,
				 uint16_t cur_sid,
				 int brace_depth,
				 uint16_t body_sid,
				 uint32_t body_close_idx,
				 bool is_for_init) {
	Token *init_tok = skip_noise(tok_next(open));
	bool saw_raw = false;
	while (init_tok && (init_tok->flags & TF_RAW) && !is_known_typedef(init_tok)) {
		Token *after_raw = skip_noise(tok_next(init_tok));
		/* `raw { … }` suppress block in for-init is never valid. */
		if (after_raw && match_ch(after_raw, '{')) {
			error_tok(after_raw,
				  "'raw { ... }' block is not allowed in for/if/switch initializer");
			return;
		}
		/* `for (raw = 0; …)` — identifier named raw, not the keyword. */
		if (!after_raw || !is_raw_declaration_context(init_tok, after_raw)) break;
		saw_raw = true;
		init_tok = after_raw;
	}
	if (saw_raw && init_tok && match_ch(init_tok, '{'))
		error_tok(init_tok, "'raw { ... }' block is not allowed in for/if/switch initializer");
	if (saw_raw && is_for_init && init_tok && init_end) {
		bool has_dim = false, has_eq = false;
		for (Token *t = init_tok; t && t != init_end && t->kind != TK_EOF; t = tok_next(t)) {
			if (match_ch(t, '[')) has_dim = true;
			if (match_ch(t, '=') && !(t->tag & TT_ASSIGN && t->len > 1)) has_eq = true;
			if ((t->flags & TF_OPEN) && tok_match(t)) {
				t = tok_match(t);
				continue;
			}
		}
		/* Allow `for (raw int arr[n];;)` (VLA suppress); reject scalar init. */
		if (has_eq && !has_dim)
			error_tok(init_tok,
				  "'raw' scalar declaration with initializer is not allowed in for-init");
	}
	if (!init_tok) return;
	ASSERT_NOT_NOISE(init_tok);
	// Skip __extension__/inline prefix — matches Phase 1D main loop
	while (init_tok && (init_tok->tag & TT_INLINE) && !(init_tok->tag & (TT_QUALIFIER | TT_TYPE)))
		init_tok = skip_noise(tok_next(init_tok));
	if (!init_tok) return;
	if (init_tok->tag & TT_TYPEDEF) {
		TD_SCOPE_SAVE();
		td_scope_open = tok_idx(open);
		td_scope_close = scope_close_idx;
		parse_typedef_declaration(init_tok, brace_depth);
		for (Token *tw = init_tok;
		     tw && tw != init_end && tw->kind != TK_EOF && !match_ch(tw, ';');) {
			if (is_enum_kw(tw)) {
				Token *brace = find_struct_body_brace(tw);
				if (brace) parse_enum_constants(brace, brace_depth);
			}
			if (tw->flags & TF_OPEN && tok_match(tw)) {
				tw = tok_next(tok_match(tw));
				continue;
			}
			tw = tok_next(tw);
		}
		TD_SCOPE_RESTORE();
		return;
	}
	if (!init_tok || !(init_tok->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_BITINT |
					    TT_INLINE | TT_STORAGE) ||
			   is_known_typedef(init_tok)))
		return;
	bool saw_static = init_tok->tag & TT_STORAGE;
	TD_SCOPE_SAVE();
	td_scope_open = tok_idx(open);
	td_scope_close = scope_close_idx;
	TypeSpecResult type = parse_type_specifier(init_tok);
	if (type.saw_type) {
		Token *ann_tok = init_tok;
		while (ann_tok && (ann_tok->tag & (TT_STORAGE | TT_INLINE)) &&
		       !(ann_tok->tag & (TT_QUALIFIER | TT_TYPE)))
			ann_tok = skip_noise(tok_next(ann_tok));
		tok_ann(ann_tok ? ann_tok : init_tok) |= P1_IS_DECL;
		Token *t = type.end;
		while (t && t != init_end && t->kind != TK_EOF) {
			bool decl_raw = saw_raw;
			t = p1_skip_decl_raw(t, &decl_raw);
			DeclResult decl = parse_declarator(t, false);
			if (!decl.var_name || !decl.end) break;
			// Phase 1D: reject bracket orelse in ctrl-paren declarations
			// conditions; moved from Pass 2 to satisfy the two-pass invariant)
			if (FEAT(F_ORELSE) && p1d_decl_has_bracket_orelse(t, decl.end))
				error_tok(t, ERR_BRACKET_OE_VLA_INIT_STMT);
			if (is_known_typedef(decl.var_name) || is_known_enum_const(decl.var_name) ||
			    (decl.var_name->tag & (TT_DEFER | TT_ORELSE | TT_NORETURN_FN | TT_SPECIAL_FN)) ||
			    (decl.var_name->flags & TF_RAW) ||
			    hashmap_get(&p1_func_proto_map, tok_loc(decl.var_name), decl.var_name->len))
				p1_register_shadow(decl.var_name, cur_sid, brace_depth);
			if (decl.is_func_decl) {
				TypedefEntry *e = p1_shadow_entry_for_token(decl.var_name);
				if (!e) {
					p1_register_shadow(decl.var_name, cur_sid, brace_depth);
					e = p1_shadow_entry_for_token(decl.var_name);
				}
				if (e) e->is_func = true;
			}
			bool is_vol_local = (type.has_volatile || type.has_volatile_member) &&
					    !decl.is_pointer && !decl.is_func_ptr;
			bool is_atomic_local = type.has_atomic && !decl.is_pointer && !decl.is_func_ptr;
			if (is_vol_local || is_atomic_local) {
				int pre_ct = typedef_table.count;
				p1_register_shadow(decl.var_name, cur_sid, brace_depth);
				if (typedef_table.count > pre_ct) {
					TypedefEntry *e = &typedef_table.entries[typedef_table.count - 1];
					if (type.has_volatile) e->is_volatile = true;
					if (type.has_volatile_member) e->has_volatile_member = true;
					if (type.has_atomic) e->is_atomic = true;
				}
			}
			if (type.is_struct && !type.is_enum && !decl.is_pointer && !decl.is_array &&
			    !decl.is_func_ptr && !decl.is_func_decl) {
				TypedefEntry *e = p1_shadow_entry_for_token(decl.var_name);
				if (!e) {
					p1_register_shadow(decl.var_name, cur_sid, brace_depth);
					e = p1_shadow_entry_for_token(decl.var_name);
				}
				if (e) e->is_aggregate = true;
			}

			// Phase 1D: register CFG entry for goto-skip-decl detection
			{
				bool has_init = match_ch(decl.end, '=');
				uint16_t eff_sid = body_sid > 0 ? body_sid : cur_sid;
				DeclShape shape = classify_decl_shape(init_tok, &type, &decl);
				p1_record_local_decl(eff_sid,
						     decl.var_name,
						     init_tok,
						     &shape,
						     &type,
						     &decl,
						     has_init,
						     decl_raw,
						     saw_static || type.has_static || type.has_extern,
						     body_sid > 0 ? 0 : body_close_idx);
				// Phase 1D: reject init-decl whose memset is unavoidable
				// typeof aggregates are accepted: Pass 2 emits `= {0}`
				if (FEAT(F_ZEROINIT) && !has_init && !decl_raw &&
				    !(saw_static || type.has_static || type.has_extern)) {
					bool eff_vla = shape.effective_vla;
					if (eff_vla && (!decl.is_pointer || decl.is_array) &&
					    !type.has_register && is_for_init)
						error_tok(decl.var_name, ERR_INIT_STMT_VLA);
					reject_register_agg_zeroinit(
					    decl.var_name, &shape, &type, init_tok, has_init, decl_raw);
					reject_const_unavoidable_memset(decl.var_name,
									 &shape,
									 &type,
									 init_tok,
									 &decl,
									 has_init,
									 decl_raw,
									 true);
				}
			}

			t = decl.end;
			if (match_ch(t, '=')) {
				t = tok_next(t);
				while (t && t != init_end && t->kind != TK_EOF) {
					if (t->flags & TF_OPEN) {
						t = tok_match(t) ? tok_next(tok_match(t)) : tok_next(t);
						continue;
					}
					if (match_ch(t, ',')) break;
					t = tok_next(t);
				}
			}
			if (t && match_ch(t, ',')) t = tok_next(t);
			else
				break;
		}
	}
	TD_SCOPE_RESTORE();
}

/* Shared for/if/switch-init Phase 1D body: open-paren → init `;` → body end →
 * shadows. */
static void
p1d_scan_ctrl_init(Token *tok, uint32_t *skip_cache, int brace_depth, uint16_t cur_sid, bool is_for) {
	Token *open = p1d_find_open_paren(tok);
	if (!open || !tok_match(open)) return;
	Token *close = tok_match(open);
	Token *init_end = find_init_semicolon(open, close);
	/* C23 if/switch: `if (int x = 1)` has no semicolon inside the parens —
	 * the declaration runs to the closing `)`. */
	if (!init_end && !is_for && close) init_end = close;
	if (!init_end) return;
	Token *body_start = skip_prep_dirs(tok_next(close));
	uint16_t body_sid = find_body_scope_id(body_start);
	uint32_t body_end_idx = tok_idx(close);
	if (is_for && body_start && match_ch(body_start, '{') && tok_match(body_start)) {
		/* C99 §6.8.5p3: for-init scope extends to the entire loop body. */
		body_end_idx = tok_idx(tok_match(body_start));
	} else {
		Token *stmt_end = skip_one_stmt_impl(body_start, skip_cache);
		if (stmt_end) body_end_idx = tok_idx(stmt_end);
		/* C23 §6.8.4.1: if-init scope extends through the else branch. */
		if (!is_for && stmt_end && (tok->tag & TT_IF)) {
			Token *n = skip_prep_dirs(tok_next(stmt_end));
			if (n && is_else_kw(n)) {
				Token *else_end = skip_one_stmt_impl(tok_next(n), skip_cache);
				if (else_end) body_end_idx = tok_idx(else_end);
			}
		}
	}
	p1_scan_init_shadows(
	    open, init_end, body_end_idx, cur_sid, brace_depth, body_sid, body_end_idx, is_for);
}

// Phase 1D: check if a declaration shadows an identifier captured by a
// satisfy the two-pass invariant (no semantic errors during emission).
static void __attribute__((noinline))
p1_check_defer_same_block_shadow(Token *var_name, uint16_t cur_sid, int p1d_cur_func) {
	if (!FEAT(F_DEFER) || p1d_cur_func < 0) return;
	char *name = tok_loc(var_name);
	int nlen = var_name->len;
	if (!hashmap_get(&func_meta[p1d_cur_func].defer_name_set, name, nlen)) return;
	int start = func_meta[p1d_cur_func].entry_start;
	for (int i = start; i < p1_entry_count; i++) {
		P1FuncEntry *e = &p1_entries[i];
		if (e->kind != P1K_DEFER || e->scope_id != cur_sid) continue;
		Token *body = tok_next(e->tok);
		if (!body) continue;
		Token *body_end = NULL;
		if (match_ch(body, '{') && tok_match(body)) body_end = tok_match(body);
		else
			body_end = skip_to_semicolon(body, NULL);
		uint32_t var_idx = tok_idx(var_name);
		uint32_t bi = tok_idx(body);
		uint32_t ei = body_end ? tok_idx(body_end) : UINT32_MAX;
		if (var_idx >= bi && var_idx < ei) continue;
		if (defer_body_has_capture(p1d_cur_func, body, name, nlen))
			error_tok(var_name, ERR_DEFER_SHADOW_SAME_SCOPE, nlen, name);
	}
}

static void p1_enum_shadow_cb(Token *t, void *ud) {
	int *p = ud;
	// Enclosing-scope enum shadows are handled by Pass 2:
	// be a false positive when the inner block has no return/goto.
	if (is_valid_varname(t)) p1_check_defer_same_block_shadow(t, (uint16_t)p[0], p[1]);
}

static void p1_check_enum_body_defer_shadow(Token *brace, uint16_t cur_sid, int p1d_cur_func) {
	if (!FEAT(F_DEFER) || p1d_cur_func < 0) return;
	int args[2] = {cur_sid, p1d_cur_func};
	for_each_enum_constant(brace, p1_enum_shadow_cb, args);
}

static bool p1_scope_in_raw_block(uint16_t sid);

static void p1_try_alloc_defer(Token *tok, uint16_t cur_sid, int func_idx) {
	/* raw { … } is a suppress region — leave defer as a soft keyword. */
	if (p1_scope_in_raw_block(cur_sid)) return;
	reject_defer_fn_body(tok, func_meta[func_idx].body_open->tag);
	p1_alloc(P1K_DEFER, cur_sid, tok);
}

// Phase 1F: validate defer body and populate name set.
static void p1_reject_defer_in_uneval_operand(Token *defer_tok, uint16_t sid) {
	for (uint16_t s = sid; s > 0 && s < scope_tree_count; s = scope_tree[s].parent_id) {
		if (!scope_tree[s].is_stmt_expr) continue;
		Token *brace = &token_pool[scope_tree[s].open_tok_idx];
		if (tok_idx(brace) == 0) break;
		Token *se_open = &token_pool[tok_idx(brace) - 1]; /* '(' of ({ */
		if (!match_ch(se_open, '(')) break;
		Token *intro = tok_walk_back(tok_idx(se_open), WB_ATTR_NOISE);
		/* Peel redundant paren wrappers: sizeof((({…}))). */
		while (intro && match_ch(intro, '(') && (intro->flags & TF_OPEN) && tok_match(intro)) {
			Token *outer_close = tok_match(intro);
			Token *se_close_brace = &token_pool[scope_tree[s].close_tok_idx];
			Token *se_paren_close = tok_next(se_close_brace);
			if (!outer_close || !se_paren_close ||
			    tok_idx(outer_close) < tok_idx(se_paren_close))
				break;
			intro = tok_walk_back(tok_idx(intro), WB_ATTR_NOISE);
		}
		if (intro && (is_uneval_operand_intro(intro) || (intro->flags & TF_SIZEOF) ||
			      (intro->tag & (TT_TYPEOF | TT_GENERIC))))
			error_tok(defer_tok,
				  "'defer' inside an unevaluated operand "
				  "(sizeof/_Alignof/typeof/_Generic) has no effect");
		break;
	}
}

static void __attribute__((noinline))
p1d_validate_defer(Token *tok, int p1d_cur_func, bool p1d_ctrl_pending, uint16_t cur_sid, int brace_depth) {
	/* File-scope defer: no scope to unwind. Struct/initializer bodies are
	 * exempt — a member or initializer field spelled `defer` never reaches
	 * Pass 2's defer machinery and passes through to the backend. */
	if (p1d_cur_func < 0 &&
	    !(cur_sid > 0 && cur_sid < scope_tree_count &&
	      (scope_tree[cur_sid].is_struct || scope_tree[cur_sid].is_init)))
		error_tok(tok, "defer outside of any scope");
	// Context validation (moved from Pass 2 handle_defer_keyword)
	if (p1d_cur_func >= 0) {
		reject_defer_context(tok,
				     false,
				     p1d_ctrl_pending,
				     cur_sid < scope_tree_count && scope_tree[cur_sid].is_stmt_expr,
				     cur_sid < scope_tree_count && scope_tree[cur_sid].is_switch);
		p1_check_defer_stmt_expr_chain(tok, cur_sid);
		p1_reject_defer_in_uneval_operand(tok, cur_sid);
	}
	{
		Token *body = skip_noise(tok_next(tok));
		if (body && !match_ch(body, '{')) {
			Token *semi = skip_to_semicolon(body, NULL);
			reject_defer_unterminated(tok, body, semi);
			/* Braceless defer bodies are erased as a unit; any `orelse` in
			 * them leaks as a soft keyword into the backend. Require braces. */
			if (FEAT(F_ORELSE)) {
				int pd = 0;
				for (Token *s = body; s && s != semi && s->kind != TK_EOF; s = tok_next(s)) {
					if (s->flags & TF_OPEN) {
						pd++;
						continue;
					}
					if (s->flags & TF_CLOSE) {
						if (pd > 0) pd--;
						continue;
					}
					if (pd == 0 && is_orelse_kw_shadow(s))
						error_tok(s,
							  "'orelse' inside a braceless defer body is not "
							  "supported; wrap the defer body in braces: "
							  "`defer { ... }`");
				}
			}
			/* A declaration as a braceless defer body is rejected outright:
			 * initialized declarations mis-scan across brace/designator
			 * initializers (body-end walk vs initializer walk disagree,
			 * duplicating trailing statements into the defer paste — found
			 * by the insertion suite), and an uninitialized declaration's
			 * object dies at the end of the paste, so the construct has no
			 * meaning.  SPEC defer constraint 10 already requires braces
			 * for the orelse flavor; this extends it to every declaration
			 * body.  `defer { T x = ...; }` remains fully supported. */
			if ((body->tag &
			     (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_BITINT | TT_STORAGE)) ||
			    (body->flags & TF_RAW) || is_known_typedef(body)) {
				Token *ts2 = skip_noise(body);
				while (ts2 && (ts2->flags & TF_RAW) && !is_known_typedef(ts2))
					ts2 = skip_noise(tok_next(ts2));
				TypeSpecResult tr2 = ts2 ? parse_type_specifier(ts2)
							 : (TypeSpecResult){0};
					/* A declaration as a braceless defer body mis-scans: deferred-range
					 * emission re-parses the declarator against the live token stream
					 * and overshoots the body's ';', duplicating every following
					 * statement into the paste (a silent miscompile — the contexts/
					 * insertion fixed-point oracle caught it; the old accept-test only
					 * checked a substring and missed the duplication).  Reject every
					 * declaration body; `defer { T x = ...; }` is the supported
					 * spelling and lowers correctly. */
					if (tr2.saw_type)
						error_tok(tok,
							  "a declaration as a braceless defer body is not "
							  "supported (it mis-scans across the following "
							  "statements); wrap the defer body in braces: "
							  "`defer { ... }`");
			}
			/* Structural guard: a braceless defer body can never cross out
			 * of its enclosing paren/bracket group.  If the scan to `;`
			 * dips below depth 0 the `defer` sits in an expression position
			 * (e.g. `({ ... } defer )`) and consuming to the `;` would eat
			 * the enclosing group's close token.  Found by the insertion
			 * suite. */
			{
				int bd2 = 0;
				for (Token *s = body; s && s != semi && s->kind != TK_EOF;
				     s = tok_next(s)) {
					if (s->flags & TF_OPEN) bd2++;
					else if (s->flags & TF_CLOSE) {
						bd2--;
						if (bd2 < 0)
							error_tok(tok,
								  "stray 'defer' in an expression "
								  "position (body would cross the "
								  "enclosing ')' or ']')");
					}
				}
			}
			// Braceless defer body tokens never hit p1d_probe_declaration at
			// stmt_start; annotate P1_IS_DECL so Pass 2 zeroinit sees the decl.
			if (FEAT(F_ZEROINIT) && brace_depth > 0 &&
			    (body->tag &
			     (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_BITINT | TT_STORAGE))) {
				Token *ts = skip_noise(body);
				TypeSpecResult tr = parse_type_specifier(ts);
				if (tr.saw_type) tok_ann(ts) |= P1_IS_DECL;
			}
		}
	}
	validate_defer_statement(tok_next(tok), false, false, 0);
	if (p1d_cur_func >= 0) {
		Token *body = tok_next(tok);
		Token *body_end = NULL;
		if (body && match_ch(body, '{') && tok_match(body)) body_end = tok_match(body);
		else if (body)
			body_end = skip_to_semicolon(body, NULL);
		if (body_end)
			defer_body_populate_captures(body,
						     body_end,
						     &func_meta[p1d_cur_func].defer_name_set,
						     &func_meta[p1d_cur_func].defer_body_captures);
	}
}

// Phase 1G: reject orelse in VLA bracket dimensions of function prototype
// the parameter names do not exist.
static void p1d_scan_param_bracket_orelse(Token *open_paren, bool is_proto) {
	Token *close = tok_match(open_paren);
	if (!close) return;
	for (Token *t = tok_next(open_paren); t && t != close; t = tok_next(t)) {
		if (match_ch(t, '[') && tok_match(t) && !(t->flags & TF_C23_ATTR)) {
			Token *bc = tok_match(t);
			for (Token *s = tok_next(t); s && s != bc; s = tok_next(s)) {
				if ((s->flags & TF_OPEN) && tok_match(s)) {
					s = tok_match(s);
					continue;
				}
				if (is_orelse_kw_shadow(s))
					error_tok(s,
						  is_proto ? "'orelse' in array dimensions of a function "
							     "prototype is not allowed (prototype parameter "
							     "arrays are never allocated; the dimension is "
							     "not evaluated at runtime)"
							   : "'orelse' in array dimensions of a function "
							     "definition parameter is not allowed (the "
							     "ternary expansion would evaluate the "
							     "dimension twice — undefined behavior for "
							     "volatile expressions)");
				/* `defer` followed by an identifier or `{` inside a
				 * parameter array dimension can only be a stray defer
				 * statement (two juxtaposed identifiers are never a
				 * valid C expression) — Pass 2's statement handler
				 * would consume through the dimension's `]` and drop
				 * tokens.  A lone `defer` identifier (a variable named
				 * defer) remains legal.  Found by the contexts suite. */
				if (FEAT(F_DEFER) && (s->tag & TT_DEFER) && tok_next(s) &&
				    (is_identifier_like(tok_next(s)) || match_ch(tok_next(s), '{')))
					error_tok(s, ERR_DEFER_EXPR_CTX);
			}
			t = bc;
			continue;
		}
		if ((t->flags & TF_OPEN) && match_ch(t, '(') && tok_match(t)) {
			p1d_scan_param_bracket_orelse(t, is_proto);
			t = tok_match(t);
			continue;
		}
		if ((t->flags & TF_OPEN) && tok_match(t)) {
			t = tok_match(t);
			continue;
		}
	}
}

static void p1d_reject_proto_param_orelse(Token *open_paren) {
	Token *close = tok_match(open_paren);
	if (!close) return;
	// skip_noise handles TT_ATTR but not TT_ASM.
	Token *after = skip_asm_specifier_trail(tok_next(close));
	if (!after || !(match_ch(after, ';') || match_ch(after, '{'))) return;
	bool is_proto = match_ch(after, ';');
	p1d_scan_param_bracket_orelse(open_paren, is_proto);
}

// Phase 1G: classify bracket orelse inside [...] array dimensions.
// hard_ctx: reject file-scope / struct / over-paren wraps.
// allow_se_hoist: local VLA decls may hoist temps for side-effect LHS;
//   false for skipped paren groups (cast/sizeof/params) — reject SE instead.
/* True when `[dim]` is part of a compound-literal type name: `(T[…]){`. */
static bool bracket_in_compound_literal_type(Token *open_bracket) {
	Token *close = tok_match(open_bracket);
	if (!close) return false;
	Token *t = tok_next(close);
	while (t && t->kind != TK_EOF) {
		if (match_ch(t, '[') && tok_match(t) && !(t->flags & TF_C23_ATTR)) {
			t = tok_next(tok_match(t));
			continue;
		}
		if ((t->flags & TF_C23_ATTR) && tok_match(t)) {
			t = tok_next(tok_match(t));
			continue;
		}
		break;
	}
	if (!t || !match_ch(t, ')')) return false;
	Token *after = skip_noise(tok_next(t));
	return after && match_ch(after, '{');
}

/* True when `[…]` is a designated-initializer index (not an expression
 * subscript in a value). Designator `[` follows `{` / `,` / `.name` / prior
 * designator `]`; value subscripts follow an expression-ending name. */
static bool bracket_in_offsetof_member(Token *open_bracket) {
	int depth = 0;
	for (uint32_t i = tok_idx(open_bracket); i > 0; i--) {
		Token *t = &token_pool[i - 1];
		if (t->kind == TK_PREP_DIR) continue;
		if (t->flags & TF_CLOSE) {
			depth++;
			continue;
		}
		if (t->flags & TF_OPEN) {
			if (depth == 0 && match_ch(t, '(')) {
				Token *before = tok_walk_back(tok_idx(t), WB_ATTR_NOISE);
				if (!before) return false;
				/* Both carry TF_SIZEOF — match lexeme so sizeof/alignof
				 * are not treated as offsetof member designators. */
				if ((before->kind == TK_IDENT || before->kind == TK_KEYWORD) &&
				    ((before->len == 8 &&
				      prism_memeq_static(tok_loc(before), "offsetof", 8)) ||
				     (before->len == 18 &&
				      prism_memeq_static(tok_loc(before), "__builtin_offsetof", 18))))
					return true;
				return false;
			}
			if (depth > 0) depth--;
			continue;
		}
		if (depth == 0 && (match_ch(t, ';') || match_ch(t, '{') || match_ch(t, '}'))) break;
	}
	return false;
}

/* `.a[…]` and `.outer.inner[…]` are initializer designators only when the
 * member chain has a leading dot.  A value expression such as `s.a[i]` or
 * `p->a[i]` has a base expression and must not inherit the designator ICE
 * restriction. */
static bool bracket_has_leading_member_designator(Token *member_name) {
	Token *name = member_name;
	while (name && is_identifier_like(name)) {
		Token *member = tok_walk_back(tok_idx(name), WB_PAST_NOISE);
		if (!member || !(member->tag & TT_MEMBER) || !match_ch(member, '.')) return false;
		Token *left = tok_walk_back(tok_idx(member), WB_PAST_NOISE);
		if (!left || match_ch(left, '{') || match_ch(left, ',')) return true;
		if (!is_identifier_like(left)) return false;
		name = left;
	}
	return false;
}

static bool bracket_is_designator_index(Token *open_bracket) {
	Token *prev = tok_walk_back(tok_idx(open_bracket), WB_PAST_NOISE);
	if (!prev) return false;
	if (match_ch(prev, '{') || match_ch(prev, ',')) return true;
	if (match_ch(prev, ']')) return true; /* [1][2] or .a[1][2] */
	if (is_identifier_like(prev)) {
		Token *before = tok_walk_back(tok_idx(prev), WB_PAST_NOISE);
		if (before && (before->tag & TT_MEMBER))
			return bracket_has_leading_member_designator(prev) ||
			       bracket_in_offsetof_member(open_bracket);
		/* offsetof(T, field[…]) / __builtin_offsetof(T, field[…]) */
		if (before && match_ch(before, ',')) return bracket_in_offsetof_member(open_bracket);
	}
	return false;
}

/* GNU `[first ... last]` range designator — orelse ternary would destroy
 * the `...` syntax (`[0 ... 2 orelse 3]` → `[(0...2)?(0...2):(3)]`). */
static bool bracket_contains_gnu_range(Token *open_bracket) {
	Token *close = tok_match(open_bracket);
	if (!close) return false;
	for (Token *t = tok_next(open_bracket); t && t != close; t = tok_next(t)) {
		if (t->kind == TK_PUNCT && t->len == 3 && t->ch0 == '.' &&
		    tok_loc(t)[1] == '.' && tok_loc(t)[2] == '.')
			return true;
		if ((t->flags & TF_OPEN) && tok_match(t)) {
			t = tok_match(t);
			continue;
		}
	}
	return false;
}

/* True when `[…]` sits in an `_Alignof`/`alignof` type operand — those
 * require a complete non-VLA type, so non-ICE dim orelse is unsound.
 * Walk through type-specifier ctors (`_Atomic(…)`, `typeof(…)`, `_BitInt(…)`)
 * so `_Alignof(_Atomic(int[n orelse 1]))` still rejects. */
static bool bracket_in_alignof_type_operand(Token *open_bracket) {
	int depth = 0;
	for (uint32_t i = tok_idx(open_bracket); i > 0; i--) {
		Token *t = &token_pool[i - 1];
		if (t->kind == TK_PREP_DIR) continue;
		if (t->flags & TF_CLOSE) {
			depth++;
			continue;
		}
		if (t->flags & TF_OPEN) {
			if (depth == 0 && match_ch(t, '(')) {
				Token *before = tok_walk_back(tok_idx(t), WB_ATTR_NOISE);
				if (!before) return false;
				/* `_Alignof` is TK_KEYWORD; soft `alignof` is TK_IDENT. */
				if ((before->kind == TK_IDENT || before->kind == TK_KEYWORD) &&
				    ((before->len == 8 &&
				      prism_memeq_static(tok_loc(before), "_Alignof", 8)) ||
				     (before->len == 7 &&
				      prism_memeq_static(tok_loc(before), "alignof", 7))))
					return true;
				/* Peel type-specifier constructors and keep looking. */
				if ((before->tag & (TT_TYPEOF | TT_BITINT | TT_ALIGNAS)) ||
				    ((before->tag & TT_TYPE) && equal(before, "_Atomic")))
					continue;
				return false;
			}
			if (depth > 0) depth--;
			continue;
		}
		if (depth == 0 && (match_ch(t, ';') || match_ch(t, '{') || match_ch(t, '}'))) break;
	}
	return false;
}

/* `_Generic(…, T[n orelse 1]: val, …)` — association types need complete
 * non-VLA types (same ICE rule as designator indices / alignof dims). */
static bool bracket_in_generic_association_type(Token *open_bracket) {
	Token *gen_open = NULL;
	for (uint32_t i = tok_idx(open_bracket); i > 0; i--) {
		Token *t = &token_pool[i - 1];
		if (t->kind == TK_PREP_DIR) continue;
		if (!(t->flags & TF_OPEN) || !match_ch(t, '(') || !tok_match(t) ||
		    tok_idx(tok_match(t)) <= tok_idx(open_bracket))
			continue; /* not an ancestor group */
		Token *before = tok_walk_back(tok_idx(t), WB_ATTR_NOISE);
		if (before && (before->tag & TT_GENERIC)) {
			gen_open = t;
			break; /* innermost containing _Generic owns the association */
		}
	}
	if (!gen_open || !tok_match(gen_open)) return false;
	Token *gen_close = tok_match(gen_open);
	int d = 0, commas = 0;
	for (Token *t = tok_next(gen_open); t && t != open_bracket; t = tok_next(t)) {
		if (t->flags & TF_OPEN) {
			d++;
			continue;
		}
		if (t->flags & TF_CLOSE) {
			if (d > 0) d--;
			continue;
		}
		if (d == 0 && match_ch(t, ',')) commas++;
	}
	if (commas < 1) return false; /* still in controlling expression */
	d = 0;
	for (Token *t = open_bracket; t && t != gen_close; t = tok_next(t)) {
		if (t->flags & TF_OPEN) {
			d++;
			continue;
		}
		if (t->flags & TF_CLOSE) {
			if (d > 0) d--;
			continue;
		}
		if (d == 0 && match_ch(t, ':')) return true;
		if (d == 0 && match_ch(t, ',')) return false;
	}
	return false;
}

/* Non-ICE dimension LHS (variable / call) — illegal for compound-literal types. */
static bool bracket_dim_lhs_nonconstant(Token *start, Token *end) {
	for (Token *s = start; s && s != end && s->kind != TK_EOF; s = tok_next(s)) {
		if (s->flags & TF_OPEN) {
			s = tok_match(s);
			continue;
		}
		if (s->kind != TK_IDENT) continue;
		if (is_type_keyword(s) || (s->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_ATTR |
							 TT_STORAGE | TT_ALIGNAS | TT_BITINT)))
			continue;
		if (typedef_lookup(s) || is_known_enum_const(s)) continue;
		return true;
	}
	return false;
}

static void __attribute__((noinline))
p1d_classify_bracket_orelse_ex(Token *tok, uint16_t cur_sid, int p1d_cur_func, bool hard_ctx,
			       bool allow_se_hoist) {
	Token *close = tok_match(tok);
	if (!(tok->flags & TF_HAS_PRISM)) return;
	/* Function / function-pointer / typedef-of-function parameter dims are
	 * never allocated VLAs (SPEC orelse constraint 11). Typedef walks
	 * classify dims via p1d_scan_balanced_group and used to lower
	 * `typedef int (*F)(int a[0 orelse 1])` to a ternary instead of
	 * rejecting — close that hole before annotation. */
	Token *param_open = FEAT(F_ORELSE) ? function_declarator_param_open(tok) : NULL;
	if (param_open) {
		bool is_proto = true;
		Token *pc = tok_match(param_open);
		Token *after = pc ? skip_asm_specifier_trail(tok_next(pc)) : NULL;
		is_proto = !after || !match_ch(after, '{');
		/* Walk every token — do not skip nested groups, or
		 * `int a[sizeof(0 orelse 1)]` in a prototype would lower. */
		Token *prev_s = tok;
		for (Token *s = tok_next(tok); s && s != close; s = tok_next(s)) {
			if (orelse_kw_at(s, prev_s) || orelse_after_type_in_parens(s, prev_s))
				error_tok(s,
					  is_proto ? "'orelse' in array dimensions of a function "
						     "prototype is not allowed (prototype parameter "
						     "arrays are never allocated; the dimension is "
						     "not evaluated at runtime)"
						   : "'orelse' in array dimensions of a function "
						     "definition parameter is not allowed (the "
						     "ternary expansion would evaluate the "
						     "dimension twice — undefined behavior for "
						     "volatile expressions)");
			prev_s = s;
		}
		return;
	}
	bool in_struct = cur_sid > 0 && cur_sid < scope_tree_count && scope_tree[cur_sid].is_struct;
	bool found_oe = false;
	Token *prev_d0_oe = NULL;
	int oe_depth = 0;
	Token *open_stack[64];
	Token *prev_bracket = tok;
	int paren_depth_scan = 0;
	int brace_depth_scan = 0;
	for (Token *s = tok_next(tok); s && s != close && s->kind != TK_EOF; s = tok_next(s)) {
		if (s->kind == TK_PREP_DIR) continue;
		/* Unevaluated / type operands nested in a dimension — `sizeof`,
		 * `_Alignof`, `typeof`, `_Generic`, `_Static_assert` — must not
		 * be treated as dimension-level orelse. Otherwise
		 * `int a[sizeof(0 orelse 1)]` mis-lowers to a bare ternary dim
		 * (sizeof is dropped) and `sizeof(int orelse 0)` leaks. */
		if (is_uneval_operand_intro(s) || (s->flags & TF_SIZEOF) || (s->tag & TT_GENERIC) ||
		    (s->flags & TF_STATIC_ASSERT)) {
			Token *lp = skip_noise(tok_next(s));
			if (lp && match_ch(lp, '(') && (lp->flags & TF_OPEN) && tok_match(lp)) {
				Token *rp = tok_match(lp);
				Token *prev_u = lp;
				for (Token *u = tok_next(lp); u && u != rp; u = tok_next(u)) {
					if (FEAT(F_ORELSE) &&
					    (orelse_kw_at(u, prev_u) || orelse_after_type_in_parens(u, prev_u)))
						error_tok(u,
							  "'orelse' cannot be used inside parentheses "
							  "(it must appear at the top level of a "
							  "declaration)");
					if ((u->flags & TF_OPEN) && tok_match(u)) {
						/* Still walk interiors so nested
						 * `sizeof(typeof(int orelse 0))` rejects. */
						prev_u = u;
						continue;
					}
					prev_u = u;
				}
				s = rp;
				prev_bracket = rp;
				continue;
			}
		}
		/* Type-junk in a dimension (`int orelse`, `_BitInt(N) orelse`) is
		 * not a keyword under orelse_kw_at_shadow, so without an explicit
		 * reject the token leaks to the C backend. */
		if (FEAT(F_ORELSE) && orelse_after_type_in_parens(s, prev_bracket))
			error_tok(s,
				  "'orelse' cannot be used after a type specifier in an "
				  "array dimension");
		/* `defer` statement shape at expression position in a dimension
		 * (not inside a `{…}` stmt-expr body). A variable named `defer`
		 * used as a primary (`arr[defer+1]`, `[defer]=…`) must pass;
		 * `defer printf(…)`, `defer 1`, `defer {…}` must reject. Pass 2's
		 * reject_defer_in_expr_context is DEBUG-only. */
		if (FEAT(F_DEFER) && brace_depth_scan == 0 && (s->tag & TT_DEFER) &&
		    !is_known_typedef(s) && !is_known_function_call(s) &&
		    !(prev_bracket && (prev_bracket->tag & TT_MEMBER))) {
			Token *nx = skip_noise(tok_next(s));
			/* Expression continuation after a primary → identifier use.
			 * Anything else after the keyword is a stray defer statement. */
			bool expr_primary =
			    nx && (match_ch(nx, ']') || match_ch(nx, ')') || match_ch(nx, ',') ||
				   match_ch(nx, ';') || match_ch(nx, ':') || match_ch(nx, '?') ||
				   match_ch(nx, '.') || (nx->tag & TT_MEMBER) || match_ch(nx, '(') ||
				   match_ch(nx, '[') ||
				   (nx->kind == TK_PUNCT && !match_ch(nx, '{') && !match_ch(nx, '}')));
			if (nx && !expr_primary) error_tok(s, ERR_DEFER_EXPR_CTX);
		}
		if (orelse_kw_at_shadow(s, prev_bracket)) {
			/* Over-paren is always untransformable (not hoist-related). */
			if (paren_depth_scan > 1)
				error_tok(s,
					  "'orelse' inside array dimension could not be transformed; "
					  "if wrapped in outer parentheses, remove them: "
					  "use '[f() orelse 1]' not '[(f() orelse 1)]'");
			if (hard_ctx) {
				if (p1d_cur_func < 0)
					error_tok(s,
						  "orelse inside array dimension at file scope is not allowed "
						  "(cannot hoist temporary variable outside a function body)");
				if (in_struct)
					error_tok(s,
						  "orelse inside array dimension in a struct/union body "
						  "cannot be transformed (statement expressions are not "
						  "allowed in struct/union definitions)");
			}
			validate_bracket_orelse(s);
			if (oe_depth == 0) {
				if (prev_d0_oe)
					reject_orelse_side_effects(tok_next(prev_d0_oe),
								   s,
								   "'orelse' in array dimension",
								   "in a chained 'orelse' (would be "
								   "evaluated twice); hoist the "
								   "expression to a variable first",
								   true,
								   true,
								   false);
				else if (!allow_se_hoist)
					reject_orelse_side_effects(tok_next(tok),
								   s,
								   "'orelse' in array dimension",
								   "in the LHS (would be evaluated twice); "
								   "hoist the expression to a variable first",
								   true,
								   true,
								   false);
				if (bracket_in_compound_literal_type(tok) &&
				    bracket_dim_lhs_nonconstant(tok_next(tok), s))
					error_tok(s,
						  "'orelse' in compound-literal array dimension with "
						  "non-constant LHS (compound literals cannot be VLAs); "
						  "use a constant dimension or a named array");
				/* Designator indices must be ICEs (C11 §6.7.9). Lowering
				 * `[idx orelse 1]` to a ternary would emit illegal C. */
				if (bracket_is_designator_index(tok) &&
				    bracket_dim_lhs_nonconstant(tok_next(tok), s))
					error_tok(s,
						  "'orelse' in a designated-initializer index requires an "
						  "integer constant expression on the left-hand side; "
						  "hoist a constant index or use a positional initializer");
				/* GNU range designators cannot be ternary-lowered. */
				if (bracket_is_designator_index(tok) && bracket_contains_gnu_range(tok))
					error_tok(s,
						  "'orelse' cannot be used in a GNU range designator "
						  "'[first ... last]' (ternary lowering would destroy "
						  "the range syntax)");
				/* `_Alignof(int[n orelse 1])` — alignof needs a non-VLA type. */
				if (bracket_in_alignof_type_operand(tok) &&
				    bracket_dim_lhs_nonconstant(tok_next(tok), s))
					error_tok(s,
						  "'orelse' in an array dimension inside "
						  "_Alignof/alignof requires an integer constant "
						  "expression on the left-hand side");
				/* `_Generic(…, int[n orelse 1]: …)` — association types too. */
				if (bracket_in_generic_association_type(tok) &&
				    bracket_dim_lhs_nonconstant(tok_next(tok), s))
					error_tok(s,
						  "'orelse' in an array dimension inside a "
						  "_Generic association type requires an integer "
						  "constant expression on the left-hand side");
				prev_d0_oe = s;
			} else {
				/* Nested orelse: check LHS of the innermost group only.
				 * For call args, include the callee so get_n(x orelse 5) rejects.
				 * Past the tracked depth, fall back to the whole dimension. */
				Token *grp = oe_depth <= 64 ? open_stack[oe_depth - 1] : NULL;
				Token *se_start = grp ? tok_next(grp) : tok_next(tok);
				if (grp && match_ch(grp, '(')) {
					Token *before = tok_walk_back(tok_idx(grp), WB_PAST_NOISE);
					if (before && is_valid_varname(before) && !is_type_keyword(before))
						se_start = before;
				}
				reject_orelse_side_effects(se_start,
							   s,
							   "'orelse' in array dimension",
							   "in the LHS (would be evaluated twice); "
							   "hoist the expression to a variable first",
							   true,
							   true,
							   false);
			}
			tok_ann(s) |= P1_OE_BRACKET | P1_IS_ORELSE_KW;
			found_oe = true;
		}
		if (s->flags & TF_OPEN) {
			if (oe_depth < 64) open_stack[oe_depth] = s;
			oe_depth++;
		}
		if (s->flags & TF_CLOSE) oe_depth--;
		if (match_ch(s, '(')) paren_depth_scan++;
		else if (match_ch(s, ')'))
			paren_depth_scan--;
		if (match_ch(s, '{')) brace_depth_scan++;
		else if (match_ch(s, '}'))
			brace_depth_scan--;
		prev_bracket = s;
	}
	if (found_oe) {
		Token *ppc = span_find_pp_conditional(tok_next(tok), close, NULL);
		if (ppc)
			error_tok(ppc,
				  "'orelse' inside array dimension cannot be used when the "
				  "dimension spans preprocessor conditionals — the "
				  "transpiler would emit tokens from all branches, "
				  "producing invalid C; "
				  "use 'cc -E' preprocessing or a temporary variable");
		tok_ann(tok) |= P1_OE_BRACKET;
	}
}

/* Annotate typeof(...) orelse for Pass 2; reject struct/file-scope when hard_ctx. */
static void p1d_annotate_typeof_orelse(Token *typeof_tok, uint16_t cur_sid, int p1d_cur_func, bool hard_ctx) {
	Token *paren = tok_next(typeof_tok);
	if (!paren || !match_ch(paren, '(') || !tok_match(paren)) return;
	const char *msg = NULL;
	if (hard_ctx) {
		if (cur_sid > 0 && cur_sid < scope_tree_count && scope_tree[cur_sid].is_struct)
			msg = "'orelse' inside typeof in a struct/union body "
			      "cannot be transformed; use the resolved type directly";
		else if (p1d_cur_func < 0)
			msg = "'orelse' inside typeof at file scope is not allowed";
	}
	if (msg) {
		for (Token *s = tok_next(paren); s && s != tok_match(paren); s = tok_next(s))
			if ((s->tag & TT_ORELSE) && !typedef_lookup(s)) error_tok(s, msg);
		return;
	}
	Token *prev_typeof = paren;
	Token *se_start = tok_next(paren);
	bool typeof_has_oe = false;
	for (Token *s = tok_next(paren); s && s != tok_match(paren); s = tok_next(s)) {
		/* Uneval intros nested in typeof — `typeof(sizeof(0 orelse 1))` —
		 * must reject like a bare `sizeof(… orelse …)`, not lower.
		 * Do NOT treat nested `typeof` as uneval here: expression
		 * `typeof(typeof(p orelse q))` is a supported transform. */
		if ((s->flags & TF_SIZEOF) || (s->tag & TT_GENERIC) || (s->flags & TF_STATIC_ASSERT) ||
		    (s->tag & TT_ALIGNAS) ||
		    (s->kind == TK_IDENT && s->len == 8 &&
		     prism_memeq_static(tok_loc(s), "_Alignof", 8)) ||
		    (s->kind == TK_IDENT && s->len == 7 && prism_memeq_static(tok_loc(s), "alignof", 7)) ||
		    (s->kind == TK_IDENT && s->len == 18 &&
		     prism_memeq_static(tok_loc(s), "__builtin_offsetof", 18))) {
			Token *lp = skip_noise(tok_next(s));
			if (lp && match_ch(lp, '(') && (lp->flags & TF_OPEN) && tok_match(lp)) {
				Token *rp = tok_match(lp);
				Token *prev_u = lp;
				for (Token *u = tok_next(lp); u && u != rp; u = tok_next(u)) {
					/* Type-name dims `sizeof(int[0 orelse 1])` /
					 * `_Alignof(int[…])` lower via ternary — do not
					 * treat them as expression-orelse rejects here. */
					if (match_ch(u, '[') && (u->flags & TF_OPEN) && tok_match(u) &&
					    !(u->flags & TF_C23_ATTR)) {
						u = tok_match(u);
						prev_u = u;
						continue;
					}
					if (orelse_kw_at(u, prev_u) || orelse_after_type_in_parens(u, prev_u))
						error_tok(u,
							  "'orelse' cannot be used inside parentheses "
							  "(it must appear at the top level of a "
							  "declaration)");
					if ((u->flags & TF_OPEN) && tok_match(u)) {
						prev_u = u;
						continue;
					}
					prev_u = u;
				}
				s = rp;
				prev_typeof = rp;
				continue;
			}
		}
		if (s->tag & TT_ORELSE) {
			/* `typeof(int orelse 0)` is type-junk, not an expression
			 * orelse — do not lower `int` into a ternary operand. */
			if (orelse_after_type_in_parens(s, prev_typeof)) {
				error_tok(s,
					  "'orelse' cannot be used inside parentheses "
					  "(it must appear at the top level of a declaration)");
				prev_typeof = s;
				continue;
			}
			if (orelse_kw_at(s, prev_typeof)) {
				tok_ann(s) |= P1_IS_ORELSE_KW;
				typeof_has_oe = true;
				reject_orelse_side_effects(se_start,
							   s,
							   "'orelse' in typeof",
							   "in the LHS (would be evaluated twice); "
							   "hoist the expression to a variable first",
							   true,
							   false,
							   false);
				se_start = tok_next(s);
				prev_typeof = s;
				continue;
			}
		}
		prev_typeof = s;
	}
	if (typeof_has_oe) tok_ann(paren) |= P1_OE_BRACKET;
}

static bool p1d_lhs_is_const_shadow(Token *start, Token *eq_tok);

/* (void*)1 = ordinary prototype; (void*)2 = returns struct/union by value. */
#define P1_PROTO_FN ((void *)(intptr_t)1)
#define P1_PROTO_STRUCT_RET ((void *)(intptr_t)2)

static bool p1_decl_looks_like_struct_return(Token *fn_name) {
	bool saw_agg = false, saw_ptr = false, saw_enum = false;
	for (Token *t = tok_walk_back(tok_idx(fn_name), WB_PAST_NOISE); t;
	     t = tok_walk_back(tok_idx(t), WB_PAST_NOISE)) {
		if (match_ch(t, ';') || match_ch(t, '{') || match_ch(t, '}') || match_ch(t, ')')) break;
		if (match_ch(t, '*')) {
			saw_ptr = true;
			continue;
		}
		if (t->tag & TT_SUE) {
			if (is_enum_kw(t))
				saw_enum = true;
			else
				saw_agg = true;
			continue;
		}
		if (typedef_flags(t) & TDF_AGGREGATE) {
			saw_agg = true;
			continue;
		}
		if (t->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_TYPEOF | TT_ATTR)) continue;
		if (is_known_typedef(t)) continue;
		if (t->kind == TK_IDENT || t->kind == TK_PUNCT) break;
	}
	return saw_agg && !saw_ptr && !saw_enum;
}

static bool p1d_func_returns_struct_value(Token *name) {
	void *proto = hashmap_get(&p1_func_proto_map, tok_loc(name), name->len);
	if (proto == P1_PROTO_STRUCT_RET) return true;
	for (int fi = 0; fi < func_meta_count; fi++) {
		Token *fn = func_meta[fi].ret_type_end;
		if (!fn) {
			Token *bt = tok_walk_back(tok_idx(func_meta[fi].body_open) - 1, WB_SKIP_ATTRS);
			if (bt && is_valid_varname(bt) &&
			    !(bt->tag & (TT_ATTR | TT_TYPE | TT_QUALIFIER | TT_SUE)))
				fn = bt;
		}
		if (!fn || fn->len != name->len || !prism_memeq_runtime_sized(tok_loc(fn), tok_loc(name), name->len))
			continue;
		bool saw_agg = false, saw_ptr = false, saw_enum = false;
		for (Token *t = func_meta[fi].ret_type_start; t && t != func_meta[fi].ret_type_end;
		     t = tok_next(t)) {
			if (t->tag & TT_SUE) {
				if (is_enum_kw(t))
					saw_enum = true;
				else
					saw_agg = true;
			}
			if (typedef_flags(t) & TDF_AGGREGATE) saw_agg = true;
			if (match_ch(t, '*')) saw_ptr = true;
		}
		return saw_agg && !saw_ptr && !saw_enum;
	}
	return false;
}

/* True when [start, end) is a struct/union-value expression (not a pointer). */
static bool p1d_expr_is_struct_value(Token *start, Token *end) {
	if (!start || start == end) return false;
	Token *t = start;
	Token *lim = end;
	while (match_ch(t, '(') && tok_match(t) && tok_next(tok_match(t)) == lim) {
		lim = tok_match(t);
		t = tok_next(t);
		if (!t || t == lim) return false;
	}
	if (t->kind == TK_IDENT && tok_next(t) == lim) {
		TypedefEntry *te = typedef_lookup(t);
		return te && te->is_shadow && te->is_aggregate && !te->is_array;
	}
	if (t->kind == TK_IDENT) {
		Token *lp = tok_next(t);
		if (lp && match_ch(lp, '(') && tok_match(lp) && tok_next(tok_match(lp)) == lim)
			return p1d_func_returns_struct_value(t);
	}
	if (match_ch(t, '(')) {
		Token *inner = skip_noise(tok_next(t));
		if (!inner) return false;
		if (inner->tag & TT_SUE) return !is_enum_kw(inner);
		if (typedef_flags(inner) & TDF_AGGREGATE) return true;
	}
	return false;
}

static void p1d_reject_orelse_chain_after_ctrl(Token *oe_kw) {
	Token *act = tok_next(oe_kw);
	if (!act) return;
	Token *u;
	Token *sp;
	if (act->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) {
		u = tok_next(act);
		/* Skip optional label after break/continue/goto — but not a following
		 * `orelse` keyword (would misread `continue orelse x` as labeled). */
		if (!(act->tag & TT_RETURN) && u && is_identifier_like(u) && !orelse_kw_at_bare(u, act))
			u = tok_next(u);
		sp = act;
	} else if (match_ch(act, '{') && (act->flags & TF_OPEN) && tok_match(act)) {
		/* Block-form action: further `orelse` after `}` cannot continue the chain. */
		sp = tok_match(act);
		u = tok_next(sp);
	} else
		return;
	for (; u && u->kind != TK_EOF; u = tok_next(u)) {
		if (u->flags & TF_OPEN) {
			sp = tok_match(u);
			u = tok_match(u);
			continue;
		}
		if ((u->flags & TF_CLOSE) || match_ch(u, ';') || match_ch(u, ',')) break;
		if ((tok_ann(u) & P1_IS_ORELSE_KW) || orelse_kw_at_bare(u, sp))
			error_tok(u,
				  "'orelse' chain cannot continue after a "
				  "control-flow action (return/goto/break/continue/block)");
		sp = u;
	}
}

static bool orelse_next_is_empty_action(Token *nx) {
	if (!nx) return false;
	if (match_ch(nx, ';') || match_ch(nx, ',')) return true;
	/* Mid-chain `… orelse orelse …` is empty unless the second `orelse` is
	 * clearly an identifier use: `orelse()`, `orelse[i]`, `orelse.x`. */
	if (!is_orelse_kw_shadow(nx)) return false;
	Token *a = tok_next(nx);
	if (a && (match_ch(a, '(') || match_ch(a, '[') || (a->tag & TT_MEMBER))) return false;
	return true;
}

// Phase 1D: validate bare orelse in expression statements.
static void __attribute__((noinline)) p1d_validate_bare_orelse(Token *tok, Token *bare_oe) {
	Token *scan_start = tok;
	{
		Token *last_comma = last_depth0_comma(tok, bare_oe);
		if (last_comma) scan_start = tok_next(last_comma);
	}
	Token *eq_tok = find_depth0_assign_eq(scan_start, bare_oe);
	bool has_eq = eq_tok != NULL;
	if (tok == bare_oe) error_tok(tok, "expected expression before 'orelse'");
	/* `x = orelse fb;` — empty expression between `=` and orelse (Pass 2's
	 * debug shell caught this only in PRISM_DEBUG builds; the release path
	 * emitted an empty test).  Found by the insertion suite. */
	if (eq_tok && skip_noise(tok_next(eq_tok)) == bare_oe)
		error_tok(bare_oe, "expected expression before 'orelse'");
	Token *after_oe = tok_next(bare_oe);
	if (orelse_next_is_empty_action(after_oe))
		error_tok(after_oe, "expected statement after 'orelse'");
	if (!has_eq && is_orelse_value_fallback(after_oe))
		error_tok(after_oe,
			  "orelse fallback requires an assignment target "
			  "(use a declaration)");
	if (has_eq && eq_tok) {
		if (scan_start != eq_tok && match_ch(scan_start, '(') && tok_match(scan_start)) {
			Token *inner_first = tok_next(scan_start);
			Token *pclose = tok_match(scan_start);
			if (inner_first && pclose && tok_next(pclose) != eq_tok &&
			    (inner_first->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF) ||
			     is_known_typedef(inner_first)))
				error_tok(scan_start,
					  "bare orelse assignment target is a cast expression "
					  "(not a modifiable lvalue)");
		}
		if (p1d_lhs_is_const_shadow(scan_start, eq_tok))
			error_tok(eq_tok, "orelse fallback cannot reassign a const-qualified variable");
	}

	if (has_eq && eq_tok && is_orelse_value_fallback(after_oe)) {
		/* Same scan Pass 2 emit_bare_orelse_impl used to own: asm,
		 * chained `=`, indirect calls, ctrl-flow in stmt-expr LHS. */
		reject_orelse_side_effects(scan_start,
					   eq_tok,
					   "orelse fallback on assignment",
					   "in the target expression",
					   true,
					   false,
					   true);
		bool fb_has_cl = false;
		{
			int fd = 0;
			Token *prev_cl = NULL;
			for (Token *s = after_oe; s && s->kind != TK_EOF; s = tok_next(s)) {
				if (match_ch(s, '{') && (fd == 0 || (prev_cl && match_ch(prev_cl, ')')))) {
					fb_has_cl = true;
					break;
				}
				if (s->flags & TF_OPEN) fd++;
				else if (s->flags & TF_CLOSE) {
					/* Unmatched `}` / `)` left the fallback expression
					 * (brace-init close, block close). Do not scan the
					 * next function's `name(){` as a compound literal. */
					if (fd == 0) break;
					fd--;
				} else if (fd == 0 && (match_ch(s, ';') || match_ch(s, ',')))
					break;
				prev_cl = s;
			}
		}
		if (fb_has_cl)
			reject_orelse_side_effects(scan_start,
						   eq_tok,
						   "orelse compound-literal fallback on assignment",
						   "in the target expression (volatile double-write "
						   "with compound literal fallback); "
						   "use a temporary variable instead",
						   false,
						   true,
						   false);
	}

	// When LHS has indirection (*, [], ., ->), Pass 2 uses typeof(RHS)
	// when the result type is variably modified (C11 §6.7.2.4p2).
	if (has_eq && eq_tok && is_orelse_value_fallback(after_oe)) {
		if (bare_lhs_has_indirection(scan_start, eq_tok)) {
			Token *rhs_start = tok_next(eq_tok);
			if (!is_strictly_bare_call(rhs_start, bare_oe))
				reject_orelse_side_effects(rhs_start,
							   bare_oe,
							   "bare orelse with indirection in LHS",
							   "in the RHS expression (typeof(RHS) evaluates its "
							   "operand for variably-modified types per C11 "
							   "\xc2\xa7"
							   "6.7.2.4p2, causing double evaluation); "
							   "hoist to a variable first",
							   false,
							   false,
							   true);
			/* Ctrl-flow in RHS is covered by reject_orelse_side_effects. */
		}
	}

	if (has_eq) {
		Token *ppc = span_find_pp_conditional(scan_start, NULL, tok_is_semicolon);
		if (ppc) error_tok(bare_oe, ERR_BARE_ORELSE_SPANS_PP);
	}
	/* Struct-value RHS cannot be tested with `if (!(lhs = rhs))` (action
	 * form) or used as a scalar temp (value form) — reject both. */
	if (has_eq && eq_tok) {
		Token *rhs = tok_next(eq_tok);
		if (p1d_expr_is_struct_value(rhs, bare_oe) || p1d_expr_is_struct_value(scan_start, eq_tok))
			error_tok(bare_oe, ERR_ORELSE_STRUCT_VALUE);
	}
	tok_ann(bare_oe) |= P1_IS_ORELSE_KW;
	p1d_reject_orelse_chain_after_ctrl(bare_oe);
	/* Annotate further depth-0 bare chain members for Pass 2. */
	{
		Token *prev = bare_oe;
		int ternary = 0;
		for (Token *s = tok_next(bare_oe); s && s->kind != TK_EOF; s = tok_next(s)) {
			if (s->flags & TF_OPEN) {
				prev = tok_match(s);
				s = tok_match(s);
				continue;
			}
			if ((s->flags & TF_CLOSE) || match_ch(s, ';')) break;
			if (match_ch(s, '?')) {
				ternary++;
				prev = s;
				continue;
			}
			if (match_ch(s, ':') && ternary > 0) {
				ternary--;
				prev = s;
				continue;
			}
			if (ternary == 0 && orelse_kw_at_bare(s, prev)) {
				tok_ann(s) |= P1_IS_ORELSE_KW;
				/* Chain tail with empty fallback: `… orelse;` / `… orelse,`
				 * would lower to an empty expression. */
				Token *nx = tok_next(s);
				if (orelse_next_is_empty_action(nx))
					error_tok(nx, "expected statement after 'orelse'");
				p1d_reject_orelse_chain_after_ctrl(s);
			}
			prev = s;
		}
	}
}

static Token *p1d_find_stmt_expr_fallback(Token *start) {
	int depth = 0;
	for (Token *s = start; s && s->kind != TK_EOF; s = tok_next(s)) {
		if (depth == 0 && (match_ch(s, ';') || match_ch(s, ','))) break;
		if (is_stmt_expr_open(s)) return s;
		if (s->flags & TF_OPEN) depth++;
		else if ((s->flags & TF_CLOSE) && depth > 0)
			depth--;
	}
	return NULL;
}

static bool p1d_lhs_is_const_shadow(Token *start, Token *eq_tok) {
	Token *lhs_start = skip_noise(start);
	Token *lhs_end = eq_tok;
	while (lhs_start && match_ch(lhs_start, '(') && tok_match(lhs_start)) {
		Token *close = tok_match(lhs_start);
		if (skip_noise(tok_next(close)) != lhs_end) break;
		lhs_start = skip_noise(tok_next(lhs_start));
		lhs_end = close;
	}
	Token *name = skip_noise(lhs_start);
	if (!name || !is_valid_varname(name)) return false;
	if (skip_noise(tok_next(name)) != lhs_end) return false;
	TypedefEntry *e = typedef_lookup(name);
	return e && e->is_shadow && e->is_const;
}

// 0 = not orelse; 1 = skip (fn/label/typedef-as-name); 2 = annotated as
// decl-init orelse
static int p1d_try_annotate_init_orelse(Token *t,
					Token *prev,
					bool (*ending)(Token *),
					bool reject_ternary,
					int ternary_depth,
					bool *out_has_orelse,
					Token **out_first_orelse) {
	if (!(t->tag & TT_ORELSE)) return 0;
	if (is_known_function_call(t)) return 1;
	/* `.orelse` / `->orelse` — member name, not the operator. The following
	 * `orelse` (if any) is classified on the next iteration. */
	if (prev && (prev->tag & TT_MEMBER)) return 1;
	if (orelse_is_label_or_goto_target(t, prev)) return 1;
	TypedefEntry *te = typedef_lookup(t);
	if (te && !(prev && ending(prev))) return 1;
	/* Shadowed `orelse` after an expression-ending token that is not a
	 * keyword context (e.g. `sizeof orelse`, `x + orelse`) stays an
	 * identifier — do not bake P1_IS_ORELSE_KW. */
	if (te && te->is_shadow && !orelse_shadow_is_kw(prev)) return 1;
	/* `int x = int orelse 1` / `= _BitInt(8) orelse 1` — type-junk LHS. */
	if (orelse_after_type_in_parens(t, prev))
		error_tok(t,
			  "'orelse' cannot be used after a type specifier in a "
			  "declaration initializer");
	if (prev && !ending(prev)) error_tok(t, ERR_ORELSE_STMT_LEVEL);
	if (reject_ternary && ternary_depth > 0) error_tok(t, ERR_ORELSE_TERNARY);
	tok_ann(t) |= P1_OE_DECL_INIT | P1_IS_ORELSE_KW;
	if (!*out_first_orelse) *out_first_orelse = t;
	*out_has_orelse = true;
	return 2;
}

static Token *p1d_scan_init_orelse(Token *t, bool *out_has_orelse, Token **out_first_orelse) {
	Token *prev_init_tok = NULL;
	bool init_is_first = true;
	int init_td = 0;
	Token *eq = t; // chain predecessor of the first init token (for wrap-paren strip)
	t = tok_next(t); // skip '='
	while (t && !match_ch(t, ',') && !match_ch(t, ';') && t->kind != TK_EOF) {
		if (match_ch(t, '?')) {
			init_td++;
			init_is_first = false;
			prev_init_tok = t;
			t = tok_next(t);
			continue;
		}
		if (match_ch(t, ':') && init_td > 0) {
			init_td--;
			init_is_first = false;
			prev_init_tok = t;
			t = tok_next(t);
			continue;
		}
		// Phase 1G: mark orelse in decl initializer
		int oe_ann = p1d_try_annotate_init_orelse(t,
							  prev_init_tok,
							  is_expr_ending_brace,
							  true,
							  init_td,
							  out_has_orelse,
							  out_first_orelse);
		/* Empty expression before orelse in a declaration initializer
		 * (`T x = orelse fb;`): mirrors the bare-assignment
		 * "expected expression before 'orelse'" reject.  Only for
		 * oe_ann==2 (orelse as the OPERATOR); oe_ann==1 is a variable /
		 * typedef / label named `orelse` and must pass through.
		 * init_is_first is true only when nothing preceded the orelse.
		 * Found by the insertion suite. */
		if (oe_ann == 2 && init_is_first)
			error_tok(t, "expected expression before 'orelse'");
		if (oe_ann == 1) {
			/* Decl-init orelse spanning #if/#else/#endif would lower one
			 * arm into statements the other arm cannot re-balance
			 * (mirrors ERR_BARE_ORELSE_SPANS_PP for bare assignments;
			 * reachable in library mode only — CLI input is
			 * preprocessed).  Found by the contexts suite. */
			if (out_first_orelse && *out_first_orelse == t) {
				Token *ppc =
				    span_find_pp_conditional(tok_next(eq), t, NULL);
				if (ppc)
					error_tok(t,
						  "'orelse' in a declaration initializer spans "
						  "preprocessor conditionals; keep the orelse "
						  "within a single #if branch");
			}
			prev_init_tok = t;
			t = tok_next(t);
			init_is_first = false;
			continue;
		}
		/* Top-level decl-init orelse must be followed by an action or
		 * value (mirrors Pass 2 require_orelse_action: not ';' or ','). */
		if (oe_ann == 2) {
			Token *nx = tok_next(t);
			if (orelse_next_is_empty_action(nx))
				error_tok(nx, "expected statement after 'orelse'");
			if (out_first_orelse && *out_first_orelse == t) {
				Token *ppc =
				    span_find_pp_conditional(tok_next(eq), t, NULL);
				if (ppc)
					error_tok(t,
						  "'orelse' in a declaration initializer spans "
						  "preprocessor conditionals; keep the orelse "
						  "within a single #if branch");
			}
		}
		if (t->flags & TF_OPEN) {
			Token *m = tok_match(t);
			if (m && match_ch(t, '(') && !is_stmt_expr_open(t)) {
				Token *am = tok_next(m);
				if (init_is_first &&
				    (!am || match_ch(am, ',') || match_ch(am, ';') || am->kind == TK_EOF)) {
					Token *prev_inner = NULL;
					bool p1d_inner_d0_comma = false;
					for (Token *inner = tok_next(t); inner && inner != m;
					     inner = tok_next(inner)) {
						if (match_ch(inner, ',')) p1d_inner_d0_comma = true;
						if (p1d_try_annotate_init_orelse(inner,
										 prev_inner,
										 is_expr_ending,
										 false,
										 0,
										 out_has_orelse,
										 out_first_orelse) == 1) {
							prev_inner = inner;
							continue;
						}
						if (inner->flags & TF_OPEN) {
							if ((FEAT(F_ORELSE) || FEAT(F_DEFER)) &&
							    match_ch(inner, '(') &&
							    !(prev_inner && (prev_inner->tag & TT_TYPEOF)))
								check_orelse_in_parens(inner);
							inner = tok_match(inner);
							prev_inner = inner;
							continue;
						}
						prev_inner = inner;
					}
					if (p1d_inner_d0_comma && *out_has_orelse) {
						for (Token *u = tok_next(t); u && u != m; u = tok_next(u)) {
							tok_ann(u) &=
							    (uint16_t)~(P1_OE_DECL_INIT | P1_IS_ORELSE_KW);
							if (u->flags & TF_OPEN && tok_match(u)) {
								u = tok_match(u);
								continue;
							}
						}
						*out_has_orelse = false;
						*out_first_orelse = NULL;
					} else if (*out_has_orelse) {
						/* Macro-hygiene parens wrapping the whole init:
						 * unlink `(` and `)` from the chain here, once —
						 * Pass 2 emits from the stripped stream and never
						 * mutates tokens. TF_OPEN is cleared so index-based
						 * walkers do not treat the orphan as a group. */
						eq->jump_idx = tok_idx(t) + 1;
						eq->flags |= TF_LINK_JUMP;
						t->flags &= ~TF_OPEN;
						Token *before_close = t;
						for (Token *u = tok_next(t); u && u != m;) {
							if (u->flags & TF_OPEN) {
								Token *um = tok_match(u);
								if (um) {
									before_close = um;
									u = tok_next(um);
									continue;
								}
							}
							before_close = u;
							u = tok_next(u);
						}
						before_close->jump_idx = tok_idx(m) + 1;
						before_close->flags |= TF_LINK_JUMP;
					}
				} else if ((FEAT(F_ORELSE) || FEAT(F_DEFER)) &&
					   !(prev_init_tok && (prev_init_tok->tag & TT_TYPEOF)))
					check_orelse_in_parens(t);
			}
			prev_init_tok = m ? m : t;
			t = m ? tok_next(m) : tok_next(t);
			init_is_first = false;
			continue;
		}
		prev_init_tok = t;
		t = tok_next(t);
		init_is_first = false;
	}
	return t;
}

static void p1d_validate_decl_orelse(Token *var_name,
				     Token *type_tok,
				     TypeSpecResult *type,
				     DeclResult *decl,
				     Token *first_orelse,
				     bool saw_static,
				     int brace_depth) {
	if (brace_depth == 0) error_tok(var_name, ERR_ORELSE_FILE_SCOPE);
	reject_decl_orelse_storage(var_name, first_orelse, type, saw_static);
	reject_decl_orelse_value_shape(var_name, type_tok, type, decl);
	if (first_orelse && (type->is_vla || decl->is_vla || type->type_vm)) {
		if (is_orelse_value_fallback(tok_next(first_orelse)) &&
		    has_effective_const_qual(type_tok, type, decl))
			error_tok(first_orelse, ERR_ORELSE_CONST_VM);
	}

	// Reject GNU statement expressions in orelse fallback values
	if (first_orelse) {
		if (is_orelse_value_fallback(tok_next(first_orelse))) {
			Token *se = p1d_find_stmt_expr_fallback(tok_next(first_orelse));
			if (se)
				error_tok(se,
					  "GNU statement expressions in orelse "
					  "fallback values are not supported; "
					  "use 'orelse { ... }' block form instead");
		}
		Token *prev = NULL;
		for (Token *s = first_orelse; s && s->kind != TK_EOF && !match_ch(s, ';') && !match_ch(s, ',');
		     s = tok_next(s)) {
			if (s->flags & TF_OPEN) {
				prev = tok_match(s);
				s = tok_match(s);
				continue;
			}
			if ((tok_ann(s) & P1_IS_ORELSE_KW) || (prev && orelse_kw_at_bare(s, prev)))
				p1d_reject_orelse_chain_after_ctrl(s);
			prev = s;
		}
	}
}

// break with anonymous structs or variably-modified type specifiers.
static void p1d_check_multi_decl_constraints(Token *t,
					     Token *type_tok,
					     TypeSpecResult *type,
					     bool any_would_memset,
					     bool vm_type,
					     bool current_decl_has_orelse) {
	Token *next_t = tok_next(t);
	bool nr = false;
	next_t = p1_skip_decl_raw(next_t, &nr);
	DeclResult nd = parse_declarator(next_t, false);
	if (!nd.var_name || !nd.end) return;
	bool split = (current_decl_has_orelse && FEAT(F_ORELSE)) ||
		     (any_would_memset && (match_ch(nd.end, '=') || nd.is_vla)) ||
		     (FEAT(F_ORELSE) && p1d_decl_has_bracket_orelse(next_t, nd.end));
	if (!split) return;
	if (type_spec_is_anon_sue(type_tok, type)) error_tok(next_t, ERR_BRACKET_OE_ANON_AGG);

	if (vm_type) error_tok(next_t, ERR_MULTIDECL_VM);
}

static Token *
p1d_scan_balanced_group(Token *tok, int brace_depth, int cur_func, uint16_t cur_sid, Token *prev_saved) {
	Token *group_end = tok_match(tok);
	Token *stmt_expr_open = NULL;
	Token *prev_inner = NULL;
	int se_depth = 0;
	Token *se_close_stack[64];
	int se_close_top = 0;
	/* Typedef / SUE walks call this on the dimension `[` itself. Classify
	 * that outer bracket as a declarator dim — otherwise
	 * `typedef int T[sizeof(int orelse 0)]` and `typedef int T[n orelse 1]`
	 * never hit p1d_classify_decl_dims and leak orelse. */
	if (FEAT(F_ORELSE) && match_ch(tok, '[') && tok_match(tok) && !(tok->flags & TF_C23_ATTR) &&
	    !(tok_ann(tok) & P1_OE_BRACKET))
		p1d_classify_bracket_orelse_ex(tok,
					      cur_sid,
					      cur_func,
					      /*hard_ctx=*/true,
					      /*allow_se_hoist=*/cur_func >= 0);
	for (Token *inner = tok_next(tok); inner && inner != group_end; inner = tok_next(inner)) {
		if (inner->flags & TF_OPEN) {
			if (FEAT(F_ORELSE) && match_ch(inner, '[') && tok_match(inner) &&
			    !(inner->flags & TF_C23_ATTR) && !(tok_ann(inner) & P1_OE_BRACKET))
				p1d_classify_bracket_orelse_ex(
				    inner, cur_sid, cur_func, cur_func >= 0, /*allow_se_hoist=*/false);
			if (is_stmt_expr_open(inner)) {
				se_depth++;
				Token *se_brace = skip_noise(tok_next(inner));
				Token *brace_close = se_brace ? tok_match(se_brace) : NULL;
				if (se_close_top < 64 && brace_close)
					se_close_stack[se_close_top++] = brace_close;
			}
		}
		if (FEAT(F_ORELSE) && (inner->tag & TT_TYPEOF))
			p1d_annotate_typeof_orelse(inner, cur_sid, cur_func, cur_func >= 0);
		if (inner->flags & TF_CLOSE) {
			if (se_close_top > 0 && inner == se_close_stack[se_close_top - 1]) {
				se_close_top--;
				se_depth--;
			}
			prev_inner = inner;
			continue;
		}
		if (is_enum_kw(inner)) {
			Token *brace = find_struct_body_brace(inner);
			if (brace) {
				parse_enum_constants(brace, brace_depth);
				p1_check_enum_body_defer_shadow(brace, cur_sid, cur_func);
			}
		}
		if (!stmt_expr_open && is_stmt_expr_open(inner)) stmt_expr_open = inner;
		if (se_depth == 0 && cur_func >= 0 && prev_saved &&
		    (prev_saved->tag & (TT_IF | TT_LOOP | TT_SWITCH)) && (inner->tag & TT_DEFER) &&
		    !typedef_lookup(inner) && !is_known_function_call(inner) &&
		    !(prev_inner && (prev_inner->tag & TT_MEMBER)))
			error_tok(inner, ERR_DEFER_CTRL_PAREN);
		if (se_depth == 0 && prev_saved && (prev_saved->tag & (TT_IF | TT_LOOP | TT_SWITCH)) &&
		    (inner->tag & TT_ORELSE) && !typedef_lookup(inner) &&
		    !(prev_inner && (prev_inner->tag & TT_MEMBER)) &&
		    !is_known_function_call(inner) && !orelse_is_label_or_goto_target(inner, prev_inner))
			error_tok(inner,
				  "'orelse' cannot be used inside control statement "
				  "condition parentheses");
		prev_inner = inner;
	}
	return stmt_expr_open;
}

static bool p1d_type_spec_has_nonempty_array_dims(Token *start, Token *end) {
	for (Token *t = start; t && t != end; t = tok_next(t)) {
		if (match_ch(t, '[') && (t->flags & TF_OPEN)) {
			Token *nx = tok_next(t);
			if (nx && !match_ch(nx, ']')) return true;
		}
	}
	return false;
}

// Records typedef shadows (Phase 1C) and per-function decl entries (Phase 1D).
// Read-only probe: does NOT advance the caller's token pointer.
static bool p1_scope_in_raw_block(uint16_t sid) {
	if (!p1_raw_block_count) return false;
	for (; sid != 0 && sid < scope_tree_count; sid = scope_tree[sid].parent_id) {
		uint32_t oi = scope_tree[sid].open_tok_idx;
		if (oi > 0 && oi < token_count && (token_pool[oi].ann & P1_RAW_BLOCK)) return true;
	}
	return false;
}

static void p1d_probe_declaration(Token *tok,
				  uint16_t cur_sid,
				  int brace_depth,
				  int cur_func,
				  bool *saw_raw,
				  bool saw_static,
				  bool ctrl_pending,
				  uint32_t *skip_cache) {
	ASSERT_NOT_NOISE(tok);
	if (!(tok->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_BITINT)) && !is_known_typedef(tok))
		return;
	if (p1_scope_in_raw_block(cur_sid)) *saw_raw = true;
	Token *type_tok = tok; // token to annotate with P1_IS_DECL
	TypeSpecResult type = parse_type_specifier(tok);
	// parse_type_specifier now skips embedded 'raw' and sets has_raw.
	if (type.has_raw) *saw_raw = true;
	/* Stray-defer closure inside declaration ranges: the main-loop rule
	 * cannot see declarator interiors (`int (* defer pa)[3]`), so scan the
	 * declaration here.  `defer` followed by an identifier or `{` is never
	 * valid inside a declaration (juxtaposed identifiers form neither a
	 * declarator nor an expression) unless `defer` names a typedef.
	 * Without this, Pass 2's statement handler consumes the declarator's
	 * tokens.  Found by the insertion suite. */
	if (type.saw_type && FEAT(F_DEFER) && cur_func >= 0) {
		/* Flag only inside pure paren/bracket nesting (declarator parens,
		 * argument lists, dimensions): statements cannot exist there, so
		 * `defer IDENT` is always a stray.  Inside any `{` group (statement
		 * expressions, initializer braces) defer statements can be
		 * legitimate and the block's own validation applies. */
		int po = 0, bo = 0;
		Token *sprev = NULL;
		for (Token *s = tok; s && s->kind != TK_EOF; s = tok_next(s)) {
			if (s->flags & TF_OPEN) {
				if (match_ch(s, '{')) bo++;
				else
					po++;
			} else if (s->flags & TF_CLOSE) {
				if (match_ch(s, '}')) {
					if (bo == 0) break;
					bo--;
				} else {
					if (po == 0 && bo == 0) break;
					if (po > 0) po--;
				}
			} else if (po == 0 && bo == 0 && (match_ch(s, ';') || match_ch(s, '{')))
				break;
			/* Stray `defer` inside a declaration: either nested in a
			 * declarator/argument/dimension paren (po>0), or at the
			 * declaration's top level but NOT its first token (`raw defer
			 * int u` — `defer` wedged into the declaration-specifier
			 * sequence).  Both mis-scan in Pass 2.  A leading `defer`
			 * (s==tok) is a real defer statement handled elsewhere; a
			 * `defer` typedef name, a `struct/union/enum defer` TAG name,
			 * or a `.defer`/`->defer` member name is a legitimate use. */
			if (bo == 0 && (po > 0 || s != tok) && (s->tag & TT_DEFER) &&
			    !is_known_typedef(s) && !(sprev && (sprev->tag & (TT_SUE | TT_MEMBER)))) {
				Token *nx = skip_noise(tok_next(s));
				if (nx && (is_identifier_like(nx) || match_ch(nx, '{')))
					error_tok(s, ERR_DEFER_EXPR_CTX);
			}
			if (s->kind != TK_PREP_DIR) sprev = s;
		}
	}
	if (!type.saw_type && type.end && (type.end->flags & TF_RAW) && !is_known_typedef(type.end)) {
		Token *after_raw = skip_noise(tok_next(type.end));
		if (after_raw && is_raw_declaration_context(type.end, after_raw)) {
			*saw_raw = true;
			type_tok = after_raw;
			type = parse_type_specifier(after_raw);
		}
	}
	if (!type.saw_type) return;
	bool annotated = false;
	// Phase 1D: bound braceless control-flow body declarations.
	// C23 labeled declarations (L: int x;) can serve as braceless
	uint32_t braceless_close_idx = 0;
	if (ctrl_pending) {
		Token *stmt_end = skip_one_stmt_impl(type_tok, skip_cache);
		if (stmt_end) braceless_close_idx = tok_idx(stmt_end);
	}
	// VLA variable entries don't leak past the statement boundary.
	// Without this, `if (c) int MyTypedef;` poisons the typedef name
	TD_SCOPE_SAVE();
	if (braceless_close_idx > 0) td_scope_close = braceless_close_idx;
	Token *t = type.end;
	bool vm_type = (type.has_typeof || type.has_atomic) && (type.is_vla || type.type_vm);
	bool any_would_memset = false;
	while (t && !match_ch(t, ';') && !match_ch(t, '{') && t->kind != TK_EOF) {
		bool decl_raw = *saw_raw;
		t = p1_skip_decl_raw(t, &decl_raw);
		DeclResult decl = parse_declarator(t, false);
		if (!decl.var_name || !decl.end) {
			// Detect GNU nested function definitions inside outer
			if (cur_func >= 0 && brace_depth > 0 && (FEAT(F_DEFER) || FEAT(F_ORELSE)) &&
			    decl.var_name && !saw_static) {
				Token *p = skip_noise(tok_next(decl.var_name));
				if (p && match_ch(p, '(') && tok_match(p)) {
					Token *a = tok_next(tok_match(p));
					while (a && (a->tag & (TT_ATTR | TT_ASM))) {
						a = (a->tag & TT_ASM) ? tok_next(a) : skip_noise(a);
						if (a && match_ch(a, '(') && tok_match(a))
							a = tok_next(tok_match(a));
					}
					bool nested = a && match_ch(a, '{');
					Token *body_open = nested ? a : NULL;
					if (!nested && a) {
						Token *b = a;
						while (b && b->kind != TK_EOF && !match_ch(b, '{') &&
						       !match_ch(b, '}'))
							b = (b->flags & TF_OPEN && tok_match(b))
								? tok_next(tok_match(b))
								: tok_next(b);
						nested = b && match_ch(b, '{') &&
							 is_knr_params(tok_next(tok_match(p)), b);
						if (nested) body_open = b;
					}
					if (nested) {
						bool outer_uses_defer = false;
						Token *fn_open = func_meta[cur_func].body_open;
						Token *fn_close = fn_open ? tok_match(fn_open) : NULL;
						if (fn_open && fn_close) {
							Token *prev_s = NULL;
							for (Token *s = tok_next(fn_open);
							     s && s != fn_close && s->kind != TK_EOF;
							     prev_s = s, s = tok_next(s)) {
								/* Pass prev so a nested function *named*
								 * defer / a param named defer is not
								 * mistaken for a defer statement. */
								if (is_defer_kw(s, prev_s)) {
									outer_uses_defer = true;
									break;
								}
							}
						}
						/* Nested bodies are passed through verbatim — orelse/
						 * defer inside them would misscompile at the backend. */
						bool nested_uses_prism = false;
						Token *nclose = body_open ? tok_match(body_open) : NULL;
						if (body_open && nclose) {
							Token *prev_n = NULL;
							for (Token *s = tok_next(body_open);
							     s && s != nclose && s->kind != TK_EOF;
							     prev_n = s, s = tok_next(s)) {
								if (is_defer_kw(s, prev_n) ||
								    (is_orelse_kw_shadow(s) &&
								     orelse_shadow_is_kw(prev_n))) {
									nested_uses_prism = true;
									break;
								}
							}
						}
						if (outer_uses_defer || nested_uses_prism)
							error_tok(decl.var_name,
								  "nested function definitions cannot use "
								  "defer/orelse (and are unsupported inside "
								  "functions using defer) — move the function "
								  "outside or use a function pointer");
					}
				}
			}
			break;
		}
		if (match_ch(decl.end, '(') && brace_depth == 0) break; // func def
		if (decl.end && !match_ch(decl.end, '=') && !match_ch(decl.end, ',') &&
		    !match_ch(decl.end, ';') && !match_ch(decl.end, '[') && !match_ch(decl.end, '(') &&
		    !match_ch(decl.end, '{') && !match_ch(decl.end, ')') && !match_ch(decl.end, ':'))
			break;
		// Phase 1D: annotate type-start token for Pass 2 fast gate
		if (!annotated && brace_depth > 0) {
			tok_ann(type_tok) |= P1_IS_DECL;
			annotated = true;
		}
		/* Array dims in the type specifier (typeof/_Atomic parens) and
		 * in the declarator — not expression subscripts. Without the
		 * type-spec walk, `_Atomic(int[n orelse 1])` never classifies
		 * the dimension and leaks orelse to the C backend.
		 * Type-spec dims lower via ternary (no temp hoist), so reject
		 * side-effect LHS; declarator dims may hoist when in a function. */
		p1d_classify_decl_dims(type_tok, type.end, cur_sid, cur_func, /*allow_se_hoist=*/false);
		p1d_classify_decl_dims(t, decl.end, cur_sid, cur_func,
				      /*allow_se_hoist=*/cur_func >= 0);
		/* Function / function-pointer parameter dims are never allocated
		 * VLAs — orelse must not hoist or leak there. */
		if (FEAT(F_ORELSE) && (decl.is_func_ptr || decl.is_func_decl)) {
			for (Token *b = t; b && b != decl.end && b->kind != TK_EOF; b = tok_next(b)) {
				if (match_ch(b, '[') && tok_match(b) && (tok_ann(b) & P1_OE_BRACKET)) {
					Token *bc = tok_match(b);
					for (Token *s = tok_next(b); s && s != bc; s = tok_next(s)) {
						if (tok_ann(s) & P1_IS_ORELSE_KW) {
							error_tok(s,
								  "'orelse' in array dimensions of a function "
								  "prototype is not allowed (prototype parameter "
								  "arrays are never allocated; the dimension is "
								  "not evaluated at runtime)");
							break;
						}
					}
				}
			}
		}
		/* static/extern/TLS/constexpr dims must be ICEs (C11 §6.7.6.2 /
		 * C23 constexpr). Bracket orelse lowering inserts a runtime
		 * temporary, turning even `static int a[0 orelse 1]` / 
		 * `constexpr int a[0 orelse 1]` into an illegal non-ICE dim. */
		if (FEAT(F_ORELSE) &&
		    (saw_static || type.has_static || type.has_extern || type.has_thread_local ||
		     type.has_constexpr)) {
			for (Token *b = type_tok; b && b != decl.end && b->kind != TK_EOF; b = tok_next(b)) {
				if (match_ch(b, '[') && tok_match(b) && (tok_ann(b) & P1_OE_BRACKET)) {
					Token *bc = tok_match(b);
					for (Token *s = tok_next(b); s && s != bc; s = tok_next(s)) {
						if (tok_ann(s) & P1_IS_ORELSE_KW) {
							error_tok(s,
								  "orelse inside array dimension of a "
								  "static/extern/_Thread_local/constexpr "
								  "declaration is not allowed (dimension "
								  "must be an integer constant expression; "
								  "orelse lowering introduces a runtime "
								  "temporary)");
							break;
						}
					}
				}
			}
		}

		// Phase 1D: reject unbraced declaration in switch body.
		{
			bool has_init = match_ch(decl.end, '=');
			bool has_explicit_intent =
			    has_init || decl_raw || saw_static || type.is_typedef || type.is_struct ||
			    type.is_enum || type.has_static || type.has_extern || type.has_thread_local ||
			    type.has_register || type.has_atomic || type.has_constexpr || type.has_alignas;
			if (FEAT(F_ZEROINIT) && brace_depth > 0 && !has_explicit_intent &&
			    braceless_close_idx == 0 && cur_sid < scope_tree_count &&
			    scope_tree[cur_sid].is_switch)
				SAFETY_DIAG(
				    type_tok,
				    "variable declaration directly in switch body without braces "
				    "(zero-init may be skipped by case labels); wrap in braces or use "
				    "'raw'");
		}

		// Phase 1C: shadow detection
		bool did_shadow = false;
		bool is_vol_local =
		    (type.has_volatile || type.has_volatile_member) && !decl.is_pointer && !decl.is_func_ptr;
		bool is_atomic_local = type.has_atomic && !decl.is_pointer && !decl.is_func_ptr;
		bool is_const_local = has_effective_const_qual(type_tok, &type, &decl);
		if (is_known_typedef(decl.var_name) || is_known_enum_const(decl.var_name) ||
		    (decl.var_name->tag & (TT_DEFER | TT_ORELSE | TT_NORETURN_FN | TT_SPECIAL_FN)) ||
		    (decl.var_name->flags & TF_RAW) ||
		    hashmap_get(&p1_func_proto_map, tok_loc(decl.var_name), decl.var_name->len) ||
		    is_vol_local || is_atomic_local || is_const_local || decl.is_func_decl) {
			p1_register_shadow(decl.var_name, cur_sid, brace_depth);
			did_shadow = true;
		}
		if (decl.is_func_decl) {
			TypedefEntry *e = p1_shadow_entry_for_token(decl.var_name);
			if (e) e->is_func = true;
		}
		if ((is_vol_local || is_atomic_local || is_const_local) && typedef_table.count > 0) {
			TypedefEntry *e = p1_shadow_entry_for_token(decl.var_name);
			if (e) {
				if (is_const_local) e->is_const = true;
				if (type.has_volatile) e->is_volatile = true;
				if (type.has_volatile_member) e->has_volatile_member = true;
				if (type.has_atomic) e->is_atomic = true;
			}
		}
		/* Track struct/union locals so bare `s = s orelse …` rejects like decl form. */
		if (brace_depth > 0 && type.is_struct && !type.is_enum && !decl.is_pointer &&
		    !decl.is_array && !decl.is_func_ptr && !decl.is_func_decl) {
			if (!did_shadow) {
				p1_register_shadow(decl.var_name, cur_sid, brace_depth);
				did_shadow = true;
			}
			TypedefEntry *e = p1_shadow_entry_for_token(decl.var_name);
			if (e) e->is_aggregate = true;
		}

		// -fbounds-check: register plain local array variables so Pass 2 can
		bool base_is_array_here = false;
		uint8_t base_array_rank_here = 0;
		if (FEAT(F_BOUNDS_CHECK) && !decl_raw && decl.var_name && !decl.is_pointer &&
		    !decl.is_func_ptr) {
			for (Token *bt = type_tok; bt && bt != type.end; bt = tok_next(bt))
				if (is_array_typedef(bt)) {
					if (!decl.is_array) base_is_array_here = true;
					base_array_rank_here = array_rank_for_tok(bt);
					break;
				}
			if (!base_is_array_here && (type.has_typeof || type.has_atomic) && type.is_array)
				base_is_array_here = true;
		}
		bool reg_as_array = decl.is_array && (!decl.paren_pointer || decl.paren_array);
		bool has_complete_dim = true;
		if (reg_as_array) {
			has_complete_dim = false;
			// ISO C: array type is complete only if the *outer* (first) `[]` has
			for (Token *dt = decl.var_name; dt && dt != decl.end; dt = tok_next(dt)) {
				if (match_ch(dt, '[')) {
					Token *nx = tok_next(dt);
					if (nx && !match_ch(nx, ']')) {
						has_complete_dim = true;
					}
					break;
				}
			}
			if (!has_complete_dim && match_ch(decl.end, '=')) has_complete_dim = true;
		}
		if (!reg_as_array && base_is_array_here) {
			has_complete_dim = false;
			for (Token *bt = type_tok; bt && bt != type.end; bt = tok_next(bt)) {
				if (is_array_typedef(bt)) {
					TypedefEntry *bte = typedef_lookup(bt);
					if (bte && bte->is_array && bte->array_dim_complete) {
						has_complete_dim = true;
						break;
					}
					BoundsArrayEntry *bbe = bounds_array_lookup(bt);
					if (bbe && bbe->array_dim_complete) has_complete_dim = true;
					break;
				}
			}
			if (!has_complete_dim && (type.has_typeof || type.has_atomic) && type.is_array &&
			    p1d_type_spec_has_nonempty_array_dims(type_tok, type.end))
				has_complete_dim = true;
		}
		if (FEAT(F_BOUNDS_CHECK) && !decl_raw && decl.var_name &&
		    (brace_depth > 0 || (brace_depth == 0 && (reg_as_array || base_is_array_here))) &&
		    has_complete_dim && (reg_as_array || base_is_array_here) && !decl.is_func_ptr) {
			int rank = 0;
			Token *prev_bt = NULL;
			for (Token *dt = decl.var_name; dt && dt != decl.end;) {
				if (match_ch(dt, '[') && (dt->flags & TF_OPEN)) {
					if (!array_bracket_closes_ptr_to_array(dt, prev_bt)) rank++;
					Token *m = tok_match(dt);
					dt = m ? tok_next(m) : tok_next(dt);
					prev_bt = m;
					continue;
				}
				prev_bt = dt;
				dt = tok_next(dt);
			}
			if (type.type_array_rank > 0) {
				rank += (int)type.type_array_rank;
			} else if (base_array_rank_here > 0) {
				rank += (int)base_array_rank_here;
			} else if (base_is_array_here) {
				rank += 1;
			}
			if (rank > 15) rank = ARRAY_RANK_WRAP_ALL;
			/* Keep typedef-shadow is_array when the name already shadowed a
			 * typedef/keyword; otherwise register only in the bounds table. */
			if (did_shadow) {
				TypedefEntry *e = p1_shadow_entry_for_token(decl.var_name);
				if (e) {
					e->is_array = true;
					e->array_rank = (uint8_t)rank;
					e->array_dim_complete = has_complete_dim;
				}
			}
			bounds_array_add(tok_loc(decl.var_name),
					 decl.var_name->len,
					 tok_idx(decl.var_name),
					 (uint8_t)rank,
					 has_complete_dim,
					 false,
					 false);
		}

		t = decl.end;
		/* Struct/union bitfield: declarator ends at `:`. Keep going so
		 * soft/Prism keyword field names (e.g. `int orelse : 3`) still
		 * get shadow registration above, then skip the width expr. */
		if (match_ch(t, ':')) {
			t = tok_next(t);
			while (t && t->kind != TK_EOF && !match_ch(t, ';')) {
				if (t->flags & TF_OPEN) {
					Token *m = tok_match(t);
					t = m ? tok_next(m) : tok_next(t);
					continue;
				}
				if (match_ch(t, ',') || match_ch(t, ';')) break;
				t = tok_next(t);
			}
		}
		bool has_init = match_ch(t, '=');
		bool is_actual_vla = type.is_vla || decl.is_vla;
		// is_vla_typedef() lookups during Pass 2.
		if (is_actual_vla && decl.var_name && brace_depth > 0) {
			/* Preserve array_rank from a prior shadow / bounds entry so
			 * VLA-of-pointers (`int *p[n]`) still refuse pointee subscripts. */
			uint8_t saved_rank = 0;
			bool saved_is_array = false;
			bool saved_complete = true;
			BoundsArrayEntry *bprev = bounds_array_entry_for_token(decl.var_name);
			if (bprev) {
				saved_rank = bprev->array_rank;
				saved_is_array = true;
				saved_complete = bprev->array_dim_complete;
			} else if (typedef_table.count > 0) {
				TypedefEntry *prev = &typedef_table.entries[typedef_table.count - 1];
				if (prev->is_shadow && prev->is_array && prev->len == decl.var_name->len &&
				    !strncmp(prev->name, tok_loc(decl.var_name), (size_t)prev->len)) {
					saved_rank = prev->array_rank;
					saved_is_array = true;
					saved_complete = prev->array_dim_complete;
				}
			}
			TYPEDEF_ADD_IDX(
			    typedef_add_vla_var(tok_loc(decl.var_name), decl.var_name->len, brace_depth),
			    decl.var_name);
			if (saved_is_array && typedef_table.count > 0) {
				TypedefEntry *ve = &typedef_table.entries[typedef_table.count - 1];
				ve->is_array = true;
				ve->array_rank = saved_rank;
				ve->array_dim_complete = saved_complete;
			}
			if (saved_is_array)
				bounds_array_add(tok_loc(decl.var_name),
						 decl.var_name->len,
						 tok_idx(decl.var_name),
						 saved_rank,
						 saved_complete,
						 true,
						 false);
		}

		// Phase 1D: record declaration entry.
		DeclShape shape = classify_decl_shape(type_tok, &type, &decl);
		bool in_aggregate_body =
		    cur_sid > 0 && cur_sid < scope_tree_count && scope_tree[cur_sid].is_struct;
		/* Locals inside a GNU nested function must not land on the outer
		 * function's CFG / cgoto×zeroinit gate — nested bodies are passed
		 * through; only the outer function's own decls matter. */
		bool in_nested_func = false;
		for (uint16_t s = cur_sid; s != 0 && s < scope_tree_count; s = scope_tree[s].parent_id) {
			if (scope_tree[s].is_func_body && scope_tree[s].parent_id != 0) {
				in_nested_func = true;
				break;
			}
		}
		P1FuncEntry *p1e = NULL;
		if (cur_func >= 0 && decl.var_name && brace_depth > 0 && !in_aggregate_body &&
		    !in_nested_func)
			p1e = p1_record_local_decl(cur_sid,
						   decl.var_name,
						   type_tok,
						   &shape,
						   &type,
						   &decl,
						   has_init,
						   decl_raw,
						   saw_static || type.has_static || type.has_extern,
						   braceless_close_idx);
		if (p1e && !ctrl_pending)
			p1_check_defer_same_block_shadow(decl.var_name, cur_sid, cur_func);

		// Phase 1D: reject register / const zeroinit shapes (locals only —
		// aggregate members are not automatic locals and must not false-reject).
		if (brace_depth > 0 && !in_aggregate_body)
			reject_register_agg_zeroinit(
			    decl.var_name, &shape, &type, type_tok, has_init, decl_raw);
		if (!in_aggregate_body)
			if (FEAT(F_ZEROINIT) && !has_init && !decl_raw && type.has_register && shape.effective_vla)
				error_tok(decl.var_name, ERR_REGISTER_VLA);
		if (!in_aggregate_body && !(saw_static || type.has_static || type.has_extern))
			reject_const_unavoidable_memset(
			    decl.var_name, &shape, &type, type_tok, &decl, has_init, decl_raw, false);

		bool decl_has_orelse = false;
		if (has_init) {
			Token *first_orelse = NULL;
			t = p1d_scan_init_orelse(t, &decl_has_orelse, &first_orelse);
			if (decl_has_orelse && FEAT(F_ORELSE))
				p1d_validate_decl_orelse(decl.var_name,
							 type_tok,
							 &type,
							 &decl,
							 first_orelse,
							 saw_static,
							 brace_depth);
		}

		// Phase 1D: track whether this declarator would need
		// typeof memset in Pass 2 (for split detection).
		if (p1e ? p1e->decl.zero_kind == P1Z_MEMSET
			: decl_shape_needs_memset(
			      &shape, &type, &decl, has_init, decl_raw, saw_static || type.has_static))
			any_would_memset = true;
		// Phase 1D: reject multi-declarator split constraints
		if (t && match_ch(t, ',') && brace_depth > 0)
			p1d_check_multi_decl_constraints(
			    t, type_tok, &type, any_would_memset, vm_type, decl_has_orelse);
		if (t && match_ch(t, ',')) {
			t = tok_next(t);
		} else
			break;
	}
	TD_SCOPE_RESTORE();
}

typedef struct {
	Token *tok;
	bool at_stmt_start;
	int brace_depth;
	uint16_t next_scope_id;
	Token *file_scope_stmt_start;
	bool p1e_ret_void;
	bool p1e_ret_captured;
	uint16_t *scope_stack;
	int scope_depth;
	int scope_cap;
	int p1d_cur_func;
	int p1d_switch_cap;
	uint16_t *p1d_switch_stack;
	uint32_t *p1d_switch_end;
	int p1d_switch_top;
	uint32_t p1d_braceless_next_sid;
	Token *p1d_prev;
	bool p1d_saw_raw;
	bool p1d_saw_static;
	int p1d_init_brace_depth;
	bool p1d_ctrl_pending;
	uint32_t *skip_cache;

	// GNU __label__ local label declarations — allows same-named labels
	struct {
		char *name;
		int len;
		uint16_t scope_id;
		char *mangled;
		int mangled_len;
	} *local_labels;

	int local_label_count;
	int local_label_cap;
} P1ScanState;

static char *p1d_find_local_label(P1ScanState *s, char *name, int len, uint16_t cur_sid, int *out_len) {
	for (int i = s->local_label_count - 1; i >= 0; i--) {
		if (s->local_labels[i].len != len) continue;
		if (!prism_memeq_runtime_sized(s->local_labels[i].name, name, (uint32_t)len)) continue;
		if (scope_is_ancestor_or_self(s->local_labels[i].scope_id, cur_sid)) {
			*out_len = s->local_labels[i].mangled_len;
			return s->local_labels[i].mangled;
		}
	}
	return NULL;
}

static void p1d_set_label_name(P1FuncEntry *e, P1ScanState *ps, Token *name, uint16_t cur_sid) {
	int ml;
	char *mangled = p1d_find_local_label(ps, tok_loc(name), name->len, cur_sid, &ml);
	if (mangled) {
		e->label.name = mangled;
		e->label.len = ml;
	} else {
		e->label.name = tok_loc(name);
		e->label.len = name->len;
	}
}

static void p1d_record_goto(P1ScanState *ps, Token *tok, uint16_t cur_sid, int p1d_cur_func) {
	if (!(tok->tag & TT_GOTO) || is_known_typedef(tok) || !tok_next(tok)) return;
	if (is_identifier_like(tok_next(tok))) {
		P1FuncEntry *e = p1_alloc(P1K_GOTO, cur_sid, tok);
		p1d_set_label_name(e, ps, tok_next(tok), cur_sid);
	} else if (match_ch(skip_noise(tok_next(tok)), '*'))
		func_meta[p1d_cur_func].has_computed_goto = true;
}

static void p1d_register_enum_at(Token *tok, int brace_depth, uint16_t sid, int p1d_cur_func) {
	if (!is_enum_kw(tok)) return;
	Token *brace = find_struct_body_brace(tok);
	if (!brace) return;
	parse_enum_constants(brace, brace_depth);
	p1_check_enum_body_defer_shadow(brace, sid, p1d_cur_func);
}

// Handle '{' in prescan: scope tracking, Phase 1E return type capture,
static void p1d_ensure_switch_cap(P1ScanState *s) {
	if (s->p1d_switch_top < s->p1d_switch_cap) return;
	int old = s->p1d_switch_cap;
	size_t nc = vec_grow_cap((size_t)old, (size_t)s->p1d_switch_top + 1, 64);
	s->p1d_switch_stack = arena_realloc(
	    &ctx->main_arena, s->p1d_switch_stack, old * sizeof(uint16_t), nc * sizeof(uint16_t));
	s->p1d_switch_end = arena_realloc(
	    &ctx->main_arena, s->p1d_switch_end, old * sizeof(uint32_t), nc * sizeof(uint32_t));
	s->p1d_switch_cap = (int)nc;
}

static void p1d_handle_open_brace(P1ScanState *s) {
	Token *tok = s->tok;
	uint32_t tidx = tok_idx(tok);
	while (s->next_scope_id < scope_tree_count && scope_tree[s->next_scope_id].open_tok_idx < tidx)
		s->next_scope_id++;
	// this '{' was skipped in phase 1A (e.g. init-in-init optimization).
	uint16_t sid;
	if (s->next_scope_id < scope_tree_count && scope_tree[s->next_scope_id].open_tok_idx == tidx) {
		sid = s->next_scope_id++;
	} else {
		sid = s->scope_depth > 0 ? s->scope_stack[s->scope_depth] : 0;
	}

	// Phase 1E: function body detection at file scope
	if (s->brace_depth == 0 && sid < scope_tree_count && scope_tree[sid].is_func_body) {
		int ret = capture_function_return_type(s->file_scope_stmt_start);
		if (ret == 0) {
			Token *prev = tok_walk_back(tok_idx(tok) - 1, WB_SKIP_NOISE);
			if (prev && match_ch(prev, ';')) prev = p1_knr_find_close_paren(prev);
			if (prev && match_ch(prev, ')') && tok_match(prev)) {
				Token *open = tok_match(prev);
				for (uint32_t pi = tok_idx(open); pi > 1; pi--) {
					Token *pt = &token_pool[pi - 1];
					if (pt->kind == TK_PREP_DIR) continue;
					if (match_ch(pt, '{') || match_ch(pt, '}') || match_ch(pt, ';'))
						break;
					s->file_scope_stmt_start = pt;
				}
				ret = capture_function_return_type(s->file_scope_stmt_start);
			}
		}
		s->p1e_ret_void = (ret == 1);
		s->p1e_ret_captured = (ret == 2);
		ARENA_ENSURE_CAP(
		    &ctx->main_arena, ctx->p1_func_meta, func_meta_count, func_meta_cap, 64, FuncMeta);
		FuncMeta *fm = &func_meta[func_meta_count++];
		fm->body_open = tok;
		fm->returns_void = s->p1e_ret_void;
		fm->defer_name_set = (HashMap){0};
		if (s->p1e_ret_captured) {
			fm->ret_type_start = ctx->func_ret_type_start;
			fm->ret_type_end = ctx->func_ret_type_end;
			fm->ret_type_suffix_start = ctx->func_ret_type_suffix_start;
			fm->ret_type_suffix_end = ctx->func_ret_type_suffix_end;
		} else {
			fm->ret_type_start = fm->ret_type_end = NULL;
			fm->ret_type_suffix_start = fm->ret_type_suffix_end = NULL;
		}
		s->p1e_ret_void = false;
		s->p1e_ret_captured = false;
		// Phase 1C: register parameter shadows at the function body scope
		if (sid < scope_tree_count) {
			td_scope_open = scope_tree[sid].open_tok_idx;
			td_scope_close = scope_tree[sid].close_tok_idx;
		}
		Token *prev_tok = tok_walk_back(tok_idx(tok) - 1, WB_SKIP_NOISE);
		if (prev_tok && match_ch(prev_tok, ';')) prev_tok = p1_knr_find_close_paren(prev_tok);
		if (prev_tok && match_ch(prev_tok, ')') && tok_match(prev_tok)) {
			p1_register_param_shadows(
			    tok_match(prev_tok), prev_tok, sid, s->brace_depth + 1, true);
			p1_register_knr_param_vlas(prev_tok, tok, sid, s->brace_depth + 1);
		}
	}

	// Phase 1D: enter function body — record entry start
	if (s->brace_depth == 0 && sid < scope_tree_count && scope_tree[sid].is_func_body) {
		s->p1d_cur_func = func_meta_count - 1;
		func_meta[s->p1d_cur_func].entry_start = p1_entry_count;
		func_meta[s->p1d_cur_func].entry_count = 0;
		s->p1d_switch_top = 0;
		s->p1d_prev = NULL;
	}

	// Phase 1D: track switch scope for case label association
	if (s->p1d_cur_func >= 0 && sid < scope_tree_count && scope_tree[sid].is_switch) {
		p1d_ensure_switch_cap(s);
		p1_alloc(P1K_SWITCH, sid, tok);
		s->p1d_switch_stack[s->p1d_switch_top] = sid;
		s->p1d_switch_end[s->p1d_switch_top] = 0; // braced: popped at }
		s->p1d_switch_top++;
	}

	s->brace_depth++;
	s->scope_depth++;
	ARENA_ENSURE_CAP(
	    &ctx->main_arena, s->scope_stack, s->scope_depth + 1, s->scope_cap, 256, uint16_t);
	s->scope_stack[s->scope_depth] = sid;
	if (sid < scope_tree_count && scope_tree[sid].is_init) s->p1d_init_brace_depth++;
	if (sid < scope_tree_count) {
		td_scope_open = scope_tree[sid].open_tok_idx;
		td_scope_close = scope_tree[sid].close_tok_idx;
	}

	s->at_stmt_start = true;
	s->p1d_saw_raw = false;
	s->p1d_saw_static = false;
	s->p1d_ctrl_pending = false;
	/* Predecessor of the first statement in the block is the '{', not a
	 * stale token from before the brace (e.g. for/if's closing ')'). */
	s->p1d_prev = tok;
	s->tok = tok_next(tok);
}

static void p1d_handle_close_brace(P1ScanState *s) {
	Token *tok = s->tok;
	// Phase 1D: pop switch scope(s) that end at this brace
	if (s->p1d_cur_func >= 0 && s->p1d_switch_top > 0) {
		uint16_t closing_sid = s->scope_stack[s->scope_depth];
		if (s->p1d_switch_end[s->p1d_switch_top - 1] == 0 &&
		    s->p1d_switch_stack[s->p1d_switch_top - 1] == closing_sid)
			s->p1d_switch_top--;
	}

	bool closing_non_stmt_brace = false;
	if (s->p1d_init_brace_depth > 0) {
		uint16_t csid = s->scope_stack[s->scope_depth];
		if (csid < scope_tree_count && (scope_tree[csid].is_struct || scope_tree[csid].is_init))
			closing_non_stmt_brace = true;
		if (csid < scope_tree_count && scope_tree[csid].is_init) s->p1d_init_brace_depth--;
	} else if (s->scope_depth > 0) {
		uint16_t csid = s->scope_stack[s->scope_depth];
		if (csid < scope_tree_count && scope_tree[csid].is_struct) closing_non_stmt_brace = true;
	}

	if (s->brace_depth > 0) {
		s->brace_depth--;
		if (s->scope_depth > 0) s->scope_depth--;
	}

	{
		uint16_t cur_sid = s->scope_stack[s->scope_depth];
		if (cur_sid > 0 && cur_sid < scope_tree_count) {
			td_scope_open = scope_tree[cur_sid].open_tok_idx;
			td_scope_close = scope_tree[cur_sid].close_tok_idx;
		} else {
			td_scope_open = 0;
			td_scope_close = UINT32_MAX;
		}
	}

	// Phase 1D: finalize function entry count
	if (s->brace_depth == 0 && s->p1d_cur_func >= 0) {
		func_meta[s->p1d_cur_func].entry_count =
		    p1_entry_count - func_meta[s->p1d_cur_func].entry_start;
		s->p1d_cur_func = -1;
		s->file_scope_stmt_start = tok_next(tok);
		s->p1e_ret_void = false;
		s->p1e_ret_captured = false;
		s->local_label_count = 0; // Reset local labels for next function
	}

	s->at_stmt_start = !closing_non_stmt_brace;
	s->p1d_saw_raw = false;
	s->p1d_saw_static = false;
	s->p1d_ctrl_pending = false;
	s->p1d_prev = tok;
	s->tok = tok_next(tok);
}

static PRISM_HOT void p1_full_depth_prescan(Token *tok) {
	P1ScanState _ps = {0};
	P1ScanState *ps = &_ps;
	ps->tok = tok;
	ps->at_stmt_start = true;
	ps->next_scope_id = 1;
	ps->file_scope_stmt_start = tok;
	ps->scope_cap = 256;
	ps->scope_stack = arena_alloc_uninit(&ctx->main_arena, 256 * sizeof(uint16_t));
	ps->scope_stack[0] = 0; // file scope
	ps->p1d_cur_func = -1;
	ps->p1d_switch_cap = 64;
	ps->p1d_switch_stack = arena_alloc_uninit(&ctx->main_arena, 64 * sizeof(uint16_t));
	ps->p1d_switch_end = arena_alloc_uninit(&ctx->main_arena, 64 * sizeof(uint32_t));
	ps->p1d_braceless_next_sid = scope_tree_count;
	ps->skip_cache = arena_alloc(&ctx->main_arena, token_count * sizeof(uint32_t));
	td_scope_open = 0;
	td_scope_close = UINT32_MAX;
	p1_raw_block_count = 0;

#define CUR_SID() (ps->scope_stack[ps->scope_depth])
#define P1D_STMT_RESET()                                                                                     \
	do {                                                                                                 \
		ps->at_stmt_start = true;                                                                    \
		ps->p1d_saw_raw = false;                                                                     \
		ps->p1d_saw_static = false;                                                                  \
		ps->p1d_ctrl_pending = false;                                                                \
	} while (0)
#ifdef PRISM_DEBUG
	/* Termination watchdog: the prescan's outer loop must advance through
	 * the token stream; total iterations are linear in token_count.  A trip
	 * here means a cursor-stall (non-termination) bug, surfaced loudly
	 * instead of hanging the build.  Debug builds only — zero release cost. */
	uint64_t p1_wd_steps = 0;
	const uint64_t p1_wd_budget = 256ull * (uint64_t)token_count + 65536ull;
#endif
	while (ps->tok && ps->tok->kind != TK_EOF) {
#ifdef PRISM_DEBUG
		if (++p1_wd_steps > p1_wd_budget)
			error_tok(ps->tok,
				  "internal: Phase 1 progress watchdog tripped "
				  "(possible non-termination); please report");
#endif
		while (ps->p1d_switch_top > 0 && ps->p1d_switch_end[ps->p1d_switch_top - 1] > 0 &&
		       tok_idx(ps->tok) > ps->p1d_switch_end[ps->p1d_switch_top - 1])
			ps->p1d_switch_top--;
		// Phase 1: record function prototypes/definitions.
		// Used by Pass 2 to avoid memset on typeof(func) declarations.
		if (token_can_name_function(ps->tok)) {
			Token *nx = tok_next(ps->tok);
			if (nx && match_ch(nx, '(')) {
				bool is_func_decl = false;
				void *proto_tag = p1_decl_looks_like_struct_return(ps->tok)
						      ? P1_PROTO_STRUCT_RET
						      : P1_PROTO_FN;
				if (ps->brace_depth == 0) {
					hashmap_put(&p1_func_proto_map, tok_loc(ps->tok), ps->tok->len, proto_tag);
					is_func_decl = true;
				} else {
					Token *prev = tok_walk_back(tok_idx(ps->tok), WB_PAST_NOISE);
					if (prev && ((prev->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE |
								   TT_SUE | TT_TYPEOF)) ||
						     is_known_typedef(prev))) {
						hashmap_put(
						    &p1_func_proto_map, tok_loc(ps->tok), ps->tok->len, proto_tag);
						is_func_decl = true;
					}
				}
				if (is_func_decl && FEAT(F_ORELSE) && tok_match(nx))
					p1d_reject_proto_param_orelse(nx);
			}
		}

		if (ps->p1d_cur_func == -1 && ps->p1d_init_brace_depth == 0 && ps->at_stmt_start &&
		    !is_known_typedef(ps->tok)) {
			if (ps->tok->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_CASE | TT_DEFAULT))
				error_tok(ps->tok,
					  "control statement at file scope (must appear inside a "
					  "function body)");
			if (ps->tok->tag & TT_GOTO)
				error_tok(ps->tok, "'goto' at file scope (must appear inside a function body)");
			if ((ps->tok->tag & (TT_IF | TT_LOOP | TT_SWITCH)) || is_else_kw(ps->tok))
				error_tok(ps->tok,
					  "control-flow statement at file scope (must appear inside a "
					  "function body)");
		}

		/* Decl dims are classified in probe (with SE hoist). Remaining `[`
		 * are expression subscripts / designators — in-place ternary, so
		 * reject side-effect LHS rather than allowing a false hoist. */
		if (FEAT(F_ORELSE) && match_ch(ps->tok, '[') && tok_match(ps->tok) &&
		    !(ps->tok->flags & TF_C23_ATTR) && !(tok_ann(ps->tok) & P1_OE_BRACKET))
			p1d_classify_bracket_orelse_ex(ps->tok, CUR_SID(), ps->p1d_cur_func, true,
						       /*allow_se_hoist=*/false);
		// Phase 1: reject orelse *keyword* uses in enum constant
		// expressions and struct/union bodies. Field / enumerator
		// *names* named orelse are identifiers (shadowed or not).
		if (FEAT(F_ORELSE) && (ps->tok->tag & TT_ORELSE) && !typedef_lookup(ps->tok)) {
			uint16_t cur_sid = CUR_SID();
			/* Positional check must see the true predecessor — after a
			 * balanced-group jump ps->p1d_prev holds the OPEN paren, so
			 * `OK = f() orelse 0` would misread as identifier context.
			 * Walk back in the pool (attr/prep noise skipped) and reject
			 * only after an expression-ending token; a declarator or
			 * enumerator *name* follows a type/typedef/`{`/`,` instead.
			 * (orelse_shadow_is_kw is unsuitable here: its cast heuristic
			 * misreads a call's empty `()` as a cast type-name.) */
			Token *pv = tok_walk_back(tok_idx(ps->tok), WB_ATTR_NOISE);
			bool expr_ctx =
			    pv && (pv->kind == TK_NUM || pv->kind == TK_STR || match_ch(pv, ')') ||
				   match_ch(pv, ']') ||
				   (is_identifier_like(pv) &&
				    !(pv->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_SUE | TT_TYPEOF |
						 TT_BITINT | TT_ALIGNAS | TT_INLINE | TT_ATTR | TT_RETURN |
						 TT_BREAK | TT_CONTINUE | TT_GOTO)) &&
				    !is_known_typedef(pv)));
			if (cur_sid < scope_tree_count &&
			    (scope_tree[cur_sid].is_enum || scope_tree[cur_sid].is_struct) && expr_ctx)
				error_tok(ps->tok, ERR_ORELSE_STMT_LEVEL);
			/* Brace-initializer RHS (incl. designator `= expr orelse …`) cannot
			 * lower statement-shaped orelse; designator *dimensions*
			 * `[idx orelse …]` are annotated P1_OE_BRACKET and stay allowed. */
			if (ps->p1d_init_brace_depth > 0 && expr_ctx &&
			    !(tok_ann(ps->tok) & P1_OE_BRACKET))
				error_tok(ps->tok,
					  "'orelse' cannot be used in a brace initializer "
					  "expression; only designator dimensions "
					  "'[idx orelse …]' are supported");
		}

		// Phase 1: reject orelse inside typeof in struct/union bodies early,
		// before any Pass 2 output is written.
		if (FEAT(F_ORELSE) && (ps->tok->tag & TT_TYPEOF))
			p1d_annotate_typeof_orelse(ps->tok, CUR_SID(), ps->p1d_cur_func, true);

		if (match_ch(ps->tok, '{')) {
			p1d_handle_open_brace(ps);
			continue;
		}
		if (match_ch(ps->tok, '}')) {
			p1d_handle_close_brace(ps);
			continue;
		}

		if (match_ch(ps->tok, ';')) {
			P1D_STMT_RESET();
			ps->p1d_prev = ps->tok;
			if (ps->brace_depth == 0) {
				// Phase 1C: C99 prototype parameter scope (§6.2.1p4).
				Token *prev_tok = tok_walk_back(tok_idx(ps->tok) - 1, WB_SKIP_NOISE);
				if (prev_tok && match_ch(prev_tok, ')') && tok_match(prev_tok)) {
					Token *open = tok_match(prev_tok);
					TD_SCOPE_SAVE();
					td_scope_open = tok_idx(open);
					td_scope_close = tok_idx(prev_tok);
					p1_register_param_shadows(open, prev_tok, 0, 1, false);
					TD_SCOPE_RESTORE();
				}

				ps->p1e_ret_void = false;
				ps->p1e_ret_captured = false;
				ps->file_scope_stmt_start = tok_next(ps->tok);
			}
			ps->tok = tok_next(ps->tok);
			continue;
		}
		if (ps->tok->kind == TK_PREP_DIR) {
			ps->at_stmt_start = true;
			/* Keep ps->p1d_saw_raw: `raw _Pragma("...") int x;` must still
			 * suppress zero-init — skip_noise in Pass 2 preserves raw
			 * across prep dirs the same way. */
			ps->p1d_saw_static = false;
			if (ps->brace_depth == 0) ps->file_scope_stmt_start = tok_next(ps->tok);
			ps->tok = tok_next(ps->tok);
			continue;
		}

		if (!ps->at_stmt_start) {
			// Phase 1D: detect gotos and defers even in non-stmt-start
			if (ps->p1d_cur_func >= 0) {
				uint16_t cur_sid = CUR_SID();
				p1d_record_goto(ps, ps->tok, cur_sid, ps->p1d_cur_func);
				if (is_defer_kw(ps->tok, ps->p1d_prev) &&
				    !(is_known_function_call(ps->tok) && !ps->p1d_ctrl_pending))
					p1_try_alloc_defer(ps->tok, cur_sid, ps->p1d_cur_func);
			}

			p1d_register_enum_at(ps->tok, ps->brace_depth, CUR_SID(), ps->p1d_cur_func);
			Token *p1d_prev_saved = ps->p1d_prev;
			ps->p1d_prev = ps->tok;
			if (ps->tok->flags & TF_OPEN && tok_match(ps->tok)) {
				// Do not skip GNU statement expressions — process their body normally
				if (is_stmt_expr_open(ps->tok)) {
					ps->tok = tok_next(ps->tok); // advance past '(' to '{'
					ps->at_stmt_start = true;
					continue;
				}
				// Phase 1D: reject orelse/defer inside non-control-flow
				// parentheses (hoisted from Pass 2 check_orelse_in_parens).
				// typeof keeps expr-orelse (`typeof(p orelse 0)`) but still
				// rejects type-junk leaks (`typeof(int orelse 0)`).
				if ((FEAT(F_ORELSE) || FEAT(F_DEFER)) && match_ch(ps->tok, '(') &&
				    !(p1d_prev_saved &&
				      (p1d_prev_saved->tag & (TT_IF | TT_LOOP | TT_SWITCH | TT_ATTR | TT_ASM)))) {
					if (p1d_prev_saved && (p1d_prev_saved->tag & TT_TYPEOF))
						check_typeof_paren_orelse_type_junk(ps->tok);
					else
						check_orelse_in_parens(ps->tok);
				}
				// Phase 1D: reject orelse/CF inside attribute and asm paren
				// groups (pre-/post-declarator attrs; __asm__(...)).
				if (tok_match(ps->tok) &&
				    ((match_ch(ps->tok, '(') && p1d_prev_saved &&
				      (p1d_prev_saved->tag & (TT_ATTR | TT_ASM))) ||
				     (ps->tok->flags & TF_C23_ATTR))) {
					int in_asm = p1d_prev_saved && (p1d_prev_saved->tag & TT_ASM);
					/* `asm volatile (` — walk back through asm qualifiers. */
					if (!in_asm && match_ch(ps->tok, '(')) {
						Token *aw = tok_walk_back(tok_idx(ps->tok), WB_ATTR_NOISE);
						while (aw && (aw->tag & TT_QUALIFIER))
							aw = tok_walk_back(tok_idx(aw), WB_ATTR_NOISE);
						in_asm = aw && (aw->tag & TT_ASM);
					}
					Token *am = tok_match(ps->tok);
					for (Token *s = tok_next(ps->tok); s && s != am; s = tok_next(s)) {
						uint32_t st = s->tag;
						if (FEAT(F_ORELSE) && (st & TT_ORELSE) && !typedef_lookup(s)) {
							/* GNU symbolic operand name `[orelse]` is an
							 * identifier, not the Prism operator. */
							Token *sp = tok_walk_back(tok_idx(s), WB_ATTR_NOISE);
							Token *sn = skip_noise(tok_next(s));
							if (sp && match_ch(sp, '[') && sn && match_ch(sn, ']'))
								continue;
							error_tok(s,
								  in_asm ? "'orelse' cannot be used inside "
									   "asm arguments"
									 : "'orelse' cannot be used inside "
									   "attribute arguments");
						}
						if (st & (TT_GOTO | TT_RETURN | TT_BREAK | TT_CONTINUE))
							error_tok(s,
								  in_asm ? "'%.*s' inside asm argument "
									   "bypasses control-flow analysis; "
									   "move it outside the asm"
									 : "'%.*s' inside attribute argument "
									   "bypasses control-flow analysis; "
									   "move it outside the attribute",
								  s->len, tok_loc(s));
					}
				}
				if (match_ch(ps->tok, '(') || match_ch(ps->tok, '[')) {
					Token *se_open = p1d_scan_balanced_group(
					    ps->tok, ps->brace_depth, ps->p1d_cur_func, CUR_SID(), p1d_prev_saved);
					if (se_open) {
						ps->tok = se_open;
						continue;
					}
				}
				Token *grp_open = ps->tok;
				Token *grp_close = tok_match(grp_open);
				ps->tok = tok_next(grp_close);
				/* if/while/for/switch condition close — not else/do body '('. */
				if (match_ch(grp_open, '(') && ctrl_condition_kw_before_paren(grp_open)) {
					ps->at_stmt_start = true;
					ps->p1d_ctrl_pending = true;
				}
			} else {
				if (match_ch(ps->tok, ')') && tok_match(ps->tok) &&
				    ctrl_condition_kw_before_paren(tok_match(ps->tok))) {
					/* Must not use WB_PAST_NOISE inside the helper: it jumps
					 * }/{ and nested (), so `} (expr)` after `while (c){}`
					 * would look like the while-condition close. */
					ps->at_stmt_start = true;
					ps->p1d_ctrl_pending = true;
				}
				ps->tok = tok_next(ps->tok);
			}
			continue;
		}

		// Skip noise (attributes, C23 [[...]], pragmas)
		Token *clean = skip_noise(ps->tok);
		if (clean != ps->tok) {
			for (Token *s = ps->tok; s && s != clean; s = tok_next(s)) {
				uint32_t st = s->tag;
				if (st & (TT_GOTO | TT_RETURN | TT_BREAK | TT_CONTINUE))
					error_tok(s,
						  "'%.*s' inside attribute argument "
						  "bypasses control-flow analysis; "
						  "move it outside the attribute",
						  s->len,
						  tok_loc(s));
				if ((st & TT_DEFER) && !typedef_lookup(s))
					error_tok(s,
						  "'defer' inside attribute argument "
						  "bypasses control-flow analysis; "
						  "move it outside the attribute",
						  s->len,
						  tok_loc(s));
				if ((st & TT_ORELSE) && !typedef_lookup(s))
					error_tok(s,
						  "'orelse' cannot be used inside "
						  "attribute arguments");
			}
			ps->tok = clean;
			continue;
		}

		// Skip storage/inline/noreturn/extension specifiers before type
		if (((ps->tok->tag & (TT_STORAGE | TT_INLINE)) || equal(ps->tok, "__extension__")) &&
		    !(is_soft_keyword_identifier(ps->tok) && token_is_label_name(ps->tok))) {
			if (ps->tok->tag & TT_STORAGE) ps->p1d_saw_static = true;
			ps->tok = tok_next(ps->tok);
			continue;
		}

		if (ps->tok->flags & TF_RAW) {
			Token *rnext = tok_next(ps->tok);
			Token *after_raw = skip_noise(rnext);
			bool typedef_kw_prefix =
			    is_known_typedef(ps->tok) && after_raw &&
			    (is_type_keyword(after_raw) || is_known_typedef(after_raw) ||
			     (after_raw->tag &
			      (TT_QUALIFIER | TT_SUE | TT_STORAGE | TT_INLINE | TT_TYPEDEF)) ||
			     (after_raw->flags & TF_RAW));
			if (!is_known_typedef(ps->tok) || typedef_kw_prefix) {
				if (!(rnext && match_ch(rnext, ':') &&
				      !(tok_next(rnext) && match_ch(tok_next(rnext), ':')))) {
					/* `raw { ... }` — annotate the brace so Pass 2 / decls
					 * suppress transforms for the whole block. */
					if (after_raw && match_ch(after_raw, '{')) {
						after_raw->ann |= P1_RAW_BLOCK;
						p1_raw_block_count++;
						ps->tok = after_raw;
						continue;
					}
					ps->p1d_saw_raw = true;
					ps->tok = tok_next(ps->tok);
					continue;
				}
			}
		}

		if (ps->tok->tag & TT_TYPEDEF) {
			uint32_t td_saved_close = 0;
			if (ps->p1d_ctrl_pending && ps->brace_depth > 0) {
				Token *stmt_end = skip_one_stmt_impl(ps->tok, ps->skip_cache);
				if (stmt_end) {
					td_saved_close = td_scope_close;
					td_scope_close = tok_idx(stmt_end);
				}
			}
			parse_typedef_declaration(ps->tok, ps->brace_depth);
			if (td_saved_close) td_scope_close = td_saved_close;
			// Braces consumed here don't increment ps->next_scope_id — the main '{'
			while (ps->tok && ps->tok->kind != TK_EOF && !match_ch(ps->tok, ';')) {
				p1d_register_enum_at(ps->tok, ps->brace_depth, CUR_SID(), ps->p1d_cur_func);
				if (match_ch(ps->tok, '{') && tok_match(ps->tok)) {
					Token *close = tok_match(ps->tok);
					TD_SCOPE_SAVE();
					td_scope_open = tok_idx(ps->tok);
					td_scope_close = tok_idx(close);
					for (Token *m = tok_next(ps->tok);
					     m && m != close && m->kind != TK_EOF;) {
						if (is_enum_kw(m)) {
							uint32_t so = td_scope_open, sc = td_scope_close;
							td_scope_open = _tds_o;
							td_scope_close = _tds_c;
							p1d_register_enum_at(
							    m, ps->brace_depth, CUR_SID(), ps->p1d_cur_func);
							td_scope_open = so;
							td_scope_close = sc;
						}
						if (m->flags & TF_OPEN && tok_match(m)) {
							if (FEAT(F_ORELSE) &&
							    (match_ch(m, '(') || match_ch(m, '[')))
								p1d_scan_balanced_group(m,
											ps->brace_depth,
											ps->p1d_cur_func,
											CUR_SID(),
											NULL);
							m = tok_next(tok_match(m));
							continue;
						}
						if (is_valid_varname(m) &&
						    (is_known_typedef(m) ||
						     (m->tag & (TT_DEFER | TT_ORELSE)))) {
							Token *nxt = tok_next(m);
							if (nxt &&
							    (match_ch(nxt, ';') || match_ch(nxt, ',') ||
							     match_ch(nxt, ':') || match_ch(nxt, '[') ||
							     match_ch(nxt, '='))) {
								// Only shadow if a type specifier precedes m in this member.
								if (match_ch(nxt, ':')) {
									bool has_type = false;
									for (uint32_t pi = tok_idx(m);
									     pi > tok_idx(ps->tok);
									     pi--) {
										Token *pt =
										    &token_pool[pi - 1];
										if (pt->kind == TK_PREP_DIR)
											continue;
										if (match_set(pt,
											      CH(';') |
												  CH(',')) ||
										    match_ch(pt, '{') ||
										    match_ch(pt, '}'))
											break;
										if (pt->tag & TT_QUALIFIER)
											continue;
										if (match_ch(pt, ')') &&
										    tok_match(pt)) {
											pi =
											    tok_idx(tok_match(
												pt)) +
											    1;
											continue;
										}
										if (match_ch(pt, ']') &&
										    tok_match(pt)) {
											pi =
											    tok_idx(tok_match(
												pt)) +
											    1;
											continue;
										}
										has_type = true;
										break;
									}
									if (!has_type) {
										m = tok_next(m);
										continue;
									}
								}
								TYPEDEF_ADD_IDX(
								    typedef_add_shadow(
									tok_loc(m), m->len, ps->brace_depth + 1),
								    m);
							}
						}
						m = tok_next(m);
					}
					TD_SCOPE_RESTORE();
					ps->tok = tok_next(close);
					continue;
				}
				if (ps->tok->flags & TF_OPEN && tok_match(ps->tok)) {
					if (FEAT(F_ORELSE) &&
					    (match_ch(ps->tok, '(') || match_ch(ps->tok, '[')))
						p1d_scan_balanced_group(ps->tok,
									ps->brace_depth,
									ps->p1d_cur_func,
									CUR_SID(),
									NULL);
					ps->tok = tok_next(tok_match(ps->tok));
				} else
					ps->tok = tok_next(ps->tok);
			}
			if (ps->tok && match_ch(ps->tok, ';')) ps->tok = tok_next(ps->tok);
			ps->at_stmt_start = true;
			if (ps->brace_depth == 0) ps->file_scope_stmt_start = ps->tok;
			continue;
		}

		// Does NOT continue — falls through to Phase 1C/1D for declaration
		// detection.
		if (ps->tok->tag & TT_SUE) {
			Token *brace = find_struct_body_brace(ps->tok);
			if (brace) {
				if (is_enum_kw(ps->tok)) {
					p1d_register_enum_at(ps->tok, ps->brace_depth, CUR_SID(), ps->p1d_cur_func);
				} else {
					bool body_vla = struct_body_contains_vla(brace);
					bool body_vol = struct_body_contains_volatile(brace);
					for (Token *t = skip_noise(tok_next(ps->tok)); t && t != brace;
					     t = skip_noise(tok_next(t))) {
						if ((t->tag & TT_QUALIFIER) && !is_soft_keyword_identifier(t))
							continue;
						if (is_valid_varname(t)) {
							int pre = typedef_table.count;
							typedef_add_entry(tok_loc(t),
									  t->len,
									  ps->brace_depth,
									  TDK_STRUCT_TAG,
									  body_vla,
									  false);
							if (typedef_table.count > pre) {
								TypedefEntry *te =
								    &typedef_table
									 .entries[typedef_table.count - 1];
								te->token_index = tok_idx(t);
								te->is_aggregate = true;
								if (body_vol) te->has_volatile_member = true;
							}
							break;
						}
						break;
					}
				}
			}
		}

		if ((ps->tok->flags & TF_STATIC_ASSERT) &&
		    !(is_soft_keyword_identifier(ps->tok) && token_is_label_name(ps->tok))) {
			/* Phase 1 normally skips _Static_assert; still classify
			 * bracket-orelse inside so Pass 2 can lower to a ternary
			 * (otherwise `orelse` leaks into the backend). Reject
			 * non-bracket orelse — statement/action forms cannot
			 * appear in an integer constant expression. */
			if (FEAT(F_ORELSE) || FEAT(F_DEFER)) {
				Token *lp = skip_noise(tok_next(ps->tok));
				if (lp && match_ch(lp, '(') && (lp->flags & TF_OPEN) && tok_match(lp)) {
					Token *rp = tok_match(lp);
					/* Walk every token — do not skip nested groups or
					 * `sizeof(char[…])` brackets are never classified. */
					if (FEAT(F_ORELSE))
						for (Token *inner = tok_next(lp); inner && inner != rp;
						     inner = tok_next(inner)) {
							if (match_ch(inner, '[') && (inner->flags & TF_OPEN) &&
							    tok_match(inner) && !(inner->flags & TF_C23_ATTR) &&
							    !(tok_ann(inner) & P1_OE_BRACKET))
								p1d_classify_bracket_orelse_ex(inner,
											     CUR_SID(),
											     ps->p1d_cur_func,
											     /*hard_ctx=*/false,
											     /*allow_se_hoist=*/false);
							/* _Static_assert needs an ICE — VLA dims from
							 * `sizeof(char[n orelse 1])` are not ICEs. */
							if (match_ch(inner, '[') && (tok_ann(inner) & P1_OE_BRACKET)) {
								Token *bc = tok_match(inner);
								for (Token *oe = tok_next(inner); oe && oe != bc;
								     oe = tok_next(oe)) {
									if (!(tok_ann(oe) & P1_IS_ORELSE_KW))
										continue;
									if (bracket_dim_lhs_nonconstant(
										tok_next(inner), oe))
										error_tok(
										    oe,
										    "'orelse' in an array "
										    "dimension inside "
										    "_Static_assert/static_assert "
										    "requires an integer constant "
										    "expression on the left-hand "
										    "side");
								}
							}
						}
					Token *prev_sa = lp;
					for (Token *s = tok_next(lp); s && s != rp; s = tok_next(s)) {
						/* Walk into nested groups — `(0 orelse 1)` and
						 * `sizeof(0 orelse 1)` / `int orelse 0` must reject, not leak. */
						if (FEAT(F_ORELSE) && !(tok_ann(s) & P1_IS_ORELSE_KW) &&
						    (orelse_kw_at_bare(s, prev_sa) ||
						     orelse_after_type_in_parens(s, prev_sa)))
							error_tok(s,
								  "'orelse' cannot be used in "
								  "_Static_assert/static_assert except "
								  "inside an array dimension");
						/* A defer statement shape inside the argument
						 * (`defer h(...)` / `defer { ... }`) would be
						 * consumed by Pass 2's statement handler, emptying
						 * the assert argument and pasting the body at scope
						 * exits — token-mangling miscompile found by the
						 * contexts suite.  A lone identifier named defer
						 * remains legal in the constant expression. */
						if (FEAT(F_DEFER) && (s->tag & TT_DEFER) && tok_next(s) &&
						    (is_identifier_like(tok_next(s)) ||
						     match_ch(tok_next(s), '{')))
							error_tok(s,
								  "'defer' cannot be used inside "
								  "_Static_assert/static_assert");
						if (s->flags & TF_OPEN && tok_match(s))
							prev_sa = s;
						else
							prev_sa = s;
					}
				}
			}
			ps->tok = skip_to_semicolon(ps->tok, NULL);
			if (ps->tok && match_ch(ps->tok, ';')) ps->tok = tok_next(ps->tok);
			ps->at_stmt_start = true;
			if (ps->brace_depth == 0) ps->file_scope_stmt_start = ps->tok;
			continue;
		}

		if ((ps->tok->tag & TT_LOOP) && ps->tok->ch0 == 'f' && ps->brace_depth > 0 && ps->p1d_cur_func >= 0)
			p1d_scan_ctrl_init(ps->tok,
					   ps->skip_cache,
					   ps->brace_depth,
					   CUR_SID(),
					   /*is_for=*/true);
		if ((ps->tok->tag & (TT_IF | TT_SWITCH)) && ps->brace_depth > 0 && ps->p1d_cur_func >= 0 &&
		    !is_else_kw(ps->tok))
			p1d_scan_ctrl_init(ps->tok,
					   ps->skip_cache,
					   ps->brace_depth,
					   CUR_SID(),
					   /*is_for=*/false);
		/* C does not allow declaration in while (...); reject C23-looking while-init. */
		if ((ps->tok->tag & TT_LOOP) && ps->tok->ch0 == 'w' && ps->brace_depth > 0) {
			Token *wopen = p1d_find_open_paren(ps->tok);
			if (wopen && tok_match(wopen)) {
				Token *inner = skip_noise(tok_next(wopen));
				if (inner &&
				    ((inner->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_SUE | TT_TYPEDEF)) ||
				     (inner->kind == TK_IDENT && is_known_typedef(inner))))
					error_tok(inner,
						  "declaration in 'while' condition is not allowed");
			}
		}
		if (ps->p1d_cur_func >= 0) {
			uint16_t cur_sid = CUR_SID();
			// GNU __label__ local label declaration: __label__ id1, id2, ...;
			if (ps->at_stmt_start && ps->tok->kind == TK_IDENT && ps->tok->len == 9 &&
			    prism_memeq_static(tok_loc(ps->tok), "__label__", 9)) {
				Token *t = tok_next(ps->tok);
				while (t && t->kind != TK_EOF && !match_ch(t, ';')) {
					if (is_identifier_like(t)) {
						ARENA_ENSURE_CAP(&ctx->main_arena,
								 ps->local_labels,
								 ps->local_label_count + 1,
								 ps->local_label_cap,
								 16,
								 ps->local_labels[0]);
						int name_len = t->len;
						char sid_buf[12];
						int sid_len = snprintf(
						    sid_buf, sizeof(sid_buf), "%u", (unsigned)cur_sid);
						int mangled_len = name_len + 1 + sid_len;
						char *mangled =
						    arena_alloc_uninit(&ctx->main_arena, mangled_len);
						memcpy(mangled, tok_loc(t), name_len);
						mangled[name_len] = '\0';
						memcpy(mangled + name_len + 1, sid_buf, sid_len);
						int li = ps->local_label_count++;
						ps->local_labels[li].name = tok_loc(t);
						ps->local_labels[li].len = name_len;
						ps->local_labels[li].scope_id = cur_sid;
						ps->local_labels[li].mangled = mangled;
						ps->local_labels[li].mangled_len = mangled_len;
					}
					t = tok_next(t);
				}
				if (t && match_ch(t, ';')) t = tok_next(t);
				ps->p1d_prev = ps->tok;
				ps->tok = t;
				ps->at_stmt_start = true;
				ps->p1d_ctrl_pending = false;
				continue;
			}

			/* `do int x=1; while(0);` — declaration is not a valid braceless do body. */
			if (ps->at_stmt_start && ps->p1d_prev && is_do_kw(ps->p1d_prev) &&
			    !match_ch(ps->tok, '{') &&
			    ((ps->tok->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_SUE | TT_TYPEDEF)) ||
			     (ps->tok->kind == TK_IDENT && is_known_typedef(ps->tok))))
				error_tok(ps->tok, "'do' body starting with a declaration requires braces");

			if (ps->at_stmt_start && is_identifier_like(ps->tok) &&
			    (!(ps->tok->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE)) ||
			     is_soft_keyword_identifier(ps->tok))) {
				Token *colon = skip_noise(tok_next(ps->tok));
				if (colon && match_ch(colon, ':') &&
				    !(tok_next(colon) && match_ch(tok_next(colon), ':')) &&
				    !(ps->tok->tag & (TT_CASE | TT_DEFAULT)) && ps->p1d_init_brace_depth == 0) {
					P1FuncEntry *e = p1_alloc(P1K_LABEL, cur_sid, ps->tok);
					p1d_set_label_name(e, ps, ps->tok, cur_sid);
					ps->p1d_prev = colon;
					ps->tok = tok_next(colon);
					ps->at_stmt_start = true;
					continue;
				}
			}

			p1d_record_goto(ps, ps->tok, cur_sid, ps->p1d_cur_func);
			if (FEAT(F_DEFER) && (ps->tok->tag & TT_DEFER) && is_defer_kw(ps->tok, ps->p1d_prev)) {
				tok_ann(ps->tok) |= P1_IS_DEFER_KW;
				p1_try_alloc_defer(ps->tok, cur_sid, ps->p1d_cur_func);
			}
			/* Stray `defer` in non-statement positions is caught by the
			 * dedicated context scanners (declarator/argument/dimension
			 * interiors in p1d_probe_declaration; parameter dimensions;
			 * _Static_assert operands; the emit_deferred/expr-context Pass-2
			 * guards) — NOT by a blanket "is_defer_kw==false" rule here,
			 * which would misfire on legitimate defers that prev-tracking
			 * fails to recognize (e.g. after a GNU `__label__` declaration
			 * inside a statement expression). */
			if (ps->tok->tag & (TT_CASE | TT_DEFAULT)) {
				uint16_t sw_sid =
				    ps->p1d_switch_top > 0 ? ps->p1d_switch_stack[ps->p1d_switch_top - 1] : 0;
				P1FuncEntry *e = p1_alloc(P1K_CASE, cur_sid, ps->tok);
				e->kase.switch_scope_id = sw_sid;
				Token *ct = tok_next(ps->tok);
				Token *cprev = ps->tok;
				int td = 0;
				while (ct && ct->kind != TK_EOF) {
					if (match_ch(ct, ';') || match_ch(ct, '{')) break;
					/* case label expressions are integer constant
					 * expressions: an orelse there has no statement
					 * context to lower into and would otherwise pass
					 * through to the backend verbatim. */
					if (FEAT(F_ORELSE) && (ct->tag & TT_ORELSE) &&
					    !(cprev->tag & TT_MEMBER) && !typedef_lookup(ct))
						error_tok(ct,
							  "'orelse' cannot be used inside a case "
							  "label expression");
					if (ct->flags & TF_OPEN && tok_match(ct)) {
						Token *cam = tok_match(ct);
						if (FEAT(F_ORELSE)) {
							Token *cip = ct;
							for (Token *s = tok_next(ct); s && s != cam;
							     s = tok_next(s)) {
								if ((s->tag & TT_ORELSE) &&
								    !(cip->tag & TT_MEMBER) &&
								    !typedef_lookup(s))
									error_tok(
									    s,
									    "'orelse' cannot be used "
									    "inside a case label "
									    "expression");
								cip = s;
							}
						}
						cprev = cam;
						ct = tok_next(cam);
						continue;
					}
					if (match_ch(ct, '?')) {
						td++;
						cprev = ct;
						ct = tok_next(ct);
						continue;
					}
					if (match_ch(ct, ':')) {
						if (td > 0) {
							td--;
							cprev = ct;
							ct = tok_next(ct);
							continue;
						}
						break;
					}
					cprev = ct;
					ct = tok_next(ct);
				}
				if (ct && match_ch(ct, ':')) {
					ps->p1d_prev = ct;
					ps->tok = tok_next(ct);
					P1D_STMT_RESET();
					continue;
				}
			}
		}

		if (tok_ann(ps->tok) & P1_IS_DEFER_KW) {
			p1d_validate_defer(ps->tok, ps->p1d_cur_func, ps->p1d_ctrl_pending, CUR_SID(),
					   ps->brace_depth);
		} else if (FEAT(F_DEFER) && (ps->tok->tag & TT_DEFER) &&
			   is_defer_kw(ps->tok, ps->p1d_prev)) {
			tok_ann(ps->tok) |= P1_IS_DEFER_KW;
			p1d_validate_defer(ps->tok, ps->p1d_cur_func, ps->p1d_ctrl_pending, CUR_SID(),
					   ps->brace_depth);
		}
		p1d_probe_declaration(ps->tok,
				      CUR_SID(),
				      ps->brace_depth,
				      ps->p1d_cur_func,
				      &ps->p1d_saw_raw,
				      ps->p1d_saw_static,
				      ps->p1d_ctrl_pending,
				      ps->skip_cache);
		// Phase 1D: detect braceless switch — emit P1K_SWITCH with synthetic
		// scope_id For `switch (expr) stmt;` (no braces), Phase 1A never creates a
		// scope,
		if (ps->p1d_cur_func >= 0 && (ps->tok->tag & TT_SWITCH) && !is_known_typedef(ps->tok)) {
			Token *p = skip_prep_dirs(tok_next(ps->tok));
			if (p && match_ch(p, '(') && tok_match(p)) {
				Token *body = skip_prep_dirs(tok_next(tok_match(p)));
				if (body && !match_ch(body, '{')) {
					uint32_t synth_sid = ps->p1d_braceless_next_sid++;
					if (synth_sid > UINT16_MAX)
						error_tok(ps->tok,
							  "too many scopes + braceless switches (>65535)");
					p1_alloc(P1K_SWITCH, (uint16_t)synth_sid, ps->tok);
					p1d_ensure_switch_cap(ps);
					ps->p1d_switch_stack[ps->p1d_switch_top] = synth_sid;
					Token *end = skip_one_stmt_impl(body, ps->skip_cache);
					ps->p1d_switch_end[ps->p1d_switch_top] = end ? tok_idx(end) : UINT32_MAX;
					ps->p1d_switch_top++;
				}
			}
		}

		if (is_else_or_do(ps->tok)) {
			ps->p1d_prev = ps->tok;
			ps->tok = tok_next(ps->tok);
			ps->at_stmt_start = true;
			ps->p1d_ctrl_pending = true;
			continue;
		}

		// Phase 1D: validate bare orelse in expression statements.
		if (ps->at_stmt_start && FEAT(F_ORELSE) && ps->p1d_cur_func >= 0 && ps->brace_depth > 0 &&
		    !(ps->tok->tag & (TT_IF | TT_LOOP | TT_SWITCH | TT_GOTO | TT_BREAK | TT_CONTINUE | TT_CASE |
				  TT_DEFAULT | TT_DEFER))) {
			if (ps->tok->tag & TT_RETURN) {
				Token *body = skip_noise(tok_next(ps->tok));
				Token *bare_oe = find_bare_orelse(body);
				if (bare_oe && !(tok_ann(bare_oe) & (P1_OE_BRACKET | P1_OE_DECL_INIT))) {
					/* `return orelse;` — identifier operand starts the expr.
					 * Do not walk back with WB_PAST_NOISE: it skips `(x)`, so
					 * `return (x) orelse 1` looked like `return orelse` and
					 * leaked the keyword to the backend. */
					if (body != bare_oe)
						error_tok(bare_oe,
							  "'orelse' cannot be used in a return expression; "
							  "assign to a temporary first");
				}
			} else {
				Token *bare_oe = find_bare_orelse(ps->tok);
				if (bare_oe && !(tok_ann(bare_oe) & (P1_OE_BRACKET | P1_OE_DECL_INIT)))
					p1d_validate_bare_orelse(ps->tok, bare_oe);
			}
		}

		/* Paren-led braceless bodies: `if (c) (x = get() orelse 0);`
		 * open at stmt-start, so the !at_stmt_start paren scan misses them. */
		if (ps->at_stmt_start && (FEAT(F_ORELSE) || FEAT(F_DEFER)) && match_ch(ps->tok, '(') &&
		    tok_match(ps->tok) && !is_stmt_expr_open(ps->tok))
			check_orelse_in_parens(ps->tok);

		ps->at_stmt_start = false;
		ps->p1d_prev = ps->tok;
		ps->tok = tok_next(ps->tok);
	}
#undef CUR_SID
#undef P1D_STMT_RESET
}

// Runs before Pass 2 — all CFG errors raised before any byte is emitted.

// Report a goto-skips-defer/decl error or warning.
static void cfg_report_goto(Token *bad, const char *msg, P1FuncEntry *label) {
	if (FEAT(F_WARN_SAFETY)) warn_tok(bad, msg, label->label.len, label->label.name);
	else
		error_tok(bad, msg, label->label.len, label->label.name);
}

static inline uint32_t decl_effective_close(const P1FuncEntry *d) {
	if (d->decl.body_close_idx > 0) return d->decl.body_close_idx;
	if (d->scope_id > 0 && d->scope_id < scope_tree_count) return scope_tree[d->scope_id].close_tok_idx;
	return 0;
}

static void cfg_error_if_looped_between(P1FuncEntry *ents,
					P1FuncEntry *g,
					P1FuncEntry *label,
					int *list,
					int lo,
					int hi,
					bool vla_only,
					const char *msg) {
	for (int di = lo; di < hi; di++) {
		P1FuncEntry *d = &ents[list[di]];
		if (vla_only && !d->decl.is_vla) continue;
		if (d->token_index >= g->token_index) continue;
		if (!scope_is_ancestor_or_self(d->scope_id, label->scope_id)) continue;
		if (!scope_is_ancestor_or_self(d->scope_id, g->scope_id)) continue;
		if (vla_only) {
			uint32_t vla_close = decl_effective_close(d);
			if (vla_close > 0 && vla_close <= g->token_index) continue;
		}
		error_tok(g->tok, msg, g->label.len, g->label.name);
		break;
	}
}

static void cfg_fill_assigned_first(P1FuncEntry *ents,
				    P1FuncEntry *label,
				    int *decl_list,
				    int decl_n,
				    uint8_t *out) {
	memset(out, 0, (size_t)decl_n);
	if (!FEAT(F_ZEROINIT) || decl_n <= 0) return;
	Token *t = &token_pool[label->token_index];
	while (t && t->kind != TK_EOF && !match_ch(t, ':')) t = tok_next(t);
	if (t) t = tok_next(t);
	int bd = 0;
	Token *first_name = NULL;
	while (t && t->kind != TK_EOF) {
		if (match_ch(t, '{')) {
			bd++;
			t = tok_next(t);
			continue;
		}
		if (match_ch(t, '}')) {
			if (bd == 0) break;
			bd--;
			t = tok_next(t);
			continue;
		}
		if (t->kind == TK_IDENT) {
			Token *n = tok_next(t);
			if (n && match_ch(n, '=')) {
				first_name = t;
				/* `x = x + 1` is not a defining first assign — RHS uses x. */
				int rd = 0;
				for (Token *s = tok_next(n); s && s->kind != TK_EOF; s = tok_next(s)) {
					if (match_ch(s, ';') && rd == 0) break;
					if (s->flags & TF_OPEN) {
						rd++;
						continue;
					}
					if (s->flags & TF_CLOSE) {
						if (rd) rd--;
						continue;
					}
					if (rd == 0 && s->kind == TK_IDENT && s->len == t->len &&
					    prism_memeq_runtime_sized(tok_loc(s), tok_loc(t), t->len)) {
						first_name = NULL;
						break;
					}
				}
			}
			break;
		}
		t = tok_next(t);
	}
	if (!first_name) return;
	for (int di = 0; di < decl_n; di++) {
		P1FuncEntry *d = &ents[decl_list[di]];
		if (d->decl.is_vla || d->decl.has_raw || d->decl.is_static_storage || d->decl.has_init)
			continue;
		Token *lname = d->tok;
		if (lname->len == first_name->len &&
		    prism_memeq_runtime_sized(tok_loc(lname), tok_loc(first_name), lname->len))
			out[di] = 1;
	}
}

static void cfg_check_range(P1FuncEntry *ents,
			    P1FuncEntry *g,
			    P1FuncEntry *label,
			    bool is_forward,
			    int *defer_list,
			    int defer_lo,
			    int defer_hi,
			    int *decl_list,
			    int decl_lo,
			    int decl_hi,
			    const uint8_t *label_assigned_first) {
	Token *bad_defer = NULL, *bad_decl = NULL;
	bool bad_decl_is_vla = false;
	if (FEAT(F_DEFER)) {
		for (int di = defer_lo; di < defer_hi; di++) {
			P1FuncEntry *d = &ents[defer_list[di]];
			// Scope must be an ancestor-or-self of the label's scope
			if (!scope_is_ancestor_or_self(d->scope_id, label->scope_id)) continue;
			if (!is_forward && scope_is_ancestor_or_self(d->scope_id, g->scope_id)) continue;
			// Defer's scope must still be open at the label position.
			if (d->scope_id > 0 && d->scope_id < scope_tree_count) {
				uint32_t close = scope_tree[d->scope_id].close_tok_idx;
				if (close < label->token_index) continue;
			}
			bad_defer = d->tok;
			break;
		}
	}

	// VLA skip is always a hard error (C99/C11 6.8.6.1p1) regardless of
	bool bad_decl_has_init = false;
	P1FuncEntry *first_vla = NULL, *first_init = NULL, *first_other = NULL;
	int first_other_di = -1;
	for (int di = decl_lo; di < decl_hi; di++) {
		P1FuncEntry *d = &ents[decl_list[di]];
		if (!scope_is_ancestor_or_self(d->scope_id, label->scope_id)) continue;
		if (!is_forward && scope_is_ancestor_or_self(d->scope_id, g->scope_id)) continue;
		uint32_t close = decl_effective_close(d);
		if (close > 0 && close < label->token_index) continue;
		if (d->decl.is_vla) {
			if (!first_vla) first_vla = d;
			continue;
		}
		if (d->decl.has_raw || d->decl.is_static_storage) continue;
		if (d->decl.has_init) {
			if (!first_init) first_init = d;
		} else if (!first_other) {
			first_other = d;
			first_other_di = di;
		}
	}
	if (first_vla) {
		bad_decl = first_vla->tok;
		bad_decl_is_vla = true;
	} else if (first_init) {
		bad_decl = first_init->tok;
		bad_decl_has_init = true;
	} else if (first_other && FEAT(F_ZEROINIT)) {
		bool assigned_first = false;
		if (label_assigned_first && first_other_di >= 0)
			assigned_first = label_assigned_first[first_other_di] != 0;
		else {
			Token *lname = first_other->tok;
			Token *t = &token_pool[label->token_index];
			while (t && t->kind != TK_EOF && !match_ch(t, ':')) t = tok_next(t);
			if (t) t = tok_next(t);
			int bd = 0;
			while (t && t->kind != TK_EOF) {
				if (match_ch(t, '{')) {
					bd++;
					t = tok_next(t);
					continue;
				}
				if (match_ch(t, '}')) {
					if (bd == 0) break;
					bd--;
					t = tok_next(t);
					continue;
				}
				if (t->kind == TK_IDENT && t->len == lname->len &&
				    prism_memeq_runtime_sized(tok_loc(t), tok_loc(lname), lname->len)) {
					Token *n = tok_next(t);
					if (n && match_ch(n, '=')) assigned_first = true;
					break;
				}
				t = tok_next(t);
			}
		}
		if (!assigned_first) bad_decl = first_other->tok;
	}

	if (bad_defer) cfg_report_goto(bad_defer, "goto '%.*s' would skip over this defer statement", label);
	if (bad_decl) {
		const char *msg;
		if (bad_decl_is_vla) msg = "goto '%.*s' would skip over this VLA declaration";
		else if (bad_decl_has_init)
			msg = "goto '%.*s' would skip over a declaration with an initializer "
			      "(undefined behavior if jumped into)";
		else
			msg = "goto '%.*s' would skip over this variable declaration "
			      "(bypasses initialization)";
		if (bad_decl_is_vla) error_tok(bad_decl, msg, label->label.len, label->label.name);
		else
			cfg_report_goto(bad_decl, msg, label);
	}
}

static void p1_verify_cfg(void) {
	for (int fi = 0; fi < func_meta_count; fi++) {
		FuncMeta *fm = &func_meta[fi];
		if (fm->entry_count == 0) continue;
		if (!FEAT(F_DEFER | F_ZEROINIT)) {
			P1FuncEntry *ents = &p1_entries[fm->entry_start];
			bool has_vla = false;
			for (int i = 0; i < fm->entry_count; i++)
				if (ents[i].kind == P1K_DECL && ents[i].decl.is_vla) {
					has_vla = true;
					break;
				}
			if (!has_vla) continue;
		}

		// Computed gotos cannot be verified statically: they could jump
		bool unverifiable_jump = fm->has_computed_goto || (fm->body_open->tag & TT_ASM);
		if (unverifiable_jump) {
			const char *jump_kind = fm->has_computed_goto ? "computed goto" : "asm goto";
			static const char fmt[] = "%s cannot be used in a function that "
						  "contains %s — the jump target cannot be "
						  "verified at compile time";
			P1FuncEntry *ents = &p1_entries[fm->entry_start];
			for (int i = 0; i < fm->entry_count; i++) {
				const char *blocker = NULL;
				if (ents[i].kind == P1K_DEFER && FEAT(F_DEFER)) blocker = "defer statements";
				else if (ents[i].kind == P1K_DECL && ents[i].decl.is_vla)
					blocker = "variable-length arrays";
				else if (ents[i].kind == P1K_DECL && FEAT(F_ZEROINIT) &&
					 !ents[i].decl.has_raw && !ents[i].decl.is_static_storage &&
					 !ents[i].decl.is_vla && !ents[i].decl.has_init)
					blocker = "zero-initialized declarations";
				if (blocker) error_tok(fm->body_open, fmt, jump_kind, blocker);
			}
		}

		if (FEAT(F_DEFER) && !fm->returns_void && !fm->ret_type_start) {
			P1FuncEntry *ents = &p1_entries[fm->entry_start];
			for (int i = 0; i < fm->entry_count; i++) {
				if (ents[i].kind == P1K_DEFER)
					error_tok(fm->body_open,
						  "defer in function with unresolvable return type; "
						  "use a named struct or typedef");
			}
		}

		// Allocated before arena mark so it persists in FuncMeta for Pass 2 O(1)
		// lookup.
		int cnt = fm->entry_count;
		if (cnt < 0) cnt = 0; // GCC VRP guard: entry_count is always ≥ 0
		int hash_sz = 64;
		while (hash_sz < cnt * 2) hash_sz <<= 1;
		int *label_hash = arena_alloc(&ctx->main_arena, (size_t)hash_sz * sizeof(int));
		memset(label_hash, 0xFF, (size_t)hash_sz * sizeof(int)); // -1 = empty
		int hash_mask = hash_sz - 1;
		ArenaMark mark = arena_mark(&ctx->main_arena);
		P1FuncEntry *ents = &p1_entries[fm->entry_start];
		int *defer_list = arena_alloc(&ctx->main_arena, (size_t)cnt * sizeof(int));
		int *decl_list = arena_alloc(&ctx->main_arena, (size_t)cnt * sizeof(int));
		int *wm_defer = arena_alloc(&ctx->main_arena, (size_t)cnt * sizeof(int));
		int *wm_decl = arena_alloc(&ctx->main_arena, (size_t)cnt * sizeof(int));
		uint8_t **af_cache = arena_alloc(&ctx->main_arena, (size_t)cnt * sizeof(uint8_t *));
		int defer_n = 0, decl_n = 0;
		for (int i = 0; i < cnt; i++) {
			if (ents[i].kind != P1K_LABEL) continue;
			uint32_t h = (uint32_t)fast_hash(ents[i].label.name, ents[i].label.len);
			for (int probe = 0; probe < hash_sz; probe++) {
				int slot = (h + probe) & hash_mask;
				if (label_hash[slot] < 0) {
					label_hash[slot] = i;
					break;
				}
				P1FuncEntry *existing = &ents[label_hash[slot]];
				if (existing->label.len == ents[i].label.len &&
				    prism_memeq_runtime_sized(existing->label.name, ents[i].label.name,
					     existing->label.len))
					error_tok(ents[i].tok,
						  "duplicate label '%.*s'",
						  ents[i].label.len,
						  ents[i].label.name);
			}
		}

		// Persist label hash in FuncMeta for O(1) lookup in Pass 2
		fm->label_hash = label_hash;
		fm->label_hash_mask = hash_mask;

		typedef struct {
			int idx, dm, cm, next;
		} FwdGoto;

		FwdGoto *fwd = arena_alloc(&ctx->main_arena, (size_t)cnt * sizeof(FwdGoto));
		int fwd_n = 0;
		int *fwd_hash_tbl = arena_alloc(&ctx->main_arena, (size_t)hash_sz * sizeof(int));
		memset(fwd_hash_tbl, 0xFF, (size_t)hash_sz * sizeof(int)); // -1 = empty

		/* Switch watermarks indexed by (scope_id - sw_min_sid): a function's
		 * switch scope_ids span its own scope range, so sizing to the SPAN is
		 * O(function scopes). Sizing to the absolute max scope_id was O(global
		 * scope_id) per function → O(n^2) memory across the TU. */
		int sw_max_sid = 0, sw_min_sid = INT_MAX;
		for (int i = 0; i < cnt; i++)
			if (ents[i].kind == P1K_SWITCH) {
				if (ents[i].scope_id > sw_max_sid) sw_max_sid = ents[i].scope_id;
				if (ents[i].scope_id < sw_min_sid) sw_min_sid = ents[i].scope_id;
			}
		int sw_sz = sw_max_sid >= sw_min_sid ? sw_max_sid - sw_min_sid + 1 : 0;
		int *sw_defer_wm = NULL, *sw_decl_wm = NULL;
		if (sw_sz > 0 && sw_sz <= 65536) {
			sw_defer_wm = arena_alloc(&ctx->main_arena, (size_t)sw_sz * sizeof(int));
			sw_decl_wm = arena_alloc(&ctx->main_arena, (size_t)sw_sz * sizeof(int));
		}

		for (int i = 0; i < cnt; i++) {
			wm_defer[i] = defer_n;
			wm_decl[i] = decl_n;
			switch (ents[i].kind) {
			case P1K_DEFER: defer_list[defer_n++] = i; break;
			case P1K_DECL: decl_list[decl_n++] = i; break;
			case P1K_LABEL: {
				uint32_t lh = (uint32_t)fast_hash(ents[i].label.name, ents[i].label.len);
				int fh_slot = lh & hash_mask;
				int prev_fi = -1;
				int fi = fwd_hash_tbl[fh_slot];
				while (fi >= 0) {
					int next_fi = fwd[fi].next;
					P1FuncEntry *g = &ents[fwd[fi].idx];
					if (g->label.len == ents[i].label.len &&
					    prism_memeq_runtime_sized(g->label.name, ents[i].label.name, g->label.len)) {
						uint16_t label_se =
						    scope_stmt_expr_ancestor(ents[i].scope_id);
						if (label_se != 0 &&
						    !scope_is_ancestor_or_self(label_se, g->scope_id))
							error_tok(
							    g->tok,
							    "goto '%.*s' jumps into a statement expression "
							    "(jumping into ({...}) is undefined behavior)",
							    g->label.len,
							    g->label.name);
						g->label.exits =
						    scope_block_exits(g->scope_id, ents[i].scope_id);
						if (!af_cache[i] && decl_n > 0) {
							af_cache[i] = arena_alloc(&ctx->main_arena,
										  (size_t)decl_n);
							cfg_fill_assigned_first(
							    ents, &ents[i], decl_list, decl_n, af_cache[i]);
						}
						cfg_check_range(ents,
								g,
								&ents[i],
								/*is_forward=*/true,
								defer_list,
								fwd[fi].dm,
								defer_n,
								decl_list,
								fwd[fi].cm,
								decl_n,
								af_cache[i]);
						if (prev_fi < 0) fwd_hash_tbl[fh_slot] = next_fi;
						else
							fwd[prev_fi].next = next_fi;
						fi = next_fi;
						continue;
					}
					prev_fi = fi;
					fi = next_fi;
				}
				break;
			}
			case P1K_GOTO: {
				P1FuncEntry *g = &ents[i];
				uint32_t h = (uint32_t)fast_hash(g->label.name, g->label.len);
				int li = -1;
				for (int probe = 0; probe < hash_sz; probe++) {
					int slot = (h + probe) & hash_mask;
					if (label_hash[slot] < 0) break;
					P1FuncEntry *cand = &ents[label_hash[slot]];
					if (cand->label.len == g->label.len &&
					    prism_memeq_runtime_sized(cand->label.name, g->label.name, g->label.len)) {
						li = label_hash[slot];
						break;
					}
				}

				if (li < 0 || li > i) {
					// Forward goto (label not yet seen, or found after current pos)
					int fi = fwd_n++;
					int fh_slot = h & hash_mask;
					fwd[fi] = (FwdGoto){.idx = i,
							    .dm = defer_n,
							    .cm = decl_n,
							    .next = fwd_hash_tbl[fh_slot]};
					fwd_hash_tbl[fh_slot] = fi;
				} else {
					uint16_t label_se = scope_stmt_expr_ancestor(ents[li].scope_id);
					if (label_se != 0 &&
					    !scope_is_ancestor_or_self(label_se, g->scope_id))
						error_tok(g->tok,
							  "goto '%.*s' jumps into a statement expression "
							  "(jumping into ({...}) is undefined behavior)",
							  g->label.len,
							  g->label.name);
					g->label.exits = scope_block_exits(g->scope_id, ents[li].scope_id);
					if (!af_cache[li] && wm_decl[li] > 0) {
						af_cache[li] =
						    arena_alloc(&ctx->main_arena, (size_t)wm_decl[li]);
						cfg_fill_assigned_first(
						    ents, &ents[li], decl_list, wm_decl[li], af_cache[li]);
					}
					cfg_check_range(ents,
							g,
							&ents[li],
							/*is_forward=*/false,
							defer_list,
							0,
							wm_defer[li],
							decl_list,
							0,
							wm_decl[li],
							af_cache[li]);
					if (FEAT(F_DEFER))
						cfg_error_if_looped_between(
						    ents,
						    g,
						    &ents[li],
						    defer_list,
						    wm_defer[li],
						    defer_n,
						    false,
						    "goto '%.*s' loops over a defer statement; "
						    "lexical defer does not execute dynamically and "
						    "the defer body will only execute once at scope exit");
					cfg_error_if_looped_between(
					    ents,
					    g,
					    &ents[li],
					    decl_list,
					    wm_decl[li],
					    decl_n,
					    true,
					    "goto '%.*s' loops over a variable-length array "
					    "declaration; each iteration allocates a new VLA "
					    "without freeing the previous one, causing "
					    "unbounded stack growth");
				}
				break;
			}
			case P1K_SWITCH: {
				int si = (int)ents[i].scope_id - sw_min_sid;
				if (sw_defer_wm && si >= 0 && si < sw_sz) {
					sw_defer_wm[si] = defer_n;
					sw_decl_wm[si] = decl_n;
				}
				break;
			}
			case P1K_CASE: {
				uint16_t sw_sid = ents[i].kase.switch_scope_id;
				// (Phase 1D records sw_sid=0 only when p1d_switch_top==0).
				if (sw_sid == 0)
					error_tok(ents[i].tok,
						  "case/default label outside any switch statement");
				{
					uint16_t case_se = scope_stmt_expr_ancestor(ents[i].scope_id);
					if (case_se != 0 && sw_sid < scope_tree_count &&
					    !scope_is_ancestor_or_self(case_se, sw_sid))
						error_tok(ents[i].tok,
							  "case/default label inside a statement expression "
							  "(jumping into ({...}) is undefined behavior)");
				}

				int sw_i = (int)sw_sid - sw_min_sid;
				if (!sw_defer_wm || sw_i < 0 || sw_i >= sw_sz) break;
				int sw_dm = sw_defer_wm[sw_i];
				int sw_cm = sw_decl_wm[sw_i];
				if (FEAT(F_DEFER)) {
					for (int di = sw_dm; di < defer_n; di++) {
						P1FuncEntry *d = &ents[defer_list[di]];
						if (!scope_is_ancestor_or_self(d->scope_id, ents[i].scope_id))
							continue;
						if (d->scope_id > 0 && d->scope_id < scope_tree_count &&
						    scope_tree[d->scope_id].close_tok_idx <
							ents[i].token_index)
							continue;
						error_tok(d->tok,
							  "defer skipped by switch fallthrough at %s:%d",
							  tok_file(ents[i].tok)->name,
							  tok_line_no(ents[i].tok));
					}
				}

				// Decl bypass: VLA skip is always fatal (C99/C11 6.8.6.1);
				for (int di = sw_cm; di < decl_n; di++) {
					P1FuncEntry *d = &ents[decl_list[di]];
					if (!scope_is_ancestor_or_self(d->scope_id, ents[i].scope_id))
						continue;
					{
						uint32_t close = decl_effective_close(d);
						if (close > 0 && close < ents[i].token_index) continue;
					}
					if (d->decl.is_vla)
						error_tok(ents[i].tok,
							  "case/default label may bypass VLA declaration");
					if (d->decl.has_raw || d->decl.is_static_storage) continue;
					if (d->decl.has_init)
						SAFETY_DIAG(ents[i].tok,
							    "case/default label may bypass declaration with "
							    "initializer (undefined if jumped into)");
					if (!FEAT(F_ZEROINIT)) continue;
					SAFETY_DIAG(
					    ents[i].tok,
					    "case/default label inside a nested block within a switch "
					    "may bypass zero-initialization (move the label to the "
					    "switch body or wrap in its own block)");
				}
				break;
			}
			} // switch
		} // sweep

		{
			for (int s = 0; s <= hash_mask; s++) {
				int fi = fwd_hash_tbl[s];
				if (fi >= 0) {
					error_tok(ents[fwd[fi].idx].tok,
						  "goto target label '%.*s' not found in scope",
						  ents[fwd[fi].idx].label.len,
						  ents[fwd[fi].idx].label.name);
					break;
				}
			}
		}

		arena_restore(&ctx->main_arena, mark);
	} // per-function
}

// One pool walk: typedef bits, GNU attr-arg spans, optional __prism_bchk sentinel.
static bool p1_annotate_pool(bool want_bchk_sentinel) {
	bool already_has_bchk = false;
	for (uint32_t i = 1; i < token_count; i++) {
		Token *t = &token_pool[i];
		if (want_bchk_sentinel && !already_has_bchk && t->kind == TK_IDENT && t->len == 12 &&
		    t->ch0 == '_' && prism_memeq_static(tok_loc(t), "__prism_bchk", 12))
			already_has_bchk = true;
		if (t->tag & TT_ATTR) {
			Token *open = tok_next(t);
			if (open && match_ch(open, '(') && (open->flags & TF_OPEN) && tok_match(open)) {
				uint32_t oi = tok_idx(open), ci = tok_idx(tok_match(open));
				for (uint32_t j = oi; j <= ci; j++) token_pool[j].ann |= P1_IN_ATTR_ARGS;
				/* Do not skip the span: typedef/shadow uses inside attribute
				 * arguments (e.g. stmt-exprs in aligned(...)) still need
				 * P1_HAS_ENTRY so Pass 2 is_defer_kw / is_known_typedef work. */
			}
		}
		if (!is_identifier_like(t)) continue;
		TypedefEntry *e = typedef_lookup(t);
		if (!e) continue;
		tok_ann(t) |= P1_HAS_ENTRY;
		if (!e->is_enum_const && !e->is_shadow && !e->is_vla_var && !e->is_struct_tag)
			tok_ann(t) |= P1_IS_TYPEDEF;
	}
	p1_typedef_annotated = true;
	return already_has_bchk;
}

// --- Pass 2: Main Transpilation Loop ---

static PRISM_HOT int transpile_tokens(Token *tok, FILE *fp) {
	out_fp = fp;
	out_buf_pos = 0;
	out_total_flushed = 0;
	if (FEAT(F_FLATTEN)) {
		emit_system_header_diag_push();
		out_char('\n');
	}

	reset_transpiler_state();
	typedef_table_reset();
	system_includes_reset();
	const char *cc = ctx->extra_compiler ? ctx->extra_compiler : PRISM_DEFAULT_CC;
	is_msvc_cached = cc_is_msvc(cc);
	// Phase 1A: Build scope tree (full-depth walk of all tokens)
	p1_build_scope_tree(tok);
	// Phase 1B: Full-depth typedef + enum registration (all scopes)
	p1_full_depth_prescan(tok);
	td_build_timelines();
	ba_build_timelines();
	p1_verify_cfg();
	bool already_has_bchk = p1_annotate_pool(FEAT(F_BOUNDS_CHECK));
	if (!FEAT(F_FLATTEN)) {
		collect_system_includes();
		emit_system_includes();
	}

	// MSVC lacks __builtin_expect / __builtin_trap — fall back to __debugbreak +
	// abort. We do NOT #include <stddef.h> / <stdlib.h>: in flatten mode the
	// output is call site. MSVC gets `unsigned __int64` (matches LLP64 size_t on
	// x64).
	if (FEAT(F_BOUNDS_CHECK)) {
		// Tag every '[' inside sizeof/_Alignof/typeof/offsetof so Pass 2
		p1_mark_uneval_brackets();
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
	Token *pending_unreachable_tok = NULL;
	bool pending_case_colon = false;
	const uint32_t feat = ctx->features;
#undef FEAT
#define FEAT(f) (feat & (f))
#ifdef PRISM_DEBUG
	/* Termination watchdog for the Pass 2 walk (see Phase 1 twin). */
	uint64_t p2_wd_steps = 0;
	const uint64_t p2_wd_budget = 256ull * (uint64_t)token_count + 65536ull;
#endif
	while (tok->kind != TK_EOF) {
#ifdef PRISM_DEBUG
		if (++p2_wd_steps > p2_wd_budget)
			error_tok(tok,
				  "internal: Pass 2 progress watchdog tripped "
				  "(possible non-termination); please report");
#endif
		/* Precomputed system-include predicate avoids a per-token cold-file lookup. */
		if (!FEAT(F_FLATTEN) && (tok->flags & TF_SYS_SKIP)) {
			if (next_func_idx < func_meta_count &&
			    func_meta[next_func_idx].body_open == tok)
				next_func_idx++;
			tok = tok_next(tok);
			continue;
		}

		Token *next;
		uint32_t tag = tok->tag;
		if (match_ch(tok, '?')) ternary_depth++;

#define DISPATCH(handler)                                                                                    \
	{                                                                                                    \
		next = handler(tok);                                                                         \
		if (next) {                                                                                  \
			tok = next;                                                                          \
			continue;                                                                            \
		}                                                                                            \
	}

		if (__builtin_expect(!tag && !ctx->at_stmt_start, 1)) {
			if (__builtin_expect((tok->flags & TF_RAW) && !is_known_typedef(tok), 0))
				goto slow_path;
			if (__builtin_expect(FEAT(F_BOUNDS_CHECK) && ctx->raw_block_depth == 0 &&
						 tok->ch0 == '*',
					     0)) {
				Token *bc_da = try_bounds_check_deref_add(tok);
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
		Token *next = emit_gnu_label_decl(tok);
		if (next) {
			tok = next;
			continue;
		}
	}

		if (ctx->at_stmt_start && !(tag & TT_STRUCTURAL) &&
		    (!ctrl_state.pending || in_for_init() || ctrl_state.parens_just_closed)) {
			/* Strip statement-form `raw { ... }` keyword; body brace is
			 * annotated P1_RAW_BLOCK and suppresses transforms. */
			if ((tok->flags & TF_RAW)) {
				Token *after = skip_noise(tok_next(tok));
				bool typedef_kw_prefix =
				    is_known_typedef(tok) && after &&
				    (is_type_keyword(after) || is_known_typedef(after) ||
				     (after->tag &
				      (TT_QUALIFIER | TT_SUE | TT_STORAGE | TT_INLINE | TT_TYPEDEF)) ||
				     (after->flags & TF_RAW));
				if ((!is_known_typedef(tok) || typedef_kw_prefix) && after &&
				    match_ch(after, '{')) {
					tok = after;
					tag = tok->tag;
					/* Fall through: structural `{` handled below. */
					goto after_stmt_start_decl;
				}
			}
			next = try_zero_init_decl(tok);
			if (next) {
				tok = next;
				ctx->at_stmt_start = true;
				continue;
			}

			check_enum_typedef_defer_shadow(tok);
			if (FEAT(F_ORELSE) && ctx->raw_block_depth == 0 && ctx->block_depth > 0 &&
			    !in_struct_body() && !(tok->tag & (TT_NON_EXPR_STMT | TT_DEFER))) {
				Token *body = skip_stmt_prefixes(tok);
				Token *orelse_tok = find_bare_orelse(body);
				if (orelse_tok) {
					tok = emit_through(tok, body);

					bool brace_wrap = ctrl_state.pending && ctrl_state.parens_just_closed;
					if (brace_wrap) ctrl_reset();
					Token *next = emit_bare_orelse_impl(tok, NULL, true, brace_wrap);
					if (next) {
						tok = next;
						end_statement_after_semicolon();
						continue;
					}

					tok = emit_orelse_condition_wrap(tok, orelse_tok);
					tok = emit_orelse_action(tok, NULL, false, false, NULL);
					OUT_LIT(" }");
					continue;
				}
			}
		}
	after_stmt_start_decl:
		ctx->at_stmt_start = false;
		if (tag & TT_NORETURN_FN) {
			uint32_t ti = tok_idx(tok);
			if (!(ti >= 1 && (token_pool[ti - 1].tag & TT_MEMBER))) {
				if (FEAT(F_DEFER) && has_active_defers())
					fprintf(
					    stderr,
					    "%s:%d: warning: '%.*s' referenced with active defers (defers "
					    "will not run if called)\n",
					    tok_file(tok)->name,
					    tok_line_no(tok),
					    tok->len,
					    tok_loc(tok));
				if (FEAT(F_AUTO_UNREACHABLE) && ctx->raw_block_depth == 0 &&
				    ctx->block_depth > 0 && !in_ctrl_paren() &&
				    !(ctrl_state.pending && ctrl_state.parens_just_closed)) {
					Token *nr = try_detect_noreturn_call(tok);
					if (nr) pending_unreachable_tok = nr;
				}
			}
		}

		if (tag) {
			{
				if (ctx->raw_block_depth == 0) {
					Token *n = try_handle_defer_flow_kw(tok);
					if (n) {
						tok = n;
						continue;
					}
				}
			}
			arm_ctrl_pending_from_tag(tok, tag);
			if ((tag & TT_GENERIC) && !in_generic()) {
				tok = emit_generic_open(tok);
				continue;
			}
		} // end if (tag)

		if (tag & (TT_CASE | TT_DEFAULT)) pending_case_colon = true;
		track_generic_token(tok);
		if (tag & TT_SUE) // struct/union/enum body
			DISPATCH(handle_sue_body);
		if (tag & TT_STRUCTURAL) {
			if (match_ch(tok, '{')) {
				if (ctx->block_depth == 0) {
					if (FEAT(F_DEFER) && next_func_idx < func_meta_count &&
					    func_meta[next_func_idx].body_open == tok) {
						FuncMeta *fm = &func_meta[next_func_idx];
						current_func_idx = next_func_idx++;
						goto_entry_cursor = 0;
						if (fm->ret_type_start) {
							ctx->func_ret_type_start = fm->ret_type_start;
							ctx->func_ret_type_end = fm->ret_type_end;
							ctx->func_ret_type_suffix_start =
							    fm->ret_type_suffix_start;
							ctx->func_ret_type_suffix_end =
							    fm->ret_type_suffix_end;
						} else
							clear_func_ret_type();
					} else
						clear_func_ret_type();
				}
				tok = handle_open_brace(tok);
				continue;
			}
			if (match_ch(tok, '}')) {
				tok = handle_close_brace(tok);
				if (ctx->block_depth == 0) current_func_idx = -1;
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

		if (__builtin_expect(tok->kind == TK_PREP_DIR, 0)) {
			tok = emit_advance(tok);
			ctx->at_stmt_start = true;
			continue;
		}

		track_common_token_state(tok);
		/* Bounds before bracket-orelse — same ordering as emit_statements /
		 * walk_balanced. Otherwise `return a[i orelse 0]` lowers the
		 * index ternary but skips __prism_bchk (v1 hook-order bug). */
		if (ctx->raw_block_depth == 0) {
			Token *bc = try_bounds_checks(tok);
			if (bc) {
				tok = bc;
				continue;
			}
			Token *next = try_orelse_expr_rewrites(tok);
			if (next) {
				tok = next;
				continue;
			}
		}

#ifdef PRISM_DEBUG
		if (__builtin_expect(FEAT(F_ORELSE) && is_orelse_keyword(tok), 0))
			error_tok(tok, ERR_ORELSE_STMT_LEVEL);
#endif

		tok = emit_advance(tok);
	}

	if (FEAT(F_FLATTEN)) {
		out_char('\n');
		emit_system_header_diag_pop();
	}
#undef FEAT
#define FEAT(f) (ctx->features & (f))

	out_close();
	free_source_defines();
	tokenizer_teardown(false);
	return 1;
}

static Token *preprocess_and_tokenize(char *input_file, double *pp_ms, double *tok_ms) {
	double t0 = prism_now_ms();
	char *pp_buf = preprocess_with_cc(input_file);
	double t1 = prism_now_ms();
	if (pp_ms) *pp_ms = t1 - t0;
	if (!pp_buf) {
		fprintf(stderr, "Preprocessing failed for: %s\n", input_file);
		return NULL;
	}
	double t2 = prism_now_ms();
	Token *tok = tokenize_buffer(input_file, pp_buf);
	double t3 = prism_now_ms();
	if (tok_ms) *tok_ms = t3 - t2;
	if (!tok) {
		fprintf(stderr, "Failed to tokenize preprocessed output\n");
		tokenizer_teardown(false);
	}
	return tok;
}

static int transpile_to_fp(char *input_file, FILE *fp) {
	ensure_keyword_cache();
	double t0 = prism_now_ms();
	double pp_ms = 0.0, tok_ms = 0.0;
	Token *tok = preprocess_and_tokenize(input_file, &pp_ms, &tok_ms);
	if (!tok) {
		fclose(fp);
		return 0;
	}

	double t1 = prism_now_ms();
	int ok = transpile_tokens(tok, fp);
	double t2 = prism_now_ms();
	if (prism_profile) {
		fprintf(stderr,
			"[prism-prof] file=%s preprocess=%.3fms tokenize=%.3fms "
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
	typedef_table_reset();
	free_source_defines();
	tokenizer_teardown(false);
	ctx->scope_depth = 0;
	ctx->block_depth = 0;
	ctx->aggregate_member_nest = 0;
	system_includes_reset();
	in_defer_emit = false;
	goto_entry_cursor = 0;
	ctrl_reset();
	ctrl_save_depth = 0;
	current_func_idx = -1;
	if (out_fp) {
		out_flush();
		fclose(out_fp);
		out_fp = NULL;
	}
}

/* --prism-verify: per-compile translation-validation certificate for the
 * defer/orelse safety theorem.
 *
 * After emitting output #1 for `orig_input`, the ENTIRE pipeline runs on
 * output #1 a second time (preprocess, tokenize, all Phase 1 analyses + CFG
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
 * Why not byte-equality: `prism(prism(x))` is deliberately NOT byte-equal to
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
static char *verify_read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len < 0) {
		fclose(f);
		return NULL;
	}
	char *buf = malloc((size_t)len + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[len] = '\0';
	fclose(f);
	return buf;
}

/* Count whole-word occurrences of `kw` at identifier boundaries, but ONLY in
 * CODE — string literals, char literals, and comments are skipped.  The leak
 * check asks "did an operator-position keyword survive to the backend"; the
 * transpiler's own error-message strings ("'orelse' cannot be used …") and a
 * `{"defer", …}` keyword-table entry are not operator keywords and their count
 * is not guaranteed stable across re-preprocessing/header-flattening on every
 * platform (this counter previously counted them, producing a spurious Linux
 * "leak" on the self-referential test harness).  A single scanner tracks
 * string/char/comment state with escape handling. */
static long verify_count_kw(const char *s, const char *kw) {
	long n = 0;
	size_t kl = strlen(kw);
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
		if (strncmp(p, kw, kl) == 0) {
			char before = (p == s) ? '\0' : p[-1];
			char after = p[kl];
			int lb = !((before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') ||
				   (before >= '0' && before <= '9') || before == '_');
			int rb = !((after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') ||
				   (after >= '0' && after <= '9') || after == '_');
			if (lb && rb) {
				n++;
				p += kl;
				continue;
			}
		}
		p++;
	}
	return n;
}

static int verify_transpiled_output(char *orig_input, char *out1_path) {
	prism_in_verify = true;
	const char **saved_dep = ctx->dep_flags;
	int saved_dep_n = ctx->dep_flags_count;
	ctx->dep_flags = NULL; /* never regenerate .d files for the verify pass */
	ctx->dep_flags_count = 0;
	prism_reset();

	char tmp2[PATH_MAX];
	int ok = 0;
	char diag[256] = "re-transpile of emitted C failed (output is not valid re-parseable C)";
	int fd = make_temp_file_registered(tmp2, sizeof(tmp2), NULL, 2, out1_path);
	if (fd >= 0) {
		close(fd);
		uint32_t saved_features = ctx->features;
		ctx->features &= ~(uint32_t)(F_ZEROINIT | F_AUTO_UNREACHABLE | F_AUTO_STATIC);
		ctx->features |= F_WARN_SAFETY;
		int retrans_ok = transpile(out1_path, tmp2);
		ctx->features = saved_features;
		if (retrans_ok) {
			char *o1 = verify_read_file(out1_path);
			char *o2 = verify_read_file(tmp2);
			if (o1 && o2) {
				long oe1 = verify_count_kw(o1, "orelse"), oe2 = verify_count_kw(o2, "orelse");
				long df1 = verify_count_kw(o1, "defer"), df2 = verify_count_kw(o2, "defer");
				if (oe2 < oe1 || df2 < df1) {
					snprintf(diag, sizeof(diag),
						 "operator keyword leaked to output: orelse %ld->%ld, "
						 "defer %ld->%ld (re-transpile lowered a keyword pass 1 "
						 "left behind)",
						 oe1, oe2, df1, df2);
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
	ctx->dep_flags = saved_dep;
	ctx->dep_flags_count = saved_dep_n;
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
	if (!ctx) return;
	if (out_fp) {
		out_flush();
		fclose(out_fp);
		out_fp = NULL;
	}
	out_buf_pos = 0;
	tokenizer_teardown(true);
	memset(&typedef_table, 0, sizeof(typedef_table));
	memset(&p1_shadow_map, 0, sizeof(p1_shadow_map));
	memset(&p1_func_proto_map, 0, sizeof(p1_func_proto_map));
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
	goto_entry_cursor = 0;
	current_func_idx = -1;
	free(sos_do_if_save);
	sos_do_if_save = NULL;
	free(sos_do_tn_save);
	sos_do_tn_save = NULL;
	free(sos_do_snap_start);
	sos_do_snap_start = NULL;
	sos_do_cap = 0;
	free(sos_do_snap_buf);
	sos_do_snap_buf = NULL;
	sos_snap_cap = 0;
	free(sos_if_trail_snap);
	sos_if_trail_snap = NULL;
	sos_if_cap = 0;
	free(ctx);
	ctx = NULL;
}

static void apply_features(PrismFeatures features) {
	ctx->features = features_to_bits(features);
	ctx->extra_compiler = features.compiler;
	ctx->extra_include_paths = features.include_paths;
	ctx->extra_include_count = features.include_count;
	ctx->extra_defines = features.defines;
	ctx->extra_define_count = features.define_count;
	ctx->extra_compiler_flags = features.compiler_flags;
	ctx->extra_compiler_flags_count = features.compiler_flags_count;
	ctx->extra_force_includes = features.force_includes;
	ctx->extra_force_include_count = features.force_include_count;
}

#ifdef PRISM_LIB_MODE
static void error_recovery_init(void) {
	ctx->error_msg[0] = '\0';
	ctx->error_line = 0;
	ctx->error_col = 0;
	ctx->error_jmp_set = true;
}

static PrismResult error_recovery_result(void) {
	ctx->error_jmp_set = false;
	PrismResult r = {.status = PRISM_ERR_SYNTAX,
			 .error_msg = strdup(ctx->error_msg[0] ? ctx->error_msg : "Unknown error"),
			 .error_line = ctx->error_line,
			 .error_col = ctx->error_col};
	// IMPORTANT: fclose must precede free — POSIX open_memstream only
	if (out_fp) {
		fclose(out_fp);
		out_fp = NULL;
	}
	if (ctx->active_membuf) {
		free(ctx->active_membuf);
		ctx->active_membuf = NULL;
	}
	prism_reset();
	return r;
}
#endif

static PrismResult transpile_to_result(Token *tok) {
	PrismResult result = {0};
	size_t memlen = 0;
#ifdef PRISM_LIB_MODE
	ctx->active_membuf = NULL;
	FILE *fp = open_memstream(&ctx->active_membuf, &memlen);
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
		result.output = ctx->active_membuf;
#else
		result.output = membuf;
#endif
		result.output_len = memlen;
		result.status = PRISM_OK;
	} else {
#ifdef PRISM_LIB_MODE
		free(ctx->active_membuf);
#else
		free(membuf);
#endif
		result.status = PRISM_ERR_SYNTAX;
		result.error_msg = strdup("Transpilation failed");
	}
#ifdef PRISM_LIB_MODE
	ctx->active_membuf = NULL;
#endif
	return result;
}

PRISM_API PrismResult prism_transpile_file(const char *input_file, PrismFeatures features) {
	prism_ctx_init();
	PrismResult result = {0};

#ifdef PRISM_LIB_MODE
	error_recovery_init();
	if (setjmp(ctx->error_jmp) != 0) return error_recovery_result();
#endif

	apply_features(features);
	ensure_keyword_cache();
	Token *tok;
	char *pp_buf = preprocess_with_cc((char *)input_file);
	if (!pp_buf) {
		result.status = PRISM_ERR_IO;
		result.error_msg = strdup("Preprocessing failed");
		goto cleanup;
	}

	tok = tokenize_buffer((char *)input_file, pp_buf);
	if (!tok) {
		result.status = PRISM_ERR_SYNTAX;
		result.error_msg = strdup("Failed to tokenize");
		tokenizer_teardown(false);
		goto cleanup;
	}

	result = transpile_to_result(tok);

cleanup:
#ifdef PRISM_LIB_MODE
	ctx->error_jmp_set = false;
#endif
	return result;
}

#ifdef PRISM_LIB_MODE
PRISM_API
PrismResult prism_transpile_source(const char *source, const char *filename, PrismFeatures features) {
	prism_ctx_init();
	PrismResult result = {0};
	if (!source) {
		result.status = PRISM_ERR_IO;
		result.error_msg = strdup("source is NULL");
		return result;
	}

	const char *fname = filename ? filename : "<source>";
	error_recovery_init();
	if (setjmp(ctx->error_jmp) != 0) return error_recovery_result();
	apply_features(features);
	ensure_keyword_cache();
	Token *tok;
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
	tok = tokenize_buffer((char *)fname, buf);
	if (!tok) {
		result.status = PRISM_ERR_SYNTAX;
		result.error_msg = strdup("Failed to tokenize");
		tokenizer_teardown(false);
		goto src_cleanup;
	}

	result = transpile_to_result(tok);

src_cleanup:
	ctx->error_jmp_set = false;
	return result;
}
#endif // PRISM_LIB_MODE

/* Grow *(arr) with sizeof(*arr) — portable, no __typeof__ (MSVC shim was
 * hardcoded to const char * and would under-allocate for any other element). */
#define CLI_PUSH(arr, cnt, cap, item)                                                                        \
	do {                                                                                                 \
		VEC_ENSURE_REALLOC((arr), (cnt) + 1, (cap), 16);                                             \
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
	VEC_ENSURE_REALLOC(*owned, *owned_count + 1, *owned_cap, 16);
	(*owned)[(*owned_count)++] = dup;
	VEC_ENSURE_REALLOC(*out, *count + 1, *cap, 16);
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
		fprintf(stderr, "error: response file nesting too deep: %s\n", path);
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
	VEC_ENSURE_REALLOC(out, count + 1, cap, 16);
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
		fprintf(stderr, "error: failed to expand response files\n");
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
			if (!strcmp(a, "-fno-defer")) {
				cli.features.defer = false;
				continue;
			}
			if (!strcmp(a, "-fno-zeroinit")) {
				cli.features.zeroinit = false;
				continue;
			}
			if (!strcmp(a, "-fno-orelse")) {
				cli.features.orelse = false;
				continue;
			}
			if (!strcmp(a, "-fno-line-directives")) {
				cli.features.line_directives = false;
				continue;
			}
			if (!strcmp(a, "-fno-safety")) {
				cli.features.warn_safety = true;
				continue;
			}
			if (!strcmp(a, "-fflatten-headers")) {
				cli.features.flatten_headers = true;
				continue;
			}
			if (!strcmp(a, "-fno-flatten-headers")) {
				cli.features.flatten_headers = false;
				continue;
			}
			if (!strcmp(a, "-fno-auto-unreachable")) {
				cli.features.auto_unreachable = false;
				continue;
			}
			if (!strcmp(a, "-fno-auto-static")) {
				cli.features.auto_static = false;
				continue;
			}
			if (!strcmp(a, "-fbounds-check")) {
				cli.features.bounds_check = true;
				continue;
			}
			if (!strcmp(a, "-fno-bounds-check")) {
				cli.features.bounds_check = false;
				continue;
			}
			if (!strcmp(a, "-fno-link-pragma")) {
				cli.no_link_pragma = true;
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
	Token *tok = preprocess_and_tokenize(input_file, &pp_ms, &tok_ms);
	if (!tok) return -1;
	int pipefd[2];
	if (pipe(pipefd) == -1) {
		perror("pipe");
		tokenizer_teardown(false);
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
		tokenizer_teardown(false);
		return -1;
	}

	FILE *fp = fdopen(pipefd[1], "w");
	if (!fp) {
		close(pipefd[1]);
		tokenizer_teardown(false);
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
			"[prism-prof] file=%s preprocess=%.3fms tokenize=%.3fms "
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

static bool ensure_install_dir(const char *p) {
	char dir[PATH_MAX];
	strncpy(dir, p, PATH_MAX - 1);
	dir[PATH_MAX - 1] = '\0';
	char *sep = strrchr(dir, '/');
	if (sep) *sep = '\0';
	struct stat st;
	if (stat(dir, &st) == 0) return true;
	mkdir(dir, 0755);
	return stat(dir, &st) == 0;
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
	if (strcmp(self_path, install_path) == 0) {
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
			old_path[0] = '\0'; // rename failed, will fall through to error
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
		fprintf(stderr, "[prism] Failed to install to %s (error %lu).\n", install_path, err);
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
	       "local/param array subscripts\n"
	       "  -fno-link-pragma       Ignore #pragma link directives in source\n"
	       "  --prism-cc=<compiler>  Use specific compiler\n"
	       "  --prism-verbose        Show commands\n"
	       "  --prism-prof           Print per-phase timing breakdown\n"
	       "  --prism-verify         Translation validation: re-transpile emitted C,\n"
	       "                         require a fixed point (also: PRISM_VERIFY env)\n"
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
	       "    name:     plain name (e.g. `Cocoa`, `m`) — macOS => -framework, "
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
					"%s:%d:%d: error: %s\n",
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
	int status = 0;
	collect_link_pragmas(cli);
	ctx->extra_compiler_flags = cli->cc_args;
	ctx->extra_compiler_flags_count = cli->cc_arg_count;
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

	use_linemarkers = FEAT(F_FLATTEN) && !clang && !msvc;
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
		if (FEAT(F_FLATTEN) && !clang) args[argc++] = "-fpreprocessed";
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
			if (FEAT(F_FLATTEN) && !clang) args[argc++] = "-fno-preprocessed";
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
			fprintf(stderr, "error: cannot specify -o when generating multiple output files\n");
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
				    .use_preprocessed = FEAT(F_FLATTEN) && !clang && !msvc,
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
			    .use_preprocessed = FEAT(F_FLATTEN) && !clang && !msvc,
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
#ifdef _WIN32
	win32_utf8_argv(&argc, &argv);
#endif
	signal(SIGINT, signal_cleanup_handler);
	signal(SIGTERM, signal_cleanup_handler);
	signal(SIGPIPE,
	       SIG_IGN); // no-op on Windows (SIGPIPE defined but never raised)
	int status = 0;
	prism_ctx_init();
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
	ctx->features = features_to_bits(cli.features);
	ctx->extra_compiler = get_real_cc(cli.cc);
	ctx->extra_compiler_flags = cli.cc_args;
	ctx->extra_compiler_flags_count = cli.cc_arg_count;
	ctx->dep_flags = cli.dep_args;
	ctx->dep_flags_count = cli.dep_arg_count;
	cli_inject_dep_mt_from_output(&cli);
	ctx->dep_flags = cli.dep_args;
	ctx->dep_flags_count = cli.dep_arg_count;
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

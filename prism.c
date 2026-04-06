#define PRISM_VERSION "1.0.5"

#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <signal.h>
#include <spawn.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>

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
#define PRISM_API static
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
#include <sys/types.h>
#include <sys/sysctl.h>
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
#define TYPEDEF_ADD_IDX(call, t) do { int _pre = typedef_table.count; call; \
	if (typedef_table.count > _pre) \
		typedef_table.entries[typedef_table.count - 1].token_index = tok_idx(t); } while(0)

typedef struct {
	// Preprocessor configuration (optional - can be left NULL/0 for defaults)
	const char *compiler;
	const char **include_paths; // -I paths
	const char **defines; // -D macros
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
} PrismFeatures;

typedef enum {
	PRISM_OK = 0,
	PRISM_ERR_SYNTAX,
	PRISM_ERR_SEMANTIC,
	PRISM_ERR_IO,
} PrismStatus;

typedef struct {
	char *output; // transpiled C (caller frees with prism_free)
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
	SCOPE_BLOCK,       // { ... } block scope
	SCOPE_INIT,        // = { ... } initializer brace (no block_depth increment)
	SCOPE_FOR_PAREN,   // for( ... ) — first ';' ends init, not stmt
	SCOPE_CTRL_PAREN,  // if/while/switch( ... )
	SCOPE_GENERIC,     // _Generic( ... )
	SCOPE_TERNARY,     // ? ... : — popped on matching ':'
} ScopeKind;

static inline bool is_brace_scope(ScopeKind k) {
	return k == SCOPE_BLOCK || k == SCOPE_INIT;
}

typedef struct {
	int defer_start_idx;
	uint8_t kind;
	bool is_loop : 1;
	bool is_switch : 1;
	bool is_struct : 1;
	bool is_stmt_expr : 1;
	bool is_ctrl_se : 1;   // stmt-expr inside ctrl parens (ctrl_state saved on ctrl_save_stack)
} ScopeNode;

// Per-function metadata collected during Pass 1.
typedef struct {
	Token *body_open;              // The '{' token
	Token *ret_type_start;         // First token of return type
	Token *ret_type_end;           // Function name token (exclusive)
	Token *ret_type_suffix_start;  // For complex declarators: closing ')'
	Token *ret_type_suffix_end;    // Token after suffix (exclusive)
	bool returns_void;
	bool has_computed_goto;    // Function contains a computed goto (*ptr)
	int entry_start;           // Start index into p1_entries[] for this function
	int entry_count;           // Number of P1FuncEntry items for this function
	HashMap defer_name_set; // Exact set of identifier names in defer bodies
	int *label_hash;           // Open-addressing hash table: name → entry index (-1=empty)
	int label_hash_mask;       // Power-of-2 mask for label_hash probing
} FuncMeta;

// Pass 1 shadow entry: a variable declaration that shadows a typedef name.
typedef struct {
	char *name;
	int len;
	uint16_t scope_id;       // Scope where the shadow is declared
	uint32_t token_index;    // Pool index of the shadowing declaration
	int prev_index;          // Chain to previous shadow for same name (-1 = none)
} P1ShadowEntry;

// Phase 1D: per-function entry for labels, gotos, defers, decls, switches, cases.
// Stored in a single combined flat array; FuncMeta records start_idx + count.
typedef enum { P1K_LABEL, P1K_GOTO, P1K_DEFER, P1K_DECL, P1K_SWITCH, P1K_CASE } P1EntryKind;
typedef struct {
	P1EntryKind kind;
	uint16_t scope_id;
	uint32_t token_index;       // tok_idx for sorting in Phase 2
	Token *tok;
	union {
		struct { char *name; int len; int exits; } label; // P1K_LABEL, P1K_GOTO (exits: pre-computed scope exits for P1K_GOTO)
		struct { bool has_init; bool is_vla; bool has_raw; bool is_static_storage;
			 uint32_t body_close_idx; } decl; // P1K_DECL
		struct { uint16_t switch_scope_id; } kase;   // P1K_CASE
	};
} P1FuncEntry;

// Typed accessors for void* fields in PrismContext
#define func_meta        ((FuncMeta *)ctx->p1_func_meta)
#define func_meta_count  (ctx->p1_func_meta_count)
#define func_meta_cap    (ctx->p1_func_meta_cap)
#define p1_shadows       ((P1ShadowEntry *)ctx->p1_shadow_entries)
#define p1_shadow_count  (ctx->p1_shadow_count)
#define p1_shadow_cap    (ctx->p1_shadow_cap)
#define p1_shadow_map    (ctx->p1_shadow_map)
#define p1_entries       ((P1FuncEntry *)ctx->p1_func_entries)
#define p1_entry_count   (ctx->p1_func_entry_count)
#define p1_entry_cap     (ctx->p1_func_entry_cap)
static inline P1FuncEntry *p1_alloc(int knd, uint16_t sid, Token *t) {
	ARENA_ENSURE_CAP(&ctx->main_arena, ctx->p1_func_entries,
			 p1_entry_count, p1_entry_cap, 256, P1FuncEntry);
	P1FuncEntry *e = &p1_entries[p1_entry_count++];
	*e = (P1FuncEntry){.kind=knd, .scope_id=sid,
	 .token_index=tok_idx(t), .tok=t};
	return e;
}

typedef enum { CLI_DEFAULT, CLI_RUN, CLI_EMIT, CLI_INSTALL } CliMode;
typedef enum { CLI_ACT_NONE, CLI_ACT_HELP, CLI_ACT_VERSION } CliAction;

typedef struct {
	PrismFeatures features;
	const char **sources;
	const char **cc_args;
	const char **dep_args;  // dependency-generation flags (routed to preprocessor only)
	const char *output;
	const char *cc;
	int source_count, source_cap;
	int cc_arg_count, cc_arg_cap;
	int dep_arg_count, dep_arg_cap;
	CliMode mode;
	CliAction action;
	bool verbose;
	bool compile_only;
	bool passthrough;
} Cli;

// --- Signal & Temp File Cleanup ---

extern char **environ;
static char **cached_clean_env = NULL;
static volatile sig_atomic_t signal_temp_registered = 0;
static char signal_temp_path[PATH_MAX];

// Transpiled temp file tracking for signal cleanup.
// Fixed-size array; entries are PATH_MAX-sized C strings.
// 256 * PATH_MAX bytes total (typically 1 MB on POSIX).
#define SIGNAL_TEMPS_MAX 256
static char signal_temps[SIGNAL_TEMPS_MAX][PATH_MAX];
static volatile sig_atomic_t signal_temps_ready[SIGNAL_TEMPS_MAX];
static volatile sig_atomic_t signal_temps_count = 0;

#ifndef signal_temp_store // Windows: defined in windows.c
#define signal_temp_store(val) __atomic_store_n(&signal_temp_registered, (val), __ATOMIC_RELEASE)
#define signal_temp_load()     __atomic_load_n(&signal_temp_registered, __ATOMIC_ACQUIRE)
#define signal_temps_store(val) __atomic_store_n(&signal_temps_count, (val), __ATOMIC_RELEASE)
#define signal_temps_load()     __atomic_load_n(&signal_temps_count, __ATOMIC_ACQUIRE)
#define signal_temps_cas(expected, desired) \
	__atomic_compare_exchange_n(&signal_temps_count, (expected), (desired), \
				    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
#define cached_env_load()       __atomic_load_n(&cached_clean_env, __ATOMIC_ACQUIRE)
#define cached_env_store(val)   __atomic_store_n(&cached_clean_env, (val), __ATOMIC_RELEASE)
#define signal_temps_ready_store(idx, val) __atomic_store_n(&signal_temps_ready[(idx)], (val), __ATOMIC_RELEASE)
#define signal_temps_ready_load(idx)       __atomic_load_n(&signal_temps_ready[(idx)], __ATOMIC_ACQUIRE)
#endif

static void signal_temps_register(const char *path) {
	size_t len = strlen(path);
	if (len >= PATH_MAX) return;
	sig_atomic_t n;
	do {
		n = signal_temps_load();
		if (n >= SIGNAL_TEMPS_MAX) {
			fprintf(stderr, "prism: warning: temp file tracking full (%d); "
				"'%s' won't be cleaned on signal\n",
				SIGNAL_TEMPS_MAX, path);
			return;
		}
	} while (!signal_temps_cas(&n, n + 1));
	memcpy(signal_temps[n], path, len + 1);
	signal_temps_ready_store(n, 1);
}

static void __attribute__((unused)) signal_temps_unregister(const char *path) {
	int n = signal_temps_load();
	for (int i = 0; i < n; i++) {
		if (signal_temps_ready_load(i) && strcmp(signal_temps[i], path) == 0) {
			signal_temps_ready_store(i, 0);
			signal_temps[i][0] = '\0';
			return;
		}
	}
}

static void signal_temps_clear(void) {
	sig_atomic_t n = signal_temps_load();
	for (int i = 0; i < n; i++) {
		signal_temps_ready_store(i, 0);
		memset(signal_temps[i], 0, PATH_MAX);
	}
	signal_temps_store(0);
}

// --- Thread-Local Transpiler State ---

static PRISM_THREAD_LOCAL char **system_include_list; // Ordered list of includes
static PRISM_THREAD_LOCAL int system_include_capacity = 0;

// Token emission - user-space buffered output for minimal syscall overhead
static PRISM_THREAD_LOCAL FILE *out_fp;
static PRISM_THREAD_LOCAL Token *last_emitted = NULL;

static PRISM_THREAD_LOCAL char out_buf[OUT_BUF_SIZE];
static PRISM_THREAD_LOCAL int out_buf_pos = 0;
static PRISM_THREAD_LOCAL int64_t out_total_flushed = 0;
static PRISM_THREAD_LOCAL bool use_linemarkers = false; // true = GCC linemarker "# N", false = C99 "#line N"

// Ring buffer tracking (absolute_byte_offset, token_pool_index) before each
// emit_tok() call.  Used by the _Generic member-injection rewrite to rewind
// the output buffer past an already-emitted member-access prefix.
// Absolute offsets survive out_flush() boundary crossings.
#define EMIT_SAVE_RING_SIZE 128
#define EMIT_SAVE_RING_MASK (EMIT_SAVE_RING_SIZE - 1)
static PRISM_THREAD_LOCAL struct { int64_t abs_pos; uint32_t tpi; } emit_save_ring[EMIT_SAVE_RING_SIZE];
static PRISM_THREAD_LOCAL int emit_save_idx = 0;

typedef struct {
	bool pending;
	bool pending_for_paren;
	bool parens_just_closed;
	int brace_depth;
} CtrlState;

static PRISM_THREAD_LOCAL ScopeNode *scope_stack = NULL;
static PRISM_THREAD_LOCAL int scope_stack_cap = 0;
static PRISM_THREAD_LOCAL DeferEntry *defer_stack = NULL;
static PRISM_THREAD_LOCAL int defer_stack_cap = 0;
static PRISM_THREAD_LOCAL int defer_count = 0;
static PRISM_THREAD_LOCAL CtrlState ctrl_state;
static PRISM_THREAD_LOCAL CtrlState *ctrl_save_stack = NULL; // saved ctrl_state for stmt-expr inside ctrl parens
static PRISM_THREAD_LOCAL int ctrl_save_depth = 0;
static PRISM_THREAD_LOCAL int ctrl_save_cap = 0;
static PRISM_THREAD_LOCAL int current_func_idx = -1; // Index into func_meta[] for the function being emitted
static PRISM_THREAD_LOCAL int goto_entry_cursor = 0; // Cursor into entries[] for next P1K_GOTO lookup
static PRISM_THREAD_LOCAL bool p1_file_has_orelse;   // true if any TT_ORELSE token exists in token stream
static PRISM_THREAD_LOCAL HashMap p1_func_proto_map;  // file-scope ident followed by '(' → (void*)1

// Track variables that shadow a name captured by a defer in an enclosing scope.
// The shadow is only dangerous if a control-flow exit pastes the defer while the
// shadowing variable is still in scope.  We record the shadow at declaration time
// and check at every exit point that actually emits defers.
typedef struct {
	char *name;
	int len;
	int block_depth;   // scope depth where the shadowing variable was declared
	Token *var_tok;    // for error reporting
	int defer_idx;     // which defer it conflicts with
} DeferShadow;

static PRISM_THREAD_LOCAL DeferShadow *defer_shadows = NULL;
static PRISM_THREAD_LOCAL int defer_shadow_count = 0;
static PRISM_THREAD_LOCAL int defer_shadow_cap = 0;

// MSVC /D define buffers (dynamically allocated, freed in prism_thread_cleanup)
static PRISM_THREAD_LOCAL char **pp_define_bufs = NULL;
static PRISM_THREAD_LOCAL int pp_define_bufs_cap = 0;

// Forward declarations (only for functions used before their definition)
static void check_defer_shadow_at_exit(DeferEmitMode mode, int stop_depth);
static Token *emit_expr_to_semicolon(Token *tok);
static Token *emit_orelse_action(Token *tok, Token *var_name, bool has_const, bool has_volatile, Token *stop_comma);
static Token *emit_return_body(Token *tok, Token *stop);
static Token *try_zero_init_decl(Token *tok);
static Token *find_bare_orelse(Token *tok);
static Token *decl_noise(Token *tok, bool emit);
static Token *walk_balanced(Token *tok, bool emit);
static Token *walk_balanced_orelse(Token *tok);
static void emit_noise_between_raws(Token *first_raw, Token *last_raw);
static inline void out_char(char c);
static inline void out_str(const char *s, int len);
#define OUT_TOK(t) out_str(tok_loc(t), (t)->len)
#define skip_balanced(tok, o, c) skip_balanced_group(tok)
static bool cc_is_msvc(const char *cc);
static inline void ctrl_reset(void);

typedef struct { int scope_depth; Token *tok; } P1LabelResult;
static P1LabelResult p1_label_find(Token *tok, int current_func_idx);

// Emit space-separated token range [start, end). First token has no leading space.
static inline void emit_token_range(Token *start, Token *end) {
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = tok_next(t)) {
		if (t != start) out_char(' ');
		OUT_TOK(t);
	}
}

static inline void clear_func_ret_type(void) {
	ctx->func_ret_type_start = ctx->func_ret_type_end = NULL;
	ctx->func_ret_type_suffix_start = ctx->func_ret_type_suffix_end = NULL;
}

// Phase 1C: record a variable declaration that shadows a typedef name.
static void p1_register_shadow(Token *t, uint16_t scope_id, int brace_depth) {
	ARENA_ENSURE_CAP(&ctx->main_arena, ctx->p1_shadow_entries,
			 p1_shadow_count, p1_shadow_cap, 64, P1ShadowEntry);
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

static void p1_register_param_shadows(Token *open, Token *close,
				      uint16_t scope_id, int brace_depth,
				      bool check_vla);

static void reset_transpiler_state(void) {
	ctx->scope_depth = 0;
	ctx->block_depth = 0;
	ctx->last_line_no = 0;
	ctx->ret_counter = 0;
	clear_func_ret_type();
	ctx->last_filename = NULL;
	ctx->last_system_header = false;
	ctx->at_stmt_start = true;
	ctrl_reset();
	ctrl_save_depth = 0;
	last_emitted = NULL;
	emit_save_idx = 0;
	out_total_flushed = 0;
	current_func_idx = -1;
	p1_typedef_annotated = false;
	p1_file_has_orelse = false;
	hashmap_zero(&p1_func_proto_map);

	// Clear arena-allocated arrays — prevents dangling pointers after arena_reset.
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
	hashmap_zero(&p1_shadow_map);
	ctx->p1_func_entries = NULL;
	p1_entry_count = 0;
	p1_entry_cap = 0;
}

// --- Output Buffer ---

PRISM_API PrismFeatures prism_defaults(void) {
	return (PrismFeatures){.defer = true,
			       .zeroinit = true,
			       .line_directives = true,
			       .flatten_headers = true,
			       .orelse = true,
			       .auto_unreachable = true};
}

static uint32_t features_to_bits(PrismFeatures f) {
	return (f.defer ? F_DEFER : 0) | (f.zeroinit ? F_ZEROINIT : 0) |
	       (f.line_directives ? F_LINE_DIR : 0) | (f.warn_safety ? F_WARN_SAFETY : 0) |
	       (f.flatten_headers ? F_FLATTEN : 0) | (f.orelse ? F_ORELSE : 0) |
	       (f.auto_unreachable ? F_AUTO_UNREACHABLE : 0);
}

static const char *get_tmp_dir(void) {
	static PRISM_THREAD_LOCAL char buf[PATH_MAX];
#ifdef _WIN32
	// Use _wgetenv to avoid ANSI codepage corruption of non-ASCII TEMP paths.
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

static inline void out_char(char c) {
	if (__builtin_expect(out_buf_pos >= OUT_BUF_SIZE, 0)) out_flush();
	out_buf[out_buf_pos++] = c;
}

static inline void out_str(const char *s, int len) {
	if (__builtin_expect(len <= 0, 0)) return;
	if (__builtin_expect(out_buf_pos + len >= OUT_BUF_SIZE, 0)) {
		if (len >= OUT_BUF_SIZE) { out_flush(); fwrite(s, 1, len, out_fp); return; }
		out_flush();
	}
	memcpy(out_buf + out_buf_pos, s, len);
	out_buf_pos += len;
}

#define OUT_LIT(s) out_str(s, sizeof(s) - 1)

// Check if the effective compiler is MSVC, falling back to PRISM_DEFAULT_CC
// when no compiler is explicitly set (e.g. prism_defaults() leaves it NULL).
static inline bool target_is_msvc(void) {
	const char *cc = ctx->extra_compiler ? ctx->extra_compiler : PRISM_DEFAULT_CC;
	return cc_is_msvc(cc);
}
#define EMIT_UNREACHABLE() do { \
	if (target_is_msvc()) OUT_LIT(" __assume(0);"); \
	else OUT_LIT(" __builtin_unreachable();"); } while(0)
#define TT_NON_EXPR_STMT (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO | \
	TT_CASE | TT_DEFAULT | TT_IF | TT_LOOP | TT_SWITCH | TT_STORAGE | TT_TYPEDEF)

// Emit __typeof__ (GNU) or typeof (C23/MSVC).
// MSVC does not support __typeof__; C23 typeof is available under /std:clatest.
static inline void emit_typeof_keyword(void) {
	if (target_is_msvc())
		OUT_LIT("typeof");
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

static void out_line(int line_no, const char *file) {
	if (use_linemarkers) OUT_LIT("# ");
	else OUT_LIT("#line ");

	out_uint(line_no);
	OUT_LIT(" \"");
	for (const char *p = file; *p; p++) {
		char c = *p;
		if (c == '\\') c = '/'; // normalize backslashes (MSVC C4129, harmless on POSIX)
		if (c == '"' || c == '\\') out_char('\\');
		out_char(c);
	}
	OUT_LIT("\"\n");
}

// Collect system headers by detecting actual #include entries (not macro expansions)
// --- System Includes ---

static void collect_system_includes(void) {
	HashMap include_map = {0};
	for (int i = 0; i < ctx->input_file_count; i++) {
		File *f = ctx->input_files[i];
		if (!f->is_system || !f->is_include_entry || !f->name) continue;
		const char *base = path_basename(f->name);
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
	free(include_map.buckets);
}

static void emit_system_header_diag_push(void) {
	if (target_is_msvc()) return; // MSVC doesn't understand #pragma GCC
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
	if (target_is_msvc()) return;
	OUT_LIT("#pragma GCC diagnostic pop\n");
}

// Emit a single define from a "NAME=VALUE" or "NAME" string (guarded by #ifndef)
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

// Emit collected #include directives with necessary feature test macros
// Emit defines that were consumed by cc -E and need to be re-emitted
// in non-flatten mode.  Called unconditionally (even without system includes)
// so ABI-altering macros like _FILE_OFFSET_BITS are preserved.
static void emit_consumed_defines(void) {
	bool any = ctx->extra_define_count > 0 || ctx->source_define_count > 0;

	if (!any) {
		for (int i = 0; i < ctx->extra_compiler_flags_count; i++) {
			const char *f = ctx->extra_compiler_flags[i];
			if (f[0] == '-' && f[1] == 'D') { any = true; break; }
		}
	}

	if (!any && ctx->system_include_count == 0) return;

	// Emit user-specified API defines (take priority over built-in feature test macros)
	for (int i = 0; i < ctx->extra_define_count; i++)
		emit_define_guarded(ctx->extra_defines[i]);

	// Emit CLI -D flags from extra_compiler_flags (these are passed to cc -E
	// but lost from un-flattened output unless re-emitted here)
	for (int i = 0; i < ctx->extra_compiler_flags_count; i++) {
		const char *f = ctx->extra_compiler_flags[i];
		if (f[0] == '-' && f[1] == 'D') {
			// -DFOO or -DFOO=bar (no space)
			if (f[2]) emit_define_guarded(f + 2);
			// -D FOO (space-separated): next arg is the define
			else if (i + 1 < ctx->extra_compiler_flags_count)
				emit_define_guarded(ctx->extra_compiler_flags[++i]);
		}
	}

	// Emit source-file defines that were consumed by the preprocessor
	for (int i = 0; i < ctx->source_define_count; i++) {
		const char *guard = ctx->source_define_guards ? ctx->source_define_guards[i] : NULL;
		if (guard) out_str(guard, strlen(guard));
		emit_define_guarded(ctx->source_defines[i]);
		if (guard) {
			// Count opening directives (#if/#ifdef/#ifndef) to determine
			// how many #endif lines to emit.
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
			for (int d = 0; d < depth; d++)
				OUT_LIT("#endif\n");
		}
	}

	// Emit built-in feature test macros (guarded to not override user defines)
	OUT_LIT("#if !defined(_WIN32)\n"
		"#ifndef _POSIX_C_SOURCE\n#define _POSIX_C_SOURCE 200809L\n#endif\n"
		"#ifndef _GNU_SOURCE\n#define _GNU_SOURCE\n#endif\n"
		"#endif\n\n");
}

static void emit_system_includes(void) {
	emit_consumed_defines();

	if (ctx->system_include_count == 0) return;

	emit_system_header_diag_push();

	for (int i = 0; i < ctx->system_include_count; i++) {
		OUT_LIT("#include \"");
		out_str(system_include_list[i], strlen(system_include_list[i]));
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

// --- Scope Stack ---

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

// End a statement at ';'.  Reset pending braceless-control state.
static void end_statement_after_semicolon(void) {
	ctx->at_stmt_start = true;
	if (ctrl_state.pending && !in_ctrl_paren()) {
		// normally be cleaned up by scope_pop on a braced body's '}'.
		while (defer_shadow_count > 0 &&
		       defer_shadows[defer_shadow_count - 1].block_depth > ctx->block_depth)
			defer_shadow_count--;
		ctrl_reset();
	}
}

static void scope_push_kind(ScopeKind kind) {
	ENSURE_ARRAY_CAP(scope_stack, ctx->scope_depth + 1, scope_stack_cap, 256, ScopeNode);
	ScopeNode *s = &scope_stack[ctx->scope_depth];
	*s = (ScopeNode){.kind = kind};
	s->defer_start_idx = defer_count;
	if (kind == SCOPE_BLOCK)
		ctx->block_depth++;
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
			ctx->block_depth--;
		}
	}
}

static void defer_add(Token *defer_keyword, Token *start, Token *end) {
	if (ctx->block_depth <= 0) error_tok(start, "defer outside of any scope");
	ENSURE_ARRAY_CAP(defer_stack, defer_count + 1, defer_stack_cap, 64, DeferEntry);
	defer_stack[defer_count++] = (DeferEntry){start, end, defer_keyword};
}

// --- Token Emission ---

// Cold path: emit C23 float suffix normalization
static bool __attribute__((noinline)) emit_tok_special(Token *tok) {
	const char *replacement;
	int suffix_len = get_extended_float_suffix(tok_loc(tok), tok->len, &replacement);
	if (suffix_len > 0) {
		out_str(tok_loc(tok), tok->len - suffix_len);
		if (replacement) out_str(replacement, strlen(replacement));
		return true;
	}
	return false;
}

static void emit_tok(Token *tok) {
	// Save pre-emission absolute output position for _Generic member-injection rewind.
	emit_save_ring[emit_save_idx & EMIT_SAVE_RING_MASK] =
		(typeof(emit_save_ring[0])){out_total_flushed + out_buf_pos, tok_idx(tok)};
	emit_save_idx++;

	TokenCold *c = tok_cold(tok);
	File *f = (c->file_idx < (uint32_t)ctx->input_file_count)
		  ? ctx->input_files[c->file_idx] : ctx->current_file;
	char *loc = f->contents + c->loc_offset;

	if (__builtin_expect(!FEAT(F_FLATTEN) && f->is_system && f->is_include_entry, 0)) return;

	bool need_line = false;
	char *tok_fname = NULL;
	int line_no = 0;

	if (FEAT(F_LINE_DIR)) {
		line_no = c->line_no;
		tok_fname = f->name;
		need_line = (ctx->last_filename != tok_fname) || (f->is_system != ctx->last_system_header) ||
			    (line_no != ctx->last_line_no && line_no != ctx->last_line_no + 1);
	}

	if (tok_at_bol(tok) || need_line) out_char('\n');
	else if ((tok->flags & TF_HAS_SPACE) || needs_space(last_emitted, tok))
		out_char(' ');

	if (need_line) {
		out_line(line_no, tok_fname);
		ctx->last_filename = tok_fname;
		ctx->last_system_header = f->is_system;
	}

	ctx->last_line_no = line_no;

	// Handle preprocessor directives (e.g., #pragma) - emit verbatim
	if (__builtin_expect(tok->kind == TK_PREP_DIR, 0)) {
		if (!tok_at_bol(tok)) out_char('\n');
		out_str(loc, tok->len);
		last_emitted = tok;
		return;
	}

	// Handle rare special case: C23 float suffix normalization
	if (__builtin_expect(tok->flags & TF_IS_FLOAT, 0) && emit_tok_special(tok)) {
		last_emitted = tok;
		return;
	}

	out_str(loc, tok->len);
	last_emitted = tok;
}

// Flags for emit_range_ex: control how tokens are emitted in a range.
#define ER_SKIP_PREP    1  // Skip TK_PREP_DIR tokens
#define ER_BALANCED     2  // Use walk_balanced for paren/bracket groups (not just stmt-expr)

static void emit_range_ex(Token *start, Token *end, int flags) {
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = tok_next(t)) {
		if ((flags & ER_SKIP_PREP) && t->kind == TK_PREP_DIR) continue;
		if ((flags & ER_BALANCED) && (t->flags & TF_OPEN) && match_set(t, CH('(') | CH('['))) {
			walk_balanced(t, true); t = tok_match(t); continue;
		}
		if (is_stmt_expr_open(t) && tok_match(t)) { walk_balanced(t, true); t = tok_match(t); continue; }
		emit_tok(t);
	}
}

#define emit_range(start, end)          emit_range_ex(start, end, 0)
#define emit_range_no_prep(start, end)  emit_range_ex(start, end, ER_SKIP_PREP)
#define emit_balanced_range(start, end) emit_range_ex(start, end, ER_SKIP_PREP | ER_BALANCED)

static Token *emit_bare_orelse_impl(Token *t, Token *end, bool comma_term, bool brace_wrap);
static Token *emit_deferred_orelse(Token *t, Token *end);
static void emit_deferred_range(Token *start, Token *end);

// --- Defer Emission & Shadow Checking ---

// Defer emission architecture:
// All control-flow exits (return, break, continue, goto) emit pending defer
// bodies through this single function.  The five call paths are:
//   emit_return_body   → emit_all_defers()      [DEFER_ALL]
//   emit_break/continue→ emit_defers(BREAK/CONT) [DEFER_BREAK/CONTINUE]
//   emit_goto_defer    → emit_goto_defers(depth) [DEFER_TO_DEPTH]
//   handle_control_exit_defer → delegates to return/break/continue above
//   emit_orelse_action → delegates to return/break/continue/goto above
// Defer 2.0 (goto-patch model) would replace the scope walk below with
// label→block patching; this function is the single seam for that change.
static void emit_defers_ex(DeferEmitMode mode, int stop_depth) {
	if (ctx->block_depth <= 0) return;

	// For control-flow exits (not end-of-scope), verify no live shadow conflicts.
	if (mode != DEFER_SCOPE)
		check_defer_shadow_at_exit(mode, stop_depth);

	int current_defer = defer_count - 1;
	int curr_bd = ctx->block_depth;
	for (int d = ctx->scope_depth - 1; d >= 0; d--) {
		if (scope_stack[d].kind != SCOPE_BLOCK) continue;
		if (mode == DEFER_TO_DEPTH && curr_bd <= stop_depth) break;

		ScopeNode *scope = &scope_stack[d];
		for (int i = current_defer; i >= scope->defer_start_idx; i--) {
			out_char(' ');
			emit_deferred_range(defer_stack[i].stmt, defer_stack[i].end);
			out_char(';');
		}
		current_defer = scope->defer_start_idx - 1;
		curr_bd--;

		if (mode == DEFER_SCOPE) break;
		if (mode == DEFER_BREAK && (scope->is_loop || scope->is_switch)) break;
		if (mode == DEFER_CONTINUE && scope->is_loop) break;
	}
}

static bool has_defers_for(DeferEmitMode mode, int stop_depth) {
	int curr_bd = ctx->block_depth;
	for (int d = ctx->scope_depth - 1; d >= 0; d--) {
		if (scope_stack[d].kind != SCOPE_BLOCK) continue;
		if (mode == DEFER_TO_DEPTH && curr_bd <= stop_depth) break;
		if (defer_count > scope_stack[d].defer_start_idx) return true;
		curr_bd--;
		if (mode == DEFER_BREAK && (scope_stack[d].is_loop || scope_stack[d].is_switch)) return false;
		if (mode == DEFER_CONTINUE && scope_stack[d].is_loop) return false;
	}
	return false;
}

// Returns true if `name` (length nlen) is referenced (not locally declared) in [body, body_end).
static bool defer_body_refs_name(Token *body, Token *body_end, const char *name, int nlen) {
	Token *prev = NULL;
	int bd = 0, pd = 0, se = 0, se_brace[8];
	int decl_depth = -1;
	bool in_decl = false, was_in_decl = false;
	int decl_bd = 0, for_init_pd = -1;
	bool for_name_hid = false;
	uint32_t for_body_end_idx = 0;
	Token *for_header_open = NULL;
	for (Token *t = body; t && t != body_end && t->kind != TK_EOF;
	     prev = t, t = tok_next(t)) {
		if (for_name_hid && for_body_end_idx && tok_idx(t) > for_body_end_idx)
			for_name_hid = false;
		if (match_ch(t, '{')) {
			if (prev && match_ch(prev, '(') && se < 8) se_brace[se++] = bd;
			bd++; continue;
		}
		if (match_ch(t, '}')) {
			bd--;
			if (se > 0 && bd == se_brace[se - 1]) se--;
			if (decl_depth >= 0 && bd < decl_depth) decl_depth = -1;
			if (in_decl && bd < decl_bd) in_decl = false;
			continue;
		}
		if (match_set(t, CH('(') | CH('['))) {
			if (match_ch(t, '(') && prev && prev->kind == TK_KEYWORD &&
			    ((prev->tag & TT_LOOP) || (prev->tag & TT_IF) || (prev->tag & TT_SWITCH))) {
				for_init_pd = pd + 1;
				for_header_open = t;
			}
			pd++; continue;
		}
		if (match_set(t, CH(')') | CH(']'))) {
			pd--;
			if (for_init_pd >= 0 && pd < for_init_pd) for_init_pd = -1;
			continue;
		}
		if (match_ch(t, ';')) {
			in_decl = false; was_in_decl = false;
			if (for_init_pd >= 0 && pd == for_init_pd) for_init_pd = -1;
			continue;
		}
		if (pd == se && match_ch(t, '=')) { in_decl = false; continue; }
		if (pd == se && match_ch(t, ',') && was_in_decl && bd == decl_bd) { in_decl = true; continue; }
		if ((((bd > 1 || se > 0) && pd == se) ||
		     (for_init_pd >= 0 && pd == for_init_pd)) &&
		    (is_type_keyword(t) || (t->tag & (TT_QUALIFIER | TT_SUE | TT_STORAGE | TT_TYPEDEF)))) {
			in_decl = true; was_in_decl = true; decl_bd = bd; continue;
		}
		if ((t->kind == TK_IDENT || t->kind == TK_KEYWORD) &&
		    !(prev && (prev->tag & TT_MEMBER)) &&
		    t->len == nlen && !memcmp(tok_loc(t), name, nlen)) {
			if (for_name_hid) continue;
			if ((bd > 1 || se > 0) && decl_depth >= 0 && bd >= decl_depth) continue;
			if ((bd > 1 || se > 0) && in_decl && pd == se) { decl_depth = bd; continue; }
			if (for_init_pd >= 0 && in_decl) {
				for_name_hid = true;
				if (for_header_open && tok_match(for_header_open)) {
					Token *fbe = skip_one_stmt(tok_next(tok_match(for_header_open)));
					for_body_end_idx = fbe ? tok_idx(fbe) : 0;
				} else for_body_end_idx = 0;
				continue;
			}
			return true;
		}
	}
	return false;
}

// Record that declaring var_name shadows a name captured by a defer in an
// enclosing scope.  The actual error is deferred until a control-flow exit
// (return/goto/break/continue) pastes the defer while the shadow is live.
// Shadowing is safe when the inner variable goes out of scope before any
// such exit (normal block end always pops the shadow first).
static void check_defer_var_shadow(Token *var_name) {
	if (!FEAT(F_DEFER) || defer_count == 0) return;
	ScopeNode *blk = scope_block_top();
	if (!blk) return;
	int outer_defer_end = blk->defer_start_idx;
	if (in_for_init() && outer_defer_end <= 0)
		outer_defer_end = defer_count;
	int same_block_start = blk->defer_start_idx;
	if (outer_defer_end <= 0 && same_block_start >= defer_count) return;
	char *name = tok_loc(var_name);
	int nlen = var_name->len;
	if (current_func_idx >= 0 &&
	    !hashmap_get(&func_meta[current_func_idx].defer_name_set, name, nlen))
		return;
	for (int i = 0; i < defer_count; i++) {
		if (i >= outer_defer_end && i < same_block_start) continue;
		uint32_t var_idx = tok_idx(var_name);
		uint32_t stmt_idx = tok_idx(defer_stack[i].stmt);
		uint32_t end_idx = defer_stack[i].end ? tok_idx(defer_stack[i].end) : UINT32_MAX;
		if (var_idx >= stmt_idx && var_idx < end_idx) continue;
		if (!defer_body_refs_name(defer_stack[i].stmt, defer_stack[i].end, name, nlen))
			continue;
		if (defer_shadow_count >= defer_shadow_cap) {
			int new_cap = defer_shadow_cap ? defer_shadow_cap * 2 : 64;
			void *tmp = realloc(defer_shadows, new_cap * sizeof(*defer_shadows));
			if (!tmp) error("out of memory");
			defer_shadows = tmp;
			defer_shadow_cap = new_cap;
		}
		if (i >= same_block_start && !in_for_init())
			error_tok(var_name,
				  "variable '%.*s' shadows a name captured "
				  "by a defer in the same scope; the defer "
				  "body would bind to the shadowing variable",
				  nlen, name);
		defer_shadows[defer_shadow_count++] = (DeferShadow){
				.name = name, .len = nlen,
				.block_depth = ctx->block_depth + (in_for_init() ? 1 : 0),
				.var_tok = var_name,
				.defer_idx = i,
			};
		return;
	}
}

// Check enum constants and typedef names for defer-captured-variable shadowing.
// Called from Pass 2 at statement-start for enum definitions and typedef declarations
// that bypass process_declarators (and thus check_defer_var_shadow).
static void check_enum_body_defer_shadow(Token *brace) {
	Token *end = tok_match(brace);
	if (!end) return;
	Token *t = tok_next(brace);
	while (t && t != end && t->kind != TK_EOF) {
		if (t->kind == TK_IDENT || t->kind == TK_KEYWORD) {
			check_defer_var_shadow(t);
			while (t && t != end && t->kind != TK_EOF && !match_ch(t, ',')) {
				if (t->flags & TF_OPEN) {
					t = tok_match(t) ? tok_next(tok_match(t)) : tok_next(t);
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
			if (decl.var_name)
				check_defer_var_shadow(decl.var_name);
			if (!decl.end) break;
			t = decl.end;
			if (match_ch(t, ',')) t = tok_next(t);
			else break;
		}
		return;
	}
}

// Check if any currently-live defer shadow conflicts with the defers that are
// about to be pasted.  Called at return/goto/break/continue — anywhere
// emit_defers_ex will paste defer bodies while inner variables are still live.
static void check_defer_shadow_at_exit(DeferEmitMode mode, int stop_depth) {
	if (defer_shadow_count == 0) return;
	// Determine which defer indices will be pasted by this exit.
	// Walk scopes the same way emit_defers_ex does, collecting the range.
	int min_defer_idx = defer_count; // exclusive upper bound not needed; just track min
	int curr_bd = ctx->block_depth;
	for (int d = ctx->scope_depth - 1; d >= 0; d--) {
		if (scope_stack[d].kind != SCOPE_BLOCK) continue;
		if (mode == DEFER_TO_DEPTH && curr_bd <= stop_depth) break;
		if (scope_stack[d].defer_start_idx < min_defer_idx)
			min_defer_idx = scope_stack[d].defer_start_idx;
		curr_bd--;
		if (mode == DEFER_BREAK &&
		    (scope_stack[d].is_loop || scope_stack[d].is_switch)) break;
		if (mode == DEFER_CONTINUE && scope_stack[d].is_loop) break;
	}
	if (min_defer_idx >= defer_count) return; // no defers will be pasted
	for (int si = 0; si < defer_shadow_count; si++) {
		DeferShadow *sh = &defer_shadows[si];
		if (sh->defer_idx >= min_defer_idx && sh->defer_idx < defer_count)
			error_tok(sh->var_tok,
				  "variable '%.*s' shadows a name captured by defer "
				  "in an enclosing scope and a control-flow exit "
				  "(return/goto/break/continue) would paste the "
				  "defer while the shadow is live",
				  sh->len, sh->name);
	}
}

// Record goto entry and set computed_goto flag if applicable.
// Check if a token is a defer keyword (not a variable/field named "defer").
// Filters out: typedef-shadowed identifiers (unless followed by '{'),
// goto targets, member access, labels (defer:), and assignment targets.
static inline bool is_defer_kw(Token *tok, Token *prev) {
	return (tok->tag & TT_DEFER) &&
	       (!typedef_lookup(tok) || match_ch(tok_next(tok), '{')) &&
	       !(prev && (prev->tag & (TT_GOTO | TT_MEMBER))) &&
	       tok_next(tok) && !match_ch(tok_next(tok), ':') &&
	       !(tok_next(tok) && (tok_next(tok)->tag & TT_ASSIGN));
}

// --- Orelse Detection & Validation ---

static inline void p1d_record_goto(Token *tok, uint16_t cur_sid, int p1d_cur_func) {
	if ((tok->tag & TT_GOTO) && !is_known_typedef(tok) && tok_next(tok)) {
		if (is_identifier_like(tok_next(tok))) {
			P1FuncEntry *e = p1_alloc(P1K_GOTO, cur_sid, tok);
			Token *target = tok_next(tok);
			e->label.name = tok_loc(target);
			e->label.len = target->len;
		} else if (match_ch(skip_noise(tok_next(tok)), '*'))
			func_meta[p1d_cur_func].has_computed_goto = true;
	}
}

static inline bool is_orelse_keyword(Token *tok) {
	if (!(tok->tag & TT_ORELSE)) return false;
	if (last_emitted && (last_emitted->tag & TT_MEMBER)) return false;
	// Shadow: in Pass 2, require last_emitted to be expression-ending
	TypedefEntry *te = typedef_lookup(tok);
	if (te && !te->is_shadow) return false;  // real typedef suppresses
	if (te && te->is_shadow) {
		if (!last_emitted) return false;
		if (!orelse_shadow_is_kw(last_emitted)) return false;
	}
	return true;
}

// Reject side effects on the LHS of an orelse expression that would be duplicated.
// `ctx_msg` is the prefix for the error message (e.g. "orelse in typeof", "orelse on assignment").
// `check_asm` enables TT_ASM check (for bare assignment path).
// `check_volatile_deref` enables volatile dereference check (for bracket/paren path).
// `check_indirect_call` enables (expr)() check (for bare assignment path).
static void reject_orelse_side_effects(Token *start, Token *end,
				       const char *ctx_msg, const char *advice,
				       bool check_asm, bool check_volatile_deref,
				       bool check_indirect_call) {
	int pd = 0;
	Token *prev_tok = NULL;
	for (Token *s = start; s && s != end && s->kind != TK_EOF; s = tok_next(s)) {
		/* Track paren/bracket depth for the depth-0 comma check below. */
		if (s->flags & TF_OPEN) { pd++; goto next_checks; }
		if (s->flags & TF_CLOSE) { pd--; goto next_checks; }
		/* A comma operator at depth 0 means the left sub-expression is a
		 * throw-away side-effect that will be evaluated twice — once in the
		 * if-condition and once in the fallback assignment arm. */
		if (pd == 0 && match_ch(s, ','))
			error_tok(s, "%s with comma operator at top level (the "
				  "left-hand sub-expression before ',' is evaluated "
				  "twice — double evaluation of volatile reads or "
				  "other side effects) %s", ctx_msg, advice);
		next_checks:
		if ((s->len == 2 && ((s->ch0 == '+' && tok_loc(s)[1] == '+') ||
				     (s->ch0 == '-' && tok_loc(s)[1] == '-'))) ||
		    is_assignment_operator_token(s))
			error_tok(s, "%s with side effect %s", ctx_msg, advice);
		if (check_asm && (s->tag & TT_ASM))
			error_tok(s, "%s with inline asm %s", ctx_msg, advice);
		if ((is_valid_varname(s) && !is_type_keyword(s)) || match_ch(s, ']') || match_ch(s, ')')) {
			Token *after_s = tok_next(s);
			if (after_s && after_s != end && match_ch(after_s, '('))
				error_tok(s, "%s with %s call %s",
					  ctx_msg,
					  check_indirect_call ? "a function" : "side effect",
					  advice);
		}
		if (check_indirect_call && match_ch(s, '(') && (s->flags & TF_OPEN) && tok_match(s) &&
		    tok_match(s) != end && tok_next(tok_match(s)) &&
		    tok_next(tok_match(s)) != end && match_ch(tok_next(tok_match(s)), '('))
			error_tok(s, "%s with an indirect call %s", ctx_msg, advice);
		if (check_volatile_deref && match_ch(s, '*') && tok_next(s) && tok_next(s) != end) {
			/* Disambiguate unary * (dereference) from binary *
			 * (multiplication) by examining the previous token.
			 * Binary * follows a value-producing token: ), ], ident,
			 * number, or string literal.
			 * Unary * follows an operator, opening delimiter, keyword,
			 * or nothing (start of expression). */
			bool is_mul = false;
			if (prev_tok) {
				if (prev_tok->kind == TK_NUM || prev_tok->kind == TK_STR)
					is_mul = true;
				else if (prev_tok->kind == TK_IDENT && !is_type_keyword(prev_tok))
					is_mul = true;
				else if (match_ch(prev_tok, ']'))
					is_mul = true;
				else if (match_ch(prev_tok, ')') && (prev_tok->flags & TF_CLOSE)) {
					/* ) from a cast means unary *, ) from a value
					 * expression means binary *.  Peek inside the
					 * matching ( — if it starts with a type keyword
					 * it's likely a cast, unless preceded by
					 * sizeof/alignof/offsetof. */
					Token *om = tok_match(prev_tok);
					if (om) {
						Token *fi = tok_next(om);
						bool looks_cast = fi && (
						    is_type_keyword(fi) ||
						    (fi->tag & (TT_QUALIFIER | TT_SUE | TT_TYPEOF)));
						if (looks_cast) {
							uint32_t oi = tok_idx(om);
							if (oi >= 2 &&
							    token_pool[oi - 1].flags & TF_SIZEOF)
								is_mul = true;
						} else is_mul = true;
					}
				}
			}
			if (!is_mul)
				error_tok(s, "%s with pointer dereference %s",
					  ctx_msg, advice);
		}
		if (check_volatile_deref && (s->tag & TT_MEMBER))
			error_tok(s, "%s with member access operator %s",
				  ctx_msg, advice);
		if (check_volatile_deref && match_ch(s, '[') && (s->flags & TF_OPEN))
			error_tok(s, "%s with array subscript %s",
				  ctx_msg, advice);
		prev_tok = s;
	}
}

// --- Type & Return Type Emission ---

// Emit type tokens with optional const stripping and struct/union body elision.
// strip_const: skip TT_CONST tokens (for orelse const-fallback casts).
// strip_sue_body: elide struct/union { body } (emit just the tag name).
static void emit_type_range(Token *start, Token *end, bool strip_const, bool strip_sue_body) {
	int raw_depth = 0;
	for (Token *t = start; t && t != end && t->kind != TK_EOF;) {
		if (strip_const && (t->tag & TT_CONST)) { t = tok_next(t); continue; }
		// which has the full keyword dispatcher (defer, goto, zeroinit).
		if (is_stmt_expr_open(t) && tok_match(t)) {
			walk_balanced(t, true);
			t = tok_next(tok_match(t));
			continue;
		}
		if (match_ch(t, '{')) raw_depth++;
		if (match_ch(t, '}')) raw_depth--;
		if (raw_depth == 0 && (t->flags & TF_RAW) && !is_known_typedef(t)) {
			Token *after = skip_noise(tok_next(t));
			Token *last = t;
			SKIP_RAW(after, last);
			emit_noise_between_raws(t, last);
			t = tok_next(last);
			continue;
		}
		if (FEAT(F_ORELSE) && (t->tag & (TT_TYPEOF | TT_BITINT | TT_ALIGNAS)) &&
		    tok_next(t) && match_ch(tok_next(t), '(')) {
			emit_tok(t);
			t = tok_next(t);
			t = walk_balanced_orelse(t);
			continue;
		}
		if (strip_sue_body && match_ch(t, '{')) {
			Token *kw = NULL;
			for (Token *s = start; s != t; s = tok_next(s))
				if (s->tag & TT_SUE) kw = s;
			bool keep = false;
			if (kw && !is_enum_kw(kw)) {
				for (Token *u = tok_next(kw); u && u != t; u = tok_next(u)) {
					if (match_ch(u, ')') && tok_match(u) && tok_loc(tok_match(u)) < tok_loc(kw)) {
						keep = true;
						break;
					}
				}
				if (!keep) {
					keep = true;
					for (Token *u = tok_next(kw); u && u != t; u = tok_next(u)) {
						if ((u->flags & TF_OPEN) && (match_ch(u, '(') || (u->flags & TF_C23_ATTR))) {
							u = tok_match(u);
							if (!u || u == t) break;
							continue;
						}
						if (is_valid_varname(u) && !(u->tag & (TT_QUALIFIER | TT_ATTR | TT_TYPEOF))) {
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
		emit_tok(t); t = tok_next(t);
	}
}

// Emit expression tokens to ';', stop token, or 'orelse' (if check_orelse).
static Token *emit_expr_to_stop(Token *tok, Token *stop, bool check_orelse) {
	while (tok->kind != TK_EOF) {
		if (tok->flags & TF_OPEN) { tok = walk_balanced(tok, true); continue; }
		if (match_ch(tok, ';') || (stop && tok == stop)) break;
		if (check_orelse && is_orelse_keyword(tok)) break;
		emit_tok(tok); tok = tok_next(tok);
	}
	return tok;
}

// Strip consecutive raw keywords at emit time: skip the raws, emit
// interleaving noise (whitespace/comments), return next non-raw token.
// Returns NULL if tok is not a strippable raw keyword.
static inline Token *try_strip_raw(Token *t) {
	if (__builtin_expect((t->flags & TF_RAW) && !is_known_typedef(t), 0)) {
		Token *after = skip_noise(tok_next(t));
		if (is_raw_strip_context(after)) {
			Token *last = t;
			SKIP_RAW(after, last);
			emit_noise_between_raws(t, last);
			return tok_next(last);
		}
	}
	return NULL;
}

// Captures function return type. Returns 1 if void function, 2 if captured, 0 if not function.
static int capture_function_return_type(Token *tok) {
	while (tok && tok->kind != TK_EOF) {
		if (tok->tag & (TT_SKIP_DECL | TT_INLINE)) { tok = tok_next(tok); continue; }
		Token *next = skip_noise(tok);
		if (next == tok) break;
		tok = next;
	}
	if (!tok || tok->kind == TK_EOF) return 0;

	Token *type_start = tok;

	TypeSpecResult type = parse_type_specifier(tok);
	if (!type.saw_type) return 0;

	bool is_void = type.has_void;
	tok = type.end;

	if (type.is_struct) {
		for (Token *t = type_start; t && t != tok && t->kind != TK_EOF; t = tok_next(t))
			if (match_ch(t, '{')) return 0;
	}

	tok = skip_pointers(tok, &is_void);

	if (tok && is_valid_varname(tok) && tok_next(tok) && match_ch(tok_next(tok), '(')) {
		if (is_void) return 1;
		ctx->func_ret_type_start = type_start;
		ctx->func_ret_type_end = tok;
		ctx->func_ret_type_suffix_start = ctx->func_ret_type_suffix_end = NULL;
		return 2;
	}

	if (tok && match_ch(tok, '(')) {
		Token *outer_open = tok;
		Token *inner = skip_pointers(tok_next(tok), &is_void);

		while (inner && match_ch(inner, '(')) {
			inner = skip_pointers(tok_next(inner), NULL);
		}
		if (inner && is_valid_varname(inner) && tok_next(inner)) {
			if (match_ch(tok_next(inner), '(')) {
				Token *after_params = skip_balanced(tok_next(inner), '(', ')');
				if (after_params && match_ch(after_params, ')')) {
					// Walk only array-dimension brackets after outer ')'.
					// Do NOT skip attributes — they belong on the function,
					// not on the return-type typedef.
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
					ctx->func_ret_type_suffix_start = ctx->func_ret_type_suffix_end = NULL;
					return 2;
				}
			}
		}
	}
	return 0;
}

// Emit captured return type. Complex declarators get a typedef.
// Errors if capture failed (cannot resolve return type portably).
static void emit_ret_type(void) {
	if (ctx->func_ret_type_start && ctx->func_ret_type_end) {
		if (ctx->func_ret_type_suffix_start) {
			// typedef PREFIX __prism_ret_t_N SUFFIX; __prism_ret_t_N
			OUT_LIT("typedef ");
			emit_token_range(ctx->func_ret_type_start, ctx->func_ret_type_end);
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
		} else emit_token_range(ctx->func_ret_type_start, ctx->func_ret_type_end);
	} else {
		error("defer in function with unresolvable return type; "
		      "use a named struct or typedef");
	}
}

// _Generic member-injection rewrite: obj._Generic(sel, int: f(A)+1, float: f(B)+2)
// → _Generic(sel, int: obj.f(A)+1, float: obj.f(B)+2)
//
// Instead of extracting a single representative target (which discards
// trailing expressions), this emits the full _Generic with the member-access
// prefix (obj. / obj->) injected into every association branch.
//
// Returns the token after the rewritten _Generic, or NULL if no rewrite.
static Token *try_generic_member_rewrite(Token *tok) {
	if (!last_emitted || !(last_emitted->tag & TT_MEMBER)) return NULL;

	Token *open = tok_next(tok);
	if (!open || !match_ch(open, '(') || !tok_match(open)) return NULL;
	Token *close = tok_match(open);
	Token *assoc_start = generic_find_assoc_start(open);
	if (!assoc_start) return NULL;

	// --- Find the start of the postfix expression chain ---
	// Walk backward from the member operator (. / ->) through the token
	// pool, crossing matched brackets and chained member accesses.
	Token *member_op = last_emitted;
	uint32_t pi = tok_idx(member_op);
	for (;;) {
		if (pi == 0) break;
		pi--;
		Token *t = &token_pool[pi];
		if (t->flags & TF_CLOSE) {
			Token *m = tok_match(t);
			if (m) pi = tok_idx(m); else break;
			// Token before matched open: include ident (function name) or
			// continue chain if preceded by another member operator.
			if (pi > 0 && is_valid_varname(&token_pool[pi - 1])) {
				pi--;
				if (pi > 0 && (token_pool[pi - 1].tag & TT_MEMBER)) {
					pi--;
					continue;
				}
			} else if (pi > 0 && (token_pool[pi - 1].tag & TT_MEMBER)) {
				pi--;
				continue;
			}
			break;
		}
		if (is_valid_varname(t) || t->kind == TK_NUM || t->kind == TK_STR) {
			if (pi > 0 && (token_pool[pi - 1].tag & TT_MEMBER)) {
				pi--;
				continue;
			}
			break;
		}
		// Unexpected token — don't include it in the chain.
		pi++;
		break;
	}
	uint32_t chain_start_pi = pi;

	// --- Look up the absolute output position before chain_start was emitted ---
	int64_t abs_rewind = -1;
	for (int i = emit_save_idx - 1;
	     i >= 0 && i >= emit_save_idx - EMIT_SAVE_RING_SIZE; i--) {
		int ri = i & EMIT_SAVE_RING_MASK;
		if (emit_save_ring[ri].tpi == chain_start_pi) {
			abs_rewind = emit_save_ring[ri].abs_pos;
			break;
		}
	}
	if (abs_rewind < 0) return NULL;

	// --- Capture the prefix text (e.g. "obj." / "ptr->") ---
	// We copy it from the output buffer before rewinding.  The text includes
	// all spacing / line directives that emit_tok produced — but we will
	// re-inject using raw out_str for each branch to avoid duplicate #line
	// directives.  Instead, build a clean, compact prefix from the tokens.
	char prefix_buf[1024];
	int prefix_len = 0;
	for (uint32_t p = chain_start_pi; p <= tok_idx(member_op); p++) {
		Token *pt = &token_pool[p];
		if (prefix_len > 0 && (pt->flags & TF_HAS_SPACE) && prefix_len < (int)sizeof(prefix_buf) - 1)
			prefix_buf[prefix_len++] = ' ';
		int tl = pt->len;
		if (prefix_len + tl > (int)sizeof(prefix_buf)) return NULL;  // prefix too large
		memcpy(prefix_buf + prefix_len, tok_loc(pt), tl);
		prefix_len += tl;
	}

	// --- Rewind output past the prefix ---
	// If the target position is still in the current buffer, rewind in-place.
	// If it was already flushed to disk, flush remaining data and fseek back.
	if (abs_rewind >= out_total_flushed) {
		out_buf_pos = (int)(abs_rewind - out_total_flushed);
	} else {
		out_flush();
		if (fseek(out_fp, (long)abs_rewind, SEEK_SET) != 0) return NULL;
		out_total_flushed = abs_rewind;
		out_buf_pos = 0;
	}

	// Fix last_emitted to the token emitted just before the prefix chain.
	last_emitted = (chain_start_pi > 0) ? &token_pool[chain_start_pi - 1] : NULL;

	// --- Emit _Generic( ---
	emit_tok(tok);   // _Generic
	emit_tok(open);  // (

	// --- Emit controlling expression (up to assoc_start) ---
	for (Token *t = tok_next(open); t && t != assoc_start; t = tok_next(t)) {
		if ((t->flags & TF_OPEN) && tok_match(t)) {
			walk_balanced(t, true);
			t = tok_match(t);
			continue;
		}
		emit_tok(t);
	}

	// --- Emit each association, injecting prefix before targets ---
	// For each branch value: inject prefix before every bracket-depth-0
	// identifier that is followed by '(' (function call).  For branches
	// that are a single bare identifier (no parens), inject before it.
	int ternary = 0;
	bool in_value = false;
	bool val_has_call = false;   // current branch has ident+( at depth 0
	bool val_bare_ident = false; // current branch is a single bare ident
	for (Token *t = assoc_start; t && t != close; t = tok_next(t)) {
		if ((t->flags & TF_OPEN) && tok_match(t)) {
			walk_balanced(t, true);
			t = tok_match(t);
			continue;
		}
		if (match_ch(t, '?')) { ternary++; emit_tok(t); continue; }
		if (match_ch(t, ':') && ternary > 0) { ternary--; emit_tok(t); continue; }
		if (match_ch(t, ':') && ternary == 0) {
			emit_tok(t);  // ':'
			in_value = true;
			// Pre-scan the branch value to decide injection strategy.
			val_has_call = false;
			val_bare_ident = false;
			Token *vs = tok_next(t);
			for (Token *s = vs; s && s != close; s = tok_next(s)) {
				if ((s->flags & TF_OPEN) && tok_match(s))
					{ s = tok_match(s); continue; }
				if (match_ch(s, ',')) break;
				if (is_valid_varname(s) && tok_next(s) &&
				    match_ch(tok_next(s), '('))
					{ val_has_call = true; break; }
			}
			if (!val_has_call && vs && is_valid_varname(vs)) {
				Token *after_vs = tok_next(vs);
				if (!after_vs || after_vs == close ||
				    match_ch(after_vs, ','))
					val_bare_ident = true;
			}
			continue;
		}
		if (match_ch(t, ',') && ternary == 0) {
			in_value = false;
			emit_tok(t);
			continue;
		}

		// Injection point: prefix before depth-0 ident+( or bare ident.
		// Skip identifiers preceded by a member operator (./->) — those are
		// chained method calls (e.g. get_api()->fetch()) where only the FIRST
		// call in the chain should receive the prefix.
		bool inject = false;
		if (in_value && is_valid_varname(t) &&
		    !(last_emitted && (last_emitted->tag & TT_MEMBER))) {
			if (val_has_call && tok_next(t) &&
			    match_ch(tok_next(t), '('))
				inject = true;
			else if (val_bare_ident) {
				inject = true;
				val_bare_ident = false; // only once
			}
		}
		if (inject) {
			// Emit spacing (space or newline) that emit_tok would
			// normally prepend, then the prefix, then the token text
			// — no gap between prefix and identifier.
			if (tok_at_bol(t)) out_char('\n');
			else if ((t->flags & TF_HAS_SPACE) ||
				 needs_space(last_emitted, t))
				out_char(' ');
			out_str(prefix_buf, prefix_len);
			out_str(tok_loc(t), t->len);
			last_emitted = t;
		} else {
			emit_tok(t);
		}
	}

	// --- Emit closing ) ---
	emit_tok(close);
	last_emitted = close;
	return tok_next(close);
}

// Zero-init declaration parsing helpers

static Token *decl_noise(Token *tok, bool emit) {
	while (tok && tok->kind != TK_EOF) {
		if (tok->tag & TT_ATTR) {
			if (emit) emit_tok(tok);
			tok = tok_next(tok);
			if (tok && match_ch(tok, '(')) tok = walk_balanced(tok, emit);
		} else if (is_c23_attr(tok)) {
			tok = walk_balanced(tok, emit);
		} else if (tok->kind == TK_PREP_DIR) {
			if (emit) emit_tok(tok);
			tok = tok_next(tok);
		} else break;
	}
	return tok;
}

static Token *handle_open_brace(Token *tok);
static Token *handle_close_brace(Token *tok);
static Token *handle_defer_keyword(Token *tok);
static Token *handle_control_exit_defer(Token *tok);
static Token *handle_goto_keyword(Token *tok);
static void end_statement_after_semicolon(void);
static inline Token *try_process_stmt_token(Token *t, Token *end, Token **unreachable_tok);
static Token *emit_block_body(Token *tok, Token *end);

// Emit control-flow condition tokens (the '(' ... ')' of if/for/while/switch)
// with full statement dispatch. Unlike walk_balanced, this tracks at_stmt_start
// so that for-init declarations receive zeroinit and orelse processing.
static Token *emit_ctrl_condition(Token *t, Token **unreachable_tok) {
	Token *close_p = tok_match(t);
	if (!close_p) { emit_tok(t); return tok_next(t); }
	emit_tok(t); t = tok_next(t); // emit '('
	ctx->at_stmt_start = true;
	while (t && t != close_p && t->kind != TK_EOF) {
		Token *next = try_process_stmt_token(t, close_p, unreachable_tok);
		if (next) { t = next; continue; }
		// Stmt-expr dispatch
		if ((t->flags & TF_OPEN) && is_stmt_expr_open(t) && tok_match(t)) {
			emit_tok(t); // '('
			Token *se_end = tok_match(t);
			Token *inner = tok_next(t); // '{'
			bool saved_ss = ctx->at_stmt_start;
			CtrlState saved_ctrl = ctrl_state;
			inner = emit_block_body(inner, se_end);
			ctx->at_stmt_start = saved_ss;
			ctrl_state = saved_ctrl;
			if (se_end) { emit_tok(se_end); t = tok_next(se_end); }
			else t = inner;
			continue;
		}
		// Nested balanced groups (function args, array subscripts, etc.)
		if ((t->flags & TF_OPEN) && tok_match(t)) {
			t = walk_balanced(t, true);
			ctx->at_stmt_start = false;
			continue;
		}
		// Semicolons (for-init separators): reset stmt_start
		if (t->len == 1 && t->ch0 == ';') {
			emit_tok(t); t = tok_next(t);
			ctx->at_stmt_start = true;
			continue;
		}
		ctx->at_stmt_start = false;
		emit_tok(t); t = tok_next(t);
	}
	if (t == close_p) { emit_tok(t); t = tok_next(t); } // emit ')'
	return t;
}

// Process block body tokens with full transpilation dispatch.
// Handles {/}, ;, prep dirs, defer/return/break/continue/goto,
// try_process_stmt_token (zeroinit/orelse/raw), orelse error, and
// ctrl-flow keyword tracking for braceless bodies.
// Caller manages ctrl_state save/restore and outer brace handling.
static Token *emit_block_body(Token *tok, Token *end) {
	Token *unreachable_tok = NULL;
	while (tok && tok != end && tok->kind != TK_EOF) {
		if (match_ch(tok, '{')) { tok = handle_open_brace(tok); continue; }
		if (match_ch(tok, '}')) { tok = handle_close_brace(tok); continue; }
		if (match_ch(tok, ';')) {
			end_statement_after_semicolon();
			bool is_ur = (tok == unreachable_tok);
			emit_tok(tok); tok = tok_next(tok);
			if (is_ur) { EMIT_UNREACHABLE(); unreachable_tok = NULL; }
			continue;
		}
		if (__builtin_expect(tok->kind == TK_PREP_DIR, 0)) {
			emit_tok(tok); tok = tok_next(tok);
			continue;
		}
		uint32_t itag = tok->tag;
		if (itag) {
			if (__builtin_expect(itag & TT_DEFER, 0) && !in_generic()) {
				Token *next = handle_defer_keyword(tok);
				if (next) { tok = next; continue; }
			}
			if (__builtin_expect(FEAT(F_DEFER) && (itag & (TT_RETURN | TT_BREAK | TT_CONTINUE)), 0)) {
				Token *next = handle_control_exit_defer(tok);
				if (next) { tok = next; continue; }
			}
			if (__builtin_expect((itag & TT_GOTO) && FEAT(F_DEFER | F_ZEROINIT), 0)) {
				Token *next = handle_goto_keyword(tok);
				if (next) { tok = next; continue; }
			}
		}
		{ Token *next = try_process_stmt_token(tok, end, &unreachable_tok);
		  if (next) { tok = next; continue; } }
		if (__builtin_expect(FEAT(F_ORELSE) && is_orelse_keyword(tok), 0))
			error_tok(tok, "'orelse' cannot be used here (it must appear at the "
				  "statement level in a declaration or bare expression)");
		if (ctx->at_stmt_start && (tok->tag & (TT_IF | TT_LOOP | TT_SWITCH)) &&
		    !is_known_typedef(tok)) {
			if (((tok->tag & TT_IF) && tok->ch0 == 'e') ||
			    ((tok->tag & TT_LOOP) && tok->ch0 == 'd')) {
				emit_tok(tok); tok = tok_next(tok);
				ctrl_state.pending = true;
				ctrl_state.parens_just_closed = true;
				continue;
			}
			emit_tok(tok); tok = tok_next(tok);
			if (tok && match_ch(tok, '(') && tok_match(tok)) {
				tok = emit_ctrl_condition(tok, &unreachable_tok);
				ctx->at_stmt_start = true;
				ctrl_state.pending = true;
				ctrl_state.parens_just_closed = true;
				continue;
			}
			continue;
		}
		ctx->at_stmt_start = false;
		emit_tok(tok); tok = tok_next(tok);
	}
	return tok;
}

static bool bracket_scan_has_orelse(Token *open) {
	if (tok_ann(open) & P1_OE_BRACKET) return true;
	for (Token *s = tok_next(open); s && s != tok_match(open); s = tok_next(s)) {
		if (is_orelse_keyword(s)) return true;
		if (s->flags & TF_OPEN) { s = tok_match(s) ? tok_match(s) : s; }
	}
	return false;
}

// Walk a balanced token group between matching delimiters, optionally emitting.
static Token *walk_balanced(Token *tok, bool emit) {
	Token *end = tok_match(tok);
	if (!end) return tok_next(tok);
	if (emit) {
		for (Token *t = tok; t != tok_next(end) && t->kind != TK_EOF;) {
			// Detects both nested stmt-exprs (t != tok) and the case
			// where walk_balanced is called directly on a stmt-expr '('.
			if ((t->flags & TF_OPEN) && is_stmt_expr_open(t)) {
				emit_tok(t); // '('
				Token *se_end = tok_match(t);
				Token *inner = tok_next(t); // '{'
				bool saved_ss = ctx->at_stmt_start;
				CtrlState saved_ctrl = ctrl_state;
				inner = emit_block_body(inner, se_end);
				ctx->at_stmt_start = saved_ss;
				ctrl_state = saved_ctrl;
				if (se_end) { emit_tok(se_end); t = tok_next(se_end); } // ')'
				else t = inner;
				continue;
			}
			// Bracket orelse: dispatch to orelse-aware walker.
			if (FEAT(F_ORELSE) && (t->flags & TF_OPEN) && match_ch(t, '[') && tok_match(t)) {
				if (bracket_scan_has_orelse(t)) { t = walk_balanced_orelse(t); continue; }
				// No orelse — emit bracket contents verbatim (no nested
				// orelse scan needed, avoiding O(N²) on nested brackets).
				Token *bclose = tok_match(t);
				while (t != bclose) { emit_tok(t); t = tok_next(t); }
				emit_tok(t); t = tok_next(t);
				continue;
			}
			// typeof with orelse inside: use orelse-aware walker
			if (FEAT(F_ORELSE) && (t->tag & TT_TYPEOF) && tok_next(t) && match_ch(tok_next(t), '(')) {
				emit_tok(t);           // typeof keyword
				t = tok_next(t);       // (
				t = walk_balanced_orelse(t);
				continue;
			}
			emit_tok(t); t = tok_next(t);
		}
	}
	return tok_next(end);
}

// Walk a balanced group, transforming any top-level orelse into a ternary.
// Rejects orelse with control-flow action; transforms value fallback to (LHS) ? (LHS) : (RHS).

// Recurses into () and [] groups so nested orelse is never emitted raw.
static void emit_token_range_orelse(Token *start, Token *end) {
	Token *orelse = NULL;
	Token *prev = NULL;
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = tok_next(t)) {
		if (t->flags & TF_OPEN) { prev = tok_match(t); t = tok_match(t); continue; }
		if (is_orelse_kw_shadow(t) &&
		    !(prev && (prev->tag & TT_MEMBER))) {
			TypedefEntry *te = typedef_lookup(t);
			if (!te || orelse_shadow_is_kw(prev)) {
				orelse = t;
				break;
			}
		}
		prev = t;
	}
	if (!orelse) {
		// No top-level orelse. Walk tokens, recursing into balanced
		// groups to catch orelse nested inside function args, etc.
		for (Token *t = start; t && t != end && t->kind != TK_EOF; t = tok_next(t)) {
			if (t != start) out_char(' ');
			// which has the full keyword dispatcher (goto, return, defer).
			if (is_stmt_expr_open(t) && tok_match(t)) {
				walk_balanced(t, true);
				t = tok_match(t);
				continue;
			}
			if ((t->flags & TF_OPEN) && (match_ch(t, '(') || match_ch(t, '['))) {
				Token *close = tok_match(t);
				if (close && close != end) {
					OUT_TOK(t);
					out_char(' ');
					emit_token_range_orelse(tok_next(t), close);
					out_char(' ');
					OUT_TOK(close);
					t = close;
					continue;
				}
			}
			OUT_TOK(t);
		}
		return;
	}
	Token *rhs = tok_next(orelse);
	reject_orelse_side_effects(start, orelse,
				  "'orelse' in array dimension / typeof",
				  "in a chained 'orelse' (would be evaluated twice); "
				  "hoist the expression to a variable first",
				  false, true, false);
	OUT_LIT("(");
	emit_token_range_orelse(start, orelse);
	OUT_LIT(") ? (");
	emit_token_range_orelse(start, orelse);
	OUT_LIT(") : (");
	emit_token_range_orelse(rhs, end);
	OUT_LIT(")");
}

// Pre-scan a declarator for bracket orelse, emit hoisted temp declarations.
static void validate_bracket_orelse(Token *oe) {
	Token *act = tok_next(oe);
	if (act && (act->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)))
		error_tok(oe, "'orelse' with control flow cannot be used inside "
			  "array dimensions or typeof expressions");
	if (act && match_ch(act, '{'))
		error_tok(oe, "'orelse' block form cannot be used inside "
			  "array dimensions or typeof expressions");
}

// Scan for orelse at depth 0 inside (open, close).
// For '[' brackets, also checks one level inside a '(' spanning entire content.
// Sets *paren_open_out (if non-NULL) when orelse is inside a spanning paren.
static Token *scan_bracket_orelse(Token *open, Token *close, Token **paren_open_out) {
	Token *prev = open;
	for (Token *t = tok_next(open); t && t != close; t = tok_next(t)) {
		if (t->flags & TF_OPEN) {
			if (match_ch(open, '[') && match_ch(t, '(')) {
				Token *pp = tok_match(t);
				if (pp && tok_next(pp) == close) {
					for (Token *p = tok_next(t); p && p != pp; p = tok_next(p)) {
						if (p->flags & TF_OPEN) { p = tok_match(p) ? tok_match(p) : p; continue; }
						if (is_orelse_kw_shadow(p)) {
							if (paren_open_out) *paren_open_out = t;
							return p;
						}
					}
				}
			}
			prev = tok_match(t); t = tok_match(t); continue;
		}
		if (is_orelse_kw_shadow(t) && !(prev && (prev->tag & TT_MEMBER)))
			return t;
		prev = t;
	}
	return NULL;
}

// Each bracket orelse gets: long long __prism_oe_ID = (LHS);
// When orelse appears in a later bracket, preceding non-orelse brackets
// are also hoisted to preserve C99 left-to-right VLA evaluation order.
static void emit_bracket_orelse_temps(Token *start, Token *end) {
	ctx->bracket_oe_count = 0;
	ctx->bracket_oe_next = 0;
	ctx->bracket_dim_count = 0;
	ctx->bracket_dim_next = 0;

	// Phase 1: collect all brackets and identify which have orelse.
	// paren_open: non-NULL when orelse is inside a single '(' group that covers
	// the entire bracket content — e.g. [(f() orelse 1)] from macro expansion.
	typedef struct { Token *open; Token *close; Token *orelse; Token *paren_open; } BracketInfo;
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
		if (t->flags & TF_C23_ATTR) { t = tok_match(t); continue; }
		Token *close = tok_match(t);
		if (!close) continue;
		if (bracket_count >= bracket_cap) {
			int old_cap = bracket_cap;
			bracket_cap *= 2;
			brackets = arena_realloc(&ctx->main_arena, brackets, old_cap * sizeof(BracketInfo), bracket_cap * sizeof(BracketInfo));
		}
		Token *paren_open_found = NULL;
		Token *orelse_found = scan_bracket_orelse(t, close, &paren_open_found);
		brackets[bracket_count++] = (BracketInfo){t, close, orelse_found, paren_open_found};
		if (orelse_found) any_orelse = true;
		t = close;
	}

	if (!any_orelse) { arena_restore(&ctx->main_arena, bracket_mark); return; }

	// Emit temps in left-to-right order.
	// Non-orelse brackets that precede the first orelse bracket get dimension temps
	// to preserve evaluation order.
	for (int i = 0; i < bracket_count; i++) {
		if (brackets[i].orelse) {
			// Orelse bracket: hoist LHS
			ARENA_ENSURE_CAP(&ctx->main_arena, ctx->bracket_oe_ids, ctx->bracket_oe_count,
					 ctx->bracket_oe_cap, 16, unsigned);
			unsigned oe = ctx->ret_counter++;
			ctx->bracket_oe_ids[ctx->bracket_oe_count++] = oe;
			OUT_LIT(" long long __prism_oe_");
			out_uint(oe);
			OUT_LIT(" = (");
			// For paren-wrapped orelse, emit LHS from inside the '(' (skip the outer paren).
			emit_token_range_orelse(
				brackets[i].paren_open ? tok_next(brackets[i].paren_open)
				                       : tok_next(brackets[i].open),
				brackets[i].orelse);
			OUT_LIT(");");
		} else {
			// Non-orelse bracket in a declarator with orelse brackets:
			// hoist to preserve left-to-right VLA evaluation order.
			Token *dim_start = tok_next(brackets[i].open);
			Token *dim_end = brackets[i].close;
			ARENA_ENSURE_CAP(&ctx->main_arena, ctx->bracket_dim_ids,
					 ctx->bracket_dim_count, ctx->bracket_dim_cap, 16, unsigned);
			// Skip empty [], single numeric literal, [*] — no reordering risk
			if (dim_start == dim_end ||
			    (tok_next(dim_start) == dim_end &&
			     (dim_start->kind == TK_NUM || match_ch(dim_start, '*')))) {
				ctx->bracket_dim_ids[ctx->bracket_dim_count++] = (unsigned)-1;
				continue;
			}
			unsigned dim = ctx->ret_counter++;
			ctx->bracket_dim_ids[ctx->bracket_dim_count++] = dim;
			OUT_LIT(" long long __prism_dim_");
			out_uint(dim);
			OUT_LIT(" = (");
			emit_token_range_orelse(dim_start, dim_end);
			OUT_LIT(");");
		}
	}
	arena_restore(&ctx->main_arena, bracket_mark);
}

static Token *walk_balanced_orelse(Token *tok) {
	Token *end = tok_match(tok);
	if (!end) { emit_tok(tok); return tok_next(tok); }
	Token *paren_open = NULL;
	Token *orelse_found = scan_bracket_orelse(tok, end, &paren_open);
	if (!orelse_found) {
		// No orelse at depth 0. Check if this bracket was pre-hoisted as a dim temp.
		if (match_ch(tok, '[') && ctx->bracket_dim_next < ctx->bracket_dim_count) {
			unsigned dim = ctx->bracket_dim_ids[ctx->bracket_dim_next++];
			if (dim != (unsigned)-1) {
				emit_tok(tok); // emit [
				OUT_LIT(" __prism_dim_");
				out_uint(dim);
				emit_tok(end); // emit ]
				return tok_next(end);
			}
		}
		// No pre-hoisted dim — emit normally with nested orelse awareness.
		for (Token *t = tok; t != tok_next(end) && t->kind != TK_EOF;) {
			// Nested [ bracket: recurse for orelse handling
			if (t != tok && t != end && match_ch(t, '[') && (t->flags & TF_OPEN) && tok_match(t)) {
				if (ctx->bracket_dim_next < ctx->bracket_dim_count) {
					unsigned dim = ctx->bracket_dim_ids[ctx->bracket_dim_next++];
					if (dim != (unsigned)-1) {
						emit_tok(t);
						OUT_LIT(" __prism_dim_");
						out_uint(dim);
						emit_tok(tok_match(t));
						t = tok_next(tok_match(t));
						continue;
					}
				}
				t = walk_balanced_orelse(t);
				continue;
			}
			// Nested typeof with potential orelse
			if (t != tok && t != end && FEAT(F_ORELSE) && (t->tag & TT_TYPEOF) &&
			    tok_next(t) && match_ch(tok_next(t), '(')) {
				emit_tok(t);       // typeof keyword
				t = tok_next(t);   // (
				t = walk_balanced_orelse(t);
				continue;
			}
			// handles zero-init, defer, goto, raw stripping, etc.
			if (t != tok && t != end && (t->flags & TF_OPEN) && is_stmt_expr_open(t)) {
				t = walk_balanced(t, true);
				continue;
			}
			// Defense-in-depth: if an 'orelse' token reaches here it was not caught by
			// the depth-0 scan above — likely wrapped in parens deeper than one level.
			if (is_orelse_kw_shadow(t))
				error_tok(t, "'orelse' inside array dimension could not be transformed; "
					   "if wrapped in outer parentheses, remove them: "
					   "use '[f() orelse 1]' not '[(f() orelse 1)]'");
			emit_tok(t); t = tok_next(t);
		}
		return tok_next(end);
	}
	// Defense-in-depth: control-flow actions are rejected in Phase 1G;
	// these checks are unreachable-by-design assertions.
	validate_bracket_orelse(orelse_found);
	// Emit: OPEN (LHS) ? (LHS) : (RHS) CLOSE
	// For paren-wrapped orelse (e.g. [(f() orelse 1)]), strip the outer parens:
	// LHS comes from inside the '(', RHS ends before its closing ')'.
	Token *lhs_start = paren_open ? tok_next(paren_open) : tok_next(tok);
	Token *rhs_start = tok_next(orelse_found);
	Token *rhs_end   = paren_open ? tok_match(paren_open) : end;
	bool is_bracket = match_ch(tok, '[');
	emit_tok(tok); // emit [ or (
	if (is_bracket && ctx->bracket_oe_next < ctx->bracket_oe_count) {
		// Use pre-hoisted temp variable (emitted before the declaration)
		unsigned oe = ctx->bracket_oe_ids[ctx->bracket_oe_next++];
		OUT_LIT(" __prism_oe_");
		out_uint(oe);
		OUT_LIT(" ? __prism_oe_");
		out_uint(oe);
		OUT_LIT(" : (");
		emit_token_range_orelse(rhs_start, rhs_end);
		OUT_LIT(")");
	} else {
		reject_orelse_side_effects(lhs_start, orelse_found,
					  is_bracket ? "'orelse' in array dimension / typeof"
					             : "'orelse' in typeof",
					  "in the LHS (would be evaluated twice); "
					  "hoist the expression to a variable first",
					  false, is_bracket, false);
		// Simple ternary: (LHS) ? (LHS) : (RHS).
		// reject_orelse_side_effects above already rejects function calls,
		// ++/--, assignments, asm, and pointer dereferences, so the double
		// evaluation of LHS is safe.  A bare volatile variable read is the
		// only case that passes — an extra hardware read is acceptable and
		// avoids GNU statement expressions / __auto_type which MSVC cannot
		// compile.
		OUT_LIT(" (");
		emit_token_range_orelse(lhs_start, orelse_found);
		OUT_LIT(") ? (");
		emit_token_range_orelse(lhs_start, orelse_found);
		OUT_LIT(") : (");
		emit_token_range_orelse(rhs_start, rhs_end);
		OUT_LIT(")");
	}
	emit_tok(end); // emit ] or )
	return tok_next(end);
}

// --- Declarator Parsing & Declaration Processing ---

static inline void decl_emit(Token *t, bool emit) {
	if (emit) emit_tok(t);
}

static inline Token *decl_array_dims(Token *t, bool emit, bool *vla) {
	while (match_ch(t, '[')) {
		if (array_size_is_vla(t)) *vla = true;
		if (emit && FEAT(F_ORELSE))
			t = walk_balanced_orelse(t);
		else
			t = walk_balanced(t, emit);
	}
	return t;
}

// Unified declarator parser. emit=true emits tokens, emit=false only advances.
static DeclResult parse_declarator(Token *tok, bool emit) {
	DeclResult r = { .end = tok };
	bool is_vla = false;
	int ptr_depth = 0;

#define DECL_EAT_PTRS(extra_ptr_action)						\
	while (tok && tok->kind != TK_EOF) {					\
		Token *_n = decl_noise(tok, emit);				\
		if (_n != tok) { tok = _n; continue; }				\
		if (match_ch(tok, '*')) {					\
			r.is_pointer = true; r.is_const = false; extra_ptr_action; \
			if (++ptr_depth > 1024) { warn_tok(tok, "pointer depth exceeds 1024; zero-initialization skipped"); r.end = NULL; return r; } \
			decl_emit(tok, emit); tok = tok_next(tok);			\
		} else if (tok->tag & TT_QUALIFIER) {				\
			if (r.is_pointer && (tok->tag & TT_CONST)) r.is_const = true; \
			decl_emit(tok, emit); tok = tok_next(tok);			\
		} else break;							\
	}

	DECL_EAT_PTRS((void)0)

	int nested_paren = 0;
	if (match_ch(tok, '(')) {
		Token *peek = skip_noise(tok_next(tok));
		if (!match_ch(peek, '*') && !match_ch(peek, '(') && !is_valid_varname(peek)) { r.end = NULL; return r; }
		decl_emit(tok, emit); tok = tok_next(tok);
		nested_paren = 1; r.has_paren = true;

		DECL_EAT_PTRS(r.paren_pointer = true)
		while (match_ch(tok, '(')) {
			if (++nested_paren > 1024) { warn_tok(tok, "parenthesization depth exceeds 1024"); r.end = NULL; return r; }
			decl_emit(tok, emit); tok = tok_next(tok);
			DECL_EAT_PTRS(r.paren_pointer = true)
		}
	}
#undef DECL_EAT_PTRS

	if (!is_valid_varname(tok)) { r.end = NULL; return r; }
	r.var_name = tok;
	decl_emit(tok, emit); tok = tok_next(tok);

	tok = decl_noise(tok, emit);

	if (r.has_paren && match_ch(tok, '[')) {
		r.is_array = true; r.paren_array = true;
		tok = decl_array_dims(tok, emit, &is_vla);
	}

	while (r.has_paren && nested_paren > 0) {
		while (match_ch(tok, '(') || match_ch(tok, '[')) {
			if (match_ch(tok, '(')) {
				if (emit && FEAT(F_ORELSE)) tok = walk_balanced_orelse(tok);
				else tok = walk_balanced(tok, emit);
			}
			else { r.is_array = true; r.paren_array = true; tok = decl_array_dims(tok, emit, &is_vla); }
		}
		if (!match_ch(tok, ')')) { r.end = NULL; return r; }
		decl_emit(tok, emit); tok = tok_next(tok);
		nested_paren--;
	}

	if (match_ch(tok, '(')) {
		if (!r.has_paren) { r.end = NULL; return r; }
		r.is_func_ptr = true;
		if (emit && FEAT(F_ORELSE)) tok = walk_balanced_orelse(tok);
		else tok = walk_balanced(tok, emit);
	}

	if (match_ch(tok, '[')) {
		r.is_array = true;
		tok = decl_array_dims(tok, emit, &is_vla);
	}

	while (tok && tok->kind != TK_EOF) {
		Token *next = decl_noise(tok, emit);
		if (next != tok) { tok = next; continue; }
		if (tok->tag & TT_ASM) {
			decl_emit(tok, emit); tok = tok_next(tok);
			if (tok && match_ch(tok, '(')) tok = walk_balanced(tok, emit);
		} else break;
	}

	r.has_init = match_ch(tok, '=');
	r.is_vla = is_vla;
	r.end = tok;
	return r;
}

// Emit noise tokens (attributes, prep dirs) interleaved between consecutive raw keywords.
// Walks from tok_next(first_raw) up to last_raw (exclusive), emitting non-TF_RAW tokens.
static void emit_noise_between_raws(Token *first_raw, Token *last_raw) {
	if (first_raw == last_raw) return;
	for (Token *t = tok_next(first_raw); t && t != last_raw && t->kind != TK_EOF; t = tok_next(t)) {
		if (t->flags & TF_RAW) continue;
		if ((t->flags & TF_OPEN) && tok_match(t)) {
			// Balanced group (C23 attr [[...]], GNU attr((...))): walk & emit
			Token *m = tok_match(t);
			for (Token *u = t; u && u != tok_next(m) && u->kind != TK_EOF; u = tok_next(u))
				emit_tok(u);
			t = m;
		} else emit_tok(t);
	}
}

// Emit first declarator raw, then zero-init subsequent uninitialised declarators.
static Token *emit_raw_verbatim_to_semicolon(Token *tok) {
	while (tok && tok->kind != TK_EOF && !match_ch(tok, ';')) {
		if (tok->flags & TF_OPEN) tok = walk_balanced(tok, true);
		else { emit_tok(tok); tok = tok_next(tok); }
	}
	if (tok && match_ch(tok, ';')) { emit_tok(tok); tok = tok_next(tok); }
	return tok;
}

// Emit memset/byte-loop zeroing for typeof/atomic/VLA variables.
static void emit_typeof_memsets(Token **vars, int count, bool has_volatile, bool has_const) {
	const char *vol = has_volatile ? "volatile " : "";
	int vol_len = has_volatile ? 9 : 0;
	bool use_loop = has_volatile || target_is_msvc();
	for (int i = 0; i < count; i++) {
		// Byte loop for volatile (memset drops volatile) and MSVC (no __builtin_memset).
		if (use_loop) {
			OUT_LIT(" { ");
			out_str(vol, vol_len);
			OUT_LIT("char *__prism_p_");
			out_uint(ctx->ret_counter);
			OUT_LIT(" = (");
			out_str(vol, vol_len);
			if (has_const)
				OUT_LIT("char *)(void *)&");
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
			if (has_const)
				OUT_LIT(" __builtin_memset((void *)&");
			else
				OUT_LIT(" __builtin_memset(&");
			OUT_TOK(vars[i]);
			OUT_LIT(", 0, sizeof(");
			OUT_TOK(vars[i]);
			OUT_LIT("));");
		}
	}
}

// Emit break/continue with defer handling. Returns next token.
static Token *emit_break_continue_defer(Token *tok) {
	bool is_break = tok->tag & TT_BREAK;
	if (FEAT(F_DEFER) && control_flow_has_defers(is_break))
		emit_defers(is_break ? DEFER_BREAK : DEFER_CONTINUE);
	out_char(' '); OUT_TOK(tok); out_char(';');
	tok = tok_next(tok);
	if (match_ch(tok, ';')) tok = tok_next(tok);
	return tok;
}

// Emit goto with defer handling. Returns next token.
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
	if (is_identifier_like(tok)) { OUT_TOK(tok); tok = tok_next(tok); }
	out_char(';');
	if (match_ch(tok, ';')) tok = tok_next(tok);
	return tok;
}

// Emit orelse fallback value tokens (walking balanced groups) until
// ';', stop_comma, or chained orelse keyword.
// Sets *chain_next to the token after 'orelse' if chained, else NULL.
static Token *emit_orelse_fallback_value(Token *tok, Token *stop_comma, Token **chain_next) {
	*chain_next = NULL;
	while (tok->kind != TK_EOF) {
		if (tok->flags & TF_OPEN) {
			if (is_stmt_expr_open(tok))
				error_tok(tok, "GNU statement expressions in orelse fallback values are not "
					  "supported; use 'orelse { ... }' block form instead");
			tok = walk_balanced(tok, true);
			continue;
		}
		if (match_ch(tok, ';') || (stop_comma && tok == stop_comma)) break;
		if (is_orelse_keyword(tok)) {
			*chain_next = tok_next(tok);
			return tok;
		}
		emit_tok(tok); tok = tok_next(tok);
	}
	return tok;
}

static Token *emit_orelse_block_body(Token *tok) {
	Token *blk_close = tok_match(tok);
	if (!blk_close) error_tok(tok, "unterminated orelse block");
	CtrlState saved_ctrl = ctrl_state;
	ctrl_reset();
	tok = handle_open_brace(tok);
	tok = emit_block_body(tok, blk_close);
	tok = handle_close_brace(tok);
	ctrl_state = saved_ctrl;
	ctrl_state.pending = false;
	ctrl_state.parens_just_closed = false;
	end_statement_after_semicolon();
	return tok;
}

// const + fallback orelse: roll back speculative output, re-emit with temp variable.
// MSVC-compatible: instead of "const T x = val ?: fallback;",
// emit: "T __prism_oe_N = (val); if (!__prism_oe_N) __prism_oe_N = (fallback); const T x = __prism_oe_N;"
// Handles chained orelse naturally: each chain link adds another if-assignment on the temp.
// Returns token after fallback expression (at ';' or boundary comma).
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

	// Emit mutable temp: strip const from type (except pointer types where it qualifies pointee).
	// Function pointers: return-type const lives in the type specifier and must be preserved;
	// the declarator-level const (e.g. *const between parens) is stripped below.
	bool strip_type_const = !decl->is_pointer && !decl->is_func_ptr;

	// Detect const typedefs (const baked into typedef, no TT_CONST tag)
	// or typeof expressions (typeof(const_var) carries implicit const).
	// Use __typeof__((type)0 + 0) to strip const via arithmetic promotion.
	bool has_const_typedef = false;
	if (strip_type_const) {
		for (Token *t = type_start; t != type->end; t = tok_next(t)) {
			if (is_const_typedef(t)) { has_const_typedef = true; break; }
			if ((t->tag & TT_TYPEOF) && t->len != 11) { has_const_typedef = true; break; }
		}
	}

	if (has_const_typedef) {
		if (target_is_msvc()) {
			// MSVC's typeof does not strip const from cast-to-rvalue,
			// so use typeof_unqual directly (available with /std:clatest).
			if (pragma_start != type_start)
				emit_range(pragma_start, type_start);
			OUT_LIT(" typeof_unqual(");
			emit_type_range(type_start, type->end, strip_type_const, true);
			out_char(')');
		} else {
		if (pragma_start != type_start)
			emit_range(pragma_start, type_start);
		// Cast to rvalue strips const: typeof((T)0) produces an
		// unqualified type.  Works for both pointer and non-pointer
		// types without arithmetic (avoids _BitInt integer promotion).
		out_char(' '); emit_typeof_keyword(); OUT_LIT("((");
		emit_type_range(type_start, type->end, strip_type_const, true);
		OUT_LIT(")0)");
		}
	} else {
		if (pragma_start != type_start)
			emit_range(pragma_start, type_start);
		emit_type_range(type_start, type->end, strip_type_const, true);
	}
	// Emit declarator prefix, stripping const for mutability.
	for (Token *t = decl_start; t != decl->var_name; t = tok_next(t)) {
		if (t->tag & TT_CONST) continue;
		emit_tok(t);
	}

	OUT_LIT(" __prism_oe_");
	out_uint(oe_id);
	// Emit declarator suffix (array dims, parens, etc.) transforming bracket orelse.
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

	// Emit fallback: chained orelse adds if-assignments; control flow gets a block.
	for (;;) {
		if (tok->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) {
			OUT_LIT(" if (!__prism_oe_");
			out_uint(oe_id);
			OUT_LIT(") {");
			if (tok->tag & TT_RETURN) {
				tok = tok_next(tok);
				tok = emit_return_body(tok, NULL);
			} else if (tok->tag & (TT_BREAK | TT_CONTINUE)) {
				tok = emit_break_continue_defer(tok);
			} else if (tok->tag & TT_GOTO) {
				tok = emit_goto_defer(tok);
			}
			OUT_LIT(" }");
			break;
		}

		// Block-form action: emit as if-block with full scope management.
		// walk_balanced emits verbatim, missing defer cleanup on return.
		if (match_ch(tok, '{')) {
			OUT_LIT(" if (!__prism_oe_");
			out_uint(oe_id);
			OUT_LIT(")");
			tok = emit_orelse_block_body(tok);
			break;
		}

		// Ternary instead of if-assignment to preserve compound literal lifetime.
		OUT_LIT(" __prism_oe_");
		out_uint(oe_id);
		OUT_LIT(" = __prism_oe_");
		out_uint(oe_id);
		OUT_LIT(" ? __prism_oe_");
		out_uint(oe_id);
		OUT_LIT(" : (");

		Token *chain_next;
		tok = emit_orelse_fallback_value(tok, stop_comma, &chain_next);
		OUT_LIT(");");
		if (!chain_next) break;
		tok = chain_next;
	}

	// Emit final const declaration: "const T declarator = __prism_oe_N;"
	if (pragma_start != type_start)
		emit_range(pragma_start, type_start);
	emit_type_range(type_start, type->end, false, false);
	parse_declarator(decl_start, true);

	OUT_LIT(" = __prism_oe_");
	out_uint(oe_id);
	out_char(';');
	return tok;
}

static void check_orelse_in_parens(Token *open) {
	Token *close = tok_match(open);
	// Statement expressions ({ ... }): orelse inside is at its own
	// declaration scope, not at the paren top level — skip entirely.
	if (is_stmt_expr_open(open) && close) return;
	for (Token *pi = open, *t = tok_next(open); t != close; pi = t, t = tok_next(t)) {
		// Skip brackets containing orelse — these are VLA dimensions
		// handled by walk_balanced_orelse.
		if ((t->flags & TF_OPEN) && match_ch(t, '[') && tok_match(t)) { t = tok_match(t); continue; }
		// Skip typeof/typeof_unqual contents — orelse inside typeof is
		// handled separately by the typeof orelse path in walk_balanced.
		if ((t->tag & TT_TYPEOF) && tok_next(t) && match_ch(tok_next(t), '(') &&
		    tok_match(tok_next(t))) {
			t = tok_match(tok_next(t)); // skip past typeof(...)
			continue;
		}
		// Skip statement expression boundaries ({ ... }) — orelse inside
		// a stmt-expr is at its own declaration scope, not at the paren
		// top level.  Jump to the matching ')' of the '(' that opens it.
		if (is_stmt_expr_open(t) && tok_match(t)) {
			t = tok_match(t);
			continue;
		}
		if (is_orelse_kw_shadow(t) && !(pi->tag & TT_MEMBER)) {
			// Skip orelse already tagged as valid declaration init
			// by Phase 1D (equivalent of scan_decl_orelse paren unlink).
			if (tok_ann(t) & P1_OE_DECL_INIT) continue;
			TypedefEntry *te = typedef_lookup(t);
			if (!te || orelse_shadow_is_kw(pi))
				error_tok(t, "'orelse' cannot be used inside parentheses "
					  "(it must appear at the top level of a declaration)");
		}
		if (FEAT(F_DEFER) && (t->tag & TT_DEFER) && (!typedef_lookup(t) || match_ch(tok_next(t), '{')) &&
		    !match_ch(tok_next(t), ':') && !(pi->tag & (TT_MEMBER | TT_GOTO)) &&
		    !(is_type_keyword(pi) || (pi->tag & TT_TYPEDEF)) && !(tok_next(t) && (tok_next(t)->tag & TT_ASSIGN)))
			error_tok(t, "defer cannot be at top level of a parenthesized expression");
	}
}

typedef struct {
	Token *orelse_tok;
	bool is_const_fallback;
} OrelseInitInfo;

typedef struct {
	Token *stop_comma;
	bool has_const_qual;
	bool is_struct_value;
} OrelseDeclTargetInfo;

static inline void reject_orelse_in_for_init(Token *tok) {
	if (in_for_init())
		error_tok(tok, "orelse cannot be used in for-loop initializers");
}

static inline void require_orelse_action(Token *tok, Token *stop_comma) {
	if (match_ch(tok, ';') || (stop_comma && tok == stop_comma))
		error_tok(tok, "expected statement after 'orelse'");
}

static inline bool is_orelse_value_fallback(Token *after_oe) {
	return after_oe &&
	    !(after_oe->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) &&
	    !match_ch(after_oe, '{') && !match_ch(after_oe, ';');
}

static inline void flush_typeof_memsets(Token **vars, int *count, TypeSpecResult *type) {
	emit_typeof_memsets(vars, *count, type->has_volatile, type->has_const);
	*count = 0;
}

static OrelseDeclTargetInfo analyze_decl_orelse_target(Token *tok,
					       Token *type_start,
					       TypeSpecResult *type,
					       DeclResult *decl) {
	OrelseDeclTargetInfo info = {
		.stop_comma = find_boundary_comma(tok),
		.has_const_qual = has_effective_const_qual(type_start, type, decl),
		.is_struct_value = type->is_struct && !type->is_enum && !decl->is_pointer && !decl->is_array,
	};
	bool base_is_array = decl->is_array && (!decl->paren_pointer || decl->paren_array);
	if (!base_is_array && !decl->is_pointer && !decl->paren_pointer) {
		for (Token *t = type_start; t && t != type->end; t = tok_next(t))
			if (is_array_typedef(t)) { base_is_array = true; break; }
	}
	if (base_is_array)
		error_tok(decl->var_name,
			  "orelse on array variable '%.*s' will never trigger "
			  "(array address is never NULL); remove the orelse clause",
			  decl->var_name->len,
			  tok_loc(decl->var_name));
	require_orelse_action(tok, info.stop_comma);
	return info;
}

static OrelseInitInfo scan_decl_orelse(Token *decl_end,
					       Token *type_start,
					       TypeSpecResult *type,
					       DeclResult *decl) {
	OrelseInitInfo info = {0};
	if (!decl->has_init || !FEAT(F_ORELSE)) return info;

	Token *scan = tok_next(decl_end);
	Token *prev_scan = NULL;
	int ternary = 0;
	while (scan && scan->kind != TK_EOF) {
		if (scan->flags & TF_OPEN) {
			// Paren group that spans the rest of the initializer and
			// contains orelse: unwrap the parens so the orelse is found
			// at depth 0 (macro hygiene: #define GET(x) (f(x) orelse 0)).
			if (match_ch(scan, '(') && tok_match(scan)) {
				Token *close = tok_match(scan);
				Token *after_close = tok_next(close);
				if (!after_close || match_ch(after_close, ',') || match_ch(after_close, ';') || after_close->kind == TK_EOF) {
					bool has_inner_orelse = false;
					bool has_depth0_comma = false;
					Token *prev_inner = NULL;
					for (Token *inner = tok_next(scan); inner && inner != close; inner = tok_next(inner)) {
						if (is_orelse_kw_shadow(inner)) {
							TypedefEntry *te_in = typedef_lookup(inner);
							if (!te_in || orelse_shadow_is_kw(prev_inner)) {
								has_inner_orelse = true;
								break;
							}
						}
						if (match_ch(inner, ',')) has_depth0_comma = true;
						if (inner->flags & TF_OPEN) {
							inner = tok_match(inner) ? tok_match(inner) : inner;
							prev_inner = inner;
							continue;
						}
						prev_inner = inner;
					}
					if (has_inner_orelse && !has_depth0_comma) {
						// Only unlink parens when they are the immediate
						// initializer wrapper (no tokens between '=' and '(').
						// When prev_scan != NULL, the paren is part of a
						// larger expression (e.g. 1+(0 orelse 5), g(0 orelse 5))
						// and stripping would destroy precedence/syntax.
						if (prev_scan) break; // fall through to check_orelse_in_parens
						// Unlink '(' from the stream
						decl_end->next_idx = scan->next_idx;
						scan->flags &= ~TF_OPEN;
						// Unlink ')' from the stream
						Token *before_close = scan;
						for (Token *t = tok_next(scan); t && t != close; ) {
							if (t->flags & TF_OPEN) {
								Token *m = tok_match(t);
								if (m) { before_close = m; t = tok_next(m); continue; }
							}
							before_close = t;
							t = tok_next(t);
						}
						before_close->next_idx = close->next_idx;
						// Continue scanning from first inner token (parens removed)
						scan = tok_next(scan);
						continue;
					}
				}
			}
			check_orelse_in_parens(scan);
			prev_scan = tok_match(scan);
			scan = tok_next(tok_match(scan));
			continue;
		}
		if (match_ch(scan, ',') || match_ch(scan, ';')) break;
		if (match_ch(scan, '?')) { ternary++; prev_scan = scan; scan = tok_next(scan); continue; }
		if (match_ch(scan, ':') && ternary > 0) { ternary--; prev_scan = scan; scan = tok_next(scan); continue; }
		if (is_orelse_kw_shadow(scan) &&
		    !(prev_scan && (prev_scan->tag & TT_MEMBER)) && ternary == 0) {
			TypedefEntry *te_scan = typedef_lookup(scan);
			if (!te_scan || orelse_shadow_is_kw(prev_scan)) {
				info.orelse_tok = scan;
				break;
			}
		}
		prev_scan = scan; scan = tok_next(scan);
	}

	if (info.orelse_tok) {
		bool is_fallback = is_orelse_value_fallback(tok_next(info.orelse_tok));
		info.is_const_fallback = has_effective_const_qual(type_start, type, decl) && is_fallback;
	}

	return info;
}

// Detect whether a type specifier refers to a function type
// (via typedef or typeof of a known function). Used to skip
// zero-initialization of function-typed declarations.
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
			// typeof((func)) or typeof(((func))) → func
			while (inner && inner != close && match_ch(inner, '(') && tok_match(inner)) {
				Token *inner_close = tok_match(inner);
				if (inner_close && tok_next(inner_close) == close) {
					inner = tok_next(inner);
					close = inner_close;
				} else break;
			}
			// Single bare identifier inside typeof()?
			if (!inner || inner == close || tok_next(inner) != close ||
			    !is_valid_varname(inner) || (inner->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF))) break;
			typeof_ident_loc = tok_loc(inner);
			typeof_ident_len = inner->len;
			typeof_inner = inner;
			// If the name is shadowed by a local variable, it's not a function.
			{ TypedefEntry *shadow = typedef_lookup(inner);
			  if (shadow && shadow->is_shadow) break; }
			// Check func_meta for a defined function with this name.
			for (int fi = 0; fi < func_meta_count; fi++) {
				Token *fn = func_meta[fi].ret_type_end;
				// For void functions, ret_type_end is NULL.
				// Extract name by walking backward from body_open.
				if (!fn) {
					uint32_t bi = tok_idx(func_meta[fi].body_open);
					for (uint32_t j = bi; j > 0; j--) {
						Token *bt = &token_pool[j - 1];
						if (bt->kind == TK_PREP_DIR) continue;
						if (match_ch(bt, ')') && tok_match(bt)) {
							j = tok_idx(tok_match(bt)) + 1;
							continue;
						}
						if (is_valid_varname(bt) && !(bt->tag & (TT_ATTR | TT_TYPE | TT_QUALIFIER | TT_SUE))) fn = bt;
						break;
					}
				}
				if (fn && fn->len == inner->len &&
				    memcmp(tok_loc(fn), tok_loc(inner), inner->len) == 0)
					return true;
			}
			break;
		}
	}
	// func_meta only has defined functions.  Also check forward declarations.
	// But skip if the name is shadowed by a local variable at this position.
	if (typeof_ident_loc && typeof_inner) {
		TypedefEntry *shadow = typedef_lookup(typeof_inner);
		if (shadow && shadow->is_shadow) return false;
		return hashmap_get(&p1_func_proto_map, typeof_ident_loc, typeof_ident_len) != NULL;
	}
	return false;
}

// Result of walking an initializer expression for orelse/noreturn detection.
typedef struct {
	Token *tok;
	Token *unreachable_tok;
	bool hit_orelse;
} InitWalkResult;

// Walk initializer tokens, emitting them and detecting orelse keywords
// and noreturn calls. Stops at ',' or ';' or orelse keyword.
static InitWalkResult emit_decl_init_walk(Token *tok) {
	InitWalkResult r = {tok, NULL, false};
	int init_ternary = 0;
	while (r.tok->kind != TK_EOF) {
		if (r.tok->flags & TF_OPEN) {
			if (FEAT(F_ORELSE)) check_orelse_in_parens(r.tok);
			r.tok = walk_balanced(r.tok, true);
			continue;
		}
		if (match_ch(r.tok, ',') || match_ch(r.tok, ';')) break;
		if (match_ch(r.tok, '?')) { init_ternary++; emit_tok(r.tok); r.tok = tok_next(r.tok); continue; }
		if (match_ch(r.tok, ':') && init_ternary > 0) { init_ternary--; emit_tok(r.tok); r.tok = tok_next(r.tok); continue; }
		if (r.tok->tag & TT_GENERIC) {
			Token *after = try_generic_member_rewrite(r.tok);
			if (after) { r.tok = after; continue; }
		}
		if (FEAT(F_ORELSE) && is_orelse_keyword(r.tok)) {
			if (init_ternary > 0)
				error_tok(r.tok, "'orelse' cannot be used inside a ternary expression");
			r.hit_orelse = true;
			break;
		}
		if (FEAT(F_AUTO_UNREACHABLE) && !in_ctrl_paren()) {
			Token *nr = try_detect_noreturn_call(r.tok);
			if (nr) r.unreachable_tok = nr;
		}
		emit_tok(r.tok); r.tok = tok_next(r.tok);
	}
	return r;
}

// Handle orelse hit during init walk. Returns true if the caller should
// continue the declarator loop (comma continuation), false if done.
static bool process_init_orelse_hit(Token **tok_p, DeclResult *decl,
				    Token *type_start, TypeSpecResult *type,
				    bool brace_wrap) {
	Token *tok = *tok_p;

	reject_orelse_in_for_init(tok);
	out_char(';');
	flush_typeof_memsets(ctx->typeof_vars, &ctx->typeof_var_count, type);

	tok = tok_next(tok); // skip 'orelse'
	OrelseDeclTargetInfo target = analyze_decl_orelse_target(tok, type_start, type, decl);

	if (target.is_struct_value)
		error_tok(decl->var_name,
			  "orelse on struct/union values is not supported (memcmp "
			  "cannot reliably detect zero due to padding)");
	tok = emit_orelse_action(
	    tok, decl->var_name, target.has_const_qual, type->has_volatile, target.stop_comma);

	if (target.stop_comma && match_ch(tok, ',')) {
		*tok_p = tok_next(tok);
		return true;
	}
	if (brace_wrap) OUT_LIT(" }");
	*tok_p = tok;
	return false;
}

// Check if a multi-declarator should be split at ','.
// Split when pending typeof memsets or bracket orelse on the next declarator.
static bool should_split_multi_decl(Token *next_decl_tok) {
	if (in_for_init()) return false;
	DeclResult next_decl = parse_declarator(next_decl_tok, false);
	if (!next_decl.end) return false;
	if (ctx->typeof_var_count > 0 && next_decl.var_name && next_decl.has_init)
		return true;
	if (FEAT(F_ORELSE) && declarator_has_bracket_orelse(next_decl_tok, next_decl.end))
		return true;
	return false;
}

// Validate that splitting a declaration doesn't split an anonymous struct/union,
// which would produce two incompatible anonymous types.
static void validate_no_anon_struct_split(Token *next_decl_tok, Token *type_start,
					  TypeSpecResult *type) {
	if (!type->is_struct || type->is_enum) return;
	for (Token *t = type_start; t && t != type->end; t = tok_next(t)) {
		if (t->tag & TT_SUE) {
			Token *after = skip_prep_dirs(tok_next(t));
			if (after && match_ch(after, '{'))
				error_tok(next_decl_tok,
					  "bracket orelse / zero-init requiring declaration split "
					  "cannot be used with anonymous struct/union; "
					  "add a tag name or use a typedef");
			break;
		}
	}
}

// Handle const + orelse fallback declaration path.
// Returns true if the caller should continue the declarator loop (comma
// continuation with need_type_emit), false if done (caller returns *tok_p).
static bool process_const_orelse_decl(Token **tok_p, Token *orelse_tok,
				      Token *decl_start, DeclResult *decl,
				      Token *type_start, TypeSpecResult *type,
				      Token *pragma_start, bool brace_wrap) {
	Token *val_start = tok_next(decl->end); // First value token after '='
	Token *tok = tok_next(orelse_tok); // skip 'orelse'
	OrelseDeclTargetInfo target = analyze_decl_orelse_target(tok, type_start, type, decl);

	if (target.is_struct_value)
		error_tok(orelse_tok,
			  "orelse value fallback on const/typeof aggregate "
			  "is not supported; use a control flow action "
			  "(return/break/goto), typeof_unqual, or an "
			  "explicit type name");

	if (type->is_vla || decl->is_vla)
		error_tok(orelse_tok,
			  "orelse on a const-qualified variably-modified type "
			  "would duplicate the type specifier, causing VLA "
			  "size expressions to be evaluated twice; hoist the "
			  "value to a non-const variable first");

	tok = handle_const_orelse_fallback(tok,
					   orelse_tok,
					   val_start,
					   decl_start,
					   decl,
					   type_start,
					   type,
					   pragma_start,
					   target.stop_comma);

	flush_typeof_memsets(ctx->typeof_vars, &ctx->typeof_var_count, type);

	if (match_ch(tok, ';')) tok = tok_next(tok);
	end_statement_after_semicolon();

	if (target.stop_comma && match_ch(tok, ',')) {
		*tok_p = tok_next(tok);
		return true;
	}
	if (brace_wrap) OUT_LIT(" }");
	*tok_p = tok;
	return false;
}

// Process all declarators in a declaration and emit with zero-init.
static Token *process_declarators(Token *tok, TypeSpecResult *type, bool is_raw, Token *type_start,
				  Token *pragma_start, Token *raw_tok, bool brace_wrap) {
	ctx->typeof_var_count = 0; // Reset count; reuse arena-allocated array across calls
	bool first_decl = true;
	bool need_type_emit = false; // Set after orelse comma — deferred to after next lookahead

	while (tok && tok->kind != TK_EOF) {
		Token *decl_start = tok;

		// Per-declarator 'raw' detection (comma-separated: int x, raw y;)
		// Also handles redundant per-declarator raw when is_raw is already set
		// (raw int x, raw y; — second 'raw' must be consumed to prevent leak).
		bool decl_is_raw = is_raw;
		if ((tok->flags & TF_RAW) && !is_known_typedef(tok)) {
			Token *after = skip_noise(tok_next(tok));
			if (after && ((is_valid_varname(after) && !is_type_keyword(after) &&
				       !is_known_typedef(after) && !(after->tag & (TT_QUALIFIER | TT_SUE))) ||
				      match_ch(after, '*') || match_ch(after, '('))) {
				Token *last_raw = tok;
				SKIP_RAW(after, last_raw);
				emit_noise_between_raws(tok, last_raw);
				decl_start = tok_next(last_raw);
				tok = after;
				decl_is_raw = true;
			}
		}

		// Step 1: Non-emitting lookahead
		DeclResult decl = parse_declarator(tok, false);
		if (!decl.end || !decl.var_name) {
			if (!first_decl) {
				if (need_type_emit) {
					if ((type->has_typeof || type->has_atomic) && type->is_vla)
						error_tok(tok, "multi-declarator with variably-modified "
							  "type specifier requires declaration split which "
							  "would double-evaluate VLA size expressions; "
							  "declare each variable on a separate line");
					emit_type_range(type_start, type->end, false, true);
				}
				goto emit_raw_bail;
			}
			return NULL;
		}

		// Step 2: If initializer exists, scan for orelse + const fallback pattern
		OrelseInitInfo orelse_info = scan_decl_orelse(decl.end, type_start, type, &decl);
		bool is_const_orelse_fallback = orelse_info.is_const_fallback;

		// Static/extern/thread-local initializers must be constant expressions
		// (C11 §6.7.9p4). Orelse splits the declaration into a runtime
		// assignment that re-executes on every function entry, destroying
		// persistence semantics.
		if (orelse_info.orelse_tok && (type->has_static || type->has_extern))
			error_tok(orelse_info.orelse_tok,
				  "'orelse' cannot be used in the initializer of a "
				  "variable with static or thread storage duration "
				  "(the runtime fallback check would re-execute on "
				  "every function entry, destroying persistence)");

		// Step 2b: Pre-hoist bracket orelse temps (before type emission)
		bool has_bo = FEAT(F_ORELSE) && declarator_has_bracket_orelse(decl_start, decl.end);
		bool brace_opened = false;
		if (has_bo) {
			if (in_ctrl_paren())
				error_tok(decl_start,
					  "bracket orelse in VLA dimensions cannot be used in "
					  "control statement conditions (hoisted temps would "
					  "inject invalid syntax); move the declaration before "
					  "the statement");
			if (first_decl && brace_wrap) { OUT_LIT(" {"); brace_opened = true; }
			emit_bracket_orelse_temps(decl_start, decl.end);
		}

		// Deferred type emission from orelse comma continuation
		if (need_type_emit) {
			// Splitting a typeof VLA re-emits the type specifier, causing
			// the VLA dimension to be evaluated twice (ISO C11 §6.7.2.5).
			if ((type->has_typeof || type->has_atomic) && type->is_vla)
				error_tok(decl_start, "multi-declarator with variably-modified "
					  "type specifier requires declaration split which "
					  "would double-evaluate VLA size expressions; "
					  "declare each variable on a separate line");
			if (!is_const_orelse_fallback) {
				if (pragma_start != type_start) {
					if (raw_tok)
						emit_range(pragma_start, raw_tok);
					else
						emit_range(pragma_start, type_start);
				}
				emit_type_range(type_start, type->end, false, true);
			}
			need_type_emit = false;
		}

		// Step 3: Emit base type for first declarator
		if (first_decl) {
			if (brace_wrap && !brace_opened) OUT_LIT(" {");
			if (!is_const_orelse_fallback) {
				if (raw_tok && pragma_start != type_start) emit_range(pragma_start, raw_tok);
				else if (pragma_start != type_start) emit_range(pragma_start, type_start);
				emit_type_range(type_start, type->end, false, false);
			}
			first_decl = false;
		}

		// Step 4: Emit declarator & initializer
		bool effective_vla = (decl.is_vla && (!decl.paren_pointer || decl.paren_array)) || (type->is_vla && !decl.is_pointer);
		bool is_aggregate =
		    (decl.is_array && (!decl.paren_pointer || decl.paren_array)) || ((type->is_struct || type->is_typedef) && !decl.is_pointer);
		// Function types (via typedef) cannot be initialized — skip zero-init entirely.
		bool is_func_type = is_typeof_func_type(type_start, type, &decl);
		// Only queue memset when zeroinit feature is enabled.
		// Static/extern variables are zero-initialized by the loader — skip.
		bool needs_memset = FEAT(F_ZEROINIT) && !decl.has_init && !decl_is_raw && (!decl.is_pointer || decl.is_array) &&
				    !type->has_register && !type->has_static && !type->has_extern && !is_func_type &&
				    (type->has_typeof || (type->has_atomic && is_aggregate) || effective_vla);

		if (is_const_orelse_fallback) {
			if (process_const_orelse_decl(&tok, orelse_info.orelse_tok, decl_start,
						     &decl, type_start, type, pragma_start, brace_wrap)) {
				need_type_emit = true;
				continue;
			}
			return tok;
		}

		// Normal path: emit declarator
		parse_declarator(decl_start, true);
		tok = decl.end;

		// Add zero initializer if needed (for non-memset types)
		if (FEAT(F_ZEROINIT) && !decl.has_init && !effective_vla && !decl_is_raw && !needs_memset && !type->has_extern && !type->has_static && !is_func_type) {
			if (type->has_register && type->has_atomic && is_aggregate)
				error_tok(decl.var_name,
					  "'register _Atomic' aggregate cannot be safely "
					  "zero-initialized; remove 'register' or use 'raw' "
					  "to opt out of automatic initialization");
			else if (is_aggregate || type->has_typeof)
				OUT_LIT(" = {0}");
			else
				OUT_LIT(" = 0");
		}

		// Queue delayed memsets until the declaration can be safely split.
		if (needs_memset) {
			if (in_for_init())
				error_tok(decl.var_name,
					  "VLA or typeof variable in for/if/switch initializer "
					  "cannot be safely zero-initialized; move the "
					  "declaration before the statement");
			ARENA_ENSURE_CAP(&ctx->main_arena,
					 ctx->typeof_vars,
					 ctx->typeof_var_count + 1,
					 ctx->typeof_var_cap,
					 128,
					 Token *);
			ctx->typeof_vars[ctx->typeof_var_count++] = decl.var_name;
		}

		// Emit initializer if present
		Token *pd_unreachable_tok = NULL;
		if (decl.has_init) {
			InitWalkResult iw = emit_decl_init_walk(tok);
			tok = iw.tok;
			pd_unreachable_tok = iw.unreachable_tok;

			if (iw.hit_orelse) {
				if (process_init_orelse_hit(&tok, &decl, type_start, type, brace_wrap)) {
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
			flush_typeof_memsets(ctx->typeof_vars, &ctx->typeof_var_count, type);
			if (is_ur) EMIT_UNREACHABLE();
			if (brace_wrap) OUT_LIT(" }");
			return tok_next(tok);
		} else if (match_ch(tok, ',')) {
			Token *next_decl_tok = tok_next(tok);

			if (should_split_multi_decl(next_decl_tok)) {
				validate_no_anon_struct_split(next_decl_tok, type_start, type);
				out_char(';');
				flush_typeof_memsets(ctx->typeof_vars, &ctx->typeof_var_count, type);
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
	while (tok && tok->kind != TK_EOF && !match_ch(tok, ';')) { emit_tok(tok); tok = tok_next(tok); }
	if (tok && match_ch(tok, ';')) { emit_tok(tok); tok = tok_next(tok); }
	flush_typeof_memsets(ctx->typeof_vars, &ctx->typeof_var_count, type);
	if (brace_wrap) OUT_LIT(" }");
	return tok;
}

// Try to handle a declaration with zero-init. Returns token after declaration, or NULL.
static Token *try_zero_init_decl(Token *tok) {
	if (ctx->block_depth <= 0 || in_struct_body()) return NULL;
	/* Always skip when both features are off. When only F_ORELSE is on (no
	 * F_ZEROINIT), proceed only if a P1_OE_BRACKET-annotated token is present
	 * in the current declaration — bracket orelse hoisting needs
	 * emit_bracket_orelse_temps which is only reachable via process_declarators. */
	if (!FEAT(F_ZEROINIT) && !FEAT(F_ORELSE)) return NULL;
	if (!FEAT(F_ZEROINIT)) {
		/* Only proceed if there is a P1_OE_BRACKET- or P1_OE_DECL_INIT-
		 * annotated orelse token in this declaration.
		 * P1_OE_BRACKET: orelse inside [...] array-dimension brackets.
		 * P1_OE_DECL_INIT: orelse inside '= expr' declaration initializer.
		 * Both require process_declarators to expand them; without this
		 * gate the bare-assign orelse path fires with the type keyword as
		 * the LHS start, emitting a duplicate declaration (invalid C). */
		bool has_bo = false;
		for (Token *s = tok; s && s->kind != TK_EOF; s = tok_next(s)) {
			if (match_ch(s, ';') || match_ch(s, '{')) break;
			if (tok_ann(s) & (P1_OE_BRACKET | P1_OE_DECL_INIT)) { has_bo = true; break; }
		}
		if (!has_bo) return NULL;
	}

	if (tok->kind >=
	    TK_STR) // Fast reject: strings, numbers, prep directives, EOF can't start a declaration
		return NULL;

	// Declarations in switch body without braces can be skipped by case jumps.
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
		if (is_raw_declaration_context(after_raw)) {
			is_raw = true;
			raw_tok = tok;
			// Advance past raw keywords so tok reaches the actual
			// type keyword for the TT_DECL_START gate below.
			// No emit — deferred to after probe commits.
			Token *last_raw = tok;
			SKIP_RAW(after_raw, last_raw);
			raw_last = last_raw;
			start = tok_next(last_raw);
			tok = after_raw;
			if (pragma_start == raw_tok) pragma_start = start;
			warn_loc = after_raw;
		}
	}

	// Probe past qualifiers/attrs/storage to find 'raw' after prefix.
	if (!is_raw) {
		Token *probe = start;
		while (probe && probe->kind != TK_EOF) {
			SKIP_NOISE_CONTINUE(probe);
			if (probe->tag & TT_QUALIFIER) { probe = tok_next(probe); continue; }
			if ((probe->tag & (TT_STORAGE | TT_TYPEDEF))) { probe = tok_next(probe); continue; }
			break;
		}
		if (probe && (probe->flags & TF_RAW) && !is_known_typedef(probe)) {
			Token *after_raw = skip_noise(tok_next(probe));
			if (is_raw_declaration_context(after_raw)) {
				// Skip chain of consecutive raw keywords
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

	if ((tok->tag & TT_SKIP_DECL) && !(tok->tag & TT_STORAGE)) // Control flow, etc. (not storage class)
	{
		if (is_raw) {
			// File-storage raw (static/extern/thread_local): emit verbatim.
			// No zero-init needed — C guarantees zero-init for file/static storage.
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
			if (ann->tag & (TT_STORAGE | TT_INLINE | TT_SKIP_DECL))
				{ ann = tok_next(ann); continue; }
			break;
		}
		if (ann && !(tok_ann(ann) & P1_IS_DECL)) return NULL;
	}

	TypeSpecResult type = parse_type_specifier(tok);
	if (!type.saw_type) return NULL;

	// parse_type_specifier now detects embedded raw (e.g. const raw int)
	if (type.has_raw && !is_raw) {
		is_raw = true;
		// Find the raw_tok for emit_noise_between_raws in process_declarators
		for (Token *r = start; r && r != type.end; r = tok_next(r))
			if ((r->flags & TF_RAW) && !is_known_typedef(r)) { raw_tok = r; break; }
	}

	// Single-declarator statement-expression initializers (int x = ({...});)
	// are emitted verbatim — process_declarators' orelse scanner can't handle ({}).
	// Exception: if orelse follows the stmt-expr, process_declarators handles
	// it correctly via its initializer orelse path (assign + if-check, no double eval).
	{
		DeclResult probe = parse_declarator(type.end, false);
		if (!probe.var_name || !probe.end) return NULL;
		if (match_ch(probe.end, '=')) {
			Token *aeq = tok_next(probe.end);
			if (aeq && is_stmt_expr_open(aeq) && tok_match(aeq)) {
				Token *after_se = tok_next(tok_match(aeq));
				bool is_orelse = after_se && is_orelse_keyword(after_se);
				if (!after_se || (!match_ch(after_se, ',') && !is_orelse)) {
					// Bail out to verbatim emit, but still check
					// defer shadow — process_declarators won't run.
					// Skip in braceless bodies (synthetic scope).
					if (!(ctrl_state.pending && ctrl_state.parens_just_closed))
						check_defer_var_shadow(probe.var_name);
					return NULL;
				}
			}
		}
	}

	if (FEAT(F_ZEROINIT) && in_switch_scope_unbraced && !is_raw && !in_for_init()) {
		error_tok(warn_loc,
			  "variable declaration directly in switch body without braces. "
			  "Wrap in braces: 'case N: { int x; ... }' to ensure safe zero-initialization, "
			  "or use 'raw' to suppress zero-init.");
	}

	// Braceless control body: wrap in braces (orelse expands to multiple stmts).
	bool brace_wrap = ctrl_state.pending && ctrl_state.parens_just_closed;

	// Emit attributes/noise between raw keywords before process_declarators.
	// Deferred to here (not the probe block) so nothing is emitted if the
	// probe bails out via an early return NULL.
	if (is_raw && raw_tok && raw_last)
		emit_noise_between_raws(raw_tok, raw_last);

	return process_declarators(type.end, &type, is_raw, start, pragma_start, raw_tok, brace_wrap);
}

// --- Statement Processing ---

// Emit expression to semicolon, handling zero-init in statement expressions.
static Token *emit_expr_to_semicolon(Token *tok) {
	int brace_depth = 0;
	int ternary_depth = 0;
	bool expr_at_stmt_start = false;
	while (tok->kind != TK_EOF) {
		// Skip matched (...) / [...] groups — no declarations inside.
		// Statement expressions ({...}): route through walk_balanced which
		// has the full keyword dispatcher (defer, goto, orelse, zeroinit).
		if ((match_ch(tok, '(') || match_ch(tok, '[')) && tok_match(tok)) {
				if (FEAT(F_ORELSE) && (match_ch(tok, '(') || match_ch(tok, '[')))
				check_orelse_in_parens(tok);
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
			if (next) { tok = next; expr_at_stmt_start = true; continue; }
			expr_at_stmt_start = false;
		}

		emit_tok(tok);

		if (match_ch(tok, ';') || match_ch(tok, '{') || match_ch(tok, '}'))
			expr_at_stmt_start = true;
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

// Check if 'defer' appears inside an attribute context: __attribute__((..., defer, ...))
static inline bool is_inside_attribute(Token *tok) {
	if (!last_emitted || (!match_ch(last_emitted, '(') && !match_ch(last_emitted, ','))) return false;
	for (Token *t = tok; t && t->kind != TK_EOF && !match_ch(t, ';') && !match_ch(t, '{'); t = tok_next(t)) {
		if (t->flags & TF_OPEN) { t = tok_match(t); continue; }
		if (match_ch(t, ')')) return true;
	}
	return false;
}

// Handle 'defer' keyword: validate context, record deferred statement.
// Returns next token after the defer statement, or NULL if tok is not a valid defer.
static Token *handle_defer_keyword(Token *tok) {
	if (!FEAT(F_DEFER)) return NULL;
	// Distinguish struct field, label, goto target, variable assignment, attribute usage
	if (match_ch(tok_next(tok), ':') || (last_emitted && (last_emitted->tag & (TT_MEMBER | TT_GOTO))) ||
	    (last_emitted && (is_type_keyword(last_emitted) || (last_emitted->tag & TT_TYPEDEF))) ||
	    (typedef_lookup(tok) && !match_ch(tok_next(tok), '{')) ||
	    (tok_next(tok) && (tok_next(tok)->tag & TT_ASSIGN)) || in_struct_body() ||
	    /* attribute context: defer inside __attribute__((...)) */
	    is_inside_attribute(tok))
		return NULL;

	// Context validation
	if (in_ctrl_paren())
		error_tok(tok, "defer cannot appear inside control statement parentheses");
	if (ctrl_state.pending && !in_ctrl_paren())
		error_tok(tok, "defer requires braces in control statements (braceless has no scope)");
	if (scope_block_top() && scope_block_top()->is_stmt_expr)
		error_tok(tok,
			  "defer cannot be at top level of statement expression; wrap in a block");
	for (int d = ctx->scope_depth - 1; d >= 0; d--) {
		if (scope_stack[d].kind != SCOPE_BLOCK) continue;
		if (scope_stack[d].is_switch)
			error_tok(tok, "defer in switch case requires braces");
		break;  // only check innermost block
	}

	Token *defer_keyword = tok;
	tok = tok_next(tok);
	Token *stmt_start = tok;

	// Block defer: defer { ... } with optional trailing semicolon
	if (match_ch(stmt_start, '{') && tok_match(stmt_start)) {
		Token *close = tok_match(stmt_start);
		Token *after = tok_next(close);
		Token *stmt_end = after;  // exclusive boundary — emits up to but not including
		defer_add(defer_keyword, stmt_start, stmt_end);
		tok = after;
			// Consume optional trailing semicolon
		{
			if (tok && match_ch(tok, ';')) tok = tok_next(tok);
			end_statement_after_semicolon();
		}
		return tok;
	}

	Token *stmt_end = skip_to_semicolon(tok);

	if (stmt_end->kind == TK_EOF || !match_ch(stmt_end, ';'))
		error_tok(defer_keyword, "unterminated defer statement; expected ';'");

	// Validate inner control flow (returns, missing semicolons, breaks outside loops)
	if (stmt_start && stmt_start->kind == TK_KEYWORD &&
	    (stmt_start->tag & (TT_NON_EXPR_STMT | TT_DEFER)))
		error_tok(defer_keyword,
			  "defer statement appears to be missing ';' (found '%.*s' keyword inside)",
			  stmt_start->len, tok_loc(stmt_start));

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

// Emit return body with defer cleanup. tok = first token after 'return'.
static Token *emit_return_body(Token *tok, Token *stop) {
	bool active = FEAT(F_DEFER) && has_active_defers();
	bool is_empty = match_ch(tok, ';') || (stop && tok == stop);

	if (active) {
		if (is_empty) {
			emit_all_defers();
			OUT_LIT(" return;");
		} else {
			bool is_void = is_void_return(tok);
			if (!is_void) {
				out_char(' '); emit_ret_type();
				OUT_LIT(" __prism_ret_"); out_uint(ctx->ret_counter); OUT_LIT(" = (");
			} else OUT_LIT(" (");

			if (stop)
				tok = emit_expr_to_stop(tok, stop, false);
			else
				tok = emit_expr_to_semicolon(tok);
			OUT_LIT(");");
			emit_all_defers();

			if (!is_void) { OUT_LIT(" return __prism_ret_"); out_uint(ctx->ret_counter++); }
			else OUT_LIT(" return");
			out_char(';');
		}
	} else {
		OUT_LIT(" return");
		if (!is_empty) {
			out_char(' ');
			if (stop)
				tok = emit_expr_to_stop(tok, stop, false);
			else
				tok = emit_expr_to_semicolon(tok);
		}
		out_char(';');
	}

	if (match_ch(tok, ';')) tok = tok_next(tok);
	return tok;
}

static Token *emit_orelse_action(Token *tok, Token *var_name, bool has_const, bool has_volatile, Token *stop_comma) {
	require_orelse_action(tok, stop_comma);

	if (match_ch(tok, '{')) {
		if (var_name) {
			OUT_LIT(" if (!");
			OUT_TOK(var_name);
			out_char(')');
		}
		// Previously returned '{' for deferred processing, but that broke
		// process_declarators (brace_wrap/stop_comma emitted before block).
		tok = emit_orelse_block_body(tok);
		if (tok && match_ch(tok, ';')) tok = tok_next(tok);
		return tok;
	}

	if (tok->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) {
		uint64_t tag = tok->tag;
		if (tag & TT_RETURN) tok = tok_next(tok);
		if (var_name) {
			OUT_LIT(" if (!");
			OUT_TOK(var_name);
			OUT_LIT(") {");
		} else
			OUT_LIT(" {");
		if (tag & TT_RETURN) tok = emit_return_body(tok, stop_comma);
		else if (tag & (TT_BREAK | TT_CONTINUE)) tok = emit_break_continue_defer(tok);
		else tok = emit_goto_defer(tok);
		OUT_LIT(" }");
		end_statement_after_semicolon();
		return tok;
	}

	if (!var_name) error_tok(tok, "orelse fallback requires an assignment target (use a declaration)");
	if (has_const) error_tok(tok, "orelse fallback cannot reassign a const-qualified variable");

	if (has_volatile) {
		// Volatile: use if-based pattern to avoid double-read.
		// "var = var ? var : fb" reads volatile twice (condition + true branch).
		// "if (!var) var = fb;" reads only once (condition).
		OUT_LIT(" if (!");
		OUT_TOK(var_name);
		OUT_LIT(") ");
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
		out_char(';');
		return emit_orelse_action(chain_next, var_name, has_const, has_volatile, stop_comma);
	}
	out_char(';');
	if (match_ch(tok, ';')) tok = tok_next(tok);
	end_statement_after_semicolon();
	return tok;
}

// Handle 'return', 'break', or 'continue' with active defers.
// Returns next token if handled, or NULL to let normal emit proceed.
static Token *handle_control_exit_defer(Token *tok) {
	if (tok->tag & TT_RETURN) {
		if (!has_active_defers()) return NULL;
		tok = tok_next(tok);
		OUT_LIT(" {");
		tok = emit_return_body(tok, NULL);
		OUT_LIT(" }");
	} else {
		bool is_break = tok->tag & TT_BREAK;
		if (!control_flow_has_defers(is_break)) return NULL;
		OUT_LIT(" {");
		tok = emit_break_continue_defer(tok);
		OUT_LIT(" }");
	}
	end_statement_after_semicolon();
	return tok;
}

// Find a label in the current function's Phase 1D P1FuncEntry array.
// Returns scope_depth and token pointer for the label, or {0, NULL} if not found.
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
			if (e->label.len == tok->len &&
			    !memcmp(e->label.name, tok_loc(tok), tok->len))
				return (P1LabelResult){scope_tree_depth(e->scope_id), e->tok};
		}
		return (P1LabelResult){0, NULL};
	}
	for (int i = 0; i < fm->entry_count; i++) {
		if (entries[i].kind == P1K_LABEL &&
		    entries[i].label.len == tok->len &&
		    !memcmp(entries[i].label.name, tok_loc(tok), tok->len))
			return (P1LabelResult){scope_tree_depth(entries[i].scope_id), entries[i].tok};
	}
	return (P1LabelResult){0, NULL};
}

// Look up pre-computed scope exits for a goto at `goto_tok`.
// Scans P1K_GOTO entries from cursor position (entries are in token order).
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

// Handle 'goto': defer cleanup emission
static Token *handle_goto_keyword(Token *tok) {
	Token *goto_tok = tok;
	tok = tok_next(tok);

	if (FEAT(F_DEFER)) {
		// Skip C23 attributes between goto and target: goto [[attr]] *ptr;
		Token *after_attrs = skip_noise(tok);

		if (match_ch(after_attrs, '*')) {
			if (has_active_defers())
				error_tok(goto_tok, "computed goto cannot be used with active defer statements");
			emit_tok(goto_tok);
			while (tok != after_attrs) {
				emit_tok(tok); tok = tok_next(tok);
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
					emit_tok(tok); tok = tok_next(tok);
				}
				emit_tok(tok); tok = tok_next(tok);
				if (match_ch(tok, ';')) { emit_tok(tok); tok = tok_next(tok); }
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
	// Compound literal inside control parens or before body
	bool did_push = false;
	if (ctrl_state.pending && (in_ctrl_paren() || !ctrl_state.parens_just_closed || (tok_ann(tok) & P1_SCOPE_INIT))) {
		// Save ctrl_state so it can be restored when this block closes.
		if (last_emitted && match_ch(last_emitted, '(')) {
			ENSURE_ARRAY_CAP(ctrl_save_stack, ctrl_save_depth + 1, ctrl_save_cap, 16, CtrlState);
			ctrl_save_stack[ctrl_save_depth++] = ctrl_state;
			did_push = true;
			// Fall through to normal block processing
		} else {
			emit_tok(tok);
			ctrl_state.brace_depth++;
			return tok_next(tok);
		}
	}
	// Consume pending state
	ctrl_state.pending = false;
	ctrl_state.pending_for_paren = false;
	ctrl_state.parens_just_closed = false;

	// Evaluate BEFORE emit_tok
	bool is_stmt_expr = last_emitted && match_ch(last_emitted, '(');
	/* Initializer brace: '= {'.  Tag as is_struct so that in_struct_body()
	 * returns true, suppressing the bare-expression orelse transform firing
	 * on designated initializer tokens like '.x = val orelse fallback'.
	 * With F_ZEROINIT, try_zero_init_decl handles the whole declaration
	 * before the main loop sees these tokens.  With F_ZEROINIT off, this
	 * tag ensures orelse produces a clear error instead of corrupted output. */
	bool is_initializer = last_emitted && match_ch(last_emitted, '=');

	uint8_t ann = tok_ann(tok);
	bool is_init_scope = is_initializer || (ann & P1_SCOPE_INIT);
	emit_tok(tok); tok = tok_next(tok);
	scope_push_kind(is_init_scope ? SCOPE_INIT : SCOPE_BLOCK);

	ScopeNode *s = &scope_stack[ctx->scope_depth - 1];
	s->is_loop = ann & P1_SCOPE_LOOP;
	s->is_switch = ann & P1_SCOPE_SWITCH;
	if (is_stmt_expr) s->is_stmt_expr = true;
	if (did_push)
		s->is_ctrl_se = true;
	if (is_init_scope) s->is_struct = true;

	ctx->at_stmt_start = true;
	return tok;
}

static Token *handle_close_brace(Token *tok) {
	// Compound literal close inside control parens
	if (ctrl_state.pending && in_ctrl_paren() && ctrl_state.brace_depth > 0) {
		ctrl_state.brace_depth--;
		emit_tok(tok);
		return tok_next(tok);
	}
	// Pop stale non-BLOCK scopes (leaked ternary/ctrl/generic scopes)
	while (ctx->scope_depth > 0 && !is_brace_scope(scope_stack[ctx->scope_depth - 1].kind))
		scope_pop();
	if (FEAT(F_DEFER) && ctx->scope_depth > 0) {
		ScopeNode *s = &scope_stack[ctx->scope_depth - 1];
		if (defer_count > s->defer_start_idx) {
			// Detect: this block is the last statement of an enclosing
			// defer call AFTER the last C expression in LIFO order,
			// overwriting the statement expression's return value with
			// the defer's result type (void for free/cleanup → compile
			// error; int for counter++ → silent wrong-value assignment).
			// Without an expression parser we cannot safely capture the
			// last expression into a temp, so reject unconditionally.
			// Detection: walk up the scope stack to find any ancestor
			// stmt_expr.  At each level, verify the next structural
			// token after the current '}' is another '}' (possibly
			// separated by empty stmts ';' and labels 'id:'), forming
			// a chain of closing braces up to the stmt_expr scope.
			{
				Token *nxt = skip_noise(tok_next(tok));
				for (int depth = ctx->scope_depth - 2; depth >= 0; depth--) {
					// Walk past empty stmts and labels to find the real next block-level token
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
					if (!probe || !match_ch(probe, '}')) break; // not a chain of closing braces
					if (scope_stack[depth].is_stmt_expr)
						error_tok(defer_stack[s->defer_start_idx].defer_kw,
							  "defer inside a block that is the last "
							  "statement of a statement expression "
							  "would corrupt the expression's return "
							  "value; ensure the last statement of the "
							  "statement expression is outside the "
							  "defer block");
					// Continue upward: the next '}' closes scope_stack[depth]
					nxt = skip_noise(tok_next(probe));
				}
			}
			emit_defers(DEFER_SCOPE);
			defer_count = s->defer_start_idx;
		}
	}

	// Check ctrl_se BEFORE popping the scope
	bool restore_ctrl = ctx->scope_depth > 0 && scope_stack[ctx->scope_depth - 1].is_ctrl_se;

	scope_pop();

	// Restore ctrl_state for stmt-expr that was inside ctrl parens
	if (restore_ctrl && ctrl_save_depth > 0)
		ctrl_state = ctrl_save_stack[--ctrl_save_depth];

	emit_tok(tok);

	tok = tok_next(tok);
	ctx->at_stmt_start = true;
	return tok;
}

// --- Process Management ---

// Build a copy of 'environ' with CC and PRISM_CC removed (cached, thread-safe)
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
	// If another thread raced us, we leak a small allocation — acceptable.
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
		if (devnull >= 0)
			posix_spawn_file_actions_adddup2(&actions, devnull, STDERR_FILENO);
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

// Run a command and wait for completion; returns exit status or -1
// Windows: defined in windows.c via _spawnvp
#ifndef _WIN32
static int run_command(char **argv) {
	return spawn_command(argv, false);
}

static int run_command_quiet(char **argv) {
	return spawn_command(argv, true);
}
#endif

// Create temp file with optional suffix. If source_adjacent is set, tries
// to create next to that file first, falls back to TMPDIR.
// Returns the open fd (>= 0) on success, -1 on failure. Caller must close.
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
		} else strcpy(dir_path, ".");
		if (dir_has_write_bits(dir_path)) {
			if (slash) {
				int dir_len = (int)(slash - source_adjacent);
				n = snprintf(buf, bufsize, "%.*s/.%s.XXXXXX.c", dir_len, source_adjacent, slash + 1);
			} else
				n = snprintf(buf, bufsize, ".%s.XXXXXX.c", source_adjacent);
			suffix_len = 2;
			if (n >= 0 && (size_t)n < bufsize) {
				int fd = mkstemps(buf, suffix_len);
				if (fd >= 0) return fd;
			}
		}
		// Source directory not writable, fall back to TMPDIR
		const char *base = slash ? slash + 1 : source_adjacent;
		n = snprintf(buf, bufsize, "%s.%s.XXXXXX.c", get_tmp_dir(), base);
		suffix_len = 2;
	} else
		n = snprintf(buf, bufsize, "%s%s", get_tmp_dir(), prefix ? prefix : "prism_tmp");
	if (n < 0 || (size_t)n >= bufsize) return -1;
	return suffix_len > 0 ? mkstemps(buf, suffix_len) : mkstemp(buf);
}

// Create a temp file and atomically register it for signal cleanup.
// Blocks SIGINT/SIGTERM across the creation+registration boundary so that
// a signal arriving between mkstemps() and signal_temps_register() cannot
// orphan the inode.
static int
make_temp_file_registered(char *buf, size_t bufsize, const char *prefix,
			  int suffix_len, const char *source_adjacent) {
	sigset_t mask, oldmask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigprocmask(SIG_BLOCK, &mask, &oldmask);

	int fd = make_temp_file(buf, bufsize, prefix, suffix_len, source_adjacent);
	if (fd >= 0)
		signal_temps_register(buf);

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

// Extract the first whitespace-delimited token from a CC string.
// e.g. "ccache gcc" -> "ccache", "gcc -m32" -> "gcc"
// Handles quoted paths: "\"C:\\Program Files\\cl.exe\" /nologo" -> "C:\\Program Files\\cl.exe"
static const char *cc_executable(const char *cc) {
	if (!cc || !*cc) return cc;
	const char *p = cc;
	while (*p == ' ' || *p == '\t') p++;
	static PRISM_THREAD_LOCAL char buf[PATH_MAX];
	if (*p == '"') {
		p++; // skip opening quote
		const char *end = strchr(p, '"');
		if (!end) end = p + strlen(p);
		size_t len = (size_t)(end - p);
		if (len >= sizeof(buf)) len = sizeof(buf) - 1;
		memcpy(buf, p, len);
		buf[len] = '\0';
		return buf;
	}
	while (*p && *p != ' ' && *p != '\t') p++;
	if (*p == '\0') return cc;
	size_t len = (size_t)(p - cc);
	if (len >= sizeof(buf)) len = sizeof(buf) - 1;
	memcpy(buf, cc, len);
	buf[len] = '\0';
	return buf;
}

// Split a CC string like "gcc -m32" or "ccache clang" into argv tokens.
// Inserts all tokens into args[*argc] and advances *argc.
static void cc_split_into_argv(const char **args, int *argc, const char *cc, char **out_dup) {
	if (out_dup) *out_dup = NULL;
	if (!cc || !*cc) return;
	char *dup = strdup(cc);
	if (!dup) { args[(*argc)++] = cc; return; }
	char *p = dup;
	while (*p) {
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;
		char quote = 0;
		if (*p == '\'' || *p == '"') { quote = *p++; }
		char *start = p;
		if (quote) {
			while (*p && *p != quote) p++;
			if (*p) *p++ = '\0';
		} else {
			while (*p && *p != ' ' && *p != '\t') p++;
			if (*p) *p++ = '\0';
		}
		args[(*argc)++] = start;
	}
	if (out_dup) *out_dup = dup;
}

// Count extra tokens in a CC string (tokens beyond the first).
// Handles quoted first token: "\"C:\\Program Files\\cl.exe\" /nologo" has 1 extra.
static int cc_extra_arg_count(const char *cc) {
	if (!cc || !*cc) return 0;
	int count = 0;
	const char *p = cc;
	while (*p == ' ' || *p == '\t') p++;
	// Skip the first token (possibly quoted)
	if (*p == '"') {
		p++; // skip opening quote
		while (*p && *p != '"') p++;
		if (*p == '"') p++; // skip closing quote
	} else while (*p && *p != ' ' && *p != '\t') p++;
	while (*p) {
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;
		count++;
		if (*p == '"') {
			p++;
			while (*p && *p != '"') p++;
			if (*p == '"') p++;
		} else while (*p && *p != ' ' && *p != '\t') p++;
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

// --- Preprocessor Integration ---

// Build preprocessor argv (shared between pipe and file paths)
static void build_pp_argv(const char **args, int *argc, const char *input_file, char **out_cc_dup) {
	const char *cc = ctx->extra_compiler ? ctx->extra_compiler : PRISM_DEFAULT_CC;
	bool msvc = cc_is_msvc(cc);
	cc_split_into_argv(args, argc, cc, out_cc_dup);

	if (msvc) {
		args[(*argc)++] = "/E";          // preprocess to stdout
		args[(*argc)++] = "/nologo";
	} else {
		args[(*argc)++] = "-E";
		args[(*argc)++] = "-w";
	}

	for (int i = 0; i < ctx->extra_compiler_flags_count; i++)
		args[(*argc)++] = ctx->extra_compiler_flags[i];

	for (int i = 0; i < ctx->dep_flags_count; i++)
		args[(*argc)++] = ctx->dep_flags[i];

	for (int i = 0; i < ctx->extra_include_count; i++)
	{
		args[(*argc)++] = msvc ? "/I" : "-I";
		args[(*argc)++] = ctx->extra_include_paths[i];
	}

	if (msvc) {
		// MSVC: /D concatenated with macro
		int needed = ctx->extra_define_count + 3; // +3 for __PRISM__, __PRISM_DEFER__, __PRISM_ZEROINIT__
		if (needed > pp_define_bufs_cap) {
			int old_cap = pp_define_bufs_cap;
			pp_define_bufs_cap = needed > 64 ? needed : 64;
			pp_define_bufs = realloc(pp_define_bufs, pp_define_bufs_cap * sizeof(char *));
			if (!pp_define_bufs) error("out of memory");
			for (int i = old_cap; i < pp_define_bufs_cap; i++)
				pp_define_bufs[i] = NULL;
		}
		int buf_idx = 0;
		for (int i = 0; i < ctx->extra_define_count; i++) {
			int len = snprintf(NULL, 0, "/D%s", ctx->extra_defines[i]) + 1;
			pp_define_bufs[buf_idx] = realloc(pp_define_bufs[buf_idx], len);
			if (!pp_define_bufs[buf_idx]) error("out of memory");
			snprintf(pp_define_bufs[buf_idx], len, "/D%s", ctx->extra_defines[i]);
			args[(*argc)++] = pp_define_bufs[buf_idx++];
		}
#define MSVC_DEFINE(str) do { \
	const char *_s = (str); int _l = (int)strlen(_s) + 1; \
	pp_define_bufs[buf_idx] = realloc(pp_define_bufs[buf_idx], _l); \
	if (!pp_define_bufs[buf_idx]) error("out of memory"); \
	memcpy(pp_define_bufs[buf_idx], _s, _l); \
	args[(*argc)++] = pp_define_bufs[buf_idx++]; \
} while (0)
		MSVC_DEFINE("/D__PRISM__=1");
		if (FEAT(F_DEFER)) MSVC_DEFINE("/D__PRISM_DEFER__=1");
		if (FEAT(F_ZEROINIT)) MSVC_DEFINE("/D__PRISM_ZEROINIT__=1");
#undef MSVC_DEFINE
	} else {
		for (int i = 0; i < ctx->extra_define_count; i++)
		{
			args[(*argc)++] = "-D";
			args[(*argc)++] = ctx->extra_defines[i];
		}
		args[(*argc)++] = "-D__PRISM__=1";
		if (FEAT(F_DEFER)) args[(*argc)++] = "-D__PRISM_DEFER__=1";
		if (FEAT(F_ZEROINIT)) args[(*argc)++] = "-D__PRISM_ZEROINIT__=1";
	}

	// Add POSIX/GNU feature test macros on non-Windows, non-MSVC
#ifndef _WIN32
	if (!msvc) {
		bool user_has_posix = false, user_has_gnu = false;
		for (int i = 0; i < ctx->extra_define_count; i++) {
			if (strncmp(ctx->extra_defines[i], "_POSIX_C_SOURCE", 15) == 0) user_has_posix = true;
			if (strncmp(ctx->extra_defines[i], "_GNU_SOURCE", 11) == 0) user_has_gnu = true;
		}
		for (int i = 0; i < ctx->extra_compiler_flags_count; i++) {
			const char *f = ctx->extra_compiler_flags[i];
			if (strncmp(f, "-D_POSIX_C_SOURCE", 17) == 0 ||
			    strncmp(f, "-U_POSIX_C_SOURCE", 17) == 0)
				user_has_posix = true;
			if (strncmp(f, "-D_GNU_SOURCE", 13) == 0 || strncmp(f, "-U_GNU_SOURCE", 13) == 0)
				user_has_gnu = true;
		}
		if (!user_has_posix) args[(*argc)++] = "-D_POSIX_C_SOURCE=200809L";
		if (!user_has_gnu) args[(*argc)++] = "-D_GNU_SOURCE";
	}
#endif

	for (int i = 0; i < ctx->extra_force_include_count; i++)
	{
		args[(*argc)++] = msvc ? "/FI" : "-include";
		args[(*argc)++] = ctx->extra_force_includes[i];
	}

	args[(*argc)++] = input_file;
	args[*argc] = NULL;
}

// Pre-scan source file for #define directives that appear before any #include.
// Build "#<directive text>\n" string from raw directive content.
static char *make_dir_line(const char *p, int len) {
	char *s = malloc(1 + len + 2);
	if (s) { s[0] = '#'; memcpy(s + 1, p, len); s[1 + len] = '\n'; s[2 + len] = '\0'; }
	return s;
}

// These ABI-altering macros (e.g. _FILE_OFFSET_BITS, _LARGEFILE_SOURCE) are
// consumed by cc -E and lost from un-flattened output unless we capture them.
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

// Scan a line segment for an unclosed /* block comment outside
// string/char/raw-string literals. Returns true if the segment ends
// inside an unterminated block comment.
// If raw_delim_out is non-NULL and the line ends inside an unclosed
// raw string literal, *raw_delim_out receives a malloc'd copy of the
// delimiter (empty string for R"(...)") and the function returns false.
static bool has_unclosed_block_comment(const char *p, char **raw_delim_out) {
	if (raw_delim_out) *raw_delim_out = NULL;
	bool in_str = false, in_chr = false;
	for (; *p && *p != '\n'; p++) {
		if (in_str) { if (*p == '\\' && p[1]) p++; else if (*p == '"') in_str = false; continue; }
		if (in_chr) { if (*p == '\\' && p[1]) p++; else if (*p == '\'') in_chr = false; continue; }
		if (*p == 'R' && p[1] == '"') {
			const char *q = p + 2;
			const char *dstart = q;
			while (*q && *q != '(' && *q != ')' && *q != '\\' && *q != ' ' &&
			       *q != '\t' && *q != '\n' && (q - dstart) < 17) q++;
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
				// Raw string not closed on this line
				if (raw_delim_out) {
					*raw_delim_out = malloc(dlen + 1);
					if (*raw_delim_out) { memcpy(*raw_delim_out, dstart, dlen); (*raw_delim_out)[dlen] = '\0'; }
				}
				return false;
			raw_closed:;
				continue;
			}
		} else if ((*p == 'u' || *p == 'U' || *p == 'L') && !in_str && !in_chr) {
			const char *rp = p;
			if (*rp == 'u' && rp[1] == '8') rp += 2; else rp++;
			if (*rp == 'R' && rp[1] == '"') { p = rp - 1; continue; } // will hit R" on next iter
		}
		if (*p == '"') { in_str = true; continue; }
		if (*p == '\'') { in_chr = true; continue; }
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
	bool in_raw_string = false;  // inside a multi-line raw string literal
	char *raw_delim = NULL;      // delimiter for the current raw string (malloc'd)
	int raw_delim_len = 0;
	int cond_depth = 0; // #if/#ifdef/#ifndef nesting depth

	// Condition stack: tracks raw directive text at each nesting level
	// for re-emitting conditional defines with their original guards.
	typedef struct {
		char *opening;       // e.g. "#ifdef __APPLE__\n"
		char *branches;      // accumulated "#else\n" or "#elif EXPR\n" text (NULL initially)
		int branches_len;
		int branches_cap;
		bool extractable;    // false if opening/branch had continuation (multi-line expr)
	} CondStackEntry;
	int cond_stack_cap = 32;
	CondStackEntry *cond_stack = calloc(cond_stack_cap, sizeof(CondStackEntry));
	if (!cond_stack) { free(line); fclose(f); return; }

	while (getline(&line, &line_cap, f) >= 0) {
		char *p = line;
		char *line_end;
		bool dir_has_continuation;
		char *dir_text_end;
		int dir_text_len;
		// Track multi-line block comments
		if (in_block_comment) {
			char *end = strstr(line, "*/");
			if (!end) continue;
			in_block_comment = false;
			// A #define may follow */ on the same line — fall through
			// to the normal '#' check with p pointing past the comment.
			p = end + 2;
			while (*p == ' ' || *p == '\t') p++;
			if (in_hash_block_comment) {
				// The '#' was already consumed on a previous line;
				// resume directive parsing directly.
				in_hash_block_comment = false;
				goto parse_directive;
			}
			if (*p != '#') continue;
			goto have_hash;
		}
		// Track multi-line raw string literals (R"delim(...)delim")
		if (in_raw_string) {
			// Search for )delim" on this line
			for (char *r = line; *r && *r != '\n'; r++) {
				if (*r == ')' &&
				    (raw_delim_len == 0 || strncmp(r + 1, raw_delim, raw_delim_len) == 0) &&
				    r[1 + raw_delim_len] == '"') {
					in_raw_string = false;
					free(raw_delim); raw_delim = NULL; raw_delim_len = 0;
					p = r + 2 + raw_delim_len;
					goto after_raw_string_close;
				}
			}
			continue; // entire line is inside raw string
		after_raw_string_close:
			while (*p == ' ' || *p == '\t') p++;
			if (*p == '\n' || *p == '\0') continue;
			// Rest of line may contain code/directives — fall through
			if (*p == '#') goto have_hash;
			// Check for block comment or another raw string on remainder
			{
				char *rd = NULL;
				if (has_unclosed_block_comment(p, &rd)) {
					in_block_comment = true;
				} else if (rd && cond_depth == 0) {
					in_raw_string = true;
					raw_delim = rd;
					raw_delim_len = (int)strlen(rd);
				} else free(rd);
			}
			continue;
		}
		// If previous line ended with '\', this is a continuation
		if (in_continuation) {
			char *end = line + strlen(line);
			while (end > line && (end[-1] == '\n' || end[-1] == '\r')) end--;
			in_continuation = (end > line && end[-1] == '\\');
			continue;
		}
		// Skip whitespace
		while (*p == ' ' || *p == '\t') p++;
		if (*p != '#') {
			// Skip blank lines and comments
			if (*p == '\n' || *p == '\0' || (p[0] == '/' && p[1] == '/')) goto check_continuation;
			if (p[0] == '/' && p[1] == '*') {
				char *close = strstr(p + 2, "*/");
				if (!close) { in_block_comment = true; goto check_continuation; }
				// Comment closes on same line — check rest for '#'
				p = close + 2;
				while (*p == ' ' || *p == '\t') p++;
				if (*p != '#') {
					char *rd = NULL;
					if (has_unclosed_block_comment(p, &rd))
						in_block_comment = true;
					else if (rd && cond_depth == 0) { in_raw_string = true; raw_delim = rd; raw_delim_len = (int)strlen(rd); }
					else free(rd);
					goto check_continuation;
				}
				goto have_hash;
			}
			// Non-preprocessor, non-blank line — scan for mid-line
			// block comment or raw string that spans subsequent lines.
			// Raw string detection suppressed inside #if/#ifdef blocks:
			// the C preprocessor doesn't lex tokens in dead branches,
			// so R"(...)" containing #endif would desync nesting.
			{
				char *rd = NULL;
				if (has_unclosed_block_comment(p, &rd))
					in_block_comment = true;
				else if (rd && cond_depth == 0) { in_raw_string = true; raw_delim = rd; raw_delim_len = (int)strlen(rd); }
				else free(rd);
			}
			goto check_continuation;
		}
	have_hash:
		p++; // skip '#'
		while (*p == ' ' || *p == '\t' || (p[0] == '/' && p[1] == '*')) {
			if (p[0] == '/' && p[1] == '*') {
				char *end = strstr(p + 2, "*/");
				if (!end) { in_block_comment = true; in_hash_block_comment = true; goto check_continuation; } // unterminated block comment
				p = end + 2;
			} else
				p++;
		}
	parse_directive: ;
		// Check if this directive line has a continuation (multi-line expression)
		line_end = p + strlen(p);
		while (line_end > p && (line_end[-1] == '\n' || line_end[-1] == '\r')) line_end--;
		dir_has_continuation = (line_end > p && line_end[-1] == '\\');

		// Capture raw directive text: "#directive args" (trimmed, no continuation backslash)
		dir_text_end = line_end;
		if (dir_has_continuation) dir_text_end--;
		while (dir_text_end > p && (dir_text_end[-1] == ' ' || dir_text_end[-1] == '\t')) dir_text_end--;
		dir_text_len = (int)(dir_text_end - p);

		// Track #if/#ifdef/#ifndef/#elif/#else/#endif nesting
		if (strncmp(p, "ifdef", 5) == 0 || strncmp(p, "ifndef", 6) == 0 ||
		    (strncmp(p, "if", 2) == 0 && (p[2] == ' ' || p[2] == '\t' || p[2] == '('))) {
			if (cond_depth >= cond_stack_cap) {
				int nc = cond_stack_cap * 2;
				CondStackEntry *ns = realloc(cond_stack, nc * sizeof(CondStackEntry));
				if (ns) {
					memset(ns + cond_stack_cap, 0, (nc - cond_stack_cap) * sizeof(CondStackEntry));
					cond_stack = ns; cond_stack_cap = nc;
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
					if (nb) { cond_stack[d].branches = nb; cond_stack[d].branches_cap = nc; }
				}
				if (cond_stack[d].branches && cond_stack[d].branches_cap >= need) {
					char *dst = cond_stack[d].branches + cond_stack[d].branches_len;
					dst[0] = '#'; memcpy(dst + 1, p, dir_text_len); dst[1 + dir_text_len] = '\n';
					cond_stack[d].branches_len += blen;
					cond_stack[d].branches[cond_stack[d].branches_len] = '\0';
				}
				if (dir_has_continuation) cond_stack[d].extractable = false;
			}
			goto check_continuation;
		}

		// Only break on #include at unconditional scope
		if (strncmp(p, "include", 7) == 0 && cond_depth == 0) break;

		// Skip defines inside conditions that have multi-line expressions.
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
			// Extract "NAME VALUE" → "NAME=VALUE"
			char *name_start = p;
			while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '(') p++;
			int name_len = (int)(p - name_start);
			if (name_len <= 0) goto check_continuation;
			// Skip function-like macros (not ABI defines)
			if (*p == '(') goto check_continuation;
			// Save the name now — getline() below may realloc 'line'
			char *saved_name = malloc(name_len + 1);
			if (!saved_name) goto check_continuation;
			memcpy(saved_name, name_start, name_len);
			saved_name[name_len] = '\0';

			while (*p == ' ' || *p == '\t') p++;
			char *val_start = p;
			// Trim trailing newline/whitespace/continuation
			char *end = val_start + strlen(val_start);
			while (end > val_start && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
				end--;
			bool has_continuation = (end > val_start && end[-1] == '\\');
			if (has_continuation) end--;
			bool prev_had_ws = (end > val_start && (end[-1] == ' ' || end[-1] == '\t'));
			while (end > val_start && (end[-1] == ' ' || end[-1] == '\t')) end--;
			int val_len = (int)(end - val_start);

			// If the value has continuation lines, buffer them
			// E.g. #define FOO \<newline>    64
			char *full_val = NULL;
			int full_val_len = 0;
			if (has_continuation) {
				// Start with whatever was on the first line
				size_t cap = (val_len > 0 ? val_len : 0) + 256;
				full_val = malloc(cap);
				if (!full_val) { free(saved_name); goto check_continuation; }
				if (val_len > 0) {
					memcpy(full_val, val_start, val_len);
					full_val_len = val_len;
				}
				// Read continuation lines (may realloc 'line')
				while (getline(&line, &line_cap, f) >= 0) {
					char *lp = line;
					bool cur_leading_ws = (*lp == ' ' || *lp == '\t');
					while (*lp == ' ' || *lp == '\t') lp++;
					char *le = lp + strlen(lp);
					while (le > lp && (le[-1] == '\n' || le[-1] == '\r' || le[-1] == ' ' || le[-1] == '\t'))
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
							if (!tmp) { free(full_val); free(saved_name); goto check_continuation; }
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

			// Build "NAME=VALUE" or "NAME" string
			int total = name_len + (val_len > 0 ? 1 + val_len : 0) + 1;
			char *def = malloc(total);
			if (!def) { free(full_val); free(saved_name); goto check_continuation; }
			memcpy(def, saved_name, name_len);
			free(saved_name);
			if (val_len > 0) {
				def[name_len] = '=';
				memcpy(def + name_len + 1, val_start, val_len);
				def[name_len + 1 + val_len] = '\0';
			} else def[name_len] = '\0';
			free(full_val);

			// Build guard text from condition stack (NULL if unconditional)
			char *guard = NULL;
			if (cond_depth > 0) {
				int glen = 0;
				for (int d = 0; d < cond_depth; d++) {
					glen += (int)strlen(cond_stack[d].opening);
					if (cond_stack[d].branches)
						glen += cond_stack[d].branches_len;
				}
				guard = malloc(glen + 1);
				if (guard) {
					int pos = 0;
					for (int d = 0; d < cond_depth; d++) {
						int olen = (int)strlen(cond_stack[d].opening);
						memcpy(guard + pos, cond_stack[d].opening, olen);
						pos += olen;
						if (cond_stack[d].branches) {
							memcpy(guard + pos, cond_stack[d].branches,
							       cond_stack[d].branches_len);
							pos += cond_stack[d].branches_len;
						}
					}
					guard[pos] = '\0';
				}
			}

			int old_cap = ctx->source_define_cap;
			ARENA_ENSURE_CAP(&ctx->main_arena, ctx->source_defines,
					 ctx->source_define_count, ctx->source_define_cap,
					 8, char *);
			if ((int)ctx->source_define_cap != old_cap)
				ctx->source_define_guards = arena_realloc(&ctx->main_arena,
					ctx->source_define_guards,
					sizeof(char *) * old_cap,
					sizeof(char *) * ctx->source_define_cap);
			ctx->source_define_guards[ctx->source_define_count] = guard;
			ctx->source_defines[ctx->source_define_count++] = def;
		}
		// Check if this preprocessor directive has a continuation
	check_continuation: {
			char *end = line + strlen(line);
			while (end > line && (end[-1] == '\n' || end[-1] == '\r')) end--;
			in_continuation = (end > line && end[-1] == '\\');
			// Directive lines may have trailing /* that opens a
			// block comment spanning subsequent lines.
			if (!in_continuation && !in_block_comment && !in_raw_string) {
				char *rd = NULL;
				if (has_unclosed_block_comment(line, &rd))
					in_block_comment = true;
				else if (rd && cond_depth == 0) { in_raw_string = true; raw_delim = rd; raw_delim_len = (int)strlen(rd); }
				else free(rd);
			}
		}
	}
	// Clean up condition stack (in case file ended without matching #endif)
	for (int d = 0; d < cond_depth && d < cond_stack_cap; d++) {
		free(cond_stack[d].opening);
		free(cond_stack[d].branches);
	}
	free(cond_stack);
	free(raw_delim);
	free(line);
	fclose(f);
}

// Run system preprocessor (cc -E) via pipe, returns malloc'd output or NULL
static char *preprocess_with_cc(const char *input_file) {
	collect_source_defines(input_file);
	const char *pp_cc = ctx->extra_compiler ? ctx->extra_compiler : PRISM_DEFAULT_CC;
	int argcap = 16 + cc_extra_arg_count(pp_cc) + ctx->extra_compiler_flags_count + ctx->dep_flags_count +
		     ctx->extra_include_count * 2 + ctx->extra_define_count * 2 + ctx->extra_force_include_count * 2;
	const char **args = alloc_argv(argcap);
	int argc = 0;
	char *cc_dup = NULL;
	build_pp_argv(args, &argc, input_file, &cc_dup);
	char **argv = (char **)args;

	char *buf = NULL;
	char *result = NULL;
	int read_fd = -1;
	pid_t pid = 0;
	char pp_stderr_path[PATH_MAX] = "";
	int pp_stderr_fd = -1;

	// Set up pipe: child writes preprocessed output, we read it
	int pipefd[2];
	if (pipe(pipefd) == -1) {
		perror("pipe");
		free(cc_dup);
		free((void *)args);
		return NULL;
	}
	read_fd = pipefd[0];

	// Capture preprocessor stderr to a temp file for diagnostics on failure
	{
		const char *tmpdir = get_tmp_dir();
		snprintf(pp_stderr_path, sizeof pp_stderr_path, "%sprism_pp_err_XXXXXX", tmpdir);
		pp_stderr_fd = mkstemp(pp_stderr_path);
		if (pp_stderr_fd < 0)
			pp_stderr_path[0] = '\0';
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addclose(&fa, pipefd[0]);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&fa, pipefd[1]);
	if (pp_stderr_fd >= 0)
		posix_spawn_file_actions_adddup2(&fa, pp_stderr_fd, STDERR_FILENO);
	else
		posix_spawn_file_actions_addopen(
		    &fa, STDERR_FILENO, "/dev/null", O_WRONLY | O_TRUNC, 0644);

	char **env = build_clean_environ();
	int err = posix_spawnp(&pid, argv[0], &fa, NULL, argv, env);
	posix_spawn_file_actions_destroy(&fa);
	if (pp_stderr_fd >= 0) close(pp_stderr_fd);
	close(pipefd[1]);

	if (err) {
		fprintf(stderr, "posix_spawnp: %s\n", strerror(err));
		pid = 0; // not spawned — pid is undefined on error
		goto cleanup;
	}

	// Read all preprocessed output from pipe
	{
		size_t cap = 8192, len = 0;
		buf = malloc(cap);
		if (!buf) goto cleanup;

		ssize_t n;
		while ((n = read(read_fd, buf + len, cap - len - 1)) > 0 || (n == -1 && errno == EINTR)) {
			if (n == -1) continue;
			len += (size_t)n;
			if (len + 1 >= cap) {
				cap *= 2;
				char *tmp = realloc(buf, cap);
				if (!tmp) goto cleanup;
				buf = tmp;
			}
		}
		close(read_fd);
		read_fd = -1;
		buf[len] = '\0';

		// Detect null bytes in preprocessor output (would silently truncate tokenization)
		if (strlen(buf) < len) {
			fprintf(stderr, "error: preprocessor output contains null bytes\n");
			goto cleanup;
		}

		// Right-size buffer to exactly len+8 bytes for SWAR comment scanning.
		// The doubling loop guarantees cap >= len+1 but NOT cap >= len+8; when
		// len == cap-1 the old conditional skip caused a 7-byte heap overflow.
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
			// Show captured preprocessor stderr on failure
			if (pp_stderr_path[0]) {
				FILE *ef = fopen(pp_stderr_path, "r");
				if (ef) {
					char line[512];
					while (fgets(line, sizeof line, ef)) fputs(line, stderr);
					fclose(ef);
				}
			}
			goto cleanup;
		}
	}

	result = buf;
	buf = NULL; // ownership transferred

cleanup:
	free(buf);
	if (read_fd >= 0) close(read_fd);
	if (pid > 0) waitpid(pid, NULL, 0);
	if (pp_stderr_path[0]) unlink(pp_stderr_path);
	free(cc_dup);
	free((void *)args);
	return result;
}

// --- Pass 2: Transpilation Engine ---

// Shared helpers for transpile_tokens

static inline void track_ctrl_paren_open(void) {
	ScopeKind k;
	if (ctrl_state.pending_for_paren) {
		k = SCOPE_FOR_PAREN;
		ctrl_state.pending_for_paren = false;
	} else k = SCOPE_CTRL_PAREN;
	ctrl_state.parens_just_closed = false;
	scope_push_kind(k);
	ctx->at_stmt_start = (k == SCOPE_FOR_PAREN);
}

static inline void track_ctrl_paren_close(void) {
	// Pop paren scopes down to and including the matching one
	while (ctx->scope_depth > 0) {
		ScopeKind k = scope_stack[ctx->scope_depth - 1].kind;
		if (k == SCOPE_FOR_PAREN || k == SCOPE_CTRL_PAREN) {
			scope_pop();
			break;
		}
		break;
	}
	ctrl_state.parens_just_closed = true;
	ctx->at_stmt_start = true;
}

static inline void track_ctrl_semicolon(void) {
	if (in_for_init()) {
		// First ';' in for-init: pop FOR_PAREN, push CTRL_PAREN (for condition/update)
		scope_pop();
		// Re-push as CTRL_PAREN: 2nd ';' stays inside paren scope.
		scope_push_kind(SCOPE_CTRL_PAREN);
	} else if (!in_ctrl_paren()) ctrl_reset();
}

static inline void track_generic_token(Token *tok) {
	if (!in_generic()) return;
	if (match_ch(tok, '(')) scope_push_kind(SCOPE_GENERIC);
	else if (match_ch(tok, ')') && ctx->scope_depth > 0 &&
	         scope_stack[ctx->scope_depth - 1].kind == SCOPE_GENERIC)
		scope_pop();
}

static inline void track_common_token_state(Token *tok) {
	if (__builtin_expect(ctrl_state.pending && tok->len == 1, 0)) {
		char c = tok->ch0;
		if (c == '(') track_ctrl_paren_open();
		else if (c == ')') track_ctrl_paren_close();
	}
	track_generic_token(tok);
}

// Scan ahead for 'orelse' at depth 0 in a bare expression
static Token *find_bare_orelse(Token *tok) {
	if (!p1_file_has_orelse) return NULL;
	Token *prev = NULL;
	int ternary = 0;
	for (Token *s = tok; s->kind != TK_EOF; s = tok_next(s)) {
		if (s->flags & TF_OPEN) { prev = tok_match(s); s = tok_match(s); continue; }
		if ((s->flags & TF_CLOSE) || match_ch(s, ';')) return NULL;
		if (match_ch(s, '?')) { ternary++; prev = s; continue; }
		if (match_ch(s, ':') && ternary > 0) { ternary--; prev = s; continue; }
		if (is_orelse_kw(s) &&
		    !(prev && (prev->tag & TT_MEMBER)) && ternary == 0)
			return s;
		prev = s;
	}
	return NULL;
}

// emit_bare_orelse_impl + emit_deferred_range.
// Placed here because they depend on find_bare_orelse, is_known_typedef,
// is_raw_declaration_context, and other helpers defined above.

// Check if another orelse follows before the next `;` (or `,` if comma_term).
static bool orelse_has_chain(Token *start, bool comma_term) {
	int pd = 0;
	for (Token *p = start; p->kind != TK_EOF; p = tok_next(p)) {
		if (p->flags & TF_OPEN) pd++;
		else if (p->flags & TF_CLOSE) pd--;
		else if (pd == 0 && (match_ch(p, ';') || (comma_term && match_ch(p, ',')))) break;
		if (pd == 0 && is_orelse_keyword(p)) return true;
	}
	return false;
}

// Unified bare-expression orelse handler.
// Returns the token after the statement, or NULL if no orelse was found.
// `comma_term`: also treat ',' at depth 0 as statement terminator.
// `end`: optional boundary (for deferred ranges).
static Token *emit_bare_orelse_impl(Token *t, Token *end, bool comma_term, bool brace_wrap) {
	Token *orelse_tok = find_bare_orelse(t);
	if (!orelse_tok || (end && tok_loc(orelse_tok) >= tok_loc(end))) return NULL;

	if (is_orelse_keyword(t))
		error_tok(t, "expected expression before 'orelse'");

	#define BARE_IS_END(s) (match_ch((s), ';') || (comma_term && match_ch((s), ',')))

	// Find last depth-0 comma before orelse (search only — don't emit yet).
	// Need to check is_bare_fallback before emitting, because if it's not
	// bare we return NULL and the caller handles it differently.
	Token *last_comma = NULL;
	{
		int sd = 0;
		for (Token *s = t; s != orelse_tok; s = tok_next(s)) {
			if (s->flags & TF_OPEN) sd++;
			else if (s->flags & TF_CLOSE) sd--;
			else if (sd == 0 && comma_term && match_ch(s, ',')) last_comma = s;
		}
	}
	Token *post_comma_t = last_comma ? tok_next(last_comma) : t;

	// Find assignment target (last = at depth 0 before orelse).
	// Must find the LAST = so that chained assignments like
	// a = b = f() orelse 5 are caught by reject_orelse_side_effects
	// (the first = lands in the LHS range and triggers the error).
	Token *bare_lhs_start = post_comma_t;
	Token *bare_assign_eq = NULL;
	{
		int sd = 0;
		for (Token *s = post_comma_t; s != orelse_tok; s = tok_next(s)) {
			if (s->flags & TF_OPEN) sd++;
			else if (s->flags & TF_CLOSE) sd--;
			else if (sd == 0 && is_assignment_operator_token(s)) {
				if (!match_ch(s, '='))
					error_tok(s, "bare assignment with 'orelse' cannot use compound operators "
						  "(e.g. +=, -=); use a plain '=' assignment");
				bare_assign_eq = s;
			}
		}
	}

	// Error if LHS has side effects (double evaluation)
	// Only applies to bare-fallback path (ternary pattern evaluates LHS
	// twice); control-flow/block actions use if-guard (single eval of LHS).

	Token *after_orelse = tok_next(orelse_tok);
	bool is_bare_fallback = bare_assign_eq && is_orelse_value_fallback(after_orelse);

	if (bare_assign_eq && is_bare_fallback) {
		reject_orelse_side_effects(bare_lhs_start, bare_assign_eq,
					  "orelse fallback on assignment",
					  "in the target expression",
					  true, false, true);
	}

	if (!is_bare_fallback) return NULL;  // caller handles non-bare fallback

	// Now we know this is bare — safe to emit.
	// Wrap in braces for braceless control-flow bodies (if/for/while/else
	// without braces) so the expansion stays as a single compound statement.
	if (brace_wrap) OUT_LIT(" {");

	// Emit comma prefix as a separate statement (comma → semicolon).
	// The orelse expansion may produce a block which cannot appear after
	// the comma operator.
	if (last_comma) {
		for (Token *s = t; s != last_comma; s = tok_next(s))
			emit_tok(s);
		out_char(';');
		t = post_comma_t;
	}

	// Reject if the statement contains preprocessor conditionals (#ifdef/#else/etc.).
	// emit_range_no_prep / emit_balanced_range skip TK_PREP_DIR tokens,
	// producing concatenated code from ALL branches — silent miscompilation.
	// We cannot statically evaluate which branch is active, so error here.
	// Scan the entire statement: LHS, test expression, and fallback(s).
	if (bare_assign_eq) {
		int sd = 0;
		for (Token *s = bare_lhs_start; s && s->kind != TK_EOF; s = tok_next(s)) {
			if (s->flags & TF_OPEN) sd++;
			else if (s->flags & TF_CLOSE) sd--;
			else if (sd == 0 && BARE_IS_END(s)) break;
			if (is_pp_conditional(s))
				error_tok(orelse_tok,
					  "bare orelse assignment cannot be used when the "
					  "expression spans preprocessor conditionals — the "
					  "transpiler would emit tokens from all branches, "
					  "producing invalid C; use a temporary variable or "
					  "move the #ifdef outside the expression");
		}
	}

	// Hoist preprocessor directives before the wrapper
	for (Token *s = t; s != orelse_tok; s = tok_next(s)) {
		if (s->kind == TK_PREP_DIR) { emit_tok(s); out_char('\n'); ctx->last_line_no++; }
	}
		bool fallback_has_compound_literal = false;
		{
			int sd = 0;
			for (Token *s = after_orelse; s && s->kind != TK_EOF; s = tok_next(s)) {
				if (sd == 0 && match_ch(s, '{')) { fallback_has_compound_literal = true; break; }
				if (s->flags & TF_OPEN) sd++;
				else if (s->flags & TF_CLOSE) sd--;
				else if (sd == 0 && BARE_IS_END(s)) break;
			}
		}

		out_char(' ');
		// Check if LHS contains indirection (*, [, . or ->).
		// typeof(LHS) evaluates its operand when the result type is
		// variably-modified (C11 §6.7.2.4p2).  Operators in LHS can
		// produce VM types (e.g. ptr-to-VLA subscript yields VLA),
		// causing spurious volatile reads.  typeof(bitfield) is also
		// a constraint violation (C23 §6.7.2.5p2).
		// When LHS has indirection, use typeof(RHS) instead —
		// function return types are never VM (C11 §6.7.6.3p1).
		bool lhs_has_indirection = false;
		if (bare_assign_eq) {
			for (Token *s = bare_lhs_start; s != bare_assign_eq; s = tok_next(s))
				if ((s->tag & TT_MEMBER) || match_ch(s, '*') ||
				    (match_ch(s, '[') && (s->flags & TF_OPEN))) {
					lhs_has_indirection = true; break;
				}
		}
		if (fallback_has_compound_literal) {
			// The compound literal ternary path evaluates LHS twice:
			// (LHS = RHS) ? (void)0 : (void)(LHS = (fb))
			// Reject volatile dereference in LHS — double bus write.
			if (bare_assign_eq)
				reject_orelse_side_effects(bare_lhs_start, bare_assign_eq,
					"orelse compound-literal fallback on assignment",
					"in the target expression (volatile double-write "
					"with compound literal fallback); "
					"use a temporary variable instead",
					false, true, false);
			// Ternary: (LHS = RHS) ? (void)0 : (void)(LHS = (fb));
			OUT_LIT("(");
			emit_balanced_range(bare_lhs_start, orelse_tok);
			OUT_LIT(") ? (void)0 : ");
			t = after_orelse;
			int fd = 0;
			bool has_chain = orelse_has_chain(t, comma_term);
			if (has_chain) OUT_LIT("("); else OUT_LIT("(void)(");
			emit_range_no_prep(bare_lhs_start, bare_assign_eq);
			OUT_LIT(" = (");
			while (t->kind != TK_EOF) {
			if ((t->flags & TF_OPEN) && (match_ch(t, '(') || match_ch(t, '['))) { t = walk_balanced(t, true); continue; }
				if (t->flags & TF_OPEN) fd++;
				else if (t->flags & TF_CLOSE) fd--;
				else if (fd == 0 && BARE_IS_END(t)) break;
				if (fd == 0 && is_orelse_keyword(t)) {
					OUT_LIT(")) ? (void)0 : ");
					t = tok_next(t);
					has_chain = orelse_has_chain(t, comma_term);
					if (has_chain) OUT_LIT("("); else OUT_LIT("(void)(");
					emit_range_no_prep(bare_lhs_start, bare_assign_eq);
					OUT_LIT(" = (");
					continue;
				}
				emit_tok(t); t = tok_next(t);
			}
			OUT_LIT("));");
		} else {
			// Temp-based (volatile-safe, single-write):
			// Evaluates RHS into a typeof(...) temp, writes LHS once.
			//
			// typeof(EXPR) evaluates its operand when the result type
			// is variably-modified (C11 §6.7.2.4p2).  When LHS is a
			// simple variable, typeof(LHS) is always safe (no side
			// effects).  When LHS has indirection (*, [], ., ->), the
			// result type may be VM (e.g. ptr-to-VLA subscript yields
			// VLA), so we use typeof(RHS) instead.  Function return
			// types are never VM (C11 §6.7.6.3p1), so typeof(RHS)
			// never evaluates for the common f() orelse pattern.
			// typeof(bitfield) is also a constraint violation
			// (C23 §6.7.2.5p2), covered by the . / -> check.
			//
			// Single:  { typeof(LHS) t0=(a); LHS = t0 ? t0 : (fb); }
			// Chained: { typeof(LHS) t0=(a); if(t0){LHS=t0;}else{
			//            typeof(LHS) t1=(b); LHS = t1 ? t1 : (c); } }
			unsigned oe_id = ctx->ret_counter++;
			OUT_LIT("{ "); emit_typeof_keyword(); out_char('(');
			if (lhs_has_indirection)
				emit_balanced_range(tok_next(bare_assign_eq), orelse_tok);
			else
				emit_range_no_prep(bare_lhs_start, bare_assign_eq);
			OUT_LIT(") __prism_oe_");
			out_uint(oe_id);
			OUT_LIT(" = (");
			emit_balanced_range(tok_next(bare_assign_eq), orelse_tok);
			OUT_LIT(");");
			t = after_orelse;
			{
				// Chained: nested if/else with per-link LHS-typed
				// temps. Single link hits is_last immediately
				// (nest stays 0, no-op).
				int nest = 0;
				while (true) {
					bool is_last = !orelse_has_chain(t, comma_term);
					if (is_last) {
						// Last link: ternary preserves CL lifetime
						// and usual arithmetic conversions.
						OUT_LIT(" ");
						emit_range_no_prep(bare_lhs_start, bare_assign_eq);
						OUT_LIT(" = __prism_oe_");
						out_uint(oe_id);
						OUT_LIT(" ? __prism_oe_");
						out_uint(oe_id);
						OUT_LIT(" : (");
						int fd = 0;
						while (t->kind != TK_EOF) {
							if ((t->flags & TF_OPEN) && (match_ch(t, '(') || match_ch(t, '['))) { t = walk_balanced(t, true); continue; }
							if (t->flags & TF_OPEN) fd++;
							else if (t->flags & TF_CLOSE) fd--;
							else if (fd == 0 && BARE_IS_END(t)) break;
							emit_tok(t); t = tok_next(t);
						}
						OUT_LIT(");");
						break;
					}
					// Intermediate link: if/else with new typed temp.
					OUT_LIT(" if (__prism_oe_");
					out_uint(oe_id);
					OUT_LIT(") { ");
					emit_range_no_prep(bare_lhs_start, bare_assign_eq);
					OUT_LIT(" = __prism_oe_");
					out_uint(oe_id);
					OUT_LIT("; } else { ");
					nest++;
					Token *fb_start = t;
					Token *fb_orelse = NULL;
					{
						int fd = 0;
						for (Token *s = t; s->kind != TK_EOF; s = tok_next(s)) {
							if ((s->flags & TF_OPEN) && (match_ch(s, '(') || match_ch(s, '['))) { s = tok_match(s); continue; }
							if (s->flags & TF_OPEN) fd++;
							else if (s->flags & TF_CLOSE) fd--;
							else if (fd == 0 && BARE_IS_END(s)) break;
							if (fd == 0 && is_orelse_keyword(s)) { fb_orelse = s; break; }
						}
					}
					oe_id = ctx->ret_counter++;
					emit_typeof_keyword(); out_char('(');
					if (lhs_has_indirection)
						emit_balanced_range(fb_start, fb_orelse);
					else
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
		if (match_ch(t, ';') || (comma_term && match_ch(t, ','))) t = tok_next(t);
	if (brace_wrap) OUT_LIT(" }");
	#undef BARE_IS_END
	return t;
}

// Emit `{ if (!(expr))` for non-bare orelse. Hoists prep dirs, returns token after orelse keyword.
static Token *emit_orelse_condition_wrap(Token *t, Token *orelse_tok) {
	for (Token *s = t; s != orelse_tok; s = tok_next(s))
		if (s->kind == TK_PREP_DIR) { emit_tok(s); out_char('\n'); ctx->last_line_no++; }
	OUT_LIT(" {");
	OUT_LIT(" if (!(");
	// Use emit_range_no_prep to route ({...}) through walk_balanced
	// for defer/orelse/zeroinit processing inside stmt-exprs.
	emit_range_no_prep(t, orelse_tok);
	OUT_LIT("))");
	return tok_next(orelse_tok);
}

// Wrapper for defer blocks: handles both bare and non-bare orelse.
// Returns the token after the statement, or NULL if no orelse was found.
static Token *emit_deferred_orelse(Token *t, Token *end) {
	Token *result = emit_bare_orelse_impl(t, end, false, false);
	if (result) return result;

	// Check for non-bare orelse (block/control-flow action)
	Token *orelse_tok = find_bare_orelse(t);
	if (!orelse_tok || (end && tok_loc(orelse_tok) >= tok_loc(end))) return NULL;

	t = emit_orelse_condition_wrap(t, orelse_tok);

	if (match_ch(t, ';'))
		error_tok(t, "expected statement after 'orelse'");

	if (t->tag & (TT_BREAK | TT_CONTINUE | TT_GOTO | TT_RETURN)) {
		// Control-flow action: emit keyword to semicolon.
		while (t && t->kind != TK_EOF && !match_ch(t, ';')) {
			emit_tok(t); t = tok_next(t);
		}
		if (match_ch(t, ';')) { emit_tok(t); t = tok_next(t); }
	} else if (match_ch(t, '{')) {
		Token *bclose = tok_match(t);
		emit_tok(t); t = tok_next(t);
		// Process block body through emit_deferred_range for zeroinit/raw/orelse
		emit_deferred_range(t, bclose);
		t = bclose;
		if (t) { emit_tok(t); t = tok_next(t); }
	} else error_tok(orelse_tok, "orelse fallback requires an assignment target (use a declaration)");
	OUT_LIT(" }");
	if (match_ch(t, ';')) t = tok_next(t);
	return t;
}

// Shared token dispatch for walk_balanced stmt-expr and emit_deferred_range.
// Returns next token if consumed, NULL if caller should emit_tok and advance.
static inline Token *try_process_stmt_token(Token *t, Token *end, Token **unreachable_tok) {
	if (ctx->at_stmt_start && FEAT(F_ZEROINIT)) {
		Token *next = try_zero_init_decl(t);
		if (next) return next;
	}
	if (ctx->at_stmt_start && FEAT(F_ORELSE) && !(t->tag & TT_NON_EXPR_STMT)) {
		Token *next = emit_deferred_orelse(t, end);
		if (next) { ctx->at_stmt_start = true; return next; }
	}
	{ Token *r = try_strip_raw(t); if (r) return r; }
	if (FEAT(F_AUTO_UNREACHABLE)) {
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

	// Track __builtin_unreachable() injection: suppress when inside a
	// braceless control-flow body, since emit_deferred_range doesn't
	// inject scope braces around braceless bodies.  Without this,
	// `if(0) abort();` would emit `__builtin_unreachable()` after `;`
	// but OUTSIDE the if-body, causing it to execute unconditionally.
	Token *dr_unreachable_tok = NULL;
	bool dr_braceless_body = false;

	for (Token *t = start; t && t != end && t->kind != TK_EOF;) {
		Token *next = try_process_stmt_token(t, end, &dr_unreachable_tok);
		if (next) { t = next; continue; }

		// Control-flow keywords: emit keyword (+ condition), mark
		// next token as statement start so zeroinit/orelse fire
		// in braceless bodies.
		if (ctx->at_stmt_start) {
			// else / do: no condition parens, body is next
			if (((t->tag & TT_IF) && t->ch0 == 'e') ||
			    ((t->tag & TT_LOOP) && t->ch0 == 'd')) {
				emit_tok(t); t = tok_next(t);
				dr_braceless_body = true;
				ctrl_state.pending = true;
				ctrl_state.parens_just_closed = true;
				continue; // at_stmt_start stays true
			}
			// if / while / for / switch: emit keyword + condition (...)
			if ((t->tag & (TT_IF | TT_LOOP | TT_SWITCH)) &&
			    !is_known_typedef(t)) {
				emit_tok(t); t = tok_next(t);
				if (t && match_ch(t, '(') && tok_match(t)) {
					t = emit_ctrl_condition(t, &dr_unreachable_tok);
					ctx->at_stmt_start = true;
					dr_braceless_body = true;
					ctrl_state.pending = true;
					ctrl_state.parens_just_closed = true;
					continue; // at_stmt_start stays true
				}
			}
		}

		ctx->at_stmt_start = false;

		if (t->tag & TT_STRUCTURAL) {
			bool is_semi = (t->len == 1 && t->ch0 == ';');
			bool is_ur_target = is_semi && !dr_braceless_body &&
					    (t == dr_unreachable_tok);
			emit_tok(t); t = tok_next(t);
			if (is_ur_target) {
				EMIT_UNREACHABLE();
				dr_unreachable_tok = NULL;
			}
			// { or } clears braceless flag (braced body is scoped);
			// ; ends the braceless body statement.
			dr_braceless_body = false;
			if (ctrl_state.pending) ctrl_reset();
			ctx->at_stmt_start = true;
			continue;
		}

		emit_tok(t); t = tok_next(t);
	}

	ctx->at_stmt_start = saved_stmt_start;
	ctrl_state = saved_ctrl;
}

// --- Phase 1: Static Analysis ---

// Phase 1A: walk all tokens, assign scope_ids, build scope_tree[] with parent links + flags.

static void p1_build_scope_tree(Token *start) {
	// scope_id 0 is reserved for file scope (never stored in scope_tree[])
	scope_tree_count = 1; // start at 1; 0 = file scope sentinel
	scope_tree_cap = 0;
	ctx->p1_scope_tree = NULL;

	// Stack for tracking current scope_id at each brace depth
	// Arena-allocated to avoid leaks on longjmp error recovery (PRISM_LIB_MODE)
	int p1a_stack_cap = 256;
	uint16_t *scope_stack_local = arena_alloc_uninit(&ctx->main_arena, p1a_stack_cap * sizeof(uint16_t));
	scope_stack_local[0] = 0; // file scope
	int depth = 0;

	for (Token *t = start; t && t->kind != TK_EOF; t = tok_next(t)) {
		if (t->tag & TT_ORELSE) p1_file_has_orelse = true;
		if (match_ch(t, '{')) {
			// Allocate a new scope entry
			uint16_t sid = scope_tree_count;
			if (sid == UINT16_MAX)
				error_tok(t, "scope tree: too many scopes (>65534)");
			ARENA_ENSURE_CAP(&ctx->main_arena, ctx->p1_scope_tree,
					 scope_tree_count, scope_tree_cap, 256, ScopeInfo);
			ScopeInfo *si = &scope_tree[sid];
			*si = (ScopeInfo){.parent_id = scope_stack_local[depth]};

			// Store token range for this scope
			si->open_tok_idx = tok_idx(t);
			si->close_tok_idx = tok_match(t) ? tok_idx(tok_match(t)) : UINT32_MAX;

			// Classify the scope by examining what precedes the '{'
			Token *prev = NULL;
			uint32_t tidx = tok_idx(t);
			for (uint32_t pi = tidx - 1; pi > 0; pi--) {
				Token *pt = &token_pool[pi];
				if (pt->kind == TK_PREP_DIR) continue;
				if (match_ch(pt, ']') && tok_match(pt) && (tok_match(pt)->flags & TF_C23_ATTR)) {
					pi = tok_idx(tok_match(pt));
					continue;
				}
				prev = pt;
				break;
			}

			if (prev) {
				if ((prev->tag & TT_LOOP) && prev->ch0 == 'd') {
					// 'do' keyword (TT_LOOP covers for/while/do; 'd' distinguishes do)
					si->is_loop = true;
				} else if (match_ch(prev, ')') && tok_match(prev)) {
					Token *open_paren = tok_match(prev);
					Token *kw = NULL;
					uint32_t opi = tok_idx(open_paren);
					for (uint32_t ki = opi - 1; ki > 0; ki--) {
						Token *kt = &token_pool[ki];
						if (kt->kind == TK_PREP_DIR) continue;
						if (match_ch(kt, ']') && tok_match(kt) && (tok_match(kt)->flags & TF_C23_ATTR)) {
							ki = tok_idx(tok_match(kt));
							continue;
						}
						kw = kt;
						break;
					}
					// If we landed on an attribute keyword, walk further back
					// past the attribute to find the real predecessor. This
					// handles: struct __attribute__((packed)) {
					if (kw && (kw->tag & TT_ATTR)) {
						for (uint32_t ki = tok_idx(kw) - 1; ki > 0; ki--) {
							Token *kt = &token_pool[ki];
							if (kt->kind == TK_PREP_DIR) continue;
							if (match_ch(kt, ')') && tok_match(kt)) { ki = tok_idx(tok_match(kt)); continue; }
							if (kt->tag & TT_ATTR) continue;
							kw = kt;
							break;
						}
					}
					if (kw) {
						if (kw->tag & TT_LOOP)
							si->is_loop = true;
						else if (kw->tag & TT_SWITCH)
							si->is_switch = true;
						else if (kw->tag & TT_IF)
							si->is_conditional = true;
						else if (kw->tag & TT_SUE) {
							si->is_struct = true;
							if (is_enum_kw(kw)) si->is_enum = true;
						}
					}
					// Function body: file-scope '{' preceded by ')' with no control keyword
					if (depth == 0 && !si->is_loop && !si->is_switch && !si->is_conditional
					    && !si->is_struct)
						si->is_func_body = true;
					// Compound literal: inner-scope ')' + '{' with no
					// control keyword (e.g. (struct S){.a = 1})
					if (depth > 0 && !si->is_loop && !si->is_switch && !si->is_conditional
					    && !si->is_struct && !si->is_func_body)
						si->is_init = true;
				} else if ((prev->tag & TT_IF) && prev->ch0 == 'e') {
					// 'else' keyword (TT_IF covers both if and else; 'e' distinguishes)
					si->is_conditional = true;
				} else if (prev->tag & TT_SUE) {
					si->is_struct = true;
					if (is_enum_kw(prev)) si->is_enum = true;
				} else if (prev->kind == TK_IDENT && !(prev->tag & (TT_TYPE|TT_QUALIFIER|TT_LOOP|TT_SWITCH|TT_IF|TT_STORAGE))) {
					// Named struct/union/enum: 'struct Name {' or
					// 'struct __attribute__((packed)) Name {'
					for (uint32_t si2 = tok_idx(prev) - 1; si2 > 0; si2--) {
						Token *st = &token_pool[si2];
						if (st->kind == TK_PREP_DIR) continue;
						if (match_ch(st, ')') && tok_match(st)) { si2 = tok_idx(tok_match(st)); continue; }
						if (st->tag & TT_ATTR) continue;
						if (st->tag & TT_SUE) {
							si->is_struct = true;
							if (is_enum_kw(st)) si->is_enum = true;
						}
						break;
					}
				} else if (depth == 0 && (match_ch(prev, ']') || match_ch(prev, ';'))) {
					// Array-returning function: int (*fn(void))[5] {
					// K&R function definition: fn(a) int a; {
					si->is_func_body = true;
				}
			}

			if (prev && match_ch(prev, '('))
				si->is_stmt_expr = true;

			// Initializer brace: a '{' that is not a compound-statement body.
			// Detected by immediately preceding '=' (direct init), or by being
			// nested inside an already-classified initializer scope (nested init).
			if (!si->is_func_body && !si->is_loop && !si->is_switch &&
			    !si->is_conditional && !si->is_struct && !si->is_stmt_expr) {
				if (prev && match_ch(prev, '=')) {
					si->is_init = true;
				} else if (depth > 0 && scope_stack_local[depth] < scope_tree_count &&
				           scope_tree[scope_stack_local[depth]].is_init) {
					si->is_init = true;
				}
			}

			// Write scope classification to pass1_ann
			uint8_t ann = 0;
			if (si->is_loop) ann |= P1_SCOPE_LOOP;
			if (si->is_switch) ann |= P1_SCOPE_SWITCH;
			if (si->is_init) ann |= P1_SCOPE_INIT;
			token_pool[tidx].ann = ann;

			scope_tree_count++;
			depth++;
			if (depth >= p1a_stack_cap) {
				int old_cap = p1a_stack_cap;
				p1a_stack_cap *= 2;
				scope_stack_local = arena_realloc(&ctx->main_arena, scope_stack_local,
					old_cap * sizeof(uint16_t), p1a_stack_cap * sizeof(uint16_t));
			}
			scope_stack_local[depth] = sid;
			continue;
		}

		if (match_ch(t, '}')) {
			if (depth > 0) depth--;
			continue;
		}
	}
}
// Phase 1B: walk all tokens at all depths to build the complete typedef + enum table.

// Walk a parameter list (open..close) registering typedef/keyword shadows.
// If check_vla, also register VLA parameter names for sizeof detection.
static void p1_register_param_shadows(Token *open, Token *close,
				      uint16_t scope_id, int brace_depth,
				      bool check_vla) {
	for (Token *t = tok_next(open); t && t != close && t->kind != TK_EOF; ) {
		Token *param_start = t;
		Token *last_ident = NULL;
		bool scanned_inner_paren = false;
		bool ident_from_inner = false;
		while (t && t != close && !match_ch(t, ',') && t->kind != TK_EOF) {
			if (t->flags & TF_OPEN) {
				if (!last_ident && !scanned_inner_paren && match_ch(t, '(') && tok_match(t))
					for (Token *s = tok_next(t); s != tok_match(t); s = tok_next(s)) {
						if (s->flags & TF_OPEN) { s = tok_match(s) ? tok_match(s) : s; continue; }
						if (is_valid_varname(s) && !(s->tag & (TT_QUALIFIER|TT_TYPE|TT_SUE|TT_TYPEOF|TT_ATTR)))
							{ last_ident = s; ident_from_inner = true; }
					}
				if (match_ch(t, '(')) scanned_inner_paren = true;
				t = tok_match(t) ? tok_next(tok_match(t)) : tok_next(t);
				continue;
			}
			if (is_valid_varname(t) && !(t->tag & (TT_QUALIFIER|TT_TYPE|TT_SUE|TT_TYPEOF|TT_ATTR)))
				{ last_ident = t; ident_from_inner = false; }
			t = tok_next(t);
		}
		if (last_ident && (is_known_typedef(last_ident) ||
		    is_known_enum_const(last_ident) ||
		    (last_ident->tag & (TT_DEFER | TT_ORELSE)) ||
		    hashmap_get(&p1_func_proto_map, tok_loc(last_ident), last_ident->len)))
			p1_register_shadow(last_ident, scope_id, brace_depth);
		if (check_vla && last_ident) {
			// For outer-level identifiers (int arr[n]), the first []
			// is the decaying dimension (sizeof(arr) = pointer size,
			// constant). Skip it. Only subsequent [] dimensions matter
			// (e.g. int arr[n][m] → int (*)[m], sizeof(*arr) = runtime).
			// For inner-paren identifiers (int (*ptr)[n]), no decay —
			// all brackets matter.
			bool skip_first = !ident_from_inner;
			Token *param_end = (t && match_ch(t, ',')) ? t : close;
			for (Token *s = param_start; s && s != param_end && s->kind != TK_EOF; s = tok_next(s))
				if (match_ch(s, '[')) {
					if (skip_first) {
						skip_first = false;
						if (tok_match(s)) s = tok_match(s);
						continue;
					}
					if (array_size_is_vla(s)) {
						TYPEDEF_ADD_IDX(typedef_add_vla_var(tok_loc(last_ident), last_ident->len, brace_depth), last_ident);
						typedef_table.entries[typedef_table.count - 1].is_param = true;
						break;
					}
				}
		}
		if (t && match_ch(t, ',')) t = tok_next(t);
	}
}

// Phase 1D helper: check if array dimensions between start and end contain orelse.
static bool p1d_decl_has_bracket_orelse(Token *start, Token *end) {
	for (Token *t = start; t && t != end; t = tok_next(t)) {
		if (match_ch(t, '[') && tok_match(t)) {
			Token *close = tok_match(t);
			for (Token *s = tok_next(t); s && s != close; s = tok_next(s))
				if ((s->tag & TT_ORELSE) && !typedef_lookup(s)) return true;
		}
	}
	return false;
}

// Scan for-init / if-switch-init declarations and register shadows.
// body_sid: scope_id of the body following the control statement (for CFG
// verifier P1K_DECL entries). 0 means don't register P1K_DECL (braceless body
// or not inside a function).
static void p1_scan_init_shadows(Token *open, Token *init_end,
    uint32_t scope_close_idx, uint16_t cur_sid, int brace_depth,
    uint16_t body_sid, uint32_t body_close_idx)
{
	Token *init_tok = tok_next(open);
	bool saw_raw = false;
	while (init_tok && (init_tok->flags & TF_RAW) && !is_known_typedef(init_tok)) {
		saw_raw = true;
		init_tok = tok_next(init_tok);
	}
	if (!init_tok || !(init_tok->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_BITINT) ||
	    is_known_typedef(init_tok)))
		return;
	bool saw_static = init_tok->tag & TT_STORAGE;
	uint32_t saved_open = td_scope_open;
	uint32_t saved_close = td_scope_close;
	td_scope_open = tok_idx(open);
	td_scope_close = scope_close_idx;
	TypeSpecResult type = parse_type_specifier(init_tok);
	if (type.saw_type) {
		tok_ann(init_tok) |= P1_IS_DECL;
		Token *t = type.end;
		while (t && t != init_end && t->kind != TK_EOF) {
			// Per-declarator 'raw' skip (for(int x, raw y; ...))
			bool decl_raw = saw_raw;
			t = p1_skip_decl_raw(t, &decl_raw);
			DeclResult decl = parse_declarator(t, false);
			if (!decl.var_name || !decl.end) break;

			// Phase 1D: reject bracket orelse in ctrl-paren declarations
			// (hoisted temps would inject invalid syntax in for/if/switch
			// conditions; moved from Pass 2 to satisfy the two-pass invariant)
			if (FEAT(F_ORELSE) && p1d_decl_has_bracket_orelse(t, decl.end))
				error_tok(t,
					  "bracket orelse in VLA dimensions cannot be used in "
					  "control statement conditions (hoisted temps would "
					  "inject invalid syntax); move the declaration before "
					  "the statement");

			if (is_known_typedef(decl.var_name) ||
			    is_known_enum_const(decl.var_name) ||
			    (decl.var_name->tag & (TT_DEFER | TT_ORELSE)) ||
			    hashmap_get(&p1_func_proto_map, tok_loc(decl.var_name), decl.var_name->len))
				p1_register_shadow(decl.var_name, cur_sid, brace_depth);

			// Phase 1D: register CFG entry for goto-skip-decl detection
			{
				bool has_init = match_ch(decl.end, '=');
				uint16_t eff_sid = body_sid > 0 ? body_sid : cur_sid;
				if (eff_sid > 0) {
					P1FuncEntry *e = p1_alloc(P1K_DECL, eff_sid, decl.var_name);
					e->decl.has_init = has_init;
					e->decl.is_vla = type.is_vla || decl.is_vla;
					e->decl.has_raw = decl_raw;
					e->decl.is_static_storage = saw_static || type.has_static || type.has_extern;
					e->decl.body_close_idx = body_sid > 0 ? 0 : body_close_idx;
				}
				// Phase 1D: reject init-decl needing memset
				// (moved from Pass 2 process_declarators to satisfy
				// the two-pass invariant: all semantic errors before emission)
				if (FEAT(F_ZEROINIT) && !has_init && !decl_raw &&
				    !(saw_static || type.has_static || type.has_extern)) {
					bool eff_vla = (decl.is_vla && (!decl.paren_pointer || decl.paren_array)) ||
						       (type.is_vla && !decl.is_pointer);
					bool is_agg = (decl.is_array && (!decl.paren_pointer || decl.paren_array)) ||
						      ((type.is_struct || type.is_typedef) && !decl.is_pointer);
					if ((!decl.is_pointer || decl.is_array) && !type.has_register &&
					    (type.has_typeof || (type.has_atomic && is_agg) || eff_vla))
						error_tok(decl.var_name,
							  "VLA or typeof variable in for/if/switch initializer "
							  "cannot be safely zero-initialized; move the "
							  "declaration before the statement");
				}
			}

			t = decl.end;
			if (match_ch(t, '=')) {
				t = tok_next(t);
				while (t && t != init_end && t->kind != TK_EOF) {
					if (t->flags & TF_OPEN) { t = tok_match(t) ? tok_next(tok_match(t)) : tok_next(t); continue; }
					if (match_ch(t, ',')) break;
					t = tok_next(t);
				}
			}
			if (t && match_ch(t, ',')) t = tok_next(t); else break;
		}
	}
	td_scope_open = saved_open;
	td_scope_close = saved_close;
}

// Phase 1D: check if a declaration shadows an identifier captured by a
// same-scope defer body.  Moved from Pass 2 check_defer_var_shadow to
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
		if (match_ch(body, '{') && tok_match(body))
			body_end = tok_match(body);
		else
			body_end = skip_to_semicolon(body);
		// Skip if var_name is declared inside this defer body
		uint32_t var_idx = tok_idx(var_name);
		uint32_t bi = tok_idx(body);
		uint32_t ei = body_end ? tok_idx(body_end) : UINT32_MAX;
		if (var_idx >= bi && var_idx < ei) continue;
		if (defer_body_refs_name(body, body_end, name, nlen))
			error_tok(var_name,
				  "variable '%.*s' shadows a name captured "
				  "by a defer in the same scope; the defer "
				  "body would bind to the shadowing variable",
				  nlen, name);
	}
}

// Check defer compatibility and allocate P1K_DEFER entry.
static void p1_try_alloc_defer(Token *tok, uint16_t cur_sid, int func_idx) {
	uint16_t btag = func_meta[func_idx].body_open->tag;
	if (btag & TT_SPECIAL_FN)
		error_tok(tok, "defer cannot be used in functions that call setjmp/longjmp/pthread_exit");
	if (btag & TT_NORETURN_FN)
		error_tok(tok, "defer cannot be used in functions that call vfork()");
	if (btag & TT_ASM)
		error_tok(tok, "defer cannot be used in functions containing asm goto");
	p1_alloc(P1K_DEFER, cur_sid, tok);
}

// Phase 1F: validate defer body and populate name set.
// Extracted from p1_full_depth_prescan to reduce main loop icache footprint.
static void __attribute__((noinline))
p1d_validate_defer(Token *tok, int p1d_cur_func, bool p1d_ctrl_pending, uint16_t cur_sid) {
	// Context validation (moved from Pass 2 handle_defer_keyword)
	if (p1d_cur_func >= 0) {
		if (p1d_ctrl_pending)
			error_tok(tok, "defer requires braces in control statements (braceless has no scope)");
		if (cur_sid < scope_tree_count && scope_tree[cur_sid].is_stmt_expr)
			error_tok(tok, "defer cannot be at top level of statement expression; wrap in a block");
		if (cur_sid < scope_tree_count && scope_tree[cur_sid].is_switch)
			error_tok(tok, "defer in switch case requires braces");
		p1_check_defer_stmt_expr_chain(tok, cur_sid);
	}
	// Unterminated / keyword heuristic (non-block defer)
	{
		Token *body = tok_next(tok);
		if (body && !match_ch(body, '{')) {
			Token *semi = skip_to_semicolon(body);
			if (semi->kind == TK_EOF || !match_ch(semi, ';'))
				error_tok(tok, "unterminated defer statement; expected ';'");
			if (body->kind == TK_KEYWORD &&
			    (body->tag & (TT_NON_EXPR_STMT | TT_DEFER)))
				error_tok(tok, "defer statement appears to be missing ';' (found '%.*s' keyword inside)",
					  body->len, tok_loc(body));
		}
	}
	validate_defer_statement(tok_next(tok), false, false, 0);

	// Populate defer name set for O(1) shadow checks.
	if (p1d_cur_func >= 0) {
		Token *body = tok_next(tok);
		Token *body_end = NULL;
		if (body && match_ch(body, '{') && tok_match(body))
			body_end = tok_match(body);
		else if (body)
			body_end = skip_to_semicolon(body);
		if (body_end) {
			Token *prev_t = NULL;
			for (Token *t = body; t && t != body_end && t->kind != TK_EOF;
			     prev_t = t, t = tok_next(t)) {
				if ((t->kind == TK_IDENT || t->kind == TK_KEYWORD) &&
				    !(prev_t && (prev_t->tag & TT_MEMBER)))
					hashmap_put(&func_meta[p1d_cur_func].defer_name_set,
						tok_loc(t), t->len, (void*)1);
			}
		}
	}
}

// Phase 1G: classify bracket orelse inside [...] array dimensions.
// Extracted from p1_full_depth_prescan to reduce main loop icache footprint.
static void __attribute__((noinline))
p1d_classify_bracket_orelse(Token *tok, uint16_t cur_sid, int p1d_cur_func) {
	Token *close = tok_match(tok);
	bool in_struct = cur_sid > 0 && cur_sid < scope_tree_count && scope_tree[cur_sid].is_struct;
	bool found_oe = false;
	Token *prev_d0_oe = NULL;
	int oe_depth = 0;
	for (Token *s = tok_next(tok); s && s != close && s->kind != TK_EOF; s = tok_next(s)) {
		if (s->flags & TF_OPEN) oe_depth++;
		if (s->flags & TF_CLOSE) oe_depth--;
		if (is_orelse_kw_shadow(s)) {
			if (p1d_cur_func < 0)
				error_tok(s, "orelse inside array dimension at file scope is not allowed "
					       "(cannot hoist temporary variable outside a function body)");
			if (in_struct)
				error_tok(s, "orelse inside array dimension in a struct/union body "
					       "cannot be transformed (statement expressions are not "
					       "allowed in struct/union definitions)");
			int paren_depth = 0;
			for (Token *p = tok_next(tok); p && p != s; p = tok_next(p)) {
				if (match_ch(p, '(')) paren_depth++;
				else if (match_ch(p, ')')) paren_depth--;
			}
			if (paren_depth > 1)
				error_tok(s, "'orelse' inside array dimension could not be transformed; "
					     "if wrapped in outer parentheses, remove them: "
					     "use '[f() orelse 1]' not '[(f() orelse 1)]'");
			validate_bracket_orelse(s);
			if (oe_depth == 0 && prev_d0_oe)
				reject_orelse_side_effects(
					tok_next(prev_d0_oe), s,
					"'orelse' in array dimension",
					"in a chained 'orelse' (would be "
					"evaluated twice); hoist the "
					"expression to a variable first",
					false, true, false);
			if (oe_depth == 0) prev_d0_oe = s;
			tok_ann(s) |= P1_OE_BRACKET;
			found_oe = true;
		}
	}
	if (found_oe) {
		for (Token *s = tok_next(tok); s && s != close && s->kind != TK_EOF; s = tok_next(s)) {
			if (is_pp_conditional(s))
				error_tok(s, "'orelse' inside array dimension cannot be used when the "
					    "dimension spans preprocessor conditionals — the "
					    "transpiler would emit tokens from all branches, "
					    "producing invalid C; "
					    "use 'cc -E' preprocessing or a temporary variable");
		}
		tok_ann(tok) |= P1_OE_BRACKET;
	}
}

// Phase 1D: validate bare orelse in expression statements.
// Extracted from p1_full_depth_prescan to reduce main loop icache footprint.
static void __attribute__((noinline))
p1d_validate_bare_orelse(Token *tok, Token *bare_oe) {
	// Skip past depth-0 commas: orelse only applies to
	// the sub-expression after the last comma operator.
	Token *scan_start = tok;
	{
		Token *last_comma = NULL;
		int cd = 0;
		for (Token *s = tok; s != bare_oe; s = tok_next(s)) {
			if (s->flags & TF_OPEN) cd++;
			else if (s->flags & TF_CLOSE) cd--;
			else if (cd == 0 && match_ch(s, ',')) last_comma = s;
		}
		if (last_comma) scan_start = tok_next(last_comma);
	}
	int sd = 0;
	bool has_eq = false;
	Token *eq_tok = NULL;
	for (Token *s = scan_start; s != bare_oe; s = tok_next(s)) {
		if (s->flags & TF_OPEN) { sd++; continue; }
		if (s->flags & TF_CLOSE) { sd--; continue; }
		if (sd == 0 && is_assignment_operator_token(s)) {
			if (!match_ch(s, '='))
				error_tok(s, "bare assignment with 'orelse' cannot use compound operators "
					  "(e.g. +=, -=); use a plain '=' assignment");
			has_eq = true;
			eq_tok = s;
		}
	}
	if (tok == bare_oe)
		error_tok(tok, "expected expression before 'orelse'");
	Token *after_oe = tok_next(bare_oe);
	if (after_oe && match_ch(after_oe, ';'))
		error_tok(after_oe, "expected statement after 'orelse'");
	if (!has_eq && is_orelse_value_fallback(after_oe))
		error_tok(after_oe, "orelse fallback requires an assignment target "
			  "(use a declaration)");

	// Modifiable lvalue validation: reject obvious non-lvalue targets.
	if (has_eq && eq_tok) {
		// Reject cast-expression targets: (type)expr = ...
		// A cast target at the start of the LHS is never an lvalue.
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
	}

	// Compound literal fallback + volatile dereference in LHS.
	// The compound literal ternary path evaluates LHS twice:
	// (LHS = RHS) ? (void)0 : (void)(LHS = fb)
	// Reject if LHS has pointer deref, member access, or subscript
	// AND the fallback contains a compound literal '{'.
	if (has_eq && eq_tok && is_orelse_value_fallback(after_oe)) {
		bool fb_has_cl = false;
		{ int fd = 0;
		  for (Token *s = after_oe; s && s->kind != TK_EOF; s = tok_next(s)) {
			if (fd == 0 && match_ch(s, '{')) { fb_has_cl = true; break; }
			if (s->flags & TF_OPEN) fd++;
			else if (s->flags & TF_CLOSE) fd--;
			else if (fd == 0 && (match_ch(s, ';') || match_ch(s, ','))) break;
		  }
		}
		if (fb_has_cl)
			reject_orelse_side_effects(scan_start, eq_tok,
				"orelse compound-literal fallback on assignment",
				"in the target expression (volatile double-write "
				"with compound literal fallback); "
				"use a temporary variable instead",
				false, true, false);
	}

	// Reject bare orelse spanning preprocessor conditionals (#ifdef/#else/etc.).
	// emit_range_no_prep / emit_balanced_range skip TK_PREP_DIR tokens,
	// producing concatenated code from ALL branches — silent miscompilation.
	if (has_eq) {
		int pd = 0;
		for (Token *s = scan_start; s && s->kind != TK_EOF; s = tok_next(s)) {
			if (s->flags & TF_OPEN) pd++;
			else if (s->flags & TF_CLOSE) pd--;
			else if (pd == 0 && match_ch(s, ';')) break;
			if (is_pp_conditional(s))
				error_tok(bare_oe,
					  "bare orelse assignment cannot be used when the "
					  "expression spans preprocessor conditionals — the "
					  "transpiler would emit tokens from all branches, "
					  "producing invalid C; use a temporary variable or "
					  "move the #ifdef outside the expression");
		}
	}
}

// Scan a declaration initializer for orelse keywords.
// t points at the '=' token; returns position after initializer (typically ',' or ';').
static Token *p1d_scan_init_orelse(Token *t, bool *out_has_orelse, Token **out_first_orelse) {
	Token *prev_init_tok = NULL;
	bool init_is_first = true;
	int init_td = 0;
	t = tok_next(t); // skip '='
	while (t && !match_ch(t, ',') && !match_ch(t, ';') && t->kind != TK_EOF) {
		if (match_ch(t, '?')) { init_td++; init_is_first = false; prev_init_tok = t; t = tok_next(t); continue; }
		if (match_ch(t, ':') && init_td > 0) { init_td--; init_is_first = false; prev_init_tok = t; t = tok_next(t); continue; }
		// Phase 1G: mark orelse in decl initializer
		if (is_orelse_kw_shadow(t)) {
			// Shadow: only treat as keyword when preceded by expression-ending token
			TypedefEntry *te_init = typedef_lookup(t);
			if (te_init && te_init->is_shadow) {
				bool pend = prev_init_tok &&
				    (match_ch(prev_init_tok, ')') || match_ch(prev_init_tok, ']') ||
				     match_ch(prev_init_tok, '}') ||
				     prev_init_tok->kind == TK_IDENT || prev_init_tok->kind == TK_KEYWORD ||
				     prev_init_tok->kind == TK_NUM || prev_init_tok->kind == TK_STR);
				if (!pend) { prev_init_tok = t; t = tok_next(t); init_is_first = false; continue; }
			}
			// Validate structural position: preceding token must end an expression
			if (prev_init_tok &&
			    !match_ch(prev_init_tok, ')') && !match_ch(prev_init_tok, ']') &&
			    !match_ch(prev_init_tok, '}') &&
			    prev_init_tok->kind != TK_IDENT && prev_init_tok->kind != TK_KEYWORD &&
			    prev_init_tok->kind != TK_NUM && prev_init_tok->kind != TK_STR)
				error_tok(t, "'orelse' cannot be used here (it must appear at the "
					  "statement level in a declaration or bare expression)");
			if (init_td > 0)
				error_tok(t, "'orelse' cannot be used inside a ternary expression");
			tok_ann(t) |= P1_OE_DECL_INIT;
			if (!*out_first_orelse) *out_first_orelse = t;
			*out_has_orelse = true;
		}
		if (t->flags & TF_OPEN) {
			// Look one level into paren group that
			// spans the rest of the initializer for
			// orelse (macro hygiene pattern).
			// Only when the paren is the first token
			// of the initializer (i.e. wraps it entirely).
			Token *m = tok_match(t);
			if (m && match_ch(t, '(')) {
				Token *am = tok_next(m);
				if (init_is_first &&
				    (!am || match_ch(am, ',') || match_ch(am, ';') || am->kind == TK_EOF)) {
					Token *prev_inner = NULL;
					bool p1d_inner_d0_comma = false;
					for (Token *inner = tok_next(t); inner && inner != m; inner = tok_next(inner)) {
						if (match_ch(inner, ','))
							p1d_inner_d0_comma = true;
						if (is_orelse_kw_shadow(inner)) {
							TypedefEntry *te_inner = typedef_lookup(inner);
							if (te_inner && te_inner->is_shadow) {
								bool pi_end = prev_inner &&
								    (match_ch(prev_inner, ')') || match_ch(prev_inner, ']') ||
								     prev_inner->kind == TK_IDENT || prev_inner->kind == TK_KEYWORD ||
								     prev_inner->kind == TK_NUM || prev_inner->kind == TK_STR);
								if (!pi_end) { prev_inner = inner; continue; }
							}
							if (prev_inner &&
							    !match_ch(prev_inner, ')') && !match_ch(prev_inner, ']') &&
							    prev_inner->kind != TK_IDENT && prev_inner->kind != TK_KEYWORD &&
							    prev_inner->kind != TK_NUM && prev_inner->kind != TK_STR)
								error_tok(inner, "'orelse' cannot be used here (it must appear at the "
									  "statement level in a declaration or bare expression)");
							tok_ann(inner) |= P1_OE_DECL_INIT;
							if (!*out_first_orelse) *out_first_orelse = inner;
							*out_has_orelse = true;
						}
						if (inner->flags & TF_OPEN) {
							if (FEAT(F_ORELSE) && match_ch(inner, '(') &&
							    !(prev_inner && (prev_inner->tag & TT_TYPEOF)))
								check_orelse_in_parens(inner);
							inner = tok_match(inner) ? tok_match(inner) : inner; prev_inner = inner; continue;
						}
						prev_inner = inner;
					}
					// Depth-0 comma means paren can't be
					// unlinked; undo P1_OE_DECL_INIT tags
					// (mirrors scan_decl_orelse's check).
					if (p1d_inner_d0_comma && *out_has_orelse) {
						for (Token *u = tok_next(t); u && u != m; u = tok_next(u)) {
							tok_ann(u) &= (uint8_t)~P1_OE_DECL_INIT;
							if (u->flags & TF_OPEN && tok_match(u)) { u = tok_match(u); continue; }
						}
						*out_has_orelse = false;
						*out_first_orelse = NULL;
					}
				} else if (FEAT(F_ORELSE) && !(prev_init_tok && (prev_init_tok->tag & TT_TYPEOF))) check_orelse_in_parens(t);
			}
			prev_init_tok = m ? m : t;
			t = m ? tok_next(m) : tok_next(t); init_is_first = false; continue;
		}
		prev_init_tok = t;
		t = tok_next(t);
		init_is_first = false;
	}
	return t;
}

// Validate orelse constraints on a declaration initializer.
// Called when orelse was found; checks for arrays, structs, file-scope,
// static, const-VLA, and statement-expression violations.
static void p1d_validate_decl_orelse(Token *var_name, Token *type_tok,
				     TypeSpecResult *type, DeclResult *decl,
				     Token *first_orelse, bool saw_static,
				     int brace_depth) {
	if (brace_depth == 0)
		error_tok(var_name,
			  "'orelse' cannot be used in file-scope initializers "
			  "(requires runtime fallback code)");
	if (saw_static || type->has_static || type->has_extern)
		error_tok(var_name,
			  "'orelse' cannot be used in the initializer of a "
			  "variable with static or thread storage duration "
			  "(the runtime fallback check would re-execute on "
			  "every function entry, destroying persistence)");
	if (decl->is_array && !decl->paren_pointer)
		error_tok(var_name,
			  "orelse on array variable '%.*s' will never trigger "
			  "(array address is never NULL); remove the orelse clause",
			  var_name->len, tok_loc(var_name));
	if (type->is_struct && !type->is_enum && !decl->is_pointer && !decl->is_array)
		error_tok(var_name,
			  "orelse on struct/union values is not supported "
			  "(memcmp cannot reliably detect zero due to padding)");

	// Reject const-VLA orelse value fallback
	if (first_orelse && (type->is_vla || decl->is_vla)) {
		if (is_orelse_value_fallback(tok_next(first_orelse)) &&
		    has_effective_const_qual(type_tok, type, decl))
			error_tok(first_orelse,
				  "orelse on a const-qualified variably-modified type "
				  "would duplicate the type specifier, causing VLA "
				  "size expressions to be evaluated twice; hoist the "
				  "value to a non-const variable first");
	}

	// Reject GNU statement expressions in orelse fallback values
	if (first_orelse) {
		if (is_orelse_value_fallback(tok_next(first_orelse))) {
			Token *after_oe = tok_next(first_orelse);
			for (Token *s = after_oe; s && s->kind != TK_EOF &&
			     !match_ch(s, ';') && !match_ch(s, ','); s = tok_next(s)) {
				if (match_ch(s, '(') && tok_next(s) &&
				    match_ch(tok_next(s), '{')) {
					error_tok(s,
						  "GNU statement expressions in orelse "
						  "fallback values are not supported; "
						  "use 'orelse { ... }' block form instead");
				}
				if (s->flags & TF_OPEN && tok_match(s)) {
					s = tok_match(s);
					continue;
				}
			}
		}
	}
}

// Check multi-declarator split constraints: reject splits that would
// break with anonymous structs or variably-modified type specifiers.
static void p1d_check_multi_decl_constraints(Token *t, Token *type_tok,
					     TypeSpecResult *type,
					     bool any_would_memset, bool vm_type) {
	// Check if the next declarator would require a split
	Token *next_t = tok_next(t);
	bool nr = false;
	next_t = p1_skip_decl_raw(next_t, &nr);
	DeclResult nd = parse_declarator(next_t, false);
	if (!nd.var_name || !nd.end) return;
	bool split = (any_would_memset && match_ch(nd.end, '=')) ||
	             (FEAT(F_ORELSE) && p1d_decl_has_bracket_orelse(next_t, nd.end));
	if (!split) return;

	// Reject anonymous struct/union multi-declarator split
	if (type->is_struct && !type->is_enum) {
		bool is_anon = false;
		for (Token *s = type_tok; s && s != type->end; s = tok_next(s)) {
			if (s->tag & TT_SUE) {
				Token *after = skip_prep_dirs(tok_next(s));
				if (after && match_ch(after, '{'))
					is_anon = true;
				break;
			}
		}
		if (is_anon)
			error_tok(next_t,
				  "bracket orelse / zero-init requiring declaration split "
				  "cannot be used with anonymous struct/union; "
				  "add a tag name or use a typedef");
	}

	// Reject VM type multi-declarator split
	if (vm_type)
		error_tok(next_t,
			  "multi-declarator with variably-modified "
			  "type specifier requires declaration split which "
			  "would double-evaluate VLA size expressions; "
			  "declare each variable on a separate line");
}

// Scan inside a balanced group (paren or bracket) for ghost enum definitions,
// statement expressions, and invalid defer/orelse in control-flow conditions.
// Returns true if a statement expression was found (caller should process
// tokens one by one instead of skipping the group).
static Token *p1d_scan_balanced_group(Token *tok, int brace_depth, int cur_func,
				      Token *prev_saved) {
	Token *group_end = tok_match(tok);
	Token *stmt_expr_open = NULL;
	Token *prev_inner = NULL;
	int inner_depth = 0;
	for (Token *inner = tok_next(tok); inner && inner != group_end; inner = tok_next(inner)) {
		if (inner->flags & TF_OPEN) inner_depth++;
		if (inner->flags & TF_CLOSE) { inner_depth--; prev_inner = inner; continue; }
		if (is_enum_kw(inner)) {
			Token *brace = find_struct_body_brace(inner);
			if (brace)
				parse_enum_constants(brace, brace_depth);
		}
		if (!stmt_expr_open && match_ch(inner, '(') && tok_next(inner) &&
		    match_ch(tok_next(inner), '{')) {
			stmt_expr_open = inner;
		}
		if (inner_depth == 0 && cur_func >= 0 && prev_saved &&
		    (prev_saved->tag & (TT_IF | TT_LOOP | TT_SWITCH)) &&
		    is_defer_kw(inner, prev_inner) &&
		    tok_next(inner)->kind == TK_IDENT)
			error_tok(inner, "defer cannot appear inside control statement parentheses");
		if (inner_depth == 0 && prev_saved &&
		    (prev_saved->tag & (TT_IF | TT_LOOP | TT_SWITCH)) &&
		    is_orelse_kw(inner) &&
		    !(prev_inner && (prev_inner->tag & TT_MEMBER)))
			error_tok(inner, "'orelse' cannot be used inside control statement condition parentheses");
		prev_inner = inner;
	}
	return stmt_expr_open;
}

// Probe a statement starting with a type-like token as a potential declaration.
// Records typedef shadows (Phase 1C) and per-function decl entries (Phase 1D).
// Read-only probe: does NOT advance the caller's token pointer.
static void p1d_probe_declaration(Token *tok, uint16_t cur_sid, int brace_depth,
				  int cur_func, bool *saw_raw, bool saw_static,
				  bool ctrl_pending, uint32_t *skip_cache) {
	if (!(tok->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_BITINT)) &&
	    !is_known_typedef(tok))
		return;

	Token *type_tok = tok; // token to annotate with P1_IS_DECL
	TypeSpecResult type = parse_type_specifier(tok);
	// parse_type_specifier now skips embedded 'raw' and sets has_raw.
	if (type.has_raw)
		*saw_raw = true;
	// Handle embedded 'raw' between qualifiers and type keyword:
	// e.g. _Atomic raw int z; — parse_type_specifier stops at 'raw'
	if (!type.saw_type && type.end && (type.end->flags & TF_RAW) &&
	    !is_known_typedef(type.end)) {
		Token *after_raw = skip_noise(tok_next(type.end));
		if (after_raw && is_raw_declaration_context(after_raw)) {
			*saw_raw = true;
			type_tok = after_raw;
			type = parse_type_specifier(after_raw);
		}
	}
	if (!type.saw_type)
		return;

	bool annotated = false;
	// Phase 1D: bound braceless control-flow body declarations.
	// C23 labeled declarations (L: int x;) can serve as braceless
	// if/while/for/do bodies.  The variable's scope ends at the
	// statement semicolon, not the enclosing block's '}'.
	uint32_t braceless_close_idx = 0;
	if (ctrl_pending) {
		Token *stmt_end = skip_one_stmt_impl(type_tok, skip_cache);
		if (stmt_end)
			braceless_close_idx = tok_idx(stmt_end);
	}
	Token *t = type.end;
	bool vm_type = (type.has_typeof || type.has_atomic) && type.is_vla;
	bool any_would_memset = false;
	while (t && !match_ch(t, ';') && !match_ch(t, '{') && t->kind != TK_EOF) {
		// Per-declarator 'raw' skip (int x, raw arr[n];)
		bool decl_raw = *saw_raw;
		t = p1_skip_decl_raw(t, &decl_raw);
		DeclResult decl = parse_declarator(t, false);
		if (!decl.var_name || !decl.end) {
			// Detect GNU nested function definitions
			// inside functions using defer/zeroinit.
			if (cur_func >= 0 && brace_depth > 0 &&
			    FEAT(F_DEFER) && decl.var_name) {
				Token *p = skip_noise(tok_next(decl.var_name));
				if (p && match_ch(p, '(') && tok_match(p)) {
					Token *a = tok_next(tok_match(p));
					while (a && (a->tag & (TT_ATTR | TT_ASM))) {
						a = (a->tag & TT_ASM) ? tok_next(a) : skip_noise(a);
						if (a && match_ch(a, '(') && tok_match(a))
							a = tok_next(tok_match(a));
					}
					bool nested = a && match_ch(a, '{');
					if (!nested && a) {
						Token *b = a;
						while (b && b->kind != TK_EOF &&
						       !match_ch(b, '{') && !match_ch(b, '}'))
							b = (b->flags & TF_OPEN && tok_match(b))
								? tok_next(tok_match(b)) : tok_next(b);
						nested = b && match_ch(b, '{') &&
							 is_knr_params(tok_next(tok_match(p)), b);
					}
					if (nested)
						error_tok(decl.var_name,
							  "nested function definitions are not "
							  "supported inside functions using "
							  "defer/zeroinit — move the function "
							  "outside or use a function pointer");
				}
			}
			break;
		}
		if (match_ch(decl.end, '(') && brace_depth == 0) break; // func def
		// Reject cast expressions like (type)value that parse_declarator
		// misidentifies as parenthesized declarators.
		if (decl.end && !match_ch(decl.end, '=') && !match_ch(decl.end, ',') &&
		    !match_ch(decl.end, ';') && !match_ch(decl.end, '[') &&
		    !match_ch(decl.end, '(') && !match_ch(decl.end, '{') &&
		    !match_ch(decl.end, ')'))
			break;

		// Phase 1D: annotate type-start token for Pass 2 fast gate
		if (!annotated && brace_depth > 0) {
			tok_ann(type_tok) |= P1_IS_DECL;
			annotated = true;
		}

		// Phase 1D: reject unbraced declaration in switch body
		if (FEAT(F_ZEROINIT) && brace_depth > 0 && !decl_raw &&
		    braceless_close_idx == 0 &&
		    cur_sid < scope_tree_count && scope_tree[cur_sid].is_switch)
			error_tok(type_tok,
				  "variable declaration directly in switch body without braces. "
				  "Wrap in braces: 'case N: { int x; ... }' to ensure safe zero-initialization, "
				  "or use 'raw' to suppress zero-init.");

		// Phase 1C: shadow detection
		if (is_known_typedef(decl.var_name) ||
		    is_known_enum_const(decl.var_name) ||
		    (decl.var_name->tag & (TT_DEFER | TT_ORELSE)) ||
		    hashmap_get(&p1_func_proto_map, tok_loc(decl.var_name), decl.var_name->len)) {
			p1_register_shadow(decl.var_name, cur_sid, brace_depth);
		}

		t = decl.end;
		bool has_init = match_ch(t, '=');
		bool is_actual_vla = type.is_vla || decl.is_vla;

		// Register VLA variables in typedef table for
		// is_vla_typedef() lookups during Pass 2.
		if (is_actual_vla && decl.var_name && brace_depth > 0) {
			TYPEDEF_ADD_IDX(typedef_add_vla_var(tok_loc(decl.var_name), decl.var_name->len, brace_depth), decl.var_name);
		}

		// Phase 1D: record declaration entry
		if (cur_func >= 0 && decl.var_name && brace_depth > 0) {
			P1FuncEntry *e = p1_alloc(P1K_DECL, cur_sid, decl.var_name);
			e->decl.has_init = has_init;
			e->decl.is_vla = type.is_vla || decl.is_vla;
			e->decl.has_raw = decl_raw;
			e->decl.is_static_storage = saw_static || type.has_static || type.has_extern;
			e->decl.body_close_idx = braceless_close_idx;
			if (!ctrl_pending)
				p1_check_defer_same_block_shadow(decl.var_name, cur_sid, cur_func);
		}

		// Phase 1D: reject register _Atomic aggregate
		if (FEAT(F_ZEROINIT) && brace_depth > 0 && !has_init && !decl_raw &&
		    !type.has_extern && !type.has_static &&
		    type.has_register && type.has_atomic) {
			bool is_aggregate = (decl.is_array && (!decl.paren_pointer || decl.paren_array)) ||
				((type.is_struct || type.is_typedef) && !decl.is_pointer);
			if (is_aggregate)
				error_tok(decl.var_name,
					  "'register _Atomic' aggregate cannot be safely "
					  "zero-initialized; remove 'register' or use 'raw' "
					  "to opt out of automatic initialization");
		}

		if (has_init) {
			bool decl_has_orelse = false;
			Token *first_orelse = NULL;
			t = p1d_scan_init_orelse(t, &decl_has_orelse, &first_orelse);

			if (decl_has_orelse && FEAT(F_ORELSE))
				p1d_validate_decl_orelse(decl.var_name, type_tok,
					&type, &decl, first_orelse,
					saw_static, brace_depth);
		}

		// Phase 1D: track whether this declarator would need
		// typeof memset in Pass 2 (for split detection).
		if (FEAT(F_ZEROINIT) && !has_init && !decl_raw &&
		    !(saw_static || type.has_static || type.has_extern) &&
		    !type.has_register && (!decl.is_pointer || decl.is_array) &&
		    (type.has_typeof || (type.has_atomic && type.is_struct) ||
		     type.is_vla || decl.is_vla))
			any_would_memset = true;

		// Phase 1D: reject multi-declarator split constraints
		if (t && match_ch(t, ',') && brace_depth > 0)
			p1d_check_multi_decl_constraints(t, type_tok, &type,
				any_would_memset, vm_type);

		if (t && match_ch(t, ',')) { t = tok_next(t); } else break;
	}
}

static void p1_full_depth_prescan(Token *tok) {
	bool at_stmt_start = true;
	int brace_depth = 0;
	uint16_t next_scope_id = 1;

	// Phase 1E: file-scope return type capture state
	Token *file_scope_stmt_start = tok;
	bool p1e_ret_void = false;
	bool p1e_ret_captured = false;

	// Phase 1F: scope_id stack for determining in_loop/in_switch context
	// Arena-allocated to avoid leaks on longjmp error recovery (PRISM_LIB_MODE)
	int p1d_scope_cap = 256;
	uint16_t *scope_stack_local = arena_alloc_uninit(&ctx->main_arena, p1d_scope_cap * sizeof(uint16_t));
	scope_stack_local[0] = 0; // file scope
	int scope_depth_local = 0;
#define CUR_SID() (scope_stack_local[scope_depth_local])
#define P1D_STMT_RESET() do { at_stmt_start = true; p1d_saw_raw = false; \
	p1d_saw_static = false; p1d_ctrl_pending = false; } while(0)
#define P1D_SWITCH_ENSURE_CAP() do { if (p1d_switch_top >= p1d_switch_cap) { \
	int _old = p1d_switch_cap; p1d_switch_cap *= 2; \
	p1d_switch_stack = arena_realloc(&ctx->main_arena, p1d_switch_stack, _old * sizeof(uint16_t), p1d_switch_cap * sizeof(uint16_t)); \
	p1d_switch_end = arena_realloc(&ctx->main_arena, p1d_switch_end, _old * sizeof(uint32_t), p1d_switch_cap * sizeof(uint32_t)); \
	} } while(0)

	// Phase 3A: initialize scope range for file-scope typedef registration
	td_scope_open = 0;
	td_scope_close = UINT32_MAX;

	// Phase 1D: per-function entry collection state
	int p1d_cur_func = -1;    // index into func_meta[], -1 when outside functions
	// Switch scope stack: track innermost switch scope_id for case label association
	int p1d_switch_cap = 64;
	uint16_t *p1d_switch_stack = arena_alloc_uninit(&ctx->main_arena, p1d_switch_cap * sizeof(uint16_t));
	uint32_t *p1d_switch_end = arena_alloc_uninit(&ctx->main_arena, p1d_switch_cap * sizeof(uint32_t));
	int p1d_switch_top = 0;
	uint32_t p1d_braceless_next_sid = scope_tree_count; // synthetic scope IDs for braceless switches
	Token *p1d_prev = NULL;   // previous non-whitespace token (for label detection)
	bool p1d_saw_raw = false;  // Phase 1D: track 'raw' keyword preceding declaration
	bool p1d_saw_static = false; // Phase 1D: track static/extern storage class preceding declaration
	int p1d_init_brace_depth = 0; // depth of initializer braces (= { ... }); labels suppressed inside
	bool p1d_ctrl_pending = false; // true when next stmt is braceless control flow body

	// O(N) amortization cache for skip_one_stmt: indexed by tok_idx,
	// non-zero entry = tok_idx(result) + 1.  Backfilled by skip_one_stmt_impl
	// so nested control-flow chains don't rescan the same tokens.
	uint32_t *skip_cache = arena_alloc(&ctx->main_arena, token_count * sizeof(uint32_t));

	while (tok && tok->kind != TK_EOF) {
		// Pop braceless switches whose body has ended
		while (p1d_switch_top > 0 && p1d_switch_end[p1d_switch_top - 1] > 0 &&
		       tok_idx(tok) > p1d_switch_end[p1d_switch_top - 1])
			p1d_switch_top--;

		// Phase 1: record function prototypes/definitions.
		// At brace_depth==0, any ident( is a file-scope prototype/def.
		// At brace_depth>0, require a type keyword/qualifier before the
		// ident to distinguish block-scope declarations (int add(int,int);)
		// from function calls (fp(10)).  Without this, typeof(func)
		// for block-scope-only forward declarations triggers spurious
		// memset in process_declarators.
		// Used by Pass 2 to avoid memset on typeof(func) declarations.
		if (tok->kind == TK_IDENT) {
			Token *nx = tok_next(tok);
			if (nx && match_ch(nx, '(')) {
				if (brace_depth == 0) {
					hashmap_put(&p1_func_proto_map, tok_loc(tok), tok->len, (void *)1);
				} else {
					uint32_t ti = tok_idx(tok);
					if (ti > 0) {
						Token *prev = &token_pool[ti - 1];
						if (prev->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_SUE | TT_TYPEOF))
							hashmap_put(&p1_func_proto_map, tok_loc(tok), tok->len, (void *)1);
					}
				}
			}
		}

		if (FEAT(F_ORELSE) && match_ch(tok, '[') && tok_match(tok))
			p1d_classify_bracket_orelse(tok, CUR_SID(), p1d_cur_func);

		// Phase 1: reject orelse in enum constant expressions and
		// struct/union bodies early, before any Pass 2 output is written.
		if (FEAT(F_ORELSE) && (tok->tag & TT_ORELSE) && !typedef_lookup(tok)) {
			uint16_t cur_sid = CUR_SID();
			if (cur_sid < scope_tree_count &&
			    (scope_tree[cur_sid].is_enum || scope_tree[cur_sid].is_struct))
				error_tok(tok,
					  "'orelse' cannot be used here (it must appear at the "
					  "statement level in a declaration or bare expression)");
		}

		// Phase 1: reject orelse inside typeof in struct/union bodies early,
		// before any Pass 2 output is written.
		if (FEAT(F_ORELSE) && (tok->tag & TT_TYPEOF)) {
			Token *paren = tok_next(tok);
			if (paren && match_ch(paren, '(') && tok_match(paren)) {
				uint16_t cur_sid = CUR_SID();
				const char *msg = NULL;
				if (cur_sid > 0 && cur_sid < scope_tree_count && scope_tree[cur_sid].is_struct)
					msg = "'orelse' inside typeof in a struct/union body "
					      "cannot be transformed; use the resolved type directly";
				else if (p1d_cur_func < 0)
					msg = "'orelse' inside typeof at file scope is not allowed";
				if (msg) {
					for (Token *s = tok_next(paren); s && s != tok_match(paren); s = tok_next(s))
						if ((s->tag & TT_ORELSE) && !typedef_lookup(s))
							error_tok(s, msg);
				}
				// Phase 1D: reject side effects in typeof orelse LHS
				// (hoisted from Pass 2 emit_token_range_orelse).
				if (!msg) {
					for (Token *s = tok_next(paren); s && s != tok_match(paren); s = tok_next(s)) {
						if (is_orelse_kw_shadow(s)) {
							TypedefEntry *te_s = typedef_lookup(s);
							if (!te_s || te_s->is_shadow) {
								reject_orelse_side_effects(
									tok_next(paren), s,
									"'orelse' in typeof",
									"in the LHS (would be evaluated twice); "
									"hoist the expression to a variable first",
									false, true, false);
								break;
							}
						}
						if (s->flags & TF_OPEN && tok_match(s)) { s = tok_match(s); continue; }
					}
				}
			}
		}

		if (match_ch(tok, '{')) {
			// Advance past any scope IDs consumed by braces that were
			// skipped (e.g., inside typedef bodies or non-stmt-start TF_OPEN).
			uint32_t tidx = tok_idx(tok);
			while (next_scope_id < scope_tree_count &&
			       scope_tree[next_scope_id].open_tok_idx < tidx)
				next_scope_id++;
uint16_t sid = next_scope_id++;

			// Phase 1E: function body detection at file scope
			if (brace_depth == 0 && sid < scope_tree_count &&
			    scope_tree[sid].is_func_body) {
				// Capture return type from the declaration start.
				// K&R param declarations (e.g. "void f(a) int a; {")
				// reset file_scope_stmt_start at each ";", so if
				// capture fails, walk backward from '{' past K&R
				// declarations to find the actual function start.
				int ret = capture_function_return_type(file_scope_stmt_start);
				if (ret == 0) {
					Token *prev = p1_find_prev_skipping_attrs(tok_idx(tok) - 1);
					if (prev && match_ch(prev, ';'))
						prev = p1_knr_find_close_paren(prev);
					if (prev && match_ch(prev, ')') && tok_match(prev)) {
						// Found param list — scan backward for declaration start
						Token *open = tok_match(prev);
						for (uint32_t pi = tok_idx(open); pi > 0; pi--) {
							Token *pt = &token_pool[pi - 1];
							if (pt->kind == TK_PREP_DIR) continue;
							if (match_ch(pt, '{') || match_ch(pt, '}') ||
							    match_ch(pt, ';'))
								break;
							file_scope_stmt_start = pt;
						}
						ret = capture_function_return_type(file_scope_stmt_start);
					}
				}
				p1e_ret_void = (ret == 1);
				p1e_ret_captured = (ret == 2);

				// Store FuncMeta entry
				ARENA_ENSURE_CAP(&ctx->main_arena, ctx->p1_func_meta,
						 func_meta_count, func_meta_cap, 64, FuncMeta);
				FuncMeta *fm = &func_meta[func_meta_count++];
				fm->body_open = tok;
				fm->returns_void = p1e_ret_void;
				fm->defer_name_set = (HashMap){0};
				if (p1e_ret_captured) {
					fm->ret_type_start = ctx->func_ret_type_start;
					fm->ret_type_end = ctx->func_ret_type_end;
					fm->ret_type_suffix_start = ctx->func_ret_type_suffix_start;
					fm->ret_type_suffix_end = ctx->func_ret_type_suffix_end;
				} else {
					fm->ret_type_start = fm->ret_type_end = NULL;
					fm->ret_type_suffix_start = fm->ret_type_suffix_end = NULL;
				}
				p1e_ret_void = false;
				p1e_ret_captured = false;

				// Phase 1C: register parameter shadows at the function body scope
				// Set scope range for param shadow registration
				if (sid < scope_tree_count) {
					td_scope_open = scope_tree[sid].open_tok_idx;
					td_scope_close = scope_tree[sid].close_tok_idx;
				}
				// Walk backward from '{' to find the parameter list '(...)'.
				// Normal (prototype-style) functions: prev_tok is ')'.
				// K&R functions: prev_tok is ';' (end of last param declaration
				// like "int a;"); scan further back to find the identifier-list ')'.
				Token *prev_tok = p1_find_prev_skipping_attrs(tok_idx(tok) - 1);
				if (prev_tok && match_ch(prev_tok, ';'))
					prev_tok = p1_knr_find_close_paren(prev_tok);
				if (prev_tok && match_ch(prev_tok, ')') && tok_match(prev_tok))
					p1_register_param_shadows(tok_match(prev_tok), prev_tok,
								  sid, brace_depth + 1, true);
			}

			// Phase 1D: enter function body — record entry start
			if (brace_depth == 0 && sid < scope_tree_count &&
			    scope_tree[sid].is_func_body) {
				p1d_cur_func = func_meta_count - 1;
				func_meta[p1d_cur_func].entry_start = p1_entry_count;
				func_meta[p1d_cur_func].entry_count = 0;
				p1d_switch_top = 0;
				p1d_prev = NULL;
			}

			// Phase 1D: track switch scope for case label association
			if (p1d_cur_func >= 0 && sid < scope_tree_count &&
			    scope_tree[sid].is_switch) {
				P1D_SWITCH_ENSURE_CAP();
				p1_alloc(P1K_SWITCH, sid, tok);
				p1d_switch_stack[p1d_switch_top] = sid;
				p1d_switch_end[p1d_switch_top] = 0; // braced: popped at }
				p1d_switch_top++;
			}

			brace_depth++;
			scope_depth_local++;
			if (scope_depth_local >= p1d_scope_cap) {
				int old_cap = p1d_scope_cap;
				p1d_scope_cap *= 2;
				scope_stack_local = arena_realloc(&ctx->main_arena, scope_stack_local,
					old_cap * sizeof(uint16_t), p1d_scope_cap * sizeof(uint16_t));
			}
			scope_stack_local[scope_depth_local] = sid;

			// Track initializer brace depth: suppress label detection inside '= { ... }'
			if (sid < scope_tree_count && scope_tree[sid].is_init)
				p1d_init_brace_depth++;

			// Update scope range for typedef_add_entry
			if (sid < scope_tree_count) {
				td_scope_open = scope_tree[sid].open_tok_idx;
				td_scope_close = scope_tree[sid].close_tok_idx;
			}

			P1D_STMT_RESET();
			tok = tok_next(tok);
			continue;
		}
		if (match_ch(tok, '}')) {
			// Phase 1D: pop switch scope(s) that end at this brace
			if (p1d_cur_func >= 0 && p1d_switch_top > 0) {
				uint16_t closing_sid = CUR_SID();
				if (p1d_switch_end[p1d_switch_top - 1] == 0 &&
				    p1d_switch_stack[p1d_switch_top - 1] == closing_sid)
					p1d_switch_top--;
			}

			// Decrement initializer-brace depth before popping the scope
			if (p1d_init_brace_depth > 0) {
				uint16_t csid = scope_stack_local[scope_depth_local];
				if (csid < scope_tree_count && scope_tree[csid].is_init)
					p1d_init_brace_depth--;
			}

			if (brace_depth > 0) {
				brace_depth--;
				if (scope_depth_local > 0) scope_depth_local--;
			}

			// Restore scope range for typedef_add_entry to enclosing scope
			{
				uint16_t cur_sid = CUR_SID();
				if (cur_sid > 0 && cur_sid < scope_tree_count) {
					td_scope_open = scope_tree[cur_sid].open_tok_idx;
					td_scope_close = scope_tree[cur_sid].close_tok_idx;
				} else {
					td_scope_open = 0;
					td_scope_close = UINT32_MAX;
				}
			}

			// Phase 1D: finalize function entry count
			if (brace_depth == 0 && p1d_cur_func >= 0) {
				func_meta[p1d_cur_func].entry_count =
					p1_entry_count - func_meta[p1d_cur_func].entry_start;
				p1d_cur_func = -1;
				file_scope_stmt_start = tok_next(tok);
				p1e_ret_void = false;
				p1e_ret_captured = false;
			}

			P1D_STMT_RESET();
			p1d_prev = tok;
			tok = tok_next(tok);
			continue;
		}

		if (match_ch(tok, ';')) {
			P1D_STMT_RESET();
			p1d_prev = tok;
			if (brace_depth == 0) {
				// Phase 1C: C99 prototype parameter scope (§6.2.1p4).
				// Register shadows for param names that collide with typedefs
				// so that subsequent params in the same list see the shadow.
				// Scope is bounded to the parameter list '(...)'.
				Token *prev_tok = p1_find_prev_skipping_attrs(tok_idx(tok) - 1);
				if (prev_tok && match_ch(prev_tok, ')') && tok_match(prev_tok)) {
					Token *open = tok_match(prev_tok);
					uint32_t saved_open = td_scope_open;
					uint32_t saved_close = td_scope_close;
					td_scope_open = tok_idx(open);
					td_scope_close = tok_idx(prev_tok);
					p1_register_param_shadows(open, prev_tok, 0, 1, false);
					td_scope_open = saved_open;
					td_scope_close = saved_close;
				}

				p1e_ret_void = false;
				p1e_ret_captured = false;
				file_scope_stmt_start = tok_next(tok);
			}
			tok = tok_next(tok);
			continue;
		}
		if (tok->kind == TK_PREP_DIR) {
			at_stmt_start = true;
			p1d_saw_raw = false;
			p1d_saw_static = false;
			if (brace_depth == 0)
				file_scope_stmt_start = tok_next(tok);
			tok = tok_next(tok);
			continue;
		}

		if (!at_stmt_start) {
			// Phase 1D: detect gotos and defers even in non-stmt-start
			// positions (e.g., braceless `if (c) goto L;`)
			if (p1d_cur_func >= 0) {
				uint16_t cur_sid = CUR_SID();
				p1d_record_goto(tok, cur_sid, p1d_cur_func);
				if (is_defer_kw(tok, p1d_prev))
					p1_try_alloc_defer(tok, cur_sid, p1d_cur_func);
			}

			// Register enum constants from ghost enums in expressions
			// (e.g., (enum { N = 5 }) in array brackets or casts).
			if (is_enum_kw(tok)) {
				Token *brace = find_struct_body_brace(tok);
				if (brace)
					parse_enum_constants(brace, brace_depth);
			}

			Token *p1d_prev_saved = p1d_prev;
			p1d_prev = tok;
			if (tok->flags & TF_OPEN && tok_match(tok)) {
				// Do not skip GNU statement expressions — process their body normally
				if (is_stmt_expr_open(tok)) {
					tok = tok_next(tok); // advance past '(' to '{'
					at_stmt_start = true;
					continue;
				}
				// Phase 1D: reject orelse/defer inside non-control-flow
				// parentheses (hoisted from Pass 2 check_orelse_in_parens).
				// Skip control-flow condition parens (if/while/for/switch),
				// typeof parens, attribute/asm parens — orelse inside those
				// is valid, handled separately, or irrelevant.
				if (FEAT(F_ORELSE) && match_ch(tok, '(') &&
				    !(p1d_prev_saved && (p1d_prev_saved->tag & (TT_IF | TT_LOOP | TT_SWITCH | TT_TYPEOF | TT_ATTR | TT_ASM))))
					check_orelse_in_parens(tok);
				// Peek inside balanced groups for ghost enum definitions
				// and nested statement expressions:
				if (match_ch(tok, '(') || match_ch(tok, '[')) {
					Token *se_open = p1d_scan_balanced_group(tok, brace_depth, p1d_cur_func, p1d_prev_saved);
					if (se_open) {
						tok = se_open;
						continue;
					}
				}
				tok = tok_next(tok_match(tok));
				// After control-flow condition (...), token is stmt start
				if (p1d_prev_saved && (p1d_prev_saved->tag & (TT_IF | TT_LOOP | TT_SWITCH))) {
					at_stmt_start = true;
					p1d_ctrl_pending = true;
				}
			} else
				tok = tok_next(tok);
			continue;
		}

		// Skip noise (attributes, C23 [[...]], pragmas)
		Token *clean = skip_noise(tok);
		if (clean != tok) { tok = clean; continue; }

		// Skip storage/inline/noreturn/extension specifiers before type
		if ((tok->tag & (TT_STORAGE | TT_INLINE)) || equal(tok, "__extension__")) {
			if (tok->tag & TT_STORAGE)
				p1d_saw_static = true;
			tok = tok_next(tok);
			continue;
		}

		// Skip 'raw' keyword (Prism extension) — but not if it's a label (raw:)
		if ((tok->flags & TF_RAW) && !is_known_typedef(tok)) {
			Token *rnext = tok_next(tok);
			if (!(rnext && match_ch(rnext, ':') && !(tok_next(rnext) && match_ch(tok_next(rnext), ':')))) {
				p1d_saw_raw = true;
				tok = tok_next(tok);
				continue;
			}
		}

		if (tok->tag & TT_TYPEDEF) {
			parse_typedef_declaration(tok, brace_depth);
			// Walk typedef body to register shadows inside struct/union bodies.
			// Braces consumed here don't increment next_scope_id — the main '{'
			// handler auto-advances past skipped scope IDs using open_tok_idx.
			while (tok && tok->kind != TK_EOF && !match_ch(tok, ';')) {
				if (is_enum_kw(tok)) {
					Token *brace = find_struct_body_brace(tok);
					if (brace) parse_enum_constants(brace, brace_depth);
				}
				if (match_ch(tok, '{') && tok_match(tok)) {
					Token *close = tok_match(tok);
					uint32_t saved_open = td_scope_open;
					uint32_t saved_close = td_scope_close;
					td_scope_open = tok_idx(tok);
					td_scope_close = tok_idx(close);
					for (Token *m = tok_next(tok); m && m != close && m->kind != TK_EOF; ) {
						if (is_enum_kw(m)) {
							Token *brace = find_struct_body_brace(m);
							if (brace) {
								// Enum constants leak to enclosing scope, not struct body.
								uint32_t so = td_scope_open, sc = td_scope_close;
								td_scope_open = saved_open;
								td_scope_close = saved_close;
								parse_enum_constants(brace, brace_depth);
								td_scope_open = so;
								td_scope_close = sc;
							}
						}
						if (m->flags & TF_OPEN && tok_match(m)) {
							m = tok_next(tok_match(m));
							continue;
						}
						if (is_valid_varname(m) &&
						    (is_known_typedef(m) || (m->tag & (TT_DEFER | TT_ORELSE)))) {
							Token *nxt = tok_next(m);
							if (nxt && (match_ch(nxt, ';') || match_ch(nxt, ',') ||
							    match_ch(nxt, ':') || match_ch(nxt, '[') ||
							    match_ch(nxt, '='))) {
								// Anonymous bitfield: T : width — m is the type, not a field name.
								// Only shadow if a type specifier precedes m in this member.
								if (match_ch(nxt, ':')) {
									bool has_type = false;
									for (uint32_t pi = tok_idx(m); pi > tok_idx(tok); pi--) {
										Token *pt = &token_pool[pi - 1];
										if (pt->kind == TK_PREP_DIR) continue;
										if (match_set(pt, CH(';') | CH(',')) || match_ch(pt, '{') || match_ch(pt, '}')) break;
										if (pt->tag & TT_QUALIFIER) continue;
										if (match_ch(pt, ')') && tok_match(pt)) { pi = tok_idx(tok_match(pt)) + 1; continue; }
										if (match_ch(pt, ']') && tok_match(pt)) { pi = tok_idx(tok_match(pt)) + 1; continue; }
										has_type = true;
										break;
									}
									if (!has_type) { m = tok_next(m); continue; }
								}
								TYPEDEF_ADD_IDX(typedef_add_shadow(tok_loc(m), m->len, brace_depth + 1), m);
							}
						}
						m = tok_next(m);
					}
					td_scope_open = saved_open;
					td_scope_close = saved_close;
					tok = tok_next(close);
					continue;
				}
				if (tok->flags & TF_OPEN && tok_match(tok))
					tok = tok_next(tok_match(tok));
				else
					tok = tok_next(tok);
			}
			if (tok && match_ch(tok, ';')) tok = tok_next(tok);
			at_stmt_start = true;
			if (brace_depth == 0)
				file_scope_stmt_start = tok;
			continue;
		}

		// Does NOT continue — falls through to Phase 1C/1D for declaration detection.
		if (tok->tag & TT_SUE) {
			Token *brace = find_struct_body_brace(tok);
			if (brace) {
				if (is_enum_kw(tok))
					parse_enum_constants(brace, brace_depth);
				else if (struct_body_contains_vla(brace)) {
					// Register struct/union tag as VLA so later
					// "struct S s;" can detect the VLA member.
					for (Token *t = tok_next(tok); t && t != brace; t = tok_next(t))
						if (is_valid_varname(t)) {
							TYPEDEF_ADD_IDX(typedef_add_vla_var(tok_loc(t), t->len, brace_depth), t);
							break;
						}
				}
			}
		}

		if (equal(tok, "_Static_assert") || equal(tok, "static_assert")) {
			tok = skip_to_semicolon(tok);
			if (tok && match_ch(tok, ';')) tok = tok_next(tok);
			at_stmt_start = true;
			if (brace_depth == 0)
				file_scope_stmt_start = tok;
			continue;
		}

		// for (int T = 0; ...) scopes T to the entire loop (condition,
		// increment, AND body), not just the parenthesized header.
		if ((tok->tag & TT_LOOP) && tok->ch0 == 'f' && brace_depth > 0 && p1d_cur_func >= 0) {
			Token *for_open = p1d_find_open_paren(tok);
			if (for_open && tok_match(for_open)) {
				Token *for_close = tok_match(for_open);
				Token *for_init_end = find_init_semicolon(for_open, for_close);
				if (for_init_end) {
					// C99 §6.8.5p3: for-init scope extends to the entire loop body.
					uint32_t body_end_idx = tok_idx(for_close);
					Token *body_start_for = skip_prep_dirs(tok_next(for_close));
					if (body_start_for && match_ch(body_start_for, '{') && tok_match(body_start_for))
						body_end_idx = tok_idx(tok_match(body_start_for));
					else {
						Token *stmt_end = skip_one_stmt_impl(body_start_for, skip_cache);
						if (stmt_end)
							body_end_idx = tok_idx(stmt_end);
					}
					uint16_t cur_sid = CUR_SID();
					uint16_t body_sid = find_body_scope_id(body_start_for);
					p1_scan_init_shadows(for_open, for_init_end, body_end_idx, cur_sid, brace_depth,
							     body_sid, body_end_idx);
				}
			}
		}

		if ((tok->tag & (TT_IF | TT_SWITCH)) && brace_depth > 0 && p1d_cur_func >= 0 &&
		    !((tok->tag & TT_IF) && tok->ch0 == 'e')) {
			Token *is_open = p1d_find_open_paren(tok);
			if (is_open && tok_match(is_open)) {
				Token *is_close = tok_match(is_open);
				Token *is_init_end = find_init_semicolon(is_open, is_close);
				if (is_init_end) {
					uint16_t cur_sid = CUR_SID();
					Token *body_start_is = skip_prep_dirs(tok_next(is_close));
					uint16_t body_sid = find_body_scope_id(body_start_is);
					/* Extend scope_close_idx past the if/switch body so the
					 * init-statement shadow covers the entire body, not just
					 * up to ')'.  Mirrors the for-loop init path which calls
					 * skip_one_stmt to reach body_end_idx. */
					uint32_t body_end_idx = tok_idx(is_close);
					{
						Token *stmt_end = skip_one_stmt_impl(body_start_is, skip_cache);
						if (stmt_end)
							body_end_idx = tok_idx(stmt_end);
						/* C23 §6.8.4.1: if-init scope extends through
						 * the else branch.  skip_one_stmt on '{' only
						 * matches to '}' — peek ahead for 'else'. */
						if (stmt_end && (tok->tag & TT_IF)) {
							Token *n = skip_prep_dirs(tok_next(stmt_end));
							if (n && (n->tag & TT_IF) && n->ch0 == 'e') {
								Token *else_end = skip_one_stmt_impl(tok_next(n), skip_cache);
								if (else_end)
									body_end_idx = tok_idx(else_end);
							}
						}
					}
					p1_scan_init_shadows(is_open, is_init_end, body_end_idx, cur_sid, brace_depth,
							     body_sid, body_end_idx);
				}
			}
		}

		if (p1d_cur_func >= 0) {
			uint16_t cur_sid = CUR_SID();

			// Label detection: ident ':' at statement start
			// (not '::' scope, not case/default, not inside initializer braces)
			// at_stmt_start filters out _Generic associations, bitfields, and
			// ternary colons — all of which appear mid-statement.
			if (at_stmt_start && is_identifier_like(tok) &&
			    !(tok->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE))) {
				Token *colon = skip_noise(tok_next(tok));
				if (colon && match_ch(colon, ':') &&
				    !(tok_next(colon) && match_ch(tok_next(colon), ':')) &&
				    !(tok->tag & (TT_CASE | TT_DEFAULT)) &&
				    p1d_init_brace_depth == 0) {
					P1FuncEntry *e = p1_alloc(P1K_LABEL, cur_sid, tok);
					e->label.name = tok_loc(tok);
					e->label.len = tok->len;
					// Advance past 'label:' so the next token is at stmt start.
					p1d_prev = colon;
					tok = tok_next(colon);
					at_stmt_start = true;
					continue;
				}
			}

			// Goto detection
			p1d_record_goto(tok, cur_sid, p1d_cur_func);

			// Defer detection
			if (is_defer_kw(tok, p1d_prev))
				p1_try_alloc_defer(tok, cur_sid, p1d_cur_func);

			// Case/default label detection
			if (tok->tag & (TT_CASE | TT_DEFAULT)) {
				uint16_t sw_sid = p1d_switch_top > 0 ?
					p1d_switch_stack[p1d_switch_top - 1] : 0;
				P1FuncEntry *e = p1_alloc(P1K_CASE, cur_sid, tok);
				e->kase.switch_scope_id = sw_sid;
				// Advance past 'case N:' or 'default:' so next token is at stmt start.
				Token *ct = tok_next(tok);
				int td = 0;
				while (ct && ct->kind != TK_EOF) {
					if (match_ch(ct, ';') || match_ch(ct, '{')) break;
					if (ct->flags & TF_OPEN && tok_match(ct))
						{ ct = tok_next(tok_match(ct)); continue; }
					if (match_ch(ct, '?')) { td++; ct = tok_next(ct); continue; }
					if (match_ch(ct, ':')) { if (td > 0) { td--; ct = tok_next(ct); continue; } break; }
					ct = tok_next(ct);
				}
				if (ct && match_ch(ct, ':')) {
					p1d_prev = ct;
					tok = tok_next(ct);
					P1D_STMT_RESET();
					continue;
				}
			}
		}

		if (is_defer_kw(tok, p1d_prev))
			p1d_validate_defer(tok, p1d_cur_func, p1d_ctrl_pending, CUR_SID());

		p1d_probe_declaration(tok, CUR_SID(), brace_depth, p1d_cur_func,
				      &p1d_saw_raw, p1d_saw_static, p1d_ctrl_pending, skip_cache);

		// Phase 1D: detect braceless switch — emit P1K_SWITCH with synthetic scope_id
		// For `switch (expr) stmt;` (no braces), Phase 1A never creates a scope,
		// so the normal '{' handler can't emit P1K_SWITCH.  Detect here at stmt_start
		// and use skip_one_stmt to bound the body.
		if (p1d_cur_func >= 0 && (tok->tag & TT_SWITCH) && !is_known_typedef(tok)) {
			Token *p = skip_prep_dirs(tok_next(tok));
			if (p && match_ch(p, '(') && tok_match(p)) {
				Token *body = skip_prep_dirs(tok_next(tok_match(p)));
				if (body && !match_ch(body, '{')) {
					uint32_t synth_sid = p1d_braceless_next_sid++;
					if (synth_sid > UINT16_MAX)
						error_tok(tok, "too many scopes + braceless switches (>65535)");
					p1_alloc(P1K_SWITCH, (uint16_t)synth_sid, tok);
					P1D_SWITCH_ENSURE_CAP();
					p1d_switch_stack[p1d_switch_top] = synth_sid;
					Token *end = skip_one_stmt_impl(body, skip_cache);
					p1d_switch_end[p1d_switch_top] = end ? tok_idx(end) : UINT32_MAX;
					p1d_switch_top++;
				}
			}
		}

		// 'else' and 'do' create statement boundaries without parens
		if (((tok->tag & TT_IF) && tok->ch0 == 'e') ||
		    ((tok->tag & TT_LOOP) && tok->ch0 == 'd')) {
			p1d_prev = tok; tok = tok_next(tok); at_stmt_start = true; p1d_ctrl_pending = true; continue;
		}

		// Phase 1D: validate bare orelse in expression statements.
		// Control-flow keywords (for/while/if/switch/do/goto/break/continue)
		// cannot start a bare orelse — their bodies have separate at_stmt_start
		// positions that will catch any orelse there.  Skipping them avoids
		// O(N) forward scans through deeply nested braceless control flow.
		if (at_stmt_start && FEAT(F_ORELSE) && p1d_cur_func >= 0 && brace_depth > 0 &&
		    !(tok->tag & (TT_IF | TT_LOOP | TT_SWITCH | TT_GOTO | TT_BREAK |
				  TT_CONTINUE | TT_CASE | TT_DEFAULT | TT_DEFER))) {
			Token *bare_oe = find_bare_orelse(tok);
			if (bare_oe && !(tok_ann(bare_oe) & (P1_OE_BRACKET | P1_OE_DECL_INIT)))
				p1d_validate_bare_orelse(tok, bare_oe);
		}

		at_stmt_start = false;
		p1d_prev = tok;
		tok = tok_next(tok);
	}
}

// Phase 2A: Verify goto→label and switch→case pairs against defers/decls.
// O(N) snapshot-and-sweep: one linear pass per function, no nested O(N) scans.
// Runs before Pass 2 — all CFG errors raised before any byte is emitted.

// Report a goto-skips-defer/decl error or warning.
static void cfg_report_goto(Token *bad, const char *msg, P1FuncEntry *label) {
	if (FEAT(F_WARN_SAFETY))
		warn_tok(bad, msg, label->label.len, label->label.name);
	else
		error_tok(bad, msg, label->label.len, label->label.name);
}

// Check defers/decls between watermarks [lo, hi) for goto→label violations.
static inline uint32_t decl_effective_close(const P1FuncEntry *d) {
	if (d->decl.body_close_idx > 0) return d->decl.body_close_idx;
	if (d->scope_id > 0 && d->scope_id < scope_tree_count)
		return scope_tree[d->scope_id].close_tok_idx;
	return 0;
}

// defer_list/decl_list are monotonic arrays of entry indices into ents[].
// For forward gotos: entries between goto and label that are in-scope at label.
// For backward gotos: entries before the label that are in ancestor-or-self
// scope of the label but NOT of the goto (goto re-enters from outside).
static void cfg_check_range(P1FuncEntry *ents,
			    P1FuncEntry *g, P1FuncEntry *label, bool is_forward,
			    int *defer_list, int defer_lo, int defer_hi,
			    int *decl_list, int decl_lo, int decl_hi) {
	Token *bad_defer = NULL, *bad_decl = NULL;
	bool bad_decl_is_vla = false;

	if (FEAT(F_DEFER)) {
		for (int di = defer_lo; di < defer_hi; di++) {
			P1FuncEntry *d = &ents[defer_list[di]];
			// Scope must be an ancestor-or-self of the label's scope
			if (!scope_is_ancestor_or_self(d->scope_id, label->scope_id)) continue;
			// For backward goto: skip defers in scopes the goto is already inside
			if (!is_forward && scope_is_ancestor_or_self(d->scope_id, g->scope_id)) continue;
			// Defer's scope must still be open at the label position.
			// (For forward: label is the destination; for backward: the label
			// is inside the scope the goto jumps into.)
			if (d->scope_id > 0 && d->scope_id < scope_tree_count) {
				uint32_t close = scope_tree[d->scope_id].close_tok_idx;
				if (close < label->token_index) continue;
			}
			bad_defer = d->tok;
			break;
		}
	}

	// VLA skip is always a hard error (C99/C11 6.8.6.1p1) regardless of
	// feature flags.  Non-VLA declaration bypass only matters with zeroinit.
	for (int di = decl_lo; di < decl_hi; di++) {
		P1FuncEntry *d = &ents[decl_list[di]];
		if (!scope_is_ancestor_or_self(d->scope_id, label->scope_id)) continue;
		if (!is_forward && scope_is_ancestor_or_self(d->scope_id, g->scope_id)) continue;
		{ uint32_t close = decl_effective_close(d); if (close > 0 && close < label->token_index) continue; }
		if (d->decl.is_vla) {
			bad_decl = d->tok;
			bad_decl_is_vla = true;
			break;
		}
		if (FEAT(F_ZEROINIT) && !d->decl.has_raw && !d->decl.is_static_storage) {
			if (!bad_decl) {
				bad_decl = d->tok;
				bad_decl_is_vla = false;
			}
		}
	}

	if (bad_defer)
		cfg_report_goto(bad_defer,
				"goto '%.*s' would skip over this defer statement", label);
	if (bad_decl) {
		const char *msg = bad_decl_is_vla
			? "goto '%.*s' would skip over this VLA declaration"
			: "goto '%.*s' would skip over this variable declaration "
			  "(bypasses initialization)";
		if (bad_decl_is_vla)
			error_tok(bad_decl, msg, label->label.len, label->label.name);
		else
			cfg_report_goto(bad_decl, msg, label);
	}
}

static void p1_verify_cfg(void) {

	for (int fi = 0; fi < func_meta_count; fi++) {
		FuncMeta *fm = &func_meta[fi];
		if (fm->entry_count == 0) continue;

		// Fast path: if neither defer nor zeroinit is enabled, we only
		// need CFG verification for VLA declarations.  Scan entries
		// to check if any VLA exists; if not, skip this function.
		if (!FEAT(F_DEFER | F_ZEROINIT)) {
			P1FuncEntry *ents = &p1_entries[fm->entry_start];
			bool has_vla = false;
			for (int i = 0; i < fm->entry_count; i++)
				if (ents[i].kind == P1K_DECL && ents[i].decl.is_vla) { has_vla = true; break; }
			if (!has_vla) continue;
		}

		// Computed gotos cannot be verified statically: they could jump
		// into any label, bypassing defers or zeroinit.  If the function
		// contains both a computed goto and any defers or zeroinit-tracked
		// declarations, reject it up front.
		// asm goto has the same problem: jump targets are inside the
		// assembly string and cannot be extracted by the token walker.
		bool unverifiable_jump = fm->has_computed_goto ||
			(fm->body_open->tag & TT_ASM);
		if (unverifiable_jump) {
			const char *jump_kind = fm->has_computed_goto
				? "computed goto" : "asm goto";
			P1FuncEntry *ents = &p1_entries[fm->entry_start];
			for (int i = 0; i < fm->entry_count; i++) {
				if (ents[i].kind == P1K_DEFER && FEAT(F_DEFER))
					error_tok(fm->body_open, "%s cannot be used in a "
						  "function that contains defer statements — the "
						  "jump target cannot be verified at compile time",
						  jump_kind);
				if (ents[i].kind == P1K_DECL && ents[i].decl.is_vla)
					error_tok(fm->body_open, "%s cannot be used in a "
						  "function that contains variable-length arrays — the "
						  "jump target cannot be verified at compile time",
						  jump_kind);
				if (ents[i].kind == P1K_DECL && FEAT(F_ZEROINIT) &&
				    !ents[i].decl.has_raw && !ents[i].decl.is_static_storage &&
				    !ents[i].decl.is_vla)
					error_tok(fm->body_open, "%s cannot be used in a "
						  "function that contains zero-initialized declarations — the "
						  "jump target cannot be verified at compile time",
						  jump_kind);
			}
		}

		// Check for defer in function with unresolvable return type.
		// Anonymous structs cannot be spelled in a separate declaration,
		// so Prism cannot generate the temp variable for defer cleanup.
		if (FEAT(F_DEFER) && !fm->returns_void && !fm->ret_type_start) {
			P1FuncEntry *ents = &p1_entries[fm->entry_start];
			for (int i = 0; i < fm->entry_count; i++) {
				if (ents[i].kind == P1K_DEFER)
					error_tok(fm->body_open,
						  "defer in function with unresolvable return type; "
						  "use a named struct or typedef");
			}
		}

		// Label hash: open-addressing, power-of-2 size, maps name → entry index.
		// Allocated before arena mark so it persists in FuncMeta for Pass 2 O(1) lookup.
		int cnt = fm->entry_count;
		if (cnt < 0) cnt = 0; // GCC VRP guard: entry_count is always ≥ 0
		int hash_sz = 64;
		while (hash_sz < cnt * 2) hash_sz <<= 1;
		int *label_hash = arena_alloc(&ctx->main_arena, (size_t)hash_sz * sizeof(int));
		memset(label_hash, 0xFF, (size_t)hash_sz * sizeof(int)); // -1 = empty
		int hash_mask = hash_sz - 1;

		ArenaMark mark = arena_mark(&ctx->main_arena);

		P1FuncEntry *ents = &p1_entries[fm->entry_start];

		// Arena-allocate per-function temporary arrays (reclaimed at loop end).
		int *defer_list = arena_alloc(&ctx->main_arena, (size_t)cnt * sizeof(int));
		int *decl_list  = arena_alloc(&ctx->main_arena, (size_t)cnt * sizeof(int));
		int *wm_defer   = arena_alloc(&ctx->main_arena, (size_t)cnt * sizeof(int));
		int *wm_decl    = arena_alloc(&ctx->main_arena, (size_t)cnt * sizeof(int));
		int defer_n = 0, decl_n = 0;

		// Build label hash in one pass
		for (int i = 0; i < cnt; i++) {
			if (ents[i].kind != P1K_LABEL) continue;
			uint32_t h = (uint32_t)fast_hash(ents[i].label.name, ents[i].label.len);
			for (int probe = 0; probe < hash_sz; probe++) {
				int slot = (h + probe) & hash_mask;
				if (label_hash[slot] < 0) { label_hash[slot] = i; break; }
				P1FuncEntry *existing = &ents[label_hash[slot]];
				if (existing->label.len == ents[i].label.len &&
				    !memcmp(existing->label.name, ents[i].label.name,
					    existing->label.len))
					error_tok(ents[i].tok, "duplicate label '%.*s'",
						  ents[i].label.len, ents[i].label.name);
			}
		}

		// Persist label hash in FuncMeta for O(1) lookup in Pass 2
		fm->label_hash = label_hash;
		fm->label_hash_mask = hash_mask;

		// Pending forward gotos: (goto_entry_idx, defer_mark, decl_mark)
		// Linked via 'next' for O(1) hash-based lookup when label is found.
		typedef struct { int idx, dm, cm, next; } FwdGoto;
		FwdGoto *fwd = arena_alloc(&ctx->main_arena, (size_t)cnt * sizeof(FwdGoto));
		int fwd_n = 0;
		// Forward-goto hash: maps target label name → head of FwdGoto chain.
		// Uses separate chaining through fwd[].next. Same size as label_hash.
		int *fwd_hash_tbl = arena_alloc(&ctx->main_arena, (size_t)hash_sz * sizeof(int));
		memset(fwd_hash_tbl, 0xFF, (size_t)hash_sz * sizeof(int)); // -1 = empty

		// Switch watermark snapshots: (switch_entry_idx, defer_mark, decl_mark)
		// Indexed by scope_id for O(1) lookup from case entries.
		int sw_max_sid = 0;
		for (int i = 0; i < cnt; i++)
			if (ents[i].kind == P1K_SWITCH && ents[i].scope_id > sw_max_sid)
				sw_max_sid = ents[i].scope_id;
		int sw_sz = sw_max_sid + 1;
		int *sw_defer_wm = NULL, *sw_decl_wm = NULL;
		if (sw_sz > 0 && sw_sz <= 65536) {
			sw_defer_wm = arena_alloc(&ctx->main_arena, (size_t)sw_sz * sizeof(int));
			sw_decl_wm  = arena_alloc(&ctx->main_arena, (size_t)sw_sz * sizeof(int));
		}

		for (int i = 0; i < cnt; i++) {
			wm_defer[i] = defer_n;
			wm_decl[i] = decl_n;

			switch (ents[i].kind) {
			case P1K_DEFER:
				defer_list[defer_n++] = i;
				break;

			case P1K_DECL:
				decl_list[decl_n++] = i;
				break;

			case P1K_LABEL: {
				// Resolve pending forward gotos targeting this label via hash.
				uint32_t lh = (uint32_t)fast_hash(ents[i].label.name, ents[i].label.len);
				int fh_slot = lh & hash_mask;
				int prev_fi = -1;
				int fi = fwd_hash_tbl[fh_slot];
				while (fi >= 0) {
					int next_fi = fwd[fi].next;
					P1FuncEntry *g = &ents[fwd[fi].idx];
					if (g->label.len == ents[i].label.len &&
					    !memcmp(g->label.name, ents[i].label.name, g->label.len)) {
						uint16_t label_se = scope_stmt_expr_ancestor(ents[i].scope_id);
						if (label_se != 0 && !scope_is_ancestor_or_self(label_se, g->scope_id))
							error_tok(g->tok, "goto '%.*s' jumps into a statement expression "
								  "(jumping into ({...}) is undefined behavior)",
								  g->label.len, g->label.name);
						// Pre-compute scope exits for Pass 2 defer cleanup
						g->label.exits = scope_block_exits(g->scope_id, ents[i].scope_id);
						cfg_check_range(ents, g, &ents[i], /*is_forward=*/true,
								defer_list, fwd[fi].dm, defer_n,
								decl_list, fwd[fi].cm, decl_n);
						// Remove from chain
						if (prev_fi < 0) fwd_hash_tbl[fh_slot] = next_fi;
						else fwd[prev_fi].next = next_fi;
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
				// O(1) label lookup via hash
				uint32_t h = (uint32_t)fast_hash(g->label.name, g->label.len);
				int li = -1;
				for (int probe = 0; probe < hash_sz; probe++) {
					int slot = (h + probe) & hash_mask;
					if (label_hash[slot] < 0) break;
					P1FuncEntry *cand = &ents[label_hash[slot]];
					if (cand->label.len == g->label.len &&
					    !memcmp(cand->label.name, g->label.name, g->label.len)) {
						li = label_hash[slot];
						break;
					}
				}

				if (li < 0 || li > i) {
					// Forward goto (label not yet seen, or found after current pos)
					int fi = fwd_n++;
					int fh_slot = h & hash_mask;
					fwd[fi] = (FwdGoto){.idx = i, .dm = defer_n, .cm = decl_n,
							    .next = fwd_hash_tbl[fh_slot]};
					fwd_hash_tbl[fh_slot] = fi;
				} else {
					// Backward goto: check defers/decls BEFORE the label
					// in scopes containing the label but not the goto.
					// The goto jumps backward into those scopes, bypassing
					// the defer/decl without passing through it.
					uint16_t label_se = scope_stmt_expr_ancestor(ents[li].scope_id);
					if (label_se != 0 && !scope_is_ancestor_or_self(label_se, g->scope_id))
						error_tok(g->tok, "goto '%.*s' jumps into a statement expression "
							  "(jumping into ({...}) is undefined behavior)",
							  g->label.len, g->label.name);
					// Pre-compute scope exits for Pass 2 defer cleanup
					g->label.exits = scope_block_exits(g->scope_id, ents[li].scope_id);
					cfg_check_range(ents, g, &ents[li], /*is_forward=*/false,
							defer_list, 0, wm_defer[li],
							decl_list, 0, wm_decl[li]);
				}
				break;
			}

			case P1K_SWITCH:
				// Snapshot watermarks keyed by switch's scope_id
				if (sw_defer_wm && ents[i].scope_id < sw_sz) {
					sw_defer_wm[ents[i].scope_id] = defer_n;
					sw_decl_wm[ents[i].scope_id] = decl_n;
				}
				break;

			case P1K_CASE: {
				// Verify case/default against its parent switch's snapshot.
				// Defer-fallthrough: any defer active now that wasn't at switch entry,
				// in ancestor-or-self scope of the case → error.
				uint16_t sw_sid = ents[i].kase.switch_scope_id;

				// Reject case/default jumping into a statement expression.
				// Mirrors the scope_stmt_expr_ancestor check in P1K_GOTO.
				{
					uint16_t case_se = scope_stmt_expr_ancestor(ents[i].scope_id);
					if (case_se != 0 && sw_sid < scope_tree_count && !scope_is_ancestor_or_self(case_se, sw_sid))
						error_tok(ents[i].tok,
							  "case/default label inside a statement expression "
							  "(jumping into ({...}) is undefined behavior)");
				}

				if (!sw_defer_wm || sw_sid >= sw_sz) break;
				int sw_dm = sw_defer_wm[sw_sid];
				int sw_cm = sw_decl_wm[sw_sid];

				if (FEAT(F_DEFER)) {
					for (int di = sw_dm; di < defer_n; di++) {
						P1FuncEntry *d = &ents[defer_list[di]];
						if (!scope_is_ancestor_or_self(d->scope_id, ents[i].scope_id)) continue;
						if (d->scope_id > 0 && d->scope_id < scope_tree_count &&
						    scope_tree[d->scope_id].close_tok_idx < ents[i].token_index)
							continue;
						error_tok(d->tok,
							  "defer skipped by switch fallthrough at %s:%d",
							  tok_file(ents[i].tok)->name,
							  tok_line_no(ents[i].tok));
					}
				}

				// Decl bypass: VLA skip is always fatal (C99/C11 6.8.6.1);
				// non-VLA declaration bypass only with zeroinit.
				for (int di = sw_cm; di < decl_n; di++) {
					P1FuncEntry *d = &ents[decl_list[di]];
					if (!scope_is_ancestor_or_self(d->scope_id, ents[i].scope_id)) continue;
					{ uint32_t close = decl_effective_close(d); if (close > 0 && close < ents[i].token_index) continue; }
					if (d->decl.is_vla)
						error_tok(ents[i].tok,
							  "case/default label may bypass VLA declaration");
					if (!FEAT(F_ZEROINIT)) continue;
					if (d->decl.has_raw || d->decl.is_static_storage) continue;
					error_tok(ents[i].tok,
						  "case/default label inside a nested block within a switch "
						  "may bypass zero-initialization (move the label to the "
						  "switch body or wrap in its own block)");
				}
				break;
			}
			} // switch
		} // sweep

		// Assert all forward gotos resolved — an unresolved goto means
		// the target label was never found in this function's P1D array.
		{
			for (int s = 0; s <= hash_mask; s++) {
				int fi = fwd_hash_tbl[s];
				if (fi >= 0) {
					error_tok(ents[fwd[fi].idx].tok, "goto target label '%.*s' not found in scope",
						  ents[fwd[fi].idx].label.len, ents[fwd[fi].idx].label.name);
					break;
				}
			}
		}

		arena_restore(&ctx->main_arena, mark);
	} // per-function
}

// Walk all tokens once and bake P1_IS_TYPEDEF into tok->ann.
// Called after all typedef registrations are complete (end of Pass 1).
// Turns Pass 2's is_known_typedef() from O(K) hash-and-walk into O(1) bit test.
static void p1_annotate_typedefs(void) {
	for (uint32_t i = 1; i < token_count; i++) {
		Token *t = &token_pool[i];
		if (!is_identifier_like(t)) continue;
		TypedefEntry *e = typedef_lookup(t);
		if (!e) continue;
		tok_ann(t) |= P1_HAS_ENTRY;
		if (!e->is_enum_const && !e->is_shadow && !e->is_vla_var)
			tok_ann(t) |= P1_IS_TYPEDEF;
	}
	p1_typedef_annotated = true;
}

// --- Pass 2: Main Transpilation Loop ---

// Core transpile: emit transformed tokens to an already-opened FILE*
static int transpile_tokens(Token *tok, FILE *fp) {
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

	// Phase 1A: Build scope tree (full-depth walk of all tokens)
	p1_build_scope_tree(tok);

	// Phase 1B: Full-depth typedef + enum registration (all scopes)
	// Also runs Phase 1C (shadow), 1D (labels/gotos/defers/decls),
	// 1E (return type), 1F (defer validation), 1G (orelse pre-classification)
	p1_full_depth_prescan(tok);

	// Phase 2A: verify goto→label pairs against defers/decls
	p1_verify_cfg();

	// Phase 3A: typedef table is now immutable — bake O(1) typedef bits.
	p1_annotate_typedefs();

	if (!FEAT(F_FLATTEN)) {
		collect_system_includes();
		emit_system_includes();
	}

	int next_func_idx = 0;
	int ternary_depth = 0;
	Token *pending_unreachable_tok = NULL;

	while (tok->kind != TK_EOF) {
		// Non-flatten mode: skip system header tokens entirely.
		// emit_tok() suppresses their output, but transformation handlers
		// (try_zero_init_decl, orelse, etc.) use OUT_LIT which is not
		// suppressed.  Skip before any processing to also avoid
		// block_depth / scope_stack pollution from system header braces.
		if (!FEAT(F_FLATTEN)) {
			File *f = tok_file(tok);
			if (f->is_system && f->is_include_entry) {
				if (next_func_idx < func_meta_count &&
				    func_meta[next_func_idx].body_open == tok)
					next_func_idx++;
				tok = tok_next(tok);
				continue;
			}
		}

		Token *next;
		uint32_t tag = tok->tag;

		if (tok->len == 1 && tok->ch0 == '?')
			ternary_depth++;

#define DISPATCH(handler)                                                                                    \
	{                                                                                                    \
		next = handler(tok);                                                                         \
		if (next) {                                                                                  \
			tok = next;                                                                          \
			continue;                                                                            \
		}                                                                                            \
	}

		// Fast path: untagged tokens not at statement start (~70-80% of tokens)
		// If TF_RAW follows a comma (multi-declarator: int x, raw y;), fall
		// through to the slow path for stripping.  This doesn't match
		// expressions like 'raw * 2' because those never follow a comma.
		if (__builtin_expect(!tag && !ctx->at_stmt_start, 1)) {
			if (__builtin_expect((tok->flags & TF_RAW) && !is_known_typedef(tok) &&
					     last_emitted && match_ch(last_emitted, ','), 0))
				goto slow_path;
			track_common_token_state(tok);
			emit_tok(tok); tok = tok_next(tok);
			continue;
		}
		slow_path:

		// Slow path: statement-start processing and tagged tokens

		// Zero-init declarations at statement start.
		// Skip structural tokens to avoid scanning brace blocks (RSS growth on musl ARM64).
		if (ctx->at_stmt_start && !(tag & TT_STRUCTURAL) &&
		    (!ctrl_state.pending || in_for_init() || ctrl_state.parens_just_closed)) {

			next = try_zero_init_decl(tok);
			if (next) {
				tok = next;
				ctx->at_stmt_start = true;
				continue;
			}

			// Enum definitions and typedef declarations bypass
			// process_declarators — check their names against active defers.
			check_enum_typedef_defer_shadow(tok);

			// Bare expression orelse.
			// Skip keywords that introduce sub-statements or labels
			// to avoid sweeping them into the if(!(...)) condition.
			// Also skip storage class specifiers and typedef — those are
			// declarations, not bare expressions.
			if (FEAT(F_ORELSE) && ctx->block_depth > 0 && !in_struct_body() &&
			    !(tok->tag & (TT_NON_EXPR_STMT | TT_DEFER))) {
				// Skip past label if current token is ident followed by ':'
				Token *orelse_scan_start = tok;
				Token *label_end = NULL;
				if (is_identifier_like(tok) && tok_next(tok) && match_ch(tok_next(tok), ':')) {
					label_end = tok_next(tok_next(tok));
					orelse_scan_start = label_end;
				}
				Token *orelse_tok = find_bare_orelse(orelse_scan_start);
				if (orelse_tok) {
					reject_orelse_in_for_init(tok);
					// Emit label before orelse processing
					if (label_end) {
						emit_tok(tok);       // label ident
						emit_tok(tok_next(tok)); // ':'
						tok = label_end;
					}

					// Braceless control flow: bare orelse can emit multiple
					// statements (comma split + assignment), so needs braces.
					// Non-bare (control-flow/block action) already wraps in
					// { if (!(...)) ... } which is a single compound statement.
					bool brace_wrap = ctrl_state.pending && ctrl_state.parens_just_closed;

					// Try bare-fallback path (handled by shared impl)
					Token *next = emit_bare_orelse_impl(tok, NULL, true, brace_wrap);
					if (next) {
						tok = next;
						end_statement_after_semicolon();
						continue;
					}

					// Non-bare fallback (control flow / block): wrap in braces
					tok = emit_orelse_condition_wrap(tok, orelse_tok);

					require_orelse_action(tok, NULL);

					tok = emit_orelse_action(tok, NULL, false, false, NULL);
					OUT_LIT(" }");
					continue;
				}
			}
		}
		ctx->at_stmt_start = false;

		if ((tag & TT_NORETURN_FN) &&
		    !(tok_idx(tok) >= 1 && (token_pool[tok_idx(tok) - 1].tag & TT_MEMBER))) {
			if (FEAT(F_DEFER) && has_active_defers())
				fprintf(stderr,
					"%s:%d: warning: '%.*s' referenced with active defers (defers "
					"will not run if called)\n",
					tok_file(tok)->name,
					tok_line_no(tok),
					tok->len,
					tok_loc(tok));
			if (FEAT(F_AUTO_UNREACHABLE) && ctx->block_depth > 0 &&
			    !in_ctrl_paren() &&
			    !(ctrl_state.pending && ctrl_state.parens_just_closed)) {
				Token *nr = try_detect_noreturn_call(tok);
				if (nr) pending_unreachable_tok = nr;
			}
		}

		// Tag-dependent dispatch
		if (tag) {
			// Keyword dispatch

			if (__builtin_expect(tag & TT_DEFER, 0) && !in_generic())
				DISPATCH(handle_defer_keyword);
			if (__builtin_expect(FEAT(F_DEFER) && (tag & (TT_RETURN | TT_BREAK | TT_CONTINUE)), 0))
				DISPATCH(handle_control_exit_defer);
			if (__builtin_expect((tag & TT_GOTO) && FEAT(F_DEFER | F_ZEROINIT), 0))
				DISPATCH(handle_goto_keyword);

			// Control-flow flag setting

			if (tag & TT_LOOP) {
				if (FEAT(F_DEFER)) {
					ctrl_state.pending = true;
				if (tok->ch0 == 'd') ctrl_state.parens_just_closed = true;
				}
				if (tok->ch0 == 'f' && FEAT(F_DEFER | F_ZEROINIT)) {
					ctrl_state.pending = true;
					ctrl_state.pending_for_paren = true;
				}
			}

			if ((tag & TT_GENERIC) && !in_generic()) {
				{ Token *after = try_generic_member_rewrite(tok); if (after) { tok = after; continue; } }
				if (last_emitted && (match_ch(last_emitted, '*') || match_ch(last_emitted, ')') ||
				    (last_emitted->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_SKIP_DECL | TT_ATTR |
				     TT_INLINE | TT_STORAGE | TT_TYPEOF | TT_BITINT)) ||
				    is_known_typedef(last_emitted))) {
					Token *name = NULL;
					Token *params_open = NULL;
					Token *params_close = NULL;
					Token *after = NULL;
					if (generic_decl_rewrite_target(tok, &name, &params_open, &params_close,
									&after)) {
						out_char('(');
						OUT_TOK(name);
						out_char(')');
						emit_range(params_open, tok_next(params_close));
						// Emit any __attribute__ / [[...]] between construct end and 'after'.
						// For Pattern 2 (params outside _Generic), skip past the already-emitted params.
						Token *gen_close = tok_match(tok_next(tok));
						Token *scan_start = gen_close;
						Token *after_gen = skip_noise(tok_next(gen_close));
						if (after_gen == params_open) scan_start = params_close;
						for (Token *a = tok_next(scan_start); a && a != after; a = tok_next(a)) {
							emit_tok(a);
							last_emitted = a;
						}
						tok = after;
						continue;
					}
				}
				emit_tok(tok);
				last_emitted = tok;
				tok = tok_next(tok);
				if (tok && match_ch(tok, '(')) {
					scope_push_kind(SCOPE_GENERIC);
					emit_tok(tok);
					last_emitted = tok;
					tok = tok_next(tok);
				}
				continue;
			}

			if (FEAT(F_DEFER) && (tag & TT_SWITCH)) ctrl_state.pending = true;

			// C23 if/switch initializers: if(int x; cond) / switch(int x; x)
			// Treat the open paren like for() so at_stmt_start is set and
			// zero-init / typedef tracking fires on the initializer decl.
			if ((tag & TT_SWITCH) && FEAT(F_DEFER | F_ZEROINIT)) {
				ctrl_state.pending = true;
				ctrl_state.pending_for_paren = true;
			}

			if (tag & TT_IF) {
				ctrl_state.pending = true;
				if (tok->ch0 == 'e') {
					ctrl_state.parens_just_closed = true;
					ctx->at_stmt_start = true;
				} else if (FEAT(F_DEFER | F_ZEROINIT)) ctrl_state.pending_for_paren = true;
			}

		} // end if (tag)

		track_generic_token(tok);

		// Void function detection and return type capture at top level
		// (handled by FuncMeta lookup at '{' time)

		if (tag & TT_SUE) // struct/union/enum body
			DISPATCH(handle_sue_body);

		// Structural punctuation: { } ; :
		if (tag & TT_STRUCTURAL) {
			if (match_ch(tok, '{')) {
				// Function definition detection via FuncMeta lookup
				if (ctx->block_depth == 0) {
					if (FEAT(F_DEFER) && next_func_idx < func_meta_count &&
					    func_meta[next_func_idx].body_open == tok) {
						FuncMeta *fm = &func_meta[next_func_idx];
						current_func_idx = next_func_idx++;
						goto_entry_cursor = 0;
						if (fm->ret_type_start) {
							ctx->func_ret_type_start = fm->ret_type_start;
							ctx->func_ret_type_end = fm->ret_type_end;
							ctx->func_ret_type_suffix_start = fm->ret_type_suffix_start;
							ctx->func_ret_type_suffix_end = fm->ret_type_suffix_end;
						} else clear_func_ret_type();
					} else clear_func_ret_type();
				}
				tok = handle_open_brace(tok);
				continue;
			}
			if (match_ch(tok, '}')) {
				tok = handle_close_brace(tok);
				if (ctx->block_depth == 0)
					current_func_idx = -1;
				continue;
			}
			char c = tok->ch0;
			if (c == ';') {
				if (in_ctrl_paren() || in_for_init()) track_ctrl_semicolon();
				else end_statement_after_semicolon();
				bool is_unreachable_target = (tok == pending_unreachable_tok);
				emit_tok(tok); tok = tok_next(tok);
				if (is_unreachable_target) {
					EMIT_UNREACHABLE();
					pending_unreachable_tok = NULL;
				}
				continue;
			}
			if (c == ':') {
				if (ternary_depth > 0) {
					ternary_depth--;
				} else if (in_generic()) {
				} else if (last_emitted &&
				           (is_identifier_like(last_emitted) || last_emitted->kind == TK_NUM) &&
				           !in_struct_body() && ctx->block_depth > 0) {
					emit_tok(tok); tok = tok_next(tok);
					ctx->at_stmt_start = true;
					continue;
				}
			}
		}

		if (__builtin_expect(tok->kind == TK_PREP_DIR, 0)) {
			emit_tok(tok); tok = tok_next(tok);
			ctx->at_stmt_start = true;
			continue;
		}

		track_common_token_state(tok);

		// Process orelse inside typeof() that was not handled by
		// try_zero_init_decl (e.g. sizeof(typeof(x orelse 0)), casts).
		// Route through walk_balanced_orelse for inline ternary transformation.
		if (__builtin_expect(FEAT(F_ORELSE) &&
				     (tok->tag & TT_TYPEOF) && tok_next(tok) &&
				     match_ch(tok_next(tok), '(') && tok_match(tok_next(tok)), 0)) {
			Token *paren = tok_next(tok);
			Token *pclose = tok_match(paren);
			bool has_oe = false;
			for (Token *s = tok_next(paren); s && s != pclose; s = tok_next(s))
				if (is_orelse_keyword(s)) { has_oe = true; break; }
			if (has_oe) {
				emit_tok(tok);                // typeof keyword
				tok = walk_balanced_orelse(paren); // ( ... )
				continue;
			}
		}

		// Process orelse inside array dimension brackets that were not
		// handled by try_zero_init_decl (struct bodies, function prototypes,
		// or other contexts where the declaration parser bailed out).
		// Phase 1G annotates the opening '[' when it's visible to the prescan;
		// brackets inside balanced-skipped groups (e.g., prototype params) need
		// a runtime fallback scan.
		if (__builtin_expect(FEAT(F_ORELSE) &&
				     match_ch(tok, '[') && (tok->flags & TF_OPEN) && tok_match(tok), 0)) {
			if (bracket_scan_has_orelse(tok)) {
				tok = walk_balanced_orelse(tok);
				continue;
			}
		}

		// Warn on unprocessed 'orelse' in unsupported context.
		// Fires in all contexts: struct/union bodies, enum bodies, file scope,
		// etc.  Valid orelse inside typeof() and bracket dimensions is already
		// consumed by walk_balanced_orelse before reaching this point.
		if (__builtin_expect(FEAT(F_ORELSE) && is_orelse_keyword(tok), 0))
			error_tok(tok,
				  "'orelse' cannot be used here (it must appear at the "
				  "statement level in a declaration or bare expression)");

		// Strip 'raw' keyword where try_zero_init_decl does not run
		// (file scope, struct body, after comma in multi-declarators).
		// The keyword is semantically a no-op in those contexts but must
		// not leak into the C output.
		{ Token *r = try_strip_raw(tok); if (r) { tok = r; continue; } }

		emit_tok(tok); tok = tok_next(tok);
	}

	if (FEAT(F_FLATTEN)) {
		out_char('\n');
		emit_system_header_diag_pop();
	}

	out_close();

	// Free malloc'd source_define strings before arena reset makes
	// the pointer array unreachable, then clear stale pointers.
	free_source_defines();

	tokenizer_teardown(false);
	return 1;
}

static Token *preprocess_and_tokenize(char *input_file) {
	char *pp_buf = preprocess_with_cc(input_file);
	if (!pp_buf) {
		fprintf(stderr, "Preprocessing failed for: %s\n", input_file);
		return NULL;
	}
	Token *tok = tokenize_buffer(input_file, pp_buf);
	if (!tok) {
		fprintf(stderr, "Failed to tokenize preprocessed output\n");
		tokenizer_teardown(false);
	}
	return tok;
}

static int transpile_to_fp(char *input_file, FILE *fp) {
	ensure_keyword_cache();

	Token *tok = preprocess_and_tokenize(input_file);
	if (!tok) { fclose(fp); return 0; }

	return transpile_tokens(tok, fp);
}

static int transpile(char *input_file, char *output_file) {
	FILE *fp = fopen(output_file, "w");
	if (!fp) return 0;
	return transpile_to_fp(input_file, fp);
}

// Transpile a source file and write the result to stdout.
// On Windows, transpiles to a temp file and copies it out (no /dev/stdout).
// On POSIX, transpiles directly to /dev/stdout.
static int transpile_to_stdout(char *input_file) {
#ifdef _WIN32
	char temp[PATH_MAX];
	int fd = make_temp_file_registered(temp, sizeof(temp), NULL, 0, input_file);
	if (fd < 0) return 0;
	FILE *wfp = fdopen(fd, "w");
	if (!wfp) { close(fd); return 0; }
	if (!transpile_to_fp(input_file, wfp)) {
		remove(temp);
		return 0;
	}
	FILE *f = fopen(temp, "r");
	if (f) {
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			fwrite(buf, 1, n, stdout);
		fclose(f);
	}
	remove(temp);
	return 1;
#else
	return transpile(input_file, "/dev/stdout");
#endif
}

// --- Public API ---

// LIBRARY API

PRISM_API void prism_free(PrismResult *r) {
	free(r->output);
	free(r->error_msg);
	r->output = r->error_msg = NULL;
}

PRISM_API void prism_reset(void) {
	typedef_table_reset();

	// Free malloc'd source_define strings before arena reset
	free_source_defines();

	tokenizer_teardown(false);

	ctx->scope_depth = 0;
	ctx->block_depth = 0;

	system_includes_reset();

	if (out_fp) {
		out_flush();
		fclose(out_fp);
		out_fp = NULL;
	}
}

// Free all per-thread resources. Call once per thread when done with Prism.
// After this call, the thread must call prism_ctx_init() again to reuse Prism.
PRISM_API void prism_thread_cleanup(void) {
	if (!ctx) return;

	if (out_fp) { out_flush(); fclose(out_fp); out_fp = NULL; }
	out_buf_pos = 0;

	tokenizer_teardown(true);

	// Free heap-allocated hashmap buckets
	free(typedef_table.name_map.buckets);
	memset(&typedef_table, 0, sizeof(typedef_table));
	free(p1_shadow_map.buckets);
	memset(&p1_shadow_map, 0, sizeof(p1_shadow_map));
	free(p1_func_proto_map.buckets);
	memset(&p1_func_proto_map, 0, sizeof(p1_func_proto_map));

	// Reset all TLS statics so a subsequent prism_ctx_init() starts clean
	system_include_list = NULL;
	system_include_capacity = 0;
	last_emitted = NULL;
	use_linemarkers = false;
	defer_count = 0;
	defer_shadow_count = 0;
	free(scope_stack); scope_stack = NULL; scope_stack_cap = 0;
	free(defer_stack); defer_stack = NULL; defer_stack_cap = 0;
	free(defer_shadows); defer_shadows = NULL; defer_shadow_cap = 0;
	for (int i = 0; i < pp_define_bufs_cap; i++) free(pp_define_bufs[i]);
	free(pp_define_bufs); pp_define_bufs = NULL; pp_define_bufs_cap = 0;
	memset(&ctrl_state, 0, sizeof(ctrl_state));
	free(ctrl_save_stack); ctrl_save_stack = NULL; ctrl_save_cap = 0; ctrl_save_depth = 0;

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
	// updates active_membuf after fclose/fflush.  Swapping leaks the buffer.
	if (out_fp) { fclose(out_fp); out_fp = NULL; }
	if (ctx->active_membuf) { free(ctx->active_membuf); ctx->active_membuf = NULL; }
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
// Transpile already-preprocessed source text (no cc -E)
PRISM_API PrismResult prism_transpile_source(const char *source, const char *filename,
                                             PrismFeatures features) {
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

// --- CLI parsing (available in both lib mode and binary mode) ---

#define CLI_PUSH(arr, cnt, cap, item)                                                                        \
	do {                                                                                                 \
		ENSURE_ARRAY_CAP(arr, (cnt) + 1, cap, 16, __typeof__(*(arr)));                               \
		(arr)[(cnt)++] = (item);                                                                     \
	} while (0)

static inline bool has_ext(const char *f, const char *ext) {
	size_t fl = strlen(f), el = strlen(ext);
	return fl >= el && !strcmp(f + fl - el, ext);
}

static bool str_startswith(const char *s, const char *prefix) {
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

// Does a flag need to consume the next argv element?
// Single-char: blacklist the few standalone flags; default = takes arg.
// Multi-char: whitelist the known split-argument flags.
static bool cc_flag_takes_arg(const char *a) {
	if (a[0] != '-' || !a[1]) return false;
	if (!a[2]) {
		switch (a[1]) {
		case 'c': case 'E': case 'S': case 'v': case 'w': case 's':
		case 'g': case 'H': case 'P': case 'p': case 'r': case 'C':
		case 'h': case 'Q': case 'O': case 'W': case 'M': case 'd':
			return false;
		default:
			return true;
		}
	}
	return !strcmp(a, "-include") || !strcmp(a, "-isystem") ||
	       !strcmp(a, "-idirafter") || !strcmp(a, "-imacros") ||
	       !strcmp(a, "-iquote") || !strcmp(a, "-iprefix") ||
	       !strcmp(a, "-iwithprefix") || !strcmp(a, "-iwithprefixbefore") ||
	       !strcmp(a, "-Xlinker") || !strcmp(a, "-Xpreprocessor") ||
	       !strcmp(a, "-Xassembler") || !strcmp(a, "-target") ||
	       !strcmp(a, "-arch");
}

// Is this a dependency-generation flag that should go to the preprocessor only?
// Returns 0 (not dep), 1 (standalone), or 2 (consumes next arg).
static int dep_flag_kind(const char *a) {
	if (a[1] == 'M') {
		if (!strcmp(a, "-MD") || !strcmp(a, "-MMD") || !strcmp(a, "-MP")) return 1;
		if (!strcmp(a, "-MF") || !strcmp(a, "-MT") || !strcmp(a, "-MQ")) return 2;
	}
	if (a[1] == 'W' && a[2] == 'p' && a[3] == ',') {
		const char *v = a + 4;
		if (strstr(v, "-MD") || strstr(v, "-MMD") || strstr(v, "-MF") ||
		    strstr(v, "-MT") || strstr(v, "-MQ") || strstr(v, "-MP"))
			return 1;
	}
	return 0;
}

// Pure CLI parser — no side effects (no exit/print). Caller handles actions.
static Cli cli_parse(int argc, char **argv) {
	Cli cli = {.features = prism_defaults()};

	for (int i = 1; i < argc; i++) {
		char *a = argv[i];

		// -- Non-flag arguments --
#ifdef _WIN32
		if (a[0] != '-' && a[0] != '/') {
#else
		if (a[0] != '-' || !a[1]) {
#endif
			if (!strcmp(a, "run"))       { cli.mode = CLI_RUN; continue; }
			if (!strcmp(a, "transpile")) { cli.mode = CLI_EMIT; continue; }
			if (!strcmp(a, "install"))   { cli.mode = CLI_INSTALL; continue; }
			if (!cli.passthrough && (has_ext(a, ".c") || has_ext(a, ".i"))) {
				CLI_PUSH(cli.sources, cli.source_count, cli.source_cap, a);
				continue;
			}
			CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
			continue;
		}

#ifdef _WIN32
		// -- MSVC-style flags (start with /) --
		if (a[0] == '/') {
			// /Fe:output or /Fe output — MSVC output name
			if (strncmp(a, "/Fe:", 4) == 0) { cli.output = a + 4; continue; }
			if (strncmp(a, "/Fe", 3) == 0 && a[3]) { cli.output = a + 3; continue; }
			if (strcmp(a, "/Fe") == 0 && i + 1 < argc) { cli.output = argv[++i]; continue; }
			// /Fo:output — MSVC object output
			if (strncmp(a, "/Fo:", 4) == 0) { cli.output = a + 4; cli.compile_only = true; continue; }
			if (strncmp(a, "/Fo", 3) == 0 && a[3]) { cli.output = a + 3; cli.compile_only = true; continue; }
			// /c — compile only (no link)
			if (strcmp(a, "/c") == 0) { cli.compile_only = true; }
			// Forward all /flags to CC
			CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
			continue;
		}
#endif

		// -- Output --
		if (a[1] == 'o' && a[1]) {
			cli.output = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
			continue;
		}

		// -- Prism's own flags (consumed, not forwarded) --
		if (a[1] == '-') {
			if (!strcmp(a, "--help"))               { cli.action = CLI_ACT_HELP; return cli; }
			if (!strcmp(a, "--version"))            { cli.action = CLI_ACT_VERSION; return cli; }
			if (str_startswith(a, "--prism-cc="))   { cli.cc = a + 11; continue; }
			if (!strcmp(a, "--prism-verbose"))       { cli.verbose = true; continue; }
			if (str_startswith(a, "--prism-emit=")) { cli.mode = CLI_EMIT; cli.output = a + 13; continue; }
			if (!strcmp(a, "--prism-emit"))          { cli.mode = CLI_EMIT; continue; }
			// fall through to forward
		} else if (a[1] == 'h' && !a[2]) {
			cli.action = CLI_ACT_HELP; return cli;
		} else if (a[1] == 'c' && !a[2]) {
			cli.compile_only = true;
			// also forward -c to CC
		} else if (a[1] == 'E' && !a[2]) {
			cli.passthrough = true;
			// also forward -E to CC
		} else if (a[1] == 'f') {
			if (!strcmp(a, "-fno-defer"))            { cli.features.defer = false; continue; }
			if (!strcmp(a, "-fno-zeroinit"))         { cli.features.zeroinit = false; continue; }
			if (!strcmp(a, "-fno-orelse"))           { cli.features.orelse = false; continue; }
			if (!strcmp(a, "-fno-line-directives"))  { cli.features.line_directives = false; continue; }
			if (!strcmp(a, "-fno-safety"))           { cli.features.warn_safety = true; continue; }
			if (!strcmp(a, "-fflatten-headers"))     { cli.features.flatten_headers = true; continue; }
			if (!strcmp(a, "-fno-flatten-headers"))  { cli.features.flatten_headers = false; continue; }
			if (!strcmp(a, "-fno-auto-unreachable")) { cli.features.auto_unreachable = false; continue; }
			// fall through to forward
		} else {
			// Dependency-generation flags → preprocessor only
			int dk = dep_flag_kind(a);
			if (dk) {
				CLI_PUSH(cli.dep_args, cli.dep_arg_count, cli.dep_arg_cap, a);
				if (dk == 2 && i + 1 < argc)
					CLI_PUSH(cli.dep_args, cli.dep_arg_count, cli.dep_arg_cap, argv[++i]);
				continue;
			}
		}

		// -- Forward to CC --
		CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
		if (i + 1 < argc && cc_flag_takes_arg(a))
			CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, argv[++i]);
	}

	return cli;
}

static void cli_free(Cli *cli) {
	free(cli->sources);
	free(cli->cc_args);
	free(cli->dep_args);
	cli->sources = NULL;
	cli->cc_args = NULL;
	cli->dep_args = NULL;
}

// --- Build, Install & Entry Point ---

#ifndef PRISM_LIB_MODE

// Transpile and pipe output directly to the compiler (no temp files)
static int transpile_and_compile(char *input_file, char **compile_argv, bool verbose) {
	if (verbose) {
		fprintf(stderr, "[prism] ");
		for (int i = 0; compile_argv[i]; i++) fprintf(stderr, "%s ", compile_argv[i]);
		fprintf(stderr, "\n");
	}

	Token *tok = preprocess_and_tokenize(input_file);
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

	transpile_tokens(tok, fp);

	return wait_for_child(pid);
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
	// Termux: $PREFIX/bin is the user-writable bin directory
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
	// On Windows, `where` finds the CWD first. If the first hit is in the
	// current directory, skip it and check the next result instead.
	char resolved_hit[PATH_MAX], resolved_install[PATH_MAX];
#ifdef _WIN32
	char cwd[PATH_MAX];
	if (first_hit[0]) {
		// Use _wgetcwd to get UTF-8 CWD (not ANSI-mangled).
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
				// First hit is in CWD — try the next line
				read_trimmed_line(first_hit, sizeof(first_hit), fp);
			}
		}
	}
	// popen output from cmd.exe is in the console's OEM codepage (e.g., CP437),
	// not UTF-8.  Convert to UTF-8 so the comparison with install_path works
	// even when the path contains non-ASCII characters.
	if (first_hit[0]) {
		UINT oem_cp = GetConsoleOutputCP();
		if (oem_cp && oem_cp != CP_UTF8) {
			wchar_t wide[PATH_MAX];
			int wlen = MultiByteToWideChar(oem_cp, 0, first_hit, -1, wide, PATH_MAX);
			if (wlen > 0) {
				char utf8[PATH_MAX];
				int ulen = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, PATH_MAX, NULL, NULL);
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
	// ERROR_SHARING_VIOLATION (32): the installed exe is currently running.
	// On Windows you can't overwrite a running exe, but you CAN rename it.
	if (input && !output && GetLastError() == ERROR_SHARING_VIOLATION) {
		snprintf(old_path, sizeof(old_path), "%s.old", install_path);
		remove(old_path); // remove any leftover from a previous update
		if (MoveFileA(install_path, old_path))
			output = fopen(install_path, "wb");
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
		// The .old exe is still locked by the running process, so remove()
		// will silently fail.  Move it to %TEMP% and schedule deletion on
		// next reboot so it doesn't accumulate in the install directory.
		if (old_path[0]) {
			if (!remove(old_path)) {
				old_path[0] = '\0'; // successfully deleted
			} else {
				char temp_old[PATH_MAX];
				const char *tmp = get_tmp_dir();
				if (tmp && *tmp) {
					snprintf(temp_old, sizeof(temp_old), "%sprism_old_%u.exe",
						 tmp, (unsigned)GetCurrentProcessId());
					if (MoveFileExA(old_path, temp_old,
							MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
						MoveFileExA(temp_old, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
						old_path[0] = '\0';
					}
				}
				// Fallback: schedule in-place deletion on reboot.
				if (old_path[0])
					MoveFileExA(old_path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
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
			} else sudo_path[0] = doas_path[0] = '\0';
			if (access("/usr/bin/sudo", X_OK) == 0 || access("/bin/sudo", X_OK) == 0 ||
			    (sudo_path[0] && access(sudo_path, X_OK) == 0))
				escalate = "sudo";
			else if (access("/usr/bin/doas", X_OK) == 0 || access("/bin/doas", X_OK) == 0 ||
				 (doas_path[0] && access(doas_path, X_OK) == 0))
				escalate = "doas";

			if (!escalate) {
				fprintf(stderr, "[prism] Permission denied and neither sudo nor doas found.\n"
						"  Install as root or copy manually:\n"
						"    cp %s %s && chmod +x %s\n", self_path, install_path, install_path);
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

// Check if the system compiler is clang
static int capture_first_line(char **argv, char *buf, size_t bufsize);
static bool cc_is_clang(const char *cc) {
#ifdef __APPLE__
	if (!cc || !*cc || strcmp(cc, "cc") == 0 || strcmp(cc, "gcc") == 0) return true;
#endif
	if (!cc || !*cc) return false;
	const char *exe = cc_executable(cc);
	const char *base = path_basename(exe);
	if (strncmp(base, "clang", 5) == 0) return true;
	// Probe: on many systems "cc" or "gcc" is actually clang behind a symlink.
	char ver[256];
	char *probe_argv[] = {(char *)exe, "--version", NULL};
	if (capture_first_line(probe_argv, ver, sizeof(ver)) == 0) {
		for (char *p = ver; *p; p++) *p = (char)tolower((unsigned char)*p);
		if (strstr(ver, "clang")) return true;
	}
	return false;
}

#ifndef _WIN32
// Capture the first line of a command's stdout. Returns 0 on success.
static int capture_first_line(char **argv, char *buf, size_t bufsize) {
	int pipefd[2];
	if (pipe(pipefd) != 0) return -1;
	char **env = build_clean_environ();
	if (!env) { close(pipefd[0]); close(pipefd[1]); return -1; }
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
	if (err) { close(pipefd[0]); buf[0] = '\0'; return -1; }
	ssize_t n = read(pipefd[0], buf, bufsize - 1);
	close(pipefd[0]);
	waitpid(pid, NULL, 0);
	if (n <= 0) { buf[0] = '\0'; return -1; }
	buf[n] = '\0';
	char *nl = strchr(buf, '\n');
	if (nl) *nl = '\0';
	return 0;
}
#endif

static void print_help(void) {
	printf("Prism v%s - Robust C transpiler\n\n"
	       "Usage: prism [options] source.c... [-o output]\n\n"
	       "Commands:\n"
	       "  run <src.c>           Transpile, compile, and run\n"
	       "  transpile <src.c>     Output transpiled C to stdout\n"
	       "  install [src.c...]    Install prism to %s\n\n"
	       "Prism Flags (consumed, not passed to CC):\n"
	       "  -fno-defer            Disable defer\n"
	       "  -fno-zeroinit         Disable zero-initialization\n"
	       "  -fno-orelse           Disable orelse keyword\n"
	       "  -fno-line-directives  Disable #line directives\n"
	       "  -fno-safety           Safety checks warn instead of error\n"
	       "  -fflatten-headers     Flatten headers into single output\n"
	       "  -fno-flatten-headers  Disable header flattening\n"
	       "  -fno-auto-unreachable Disable __builtin_unreachable after noreturn calls\n"
	       "  --prism-cc=<compiler> Use specific compiler\n"
	       "  --prism-verbose       Show commands\n\n"
	       "All other flags are passed through to CC.\n\n"
	       "Examples:\n"
	       "  prism foo.c -o foo               Compile (GCC-compatible)\n"
	       "  prism run foo.c                  Compile and run\n"
	       "  prism transpile foo.c            Output transpiled C\n"
	       "  prism -O2 -Wall foo.c -o foo     With optimization\n"
	       "  CC=clang prism foo.c             Use clang as backend\n\n"
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
	else args[(*argc)++] = "-Wno-logical-op";
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

// Returns pointer to a thread-local buffer.
static const char *cli_output_path(const Cli *cli, const char *temp_exe, bool msvc) {
	static PRISM_THREAD_LOCAL char defobj[PATH_MAX];

	if (cli->mode == CLI_RUN) return temp_exe;
	if (cli->output) return cli->output;
	if (cli->compile_only && cli->source_count == 1) {
		const char *base = path_basename(cli->sources[0]);
		snprintf(defobj, sizeof(defobj), "%s", base);
		char *dot = strrchr(defobj, '.');
		if (dot) snprintf(dot, sizeof(defobj) - (dot - defobj), "%s", msvc ? ".obj" : ".o");
		return defobj;
	}
	return NULL;
}

static void argv_add_output(const char **args, int *argc, const char *out, bool msvc, bool compile_only) {
	if (!out) return;

	if (msvc) {
		static PRISM_THREAD_LOCAL char flag[PATH_MAX + 8]; // cl.exe: /Fe:exe or /Fo:obj
		if (compile_only) snprintf(flag, sizeof(flag), "/Fo:%s", out);
		else snprintf(flag, sizeof(flag), "/Fe:%s", out);
		args[(*argc)++] = flag;
	} else {
		args[(*argc)++] = "-o";
		args[(*argc)++] = out;
	}
}

static void make_run_temp(char *buf, size_t size, CliMode mode) {
	buf[0] = '\0';
	if (mode != CLI_RUN) return;
	int suffix_len = (int)strlen(EXE_SUFFIX);
	snprintf(buf, size, "%sprism_run.XXXXXX%s", get_tmp_dir(), EXE_SUFFIX);
	int fd = suffix_len > 0 ? mkstemps(buf, suffix_len) : mkstemp(buf);
	if (fd >= 0) close(fd);
	else buf[0] = '\0';
}

static int passthrough_cc(const Cli *cli) {
	const char *compiler = get_real_cc(cli->cc);
	int cc_extra = cc_extra_arg_count(compiler);
	bool msvc = cc_is_msvc(compiler);
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
		// Compute that path and remove it.
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

static int run_temp_compile_plan(const Cli *cli, char **temps, int temp_count,
					 const TempCompilePlan *plan) {
	int cc_extra = cc_extra_arg_count(plan->compiler);
	const char **args = alloc_argv(temp_count + cli->cc_arg_count + cc_extra + 24);
	int argc = 0;
	char *cc_dup = NULL;
	cc_split_into_argv(args, &argc, plan->compiler, &cc_dup);
	if (plan->msvc) args[argc++] = "/nologo";
	// Prism may emit typeof()/typeof_unqual() which require C23 mode on MSVC.
	// Always inject /std:clatest and strip any conflicting /std: from user args
	// to prevent compilation failures (e.g. user passes /std:c11 but generated
	// code uses typeof).
	if (plan->msvc) args[argc++] = "/std:clatest";
	if (plan->optimize) args[argc++] = plan->msvc ? "/O2" : "-O2";
	if (plan->use_preprocessed) args[argc++] = "-fpreprocessed";
	for (int i = 0; i < temp_count; i++) args[argc++] = temps[i];
	if (plan->use_preprocessed) args[argc++] = "-fno-preprocessed";
	for (int i = 0; i < cli->cc_arg_count; i++) {
		// Skip user's /std: flags on MSVC — we already injected /std:clatest.
		if (plan->msvc && strncmp(cli->cc_args[i], "/std:", 5) == 0) continue;
		args[argc++] = cli->cc_args[i];
	}
	if (plan->suppress_warnings)
		add_warn_suppress(args, &argc, plan->clang, plan->msvc);
	argv_add_output(args, &argc, plan->output, plan->msvc, plan->compile_only);
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
			if (fd < 0)
				die("Failed to create temp file");
			PrismResult result = prism_transpile_file(cli->sources[i], cli->features);
			if (result.status != PRISM_OK) {
				fprintf(stderr, "%s:%d:%d: error: %s\n", cli->sources[i],
					result.error_line, result.error_col,
					result.error_msg ? result.error_msg : "transpilation failed");
				prism_free(&result);
				close(fd);
				cleanup_temp_range(temps, i + 1);
				return NULL;
			}
			FILE *f = fdopen(fd, "w");
			if (!f) { prism_free(&result); close(fd); die("Failed to create temp file"); }
			fwrite(result.output, 1, result.output_len, f);
			fclose(f);
			prism_free(&result);
		} else {
			temps[i] = malloc(512);
			if (!temps[i]) die("Out of memory");
			int fd = make_temp_file_registered(temps[i], 512, NULL, 0, cli->sources[i]);
			if (fd < 0)
				die("Failed to create temp file");
			if (cli->verbose)
				fprintf(stderr, "[prism] Transpiling %s -> %s\n", cli->sources[i], temps[i]);
			FILE *wfp = fdopen(fd, "w");
			if (!wfp) { close(fd); die("Failed to open temp file"); }
			if (!transpile_to_fp((char *)cli->sources[i], wfp)) {
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

static int compile_sources(Cli *cli) {
	int status = 0;
	const char *compiler = get_real_cc(cli->cc);
	int cc_extra = cc_extra_arg_count(compiler);
	bool clang = cc_is_clang(compiler);
	bool msvc = cc_is_msvc(compiler);
	char temp_exe[PATH_MAX];
	make_run_temp(temp_exe, sizeof(temp_exe), cli->mode);

	if (temp_exe[0]) {
		signal_temp_store(0);
		memcpy(signal_temp_path, temp_exe, sizeof(signal_temp_path));
		signal_temp_store(1);
	}

	use_linemarkers = FEAT(F_FLATTEN) && !clang && !msvc;

	if (cli->source_count == 1 && !msvc) {
		// Extract user-specified -x <lang> from cc_args (if any) for the pipe language.
		// Without this, -x objective-c ends up after stdin and has no effect.
		const char *pipe_lang = "c";
		int x_flag_idx = -1;
		for (int i = 0; i < cli->cc_arg_count - 1; i++) {
			if (strcmp(cli->cc_args[i], "-x") == 0) {
				pipe_lang = cli->cc_args[i + 1];
				x_flag_idx = i;
				break;
			}
		}

		const char **args = alloc_argv(cli->cc_arg_count + cc_extra + 20);
		int argc = 0;
		char *cc_dup = NULL;
		cc_split_into_argv(args, &argc, compiler, &cc_dup);
		args[argc++] = "-x";
		args[argc++] = pipe_lang;
		if (FEAT(F_FLATTEN) && !clang) args[argc++] = "-fpreprocessed";
		args[argc++] = "-";
		// Emit -x none to reset language before any positional file args
		// (object files, libraries) so they aren't misinterpreted as C source.
		// Skip if cc_args are all flags — clang warns about -x none after
		// the last input file.
		bool need_x_none = false;
		for (int i = 0; i < cli->cc_arg_count; i++) {
			if (i == x_flag_idx) { i++; continue; }
			const char *a = cli->cc_args[i];
			if (a[0] != '-') { need_x_none = true; break; }
			// skip flags whose next arg is a value, not a file
			if (strcmp(a, "-o") == 0 || strcmp(a, "-MF") == 0 ||
			    strcmp(a, "-MQ") == 0 || strcmp(a, "-MT") == 0) { i++; continue; }
		}
		if (need_x_none) {
			args[argc++] = "-x";
			args[argc++] = "none";
		}
		for (int i = 0; i < cli->cc_arg_count; i++) {
			if (i == x_flag_idx) { i++; continue; } // skip extracted -x <lang>
			args[argc++] = cli->cc_args[i];
		}
		add_warn_suppress(args, &argc, clang, false);
		argv_add_output(args, &argc, cli_output_path(cli, temp_exe, false), false, cli->compile_only);
		args[argc] = NULL;

		if (cli->verbose) fprintf(stderr, "[prism] Transpiling %s (pipe → cc)\n", cli->sources[0]);
		status = transpile_and_compile((char *)cli->sources[0], (char **)args, cli->verbose);
		free(cc_dup);
		free((void *)args);
	} else {
		char **temps = transpile_sources_to_temps(cli, false);
		if (!temps) die("Transpilation failed");

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

	if (status != 0) {
		if (temp_exe[0]) remove(temp_exe);
		signal_temp_store(0);
		return status;
	}

	if (cli->mode == CLI_RUN) {
		char *run[] = {temp_exe, NULL};
		if (cli->verbose) fprintf(stderr, "[prism] Running %s\n", temp_exe);
		status = run_command(run);
		remove(temp_exe);
	}

	signal_temp_store(0);
	return status;
}

static void signal_cleanup_handler(int sig) {
	// Note on async-signal-safety: on POSIX, only async-signal-safe
	// functions are used here (unlink, signal, raise).  On Windows,
	// fflush/fclose below are NOT async-signal-safe, but are required
	// because Windows locks open files, preventing unlink.  Windows
	// dispatches Ctrl-C on a separate thread, so the deadlock window
	// is narrow; this is an accepted trade-off.
#ifdef _WIN32
	// On Windows, unlink/_wunlink fails with EACCES if the file is open.
	// Close any open output/memstream files before attempting cleanup.
	// Use the raw CRT fclose to avoid memstream wrapper logic in a signal handler.
	if (out_fp) { fflush(out_fp); win32_real_fclose(out_fp); out_fp = NULL; }
	if (win32_memstream_fp) {
		win32_real_fclose(win32_memstream_fp);
		win32_memstream_fp = NULL;
	}
#endif
	if (signal_temp_load() && signal_temp_path[0])
		unlink(signal_temp_path);
	int n = signal_temps_load();
	for (int i = 0; i < n; i++)
		if (signal_temps_ready_load(i) && signal_temps[i][0] != '\0')
			unlink(signal_temps[i]);
	signal(sig, SIG_DFL);
	raise(sig);
}

int main(int argc, char **argv) {
#ifdef _WIN32
	win32_utf8_argv(&argc, &argv);
#endif
	signal(SIGINT, signal_cleanup_handler);
	signal(SIGTERM, signal_cleanup_handler);
	signal(SIGPIPE, SIG_IGN); // no-op on Windows (SIGPIPE defined but never raised)
	int status = 0;
	prism_ctx_init();

	if (argc < 2) {
		print_help();
		return 0;
	}

	Cli cli = cli_parse(argc, argv);

	// Handle help/version actions (cli_parse sets these without side effects)
	if (cli.action == CLI_ACT_HELP) { print_help(); cli_free(&cli); return 0; }
	if (cli.action == CLI_ACT_VERSION) {
		const char *real_cc = get_real_cc(cli.cc);
		char cc_line[256];
		char *vargs[] = {(char *)real_cc, "--version", NULL};
		if (capture_first_line(vargs, cc_line, sizeof(cc_line)) == 0 && cc_line[0])
			printf("prism %s (%s)\n", PRISM_VERSION, cc_line);
		else
			printf("prism %s\n", PRISM_VERSION);
		cli_free(&cli);
		return 0;
	}

	// Resolve CC (env vars checked here, not in cli_parse, to keep it pure)
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

	ctx->features = features_to_bits(cli.features);
	ctx->extra_compiler = get_real_cc(cli.cc);
	ctx->extra_compiler_flags = cli.cc_args;
	ctx->extra_compiler_flags_count = cli.cc_arg_count;
	ctx->dep_flags = cli.dep_args;
	ctx->dep_flags_count = cli.dep_arg_count;

	if (cli.mode == CLI_INSTALL)
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
			if (!transpile_to_stdout((char *)cli.sources[i]))
				die("Transpilation failed");
		}
	} else if (cli.source_count == 0)
		status = passthrough_cc(&cli);
	else
		status = compile_sources(&cli);

	cli_free(&cli);
	return status;
}

#endif // PRISM_LIB_MODE

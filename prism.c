#define PRISM_VERSION "0.119.0"

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

#include "parse.c"

static int run_command(char **argv);
static int run_command_quiet(char **argv);

#define match_ch _equal_1

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#define INITIAL_ARRAY_CAP 32
#define OUT_BUF_SIZE (128 * 1024)

#define FEAT(f) (ctx->features & (f))
#define is_c23_attr(t) ((t) && ((t)->flags & TF_C23_ATTR))
#define is_raw(t) ((t)->flags & TF_RAW)
#define is_sizeof_like(t) ((t)->flags & TF_SIZEOF)
#define is_enum_kw(t) ((t)->tag & TT_SUE && tok_loc(t)[0] == 'e')

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

typedef struct {
	Token *end;		  // First token after the type specifier
	bool saw_type : 1;	  // True if a type was recognized
	bool is_struct : 1;
	bool is_enum : 1;
	bool is_typedef : 1;
	bool is_vla : 1;
	bool has_typeof : 1;
	bool has_atomic : 1;
	bool has_register : 1;
	bool has_volatile : 1;
	bool has_const : 1;
	bool has_void : 1;	  // True if void or void typedef
	bool has_extern : 1;
} TypeSpecResult;

typedef enum {
	DEFER_SCOPE,	// DEFER_SCOPE=current only
	DEFER_ALL,	// DEFER_ALL=all scopes
	DEFER_BREAK,	// DEFER_BREAK=stop at loop/switch,
	DEFER_CONTINUE, // DEFER_CONTINUE=stop at loop,
	DEFER_TO_DEPTH	// DEFER_TO_DEPTH=stop at given depth (for goto)
} DeferEmitMode;

typedef enum {
	TDK_TYPEDEF,
	TDK_SHADOW,
	TDK_ENUM_CONST,
	TDK_VLA_VAR // VLA variable (not typedef, but actual VLA array variable)
} TypedefKind;

typedef struct {
	Token *stmt, *end, *defer_kw;
} DeferEntry;

typedef enum {
	SCOPE_BLOCK,       // { ... } block scope
	SCOPE_FOR_PAREN,   // for( ... ) — first ';' ends init, not stmt
	SCOPE_CTRL_PAREN,  // if/while/switch( ... )
	SCOPE_GENERIC,     // _Generic( ... )
	SCOPE_TERNARY,     // ? ... : — popped on matching ':'
} ScopeKind;

typedef struct {
	int defer_start_idx;
	uint8_t kind;
	bool is_loop : 1;
	bool is_switch : 1;
	bool had_control_exit : 1; // true if unconditional break/return/goto/continue was seen
				   // NOTE: only set on switch scopes (by mark_switch_control_exit)
	bool is_conditional : 1;
	bool is_struct : 1;
	bool has_zeroinit_decl : 1;
	bool is_stmt_expr : 1;
	bool is_orelse_guard : 1;
} ScopeNode;

typedef struct {
	Token *skipped_defer;
	Token *skipped_decl;
} GotoSkipResult;

typedef struct {
	char *name;
	int name_len;
	int scope_depth;
	Token *tok;
	Token *block_open; // The '{' that opens the label's immediate scope
} LabelInfo;

typedef struct {
	char *name; // Points into token stream (no alloc needed)
	int len;
	int scope_depth;	    // Scope where defined (aligns with ctx->block_depth)
	int prev_index;		    // Index of previous entry with same name (-1 if none), for hash map chaining
	bool is_vla : 1;
	bool is_void : 1;
	bool is_const : 1;
	bool is_ptr : 1;
	bool is_array : 1;
	bool is_shadow : 1;
	bool is_enum_const : 1;
	bool is_vla_var : 1;
	bool is_aggregate : 1;
} TypedefEntry;

typedef struct {
	TypedefEntry *entries;
	int count;
	int capacity;
	HashMap name_map; // Maps name -> index+1 of most recent entry (0 means not found)
} TypedefTable;

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

// Control flow scope flags
enum { NS_LOOP = 1, NS_SWITCH = 2, NS_CONDITIONAL = 4 };

// Declarator parsing result
typedef struct {
	Token *end;		  // First token after declarator
	Token *var_name;
	bool is_pointer : 1;
	bool is_array : 1;
	bool is_vla : 1;
	bool is_func_ptr : 1;
	bool has_paren : 1;
	bool paren_pointer : 1;	  // Has pointer (*) inside parenthesized declarator
	bool paren_array : 1;
	bool has_init : 1;
	bool is_const : 1;
} DeclResult;

static bool struct_body_contains_vla(Token *);
static bool typedef_contains_vla(Token *);

extern char **environ;
static char **cached_clean_env = NULL;
static volatile sig_atomic_t signal_temp_registered = 0;
static char signal_temp_path[PATH_MAX];

// Transpiled temp file tracking for signal cleanup.
// Fixed-size ring; entries are PATH_MAX-sized C strings.
#define SIGNAL_TEMPS_MAX 64
static char signal_temps[SIGNAL_TEMPS_MAX][PATH_MAX];
static volatile sig_atomic_t signal_temps_count = 0;

#if defined(_MSC_VER)
// MSVC volatile has acquire/release semantics on x86/x64
#define signal_temp_store(val) (signal_temp_registered = (val))
#define signal_temp_load()     (signal_temp_registered)
#define signal_temps_store(val) (signal_temps_count = (val))
#define signal_temps_load()     (signal_temps_count)
#else
#define signal_temp_store(val) __atomic_store_n(&signal_temp_registered, (val), __ATOMIC_RELEASE)
#define signal_temp_load()     __atomic_load_n(&signal_temp_registered, __ATOMIC_ACQUIRE)
#define signal_temps_store(val) __atomic_store_n(&signal_temps_count, (val), __ATOMIC_RELEASE)
#define signal_temps_load()     __atomic_load_n(&signal_temps_count, __ATOMIC_ACQUIRE)
#endif

static void signal_temps_register(const char *path) {
	int n = signal_temps_load();
	if (n >= SIGNAL_TEMPS_MAX) return;
	size_t len = strlen(path);
	if (len >= PATH_MAX) return;
	memcpy(signal_temps[n], path, len + 1);
	signal_temps_store(n + 1);
}

static void signal_temps_clear(void) {
	signal_temps_store(0);
}

static char **system_include_list; // Ordered list of includes
static int system_include_capacity = 0;

static LabelInfo *label_table = NULL;
static int label_count = 0;
static int label_capacity = 0;

// Token emission - user-space buffered output for minimal syscall overhead
static FILE *out_fp;
static Token *last_emitted = NULL;

static char out_buf[OUT_BUF_SIZE];
static int out_buf_pos = 0;
static bool use_linemarkers = false; // true = GCC linemarker "# N", false = C99 "#line N"

static TypedefTable typedef_table;

typedef struct {
	uint8_t scope_flags;
	bool pending;
	bool pending_for_paren;
	bool parens_just_closed;
	bool pending_orelse_guard;
	int brace_depth;
} CtrlState;

typedef struct {
	Token *prev_tok;
	Token *last_paren;
	Token *last_open_paren;
	int paren_depth;
} ToplevelState;

static ScopeNode scope_stack[4096];
static DeferEntry defer_stack[2048];
static int defer_count = 0;
static CtrlState ctrl_state;

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

#define MAX_DEFER_SHADOWS 256
static DeferShadow defer_shadows[MAX_DEFER_SHADOWS];
static int defer_shadow_count = 0;

// Forward declarations (only for functions used before their definition)
static DeclResult parse_declarator(Token *tok, bool emit);
static bool is_type_keyword(Token *tok);
static inline bool is_valid_varname(Token *tok);
static void typedef_pop_scope(int scope_depth);
static void defer_shadow_pop_scope(int leaving_block_depth);
static void check_defer_shadow_at_exit(DeferEmitMode mode, int stop_depth);
static TypeSpecResult parse_type_specifier(Token *tok);
static Token *emit_expr_to_semicolon(Token *tok);
static Token *emit_orelse_action(Token *tok, Token *var_name, bool has_const, bool has_volatile, Token *stop_comma);
static Token *emit_return_body(Token *tok, Token *stop);
static Token *try_zero_init_decl(Token *tok);
static Token *decl_noise(Token *tok, bool emit);
static Token *walk_balanced(Token *tok, bool emit);
static Token *walk_balanced_orelse(Token *tok);
#define skip_noise(tok) decl_noise(tok, false)
static inline void out_char(char c);
static inline void out_str(const char *s, int len);
#define skip_balanced(tok, o, c) walk_balanced((tok), false)
static bool cc_is_msvc(const char *cc);
static LabelInfo *label_find(Token *tok);
static inline void ctrl_reset(void);

// Emit space-separated token range [start, end). First token has no leading space.
static inline void emit_token_range(Token *start, Token *end) {
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = tok_next(t)) {
		if (t != start) out_char(' ');
		out_str(tok_loc(t), t->len);
	}
}

static inline void clear_func_ret_type(void) {
	ctx->func_ret_type_start = ctx->func_ret_type_end = NULL;
	ctx->func_ret_type_suffix_start = ctx->func_ret_type_suffix_end = NULL;
}

// Skip trailing declarator parts (attrs, array dims, param lists) until '{' or ';'
static Token *skip_declarator_suffix(Token *tok) {
	while (tok && tok->kind != TK_EOF && !match_ch(tok, '{') && !match_ch(tok, ';')) {
		Token *next = skip_noise(tok);
		if (next != tok) { tok = next; continue; }
		if (tok->flags & TF_OPEN) tok = walk_balanced(tok, false);
		else break;
	}
	return tok;
}

static void reset_transpiler_state(void) {
	ctx->scope_depth = 0;
	ctx->block_depth = 0;
	ctx->last_line_no = 0;
	ctx->ret_counter = 0;
	clear_func_ret_type();
	ctx->last_filename = NULL;
	ctx->last_system_header = false;
	ctx->current_func_returns_void = false;
	ctx->current_func_has_setjmp = false;
	ctx->current_func_has_asm = false;
	ctx->current_func_has_vfork = false;
	ctx->at_stmt_start = true;
	ctrl_reset();
	last_emitted = NULL;

	// Reset arena-allocated arrays to prevent stale pointers after arena_reset.
	// After tokenizer_teardown(false) resets the arena, these pointers become
	// dangling. Without clearing them, ARENA_ENSURE_CAP would skip allocation
	// (thinking capacity is sufficient) and write through dangling pointers,
	// corrupting token data in the reused arena blocks.
	label_table = NULL;
	label_count = 0;
	label_capacity = 0;
	defer_count = 0;
	defer_shadow_count = 0;
	ctx->bracket_oe_ids = NULL;
	ctx->bracket_oe_count = 0;
	ctx->bracket_oe_cap = 0;
	ctx->bracket_oe_next = 0;

}

PRISM_API PrismFeatures prism_defaults(void) {
	return (PrismFeatures){.defer = true,
			       .zeroinit = true,
			       .line_directives = true,
			       .flatten_headers = true,
			       .orelse = true};
}

static uint32_t features_to_bits(PrismFeatures f) {
	return (f.defer ? F_DEFER : 0) | (f.zeroinit ? F_ZEROINIT : 0) |
	       (f.line_directives ? F_LINE_DIR : 0) | (f.warn_safety ? F_WARN_SAFETY : 0) |
	       (f.flatten_headers ? F_FLATTEN : 0) | (f.orelse ? F_ORELSE : 0);
}

static const char *get_tmp_dir(void) {
	static char buf[PATH_MAX];
	const char *t = getenv(TMPDIR_ENVVAR);
#ifdef TMPDIR_ENVVAR_ALT
	if (!t || !*t) t = getenv(TMPDIR_ENVVAR_ALT);
#endif
	if (!t || !*t) return TMPDIR_FALLBACK;

	size_t len = strlen(t);
	snprintf(buf, sizeof(buf), "%s%s", t, (t[len - 1] == '/' || t[len - 1] == '\\') ? "" : "/");
	return buf;
}

static bool dir_has_write_bits(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0) return true;
	if (!S_ISDIR(st.st_mode)) return false;
	return (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) != 0;
}

static inline bool is_identifier_like(Token *tok) {
	return tok->kind <= TK_KEYWORD; // TK_IDENT=0, TK_KEYWORD=1
}

static Token *skip_to_semicolon(Token *tok) {
	while (tok->kind != TK_EOF) {
		if (tok->flags & TF_OPEN) { tok = tok_next(tok_match(tok)); continue; }
		if (match_ch(tok, ';')) return tok;
		tok = tok_next(tok);
	}
	return tok;
}



static void out_flush(void) {
	if (out_buf_pos > 0) {
		fwrite(out_buf, 1, out_buf_pos, out_fp);
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

static void out_init(FILE *fp) {
	out_fp = fp;
	out_buf_pos = 0;
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
#ifdef _WIN32
		if (c == '\\') c = '/'; // normalize backslashes for MSVC C4129
#endif
		if (c == '"' || c == '\\') out_char('\\');
		out_char(c);
	}
	OUT_LIT("\"\n");
}

static bool is_reincludable_header(const char *name) {
	const char *base = strrchr(name, '/');
	return !strcmp(base ? base + 1 : name, "assert.h");
}

// Collect system headers by detecting actual #include entries (not macro expansions)
static void collect_system_includes(void) {
	for (int i = 0; i < ctx->input_file_count; i++) {
		File *f = ctx->input_files[i];
		if (!f->is_system || !f->is_include_entry || !f->name) continue;
		if (!is_reincludable_header(f->name)) {
			bool found = false;
			for (int j = 0; j < ctx->system_include_count; j++) {
				if (strcmp(system_include_list[j], f->name) == 0) { found = true; break; }
			}
			if (found) continue;
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
	OUT_LIT("#pragma GCC diagnostic pop\n");
}

// Emit collected #include directives with necessary feature test macros
static void emit_system_includes(void) {
	if (ctx->system_include_count == 0) return;

	// Emit user-specified defines (take priority over built-in feature test macros)
	for (int i = 0; i < ctx->extra_define_count; i++) {
		const char *def = ctx->extra_defines[i];
		const char *eq = strchr(def, '=');
		int name_len = eq ? (int)(eq - def) : (int)strlen(def);
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

	// Emit built-in feature test macros (guarded to not override user defines)
	OUT_LIT("#if !defined(_WIN32)\n"
		"#ifndef _POSIX_C_SOURCE\n#define _POSIX_C_SOURCE 200809L\n#endif\n"
		"#ifndef _GNU_SOURCE\n#define _GNU_SOURCE\n#endif\n"
		"#endif\n\n");

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

static inline void ctrl_reset(void) {
	ctrl_state = (CtrlState){0};
}

// ── Scope stack queries ──

static inline ScopeKind scope_top_kind(void) {
	return ctx->scope_depth > 0 ? scope_stack[ctx->scope_depth - 1].kind : SCOPE_BLOCK;
}

static inline ScopeNode *scope_block_top(void) {
	for (int i = ctx->scope_depth - 1; i >= 0; i--)
		if (scope_stack[i].kind == SCOPE_BLOCK) return &scope_stack[i];
	return NULL;
}

static inline bool in_for_init(void) {
	return ctx->scope_depth > 0 && scope_stack[ctx->scope_depth - 1].kind == SCOPE_FOR_PAREN;
}

static inline bool in_ctrl_paren(void) {
	for (int i = ctx->scope_depth - 1; i >= 0; i--) {
		ScopeKind k = scope_stack[i].kind;
		if (k == SCOPE_BLOCK) return false;
		if (k == SCOPE_FOR_PAREN || k == SCOPE_CTRL_PAREN) return true;
	}
	return false;
}

static inline bool in_struct_body(void) {
	for (int i = ctx->scope_depth - 1; i >= 0; i--)
		if (scope_stack[i].kind == SCOPE_BLOCK && scope_stack[i].is_struct) return true;
	return false;
}

static inline bool in_generic(void) {
	for (int i = ctx->scope_depth - 1; i >= 0; i--)
		if (scope_stack[i].kind == SCOPE_GENERIC) return true;
	return false;
}

static inline bool in_conditional_block(void) {
	for (int i = ctx->scope_depth - 1; i >= 0; i--)
		if (scope_stack[i].kind == SCOPE_BLOCK && scope_stack[i].is_conditional) return true;
	return false;
}

static void end_statement_after_semicolon(void) {
	ctx->at_stmt_start = true;
	if (ctrl_state.pending && !in_ctrl_paren()) {
		// Pop phantom scopes for braceless control bodies ending via break/continue/return/goto.
		// For-init variables (e.g., "for (int T = 0; ...)  break;") are registered at
		// block_depth + 1 but never get a matching '}' pop. Without this, the shadow
		// persists and corrupts subsequent typedef lookups (ghost shadow bug).
		typedef_pop_scope(ctx->block_depth + 1);
		ctrl_reset();
	}
}

static void scope_push_kind(ScopeKind kind, bool consume_flags) {
	// Guard against pathologically deep nesting (e.g., fuzz input with thousands of '{')
	if (ctx->scope_depth >= 4096) error("brace nesting depth exceeds 4096");
	ScopeNode *s = &scope_stack[ctx->scope_depth];
	*s = (ScopeNode){.kind = kind};
	s->defer_start_idx = defer_count;
	if (kind == SCOPE_BLOCK) {
		if (consume_flags) {
			s->is_loop = ctrl_state.scope_flags & NS_LOOP;
			s->is_switch = ctrl_state.scope_flags & NS_SWITCH;
			s->is_conditional = ctrl_state.scope_flags & NS_CONDITIONAL;
			ctrl_state.scope_flags = 0;
		}
		ctx->block_depth++;
	}
	ctx->scope_depth++;
}

static void scope_pop(void) {
	if (ctx->scope_depth > 0) {
		ctx->scope_depth--;
		ScopeNode *s = &scope_stack[ctx->scope_depth];
		if (s->kind == SCOPE_BLOCK) {
			defer_shadow_pop_scope(ctx->block_depth);
			ctx->block_depth--;
		}
	}
}

static void defer_add(Token *defer_keyword, Token *start, Token *end) {
	if (ctx->block_depth <= 0) error_tok(start, "defer outside of any scope");
	if (defer_count >= 2048) error_tok(start, "too many defers");
	defer_stack[defer_count++] = (DeferEntry){start, end, defer_keyword};
	scope_block_top()->had_control_exit = false;
}

static int find_switch_scope(void) {
	for (int d = ctx->scope_depth - 1; d >= 0; d--)
		if (scope_stack[d].kind == SCOPE_BLOCK && scope_stack[d].is_switch) return d;
	return -1;
}

// Mark unconditional control exit in the innermost switch scope.
// If loop_targetable is true (break/continue), skip if a loop scope
// sits between the current position and the switch scope, since the
// break/continue targets the loop, not the switch.
static void mark_switch_control_exit(bool loop_targetable) {
	if (ctrl_state.pending || in_conditional_block()) return;
	int sd = find_switch_scope();
	if (sd < 0) return;
	if (loop_targetable) {
		for (int d = ctx->scope_depth - 1; d > sd; d--)
			if (scope_stack[d].is_loop) return;
	}
	scope_stack[sd].had_control_exit = true;
}

static bool needs_space(Token *prev, Token *tok) {
	if (!prev || tok_at_bol(tok)) return false;
	if (tok_has_space(tok)) return true;
	if ((is_identifier_like(prev) || prev->kind == TK_NUM) &&
	    (is_identifier_like(tok) || tok->kind == TK_NUM))
		return true;
	if (prev->kind != TK_PUNCT || tok->kind != TK_PUNCT) return false;
	
	char a = (prev->len == 1) ? tok_loc(prev)[0] : tok_loc(prev)[prev->len - 1];
	char b = tok_loc(tok)[0];
	
	if (b == '=') return strchr("=!<>+-*/%&|^", a) != NULL;
	return (a == b && strchr("+-<>&|#", a)) || (a == '-' && b == '>') || 
	       (a == '/' && b == '*') || (a == '*' && b == '/');
}

// Check if 'tok' is inside an attribute (prevents 'defer' from being treated as keyword)
static bool is_inside_attribute(Token *tok) {
	if (!last_emitted || (!match_ch(last_emitted, '(') && !match_ch(last_emitted, ','))) return false;
	for (Token *t = tok; t && t->kind != TK_EOF && !match_ch(t, ';') && !match_ch(t, '{'); t = tok_next(t)) {
		if (t->flags & TF_OPEN) { t = tok_match(t); continue; }
		if (match_ch(t, ')')) return true;
	}
	return false;
}

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

// Forward declarations for ghost enum detection in emit_tok
static Token *find_struct_body_brace(Token *tok);
static void parse_enum_constants(Token *tok, int scope_depth);

static void emit_tok(Token *tok) {
	File *f = tok_file(tok);

	if (__builtin_expect(!FEAT(F_FLATTEN) && f->is_system && f->is_include_entry, 0)) return;

	bool need_line = false;
	char *tok_fname = NULL;
	int line_no = 0;

	if (FEAT(F_LINE_DIR)) {
		line_no = tok_cold(tok)->line_no;
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
		out_str(tok_loc(tok), tok->len);
		last_emitted = tok;
		return;
	}

	// Handle rare special case: C23 float suffix normalization
	if (__builtin_expect(tok->flags & TF_IS_FLOAT, 0) && emit_tok_special(tok)) {
		last_emitted = tok;
		return;
	}

	// Catch "ghost enums" — enum bodies hidden in casts, initializers, or
	// parameter lists that bypass try_zero_init_decl / handle_sue_body.
	// Register their constants immediately so array_size_is_vla won't
	// mistake them for runtime variables.
	if (__builtin_expect(tok->tag & TT_SUE, 0) && is_enum_kw(tok)) {
		Token *brace = find_struct_body_brace(tok);
		if (brace) parse_enum_constants(brace, ctx->block_depth);
	}

	out_str(tok_loc(tok), tok->len);
	last_emitted = tok;
}

static void emit_range(Token *start, Token *end) {
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = tok_next(t)) emit_tok(t);
}

// Like emit_range but processes zero-init/raw/orelse inside defer blocks.
static void emit_deferred_range(Token *start, Token *end) {
	bool saved_stmt_start = ctx->at_stmt_start;
	CtrlState saved_ctrl = ctrl_state;
	ctrl_state.pending = false;
	ctrl_state.pending_for_paren = false;
	ctrl_state.parens_just_closed = false;
	ctrl_state.scope_flags = 0;
	ctrl_state.pending_orelse_guard = false;
	ctrl_state.brace_depth = 0;

	ctx->at_stmt_start = true;

	for (Token *t = start; t && t != end && t->kind != TK_EOF;) {
		if (ctx->at_stmt_start && FEAT(F_ZEROINIT)) {
			Token *next = try_zero_init_decl(t);
			if (next) {
				t = next;
				ctx->at_stmt_start = true;
				continue;
			}
		}
		ctx->at_stmt_start = false;

		if (t->tag & TT_STRUCTURAL) {
			emit_tok(t);
			t = tok_next(t);
			ctx->at_stmt_start = true;
			continue;
		}

		emit_tok(t);
		t = tok_next(t);
	}

	ctx->at_stmt_start = saved_stmt_start;
	ctrl_state = saved_ctrl;
}

static void emit_defers_ex(DeferEmitMode mode, int stop_depth) {
	if (ctx->block_depth <= 0) return;

	// For control-flow exits (not end-of-scope), verify no live shadow conflicts.
	if (mode != DEFER_SCOPE)
		check_defer_shadow_at_exit(mode, stop_depth);

	int current_defer = defer_count - 1;
	for (int d = ctx->scope_depth - 1; d >= 0; d--) {
		if (scope_stack[d].kind != SCOPE_BLOCK) continue;
		if (mode == DEFER_TO_DEPTH && d < stop_depth) break;

		ScopeNode *scope = &scope_stack[d];
		for (int i = current_defer; i >= scope->defer_start_idx; i--) {
			out_char(' ');
			emit_deferred_range(defer_stack[i].stmt, defer_stack[i].end);
			out_char(';');
		}
		current_defer = scope->defer_start_idx - 1;

		if (mode == DEFER_SCOPE) break;
		if (mode == DEFER_BREAK && (scope->is_loop || scope->is_switch)) break;
		if (mode == DEFER_CONTINUE && scope->is_loop) break;
	}
}

static bool has_defers_for(DeferEmitMode mode, int stop_depth) {
	for (int d = ctx->scope_depth - 1; d >= 0; d--) {
		if (scope_stack[d].kind != SCOPE_BLOCK) continue;
		if (mode == DEFER_TO_DEPTH && d < stop_depth) break;
		if (defer_count > scope_stack[d].defer_start_idx) return true;
		if (mode == DEFER_BREAK && (scope_stack[d].is_loop || scope_stack[d].is_switch))
			return false;
		if (mode == DEFER_CONTINUE && scope_stack[d].is_loop) return false;
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
	if (outer_defer_end <= 0) return;
	char *name = tok_loc(var_name);
	int nlen = var_name->len;
	for (int i = 0; i < outer_defer_end; i++) {
		Token *prev = NULL;
		for (Token *t = defer_stack[i].stmt; t && t != defer_stack[i].end && t->kind != TK_EOF;
		     prev = t, t = tok_next(t)) {
			if ((t->kind == TK_IDENT || t->kind == TK_KEYWORD) &&
			    !(prev && (prev->tag & TT_MEMBER)) &&
			    t->len == nlen && !memcmp(tok_loc(t), name, nlen)) {
				if (defer_shadow_count >= MAX_DEFER_SHADOWS)
					error_tok(var_name, "too many shadowed variables in defer scope; limit is %d",
						  MAX_DEFER_SHADOWS);
				defer_shadows[defer_shadow_count++] = (DeferShadow){
						.name = name, .len = nlen,
						.block_depth = ctx->block_depth,
						.var_tok = var_name,
						.defer_idx = i,
					};
				return;
			}
		}
	}
}

// Pop defer shadows when leaving a scope (block_depth is about to decrease).
static void defer_shadow_pop_scope(int leaving_block_depth) {
	while (defer_shadow_count > 0 &&
	       defer_shadows[defer_shadow_count - 1].block_depth >= leaving_block_depth)
		defer_shadow_count--;
}

// Check if any currently-live defer shadow conflicts with the defers that are
// about to be pasted.  Called at return/goto/break/continue — anywhere
// emit_defers_ex will paste defer bodies while inner variables are still live.
static void check_defer_shadow_at_exit(DeferEmitMode mode, int stop_depth) {
	if (defer_shadow_count == 0) return;
	// Determine which defer indices will be pasted by this exit.
	// Walk scopes the same way emit_defers_ex does, collecting the range.
	int min_defer_idx = defer_count; // exclusive upper bound not needed; just track min
	for (int d = ctx->scope_depth - 1; d >= 0; d--) {
		if (scope_stack[d].kind != SCOPE_BLOCK) continue;
		if (mode == DEFER_TO_DEPTH && d < stop_depth) break;
		if (scope_stack[d].defer_start_idx < min_defer_idx)
			min_defer_idx = scope_stack[d].defer_start_idx;
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

static void label_table_add(char *name, int name_len, int scope_depth, Token *tok, Token *block_open) {
	ARENA_ENSURE_CAP(&ctx->main_arena,
			 label_table,
			 label_count + 1,
			 label_capacity,
			 128,
			 LabelInfo);
	LabelInfo *info = &label_table[label_count++];
	info->name = name; info->name_len = name_len; info->scope_depth = scope_depth; info->tok = tok; info->block_open = block_open;
}

static void typedef_table_reset(void) {
	typedef_table.entries = NULL;
	typedef_table.count = 0;
	typedef_table.capacity = 0;
	hashmap_zero(&typedef_table.name_map);
}

static int typedef_get_index(char *name, int len) {
	void *val = hashmap_get(&typedef_table.name_map, name, len);
	return val ? (int)(intptr_t)val - 1 : -1;
}

static void typedef_set_index(char *name, int len, int index) {
	hashmap_put(&typedef_table.name_map, name, len, (void *)(intptr_t)(index + 1));
}

static void
typedef_add_entry(char *name, int len, int scope_depth, TypedefKind kind, bool is_vla, bool is_void) {
	// Skip duplicate re-definitions at the same scope (valid C11 §6.7/3).
	if (kind == TDK_TYPEDEF || kind == TDK_ENUM_CONST) {
		int existing = typedef_get_index(name, len);
		if (existing >= 0) {
			TypedefEntry *prev = &typedef_table.entries[existing];
			if (prev->scope_depth == scope_depth && !prev->is_shadow)
				return;
		}
	}

	ARENA_ENSURE_CAP(&ctx->main_arena,
			 typedef_table.entries,
			 typedef_table.count + 1,
			 typedef_table.capacity,
			 INITIAL_ARRAY_CAP,
			 TypedefEntry);
	int new_index = typedef_table.count++;
	TypedefEntry *e = &typedef_table.entries[new_index];
	e->name = name;
	e->len = len;
	e->scope_depth = scope_depth;
	e->is_vla = (kind == TDK_TYPEDEF || kind == TDK_VLA_VAR) ? is_vla : false;
	e->is_void = (kind == TDK_TYPEDEF) ? is_void : false;
	e->is_const = false;
	e->is_shadow = (kind == TDK_SHADOW || kind == TDK_ENUM_CONST);
	e->is_enum_const = (kind == TDK_ENUM_CONST);
	e->is_vla_var = (kind == TDK_VLA_VAR);
	e->prev_index = typedef_get_index(name, len);
	typedef_set_index(name, len, new_index);
}

// Removes typedefs at or above given depth. Uses >= for resilience against
// missed phantom pops; in normal operation, entries are monotonically non-decreasing.
static void typedef_pop_scope(int scope_depth) {
	while (typedef_table.count > 0 &&
	       typedef_table.entries[typedef_table.count - 1].scope_depth >= scope_depth) {
		TypedefEntry *e = &typedef_table.entries[typedef_table.count - 1];
		if (e->prev_index >= 0) typedef_set_index(e->name, e->len, e->prev_index);
		else
			hashmap_put(&typedef_table.name_map, e->name, e->len, NULL);
		typedef_table.count--;
	}
}

// Register enum constants as typedef shadows. tok points to opening '{'.
static void parse_enum_constants(Token *tok, int scope_depth) {
	if (!tok || !match_ch(tok, '{')) return;
	tok = tok_next(tok); // Skip '{'

	while (tok && tok->kind != TK_EOF && !match_ch(tok, '}')) {
		Token *next = skip_noise(tok);
		if (next != tok) { tok = next; continue; }

		if (is_valid_varname(tok)) {
			typedef_add_enum_const(tok_loc(tok), tok->len, scope_depth);
			tok = tok_next(tok);

			if (tok && match_ch(tok, '=')) {
				tok = tok_next(tok);
				while (tok && tok->kind != TK_EOF) {
					if (tok->flags & TF_OPEN) { tok = tok_next(tok_match(tok)); continue; }
					if (match_ch(tok, ',') || match_ch(tok, '}')) break;
					tok = tok_next(tok);
				}
			}

			if (tok && match_ch(tok, ',')) tok = tok_next(tok);
		} else {
			tok = tok_next(tok);
		}
	}
}

static TypedefEntry *typedef_lookup(Token *tok) {
	if (!is_identifier_like(tok)) return NULL;
	int idx = typedef_get_index(tok_loc(tok), tok->len);
	return idx >= 0 ? &typedef_table.entries[idx] : NULL;
}

// Typedef query flags (single lookup, check multiple properties)
enum { TDF_TYPEDEF = 1, TDF_VLA = 2, TDF_VOID = 4, TDF_ENUM_CONST = 8, TDF_CONST = 16, TDF_PTR = 32, TDF_ARRAY = 64, TDF_AGGREGATE = 128 };

static inline int typedef_flags(Token *tok) {
	TypedefEntry *e = typedef_lookup(tok);
	if (!e) return 0;
	if (e->is_enum_const) return TDF_ENUM_CONST;
	if (e->is_shadow) return 0;
	if (e->is_vla_var) return TDF_VLA;
	return TDF_TYPEDEF | (e->is_vla ? TDF_VLA : 0) | (e->is_void ? TDF_VOID : 0) |
	       (e->is_const ? TDF_CONST : 0) | (e->is_ptr ? TDF_PTR : 0) |
	       (e->is_array ? TDF_ARRAY : 0) | (e->is_aggregate ? TDF_AGGREGATE : 0);
}

#define is_known_typedef(tok) (typedef_flags(tok) & TDF_TYPEDEF)
#define is_vla_typedef(tok) (typedef_flags(tok) & TDF_VLA)
#define is_void_typedef(tok) (typedef_flags(tok) & TDF_VOID)
#define is_known_enum_const(tok) (typedef_flags(tok) & TDF_ENUM_CONST)
#define is_const_typedef(tok) (typedef_flags(tok) & TDF_CONST)
#define is_ptr_typedef(tok) (typedef_flags(tok) & TDF_PTR)
#define is_array_typedef(tok) (typedef_flags(tok) & TDF_ARRAY)
#define is_aggregate_typedef(tok) (typedef_flags(tok) & TDF_AGGREGATE)

static bool is_typedef_like(Token *tok);  // forward declaration

static Token *find_boundary_comma(Token *tok) {
	while (tok->kind != TK_EOF) {
		if (tok->flags & TF_OPEN) { tok = tok_next(tok_match(tok)); continue; }
		if (match_ch(tok, ';')) return NULL;
		if (match_ch(tok, ',')) {
			// Verify following token can start a declarator (*, (, qualifier, or identifier).
			// If not, this is a comma operator, not a declarator separator.
			Token *n = tok_next(tok);
			if (n) {
				if (match_ch(n, '(')) {
					Token *inside = tok_next(n);
					if (inside && !(inside->tag & (TT_TYPE | TT_SUE | TT_TYPEOF | TT_QUALIFIER))
					    && !is_typedef_like(inside) && !is_c23_attr(inside))
						return tok;
				} else if (match_ch(n, '*') || (n->tag & TT_QUALIFIER) || (n->tag & TT_ATTR) || is_c23_attr(n)) {
					return tok;
				} else if (is_valid_varname(n) && !(n->tag & (TT_TYPE | TT_SUE | TT_TYPEOF))) {
					if (tok_next(n) && match_ch(tok_next(n), '(')) {
						Token *inside = tok_next(tok_next(n));
						if (inside &&
						    (inside->tag & (TT_TYPE | TT_SUE | TT_TYPEOF | TT_QUALIFIER)))
							return tok;
					} else
						return tok;
				}
			}
		}
		tok = tok_next(tok);
	}
	return NULL;
}

// Strict type resolution: a token is a type only if it was explicitly registered
// in the symbol table by the declaration pre-scan or inline parse_typedef_declaration.
// No heuristics — if it's not in the table, it's not a type. Period.
static inline bool is_typedef_heuristic(Token *tok) {
	(void)tok;
	return false;
}

static bool is_typedef_like(Token *tok) {
	if (!is_identifier_like(tok)) return false;
	return is_known_typedef(tok);
}

// Find opening brace of a struct/union/enum body, or NULL if no body.
static Token *find_struct_body_brace(Token *tok) {
	Token *t = tok_next(tok);
	while (t && t->kind != TK_EOF) {
		Token *next = skip_noise(t);
		if (next != t) { t = next; continue; }
		if (is_valid_varname(t) || (t->tag & TT_QUALIFIER)) {
			t = tok_next(t);
		} else break;
	}
	return (t && match_ch(t, '{')) ? t : NULL;
}

// Advance tok, skipping struct/union/enum bodies and _Generic expressions.
static inline Token *skip_sue_generic(Token *tok) {
	if (tok->tag & TT_SUE) {
		Token *brace = find_struct_body_brace(tok);
		if (brace) return tok_match(brace) ? tok_next(tok_match(brace)) : brace;
	}
	if (tok->tag & TT_GENERIC) {
		tok = tok_next(tok);
		if (tok && match_ch(tok, '(')) return tok_match(tok) ? tok_next(tok_match(tok)) : tok;
		return tok;
	}
	return tok_next(tok);
}

// Check if tok is a goto label (ident : not :: not case/default/ternary).
static inline Token *check_label(Token *tok, Token *prev, int *ternary_depth) {
	if (!is_identifier_like(tok)) return NULL;
	Token *t = skip_noise(tok_next(tok));
	if (!t || !match_ch(t, ':')) return NULL;
	if (tok_next(t) && match_ch(tok_next(t), ':')) return NULL;
	if (prev && match_ch(prev, '?')) return NULL;
	if (*ternary_depth > 0) { (*ternary_depth)--; return NULL; }
	if (prev && (prev->tag & (TT_CASE | TT_DEFAULT))) return NULL;
	return tok;
}

// Scan function body for labels. All flags read from brace tags set by parse.c.
static void scan_labels_in_function(Token *tok) {
	label_count = 0;
	ctx->current_func_has_setjmp = false;
	ctx->current_func_has_asm = false;
	ctx->current_func_has_vfork = false;
	if (!tok || !match_ch(tok, '{') || !tok_match(tok)) return;

	ctx->current_func_has_setjmp = (tok->tag & TT_SPECIAL_FN) != 0;
	ctx->current_func_has_asm = (tok->tag & TT_ASM) != 0;
	ctx->current_func_has_vfork = (tok->tag & TT_NORETURN_FN) != 0;

	#define BLOCK_STACK_MAX 256
	Token *block_stack[BLOCK_STACK_MAX];
	block_stack[0] = NULL;
	block_stack[1] = tok;
	Token *end = tok_match(tok);
	int depth = 1, ternary_depth = 0;
	Token *prev = NULL;

	for (Token *t = tok_next(tok); t != end && t->kind != TK_EOF; ) {
		if (match_ch(t, '{') && ++depth < BLOCK_STACK_MAX)
			block_stack[depth] = t;
		else if (match_ch(t, '}'))
			depth--;
		if (match_ch(t, '?')) ternary_depth++;
		Token *label = check_label(t, prev, &ternary_depth);
		if (label) {
			Token *bo = (depth > 0 && depth < BLOCK_STACK_MAX) ? block_stack[depth] : NULL;
			label_table_add(tok_loc(label), label->len, depth, label, bo);
		}
		prev = t;
		t = skip_sue_generic(t);
	}
	#undef BLOCK_STACK_MAX
}

// Quick pre-check: is this a variable declaration (not a function decl or stmt expr)?
static bool is_var_declaration(Token *type_end) {
	DeclResult decl = parse_declarator(type_end, false);
	if (!decl.var_name || !decl.end) return false;

	// Statement expression initializer: type name = ({...})
	// For multi-declarator decls, skip past the stmt-expr and check for ','.
	if (match_ch(decl.end, '=')) {
		Token *after_eq = tok_next(decl.end);
		if (after_eq && match_ch(after_eq, '(') && tok_next(after_eq) && match_ch(tok_next(after_eq), '{')) {
			Token *after_stmt_expr = skip_balanced(after_eq, '(', ')');
			while (after_stmt_expr && after_stmt_expr->kind != TK_EOF &&
			       (after_stmt_expr->tag & TT_ATTR))
				after_stmt_expr = skip_noise(after_stmt_expr);
			return match_ch(after_stmt_expr, ',');
		}
		return true;
	}

	return match_ch(decl.end, ',') || match_ch(decl.end, ';');
}

// Like is_var_declaration but only matches declarations WITHOUT an initializer
// (i.e., declarations where zeroinit would add = 0).
static bool is_uninit_var_declaration(Token *type_end) {
	DeclResult decl = parse_declarator(type_end, false);
	if (!decl.var_name || !decl.end) return false;
	if (decl.has_init) return false;
	return match_ch(decl.end, ',') || match_ch(decl.end, ';');
}

static inline bool is_orelse_keyword(Token *tok) {
	return (tok->tag & TT_ORELSE) && !is_known_typedef(tok) && !typedef_lookup(tok) &&
	       !(last_emitted && (last_emitted->tag & TT_MEMBER));
}

static inline bool is_assignment_operator_token(Token *tok) {
	return match_ch(tok, '=') || equal(tok, "+=") || equal(tok, "-=") ||
	       equal(tok, "*=") || equal(tok, "/=") || equal(tok, "%=") ||
	       equal(tok, "<<=") || equal(tok, ">>=") || equal(tok, "&=") ||
	       equal(tok, "^=") || equal(tok, "|=");
}

// Emit type tokens, optionally stripping const and struct/enum bodies.
static void emit_type_stripped(Token *start, Token *end, bool strip_const) {
	Token *t = start;
	while (t && t != end) {
		if (strip_const && (t->tag & TT_CONST)) { t = tok_next(t); continue; }
		// typeof with potential orelse inside: use orelse-aware walker
		if (FEAT(F_ORELSE) && (t->tag & TT_TYPEOF) && tok_next(t) && match_ch(tok_next(t), '(')) {
			emit_tok(t);           // typeof keyword
			t = tok_next(t);       // (
			t = walk_balanced_orelse(t);
			continue;
		}
		if (match_ch(t, '{')) {
			Token *kw = NULL;
			for (Token *s = start; s != t; s = tok_next(s))
				if (s->tag & TT_SUE) kw = s;
			bool keep = false;
			if (kw && !is_enum_kw(kw)) {
				// Preserve braces for compound literals such as
				// `typeof((struct S){1})`, where the struct tag is wrapped by an
				// outer parenthesized type and this brace is the initializer, not
				// the struct body.
				for (Token *u = tok_next(kw); u && u != t; u = tok_next(u)) {
					if (match_ch(u, ')') && tok_match(u) && tok_loc(tok_match(u)) < tok_loc(kw)) {
						keep = true;
						break;
					}
				}
				if (!keep) {
					keep = true;
					for (Token *u = tok_next(kw); u && u != t; u = tok_next(u)) {
						if (is_valid_varname(u) && !(u->tag & (TT_QUALIFIER | TT_ATTR | TT_TYPEOF))) {
							keep = false;
							break;
						}
					}
				}
			}
			if (!keep) {
				t = walk_balanced(t, false);
				if (t == end) break;
			}
		}
		emit_tok(t);
		t = tok_next(t);
	}
}

// Like emit_range but transforms orelse inside typeof expressions.
static void emit_type_range_orelse(Token *start, Token *end) {
	for (Token *t = start; t && t != end && t->kind != TK_EOF;) {
		if (FEAT(F_ORELSE) && (t->tag & TT_TYPEOF) && tok_next(t) && match_ch(tok_next(t), '(')) {
			emit_tok(t);           // typeof keyword
			t = tok_next(t);       // (
			t = walk_balanced_orelse(t);
			continue;
		}
		emit_tok(t);
		t = tok_next(t);
	}
}

static inline bool is_file_storage_keyword(Token *tok) {
	return tok->tag & (TT_STORAGE | TT_TYPEDEF);
}

// Emit expression tokens to ';', stop token, or 'orelse' (if check_orelse).
static Token *emit_expr_to_stop(Token *tok, Token *stop, bool check_orelse) {
	while (tok->kind != TK_EOF) {
		if (tok->flags & TF_OPEN) { tok = walk_balanced(tok, true); continue; }
		if (match_ch(tok, ';') || (stop && tok == stop)) break;
		if (check_orelse && is_orelse_keyword(tok)) break;
		emit_tok(tok);
		tok = tok_next(tok);
	}
	return tok;
}

// Returns true if 'raw' is followed by a declaration context (type keyword, typedef, *, etc.)
static bool is_raw_declaration_context(Token *after_raw) {
	after_raw = skip_noise(after_raw);
	return after_raw && (is_type_keyword(after_raw) || is_known_typedef(after_raw) ||
			     match_ch(after_raw, '*') || (after_raw->tag & (TT_QUALIFIER | TT_SUE)));
}

// Count net scope exits (brace depths) between start and end.
static int goto_scope_exits(Token *start, Token *end) {
	int depth = 0, min_depth = 0;
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = tok_next(t)) {
		if (t->flags & TF_OPEN) {
			if (tok_loc(end) > tok_loc(tok_match(t))) { t = tok_match(t); continue; }
			depth++;
		} else if (t->flags & TF_CLOSE) {
			if (--depth < min_depth) min_depth = depth;
		}
	}
	return -min_depth;
}

static Token *backward_goto_skips_decl(Token *goto_tok, LabelInfo *info) {
	if (!FEAT(F_ZEROINIT | F_DEFER) || !info || tok_loc(info->tok) >= tok_loc(goto_tok)) return NULL;

	if (!info->block_open) {
		if (info->scope_depth > 0)
			error_tok(goto_tok, "backward goto to '%.*s' in deeply nested block "
				  "(depth %d exceeds tracking limit); reduce nesting",
				  info->name_len, info->name, info->scope_depth);
		return NULL;
	}

	Token *block_close = tok_match(info->block_open);
	if (block_close && tok_loc(goto_tok) < tok_loc(block_close) && tok_loc(goto_tok) > tok_loc(info->block_open))
		return NULL;

	bool is_stmt_start = true;
	for (Token *t = tok_next(info->block_open); t && t != info->tok && t->kind != TK_EOF; t = tok_next(t)) {
		if (t->flags & TF_OPEN) { t = tok_match(t); continue; }
		if (match_ch(t, ';') || match_ch(t, ':')) { is_stmt_start = true; continue; }

		if (is_stmt_start) {
			Token *s = t;
			if ((s->flags & TF_RAW) && !is_known_typedef(s)) s = tok_next(s);
			if (s) s = skip_noise(s);
			if (s && !is_file_storage_keyword(s)) {
				TypeSpecResult type = parse_type_specifier(s);
				if (type.saw_type && is_uninit_var_declaration(type.end)) return t;
			}
		}
		is_stmt_start = false;
	}
	return NULL;
}

static GotoSkipResult goto_skips_check(Token *goto_tok, LabelInfo *info, bool check_defer, bool check_decl) {
	GotoSkipResult r = {NULL, NULL};
	if (check_decl && !FEAT(F_ZEROINIT | F_DEFER)) check_decl = false;
	if (!check_defer && !check_decl) return r;
	if (!info || tok_loc(info->tok) < tok_loc(goto_tok)) return r; // Forward only

	Token *start = tok_next(tok_next(goto_tok));
	if (start && match_ch(start, ';')) start = tok_next(start);

	Token *active_defer = NULL, *active_decl = NULL;
	int scope_depth = -1, decl_depth = -1, depth = 0;
	bool is_stmt_start = true;

	for (Token *tok = start; tok && tok != info->tok && tok->kind != TK_EOF; ) {
		if (tok->flags & TF_OPEN) {
			if (tok_loc(info->tok) > tok_loc(tok_match(tok))) { tok = tok_next(tok_match(tok)); continue; }
			depth++; is_stmt_start = true; tok = tok_next(tok); continue;
		}
		if (match_ch(tok, '}')) {
			if (active_defer && depth <= scope_depth) { active_defer = NULL; scope_depth = -1; }
			if (active_decl && depth <= decl_depth) { active_decl = NULL; decl_depth = -1; }
			depth--; is_stmt_start = true; tok = tok_next(tok); continue;
		}
		if (match_ch(tok, ';')) {
			is_stmt_start = true; tok = tok_next(tok); continue;
		}

		if (check_defer && (tok->tag & TT_DEFER) && !is_known_typedef(tok)) {
			if (!active_defer || depth <= scope_depth) {
				active_defer = tok;
				scope_depth = depth;
			}
		}

		if (check_decl && is_stmt_start && !(tok->tag & TT_LOOP)) {
			Token *s = tok;
			bool has_raw = false;
			if ((s->flags & TF_RAW) && !is_known_typedef(s)) { has_raw = true; s = tok_next(s); }
			if (s) s = skip_noise(s);
			if (s && !is_file_storage_keyword(s)) {
				TypeSpecResult type = parse_type_specifier(s);
				if (type.saw_type && is_uninit_var_declaration(type.end)) {
					if (!has_raw && (!active_decl || depth <= decl_depth)) {
						active_decl = tok; decl_depth = depth;
					}
				}
			}
		}
		is_stmt_start = false;
		tok = tok_next(tok);
	}
	r.skipped_defer = active_defer;
	r.skipped_decl = active_decl;
	return r;
}

// Legacy K&R C parameter detection (slow path heuristic).
static bool is_knr_params(Token *start, Token *brace) {
	if (!start || start == brace || match_ch(start, ';'))
		return false;
	bool saw_semi = false;
	for (Token *t = start; t && t != brace && t->kind != TK_EOF; t = tok_next(t)) {
		if (match_ch(t, ';')) saw_semi = true;
		if (t->flags & TF_OPEN) t = tok_match(t) ? tok_match(t) : t; // skip groups safely
	}
	return saw_semi;
}

static Token *skip_pointers(Token *tok, bool *is_void) {
	while (tok && tok->kind != TK_EOF) {
		Token *next = skip_noise(tok);
		if (next != tok) { tok = next; continue; }
		if (match_ch(tok, '*') || (tok->tag & TT_QUALIFIER)) {
			tok = tok_next(tok);
			if (is_void) *is_void = false;
		} else break;
	}
	return tok;
}

// Captures function return type. Returns 1 if void function, 2 if captured, 0 if not function.
static int capture_function_return_type(Token *tok) {
	while (tok && tok->kind != TK_EOF) {
		if ((tok->tag & (TT_SKIP_DECL | TT_INLINE)) || equal(tok, "_Noreturn") || equal(tok, "noreturn")) {
			tok = tok_next(tok); continue;
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
					Token *decl_end = skip_balanced(outer_open, '(', ')');
					decl_end = skip_declarator_suffix(decl_end);
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
			// typedef PREFIX _Prism_ret_t_N SUFFIX; _Prism_ret_t_N
			OUT_LIT("typedef ");
			emit_token_range(ctx->func_ret_type_start, ctx->func_ret_type_end);
			OUT_LIT(" _Prism_ret_t_");
			out_uint(ctx->ret_counter);
			for (Token *t = ctx->func_ret_type_suffix_start;
			     t && t != ctx->func_ret_type_suffix_end && t->kind != TK_EOF;
			     t = tok_next(t)) {
				out_char(' ');
				out_str(tok_loc(t), t->len);
			}
			OUT_LIT("; _Prism_ret_t_");
			out_uint(ctx->ret_counter);
		} else {
			emit_token_range(ctx->func_ret_type_start, ctx->func_ret_type_end);
		}
	} else {
		error("defer in function with unresolvable return type; "
		      "use a named struct or typedef");
	}
}

// Check if array dimension contains a VLA expression (runtime variable).
// sizeof/alignof/offsetof args are skipped except sizeof(VLA_Typedef).
static bool array_size_is_vla(Token *open_bracket) {
	Token *close = tok_match(open_bracket);
	if (!close) return false;
	Token *tok = tok_next(open_bracket);

	while (tok != close) {
		if (match_ch(tok, '[')) {
			if (array_size_is_vla(tok)) return true;
			tok = skip_balanced(tok, '[', ']');
			continue;
		}
		if (tok->tag & TT_GENERIC) return true;
		Token *next = skip_noise(tok);
		if (next != tok) { tok = next; continue; }

		// sizeof/alignof: skip argument but check for VLA typedef and inner VLA types.
		if (is_sizeof_like(tok)) {
			bool is_sizeof = tok_loc(tok)[0] == 's';
			tok = tok_next(tok);
			if (tok != close && match_ch(tok, '(')) {
				Token *end = skip_balanced(tok, '(', ')');
				if (is_sizeof) {
					Token *prev_inner = tok;
					for (Token *inner = tok_next(tok); inner && inner != end; prev_inner = inner, inner = tok_next(inner)) {
						if (is_enum_kw(inner)) {
							Token *brace = find_struct_body_brace(inner);
							if (brace) {
								parse_enum_constants(brace, ctx->block_depth);
								inner = skip_balanced(brace, '{', '}');
								if (inner == end) break;
								continue;
							}
						}
						if (is_vla_typedef(inner)) return true;
						if (match_ch(inner, '[') &&
						    (is_type_keyword(prev_inner) || is_known_typedef(prev_inner) ||
						     match_ch(prev_inner, ']') || match_ch(prev_inner, '*') ||
						     match_ch(prev_inner, ')')) &&
						    array_size_is_vla(inner))
							return true;
						if (is_valid_varname(inner) && !is_type_keyword(inner) &&
						    !is_known_typedef(inner) && !is_known_enum_const(inner) &&
						    tok_next(inner) && inner != end && match_ch(tok_next(inner), '(')) {
							Token *call_end = skip_balanced(tok_next(inner), '(', ')');
							bool is_deref = match_ch(prev_inner, '*') ||
							    (call_end && call_end != end &&
							     (match_ch(call_end, '[') || equal(call_end, "->") || match_ch(call_end, '.')));
							if (is_deref)
								for (Token *a = tok_next(tok_next(inner)); a && a != call_end; a = tok_next(a))
									if (is_valid_varname(a) && !is_known_enum_const(a) &&
									    !is_type_keyword(a))
										return true;
							prev_inner = inner;
							inner = call_end;
							if (!inner || inner == end) break;
						}
					}
				}
				tok = end;
				if (tok != close && match_ch(tok, '{'))
					tok = skip_balanced(tok, '{', '}');
			} else if (tok != close) {
				// Unparenthesized sizeof/alignof: skip prefix ops, pragmas, operand, postfix.
				while (tok != close) {
					Token *next = skip_noise(tok);
					if (next != tok) { tok = next; continue; }
					if (match_ch(tok, '*') || match_ch(tok, '&') || match_ch(tok, '!') ||
					    match_ch(tok, '~') || equal(tok, "++") || equal(tok, "--") ||
					    match_ch(tok, '+') || match_ch(tok, '-') || is_sizeof_like(tok)) {
						tok = tok_next(tok);
						continue;
					}
					break;
				}
				if (tok != close) {
					if (tok->flags & TF_OPEN) {
						tok = tok_match(tok) ? tok_next(tok_match(tok)) : tok;
						if (tok != close && match_ch(tok, '{'))
							tok = tok_match(tok) ? tok_next(tok_match(tok)) : tok;
					} else {
						if (is_identifier_like(tok) && is_vla_typedef(tok)) return true;
						tok = tok_next(tok);
					}
				}
				while (tok != close) {
					if (tok->tag & TT_MEMBER) {
						tok = tok_next(tok);
						if (tok != close) tok = tok_next(tok);
					} else if (tok->flags & TF_OPEN)
						tok = tok_match(tok) ? tok_next(tok_match(tok)) : tok;
					else break;
				}
			}
			continue;
		}

		if (tok->tag & TT_MEMBER) return true;
		if (is_valid_varname(tok) && !is_known_enum_const(tok) && !is_type_keyword(tok)) return true;
		tok = tok_next(tok);
	}
	return false;
}

static bool struct_body_contains_vla(Token *brace) {
	if (!brace || !match_ch(brace, '{') || !tok_match(brace)) return false;
	Token *end = tok_match(brace);
	for (Token *t = tok_next(brace); t && t != end; t = tok_next(t)) {
		if (match_ch(t, '{')) {
			if (struct_body_contains_vla(t)) return true;
			t = tok_match(t); continue;
		}
		if ((t->flags & TF_OPEN) && !match_ch(t, '[')) { t = tok_match(t); continue; }
		if (match_ch(t, '[') && array_size_is_vla(t)) return true;
		if (is_identifier_like(t) && is_vla_typedef(t)) return true;
	}
	return false;
}

static bool typedef_contains_vla(Token *tok) {
	while (tok && tok->kind != TK_EOF) {
		if (match_ch(tok, ';')) break;
		if ((tok->flags & TF_OPEN) && !match_ch(tok, '[')) { tok = tok_next(tok_match(tok)); continue; }
		if (match_ch(tok, '[') && array_size_is_vla(tok)) return true;
		tok = tok_next(tok);
	}
	return false;
}

static void parse_typedef_declaration(Token *tok, int scope_depth) {
	Token *typedef_start = tok;
	tok = tok_next(tok); // Skip 'typedef'
	Token *type_start = tok;
	TypeSpecResult type_spec = parse_type_specifier(tok);
	tok = type_spec.end; // Skip the base type

	bool is_vla = type_spec.is_vla || typedef_contains_vla(typedef_start);

	// Check if the base type has const — either explicit tokens or from a const typedef.
	bool base_is_const = type_spec.has_const;
	if (!base_is_const) {
		for (Token *t = type_start; t && t != tok; t = tok_next(t))
			if (is_const_typedef(t)) { base_is_const = true; break; }
	}

	// Check if the base type is void (or a typedef alias for void).
	bool base_is_void = type_spec.has_void;

	// Check if base type is a pointer (for safe const-stripping in orelse).
	bool base_is_ptr = false;
	bool base_is_array = false;
	for (Token *bt = type_start; bt && bt != type_spec.end; bt = tok_next(bt)) {
		if (is_ptr_typedef(bt)) { base_is_ptr = true; break; }
		if (is_array_typedef(bt)) { base_is_array = true; break; }
	}

	// Parse declarator(s) until semicolon
	while (tok && !match_ch(tok, ';') && tok->kind != TK_EOF) {
		DeclResult decl = parse_declarator(tok, false);
		if (decl.var_name) {
			// A typedef is void only if base is void and declarator is plain
			bool is_void =
			    base_is_void && !decl.is_pointer && !decl.is_array && !decl.is_func_ptr;
			// A typedef is const only if base has const and declarator is plain
			bool is_const =
			    base_is_const && !decl.is_pointer && !decl.is_array && !decl.is_func_ptr;
			// Pointer if declarator has ptr/fptr or base is ptr. Not arrays (cast constraint violation).
			bool is_ptr =
			    decl.is_pointer || decl.is_func_ptr || base_is_ptr;
			typedef_add(tok_loc(decl.var_name), decl.var_name->len, scope_depth, is_vla, is_void);
			// Set is_const, is_ptr, is_array, is_aggregate on the just-added entry
			if (typedef_table.count > 0) {
				if (is_const)
					typedef_table.entries[typedef_table.count - 1].is_const = true;
				if (is_ptr)
					typedef_table.entries[typedef_table.count - 1].is_ptr = true;
				if ((decl.is_array || base_is_array) && !decl.is_pointer && !decl.is_func_ptr)
					typedef_table.entries[typedef_table.count - 1].is_array = true;
				if (type_spec.is_struct && !type_spec.is_enum && !decl.is_pointer && !decl.is_func_ptr)
					typedef_table.entries[typedef_table.count - 1].is_aggregate = true;
			}
		}
		tok = decl.end ? decl.end : tok_next(tok);

		while (tok && !match_ch(tok, ',') && !match_ch(tok, ';') && tok->kind != TK_EOF) {
			if (match_ch(tok, '(')) tok = skip_balanced(tok, '(', ')');
			else if (match_ch(tok, '['))
				tok = skip_balanced(tok, '[', ']');
			else
				tok = tok_next(tok);
		}

		if (tok && match_ch(tok, ',')) tok = tok_next(tok);
	}
}

// Zero-init helpers

static bool is_type_keyword(Token *tok) {
	if (tok->tag & TT_TYPE) return true;
	// Only identifiers and prism keywords (raw, defer, orelse) can be user-defined typedefs
	if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD) return false;
	return is_typedef_like(tok);
}

static inline bool is_valid_varname(Token *tok) {
	return tok->kind == TK_IDENT || (tok->flags & TF_RAW) || (tok->tag & (TT_DEFER | TT_ORELSE));
}

static bool is_decl_prefix_token(Token *tok) {
	if (!tok) return false;
	if (match_ch(tok, '*') || match_ch(tok, ')')) return true;
	if (tok->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_SKIP_DECL | TT_ATTR |
			TT_INLINE | TT_STORAGE | TT_TYPEOF | TT_BITINT))
		return true;
	return is_typedef_like(tok);
}

static bool params_look_like_decls(Token *open) {
	Token *close = tok_match(open);
	if (!close) return false;

	for (Token *t = tok_next(open); t && t != close; t = tok_next(t)) {
		if (t->flags & TF_OPEN) {
			if (tok_match(t)) t = tok_match(t);
			continue;
		}
		if (t->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_BITINT |
			      TT_ATTR | TT_STORAGE))
			return true;
		if (is_typedef_like(t)) return true;
	}

	return false;
}

static bool generic_decl_rewrite_target(Token *generic_tok, Token **name_out,
					Token **params_open_out,
					Token **params_close_out,
					Token **next_out) {
	Token *open = tok_next(generic_tok);
	if (!open || !match_ch(open, '(') || !tok_match(open)) return false;

	Token *close = tok_match(open);
	Token *after = skip_noise(tok_next(close));
	if (!after) return false;

	// Pattern 1: _Generic(...name(decl-params)...) ;/attr
	if (match_ch(after, ';') || match_ch(after, ',') || (after->tag & TT_ATTR) ||
	    is_c23_attr(after)) {
		for (Token *t = tok_next(open); t && t != close; t = tok_next(t)) {
			Token *call_open = skip_noise(tok_next(t));
			if (!is_valid_varname(t) || !call_open || !match_ch(call_open, '(') || !tok_match(call_open))
				continue;
			if (!params_look_like_decls(call_open)) continue;

			*name_out = t;
			*params_open_out = call_open;
			*params_close_out = tok_match(call_open);
			*next_out = after;
			return true;
		}
	}

	// Pattern 2: _Generic(...(cast)name)(decl-params) ;/attr
	// glibc-style: function name inside _Generic, params follow outside.
	if (match_ch(after, '(') && tok_match(after) && params_look_like_decls(after)) {
		Token *ext_close = tok_match(after);
		Token *after_ext = skip_noise(tok_next(ext_close));
		if (after_ext && (match_ch(after_ext, ';') || match_ch(after_ext, ',') ||
		    (after_ext->tag & TT_ATTR) || is_c23_attr(after_ext))) {
			Token *found = NULL;
			for (Token *t = tok_next(open); t && t != close; t = tok_next(t)) {
				if (is_valid_varname(t)) found = t;
			}
			if (found) {
				*name_out = found;
				*params_open_out = after;
				*params_close_out = ext_close;
				*next_out = after_ext;
				return true;
			}
		}
	}

	return false;
}

static bool generic_member_rewrite_target(Token *generic_tok, Token **name_out,
					  Token **args_open_out,
					  Token **args_close_out,
					  Token **next_out) {
	Token *open = tok_next(generic_tok);
	if (!open || !match_ch(open, '(') || !tok_match(open)) return false;

	Token *close = tok_match(open);
	Token *after = skip_noise(tok_next(close));
	if (!after) return false;

	for (Token *t = tok_next(open); t && t != close; t = tok_next(t)) {
		Token *call_open = skip_noise(tok_next(t));
		if (!is_valid_varname(t) || !call_open || !match_ch(call_open, '(') || !tok_match(call_open))
			continue;
		if (params_look_like_decls(call_open)) continue;

		*name_out = t;
		*args_open_out = call_open;
		*args_close_out = tok_match(call_open);
		*next_out = after;
		return true;
	}

	return false;
}

// ============================================================================
// Zero-init declaration parsing helpers
// ============================================================================

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
		} else {
			break;
		}
	}
	return tok;
}

// Parse type specifier: qualifiers, type keywords, struct/union/enum, typeof, _Atomic.
static TypeSpecResult parse_type_specifier(Token *tok) {
	TypeSpecResult r = { .end = tok };

	while (tok && tok->kind != TK_EOF) {
		Token *next = skip_noise(tok);
		if (next != tok) { tok = next; r.end = tok; continue; }

		uint32_t tag = tok->tag;
		bool is_type = is_type_keyword(tok);
		if (!(tag & (TT_QUALIFIER | TT_STORAGE)) && !is_type && !(tag & (TT_BITINT | TT_ALIGNAS))) break;
		
		if (equal(tok, "void") || is_void_typedef(tok)) r.has_void = true;
		
		bool had_type = r.saw_type;

		if ((tag & TT_STORAGE) && equal(tok, "extern")) r.has_extern = true;

		if (tag & TT_QUALIFIER) {
			if (tag & TT_VOLATILE) r.has_volatile = true;
			if (tag & TT_REGISTER) r.has_register = true;
			if (tag & TT_CONST) r.has_const = true;
			if (tag & TT_TYPE) {
				if (equal(tok, "auto")) r.saw_type = true;
				else r.has_atomic = true;
			}
		}

		// Bare _Atomic (not _Atomic(type)) is only a qualifier, not a type specifier.
		if (is_type && (tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE)
		    && !(tok_next(tok) && match_ch(tok_next(tok), '(')))
			is_type = false;
		if (is_type) r.saw_type = true;
		is_type = false;

		// _Atomic(type) specifier form (must precede generic attr/alignas handling)
		if ((tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE) && tok_next(tok) &&
		    match_ch(tok_next(tok), '(')) {
			r.saw_type = true;
			r.has_atomic = true;
			tok = tok_next(tok);
			Token *inner_start = tok_next(tok);
			tok = skip_balanced(tok, '(', ')');
			if (inner_start && (inner_start->tag & TT_SUE)) r.is_struct = true;
			if (inner_start && is_identifier_like(inner_start) && is_known_typedef(inner_start))
				r.is_typedef = true;
			r.end = tok;
			continue;
		}

		// struct/union/enum
		if (tag & TT_SUE) {
			r.is_struct = true;
			if (tok_loc(tok)[0] == 'e') r.is_enum = true;
			r.saw_type = true;
			tok = tok_next(tok);
			while (tok && tok->kind != TK_EOF) {
				Token *next = skip_noise(tok);
				if (next != tok) { tok = next; continue; }
				if (tok->tag & TT_QUALIFIER) tok = tok_next(tok);
				else break;
			}
			Token *sue_tag = NULL;
			if (tok && is_valid_varname(tok)) { sue_tag = tok; tok = tok_next(tok); }
			if (tok && match_ch(tok, '{')) {
				if (struct_body_contains_vla(tok)) {
					r.is_vla = true;
					if (sue_tag)
						typedef_add_vla_var(tok_loc(sue_tag), sue_tag->len, ctx->block_depth);
				}
				tok = skip_balanced(tok, '{', '}');
			} else if (sue_tag && is_vla_typedef(sue_tag)) {
				r.is_vla = true;
			}
			r.end = tok;
			continue;
		}

		// typeof/typeof_unqual/__typeof__
		if (tag & TT_TYPEOF) {
			bool is_unqual = equal(tok, "typeof_unqual");
			r.saw_type = true;
			r.has_typeof = true;
			tok = tok_next(tok);
			if (tok && match_ch(tok, '(')) {
				Token *end = walk_balanced(tok, false);
				if (tok_next(tok) && equal(tok_next(tok), "void") && tok_next(tok_next(tok)) == tok_match(tok)) r.has_void = true;
				if (!is_unqual)
					for (Token *t = tok_next(tok); t && t != end; t = tok_next(t)) {
						if (t->tag & TT_VOLATILE) r.has_volatile = true;
						if (t->tag & TT_CONST) r.has_const = true;
						// _Atomic is TT_QUALIFIER | TT_TYPE
						if ((t->tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE))
							r.has_atomic = true;
					}
				// Detect VLA inside typeof: array dimensions with runtime variables,
				// or references to known VLA variables.
				// Only treat '[' as an array dimension if it follows a type keyword,
				// typedef, pointer, or another ']' — not an expression like arr[x].
				// Track paren depth: if depth > 1, we're inside a nested (...)
				// such as a function pointer parameter list, so ignore VLAs there.
				int typeof_paren_depth = 1;
				Token *prev = tok; // '(' itself
				for (Token *t = tok_next(tok); t && t != end; prev = t, t = tok_next(t)) {
					if (match_ch(t, '(')) typeof_paren_depth++;
					else if (match_ch(t, ')')) typeof_paren_depth--;
					if (typeof_paren_depth == 1 && match_ch(t, '[') &&
					    (is_type_keyword(prev) || is_known_typedef(prev) ||
					     match_ch(prev, ']') || match_ch(prev, '*')) &&
					    array_size_is_vla(t)) {
						r.is_vla = true;
						break;
					}
					if (typeof_paren_depth == 1 && is_identifier_like(t) && (typedef_flags(t) & TDF_VLA)) {
						r.is_vla = true;
						break;
					}
				}
				tok = end;
			}
			r.end = tok;
			continue;
		}

		if (tag & (TT_BITINT | TT_ATTR | TT_ALIGNAS)) {
			if (tag & TT_BITINT) r.saw_type = true;
			tok = tok_next(tok);
			if (tok && match_ch(tok, '(')) tok = skip_balanced(tok, '(', ')');
			r.end = tok;
			continue;
		}

		int tflags = typedef_flags(tok);
		if ((tflags & TDF_TYPEDEF) || is_typedef_like(tok)) {
			// After a concrete type, a typedef name is a declarator, not part of the type.
			if (had_type) break;
			r.is_typedef = true;
			if (tflags & TDF_VLA) r.is_vla = true;
			if (tflags & TDF_AGGREGATE) r.is_struct = true;
			Token *peek = tok_next(tok);
			while (peek && (peek->tag & TT_QUALIFIER)) peek = tok_next(peek);
			if (peek && is_valid_varname(peek)) {
				Token *after = tok_next(peek);
				if (after && (match_ch(after, ';') || match_ch(after, '[') || match_ch(after, ',') ||
					      match_ch(after, '='))) {
					tok = tok_next(tok);
					r.end = tok;
					r.saw_type = true;
					return r;
				}
			}
		}

		tok = tok_next(tok);
		r.end = tok;
	}

	return r;
}

// Walk a balanced token group between matching delimiters, optionally emitting.
static Token *walk_balanced(Token *tok, bool emit) {
	Token *end = tok_match(tok);
	if (!end) return tok_next(tok);
	if (emit) {
		for (Token *t = tok; t != tok_next(end) && t->kind != TK_EOF; t = tok_next(t))
			emit_tok(t);
	}
	return tok_next(end);
}

// Walk a balanced group, transforming any top-level orelse into a ternary.
// Rejects orelse with control-flow action; transforms value fallback to (LHS) ? (LHS) : (RHS).

// Like emit_token_range but rewrites orelse → ternary within the flat range.
static void emit_token_range_orelse(Token *start, Token *end) {
	Token *orelse = NULL;
	Token *prev = NULL;
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = tok_next(t)) {
		if (t->flags & TF_OPEN) { prev = tok_match(t); t = tok_match(t); continue; }
		if ((t->tag & TT_ORELSE) && !is_known_typedef(t) && !typedef_lookup(t) &&
		    !(prev && (prev->tag & TT_MEMBER))) {
			orelse = t;
			break;
		}
		prev = t;
	}
	if (!orelse) { emit_token_range(start, end); return; }
	Token *rhs = tok_next(orelse);
	OUT_LIT("(");
	emit_token_range(start, orelse);
	OUT_LIT(") ? (");
	emit_token_range(start, orelse);
	OUT_LIT(") : (");
	emit_token_range_orelse(rhs, end);
	OUT_LIT(")");
}

// Check if a declarator's array brackets contain orelse at depth 0.
static bool declarator_has_bracket_orelse(Token *start, Token *end) {
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = tok_next(t)) {
		if (!match_ch(t, '[')) continue;
		Token *close = tok_match(t);
		if (!close) continue;
		Token *prev = t;
		for (Token *s = tok_next(t); s && s != close; s = tok_next(s)) {
			if (s->flags & TF_OPEN) { prev = tok_match(s); s = tok_match(s); continue; }
			if ((s->tag & TT_ORELSE) && !is_known_typedef(s) && !typedef_lookup(s) &&
			    !(prev && (prev->tag & TT_MEMBER)))
				return true;
			prev = s;
		}
		t = close;
	}
	return false;
}

// Pre-scan a declarator for bracket orelse, emit hoisted temp declarations.
// Each bracket orelse gets: long long _Prism_oe_ID = (LHS);
static void emit_bracket_orelse_temps(Token *start, Token *end) {
	ctx->bracket_oe_count = 0;
	ctx->bracket_oe_next = 0;
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = tok_next(t)) {
		if (!match_ch(t, '[')) continue;
		Token *close = tok_match(t);
		if (!close) continue;
		Token *orelse_found = NULL;
		Token *prev = t;
		for (Token *s = tok_next(t); s && s != close; s = tok_next(s)) {
			if (s->flags & TF_OPEN) { prev = tok_match(s); s = tok_match(s); continue; }
			if ((s->tag & TT_ORELSE) && !is_known_typedef(s) && !typedef_lookup(s) &&
			    !(prev && (prev->tag & TT_MEMBER))) {
				orelse_found = s;
				break;
			}
			prev = s;
		}
		if (!orelse_found) { t = close; continue; }
		ARENA_ENSURE_CAP(&ctx->main_arena, ctx->bracket_oe_ids, ctx->bracket_oe_count,
				 ctx->bracket_oe_cap, 16, unsigned);
		unsigned oe = ctx->ret_counter++;
		ctx->bracket_oe_ids[ctx->bracket_oe_count++] = oe;
		OUT_LIT(" long long _Prism_oe_");
		out_uint(oe);
		OUT_LIT(" = (");
		emit_token_range(tok_next(t), orelse_found);
		OUT_LIT(");");
		t = close;
	}
}

static Token *walk_balanced_orelse(Token *tok) {
	Token *end = tok_match(tok);
	if (!end) { emit_tok(tok); return tok_next(tok); }
	// Scan for orelse inside at depth 0 relative to the outer delimiters
	Token *orelse_found = NULL;
	Token *prev = tok;
	for (Token *t = tok_next(tok); t && t != end; t = tok_next(t)) {
		if (t->flags & TF_OPEN) { prev = tok_match(t); t = tok_match(t); continue; }
		if ((t->tag & TT_ORELSE) && !is_known_typedef(t) && !typedef_lookup(t) &&
		    !(prev && (prev->tag & TT_MEMBER))) {
			orelse_found = t;
			break;
		}
		prev = t;
	}
	if (!orelse_found) {
		// No orelse at depth 0 — but nested brackets may contain orelse.
		// Use orelse-aware emission for nested '[' groups.
		for (Token *t = tok; t != tok_next(end) && t->kind != TK_EOF;) {
			if (t != tok && t != end && match_ch(t, '[') && (t->flags & TF_OPEN) && tok_match(t)) {
				t = walk_balanced_orelse(t);
				continue;
			}
			emit_tok(t);
			t = tok_next(t);
		}
		return tok_next(end);
	}
	// Reject control-flow actions inside brackets/typeof
	Token *action = tok_next(orelse_found);
	if (action && (action->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)))
		error_tok(orelse_found, "'orelse' with control flow cannot be used inside "
			  "array dimensions or typeof expressions");
	if (action && match_ch(action, '{'))
		error_tok(orelse_found, "'orelse' block form cannot be used inside "
			  "array dimensions or typeof expressions");
	// Emit: OPEN (LHS) ? (LHS) : (RHS) CLOSE
	Token *lhs_start = tok_next(tok);    // first token after [ or (
	Token *rhs_start = tok_next(orelse_found); // first token after orelse
	bool is_bracket = match_ch(tok, '[');
	emit_tok(tok); // emit [ or (
	if (is_bracket && ctx->bracket_oe_next < ctx->bracket_oe_count) {
		// Use pre-hoisted temp variable (emitted before the declaration)
		unsigned oe = ctx->bracket_oe_ids[ctx->bracket_oe_next++];
		OUT_LIT(" _Prism_oe_");
		out_uint(oe);
		OUT_LIT(" ? _Prism_oe_");
		out_uint(oe);
		OUT_LIT(" : (");
		emit_token_range_orelse(rhs_start, end);
		OUT_LIT(")");
	} else if (is_bracket) {
		// Fallback for brackets not covered by pre-hoisting (e.g. typeof):
		// Reject if LHS has side effects — duplication would fire them twice.
		for (Token *s = lhs_start; s && s != orelse_found && s->kind != TK_EOF; s = tok_next(s)) {
			if (s->flags & TF_OPEN) { s = tok_match(s) ? tok_match(s) : s; continue; }
			if (equal(s, "++") || equal(s, "--"))
				error_tok(s, "'orelse' in array dimension / typeof with side effect "
					  "in the LHS (would be evaluated twice); "
					  "hoist the expression to a variable first");
			if (is_assignment_operator_token(s))
				error_tok(s, "'orelse' in array dimension / typeof with side effect "
					  "in the LHS (would be evaluated twice); "
					  "hoist the expression to a variable first");
			if (is_valid_varname(s) && !is_type_keyword(s) && tok_next(s) &&
			    tok_next(s) != orelse_found && match_ch(tok_next(s), '('))
				error_tok(s, "'orelse' in array dimension / typeof with side effect "
					  "in the LHS (would be evaluated twice); "
					  "hoist the expression to a variable first");
		}
		// emit ternary with LHS duplication (no statement expression)
		OUT_LIT(" (");
		emit_token_range(lhs_start, orelse_found);
		OUT_LIT(") ? (");
		emit_token_range(lhs_start, orelse_found);
		OUT_LIT(") : (");
		emit_token_range_orelse(rhs_start, end);
		OUT_LIT(")");
	} else {
		OUT_LIT(" (");
		emit_token_range(lhs_start, orelse_found);
		OUT_LIT(") ? (");
		emit_token_range(lhs_start, orelse_found);
		OUT_LIT(") : (");
		emit_token_range_orelse(rhs_start, end);
		OUT_LIT(")");
	}
	emit_tok(end); // emit ] or )
	return tok_next(end);
}

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
		if (!match_ch(peek, '*') && !match_ch(peek, '(') && !is_valid_varname(peek)) {
			r.end = NULL; return r;
		}
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
			if (match_ch(tok, '(')) tok = walk_balanced(tok, emit);
			else { r.is_array = true; r.paren_array = true; tok = decl_array_dims(tok, emit, &is_vla); }
		}
		if (!match_ch(tok, ')')) { r.end = NULL; return r; }
		decl_emit(tok, emit); tok = tok_next(tok);
		nested_paren--;
	}

	if (match_ch(tok, '(')) {
		if (!r.has_paren) { r.end = NULL; return r; }
		r.is_func_ptr = true;
		tok = walk_balanced(tok, emit);
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
	bool use_loop = has_volatile || cc_is_msvc(ctx->extra_compiler);
	for (int i = 0; i < count; i++) {
		// Byte loop for volatile (memset drops volatile) and MSVC (no __builtin_memset).
		if (use_loop) {
			OUT_LIT(" { ");
			out_str(vol, vol_len);
			OUT_LIT("char *_Prism_p_");
			out_uint(ctx->ret_counter);
			OUT_LIT(" = (");
			out_str(vol, vol_len);
			if (has_const)
				OUT_LIT("char *)(void *)&");
			else
				OUT_LIT("char *)&");
			out_str(tok_loc(vars[i]), vars[i]->len);
			OUT_LIT("; for (unsigned long long _Prism_i_");
			out_uint(ctx->ret_counter);
			OUT_LIT(" = 0; _Prism_i_");
			out_uint(ctx->ret_counter);
			OUT_LIT(" < sizeof(");
			out_str(tok_loc(vars[i]), vars[i]->len);
			OUT_LIT("); _Prism_i_");
			out_uint(ctx->ret_counter);
			OUT_LIT("++) _Prism_p_");
			out_uint(ctx->ret_counter);
			OUT_LIT("[_Prism_i_");
			out_uint(ctx->ret_counter);
			OUT_LIT("] = 0; }");
			ctx->ret_counter++;
		} else {
			if (has_const)
				OUT_LIT(" __builtin_memset((void *)&");
			else
				OUT_LIT(" __builtin_memset(&");
			out_str(tok_loc(vars[i]), vars[i]->len);
			OUT_LIT(", 0, sizeof(");
			out_str(tok_loc(vars[i]), vars[i]->len);
			OUT_LIT("));");
		}
	}
}

// Register typedef shadows and VLA variables for function parameters.
static inline void register_one_param(Token *ident, Token *param_start, Token *end) {
	if (!ident) return;
	if (is_known_typedef(ident) || is_typedef_heuristic(ident))
		typedef_add_shadow(tok_loc(ident), ident->len, 1);
	for (Token *s = param_start; s && s != end && s->kind != TK_EOF; s = tok_next(s))
		if (match_ch(s, '[') && array_size_is_vla(s))
			{ typedef_add_vla_var(tok_loc(ident), ident->len, 1); break; }
}
static void register_param_shadows(Token *open_paren, Token *close_paren) {
	if (!open_paren || !close_paren) return;
	Token *last_ident = NULL;
	Token *param_start = tok_next(open_paren);
	for (Token *t = tok_next(open_paren); t && t != close_paren && t->kind != TK_EOF; t = tok_next(t)) {
		if (t->flags & TF_OPEN) {
			if (!last_ident && match_ch(t, '(') && tok_match(t))
				for (Token *s = tok_next(t); s != tok_match(t); s = tok_next(s)) {
					if (s->flags & TF_OPEN) { s = tok_match(s) ? tok_match(s) : s; continue; }
					if (is_valid_varname(s) && !(s->tag & (TT_QUALIFIER|TT_TYPE|TT_SUE|TT_TYPEOF|TT_ATTR)))
						last_ident = s;
				}
			if (tok_match(t)) t = tok_match(t);
			continue;
		}
		if (is_valid_varname(t) && !(t->tag & (TT_QUALIFIER|TT_TYPE|TT_SUE|TT_TYPEOF|TT_ATTR)))
			last_ident = t;
		if (match_ch(t, ',') || tok_next(t) == close_paren) {
			register_one_param(last_ident, param_start, tok_next(t) == close_paren ? close_paren : t);
			param_start = tok_next(t);
			last_ident = NULL;
		}
	}
	register_one_param(last_ident, param_start, close_paren);
}

// Register typedef shadows and VLA variables for a declarator
static inline void register_decl_shadows(Token *var_name, bool effective_vla) {
	int depth = ctx->block_depth + (in_for_init() ? 1 : 0);
	if (is_known_typedef(var_name) || is_typedef_heuristic(var_name) ||
	    (var_name->tag & (TT_DEFER | TT_ORELSE)))
		typedef_add_shadow(tok_loc(var_name), var_name->len, depth);
	if (effective_vla && var_name) typedef_add_vla_var(tok_loc(var_name), var_name->len, depth);
}



// Emit break/continue with defer handling. Returns next token.
static Token *emit_break_continue_defer(Token *tok) {
	bool is_break = tok->tag & TT_BREAK;
	mark_switch_control_exit(true);
	if (FEAT(F_DEFER) && control_flow_has_defers(is_break))
		emit_defers(is_break ? DEFER_BREAK : DEFER_CONTINUE);
	out_char(' '); out_str(tok_loc(tok), tok->len); out_char(';');
	tok = tok_next(tok);
	if (match_ch(tok, ';')) tok = tok_next(tok);
	return tok;
}

// Emit goto with defer handling. Returns next token.
static Token *emit_goto_defer(Token *tok) {
	mark_switch_control_exit(false);
	tok = tok_next(tok);
	if (FEAT(F_DEFER) && is_identifier_like(tok)) {
		LabelInfo *info = label_find(tok);
		int td = info ? info->scope_depth : ctx->block_depth;
		if (goto_has_defers(td)) emit_goto_defers(td);
	}
	OUT_LIT(" goto ");
	if (is_identifier_like(tok)) { out_str(tok_loc(tok), tok->len); tok = tok_next(tok); }
	out_char(';');
	if (match_ch(tok, ';')) tok = tok_next(tok);
	return tok;
}

// const + fallback orelse: roll back speculative output, re-emit with temp variable.
// MSVC-compatible: instead of "const T x = val ?: fallback;",
// emit: "T _Prism_oe_N = (val); if (!_Prism_oe_N) _Prism_oe_N = (fallback); const T x = _Prism_oe_N;"
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
			if (t->tag & TT_TYPEOF) { has_const_typedef = true; break; }
		}
	}

	if (has_const_typedef) {
		// Pointer-to-incomplete: use &* trick instead of arithmetic.
		bool const_td_is_ptr = false;
		bool const_td_is_array = false;
		for (Token *t = type_start; t != type->end; t = tok_next(t)) {
			if (is_ptr_typedef(t)) { const_td_is_ptr = true; break; }
			if (is_array_typedef(t)) { const_td_is_array = true; break; }
		}

		if (pragma_start != type_start)
			emit_range(pragma_start, type_start);
		if (const_td_is_ptr) {
			OUT_LIT(" __typeof__(&*(");
			emit_type_stripped(type_start, type->end, strip_type_const);
			OUT_LIT(")0)");
		} else if (const_td_is_array) {
			OUT_LIT(" __typeof__(*(0 ? (");
			emit_type_stripped(type_start, type->end, strip_type_const);
			OUT_LIT("*)0 : (");
			emit_type_stripped(type_start, type->end, strip_type_const);
			OUT_LIT("*)0))");
		} else {
			OUT_LIT(" __typeof__((");
			emit_type_stripped(type_start, type->end, strip_type_const);
			OUT_LIT(")0 + 0)");
		}
	} else {
		if (pragma_start != type_start)
			emit_range(pragma_start, type_start);
		emit_type_stripped(type_start, type->end, strip_type_const);
	}
	// Emit declarator prefix, stripping const for mutability.
	for (Token *t = decl_start; t != decl->var_name; t = tok_next(t)) {
		if (t->tag & TT_CONST) continue;
		emit_tok(t);
	}
	
	OUT_LIT(" _Prism_oe_");
	out_uint(oe_id);
	emit_range(tok_next(decl->var_name), decl->end);
	OUT_LIT(" = (");
	emit_range(val_start, orelse_tok);
	OUT_LIT(");");

	// Emit fallback: chained orelse adds if-assignments; control flow gets a block.
	for (;;) {
		if (tok->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) {
			OUT_LIT(" if (!_Prism_oe_");
			out_uint(oe_id);
			OUT_LIT(") {");
			if (tok->tag & TT_RETURN) {
				mark_switch_control_exit(false);
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

		// Ternary instead of if-assignment to preserve compound literal lifetime.
		OUT_LIT(" _Prism_oe_");
		out_uint(oe_id);
		OUT_LIT(" = _Prism_oe_");
		out_uint(oe_id);
		OUT_LIT(" ? _Prism_oe_");
		out_uint(oe_id);
		OUT_LIT(" : (");

		bool chained = false;
		while (tok->kind != TK_EOF) {
			if (tok->flags & TF_OPEN) {
				if (match_ch(tok, '(') && tok_next(tok) && match_ch(tok_next(tok), '{'))
					error_tok(tok, "GNU statement expressions in orelse fallback values are not "
						  "supported; use 'orelse { ... }' block form instead");
				tok = walk_balanced(tok, true);
				continue;
			}
			if (match_ch(tok, ';') || (stop_comma && tok == stop_comma))
				break;
			if (is_orelse_keyword(tok)) {
				chained = true;
				tok = tok_next(tok);
				break;
			}
			emit_tok(tok);
			tok = tok_next(tok);
		}
		OUT_LIT(");");
		if (!chained) break;
	}

	// Emit final const declaration: "const T declarator = _Prism_oe_N;"
	if (pragma_start != type_start)
		emit_range(pragma_start, type_start);
	emit_range(type_start, type->end);
	parse_declarator(decl_start, true);

	OUT_LIT(" = _Prism_oe_");
	out_uint(oe_id);
	out_char(';');
	return tok;
}

static void check_orelse_in_parens(Token *open) {
	Token *close = tok_match(open);
	for (Token *pi = open, *t = tok_next(open); t != close; pi = t, t = tok_next(t)) {
		if ((t->tag & TT_ORELSE) && !is_known_typedef(t) && !typedef_lookup(t) && !(pi->tag & TT_MEMBER))
			error_tok(t, "'orelse' cannot be used inside parentheses "
				  "(it must appear at the top level of a declaration)");
		if (FEAT(F_DEFER) && (t->tag & TT_DEFER) && !is_known_typedef(t) && !typedef_lookup(t) &&
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

static bool has_effective_const_qual(Token *type_start, TypeSpecResult *type, DeclResult *decl) {
	bool has_const_qual = (type->has_const && !decl->is_func_ptr) || decl->is_const;
	// typeof() may carry implicit const from its argument (e.g. typeof(const_var)).
	// Route through the const-stripping path to be safe; the __typeof__((T)0 + 0)
	// wrapper is a no-op for non-const types.
	if (type->has_typeof && !decl->is_func_ptr && !decl->is_pointer)
		has_const_qual = true;
	if (!has_const_qual && !decl->is_func_ptr && !decl->is_pointer) {
		for (Token *t = type_start; t && t != type->end; t = tok_next(t))
			if (is_const_typedef(t)) { has_const_qual = true; break; }
	}
	return has_const_qual;
}

static inline void reject_orelse_array_target(DeclResult *decl) {
	if (decl->is_array && !decl->paren_pointer)
		error_tok(decl->var_name,
			  "orelse on array variable '%.*s' will never trigger "
			  "(array address is never NULL); remove the orelse clause",
			  decl->var_name->len,
			  tok_loc(decl->var_name));
}

static inline bool is_decl_struct_value(TypeSpecResult *type, DeclResult *decl) {
	return type->is_struct && !type->is_enum && !decl->is_pointer && !decl->is_array;
}

static inline void reject_orelse_in_for_init(Token *tok) {
	if (in_for_init())
		error_tok(tok, "orelse cannot be used in for-loop initializers");
}

static inline void require_orelse_action(Token *tok, Token *stop_comma) {
	if (match_ch(tok, ';') || (stop_comma && tok == stop_comma))
		error_tok(tok, "expected statement after 'orelse'");
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
		.is_struct_value = is_decl_struct_value(type, decl),
	};
	reject_orelse_array_target(decl);
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
			check_orelse_in_parens(scan);
			prev_scan = tok_match(scan);
			scan = tok_next(tok_match(scan));
			continue;
		}
		if (match_ch(scan, ',') || match_ch(scan, ';')) break;
		if (match_ch(scan, '?')) { ternary++; prev_scan = scan; scan = tok_next(scan); continue; }
		if (match_ch(scan, ':') && ternary > 0) { ternary--; prev_scan = scan; scan = tok_next(scan); continue; }
		if ((scan->tag & TT_ORELSE) && !is_known_typedef(scan) && !typedef_lookup(scan) &&
		    !(prev_scan && (prev_scan->tag & TT_MEMBER)) && ternary == 0) {
			info.orelse_tok = scan;
			break;
		}
		prev_scan = scan;
		scan = tok_next(scan);
	}

	if (info.orelse_tok) {
		Token *action = tok_next(info.orelse_tok);
		bool is_fallback = action &&
		    !(action->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) &&
		    !match_ch(action, '{') && !match_ch(action, ';');
		info.is_const_fallback = has_effective_const_qual(type_start, type, decl) && is_fallback;
	}

	return info;
}

// Process all declarators in a declaration and emit with zero-init.
static Token *process_declarators(Token *tok, TypeSpecResult *type, bool is_raw, Token *type_start,
				  Token *pragma_start, Token *raw_tok, bool brace_wrap) {
	Token **typeof_vars = NULL;
	int typeof_var_count = 0;
	int typeof_var_cap = 0;
	bool first_decl = true;
	bool need_type_emit = false; // Set after orelse comma — deferred to after next lookahead

	while (tok && tok->kind != TK_EOF) {
		Token *decl_start = tok;

		// Step 1: Non-emitting lookahead
		DeclResult decl = parse_declarator(tok, false);
		if (!decl.end || !decl.var_name) {
			if (!first_decl) {
				if (need_type_emit)
					emit_type_stripped(type_start, type->end, false);
				goto emit_raw_bail;
			}
			return NULL;
		}

		// Step 2: If initializer exists, scan for orelse + const fallback pattern
		OrelseInitInfo orelse_info = scan_decl_orelse(decl.end, type_start, type, &decl);
		bool is_const_orelse_fallback = orelse_info.is_const_fallback;

		// Step 2b: Pre-hoist bracket orelse temps (before type emission)
		bool has_bo = FEAT(F_ORELSE) && declarator_has_bracket_orelse(decl_start, decl.end);
		if (has_bo) {
			if (in_for_init())
				error_tok(decl_start,
					  "bracket orelse in VLA dimensions cannot be used in "
					  "for-loop initializers (hoisted temps would create "
					  "multiple declarations); move the declaration before the loop");
			emit_bracket_orelse_temps(decl_start, decl.end);
		}

		// Deferred type emission from orelse comma continuation
		if (need_type_emit) {
			if (!is_const_orelse_fallback) {
				if (pragma_start != type_start) {
					if (raw_tok)
						emit_range(pragma_start, raw_tok);
					else
						emit_range(pragma_start, type_start);
				}
				emit_type_stripped(type_start, type->end, false);
			}
			need_type_emit = false;
		}

		// Step 3: Emit base type for first declarator
		if (first_decl) {
			if (brace_wrap) OUT_LIT(" {");
			if (!is_const_orelse_fallback) {
				if (raw_tok && pragma_start != type_start) {
					emit_range(pragma_start, raw_tok);
				} else if (pragma_start != type_start) {
					emit_range(pragma_start, type_start);
				}
				emit_type_range_orelse(type_start, type->end);
			}
			first_decl = false;
		}

		// Step 4: Emit declarator & initializer
		bool effective_vla = (decl.is_vla && (!decl.paren_pointer || decl.paren_array)) || (type->is_vla && !decl.is_pointer);
		bool is_vla_type = decl.is_vla || type->is_vla;
		bool is_aggregate =
		    decl.is_array || ((type->is_struct || type->is_typedef) && !decl.is_pointer);
		bool needs_memset = !decl.has_init && !is_raw && (!decl.is_pointer || decl.is_array) &&
				    !type->has_register &&
				    (type->has_typeof || (type->has_atomic && is_aggregate) || effective_vla);

		if (is_const_orelse_fallback) {
			// const + fallback orelse: handle_const_orelse_fallback emits its own base type
			Token *orelse_tok = orelse_info.orelse_tok;
			Token *val_start = tok_next(decl.end); // First value token after '='
			tok = tok_next(orelse_tok); // skip 'orelse'
			OrelseDeclTargetInfo target = analyze_decl_orelse_target(tok, type_start, type, &decl);

			if (target.is_struct_value)
				error_tok(orelse_tok,
					  "orelse fallback on const struct is not supported; "
					  "use a control flow action (return/break/goto) or "
					  "remove const");

			register_decl_shadows(decl.var_name, is_vla_type);

			tok = handle_const_orelse_fallback(tok,
							   orelse_tok,
							   val_start,
							   decl_start,
							   &decl,
							   type_start,
							   type,
							   pragma_start,
							   target.stop_comma);

			flush_typeof_memsets(typeof_vars, &typeof_var_count, type);

			if (match_ch(tok, ';')) tok = tok_next(tok);
			end_statement_after_semicolon();

			if (target.stop_comma && match_ch(tok, ',')) {
				tok = tok_next(tok);
				need_type_emit = true;
				is_raw = false;
				continue;
			}
			if (brace_wrap) OUT_LIT(" }");
			return tok;
		}

		// Normal path: emit declarator
		parse_declarator(decl_start, true);
		tok = decl.end;

		// Add zero initializer if needed (for non-memset types)
		if (!decl.has_init && !effective_vla && !is_raw && !needs_memset && !type->has_extern) {
			// register + _Atomic aggregate: no safe zero-init path.
			if (type->has_register && type->has_atomic && is_aggregate)
				;
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
					 typeof_vars,
					 typeof_var_count + 1,
					 typeof_var_cap,
					 128,
					 Token *);
			typeof_vars[typeof_var_count++] = decl.var_name;
		}

		// Emit initializer if present
		if (decl.has_init) {
			bool hit_orelse = false;
			int init_ternary = 0;
			while (tok->kind != TK_EOF) {
				if (tok->flags & TF_OPEN) {
					if (FEAT(F_ORELSE)) check_orelse_in_parens(tok);
					tok = walk_balanced(tok, true);
					continue;
				}
				if (match_ch(tok, ',') || match_ch(tok, ';'))
					break;
				if (match_ch(tok, '?')) { init_ternary++; emit_tok(tok); tok = tok_next(tok); continue; }
				if (match_ch(tok, ':') && init_ternary > 0) { init_ternary--; emit_tok(tok); tok = tok_next(tok); continue; }
			// _Generic member rewrite: struct.strstr() → _Generic after '.'
				if ((tok->tag & TT_GENERIC) && last_emitted &&
				    (match_ch(last_emitted, '.') || equal(last_emitted, "->"))) {
					Token *name = NULL, *args_open = NULL, *args_close = NULL, *after = NULL;
					if (generic_member_rewrite_target(tok, &name, &args_open, &args_close, &after)) {
						emit_tok(name);
						emit_range(args_open, tok_next(args_close));
						last_emitted = args_close;
						tok = after;
						continue;
					}
				}
			// Detect 'orelse' at depth 0 in initializer
				if (FEAT(F_ORELSE) && (tok->tag & TT_ORELSE) &&
				    !is_known_typedef(tok) && !typedef_lookup(tok) &&
				    !(last_emitted && (last_emitted->tag & TT_MEMBER))) {
					if (init_ternary > 0)
						error_tok(tok, "'orelse' cannot be used inside a ternary expression");
					hit_orelse = true;
					break;
				}
				emit_tok(tok);
				tok = tok_next(tok);
			}

			if (hit_orelse) {
				reject_orelse_in_for_init(tok);

				out_char(';');

				flush_typeof_memsets(typeof_vars, &typeof_var_count, type);

				register_decl_shadows(decl.var_name, is_vla_type);

				tok = tok_next(tok); // skip 'orelse'
				OrelseDeclTargetInfo target = analyze_decl_orelse_target(tok, type_start, type, &decl);

				if (target.is_struct_value)
					error_tok(decl.var_name,
						  "orelse on struct/union values is not supported (memcmp "
						  "cannot reliably detect zero due to padding)");
				tok = emit_orelse_action(
				    tok, decl.var_name, target.has_const_qual, type->has_volatile, target.stop_comma);

				if (target.stop_comma && match_ch(tok, ',')) {
					tok = tok_next(tok);
					need_type_emit = true;
					is_raw = false;
					continue;
				}
				if (brace_wrap) OUT_LIT(" }");
				return tok;
			}
		}

		check_defer_var_shadow(decl.var_name);
		register_decl_shadows(decl.var_name, is_vla_type);

		if (match_ch(tok, ';')) {
			emit_tok(tok);
			flush_typeof_memsets(typeof_vars, &typeof_var_count, type);
			if (brace_wrap) OUT_LIT(" }");
			return tok_next(tok);
		} else if (match_ch(tok, ',')) {
			bool split_decl = false;
			Token *next_decl_tok = tok_next(tok);

			// Delayed memset zeroing must happen before a later declarator's
			// initializer can read earlier variables, and before the fixed queue fills.
			if (typeof_var_count > 0 && !in_for_init()) {
				DeclResult next_decl = parse_declarator(next_decl_tok, false);
				if (next_decl.end && next_decl.var_name && next_decl.has_init)
					split_decl = true;
			}

			// Split when the next declarator has bracket orelse, so
			// hoisted temps appear between the declarations.
			if (!split_decl && FEAT(F_ORELSE) && !in_for_init()) {
				DeclResult next_decl = parse_declarator(next_decl_tok, false);
				if (next_decl.end && declarator_has_bracket_orelse(next_decl_tok, next_decl.end))
					split_decl = true;
			}

			if (split_decl) {
				out_char(';');
				flush_typeof_memsets(typeof_vars, &typeof_var_count, type);
				tok = next_decl_tok;
				need_type_emit = true;
				is_raw = false;
				continue;
			}

			emit_tok(tok);
			tok = next_decl_tok;
			is_raw = false; // raw applies to first declarator only
		} else {
			if (!first_decl) goto emit_raw_bail;
			return NULL;
		}
	}
	return NULL;

emit_raw_bail:
	while (tok && tok->kind != TK_EOF && !match_ch(tok, ';')) { emit_tok(tok); tok = tok_next(tok); }
	if (tok && match_ch(tok, ';')) { emit_tok(tok); tok = tok_next(tok); }
	flush_typeof_memsets(typeof_vars, &typeof_var_count, type);
	if (brace_wrap) OUT_LIT(" }");
	return tok;
}

// Register shadows for file-scope _t/__* variable declarations.
static void register_toplevel_shadows(Token *tok) {
	if (tok->kind >= TK_STR) return;
	while (tok && tok->kind != TK_EOF) {
		if (tok->tag & TT_TYPEDEF) return;
		if (tok->tag & (TT_SKIP_DECL | TT_INLINE)) tok = tok_next(tok);
		else {
			Token *next = skip_noise(tok);
			if (next == tok) break;
			tok = next;
		}
	}
	if (!tok || tok->kind == TK_EOF) return;
	if (!(tok->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_BITINT)) && !is_known_typedef(tok)) return;

	TypeSpecResult type = parse_type_specifier(tok);
	if (!type.saw_type) return;

	Token *t = type.end;
	while (t && !match_ch(t, ';') && t->kind != TK_EOF) {
		DeclResult decl = parse_declarator(t, false);
		if (!decl.var_name || !decl.end) break;
		if (match_ch(decl.end, '(')) return; // function declaration, not variable

		if (is_typedef_heuristic(decl.var_name))
			typedef_add_shadow(tok_loc(decl.var_name), decl.var_name->len, 0);

		t = decl.end;
		if (match_ch(t, '=')) {
			t = tok_next(t);
			while (t && t->kind != TK_EOF) {
				if (t->flags & TF_OPEN) { t = walk_balanced(t, false); continue; }
				if (match_ch(t, ',') || match_ch(t, ';')) break;
				t = tok_next(t);
			}
		}
		if (t && match_ch(t, ',')) t = tok_next(t); else break;
	}
}

static bool has_storage_in(Token *from, Token *to) {
	for (Token *s = from; s && s != to; s = tok_next(s))
		if (s->tag & TT_STORAGE) return true;
	return false;
}

// Try to handle a declaration with zero-init. Returns token after declaration, or NULL.
static Token *try_zero_init_decl(Token *tok) {
	if (!FEAT(F_ZEROINIT) || ctx->block_depth <= 0 || in_struct_body()) return NULL;

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
	if ((tok->flags & TF_RAW) && !is_known_typedef(tok)) {
		Token *after_raw = skip_noise(tok_next(tok));
		if (is_raw_declaration_context(after_raw)) {
			is_raw = true;
			raw_tok = tok;
			start = tok_next(tok);
			tok = after_raw;
			if (pragma_start == raw_tok) pragma_start = start;
			warn_loc = after_raw;
		}
	}

	// Probe past qualifiers/attrs/storage to find 'raw' after prefix.
	if (!is_raw) {
		Token *probe = start;
		while (probe && probe->kind != TK_EOF) {
			Token *next = skip_noise(probe);
			if (next != probe) { probe = next; continue; }
			if (probe->tag & TT_QUALIFIER) { probe = tok_next(probe); continue; }
			if ((probe->tag & (TT_STORAGE | TT_TYPEDEF))) { probe = tok_next(probe); continue; }
			break;
		}
		if (probe && (probe->flags & TF_RAW) && !is_known_typedef(probe)) {
			Token *after_raw = skip_noise(tok_next(probe));
			if (is_raw_declaration_context(after_raw)) {
				if (has_storage_in(pragma_start, probe)) {
					emit_range(pragma_start, probe);
					return emit_raw_verbatim_to_semicolon(after_raw);
				}
				is_raw = true;
				raw_tok = probe;
				start = after_raw;
				tok = after_raw;
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

	if (!(tok->tag & TT_DECL_START) && !is_typedef_like(tok)) return NULL;

	TypeSpecResult type = parse_type_specifier(tok);
	if (!type.saw_type) return NULL;

	if (!is_var_declaration(type.end)) {
		// Detect GNU nested function definitions (break defer/return tracking).
		if (FEAT(F_DEFER)) {
			DeclResult decl = parse_declarator(type.end, false);
			if (decl.var_name) {
				Token *p = skip_noise(tok_next(decl.var_name));
				if (p && match_ch(p, '(') && tok_match(p)) {
					Token *a = tok_next(tok_match(p));
					while (a && (a->tag & (TT_ATTR | TT_ASM))) {
						a = (a->tag & TT_ASM) ? tok_next(a) : skip_noise(a);
						if (a && match_ch(a, '(') && tok_match(a)) a = tok_next(tok_match(a));
					}
					bool nested = a && match_ch(a, '{');
					if (!nested && a) { // K&R: type name(a,b) int a; int b; { ... }
						Token *b = a;
						while (b && b->kind != TK_EOF && !match_ch(b, '{') && !match_ch(b, '}'))
							b = (b->flags & TF_OPEN && tok_match(b)) ? tok_next(tok_match(b)) : tok_next(b);
						nested = b && match_ch(b, '{') && is_knr_params(tok_next(tok_match(p)), b);
					}
					if (nested)
						error_tok(decl.var_name,
							  "nested function definitions are not supported inside "
							  "functions using defer/zeroinit — move the function outside "
							  "or use a function pointer");
				}
			}
		}
		return NULL;
	}

	if (in_switch_scope_unbraced && !is_raw) {
		error_tok(warn_loc,
			  "variable declaration directly in switch body without braces. "
			  "Wrap in braces: 'case N: { int x; ... }' to ensure safe zero-initialization, "
			  "or use 'raw' to suppress zero-init.");
	}

	// Braceless control body: wrap in braces (orelse expands to multiple stmts).
	bool brace_wrap = ctrl_state.pending && ctrl_state.parens_just_closed;

	Token *result = process_declarators(type.end, &type, is_raw, start, pragma_start, raw_tok, brace_wrap);
	if (result && !is_raw && ctx->block_depth > 0) {
		ScopeNode *bt = scope_block_top();
		if (bt) bt->has_zeroinit_decl = true;
	}
	return result;
}

// Emit expression to semicolon, handling zero-init in statement expressions.
static Token *emit_expr_to_semicolon(Token *tok) {
	int brace_depth = 0;
	int ternary_depth = 0;
	bool expr_at_stmt_start = false;
	while (tok->kind != TK_EOF) {
		// Skip matched (...) / [...] groups — no declarations inside.
		// Exception: statement expressions ({...}) need inner zero-init processing.
		if ((match_ch(tok, '(') || match_ch(tok, '[')) && tok_match(tok)) {
			if (match_ch(tok, '(') && tok_next(tok) && match_ch(tok_next(tok), '{')) {
				emit_tok(tok);
				tok = tok_next(tok);
			} else {
					if (FEAT(F_ORELSE) && (match_ch(tok, '(') || match_ch(tok, '[')))
					check_orelse_in_parens(tok);
				tok = walk_balanced(tok, true);
			}
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

		if (match_ch(tok, '{') || match_ch(tok, ';') || match_ch(tok, '}'))
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

// Validate control flow keywords inside defer blocks are safe.
// Validation is statement-structured so loop/switch scope does not leak across siblings.
static inline Token *skip_defer_control_head(Token *tok) {
	tok = skip_noise(tok);
	if (tok && match_ch(tok, '(') && tok_match(tok)) return tok_next(tok_match(tok));
	return tok;
}

static Token *validate_defer_statement(Token *tok, bool in_loop, bool in_switch) {
	tok = skip_noise(tok);
	if (!tok || tok->kind == TK_EOF) return tok;

	if (match_ch(tok, '{')) {
		Token *end = tok_match(tok);
		for (tok = skip_noise(tok_next(tok)); tok && tok != end && tok->kind != TK_EOF; tok = skip_noise(tok)) {
			Token *next = validate_defer_statement(tok, in_loop, in_switch);
			if (next == tok) break;
			tok = next;
		}
		return end ? tok_next(end) : tok;
	}

	if (equal(tok, "if")) {
		Token *after_then = validate_defer_statement(skip_defer_control_head(tok_next(tok)), in_loop, in_switch);
		Token *else_tok = skip_noise(after_then);
		if (else_tok && equal(else_tok, "else"))
			return validate_defer_statement(tok_next(else_tok), in_loop, in_switch);
		return after_then;
	}

	if (tok->tag & (TT_CASE | TT_DEFAULT)) {
		while (tok && tok->kind != TK_EOF && !match_ch(tok, ':')) {
			if (tok->flags & TF_OPEN) tok = tok_match(tok) ? tok_next(tok_match(tok)) : tok_next(tok);
			else tok = tok_next(tok);
		}
		return tok && match_ch(tok, ':') ? validate_defer_statement(tok_next(tok), in_loop, in_switch) : tok;
	}

	if (tok->tag & TT_SWITCH)
		return validate_defer_statement(skip_defer_control_head(tok_next(tok)), in_loop, true);

	if (tok->tag & TT_LOOP) {
		if (equal(tok, "do")) {
			tok = validate_defer_statement(tok_next(tok), true, in_switch);
			Token *w = skip_noise(tok);
			if (w && equal(w, "while")) {
				tok = skip_defer_control_head(tok_next(w));
				tok = skip_noise(tok);
				if (tok && match_ch(tok, ';')) tok = tok_next(tok);
			}
			return tok;
		}
		return validate_defer_statement(skip_defer_control_head(tok_next(tok)), true, in_switch);
	}

	if (tok->flags & TF_OPEN) {
		// GNU statement expression ({...}): validate the inner block recursively
		if (match_ch(tok, '(') && tok_next(tok) && match_ch(tok_next(tok), '{')) {
			Token *inner_brace = tok_next(tok);
			validate_defer_statement(inner_brace, in_loop, in_switch);
			return tok_match(tok) ? tok_next(tok_match(tok)) : tok_next(tok);
		}
		return tok_match(tok) ? tok_next(tok_match(tok)) : tok_next(tok);
	}

	// Labeled statement: ident ':' <stmt>
	if (tok->kind == TK_IDENT && tok_next(tok) && match_ch(tok_next(tok), ':'))
		error_tok(tok, "labels inside defer blocks produce duplicate labels "
			  "when the defer body is copied to multiple exit points");

	if (tok->kind == TK_KEYWORD) {
		if (tok->tag & TT_RETURN)
			error_tok(tok, "'return' inside defer block bypasses remaining defers");
		if ((tok->tag & TT_GOTO) && !is_known_typedef(tok))
			error_tok(tok, "'goto' inside defer block could bypass remaining defers");
		if ((tok->tag & TT_BREAK) && !in_loop && !in_switch)
			error_tok(tok, "'break' inside defer block bypasses remaining defers");
		if ((tok->tag & TT_CONTINUE) && !in_loop)
			error_tok(tok, "'continue' inside defer block bypasses remaining defers");
		if ((tok->tag & TT_DEFER) && !is_known_typedef(tok) &&
		    !match_ch(tok_next(tok), ':') && !(tok_next(tok) && (tok_next(tok)->tag & TT_ASSIGN)))
			error_tok(tok, "nested defer is not supported");
	}

	// Scan for orelse with forbidden control flow before skipping
	if (FEAT(F_ORELSE)) {
		for (Token *s = tok; s && s->kind != TK_EOF && !match_ch(s, ';'); s = tok_next(s)) {
			if (s->flags & TF_OPEN) { s = tok_match(s); continue; }
			if ((s->tag & TT_ORELSE) && !is_known_typedef(s) && !typedef_lookup(s)) {
				Token *act = tok_next(s);
				if (act && (act->tag & TT_RETURN))
					error_tok(act, "'return' inside defer block bypasses remaining defers");
				if (act && (act->tag & TT_GOTO) && !is_known_typedef(act))
					error_tok(act, "'goto' inside defer block could bypass remaining defers");
				if (act && (act->tag & TT_BREAK) && !in_loop && !in_switch)
					error_tok(act, "'break' inside defer block bypasses remaining defers");
				if (act && (act->tag & TT_CONTINUE) && !in_loop)
					error_tok(act, "'continue' inside defer block bypasses remaining defers");
				break;
			}
		}
	}

	// Catch-all: walk to semicolon, recursively validating any GNU statement
	// expressions ({...}) encountered along the way.
	for (Token *s = tok; s && s->kind != TK_EOF && !match_ch(s, ';'); s = tok_next(s)) {
		if (s->flags & TF_OPEN) {
			if (match_ch(s, '(') && tok_next(s) && match_ch(tok_next(s), '{'))
				validate_defer_statement(tok_next(s), in_loop, in_switch);
			s = tok_match(s) ? tok_match(s) : s;
			continue;
		}
	}
	Token *semi = skip_to_semicolon(tok);
	return (semi && semi->kind != TK_EOF) ? tok_next(semi) : semi;
}

// Handle 'defer' keyword: validate context, record deferred statement.
// Returns next token after the defer statement, or NULL if tok is not a valid defer.
static Token *handle_defer_keyword(Token *tok) {
	if (!FEAT(F_DEFER)) return NULL;
	// Distinguish struct field, label, goto target, variable assignment, attribute usage
	if (match_ch(tok_next(tok), ':') || (last_emitted && (last_emitted->tag & (TT_MEMBER | TT_GOTO))) ||
	    (last_emitted && (is_type_keyword(last_emitted) || (last_emitted->tag & TT_TYPEDEF))) ||
	    is_known_typedef(tok) || typedef_lookup(tok) ||
	    (tok_next(tok) && (tok_next(tok)->tag & TT_ASSIGN)) || in_struct_body() ||
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
	// setjmp/longjmp/vfork/asm safety checks
	if (ctx->current_func_has_setjmp)
		error_tok(tok, "defer cannot be used in functions that call setjmp/longjmp/pthread_exit");
	if (ctx->current_func_has_vfork)
		error_tok(tok, "defer cannot be used in functions that call vfork()");
	if (ctx->current_func_has_asm)
		error_tok(tok, "defer cannot be used in functions containing inline assembly");
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
		validate_defer_statement(stmt_start, false, false);
		defer_add(defer_keyword, stmt_start, stmt_end);
		tok = after;
		// Consume optional trailing semicolon
		if (tok && match_ch(tok, ';')) tok = tok_next(tok);
		end_statement_after_semicolon();
		return tok;
	}

	Token *stmt_end = skip_to_semicolon(tok);

	if (stmt_end->kind == TK_EOF || !match_ch(stmt_end, ';'))
		error_tok(defer_keyword, "unterminated defer statement; expected ';'");

	// Validate inner control flow (returns, missing semicolons, breaks outside loops)
	if (stmt_start && stmt_start->kind == TK_KEYWORD &&
	    (stmt_start->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO | TT_IF | TT_LOOP |
				TT_SWITCH | TT_CASE | TT_DEFAULT | TT_DEFER)))
		error_tok(defer_keyword,
			  "defer statement appears to be missing ';' (found '%.*s' keyword inside)",
			  stmt_start->len, tok_loc(stmt_start));

	validate_defer_statement(stmt_start, false, false);

	defer_add(defer_keyword, stmt_start, stmt_end);
	tok = (stmt_end->kind != TK_EOF) ? tok_next(stmt_end) : stmt_end;
	end_statement_after_semicolon();
	return tok;
}

static inline bool is_void_return(Token *tok) {
	return ctx->current_func_returns_void || (match_ch(tok, '(') && tok_next(tok) && equal(tok_next(tok), "void") &&
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
				OUT_LIT(" _Prism_ret_"); out_uint(ctx->ret_counter); OUT_LIT(" = (");
			} else OUT_LIT(" (");

			if (stop)
				tok = emit_expr_to_stop(tok, stop, false);
			else
				tok = emit_expr_to_semicolon(tok);
			OUT_LIT(");");
			emit_all_defers();

			if (!is_void) { OUT_LIT(" return _Prism_ret_"); out_uint(ctx->ret_counter++); }
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

// Emit orelse action (fallback after 'orelse').
static inline void orelse_open(Token *var_name) {
	if (var_name) {
		OUT_LIT(" if (!");
		out_str(tok_loc(var_name), var_name->len);
		OUT_LIT(") {");
	} else
		OUT_LIT(" {");
}

static Token *emit_orelse_action(Token *tok, Token *var_name, bool has_const, bool has_volatile, Token *stop_comma) {
	require_orelse_action(tok, stop_comma);

	if (match_ch(tok, '{')) {
		if (var_name) {
			OUT_LIT(" if (!");
			out_str(tok_loc(var_name), var_name->len);
			out_char(')');
		}
		ctx->at_stmt_start = false;
		return tok;
	}

	if (tok->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) {
		uint64_t tag = tok->tag;
		if (tag & TT_RETURN) {
			mark_switch_control_exit(false);
			tok = tok_next(tok);
		}
		orelse_open(var_name);
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
		out_str(tok_loc(var_name), var_name->len);
		OUT_LIT(") ");
		out_str(tok_loc(var_name), var_name->len);
		OUT_LIT(" =");
	} else {
		out_char(' ');
		out_str(tok_loc(var_name), var_name->len);
		OUT_LIT(" = ");
		out_str(tok_loc(var_name), var_name->len);
		OUT_LIT(" ? ");
		out_str(tok_loc(var_name), var_name->len);
		OUT_LIT(" :");
	}
	while (tok->kind != TK_EOF) {
		if (tok->flags & TF_OPEN) {
			if (match_ch(tok, '(') && tok_next(tok) && match_ch(tok_next(tok), '{'))
				error_tok(tok, "GNU statement expressions in orelse fallback values are not "
					  "supported; use 'orelse { ... }' block form instead");
			tok = walk_balanced(tok, true);
			continue;
		}
		if (match_ch(tok, ';') || (stop_comma && tok == stop_comma))
			break;
		// Chained orelse: "x = a orelse b orelse c" → close this assignment,
		// then recursively handle the next orelse on the same variable.
		if (is_orelse_keyword(tok)) {
			out_char(';');
			tok = tok_next(tok);
			return emit_orelse_action(tok, var_name, has_const, has_volatile, stop_comma);
		}
		emit_tok(tok);
		tok = tok_next(tok);
	}
	out_char(';');
	if (match_ch(tok, ';')) tok = tok_next(tok);
	end_statement_after_semicolon();
	return tok;
}

// Handle 'return' with active defers: save expr, emit defers, then return.
// Returns next token if handled, or NULL to let normal emit proceed.
static Token *handle_return_defer(Token *tok) {
	mark_switch_control_exit(false);
	if (!has_active_defers()) return NULL;
	tok = tok_next(tok); // skip 'return'
	OUT_LIT(" {");
	tok = emit_return_body(tok, NULL);
	OUT_LIT(" }");
	end_statement_after_semicolon();
	return tok;
}

// Handle 'break' or 'continue' with active defers.
// Returns next token if handled, or NULL.
static Token *handle_break_continue_defer(Token *tok) {
	bool is_break = tok->tag & TT_BREAK;
	mark_switch_control_exit(true);
	if (!control_flow_has_defers(is_break)) return NULL;
	OUT_LIT(" {");
	tok = emit_break_continue_defer(tok);
	OUT_LIT(" }");
	end_statement_after_semicolon();
	return tok;
}

// Report goto skipping over a variable declaration (warn or error based on FEAT(F_WARN_SAFETY))
static void report_goto_skips_decl(Token *skipped_decl, Token *label_tok) {
	if (!FEAT(F_ZEROINIT)) return;
	const char *msg = "goto '%.*s' would skip over this variable declaration (bypasses zero-init)";
	if (FEAT(F_WARN_SAFETY)) warn_tok(skipped_decl, msg, label_tok->len, tok_loc(label_tok));
	else error_tok(skipped_decl, msg, label_tok->len, tok_loc(label_tok));
}

static void check_goto_decl_safety(GotoSkipResult *skip, Token *goto_tok, Token *label_tok, LabelInfo *info) {
	if (skip->skipped_decl) report_goto_skips_decl(skip->skipped_decl, label_tok);
	else {
		Token *back_decl = backward_goto_skips_decl(goto_tok, info);
		if (back_decl) report_goto_skips_decl(back_decl, label_tok);
	}
}

static LabelInfo *label_find(Token *tok) {
	for (int i = 0; i < label_count; i++)
		if (label_table[i].name_len == tok->len &&
		    !memcmp(label_table[i].name, tok_loc(tok), tok->len))
			return &label_table[i];
	return NULL;
}

// Handle 'goto': defer cleanup + zeroinit safety checks
static Token *handle_goto_keyword(Token *tok) {
	Token *goto_tok = tok;
	tok = tok_next(tok);

	if (FEAT(F_DEFER)) {
		mark_switch_control_exit(false);

		if (match_ch(tok, '*')) {
			if (has_active_defers())
				error_tok(goto_tok, "computed goto cannot be used with active defer statements");
			emit_tok(goto_tok);
			return tok;
		}

		if (is_identifier_like(tok)) {
			LabelInfo *info = label_find(tok);

			GotoSkipResult skip = goto_skips_check(goto_tok, info, true, true);
			if (skip.skipped_defer) {
				const char *msg = "goto '%.*s' would skip over this defer statement";
				if (FEAT(F_WARN_SAFETY)) warn_tok(skip.skipped_defer, msg, tok->len, tok_loc(tok));
				else error_tok(skip.skipped_defer, msg, tok->len, tok_loc(tok));
			}
			check_goto_decl_safety(&skip, goto_tok, tok, info);

			int target_depth = info ? info->scope_depth : ctx->block_depth;

			if (target_depth >= ctx->block_depth) {
				int exits = 0;
				if (info && tok_loc(info->tok) > tok_loc(goto_tok)) exits = goto_scope_exits(goto_tok, info->tok);
				else if (info) exits = goto_scope_exits(info->tok, goto_tok);

				if (exits > 0) {
					target_depth = ctx->block_depth - exits;
					if (target_depth < 0) target_depth = 0;
				}
			}

			if (goto_has_defers(target_depth)) {
				OUT_LIT(" {");
				emit_goto_defers(target_depth);
				OUT_LIT(" goto");
				emit_tok(tok);
				tok = tok_next(tok);
				if (match_ch(tok, ';')) {
					emit_tok(tok);
					tok = tok_next(tok);
				}
				OUT_LIT(" }");
				end_statement_after_semicolon();
				return tok;
			}
		}
		emit_tok(goto_tok);
		return tok;
	}

	if (FEAT(F_ZEROINIT) && is_identifier_like(tok)) {
		LabelInfo *info = label_find(tok);
		GotoSkipResult skip = goto_skips_check(goto_tok, info, false, true);
		check_goto_decl_safety(&skip, goto_tok, tok, info);
	}
	emit_tok(goto_tok);
	return tok;
}

static void handle_case_default(Token *tok) {
	if (!FEAT(F_DEFER)) return;
	int sd = find_switch_scope();
	if (sd < 0) return;
	bool is_case = tok->tag & TT_CASE;
	bool is_default = (tok->tag & TT_DEFAULT) && !in_generic();
	if (is_default) {
		Token *t = skip_noise(tok_next(tok));
		if (!t || !match_ch(t, ':')) return;
	}
	if (!is_case && !is_default) return;

	for (int d = ctx->scope_depth - 1; d >= sd; d--) {
		if (scope_stack[d].kind != SCOPE_BLOCK) continue;
		if (defer_count > scope_stack[d].defer_start_idx && !scope_stack[d].had_control_exit)
			error_tok(defer_stack[scope_stack[d].defer_start_idx].defer_kw,
				  "defer skipped by switch fallthrough at %s:%d",
				  tok_file(tok)->name,
				  tok_line_no(tok));
		defer_count = scope_stack[d].defer_start_idx;
		scope_stack[d].had_control_exit = false;
	}

	// case/default inside nested block may bypass zero-init'd declarations
	if (FEAT(F_ZEROINIT) && ctx->scope_depth > sd + 1) {
		for (int d = sd + 1; d < ctx->scope_depth; d++) {
			if (scope_stack[d].kind != SCOPE_BLOCK) continue;
			if (scope_stack[d].has_zeroinit_decl) {
				error_tok(tok,
					  "case/default label inside a nested block within a switch may bypass "
					  "zero-initialization (move the label to the switch body or wrap in its own block)");
			}
		}
	}

}

static Token *handle_sue_body(Token *tok) {
	Token *brace = find_struct_body_brace(tok);
	if (!brace) return NULL;

	emit_range(tok, brace);
	emit_tok(brace);
	tok = tok_next(brace);
	scope_push_kind(SCOPE_BLOCK, false);
	scope_stack[ctx->scope_depth - 1].is_struct = true;
	ctx->at_stmt_start = true;
	return tok;
}

static Token *handle_open_brace(Token *tok) {
	// Compound literal inside control parens or before body
	if (ctrl_state.pending && (in_ctrl_paren() || !ctrl_state.parens_just_closed)) {
		emit_tok(tok);
		ctrl_state.brace_depth++;
		return tok_next(tok);
	}
	if (ctrl_state.pending && !(ctrl_state.scope_flags & NS_SWITCH)) ctrl_state.scope_flags |= NS_CONDITIONAL;
	bool orelse_guard = ctrl_state.pending_orelse_guard;
	// Consume pending state but keep scope flags for scope_push
	ctrl_state.pending = false;
	ctrl_state.pending_for_paren = false;
	ctrl_state.parens_just_closed = false;
	ctrl_state.pending_orelse_guard = false;

	// Evaluate BEFORE emit_tok
	bool is_stmt_expr = last_emitted && match_ch(last_emitted, '(');

	emit_tok(tok);
	tok = tok_next(tok);
	scope_push_kind(SCOPE_BLOCK, true);

	// Tag the newly created scope directly
	if (is_stmt_expr) scope_stack[ctx->scope_depth - 1].is_stmt_expr = true;
	if (orelse_guard) scope_stack[ctx->scope_depth - 1].is_orelse_guard = true;

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
	while (ctx->scope_depth > 0 && scope_stack[ctx->scope_depth - 1].kind != SCOPE_BLOCK)
		scope_pop();
	typedef_pop_scope(ctx->block_depth);
	if (FEAT(F_DEFER) && ctx->scope_depth > 0) {
		ScopeNode *s = &scope_stack[ctx->scope_depth - 1];
		if (defer_count > s->defer_start_idx) {
			emit_defers(DEFER_SCOPE);
			defer_count = s->defer_start_idx;
		}
	}

	// Check guard BEFORE popping the scope
	bool close_guard = ctx->scope_depth > 0 && scope_stack[ctx->scope_depth - 1].is_orelse_guard;

	scope_pop();
	emit_tok(tok);

	// Close dangling-else guard brace if flagged
	if (close_guard) OUT_LIT(" }");

	tok = tok_next(tok);
	ctx->at_stmt_start = true;
	return tok;
}

// Build a copy of 'environ' with CC and PRISM_CC removed (cached)
static char **build_clean_environ(void) {
	if (cached_clean_env) return cached_clean_env;
	int n = 0;
	for (char **e = environ; *e; e++) n++;
	char **env = malloc((n + 1) * sizeof(char *));
	if (!env) return NULL;
	int j = 0;
	for (char **e = environ; *e; e++) {
		if (strncmp(*e, "CC=", 3) != 0 && strncmp(*e, "PRISM_CC=", 9) != 0) env[j++] = *e;
	}
	env[j] = NULL;
	cached_clean_env = env;
	return env;
}

static int wait_for_child(pid_t pid) {
	int status;
	if (waitpid(pid, &status, 0) == -1) {
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

// Like run_command but suppresses stderr (for probe attempts)
static int run_command_quiet(char **argv) {
	return spawn_command(argv, true);
}
#endif

// Create temp file with optional suffix. If source_adjacent is set, tries
// to create next to that file first, falls back to TMPDIR.
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
		} else {
			strcpy(dir_path, ".");
		}
		if (dir_has_write_bits(dir_path)) {
			if (slash) {
				int dir_len = (int)(slash - source_adjacent);
				n = snprintf(buf, bufsize, "%.*s/.%s.XXXXXX.c", dir_len, source_adjacent, slash + 1);
			} else
				n = snprintf(buf, bufsize, ".%s.XXXXXX.c", source_adjacent);
			suffix_len = 2;
			if (n >= 0 && (size_t)n < bufsize) {
				int fd = mkstemps(buf, suffix_len);
				if (fd >= 0) {
					close(fd);
					return 0;
				}
			}
		}
		// Source directory not writable, fall back to TMPDIR
		const char *base = slash ? slash + 1 : source_adjacent;
		n = snprintf(buf, bufsize, "%s.%s.XXXXXX.c", get_tmp_dir(), base);
		suffix_len = 2;
	} else
		n = snprintf(buf, bufsize, "%s%s", get_tmp_dir(), prefix ? prefix : "prism_tmp");
	if (n < 0 || (size_t)n >= bufsize) return -1;
	int fd = suffix_len > 0 ? mkstemps(buf, suffix_len) : mkstemp(buf);
	if (fd < 0) return -1;
	close(fd);
	return 0;
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
static const char *cc_executable(const char *cc) {
	if (!cc || !*cc) return cc;
	const char *p = cc;
	while (*p && *p != ' ' && *p != '\t') p++;
	if (*p == '\0') return cc;
	size_t len = (size_t)(p - cc);
	static char buf[PATH_MAX];
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
static int cc_extra_arg_count(const char *cc) {
	if (!cc || !*cc) return 0;
	int count = 0;
	const char *p = cc;
	while (*p == ' ' || *p == '\t') p++;
	while (*p && *p != ' ' && *p != '\t') p++;
	while (*p) {
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;
		count++;
		while (*p && *p != ' ' && *p != '\t') p++;
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

// Build preprocessor argv (shared between pipe and file paths)
static void build_pp_argv(const char **args, int *argc, const char *input_file, char **out_cc_dup) {
	const char *cc = ctx->extra_compiler ? ctx->extra_compiler : PRISM_DEFAULT_CC;
	cc_split_into_argv(args, argc, cc, out_cc_dup);
	args[(*argc)++] = "-E";
	args[(*argc)++] = "-w";

	for (int i = 0; i < ctx->extra_compiler_flags_count; i++)
		args[(*argc)++] = ctx->extra_compiler_flags[i];

	for (int i = 0; i < ctx->dep_flags_count; i++)
		args[(*argc)++] = ctx->dep_flags[i];

	for (int i = 0; i < ctx->extra_include_count; i++)
	{
		args[(*argc)++] = "-I";
		args[(*argc)++] = ctx->extra_include_paths[i];
	}

	for (int i = 0; i < ctx->extra_define_count; i++)
	{
		args[(*argc)++] = "-D";
		args[(*argc)++] = ctx->extra_defines[i];
	}

	args[(*argc)++] = "-D__PRISM__=1";
	if (FEAT(F_DEFER)) args[(*argc)++] = "-D__PRISM_DEFER__=1";
	if (FEAT(F_ZEROINIT)) args[(*argc)++] = "-D__PRISM_ZEROINIT__=1";

	// Add POSIX/GNU feature test macros unless user already defined them
#ifndef _WIN32
	{
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
		args[(*argc)++] = cc_is_msvc(args[0]) ? "/FI" : "-include";
		args[(*argc)++] = ctx->extra_force_includes[i];
	}

	args[(*argc)++] = input_file;
	args[*argc] = NULL;
}

// Run system preprocessor (cc -E) via pipe, returns malloc'd output or NULL
static char *preprocess_with_cc(const char *input_file) {
	const char *pp_cc = ctx->extra_compiler ? ctx->extra_compiler : PRISM_DEFAULT_CC;
	int argcap = 16 + cc_extra_arg_count(pp_cc) + ctx->extra_compiler_flags_count + ctx->dep_flags_count +
		     ctx->extra_include_count * 2 + ctx->extra_define_count * 2 + ctx->extra_force_include_count * 2;
	const char **args = alloc_argv(argcap);
	int argc = 0;
	char *cc_dup = NULL;
	build_pp_argv(args, &argc, input_file, &cc_dup);
	char **argv = (char **)args;

	// Set up pipe: child writes preprocessed output, we read it
	int pipefd[2];
	if (pipe(pipefd) == -1) {
		perror("pipe");
		free(cc_dup);
		free((void *)args);
		return NULL;
	}

	// Capture preprocessor stderr to a temp file for diagnostics on failure
	char pp_stderr_path[PATH_MAX] = "";
	int pp_stderr_fd = -1;
	{
		const char *tmpdir = getenv(TMPDIR_ENVVAR);
#ifdef TMPDIR_ENVVAR_ALT
		if (!tmpdir || !*tmpdir) tmpdir = getenv(TMPDIR_ENVVAR_ALT);
#endif
		if (!tmpdir || !*tmpdir) tmpdir = TMPDIR_FALLBACK;
		snprintf(pp_stderr_path, sizeof pp_stderr_path, "%s/prism_pp_err_XXXXXX", tmpdir);
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
	pid_t pid;
	int err = posix_spawnp(&pid, argv[0], &fa, NULL, argv, env);
	posix_spawn_file_actions_destroy(&fa);
	if (pp_stderr_fd >= 0) close(pp_stderr_fd);
	close(pipefd[1]);

	if (err) {
		fprintf(stderr, "posix_spawnp: %s\n", strerror(err));
		if (pp_stderr_path[0]) unlink(pp_stderr_path);
		close(pipefd[0]);
		free(cc_dup);
		free((void *)args);
		return NULL;
	}

	// Read all preprocessed output from pipe
	size_t cap = 8192, len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		free(cc_dup);
		free((void *)args);
		return NULL;
	}

	ssize_t n;
	while ((n = read(pipefd[0], buf + len, cap - len - 1)) > 0 || (n == -1 && errno == EINTR)) {
		if (n == -1) continue;
		len += (size_t)n;
		if (len + 1 >= cap) {
			cap *= 2;
			char *tmp = realloc(buf, cap);
			if (!tmp) {
				free(buf);
				close(pipefd[0]);
				waitpid(pid, NULL, 0);
				free(cc_dup);
				free((void *)args);
				return NULL;
			}
			buf = tmp;
		}
	}
	close(pipefd[0]);
	buf[len] = '\0';

	// Detect null bytes in preprocessor output (would silently truncate tokenization)
	if (strlen(buf) < len) {
		fprintf(stderr, "error: preprocessor output contains null bytes\n");
		free(buf);
		if (pp_stderr_path[0]) unlink(pp_stderr_path);
		waitpid(pid, NULL, 0);
		free(cc_dup);
		free((void *)args);
		return NULL;
	}

	// Right-size buffer with 8-byte padding for SWAR comment scanning
	if (len + 8 < cap) {
		char *fitted = realloc(buf, len + 8);
		if (fitted) buf = fitted;
	}
	memset(buf + len, 0, 8);

	int status;
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		// Show captured preprocessor stderr on failure
		if (pp_stderr_path[0]) {
			FILE *ef = fopen(pp_stderr_path, "r");
			if (ef) {
				char line[512];
				while (fgets(line, sizeof line, ef)) fputs(line, stderr);
				fclose(ef);
			}
			unlink(pp_stderr_path);
		}
		free(buf);
		free(cc_dup);
		free((void *)args);
		return NULL;
	}
	if (pp_stderr_path[0]) unlink(pp_stderr_path);
	free(cc_dup);
	free((void *)args);

	return buf;
}

// Shared helpers for transpile_tokens

static inline void track_ctrl_paren_open(void) {
	ScopeKind k;
	if (ctrl_state.pending_for_paren) {
		k = SCOPE_FOR_PAREN;
		ctrl_state.pending_for_paren = false;
	} else {
		k = SCOPE_CTRL_PAREN;
	}
	ctrl_state.parens_just_closed = false;
	scope_push_kind(k, false);
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
		// Re-push as CTRL_PAREN so second ';' triggers end_statement
		// Actually: the 2nd semicolon is still inside parens, so we just downgrade
		// No — for(init; cond; update) has parens not semicolons controlling it.
		// in_for_init returns false now. That's correct.
		// But we need to stay inside a ctrl paren scope so ';' doesn't end the statement
		scope_push_kind(SCOPE_CTRL_PAREN, false);
	} else if (!in_ctrl_paren()) {
		typedef_pop_scope(ctx->block_depth + 1);
		ctrl_reset();
	}
}

static inline void pop_generic_scope(void) {
	while (ctx->scope_depth > 0) {
		ScopeKind k = scope_stack[ctx->scope_depth - 1].kind;
		if (k == SCOPE_GENERIC) { scope_pop(); break; }
		break;
	}
}

static inline void track_generic_token(Token *tok) {
	if (!in_generic()) return;
	if (match_ch(tok, '(')) scope_push_kind(SCOPE_GENERIC, false);
	else if (match_ch(tok, ')')) pop_generic_scope();
}

static inline void toplevel_track_token(ToplevelState *state, Token *tok) {
	if (ctx->block_depth != 0) return;
	if (match_ch(tok, '(')) {
		if (state->paren_depth++ == 0) state->last_open_paren = tok;
	} else if (match_ch(tok, ')')) {
		if (--state->paren_depth <= 0) {
			state->paren_depth = 0;
			state->last_paren = tok;
		}
	}
	state->prev_tok = tok;
}

static inline void toplevel_set_paren_pair(ToplevelState *state, Token *open, Token *close) {
	state->last_open_paren = open;
	state->last_paren = close;
	state->prev_tok = close;
}

static inline void toplevel_clear_last_paren(ToplevelState *state) {
	state->last_paren = NULL;
}

static inline void track_common_token_state(Token *tok, ToplevelState *state) {
	if (__builtin_expect(ctrl_state.pending && tok->len == 1, 0)) {
		char c = tok_loc(tok)[0];
		if (c == '(') track_ctrl_paren_open();
		else if (c == ')') track_ctrl_paren_close();
	}
	track_generic_token(tok);
	toplevel_track_token(state, tok);
}

// Scan ahead for 'orelse' at depth 0 in a bare expression
static Token *find_bare_orelse(Token *tok) {
	Token *prev = NULL;
	int ternary = 0;
	for (Token *s = tok; s->kind != TK_EOF; s = tok_next(s)) {
		if (s->flags & TF_OPEN) { prev = tok_match(s); s = tok_match(s); continue; }
		if (s->flags & TF_CLOSE) return NULL;
		if (match_ch(s, ';')) return NULL;
		if (match_ch(s, '?')) { ternary++; prev = s; continue; }
		if (match_ch(s, ':') && ternary > 0) { ternary--; prev = s; continue; }
		if ((s->tag & TT_ORELSE) && !is_known_typedef(s) && !typedef_lookup(s) &&
		    !(prev && (prev->tag & TT_MEMBER)) && ternary == 0)
			return s;
		prev = s;
	}
	return NULL;
}

// ── Pre-scan: strict file-scope declaration registration ──
//
// Walk the entire token stream at file scope (brace depth 0) before
// transpilation begins.  Register every typedef name, enum constant,
// and struct/union/enum tag so that the symbol table is fully populated
// before any expression context needs to resolve
//
// We deliberately ignore function bodies (skip matched '{' … '}' at depth > 0)
// because inner-scope typedefs are registered during transpilation by
// parse_typedef_declaration, which tracks scope depth correctly.

static void prescan_file_scope_declarations(Token *tok) {
	bool at_stmt_start = true;
	int brace_depth = 0;

	while (tok && tok->kind != TK_EOF) {
		// Track brace depth — only process depth 0 (file scope)
		if (match_ch(tok, '{')) {
			brace_depth++;
			// At depth 1 (struct/union/enum body at file scope), we continue
			// scanning to register enum constants.
			if (brace_depth == 1) {
				at_stmt_start = true;
				tok = tok_next(tok);
				continue;
			}
			// Skip function bodies and deeper nested scopes entirely
			if (tok_match(tok)) {
				tok = tok_next(tok_match(tok));
				brace_depth--;
			} else {
				tok = tok_next(tok);
			}
			at_stmt_start = true;
			continue;
		}
		if (match_ch(tok, '}')) {
			brace_depth--;
			if (brace_depth < 0) brace_depth = 0;
			at_stmt_start = true;
			tok = tok_next(tok);
			continue;
		}

		// Only care about file scope (brace_depth 0) and
		// depth 1 for struct/union/enum body contents (enum constants).
		if (brace_depth > 1) {
			if (tok->flags & TF_OPEN && tok_match(tok))
				tok = tok_next(tok_match(tok));
			else
				tok = tok_next(tok);
			continue;
		}

		// Inside struct/union/enum body (depth 1): register enum constants
		if (brace_depth == 1) {
			if (is_enum_kw(tok)) {
				Token *brace = find_struct_body_brace(tok);
				if (brace) {
					parse_enum_constants(brace, 0);
					tok = tok_next(tok_match(brace));
					continue;
				}
			}
			if (tok->flags & TF_OPEN && tok_match(tok))
				tok = tok_next(tok_match(tok));
			else
				tok = tok_next(tok);
			continue;
		}

		// File scope (depth 0): statement-start dispatch
		if (match_ch(tok, ';')) {
			at_stmt_start = true;
			tok = tok_next(tok);
			continue;
		}
		if (tok->kind == TK_PREP_DIR) {
			at_stmt_start = true;
			tok = tok_next(tok);
			continue;
		}

		if (!at_stmt_start) {
			tok = tok_next(tok);
			continue;
		}

		// Skip noise (attributes, C23 [[...]], pragmas)
		Token *clean = skip_noise(tok);
		if (clean != tok) { tok = clean; continue; }

		// Skip storage/inline/noreturn specifiers before type
		if ((tok->tag & (TT_STORAGE | TT_INLINE)) || equal(tok, "_Noreturn") || equal(tok, "noreturn") ||
		    equal(tok, "__extension__")) {
			tok = tok_next(tok);
			continue;
		}

		// Skip 'raw' keyword (Prism extension)
		if ((tok->flags & TF_RAW) && !is_known_typedef(tok)) {
			tok = tok_next(tok);
			continue;
		}

		// == typedef declaration ==
		if (tok->tag & TT_TYPEDEF) {
			parse_typedef_declaration(tok, 0);
			while (tok && tok->kind != TK_EOF && !match_ch(tok, ';')) {
				if (tok->flags & TF_OPEN && tok_match(tok))
					tok = tok_next(tok_match(tok));
				else
					tok = tok_next(tok);
			}
			if (tok && match_ch(tok, ';')) tok = tok_next(tok);
			at_stmt_start = true;
			continue;
		}

		// == struct/union/enum with body ==
		if (tok->tag & TT_SUE) {
			bool is_enum = is_enum_kw(tok);
			Token *brace = find_struct_body_brace(tok);
			if (brace) {
				if (is_enum)
					parse_enum_constants(brace, 0);
				tok = tok_next(tok);
				at_stmt_start = false;
				continue;
			}
			tok = tok_next(tok);
			at_stmt_start = false;
			continue;
		}

		// == _Static_assert — skip to semicolon ==
		if (equal(tok, "_Static_assert") || equal(tok, "static_assert")) {
			while (tok && tok->kind != TK_EOF && !match_ch(tok, ';')) {
				if (tok->flags & TF_OPEN && tok_match(tok))
					tok = tok_next(tok_match(tok));
				else
					tok = tok_next(tok);
			}
			if (tok && match_ch(tok, ';')) tok = tok_next(tok);
			at_stmt_start = true;
			continue;
		}

		at_stmt_start = false;
		tok = tok_next(tok);
	}
}

// Core transpile: emit transformed tokens to an already-opened FILE*
static int transpile_tokens(Token *tok, FILE *fp) {
	out_init(fp);

	if (FEAT(F_FLATTEN)) {
		emit_system_header_diag_push();
		out_char('\n');
	}

	reset_transpiler_state();
	typedef_table_reset();
	system_includes_reset();

	// Pre-scan: register all file-scope typedefs, enum constants, and
	// struct/union/enum tags before transpilation begins.  This replaces
	// the old suffix/prefix heuristic with deterministic symbol resolution.
	prescan_file_scope_declarations(tok);

	if (!FEAT(F_FLATTEN)) {
		collect_system_includes();
		emit_system_includes();
	}

	bool next_func_returns_void = false;
	bool next_func_ret_captured = false;
	ToplevelState toplevel = {0};
	int ternary_depth = 0;

	while (tok->kind != TK_EOF) {
		Token *next;
		uint32_t tag = tok->tag;

		if (tok->len == 1 && tok_loc(tok)[0] == '?')
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
		if (__builtin_expect(!tag && !ctx->at_stmt_start, 1)) {
			track_common_token_state(tok, &toplevel);
			emit_tok(tok);
			tok = tok_next(tok);
			continue;
		}

		// Slow path: statement-start processing and tagged tokens

		// Track typedefs (must precede zero-init check)
		// For-init typedefs registered at block_depth+1 for loop-scoped cleanup
		if (ctx->at_stmt_start && !in_struct_body()) {
			if (tag & TT_TYPEDEF) {
				parse_typedef_declaration(tok, ctx->block_depth + (in_for_init() ? 1 : 0));
			} else if (is_raw(tok) && tok_next(tok) && (tok_next(tok)->tag & TT_TYPEDEF)) {
				parse_typedef_declaration(tok_next(tok), ctx->block_depth + (in_for_init() ? 1 : 0));
				tok = tok_next(tok);
				continue;
			}
		}

		// Register shadows for file-scope _t/__ variable declarations
		// to prevent typedef heuristic misidentification.
		// Skip between function ')' and '{' (K&R parameter declarations).
		if (ctx->at_stmt_start && ctx->block_depth == 0 && !in_struct_body() &&
		    !(tag & TT_TYPEDEF) && !toplevel.last_paren)
			register_toplevel_shadows(tok);

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

			// Bare expression orelse.
			// Skip keywords that introduce sub-statements or labels
			// to avoid sweeping them into the if(!(...)) condition.
			// Also skip storage class specifiers and typedef — those are
			// declarations, not bare expressions.
			if (FEAT(F_ORELSE) && ctx->block_depth > 0 && !in_struct_body() &&
			    !(tok->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO |
					  TT_CASE | TT_DEFAULT | TT_IF | TT_LOOP | TT_SWITCH |
					  TT_STORAGE | TT_TYPEDEF))) {
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
					if (is_orelse_keyword(tok))
						error_tok(tok, "expected expression before 'orelse'");
					// Emit label before orelse processing
					if (label_end) {
						emit_tok(tok);       // label ident
						emit_tok(tok_next(tok)); // ':'
						tok = label_end;
					}

					// Find assignment target for re-emit in fallback
					Token *bare_lhs_start = tok;
					Token *bare_assign_eq = NULL;
					{
						int sd = 0;
						for (Token *s = tok; s != orelse_tok; s = tok_next(s)) {
							if (s->flags & TF_OPEN) sd++;
							else if (s->flags & TF_CLOSE) sd--;
							else if (sd == 0 && is_assignment_operator_token(s)) {
								if (!match_ch(s, '='))
									error_tok(s, "bare assignment with 'orelse' cannot use compound operators "
										  "(e.g. +=, -=); use a plain '=' assignment");
								bare_assign_eq = s;
								break;
							}
						}
					}

					// Error if LHS has side effects (double evaluation)
					if (bare_assign_eq) {
						for (Token *s = bare_lhs_start; s != bare_assign_eq; s = tok_next(s)) {
							if (is_assignment_operator_token(s))
								error_tok(s, "orelse fallback on assignment with side effects "
									  "in the target expression (double evaluation); "
									  "use a temporary variable instead");
							if (equal(s, "++") || equal(s, "--"))
								error_tok(s, "orelse fallback on assignment with side effects "
									  "in the target expression (double evaluation); "
									  "use a temporary variable instead");
							if (s->tag & TT_ASM)
								error_tok(s, "orelse fallback on assignment with inline asm "
									  "in the target expression (double evaluation); "
									  "use a temporary variable instead");
							if (is_valid_varname(s) && !is_type_keyword(s) && tok_next(s) &&
							    tok_next(s) != bare_assign_eq && match_ch(tok_next(s), '('))
								error_tok(s, "orelse fallback on assignment with a function call "
									  "in the target expression (double evaluation); "
									  "use a temporary variable instead");
							// Indirect calls: (expr)() — paren group followed by (
							if (match_ch(s, '(') && (s->flags & TF_OPEN) && tok_match(s) &&
							    tok_match(s) != bare_assign_eq && tok_next(tok_match(s)) &&
							    tok_next(tok_match(s)) != bare_assign_eq &&
							    match_ch(tok_next(tok_match(s)), '('))
								error_tok(s, "orelse fallback on assignment with an indirect call "
									  "in the target expression (double evaluation); "
									  "use a temporary variable instead");
						}
					}

					// Check if fallback is a bare value (not control flow or block)
					Token *after_orelse = tok_next(orelse_tok);
					bool is_bare_fallback = bare_assign_eq && after_orelse &&
						!(after_orelse->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) &&
						!match_ch(after_orelse, '{') && !match_ch(after_orelse, ';');

					// Hoist TK_PREP_DIR tokens before the if wrapper (invalid inside parens)
					for (Token *s = tok; s != orelse_tok; s = tok_next(s)) {
						if (s->kind == TK_PREP_DIR) { emit_tok(s); out_char('\n'); }
					}

					if (is_bare_fallback) {
						// Detect compound literals in fallback (contain '{'):
						// Use brace-free if-based pattern to preserve compound
						// literal scope lifetime in the enclosing block AND
						// avoid double evaluation (safe for volatile targets).
						// Non-compound fallbacks use brace-wrapped if-based pattern.
						// Scan the entire expression (including chained orelse) for '{'.
						bool fallback_has_compound_literal = false;
						{
							int sd = 0;
							for (Token *s = tok_next(orelse_tok); s && s->kind != TK_EOF; s = tok_next(s)) {
								if (s->flags & TF_OPEN) sd++;
								else if (s->flags & TF_CLOSE) sd--;
								else if (sd == 0 && (match_ch(s, ';') || match_ch(s, ','))) break;
								if (match_ch(s, '{')) { fallback_has_compound_literal = true; break; }
							}
						}

						// Hoist pre-processor directives
						out_char(' ');
						if (fallback_has_compound_literal) {
							// Brace-free if: LHS = rhs; if (!t) t = (fb);
							// Preserves compound literal scope in the enclosing block
							// and reads the target only once (volatile-safe).
							for (Token *t = bare_lhs_start; t != orelse_tok; t = tok_next(t)) {
								if (t->kind == TK_PREP_DIR) continue;
								emit_tok(t);
							}
							out_char(';');
							OUT_LIT(" if (!");
							emit_range(bare_lhs_start, bare_assign_eq);
							OUT_LIT(")");
							emit_range(bare_lhs_start, bare_assign_eq);
							OUT_LIT(" = (");
							tok = tok_next(orelse_tok);
							int fd = 0;
							while (tok->kind != TK_EOF) {
								if (tok->flags & TF_OPEN) fd++;
								else if (tok->flags & TF_CLOSE) fd--;
								else if (fd == 0 && (match_ch(tok, ';') || match_ch(tok, ','))) break;
								if (fd == 0 && is_orelse_keyword(tok)) {
									OUT_LIT("); if (!");
									emit_range(bare_lhs_start, bare_assign_eq);
									OUT_LIT(")");
									emit_range(bare_lhs_start, bare_assign_eq);
									OUT_LIT(" = (");
									tok = tok_next(tok);
									continue;
								}
								emit_tok(tok);
								tok = tok_next(tok);
							}
							OUT_LIT(");");
						} else {
							// If-based: { LHS = rhs; if (!t) t = (fb); }
							// Safe for volatile — no double evaluation.
							OUT_LIT("{");
							out_char(' ');
							for (Token *t = bare_lhs_start; t != orelse_tok; t = tok_next(t)) {
								if (t->kind == TK_PREP_DIR) continue;
								emit_tok(t);
							}
							out_char(';');
							OUT_LIT(" if (!");
							emit_range(bare_lhs_start, bare_assign_eq);
							OUT_LIT(")");
							emit_range(bare_lhs_start, bare_assign_eq);
							OUT_LIT(" = (");
							tok = tok_next(orelse_tok);
							int fd = 0;
							while (tok->kind != TK_EOF) {
								if (tok->flags & TF_OPEN) fd++;
								else if (tok->flags & TF_CLOSE) fd--;
								else if (fd == 0 && (match_ch(tok, ';') || match_ch(tok, ','))) break;
								if (fd == 0 && is_orelse_keyword(tok)) {
									OUT_LIT("); if (!");
									emit_range(bare_lhs_start, bare_assign_eq);
									OUT_LIT(")");
									emit_range(bare_lhs_start, bare_assign_eq);
									OUT_LIT(" = (");
									tok = tok_next(tok);
									continue;
								}
								emit_tok(tok);
								tok = tok_next(tok);
							}
							OUT_LIT("); }");
						}
						if (match_ch(tok, ';')) tok = tok_next(tok);
						else if (match_ch(tok, ',')) tok = tok_next(tok);
						end_statement_after_semicolon();
						continue;
					}

				// Non-bare fallback (control flow / block): wrap in braces
				OUT_LIT(" {");
				OUT_LIT(" if (!(");
				while (tok != orelse_tok) {
					if (tok->kind == TK_PREP_DIR) { tok = tok_next(tok); continue; }
					emit_tok(tok);
					tok = tok_next(tok);
				}
				OUT_LIT("))");
				tok = tok_next(tok);

					require_orelse_action(tok, NULL);

					bool is_block_action = match_ch(tok, '{');
					tok = emit_orelse_action(tok, NULL, false, false, NULL);
					if (is_block_action) {
						ctrl_state.pending_orelse_guard = true;
					} else {
						OUT_LIT(" }");
					}
					continue;
				}
			}
		}
		ctx->at_stmt_start = false;

		if (tag & TT_NORETURN_FN) {
			if (tok_next(tok) && match_ch(tok_next(tok), '(')) {
				mark_switch_control_exit(false);
				if (FEAT(F_DEFER) && has_active_defers())
					fprintf(stderr,
						"%s:%d: warning: '%.*s' called with active defers (defers "
						"will not run)\n",
						tok_file(tok)->name,
						tok_line_no(tok),
						tok->len,
						tok_loc(tok));
			}
		}

		// Tag-dependent dispatch
		if (tag) {
			// Keyword dispatch

			if (__builtin_expect(tag & TT_DEFER, 0) && !in_generic())
				DISPATCH(handle_defer_keyword);
			if (__builtin_expect(FEAT(F_DEFER) && (tag & TT_RETURN), 0))
				DISPATCH(handle_return_defer);
			if (__builtin_expect(FEAT(F_DEFER) && (tag & (TT_BREAK | TT_CONTINUE)), 0))
				DISPATCH(handle_break_continue_defer);
			if (__builtin_expect((tag & TT_GOTO) && FEAT(F_DEFER | F_ZEROINIT), 0))
				DISPATCH(handle_goto_keyword);

			// Control-flow flag setting

			if (tag & TT_LOOP) {
				if (FEAT(F_DEFER)) {
					ctrl_state.scope_flags |= NS_LOOP;
					ctrl_state.pending = true;
					if (equal(tok, "do")) ctrl_state.parens_just_closed = true;
				}
				if (equal(tok, "for") && FEAT(F_DEFER | F_ZEROINIT)) {
					ctrl_state.pending = true;
					ctrl_state.pending_for_paren = true;
				}
			}

			if ((tag & TT_GENERIC) && !in_generic()) {
				if (last_emitted && (match_ch(last_emitted, '.') || equal(last_emitted, "->"))) {
					Token *name = NULL;
					Token *args_open = NULL;
					Token *args_close = NULL;
					Token *after = NULL;
					if (generic_member_rewrite_target(tok, &name, &args_open, &args_close,
									  &after)) {
						emit_tok(name);
						emit_range(args_open, tok_next(args_close));
						last_emitted = args_close;
						tok = after;
						continue;
					}
				}
				if (is_decl_prefix_token(last_emitted)) {
					Token *name = NULL;
					Token *params_open = NULL;
					Token *params_close = NULL;
					Token *after = NULL;
					if (generic_decl_rewrite_target(tok, &name, &params_open, &params_close,
									&after)) {
						out_char('(');
						out_str(tok_loc(name), name->len);
						out_char(')');
						emit_range(params_open, tok_next(params_close));
						if (ctx->block_depth == 0) {
							toplevel_set_paren_pair(&toplevel, params_open, params_close);
						}
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
					scope_push_kind(SCOPE_GENERIC, false);
					emit_tok(tok);
					last_emitted = tok;
					tok = tok_next(tok);
				}
				continue;
			}

			if (FEAT(F_DEFER) && (tag & TT_SWITCH)) {
				ctrl_state.scope_flags |= NS_SWITCH;
				ctrl_state.pending = true;
			}

			// C23 if/switch initializers: if(int x; cond) / switch(int x; x)
			// Treat the open paren like for() so at_stmt_start is set and
			// zero-init / typedef tracking fires on the initializer decl.
			if ((tag & TT_SWITCH) && FEAT(F_DEFER | F_ZEROINIT)) {
				ctrl_state.pending = true;
				ctrl_state.pending_for_paren = true;
			}

			if (tag & TT_IF) {
				ctrl_state.pending = true;
				if (equal(tok, "else")) {
					ctrl_state.parens_just_closed = true;
					ctx->at_stmt_start = true;
				} else if (FEAT(F_DEFER | F_ZEROINIT)) {
					ctrl_state.pending_for_paren = true;
				}
			}

			// Case/default label handling
			if (tag & (TT_CASE | TT_DEFAULT)) handle_case_default(tok);

		} // end if (tag)

		track_generic_token(tok);

		// Void function detection and return type capture at top level
		if (ctx->block_depth == 0 &&
		    (tag & (TT_TYPE | TT_QUALIFIER | TT_SKIP_DECL | TT_ATTR | TT_INLINE) ||
		     (is_identifier_like(tok) && (is_void_typedef(tok) || is_known_typedef(tok))))) {
			int ret = capture_function_return_type(tok);
			if (ret == 1) next_func_returns_void = true;
			else if (ret == 2) next_func_ret_captured = true;
		}

		if (tag & TT_SUE) // struct/union/enum body
			DISPATCH(handle_sue_body);

		// Structural punctuation: { } ; :
		if (tag & TT_STRUCTURAL) {
			if (match_ch(tok, '{')) {
				// Function definition detection at top level
				if (ctx->block_depth == 0 && FEAT(F_DEFER)) {
					bool is_func_def = false;
					if (toplevel.prev_tok && match_ch(toplevel.prev_tok, ')'))
						is_func_def = true;
					else if (toplevel.last_paren &&
						 is_knr_params(tok_next(toplevel.last_paren), tok))
						is_func_def = true;
					else if (toplevel.last_paren) {
						// Skip attrs, array dims, param lists between ')' and '{'
						Token *after = skip_declarator_suffix(tok_next(toplevel.last_paren));
						if (after == tok) is_func_def = true;
					}
					if (is_func_def) {
						scan_labels_in_function(tok);
						register_param_shadows(toplevel.last_open_paren,
								       toplevel.last_paren);
						ctx->current_func_returns_void = next_func_returns_void;
						if (next_func_returns_void || !next_func_ret_captured)
							clear_func_ret_type();
					} else {
						clear_func_ret_type();
					}
					next_func_returns_void = false;
					next_func_ret_captured = false;
					toplevel_clear_last_paren(&toplevel);
				}
				// Reset K&R tracking at file scope to avoid stale shadow table guard
				if (ctx->block_depth == 0)
					toplevel_clear_last_paren(&toplevel);
				tok = handle_open_brace(tok);
				continue;
			}
			if (match_ch(tok, '}')) {
				tok = handle_close_brace(tok);
				// Reset per-function flags when leaving a function body
				if (ctx->block_depth == 0) {
					ctx->current_func_has_setjmp = false;
					ctx->current_func_has_vfork = false;
					ctx->current_func_has_asm = false;
				}
				continue;
			}
			char c = tok_loc(tok)[0];
			if (c == ';') {
				if (in_ctrl_paren() || in_for_init()) track_ctrl_semicolon();
				else end_statement_after_semicolon();
				if (ctx->block_depth == 0) {
					next_func_returns_void = false;
					next_func_ret_captured = false;
				}
				emit_tok(tok);
				tok = tok_next(tok);
				continue;
			}
			if (c == ':') {
				if (ternary_depth > 0) {
					ternary_depth--;
				} else if (in_generic()) {
				} else if (last_emitted &&
				           (is_identifier_like(last_emitted) || last_emitted->kind == TK_NUM) &&
				           !in_struct_body() && ctx->block_depth > 0) {
					emit_tok(tok);
					tok = tok_next(tok);
					ctx->at_stmt_start = true;
					continue;
				}
			}
		}

		if (__builtin_expect(tok->kind == TK_PREP_DIR, 0)) {
			emit_tok(tok);
			tok = tok_next(tok);
			ctx->at_stmt_start = true;
			continue;
		}

		track_common_token_state(tok, &toplevel);

		// Reject unprocessed 'orelse' inside typeof expressions in struct bodies.
		// Struct field names like "bool orelse;" are legitimate and handled below,
		// but orelse inside typeof(expr orelse fallback) would pass through verbatim.
		if (__builtin_expect(FEAT(F_ORELSE) && in_struct_body() && (tok->tag & TT_TYPEOF), 0)) {
			Token *paren = tok_next(tok);
			if (paren && match_ch(paren, '(') && tok_match(paren)) {
				for (Token *s = tok_next(paren); s && s != tok_match(paren); s = tok_next(s))
					if (is_orelse_keyword(s))
						error_tok(s,
							  "'orelse' inside typeof in a struct/union body "
							  "cannot be transformed; use the resolved type directly");
			}
		}

		// Warn on unprocessed 'orelse' in unsupported context
		if (__builtin_expect(FEAT(F_ORELSE) && is_orelse_keyword(tok) &&
				     !in_struct_body(), 0))
			error_tok(tok,
				  "'orelse' cannot be used here (it must appear at the "
				  "statement level in a declaration or bare expression)");

		emit_tok(tok);
		tok = tok_next(tok);
	}

	if (FEAT(F_FLATTEN)) {
		out_char('\n');
		emit_system_header_diag_pop();
	}

	out_close();
	tokenizer_teardown(false);
	return 1;
}

static int transpile(char *input_file, char *output_file) {
	ensure_keyword_cache();

	char *pp_buf = preprocess_with_cc(input_file);
	if (!pp_buf) {
		fprintf(stderr, "Preprocessing failed for: %s\n", input_file);
		return 0;
	}

	Token *tok = tokenize_buffer(input_file, pp_buf);

	if (!tok) {
		fprintf(stderr, "Failed to tokenize preprocessed output\n");
		tokenizer_teardown(false);
		return 0;
	}

	FILE *fp = fopen(output_file, "w");
	if (!fp) {
		tokenizer_teardown(false);
		return 0;
	}

	return transpile_tokens(tok, fp);
}

// LIBRARY API

PRISM_API void prism_free(PrismResult *r) {
	free(r->output);
	free(r->error_msg);
	r->output = r->error_msg = NULL;
}

PRISM_API void prism_reset(void) {
	typedef_table_reset();
	tokenizer_teardown(false);

	ctx->scope_depth = 0;
	ctx->block_depth = 0;

	label_table = NULL;
	label_count = 0;
	label_capacity = 0;

	system_includes_reset();

	if (out_fp) {
		out_flush();
		fclose(out_fp);
		out_fp = NULL;
	}
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
		if (a[0] != '-' || !a[1]) {
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

#ifndef PRISM_LIB_MODE

// Transpile and pipe output directly to the compiler (no temp files)
static int transpile_and_compile(char *input_file, char **compile_argv, bool verbose) {
	char *pp_buf = preprocess_with_cc(input_file);
	if (!pp_buf) {
		fprintf(stderr, "Preprocessing failed for: %s\n", input_file);
		return -1;
	}

	Token *tok = tokenize_buffer(input_file, pp_buf);
	if (!tok) {
		fprintf(stderr, "Failed to tokenize preprocessed output\n");
		tokenizer_teardown(false);
		return -1;
	}

	int pipefd[2];
	if (pipe(pipefd) == -1) {
		perror("pipe");
		tokenizer_teardown(false);
		return -1;
	}

	if (verbose) {
		fprintf(stderr, "[prism] ");
		for (int i = 0; compile_argv[i]; i++) fprintf(stderr, "%s ", compile_argv[i]);
		fprintf(stderr, "\n");
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

static const char *get_install_path(void) { return INSTALL_PATH; }

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

static void check_path_shadow(const char *install_path) {
	const char *cmd = FIND_EXE_CMD;
	FILE *fp = popen(cmd, "r");
	if (!fp) return;
	char first_hit[PATH_MAX];
	first_hit[0] = '\0';
	if (fgets(first_hit, sizeof(first_hit), fp)) {
		size_t len = strlen(first_hit);
		if (len > 0 && first_hit[len - 1] == '\n') first_hit[len - 1] = '\0';
	}
	pclose(fp);
	if (first_hit[0] && strcmp(first_hit, install_path) != 0) {
		char resolved_hit[PATH_MAX], resolved_install[PATH_MAX];
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

	if (!ensure_install_dir(install_path)) {
		goto use_sudo;
	}

	char resolved_path[PATH_MAX];
	struct stat st;
	if (stat(self_path, &st) != 0 && get_self_exe_path(resolved_path, sizeof(resolved_path)))
		self_path = resolved_path;

	if (strcmp(self_path, install_path) == 0) {
		printf("[prism] Already installed at %s\n", install_path);
		return 0;
	}

	FILE *input = fopen(self_path, "rb");
	FILE *output = input ? fopen(install_path, "wb") : NULL;

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
	fprintf(stderr, "[prism] Failed to install (run as Administrator?)\n");
	return 1;
#else
	{
		const char *argv_cp[] = {"cp", self_path, install_path, NULL};
		if (run_command_quiet((char **)argv_cp) == 0) {
			const char *argv_chmod[] = {"chmod", "+x", install_path, NULL};
			run_command((char **)argv_chmod);
		} else {
			const char *escalate = NULL;
			if (access("/usr/bin/sudo", X_OK) == 0 || access("/bin/sudo", X_OK) == 0)
				escalate = "sudo";
			else if (access("/usr/bin/doas", X_OK) == 0 || access("/bin/doas", X_OK) == 0)
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
#ifndef _WIN32
static int capture_first_line(char **argv, char *buf, size_t bufsize);
#endif
static bool cc_is_clang(const char *cc) {
#ifdef __APPLE__
	if (!cc || !*cc || strcmp(cc, "cc") == 0 || strcmp(cc, "gcc") == 0) return true;
#endif
	if (!cc || !*cc) return false;
	const char *exe = cc_executable(cc);
	const char *base = path_basename(exe);
	if (strncmp(base, "clang", 5) == 0) return true;
#ifndef _WIN32
	// Probe: on many systems (Termux, FreeBSD, some Linux distros),
	// "cc" or "gcc" is actually clang behind a symlink.
	char ver[256];
	char *argv[] = {(char *)exe, "--version", NULL};
	if (capture_first_line(argv, ver, sizeof(ver)) == 0) {
		// clang --version outputs "... clang ..." on the first line
		for (char *p = ver; *p; p++) *p = (char)tolower((unsigned char)*p);
		if (strstr(ver, "clang")) return true;
	}
#endif
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

static const char *cli_output_path(const Cli *cli, const char *temp_exe, bool msvc) {
	static char defobj[PATH_MAX];

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
		static char flag[PATH_MAX + 8]; // cl.exe: /Fe:exe or /Fo:obj
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
	if (plan->optimize) args[argc++] = plan->msvc ? "/O2" : "-O2";
	if (plan->use_preprocessed) args[argc++] = "-fpreprocessed";
	for (int i = 0; i < temp_count; i++) args[argc++] = temps[i];
	if (plan->use_preprocessed) args[argc++] = "-fno-preprocessed";
	for (int i = 0; i < cli->cc_arg_count; i++) args[argc++] = cli->cc_args[i];
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
	const char *cc = get_real_cc(cli->cc ? cli->cc : getenv("PRISM_CC"));
	if (!cc || (strcmp(cc, "cc") == 0 && !cli->cc)) {
		cc = getenv("CC");
		if (cc) cc = get_real_cc(cc);
	}
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
			if (make_temp_file(temps[i], PATH_MAX, NULL, 0, cli->sources[i]) < 0)
				die("Failed to create temp file");
			signal_temps_register(temps[i]);
			PrismResult result = prism_transpile_file(cli->sources[i], cli->features);
			if (result.status != PRISM_OK) {
				fprintf(stderr, "%s:%d:%d: error: %s\n", cli->sources[i],
					result.error_line, result.error_col,
					result.error_msg ? result.error_msg : "transpilation failed");
				prism_free(&result);
				cleanup_temp_range(temps, i + 1);
				return NULL;
			}
			FILE *f = fopen(temps[i], "w");
			if (!f) { prism_free(&result); die("Failed to create temp file"); }
			fwrite(result.output, 1, result.output_len, f);
			fclose(f);
			prism_free(&result);
		} else {
			temps[i] = malloc(512);
			if (!temps[i]) die("Out of memory");
			if (make_temp_file(temps[i], 512, NULL, 0, cli->sources[i]) < 0)
				die("Failed to create temp file");
			signal_temps_register(temps[i]);
			if (cli->verbose)
				fprintf(stderr, "[prism] Transpiling %s -> %s\n", cli->sources[i], temps[i]);
			if (!transpile((char *)cli->sources[i], temps[i])) {
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
		if (cli->cc_arg_count > 0) {
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
	if (signal_temp_load() && signal_temp_path[0])
		unlink(signal_temp_path);
	int n = signal_temps_load();
	for (int i = 0; i < n; i++)
		unlink(signal_temps[i]);
	signal(sig, SIG_DFL);
	raise(sig);
}

int main(int argc, char **argv) {
#ifndef _WIN32
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, signal_cleanup_handler);
	signal(SIGTERM, signal_cleanup_handler);
#endif
	int status = 0;
	prism_ctx_init();

	if (argc < 2) {
		print_help();
		return 0;
	}

	Cli cli = cli_parse(argc, argv);

	// Handle help/version actions (cli_parse sets these without side effects)
	if (cli.action == CLI_ACT_HELP) { print_help(); return 0; }
	if (cli.action == CLI_ACT_VERSION) {
		const char *real_cc = get_real_cc(cli.cc);
#ifndef _WIN32
		char cc_line[256];
		char *vargs[] = {(char *)real_cc, "--version", NULL};
		if (capture_first_line(vargs, cc_line, sizeof(cc_line)) == 0 && cc_line[0])
			printf("prism %s (%s)\n", PRISM_VERSION, cc_line);
		else
#endif
			printf("prism %s\n", PRISM_VERSION);
		return 0;
	}

	// Resolve CC (env vars checked here, not in cli_parse, to keep it pure)
	if (!cli.cc) {
		char *env_cc = getenv("PRISM_CC");
		if (!env_cc || !*env_cc || is_prism_cc(env_cc)) {
			env_cc = getenv("CC");
			if (is_prism_cc(env_cc)) env_cc = NULL;
		}
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
		#ifdef _WIN32
			char temp[PATH_MAX];
			if (make_temp_file(temp, sizeof(temp), NULL, 0, cli.sources[i]) < 0)
				die("Failed to create temp file");
			if (!transpile((char *)cli.sources[i], temp)) {
				remove(temp);
				die("Transpilation failed");
			}
			FILE *f = fopen(temp, "r");
			if (f) {
				int c;
				while ((c = fgetc(f)) != EOF) putchar(c);
				fclose(f);
			}
			remove(temp);
		#else
			if (!transpile((char *)cli.sources[i], "/dev/stdout")) die("Transpilation failed");
		#endif
		}
	} else if (cli.source_count == 0)
		status = passthrough_cc(&cli);
	else
		status = compile_sources(&cli);

	// cli_free(&cli); // not needed as os reclaims all resources anyway
	return status;
}

#endif // PRISM_LIB_MODE

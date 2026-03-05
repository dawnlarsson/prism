#define PRISM_VERSION "0.118.0"

#ifdef _WIN32
#define PRISM_DEFAULT_CC "cl"
#define EXE_SUFFIX ".exe"
#define TMPDIR_ENVVAR "TEMP"
#define TMPDIR_ENVVAR_ALT "TMP"
#define TMPDIR_FALLBACK ".\\"
#define FIND_EXE_CMD "where prism.exe 2>nul"
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

#define INSTALL_PATH "/usr/local/bin/prism"
#define PRISM_DEFAULT_CC "cc"
#define EXE_SUFFIX ""
#define TMPDIR_ENVVAR "TMPDIR"
#define TMPDIR_ENVVAR_ALT NULL
#define TMPDIR_FALLBACK "/tmp/"
#define FIND_EXE_CMD "which -a prism 2>/dev/null || command -v prism 2>/dev/null"
#endif

#ifdef PRISM_LIB_MODE
#define PRISM_API
#else
#define PRISM_API static
#endif

#include "parse.c"

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
#define is_c23_attr(t) ((t) && equal(t, "[") && (t)->next && equal((t)->next, "["))

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
#define argv_builder_init(ab) (*(ab) = (ArgvBuilder){0})
#define argv_builder_finish(ab) ((ab)->data)

typedef struct {
	bool defer;
	bool zeroinit;
	bool line_directives;
	bool warn_safety;     // If true, safety checks warn instead of error
	bool flatten_headers; // If true, include flattened system headers (default: true)
	bool orelse;	      // If true, enable orelse keyword (default: true)

	// Preprocessor configuration (optional - can be left NULL/0 for defaults)
	const char *compiler;	    // Compiler to use (default: "cc")
	const char **include_paths; // -I paths
	int include_count;
	const char **defines; // -D macros
	int define_count;
	const char **compiler_flags; // Additional flags (-std=c99, -m32, etc.)
	int compiler_flags_count;
	const char **force_includes; // -include files
	int force_include_count;
} PrismFeatures;

typedef enum {
	PRISM_OK = 0,
	PRISM_ERR_SYNTAX,
	PRISM_ERR_SEMANTIC,
	PRISM_ERR_IO,
} PrismStatus;

typedef struct {
	PrismStatus status;
	char *output; // transpiled C (caller frees with prism_free)
	size_t output_len;
	char *error_msg; // error message (NULL on success)
	int error_line;
	int error_col;
} PrismResult;

typedef struct {
	Token *end;	   // First token after the type specifier
	bool saw_type;	   // True if a type was recognized
	bool is_struct;	   // True if struct/union/enum type
	bool is_typedef;   // True if user-defined typedef
	bool is_vla;	   // True if VLA typedef or struct with VLA member
	bool has_typeof;   // True if typeof/typeof_unqual (cannot determine VLA at transpile-time)
	bool has_atomic;   // True if _Atomic qualifier/specifier present
	bool has_register; // True if register storage class
	bool has_volatile; // True if volatile qualifier
	bool has_const;	   // True if const qualifier
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

typedef struct {
	DeferEntry *entries;
	int count;
	int capacity;
	bool is_loop;	       // true if this scope is a for/while/do loop
	bool is_switch;
	bool had_control_exit; // true if unconditional break/return/goto/continue was seen
			       // NOTE: only set on switch scopes (by mark_switch_control_exit)
	bool
	    is_conditional; // true if this scope is an if/while/for block (for tracking conditional control exits)
	bool seen_case_label; // true if case/default label seen in this switch scope (for zero-init safety)
	bool is_struct;	      // true if this scope is a struct/union/enum body
	bool has_zeroinit_decl; // true if a zero-init'd variable declaration was emitted in this scope
} DeferScope;

// Result of goto skip analysis — both defer and decl checked in a single walk
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
	LabelInfo *labels;
	int count;
	int capacity;
	HashMap name_map; // For O(1) lookups by name
} LabelTable;

typedef struct {
	char *name; // Points into token stream (no alloc needed)
	int len;
	int scope_depth;    // Scope where defined (aligns with ctx->defer_depth)
	bool is_vla;	    // True if typedef refers to a VLA type
	bool is_void;	    // True if typedef resolves to void (e.g., typedef void Void)
	bool is_const;	    // True if typedef includes const (e.g., typedef const int cint)
	bool is_ptr;	    // True if typedef resolves to a pointer type (e.g., typedef int *ptr)
	bool is_array;      // True if typedef resolves to an array type (e.g., typedef int arr_t[5])
	bool is_shadow;	    // True if this entry shadows a typedef (variable with same name)
	bool is_enum_const; // True if this is an enum constant (compile-time constant)
	bool is_vla_var;    // True if this is a VLA variable (not a typedef, but needs VLA tracking)
	int prev_index;	    // Index of previous entry with same name (-1 if none), for hash map chaining
} TypedefEntry;

typedef struct {
	TypedefEntry *entries;
	int count;
	int capacity;
	HashMap name_map; // Maps name -> index+1 of most recent entry (0 means not found)
} TypedefTable;

typedef enum { CLI_DEFAULT, CLI_RUN, CLI_EMIT, CLI_INSTALL } CliMode;

typedef struct {
	CliMode mode;
	PrismFeatures features;
	const char **sources;
	int source_count, source_cap;
	const char **cc_args;
	int cc_arg_count, cc_arg_cap;
	const char *output;
	const char *cc;
	bool verbose;
	bool compile_only;
} Cli;

// Control flow state
enum { NS_LOOP = 1, NS_SWITCH = 2, NS_CONDITIONAL = 4 };

typedef struct {
	int paren_depth;
	int brace_depth;
	uint8_t next_scope; // NS_LOOP | NS_SWITCH | NS_CONDITIONAL
	bool pending;
	bool parens_just_closed;
	bool for_init;
	bool await_for_paren;
} ControlFlow;

typedef struct {
	char **data;
	int count, capacity;
} ArgvBuilder;

// Unified token walker used by scan_labels_in_function and goto_skips_check
typedef struct {
	Token *tok;
	Token *prev;
	int depth;
	int initial_depth;
	int ternary_depth;
} TokenWalker;

// Declarator parsing result
typedef struct {
	Token *end;	  // First token after declarator
	Token *var_name;
	bool is_pointer;
	bool is_array;
	bool is_vla;		// Has variable-length array dimension
	bool is_func_ptr;
	bool has_paren;
	bool paren_pointer;	// Has pointer (*) inside parenthesized declarator
	bool paren_array;	// Has array dimension inside parenthesized declarator
	bool has_init;
	bool is_const;		// Has const qualifier on declarator (e.g. * const)
} DeclResult;

#define struct_body_contains_vla(brace) scan_for_vla(brace, "{", "}")
#define typedef_contains_vla(tok) scan_for_vla(tok, NULL, ";")

extern char **environ;
static char **cached_clean_env = NULL;

static HashMap system_includes;	   // Tracks unique system headers to emit
static char **system_include_list; // Ordered list of includes
static int system_include_capacity = 0;

static HashMap longjmp_wrapper_map; // Tracks functions that call longjmp (for indirect detection)

static ControlFlow ctrl = {0};
static LabelTable label_table;

static int *stmt_expr_levels = NULL; // ctx->defer_depth when stmt expr started (dynamic)
static int stmt_expr_capacity = 0;
static int *orelse_guard_levels = NULL; // defer_depth+1 for pending dangling-else guard braces
static int orelse_guard_capacity = 0;

// Token emission - user-space buffered output for minimal syscall overhead
static FILE *out_fp;
static Token *last_emitted = NULL;
static int cached_file_idx = -1;
static File *cached_file = NULL;

static char out_buf_static[OUT_BUF_SIZE];
static char *out_buf = out_buf_static;
static int out_buf_pos = 0;
static int out_buf_cap = OUT_BUF_SIZE;
static int oe_buf_checkpoint = -1;   // Output buffer checkpoint for speculative rollback
static bool use_linemarkers = false; // true = GCC linemarker "# N", false = C99 "#line N"

static TypedefTable typedef_table;

static DeferScope *defer_stack = NULL;
static int defer_stack_capacity = 0;

// Forward declarations (only for functions used before their definition)
static DeclResult parse_declarator(Token *tok, bool emit);
static bool is_type_keyword(Token *tok);
static inline bool is_valid_varname(Token *tok);
static void typedef_pop_scope(int scope_depth);
static TypeSpecResult parse_type_specifier(Token *tok);
static Token *emit_expr_to_semicolon(Token *tok);
static Token *emit_orelse_action(Token *tok, Token *var_name, bool has_const, Token *stop_comma);
static Token *emit_return_body(Token *tok, Token *stop);
static Token *try_zero_init_decl(Token *tok);
static Token *skip_pragma_operators(Token *tok);
static inline void out_char(char c);
static inline void out_str(const char *s, int len);
static Token *skip_balanced(Token *tok, char open, char close);
static Token *skip_gnu_attributes(Token *tok);
static Token *skip_c23_attr(Token *tok);
static bool cc_is_msvc(const char *cc);

// Emit space-separated token range [start, end). First token has no leading space.
static inline void emit_token_range(Token *start, Token *end) {
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = t->next) {
		if (t != start) out_char(' ');
		out_str(t->loc, t->len);
	}
}

static inline void clear_func_ret_type(void) {
	ctx->func_ret_type_start = ctx->func_ret_type_end = NULL;
	ctx->func_ret_type_suffix_start = ctx->func_ret_type_suffix_end = NULL;
}

// Skip trailing declarator parts (attrs, array dims, param lists) until '{' or ';'
static Token *skip_declarator_suffix(Token *tok) {
	while (tok && tok->kind != TK_EOF && !equal(tok, "{") && !equal(tok, ";")) {
		if (tok->tag & TT_ATTR)
			tok = skip_gnu_attributes(tok);
		else if (is_c23_attr(tok))
			tok = skip_c23_attr(tok);
		else if (equal(tok, "["))
			tok = skip_balanced(tok, '[', ']');
		else if (equal(tok, "("))
			tok = skip_balanced(tok, '(', ')');
		else
			break;
	}
	return tok;
}

static inline void control_flow_reset(void) {
	ctrl = (ControlFlow){0};
}

static void walker_init(TokenWalker *w, Token *start, int initial_depth) {
	*w = (TokenWalker){.tok = start, .initial_depth = initial_depth, .depth = initial_depth};
}

static void reset_transpiler_state(void) {
	ctx->defer_depth = 0;
	ctx->struct_depth = 0;
	ctx->conditional_block_depth = 0;
	ctx->generic_paren_depth = 0;
	ctx->stmt_expr_count = 0;
	ctx->orelse_guard_count = 0;
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
	control_flow_reset();
	last_emitted = NULL;
	cached_file_idx = -1;
	cached_file = NULL;

	// Reset arena-allocated arrays to prevent stale pointers after arena_reset.
	// After tokenizer_teardown(false) resets the arena, these pointers become
	// dangling. Without clearing them, ARENA_ENSURE_CAP would skip allocation
	// (thinking capacity is sufficient) and write through dangling pointers,
	// corrupting token data in the reused arena blocks.
	defer_stack = NULL;
	defer_stack_capacity = 0;
	stmt_expr_levels = NULL;
	stmt_expr_capacity = 0;
	orelse_guard_levels = NULL;
	orelse_guard_capacity = 0;
	label_table.labels = NULL;
	label_table.count = 0;
	label_table.capacity = 0;
	hashmap_zero(&label_table.name_map);
	hashmap_zero(&longjmp_wrapper_map);
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
	if ((!t || !*t) && TMPDIR_ENVVAR_ALT) t = getenv(TMPDIR_ENVVAR_ALT);
	if (!t || !*t) return TMPDIR_FALLBACK;
	
	size_t len = strlen(t);
	snprintf(buf, sizeof(buf), "%s%s", t, (t[len - 1] == '/' || t[len - 1] == '\\') ? "" : "/");
	return buf;
}

#define match_ch _equal_1

static inline bool is_identifier_like(Token *tok) {
	return tok->kind <= TK_KEYWORD; // TK_IDENT=0, TK_KEYWORD=1
}

static Token *skip_to_semicolon(Token *tok) {
	for (int depth = 0; tok->kind != TK_EOF; tok = tok->next) {
		if (tok->flags & TF_OPEN) depth++;
		else if (tok->flags & TF_CLOSE) depth--;
		else if (depth == 0 && match_ch(tok, ';')) return tok;
	}
	return tok;
}

static Token *skip_balanced(Token *tok, char open, char close) {
	int depth = 1;
	for (tok = tok->next; tok->kind != TK_EOF && depth > 0; tok = tok->next) {
		if (match_ch(tok, open)) depth++;
		else if (match_ch(tok, close)) depth--;
	}
	return tok;
}

// Skip GNU/MSVC-style attributes: __attribute__((...)), __declspec(...)
static Token *skip_gnu_attributes(Token *tok) {
	while (tok && (tok->tag & TT_ATTR)) {
		tok = tok->next;
		if (tok && equal(tok, "(")) tok = skip_balanced(tok, '(', ')');
	}
	return tok;
}

// Skip a single C23 [[ ... ]] attribute. Assumes tok is at first '['.
static Token *skip_c23_attr(Token *tok) {
	return skip_balanced(tok, '[', ']');
}

static Token *skip_all_attributes(Token *tok) {
	while (tok && tok->kind != TK_EOF) {
		if (tok->tag & TT_ATTR) tok = skip_gnu_attributes(tok);
		else if (is_c23_attr(tok)) tok = skip_c23_attr(tok);
		else break;
	}
	return tok;
}

static void out_buf_grow(void) {
	out_buf_cap *= 2;
	if (out_buf == out_buf_static) {
		out_buf = malloc(out_buf_cap);
		if (!out_buf) error("out of memory");
		memcpy(out_buf, out_buf_static, out_buf_pos);
	} else {
		out_buf = realloc(out_buf, out_buf_cap);
		if (!out_buf) error("out of memory");
	}
}

static void out_flush(void) {
	if (oe_buf_checkpoint >= 0) {
		out_buf_grow();
		return;
	}
	if (out_buf_pos > 0) {
		fwrite(out_buf, 1, out_buf_pos, out_fp);
		out_buf_pos = 0;
	}
	if (out_buf != out_buf_static) {
		free(out_buf);
		out_buf = out_buf_static;
		out_buf_cap = OUT_BUF_SIZE;
	}
}

static inline void out_char(char c) {
	if (__builtin_expect(out_buf_pos >= out_buf_cap, 0)) out_flush();
	out_buf[out_buf_pos++] = c;
}

static inline void out_str(const char *s, int len) {
	if (__builtin_expect(len <= 0, 0)) return;
	while (__builtin_expect(out_buf_pos + len >= out_buf_cap, 0)) {
		if (oe_buf_checkpoint >= 0) out_buf_grow();
		else if (len >= out_buf_cap) { out_flush(); fwrite(s, 1, len, out_fp); return; }
		else out_flush();
	}
	memcpy(out_buf + out_buf_pos, s, len);
	out_buf_pos += len;
}

#define OUT_LIT(s) out_str(s, sizeof(s) - 1)

static void out_init(FILE *fp) {
	out_fp = fp;
	out_buf_pos = 0;
	oe_buf_checkpoint = -1;
	// Shrink back to static buffer if grown during speculation
	if (out_buf != out_buf_static) {
		free(out_buf);
		out_buf = out_buf_static;
		out_buf_cap = OUT_BUF_SIZE;
	}
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
		if (!is_reincludable_header(f->name) && hashmap_get(&system_includes, f->name, strlen(f->name))) continue;
		char *inc = arena_strdup(&ctx->main_arena, f->name);
		hashmap_put(&system_includes, inc, strlen(inc), (void *)1);
		ARENA_ENSURE_CAP(&ctx->main_arena,
				 system_include_list,
				 ctx->system_include_count + 1,
				 system_include_capacity,
				 32,
				 char *);
		system_include_list[ctx->system_include_count++] = inc;
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
	// Keys are arena strings in system_include_list
	hashmap_zero(&system_includes);
	system_include_list = NULL;
	ctx->system_include_count = 0;
	system_include_capacity = 0;
}

static void end_statement_after_semicolon(void) {
	ctx->at_stmt_start = true;
	ctrl.for_init = false; // Semicolon ends init clause
	if (ctrl.pending && ctrl.paren_depth == 0) {
		// Pop phantom scopes for braceless control bodies ending via break/continue/return/goto.
		// For-init variables (e.g., "for (int T = 0; ...)  break;") are registered at
		// defer_depth + 1 but never get a matching '}' pop. Without this, the shadow
		// persists and corrupts subsequent typedef lookups (ghost shadow bug).
		typedef_pop_scope(ctx->defer_depth + 1);
		control_flow_reset();
	}
}

static void defer_push_scope(bool consume_flags) {
	// Guard against pathologically deep nesting (e.g., fuzz input with thousands of '{')
	if (ctx->defer_depth >= 4096) error("brace nesting depth exceeds 4096");
	int old_cap = defer_stack_capacity;
	ARENA_ENSURE_CAP(&ctx->main_arena,
			 defer_stack,
			 ctx->defer_depth + 1,
			 defer_stack_capacity,
			 INITIAL_ARRAY_CAP,
			 DeferScope);
	for (int i = old_cap; i < defer_stack_capacity; i++) defer_stack[i] = (DeferScope){0};
	DeferScope *s = &defer_stack[ctx->defer_depth];
	DeferEntry *prev_entries = s->entries;
	int prev_cap = s->capacity;
	*s = (DeferScope){.entries = prev_entries, .capacity = prev_cap};
	if (consume_flags) {
		s->is_loop = ctrl.next_scope & NS_LOOP;
		s->is_switch = ctrl.next_scope & NS_SWITCH;
		s->is_conditional = ctrl.next_scope & NS_CONDITIONAL;
		if (ctrl.next_scope & NS_CONDITIONAL) ctx->conditional_block_depth++;
		ctrl.next_scope = 0;
	}
	ctx->defer_depth++;
}

static void defer_pop_scope(void) {
	if (ctx->defer_depth > 0) {
		ctx->defer_depth--;
		if (defer_stack[ctx->defer_depth].is_conditional) ctx->conditional_block_depth--;
	}
}

static void defer_add(Token *defer_keyword, Token *start, Token *end) {
	if (ctx->defer_depth <= 0) error_tok(start, "defer outside of any scope");
	DeferScope *scope = &defer_stack[ctx->defer_depth - 1];
	ARENA_ENSURE_CAP(&ctx->main_arena,
			 scope->entries,
			 scope->count + 1,
			 scope->capacity,
			 INITIAL_ARRAY_CAP,
			 DeferEntry);
	scope->entries[scope->count++] = (DeferEntry){start, end, defer_keyword};
	scope->had_control_exit = false;
}

static int find_switch_scope(void) {
	for (int d = ctx->defer_depth - 1; d >= 0; d--)
		if (defer_stack[d].is_switch) return d;
	return -1;
}

// Mark unconditional control exit in the innermost switch scope
static void mark_switch_control_exit(void) {
	if (ctrl.pending || ctx->conditional_block_depth > 0) return;
	int sd = find_switch_scope();
	if (sd >= 0) defer_stack[sd].had_control_exit = true;
}

static bool needs_space(Token *prev, Token *tok) {
	if (!prev || tok_at_bol(tok)) return false;
	if (tok_has_space(tok)) return true;
	if ((is_identifier_like(prev) || prev->kind == TK_NUM) &&
	    (is_identifier_like(tok) || tok->kind == TK_NUM))
		return true;
	if (prev->kind != TK_PUNCT || tok->kind != TK_PUNCT) return false;
	
	char a = (prev->len == 1) ? prev->shortcut : prev->loc[prev->len - 1];
	char b = tok->shortcut;
	
	if (b == '=') return strchr("=!<>+-*/%&|^", a) != NULL;
	return (a == b && strchr("+-<>&|#", a)) || (a == '-' && b == '>') || 
	       (a == '/' && b == '*') || (a == '*' && b == '/');
}

// Check if 'tok' is inside an attribute (prevents 'defer' from being treated as keyword)
static bool is_inside_attribute(Token *tok) {
	if (!last_emitted || (!equal(last_emitted, "(") && !equal(last_emitted, ","))) return false;
	int depth = 0;
	for (Token *t = tok; t && t->kind != TK_EOF && !equal(t, ";") && !equal(t, "{"); t = t->next)
		if (equal(t, "(")) depth++;
		else if (equal(t, ")") && --depth < 0)
			return true;
	return false;
}

// Cold path: emit C23 float suffix normalization
static bool __attribute__((noinline)) emit_tok_special(Token *tok) {
	if (tok->kind != TK_NUM) return false;
	const char *replacement;
	int suffix_len = get_extended_float_suffix(tok->loc, tok->len, &replacement);
	if (suffix_len > 0) {
		out_str(tok->loc, tok->len - suffix_len);
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
		line_no = tok->line_no;
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
		out_str(tok->loc, tok->len);
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
	if (__builtin_expect(tok->tag & TT_SUE, 0) && equal(tok, "enum")) {
		Token *brace = find_struct_body_brace(tok);
		if (brace) parse_enum_constants(brace, ctx->defer_depth);
	}

	out_str(tok->loc, tok->len);
	last_emitted = tok;
}

static void emit_range(Token *start, Token *end) {
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = t->next) emit_tok(t);
}

// Like emit_range but processes zero-init/raw/orelse inside defer blocks.
static void emit_deferred_range(Token *start, Token *end) {
	bool saved_stmt_start = ctx->at_stmt_start;
	ControlFlow saved_ctrl = ctrl;
	ctrl = (ControlFlow){0};

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
			t = t->next;
			ctx->at_stmt_start = true;
			continue;
		}

		emit_tok(t);
		t = t->next;
	}

	ctx->at_stmt_start = saved_stmt_start;
	ctrl = saved_ctrl;
}

static void emit_defers_ex(DeferEmitMode mode, int stop_depth) {
	if (ctx->defer_depth <= 0) return;

	for (int d = ctx->defer_depth - 1; d >= 0; d--) {
		if (mode == DEFER_SCOPE && d < ctx->defer_depth - 1) break;
		if (mode == DEFER_TO_DEPTH && d < stop_depth) break;

		DeferScope *scope = &defer_stack[d];
		for (int i = scope->count - 1; i >= 0; i--) {
			out_char(' ');
			emit_deferred_range(scope->entries[i].stmt, scope->entries[i].end);
			out_char(';');
		}

		if (mode == DEFER_BREAK && (scope->is_loop || scope->is_switch)) break;
		if (mode == DEFER_CONTINUE && scope->is_loop) break;
	}
}

static bool has_defers_for(DeferEmitMode mode, int stop_depth) {
	for (int d = ctx->defer_depth - 1; d >= 0; d--) {
		if (mode == DEFER_TO_DEPTH && d < stop_depth) break;
		if (defer_stack[d].count > 0) return true;
		if (mode == DEFER_BREAK && (defer_stack[d].is_loop || defer_stack[d].is_switch))
			return false;
		if (mode == DEFER_CONTINUE && defer_stack[d].is_loop) return false;
	}
	return false;
}

static void label_table_add(char *name, int name_len, int scope_depth, Token *tok, Token *block_open) {
	ARENA_ENSURE_CAP(&ctx->main_arena,
			 label_table.labels,
			 label_table.count + 1,
			 label_table.capacity,
			 INITIAL_ARRAY_CAP,
			 LabelInfo);
	LabelInfo *info = &label_table.labels[label_table.count++];
	info->name = name;
	info->name_len = name_len;
	info->scope_depth = scope_depth;
	info->tok = tok;
	info->block_open = block_open;
	hashmap_put(&label_table.name_map, name, name_len, (void *)(intptr_t)(scope_depth + 1));
}

static int label_table_lookup(char *name, int name_len) {
	void *val = hashmap_get(&label_table.name_map, name, name_len);
	return val ? (int)(intptr_t)val - 1 : -1;
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
	if (kind == TDK_TYPEDEF) {
		int existing = typedef_get_index(name, len);
		if (existing >= 0) {
			TypedefEntry *prev = &typedef_table.entries[existing];
			if (prev->scope_depth == scope_depth && !prev->is_shadow && !prev->is_enum_const)
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
			hashmap_delete(&typedef_table.name_map, e->name, e->len);
		typedef_table.count--;
	}
}

// Register enum constants as typedef shadows. tok points to opening '{'.
static void parse_enum_constants(Token *tok, int scope_depth) {
	if (!tok || !equal(tok, "{")) return;
	tok = tok->next; // Skip '{'

	while (tok && tok->kind != TK_EOF && !equal(tok, "}")) {
		if (tok->tag & TT_ATTR) {
			tok = skip_gnu_attributes(tok);
			continue;
		}
		if (is_c23_attr(tok)) {
			tok = skip_c23_attr(tok);
			continue;
		}

		if (is_valid_varname(tok)) {
			typedef_add_enum_const(tok->loc, tok->len, scope_depth);
			tok = tok->next;

			if (tok && equal(tok, "=")) {
				tok = tok->next;
				int depth = 0;
				while (tok && tok->kind != TK_EOF) {
					if (tok->flags & TF_OPEN) depth++;
					else if (tok->flags & TF_CLOSE) {
						if (depth > 0) depth--;
						else if (match_ch(tok, '}'))
							break;
					} else if (depth == 0 && equal(tok, ","))
						break;
					tok = tok->next;
				}
			}

			if (tok && equal(tok, ",")) tok = tok->next;
		} else {
			tok = tok->next;
		}
	}
}

static TypedefEntry *typedef_lookup(Token *tok) {
	if (!is_identifier_like(tok)) return NULL;
	int idx = typedef_get_index(tok->loc, tok->len);
	return idx >= 0 ? &typedef_table.entries[idx] : NULL;
}

// Typedef query flags (single lookup, check multiple properties)
enum { TDF_TYPEDEF = 1, TDF_VLA = 2, TDF_VOID = 4, TDF_ENUM_CONST = 8, TDF_CONST = 16, TDF_PTR = 32, TDF_ARRAY = 64 };

static inline int typedef_flags(Token *tok) {
	TypedefEntry *e = typedef_lookup(tok);
	if (!e) return 0;
	if (e->is_enum_const) return TDF_ENUM_CONST;
	if (e->is_shadow) return 0;
	if (e->is_vla_var) return TDF_VLA;
	return TDF_TYPEDEF | (e->is_vla ? TDF_VLA : 0) | (e->is_void ? TDF_VOID : 0) |
	       (e->is_const ? TDF_CONST : 0) | (e->is_ptr ? TDF_PTR : 0) |
	       (e->is_array ? TDF_ARRAY : 0);
}

#define is_known_typedef(tok) (typedef_flags(tok) & TDF_TYPEDEF)
#define is_vla_typedef(tok) (typedef_flags(tok) & TDF_VLA)
#define is_void_typedef(tok) (typedef_flags(tok) & TDF_VOID)
#define is_known_enum_const(tok) (typedef_flags(tok) & TDF_ENUM_CONST)
#define is_const_typedef(tok) (typedef_flags(tok) & TDF_CONST)
#define is_ptr_typedef(tok) (typedef_flags(tok) & TDF_PTR)
#define is_array_typedef(tok) (typedef_flags(tok) & TDF_ARRAY)

static bool is_typedef_like(Token *tok);  // forward declaration

static Token *find_boundary_comma(Token *tok) {
	for (int depth = 0; tok->kind != TK_EOF; tok = tok->next) {
		if (tok->flags & TF_OPEN) depth++;
		else if (tok->flags & TF_CLOSE) depth--;
		else if (depth == 0 && match_ch(tok, ',')) {
			// Verify following token can start a declarator (*, (, qualifier, or identifier).
			// If not, this is a comma operator, not a declarator separator.
			Token *n = tok->next;
			if (!n) continue;
		if (match_ch(n, '(')) {
				Token *inside = n->next;
				if (inside && !(inside->tag & (TT_TYPE | TT_SUE | TT_TYPEOF | TT_QUALIFIER))
				    && !is_typedef_like(inside) && !is_c23_attr(inside))
					return tok;
			} else if (match_ch(n, '*') || (n->tag & TT_QUALIFIER)) {
				return tok;
			} else if (equal(n, "_Pragma") || is_c23_attr(n)) {
				return tok;
			} else if (is_valid_varname(n) && !(n->tag & (TT_TYPE | TT_SUE | TT_TYPEOF))) {
				if (n->next && match_ch(n->next, '(')) {
					Token *inside = n->next->next;
					if (inside &&
					    (inside->tag & (TT_TYPE | TT_SUE | TT_TYPEOF | TT_QUALIFIER)))
						return tok;
				} else
					return tok;
			}
		}
		else if (depth == 0 && match_ch(tok, ';')) return NULL;
	}
	return NULL;
}

static inline bool is_typedef_heuristic(Token *tok) {
	if (tok->kind != TK_IDENT || tok->len < 2) return false;
	if (tok->loc[tok->len - 2] == '_' && tok->loc[tok->len - 1] == 't') return true;
	return tok->loc[0] == '_' && tok->loc[1] == '_' && !(tok->next && equal(tok->next, "("));
}

// Known typedef or system typedef heuristic (e.g., size_t, __rlim_t)
static bool is_typedef_like(Token *tok) {
	if (!is_identifier_like(tok)) return false;
	if (is_known_typedef(tok)) return true;
	// Check if a shadow was registered (variable with a heuristic-type name).
	TypedefEntry *e = typedef_lookup(tok);
	if (e && (e->is_shadow || e->is_vla_var)) return false;
	return is_typedef_heuristic(tok);
}

// Find opening brace of a struct/union/enum body, or NULL if no body.
static Token *find_struct_body_brace(Token *tok) {
	Token *t = tok->next;
	while (t && t->kind != TK_EOF) {
		if (equal(t, "_Pragma") && t->next && equal(t->next, "(")) {
			t = skip_pragma_operators(t);
			continue;
		}
		if (t->kind == TK_PREP_DIR) {
			t = t->next;
			continue;
		}
		if (is_valid_varname(t) || (t->tag & TT_QUALIFIER)) {
			t = t->next;
		} else {
			Token *next = skip_all_attributes(t);
			if (next == t) break;
			t = next;
		}
	}
	return (t && equal(t, "{")) ? t : NULL;
}

static bool walker_next(TokenWalker *w) {
	if (!w->tok || w->tok->kind == TK_EOF) return false;

	if (match_ch(w->tok, '{')) w->depth++;
	else if (match_ch(w->tok, '}')) {
		if (w->depth <= w->initial_depth) return false;
		w->depth--;
	}

	return true;
}

static inline void walker_advance(TokenWalker *w) {
	if (match_ch(w->tok, '?')) w->ternary_depth++;

	w->prev = w->tok;
	
	// Skip struct/union/enum bodies
	if (w->tok->tag & TT_SUE) {
		Token *brace = find_struct_body_brace(w->tok);
		if (brace) {
			w->tok = skip_balanced(brace, '{', '}');
			return;
		}
	}
	
	// Skip _Generic(...)
	if (w->tok->tag & TT_GENERIC) {
		w->tok = w->tok->next;
		if (w->tok && equal(w->tok, "(")) w->tok = skip_balanced(w->tok, '(', ')');
		return;
	}
	
	w->tok = w->tok->next;
}

// Check if token is a goto label (ident followed by ':'), filtering ternary/case/bitfields.
static Token *walker_check_label(TokenWalker *w) {
	if (!is_identifier_like(w->tok)) return NULL;
	Token *t = skip_all_attributes(w->tok->next);
	if (!t || !equal(t, ":")) return NULL;
	if (t->next && equal(t->next, ":")) return NULL;
	if (w->prev && equal(w->prev, "?")) return NULL;
	if (w->ternary_depth > 0) { w->ternary_depth--; return NULL; }
	if (w->prev && (w->prev->tag & (TT_CASE | TT_DEFAULT))) return NULL;
	return w->tok;
}

// ============================================================================

static void register_longjmp_wrapper(char *name, int len) {
	hashmap_put(&longjmp_wrapper_map, name, len, (void *)1);
}

static bool is_longjmp_wrapper(char *name, int len) {
	return hashmap_get(&longjmp_wrapper_map, name, len) != NULL;
}

// Pre-scan top-level function bodies to register direct longjmp wrappers.
// Only registers functions whose bodies directly contain TT_SPECIAL_FN tokens.
// Deeper transitive chains are not tracked to avoid poisoning in self-hosted builds.
static void prescan_longjmp_wrappers(Token *tok) {
	int depth = 0;
	Token *func_name = NULL;
	Token *prev = NULL;
	for (Token *t = tok; t && t->kind != TK_EOF; t = t->next) {
		if (depth == 0) {
			if (is_identifier_like(t) && t->next && equal(t->next, "(") &&
			    !(t->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_ATTR)))
				func_name = t;
			if (equal(t, "{") && prev && (equal(prev, ")") ||
			    (func_name && is_identifier_like(prev)))) {
				// Scan function body for longjmp calls
				bool has_longjmp = false;
				int bd = 1;
				for (Token *b = t->next; b && b->kind != TK_EOF; b = b->next) {
					if (equal(b, "{")) bd++;
					else if (equal(b, "}")) { if (--bd <= 0) { t = b; break; } }
					if (func_name && !has_longjmp &&
					    (b->tag & TT_SPECIAL_FN) && !equal(b, "vfork"))
						has_longjmp = true;
				}
				if (has_longjmp)
					register_longjmp_wrapper(func_name->loc, func_name->len);
				func_name = NULL;
				prev = t;
				continue;
			}
		} else {
			if (equal(t, "{")) depth++;
			else if (equal(t, "}")) depth--;
		}
		prev = t;
	}
}

// Scan function body for labels and detect setjmp/longjmp/vfork/asm.
// Two-phase: quick pre-scan for goto/setjmp/vfork/asm, then full label scan if needed.
static void scan_labels_in_function(Token *tok) {
	label_table.count = 0;
	hashmap_zero(&label_table.name_map);
	ctx->current_func_has_setjmp = false;
	ctx->current_func_has_asm = false;
	ctx->current_func_has_vfork = false;
	if (!tok || !equal(tok, "{")) return;

	// Track most recent '{' at each depth for label block_open tracking.
	#define BLOCK_STACK_MAX 256
	Token *block_stack[BLOCK_STACK_MAX];
	block_stack[0] = NULL;
	block_stack[1] = tok; // depth 1 = the function body's '{'

	TokenWalker w;
	walker_init(&w, tok->next, 1);
	while (walker_next(&w)) {
		if (match_ch(w.tok, '{') && w.depth < BLOCK_STACK_MAX)
			block_stack[w.depth] = w.tok;

		uint32_t tg = w.tok->tag;
		if (tg & TT_SPECIAL_FN) {
			if (equal(w.tok, "vfork")) ctx->current_func_has_vfork = true;
			else ctx->current_func_has_setjmp = true;
		}
		if (tg & TT_ASM) ctx->current_func_has_asm = true;

		if (!ctx->current_func_has_setjmp &&
		    is_identifier_like(w.tok) && w.tok->next && equal(w.tok->next, "(") &&
		    is_longjmp_wrapper(w.tok->loc, w.tok->len)) {
			ctx->current_func_has_setjmp = true;
		}

		Token *label = walker_check_label(&w);
		if (label) {
			Token *bo = (w.depth > 0 && w.depth < BLOCK_STACK_MAX) ? block_stack[w.depth] : NULL;
			label_table_add(label->loc, label->len, w.depth, label, bo);
		}
		walker_advance(&w);
	}
	#undef BLOCK_STACK_MAX
}

// Quick pre-check: is this a variable declaration (not a function decl or stmt expr)?
static bool is_var_declaration(Token *type_end) {
	DeclResult decl = parse_declarator(type_end, false);
	if (!decl.var_name || !decl.end) return false;

	// Statement expression initializer: type name = ({...})
	// For multi-declarator decls, skip past the stmt-expr and check for ','.
	if (equal(decl.end, "=")) {
		Token *after_eq = decl.end->next;
		if (after_eq && equal(after_eq, "(") && after_eq->next && equal(after_eq->next, "{")) {
			Token *after_stmt_expr = skip_balanced(after_eq, '(', ')');
			while (after_stmt_expr && after_stmt_expr->kind != TK_EOF &&
			       (after_stmt_expr->tag & TT_ATTR))
				after_stmt_expr = skip_gnu_attributes(after_stmt_expr);
			return equal(after_stmt_expr, ",");
		}
		return true;
	}

	return equal(decl.end, ",") || equal(decl.end, ";");
}

static inline bool is_orelse_keyword(Token *tok) {
	return (tok->tag & TT_ORELSE) && !is_known_typedef(tok) &&
	       !(last_emitted && (last_emitted->tag & TT_MEMBER));
}

// Emit type tokens, optionally stripping const and struct/enum bodies.
static void emit_type_stripped(Token *start, Token *end, bool strip_const) {
	for (Token *t = start; t != end; t = t->next) {
		if (strip_const && (t->tag & TT_CONST)) continue;
		if (equal(t, "{")) { t = skip_balanced(t, '{', '}'); if (t == end) break; }
		emit_tok(t);
	}
}

static inline bool is_file_storage_keyword(Token *tok) {
	return equal(tok, "extern") || equal(tok, "typedef") ||
	       equal(tok, "static") || equal(tok, "_Thread_local") || equal(tok, "thread_local");
}

// Emit expression tokens to ';', stop token, or 'orelse' (if check_orelse).
static Token *emit_expr_to_stop(Token *tok, Token *stop, bool check_orelse) {
	int depth = 0;
	while (tok->kind != TK_EOF) {
		if (tok->flags & TF_OPEN) depth++;
		else if (tok->flags & TF_CLOSE) depth--;
		else if (depth == 0 && (equal(tok, ";") || (stop && tok == stop))) break;
		if (check_orelse && depth == 0 && is_orelse_keyword(tok)) break;
		emit_tok(tok);
		tok = tok->next;
	}
	return tok;
}

// Returns true if 'raw' is followed by a declaration context (type keyword, typedef, *, etc.)
static bool is_raw_declaration_context(Token *after_raw) {
	while (is_c23_attr(after_raw)) after_raw = skip_c23_attr(after_raw);
	after_raw = skip_pragma_operators(after_raw);
	while (after_raw && after_raw->kind == TK_PREP_DIR) after_raw = after_raw->next;
	return after_raw && (is_type_keyword(after_raw) || is_known_typedef(after_raw) ||
			     equal(after_raw, "*") || (after_raw->tag & (TT_QUALIFIER | TT_SUE)));
}

// Forward goto scope exit count: how many levels the path dips below starting depth.
// Needed because depth-based defer can't distinguish sibling scopes.
static int forward_goto_scope_exits(Token *after_goto_label, char *label_name, int label_len) {
	int depth = 0;
	int min_depth = 0;
	int ternary_depth = 0;
	Token *prev = NULL;
	for (Token *t = after_goto_label; t && t->kind != TK_EOF; t = t->next) {
		if (match_ch(t, '{')) depth++;
		else if (match_ch(t, '}')) {
			if (--depth < min_depth) min_depth = depth;
		}
		if (match_ch(t, '?')) ternary_depth++;
		if (is_identifier_like(t) && t->len == label_len &&
		    !memcmp(t->loc, label_name, label_len)) {
			Token *n = t->next;
			n = skip_all_attributes(n);
			if (n && equal(n, ":") && !(n->next && equal(n->next, ":"))) {
				if (prev && equal(prev, "?")) continue;
				if (ternary_depth > 0) { ternary_depth--; continue; }
				return -min_depth;
			}
		}
		prev = t;
	}
	return 0; // label not found (backward goto or end of function)
}

// Backward goto scope exit count: scans from label to goto_tok tracking brace depth.
static int backward_goto_scope_exits(Token *goto_tok, char *label_name, int label_len) {
	for (int i = 0; i < label_table.count; i++) {
		LabelInfo *info = &label_table.labels[i];
		if (info->name_len == label_len && !memcmp(info->name, label_name, label_len)) {
			if (info->tok->loc >= goto_tok->loc) return 0;
			int depth = 0, min_depth = 0;
			for (Token *t = info->tok; t && t != goto_tok && t->kind != TK_EOF; t = t->next) {
				if (match_ch(t, '{')) depth++;
				else if (match_ch(t, '}')) {
					if (--depth < min_depth) min_depth = depth;
				}
			}
			return -min_depth;
		}
	}
	return 0;
}

// Check if backward goto re-enters a block past zero-init declarations.
// Returns the first skipped declaration token, or NULL if safe.
static Token *backward_goto_skips_decl(Token *goto_tok, char *label_name, int label_len) {
	if (!FEAT(F_ZEROINIT | F_DEFER)) return NULL;
	for (int i = 0; i < label_table.count; i++) {
		LabelInfo *info = &label_table.labels[i];
		if (info->name_len != label_len || memcmp(info->name, label_name, label_len)) continue;
		if (info->tok->loc >= goto_tok->loc) return NULL;
		// NULL block_open at depth > 0 means tracking limit exceeded.
		if (!info->block_open) {
			if (info->scope_depth > 0)
				error_tok(goto_tok,
					  "backward goto to '%.*s' in deeply nested block "
					  "(depth %d exceeds tracking limit); "
					  "reduce nesting or restructure code",
					  label_len, label_name, info->scope_depth);
			return NULL;
		}

		// Check if goto is inside the label's block (backward loop = safe).
		Token *block_close = skip_balanced(info->block_open, '{', '}');
		if (block_close && goto_tok->loc < block_close->loc &&
		    goto_tok->loc > info->block_open->loc)
			return NULL;

		// Goto re-enters the block — scan for declarations at depth 0.
		Token *found_decl = NULL;
		int depth = 0;
		bool is_stmt_start = true;
		for (Token *t = info->block_open->next; t && t != info->tok && t->kind != TK_EOF; t = t->next) {
			if (match_ch(t, '{')) { depth++; is_stmt_start = true; continue; }
			if (match_ch(t, '}')) { depth--; is_stmt_start = true; continue; }
			if (match_ch(t, ';') || match_ch(t, ':')) { is_stmt_start = true; continue; }

					if (depth == 0 && is_stmt_start) {
				Token *s = t;
				if (equal(s, "raw") && !is_known_typedef(s)) s = s->next;
				if (s && equal(s, "_Pragma") && s->next && equal(s->next, "("))
					s = skip_pragma_operators(s);
				if (s && !is_file_storage_keyword(s)) {
					TypeSpecResult type = parse_type_specifier(s);
					if (type.saw_type && is_var_declaration(type.end)) {
						found_decl = t;
						break; // One is enough to report
					}
				}
			}
			is_stmt_start = false;
		}
		return found_decl;
	}
	return NULL;
}

// Check if a forward goto would skip over defer statements or variable declarations.
// Returns both results in one walk. Either field NULL means safe for that check.
//
// Key rule: if we find the label BEFORE exiting the scope containing the item, it's invalid.
// A goto that jumps OVER an entire block is fine; jumping INTO a block past a defer/decl is not.
static GotoSkipResult
goto_skips_check(Token *goto_tok, char *label_name, int label_len, bool check_defer, bool check_decl) {
	GotoSkipResult r = {NULL, NULL};
	if (check_decl && !FEAT(F_ZEROINIT | F_DEFER)) check_decl = false;
	if (!check_defer && !check_decl) return r;

	Token *start = goto_tok->next->next; // skip 'goto' and label name
	if (start && equal(start, ";")) start = start->next;

	Token *active_defer = NULL, *active_decl = NULL;
	int defer_depth = -1, decl_depth = -1;
	bool is_stmt_start = true;

	TokenWalker w;
	walker_init(&w, start, 0);

	while (walker_next(&w)) {
		// For-loop init detection (for decl check)
		// Skip entire for-header (...) and check for decl in the init clause.
		// For-init vars (e.g. `int i` in `for (int i = 0; ...)`) are scoped to
		// the for body per C99+. Record at depth+1 so the decl is cleared when
		// leaving the for body scope — a goto over the entire for is safe.
		if (check_decl && (w.tok->tag & TT_LOOP) && equal(w.tok, "for")) {
			walker_advance(&w);
			if (w.tok && equal(w.tok, "(")) {
				Token *paren = w.tok;
				Token *after_paren = skip_balanced(paren, '(', ')');

				// Check first clause (between '(' and first ';') for a decl
				Token *t = paren->next;
				if (t && !equal(t, ";")) {
					Token *s = t;
					if (equal(s, "raw") && !is_known_typedef(s)) s = s->next;
					if (s && !is_file_storage_keyword(s)) {
						TypeSpecResult type = parse_type_specifier(s);
						if (type.saw_type && is_var_declaration(type.end)) {
							if (!active_decl || w.depth + 1 <= decl_depth) {
								active_decl = t;
								decl_depth = w.depth + 1;
							}
						}
					}
				}

				w.prev = paren;
				w.tok = after_paren;
				is_stmt_start = true;
				continue;
			}
			is_stmt_start = false;
			continue;
		}

		// Consolidated structural token handling: {, }, ;
		if (match_ch(w.tok, '{') || match_ch(w.tok, '}') || match_ch(w.tok, ';')) {
			if (match_ch(w.tok, '}')) {
				if (active_defer && w.depth < defer_depth) { active_defer = NULL; defer_depth = -1; }
				if (active_decl && w.depth < decl_depth) { active_decl = NULL; decl_depth = -1; }
			} else if (match_ch(w.tok, ';')) {
				// Clear decls whose scope has ended (for-init decls in braceless for bodies)
				if (active_decl && w.depth < decl_depth) { active_decl = NULL; decl_depth = -1; }
			}
			is_stmt_start = true;
			walker_advance(&w);
			continue;
		}

		// Track defers
		if (check_defer) {
			bool is_var = w.prev && (is_type_keyword(w.prev) || equal(w.prev, "*") ||
						 (w.prev->tag & TT_QUALIFIER) ||
						 equal(w.prev, "__restrict") || equal(w.prev, ","));
			if ((w.tok->tag & TT_DEFER) && !equal(w.tok->next, ":") &&
			    !(w.prev && (w.prev->tag & TT_MEMBER)) && !is_var &&
			    !(w.tok->next && (w.tok->next->tag & TT_ASSIGN))) {
				// Only flag defers at depth <= 0 (same scope as the goto).
				// Defers at depth > 0 are inside blocks being entered —
				// they fire at scope exit regardless of how the block was
				// entered, so jumping into a block past a defer is safe.
				if (w.depth <= 0 && (!active_defer || w.depth <= defer_depth)) {
					active_defer = w.tok;
					defer_depth = w.depth;
				}
			}
		}

		// Track declarations
		if (check_decl && is_stmt_start) {
			Token *decl_start = w.tok;
			Token *t = w.tok;
			bool has_raw = false;
			if (equal(t, "raw") && !is_known_typedef(t)) {
				has_raw = true;
				t = t->next;
			}
			if (t && t->len == 7 && equal(t, "_Pragma") && t->next && equal(t->next, "("))
				t = skip_pragma_operators(t);
			// static/thread_local: always initialized before startup, safe to jump over.
			if (!is_file_storage_keyword(t)) {
				TypeSpecResult type = parse_type_specifier(t);
				if (type.saw_type && is_var_declaration(type.end)) {
					if (!has_raw && (!active_decl || w.depth <= decl_depth)) {
						active_decl = decl_start;
						decl_depth = w.depth;
					}
				}
			}
		}

		if (is_identifier_like(w.tok) && w.tok->len == label_len &&
		    !memcmp(w.tok->loc, label_name, label_len)) {
			Token *label = walker_check_label(&w);
			if (label) {
				r.skipped_defer = active_defer;
				r.skipped_decl = active_decl;
				return r;
			}
		}

		is_stmt_start = false;
		walker_advance(&w);
	}

	return r;
}

// K&R parameter declarations: type ident [, ident]* ; ... repeated until {
static bool is_knr_params(Token *after_paren, Token *brace) {
	Token *t = after_paren;
	bool saw_any = false;
	while (t && t != brace && t->kind != TK_EOF) {
		TypeSpecResult type = parse_type_specifier(t);
		if (!type.saw_type) return false;
		t = type.end;
		bool saw_ident = false;
		while (t && t != brace && !equal(t, ";")) {
			DeclResult decl = parse_declarator(t, false);
			if (!decl.var_name) return false;
			saw_ident = true;
			t = decl.end;
			if (t && equal(t, ",")) t = t->next;
		}
		if (!saw_ident || !equal(t, ";")) return false;
		t = t->next;
		saw_any = true;
	}
	return saw_any && t == brace;
}

static Token *skip_func_specifiers(Token *tok) {
	Token *next;
	while (tok && tok->kind != TK_EOF) {
		if (tok->tag & (TT_QUALIFIER | TT_SKIP_DECL | TT_INLINE)) tok = tok->next;
		else if (equal(tok, "_Pragma")) tok = skip_pragma_operators(tok);
		else if ((next = skip_all_attributes(tok)) != tok) tok = next;
		else break;
	}
	return tok;
}

// Check if token starts a void function declaration.
static bool is_void_function_decl(Token *tok) {
	tok = skip_func_specifiers(tok);
	if (!tok) return false;

	if (tok->tag & TT_TYPEOF) {
		Token *t = tok->next;
		if (t && equal(t, "(") && t->next && equal(t->next, "void") && t->next->next && equal(t->next->next, ")"))
			tok = t->next->next->next;
		else
			return false;
	} else if (equal(tok, "void") || is_void_typedef(tok)) {
		tok = tok->next;
	} else {
		return false;
	}

	if (tok && equal(tok, "*")) return false;

	tok = skip_func_specifiers(tok);

	// Handle simple case: void foo()
	if (tok && is_valid_varname(tok) && tok->next && equal(tok->next, "(")) return true;

	// Parenthesized case: void (foo)() or void (__attr foo)()
	if (tok && equal(tok, "(")) {
		Token *inner = tok->next;
		while (inner && inner->kind != TK_EOF && (inner->tag & TT_ATTR)) {
			inner = skip_gnu_attributes(inner);
		}
		if (inner && is_valid_varname(inner) && inner->next && equal(inner->next, ")")) {
			Token *after = inner->next->next;
			return after && equal(after, "(");
		}
	}

	return false;
}

// Capture non-void function return type for defer return variable.
static bool try_capture_func_return_type(Token *tok) {
	// Skip storage class specifiers, function specifiers, and attributes.
	while (tok && tok->kind != TK_EOF) {
		if ((tok->tag & (TT_SKIP_DECL | TT_INLINE)) || 
		    equal(tok, "_Noreturn") || equal(tok, "noreturn")) {
			tok = tok->next;
			continue;
		}
		if (equal(tok, "_Pragma")) {
			tok = skip_pragma_operators(tok);
			continue;
		}
		Token *next = skip_all_attributes(tok);
		if (next == tok) break;
		tok = next;
	}
	if (!tok || tok->kind == TK_EOF) return false;

	Token *type_start = tok;

	// void return type — not captured unless it's void* or void(complex_decl).
	if (equal(tok, "void") || is_void_typedef(tok)) {
		Token *after_void = tok->next;
		if (after_void && !equal(after_void, "*") && !equal(after_void, "("))
			return false;
	}
	if (tok->tag & TT_TYPEOF) {
		Token *t = tok->next;
		if (t && equal(t, "(") && t->next && equal(t->next, "void") && t->next->next &&
		    equal(t->next->next, ")"))
			return false;
	}

	// Parse the type specifier (int, struct Foo, size_t, typeof(...), etc.)
	TypeSpecResult type = parse_type_specifier(tok);
	if (!type.saw_type) return false;

	tok = type.end;

	// Reject anonymous struct/union/enum — re-emitting creates a second incompatible type.
	if (type.is_struct) {
		for (Token *t = type_start; t && t != tok && t->kind != TK_EOF; t = t->next)
			if (equal(t, "{")) return false;
	}

	// Skip pointer decorations and qualifiers: *, const, volatile, restrict
	while (tok && tok->kind != TK_EOF &&
	       (equal(tok, "*") || (tok->tag & TT_QUALIFIER) ||
		equal(tok, "restrict") || equal(tok, "__restrict") || equal(tok, "__restrict__")))
		tok = tok->next;

	// Skip any trailing attributes between pointer/qualifiers and name
	tok = skip_all_attributes(tok);

	// Simple case: function name followed by (
	if (tok && is_valid_varname(tok) && tok->next && equal(tok->next, "(")) {
		ctx->func_ret_type_start = type_start;
		ctx->func_ret_type_end = tok;
		ctx->func_ret_type_suffix_start = ctx->func_ret_type_suffix_end = NULL;
		return true;
	}

	// Complex declarator: TYPE (* FUNCNAME(PARAMS))(ARGS) or deeper nesting.
	if (tok && equal(tok, "(")) {
		Token *outer_open = tok;
		Token *inner = tok->next;
		while (inner && inner->kind != TK_EOF &&
		       (equal(inner, "*") || (inner->tag & (TT_CONST | TT_VOLATILE)) ||
			equal(inner, "restrict") || equal(inner, "__restrict") ||
			equal(inner, "__restrict__") || (inner->tag & TT_ATTR))) {
			inner = (inner->tag & TT_ATTR) ? skip_gnu_attributes(inner) : inner->next;
		}
		// Recurse through nested declarator groups
		while (inner && equal(inner, "(")) {
			inner = inner->next;
			while (inner && inner->kind != TK_EOF &&
			       (equal(inner, "*") || (inner->tag & (TT_CONST | TT_VOLATILE)) ||
				equal(inner, "restrict") || equal(inner, "__restrict") ||
				equal(inner, "__restrict__") || (inner->tag & TT_ATTR))) {
				inner = (inner->tag & TT_ATTR) ? skip_gnu_attributes(inner) : inner->next;
			}
		}
		if (inner && is_valid_varname(inner) && inner->next && equal(inner->next, "(")) {
			Token *after_params = skip_balanced(inner->next, '(', ')');
			if (after_params && equal(after_params, ")")) {
				Token *decl_end = skip_balanced(outer_open, '(', ')');
				decl_end = skip_declarator_suffix(decl_end);
				ctx->func_ret_type_start = type_start;
				ctx->func_ret_type_end = inner;
				ctx->func_ret_type_suffix_start = after_params;
				ctx->func_ret_type_suffix_end = decl_end;
				return true;
			}
		}
	}

	return false;
}

// Emit captured return type. Complex declarators get a typedef.
// Falls back to __auto_type (GCC/Clang) or void* (MSVC) if capture failed.
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
			     t = t->next) {
				out_char(' ');
				out_str(t->loc, t->len);
			}
			OUT_LIT("; _Prism_ret_t_");
			out_uint(ctx->ret_counter);
		} else {
			emit_token_range(ctx->func_ret_type_start, ctx->func_ret_type_end);
		}
	} else {
		if (cc_is_msvc(ctx->extra_compiler))
			OUT_LIT("void *");
		else
			OUT_LIT("__auto_type");
	}
}

// Check if array dimension contains a VLA expression (runtime variable).
// sizeof/alignof/offsetof args are skipped except sizeof(VLA_Typedef).
static bool array_size_is_vla(Token *open_bracket) {
	Token *tok = open_bracket->next;

	while (tok && tok->kind != TK_EOF && !equal(tok, "]")) {
		if (equal(tok, "["))
		{
			if (array_size_is_vla(tok)) return true;
			tok = skip_balanced(tok, '[', ']');
			continue;
		}

		// _Generic: conservatively assume VLA (runtime vs constant undecidable at token level).
		if (tok->tag & TT_GENERIC) {
			return true;
		}

		if (equal(tok, "_Pragma")) { tok = skip_pragma_operators(tok); continue; }
		if (tok->kind == TK_PREP_DIR) { tok = tok->next; continue; }

		// sizeof/alignof: skip argument but check for VLA typedef and inner VLA types.
		if (equal(tok, "sizeof") || equal(tok, "_Alignof") || equal(tok, "alignof")) {
			bool is_sizeof = equal(tok, "sizeof");
			tok = tok->next;
			if (tok && equal(tok, "(")) {
				Token *end = skip_balanced(tok, '(', ')');
				if (is_sizeof) {
					// Scan sizeof(...) for VLA usage; skip enum bodies and track prev for array vs index.
					Token *prev_inner = tok;
					for (Token *inner = tok->next; inner && inner != end; prev_inner = inner, inner = inner->next) {
						if (equal(inner, "enum")) {
							Token *brace = find_struct_body_brace(inner);
							if (brace) {
								parse_enum_constants(brace, ctx->defer_depth);
								inner = skip_balanced(brace, '{', '}');
								if (inner == end) break;
								continue;
							}
						}
						if (is_vla_typedef(inner)) return true;
						if (equal(inner, "[") &&
						    (is_type_keyword(prev_inner) || is_known_typedef(prev_inner) ||
						     equal(prev_inner, "]") || equal(prev_inner, "*") ||
						     equal(prev_inner, ")")) &&
						    array_size_is_vla(inner)) {
							return true;
						}
						// Detect function calls returning VLA pointers (e.g. sizeof(*func(n))).
						if (is_valid_varname(inner) && !is_type_keyword(inner) &&
						    !is_known_typedef(inner) && !is_known_enum_const(inner) &&
						    inner->next && inner != end && equal(inner->next, "(")) {
							Token *call_end = skip_balanced(inner->next, '(', ')');
							bool is_deref = equal(prev_inner, "*") ||
							    (call_end && call_end != end &&
							     (equal(call_end, "[") || equal(call_end, "->") || equal(call_end, ".")));
							if (is_deref) {
								for (Token *a = inner->next->next; a && a != call_end; a = a->next) {
									if (is_valid_varname(a) && !is_known_enum_const(a) &&
									    !is_type_keyword(a)) {
										return true;
									}
								}
							}
							prev_inner = inner;
							inner = call_end;
							if (!inner || inner == end) break;
						}
					}
				}
				tok = end;
				if (tok && equal(tok, "{"))
					tok = skip_balanced(tok, '{', '}');
			} else if (tok && !equal(tok, "]")) {
				// Unparenthesized sizeof/alignof: skip prefix unary ops
				while (tok && !equal(tok, "]") &&
				       (equal(tok, "*") || equal(tok, "&") || equal(tok, "!") ||
				        equal(tok, "~") || equal(tok, "++") || equal(tok, "--") ||
				        equal(tok, "+") || equal(tok, "-") ||
				        equal(tok, "sizeof") || equal(tok, "_Alignof") || equal(tok, "alignof")))
					tok = tok->next;
				while (tok && !equal(tok, "]")) {
					if (equal(tok, "_Pragma")) { tok = skip_pragma_operators(tok); continue; }
					if (tok->kind == TK_PREP_DIR) { tok = tok->next; continue; }
					break;
				}
				if (tok && !equal(tok, "]")) {
					if (equal(tok, "(")) {
						tok = skip_balanced(tok, '(', ')');
						if (tok && equal(tok, "{"))
							tok = skip_balanced(tok, '{', '}');
					}
					else {
						if (is_identifier_like(tok) && is_vla_typedef(tok)) return true;
						tok = tok->next;
					}
				}
				while (tok && !equal(tok, "]")) {
					if (equal(tok, ".") || equal(tok, "->")) {
						tok = tok->next;
						if (tok && !equal(tok, "]")) tok = tok->next;
					} else if (equal(tok, "[")) {
						tok = skip_balanced(tok, '[', ']');
					} else
						break;
				}
			}
			continue;
		}

		if (equal(tok, "offsetof") || equal(tok, "__builtin_offsetof")) return true;

		if (equal(tok, "->") || equal(tok, ".")) return true;

		if (is_valid_varname(tok) && !is_known_enum_const(tok) && !is_type_keyword(tok)) return true;

		tok = tok->next;
	}
	return false;
}

static bool scan_for_vla(Token *tok, const char *open, const char *close) {
	if (open && (!tok || !equal(tok, open))) return false;
	if (open) tok = tok->next;
	int depth = 1;
	while (tok && tok->kind != TK_EOF) {
		if (open && equal(tok, open)) depth++;
		else if (equal(tok, close)) {
			if (open) {
				if (--depth <= 0) break;
			} else
				break;
		} else if (!open && (equal(tok, "(") || equal(tok, "{")))
			depth++;
		else if (!open && (equal(tok, ")") || equal(tok, "}"))) {
			if (depth > 0) depth--;
		} else if (equal(tok, "[") && depth >= 1 && array_size_is_vla(tok))
			return true;
		tok = tok->next;
	}
	return false;
}

static void parse_typedef_declaration(Token *tok, int scope_depth) {
	Token *typedef_start = tok;
	tok = tok->next; // Skip 'typedef'
	Token *type_start = tok;
	TypeSpecResult type_spec = parse_type_specifier(tok);
	tok = type_spec.end; // Skip the base type

	bool is_vla = typedef_contains_vla(typedef_start);

	// Check if the base type has const — either explicit tokens or from a const typedef.
	bool base_is_const = type_spec.has_const;
	if (!base_is_const) {
		for (Token *t = type_start; t && t != tok; t = t->next)
			if (is_const_typedef(t)) { base_is_const = true; break; }
	}

	// Check if the base type is void (or a typedef alias for void).
	// Scan tokens from type_start to tok for 'void' keyword or known void typedef.
	// excludes struct/union/enum, typeof, _Atomic(...), function pointers.
	bool base_is_void = false;
	Token *t = type_start;
	while (t && t != tok) {
		if (t->tag & TT_TYPEOF) {
			t = t->next;
			if (t && equal(t, "(")) t = skip_balanced(t, '(', ')');
		} else if (t->tag & TT_SUE) {
			Token *brace = find_struct_body_brace(t);
			if (brace) t = skip_balanced(brace, '{', '}');
			else t = t->next;
		} else if ((t->tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE) && t->next && equal(t->next, "(")) {
			t = skip_balanced(t->next, '(', ')');
		} else if (t->tag & (TT_BITINT | TT_ATTR | TT_ALIGNAS)) {
			t = t->next;
			if (t && equal(t, "(")) t = skip_balanced(t, '(', ')');
		} else if (is_c23_attr(t)) {
			t = skip_c23_attr(t);
		} else if (equal(t, "void") || is_void_typedef(t)) {
			base_is_void = true;
			break;
		} else {
			t = t->next;
		}
	}

	// Check if base type is a pointer (for safe const-stripping in orelse).
	bool base_is_ptr = false;
	bool base_is_array = false;
	for (Token *bt = type_start; bt && bt != type_spec.end; bt = bt->next) {
		if (equal(bt, "*")) { base_is_ptr = true; break; }
		if (is_ptr_typedef(bt)) { base_is_ptr = true; break; }
		if (is_array_typedef(bt)) { base_is_array = true; break; }
	}

	// Parse declarator(s) until semicolon
	while (tok && !equal(tok, ";") && tok->kind != TK_EOF) {
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
			typedef_add(decl.var_name->loc, decl.var_name->len, scope_depth, is_vla, is_void);
			// Set is_const, is_ptr, is_array on the just-added entry
			if (typedef_table.count > 0) {
				if (is_const)
					typedef_table.entries[typedef_table.count - 1].is_const = true;
				if (is_ptr)
					typedef_table.entries[typedef_table.count - 1].is_ptr = true;
				if ((decl.is_array || base_is_array) && !decl.is_pointer && !decl.is_func_ptr)
					typedef_table.entries[typedef_table.count - 1].is_array = true;
			}
		}
		tok = decl.end ? decl.end : tok->next;

		while (tok && !equal(tok, ",") && !equal(tok, ";") && tok->kind != TK_EOF) {
			if (equal(tok, "(")) tok = skip_balanced(tok, '(', ')');
			else if (equal(tok, "["))
				tok = skip_balanced(tok, '[', ']');
			else
				tok = tok->next;
		}

		if (tok && equal(tok, ",")) tok = tok->next;
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
	return tok->kind == TK_IDENT || equal(tok, "raw") || equal(tok, "defer") || equal(tok, "orelse");
}

// ============================================================================
// Zero-init declaration parsing helpers
// ============================================================================

// Skip _Pragma(...) operator sequences (C99 6.10.9)
// _Pragma is equivalent to #pragma but can appear in macro expansions
// Returns the token after all _Pragma(...) sequences
static Token *skip_pragma_operators(Token *tok) {
	while (tok && equal(tok, "_Pragma") && tok->next && equal(tok->next, "(")) {
		tok = skip_balanced(tok->next, '(', ')'); // skip _Pragma and (...)
	}
	return tok;
}

// Parse type specifier: qualifiers, type keywords, struct/union/enum, typeof, _Atomic.
static TypeSpecResult parse_type_specifier(Token *tok) {
	TypeSpecResult r = { .end = tok };

	bool is_type = false;
	while ((tok->tag & TT_QUALIFIER) || (is_type = is_type_keyword(tok)) || is_c23_attr(tok)
	       || tok->kind == TK_PREP_DIR
	       || (equal(tok, "_Pragma") && tok->next && equal(tok->next, "("))) {
		bool had_type = r.saw_type;
		uint32_t tag = tok->tag;

		if (tag & TT_QUALIFIER) {
			if (tag & TT_VOLATILE) r.has_volatile = true;
			if (tag & TT_REGISTER) r.has_register = true;
			if (tag & TT_CONST) r.has_const = true;
			if (tag & TT_TYPE) {
				if (equal(tok, "auto"))
					r.saw_type = true;
				else
					r.has_atomic = true;
			}
		}

		if (is_c23_attr(tok)) {
			tok = skip_c23_attr(tok);
			r.end = tok;
			is_type = false;
			continue;
		}

		// _Pragma: skip transparently between qualifiers/type keywords.
		if (equal(tok, "_Pragma") && tok->next && equal(tok->next, "(")) {
			tok = skip_pragma_operators(tok);
			r.end = tok;
			is_type = false;
			continue;
		}
		if (tok->kind == TK_PREP_DIR) {
			tok = tok->next;
			r.end = tok;
			is_type = false;
			continue;
		}

		// Bare _Atomic (not _Atomic(type)) is only a qualifier, not a type specifier.
		if (is_type && (tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE)
		    && !(tok->next && equal(tok->next, "(")))
			is_type = false;
		if (is_type) r.saw_type = true;
		is_type = false;

		// _Atomic(type) specifier form (must precede generic attr/alignas handling)
		if ((tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE) && tok->next &&
		    equal(tok->next, "(")) {
			r.saw_type = true;
			r.has_atomic = true;
			tok = tok->next;
			Token *inner_start = tok->next;
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
			r.saw_type = true;
			tok = tok->next;
			while (tok && tok->kind != TK_EOF) {
				if (equal(tok, "_Pragma") && tok->next && equal(tok->next, "(")) {
					tok = skip_pragma_operators(tok);
					continue;
				}
				if (tok->kind == TK_PREP_DIR) { tok = tok->next; continue; }
				if (tok->tag & TT_QUALIFIER) tok = tok->next;
				else {
					Token *next = skip_all_attributes(tok);
					if (next == tok) break;
					tok = next;
				}
			}
			if (tok && is_valid_varname(tok)) tok = tok->next;
			if (tok && equal(tok, "{")) {
				if (struct_body_contains_vla(tok)) r.is_vla = true;
				tok = skip_balanced(tok, '{', '}');
			}
			r.end = tok;
			continue;
		}

		// typeof/typeof_unqual/__typeof__
		if (tag & TT_TYPEOF) {
			bool is_unqual = equal(tok, "typeof_unqual");
			r.saw_type = true;
			r.has_typeof = true;
			tok = tok->next;
			if (tok && equal(tok, "(")) {
				Token *end = skip_balanced(tok, '(', ')');
				if (!is_unqual)
					for (Token *t = tok->next; t && t != end; t = t->next) {
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
				for (Token *t = tok->next; t && t != end; prev = t, t = t->next) {
					if (equal(t, "(")) typeof_paren_depth++;
					else if (equal(t, ")")) typeof_paren_depth--;
					if (typeof_paren_depth == 1 && equal(t, "[") &&
					    (is_type_keyword(prev) || is_known_typedef(prev) ||
					     equal(prev, "]") || equal(prev, "*")) &&
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
			tok = tok->next;
			if (tok && equal(tok, "(")) tok = skip_balanced(tok, '(', ')');
			r.end = tok;
			continue;
		}

		int tflags = typedef_flags(tok);
		if ((tflags & TDF_TYPEDEF) || is_typedef_like(tok)) {
			// After a concrete type, a typedef name is a declarator, not part of the type.
			if (had_type) break;
			r.is_typedef = true;
			if (tflags & TDF_VLA) r.is_vla = true;
			Token *peek = tok->next;
			while (peek && (peek->tag & TT_QUALIFIER)) peek = peek->next;
			if (peek && is_valid_varname(peek)) {
				Token *after = peek->next;
				if (after && (equal(after, ";") || equal(after, "[") || equal(after, ",") ||
					      equal(after, "="))) {
					tok = tok->next;
					r.end = tok;
					r.saw_type = true;
					return r;
				}
			}
		}

		tok = tok->next;
		r.end = tok;
	}

	return r;
}

// Get canonical delimiter char for balanced walking
static inline char tok_open_ch(Token *t) {
	return t->loc[0];
}

static inline char close_for(char c) {
	return c == '(' ? ')' : c == '[' ? ']' : '}';
}

// Walk a balanced token group between matching delimiters, optionally emitting.
static Token *walk_balanced(Token *tok, bool emit) {
	char open_c = tok_open_ch(tok);
	char close_c = close_for(open_c);
	if (emit) emit_tok(tok);
	tok = tok->next;
	int depth = 1;
	while (tok && tok->kind != TK_EOF && depth > 0) {
		if (match_ch(tok, open_c)) depth++;
		else if (match_ch(tok, close_c))
			depth--;
		if (emit) emit_tok(tok);
		tok = tok->next;
	}
	return tok;
}

static inline void decl_emit(Token *t, bool emit) {
	if (emit) emit_tok(t);
}

static inline Token *decl_balanced(Token *t, bool emit) {
	return emit ? walk_balanced(t, true) : skip_balanced(t, tok_open_ch(t), close_for(tok_open_ch(t)));
}

static inline Token *decl_attr(Token *t, bool emit) {
	if (emit) emit_tok(t);
	t = t->next;
	if (t && equal(t, "(")) t = emit ? walk_balanced(t, true) : skip_balanced(t, '(', ')');
	return t;
}

static inline Token *decl_array_dims(Token *t, bool emit, bool *vla) {
	while (equal(t, "[")) {
		if (array_size_is_vla(t)) *vla = true;
		t = decl_balanced(t, emit);
	}
	return t;
}

// Unified declarator parser. emit=true emits tokens, emit=false only advances.
static DeclResult parse_declarator(Token *tok, bool emit) {
	DeclResult r = { .end = tok };

	int ptr_depth = 0;
	while (equal(tok, "*") || (tok->tag & TT_QUALIFIER) || equal(tok, "_Pragma") ||
	       tok->kind == TK_PREP_DIR) {
		if (tok->kind == TK_PREP_DIR) { decl_emit(tok, emit); tok = tok->next; continue; }
		if (equal(tok, "_Pragma")) {
			if (emit) {
				emit_tok(tok);
				tok = tok->next;
				if (tok && equal(tok, "(")) tok = walk_balanced(tok, true);
			} else {
				tok = skip_pragma_operators(tok);
			}
			continue;
		}
		if (equal(tok, "*")) {
			r.is_pointer = true;
			r.is_const = false;
			if (++ptr_depth > 1024) {
				warn_tok(tok,
					 "pointer depth exceeds 1024; zero-initialization skipped for this "
					 "declaration");
				r.end = NULL;
				return r;
			}
		} else if (r.is_pointer && (tok->tag & TT_CONST))
			r.is_const = true;
		if (tok->tag & TT_ATTR) {
			tok = decl_attr(tok, emit);
			continue;
		}
		decl_emit(tok, emit);
		tok = tok->next;
	}

	int nested_paren = 0;
	if (equal(tok, "(")) {
		Token *peek = tok->next;
		while (peek && (peek->tag & TT_ATTR)) {
			peek = peek->next;
			if (peek && equal(peek, "(")) peek = skip_balanced(peek, '(', ')');
		}
		if (!equal(peek, "*") && !equal(peek, "(") && !is_valid_varname(peek)) {
			r.end = NULL;
			return r;
		}

		decl_emit(tok, emit);
		tok = tok->next;
		nested_paren = 1;
		r.has_paren = true;

		while (equal(tok, "*") || (tok->tag & TT_QUALIFIER) || equal(tok, "(")) {
			if (equal(tok, "*")) {
				r.is_pointer = true;
				r.paren_pointer = true;
				r.is_const = false;
			} else if (r.is_pointer && (tok->tag & TT_CONST))
				r.is_const = true;
			if (equal(tok, "(")) {
				if (++nested_paren > 1024) {
					warn_tok(tok,
						 "declarator parenthesization depth exceeds 1024; "
						 "zero-initialization skipped");
					r.end = NULL;
					return r;
				}
			}
			if (tok->tag & TT_ATTR) {
				tok = decl_attr(tok, emit);
				continue;
			}
			decl_emit(tok, emit);
			tok = tok->next;
		}
	}

	if (!is_valid_varname(tok)) // Must have identifier
	{
		r.end = NULL;
		return r;
	}

	r.var_name = tok;
	decl_emit(tok, emit);
	tok = tok->next;

	while (tok && tok->kind != TK_EOF) {
		if (tok->tag & TT_ATTR) tok = decl_attr(tok, emit);
		else if (is_c23_attr(tok)) tok = decl_balanced(tok, emit);
		else break;
	}

	if (r.has_paren && equal(tok, "[")) {
		r.is_array = true;
		r.paren_array = true;
		tok = decl_array_dims(tok, emit, &r.is_vla);
	}

	while (r.has_paren && nested_paren > 0) {
		while (equal(tok, "(") || equal(tok, "[")) {
			if (equal(tok, "(")) tok = decl_balanced(tok, emit);
			else {
				r.is_array = true;
				r.paren_array = true;
				tok = decl_array_dims(tok, emit, &r.is_vla);
			}
		}
		if (!equal(tok, ")")) {
			r.end = NULL;
			return r;
		}
		decl_emit(tok, emit);
		tok = tok->next;
		nested_paren--;
	}

	if (equal(tok, "(")) {
		if (!r.has_paren) {
			r.end = NULL;
			return r;
		}
		r.is_func_ptr = true;
		tok = decl_balanced(tok, emit);
	}

	if (equal(tok, "[")) {
		r.is_array = true;
		tok = decl_array_dims(tok, emit, &r.is_vla);
	}

	while (tok && tok->kind != TK_EOF) {
		if (tok->tag & TT_ATTR) tok = decl_attr(tok, emit);
		else if (is_c23_attr(tok)) tok = decl_balanced(tok, emit);
		else if (tok->tag & TT_ASM) {
			if (emit) emit_tok(tok);
			tok = tok->next;
			if (tok && equal(tok, "(")) tok = decl_balanced(tok, emit);
		} else break;
	}

	r.has_init = equal(tok, "=");
	r.end = tok;

	return r;
}

// Emit first declarator raw, then zero-init subsequent uninitialised declarators.
static Token *emit_raw_first_then_zeroinit_rest(Token *tok, bool file_storage) {
	int depth = 0;
	while (tok && tok->kind != TK_EOF) {
		if (tok->flags & TF_OPEN) depth++;
		else if (tok->flags & TF_CLOSE) depth--;
		else if (depth == 0 && (equal(tok, ",") || equal(tok, ";"))) break;
		emit_tok(tok);
		tok = tok->next;
	}
	while (tok && equal(tok, ",")) {
		emit_tok(tok);
		tok = tok->next;
		depth = 0;
		bool has_init = false;
		while (tok && tok->kind != TK_EOF) {
			if (tok->flags & TF_OPEN) depth++;
			else if (tok->flags & TF_CLOSE) depth--;
			else if (depth == 0 && (equal(tok, ",") || equal(tok, ";"))) break;
			if (depth == 0 && equal(tok, "=")) has_init = true;
			emit_tok(tok);
			tok = tok->next;
		}
		if (!has_init && !file_storage) OUT_LIT(" = 0");
	}
	if (tok && equal(tok, ";")) { emit_tok(tok); tok = tok->next; }
	return tok;
}

// Emit memset/byte-loop zeroing for typeof/atomic/VLA variables.
static void emit_typeof_memsets(Token **vars, int count, bool has_volatile, bool has_const) {
	const char *vol = has_volatile ? "volatile " : "";
	int vol_len = has_volatile ? 9 : 0;
	for (int i = 0; i < count; i++) {
		// memset for non-volatile; byte loop for volatile (memset drops volatile).
		if (has_volatile) {
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
			out_str(vars[i]->loc, vars[i]->len);
			OUT_LIT("; for (unsigned long _Prism_i_");
			out_uint(ctx->ret_counter);
			OUT_LIT(" = 0; _Prism_i_");
			out_uint(ctx->ret_counter);
			OUT_LIT(" < sizeof(");
			out_str(vars[i]->loc, vars[i]->len);
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
				OUT_LIT(" memset((void *)&");
			else
				OUT_LIT(" memset(&");
			out_str(vars[i]->loc, vars[i]->len);
			OUT_LIT(", 0, sizeof(");
			out_str(vars[i]->loc, vars[i]->len);
			OUT_LIT("));");
		}
	}
}

// Register typedef shadows and VLA variables for function parameters.
static void register_param_shadows(Token *open_paren, Token *close_paren) {
	if (!open_paren || !close_paren) return;
	Token *last_ident = NULL;
	Token *param_start = open_paren->next; // start of current parameter tokens
	int depth = 0;
	int ident_depth = -1;              // depth where last_ident was captured
	bool seen_close_after_ident = false; // ')' closed back to ident_depth or shallower
	for (Token *t = open_paren->next; t && t != close_paren && t->kind != TK_EOF; t = t->next) {
		if (t->flags & TF_OPEN) depth++;
		else if (t->flags & TF_CLOSE) {
			depth--;
			if (last_ident && depth <= ident_depth) seen_close_after_ident = true;
		}
		else if (!seen_close_after_ident && is_valid_varname(t) &&
			 !(t->tag & (TT_QUALIFIER | TT_TYPE | TT_SUE | TT_TYPEOF | TT_ATTR))) {
			last_ident = t;
			ident_depth = depth;
		}
		if (depth == 0 && (equal(t, ",") || t->next == close_paren)) {
			if (last_ident) {
				if (is_known_typedef(last_ident) || is_typedef_heuristic(last_ident))
					typedef_add_shadow(last_ident->loc, last_ident->len, 1);
				// Check for VLA dimensions in parameter type.
				Token *end = t->next == close_paren ? close_paren : t;
				for (Token *s = param_start; s && s != end && s->kind != TK_EOF; s = s->next) {
					if (equal(s, "[") && array_size_is_vla(s)) {
						typedef_add_vla_var(last_ident->loc, last_ident->len, 1);
						break;
					}
				}
			}
			param_start = t->next; // next parameter starts after ','
			last_ident = NULL;
			ident_depth = -1;
			seen_close_after_ident = false;
		}
	}
	// Handle last parameter (if close_paren was hit without trailing comma)
	if (last_ident) {
		if (is_known_typedef(last_ident) || is_typedef_heuristic(last_ident))
			typedef_add_shadow(last_ident->loc, last_ident->len, 1);
		for (Token *s = param_start; s && s != close_paren && s->kind != TK_EOF; s = s->next) {
			if (equal(s, "[") && array_size_is_vla(s)) {
				typedef_add_vla_var(last_ident->loc, last_ident->len, 1);
				break;
			}
		}
	}
}

// Register typedef shadows and VLA variables for a declarator
static inline void register_decl_shadows(Token *var_name, bool effective_vla) {
	int depth = ctx->defer_depth + (ctrl.for_init ? 1 : 0);
	if (is_known_typedef(var_name) || is_typedef_heuristic(var_name))
		typedef_add_shadow(var_name->loc, var_name->len, depth);
	if (effective_vla && var_name) typedef_add_vla_var(var_name->loc, var_name->len, depth);
}

// Re-emit type specifier tokens, skipping struct/union/enum bodies
// to avoid redefinition errors (e.g. enum constant redeclaration).
// Exception: anonymous struct/union bodies (no tag name between keyword
// and '{') must be preserved — stripping them produces an incomplete type
// like bare 'struct' which is always wrong.
static void re_emit_type_specifier(Token *type_start, Token *type_end) {
	for (Token *t = type_start; t != type_end; t = t->next) {
		if (equal(t, "{")) {
			// Find the nearest struct/union/enum keyword before this brace.
			Token *kw = NULL;
			for (Token *s = type_start; s != t; s = s->next)
				if (equal(s, "struct") || equal(s, "union") || equal(s, "enum"))
					kw = s;
			// For anonymous struct/union (no tag name between keyword and brace),
			// keep the body so the type remains complete.
			bool keep = false;
			if (kw && !equal(kw, "enum")) {
				keep = true;
				for (Token *u = kw->next; u && u != t; u = u->next) {
					if (is_valid_varname(u) && !(u->tag & (TT_QUALIFIER | TT_ATTR | TT_TYPEOF))) {
						keep = false; // tag name found — safe to strip body
						break;
					}
				}
			}
			if (!keep) {
				t = skip_balanced(t, '{', '}');
				if (t == type_end) break;
			}
		}
		emit_tok(t);
	}
}

// Emit break/continue with defer handling. Returns next token.
static Token *emit_break_continue_defer(Token *tok) {
	bool is_break = tok->tag & TT_BREAK;
	mark_switch_control_exit();
	if (FEAT(F_DEFER) && control_flow_has_defers(is_break))
		emit_defers(is_break ? DEFER_BREAK : DEFER_CONTINUE);
	out_char(' '); out_str(tok->loc, tok->len); out_char(';');
	tok = tok->next;
	if (equal(tok, ";")) tok = tok->next;
	return tok;
}

// Emit goto with defer handling. Returns next token.
static Token *emit_goto_defer(Token *tok) {
	mark_switch_control_exit();
	tok = tok->next;
	if (FEAT(F_DEFER) && is_identifier_like(tok)) {
		int td = label_table_lookup(tok->loc, tok->len);
		if (td < 0) td = ctx->defer_depth;
		if (goto_has_defers(td)) emit_goto_defers(td);
	}
	OUT_LIT(" goto ");
	if (is_identifier_like(tok)) { out_str(tok->loc, tok->len); tok = tok->next; }
	out_char(';');
	if (equal(tok, ";")) tok = tok->next;
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
					   Token *stop_comma) {
	// Roll back speculative output.
	out_buf_pos = oe_buf_checkpoint;
	last_emitted = NULL;

	unsigned oe_id = ctx->ret_counter++;

	// Emit mutable temp: strip const from type (except pointer types where it qualifies pointee).
	// Function pointers: strip const (it qualifies return type, not the pointer variable).
	// Declarator-level const (e.g. *const) also stripped for mutability.
	bool strip_type_const = !decl->is_pointer || decl->is_func_ptr;

	// Detect const typedefs (const baked into typedef, no TT_CONST tag).
	// Use __typeof__((type)0 + 0) to strip const via arithmetic promotion.
	bool has_const_typedef = false;
	if (strip_type_const) {
		for (Token *t = type_start; t != type->end; t = t->next)
			if (is_const_typedef(t)) { has_const_typedef = true; break; }
	}

	if (has_const_typedef) {
		// Pointer-to-incomplete: use &* trick instead of arithmetic.
		bool const_td_is_ptr = false;
		bool const_td_is_array = false;
		for (Token *t = type_start; t != type->end; t = t->next) {
			if (is_ptr_typedef(t)) { const_td_is_ptr = true; break; }
			if (is_array_typedef(t)) { const_td_is_array = true; break; }
		}

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
		emit_type_stripped(type_start, type->end, strip_type_const);
	}
	// Emit declarator prefix, stripping const for mutability.
	for (Token *t = decl_start; t != decl->var_name; t = t->next) {
		if (t->tag & TT_CONST) continue;
		emit_tok(t);
	}
	
	OUT_LIT(" _Prism_oe_");
	out_uint(oe_id);
	emit_range(decl->var_name->next, decl->end);
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
				mark_switch_control_exit();
				tok = tok->next;
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

		int fdepth = 0;
		bool chained = false;
		while (tok->kind != TK_EOF) {
			if (tok->flags & TF_OPEN) fdepth++;
			else if (tok->flags & TF_CLOSE)
				fdepth--;
			else if (fdepth == 0 && (equal(tok, ";") || (stop_comma && tok == stop_comma)))
				break;
			if (fdepth == 0 && is_orelse_keyword(tok)) {
				chained = true;
				tok = tok->next;
				break;
			}
			if (equal(tok, "(") && tok->next && equal(tok->next, "{"))
				error_tok(tok, "GNU statement expressions in orelse fallback values are not "
					  "supported; use 'orelse { ... }' block form instead");
			emit_tok(tok);
			tok = tok->next;
		}
		OUT_LIT(");");
		if (!chained) break;
	}

	// Emit final const declaration: "const T declarator = _Prism_oe_N;"
	emit_range(type_start, type->end);
	parse_declarator(decl_start, true);

	OUT_LIT(" = _Prism_oe_");
	out_uint(oe_id);
	out_char(';');
	return tok;
}

// Process all declarators in a declaration and emit with zero-init.
static Token *process_declarators(Token *tok, TypeSpecResult *type, bool is_raw, Token *type_start) {
	Token **typeof_vars = NULL;
	int typeof_var_count = 0;
	int typeof_var_cap = 0;
	ctx->active_typeof_vars = NULL; // Track for longjmp safety
	int original_oe_checkpoint = oe_buf_checkpoint;

	while (tok && tok->kind != TK_EOF) {
		Token *decl_start = tok;
		int buf_pos_before_decl = out_buf_pos;
		DeclResult decl = parse_declarator(tok, true);
		if (!decl.end || !decl.var_name) {
			// Rollback emitted tokens; buffer grows without flushing during speculation.
			if (buf_pos_before_decl <= out_buf_pos) out_buf_pos = buf_pos_before_decl;
			// If orelse committed output, emit rest as raw instead of rolling back.
			if (oe_buf_checkpoint != original_oe_checkpoint)
				goto emit_raw_remainder;
			ctx->active_typeof_vars = NULL;
			return NULL;
		}

		tok = decl.end;

		// Effective VLA: paren_pointer suppresses VLA unless paren_array is also set.
		bool effective_vla = (decl.is_vla && (!decl.paren_pointer || decl.paren_array)) || (type->is_vla && !decl.is_pointer);
		bool is_vla_type = decl.is_vla || type->is_vla;

		// typeof/atomic aggregate/VLA: use memset (can't use = 0 or = {0})
		bool is_aggregate =
		    decl.is_array || ((type->is_struct || type->is_typedef) && !decl.is_pointer);
		bool needs_memset = !decl.has_init && !is_raw && (!decl.is_pointer || decl.is_array) &&
				    !type->has_register &&
				    (type->has_typeof || (type->has_atomic && is_aggregate) || effective_vla);

		// Add zero initializer if needed (for non-memset types)
		if (!decl.has_init && !effective_vla && !is_raw && !needs_memset) {
			// register + _Atomic aggregate: no safe zero-init path.
			if (type->has_register && type->has_atomic && is_aggregate)
				;
			else if (is_aggregate || type->has_typeof)
				OUT_LIT(" = {0}");
			else
				OUT_LIT(" = 0");
		}

		// Track typeof variables for memset emission (dynamic, no hard limit)
		if (needs_memset) {
			ARENA_ENSURE_CAP(
			    &ctx->main_arena, typeof_vars, typeof_var_count + 1, typeof_var_cap, 8, Token *);
			typeof_vars[typeof_var_count++] = decl.var_name;
			ctx->active_typeof_vars = typeof_vars; // Update tracking after potential realloc
		}

		// Emit initializer if present
		if (decl.has_init) {
			int depth = 0;
			bool hit_orelse = false;
			while (tok->kind != TK_EOF) {
				if (tok->flags & TF_OPEN) depth++;
				else if (tok->flags & TF_CLOSE)
					depth--;
				else if (depth == 0 && (equal(tok, ",") || equal(tok, ";")))
					break;
			// Detect 'orelse' at depth 0 in initializer
				if (FEAT(F_ORELSE) && depth == 0 && (tok->tag & TT_ORELSE) &&
				    !is_known_typedef(tok) &&
				    !(last_emitted && (last_emitted->tag & TT_MEMBER))) {
					hit_orelse = true;
					break;
				}
				// orelse inside parens/braces: error (only works at statement level).
				if (FEAT(F_ORELSE) && depth > 0 && (tok->tag & TT_ORELSE) &&
				    !is_known_typedef(tok) &&
				    !(last_emitted && (last_emitted->tag & TT_MEMBER)))
					error_tok(tok,
						  "'orelse' cannot be used inside parentheses "
						  "(it must appear at the top level of a declaration)");
				emit_tok(tok);
				tok = tok->next;
			}

			if (hit_orelse) {
				if (ctrl.for_init)
					error_tok(tok, "orelse cannot be used in for-loop initializers");

				// Detect struct/union value (not enum)
				bool type_is_sue_not_enum = type->is_struct;
				if (type_is_sue_not_enum) {
					for (Token *t = type_start; t && t != type->end; t = t->next) {
						if (equal(t, "enum")) {
							type_is_sue_not_enum = false;
							break;
						}
						if (equal(t, "struct") || equal(t, "union")) break;
					}
				}

				// For function pointers, type-level const qualifies return type, not the variable.
				bool has_const_qual = (type->has_const && !decl.is_func_ptr) || decl.is_const;
				if (!has_const_qual && !decl.is_func_ptr && !decl.is_pointer) {
					for (Token *t = type_start; t && t != type->end; t = t->next)
						if (is_const_typedef(t)) { has_const_qual = true; break; }
				}
				bool is_struct_val =
				    type_is_sue_not_enum && !decl.is_pointer && !decl.is_array;

				// Array address is never NULL — orelse can never trigger
				if (decl.is_array && !decl.is_pointer)
					error_tok(decl.var_name,
						  "orelse on array variable '%.*s' will never trigger "
						  "(array address is never NULL); remove the orelse clause",
						  decl.var_name->len,
						  decl.var_name->loc);

				Token *peek_action = tok->next;
				bool is_orelse_fallback =
				    peek_action &&
				    !(peek_action->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) &&
				    !equal(peek_action, "{") && !equal(peek_action, ";");

				// const + fallback orelse: temp variable approach (avoids GNU ternary)
				if (has_const_qual && is_orelse_fallback) {
					if (is_struct_val)
						error_tok(tok,
							  "orelse fallback on const struct is not supported; "
							  "use a control flow action (return/break/goto) or "
							  "remove const");

					Token *orelse_tok = tok;
					Token *val_start = decl.end->next; // First value token after '='

					register_decl_shadows(decl.var_name, is_vla_type);
					tok = orelse_tok->next; // skip 'orelse'

					Token *stop_comma = find_boundary_comma(tok);

					tok = handle_const_orelse_fallback(tok,
									   orelse_tok,
									   val_start,
									   decl_start,
									   &decl,
									   type_start,
									   type,
									   stop_comma);

					emit_typeof_memsets(
					    typeof_vars, typeof_var_count, type->has_volatile, type->has_const);
					typeof_var_count = 0;

					if (equal(tok, ";")) tok = tok->next;
					end_statement_after_semicolon();

					if (stop_comma && equal(tok, ",")) {
						tok = tok->next;
						oe_buf_checkpoint = out_buf_pos;
						re_emit_type_specifier(type_start, type->end);
						is_raw = false;
						continue;
					}
					ctx->active_typeof_vars = NULL;
					return tok;
				}

				out_char(';');

				emit_typeof_memsets(typeof_vars, typeof_var_count, type->has_volatile, type->has_const);
				typeof_var_count = 0; // reset for remaining declarators

				register_decl_shadows(decl.var_name, is_vla_type);

				tok = tok->next; // skip 'orelse'

				// Error on missing action
				if (equal(tok, ";")) error_tok(tok, "expected statement after 'orelse'");

				Token *stop_comma = find_boundary_comma(tok);

				// Struct/union value orelse — error
				bool is_struct_value =
				    type_is_sue_not_enum && !decl.is_pointer && !decl.is_array;
				if (is_struct_value)
					error_tok(decl.var_name,
						  "orelse on struct/union values is not supported (memcmp "
						  "cannot reliably detect zero due to padding)");
				tok = emit_orelse_action(
				    tok, decl.var_name, has_const_qual, stop_comma);

				if (stop_comma && equal(tok, ",")) {
					tok = tok->next;
					oe_buf_checkpoint = out_buf_pos;
					re_emit_type_specifier(type_start, type->end);
					is_raw = false;
					continue;
				}
				ctx->active_typeof_vars = NULL;
				return tok;
			}
		}

		register_decl_shadows(decl.var_name, is_vla_type);

		if (equal(tok, ";")) {
			emit_tok(tok);
			emit_typeof_memsets(typeof_vars, typeof_var_count, type->has_volatile, type->has_const);
			ctx->active_typeof_vars = NULL;
			return tok->next;
		} else if (equal(tok, ",")) {
			emit_tok(tok);
			tok = tok->next;
			is_raw = false; // raw applies to first declarator only
		} else {
			if (oe_buf_checkpoint != original_oe_checkpoint)
				goto emit_raw_remainder;
			ctx->active_typeof_vars = NULL;
			return NULL;
		}
	}

emit_raw_remainder:
	if (oe_buf_checkpoint != original_oe_checkpoint) {
		while (tok && tok->kind != TK_EOF && !equal(tok, ";")) {
			emit_tok(tok);
			tok = tok->next;
		}
		if (tok && equal(tok, ";")) {
			emit_tok(tok);
			tok = tok->next;
		}
		emit_typeof_memsets(typeof_vars, typeof_var_count, type->has_volatile, type->has_const);
		ctx->active_typeof_vars = NULL;
		return tok;
	}
	ctx->active_typeof_vars = NULL;
	return NULL;
}

// Register shadows for file-scope _t/__* variable declarations.
static void register_toplevel_shadows(Token *tok) {
	if (tok->kind >= TK_STR) return;

	Token *t = tok;
	while (t && t->kind != TK_EOF) {
		if (t->tag & TT_TYPEDEF) return;
		if (t->tag & (TT_SKIP_DECL | TT_INLINE)) t = t->next;
		else if (t->tag & TT_ATTR) t = skip_gnu_attributes(t);
		else if (is_c23_attr(t)) t = skip_c23_attr(t);
		else break;
	}
	if (!t || t->kind == TK_EOF) return;

	// Must start with a type keyword or known typedef
	if (!(t->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_BITINT)) &&
	    !is_known_typedef(t))
		return;

	// Advance past type specifier
	while (t && t->kind != TK_EOF) {
		if (t->tag & (TT_TYPE | TT_QUALIFIER)) t = t->next;
		else if (t->tag & TT_SUE) {
			t = t->next;
			if (t && (t->tag & (TT_ATTR | TT_QUALIFIER))) {
				t = t->next;
				if (t && equal(t, "(")) t = skip_balanced(t, '(', ')');
			}
			if (t && is_valid_varname(t)) t = t->next;
			if (t && equal(t, "{")) t = skip_balanced(t, '{', '}');
		}
		else if (t->tag & (TT_TYPEOF | TT_ATTR | TT_BITINT | TT_ALIGNAS)) {
			t = t->next;
			if (t && equal(t, "(")) t = skip_balanced(t, '(', ')');
		}
		else if (is_c23_attr(t)) t = skip_c23_attr(t);
		else if (is_known_typedef(t)) t = t->next;
		else break;
	}
	if (!t || t->kind == TK_EOF) return;

	// Scan declarator(s), register shadows for _t/__* names
	while (t && !equal(t, ";") && t->kind != TK_EOF) {
		DeclResult decl = parse_declarator(t, false);
		if (!decl.var_name || !decl.end) break;
		if (equal(decl.end, "(")) return; // function declaration, not variable

		if (is_typedef_heuristic(decl.var_name))
			typedef_add_shadow(decl.var_name->loc, decl.var_name->len, 0);

		t = decl.end;
		if (equal(t, "=")) { // skip initializer
			t = t->next;
			for (int depth = 0; t && t->kind != TK_EOF; t = t->next) {
				if (t->flags & TF_OPEN) depth++;
				else if (t->flags & TF_CLOSE) depth--;
				else if (depth == 0 && (equal(t, ",") || equal(t, ";"))) break;
			}
		}
		if (t && equal(t, ",")) t = t->next; else break;
	}
}

// Try to handle a declaration with zero-init. Returns token after declaration, or NULL.
static Token *try_zero_init_decl(Token *tok) {
	if (!FEAT(F_ZEROINIT) || ctx->defer_depth <= 0 || ctx->struct_depth > 0) return NULL;

	if (tok->kind >=
	    TK_STR) // Fast reject: strings, numbers, prep directives, EOF can't start a declaration
		return NULL;

	// Declarations in switch body without braces can be skipped by case jumps.
	bool in_switch_scope_unbraced = ctx->defer_depth > 0 && defer_stack[ctx->defer_depth - 1].is_switch;

	Token *warn_loc = tok;
	Token *pragma_start = tok;

	while (is_c23_attr(tok)) tok = skip_c23_attr(tok);
	tok = skip_pragma_operators(tok);
	Token *start = tok;

	bool is_raw = false;
	Token *raw_tok = NULL;
	if (equal(tok, "raw") && !is_known_typedef(tok)) {
		Token *after_raw = tok->next;
		while (is_c23_attr(after_raw)) after_raw = skip_c23_attr(after_raw);
		after_raw = skip_pragma_operators(after_raw);
		while (after_raw && after_raw->kind == TK_PREP_DIR) after_raw = after_raw->next;
		if (is_raw_declaration_context(after_raw)) {
			is_raw = true;
			raw_tok = tok;
			start = tok->next;
			tok = after_raw;
			if (pragma_start == raw_tok) pragma_start = start;
			warn_loc = after_raw;
		}
	}

	// Probe past qualifiers/attrs/storage to find 'raw' after prefix.
	if (!is_raw) {
		Token *probe = start;
		while (probe && probe->kind != TK_EOF) {
			Token *next = skip_all_attributes(probe);
			if (next != probe) { probe = next; continue; }
			if (probe->tag & TT_QUALIFIER) { probe = probe->next; continue; }
			if (equal(probe, "_Pragma")) { probe = skip_pragma_operators(probe); continue; }
			if (probe->kind == TK_PREP_DIR) { probe = probe->next; continue; }
			if (equal(probe, "_Thread_local") || equal(probe, "static") ||
			    equal(probe, "extern") || (probe->tag & TT_TYPEDEF)) { probe = probe->next; continue; }
			break;
		}
		if (probe && equal(probe, "raw") && !is_known_typedef(probe)) {
			Token *after_raw = probe->next;
			while (is_c23_attr(after_raw)) after_raw = skip_c23_attr(after_raw);
			after_raw = skip_pragma_operators(after_raw);
			while (after_raw && after_raw->kind == TK_PREP_DIR) after_raw = after_raw->next;
			if (is_raw_declaration_context(after_raw)) {
				bool has_file_storage = false;
				for (Token *s = pragma_start; s != probe; s = s->next)
					if (equal(s, "extern") || equal(s, "static")) { has_file_storage = true; break; }
				emit_range(pragma_start, probe);
				return emit_raw_first_then_zeroinit_rest(after_raw, has_file_storage);
			}
		}
	}

	if (tok->tag & TT_SKIP_DECL) // Storage class specifiers, control flow, etc.
	{
		if (is_raw) {
			bool has_file_storage = false;
			for (Token *s = pragma_start; s && s != start; s = s->next)
				if (equal(s, "extern") || equal(s, "static")) { has_file_storage = true; break; }
			if (!has_file_storage && tok && (equal(tok, "extern") || equal(tok, "static")))
				has_file_storage = true;
			return emit_raw_first_then_zeroinit_rest(start, has_file_storage);
		}
		return NULL;
	}

	if (!(tok->tag & TT_DECL_START) && !is_typedef_like(tok)) return NULL;

	TypeSpecResult type = parse_type_specifier(tok);
	if (!type.saw_type) return NULL;

	if (!is_var_declaration(type.end)) {
		// Detect GNU nested function definitions (break defer/return tracking).
		if (FEAT(F_DEFER)) {
			Token *p = type.end;
			while (p && (equal(p, "*") || (p->tag & TT_QUALIFIER) || (p->tag & TT_ATTR))) {
				if (p->tag & TT_ATTR) { p = p->next; if (p && equal(p, "(")) p = skip_balanced(p, '(', ')'); continue; }
				p = p->next;
			}
			if (p && is_valid_varname(p)) {
				Token *after_name = p->next;
				while (after_name && (after_name->tag & TT_ATTR))
					after_name = skip_gnu_attributes(after_name);
				if (after_name && equal(after_name, "(")) {
					Token *after_params = skip_balanced(after_name, '(', ')');
					while (after_params && ((after_params->tag & TT_ATTR) || (after_params->tag & TT_ASM))) {
						if (after_params->tag & TT_ASM) { after_params = after_params->next; }
						else after_params = skip_gnu_attributes(after_params);
						if (after_params && equal(after_params, "(")) after_params = skip_balanced(after_params, '(', ')');
					}
					if (after_params && equal(after_params, "{"))
						error_tok(p,
							  "nested function definitions are not supported inside "
							  "functions using defer/zeroinit — move the function outside "
							  "or use a function pointer");
					// K&R-style: type name(a, b) int a; int b; { ... }
					if (after_params && !equal(after_params, "{")) {
						Token *maybe_brace = after_params;
						while (maybe_brace && maybe_brace->kind != TK_EOF &&
						       !equal(maybe_brace, "{") && !equal(maybe_brace, "}") &&
						       !equal(maybe_brace, ";"))
							maybe_brace = maybe_brace->next;
						// Allow scanning through K&R decls (ending with ';')
						if (maybe_brace && equal(maybe_brace, ";")) {
							while (maybe_brace && maybe_brace->kind != TK_EOF &&
							       !equal(maybe_brace, "{") && !equal(maybe_brace, "}"))
								maybe_brace = maybe_brace->next;
						}
						if (maybe_brace && equal(maybe_brace, "{") &&
						    is_knr_params(after_params, maybe_brace))
							error_tok(p,
								  "nested function definitions are not supported inside "
								  "functions using defer/zeroinit — move the function outside "
								  "or use a function pointer");
					}
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

	if (type.is_struct) {
		for (Token *t = start; t && t != type.end; t = t->next) {
			if (equal(t, "enum")) {
				Token *brace = find_struct_body_brace(t);
				if (brace && brace != type.end) parse_enum_constants(brace, ctx->defer_depth);
				break;
			}
		}
	}

	// Flush before checkpoint so speculative output + rollback is safe.
	out_flush();
	oe_buf_checkpoint = out_buf_pos;

	// Braceless control body: wrap in braces (orelse expands to multiple stmts).
	bool brace_wrap = ctrl.pending && ctrl.parens_just_closed;
	if (brace_wrap) OUT_LIT(" {");

	if (raw_tok && pragma_start != start) {
		emit_range(pragma_start, raw_tok);
	} else if (pragma_start != start) {
		emit_range(pragma_start, start);
	}
	emit_range(start, type.end);

	Token *result = process_declarators(type.end, &type, is_raw, start);
	if (result && brace_wrap) OUT_LIT(" }");
	if (!result && oe_buf_checkpoint >= 0) {
		// Rollback speculative output.
		out_buf_pos = oe_buf_checkpoint;
		last_emitted = NULL;
	}
	oe_buf_checkpoint = -1;
	if (result && !is_raw && ctx->defer_depth > 0)
		defer_stack[ctx->defer_depth - 1].has_zeroinit_decl = true;
	return result;
}

// Emit expression to semicolon, handling zero-init in statement expressions.
static Token *emit_expr_to_semicolon(Token *tok) {
	int depth = 0;
	int ternary_depth = 0;
	bool expr_at_stmt_start = false;
	while (tok->kind != TK_EOF) {
		if (tok->flags & TF_OPEN) {
			depth++;
			if (match_ch(tok, '{')) expr_at_stmt_start = true;
		} else if (tok->flags & TF_CLOSE)
			depth--;
		else if (depth == 0 && equal(tok, ";"))
			break;
		else if (equal(tok, "?"))
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

		emit_tok(tok);
		tok = tok->next;

		if (last_emitted &&
		    (equal(last_emitted, "{") || equal(last_emitted, ";") || equal(last_emitted, "}")))
			expr_at_stmt_start = true;
		else if (last_emitted && equal(last_emitted, ":") && ternary_depth <= 0)
			expr_at_stmt_start = true;
		else {
			if (last_emitted && equal(last_emitted, ":") && ternary_depth > 0) ternary_depth--;
			expr_at_stmt_start = false;
		}
	}
	return tok;
}

// Validate control flow keywords inside defer blocks are safe
static void validate_defer_control_flow(Token *t, int inner_loop_depth, int inner_switch_depth) {
	if (t->tag & TT_RETURN) error_tok(t, "'return' inside defer block bypasses remaining defers");
	if ((t->tag & TT_GOTO) && !is_known_typedef(t))
		error_tok(t, "'goto' inside defer block could bypass remaining defers");
	if ((t->tag & TT_BREAK) && inner_loop_depth == 0 && inner_switch_depth == 0)
		error_tok(t, "'break' inside defer block bypasses remaining defers");
	if ((t->tag & TT_CONTINUE) && inner_loop_depth == 0)
		error_tok(t, "'continue' inside defer block bypasses remaining defers");
}

// Handle 'defer' keyword: validate context, record deferred statement.
// Returns next token after the defer statement, or NULL if tok is not a valid defer.
static Token *handle_defer_keyword(Token *tok) {
	if (!FEAT(F_DEFER)) return NULL;
	// Distinguish struct field, label, goto target, variable assignment, attribute usage
	if (equal(tok->next, ":") || (last_emitted && (last_emitted->tag & (TT_MEMBER | TT_GOTO))) ||
	    (last_emitted && (is_type_keyword(last_emitted) || (last_emitted->tag & TT_TYPEDEF))) ||
	    is_known_typedef(tok) || (tok->next && (tok->next->tag & TT_ASSIGN)) || ctx->struct_depth > 0 ||
	    is_inside_attribute(tok))
		return NULL;

	// Context validation
	if (ctrl.pending && ctrl.paren_depth > 0)
		error_tok(tok, "defer cannot appear inside control statement parentheses");
	if (ctrl.pending && ctrl.paren_depth == 0)
		error_tok(tok, "defer requires braces in control statements (braceless has no scope)");
	for (int i = 0; i < ctx->stmt_expr_count; i++)
		if (ctx->defer_depth == stmt_expr_levels[i])
			error_tok(tok,
				  "defer cannot be at top level of statement expression; wrap in a block");
	// setjmp/longjmp/vfork/asm safety checks
	if (ctx->current_func_has_setjmp)
		error_tok(tok, "defer cannot be used in functions that call setjmp/longjmp/pthread_exit");
	if (ctx->current_func_has_vfork)
		error_tok(tok, "defer cannot be used in functions that call vfork()");
	if (ctx->current_func_has_asm)
		error_tok(tok, "defer cannot be used in functions containing inline assembly");
	for (int d = ctx->defer_depth - 1; d >= 0; d--)
		if (defer_stack[d].is_switch && ctx->defer_depth - 1 == d)
			error_tok(tok, "defer in switch case requires braces");

	Token *defer_keyword = tok;
	tok = tok->next;
	Token *stmt_start = tok;
	Token *stmt_end = skip_to_semicolon(tok);

	if (stmt_end->kind == TK_EOF || !equal(stmt_end, ";"))
		error_tok(defer_keyword, "unterminated defer statement; expected ';'");

	int bd = 0, pd = 0, bkd = 0;

	// Track loop/switch nesting for break/continue validation.
	int inner_loop_depth = 0;
	int inner_switch_depth = 0;
	// scope_type_at_depth: 1=loop, 2=switch; fixes nested switch-inside-loop.
	uint8_t scope_type_at_depth[4096] = {0};

	bool pk_is_loop = false;
	int pk_paren = 0;
	bool pk_tracking = false;
	bool pk_body_next = false;
	bool pk_do_body = false;
	bool pk_skip_body = false;
	bool prev_was_rbrace = false;

	int braceless_loop_n = 0;
	int braceless_switch_n = 0;
	int braceless_bd = -1;

	for (Token *t = stmt_start; t != stmt_end && t->kind != TK_EOF; t = t->next) {
		bool at_top = (bd == 0 && pd == 0 && bkd == 0);
		if (t != stmt_start && tok_at_bol(t) && at_top && !equal(t, "{") && !equal(t, "(") &&
		    !equal(t, "["))
			error_tok(defer_keyword,
				  "defer statement spans multiple lines without ';' - add semicolon");

		if (pk_body_next || pk_do_body) {
			if (equal(t, "{")) {
				int new_bd = bd + 1;
				bool is_loop = pk_is_loop || pk_do_body;
					if (new_bd < 4096) scope_type_at_depth[new_bd] = is_loop ? 1 : 2;
				
				if (is_loop) inner_loop_depth++;
				else inner_switch_depth++;
				
				pk_body_next = false;
				pk_do_body = false;
			} else {
				if (pk_is_loop || pk_do_body) {
					inner_loop_depth++;
					braceless_loop_n++;
				} else {
					inner_switch_depth++;
					braceless_switch_n++;
				}
				braceless_bd = bd;
				pk_body_next = false;
				pk_do_body = false;
			}
		}

		if (bd > 0 && t->kind == TK_KEYWORD) {
			if ((t->tag & TT_LOOP) && !prev_was_rbrace) {
				if (equal(t, "do")) {
					pk_do_body = true;
					pk_is_loop = true;
				} else {
					pk_tracking = true;
					pk_is_loop = true;
					pk_paren = 0;
				}
			} else if (t->tag & TT_SWITCH) {
				pk_tracking = true;
				pk_is_loop = false;
				pk_paren = 0;
			}
		}

		if (pk_tracking) {
			if (equal(t, "(")) pk_paren++;
			else if (equal(t, ")")) {
				pk_paren--;
				if (pk_paren <= 0) {
					if (!pk_skip_body) pk_body_next = true;
					pk_tracking = false;
					pk_skip_body = false;
				}
			}
		}

		if (equal(t, "{")) bd++;
		else if (equal(t, "}")) {
			if (bd < 4096 && bd > 0) {
				if (scope_type_at_depth[bd] == 1 && inner_loop_depth > 0) inner_loop_depth--;
				else if (scope_type_at_depth[bd] == 2 && inner_switch_depth > 0) inner_switch_depth--;
				scope_type_at_depth[bd] = 0;
			}
			bd--;
		} else if (equal(t, "("))
			pd++;
		else if (equal(t, ")"))
			pd--;
		else if (equal(t, "["))
			bkd++;
		else if (equal(t, "]"))
			bkd--;

		// Braceless body end: '}' or ';' at entry depth, unless 'else' follows.
		if (braceless_bd >= 0 && bd == braceless_bd && pd == 0 && bkd == 0 &&
		    (equal(t, "}") || equal(t, ";")) && !(t->next && equal(t->next, "else"))) {
			if (braceless_loop_n > 0) {
				inner_loop_depth -= braceless_loop_n;
				if (inner_loop_depth < 0) inner_loop_depth = 0;
				braceless_loop_n = 0;
			}
			if (braceless_switch_n > 0) {
				inner_switch_depth -= braceless_switch_n;
				if (inner_switch_depth < 0) inner_switch_depth = 0;
				braceless_switch_n = 0;
			}
			braceless_bd = -1;
		}

		prev_was_rbrace = equal(t, "}");

		if (at_top && t->kind == TK_KEYWORD &&
		    (t->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO | TT_IF | TT_LOOP | TT_SWITCH |
			       TT_CASE | TT_DEFAULT | TT_DEFER)))
			error_tok(defer_keyword,
				  "defer statement appears to be missing ';' (found '%.*s' keyword inside)",
				  t->len,
				  t->loc);
		if (!at_top && (t->tag & TT_DEFER) && !is_known_typedef(t) && !equal(t->next, ":") &&
		    !(t->next && (t->next->tag & TT_ASSIGN)))
			error_tok(defer_keyword, "nested defer is not supported");
		if (!at_top && t->kind == TK_KEYWORD &&
		    (t->tag & (TT_RETURN | TT_GOTO | TT_BREAK | TT_CONTINUE)))
			validate_defer_control_flow(t, inner_loop_depth, inner_switch_depth);
	}

	defer_add(defer_keyword, stmt_start, stmt_end);
	tok = (stmt_end->kind != TK_EOF) ? stmt_end->next : stmt_end;
	end_statement_after_semicolon();
	return tok;
}

static inline bool is_void_return(Token *tok) {
	return ctx->current_func_returns_void || (equal(tok, "(") && tok->next && equal(tok->next, "void") &&
						  tok->next->next && equal(tok->next->next, ")"));
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

	if (match_ch(tok, ';')) tok = tok->next;
	return tok;
}

// Emit orelse action (fallback after 'orelse').
static inline void orelse_open(Token *var_name) {
	if (var_name) {
		OUT_LIT(" if (!");
		out_str(var_name->loc, var_name->len);
		OUT_LIT(") {");
	} else
		OUT_LIT(" {");
}

static Token *emit_orelse_action(Token *tok, Token *var_name, bool has_const, Token *stop_comma) {
	if (equal(tok, "{")) {
		if (var_name) {
			OUT_LIT(" if (!");
			out_str(var_name->loc, var_name->len);
			out_char(')');
		}
		ctx->at_stmt_start = false;
		return tok;
	}

	if (tok->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) {
		uint64_t tag = tok->tag;
		if (tag & TT_RETURN) {
			mark_switch_control_exit();
			tok = tok->next;
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
	OUT_LIT(" if (!");
	out_str(var_name->loc, var_name->len);
	OUT_LIT(") ");
	out_str(var_name->loc, var_name->len);
	OUT_LIT(" =");
	int fdepth = 0;
	while (tok->kind != TK_EOF) {
		if (tok->flags & TF_OPEN) fdepth++;
		else if (tok->flags & TF_CLOSE)
			fdepth--;
		else if (fdepth == 0 && (equal(tok, ";") || (stop_comma && tok == stop_comma)))
			break;
		// Chained orelse: "x = a orelse b orelse c" → close this assignment,
		// then recursively handle the next orelse on the same variable.
		if (fdepth == 0 && is_orelse_keyword(tok)) {
			out_char(';');
			tok = tok->next;
			if (equal(tok, ";"))
				error_tok(tok, "expected statement after 'orelse'");
			return emit_orelse_action(tok, var_name, has_const, stop_comma);
		}
		if (equal(tok, "(") && tok->next && equal(tok->next, "{"))
			error_tok(tok, "GNU statement expressions in orelse fallback values are not "
				  "supported; use 'orelse { ... }' block form instead");
		emit_tok(tok);
		tok = tok->next;
	}
	out_char(';');
	if (equal(tok, ";")) tok = tok->next;
	end_statement_after_semicolon();
	return tok;
}

// Handle 'return' with active defers: save expr, emit defers, then return.
// Returns next token if handled, or NULL to let normal emit proceed.
static Token *handle_return_defer(Token *tok) {
	mark_switch_control_exit();
	if (!has_active_defers()) return NULL;
	tok = tok->next; // skip 'return'
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
	mark_switch_control_exit();
	if (!control_flow_has_defers(is_break)) return NULL;
	OUT_LIT(" {");
	tok = emit_break_continue_defer(tok);
	OUT_LIT(" }");
	end_statement_after_semicolon();
	return tok;
}

// Report goto skipping over a variable declaration (warn or error based on FEAT(F_WARN_SAFETY))
static void report_goto_skips_decl(Token *skipped_decl, Token *label_tok) {
	const char *msg = "goto '%.*s' would skip over this variable declaration (bypasses zero-init)";
	if (FEAT(F_WARN_SAFETY)) warn_tok(skipped_decl, msg, label_tok->len, label_tok->loc);
	else
		error_tok(skipped_decl, msg, label_tok->len, label_tok->loc);
}

static void check_goto_decl_safety(GotoSkipResult *skip, Token *goto_tok, Token *label_tok) {
	if (skip->skipped_decl) report_goto_skips_decl(skip->skipped_decl, label_tok);
	else {
		Token *back_decl = backward_goto_skips_decl(goto_tok, label_tok->loc, label_tok->len);
		if (back_decl) report_goto_skips_decl(back_decl, label_tok);
	}
}

// Handle 'goto': defer cleanup + zeroinit safety checks
static Token *handle_goto_keyword(Token *tok) {
	Token *goto_tok = tok;
	tok = tok->next;

	if (FEAT(F_DEFER)) {
		mark_switch_control_exit();

		if (equal(tok, "*")) {
			if (has_active_defers())
				error_tok(goto_tok,
					  "computed goto cannot be used with active defer statements");
			emit_tok(goto_tok);
			return tok;
		}

		if (is_identifier_like(tok)) {
			GotoSkipResult skip = goto_skips_check(goto_tok, tok->loc, tok->len, true, true);
			if (skip.skipped_defer) {
				const char *msg = "goto '%.*s' would skip over this defer statement";
				if (FEAT(F_WARN_SAFETY))
					warn_tok(skip.skipped_defer, msg, tok->len, tok->loc);
				else
					error_tok(skip.skipped_defer, msg, tok->len, tok->loc);
			}
			check_goto_decl_safety(&skip, goto_tok, tok);

			int target_depth = label_table_lookup(tok->loc, tok->len);
			bool label_found = (target_depth >= 0);
			if (target_depth < 0) target_depth = ctx->defer_depth;

			// Adjust target_depth for sibling-scope gotos so defers are emitted
			if (target_depth >= ctx->defer_depth) {
				int exits = forward_goto_scope_exits(tok->next, tok->loc, tok->len);
				if (exits > 0) {
					target_depth = ctx->defer_depth - exits;
					if (target_depth < 0) target_depth = 0;
				} else if (label_found) {
					exits = backward_goto_scope_exits(goto_tok, tok->loc, tok->len);
					if (exits > 0) {
						target_depth = ctx->defer_depth - exits;
						if (target_depth < 0) target_depth = 0;
					}
				}
			}

			if (goto_has_defers(target_depth)) {
				OUT_LIT(" {");
				emit_goto_defers(target_depth);
				OUT_LIT(" goto");
				emit_tok(tok);
				tok = tok->next;
				if (equal(tok, ";")) {
					emit_tok(tok);
					tok = tok->next;
				}
				OUT_LIT(" }");
				end_statement_after_semicolon();
				return tok;
			}
		}
		emit_tok(goto_tok);
		return tok;
	}

	// Zeroinit-only goto safety
	if (FEAT(F_ZEROINIT) && is_identifier_like(tok)) {
		GotoSkipResult skip = goto_skips_check(goto_tok, tok->loc, tok->len, false, true);
		check_goto_decl_safety(&skip, goto_tok, tok);
	}
	emit_tok(goto_tok);
	return tok;
}

static void handle_case_default(Token *tok) {
	if (!FEAT(F_DEFER)) return;
	int sd = find_switch_scope();
	if (sd < 0) return;
	bool is_case = tok->tag & TT_CASE;
	bool is_default = (tok->tag & TT_DEFAULT) && ctx->generic_paren_depth == 0;
	if (is_default) {
		Token *t = skip_all_attributes(tok->next);
		if (!t || !equal(t, ":")) return;
	}
	if (!is_case && !is_default) return;

	for (int d = ctx->defer_depth - 1; d >= sd; d--) {
		if (defer_stack[d].count > 0 && !defer_stack[d].had_control_exit)
			error_tok(defer_stack[d].entries[0].defer_kw,
				  "defer skipped by switch fallthrough at %s:%d",
				  tok_file(tok)->name,
				  tok_line_no(tok));
		defer_stack[d].count = 0;
		defer_stack[d].had_control_exit = false;
	}

	// case/default inside nested block may bypass zero-init'd declarations
	if (FEAT(F_ZEROINIT) && ctx->defer_depth > sd + 1) {
		for (int d = sd + 1; d < ctx->defer_depth; d++) {
			if (defer_stack[d].has_zeroinit_decl) {
				error_tok(tok,
					  "case/default label inside a nested block within a switch may bypass "
					  "zero-initialization (move the label to the switch body or wrap in its own block)");
			}
		}
	}

	defer_stack[sd].seen_case_label = true;
}

static Token *handle_sue_body(Token *tok) {
	bool is_enum = equal(tok, "enum");
	Token *brace = find_struct_body_brace(tok);
	if (!brace) return NULL;

	if (is_enum) parse_enum_constants(brace, ctx->defer_depth);
	emit_range(tok, brace);
	emit_tok(brace);
	tok = brace->next;
	ctx->struct_depth++;
	defer_push_scope(false);
	defer_stack[ctx->defer_depth - 1].is_struct = true;
	ctx->at_stmt_start = true;
	return tok;
}

static Token *handle_open_brace(Token *tok) {
	// Compound literal inside control parens or before body
	if (ctrl.pending && (ctrl.paren_depth > 0 || !ctrl.parens_just_closed)) {
		emit_tok(tok);
		ctrl.brace_depth++;
		return tok->next;
	}
	if (ctrl.pending && !(ctrl.next_scope & NS_SWITCH)) ctrl.next_scope |= NS_CONDITIONAL;
	ctrl = (ControlFlow){.next_scope = ctrl.next_scope};

	// Detect statement expression: ({
	if (last_emitted && equal(last_emitted, "(")) {
		ARENA_ENSURE_CAP(&ctx->main_arena,
				 stmt_expr_levels,
				 ctx->stmt_expr_count + 1,
				 stmt_expr_capacity,
				 INITIAL_ARRAY_CAP,
				 int);
		stmt_expr_levels[ctx->stmt_expr_count++] = ctx->defer_depth + 1;
	}
	emit_tok(tok);
	tok = tok->next;
	defer_push_scope(true);
	ctx->at_stmt_start = true;
	return tok;
}

static Token *handle_close_brace(Token *tok) {
	// Compound literal close inside control parens
	if (ctrl.pending && ctrl.paren_depth > 0 && ctrl.brace_depth > 0) {
		ctrl.brace_depth--;
		emit_tok(tok);
		return tok->next;
	}
	if (ctx->defer_depth > 0 && defer_stack[ctx->defer_depth - 1].is_struct && ctx->struct_depth > 0)
		ctx->struct_depth--;
	typedef_pop_scope(ctx->defer_depth);
	if (FEAT(F_DEFER) && ctx->defer_depth > 0 && defer_stack[ctx->defer_depth - 1].count > 0)
		emit_defers(DEFER_SCOPE);
	defer_pop_scope();
	emit_tok(tok);
	// Close dangling-else guard brace from bare block orelse
	if (ctx->orelse_guard_count > 0 &&
	    orelse_guard_levels[ctx->orelse_guard_count - 1] == ctx->defer_depth + 1) {
		ctx->orelse_guard_count--;
		OUT_LIT(" }");
	}
	tok = tok->next;
	if (tok && equal(tok, ")") && ctx->stmt_expr_count > 0 &&
	    stmt_expr_levels[ctx->stmt_expr_count - 1] == ctx->defer_depth + 1)
		ctx->stmt_expr_count--;
	ctx->at_stmt_start = true;
	return tok;
}

static inline void argv_builder_add(ArgvBuilder *ab, const char *arg) {
	ARENA_ENSURE_CAP(&ctx->main_arena, ab->data, ab->count + 2, ab->capacity, 64, char *);
	ab->data[ab->count] = arena_strdup(&ctx->main_arena, arg);
	ab->count++;
	ab->data[ab->count] = NULL;
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

// Run a command and wait for completion; returns exit status or -1
// Windows: defined in windows.c via _spawnvp
#ifndef _WIN32
static int run_command(char **argv) {
	char **env = build_clean_environ();
	if (!env) return -1;
	pid_t pid;
	int err = posix_spawnp(&pid, argv[0], NULL, NULL, argv, env);
	if (err) {
		fprintf(stderr, "posix_spawnp: %s: %s\n", argv[0], strerror(err));
		return -1;
	}
	return wait_for_child(pid);
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
		if (bslash && (!slash || bslash > slash)) slash = bslash;
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

#ifndef _WIN32
static bool cc_is_msvc(const char *cc) {
	if (!cc || !*cc) return false;
	const char *base = cc;
	for (const char *p = cc; *p; p++)
		if (*p == '/' || *p == '\\') base = p + 1;
	return (strcasecmp(base, "cl") == 0 || strcasecmp(base, "cl.exe") == 0);
}
#endif

// Build preprocessor argv (shared between pipe and file paths)
static void build_pp_argv(ArgvBuilder *ab, const char *input_file) {
	const char *cc = ctx->extra_compiler ? ctx->extra_compiler : PRISM_DEFAULT_CC;
	argv_builder_add(ab, cc);
	argv_builder_add(ab, "-E");
	argv_builder_add(ab, "-w");

	for (int i = 0; i < ctx->extra_compiler_flags_count; i++)
		argv_builder_add(ab, ctx->extra_compiler_flags[i]);

	for (int i = 0; i < ctx->extra_include_count; i++)
	{
		argv_builder_add(ab, "-I");
		argv_builder_add(ab, ctx->extra_include_paths[i]);
	}

	for (int i = 0; i < ctx->extra_define_count; i++)
	{
		argv_builder_add(ab, "-D");
		argv_builder_add(ab, ctx->extra_defines[i]);
	}

	argv_builder_add(ab, "-D__PRISM__=1");
	if (FEAT(F_DEFER)) argv_builder_add(ab, "-D__PRISM_DEFER__=1");
	if (FEAT(F_ZEROINIT)) argv_builder_add(ab, "-D__PRISM_ZEROINIT__=1");

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
		if (!user_has_posix) argv_builder_add(ab, "-D_POSIX_C_SOURCE=200809L");
		if (!user_has_gnu) argv_builder_add(ab, "-D_GNU_SOURCE");
	}
#endif

	for (int i = 0; i < ctx->extra_force_include_count; i++)
	{
		argv_builder_add(ab, cc_is_msvc(ab->data[0]) ? "/FI" : "-include");
		argv_builder_add(ab, ctx->extra_force_includes[i]);
	}

	argv_builder_add(ab, input_file);
}

// Run system preprocessor (cc -E) via pipe, returns malloc'd output or NULL
static char *preprocess_with_cc(const char *input_file) {
	ArgvBuilder ab;
	argv_builder_init(&ab);
	build_pp_argv(&ab, input_file);
	char **argv = argv_builder_finish(&ab);

	// Set up pipe: child writes preprocessed output, we read it
	int pipefd[2];
	if (pipe(pipefd) == -1) {
		perror("pipe");
		return NULL;
	}

	// Capture preprocessor stderr to a temp file for diagnostics on failure
	char pp_stderr_path[256] = "";
	{
		const char *tmpdir = getenv(TMPDIR_ENVVAR);
		if ((!tmpdir || !*tmpdir) && TMPDIR_ENVVAR_ALT) tmpdir = getenv(TMPDIR_ENVVAR_ALT);
		if (!tmpdir || !*tmpdir) tmpdir = TMPDIR_FALLBACK;
		snprintf(pp_stderr_path, sizeof pp_stderr_path, "%s/prism_pp_err_XXXXXX", tmpdir);
		int fd = mkstemp(pp_stderr_path);
		if (fd >= 0) close(fd);
		else
			pp_stderr_path[0] = '\0';
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addclose(&fa, pipefd[0]);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&fa, pipefd[1]);
	posix_spawn_file_actions_addopen(
	    &fa, STDERR_FILENO, pp_stderr_path[0] ? pp_stderr_path : "/dev/null", O_WRONLY | O_TRUNC, 0644);

	char **env = build_clean_environ();
	pid_t pid;
	int err = posix_spawnp(&pid, argv[0], &fa, NULL, argv, env);
	posix_spawn_file_actions_destroy(&fa);
	close(pipefd[1]);

	if (err) {
		fprintf(stderr, "posix_spawnp: %s\n", strerror(err));
		close(pipefd[0]);
		return NULL;
	}

	// Read all preprocessed output from pipe
	size_t cap = 8192, len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return NULL;
	}

	ssize_t n;
	while ((n = read(pipefd[0], buf + len, cap - len - 1)) > 0) {
		len += (size_t)n;
		if (len + 1 >= cap) {
			cap *= 2;
			char *tmp = realloc(buf, cap);
			if (!tmp) {
				free(buf);
				close(pipefd[0]);
				waitpid(pid, NULL, 0);
				return NULL;
			}
			buf = tmp;
		}
	}
	close(pipefd[0]);
	buf[len] = '\0';

	// Right-size buffer to reduce heap fragmentation
	if (len + 1 < cap) {
		char *fitted = realloc(buf, len + 1);
		if (fitted) buf = fitted;
	}

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
		return NULL;
	}
	if (pp_stderr_path[0]) unlink(pp_stderr_path);

	return buf;
}

// Shared helpers for transpile_tokens

static inline void track_ctrl_paren_open(void) {
	ctrl.paren_depth++;
	ctrl.parens_just_closed = false;
	if (ctrl.await_for_paren) {
		ctrl.for_init = true;
		ctx->at_stmt_start = true;
		ctrl.await_for_paren = false;
	}
}

static inline void track_ctrl_paren_close(void) {
	ctrl.paren_depth--;
	if (ctrl.paren_depth == 0) {
		ctrl.for_init = false;
		ctrl.parens_just_closed = true;
		// Mark next token as stmt start for braceless control-flow bodies
		ctx->at_stmt_start = true;
	}
}

static inline void track_ctrl_semicolon(void) {
	if (ctrl.paren_depth == 1 && ctrl.for_init && ctrl.brace_depth == 0) ctrl.for_init = false;
	else if (ctrl.paren_depth == 0) {
		typedef_pop_scope(ctx->defer_depth + 1);
		control_flow_reset();
	}
}

static inline void track_generic_paren(Token *tok) {
	if (equal(tok, "(")) ctx->generic_paren_depth++;
	else if (equal(tok, ")"))
		ctx->generic_paren_depth--;
}

// Scan ahead for 'orelse' at depth 0 in a bare expression
static Token *find_bare_orelse(Token *tok) {
	int depth = 0;
	Token *prev = NULL;
	for (Token *s = tok; s->kind != TK_EOF; s = s->next) {
		if (s->flags & TF_OPEN) depth++;
		else if (s->flags & TF_CLOSE) {
			if (--depth < 0) return NULL;
		} else if (depth == 0 && equal(s, ";"))
			return NULL;
		if (depth == 0 && (s->tag & TT_ORELSE) && !is_known_typedef(s) &&
		    !(prev && (prev->tag & TT_MEMBER)))
			return s;
		prev = s;
	}
	return NULL;
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

	// Pre-scan for longjmp calls for forward-declared wrappers
	if (FEAT(F_DEFER))
		prescan_longjmp_wrappers(tok);

	if (!FEAT(F_FLATTEN)) {
		collect_system_includes();
		emit_system_includes();
	}

	bool next_func_returns_void = false;
	bool next_func_ret_captured = false;
	Token *prev_toplevel_tok = NULL;
	Token *last_toplevel_paren = NULL;
	Token *last_toplevel_open_paren = NULL;
	Token *func_name_before_paren = NULL;
	int toplevel_paren_depth = 0;
	int ternary_depth = 0;

	while (tok->kind != TK_EOF) {
		Token *next;
		uint32_t tag = tok->tag;

		if (tok->len == 1 && tok->shortcut == '?') ternary_depth++;

#define DISPATCH(handler)                                                                                    \
	{                                                                                                    \
		next = handler(tok);                                                                         \
		if (next) {                                                                                  \
			tok = next;                                                                          \
			continue;                                                                            \
		}                                                                                            \
	}

// Track top-level parentheses for function detection
#define TRACK_TOPLEVEL_PAREN(tok)                                                                            \
	if (ctx->defer_depth == 0) {                                                                         \
		if (match_ch(tok, '(')) {                                                                    \
			if (toplevel_paren_depth == 0) {                                                     \
				last_toplevel_open_paren = tok;                                              \
				func_name_before_paren = prev_toplevel_tok;                                  \
			}                                                                                    \
			toplevel_paren_depth++;                                                              \
		} else if (match_ch(tok, ')')) {                                                             \
			if (--toplevel_paren_depth <= 0) {                                                   \
				toplevel_paren_depth = 0;                                                    \
				last_toplevel_paren = tok;                                                   \
			}                                                                                    \
		}                                                                                            \
		prev_toplevel_tok = tok;                                                                     \
	}

		// Fast path: untagged tokens not at statement start (~70-80% of tokens)
		if (__builtin_expect(!tag && !ctx->at_stmt_start, 1)) {
			if (__builtin_expect(ctrl.pending && tok->len == 1, 0)) {
				char c = tok->shortcut;
				if (c == '(') track_ctrl_paren_open();
				else if (c == ')')
					track_ctrl_paren_close();
			}
			if (__builtin_expect(ctx->generic_paren_depth > 0, 0)) track_generic_paren(tok);
			TRACK_TOPLEVEL_PAREN(tok);
			emit_tok(tok);
			tok = tok->next;
			continue;
		}

		// Slow path: statement-start processing and tagged tokens

		// Track typedefs (must precede zero-init check)
		// For-init typedefs registered at defer_depth+1 for loop-scoped cleanup
		if (ctx->at_stmt_start && ctx->struct_depth == 0) {
			if (tag & TT_TYPEDEF) {
				parse_typedef_declaration(tok, ctx->defer_depth + (ctrl.for_init ? 1 : 0));
			} else if (equal(tok, "raw") && tok->next && (tok->next->tag & TT_TYPEDEF)) {
				parse_typedef_declaration(tok->next, ctx->defer_depth + (ctrl.for_init ? 1 : 0));
				tok = tok->next;
				continue;
			}
		}

		// Register shadows for file-scope _t/__ variable declarations
		// to prevent typedef heuristic misidentification.
		// Skip between function ')' and '{' (K&R parameter declarations).
		if (ctx->at_stmt_start && ctx->defer_depth == 0 && ctx->struct_depth == 0 &&
		    !(tag & TT_TYPEDEF) && !last_toplevel_paren)
			register_toplevel_shadows(tok);

		// Zero-init declarations at statement start.
		// Skip structural tokens to avoid scanning brace blocks (RSS growth on musl ARM64).
		if (ctx->at_stmt_start && !(tag & TT_STRUCTURAL) &&
		    (!ctrl.pending || ctrl.for_init || ctrl.parens_just_closed)) {
			next = try_zero_init_decl(tok);
			if (next) {
				tok = next;
				ctx->at_stmt_start = true;
				continue;
			}

			// Bare expression orelse.
			// Skip keywords that introduce sub-statements or labels
			// to avoid sweeping them into the if(!(...)) condition.
			if (FEAT(F_ORELSE) && ctx->defer_depth > 0 && ctx->struct_depth == 0 &&
			    !(tok->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO |
					  TT_CASE | TT_DEFAULT | TT_IF | TT_LOOP | TT_SWITCH))) {
				Token *orelse_tok = find_bare_orelse(tok);
				if (orelse_tok) {
					if (ctrl.for_init)
						error_tok(tok, "orelse cannot be used in for-loop initializers");
					if (is_orelse_keyword(tok))
						error_tok(tok, "expected expression before 'orelse'");
					// Wrap in braces to prevent dangling-else binding
					OUT_LIT(" {");

					// Find assignment target for re-emit in fallback
					Token *bare_lhs_start = tok;
					Token *bare_assign_eq = NULL;
					{
						int sd = 0;
						for (Token *s = tok; s != orelse_tok; s = s->next) {
							if (s->flags & TF_OPEN) sd++;
							else if (s->flags & TF_CLOSE) sd--;
							else if (sd == 0 && equal(s, "=")) {
								bare_assign_eq = s;
								break;
							}
						}
					}

					// Error if LHS has side effects (double evaluation)
					if (bare_assign_eq) {
						for (Token *s = bare_lhs_start; s != bare_assign_eq; s = s->next) {
							if (equal(s, "++") || equal(s, "--"))
								error_tok(s, "orelse fallback on assignment with side effects "
									  "in the target expression (double evaluation); "
									  "use a temporary variable instead");
							if (is_valid_varname(s) && !is_type_keyword(s) && s->next &&
							    s->next != bare_assign_eq && equal(s->next, "("))
								error_tok(s, "orelse fallback on assignment with a function call "
									  "in the target expression (double evaluation); "
									  "use a temporary variable instead");
						}
					}

					// Check if fallback is a bare value (not control flow or block)
					Token *after_orelse = orelse_tok->next;
					bool is_bare_fallback = bare_assign_eq && after_orelse &&
						!(after_orelse->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO)) &&
						!equal(after_orelse, "{") && !equal(after_orelse, ";");

					// Hoist TK_PREP_DIR tokens before the if wrapper (invalid inside parens)
					for (Token *s = tok; s != orelse_tok; s = s->next) {
						if (s->kind == TK_PREP_DIR) { emit_tok(s); out_char('\n'); }
					}

					if (is_bare_fallback) {
						// Use ternary to preserve compound literal lifetime (C99 §6.5.2.5)
						// Emit: LHS = (LHS = val) ? LHS : (fallback);
						out_char(' ');
						emit_range(bare_lhs_start, bare_assign_eq);
						OUT_LIT(" = (");
						for (Token *t = bare_lhs_start; t != orelse_tok; t = t->next) {
							if (t->kind == TK_PREP_DIR) continue;
							emit_tok(t);
						}
						OUT_LIT(") ?");
						emit_range(bare_lhs_start, bare_assign_eq);
						OUT_LIT(" : (");
						tok = orelse_tok->next; // skip 'orelse'
						int fd = 0;
						while (tok->kind != TK_EOF) {
							if (tok->flags & TF_OPEN) fd++;
							else if (tok->flags & TF_CLOSE) fd--;
							else if (fd == 0 && (equal(tok, ";") || equal(tok, ","))) break;
							// Chained orelse: close current ternary, start new one
							if (fd == 0 && is_orelse_keyword(tok)) {
								OUT_LIT(");");
								emit_range(bare_lhs_start, bare_assign_eq);
								OUT_LIT(" =");
								emit_range(bare_lhs_start, bare_assign_eq);
								OUT_LIT(" ?");
								emit_range(bare_lhs_start, bare_assign_eq);
								OUT_LIT(" : (");
								tok = tok->next; // skip 'orelse'
								continue;
							}
							emit_tok(tok);
							tok = tok->next;
						}
						OUT_LIT(");");
						if (equal(tok, ";")) tok = tok->next;
						else if (equal(tok, ",")) tok = tok->next;
						OUT_LIT(" }");
						end_statement_after_semicolon();
						continue;
					}

				OUT_LIT(" if (!(");
				while (tok != orelse_tok) {
					if (tok->kind == TK_PREP_DIR) { tok = tok->next; continue; }
					emit_tok(tok);
					tok = tok->next;
				}
				OUT_LIT("))");
				tok = tok->next;

					if (equal(tok, ";"))
						error_tok(tok, "expected statement after 'orelse'");

					bool is_block_action = equal(tok, "{");
					tok = emit_orelse_action(tok, NULL, false, NULL);
					if (is_block_action) {
						// Record level so handle_close_brace emits the guard '}'
						ARENA_ENSURE_CAP(&ctx->main_arena,
								 orelse_guard_levels,
								 ctx->orelse_guard_count + 1,
								 orelse_guard_capacity,
								 INITIAL_ARRAY_CAP,
								 int);
						orelse_guard_levels[ctx->orelse_guard_count++] =
						    ctx->defer_depth + 1;
					} else {
						OUT_LIT(" }");
					}
					continue;
				}
			}
		}
		ctx->at_stmt_start = false;

		if (tag & TT_NORETURN_FN) {
			if (tok->next && equal(tok->next, "(")) {
				mark_switch_control_exit();
				if (FEAT(F_DEFER) && has_active_defers())
					fprintf(stderr,
						"%s:%d: warning: '%.*s' called with active defers (defers "
						"will not run)\n",
						tok_file(tok)->name,
						tok_line_no(tok),
						tok->len,
						tok->loc);
			}
		}

		// Tag-dependent dispatch
		if (tag) {
			// Keyword dispatch

			if (__builtin_expect(tag & TT_DEFER, 0) && ctx->generic_paren_depth == 0)
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
					ctrl.next_scope |= NS_LOOP;
					ctrl.pending = true;
					if (equal(tok, "do")) ctrl.parens_just_closed = true;
				}
				if (equal(tok, "for") && FEAT(F_DEFER | F_ZEROINIT)) {
					ctrl.pending = true;
					ctrl.await_for_paren = true;
				}
			}

			if ((tag & TT_GENERIC) && ctx->generic_paren_depth == 0) {
				emit_tok(tok);
				last_emitted = tok;
				tok = tok->next;
				if (tok && equal(tok, "(")) {
					ctx->generic_paren_depth = 1;
					emit_tok(tok);
					last_emitted = tok;
					tok = tok->next;
				}
				continue;
			}

			if (FEAT(F_DEFER) && (tag & TT_SWITCH)) {
				ctrl.next_scope |= NS_SWITCH;
				ctrl.pending = true;
			}

			if (tag & TT_IF) {
				ctrl.pending = true;
				if (equal(tok, "else")) {
					ctrl.parens_just_closed = true;
					ctx->at_stmt_start = true;
				}
			}

			// Case/default label handling
			if (tag & (TT_CASE | TT_DEFAULT)) handle_case_default(tok);

		} // end if (tag)

		if (__builtin_expect(ctx->generic_paren_depth > 0, 0)) track_generic_paren(tok);

		// Void function detection and return type capture at top level
		if (ctx->defer_depth == 0 &&
		    (tag & (TT_TYPE | TT_QUALIFIER | TT_SKIP_DECL | TT_ATTR | TT_INLINE) ||
		     equal(tok, "_Pragma") ||
		     (is_identifier_like(tok) && (is_void_typedef(tok) || is_known_typedef(tok))))) {
			if (is_void_function_decl(tok)) next_func_returns_void = true;
			else if (!next_func_ret_captured &&
				 (tag & (TT_TYPE | TT_QUALIFIER | TT_ATTR | TT_INLINE) ||
				  equal(tok, "_Pragma") ||
				  (is_identifier_like(tok) && is_known_typedef(tok))))
				next_func_ret_captured = try_capture_func_return_type(tok);
		}

		if (tag & TT_SUE) // struct/union/enum body
			DISPATCH(handle_sue_body);

		// Structural punctuation: { } ; :
		if (tag & TT_STRUCTURAL) {
			if (match_ch(tok, '{')) {
				// Function definition detection at top level
				if (ctx->defer_depth == 0 && FEAT(F_DEFER)) {
					bool is_func_def = false;
					if (prev_toplevel_tok && equal(prev_toplevel_tok, ")"))
						is_func_def = true;
					else if (last_toplevel_paren &&
						 is_knr_params(last_toplevel_paren->next, tok))
						is_func_def = true;
					else if (last_toplevel_paren) {
						// Skip attrs, array dims, param lists between ')' and '{'
						Token *after = skip_declarator_suffix(last_toplevel_paren->next);
						if (after == tok) is_func_def = true;
					}
					if (is_func_def) {
						scan_labels_in_function(tok);
						register_param_shadows(last_toplevel_open_paren,
								       last_toplevel_paren);
						ctx->current_func_returns_void = next_func_returns_void;
						if (next_func_returns_void || !next_func_ret_captured)
							clear_func_ret_type();
					} else {
						clear_func_ret_type();
					}
					next_func_returns_void = false;
					next_func_ret_captured = false;
					last_toplevel_paren = NULL;
				}
				// Reset K&R tracking at file scope to avoid stale shadow table guard
				if (ctx->defer_depth == 0)
					last_toplevel_paren = NULL;
				tok = handle_open_brace(tok);
				continue;
			}
			if (match_ch(tok, '}')) {
				tok = handle_close_brace(tok);
				// Reset per-function flags when leaving a function body
				if (ctx->defer_depth == 0) {
					ctx->current_func_has_setjmp = false;
					ctx->current_func_has_vfork = false;
					ctx->current_func_has_asm = false;
				}
				continue;
			}
			char c = tok->loc[0];
			if (c == ';') {
				if (ctrl.pending) track_ctrl_semicolon();
				if (!ctrl.pending) end_statement_after_semicolon();
				if (ctx->defer_depth == 0) {
					next_func_returns_void = false;
					next_func_ret_captured = false;
				}
				emit_tok(tok);
				tok = tok->next;
				continue;
			}
			if (c == ':') {
				if (ternary_depth > 0) {
					ternary_depth--;
				} else if (ctx->generic_paren_depth > 0) {
				} else if (last_emitted &&
				           (is_identifier_like(last_emitted) || last_emitted->kind == TK_NUM) &&
				           ctx->struct_depth == 0 && ctx->defer_depth > 0) {
					emit_tok(tok);
					tok = tok->next;
					ctx->at_stmt_start = true;
					continue;
				}
			}
		}

		if (__builtin_expect(tok->kind == TK_PREP_DIR, 0)) {
			emit_tok(tok);
			tok = tok->next;
			ctx->at_stmt_start = true;
			continue;
		}

		if (ctrl.pending && tok->len == 1) {
			char c = tok->loc[0];
			if (c == '(') track_ctrl_paren_open();
			else if (c == ')')
				track_ctrl_paren_close();
		}

		TRACK_TOPLEVEL_PAREN(tok);

		// Warn on unprocessed 'orelse' in unsupported context
		if (__builtin_expect(FEAT(F_ORELSE) && is_orelse_keyword(tok) &&
				     ctx->struct_depth == 0, 0))
			error_tok(tok,
				  "'orelse' cannot be used here (it must appear at the "
				  "statement level in a declaration or bare expression)");

		emit_tok(tok);
		tok = tok->next;
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
	if (!ctx->keyword_map.capacity) init_keyword_map();

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

	defer_stack = NULL;
	ctx->defer_depth = 0;
	defer_stack_capacity = 0;

	hashmap_zero(&label_table.name_map);
	label_table.labels = NULL;
	label_table.count = 0;
	label_table.capacity = 0;

	system_includes_reset();

	stmt_expr_levels = NULL;
	ctx->stmt_expr_count = 0;
	stmt_expr_capacity = 0;
	orelse_guard_levels = NULL;
	ctx->orelse_guard_count = 0;
	orelse_guard_capacity = 0;

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
	if (ctx->active_temp_output[0]) {
		remove(ctx->active_temp_output);
		ctx->active_temp_output[0] = '\0';
	}
	ctx->active_typeof_vars = NULL;
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
	if (!ctx->keyword_map.capacity) init_keyword_map();

	char *pp_buf = preprocess_with_cc((char *)input_file);
	if (!pp_buf) {
		result.status = PRISM_ERR_IO;
		result.error_msg = strdup("Preprocessing failed");
		goto cleanup;
	}

	Token *tok = tokenize_buffer((char *)input_file, pp_buf);
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
	if (!ctx->keyword_map.capacity) init_keyword_map();

	char *buf = strdup(source);
	if (!buf) {
		result.status = PRISM_ERR_IO;
		result.error_msg = strdup("Out of memory");
		goto src_cleanup;
	}

	Token *tok = tokenize_buffer((char *)fname, buf);
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

static char **build_argv(const char *first, ...) {
	ArgvBuilder ab;
	argv_builder_init(&ab);
	if (first) argv_builder_add(&ab, first);
	va_list ap;
	va_start(ap, first);
	const char *arg;
	while ((arg = va_arg(ap, const char *)) != NULL) argv_builder_add(&ab, arg);
	va_end(ap);
	return argv_builder_finish(&ab);
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
	int mib = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
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
#endif

#ifndef _WIN32
static const char *get_install_path(void) { return INSTALL_PATH; }

static bool ensure_install_dir(const char *p) {
	(void)p;
	return true;
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
		fprintf(stderr, "[prism] Failed to create install directory\n");
		return 1;
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
		// Add install directory to user PATH on Windows
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
		char **argv = build_argv("sudo", "rm", "-f", install_path, NULL);
		run_command(argv);

		argv = build_argv("sudo", "cp", self_path, install_path, NULL);
		int status = run_command(argv);
		if (status != 0) {
			fprintf(stderr, "Failed to install\n");
			return 1;
		}

		argv = build_argv("sudo", "chmod", "+x", install_path, NULL);
		run_command(argv);
	}
#endif

	printf("[prism] Installed!\n");
	check_path_shadow(install_path);
	return 0;
}

static bool is_prism_cc(const char *cc) {
	if (!cc || !*cc) return false;
	const char *base = path_basename(cc);
	if (strncmp(base, "prism", 5) == 0) {
		char next = base[5];
		if (next == '\0' || next == ' ' || next == '.') return true;
	}
	return false;
}

static const char *get_real_cc(const char *cc) {
	if (!cc || !*cc || is_prism_cc(cc)) return PRISM_DEFAULT_CC;
	if (!strpbrk(cc, "/\\")) return cc;

	char cc_real[PATH_MAX], self_real[PATH_MAX];
	if (get_self_exe_path(self_real, sizeof(self_real)) && realpath(cc, cc_real))
		if (strcmp(cc_real, self_real) == 0) return PRISM_DEFAULT_CC;

	return cc;
}

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

// Check if the system compiler is clang
static bool cc_is_clang(const char *cc) {
#ifdef __APPLE__
	if (!cc || !*cc || strcmp(cc, "cc") == 0 || strcmp(cc, "gcc") == 0) return true;
#endif
	if (!cc || !*cc) return false;
	const char *base = path_basename(cc);
	return strncmp(base, "clang", 5) == 0;
}

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

static Cli cli_parse(int argc, char **argv) {
	bool passthrough = false;
	Cli cli = {.features = prism_defaults()};

	char *env_cc = getenv("PRISM_CC");
	if (!env_cc || !*env_cc || is_prism_cc(env_cc)) {
		env_cc = getenv("CC");
		if (is_prism_cc(env_cc)) env_cc = NULL;
	}
	cli.cc = (env_cc && *env_cc) ? env_cc : PRISM_DEFAULT_CC;

	for (int i = 1; i < argc; i++) {
		char *a = argv[i];

		if (a[0] == '-' &&
		a[1] !=
			'\0')
		{
			char c1 = a[1];

			if (c1 == 'o') {
				if (a[2]) cli.output = a + 2;
				else if (i + 1 < argc) cli.output = argv[++i];
				continue;
			}

			if (c1 == 'c' && !a[2]) {
				cli.compile_only = true;
				CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
				continue;
			}

			if (c1 == 'E' && !a[2]) {
				passthrough = true;
				CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
				continue;
			}

			if (c1 == 'h' && !a[2]) {
				print_help();
				exit(0);
			}

			if (c1 == '-') {
				if (!strcmp(a, "--help")) {print_help(); exit(0);}
				if (!strcmp(a, "--version")) { printf("prism %s\n", PRISM_VERSION); exit(0);}
				if (str_startswith(a, "--prism-cc=")) { cli.cc = a + 11; continue; }
				if (!strcmp(a, "--prism-verbose")) { cli.verbose = true; continue; }
				if (str_startswith(a, "--prism-emit=")) { cli.mode = CLI_EMIT; cli.output = a + 13; continue; }
				if (!strcmp(a, "--prism-emit")) { cli.mode = CLI_EMIT; continue; }
				CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
				continue;
			}

			if (c1 == 'f') {
				if (!strcmp(a, "-fno-defer")) { cli.features.defer = false; continue; }
				if (!strcmp(a, "-fno-zeroinit")) { cli.features.zeroinit = false; continue; }
				if (!strcmp(a, "-fno-orelse")) { cli.features.orelse = false; continue; }
				if (!strcmp(a, "-fno-line-directives")) { cli.features.line_directives = false; continue; }
				if (!strcmp(a, "-fno-safety")) { cli.features.warn_safety = true; continue; }
				if (!strcmp(a, "-fflatten-headers")) { cli.features.flatten_headers = true; continue; }
				if (!strcmp(a, "-fno-flatten-headers")) { cli.features.flatten_headers = false; continue; }
				
				CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
				continue;
			}

			CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
			continue;
		}

		if (!strcmp(a, "run")) { cli.mode = CLI_RUN; continue; }
		if (!strcmp(a, "transpile")) { cli.mode = CLI_EMIT; continue; }
		if (!strcmp(a, "install")) { cli.mode = CLI_INSTALL; continue; }

		if ((has_ext(a, ".c") || has_ext(a, ".i")) && !passthrough) {
			CLI_PUSH(cli.sources, cli.source_count, cli.source_cap, a);
			continue;
		}

		CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
	}

	return cli;
}

static void add_warn_suppress(ArgvBuilder *ab, bool clang, bool msvc) {
	if (msvc) {
		argv_builder_add(ab, "/wd4100");
		argv_builder_add(ab, "/wd4189");
		argv_builder_add(ab, "/wd4244");
		argv_builder_add(ab, "/wd4267");
		argv_builder_add(ab, "/wd4068");
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
	for (int i = 0; i < (int)(sizeof(w) / sizeof(*w)); i++) argv_builder_add(ab, w[i]);
	if (clang) argv_builder_add(ab, "-Wno-unknown-warning-option");
	else argv_builder_add(ab, "-Wno-logical-op");
}

static void verbose_argv(char **args) {
	fprintf(stderr, "[prism]");
	for (int i = 0; args[i]; i++) fprintf(stderr, " %s", args[i]);
	fprintf(stderr, "\n");
}

static void add_output_flags(ArgvBuilder *ab, const Cli *cli, const char *temp_exe, bool msvc) {
	static char defobj[PATH_MAX];
	const char *out = NULL;

	if (cli->mode == CLI_RUN) out = temp_exe;
	else if (cli->output)
		out = cli->output;
	else if (cli->compile_only && cli->source_count == 1) {
		const char *base = path_basename(cli->sources[0]);
		snprintf(defobj, sizeof(defobj), "%s", base);
		char *dot = strrchr(defobj, '.');
		if (dot) snprintf(dot, sizeof(defobj) - (dot - defobj), "%s", msvc ? ".obj" : ".o");
		out = defobj;
	}

	if (!out) return;

	if (msvc) {
		static char flag[PATH_MAX + 8]; // cl.exe: /Fe:exe or /Fo:obj
		if (cli->compile_only) snprintf(flag, sizeof(flag), "/Fo:%s", out);
		else snprintf(flag, sizeof(flag), "/Fe:%s", out);
		argv_builder_add(ab, flag);
	} else {
		argv_builder_add(ab, "-o");
		argv_builder_add(ab, out);
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
	bool msvc = cc_is_msvc(compiler);
	ArgvBuilder ab;
	argv_builder_init(&ab);
	argv_builder_add(&ab, compiler);
	for (int i = 0; i < cli->cc_arg_count; i++) argv_builder_add(&ab, cli->cc_args[i]);
	if (cli->output) {
		if (msvc) {
			static char fe_flag[PATH_MAX + 8];
			snprintf(fe_flag, sizeof(fe_flag), "/Fe:%s", cli->output);
			argv_builder_add(&ab, fe_flag);
		} else {
			argv_builder_add(&ab, "-o");
			argv_builder_add(&ab, cli->output);
		}
	}
	char **pass = argv_builder_finish(&ab);
	if (cli->verbose) verbose_argv(pass);
	int st = run_command(pass);
	return st;
}

static int install_from_source(Cli *cli) {
	char temp_bin[PATH_MAX];
	snprintf(temp_bin, sizeof(temp_bin), "%sprism_install_%d" EXE_SUFFIX, get_tmp_dir(), getpid());

	const char *cc = get_real_cc(cli->cc ? cli->cc : getenv("PRISM_CC"));
	if (!cc || (strcmp(cc, "cc") == 0 && !cli->cc)) {
		cc = getenv("CC");
		if (cc) cc = get_real_cc(cc);
	}

	if (!cc) cc = PRISM_DEFAULT_CC;

	bool msvc = cc_is_msvc(cc);

	char **temp_files = malloc(cli->source_count * sizeof(char *));
	if (!temp_files) die("Memory allocation failed");

	for (int i = 0; i < cli->source_count; i++) {
		temp_files[i] = malloc(PATH_MAX);
		if (!temp_files[i]) die("Memory allocation failed");
		snprintf(temp_files[i], PATH_MAX, "%sprism_install_%d_%d.c", get_tmp_dir(), getpid(), i);

		PrismResult result = prism_transpile_file(cli->sources[i], cli->features);
		if (result.status != PRISM_OK) {
			fprintf(stderr,
				"%s:%d:%d: error: %s\n",
				cli->sources[i],
				result.error_line,
				result.error_col,
				result.error_msg ? result.error_msg : "transpilation failed");
			for (int j = 0; j <= i; j++) {
				remove(temp_files[j]);
				free(temp_files[j]);
			}
			free(temp_files);
			return 1;
		}

		FILE *f = fopen(temp_files[i], "w");
		if (!f) {
			prism_free(&result);
			die("Failed to create temp file");
		}
		fwrite(result.output, 1, result.output_len, f);
		fclose(f);
		prism_free(&result);
	}

	ArgvBuilder ab;
	argv_builder_init(&ab);
	argv_builder_add(&ab, cc);
	argv_builder_add(&ab, msvc ? "/O2" : "-O2");

	for (int i = 0; i < cli->source_count; i++) argv_builder_add(&ab, temp_files[i]);

	for (int i = 0; i < cli->cc_arg_count; i++) argv_builder_add(&ab, cli->cc_args[i]);

	if (msvc) {
		static char fe_flag[PATH_MAX + 8];
		snprintf(fe_flag, sizeof(fe_flag), "/Fe:%s", temp_bin);
		argv_builder_add(&ab, fe_flag);
	} else {
		argv_builder_add(&ab, "-o");
		argv_builder_add(&ab, temp_bin);
	}
	char **argv_cc = argv_builder_finish(&ab);

	if (cli->verbose) verbose_argv(argv_cc);

	int status = run_command(argv_cc);

	for (int i = 0; i < cli->source_count; i++) {
		remove(temp_files[i]);
		free(temp_files[i]);
	}
	free(temp_files);

	if (status != 0) return 1;

	int result = install(temp_bin);
	remove(temp_bin);
	return result;
}

static int compile_sources(Cli *cli) {
	int status = 0;
	const char *compiler = get_real_cc(cli->cc);
	bool clang = cc_is_clang(compiler);
	bool msvc = cc_is_msvc(compiler);
	char temp_exe[PATH_MAX];
	make_run_temp(temp_exe, sizeof(temp_exe), cli->mode);

	use_linemarkers = FEAT(F_FLATTEN) && !clang && !msvc;

	if (cli->source_count == 1 && !msvc) {
		ArgvBuilder ab;
		argv_builder_init(&ab);
		argv_builder_add(&ab, compiler);

		argv_builder_add(&ab, "-x");
		argv_builder_add(&ab, "c");
		if (FEAT(F_FLATTEN) && !clang) argv_builder_add(&ab, "-fpreprocessed");
		argv_builder_add(&ab, "-");

		if (cli->cc_arg_count > 0) {
			argv_builder_add(&ab, "-x");
			argv_builder_add(&ab, "none");
		}

		for (int i = 0; i < cli->cc_arg_count; i++) argv_builder_add(&ab, cli->cc_args[i]);

		add_warn_suppress(&ab, clang, false);
		add_output_flags(&ab, cli, temp_exe, false);
		char **argv_cc = argv_builder_finish(&ab);

		if (cli->verbose) fprintf(stderr, "[prism] Transpiling %s (pipe → cc)\n", cli->sources[0]);
		status = transpile_and_compile((char *)cli->sources[0], argv_cc, cli->verbose);
	} else {
		char **temps = calloc(cli->source_count, sizeof(char *));
		if (!temps) die("Out of memory");

		for (int i = 0; i < cli->source_count; i++) {
			temps[i] = malloc(512);
			if (!temps[i]) die("Out of memory");
			if (make_temp_file(temps[i], 512, NULL, 0, cli->sources[i]) < 0)
				die("Failed to create temp file");
			if (cli->verbose)
				fprintf(stderr, "[prism] Transpiling %s -> %s\n", cli->sources[i], temps[i]);
			if (!transpile((char *)cli->sources[i], temps[i])) {
				for (int j = 0; j <= i; j++) {
					remove(temps[j]);
					free(temps[j]);
				}
				free(temps);
				die("Transpilation failed");
			}
		}

		ArgvBuilder ab;
		argv_builder_init(&ab);
		argv_builder_add(&ab, compiler);

		if (FEAT(F_FLATTEN) && !clang && !msvc) argv_builder_add(&ab, "-fpreprocessed");

		for (int i = 0; i < cli->source_count; i++) argv_builder_add(&ab, temps[i]);

		if (FEAT(F_FLATTEN) && !clang && !msvc) argv_builder_add(&ab, "-fno-preprocessed");

		for (int i = 0; i < cli->cc_arg_count; i++) argv_builder_add(&ab, cli->cc_args[i]);

		add_warn_suppress(&ab, clang, msvc);
		add_output_flags(&ab, cli, temp_exe, msvc);
		char **argv_cc = argv_builder_finish(&ab);

		if (cli->verbose) verbose_argv(argv_cc);
		status = run_command(argv_cc);

		for (int i = 0; i < cli->source_count; i++) {
			remove(temps[i]);
			free(temps[i]);
		}
		free(temps);
	}

	if (status != 0) {
		if (temp_exe[0]) remove(temp_exe);
		return status;
	}

	if (cli->mode == CLI_RUN) {
		char **run = build_argv(temp_exe, NULL);
		if (cli->verbose) fprintf(stderr, "[prism] Running %s\n", temp_exe);
		status = run_command(run);
		remove(temp_exe);
	}

	return status;
}

int main(int argc, char **argv) {
#ifndef _WIN32
	signal(SIGPIPE, SIG_IGN);
#endif
	int status = 0;
	prism_ctx_init();

	if (argc < 2) {
		print_help();
		return 0;
	}

	Cli cli = cli_parse(argc, argv);
	ctx->features = features_to_bits(cli.features);
	ctx->extra_compiler = get_real_cc(cli.cc);
	ctx->extra_compiler_flags = cli.cc_args;
	ctx->extra_compiler_flags_count = cli.cc_arg_count;

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

	return status;
}

#endif // PRISM_LIB_MODE

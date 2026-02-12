#define PRISM_VERSION "0.114.0"

#ifdef _WIN32
#define PRISM_DEFAULT_CC "cl"
#define EXE_SUFFIX ".exe"
#define INSTALL_PATH "prism.exe"
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

#define INSTALL_PATH "/usr/local/bin/prism"
#define PRISM_DEFAULT_CC "cc"
#define EXE_SUFFIX ""
#endif

#ifdef PRISM_LIB_MODE
#define PRISM_API
#else
#define PRISM_API static
#endif

#include "parse.c"

#ifdef __APPLE__
#include <mach-o/dyld.h> // _NSGetExecutablePath
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
#define control_flow_has_defers(include_switch) has_defers_for((include_switch) ? DEFER_BREAK : DEFER_CONTINUE, 0)
#define goto_has_defers(depth) has_defers_for(DEFER_TO_DEPTH, depth)
#define typedef_add(name, len, depth, is_vla, is_void) typedef_add_entry(name, len, depth, TDK_TYPEDEF, is_vla, is_void)
#define typedef_add_shadow(name, len, depth) typedef_add_entry(name, len, depth, TDK_SHADOW, false, false)
#define typedef_add_enum_const(name, len, depth) typedef_add_entry(name, len, depth, TDK_ENUM_CONST, false, false)
#define typedef_add_vla_var(name, len, depth) typedef_add_entry(name, len, depth, TDK_VLA_VAR, true, false)
#define argv_builder_init(ab) (*(ab) = (ArgvBuilder){0})
#define argv_builder_finish(ab) ((ab)->data)

typedef struct
{
  bool defer;
  bool zeroinit;
  bool line_directives;
  bool warn_safety;     // If true, safety checks warn instead of error
  bool flatten_headers; // If true, include flattened system headers (default: true)
  bool orelse;          // If true, enable orelse keyword (default: true)

  // Preprocessor configuration (optional - can be left NULL/0 for defaults)
  const char *compiler;       // Compiler to use (default: "cc")
  const char **include_paths; // -I paths
  int include_count;
  const char **defines; // -D macros
  int define_count;
  const char **compiler_flags; // Additional flags (-std=c99, -m32, etc.)
  int compiler_flags_count;
  const char **force_includes; // -include files
  int force_include_count;
} PrismFeatures;

typedef enum
{
  PRISM_OK = 0,
  PRISM_ERR_SYNTAX,
  PRISM_ERR_SEMANTIC,
  PRISM_ERR_IO,
} PrismStatus;

typedef struct
{
  PrismStatus status;
  char *output; // transpiled C (caller frees with prism_free)
  size_t output_len;
  char *error_msg; // error message (NULL on success)
  int error_line;
  int error_col;
} PrismResult;

typedef struct
{
  Token *end;        // First token after the type specifier
  bool saw_type;     // True if a type was recognized
  bool is_struct;    // True if struct/union/enum type
  bool is_typedef;   // True if user-defined typedef
  bool is_vla;       // True if VLA typedef or struct with VLA member
  bool has_typeof;   // True if typeof/typeof_unqual (cannot determine VLA at transpile-time)
  bool has_atomic;   // True if _Atomic qualifier/specifier present
  bool has_register; // True if register storage class
  bool has_volatile; // True if volatile qualifier
  bool has_const;    // True if const qualifier
} TypeSpecResult;

typedef enum
{
  DEFER_SCOPE,    // DEFER_SCOPE=current only
  DEFER_ALL,      // DEFER_ALL=all scopes
  DEFER_BREAK,    // DEFER_BREAK=stop at loop/switch,
  DEFER_CONTINUE, // DEFER_CONTINUE=stop at loop,
  DEFER_TO_DEPTH  // DEFER_TO_DEPTH=stop at given depth (for goto)
} DeferEmitMode;

typedef enum
{
  TDK_TYPEDEF,
  TDK_SHADOW,
  TDK_ENUM_CONST,
  TDK_VLA_VAR // VLA variable (not typedef, but actual VLA array variable)
} TypedefKind;

typedef struct
{
  Token *stmt, *end, *defer_kw;
} DeferEntry;

typedef struct
{
  DeferEntry *entries; // Deferred statements (dynamic)
  int count;
  int capacity;
  bool is_loop;          // true if this scope is a for/while/do loop
  bool is_switch;        // true if this scope is a switch statement
  bool had_control_exit; // true if unconditional break/return/goto/continue was seen
                         // NOTE: only set on switch scopes (by mark_switch_control_exit)
  bool is_conditional;   // true if this scope is an if/while/for block (for tracking conditional control exits)
  bool seen_case_label;  // true if case/default label seen in this switch scope (for zero-init safety)
  bool is_struct;        // true if this scope is a struct/union/enum body
} DeferScope;

typedef struct
{
  char *name;
  int name_len;
  int scope_depth;
} LabelInfo;

typedef struct
{
  LabelInfo *labels; // Dynamic array
  int count;
  int capacity;
  HashMap name_map; // For O(1) lookups by name
} LabelTable;

typedef struct
{
  char *name; // Points into token stream (no alloc needed)
  int len;
  int scope_depth;    // Scope where defined (aligns with ctx->defer_depth)
  bool is_vla;        // True if typedef refers to a VLA type
  bool is_void;       // True if typedef resolves to void (e.g., typedef void Void)
  bool is_shadow;     // True if this entry shadows a typedef (variable with same name)
  bool is_enum_const; // True if this is an enum constant (compile-time constant)
  int prev_index;     // Index of previous entry with same name (-1 if none), for hash map chaining
} TypedefEntry;

typedef struct
{
  TypedefEntry *entries;
  int count;
  int capacity;
  HashMap name_map; // Maps name -> index+1 of most recent entry (0 means not found)
  uint64_t bloom;   // Bloom filter for fast-reject of non-typedef lookups
} TypedefTable;

typedef enum
{
  CLI_DEFAULT,
  CLI_RUN,
  CLI_EMIT,
  CLI_INSTALL
} CliMode;

typedef struct
{
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

// Control flow state — tracks control keyword to body transition
enum
{
  NS_LOOP = 1,
  NS_SWITCH = 2,
  NS_CONDITIONAL = 4
};

typedef struct
{
  int paren_depth;
  int brace_depth;
  uint8_t next_scope; // NS_LOOP | NS_SWITCH | NS_CONDITIONAL
  bool pending;
  bool parens_just_closed;
  bool for_init;
  bool await_for_paren;
} ControlFlow;

typedef struct
{
  char **data;
  int count, capacity;
} ArgvBuilder;

// Unified token walker — iterates tokens tracking brace depth, struct bodies,
// and _Generic skipping. Used by scan_labels_in_function and goto_skips_check
// to eliminate duplicated structural token-walking logic.
typedef struct
{
  Token *tok;               // Current token
  Token *prev;              // Previous meaningful token
  int depth;                // Brace depth (updated on { and })
  int struct_depth;         // Nesting inside struct/union/enum bodies
  int initial_depth;        // Starting depth (break when } would go below this)
  uint64_t struct_at_depth; // Bitset: bit i set = depth i is a struct/union/enum scope
} TokenWalker;

// Result of goto skip analysis — both defer and decl checked in a single walk
typedef struct
{
  Token *skipped_defer;
  Token *skipped_decl;
} GotoSkipResult;

// Declarator parsing result
typedef struct
{
  Token *end;       // First token after declarator
  Token *var_name;  // The variable name token
  bool is_pointer;  // Has pointer modifier
  bool is_array;    // Is array type
  bool is_vla;      // Has variable-length array dimension
  bool is_func_ptr; // Is function pointer
  bool has_paren;   // Has parenthesized declarator
  bool has_init;    // Has initializer (=)
  bool is_const;    // Has const qualifier on declarator (e.g. * const)
} DeclResult;

#define struct_body_contains_vla(brace) scan_for_vla(brace, "{", "}")
#define typedef_contains_vla(tok) scan_for_vla(tok, NULL, ";")

extern char **environ;
static char **cached_clean_env = NULL;

static HashMap system_includes;    // Tracks unique system headers to emit
static char **system_include_list; // Ordered list of includes
static int system_include_capacity = 0;

static ControlFlow ctrl = {0};
static LabelTable label_table;

static int *stmt_expr_levels = NULL; // ctx->defer_depth when stmt expr started (dynamic)
static int stmt_expr_capacity = 0;   // capacity of stmt_expr_levels array

// Token emission - user-space buffered output for minimal syscall overhead
static FILE *out_fp;
static Token *last_emitted = NULL;
static int cached_file_idx = -1;
static File *cached_file = NULL;

static char out_buf[OUT_BUF_SIZE];
static int out_buf_pos = 0;

static TypedefTable typedef_table;

static DeferScope *defer_stack = NULL;
static int defer_stack_capacity = 0;

// Forward declarations (only for functions used before their definition)
static DeclResult parse_declarator(Token *tok, bool emit);
static bool is_type_keyword(Token *tok);
static void typedef_pop_scope(int scope_depth);
static TypeSpecResult parse_type_specifier(Token *tok);
static Token *emit_expr_to_semicolon(Token *tok);
static Token *emit_orelse_action(Token *tok, Token *var_name, bool has_const, Token *stop_comma);
static Token *try_zero_init_decl(Token *tok);

static inline void control_flow_reset(void) { ctrl = (ControlFlow){0}; }
static void walker_init(TokenWalker *w, Token *start, int initial_depth)
{
  *w = (TokenWalker){.tok = start, .initial_depth = initial_depth, .depth = initial_depth};
}

static void reset_transpiler_state(void)
{
  ctx->defer_depth = 0;
  ctx->struct_depth = 0;
  ctx->conditional_block_depth = 0;
  ctx->generic_paren_depth = 0;
  ctx->stmt_expr_count = 0;
  ctx->last_line_no = 0;
  ctx->ret_counter = 0;
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
}

PRISM_API PrismFeatures prism_defaults(void)
{
  return (PrismFeatures){
      .defer = true,
      .zeroinit = true,
      .line_directives = true,
      .flatten_headers = true,
      .orelse = true};
}

static uint32_t features_to_bits(PrismFeatures f)
{
  return (f.defer ? F_DEFER : 0) | (f.zeroinit ? F_ZEROINIT : 0) | (f.line_directives ? F_LINE_DIR : 0) | (f.warn_safety ? F_WARN_SAFETY : 0) | (f.flatten_headers ? F_FLATTEN : 0) | (f.orelse ? F_ORELSE : 0);
}

// respects $TMPDIR environment variable - Returns path with trailing slash (or empty string on Windows)
static const char *get_tmp_dir(void)
{
  static char tmp_buf[PATH_MAX];
#ifdef _WIN32
  const char *tmpdir = getenv("TEMP");
  if (!tmpdir || !tmpdir[0])
    tmpdir = getenv("TMP");
  static const char *fallback = ".\\";
#else
  const char *tmpdir = getenv("TMPDIR");
  static const char *fallback = "/tmp/";
#endif
  if (tmpdir && tmpdir[0])
  {
    size_t len = strlen(tmpdir);
    if (len < PATH_MAX - 2)
    {
      strcpy(tmp_buf, tmpdir);
      char last = tmp_buf[len - 1];
      if (last != '/' && last != '\\')
      {
        tmp_buf[len] = '/';
        tmp_buf[len + 1] = '\0';
      }
      return tmp_buf;
    }
  }
  return fallback;
}

// Fast single-char match for balanced scanners: direct char compare with rare digraph fallback
static inline bool match_ch(Token *tok, char c)
{
  if (tok->len == 1)
    return tok->shortcut == (uint8_t)c;
  if (__builtin_expect(tok->flags & TF_IS_DIGRAPH, 0))
    return _equal_1_digraph(tok, c);
  return false;
}

// Check if token is identifier-like (TK_IDENT or TK_KEYWORD like 'raw'/'defer')
static inline bool is_identifier_like(Token *tok)
{
  return tok->kind <= TK_KEYWORD; // TK_IDENT=0, TK_KEYWORD=1
}

// Skip tokens to the next semicolon at depth 0, respecting balanced delimiters
static Token *skip_to_semicolon(Token *tok)
{
  int depth = 0;
  while (tok->kind != TK_EOF)
  {
    if (tok->flags & TF_OPEN)
      depth++;
    else if (tok->flags & TF_CLOSE)
      depth--;
    else if (depth == 0 && equal(tok, ";"))
      return tok;
    tok = tok->next;
  }
  return tok;
}

// Skip a balanced pair of delimiters (parens, brackets, braces).
// tok must point to the opening delimiter. Returns the token after the closing one.
static Token *skip_balanced(Token *tok, char open, char close)
{
  int depth = 1;
  tok = tok->next; // skip opening
  while (tok->kind != TK_EOF && depth > 0)
  {
    if (match_ch(tok, open))
      depth++;
    else if (match_ch(tok, close))
      depth--;
    tok = tok->next;
  }
  return tok;
}

// Skip GNU/MSVC-style attributes: __attribute__((...)), __declspec(...)
static Token *skip_gnu_attributes(Token *tok)
{
  while (tok && (tok->tag & TT_ATTR))
  {
    tok = tok->next;
    if (tok && equal(tok, "("))
      tok = skip_balanced(tok, '(', ')');
  }
  return tok;
}

// Skip a single C23 [[ ... ]] attribute. Assumes tok is at first '['.
static Token *skip_c23_attr(Token *tok)
{
  if (!tok->next || !tok->next->next)
    return tok->next ? tok->next : tok;
  tok = tok->next->next; // skip [[
  int depth = 1;
  while (tok && tok->kind != TK_EOF && depth > 0)
  {
    if (match_ch(tok, '['))
      depth++;
    else if (match_ch(tok, ']'))
      depth--;
    tok = tok->next;
  }
  if (tok && match_ch(tok, ']'))
    tok = tok->next;
  return tok;
}

// Skip all attributes (GNU-style and C23-style [[...]])
static Token *skip_all_attributes(Token *tok)
{
  while (tok && tok->kind != TK_EOF)
  {
    if (tok->tag & TT_ATTR)
    {
      tok = skip_gnu_attributes(tok);
      continue;
    }
    if (is_c23_attr(tok))
    {
      tok = skip_c23_attr(tok);
      continue;
    }
    break;
  }
  return tok;
}

static void out_flush(void)
{
  if (out_buf_pos > 0)
  {
    fwrite(out_buf, 1, out_buf_pos, out_fp);
    out_buf_pos = 0;
  }
}

static inline void out_char(char c)
{
  if (__builtin_expect(out_buf_pos >= OUT_BUF_SIZE, 0))
    out_flush();
  out_buf[out_buf_pos++] = c;
}

static inline void out_str(const char *s, int len)
{
  if (__builtin_expect(len <= 0, 0))
    return;
  if (__builtin_expect((size_t)out_buf_pos + (size_t)len > OUT_BUF_SIZE, 0))
    out_flush();
  if (__builtin_expect(len > OUT_BUF_SIZE, 0))
  {
    fwrite(s, 1, len, out_fp);
    return;
  }
  memcpy(out_buf + out_buf_pos, s, len);
  out_buf_pos += len;
}

#define OUT_LIT(s) out_str(s, sizeof(s) - 1)

static void out_init(FILE *fp)
{
  out_fp = fp;
  out_buf_pos = 0;
}
static void out_close(void)
{
  if (out_fp)
  {
    out_flush();
    fclose(out_fp);
    out_fp = NULL;
  }
}

static void out_uint(unsigned long long v)
{
  char buf[24], *p = buf + sizeof(buf);
  do
  {
    *--p = '0' + v % 10;
  } while (v /= 10);
  out_str(p, buf + sizeof(buf) - p);
}

static void out_line(int line_no, const char *file)
{
  // GCC linemarker format: # N "file" (compatible with -fpreprocessed)
  OUT_LIT("# ");
  out_uint(line_no);
  OUT_LIT(" \"");
  out_str(file, strlen(file));
  OUT_LIT("\"\n");
}

// Headers that must not be deduplicated (C standard requires re-includability)
static bool is_reincludable_header(const char *name)
{
  // C11 §7.2: <assert.h> behavior depends on NDEBUG at point of inclusion
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  return strcmp(base, "assert.h") == 0;
}

// Collect system headers by detecting actual #include entries (not macro expansions)
static void collect_system_includes(void)
{
  for (int i = 0; i < ctx->input_file_count; i++)
  {
    File *f = ctx->input_files[i];
    if (!f->is_system || !f->is_include_entry || !f->name)
      continue;
    char *inc = strdup(f->name);
    if (!inc)
      continue;
    if (!is_reincludable_header(inc) && hashmap_get(&system_includes, inc, strlen(inc)))
    {
      free(inc);
      continue;
    }
    hashmap_put(&system_includes, inc, strlen(inc), (void *)1);
    ENSURE_ARRAY_CAP(system_include_list, ctx->system_include_count + 1, system_include_capacity, 32, char *);
    system_include_list[ctx->system_include_count++] = inc;
  }
}

// Emit diagnostic pragmas to suppress warnings from system headers.
static void emit_system_header_diag_push(void)
{
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

static void emit_system_header_diag_pop(void)
{
  OUT_LIT("#pragma GCC diagnostic pop\n");
}

// Emit collected #include directives with necessary feature test macros
static void emit_system_includes(void)
{
  if (ctx->system_include_count == 0)
    return;

  // Emit user-specified defines (take priority over built-in feature test macros)
  for (int i = 0; i < ctx->extra_define_count; i++)
  {
    const char *def = ctx->extra_defines[i];
    const char *eq = strchr(def, '=');
    OUT_LIT("#ifndef ");
    if (eq)
    {
      out_str(def, eq - def);
      OUT_LIT("\n#define ");
      out_str(def, eq - def);
      OUT_LIT(" ");
      out_str(eq + 1, strlen(eq + 1));
    }
    else
    {
      out_str(def, strlen(def));
      OUT_LIT("\n#define ");
      out_str(def, strlen(def));
    }
    OUT_LIT("\n#endif\n");
  }

  // Emit built-in feature test macros (guarded to not override user defines)
  OUT_LIT("#ifndef _POSIX_C_SOURCE\n#define _POSIX_C_SOURCE 200809L\n#endif\n"
          "#ifndef _GNU_SOURCE\n#define _GNU_SOURCE\n#endif\n\n");

  emit_system_header_diag_push();

  for (int i = 0; i < ctx->system_include_count; i++)
  {
    OUT_LIT("#include \"");
    out_str(system_include_list[i], strlen(system_include_list[i]));
    OUT_LIT("\"\n");
  }

  emit_system_header_diag_pop();

  if (ctx->system_include_count > 0)
    out_char('\n');
}

// Reset system include tracking
static void system_includes_reset(void)
{
  // Clear hashmap first to avoid dangling key pointers
  // (keys are the same strdup'd pointers stored in system_include_list)
  hashmap_clear(&system_includes);
  if (system_include_list)
  {
    for (int i = 0; i < ctx->system_include_count; i++)
      free(system_include_list[i]);
    free(system_include_list);
  }
  system_include_list = NULL;
  ctx->system_include_count = 0;
  system_include_capacity = 0;
}

static void end_statement_after_semicolon(void)
{
  ctx->at_stmt_start = true;
  ctrl.for_init = false; // Semicolon ends init clause
  if (ctrl.pending && ctrl.paren_depth == 0)
  {
    // Pop phantom scopes for braceless control bodies ending via break/continue/return/goto.
    // For-init variables (e.g., "for (int T = 0; ...)  break;") are registered at
    // defer_depth + 1 but never get a matching '}' pop. Without this, the shadow
    // persists and corrupts subsequent typedef lookups (ghost shadow bug).
    typedef_pop_scope(ctx->defer_depth + 1);
    control_flow_reset();
  }
}

static void defer_push_scope(bool consume_flags)
{
  int old_cap = defer_stack_capacity;
  ENSURE_ARRAY_CAP(defer_stack, ctx->defer_depth + 1, defer_stack_capacity, INITIAL_ARRAY_CAP, DeferScope);
  for (int i = old_cap; i < defer_stack_capacity; i++)
    defer_stack[i] = (DeferScope){0};
  DeferScope *s = &defer_stack[ctx->defer_depth];
  s->count = 0;
  s->had_control_exit = false;
  s->seen_case_label = false;
  s->is_struct = false;
  if (consume_flags)
  {
    s->is_loop = ctrl.next_scope & NS_LOOP;
    s->is_switch = ctrl.next_scope & NS_SWITCH;
    s->is_conditional = ctrl.next_scope & NS_CONDITIONAL;
    if (ctrl.next_scope & NS_CONDITIONAL)
      ctx->conditional_block_depth++;
    ctrl.next_scope = 0;
  }
  else
    s->is_loop = s->is_switch = s->is_conditional = false;
  ctx->defer_depth++;
}

static void defer_pop_scope(void)
{
  if (ctx->defer_depth > 0)
  {
    ctx->defer_depth--;
    if (defer_stack[ctx->defer_depth].is_conditional)
      ctx->conditional_block_depth--;
  }
}

static void defer_add(Token *defer_keyword, Token *start, Token *end)
{
  if (ctx->defer_depth <= 0)
    error_tok(start, "defer outside of any scope");
  DeferScope *scope = &defer_stack[ctx->defer_depth - 1];
  ENSURE_ARRAY_CAP(scope->entries, scope->count + 1, scope->capacity, INITIAL_ARRAY_CAP, DeferEntry);
  scope->entries[scope->count++] = (DeferEntry){start, end, defer_keyword};
  scope->had_control_exit = false;
}

// Find innermost switch scope index, or -1 if not in a switch
static int find_switch_scope(void)
{
  for (int d = ctx->defer_depth - 1; d >= 0; d--)
    if (defer_stack[d].is_switch)
      return d;
  return -1;
}

// Mark that control flow exited (break/return/goto) in the innermost switch scope.
// Only mark if the exit is unconditional (not inside an if/while/for block).
static void mark_switch_control_exit(void)
{
  if (ctrl.pending || ctx->conditional_block_depth > 0)
    return;
  int sd = find_switch_scope();
  if (sd >= 0)
    defer_stack[sd].had_control_exit = true;
}

// Lookup tables for punctuator merge detection (replaces 20+ branch chain)
// merges_with_eq[c]=1 means 'c=' would form a compound token (+=, ==, !=, etc.)
static const uint8_t merges_with_eq[256] = {
    ['='] = 1,
    ['!'] = 1,
    ['<'] = 1,
    ['>'] = 1,
    ['+'] = 1,
    ['-'] = 1,
    ['*'] = 1,
    ['/'] = 1,
    ['%'] = 1,
    ['&'] = 1,
    ['|'] = 1,
    ['^'] = 1,
};
// merges_with_self[c]=1 means 'cc' forms a different token (++, --, <<, >>, etc.)
static const uint8_t merges_with_self[256] = {
    ['+'] = 1,
    ['-'] = 1,
    ['<'] = 1,
    ['>'] = 1,
    ['&'] = 1,
    ['|'] = 1,
    ['#'] = 1,
};

// Check if a space is needed between two tokens
static bool needs_space(Token *prev, Token *tok)
{
  if (!prev || tok_at_bol(tok))
    return false;
  if (tok_has_space(tok))
    return true;
  if ((is_identifier_like(prev) || prev->kind == TK_NUM) &&
      (is_identifier_like(tok) || tok->kind == TK_NUM))
    return true;
  if (prev->kind != TK_PUNCT || tok->kind != TK_PUNCT)
    return false;
  // Two adjacent punctuators that would merge into a different token
  uint8_t a = (prev->len == 1) ? prev->shortcut : (uint8_t)prev->loc[prev->len - 1];
  uint8_t b = tok->shortcut;
  if (b == '=')
    return merges_with_eq[a];
  return (a == b && merges_with_self[a]) ||
         (a == '-' && b == '>') || (a == '/' && b == '*') || (a == '*' && b == '/');
}

// Check if 'tok' is inside a parenthesized context (e.g., __attribute__((cleanup(defer)))).
// Prevents 'defer' from being treated as a keyword when used as an identifier in attributes.
static bool is_inside_attribute(Token *tok)
{
  if (!last_emitted || (!equal(last_emitted, "(") && !equal(last_emitted, ",")))
    return false;
  int depth = 0;
  for (Token *t = tok; t && t->kind != TK_EOF && !equal(t, ";") && !equal(t, "{"); t = t->next)
    if (equal(t, "("))
      depth++;
    else if (equal(t, ")") && --depth < 0)
      return true;
  return false;
}

// Cold path: emit C23 float suffix or digraph translation (rare, <1% of tokens)
static bool __attribute__((noinline)) emit_tok_special(Token *tok)
{
  if ((tok->flags & TF_IS_FLOAT) && tok->kind == TK_NUM)
  {
    const char *replacement;
    int suffix_len = get_extended_float_suffix(tok->loc, tok->len, &replacement);
    if (suffix_len > 0)
    {
      out_str(tok->loc, tok->len - suffix_len);
      if (replacement)
        out_str(replacement, strlen(replacement));
      return true;
    }
  }
  if (tok->flags & TF_IS_DIGRAPH)
  {
    const char *equiv = digraph_equiv(tok);
    if (equiv)
    {
      out_str(equiv, strlen(equiv));
      return true;
    }
  }
  return false;
}

// Emit a single token with appropriate spacing
static void emit_tok(Token *tok)
{
  // Get file info — cache to avoid repeated indexing for consecutive tokens
  File *f;
  if (__builtin_expect(tok->file_idx == cached_file_idx, 1))
    f = cached_file;
  else
  {
    f = tok_file(tok);
    cached_file_idx = tok->file_idx;
    cached_file = f;
  }

  // Skip system header include content when not flattening
  if (__builtin_expect(!FEAT(F_FLATTEN) && f->is_system && f->is_include_entry, 0))
    return;

  // Check if we need a #line directive BEFORE emitting the token
  bool need_line = false;
  char *tok_fname = NULL;
  int line_no = 0;

  if (FEAT(F_LINE_DIR))
  {
    line_no = tok->line_no;
    tok_fname = f->name; // always interned — pointer compare is safe
    need_line = (ctx->last_filename != tok_fname) ||
                (f->is_system != ctx->last_system_header) ||
                (line_no != ctx->last_line_no && line_no != ctx->last_line_no + 1);
  }

  // Spacing: BOL gets newline, otherwise check for #line or token-merge space
  if (tok_at_bol(tok) || need_line)
    out_char('\n');
  else if ((tok->flags & TF_HAS_SPACE) || needs_space(last_emitted, tok))
    out_char(' ');

  if (need_line)
  {
    out_line(line_no, tok_fname);
    ctx->last_line_no = line_no;
    ctx->last_filename = tok_fname;
    ctx->last_system_header = f->is_system;
  }
  else if (line_no > ctx->last_line_no)
    ctx->last_line_no = line_no;

  // Handle preprocessor directives (e.g., #pragma) - emit verbatim
  if (__builtin_expect(tok->kind == TK_PREP_DIR, 0))
  {
    if (!tok_at_bol(tok))
      out_char('\n');
    out_str(tok->loc, tok->len);
    last_emitted = tok;
    return;
  }

  // Handle rare special cases: C23 float suffixes, digraphs
  if (__builtin_expect(tok->flags & (TF_IS_FLOAT | TF_IS_DIGRAPH), 0) && emit_tok_special(tok))
  {
    last_emitted = tok;
    return;
  }

  out_str(tok->loc, tok->len);
  last_emitted = tok;
}

// Emit tokens from start up to (but not including) end
static void emit_range(Token *start, Token *end)
{
  for (Token *t = start; t && t != end && t->kind != TK_EOF; t = t->next)
    emit_tok(t);
}

// Emit a deferred token range with feature processing (zero-init, raw, orelse).
// Unlike emit_range, this processes tokens through the transpilation pipeline
// so that Prism features work correctly inside defer blocks.
static void emit_deferred_range(Token *start, Token *end)
{
  bool saved_stmt_start = ctx->at_stmt_start;
  ControlFlow saved_ctrl = ctrl;
  ctrl = (ControlFlow){0};

  // Determine initial stmt_start based on whether body is braced
  ctx->at_stmt_start = true;

  for (Token *t = start; t && t != end && t->kind != TK_EOF;)
  {
    // Zero-init / raw / orelse at statement start
    if (ctx->at_stmt_start && FEAT(F_ZEROINIT))
    {
      Token *next = try_zero_init_decl(t);
      if (next)
      {
        t = next;
        ctx->at_stmt_start = true;
        continue;
      }
    }
    ctx->at_stmt_start = false;

    // Track structural tokens for statement boundaries
    if (t->tag & TT_STRUCTURAL)
    {
      if (match_ch(t, '{') || match_ch(t, '}'))
      {
        emit_tok(t);
        t = t->next;
        ctx->at_stmt_start = true;
        continue;
      }
      char c = t->loc[0];
      if (c == ';' || c == ':')
      {
        emit_tok(t);
        t = t->next;
        ctx->at_stmt_start = true;
        continue;
      }
    }

    emit_tok(t);
    t = t->next;
  }

  ctx->at_stmt_start = saved_stmt_start;
  ctrl = saved_ctrl;
}

static void emit_defers_ex(DeferEmitMode mode, int stop_depth)
{
  if (ctx->defer_depth <= 0)
    return;

  for (int d = ctx->defer_depth - 1; d >= 0; d--)
  {
    if (mode == DEFER_SCOPE && d < ctx->defer_depth - 1)
      break;
    if (mode == DEFER_TO_DEPTH && d < stop_depth)
      break;

    DeferScope *scope = &defer_stack[d];
    for (int i = scope->count - 1; i >= 0; i--)
    {
      out_char(' ');
      emit_deferred_range(scope->entries[i].stmt, scope->entries[i].end);
      out_char(';');
    }

    if (mode == DEFER_BREAK && (scope->is_loop || scope->is_switch))
      break;
    if (mode == DEFER_CONTINUE && scope->is_loop)
      break;
  }
}

// Check if defers exist in scopes from current depth down to stop_depth (or boundary).
// mode: DEFER_ALL=any active, DEFER_BREAK=to loop/switch, DEFER_CONTINUE=to loop, DEFER_TO_DEPTH=to depth
static bool has_defers_for(DeferEmitMode mode, int stop_depth)
{
  for (int d = ctx->defer_depth - 1; d >= 0; d--)
  {
    if (mode == DEFER_TO_DEPTH && d < stop_depth)
      break;
    if (defer_stack[d].count > 0)
      return true;
    if (mode == DEFER_BREAK && (defer_stack[d].is_loop || defer_stack[d].is_switch))
      return false; // Hit boundary without finding defers
    if (mode == DEFER_CONTINUE && defer_stack[d].is_loop)
      return false;
  }
  return false;
}

static void label_table_add(char *name, int name_len, int scope_depth)
{
  ENSURE_ARRAY_CAP(label_table.labels, label_table.count + 1, label_table.capacity, INITIAL_ARRAY_CAP, LabelInfo);
  LabelInfo *info = &label_table.labels[label_table.count++];
  info->name = name;
  info->name_len = name_len;
  info->scope_depth = scope_depth;
  // Store scope_depth + 1 so 0 (NULL) means "not found"
  hashmap_put(&label_table.name_map, name, name_len, (void *)(intptr_t)(scope_depth + 1));
}

static int label_table_lookup(char *name, int name_len)
{
  void *val = hashmap_get(&label_table.name_map, name, name_len);
  return val ? (int)(intptr_t)val - 1 : -1;
}

// Typedef tracking for zero-init
static void typedef_table_reset(void)
{
  free(typedef_table.entries);
  typedef_table.entries = NULL;
  typedef_table.count = 0;
  typedef_table.capacity = 0;
  typedef_table.bloom = 0;
  hashmap_clear(&typedef_table.name_map);
}

// Helper to get current index for a name from the hash map (-1 if not found)
// Bloom filter hash: constant-time from length + first/last byte → bit position (0-63)
static inline uint64_t typedef_bloom_bit(char *name, int len)
{
  unsigned h = (unsigned)len ^ ((unsigned char)name[0] * 7) ^ ((unsigned char)name[len - 1] * 31);
  return 1ULL << (h & 63);
}

static int typedef_get_index(char *name, int len)
{
  // Bloom filter fast-reject: if the bit isn't set, this name was never added
  if (!(typedef_table.bloom & typedef_bloom_bit(name, len)))
    return -1;
  void *val = hashmap_get(&typedef_table.name_map, name, len);
  return val ? (int)(intptr_t)val - 1 : -1;
}

// Helper to update hash map with new index for a name
static void typedef_set_index(char *name, int len, int index)
{
  // Store index+1 so that 0 (NULL) means "not found"
  hashmap_put(&typedef_table.name_map, name, len, (void *)(intptr_t)(index + 1));
}

static void typedef_add_entry(char *name, int len, int scope_depth, TypedefKind kind, bool is_vla,
                              bool is_void)
{
  // Skip duplicate typedef re-definitions at the same scope (valid C11 §6.7/3).
  // Prevents table bloat in unity builds with repeated typedef declarations.
  if (kind == TDK_TYPEDEF)
  {
    int existing = typedef_get_index(name, len);
    if (existing >= 0)
    {
      TypedefEntry *prev = &typedef_table.entries[existing];
      if (prev->scope_depth == scope_depth && !prev->is_shadow && !prev->is_enum_const)
        return; // Benign re-definition — already registered
    }
  }

  ENSURE_ARRAY_CAP(typedef_table.entries, typedef_table.count + 1, typedef_table.capacity, INITIAL_ARRAY_CAP, TypedefEntry);
  typedef_table.bloom |= typedef_bloom_bit(name, len);
  int new_index = typedef_table.count++;
  TypedefEntry *e = &typedef_table.entries[new_index];
  e->name = name;
  e->len = len;
  e->scope_depth = scope_depth;
  e->is_vla = (kind == TDK_TYPEDEF || kind == TDK_VLA_VAR) ? is_vla : false;
  e->is_void = (kind == TDK_TYPEDEF) ? is_void : false;
  e->is_shadow = (kind == TDK_SHADOW || kind == TDK_ENUM_CONST); // Enum consts also shadow typedefs
  e->is_enum_const = (kind == TDK_ENUM_CONST);
  e->prev_index = typedef_get_index(name, len);
  typedef_set_index(name, len, new_index);
}

// Called when exiting a scope - removes typedefs defined at or above given depth.
// Uses >= instead of == for resilience: if a deeper scope's entries weren't cleaned up
// (e.g., missed phantom pop, macro edge case), they get swept up here rather than
// permanently blocking the stack. In normal operation, entries are monotonically
// non-decreasing in depth, so >= behaves identically to ==.
static void typedef_pop_scope(int scope_depth)
{
  while (typedef_table.count > 0 && typedef_table.entries[typedef_table.count - 1].scope_depth >= scope_depth)
  {
    TypedefEntry *e = &typedef_table.entries[typedef_table.count - 1];
    // Restore hash map to point to previous entry (or remove if none)
    if (e->prev_index >= 0)
      typedef_set_index(e->name, e->len, e->prev_index);
    else
      hashmap_delete(&typedef_table.name_map, e->name, e->len);
    typedef_table.count--;
  }
}

// Parse enum body and register constants as shadows for any matching typedefs.
// Enum constants are visible in the enclosing scope, so they shadow typedefs.
// Also register all enum constants for VLA array size detection.
// tok should point to the opening '{' of the enum body.
// scope_depth is the scope where the enum constants become visible.
static void parse_enum_constants(Token *tok, int scope_depth)
{
  if (!tok || !equal(tok, "{"))
    return;
  tok = tok->next; // Skip '{'

  while (tok && tok->kind != TK_EOF && !equal(tok, "}"))
  {
    // Each enum constant is: IDENTIFIER or IDENTIFIER = expr
    if (tok->kind == TK_IDENT)
    {
      // Register enum constant - it shadows any typedef with the same name
      // Use typedef_add_enum_const which sets both is_shadow and is_enum_const
      // (is_shadow so is_known_typedef returns false, is_enum_const so
      // is_known_enum_const returns true for array size detection)
      typedef_add_enum_const(tok->loc, tok->len, scope_depth);
      tok = tok->next;

      // Skip = expr if present
      if (tok && equal(tok, "="))
      {
        tok = tok->next;
        // Skip expression until ',' or '}'
        int depth = 0;
        while (tok && tok->kind != TK_EOF)
        {
          if (tok->flags & TF_OPEN)
            depth++;
          else if (tok->flags & TF_CLOSE)
          {
            if (depth > 0)
              depth--;
            else if (match_ch(tok, '}'))
              break;
          }
          else if (depth == 0 && equal(tok, ","))
            break;
          tok = tok->next;
        }
      }

      // Skip comma
      if (tok && equal(tok, ","))
        tok = tok->next;
    }
    else
    {
      tok = tok->next;
    }
  }
}

// Helper: look up a typedef entry by token, or NULL if not found/not identifier
static TypedefEntry *typedef_lookup(Token *tok)
{
  if (!is_identifier_like(tok))
    return NULL;
  int idx = typedef_get_index(tok->loc, tok->len);
  return idx >= 0 ? &typedef_table.entries[idx] : NULL;
}

// Typedef query flags (single lookup, check multiple properties)
enum
{
  TDF_TYPEDEF = 1,
  TDF_VLA = 2,
  TDF_VOID = 4,
  TDF_ENUM_CONST = 8
};

static inline int typedef_flags(Token *tok)
{
  TypedefEntry *e = typedef_lookup(tok);
  if (!e)
    return 0;
  if (e->is_enum_const)
    return TDF_ENUM_CONST;
  if (e->is_shadow)
    return 0;
  return TDF_TYPEDEF | (e->is_vla ? TDF_VLA : 0) | (e->is_void ? TDF_VOID : 0);
}
#define is_known_typedef(tok) (typedef_flags(tok) & TDF_TYPEDEF)
#define is_vla_typedef(tok) (typedef_flags(tok) & TDF_VLA)
#define is_void_typedef(tok) (typedef_flags(tok) & TDF_VOID)
#define is_known_enum_const(tok) (typedef_flags(tok) & TDF_ENUM_CONST)

// Heuristic: matches system typedefs not seen during transpilation
// *_t (size_t, time_t) and __* (glibc internals) but not __func()
static inline bool is_typedef_heuristic(Token *tok)
{
  if (tok->kind != TK_IDENT)
    return false;
  if (tok->len >= 3 && tok->loc[tok->len - 2] == '_' && tok->loc[tok->len - 1] == 't')
    return true;
  if (tok->len >= 2 && tok->loc[0] == '_' && tok->loc[1] == '_')
    return !(tok->next && equal(tok->next, "("));
  return false;
}

// Check if token is a known typedef or looks like a system typedef (e.g., size_t, __rlim_t)
static bool is_typedef_like(Token *tok)
{
  if (!is_identifier_like(tok))
    return false;
  if (is_known_typedef(tok))
    return true;
  return is_typedef_heuristic(tok);
}

// Given a struct/union/enum keyword, find its opening brace if it has a body.
// Handles: "struct {", "struct name {", "struct __attribute__((packed)) name {"
// Returns the '{' token, or NULL if no body (e.g., "struct name;" or "struct name var;")
static Token *find_struct_body_brace(Token *tok)
{
  Token *t = tok->next;
  while (t && (t->kind == TK_IDENT || (t->tag & (TT_ATTR | TT_QUALIFIER))))
  {
    if (t->tag & (TT_ATTR | TT_QUALIFIER))
    {
      t = t->next;
      if (t && equal(t, "("))
        t = skip_balanced(t, '(', ')');
    }
    else
    {
      t = t->next;
    }
  }
  return (t && equal(t, "{")) ? t : NULL;
}

// Advance to next meaningful token. Skips struct/union/enum body openings
// (fast-forwards from keyword to past '{') and _Generic(...) expressions.
// Tracks brace depth on { and } — these ARE returned to the caller.
// Returns false when region ends (EOF or '}' at initial_depth).
static bool walker_next(TokenWalker *w)
{
  for (;;)
  {
    if (!w->tok || w->tok->kind == TK_EOF)
      return false;

    // Skip struct/union/enum body openings (keyword → past '{')
    if (w->tok->tag & TT_SUE)
    {
      Token *brace = find_struct_body_brace(w->tok);
      if (brace)
      {
        while (w->tok != brace)
        {
          w->prev = w->tok;
          w->tok = w->tok->next;
        }
        w->struct_depth++;
        w->depth++;
        if ((unsigned)w->depth < 64)
          w->struct_at_depth |= (1ULL << w->depth);
        // Depths >= 64 can't be tracked in the bitmask; struct_depth is
        // still incremented and will be decremented on '}' via the
        // fallback below.
        w->prev = w->tok;
        w->tok = w->tok->next;
        continue;
      }
    }

    // Skip _Generic(...)
    if (w->tok->tag & TT_GENERIC)
    {
      w->prev = w->tok;
      w->tok = w->tok->next;
      if (w->tok && equal(w->tok, "("))
        w->tok = skip_balanced(w->tok, '(', ')');
      w->prev = NULL;
      continue;
    }

    // Track braces — update depth but still return token to caller
    // Fast path: single len==1 check covers both { and }, avoiding two equal() calls
    if (w->tok->len == 1)
    {
      char c = w->tok->loc[0];
      if (c == '{')
        w->depth++;
      else if (c == '}')
      {
        if (w->depth <= w->initial_depth)
          return false; // End of region
        if ((unsigned)w->depth < 64)
        {
          if (w->struct_at_depth & (1ULL << w->depth))
          {
            w->struct_at_depth &= ~(1ULL << w->depth);
            if (w->struct_depth > 0)
              w->struct_depth--;
          }
        }
        else if (w->struct_depth > 0)
        {
          // Depth >= 64: bitmask can't track, conservatively decrement
          w->struct_depth--;
        }
        w->depth--;
      }
    }
    else if (__builtin_expect(w->tok->flags & TF_IS_DIGRAPH, 0))
    {
      // Handle <% and %> digraphs (extremely rare)
      if (_equal_1(w->tok, '{'))
        w->depth++;
      else if (_equal_1(w->tok, '}'))
      {
        if (w->depth <= w->initial_depth)
          return false;
        if ((unsigned)w->depth < 64)
        {
          if (w->struct_at_depth & (1ULL << w->depth))
          {
            w->struct_at_depth &= ~(1ULL << w->depth);
            if (w->struct_depth > 0)
              w->struct_depth--;
          }
        }
        else if (w->struct_depth > 0)
        {
          // Depth >= 64: bitmask can't track, conservatively decrement
          w->struct_depth--;
        }
        w->depth--;
      }
    }

    return true;
  }
}

static inline void walker_advance(TokenWalker *w)
{
  w->prev = w->tok;
  w->tok = w->tok->next;
}

// Check if current token is a goto label (identifier followed by ':').
// Filters out: '::', ternary '?:', case/default, bitfields in struct bodies.
static Token *walker_check_label(TokenWalker *w)
{
  if (!is_identifier_like(w->tok))
    return NULL;
  Token *t = skip_gnu_attributes(w->tok->next);
  if (!t || !equal(t, ":"))
    return NULL;
  if (t->next && equal(t->next, ":"))
    return NULL; // :: scope resolution
  if (w->prev && equal(w->prev, "?"))
    return NULL; // ternary
  if (w->prev && (w->prev->tag & (TT_CASE | TT_DEFAULT)))
    return NULL; // switch case
  if (w->struct_depth > 0)
    return NULL; // bitfield
  return w->tok;
}

// ============================================================================

// Scan a function body for labels and record their scope depths
// Also detects setjmp/longjmp/pthread_exit/vfork and inline asm usage
// tok should point to the opening '{' of the function body
//
// Limitation: detection is token-based and only catches direct calls to
// setjmp/longjmp/_setjmp/_longjmp/sigsetjmp/siglongjmp/pthread_exit/vfork.
// Indirect calls through function pointers or wrapper functions (e.g.
// "my_longjmp(buf, 1)") are NOT detected. In such cases, defer cleanup
// code may execute incorrectly if a longjmp crosses a defer scope.
//
// Performance: this is a hot function (~77K tokens scanned per run).
// Uses a two-phase approach: quick pre-scan for goto/setjmp/vfork/asm,
// then full label scan only if the function contains goto statements.
static void scan_labels_in_function(Token *tok)
{
  label_table.count = 0;
  hashmap_clear(&label_table.name_map);
  ctx->current_func_has_setjmp = false;
  ctx->current_func_has_asm = false;
  ctx->current_func_has_vfork = false;
  if (!tok || !equal(tok, "{"))
    return;

  // Phase 1: Quick scan for goto + setjmp/vfork/asm
  // Uses TT_STRUCTURAL tag for brace depth tracking (avoids len==1 check on every token)
  bool needs_labels = false;
  {
    int d = 1;
    for (Token *t = tok->next; t && t->kind != TK_EOF; t = t->next)
    {
      uint32_t tg = t->tag;
      if (__builtin_expect(tg & (TT_SPECIAL_FN | TT_ASM | TT_GOTO), 0))
      {
        if (tg & TT_SPECIAL_FN)
        {
          if (equal(t, "vfork"))
            ctx->current_func_has_vfork = true;
          else
            ctx->current_func_has_setjmp = true;
        }
        if (tg & TT_ASM)
          ctx->current_func_has_asm = true;
        if (tg & TT_GOTO)
          needs_labels = true;
      }
      if (__builtin_expect(tg & TT_STRUCTURAL, 0))
      {
        if (match_ch(t, '{'))
          d++;
        else if (match_ch(t, '}') && --d <= 0)
          break;
      }
    }
  }

  // Phase 2: Full label scan — only needed when function contains goto
  if (!needs_labels)
    return;

  TokenWalker w;
  walker_init(&w, tok->next, 1);
  while (walker_next(&w))
  {
    Token *label = walker_check_label(&w);
    if (label)
      label_table_add(label->loc, label->len, w.depth);
    walker_advance(&w);
  }
}

// Check if a forward goto would skip over defer statements or variable declarations.
// Returns both results in one walk. Either field NULL means safe for that check.
//
// Key rule: if we find the label BEFORE exiting the scope containing the item, it's invalid.
// A goto that jumps OVER an entire block is fine; jumping INTO a block past a defer/decl is not.
static GotoSkipResult goto_skips_check(Token *goto_tok, char *label_name, int label_len,
                                       bool check_defer, bool check_decl)
{
  GotoSkipResult r = {NULL, NULL};
  if (check_decl && !FEAT(F_ZEROINIT | F_DEFER))
    check_decl = false;
  if (!check_defer && !check_decl)
    return r;

  Token *start = goto_tok->next->next; // skip 'goto' and label name
  if (start && equal(start, ";"))
    start = start->next;

  Token *active_defer = NULL, *active_decl = NULL;
  int defer_depth = -1, decl_depth = -1;
  bool is_stmt_start = true;
  bool is_in_for_init = false;

  TokenWalker w;
  walker_init(&w, start, 0);

  while (walker_next(&w))
  {
    // For-loop init detection (for decl check)
    if (check_decl && (w.tok->tag & TT_LOOP) && equal(w.tok, "for"))
    {
      walker_advance(&w);
      if (w.tok && equal(w.tok, "("))
      {
        is_in_for_init = true;
        walker_advance(&w);
        is_stmt_start = true;
        continue;
      }
      is_stmt_start = false;
      continue;
    }

    // Consolidated structural token handling: {, }, ;
    if (w.tok->len == 1)
    {
      char c = w.tok->loc[0];
      if (c == '{' || c == '}' || c == ';')
      {
        if (c == '}')
        {
          if (active_defer && w.depth < defer_depth)
          {
            active_defer = NULL;
            defer_depth = -1;
          }
          if (active_decl && w.depth < decl_depth)
          {
            active_decl = NULL;
            decl_depth = -1;
          }
        }
        if (c == ';' && is_in_for_init)
          is_in_for_init = false;
        is_stmt_start = true;
        walker_advance(&w);
        continue;
      }
    }

    // Track defers
    if (check_defer)
    {
      bool is_var = w.prev && (is_type_keyword(w.prev) || equal(w.prev, "*") ||
                               (w.prev->tag & TT_QUALIFIER) ||
                               equal(w.prev, "__restrict") || equal(w.prev, ","));
      if ((w.tok->tag & TT_DEFER) &&
          !equal(w.tok->next, ":") &&
          !(w.prev && (w.prev->tag & TT_MEMBER)) && !is_var &&
          !(w.tok->next && (w.tok->next->tag & TT_ASSIGN)))
      {
        if (!active_defer || w.depth <= defer_depth)
        {
          active_defer = w.tok;
          defer_depth = w.depth;
        }
      }
    }

    // Track declarations
    if (check_decl && (is_stmt_start || is_in_for_init) && w.struct_depth == 0)
    {
      Token *decl_start = w.tok;
      Token *t = w.tok;
      bool has_raw = false;
      if (equal(t, "raw") && !is_known_typedef(t))
      {
        has_raw = true;
        t = t->next;
      }
      if (!equal(t, "extern") && !equal(t, "typedef"))
      {
        TypeSpecResult type = parse_type_specifier(t);
        if (type.saw_type)
        {
          t = type.end;
          while (t && (equal(t, "*") || (t->tag & TT_QUALIFIER) ||
                       equal(t, "__restrict") || equal(t, "__restrict__")))
            t = t->next;
          if (t && t->kind == TK_IDENT && t->next && !equal(t->next, "("))
          {
            if (!has_raw && (!active_decl || w.depth <= decl_depth))
            {
              active_decl = decl_start;
              decl_depth = w.depth;
            }
          }
        }
      }
    }

    // Label detection
    if (w.tok->kind == TK_IDENT && w.tok->len == label_len &&
        !memcmp(w.tok->loc, label_name, label_len))
    {
      Token *label = walker_check_label(&w);
      if (label)
      {
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

// Check if tokens between after_paren and brace form K&R parameter declarations.
// K&R pattern: type ident [, ident]* ; ... repeated until {
// Example: int func(a, b) int a; int b; {
static bool is_knr_params(Token *after_paren, Token *brace)
{
  Token *t = after_paren;
  bool saw_any = false;
  while (t && t != brace && t->kind != TK_EOF)
  {
    // Each K&R declaration starts with a type specifier
    TypeSpecResult type = parse_type_specifier(t);
    if (!type.saw_type)
      return false;
    t = type.end;
    // Skip declarators: pointers, identifiers, arrays, commas
    bool saw_ident = false;
    while (t && t != brace && !equal(t, ";"))
    {
      if (equal(t, "*") || (t->tag & TT_QUALIFIER) ||
          equal(t, "__restrict") || equal(t, "__restrict__"))
        t = t->next;
      else if (t->kind == TK_IDENT)
      {
        saw_ident = true;
        t = t->next;
      }
      else if (equal(t, "["))
        t = skip_balanced(t, '[', ']');
      else if (equal(t, ","))
        t = t->next;
      else if (equal(t, "("))
        t = skip_balanced(t, '(', ')');
      else
        return false;
    }
    if (!saw_ident || !equal(t, ";"))
      return false;
    t = t->next;
    saw_any = true;
  }
  return saw_any && t == brace;
}

// Skip function specifiers/qualifiers/attributes that can precede a type
static Token *skip_func_specifiers(Token *tok)
{
  while (tok && (tok->tag & (TT_QUALIFIER | TT_SKIP_DECL | TT_ATTR | TT_INLINE)))
  {
    if (tok->tag & TT_ATTR)
      tok = skip_gnu_attributes(tok);
    else
      tok = tok->next;
  }
  return tok;
}

// Check if token starts a void function declaration
// Handles: void func(, static void func(, __attribute__((...)) void func(, etc.
static bool is_void_function_decl(Token *tok)
{
  tok = skip_func_specifiers(tok);

  // Must be at 'void', a typedef alias for void, or typeof(void)/typeof_unqual(void)
  if (!tok)
    return false;
  if (tok->tag & TT_TYPEOF)
  {
    // Check for typeof(void) or typeof_unqual(void)
    Token *t = tok->next;
    if (t && equal(t, "(") && t->next && equal(t->next, "void") &&
        t->next->next && equal(t->next->next, ")"))
      tok = t->next->next->next;
    else
      return false;
  }
  else if (!equal(tok, "void") && !is_void_typedef(tok))
    return false;
  else
    tok = tok->next;

  // void* is not a void-returning function
  if (tok && equal(tok, "*"))
    return false;

  // Skip attributes and qualifiers after void
  tok = skip_func_specifiers(tok);

  // Should be at function name followed by (
  return tok && tok->kind == TK_IDENT && tok->next && equal(tok->next, "(");
}

// Check if an array dimension (from '[' to matching ']') contains a VLA expression.
// Simple rule: any identifier that isn't a type/enum constant, or appears after
// member access (./->) is a runtime variable → VLA. sizeof/alignof/offsetof
// arguments are skipped (always constant) except sizeof(VLA_Typedef).
static bool array_size_is_vla(Token *open_bracket)
{
  Token *tok = open_bracket->next;

  while (tok && tok->kind != TK_EOF && !equal(tok, "]"))
  {
    if (equal(tok, "[")) // Skip nested brackets
    {
      tok = skip_balanced(tok, '[', ']');
      continue;
    }

    // sizeof/alignof — skip argument, but check for VLA typedef and VLA inner types
    if (equal(tok, "sizeof") || equal(tok, "_Alignof") || equal(tok, "alignof"))
    {
      bool is_sizeof = equal(tok, "sizeof");
      tok = tok->next;
      if (tok && equal(tok, "("))
      {
        if (is_sizeof)
        {
          if (tok->next && is_vla_typedef(tok->next))
            return true;
          Token *inner = tok->next;
          Token *end = skip_balanced(tok, '(', ')');
          for (; inner && inner != end && inner->kind != TK_EOF; inner = inner->next)
            if (equal(inner, "[") && array_size_is_vla(inner))
              return true;
          tok = end;
        }
        else
          tok = skip_balanced(tok, '(', ')');
      }
      continue;
    }

    // offsetof — GCC treats as VLA in array bounds
    if (equal(tok, "offsetof") || equal(tok, "__builtin_offsetof"))
      return true;

    // Member access operators — non-ICE (pointer/struct dereference)
    if (equal(tok, "->") || equal(tok, "."))
      return true;

    // Non-constant identifier → VLA
    if (tok->kind == TK_IDENT && !is_known_enum_const(tok) && !is_type_keyword(tok))
      return true;

    tok = tok->next;
  }
  return false;
}

// Scan a delimited region for VLA array dimensions.
static bool scan_for_vla(Token *tok, const char *open, const char *close)
{
  if (open && (!tok || !equal(tok, open)))
    return false;
  if (open)
    tok = tok->next;
  int depth = 1;
  while (tok && tok->kind != TK_EOF)
  {
    if (open && equal(tok, open))
      depth++;
    else if (equal(tok, close))
    {
      if (open)
      {
        if (--depth <= 0)
          break;
      }
      else
        break;
    }
    else if (!open && (equal(tok, "(") || equal(tok, "{")))
      depth++;
    else if (!open && (equal(tok, ")") || equal(tok, "}")))
    {
      if (depth > 0)
        depth--;
    }
    else if (equal(tok, "[") && depth >= 1 && array_size_is_vla(tok))
      return true;
    tok = tok->next;
  }
  return false;
}

static void parse_typedef_declaration(Token *tok, int scope_depth)
{
  Token *typedef_start = tok;
  tok = tok->next; // Skip 'typedef'
  Token *type_start = tok;
  tok = parse_type_specifier(tok).end; // Skip the base type

  bool is_vla = typedef_contains_vla(typedef_start);

  // Check if the base type is void (or a typedef alias for void).
  // Scan tokens from type_start to tok for 'void' keyword or known void typedef.
  // Excludes: struct/union/enum, typeof, _Atomic(...), function pointers.
  bool base_is_void = false;
  for (Token *t = type_start; t && t != tok; t = t->next)
  {
    if (equal(t, "void"))
    {
      base_is_void = true;
      break;
    }
    // Check for chained typedefs: typedef Void MyVoid;
    if (t->kind == TK_IDENT && is_known_typedef(t))
    {
      int idx = typedef_get_index(t->loc, t->len);
      if (idx >= 0 && typedef_table.entries[idx].is_void)
      {
        base_is_void = true;
        break;
      }
    }
  }

  // Parse declarator(s) until semicolon
  while (tok && !equal(tok, ";") && tok->kind != TK_EOF)
  {
    DeclResult decl = parse_declarator(tok, false);
    if (decl.var_name)
    {
      // A typedef is void only if the base type is void AND the declarator
      // is plain (no pointer, array, or function pointer indirection)
      bool is_void = base_is_void && !decl.is_pointer && !decl.is_array && !decl.is_func_ptr;
      typedef_add(decl.var_name->loc, decl.var_name->len, scope_depth, is_vla, is_void);
    }
    tok = decl.end ? decl.end : tok->next;

    // Skip to comma or semicolon (past any initializer)
    while (tok && !equal(tok, ",") && !equal(tok, ";") && tok->kind != TK_EOF)
    {
      if (equal(tok, "("))
        tok = skip_balanced(tok, '(', ')');
      else if (equal(tok, "["))
        tok = skip_balanced(tok, '[', ']');
      else
        tok = tok->next;
    }

    if (tok && equal(tok, ","))
      tok = tok->next;
  }
}

// Zero-init helpers

static bool is_type_keyword(Token *tok)
{
  if (tok->tag & TT_TYPE)
    return true;
  // Only identifiers and prism keywords (raw, defer, orelse) can be user-defined typedefs
  if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD)
    return false;
  return is_typedef_like(tok);
}

// Check if token can be used as a variable name in a declarator.
// Identifiers and prism keywords (raw, defer) which are only special at declaration start.
static inline bool is_valid_varname(Token *tok)
{
  return tok->kind == TK_IDENT || equal(tok, "raw") || equal(tok, "defer");
}

// ============================================================================
// Zero-init declaration parsing helpers
// ============================================================================

// Skip _Pragma(...) operator sequences (C99 6.10.9)
// _Pragma is equivalent to #pragma but can appear in macro expansions
// Returns the token after all _Pragma(...) sequences
static Token *skip_pragma_operators(Token *tok)
{
  // Fast path: _Pragma is 7 chars and extremely rare in preprocessed output
  while (tok && tok->len == 7 && equal(tok, "_Pragma") && tok->next && equal(tok->next, "("))
  {
    tok = tok->next;                    // skip _Pragma
    tok = skip_balanced(tok, '(', ')'); // skip (...)
  }
  return tok;
}

// Parse type specifier: qualifiers, type keywords, struct/union/enum, typeof, _Atomic, etc.
// Returns info about the type and position after it
static TypeSpecResult parse_type_specifier(Token *tok)
{
  TypeSpecResult r = {tok, false, false, false, false, false, false, false, false, false};

  bool is_type = false;
  while ((tok->tag & TT_QUALIFIER) || (is_type = is_type_keyword(tok)) || is_c23_attr(tok))
  {
    uint32_t tag = tok->tag;

    // Track qualifiers via tag bits
    if (tag & TT_QUALIFIER)
    {
      if (tag & TT_VOLATILE)
        r.has_volatile = true;
      if (tag & TT_REGISTER)
        r.has_register = true;
      if (tag & TT_CONST)
        r.has_const = true;
      if (tag & TT_TYPE) // _Atomic is TT_QUALIFIER | TT_TYPE
        r.has_atomic = true;
    }

    if (is_c23_attr(tok))
    {
      tok = skip_c23_attr(tok);
      r.end = tok;
      is_type = false;
      continue;
    }

    if (is_type)
      r.saw_type = true;
    is_type = false;

    // _Atomic(type) specifier form (must precede generic attr/alignas handling)
    if ((tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE) &&
        tok->next && equal(tok->next, "("))
    {
      r.saw_type = true;
      r.has_atomic = true;
      tok = tok->next;
      Token *inner_start = tok->next;
      tok = skip_balanced(tok, '(', ')');
      if (inner_start && (inner_start->tag & TT_SUE))
        r.is_struct = true;
      if (inner_start && inner_start->kind == TK_IDENT && is_known_typedef(inner_start))
        r.is_typedef = true;
      r.end = tok;
      continue;
    }

    // struct/union/enum
    if (tag & TT_SUE)
    {
      r.is_struct = true;
      r.saw_type = true;
      tok = tok->next;
      while (tok && (tok->tag & (TT_ATTR | TT_QUALIFIER)))
      {
        tok = tok->next;
        if (tok && equal(tok, "("))
          tok = skip_balanced(tok, '(', ')');
      }
      if (tok && tok->kind == TK_IDENT)
        tok = tok->next;
      if (tok && equal(tok, "{"))
      {
        if (struct_body_contains_vla(tok))
          r.is_vla = true;
        tok = skip_balanced(tok, '{', '}');
      }
      r.end = tok;
      continue;
    }

    // typeof/typeof_unqual/__typeof__
    if (tag & TT_TYPEOF)
    {
      bool is_unqual = equal(tok, "typeof_unqual");
      r.saw_type = true;
      r.has_typeof = true;
      tok = tok->next;
      if (tok && equal(tok, "("))
      {
        Token *end = skip_balanced(tok, '(', ')');
        if (!is_unqual)
          for (Token *t = tok->next; t && t != end; t = t->next)
            if (t->tag & TT_VOLATILE)
            {
              r.has_volatile = true;
              break;
            }
        tok = end;
      }
      r.end = tok;
      continue;
    }

    // _BitInt(N), _Alignas(N)/alignas(N), __attribute__((...)) — skip token + balanced paren
    if (tag & (TT_BITINT | TT_ATTR | TT_ALIGNAS))
    {
      if (tag & TT_BITINT)
        r.saw_type = true;
      tok = tok->next;
      if (tok && equal(tok, "("))
        tok = skip_balanced(tok, '(', ')');
      r.end = tok;
      continue;
    }

    // User-defined typedef or system typedef (pthread_mutex_t, etc.)
    int tflags = typedef_flags(tok);
    if ((tflags & TDF_TYPEDEF) || is_typedef_heuristic(tok))
    {
      r.is_typedef = true;
      if (tflags & TDF_VLA)
        r.is_vla = true;
      Token *peek = tok->next;
      while (peek && (peek->tag & TT_QUALIFIER))
        peek = peek->next;
      if (peek && peek->kind == TK_IDENT)
      {
        Token *after = peek->next;
        if (after && (equal(after, ";") || equal(after, "[") ||
                      equal(after, ",") || equal(after, "=")))
        {
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

// Get canonical delimiter char (digraph-aware)
static inline char tok_open_ch(Token *t)
{
  if (t->len == 1)
    return t->loc[0];
  if (__builtin_expect(t->flags & TF_IS_DIGRAPH, 0))
  {
    const char *e = digraph_equiv(t);
    if (e)
      return e[0];
  }
  return t->loc[0];
}
static inline char close_for(char c) { return c == '(' ? ')' : c == '[' ? ']'
                                                                        : '}'; }

// Walk a balanced token group. When emit=true, emits all tokens; when false, skips.
// Works for (), [], {}, and __attribute__((...)). Returns position after closing delimiter.
static Token *walk_balanced(Token *tok, bool emit)
{
  char open_c = tok_open_ch(tok);
  char close_c = close_for(open_c);
  if (emit)
    emit_tok(tok);
  tok = tok->next;
  int depth = 1;
  while (tok && tok->kind != TK_EOF && depth > 0)
  {
    if (match_ch(tok, open_c))
      depth++;
    else if (match_ch(tok, close_c))
      depth--;
    if (emit)
      emit_tok(tok);
    tok = tok->next;
  }
  return tok;
}

static inline void decl_emit(Token *t, bool emit)
{
  if (emit)
    emit_tok(t);
}

static inline Token *decl_balanced(Token *t, bool emit)
{
  return emit ? walk_balanced(t, true) : skip_balanced(t, tok_open_ch(t), close_for(tok_open_ch(t)));
}

static inline Token *decl_attr(Token *t, bool emit)
{
  if (emit)
    emit_tok(t);
  t = t->next;
  if (t && equal(t, "("))
    t = emit ? walk_balanced(t, true) : skip_balanced(t, '(', ')');
  return t;
}

static inline Token *decl_array_dims(Token *t, bool emit, bool *vla)
{
  while (equal(t, "["))
  {
    if (array_size_is_vla(t))
      *vla = true;
    t = decl_balanced(t, emit);
  }
  return t;
}

// Unified declarator parser. When emit=true, emits tokens while parsing.
// When emit=false, only advances without output (replaces old skip_declarator).
static DeclResult parse_declarator(Token *tok, bool emit)
{
  DeclResult r = {tok, NULL, false, false, false, false, false, false, false};

  // Pointer modifiers and qualifiers (with depth limit for safety)
  int ptr_depth = 0;
  while (equal(tok, "*") || (tok->tag & TT_QUALIFIER))
  {
    if (equal(tok, "*"))
    {
      r.is_pointer = true;
      r.is_const = false; // Reset: const after a new '*' applies to this pointer level
      if (++ptr_depth > 256)
      {
        r.end = NULL;
        return r;
      }
    }
    else if (r.is_pointer && (tok->tag & TT_CONST))
      r.is_const = true;
    if (tok->tag & TT_ATTR)
    {
      tok = decl_attr(tok, emit);
      continue;
    }
    decl_emit(tok, emit);
    tok = tok->next;
  }

  // Parenthesized declarator: (*name), (*name)[N], (*(*name)(args))[N]
  int nested_paren = 0;
  if (equal(tok, "("))
  {
    Token *peek = tok->next;
    // Skip __attribute__((...)) before '*' in parenthesized declarators
    // e.g. int (__attribute__((unused)) *p);
    while (peek && (peek->tag & TT_ATTR))
    {
      peek = peek->next;
      if (peek && equal(peek, "("))
        peek = skip_balanced(peek, '(', ')');
    }
    if (!equal(peek, "*") && !equal(peek, "("))
    {
      r.end = NULL;
      return r;
    }

    decl_emit(tok, emit);
    tok = tok->next;
    nested_paren = 1;
    r.is_pointer = true;
    r.has_paren = true;

    // Handle nesting: (*(*(*name)...
    while (equal(tok, "*") || (tok->tag & TT_QUALIFIER) || equal(tok, "("))
    {
      if (equal(tok, "*"))
        r.is_pointer = true;
      else if (equal(tok, "("))
        nested_paren++;
      if (tok->tag & TT_ATTR)
      {
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

  // Skip __attribute__ after variable name
  while (tok && tok->kind != TK_EOF && (tok->tag & TT_ATTR))
    tok = decl_attr(tok, emit);

  // Array dims inside parens: (*name[N])
  if (r.has_paren && equal(tok, "["))
  {
    r.is_array = true;
    tok = decl_array_dims(tok, emit, &r.is_vla);
  }

  // Close nested parens, handling function args or array dims at each level
  while (r.has_paren && nested_paren > 0)
  {
    while (equal(tok, "(") || equal(tok, "["))
    {
      if (equal(tok, "("))
        tok = decl_balanced(tok, emit);
      else
      {
        r.is_array = true;
        tok = decl_array_dims(tok, emit, &r.is_vla);
      }
    }
    if (!equal(tok, ")"))
    {
      r.end = NULL;
      return r;
    }
    decl_emit(tok, emit);
    tok = tok->next;
    nested_paren--;
  }

  // Function pointer: (*name)(args)
  if (equal(tok, "("))
  {
    if (!r.has_paren)
    {
      r.end = NULL; // Regular function declaration
      return r;
    }
    r.is_func_ptr = true;
    tok = decl_balanced(tok, emit);
  }

  // Array dimensions outside parens
  if (equal(tok, "["))
  {
    r.is_array = true;
    tok = decl_array_dims(tok, emit, &r.is_vla);
  }

  // Skip __attribute__ before initializer or end of declarator
  while (tok && tok->kind != TK_EOF && (tok->tag & TT_ATTR))
    tok = decl_attr(tok, emit);

  r.has_init = equal(tok, "=");
  r.end = tok;

  return r;
}

// Quick pre-check: is this a variable declaration (not a function decl or stmt expr)?
// Uses parse_declarator(skip mode) for the heavy lifting, avoiding a separate scan.
static bool is_var_declaration(Token *type_end)
{
  DeclResult decl = parse_declarator(type_end, false);
  if (!decl.var_name || !decl.end)
    return false;

  // Statement expression initializer: type name = ({...})
  // For single-declarator stmt-expr decls, the main loop handles scope/defer processing.
  // For multi-declarator decls (e.g. int a = ({...}), b;), we still need to process
  // subsequent declarators for zero-init, so skip past the stmt-expr and check for ',' or ';'.
  if (equal(decl.end, "="))
  {
    Token *after_eq = decl.end->next;
    if (after_eq && equal(after_eq, "(") && after_eq->next && equal(after_eq->next, "{"))
    {
      // Skip past the balanced (...) of the statement expression
      Token *after_stmt_expr = skip_balanced(after_eq, '(', ')');
      // Skip trailing attributes
      while (after_stmt_expr && after_stmt_expr->kind != TK_EOF && (after_stmt_expr->tag & TT_ATTR))
        after_stmt_expr = skip_gnu_attributes(after_stmt_expr);
      return equal(after_stmt_expr, ",");
    }
    return true; // Regular initializer (not stmt expr)
  }

  return equal(decl.end, ",") || equal(decl.end, ";");
}

// Check if token after 'raw' indicates 'raw' is being used as an identifier, not the keyword
// Returns true if 'raw' is followed by something that makes it look like a declaration
static bool is_raw_declaration_context(Token *after_raw)
{
  return after_raw && (is_type_keyword(after_raw) || is_known_typedef(after_raw) || equal(after_raw, "*") || (after_raw->tag & (TT_QUALIFIER | TT_SUE)));
}

// Emit tokens from start through semicolon
// Handle 'raw' after storage class: "static raw int x;"
static Token *handle_storage_raw(Token *storage_tok)
{
  Token *p = storage_tok->next;
  while (p && (equal(p, "_Pragma") || (p->tag & TT_ATTR)))
  {
    p = p->next;
    if (p && equal(p, "("))
      p = skip_balanced(p, '(', ')');
  }
  if (!equal(p, "raw") || is_known_typedef(p))
    return NULL;

  // Emit storage class and any attributes, skip 'raw'
  Token *t = storage_tok;
  while (t != p)
  {
    emit_tok(t);
    t = t->next;
  }
  // Skip 'raw', emit rest through semicolon
  Token *t2 = p->next;
  while (t2 && !equal(t2, ";") && t2->kind != TK_EOF)
    t2 = t2->next;
  if (equal(t2, ";"))
    t2 = t2->next;
  emit_range(p->next, t2);
  return t2;
}

// Emit memset-based zeroing for typeof/atomic/VLA variables
static void emit_typeof_memsets(Token **vars, int count, bool has_volatile)
{
  for (int i = 0; i < count; i++)
  {
    if (has_volatile)
    {
      OUT_LIT(" { volatile char *_p = (volatile char *)&");
      out_str(vars[i]->loc, vars[i]->len);
      OUT_LIT("; for (size_t _i = 0; _i < sizeof(");
      out_str(vars[i]->loc, vars[i]->len);
      OUT_LIT("); _i++) _p[_i] = 0; }");
    }
    else
    {
      OUT_LIT(" __builtin_memset(&");
      out_str(vars[i]->loc, vars[i]->len);
      OUT_LIT(", 0, sizeof(");
      out_str(vars[i]->loc, vars[i]->len);
      OUT_LIT("));");
    }
  }
}

// Register typedef shadows and VLA variables for a declarator
static inline void register_decl_shadows(Token *var_name, bool effective_vla)
{
  int depth = ctx->defer_depth + (ctrl.for_init ? 1 : 0);
  if (is_known_typedef(var_name))
    typedef_add_shadow(var_name->loc, var_name->len, depth);
  if (effective_vla && var_name)
    typedef_add_vla_var(var_name->loc, var_name->len, depth);
}

// Process all declarators in a declaration and emit with zero-init
// Returns token after declaration, or NULL on failure
static Token *process_declarators(Token *tok, TypeSpecResult *type, bool is_raw, Token *type_start)
{
  Token **typeof_vars = NULL;
  int typeof_var_count = 0;
  int typeof_var_cap = 0;

  while (tok && tok->kind != TK_EOF)
  {
    DeclResult decl = parse_declarator(tok, true);
    if (!decl.end || !decl.var_name)
    {
      free(typeof_vars);
      return NULL;
    }

    tok = decl.end;

    // Determine effective VLA status (excluding typeof - handled specially)
    bool effective_vla = (decl.is_vla && !decl.has_paren) || (type->is_vla && !decl.is_pointer);

    // For typeof declarations, use memset instead of = 0 or = {0}
    // Also for atomic aggregates: Clang doesn't support _Atomic aggregate init syntax
    // Also for VLAs: can't use = {0}, so memset at runtime (sizeof evaluates to runtime size)
    // But NOT for register variables (can't take address) or volatile (needs volatile semantics)
    bool is_aggregate = decl.is_array || ((type->is_struct || type->is_typedef) && !decl.is_pointer);
    bool needs_memset = !decl.has_init && !is_raw && !decl.is_pointer && !type->has_register &&
                        (type->has_typeof || (type->has_atomic && is_aggregate) || effective_vla);

    // Add zero initializer if needed (for non-memset types)
    if (!decl.has_init && !effective_vla && !is_raw && !needs_memset)
    {
      if (is_aggregate)
        OUT_LIT(" = {0}");
      else
        OUT_LIT(" = 0");
    }

    // Track typeof variables for memset emission (dynamic, no hard limit)
    if (needs_memset)
    {
      ENSURE_ARRAY_CAP(typeof_vars, typeof_var_count + 1, typeof_var_cap, 8, Token *);
      typeof_vars[typeof_var_count++] = decl.var_name;
    }

    // Emit initializer if present
    if (decl.has_init)
    {
      int depth = 0;
      bool hit_orelse = false;
      while (tok->kind != TK_EOF)
      {
        if (tok->flags & TF_OPEN)
          depth++;
        else if (tok->flags & TF_CLOSE)
          depth--;
        else if (depth == 0 && (equal(tok, ",") || equal(tok, ";")))
          break;
        // Detect 'orelse' at depth 0 in initializer
        if (FEAT(F_ORELSE) && depth == 0 && (tok->tag & TT_ORELSE) && !is_known_typedef(tok) &&
            !(last_emitted && (last_emitted->tag & TT_MEMBER)))
        {
          hit_orelse = true;
          break;
        }
        emit_tok(tok);
        tok = tok->next;
      }

      if (hit_orelse)
      {
        if (ctrl.for_init)
          error_tok(tok, "orelse cannot be used in for-loop initializers");
        out_char(';');

        emit_typeof_memsets(typeof_vars, typeof_var_count, type->has_volatile);
        typeof_var_count = 0; // reset for remaining declarators

        register_decl_shadows(decl.var_name, effective_vla);

        tok = tok->next; // skip 'orelse'

        // Error on missing action: "int x = val orelse;" is malformed
        if (equal(tok, ";"))
          error_tok(tok, "expected statement after 'orelse'");

        // Warn if orelse is applied to a non-pointer array (address is never NULL)
        if (decl.is_array && !decl.is_pointer)
          warn_tok(decl.var_name, "orelse on array variable '%.*s' will never trigger (array address is never NULL)",
                   decl.var_name->len, decl.var_name->loc);

        // Find boundary comma for multi-declarator orelse
        Token *stop_comma = NULL;
        {
          int sd = 0;
          for (Token *t = tok; t->kind != TK_EOF; t = t->next)
          {
            if (t->flags & TF_OPEN)
              sd++;
            else if (t->flags & TF_CLOSE)
              sd--;
            else if (sd == 0 && equal(t, ","))
            {
              stop_comma = t;
              break;
            }
            else if (sd == 0 && equal(t, ";"))
              break;
          }
        }

        tok = emit_orelse_action(tok, decl.var_name, type->has_const || decl.is_const, stop_comma);

        // Continue processing remaining declarators after comma
        if (stop_comma && equal(tok, ","))
        {
          tok = tok->next; // skip comma
          // Re-emit the type specifier, skipping struct/union/enum bodies
          // to avoid redefinition errors (e.g. enum constant redeclaration)
          for (Token *t = type_start; t != type->end; t = t->next)
          {
            if (equal(t, "{"))
            {
              t = skip_balanced(t, '{', '}');
              if (t == type->end)
                break;
            }
            emit_tok(t);
          }
          continue;
        }
        free(typeof_vars);
        return tok;
      }
    }

    register_decl_shadows(decl.var_name, effective_vla);

    if (equal(tok, ";"))
    {
      emit_tok(tok);
      emit_typeof_memsets(typeof_vars, typeof_var_count, type->has_volatile);
      free(typeof_vars);
      return tok->next;
    }
    else if (equal(tok, ","))
    {
      emit_tok(tok);
      tok = tok->next;
    }
    else
    {
      free(typeof_vars);
      return NULL;
    }
  }

  free(typeof_vars);
  return NULL;
}

// Try to handle a declaration with zero-init
// Supports multi-declarators like: int a, b, *c, d[10];
// Returns the token after the declaration if handled, NULL otherwise
//
// SAFETY: If we see a type but fail to parse the declarator, we emit a warning
// to alert the user that zero-init may have been skipped.
static Token *try_zero_init_decl(Token *tok)
{
  if (!FEAT(F_ZEROINIT) || ctx->defer_depth <= 0 || ctx->struct_depth > 0)
    return NULL;

  if (tok->kind >= TK_STR) // Fast reject: strings, numbers, prep directives, EOF can't start a declaration
    return NULL;

  // Check for "switch skip hole" - declarations directly in switch body
  // without braces can have their initialization skipped by case jumps.
  // This catches both: before first case label, and between case labels.
  bool in_switch_scope_unbraced = ctx->defer_depth > 0 && defer_stack[ctx->defer_depth - 1].is_switch;

  Token *warn_loc = tok;
  Token *pragma_start = tok;

  // Skip leading attributes and pragmas
  while (is_c23_attr(tok))
    tok = skip_c23_attr(tok);
  if (tok->len == 7) // Fast path: only call skip_pragma_operators if token could be _Pragma
    tok = skip_pragma_operators(tok);
  Token *start = tok;

  // Check for 'raw' keyword
  bool is_raw = false;
  if (equal(tok, "raw") && !is_known_typedef(tok))
  {
    Token *after_raw = tok->next;
    while (after_raw && equal(after_raw, "_Pragma"))
    {
      after_raw = after_raw->next;
      if (after_raw && equal(after_raw, "("))
        after_raw = skip_balanced(after_raw, '(', ')');
    }
    if (is_raw_declaration_context(after_raw))
    {
      is_raw = true;
      tok = tok->next;
      start = tok;
      pragma_start = tok;
      warn_loc = tok;
    }
  }

  // Skip pragmas after 'raw'
  if (tok->len == 7)
  { // Fast path: only check if token could be _Pragma
    Token *before = tok;
    tok = skip_pragma_operators(tok);
    if (tok != before && !is_raw)
      start = tok;
  }

  if (tok->tag & TT_SKIP_DECL) // Storage class specifiers
  {
    if (is_raw)
    {
      Token *e = start;
      while (e && !equal(e, ";") && e->kind != TK_EOF)
        e = e->next;
      if (equal(e, ";"))
        e = e->next;
      emit_range(start, e);
      return e;
    }
    if (equal(tok, "static") || equal(tok, "extern") || (tok->tag & TT_TYPEDEF))
    {
      Token *result = handle_storage_raw(tok);
      if (result)
        return result;
    }
    return NULL;
  }

  // Fast reject: if token has no declaration-start tag and isn't a typedef, skip parse_type_specifier
  if (!(tok->tag & TT_DECL_START) && !is_typedef_like(tok))
    return NULL;

  // Parse type specifier
  TypeSpecResult type = parse_type_specifier(tok);
  if (!type.saw_type)
    return NULL;

  // Validate declaration structure
  if (!is_var_declaration(type.end))
    return NULL;

  // Error if in switch scope without braces
  if (in_switch_scope_unbraced && !is_raw)
  {
    error_tok(warn_loc,
              "variable declaration directly in switch body without braces. "
              "Wrap in braces: 'case N: { int x; ... }' to ensure safe zero-initialization, "
              "or use 'raw' to suppress zero-init.");
  }

  // Emit pragmas and type
  if (pragma_start != start)
    emit_range(pragma_start, start);
  emit_range(start, type.end);

  return process_declarators(type.end, &type, is_raw, start);
}

// Emit an expression until semicolon, tracking depth for statement expressions.
// Handles zero-init for declarations inside statement expressions.
// Returns the token after the expression (the semicolon, or EOF).
static Token *emit_expr_to_semicolon(Token *tok)
{
  int depth = 0;
  int ternary_depth = 0;
  bool expr_at_stmt_start = false;
  Token *prev_tok = NULL;
  while (tok->kind != TK_EOF)
  {
    if (tok->flags & TF_OPEN)
    {
      depth++;
      if (match_ch(tok, '{'))
        expr_at_stmt_start = true;
    }
    else if (tok->flags & TF_CLOSE)
      depth--;
    else if (depth == 0 && equal(tok, ";"))
      break;
    else if (equal(tok, "?"))
      ternary_depth++;

    if (expr_at_stmt_start && FEAT(F_ZEROINIT))
    {
      Token *next = try_zero_init_decl(tok);
      if (next)
      {
        tok = next;
        expr_at_stmt_start = true;
        continue;
      }
      expr_at_stmt_start = false;
    }

    emit_tok(tok);
    prev_tok = tok;
    tok = tok->next;

    if (prev_tok && (equal(prev_tok, "{") || equal(prev_tok, ";") || equal(prev_tok, "}")))
      expr_at_stmt_start = true;
    else if (prev_tok && equal(prev_tok, ":") && ternary_depth <= 0)
      expr_at_stmt_start = true;
    else
    {
      if (prev_tok && equal(prev_tok, ":") && ternary_depth > 0)
        ternary_depth--;
      expr_at_stmt_start = false;
    }
  }
  return tok;
}

// Handle 'defer' keyword: validate context, record deferred statement.
// Returns next token after the defer statement, or NULL if tok is not a valid defer.
static Token *handle_defer_keyword(Token *tok)
{
  if (!FEAT(F_DEFER))
    return NULL;
  // Distinguish struct field, label, goto target, variable assignment, attribute usage
  if (equal(tok->next, ":") ||
      (last_emitted && (last_emitted->tag & (TT_MEMBER | TT_GOTO))) ||
      (last_emitted && (is_type_keyword(last_emitted) || (last_emitted->tag & TT_TYPEDEF))) ||
      is_known_typedef(tok) ||
      (tok->next && (tok->next->tag & TT_ASSIGN)) ||
      ctx->struct_depth > 0 ||
      is_inside_attribute(tok))
    return NULL;

  // Context validation
  if (ctrl.pending && ctrl.paren_depth > 0)
    error_tok(tok, "defer cannot appear inside control statement parentheses");
  if (ctrl.pending && ctrl.paren_depth == 0)
    error_tok(tok, "defer requires braces in control statements (braceless has no scope)");
  for (int i = 0; i < ctx->stmt_expr_count; i++)
    if (ctx->defer_depth == stmt_expr_levels[i])
      error_tok(tok, "defer cannot be at top level of statement expression; wrap in a block");
  // setjmp/longjmp safety: only direct calls are detected by scan_labels_in_function.
  // Indirect calls via function pointers or wrappers are not caught (see limitation
  // comment on scan_labels_in_function).
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

  // Validate: no bare control-flow keywords, no unbracketed multi-line spans
  int bd = 0, pd = 0, bkd = 0;
  for (Token *t = stmt_start; t != stmt_end && t->kind != TK_EOF; t = t->next)
  {
    bool at_top = (bd == 0 && pd == 0 && bkd == 0);
    if (t != stmt_start && tok_at_bol(t) && at_top &&
        !equal(t, "{") && !equal(t, "(") && !equal(t, "["))
      error_tok(defer_keyword, "defer statement spans multiple lines without ';' - add semicolon");
    if (equal(t, "{"))
      bd++;
    else if (equal(t, "}"))
      bd--;
    else if (equal(t, "("))
      pd++;
    else if (equal(t, ")"))
      pd--;
    else if (equal(t, "["))
      bkd++;
    else if (equal(t, "]"))
      bkd--;
    if (at_top && t->kind == TK_KEYWORD &&
        (t->tag & (TT_RETURN | TT_BREAK | TT_CONTINUE | TT_GOTO |
                   TT_IF | TT_LOOP | TT_SWITCH | TT_CASE | TT_DEFAULT | TT_DEFER)))
      error_tok(defer_keyword, "defer statement appears to be missing ';' (found '%.*s' keyword inside)",
                t->len, t->loc);
    // Nested defer is never valid — catch it at any depth (includes braced blocks)
    if (!at_top && (t->tag & TT_DEFER) && !is_known_typedef(t) && !equal(t->next, ":") &&
        !(t->next && (t->next->tag & TT_ASSIGN)))
      error_tok(defer_keyword, "nested defer is not supported (found 'defer' inside deferred block)");
  }

  defer_add(defer_keyword, stmt_start, stmt_end);
  tok = (stmt_end->kind != TK_EOF) ? stmt_end->next : stmt_end;
  end_statement_after_semicolon();
  return tok;
}

// Emit return statement body with optional defer cleanup.
// tok points to first token after 'return'. Returns updated tok.
static Token *emit_return_body(Token *tok)
{
  if (FEAT(F_DEFER) && has_active_defers())
  {
    if (equal(tok, ";"))
    {
      emit_all_defers();
      OUT_LIT(" return;");
      tok = tok->next;
    }
    else
    {
      bool is_void_cast = equal(tok, "(") && tok->next && equal(tok->next, "void") &&
                          tok->next->next && equal(tok->next->next, ")");
      bool is_void = ctx->current_func_returns_void || is_void_cast;
      if (!is_void)
      {
        OUT_LIT(" __auto_type _prism_ret_");
        out_uint(ctx->ret_counter);
        OUT_LIT(" = (");
      }
      else
        OUT_LIT(" (");
      tok = emit_expr_to_semicolon(tok);
      OUT_LIT(");");
      emit_all_defers();
      if (!is_void)
      {
        OUT_LIT(" return _prism_ret_");
        out_uint(ctx->ret_counter++);
      }
      else
        OUT_LIT(" return");
      out_char(';');
      if (equal(tok, ";"))
        tok = tok->next;
    }
  }
  else
  {
    OUT_LIT(" return");
    if (!equal(tok, ";"))
    {
      out_char(' ');
      tok = emit_expr_to_semicolon(tok);
    }
    out_char(';');
    if (equal(tok, ";"))
      tok = tok->next;
  }
  return tok;
}

// Emit orelse action: the fallback part after 'orelse'.
// For declaration orelse: var_name is the variable, has_const from type/decl.
// For bare expression orelse: var_name is NULL (condition already emitted).
// Calls end_statement_after_semicolon() internally for non-block cases.
static inline void orelse_open(Token *var_name)
{
  if (var_name)
  {
    OUT_LIT(" if (!");
    out_str(var_name->loc, var_name->len);
    OUT_LIT(") {");
  }
  else
    OUT_LIT(" {");
}

static Token *emit_orelse_action(Token *tok, Token *var_name, bool has_const, Token *stop_comma)
{
  // Block orelse: '{' handled by caller
  if (equal(tok, "{"))
  {
    if (var_name)
    {
      OUT_LIT(" if (!");
      out_str(var_name->loc, var_name->len);
      out_char(')');
    }
    ctx->at_stmt_start = false;
    return tok;
  }

  // Return orelse
  if (tok->tag & TT_RETURN)
  {
    mark_switch_control_exit();
    tok = tok->next;
    orelse_open(var_name);
    if (stop_comma)
    {
      // Multi-declarator context: emit return body stopping at the declarator comma
      bool has_defers = FEAT(F_DEFER) && has_active_defers();
      bool at_end = equal(tok, ";") || tok == stop_comma;
      if (has_defers)
      {
        if (at_end)
        {
          emit_all_defers();
          OUT_LIT(" return;");
        }
        else
        {
          bool is_void_cast = equal(tok, "(") && tok->next && equal(tok->next, "void") &&
                              tok->next->next && equal(tok->next->next, ")");
          bool is_void = ctx->current_func_returns_void || is_void_cast;
          if (!is_void)
          {
            OUT_LIT(" __auto_type _prism_ret_");
            out_uint(ctx->ret_counter);
            OUT_LIT(" = (");
          }
          else
            OUT_LIT(" (");
          int rd = 0;
          while (tok->kind != TK_EOF && tok != stop_comma)
          {
            if (tok->flags & TF_OPEN)
              rd++;
            else if (tok->flags & TF_CLOSE)
              rd--;
            else if (rd == 0 && equal(tok, ";"))
              break;
            emit_tok(tok);
            tok = tok->next;
          }
          OUT_LIT(");");
          emit_all_defers();
          if (!is_void)
          {
            OUT_LIT(" return _prism_ret_");
            out_uint(ctx->ret_counter++);
          }
          else
            OUT_LIT(" return");
          out_char(';');
        }
      }
      else
      {
        OUT_LIT(" return");
        if (!at_end)
        {
          out_char(' ');
          int rd = 0;
          while (tok->kind != TK_EOF && tok != stop_comma)
          {
            if (tok->flags & TF_OPEN)
              rd++;
            else if (tok->flags & TF_CLOSE)
              rd--;
            else if (rd == 0 && equal(tok, ";"))
              break;
            emit_tok(tok);
            tok = tok->next;
          }
        }
        out_char(';');
      }
      if (equal(tok, ";"))
        tok = tok->next;
    }
    else
      tok = emit_return_body(tok);
    OUT_LIT(" }");
    end_statement_after_semicolon();
    return tok;
  }

  // Break/continue orelse
  if (tok->tag & (TT_BREAK | TT_CONTINUE))
  {
    bool is_break = tok->tag & TT_BREAK;
    mark_switch_control_exit();
    orelse_open(var_name);
    if (FEAT(F_DEFER) && control_flow_has_defers(is_break))
      emit_defers(is_break ? DEFER_BREAK : DEFER_CONTINUE);
    out_char(' ');
    out_str(tok->loc, tok->len);
    OUT_LIT("; }");
    tok = tok->next;
    if (equal(tok, ";"))
      tok = tok->next;
    end_statement_after_semicolon();
    return tok;
  }

  // Goto orelse
  if (tok->tag & TT_GOTO)
  {
    mark_switch_control_exit();
    orelse_open(var_name);
    tok = tok->next;
    if (FEAT(F_DEFER) && is_identifier_like(tok))
    {
      int td = label_table_lookup(tok->loc, tok->len);
      if (td < 0)
        td = ctx->defer_depth;
      if (goto_has_defers(td))
        emit_goto_defers(td);
    }
    OUT_LIT(" goto ");
    if (is_identifier_like(tok))
    {
      out_str(tok->loc, tok->len);
      tok = tok->next;
    }
    OUT_LIT("; }");
    if (equal(tok, ";"))
      tok = tok->next;
    end_statement_after_semicolon();
    return tok;
  }

  // Fallback: assignment
  if (!var_name)
    error_tok(tok, "orelse fallback requires an assignment target (use a declaration)");
  if (has_const)
    error_tok(tok, "orelse fallback cannot reassign a const-qualified variable");
  OUT_LIT(" if (!");
  out_str(var_name->loc, var_name->len);
  OUT_LIT(") ");
  out_str(var_name->loc, var_name->len);
  OUT_LIT(" =");
  int fdepth = 0;
  while (tok->kind != TK_EOF)
  {
    if (tok->flags & TF_OPEN)
      fdepth++;
    else if (tok->flags & TF_CLOSE)
      fdepth--;
    else if (fdepth == 0 && (equal(tok, ";") || equal(tok, ",")))
      break;
    emit_tok(tok);
    tok = tok->next;
  }
  out_char(';');
  if (equal(tok, ";"))
    tok = tok->next;
  end_statement_after_semicolon();
  return tok;
}

// Handle 'return' with active defers: save expr, emit defers, then return.
// Returns next token if handled, or NULL to let normal emit proceed.
static Token *handle_return_defer(Token *tok)
{
  mark_switch_control_exit();
  if (!has_active_defers())
    return NULL;
  tok = tok->next; // skip 'return'
  OUT_LIT(" {");
  tok = emit_return_body(tok);
  OUT_LIT(" }");
  end_statement_after_semicolon();
  return tok;
}

// Handle 'break' or 'continue' with active defers.
// Returns next token if handled, or NULL.
static Token *handle_break_continue_defer(Token *tok)
{
  bool is_break = tok->tag & TT_BREAK;
  mark_switch_control_exit();
  if (!control_flow_has_defers(is_break))
    return NULL;
  OUT_LIT(" {");
  emit_defers(is_break ? DEFER_BREAK : DEFER_CONTINUE);
  out_char(' ');
  out_str(tok->loc, tok->len);
  OUT_LIT("; }");
  tok = tok->next;
  if (equal(tok, ";"))
    tok = tok->next;
  end_statement_after_semicolon();
  return tok;
}

// Report goto skipping over a variable declaration (warn or error based on FEAT(F_WARN_SAFETY))
static void report_goto_skips_decl(Token *skipped_decl, Token *label_tok)
{
  const char *msg = "goto '%.*s' would skip over this variable declaration (bypasses zero-init)";
  if (FEAT(F_WARN_SAFETY))
    warn_tok(skipped_decl, msg, label_tok->len, label_tok->loc);
  else
    error_tok(skipped_decl, msg, label_tok->len, label_tok->loc);
}

// Handle 'goto': defer cleanup + zeroinit safety checks.
// Returns next token if fully handled, or NULL to let normal emit proceed.
static Token *handle_goto_keyword(Token *tok)
{
  Token *goto_tok = tok;
  tok = tok->next;

  if (FEAT(F_DEFER))
  {
    mark_switch_control_exit();

    if (equal(tok, "*")) // Computed goto
    {
      if (has_active_defers())
        error_tok(goto_tok, "computed goto cannot be used with active defer statements");
      emit_tok(goto_tok);
      return tok; // let main loop emit '*' and rest
    }

    if (is_identifier_like(tok))
    {
      GotoSkipResult skip = goto_skips_check(goto_tok, tok->loc, tok->len, true, true);
      if (skip.skipped_defer)
        error_tok(skip.skipped_defer, "goto '%.*s' would skip over this defer statement", tok->len, tok->loc);
      if (skip.skipped_decl)
        report_goto_skips_decl(skip.skipped_decl, tok);

      int target_depth = label_table_lookup(tok->loc, tok->len);
      if (target_depth < 0)
        target_depth = ctx->defer_depth;

      if (goto_has_defers(target_depth))
      {
        OUT_LIT(" {");
        emit_goto_defers(target_depth);
        OUT_LIT(" goto");
        emit_tok(tok);
        tok = tok->next;
        if (equal(tok, ";"))
        {
          emit_tok(tok);
          tok = tok->next;
        }
        OUT_LIT(" }");
        end_statement_after_semicolon();
        return tok;
      }
    }
    // No defers — emit goto, let main loop handle the rest
    emit_tok(goto_tok);
    return tok;
  }

  // Zeroinit-only goto safety
  if (FEAT(F_ZEROINIT) && is_identifier_like(tok))
  {
    GotoSkipResult skip = goto_skips_check(goto_tok, tok->loc, tok->len, false, true);
    if (skip.skipped_decl)
      report_goto_skips_decl(skip.skipped_decl, tok);
  }
  emit_tok(goto_tok);
  return tok;
}

// Handle case/default labels: check for defer fallthrough, clear switch defers.
static void handle_case_default(Token *tok)
{
  if (!FEAT(F_DEFER))
    return;
  int sd = find_switch_scope();
  if (sd < 0)
    return;
  bool is_case = tok->tag & TT_CASE;
  bool is_default = (tok->tag & TT_DEFAULT) && ctx->generic_paren_depth == 0;
  if (is_default)
  {
    Token *t = skip_all_attributes(tok->next);
    if (!t || !equal(t, ":"))
      return; // Not a switch label
  }
  if (!is_case && !is_default)
    return;

  // Check for defer fallthrough and clear defers (single pass)
  for (int d = ctx->defer_depth - 1; d >= sd; d--)
  {
    if (defer_stack[d].count > 0 && !defer_stack[d].had_control_exit)
      error_tok(defer_stack[d].entries[0].defer_kw,
                "defer skipped by switch fallthrough at %s:%d",
                tok_file(tok)->name, tok_line_no(tok));
    defer_stack[d].count = 0;
    defer_stack[d].had_control_exit = false;
  }
  defer_stack[sd].seen_case_label = true;
}

// Handle struct/union/enum body opening: emit to '{', push scope.
// Returns next token if handled, or NULL if no body follows.
static Token *handle_sue_body(Token *tok)
{
  bool is_enum = equal(tok, "enum");
  Token *brace = find_struct_body_brace(tok);
  if (!brace)
    return NULL;

  if (is_enum)
    parse_enum_constants(brace, ctx->defer_depth);
  emit_range(tok, brace);
  emit_tok(brace); // emit '{'
  tok = brace->next;
  ctx->struct_depth++;
  defer_push_scope(false);
  defer_stack[ctx->defer_depth - 1].is_struct = true;
  ctx->at_stmt_start = true;
  return tok;
}

// Handle '{': push scope, detect statement expressions and compound literals.
// Returns next token.
static Token *handle_open_brace(Token *tok)
{
  // Compound literal inside control parens, or after condition (before body)
  if (ctrl.pending && (ctrl.paren_depth > 0 || !ctrl.parens_just_closed))
  {
    emit_tok(tok);
    ctrl.brace_depth++;
    return tok->next;
  }
  if (ctrl.pending && !(ctrl.next_scope & NS_SWITCH))
    ctrl.next_scope |= NS_CONDITIONAL;
  ctrl = (ControlFlow){.next_scope = ctrl.next_scope};

  // Detect statement expression: ({
  if (last_emitted && equal(last_emitted, "("))
  {
    ENSURE_ARRAY_CAP(stmt_expr_levels, ctx->stmt_expr_count + 1, stmt_expr_capacity, INITIAL_ARRAY_CAP, int);
    stmt_expr_levels[ctx->stmt_expr_count++] = ctx->defer_depth + 1;
  }
  emit_tok(tok);
  tok = tok->next;
  defer_push_scope(true);
  ctx->at_stmt_start = true;
  return tok;
}

// Handle '}': emit defers, pop scope, detect stmt-expr exit.
// Returns next token.
static Token *handle_close_brace(Token *tok)
{
  // Compound literal close inside control parens
  if (ctrl.pending && ctrl.paren_depth > 0 && ctrl.brace_depth > 0)
  {
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
  tok = tok->next;
  if (tok && equal(tok, ")") && ctx->stmt_expr_count > 0 &&
      stmt_expr_levels[ctx->stmt_expr_count - 1] == ctx->defer_depth + 1)
    ctx->stmt_expr_count--;
  ctx->at_stmt_start = true;
  return tok;
}

static inline void argv_builder_add(ArgvBuilder *ab, const char *arg)
{
  ENSURE_ARRAY_CAP(ab->data, ab->count + 2, ab->capacity, 64, char *);
  ab->data[ab->count] = strdup(arg);
  if (!ab->data[ab->count])
    error("out of memory");
  ab->count++;
  ab->data[ab->count] = NULL;
}

// Build a copy of 'environ' with CC and PRISM_CC removed.
// Cached: built once, reused for all child processes.
// Caller must NOT free() the returned array.
static char **build_clean_environ(void)
{
  if (cached_clean_env)
    return cached_clean_env;
  int n = 0;
  for (char **e = environ; *e; e++)
    n++;
  char **env = malloc((n + 1) * sizeof(char *));
  if (!env)
    return NULL;
  int j = 0;
  for (int i = 0; i < n; i++)
  {
    if (strncmp(environ[i], "CC=", 3) == 0 ||
        strncmp(environ[i], "PRISM_CC=", 9) == 0)
      continue;
    env[j++] = environ[i];
  }
  env[j] = NULL;
  cached_clean_env = env;
  return env;
}

static int wait_for_child(pid_t pid)
{
  int status;
  if (waitpid(pid, &status, 0) == -1)
  {
    perror("waitpid");
    return -1;
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return -1;
}

// Run a command and wait for it to complete
// Returns exit status, or -1 on error
static int run_command(char **argv)
{
#ifdef _WIN32
  intptr_t status = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
  return (int)status;
#else
  char **env = build_clean_environ();
  if (!env)
    return -1;
  pid_t pid;
  int err = posix_spawnp(&pid, argv[0], NULL, NULL, argv, env);
  if (err)
  {
    fprintf(stderr, "posix_spawnp: %s: %s\n", argv[0], strerror(err));
    return -1;
  }
  return wait_for_child(pid);
#endif
}

static void free_argv(char **argv)
{
  if (!argv)
    return;
  for (int i = 0; argv[i]; i++)
    free(argv[i]);
  free(argv);
}

// Unified temp file creation with optional suffix (e.g., ".c" → suffix_len=2)
// If source_adjacent is non-NULL, creates file next to that source file instead of in tmp dir.
// Returns 0 on success, -1 on failure. Path written to buf.
// Atomically creates a temp file from an XXXXXX template.
// On Windows, appends win_suffix (e.g. ".c", ".exe") after name generation.
// On POSIX, uses mkstemps if suffix_len > 0 (suffix is already in template).
#if defined(_WIN32)
static int atomic_mkstemp(char *buf, size_t bufsize, const char *win_suffix)
{
  for (int attempt = 0; attempt < 100; attempt++)
  {
    char try_buf[PATH_MAX];
    size_t len = strlen(buf);
    memcpy(try_buf, buf, len + 1);
    if (_mktemp_s(try_buf, len + 1) != 0)
      return -1;
    if (win_suffix)
    {
      size_t tlen = strlen(try_buf);
      size_t slen = strlen(win_suffix);
      if (tlen + slen + 1 >= sizeof(try_buf))
        return -1;
      memcpy(try_buf + tlen, win_suffix, slen + 1);
    }
    int fd;
    errno_t err = _sopen_s(&fd, try_buf, _O_CREAT | _O_EXCL | _O_WRONLY, _SH_DENYNO, _S_IREAD | _S_IWRITE);
    if (err == 0 && fd >= 0)
    {
      _close(fd);
      memcpy(buf, try_buf, strlen(try_buf) + 1);
      return 0;
    }
  }
  return -1;
}
#endif

static int make_temp_file(char *buf, size_t bufsize, const char *prefix, int suffix_len, const char *source_adjacent)
{
  int n;
  if (source_adjacent)
  {
    const char *slash = strrchr(source_adjacent, '/');
    const char *bslash = strrchr(source_adjacent, '\\');
    if (bslash && (!slash || bslash > slash))
      slash = bslash;
    if (slash)
    {
      int dir_len = (int)(slash - source_adjacent);
#ifdef _WIN32
      n = snprintf(buf, bufsize, "%.*s/.%s.XXXXXX", dir_len, source_adjacent, slash + 1);
#else
      n = snprintf(buf, bufsize, "%.*s/.%s.XXXXXX.c", dir_len, source_adjacent, slash + 1);
#endif
    }
    else
    {
#ifdef _WIN32
      n = snprintf(buf, bufsize, ".%s.XXXXXX", source_adjacent);
#else
      n = snprintf(buf, bufsize, ".%s.XXXXXX.c", source_adjacent);
#endif
    }
    suffix_len = 2;
  }
  else
    n = snprintf(buf, bufsize, "%s%s", get_tmp_dir(), prefix);
  if (n < 0 || (size_t)n >= bufsize)
    return -1;
#if defined(_WIN32)
  return atomic_mkstemp(buf, bufsize, source_adjacent ? ".c" : NULL);
#else
  int fd = suffix_len > 0 ? mkstemps(buf, suffix_len) : mkstemp(buf);
  if (fd < 0)
    return -1;
  // Keep fd open and use fdopen to avoid TOCTOU race between close+fopen.
  // We immediately close the FILE* (which also closes fd) since the caller
  // will reopen the path later via fopen/transpile. The file now exists
  // atomically with our chosen name.
  FILE *fp = fdopen(fd, "w");
  if (!fp)
  {
    close(fd);
    return -1;
  }
  fclose(fp);
  return 0;
#endif
}

// Extract filename from path (handles both / and \ separators)
static const char *path_basename(const char *path)
{
  const char *fwd = strrchr(path, '/');
  const char *bck = strrchr(path, '\\');
  if (bck && (!fwd || bck > fwd))
    fwd = bck;
  return fwd ? fwd + 1 : path;
}

static bool cc_is_msvc(const char *cc)
{
#ifndef _WIN32
  (void)cc;
  return false;
#else
  if (!cc || !*cc)
    return false;
  const char *base = path_basename(cc);
  return (_stricmp(base, "cl") == 0 || _stricmp(base, "cl.exe") == 0);
#endif
}

// Build preprocessor argv into an ArgvBuilder.
// Shared between pipe-based and file-based preprocessor paths.
static void build_pp_argv(ArgvBuilder *ab, const char *input_file)
{
  const char *cc = ctx->extra_compiler ? ctx->extra_compiler : PRISM_DEFAULT_CC;
  argv_builder_add(ab, cc);
  argv_builder_add(ab, "-E");
  argv_builder_add(ab, "-w"); // suppress warnings (linker flags passed through are harmless)

  for (int i = 0; i < ctx->extra_compiler_flags_count; i++) // compiler flags
    argv_builder_add(ab, ctx->extra_compiler_flags[i]);

  for (int i = 0; i < ctx->extra_include_count; i++) // include paths
  {
    argv_builder_add(ab, "-I");
    argv_builder_add(ab, ctx->extra_include_paths[i]);
  }

  for (int i = 0; i < ctx->extra_define_count; i++) // defines
  {
    argv_builder_add(ab, "-D");
    argv_builder_add(ab, ctx->extra_defines[i]);
  }

  // Add prism-specific macros
  argv_builder_add(ab, "-D__PRISM__=1");
  if (FEAT(F_DEFER))
    argv_builder_add(ab, "-D__PRISM_DEFER__=1");
  if (FEAT(F_ZEROINIT))
    argv_builder_add(ab, "-D__PRISM_ZEROINIT__=1");

  // Add standard feature test macros for POSIX/GNU compatibility (unless user already defined them)
#ifndef _WIN32
  {
    bool user_has_posix = false, user_has_gnu = false;
    for (int i = 0; i < ctx->extra_define_count; i++)
    {
      if (strncmp(ctx->extra_defines[i], "_POSIX_C_SOURCE", 15) == 0)
        user_has_posix = true;
      if (strncmp(ctx->extra_defines[i], "_GNU_SOURCE", 11) == 0)
        user_has_gnu = true;
    }
    for (int i = 0; i < ctx->extra_compiler_flags_count; i++)
    {
      const char *f = ctx->extra_compiler_flags[i];
      if (strncmp(f, "-D_POSIX_C_SOURCE", 17) == 0 || strncmp(f, "-U_POSIX_C_SOURCE", 17) == 0)
        user_has_posix = true;
      if (strncmp(f, "-D_GNU_SOURCE", 13) == 0 || strncmp(f, "-U_GNU_SOURCE", 13) == 0)
        user_has_gnu = true;
    }
    if (!user_has_posix)
      argv_builder_add(ab, "-D_POSIX_C_SOURCE=200809L");
    if (!user_has_gnu)
      argv_builder_add(ab, "-D_GNU_SOURCE");
  }
#endif

  for (int i = 0; i < ctx->extra_force_include_count; i++) // force-includes
  {
    argv_builder_add(ab, cc_is_msvc(ab->data[0]) ? "/FI" : "-include");
    argv_builder_add(ab, ctx->extra_force_includes[i]);
  }

  argv_builder_add(ab, input_file);
}

// Run system preprocessor (cc -E) via pipe — no temp files.
// Returns a malloc'd NUL-terminated buffer with preprocessed output, or NULL on failure.
// Caller must free() the buffer.
static char *preprocess_with_cc(const char *input_file)
{
  ArgvBuilder ab;
  argv_builder_init(&ab);
  build_pp_argv(&ab, input_file);
  char **argv = argv_builder_finish(&ab);

  // Set up pipe: child writes preprocessed output to stdout, we read from pipe
  int pipefd[2];
  if (pipe(pipefd) == -1)
  {
    perror("pipe");
    free_argv(argv);
    return NULL;
  }

  // Set up file actions: child stdout → pipe write end, stderr → /dev/null
  // Redirecting stderr prevents deadlock: if the compiler emits massive stderr
  // output (e.g., thousands of errors), the child would block writing stderr
  // while the parent blocks reading stdout, causing a deadlock.
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addclose(&fa, pipefd[0]);
  posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&fa, pipefd[1]);
  posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

  char **env = build_clean_environ();
  pid_t pid;
  int err = posix_spawnp(&pid, argv[0], &fa, NULL, argv, env);
  posix_spawn_file_actions_destroy(&fa);
  close(pipefd[1]);
  free_argv(argv);

  if (err)
  {
    fprintf(stderr, "posix_spawnp: %s\n", strerror(err));
    close(pipefd[0]);
    return NULL;
  }

  // Parent: read all preprocessed output from pipe
  size_t cap = 128 * 1024, len = 0;
  char *buf = malloc(cap);
  if (!buf)
  {
    close(pipefd[0]);
    waitpid(pid, NULL, 0);
    return NULL;
  }

  ssize_t n;
  while ((n = read(pipefd[0], buf + len, cap - len - 1)) > 0)
  {
    len += (size_t)n;
    if (len + 1 >= cap)
    {
      cap *= 2;
      char *tmp = realloc(buf, cap);
      if (!tmp)
      {
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

  int status;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
  {
    free(buf);
    return NULL;
  }

  return buf;
}

// ── Shared helpers for transpile_tokens (called from both fast and slow paths) ──

static inline void track_ctrl_paren_open(void)
{
  ctrl.paren_depth++;
  ctrl.parens_just_closed = false;
  if (ctrl.await_for_paren)
  {
    ctrl.for_init = true;
    ctx->at_stmt_start = true;
    ctrl.await_for_paren = false;
  }
}

static inline void track_ctrl_paren_close(void)
{
  ctrl.paren_depth--;
  if (ctrl.paren_depth == 0)
  {
    ctrl.for_init = false;
    ctrl.parens_just_closed = true;
  }
}

static inline void track_ctrl_semicolon(void)
{
  if (ctrl.paren_depth == 1 && ctrl.for_init && ctrl.brace_depth == 0)
    ctrl.for_init = false;
  else if (ctrl.paren_depth == 0)
  {
    typedef_pop_scope(ctx->defer_depth + 1);
    control_flow_reset();
  }
}

static inline void track_generic_paren(Token *tok)
{
  if (equal(tok, "("))
    ctx->generic_paren_depth++;
  else if (equal(tok, ")"))
    ctx->generic_paren_depth--;
}

// Scan ahead for 'orelse' at depth 0 in a bare expression.
// Returns the orelse token, or NULL if not found before ';' or unbalanced close.
// Note: This does NOT stop at commas — in a bare expression context,
// commas are part of the C comma operator, not declarator separators.
// Multi-declarator orelse is handled separately by process_declarators.
static Token *find_bare_orelse(Token *tok)
{
  int depth = 0;
  Token *prev = NULL;
  for (Token *s = tok; s->kind != TK_EOF; s = s->next)
  {
    if (s->flags & TF_OPEN)
      depth++;
    else if (s->flags & TF_CLOSE)
    {
      if (--depth < 0)
        return NULL;
    }
    else if (depth == 0 && equal(s, ";"))
      return NULL;
    if (depth == 0 && (s->tag & TT_ORELSE) && !is_known_typedef(s) &&
        !(prev && (prev->tag & TT_MEMBER)))
      return s;
    prev = s;
  }
  return NULL;
}

// Core transpile: emit transformed tokens to an already-opened FILE*.
// Handles the main transpilation loop, system headers, and cleanup.
// The caller has already preprocessed and tokenized the input.
static int transpile_tokens(Token *tok, FILE *fp)
{
  out_init(fp);

  if (FEAT(F_FLATTEN)) // Suppress warnings for inlined system header content.
  {
    emit_system_header_diag_push();
    out_char('\n');
  }

  reset_transpiler_state();
  typedef_table_reset();
  system_includes_reset();

  if (!FEAT(F_FLATTEN))
  {
    collect_system_includes();
    emit_system_includes();
  }

  bool next_func_returns_void = false; // Track void functions at top level
  Token *prev_toplevel_tok = NULL;     // Track previous token at top level for function detection
  Token *last_toplevel_paren = NULL;   // Track last ')' at top level for K&R detection

  // Walk tokens and emit
  while (tok->kind != TK_EOF)
  {
    Token *next;
    uint32_t tag = tok->tag;

#define DISPATCH(handler) \
  {                       \
    next = handler(tok);  \
    if (next)             \
    {                     \
      tok = next;         \
      continue;           \
    }                     \
  }

    // ── Fast path: untagged tokens with no stmt-start processing ──
    // Most tokens (~70-80%) are plain identifiers, numbers, or operators without
    // any tag bits set and not at statement start. Skip all dispatch and emit directly.
    // Structural punctuation ({, }, ;, :) has TT_STRUCTURAL set, so it falls through.
    if (__builtin_expect(!tag && !ctx->at_stmt_start, 1))
    {
      if (__builtin_expect(ctrl.pending && tok->len == 1, 0))
      {
        char c = tok->shortcut;
        if (c == '(')
          track_ctrl_paren_open();
        else if (c == ')')
          track_ctrl_paren_close();
      }
      if (__builtin_expect(ctx->generic_paren_depth > 0, 0))
        track_generic_paren(tok);
      if (ctx->defer_depth == 0)
      {
        if (match_ch(tok, ')'))
          last_toplevel_paren = tok;
        prev_toplevel_tok = tok;
      }
      emit_tok(tok);
      tok = tok->next;
      continue;
    }

    // Slower path: statement-start processing and tagged tokens

    // Track typedefs (must precede zero-init check)
    if (ctx->at_stmt_start && ctx->struct_depth == 0 && (tag & TT_TYPEDEF))
      parse_typedef_declaration(tok, ctx->defer_depth);

    // Zero-init declarations at statement start
    if (ctx->at_stmt_start && (!ctrl.pending || ctrl.for_init))
    {
      next = try_zero_init_decl(tok);
      if (next)
      {
        tok = next;
        ctx->at_stmt_start = true;
        continue;
      }

      // Bare expression orelse
      if (FEAT(F_ORELSE) && ctx->defer_depth > 0 && ctx->struct_depth == 0)
      {
        Token *orelse_tok = find_bare_orelse(tok);
        if (orelse_tok)
        {
          if (tok->tag & TT_ORELSE && !is_known_typedef(tok))
            error_tok(tok, "expected expression before 'orelse'");
          OUT_LIT(" if (!(");
          while (tok != orelse_tok)
          {
            emit_tok(tok);
            tok = tok->next;
          }
          OUT_LIT("))");
          tok = tok->next; // skip 'orelse'

          if (equal(tok, ";"))
            error_tok(tok, "expected statement after 'orelse'");

          tok = emit_orelse_action(tok, NULL, false, NULL);
          continue;
        }
      }
    }
    ctx->at_stmt_start = false;

    if (tag & TT_NORETURN_FN) // Noreturn function warning
    {
      if (tok->next && equal(tok->next, "("))
      {
        mark_switch_control_exit();
        if (FEAT(F_DEFER) && has_active_defers())
          fprintf(stderr, "%s:%d: warning: '%.*s' called with active defers (defers will not run)\n",
                  tok_file(tok)->name, tok_line_no(tok), tok->len, tok->loc);
      }
    }

    // ── Tag-dependent dispatch (skip entirely for untagged tokens like punctuation) ──
    if (tag)
    {
      // ── Keyword dispatch (defer, return, break/continue, goto) ──

      if (__builtin_expect(tag & TT_DEFER, 0))
        DISPATCH(handle_defer_keyword);
      if (__builtin_expect(FEAT(F_DEFER) && (tag & TT_RETURN), 0))
        DISPATCH(handle_return_defer);
      if (__builtin_expect(FEAT(F_DEFER) && (tag & (TT_BREAK | TT_CONTINUE)), 0))
        DISPATCH(handle_break_continue_defer);
      if (__builtin_expect((tag & TT_GOTO) && FEAT(F_DEFER | F_ZEROINIT), 0))
        DISPATCH(handle_goto_keyword);

      // ── Control-flow flag setting (loop, switch, if/else) ──

      if (tag & TT_LOOP)
      {
        if (FEAT(F_DEFER))
        {
          ctrl.next_scope |= NS_LOOP;
          ctrl.pending = true;
          if (equal(tok, "do"))
            ctrl.parens_just_closed = true;
        }
        if (equal(tok, "for") && FEAT(F_DEFER | F_ZEROINIT))
        {
          ctrl.pending = true;
          ctrl.await_for_paren = true;
        }
      }

      if ((tag & TT_GENERIC) && ctx->generic_paren_depth == 0)
      {
        emit_tok(tok);
        last_emitted = tok;
        tok = tok->next;
        if (tok && equal(tok, "("))
        {
          ctx->generic_paren_depth = 1;
          emit_tok(tok);
          last_emitted = tok;
          tok = tok->next;
        }
        continue;
      }

      if (FEAT(F_DEFER) && (tag & TT_SWITCH))
      {
        ctrl.next_scope |= NS_SWITCH;
        ctrl.pending = true;
      }

      if (tag & TT_IF)
      {
        ctrl.pending = true;
        if (equal(tok, "else"))
          ctrl.parens_just_closed = true;
      }

      // Case/default label handling
      if (tag & (TT_CASE | TT_DEFAULT))
        handle_case_default(tok);

    } // end if (tag)

    // _Generic paren tracking (only active inside _Generic expressions)
    if (__builtin_expect(ctx->generic_paren_depth > 0, 0))
      track_generic_paren(tok);

    // Void function detection at top level
    // Fast-reject: only enter is_void_function_decl for tokens that could plausibly
    // start a void function (tagged keywords, or void typedef identifiers)
    if (ctx->defer_depth == 0 &&
        (tag & (TT_TYPE | TT_QUALIFIER | TT_SKIP_DECL | TT_ATTR | TT_INLINE) ||
         (tok->kind == TK_IDENT && is_void_typedef(tok))) &&
        is_void_function_decl(tok))
      next_func_returns_void = true;

    if (tag & TT_SUE) // struct/union/enum body
      DISPATCH(handle_sue_body);

    // Structural punctuation dispatch: { } ; :
    // TT_STRUCTURAL is set on { } ; : (and their digraph equivalents <% %>)
    if (tag & TT_STRUCTURAL)
    {
      // Use match_ch for digraph-safe comparison
      if (match_ch(tok, '{'))
      {
        // First check: function definition detection at top level
        if (ctx->defer_depth == 0 && FEAT(F_DEFER))
        {
          bool is_func_def = false;
          if (prev_toplevel_tok && equal(prev_toplevel_tok, ")"))
            is_func_def = true;
          else if (last_toplevel_paren && is_knr_params(last_toplevel_paren->next, tok))
            is_func_def = true;
          else if (last_toplevel_paren)
          {
            // Check for attributes between ')' and '{' (e.g. C23 [[...]] attrs)
            Token *after = skip_all_attributes(last_toplevel_paren->next);
            if (after == tok)
              is_func_def = true;
          }
          if (is_func_def)
          {
            scan_labels_in_function(tok);
            ctx->current_func_returns_void = next_func_returns_void;
          }
          next_func_returns_void = false;
          last_toplevel_paren = NULL;
        }
        tok = handle_open_brace(tok);
        continue;
      }
      if (match_ch(tok, '}'))
      {
        tok = handle_close_brace(tok);
        continue;
      }
      // ; and : are never digraphs — direct char compare
      char c = tok->loc[0];
      if (c == ';')
      {
        if (ctrl.pending)
          track_ctrl_semicolon();
        if (!ctrl.pending)
          end_statement_after_semicolon();
        if (ctx->defer_depth == 0)
          next_func_returns_void = false;
        emit_tok(tok);
        tok = tok->next;
        continue;
      }
      if (c == ':' && last_emitted && last_emitted->kind == TK_IDENT &&
          ctx->struct_depth == 0 && ctx->defer_depth > 0)
      {
        emit_tok(tok);
        tok = tok->next;
        ctx->at_stmt_start = true;
        continue;
      }
      // ':' that isn't a label — fall through to default emit
    }

    // Preprocessor directives
    if (__builtin_expect(tok->kind == TK_PREP_DIR, 0))
    {
      emit_tok(tok);
      tok = tok->next;
      ctx->at_stmt_start = true;
      continue;
    }

    // Paren tracking for control flow (parens without TT_STRUCTURAL are ( and ))
    if (ctrl.pending && tok->len == 1)
    {
      char c = tok->loc[0];
      if (c == '(')
        track_ctrl_paren_open();
      else if (c == ')')
        track_ctrl_paren_close();
    }

    // Track previous token at top level for function detection
    if (ctx->defer_depth == 0)
    {
      if (match_ch(tok, ')'))
        last_toplevel_paren = tok;
      prev_toplevel_tok = tok;
    }

    // Default: emit token as-is
    emit_tok(tok);
    tok = tok->next;
  }

  if (FEAT(F_FLATTEN)) // Close diagnostic pragma in flattening mode
  {
    out_char('\n');
    emit_system_header_diag_pop();
  }

  out_close();
  tokenizer_teardown(false);
  return 1;
}

// Preprocess + tokenize + transpile to a file path.
// This is the original interface used by the CLI and library API.
static int transpile(char *input_file, char *output_file)
{
  // Ensure keyword map is initialized before allocating preprocessor buffer.
  // If init fails (OOM → longjmp in lib mode), we haven't allocated pp_buf yet.
  if (!ctx->keyword_map.capacity)
    init_keyword_map();

  // Run system preprocessor via pipe — no temp files
  char *pp_buf = preprocess_with_cc(input_file);
  if (!pp_buf)
  {
    fprintf(stderr, "Preprocessing failed for: %s\n", input_file);
    return 0;
  }

  // Tokenize directly from the in-memory buffer (takes ownership of pp_buf)
  Token *tok = tokenize_buffer(input_file, pp_buf);
  // pp_buf is now owned by the tokenizer (freed in tokenizer_teardown)

  if (!tok)
  {
    fprintf(stderr, "Failed to tokenize preprocessed output\n");
    tokenizer_teardown(false);
    return 0;
  }

  FILE *fp = fopen(output_file, "w");
  if (!fp)
  {
    tokenizer_teardown(false);
    return 0;
  }

  return transpile_tokens(tok, fp);
}

// LIBRARY API

PRISM_API void prism_free(PrismResult *r)
{
  free(r->output);
  free(r->error_msg);
  r->output = r->error_msg = NULL;
}

PRISM_API void prism_reset(void)
{
  typedef_table_reset();
  tokenizer_teardown(true);

  for (int i = 0; i < defer_stack_capacity; i++)
  {
    free(defer_stack[i].entries);
    defer_stack[i] = (DeferScope){0};
  }
  free(defer_stack);
  defer_stack = NULL;
  ctx->defer_depth = 0;
  defer_stack_capacity = 0;

  free(label_table.labels);
  hashmap_clear(&label_table.name_map);
  label_table = (LabelTable){0};

  system_includes_reset();

  free(stmt_expr_levels);
  stmt_expr_levels = NULL;
  ctx->stmt_expr_count = 0;
  stmt_expr_capacity = 0;

  if (out_fp)
  {
    out_flush();
    fclose(out_fp);
    out_fp = NULL;
  }
}

PRISM_API PrismResult prism_transpile_file(const char *input_file, PrismFeatures features)
{
  prism_ctx_init();
  PrismResult result = {0};

#ifdef PRISM_LIB_MODE
  // Set up error recovery point - if error()/error_tok()/error_at() is called,
  // we longjmp back here instead of calling exit(1)
  ctx->error_msg[0] = '\0';
  ctx->error_line = 0;
  ctx->error_col = 0;
  ctx->error_jmp_set = true;

  if (setjmp(ctx->error_jmp) != 0)
  {
    // We got here via longjmp from an error function
    ctx->error_jmp_set = false;
    result.status = PRISM_ERR_SYNTAX;
    result.error_msg = strdup(ctx->error_msg[0] ? ctx->error_msg : "Unknown error");
    // strdup returning NULL is fine — caller treats NULL error_msg as "unknown"
    result.error_line = ctx->error_line;
    result.error_col = ctx->error_col;
    // Clean up any temp files that were created before the error
    if (ctx->active_temp_output[0])
    {
      remove(ctx->active_temp_output);
      ctx->active_temp_output[0] = '\0';
    }
    // Free open_memstream buffer if longjmp fired during transpile_tokens
    // Close out_fp first since it references the membuf (open_memstream FILE)
    if (out_fp)
    {
      fclose(out_fp);
      out_fp = NULL;
    }
    if (ctx->active_membuf)
    {
      free(ctx->active_membuf);
      ctx->active_membuf = NULL;
    }
    // Reset global state to allow future transpilations
    prism_reset();
    return result;
  }
#endif

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

  // Use open_memstream to write directly to memory (no temp files)
  if (!ctx->keyword_map.capacity)
    init_keyword_map();

  char *pp_buf = preprocess_with_cc((char *)input_file);
  if (!pp_buf)
  {
    result.status = PRISM_ERR_IO;
    result.error_msg = strdup("Preprocessing failed");
    goto cleanup;
  }

  Token *tok = tokenize_buffer((char *)input_file, pp_buf);
  if (!tok)
  {
    result.status = PRISM_ERR_SYNTAX;
    result.error_msg = strdup("Failed to tokenize");
    tokenizer_teardown(false);
    goto cleanup;
  }

  {
    size_t memlen = 0;
#ifdef PRISM_LIB_MODE
    ctx->active_membuf = NULL;
    FILE *fp = open_memstream(&ctx->active_membuf, &memlen);
#else
    char *membuf = NULL;
    FILE *fp = open_memstream(&membuf, &memlen);
#endif
    if (!fp)
    {
      result.status = PRISM_ERR_IO;
      result.error_msg = strdup("open_memstream failed");
      prism_reset();
      goto cleanup;
    }

    if (transpile_tokens(tok, fp))
    {
#ifdef PRISM_LIB_MODE
      result.output = ctx->active_membuf;
#else
      result.output = membuf;
#endif
      result.output_len = memlen;
      result.status = PRISM_OK;
    }
    else
    {
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
  }

cleanup:
#ifdef PRISM_LIB_MODE
  ctx->error_jmp_set = false;
#endif
  return result;
}

#ifndef PRISM_LIB_MODE

// Transpile a single source file and pipe the output directly to the compiler.
// No temp files for transpiled output. The compiler reads from stdin.
// compile_argv must contain the full argv for the compiler (without input file — uses stdin via "-").
// Returns the compiler's exit status, or -1 on error.
static int transpile_and_compile(char *input_file, char **compile_argv, bool verbose)
{
  // Preprocess via pipe
  char *pp_buf = preprocess_with_cc(input_file);
  if (!pp_buf)
  {
    fprintf(stderr, "Preprocessing failed for: %s\n", input_file);
    return -1;
  }

  Token *tok = tokenize_buffer(input_file, pp_buf);
  if (!tok)
  {
    fprintf(stderr, "Failed to tokenize preprocessed output\n");
    tokenizer_teardown(false);
    return -1;
  }

  // Set up pipe: prism writes transpiled output → compiler reads from stdin
  int pipefd[2];
  if (pipe(pipefd) == -1)
  {
    perror("pipe");
    tokenizer_teardown(false);
    return -1;
  }

  if (verbose)
  {
    fprintf(stderr, "[prism] ");
    for (int i = 0; compile_argv[i]; i++)
      fprintf(stderr, "%s ", compile_argv[i]);
    fprintf(stderr, "\n");
  }

  // Set up file actions: child stdin ← pipe read end
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

  if (err)
  {
    fprintf(stderr, "posix_spawnp: %s: %s\n", compile_argv[0], strerror(err));
    close(pipefd[1]);
    tokenizer_teardown(false);
    return -1;
  }

  // Parent: write transpiled output to pipe
  FILE *fp = fdopen(pipefd[1], "w");
  if (!fp)
  {
    close(pipefd[1]);
    tokenizer_teardown(false);
    waitpid(pid, NULL, 0);
    return -1;
  }

  transpile_tokens(tok, fp);
  // out_close() inside transpile_tokens closes fp → closes pipe → EOF to compiler

  return wait_for_child(pid);
}

static char **build_argv(const char *first, ...)
{
  ArgvBuilder ab;
  argv_builder_init(&ab);
  if (first)
    argv_builder_add(&ab, first);
  va_list ap;
  va_start(ap, first);
  const char *arg;
  while ((arg = va_arg(ap, const char *)) != NULL)
    argv_builder_add(&ab, arg);
  va_end(ap);
  return argv_builder_finish(&ab);
}

static noreturn void die(char *message)
{
  fprintf(stderr, "%s\n", message);
  exit(1);
}

// Resolve the path to the currently running executable (platform-specific)
static bool get_self_exe_path(char *buf, size_t bufsize)
{
#if defined(_WIN32)
  DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)bufsize);
  return (len > 0 && len < bufsize);
#elif defined(__linux__)
  ssize_t len = readlink("/proc/self/exe", buf, bufsize - 1);
  if (len > 0)
  {
    buf[len] = '\0';
    return true;
  }
#elif defined(__APPLE__)
  uint32_t sz = (uint32_t)bufsize;
  if (_NSGetExecutablePath(buf, &sz) == 0)
  {
    char temp[PATH_MAX];
    if (realpath(buf, temp))
    {
      strncpy(buf, temp, bufsize - 1);
      buf[bufsize - 1] = '\0';
    }
    return true;
  }
#elif defined(__FreeBSD__) || defined(__DragonFly__)
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
  size_t len = bufsize;
  if (sysctl(mib, 4, buf, &len, NULL, 0) == 0)
    return true;
#elif defined(__NetBSD__)
  ssize_t len = readlink("/proc/curproc/exe", buf, bufsize - 1);
  if (len > 0)
  {
    buf[len] = '\0';
    return true;
  }
#elif defined(__OpenBSD__)
  // OpenBSD doesn't expose exe path via sysctl or /proc;
  // use argv[0] resolution via realpath at caller site.
  // Fallthrough to return false.
#endif
  // Generic fallback: try Solaris/Illumos /proc path, then Linux-style
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__DragonFly__) && !defined(__NetBSD__) && !defined(__linux__)
  {
    // Solaris/Illumos
    ssize_t len2 = readlink("/proc/self/path/a.out", buf, bufsize - 1);
    if (len2 > 0)
    {
      buf[len2] = '\0';
      return true;
    }
    // Other /proc-based systems
    len2 = readlink("/proc/self/exe", buf, bufsize - 1);
    if (len2 > 0)
    {
      buf[len2] = '\0';
      return true;
    }
  }
#endif
  (void)buf;
  (void)bufsize;
  return false;
}

static void check_path_shadow(void)
{
#ifdef _WIN32
  const char *cmd = "where prism.exe 2>nul";
#else
  const char *cmd = "which -a prism 2>/dev/null || command -v prism 2>/dev/null";
#endif
  FILE *fp = popen(cmd, "r");
  if (!fp)
    return;
  char first_hit[PATH_MAX];
  first_hit[0] = '\0';
  if (fgets(first_hit, sizeof(first_hit), fp))
  {
    // Strip trailing newline
    size_t len = strlen(first_hit);
    while (len > 0 && (first_hit[len - 1] == '\n' || first_hit[len - 1] == '\r'))
      first_hit[--len] = '\0';
  }
  pclose(fp);
  if (first_hit[0] && strcmp(first_hit, INSTALL_PATH) != 0)
  {
    // Resolve symlinks for both paths before comparing
    char resolved_hit[PATH_MAX], resolved_install[PATH_MAX];
    char *rh = realpath(first_hit, resolved_hit);
    char *ri = realpath(INSTALL_PATH, resolved_install);
    if (rh && ri && strcmp(rh, ri) == 0)
      return; // Same file via symlink — no shadow
    fprintf(stderr,
            "[prism] Warning: '%s' shadows '%s' in your PATH.\n"
            "[prism] The newly installed version will NOT be used.\n"
            "[prism] Fix: remove or update '%s', or adjust your PATH.\n",
            first_hit, INSTALL_PATH, first_hit);
  }
}

static int install(char *self_path)
{
  printf("[prism] Installing to %s...\n", INSTALL_PATH);

  // Resolve self_path to actual executable path only if the given path doesn't exist
  char resolved_path[PATH_MAX];
  struct stat st;
  if (stat(self_path, &st) != 0 && get_self_exe_path(resolved_path, sizeof(resolved_path)))
    self_path = resolved_path;

  if (strcmp(self_path, INSTALL_PATH) == 0)
  {
    printf("[prism] Already installed at %s\n", INSTALL_PATH);
    return 0;
  }

  // Try direct copy first (avoids sudo if we have permission)
  FILE *input = fopen(self_path, "rb");
  FILE *output = input ? fopen(INSTALL_PATH, "wb") : NULL;

  if (input && output)
  {
    char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, 4096, input)) > 0)
    {
      if (fwrite(buffer, 1, bytes, output) != bytes)
      {
        fclose(input);
        fclose(output);
        goto use_sudo;
      }
    }
    fclose(input);
    fclose(output);
    chmod(INSTALL_PATH, 0755); // no-op on Windows (shimmed)
    printf("[prism] Installed!\n");
    check_path_shadow();
    return 0;
  }

  if (input)
    fclose(input);
  if (output)
    fclose(output);

use_sudo:;
#ifdef _WIN32 // Windows: no sudo — just report failure
  fprintf(stderr, "Failed to install (run as Administrator?)\n");
  return 1;
#else
  char **argv = build_argv("sudo", "rm", "-f", INSTALL_PATH, NULL);
  run_command(argv);
  free_argv(argv);

  // Copy
  argv = build_argv("sudo", "cp", self_path, INSTALL_PATH, NULL);
  int status = run_command(argv);
  free_argv(argv);
  if (status != 0)
  {
    fprintf(stderr, "Failed to install\n");
    return 1;
  }

  argv = build_argv("sudo", "chmod", "+x", INSTALL_PATH, NULL);
  run_command(argv);
  free_argv(argv);
#endif

  printf("[prism] Installed!\n");
  check_path_shadow();
  return 0;
}

// CLI
// ─────────────────────────────────────────────────────────────────────────────

static bool is_prism_cc(const char *cc)
{
  if (!cc || !*cc)
    return false;
  const char *base = path_basename(cc);
  if (strncmp(base, "prism", 5) == 0)
  {
    char next = base[5];
    if (next == '\0' || next == ' ' || next == '.')
      return true;
  }
  return false;
}

// Get the actual C compiler to use, avoiding infinite recursion if CC=prism
static const char *get_real_cc(const char *cc)
{
  if (!cc || !*cc)
    return PRISM_DEFAULT_CC; // NULL or empty string

  // Simple check: if basename is "prism" or "prism.exe", return default
  if (is_prism_cc(cc))
    return PRISM_DEFAULT_CC;

  // Fast path: if cc has no path separator, it's a bare command name like "cc" or "gcc".
  // These can't be a renamed prism binary (we already checked is_prism_cc above),
  // so skip the expensive realpath() + get_self_exe_path() syscalls.
  if (!strchr(cc, '/') && !strchr(cc, '\\'))
    return cc;

  // Advanced check: resolve paths and compare with current executable
  // This prevents infinite recursion if prism is symlinked or copied to another name
  char cc_real[PATH_MAX];
  char self_real[PATH_MAX];

  if (!get_self_exe_path(self_real, sizeof(self_real)))
    return cc;

  if (realpath(cc, cc_real) == NULL)
    return cc;

  if (strcmp(cc_real, self_real) == 0)
    return PRISM_DEFAULT_CC;

  return cc;
}

#define CLI_PUSH(arr, cnt, cap, item)                              \
  do                                                               \
  {                                                                \
    ENSURE_ARRAY_CAP(arr, (cnt) + 1, cap, 16, __typeof__(*(arr))); \
    (arr)[(cnt)++] = (item);                                       \
  } while (0)

static inline bool has_ext(const char *f, const char *ext)
{
  size_t fl = strlen(f), el = strlen(ext);
  return fl >= el && !strcmp(f + fl - el, ext);
}

static bool str_startswith(const char *s, const char *prefix)
{
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

// Check if the system compiler is clang.
// On macOS, cc/gcc are always Apple Clang (hardlinked, not symlinked,
// so realpath doesn't help).  On other platforms, check the basename.
static bool cc_is_clang(const char *cc)
{
#ifdef __APPLE__
  // On macOS, the default "cc" is always Apple Clang.
  // Only return false if the user explicitly set a non-clang compiler.
  if (!cc || !*cc || strcmp(cc, "cc") == 0 || strcmp(cc, "gcc") == 0)
    return true;
#endif
  if (!cc || !*cc)
    return false;
  const char *base = path_basename(cc);
  return strncmp(base, "clang", 5) == 0;
}

static void print_help(void)
{
  printf(
      "Prism v%s - Robust C transpiler\n\n"
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
      PRISM_VERSION, INSTALL_PATH);
}

static Cli cli_parse(int argc, char **argv)
{
  bool passthrough = false; // -E passthrough detected inline
  Cli cli = {.features = prism_defaults()};

  // Get compiler from environment (avoid infinite recursion if CC=prism)
  char *env_cc = getenv("PRISM_CC");
  if (!env_cc || !*env_cc || is_prism_cc(env_cc))
  {
    env_cc = getenv("CC");
    if (is_prism_cc(env_cc))
      env_cc = NULL;
  }
  cli.cc = (env_cc && *env_cc) ? env_cc : PRISM_DEFAULT_CC;

  for (int i = 1; i < argc; i++)
  {
    char *a = argv[i];

    if (a[0] == '-' && a[1] != '\0') // Fast path: Reject prism flag prefixes first, then fall through to CC passthrough immediately.
    {
      char c1 = a[1]; // Single-letter flags that need special handling

      if (c1 == 'o') // -o: intercept output path
      {
        if (a[2])
          cli.output = a + 2;
        else if (i + 1 < argc)
          cli.output = argv[++i];
        continue;
      }

      if (c1 == 'c' && !a[2]) // -c: track compile-only
      {
        cli.compile_only = true;
        CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
        continue;
      }

      if (c1 == 'E' && !a[2]) // -E: passthrough mode
      {
        passthrough = true;
        CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
        continue;
      }

      if (c1 == 'h' && !a[2])
      {
        print_help();
        exit(0);
      }

      if (c1 == '-') // -- prefix: prism options
      {
        if (!strcmp(a, "--help"))
        {
          print_help();
          exit(0);
        }
        if (!strcmp(a, "--version"))
        {
          printf("prism %s\n", PRISM_VERSION);
          exit(0);
        }
        if (str_startswith(a, "--prism-cc="))
        {
          cli.cc = a + 11;
          continue;
        }
        if (!strcmp(a, "--prism-verbose"))
        {
          cli.verbose = true;
          continue;
        }
        if (str_startswith(a, "--prism-emit="))
        {
          cli.mode = CLI_EMIT;
          cli.output = a + 13;
          continue;
        }
        if (!strcmp(a, "--prism-emit"))
        {
          cli.mode = CLI_EMIT;
          continue;
        }
        CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
        continue;
      }

      if (c1 == 'f') // check for prism feature flags, else CC passthrough
      {
        static const struct
        {
          const char *flag;
          int off;
          int val;
        } feat_flags[] = {
            {"-fno-defer", offsetof(PrismFeatures, defer), 0},
            {"-fno-zeroinit", offsetof(PrismFeatures, zeroinit), 0},
            {"-fno-orelse", offsetof(PrismFeatures, orelse), 0},
            {"-fno-line-directives", offsetof(PrismFeatures, line_directives), 0},
            {"-fno-safety", offsetof(PrismFeatures, warn_safety), 1},
            {"-fflatten-headers", offsetof(PrismFeatures, flatten_headers), 1},
            {"-fno-flatten-headers", offsetof(PrismFeatures, flatten_headers), 0},
        };
        bool matched = false;
        for (int f = 0; f < (int)(sizeof(feat_flags) / sizeof(*feat_flags)); f++)
          if (!strcmp(a, feat_flags[f].flag))
          {
            *(bool *)((char *)&cli.features + feat_flags[f].off) = feat_flags[f].val;
            matched = true;
            break;
          }
        if (matched)
          continue;
        // Not a prism -f flag — pass through
        CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
        continue;
      }

      // All other -X flags (e.g. -D, -I, -W, -g, -O, -m, -std, -l, -L, -s, etc.)
      // are CC passthrough — skip all prism checks entirely
      CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a);
      continue;
    }

    // Prism subcommands
    {
      static const struct
      {
        const char *cmd;
        CliMode mode;
      } subcmds[] = {
          {"run", CLI_RUN},
          {"transpile", CLI_EMIT},
          {"install", CLI_INSTALL},
      };
      bool matched = false;
      for (int j = 0; j < (int)(sizeof(subcmds) / sizeof(*subcmds)); j++)
        if (!strcmp(a, subcmds[j].cmd))
        {
          cli.mode = subcmds[j].mode;
          matched = true;
          break;
        }
      if (matched)
        continue;
    }

    // Source files (.c/.i) — transpile unless -E passthrough
    if ((has_ext(a, ".c") || has_ext(a, ".i")) && !passthrough)
    {
      CLI_PUSH(cli.sources, cli.source_count, cli.source_cap, a);
      continue;
    }

    CLI_PUSH(cli.cc_args, cli.cc_arg_count, cli.cc_arg_cap, a); // pass through to CC (object files, libraries, etc.)
  }

  return cli;
}

static void cli_free(Cli *cli)
{
  free(cli->sources);
  free(cli->cc_args);
}

static void add_warn_suppress(ArgvBuilder *ab, bool clang, bool msvc)
{
  if (msvc) // suppress common transpiler-generated warnings
  {
    argv_builder_add(ab, "/wd4100"); // unreferenced formal parameter
    argv_builder_add(ab, "/wd4189"); // local variable initialized but not referenced
    argv_builder_add(ab, "/wd4244"); // possible loss of data (implicit narrowing)
    argv_builder_add(ab, "/wd4267"); // conversion from 'size_t' to 'int'
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
  for (int i = 0; i < (int)(sizeof(w) / sizeof(*w)); i++)
    argv_builder_add(ab, w[i]);
  if (clang)
    argv_builder_add(ab, "-Wno-unknown-warning-option");
  else
    argv_builder_add(ab, "-Wno-logical-op");
}

static void verbose_argv(char **args)
{
  fprintf(stderr, "[prism]");
  for (int i = 0; args[i]; i++)
    fprintf(stderr, " %s", args[i]);
  fprintf(stderr, "\n");
}

static void add_output_flags(ArgvBuilder *ab, const Cli *cli, const char *temp_exe, bool msvc)
{
  static char defobj[PATH_MAX];
  const char *out = NULL;

  if (cli->mode == CLI_RUN)
    out = temp_exe;
  else if (cli->output)
    out = cli->output;
  else if (cli->compile_only && cli->source_count == 1)
  {
    const char *base = path_basename(cli->sources[0]);
    snprintf(defobj, sizeof(defobj), "%s", base);
    char *dot = strrchr(defobj, '.');
    if (dot)
      strcpy(dot, msvc ? ".obj" : ".o");
    out = defobj;
  }

  if (!out)
    return;

  if (msvc)
  {
    static char flag[PATH_MAX + 8]; // cl.exe: /Fe:exe or /Fo:obj
    if (cli->compile_only)
      snprintf(flag, sizeof(flag), "/Fo:%s", out);
    else
      snprintf(flag, sizeof(flag), "/Fe:%s", out);
    argv_builder_add(ab, flag);
  }
  else
  {
    argv_builder_add(ab, "-o");
    argv_builder_add(ab, out);
  }
}

static void make_run_temp(char *buf, size_t size, CliMode mode)
{
  buf[0] = '\0';
  if (mode != CLI_RUN)
    return;
  snprintf(buf, size, "%sprism_run.XXXXXX", get_tmp_dir());
#if defined(_WIN32)
  if (atomic_mkstemp(buf, size, ".exe") != 0)
    buf[0] = '\0';
#else
  int fd = mkstemp(buf);
  if (fd >= 0)
    close(fd);
#endif
}

static int passthrough_cc(const Cli *cli)
{
  const char *compiler = get_real_cc(cli->cc);
  bool msvc = cc_is_msvc(compiler);
  ArgvBuilder ab;
  argv_builder_init(&ab);
  argv_builder_add(&ab, compiler);
  for (int i = 0; i < cli->cc_arg_count; i++)
    argv_builder_add(&ab, cli->cc_args[i]);
  if (cli->output)
  {
    if (msvc)
    {
      static char fe_flag[PATH_MAX + 8];
      snprintf(fe_flag, sizeof(fe_flag), "/Fe:%s", cli->output);
      argv_builder_add(&ab, fe_flag);
    }
    else
    {
      argv_builder_add(&ab, "-o");
      argv_builder_add(&ab, cli->output);
    }
  }
  char **pass = argv_builder_finish(&ab);
  if (cli->verbose)
    verbose_argv(pass);
  int st = run_command(pass);
  free_argv(pass);
  return st;
}

static int install_from_source(Cli *cli)
{
  char temp_bin[PATH_MAX];
  snprintf(temp_bin, sizeof(temp_bin), "%sprism_install_%d" EXE_SUFFIX, get_tmp_dir(), getpid());

  const char *cc = get_real_cc(cli->cc ? cli->cc : getenv("PRISM_CC"));
  if (!cc || (strcmp(cc, "cc") == 0 && !cli->cc))
  {
    cc = getenv("CC");
    if (cc)
      cc = get_real_cc(cc);
  }

  if (!cc)
    cc = PRISM_DEFAULT_CC;

  bool msvc = cc_is_msvc(cc);

  char **temp_files = malloc(cli->source_count * sizeof(char *));
  if (!temp_files)
    die("Memory allocation failed");

  for (int i = 0; i < cli->source_count; i++)
  {
    temp_files[i] = malloc(PATH_MAX);
    if (!temp_files[i])
      die("Memory allocation failed");
    snprintf(temp_files[i], PATH_MAX, "%sprism_install_%d_%d.c", get_tmp_dir(), getpid(), i);

    PrismResult result = prism_transpile_file(cli->sources[i], cli->features);
    if (result.status != PRISM_OK)
    {
      fprintf(stderr, "%s:%d:%d: error: %s\n",
              cli->sources[i], result.error_line, result.error_col,
              result.error_msg ? result.error_msg : "transpilation failed");
      for (int j = 0; j <= i; j++)
      {
        remove(temp_files[j]);
        free(temp_files[j]);
      }
      free(temp_files);
      return 1;
    }

    FILE *f = fopen(temp_files[i], "w");
    if (!f)
    {
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

  for (int i = 0; i < cli->source_count; i++)
    argv_builder_add(&ab, temp_files[i]);

  for (int i = 0; i < cli->cc_arg_count; i++)
    argv_builder_add(&ab, cli->cc_args[i]);

  if (msvc)
  {
    static char fe_flag[PATH_MAX + 8];
    snprintf(fe_flag, sizeof(fe_flag), "/Fe:%s", temp_bin);
    argv_builder_add(&ab, fe_flag);
  }
  else
  {
    argv_builder_add(&ab, "-o");
    argv_builder_add(&ab, temp_bin);
  }
  char **argv_cc = argv_builder_finish(&ab);

  if (cli->verbose)
    verbose_argv(argv_cc);

  int status = run_command(argv_cc);
  free_argv(argv_cc);

  for (int i = 0; i < cli->source_count; i++)
  {
    remove(temp_files[i]);
    free(temp_files[i]);
  }
  free(temp_files);

  if (status != 0)
    return 1;

  int result = install(temp_bin);
  remove(temp_bin);
  return result;
}

static int compile_sources(Cli *cli)
{
  int status = 0;
  const char *compiler = get_real_cc(cli->cc);
  bool clang = cc_is_clang(compiler);
  bool msvc = cc_is_msvc(compiler);
  char temp_exe[PATH_MAX];
  make_run_temp(temp_exe, sizeof(temp_exe), cli->mode);

  if (cli->source_count == 1 && !msvc)
  {
    // Single source: pipe-based transpile+compile (no temp files)
    // MSVC cl.exe cannot read C from stdin, so it always uses the temp-file path below.
    ArgvBuilder ab;
    argv_builder_init(&ab);
    argv_builder_add(&ab, compiler);

    // Tell cc the input is already preprocessed (flatten mode).
    // Clang does not support -fpreprocessed.
    argv_builder_add(&ab, "-x");
    argv_builder_add(&ab, "c");
    if (FEAT(F_FLATTEN) && !clang)
      argv_builder_add(&ab, "-fpreprocessed");
    argv_builder_add(&ab, "-");

    if (cli->cc_arg_count > 0) // Reset language so subsequent args aren't forced to C.
    {
      argv_builder_add(&ab, "-x");
      argv_builder_add(&ab, "none");
    }

    for (int i = 0; i < cli->cc_arg_count; i++)
      argv_builder_add(&ab, cli->cc_args[i]);

    add_warn_suppress(&ab, clang, false);
    add_output_flags(&ab, cli, temp_exe, false);
    char **argv_cc = argv_builder_finish(&ab);

    if (cli->verbose)
      fprintf(stderr, "[prism] Transpiling %s (pipe → cc)\n", cli->sources[0]);
    status = transpile_and_compile((char *)cli->sources[0], argv_cc, cli->verbose);
    free_argv(argv_cc);
  }
  else
  {
    // Multi-source: transpile to temp files, then compile together
    char **temps = calloc(cli->source_count, sizeof(char *));
    if (!temps)
      die("Out of memory");

    for (int i = 0; i < cli->source_count; i++)
    {
      temps[i] = malloc(512);
      if (!temps[i])
        die("Out of memory");
      if (make_temp_file(temps[i], 512, NULL, 0, cli->sources[i]) < 0)
        die("Failed to create temp file");
      if (cli->verbose)
        fprintf(stderr, "[prism] Transpiling %s -> %s\n", cli->sources[i], temps[i]);
      if (!transpile((char *)cli->sources[i], temps[i]))
      {
        for (int j = 0; j <= i; j++)
        {
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

    if (FEAT(F_FLATTEN) && !clang && !msvc)
      argv_builder_add(&ab, "-fpreprocessed");

    for (int i = 0; i < cli->source_count; i++)
      argv_builder_add(&ab, temps[i]);

    if (FEAT(F_FLATTEN) && !clang && !msvc)
      argv_builder_add(&ab, "-fno-preprocessed");

    for (int i = 0; i < cli->cc_arg_count; i++)
      argv_builder_add(&ab, cli->cc_args[i]);

    add_warn_suppress(&ab, clang, msvc);
    add_output_flags(&ab, cli, temp_exe, msvc);
    char **argv_cc = argv_builder_finish(&ab);

    if (cli->verbose)
      verbose_argv(argv_cc);
    status = run_command(argv_cc);
    free_argv(argv_cc);

    for (int i = 0; i < cli->source_count; i++)
    {
      remove(temps[i]);
      free(temps[i]);
    }
    free(temps);
  }

  if (status != 0)
  {
    if (temp_exe[0])
      remove(temp_exe);
    return status;
  }

  if (cli->mode == CLI_RUN)
  {
    char **run = build_argv(temp_exe, NULL);
    if (cli->verbose)
      fprintf(stderr, "[prism] Running %s\n", temp_exe);
    status = run_command(run);
    free_argv(run);
    remove(temp_exe);
  }

  return status;
}

int main(int argc, char **argv)
{
  int status = 0;
  prism_ctx_init();

  if (argc < 2)
  {
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
  else if (cli.mode == CLI_EMIT)
  {
    if (cli.source_count == 0)
      die("No source files specified");
    for (int i = 0; i < cli.source_count; i++)
    {
      if (cli.output)
      {
        if (cli.verbose)
          fprintf(stderr, "[prism] %s -> %s\n", cli.sources[i], cli.output);
        if (!transpile((char *)cli.sources[i], (char *)cli.output))
          die("Transpilation failed");
      }
      else
      {
#ifdef _WIN32
        char temp[PATH_MAX];
        if (make_temp_file(temp, sizeof(temp), NULL, 0, cli.sources[i]) < 0)
          die("Failed to create temp file");
        if (!transpile((char *)cli.sources[i], temp))
        {
          remove(temp);
          die("Transpilation failed");
        }
        FILE *f = fopen(temp, "r");
        if (f)
        {
          int c;
          while ((c = fgetc(f)) != EOF)
            putchar(c);
          fclose(f);
        }
        remove(temp);
#else
        if (!transpile((char *)cli.sources[i], "/dev/stdout"))
          die("Transpilation failed");
#endif
      }
    }
  }
  else if (cli.source_count == 0)
    status = passthrough_cc(&cli);
  else
    status = compile_sources(&cli);

  cli_free(&cli);
  return status;
}

#endif // PRISM_LIB_MODE

#define PRISM_VERSION "0.101.0"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "parse.c"

#include <sys/stat.h>
#include <sys/wait.h>

// Platform-specific headers for get_real_cc()
#ifdef __APPLE__
#include <mach-o/dyld.h> // _NSGetExecutablePath
#endif
#if defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

// LIBRARY API

typedef struct
{
  bool defer;
  bool zeroinit;
  bool line_directives;
  bool warn_safety;     // If true, safety checks warn instead of error
  bool flatten_headers; // If true, include flattened system headers (default: true)

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

// API visibility control - in library mode, API functions are non-static for linking
#ifdef PRISM_LIB_MODE
#define PRISM_API
#else
#define PRISM_API static
#endif

PRISM_API PrismFeatures prism_defaults(void)
{
  return (PrismFeatures){
      .defer = true,
      .zeroinit = true,
      .line_directives = true,
      .warn_safety = false,
      .flatten_headers = true,
      .compiler = NULL,
      .include_paths = NULL,
      .include_count = 0,
      .defines = NULL,
      .define_count = 0,
      .compiler_flags = NULL,
      .compiler_flags_count = 0,
      .force_includes = NULL,
      .force_include_count = 0};
}

// Forward declarations for library API
PRISM_API PrismResult prism_transpile_file(const char *input_file, PrismFeatures features);
PRISM_API void prism_free(PrismResult *r);
PRISM_API void prism_reset(void);

// Get temp directory - respects $TMPDIR environment variable (POSIX standard)
// Returns path with trailing slash (or empty string on Windows)
static const char *get_tmp_dir(void)
{
#ifdef _WIN32
  return "";
#else
  static char tmp_buf[PATH_MAX];
  const char *tmpdir = getenv("TMPDIR");
  if (tmpdir && tmpdir[0])
  {
    size_t len = strlen(tmpdir);
    if (len > 0 && len < PATH_MAX - 1)
    {
      strcpy(tmp_buf, tmpdir);
      if (tmp_buf[len - 1] != '/')
      {
        tmp_buf[len] = '/';
        tmp_buf[len + 1] = '\0';
      }
      return tmp_buf;
    }
  }
  return "/tmp/";
#endif
}

// CLI CONFIGURATION (excluded with -DPRISM_LIB_MODE)

#ifndef PRISM_LIB_MODE

#ifdef _WIN32
#define INSTALL_PATH "prism.exe"
#else
#define INSTALL_PATH "/usr/local/bin/prism"
#endif

#endif // PRISM_LIB_MODE

// Initial capacity for all dynamic arrays (grows as needed)
#define INITIAL_CAP 32

// Note: ENSURE_ARRAY_CAP is defined in parse.c

#define MAX_TYPEOF_VARS_PER_DECL 32

static Token *skip_balanced(Token *tok, char *open, char *close);

// Check if token is an attribute keyword (__attribute__, __attribute, __declspec)
static inline bool is_attribute_keyword(Token *tok)
{
  return equal(tok, "__attribute__") || equal(tok, "__attribute") || equal(tok, "__declspec");
}

// Check if token is identifier-like (TK_IDENT or TK_KEYWORD like 'raw'/'defer')
static inline bool is_identifier_like(Token *tok)
{
  return tok->kind == TK_IDENT || tok->kind == TK_KEYWORD;
}

// Skip all attributes (GNU-style and C23-style) starting at tok
// Returns pointer to first token after all attributes
static Token *skip_all_attributes(Token *tok)
{
  while (tok && tok->kind != TK_EOF)
  {
    // Skip GNU/MSVC-style: __attribute__((...)), __declspec(...)
    if (is_attribute_keyword(tok))
    {
      tok = tok->next;
      if (tok && equal(tok, "("))
        tok = skip_balanced(tok, "(", ")");
      continue;
    }
    // Skip C23-style: [[...]]
    if (equal(tok, "[") && tok->next && equal(tok->next, "["))
    {
      tok = tok->next->next;
      int depth = 1;
      while (tok && tok->kind != TK_EOF && depth > 0)
      {
        if (equal(tok, "["))
          depth++;
        else if (equal(tok, "]"))
          depth--;
        tok = tok->next;
      }
      continue;
    }
    break;
  }
  return tok;
}

// Skip only GNU/MSVC-style attributes: __attribute__((...)), __declspec(...)
// For use when C23 [[...]] attributes aren't expected
static Token *skip_gnu_attributes(Token *tok)
{
  while (tok && is_attribute_keyword(tok))
  {
    tok = tok->next;
    if (tok && equal(tok, "("))
      tok = skip_balanced(tok, "(", ")");
  }
  return tok;
}

typedef struct
{
  Token **stmts;     // Start token of each deferred statement (dynamic)
  Token **ends;      // End token (the semicolon) (dynamic)
  Token **defer_tok; // The 'defer' keyword token (for error messages) (dynamic)
  int count;
  int capacity;          // Current capacity of the arrays
  bool is_loop;          // true if this scope is a for/while/do loop
  bool is_switch;        // true if this scope is a switch statement
  bool had_control_exit; // true if unconditional break/return/goto/continue was seen
                         // NOTE: only set on switch scopes (by mark_switch_control_exit)
  bool is_conditional;   // true if this scope is an if/while/for block (for tracking conditional control exits)
  bool seen_case_label;  // true if case/default label seen in this switch scope (for zero-init safety)
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
} TypedefTable;

static TypedefTable typedef_table;

static DeferScope *defer_stack = NULL;
static int defer_stack_capacity = 0;

// Control flow parsing state - tracks the transition from control keyword to body
// e.g., "for (int i = 0; i < n; i++)" parens, or "if (cond)" before the { or statement
typedef struct
{
  bool pending;            // True after if/else/for/while/do/switch until we see { or ;
  int paren_depth;         // Track parens to distinguish for(;;) from braceless body
  int brace_depth;         // Track braces inside control parens (compound literals)
  bool parens_just_closed; // True immediately after control parens close (depth 1→0)
} ControlFlowState;

static ControlFlowState control_state = {0};

static inline void control_state_reset(void)
{
  control_state = (ControlFlowState){0};
}

static LabelTable label_table;

// Track statement expression scopes - store the ctx->defer_depth at which each starts
static int *stmt_expr_levels = NULL; // ctx->defer_depth when stmt expr started (dynamic)
static int stmt_expr_capacity = 0;   // capacity of stmt_expr_levels array

// Token emission - Buffered Output Writer
// Uses a memory buffer to batch small writes, significantly faster than per-token fprintf
#define OUT_BUF_SIZE (64 * 1024) // 64KB buffer

typedef struct
{
  FILE *fp;
  char *buf;
  size_t pos;
  size_t cap;
} OutputBuffer;

static OutputBuffer out_buf = {0};
static Token *last_emitted = NULL;

static void out_init(FILE *fp)
{
  out_buf.fp = fp;
  if (!out_buf.buf)
  {
    out_buf.buf = malloc(OUT_BUF_SIZE);
    if (!out_buf.buf)
    {
      fprintf(stderr, "out of memory allocating output buffer\n");
      exit(1);
    }
    out_buf.cap = OUT_BUF_SIZE;
  }
  out_buf.pos = 0;
}

static void out_flush(void)
{
  if (out_buf.pos > 0 && out_buf.fp)
  {
    fwrite(out_buf.buf, 1, out_buf.pos, out_buf.fp);
    out_buf.pos = 0;
  }
}

static void out_close(void)
{
  out_flush();
  if (out_buf.fp)
  {
    fclose(out_buf.fp);
    out_buf.fp = NULL;
  }
}

static void out_char(char c)
{
  if (out_buf.pos >= out_buf.cap)
    out_flush();

  out_buf.buf[out_buf.pos++] = c;
}

#define OUT_LIT(s) out_str(s, sizeof(s) - 1)

static void out_str(const char *s, size_t len)
{
  // If it fits in buffer, copy directly
  if (out_buf.pos + len <= out_buf.cap)
  {
    memcpy(out_buf.buf + out_buf.pos, s, len);
    out_buf.pos += len;
    return;
  }

  // Flush and handle large writes
  out_flush();
  if (len >= out_buf.cap)
  {
    // Write directly for very large strings
    fwrite(s, 1, len, out_buf.fp);
    return;
  }

  memcpy(out_buf.buf, s, len);
  out_buf.pos = len;
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
  OUT_LIT("#line ");
  out_uint(line_no);
  OUT_LIT(" \"");
  out_str(file, strlen(file));
  OUT_LIT("\"\n");
}

// System header tracking (for non-flattened output)
static HashMap system_includes;    // Tracks unique system headers to emit
static char **system_include_list; // Ordered list of includes
static int system_include_capacity = 0;

// Record a system header for later emission
static void record_system_include(const char *path)
{
  if (!path)
    return;
  char *inc = strdup(path);
  if (!inc)
    return;

  // Check if already recorded (using hashmap for O(1) lookup)
  if (hashmap_get(&system_includes, inc, strlen(inc)))
  {
    free(inc);
    return;
  }

  // Add to hashmap
  hashmap_put(&system_includes, inc, strlen(inc), (void *)1);

  // Add to ordered list
  ENSURE_ARRAY_CAP(system_include_list, ctx->system_include_count + 1, system_include_capacity, 32, char *);
  system_include_list[ctx->system_include_count++] = inc;
}

// Collect system headers by detecting actual #include entries (not macro expansions)
static void collect_system_includes(void)
{
  for (int i = 0; i < ctx->input_file_count; i++)
  {
    File *f = ctx->input_files[i];
    // Only record headers that were actually #included (is_include_entry=true)
    // Skip macro expansion sources (is_include_entry=false)
    if (f->is_system && f->is_include_entry)
      record_system_include(f->name);
  }
}

// Emit diagnostic pragmas to suppress warnings from system headers.
// System headers use constructs that trigger various warnings with strict flags.
static void emit_system_header_diag_push(void)
{
  OUT_LIT("#if defined(__GNUC__) || defined(__clang__)\n"
          "#pragma GCC diagnostic push\n"
          "#pragma GCC diagnostic ignored \"-Wredundant-decls\"\n"
          "#pragma GCC diagnostic ignored \"-Wstrict-prototypes\"\n"
          "#pragma GCC diagnostic ignored \"-Wold-style-definition\"\n"
          "#pragma GCC diagnostic ignored \"-Wpedantic\"\n"
          "#pragma GCC diagnostic ignored \"-Wunused-function\"\n"
          "#pragma GCC diagnostic ignored \"-Wunused-parameter\"\n"
          "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n"
          "#pragma GCC diagnostic ignored \"-Wcast-qual\"\n"
          "#pragma GCC diagnostic ignored \"-Wsign-conversion\"\n"
          "#pragma GCC diagnostic ignored \"-Wconversion\"\n"
          "#endif\n");
}

static void emit_system_header_diag_pop(void)
{
  OUT_LIT("#if defined(__GNUC__) || defined(__clang__)\n"
          "#pragma GCC diagnostic pop\n"
          "#endif\n");
}

// Emit collected #include directives with necessary feature test macros
static void emit_system_includes(void)
{
  if (ctx->system_include_count == 0)
    return;

  // Emit feature test macros that prism uses during preprocessing
  // These must come before any system includes to enable GNU/POSIX extensions
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
  hashmap_clear(&system_includes);
  for (int i = 0; i < ctx->system_include_count; i++)
    free(system_include_list[i]);
  free(system_include_list);
  system_include_list = NULL;
  ctx->system_include_count = 0;
  system_include_capacity = 0;
}

static void end_statement_after_semicolon(void)
{
  ctx->at_stmt_start = true;
  ctx->in_for_init = false; // Semicolon ends init clause
  if (control_state.pending && control_state.paren_depth == 0)
  {
    control_state.pending = false;
    ctx->next_scope_is_loop = false;
    ctx->next_scope_is_switch = false;
    ctx->next_scope_is_conditional = false;
  }
}

// Ensure defer_stack has capacity for at least n scopes
static void defer_stack_ensure_capacity(int n)
{
  if (n <= defer_stack_capacity)
    return;
  int new_cap = defer_stack_capacity == 0 ? INITIAL_CAP : defer_stack_capacity * 2;
  while (new_cap < n)
    new_cap *= 2;
  DeferScope *new_stack = realloc(defer_stack, sizeof(DeferScope) * new_cap);
  if (!new_stack)
    error("out of memory growing defer stack");
  // Initialize new scopes
  for (int i = defer_stack_capacity; i < new_cap; i++)
  {
    new_stack[i].stmts = NULL;
    new_stack[i].ends = NULL;
    new_stack[i].defer_tok = NULL;
    new_stack[i].count = 0;
    new_stack[i].capacity = 0;
    new_stack[i].is_loop = false;
    new_stack[i].is_switch = false;
    new_stack[i].is_conditional = false;
    new_stack[i].had_control_exit = false;
  }
  defer_stack = new_stack;
  defer_stack_capacity = new_cap;
}

static void defer_push_scope(void)
{
  defer_stack_ensure_capacity(ctx->defer_depth + 1);
  defer_stack[ctx->defer_depth].count = 0;
  defer_stack[ctx->defer_depth].is_loop = ctx->next_scope_is_loop;
  defer_stack[ctx->defer_depth].is_switch = ctx->next_scope_is_switch;
  defer_stack[ctx->defer_depth].is_conditional = ctx->next_scope_is_conditional;
  defer_stack[ctx->defer_depth].had_control_exit = false;
  defer_stack[ctx->defer_depth].seen_case_label = false;

  if (ctx->next_scope_is_conditional)
    ctx->conditional_block_depth++;

  ctx->next_scope_is_loop = false;
  ctx->next_scope_is_switch = false;
  ctx->next_scope_is_conditional = false;
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

// Ensure a DeferScope has capacity for at least n defers
static void defer_scope_ensure_capacity(DeferScope *scope, int n)
{
  if (n <= scope->capacity)
    return;
  int new_cap = scope->capacity == 0 ? INITIAL_CAP : scope->capacity * 2;
  while (new_cap < n)
    new_cap *= 2;
  scope->stmts = realloc(scope->stmts, sizeof(Token *) * new_cap);
  scope->ends = realloc(scope->ends, sizeof(Token *) * new_cap);
  scope->defer_tok = realloc(scope->defer_tok, sizeof(Token *) * new_cap);
  if (!scope->stmts || !scope->ends || !scope->defer_tok)
    error("out of memory");
  scope->capacity = new_cap;
}

static void defer_add(Token *defer_keyword, Token *start, Token *end)
{
  if (ctx->defer_depth <= 0)
    error_tok(start, "defer outside of any scope");
  DeferScope *scope = &defer_stack[ctx->defer_depth - 1];
  defer_scope_ensure_capacity(scope, scope->count + 1);
  scope->defer_tok[scope->count] = defer_keyword;
  scope->stmts[scope->count] = start;
  scope->ends[scope->count] = end;
  scope->count++;
  // Reset control exit flag - new defer means we need a new control exit
  scope->had_control_exit = false;
}

// Mark that control flow exited (break/return/goto) in the innermost switch scope
// This tells us that defers were properly executed before the case ended
// Only mark if the exit is unconditional (not inside an if/while/for block)
static void mark_switch_control_exit(void)
{
  // Only mark as definite control exit if not inside a conditional context
  // If inside if/while/for (even braceless), the control exit might not be taken at runtime
  // control_state.pending is true for braceless: "if (x) break;"
  // ctx->conditional_block_depth > 0 for braced: "if (x) { break; }"
  if (control_state.pending || ctx->conditional_block_depth > 0)
    return;

  for (int d = ctx->defer_depth - 1; d >= 0; d--)
  {
    if (!defer_stack[d].is_switch)
      continue;

    defer_stack[d].had_control_exit = true;
    return;
  }
}

// Check if we're currently inside a switch scope
static bool inside_switch_scope(void)
{
  for (int d = ctx->defer_depth - 1; d >= 0; d--)
  {
    if (defer_stack[d].is_switch)
      return true;
  }
  return false;
}

// Clear defers at innermost switch scope when hitting case/default
// This is necessary because the transpiler can't track which case was entered at runtime.
// Note: This means defer in case with fallthrough will NOT preserve defers from previous cases.
// For reliable defer behavior in switch, wrap each case body in braces.
// Must clear defers at ALL scopes from current depth down to the switch scope,
// because case labels can appear inside nested blocks (e.g., Duff's device pattern).
static void clear_switch_scope_defers(void)
{
  // First find the switch scope to avoid clearing non-switch scopes
  // if case/default somehow appears outside a switch (malformed input)
  int switch_depth = -1;
  for (int d = ctx->defer_depth - 1; d >= 0; d--)
  {
    if (defer_stack[d].is_switch)
    {
      switch_depth = d;
      break;
    }
  }
  if (switch_depth < 0)
    return; // Not inside a switch — don't clear anything

  // Clear all scopes from current depth down to and including the switch scope
  for (int d = ctx->defer_depth - 1; d >= switch_depth; d--)
  {
    defer_stack[d].count = 0;
    defer_stack[d].had_control_exit = false;
  }
}

// Check if a space is needed between two tokens
static bool needs_space(Token *prev, Token *tok)
{
  if (!prev)
    return false;
  if (tok_at_bol(tok))
    return false; // newline will be emitted
  if (tok_has_space(tok))
    return true;

  // Check if merging would create a different token
  // e.g., "int" + "x" -> "intx", "+" + "+" -> "++"
  char prev_last = prev->loc[prev->len - 1];
  char tok_first = tok->loc[0];

  // Identifier/keyword followed by identifier/keyword or number
  if ((is_identifier_like(prev) || prev->kind == TK_NUM) &&
      (is_identifier_like(tok) || tok->kind == TK_NUM))
    return true;

  // Punctuation that could merge
  if (prev->kind != TK_PUNCT || tok->kind != TK_PUNCT)
    return false;

  // Check common cases: ++ -- << >> && || etc.
  return (prev_last == '+' && tok_first == '+') ||
         (prev_last == '-' && tok_first == '-') ||
         (prev_last == '<' && tok_first == '<') ||
         (prev_last == '>' && tok_first == '>') ||
         (prev_last == '&' && tok_first == '&') ||
         (prev_last == '|' && tok_first == '|') ||
         (prev_last == '=' && tok_first == '=') ||
         (prev_last == '!' && tok_first == '=') ||
         (prev_last == '<' && tok_first == '=') ||
         (prev_last == '>' && tok_first == '=') ||
         (prev_last == '+' && tok_first == '=') ||
         (prev_last == '-' && tok_first == '=') ||
         (prev_last == '*' && tok_first == '=') ||
         (prev_last == '/' && tok_first == '=') ||
         (prev_last == '-' && tok_first == '>') ||
         (prev_last == '#' && tok_first == '#') ||
         (prev_last == '/' && tok_first == '*') ||
         (prev_last == '*' && tok_first == '/');
}

// Check if token is a member access operator (. or ->)
// Used to distinguish keyword usage from struct member names
static bool is_member_access(Token *tok)
{
  return tok && tok->kind == TK_PUNCT && (equal(tok, ".") || equal(tok, "->"));
}

// Check if token is an assignment or compound assignment operator
// Used to distinguish variable assignments from keyword statements
static bool is_assignment_op(Token *tok)
{
  if (!tok)
    return false;

  return equal(tok, "=") || equal(tok, "+=") || equal(tok, "-=") ||
         equal(tok, "*=") || equal(tok, "/=") || equal(tok, "%=") ||
         equal(tok, "&=") || equal(tok, "|=") || equal(tok, "^=") ||
         equal(tok, "<<=") || equal(tok, ">>=") || equal(tok, "++") ||
         equal(tok, "--") || equal(tok, "[");
}

// Check if 'tok' is inside a __attribute__((...)) or __declspec(...) context.
// This prevents 'defer' from being recognized as a keyword when it appears
// as a function name inside cleanup() or similar attributes.
// Uses forward-looking heuristic: if we see unbalanced ')' before ';' or '{',
// we're likely inside a parenthesized context (attribute argument list).
static bool is_inside_attribute(Token *tok)
{
  if (!last_emitted)
    return false;

  // Quick check: defer in cleanup(defer) would follow '(' or ','
  if (!equal(last_emitted, "(") && !equal(last_emitted, ","))
    return false;

  // Forward check: from tok, count parens until we hit ';' or EOF
  // If we see unbalanced ')' first, we're inside some paren context
  int paren_depth = 0;
  for (Token *t = tok; t && t->kind != TK_EOF; t = t->next)
  {
    if (equal(t, "("))
      paren_depth++;
    else if (equal(t, ")"))
    {
      if (--paren_depth < 0)
        return true; // Unmatched ')' - inside attribute parens
    }
    else if (equal(t, ";") || equal(t, "{"))
      break;
  }

  return false;
}

// Emit a single token with appropriate spacing
static void emit_tok(Token *tok)
{
  // Skip system header include content when not flattening
  // But keep macro expansions (is_include_entry=false means it's a macro, not an include)
  File *f = tok_file(tok);
  if (!ctx->feature_flatten_headers && f && f->is_system && f->is_include_entry)
    return;

  // Check if we need a #line directive BEFORE emitting the token
  bool need_line_directive = false;
  char *tok_fname = NULL;
  int line_no = tok_line_no(tok);

  // Skip line directive handling for synthetic tokens (line_no == -1)
  if (ctx->emit_line_directives && f && line_no > 0)
  {
    tok_fname = f->display_name ? f->display_name : f->name;
    bool file_changed = (ctx->last_filename != tok_fname &&
                         (!ctx->last_filename || !tok_fname || strcmp(ctx->last_filename, tok_fname) != 0));
    bool system_changed = (f->is_system != ctx->last_system_header);
    bool line_jumped = (line_no != ctx->last_line_no && line_no != ctx->last_line_no + 1);
    need_line_directive = file_changed || line_jumped || system_changed;
  }

  // Handle newlines and spacing
  if (tok_at_bol(tok))
  {
    out_char('\n');
    // Emit #line directive on new line if needed
    if (need_line_directive)
    {
      out_line(line_no, tok_fname ? tok_fname : "unknown");
      ctx->last_line_no = line_no;
      ctx->last_filename = tok_fname;
      ctx->last_system_header = f->is_system;
    }
    else if (ctx->emit_line_directives && f && line_no > 0 && line_no > ctx->last_line_no)
    {
      ctx->last_line_no = line_no;
    }
  }
  else
  {
    // Not at beginning of line - emit #line before token if file/line changed significantly
    if (need_line_directive)
    {
      out_char('\n');
      out_line(line_no, tok_fname ? tok_fname : "unknown");
      ctx->last_line_no = line_no;
      ctx->last_filename = tok_fname;
      ctx->last_system_header = f->is_system;
    }
    else if (needs_space(last_emitted, tok))
    {
      out_char(' ');
    }
  }

  // Handle preprocessor directives (e.g., #pragma) - emit verbatim
  if (tok->kind == TK_PREP_DIR)
  {
    // Preprocessor directives should be at BOL
    if (!tok_at_bol(tok))
      out_char('\n');
    out_str(tok->loc, tok->len);
    last_emitted = tok;
    return;
  }

  // Handle C23 extended float suffixes (F128, F64, F32, F16, BF16)
  if (tok->kind == TK_NUM && (tok->flags & TF_IS_FLOAT))
  {
    const char *replacement;
    int suffix_len = get_extended_float_suffix(tok->loc, tok->len, &replacement);
    if (suffix_len > 0)
    {
      out_str(tok->loc, tok->len - suffix_len);
      if (replacement)
        out_str(replacement, strlen(replacement));
      last_emitted = tok;
      return;
    }
  }

  // Handle digraphs - translate to canonical form
  const char *equiv = digraph_equiv(tok);
  if (equiv)
  {
    out_str(equiv, strlen(equiv));
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

// Emit defers with boundary control:
// DEFER_SCOPE=current only, DEFER_ALL=all scopes, DEFER_BREAK=stop at loop/switch, DEFER_CONTINUE=stop at loop
typedef enum
{
  DEFER_SCOPE,
  DEFER_ALL,
  DEFER_BREAK,
  DEFER_CONTINUE
} DeferEmitMode;

static void emit_defers(DeferEmitMode mode)
{
  if (ctx->defer_depth <= 0)
    return;

  int start = ctx->defer_depth - 1;
  int end = (mode == DEFER_SCOPE) ? ctx->defer_depth - 1 : 0;

  for (int d = start; d >= end; d--)
  {
    DeferScope *scope = &defer_stack[d];
    for (int i = scope->count - 1; i >= 0; i--)
    {
      out_char(' ');
      emit_range(scope->stmts[i], scope->ends[i]);
      out_char(';');
    }

    if (mode == DEFER_SCOPE)
      break;
    if (mode == DEFER_BREAK && (scope->is_loop || scope->is_switch))
      break;
    if (mode == DEFER_CONTINUE && scope->is_loop)
      break;
  }
}

#define emit_scope_defers() emit_defers(DEFER_SCOPE)
#define emit_all_defers() emit_defers(DEFER_ALL)
#define emit_break_defers() emit_defers(DEFER_BREAK)
#define emit_continue_defers() emit_defers(DEFER_CONTINUE)

// Check if there are any active defers
static bool has_active_defers(void)
{
  for (int d = 0; d < ctx->defer_depth; d++)
    if (defer_stack[d].count > 0)
      return true;
  return false;
}

// Check if break/continue needs to emit defers
// include_switch: true for break (stops at loop OR switch), false for continue (stops at loop only)
static bool control_flow_has_defers(bool include_switch)
{
  bool found_boundary = false;
  bool found_defers = false;

  for (int d = ctx->defer_depth - 1; d >= 0; d--)
  {
    if (defer_stack[d].count > 0)
      found_defers = true;

    if (defer_stack[d].is_loop || (include_switch && defer_stack[d].is_switch))
    {
      found_boundary = true;
      break;
    }
  }

  return found_boundary && found_defers;
}

static void label_table_add(char *name, int name_len, int scope_depth)
{
  ENSURE_ARRAY_CAP(label_table.labels, label_table.count + 1, label_table.capacity, INITIAL_CAP, LabelInfo);
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
  if (typedef_table.entries)
  {
    free(typedef_table.entries);
    typedef_table.entries = NULL;
  }
  typedef_table.count = 0;
  typedef_table.capacity = 0;
  hashmap_clear(&typedef_table.name_map);
}

// Helper to get current index for a name from the hash map (-1 if not found)
static int typedef_get_index(char *name, int len)
{
  void *val = hashmap_get(&typedef_table.name_map, name, len);
  return val ? (int)(intptr_t)val - 1 : -1;
}

// Helper to update hash map with new index for a name
static void typedef_set_index(char *name, int len, int index)
{
  // Store index+1 so that 0 (NULL) means "not found"
  hashmap_put(&typedef_table.name_map, name, len, (void *)(intptr_t)(index + 1));
}

typedef enum
{
  TDK_TYPEDEF,
  TDK_SHADOW,
  TDK_ENUM_CONST,
  TDK_VLA_VAR // VLA variable (not typedef, but actual VLA array variable)
} TypedefKind;

static void typedef_add_entry(char *name, int len, int scope_depth, TypedefKind kind, bool is_vla)
{
  ENSURE_ARRAY_CAP(typedef_table.entries, typedef_table.count + 1, typedef_table.capacity, INITIAL_CAP, TypedefEntry);
  int new_index = typedef_table.count++;
  TypedefEntry *e = &typedef_table.entries[new_index];
  e->name = name;
  e->len = len;
  e->scope_depth = scope_depth;
  e->is_vla = (kind == TDK_TYPEDEF || kind == TDK_VLA_VAR) ? is_vla : false;
  e->is_shadow = (kind == TDK_SHADOW || kind == TDK_ENUM_CONST); // Enum consts also shadow typedefs
  e->is_enum_const = (kind == TDK_ENUM_CONST);
  e->prev_index = typedef_get_index(name, len);
  typedef_set_index(name, len, new_index);
}

#define typedef_add(name, len, depth, is_vla) typedef_add_entry(name, len, depth, TDK_TYPEDEF, is_vla)
#define typedef_add_shadow(name, len, depth) typedef_add_entry(name, len, depth, TDK_SHADOW, false)
#define typedef_add_enum_const(name, len, depth) typedef_add_entry(name, len, depth, TDK_ENUM_CONST, false)
#define typedef_add_vla_var(name, len, depth) typedef_add_entry(name, len, depth, TDK_VLA_VAR, true)

// Called when exiting a scope - removes typedefs defined at that depth
static void typedef_pop_scope(int scope_depth)
{
  while (typedef_table.count > 0 && typedef_table.entries[typedef_table.count - 1].scope_depth == scope_depth)
  {
    TypedefEntry *e = &typedef_table.entries[typedef_table.count - 1];
    // Restore hash map to point to previous entry (or remove if none)
    if (e->prev_index >= 0)
      typedef_set_index(e->name, e->len, e->prev_index);
    else
      hashmap_delete2(&typedef_table.name_map, e->name, e->len);
    typedef_table.count--;
  }
}

// Forward declarations
static bool is_known_typedef(Token *tok);
static bool is_type_keyword(Token *tok);

// Parse enum body and register constants as shadows for any matching typedefs.
// Enum constants are visible in the enclosing scope, so they shadow typedefs.
// Also register all enum constants for is_const_array_size detection.
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
          if (equal(tok, "(") || equal(tok, "[") || equal(tok, "{"))
            depth++;
          else if (equal(tok, ")") || equal(tok, "]") || equal(tok, "}"))
          {
            if (depth > 0)
              depth--;
            else if (equal(tok, "}"))
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

// Check if token is a known typedef (search most recent first for shadowing)
// Returns false if the most recent entry with this name is a shadow (variable)
static bool is_known_typedef(Token *tok)
{
  if (!is_identifier_like(tok))
    return false;
  int idx = typedef_get_index(tok->loc, tok->len);
  if (idx < 0)
    return false;
  TypedefEntry *e = &typedef_table.entries[idx];
  return !e->is_shadow && !e->is_enum_const;
}

// Check if token is a known shadow variable (variable that shadows a typedef)
// Used to prevent looks_like_system_typedef from matching user variables like "int size_t = 10;"
static bool is_known_shadow(Token *tok)
{
  if (!is_identifier_like(tok))
    return false;
  int idx = typedef_get_index(tok->loc, tok->len);
  if (idx < 0)
    return false;
  return typedef_table.entries[idx].is_shadow;
}

// Check if token is a known VLA typedef (search most recent first for shadowing)
static bool is_vla_typedef(Token *tok)
{
  if (!is_identifier_like(tok))
    return false;
  int idx = typedef_get_index(tok->loc, tok->len);
  if (idx < 0)
    return false;
  TypedefEntry *e = &typedef_table.entries[idx];
  if (e->is_shadow)
    return false;
  return e->is_vla;
}

// Check if token is a known enum constant (compile-time constant)
static bool is_known_enum_const(Token *tok)
{
  if (!is_identifier_like(tok))
    return false;
  int idx = typedef_get_index(tok->loc, tok->len);
  if (idx < 0)
    return false;
  return typedef_table.entries[idx].is_enum_const;
}

// Check if token is struct/union/enum keyword
static bool is_sue_keyword(Token *tok)
{
  return equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum");
}

// Given a struct/union/enum keyword, find its opening brace if it has a body.
// Handles: "struct {", "struct name {", "struct __attribute__((packed)) name {"
// Returns the '{' token, or NULL if no body (e.g., "struct name;" or "struct name var;")
static Token *find_struct_body_brace(Token *tok)
{
  Token *t = tok->next;
  while (t && (t->kind == TK_IDENT || is_attribute_keyword(t) ||
               equal(t, "_Alignas") || equal(t, "alignas")))
  {
    if (is_attribute_keyword(t) || equal(t, "_Alignas") || equal(t, "alignas"))
    {
      t = t->next;
      if (t && equal(t, "("))
        t = skip_balanced(t, "(", ")");
    }
    else
    {
      t = t->next;
    }
  }
  return (t && equal(t, "{")) ? t : NULL;
}

// Scan a function body for labels and record their scope depths
// Also detects setjmp/longjmp/pthread_exit/vfork and inline asm usage
// tok should point to the opening '{' of the function body
static void scan_labels_in_function(Token *tok)
{
  label_table.count = 0;
  hashmap_clear(&label_table.name_map); // Clear for O(1) lookups
  ctx->current_func_has_setjmp = false; // Reset for new function
  ctx->current_func_has_asm = false;    // Reset for new function
  ctx->current_func_has_vfork = false;  // Reset for new function
  if (!tok || !equal(tok, "{"))
    return;

  // Start at depth 1 to align with ctx->defer_depth (which is 1 inside function body)
  int depth = 1;
  int local_struct_depth = 0; // Track nesting inside struct/union/enum bodies
  Token *prev = NULL;
  tok = tok->next; // Skip opening brace

  while (tok && tok->kind != TK_EOF)
  {
    // Track struct/union/enum bodies to skip bitfield declarations
    if (is_sue_keyword(tok))
    {
      Token *brace = find_struct_body_brace(tok);
      if (brace)
      {
        while (tok != brace)
        {
          prev = tok;
          tok = tok->next;
        }
        local_struct_depth++;
        depth++;
        prev = tok;
        tok = tok->next;
        continue;
      }
    }

    if (equal(tok, "{"))
    {
      depth++;
      prev = tok;
      tok = tok->next;
      continue;
    }
    if (equal(tok, "}"))
    {
      if (depth == 1)
        break; // End of function
      if (local_struct_depth > 0)
        local_struct_depth--;
      depth--;
      prev = tok;
      tok = tok->next;
      continue;
    }

    // Detect setjmp/longjmp/sigsetjmp/siglongjmp usage
    // Also detect pthread_exit which bypasses cleanup like longjmp
    if (tok->kind == TK_IDENT &&
        (equal(tok, "setjmp") || equal(tok, "longjmp") ||
         equal(tok, "_setjmp") || equal(tok, "_longjmp") ||
         equal(tok, "sigsetjmp") || equal(tok, "siglongjmp") ||
         equal(tok, "pthread_exit")))
      ctx->current_func_has_setjmp = true;

    // Detect vfork which has unpredictable control flow
    if (tok->kind == TK_IDENT && equal(tok, "vfork"))
      ctx->current_func_has_vfork = true;

    // Detect inline asm which may contain hidden jumps
    if (tok->kind == TK_KEYWORD && (equal(tok, "asm") || equal(tok, "__asm__") || equal(tok, "__asm")))
      ctx->current_func_has_asm = true;

    // Skip _Generic(...) - type associations inside look like labels (Type: expr)
    if (tok->kind == TK_KEYWORD && equal(tok, "_Generic"))
    {
      prev = tok;
      tok = tok->next;
      if (tok && equal(tok, "("))
      {
        tok = skip_balanced(tok, "(", ")");
        prev = NULL; // Reset prev since we skipped a complex expression
      }
      continue;
    }

    // Check for label: identifier followed by ':' (but not ::)
    // Also handle labels with attributes: identifier __attribute__((...)) :
    // Filter out: ternary operator, switch cases, bitfields
    // Also handle 'defer' keyword used as a label (defer:) - valid if user isn't using defer feature
    if (is_identifier_like(tok))
    {
      // Look ahead for colon, skipping any __attribute__((...)) sequences
      Token *t = skip_gnu_attributes(tok->next);

      if (t && equal(t, ":"))
      {
        Token *colon = t;
        // Make sure it's not :: (C++ scope resolution)
        bool is_scope_resolution = colon->next && equal(colon->next, ":");
        bool is_ternary = prev && equal(prev, "?");
        bool is_switch_case = prev && (equal(prev, "case") || equal(prev, "default"));
        bool is_bitfield = local_struct_depth > 0;

        if (!is_scope_resolution && !is_ternary && !is_switch_case && !is_bitfield)
          label_table_add(tok->loc, tok->len, depth);
      }
    }

    prev = tok;
    tok = tok->next;
  }
}

// Emit defers for goto - from current scope down to target scope (inclusive)
// We emit defers for scopes we're EXITING, not the scope we're jumping TO
static void emit_goto_defers(int target_depth)
{
  for (int d = ctx->defer_depth - 1; d >= target_depth; d--)
  {
    DeferScope *scope = &defer_stack[d];
    for (int i = scope->count - 1; i >= 0; i--)
    {
      out_char(' ');
      emit_range(scope->stmts[i], scope->ends[i]);
      out_char(';');
    }
  }
}

// Check if goto needs defers (jumping out of scopes with defers)
static bool goto_has_defers(int target_depth)
{
  for (int d = ctx->defer_depth - 1; d >= target_depth; d--)
  {
    if (defer_stack[d].count > 0)
      return true;
  }
  return false;
}

// Modes for goto_skips_check - what to look for between goto and label
typedef enum
{
  GOTO_CHECK_DEFER, // Look for defer statements
  GOTO_CHECK_DECL   // Look for variable declarations
} GotoCheckMode;

// Check if a forward goto would skip over defer statements or variable declarations.
// Returns the token that would be skipped, or NULL if safe.
//
// Key distinction:
// - INVALID: goto jumps INTO a block, landing AFTER a defer/decl inside that block
//   Example: goto inner; { defer X; inner: ... } -- X would run but wasn't registered
// - VALID: goto jumps OVER an entire block containing a defer/decl
//   Example: goto done; { defer X; ... } done: -- we skip the whole block, defer never registered
//
// The rule: if we find the label BEFORE exiting the scope containing the item, it's invalid.
static Token *goto_skips_check(Token *goto_tok, char *label_name, int label_len, GotoCheckMode mode)
{
  // For DECL mode, only check when zero-init is enabled
  if (mode == GOTO_CHECK_DECL && !ctx->feature_zeroinit)
    return NULL;

  // Scan forward from goto to find the label
  Token *tok = goto_tok->next->next; // skip 'goto' and label name
  if (tok && equal(tok, ";"))
    tok = tok->next;

  int depth = 0;
  int local_struct_depth = 0;
  Token *active_item = NULL;  // Most recently seen item that's still "in scope"
  int active_item_depth = -1; // Depth at which active_item was found
  Token *prev = NULL;
  bool is_stmt_start = true;   // Only needed for DECL mode
  bool is_in_for_init = false; // Track if we're in for loop initialization clause

  while (tok && tok->kind != TK_EOF)
  {
    // Track for loops to detect declarations in initialization clause
    if (mode == GOTO_CHECK_DECL && tok->kind == TK_KEYWORD && equal(tok, "for"))
    {
      prev = tok;
      tok = tok->next;
      if (tok && equal(tok, "("))
      {
        // We're entering the for loop initialization
        is_in_for_init = true;
        prev = tok;
        tok = tok->next;
        is_stmt_start = true; // Treat start of for init as statement start
        continue;
      }
      is_stmt_start = false;
      continue;
    }

    // Track struct/union/enum bodies to skip bitfield declarations
    if (is_sue_keyword(tok))
    {
      Token *brace = find_struct_body_brace(tok);
      if (brace)
      {
        while (tok != brace)
        {
          prev = tok;
          tok = tok->next;
        }
        local_struct_depth++;
        depth++;
        prev = tok;
        tok = tok->next;
        is_stmt_start = false;
        continue;
      }
    }

    if (equal(tok, "{"))
    {
      depth++;
      prev = tok;
      tok = tok->next;
      is_stmt_start = true;
      continue;
    }
    if (equal(tok, "}"))
    {
      // Exiting a scope - if we exit past where we found the item, clear it
      if (active_item && depth <= active_item_depth)
      {
        active_item = NULL;
        active_item_depth = -1;
      }
      if (local_struct_depth > 0)
        local_struct_depth--;
      if (depth == 0)
        break;
      depth--;
      prev = tok;
      tok = tok->next;
      is_stmt_start = true;
      continue;
    }
    if (equal(tok, ";"))
    {
      is_stmt_start = true;
      // If we're in for loop init, semicolon ends the init clause
      if (is_in_for_init)
        is_in_for_init = false;
      prev = tok;
      tok = tok->next;
      continue;
    }

    // Skip _Generic(...) - type associations inside look like labels
    if (tok->kind == TK_KEYWORD && equal(tok, "_Generic"))
    {
      prev = tok;
      tok = tok->next;
      if (tok && equal(tok, "("))
      {
        tok = skip_balanced(tok, "(", ")");
        prev = NULL;
      }
      is_stmt_start = false;
      continue;
    }

    // Mode-specific: track defers or declarations
    if (mode == GOTO_CHECK_DEFER)
    {
      // Track defers we pass over (skip if preceded by member access)
      // Also skip if this is a variable declaration: "int defer;" not "defer stmt;"
      // Check if preceded by a type keyword - if so, it's a variable name, not defer statement
      bool is_variable_name = prev && (is_type_keyword(prev) || equal(prev, "*") ||
                                       equal(prev, "const") || equal(prev, "volatile") ||
                                       equal(prev, "restrict") || equal(prev, "__restrict") ||
                                       equal(prev, ",")); // Also check comma for multi-declarators

      // Check for defer statement (not a label named "defer:")
      if (tok->kind == TK_KEYWORD && equal(tok, "defer") &&
          !equal(tok->next, ":") && // Distinguish from label named "defer:"
          !is_member_access(prev) && !is_variable_name && !is_assignment_op(tok->next))
      {
        if (!active_item || depth <= active_item_depth)
        {
          active_item = tok;
          active_item_depth = depth;
        }
      }
    }
    else if (mode == GOTO_CHECK_DECL && (is_stmt_start || is_in_for_init) && local_struct_depth == 0)
    {
      // Detect variable declarations at statement start OR in for loop initialization
      Token *decl_start = tok;
      Token *t = tok;

      // Check for 'raw' keyword - if present, skip checking this declaration
      // The 'raw' keyword explicitly opts out of zero-init, so jumping over it is allowed
      bool has_raw = false;
      if (equal(t, "raw"))
      {
        has_raw = true;
        t = t->next;
      }

      // Skip extern/typedef - these don't create initialized variables
      if (!equal(t, "extern") && !equal(t, "typedef"))
      {
        // Skip qualifiers
        while (t && (equal(t, "const") || equal(t, "volatile") || equal(t, "static") ||
                     equal(t, "auto") || equal(t, "register") || equal(t, "_Atomic") ||
                     equal(t, "restrict") || equal(t, "__restrict") || equal(t, "__restrict__")))
          t = t->next;

        // Check for type keyword
        if (t && (equal(t, "int") || equal(t, "char") || equal(t, "short") ||
                  equal(t, "long") || equal(t, "float") || equal(t, "double") ||
                  equal(t, "void") || equal(t, "signed") || equal(t, "unsigned") ||
                  equal(t, "_Bool") || equal(t, "bool") ||
                  equal(t, "struct") || equal(t, "union") || equal(t, "enum") ||
                  is_known_typedef(t)))
        {
          // Skip past the type
          if (equal(t, "struct") || equal(t, "union") || equal(t, "enum"))
          {
            t = t->next;
            if (t && t->kind == TK_IDENT)
              t = t->next;
            if (t && equal(t, "{"))
            {
              int bd = 1;
              t = t->next;
              while (t && bd > 0)
              {
                if (equal(t, "{"))
                  bd++;
                else if (equal(t, "}"))
                  bd--;
                t = t->next;
              }
            }
          }
          else
          {
            while (t && (equal(t, "int") || equal(t, "char") || equal(t, "short") ||
                         equal(t, "long") || equal(t, "float") || equal(t, "double") ||
                         equal(t, "signed") || equal(t, "unsigned") ||
                         is_known_typedef(t)))
              t = t->next;
          }

          // Skip pointers and qualifiers
          while (t && (equal(t, "*") || equal(t, "const") || equal(t, "volatile") ||
                       equal(t, "restrict") || equal(t, "__restrict") || equal(t, "__restrict__")))
            t = t->next;

          // Should be at identifier now - and NOT followed by '(' (that's a function)
          if (t && t->kind == TK_IDENT && t->next && !equal(t->next, "("))
          {
            // Only track as active_item if NOT marked with 'raw'
            // 'raw' explicitly opts out of zero-init, so jumping over it is safe
            if (!has_raw && (!active_item || depth <= active_item_depth))
            {
              active_item = decl_start;
              active_item_depth = depth;
            }
          }
        }
      }
    }

    // Found the label? Check with proper filtering
    if (tok->kind == TK_IDENT && tok->len == label_len &&
        !memcmp(tok->loc, label_name, label_len))
    {
      // Look ahead for colon, skipping any __attribute__((...)) sequences
      Token *t = skip_gnu_attributes(tok->next);

      if (t && equal(t, ":"))
      {
        Token *colon = t;
        bool is_scope_resolution = colon->next && equal(colon->next, ":");
        bool is_ternary = prev && equal(prev, "?");
        bool is_switch_case = prev && (equal(prev, "case") || equal(prev, "default"));
        bool is_bitfield = local_struct_depth > 0;

        if (!is_scope_resolution && !is_ternary && !is_switch_case && !is_bitfield)
          return active_item; // Label found - return any active item we're skipping
      }
    }

    is_stmt_start = false;
    prev = tok;
    tok = tok->next;
  }

  return NULL;
}

// Transpiler
static Token *skip_to_semicolon(Token *tok)
{
  int depth = 0;
  while (tok->kind != TK_EOF)
  {
    if (equal(tok, "(") || equal(tok, "[") || equal(tok, "{"))
      depth++;
    else if (equal(tok, ")") || equal(tok, "]") || equal(tok, "}"))
      depth--;
    else if (depth == 0 && equal(tok, ";"))
      return tok;
    tok = tok->next;
  }
  return tok;
}

static Token *skip_balanced(Token *tok, char *open, char *close)
{
  int depth = 1;
  tok = tok->next; // skip opening
  while (tok->kind != TK_EOF && depth > 0)
  {
    if (equal(tok, open))
      depth++;
    else if (equal(tok, close))
      depth--;
    tok = tok->next;
  }
  return tok;
}

// Check if token starts a void function declaration
// Handles: void func(, static void func(, __attribute__((...)) void func(, etc.
static bool is_void_function_decl(Token *tok)
{
  // Skip storage class specifiers and attributes
  while (tok && (equal(tok, "static") || equal(tok, "inline") || equal(tok, "extern") ||
                 equal(tok, "_Noreturn") || equal(tok, "__inline") || equal(tok, "__inline__") ||
                 equal(tok, "typedef") || is_attribute_keyword(tok)))
  {
    if (is_attribute_keyword(tok))
      tok = skip_gnu_attributes(tok);
    else
      tok = tok->next;
  }

  // Must be at 'void' now
  if (!tok || !equal(tok, "void"))
    return false;

  tok = tok->next;

  // void* is not a void-returning function
  if (tok && equal(tok, "*"))
    return false;

  // Skip attributes and qualifiers after void
  while (tok && (equal(tok, "const") || equal(tok, "volatile") ||
                 equal(tok, "__restrict") || equal(tok, "__restrict__") ||
                 is_attribute_keyword(tok)))
  {
    if (is_attribute_keyword(tok))
      tok = skip_gnu_attributes(tok);
    else
      tok = tok->next;
  }

  // Should be at function name followed by (
  return tok && tok->kind == TK_IDENT && tok->next && equal(tok->next, "(");
}

// Skip the base type in a typedef (everything before the declarator)
static Token *scan_typedef_base_type(Token *tok)
{
  // Skip qualifiers: const, volatile, restrict, _Atomic
  // Handle _Atomic(type) specifier form specially
  while (tok && (equal(tok, "const") || equal(tok, "volatile") ||
                 equal(tok, "restrict") || equal(tok, "_Atomic") ||
                 equal(tok, "__const") || equal(tok, "__const__") ||
                 equal(tok, "__volatile") || equal(tok, "__volatile__") ||
                 equal(tok, "__restrict") || equal(tok, "__restrict__")))
  {
    // Handle _Atomic(type) specifier form - skip the parenthesized type
    if (equal(tok, "_Atomic") && tok->next && equal(tok->next, "("))
    {
      tok = tok->next;                    // Skip _Atomic
      tok = skip_balanced(tok, "(", ")"); // Skip (type)
    }
    else
    {
      tok = tok->next;
    }
  }

  // Skip attributes
  tok = skip_gnu_attributes(tok);

  // Handle struct/union/enum
  if (tok && is_sue_keyword(tok))
  {
    tok = tok->next;

    // Skip attributes after keyword
    tok = skip_gnu_attributes(tok);

    // Skip optional tag name
    if (tok && tok->kind == TK_IDENT)
      tok = tok->next;

    // Skip body { ... } if present
    if (tok && equal(tok, "{"))
      tok = skip_balanced(tok, "{", "}");

    return tok;
  }

  // Skip type specifiers and existing typedef names
  while (tok && tok->kind != TK_EOF &&
         (is_type_keyword(tok) || is_known_typedef(tok) ||
          equal(tok, "signed") || equal(tok, "unsigned") ||
          equal(tok, "__signed__") || equal(tok, "__signed")))
  {
    tok = tok->next;
    // Skip attributes between type keywords
    tok = skip_gnu_attributes(tok);
  }

  return tok;
}

// Extract the typedef name from a declarator, advance *tokp past the declarator
static Token *scan_typedef_name(Token **tokp)
{
  Token *tok = *tokp;

  // Skip pointers and qualifiers
  while (tok && (equal(tok, "*") || equal(tok, "const") || equal(tok, "volatile") ||
                 equal(tok, "restrict") || equal(tok, "_Atomic") ||
                 equal(tok, "__const") || equal(tok, "__const__") ||
                 equal(tok, "__volatile") || equal(tok, "__volatile__") ||
                 equal(tok, "__restrict") || equal(tok, "__restrict__")))
    tok = tok->next;

  // Skip attributes on pointer
  tok = skip_gnu_attributes(tok);

  // Case 1: Parenthesized declarator - (*name), (*name[N]), (*name)(args)
  if (tok && equal(tok, "("))
  {
    tok = tok->next;

    // Skip inner pointers and qualifiers
    while (tok && (equal(tok, "*") || equal(tok, "const") || equal(tok, "volatile") ||
                   equal(tok, "restrict") || equal(tok, "_Atomic")))
      tok = tok->next;

    tok = skip_gnu_attributes(tok);

    if (tok && is_identifier_like(tok))
    {
      Token *name = tok;
      tok = tok->next;

      // Skip array dims inside parens: (*name[N])
      while (tok && equal(tok, "["))
        tok = skip_balanced(tok, "[", "]");

      // Skip closing paren
      if (tok && equal(tok, ")"))
        tok = tok->next;

      // Skip trailing array dims or function params
      while (tok && equal(tok, "["))
        tok = skip_balanced(tok, "[", "]");
      if (tok && equal(tok, "("))
        tok = skip_balanced(tok, "(", ")");

      *tokp = tok;
      return name;
    }

    // Nested/complex - try to skip to matching ) and continue
    int depth = 1;
    while (tok && tok->kind != TK_EOF && depth > 0)
    {
      if (equal(tok, "("))
        depth++;
      else if (equal(tok, ")"))
        depth--;
      tok = tok->next;
    }
    *tokp = tok;
    return NULL; // Couldn't extract - graceful failure
  }

  // Case 2: Direct declarator - name, name[N], name(args)
  if (tok && is_identifier_like(tok))
  {
    Token *name = tok;
    tok = tok->next;

    // Skip array dimensions
    while (tok && equal(tok, "["))
      tok = skip_balanced(tok, "[", "]");

    // Skip function params (for func types without outer parens)
    if (tok && equal(tok, "("))
      tok = skip_balanced(tok, "(", ")");

    *tokp = tok;
    return name;
  }

  *tokp = tok;
  return NULL;
}

// Forward declarations (used by struct_body_contains_vla and array_size_is_vla)
static bool array_size_is_vla(Token *open_bracket, bool strict_mode);
static bool has_manual_offsetof_pattern(Token *start, Token *end);
static bool looks_like_system_typedef(Token *tok);
static bool is_const_expr_operator(Token *tok);
static bool is_const_identifier(Token *tok);

// Unified VLA array size checker
// strict_mode=true: Treat offsetof patterns as VLA (for zero-init safety)
// strict_mode=false: Treat offsetof patterns as constant (for struct validation)
static bool array_size_is_vla(Token *open_bracket, bool strict_mode)
{
  Token *tok = open_bracket->next;
  int depth = 1;

  // Find closing bracket (needed for offsetof pattern check)
  Token *close_bracket = NULL;
  if (!strict_mode)
  {
    Token *t = open_bracket->next;
    int d = 1;
    while (t && t->kind != TK_EOF && d > 0)
    {
      if (equal(t, "["))
        d++;
      else if (equal(t, "]"))
      {
        d--;
        if (d == 0)
          close_bracket = t;
      }
      t = t->next;
    }
    // In lenient mode, manual offsetof patterns are constant (not VLA)
    if (close_bracket && has_manual_offsetof_pattern(open_bracket->next, close_bracket))
      return false;
  }

  bool prev_was_member_access = false;
  while (tok && tok->kind != TK_EOF && depth > 0)
  {
    if (equal(tok, "["))
    {
      depth++;
      tok = tok->next;
      continue;
    }
    if (equal(tok, "]"))
    {
      depth--;
      tok = tok->next;
      continue;
    }

    // In strict mode, offsetof keywords and patterns indicate potential VLA
    if (strict_mode)
    {
      if (equal(tok, "offsetof") || equal(tok, "__builtin_offsetof"))
        return true;
      // Detect manual offsetof pattern: (char*)
      if (equal(tok, "(") && tok->next && equal(tok->next, "char") &&
          tok->next->next && equal(tok->next->next, "*"))
        return true;
    }

    // sizeof, _Alignof, alignof - handle specially
    if (equal(tok, "sizeof") || equal(tok, "_Alignof") || equal(tok, "alignof"))
    {
      bool is_sizeof = equal(tok, "sizeof");
      tok = tok->next;
      if (tok && equal(tok, "("))
      {
        Token *arg_start = tok->next;

        // Find matching closing paren
        int paren_depth = 1;
        Token *t = tok->next;
        while (t && t->kind != TK_EOF && paren_depth > 0)
        {
          if (equal(t, "("))
            paren_depth++;
          else if (equal(t, ")"))
            paren_depth--;
          t = t->next;
        }
        Token *arg_end = t;

        if (strict_mode && is_sizeof)
        {
          // sizeof(VLA_Type) - check for VLA typedef
          if (arg_start && is_vla_typedef(arg_start))
            return true;

          // sizeof(identifier) where identifier is a simple variable name is
          // always a compile-time constant in C. Even if the variable is a VLA,
          // the sizeof is evaluated at the point of the VLA declaration, but
          // using sizeof(vla_var) in an array bound is extremely rare.
          // We only flag sizeof(VLA_typedef) as runtime (checked above).
          // Note: sizeof(identifier) was previously flagged as VLA which caused
          // false positives like: int x; char buf[sizeof(x)]; // wrongly treated as VLA

          // Check for VLA patterns inside the sizeof argument (e.g., sizeof(int[n]))
          for (Token *st = arg_start; st && st != arg_end && st->kind != TK_EOF; st = st->next)
          {
            if (equal(st, "[") && array_size_is_vla(st, true))
              return true;
          }
        }

        tok = arg_end;
      }
      prev_was_member_access = false;
      continue;
    }

    // In lenient mode, skip offsetof entirely (already handled above)
    if (!strict_mode && (equal(tok, "offsetof") || equal(tok, "__builtin_offsetof")))
    {
      tok = tok->next;
      if (tok && equal(tok, "("))
        tok = skip_balanced(tok, "(", ")");
      prev_was_member_access = false;
      continue;
    }

    // Track member access operators
    if (equal(tok, "->") || equal(tok, "."))
    {
      prev_was_member_access = true;
      tok = tok->next;
      continue;
    }

    // Allow constant expression operators
    if (is_const_expr_operator(tok))
    {
      prev_was_member_access = false;
      tok = tok->next;
      continue;
    }

    // Check identifiers
    if (tok->kind == TK_IDENT)
    {
      // Member names after -> or . are compile-time constant
      if (prev_was_member_access)
      {
        prev_was_member_access = false;
        tok = tok->next;
        continue;
      }

      // Constant identifiers (enums, typedefs, type keywords, system types)
      if (is_const_identifier(tok))
      {
        tok = tok->next;
        continue;
      }

      // Non-constant identifier found - this is a VLA
      return true;
    }

    prev_was_member_access = false;
    tok = tok->next;
  }

  return false; // All tokens were constants
}

// Wrapper for backward compatibility - strict mode (for zero-init)
static inline bool is_const_array_size(Token *open_bracket)
{
  return !array_size_is_vla(open_bracket, true);
}

// Check if a struct/union body contains any true VLA arrays (not just patterns
// that look like VLAs for zero-init purposes, but actual variable-length arrays)
// Scans from the opening { to the closing }
static bool struct_body_contains_true_vla(Token *open_brace)
{
  if (!open_brace || !equal(open_brace, "{"))
    return false;

  Token *tok = open_brace->next;
  int depth = 1;

  while (tok && tok->kind != TK_EOF && depth > 0)
  {
    if (equal(tok, "{"))
      depth++;
    else if (equal(tok, "}"))
      depth--;
    else if (equal(tok, "[") && depth > 0)
    {
      // Found an array dimension at any level within the struct/union
      // Use strict_mode=false: offsetof patterns are OK in struct validation
      if (array_size_is_vla(tok, false))
        return true;
    }
    tok = tok->next;
  }
  return false;
}

// Check if a struct/union body contains any VLA-like arrays (for zero-init purposes)
// This is more conservative than struct_body_contains_true_vla
// Scans from the opening { to the closing }
static bool struct_body_contains_vla(Token *open_brace)
{
  if (!open_brace || !equal(open_brace, "{"))
    return false;

  Token *tok = open_brace->next;
  int depth = 1;

  while (tok && tok->kind != TK_EOF && depth > 0)
  {
    if (equal(tok, "{"))
      depth++;
    else if (equal(tok, "}"))
      depth--;
    else if (equal(tok, "[") && depth > 0)
    {
      // Found an array dimension at any level within the struct/union
      if (!is_const_array_size(tok))
        return true;
    }
    tok = tok->next;
  }
  return false;
}

// Parse a typedef declaration and add names to the typedef table
// Check if a typedef declaration contains a VLA (scan from current position to semicolon)
static bool typedef_contains_vla(Token *tok)
{
  int depth = 0;
  while (tok && !equal(tok, ";") && tok->kind != TK_EOF)
  {
    if (equal(tok, "(") || equal(tok, "{"))
      depth++;
    else if (equal(tok, ")") || equal(tok, "}"))
      depth--;
    else if (equal(tok, "[") && depth == 0)
    {
      // Found array dimension - check if it's a VLA
      if (!is_const_array_size(tok))
        return true;
    }
    tok = tok->next;
  }
  return false;
}

static void parse_typedef_declaration(Token *tok, int scope_depth)
{
  Token *typedef_start = tok;
  tok = tok->next;                   // Skip 'typedef'
  tok = scan_typedef_base_type(tok); // Skip the base type

  // Check if this typedef contains a VLA anywhere
  bool is_vla = typedef_contains_vla(typedef_start);

  // Parse declarator(s) until semicolon
  while (tok && !equal(tok, ";") && tok->kind != TK_EOF)
  {
    Token *name = scan_typedef_name(&tok);
    if (name)
      typedef_add(name->loc, name->len, scope_depth, is_vla);

    // Skip to comma or semicolon
    while (tok && !equal(tok, ",") && !equal(tok, ";") && tok->kind != TK_EOF)
    {
      if (equal(tok, "("))
        tok = skip_balanced(tok, "(", ")");
      else if (equal(tok, "["))
        tok = skip_balanced(tok, "[", "]");
      else
        tok = tok->next;
    }

    if (tok && equal(tok, ","))
      tok = tok->next;
  }
}

// Zero-init helpers
// Sorted array of type keywords for binary search (replaces HashMap)
static const char *const type_keywords[] = {
    "FILE",
    "_BitInt",
    "_Bool",
    "_Complex",
    "_Imaginary",
    "__int128",
    "__int128_t",
    "__typeof",
    "__typeof__",
    "__uint128",
    "__uint128_t",
    "bool",
    "char",
    "complex",
    "double",
    "enum",
    "float",
    "fpos_t",
    "imaginary",
    "int",
    "int16_t",
    "int32_t",
    "int64_t",
    "int8_t",
    "int_fast16_t",
    "int_fast32_t",
    "int_fast64_t",
    "int_fast8_t",
    "int_least16_t",
    "int_least32_t",
    "int_least64_t",
    "int_least8_t",
    "intmax_t",
    "intptr_t",
    "long",
    "off_t",
    "pid_t",
    "ptrdiff_t",
    "short",
    "signed",
    "size_t",
    "ssize_t",
    "struct",
    "time_t",
    "typeof",
    "typeof_unqual",
    "uint16_t",
    "uint32_t",
    "uint64_t",
    "uint8_t",
    "uint_fast16_t",
    "uint_fast32_t",
    "uint_fast64_t",
    "uint_fast8_t",
    "uint_least16_t",
    "uint_least32_t",
    "uint_least64_t",
    "uint_least8_t",
    "uintmax_t",
    "uintptr_t",
    "union",
    "unsigned",
    "void",
    "wchar_t",
    "wint_t",
};

static int compare_keyword(const void *a, const void *b)
{
  const char *key = (const char *)a;
  const char *const *keyword = (const char *const *)b;
  return strcmp(key, *keyword);
}

static bool is_type_keyword(Token *tok)
{
  if (tok->kind != TK_KEYWORD && tok->kind != TK_IDENT)
    return false;

  // Create a null-terminated string from token for comparison
  char buf[256];
  if (tok->len >= (int)sizeof(buf))
    return false;
  memcpy(buf, tok->loc, tok->len);
  buf[tok->len] = '\0';

  // Binary search in sorted array
  if (bsearch(buf, type_keywords, sizeof(type_keywords) / sizeof(type_keywords[0]),
              sizeof(type_keywords[0]), compare_keyword))
    return true;

  // User-defined typedefs (tracked during transpilation)
  if (is_known_typedef(tok))
    return true;

  // System typedefs that haven't been parsed (e.g., pthread_mutex_t when headers not flattened)
  // This is a fallback heuristic for types like *_t, __* prefixed names
  // BUT only if not explicitly declared as a variable (shadow) by the user
  if (!is_known_shadow(tok) && looks_like_system_typedef(tok))
    return true;

  return false;
}

static bool is_type_qualifier(Token *tok)
{
  if (tok->kind != TK_KEYWORD && tok->kind != TK_IDENT)
    return false;
  return equal(tok, "const") || equal(tok, "volatile") || equal(tok, "restrict") ||
         equal(tok, "static") || equal(tok, "auto") || equal(tok, "register") ||
         equal(tok, "_Atomic") || equal(tok, "_Alignas") || equal(tok, "alignas") ||
         is_attribute_keyword(tok);
}

static bool is_skip_decl_keyword(Token *tok)
{
  // Storage class specifiers that prism shouldn't zero-init
  if (equal(tok, "extern") || equal(tok, "typedef") || equal(tok, "static"))
    return true;
  // Expression keywords that cannot start a declaration
  if (equal(tok, "sizeof") || equal(tok, "_Alignof") || equal(tok, "alignof") ||
      equal(tok, "_Generic") || equal(tok, "return") || equal(tok, "if") ||
      equal(tok, "else") || equal(tok, "while") || equal(tok, "for") ||
      equal(tok, "do") || equal(tok, "switch") || equal(tok, "case") ||
      equal(tok, "default") || equal(tok, "break") || equal(tok, "continue") ||
      equal(tok, "goto") || equal(tok, "asm") || equal(tok, "__asm__") ||
      equal(tok, "__asm"))
    return true;
  return false;
}

// Check if an identifier looks like a system/standard typedef name
// These are commonly used in constant expressions (sizeof, casts)
static bool looks_like_system_typedef(Token *tok)
{
  if (tok->kind != TK_IDENT)
    return false;
  // Common pattern: ends with _t (size_t, time_t, rlim_t, etc.)
  if (tok->len >= 3 && tok->loc[tok->len - 2] == '_' && tok->loc[tok->len - 1] == 't')
    return true;
  // Common pattern: starts with __ (glibc internal types like __rlim_t)
  // But NOT if followed by ( which indicates a function call like __ctype_get_mb_cur_max()
  if (tok->len >= 2 && tok->loc[0] == '_' && tok->loc[1] == '_')
  {
    Token *next = tok->next;
    if (next && equal(next, "("))
      return false; // Function call, not a typedef
    return true;
  }
  return false;
}

// Check if the array size contains manual offsetof pointer arithmetic pattern:
// ((size_t)((char*)&((TYPE*)0)->MEMBER - (char*)0))
// GCC treats this as a VLA even though it's technically a compile-time constant.
static bool has_manual_offsetof_pattern(Token *start, Token *end)
{
  // Look for the pattern: (char*) followed eventually by -> and then - (char*)
  // This matches the manual offsetof expansion that GCC doesn't treat as constant
  for (Token *tok = start; tok && tok != end && tok->kind != TK_EOF; tok = tok->next)
  {
    // Look for (char*) pattern
    if (equal(tok, "(") && tok->next && equal(tok->next, "char"))
    {
      Token *t = tok->next->next;
      if (t && equal(t, "*") && t->next && equal(t->next, ")"))
      {
        // Found (char*), now look for -> followed by - (char*)
        for (Token *t2 = t->next; t2 && t2 != end && t2->kind != TK_EOF; t2 = t2->next)
        {
          if (equal(t2, "->") || equal(t2, "."))
          {
            // Found member access, look for - (char*) pattern after it
            for (Token *t3 = t2->next; t3 && t3 != end && t3->kind != TK_EOF; t3 = t3->next)
            {
              if (equal(t3, "-") && t3->next && equal(t3->next, "(") &&
                  t3->next->next && equal(t3->next->next, "char"))
              {
                return true; // Found manual offsetof pattern
              }
            }
          }
        }
      }
    }
  }
  return false;
}

// Check if token is a constant expression operator (safe in array dimensions)
static bool is_const_expr_operator(Token *tok)
{
  return tok->kind == TK_NUM ||
         equal(tok, "+") || equal(tok, "-") || equal(tok, "*") ||
         equal(tok, "/") || equal(tok, "%") || equal(tok, "(") || equal(tok, ")") ||
         equal(tok, "<<") || equal(tok, ">>") || equal(tok, "&") ||
         equal(tok, "|") || equal(tok, "^") || equal(tok, "~") ||
         equal(tok, "!") || equal(tok, "<") || equal(tok, ">") ||
         equal(tok, "<=") || equal(tok, ">=") || equal(tok, "==") ||
         equal(tok, "!=") || equal(tok, "&&") || equal(tok, "||") ||
         equal(tok, "?") || equal(tok, ":");
}

// Check if identifier is a compile-time constant (enum, typedef, type keyword, system type)
static bool is_const_identifier(Token *tok)
{
  return is_known_enum_const(tok) || is_known_typedef(tok) ||
         is_type_keyword(tok) || (!is_known_shadow(tok) && looks_like_system_typedef(tok));
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

// Skip leading C23 [[ ... ]] attributes at declaration start
// Returns updated position after all leading attributes
static Token *skip_leading_attributes(Token *tok)
{
  while (tok && equal(tok, "[") && tok->next && equal(tok->next, "["))
  {
    tok = tok->next->next; // skip [[
    int depth = 1;
    while (tok && tok->kind != TK_EOF && depth > 0)
    {
      if (equal(tok, "["))
        depth++;
      else if (equal(tok, "]"))
        depth--;
      tok = tok->next;
    }
    if (tok && equal(tok, "]"))
      tok = tok->next;
  }
  return tok;
}

// Skip _Pragma(...) operator sequences (C99 6.10.9)
// _Pragma is equivalent to #pragma but can appear in macro expansions
// Returns the token after all _Pragma(...) sequences
static Token *skip_pragma_operators(Token *tok)
{
  while (tok && equal(tok, "_Pragma") && tok->next && equal(tok->next, "("))
  {
    tok = tok->next;                    // skip _Pragma
    tok = skip_balanced(tok, "(", ")"); // skip (...)
  }
  return tok;
}

// Type specifier parsing result
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
} TypeSpecResult;

// Parse type specifier: qualifiers, type keywords, struct/union/enum, typeof, _Atomic, etc.
// Returns info about the type and position after it
static TypeSpecResult parse_type_specifier(Token *tok)
{
  TypeSpecResult r = {tok, false, false, false, false, false, false, false, false};

  while (is_type_qualifier(tok) || is_type_keyword(tok) ||
         (tok && equal(tok, "[") && tok->next && equal(tok->next, "[")))
  {
    // Track _Atomic
    if (equal(tok, "_Atomic"))
      r.has_atomic = true;

    // Track register storage class
    if (equal(tok, "register"))
      r.has_register = true;

    // Track volatile qualifier
    if (equal(tok, "volatile"))
      r.has_volatile = true;

    // Skip C23 [[ ... ]] attributes
    if (equal(tok, "[") && tok->next && equal(tok->next, "["))
    {
      tok = tok->next->next;
      int depth = 1;
      while (tok && tok->kind != TK_EOF && depth > 0)
      {
        if (equal(tok, "["))
          depth++;
        else if (equal(tok, "]"))
          depth--;
        tok = tok->next;
      }
      if (tok && equal(tok, "]"))
        tok = tok->next;
      r.end = tok;
      continue;
    }

    if (is_type_keyword(tok))
      r.saw_type = true;

    // struct/union/enum
    if (is_sue_keyword(tok))
    {
      r.is_struct = true;
      r.saw_type = true;
      tok = tok->next;
      // Skip attributes before tag
      while (tok && (is_attribute_keyword(tok) ||
                     equal(tok, "_Alignas") || equal(tok, "alignas")))
      {
        tok = tok->next;
        if (tok && equal(tok, "("))
          tok = skip_balanced(tok, "(", ")");
      }
      // Skip tag name
      if (tok && tok->kind == TK_IDENT)
        tok = tok->next;
      // Skip body
      if (tok && equal(tok, "{"))
      {
        if (struct_body_contains_true_vla(tok))
          error_tok(tok, "variable length array in struct/union is not supported");
        if (struct_body_contains_vla(tok))
          r.is_vla = true;
        tok = skip_balanced(tok, "{", "}");
      }
      r.end = tok;
      continue;
    }

    // typeof/typeof_unqual/__typeof__
    if (equal(tok, "typeof") || equal(tok, "__typeof__") || equal(tok, "__typeof") ||
        equal(tok, "typeof_unqual"))
    {
      r.saw_type = true;
      r.has_typeof = true;
      tok = tok->next;
      if (tok && equal(tok, "("))
        tok = skip_balanced(tok, "(", ")");
      r.end = tok;
      continue;
    }

    // _BitInt(N)
    if (equal(tok, "_BitInt"))
    {
      r.saw_type = true;
      tok = tok->next;
      if (tok && equal(tok, "("))
        tok = skip_balanced(tok, "(", ")");
      r.end = tok;
      continue;
    }

    // _Atomic(type) specifier form
    if (equal(tok, "_Atomic") && tok->next && equal(tok->next, "("))
    {
      r.saw_type = true;
      r.has_atomic = true;
      tok = tok->next;                // Move past _Atomic
      Token *inner_start = tok->next; // Start of inner type (after '(')
      tok = skip_balanced(tok, "(", ")");
      // Check if inner type is struct/union/enum
      if (inner_start && is_sue_keyword(inner_start))
        r.is_struct = true;
      // Also check for typedef'd types inside _Atomic(...)
      if (inner_start && inner_start->kind == TK_IDENT && is_known_typedef(inner_start))
        r.is_typedef = true;
      r.end = tok;
      continue;
    }

    // _Alignas/alignas/__attribute__
    if (equal(tok, "_Alignas") || equal(tok, "alignas") || is_attribute_keyword(tok))
    {
      tok = tok->next;
      if (tok && equal(tok, "("))
        tok = skip_balanced(tok, "(", ")");
      r.end = tok;
      continue;
    }

    // User-defined typedef or system typedef (pthread_mutex_t, etc.)
    // Check shadow first to handle "int size_t = 10;" correctly
    if (is_known_typedef(tok) || (!is_known_shadow(tok) && looks_like_system_typedef(tok)))
    {
      r.is_typedef = true;
      if (is_vla_typedef(tok))
        r.is_vla = true;
      // Check if next token is declarator (not part of type)
      Token *peek = tok->next;
      while (peek && is_type_qualifier(peek))
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
          return r; // Exit - next is variable name
        }
      }
    }

    tok = tok->next;
    r.end = tok;
  }

  // Check for "typedef_name varname" pattern (no pointer)
  if (!r.saw_type && tok->kind == TK_IDENT && (is_known_typedef(tok) || (!is_known_shadow(tok) && looks_like_system_typedef(tok))))
  {
    Token *t = tok->next;
    while (t && is_type_qualifier(t))
      t = t->next;
    if (t && t->kind == TK_IDENT && !equal(tok->next, "*"))
    {
      Token *after = t->next;
      if (after && (equal(after, ";") || equal(after, "[") ||
                    equal(after, ",") || equal(after, "=")))
      {
        r.saw_type = true;
        r.is_typedef = true;
        if (is_vla_typedef(tok))
          r.is_vla = true;
        r.end = tok->next;
      }
    }
  }

  return r;
}

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
} DeclResult;

// Skip __attribute__((...)) and emit it, return position after
static Token *skip_emit_attribute(Token *tok)
{
  emit_tok(tok);
  tok = tok->next;
  if (tok && equal(tok, "("))
  {
    emit_tok(tok);
    tok = tok->next;
    int depth = 1;
    while (tok && tok->kind != TK_EOF && depth > 0)
    {
      if (equal(tok, "("))
        depth++;
      else if (equal(tok, ")"))
        depth--;
      emit_tok(tok);
      tok = tok->next;
    }
  }
  return tok;
}

// Emit array dimension(s) starting at '[', return position after
static Token *emit_array_dims(Token *tok, bool *is_vla)
{
  while (equal(tok, "["))
  {
    if (!is_const_array_size(tok))
      *is_vla = true;
    emit_tok(tok);
    tok = tok->next;
    int bracket_depth = 1;
    while (tok->kind != TK_EOF && bracket_depth > 0)
    {
      if (equal(tok, "["))
        bracket_depth++;
      else if (equal(tok, "]"))
        bracket_depth--;
      if (bracket_depth > 0)
      {
        emit_tok(tok);
        tok = tok->next;
      }
    }
    if (equal(tok, "]"))
    {
      emit_tok(tok);
      tok = tok->next;
    }
  }
  return tok;
}

// Emit function parameter list starting at '(', return position after
static Token *emit_func_params(Token *tok)
{
  emit_tok(tok);
  tok = tok->next;
  int depth = 1;
  while (tok->kind != TK_EOF && depth > 0)
  {
    if (equal(tok, "("))
      depth++;
    else if (equal(tok, ")"))
      depth--;
    emit_tok(tok);
    tok = tok->next;
  }
  return tok;
}

// Parse a single declarator (pointer modifiers, name, array dims, etc.)
// Emits tokens as it parses. Returns info about the declarator.
static DeclResult parse_declarator(Token *tok, Token *warn_loc)
{
  DeclResult r = {tok, NULL, false, false, false, false, false, false};

  // Pointer modifiers and qualifiers
  while (equal(tok, "*") || is_type_qualifier(tok))
  {
    if (equal(tok, "*"))
      r.is_pointer = true;
    if (is_attribute_keyword(tok))
    {
      tok = skip_emit_attribute(tok);
      continue;
    }
    emit_tok(tok);
    tok = tok->next;
  }

  // Parenthesized declarator: (*name), (*name)[N], (*(*name)(args))[N]
  int nested_paren = 0;
  if (equal(tok, "("))
  {
    Token *peek = tok->next;
    if (!equal(peek, "*") && !equal(peek, "("))
    {
      // Not a pointer declarator pattern
      fprintf(stderr, "%s:%d: warning: zero-init: parenthesized pattern not recognized\n",
              tok_file(warn_loc)->name, tok_line_no(warn_loc));
      r.end = NULL;
      return r;
    }

    emit_tok(tok);
    tok = tok->next;
    nested_paren = 1;
    r.is_pointer = true;
    r.has_paren = true;

    // Handle nesting: (*(*(*name)...
    while (equal(tok, "*") || is_type_qualifier(tok) || equal(tok, "("))
    {
      if (equal(tok, "*"))
        r.is_pointer = true;
      else if (equal(tok, "("))
        nested_paren++;
      if (is_attribute_keyword(tok))
      {
        tok = skip_emit_attribute(tok);
        continue;
      }
      emit_tok(tok);
      tok = tok->next;
    }
  }

  // Must have identifier
  if (!is_valid_varname(tok))
  {
    fprintf(stderr, "%s:%d: warning: zero-init: expected identifier in declarator\n",
            tok_file(warn_loc)->name, tok_line_no(warn_loc));
    r.end = NULL;
    return r;
  }

  r.var_name = tok;
  emit_tok(tok);
  tok = tok->next;

  // Skip __attribute__ after variable name
  while (is_attribute_keyword(tok))
  {
    tok = skip_emit_attribute(tok);
  }

  // Array dims inside parens: (*name[N])
  if (r.has_paren && equal(tok, "["))
  {
    r.is_array = true;
    tok = emit_array_dims(tok, &r.is_vla);
  }

  // Close nested parens, handling function args or array dims at each level
  while (r.has_paren && nested_paren > 0)
  {
    while (equal(tok, "(") || equal(tok, "["))
    {
      if (equal(tok, "("))
        tok = emit_func_params(tok);
      else
      {
        r.is_array = true;
        tok = emit_array_dims(tok, &r.is_vla);
      }
    }
    if (!equal(tok, ")"))
    {
      fprintf(stderr, "%s:%d: warning: zero-init: expected ')' in declarator\n",
              tok_file(warn_loc)->name, tok_line_no(warn_loc));
      r.end = NULL;
      return r;
    }
    emit_tok(tok);
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
    tok = emit_func_params(tok);
  }

  // Array dimensions outside parens
  if (equal(tok, "["))
  {
    r.is_array = true;
    tok = emit_array_dims(tok, &r.is_vla);
  }

  // Skip __attribute__ before initializer or end of declarator
  while (is_attribute_keyword(tok))
  {
    tok = skip_emit_attribute(tok);
  }

  r.has_init = equal(tok, "=");
  r.end = tok;
  return r;
}

// Result of validating a declaration structure after type parsing
typedef struct
{
  bool valid;        // True if this is a valid variable declaration
  bool warn_complex; // True if we should warn about unparsed pattern
} DeclValidation;

// Validate declaration structure and check for statement expressions/function declarations
// Combines two checks in a single scan for efficiency
static DeclValidation validate_declaration(Token *type_end, Token *warn_loc)
{
  DeclValidation result = {false, false};
  Token *check = type_end;

  // Skip pointer modifiers and qualifiers to find first declarator token
  while (equal(check, "*") || is_type_qualifier(check))
  {
    if (equal(check, "__attribute__") || equal(check, "__attribute"))
    {
      check = check->next;
      if (check && equal(check, "("))
        check = skip_balanced(check, "(", ")");
      continue;
    }
    check = check->next;
  }

  // Check for valid declarator start
  bool has_paren_declarator = equal(check, "(");
  if (has_paren_declarator)
  {
    // Parenthesized declarator - find identifier inside
    int depth = 1;
    Token *inner = check->next;
    bool found_ident = false;
    while (inner && inner->kind != TK_EOF && depth > 0)
    {
      if (equal(inner, "("))
        depth++;
      else if (equal(inner, ")"))
        depth--;
      else if (inner->kind == TK_IDENT && !found_ident &&
               !is_type_keyword(inner) && !is_known_typedef(inner))
        found_ident = true;
      inner = inner->next;
    }
    if (!found_ident)
    {
      result.warn_complex = true;
      return result;
    }
  }
  else if (check->kind != TK_IDENT)
  {
    return result; // Not a declaration
  }

  // Scan for statement expressions or function declarations
  Token *scan = type_end;
  int depth = 0;
  bool seen_ident = false;

  while (scan && scan->kind != TK_EOF)
  {
    if (equal(scan, "__attribute__") || equal(scan, "__attribute"))
    {
      scan = scan->next;
      if (scan && equal(scan, "("))
        scan = skip_balanced(scan, "(", ")");
      continue;
    }
    if (equal(scan, "(") || equal(scan, "[") || equal(scan, "{"))
    {
      // Statement expression at top level
      if (depth == 0 && equal(scan, "(") && scan->next && equal(scan->next, "{"))
        return result;
      // Function declaration (identifier followed by paren, no pointer)
      if (depth == 0 && equal(scan, "(") && seen_ident)
      {
        Token *t = type_end;
        bool has_star = false;
        while (t && t != scan)
        {
          if (equal(t, "*"))
            has_star = true;
          if (equal(t, "("))
            break;
          t = t->next;
        }
        if (!has_star)
          return result;
      }
      depth++;
    }
    else if (equal(scan, ")") || equal(scan, "]") || equal(scan, "}"))
      depth--;
    else if (depth == 0 && equal(scan, ";"))
      break;
    else if (depth == 0 && scan->kind == TK_IDENT)
      seen_ident = true;
    scan = scan->next;
  }

  result.valid = true;
  if (result.warn_complex)
    fprintf(stderr, "%s:%d: warning: zero-init: complex pattern not parsed\n",
            tok_file(warn_loc)->name, tok_line_no(warn_loc));
  return result;
}

// Check if token after 'raw' indicates 'raw' is being used as an identifier, not the keyword
// Returns true if 'raw' is followed by something that makes it look like a declaration
static bool is_raw_declaration_context(Token *after_raw)
{
  if (!after_raw)
    return false;
  // If followed by a type keyword, typedef, or attribute, 'raw' is the prism keyword
  return is_type_keyword(after_raw) || is_known_typedef(after_raw) ||
         is_type_qualifier(after_raw) || is_sue_keyword(after_raw) ||
         equal(after_raw, "__attribute__") || equal(after_raw, "__attribute") ||
         equal(after_raw, "typeof") || equal(after_raw, "__typeof__") ||
         equal(after_raw, "_Atomic");
}

// Emit tokens from start through semicolon
static Token *emit_to_semicolon(Token *start)
{
  Token *end = start;
  while (end && !equal(end, ";") && end->kind != TK_EOF)
    end = end->next;
  if (equal(end, ";"))
    end = end->next;
  emit_range(start, end);
  return end;
}

// Handle 'raw' after storage class: "static raw int x;"
static Token *handle_storage_raw(Token *storage_tok)
{
  Token *p = storage_tok->next;
  while (p && (equal(p, "_Pragma") || equal(p, "__attribute__") || equal(p, "__attribute")))
  {
    p = p->next;
    if (p && equal(p, "("))
      p = skip_balanced(p, "(", ")");
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
  return emit_to_semicolon(p->next); // Skip 'raw', emit rest
}

// Process all declarators in a declaration and emit with zero-init
// Returns token after declaration, or NULL on failure
static Token *process_declarators(Token *tok, TypeSpecResult *type, Token *warn_loc, bool is_raw)
{
  Token *typeof_vars[MAX_TYPEOF_VARS_PER_DECL];
  int typeof_var_count = 0;

  while (tok && tok->kind != TK_EOF)
  {
    DeclResult decl = parse_declarator(tok, warn_loc);
    if (!decl.end || !decl.var_name)
      return NULL;

    tok = decl.end;

    // Determine effective VLA status (excluding typeof - handled specially)
    bool effective_vla = (decl.is_vla && !decl.has_paren) ||
                         (type->is_vla && !decl.is_pointer);

    // For typeof declarations, use memset instead of = 0 or = {0}
    // Also for atomic aggregates: Clang doesn't support _Atomic aggregate init syntax
    // But NOT for register variables (can't take address) or volatile (needs volatile semantics)
    bool is_aggregate = decl.is_array || ((type->is_struct || type->is_typedef) && !decl.is_pointer);
    bool needs_memset = !decl.has_init && !is_raw && !decl.is_pointer && !type->has_register &&
                        (type->has_typeof || (type->has_atomic && is_aggregate));

    // Add zero initializer if needed (for non-memset types)
    if (!decl.has_init && !effective_vla && !is_raw && !needs_memset)
    {
      if (is_aggregate)
        OUT_LIT(" = {0}");
      else
        OUT_LIT(" = 0");
    }

    // Track typeof variables for memset emission
    if (needs_memset && typeof_var_count < MAX_TYPEOF_VARS_PER_DECL)
      typeof_vars[typeof_var_count++] = decl.var_name;

    // Emit initializer if present
    if (decl.has_init)
    {
      int depth = 0;
      while (tok->kind != TK_EOF)
      {
        if (equal(tok, "(") || equal(tok, "[") || equal(tok, "{"))
          depth++;
        else if (equal(tok, ")") || equal(tok, "]") || equal(tok, "}"))
          depth--;
        else if (depth == 0 && (equal(tok, ",") || equal(tok, ";")))
          break;
        emit_tok(tok);
        tok = tok->next;
      }
    }

    // Register shadow for typedef names used as variables
    if (is_known_typedef(decl.var_name))
    {
      int shadow_depth = ctx->in_for_init ? ctx->defer_depth + 1 : ctx->defer_depth;
      typedef_add_shadow(decl.var_name->loc, decl.var_name->len, shadow_depth);
    }

    // Register VLA variables so sizeof(vla_var) can be detected as non-constant
    if (effective_vla && decl.var_name)
    {
      int vla_depth = ctx->in_for_init ? ctx->defer_depth + 1 : ctx->defer_depth;
      typedef_add_vla_var(decl.var_name->loc, decl.var_name->len, vla_depth);
    }

    if (equal(tok, ";"))
    {
      emit_tok(tok);
      // Emit memset for typeof variables
      // For volatile variables, use volatile-aware zeroing to ensure stores aren't optimized out
      for (int i = 0; i < typeof_var_count; i++)
      {
        if (type->has_volatile)
        {
          // Use volatile char* to ensure each byte write is not optimized
          OUT_LIT(" { volatile char *_p = (volatile char *)&");
          out_str(typeof_vars[i]->loc, typeof_vars[i]->len);
          OUT_LIT("; for (size_t _i = 0; _i < sizeof(");
          out_str(typeof_vars[i]->loc, typeof_vars[i]->len);
          OUT_LIT("); _i++) _p[_i] = 0; }");
        }
        else
        {
          OUT_LIT(" memset(&");
          out_str(typeof_vars[i]->loc, typeof_vars[i]->len);
          OUT_LIT(", 0, sizeof(");
          out_str(typeof_vars[i]->loc, typeof_vars[i]->len);
          OUT_LIT("));");
        }
      }
      return tok->next;
    }
    else if (equal(tok, ","))
    {
      emit_tok(tok);
      tok = tok->next;
    }
    else
      return NULL;
  }

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
  if (!ctx->feature_zeroinit || ctx->defer_depth <= 0 || ctx->struct_depth > 0)
    return NULL;

  // Check for "switch skip hole" - declarations before first case label
  bool in_switch_before_case = false;
  for (int d = ctx->defer_depth - 1; d >= 0; d--)
  {
    if (defer_stack[d].is_switch && !defer_stack[d].seen_case_label)
    {
      in_switch_before_case = true;
      break;
    }
    if (!defer_stack[d].is_switch)
      break;
  }

  Token *warn_loc = tok;
  Token *pragma_start = tok;

  // Skip leading attributes and pragmas
  tok = skip_leading_attributes(tok);
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
        after_raw = skip_balanced(after_raw, "(", ")");
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
  Token *before = tok;
  tok = skip_pragma_operators(tok);
  if (tok != before && !is_raw)
    start = tok;

  // Handle storage class specifiers
  if (is_skip_decl_keyword(tok))
  {
    if (is_raw)
      return emit_to_semicolon(start);
    if (equal(tok, "static") || equal(tok, "extern") || equal(tok, "typedef"))
    {
      Token *result = handle_storage_raw(tok);
      if (result)
        return result;
    }
    return NULL;
  }

  // Parse type specifier
  TypeSpecResult type = parse_type_specifier(tok);
  if (!type.saw_type)
    return NULL;

  // Validate declaration structure (combined check)
  DeclValidation v = validate_declaration(type.end, warn_loc);
  if (!v.valid)
    return NULL;

  // Error if in switch before case label
  if (in_switch_before_case && !is_raw)
  {
    error_tok(warn_loc,
              "variable declaration before first 'case' label in switch. "
              "Move this declaration before the switch, or use 'raw' to suppress zero-init.");
  }

  // Emit pragmas and type
  if (pragma_start != start)
    emit_range(pragma_start, start);
  emit_range(start, type.end);

  return process_declarators(type.end, &type, warn_loc, is_raw);
}

// Emit an expression until semicolon, tracking depth for statement expressions.
// Handles zero-init for declarations inside statement expressions.
// Returns the token after the expression (the semicolon, or EOF).
static Token *emit_expr_to_semicolon(Token *tok)
{
  int depth = 0;
  bool expr_at_stmt_start = false;
  Token *prev_tok = NULL;
  while (tok->kind != TK_EOF)
  {
    if (equal(tok, "(") || equal(tok, "[") || equal(tok, "{"))
    {
      depth++;
      if (equal(tok, "{"))
        expr_at_stmt_start = true;
    }
    else if (equal(tok, ")") || equal(tok, "]") || equal(tok, "}"))
      depth--;
    else if (depth == 0 && equal(tok, ";"))
      break;

    if (expr_at_stmt_start && ctx->feature_zeroinit)
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
    else
      expr_at_stmt_start = false;
  }
  return tok;
}

// Report goto skipping over a variable declaration (warn or error based on ctx->feature_warn_safety)
static void report_goto_skips_decl(Token *skipped_decl, Token *label_tok)
{
  const char *msg = "goto '%.*s' would skip over this variable declaration, "
                    "bypassing zero-initialization (undefined behavior in C). "
                    "Move the declaration before the goto, or restructure the code.";
  if (ctx->feature_warn_safety)
    warn_tok(skipped_decl, msg, label_tok->len, label_tok->loc);
  else
    error_tok(skipped_decl, msg, label_tok->len, label_tok->loc);
}

// Dynamic argv array
typedef struct
{
  char **data;
  int count, capacity;
} ArgvBuilder;
#define argv_builder_init(ab) (*(ab) = (ArgvBuilder){0})
static inline void argv_builder_add(ArgvBuilder *ab, const char *arg)
{
  ENSURE_ARRAY_CAP(ab->data, ab->count + 2, ab->capacity, 64, char *);
  ab->data[ab->count++] = strdup(arg);
  ab->data[ab->count] = NULL;
}
#define argv_builder_finish(ab) ((ab)->data)

// Run a command and wait for it to complete
// Returns exit status, or -1 on error
static int run_command(char **argv)
{
#ifdef _WIN32
  intptr_t status = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
  return (int)status;
#else
  pid_t pid = fork();
  if (pid == -1)
  {
    perror("fork");
    return -1;
  }
  if (pid == 0)
  {
    // Unset CC/PRISM_CC to prevent infinite recursion when prism is used as CC
    unsetenv("CC");
    unsetenv("PRISM_CC");
    execvp(argv[0], argv);
    perror("execvp");
    _exit(127);
  }
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

// Run system preprocessor (cc -E -P) on input file
// Returns path to temp file with preprocessed output, or NULL on failure
// Caller must free() the path and unlink() the file
//
// NOTE: In library mode (PRISM_LIB_MODE), errors from the C compiler
// (e.g., #error directives, missing includes) go directly to stderr.
// NOTE: In library mode, a host IDE/GUI cannot capture these messages for
// display in a UI pane. A future version could capture child stderr via pipe.
static char *preprocess_with_cc(const char *input_file)
{
  // Create temp file for preprocessed output
  char tmppath[PATH_MAX];
  snprintf(tmppath, sizeof(tmppath), "%sprism_pp_XXXXXX", get_tmp_dir());
  int fd = mkstemp(tmppath);
  if (fd < 0)
  {
    perror("mkstemp");
    return NULL;
  }
  close(fd);

  // Build argv for preprocessor: cc -E -P [flags] input -o output
  ArgvBuilder ab;
  argv_builder_init(&ab);

  const char *cc = ctx->extra_compiler ? ctx->extra_compiler : "cc";
  argv_builder_add(&ab, cc);
  argv_builder_add(&ab, "-E");

  // Add compiler flags (like -std=c99, -m32)
  for (int i = 0; i < ctx->extra_compiler_flags_count; i++)
    argv_builder_add(&ab, ctx->extra_compiler_flags[i]);

  // Add include paths
  for (int i = 0; i < ctx->extra_include_count; i++)
  {
    argv_builder_add(&ab, "-I");
    argv_builder_add(&ab, ctx->extra_include_paths[i]);
  }

  // Add defines
  for (int i = 0; i < ctx->extra_define_count; i++)
  {
    argv_builder_add(&ab, "-D");
    argv_builder_add(&ab, ctx->extra_defines[i]);
  }

  // Add prism-specific macros
  argv_builder_add(&ab, "-D__PRISM__=1");
  if (ctx->feature_defer)
    argv_builder_add(&ab, "-D__PRISM_DEFER__=1");
  if (ctx->feature_zeroinit)
    argv_builder_add(&ab, "-D__PRISM_ZEROINIT__=1");

  // Add standard feature test macros for POSIX/GNU compatibility
  argv_builder_add(&ab, "-D_POSIX_C_SOURCE=200809L");
  argv_builder_add(&ab, "-D_GNU_SOURCE");

  // Add force-includes
  for (int i = 0; i < ctx->extra_force_include_count; i++)
  {
    argv_builder_add(&ab, "-include");
    argv_builder_add(&ab, ctx->extra_force_includes[i]);
  }

  // Input file and output
  argv_builder_add(&ab, input_file);
  argv_builder_add(&ab, "-o");
  argv_builder_add(&ab, tmppath);

  char **argv = argv_builder_finish(&ab);

  // Run preprocessor using execvp directly to avoid shell quote stripping
  int ret = run_command(argv);
  free_argv(argv);

  if (ret != 0)
  {
    // Preprocessor failed - print error output
    // (Errors go to stderr directly from child process)
    unlink(tmppath);
    return NULL;
  }

  char *result = strdup(tmppath);
  return result;
}

static int transpile(char *input_file, char *output_file)
{
  // Run system preprocessor
  char *pp_file = preprocess_with_cc(input_file);
  if (!pp_file)
  {
    fprintf(stderr, "Preprocessing failed for: %s\n", input_file);
    return 0;
  }

#ifdef PRISM_LIB_MODE
  // Track preprocessor temp file for cleanup on error
  strncpy(ctx->active_temp_pp, pp_file, PATH_MAX - 1);
  ctx->active_temp_pp[PATH_MAX - 1] = '\0';
#endif

  // Tokenize the preprocessed output
  Token *tok = tokenize_file(pp_file);
  unlink(pp_file);
  free(pp_file);

#ifdef PRISM_LIB_MODE
  ctx->active_temp_pp[0] = '\0'; // Clear tracking after cleanup
#endif

  if (!tok)
  {
    fprintf(stderr, "Failed to tokenize preprocessed output\n");
    tokenizer_reset(); // Clean up tokenizer state on error
    return 0;
  }

  FILE *out_fp = fopen(output_file, "w");
  if (!out_fp)
  {
    tokenizer_reset(); // Clean up tokenizer state on error
    return 0;
  }
  out_init(out_fp);

  // Suppress warnings for inlined system header content.
  // System headers (especially glibc) use constructs that trigger various
  // warnings when compiled with strict flags. We disable these for the
  // entire flattened output since macro expansions can inject system
  // header content mid-expression, making per-region tracking impractical.
  if (ctx->feature_flatten_headers)
  {
    emit_system_header_diag_push();
    out_char('\n');
  }

  // Reset state
  ctx->defer_depth = 0;
  ctx->struct_depth = 0;
  last_emitted = NULL;
  ctx->last_line_no = 0;
  ctx->last_filename = NULL;
  ctx->last_system_header = false;
  ctx->next_scope_is_loop = false;
  ctx->next_scope_is_switch = false;
  ctx->next_scope_is_conditional = false;
  control_state_reset();
  ctx->in_for_init = false;
  ctx->pending_for_paren = false;
  ctx->conditional_block_depth = 0;
  ctx->generic_paren_depth = 0;
  ctx->current_func_returns_void = false;
  ctx->stmt_expr_count = 0;
  ctx->at_stmt_start = true; // Start of file is start of statement
  typedef_table_reset();     // Reset typedef tracking

  // Handle system headers: collect and emit #includes, or flatten
  system_includes_reset();
  if (!ctx->feature_flatten_headers)
  {
    collect_system_includes();
    emit_system_includes();
  }

  bool next_func_returns_void = false; // Track void functions at top level
  Token *prev_toplevel_tok = NULL;     // Track previous token at top level for function detection

  // Walk tokens and emit
  while (tok->kind != TK_EOF)
  {
    // Track typedefs for zero-init (must happen before zero-init check)
    // Only at statement start and outside struct/union/enum bodies
    if (ctx->at_stmt_start && ctx->struct_depth == 0 && equal(tok, "typedef"))
      parse_typedef_declaration(tok, ctx->defer_depth); // Fall through to emit the typedef normally

    // Try zero-init for declarations at statement start
    // Also allow in the init clause of a for loop: for (int i; ...)
    if (ctx->at_stmt_start && (!control_state.pending || ctx->in_for_init))
    {
      // try_zero_init_decl handles 'raw' keyword internally - it only consumes it
      // if followed by a valid declaration, otherwise treats 'raw' as a variable name
      Token *next = try_zero_init_decl(tok);
      if (next)
      {
        // Successfully handled - if there was 'raw', it's been processed
        tok = next;
        ctx->at_stmt_start = true; // Still at statement start after decl
        continue;
      }

      // try_zero_init_decl returned NULL (didn't handle it)
      // 'raw' is handled inside try_zero_init_decl now - it only consumes it
      // if followed by a declaration, otherwise treats it as a variable name
    }
    ctx->at_stmt_start = false;

    // Warn about noreturn functions that bypass defer cleanup
    // These functions terminate the program without running defers - RAII violation
    // Also mark as control exit for switch fallthrough detection (they don't fall through)
    if (ctx->feature_defer && tok->kind == TK_IDENT && tok->next && equal(tok->next, "("))
    {
      if (equal(tok, "exit") || equal(tok, "_Exit") || equal(tok, "_exit") ||
          equal(tok, "abort") || equal(tok, "quick_exit") ||
          equal(tok, "__builtin_trap") || equal(tok, "__builtin_unreachable") ||
          equal(tok, "thrd_exit"))
      {
        // Mark as control exit - noreturn functions don't fall through to next case
        mark_switch_control_exit();

        if (has_active_defers())
        {
          fprintf(stderr, "%s:%d: warning: '%.*s' called with active defers - deferred statements will NOT run. "
                          "Consider using return with cleanup, or restructure to avoid defer here.\n",
                  tok_file(tok)->name, tok_line_no(tok), tok->len, tok->loc);
        }
      }
    }

    // Handle 'defer' keyword
    // Skip if: preceded by member access (. or ->) - that's a struct field, not keyword
    //          inside struct/union/enum body - that's a field declaration, not keyword
    //          preceded by a type keyword - that's a variable/typedef name, not keyword
    //          'defer' is registered as a typedef - that's a type name, not keyword
    //          followed by assignment operator - that's a variable assignment, not defer statement
    //          inside __attribute__((...)) - that's a function name, not keyword
    if (ctx->feature_defer && tok->kind == TK_KEYWORD && equal(tok, "defer") &&
        !equal(tok->next, ":") &&                         // Distinguish defer statement from label named "defer:"
        !(last_emitted && equal(last_emitted, "goto")) && // Distinguish from "goto defer;"
        !is_member_access(last_emitted) && ctx->struct_depth == 0 &&
        !(last_emitted && (is_type_keyword(last_emitted) || equal(last_emitted, "typedef"))) &&
        !is_known_typedef(tok) && !is_assignment_op(tok->next) &&
        !is_inside_attribute(tok))
    {
      // Check for defer inside for/while/switch/if parentheses - this is invalid
      if (control_state.pending && control_state.paren_depth > 0)
        error_tok(tok, "defer cannot appear inside control statement parentheses");

      // Check for defer in braceless control flow - this causes unexpected behavior
      // The defer binds to the parent scope instead of the control statement scope,
      // causing it to execute unconditionally regardless of the condition
      if (control_state.pending && control_state.paren_depth == 0)
        error_tok(tok, "defer requires braces in if/while/for/switch statements.\n"
                       "       Braceless control flow does not create a scope, so defer binds to the parent scope\n"
                       "       and executes unconditionally. Add braces to create a proper scope:\n"
                       "       Bad:  if (x) defer cleanup();\n"
                       "       Good: if (x) { defer cleanup(); }");

      // Check for defer at the top-level of a statement expression - semantics are problematic
      // In ({ defer X; expr; }), the defer would execute after expr, making the result void
      // But defer in nested blocks inside stmt expr is OK: ({ { defer X; } expr; })
      for (int i = 0; i < ctx->stmt_expr_count; i++)
      {
        if (ctx->defer_depth == stmt_expr_levels[i])
        {
          error_tok(tok, "defer cannot be used at the top level of statement expressions ({ ... }). "
                         "The defer would execute after the final expression, changing the return type to void. "
                         "Wrap the defer in a block: ({ { defer X; ... } result; })");
          break;
        }
      }

      // setjmp/longjmp/pthread_exit bypasses defer cleanup - this MUST be an error
      if (ctx->current_func_has_setjmp)
      {
        error_tok(tok, "defer cannot be used in functions that call setjmp/longjmp/pthread_exit. "
                       "These functions bypass defer cleanup entirely, causing resource leaks. "
                       "Use explicit cleanup patterns (goto cleanup, or manual RAII) instead.");
      }

      // vfork has unpredictable control flow that can bypass cleanup
      if (ctx->current_func_has_vfork)
      {
        error_tok(tok, "defer cannot be used in functions that call vfork(). "
                       "vfork shares address space with parent and has unpredictable control flow. "
                       "Use fork() instead, or move defer to a wrapper function.");
      }

      // Inline asm may contain hidden jumps that bypass defer
      if (ctx->current_func_has_asm)
      {
        error_tok(tok, "defer cannot be used in functions containing inline assembly. "
                       "Inline asm may contain jumps (jmp, call, etc.) that bypass defer cleanup. "
                       "Move the asm to a separate function, or use explicit cleanup instead.");
      }

      // Check for defer in switch case without braces
      // Defer at switch scope level (without braces) has undefined behavior:
      // - goto between cases doesn't execute the defer (same scope depth)
      // - Hitting the next case label clears the defer
      // - Result: resource leaks and unpredictable behavior
      // Require braces to create a proper scope for the defer
      for (int d = ctx->defer_depth - 1; d >= 0; d--)
      {
        if (defer_stack[d].is_switch && ctx->defer_depth - 1 == d)
        {
          error_tok(tok, "defer in switch case requires braces to create a proper scope.\n"
                         "       Without braces, defer at switch-level has unpredictable behavior:\n"
                         "       - goto between cases may not execute the defer\n"
                         "       - Hitting the next case label clears the defer\n"
                         "       Wrap the case body in braces:\n"
                         "       Bad:  case X: defer cleanup(); break;\n"
                         "       Good: case X: { defer cleanup(); } break;");
        }
      }

      Token *defer_keyword = tok;
      tok = tok->next; // skip 'defer'

      // Find the statement (up to semicolon)
      Token *stmt_start = tok;
      Token *stmt_end = skip_to_semicolon(tok);

      // Error if semicolon not found (ran to EOF or end of block)
      if (stmt_end->kind == TK_EOF || !equal(stmt_end, ";"))
        error_tok(defer_keyword, "unterminated defer statement; expected ';'");

      // Validate defer statement doesn't contain control flow keywords
      // (which would indicate the semicolon came from a different statement)
      // Track all grouping depths - content inside (), [], {} is allowed to span lines
      int brace_depth = 0;
      int paren_depth = 0;
      int bracket_depth = 0;
      for (Token *t = stmt_start; t != stmt_end && t->kind != TK_EOF; t = t->next)
      {
        // Check ctx->at_bol BEFORE updating depths, skip for grouping tokens themselves
        // Allow multi-line when inside any grouping: (), [], {}
        bool at_top_level = (brace_depth == 0 && paren_depth == 0 && bracket_depth == 0);
        if (t != stmt_start && tok_at_bol(t) && at_top_level &&
            !equal(t, "{") && !equal(t, "(") && !equal(t, "["))
        {
          error_tok(defer_keyword,
                    "defer statement spans multiple lines without ';' - add semicolon");
        }
        // Update depths
        if (equal(t, "{"))
          brace_depth++;
        else if (equal(t, "}"))
          brace_depth--;
        else if (equal(t, "("))
          paren_depth++;
        else if (equal(t, ")"))
          paren_depth--;
        else if (equal(t, "["))
          bracket_depth++;
        else if (equal(t, "]"))
          bracket_depth--;
        // Only flag control-flow keywords at top level (not inside compound defer block)
        if (brace_depth == 0 && paren_depth == 0 && bracket_depth == 0 &&
            t->kind == TK_KEYWORD &&
            (equal(t, "return") || equal(t, "break") || equal(t, "continue") ||
             equal(t, "goto") || equal(t, "if") || equal(t, "else") ||
             equal(t, "for") || equal(t, "while") || equal(t, "do") ||
             equal(t, "switch") || equal(t, "case") || equal(t, "default") ||
             equal(t, "defer")))
        {
          error_tok(defer_keyword,
                    "defer statement appears to be missing ';' (found '%.*s' keyword inside)",
                    t->len, t->loc);
        }
      }

      // Record the defer
      defer_add(defer_keyword, stmt_start, stmt_end);

      // Skip past the semicolon (don't emit the defer yet)
      if (stmt_end->kind != TK_EOF)
        tok = stmt_end->next;
      else
        tok = stmt_end;
      end_statement_after_semicolon();

      continue;
    }

    // Handle 'return' - evaluate expr, run defers, then return
    if (ctx->feature_defer && tok->kind == TK_KEYWORD && equal(tok, "return"))
    {
      mark_switch_control_exit(); // Mark that we exited via return
      if (has_active_defers())
      {
        tok = tok->next; // skip 'return'

        // Check if there's an expression or just "return;"
        if (equal(tok, ";"))
        {
          // void return: { defers; return; }
          OUT_LIT(" {");
          emit_all_defers();
          OUT_LIT(" return;");
          tok = tok->next;
          OUT_LIT(" }");
        }
        else
        {
          // return with expression
          // Check if expression is a void cast: (void)expr - treat as void return
          // This handles typedef void cases like: VoidType func() { return (void)expr; }
          bool is_void_cast = equal(tok, "(") && tok->next && equal(tok->next, "void") &&
                              tok->next->next && equal(tok->next->next, ")");

          if (ctx->current_func_returns_void || is_void_cast)
          {
            // void function: { (expr); defers; return; }
            // The expression is executed for side effects, then we return void
            OUT_LIT(" { (");
            tok = emit_expr_to_semicolon(tok);
            OUT_LIT(");");
            emit_all_defers();
            OUT_LIT(" return;");
            if (equal(tok, ";"))
              tok = tok->next;
            OUT_LIT(" }");
          }
          else
          {
            // non-void function: { __auto_type _ret = (expr); defers; return _ret; }
            // PORTABILITY: __auto_type is a GCC/Clang extension (also supported by TCC).
            // Standard C alternative would require parsing the return type, which is complex.
            // Users targeting MSVC or strict C compilers should avoid defer with return values,
            // or use C23's typeof (once widely supported).
            unsigned long long my_ret = ctx->ret_counter++;
            OUT_LIT(" { __auto_type _prism_ret_");
            out_uint(my_ret);
            OUT_LIT(" = (");

            tok = emit_expr_to_semicolon(tok);

            OUT_LIT(");");
            emit_all_defers();
            OUT_LIT(" return _prism_ret_");
            out_uint(my_ret);
            out_char(';');

            if (equal(tok, ";"))
              tok = tok->next;

            OUT_LIT(" }");
          }
        }
        end_statement_after_semicolon();
        continue;
      }
      // No defers, emit normally
    }

    // Handle 'break' - emit defers up through loop/switch
    if (ctx->feature_defer && tok->kind == TK_KEYWORD && equal(tok, "break"))
    {
      mark_switch_control_exit(); // Mark that we exited via break
      if (control_flow_has_defers(true))
      {
        OUT_LIT(" {");
        emit_break_defers();
        OUT_LIT(" break; }");
        tok = tok->next;
        // Skip the semicolon
        if (equal(tok, ";"))
          tok = tok->next;
        end_statement_after_semicolon();
        continue;
      }
      // No defers, emit normally (fall through to default emit)
    }

    // Handle 'continue' - emit defers up to (not including) loop
    if (ctx->feature_defer && tok->kind == TK_KEYWORD && equal(tok, "continue"))
    {
      mark_switch_control_exit(); // Continue also exits the switch (like break/return/goto)
      if (control_flow_has_defers(false))
      {
        OUT_LIT(" {");
        emit_continue_defers();
        OUT_LIT(" continue; }");
        tok = tok->next;
        // Skip the semicolon
        if (equal(tok, ";"))
          tok = tok->next;
        end_statement_after_semicolon();
        continue;
      }
      // No defers, emit normally
    }

    // Handle 'goto' - emit defers for scopes being exited
    if (ctx->feature_defer && tok->kind == TK_KEYWORD && equal(tok, "goto"))
    {
      mark_switch_control_exit(); // Mark that we exited via goto (like break/return)
      Token *goto_tok = tok;
      tok = tok->next; // skip 'goto'

      // Handle computed goto (goto *ptr) - GCC extension
      if (equal(tok, "*"))
      {
        // Computed goto target is determined at runtime - can't emit defers safely
        // This MUST be an error because we can't guarantee cleanup runs
        if (has_active_defers())
        {
          error_tok(goto_tok, "computed goto (goto *) cannot be used with active defer statements. "
                              "Defer cleanup cannot be guaranteed for runtime-determined jump targets. "
                              "Restructure code to avoid computed goto or move defer outside this scope.");
        }
        // No defers active, emit normally
        emit_tok(goto_tok);
        continue;
      }

      // Get the label name - can be an identifier or a keyword used as label
      if (is_identifier_like(tok))
      {
        // Check if this goto would skip over a defer statement
        Token *skipped = goto_skips_check(goto_tok, tok->loc, tok->len, GOTO_CHECK_DEFER);
        if (skipped)
          error_tok(skipped, "goto '%.*s' would skip over this defer statement",
                    tok->len, tok->loc);

        // Check if this goto would skip over a variable declaration (zero-init safety)
        Token *skipped_decl = goto_skips_check(goto_tok, tok->loc, tok->len, GOTO_CHECK_DECL);
        if (skipped_decl)
          report_goto_skips_decl(skipped_decl, tok);

        int target_depth = label_table_lookup(tok->loc, tok->len);
        // If label not found, assume same depth (forward reference within scope)
        if (target_depth < 0)
          target_depth = ctx->defer_depth;

        if (goto_has_defers(target_depth))
        {
          OUT_LIT(" {");
          emit_goto_defers(target_depth);
          OUT_LIT(" goto");
          emit_tok(tok); // label name
          tok = tok->next;
          if (equal(tok, ";"))
          {
            emit_tok(tok);
            tok = tok->next;
          }
          OUT_LIT(" }");
          end_statement_after_semicolon();
          continue;
        }
      }
      // No defers or couldn't parse, emit goto and let normal loop emit the rest
      emit_tok(goto_tok);
      // Don't continue - let normal token processing handle the label name
    }

    // Check goto for zeroinit safety even when defer is disabled
    if (ctx->feature_zeroinit && !ctx->feature_defer && tok->kind == TK_KEYWORD && equal(tok, "goto"))
    {
      Token *goto_tok = tok;
      tok = tok->next;
      // Handle keyword labels like 'defer' used as goto target
      if (is_identifier_like(tok))
      {
        Token *skipped_decl = goto_skips_check(goto_tok, tok->loc, tok->len, GOTO_CHECK_DECL);
        if (skipped_decl)
          report_goto_skips_decl(skipped_decl, tok);
      }
      emit_tok(goto_tok);
      continue;
    }

    // Mark loop keywords so next '{' knows it's a loop scope
    if (ctx->feature_defer && tok->kind == TK_KEYWORD &&
        (equal(tok, "for") || equal(tok, "while") || equal(tok, "do")))
    {
      ctx->next_scope_is_loop = true;
      control_state.pending = true;
      // For 'for' loops, we need to allow zero-init in the init clause
      if (equal(tok, "for"))
        ctx->pending_for_paren = true;
      // For 'do' loops, the body comes immediately (no parens before body)
      // Set cf.parens_just_closed so the next '{' is recognized as the loop body
      if (equal(tok, "do"))
        control_state.parens_just_closed = true;
    }
    // Also track 'for' for zero-init even if defer is disabled
    else if (ctx->feature_zeroinit && !ctx->feature_defer && tok->kind == TK_KEYWORD && equal(tok, "for"))
    {
      control_state.pending = true;
      ctx->pending_for_paren = true;
    }

    if (tok->kind == TK_KEYWORD && equal(tok, "_Generic") && ctx->generic_paren_depth == 0)
    {
      // Emit the _Generic token and look for the opening paren
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
    // Track parentheses inside _Generic
    if (ctx->generic_paren_depth > 0)
    {
      if (equal(tok, "("))
        ctx->generic_paren_depth++;
      else if (equal(tok, ")"))
      {
        ctx->generic_paren_depth--;
        // When we exit the _Generic entirely, depth goes to 0
      }
    }

    // Mark switch keyword
    if (ctx->feature_defer && tok->kind == TK_KEYWORD && equal(tok, "switch"))
    {
      ctx->next_scope_is_switch = true;
      control_state.pending = true;
    }

    // Mark if/else keywords
    if (tok->kind == TK_KEYWORD && (equal(tok, "if") || equal(tok, "else")))
    {
      control_state.pending = true;
      // For 'else', the body comes immediately (no parens before body like 'if')
      if (equal(tok, "else"))
        control_state.parens_just_closed = true;
    }

    // Handle case/default labels - clear defers from switch scope
    // This prevents defers from leaking across cases (which would cause incorrect behavior
    // since the transpiler can't know which case is entered at runtime)
    // IMPORTANT: Only treat "default" as a switch label if NOT inside _Generic(...)
    // This prevents false positives from _Generic: _Generic(x, int: 1, default: 2)
    // Also handles edge case: _Generic(v, default: 0) where default is NOT preceded by comma
    bool is_switch_label = false;
    if (ctx->feature_defer && tok->kind == TK_KEYWORD)
    {
      if (equal(tok, "case"))
        is_switch_label = true;
      else if (equal(tok, "default"))
      {
        // "default" could be followed by ":" or by attributes then ":"
        // Example: default __attribute__((unused)): or default [[fallthrough]]:
        // Skip if we're inside _Generic(...) - detected by ctx->generic_paren_depth > 0
        if (ctx->generic_paren_depth == 0)
        {
          // Look ahead for colon, skipping any attributes
          Token *t = skip_all_attributes(tok->next);
          if (t && equal(t, ":"))
            is_switch_label = true;
        }
      }
    }

    if (is_switch_label && inside_switch_scope())
    {
      // Check if there are active defers that would be lost (fallthrough scenario)
      // Must check ALL scopes from current depth down to the switch scope,
      // because case labels can appear inside nested blocks
      for (int d = ctx->defer_depth - 1; d >= 0; d--)
      {
        // Check for defers at this scope that would be cleared
        if (defer_stack[d].count > 0 && !defer_stack[d].had_control_exit)
        {
          // There are defers that will be cleared - this is a resource leak!
          // Make it an error to force the user to fix it.
          error_tok(defer_stack[d].defer_tok[0],
                    "defer would be skipped due to switch fallthrough at %s:%d. "
                    "Add 'break;' before the next case, or wrap case body in braces.",
                    tok_file(tok)->name, tok_line_no(tok));
        }

        // Mark that we've seen a case label in the switch scope (for zero-init safety)
        if (defer_stack[d].is_switch)
        {
          defer_stack[d].seen_case_label = true;
          break;
        }
      }
      clear_switch_scope_defers();
    }

    // Detect function definition and scan for labels
    // Pattern: identifier '(' ... ')' '{'
    // Only trigger when previous token at top level was ')' (end of parameter list)
    if (ctx->feature_defer && equal(tok, "{") && ctx->defer_depth == 0)
    {
      // Only scan for labels if this looks like a function body (prev token is ')')
      // This avoids false positives from: int arr[] = {1,2,3}; or compound literals
      if (prev_toplevel_tok && equal(prev_toplevel_tok, ")"))
      {
        scan_labels_in_function(tok);
        // Set the void return flag from what we detected
        ctx->current_func_returns_void = next_func_returns_void;
      }
      next_func_returns_void = false;
    }

    // Detect void function definitions at top level
    // This sets next_func_returns_void for when we enter the function body
    if (ctx->defer_depth == 0 && is_void_function_decl(tok))
      next_func_returns_void = true;

    // Track struct/union/enum to avoid zero-init inside them
    if (is_sue_keyword(tok))
    {
      bool is_enum = equal(tok, "enum");
      // Look ahead to see if this has a body
      // Must handle: struct name {, struct {, struct __attribute__((...)) name {
      Token *t = tok->next;
      // Skip identifiers and __attribute__((...))
      while (t && (t->kind == TK_IDENT || is_attribute_keyword(t)))
      {
        if (is_attribute_keyword(t))
        {
          t = t->next;
          // Skip (( ... ))
          if (t && equal(t, "("))
            t = skip_balanced(t, "(", ")");
        }
        else
        {
          t = t->next;
        }
      }
      if (t && equal(t, "{"))
      {
        // For enums, parse constants to register shadows BEFORE emitting
        // Enum constants are visible at the enclosing scope (ctx->defer_depth)
        if (is_enum)
          parse_enum_constants(t, ctx->defer_depth);

        // Emit tokens up to and including the {
        while (tok != t)
        {
          emit_tok(tok);
          tok = tok->next;
        }
        emit_tok(tok); // emit the {
        tok = tok->next;
        ctx->struct_depth++;
        if (ctx->feature_defer)
        {
          // If we're inside control flow (e.g., for loop condition with
          // anonymous struct compound literal), preserve the loop flag!
          // struct { int x; } inside for(...) should NOT consume ctx->next_scope_is_loop.
          bool save_loop = ctx->next_scope_is_loop;
          bool save_switch = ctx->next_scope_is_switch;
          bool save_conditional = ctx->next_scope_is_conditional;
          defer_push_scope();
          // Restore flags if we're inside control flow
          if (control_state.pending)
          {
            ctx->next_scope_is_loop = save_loop;
            ctx->next_scope_is_switch = save_switch;
            ctx->next_scope_is_conditional = save_conditional;
          }
        }
        else
        {
          // Still track scope depth for typedef scoping
          defer_stack_ensure_capacity(ctx->defer_depth + 1);
          ctx->defer_depth++;
        }
        ctx->at_stmt_start = true;
        continue;
      }
    }

    // Handle '{' - push scope
    if (equal(tok, "{"))
    {
      // Detect compound literals in control flow expressions.

      // Inside control parens: definitely compound literal
      if (control_state.pending && control_state.paren_depth > 0)
      {
        // This is a compound literal inside control expression - just emit and continue
        // Do NOT call defer_push_scope() or reset control flow flags
        emit_tok(tok);
        tok = tok->next;
        // Track compound literal brace depth separately
        control_state.brace_depth++;
        continue;
      }

      // Outside control parens but not immediately after they closed: compound literal after condition
      if (control_state.pending && control_state.paren_depth == 0 && !control_state.parens_just_closed)
      {
        // The control parens closed earlier (e.g., for(...) was complete) but then we saw more tokens
        // before this '{'. This means we're in a compound literal after the condition.
        emit_tok(tok);
        tok = tok->next;
        control_state.brace_depth++;
        continue;
      }

      // Track if we're entering a conditional block (if/while/for) for accurate control exit detection
      // Switch blocks are not conditional in the same sense (we always enter one case)
      if (control_state.pending && !ctx->next_scope_is_switch)
        ctx->next_scope_is_conditional = true;

      control_state_reset(); // Proper braces found - reset control flow state
      // Check if this is a statement expression: ({ ... })
      // The previous emitted token would be '('
      if (last_emitted && equal(last_emitted, "("))
      {
        // Remember the ctx->defer_depth BEFORE we push the new scope
        // This will be the scope level of the statement expression
        // Grow stmt_expr_levels if needed
        ENSURE_ARRAY_CAP(stmt_expr_levels, ctx->stmt_expr_count + 1, stmt_expr_capacity, INITIAL_CAP, int);
        stmt_expr_levels[ctx->stmt_expr_count++] = ctx->defer_depth + 1; // +1 because we're about to push
      }
      emit_tok(tok);
      tok = tok->next;
      if (ctx->feature_defer)
        defer_push_scope();
      else
      {
        // Still need to track scope depth for typedef scoping even without defer
        defer_stack_ensure_capacity(ctx->defer_depth + 1);
        ctx->defer_depth++;
      }
      ctx->at_stmt_start = true;
      continue;
    }

    // Handle '}' - emit scope defers, then pop
    if (equal(tok, "}"))
    {
      // If we're closing a compound literal inside control parentheses,
      // just emit the brace and decrement the tracking counter - no defer handling!
      if (control_state.pending && control_state.paren_depth > 0 && control_state.brace_depth > 0)
      {
        control_state.brace_depth--;
        emit_tok(tok);
        tok = tok->next;
        continue;
      }

      if (ctx->struct_depth > 0)
        ctx->struct_depth--;
      typedef_pop_scope(ctx->defer_depth); // Pop typedefs at current scope (before depth changes)
      if (ctx->feature_defer)
      {
        emit_scope_defers();
        defer_pop_scope();
      }
      else
      {
        // Still need to track scope depth for typedef scoping even without defer
        if (ctx->defer_depth > 0)
          ctx->defer_depth--;
      }
      emit_tok(tok);
      tok = tok->next;
      // Check if we're exiting a statement expression: ... })
      // Match if the next token is ')' and we're at a stmt_expr level
      if (tok && equal(tok, ")") && ctx->stmt_expr_count > 0 &&
          stmt_expr_levels[ctx->stmt_expr_count - 1] == ctx->defer_depth + 1)
      {
        ctx->stmt_expr_count--;
      }
      // After closing brace, we're at the start of a new statement
      // (especially important at file scope for tracking typedefs)
      ctx->at_stmt_start = true;
      continue;
    }

    // Track parentheses during pending control flow (for distinguishing for(;;) from body)
    if (control_state.pending)
    {
      if (equal(tok, "("))
      {
        control_state.paren_depth++;
        control_state.parens_just_closed = false; // Opening paren, resets the "just closed" state
        // If we just saw 'for' and this is the opening paren, we're entering the init clause
        if (ctx->pending_for_paren)
        {
          ctx->in_for_init = true;
          ctx->at_stmt_start = true; // Init clause is like start of a statement
          ctx->pending_for_paren = false;
        }
      }
      else if (equal(tok, ")"))
      {
        control_state.paren_depth--;
        // Exiting the for() parens entirely clears init state
        if (control_state.paren_depth == 0)
        {
          ctx->in_for_init = false;
          control_state.parens_just_closed = true; // Mark that we just exited control parens
        }
        // Note: for inner parens (depth > 0), don't change the flag
      }
      // Semicolon inside for() parens ends the init clause (first ;) and condition (second ;)
      if (equal(tok, ";") && control_state.paren_depth == 1)
      {
        if (ctx->in_for_init)
        {
          ctx->in_for_init = false;
          // After init clause semicolon, we're at start of condition
          // (not a declaration context, so don't set ctx->at_stmt_start)
        }
      }
      // Semicolon at depth 0 ends a braceless statement body
      else if (equal(tok, ";") && control_state.paren_depth == 0)
      {
        // Pop any phantom scopes registered at ctx->defer_depth + 1
        // For braceless loop bodies like: for (int T = 0; T < 5; T++);
        // The loop variable T would be registered as shadow at ctx->defer_depth + 1
        // but we never actually enter/exit that scope with braces.
        // Without this cleanup, the shadow persists and corrupts typedef lookups.
        typedef_pop_scope(ctx->defer_depth + 1);

        control_state.pending = false;
        ctx->next_scope_is_loop = false;
        ctx->next_scope_is_switch = false;
        ctx->next_scope_is_conditional = false;
        ctx->in_for_init = false;
        ctx->pending_for_paren = false;
      }
    }

    // Track statement boundaries for zero-init
    if (equal(tok, ";") && !control_state.pending)
      ctx->at_stmt_start = true;

    // Preprocessor directives don't consume statement-start position
    // This is important for _Pragma which expands to #pragma before declarations
    if (tok->kind == TK_PREP_DIR)
    {
      emit_tok(tok);
      tok = tok->next;
      ctx->at_stmt_start = true; // Next token is still at statement start
      continue;
    }

    // Reset void function detection at top-level semicolons
    // This prevents function declarations like "void foo(void);" from affecting
    // subsequent function definitions
    if (equal(tok, ";") && ctx->defer_depth == 0)
      next_func_returns_void = false;

    // Handle user-defined labels (label:) - statement after label is at statement start
    // This ensures declarations after labels get zero-initialized
    // Must distinguish from: ternary (?:), bitfield (int x:5), case/default (handled above)
    if (equal(tok, ":") && last_emitted && last_emitted->kind == TK_IDENT &&
        ctx->struct_depth == 0 && ctx->defer_depth > 0)
    {
      // Check if previous identifier was part of a ternary by looking back further
      // In ternary, there would be a '?' before the identifier
      // We can't easily look back multiple tokens, so we rely on a different check:
      // If we're at a label, the identifier is at statement start position
      // This is an approximation - case/default are already handled above
      emit_tok(tok);
      tok = tok->next;
      ctx->at_stmt_start = true;
      continue;
    }

    // Track previous token at top level for function detection
    if (ctx->defer_depth == 0)
      prev_toplevel_tok = tok;

    // Default: emit token as-is
    emit_tok(tok);
    tok = tok->next;
  }

  // Close diagnostic pragma that was opened at the start for flatten mode
  if (ctx->feature_flatten_headers)
  {
    out_char('\n');
    emit_system_header_diag_pop();
  }

  out_close();

  // Reset tokenizer state for library mode reuse
  // This frees arena blocks and file state, preparing for next transpilation
  tokenizer_reset();

  return 1;
}

// LIBRARY API IMPLEMENTATION

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
    result.error_line = ctx->error_line;
    result.error_col = ctx->error_col;
    // Clean up any temp files that were created before the error
    if (ctx->active_temp_output[0])
    {
      remove(ctx->active_temp_output);
      ctx->active_temp_output[0] = '\0';
    }
    if (ctx->active_temp_pp[0])
    {
      unlink(ctx->active_temp_pp);
      ctx->active_temp_pp[0] = '\0';
    }
    // Reset global state to allow future transpilations
    prism_reset();
    return result;
  }
#endif

  // Set global feature flags from PrismFeatures
  ctx->feature_defer = features.defer;
  ctx->feature_zeroinit = features.zeroinit;
  ctx->feature_warn_safety = features.warn_safety;
  ctx->emit_line_directives = features.line_directives;
  ctx->feature_flatten_headers = features.flatten_headers;

  // Set preprocessor configuration from PrismFeatures
  ctx->extra_compiler = features.compiler;
  ctx->extra_include_paths = features.include_paths;
  ctx->extra_include_count = features.include_count;
  ctx->extra_defines = features.defines;
  ctx->extra_define_count = features.define_count;
  ctx->extra_compiler_flags = features.compiler_flags;
  ctx->extra_compiler_flags_count = features.compiler_flags_count;
  ctx->extra_force_includes = features.force_includes;
  ctx->extra_force_include_count = features.force_include_count;

  // Create temp file for output
  char temp_path[PATH_MAX];
  snprintf(temp_path, sizeof(temp_path), "%sprism_out.XXXXXX.c", get_tmp_dir());

#if defined(_WIN32)
  if (_mktemp_s(temp_path, sizeof(temp_path)) != 0)
  {
    result.status = PRISM_ERR_IO;
    result.error_msg = strdup("Failed to create temp file");
#ifdef PRISM_LIB_MODE
    ctx->error_jmp_set = false;
#endif
    return result;
  }
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
  {
    int fd = mkstemps(temp_path, 2);
    if (fd < 0)
    {
      result.status = PRISM_ERR_IO;
      result.error_msg = strdup("Failed to create temp file");
#ifdef PRISM_LIB_MODE
      ctx->error_jmp_set = false;
#endif
      return result;
    }
    close(fd);
  }
#else
  {
    int fd = mkstemp(temp_path);
    if (fd < 0)
    {
      result.status = PRISM_ERR_IO;
      result.error_msg = strdup("Failed to create temp file");
#ifdef PRISM_LIB_MODE
      ctx->error_jmp_set = false;
#endif
      return result;
    }
    close(fd);
    unlink(temp_path);
    size_t len = strlen(temp_path);
    if (len + 2 < sizeof(temp_path))
    {
      temp_path[len] = '.';
      temp_path[len + 1] = 'c';
      temp_path[len + 2] = '\0';
    }
  }
#endif

#ifdef PRISM_LIB_MODE
  // Track output temp file for cleanup on error (temp_path is now the actual path)
  strncpy(ctx->active_temp_output, temp_path, PATH_MAX - 1);
  ctx->active_temp_output[PATH_MAX - 1] = '\0';
#endif

  // Transpile
  if (!transpile((char *)input_file, temp_path))
  {
    result.status = PRISM_ERR_SYNTAX;
    result.error_msg = strdup("Transpilation failed");
    remove(temp_path);
#ifdef PRISM_LIB_MODE
    ctx->active_temp_output[0] = '\0';
    ctx->error_jmp_set = false;
#endif
    return result;
  }

  // Read result into memory
  FILE *f = fopen(temp_path, "rb");
  if (!f)
  {
    result.status = PRISM_ERR_IO;
    result.error_msg = strdup("Failed to read transpiled output");
    remove(temp_path);
#ifdef PRISM_LIB_MODE
    ctx->active_temp_output[0] = '\0';
    ctx->error_jmp_set = false;
#endif
    return result;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  result.output = malloc(size + 1);
  if (!result.output)
  {
    result.status = PRISM_ERR_IO;
    result.error_msg = strdup("Out of memory");
    fclose(f);
    remove(temp_path);
#ifdef PRISM_LIB_MODE
    ctx->active_temp_output[0] = '\0';
    ctx->error_jmp_set = false;
#endif
    return result;
  }

  size_t read = fread(result.output, 1, size, f);
  result.output[read] = '\0';
  result.output_len = read;
  result.status = PRISM_OK;

  fclose(f);
  remove(temp_path);

#ifdef PRISM_LIB_MODE
  ctx->active_temp_output[0] = '\0';
  ctx->error_jmp_set = false;
#endif
  return result;
}

PRISM_API void prism_free(PrismResult *r)
{
  if (r->output)
  {
    free(r->output);
    r->output = NULL;
  }
  if (r->error_msg)
  {
    free(r->error_msg);
    r->error_msg = NULL;
  }
}

// Reset all transpiler state for clean reuse (prevents memory leaks on repeated use)
PRISM_API void prism_reset(void)
{
  // Full tokenizer cleanup (parse.c) - frees arena blocks
  tokenizer_cleanup();

  // Reset defer stack
  for (int i = 0; i < defer_stack_capacity; i++)
  {
    free(defer_stack[i].stmts);
    free(defer_stack[i].ends);
    free(defer_stack[i].defer_tok);
    defer_stack[i].stmts = NULL;
    defer_stack[i].ends = NULL;
    defer_stack[i].defer_tok = NULL;
    defer_stack[i].count = 0;
    defer_stack[i].capacity = 0;
  }
  free(defer_stack);
  defer_stack = NULL;
  ctx->defer_depth = 0;
  defer_stack_capacity = 0;

  // Reset label table
  free(label_table.labels);
  label_table.labels = NULL;
  label_table.count = 0;
  label_table.capacity = 0;
  hashmap_clear(&label_table.name_map);

  // Reset typedef table (already has reset function but doesn't free hashmap buckets)
  typedef_table_reset();

  // Reset system includes
  system_includes_reset();

  // Reset statement expression tracking
  free(stmt_expr_levels);
  stmt_expr_levels = NULL;
  ctx->stmt_expr_count = 0;
  stmt_expr_capacity = 0;

  // Reset output state - close file if open (prevents FD leak on error recovery)
  if (out_buf.fp)
  {
    fclose(out_buf.fp);
    out_buf.fp = NULL;
  }
  free(out_buf.buf);
  out_buf.buf = NULL;
  out_buf.pos = 0;
  out_buf.cap = 0;
  last_emitted = NULL;
  ctx->last_line_no = 0;
  ctx->last_filename = NULL;
  ctx->last_system_header = false;

  ctx->struct_depth = 0;
  ctx->ret_counter = 0;
  ctx->next_scope_is_loop = false;
  ctx->next_scope_is_switch = false;
  ctx->next_scope_is_conditional = false;
  ctx->in_for_init = false;
  ctx->pending_for_paren = false;
  ctx->conditional_block_depth = 0;
  ctx->generic_paren_depth = 0;
  ctx->current_func_returns_void = false;
  ctx->current_func_has_setjmp = false;
  ctx->current_func_has_asm = false;
  ctx->current_func_has_vfork = false;
  ctx->at_stmt_start = true;
  control_state_reset();
}

// CLI IMPLEMENTATION (excluded with -DPRISM_LIB_MODE)

#ifndef PRISM_LIB_MODE

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

static int install(char *self_path)
{
  printf("[prism] Installing to %s...\n", INSTALL_PATH);

  // Resolve self_path to actual executable path only if the given path doesn't exist
  // This allows installing from a temp binary (e.g., after compiling from sources)
  char resolved_path[PATH_MAX];
  bool use_resolved = false;

  // First check if the given path exists as a file
  struct stat st;
  if (stat(self_path, &st) != 0)
  {
    // Path doesn't exist, try to resolve via /proc/self/exe or equivalent
#if defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", resolved_path, sizeof(resolved_path) - 1);
    if (len > 0)
    {
      resolved_path[len] = '\0';
      use_resolved = true;
    }
#elif defined(__APPLE__)
    uint32_t size = sizeof(resolved_path);
    if (_NSGetExecutablePath(resolved_path, &size) == 0)
      use_resolved = true;
#endif
  }

  if (use_resolved)
    self_path = resolved_path;

  // Check if we're trying to install over ourselves
  if (strcmp(self_path, INSTALL_PATH) == 0)
  {
    printf("[prism] Already installed at %s\n", INSTALL_PATH);
    return 0;
  }

  // Remove first (can't overwrite running executable, but can remove and replace)
  remove(INSTALL_PATH);

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
#ifndef _WIN32
    chmod(INSTALL_PATH, 0755);
#endif
    printf("[prism] Installed!\n");
    return 0;
  }

  if (input)
    fclose(input);
  if (output)
    fclose(output);

use_sudo:;
  // Remove first (can't overwrite running executable, but can remove and replace)
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

#ifndef _WIN32
  argv = build_argv("sudo", "chmod", "+x", INSTALL_PATH, NULL);
  run_command(argv);
  free_argv(argv);
#endif

  printf("[prism] Installed!\n");
  return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// CLI Types
// ─────────────────────────────────────────────────────────────────────────────

typedef enum
{
  CLI_MODE_COMPILE_AND_LINK, // default: prism foo.c → a.out (GCC-compatible)
  CLI_MODE_COMPILE_ONLY,     // -c: prism -c foo.c -o foo.o
  CLI_MODE_PASSTHROUGH,      // -E/-S: pass sources directly to CC without transpiling
  CLI_MODE_RUN,              // run: prism run foo.c (transpile + compile + execute)
  CLI_MODE_EMIT,             // transpile: prism transpile foo.c (output C)
  CLI_MODE_INSTALL,          // prism install
  CLI_MODE_HELP,             // --help / -h
  CLI_MODE_VERSION,          // --version / -v
} CliMode;

typedef struct
{
  CliMode mode;
  PrismFeatures features;

  // Sources
  const char **sources;
  int source_count;
  int source_capacity;

  // Output
  const char *output; // -o target

  // Pass-through to CC
  const char **cc_args;
  int cc_arg_count;
  int cc_arg_capacity;

  // Preprocessor flags (need to feed to prism's preprocessor AND pass to CC)
  const char **include_paths; // -I paths
  int include_count;
  int include_capacity;
  const char **defines; // -D macros (name or name=value)
  int define_count;
  int define_capacity;
  const char **force_includes; // -include files
  int force_include_count;
  int force_include_capacity;

  // Compiler flags that affect macro/include path discovery
  // (e.g., -std=c99, -m32, -march=x86_64)
  const char **pp_flags;
  int pp_flags_count;
  int pp_flags_capacity;

  // Prism-specific
  const char *cc; // --prism-cc (default: $PRISM_CC or $CC or "cc")
  bool verbose;   // --prism-verbose

  // Link-only mode detection
  bool has_objects; // true if .o, .a, or .so files were provided
} Cli;

// Get the actual C compiler to use, avoiding infinite recursion if CC=prism
static const char *get_real_cc(const char *cc)
{
  if (!cc || !*cc)
    return "cc"; // NULL or empty string

  // Simple check: if basename is "prism" or "prism.exe", return "cc"
  const char *base = strrchr(cc, '/');
  base = base ? base + 1 : cc;
  if (strcmp(base, "prism") == 0 || strcmp(base, "prism.exe") == 0)
    return "cc";

  // Advanced check: resolve paths and compare with current executable
  // This prevents infinite recursion if prism is symlinked or copied to another name
  // Example: ln -s prism my-compiler && CC=./my-compiler ./my-compiler test.c
  // Or:      cp prism cc && CC=./cc ./cc test.c (hard copy, not symlink)
  char cc_real[PATH_MAX];
  char self_real[PATH_MAX];
  bool got_self_path = false;

#ifdef __linux__
  // Linux: use /proc/self/exe to get current executable path
  ssize_t self_len = readlink("/proc/self/exe", self_real, sizeof(self_real) - 1);
  if (self_len != -1)
  {
    self_real[self_len] = '\0';
    got_self_path = true;
  }
#elif defined(__APPLE__)
  // macOS: use _NSGetExecutablePath
  uint32_t bufsize = sizeof(self_real);
  if (_NSGetExecutablePath(self_real, &bufsize) == 0)
  {
    // Resolve to canonical path
    char temp[PATH_MAX];
    if (realpath(self_real, temp) != NULL)
    {
      strncpy(self_real, temp, sizeof(self_real) - 1);
      self_real[sizeof(self_real) - 1] = '\0';
      got_self_path = true;
    }
  }
#elif defined(__FreeBSD__)
  // FreeBSD: use sysctl with KERN_PROC_PATHNAME
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
  size_t len = sizeof(self_real);
  if (sysctl(mib, 4, self_real, &len, NULL, 0) == 0)
  {
    got_self_path = true;
  }
#elif defined(__NetBSD__)
  // NetBSD: use /proc/curproc/exe (if procfs is mounted)
  ssize_t self_len = readlink("/proc/curproc/exe", self_real, sizeof(self_real) - 1);
  if (self_len != -1)
  {
    self_real[self_len] = '\0';
    got_self_path = true;
  }
#elif defined(__OpenBSD__)
  // OpenBSD: no reliable way to get executable path
  // Fall back to simple check only
  (void)self_real;
#endif

  if (!got_self_path)
  {
    // Can't determine self path, fall back to simple basename check (already done above)
    return cc;
  }

  // Resolve the CC path to canonical form
  if (realpath(cc, cc_real) == NULL)
  {
    // Can't resolve CC path, assume it's not prism
    return cc;
  }

  // Compare the resolved paths
  if (strcmp(cc_real, self_real) == 0)
  {
    // CC points to prism itself (possibly via symlink or hard copy), use "cc" instead
    return "cc";
  }

  return cc;
}

// Generic array append for CLI - grows array if needed
#define CLI_ARRAY_APPEND(cli, arr, cnt, cap, item, init_cap)                \
  do                                                                        \
  {                                                                         \
    if ((cli)->cnt >= (cli)->cap)                                           \
    {                                                                       \
      int new_cap = (cli)->cap == 0 ? (init_cap) : (cli)->cap * 2;          \
      const char **new_arr = realloc((cli)->arr, sizeof(char *) * new_cap); \
      if (!new_arr)                                                         \
        die("Out of memory");                                               \
      (cli)->arr = new_arr;                                                 \
      (cli)->cap = new_cap;                                                 \
    }                                                                       \
    (cli)->arr[(cli)->cnt++] = (item);                                      \
  } while (0)

#define cli_add_source(cli, src) CLI_ARRAY_APPEND(cli, sources, source_count, source_capacity, src, 16)
#define cli_add_include(cli, path) CLI_ARRAY_APPEND(cli, include_paths, include_count, include_capacity, path, 16)
#define cli_add_define(cli, def) CLI_ARRAY_APPEND(cli, defines, define_count, define_capacity, def, 16)
#define cli_add_force_include(cli, p) CLI_ARRAY_APPEND(cli, force_includes, force_include_count, force_include_capacity, p, 16)
#define cli_add_pp_flag(cli, flag) CLI_ARRAY_APPEND(cli, pp_flags, pp_flags_count, pp_flags_capacity, flag, 16)
#define cli_add_cc_arg(cli, arg) CLI_ARRAY_APPEND(cli, cc_args, cc_arg_count, cc_arg_capacity, arg, 64)

// Check if filename ends with given extension (case-sensitive)
static inline bool has_extension(const char *filename, const char *ext)
{
  size_t flen = strlen(filename);
  size_t elen = strlen(ext);
  return flen >= elen && !strcmp(filename + flen - elen, ext);
}

static bool is_assembly_file(const char *arg)
{
  return has_extension(arg, ".s") || has_extension(arg, ".S");
}

static bool is_cpp_file(const char *arg)
{
  return has_extension(arg, ".cc") || has_extension(arg, ".cpp") ||
         has_extension(arg, ".cxx") || has_extension(arg, ".c++") ||
         has_extension(arg, ".C") || has_extension(arg, ".mm");
}

static bool is_objc_file(const char *arg)
{
  // .m but not .mm (which is C++)
  return has_extension(arg, ".m") && !has_extension(arg, ".mm");
}

static bool needs_passthrough(const char *arg)
{
  // Files that should not be transpiled
  return is_assembly_file(arg) || is_cpp_file(arg) || is_objc_file(arg);
}

static bool is_source_file(const char *arg)
{
  return has_extension(arg, ".c") || has_extension(arg, ".i") ||
         is_assembly_file(arg) || is_cpp_file(arg) || is_objc_file(arg);
}

static bool str_startswith(const char *s, const char *prefix)
{
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

// Check if flag needs a separate argument (e.g., -L, -x)
// Note: -I, -D, -U, -include, -isystem are handled explicitly above
static bool flag_needs_arg(const char *arg)
{
  // These flags can be space-separated: -L dir, -l lib, etc.
  const char *flags[] = {"-L", "-l", "-idirafter", "-iprefix", "-iwithprefix", "-x", NULL};
  for (int i = 0; flags[i]; i++)
  {
    if (!strcmp(arg, flags[i]))
      return true;
  }
  return false;
}

static void print_help(void)
{
  printf(
      "Prism v%s - Robust C transpiler\n\n"
      "Usage: prism [options] source.c... [-o output]\n\n"
      "GCC-Compatible Options:\n"
      "  -c                    Compile only, don't link\n"
      "  -o <file>             Output file\n"
      "  -O0/-O1/-O2/-O3/-Os   Optimization level (passed to CC)\n"
      "  -g                    Debug info (passed to CC)\n"
      "  -W...                 Warnings (passed to CC)\n"
      "  -I/-D/-U/-L/-l        Include/define/lib flags (passed to CC)\n"
      "  -std=...              Language standard (passed to CC)\n\n"
      "Prism Options:\n"
      "  -fno-defer            Disable defer feature\n"
      "  -fno-zeroinit         Disable zero-initialization\n"
      "  -fno-line-directives  Disable #line directives in output\n"
      "  -fflatten-headers     Flatten included headers into single output file\n"
      "  -fno-flatten-headers  Disable header flattening\n"
      "  -fno-safety           Safety checks warn instead of error\n"
      "  --prism-cc=<compiler> Use specific compiler (default: $CC or cc)\n"
      "  --prism-verbose       Show transpile and compile commands\n\n"
      "Commands:\n"
      "  run <src.c>           Transpile, compile, and execute\n"
      "  transpile <src.c>     Output transpiled C to stdout\n"
      "  install               Install prism to %s\n"
      "  --help, -h            Show this help\n"
      "  --version, -v         Show version\n\n"
      "Environment:\n"
      "  CC                    C compiler to use (default: cc)\n"
      "  PRISM_CC              Override CC for prism specifically\n\n"
      "Examples:\n"
      "  prism foo.c                      Compile to a.out (GCC-compatible)\n"
      "  prism foo.c -o foo               Compile to 'foo'\n"
      "  prism run foo.c                  Compile and run immediately\n"
      "  prism transpile foo.c            Output transpiled C\n"
      "  prism transpile foo.c -o out.c   Transpile to file\n"
      "  prism -c foo.c -o foo.o          Compile to object file\n"
      "  prism -O2 -Wall foo.c -o foo     With optimization and warnings\n"
      "  CC=clang prism foo.c             Use clang as backend\n\n"
      "Note: Windows is not supported at this time.\n\n"
      "Apache 2.0 license (c) Dawn Larsson 2026\n"
      "https://github.com/dawnlarsson/prism\n",
      PRISM_VERSION, INSTALL_PATH);
}

// Table-driven CLI parsing
// ─────────────────────────────────────────────────────────────────────────────

// Flags that set CLI mode
typedef struct
{
  const char *flag;
  const char *alt; // Alternate flag (e.g., -h for --help), NULL if none
  CliMode mode;
  bool passthrough; // If true, also pass to CC
} ModeFlagDef;

static const ModeFlagDef mode_flags[] = {
    {"run", NULL, CLI_MODE_RUN, false},
    {"transpile", NULL, CLI_MODE_EMIT, false},
    {"install", NULL, CLI_MODE_INSTALL, false},
    {"--help", "-h", CLI_MODE_HELP, false},
    {"--version", "-v", CLI_MODE_VERSION, false},
    {"-c", NULL, CLI_MODE_COMPILE_ONLY, false},
    {"--prism-emit", NULL, CLI_MODE_EMIT, false},
    {NULL, NULL, 0, false}};

// Passthrough mode flags (set mode + pass to CC)
static const ModeFlagDef passthrough_mode_flags[] = {
    {"-E", NULL, CLI_MODE_PASSTHROUGH, true},
    {"-S", NULL, CLI_MODE_PASSTHROUGH, true},
    // Compiler probe flags used by libtool/autoconf/configure scripts
    {"-dumpmachine", NULL, CLI_MODE_PASSTHROUGH, true},
    {"-dumpversion", NULL, CLI_MODE_PASSTHROUGH, true},
    {"-dumpfullversion", NULL, CLI_MODE_PASSTHROUGH, true},
    {"-dumpspecs", NULL, CLI_MODE_PASSTHROUGH, true},
    {"-print-search-dirs", NULL, CLI_MODE_PASSTHROUGH, true},
    {"-print-libgcc-file-name", NULL, CLI_MODE_PASSTHROUGH, true},
    {"-print-multi-lib", NULL, CLI_MODE_PASSTHROUGH, true},
    {"-print-multi-directory", NULL, CLI_MODE_PASSTHROUGH, true},
    {"-print-multi-os-directory", NULL, CLI_MODE_PASSTHROUGH, true},
    {"-print-sysroot", NULL, CLI_MODE_PASSTHROUGH, true},
    {"-print-sysroot-headers-suffix", NULL, CLI_MODE_PASSTHROUGH, true},
    {"-print-prog-name=", NULL, CLI_MODE_PASSTHROUGH, true}, // prefix match handled below
    {"-print-file-name=", NULL, CLI_MODE_PASSTHROUGH, true}, // prefix match handled below
    {"--help", NULL, CLI_MODE_PASSTHROUGH, true},            // GCC help, not prism help
    {"--target-help", NULL, CLI_MODE_PASSTHROUGH, true},
    {"-###", NULL, CLI_MODE_PASSTHROUGH, true}, // show commands without executing
    {NULL, NULL, 0, false}};

// Feature flags that toggle booleans
typedef struct
{
  const char *flag;
  size_t offset; // offsetof(PrismFeatures, field)
  bool value;    // Value to set when flag is matched
} FeatureFlagDef;

#define FEATURE_FLAG(name, field, val) {name, offsetof(PrismFeatures, field), val}

static const FeatureFlagDef feature_flags[] = {
    FEATURE_FLAG("-fno-defer", defer, false),
    FEATURE_FLAG("-fno-zeroinit", zeroinit, false),
    FEATURE_FLAG("-fno-line-directives", line_directives, false),
    FEATURE_FLAG("-fno-safety", warn_safety, true),
    FEATURE_FLAG("-fflatten-headers", flatten_headers, true),
    FEATURE_FLAG("-fno-flatten-headers", flatten_headers, false),
    {NULL, 0, false}};

// Prefixes that should also be passed to preprocessor
static const char *pp_prefixes[] = {
    "-std=", "-m", "--target=", "-f", "-O", "-g", NULL};

static const char *pp_exact_flags[] = {
    "-pthread", "-pthreads", "-mthreads", "-mt", "--thread-safe",
    "-pedantic", "-pedantic-errors", "-ansi", "-traditional",
    "-traditional-cpp", "-nostdinc", "-nostdinc++", "-undef", "-trigraphs",
    NULL};

static bool should_pass_to_pp(const char *arg)
{
  for (const char **p = pp_prefixes; *p; p++)
    if (strncmp(arg, *p, strlen(*p)) == 0)
      return true;
  for (const char **p = pp_exact_flags; *p; p++)
    if (strcmp(arg, *p) == 0)
      return true;
  return false;
}

// Helper to check if a string starts with "prism" (possibly with path prefix)
// Used to detect when CC=prism or CC="prism " to avoid infinite recursion
static bool is_prism_cc(const char *cc)
{
  if (!cc || !*cc)
    return false;

  // Get basename (skip path)
  const char *base = strrchr(cc, '/');
  base = base ? base + 1 : cc;

  // Compare just the "prism" part (ignore trailing space or args like "prism -std=c11")
  // We need to match: "prism", "prism ", "prism -std=gnu11", "prism.exe", etc.
  if (strncmp(base, "prism", 5) == 0)
  {
    char next = base[5];
    // Valid if: end of string, space, dot (for .exe), or other delimiter
    if (next == '\0' || next == ' ' || next == '.')
      return true;
  }
  return false;
}

static Cli cli_parse(int argc, char **argv)
{
  Cli cli = {
      .mode = CLI_MODE_COMPILE_AND_LINK,
      .features = prism_defaults(),
  };

  // Get compiler from environment
  // Note: We check PRISM_CC first, then CC. If CC=prism or CC="prism -flag",
  // we ignore it to avoid infinite recursion.
  char *env_cc = getenv("PRISM_CC");
  if (!env_cc || !*env_cc || is_prism_cc(env_cc))
  {
    env_cc = getenv("CC");
    // If CC is set to "prism" or contains prism as the command, ignore it
    if (is_prism_cc(env_cc))
      env_cc = NULL;
  }
  if (!env_cc || !*env_cc)
    env_cc = NULL;
  cli.cc = env_cc ? env_cc : "cc";

  for (int i = 1; i < argc; i++)
  {
    char *arg = argv[i];
    bool handled = false;

    // ─── Mode flags (table-driven) ───
    for (const ModeFlagDef *f = mode_flags; f->flag; f++)
    {
      if (!strcmp(arg, f->flag) || (f->alt && !strcmp(arg, f->alt)))
      {
        cli.mode = f->mode;
        handled = true;
        break;
      }
    }
    if (handled)
      continue;

    // ─── Passthrough mode flags (table-driven) ───
    for (const ModeFlagDef *f = passthrough_mode_flags; f->flag; f++)
    {
      // Check for exact match or prefix match (for flags ending with =)
      size_t flen = strlen(f->flag);
      bool is_prefix = flen > 0 && f->flag[flen - 1] == '=';
      bool matches = is_prefix ? strncmp(arg, f->flag, flen) == 0
                               : strcmp(arg, f->flag) == 0;
      if (matches)
      {
        cli.mode = f->mode;
        cli_add_cc_arg(&cli, arg);
        handled = true;
        break;
      }
    }
    if (handled)
      continue;

    // ─── Feature flags (table-driven) ───
    for (const FeatureFlagDef *f = feature_flags; f->flag; f++)
    {
      if (!strcmp(arg, f->flag))
      {
        *(bool *)((char *)&cli.features + f->offset) = f->value;
        handled = true;
        break;
      }
    }
    if (handled)
      continue;

    // ─── Prism-specific options with arguments ───
    if (str_startswith(arg, "--prism-emit="))
    {
      cli.mode = CLI_MODE_EMIT;
      cli.output = arg + 13;
      continue;
    }
    if (str_startswith(arg, "--prism-cc="))
    {
      cli.cc = arg + 11;
      continue;
    }
    if (!strcmp(arg, "--prism-verbose"))
    {
      cli.verbose = true;
      continue;
    }

    // ─── GCC-compatible: output ───
    if (!strcmp(arg, "-o"))
    {
      if (i + 1 < argc)
        cli.output = argv[++i];
      continue;
    }
    if (str_startswith(arg, "-o"))
    {
      cli.output = arg + 2;
      continue;
    }

    // ─── GCC-compatible: include paths ───
    if (!strcmp(arg, "-I"))
    {
      if (i + 1 < argc)
      {
        const char *path = argv[++i];
        cli_add_include(&cli, path);
        cli_add_cc_arg(&cli, "-I");
        cli_add_cc_arg(&cli, path);
      }
      continue;
    }
    if (str_startswith(arg, "-I"))
    {
      const char *path = arg + 2;
      cli_add_include(&cli, path);
      cli_add_cc_arg(&cli, arg);
      continue;
    }

    // ─── GCC-compatible: defines ───
    if (!strcmp(arg, "-D"))
    {
      if (i + 1 < argc)
      {
        const char *def = argv[++i];
        cli_add_define(&cli, def);
        cli_add_cc_arg(&cli, "-D");
        cli_add_cc_arg(&cli, def);
      }
      continue;
    }
    if (str_startswith(arg, "-D"))
    {
      const char *def = arg + 2;
      cli_add_define(&cli, def);
      cli_add_cc_arg(&cli, arg);
      continue;
    }

    // ─── GCC-compatible: undefines ───
    if (!strcmp(arg, "-U"))
    {
      cli_add_pp_flag(&cli, arg);
      cli_add_cc_arg(&cli, arg);
      if (i + 1 < argc)
      {
        const char *undef = argv[++i];
        cli_add_pp_flag(&cli, undef);
        cli_add_cc_arg(&cli, undef);
      }
      continue;
    }
    if (str_startswith(arg, "-U"))
    {
      cli_add_pp_flag(&cli, arg);
      cli_add_cc_arg(&cli, arg);
      continue;
    }

    // ─── GCC-compatible: force include ───
    if (!strcmp(arg, "-include"))
    {
      cli_add_cc_arg(&cli, arg);
      if (i + 1 < argc)
      {
        const char *path = argv[++i];
        cli_add_cc_arg(&cli, path);
        cli_add_force_include(&cli, path);
      }
      continue;
    }

    // ─── GCC-compatible: system include path ───
    if (!strcmp(arg, "-isystem"))
    {
      if (i + 1 < argc)
      {
        const char *path = argv[++i];
        cli_add_include(&cli, path);
        cli_add_cc_arg(&cli, "-isystem");
        cli_add_cc_arg(&cli, path);
      }
      continue;
    }
    if (str_startswith(arg, "-isystem"))
    {
      const char *path = arg + 8;
      cli_add_include(&cli, path);
      cli_add_cc_arg(&cli, arg);
      continue;
    }

    // ─── Source files ───
    if (arg[0] != '-' && is_source_file(arg))
    {
      cli_add_source(&cli, arg);
      continue;
    }

    // ─── Object files and libraries (pass through) ───
    if (arg[0] != '-')
    {
      size_t len = strlen(arg);
      if ((len >= 2 && !strcmp(arg + len - 2, ".o")) ||
          (len >= 2 && !strcmp(arg + len - 2, ".a")) ||
          (len >= 3 && !strcmp(arg + len - 3, ".so")) ||
          (len > 3 && strstr(arg, ".so.") != NULL))
      {
        cli.has_objects = true;
      }
      cli_add_cc_arg(&cli, arg);
      continue;
    }

    // ─── Everything else: pass through to CC ───
    if (should_pass_to_pp(arg))
      cli_add_pp_flag(&cli, arg);
    cli_add_cc_arg(&cli, arg);

    // Handle space-separated args like -L dir, -x lang
    if (flag_needs_arg(arg) && i + 1 < argc && argv[i + 1][0] != '-')
      cli_add_cc_arg(&cli, argv[++i]);
  }

  return cli;
}

static void cli_free(Cli *cli)
{
  free(cli->sources);
  free(cli->cc_args);
  free(cli->include_paths);
  free(cli->defines);
  free(cli->force_includes);
  free(cli->pp_flags);
}

static char *create_temp_file(const char *source, char *buf, size_t bufsize)
{
  char *basename_ptr = strrchr(source, '/');
#ifdef _WIN32
  char *win_basename = strrchr(source, '\\');
  if (!basename_ptr || (win_basename && win_basename > basename_ptr))
    basename_ptr = win_basename;
#endif

  if (!basename_ptr)
    snprintf(buf, bufsize, ".%s.XXXXXX.c", source);
  else
  {
    char source_dir[PATH_MAX];
    size_t dir_len = basename_ptr - source;
    if (dir_len >= sizeof(source_dir))
      dir_len = sizeof(source_dir) - 1;
    strncpy(source_dir, source, dir_len);
    source_dir[dir_len] = 0;
    basename_ptr++;
    snprintf(buf, bufsize, "%s/.%s.XXXXXX.c", source_dir, basename_ptr);
  }

#if defined(_WIN32)
  if (_mktemp_s(buf, bufsize) != 0)
    return NULL;
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
  {
    int fd = mkstemps(buf, 2);
    if (fd < 0)
    {
      fprintf(stderr, "prism: mkstemps failed for '%s': %s\n", buf, strerror(errno));
      return NULL;
    }
    close(fd);
  }
#else
  {
    int fd = mkstemp(buf);
    if (fd < 0)
      return NULL;
    close(fd);
    unlink(buf);
    size_t len = strlen(buf);
    if (len + 2 < bufsize)
    {
      buf[len] = '.';
      buf[len + 1] = 'c';
      buf[len + 2] = '\0';
    }
  }
#endif

  return buf;
}

int main(int argc, char **argv)
{
  prism_ctx_init();

  if (argc < 2)
  {
    print_help();
    return 0;
  }

  Cli cli = cli_parse(argc, argv);

  // Set feature flags from CLI
  ctx->feature_defer = cli.features.defer;
  ctx->feature_zeroinit = cli.features.zeroinit;
  ctx->feature_warn_safety = cli.features.warn_safety;

  // Set preprocessor configuration from CLI
  // Use get_real_cc() to avoid infinite recursion if CC=prism
  ctx->extra_compiler = get_real_cc(cli.cc);
  ctx->extra_compiler_flags = cli.pp_flags;
  ctx->extra_compiler_flags_count = cli.pp_flags_count;
  ctx->extra_include_paths = cli.include_paths;
  ctx->extra_include_count = cli.include_count;
  ctx->extra_defines = cli.defines;
  ctx->extra_define_count = cli.define_count;
  ctx->extra_force_includes = cli.force_includes;
  ctx->extra_force_include_count = cli.force_include_count;
  ctx->emit_line_directives = cli.features.line_directives;
  ctx->feature_flatten_headers = cli.features.flatten_headers;

  // Handle special modes
  switch (cli.mode)
  {
  case CLI_MODE_HELP:
    print_help();
    cli_free(&cli);
    return 0;

  case CLI_MODE_VERSION:
    // If -v was used with source files or other args, pass through to compiler
    // Only show prism version when -v/--version is used alone
    if (cli.source_count > 0 || cli.cc_arg_count > 0 || cli.has_objects)
    {
      // Pass -v through to compiler
      const char *compiler = get_real_cc(cli.cc);

      ArgvBuilder ab;
      argv_builder_init(&ab);
      argv_builder_add(&ab, compiler);
      argv_builder_add(&ab, "-v");

      for (int i = 0; i < cli.cc_arg_count; i++)
        argv_builder_add(&ab, cli.cc_args[i]);

      for (int i = 0; i < cli.source_count; i++)
        argv_builder_add(&ab, cli.sources[i]);

      if (cli.output)
      {
        argv_builder_add(&ab, "-o");
        argv_builder_add(&ab, cli.output);
      }

      char **pass_argv = argv_builder_finish(&ab);

      if (cli.verbose)
      {
        fprintf(stderr, "[prism] Passthrough -v: ");
        for (int i = 0; pass_argv[i]; i++)
          fprintf(stderr, "%s ", pass_argv[i]);
        fprintf(stderr, "\n");
      }

      int status = run_command(pass_argv);
      free_argv(pass_argv);
      cli_free(&cli);
      return status;
    }
    printf("prism %s\n", PRISM_VERSION);
    cli_free(&cli);
    return 0;

  case CLI_MODE_INSTALL:
    // If source files provided, compile them first to produce the binary to install
    if (cli.source_count > 0)
    {
      // Compile sources to a temp binary, then install that
      char temp_bin[PATH_MAX];
      snprintf(temp_bin, sizeof(temp_bin), "%sprism_install_%d", get_tmp_dir(), getpid());

      // Build compile command
      const char *cc = get_real_cc(cli.cc ? cli.cc : getenv("PRISM_CC"));
      if (!cc || (strcmp(cc, "cc") == 0 && !cli.cc))
      {
        cc = getenv("CC");
        if (cc)
          cc = get_real_cc(cc);
      }
      if (!cc)
        cc = "cc";

      // Transpile and compile each source
      char **temp_files = malloc(cli.source_count * sizeof(char *));
      if (!temp_files)
        die("Memory allocation failed");

      for (int i = 0; i < cli.source_count; i++)
      {
        temp_files[i] = malloc(PATH_MAX);
        if (!temp_files[i])
          die("Memory allocation failed");
        snprintf(temp_files[i], PATH_MAX, "%sprism_install_%d_%d.c", get_tmp_dir(), getpid(), i);

        PrismResult result = prism_transpile_file(cli.sources[i], cli.features);
        if (result.status != PRISM_OK)
        {
          fprintf(stderr, "%s:%d:%d: error: %s\n",
                  cli.sources[i], result.error_line, result.error_col,
                  result.error_msg ? result.error_msg : "transpilation failed");
          for (int j = 0; j <= i; j++)
          {
            remove(temp_files[j]);
            free(temp_files[j]);
          }
          free(temp_files);
          cli_free(&cli);
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

      // Build argv for compiler: cc -O2 temp1.c temp2.c ... -o temp_bin
      int argc_max = 4 + cli.source_count + cli.cc_arg_count;
      char **argv_cc = malloc((argc_max + 1) * sizeof(char *));
      if (!argv_cc)
        die("Memory allocation failed");

      int argc_cc = 0;
      argv_cc[argc_cc++] = (char *)cc;
      argv_cc[argc_cc++] = "-O2";
      for (int i = 0; i < cli.source_count; i++)
        argv_cc[argc_cc++] = temp_files[i];
      for (int i = 0; i < cli.cc_arg_count; i++)
        argv_cc[argc_cc++] = (char *)cli.cc_args[i];
      argv_cc[argc_cc++] = "-o";
      argv_cc[argc_cc++] = temp_bin;
      argv_cc[argc_cc] = NULL;

      if (cli.verbose)
      {
        fprintf(stderr, "[prism] Compiling:");
        for (int i = 0; i < argc_cc; i++)
          fprintf(stderr, " %s", argv_cc[i]);
        fprintf(stderr, "\n");
      }

      int status = run_command(argv_cc);
      free(argv_cc);

      // Clean up temp source files
      for (int i = 0; i < cli.source_count; i++)
      {
        remove(temp_files[i]);
        free(temp_files[i]);
      }
      free(temp_files);

      if (status != 0)
      {
        cli_free(&cli);
        return 1;
      }

      // Install the compiled binary
      int result = install(temp_bin);
      remove(temp_bin);
      cli_free(&cli);
      return result;
    }
    cli_free(&cli);
    return install(argv[0]);

  case CLI_MODE_EMIT:
  {
    if (cli.source_count == 0)
      die("No source files specified");

    for (int i = 0; i < cli.source_count; i++)
    {
      const char *source = cli.sources[i];

      if (cli.output)
      {
        // Transpile to file
        if (cli.verbose)
          fprintf(stderr, "[prism] %s -> %s\n", source, cli.output);
        if (!transpile((char *)source, (char *)cli.output))
          die("Transpilation failed");
      }
      else
      {
        // Transpile to stdout - use /dev/stdout to avoid temp file I/O
#ifdef _WIN32
        // Windows: fall back to temp file
        char temp[PATH_MAX];
        if (!create_temp_file(source, temp, sizeof(temp)))
          die("Failed to create temp file");

        if (!transpile((char *)source, temp))
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
        // Unix: write directly to /dev/stdout
        if (!transpile((char *)source, "/dev/stdout"))
          die("Transpilation failed");
#endif
      }
    }
    cli_free(&cli);
    return 0;
  }

  case CLI_MODE_PASSTHROUGH:
  {
    // Pass sources directly to compiler without transpiling (-E, -S modes)
    const char *compiler = get_real_cc(cli.cc);

    ArgvBuilder ab;
    argv_builder_init(&ab);
    argv_builder_add(&ab, compiler);

    // Add all CC args (includes -E or -S flag)
    for (int i = 0; i < cli.cc_arg_count; i++)
      argv_builder_add(&ab, cli.cc_args[i]);

    // Add source files directly (no transpiling)
    for (int i = 0; i < cli.source_count; i++)
      argv_builder_add(&ab, cli.sources[i]);

    if (cli.output)
    {
      argv_builder_add(&ab, "-o");
      argv_builder_add(&ab, cli.output);
    }

    char **pass_argv = argv_builder_finish(&ab);

    if (cli.verbose)
    {
      fprintf(stderr, "[prism] Passthrough: ");
      for (int i = 0; pass_argv[i]; i++)
        fprintf(stderr, "%s ", pass_argv[i]);
      fprintf(stderr, "\n");
    }

    int status = run_command(pass_argv);
    free_argv(pass_argv);
    cli_free(&cli);
    return status;
  }

  case CLI_MODE_COMPILE_ONLY:
  case CLI_MODE_COMPILE_AND_LINK:
  case CLI_MODE_RUN:
    break; // Continue below
  }

  // Link-only mode: if no sources but has object files, pass through to compiler
  if (cli.source_count == 0 && cli.has_objects && cli.mode != CLI_MODE_RUN)
  {
    const char *compiler = get_real_cc(cli.cc);

    ArgvBuilder ab;
    argv_builder_init(&ab);
    argv_builder_add(&ab, compiler);

    for (int i = 0; i < cli.cc_arg_count; i++)
      argv_builder_add(&ab, cli.cc_args[i]);

    if (cli.output)
    {
      argv_builder_add(&ab, "-o");
      argv_builder_add(&ab, cli.output);
    }

    char **link_argv = argv_builder_finish(&ab);

    if (cli.verbose)
    {
      fprintf(stderr, "[prism] Link-only: ");
      for (int i = 0; link_argv[i]; i++)
        fprintf(stderr, "%s ", link_argv[i]);
      fprintf(stderr, "\n");
    }

    int status = run_command(link_argv);
    free_argv(link_argv);
    cli_free(&cli);
    return status;
  }

  // Need at least one source
  if (cli.source_count == 0)
    die("No source files specified");

  // Check if we have any C++/Objective-C files
  bool has_cpp_files = false;
  bool has_objc_files = false;
  for (int i = 0; i < cli.source_count; i++)
  {
    if (is_cpp_file(cli.sources[i]))
      has_cpp_files = true;
    if (is_objc_file(cli.sources[i]))
      has_objc_files = true;
  }

  // Determine compiler
  const char *compiler = get_real_cc(cli.cc);

  // Switch to C++/Objective-C compiler if needed
  if (has_cpp_files || has_objc_files)
  {
    // Map C compiler to C++ compiler
    // Use precise matching to avoid false positives (e.g., "tcc" contains "cc" but isn't gcc)
    int clen = strlen(compiler);
    bool is_gcc_family = (clen >= 3 && strcmp(compiler + clen - 3, "gcc") == 0) ||
                         (strcmp(compiler, "cc") == 0) ||
                         (clen >= 3 && strcmp(compiler + clen - 3, "/cc") == 0);
    bool is_clang_family = strstr(compiler, "clang") != NULL;

    if (is_gcc_family)
    {
      compiler = has_cpp_files ? "g++" : "gcc";
    }
    else if (is_clang_family)
    {
      compiler = has_cpp_files ? "clang++" : "clang";
    }
    else if (has_cpp_files)
    {
      // Unknown compiler with C++ files - warn and continue with user's choice
      fprintf(stderr, "[prism] Warning: C++ files detected but compiler '%s' is not recognized.\n"
                      "         Prism cannot automatically switch to C++ mode for this compiler.\n"
                      "         Please specify a C++ compiler explicitly if compilation fails.\n",
              compiler);
    }

    if (cli.verbose && (is_gcc_family || is_clang_family))
    {
      fprintf(stderr, "[prism] Detected %s files, switching to %s\n",
              has_cpp_files ? "C++" : "Objective-C", compiler);
    }
  }

  // Transpile all sources to temp files (skip assembly files - pass through directly)
  // Note: source_count is guaranteed > 0 here (checked above), cast suppresses warning
  char **temp_files = calloc((unsigned)cli.source_count, sizeof(char *));
  if (!temp_files)
    die("Out of memory");

  for (int i = 0; i < cli.source_count; i++)
  {
    // Assembly, C++, and Objective-C files don't need transpilation - pass through directly
    if (needs_passthrough(cli.sources[i]))
    {
      temp_files[i] = strdup(cli.sources[i]);
      if (!temp_files[i])
        die("Out of memory");
      if (cli.verbose)
      {
        const char *type = is_assembly_file(cli.sources[i]) ? "assembly" : is_cpp_file(cli.sources[i]) ? "C++"
                                                                                                       : "Objective-C";
        fprintf(stderr, "[prism] Passing through %s file: %s\n", type, cli.sources[i]);
      }
      continue;
    }

    temp_files[i] = malloc(512);
    if (!temp_files[i])
      die("Out of memory");

    if (!create_temp_file(cli.sources[i], temp_files[i], 512))
    {
      for (int j = 0; j < i; j++)
      {
        if (!needs_passthrough(cli.sources[j]))
          remove(temp_files[j]);
        free(temp_files[j]);
      }
      free(temp_files);
      die("Failed to create temp file");
    }

    if (cli.verbose)
      fprintf(stderr, "[prism] Transpiling %s -> %s\n", cli.sources[i], temp_files[i]);

    if (!transpile((char *)cli.sources[i], temp_files[i]))
    {
      for (int j = 0; j <= i; j++)
      {
        if (!needs_passthrough(cli.sources[j]))
          remove(temp_files[j]);
        free(temp_files[j]);
      }
      free(temp_files);
      die("Transpilation failed");
    }
  }

  // For RUN mode, compile to temp executable
  char temp_exe[PATH_MAX] = {0};
  if (cli.mode == CLI_MODE_RUN)
  {
    snprintf(temp_exe, sizeof(temp_exe), "%sprism_run.XXXXXX", get_tmp_dir());
#if defined(_WIN32)
    _mktemp_s(temp_exe, sizeof(temp_exe));
    strcat(temp_exe, ".exe");
#else
    int fd = mkstemp(temp_exe);
    if (fd >= 0)
      close(fd);
#endif
  }

  // Build compiler command
  ArgvBuilder ab;
  argv_builder_init(&ab);

  argv_builder_add(&ab, compiler);

  // Add transpiled sources
  for (int i = 0; i < cli.source_count; i++)
    argv_builder_add(&ab, temp_files[i]);

  // Add pass-through args
  for (int i = 0; i < cli.cc_arg_count; i++)
    argv_builder_add(&ab, cli.cc_args[i]);

  // Suppress warnings from inlined system headers and preprocessed code
  // These come from system headers expanded during preprocessing, or intentional
  // fallthrough patterns in external code (binutils, coreutils, etc.)
  argv_builder_add(&ab, "-Wno-type-limits");          // wchar.h unsigned >= 0
  argv_builder_add(&ab, "-Wno-cast-align");           // SSE/AVX intrinsics
  argv_builder_add(&ab, "-Wno-logical-op");           // errno EAGAIN==EWOULDBLOCK
  argv_builder_add(&ab, "-Wno-implicit-fallthrough"); // switch fallthrough patterns
  argv_builder_add(&ab, "-Wno-unused-function");      // system inline functions
  argv_builder_add(&ab, "-Wno-unused-variable");      // system variables
  argv_builder_add(&ab, "-Wno-unused-parameter");     // system function params
  argv_builder_add(&ab, "-Wno-maybe-uninitialized");  // false positives in preprocessed code

  // Add -c for compile-only mode
  if (cli.mode == CLI_MODE_COMPILE_ONLY)
    argv_builder_add(&ab, "-c");

  // Add output
  if (cli.mode == CLI_MODE_RUN)
  {
    argv_builder_add(&ab, "-o");
    argv_builder_add(&ab, temp_exe);
  }
  else if (cli.output)
  {
    argv_builder_add(&ab, "-o");
    argv_builder_add(&ab, cli.output);
  }
  else if (cli.mode == CLI_MODE_COMPILE_ONLY && cli.source_count == 1)
  {
    // GCC-compatible: -c foo.c produces foo.o
    static char default_obj[PATH_MAX];
    const char *src = cli.sources[0];
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    snprintf(default_obj, sizeof(default_obj), "%s", base);
    char *dot = strrchr(default_obj, '.');
    if (dot)
      strcpy(dot, ".o");
    else
    {
      size_t len = strlen(default_obj);
      if (len + 2 < sizeof(default_obj))
        memcpy(default_obj + len, ".o", 3);
    }
    argv_builder_add(&ab, "-o");
    argv_builder_add(&ab, default_obj);
  }
  // GCC-compatible: no -o means output to a.out (handled by CC)

  char **compile_argv = argv_builder_finish(&ab);

  if (cli.verbose)
  {
    fprintf(stderr, "[prism] ");
    for (int i = 0; compile_argv[i]; i++)
      fprintf(stderr, "%s ", compile_argv[i]);
    fprintf(stderr, "\n");
  }

  int status = run_command(compile_argv);
  free_argv(compile_argv);

  // Cleanup temp source files (but not assembly files - those are originals)
  for (int i = 0; i < cli.source_count; i++)
  {
    if (!needs_passthrough(cli.sources[i]))
      remove(temp_files[i]);
    free(temp_files[i]);
  }
  free(temp_files);

  if (status != 0)
  {
    if (cli.mode == CLI_MODE_RUN && temp_exe[0])
      remove(temp_exe);
    cli_free(&cli);
    return status;
  }

  // RUN mode: execute the compiled binary
  if (cli.mode == CLI_MODE_RUN)
  {
    char **run_argv = build_argv(temp_exe, NULL);
    if (cli.verbose)
      fprintf(stderr, "[prism] Running %s\n", temp_exe);
    status = run_command(run_argv);
    free_argv(run_argv);
    remove(temp_exe);
  }

  cli_free(&cli);
  return status;
}

#endif // PRISM_LIB_MODE
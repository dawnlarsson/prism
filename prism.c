#define _DARWIN_C_SOURCE
#define PRISM_VERSION "0.64.0"

#include "parse.c"

// ═══════════════════════════════════════════════════════════════════════════
// LIBRARY API
// ═══════════════════════════════════════════════════════════════════════════

typedef struct
{
  bool defer;
  bool zeroinit;
  bool line_directives;
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

static PrismFeatures prism_defaults(void)
{
  return (PrismFeatures){.defer = true, .zeroinit = true, .line_directives = true};
}

// Forward declarations for library API
static PrismResult prism_transpile_file(const char *input_file, PrismFeatures features);
static void prism_free(PrismResult *r);

// ═══════════════════════════════════════════════════════════════════════════
// CLI CONFIGURATION (excluded with -DPRISM_LIB_MODE)
// ═══════════════════════════════════════════════════════════════════════════

#ifndef PRISM_LIB_MODE

#ifdef _WIN32
#define INSTALL_PATH "prism.exe"
#define TMP_DIR ""
#else
#define INSTALL_PATH "/usr/local/bin/prism"
#define TMP_DIR "/tmp/"
#endif

#endif // PRISM_LIB_MODE

#if defined(__x86_64__) || defined(_M_X64)
#define NATIVE_ARCH "x86"
#define NATIVE_BITS 64
#elif defined(__i386__) || defined(_M_IX86)
#define NATIVE_ARCH "x86"
#define NATIVE_BITS 32
#elif defined(__aarch64__) || defined(_M_ARM64)
#define NATIVE_ARCH "arm"
#define NATIVE_BITS 64
#elif defined(__arm__) || defined(_M_ARM)
#define NATIVE_ARCH "arm"
#define NATIVE_BITS 32
#else
#define NATIVE_ARCH "x86"
#define NATIVE_BITS 64
#endif

#if defined(__linux__)
#define NATIVE_PLATFORM "linux"
#elif defined(_WIN32)
#define NATIVE_PLATFORM "windows"
#elif defined(__APPLE__)
#define NATIVE_PLATFORM "macos"
#else
#define NATIVE_PLATFORM "linux"
#endif

// Internal feature flags (set from PrismFeatures before transpile)
static bool feature_defer = true;
static bool feature_zeroinit = true;

// Extra preprocessor configuration (set by CLI before transpile)
static const char **extra_include_paths = NULL;
static int extra_include_count = 0;
static const char **extra_defines = NULL;
static int extra_define_count = 0;

static int struct_depth = 0;

// Initial capacities for dynamic arrays (will grow as needed)
#define INITIAL_DEFER_DEPTH 64
#define INITIAL_DEFERS_PER_SCOPE 32
#define INITIAL_LABELS 256
#define INITIAL_ARGS 128
#define INITIAL_STMT_EXPR_DEPTH 32

typedef struct
{
  Token **stmts;     // Start token of each deferred statement (dynamic)
  Token **ends;      // End token (the semicolon) (dynamic)
  Token **defer_tok; // The 'defer' keyword token (for error messages) (dynamic)
  int count;
  int capacity;          // Current capacity of the arrays
  bool is_loop;          // true if this scope is a for/while/do loop
  bool is_switch;        // true if this scope is a switch statement
  bool had_control_exit; // true if break/return/goto/continue seen since last defer in switch
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
} LabelTable;

typedef struct
{
  char *name; // Points into token stream (no alloc needed)
  int len;
  int scope_depth;    // Scope where defined (aligns with defer_depth)
  bool is_vla;        // True if typedef refers to a VLA type
  bool is_shadow;     // True if this entry shadows a typedef (variable with same name)
  bool is_enum_const; // True if this is an enum constant (compile-time constant)
} TypedefEntry;

typedef struct
{
  TypedefEntry *entries;
  int count;
  int capacity;
} TypedefTable;

static TypedefTable typedef_table;

static DeferScope *defer_stack = NULL;
static int defer_depth = 0;
static int defer_stack_capacity = 0;

static char pending_temp_file_buf[512];
static char *pending_temp_file = NULL;

// Track pending loop/switch for next scope
static bool next_scope_is_loop = false;
static bool next_scope_is_switch = false;
static bool pending_control_flow = false; // True after if/else/for/while/do/switch until we see { or ;
static int control_paren_depth = 0;       // Track parens to distinguish for(;;) from braceless body
static bool in_for_init = false;          // True when inside the init clause of a for loop (before first ;)
static bool pending_for_paren = false;    // True after seeing 'for', waiting for '(' to start init clause

static LabelTable label_table;
static bool current_func_returns_void = false;
static bool current_func_has_setjmp = false;
static bool current_func_has_asm = false;
static bool current_func_has_vfork = false;

static bool at_stmt_start = false;
static bool in_switch_case_body = false;
// Track statement expression scopes - store the defer_depth at which each starts
static int *stmt_expr_levels = NULL; // defer_depth when stmt expr started (dynamic)
static int stmt_expr_count = 0;      // number of active statement expressions
static int stmt_expr_capacity = 0;   // capacity of stmt_expr_levels array

// Token emission
static FILE *out;
static Token *last_emitted = NULL;

// Line tracking for #line directives
static int last_line_no = 0;
static char *last_filename = NULL;
static bool emit_line_directives = true; // Can be disabled with no-line-directives flag

static void cleanup_temp_file(void)
{
  if (pending_temp_file && pending_temp_file[0])
  {
    remove(pending_temp_file);
    pending_temp_file = NULL;
  }
}

static void end_statement_after_semicolon(void)
{
  at_stmt_start = true;
  in_for_init = false; // Semicolon ends init clause
  if (pending_control_flow && control_paren_depth == 0)
  {
    pending_control_flow = false;
    next_scope_is_loop = false;
    next_scope_is_switch = false;
  }
}

// Ensure defer_stack has capacity for at least n scopes
static void defer_stack_ensure_capacity(int n)
{
  if (n <= defer_stack_capacity)
    return;
  int new_cap = defer_stack_capacity == 0 ? INITIAL_DEFER_DEPTH : defer_stack_capacity * 2;
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
    new_stack[i].had_control_exit = false;
  }
  defer_stack = new_stack;
  defer_stack_capacity = new_cap;
}

static void defer_push_scope(void)
{
  defer_stack_ensure_capacity(defer_depth + 1);
  defer_stack[defer_depth].count = 0;
  defer_stack[defer_depth].is_loop = next_scope_is_loop;
  defer_stack[defer_depth].is_switch = next_scope_is_switch;
  defer_stack[defer_depth].had_control_exit = false;
  next_scope_is_loop = false;
  next_scope_is_switch = false;
  defer_depth++;
}

static void defer_pop_scope(void)
{
  if (defer_depth > 0)
    defer_depth--;
}

// Ensure a DeferScope has capacity for at least n defers
static void defer_scope_ensure_capacity(DeferScope *scope, int n)
{
  if (n <= scope->capacity)
    return;
  int new_cap = scope->capacity == 0 ? INITIAL_DEFERS_PER_SCOPE : scope->capacity * 2;
  while (new_cap < n)
    new_cap *= 2;

  // Allocate all three arrays, handling partial failures
  Token **new_stmts = realloc(scope->stmts, sizeof(Token *) * new_cap);
  if (!new_stmts)
    error("out of memory growing defer scope");
  scope->stmts = new_stmts;

  Token **new_ends = realloc(scope->ends, sizeof(Token *) * new_cap);
  if (!new_ends)
    error("out of memory growing defer scope");
  scope->ends = new_ends;

  Token **new_defer_tok = realloc(scope->defer_tok, sizeof(Token *) * new_cap);
  if (!new_defer_tok)
    error("out of memory growing defer scope");
  scope->defer_tok = new_defer_tok;

  scope->capacity = new_cap;
}

static void defer_add(Token *defer_keyword, Token *start, Token *end)
{
  if (defer_depth <= 0)
    error_tok(start, "defer outside of any scope");
  DeferScope *scope = &defer_stack[defer_depth - 1];
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
static void mark_switch_control_exit(void)
{
  for (int d = defer_depth - 1; d >= 0; d--)
  {
    if (!defer_stack[d].is_switch)
      continue;

    defer_stack[d].had_control_exit = true;
    return;
  }
}

// Clear defers at innermost switch scope when hitting case/default
// This is necessary because the transpiler can't track which case was entered at runtime.
// Note: This means defer in case with fallthrough will NOT preserve defers from previous cases.
// For reliable defer behavior in switch, wrap each case body in braces.
// Must clear defers at ALL scopes from current depth down to the switch scope,
// because case labels can appear inside nested blocks (e.g., Duff's device pattern).
static void clear_switch_scope_defers(void)
{
  // Find the switch scope and clear all scopes from current down to it
  for (int d = defer_depth - 1; d >= 0; d--)
  {
    // Clear defers at this scope
    defer_stack[d].count = 0;
    defer_stack[d].had_control_exit = false;

    // Stop when we hit the switch scope
    if (defer_stack[d].is_switch)
      return;
  }
}

// Check if a space is needed between two tokens
static bool needs_space(Token *prev, Token *tok)
{
  if (!prev)
    return false;
  if (tok->at_bol)
    return false; // newline will be emitted
  if (tok->has_space)
    return true;

  // Check if merging would create a different token
  // e.g., "int" + "x" -> "intx", "+" + "+" -> "++"
  char prev_last = prev->loc[prev->len - 1];
  char tok_first = tok->loc[0];

  // Identifier/keyword followed by identifier/keyword or number
  if ((prev->kind == TK_IDENT || prev->kind == TK_KEYWORD || prev->kind == TK_NUM) &&
      (tok->kind == TK_IDENT || tok->kind == TK_KEYWORD || tok->kind == TK_NUM))
    return true;

  // Punctuation that could merge
  if (prev->kind == TK_PUNCT && tok->kind == TK_PUNCT)
  {
    // Check common cases: ++ -- << >> && || etc.
    if ((prev_last == '+' && tok_first == '+') ||
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
        (prev_last == '*' && tok_first == '/'))
      return true;
  }

  return false;
}

// Check if token is a member access operator (. or ->)
// Used to distinguish keyword usage from struct member names
static bool is_member_access(Token *tok)
{
  return tok && tok->kind == TK_PUNCT && (equal(tok, ".") || equal(tok, "->"));
}

// Emit a single token with appropriate spacing
static void emit_tok(Token *tok)
{
  // Check if we need a #line directive BEFORE emitting the token
  bool need_line_directive = false;
  char *current_file = NULL;

  if (emit_line_directives && tok->file)
  {
    current_file = tok->file->display_name ? tok->file->display_name : tok->file->name;
    bool file_changed = (last_filename != current_file &&
                         (!last_filename || !current_file || strcmp(last_filename, current_file) != 0));
    bool line_jumped = (tok->line_no != last_line_no && tok->line_no != last_line_no + 1);
    need_line_directive = file_changed || line_jumped;
  }

  // Handle newlines and spacing
  if (tok->at_bol)
  {
    fputc('\n', out);
    // Emit #line directive on new line if needed
    if (need_line_directive)
    {
      fprintf(out, "#line %d \"%s\"\n", tok->line_no, current_file ? current_file : "unknown");
      last_line_no = tok->line_no;
      last_filename = current_file;
    }
    else if (emit_line_directives && tok->file && tok->line_no > last_line_no)
    {
      last_line_no = tok->line_no;
    }
  }
  else
  {
    // Not at beginning of line - emit #line before token if file/line changed significantly
    if (need_line_directive)
    {
      fputc('\n', out);
      fprintf(out, "#line %d \"%s\"\n", tok->line_no, current_file ? current_file : "unknown");
      last_line_no = tok->line_no;
      last_filename = current_file;
    }
    else if (needs_space(last_emitted, tok))
    {
      fputc(' ', out);
    }
  }

  fprintf(out, "%.*s", tok->len, tok->loc);
  last_emitted = tok;
}

// Emit tokens from start up to (but not including) end
static void emit_range(Token *start, Token *end)
{
  for (Token *t = start; t && t != end && t->kind != TK_EOF; t = t->next)
    emit_tok(t);
}

// Emit all defers for current scope (LIFO order)
static void emit_scope_defers(void)
{
  if (defer_depth <= 0)
    return;
  DeferScope *scope = &defer_stack[defer_depth - 1];
  for (int i = scope->count - 1; i >= 0; i--)
  {
    fputc(' ', out);
    emit_range(scope->stmts[i], scope->ends[i]);
    fputc(';', out);
  }
}

// Emit all defers from all scopes (for return statements)
static void emit_all_defers(void)
{
  for (int d = defer_depth - 1; d >= 0; d--)
  {
    DeferScope *scope = &defer_stack[d];
    for (int i = scope->count - 1; i >= 0; i--)
    {
      fputc(' ', out);
      emit_range(scope->stmts[i], scope->ends[i]);
      fputc(';', out);
    }
  }
}

// Emit defers for break - from current scope through innermost loop/switch
static void emit_break_defers(void)
{
  for (int d = defer_depth - 1; d >= 0; d--)
  {
    DeferScope *scope = &defer_stack[d];
    for (int i = scope->count - 1; i >= 0; i--)
    {
      fputc(' ', out);
      emit_range(scope->stmts[i], scope->ends[i]);
      fputc(';', out);
    }
    if (scope->is_loop || scope->is_switch)
      break;
  }
}

// Emit defers for continue - from current scope through innermost loop scope (inclusive)
// Continue jumps to loop update/condition, so all defers in the loop body must run
static void emit_continue_defers(void)
{
  for (int d = defer_depth - 1; d >= 0; d--)
  {
    DeferScope *scope = &defer_stack[d];
    for (int i = scope->count - 1; i >= 0; i--)
    {
      fputc(' ', out);
      emit_range(scope->stmts[i], scope->ends[i]);
      fputc(';', out);
    }
    if (scope->is_loop)
      break;
  }
}

// Check if there are any active defers
static bool has_active_defers(void)
{
  for (int d = 0; d < defer_depth; d++)
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
  for (int d = defer_depth - 1; d >= 0; d--)
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

// Ensure label_table has capacity for at least n labels
static void label_table_ensure_capacity(int n)
{
  if (n <= label_table.capacity)
    return;
  int new_cap = label_table.capacity == 0 ? INITIAL_LABELS : label_table.capacity * 2;
  while (new_cap < n)
    new_cap *= 2;
  LabelInfo *new_labels = realloc(label_table.labels, sizeof(LabelInfo) * new_cap);
  if (!new_labels)
    error("out of memory growing label table");
  label_table.labels = new_labels;
  label_table.capacity = new_cap;
}

static void label_table_add(char *name, int name_len, int scope_depth)
{
  label_table_ensure_capacity(label_table.count + 1);
  LabelInfo *info = &label_table.labels[label_table.count++];
  info->name = name;
  info->name_len = name_len;
  info->scope_depth = scope_depth;
}

static int label_table_lookup(char *name, int name_len)
{
  for (int i = 0; i < label_table.count; i++)
  {
    LabelInfo *info = &label_table.labels[i];
    if (info->name_len == name_len && !memcmp(info->name, name, name_len))
      return info->scope_depth;
  }
  return -1; // Not found
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
}

static void typedef_add(char *name, int len, int scope_depth, bool is_vla)
{
  // Grow if needed
  if (typedef_table.count >= typedef_table.capacity)
  {
    int new_cap = typedef_table.capacity == 0 ? 256 : typedef_table.capacity * 2;
    TypedefEntry *new_entries = realloc(typedef_table.entries, sizeof(TypedefEntry) * new_cap);
    if (!new_entries)
      error("out of memory tracking typedefs");
    typedef_table.entries = new_entries;
    typedef_table.capacity = new_cap;
  }
  TypedefEntry *e = &typedef_table.entries[typedef_table.count++];
  e->name = name;
  e->len = len;
  e->scope_depth = scope_depth;
  e->is_vla = is_vla;
  e->is_shadow = false;
  e->is_enum_const = false;
}

// Add a shadow entry: marks that a variable with this name exists at this scope,
// effectively hiding any typedef with the same name
static void typedef_add_shadow(char *name, int len, int scope_depth)
{
  // Grow if needed
  if (typedef_table.count >= typedef_table.capacity)
  {
    int new_cap = typedef_table.capacity == 0 ? 256 : typedef_table.capacity * 2;
    TypedefEntry *new_entries = realloc(typedef_table.entries, sizeof(TypedefEntry) * new_cap);
    if (!new_entries)
      error("out of memory tracking typedefs");
    typedef_table.entries = new_entries;
    typedef_table.capacity = new_cap;
  }
  TypedefEntry *e = &typedef_table.entries[typedef_table.count++];
  e->name = name;
  e->len = len;
  e->scope_depth = scope_depth;
  e->is_vla = false;
  e->is_shadow = true;
  e->is_enum_const = false;
}

// Add an enum constant entry
static void typedef_add_enum_const(char *name, int len, int scope_depth)
{
  // Grow if needed
  if (typedef_table.count >= typedef_table.capacity)
  {
    int new_cap = typedef_table.capacity == 0 ? 256 : typedef_table.capacity * 2;
    TypedefEntry *new_entries = realloc(typedef_table.entries, sizeof(TypedefEntry) * new_cap);
    if (!new_entries)
      error("out of memory tracking typedefs");
    typedef_table.entries = new_entries;
    typedef_table.capacity = new_cap;
  }
  TypedefEntry *e = &typedef_table.entries[typedef_table.count++];
  e->name = name;
  e->len = len;
  e->scope_depth = scope_depth;
  e->is_vla = false;
  e->is_shadow = false;
  e->is_enum_const = true;
}

// Called when exiting a scope - removes typedefs defined at that depth
static void typedef_pop_scope(int scope_depth)
{
  while (typedef_table.count > 0 && typedef_table.entries[typedef_table.count - 1].scope_depth == scope_depth)
    typedef_table.count--;
}

// Forward declaration for parse_enum_constants
static bool is_known_typedef(Token *tok);

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
      // Register this constant as a shadow if it matches any typedef
      if (is_known_typedef(tok))
        typedef_add_shadow(tok->loc, tok->len, scope_depth);
      else
        // Register as enum constant for array size detection
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
  if (tok->kind != TK_IDENT)
    return false;
  for (int i = typedef_table.count - 1; i >= 0; i--)
  {
    TypedefEntry *e = &typedef_table.entries[i];
    if (e->len == tok->len && !memcmp(e->name, tok->loc, tok->len))
      return !e->is_shadow;
  }
  return false;
}

// Check if token is a known VLA typedef (search most recent first for shadowing)
static bool is_vla_typedef(Token *tok)
{
  if (tok->kind != TK_IDENT)
    return false;
  for (int i = typedef_table.count - 1; i >= 0; i--)
  {
    TypedefEntry *e = &typedef_table.entries[i];
    if (e->len == tok->len && !memcmp(e->name, tok->loc, tok->len))
    {
      if (e->is_shadow)
        return false;
      return e->is_vla;
    }
  }
  return false;
}

// Check if token is a known enum constant (compile-time constant)
static bool is_known_enum_const(Token *tok)
{
  if (tok->kind != TK_IDENT)
    return false;
  for (int i = typedef_table.count - 1; i >= 0; i--)
  {
    TypedefEntry *e = &typedef_table.entries[i];
    if (e->len == tok->len && !memcmp(e->name, tok->loc, tok->len))
      return e->is_enum_const;
  }
  return false;
}

// Check if token could be a label (identifier followed by ':')
// Note: This can have false positives from ternary operator
// The caller should filter using context (prev token shouldn't be '?')
static bool is_label(Token *tok)
{
  return tok->kind == TK_IDENT && tok->next && equal(tok->next, ":");
}

// Scan a function body for labels and record their scope depths
// Also detects setjmp/longjmp/pthread_exit/vfork and inline asm usage
// tok should point to the opening '{' of the function body
static void scan_labels_in_function(Token *tok)
{
  label_table.count = 0;
  current_func_has_setjmp = false; // Reset for new function
  current_func_has_asm = false;    // Reset for new function
  current_func_has_vfork = false;  // Reset for new function
  if (!tok || !equal(tok, "{"))
    return;

  // Start at depth 1 to align with defer_depth (which is 1 inside function body)
  int depth = 1;
  int local_struct_depth = 0; // Track nesting inside struct/union/enum bodies
  Token *prev = NULL;
  tok = tok->next; // Skip opening brace

  while (tok && tok->kind != TK_EOF)
  {
    // Track struct/union/enum bodies to skip bitfield declarations
    if (equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum"))
    {
      // Look ahead for '{' - could be:
      //   "struct {"
      //   "struct name {"
      //   "struct __attribute__((packed)) {"
      //   "struct __attribute__((packed)) name {"
      Token *t = tok->next;
      // Skip identifiers, struct names, and __attribute__((...))
      while (t && (t->kind == TK_IDENT || equal(t, "__attribute__")))
      {
        // Handle __attribute__((...)) - skip the entire attribute
        if (equal(t, "__attribute__"))
        {
          t = t->next;
          // Skip (( ... ))
          if (t && equal(t, "("))
          {
            int paren_depth = 1;
            t = t->next;
            while (t && paren_depth > 0)
            {
              if (equal(t, "("))
                paren_depth++;
              else if (equal(t, ")"))
                paren_depth--;
              t = t->next;
            }
          }
        }
        else
          t = t->next;
      }
      if (t && equal(t, "{"))
      {
        // Skip to the opening brace
        while (tok != t)
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
      current_func_has_setjmp = true;

    // Detect vfork which has unpredictable control flow
    if (tok->kind == TK_IDENT && equal(tok, "vfork"))
      current_func_has_vfork = true;

    // Detect inline asm which may contain hidden jumps
    if (tok->kind == TK_KEYWORD && (equal(tok, "asm") || equal(tok, "__asm__") || equal(tok, "__asm")))
      current_func_has_asm = true;

    // Check for label: identifier followed by ':' (but not ::)
    // Filter out: ternary operator, switch cases, bitfields
    if (is_label(tok))
    {
      Token *colon = tok->next;
      // Make sure it's not :: (C++ scope resolution)
      bool is_scope_resolution = colon->next && equal(colon->next, ":");
      bool is_ternary = prev && equal(prev, "?");
      bool is_switch_case = prev && (equal(prev, "case") || equal(prev, "default"));
      bool is_bitfield = local_struct_depth > 0;

      if (!is_scope_resolution && !is_ternary && !is_switch_case && !is_bitfield)
        label_table_add(tok->loc, tok->len, depth);
    }

    prev = tok;
    tok = tok->next;
  }
}

// Emit defers for goto - from current scope down to target scope (inclusive)
// We emit defers for scopes we're EXITING, not the scope we're jumping TO
static void emit_goto_defers(int target_depth)
{
  for (int d = defer_depth - 1; d >= target_depth; d--)
  {
    DeferScope *scope = &defer_stack[d];
    for (int i = scope->count - 1; i >= 0; i--)
    {
      fputc(' ', out);
      emit_range(scope->stmts[i], scope->ends[i]);
      fputc(';', out);
    }
  }
}

// Check if goto needs defers (jumping out of scopes with defers)
static bool goto_has_defers(int target_depth)
{
  for (int d = defer_depth - 1; d >= target_depth; d--)
  {
    if (defer_stack[d].count > 0)
      return true;
  }
  return false;
}

// Check if a forward goto would skip over any defer statements
// This scans forward from goto_tok to find if any defer exists before the target label.
//
// Key distinction:
// - INVALID: goto jumps INTO a block, landing AFTER a defer inside that block
//   Example: goto inner; { defer X; inner: ... } -- X would run but wasn't registered
// - VALID: goto jumps OVER an entire block containing a defer
//   Example: goto done; { defer X; ... } done: -- we skip the whole block, defer never registered
//
// The rule: if we find the label BEFORE exiting the scope containing a defer, it's invalid.
static Token *goto_skips_defer(Token *goto_tok, char *label_name, int label_len)
{
  // Scan forward from goto to find the label
  Token *tok = goto_tok->next->next; // skip 'goto' and label name
  if (tok && equal(tok, ";"))
    tok = tok->next;

  int depth = 0;
  int local_struct_depth = 0;  // Track struct/union/enum bodies for bitfield filtering
  Token *active_defer = NULL;  // Most recently seen defer that's still "in scope"
  int active_defer_depth = -1; // Depth at which active_defer was found
  Token *prev = NULL;

  while (tok && tok->kind != TK_EOF)
  {
    // Track struct/union/enum bodies to skip bitfield declarations
    if (equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum"))
    {
      // Look ahead for '{' to detect struct body
      Token *t = tok->next;
      while (t && (t->kind == TK_IDENT || equal(t, "__attribute__")))
      {
        if (equal(t, "__attribute__"))
        {
          t = t->next;
          if (t && equal(t, "("))
          {
            int paren_depth = 1;
            t = t->next;
            while (t && paren_depth > 0)
            {
              if (equal(t, "("))
                paren_depth++;
              else if (equal(t, ")"))
                paren_depth--;
              t = t->next;
            }
          }
        }
        else
        {
          t = t->next;
        }
      }
      if (t && equal(t, "{"))
      {
        // Skip to the opening brace
        while (tok != t)
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
      // Exiting a scope - if we exit past where we found the defer, clear it
      // (means the goto is skipping the entire block, which is fine)
      if (active_defer && depth <= active_defer_depth)
      {
        active_defer = NULL;
        active_defer_depth = -1;
      }
      if (local_struct_depth > 0)
        local_struct_depth--;
      if (depth == 0)
        break; // End of containing scope, label not found here
      depth--;
      prev = tok;
      tok = tok->next;
      continue;
    }

    // Track defers we pass over
    // Skip if preceded by member access (. or ->) - that's a struct field, not keyword
    if (tok->kind == TK_KEYWORD && equal(tok, "defer") && !is_member_access(prev))
    {
      // Remember this defer (prefer shallowest depth if multiple)
      if (!active_defer || depth <= active_defer_depth)
      {
        active_defer = tok;
        active_defer_depth = depth;
      }
    }

    // Found the label? Apply same filtering as scan_labels_in_function
    if (is_label(tok) && tok->len == label_len &&
        !memcmp(tok->loc, label_name, label_len))
    {
      Token *colon = tok->next;
      // Filter out false positives:
      bool is_scope_resolution = colon->next && equal(colon->next, ":");
      bool is_ternary = prev && equal(prev, "?");
      bool is_switch_case = prev && (equal(prev, "case") || equal(prev, "default"));
      bool is_bitfield = local_struct_depth > 0;

      if (!is_scope_resolution && !is_ternary && !is_switch_case && !is_bitfield)
      {
        // Real label found - if we have an active defer, we're jumping past it
        return active_defer;
      }
    }

    prev = tok;
    tok = tok->next;
  }

  return NULL; // Label not found in forward scan, or all defers were in skipped blocks
}

// Check if a forward goto would skip over any variable declarations (with zero-init)
// This is undefined behavior in C - the declaration executes but initialization is skipped.
// We make this a hard error to maintain zero-init safety guarantees.
//
// Same logic as goto_skips_defer:
// - INVALID: goto jumps INTO a block, landing AFTER a declaration inside that block
// - VALID: goto jumps OVER an entire block containing a declaration
static Token *goto_skips_decl(Token *goto_tok, char *label_name, int label_len)
{
  if (!feature_zeroinit)
    return NULL; // Only check when zero-init is enabled

  // Scan forward from goto to find the label
  Token *tok = goto_tok->next->next; // skip 'goto' and label name
  if (tok && equal(tok, ";"))
    tok = tok->next;

  int depth = 0;
  int local_struct_depth = 0;
  Token *active_decl = NULL; // Most recently seen declaration that's still "in scope"
  int active_decl_depth = -1;
  Token *prev = NULL;
  bool at_stmt_start = true;

  while (tok && tok->kind != TK_EOF)
  {
    // Track struct/union/enum bodies
    if (equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum"))
    {
      Token *t = tok->next;
      while (t && (t->kind == TK_IDENT || equal(t, "__attribute__")))
      {
        if (equal(t, "__attribute__"))
        {
          t = t->next;
          if (t && equal(t, "("))
          {
            int paren_depth = 1;
            t = t->next;
            while (t && paren_depth > 0)
            {
              if (equal(t, "("))
                paren_depth++;
              else if (equal(t, ")"))
                paren_depth--;
              t = t->next;
            }
          }
        }
        else
          t = t->next;
      }
      if (t && equal(t, "{"))
      {
        while (tok != t)
        {
          prev = tok;
          tok = tok->next;
        }
        local_struct_depth++;
        depth++;
        prev = tok;
        tok = tok->next;
        at_stmt_start = false;
        continue;
      }
    }

    if (equal(tok, "{"))
    {
      depth++;
      prev = tok;
      tok = tok->next;
      at_stmt_start = true;
      continue;
    }
    if (equal(tok, "}"))
    {
      if (active_decl && depth <= active_decl_depth)
      {
        active_decl = NULL;
        active_decl_depth = -1;
      }
      if (local_struct_depth > 0)
        local_struct_depth--;
      if (depth == 0)
        break;
      depth--;
      prev = tok;
      tok = tok->next;
      at_stmt_start = true;
      continue;
    }
    if (equal(tok, ";"))
    {
      at_stmt_start = true;
      prev = tok;
      tok = tok->next;
      continue;
    }

    // Detect variable declarations at statement start (not inside struct bodies)
    // Skip extern/typedef/static declarations
    if (at_stmt_start && local_struct_depth == 0)
    {
      Token *decl_start = tok;

      // Skip 'raw' keyword if present
      if (equal(tok, "raw"))
        tok = tok->next;

      // Skip extern/typedef - these don't create initialized variables
      if (!equal(tok, "extern") && !equal(tok, "typedef"))
      {
        // Check if this looks like a declaration
        Token *t = tok;

        // Skip qualifiers and type keywords
        bool saw_type = false;
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
          saw_type = true;

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
            // This is a variable declaration - remember it
            if (!active_decl || depth <= active_decl_depth)
            {
              active_decl = decl_start;
              active_decl_depth = depth;
            }
          }
        }
      }
    }

    // Found the label?
    if (is_label(tok) && tok->len == label_len &&
        !memcmp(tok->loc, label_name, label_len))
    {
      Token *colon = tok->next;
      bool is_scope_resolution = colon->next && equal(colon->next, ":");
      bool is_ternary = prev && equal(prev, "?");
      bool is_switch_case = prev && (equal(prev, "case") || equal(prev, "default"));
      bool is_bitfield = local_struct_depth > 0;

      if (!is_scope_resolution && !is_ternary && !is_switch_case && !is_bitfield)
      {
        // Real label found - if we have an active decl, goto skips it
        return active_decl;
      }
    }

    at_stmt_start = false;
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

// Forward declaration for mutual recursion
static bool is_type_keyword(Token *tok);

// Skip attributes like __attribute__((...)) and __declspec(...)
static Token *skip_attributes(Token *tok)
{
  while (tok && (equal(tok, "__attribute__") || equal(tok, "__attribute") ||
                 equal(tok, "__declspec")))
  {
    tok = tok->next;
    if (tok && equal(tok, "("))
      tok = skip_balanced(tok, "(", ")");
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
                 equal(tok, "typedef") || equal(tok, "__attribute__") || equal(tok, "__attribute") ||
                 equal(tok, "__declspec")))
  {
    if (equal(tok, "__attribute__") || equal(tok, "__attribute") || equal(tok, "__declspec"))
      tok = skip_attributes(tok);
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
                 equal(tok, "__attribute__") || equal(tok, "__attribute") ||
                 equal(tok, "__declspec")))
  {
    if (equal(tok, "__attribute__") || equal(tok, "__attribute") || equal(tok, "__declspec"))
      tok = skip_attributes(tok);
    else
      tok = tok->next;
  }

  // Should be at function name followed by (
  if (tok && tok->kind == TK_IDENT && tok->next && equal(tok->next, "("))
    return true;

  return false;
}

// Skip the base type in a typedef (everything before the declarator)
static Token *scan_typedef_base_type(Token *tok)
{
  // Skip qualifiers: const, volatile, restrict, _Atomic
  while (tok && (equal(tok, "const") || equal(tok, "volatile") ||
                 equal(tok, "restrict") || equal(tok, "_Atomic") ||
                 equal(tok, "__const") || equal(tok, "__const__") ||
                 equal(tok, "__volatile") || equal(tok, "__volatile__") ||
                 equal(tok, "__restrict") || equal(tok, "__restrict__")))
    tok = tok->next;

  // Skip attributes
  tok = skip_attributes(tok);

  // Handle struct/union/enum
  if (tok && (equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum")))
  {
    tok = tok->next;

    // Skip attributes after keyword
    tok = skip_attributes(tok);

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
    tok = skip_attributes(tok);
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
  tok = skip_attributes(tok);

  // Case 1: Parenthesized declarator - (*name), (*name[N]), (*name)(args)
  if (tok && equal(tok, "("))
  {
    tok = tok->next;

    // Skip inner pointers and qualifiers
    while (tok && (equal(tok, "*") || equal(tok, "const") || equal(tok, "volatile") ||
                   equal(tok, "restrict") || equal(tok, "_Atomic")))
      tok = tok->next;

    tok = skip_attributes(tok);

    if (tok && tok->kind == TK_IDENT)
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
  if (tok && tok->kind == TK_IDENT)
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

// Forward declaration for is_const_array_size (used by typedef_contains_vla)
static bool is_const_array_size(Token *open_bracket);

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
static bool is_type_keyword(Token *tok)
{
  if (tok->kind != TK_KEYWORD && tok->kind != TK_IDENT)
    return false;
  // Standard C type keywords
  if (equal(tok, "int") || equal(tok, "char") || equal(tok, "short") ||
      equal(tok, "long") || equal(tok, "float") || equal(tok, "double") ||
      equal(tok, "void") || equal(tok, "signed") || equal(tok, "unsigned") ||
      equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum") ||
      equal(tok, "_Bool") || equal(tok, "bool"))
    return true;
  // typeof (GCC/C23 extension)
  if (equal(tok, "typeof") || equal(tok, "__typeof__") || equal(tok, "__typeof") ||
      equal(tok, "typeof_unqual"))
    return true;
  // Common typedef types from stdint.h, stddef.h, etc.
  if (equal(tok, "size_t") || equal(tok, "ssize_t") || equal(tok, "ptrdiff_t") ||
      equal(tok, "intptr_t") || equal(tok, "uintptr_t") ||
      equal(tok, "intmax_t") || equal(tok, "uintmax_t") ||
      equal(tok, "int8_t") || equal(tok, "int16_t") || equal(tok, "int32_t") || equal(tok, "int64_t") ||
      equal(tok, "uint8_t") || equal(tok, "uint16_t") || equal(tok, "uint32_t") || equal(tok, "uint64_t") ||
      equal(tok, "int_fast8_t") || equal(tok, "int_fast16_t") || equal(tok, "int_fast32_t") || equal(tok, "int_fast64_t") ||
      equal(tok, "uint_fast8_t") || equal(tok, "uint_fast16_t") || equal(tok, "uint_fast32_t") || equal(tok, "uint_fast64_t") ||
      equal(tok, "int_least8_t") || equal(tok, "int_least16_t") || equal(tok, "int_least32_t") || equal(tok, "int_least64_t") ||
      equal(tok, "uint_least8_t") || equal(tok, "uint_least16_t") || equal(tok, "uint_least32_t") || equal(tok, "uint_least64_t") ||
      equal(tok, "time_t") || equal(tok, "off_t") || equal(tok, "pid_t") ||
      equal(tok, "FILE") || equal(tok, "fpos_t") ||
      equal(tok, "wchar_t") || equal(tok, "wint_t"))
    return true;
  // User-defined typedefs (tracked during transpilation)
  if (is_known_typedef(tok))
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
         equal(tok, "__attribute__") || equal(tok, "__attribute");
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
  if (tok->len >= 2 && tok->loc[0] == '_' && tok->loc[1] == '_')
    return true;
  return false;
}

// Check if array size is a compile-time constant (not a VLA)
static bool is_const_array_size(Token *open_bracket)
{
  Token *tok = open_bracket->next;
  int depth = 1;
  bool has_only_literals = true;
  bool is_empty = true;

  while (tok->kind != TK_EOF && depth > 0)
  {
    if (equal(tok, "["))
      depth++;
    else if (equal(tok, "]"))
      depth--;
    else
    {
      is_empty = false;
      // sizeof and _Alignof/alignof always produce compile-time constants,
      // regardless of their argument (type name or expression).
      // Skip their parenthesized argument entirely.
      if (equal(tok, "sizeof") || equal(tok, "_Alignof") || equal(tok, "alignof"))
      {
        tok = tok->next;
        if (tok && equal(tok, "("))
        {
          int paren_depth = 1;
          tok = tok->next;
          while (tok->kind != TK_EOF && paren_depth > 0)
          {
            if (equal(tok, "("))
              paren_depth++;
            else if (equal(tok, ")"))
              paren_depth--;
            tok = tok->next;
          }
        }
        continue;
      }
      // Allow numeric literals and basic operators
      if (tok->kind != TK_NUM &&
          !equal(tok, "+") && !equal(tok, "-") && !equal(tok, "*") &&
          !equal(tok, "/") && !equal(tok, "%") && !equal(tok, "(") && !equal(tok, ")") &&
          !equal(tok, "<<") && !equal(tok, ">>") && !equal(tok, "&") &&
          !equal(tok, "|") && !equal(tok, "^") && !equal(tok, "~") &&
          !equal(tok, "!") && !equal(tok, "<") && !equal(tok, ">") &&
          !equal(tok, "<=") && !equal(tok, ">=") && !equal(tok, "==") &&
          !equal(tok, "!=") && !equal(tok, "&&") && !equal(tok, "||") &&
          !equal(tok, "?") && !equal(tok, ":"))
      {
        // Identifiers are only allowed if they're:
        // - Known enum constants (compile-time constants)
        // - Known typedefs (used in casts like (MyType)0)
        // - Type keywords (used in casts like (int)0)
        // - System typedefs (names ending in _t, like rlim_t, size_t)
        if (tok->kind == TK_IDENT && !is_known_enum_const(tok) &&
            !is_known_typedef(tok) && !is_type_keyword(tok) &&
            !looks_like_system_typedef(tok))
        {
          has_only_literals = false;
        }
      }
    }
    tok = tok->next;
  }
  return is_empty || has_only_literals;
}

// Try to handle a declaration with zero-init
// Supports multi-declarators like: int a, b, *c, d[10];
// Returns the token after the declaration if handled, NULL otherwise
//
// SAFETY: If we see a type but fail to parse the declarator, we emit a warning
// to alert the user that zero-init may have been skipped.
//
// Debug: Compile with -DPRISM_DEBUG_ZEROINIT to trace parsing
static Token *try_zero_init_decl(Token *tok)
{
  if (!feature_zeroinit || defer_depth <= 0 || struct_depth > 0)
    return NULL;

  Token *start = tok;
  Token *decl_start_for_warning = tok; // Remember for potential warning

  // Check for 'raw' keyword - skip zero-init for this declaration
  bool is_raw = false;
  if (equal(tok, "raw"))
  {
    is_raw = true;
    tok = tok->next;
    start = tok; // Don't emit 'raw'
    decl_start_for_warning = tok;
  }

  if (is_skip_decl_keyword(tok)) // Skip extern/typedef
    return NULL;

  // Must start with qualifier or type
  bool saw_type = false;
  bool is_struct_type = false;
  bool is_typedef_type = false;
  bool is_typedef_vla = false; // True if the typedef refers to a VLA
  Token *type_end = tok;       // Will point to first token after the base type

  while (is_type_qualifier(tok) || is_type_keyword(tok))
  {
    if (is_type_keyword(tok))
      saw_type = true;
    // Handle struct/union/enum followed by optional tag
    if (equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum"))
    {
      is_struct_type = true;
      tok = tok->next;
      if (tok->kind == TK_IDENT)
        tok = tok->next;
      // Could have { body } - skip it
      if (equal(tok, "{"))
        tok = skip_balanced(tok, "{", "}");
      saw_type = true;
      type_end = tok;
      continue;
    }
    // Handle typeof/typeof_unqual with parenthesized expression
    if (equal(tok, "typeof") || equal(tok, "__typeof__") || equal(tok, "__typeof") ||
        equal(tok, "typeof_unqual"))
    {
      saw_type = true;
      tok = tok->next;
      if (tok && equal(tok, "("))
        tok = skip_balanced(tok, "(", ")");
      type_end = tok;
      continue;
    }
    // Handle _Atomic(type) specifier form (different from _Atomic as qualifier)
    if (equal(tok, "_Atomic") && tok->next && equal(tok->next, "("))
    {
      saw_type = true;
      tok = tok->next;                    // skip _Atomic
      tok = skip_balanced(tok, "(", ")"); // skip (type)
      type_end = tok;
      continue;
    }
    // Handle _Alignas/alignas and __attribute__ with parenthesized arguments
    if (equal(tok, "_Alignas") || equal(tok, "alignas") ||
        equal(tok, "__attribute__") || equal(tok, "__attribute"))
    {
      tok = tok->next;
      if (tok && equal(tok, "("))
        tok = skip_balanced(tok, "(", ")");
      type_end = tok;
      continue;
    }
    // Check for user-defined typedef (needs {0} initialization for structs)
    if (is_known_typedef(tok))
    {
      is_typedef_type = true;
      if (is_vla_typedef(tok))
        is_typedef_vla = true;
      // CRITICAL: If the next token looks like a declarator, stop here.
      // This prevents "T T;" from consuming both Ts as the type.
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
          type_end = tok;
          break; // Exit loop - next token is variable name, not part of type
        }
      }
    }
    tok = tok->next;
    type_end = tok;
  }

  // Check for typedef'd type: identifier followed by identifier (no pointer)
  // Pattern: my_type x; - but NOT my_type *x; (ambiguous with multiplication)
  // Also ensure the identifier is actually a known typedef (not shadowed)
  if (!saw_type && tok->kind == TK_IDENT && is_known_typedef(tok))
  {
    Token *maybe_type = tok;
    Token *t = tok->next;

    // Only match direct "TypeName varname" pattern (no * which is ambiguous)
    // Skip only qualifiers, NOT pointers
    while (t && is_type_qualifier(t))
      t = t->next;

    // If directly followed by identifier (no *) then ; or [ or , this is likely a declaration
    if (t && t->kind == TK_IDENT && !equal(maybe_type->next, "*"))
    {
      Token *after_name = t->next;
      if (after_name && (equal(after_name, ";") || equal(after_name, "[") ||
                         equal(after_name, ",") || equal(after_name, "=")))
      {
        saw_type = true;
        is_typedef_type = true;
        // Check if this is a VLA typedef
        if (is_vla_typedef(maybe_type))
          is_typedef_vla = true;
        tok = maybe_type->next;
        type_end = tok;
      }
    }
  }

  if (!saw_type)
    return NULL;

  // Before emitting anything, verify there's at least one declarator
  // Look for: pointers/qualifiers followed by identifier (possibly inside nested parens)
  Token *check = tok;
  while (equal(check, "*") || is_type_qualifier(check))
  {
    // Skip __attribute__((...)) entirely
    if (equal(check, "__attribute__") || equal(check, "__attribute"))
    {
      check = check->next;
      if (check && equal(check, "("))
      {
        int attr_depth = 1;
        check = check->next;
        while (check && check->kind != TK_EOF && attr_depth > 0)
        {
          if (equal(check, "("))
            attr_depth++;
          else if (equal(check, ")"))
            attr_depth--;
          check = check->next;
        }
      }
      continue;
    }
    check = check->next;
  }

  // Handle parenthesized declarators - may be arbitrarily nested: (*name), (*(*name)(args))[N], etc.
  // We need to find an identifier somewhere inside the parentheses
  int paren_depth = 0;
  bool found_ident = false;
  Token *scan = check;

  if (equal(scan, "("))
  {
    // Scan through nested parens to find an identifier
    paren_depth = 1;
    scan = scan->next;

    while (scan && scan->kind != TK_EOF && paren_depth > 0)
    {
      if (equal(scan, "("))
        paren_depth++;
      else if (equal(scan, ")"))
        paren_depth--;
      else if (scan->kind == TK_IDENT && !found_ident)
      {
        // Found potential identifier - verify it's not a type keyword
        // (could be a typedef or cast)
        if (!is_type_keyword(scan) && !is_known_typedef(scan))
          found_ident = true;
      }
      scan = scan->next;
    }

    if (!found_ident)
    {
      // Saw type but couldn't find declarator identifier in parens
      // This might be a cast or complex pattern we don't support
      // Emit warning for safety
      fprintf(stderr, "%s:%d: warning: zero-init: complex parenthesized pattern not parsed, "
                      "variable may be uninitialized. Consider adding explicit initializer.\n",
              decl_start_for_warning->file->name, decl_start_for_warning->line_no);
      return NULL;
    }
    check = scan; // Continue checking after the paren group
  }
  else
  {
    // No parens - must have identifier directly
    if (check->kind != TK_IDENT)
      return NULL;
  }

  // Must have identifier - if not, this is just a type declaration (e.g., struct Foo {};)
  // (This check is now partially redundant but kept for clarity)
  if (!found_ident && check->kind != TK_IDENT)
    return NULL;

  // Before emitting anything, check if any declarator has a statement expression initializer
  // Statement expressions like ({ ... }) can contain defer, so we need the main loop to handle them
  {
    Token *scan = tok;
    int scan_depth = 0;
    while (scan && scan->kind != TK_EOF)
    {
      if (equal(scan, "(") || equal(scan, "[") || equal(scan, "{"))
      {
        // Check for statement expression: '(' followed by '{'
        if (equal(scan, "(") && scan->next && equal(scan->next, "{"))
          return NULL; // Let main loop handle statement expressions
        scan_depth++;
      }
      else if (equal(scan, ")") || equal(scan, "]") || equal(scan, "}"))
        scan_depth--;
      else if (scan_depth == 0 && equal(scan, ";"))
        break; // End of declaration
      scan = scan->next;
    }
  }

  // Now we've confirmed there's at least one declarator. Emit the base type.
  emit_range(start, type_end);

  // Process each declarator in the list
  bool first_declarator = true;
  while (tok && tok->kind != TK_EOF)
  {
    // For subsequent declarators, we already emitted the comma
    Token *decl_start = tok;

    // Parse this declarator's pointer modifiers
    bool is_pointer = false;
    while (equal(tok, "*") || is_type_qualifier(tok))
    {
      if (equal(tok, "*"))
        is_pointer = true;
      // Handle __attribute__((...)) - skip the entire attribute including parens
      if (equal(tok, "__attribute__") || equal(tok, "__attribute"))
      {
        emit_tok(tok);
        tok = tok->next;
        if (tok && equal(tok, "("))
        {
          // Skip the outer and inner parens: __attribute__((xxx))
          emit_tok(tok); // (
          tok = tok->next;
          int attr_depth = 1;
          while (tok && tok->kind != TK_EOF && attr_depth > 0)
          {
            if (equal(tok, "("))
              attr_depth++;
            else if (equal(tok, ")"))
              attr_depth--;
            emit_tok(tok);
            tok = tok->next;
          }
        }
        continue;
      }
      emit_tok(tok);
      tok = tok->next;
    }

    // Handle parenthesized declarators: (*name), (*name)[N], (*name)(args)
    // Also handles nested patterns like (*(*name)(args))[N]
    bool has_paren_declarator = false;
    bool array_inside_paren = false;
    int nested_paren_count = 0; // Track nesting level for complex declarators

    if (equal(tok, "("))
    {
      Token *paren_start = tok;
      Token *peek = tok->next;

      // Should be * for pointer declarator (or another ( for nested)
      if (!equal(peek, "*") && !equal(peek, "("))
      {
        // Saw type, started declarator, but pattern not recognized
        fprintf(stderr, "%s:%d: warning: zero-init: parenthesized declarator pattern not recognized, "
                        "variable may be uninitialized. Consider adding explicit initializer.\n",
                decl_start_for_warning->file->name, decl_start_for_warning->line_no);
        return NULL; // Not a pointer declarator, bail
      }

      emit_tok(tok); // emit '('
      tok = tok->next;
      nested_paren_count = 1;
      is_pointer = true;

      // Handle arbitrary nesting: (*(*(*name)...
      while (equal(tok, "*") || is_type_qualifier(tok) || equal(tok, "("))
      {
        if (equal(tok, "*"))
          is_pointer = true;
        else if (equal(tok, "("))
          nested_paren_count++;
        // Handle __attribute__((...)) inside parenthesized declarator
        if (equal(tok, "__attribute__") || equal(tok, "__attribute"))
        {
          emit_tok(tok);
          tok = tok->next;
          if (tok && equal(tok, "("))
          {
            emit_tok(tok);
            tok = tok->next;
            int attr_depth = 1;
            while (tok && tok->kind != TK_EOF && attr_depth > 0)
            {
              if (equal(tok, "("))
                attr_depth++;
              else if (equal(tok, ")"))
                attr_depth--;
              emit_tok(tok);
              tok = tok->next;
            }
          }
          continue;
        }
        emit_tok(tok);
        tok = tok->next;
      }

      // Must be identifier now
      if (tok->kind != TK_IDENT)
      {
        fprintf(stderr, "%s:%d: warning: zero-init: expected identifier in declarator, "
                        "variable may be uninitialized. Consider adding explicit initializer.\n",
                decl_start_for_warning->file->name, decl_start_for_warning->line_no);
        return NULL;
      }

      has_paren_declarator = true;
    }

    // Must have identifier
    if (tok->kind != TK_IDENT)
    {
      // We've emitted type but no identifier - shouldn't happen if validation worked
      fprintf(stderr, "%s:%d: warning: zero-init: expected identifier after type, "
                      "variable may be uninitialized. Consider adding explicit initializer.\n",
              decl_start_for_warning->file->name, decl_start_for_warning->line_no);
      return NULL;
    }

    Token *var_name = tok;
    emit_tok(tok);
    tok = tok->next;

    // Array dimension INSIDE parentheses: (*name[N])
    if (has_paren_declarator && equal(tok, "["))
    {
      array_inside_paren = true;
      while (equal(tok, "["))
      {
        emit_tok(tok);
        tok = tok->next;
        // Track balanced brackets and parens (for sizeof(x[0]) etc.)
        int bracket_depth = 1;
        int paren_depth = 0;
        while (tok->kind != TK_EOF && bracket_depth > 0)
        {
          if (equal(tok, "["))
            bracket_depth++;
          else if (equal(tok, "]"))
            bracket_depth--;
          else if (equal(tok, "("))
            paren_depth++;
          else if (equal(tok, ")"))
            paren_depth--;
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
    }

    // Close all nested parens for parenthesized declarator
    // Each paren level may be followed by function args or array dims
    while (has_paren_declarator && nested_paren_count > 0)
    {
      // Handle function args at this level: (*name)(args) or array dims
      while (equal(tok, "(") || equal(tok, "["))
      {
        if (equal(tok, "("))
        {
          // Function parameter list
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
        }
        else if (equal(tok, "["))
        {
          // Array dimension
          array_inside_paren = true;
          while (equal(tok, "["))
          {
            emit_tok(tok);
            tok = tok->next;
            // Track balanced brackets and parens (for sizeof(x[0]) etc.)
            int bracket_depth = 1;
            int paren_depth = 0;
            while (tok->kind != TK_EOF && bracket_depth > 0)
            {
              if (equal(tok, "["))
                bracket_depth++;
              else if (equal(tok, "]"))
                bracket_depth--;
              else if (equal(tok, "("))
                paren_depth++;
              else if (equal(tok, ")"))
                paren_depth--;
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
        }
      }

      // Now close this paren level
      if (!equal(tok, ")"))
      {
        fprintf(stderr, "%s:%d: warning: zero-init: expected ')' in declarator, "
                        "variable may be uninitialized. Consider adding explicit initializer.\n",
                decl_start_for_warning->file->name, decl_start_for_warning->line_no);
        return NULL;
      }
      emit_tok(tok);
      tok = tok->next;
      nested_paren_count--;
    }

    // Check what follows the identifier
    bool is_array = array_inside_paren;
    bool is_vla = false;
    bool is_func_ptr = false;

    // Function pointer: (*name)(args)
    if (equal(tok, "("))
    {
      if (!has_paren_declarator)
        return NULL; // Regular function declaration
      is_func_ptr = true;
      // Emit the function args
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
    }

    // Array dimensions
    if (equal(tok, "["))
    {
      is_array = true;
      while (equal(tok, "["))
      {
        // Check if VLA before emitting
        if (!is_const_array_size(tok))
          is_vla = true;
        emit_tok(tok); // emit opening '['
        tok = tok->next;
        // Track balanced brackets and parens within the array dimension
        // This handles cases like: buf[sizeof(arr[0])] or buf[(a+b)*c]
        int bracket_depth = 1; // for [ ]
        int paren_depth = 0;   // for ( )
        while (tok->kind != TK_EOF && bracket_depth > 0)
        {
          if (equal(tok, "["))
            bracket_depth++;
          else if (equal(tok, "]"))
            bracket_depth--;
          else if (equal(tok, "("))
            paren_depth++;
          else if (equal(tok, ")"))
            paren_depth--;
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
    }

    // Check if this declarator already has an initializer
    bool has_init = equal(tok, "=");

    // Combine direct VLA detection with typedef VLA detection
    // But pointers to VLAs are NOT VLAs - they can be zero-initialized to NULL
    bool effective_vla = (is_vla || is_typedef_vla) && !is_pointer;

    // VLAs cannot be initialized - this is a hard error, no bypass allowed
    // C syntax doesn't allow `int arr[n] = {0};` so VLAs break safety guarantees
    /* // todo make this "strict" mode, intended for prism only codebases
    if (effective_vla && !has_init)
    {
      error_tok(var_name, "VLA '%.*s' cannot be zero-initialized (C language limitation). "
                          "Options: (1) Use a fixed-size array, (2) Use malloc()+memset(), "
                          "(3) Add explicit memset() after declaration, or "
                          "(4) Use 'prism no-zeroinit' to disable zero-init globally.",
                var_name->len, var_name->loc);
    }
    */

    // Add zero initializer if no existing initializer, not a VLA, and not raw
    if (!has_init && !effective_vla && !is_raw)
    {
      if (is_array || ((is_struct_type || is_typedef_type) && !is_pointer))
        fprintf(out, " = {0}");
      else
        fprintf(out, " = 0");
    }

    // If has initializer, emit it (= and everything up to , or ;)
    if (has_init)
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

    // Register shadow if this variable name matches a typedef
    // This ensures subsequent uses of this name in the same scope
    // are treated as variables, not types
    if (is_known_typedef(var_name))
    {
      typedef_add_shadow(var_name->loc, var_name->len, defer_depth);
    }

    // Check what's next
    if (equal(tok, ";"))
    {
      emit_tok(tok);
      return tok->next;
    }
    else if (equal(tok, ","))
    {
      emit_tok(tok);
      tok = tok->next;
      first_declarator = false;
      // Continue to next declarator
    }
    else
      return NULL;
  }

  return NULL;
}

static int transpile(char *input_file, char *output_file)
{
  pp_init();
  pp_add_default_include_paths();

  // Add extra include paths from CLI (-I flags)
  for (int i = 0; i < extra_include_count; i++)
    pp_add_include_path(extra_include_paths[i]);

  // Add extra defines from CLI (-D flags)
  for (int i = 0; i < extra_define_count; i++)
  {
    const char *def = extra_defines[i];
    char *eq = strchr(def, '=');
    if (eq)
    {
      // -DNAME=VALUE
      size_t name_len = eq - def;
      char *name = malloc(name_len + 1);
      memcpy(name, def, name_len);
      name[name_len] = '\0';
      // Note: pp_define_macro keeps pointer to name, so don't free it
      pp_define_macro(name, (char *)(eq + 1));
    }
    else
    {
      // -DNAME (define as 1)
      pp_define_macro((char *)def, "1");
    }
  }

  pp_define_macro("__PRISM__", "1");
  if (feature_defer)
    pp_define_macro("__PRISM_DEFER__", "1");
  if (feature_zeroinit)
    pp_define_macro("__PRISM_ZEROINIT__", "1");

  Token *tok = tokenize_file(input_file);
  if (!tok)
  {
    fprintf(stderr, "Failed to open: %s\n", input_file);
    return 0;
  }

  tok = preprocess(tok);

  out = fopen(output_file, "w");
  if (!out)
    return 0;

  // Emit feature test macros at the very start of the output
  // These must come before any #include <...> directives for glibc to work correctly
  {
    const char **ftm_names;
    const char **ftm_values;
    int ftm_count = pp_get_feature_test_macros(&ftm_names, &ftm_values);
    for (int i = 0; i < ftm_count; i++)
    {
      fprintf(out, "#define %s %s\n", ftm_names[i], ftm_values[i]);
    }
    // Always include errno.h for error constants like EINVAL, ENOENT, etc.
    // Many C programs use these constants without explicitly including errno.h,
    // relying on transitive includes that prism's preprocessing may not preserve.
    fprintf(out, "#include <errno.h>\n");
  }

  // Reset state
  defer_depth = 0;
  struct_depth = 0;
  last_emitted = NULL;
  last_line_no = 0;
  last_filename = NULL;
  next_scope_is_loop = false;
  next_scope_is_switch = false;
  pending_control_flow = false;
  control_paren_depth = 0;
  in_for_init = false;
  pending_for_paren = false;
  current_func_returns_void = false;
  stmt_expr_count = 0;
  at_stmt_start = true;                // Start of file is start of statement
  typedef_table_reset();               // Reset typedef tracking
  bool next_func_returns_void = false; // Track void functions at top level
  Token *prev_toplevel_tok = NULL;     // Track previous token at top level for function detection

  // Walk tokens and emit
  while (tok->kind != TK_EOF)
  {
    // Track typedefs for zero-init (must happen before zero-init check)
    // Only at statement start and outside struct/union/enum bodies
    if (at_stmt_start && struct_depth == 0 && equal(tok, "typedef"))
      parse_typedef_declaration(tok, defer_depth); // Fall through to emit the typedef normally

    // Try zero-init for declarations at statement start
    // Also allow in the init clause of a for loop: for (int i; ...)
    if (at_stmt_start && (!pending_control_flow || in_for_init))
    {
      Token *next = try_zero_init_decl(tok);
      if (next)
      {
        tok = next;
        at_stmt_start = true; // Still at statement start after decl
        continue;
      }
    }
    at_stmt_start = false;

    // Warn about noreturn functions that bypass defer cleanup
    // These functions terminate the program without running defers - RAII violation
    // Also mark as control exit for switch fallthrough detection (they don't fall through)
    if (feature_defer && tok->kind == TK_IDENT && tok->next && equal(tok->next, "("))
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
                  tok->file->name, tok->line_no, tok->len, tok->loc);
        }
      }
    }

    // Handle 'defer' keyword
    // Skip if: preceded by member access (. or ->) - that's a struct field, not keyword
    //          inside struct/union/enum body - that's a field declaration, not keyword
    if (feature_defer && tok->kind == TK_KEYWORD && equal(tok, "defer") &&
        !is_member_access(last_emitted) && struct_depth == 0)
    {
      // Check for defer inside for/while/switch/if parentheses - this is invalid
      if (pending_control_flow && control_paren_depth > 0)
        error_tok(tok, "defer cannot appear inside control statement parentheses");

      // Check for braceless control flow - defer needs a proper scope
      if (pending_control_flow)
        error_tok(tok, "defer cannot be the body of a braceless control statement - add braces");

      // Check for defer at the top-level of a statement expression - semantics are problematic
      // In ({ defer X; expr; }), the defer would execute after expr, making the result void
      // But defer in nested blocks inside stmt expr is OK: ({ { defer X; } expr; })
      for (int i = 0; i < stmt_expr_count; i++)
      {
        if (defer_depth == stmt_expr_levels[i])
        {
          error_tok(tok, "defer cannot be used at the top level of statement expressions ({ ... }). "
                         "The defer would execute after the final expression, changing the return type to void. "
                         "Wrap the defer in a block: ({ { defer X; ... } result; })");
          break;
        }
      }

      // setjmp/longjmp/pthread_exit bypasses defer cleanup - this MUST be an error
      if (current_func_has_setjmp)
      {
        error_tok(tok, "defer cannot be used in functions that call setjmp/longjmp/pthread_exit. "
                       "These functions bypass defer cleanup entirely, causing resource leaks. "
                       "Use explicit cleanup patterns (goto cleanup, or manual RAII) instead.");
      }

      // vfork has unpredictable control flow that can bypass cleanup
      if (current_func_has_vfork)
      {
        error_tok(tok, "defer cannot be used in functions that call vfork(). "
                       "vfork shares address space with parent and has unpredictable control flow. "
                       "Use fork() instead, or move defer to a wrapper function.");
      }

      // Inline asm may contain hidden jumps that bypass defer
      if (current_func_has_asm)
      {
        error_tok(tok, "defer cannot be used in functions containing inline assembly. "
                       "Inline asm may contain jumps (jmp, call, etc.) that bypass defer cleanup. "
                       "Move the asm to a separate function, or use explicit cleanup instead.");
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
        // Check at_bol BEFORE updating depths, skip for grouping tokens themselves
        // Allow multi-line when inside any grouping: (), [], {}
        bool at_top_level = (brace_depth == 0 && paren_depth == 0 && bracket_depth == 0);
        if (t != stmt_start && t->at_bol && at_top_level &&
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
    if (feature_defer && tok->kind == TK_KEYWORD && equal(tok, "return"))
    {
      mark_switch_control_exit(); // Mark that we exited via return
      if (has_active_defers())
      {
        tok = tok->next; // skip 'return'

        // Check if there's an expression or just "return;"
        if (equal(tok, ";"))
        {
          // void return: { defers; return; }
          fprintf(out, " {");
          emit_all_defers();
          fprintf(out, " return;");
          tok = tok->next;
          fprintf(out, " }");
        }
        else
        {
          // return with expression
          // Check if expression is a void cast: (void)expr - treat as void return
          // This handles typedef void cases like: VoidType func() { return (void)expr; }
          bool is_void_cast = equal(tok, "(") && tok->next && equal(tok->next, "void") &&
                              tok->next->next && equal(tok->next->next, ")");

          if (current_func_returns_void || is_void_cast)
          {
            // void function: { (expr); defers; return; }
            // The expression is executed for side effects, then we return void
            fprintf(out, " { (");
            while (tok->kind != TK_EOF && !equal(tok, ";"))
            {
              emit_tok(tok);
              tok = tok->next;
            }
            fprintf(out, ");");
            emit_all_defers();
            fprintf(out, " return;");
            if (equal(tok, ";"))
              tok = tok->next;
            fprintf(out, " }");
          }
          else
          {
            // non-void function: { __auto_type _ret = (expr); defers; return _ret; }
            static unsigned long long ret_counter = 0;
            unsigned long long my_ret = ret_counter++;
            fprintf(out, " { __auto_type _prism_ret_%llu = (", my_ret);

            // Emit expression until semicolon
            while (tok->kind != TK_EOF && !equal(tok, ";"))
            {
              emit_tok(tok);
              tok = tok->next;
            }

            fprintf(out, ");");
            emit_all_defers();
            fprintf(out, " return _prism_ret_%llu;", my_ret);

            if (equal(tok, ";"))
              tok = tok->next;

            fprintf(out, " }");
          }
        }
        end_statement_after_semicolon();
        continue;
      }
      // No defers, emit normally
    }

    // Handle 'break' - emit defers up through loop/switch
    if (feature_defer && tok->kind == TK_KEYWORD && equal(tok, "break"))
    {
      mark_switch_control_exit(); // Mark that we exited via break
      if (control_flow_has_defers(true))
      {
        fprintf(out, " {");
        emit_break_defers();
        fprintf(out, " break; }");
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
    if (feature_defer && tok->kind == TK_KEYWORD && equal(tok, "continue"))
    {
      mark_switch_control_exit(); // Continue also exits the switch (like break/return/goto)
      if (control_flow_has_defers(false))
      {
        fprintf(out, " {");
        emit_continue_defers();
        fprintf(out, " continue; }");
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
    if (feature_defer && tok->kind == TK_KEYWORD && equal(tok, "goto"))
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

      // Get the label name
      if (tok->kind == TK_IDENT)
      {
        // Check if this goto would skip over a defer statement
        Token *skipped = goto_skips_defer(goto_tok, tok->loc, tok->len);
        if (skipped)
          error_tok(skipped, "goto '%.*s' would skip over this defer statement",
                    tok->len, tok->loc);

        // Check if this goto would skip over a variable declaration (zero-init safety)
        Token *skipped_decl = goto_skips_decl(goto_tok, tok->loc, tok->len);
        if (skipped_decl)
          error_tok(skipped_decl, "goto '%.*s' would skip over this variable declaration, "
                                  "bypassing zero-initialization (undefined behavior in C). "
                                  "Move the declaration before the goto, or restructure the code.",
                    tok->len, tok->loc);

        int target_depth = label_table_lookup(tok->loc, tok->len);
        // If label not found, assume same depth (forward reference within scope)
        if (target_depth < 0)
          target_depth = defer_depth;

        if (goto_has_defers(target_depth))
        {
          fprintf(out, " {");
          emit_goto_defers(target_depth);
          fprintf(out, " goto");
          emit_tok(tok); // label name
          tok = tok->next;
          if (equal(tok, ";"))
          {
            emit_tok(tok);
            tok = tok->next;
          }
          fprintf(out, " }");
          end_statement_after_semicolon();
          continue;
        }
      }
      // No defers or couldn't parse, emit normally
      emit_tok(goto_tok);
      continue;
    }

    // Check goto for zeroinit safety even when defer is disabled
    if (feature_zeroinit && !feature_defer && tok->kind == TK_KEYWORD && equal(tok, "goto"))
    {
      Token *goto_tok = tok;
      tok = tok->next;
      if (tok->kind == TK_IDENT)
      {
        Token *skipped_decl = goto_skips_decl(goto_tok, tok->loc, tok->len);
        if (skipped_decl)
          error_tok(skipped_decl, "goto '%.*s' would skip over this variable declaration, "
                                  "bypassing zero-initialization (undefined behavior in C). "
                                  "Move the declaration before the goto, or restructure the code.",
                    tok->len, tok->loc);
      }
      emit_tok(goto_tok);
      continue;
    }

    // Mark loop keywords so next '{' knows it's a loop scope
    if (feature_defer && tok->kind == TK_KEYWORD &&
        (equal(tok, "for") || equal(tok, "while") || equal(tok, "do")))
    {
      next_scope_is_loop = true;
      pending_control_flow = true;
      // For 'for' loops, we need to allow zero-init in the init clause
      if (equal(tok, "for"))
        pending_for_paren = true;
    }
    // Also track 'for' for zero-init even if defer is disabled
    else if (feature_zeroinit && !feature_defer && tok->kind == TK_KEYWORD && equal(tok, "for"))
    {
      pending_control_flow = true;
      pending_for_paren = true;
    }

    // Mark switch keyword
    if (feature_defer && tok->kind == TK_KEYWORD && equal(tok, "switch"))
    {
      next_scope_is_switch = true;
      pending_control_flow = true;
    }

    // Mark if/else keywords
    if (tok->kind == TK_KEYWORD && (equal(tok, "if") || equal(tok, "else")))
      pending_control_flow = true;

    // Handle case/default labels - clear defers from switch scope
    // This prevents defers from leaking across cases (which would cause incorrect behavior
    // since the transpiler can't know which case is entered at runtime)
    if (feature_defer && tok->kind == TK_KEYWORD && (equal(tok, "case") || equal(tok, "default")))
    {
      // Check if there are active defers that would be lost (fallthrough scenario)
      // Must check ALL scopes from current depth down to the switch scope,
      // because case labels can appear inside nested blocks
      for (int d = defer_depth - 1; d >= 0; d--)
      {
        // Check for defers at this scope that would be cleared
        if (defer_stack[d].count > 0 && !defer_stack[d].had_control_exit)
        {
          // There are defers that will be cleared - this is a resource leak!
          // Make it an error to force the user to fix it.
          error_tok(defer_stack[d].defer_tok[0],
                    "defer would be skipped due to switch fallthrough at %s:%d. "
                    "Add 'break;' before the next case, or wrap case body in braces.",
                    tok->file->name, tok->line_no);
        }

        // Stop when we hit the switch scope
        if (defer_stack[d].is_switch)
          break;
      }
      clear_switch_scope_defers();
    }

    // Detect function definition and scan for labels
    // Pattern: identifier '(' ... ')' '{'
    // Only trigger when previous token at top level was ')' (end of parameter list)
    if (feature_defer && equal(tok, "{") && defer_depth == 0)
    {
      // Only scan for labels if this looks like a function body (prev token is ')')
      // This avoids false positives from: int arr[] = {1,2,3}; or compound literals
      if (prev_toplevel_tok && equal(prev_toplevel_tok, ")"))
      {
        scan_labels_in_function(tok);
        // Set the void return flag from what we detected
        current_func_returns_void = next_func_returns_void;
      }
      next_func_returns_void = false;
    }

    // Detect void function definitions at top level
    // This sets next_func_returns_void for when we enter the function body
    if (defer_depth == 0 && is_void_function_decl(tok))
      next_func_returns_void = true;

    // Track struct/union/enum to avoid zero-init inside them
    if (equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum"))
    {
      bool is_enum = equal(tok, "enum");
      // Look ahead to see if this has a body
      // Must handle: struct name {, struct {, struct __attribute__((...)) name {
      Token *t = tok->next;
      // Skip identifiers and __attribute__((...))
      while (t && (t->kind == TK_IDENT || equal(t, "__attribute__")))
      {
        if (equal(t, "__attribute__"))
        {
          t = t->next;
          // Skip (( ... ))
          if (t && equal(t, "("))
          {
            int paren_depth = 1;
            t = t->next;
            while (t && paren_depth > 0)
            {
              if (equal(t, "("))
                paren_depth++;
              else if (equal(t, ")"))
                paren_depth--;
              t = t->next;
            }
          }
        }
        else
        {
          t = t->next;
        }
      }
      if (t && equal(t, "{"))
      {
        // For enums, parse constants to register shadows BEFORE emitting
        // Enum constants are visible at the enclosing scope (defer_depth)
        if (is_enum)
          parse_enum_constants(t, defer_depth);

        // Emit tokens up to and including the {
        while (tok != t)
        {
          emit_tok(tok);
          tok = tok->next;
        }
        emit_tok(tok); // emit the {
        tok = tok->next;
        struct_depth++;
        if (feature_defer)
          defer_push_scope();
        else
        {
          // Still track scope depth for typedef scoping
          defer_stack_ensure_capacity(defer_depth + 1);
          defer_depth++;
        }
        at_stmt_start = true;
        continue;
      }
    }

    // Handle '{' - push scope
    if (equal(tok, "{"))
    {
      pending_control_flow = false; // Proper braces found
      // Check if this is a statement expression: ({ ... })
      // The previous emitted token would be '('
      if (last_emitted && equal(last_emitted, "("))
      {
        // Remember the defer_depth BEFORE we push the new scope
        // This will be the scope level of the statement expression
        // Grow stmt_expr_levels if needed
        if (stmt_expr_count >= stmt_expr_capacity)
        {
          int new_cap = stmt_expr_capacity == 0 ? INITIAL_STMT_EXPR_DEPTH : stmt_expr_capacity * 2;
          int *new_levels = realloc(stmt_expr_levels, sizeof(int) * new_cap);
          if (!new_levels)
            error("out of memory growing statement expression stack");
          stmt_expr_levels = new_levels;
          stmt_expr_capacity = new_cap;
        }
        stmt_expr_levels[stmt_expr_count++] = defer_depth + 1; // +1 because we're about to push
      }
      emit_tok(tok);
      tok = tok->next;
      if (feature_defer)
        defer_push_scope();
      else
      {
        // Still need to track scope depth for typedef scoping even without defer
        defer_stack_ensure_capacity(defer_depth + 1);
        defer_depth++;
      }
      at_stmt_start = true;
      continue;
    }

    // Handle '}' - emit scope defers, then pop
    if (equal(tok, "}"))
    {
      if (struct_depth > 0)
        struct_depth--;
      typedef_pop_scope(defer_depth); // Pop typedefs at current scope (before depth changes)
      if (feature_defer)
      {
        emit_scope_defers();
        defer_pop_scope();
      }
      else
      {
        // Still need to track scope depth for typedef scoping even without defer
        if (defer_depth > 0)
          defer_depth--;
      }
      emit_tok(tok);
      tok = tok->next;
      // Check if we're exiting a statement expression: ... })
      // Match if the next token is ')' and we're at a stmt_expr level
      if (tok && equal(tok, ")") && stmt_expr_count > 0 &&
          stmt_expr_levels[stmt_expr_count - 1] == defer_depth + 1)
      {
        stmt_expr_count--;
      }
      // After closing brace, we're at the start of a new statement
      // (especially important at file scope for tracking typedefs)
      at_stmt_start = true;
      continue;
    }

    // Track parentheses during pending control flow (for distinguishing for(;;) from body)
    if (pending_control_flow)
    {
      if (equal(tok, "("))
      {
        control_paren_depth++;
        // If we just saw 'for' and this is the opening paren, we're entering the init clause
        if (pending_for_paren)
        {
          in_for_init = true;
          at_stmt_start = true; // Init clause is like start of a statement
          pending_for_paren = false;
        }
      }
      else if (equal(tok, ")"))
      {
        control_paren_depth--;
        // Exiting the for() parens entirely clears init state
        if (control_paren_depth == 0)
          in_for_init = false;
      }
      // Semicolon inside for() parens ends the init clause (first ;) and condition (second ;)
      else if (equal(tok, ";") && control_paren_depth == 1)
      {
        if (in_for_init)
        {
          in_for_init = false;
          // After init clause semicolon, we're at start of condition
          // (not a declaration context, so don't set at_stmt_start)
        }
      }
      // Semicolon at depth 0 ends a braceless statement body
      else if (equal(tok, ";") && control_paren_depth == 0)
      {
        pending_control_flow = false;
        next_scope_is_loop = false;
        next_scope_is_switch = false;
        in_for_init = false;
        pending_for_paren = false;
      }
    }

    // Track statement boundaries for zero-init
    if (equal(tok, ";") && !pending_control_flow)
      at_stmt_start = true;

    // Reset void function detection at top-level semicolons
    // This prevents function declarations like "void foo(void);" from affecting
    // subsequent function definitions
    if (equal(tok, ";") && defer_depth == 0)
      next_func_returns_void = false;

    // Handle user-defined labels (label:) - statement after label is at statement start
    // This ensures declarations after labels get zero-initialized
    // Must distinguish from: ternary (?:), bitfield (int x:5), case/default (handled above)
    if (equal(tok, ":") && last_emitted && last_emitted->kind == TK_IDENT &&
        struct_depth == 0 && defer_depth > 0)
    {
      // Check if previous identifier was part of a ternary by looking back further
      // In ternary, there would be a '?' before the identifier
      // We can't easily look back multiple tokens, so we rely on a different check:
      // If we're at a label, the identifier is at statement start position
      // This is an approximation - case/default are already handled above
      emit_tok(tok);
      tok = tok->next;
      at_stmt_start = true;
      continue;
    }

    // Track previous token at top level for function detection
    if (defer_depth == 0)
      prev_toplevel_tok = tok;

    // Default: emit token as-is
    emit_tok(tok);
    tok = tok->next;
  }

  fclose(out);
  return 1;
}

// ═══════════════════════════════════════════════════════════════════════════
// LIBRARY API IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════════════════

#ifndef TMP_DIR
#ifdef _WIN32
#define TMP_DIR ""
#else
#define TMP_DIR "/tmp/"
#endif
#endif

static PrismResult prism_transpile_file(const char *input_file, PrismFeatures features)
{
  PrismResult result = {0};

  // Set global feature flags from PrismFeatures
  feature_defer = features.defer;
  feature_zeroinit = features.zeroinit;
  emit_line_directives = features.line_directives;

  // Create temp file for output
  char temp_path[512];
  snprintf(temp_path, sizeof(temp_path), "%sprism_out.XXXXXX.c", TMP_DIR);

#if defined(_WIN32)
  if (_mktemp_s(temp_path, sizeof(temp_path)) != 0)
  {
    result.status = PRISM_ERR_IO;
    result.error_msg = strdup("Failed to create temp file");
    return result;
  }
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
  {
    int fd = mkstemps(temp_path, 2);
    if (fd < 0)
    {
      result.status = PRISM_ERR_IO;
      result.error_msg = strdup("Failed to create temp file");
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

  // Transpile
  if (!transpile((char *)input_file, temp_path))
  {
    result.status = PRISM_ERR_SYNTAX;
    result.error_msg = strdup("Transpilation failed");
    remove(temp_path);
    return result;
  }

  // Read result into memory
  FILE *f = fopen(temp_path, "rb");
  if (!f)
  {
    result.status = PRISM_ERR_IO;
    result.error_msg = strdup("Failed to read transpiled output");
    remove(temp_path);
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
    return result;
  }

  size_t read = fread(result.output, 1, size, f);
  result.output[read] = '\0';
  result.output_len = read;
  result.status = PRISM_OK;

  fclose(f);
  remove(temp_path);
  return result;
}

static void prism_free(PrismResult *r)
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

// ═══════════════════════════════════════════════════════════════════════════
// CLI IMPLEMENTATION (excluded with -DPRISM_LIB_MODE)
// ═══════════════════════════════════════════════════════════════════════════

#ifndef PRISM_LIB_MODE

// Split a space-separated string into an argv array
// Returns number of arguments, or -1 on error
// Caller must free each element and the array itself
static void free_argv(char **argv)
{
  if (!argv)
    return;
  for (int i = 0; argv[i]; i++)
    free(argv[i]);
  free(argv);
}

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

// Build an argv array from individual components (dynamic growth)
typedef struct
{
  char **data;
  int count;
  int capacity;
} ArgvBuilder;

static void argv_builder_init(ArgvBuilder *ab)
{
  ab->data = NULL;
  ab->count = 0;
  ab->capacity = 0;
}

static bool argv_builder_add(ArgvBuilder *ab, const char *arg)
{
  if (ab->count + 1 >= ab->capacity) // +1 for NULL terminator
  {
    int new_cap = ab->capacity == 0 ? INITIAL_ARGS : ab->capacity * 2;
    char **new_data = realloc(ab->data, sizeof(char *) * new_cap);
    if (!new_data)
      return false;
    ab->data = new_data;
    ab->capacity = new_cap;
  }
  ab->data[ab->count] = strdup(arg);
  if (!ab->data[ab->count])
    return false;
  ab->count++;
  ab->data[ab->count] = NULL; // Keep NULL terminated
  return true;
}

static char **argv_builder_finish(ArgvBuilder *ab)
{
  return ab->data;
}

static char **build_argv(const char *first, ...)
{
  ArgvBuilder ab;
  argv_builder_init(&ab);

  if (first)
  {
    if (!argv_builder_add(&ab, first))
    {
      free_argv(ab.data);
      return NULL;
    }
  }

  // Add remaining arguments
  va_list ap;
  va_start(ap, first);
  const char *arg;
  while ((arg = va_arg(ap, const char *)) != NULL)
  {
    if (!argv_builder_add(&ab, arg))
    {
      va_end(ap);
      free_argv(ab.data);
      return NULL;
    }
  }
  va_end(ap);

  return argv_builder_finish(&ab);
}

// Append arguments from flags string to an ArgvBuilder
// Returns true on success, false on allocation failure
static bool argv_builder_append_flags(ArgvBuilder *ab, const char *flags)
{
  if (!flags || !*flags)
    return true;

  const char *p = flags;
  while (*p)
  {
    while (*p && isspace(*p))
      p++;
    if (!*p)
      break;

    const char *start = p;
    while (*p && !isspace(*p))
      p++;

    size_t len = p - start;
    char *arg = malloc(len + 1);
    if (!arg)
      return false;
    memcpy(arg, start, len);
    arg[len] = '\0';

    // Add to builder (takes ownership)
    if (ab->count + 1 >= ab->capacity)
    {
      int new_cap = ab->capacity == 0 ? INITIAL_ARGS : ab->capacity * 2;
      char **new_data = realloc(ab->data, sizeof(char *) * new_cap);
      if (!new_data)
      {
        free(arg);
        return false;
      }
      ab->data = new_data;
      ab->capacity = new_cap;
    }
    ab->data[ab->count++] = arg;
    ab->data[ab->count] = NULL;
  }
  return true;
}

static void die(char *message)
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

  remove(INSTALL_PATH);

  FILE *input = fopen(self_path, "rb");
  FILE *output = fopen(INSTALL_PATH, "wb");

  if (!input || !output)
  {
    // Fallback to sudo - use run_command for security
    // First try to remove existing file
    char **rm_argv = build_argv("sudo", "rm", "-f", INSTALL_PATH, NULL);
    if (rm_argv)
    {
      run_command(rm_argv);
      free_argv(rm_argv);
    }

    // Copy the file
    char **cp_argv = build_argv("sudo", "cp", self_path, INSTALL_PATH, NULL);
    if (!cp_argv)
    {
      fprintf(stderr, "Memory allocation failed\n");
      return 1;
    }
    int cp_status = run_command(cp_argv);
    free_argv(cp_argv);
    if (cp_status != 0)
    {
      fprintf(stderr, "Failed to copy file\n");
      return 1;
    }

    // Set executable permission
    char **chmod_argv = build_argv("sudo", "chmod", "+x", INSTALL_PATH, NULL);
    if (!chmod_argv)
    {
      fprintf(stderr, "Memory allocation failed\n");
      return 1;
    }
    int chmod_status = run_command(chmod_argv);
    free_argv(chmod_argv);
    if (chmod_status != 0)
    {
      fprintf(stderr, "Failed to set permissions\n");
      return 1;
    }

    printf("[prism] Installed!\n");
    return 0;
  }

  char buffer[4096];
  size_t bytes;
  while ((bytes = fread(buffer, 1, 4096, input)) > 0)
  {
    if (fwrite(buffer, 1, bytes, output) != bytes)
    {
      fclose(input);
      fclose(output);
      return 1;
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

// Legacy Mode enum for get_flags compatibility
typedef enum
{
  MODE_DEFAULT,
  MODE_DEBUG,
  MODE_RELEASE,
  MODE_SMALL
} Mode;

static void get_flags(char *source_file, char *buffer, Mode mode)
{
  buffer[0] = 0;
  size_t bufsize = 2048; // Known from caller
  size_t len = 0;

  // Safe append helper
#define SAFE_APPEND(s)                   \
  do                                     \
  {                                      \
    size_t slen = strlen(s);             \
    if (len + slen < bufsize - 1)        \
    {                                    \
      memcpy(buffer + len, s, slen + 1); \
      len += slen;                       \
    }                                    \
  } while (0)

  if (mode == MODE_DEBUG)
    SAFE_APPEND(" -g -O0");
  if (mode == MODE_RELEASE)
    SAFE_APPEND(" -O3");
  if (mode == MODE_SMALL)
    SAFE_APPEND(" -Os");

  FILE *file = fopen(source_file, "r");
  if (!file)
    return;

  char line[1024];
  while (fgets(line, sizeof(line), file))
  {
    char *ptr = line;
    while (isspace(*ptr))
      ptr++;

    if (strncmp(ptr, "#define PRISM_", 14))
      continue;
    ptr += 14;

    int match = ((!strncmp(ptr, "FLAGS ", 6) || !strncmp(ptr, "LIBS ", 5)) ||
                 (mode == MODE_DEBUG && !strncmp(ptr, "FLAGS_DEBUG ", 12)) ||
                 (mode == MODE_RELEASE && !strncmp(ptr, "FLAGS_RELEASE ", 14)) ||
                 (mode == MODE_SMALL && !strncmp(ptr, "FLAGS_SMALL ", 12)));

    if (!match)
      continue;

    char *quote_start = strchr(ptr, '"');
    char *quote_end = quote_start ? strchr(quote_start + 1, '"') : NULL;
    if (quote_start && quote_end)
    {
      *quote_end = 0;
      size_t add_len = strlen(quote_start + 1);
      // Bounds check: leave room for space and null
      if (len + add_len + 2 < bufsize)
      {
        buffer[len++] = ' ';
        memcpy(buffer + len, quote_start + 1, add_len + 1);
        len += add_len;
      }
    }
  }

#undef SAFE_APPEND
  fclose(file);
}

static char *get_compiler_for_cross(char *arch, int bits, char *platform)
{
  int is_native = !strcmp(arch, NATIVE_ARCH) && bits == NATIVE_BITS &&
                  !strcmp(platform, NATIVE_PLATFORM);
  if (is_native)
    return NULL; // Use default CC

  if (!strcmp(platform, "linux"))
  {
    if (!strcmp(arch, "arm"))
      return bits == 64 ? "aarch64-linux-gnu-gcc" : "arm-linux-gnueabihf-gcc";
    else
      return bits == 64 ? "x86_64-linux-gnu-gcc" : "i686-linux-gnu-gcc";
  }

  if (!strcmp(platform, "windows"))
  {
    if (!strcmp(arch, "arm"))
      return bits == 64 ? "aarch64-w64-mingw32-gcc" : "armv7-w64-mingw32-gcc";
    else
      return bits == 64 ? "x86_64-w64-mingw32-gcc" : "i686-w64-mingw32-gcc";
  }

  if (!strcmp(platform, "macos"))
  {
    return !strcmp(arch, "arm") ? "oa64-clang" : "o64-clang";
  }

  return NULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// CLI Types
// ─────────────────────────────────────────────────────────────────────────────

typedef enum
{
  CLI_MODE_COMPILE_AND_LINK, // default: prism foo.c → a.out (GCC-compatible)
  CLI_MODE_COMPILE_ONLY,     // -c: prism -c foo.c -o foo.o
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

  // Prism-specific
  const char *cc; // --prism-cc (default: $PRISM_CC or $CC or "cc")
  bool verbose;   // --prism-verbose

  // Cross-compilation (legacy support)
  char *cross_arch;
  int cross_bits;
  char *cross_platform;

  // Link-only mode detection
  bool has_objects; // true if .o, .a, or .so files were provided
} Cli;

static void cli_add_source(Cli *cli, const char *src)
{
  if (cli->source_count >= cli->source_capacity)
  {
    int new_cap = cli->source_capacity == 0 ? 16 : cli->source_capacity * 2;
    const char **new_sources = realloc(cli->sources, sizeof(char *) * new_cap);
    if (!new_sources)
      die("Out of memory");
    cli->sources = new_sources;
    cli->source_capacity = new_cap;
  }
  cli->sources[cli->source_count++] = src;
}

static void cli_add_include(Cli *cli, const char *path)
{
  if (cli->include_count >= cli->include_capacity)
  {
    int new_cap = cli->include_capacity == 0 ? 16 : cli->include_capacity * 2;
    const char **new_paths = realloc(cli->include_paths, sizeof(char *) * new_cap);
    if (!new_paths)
      die("Out of memory");
    cli->include_paths = new_paths;
    cli->include_capacity = new_cap;
  }
  cli->include_paths[cli->include_count++] = path;
}

static void cli_add_define(Cli *cli, const char *def)
{
  if (cli->define_count >= cli->define_capacity)
  {
    int new_cap = cli->define_capacity == 0 ? 16 : cli->define_capacity * 2;
    const char **new_defs = realloc(cli->defines, sizeof(char *) * new_cap);
    if (!new_defs)
      die("Out of memory");
    cli->defines = new_defs;
    cli->define_capacity = new_cap;
  }
  cli->defines[cli->define_count++] = def;
}

static void cli_add_cc_arg(Cli *cli, const char *arg)
{
  if (cli->cc_arg_count >= cli->cc_arg_capacity)
  {
    int new_cap = cli->cc_arg_capacity == 0 ? 64 : cli->cc_arg_capacity * 2;
    const char **new_args = realloc(cli->cc_args, sizeof(char *) * new_cap);
    if (!new_args)
      die("Out of memory");
    cli->cc_args = new_args;
    cli->cc_arg_capacity = new_cap;
  }
  cli->cc_args[cli->cc_arg_count++] = arg;
}

static bool is_source_file(const char *arg)
{
  size_t len = strlen(arg);
  if (len < 2)
    return false;
  // .c files
  if (len >= 2 && !strcmp(arg + len - 2, ".c"))
    return true;
  // .i files (preprocessed)
  if (len >= 2 && !strcmp(arg + len - 2, ".i"))
    return true;
  // .s/.S files (assembly)
  if (len >= 2 && (!strcmp(arg + len - 2, ".s") || !strcmp(arg + len - 2, ".S")))
    return true;
  return false;
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
      "Apache 2.0 license (c) Dawn Larsson 2026\n"
      "https://github.com/dawnlarsson/prism\n",
      PRISM_VERSION, INSTALL_PATH);
}

static Cli cli_parse(int argc, char **argv)
{
  Cli cli = {
      .mode = CLI_MODE_COMPILE_AND_LINK,
      .features = prism_defaults(),
      .cross_arch = NATIVE_ARCH,
      .cross_bits = NATIVE_BITS,
      .cross_platform = NATIVE_PLATFORM,
  };

  // Get compiler from environment
  char *env_cc = getenv("PRISM_CC");
  if (!env_cc)
    env_cc = getenv("CC");
  cli.cc = env_cc ? env_cc : "cc";

  for (int i = 1; i < argc; i++)
  {
    char *arg = argv[i];

    // ─── Prism commands ───
    if (!strcmp(arg, "run"))
    {
      cli.mode = CLI_MODE_RUN;
      continue;
    }
    if (!strcmp(arg, "transpile"))
    {
      cli.mode = CLI_MODE_EMIT;
      continue;
    }
    if (!strcmp(arg, "install"))
    {
      cli.mode = CLI_MODE_INSTALL;
      continue;
    }
    if (!strcmp(arg, "--help") || !strcmp(arg, "-h"))
    {
      cli.mode = CLI_MODE_HELP;
      continue;
    }
    if (!strcmp(arg, "--version") || !strcmp(arg, "-v"))
    {
      cli.mode = CLI_MODE_VERSION;
      continue;
    }

    // ─── Prism feature flags (gcc-style) ───
    if (!strcmp(arg, "-fno-defer"))
    {
      cli.features.defer = false;
      continue;
    }
    if (!strcmp(arg, "-fno-zeroinit"))
    {
      cli.features.zeroinit = false;
      continue;
    }
    if (!strcmp(arg, "-fno-line-directives"))
    {
      cli.features.line_directives = false;
      continue;
    }

    // ─── Legacy prism flags (still supported) ───
    if (!strcmp(arg, "no-defer"))
    {
      cli.features.defer = false;
      continue;
    }
    if (!strcmp(arg, "no-zeroinit"))
    {
      cli.features.zeroinit = false;
      continue;
    }

    // ─── Legacy cross-compilation (still supported) ───
    if (!strcmp(arg, "arm"))
    {
      cli.cross_arch = "arm";
      continue;
    }
    if (!strcmp(arg, "x86"))
    {
      cli.cross_arch = "x86";
      continue;
    }
    if (!strcmp(arg, "32"))
    {
      cli.cross_bits = 32;
      continue;
    }
    if (!strcmp(arg, "64"))
    {
      cli.cross_bits = 64;
      continue;
    }
    if (!strcmp(arg, "linux"))
    {
      cli.cross_platform = "linux";
      continue;
    }
    if (!strcmp(arg, "windows"))
    {
      cli.cross_platform = "windows";
      continue;
    }
    if (!strcmp(arg, "macos"))
    {
      cli.cross_platform = "macos";
      continue;
    }

    // ─── Legacy modes (backward compatibility) ───
    if (!strcmp(arg, "build"))
    {
      // 'build' is now the default behavior, ignore for compat
      continue;
    }

    // ─── Prism-specific options ───
    if (!strcmp(arg, "--prism-emit"))
    {
      cli.mode = CLI_MODE_EMIT;
      continue;
    }
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

    // ─── GCC-compatible: compile only ───
    if (!strcmp(arg, "-c"))
    {
      cli.mode = CLI_MODE_COMPILE_ONLY;
      continue;
    }

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

    // -D (define)
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

    // -U (undefine) - pass to CC, prism doesn't have pp_undef yet
    if (!strcmp(arg, "-U") || str_startswith(arg, "-U"))
    {
      cli_add_cc_arg(&cli, arg);
      if (!strcmp(arg, "-U") && i + 1 < argc)
        cli_add_cc_arg(&cli, argv[++i]);
      continue;
    }

    // -include (force include) - pass to CC
    if (!strcmp(arg, "-include"))
    {
      cli_add_cc_arg(&cli, arg);
      if (i + 1 < argc)
        cli_add_cc_arg(&cli, argv[++i]);
      continue;
    }

    // -isystem (system include path) - treat like -I for prism
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
      // Could be .o file, .a file, .so file, etc. - pass to linker
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
    cli_add_cc_arg(&cli, arg);

    // Handle space-separated args like -L dir
    if (flag_needs_arg(arg) && i + 1 < argc && argv[i + 1][0] != '-')
    {
      cli_add_cc_arg(&cli, argv[++i]);
    }
  }

  return cli;
}

static void cli_free(Cli *cli)
{
  free(cli->sources);
  free(cli->cc_args);
  free(cli->include_paths);
  free(cli->defines);
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
    char source_dir[512];
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
      return NULL;
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
  if (argc < 2)
  {
    print_help();
    return 0;
  }

  Cli cli = cli_parse(argc, argv);

  // Set feature flags from CLI
  feature_defer = cli.features.defer;
  feature_zeroinit = cli.features.zeroinit;

  // Set preprocessor configuration from CLI
  extra_include_paths = cli.include_paths;
  extra_include_count = cli.include_count;
  extra_defines = cli.defines;
  extra_define_count = cli.define_count;
  emit_line_directives = cli.features.line_directives;

  // Handle special modes
  switch (cli.mode)
  {
  case CLI_MODE_HELP:
    print_help();
    cli_free(&cli);
    return 0;

  case CLI_MODE_VERSION:
    printf("prism %s\n", PRISM_VERSION);
    cli_free(&cli);
    return 0;

  case CLI_MODE_INSTALL:
    // If source files provided, compile them first to produce the binary to install
    if (cli.source_count > 0)
    {
      // Compile sources to a temp binary, then install that
      char temp_bin[PATH_MAX];
      snprintf(temp_bin, sizeof(temp_bin), "%sprism_install_%d", TMP_DIR, getpid());

      // Build compile command
      const char *cc = cli.cc ? cli.cc : getenv("PRISM_CC");
      if (!cc)
        cc = getenv("CC");
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
        snprintf(temp_files[i], PATH_MAX, "%sprism_install_%d_%d.c", TMP_DIR, getpid(), i);

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
        // Transpile to stdout
        char temp[512];
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
      }
    }
    cli_free(&cli);
    return 0;
  }

  case CLI_MODE_COMPILE_ONLY:
  case CLI_MODE_COMPILE_AND_LINK:
  case CLI_MODE_RUN:
    break; // Continue below
  }

  // Link-only mode: if no sources but has object files, pass through to compiler
  if (cli.source_count == 0 && cli.has_objects && cli.mode != CLI_MODE_RUN)
  {
    const char *compiler = cli.cc;
    char *cross_cc = get_compiler_for_cross(cli.cross_arch, cli.cross_bits, cli.cross_platform);
    if (cross_cc)
      compiler = cross_cc;

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

  // Determine compiler
  const char *compiler = cli.cc;
  char *cross_cc = get_compiler_for_cross(cli.cross_arch, cli.cross_bits, cli.cross_platform);
  if (cross_cc)
    compiler = cross_cc;

  // Transpile all sources to temp files
  char **temp_files = calloc(cli.source_count, sizeof(char *));
  if (!temp_files)
    die("Out of memory");

  for (int i = 0; i < cli.source_count; i++)
  {
    temp_files[i] = malloc(512);
    if (!temp_files[i])
      die("Out of memory");

    if (!create_temp_file(cli.sources[i], temp_files[i], 512))
    {
      for (int j = 0; j < i; j++)
      {
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
        remove(temp_files[j]);
        free(temp_files[j]);
      }
      free(temp_files);
      die("Transpilation failed");
    }
  }

  // For RUN mode, compile to temp executable
  char temp_exe[512] = {0};
  if (cli.mode == CLI_MODE_RUN)
  {
    snprintf(temp_exe, sizeof(temp_exe), "%sprism_run.XXXXXX", TMP_DIR);
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
    static char default_obj[512];
    const char *src = cli.sources[0];
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    snprintf(default_obj, sizeof(default_obj), "%s", base);
    char *dot = strrchr(default_obj, '.');
    if (dot)
      strcpy(dot, ".o");
    else
      strcat(default_obj, ".o");
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

  // Cleanup temp source files
  for (int i = 0; i < cli.source_count; i++)
  {
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
#define PRISM_FLAGS "-O3 -flto -s"
#define VERSION "0.22.0"

// Include the tokenizer/preprocessor
#include "parse.c"
// Platform detection
#ifdef _WIN32
#define INSTALL "prism.exe"
#define TMP ""
#else
#define INSTALL "/usr/local/bin/prism"
#define TMP "/tmp/"
#endif

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

typedef enum
{
  MODE_DEFAULT,
  MODE_DEBUG,
  MODE_RELEASE,
  MODE_SMALL
} Mode;

// Feature flags (all enabled by default)
static bool feature_defer = true;
static bool feature_zeroinit = true;

// Track struct/union/enum depth (don't zero-init inside these)
static int struct_depth = 0;

// Defer tracking
#define MAX_DEFER_DEPTH 64
#define MAX_DEFERS_PER_SCOPE 32
#define MAX_LABELS 256

typedef struct
{
  Token *stmts[MAX_DEFERS_PER_SCOPE];     // Start token of each deferred statement
  Token *ends[MAX_DEFERS_PER_SCOPE];      // End token (the semicolon)
  Token *defer_tok[MAX_DEFERS_PER_SCOPE]; // The 'defer' keyword token (for error messages)
  int count;
  bool is_loop;   // true if this scope is a for/while/do loop
  bool is_switch; // true if this scope is a switch statement
} DeferScope;

// Label tracking for goto handling
typedef struct
{
  char *name;
  int name_len;
  int scope_depth; // Defer scope depth where label resides
} LabelInfo;

typedef struct
{
  LabelInfo labels[MAX_LABELS];
  int count;
} LabelTable;

static DeferScope defer_stack[MAX_DEFER_DEPTH];
static int defer_depth = 0;

// Track pending loop/switch for next scope
static bool next_scope_is_loop = false;
static bool next_scope_is_switch = false;
static bool pending_control_flow = false; // True after if/else/for/while/do/switch until we see { or ;
static int control_paren_depth = 0;       // Track parens to distinguish for(;;) from braceless body

// Label table for current function (for goto handling)
static LabelTable label_table;

static void defer_push_scope(void)
{
  if (defer_depth >= MAX_DEFER_DEPTH)
    error("defer: scope nesting too deep (max %d)", MAX_DEFER_DEPTH);
  defer_stack[defer_depth].count = 0;
  defer_stack[defer_depth].is_loop = next_scope_is_loop;
  defer_stack[defer_depth].is_switch = next_scope_is_switch;
  next_scope_is_loop = false;
  next_scope_is_switch = false;
  defer_depth++;
}

static void defer_pop_scope(void)
{
  if (defer_depth > 0)
    defer_depth--;
}

static void defer_add(Token *defer_keyword, Token *start, Token *end)
{
  if (defer_depth <= 0)
    error_tok(start, "defer outside of any scope");
  DeferScope *scope = &defer_stack[defer_depth - 1];
  if (scope->count >= MAX_DEFERS_PER_SCOPE)
    error_tok(start, "too many defers in scope (max %d)", MAX_DEFERS_PER_SCOPE);
  scope->defer_tok[scope->count] = defer_keyword;
  scope->stmts[scope->count] = start;
  scope->ends[scope->count] = end;
  scope->count++;
}

// Clear defers at innermost switch scope when hitting case/default
// This prevents defers from one case leaking to another case
static void clear_switch_scope_defers(void)
{
  for (int d = defer_depth - 1; d >= 0; d--)
  {
    if (defer_stack[d].is_switch)
    {
      defer_stack[d].count = 0;
      return;
    }
  }
}
// Token emission
static FILE *out;
static Token *last_emitted = NULL;

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
        (prev_last == '/' && tok_first == '*'))
      return true;
  }

  return false;
}

// Emit a single token with appropriate spacing
static void emit_tok(Token *tok)
{
  if (tok->at_bol)
    fputc('\n', out);
  else if (needs_space(last_emitted, tok))
    fputc(' ', out);
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

// Check if break needs to emit defers
static bool break_has_defers(void)
{
  for (int d = defer_depth - 1; d >= 0; d--)
  {
    if (defer_stack[d].count > 0)
      return true;
    if (defer_stack[d].is_loop || defer_stack[d].is_switch)
      break;
  }
  return false;
}

// Check if continue needs to emit defers
static bool continue_has_defers(void)
{
  for (int d = defer_depth - 1; d >= 0; d--)
  {
    if (defer_stack[d].count > 0)
      return true;
    if (defer_stack[d].is_loop)
      break;
  }
  return false;
}
// Label tracking for goto
static void label_table_reset(void)
{
  label_table.count = 0;
}

static void label_table_add(char *name, int name_len, int scope_depth)
{
  if (label_table.count >= MAX_LABELS)
  {
    fprintf(stderr, "warning: too many labels in function (max %d), goto/defer tracking may be inaccurate\n", MAX_LABELS);
    return;
  }
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

// Check if token could be a label (identifier followed by ':')
// Note: This can have false positives from ternary operator
// The caller should filter using context (prev token shouldn't be '?')
static bool is_label(Token *tok)
{
  if (tok->kind != TK_IDENT)
    return false;
  Token *next = tok->next;
  if (!next || !equal(next, ":"))
    return false;
  return true;
}

// Scan a function body for labels and record their scope depths
// tok should point to the opening '{' of the function body
static void scan_labels_in_function(Token *tok)
{
  label_table_reset();
  if (!tok || !equal(tok, "{"))
    return;

  // Start at depth 1 to align with defer_depth (which is 1 inside function body)
  int depth = 1;
  int struct_depth = 0; // Track nesting inside struct/union/enum bodies
  Token *prev = NULL;
  tok = tok->next; // Skip opening brace

  while (tok && tok->kind != TK_EOF)
  {
    // Track struct/union/enum bodies to skip bitfield declarations
    if (equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum"))
    {
      // Look ahead for '{' - could be "struct {" or "struct name {"
      Token *t = tok->next;
      while (t && t->kind == TK_IDENT)
        t = t->next;
      if (t && equal(t, "{"))
      {
        // Skip to the opening brace
        while (tok != t)
        {
          prev = tok;
          tok = tok->next;
        }
        struct_depth++;
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
      if (struct_depth > 0)
        struct_depth--;
      depth--;
      prev = tok;
      tok = tok->next;
      continue;
    }

    // Check for label: identifier followed by ':' (but not ::)
    // Filter out: ternary operator, switch cases, bitfields
    if (is_label(tok))
    {
      Token *colon = tok->next;
      // Make sure it's not :: (C++ scope resolution)
      bool is_scope_resolution = colon->next && equal(colon->next, ":");
      // Ternary operator: prev token is '?'
      bool is_ternary = prev && equal(prev, "?");
      // Switch case: prev token is 'case' or 'default'
      bool is_switch_case = prev && (equal(prev, "case") || equal(prev, "default"));
      // Bitfield: we're inside a struct/union body
      bool is_bitfield = struct_depth > 0;

      if (!is_scope_resolution && !is_ternary && !is_switch_case && !is_bitfield)
      {
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
  Token *active_defer = NULL;  // Most recently seen defer that's still "in scope"
  int active_defer_depth = -1; // Depth at which active_defer was found

  while (tok && tok->kind != TK_EOF)
  {
    if (equal(tok, "{"))
    {
      depth++;
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
      if (depth == 0)
        break; // End of containing scope, label not found here
      depth--;
      tok = tok->next;
      continue;
    }

    // Track defers we pass over
    if (tok->kind == TK_KEYWORD && equal(tok, "defer"))
    {
      // Remember this defer (prefer shallowest depth if multiple)
      if (!active_defer || depth <= active_defer_depth)
      {
        active_defer = tok;
        active_defer_depth = depth;
      }
    }

    // Found the label?
    if (is_label(tok) && tok->len == label_len &&
        !memcmp(tok->loc, label_name, label_len))
    {
      // If we have an active defer, we're jumping past it into its scope = error
      return active_defer;
    }

    tok = tok->next;
  }

  return NULL; // Label not found in forward scan, or all defers were in skipped blocks
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
  return false;
}

static bool is_type_qualifier(Token *tok)
{
  if (tok->kind != TK_KEYWORD)
    return false;
  return equal(tok, "const") || equal(tok, "volatile") || equal(tok, "restrict") ||
         equal(tok, "static") || equal(tok, "auto") || equal(tok, "register") ||
         equal(tok, "_Atomic");
}

static bool is_skip_decl_keyword(Token *tok)
{
  return equal(tok, "extern") || equal(tok, "typedef") || equal(tok, "static");
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
      // Allow numeric literals, sizeof, and basic operators
      if (tok->kind != TK_NUM && !equal(tok, "sizeof") &&
          !equal(tok, "+") && !equal(tok, "-") && !equal(tok, "*") &&
          !equal(tok, "/") && !equal(tok, "(") && !equal(tok, ")"))
      {
        // If it's an identifier, it might be a VLA
        if (tok->kind == TK_IDENT)
          has_only_literals = false;
      }
    }
    tok = tok->next;
  }
  return is_empty || has_only_literals;
}

// Try to handle a declaration with zero-init
// Returns the token after the declaration if handled, NULL otherwise
static Token *try_zero_init_decl(Token *tok)
{
  if (!feature_zeroinit || defer_depth <= 0 || struct_depth > 0)
    return NULL;

  Token *start = tok;

  // Skip extern/typedef - don't init these
  if (is_skip_decl_keyword(tok))
    return NULL;

  // Must start with qualifier or type
  bool saw_type = false;
  bool is_struct_type = false;
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
      continue;
    }
    tok = tok->next;
  }

  if (!saw_type)
    return NULL;

  // Skip pointers - if it's a pointer, it's not a struct anymore
  bool is_pointer = false;
  while (equal(tok, "*") || is_type_qualifier(tok))
  {
    if (equal(tok, "*"))
      is_pointer = true;
    tok = tok->next;
  }

  // Must have identifier
  if (tok->kind != TK_IDENT)
    return NULL;

  Token *var_name = tok;
  tok = tok->next;

  // Check what follows the identifier
  bool is_array = false;
  bool is_vla = false;

  // Function declaration? Skip
  if (equal(tok, "("))
    return NULL;

  // Array?
  if (equal(tok, "["))
  {
    is_array = true;
    if (!is_const_array_size(tok))
      is_vla = true;
    // Skip all array dimensions
    while (equal(tok, "["))
      tok = skip_balanced(tok, "[", "]");
  }

  // Multiple declarators? Too complex for now
  if (equal(tok, ","))
    return NULL;

  // Already has initializer?
  if (equal(tok, "="))
    return NULL;

  // Must end with semicolon
  if (!equal(tok, ";"))
    return NULL;

  // VLAs can't be initialized
  if (is_vla)
    return NULL;

  // Emit the declaration with zero initializer
  emit_range(start, tok); // everything up to semicolon
  if (is_array || (is_struct_type && !is_pointer))
    fprintf(out, " = {0}");
  else
    fprintf(out, " = 0");
  emit_tok(tok); // semicolon

  return tok->next;
}

static int transpile(char *input_file, char *output_file)
{
  // Initialize preprocessor
  pp_init();
  pp_add_default_include_paths();

  // Add prism-specific predefined macro
  pp_define_macro("__PRISM__", "1");
  if (feature_defer)
    pp_define_macro("__PRISM_DEFER__", "1");
  if (feature_zeroinit)
    pp_define_macro("__PRISM_ZEROINIT__", "1");

  // Tokenize and preprocess
  Token *tok = tokenize_file(input_file);
  if (!tok)
  {
    fprintf(stderr, "Failed to open: %s\n", input_file);
    return 0;
  }

  tok = preprocess(tok);

  // Open output
  out = fopen(output_file, "w");
  if (!out)
    return 0;

  // Reset state
  defer_depth = 0;
  struct_depth = 0;
  last_emitted = NULL;
  next_scope_is_loop = false;
  next_scope_is_switch = false;
  pending_control_flow = false;
  control_paren_depth = 0;
  bool at_stmt_start = false; // Track if we're at start of a statement

  // Walk tokens and emit
  while (tok->kind != TK_EOF)
  {
    // Try zero-init for declarations at statement start
    if (at_stmt_start && !pending_control_flow)
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

    // Handle 'defer' keyword
    if (feature_defer && tok->kind == TK_KEYWORD && equal(tok, "defer"))
    {
      // Check for defer inside for/while/switch/if parentheses - this is invalid
      if (pending_control_flow && control_paren_depth > 0)
        error_tok(tok, "defer cannot appear inside control statement parentheses");

      // Check for braceless control flow - defer needs a proper scope
      if (pending_control_flow)
        error_tok(tok, "defer cannot be the body of a braceless control statement - add braces");

      Token *defer_keyword = tok;
      tok = tok->next; // skip 'defer'

      // Find the statement (up to semicolon)
      Token *stmt_start = tok;
      Token *stmt_end = skip_to_semicolon(tok);

      // Record the defer
      defer_add(defer_keyword, stmt_start, stmt_end);

      // Skip past the semicolon (don't emit the defer yet)
      if (stmt_end->kind != TK_EOF)
        tok = stmt_end->next;
      else
        tok = stmt_end;
      continue;
    }

    // Handle 'return' - evaluate expr, run defers, then return
    if (feature_defer && tok->kind == TK_KEYWORD && equal(tok, "return"))
    {
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
          // return with expression: { __auto_type _ret = (expr); defers; return _ret; }
          static int ret_counter = 0;
          int my_ret = ret_counter++;
          fprintf(out, " { __auto_type _prism_ret_%d = (", my_ret);

          // Emit expression until semicolon
          while (tok->kind != TK_EOF && !equal(tok, ";"))
          {
            emit_tok(tok);
            tok = tok->next;
          }

          fprintf(out, ");");
          emit_all_defers();
          fprintf(out, " return _prism_ret_%d;", my_ret);

          if (equal(tok, ";"))
            tok = tok->next;

          fprintf(out, " }");
        }
        continue;
      }
      // No defers, emit normally
    }

    // Handle 'break' - emit defers up through loop/switch
    if (feature_defer && tok->kind == TK_KEYWORD && equal(tok, "break"))
    {
      if (break_has_defers())
      {
        fprintf(out, " {");
        emit_break_defers();
        fprintf(out, " break; }");
        tok = tok->next;
        // Skip the semicolon
        if (equal(tok, ";"))
          tok = tok->next;
        continue;
      }
      // No defers, emit normally
    }

    // Handle 'continue' - emit defers up to (not including) loop
    if (feature_defer && tok->kind == TK_KEYWORD && equal(tok, "continue"))
    {
      if (continue_has_defers())
      {
        fprintf(out, " {");
        emit_continue_defers();
        fprintf(out, " continue; }");
        tok = tok->next;
        // Skip the semicolon
        if (equal(tok, ";"))
          tok = tok->next;
        continue;
      }
      // No defers, emit normally
    }

    // Handle 'goto' - emit defers for scopes being exited
    if (feature_defer && tok->kind == TK_KEYWORD && equal(tok, "goto"))
    {
      Token *goto_tok = tok;
      tok = tok->next; // skip 'goto'

      // Get the label name
      if (tok->kind == TK_IDENT)
      {
        // Check if this goto would skip over a defer statement
        Token *skipped = goto_skips_defer(goto_tok, tok->loc, tok->len);
        if (skipped)
          error_tok(skipped, "goto '%.*s' would skip over this defer statement",
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
          continue;
        }
      }
      // No defers or couldn't parse, emit normally
      emit_tok(goto_tok);
      continue;
    }

    // Mark loop keywords so next '{' knows it's a loop scope
    if (feature_defer && tok->kind == TK_KEYWORD &&
        (equal(tok, "for") || equal(tok, "while") || equal(tok, "do")))
    {
      next_scope_is_loop = true;
      pending_control_flow = true;
    }

    // Mark switch keyword
    if (feature_defer && tok->kind == TK_KEYWORD && equal(tok, "switch"))
    {
      next_scope_is_switch = true;
      pending_control_flow = true;
    }

    // Mark if/else keywords
    if (tok->kind == TK_KEYWORD && (equal(tok, "if") || equal(tok, "else")))
    {
      pending_control_flow = true;
    }

    // Handle case/default labels - clear defers from switch scope
    // This prevents defers registered in one case from leaking to other cases
    if (feature_defer && tok->kind == TK_KEYWORD && (equal(tok, "case") || equal(tok, "default")))
    {
      clear_switch_scope_defers();
    }

    // Detect function definition and scan for labels
    // Pattern: identifier '(' ... ')' '{'
    // We detect this when we see '{' after ')' at depth 0
    if (feature_defer && equal(tok, "{") && defer_depth == 0)
    {
      // At top level, this is likely a function body - scan for labels
      scan_labels_in_function(tok);
    }

    // Track struct/union/enum to avoid zero-init inside them
    if (equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum"))
    {
      // Look ahead to see if this has a body
      Token *t = tok->next;
      if (t && t->kind == TK_IDENT)
        t = t->next;
      if (t && equal(t, "{"))
      {
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
        at_stmt_start = true;
        continue;
      }
    }

    // Handle '{' - push scope
    if (equal(tok, "{"))
    {
      pending_control_flow = false; // Proper braces found
      emit_tok(tok);
      tok = tok->next;
      if (feature_defer)
        defer_push_scope();
      at_stmt_start = true;
      continue;
    }

    // Handle '}' - emit scope defers, then pop
    if (equal(tok, "}"))
    {
      if (struct_depth > 0)
        struct_depth--;
      if (feature_defer)
      {
        emit_scope_defers();
        defer_pop_scope();
      }
      emit_tok(tok);
      tok = tok->next;
      continue;
    }

    // Track parentheses during pending control flow (for distinguishing for(;;) from body)
    if (pending_control_flow)
    {
      if (equal(tok, "("))
        control_paren_depth++;
      else if (equal(tok, ")"))
        control_paren_depth--;
      // Semicolon at depth 0 ends a braceless statement body
      else if (equal(tok, ";") && control_paren_depth == 0)
      {
        pending_control_flow = false;
        next_scope_is_loop = false;
        next_scope_is_switch = false;
      }
    }

    // Track statement boundaries for zero-init
    if (equal(tok, ";") && !pending_control_flow)
      at_stmt_start = true;

    // Default: emit token as-is
    emit_tok(tok);
    tok = tok->next;
  }

  fclose(out);
  return 1;
}
// Build system
static void die(char *message)
{
  fprintf(stderr, "%s\n", message);
  exit(1);
}

static int install(char *self_path)
{
  printf("[prism] Installing to %s...\n", INSTALL);
  remove(INSTALL);

  FILE *input = fopen(self_path, "rb");
  FILE *output = fopen(INSTALL, "wb");

  if (!input || !output)
  {
    char command[512];
    snprintf(command, 512,
             "sudo rm -f \"%s\" && sudo cp \"%s\" \"%s\" && sudo chmod +x \"%s\"",
             INSTALL, self_path, INSTALL, INSTALL);
    return system(command) == 0 ? 0 : 1;
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
  chmod(INSTALL, 0755);
#endif

  printf("[prism] Installed!\n");
  return 0;
}

static void get_flags(char *source_file, char *buffer, Mode mode)
{
  buffer[0] = 0;

  if (mode == MODE_DEBUG)
    strcat(buffer, " -g -O0");
  if (mode == MODE_RELEASE)
    strcat(buffer, " -O3");
  if (mode == MODE_SMALL)
    strcat(buffer, " -Os");

  FILE *file = fopen(source_file, "r");
  if (!file)
    return;

  char line[1024];
  while (fgets(line, 1024, file))
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
      strcat(buffer, " ");
      strcat(buffer, quote_start + 1);
    }
  }

  fclose(file);
}

static char *get_compiler(char *arch, int bits, char *platform)
{
  int is_native = !strcmp(arch, NATIVE_ARCH) && bits == NATIVE_BITS &&
                  !strcmp(platform, NATIVE_PLATFORM);
  if (is_native)
    return "cc";

  if (!strcmp(platform, "linux"))
  {
    if (!strcmp(arch, "arm"))
      return bits == 64 ? "aarch64-linux-gnu-gcc" : "arm-linux-gnueabihf-gcc";
    else
      return bits == 64 ? "x86_64-linux-gnu-gcc" : "gcc -m32";
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

  return "cc";
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    printf("Prism v%s\n"
           "Usage: prism [options] src.c [output] [args]\n\n"
           "Options:\n"
           "  build          Build only, don't run\n"
           "  transpile      Transpile only, output to stdout or file\n"
           "  debug/release/small  Optimization mode\n"
           "  arm/x86        Architecture (default: native)\n"
           "  32/64          Word size (default: 64)\n"
           "  linux/windows/macos  Platform (default: native)\n"
           "  no-defer       Disable defer feature\n"
           "  no-zeroinit    Disable zero-initialization\n\n"
           "Examples:\n"
           "  prism src.c              Run src.c\n"
           "  prism build src.c        Build src\n"
           "  prism build src.c out    Build to 'out'\n"
           "  prism build arm src.c    Build for arm64 linux\n"
           "  prism transpile src.c    Transpile to stdout\n"
           "  prism transpile src.c out.c  Transpile to out.c\n"
           "  prism no-defer src.c     Run without defer\n\n"
           "Prism extensions:\n"
           "  defer stmt;    Execute stmt when scope exits\n"
           "  Zero-init      Local vars auto-initialized to 0\n\n"
           "install\n",
           VERSION);
    return 0;
  }

  if (!strcmp(argv[1], "install"))
    return install(argv[0]);

  int arg_idx = 1, is_build_only = 0, is_transpile_only = 0;
  Mode mode = MODE_DEFAULT;
  char *arch = NATIVE_ARCH;
  int bits = NATIVE_BITS;
  char *platform = NATIVE_PLATFORM;

// Helper to check if filename ends with .c
#define ends_with_c(s) ({                                    \
  int _len = strlen(s);                                      \
  _len >= 2 && (s)[_len - 2] == '.' && (s)[_len - 1] == 'c'; \
})

  // Parse options until we hit a .c file
  while (arg_idx < argc && !ends_with_c(argv[arg_idx]))
  {
    char *arg = argv[arg_idx];
    if (!strcmp(arg, "build"))
      is_build_only = 1;
    else if (!strcmp(arg, "transpile"))
      is_transpile_only = 1;
    else if (!strcmp(arg, "debug"))
      mode = MODE_DEBUG;
    else if (!strcmp(arg, "release"))
      mode = MODE_RELEASE;
    else if (!strcmp(arg, "small"))
      mode = MODE_SMALL;
    else if (!strcmp(arg, "arm"))
      arch = "arm";
    else if (!strcmp(arg, "x86"))
      arch = "x86";
    else if (!strcmp(arg, "32"))
      bits = 32;
    else if (!strcmp(arg, "64"))
      bits = 64;
    else if (!strcmp(arg, "linux"))
      platform = "linux";
    else if (!strcmp(arg, "windows"))
      platform = "windows";
    else if (!strcmp(arg, "macos"))
      platform = "macos";
    else if (!strcmp(arg, "no-defer"))
      feature_defer = false;
    else if (!strcmp(arg, "no-zeroinit"))
      feature_zeroinit = false;
    else
      break;
    arg_idx++;
  }

  if (arg_idx >= argc)
    die("Missing source file.");

  char *source = argv[arg_idx];
  char flags[2048], output_path[512], command[4096], transpiled[512];

  // Generate transpiled filename
  char *basename = strrchr(source, '/');
#ifdef _WIN32
  char *win_basename = strrchr(source, '\\');
  if (!basename || (win_basename && win_basename > basename))
    basename = win_basename;
#endif

  if (!basename)
  {
    snprintf(transpiled, sizeof(transpiled), ".%s.%d.prism.c", source, getpid());
  }
  else
  {
    char source_dir[512];
    size_t dir_len = basename - source;
    strncpy(source_dir, source, dir_len);
    source_dir[dir_len] = 0;
    basename++;
    snprintf(transpiled, sizeof(transpiled), "%s/.%s.%d.prism.c",
             source_dir, basename, getpid());
  }

  // Handle transpile-only mode
  if (is_transpile_only)
  {
    char *transpile_output = NULL;
    if (arg_idx + 1 < argc && argv[arg_idx + 1][0] != '-')
      transpile_output = argv[arg_idx + 1];

    if (transpile_output)
    {
      printf("[prism] Transpiling %s -> %s\n", source, transpile_output);
      if (!transpile(source, transpile_output))
        die("Transpilation failed.");
    }
    else
    {
      // Transpile to temp file then output to stdout
      if (!transpile(source, transpiled))
        die("Transpilation failed.");
      FILE *f = fopen(transpiled, "r");
      if (!f)
        die("Failed to read transpiled output.");
      int c;
      while ((c = fgetc(f)) != EOF)
        putchar(c);
      fclose(f);
      remove(transpiled);
    }
    return 0;
  }

  // Transpile
  printf("[prism] Transpiling %s...\n", source);
  if (!transpile(source, transpiled))
    die("Transpilation failed.");

  // Get compiler flags
  get_flags(source, flags, mode);
  char *compiler = get_compiler(arch, bits, platform);
  int is_windows = !strcmp(platform, "windows");
  char *custom_output = NULL;

  if (is_build_only && arg_idx + 1 < argc && argv[arg_idx + 1][0] != '-')
  {
    custom_output = argv[arg_idx + 1];
    arg_idx++;
  }

  if (is_build_only)
  {
    if (custom_output)
    {
      strncpy(output_path, custom_output, 511);
    }
    else
    {
      strncpy(output_path, source, 511);
      char *ext = strrchr(output_path, '.');
      if (ext)
        *ext = 0;
      if (is_windows)
        strcat(output_path, ".exe");
    }
    printf("[prism] Building %s (%s %d-bit %s)...\n", output_path, arch, bits, platform);
  }
  else
  {
    snprintf(output_path, sizeof(output_path), "%sprism_out.%d", TMP, getpid());
  }

  snprintf(command, 4096, "%s \"%s\" -o \"%s\"%s", compiler, transpiled, output_path, flags);

  if (system(command))
  {
    remove(transpiled);
    die("Compilation failed.");
  }

  if (!is_build_only)
  {
    snprintf(command, 4096, "\"%s\"", output_path);
    for (int j = arg_idx + 1; j < argc; j++)
    {
      size_t len = strlen(command);
      if (len + strlen(argv[j]) + 4 < 4096)
        snprintf(command + len, 4096 - len, " \"%s\"", argv[j]);
    }
    int status = system(command);
    remove(output_path);
    remove(transpiled);
    exit(status > 255 ? status >> 8 : status);
  }

  remove(transpiled);
  return 0;
}
#define PRISM_FLAGS "-O3 -flto -s"
#define VERSION "0.29.0"

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
  bool is_loop;          // true if this scope is a for/while/do loop
  bool is_switch;        // true if this scope is a switch statement
  bool had_control_exit; // true if break/return/goto/continue seen since last defer in switch
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

// Temp file path for cleanup on error exit
static char *pending_temp_file = NULL;

static void cleanup_temp_file(void)
{
  if (pending_temp_file && pending_temp_file[0])
  {
    remove(pending_temp_file);
    pending_temp_file = NULL;
  }
}

// Track pending loop/switch for next scope
static bool next_scope_is_loop = false;
static bool next_scope_is_switch = false;
static bool pending_control_flow = false; // True after if/else/for/while/do/switch until we see { or ;
static int control_paren_depth = 0;       // Track parens to distinguish for(;;) from braceless body

// Label table for current function (for goto handling)
static LabelTable label_table;

// Track if current function returns void (for return statement handling)
static bool current_func_returns_void = false;

// Track statement boundaries for zero-init (moved to static for helper access)
static bool at_stmt_start = false;

// Helper: call this when a handler consumes a semicolon and continues
// This ensures state is consistent as if the main loop saw the ';'
static void end_statement_after_semicolon(void)
{
  at_stmt_start = true;
  if (pending_control_flow && control_paren_depth == 0)
  {
    pending_control_flow = false;
    next_scope_is_loop = false;
    next_scope_is_switch = false;
  }
}

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
  // Reset control exit flag - new defer means we need a new control exit
  scope->had_control_exit = false;
}

// Track if we're directly inside a switch (no braces after case)
static bool in_switch_case_body = false;

// Mark that control flow exited (break/return/goto) in the innermost switch scope
// This tells us that defers were properly executed before the case ended
static void mark_switch_control_exit(void)
{
  for (int d = defer_depth - 1; d >= 0; d--)
  {
    if (defer_stack[d].is_switch)
    {
      defer_stack[d].had_control_exit = true;
      return;
    }
  }
}

// Clear defers at innermost switch scope when hitting case/default
// This is necessary because the transpiler can't track which case was entered at runtime.
// Note: This means defer in case with fallthrough will NOT preserve defers from previous cases.
// For reliable defer behavior in switch, wrap each case body in braces.
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
        (prev_last == '/' && tok_first == '*') ||
        (prev_last == '*' && tok_first == '/'))
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
// Only returns true if there are defers between current scope and the loop/switch
static bool break_has_defers(void)
{
  bool found_loop_or_switch = false;
  bool found_defers = false;
  for (int d = defer_depth - 1; d >= 0; d--)
  {
    if (defer_stack[d].count > 0)
      found_defers = true;
    if (defer_stack[d].is_loop || defer_stack[d].is_switch)
    {
      found_loop_or_switch = true;
      break;
    }
  }
  // Only emit defers if we found the loop/switch boundary
  // (braceless loops don't have a scope marked as loop, so we shouldn't emit)
  return found_loop_or_switch && found_defers;
}

// Check if continue needs to emit defers
// Only returns true if there are defers between current scope and the loop
static bool continue_has_defers(void)
{
  bool found_loop = false;
  bool found_defers = false;
  for (int d = defer_depth - 1; d >= 0; d--)
  {
    if (defer_stack[d].count > 0)
      found_defers = true;
    if (defer_stack[d].is_loop)
    {
      found_loop = true;
      break;
    }
  }
  // Only emit defers if we found the loop boundary
  return found_loop && found_defers;
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
    if (tok->kind == TK_KEYWORD && equal(tok, "defer"))
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
  bool is_typedef_type = false;

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

  // Check for typedef'd type: identifier followed by identifier (no pointer)
  // Pattern: my_type x; - but NOT my_type *x; (ambiguous with multiplication)
  // We only match "TypeName varname;" to avoid misidentifying "a * b;" as a declaration
  if (!saw_type && tok->kind == TK_IDENT)
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
        tok = maybe_type->next; // Move past the type name
      }
    }
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

  // Multiple declarators? Too complex - warn user once
  if (equal(tok, ","))
  {
    static bool warned_multi_decl = false;
    if (!warned_multi_decl)
    {
      fprintf(stderr, "prism: note: multi-declarator '%.*s, ...' not zero-initialized "
                      "(split into separate declarations for auto-init)\n",
              var_name->len, var_name->loc);
      warned_multi_decl = true;
    }
    return NULL;
  }

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
  if (is_array || ((is_struct_type || is_typedef_type) && !is_pointer))
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
  current_func_returns_void = false;
  at_stmt_start = false;               // Reset static - track if we're at start of a statement
  bool next_func_returns_void = false; // Track void functions at top level
  Token *prev_toplevel_tok = NULL;     // Track previous token at top level for function detection

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

      // Error if semicolon not found (ran to EOF or end of block)
      if (stmt_end->kind == TK_EOF || !equal(stmt_end, ";"))
        error_tok(defer_keyword, "unterminated defer statement; expected ';'");

      // Validate defer statement doesn't contain control flow keywords
      // (which would indicate the semicolon came from a different statement)
      int brace_depth = 0;
      for (Token *t = stmt_start; t != stmt_end && t->kind != TK_EOF; t = t->next)
      {
        // Check at_bol BEFORE updating brace_depth, skip for { and } tokens themselves
        // But allow multi-line when inside braces (compound statement)
        if (t != stmt_start && t->at_bol && brace_depth == 0 && !equal(t, "{"))
        {
          error_tok(defer_keyword,
                    "defer statement spans multiple lines without ';' - add semicolon");
        }
        if (equal(t, "{")) brace_depth++;
        else if (equal(t, "}")) brace_depth--;
        if (t->kind == TK_KEYWORD &&
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
      if (break_has_defers())
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
      if (continue_has_defers())
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
          end_statement_after_semicolon();
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
    // This prevents defers from leaking across cases (which would cause incorrect behavior
    // since the transpiler can't know which case is entered at runtime)
    if (feature_defer && tok->kind == TK_KEYWORD && (equal(tok, "case") || equal(tok, "default")))
    {
      // Check if there are active defers that would be lost (fallthrough scenario)
      for (int d = defer_depth - 1; d >= 0; d--)
      {
        if (defer_stack[d].is_switch)
        {
          // Only error if: there are defers AND no control exit (break/return) since the last defer
          if (defer_stack[d].count > 0 && !defer_stack[d].had_control_exit)
          {
            // There are defers that will be cleared - this is a resource leak!
            // Make it an error to force the user to fix it.
            error_tok(defer_stack[d].defer_tok[0],
                      "defer would be skipped due to switch fallthrough at %s:%d. "
                      "Add 'break;' before the next case, or wrap case body in braces.",
                      tok->file->name, tok->line_no);
          }
          break;
        }
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
    // Handles: void func(, static void func(, __attribute__((...)) void func(, etc.
    // This sets next_func_returns_void for when we enter the function body
    if (defer_depth == 0 && equal(tok, "void"))
    {
      Token *t = tok->next;
      // Skip pointers (void *func is not void-returning)
      if (t && equal(t, "*"))
      {
        // void* - not a void function
      }
      else
      {
        // Skip attributes and qualifiers after void
        while (t && (equal(t, "__attribute__") || equal(t, "__declspec") ||
                     equal(t, "const") || equal(t, "volatile") ||
                     equal(t, "__restrict") || equal(t, "__restrict__")))
        {
          if (equal(t, "__attribute__") || equal(t, "__declspec"))
          {
            t = t->next;
            // Skip ((...))
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
        // Now t should be at the function name
        if (t && t->kind == TK_IDENT)
        {
          Token *after_name = t->next;
          if (after_name && equal(after_name, "("))
          {
            // This looks like: void [attrs] func_name( - it's a void function
            next_func_returns_void = true;
          }
        }
      }
    }

    // Also detect void after specifiers: static void, inline void, extern void
    if (defer_depth == 0 && (equal(tok, "static") || equal(tok, "inline") ||
                             equal(tok, "extern") || equal(tok, "_Noreturn") ||
                             equal(tok, "__inline") || equal(tok, "__inline__")))
    {
      // Look ahead for void
      Token *t = tok->next;
      // Skip more specifiers
      while (t && (equal(t, "static") || equal(t, "inline") || equal(t, "extern") ||
                   equal(t, "_Noreturn") || equal(t, "__inline") || equal(t, "__inline__") ||
                   equal(t, "__attribute__") || equal(t, "__declspec")))
      {
        if (equal(t, "__attribute__") || equal(t, "__declspec"))
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
      // Now check if we're at void
      if (t && equal(t, "void"))
      {
        t = t->next;
        // Skip pointers
        if (t && !equal(t, "*"))
        {
          // Skip attributes after void
          while (t && (equal(t, "__attribute__") || equal(t, "__declspec")))
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
          if (t && t->kind == TK_IDENT)
          {
            Token *after_name = t->next;
            if (after_name && equal(after_name, "("))
            {
              next_func_returns_void = true;
            }
          }
        }
      }
    }

    // Track struct/union/enum to avoid zero-init inside them
    if (equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum"))
    {
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
// Build system

// Escape a string for safe use in shell commands
// Returns a newly allocated string that must be freed
static char *shell_escape(const char *s)
{
#ifdef _WIN32
  // Windows: Use double quotes, escape internal double quotes and backslashes
  // before quotes (cmd.exe uses \" for escaping, but backslashes before quotes
  // need escaping too)
  size_t len = 0;
  for (const char *p = s; *p; p++)
  {
    if (*p == '"')
      len += 2; // \"
    else if (*p == '\\')
    {
      // Count consecutive backslashes
      int bs = 0;
      while (p[bs] == '\\')
        bs++;
      if (p[bs] == '"' || p[bs] == '\0')
        len += bs * 2; // Double backslashes before quote or end
      else
        len += bs;
      p += bs - 1;
    }
    else
    {
      len += 1;
    }
  }
  len += 3; // opening ", closing ", and null terminator

  char *result = malloc(len);
  if (!result)
    return NULL;

  char *out = result;
  *out++ = '"';
  for (const char *p = s; *p; p++)
  {
    if (*p == '"')
    {
      *out++ = '\\';
      *out++ = '"';
    }
    else if (*p == '\\')
    {
      int bs = 0;
      while (p[bs] == '\\')
        bs++;
      if (p[bs] == '"' || p[bs] == '\0')
      {
        // Double the backslashes
        for (int i = 0; i < bs * 2; i++)
          *out++ = '\\';
      }
      else
      {
        for (int i = 0; i < bs; i++)
          *out++ = '\\';
      }
      p += bs - 1;
    }
    else
    {
      *out++ = *p;
    }
  }
  *out++ = '"';
  *out = '\0';
  return result;
#else
  // POSIX: Use single quotes, escape single quotes as '\''
  size_t len = 0;
  for (const char *p = s; *p; p++)
  {
    if (*p == '\'')
      len += 4; // '\'' to escape a single quote
    else
      len += 1;
  }
  len += 3; // opening ', closing ', and null terminator

  char *result = malloc(len);
  if (!result)
    return NULL;

  char *out = result;
  *out++ = '\'';
  for (const char *p = s; *p; p++)
  {
    if (*p == '\'')
    {
      // End quote, escaped quote, start quote again: '\''
      *out++ = '\'';
      *out++ = '\\';
      *out++ = '\'';
      *out++ = '\'';
    }
    else
    {
      *out++ = *p;
    }
  }
  *out++ = '\'';
  *out = '\0';
  return result;
#endif
}

// Maximum arguments for run_command
#define MAX_ARGS 128

// Split a space-separated string into an argv array
// Returns number of arguments, or -1 on error
// Caller must free each element and the array itself
static int split_args(const char *str, char ***argv_out)
{
  char **argv = calloc(MAX_ARGS, sizeof(char *));
  if (!argv)
    return -1;

  int argc = 0;
  const char *p = str;

  while (*p && argc < MAX_ARGS - 1)
  {
    // Skip leading whitespace
    while (*p && isspace(*p))
      p++;
    if (!*p)
      break;

    // Find end of this argument
    const char *start = p;
    while (*p && !isspace(*p))
      p++;

    // Copy argument
    size_t len = p - start;
    argv[argc] = malloc(len + 1);
    if (!argv[argc])
    {
      // Cleanup on failure
      for (int i = 0; i < argc; i++)
        free(argv[i]);
      free(argv);
      return -1;
    }
    memcpy(argv[argc], start, len);
    argv[argc][len] = '\0';
    argc++;
  }

  argv[argc] = NULL;
  *argv_out = argv;
  return argc;
}

// Free an argv array allocated by split_args or build_argv
static void free_argv(char **argv)
{
  if (!argv)
    return;
  for (int i = 0; argv[i]; i++)
    free(argv[i]);
  free(argv);
}

// Run a command without invoking a shell (secure execution)
// Returns the exit status, or -1 on error
static int run_command(char **argv)
{
#ifdef _WIN32
  // Windows: use _spawnvp
  intptr_t status = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
  return (int)status;
#else
  // POSIX: fork + execvp
  pid_t pid = fork();
  if (pid == -1)
  {
    perror("fork");
    return -1;
  }
  if (pid == 0)
  {
    // Child process
    execvp(argv[0], argv);
    // If execvp returns, it failed
    perror("execvp");
    _exit(127);
  }
  // Parent process
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

// Build an argv array from individual components
// Variable args must end with NULL
// Returns argv array (caller must free with free_argv)
static char **build_argv(const char *first, ...)
{
  char **argv = calloc(MAX_ARGS, sizeof(char *));
  if (!argv)
    return NULL;

  int argc = 0;
  va_list ap;

  // Add first argument
  if (first)
  {
    argv[argc] = strdup(first);
    if (!argv[argc])
    {
      free(argv);
      return NULL;
    }
    argc++;
  }

  // Add remaining arguments
  va_start(ap, first);
  const char *arg;
  while ((arg = va_arg(ap, const char *)) != NULL && argc < MAX_ARGS - 1)
  {
    argv[argc] = strdup(arg);
    if (!argv[argc])
    {
      va_end(ap);
      free_argv(argv);
      return NULL;
    }
    argc++;
  }
  va_end(ap);

  argv[argc] = NULL;
  return argv;
}

// Append arguments from flags string to existing argv
// Returns new argc, or -1 on error
static int append_flags_to_argv(char **argv, int argc, const char *flags)
{
  if (!flags || !*flags)
    return argc;

  const char *p = flags;
  while (*p && argc < MAX_ARGS - 1)
  {
    while (*p && isspace(*p))
      p++;
    if (!*p)
      break;

    const char *start = p;
    while (*p && !isspace(*p))
      p++;

    size_t len = p - start;
    argv[argc] = malloc(len + 1);
    if (!argv[argc])
      return -1;
    memcpy(argv[argc], start, len);
    argv[argc][len] = '\0';
    argc++;
  }
  argv[argc] = NULL;
  return argc;
}

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
    // Fallback to sudo - use run_command for security
    // First try to remove existing file
    char **rm_argv = build_argv("sudo", "rm", "-f", INSTALL, NULL);
    if (rm_argv)
    {
      run_command(rm_argv);
      free_argv(rm_argv);
    }

    // Copy the file
    char **cp_argv = build_argv("sudo", "cp", self_path, INSTALL, NULL);
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
    char **chmod_argv = build_argv("sudo", "chmod", "+x", INSTALL, NULL);
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
  chmod(INSTALL, 0755);
#endif

  printf("[prism] Installed!\n");
  return 0;
}

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

  // Parse options until we hit a .c file
  while (arg_idx < argc)
  {
    char *arg = argv[arg_idx];
    size_t len = strlen(arg);
    // Check if filename ends with .c
    if (len >= 2 && arg[len - 2] == '.' && arg[len - 1] == 'c')
      break;
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
  char flags[2048], output_path[512], transpiled[512];

  // Generate secure transpiled filename using mkstemp()
  char *basename_ptr = strrchr(source, '/');
#ifdef _WIN32
  char *win_basename = strrchr(source, '\\');
  if (!basename_ptr || (win_basename && win_basename > basename_ptr))
    basename_ptr = win_basename;
#endif

  if (!basename_ptr)
  {
    snprintf(transpiled, sizeof(transpiled), ".%s.XXXXXX.c", source);
  }
  else
  {
    char source_dir[512];
    size_t dir_len = basename_ptr - source;
    if (dir_len >= sizeof(source_dir))
      dir_len = sizeof(source_dir) - 1;
    strncpy(source_dir, source, dir_len);
    source_dir[dir_len] = 0;
    basename_ptr++;
    snprintf(transpiled, sizeof(transpiled), "%s/.%s.XXXXXX.c",
             source_dir, basename_ptr);
  }

  // mkstemp requires template to end with XXXXXX, but we want .c extension
  // So we use mkstemps() on systems that have it, or work around it
#if defined(_WIN32)
  // Windows: use _mktemp_s (less secure but portable)
  if (_mktemp_s(transpiled, sizeof(transpiled)) != 0)
    die("Failed to create temp filename.");
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
  // POSIX: use mkstemps() if available (suffix length = 2 for ".c")
  {
    int fd = mkstemps(transpiled, 2);
    if (fd < 0)
      die("Failed to create secure temp file.");
    close(fd); // Close fd, we just need the unique filename
  }
#else
  // Fallback for systems without mkstemps (rare - most POSIX systems have it)
  // NOTE: This has a small TOCTOU window between unlink and when transpile()
  // creates the file. For truly paranoid security on exotic systems, implement
  // a custom mkstemps equivalent. In practice, this fallback is almost never used.
  {
    int fd = mkstemp(transpiled);
    if (fd < 0)
      die("Failed to create secure temp file.");
    // Immediately close and unlink - we just needed the unique name
    close(fd);
    unlink(transpiled);
    // Append .c extension to the unique name
    size_t len = strlen(transpiled);
    if (len + 2 < sizeof(transpiled))
    {
      transpiled[len] = '.';
      transpiled[len + 1] = 'c';
      transpiled[len + 2] = '\0';
    }
  }
#endif

  // Register cleanup handler and set pending temp file for cleanup on error
  pending_temp_file = transpiled;
  atexit(cleanup_temp_file);

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
      pending_temp_file = NULL;
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
      snprintf(output_path, sizeof(output_path), "%s", custom_output);
    }
    else
    {
      snprintf(output_path, sizeof(output_path), "%s", source);
      char *ext = strrchr(output_path, '.');
      if (ext)
        *ext = 0;
      if (is_windows)
      {
        size_t len = strlen(output_path);
        if (len + 5 < sizeof(output_path)) // ".exe" + null
          memcpy(output_path + len, ".exe", 5);
      }
    }
    printf("[prism] Building %s (%s %d-bit %s)...\n", output_path, arch, bits, platform);
  }
  else
  {
    // Secure temp file for output binary
    snprintf(output_path, sizeof(output_path), "%sprism_out.XXXXXX", TMP);
#if defined(_WIN32)
    if (_mktemp_s(output_path, sizeof(output_path)) != 0)
      die("Failed to create temp output filename.");
#else
    {
      int fd = mkstemp(output_path);
      if (fd < 0)
        die("Failed to create secure temp output file.");
      close(fd);
    }
#endif
  }

  // Build argument array for compilation (no shell escaping needed with execvp!)
  char **compile_argv = calloc(MAX_ARGS, sizeof(char *));
  if (!compile_argv)
    die("Memory allocation failed.");

  int compile_argc = 0;
  compile_argv[compile_argc++] = strdup(compiler);
  compile_argv[compile_argc++] = strdup(transpiled);
  compile_argv[compile_argc++] = strdup("-o");
  compile_argv[compile_argc++] = strdup(output_path);

  // Check allocations
  for (int i = 0; i < compile_argc; i++)
  {
    if (!compile_argv[i])
    {
      free_argv(compile_argv);
      die("Memory allocation failed.");
    }
  }

  // Append flags
  compile_argc = append_flags_to_argv(compile_argv, compile_argc, flags);
  if (compile_argc < 0)
  {
    free_argv(compile_argv);
    die("Memory allocation failed.");
  }

  int compile_status = run_command(compile_argv);
  free_argv(compile_argv);

  if (compile_status != 0)
  {
    remove(transpiled);
    pending_temp_file = NULL;
    die("Compilation failed.");
  }

  if (!is_build_only)
  {
    // Build argument array for execution
    int run_argc = 0;
    int max_run_args = argc - arg_idx + 1; // output + remaining args + NULL
    char **run_argv = calloc(max_run_args + 1, sizeof(char *));
    if (!run_argv)
      die("Memory allocation failed.");

    run_argv[run_argc++] = strdup(output_path);
    if (!run_argv[0])
    {
      free(run_argv);
      die("Memory allocation failed.");
    }

    // Add user arguments
    for (int j = arg_idx + 1; j < argc; j++)
    {
      run_argv[run_argc] = strdup(argv[j]);
      if (!run_argv[run_argc])
      {
        free_argv(run_argv);
        die("Memory allocation failed.");
      }
      run_argc++;
    }
    run_argv[run_argc] = NULL;

    int run_status = run_command(run_argv);
    free_argv(run_argv);
    remove(output_path);
    remove(transpiled);
    pending_temp_file = NULL;
    exit(run_status);
  }

  remove(transpiled);
  pending_temp_file = NULL;
  return 0;
}
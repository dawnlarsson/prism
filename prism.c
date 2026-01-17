// prism.c - C with defer
// 1: cc -o /tmp/prism prism.c && /tmp/prism install && rm /tmp/prism
// 2: prism prism.c install

#define PRISM_FLAGS "-O3 -flto -s"
#define VERSION "0.10.0"

// Include the tokenizer/preprocessor
#include "parse.c"

// =============================================================================
// Platform detection
// =============================================================================

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

// =============================================================================
// Defer tracking
// =============================================================================

#define MAX_DEFER_DEPTH 64
#define MAX_DEFERS_PER_SCOPE 32

typedef struct
{
  Token *stmts[MAX_DEFERS_PER_SCOPE]; // Start token of each deferred statement
  Token *ends[MAX_DEFERS_PER_SCOPE];  // End token (the semicolon)
  int count;
  bool is_loop;   // true if this scope is a for/while/do loop
  bool is_switch; // true if this scope is a switch statement
} DeferScope;

static DeferScope defer_stack[MAX_DEFER_DEPTH];
static int defer_depth = 0;

// Track pending loop/switch for next scope
static bool next_scope_is_loop = false;
static bool next_scope_is_switch = false;
static bool pending_control_flow = false; // True after if/else/for/while/do/switch until we see { or ;

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

static void defer_add(Token *start, Token *end)
{
  if (defer_depth <= 0)
    error_tok(start, "defer outside of any scope");
  DeferScope *scope = &defer_stack[defer_depth - 1];
  if (scope->count >= MAX_DEFERS_PER_SCOPE)
    error_tok(start, "too many defers in scope (max %d)", MAX_DEFERS_PER_SCOPE);
  scope->stmts[scope->count] = start;
  scope->ends[scope->count] = end;
  scope->count++;
}

// =============================================================================
// Token emission
// =============================================================================

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
        (prev_last == '#' && tok_first == '#'))
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
      break; // Stop after the loop/switch scope
  }
}

// Emit defers for continue - from current scope through innermost loop scope
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
      break; // Stop after emitting loop scope's defers
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

// =============================================================================
// Transpiler
// =============================================================================

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

static int transpile(char *input_file, char *output_file)
{
  // Initialize preprocessor
  pp_init();
  pp_add_default_include_paths();

  // Add prism-specific predefined macro
  pp_define_macro("__PRISM__", "1");

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
  last_emitted = NULL;
  next_scope_is_loop = false;
  next_scope_is_switch = false;
  pending_control_flow = false;

  // Walk tokens and emit
  while (tok->kind != TK_EOF)
  {
    // Handle 'defer' keyword
    if (tok->kind == TK_KEYWORD && equal(tok, "defer"))
    {
      // Check for braceless control flow - defer needs a proper scope
      if (pending_control_flow)
        error_tok(tok, "defer cannot be the body of a braceless control statement - add braces");

      tok = tok->next; // skip 'defer'

      // Find the statement (up to semicolon)
      Token *stmt_start = tok;
      Token *stmt_end = skip_to_semicolon(tok);

      // Record the defer
      defer_add(stmt_start, stmt_end);

      // Skip past the semicolon (don't emit the defer yet)
      if (stmt_end->kind != TK_EOF)
        tok = stmt_end->next;
      else
        tok = stmt_end;
      continue;
    }

    // Handle 'return' - evaluate expr, run defers, then return
    if (tok->kind == TK_KEYWORD && equal(tok, "return"))
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
    if (tok->kind == TK_KEYWORD && equal(tok, "break"))
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
    if (tok->kind == TK_KEYWORD && equal(tok, "continue"))
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

    // Mark loop keywords so next '{' knows it's a loop scope
    if (tok->kind == TK_KEYWORD &&
        (equal(tok, "for") || equal(tok, "while") || equal(tok, "do")))
    {
      next_scope_is_loop = true;
      pending_control_flow = true;
    }

    // Mark switch keyword
    if (tok->kind == TK_KEYWORD && equal(tok, "switch"))
    {
      next_scope_is_switch = true;
      pending_control_flow = true;
    }

    // Mark if/else keywords
    if (tok->kind == TK_KEYWORD && (equal(tok, "if") || equal(tok, "else")))
    {
      pending_control_flow = true;
    }

    // Handle '{' - push scope
    if (equal(tok, "{"))
    {
      pending_control_flow = false; // Proper braces found
      emit_tok(tok);
      tok = tok->next;
      defer_push_scope();
      continue;
    }

    // Handle '}' - emit scope defers, then pop
    if (equal(tok, "}"))
    {
      emit_scope_defers();
      defer_pop_scope();
      emit_tok(tok);
      tok = tok->next;
      continue;
    }

    // Default: emit token as-is
    emit_tok(tok);
    tok = tok->next;
  }

  fclose(out);
  return 1;
}

// =============================================================================
// Build system
// =============================================================================

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
           "  debug/release/small  Optimization mode\n"
           "  arm/x86        Architecture (default: native)\n"
           "  32/64          Word size (default: 64)\n"
           "  linux/windows/macos  Platform (default: native)\n\n"
           "Examples:\n"
           "  prism src.c              Run src.c\n"
           "  prism build src.c        Build src\n"
           "  prism build src.c out    Build to 'out'\n"
           "  prism build arm src.c    Build for arm64 linux\n\n"
           "Prism extensions:\n"
           "  defer stmt;    Execute stmt when scope exits\n\n"
           "install\n",
           VERSION);
    return 0;
  }

  if (!strcmp(argv[1], "install"))
    return install(argv[0]);

  int arg_idx = 1, is_build_only = 0;
  Mode mode = MODE_DEFAULT;
  char *arch = NATIVE_ARCH;
  int bits = NATIVE_BITS;
  char *platform = NATIVE_PLATFORM;

  // Parse options until we hit a .c file
  while (arg_idx < argc && !strstr(argv[arg_idx], ".c"))
  {
    char *arg = argv[arg_idx];
    if (!strcmp(arg, "build"))
      is_build_only = 1;
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
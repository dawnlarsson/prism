/* prism.c - A self-building C transpiler with 'defer' support.
   Build: cc -o prism prism.c && ./prism install
   Usage: prism main.c
*/

// 1: cc -o /tmp/prism prism.c && /tmp/prism install && rm /tmp/prism
// 2: prism prism.c install

#define PRISM_FLAGS "-O3 -flto -s"
#define VERSION "0.4.0"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#define INSTALL "prism.exe"
#define TMP ""
#else
#define INSTALL "/usr/local/bin/prism"
#define TMP "/tmp/"
#endif

// --- Defer Logic Structures ---
#define MAX_STACK 128
#define MAX_DEFER 64
#define MAX_LINE 4096

typedef struct
{
  char lines[MAX_DEFER][MAX_LINE];
  int count;
} Scope;

typedef enum
{
  MODE_DEFAULT,
  MODE_DEBUG,
  MODE_RELEASE,
  MODE_SMALL
} Mode;

typedef enum
{
  STATE_CODE,
  STATE_STRING,
  STATE_CHAR,
  STATE_LINE_COMMENT,
  STATE_BLOCK_COMMENT
} ParseState;

// --- Utils ---

void die(char *message)
{
  fprintf(stderr, "[prism] Error: %s\n", message);
  exit(1);
}

// Check if 'word' exists in 'line' as a standalone identifier
char *find_word(char *line, const char *word)
{
  char *p = line;
  while ((p = strstr(p, word)))
  {
    char prev = (p == line) ? ' ' : *(p - 1);
    char next = *(p + strlen(word));
    if (!isalnum(prev) && prev != '_' && !isalnum(next) && next != '_')
    {
      return p;
    }
    p++;
  }
  return NULL;
}

// --- Installation ---

int install(char *self_path)
{
  printf("[prism] Installing to %s...\n", INSTALL);
  remove(INSTALL);

  FILE *input = fopen(self_path, "rb");
  FILE *output = fopen(INSTALL, "wb");

  if (!input || !output)
  {
    char command[512];
    snprintf(command, 512, "sudo rm -f \"%s\" && sudo cp \"%s\" \"%s\" && sudo chmod +x \"%s\"", INSTALL, self_path, INSTALL, INSTALL);
    return system(command) == 0 ? 0 : 1;
  }

  char buffer[4096];
  size_t bytes_read;
  while ((bytes_read = fread(buffer, 1, 4096, input)) > 0)
  {
    fwrite(buffer, 1, bytes_read, output);
  }

  fclose(input);
  fclose(output);

#ifndef _WIN32
  chmod(INSTALL, 0755);
#endif

  printf("[prism] Installed!\n");
  return 0;
}

// --- Transpiler Core ---

int transpile(char *input_file, char *output_file)
{
  FILE *input = fopen(input_file, "r");
  if (!input)
    return 0;

  FILE *output = fopen(output_file, "w");
  if (!output)
  {
    fclose(input);
    return 0;
  }

  // State
  char line[MAX_LINE];
  char clean[MAX_LINE]; // Line with strings/comments masked out
  ParseState state = STATE_CODE;

  Scope stack[MAX_STACK];
  int stack_top = 0;
  stack[0].count = 0; // Global scope

  fprintf(output, "#line 1 \"%s\"\n", input_file); // Sync debuggers

  int line_num = 0;
  while (fgets(line, sizeof(line), input))
  {
    line_num++;

    // 1. Create a "Masked" line for parsing
    // We replace string contents and comments with spaces to avoid false positives
    strcpy(clean, line);
    int escape = 0;

    for (int i = 0; clean[i]; i++)
    {
      char c = clean[i];
      char next = clean[i + 1];

      if (escape)
      {
        escape = 0;
        clean[i] = ' ';
        continue;
      }

      switch (state)
      {
      case STATE_CODE:
        if (c == '"')
          state = STATE_STRING;
        else if (c == '\'')
          state = STATE_CHAR;
        else if (c == '/' && next == '/')
        {
          state = STATE_LINE_COMMENT;
          clean[i] = ' ';
          clean[i + 1] = ' ';
          i++;
        }
        else if (c == '/' && next == '*')
        {
          state = STATE_BLOCK_COMMENT;
          clean[i] = ' ';
          clean[i + 1] = ' ';
          i++;
        }
        break;
      case STATE_STRING:
        if (c == '\\')
          escape = 1;
        else if (c == '"')
          state = STATE_CODE;
        else
          clean[i] = ' '; // Mask content
        break;
      case STATE_CHAR:
        if (c == '\\')
          escape = 1;
        else if (c == '\'')
          state = STATE_CODE;
        else
          clean[i] = ' ';
        break;
      case STATE_LINE_COMMENT:
        if (c == '\n')
          state = STATE_CODE;
        else
          clean[i] = ' ';
        break;
      case STATE_BLOCK_COMMENT:
        if (c == '*' && next == '/')
        {
          state = STATE_CODE;
          clean[i] = ' ';
          clean[i + 1] = ' ';
          i++;
        }
        else
        {
          clean[i] = ' ';
        }
        break;
      }
    }
    // End of line reset for single-line comments
    if (state == STATE_LINE_COMMENT)
      state = STATE_CODE;

    // 2. Process logic using the 'clean' line

    // Scope Open
    if (strchr(clean, '{'))
    {
      if (stack_top < MAX_STACK - 1)
      {
        stack_top++;
        stack[stack_top].count = 0;
      }
    }

    // Defer Statement
    char *defer_pos = find_word(clean, "defer");
    if (defer_pos)
    {
      if (stack_top >= 0 && stack[stack_top].count < MAX_DEFER)
      {
        // Extract exact offset from original line
        int offset = defer_pos - clean;
        char *stmt_start = line + offset + 5; // skip "defer"

        // Basic trim
        while (isspace(*stmt_start))
          stmt_start++;

        // Store it (removing newline if present)
        char *newline = strchr(stmt_start, '\n');
        if (newline)
          *newline = 0;

        strcpy(stack[stack_top].lines[stack[stack_top].count++], stmt_start);

        // Comment out the defer line in output
        fprintf(output, "// [prism] %s", line);
        continue;
      }
    }

    // Return Statement
    if (find_word(clean, "return"))
    {
      // Inject all defers from current scope down to 0 (or until function boundary?)
      // For MVP we dump everything. In a real C parser we'd stop at function start.
      // Here we assume stack[0] is global and shouldn't be dumped on return usually,
      // but for safety we dump top-down.
      for (int s = stack_top; s > 0; s--)
      {
        for (int d = stack[s].count - 1; d >= 0; d--)
        {
          fprintf(output, "{ %s } /* deferred */\n", stack[s].lines[d]);
        }
      }
    }

    // Scope Close
    if (strchr(clean, '}'))
    {
      // Dump current scope defers
      if (stack_top > 0)
      {
        for (int d = stack[stack_top].count - 1; d >= 0; d--)
        {
          fprintf(output, "  { %s } /* deferred */\n", stack[stack_top].lines[d]);
        }
        stack_top--;
      }
    }

    // 3. Passthrough original line
    fputs(line, output);
  }

  fclose(input);
  fclose(output);
  return 1;
}

// --- Flags & Main ---

void get_flags(char *source_file, char *buffer, Mode mode)
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

    // Support shebang-style flags: #define PRISM_FLAGS "-lSDL2"
    if (strncmp(ptr, "#define PRISM_", 14) == 0)
    {
      ptr += 14;
      int match = 0;
      if (strncmp(ptr, "FLAGS ", 6) == 0 || strncmp(ptr, "LIBS ", 5) == 0)
        match = 1;
      else if (mode == MODE_DEBUG && strncmp(ptr, "FLAGS_DEBUG ", 12) == 0)
        match = 1;
      else if (mode == MODE_RELEASE && strncmp(ptr, "FLAGS_RELEASE ", 14) == 0)
        match = 1;

      if (match)
      {
        char *start = strchr(ptr, '"');
        char *end = start ? strchr(start + 1, '"') : NULL;
        if (start && end)
        {
          *end = 0;
          strcat(buffer, " ");
          strcat(buffer, start + 1);
        }
      }
    }
  }
  fclose(file);
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    printf("Prism v%s\nUsage: prism [mode] file.c\n       prism install\n", VERSION);
    return 0;
  }

  if (!strcmp(argv[1], "install"))
    return install(argv[0]);

  int arg_idx = 1;
  int build_only = 0;
  Mode mode = MODE_DEFAULT;

  if (strcmp(argv[arg_idx], "build") == 0)
  {
    build_only = 1;
    arg_idx++;
  }

  if (arg_idx < argc)
  {
    if (strcmp(argv[arg_idx], "debug") == 0)
    {
      mode = MODE_DEBUG;
      arg_idx++;
    }
    else if (strcmp(argv[arg_idx], "release") == 0)
    {
      mode = MODE_RELEASE;
      arg_idx++;
    }
    else if (strcmp(argv[arg_idx], "small") == 0)
    {
      mode = MODE_SMALL;
      arg_idx++;
    }
  }

  if (arg_idx >= argc)
    die("No input file.");

  char *src = argv[arg_idx];
  char flags[2048];
  char transpiled[512];
  char output_bin[512];
  char cmd[4096];

  // Temp file for transpilation
  snprintf(transpiled, 512, "%s.prism.c", src);

  if (!transpile(src, transpiled))
    die("Transpilation failed");

  get_flags(src, flags, mode);

  if (build_only)
  {
    snprintf(output_bin, 512, "%s", src);
    char *ext = strrchr(output_bin, '.');
    if (ext)
      *ext = 0; // strip .c
  }
  else
  {
    snprintf(output_bin, 512, "%sprism_out.%d", TMP, getpid());
  }

  printf("[prism] Compiling %s...\n", src);
  snprintf(cmd, 4096, "cc \"%s\" -o \"%s\" %s", transpiled, output_bin, flags);

  if (system(cmd) != 0)
    die("Compilation Error");

  remove(transpiled);

  if (!build_only)
  {
    snprintf(cmd, 4096, "\"%s\"", output_bin);
    // Forward extra args? (Simplified for now)
    int status = system(cmd);
    remove(output_bin);
    return status;
  }

  return 0;
}
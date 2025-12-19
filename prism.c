// cc -o /tmp/prism prism.c && /tmp/prism install && rm /tmp/prism
#define PRISM_FLAGS "-O3 -flto -s"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include <ctype.h>

#define VERSION "0.1.1"

#ifdef _WIN32
#define INSTALL_PATH "C:\\Windows\\System32\\prism.exe"
#define TMP_DIR "C:\\Windows\\Temp\\"
#define DIR_SEP '\\'
#else
#define INSTALL_PATH "/usr/local/bin/prism"
#define TMP_DIR "/tmp/"
#define DIR_SEP '/'
#endif

// --- Build Modes ---
typedef enum
{
  MODE_DEFAULT,
  MODE_DEBUG,
  MODE_RELEASE,
  MODE_SMALL
} BuildMode;

void die(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr, "\n");
  exit(1);
}

int copy_file(const char *src, const char *dst)
{
  FILE *in = fopen(src, "rb");
  if (!in)
    return 0;

  FILE *out = fopen(dst, "wb");
  if (!out)
  {
    fclose(in);
    return 0;
  }

  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    fwrite(buf, 1, n, out);

  fclose(in);
  fclose(out);

#ifndef _WIN32
  chmod(dst, 0755);
#endif
  return 1;
}

void trim_whitespace(char *str)
{
  char *end;
  while (isspace((unsigned char)*str))
    str++;
  if (*str == 0)
    return;
  end = str + strlen(str) - 1;
  while (end > str && isspace((unsigned char)*end))
    end--;
  end[1] = '\0';
}

void append_flags(char *buffer, size_t size, const char *new_flags)
{
  if (!new_flags || strlen(new_flags) == 0)
    return;
  size_t cur = strlen(buffer);
  if (cur + strlen(new_flags) + 2 < size)
  {
    strcat(buffer, " ");
    strcat(buffer, new_flags);
  }
}

void get_build_flags(const char *src_path, char *buffer, size_t size, BuildMode mode)
{
  buffer[0] = '\0';

  if (mode == MODE_DEBUG)
    append_flags(buffer, size, "-g -O0");
  if (mode == MODE_RELEASE)
    append_flags(buffer, size, "-O3");
  if (mode == MODE_SMALL)
    append_flags(buffer, size, "-Os");

  FILE *f = fopen(src_path, "r");
  if (!f)
    return;

  char line[1024];
  while (fgets(line, sizeof(line), f))
  {
    char *p = line;
    while (isspace((unsigned char)*p))
      p++;

    if (strncmp(p, "#define", 7) != 0)
      continue;
    p += 7;
    if (!isspace((unsigned char)*p))
      continue;
    while (isspace((unsigned char)*p))
      p++;

    if (strncmp(p, "PRISM_", 6) != 0)
      continue;

    int is_base = (strncmp(p, "PRISM_FLAGS ", 12) == 0) || (strncmp(p, "PRISM_FLAGS\"", 11) == 0);
    int is_libs = (strncmp(p, "PRISM_LIBS", 10) == 0);
    int is_debug = (strncmp(p, "PRISM_FLAGS_DEBUG", 17) == 0);
    int is_release = (strncmp(p, "PRISM_FLAGS_RELEASE", 19) == 0);
    int is_small = (strncmp(p, "PRISM_FLAGS_SMALL", 17) == 0);

    int include = 0;
    if (is_base || is_libs)
      include = 1;
    else if (mode == MODE_DEBUG && is_debug)
      include = 1;
    else if (mode == MODE_RELEASE && is_release)
      include = 1;
    else if (mode == MODE_SMALL && is_small)
      include = 1;

    if (!include)
      continue;

    char *quote = strchr(p, '"');
    if (!quote)
      continue;

    char *end_quote = strchr(quote + 1, '"');
    if (!end_quote)
      continue;

    *end_quote = '\0';
    char *flags = quote + 1;

    append_flags(buffer, size, flags);
  }
  fclose(f);
}

void build_and_run(const char *src, int is_build_only, BuildMode mode, int argc, char **argv)
{
  char flags[4096];
  get_build_flags(src, flags, sizeof(flags), mode);

  if (strlen(flags) > 0)
  {
    const char *mstr = "DEFAULT";
    if (mode == MODE_DEBUG)
      mstr = "DEBUG";
    if (mode == MODE_RELEASE)
      mstr = "RELEASE";
    if (mode == MODE_SMALL)
      mstr = "SMALL";
  }

  char out_bin[1024];
  if (is_build_only)
  {
    strncpy(out_bin, src, sizeof(out_bin) - 1);
    char *ext = strrchr(out_bin, '.');
    if (ext)
      *ext = '\0';
  }
  else
  {
    snprintf(out_bin, sizeof(out_bin), "%sprism_out", TMP_DIR);
  }

  char cmd[8192];
  snprintf(cmd, sizeof(cmd), "cc \"%s\" -o \"%s\"%s", src, out_bin, flags);

  if (is_build_only)
    printf("[prism] Building %s...\n", out_bin);

  if (system(cmd) != 0)
    die("Compilation failed.");

  if (!is_build_only)
  {
    char run_cmd[8192];
    snprintf(run_cmd, sizeof(run_cmd), "\"%s\"", out_bin);

    int arg_start = -1;
    for (int i = 0; i < argc; i++)
    {
      if (argv[i] == src)
      {
        arg_start = i + 1;
        break;
      }
    }

    if (arg_start != -1)
    {
      for (int i = arg_start; i < argc; i++)
      {
        size_t len = strlen(run_cmd);
        if (len + strlen(argv[i]) + 4 < sizeof(run_cmd))
          snprintf(run_cmd + len, sizeof(run_cmd) - len, " \"%s\"", argv[i]);
      }
    }

    int status = system(run_cmd);
    remove(out_bin);
    if (status != 0)
    {
#ifdef _WIN32
      exit(status);
#else
      if (status > 255)
        exit(status >> 8);
      else
        exit(status);
#endif
    }
  }
}

void install_self(const char *self_path)
{
  printf("[prism] Installing %s to %s...\n", self_path, INSTALL_PATH);

  if (copy_file(self_path, INSTALL_PATH))
  {
    printf("[prism] Successfully installed!\n");
    return;
  }

  if (errno == EACCES || errno == EPERM)
  {
    printf("[prism] Permission denied. Attempting with sudo...\n");
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "sudo rm -f \"%s\" && sudo cp \"%s\" \"%s\" && sudo chmod +x \"%s\"",
             INSTALL_PATH, self_path, INSTALL_PATH, INSTALL_PATH);

    if (system(cmd) == 0)
    {
      printf("[prism] Successfully installed via sudo!\n");
    }
    else
    {
      die("Installation failed.");
    }
  }
  else
  {
    die("Failed to copy file. (Error: %s)", strerror(errno));
  }
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    printf("Prism v%s\n", VERSION);
    printf("Usage:\n");
    printf("  prism [mode] source.c [args]   Compile & Run (Mode: debug, release, small)\n");
    printf("  prism build [mode] source.c    Compile only\n");
    printf("  prism install                  Install to system\n");
    return 1;
  }

  if (strcmp(argv[1], "install") == 0)
  {
    install_self(argv[0]);
    return 0;
  }

  int arg_idx = 1;
  int is_build_only = 0;
  BuildMode mode = MODE_DEFAULT;

  if (strcmp(argv[arg_idx], "build") == 0)
  {
    is_build_only = 1;
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
    die("Missing source file.");

  const char *src = argv[arg_idx];
  build_and_run(src, is_build_only, mode, argc, argv);
  return 0;
}
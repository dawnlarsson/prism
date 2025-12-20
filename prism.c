// 1: cc -o /tmp/prism prism.c && /tmp/prism install && rm /tmp/prism
// 2: prism prism.c install

#define PRISM_FLAGS "-O3 -flto -s"
#define VERSION "0.2.0"

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

typedef enum
{
  MODE_DEFAULT,
  MODE_DEBUG,
  MODE_RELEASE,
  MODE_SMALL
} Mode;

void die(char *message)
{
  fprintf(stderr, "%s\n", message);
  exit(1);
}

int install(char *self_path)
{
  printf("[prism] Installing to %s...\n", INSTALL);
  remove(INSTALL);

  FILE *input = fopen(self_path, "rb"), *output = fopen(INSTALL, "wb");

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
    if (fwrite(buffer, 1, bytes_read, output) != bytes_read)
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

  char line[4096];
  while (fgets(line, sizeof(line), input))
  {
    fputs(line, output);
  }

  fclose(input);
  fclose(output);
  return 1;
}

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

  char line[1024], *ptr, *quote_start, *quote_end;
  while (fgets(line, 1024, file))
  {
    ptr = line;
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

    if ((quote_start = strchr(ptr, '"')) && (quote_end = strchr(quote_start + 1, '"')))
    {
      *quote_end = 0;
      strcat(buffer, " ");
      strcat(buffer, quote_start + 1);
    }
  }

  fclose(file);
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    printf("Prism v%s\n\
Usage:  prism [mode] src.c [args]   (mode: debug,release,small)\n\
        build [mode] src.c\n\
        install\n",
           VERSION);
    return 0;
  }

  if (!strcmp(argv[1], "install"))
    return install(argv[0]);

  int arg_idx = 1, is_build_only = 0;
  Mode mode = MODE_DEFAULT;

  if (!strcmp(argv[arg_idx], "build"))
  {
    is_build_only = 1;
    arg_idx++;
  }

  if (arg_idx < argc)
  {
    if (!strcmp(argv[arg_idx], "debug"))
      mode = MODE_DEBUG, arg_idx++;
    else if (!strcmp(argv[arg_idx], "release"))
      mode = MODE_RELEASE, arg_idx++;
    else if (!strcmp(argv[arg_idx], "small"))
      mode = MODE_SMALL, arg_idx++;
  }

  if (arg_idx >= argc)
    die("Missing source file.");

  char *source = argv[arg_idx], flags[2048], output[512], command[4096];
  char transpiled[512];

  char *basename = strrchr(source, '/');
#ifdef _WIN32
  char *win_basename = strrchr(source, '\\');
  if (!basename || (win_basename && win_basename > basename))
    basename = win_basename;
#endif
  if (!basename)
    basename = source;
  else
    basename++;

  snprintf(transpiled, sizeof(transpiled), "%s%s.%d.transpiled.c", TMP, basename, getpid());

  if (!transpile(source, transpiled))
    die("Transpilation failed.");

  get_flags(source, flags, mode);

  if (is_build_only)
  {
    strncpy(output, source, 511);
    char *extension = strrchr(output, '.');
    if (extension)
      *extension = 0;
    printf("[prism] Building %s...\n", output);
  }
  else
    snprintf(output, sizeof(output), "%sprism_out.%d", TMP, getpid());

  snprintf(command, 4096, "cc \"%s\" -o \"%s\"%s", transpiled, output, flags);

  if (system(command))
    die("Compilation failed.");

  if (!is_build_only)
  {
    snprintf(command, 4096, "\"%s\"", output);

    for (int j = arg_idx + 1; j < argc; j++)
    {
      size_t len = strlen(command);
      if (len + strlen(argv[j]) + 4 < 4096)
        snprintf(command + len, 4096 - len, " \"%s\"", argv[j]);
    }

    int status = system(command);
    remove(output);
    remove(transpiled);
    exit(status > 255 ? status >> 8 : status);
  }

  remove(transpiled);
  return 0;
}

// cc -o /tmp/prism prism.c && /tmp/prism install && rm /tmp/prism
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>

#define VERSION "0.1.0"

#ifdef _WIN32
#define INSTALL_PATH "C:\\Windows\\System32\\prism.exe"
#define TMP_DIR "C:\\Windows\\Temp\\"
#define DIR_SEP '\\'
#else
#define INSTALL_PATH "/usr/local/bin/prism"
#define TMP_DIR "/tmp/"
#define DIR_SEP '/'
#endif

const char *get_basename(const char *path)
{
  const char *base = strrchr(path, DIR_SEP);
  return base ? base + 1 : path;
}

void strip_ext(char *fname)
{
  char *ext = strrchr(fname, '.');
  if (ext)
    *ext = '\0';
}

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

  remove(dst);

  FILE *out = fopen(dst, "wb");
  if (!out)
  {
    fclose(in);
    return 0;
  }

  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
  {
    if (fwrite(buf, 1, n, out) != n)
    {
      fclose(in);
      fclose(out);
      return 0;
    }
  }

  fclose(in);
  fclose(out);

#ifndef _WIN32
  chmod(dst, 0755);
#endif

  return 1;
}

void compile(const char *src, const char *out)
{
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "cc \"%s\" -o \"%s\"", src, out);

  if (system(cmd) != 0)
  {
    die("Compilation failed.");
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

    snprintf(cmd, sizeof(cmd), "sudo rm -f \"%s\" && sudo cp \"%s\" \"%s\" && sudo chmod +x \"%s\"",
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
    printf("  prism source.c [args]  Compile to /tmp and run with args\n");
    printf("  prism build source.c   Compile to local binary\n");
    printf("  prism install          Install prism to %s\n", INSTALL_PATH);
    return 1;
  }

  if (strcmp(argv[1], "install") == 0)
  {
    install_self(argv[0]);
    return 0;
  }

  if (strcmp(argv[1], "build") == 0)
  {
    if (argc < 3)
      die("Missing source file for build.");
    const char *src = argv[2];

    char out[1024];
    strncpy(out, src, sizeof(out) - 1);
    strip_ext(out);

    printf("[prism] Building %s...\n", out);
    compile(src, out);
    return 0;
  }

  const char *src = argv[1];

  char out[1024];
  const char *base = get_basename(src);

  snprintf(out, sizeof(out), "%s%s", TMP_DIR, base);
  strip_ext(out);

  compile(src, out);

  char run_cmd[4096];
  snprintf(run_cmd, sizeof(run_cmd), "\"%s\"", out);

  for (int i = 2; i < argc; i++)
  {
    size_t current_len = strlen(run_cmd);

    if (current_len + strlen(argv[i]) + 4 >= sizeof(run_cmd))
      die("Arguments too long.");

    snprintf(run_cmd + current_len, sizeof(run_cmd) - current_len, " \"%s\"", argv[i]);
  }

  return system(run_cmd);
}
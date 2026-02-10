// Prism POSIX shim layer for native Windows (MSVC cl.exe)

#ifndef PRISM_WINDOWS_C
#define PRISM_WINDOWS_C

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <share.h>
#include <errno.h>

// Must be included BEFORE GCC-ism macros (especially noreturn) to avoid
// poisoning system headers like <setjmp.h> and <stdlib.h>.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#include <BaseTsd.h>

typedef SSIZE_T ssize_t;
typedef intptr_t pid_t;
typedef int mode_t;

// MSVC doesn't have these — define them away or provide equivalents.
#define __attribute__(x)
#define __builtin_expect(expr, val) (expr)
#define __builtin_constant_p(x) 0
#define __builtin_strlen(s) strlen(s)
#define __typeof__(x) const char * // only used for CLI_PUSH's array elem type

#define WIFEXITED(s) 1     // Always "exited" on Windows (no signals)
#define WEXITSTATUS(s) (s) // status IS the exit code
#define WIFSIGNALED(s) 0   // No signal concept
#define WTERMSIG(s) 0

#ifndef noreturn // Define AFTER including all system headers so we don't poison them.
#define noreturn __declspec(noreturn)
#endif

#ifndef environ
#define environ _environ
#endif

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif

#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif

#ifndef O_WRONLY
#define O_WRONLY _O_WRONLY
#endif
#ifndef O_CREAT
#define O_CREAT _O_CREAT
#endif
#ifndef O_EXCL
#define O_EXCL _O_EXCL
#endif

#ifndef S_IRUSR
#define S_IRUSR _S_IREAD
#endif
#ifndef S_IWUSR
#define S_IWUSR _S_IWRITE
#endif

#define strdup _strdup
#define fileno _fileno
#define fdopen _fdopen
#define access _access
#define getpid _getpid
#define unlink _unlink
#define close _close
#define popen _popen
#define pclose _pclose
// Redirect read() calls to our wrapper via macro.
// Using a unique name avoids conflicting with MSVC ucrt's read() declaration.
#define read prism_posix_read_

#define SPAWN_ACTION_MAX 8

typedef enum
{
    SPAWN_ACT_CLOSE,
    SPAWN_ACT_DUP2,
    SPAWN_ACT_OPEN
} SpawnActionKind;

typedef struct
{
    SpawnActionKind kind;
    int fd;           // target fd
    int src_fd;       // source fd (for dup2)
    const char *path; // path (for open)
    int oflag;        // open flags
    mode_t mode;      // open mode
} SpawnAction;

typedef struct
{
    SpawnAction actions[SPAWN_ACTION_MAX];
    int count;
} posix_spawn_file_actions_t;

static int pipe(int pipefd[2]) { return _pipe(pipefd, 65536, _O_BINARY); }
static char *realpath(const char *path, char *resolved) { return _fullpath(resolved, path, PATH_MAX); }

// MSVC _read takes unsigned int for count, returns int
static ssize_t prism_posix_read_(int fd, void *buf, size_t count)
{
    unsigned int to_read = (count > 0x7FFFFFFF) ? 0x7FFFFFFF : (unsigned int)count;
    return (ssize_t)_read(fd, buf, to_read);
}

// tries up to 10000 unique names with randomized template + _sopen_s
static int mkstemp(char *tmpl)
{
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t len = strlen(tmpl);
    char *try_buf = (char *)malloc(len + 1);
    if (!try_buf)
        return -1;

    // Find the X's in the template
    size_t x_start = len;
    while (x_start > 0 && tmpl[x_start - 1] == 'X')
        x_start--;
    size_t x_count = len - x_start;

    // Seed with PID + high-resolution timer + tick count for better entropy
    // Avoids collisions in parallel build systems (e.g., ninja -j32)
    LARGE_INTEGER perf_counter;
    QueryPerformanceCounter(&perf_counter);
    unsigned int seed = (unsigned int)_getpid() ^ (unsigned int)GetTickCount() ^ (unsigned int)perf_counter.LowPart ^ (unsigned int)(perf_counter.LowPart >> 16);

    for (int attempt = 0; attempt < 10000; attempt++)
    {
        memcpy(try_buf, tmpl, len + 1);
        // Per-attempt seed: mix attempt number to ensure distinct sequences
        // even if multiple processes share the same initial seed
        unsigned int attempt_seed = seed ^ ((unsigned int)attempt * 2654435761u);
        for (size_t i = x_start; i < x_start + x_count; i++)
        {
            attempt_seed = attempt_seed * 1103515245 + 12345;
            try_buf[i] = chars[(attempt_seed >> 16) % (sizeof(chars) - 1)];
        }
        int fd;
        errno_t err = _sopen_s(&fd, try_buf, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                               _SH_DENYNO, _S_IREAD | _S_IWRITE);
        if (err == 0 && fd >= 0)
        {
            memcpy(tmpl, try_buf, len + 1);
            free(try_buf);
            return fd;
        }
    }
    free(try_buf);
    return -1;
}

static int mkstemps(char *tmpl, int suffix_len)
{
    if (suffix_len <= 0)
        return mkstemp(tmpl);

    // Template is like "foo.XXXXXX.c" — X's are before the suffix.
    // Temporarily strip the suffix so mkstemp sees X's at the end.
    size_t len = strlen(tmpl);
    if ((size_t)suffix_len >= len)
        return -1;
    char saved[16]; // suffix_len is always small (2 for ".c")
    if (suffix_len > (int)sizeof(saved))
        return -1;
    memcpy(saved, tmpl + len - suffix_len, suffix_len);
    tmpl[len - suffix_len] = '\0';

    int fd = mkstemp(tmpl);
    if (fd < 0)
    {
        // Restore suffix on failure
        memcpy(tmpl + len - suffix_len, saved, suffix_len);
        tmpl[len] = '\0';
        return -1;
    }

    // mkstemp succeeded — restore suffix and rename the file
    // tmpl now has the randomized name without suffix
    char with_suffix[PATH_MAX];
    size_t new_len = strlen(tmpl);
    if (new_len + suffix_len >= sizeof(with_suffix))
    {
        _close(fd);
        _unlink(tmpl);
        return -1;
    }
    memcpy(with_suffix, tmpl, new_len);
    memcpy(with_suffix + new_len, saved, suffix_len);
    with_suffix[new_len + suffix_len] = '\0';

    // Close the fd, rename, then reopen
    _close(fd);
    if (rename(tmpl, with_suffix) != 0)
    {
        _unlink(tmpl);
        return -1;
    }
    memcpy(tmpl, with_suffix, new_len + suffix_len + 1);

    // Reopen with same flags as mkstemp
    errno_t err = _sopen_s(&fd, tmpl, _O_RDWR | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE);
    if (err != 0)
    {
        _unlink(tmpl);
        return -1;
    }
    return fd;
}

// No-op on Windows
static int chmod(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    return 0;
}

// No-op on Windows
static ssize_t readlink(const char *path, char *buf, size_t bufsize)
{
    (void)path;
    (void)buf;
    (void)bufsize;
    return -1;
}

static int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa)
{
    fa->count = 0;
    return 0;
}

static int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa)
{
    for (int i = 0; i < fa->count; i++)
    {
        // path was strdup'd in addopen
        if (fa->actions[i].kind == SPAWN_ACT_OPEN && fa->actions[i].path)
            free((void *)fa->actions[i].path);
    }
    fa->count = 0;
    return 0;
}

static int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd)
{
    if (fa->count >= SPAWN_ACTION_MAX)
        return -1;
    fa->actions[fa->count++] = (SpawnAction){.kind = SPAWN_ACT_CLOSE, .fd = fd};
    return 0;
}

static int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int src_fd, int target_fd)
{
    if (fa->count >= SPAWN_ACTION_MAX)
        return -1;
    fa->actions[fa->count++] = (SpawnAction){.kind = SPAWN_ACT_DUP2, .fd = target_fd, .src_fd = src_fd};
    return 0;
}

static int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd,
                                            const char *path, int oflag, mode_t mode)
{
    if (fa->count >= SPAWN_ACTION_MAX)
        return -1;
    fa->actions[fa->count++] = (SpawnAction){
        .kind = SPAWN_ACT_OPEN, .fd = fd, .path = _strdup(path), .oflag = oflag, .mode = mode};
    return 0;
}

// Build a command line string from argv, following MSVC/CommandLineToArgvW
// escaping rules: backslashes are literal unless immediately before a double
// quote, in which case they must be doubled. Embedded quotes are escaped as \".
static char *win32_argv_to_cmdline(char **argv)
{
    // First pass: compute total length needed
    size_t total = 0;
    for (int i = 0; argv[i]; i++)
    {
        if (i > 0)
            total++; // space separator
        const char *s = argv[i];
        int needs_quote = (s[0] == '\0' || strchr(s, ' ') || strchr(s, '\t') || strchr(s, '"'));
        if (needs_quote)
            total += 2; // opening and closing quotes
        for (const char *c = s; *c; c++)
        {
            if (*c == '"')
                total += 2; // \" escape
            else if (*c == '\\')
            {
                size_t run = 0;
                while (c[run] == '\\')
                    run++;
                if (c[run] == '"' || (c[run] == '\0' && needs_quote))
                    total += run * 2; // double backslashes before quote
                else
                    total += run; // literal backslashes
                c += run - 1;
            }
            else
                total++;
        }
    }

    char *cmdline = (char *)malloc(total + 1);
    if (!cmdline)
        return NULL;
    char *p = cmdline;

    // Second pass: emit quoted/escaped arguments
    for (int i = 0; argv[i]; i++)
    {
        if (i > 0)
            *p++ = ' ';
        const char *s = argv[i];
        int needs_quote = (s[0] == '\0' || strchr(s, ' ') || strchr(s, '\t') || strchr(s, '"'));
        if (needs_quote)
            *p++ = '"';
        for (const char *c = s; *c; c++)
        {
            if (*c == '\\')
            {
                size_t run = 0;
                while (c[run] == '\\')
                    run++;
                // Double backslashes only if followed by " or end-of-string inside quotes
                if (c[run] == '"' || (c[run] == '\0' && needs_quote))
                    for (size_t j = 0; j < run * 2; j++)
                        *p++ = '\\';
                else
                    for (size_t j = 0; j < run; j++)
                        *p++ = '\\';
                c += run - 1;
            }
            else if (*c == '"')
            {
                *p++ = '\\';
                *p++ = '"';
            }
            else
                *p++ = *c;
        }
        if (needs_quote)
            *p++ = '"';
    }
    *p = '\0';
    return cmdline;
}

// Resolve file actions into STARTUPINFO handle redirections
// Returns HANDLE for the spawned process, or INVALID_HANDLE_VALUE on failure
static HANDLE win32_spawn_with_actions(char **argv, posix_spawn_file_actions_t *fa, char **envp)
{
    (void)envp; // environment filtering handled separately on Windows

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    // We need to figure out which handles to redirect based on the file actions.
    // The pattern used by prism is always:
    //   - close one end of a pipe
    //   - dup2 the other end to stdin or stdout
    //   - close the original
    //   - optionally open /dev/null on stderr
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hStdErr = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE hNul = INVALID_HANDLE_VALUE;

    if (fa)
    {
        si.dwFlags |= STARTF_USESTDHANDLES;
        for (int i = 0; i < fa->count; i++)
        {
            SpawnAction *a = &fa->actions[i];
            if (a->kind == SPAWN_ACT_DUP2)
            {
                // Convert C fd to Win32 HANDLE
                HANDLE h = (HANDLE)_get_osfhandle(a->src_fd);
                if (a->fd == STDOUT_FILENO)
                    hStdOut = h;
                else if (a->fd == STDIN_FILENO)
                    hStdIn = h;
                else if (a->fd == STDERR_FILENO)
                    hStdErr = h;
            }
            else if (a->kind == SPAWN_ACT_OPEN)
            {
                // Only case used: open /dev/null on stderr
                // Map /dev/null → NUL
                const char *winpath = a->path;
                if (strcmp(winpath, "/dev/null") == 0)
                    winpath = "NUL";

                SECURITY_ATTRIBUTES sa_nul = {sizeof(sa_nul), NULL, TRUE};
                hNul = CreateFileA(winpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &sa_nul, OPEN_EXISTING, 0, NULL);
                if (a->fd == STDERR_FILENO)
                    hStdErr = hNul;
                else if (a->fd == STDOUT_FILENO)
                    hStdOut = hNul;
            }
            // SPAWN_ACT_CLOSE: we handle these after spawn (close our end of pipes)
        }
        si.hStdInput = hStdIn;
        si.hStdOutput = hStdOut;
        si.hStdError = hStdErr;
    }

    // Make pipe handles inheritable, saving original flags for cleanup
    DWORD orig_in_flags = 0, orig_out_flags = 0, orig_err_flags = 0;
    if (si.dwFlags & STARTF_USESTDHANDLES)
    {
        if (hStdIn != INVALID_HANDLE_VALUE)
        {
            GetHandleInformation(hStdIn, &orig_in_flags);
            SetHandleInformation(hStdIn, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        }
        if (hStdOut != INVALID_HANDLE_VALUE)
        {
            GetHandleInformation(hStdOut, &orig_out_flags);
            SetHandleInformation(hStdOut, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        }
        if (hStdErr != INVALID_HANDLE_VALUE)
        {
            GetHandleInformation(hStdErr, &orig_err_flags);
            SetHandleInformation(hStdErr, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        }
    }

    HANDLE hResult = INVALID_HANDLE_VALUE;
    char *cmdline = win32_argv_to_cmdline(argv);
    if (!cmdline)
        goto cleanup;

    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(cmdline);

    if (!ok)
        fprintf(stderr, "CreateProcess failed for '%s': error %lu\n", argv[0], GetLastError());
    else
    {
        CloseHandle(pi.hThread);
        hResult = pi.hProcess;
    }

cleanup:
    // Restore original handle inheritance flags
    if (si.dwFlags & STARTF_USESTDHANDLES)
    {
        if (hStdIn != INVALID_HANDLE_VALUE)
            SetHandleInformation(hStdIn, HANDLE_FLAG_INHERIT, orig_in_flags & HANDLE_FLAG_INHERIT);
        if (hStdOut != INVALID_HANDLE_VALUE)
            SetHandleInformation(hStdOut, HANDLE_FLAG_INHERIT, orig_out_flags & HANDLE_FLAG_INHERIT);
        if (hStdErr != INVALID_HANDLE_VALUE)
            SetHandleInformation(hStdErr, HANDLE_FLAG_INHERIT, orig_err_flags & HANDLE_FLAG_INHERIT);
    }
    if (hNul != INVALID_HANDLE_VALUE)
        CloseHandle(hNul);
    return hResult;
}

// POSIX: int posix_spawnp(pid_t *pid, const char *file,
//                         const posix_spawn_file_actions_t *fa,
//                         const void *attrp, char *const argv[], char *const envp[])
// Returns 0 on success, error code on failure.
// On Windows, *pid is stuffed with the process HANDLE (cast to pid_t / intptr_t).
// This is a cheat — but waitpid() below knows to interpret it as a HANDLE.
static int posix_spawnp(pid_t *pid, const char *file,
                        posix_spawn_file_actions_t *fa,
                        const void *attrp, char **argv, char **envp)
{
    (void)file;  // argv[0] is used by CreateProcess
    (void)attrp; // not used by prism

    HANDLE hp = win32_spawn_with_actions(argv, fa, envp);
    if (hp == INVALID_HANDLE_VALUE)
        return ENOENT; // Return an error code (not zero)

    // Stash the HANDLE as pid_t — waitpid will cast it back (lossless: pid_t = intptr_t)
    *pid = (pid_t)hp;
    return 0;
}

static pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    HANDLE hp = (HANDLE)pid;
    DWORD wait_result = WaitForSingleObject(hp, INFINITE);
    if (wait_result == WAIT_FAILED)
    {
        CloseHandle(hp);
        if (status)
            *status = 1;
        return -1;
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(hp, &exit_code);
    CloseHandle(hp);
    if (status)
        *status = (int)exit_code;
    return pid;
}

// get_self_exe_path: readlink() shim returns -1 on Windows.
// prism.c handles _WIN32 via GetModuleFileNameA directly.

#endif // PRISM_WINDOWS_C

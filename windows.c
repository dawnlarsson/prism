// Prism POSIX shim layer for native Windows (MSVC cl.exe)

#ifndef PRISM_WINDOWS_C
#define PRISM_WINDOWS_C

#define PRISM_DEFAULT_CC "cl"
#define EXE_SUFFIX ".exe"
#define TMPDIR_ENVVAR "TEMP"
#define TMPDIR_ENVVAR_ALT "TMP"
#define TMPDIR_FALLBACK ".\\"
#define FIND_EXE_CMD "where prism.exe 2>nul"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <share.h>
#include <errno.h>
#include <signal.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

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

// MSVC volatile has acquire/release semantics on x86/x64, so plain
// reads/writes are sufficient for signal_temp_* atomics.
#define signal_temp_store(val) (signal_temp_registered = (val))
#define signal_temp_load()     (signal_temp_registered)
#define signal_temps_store(val) (signal_temps_count = (val))
#define signal_temps_load()     (signal_temps_count)
#define signal_temps_cas(expected, desired) \
	(InterlockedCompareExchange((volatile LONG *)&signal_temps_count, \
				    (LONG)(desired), (LONG)(*(expected))) == (LONG)(*(expected)))
// MSVC volatile has acquire/release on x86/x64 — sufficient for cached_clean_env init-once.
#define cached_env_load()       (cached_clean_env)
#define cached_env_store(val)   (cached_clean_env = (val))
#define signal_temps_ready_store(idx, val) (signal_temps_ready[(idx)] = (val))
#define signal_temps_ready_load(idx)       (signal_temps_ready[(idx)])

// MSVC doesn't have these — define them away or provide equivalents.
#define __attribute__(x)
#define __builtin_expect(expr, val) (expr)
#define __builtin_constant_p(x) 0
#define __builtin_strlen(s) strlen(s)
#define __builtin_unreachable() __assume(0)
#define __typeof__(x) const char * // only used for CLI_PUSH's array elem type
#define __alignof__(x) __alignof(x)

#define WIFEXITED(s) 1	   // Always "exited" on Windows (no signals)
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
#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#endif
#ifndef O_RDWR
#define O_RDWR _O_RDWR
#endif
#ifndef O_TRUNC
#define O_TRUNC _O_TRUNC
#endif

#ifndef S_IRUSR
#define S_IRUSR _S_IREAD
#endif
#ifndef S_IWUSR
#define S_IWUSR _S_IWRITE
#endif
#ifndef S_IWGRP
#define S_IWGRP 0
#endif
#ifndef S_IWOTH
#define S_IWOTH 0
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

#ifndef X_OK
#define X_OK 0 // Windows has no execute permission bit; check existence instead
#endif

// POSIX signals not available on Windows
#ifndef SIGPIPE
#define SIGPIPE 13 // define but never raised on Windows
#endif

// sigprocmask stubs — Windows doesn't deliver asynchronous POSIX signals,
// so the TOCTOU window between temp creation and registration is harmless.
// These stubs let make_temp_file_registered compile unchanged.
#ifndef SIG_BLOCK
#define SIG_BLOCK   0
#define SIG_SETMASK 2
typedef unsigned long sigset_t;
static inline int sigemptyset(sigset_t *s) { *s = 0; return 0; }
static inline int sigaddset(sigset_t *s, int sig) { (void)s; (void)sig; return 0; }
static inline int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
	(void)how; (void)set; (void)old; return 0;
}
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
#define dup2 _dup2
#define write _write
#define fstat _fstat
#define strcasecmp _stricmp
#define mkdir(path, mode) _mkdir(path)
#define chmod(path, mode) ((void)(path), (void)(mode), 0)
#define open prism_posix_open_
// Redirect read() calls to our wrapper via macro.
// Using a unique name avoids conflicting with MSVC ucrt's read() declaration.
#define read prism_posix_read_

// POSIX open_memstream returns a FILE* that writes to a growing buffer.
// On fclose, *bufp and *sizep are updated.  Windows has no equivalent,
// so we back the stream with a temp file and intercept fclose to read
// the contents into a malloc'd buffer.
// Thread-local storage for per-thread memstream state.

static PRISM_THREAD_LOCAL FILE *win32_memstream_fp;
static PRISM_THREAD_LOCAL char **win32_memstream_bufp;
static PRISM_THREAD_LOCAL size_t *win32_memstream_sizep;
static PRISM_THREAD_LOCAL char win32_memstream_path[MAX_PATH];

// Grab the real CRT fclose BEFORE we macro-redirect it.
static int (*win32_real_fclose)(FILE *) = fclose;

static FILE *open_memstream(char **bufp, size_t *sizep) {
	char tmpdir[MAX_PATH];
	GetTempPathA(MAX_PATH, tmpdir);
	GetTempFileNameA(tmpdir, "prm", 0, win32_memstream_path);
	FILE *fp = fopen(win32_memstream_path, "w+b");
	if (!fp) return NULL;
	*bufp = NULL;
	*sizep = 0;
	win32_memstream_fp = fp;
	win32_memstream_bufp = bufp;
	win32_memstream_sizep = sizep;
	return fp;
}

static int win32_fclose_wrapper(FILE *fp) {
	if (fp && fp == win32_memstream_fp) {
		fflush(fp);
		long pos = ftell(fp);
		if (pos < 0) pos = 0;
		rewind(fp);
		char *buf = (char *)malloc((size_t)pos + 1);
		if (!buf) {
			win32_real_fclose(fp);
			remove(win32_memstream_path);
			errno = ENOMEM;
			return EOF;
		}
		size_t nread = fread(buf, 1, (size_t)pos, fp);
		buf[nread] = '\0';
		*win32_memstream_bufp = buf;
		*win32_memstream_sizep = nread;
		int ret = win32_real_fclose(fp);
		_unlink(win32_memstream_path);
		win32_memstream_fp = NULL;
		return ret;
	}
	return win32_real_fclose(fp);
}

#define fclose(fp) win32_fclose_wrapper(fp)

// fopen shim: try wide-char API so non-ASCII (UTF-8) paths work.
// Falls back to plain fopen if the path is pure ASCII.
static FILE *(*win32_real_fopen)(const char *, const char *) = fopen;
static FILE *win32_fopen_utf8(const char *path, const char *mode) {
	wchar_t wpath[MAX_PATH], wmode[16];
	int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, MAX_PATH);
	if (wn > 0) {
		MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16);
		FILE *fp = _wfopen(wpath, wmode);
		if (fp) return fp;
	}
	return win32_real_fopen(path, mode);
}
#define fopen(path, mode) win32_fopen_utf8(path, mode)

// stat shim: try wide-char API for non-ASCII paths.
static int win32_stat_utf8(const char *path, struct _stat *st) {
	wchar_t wpath[MAX_PATH];
	int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, MAX_PATH);
	if (wn > 0) {
		int r = _wstat(wpath, (struct _stat *)st);
		if (r == 0) return 0;
	}
	return _stat(path, st);
}
#define stat(path, st) win32_stat_utf8(path, (struct _stat *)(st))

// access shim: try wide-char API for non-ASCII paths.
static int win32_access_utf8(const char *path, int mode) {
	wchar_t wpath[MAX_PATH];
	int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, MAX_PATH);
	if (wn > 0) {
		int r = _waccess(wpath, mode);
		if (r == 0) return 0;
	}
	return _access(path, mode);
}
#undef access
#define access win32_access_utf8

// unlink shim: try wide-char API for non-ASCII paths.
static int win32_unlink_utf8(const char *path) {
	wchar_t wpath[MAX_PATH];
	int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, MAX_PATH);
	if (wn > 0) {
		int r = _wunlink(wpath);
		if (r == 0) return 0;
	}
	return _unlink(path);
}
#undef unlink
#define unlink win32_unlink_utf8

// Convert argv from the system ANSI codepage to UTF-8 using GetCommandLineW.
// This lets Prism handle non-ASCII source paths on Windows.
// Returns a malloc'd argv array (and strings); caller must free.
static void win32_utf8_argv(int *argc_out, char ***argv_out) {
	int wargc;
	LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
	if (!wargv) return;

	char **argv = (char **)calloc((size_t)wargc + 1, sizeof(char *));
	if (!argv) { LocalFree(wargv); return; }

	for (int i = 0; i < wargc; i++) {
		int needed = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
		argv[i] = (char *)malloc((size_t)needed);
		if (argv[i])
			WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], needed, NULL, NULL);
		else
			argv[i] = _strdup("");
	}
	argv[wargc] = NULL;
	LocalFree(wargv);

	*argc_out = wargc;
	*argv_out = argv;
}

#define SPAWN_ACTION_MAX 8

typedef enum { SPAWN_ACT_CLOSE, SPAWN_ACT_DUP2, SPAWN_ACT_OPEN } SpawnActionKind;

typedef struct {
	SpawnActionKind kind;
	int fd;		  // target fd
	int src_fd;	  // source fd (for dup2)
	const char *path; // path (for open)
	int oflag;	  // open flags
	mode_t mode;	  // open mode
} SpawnAction;

typedef struct {
	SpawnAction actions[SPAWN_ACTION_MAX];
	int count;
} posix_spawn_file_actions_t;

static int pipe(int pipefd[2]) {
	return _pipe(pipefd, 65536, _O_BINARY);
}

// POSIX realpath: resolve symlinks/reparse points and canonicalize.
// _fullpath is purely lexical; we must use GetFinalPathNameByHandleA
// to resolve Junctions and Symlinks on NTFS.
static char *realpath(const char *path, char *resolved) {
	if (!path) { errno = EINVAL; return NULL; }
	char *out = resolved ? resolved : (char *)malloc(PATH_MAX);
	if (!out) return NULL;

	HANDLE h = CreateFileA(path, 0, /* no access needed */
			       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			       NULL, OPEN_EXISTING,
			       FILE_FLAG_BACKUP_SEMANTICS, /* required for directories */
			       NULL);
	if (h == INVALID_HANDLE_VALUE) {
		// Fall back to _fullpath for paths that don't yet exist on disk
		char *r = _fullpath(out, path, PATH_MAX);
		if (!r && !resolved) free(out);
		return r;
	}
	DWORD len = GetFinalPathNameByHandleA(h, out, PATH_MAX, FILE_NAME_NORMALIZED);
	CloseHandle(h);
	if (len == 0 || len >= PATH_MAX) {
		if (!resolved) free(out);
		errno = ENAMETOOLONG;
		return NULL;
	}
	// Strip the \\?\\ prefix if present
	if (len >= 4 && out[0] == '\\' && out[1] == '\\' && out[2] == '?' && out[3] == '\\') {
		memmove(out, out + 4, len - 4 + 1);
	}
	return out;
}

// MSVC _read takes unsigned int for count, returns int
static ssize_t prism_posix_read_(int fd, void *buf, size_t count) {
	unsigned int to_read = (count > 0x7FFFFFFF) ? 0x7FFFFFFF : (unsigned int)count;
	return (ssize_t)_read(fd, buf, to_read);
}

// POSIX open() shim: maps /dev/null to NUL and calls _open with _O_BINARY
static int prism_posix_open_(const char *path, int oflag, ...) {
	va_list ap;
	int mode = 0;
	if (oflag & _O_CREAT) {
		va_start(ap, oflag);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	// Map /dev/null -> NUL
	const char *winpath = path;
	if (strcmp(path, "/dev/null") == 0) winpath = "NUL";
	if (oflag & _O_CREAT)
		return _open(winpath, oflag | _O_BINARY, mode);
	return _open(winpath, oflag | _O_BINARY);
}

// POSIX getline() shim: reads a full line into a malloc'd buffer.
static ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
	if (!lineptr || !n || !stream) { errno = EINVAL; return -1; }
	size_t pos = 0;
	int c;
	if (!*lineptr || *n == 0) {
		*n = 128;
		char *tmp = (char *)realloc(*lineptr, *n);
		if (!tmp) return -1;
		*lineptr = tmp;
	}
	while ((c = fgetc(stream)) != EOF) {
		if (pos + 2 > *n) {
			size_t new_n = *n * 2;
			char *tmp = (char *)realloc(*lineptr, new_n);
			if (!tmp) return -1;
			*lineptr = tmp;
			*n = new_n;
		}
		(*lineptr)[pos++] = (char)c;
		if (c == '\n') break;
	}
	if (pos == 0) return -1;
	(*lineptr)[pos] = '\0';
	return (ssize_t)pos;
}

// tries up to 10000 unique names with randomized template + _sopen_s
// suffix_len: number of chars after the X's (e.g. 2 for ".c" in "foo.XXXXXX.c")
static int mkstemps(char *tmpl, int suffix_len) {
	static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	size_t len = strlen(tmpl);
	if (suffix_len < 0) suffix_len = 0;
	if ((size_t)suffix_len >= len) return -1;

	char try_buf[MAX_PATH];
	if (len >= MAX_PATH) {
		errno = ENAMETOOLONG;
		return -1;
	}

	// Find the X's in the template (before the suffix)
	size_t x_end = len - suffix_len;
	size_t x_start = x_end;
	while (x_start > 0 && tmpl[x_start - 1] == 'X') x_start--;
	size_t x_count = x_end - x_start;

	// Seed with PID + high-resolution timer + tick count for better entropy
	// Avoids collisions in parallel build systems (e.g., ninja -j32)
	LARGE_INTEGER perf_counter;
	QueryPerformanceCounter(&perf_counter);
	unsigned int seed = (unsigned int)_getpid() ^ (unsigned int)GetTickCount() ^
			    (unsigned int)perf_counter.LowPart ^ (unsigned int)(perf_counter.LowPart >> 16);

	for (int attempt = 0; attempt < 10000; attempt++) {
		memcpy(try_buf, tmpl, len + 1);
		// Per-attempt seed: mix attempt number to ensure distinct sequences
		// even if multiple processes share the same initial seed
		unsigned int attempt_seed = seed ^ ((unsigned int)attempt * 2654435761u);
		for (size_t i = x_start; i < x_start + x_count; i++) {
			attempt_seed = attempt_seed * 1103515245 + 12345;
			try_buf[i] = chars[(attempt_seed >> 16) % (sizeof(chars) - 1)];
		}
		int fd;
		// Use wide-char API so non-ASCII paths (e.g. Unicode directories) work.
		wchar_t wtry[MAX_PATH];
		int wlen = MultiByteToWideChar(CP_UTF8, 0, try_buf, -1, wtry, MAX_PATH);
		if (wlen <= 0)
			wlen = MultiByteToWideChar(CP_ACP, 0, try_buf, -1, wtry, MAX_PATH);
		if (wlen <= 0) continue;
		errno_t err = _wsopen_s(
		    &fd, wtry, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE);
		if (err == 0 && fd >= 0) {
			memcpy(tmpl, try_buf, len + 1);
			return fd;
		}
	}
	return -1;
}

static int mkstemp(char *tmpl) {
	return mkstemps(tmpl, 0);
}

static char *mkdtemp(char *tmpl) {
	static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	size_t len = strlen(tmpl);
	size_t x_end = len;
	size_t x_start = x_end;
	while (x_start > 0 && tmpl[x_start - 1] == 'X') x_start--;
	size_t x_count = x_end - x_start;

	LARGE_INTEGER perf_counter;
	QueryPerformanceCounter(&perf_counter);
	unsigned int seed = (unsigned int)_getpid() ^ (unsigned int)GetTickCount() ^
	                    (unsigned int)perf_counter.LowPart;

	for (int attempt = 0; attempt < 10000; attempt++) {
		char try_buf[MAX_PATH];
		if (len >= MAX_PATH) { errno = ENAMETOOLONG; return NULL; }
		memcpy(try_buf, tmpl, len + 1);
		unsigned int attempt_seed = seed ^ ((unsigned int)attempt * 2654435761u);
		for (size_t i = x_start; i < x_start + x_count; i++) {
			attempt_seed = attempt_seed * 1103515245 + 12345;
			try_buf[i] = chars[(attempt_seed >> 16) % (sizeof(chars) - 1)];
		}
		if (CreateDirectoryA(try_buf, NULL)) {
			memcpy(tmpl, try_buf, len + 1);
			return tmpl;
		}
		if (GetLastError() != ERROR_ALREADY_EXISTS) return NULL;
	}
	return NULL;
}

/* Stub: Windows callers use get_self_exe_path() via GetModuleFileNameA instead */
static ssize_t readlink(const char *path, char *buf, size_t bufsize) {
	(void)path;
	(void)buf;
	(void)bufsize;
	return -1;
}

static int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa) {
	fa->count = 0;
	return 0;
}

static int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa) {
	for (int i = 0; i < fa->count; i++) {
		// path was strdup'd in addopen
		if (fa->actions[i].kind == SPAWN_ACT_OPEN && fa->actions[i].path)
			free((void *)fa->actions[i].path);
	}
	fa->count = 0;
	return 0;
}

static int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd) {
	if (fa->count >= SPAWN_ACTION_MAX) return -1;
	fa->actions[fa->count++] = (SpawnAction){.kind = SPAWN_ACT_CLOSE, .fd = fd};
	return 0;
}

static int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int src_fd, int target_fd) {
	if (fa->count >= SPAWN_ACTION_MAX) return -1;
	fa->actions[fa->count++] = (SpawnAction){.kind = SPAWN_ACT_DUP2, .fd = target_fd, .src_fd = src_fd};
	return 0;
}

static int posix_spawn_file_actions_addopen(
    posix_spawn_file_actions_t *fa, int fd, const char *path, int oflag, mode_t mode) {
	if (fa->count >= SPAWN_ACTION_MAX) return -1;
	fa->actions[fa->count++] = (SpawnAction){
	    .kind = SPAWN_ACT_OPEN, .fd = fd, .path = _strdup(path), .oflag = oflag, .mode = mode};
	return 0;
}

// Build a command line string from argv, following MSVC/CommandLineToArgvW
// escaping rules: backslashes are literal unless immediately before a double
// quote, in which case they must be doubled. Embedded quotes are escaped as \".
static char *win32_argv_to_cmdline(char **argv) {
	// First pass: compute total length needed
	size_t total = 0;
	for (int i = 0; argv[i]; i++) {
		if (i > 0) total++; // space separator
		const char *s = argv[i];
		int needs_quote = (s[0] == '\0' || strchr(s, ' ') || strchr(s, '\t') || strchr(s, '"'));
		if (needs_quote) total += 2; // opening and closing quotes
		for (const char *c = s; *c; c++) {
			if (*c == '"') total += 2; // \" escape
			else if (*c == '\\') {
				size_t run = 0;
				while (c[run] == '\\') run++;
				if (c[run] == '"' || (c[run] == '\0' && needs_quote))
					total += run * 2; // double backslashes before quote
				else
					total += run; // literal backslashes
				c += run - 1;
			} else
				total++;
		}
	}

	char *cmdline = (char *)malloc(total + 1);
	if (!cmdline) return NULL;
	char *p = cmdline;

	// Second pass: emit quoted/escaped arguments
	for (int i = 0; argv[i]; i++) {
		if (i > 0) *p++ = ' ';
		const char *s = argv[i];
		int needs_quote = (s[0] == '\0' || strchr(s, ' ') || strchr(s, '\t') || strchr(s, '"'));
		if (needs_quote) *p++ = '"';
		for (const char *c = s; *c; c++) {
			if (*c == '\\') {
				size_t run = 0;
				while (c[run] == '\\') run++;
				// Double backslashes only if followed by " or end-of-string inside quotes
				if (c[run] == '"' || (c[run] == '\0' && needs_quote))
					for (size_t j = 0; j < run * 2; j++) *p++ = '\\';
				else
					for (size_t j = 0; j < run; j++) *p++ = '\\';
				c += run - 1;
			} else if (*c == '"') {
				*p++ = '\\';
				*p++ = '"';
			} else
				*p++ = *c;
		}
		if (needs_quote) *p++ = '"';
	}
	*p = '\0';
	return cmdline;
}

// Resolve file actions into STARTUPINFO handle redirections
// Returns HANDLE for the spawned process, or INVALID_HANDLE_VALUE on failure
static HANDLE win32_spawn_with_actions(char **argv, posix_spawn_file_actions_t *fa, char **envp) {
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
	// Track which handles were explicitly redirected by file actions.
	// Only redirected handles need SetHandleInformation — touching
	// process-global standard handles is a thread-safety hazard in
	// PRISM_LIB_MODE (concurrent prism_transpile_file calls).
	bool redirected_in = false, redirected_out = false, redirected_err = false;

	if (fa) {
		si.dwFlags |= STARTF_USESTDHANDLES;
		for (int i = 0; i < fa->count; i++) {
			SpawnAction *a = &fa->actions[i];
			if (a->kind == SPAWN_ACT_DUP2) {
				// Convert C fd to Win32 HANDLE
				HANDLE h = (HANDLE)_get_osfhandle(a->src_fd);
				if (a->fd == STDOUT_FILENO) { hStdOut = h; redirected_out = true; }
				else if (a->fd == STDIN_FILENO)
					{ hStdIn = h; redirected_in = true; }
				else if (a->fd == STDERR_FILENO)
					{ hStdErr = h; redirected_err = true; }
			} else if (a->kind == SPAWN_ACT_OPEN) {
				// Map /dev/null → NUL
				const char *winpath = a->path;
				if (strcmp(winpath, "/dev/null") == 0) winpath = "NUL";

				// Translate POSIX oflag to Win32 access/disposition
				DWORD access_flags = 0;
				DWORD disposition = OPEN_EXISTING;
				if ((a->oflag & O_RDWR) == O_RDWR)
					access_flags = GENERIC_READ | GENERIC_WRITE;
				else if (a->oflag & O_WRONLY)
					access_flags = GENERIC_WRITE;
				else
					access_flags = GENERIC_READ;
				if (a->oflag & O_CREAT) {
					if (a->oflag & O_EXCL)
						disposition = CREATE_NEW;
					else if (a->oflag & O_TRUNC)
						disposition = CREATE_ALWAYS;
					else
						disposition = OPEN_ALWAYS;
				} else if (a->oflag & O_TRUNC)
					disposition = TRUNCATE_EXISTING;

				SECURITY_ATTRIBUTES sa_nul = {sizeof(sa_nul), NULL, TRUE};
				hNul = CreateFileA(winpath,
						   access_flags,
						   FILE_SHARE_READ | FILE_SHARE_WRITE,
						   &sa_nul,
						   disposition,
						   0,
						   NULL);
				if (hNul == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
				if (a->fd == STDERR_FILENO) { hStdErr = hNul; redirected_err = true; }
				else if (a->fd == STDOUT_FILENO)
					{ hStdOut = hNul; redirected_out = true; }
				else if (a->fd == STDIN_FILENO)
					{ hStdIn = hNul; redirected_in = true; }
			}
			// SPAWN_ACT_CLOSE: handled by the caller manually, not by this function
		}
		si.hStdInput = hStdIn;
		si.hStdOutput = hStdOut;
		si.hStdError = hStdErr;
	}

	// Make redirected handles inheritable, saving original flags for cleanup.
	// Only touch handles explicitly set by file actions — process-global
	// standard handles must not be modified (thread-safety).
	DWORD orig_in_flags = 0, orig_out_flags = 0, orig_err_flags = 0;
	if (si.dwFlags & STARTF_USESTDHANDLES) {
		if (redirected_in && hStdIn != INVALID_HANDLE_VALUE) {
			GetHandleInformation(hStdIn, &orig_in_flags);
			SetHandleInformation(hStdIn, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
		}
		if (redirected_out && hStdOut != INVALID_HANDLE_VALUE) {
			GetHandleInformation(hStdOut, &orig_out_flags);
			SetHandleInformation(hStdOut, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
		}
		if (redirected_err && hStdErr != INVALID_HANDLE_VALUE) {
			GetHandleInformation(hStdErr, &orig_err_flags);
			SetHandleInformation(hStdErr, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
		}
	}

	HANDLE hResult = INVALID_HANDLE_VALUE;
	char *cmdline = win32_argv_to_cmdline(argv);
	BOOL ok = FALSE;
	if (!cmdline) goto cleanup;

	// Use CreateProcessW so UTF-8 paths (converted by win32_utf8_argv) work.
	{
		int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, NULL, 0);
		wchar_t *wcmdline = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
		if (wcmdline) {
			MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, wcmdline, wlen);
			STARTUPINFOW siw;
			memset(&siw, 0, sizeof(siw));
			siw.cb = sizeof(siw);
			siw.dwFlags = si.dwFlags;
			siw.hStdInput = si.hStdInput;
			siw.hStdOutput = si.hStdOutput;
			siw.hStdError = si.hStdError;
			ok = CreateProcessW(NULL, wcmdline, NULL, NULL, TRUE, 0, NULL, NULL, &siw, &pi);
			free(wcmdline);
		}
	}
	free(cmdline);

	if (!ok) fprintf(stderr, "CreateProcess failed for '%s': error %lu\n", argv[0], GetLastError());
	else {
		CloseHandle(pi.hThread);
		hResult = pi.hProcess;
	}

cleanup:
	// Restore original handle inheritance flags (only for redirected handles)
	if (si.dwFlags & STARTF_USESTDHANDLES) {
		if (redirected_in && hStdIn != INVALID_HANDLE_VALUE)
			SetHandleInformation(
			    hStdIn, HANDLE_FLAG_INHERIT, orig_in_flags & HANDLE_FLAG_INHERIT);
		if (redirected_out && hStdOut != INVALID_HANDLE_VALUE)
			SetHandleInformation(
			    hStdOut, HANDLE_FLAG_INHERIT, orig_out_flags & HANDLE_FLAG_INHERIT);
		if (redirected_err && hStdErr != INVALID_HANDLE_VALUE)
			SetHandleInformation(
			    hStdErr, HANDLE_FLAG_INHERIT, orig_err_flags & HANDLE_FLAG_INHERIT);
	}
	if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
	return hResult;
}

// POSIX: int posix_spawnp(pid_t *pid, const char *file,
//                         const posix_spawn_file_actions_t *fa,
//                         const void *attrp, char *const argv[], char *const envp[])
// Returns 0 on success, error code on failure.
// On Windows, *pid is stuffed with the process HANDLE (cast to pid_t / intptr_t).
// This is a cheat — but waitpid() below knows to interpret it as a HANDLE.
static int posix_spawnp(pid_t *pid,
			const char *file,
			posix_spawn_file_actions_t *fa,
			const void *attrp,
			char **argv,
			char **envp) {
	(void)file;  // argv[0] is used by CreateProcess
	(void)attrp; // not used by prism

	HANDLE hp = win32_spawn_with_actions(argv, fa, envp);
	if (hp == INVALID_HANDLE_VALUE) return ENOENT; // Return an error code (not zero)

	// Stash the HANDLE as pid_t — waitpid will cast it back (lossless: pid_t = intptr_t)
	*pid = (pid_t)hp;
	return 0;
}

static pid_t waitpid(pid_t pid, int *status, int options) {
	(void)options;
	HANDLE hp = (HANDLE)pid;
	DWORD wait_result = WaitForSingleObject(hp, INFINITE);
	if (wait_result == WAIT_FAILED) {
		CloseHandle(hp);
		if (status) *status = 1;
		return -1;
	}
	DWORD exit_code = 1;
	GetExitCodeProcess(hp, &exit_code);
	CloseHandle(hp);
	if (status) *status = (int)exit_code;
	return pid;
}
// Resolve the path to the currently running executable
static bool get_self_exe_path(char *buf, size_t bufsize) {
	DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)bufsize);
	return (len > 0 && len < bufsize);
}

// Detect whether the compiler is MSVC cl.exe
// Handles quoted paths like "C:\Program Files\...\cl.exe" /nologo
static bool cc_is_msvc(const char *cc) {
	if (!cc || !*cc) return false;
	while (*cc == ' ') cc++;
	// Find the end of the command token (before args), respecting quotes
	const char *cmd_start = cc;
	const char *cmd_end;
	if (*cc == '"') {
		cmd_start = cc + 1; // skip opening quote
		cmd_end = strchr(cmd_start, '"');
		if (!cmd_end) cmd_end = cmd_start + strlen(cmd_start);
	} else {
		cmd_end = cc;
		while (*cmd_end && *cmd_end != ' ' && *cmd_end != '\t') cmd_end++;
	}
	// Find last path separator within command only
	const char *base = cmd_start;
	for (const char *p = cmd_start; p < cmd_end; p++)
		if (*p == '/' || *p == '\\') base = p + 1;
	size_t len = (size_t)(cmd_end - base);
	return ((len == 2 && _strnicmp(base, "cl", 2) == 0) ||
	        (len == 6 && _strnicmp(base, "cl.exe", 6) == 0));
}

// Run a command and wait for it to complete.
// Returns exit status, or -1 on error.
// Uses CreateProcess with proper quoting so paths with spaces work.
static int run_command(char **argv) {
	HANDLE hp = win32_spawn_with_actions(argv, NULL, NULL);
	if (hp == INVALID_HANDLE_VALUE) return -1;
	WaitForSingleObject(hp, INFINITE);
	DWORD exit_code = 1;
	GetExitCodeProcess(hp, &exit_code);
	CloseHandle(hp);
	return (int)exit_code;
}

// Like run_command but suppresses stderr (for probe attempts).
// Uses posix_spawn_file_actions to redirect stderr only in the child,
// avoiding process-global fd mutation (thread-safe for PRISM_LIB_MODE).
static int run_command_quiet(char **argv) {
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

	HANDLE hp = win32_spawn_with_actions(argv, &fa, NULL);
	posix_spawn_file_actions_destroy(&fa);
	if (hp == INVALID_HANDLE_VALUE) return -1;

	WaitForSingleObject(hp, INFINITE);
	DWORD exit_code = 1;
	GetExitCodeProcess(hp, &exit_code);
	CloseHandle(hp);
	return (int)exit_code;
}

// Capture the first line of a command's stdout. Returns 0 on success.
// Uses CreateProcess + pipe instead of _popen to avoid cmd.exe injection.
static int capture_first_line(char **argv, char *buf, size_t bufsize) {
	buf[0] = '\0';

	// Build a properly escaped command line (no cmd.exe shell involved)
	char *cmdline = win32_argv_to_cmdline(argv);
	if (!cmdline) return -1;

	// Create a pipe for the child's stdout
	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
	HANDLE hReadPipe, hWritePipe;
	if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
		free(cmdline);
		return -1;
	}
	// The read end should not be inherited by the child
	SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

	// Open NUL for stderr suppression
	SECURITY_ATTRIBUTES sa_nul = {sizeof(sa_nul), NULL, TRUE};
	HANDLE hNul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
				   &sa_nul, OPEN_EXISTING, 0, NULL);

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = hWritePipe;
	si.hStdError = (hNul != INVALID_HANDLE_VALUE) ? hNul : GetStdHandle(STD_ERROR_HANDLE);
	memset(&pi, 0, sizeof(pi));

	BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
	free(cmdline);
	CloseHandle(hWritePipe); // Close write end in parent so reads will EOF
	if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

	if (!ok) {
		CloseHandle(hReadPipe);
		return -1;
	}

	// Read the first line from the pipe
	size_t pos = 0;
	char ch;
	DWORD nread;
	while (pos + 1 < bufsize && ReadFile(hReadPipe, &ch, 1, &nread, NULL) && nread == 1) {
		if (ch == '\n') break;
		if (ch != '\r') buf[pos++] = ch;
	}
	buf[pos] = '\0';
	CloseHandle(hReadPipe);

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD exit_code = 1;
	GetExitCodeProcess(pi.hProcess, &exit_code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	if (buf[0]) return 0;
	return (exit_code != 0) ? (int)exit_code : -1;
}

// Get the platform install path: %LOCALAPPDATA%\prism\prism.exe
static const char *get_install_path(void) {
	static PRISM_THREAD_LOCAL char path[MAX_PATH];
	if (path[0]) return path;
	DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH);
	if (len == 0 || len >= MAX_PATH - 20) {
		// Fallback: install next to the running executable
		if (GetModuleFileNameA(NULL, path, MAX_PATH)) return path;
		strcpy(path, "prism.exe");
		return path;
	}
	// Append \prism\prism.exe
	strcat(path, "\\prism\\prism.exe");
	return path;
}

// Ensure the install directory exists (create %LOCALAPPDATA%\prism)
static bool ensure_install_dir(const char *install_path) {
	// Extract directory from install_path
	char dir[MAX_PATH];
	strncpy(dir, install_path, MAX_PATH - 1);
	dir[MAX_PATH - 1] = '\0';
	char *last_sep = strrchr(dir, '\\');
	if (!last_sep) last_sep = strrchr(dir, '/');
	if (last_sep) *last_sep = '\0';
	else
		return true; // No directory component

	DWORD attr = GetFileAttributesA(dir);
	if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
		return true; // Already exists

	return CreateDirectoryA(dir, NULL) != 0;
}

// Check if 'dir' appears as a complete semicolon-delimited segment in 'path'.
static bool path_contains_dir(const char *path, const char *dir) {
	if (!path || !dir) return false;
	size_t dir_len = strlen(dir);
	if (dir_len == 0) return false;
	for (const char *p = path; ; ) {
		const char *found = strstr(p, dir);
		if (!found) return false;
		bool at_start = (found == path || found[-1] == ';');
		bool at_end = (found[dir_len] == '\0' || found[dir_len] == ';');
		if (at_start && at_end) return true;
		p = found + 1;
	}
}

// Add a directory to the user's PATH via the registry (persistent)
static void add_to_user_path(const char *dir) {
	// Check if dir is already in PATH
	char *path_env = getenv("PATH");
	if (path_contains_dir(path_env, dir)) return; // Already in PATH

	// Read current user PATH from registry
	HKEY hKey;
	if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0, KEY_READ | KEY_WRITE, &hKey) != ERROR_SUCCESS)
		return;

	// Query size first, then allocate dynamically (PATH can be up to 32767 bytes)
	DWORD size = 0;
	DWORD type = REG_EXPAND_SZ;
	LONG rc = RegQueryValueExA(hKey, "Path", NULL, &type, NULL, &size);
	char *current = NULL;
	if (rc == ERROR_SUCCESS && size > 0) {
		current = (char *)malloc((size_t)size + 1);
		if (!current) { RegCloseKey(hKey); return; }
		rc = RegQueryValueExA(hKey, "Path", NULL, &type, (BYTE *)current, &size);
		if (rc != ERROR_SUCCESS) { free(current); RegCloseKey(hKey); return; }
		current[size] = '\0';
	} else if (rc == ERROR_FILE_NOT_FOUND) {
		current = (char *)calloc(1, 1); // empty string
		if (!current) { RegCloseKey(hKey); return; }
	} else {
		// Genuine error (access denied, etc.) — abort, do NOT wipe PATH
		RegCloseKey(hKey);
		return;
	}

	// Check again in registry value
	if (path_contains_dir(current, dir)) { free(current); RegCloseKey(hKey); return; }

	// Append ;dir
	size_t cur_len = strlen(current);
	size_t dir_len = strlen(dir);
	size_t new_len = cur_len + 1 + dir_len + 1; // ";" + dir + NUL
	char *newpath = (char *)malloc(new_len);
	if (!newpath) { free(current); RegCloseKey(hKey); return; }
	if (cur_len > 0)
		snprintf(newpath, new_len, "%s;%s", current, dir);
	else
		snprintf(newpath, new_len, "%s", dir);
	free(current);

	RegSetValueExA(hKey, "Path", 0, REG_EXPAND_SZ, (const BYTE *)newpath, (DWORD)(strlen(newpath) + 1));
	free(newpath);
	RegCloseKey(hKey);

	// Notify other programs of the environment change
	SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)"Environment",
			    SMTO_ABORTIFHUNG, 5000, NULL);

	fprintf(stderr,
		"[prism] Added '%s' to your PATH (restart your terminal to use 'prism' directly).\n",
		dir);
}

#endif // PRISM_WINDOWS_C

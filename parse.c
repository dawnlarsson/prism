// parse.c - C tokenizer and preprocessor extracted from chibicc
// Original: https://github.com/rui314/chibicc (MIT License)
//
// This provides:
//   - tokenize_file(path) -> Token list
//   - preprocess(tok) -> Token list (macro-expanded)
//
// Build: Include this file before your main code, or compile separately
// Regen: This is a one-time extraction, modify as needed
// Public API Summary:
//
// Setup:
//   pp_init()                    - Initialize built-in macros
//   pp_add_include_path(path)    - Add include search path
//   pp_add_default_include_paths() - Add /usr/include, /usr/local/include
//   pp_define_macro(name, val)   - Define a macro like -D
//
// Process:
//   Token *tok = tokenize_file(path)  - Tokenize a file
//   tok = preprocess(tok)              - Expand macros, process #include etc.
//
// iterate tok->next until tok->kind == TK_EOF

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>

#ifndef _WIN32
#include <sys/wait.h>
#else
#include <process.h>
#endif

#ifdef __APPLE__
#include <sys/syslimits.h>
#endif

// Fallback for PATH_MAX (should rarely be needed)
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef __GNUC__
#define __attribute__(x)
#endif

// Maximum recursion depth for preprocessor expression evaluation
#define FORMAT_MAX_SIZE (16 * 1024 * 1024) // 16 MB
#define PP_MAX_EVAL_DEPTH 1000
#define TOMBSTONE ((void *)-1)
#define unreachable() error("internal error at %s:%d", __FILE__, __LINE__)

// Include paths for preprocessor
static char **pp_include_paths = NULL;
static int pp_include_paths_count = 0;
static int pp_user_paths_start = 0; // Index where user-specified paths begin
static int pp_eval_depth = 0;

// Compiler flags that affect macro/path discovery (set before pp_init)
static const char *pp_compiler = NULL;  // Compiler to use (default: "cc")
static char **pp_compiler_flags = NULL; // Flags like -std=c99, -m32, etc.
static int pp_compiler_flags_count = 0;

typedef struct Token Token;
typedef struct Hideset Hideset;

// String array utility
typedef struct
{
    char **data;
    int capacity;
    int len;
} StringArray;

// File info
typedef struct
{
    char *name;
    int file_no;
    char *contents;
    char *display_name;
    int line_delta;
    int *line_offsets; // Array of byte offsets where each line starts
    int line_count;    // Number of lines (size of line_offsets array)
} File;

// Token kinds (fits in 4 bits, but we use 8 for alignment)
typedef enum
{
    TK_IDENT,   // Identifiers
    TK_PUNCT,   // Punctuators
    TK_KEYWORD, // Keywords
    TK_STR,     // String literals
    TK_NUM,     // Numeric literals
    TK_PP_NUM,  // Preprocessing numbers
    TK_EOF,     // End-of-file markers
} TokenKind;

// Token flags (packed into 8 bits)
enum
{
    TF_AT_BOL = 1 << 0,      // Token is at beginning of line
    TF_HAS_SPACE = 1 << 1,   // Token preceded by whitespace
    TF_IS_FLOAT = 1 << 2,    // For TK_NUM: value was a floating point literal
    TF_FOREIGN_LOC = 1 << 3, // Token loc doesn't belong to tok_file() contents
};

struct Token
{
    char *loc;   // 8 bytes: pointer into source buffer
    Token *next; // 8 bytes: next token in list (kept for compatibility)
    union
    {
        int64_t i64;   // Integer value for TK_NUM
        char *str;     // String content for TK_STR (heap allocated)
    } val;             // 8 bytes: payload
    uint32_t len;      // 4 bytes: token length
    uint8_t kind;      // 1 byte: TokenKind
    uint8_t flags;     // 1 byte: TF_* flags
    uint16_t file_idx; // 2 bytes: index into input_files array
};
// Total: 32 bytes (with alignment), but much smaller than 128+ bytes before

static inline bool tok_at_bol(Token *tok);
static inline bool tok_has_space(Token *tok);
static inline void tok_set_at_bol(Token *tok, bool v);
static inline void tok_set_has_space(Token *tok, bool v);
static bool is_user_header_path(const char *path);
static bool header_contains_include_next(Token *tok);

// Arena Allocator for Tokens
// Allocates tokens in large blocks to avoid per-token malloc overhead

#define ARENA_BLOCK_SIZE 4096 // Tokens per block

typedef struct ArenaBlock ArenaBlock;
struct ArenaBlock
{
    ArenaBlock *next;
    int used;
    Token tokens[ARENA_BLOCK_SIZE];
};

typedef struct
{
    ArenaBlock *head;    // First block (for freeing)
    ArenaBlock *current; // Current block for allocation
} TokenArena;

static TokenArena token_arena = {0};

static Token *arena_alloc_token(void)
{
    if (!token_arena.current || token_arena.current->used >= ARENA_BLOCK_SIZE)
    {
        // Allocate new block
        ArenaBlock *block = malloc(sizeof(ArenaBlock));
        if (!block)
        {
            fprintf(stderr, "out of memory allocating token arena block\n");
            exit(1);
        }
        block->next = NULL;
        block->used = 0;

        if (token_arena.current)
        {
            token_arena.current->next = block;
        }
        else
        {
            token_arena.head = block;
        }
        token_arena.current = block;
    }

    Token *tok = &token_arena.current->tokens[token_arena.current->used++];
    memset(tok, 0, sizeof(Token));
    return tok;
}

// Reset arena for reuse (keeps memory allocated)
static void arena_reset(void)
{
    for (ArenaBlock *b = token_arena.head; b; b = b->next)
    {
        b->used = 0;
    }
    token_arena.current = token_arena.head;
}

// Free all arena memory
static void arena_free(void)
{
    ArenaBlock *b = token_arena.head;
    while (b)
    {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    token_arena.head = NULL;
    token_arena.current = NULL;
}

// Hashmap
typedef struct
{
    char *key;
    int keylen;
    void *val;
} HashEntry;

typedef struct
{
    HashEntry *buckets;
    int capacity;
    int used;
} HashMap;

// Side tables for rarely-used token data (indexed by token pointer)
static HashMap token_hidesets;
static HashMap token_origins;

// Error handling
static File *current_file;

// Tokenizer
static File **input_files;
static int input_file_count;
static bool at_bol;
static bool has_space;

// Free a File structure and all its allocated memory
static void free_file(File *f)
{
    if (!f)
        return;
    // f->contents is allocated by open_memstream in read_file()
    if (f->contents)
        free(f->contents);
    // f->line_offsets is allocated by new_file()
    if (f->line_offsets)
        free(f->line_offsets);
    // f->name is strdup'd in new_file() - we own it
    // f->display_name points to same memory as f->name
    if (f->name)
        free(f->name);
    free(f);
}

// Free all input files
static void free_input_files(void)
{
    if (!input_files)
        return;
    for (int i = 0; i < input_file_count; i++)
    {
        free_file(input_files[i]);
    }
    free(input_files);
    input_files = NULL;
    input_file_count = 0;
}

// Preprocessor
typedef struct MacroParam MacroParam;
struct MacroParam
{
    MacroParam *next;
    char *name;
};

typedef struct MacroArg MacroArg;
struct MacroArg
{
    MacroArg *next;
    char *name;
    int name_len;       // Cached length for fast comparison
    uint32_t name_hash; // Cached hash for fast comparison
    bool is_va_args;
    Token *tok;
};

typedef Token *macro_handler_fn(Token *);

typedef struct Macro Macro;
struct Macro
{
    char *name;
    bool is_objlike;
    bool is_virtual; // Defined in a virtual include - tokens should inherit expansion site location
    MacroParam *params;
    char *va_args_name;
    Token *body;
    macro_handler_fn *handler;
};

typedef struct CondIncl CondIncl;
struct CondIncl
{
    CondIncl *next;
    enum
    {
        IN_THEN,
        IN_ELIF,
        IN_ELSE
    } ctx;
    Token *tok;
    bool included;
};

static HashMap macros;
static CondIncl *cond_incl;
static HashMap pragma_once_files;       // Files with #pragma once
static HashMap guarded_files;           // Files we've fully included (guard macro now defined)
static HashMap virtual_includes;        // Files processed in virtual mode (macros learned, not inlined)
static bool in_virtual_include = false; // true when processing a virtual include file
static bool in_pp_const_expr = false;   // true when evaluating #if/#elif constant expressions
static bool pp_eval_skip = false;

// Hash set for fast feature test macro lookup
static HashMap feature_test_macro_set;

// Forward declaration for hashmap_get2 (needed before is_feature_test_macro)
static void *hashmap_get2(HashMap *map, char *key, int keylen);

// Feature test macros that need to be passed through to the backend compiler
// These must be defined before system headers for glibc to expose certain functions
static const char *feature_test_macro_names[] = {
    "_GNU_SOURCE",
    "_POSIX_SOURCE",
    "_POSIX_C_SOURCE",
    "_XOPEN_SOURCE",
    "_XOPEN_SOURCE_EXTENDED",
    "_ISOC99_SOURCE",
    "_ISOC11_SOURCE",
    "_ISOC2X_SOURCE",
    "_LARGEFILE_SOURCE",
    "_LARGEFILE64_SOURCE",
    "_FILE_OFFSET_BITS",
    "_BSD_SOURCE",
    "_SVID_SOURCE",
    "_DEFAULT_SOURCE",
    "_ATFILE_SOURCE",
    "_REENTRANT",
    "_THREAD_SAFE",
    "__STDC_WANT_LIB_EXT1__",
    "__STDC_WANT_IEC_60559_BFP_EXT__",
    "__STDC_WANT_IEC_60559_DFP_EXT__",
    "__STDC_WANT_IEC_60559_FUNCS_EXT__",
    "__STDC_WANT_IEC_60559_TYPES_EXT__",
    NULL};

// Storage for feature test macros found in user code
typedef struct
{
    char *name;
    char *value; // The macro value (or "1" if no value)
} FeatureTestMacro;

#define MAX_FEATURE_TEST_MACROS 32
static FeatureTestMacro feature_test_macros[MAX_FEATURE_TEST_MACROS];
static int feature_test_macro_count = 0;

static bool is_feature_test_macro(const char *name, int len)
{
    return hashmap_get2(&feature_test_macro_set, (char *)name, len) != NULL;
}

static void record_feature_test_macro(const char *name, int name_len, Token *body)
{
    if (feature_test_macro_count >= MAX_FEATURE_TEST_MACROS)
        return;

    // Check if already recorded
    for (int i = 0; i < feature_test_macro_count; i++)
    {
        if (strlen(feature_test_macros[i].name) == (size_t)name_len &&
            strncmp(feature_test_macros[i].name, name, name_len) == 0)
            return; // Already recorded
    }

    // Get the value from the body tokens
    char value_buf[256] = "";
    int pos = 0;
    for (Token *t = body; t && t->kind != TK_EOF && pos < 250; t = t->next)
    {
        if (tok_has_space(t) && pos > 0)
            value_buf[pos++] = ' ';
        int copy_len = t->len;
        if (pos + copy_len >= 250)
            copy_len = 250 - pos;
        memcpy(value_buf + pos, t->loc, copy_len);
        pos += copy_len;
    }
    value_buf[pos] = '\0';

    // If empty, default to "1"
    if (pos == 0)
        strcpy(value_buf, "1");

    feature_test_macros[feature_test_macro_count].name = strndup(name, name_len);
    feature_test_macros[feature_test_macro_count].value = strdup(value_buf);
    feature_test_macro_count++;
}

// Public API: get feature test macros for emission
int pp_get_feature_test_macros(const char ***names, const char ***values)
{
    static const char *name_ptrs[MAX_FEATURE_TEST_MACROS];
    static const char *value_ptrs[MAX_FEATURE_TEST_MACROS];

    for (int i = 0; i < feature_test_macro_count; i++)
    {
        name_ptrs[i] = feature_test_macros[i].name;
        value_ptrs[i] = feature_test_macros[i].value;
    }

    *names = name_ptrs;
    *values = value_ptrs;
    return feature_test_macro_count;
}

static void reset_feature_test_macros(void)
{
    for (int i = 0; i < feature_test_macro_count; i++)
    {
        free(feature_test_macros[i].name);
        free(feature_test_macros[i].value);
        feature_test_macros[i].name = NULL;
        feature_test_macros[i].value = NULL;
    }
    feature_test_macro_count = 0;
}

// Hideset for macro expansion (with pre-computed hash for fast lookup)
struct Hideset
{
    Hideset *next;
    char *name;
    int len;       // Length of name (avoid strlen)
    uint32_t hash; // Pre-computed hash for fast comparison
};

// Set compiler and flags for system macro/path discovery
// Call this before pp_init() to affect how system macros are queried
void pp_set_compiler(const char *compiler)
{
    pp_compiler = compiler;
}

void pp_add_compiler_flag(const char *flag)
{
    char **new_flags = realloc(pp_compiler_flags, sizeof(char *) * (pp_compiler_flags_count + 1));
    if (!new_flags)
    {
        fprintf(stderr, "out of memory adding compiler flag\n");
        exit(1);
    }
    pp_compiler_flags = new_flags;
    pp_compiler_flags[pp_compiler_flags_count++] = strdup(flag);
}

// Build a command string with the configured compiler and flags
// Note: We detect if the compiler is "prism" and use "cc" instead to avoid
// infinite recursion when prism itself is used as CC
// Find a real C compiler that isn't prism
static const char *find_real_cc(void)
{
    static const char *candidates[] = {
        "/usr/bin/gcc",
        "/usr/bin/clang",
        "/usr/bin/cc",
        "/bin/gcc",
        "/bin/clang",
        "/bin/cc",
        "gcc",
        "clang",
        NULL};

    struct stat st;
    for (int i = 0; candidates[i]; i++)
    {
        if (candidates[i][0] == '/')
        {
            // Absolute path - check it exists and isn't prism
            if (stat(candidates[i], &st) == 0)
            {
                // Read symlink to make sure it's not prism
                char resolved[PATH_MAX];
                if (realpath(candidates[i], resolved))
                {
                    const char *base = strrchr(resolved, '/');
                    base = base ? base + 1 : resolved;
                    if (strcmp(base, "prism") != 0 && strcmp(base, "prism.exe") != 0)
                        return candidates[i];
                }
                else
                {
                    return candidates[i]; // Can't resolve, assume it's fine
                }
            }
        }
        else
        {
            // Relative - just return it, will be found in PATH
            return candidates[i];
        }
    }
    return "cc"; // Fallback
}

static char *build_compiler_cmd(const char *base_cmd)
{
    const char *cc = pp_compiler ? pp_compiler : "cc";

    // Avoid infinite recursion: if compiler is prism or CC env is prism, use a real compiler
    const char *base = strrchr(cc, '/');
    base = base ? base + 1 : cc;
    int is_prism = (strcmp(base, "prism") == 0 || strcmp(base, "prism.exe") == 0);

    const char *env_cc = getenv("CC");
    if (env_cc)
    {
        const char *env_base = strrchr(env_cc, '/');
        env_base = env_base ? env_base + 1 : env_cc;
        if (strcmp(env_base, "prism") == 0 || strcmp(env_base, "prism.exe") == 0)
            is_prism = 1;
    }

    if (is_prism)
        cc = find_real_cc();

    // Calculate total length needed
    size_t len = strlen(cc) + 1 + strlen(base_cmd) + 1;
    // Always prepend CC= to override any env variable
    len += 4;
    for (int i = 0; i < pp_compiler_flags_count; i++)
        len += strlen(pp_compiler_flags[i]) + 1;

    char *cmd = malloc(len + 64); // Extra space for safety
    if (!cmd)
        return NULL;

    // Build: "CC= cc [flags...] base_cmd"
    // Always prepend CC= to ensure we don't pick up prism from environment
    char *p = cmd;
    p += sprintf(p, "CC= %s", cc);
    for (int i = 0; i < pp_compiler_flags_count; i++)
        p += sprintf(p, " %s", pp_compiler_flags[i]);
    sprintf(p, " %s", base_cmd);

    return cmd;
}

static void pp_add_include_path(const char *path)
{
    // Resolve to canonical path for comparison
    char *real = realpath(path, NULL);
    const char *canonical = real ? real : path;

    // Check if this path is already in the list (avoid duplicates)
    // This is important because if -I/usr/include is passed, we don't want
    // to add it again as a "user" path - it should remain a system path
    for (int i = 0; i < pp_include_paths_count; i++)
    {
        char *existing_real = realpath(pp_include_paths[i], NULL);
        const char *existing = existing_real ? existing_real : pp_include_paths[i];
        bool same = (strcmp(canonical, existing) == 0);
        if (existing_real)
            free(existing_real);
        if (same)
        {
            if (real)
                free(real);
            return; // Path already exists, skip adding
        }
    }

    char **new_paths = realloc(pp_include_paths, sizeof(char *) * (pp_include_paths_count + 1));
    if (!new_paths)
    {
        if (real)
            free(real);
        fprintf(stderr, "out of memory adding include path\n");
        exit(1);
    }
    pp_include_paths = new_paths;
    char *dup = strdup(path);
    if (!dup)
    {
        if (real)
            free(real);
        fprintf(stderr, "out of memory duplicating include path\n");
        exit(1);
    }
    pp_include_paths[pp_include_paths_count++] = dup;
    if (real)
        free(real);
}

// Helper to find GCC include path by scanning directories
static char *find_gcc_include_path(const char *base_dir, const char *triple)
{
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", base_dir, triple);

    DIR *dir = opendir(dir_path);
    if (!dir)
        return NULL;

    char *best_version = NULL;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
        if (ent->d_name[0] == '.')
            continue;
        // Check if it's a version directory with include subdir
        char include_path[PATH_MAX];
        snprintf(include_path, sizeof(include_path), "%s/%s/include", dir_path, ent->d_name);
        struct stat st;
        if (stat(include_path, &st) == 0 && S_ISDIR(st.st_mode))
        {
            // Keep the highest version (simple string compare works for version numbers)
            if (!best_version || strcmp(ent->d_name, best_version) > 0)
            {
                free(best_version);
                best_version = strdup(ent->d_name);
                if (!best_version)
                {
                    closedir(dir);
                    return NULL;
                }
            }
        }
    }
    closedir(dir);

    if (best_version)
    {
        char *result = malloc(PATH_MAX);
        if (!result)
        {
            free(best_version);
            return NULL;
        }
        snprintf(result, PATH_MAX, "%s/%s/include", dir_path, best_version);
        free(best_version);
        return result;
    }
    return NULL;
}

static void pp_add_default_include_paths(void)
{
    struct stat st;
    const char *cc = pp_compiler ? pp_compiler : "cc";

    // Avoid infinite recursion: if compiler is prism or CC env is prism, use a real compiler
    const char *base = strrchr(cc, '/');
    base = base ? base + 1 : cc;
    int is_prism = (strcmp(base, "prism") == 0 || strcmp(base, "prism.exe") == 0);

    const char *env_cc = getenv("CC");
    if (env_cc)
    {
        const char *env_base = strrchr(env_cc, '/');
        env_base = env_base ? env_base + 1 : env_cc;
        if (strcmp(env_base, "prism") == 0 || strcmp(env_base, "prism.exe") == 0)
            is_prism = 1;
    }

    if (is_prism)
        cc = find_real_cc();

#ifdef __APPLE__
    // macOS: Get Clang resource dir for built-in headers (stdarg.h, etc.)
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "CC= %s -print-resource-dir 2>/dev/null", cc);
    FILE *fp = popen(cmd, "r");
    if (fp)
    {
        char clang_dir[PATH_MAX];
        if (fgets(clang_dir, sizeof(clang_dir), fp))
        {
            size_t len = strlen(clang_dir);
            if (len > 0 && clang_dir[len - 1] == '\n')
                clang_dir[len - 1] = '\0';

            char include_path[PATH_MAX];
            snprintf(include_path, PATH_MAX, "%s/include", clang_dir);
            if (stat(include_path, &st) == 0)
                pp_add_include_path(include_path);
        }
        pclose(fp);
    }

    // macOS: Get SDK path from xcrun
    fp = popen("xcrun --show-sdk-path 2>/dev/null", "r");
    if (fp)
    {
        char sdk_path[PATH_MAX];
        if (fgets(sdk_path, sizeof(sdk_path), fp))
        {
            // Remove trailing newline
            size_t len = strlen(sdk_path);
            if (len > 0 && sdk_path[len - 1] == '\n')
                sdk_path[len - 1] = '\0';

            // Add SDK include path
            char include_path[PATH_MAX];
            snprintf(include_path, PATH_MAX, "%s/usr/include", sdk_path);
            if (stat(include_path, &st) == 0)
                pp_add_include_path(include_path);
        }
        pclose(fp);
    }
    pp_add_include_path("/usr/local/include");
#else
    // GCC built-in headers first - needed for include_next to work correctly
    // Try to find GCC include path dynamically
    static const char *gcc_base_dirs[] = {
        "/usr/lib/gcc",
        "/usr/lib64/gcc",
        NULL};
    static const char *gcc_triples[] = {
        "x86_64-pc-linux-gnu",
        "x86_64-linux-gnu",
        "aarch64-linux-gnu",
        "x86_64-suse-linux",
        NULL};

    bool found_gcc = false;
    for (int i = 0; !found_gcc && gcc_base_dirs[i]; i++)
    {
        for (int j = 0; !found_gcc && gcc_triples[j]; j++)
        {
            char *path = find_gcc_include_path(gcc_base_dirs[i], gcc_triples[j]);
            if (path)
            {
                if (stat(path, &st) == 0)
                {
                    pp_add_include_path(path);
                    found_gcc = true;
                }
                else
                {
                    free(path);
                }
            }
        }
    }

    pp_add_include_path("/usr/local/include");

    // Try common multiarch paths
    pp_add_include_path("/usr/include/x86_64-linux-gnu");
    pp_add_include_path("/usr/include/aarch64-linux-gnu");

    pp_add_include_path("/usr/include");
#endif

    // Mark where system paths end; user paths will be added after this
    pp_user_paths_start = pp_include_paths_count;
}

static void strarray_push(StringArray *arr, char *s)
{
    if (!arr->data)
    {
        arr->data = calloc(8, sizeof(char *));
        if (!arr->data)
        {
            fprintf(stderr, "out of memory in strarray_push\n");
            exit(1);
        }
        arr->capacity = 8;
    }
    if (arr->capacity == arr->len)
    {
        // Check for overflow before doubling
        if (arr->capacity > SIZE_MAX / (sizeof(char *) * 2))
        {
            fprintf(stderr, "strarray capacity overflow\n");
            exit(1);
        }
        size_t new_cap = arr->capacity * 2;
        char **new_data = realloc(arr->data, sizeof(char *) * new_cap);
        if (!new_data)
        {
            fprintf(stderr, "out of memory in strarray_push\n");
            exit(1);
        }
        arr->data = new_data;
        arr->capacity = new_cap;
    }
    arr->data[arr->len++] = s;
}

__attribute__((format(printf, 1, 2))) static char *format(char *fmt, ...)
{
    // Fast path: use stack buffer for small strings
    char stack_buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);

    if (len < 0)
    {
        fprintf(stderr, "format error\n");
        exit(1);
    }

    if ((size_t)len < sizeof(stack_buf))
    {
        // Fits in stack buffer, just duplicate it
        return strndup(stack_buf, len);
    }

    // Large string: allocate exact size and format again
    if (len > FORMAT_MAX_SIZE)
    {
        fprintf(stderr, "formatted string too large (%d bytes, max %d)\n", len, FORMAT_MAX_SIZE);
        exit(1);
    }

    char *buf = malloc(len + 1);
    if (!buf)
    {
        fprintf(stderr, "out of memory in format\n");
        exit(1);
    }
    va_start(ap, fmt);
    vsnprintf(buf, len + 1, fmt, ap);
    va_end(ap);
    return buf;
}

static uint64_t fnv_hash(char *s, int len)
{
    uint64_t hash = 0xcbf29ce484222325;
    for (int i = 0; i < len; i++)
    {
        hash *= 0x100000001b3;
        hash ^= (unsigned char)s[i];
    }
    return hash;
}

static void hashmap_put2(HashMap *map, char *key, int keylen, void *val);

static void hashmap_rehash(HashMap *map)
{
    int nkeys = 0;
    for (int i = 0; i < map->capacity; i++)
        if (map->buckets[i].key && map->buckets[i].key != TOMBSTONE)
            nkeys++;

    int cap = map->capacity;
    if (cap < 16)
        cap = 16;
    while ((nkeys * 100) / cap >= 50)
        cap *= 2;

    HashMap map2 = {};
    map2.buckets = calloc((size_t)cap, sizeof(HashEntry));
    if (!map2.buckets)
    {
        fprintf(stderr, "out of memory in hashmap_rehash\n");
        exit(1);
    }
    map2.capacity = cap;

    for (int i = 0; i < map->capacity; i++)
    {
        HashEntry *ent = &map->buckets[i];
        if (ent->key && ent->key != TOMBSTONE)
            hashmap_put2(&map2, ent->key, ent->keylen, ent->val);
    }
    // Free old keys and buckets
    for (int i = 0; i < map->capacity; i++)
    {
        if (map->buckets[i].key && map->buckets[i].key != TOMBSTONE)
            free(map->buckets[i].key);
    }
    free(map->buckets);
    *map = map2;
}

static HashEntry *hashmap_get_entry(HashMap *map, char *key, int keylen)
{
    if (!map->buckets)
        return NULL;
    uint64_t hash = fnv_hash(key, keylen);
    for (int i = 0; i < map->capacity; i++)
    {
        HashEntry *ent = &map->buckets[(hash + i) % map->capacity];
        if (ent->key && ent->key != TOMBSTONE &&
            ent->keylen == keylen && !memcmp(ent->key, key, keylen))
            return ent;
        if (!ent->key)
            return NULL;
    }
    return NULL;
}

static void *hashmap_get2(HashMap *map, char *key, int keylen)
{
    HashEntry *ent = hashmap_get_entry(map, key, keylen);
    return ent ? ent->val : NULL;
}

static void *hashmap_get(HashMap *map, char *key)
{
    return hashmap_get2(map, key, strlen(key));
}

static void hashmap_put2(HashMap *map, char *key, int keylen, void *val)
{
    if (!map->buckets)
    {
        map->buckets = calloc(16, sizeof(HashEntry));
        map->capacity = 16;
    }
    else if ((map->used * 100) / map->capacity >= 70)
    {
        hashmap_rehash(map);
    }

    uint64_t hash = fnv_hash(key, keylen);
    for (int i = 0; i < map->capacity; i++)
    {
        HashEntry *ent = &map->buckets[(hash + i) % map->capacity];
        if (ent->key && ent->key != TOMBSTONE &&
            ent->keylen == keylen && !memcmp(ent->key, key, keylen))
        {
            ent->val = val;
            return;
        }
        if (!ent->key || ent->key == TOMBSTONE)
        {
            bool was_empty = !ent->key;
            // Copy the key to heap memory to avoid dangling stack references
            char *key_copy = malloc(keylen);
            memcpy(key_copy, key, keylen);
            ent->key = key_copy;
            ent->keylen = keylen;
            ent->val = val;
            if (was_empty)
                map->used++;
            return;
        }
    }
}

static void hashmap_put(HashMap *map, char *key, void *val)
{
    hashmap_put2(map, key, strlen(key), val);
}

static void hashmap_delete2(HashMap *map, char *key, int keylen)
{
    HashEntry *ent = hashmap_get_entry(map, key, keylen);
    if (ent)
    {
        free(ent->key); // Free the allocated key copy
        ent->key = TOMBSTONE;
    }
}

static void hashmap_delete(HashMap *map, char *key)
{
    hashmap_delete2(map, key, strlen(key));
}

static void hashmap_clear(HashMap *map)
{
    if (map->buckets)
    {
        // Free keys that were allocated by hashmap_put2
        for (int i = 0; i < map->capacity; i++)
        {
            if (map->buckets[i].key && map->buckets[i].key != TOMBSTONE)
                free(map->buckets[i].key);
        }
        free(map->buckets);
        map->buckets = NULL;
    }
    map->capacity = 0;
    map->used = 0;
}

// Token accessor functions

// Get the File* for a token (via file_idx into input_files array)
static inline File *tok_file(Token *tok)
{
    if (!tok || tok->file_idx >= input_file_count)
        return current_file;
    return input_files[tok->file_idx];
}

// Get line number for a token using binary search on line offset table
// O(log n) instead of O(n) scanning
// Returns -1 for synthetic tokens where loc doesn't point into file contents
static int tok_line_no(Token *tok)
{
    File *f = tok_file(tok);
    if (!f || !f->contents || !tok->loc)
        return -1;
    if (tok->flags & TF_FOREIGN_LOC)
        return -1;

    // Check if tok->loc points into the file contents
    char *file_start = f->contents;
    char *file_end = f->contents + strlen(f->contents);
    if (tok->loc < file_start || tok->loc >= file_end)
        return -1; // Synthetic token - loc doesn't point into file

    int offset = (int)(tok->loc - f->contents);

    // Binary search for the line containing this offset
    int lo = 0, hi = f->line_count - 1;
    while (lo < hi)
    {
        int mid = lo + (hi - lo + 1) / 2;
        if (f->line_offsets[mid] <= offset)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo + 1; // Lines are 1-indexed
}

// Flag accessors
static inline bool tok_at_bol(Token *tok) { return tok->flags & TF_AT_BOL; }
static inline bool tok_has_space(Token *tok) { return tok->flags & TF_HAS_SPACE; }
static inline void tok_set_at_bol(Token *tok, bool v)
{
    if (v)
        tok->flags |= TF_AT_BOL;
    else
        tok->flags &= ~TF_AT_BOL;
}
static inline void tok_set_has_space(Token *tok, bool v)
{
    if (v)
        tok->flags |= TF_HAS_SPACE;
    else
        tok->flags &= ~TF_HAS_SPACE;
}

// Hideset side table accessors
static inline Hideset *tok_hideset(Token *tok)
{
    return (Hideset *)hashmap_get2(&token_hidesets, (char *)&tok, sizeof(tok));
}

static inline void tok_set_hideset(Token *tok, Hideset *hs)
{
    if (hs)
        hashmap_put2(&token_hidesets, (char *)&tok, sizeof(tok), hs);
    else
        hashmap_delete2(&token_hidesets, (char *)&tok, sizeof(tok));
}

// Origin side table accessors (for __LINE__ in macro expansions)
static inline Token *tok_origin(Token *tok)
{
    return (Token *)hashmap_get2(&token_origins, (char *)&tok, sizeof(tok));
}

static inline void tok_set_origin(Token *tok, Token *origin)
{
    if (origin)
        hashmap_put2(&token_origins, (char *)&tok, sizeof(tok), origin);
    else
        hashmap_delete2(&token_origins, (char *)&tok, sizeof(tok));
}

// Unicode
static uint32_t decode_utf8(char **new_pos, char *p)
{
    unsigned char c0 = (unsigned char)*p;
    if (c0 < 128)
    {
        *new_pos = p + 1;
        return *p;
    }

    int len;
    uint32_t c;
    if (c0 >= 0xF0)
    {
        len = 4;
        c = c0 & 0x7;
    }
    else if (c0 >= 0xE0)
    {
        len = 3;
        c = c0 & 0xF;
    }
    else if (c0 >= 0xC0)
    {
        len = 2;
        c = c0 & 0x1F;
    }
    else
    {
        *new_pos = p + 1;
        return *p;
    } // Invalid, skip

    for (int i = 1; i < len; i++)
    {
        unsigned char ci = (unsigned char)p[i];
        if (ci == 0 || (ci & 0xC0) != 0x80)
        {
            *new_pos = p + 1;
            return c0;
        }
        c = (c << 6) | (ci & 0x3F);
    }
    *new_pos = p + len;
    return c;
}

static bool in_range(uint32_t *range, uint32_t c)
{
    for (int i = 0; range[i] != (uint32_t)-1; i += 2)
        if (range[i] <= c && c <= range[i + 1])
            return true;
    return false;
}

static bool is_ident1(uint32_t c)
{
    static uint32_t range[] = {
        '_',
        '_',
        'a',
        'z',
        'A',
        'Z',
        '$',
        '$',
        0x00A8,
        0x00A8,
        0x00AA,
        0x00AA,
        0x00AD,
        0x00AD,
        0x00AF,
        0x00AF,
        0x00B2,
        0x00B5,
        0x00B7,
        0x00BA,
        0x00BC,
        0x00BE,
        0x00C0,
        0x00D6,
        0x00D8,
        0x00F6,
        0x00F8,
        0x02FF,
        0x0370,
        0x167F,
        0x1681,
        0x180D,
        0x180F,
        0x1FFF,
        0x200B,
        0x200D,
        0x202A,
        0x202E,
        0x203F,
        0x2040,
        0x2054,
        0x2054,
        0x2060,
        0x218F,
        0x2460,
        0x24FF,
        0x2776,
        0x2793,
        0x2C00,
        0x2DFF,
        0x2E80,
        0x2FFF,
        0x3004,
        0x3007,
        0x3021,
        0x302F,
        0x3031,
        0xD7FF,
        0xF900,
        0xFD3D,
        0xFD40,
        0xFDCF,
        0xFDF0,
        0xFE44,
        0xFE47,
        0xFFFD,
        0x10000,
        0xEFFFD,
        (uint32_t)-1,
    };
    return in_range(range, c);
}

static bool is_ident2(uint32_t c)
{
    static uint32_t range[] = {
        '0',
        '9',
        '$',
        '$',
        0x0300,
        0x036F,
        0x1DC0,
        0x1DFF,
        0x20D0,
        0x20FF,
        0xFE20,
        0xFE2F,
        (uint32_t)-1,
    };
    return is_ident1(c) || in_range(range, c);
}

static int char_width(uint32_t c)
{
    // Simplified - returns 1 for most chars, 2 for wide CJK
    if (c < 0x1100)
        return 1;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE10 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
        (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

static int display_width(char *p, int len)
{
    char *end = p + len;
    int w = 0;
    while (p < end)
    {
        uint32_t c = decode_utf8(&p, p);
        w += char_width(c);
    }
    return w;
}

noreturn void error(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    exit(1);
}

static void verror_at(char *filename, char *input, int line_no, char *loc, char *fmt, va_list ap)
{
    if (!input || !loc || line_no <= 0)
    {
        fprintf(stderr, "%s:?: ", filename ? filename : "<unknown>");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        return;
    }
    char *line = loc;
    while (input < line && line[-1] != '\n')
        line--;
    char *end = loc;
    while (*end && *end != '\n')
        end++;

    int indent = fprintf(stderr, "%s:%d: ", filename, line_no);
    fprintf(stderr, "%.*s\n", (int)(end - line), line);
    fprintf(stderr, "%*s^ ", indent + display_width(line, loc - line), "");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
}

noreturn void error_at(char *loc, char *fmt, ...)
{
    int line_no = 1;
    for (char *p = current_file->contents; p < loc; p++)
        if (*p == '\n')
            line_no++;
    va_list ap;
    va_start(ap, fmt);
    verror_at(current_file->name, current_file->contents, line_no, loc, fmt, ap);
    exit(1);
}

noreturn void error_tok(Token *tok, char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    Token *t = tok;
    for (; t && tok_line_no(t) <= 0; t = tok_origin(t))
        ;
    if (!t)
        t = tok;
    File *f = tok_file(t);
    verror_at(f->name, f->contents, tok_line_no(t), t->loc, fmt, ap);
    exit(1);
}

static void warn_tok(Token *tok, char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    Token *t = tok;
    for (; t && tok_line_no(t) <= 0; t = tok_origin(t))
        ;
    if (!t)
        t = tok;
    File *f = tok_file(t);
    verror_at(f->name, f->contents, tok_line_no(t), t->loc, fmt, ap);
    va_end(ap);
}

// Use inline function with __builtin_constant_p for compile-time strlen when possible
static inline bool equal(Token *tok, const char *op)
{
    size_t len = __builtin_constant_p(*op) ? __builtin_strlen(op) : strlen(op);
    return tok->len == len && !memcmp(tok->loc, op, len);
}

static Token *skip(Token *tok, char *op)
{
    if (!equal(tok, op))
        error_tok(tok, "expected '%s'", op);
    return tok->next;
}

static bool consume(Token **rest, Token *tok, char *str)
{
    if (equal(tok, str))
    {
        *rest = tok->next;
        return true;
    }
    *rest = tok;
    return false;
}

static Token *new_token(TokenKind kind, char *start, char *end)
{
    Token *tok = arena_alloc_token();
    tok->kind = kind;
    tok->loc = start;
    tok->len = end - start;
    tok->file_idx = current_file ? current_file->file_no : 0;
    tok_set_at_bol(tok, at_bol);
    tok_set_has_space(tok, has_space);
    at_bol = has_space = false;
    return tok;
}

static bool startswith(char *p, char *q)
{
    return !strncmp(p, q, strlen(q));
}

static int read_ident(char *start)
{
    char *p = start;
    uint32_t c = decode_utf8(&p, p);
    if (!is_ident1(c))
        return 0;
    for (;;)
    {
        char *q;
        c = decode_utf8(&q, p);
        if (!is_ident2(c))
            return p - start;
        p = q;
    }
}

static int from_hex(char c)
{
    if ('0' <= c && c <= '9')
        return c - '0';
    if ('a' <= c && c <= 'f')
        return c - 'a' + 10;
    if ('A' <= c && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int read_punct(char *p)
{
    // Fast path: check 3-char punctuators first
    if (p[0] && p[1] && p[2])
    {
        if (p[0] == '<' && p[1] == '<' && p[2] == '=')
            return 3;
        if (p[0] == '>' && p[1] == '>' && p[2] == '=')
            return 3;
        if (p[0] == '.' && p[1] == '.' && p[2] == '.')
            return 3;
    }
    // Check 2-char punctuators using first character as index
    if (p[0] && p[1])
    {
        switch (p[0])
        {
        case '=':
            if (p[1] == '=')
                return 2;
            break;
        case '!':
            if (p[1] == '=')
                return 2;
            break;
        case '<':
            if (p[1] == '=' || p[1] == '<')
                return 2;
            break;
        case '>':
            if (p[1] == '=' || p[1] == '>')
                return 2;
            break;
        case '-':
            if (p[1] == '>' || p[1] == '=' || p[1] == '-')
                return 2;
            break;
        case '+':
            if (p[1] == '=' || p[1] == '+')
                return 2;
            break;
        case '*':
            if (p[1] == '=')
                return 2;
            break;
        case '/':
            if (p[1] == '=')
                return 2;
            break;
        case '%':
            if (p[1] == '=')
                return 2;
            break;
        case '&':
            if (p[1] == '=' || p[1] == '&')
                return 2;
            break;
        case '|':
            if (p[1] == '=' || p[1] == '|')
                return 2;
            break;
        case '^':
            if (p[1] == '=')
                return 2;
            break;
        case '#':
            if (p[1] == '#')
                return 2;
            break;
        }
    }
    return ispunct((unsigned char)*p) ? 1 : 0;
}

static HashMap keyword_map;

static void init_keyword_map(void)
{
    static char *kw[] = {
        "return",
        "if",
        "else",
        "for",
        "while",
        "int",
        "sizeof",
        "char",
        "struct",
        "union",
        "short",
        "long",
        "void",
        "typedef",
        "_Bool",
        "enum",
        "static",
        "goto",
        "break",
        "continue",
        "switch",
        "case",
        "default",
        "extern",
        "_Alignof",
        "_Alignas",
        "do",
        "signed",
        "unsigned",
        "const",
        "volatile",
        "auto",
        "register",
        "restrict",
        "__restrict",
        "__restrict__",
        "_Noreturn",
        "float",
        "double",
        "typeof",
        "asm",
        "_Thread_local",
        "__thread",
        "_Atomic",
        "__attribute__",
        "defer", // Prism extension
        "raw",   // Prism extension: skip zero-init
    };
    for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
        hashmap_put(&keyword_map, kw[i], (void *)1);
}

static bool is_keyword(Token *tok)
{
    return hashmap_get2(&keyword_map, tok->loc, tok->len) != NULL;
}

static int read_escaped_char(char **new_pos, char *p)
{
    if ('0' <= *p && *p <= '7')
    {
        int c = *p++ - '0';
        if ('0' <= *p && *p <= '7')
        {
            c = (c << 3) + (*p++ - '0');
            if (c > 0xFF)
                error_at(p - 1, "octal escape sequence out of range");
            if ('0' <= *p && *p <= '7')
            {
                c = (c << 3) + (*p++ - '0');
                if (c > 0xFF)
                    error_at(p - 1, "octal escape sequence out of range");
            }
        }
        if (c > 0xFF)
            error_at(p - 1, "octal escape sequence out of range");
        *new_pos = p;
        return c;
    }
    if (*p == 'x')
    {
        p++;
        if (!isxdigit(*p))
            error_at(p, "invalid hex escape");
        int c = 0;
        for (; isxdigit(*p); p++)
        {
            int d = from_hex(*p);
            if (d < 0)
                error_at(p, "invalid hex escape");
            c = (c << 4) + d;
            if (c > 0xFF)
                error_at(p, "hex escape sequence out of range");
        }
        *new_pos = p;
        return c;
    }
    *new_pos = p + 1;
    switch (*p)
    {
    case 'a':
        return '\a';
    case 'b':
        return '\b';
    case 't':
        return '\t';
    case 'n':
        return '\n';
    case 'v':
        return '\v';
    case 'f':
        return '\f';
    case 'r':
        return '\r';
    case 'e':
        return 27;
    default:
        return *p;
    }
}

static char *string_literal_end(char *p)
{
    char *start = p;
    for (; *p != '"'; p++)
    {
        if (*p == '\n' || *p == '\0')
            error_at(start, "unclosed string literal");
        if (*p == '\\')
            p++;
    }
    return p;
}

static Token *read_string_literal(char *start, char *quote)
{
    char *end = string_literal_end(quote + 1);
    // Allocate buffer for the unescaped string content + null terminator
    // Size is (end - quote) which includes space for the closing quote we don't copy
    size_t buf_size = (end - quote);
    if (buf_size == 0)
        buf_size = 1; // At minimum, space for null terminator
    char *buf = calloc(1, buf_size);
    if (!buf)
    {
        fprintf(stderr, "out of memory in read_string_literal\n");
        exit(1);
    }
    int len = 0;
    for (char *p = quote + 1; p < end;)
    {
        if (*p == '\\')
            buf[len++] = read_escaped_char(&p, p + 1);
        else
            buf[len++] = *p++;
    }
    Token *tok = new_token(TK_STR, start, end + 1);
    tok->val.str = buf;
    return tok;
}

static Token *read_char_literal(char *start, char *quote)
{
    char *p = quote + 1;
    if (*p == '\0')
        error_at(start, "unclosed char literal");

    uint64_t val = 0;
    int count = 0;
    int first_c = 0;

    for (;;)
    {
        if (*p == '\n' || *p == '\0')
            error_at(p, "unclosed char literal");
        if (*p == '\'')
            break;

        int c;
        if (*p == '\\')
            c = read_escaped_char(&p, p + 1);
        else
            c = decode_utf8(&p, p);

        if (count == 0)
            first_c = c;
        if (count < 4) // sizeof(int)
            val = (val << 8) | (c & 0xFF);
        count++;
        // Multi-character constants are implementation-defined but allowed
        // Continue parsing but truncate value to 4 bytes
    }

    if (count == 0)
        error_at(start, "empty char literal");

    Token *tok = new_token(TK_NUM, start, p + 1);
    tok->val.i64 = (count == 1) ? first_c : (int32_t)val;
    return tok;
}

static int64_t read_int(char **p, int base, bool *overflow)
{
    uint64_t val = 0;
    uint64_t max_before_mul = UINT64_MAX / base;
    *overflow = false;

    while (isxdigit(**p))
    {
        int d = from_hex(**p);
        if (d < 0)
            break;
        if (d >= base)
            break;

        // Check for overflow before multiplication
        if (val > max_before_mul)
        {
            *overflow = true;
        }
        val *= base;

        // Check for overflow before addition
        if (val > UINT64_MAX - d)
        {
            *overflow = true;
        }
        val += d;
        (*p)++;
    }
    return (int64_t)val;
}

static void convert_pp_number(Token *tok)
{
    char *start = tok->loc;
    char *end = tok->loc + tok->len;

    bool is_hex = (end - start >= 2 && start[0] == '0' &&
                   (start[1] == 'x' || start[1] == 'X'));
    bool is_bin = (end - start >= 2 && start[0] == '0' &&
                   (start[1] == 'b' || start[1] == 'B'));
    bool is_float = false;

    if (is_hex)
    {
        for (char *q = start + 2; q < end; q++)
        {
            if (*q == '.' || *q == 'p' || *q == 'P')
            {
                is_float = true;
                break;
            }
        }
    }
    else
    {
        for (char *q = start; q < end; q++)
        {
            if (*q == '.' || *q == 'e' || *q == 'E')
            {
                is_float = true;
                break;
            }
        }
    }

    if (is_bin && is_float)
        error_tok(tok, "invalid binary constant");

    if (is_float)
    {
        char *buf = strndup(start, end - start);
        if (!buf)
            error_tok(tok, "out of memory");
        char *endp;
        long double fval = strtold(buf, &endp);
        if (endp == buf)
            error_tok(tok, "invalid floating constant");
        if (is_hex && !strpbrk(buf, "pP"))
            error_tok(tok, "invalid hex floating constant");
        if (!is_hex && strpbrk(buf, "pP"))
            error_tok(tok, "invalid floating constant");

        if (*endp && endp[1] == '\0' &&
            (*endp == 'f' || *endp == 'F' || *endp == 'l' || *endp == 'L'))
            endp++;
        // Handle C23/GCC extended float suffixes: f16, f32, f64, f128, bf16, F16, F32, F64, F128, BF16
        if (*endp == 'f' || *endp == 'F' || *endp == 'b' || *endp == 'B')
        {
            char *suffix_start = endp;
            // Check for bf16/BF16
            if ((*endp == 'b' || *endp == 'B') && (endp[1] == 'f' || endp[1] == 'F'))
                endp += 2;
            else if (*endp == 'f' || *endp == 'F')
                endp++;
            // Check for numeric part: 16, 32, 64, 128
            if (isdigit(*endp))
            {
                while (isdigit(*endp))
                    endp++;
            }
        }
        // Also handle 'l'/'L' suffix
        if (*endp == 'l' || *endp == 'L')
            endp++;
        if (*endp)
            error_tok(tok, "invalid floating constant");
        tok->kind = TK_NUM;
        tok->flags |= TF_IS_FLOAT;
        tok->val.i64 = (int64_t)fval; // Convert to int for preprocessor expressions
        free(buf);
        return;
    }

    int base = 10;
    char *p = start;
    if (is_hex)
    {
        base = 16;
        p += 2;
    }
    else if (is_bin)
    {
        base = 2;
        p += 2;
    }
    else if (*p == '0')
    {
        base = 8;
    }

    char *digits_start = p;
    bool overflow = false;
    int64_t val = read_int(&p, base, &overflow);
    if (p == digits_start)
        error_tok(tok, "invalid integer constant");
    if (overflow)
        warn_tok(tok, "integer constant overflow");

    bool seen_u = false;
    int seen_l = 0;
    while (p < end)
    {
        if (*p == 'u' || *p == 'U')
        {
            if (seen_u)
                error_tok(tok, "invalid integer constant");
            seen_u = true;
            p++;
            continue;
        }
        if (*p == 'l' || *p == 'L')
        {
            if (seen_l == 2)
                error_tok(tok, "invalid integer constant");
            seen_l++;
            p++;
            continue;
        }
        break;
    }

    if (p != end)
        error_tok(tok, "invalid integer constant");

    tok->kind = TK_NUM;
    tok->val.i64 = val;
}

static void convert_pp_tokens(Token *tok)
{
    for (Token *t = tok; t; t = t->next)
    {
        if (is_keyword(t))
            t->kind = TK_KEYWORD;
        else if (t->kind == TK_PP_NUM)
            convert_pp_number(t);
    }
}

// Line numbers are now computed on demand via tok_line_no()
// This function is kept as a stub for compatibility
static void add_line_numbers(Token *tok)
{
    (void)tok; // No-op: line numbers computed via tok_line_no()
}

static File *new_file(char *name, int file_no, char *contents)
{
    File *file = calloc(1, sizeof(File));
    if (!file)
    {
        fprintf(stderr, "out of memory in new_file\n");
        exit(1);
    }
    // Always duplicate the name so we own it and can free it
    file->name = strdup(name);
    file->display_name = file->name;
    file->file_no = file_no;
    file->contents = contents;

    // Build line offset table for O(log n) line number lookups
    // First pass: count lines
    int line_count = 1;
    for (char *p = contents; *p; p++)
        if (*p == '\n')
            line_count++;

    // Allocate and fill line offset table
    file->line_offsets = malloc(sizeof(int) * line_count);
    if (!file->line_offsets)
    {
        fprintf(stderr, "out of memory allocating line table\n");
        exit(1);
    }
    file->line_count = line_count;

    int line = 0;
    file->line_offsets[line++] = 0; // Line 1 starts at offset 0
    for (char *p = contents; *p; p++)
    {
        if (*p == '\n' && *(p + 1))
            file->line_offsets[line++] = (int)(p + 1 - contents);
    }

    return file;
}

static Token *tokenize(File *file)
{
    current_file = file;
    char *p = file->contents;
    Token head = {};
    Token *cur = &head;

    at_bol = true;
    has_space = false;

    while (*p)
    {
        if (startswith(p, "//"))
        {
            p += 2;
            while (*p && *p != '\n')
                p++;
            has_space = true;
            continue;
        }
        if (startswith(p, "/*"))
        {
            char *q = strstr(p + 2, "*/");
            if (!q)
                error_at(p, "unclosed block comment");
            p = q + 2;
            has_space = true;
            continue;
        }
        if (*p == '\n')
        {
            p++;
            at_bol = true;
            has_space = false;
            continue;
        }
        if (isspace(*p))
        {
            p++;
            has_space = true;
            continue;
        }
        if (isdigit(*p) || (*p == '.' && isdigit(p[1])))
        {
            char *q = p++;
            for (;;)
            {
                if (p[0] && p[1] && strchr("eEpP", p[0]) && strchr("+-", p[1]))
                    p += 2;
                else if (isalnum(*p) || *p == '.' || *p == '_')
                    p++;
                else
                    break;
            }
            cur = cur->next = new_token(TK_PP_NUM, q, p);
            continue;
        }
        if (*p == '"')
        {
            cur = cur->next = read_string_literal(p, p);
            p += cur->len;
            continue;
        }
        // Wide and unicode string literals: L"...", u"...", U"...", u8"..."
        if (startswith(p, "u8\""))
        {
            cur = cur->next = read_string_literal(p, p + 2);
            p += cur->len;
            continue;
        }
        if ((*p == 'L' || *p == 'u' || *p == 'U') && p[1] == '"')
        {
            cur = cur->next = read_string_literal(p, p + 1);
            p += cur->len;
            continue;
        }
        if (*p == '\'')
        {
            cur = cur->next = read_char_literal(p, p);
            p += cur->len;
            continue;
        }
        // Wide and unicode character literals: L'...', u'...', U'...'
        if ((*p == 'L' || *p == 'u' || *p == 'U') && p[1] == '\'')
        {
            cur = cur->next = read_char_literal(p, p + 1);
            p += cur->len;
            continue;
        }
        int ident_len = read_ident(p);
        if (ident_len)
        {
            cur = cur->next = new_token(TK_IDENT, p, p + ident_len);
            p += ident_len;
            continue;
        }
        int punct_len = read_punct(p);
        if (punct_len)
        {
            cur = cur->next = new_token(TK_PUNCT, p, p + punct_len);
            p += punct_len;
            continue;
        }
        error_at(p, "invalid token");
    }

    cur = cur->next = new_token(TK_EOF, p, p);
    add_line_numbers(head.next);
    return head.next;
}

static char *read_file(char *path)
{
    FILE *fp = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
    if (!fp)
        return NULL;

    char *buf;
    size_t buflen;
    FILE *out = open_memstream(&buf, &buflen);
    if (!out)
    {
        if (fp != stdin)
            fclose(fp);
        return NULL;
    }
    char buf2[4096];
    int n;
    while ((n = fread(buf2, 1, sizeof(buf2), fp)) > 0)
        fwrite(buf2, 1, n, out);
    if (fp != stdin)
        fclose(fp);

    fflush(out);
    if (buflen == 0 || buf[buflen - 1] != '\n')
        fputc('\n', out);
    fputc('\0', out);
    fclose(out);
    return buf;
}

static void canonicalize_newline(char *p)
{
    int i = 0, j = 0;
    while (p[i])
    {
        if (p[i] == '\r' && p[i + 1] == '\n')
        {
            i += 2;
            p[j++] = '\n';
        }
        else if (p[i] == '\r')
        {
            i++;
            p[j++] = '\n';
        }
        else
            p[j++] = p[i++];
    }
    p[j] = '\0';
}

static void remove_backslash_newline(char *p)
{
    int i = 0, j = 0, n = 0;
    while (p[i])
    {
        if (p[i] == '\\' && p[i + 1] == '\n')
        {
            i += 2;
            n++;
        }
        else if (p[i] == '\n')
        {
            p[j++] = p[i++];
            for (; n > 0; n--)
                p[j++] = '\n';
        }
        else
            p[j++] = p[i++];
    }
    for (; n > 0; n--)
        p[j++] = '\n';
    p[j] = '\0';
}

// Forward declarations
static bool is_hash(Token *tok);
static Macro *find_macro_by_name(char *name);
static Token *new_eof(Token *tok);
static Token *copy_token_list(Token *tok);

// Detect if file has include guard pattern: #ifndef X / #define X ... #endif
// Returns the guard macro name if detected, NULL otherwise
static char *detect_include_guard(Token *tok)
{
    // Skip any leading whitespace/comments - look for #ifndef at start
    while (tok && tok->kind != TK_EOF && !is_hash(tok))
        tok = tok->next;

    if (!tok || tok->kind == TK_EOF || !is_hash(tok))
        return NULL;

    tok = tok->next; // skip #
    if (!equal(tok, "ifndef"))
        return NULL;

    tok = tok->next;
    if (tok->kind != TK_IDENT)
        return NULL;

    char *guard_name = strndup(tok->loc, tok->len);

    // Next non-whitespace should be #define with same name
    tok = tok->next;
    while (tok && tok->kind != TK_EOF && !is_hash(tok))
        tok = tok->next;

    if (!tok || tok->kind == TK_EOF || !is_hash(tok))
    {
        free(guard_name);
        return NULL;
    }

    tok = tok->next; // skip #
    if (!equal(tok, "define"))
    {
        free(guard_name);
        return NULL;
    }

    tok = tok->next;
    if (tok->kind != TK_IDENT || tok->len != (int)strlen(guard_name) ||
        strncmp(tok->loc, guard_name, tok->len) != 0)
    {
        free(guard_name);
        return NULL;
    }

    return guard_name;
}

Token *tokenize_file(char *path)
{
    // Resolve to canonical path
    char *real = realpath(path, NULL);
    char *key = real ? real : path;

    // Check if we've already fully included this file (pragma once or guard-protected)
    if (hashmap_get(&pragma_once_files, key) || hashmap_get(&guarded_files, key))
    {
        if (real)
            free(real);
        return new_eof(NULL);
    }

    // Read file from disk
    char *p = read_file(path);
    if (!p)
    {
        if (real)
            free(real);
        return NULL;
    }

    // Skip UTF-8 BOM if present, but keep original pointer for proper freeing
    char *contents_start = p;
    if (!memcmp(p, "\xef\xbb\xbf", 3))
        contents_start = p + 3;
    canonicalize_newline(contents_start);
    remove_backslash_newline(contents_start);

    // If BOM was skipped, shift contents to start of buffer so free() works correctly
    if (contents_start != p)
    {
        memmove(p, contents_start, strlen(contents_start) + 1);
    }

    File *file = new_file(path, input_file_count, p);
    File **new_input_files = realloc(input_files, sizeof(File *) * (input_file_count + 2));
    if (!new_input_files)
        error("out of memory loading file");
    input_files = new_input_files;
    input_files[input_file_count++] = file;
    input_files[input_file_count] = NULL;

    Token *toks = tokenize(file);

    // Detect include guard pattern: #ifndef X / #define X
    // If found, mark file as guarded so we skip on subsequent includes
    char *detected_guard = detect_include_guard(toks);
    if (detected_guard)
    {
        // Store marker value (1) instead of guard name - we only check presence
        hashmap_put(&guarded_files, real ? real : path, (void *)1);
        free(detected_guard);
    }

    if (real)
        free(real);
    return toks;
}

static Token *preprocess2(Token *tok);

static bool is_hash(Token *tok)
{
    return tok_at_bol(tok) && equal(tok, "#");
}

static bool header_contains_include_next(Token *tok)
{
    for (Token *t = tok; t && t->kind != TK_EOF; t = t->next)
    {
        if (is_hash(t) && t->next && equal(t->next, "include_next"))
            return true;
    }
    return false;
}

static Token *skip_line(Token *tok)
{
    if (tok_at_bol(tok))
        return tok;
    warn_tok(tok, "extra token");
    while (!tok_at_bol(tok))
        tok = tok->next;
    return tok;
}

// Skip to end of line without warning (for ignored directives like #pragma)
static Token *skip_line_quiet(Token *tok)
{
    while (!tok_at_bol(tok))
        tok = tok->next;
    return tok;
}

static Token *copy_token(Token *tok)
{
    Token *t = arena_alloc_token();
    *t = *tok;
    t->next = NULL;
    // Copy hideset from the source token
    Hideset *hs = tok_hideset(tok);
    if (hs)
        tok_set_hideset(t, hs);
    return t;
}

static Token *new_eof(Token *tok)
{
    Token *t;
    if (tok)
    {
        t = copy_token(tok);
    }
    else
    {
        t = arena_alloc_token();
        t->loc = "";
        t->next = NULL;
    }
    t->kind = TK_EOF;
    t->len = 0;
    return t;
}

static Token *copy_token_list(Token *tok)
{
    Token head = {};
    Token *cur = &head;
    for (; tok && tok->kind != TK_EOF; tok = tok->next)
        cur = cur->next = copy_token(tok);
    cur->next = new_eof(tok ? tok : cur);
    return head.next;
}

static Hideset *new_hideset(char *name)
{
    Hideset *hs = calloc(1, sizeof(Hideset));
    if (!hs)
    {
        fprintf(stderr, "out of memory in new_hideset\n");
        exit(1);
    }
    hs->name = name;
    hs->len = strlen(name);
    hs->hash = fnv_hash(name, hs->len);
    return hs;
}

static Hideset *hideset_union(Hideset *hs1, Hideset *hs2)
{
    Hideset head = {};
    Hideset *cur = &head;
    for (; hs1; hs1 = hs1->next)
        cur = cur->next = new_hideset(hs1->name);
    cur->next = hs2;
    return head.next;
}

static bool hideset_contains(Hideset *hs, char *s, int len)
{
    uint32_t h = fnv_hash(s, len);
    for (; hs; hs = hs->next)
        if (hs->hash == h && hs->len == len && !memcmp(hs->name, s, len))
            return true;
    return false;
}

static Hideset *hideset_intersection(Hideset *hs1, Hideset *hs2)
{
    Hideset head = {};
    Hideset *cur = &head;
    for (; hs1; hs1 = hs1->next)
        if (hideset_contains(hs2, hs1->name, hs1->len))
            cur = cur->next = new_hideset(hs1->name);
    return head.next;
}

static Token *add_hideset(Token *tok, Hideset *hs)
{
    Token head = {};
    Token *cur = &head;
    for (; tok; tok = tok->next)
    {
        Token *t = copy_token(tok);
        tok_set_hideset(t, hideset_union(tok_hideset(t), hs));
        cur = cur->next = t;
    }
    return head.next;
}

static Token *append(Token *tok1, Token *tok2)
{
    if (!tok1 || tok1->kind == TK_EOF)
        return tok2;
    Token head = {};
    Token *cur = &head;
    for (; tok1 && tok1->kind != TK_EOF; tok1 = tok1->next)
        cur = cur->next = copy_token(tok1);
    cur->next = tok2;
    return head.next;
}

static Macro *find_macro_by_name(char *name)
{
    if (!name)
        return NULL;
    return hashmap_get(&macros, name);
}

static Macro *find_macro(Token *tok)
{
    if (tok->kind != TK_IDENT)
        return NULL;
    return hashmap_get2(&macros, tok->loc, tok->len);
}

// Free a Macro structure and all its allocated memory
static void free_macro(Macro *m)
{
    if (!m)
        return;
    // Free macro name (strdup'd in add_macro)
    if (m->name)
        free(m->name);
    // Free va_args_name if present (allocated by strndup/strdup)
    if (m->va_args_name)
        free(m->va_args_name);
    // Free params linked list (each param->name allocated by strndup)
    MacroParam *p = m->params;
    while (p)
    {
        MacroParam *next = p->next;
        if (p->name)
            free(p->name);
        free(p);
        p = next;
    }
    // m->body tokens are in the arena, don't free individually
    // m->handler is a function pointer, not allocated
    free(m);
}

// Free all macros in the hashmap
static void free_all_macros(void)
{
    if (!macros.buckets)
        return;
    for (int i = 0; i < macros.capacity; i++)
    {
        if (macros.buckets[i].key && macros.buckets[i].key != TOMBSTONE)
        {
            Macro *m = (Macro *)macros.buckets[i].val;
            macros.buckets[i].val = NULL; // Null out before freeing to prevent double-free
            free_macro(m);
        }
    }
}

static Macro *add_macro(char *name, bool is_objlike, Token *body, Token *def_tok)
{
    // Check if macro already exists - if so, free it first to avoid memory leaks
    // and corrupted state on subsequent pp_reset() calls
    Macro *existing = hashmap_get(&macros, name);
    if (existing)
    {
        // Remove from hashmap first (don't free key - hashmap_delete2 handles that)
        hashmap_delete2(&macros, name, strlen(name));
        free_macro(existing);
    }

    Macro *m = calloc(1, sizeof(Macro));
    if (!m)
    {
        fprintf(stderr, "out of memory in add_macro\n");
        exit(1);
    }
    // Always strdup the name so we own it and can free it in free_macro
    m->name = strdup(name);
    if (!m->name)
    {
        fprintf(stderr, "out of memory in add_macro\n");
        exit(1);
    }
    m->is_objlike = is_objlike;
    // Mark macros as virtual when defined inside a virtual include context.
    // Virtual macros are not expanded in user code (only used for #if evaluation),
    // because the backend compiler will see the real definitions when processing
    // the passed-through header.
    m->is_virtual = in_virtual_include;
    m->body = body;
    hashmap_put(&macros, m->name, m);
    return m;
}

static Token *new_str_token(char *str, Token *tmpl)
{
    char *buf = format("\"%s\"", str);
    File *f = tok_file(tmpl);
    return tokenize(new_file(f->name, f->file_no, buf));
}

static Token *new_num_token(int val, Token *tmpl)
{
    char *buf = format("%d", val);
    File *f = tok_file(tmpl);
    return tokenize(new_file(f->name, f->file_no, buf));
}

// Built-in macros
static Token *file_macro(Token *tmpl)
{
    while (tok_origin(tmpl))
        tmpl = tok_origin(tmpl);
    return new_str_token(tok_file(tmpl)->display_name, tmpl);
}

static Token *line_macro(Token *tmpl)
{
    while (tok_origin(tmpl))
        tmpl = tok_origin(tmpl);
    File *f = tok_file(tmpl);
    return new_num_token(tok_line_no(tmpl) + f->line_delta, tmpl);
}

static void add_builtin(char *name, macro_handler_fn *fn)
{
    Macro *m = add_macro(name, true, NULL, NULL);
    m->handler = fn;
}

static MacroArg *read_macro_args(Token **rest, Token *tok, MacroParam *params, char *va_name)
{
    MacroArg head = {};
    MacroArg *cur = &head;

    MacroParam *pp = params;

    // Read positional arguments
    while (pp)
    {
        if (cur != &head)
            tok = skip(tok, ",");

        MacroArg *arg = calloc(1, sizeof(MacroArg));
        arg->name = pp->name;
        arg->name_len = strlen(pp->name);
        arg->name_hash = fnv_hash(pp->name, arg->name_len);

        Token arg_head = {};
        Token *arg_cur = &arg_head;
        int depth = 0;

        while (tok->kind != TK_EOF)
        {
            if (depth == 0 && equal(tok, ")"))
                break;
            if (depth == 0 && equal(tok, ",") && (pp->next || va_name))
                break;
            if (equal(tok, "("))
                depth++;
            else if (equal(tok, ")"))
                depth--;
            arg_cur = arg_cur->next = copy_token(tok);
            tok = tok->next;
        }

        arg_cur->next = new_eof(tok);
        arg->tok = arg_head.next ? arg_head.next : new_eof(tok);
        cur->next = arg;
        cur = arg;
        pp = pp->next;
    }

    // Read variadic arguments if present
    if (va_name)
    {
        MacroArg *arg = calloc(1, sizeof(MacroArg));
        arg->name = va_name;
        arg->name_len = strlen(va_name);
        arg->name_hash = fnv_hash(va_name, arg->name_len);
        arg->is_va_args = true;

        Token arg_head = {};
        Token *arg_cur = &arg_head;

        // Skip comma if we had positional args and there are more args
        if (cur != &head && !equal(tok, ")"))
            tok = skip(tok, ",");

        int depth = 0;
        while (tok->kind != TK_EOF)
        {
            if (depth == 0 && equal(tok, ")"))
                break;
            if (equal(tok, "("))
                depth++;
            else if (equal(tok, ")"))
                depth--;
            arg_cur = arg_cur->next = copy_token(tok);
            tok = tok->next;
        }

        arg_cur->next = new_eof(tok);
        arg->tok = arg_head.next ? arg_head.next : new_eof(tok);
        cur->next = arg;
    }

    *rest = skip(tok, ")");
    return head.next;
}

static Token *find_arg(MacroArg *args, Token *tok)
{
    uint32_t h = fnv_hash(tok->loc, tok->len);
    for (MacroArg *ap = args; ap; ap = ap->next)
        if (ap->name_hash == h && tok->len == ap->name_len && !memcmp(tok->loc, ap->name, tok->len))
            return ap->tok;
    return NULL;
}

// Stringify tokens
static char *stringify_tokens(Token *tok)
{
    char *buf;
    size_t buflen;
    FILE *fp = open_memstream(&buf, &buflen);

    for (Token *t = tok; t && t->kind != TK_EOF; t = t->next)
    {
        if (t != tok && tok_has_space(t))
            fputc(' ', fp);
        // Escape quotes and backslashes in strings
        if (t->kind == TK_STR)
        {
            fputc('"', fp);
            for (int i = 0; i < (int)t->len - 2; i++)
            {
                char c = t->loc[i + 1];
                if (c == '"' || c == '\\')
                    fputc('\\', fp);
                fputc(c, fp);
            }
            fputc('"', fp);
        }
        else
        {
            fprintf(fp, "%.*s", t->len, t->loc);
        }
    }
    fclose(fp);
    return buf;
}

static Token *new_str_token_raw(char *str, Token *tmpl)
{
    // Create a properly escaped string token
    char *buf;
    size_t buflen;
    FILE *fp = open_memstream(&buf, &buflen);
    fputc('"', fp);
    for (char *p = str; *p; p++)
    {
        if (*p == '"' || *p == '\\')
            fputc('\\', fp);
        fputc(*p, fp);
    }
    fputc('"', fp);
    fputc('\n', fp);
    fclose(fp);
    File *f = tok_file(tmpl);
    return tokenize(new_file(f->name, f->file_no, buf));
}

// Paste two tokens together
static Token *paste(Token *lhs, Token *rhs)
{
    char *buf = format("%.*s%.*s\n", lhs->len, lhs->loc, rhs->len, rhs->loc);
    File *f = tok_file(lhs);
    Token *tok = tokenize(new_file(f->name, f->file_no, buf));
    if (tok->next->kind != TK_EOF)
        error_tok(lhs, "pasting \"%.*s\" and \"%.*s\" does not produce a valid token",
                  lhs->len, lhs->loc, rhs->len, rhs->loc);
    return tok;
}

static Token *subst(Token *tok, MacroArg *args)
{
    Token head = {};
    Token *cur = &head;

    while (tok->kind != TK_EOF)
    {
        // Handle # (stringification)
        if (equal(tok, "#"))
        {
            if (tok->next->kind == TK_EOF)
                error_tok(tok, "'#' cannot appear at end of macro expansion");
            Token *arg = find_arg(args, tok->next);
            if (!arg)
                error_tok(tok->next, "'#' is not followed by a macro parameter");
            char *str = stringify_tokens(arg);
            cur = cur->next = new_str_token_raw(str, tok);
            tok = tok->next->next;
            continue;
        }

        // Handle ## (token pasting)
        if (equal(tok, "##"))
        {
            if (cur == &head)
                error_tok(tok, "'##' cannot appear at start of macro expansion");
            Token *hash_tok = tok; // Save for error message
            tok = tok->next;

            // Check for ## at end of macro
            if (tok->kind == TK_EOF)
                error_tok(hash_tok, "'##' cannot appear at end of macro expansion");

            // Get rhs - either arg tokens or literal token
            Token *rhs = find_arg(args, tok);
            if (rhs)
            {
                // If arg is empty, skip
                if (rhs->kind == TK_EOF)
                {
                    tok = tok->next;
                    continue;
                }
                // Paste cur with first token of arg
                *cur = *paste(cur, rhs);
                // Append rest of arg
                for (Token *t = rhs->next; t && t->kind != TK_EOF; t = t->next)
                    cur = cur->next = copy_token(t);
            }
            else
            {
                *cur = *paste(cur, tok);
            }
            tok = tok->next;
            continue;
        }

        // Check if next is ## and current is an arg
        Token *arg = find_arg(args, tok);
        if (equal(tok->next, "##") && arg)
        {
            // If arg is empty, skip both arg and ##
            if (arg->kind == TK_EOF)
            {
                tok = tok->next->next;
                continue;
            }
            // Add arg tokens; last one will be pasted by ## handling
            for (Token *t = arg; t && t->kind != TK_EOF; t = t->next)
                cur = cur->next = copy_token(t);
            tok = tok->next;
            continue;
        }

        if (arg)
        {
            // Expand argument (without ##) - not affected by virtual include mode
            bool save_virtual = in_virtual_include;
            in_virtual_include = false;
            Token *expanded = preprocess2(copy_token_list(arg));
            in_virtual_include = save_virtual;
            for (Token *t = expanded; t && t->kind != TK_EOF; t = t->next)
                cur = cur->next = copy_token(t);
            tok = tok->next;
            continue;
        }
        cur = cur->next = copy_token(tok);
        tok = tok->next;
    }

    cur->next = tok;
    return head.next;
}

// Check if a macro body is a struct member access pattern: identifier.identifier
// These macros (like sa_handler -> __sigaction_handler.sa_handler) should not be
// expanded because they are transparent struct accessors that depend on platform-
// specific struct layouts.
static bool is_member_access_macro(Token *body)
{
    // Pattern: identifier . identifier [EOF or end]
    if (!body || body->kind != TK_IDENT)
        return false;
    Token *dot = body->next;
    if (!dot || dot->kind != TK_PUNCT || !equal(dot, "."))
        return false;
    Token *member = dot->next;
    if (!member || member->kind != TK_IDENT)
        return false;
    Token *end = member->next;
    return !end || end->kind == TK_EOF;
}

// Transfer file_idx from source token to all tokens in a list
// Used when expanding virtual macros so #line directives don't jump to system headers
static void transfer_file_location(Token *body, Token *source)
{
    for (Token *t = body; t && t->kind != TK_EOF; t = t->next)
    {
        t->file_idx = source->file_idx;
        t->flags |= TF_FOREIGN_LOC;
    }
}

static bool expand_macro(Token **rest, Token *tok)
{
    if (hideset_contains(tok_hideset(tok), tok->loc, tok->len))
        return false;

    Macro *m = find_macro(tok);
    if (!m)
        return false;

    if (m->handler)
    {
        bool at_bol = tok_at_bol(tok);
        bool has_space = tok_has_space(tok);
        Token *body = m->handler(tok);
        if (body && body->kind != TK_EOF)
        {
            tok_set_at_bol(body, at_bol);
            tok_set_has_space(body, has_space);
        }
        *rest = append(body, tok->next);
        return true;
    }

    if (m->is_objlike)
    {
        // Don't expand struct member access macros like sa_handler
        if (is_member_access_macro(m->body))
            return false;

        bool at_bol = tok_at_bol(tok);
        bool has_space = tok_has_space(tok);
        Hideset *hs = hideset_union(tok_hideset(tok), new_hideset(m->name));
        Token *body = add_hideset(m->body, hs);
        if (body && body->kind != TK_EOF)
        {
            tok_set_at_bol(body, at_bol);
            tok_set_has_space(body, has_space);
        }
        for (Token *t = body; t && t->kind != TK_EOF; t = t->next)
            tok_set_origin(t, tok);
        // For virtual macros, transfer file location so #line doesn't jump to system headers
        if (m->is_virtual)
            transfer_file_location(body, tok);
        *rest = append(body, tok->next);
        return true;
    }

    // Don't expand function-like macros from virtual includes in regular code
    // They may reference platform-specific struct members (e.g., FD_ZERO uses __fds_bits)
    // But DO expand them in preprocessor constant expressions (e.g., #if UINT64_MAX > X)
    // where unexpanded macros would cause parse errors like "0(...)"
    if (m->is_virtual && !in_pp_const_expr)
        return false;

    if (!equal(tok->next, "("))
        return false;

    bool at_bol = tok_at_bol(tok);
    bool has_space = tok_has_space(tok);
    Token *macro_tok = tok;
    MacroArg *args = read_macro_args(&tok, tok->next->next, m->params, m->va_args_name);
    Token *body = subst(m->body, args);
    Hideset *hs = hideset_intersection(tok_hideset(macro_tok), tok_hideset(tok));
    hs = hideset_union(hs, new_hideset(m->name));
    body = add_hideset(body, hs);
    if (body && body->kind != TK_EOF)
    {
        tok_set_at_bol(body, at_bol);
        tok_set_has_space(body, has_space);
    }
    for (Token *t = body; t && t->kind != TK_EOF; t = t->next)
        tok_set_origin(t, macro_tok);
    // For virtual macros, transfer file location so #line doesn't jump to system headers
    if (m->is_virtual)
        transfer_file_location(body, macro_tok);
    *rest = append(body, tok);
    return true;
}

static char *search_include_paths(char *filename)
{
    for (int i = 0; i < pp_include_paths_count; i++)
    {
        char *path = format("%s/%s", pp_include_paths[i], filename);
        struct stat st;
        if (stat(path, &st) == 0)
            return path;
        free(path);
    }
    return NULL;
}

static bool path_is_under(const char *path, const char *dir)
{
    if (!path || !dir)
        return false;
    size_t len = strlen(dir);
    return strncmp(path, dir, len) == 0 && (path[len] == '/' || path[len] == '\0');
}

static bool is_user_header_path(const char *path)
{
    if (!path)
        return false;
    char *path_real = realpath(path, NULL);
    const char *path_cmp = path_real ? path_real : path;

    for (int i = pp_user_paths_start; i < pp_include_paths_count; i++)
    {
        char *inc_real = realpath(pp_include_paths[i], NULL);
        const char *inc_cmp = inc_real ? inc_real : pp_include_paths[i];
        bool match = path_is_under(path_cmp, inc_cmp);
        if (inc_real)
            free(inc_real);
        if (match)
        {
            if (path_real)
                free(path_real);
            return true;
        }
    }

    if (path_real)
        free(path_real);
    return false;
}

static bool is_config_header(const char *filename)
{
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    return strcmp(base, "config.h") == 0;
}

// Search a subset of include paths for include_next
static char *search_include_next_range(const char *filename, int start, int end, const char *matched_real)
{
    struct stat st;
    for (int i = start; i < end; i++)
    {
        if (matched_real)
        {
            char *inc_real = realpath(pp_include_paths[i], NULL);
            if (inc_real && strcmp(inc_real, matched_real) == 0)
            {
                free(inc_real);
                continue;
            }
            if (inc_real)
                free(inc_real);
        }

        char *try = format("%s/%s", pp_include_paths[i], filename);
        if (stat(try, &st) == 0)
            return try;
        free(try);
    }
    return NULL;
}

// Search only user-specified include paths (from -I flags)
static char *search_user_include_paths(char *filename)
{
    for (int i = pp_user_paths_start; i < pp_include_paths_count; i++)
    {
        char *path = format("%s/%s", pp_include_paths[i], filename);
        struct stat st;
        if (stat(path, &st) == 0)
            return path;
        free(path);
    }
    return NULL;
}

static char *read_include_filename(Token **rest, Token *tok, bool *is_dquote)
{
    if (tok->kind == TK_STR)
    {
        if (tok->len < 2 || tok->loc[0] != '"' || tok->loc[tok->len - 1] != '"')
            error_tok(tok, "expected a filename");
        *is_dquote = true;
        *rest = tok->next;
        return strndup(tok->loc + 1, tok->len - 2);
    }

    if (equal(tok, "<"))
    {
        Token *start = tok->next;
        int len = 0;
        for (tok = tok->next; !equal(tok, ">"); tok = tok->next)
        {
            if (tok_at_bol(tok) || tok->kind == TK_EOF)
                error_tok(tok, "expected '>'");
            if (tok_has_space(tok))
                error_tok(tok, "invalid header name");
            len += tok->len;
        }
        *is_dquote = false;
        *rest = tok->next;

        if (len == 0)
            error_tok(tok, "expected a filename");

        char *buf = calloc(1, len + 1);
        char *p = buf;
        for (Token *t = start; !equal(t, ">"); t = t->next)
        {
            memcpy(p, t->loc, t->len);
            p += t->len;
        }
        return buf;
    }

    error_tok(tok, "expected a filename");
}

static Token *emit_include_like_directive(Token *start, Token *next_tok, const char *path, bool use_quotes,
                                          const char *directive)
{
    size_t path_len = strlen(path);
    char *include_str = malloc(path_len + 32);
    if (!include_str)
        error_tok(start, "out of memory");
    if (use_quotes)
        sprintf(include_str, "#%s \"%s\"", directive, path);
    else
        sprintf(include_str, "#%s <%s>", directive, path);

    Token *out_tok = copy_token(start);
    out_tok->kind = TK_IDENT;
    out_tok->loc = include_str;
    out_tok->len = strlen(include_str);
    tok_set_at_bol(out_tok, true);
    tok_set_has_space(out_tok, false);
    out_tok->next = next_tok;
    if (next_tok && next_tok->kind != TK_EOF)
        tok_set_at_bol(next_tok, true);
    return out_tok;
}

static Token *emit_include_directive(Token *start, Token *next_tok, const char *path, bool use_quotes)
{
    return emit_include_like_directive(start, next_tok, path, use_quotes, "include");
}

static Token *emit_include_next_directive(Token *start, Token *next_tok, const char *path, bool use_quotes)
{
    return emit_include_like_directive(start, next_tok, path, use_quotes, "include_next");
}

// Check if a macro name looks like a behavior-control macro that should be
// passed through to ensure correct behavior of passed-through headers.
// We are careful to NOT match include guards (*_H) which would cause
// -Wunused-macros warnings.
static bool is_passthrough_guard_macro(const char *name, int name_len)
{
    // IN_* macros: "I'm building this module" guards (e.g., IN_MBSRTOC32S)
    if (name_len >= 3 && strncmp(name, "IN_", 3) == 0)
        return true;
    // _GL_NO_* macros: disable feature flags (e.g., _GL_NO_CONST_GENERICS)
    if (name_len >= 7 && strncmp(name, "_GL_NO_", 7) == 0)
        return true;
    return false;
}

// Convert a simple macro body to a string representation
// Returns NULL if the body is too complex to emit as a define
static char *macro_body_to_string(Token *body)
{
    if (!body || body->kind == TK_EOF)
        return strdup("");

    // For simple single-token bodies, return the token text
    if (body->next && body->next->kind == TK_EOF)
    {
        return strndup(body->loc, body->len);
    }

    // For multi-token bodies, concatenate them
    size_t total_len = 0;
    for (Token *t = body; t && t->kind != TK_EOF; t = t->next)
    {
        if (tok_has_space(t) && total_len > 0)
            total_len++;
        total_len += t->len;
    }

    char *result = malloc(total_len + 1);
    if (!result)
        return NULL;

    char *p = result;
    for (Token *t = body; t && t->kind != TK_EOF; t = t->next)
    {
        if (tok_has_space(t) && p > result)
            *p++ = ' ';
        memcpy(p, t->loc, t->len);
        p += t->len;
    }
    *p = '\0';
    return result;
}

// Emit a #define directive to the output stream
// Used to preserve guard macros that affect passed-through headers
static Token *emit_define_directive(Token *start, Token *next_tok, const char *name, const char *value)
{
    size_t name_len = strlen(name);
    size_t value_len = value ? strlen(value) : 0;
    size_t total_len = 8 + name_len + (value_len ? 1 + value_len : 0) + 1; // "#define " + name + " " + value + null
    char *define_str = malloc(total_len);
    if (!define_str)
        error_tok(start, "out of memory");
    if (value && *value)
        sprintf(define_str, "#define %s %s", name, value);
    else
        sprintf(define_str, "#define %s", name);

    Token *out_tok = copy_token(start);
    out_tok->kind = TK_IDENT;
    out_tok->loc = define_str;
    out_tok->len = strlen(define_str);
    tok_set_at_bol(out_tok, true);
    tok_set_has_space(out_tok, false);
    out_tok->next = next_tok;
    if (next_tok && next_tok->kind != TK_EOF)
        tok_set_at_bol(next_tok, true);
    return out_tok;
}

static Token *include_file(Token *tok, char *path, Token *cont)
{
    Token *tok2 = tokenize_file(path);
    if (!tok2)
        error_tok(tok, "%s: cannot open file", path);

    return append(tok2, cont);
}

static MacroParam *read_macro_params(Token **rest, Token *tok, char **va_name)
{
    MacroParam head = {};
    MacroParam *cur = &head;

    while (!equal(tok, ")"))
    {
        if (cur != &head)
            tok = skip(tok, ",");

        if (equal(tok, "..."))
        {
            *va_name = strdup("__VA_ARGS__");
            if (!*va_name)
            {
                fprintf(stderr, "out of memory in read_macro_params\n");
                exit(1);
            }
            tok = tok->next;
            break;
        }

        if (tok->kind != TK_IDENT)
            error_tok(tok, "expected identifier");

        if (equal(tok->next, "..."))
        {
            *va_name = strndup(tok->loc, tok->len);
            tok = tok->next->next;
            break;
        }

        MacroParam *m = calloc(1, sizeof(MacroParam));
        m->name = strndup(tok->loc, tok->len);
        cur = cur->next = m;
        tok = tok->next;
    }

    *rest = skip(tok, ")");
    return head.next;
}

static Token *copy_line(Token **rest, Token *tok)
{
    Token head = {};
    Token *cur = &head;
    for (; !tok_at_bol(tok); tok = tok->next)
        cur = cur->next = copy_token(tok);
    cur->next = new_eof(tok);
    *rest = tok;
    return head.next;
}

static void read_macro_definition(Token **rest, Token *tok)
{
    if (tok->kind != TK_IDENT)
        error_tok(tok, "expected identifier");

    char *name = strndup(tok->loc, tok->len);
    int name_len = tok->len;
    tok = tok->next;

    if (!tok_has_space(tok) && equal(tok, "("))
    {
        // Function-like macro
        char *va_name = NULL;
        MacroParam *params = read_macro_params(&tok, tok->next, &va_name);
        Macro *m = add_macro(name, false, copy_line(rest, tok), tok);
        free(name); // add_macro strdup's the name, so we can free our copy
        m->params = params;
        m->va_args_name = va_name;
        return;
    }

    // Object-like macro
    Token *body = copy_line(rest, tok);

    // Check if this is a feature test macro that needs to be passed to backend
    if (is_feature_test_macro(name, name_len))
        record_feature_test_macro(name, name_len, body);

    add_macro(name, true, body, tok);
    free(name); // add_macro strdup's the name, so we can free our copy
}

static int64_t eval_const_expr(Token **rest, Token *tok);

static int64_t eval_primary(Token **rest, Token *tok)
{
    if (equal(tok, "("))
    {
        int64_t val = eval_const_expr(&tok, tok->next);
        *rest = skip(tok, ")");
        return val;
    }

    if (tok->kind == TK_NUM)
    {
        *rest = tok->next;
        return tok->val.i64;
    }

    // Handle 'defined' operator (can appear after macro expansion)
    if (tok->kind == TK_IDENT && equal(tok, "defined"))
    {
        tok = tok->next;
        bool has_paren = equal(tok, "(");
        if (has_paren)
            tok = tok->next;
        // If the argument was already macro-expanded to a non-identifier,
        // this is technically undefined behavior per C standard.
        // We treat it as "not defined" (return 0).
        int64_t result = 0;
        if (tok->kind == TK_IDENT)
        {
            Macro *m = find_macro(tok);
            result = m ? 1 : 0;
        }
        tok = tok->next;
        if (has_paren && equal(tok, ")"))
            tok = tok->next;
        *rest = tok;
        return result;
    }

    // Undefined identifier is zero
    *rest = tok->next;
    return 0;
}

static int64_t eval_unary(Token **rest, Token *tok)
{
    if (++pp_eval_depth > PP_MAX_EVAL_DEPTH)
        error_tok(tok, "preprocessor expression too deeply nested (max %d)", PP_MAX_EVAL_DEPTH);

    int64_t result;
    if (equal(tok, "+"))
        result = eval_unary(rest, tok->next);
    else if (equal(tok, "-"))
        result = -eval_unary(rest, tok->next);
    else if (equal(tok, "!"))
        result = !eval_unary(rest, tok->next);
    else if (equal(tok, "~"))
        result = ~eval_unary(rest, tok->next);
    else
        result = eval_primary(rest, tok);

    pp_eval_depth--;
    return result;
}

static int64_t eval_mul(Token **rest, Token *tok)
{
    int64_t val = eval_unary(&tok, tok);
    for (;;)
    {
        if (equal(tok, "*"))
        {
            int64_t rhs = eval_unary(&tok, tok->next);
            if (!pp_eval_skip)
                val *= rhs;
            continue;
        }
        if (equal(tok, "/"))
        {
            Token *op = tok;
            int64_t rhs = eval_unary(&tok, tok->next);
            if (!pp_eval_skip)
            {
                if (rhs == 0)
                    error_tok(op, "division by zero in preprocessor expression");
                val /= rhs;
            }
            continue;
        }
        if (equal(tok, "%"))
        {
            Token *op = tok;
            int64_t rhs = eval_unary(&tok, tok->next);
            if (!pp_eval_skip)
            {
                if (rhs == 0)
                    error_tok(op, "division by zero in preprocessor expression");
                val %= rhs;
            }
            continue;
        }
        *rest = tok;
        return val;
    }
}

static int64_t eval_add(Token **rest, Token *tok)
{
    int64_t val = eval_mul(&tok, tok);
    for (;;)
    {
        if (equal(tok, "+"))
        {
            val += eval_mul(&tok, tok->next);
            continue;
        }
        if (equal(tok, "-"))
        {
            val -= eval_mul(&tok, tok->next);
            continue;
        }
        *rest = tok;
        return val;
    }
}

static int64_t eval_shift(Token **rest, Token *tok)
{
    int64_t val = eval_add(&tok, tok);
    for (;;)
    {
        if (equal(tok, "<<"))
        {
            Token *op = tok;
            int64_t rhs = eval_add(&tok, tok->next);
            if (!pp_eval_skip)
            {
                if (rhs < 0 || rhs >= 64)
                    error_tok(op, "shift amount %ld is out of range (0-63)", rhs);
                val <<= rhs;
            }
            continue;
        }
        if (equal(tok, ">>"))
        {
            Token *op = tok;
            int64_t rhs = eval_add(&tok, tok->next);
            if (!pp_eval_skip)
            {
                if (rhs < 0 || rhs >= 64)
                    error_tok(op, "shift amount %ld is out of range (0-63)", rhs);
                val >>= rhs;
            }
            continue;
        }
        *rest = tok;
        return val;
    }
}

static int64_t eval_relational(Token **rest, Token *tok)
{
    int64_t val = eval_shift(&tok, tok);
    for (;;)
    {
        if (equal(tok, "<"))
        {
            val = val < eval_shift(&tok, tok->next);
            continue;
        }
        if (equal(tok, "<="))
        {
            val = val <= eval_shift(&tok, tok->next);
            continue;
        }
        if (equal(tok, ">"))
        {
            val = val > eval_shift(&tok, tok->next);
            continue;
        }
        if (equal(tok, ">="))
        {
            val = val >= eval_shift(&tok, tok->next);
            continue;
        }
        *rest = tok;
        return val;
    }
}

static int64_t eval_equality(Token **rest, Token *tok)
{
    int64_t val = eval_relational(&tok, tok);
    for (;;)
    {
        if (equal(tok, "=="))
        {
            val = val == eval_relational(&tok, tok->next);
            continue;
        }
        if (equal(tok, "!="))
        {
            val = val != eval_relational(&tok, tok->next);
            continue;
        }
        *rest = tok;
        return val;
    }
}

static int64_t eval_bitand(Token **rest, Token *tok)
{
    int64_t val = eval_equality(&tok, tok);
    while (equal(tok, "&"))
        val &= eval_equality(&tok, tok->next);
    *rest = tok;
    return val;
}

static int64_t eval_bitxor(Token **rest, Token *tok)
{
    int64_t val = eval_bitand(&tok, tok);
    while (equal(tok, "^"))
        val ^= eval_bitand(&tok, tok->next);
    *rest = tok;
    return val;
}

static int64_t eval_bitor(Token **rest, Token *tok)
{
    int64_t val = eval_bitxor(&tok, tok);
    while (equal(tok, "|"))
        val |= eval_bitxor(&tok, tok->next);
    *rest = tok;
    return val;
}

static int64_t eval_logand(Token **rest, Token *tok)
{
    int64_t val = eval_bitor(&tok, tok);
    while (equal(tok, "&&"))
    {
        tok = tok->next;
        bool was_skip = pp_eval_skip;
        if (!val)
            pp_eval_skip = true;
        int64_t rhs = eval_bitor(&tok, tok);
        pp_eval_skip = was_skip;
        val = val && rhs;
    }
    *rest = tok;
    return val;
}

static int64_t eval_logor(Token **rest, Token *tok)
{
    int64_t val = eval_logand(&tok, tok);
    while (equal(tok, "||"))
    {
        tok = tok->next;
        bool was_skip = pp_eval_skip;
        if (val)
            pp_eval_skip = true;
        int64_t rhs = eval_logand(&tok, tok);
        pp_eval_skip = was_skip;
        val = val || rhs;
    }
    *rest = tok;
    return val;
}

static int64_t eval_ternary(Token **rest, Token *tok)
{
    int64_t cond = eval_logor(&tok, tok);
    if (!equal(tok, "?"))
    {
        *rest = tok;
        return cond;
    }
    tok = tok->next;
    bool was_skip = pp_eval_skip;
    if (!cond)
        pp_eval_skip = true; // Skip 'then' branch
    int64_t then = eval_ternary(&tok, tok);
    pp_eval_skip = was_skip;
    tok = skip(tok, ":");
    if (cond)
        pp_eval_skip = true; // Skip 'else' branch
    int64_t els = eval_ternary(&tok, tok);
    pp_eval_skip = was_skip;
    *rest = tok;
    return cond ? then : els;
}

static int64_t eval_const_expr(Token **rest, Token *tok)
{
    return eval_ternary(rest, tok);
}

static Token *read_const_expr(Token **rest, Token *tok)
{
    Token *start = tok;
    Token head = {};
    Token *cur = &head;

    while (!tok_at_bol(tok))
    {
        if (equal(tok, "defined"))
        {
            Token *def = tok;
            tok = tok->next;
            bool has_paren = consume(&tok, tok, "(");
            if (tok->kind != TK_IDENT)
                error_tok(tok, "expected identifier");
            Macro *m = find_macro(tok);
            tok = tok->next;
            if (has_paren)
                tok = skip(tok, ")");

            cur = cur->next = new_num_token(m ? 1 : 0, def);
            continue;
        }

        // Handle __has_include(<file>) or __has_include("file")
        if (equal(tok, "__has_include") || equal(tok, "__has_include_next"))
        {
            Token *start_tok = tok;
            bool is_include_next = equal(tok, "__has_include_next");
            tok = tok->next;
            tok = skip(tok, "(");

            // Read the filename - can be <...> or "..."
            bool found = false;
            if (equal(tok, "<"))
            {
                // Angle bracket include
                char *filename = NULL;
                tok = tok->next;
                // Collect tokens until >
                while (!equal(tok, ">") && !tok_at_bol(tok) && tok->kind != TK_EOF)
                {
                    if (!filename)
                    {
                        filename = format("%.*s", tok->len, tok->loc);
                    }
                    else
                    {
                        char *old = filename;
                        filename = format("%s%.*s", filename, tok->len, tok->loc);
                        free(old);
                    }
                    tok = tok->next;
                }
                if (equal(tok, ">"))
                    tok = tok->next;
                if (filename)
                {
                    if (is_include_next)
                    {
                        // For __has_include_next, search only paths after current file
                        File *cur_file = tok_file(start_tok);
                        char *cur_real = cur_file ? realpath(cur_file->name, NULL) : NULL;
                        int matched_idx = -1;
                        size_t matched_len = 0;
                        char *matched_real = NULL;

                        for (int i = 0; i < pp_include_paths_count; i++)
                        {
                            char *inc_real = realpath(pp_include_paths[i], NULL);
                            if (!inc_real)
                                continue;
                            size_t len = strlen(inc_real);
                            if (cur_real && strncmp(cur_real, inc_real, len) == 0 &&
                                (cur_real[len] == '/' || cur_real[len] == '\0'))
                            {
                                if (len > matched_len)
                                {
                                    if (matched_real)
                                        free(matched_real);
                                    matched_real = inc_real;
                                    matched_len = len;
                                    matched_idx = i;
                                }
                                else
                                {
                                    free(inc_real);
                                }
                                continue;
                            }
                            free(inc_real);
                        }
                        if (cur_real)
                            free(cur_real);

                        // Search only after the matched path (if found)
                        // Match #include_next behavior: system headers search system paths only
                        char *path = NULL;
                        if (matched_idx >= 0)
                        {
                            bool is_user_path = (matched_idx >= pp_user_paths_start);
                            if (is_user_path)
                            {
                                // User path: search remaining user paths, then system paths
                                path = search_include_next_range(filename, matched_idx + 1, pp_include_paths_count, matched_real);
                                if (!path)
                                    path = search_include_next_range(filename, 0, pp_user_paths_start, matched_real);
                            }
                            else
                            {
                                // System path: only search remaining system paths
                                path = search_include_next_range(filename, matched_idx + 1, pp_user_paths_start, matched_real);
                            }
                        }
                        if (matched_real)
                            free(matched_real);
                        found = (path != NULL);
                        if (path)
                            free(path);
                    }
                    else
                    {
                        char *path = search_include_paths(filename);
                        found = (path != NULL);
                        if (path)
                            free(path);
                    }
                    free(filename);
                }
            }
            else if (tok->kind == TK_STR)
            {
                // Quoted include
                char *filename = strndup(tok->loc + 1, tok->len - 2);
                tok = tok->next;
                struct stat st;
                found = (stat(filename, &st) == 0);
                if (!found)
                {
                    if (is_include_next)
                    {
                        // For __has_include_next, search only paths after current file
                        File *cur_file = tok_file(start_tok);
                        char *cur_real = cur_file ? realpath(cur_file->name, NULL) : NULL;
                        int matched_idx = -1;
                        size_t matched_len = 0;
                        char *matched_real = NULL;

                        for (int i = 0; i < pp_include_paths_count; i++)
                        {
                            char *inc_real = realpath(pp_include_paths[i], NULL);
                            if (!inc_real)
                                continue;
                            size_t len = strlen(inc_real);
                            if (cur_real && strncmp(cur_real, inc_real, len) == 0 &&
                                (cur_real[len] == '/' || cur_real[len] == '\0'))
                            {
                                if (len > matched_len)
                                {
                                    if (matched_real)
                                        free(matched_real);
                                    matched_real = inc_real;
                                    matched_len = len;
                                    matched_idx = i;
                                }
                                else
                                {
                                    free(inc_real);
                                }
                                continue;
                            }
                            free(inc_real);
                        }
                        if (cur_real)
                            free(cur_real);

                        char *path = NULL;
                        if (matched_idx >= 0)
                        {
                            bool is_user_path = (matched_idx >= pp_user_paths_start);
                            if (is_user_path)
                            {
                                path = search_include_next_range(filename, matched_idx + 1, pp_include_paths_count, matched_real);
                                if (!path)
                                    path = search_include_next_range(filename, 0, pp_user_paths_start, matched_real);
                            }
                            else
                            {
                                path = search_include_next_range(filename, matched_idx + 1, pp_user_paths_start, matched_real);
                            }
                        }
                        if (matched_real)
                            free(matched_real);
                        found = (path != NULL);
                        if (path)
                            free(path);
                    }
                    else
                    {
                        char *path = search_include_paths(filename);
                        found = (path != NULL);
                        if (path)
                            free(path);
                    }
                }
                free(filename);
            }
            tok = skip(tok, ")");
            cur = cur->next = new_num_token(found ? 1 : 0, start_tok);
            continue;
        }

        cur = cur->next = copy_token(tok);
        tok = tok->next;
    }

    cur->next = new_eof(tok);
    *rest = tok;
    return head.next;
}

static CondIncl *push_cond_incl(Token *tok, bool included)
{
    CondIncl *ci = calloc(1, sizeof(CondIncl));
    if (!ci)
    {
        fprintf(stderr, "out of memory in push_cond_incl\n");
        exit(1);
    }
    ci->next = cond_incl;
    ci->ctx = IN_THEN;
    ci->tok = tok;
    ci->included = included;
    cond_incl = ci;
    return ci;
}

static Token *skip_cond_incl2(Token *tok)
{
    while (tok->kind != TK_EOF)
    {
        if (is_hash(tok) &&
            (equal(tok->next, "if") || equal(tok->next, "ifdef") ||
             equal(tok->next, "ifndef")))
        {
            tok = skip_cond_incl2(tok->next->next);
            continue;
        }
        if (is_hash(tok) && equal(tok->next, "endif"))
            return tok->next->next;
        tok = tok->next;
    }
    return tok;
}

static Token *skip_cond_incl(Token *tok)
{
    while (tok->kind != TK_EOF)
    {
        if (is_hash(tok) &&
            (equal(tok->next, "if") || equal(tok->next, "ifdef") ||
             equal(tok->next, "ifndef")))
        {
            tok = skip_cond_incl2(tok->next->next);
            continue;
        }
        if (is_hash(tok) &&
            (equal(tok->next, "elif") || equal(tok->next, "else") ||
             equal(tok->next, "endif")))
            break;
        tok = tok->next;
    }
    return tok;
}

static Token *preprocess2(Token *tok)
{
    Token head = {};
    Token *cur = &head;

    while (tok->kind != TK_EOF)
    {
        if (expand_macro(&tok, tok))
            continue;

        if (!is_hash(tok))
        {
            // Only add tokens to output if not inside a virtual include
            if (!in_virtual_include)
                cur = cur->next = tok;
            tok = tok->next;
            continue;
        }

        Token *start = tok;
        tok = tok->next;

        // Null directive: just # on a line by itself (or # followed by only whitespace)
        if (tok_at_bol(tok))
            continue;

        if (equal(tok, "include"))
        {
            bool is_dquote;
            char *filename = read_include_filename(&tok, tok->next, &is_dquote);

            // For angle-bracket includes, check if file exists in user-specified paths first
            // If found there, inline it; otherwise process virtually and pass through
            if (!is_dquote)
            {
                char *user_path = search_user_include_paths(filename);
                if (user_path)
                {
                    // Found in user include paths
                    Token *user_tok = tokenize_file(user_path);
                    if (!user_tok)
                        error_tok(start, "%s: cannot open file", filename);

                    bool has_include_next = header_contains_include_next(user_tok);
                    if (is_config_header(filename) || has_include_next)
                    {
                        // Process config.h virtually but emit include for compiler
                        if (!hashmap_get(&virtual_includes, user_path))
                        {
                            hashmap_put(&virtual_includes, user_path, (void *)1);
                            bool was_virtual = in_virtual_include;
                            in_virtual_include = true;
                            preprocess2(user_tok);
                            in_virtual_include = was_virtual;
                        }

                        tok = skip_line(tok);
                        Token *directive = emit_include_directive(start, tok, filename, false);
                        if (!in_virtual_include)
                            cur = cur->next = directive;
                        continue;
                    }

                    // Inline it
                    tok = skip_line(tok);
                    tok = append(user_tok, tok);
                    continue;
                }

                // System header: find in system paths, process virtually for macros
                char *sys_path = search_include_paths(filename);
                if (sys_path)
                {
                    if (!hashmap_get(&virtual_includes, sys_path))
                    {
                        // Mark as virtually included to prevent reprocessing
                        hashmap_put(&virtual_includes, sys_path, (void *)1);

                        // Process the header to learn macros, but discard output
                        Token *sys_tok = tokenize_file(sys_path);
                        if (sys_tok)
                        {
                            bool was_virtual = in_virtual_include;
                            in_virtual_include = true;
                            preprocess2(sys_tok); // Process for macros, output discarded
                            in_virtual_include = was_virtual;
                        }
                    }
                    free(sys_path);
                }

                // Skip to end of line and continue
                tok = skip_line(tok);
                Token *directive = emit_include_directive(start, tok, filename, false);

                // Only emit if not inside a virtual include
                if (!in_virtual_include)
                    cur = cur->next = directive;
                continue;
            }

            char *path = NULL;
            if (is_dquote)
            {
                // Search relative to current file first
                File *start_file = tok_file(start);
                char *name_copy = strdup(start_file->name);
                if (name_copy)
                {
                    char *dir = dirname(name_copy);
                    char *try = format("%s/%s", dir, filename);
                    free(name_copy); // Safe to free after format() made its own copy
                    struct stat st;
                    if (stat(try, &st) == 0)
                        path = try;
                    else
                        free(try);
                }
                // For quoted includes, search user-specified paths (-I) only
                // Don't fall back to system paths - if not found locally, pass through
                if (!path)
                    path = search_user_include_paths(filename);

                // If quoted include not found in current dir or user paths,
                // process virtually and pass through to backend compiler
                if (!path)
                {
                    // Try to find in system paths for virtual processing
                    char *sys_path = search_include_paths(filename);
                    if (sys_path)
                    {
                        if (!hashmap_get(&virtual_includes, sys_path))
                        {
                            hashmap_put(&virtual_includes, sys_path, (void *)1);
                            Token *sys_tok = tokenize_file(sys_path);
                            if (sys_tok)
                            {
                                bool was_virtual = in_virtual_include;
                                in_virtual_include = true;
                                preprocess2(sys_tok);
                                in_virtual_include = was_virtual;
                            }
                        }
                        free(sys_path);
                    }

                    tok = skip_line(tok);
                    Token *directive = emit_include_directive(start, tok, filename, false);
                    if (!in_virtual_include)
                        cur = cur->next = directive;
                    continue;
                }
            }
            else
            {
                // Angle-bracket include: search all paths (user + system)
                path = search_include_paths(filename);
            }
            if (!path)
                error_tok(start, "%s: cannot open file", filename);

            if (is_user_header_path(path))
            {
                Token *user_tok = tokenize_file(path);
                if (!user_tok)
                    error_tok(start, "%s: cannot open file", filename);

                bool has_include_next = header_contains_include_next(user_tok);
                if (is_config_header(filename) || has_include_next)
                {
                    if (!hashmap_get(&virtual_includes, path))
                    {
                        hashmap_put(&virtual_includes, path, (void *)1);
                        bool was_virtual = in_virtual_include;
                        in_virtual_include = true;
                        preprocess2(user_tok);
                        in_virtual_include = was_virtual;
                    }

                    tok = skip_line(tok);
                    Token *directive = emit_include_directive(start, tok, filename, is_dquote);
                    if (!in_virtual_include)
                        cur = cur->next = directive;
                    continue;
                }

                // Inline user header that doesn't use include_next
                tok = skip_line(tok);
                tok = append(user_tok, tok);
                continue;
            }

            tok = skip_line(tok);
            tok = include_file(start, path, tok);
            continue;
        }

        if (equal(tok, "include_next"))
        {
            bool is_dquote;
            char *filename = read_include_filename(&tok, tok->next, &is_dquote);

            // Search from the next include path after where current file was found
            char *path = NULL;
            File *start_file = tok_file(start);
            char *cur_file = start_file->name;

            // Resolve current file to realpath for accurate comparison
            char *cur_real = realpath(cur_file, NULL);

            // Find which include path the current file is under (prefer longest match)
            int matched_idx = -1;
            int matched_is_user = 0;
            size_t matched_len = 0;
            char *matched_real = NULL;
            for (int i = 0; i < pp_include_paths_count; i++)
            {
                char *inc_real = realpath(pp_include_paths[i], NULL);
                if (!inc_real)
                    continue;

                size_t len = strlen(inc_real);
                if (cur_real && strncmp(cur_real, inc_real, len) == 0 &&
                    (cur_real[len] == '/' || cur_real[len] == '\0'))
                {
                    if (len > matched_len)
                    {
                        if (matched_real)
                            free(matched_real);
                        matched_real = inc_real;
                        matched_len = len;
                        matched_idx = i;
                        matched_is_user = (i >= pp_user_paths_start);
                    }
                    else
                    {
                        free(inc_real);
                    }
                    continue;
                }
                free(inc_real);
            }

            if (cur_real)
                free(cur_real);

            if (matched_idx >= 0)
            {
                if (matched_is_user)
                {
                    // Logical search order: remaining user paths, then system paths
                    path = search_include_next_range(filename, matched_idx + 1, pp_include_paths_count, matched_real);
                    bool found_in_user = (path != NULL);
                    if (!path)
                        path = search_include_next_range(filename, 0, pp_user_paths_start, matched_real);

                    if (matched_real)
                        free(matched_real);

                    if (!path)
                        error_tok(start, "%s: cannot find next file (include_next)", filename);

                    tok = skip_line(tok);

                    if (found_in_user)
                    {
                        tok = include_file(start, path, tok);
                        continue;
                    }

                    // System header: process virtually and emit include_next directive
                    if (!hashmap_get(&virtual_includes, path))
                    {
                        hashmap_put(&virtual_includes, path, (void *)1);
                        Token *sys_tok = tokenize_file(path);
                        if (sys_tok)
                        {
                            bool was_virtual = in_virtual_include;
                            in_virtual_include = true;
                            preprocess2(sys_tok);
                            in_virtual_include = was_virtual;
                        }
                    }

                    Token *directive = emit_include_next_directive(start, tok, filename, is_dquote);
                    if (!in_virtual_include)
                        cur = cur->next = directive;
                    continue;
                }
                else
                {
                    // Current file is in a system path: only search remaining system paths
                    path = search_include_next_range(filename, matched_idx + 1, pp_user_paths_start, matched_real);
                }
            }
            else
            {
                // Fallback: Assume we are in a user header wrapping a system header.
                // Search system paths only.
                path = search_include_next_range(filename, 0, pp_user_paths_start, matched_real);
            }

            if (matched_real)
                free(matched_real);
            if (!path)
                error_tok(start, "%s: cannot find next file (include_next)", filename);

            tok = skip_line(tok);
            // For system headers, process virtually and emit include_next directive
            {
                char *inc_real = realpath(path, NULL);
                bool is_system = true;
                if (inc_real)
                {
                    for (int i = pp_user_paths_start; i < pp_include_paths_count; i++)
                    {
                        char *usr_real = realpath(pp_include_paths[i], NULL);
                        if (usr_real && strcmp(usr_real, inc_real) == 0)
                        {
                            is_system = false;
                            free(usr_real);
                            break;
                        }
                        if (usr_real)
                            free(usr_real);
                    }
                    free(inc_real);
                }

                if (!is_system)
                {
                    tok = include_file(start, path, tok);
                    continue;
                }

                if (!hashmap_get(&virtual_includes, path))
                {
                    hashmap_put(&virtual_includes, path, (void *)1);
                    Token *sys_tok = tokenize_file(path);
                    if (sys_tok)
                    {
                        bool was_virtual = in_virtual_include;
                        in_virtual_include = true;
                        preprocess2(sys_tok);
                        in_virtual_include = was_virtual;
                    }
                }

                Token *directive = emit_include_next_directive(start, tok, filename, is_dquote);
                if (!in_virtual_include)
                    cur = cur->next = directive;
                continue;
            }
        }

        if (equal(tok, "define"))
        {
            Token *define_start = start;
            Token *name_tok = tok->next;

            // Check if this is an object-like macro (no paren immediately after name)
            bool is_objlike = true;
            if (name_tok && name_tok->kind == TK_IDENT)
            {
                Token *after_name = name_tok->next;
                if (after_name && !tok_has_space(after_name) && equal(after_name, "("))
                    is_objlike = false;
            }

            // Save macro name before processing
            char *macro_name = NULL;
            int macro_name_len = 0;
            if (name_tok && name_tok->kind == TK_IDENT)
            {
                macro_name = strndup(name_tok->loc, name_tok->len);
                macro_name_len = name_tok->len;
            }

            read_macro_definition(&tok, tok->next);

            // Emit guard macros when not in virtual include mode
            // These are needed for passed-through headers to work correctly
            if (!in_virtual_include && is_objlike && macro_name &&
                is_passthrough_guard_macro(macro_name, macro_name_len))
            {
                Macro *m = hashmap_get(&macros, macro_name);
                if (m && m->is_objlike)
                {
                    char *body_str = macro_body_to_string(m->body);
                    if (body_str)
                    {
                        Token *directive = emit_define_directive(define_start, tok, macro_name, body_str);
                        cur = cur->next = directive;
                        free(body_str);
                    }
                }
            }
            if (macro_name)
                free(macro_name);
            continue;
        }

        if (equal(tok, "undef"))
        {
            tok = tok->next;
            if (tok->kind != TK_IDENT)
                error_tok(tok, "expected identifier");
            // Free the macro struct before removing from hashmap
            Macro *m = hashmap_get2(&macros, tok->loc, tok->len);
            if (m)
                free_macro(m);
            hashmap_delete2(&macros, tok->loc, tok->len);
            tok = skip_line(tok->next);
            continue;
        }

        if (equal(tok, "if"))
        {
            Token *expr = read_const_expr(&tok, tok->next);
            // Expression preprocessing should not be affected by virtual include mode
            // BUT should allow virtual function-like macros to expand (for UINT64_MAX etc.)
            bool save_virtual = in_virtual_include;
            bool save_pp_const = in_pp_const_expr;
            in_virtual_include = false;
            in_pp_const_expr = true;
            expr = preprocess2(expr);
            in_virtual_include = save_virtual;
            in_pp_const_expr = save_pp_const;
            convert_pp_tokens(expr);
            int64_t val = eval_const_expr(&expr, expr);

            push_cond_incl(start, val);
            if (!val)
                tok = skip_cond_incl(tok);
            continue;
        }

        if (equal(tok, "ifdef"))
        {
            tok = tok->next;
            if (tok->kind != TK_IDENT)
                error_tok(tok, "expected identifier");
            bool defined = find_macro(tok);
            push_cond_incl(tok, defined);
            tok = skip_line(tok->next);
            if (!defined)
                tok = skip_cond_incl(tok);
            continue;
        }

        if (equal(tok, "ifndef"))
        {
            tok = tok->next;
            if (tok->kind != TK_IDENT)
                error_tok(tok, "expected identifier");
            bool defined = find_macro(tok);
            push_cond_incl(tok, !defined);
            tok = skip_line(tok->next);
            if (defined)
                tok = skip_cond_incl(tok);
            continue;
        }

        if (equal(tok, "elif"))
        {
            if (!cond_incl || cond_incl->ctx == IN_ELSE)
                error_tok(tok, "stray #elif");
            cond_incl->ctx = IN_ELIF;

            if (!cond_incl->included)
            {
                Token *expr = read_const_expr(&tok, tok->next);
                bool save_virtual = in_virtual_include;
                bool save_pp_const = in_pp_const_expr;
                in_virtual_include = false;
                in_pp_const_expr = true;
                expr = preprocess2(expr);
                in_virtual_include = save_virtual;
                in_pp_const_expr = save_pp_const;
                convert_pp_tokens(expr);
                if (eval_const_expr(&expr, expr))
                {
                    cond_incl->included = true;
                    continue;
                }
            }
            tok = skip_cond_incl(tok); // Fixed: was tok->next
            continue;
        }

        if (equal(tok, "else"))
        {
            if (!cond_incl || cond_incl->ctx == IN_ELSE)
                error_tok(tok, "stray #else");
            cond_incl->ctx = IN_ELSE;
            tok = skip_line(tok->next);
            if (cond_incl->included)
                tok = skip_cond_incl(tok);
            continue;
        }

        if (equal(tok, "endif"))
        {
            if (!cond_incl)
                error_tok(tok, "stray #endif");
            cond_incl = cond_incl->next;
            tok = skip_line(tok->next);
            continue;
        }

        if (equal(tok, "error"))
            error_tok(tok, "#error");

        if (equal(tok, "pragma"))
        {
            if (equal(tok->next, "once"))
            {
                // Mark this file as pragma once
                File *start_file = tok_file(start);
                char *real = realpath(start_file->name, NULL);
                char *key = real ? real : start_file->name;
                hashmap_put(&pragma_once_files, key, (void *)1);
                if (real)
                    free(real);
            }
            tok = skip_line_quiet(tok->next);
            continue;
        }

        if (equal(tok, "warning"))
        {
            // Just skip #warning, optionally print it
            tok = skip_line(tok->next);
            continue;
        }

        if (equal(tok, "line"))
        {
            // #line has format: #line linenum ["filename"]
            // Skip the entire line quietly since the filename is optional
            tok = skip_line_quiet(tok->next);
            continue;
        }

        if (equal(tok, "ident") || equal(tok, "sccs"))
        {
            // #ident and #sccs are ignored (GCC extension)
            tok = skip_line_quiet(tok->next);
            continue;
        }

        if (tok->kind == TK_PP_NUM)
        {
            // Line directive like: # 1 "filename" [flags]
            // This is GCC's line marker format - skip quietly
            tok = skip_line_quiet(tok);
            continue;
        }

        error_tok(start, "invalid preprocessor directive");
    }

    cur->next = tok;
    return head.next;
}

void pp_define_macro(char *name, char *val)
{
    Token *tok = tokenize(new_file("<built-in>", 0, format("%s %s\n", name, val)));
    // Skip the macro name token - the body starts after it
    tok = tok->next;
    add_macro(name, true, tok, NULL);
}

// Check if a macro is defined and return its value (or "1" for macros without value)
// Returns NULL if macro is not defined
// Note: For simple macros, returns pointer into source buffer (valid during transpile).
// The returned string may not be null-terminated at the value boundary.
// Caller should use the returned length if precision is needed.
const char *pp_get_macro_value(const char *name)
{
    Macro *m = find_macro_by_name((char *)name);
    if (!m)
        return NULL;

    // If macro has a body with a simple value, return "1"
    // (We return "1" for any defined macro since we mainly care about definedness)
    // For NDEBUG specifically, the value doesn't matter - just that it's defined
    return "1";
}

// Define a full macro (supports function-like macros)
// Usage: pp_define_full("__has_feature(x) 0")
static void pp_define_full(char *def)
{
    Token *tok = tokenize(new_file("<built-in>", 0, format("%s\n", def)));
    read_macro_definition(&tok, tok);
}

// Extract predefined macros from the system compiler
static int pp_import_system_macros(void)
{
    // Build command with user-specified compiler and flags
    // This ensures flags like -std=c99, -m32, -D_GNU_SOURCE affect macro definitions
    char *cmd = build_compiler_cmd("-dM -E - 2>/dev/null");
    if (!cmd)
        return -1;

    // Prepend echo with headers and pipe to compiler
    char full_cmd[4096];
    snprintf(full_cmd, sizeof(full_cmd),
             "echo '#include <features.h>\n#include <signal.h>\n#include <limits.h>\n#include <unistd.h>' | %s",
             cmd);
    free(cmd);

    FILE *fp = popen(full_cmd, "r");
    if (!fp)
        return -1;

    char line[4096];
    int count = 0;

    while (fgets(line, sizeof(line), fp))
    {
        // Each line is: #define NAME VALUE
        // or: #define NAME(args) VALUE
        if (strncmp(line, "#define ", 8) != 0)
            continue;

        char *start = line + 8; // Skip "#define "

        // Find end of macro name (space, '(', or newline)
        char *end = start;
        while (*end && *end != ' ' && *end != '(' && *end != '\n' && *end != '\r')
            end++;

        if (end == start)
            continue; // Empty name

        int name_len = end - start;

        // Skip macros that we handle specially (dynamic values)
        if ((name_len == 8 && strncmp(start, "__FILE__", 8) == 0) ||
            (name_len == 8 && strncmp(start, "__LINE__", 8) == 0) ||
            (name_len == 11 && strncmp(start, "__COUNTER__", 11) == 0) ||
            (name_len == 8 && strncmp(start, "__DATE__", 8) == 0) ||
            (name_len == 8 && strncmp(start, "__TIME__", 8) == 0) ||
            (name_len == 13 && strncmp(start, "__TIMESTAMP__", 13) == 0) ||
            (name_len == 13 && strncmp(start, "__BASE_FILE__", 13) == 0))
            continue;

        // For function-like macros (has '('), use pp_define_full
        // For object-like macros, use pp_define_macro
        if (*end == '(')
        {
            // Find the full definition (remove trailing newline)
            char *nl = strchr(start, '\n');
            if (nl)
                *nl = '\0';
            nl = strchr(start, '\r');
            if (nl)
                *nl = '\0';

            pp_define_full(start);
        }
        else
        {
            // Object-like macro
            char name[256];
            if (name_len >= (int)sizeof(name))
                continue;
            memcpy(name, start, name_len);
            name[name_len] = '\0';

            // Get value (skip space after name)
            char *value = end;
            while (*value == ' ' || *value == '\t')
                value++;

            // Remove trailing newline
            char *nl = strchr(value, '\n');
            if (nl)
                *nl = '\0';
            nl = strchr(value, '\r');
            if (nl)
                *nl = '\0';

            // Empty value means define as empty string
            if (*value == '\0')
                pp_define_macro(name, "");
            else
                pp_define_macro(name, value);
        }
        count++;
    }

    pclose(fp);
    return count;
}

// Reset preprocessor state - call before processing a new file to avoid state leakage
void pp_reset(void)
{
    // Free macro values before clearing the hashmap
    free_all_macros();
    hashmap_clear(&macros);
    hashmap_clear(&pragma_once_files);
    hashmap_clear(&guarded_files);
    hashmap_clear(&virtual_includes);
    hashmap_clear(&token_hidesets);
    hashmap_clear(&token_origins);
    cond_incl = NULL;
    pp_eval_skip = false;
    in_virtual_include = false;
    in_pp_const_expr = false;
    reset_feature_test_macros();
    // Free input files (file contents, line tables, etc.)
    free_input_files();
    arena_reset(); // Reset token arena for reuse
}

void pp_init(void)
{
    pp_reset();

    if (!keyword_map.capacity)
        init_keyword_map();

    if (!feature_test_macro_set.capacity)
    {
        for (int i = 0; feature_test_macro_names[i]; i++)
            hashmap_put(&feature_test_macro_set, (char *)feature_test_macro_names[i], (void *)1);
    }

    // Import all macros from the system compiler (includes features.h, signal.h, limits.h, unistd.h)
    pp_import_system_macros();

    // Dynamic builtins that depend on current file/line
    add_builtin("__FILE__", file_macro);
    add_builtin("__LINE__", line_macro);

    // GCC extension - make it expand to nothing
    if (!hashmap_get(&macros, "__extension__"))
        pp_define_macro("__extension__", "");

    // Clang feature-check macros - need defaults if not provided by system compiler
    if (!hashmap_get(&macros, "__has_feature"))
        pp_define_full("__has_feature(x) 0");
    if (!hashmap_get(&macros, "__has_extension"))
        pp_define_full("__has_extension(x) 0");
    if (!hashmap_get(&macros, "__has_builtin"))
        pp_define_full("__has_builtin(x) 0");
    if (!hashmap_get(&macros, "__has_warning"))
        pp_define_full("__has_warning(x) 0");
    if (!hashmap_get(&macros, "__building_module"))
        pp_define_full("__building_module(x) 0");

#ifdef __APPLE__
    // Apple availability attributes - make them expand to nothing if not already defined
    if (!hashmap_get(&macros, "__API_AVAILABLE"))
        pp_define_full("__API_AVAILABLE(...) ");
    if (!hashmap_get(&macros, "__API_UNAVAILABLE"))
        pp_define_full("__API_UNAVAILABLE(...) ");
    if (!hashmap_get(&macros, "__API_DEPRECATED"))
        pp_define_full("__API_DEPRECATED(...) ");
    if (!hashmap_get(&macros, "__API_DEPRECATED_WITH_REPLACEMENT"))
        pp_define_full("__API_DEPRECATED_WITH_REPLACEMENT(...) ");
    if (!hashmap_get(&macros, "__OSX_AVAILABLE"))
        pp_define_full("__OSX_AVAILABLE(...) ");
    if (!hashmap_get(&macros, "__IOS_AVAILABLE"))
        pp_define_full("__IOS_AVAILABLE(...) ");
    if (!hashmap_get(&macros, "__TVOS_AVAILABLE"))
        pp_define_full("__TVOS_AVAILABLE(...) ");
    if (!hashmap_get(&macros, "__WATCHOS_AVAILABLE"))
        pp_define_full("__WATCHOS_AVAILABLE(...) ");
    if (!hashmap_get(&macros, "__OSX_AVAILABLE_STARTING"))
        pp_define_full("__OSX_AVAILABLE_STARTING(...) ");
    if (!hashmap_get(&macros, "__OSX_AVAILABLE_BUT_DEPRECATED"))
        pp_define_full("__OSX_AVAILABLE_BUT_DEPRECATED(...) ");
    if (!hashmap_get(&macros, "__OSX_DEPRECATED"))
        pp_define_full("__OSX_DEPRECATED(...) ");
    if (!hashmap_get(&macros, "__IOS_PROHIBITED"))
        pp_define_full("__IOS_PROHIBITED ");
    if (!hashmap_get(&macros, "__TVOS_PROHIBITED"))
        pp_define_full("__TVOS_PROHIBITED ");
    if (!hashmap_get(&macros, "__WATCHOS_PROHIBITED"))
        pp_define_full("__WATCHOS_PROHIBITED ");
    if (!hashmap_get(&macros, "__OSX_EXTENSION_UNAVAILABLE"))
        pp_define_macro("__OSX_EXTENSION_UNAVAILABLE", "");
    if (!hashmap_get(&macros, "__IOS_EXTENSION_UNAVAILABLE"))
        pp_define_macro("__IOS_EXTENSION_UNAVAILABLE", "");
    if (!hashmap_get(&macros, "_API_AVAILABLE"))
        pp_define_full("_API_AVAILABLE(...) ");
    if (!hashmap_get(&macros, "__SWIFT_UNAVAILABLE_MSG"))
        pp_define_full("__SWIFT_UNAVAILABLE_MSG(...) ");
    if (!hashmap_get(&macros, "__SWIFT_UNAVAILABLE"))
        pp_define_macro("__SWIFT_UNAVAILABLE", "");
#endif
}

Token *preprocess(Token *tok)
{
    tok = preprocess2(tok);
    if (cond_incl)
        error_tok(cond_incl->tok, "unterminated conditional directive");
    convert_pp_tokens(tok);
    return tok;
}
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

typedef struct Type Type;
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
} File;

// Token kinds
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

// Token
struct Token
{
    TokenKind kind;
    Token *next;
    int64_t val;
    long double fval;
    char *loc;
    int len;
    Type *ty;
    char *str;
    File *file;
    char *filename;
    int line_no;
    int line_delta;
    bool at_bol;
    bool has_space;
    Hideset *hideset;
    Token *origin;
};

// Minimal type info (only for string literals)
typedef enum
{
    TY_VOID,
    TY_BOOL,
    TY_CHAR,
    TY_SHORT,
    TY_INT,
    TY_LONG,
    TY_FLOAT,
    TY_DOUBLE,
    TY_LDOUBLE,
    TY_PTR,
    TY_ARRAY,
} TypeKind;

struct Type
{
    TypeKind kind;
    int size;
    int align;
    bool is_unsigned;
    Type *base;
    int array_len;
};

static Type ty_char_val = {TY_CHAR, 1, 1, false};
static Type ty_int_val = {TY_INT, 4, 4, false};

static Type *ty_char = &ty_char_val;
static Type *ty_int = &ty_int_val;

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

// Error handling
static File *current_file;

// Tokenizer
static File **input_files;
static int input_file_count;
static bool at_bol;
static bool has_space;

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
    bool is_va_args;
    Token *tok;
};

typedef Token *macro_handler_fn(Token *);

typedef struct Macro Macro;
struct Macro
{
    char *name;
    bool is_objlike;
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
static HashMap included_files;
static bool pp_eval_skip = false;

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
    for (int i = 0; feature_test_macro_names[i]; i++)
    {
        if (strlen(feature_test_macro_names[i]) == (size_t)len &&
            strncmp(feature_test_macro_names[i], name, len) == 0)
            return true;
    }
    return false;
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
        if (t->has_space && pos > 0)
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

static Type *array_of(Type *base, int len)
{
    Type *ty = calloc(1, sizeof(Type));
    if (!ty)
    {
        fprintf(stderr, "out of memory in array_of\n");
        exit(1);
    }
    ty->kind = TY_ARRAY;
    ty->base = base;
    // Check for integer overflow
    if (len > 0 && base->size > INT_MAX / len)
    {
        fprintf(stderr, "array size overflow\n");
        exit(1);
    }
    ty->size = base->size * len;
    ty->array_len = len;
    return ty;
}

// Hideset for macro expansion
struct Hideset
{
    Hideset *next;
    char *name;
};

static void pp_add_include_path(const char *path)
{
    char **new_paths = realloc(pp_include_paths, sizeof(char *) * (pp_include_paths_count + 1));
    if (!new_paths)
    {
        fprintf(stderr, "out of memory adding include path\n");
        exit(1);
    }
    pp_include_paths = new_paths;
    char *dup = strdup(path);
    if (!dup)
    {
        fprintf(stderr, "out of memory duplicating include path\n");
        exit(1);
    }
    pp_include_paths[pp_include_paths_count++] = dup;
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

#ifdef __APPLE__
    // macOS: Get Clang resource dir for built-in headers (stdarg.h, etc.)
    FILE *fp = popen("clang -print-resource-dir 2>/dev/null", "r");
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
    char *buf;
    size_t buflen;
    FILE *fp = open_memstream(&buf, &buflen);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fclose(fp);
    if (buflen > FORMAT_MAX_SIZE)
    {
        fprintf(stderr, "formatted string too large (%zu bytes, max %d)\n", buflen, FORMAT_MAX_SIZE);
        exit(1);
    }
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
            ent->key = key;
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
        ent->key = TOMBSTONE;
}

static void hashmap_delete(HashMap *map, char *key)
{
    hashmap_delete2(map, key, strlen(key));
}

static void hashmap_clear(HashMap *map)
{
    if (map->buckets)
    {
        free(map->buckets);
        map->buckets = NULL;
    }
    map->capacity = 0;
    map->used = 0;
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
    verror_at(tok->file->name, tok->file->contents, tok->line_no, tok->loc, fmt, ap);
    exit(1);
}

static void warn_tok(Token *tok, char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verror_at(tok->file->name, tok->file->contents, tok->line_no, tok->loc, fmt, ap);
    va_end(ap);
}

static bool equal(Token *tok, char *op)
{
    return tok->len == strlen(op) && !memcmp(tok->loc, op, tok->len);
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
    Token *tok = calloc(1, sizeof(Token));
    if (!tok)
    {
        fprintf(stderr, "out of memory in new_token\n");
        exit(1);
    }
    tok->kind = kind;
    tok->loc = start;
    tok->len = end - start;
    tok->file = current_file;
    tok->filename = current_file->display_name;
    tok->at_bol = at_bol;
    tok->has_space = has_space;
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
    static char *kw[] = {
        "<<=",
        ">>=",
        "...",
        "==",
        "!=",
        "<=",
        ">=",
        "->",
        "+=",
        "-=",
        "*=",
        "/=",
        "++",
        "--",
        "%=",
        "&=",
        "|=",
        "^=",
        "&&",
        "||",
        "<<",
        ">>",
        "##",
    };
    for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
        if (startswith(p, kw[i]))
            return strlen(kw[i]);
    return ispunct(*p) ? 1 : 0;
}

static bool is_keyword(Token *tok)
{
    static HashMap map;
    if (!map.capacity)
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
            hashmap_put(&map, kw[i], (void *)1);
    }
    return hashmap_get2(&map, tok->loc, tok->len);
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
    tok->ty = array_of(ty_char, len + 1);
    tok->str = buf;
    return tok;
}

static Token *read_char_literal(char *start, char *quote, Type *ty)
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
        if (count < ty->size)
            val = (val << 8) | (c & 0xFF);
        count++;
        // Multi-character constants are implementation-defined but allowed
        // Continue parsing but truncate value to ty->size bytes
    }

    if (count == 0)
        error_at(start, "empty char literal");

    Token *tok = new_token(TK_NUM, start, p + 1);
    tok->val = (count == 1) ? first_c : (int32_t)val;
    tok->ty = ty;
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
        tok->fval = strtold(buf, &endp);
        if (endp == buf)
            error_tok(tok, "invalid floating constant");
        if (is_hex && !strpbrk(buf, "pP"))
            error_tok(tok, "invalid hex floating constant");
        if (!is_hex && strpbrk(buf, "pP"))
            error_tok(tok, "invalid floating constant");

        if (*endp && endp[1] == '\0' &&
            (*endp == 'f' || *endp == 'F' || *endp == 'l' || *endp == 'L'))
            endp++;
        if (*endp)
            error_tok(tok, "invalid floating constant");
        tok->kind = TK_NUM;
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
    tok->val = val;
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

static void add_line_numbers(Token *tok)
{
    int n = 1;
    for (char *p = current_file->contents; *p; p++)
    {
        if (p == tok->loc)
        {
            tok->line_no = n;
            tok = tok->next;
            if (!tok)
                return;
        }
        if (*p == '\n')
            n++;
    }
}

static File *new_file(char *name, int file_no, char *contents)
{
    File *file = calloc(1, sizeof(File));
    if (!file)
    {
        fprintf(stderr, "out of memory in new_file\n");
        exit(1);
    }
    file->name = name;
    file->display_name = name;
    file->file_no = file_no;
    file->contents = contents;
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
                else if (isalnum(*p) || *p == '.')
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
            cur = cur->next = read_char_literal(p, p, ty_int);
            p += cur->len;
            continue;
        }
        // Wide and unicode character literals: L'...', u'...', U'...'
        if ((*p == 'L' || *p == 'u' || *p == 'U') && p[1] == '\'')
        {
            cur = cur->next = read_char_literal(p, p + 1, ty_int);
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

Token *tokenize_file(char *path)
{
    char *p = read_file(path);
    if (!p)
        return NULL;

    if (!memcmp(p, "\xef\xbb\xbf", 3))
        p += 3; // Skip BOM
    canonicalize_newline(p);
    remove_backslash_newline(p);

    File *file = new_file(path, input_file_count, p);
    File **new_input_files = realloc(input_files, sizeof(File *) * (input_file_count + 2));
    if (!new_input_files)
        error("out of memory loading file");
    input_files = new_input_files;
    input_files[input_file_count++] = file;
    input_files[input_file_count] = NULL;

    return tokenize(file);
}

static Token *preprocess2(Token *tok);

static bool is_hash(Token *tok)
{
    return tok->at_bol && equal(tok, "#");
}

static Token *skip_line(Token *tok)
{
    if (tok->at_bol)
        return tok;
    warn_tok(tok, "extra token");
    while (!tok->at_bol)
        tok = tok->next;
    return tok;
}

// Skip to end of line without warning (for ignored directives like #pragma)
static Token *skip_line_quiet(Token *tok)
{
    while (!tok->at_bol)
        tok = tok->next;
    return tok;
}

static Token *copy_token(Token *tok)
{
    Token *t = calloc(1, sizeof(Token));
    if (!t)
    {
        fprintf(stderr, "out of memory in copy_token\n");
        exit(1);
    }
    *t = *tok;
    t->next = NULL;
    return t;
}

static Token *new_eof(Token *tok)
{
    Token *t = copy_token(tok);
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
    for (; hs; hs = hs->next)
        if (strlen(hs->name) == len && !memcmp(hs->name, s, len))
            return true;
    return false;
}

static Hideset *hideset_intersection(Hideset *hs1, Hideset *hs2)
{
    Hideset head = {};
    Hideset *cur = &head;
    for (; hs1; hs1 = hs1->next)
        if (hideset_contains(hs2, hs1->name, strlen(hs1->name)))
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
        t->hideset = hideset_union(t->hideset, hs);
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

static Macro *find_macro(Token *tok)
{
    if (tok->kind != TK_IDENT)
        return NULL;
    return hashmap_get2(&macros, tok->loc, tok->len);
}

static Macro *add_macro(char *name, bool is_objlike, Token *body)
{
    Macro *m = calloc(1, sizeof(Macro));
    if (!m)
    {
        fprintf(stderr, "out of memory in add_macro\n");
        exit(1);
    }
    m->name = name;
    m->is_objlike = is_objlike;
    m->body = body;
    hashmap_put(&macros, name, m);
    return m;
}

static Token *new_str_token(char *str, Token *tmpl)
{
    char *buf = format("\"%s\"", str);
    return tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

static Token *new_num_token(int val, Token *tmpl)
{
    char *buf = format("%d", val);
    return tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

// Built-in macros
static Token *file_macro(Token *tmpl)
{
    while (tmpl->origin)
        tmpl = tmpl->origin;
    return new_str_token(tmpl->file->display_name, tmpl);
}

static Token *line_macro(Token *tmpl)
{
    while (tmpl->origin)
        tmpl = tmpl->origin;
    return new_num_token(tmpl->line_no + tmpl->file->line_delta, tmpl);
}

static void add_builtin(char *name, macro_handler_fn *fn)
{
    Macro *m = add_macro(name, true, NULL);
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
    for (MacroArg *ap = args; ap; ap = ap->next)
        if (tok->len == strlen(ap->name) && !memcmp(tok->loc, ap->name, tok->len))
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
        if (t != tok && t->has_space)
            fputc(' ', fp);
        // Escape quotes and backslashes in strings
        if (t->kind == TK_STR)
        {
            fputc('"', fp);
            for (int i = 0; i < t->len - 2; i++)
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
    return tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

// Paste two tokens together
static Token *paste(Token *lhs, Token *rhs)
{
    char *buf = format("%.*s%.*s", lhs->len, lhs->loc, rhs->len, rhs->loc);
    Token *tok = tokenize(new_file(lhs->file->name, lhs->file->file_no, format("%s\n", buf)));
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
            // Expand argument (without ##)
            Token *expanded = preprocess2(copy_token_list(arg));
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

static bool expand_macro(Token **rest, Token *tok)
{
    if (hideset_contains(tok->hideset, tok->loc, tok->len))
        return false;

    Macro *m = find_macro(tok);
    if (!m)
        return false;

    if (m->handler)
    {
        *rest = append(m->handler(tok), tok->next);
        return true;
    }

    if (m->is_objlike)
    {
        Hideset *hs = hideset_union(tok->hideset, new_hideset(m->name));
        Token *body = add_hideset(m->body, hs);
        for (Token *t = body; t && t->kind != TK_EOF; t = t->next)
            t->origin = tok;
        *rest = append(body, tok->next);
        return true;
    }

    if (!equal(tok->next, "("))
        return false;

    Token *macro_tok = tok;
    MacroArg *args = read_macro_args(&tok, tok->next->next, m->params, m->va_args_name);
    Token *body = subst(m->body, args);
    Hideset *hs = hideset_intersection(macro_tok->hideset, tok->hideset);
    hs = hideset_union(hs, new_hideset(m->name));
    body = add_hideset(body, hs);
    for (Token *t = body; t && t->kind != TK_EOF; t = t->next)
        t->origin = macro_tok;
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
            if (tok->at_bol || tok->kind == TK_EOF)
                error_tok(tok, "expected '>'");
            if (tok->has_space)
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

static Token *include_file(Token *tok, char *path, Token *cont)
{
    // Resolve to canonical path for deduplication
    char *real = realpath(path, NULL);
    char *key = real ? real : path;

    // Check if already included (via #pragma once or prior include)
    if (hashmap_get(&included_files, key))
    {
        if (real)
            free(real);
        return cont;
    }

    Token *tok2 = tokenize_file(path);
    if (!tok2)
        error_tok(tok, "%s: cannot open file", path);

    if (real)
        free(real);

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
            *va_name = "__VA_ARGS__";
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
    for (; !tok->at_bol; tok = tok->next)
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

    if (!tok->has_space && equal(tok, "("))
    {
        // Function-like macro
        char *va_name = NULL;
        MacroParam *params = read_macro_params(&tok, tok->next, &va_name);
        Macro *m = add_macro(name, false, copy_line(rest, tok));
        m->params = params;
        m->va_args_name = va_name;
        return;
    }

    // Object-like macro
    Token *body = copy_line(rest, tok);

    // Check if this is a feature test macro that needs to be passed to backend
    if (is_feature_test_macro(name, name_len))
        record_feature_test_macro(name, name_len, body);

    add_macro(name, true, body);
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
        return tok->val;
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

    while (!tok->at_bol)
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
                while (!equal(tok, ">") && !tok->at_bol && tok->kind != TK_EOF)
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
                    char *path = search_include_paths(filename);
                    found = (path != NULL);
                    if (path)
                        free(path);
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
                    char *path = search_include_paths(filename);
                    found = (path != NULL);
                    if (path)
                        free(path);
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
            cur = cur->next = tok;
            tok = tok->next;
            continue;
        }

        Token *start = tok;
        tok = tok->next;

        // Null directive: just # on a line by itself (or # followed by only whitespace)
        if (tok->at_bol)
            continue;

        if (equal(tok, "include"))
        {
            bool is_dquote;
            char *filename = read_include_filename(&tok, tok->next, &is_dquote);

            // For angle-bracket includes, check if file exists in user-specified paths first
            // If found there, inline it; otherwise pass through to the backend compiler
            if (!is_dquote)
            {
                char *user_path = search_user_include_paths(filename);
                if (user_path)
                {
                    // Found in user include paths - inline it
                    tok = skip_line(tok);
                    tok = include_file(start, user_path, tok);
                    continue;
                }

                // Not in user paths - preserve #include directive as-is for backend compiler
                Token *hash = copy_token(start);
                hash->kind = TK_PUNCT;
                hash->loc = "#";
                hash->len = 1;
                hash->at_bol = true; // # starts at beginning of line
                hash->has_space = false;

                Token *inc = copy_token(start);
                inc->kind = TK_IDENT;
                inc->loc = "include";
                inc->len = 7;
                inc->at_bol = false;
                inc->has_space = false; // No space between # and include

                Token *open = copy_token(start);
                open->kind = TK_PUNCT;
                open->loc = "<";
                open->len = 1;
                open->at_bol = false;
                open->has_space = true; // Space before <

                Token *name = copy_token(start);
                name->kind = TK_IDENT;
                name->loc = filename;
                name->len = strlen(filename);
                name->at_bol = false;
                name->has_space = false;

                Token *close = copy_token(start);
                close->kind = TK_PUNCT;
                close->loc = ">";
                close->len = 1;
                close->at_bol = false;
                close->has_space = false;

                // Link them together
                hash->next = inc;
                inc->next = open;
                open->next = name;
                name->next = close;

                // Skip to end of line and continue
                tok = skip_line(tok);
                close->next = tok;

                cur = cur->next = hash;
                cur = cur->next = inc;
                cur = cur->next = open;
                cur = cur->next = name;
                cur = cur->next = close;
                continue;
            }

            char *path = NULL;
            if (is_dquote)
            {
                // Search relative to current file first
                char *name_copy = strdup(start->file->name);
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
                // For quoted includes, search user-specified paths (-I) first
                // before falling back to system include paths
                if (!path)
                    path = search_user_include_paths(filename);
            }
            if (!path)
                path = search_include_paths(filename);
            if (!path)
                error_tok(start, "%s: cannot open file", filename);

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
            char *cur_file = start->file->name;

            // Find which include path the current file is under
            int start_idx = 0;
            for (int i = 0; i < pp_include_paths_count; i++)
            {
                int len = strlen(pp_include_paths[i]);
                if (strncmp(cur_file, pp_include_paths[i], len) == 0 &&
                    (cur_file[len] == '/' || cur_file[len] == '\0'))
                {
                    start_idx = i + 1;
                    break;
                }
            }

            // Search from the next path onwards
            for (int i = start_idx; i < pp_include_paths_count; i++)
            {
                char *try = format("%s/%s", pp_include_paths[i], filename);
                struct stat st;
                if (stat(try, &st) == 0)
                {
                    path = try;
                    break;
                }
                free(try);
            }
            if (!path)
                error_tok(start, "%s: cannot open file", filename);

            tok = skip_line(tok);
            tok = include_file(start, path, tok);
            continue;
        }

        if (equal(tok, "define"))
        {
            read_macro_definition(&tok, tok->next);
            continue;
        }

        if (equal(tok, "undef"))
        {
            tok = tok->next;
            if (tok->kind != TK_IDENT)
                error_tok(tok, "expected identifier");
            hashmap_delete2(&macros, tok->loc, tok->len);
            tok = skip_line(tok->next);
            continue;
        }

        if (equal(tok, "if"))
        {
            Token *expr = read_const_expr(&tok, tok->next);
            expr = preprocess2(expr);
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
                expr = preprocess2(expr);
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
                // Mark this file as included
                char *real = realpath(start->file->name, NULL);
                char *key = real ? real : strdup(start->file->name);
                hashmap_put(&included_files, key, (void *)1);
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
    add_macro(name, true, tok);
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
    // Run the system compiler to get predefined macros
    // Include features.h to get library-defined macros like __GLIBC__
#if defined(__linux__)
    FILE *fp = popen("echo '#include <features.h>' | cc -dM -E - 2>/dev/null", "r");
#elif defined(__APPLE__)
    FILE *fp = popen("echo '#include <sys/cdefs.h>' | cc -dM -E - 2>/dev/null", "r");
#else
    FILE *fp = popen("cc -dM -E - < /dev/null 2>/dev/null", "r");
#endif
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

        // Skip certain problematic macros that we handle specially
        // or that would cause issues
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
                pp_define_macro(strdup(name), "");
            else
                pp_define_macro(strdup(name), strdup(value));
        }
        count++;
    }

    pclose(fp);
    return count;
}

// Reset preprocessor state - call before processing a new file to avoid state leakage
void pp_reset(void)
{
    hashmap_clear(&macros);
    hashmap_clear(&included_files);
    cond_incl = NULL;
    pp_eval_skip = false;
    reset_feature_test_macros();
}

void pp_init(void)
{
    // Reset any existing state first
    pp_reset();

    int imported = pp_import_system_macros();
    (void)imported; // May be -1 if compiler not found, we'll use fallbacks

    add_builtin("__FILE__", file_macro);
    add_builtin("__LINE__", line_macro);

    if (!hashmap_get(&macros, "__STDC__"))
        pp_define_macro("__STDC__", "1");
    if (!hashmap_get(&macros, "__STDC_VERSION__"))
        pp_define_macro("__STDC_VERSION__", "201112L");
    if (!hashmap_get(&macros, "__STDC_HOSTED__"))
        pp_define_macro("__STDC_HOSTED__", "1");

    pp_define_macro("__extension__", "");

    pp_define_full("__has_feature(x) 0");
    pp_define_full("__has_extension(x) 0");
    pp_define_full("__has_builtin(x) 0");
    pp_define_full("__has_attribute(x) 0");
    pp_define_full("__has_warning(x) 0");
    pp_define_full("__has_c_attribute(x) 0");
    pp_define_full("__building_module(x) 0");

    if (!hashmap_get(&macros, "CHAR_BIT"))
    {
        pp_define_macro("CHAR_BIT", "8");
        pp_define_macro("SCHAR_MIN", "(-128)");
        pp_define_macro("SCHAR_MAX", "127");
        pp_define_macro("UCHAR_MAX", "255");
        pp_define_macro("CHAR_MIN", "(-128)");
        pp_define_macro("CHAR_MAX", "127");
        pp_define_macro("SHRT_MIN", "(-32768)");
        pp_define_macro("SHRT_MAX", "32767");
        pp_define_macro("USHRT_MAX", "65535");
        pp_define_macro("INT_MIN", "(-2147483647-1)");
        pp_define_macro("INT_MAX", "2147483647");
        pp_define_macro("UINT_MAX", "4294967295U");
        pp_define_macro("LONG_MIN", "(-9223372036854775807L-1L)");
        pp_define_macro("LONG_MAX", "9223372036854775807L");
        pp_define_macro("ULONG_MAX", "18446744073709551615UL");
        pp_define_macro("LLONG_MIN", "(-9223372036854775807LL-1LL)");
        pp_define_macro("LLONG_MAX", "9223372036854775807LL");
        pp_define_macro("ULLONG_MAX", "18446744073709551615ULL");
    }

    // glibc internal macros needed when system headers are passed through
    if (!hashmap_get(&macros, "__getopt_argv_const"))
        pp_define_macro("__getopt_argv_const", "const");

#ifdef __APPLE__
    // Apple availability attributes - make them expand to nothing
    // These are used for API versioning and would break compilation otherwise
    pp_define_full("__API_AVAILABLE(...) ");
    pp_define_full("__API_UNAVAILABLE(...) ");
    pp_define_full("__API_DEPRECATED(...) ");
    pp_define_full("__API_DEPRECATED_WITH_REPLACEMENT(...) ");
    pp_define_full("__OSX_AVAILABLE(...) ");
    pp_define_full("__IOS_AVAILABLE(...) ");
    pp_define_full("__TVOS_AVAILABLE(...) ");
    pp_define_full("__WATCHOS_AVAILABLE(...) ");
    pp_define_full("__OSX_AVAILABLE_STARTING(...) ");
    pp_define_full("__OSX_AVAILABLE_BUT_DEPRECATED(...) ");
    pp_define_full("__OSX_DEPRECATED(...) ");
    pp_define_full("__IOS_PROHIBITED ");
    pp_define_full("__TVOS_PROHIBITED ");
    pp_define_full("__WATCHOS_PROHIBITED ");
    pp_define_macro("__OSX_EXTENSION_UNAVAILABLE", "");
    pp_define_macro("__IOS_EXTENSION_UNAVAILABLE", "");
    pp_define_full("_API_AVAILABLE(...) ");
    pp_define_full("__SWIFT_UNAVAILABLE_MSG(...) ");
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
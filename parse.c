// parse.c - C tokenizer and preprocessor extracted from chibicc
// Original: https://github.com/rui314/chibicc (MIT License)
//
// This provides:
//   - tokenize_file(path) -> Token list
//   - preprocess(tok) -> Token list (macro-expanded)
//
// Build: Include this file before your main code, or compile separately
// Regen: This is a one-time extraction, modify as needed

// =============================================================================
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

#ifndef __GNUC__
#define __attribute__(x)
#endif

// Include paths for preprocessor
static char **pp_include_paths = NULL;
static int pp_include_paths_count = 0;

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
static Type ty_ushort_val = {TY_SHORT, 2, 2, true};
static Type ty_uint_val = {TY_INT, 4, 4, true};

static Type *ty_char = &ty_char_val;
static Type *ty_int = &ty_int_val;
static Type *ty_ushort = &ty_ushort_val;
static Type *ty_uint = &ty_uint_val;

static Type *array_of(Type *base, int len)
{
    Type *ty = calloc(1, sizeof(Type));
    ty->kind = TY_ARRAY;
    ty->base = base;
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
    pp_include_paths = realloc(pp_include_paths, sizeof(char *) * (pp_include_paths_count + 1));
    pp_include_paths[pp_include_paths_count++] = strdup(path);
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
            }
        }
    }
    closedir(dir);

    if (best_version)
    {
        char *result = malloc(PATH_MAX);
        snprintf(result, PATH_MAX, "%s/%s/include", dir_path, best_version);
        free(best_version);
        return result;
    }
    return NULL;
}

static void pp_add_default_include_paths(void)
{
    struct stat st;

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
            if (path && stat(path, &st) == 0)
            {
                pp_add_include_path(path);
                found_gcc = true;
            }
        }
    }

    pp_add_include_path("/usr/local/include");

    // Try common multiarch paths
    pp_add_include_path("/usr/include/x86_64-linux-gnu");
    pp_add_include_path("/usr/include/aarch64-linux-gnu");

    pp_add_include_path("/usr/include");
}

// =============================================================================
// Utility functions
// =============================================================================

static bool is_hex(Token *tok) { return tok->len >= 3 && !memcmp(tok->loc, "0x", 2); }
static bool is_bin(Token *tok) { return tok->len >= 3 && !memcmp(tok->loc, "0b", 2); }

static void strarray_push(StringArray *arr, char *s)
{
    if (!arr->data)
    {
        arr->data = calloc(8, sizeof(char *));
        arr->capacity = 8;
    }
    if (arr->capacity == arr->len)
    {
        arr->data = realloc(arr->data, sizeof(char *) * arr->capacity * 2);
        arr->capacity *= 2;
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
    return buf;
}

// =============================================================================
// Hashmap
// =============================================================================

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

#define TOMBSTONE ((void *)-1)

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

// =============================================================================
// Unicode
// =============================================================================

static int encode_utf8(char *buf, uint32_t c)
{
    if (c <= 0x7F)
    {
        buf[0] = c;
        return 1;
    }
    if (c <= 0x7FF)
    {
        buf[0] = 0xC0 | (c >> 6);
        buf[1] = 0x80 | (c & 0x3F);
        return 2;
    }
    if (c <= 0xFFFF)
    {
        buf[0] = 0xE0 | (c >> 12);
        buf[1] = 0x80 | ((c >> 6) & 0x3F);
        buf[2] = 0x80 | (c & 0x3F);
        return 3;
    }
    buf[0] = 0xF0 | (c >> 18);
    buf[1] = 0x80 | ((c >> 12) & 0x3F);
    buf[2] = 0x80 | ((c >> 6) & 0x3F);
    buf[3] = 0x80 | (c & 0x3F);
    return 4;
}

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

// =============================================================================
// Error handling
// =============================================================================

static File *current_file;

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

#define unreachable() error("internal error at %s:%d", __FILE__, __LINE__)

// =============================================================================
// Tokenizer
// =============================================================================

static File **input_files;
static int input_file_count;
static bool at_bol;
static bool has_space;

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
    char *buf = calloc(1, end - quote);
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
    int c;
    if (*p == '\\')
        c = read_escaped_char(&p, p + 1);
    else
        c = decode_utf8(&p, p);
    char *end = p;
    for (; *end != '\''; end++)
        if (*end == '\n' || *end == '\0')
            error_at(p, "unclosed char literal");
    Token *tok = new_token(TK_NUM, start, end + 1);
    tok->val = c;
    tok->ty = ty;
    return tok;
}

static int64_t read_int(char **p, int base)
{
    int64_t val = 0;
    while (isxdigit(**p))
    {
        int d = from_hex(**p);
        if (d < 0)
            break;
        if (d >= base)
            break;
        val = val * base + d;
        (*p)++;
    }
    return val;
}

static void convert_pp_number(Token *tok)
{
    char *p = tok->loc;

    // Try integer
    int base = 10;
    if (startswith(p, "0x") || startswith(p, "0X"))
    {
        base = 16;
        p += 2;
    }
    else if (startswith(p, "0b") || startswith(p, "0B"))
    {
        base = 2;
        p += 2;
    }
    else if (*p == '0')
        base = 8;

    int64_t val = read_int(&p, base);

    // Skip suffixes
    bool is_unsigned = false;
    for (;;)
    {
        if (*p == 'u' || *p == 'U')
        {
            is_unsigned = true;
            p++;
        }
        else if (*p == 'l' || *p == 'L')
            p++;
        else
            break;
    }

    // Check for float
    if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'p' || *p == 'P')
    {
        char *end;
        tok->fval = strtold(tok->loc, &end);
        tok->kind = TK_NUM;
        return;
    }

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
            while (*p != '\n')
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
        if (startswith(p, "u8\""))
        {
            cur = cur->next = read_string_literal(p, p + 2);
            p += cur->len;
            continue;
        }
        if (*p == '\'')
        {
            cur = cur->next = read_char_literal(p, p, ty_int);
            cur->val = (char)cur->val;
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
    input_files = realloc(input_files, sizeof(File *) * (input_file_count + 2));
    input_files[input_file_count++] = file;
    input_files[input_file_count] = NULL;

    return tokenize(file);
}

// =============================================================================
// Preprocessor
// =============================================================================

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

static char *read_include_filename(Token **rest, Token *tok, bool *is_dquote)
{
    if (tok->kind == TK_STR)
    {
        *is_dquote = true;
        *rest = tok->next;
        return strndup(tok->str, tok->ty->array_len - 1);
    }

    if (equal(tok, "<"))
    {
        Token *start = tok;
        for (; !equal(tok, ">"); tok = tok->next)
            if (tok->at_bol || tok->kind == TK_EOF)
                error_tok(tok, "expected '>'");
        *is_dquote = false;
        *rest = tok->next;

        int len = tok->loc - start->next->loc;
        return strndup(start->next->loc, len);
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
    add_macro(name, true, copy_line(rest, tok));
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

    // Undefined identifier is zero
    *rest = tok->next;
    return 0;
}

static int64_t eval_unary(Token **rest, Token *tok)
{
    if (equal(tok, "+"))
        return eval_unary(rest, tok->next);
    if (equal(tok, "-"))
        return -eval_unary(rest, tok->next);
    if (equal(tok, "!"))
        return !eval_unary(rest, tok->next);
    if (equal(tok, "~"))
        return ~eval_unary(rest, tok->next);
    return eval_primary(rest, tok);
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
            val <<= eval_add(&tok, tok->next);
            continue;
        }
        if (equal(tok, ">>"))
        {
            val >>= eval_add(&tok, tok->next);
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

        cur = cur->next = tok;
        tok = tok->next;
    }

    cur->next = new_eof(tok);
    *rest = tok;
    return head.next;
}

static CondIncl *push_cond_incl(Token *tok, bool included)
{
    CondIncl *ci = calloc(1, sizeof(CondIncl));
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

        if (equal(tok, "include"))
        {
            bool is_dquote;
            char *filename = read_include_filename(&tok, tok->next, &is_dquote);

            char *path = NULL;
            if (is_dquote)
            {
                // Search relative to current file first
                char *dir = dirname(strdup(start->file->name));
                char *try = format("%s/%s", dir, filename);
                struct stat st;
                if (stat(try, &st) == 0)
                    path = try;
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
            tok = skip_cond_incl(tok->next);
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
            tok = skip_line(tok->next);
            continue;
        }

        if (tok->kind == TK_PP_NUM)
        {
            // Line directive like: # 1 "filename"
            tok = skip_line(tok);
            continue;
        }

        error_tok(tok, "invalid preprocessor directive");
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

void pp_init(void)
{
    add_builtin("__FILE__", file_macro);
    add_builtin("__LINE__", line_macro);

    pp_define_macro("__STDC__", "1");
    pp_define_macro("__STDC_VERSION__", "201112L");
    pp_define_macro("__STDC_HOSTED__", "1");
    pp_define_macro("__STDC_UTF_16__", "1");
    pp_define_macro("__STDC_UTF_32__", "1");

    // Common predefined macros
    pp_define_macro("__LP64__", "1");
    pp_define_macro("__SIZEOF_POINTER__", "8");
    pp_define_macro("__SIZEOF_LONG__", "8");
    pp_define_macro("__SIZEOF_INT__", "4");
    pp_define_macro("__SIZEOF_SHORT__", "2");
    pp_define_macro("__SIZEOF_FLOAT__", "4");
    pp_define_macro("__SIZEOF_DOUBLE__", "8");
    pp_define_macro("__SIZEOF_LONG_DOUBLE__", "16");
    pp_define_macro("__SIZEOF_LONG_LONG__", "8");
    pp_define_macro("__SIZEOF_SIZE_T__", "8");
    pp_define_macro("__SIZEOF_PTRDIFF_T__", "8");
    pp_define_macro("__SIZEOF_WCHAR_T__", "4");
    pp_define_macro("__SIZEOF_WINT_T__", "4");

    // GCC compatibility - critical for glibc headers
    // Use GCC 13+ to avoid _Float32 typedef conflicts (they're compiler built-ins in modern GCC)
    pp_define_macro("__GNUC__", "13");
    pp_define_macro("__GNUC_MINOR__", "0");
    pp_define_macro("__GNUC_PATCHLEVEL__", "0");
    pp_define_macro("__GNUC_STDC_INLINE__", "1");

    // GNU C extensions - just make them empty/passthrough
    pp_define_macro("__extension__", "");
    pp_define_macro("__inline", "inline");
    pp_define_macro("__inline__", "inline");
    pp_define_macro("__signed__", "signed");
    pp_define_macro("__const", "const");
    pp_define_macro("__const__", "const");
    pp_define_macro("__volatile__", "volatile");
    pp_define_macro("__asm__", "asm");
    pp_define_macro("__asm", "asm");
    pp_define_macro("__typeof__", "typeof");

    // Byte order
    pp_define_macro("__BYTE_ORDER__", "1234");
    pp_define_macro("__ORDER_LITTLE_ENDIAN__", "1234");
    pp_define_macro("__ORDER_BIG_ENDIAN__", "4321");

    // Char signedness
    pp_define_macro("__CHAR_BIT__", "8");
    pp_define_macro("__SCHAR_MAX__", "127");
    pp_define_macro("__SHRT_MAX__", "32767");
    pp_define_macro("__INT_MAX__", "2147483647");
    pp_define_macro("__LONG_MAX__", "9223372036854775807L");
    pp_define_macro("__LONG_LONG_MAX__", "9223372036854775807LL");

#if defined(__linux__)
    pp_define_macro("__linux__", "1");
    pp_define_macro("__linux", "1");
    pp_define_macro("linux", "1");
    pp_define_macro("__unix__", "1");
    pp_define_macro("__unix", "1");
    pp_define_macro("unix", "1");
    pp_define_macro("__gnu_linux__", "1");
    pp_define_macro("__ELF__", "1");
#elif defined(__APPLE__)
    pp_define_macro("__APPLE__", "1");
    pp_define_macro("__MACH__", "1");
#elif defined(_WIN32)
    pp_define_macro("_WIN32", "1");
#endif

#if defined(__x86_64__)
    pp_define_macro("__x86_64__", "1");
    pp_define_macro("__x86_64", "1");
    pp_define_macro("__amd64__", "1");
    pp_define_macro("__amd64", "1");
#elif defined(__aarch64__)
    pp_define_macro("__aarch64__", "1");
#elif defined(__i386__)
    pp_define_macro("__i386__", "1");
    pp_define_macro("__i386", "1");
    pp_define_macro("i386", "1");
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

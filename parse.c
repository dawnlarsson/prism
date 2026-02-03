// parse.c - C tokenizer (preprocessor removed - use cc -E)
// Original: https://github.com/rui314/chibicc (MIT License)
//
// API:
//   Token *tokenize_file(path)  - Tokenize a file
//   Token *tokenize(File *file) - Tokenize from File struct
//
// Usage: Run "cc -E -P input.c" first, then tokenize the output.

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef __GNUC__
#define __attribute__(x)
#endif

#define TOMBSTONE ((void *)-1)

typedef struct Token Token;

// File info
typedef struct
{
    char *name;
    int file_no;
    char *contents;
    char *display_name;
    int line_delta;
    int *line_offsets;
    int line_count;
    bool owns_contents;
    bool owns_line_offsets;
    bool is_system;
    bool is_include_entry; // True if inside a system #include chain (not just macro expansion)
} File;

// Token types
typedef enum
{
    TK_IDENT,
    TK_PUNCT,
    TK_KEYWORD,
    TK_STR,
    TK_NUM,
    TK_PP_NUM,
    TK_PREP_DIR, // Preprocessor directive (e.g., #pragma) to preserve
    TK_EOF,
} TokenKind;

// Token flags
enum
{
    TF_AT_BOL = 1 << 0,
    TF_HAS_SPACE = 1 << 1,
    TF_IS_FLOAT = 1 << 2,
};

struct Token
{
    char *loc;
    Token *next;
    union
    {
        int64_t i64;
        char *str;
    } val;
    int len;
    TokenKind kind;
    uint16_t file_idx;
    uint8_t flags;
};

// Token accessors
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

// Arena allocator for tokens
#define ARENA_BLOCK_SIZE 4096

typedef struct ArenaBlock ArenaBlock;
struct ArenaBlock
{
    ArenaBlock *next;
    int used;
    Token tokens[ARENA_BLOCK_SIZE];
};

typedef struct
{
    ArenaBlock *head;
    ArenaBlock *current;
} TokenArena;

static TokenArena token_arena = {0};

static Token *arena_alloc_token(void)
{
    if (!token_arena.current || token_arena.current->used >= ARENA_BLOCK_SIZE)
    {
        ArenaBlock *block = malloc(sizeof(ArenaBlock));
        if (!block)
        {
            fprintf(stderr, "out of memory allocating token arena block\n");
            exit(1);
        }
        block->next = NULL;
        block->used = 0;
        if (token_arena.current)
            token_arena.current->next = block;
        else
            token_arena.head = block;
        token_arena.current = block;
    }
    Token *tok = &token_arena.current->tokens[token_arena.current->used++];
    memset(tok, 0, sizeof(Token));
    return tok;
}

static void arena_reset(void)
{
    for (ArenaBlock *b = token_arena.head; b; b = b->next)
        b->used = 0;
    token_arena.current = token_arena.head;
}

// String allocation tracker - tracks malloc'd strings for cleanup
static char **string_allocs = NULL;
static int string_alloc_count = 0;
static int string_alloc_capacity = 0;

static char *track_string_alloc(size_t size)
{
    char *ptr = calloc(1, size);
    if (!ptr)
        return NULL;
    if (string_alloc_count >= string_alloc_capacity)
    {
        int new_cap = string_alloc_capacity == 0 ? 256 : string_alloc_capacity * 2;
        char **new_allocs = realloc(string_allocs, sizeof(char *) * new_cap);
        if (!new_allocs)
        {
            free(ptr);
            return NULL;
        }
        string_allocs = new_allocs;
        string_alloc_capacity = new_cap;
    }
    string_allocs[string_alloc_count++] = ptr;
    return ptr;
}

static void free_string_allocs(void)
{
    for (int i = 0; i < string_alloc_count; i++)
        free(string_allocs[i]);
    string_alloc_count = 0;
}

// Simple hashmap for keywords
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

static void *hashmap_get(HashMap *map, char *key, int keylen)
{
    if (!map->buckets)
        return NULL;
    uint64_t hash = fnv_hash(key, keylen);
    for (int i = 0; i < map->capacity; i++)
    {
        HashEntry *ent = &map->buckets[(hash + i) % map->capacity];
        if (ent->key && ent->key != TOMBSTONE &&
            ent->keylen == keylen && !memcmp(ent->key, key, keylen))
            return ent->val;
        if (!ent->key)
            return NULL;
    }
    return NULL;
}

static void hashmap_put(HashMap *map, char *key, int keylen, void *val);

static void hashmap_resize(HashMap *map, int newcap)
{
    HashMap map2 = {.buckets = calloc(newcap, sizeof(HashEntry)), .capacity = newcap};
    for (int i = 0; i < map->capacity; i++)
    {
        HashEntry *ent = &map->buckets[i];
        if (ent->key && ent->key != TOMBSTONE)
            hashmap_put(&map2, ent->key, ent->keylen, ent->val);
    }
    free(map->buckets);
    *map = map2;
}

static void hashmap_put(HashMap *map, char *key, int keylen, void *val)
{
    if (!map->buckets)
    {
        map->buckets = calloc(64, sizeof(HashEntry));
        map->capacity = 64;
    }
    else if (map->used * 100 / map->capacity >= 70)
        hashmap_resize(map, map->capacity * 2);
    uint64_t hash = fnv_hash(key, keylen);
    int first_empty = -1;
    for (int i = 0; i < map->capacity; i++)
    {
        HashEntry *ent = &map->buckets[(hash + i) % map->capacity];
        // Check for existing key to update
        if (ent->key && ent->key != TOMBSTONE &&
            ent->keylen == keylen && !memcmp(ent->key, key, keylen))
        {
            ent->val = val; // Update existing entry
            return;
        }
        // Remember first empty slot
        if (first_empty < 0 && (!ent->key || ent->key == TOMBSTONE))
            first_empty = (hash + i) % map->capacity;
        // Stop at truly empty slot (not tombstone)
        if (!ent->key)
            break;
    }
    // Insert into first empty slot
    if (first_empty >= 0)
    {
        HashEntry *ent = &map->buckets[first_empty];
        ent->key = key;
        ent->keylen = keylen;
        ent->val = val;
        if (ent->key != TOMBSTONE)
            map->used++;
    }
}

static void hashmap_delete2(HashMap *map, char *key, int keylen)
{
    if (!map->buckets)
        return;
    uint64_t hash = fnv_hash(key, keylen);
    for (int i = 0; i < map->capacity; i++)
    {
        HashEntry *ent = &map->buckets[(hash + i) % map->capacity];
        if (ent->key && ent->key != TOMBSTONE &&
            ent->keylen == keylen && !memcmp(ent->key, key, keylen))
        {
            ent->key = TOMBSTONE;
            return;
        }
        if (!ent->key)
            return;
    }
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

// File tracking
static File *current_file;
static File **input_files;
static int input_file_count;
static int input_file_capacity;
static bool at_bol;
static bool has_space;

static void free_file(File *f)
{
    if (!f)
        return;
    if (f->contents && f->owns_contents)
        free(f->contents);
    if (f->line_offsets && f->owns_line_offsets)
        free(f->line_offsets);
    if (f->name)
        free(f->name);
    free(f);
}

static inline File *tok_file(Token *tok)
{
    if (!tok || tok->file_idx >= input_file_count)
        return current_file;
    return input_files[tok->file_idx];
}

static int tok_line_no(Token *tok)
{
    File *f = tok_file(tok);
    if (!f || !f->contents || !tok->loc)
        return -1;
    char *file_start = f->contents;
    char *file_end = f->contents + strlen(f->contents);
    if (tok->loc < file_start || tok->loc >= file_end)
        return -1;
    int offset = (int)(tok->loc - f->contents);
    int lo = 0, hi = f->line_count - 1;
    while (lo < hi)
    {
        int mid = lo + (hi - lo + 1) / 2;
        if (f->line_offsets[mid] <= offset)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo + 1 + f->line_delta;
}

// Error handling
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
    fprintf(stderr, "%.*s\n%*s^ ", (int)(end - line), line, indent + (int)(loc - line), "");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
}

static int count_lines(char *base, char *loc)
{
    int n = 1;
    for (char *p = base; p < loc; p++)
        if (*p == '\n')
            n++;
    return n;
}

noreturn void error_at(char *loc, char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verror_at(current_file->name, current_file->contents, count_lines(current_file->contents, loc), loc, fmt, ap);
    exit(1);
}

noreturn void error_tok(Token *tok, char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    File *f = tok_file(tok);
    verror_at(f->name, f->contents, tok_line_no(tok), tok->loc, fmt, ap);
    exit(1);
}

static void warn_tok(Token *tok, char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    File *f = tok_file(tok);
    verror_at(f->name, f->contents, tok_line_no(tok), tok->loc, fmt, ap);
    va_end(ap);
}

// Token helpers
static inline bool equal(Token *tok, const char *op)
{
    size_t len = __builtin_constant_p(*op) ? __builtin_strlen(op) : strlen(op);
    return tok->len == (int)len && !memcmp(tok->loc, op, len);
}

static Token *skip(Token *tok, char *op)
{
    if (!equal(tok, op))
        error_tok(tok, "expected '%s'", op);
    return tok->next;
}

// Keyword map
static HashMap keyword_map;

static void init_keyword_map(void)
{
    static char *kw[] = {
        "return",
        "if",
        "else",
        "for",
        "while",
        "do",
        "switch",
        "case",
        "default",
        "break",
        "continue",
        "goto",
        "sizeof",
        "alignof",
        "struct",
        "union",
        "enum",
        "typedef",
        "static",
        "extern",
        "inline",
        "const",
        "volatile",
        "restrict",
        "_Atomic",
        "_Noreturn",
        "_Thread_local",
        "void",
        "char",
        "short",
        "int",
        "long",
        "float",
        "double",
        "signed",
        "unsigned",
        "_Bool",
        "auto",
        "register",
        "_Alignas",
        "_Static_assert",
        "_Generic",
        "typeof",
        "__typeof__",
        "asm",
        "__asm__",
        "__attribute__",
        "__extension__",
        "__builtin_va_list",
        "__builtin_va_arg",
        "__builtin_offsetof",
        "__builtin_types_compatible_p",
        // Prism keywords
        "defer",
        "raw",
    };
    for (size_t i = 0; i < sizeof(kw) / sizeof(*kw); i++)
        hashmap_put(&keyword_map, kw[i], strlen(kw[i]), (void *)1);
}

static bool is_keyword(Token *tok)
{
    return hashmap_get(&keyword_map, tok->loc, tok->len) != NULL;
}

// Tokenizer helpers
static int read_ident(char *start)
{
    char *p = start;
    if (!isalpha(*p) && *p != '_' && *p != '$')
        return 0;
    while (isalnum(*p) || *p == '_' || *p == '$')
        p++;
    return p - start;
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
        "%=",
        "&=",
        "|=",
        "^=",
        "&&",
        "||",
        "++",
        "--",
        "<<",
        ">>",
        "##",
    };
    for (size_t i = 0; i < sizeof(kw) / sizeof(*kw); i++)
        if (!strncmp(p, kw[i], strlen(kw[i])))
            return strlen(kw[i]);
    return ispunct(*p) ? 1 : 0;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\f' || c == '\r' || c == '\v';
}

static char *skip_line_comment(char *p)
{
    for (; *p && *p != '\n'; p++)
        ;
    return p;
}

static char *skip_block_comment(char *p)
{
    for (; *p; p++)
        if (p[0] == '*' && p[1] == '/')
            return p + 2;
    error_at(p, "unclosed block comment");
}

static int read_escaped_char(char **new_pos, char *p)
{
    if ('0' <= *p && *p <= '7')
    {
        int c = *p++ - '0';
        if ('0' <= *p && *p <= '7')
            c = (c << 3) + (*p++ - '0');
        if ('0' <= *p && *p <= '7')
            c = (c << 3) + (*p++ - '0');
        *new_pos = p;
        return c;
    }
    if (*p == 'x')
    {
        p++;
        if (!isxdigit(*p))
            error_at(p, "invalid hex escape");
        int c = 0;
        while (isxdigit(*p))
            c = (c << 4) + from_hex(*p++);
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
    for (; *p != '"'; p++)
    {
        if (*p == '\0')
            error_at(p, "unclosed string literal");
        if (*p == '\\')
            p++;
    }
    return p;
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

static Token *read_string_literal(char *start, char *quote)
{
    char *end = string_literal_end(quote + 1);
    size_t buf_size = (end - quote);
    if (buf_size == 0)
        buf_size = 1;
    char *buf = track_string_alloc(buf_size);
    if (!buf)
        error("out of memory in read_string_literal");
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
    int count = 0, first_c = 0;
    for (;;)
    {
        if (*p == '\n' || *p == '\0')
            error_at(p, "unclosed char literal");
        if (*p == '\'')
            break;
        int c = (*p == '\\') ? read_escaped_char(&p, p + 1) : (unsigned char)*p++;
        if (count == 0)
            first_c = c;
        if (count < 4)
            val = (val << 8) | (c & 0xFF);
        count++;
    }
    if (count == 0)
        error_at(start, "empty char literal");
    Token *tok = new_token(TK_NUM, start, p + 1);
    tok->val.i64 = (count == 1) ? first_c : (int32_t)val;
    return tok;
}

static int64_t read_int_literal(char **pp, int base)
{
    char *end;
    int64_t val = strtoll(*pp, &end, base);
    *pp = end;
    return val;
}

// Check for C23 extended float suffix and return info for normalization
// Returns: suffix length to strip (0 if no extended suffix)
// Sets *replacement to the standard suffix to use (NULL for none, "f" for float, "L" for long double)
static int get_extended_float_suffix(const char *p, int len, const char **replacement)
{
    if (replacement)
        *replacement = NULL;
    if (len < 3)
        return 0;
    const char *e = p + len;
    // 4-char suffixes: BF16, F128
    if (len >= 4)
    {
        char c = e[-4] | 0x20; // lowercase
        if (c == 'b' && (e[-3] | 0x20) == 'f' && e[-2] == '1' && e[-1] == '6')
        {
            if (replacement)
                *replacement = "f";
            return 4;
        }
        if (c == 'f' && e[-3] == '1' && e[-2] == '2' && e[-1] == '8')
        {
            if (replacement)
                *replacement = "L";
            return 4;
        }
    }
    // 3-char suffixes: F16, F32, F64
    if ((e[-3] | 0x20) == 'f' && e[-1] >= '2' && e[-1] <= '6')
    {
        if (e[-2] == '6' && e[-1] == '4')
            return 3;
        if (e[-2] == '3' && e[-1] == '2')
        {
            if (replacement)
                *replacement = "f";
            return 3;
        }
        if (e[-2] == '1' && e[-1] == '6')
        {
            if (replacement)
                *replacement = "f";
            return 3;
        }
    }
    return 0;
}

static void convert_pp_number(Token *tok)
{
    char *p = tok->loc;
    int base = 10;

    // Determine base from prefix
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        base = 16;
        p += 2;
    }
    else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B'))
    {
        base = 2;
        p += 2;
    }
    else if (p[0] == '0' && tok->len > 1)
        base = 8;

    // Check for float (decimal/hex only, not binary)
    if (base != 2)
    {
        // C23 extended float suffixes (F16, F32, F64, F128, BF16)
        if (base == 10 && get_extended_float_suffix(tok->loc, tok->len, NULL))
            goto is_float;

        // Standard float indicators: '.', exponent (e/E for decimal, p/P for hex)
        for (char *q = tok->loc; q < tok->loc + tok->len; q++)
        {
            if (*q == '.' || *q == 'p' || *q == 'P' ||
                (base != 16 && (*q == 'e' || *q == 'E')))
                goto is_float;
        }
    }

    tok->kind = TK_NUM;
    tok->val.i64 = read_int_literal(&p, base);
    return;

is_float:
    tok->kind = TK_NUM;
    tok->flags |= TF_IS_FLOAT;
    tok->val.i64 = 0;
}

static void convert_pp_tokens(Token *tok)
{
    for (Token *t = tok; t && t->kind != TK_EOF; t = t->next)
    {
        if (is_keyword(t))
            t->kind = TK_KEYWORD;
        else if (t->kind == TK_PP_NUM)
            convert_pp_number(t);
    }
}

static File *new_file(char *name, int file_no, char *contents)
{
    File *file = calloc(1, sizeof(File));
    if (!file)
        error("out of memory");
    file->name = strdup(name);
    file->display_name = file->name;
    file->file_no = file_no;
    file->contents = contents;
    file->line_delta = 0;
    file->owns_contents = true;
    file->owns_line_offsets = true;
    file->is_system = false;

    // Build line offset table
    int line_count = 1;
    for (char *p = contents; *p; p++)
        if (*p == '\n')
            line_count++;
    file->line_offsets = malloc(sizeof(int) * line_count);
    if (!file->line_offsets)
        error("out of memory");
    file->line_count = line_count;
    file->line_offsets[0] = 0;
    int line = 1;
    for (char *p = contents; *p; p++)
        if (*p == '\n' && line < line_count)
            file->line_offsets[line++] = (p - contents) + 1;
    return file;
}

static void add_input_file(File *file)
{
    if (input_file_count >= input_file_capacity)
    {
        int new_cap = input_file_capacity == 0 ? 16 : input_file_capacity * 2;
        File **new_files = realloc(input_files, sizeof(File *) * new_cap);
        if (!new_files)
            error("out of memory");
        input_files = new_files;
        input_file_capacity = new_cap;
    }
    input_files[input_file_count++] = file;
}

static File *new_file_view(const char *name, File *base, int line_delta, bool is_system, bool is_include_entry)
{
    File *file = calloc(1, sizeof(File));
    if (!file)
        error("out of memory");
    file->name = strdup(name ? name : base->name);
    file->display_name = file->name;
    file->file_no = input_file_count;
    file->contents = base->contents;
    file->line_offsets = base->line_offsets;
    file->line_count = base->line_count;
    file->line_delta = line_delta;
    file->owns_contents = false;
    file->owns_line_offsets = false;
    file->is_system = is_system;
    file->is_include_entry = is_include_entry;
    return file;
}

// Scan a preprocessor directive starting at '#'
// Returns new position after directive, or NULL if not a line marker (caller handles as TK_PREP_DIR)
// Updates *in_system_include, *line_no, and may switch current_file
static char *scan_line_directive(char *p, File *base_file, int *line_no, bool *in_system_include)
{
    char *directive_start = p;
    int directive_line = *line_no;
    p++; // skip '#'
    while (*p == ' ' || *p == '\t')
        p++;

    // Parse optional "line" keyword
    if (!strncmp(p, "line", 4) && (p[4] == ' ' || p[4] == '\t'))
    {
        p += 4;
        while (*p == ' ' || *p == '\t')
            p++;
    }

    // Must have line number to be a line marker
    if (!isdigit(*p))
        return NULL;

    long new_line = 0;
    while (isdigit(*p))
    {
        new_line = new_line * 10 + (*p - '0');
        p++;
    }
    while (*p == ' ' || *p == '\t')
        p++;

    // Parse optional filename in quotes
    char *filename = NULL;
    if (*p == '"')
    {
        p++;
        char *start = p;
        while (*p && *p != '"')
        {
            if (*p == '\\' && p[1])
                p++;
            p++;
        }
        int len = p - start;
        filename = malloc(len + 1);
        if (!filename)
            error("out of memory");
        memcpy(filename, start, len);
        filename[len] = '\0';
        if (*p == '"')
            p++;
    }

    // Parse flags (numbers after filename)
    bool is_system = false, is_entering = false, is_returning = false;
    while (*p == ' ' || *p == '\t')
        p++;
    while (isdigit(*p))
    {
        int flag = 0;
        while (isdigit(*p))
        {
            flag = flag * 10 + (*p - '0');
            p++;
        }
        if (flag == 1)
            is_entering = true;
        if (flag == 2)
            is_returning = true;
        if (flag == 3)
            is_system = true;
        while (*p == ' ' || *p == '\t')
            p++;
    }

    // Track system include state
    if (is_entering && is_system)
        *in_system_include = true;
    else if (is_returning && !is_system)
        *in_system_include = false;

    int line_delta = (int)new_line - (directive_line + 1);
    File *view = new_file_view(filename ? filename : current_file->name,
                               base_file, line_delta, is_system, *in_system_include);
    add_input_file(view);
    current_file = view;
    free(filename);

    // Skip to end of directive
    while (*p && *p != '\n')
        p++;
    if (*p == '\n')
    {
        p++;
        (*line_no)++;
    }
    return p;
}

// Scan a preprocessor number starting at p, return pointer past end
static char *scan_pp_number(char *p)
{
    for (;;)
    {
        if (p[0] && p[1] && (p[0] == 'e' || p[0] == 'E' || p[0] == 'p' || p[0] == 'P') &&
            (p[1] == '+' || p[1] == '-'))
        {
            p += 2;
        }
        else if (isdigit(*p) || *p == '.' || *p == '_')
        {
            p++;
        }
        else if (isalpha(*p))
        {
            char c = *p;
            if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
                c == 'x' || c == 'X' || c == 'b' || c == 'B' ||
                c == 'e' || c == 'E' || c == 'p' || c == 'P' ||
                c == 'u' || c == 'U' || c == 'l' || c == 'L' ||
                c == 'f' || c == 'F')
            {
                p++;
            }
            else
                break;
        }
        else
            break;
    }
    return p;
}

static Token *tokenize(File *file)
{
    File *base_file = file;
    current_file = file;
    char *p = file->contents;

    Token head = {};
    Token *cur = &head;
    at_bol = true;
    has_space = false;
    int line_no = 1;

    // Track if we're inside a system header include chain
    // This persists across nested includes until we return to user code
    static bool in_system_include = false;
    in_system_include = false; // Reset for each new file

    while (*p)
    {
        // Preprocessor directives (#line markers from -E output, or #pragma etc.)
        if (at_bol && *p == '#')
        {
            char *directive_start = p;
            char *after = scan_line_directive(p, base_file, &line_no, &in_system_include);
            if (after)
            {
                p = after;
                at_bol = true;
                has_space = false;
                continue;
            }

            // Not a line marker - preserve as preprocessor directive token
            while (*p && *p != '\n')
                p++;
            cur = cur->next = new_token(TK_PREP_DIR, directive_start, p);
            tok_set_at_bol(cur, true);
            if (*p == '\n')
            {
                p++;
                line_no++;
                at_bol = true;
                has_space = false;
            }
            continue;
        }

        // Line comment
        if (p[0] == '/' && p[1] == '/')
        {
            p = skip_line_comment(p + 2);
            has_space = true;
            continue;
        }
        // Block comment
        if (p[0] == '/' && p[1] == '*')
        {
            p = skip_block_comment(p + 2);
            has_space = true;
            continue;
        }
        // Newline
        if (*p == '\n')
        {
            p++;
            line_no++;
            at_bol = true;
            has_space = false;
            continue;
        }
        // Whitespace
        if (is_space(*p))
        {
            p++;
            has_space = true;
            continue;
        }
        // Preprocessor number
        if (isdigit(*p) || (*p == '.' && isdigit(p[1])))
        {
            char *start = p;
            p = scan_pp_number(p);
            cur = cur->next = new_token(TK_PP_NUM, start, p);
            continue;
        }
        // String literal
        if (*p == '"')
        {
            cur = cur->next = read_string_literal(p, p);
            p += cur->len;
            continue;
        }
        // UTF-8/wide string
        if ((p[0] == 'u' && p[1] == '8' && p[2] == '"') ||
            ((p[0] == 'u' || p[0] == 'U' || p[0] == 'L') && p[1] == '"'))
        {
            char *start = p;
            p += (p[0] == 'u' && p[1] == '8') ? 2 : 1;
            cur = cur->next = read_string_literal(start, p);
            p = start + cur->len;
            continue;
        }
        // Character literal
        if (*p == '\'')
        {
            cur = cur->next = read_char_literal(p, p);
            p += cur->len;
            continue;
        }
        if ((p[0] == 'u' || p[0] == 'U' || p[0] == 'L') && p[1] == '\'')
        {
            cur = cur->next = read_char_literal(p, p + 1);
            p += cur->len;
            continue;
        }
        // Identifier
        int ident_len = read_ident(p);
        if (ident_len)
        {
            cur = cur->next = new_token(TK_IDENT, p, p + ident_len);
            p += ident_len;
            continue;
        }
        // Punctuator
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
    convert_pp_tokens(head.next);
    return head.next;
}

Token *tokenize_file(char *path)
{
    // Initialize keyword map on first call
    if (!keyword_map.capacity)
        init_keyword_map();

    FILE *fp = fopen(path, "r");
    if (!fp)
        return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf)
    {
        fclose(fp);
        return NULL;
    }
    fread(buf, 1, size, fp);
    buf[size] = '\0';
    fclose(fp);

    File *file = new_file(path, input_file_count, buf);
    add_input_file(file);

    return tokenize(file);
}

// Reset state for reuse
void tokenizer_reset(void)
{
    free_string_allocs();
    arena_reset();
    for (int i = 0; i < input_file_count; i++)
        free_file(input_files[i]);
    free(input_files);
    input_files = NULL;
    input_file_count = 0;
    input_file_capacity = 0;
    current_file = NULL;
}
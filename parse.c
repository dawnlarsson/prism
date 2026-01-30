// parse.c - C tokenizer (preprocessor removed - use cc -E)
// Original: https://github.com/rui314/chibicc (MIT License)
//
// API:
//   Token *tokenize_file(path)  - Tokenize a file
//   Token *tokenize(File *file) - Tokenize from File struct
//
// Usage: Run "cc -E -P input.c" first, then tokenize the output.

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
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

static void *hashmap_get2(HashMap *map, char *key, int keylen)
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

static void hashmap_put2(HashMap *map, char *key, int keylen, void *val)
{
    if (!map->buckets)
    {
        map->buckets = calloc(64, sizeof(HashEntry));
        map->capacity = 64;
    }
    else if ((map->used * 100) / map->capacity >= 70)
    {
        // Rehash
        HashMap map2 = {};
        map2.buckets = calloc(map->capacity * 2, sizeof(HashEntry));
        map2.capacity = map->capacity * 2;
        for (int i = 0; i < map->capacity; i++)
        {
            HashEntry *ent = &map->buckets[i];
            if (ent->key && ent->key != TOMBSTONE)
                hashmap_put2(&map2, ent->key, ent->keylen, ent->val);
        }
        free(map->buckets);
        *map = map2;
    }
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

static void hashmap_put(HashMap *map, char *key, void *val)
{
    hashmap_put2(map, key, strlen(key), val);
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
static bool at_bol;
static bool has_space;

static void free_file(File *f)
{
    if (!f)
        return;
    if (f->contents)
        free(f->contents);
    if (f->line_offsets)
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
    return lo + 1;
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
    fprintf(stderr, "%.*s\n", (int)(end - line), line);
    fprintf(stderr, "%*s^ ", indent + (int)(loc - line), "");
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
        hashmap_put2(&keyword_map, kw[i], strlen(kw[i]), (void *)1);
}

static bool is_keyword(Token *tok)
{
    return hashmap_get2(&keyword_map, tok->loc, tok->len) != NULL;
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
    char *buf = calloc(1, buf_size);
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
    char *p = *pp;
    uint64_t val = 0;
    while (isxdigit(*p))
    {
        int d = from_hex(*p);
        if (d >= base)
            break;
        val = val * base + d;
        p++;
    }
    *pp = p;
    return val;
}

static void convert_pp_number(Token *tok)
{
    char *p = tok->loc;
    bool is_hex = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'));
    bool is_bin = (p[0] == '0' && (p[1] == 'b' || p[1] == 'B'));

    // Check for float
    if (!is_bin)
    {
        for (char *q = p; q < p + tok->len; q++)
        {
            if (*q == '.' || *q == 'e' || *q == 'E' ||
                (!is_hex && (*q == 'p' || *q == 'P')) ||
                (is_hex && (*q == 'p' || *q == 'P')))
            {
                tok->kind = TK_NUM;
                tok->flags |= TF_IS_FLOAT;
                tok->val.i64 = 0; // Float value doesn't matter for transpiler
                return;
            }
        }
    }

    int base = 10;
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
        base = 8;

    tok->val.i64 = read_int_literal(&p, base);
    tok->kind = TK_NUM;
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
            for (;;)
            {
                if (p[0] && p[1] && (p[0] == 'e' || p[0] == 'E' || p[0] == 'p' || p[0] == 'P') &&
                    (p[1] == '+' || p[1] == '-'))
                {
                    p += 2;
                }
                else if (isalnum(*p) || *p == '.')
                {
                    p++;
                }
                else
                {
                    break;
                }
            }
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

    // Add to input_files array
    File **new_files = realloc(input_files, sizeof(File *) * (input_file_count + 1));
    if (!new_files)
        error("out of memory");
    input_files = new_files;
    File *file = new_file(path, input_file_count, buf);
    input_files[input_file_count++] = file;

    return tokenize(file);
}

// Reset state for reuse
void tokenizer_reset(void)
{
    arena_reset();
    for (int i = 0; i < input_file_count; i++)
        free_file(input_files[i]);
    free(input_files);
    input_files = NULL;
    input_file_count = 0;
    current_file = NULL;
}
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
#include <setjmp.h>
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

// Fully free all arena blocks (for complete cleanup)
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

// Forward declaration for error reporting (defined later)
static noreturn void error(char *fmt, ...);

// Library mode error recovery - when PRISM_LIB_MODE is defined, errors
// longjmp back to a recovery point instead of calling exit(1).
// This allows the host process to continue after transpilation errors.
#ifdef PRISM_LIB_MODE
static jmp_buf prism_error_jmp;
static bool prism_error_jmp_set = false;
static char prism_error_msg[1024];
static int prism_error_line = 0;
static int prism_error_col = 0;
#endif

// File tracking
static File *current_file;
static File **input_files;
static int input_file_count;
static int input_file_capacity;
static bool at_bol;
static bool has_space;

// Filename interning - avoids duplicating identical filename strings
// Each entry maps filename string -> interned copy
static HashMap filename_intern_map;

static char *intern_filename(const char *name)
{
    if (!name)
        return NULL;
    int len = strlen(name);
    char *existing = hashmap_get(&filename_intern_map, (char *)name, len);
    if (existing)
        return existing;
    // Allocate and store new interned string
    char *interned = malloc(len + 1);
    if (!interned)
        error("out of memory");
    memcpy(interned, name, len + 1);
    hashmap_put(&filename_intern_map, interned, len, interned);
    return interned;
}

// File view cache - avoids creating duplicate File structs for same file context
// Key: combines filename pointer, line_delta, is_system, is_include_entry
// This dramatically reduces allocations for headers with many #line directives
typedef struct
{
    char *filename; // Interned filename (pointer comparison valid)
    int line_delta;
    bool is_system;
    bool is_include_entry;
    File *file; // Cached File struct
} FileViewCacheEntry;

static FileViewCacheEntry *file_view_cache = NULL;
static int file_view_cache_count = 0;
static int file_view_cache_capacity = 0;

static File *find_cached_file_view(char *filename, int line_delta, bool is_system, bool is_include_entry)
{
    // Linear search is acceptable for typical use (hundreds of unique entries)
    // For very large translation units, this could be converted to a hash
    for (int i = 0; i < file_view_cache_count; i++)
    {
        FileViewCacheEntry *e = &file_view_cache[i];
        if (e->filename == filename && // Pointer comparison (interned)
            e->line_delta == line_delta &&
            e->is_system == is_system &&
            e->is_include_entry == is_include_entry)
        {
            return e->file;
        }
    }
    return NULL;
}

static void cache_file_view(char *filename, int line_delta, bool is_system, bool is_include_entry, File *file)
{
    if (file_view_cache_count >= file_view_cache_capacity)
    {
        int new_cap = file_view_cache_capacity == 0 ? 64 : file_view_cache_capacity * 2;
        FileViewCacheEntry *new_cache = realloc(file_view_cache, sizeof(FileViewCacheEntry) * new_cap);
        if (!new_cache)
            error("out of memory");
        file_view_cache = new_cache;
        file_view_cache_capacity = new_cap;
    }
    FileViewCacheEntry *e = &file_view_cache[file_view_cache_count++];
    e->filename = filename;
    e->line_delta = line_delta;
    e->is_system = is_system;
    e->is_include_entry = is_include_entry;
    e->file = file;
}

// Check if a filename pointer is from the intern map (vs malloc'd independently)
static bool is_interned_filename(char *name)
{
    if (!name)
        return false;
    int len = strlen(name);
    char *found = hashmap_get(&filename_intern_map, name, len);
    return found == name; // Pointer comparison
}

static void free_file(File *f)
{
    if (!f)
        return;
    if (f->contents && f->owns_contents)
        free(f->contents);
    if (f->line_offsets && f->owns_line_offsets)
        free(f->line_offsets);
    // Don't free interned filenames - they're managed by filename_intern_map
    if (f->name && !is_interned_filename(f->name))
        free(f->name);
    free(f);
}

// Free all interned filenames and clear the map
static void free_filename_intern_map(void)
{
    if (!filename_intern_map.buckets)
        return;
    for (int i = 0; i < filename_intern_map.capacity; i++)
    {
        HashEntry *ent = &filename_intern_map.buckets[i];
        if (ent->key && ent->key != TOMBSTONE)
        {
            free(ent->key); // Free the interned string
        }
    }
    free(filename_intern_map.buckets);
    filename_intern_map.buckets = NULL;
    filename_intern_map.capacity = 0;
    filename_intern_map.used = 0;
}

// Free file view cache (the File structs are freed separately)
static void free_file_view_cache(void)
{
    if (file_view_cache)
    {
        free(file_view_cache);
        file_view_cache = NULL;
    }
    file_view_cache_count = 0;
    file_view_cache_capacity = 0;
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
static noreturn void error(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
#ifdef PRISM_LIB_MODE
    if (prism_error_jmp_set)
    {
        vsnprintf(prism_error_msg, sizeof(prism_error_msg), fmt, ap);
        va_end(ap);
        longjmp(prism_error_jmp, 1);
    }
#endif
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
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
#ifdef PRISM_LIB_MODE
    if (prism_error_jmp_set)
    {
        prism_error_line = count_lines(current_file->contents, loc);
        vsnprintf(prism_error_msg, sizeof(prism_error_msg), fmt, ap);
        va_end(ap);
        longjmp(prism_error_jmp, 1);
    }
#endif
    verror_at(current_file->name, current_file->contents, count_lines(current_file->contents, loc), loc, fmt, ap);
    va_end(ap);
    exit(1);
}

noreturn void error_tok(Token *tok, char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
#ifdef PRISM_LIB_MODE
    if (prism_error_jmp_set)
    {
        File *f = tok_file(tok);
        prism_error_line = tok_line_no(tok);
        vsnprintf(prism_error_msg, sizeof(prism_error_msg), fmt, ap);
        va_end(ap);
        longjmp(prism_error_jmp, 1);
    }
#endif
    File *f = tok_file(tok);
    verror_at(f->name, f->contents, tok_line_no(tok), tok->loc, fmt, ap);
    va_end(ap);
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
// Check if token matches a digraph and return its canonical equivalent
static inline const char *digraph_equiv(Token *tok)
{
    if (tok->kind != TK_PUNCT)
        return NULL;
    if (tok->len == 4 && !memcmp(tok->loc, "%:%:", 4))
        return "##";
    if (tok->len == 2)
    {
        if (!memcmp(tok->loc, "<:", 2))
            return "[";
        if (!memcmp(tok->loc, ":>", 2))
            return "]";
        if (!memcmp(tok->loc, "<%", 2))
            return "{";
        if (!memcmp(tok->loc, "%>", 2))
            return "}";
        if (!memcmp(tok->loc, "%:", 2))
            return "#";
    }
    return NULL;
}

static inline bool equal(Token *tok, const char *op)
{
    size_t len = __builtin_constant_p(*op) ? __builtin_strlen(op) : strlen(op);
    if (tok->len == (int)len && !memcmp(tok->loc, op, len))
        return true;
    // Check digraph equivalence
    const char *equiv = digraph_equiv(tok);
    if (equiv && strlen(equiv) == len && !memcmp(equiv, op, len))
        return true;
    return false;
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

// Forward declaration for hex digit conversion
static int from_hex(char c);

// UTF-8 helpers for identifier parsing
// Check if byte is a UTF-8 continuation byte (10xxxxxx)
static inline bool is_utf8_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

// Get the number of bytes in a UTF-8 sequence from the leading byte
static int utf8_char_len(unsigned char c)
{
    if ((c & 0x80) == 0)
        return 1; // 0xxxxxxx - ASCII
    if ((c & 0xE0) == 0xC0)
        return 2; // 110xxxxx
    if ((c & 0xF0) == 0xE0)
        return 3; // 1110xxxx
    if ((c & 0xF8) == 0xF0)
        return 4; // 11110xxx
    return 0;     // Invalid
}

// Decode a UTF-8 character and return its Unicode codepoint
static uint32_t decode_utf8(char *p, int *len)
{
    unsigned char *s = (unsigned char *)p;
    int n = utf8_char_len(s[0]);
    if (n == 0)
    {
        *len = 1;
        return 0;
    }

    // Validate continuation bytes
    for (int i = 1; i < n; i++)
        if (!is_utf8_cont(s[i]))
        {
            *len = 1;
            return 0;
        }

    *len = n;
    switch (n)
    {
    case 1:
        return s[0];
    case 2:
        return ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    case 3:
        return ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    case 4:
        return ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    default:
        return 0;
    }
}

// Check if a Unicode codepoint is valid for identifier start (XID_Start + _ + $)
// Simplified: allows Latin letters, Greek, Cyrillic, CJK, and common scripts
static bool is_ident_start_unicode(uint32_t cp)
{
    if (cp < 0x80)
        return isalpha(cp) || cp == '_' || cp == '$';

    // C11/C23 XID_Start compatible ranges (covers most common scripts)
    // See: Unicode Standard Annex #31

    // Latin Extended blocks
    if (cp >= 0x00C0 && cp <= 0x00FF)
        return true; // Latin-1 Supplement
    if (cp >= 0x0100 && cp <= 0x017F)
        return true; // Latin Extended-A
    if (cp >= 0x0180 && cp <= 0x024F)
        return true; // Latin Extended-B
    if (cp >= 0x0250 && cp <= 0x02AF)
        return true; // IPA Extensions
    if (cp >= 0x1E00 && cp <= 0x1EFF)
        return true; // Latin Extended Additional

    // Greek and Coptic
    if (cp >= 0x0370 && cp <= 0x03FF)
        return true;
    if (cp >= 0x1F00 && cp <= 0x1FFF)
        return true; // Greek Extended

    // Cyrillic
    if (cp >= 0x0400 && cp <= 0x04FF)
        return true;
    if (cp >= 0x0500 && cp <= 0x052F)
        return true; // Cyrillic Supplement

    // Armenian
    if (cp >= 0x0530 && cp <= 0x058F)
        return true;

    // Hebrew
    if (cp >= 0x0590 && cp <= 0x05FF)
        return true;

    // Arabic
    if (cp >= 0x0600 && cp <= 0x06FF)
        return true;
    if (cp >= 0x0750 && cp <= 0x077F)
        return true; // Arabic Supplement

    // Devanagari and other Indic scripts
    if (cp >= 0x0900 && cp <= 0x097F)
        return true; // Devanagari
    if (cp >= 0x0980 && cp <= 0x09FF)
        return true; // Bengali
    if (cp >= 0x0A00 && cp <= 0x0A7F)
        return true; // Gurmukhi
    if (cp >= 0x0A80 && cp <= 0x0AFF)
        return true; // Gujarati
    if (cp >= 0x0B00 && cp <= 0x0B7F)
        return true; // Oriya
    if (cp >= 0x0B80 && cp <= 0x0BFF)
        return true; // Tamil
    if (cp >= 0x0C00 && cp <= 0x0C7F)
        return true; // Telugu
    if (cp >= 0x0C80 && cp <= 0x0CFF)
        return true; // Kannada
    if (cp >= 0x0D00 && cp <= 0x0D7F)
        return true; // Malayalam
    if (cp >= 0x0D80 && cp <= 0x0DFF)
        return true; // Sinhala

    // Thai and Lao
    if (cp >= 0x0E00 && cp <= 0x0E7F)
        return true; // Thai
    if (cp >= 0x0E80 && cp <= 0x0EFF)
        return true; // Lao

    // Tibetan
    if (cp >= 0x0F00 && cp <= 0x0FFF)
        return true;

    // Georgian
    if (cp >= 0x10A0 && cp <= 0x10FF)
        return true;

    // Hangul Jamo and Syllables
    if (cp >= 0x1100 && cp <= 0x11FF)
        return true; // Hangul Jamo
    if (cp >= 0xAC00 && cp <= 0xD7AF)
        return true; // Hangul Syllables

    // Ethiopian
    if (cp >= 0x1200 && cp <= 0x137F)
        return true;

    // Cherokee
    if (cp >= 0x13A0 && cp <= 0x13FF)
        return true;

    // Canadian Aboriginal Syllabics
    if (cp >= 0x1400 && cp <= 0x167F)
        return true;

    // Khmer
    if (cp >= 0x1780 && cp <= 0x17FF)
        return true;

    // Mongolian
    if (cp >= 0x1800 && cp <= 0x18AF)
        return true;

    // Hiragana, Katakana, Bopomofo
    if (cp >= 0x3040 && cp <= 0x309F)
        return true; // Hiragana
    if (cp >= 0x30A0 && cp <= 0x30FF)
        return true; // Katakana
    if (cp >= 0x3100 && cp <= 0x312F)
        return true; // Bopomofo
    if (cp >= 0x31A0 && cp <= 0x31BF)
        return true; // Bopomofo Extended
    if (cp >= 0x31F0 && cp <= 0x31FF)
        return true; // Katakana Phonetic Extensions

    // CJK Unified Ideographs (all extensions)
    if (cp >= 0x3400 && cp <= 0x4DBF)
        return true; // CJK Extension A
    if (cp >= 0x4E00 && cp <= 0x9FFF)
        return true; // CJK Unified
    if (cp >= 0xF900 && cp <= 0xFAFF)
        return true; // CJK Compatibility Ideographs
    if (cp >= 0x20000 && cp <= 0x2A6DF)
        return true; // CJK Extension B
    if (cp >= 0x2A700 && cp <= 0x2B73F)
        return true; // CJK Extension C
    if (cp >= 0x2B740 && cp <= 0x2B81F)
        return true; // CJK Extension D
    if (cp >= 0x2B820 && cp <= 0x2CEAF)
        return true; // CJK Extension E
    if (cp >= 0x2CEB0 && cp <= 0x2EBEF)
        return true; // CJK Extension F
    if (cp >= 0x30000 && cp <= 0x3134F)
        return true; // CJK Extension G

    // Mathematical Alphanumeric Symbols (some compilers accept these)
    if (cp >= 0x1D400 && cp <= 0x1D7FF)
        return true;

    // Letterlike Symbols
    if (cp >= 0x2100 && cp <= 0x214F)
        return true;

    return false;
}

// Check if a Unicode codepoint is valid for identifier continuation (XID_Continue)
static bool is_ident_cont_unicode(uint32_t cp)
{
    if (cp < 0x80)
        return isalnum(cp) || cp == '_' || cp == '$';
    if (is_ident_start_unicode(cp))
        return true;
    // Combining marks, modifiers, and other continuation characters
    if (cp >= 0x0300 && cp <= 0x036F)
        return true; // Combining Diacritical Marks
    if (cp >= 0x1DC0 && cp <= 0x1DFF)
        return true; // Combining Diacritical Marks Supplement
    if (cp >= 0x20D0 && cp <= 0x20FF)
        return true; // Combining Diacritical Marks for Symbols
    if (cp >= 0xFE20 && cp <= 0xFE2F)
        return true; // Combining Half Marks
    // Numeric characters (for continuation only)
    if (cp >= 0x0660 && cp <= 0x0669)
        return true; // Arabic-Indic Digits
    if (cp >= 0x06F0 && cp <= 0x06F9)
        return true; // Extended Arabic-Indic Digits
    if (cp >= 0x0966 && cp <= 0x096F)
        return true; // Devanagari Digits
    if (cp >= 0x09E6 && cp <= 0x09EF)
        return true; // Bengali Digits
    if (cp >= 0x0E50 && cp <= 0x0E59)
        return true; // Thai Digits
    if (cp >= 0xFF10 && cp <= 0xFF19)
        return true; // Fullwidth Digits
    return false;
}

// Read a UCN (Universal Character Name) \uXXXX or \UXXXXXXXX
// Returns the number of bytes consumed, or 0 if not a valid UCN
static int read_ucn(char *p, uint32_t *cp)
{
    if (p[0] != '\\')
        return 0;
    int hex_len = 0;
    if (p[1] == 'u')
        hex_len = 4;
    else if (p[1] == 'U')
        hex_len = 8;
    else
        return 0;

    uint32_t val = 0;
    for (int i = 0; i < hex_len; i++)
    {
        int h = from_hex(p[2 + i]);
        if (h < 0)
            return 0;
        val = (val << 4) | h;
    }
    *cp = val;
    return 2 + hex_len; // \ + u/U + hex digits
}

// Tokenizer helpers
static int read_ident(char *start)
{
    char *p = start;
    uint32_t cp;
    int len;

    // Check for UCN at start
    len = read_ucn(p, &cp);
    if (len > 0)
    {
        if (!is_ident_start_unicode(cp))
            return 0;
        p += len;
    }
    else
    {
        // Check for UTF-8 or ASCII start
        cp = decode_utf8(p, &len);
        if (!is_ident_start_unicode(cp))
            return 0;
        p += len;
    }

    // Continue reading identifier characters
    while (*p)
    {
        len = read_ucn(p, &cp);
        if (len > 0)
        {
            if (!is_ident_cont_unicode(cp))
                break;
            p += len;
            continue;
        }
        cp = decode_utf8(p, &len);
        if (!is_ident_cont_unicode(cp))
            break;
        p += len;
    }

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

// Digraph translation table
typedef struct
{
    char *digraph;
    int len;
    char *equiv;
} Digraph;
static Digraph digraphs[] = {
    {"%:%:", 4, "##"}, // Must come before %:
    {"<:", 2, "["},
    {":>", 2, "]"},
    {"<%", 2, "{"},
    {"%>", 2, "}"},
    {"%:", 2, "#"},
};

static int read_punct(char *p)
{
    // Check for digraphs first
    for (size_t i = 0; i < sizeof(digraphs) / sizeof(*digraphs); i++)
        if (!strncmp(p, digraphs[i].digraph, digraphs[i].len))
            return digraphs[i].len;

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

// Check if an integer literal has unsigned suffix (u, U, ul, UL, ull, ULL, etc.)
static bool has_unsigned_suffix(const char *start, int len)
{
    const char *p = start + len - 1;
    // Skip trailing L/l suffixes
    while (p > start && (*p == 'l' || *p == 'L'))
        p--;
    // Check for U/u suffix
    return p >= start && (*p == 'u' || *p == 'U');
}

static int64_t read_int_literal(char **pp, int base, bool is_unsigned)
{
    char *end;
    int64_t val;
    if (is_unsigned)
    {
        // Use strtoull for unsigned literals to handle full 64-bit range
        val = (int64_t)strtoull(*pp, &end, base);
    }
    else
    {
        val = strtoll(*pp, &end, base);
    }
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
    bool is_unsigned = has_unsigned_suffix(tok->loc, tok->len);
    tok->val.i64 = read_int_literal(&p, base, is_unsigned);
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
    // Intern the filename to allow pointer comparison
    char *interned_name = intern_filename(name ? name : base->name);

    // Check cache for existing File view with same parameters
    File *cached = find_cached_file_view(interned_name, line_delta, is_system, is_include_entry);
    if (cached)
    {
        return cached;
    }

    // Create new File view
    File *file = calloc(1, sizeof(File));
    if (!file)
        error("out of memory");
    file->name = interned_name; // Use interned string (no strdup needed)
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

    // Cache this file view
    cache_file_view(interned_name, line_delta, is_system, is_include_entry, file);

    add_input_file(file);

    return file;
}

// Scan a preprocessor directive starting at '#'
// Returns new position after directive, or NULL if not a line marker (caller handles as TK_PREP_DIR)
// Updates *in_system_include, *line_no, and may switch current_file
static char *scan_line_directive(char *p, File *base_file, int *line_no, bool *in_system_include)
{
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

// Reset state for reuse (keeps arena blocks for reuse)
void tokenizer_reset(void)
{
    free_string_allocs();
    arena_reset();
    // Free file view cache first (before freeing files)
    free_file_view_cache();
    for (int i = 0; i < input_file_count; i++)
        free_file(input_files[i]);
    free(input_files);
    input_files = NULL;
    input_file_count = 0;
    input_file_capacity = 0;
    current_file = NULL;
    // Free interned filenames last (after all files are freed)
    free_filename_intern_map();
}

// Full cleanup - frees all memory including arena blocks
void tokenizer_cleanup(void)
{
    free_string_allocs();
    arena_free();
    // Free file view cache first (before freeing files)
    free_file_view_cache();
    for (int i = 0; i < input_file_count; i++)
        free_file(input_files[i]);
    free(input_files);
    input_files = NULL;
    input_file_count = 0;
    input_file_capacity = 0;
    current_file = NULL;
    // Free interned filenames last (after all files are freed)
    free_filename_intern_map();
}
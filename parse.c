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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef __GNUC__
#define __attribute__(x)
#endif

#define TOMBSTONE ((void *)-1)

// Generic array growth: ensures *arr has capacity for n elements
// Note: Uses error() instead of exit(1) to support PRISM_LIB_MODE where
// error() uses longjmp for recovery instead of terminating the host process.
#define ENSURE_ARRAY_CAP(arr, count, cap, init_cap, T)         \
    do                                                         \
    {                                                          \
        if ((count) >= (cap))                                  \
        {                                                      \
            int new_cap = (cap) == 0 ? (init_cap) : (cap) * 2; \
            while (new_cap < (count))                          \
                new_cap *= 2;                                  \
            T *tmp = realloc((arr), sizeof(T) * new_cap);      \
            if (!tmp)                                          \
                error("out of memory");                        \
            (arr) = tmp;                                       \
            (cap) = new_cap;                                   \
        }                                                      \
    } while (0)

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

// Forward declaration for error reporting (used by arena and hashmap OOM handling)
static noreturn void error(char *fmt, ...);

// Generic arena allocator - unified "linked list of blocks" bump allocator
// Used for both token allocation and string data
#define ARENA_DEFAULT_BLOCK_SIZE (64 * 1024)

typedef struct ArenaBlock ArenaBlock;
struct ArenaBlock
{
    ArenaBlock *next;
    size_t used;
    size_t capacity;
    char data[];
};

typedef struct
{
    ArenaBlock *head;
    ArenaBlock *current;
    size_t default_block_size;
} Arena;

static ArenaBlock *arena_new_block(size_t min_size, size_t default_size)
{
    size_t capacity = default_size;
    if (min_size > capacity)
        capacity = min_size;
    ArenaBlock *block = malloc(sizeof(ArenaBlock) + capacity);
    if (!block)
        error("out of memory allocating arena block");
    block->next = NULL;
    block->used = 0;
    block->capacity = capacity;
    return block;
}

static void *arena_alloc(Arena *arena, size_t size)
{
    if (size == 0)
        size = 1;

    // Align to 8 bytes for proper alignment of any type
    size = (size + 7) & ~(size_t)7;

    if (!arena->current || arena->current->used + size > arena->current->capacity)
    {
        size_t block_size = arena->default_block_size ? arena->default_block_size : ARENA_DEFAULT_BLOCK_SIZE;
        ArenaBlock *block = arena_new_block(size, block_size);
        if (arena->current)
            arena->current->next = block;
        else
            arena->head = block;
        arena->current = block;
    }

    void *ptr = arena->current->data + arena->current->used;
    arena->current->used += size;
    memset(ptr, 0, size);
    return ptr;
}

static void arena_reset(Arena *arena)
{
    for (ArenaBlock *b = arena->head; b; b = b->next)
        b->used = 0;
    arena->current = arena->head;
}

static void arena_free(Arena *arena)
{
    ArenaBlock *b = arena->head;
    while (b)
    {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    arena->head = NULL;
    arena->current = NULL;
}

// Token arena - specialized for Token allocation
#define TOKEN_ARENA_BLOCK_SIZE (4096 * sizeof(Token))
static Arena token_arena = {.default_block_size = TOKEN_ARENA_BLOCK_SIZE};

static Token *arena_alloc_token(void)
{
    return arena_alloc(&token_arena, sizeof(Token));
}

// String arena - for string data (literals, identifiers, etc.)
#define STRING_ARENA_BLOCK_SIZE (64 * 1024)
static Arena string_arena = {.default_block_size = STRING_ARENA_BLOCK_SIZE};

static char *string_arena_alloc(size_t size)
{
    return arena_alloc(&string_arena, size);
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
    HashMap new_map = {.buckets = calloc(newcap, sizeof(HashEntry)), .capacity = newcap};
    if (!new_map.buckets)
        error("out of memory resizing hashmap");
    for (int i = 0; i < map->capacity; i++)
    {
        HashEntry *ent = &map->buckets[i];
        if (ent->key && ent->key != TOMBSTONE)
            hashmap_put(&new_map, ent->key, ent->keylen, ent->val);
    }
    free(map->buckets);
    *map = new_map;
}

static void hashmap_put(HashMap *map, char *key, int keylen, void *val)
{
    if (!map->buckets)
    {
        map->buckets = calloc(64, sizeof(HashEntry));
        if (!map->buckets)
            error("out of memory allocating hashmap");
        map->capacity = 64;
    }
    else if (map->used * 100 / map->capacity >= 70)
    {
        hashmap_resize(map, map->capacity * 2);
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
    if (first_empty < 0)
        return;

    HashEntry *ent = &map->buckets[first_empty];
    ent->key = key;
    ent->keylen = keylen;
    ent->val = val;
    if (ent->key != TOMBSTONE)
        map->used++;
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
// Uses the generic HashMap with a packed composite key
// Key: combines filename pointer, line_delta, is_system, is_include_entry
typedef struct
{
    char *filename; // Interned filename (pointer comparison valid)
    int line_delta;
    uint8_t flags; // is_system (bit 0), is_include_entry (bit 1)
} FileViewKey;

static HashMap file_view_cache = {0};

static File *find_cached_file_view(char *filename, int line_delta, bool is_system, bool is_include_entry)
{
    FileViewKey key = {filename, line_delta, (is_system ? 1 : 0) | (is_include_entry ? 2 : 0)};
    return hashmap_get(&file_view_cache, (char *)&key, sizeof(key));
}

static void cache_file_view(char *filename, int line_delta, bool is_system, bool is_include_entry, File *file)
{
    FileViewKey key = {filename, line_delta, (is_system ? 1 : 0) | (is_include_entry ? 2 : 0)};
    // Need to allocate key storage since HashMap stores pointer to key
    FileViewKey *stored_key = malloc(sizeof(FileViewKey));
    if (!stored_key)
        error("out of memory");
    *stored_key = key;
    hashmap_put(&file_view_cache, (char *)stored_key, sizeof(FileViewKey), file);
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
    // Check by looking up in the intern map (pointer comparison after lookup)
    if (f->name)
    {
        int len = strlen(f->name);
        char *found = hashmap_get(&filename_intern_map, f->name, len);
        if (found != f->name) // Not interned, safe to free
            free(f->name);
    }
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

// Free file view cache keys and clear the map (File structs are freed separately)
static void free_file_view_cache(void)
{
    if (!file_view_cache.buckets)
        return;
    // Free allocated keys
    for (int i = 0; i < file_view_cache.capacity; i++)
    {
        HashEntry *ent = &file_view_cache.buckets[i];
        if (ent->key && ent->key != TOMBSTONE)
            free(ent->key);
    }
    hashmap_clear(&file_view_cache);
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
// Note: va_list scoping is intentional to avoid undefined behavior when
// PRISM_LIB_MODE is defined but prism_error_jmp_set is false.
static noreturn void error(char *fmt, ...)
{
#ifdef PRISM_LIB_MODE
    if (prism_error_jmp_set)
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(prism_error_msg, sizeof(prism_error_msg), fmt, ap);
        va_end(ap);
        longjmp(prism_error_jmp, 1);
    }
#endif
    va_list ap;
    va_start(ap, fmt);
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

    if (tok->len != 2)
        return NULL;

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

    return NULL;
}

static inline bool equal(Token *tok, const char *op)
{
    size_t len = __builtin_constant_p(*op) ? __builtin_strlen(op) : strlen(op);

    if (tok->len == (int)len && !memcmp(tok->loc, op, len))
        return true;

    // Check digraph equivalence
    const char *equiv = digraph_equiv(tok);
    return equiv && strlen(equiv) == len && !memcmp(equiv, op, len);
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

// Unicode XID_Start ranges for identifier start characters (sorted by start)
// See: Unicode Standard Annex #31, C11/C23 compatible
static const struct
{
    uint32_t start, end;
} xid_start_ranges[] = {
    {0x00C0, 0x00FF},   // Latin-1 Supplement
    {0x0100, 0x017F},   // Latin Extended-A
    {0x0180, 0x024F},   // Latin Extended-B
    {0x0250, 0x02AF},   // IPA Extensions
    {0x0370, 0x03FF},   // Greek and Coptic
    {0x0400, 0x04FF},   // Cyrillic
    {0x0500, 0x052F},   // Cyrillic Supplement
    {0x0530, 0x058F},   // Armenian
    {0x0590, 0x05FF},   // Hebrew
    {0x0600, 0x06FF},   // Arabic
    {0x0750, 0x077F},   // Arabic Supplement
    {0x0900, 0x097F},   // Devanagari
    {0x0980, 0x09FF},   // Bengali
    {0x0A00, 0x0A7F},   // Gurmukhi
    {0x0A80, 0x0AFF},   // Gujarati
    {0x0B00, 0x0B7F},   // Oriya
    {0x0B80, 0x0BFF},   // Tamil
    {0x0C00, 0x0C7F},   // Telugu
    {0x0C80, 0x0CFF},   // Kannada
    {0x0D00, 0x0D7F},   // Malayalam
    {0x0D80, 0x0DFF},   // Sinhala
    {0x0E00, 0x0E7F},   // Thai
    {0x0E80, 0x0EFF},   // Lao
    {0x0F00, 0x0FFF},   // Tibetan
    {0x10A0, 0x10FF},   // Georgian
    {0x1100, 0x11FF},   // Hangul Jamo
    {0x1200, 0x137F},   // Ethiopian
    {0x13A0, 0x13FF},   // Cherokee
    {0x1400, 0x167F},   // Canadian Aboriginal Syllabics
    {0x1780, 0x17FF},   // Khmer
    {0x1800, 0x18AF},   // Mongolian
    {0x1E00, 0x1EFF},   // Latin Extended Additional
    {0x1F00, 0x1FFF},   // Greek Extended
    {0x2100, 0x214F},   // Letterlike Symbols
    {0x3040, 0x309F},   // Hiragana
    {0x30A0, 0x30FF},   // Katakana
    {0x3100, 0x312F},   // Bopomofo
    {0x31A0, 0x31BF},   // Bopomofo Extended
    {0x31F0, 0x31FF},   // Katakana Phonetic Extensions
    {0x3400, 0x4DBF},   // CJK Extension A
    {0x4E00, 0x9FFF},   // CJK Unified
    {0xAC00, 0xD7AF},   // Hangul Syllables
    {0xF900, 0xFAFF},   // CJK Compatibility Ideographs
    {0x1D400, 0x1D7FF}, // Mathematical Alphanumeric Symbols
    {0x20000, 0x2A6DF}, // CJK Extension B
    {0x2A700, 0x2B73F}, // CJK Extension C
    {0x2B740, 0x2B81F}, // CJK Extension D
    {0x2B820, 0x2CEAF}, // CJK Extension E
    {0x2CEB0, 0x2EBEF}, // CJK Extension F
    {0x30000, 0x3134F}, // CJK Extension G
};

#define XID_START_RANGE_COUNT (sizeof(xid_start_ranges) / sizeof(xid_start_ranges[0]))

// Check if a Unicode codepoint is valid for identifier start (XID_Start + _ + $)
// Uses binary search over sorted ranges for O(log N) lookup
static bool is_ident_start_unicode(uint32_t cp)
{
    if (cp < 0x80)
        return isalpha(cp) || cp == '_' || cp == '$';

    // Binary search over XID_Start ranges
    int lo = 0, hi = XID_START_RANGE_COUNT - 1;
    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        if (cp < xid_start_ranges[mid].start)
            hi = mid - 1;
        else if (cp > xid_start_ranges[mid].end)
            lo = mid + 1;
        else
            return true; // cp is within range [start, end]
    }
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
// Punctuator entry with precomputed length
typedef struct
{
    const char *str;
    int len;
} Punct;

static int read_punct(char *p)
{
    // Switch-on-first-char optimization: O(1) dispatch instead of O(N) strncmp calls
    switch (*p)
    {
    case '<':
        if (p[1] == '<' && p[2] == '=')
            return 3; // <<=
        if (p[1] == '<')
            return 2; // <<
        if (p[1] == '=')
            return 2; // <=
        if (p[1] == ':')
            return 2; // <: (digraph)
        if (p[1] == '%')
            return 2; // <% (digraph)
        return 1;
    case '>':
        if (p[1] == '>' && p[2] == '=')
            return 3; // >>=
        if (p[1] == '>')
            return 2; // >>
        if (p[1] == '=')
            return 2; // >=
        return 1;
    case '.':
        if (p[1] == '.' && p[2] == '.')
            return 3; // ...
        return 1;
    case '=':
        return (p[1] == '=') ? 2 : 1; // == or =
    case '!':
        return (p[1] == '=') ? 2 : 1; // != or !
    case '-':
        if (p[1] == '>')
            return 2; // ->
        if (p[1] == '=')
            return 2; // -=
        if (p[1] == '-')
            return 2; // --
        return 1;
    case '+':
        if (p[1] == '=')
            return 2; // +=
        if (p[1] == '+')
            return 2; // ++
        return 1;
    case '*':
        return (p[1] == '=') ? 2 : 1; // *= or *
    case '/':
        return (p[1] == '=') ? 2 : 1; // /= or /
    case '%':
        if (p[1] == ':' && p[2] == '%' && p[3] == ':')
            return 4; // %:%: (digraph ##)
        if (p[1] == ':')
            return 2; // %: (digraph #)
        if (p[1] == '>')
            return 2; // %> (digraph })
        if (p[1] == '=')
            return 2; // %=
        return 1;
    case '&':
        if (p[1] == '&')
            return 2; // &&
        if (p[1] == '=')
            return 2; // &=
        return 1;
    case '|':
        if (p[1] == '|')
            return 2; // ||
        if (p[1] == '=')
            return 2; // |=
        return 1;
    case '^':
        return (p[1] == '=') ? 2 : 1; // ^= or ^
    case '#':
        return (p[1] == '#') ? 2 : 1; // ## or #
    case ':':
        return (p[1] == '>') ? 2 : 1; // :> (digraph) or :
    default:
        return ispunct((unsigned char)*p) ? 1 : 0;
    }
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

// Scan C++11/C23 raw string literal: R"delimiter(content)delimiter"
// p points to the opening quote after 'R' prefix (or after LR, uR, etc.)
// Returns pointer past the closing quote, or NULL if not a valid raw string
static char *raw_string_literal_end(char *p)
{
    // p points to '"' - extract delimiter between '"' and '('
    char *delim_start = p + 1;
    char *paren = delim_start;
    while (*paren && *paren != '(' && *paren != ')' && *paren != '\\' &&
           *paren != ' ' && *paren != '\t' && *paren != '\n' && (paren - delim_start) < 16)
        paren++;

    if (*paren != '(')
        return NULL; // Not a valid raw string literal

    int delim_len = paren - delim_start;
    char *content = paren + 1;

    // Search for )delimiter"
    for (char *q = content; *q; q++)
    {
        if (*q == ')' &&
            (delim_len == 0 || strncmp(q + 1, delim_start, delim_len) == 0) &&
            q[1 + delim_len] == '"')
        {
            return q + 1 + delim_len + 1; // Past the closing quote
        }
    }

    error_at(p, "unclosed raw string literal");
    return NULL;
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

    char *buf = string_arena_alloc(buf_size);
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

// Read a C++11/C23 raw string literal token
// start points to the beginning of the token (R, or prefix like L, u, U, u8)
// quote points to the opening '"'
static Token *read_raw_string_literal(char *start, char *quote)
{
    char *end = raw_string_literal_end(quote);
    if (!end)
        error_at(start, "invalid raw string literal");

    // For raw strings, we store the content between ( and ) without escape processing
    // Find the delimiter and content boundaries
    char *delim_start = quote + 1;
    char *paren = delim_start;
    while (*paren != '(')
        paren++;
    int delim_len = paren - delim_start;
    char *content_start = paren + 1;
    char *content_end = end - 2 - delim_len; // Back from )" or )delim"

    size_t content_len = content_end - content_start;
    char *buf = string_arena_alloc(content_len + 1);
    memcpy(buf, content_start, content_len);
    buf[content_len] = '\0';

    Token *tok = new_token(TK_STR, start, end);
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
    if ((e[-3] | 0x20) != 'f' || e[-1] < '2' || e[-1] > '6')
        return 0;

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

    return 0;
}

// Convert preprocessor number token to TK_NUM.
// We detect floats (for C23 extended suffix rewriting) but don't parse values.
static void convert_pp_number(Token *tok)
{
    tok->kind = TK_NUM;

    char *p = tok->loc;
    bool is_hex = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'));

    // Detect floats by scanning for '.', or exponents
    for (char *q = tok->loc; q < tok->loc + tok->len; q++)
    {
        // '.' always indicates float
        if (*q == '.')
        {
            tok->flags |= TF_IS_FLOAT;
            return;
        }
        // 'p'/'P' exponent (hex floats)
        if (*q == 'p' || *q == 'P')
        {
            tok->flags |= TF_IS_FLOAT;
            return;
        }
        // 'e'/'E' exponent (decimal only - in hex these are valid digits)
        if (!is_hex && (*q == 'e' || *q == 'E'))
        {
            tok->flags |= TF_IS_FLOAT;
            return;
        }
    }

    // Check for C23 extended float suffixes (F16, F32, F64, F128, BF16)
    // Only for decimal numbers - hex numbers can have these as valid hex digits
    if (!is_hex && get_extended_float_suffix(tok->loc, tok->len, NULL))
        tok->flags |= TF_IS_FLOAT;
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
    ENSURE_ARRAY_CAP(input_files, input_file_count + 1, input_file_capacity, 16, File *);
    input_files[input_file_count++] = file;
}

static File *new_file_view(const char *name, File *base, int line_delta, bool is_system, bool is_include_entry)
{
    // Intern the filename to allow pointer comparison
    char *interned_name = intern_filename(name ? name : base->name);

    // Check cache for existing File view with same parameters
    File *cached = find_cached_file_view(interned_name, line_delta, is_system, is_include_entry);
    if (cached)
        return cached;

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
        // Handle exponent signs: e+, e-, E+, E-, p+, p-, P+, P- (hex floats)
        char c = *p;
        if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') &&
            (p[1] == '+' || p[1] == '-'))
        {
            p += 2;
        }
        else if (isalnum(c) || c == '.' || c == '_')
        {
            // Accept digits, letters (hex, suffixes, extensions), dot, underscore
            p++;
        }
        else if (c == '\'' && (isxdigit(p[1]) || isdigit(p[1])))
        {
            // C23 digit separator: accept ' if followed by a digit (hex or decimal)
            p++;
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
        // C++11/C23 raw string literals: R"...", LR"...", uR"...", UR"...", u8R"..."
        if (p[0] == 'R' && p[1] == '"')
        {
            cur = cur->next = read_raw_string_literal(p, p + 1);
            p += cur->len;
            continue;
        }
        if ((p[0] == 'L' || p[0] == 'u' || p[0] == 'U') && p[1] == 'R' && p[2] == '"')
        {
            cur = cur->next = read_raw_string_literal(p, p + 2);
            p += cur->len;
            continue;
        }
        if (p[0] == 'u' && p[1] == '8' && p[2] == 'R' && p[3] == '"')
        {
            cur = cur->next = read_raw_string_literal(p, p + 3);
            p += cur->len;
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
    arena_reset(&string_arena);
    arena_reset(&token_arena);
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
    arena_free(&string_arena);
    arena_free(&token_arena);
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
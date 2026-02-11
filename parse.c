// parse.c - C tokenizer (preprocessor removed - use cc -E)
// Original: https://github.com/rui314/chibicc (MIT License)
//
// API:
//   Token *tokenize_file(path)  - Tokenize a file
//   Token *tokenize(File *file) - Tokenize from File struct
//
// Usage: Run "cc -E -P input.c" first, then tokenize the output.

#ifdef _WIN32
#include "windows.c"
#else
#define _POSIX_C_SOURCE 200809L
#include <stdnoreturn.h>
#include <unistd.h>
#endif

#include <ctype.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#if !defined(__GNUC__) && !defined(__attribute__)
#define __attribute__(x)
#endif

#define TOMBSTONE ((void *)-1)
#define IS_DIGIT(c) ((unsigned)(c) - '0' < 10u)
#define IS_ALPHA(c) (((unsigned)((c) | 0x20) - 'a') < 26u || (c) == '_' || (c) == '$')
#define IS_ALNUM(c) (IS_DIGIT(c) || IS_ALPHA(c))
#define IS_XDIGIT(c) (IS_DIGIT(c) || ((unsigned)((c) | 0x20) - 'a') < 6u)
#define ARENA_DEFAULT_BLOCK_SIZE (64 * 1024)
#define KW_MARKER 0x80000000UL // Internal marker bit for keyword map: values are (tag | KW_MARKER)
#define TOKEN_ALLOC_SIZE (((int)sizeof(Token) + 7) & ~7)

#define equal(tok, s)                                                                                                                               \
    (__builtin_constant_p(s) ? (__builtin_strlen(s) == 1 ? _equal_1(tok, (s)[0]) : __builtin_strlen(s) == 2 ? _equal_2(tok, s)                      \
                                                                                                            : equal_n(tok, s, __builtin_strlen(s))) \
                             : equal_n(tok, s, strlen(s)))

#define KEYWORD_HASH(key, len)                                         \
    (((unsigned)(len) * 2 + (unsigned char)(key)[0] * 99 +             \
      (unsigned char)(key)[1] * 125 +                                  \
      (unsigned char)((len) > 6 ? (key)[6] : (key)[(len) - 1]) * 69) & \
     255)

// Uses error() for OOM to support PRISM_LIB_MODE longjmp recovery
#define ENSURE_ARRAY_CAP(arr, count, cap, init_cap, T)                    \
    do                                                                    \
    {                                                                     \
        if ((count) >= (cap))                                             \
        {                                                                 \
            size_t new_cap = (cap) == 0 ? (init_cap) : (size_t)(cap) * 2; \
            while (new_cap < (size_t)(count))                             \
                new_cap *= 2;                                             \
            T *old_ptr_ = (arr);                                          \
            T *tmp = realloc((arr), sizeof(T) * new_cap);                 \
            if (!tmp)                                                     \
            {                                                             \
                free(old_ptr_);                                           \
                (arr) = NULL;                                             \
                (cap) = 0;                                                \
                error("out of memory");                                   \
            }                                                             \
            (arr) = tmp;                                                  \
            (cap) = new_cap;                                              \
        }                                                                 \
    } while (0)

// Lookup table for identifier-continuation chars (alnum + _ + $ + bytes >= 0x80)
#ifdef _WIN32
// MSVC: no GCC range designators — initialize at startup
static uint8_t ident_char[256];
static void init_ident_char(void)
{
    for (int c = '0'; c <= '9'; c++)
        ident_char[c] = 1;
    for (int c = 'A'; c <= 'Z'; c++)
        ident_char[c] = 1;
    for (int c = 'a'; c <= 'z'; c++)
        ident_char[c] = 1;
    ident_char['_'] = 1;
    ident_char['$'] = 1;
    for (int c = 0x80; c <= 0xFF; c++)
        ident_char[c] = 1;
}
#else
static const uint8_t ident_char[256] = {
    ['0' ... '9'] = 1,
    ['A' ... 'Z'] = 1,
    ['a' ... 'z'] = 1,
    ['_'] = 1,
    ['$'] = 1,
    [0x80 ... 0xFF] = 1,
};
#endif

typedef struct Token Token;
typedef struct ArenaBlock ArenaBlock;

// File info
typedef struct
{
    char *name;
    int file_no;
    char *contents;
    size_t contents_len;
    char *display_name;
    int line_delta;
    bool owns_contents;
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
    TK_PREP_DIR, // Preprocessor directive (e.g., #pragma) to preserve
    TK_EOF,
} TokenKind;

// Token flags
enum
{
    TF_AT_BOL = 1 << 0,
    TF_HAS_SPACE = 1 << 1,
    TF_IS_FLOAT = 1 << 2,
    TF_IS_DIGRAPH = 1 << 3,
};

// Token tags - bitmask classification assigned once at tokenize time
// Eliminates repeated string comparisons in the transpiler
enum
{
    TT_TYPE = 1 << 0,         // Type keyword (int, char, void, struct, etc.)
    TT_QUALIFIER = 1 << 1,    // Type qualifier (const, volatile, restrict, static, auto, register, _Atomic, _Alignas, __attribute__)
    TT_SUE = 1 << 2,          // struct/union/enum
    TT_SKIP_DECL = 1 << 3,    // Keywords that can't start a zero-init declaration
    TT_ATTR = 1 << 4,         // Attribute keyword (__attribute__, __attribute, __declspec)
    TT_ASSIGN = 1 << 5,       // Assignment or compound assignment operator (=, +=, ++, --, [)
    TT_MEMBER = 1 << 6,       // Member access operator (. or ->)
    TT_LOOP = 1 << 7,         // Loop keyword (for, while, do)
    TT_CONTROL = 1 << 8,      // Control flow keyword (if, else, for, while, do, switch)
    TT_ASM = 1 << 9,          // Inline assembly (asm, __asm__, __asm)
    TT_INLINE = 1 << 10,      // inline, __inline, __inline__
    TT_NORETURN_FN = 1 << 11, // Noreturn function identifier (exit, abort, etc.)
    TT_SETJMP_FN = 1 << 12,   // setjmp/longjmp family identifier
    TT_VFORK_FN = 1 << 13,    // vfork identifier
    // Dispatch tags for main loop (bits 14-24)
    TT_RETURN = 1 << 14,   // return
    TT_BREAK = 1 << 15,    // break
    TT_CONTINUE = 1 << 16, // continue
    TT_GOTO = 1 << 17,     // goto
    TT_CASE = 1 << 18,     // case
    TT_DEFAULT = 1 << 19,  // default
    TT_DEFER = 1 << 20,    // defer
    TT_GENERIC = 1 << 21,  // _Generic
    TT_SWITCH = 1 << 22,   // switch
    TT_IF = 1 << 23,       // if, else
    TT_TYPEDEF = 1 << 24,  // typedef
    // Specific keyword ID tags (bits 25-29) - eliminates equal() in parse_type_specifier
    TT_VOLATILE = 1 << 25, // volatile
    TT_REGISTER = 1 << 26, // register
    TT_TYPEOF = 1 << 27,   // typeof, typeof_unqual, __typeof__, __typeof
    TT_BITINT = 1 << 28,   // _BitInt
    TT_ALIGNAS = 1 << 29,  // _Alignas, alignas
    TT_ORELSE = 1 << 30,   // orelse
};

struct Token
{
    char *loc;
    Token *next;
    int len;
    int line_no; // Cached line number (computed once during tokenization)
    TokenKind kind;
    uint16_t file_idx;
    uint8_t flags;
    uint32_t tag; // TT_* bitmask - token classification
};

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
    int tombstones;
} HashMap;

// Perfect hash keyword table — O(1) lookup with single memcmp
typedef struct
{
    char *name;
    uint8_t len;
    uintptr_t value;
} KeywordEntry;

static KeywordEntry keyword_perfect[256];

enum // Feature flags
{
    F_DEFER = 1,
    F_ZEROINIT = 2,
    F_LINE_DIR = 4,
    F_WARN_SAFETY = 8,
    F_FLATTEN = 16,
    F_ORELSE = 32
};

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

typedef struct PrismContext
{
    Arena main_arena;
    File *current_file;
    File **input_files;
    int input_file_count;
    int input_file_capacity;
    bool at_bol;
    bool has_space;
    int tok_line_no; // Current line number during tokenization
    HashMap filename_intern_map;
    HashMap file_view_cache;
    HashMap keyword_map;
#ifdef PRISM_LIB_MODE
    jmp_buf error_jmp;
    bool error_jmp_set;
    char error_msg[1024];
    int error_line;
    int error_col;
#endif
    uint32_t features; // F_DEFER | F_ZEROINIT | F_LINE_DIR | F_WARN_SAFETY | F_FLATTEN | F_ORELSE
    const char *extra_compiler;
    const char **extra_compiler_flags;
    int extra_compiler_flags_count;
    const char **extra_include_paths;
    int extra_include_count;
    const char **extra_defines;
    int extra_define_count;
    const char **extra_force_includes;
    int extra_force_include_count;
    int struct_depth;
    int defer_depth;
    int conditional_block_depth;
    int generic_paren_depth;
    bool current_func_returns_void;
    bool current_func_has_setjmp;
    bool current_func_has_asm;
    bool current_func_has_vfork;
    int stmt_expr_count;
    bool last_system_header;
    int last_line_no;
    char *last_filename;
    bool at_stmt_start;
    int system_include_count;
    unsigned long long ret_counter;
#ifdef PRISM_LIB_MODE
    char active_temp_output[PATH_MAX];
#endif
} PrismContext;

typedef struct
{
    char *filename; // Interned filename (pointer comparison valid)
    int line_delta;
    uint8_t flags; // is_system (bit 0), is_include_entry (bit 1)
} FileViewKey;

typedef struct
{
    char c1, c2;
    const char *equiv;
} Digraph;

static const Digraph digraphs[] = {{'<', ':', "["}, {':', '>', "]"}, {'<', '%', "{"}, {'%', '>', "}"}, {'%', ':', "#"}, {0, 0, NULL}};

static PrismContext *ctx = NULL;

static noreturn void error(char *fmt, ...);
static void hashmap_put(HashMap *map, char *key, int keylen, void *val);

static inline bool tok_at_bol(Token *tok) { return tok->flags & TF_AT_BOL; }
static inline bool tok_has_space(Token *tok) { return tok->flags & TF_HAS_SPACE; }
static inline void tok_set_at_bol(Token *tok, bool v) { tok->flags = v ? (tok->flags | TF_AT_BOL) : (tok->flags & ~TF_AT_BOL); }
static inline void tok_set_has_space(Token *tok, bool v) { tok->flags = v ? (tok->flags | TF_HAS_SPACE) : (tok->flags & ~TF_HAS_SPACE); }

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

// Ensure arena has room for 'size' bytes, growing if needed
static void arena_ensure(Arena *arena, size_t size)
{
    if (arena->current && arena->current->used + size <= arena->current->capacity)
        return;
    // Try reusing the next block in the chain (from a previous arena_reset cycle)
    if (arena->current && arena->current->next &&
        arena->current->next->used + size <= arena->current->next->capacity)
    {
        arena->current = arena->current->next;
        return;
    }
    size_t block_size = arena->default_block_size ? arena->default_block_size : ARENA_DEFAULT_BLOCK_SIZE;
    ArenaBlock *block = arena_new_block(size, block_size);
    if (arena->current)
    {
        // Preserve the rest of the chain so arena_free can free all blocks
        block->next = arena->current->next;
        arena->current->next = block;
    }
    else
        arena->head = block;
    arena->current = block;
}

// Like arena_alloc but without zeroing - caller must initialize all fields
static void *arena_alloc_uninit(Arena *arena, size_t size)
{
    if (size == 0)
        size = 1;
    size = (size + 7) & ~(size_t)7;
    arena_ensure(arena, size);
    void *ptr = arena->current->data + arena->current->used;
    arena->current->used += size;
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

static void prism_ctx_init(void)
{
    if (ctx)
        return;
    ctx = calloc(1, sizeof(PrismContext));
    if (!ctx)
    {
        fprintf(stderr, "prism: out of memory\n");
        exit(1);
    }
    ctx->main_arena.default_block_size = ARENA_DEFAULT_BLOCK_SIZE;
    ctx->features = F_DEFER | F_ZEROINIT | F_LINE_DIR | F_FLATTEN | F_ORELSE;
    ctx->at_stmt_start = true;
#ifdef _WIN32
    init_ident_char();
#endif
}

static inline Token *arena_alloc_token(void)
{
    Arena *arena = &ctx->main_arena;
    ArenaBlock *blk = arena->current;
    if (__builtin_expect(blk && blk->used + TOKEN_ALLOC_SIZE <= blk->capacity, 1))
    {
        Token *tok = (Token *)(blk->data + blk->used);
        blk->used += TOKEN_ALLOC_SIZE;
        return tok;
    }
    return arena_alloc_uninit(arena, sizeof(Token));
}

static inline uint64_t fnv_hash(char *s, int len)
{
    uint64_t hash = 0xcbf29ce484222325;
    for (int i = 0; i < len; i++)
    {
        hash ^= (unsigned char)s[i];
        hash *= 0x100000001b3;
    }
    return hash;
}

static void *hashmap_get(HashMap *map, char *key, int keylen)
{
    if (!map->buckets)
        return NULL;
    uint64_t hash = fnv_hash(key, keylen);
    int mask = map->capacity - 1; // capacity is always power-of-2
    for (int i = 0; i <= mask; i++)
    {
        HashEntry *ent = &map->buckets[(hash + i) & mask];
        if (ent->key && ent->key != TOMBSTONE &&
            ent->keylen == keylen && !memcmp(ent->key, key, keylen))
            return ent->val;
        if (!ent->key)
            return NULL;
    }
    return NULL;
}

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
    else if ((map->used + map->tombstones) * 100 / map->capacity >= 70)
    {
        // Compact in place when tombstones dominate; otherwise double capacity
        int newcap = (map->tombstones > map->used) ? map->capacity : map->capacity * 2;
        hashmap_resize(map, newcap);
    }

    uint64_t hash = fnv_hash(key, keylen);
    int mask = map->capacity - 1;
    int first_empty = -1;

    for (int i = 0; i <= mask; i++)
    {
        int idx = (hash + i) & mask;
        HashEntry *ent = &map->buckets[idx];

        // Check for existing key to update
        if (ent->key && ent->key != TOMBSTONE &&
            ent->keylen == keylen && !memcmp(ent->key, key, keylen))
        {
            ent->val = val; // Update existing entry
            return;
        }

        // Remember first empty slot
        if (first_empty < 0 && (!ent->key || ent->key == TOMBSTONE))
            first_empty = idx;

        // Stop at truly empty slot (not tombstone)
        if (!ent->key)
            break;
    }

    // Insert into first empty slot
    if (first_empty < 0)
        return;

    HashEntry *ent = &map->buckets[first_empty];
    if (ent->key == TOMBSTONE)
        map->tombstones--;
    ent->key = key;
    ent->keylen = keylen;
    ent->val = val;
    map->used++;
}

static void hashmap_delete(HashMap *map, char *key, int keylen)
{
    if (!map->buckets)
        return;
    uint64_t hash = fnv_hash(key, keylen);
    int mask = map->capacity - 1;
    for (int i = 0; i <= mask; i++)
    {
        HashEntry *ent = &map->buckets[(hash + i) & mask];
        if (ent->key && ent->key != TOMBSTONE &&
            ent->keylen == keylen && !memcmp(ent->key, key, keylen))
        {
            ent->key = TOMBSTONE;
            map->used--;
            map->tombstones++;
            return;
        }
        if (!ent->key)
            return;
    }
}

static void hashmap_clear(HashMap *map)
{
    free(map->buckets);
    map->buckets = NULL;
    map->capacity = 0;
    map->used = 0;
    map->tombstones = 0;
}

// Free all keys in a hashmap and clear it
static void hashmap_free_keys(HashMap *map)
{
    if (!map->buckets)
        return;
    for (int i = 0; i < map->capacity; i++)
    {
        HashEntry *ent = &map->buckets[i];
        if (ent->key && ent->key != TOMBSTONE)
            free(ent->key);
    }
    hashmap_clear(map);
}

// Filename interning - avoids duplicating identical filename strings
// Each entry maps filename string -> interned copy
static char *intern_filename(const char *name)
{
    if (!name)
        return NULL;
    int len = strlen(name);
    char *existing = hashmap_get(&ctx->filename_intern_map, (char *)name, len);
    if (existing)
        return existing;
    // Allocate and store new interned string
    char *interned = malloc(len + 1);
    if (!interned)
        error("out of memory");
    memcpy(interned, name, len + 1);
    hashmap_put(&ctx->filename_intern_map, interned, len, interned);
    return interned;
}

static File *find_cached_file_view(char *filename, int line_delta, bool is_system, bool is_include_entry)
{
    FileViewKey key;
    memset(&key, 0, sizeof(key)); // Zero padding bytes for reliable hash/memcmp
    key.filename = filename;
    key.line_delta = line_delta;
    key.flags = (is_system ? 1 : 0) | (is_include_entry ? 2 : 0);
    return hashmap_get(&ctx->file_view_cache, (char *)&key, sizeof(key));
}

static void cache_file_view(char *filename, int line_delta, bool is_system, bool is_include_entry, File *file)
{
    // Need to allocate key storage since HashMap stores pointer to key
    FileViewKey *stored_key = calloc(1, sizeof(FileViewKey)); // calloc zeros padding bytes
    if (!stored_key)
        error("out of memory");
    stored_key->filename = filename;
    stored_key->line_delta = line_delta;
    stored_key->flags = (is_system ? 1 : 0) | (is_include_entry ? 2 : 0);
    hashmap_put(&ctx->file_view_cache, (char *)stored_key, sizeof(FileViewKey), file);
}

static void free_file(File *f)
{
    if (!f)
        return;
    if (f->contents && f->owns_contents)
        free(f->contents);
    // Don't free interned filenames - they're managed by filename_intern_map
    // Check by looking up in the intern map (pointer comparison after lookup)
    if (f->name)
    {
        int len = strlen(f->name);
        char *found = hashmap_get(&ctx->filename_intern_map, f->name, len);
        if (found != f->name) // Not interned, safe to free
            free(f->name);
    }
    free(f);
}

static void free_filename_intern_map(void) { hashmap_free_keys(&ctx->filename_intern_map); }
static void free_file_view_cache(void) { hashmap_free_keys(&ctx->file_view_cache); }

static inline File *tok_file(Token *tok)
{
    if (!tok || tok->file_idx >= ctx->input_file_count)
        return ctx->current_file;
    return ctx->input_files[tok->file_idx];
}

static int tok_line_no(Token *tok) { return tok->line_no; }

// Error handling
// Note: va_list scoping is intentional to avoid undefined behavior when
// PRISM_LIB_MODE is defined but ctx->error_jmp_set is false.
static noreturn void error(char *fmt, ...)
{
#ifdef PRISM_LIB_MODE
    if (ctx && ctx->error_jmp_set)
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(ctx->error_msg, sizeof(ctx->error_msg), fmt, ap);
        va_end(ap);
        longjmp(ctx->error_jmp, 1);
    }
#endif
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void verror_at(char *filename, char *input, int line_no, char *loc, const char *fmt, va_list ap)
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
    if (ctx->error_jmp_set)
    {
        ctx->error_line = count_lines(ctx->current_file->contents, loc);
        vsnprintf(ctx->error_msg, sizeof(ctx->error_msg), fmt, ap);
        va_end(ap);
        longjmp(ctx->error_jmp, 1);
    }
#endif
    verror_at(ctx->current_file->name, ctx->current_file->contents, count_lines(ctx->current_file->contents, loc), loc, fmt, ap);
    va_end(ap);
    exit(1);
}

noreturn void error_tok(Token *tok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    File *f = tok_file(tok);
#ifdef PRISM_LIB_MODE
    if (ctx->error_jmp_set)
    {
        ctx->error_line = tok_line_no(tok);
        vsnprintf(ctx->error_msg, sizeof(ctx->error_msg), fmt, ap);
        va_end(ap);
        longjmp(ctx->error_jmp, 1);
    }
#endif
    verror_at(f->name, f->contents, tok_line_no(tok), tok->loc, fmt, ap);
    va_end(ap);
    exit(1);
}

static void warn_tok(Token *tok, const char *fmt, ...)
{
#ifdef PRISM_LIB_MODE
    (void)tok;
    (void)fmt;
    return; // Suppress warnings in library mode — don't pollute host's stderr
#else
    va_list ap;
    va_start(ap, fmt);
    File *f = tok_file(tok);
    verror_at(f->name, f->contents, tok_line_no(tok), tok->loc, fmt, ap);
    va_end(ap);
#endif
}

static inline const char *digraph_equiv(Token *tok)
{
    if (tok->kind != TK_PUNCT)
        return NULL;
    if (tok->len == 4 && !memcmp(tok->loc, "%:%:", 4))
        return "##";
    if (tok->len != 2)
        return NULL;
    for (const Digraph *d = digraphs; d->equiv; d++)
        if (tok->loc[0] == d->c1 && tok->loc[1] == d->c2)
            return d->equiv;
    return NULL;
}

static inline bool equal_n(Token *tok, const char *op, size_t len)
{
    if (tok->len == (int)len && !memcmp(tok->loc, op, len))
        return true;

    // Only check digraph equivalence if token was flagged as a digraph
    if (!(tok->flags & TF_IS_DIGRAPH))
        return false;
    const char *equiv = digraph_equiv(tok);
    if (!equiv)
        return false;
    // digraph equivalents are 1 or 2 chars, compute length directly
    size_t elen = equiv[0] ? (equiv[1] ? 2 : 1) : 0;
    return elen == len && !memcmp(equiv, op, len);
}

// Cold path: check digraph equivalence for single-char comparison
static bool __attribute__((noinline)) _equal_1_digraph(Token *tok, char c)
{
    const char *e = digraph_equiv(tok);
    return e && e[0] == c && !e[1];
}

// Fast inline path for single-char comparisons (avoids function call + memcmp)
static inline bool _equal_1(Token *tok, char c)
{
    if (tok->len == 1)
        return tok->loc[0] == c;
    if (__builtin_expect(tok->flags & TF_IS_DIGRAPH, 0))
        return _equal_1_digraph(tok, c);
    return false;
}

// Cold path: check digraph equivalence for two-char comparison
static bool __attribute__((noinline)) _equal_2_digraph(Token *tok, const char *s)
{
    const char *e = digraph_equiv(tok);
    return e && e[0] == s[0] && e[1] == s[1];
}

// Fast inline path for two-char comparisons
static inline bool _equal_2(Token *tok, const char *s)
{
    if (tok->len == 2)
        return tok->loc[0] == s[0] && tok->loc[1] == s[1];
    if (__builtin_expect(tok->flags & TF_IS_DIGRAPH, 0))
        return _equal_2_digraph(tok, s);
    return false;
}

static inline uintptr_t keyword_lookup(char *key, int keylen)
{
    if (keylen < 2)
        return 0; // No keyword is shorter than 2 chars
    unsigned slot = KEYWORD_HASH(key, keylen);
    KeywordEntry *ent = &keyword_perfect[slot];
    if (ent->len == keylen && !memcmp(ent->name, key, keylen))
        return ent->value;
    return 0;
}

static void init_keyword_map(void)
{
    // Each entry: {keyword, TT_* tag bitmask}
    // Tags are assigned once here, then stored on tokens during convert_pp_tokens
    static struct
    {
        char *name;
        uint32_t tag;
    } kw[] = {
        // Control flow (skip-decl is set on all; can't start a zero-init declaration)
        {"return", TT_SKIP_DECL | TT_RETURN},
        {"if", TT_SKIP_DECL | TT_CONTROL | TT_IF},
        {"else", TT_SKIP_DECL | TT_CONTROL | TT_IF},
        {"for", TT_SKIP_DECL | TT_LOOP | TT_CONTROL},
        {"while", TT_SKIP_DECL | TT_LOOP | TT_CONTROL},
        {"do", TT_SKIP_DECL | TT_LOOP | TT_CONTROL},
        {"switch", TT_SKIP_DECL | TT_CONTROL | TT_SWITCH},
        {"case", TT_SKIP_DECL | TT_CASE},
        {"default", TT_SKIP_DECL | TT_DEFAULT},
        {"break", TT_SKIP_DECL | TT_BREAK},
        {"continue", TT_SKIP_DECL | TT_CONTINUE},
        {"goto", TT_SKIP_DECL | TT_GOTO},
        {"sizeof", TT_SKIP_DECL},
        {"alignof", TT_SKIP_DECL},
        {"_Alignof", TT_SKIP_DECL},
        {"_Generic", TT_SKIP_DECL | TT_GENERIC},
        {"_Static_assert", 0},
        // struct/union/enum (also type keywords)
        {"struct", TT_TYPE | TT_SUE},
        {"union", TT_TYPE | TT_SUE},
        {"enum", TT_TYPE | TT_SUE},
        // Storage class / qualifiers that also skip decl
        {"typedef", TT_SKIP_DECL | TT_TYPEDEF},
        {"static", TT_QUALIFIER | TT_SKIP_DECL},
        {"extern", TT_SKIP_DECL},
        {"inline", TT_INLINE},
        // Type qualifiers
        {"const", TT_QUALIFIER},
        {"volatile", TT_QUALIFIER | TT_VOLATILE},
        {"restrict", TT_QUALIFIER},
        {"_Atomic", TT_QUALIFIER | TT_TYPE},
        {"_Noreturn", 0},
        {"__inline", TT_INLINE},
        {"__inline__", TT_INLINE},
        {"_Thread_local", 0},
        // Type keywords
        {"void", TT_TYPE},
        {"char", TT_TYPE},
        {"short", TT_TYPE},
        {"int", TT_TYPE},
        {"long", TT_TYPE},
        {"float", TT_TYPE},
        {"double", TT_TYPE},
        {"signed", TT_TYPE},
        {"unsigned", TT_TYPE},
        {"_Bool", TT_TYPE},
        {"bool", TT_TYPE},
        {"_Complex", TT_TYPE},
        {"_Imaginary", TT_TYPE},
        {"__int128", TT_TYPE},
        {"__int128_t", TT_TYPE},
        {"__uint128", TT_TYPE},
        {"__uint128_t", TT_TYPE},
        {"typeof_unqual", TT_TYPE | TT_TYPEOF},
        {"auto", TT_QUALIFIER},
        {"register", TT_QUALIFIER | TT_REGISTER},
        {"_Alignas", TT_QUALIFIER | TT_ALIGNAS},
        {"alignas", TT_QUALIFIER | TT_ALIGNAS},
        {"typeof", TT_TYPE | TT_TYPEOF},
        {"__typeof__", TT_TYPE | TT_TYPEOF},
        {"__typeof", TT_TYPE | TT_TYPEOF},
        {"_BitInt", TT_TYPE | TT_BITINT},
        // Asm (skip-decl: can't start a declaration)
        {"asm", TT_SKIP_DECL | TT_ASM},
        {"__asm__", TT_SKIP_DECL | TT_ASM},
        {"__asm", TT_SKIP_DECL | TT_ASM},
        // Attributes
        {"__attribute__", TT_ATTR | TT_QUALIFIER},
        {"__attribute", TT_ATTR | TT_QUALIFIER},
        {"__declspec", TT_ATTR | TT_QUALIFIER},
        // Other builtins
        {"__extension__", 0},
        {"__builtin_va_list", 0},
        {"__builtin_va_arg", 0},
        {"__builtin_offsetof", 0},
        {"__builtin_types_compatible_p", 0},
        // Prism keywords
        {"defer", TT_DEFER},
        {"orelse", TT_ORELSE},
        {"raw", 0},
    };
    for (size_t i = 0; i < sizeof(kw) / sizeof(*kw); i++)
        hashmap_put(&ctx->keyword_map, kw[i].name, strlen(kw[i].name),
                    (void *)(uintptr_t)(kw[i].tag | KW_MARKER));

    // Tagged identifiers — not keywords (stay TK_IDENT) but carry
    // classification tags for O(1) lookup in the transpiler.
    // Stored WITHOUT KW_MARKER so is_keyword() returns false.
    static struct
    {
        char *name;
        uint32_t tag;
    } id_tags[] = {
        {"exit", TT_NORETURN_FN},
        {"_Exit", TT_NORETURN_FN},
        {"_exit", TT_NORETURN_FN},
        {"abort", TT_NORETURN_FN},
        {"quick_exit", TT_NORETURN_FN},
        {"__builtin_trap", TT_NORETURN_FN},
        {"__builtin_unreachable", TT_NORETURN_FN},
        {"thrd_exit", TT_NORETURN_FN},
        {"setjmp", TT_SETJMP_FN},
        {"longjmp", TT_SETJMP_FN},
        {"_setjmp", TT_SETJMP_FN},
        {"_longjmp", TT_SETJMP_FN},
        {"sigsetjmp", TT_SETJMP_FN},
        {"siglongjmp", TT_SETJMP_FN},
        {"pthread_exit", TT_SETJMP_FN},
        {"__builtin_setjmp", TT_SETJMP_FN},
        {"__builtin_longjmp", TT_SETJMP_FN},
        {"__builtin_setjmp_receive", TT_SETJMP_FN},
        {"savectx", TT_SETJMP_FN},
        {"vfork", TT_VFORK_FN},
    };
    for (size_t i = 0; i < sizeof(id_tags) / sizeof(*id_tags); i++)
        hashmap_put(&ctx->keyword_map, id_tags[i].name, strlen(id_tags[i].name),
                    (void *)(uintptr_t)(id_tags[i].tag));

    // Populate perfect hash table for O(1) keyword lookup
    memset(keyword_perfect, 0, sizeof(keyword_perfect));
    for (size_t i = 0; i < sizeof(kw) / sizeof(*kw); i++)
    {
        int len = strlen(kw[i].name);
        unsigned slot = KEYWORD_HASH(kw[i].name, len);
        if (keyword_perfect[slot].name)
            error("keyword hash collision: '%s' and '%s'", keyword_perfect[slot].name, kw[i].name);
        keyword_perfect[slot].name = kw[i].name;
        keyword_perfect[slot].len = len;
        keyword_perfect[slot].value = kw[i].tag | KW_MARKER;
    }
    for (size_t i = 0; i < sizeof(id_tags) / sizeof(*id_tags); i++)
    {
        int len = strlen(id_tags[i].name);
        unsigned slot = KEYWORD_HASH(id_tags[i].name, len);
        if (keyword_perfect[slot].name)
            continue; // Collision with keyword — fall back to hashmap for this tag
        keyword_perfect[slot].name = id_tags[i].name;
        keyword_perfect[slot].len = len;
        keyword_perfect[slot].value = id_tags[i].tag;
    }
}

// After cc -E, UCNs are resolved. Just handle ASCII + pass through non-ASCII bytes.
static int read_ident(char *start)
{
    char *p = start;
    if ((unsigned char)*p >= 0x80)
        p++;
    else if (IS_ALPHA(*p))
        p++;
    else
        return 0;
    while (ident_char[(unsigned char)*p])
        p++;
    return p - start;
}

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
            return -2; // <: (digraph)
        if (p[1] == '%')
            return -2; // <% (digraph)
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
            return -4; // %:%: (digraph ##)
        if (p[1] == ':')
            return -2; // %: (digraph #)
        if (p[1] == '>')
            return -2; // %> (digraph })
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
        return (p[1] == '>') ? -2 : 1; // :> (digraph) or :
    default:
        return ((unsigned char)*p > 0x20 && *p != 0x7f && !IS_ALNUM(*p)) ? 1 : 0;
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
    {
        if (*p == '\n')
            ctx->tok_line_no++;
        if (p[0] == '*' && p[1] == '/')
            return p + 2;
    }
    error_at(p, "unclosed block comment");
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
        if (*q == '\n')
            ctx->tok_line_no++;
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

static inline __attribute__((always_inline)) Token *new_token(TokenKind kind, char *start, char *end)
{
    Token *tok = arena_alloc_token();
    tok->kind = kind;
    tok->loc = start;
    tok->len = end - start;
    tok->next = NULL;
    tok->tag = 0;
    tok->line_no = ctx->tok_line_no + ctx->current_file->line_delta;
    tok->file_idx = ctx->current_file->file_no;
    tok->flags = (ctx->at_bol ? TF_AT_BOL : 0) | (ctx->has_space ? TF_HAS_SPACE : 0);
    ctx->at_bol = ctx->has_space = false;
    return tok;
}

static Token *read_string_literal(char *start, char *quote)
{
    char *end = string_literal_end(quote + 1);
    return new_token(TK_STR, start, end + 1);
}

static Token *read_raw_string_literal(char *start, char *quote)
{
    char *end = raw_string_literal_end(quote);
    if (!end)
        error_at(start, "invalid raw string literal");
    return new_token(TK_STR, start, end);
}

static Token *read_char_literal(char *start, char *quote)
{
    char *p = quote + 1;
    if (*p == '\0')
        error_at(start, "unclosed char literal");
    for (; *p != '\''; p++)
    {
        if (*p == '\n' || *p == '\0')
            error_at(p, "unclosed char literal");
        if (*p == '\\')
            p++;
    }
    return new_token(TK_NUM, start, p + 1);
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

    if (len >= 4) // 4-char suffixes: BF16, F128
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
// Classify a number token: detect floats for C23 suffix rewriting
static void classify_number(Token *tok)
{
    char *p = tok->loc;
    bool is_hex = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'));
    for (char *q = p; q < p + tok->len; q++)
    {
        char c = *q;
        if (c == '.' || c == 'p' || c == 'P' || (!is_hex && (c == 'e' || c == 'E')))
        {
            tok->flags |= TF_IS_FLOAT;
            return;
        }
    }
    if (!is_hex && get_extended_float_suffix(p, tok->len, NULL))
        tok->flags |= TF_IS_FLOAT;
}

// Tag a punctuator token with TT_ASSIGN or TT_MEMBER
static inline void classify_punct(Token *t)
{
    char c = t->loc[0];
    if (t->len == 1)
    {
        if (c == '=' || c == '[')
            t->tag = TT_ASSIGN;
        else if (c == '.')
            t->tag = TT_MEMBER;
    }
    else if (t->len == 2)
    {
        char c2 = t->loc[1];
        if (c2 == '=' && c != '!' && c != '<' && c != '>' && c != '=')
            t->tag = TT_ASSIGN;
        else if (c == '+' && c2 == '+')
            t->tag = TT_ASSIGN;
        else if (c == '-' && c2 == '-')
            t->tag = TT_ASSIGN;
        else if (c == '-' && c2 == '>')
            t->tag = TT_MEMBER;
    }
    else if (t->len == 3 && t->loc[2] == '=' && (c == '<' || c == '>') && t->loc[1] == c)
        t->tag = TT_ASSIGN;
}

static File *new_file(char *name, int file_no, char *contents)
{
    File *file = calloc(1, sizeof(File));
    if (!file)
    {
        free(contents);
        error("out of memory");
    }
    file->name = strdup(name);
    if (!file->name)
    {
        free(contents);
        free(file);
        error("out of memory");
    }
    file->display_name = file->name;
    file->file_no = file_no;
    file->contents = contents;
    file->contents_len = strlen(contents);
    file->owns_contents = true;
    return file;
}

static void add_input_file(File *file)
{
    ENSURE_ARRAY_CAP(ctx->input_files, ctx->input_file_count + 1, ctx->input_file_capacity, 16, File *);
    ctx->input_files[ctx->input_file_count++] = file;
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
    file->file_no = ctx->input_file_count;
    file->contents = base->contents;
    file->contents_len = base->contents_len;
    file->line_delta = line_delta;
    file->is_system = is_system;
    file->is_include_entry = is_include_entry;

    // Cache this file view
    cache_file_view(interned_name, line_delta, is_system, is_include_entry, file);
    add_input_file(file);

    return file;
}

// Scan a preprocessor directive starting at '#'
// Returns new position after directive, or NULL if not a line marker (caller handles as TK_PREP_DIR)
// Updates *in_system_include, *line_no, and may switch ctx->current_file
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

    if (!IS_DIGIT(*p)) // Must have line number to be a line marker
        return NULL;

    unsigned long new_line = 0;
    while (IS_DIGIT(*p))
    {
        unsigned int digit = *p - '0';
        if (new_line > (ULONG_MAX - digit) / 10)
            return NULL; // overflow
        new_line = new_line * 10 + digit;
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
    while (IS_DIGIT(*p))
    {
        int flag = 0;
        while (IS_DIGIT(*p))
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

    if (is_entering && is_system)
        *in_system_include = true;
    else if (is_returning && !is_system)
        *in_system_include = false;

    if (new_line > (unsigned long)INT_MAX)
        return NULL; // line number too large for int

    int line_delta = (int)new_line - (directive_line + 1);
    File *view = new_file_view(filename ? filename : ctx->current_file->name,
                               base_file, line_delta, is_system, *in_system_include);
    ctx->current_file = view;
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
        if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') && (p[1] == '+' || p[1] == '-'))
        {
            p += 2;
        }
        else if (ident_char[(unsigned char)c] || c == '.')
        {
            p++; // Accept digits, letters (hex, suffixes, extensions), dot, underscore
        }
        else if (c == '\'' && ident_char[(unsigned char)p[1]])
        {
            p++; // C23 digit separator: accept ' followed by any identifier char
        }
        else
            break;
    }
    return p;
}

static Token *tokenize(File *file)
{
    File *base_file = file;
    ctx->current_file = file;
    char *p = file->contents;

    Token head = {};
    Token *cur = &head;
    ctx->at_bol = true;
    ctx->has_space = false;
    ctx->tok_line_no = 1;

    // Track if we're inside a system header include chain
    // This persists across nested includes until we return to user code
    bool in_system_include = false;

    while (*p)
    {
        // Preprocessor directives (#line markers from -E output, or #pragma etc.)
        if (ctx->at_bol && *p == '#')
        {
            char *directive_start = p;
            char *after = scan_line_directive(p, base_file, &ctx->tok_line_no, &in_system_include);
            if (after)
            {
                p = after;
                ctx->at_bol = true;
                ctx->has_space = false;
                continue;
            }

            while (*p && *p != '\n') // Not a line marker - preserve as preprocessor directive token
                p++;
            cur = cur->next = new_token(TK_PREP_DIR, directive_start, p);
            tok_set_at_bol(cur, true);
            if (*p == '\n')
            {
                p++;
                ctx->tok_line_no++;
                ctx->at_bol = true;
                ctx->has_space = false;
            }
            continue;
        }

        if (p[0] == '/' && p[1] == '/') // Line comment
        {
            p = skip_line_comment(p + 2);
            ctx->has_space = true;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') // Block comment
        {
            p = skip_block_comment(p + 2);
            ctx->has_space = true;
            continue;
        }
        if (*p == '\n') // Newline
        {
            p++;
            ctx->tok_line_no++;
            ctx->at_bol = true;
            ctx->has_space = false;
            continue;
        }
        if (is_space(*p)) // Whitespace
        {
            p++;
            ctx->has_space = true;
            continue;
        }
        if (IS_DIGIT(*p) || (*p == '.' && IS_DIGIT(p[1])))
        {
            char *start = p;
            p = scan_pp_number(p);
            Token *t = cur = cur->next = new_token(TK_NUM, start, p);
            classify_number(t);
            continue;
        }
        { // C++11/C23 raw string literals: R"...", LR"...", uR"...", UR"...", u8R"..."
            int raw_pfx = (p[0] == 'R')                                                  ? 0
                          : (p[0] == 'u' && p[1] == '8' && p[2] == 'R')                  ? 2
                          : ((p[0] == 'L' || p[0] == 'u' || p[0] == 'U') && p[1] == 'R') ? 1
                                                                                         : -1;
            if (raw_pfx >= 0 && p[raw_pfx] == 'R' && p[raw_pfx + 1] == '"')
            {
                cur = cur->next = read_raw_string_literal(p, p + raw_pfx + 1);
                p += cur->len;
                continue;
            }
        }
        if (*p == '"') // String literal
        {
            cur = cur->next = read_string_literal(p, p);
            p += cur->len;
            continue;
        }
        // UTF-8/wide string
        if ((p[0] == 'u' && p[1] == '8' && p[2] == '"') || ((p[0] == 'u' || p[0] == 'U' || p[0] == 'L') && p[1] == '"'))
        {
            char *start = p;
            p += (p[0] == 'u' && p[1] == '8') ? 2 : 1;
            cur = cur->next = read_string_literal(start, p);
            p = start + cur->len;
            continue;
        }
        if (*p == '\'') // Character literal
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
        // Identifier / keyword
        int ident_len = read_ident(p);
        if (ident_len)
        {
            Token *t = cur = cur->next = new_token(TK_IDENT, p, p + ident_len);
            uintptr_t kw = keyword_lookup(p, ident_len);
            if (!kw)
                kw = (uintptr_t)hashmap_get(&ctx->keyword_map, p, ident_len);
            if (kw)
            {
                if (kw & KW_MARKER)
                {
                    t->kind = TK_KEYWORD;
                    t->tag = (uint32_t)(kw & ~KW_MARKER);
                }
                else
                    t->tag = (uint32_t)kw;
            }
            p += ident_len;
            continue;
        }
        // Punctuator (read_punct returns negative for digraphs)
        int punct_len = read_punct(p);
        if (punct_len)
        {
            int abs_len = punct_len < 0 ? -punct_len : punct_len;
            Token *t = cur = cur->next = new_token(TK_PUNCT, p, p + abs_len);
            if (punct_len < 0)
                t->flags |= TF_IS_DIGRAPH;
            classify_punct(t);
            p += abs_len;
            continue;
        }
        error_at(p, "invalid token");
    }

    cur = cur->next = new_token(TK_EOF, p, p);
    return head.next;
}

// Tokenize from an already-loaded buffer (takes ownership of buf).
// The buffer must be NUL-terminated. Used by pipe-based preprocessor
// to avoid writing/reading temp files.
static Token *tokenize_buffer(char *name, char *buf)
{
    if (!buf)
        return NULL;

    // Init keyword map before new_file() takes ownership of buf.
    // If init_keyword_map() fails (OOM → longjmp in lib mode), buf hasn't
    // been stored yet, so the caller can still free it. By failing early
    // here, we avoid leaking buf.
    if (!ctx->keyword_map.capacity)
        init_keyword_map();

    File *file = new_file(name, ctx->input_file_count, buf);
    add_input_file(file);

    return tokenize(file);
}

Token *tokenize_file(char *path)
{
    // Initialize keyword map on first call
    if (!ctx->keyword_map.capacity)
        init_keyword_map();

    FILE *fp = fopen(path, "r");
    if (!fp)
        return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size < 0)
    {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf)
    {
        fclose(fp);
        return NULL;
    }
    size_t nread = fread(buf, 1, size, fp);
    buf[nread] = '\0';
    fclose(fp);

    File *file = new_file(path, ctx->input_file_count, buf);
    add_input_file(file);

    return tokenize(file);
}

// Teardown tokenizer state
// full=false: reset for reuse (keeps arena blocks allocated)
// full=true:  free all memory including arena blocks and keyword map
void tokenizer_teardown(bool full)
{
    if (full)
        arena_free(&ctx->main_arena);
    else
        arena_reset(&ctx->main_arena);
    free_file_view_cache();
    if (ctx->input_files)
    {
        for (int i = 0; i < ctx->input_file_count; i++)
            free_file(ctx->input_files[i]);
        free(ctx->input_files);
    }
    ctx->input_files = NULL;
    ctx->input_file_count = 0;
    ctx->input_file_capacity = 0;
    ctx->current_file = NULL;
    free_filename_intern_map();
    if (full)
        hashmap_clear(&ctx->keyword_map);
}

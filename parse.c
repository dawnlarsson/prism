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
};

struct Token
{
    char *loc;
    Token *next;
    int len;
    TokenKind kind;
    uint16_t file_idx;
    uint8_t flags;
    uint32_t tag; // TT_* bitmask - token classification
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

// Feature flags (compact bitmask for PrismContext.features)
enum
{
    F_DEFER = 1,
    F_ZEROINIT = 2,
    F_LINE_DIR = 4,
    F_WARN_SAFETY = 8,
    F_FLATTEN = 16
};

typedef struct PrismContext
{
    Arena main_arena;
    File *current_file;
    File **input_files;
    int input_file_count;
    int input_file_capacity;
    bool at_bol;
    bool has_space;
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
    uint32_t features; // F_DEFER | F_ZEROINIT | F_LINE_DIR | F_WARN_SAFETY | F_FLATTEN
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
    char active_temp_pp[PATH_MAX];
#endif
} PrismContext;

static PrismContext *ctx = NULL;

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
    ctx->features = F_DEFER | F_ZEROINIT | F_LINE_DIR | F_FLATTEN;
    ctx->at_stmt_start = true;
}

// Token arena - uses main_arena
static Token *arena_alloc_token(void)
{
    return arena_alloc(&ctx->main_arena, sizeof(Token));
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

// File view cache - avoids creating duplicate File structs for same file context
// Uses the generic HashMap with a packed composite key
// Key: combines filename pointer, line_delta, is_system, is_include_entry
typedef struct
{
    char *filename; // Interned filename (pointer comparison valid)
    int line_delta;
    uint8_t flags; // is_system (bit 0), is_include_entry (bit 1)
} FileViewKey;

static File *find_cached_file_view(char *filename, int line_delta, bool is_system, bool is_include_entry)
{
    FileViewKey key = {filename, line_delta, (is_system ? 1 : 0) | (is_include_entry ? 2 : 0)};
    return hashmap_get(&ctx->file_view_cache, (char *)&key, sizeof(key));
}

static void cache_file_view(char *filename, int line_delta, bool is_system, bool is_include_entry, File *file)
{
    FileViewKey key = {filename, line_delta, (is_system ? 1 : 0) | (is_include_entry ? 2 : 0)};
    // Need to allocate key storage since HashMap stores pointer to key
    FileViewKey *stored_key = malloc(sizeof(FileViewKey));
    if (!stored_key)
        error("out of memory");
    *stored_key = key;
    hashmap_put(&ctx->file_view_cache, (char *)stored_key, sizeof(FileViewKey), file);
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
        char *found = hashmap_get(&ctx->filename_intern_map, f->name, len);
        if (found != f->name) // Not interned, safe to free
            free(f->name);
    }
    free(f);
}

// Free all interned filenames and clear the map
static void free_filename_intern_map(void)
{
    if (!ctx->filename_intern_map.buckets)
        return;
    for (int i = 0; i < ctx->filename_intern_map.capacity; i++)
    {
        HashEntry *ent = &ctx->filename_intern_map.buckets[i];
        if (ent->key && ent->key != TOMBSTONE)
        {
            free(ent->key); // Free the interned string
        }
    }
    free(ctx->filename_intern_map.buckets);
    ctx->filename_intern_map.buckets = NULL;
    ctx->filename_intern_map.capacity = 0;
    ctx->filename_intern_map.used = 0;
}

// Free file view cache keys and clear the map (File structs are freed separately)
static void free_file_view_cache(void)
{
    if (!ctx->file_view_cache.buckets)
        return;
    // Free allocated keys
    for (int i = 0; i < ctx->file_view_cache.capacity; i++)
    {
        HashEntry *ent = &ctx->file_view_cache.buckets[i];
        if (ent->key && ent->key != TOMBSTONE)
            free(ent->key);
    }
    hashmap_clear(&ctx->file_view_cache);
}

static inline File *tok_file(Token *tok)
{
    if (!tok || tok->file_idx >= ctx->input_file_count)
        return ctx->current_file;
    return ctx->input_files[tok->file_idx];
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
// PRISM_LIB_MODE is defined but ctx->error_jmp_set is false.
static noreturn void error(char *fmt, ...)
{
#ifdef PRISM_LIB_MODE
    if (ctx->error_jmp_set)
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

// Internal marker bit for keyword map: values are (tag | KW_MARKER)
// This distinguishes tag=0 keywords from "not found" (NULL)
#define KW_MARKER 0x80000000UL

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
        {"volatile", TT_QUALIFIER},
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
        {"typeof_unqual", TT_TYPE},
        {"auto", TT_QUALIFIER},
        {"register", TT_QUALIFIER},
        {"_Alignas", TT_QUALIFIER},
        {"typeof", TT_TYPE},
        {"__typeof__", TT_TYPE},
        {"__typeof", TT_TYPE},
        {"_BitInt", TT_TYPE},
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
        {"vfork", TT_VFORK_FN},
    };
    for (size_t i = 0; i < sizeof(id_tags) / sizeof(*id_tags); i++)
        hashmap_put(&ctx->keyword_map, id_tags[i].name, strlen(id_tags[i].name),
                    (void *)(uintptr_t)(id_tags[i].tag));
}

// After cc -E, UCNs are resolved. Just handle ASCII + pass through non-ASCII bytes.
static int read_ident(char *start)
{
    char *p = start;
    if ((unsigned char)*p >= 0x80)
        p++;
    else if (isalpha(*p) || *p == '_' || *p == '$')
        p++;
    else
        return 0;
    while (isalnum(*p) || *p == '_' || *p == '$' || (unsigned char)*p >= 0x80)
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
    tok->file_idx = ctx->current_file ? ctx->current_file->file_no : 0;
    tok_set_at_bol(tok, ctx->at_bol);
    tok_set_has_space(tok, ctx->has_space);
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
// Classify a number token: detect floats for C23 suffix rewriting
static void classify_number(Token *tok)
{
    char *p = tok->loc;
    bool is_hex = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'));
    for (char *q = p; q < p + tok->len; q++)
    {
        if (*q == '.' || *q == 'p' || *q == 'P')
        {
            tok->flags |= TF_IS_FLOAT;
            return;
        }
        if (!is_hex && (*q == 'e' || *q == 'E'))
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
    ctx->current_file = file;
    char *p = file->contents;

    Token head = {};
    Token *cur = &head;
    ctx->at_bol = true;
    ctx->has_space = false;
    int line_no = 1;

    // Track if we're inside a system header include chain
    // This persists across nested includes until we return to user code
    static bool in_system_include = false;
    in_system_include = false; // Reset for each new file

    while (*p)
    {
        // Preprocessor directives (#line markers from -E output, or #pragma etc.)
        if (ctx->at_bol && *p == '#')
        {
            char *directive_start = p;
            char *after = scan_line_directive(p, base_file, &line_no, &in_system_include);
            if (after)
            {
                p = after;
                ctx->at_bol = true;
                ctx->has_space = false;
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
                ctx->at_bol = true;
                ctx->has_space = false;
            }
            continue;
        }

        // Line comment
        if (p[0] == '/' && p[1] == '/')
        {
            p = skip_line_comment(p + 2);
            ctx->has_space = true;
            continue;
        }
        // Block comment
        if (p[0] == '/' && p[1] == '*')
        {
            p = skip_block_comment(p + 2);
            ctx->has_space = true;
            continue;
        }
        // Newline
        if (*p == '\n')
        {
            p++;
            line_no++;
            ctx->at_bol = true;
            ctx->has_space = false;
            continue;
        }
        // Whitespace
        if (is_space(*p))
        {
            p++;
            ctx->has_space = true;
            continue;
        }
        // Number
        if (isdigit(*p) || (*p == '.' && isdigit(p[1])))
        {
            char *start = p;
            p = scan_pp_number(p);
            Token *t = cur = cur->next = new_token(TK_NUM, start, p);
            classify_number(t);
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
        // Identifier / keyword
        int ident_len = read_ident(p);
        if (ident_len)
        {
            Token *t = cur = cur->next = new_token(TK_IDENT, p, p + ident_len);
            void *kw = hashmap_get(&ctx->keyword_map, p, ident_len);
            if (kw)
            {
                uintptr_t v = (uintptr_t)kw;
                if (v & KW_MARKER)
                {
                    t->kind = TK_KEYWORD;
                    t->tag = (uint32_t)(v & ~KW_MARKER);
                }
                else
                    t->tag = (uint32_t)v;
            }
            p += ident_len;
            continue;
        }
        // Punctuator
        int punct_len = read_punct(p);
        if (punct_len)
        {
            Token *t = cur = cur->next = new_token(TK_PUNCT, p, p + punct_len);
            classify_punct(t);
            p += punct_len;
            continue;
        }
        error_at(p, "invalid token");
    }

    cur = cur->next = new_token(TK_EOF, p, p);
    return head.next;
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
    for (int i = 0; i < ctx->input_file_count; i++)
        free_file(ctx->input_files[i]);
    free(ctx->input_files);
    ctx->input_files = NULL;
    ctx->input_file_count = 0;
    ctx->input_file_capacity = 0;
    ctx->current_file = NULL;
    free_filename_intern_map();
    if (full)
        hashmap_clear(&ctx->keyword_map);
}

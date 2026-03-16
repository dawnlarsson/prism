// parse.c - C tokenizer. Expects preprocessed input (cc -E).
// Based on https://github.com/rui314/chibicc (MIT License)

// Thread-local storage qualifier: all per-invocation mutable state uses this
// for thread safety. Each thread gets its own copy.
#if defined(_MSC_VER)
#define PRISM_THREAD_LOCAL __declspec(thread)
#else
#define PRISM_THREAD_LOCAL _Thread_local
#endif

#ifdef _WIN32
#include "windows.c"
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
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

#define TOMBSTONE ((char *)1)
#define ENTRY_MATCHES(ent, k, kl)                                                                            \
	((ent)->key && (ent)->key != TOMBSTONE && (ent)->key_len == (kl) && !memcmp((ent)->key, (k), (kl)))
#define IS_DIGIT(c) ((unsigned)(c) - '0' < 10u)
#define IS_ALPHA(c) (((unsigned)((c) | 0x20) - 'a') < 26u || (c) == '_' || (c) == '$')
#define IS_ALNUM(c) (IS_DIGIT(c) || IS_ALPHA(c))
#define IS_XDIGIT(c) (IS_DIGIT(c) || ((unsigned)((c) | 0x20) - 'a') < 6u)
#define ARENA_DEFAULT_BLOCK_SIZE (64 * 1024)
#define KW_MARKER 0x80000000UL // Internal marker bit for keyword map: values are (tag | KW_MARKER)
#define KW_FLAGS_SHIFT 32       // Extra token flags encoded in bits 32-39 of keyword value

#if defined(_MSC_VER)
#define ARENA_ALIGN 8
#else
#define ARENA_ALIGN (__alignof__(long double))
#endif

#define equal(                                                                                                                     \
    tok,                                                                                                                           \
    s) /* known-length strings of 1/2 bytes use branchless comparisons; others use memcmp. Runtime strings fall back to strlen. */ \
	(__builtin_constant_p(s) ? (__builtin_strlen(s) == 1   ? _equal_1(tok, (s)[0])                                             \
				    : __builtin_strlen(s) == 2 ? _equal_2(tok, s)                                                  \
							       : equal_n(tok, s, __builtin_strlen(s)))                             \
				 : equal_n(tok, s, strlen(s)))

#define KEYWORD_HASH(key, len)                                                                               \
	((len) == 0 ? 0                                                                                      \
		    : (((unsigned)(len) * 2 + (unsigned char)(key)[0] * 99 +                                 \
			(unsigned char)((len) > 1 ? (key)[1] : (key)[0]) * 125 +                             \
			(unsigned char)((len) > 6 ? (key)[6] : (key)[(len) - 1]) * 69) &                     \
		       255))

#define ENSURE_ARRAY_CAP(arr, count, cap, init_cap, T)                                                       \
	do {                                                                                                 \
		if ((count) >= (cap)) {                                                                      \
			size_t new_cap =                                                                     \
			    (cap) == 0 ? ((init_cap) > 0 ? (size_t)(init_cap) : 1) : (size_t)(cap) * 2;      \
			while (new_cap < (size_t)(count)) new_cap *= 2;                                      \
			if (new_cap > SIZE_MAX / sizeof(T)) error("allocation overflow");                    \
			T *tmp = realloc((arr), sizeof(T) * new_cap);                                        \
			if (!tmp) {                                                                          \
				error("out of memory");                                                      \
			}                                                                                    \
			(arr) = tmp;                                                                         \
			(cap) = new_cap;                                                                     \
		}                                                                                            \
	} while (0)

#define ARENA_ENSURE_CAP(arena, arr, count, cap, init_cap, T)                                                \
	do {                                                                                                 \
		if ((count) >= (cap)) {                                                                      \
			size_t old_cap = (size_t)(cap);                                                      \
			size_t new_cap =                                                                     \
			    old_cap == 0 ? ((init_cap) > 0 ? (size_t)(init_cap) : 1) : old_cap * 2;          \
			while (new_cap < (size_t)(count)) new_cap *= 2;                                      \
			if (new_cap > SIZE_MAX / sizeof(T)) error("allocation overflow");                    \
			(arr) = arena_realloc((arena), (arr), sizeof(T) * old_cap, sizeof(T) * new_cap);     \
			(cap) = new_cap;                                                                     \
		}                                                                                            \
	} while (0)

// Identifier-continuation chars: alnum + _ + $ + bytes >= 0x80
static const uint8_t ident_char[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, // 0x00-0x0F
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, // 0x10-0x1F
    0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, // 0x20-0x2F ($)
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
    0, // 0x30-0x3F (0-9)
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, // 0x40-0x4F (A-O)
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0,
    1, // 0x50-0x5F (P-Z, _)
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, // 0x60-0x6F (a-o)
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0,
    0, // 0x70-0x7F (p-z)
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, // 0x80-0xFF (non-ASCII)
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

typedef struct Token Token;
typedef struct ArenaBlock ArenaBlock;

typedef struct {
	char *name;
	char *contents;
	size_t contents_len;
	size_t mmap_size;
	int file_no;
	int line_delta;
	bool owns_contents;
	bool is_system;
	bool is_include_entry; // True if inside a system #include chain (not just macro expansion)
} File;

typedef enum {
	TK_IDENT,
	TK_KEYWORD,
	TK_PUNCT,
	TK_STR,
	TK_NUM,
	TK_PREP_DIR, // Preprocessor directive (e.g., #pragma) to preserve
	TK_EOF,
} TokenKind;

enum {
	TF_AT_BOL = 1 << 0,
	TF_HAS_SPACE = 1 << 1,
	TF_IS_FLOAT = 1 << 2,
	TF_OPEN = 1 << 3,     // Opening delimiter: ( [ {
	TF_CLOSE = 1 << 4,    // Closing delimiter: ) ] }
	TF_C23_ATTR = 1 << 5, // First '[' of C23 [[ ... ]] attribute
	TF_RAW = 1 << 6,      // 'raw' keyword
	TF_SIZEOF = 1 << 7,   // sizeof, alignof, _Alignof
};

// Token tags — bitmask set at tokenize time
enum {
	TT_TYPE = 1 << 0, // Type keyword (int, char, void, struct, etc.)
	TT_QUALIFIER =
	    1 << 1,
	TT_SUE = 1 << 2,	  // struct/union/enum
	TT_SKIP_DECL = 1 << 3,
	TT_ATTR = 1 << 4,	  // Attribute keyword (__attribute__, __attribute, __declspec)
	TT_ASSIGN = 1 << 5,	  // Assignment or compound assignment operator (=, +=, ++, --, [)
	TT_MEMBER = 1 << 6,	  // Member access operator (. or ->)
	TT_LOOP = 1 << 7,	  // Loop keyword (for, while, do)
	TT_STORAGE = 1 << 8,	  // Storage class: extern, static, _Thread_local, thread_local
	TT_ASM = 1 << 9,	  // Inline assembly (asm, __asm__, __asm)
	TT_INLINE = 1 << 10,	  // inline, __inline, __inline__
	TT_NORETURN_FN = 1 << 11, // Noreturn function identifier (exit, abort, etc.)
	TT_SPECIAL_FN = 1 << 12,
	TT_CONST = 1 << 13,	  // const keyword

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

	TT_VOLATILE = 1 << 25,	  // volatile
	TT_REGISTER = 1 << 26,	  // register
	TT_TYPEOF = 1 << 27,	  // typeof, typeof_unqual, __typeof__, __typeof
	TT_BITINT = 1 << 28,	  // _BitInt
	TT_ALIGNAS = 1 << 29,	  // _Alignas, alignas
	TT_ORELSE = 1 << 30,	  // orelse
	TT_STRUCTURAL = 1u << 31, // Structural punctuation: { } ; : — forces slow-path dispatch
};

// Tags that can start a declaration
#define TT_DECL_START                                                                                        \
	(TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_INLINE | TT_ALIGNAS | TT_SKIP_DECL | TT_ATTR)

struct Token {
	uint32_t tag;         // TT_* bitmask - token classification
	uint32_t next_idx;    // Token pool index (0 = NULL)
	uint32_t match_idx;   // Token pool index (0 = NULL)
	uint32_t len;         // Token length in bytes (must handle >65535 for large literals)
	uint8_t  kind;
	uint8_t  flags;
}; // 20 bytes — hot path only

typedef struct {
	uint32_t loc_offset;  // Byte offset from File->contents
	int32_t  line_no : 21;
	uint32_t file_idx : 11;
} TokenCold; // 8 bytes — error/debug path

typedef struct {
	char *key;
	void *val;
	uint16_t key_len;
} HashEntry;

typedef struct {
	HashEntry *buckets;
	int capacity;
	int used;
} HashMap;

// O(1) keyword lookup by hash slot
typedef struct {
	char *name;
	uintptr_t value;
	uint8_t len;
} KeywordEntry;

enum // Feature flags
{
	F_DEFER = 1,
	F_ZEROINIT = 2,
	F_LINE_DIR = 4,
	F_WARN_SAFETY = 8,
	F_FLATTEN = 16,
	F_ORELSE = 32
};

struct ArenaBlock;
typedef struct ArenaBlock ArenaBlock;

struct ArenaBlock {
	ArenaBlock *next;
	size_t used;
	size_t capacity;
#if defined(_MSC_VER)
	__declspec(align(ARENA_ALIGN)) char data[];
#else
	_Alignas(ARENA_ALIGN) char data[];
#endif
};

typedef struct {
	ArenaBlock *head;
	ArenaBlock *current;
	size_t default_block_size;
} Arena;

typedef struct {
	bool at_bol;
	bool has_space;
	int line_no;
} TokState;

typedef struct PrismContext {
	Arena main_arena;
	File *current_file;
	File **input_files;
	int input_file_count;
	int input_file_capacity;

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
	const char **dep_flags;    // -Wp,-MMD, -MD, -MF etc. (preprocessor-only)
	int dep_flags_count;
	int scope_depth;
	int block_depth;
	bool current_func_returns_void;
	bool current_func_has_setjmp;
	bool current_func_has_asm;
	bool current_func_has_vfork;

	bool last_system_header;
	int last_line_no;
	char *last_filename;
	bool at_stmt_start;
	int system_include_count;
	unsigned long long ret_counter;
	Token *func_ret_type_start;	   // First token of return type (after storage/function specifiers)
	Token *func_ret_type_end;	   // Function name token (exclusive end of return type range)
	Token *func_ret_type_suffix_start; // For complex declarators: closing ')' after func params
	Token *func_ret_type_suffix_end;   // For complex declarators: token after suffix (exclusive)
	unsigned *bracket_oe_ids;              // Pre-assigned temp IDs for bracket orelse hoisting (dynamic)
	int bracket_oe_count;              // Count of hoisted bracket orelse temps
	int bracket_oe_cap;                // Capacity of bracket_oe_ids array
	int bracket_oe_next;               // Next temp to consume during emit

	// Token pools: parallel hot/cold arrays for cache-optimal access
	Token *tp_pool;        // Hot: tag, next_idx, match_idx, len, kind, flags
	TokenCold *tp_cold;    // Cold: loc_offset, line_no, file_idx
	uint32_t tp_count;     // Next free index. 0 reserved as NULL sentinel.
	uint32_t tp_cap;

	// Keyword cache: O(1) lookup by hash slot
	KeywordEntry kw_cache[256];
	uint32_t keyword_cache_features; // features used when keyword_cache was built

	// Digraph normalization targets (per-context for token loc comparison)
	char dg_bracket_open[2];
	char dg_bracket_close[2];
	char dg_brace_open[2];
	char dg_brace_close[2];
	char dg_hash[2];
	char dg_paste[3];

#ifdef PRISM_LIB_MODE
	char *active_membuf;	       // open_memstream buffer; freed on longjmp recovery
#endif
} PrismContext;

static PRISM_THREAD_LOCAL PrismContext *ctx = NULL;

// True if loc points to a digraph normalization buffer (not the source buffer)
static inline bool is_digraph_loc(char *loc) {
	return loc == ctx->dg_bracket_open || loc == ctx->dg_bracket_close ||
	       loc == ctx->dg_brace_open || loc == ctx->dg_brace_close ||
	       loc == ctx->dg_hash || loc == ctx->dg_paste;
}

// Convenience accessors for per-context state (go through thread-local ctx)
#define token_pool  (ctx->tp_pool)
#define token_cold  (ctx->tp_cold)
#define token_count (ctx->tp_count)
#define token_cap   (ctx->tp_cap)
#define keyword_cache (ctx->kw_cache)
#define digraph_norm_bracket_open  (ctx->dg_bracket_open)
#define digraph_norm_bracket_close (ctx->dg_bracket_close)
#define digraph_norm_brace_open    (ctx->dg_brace_open)
#define digraph_norm_brace_close   (ctx->dg_brace_close)
#define digraph_norm_hash          (ctx->dg_hash)
#define digraph_norm_paste         (ctx->dg_paste)

static noreturn void error(char *fmt, ...);
static void hashmap_put(HashMap *map, char *key, int keylen, void *val);

static inline bool tok_at_bol(Token *tok) {
	return tok->flags & TF_AT_BOL;
}

static inline bool tok_has_space(Token *tok) {
	return tok->flags & TF_HAS_SPACE;
}

static inline void tok_set_at_bol(Token *tok, bool v) {
	tok->flags = v ? (tok->flags | TF_AT_BOL) : (tok->flags & ~TF_AT_BOL);
}

static ArenaBlock *arena_new_block(size_t min_size, size_t default_size) {
	size_t capacity = default_size;
	if (min_size > capacity) capacity = min_size;
	ArenaBlock *block = malloc(sizeof(ArenaBlock) + capacity);
	if (!block) error("out of memory allocating arena block");
	block->next = NULL;
	block->used = 0;
	block->capacity = capacity;
	return block;
}

static void arena_ensure(Arena *arena, size_t size) {
	if (arena->current && arena->current->used + size <= arena->current->capacity) return;
	if (arena->current && arena->current->next && size <= arena->current->next->capacity) {
		arena->current = arena->current->next;
		arena->current->used = 0;
		return;
	}
	size_t block_size = arena->default_block_size ? arena->default_block_size : ARENA_DEFAULT_BLOCK_SIZE;
	ArenaBlock *block = arena_new_block(size, block_size);
	if (arena->current) {
		block->next = arena->current->next;
		arena->current->next = block;
	} else
		arena->head = block;
	arena->current = block;
}

static void *arena_alloc_uninit(Arena *arena, size_t size) {
	if (size == 0) size = 1;
	size = (size + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
	arena_ensure(arena, size);
	void *ptr = arena->current->data + arena->current->used;
	arena->current->used += size;
	return ptr;
}

static void *arena_alloc(Arena *arena, size_t size) {
	void *ptr = arena_alloc_uninit(arena, size);
	memset(ptr, 0, size);
	return ptr;
}

static void *arena_realloc(Arena *arena, void *old, size_t old_size, size_t new_size) {
	void *p = arena_alloc_uninit(arena, new_size);
	if (old && old_size > 0) memcpy(p, old, old_size < new_size ? old_size : new_size);
	if (new_size > old_size) memset((char *)p + old_size, 0, new_size - old_size);
	return p;
}

static char *arena_strdup(Arena *arena, const char *s) {
	size_t len = strlen(s);
	char *p = arena_alloc_uninit(arena, len + 1);
	memcpy(p, s, len + 1);
	return p;
}

static void arena_reset(Arena *arena) {
	for (ArenaBlock *b = arena->head; b; b = b->next) b->used = 0;
	arena->current = arena->head;
}

static void arena_free(Arena *arena) {
	ArenaBlock *b = arena->head;
	while (b) {
		ArenaBlock *next = b->next;
		free(b);
		b = next;
	}
	arena->head = NULL;
	arena->current = NULL;
}

static void prism_ctx_init(void) {
	if (ctx) return;
	PrismContext *c = calloc(1, sizeof(PrismContext));
	if (!c) {
		fprintf(stderr, "prism: out of memory\n");
		exit(1);
	}
	c->main_arena.default_block_size = ARENA_DEFAULT_BLOCK_SIZE;
	c->features = F_DEFER | F_ZEROINIT | F_LINE_DIR | F_FLATTEN | F_ORELSE;
	c->at_stmt_start = true;
	c->tp_count = 1; // 0 reserved as NULL sentinel

	// Initialize digraph normalization buffers
	memcpy(c->dg_bracket_open, "[", 2);
	memcpy(c->dg_bracket_close, "]", 2);
	memcpy(c->dg_brace_open, "{", 2);
	memcpy(c->dg_brace_close, "}", 2);
	memcpy(c->dg_hash, "#", 2);
	memcpy(c->dg_paste, "##", 3);

	ctx = c;
}

static void token_pool_ensure(size_t need) {
	if (need <= token_cap) return;
	size_t new_cap = token_cap ? token_cap * 2 : 65536;
	while (new_cap < need) new_cap *= 2;
	if (new_cap > (uint32_t)-1 || new_cap > SIZE_MAX / sizeof(Token))
		error("token pool capacity exceeded");
	Token *p = realloc(token_pool, new_cap * sizeof(Token));
	if (!p) error("out of memory allocating token pool");
	token_pool = p;
	TokenCold *c = realloc(token_cold, new_cap * sizeof(TokenCold));
	if (!c) error("out of memory allocating token cold pool");
	token_cold = c;
	token_cap = (uint32_t)new_cap;
}

static inline Token *pool_alloc_token(void) {
	if (token_count == UINT32_MAX)
		error("maximum token limit reached");
	if (__builtin_expect(token_count < token_cap, 1))
		return &token_pool[token_count++];
	token_pool_ensure(token_count + 1);
	return &token_pool[token_count++];
}

// Accessor: get cold data for a token
static inline TokenCold *tok_cold(Token *tok) {
	return &token_cold[tok - token_pool];
}

// Accessor: resolve token -> source location pointer
static inline char *tok_loc(Token *tok) {
	TokenCold *c = tok_cold(tok);
	return ctx->input_files[c->file_idx]->contents + c->loc_offset;
}

// Accessor: resolve next_idx -> Token*
static inline Token *tok_next(Token *tok) {
	return tok->next_idx ? &token_pool[tok->next_idx] : NULL;
}

// Accessor: resolve match_idx -> Token*
static inline Token *tok_match(Token *tok) {
	return tok->match_idx ? &token_pool[tok->match_idx] : NULL;
}

// Convert Token* to pool index (0 for NULL)
static inline uint32_t tok_idx(Token *tok) {
	return tok ? (uint32_t)(tok - token_pool) : 0;
}

// Fast multiply-mix hash (~2-4x faster than FNV-1a for short strings)
static inline uint64_t fast_hash(char *s, int len) {
	uint64_t a = 0, b = 0;
	if (len >= 8) {
		memcpy(&a, s, 8);
		memcpy(&b, s + len - 8, 8);
	} else if (len >= 4) {
		uint32_t lo, hi;
		memcpy(&lo, s, 4);
		memcpy(&hi, s + len - 4, 4);
		a = lo | ((uint64_t)hi << 32);
	} else if (len > 0) {
		// 1-3 bytes: pack into a
		a = ((uint64_t)(unsigned char)s[0] << 16) | ((uint64_t)(unsigned char)s[len >> 1] << 8) |
		    (unsigned char)s[len - 1];
	}
	a ^= (uint64_t)len * 0x9e3779b97f4a7c15ULL;
	a ^= b;
	a *= 0xbf58476d1ce4e5b9ULL;
	a ^= a >> 31;
	a *= 0x94d049bb133111ebULL;
	a ^= a >> 31;
	return a;
}

static void *hashmap_get(HashMap *map, char *key, int keylen) {
	if (!map->buckets) return NULL;
	uint64_t hash = fast_hash(key, keylen);
	int mask = map->capacity - 1;
	for (int i = 0; i <= mask; i++) {
		HashEntry *ent = &map->buckets[(hash + i) & mask];
		if (ENTRY_MATCHES(ent, key, keylen)) return ent->val;
		if (!ent->key) return NULL;
	}
	return NULL;
}

static void hashmap_resize(HashMap *map, int newcap) {
	HashMap new_map = {.buckets = calloc(newcap, sizeof(HashEntry)), .capacity = newcap};
	if (!new_map.buckets) error("out of memory resizing hashmap");
	for (int i = 0; i < map->capacity; i++) {
		HashEntry *ent = &map->buckets[i];
		if (ent->key && ent->key != TOMBSTONE)
			hashmap_put(&new_map, ent->key, ent->key_len, ent->val);
	}
	free(map->buckets);
	*map = new_map;
}

static void hashmap_put(HashMap *map, char *key, int keylen, void *val) {
	if (!map->buckets) {
		map->buckets = calloc(64, sizeof(HashEntry));
		if (!map->buckets) error("out of memory allocating hashmap");
		map->capacity = 64;
	} else if ((unsigned long)map->used * 100 / (unsigned long)map->capacity >= 70) {
		hashmap_resize(map, map->capacity * 2);
	}

	uint64_t hash = fast_hash(key, keylen);
	int mask = map->capacity - 1;
	int first_empty = -1;

	for (int i = 0; i <= mask; i++) {
		int idx = (hash + i) & mask;
		HashEntry *ent = &map->buckets[idx];

		if (ENTRY_MATCHES(ent, key, keylen)) {
			ent->val = val;
			return;
		}

		if (first_empty < 0 && !ent->key) first_empty = idx;
		if (!ent->key) break;
	}

	if (first_empty < 0) error("hashmap_put: no empty slot found (internal error)");

	HashEntry *ent = &map->buckets[first_empty];
	ent->key = key;
	ent->key_len = keylen;
	ent->val = val;
	map->used++;
}

// Reset entries without freeing the bucket array
static void hashmap_zero(HashMap *map) {
	if (map->buckets) memset(map->buckets, 0, (size_t)map->capacity * sizeof(HashEntry));
	map->used = 0;
}

static char *intern_filename(const char *name) {
	return name ? arena_strdup(&ctx->main_arena, name) : NULL;
}

static void free_file_contents(File *f) {
	if (!f || !f->contents) return;
#ifdef _WIN32
	if (f->mmap_size) { UnmapViewOfFile(f->contents); f->contents = NULL; return; }
#else
	if (f->mmap_size) { munmap(f->contents, f->mmap_size); f->contents = NULL; return; }
#endif
	if (f->owns_contents) free(f->contents);
}

static inline File *tok_file(Token *tok) {
	if (!tok) return ctx->current_file;
	TokenCold *c = tok_cold(tok);
	if (c->file_idx >= (uint32_t)ctx->input_file_count) return ctx->current_file;
	return ctx->input_files[c->file_idx];
}

static int tok_line_no(Token *tok) {
	return tok_cold(tok)->line_no;
}

#ifdef PRISM_LIB_MODE
static noreturn void lib_error_jump(int line) {
	ctx->error_line = line;
	longjmp(ctx->error_jmp, 1);
}

static inline bool lib_error_enabled(void) {
	return ctx && ctx->error_jmp_set;
}

static noreturn void lib_errorf(int line, const char *fmt, va_list ap) {
	vsnprintf(ctx->error_msg, sizeof(ctx->error_msg), fmt, ap);
	lib_error_jump(line);
}
#endif

static noreturn void error(char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
#ifdef PRISM_LIB_MODE
	if (lib_error_enabled()) lib_errorf(0, fmt, ap);
#endif
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static void verror_at(char *filename, char *input, int line_no, char *loc, const char *fmt, va_list ap) {
	// Digraph locs point to static storage; avoid UB from cross-object pointer comparison
	if (!input || !loc || line_no <= 0 || is_digraph_loc(loc)) {
		fprintf(stderr, "%s:%d: ", filename ? filename : "<unknown>", line_no > 0 ? line_no : 0);
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
		return;
	}

	char *line = loc;
	while (input < line && line[-1] != '\n') line--;

	char *end = loc;
	while (*end && *end != '\n') end++;

	int indent = fprintf(stderr, "%s:%d: ", filename, line_no);
	if (indent < 0) indent = 0;
	fprintf(stderr, "%.*s\n%*s^ ", (int)(end - line), line, indent + (int)(loc - line), "");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
}

static int count_lines(char *base, char *loc) {
	int n = 1;
	for (char *p = base; p < loc; p++)
		if (*p == '\n') n++;
	return n;
}

noreturn void error_at(char *loc, char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
#ifdef PRISM_LIB_MODE
	int line = ctx->current_file && ctx->current_file->contents && !is_digraph_loc(loc)
		       ? count_lines(ctx->current_file->contents, loc)
		       : 0;
	if (lib_error_enabled()) lib_errorf(line, fmt, ap);
#endif
	if (ctx->current_file)
		verror_at(ctx->current_file->name,
			  ctx->current_file->contents,
			  is_digraph_loc(loc) ? 0 : count_lines(ctx->current_file->contents, loc),
			  loc,
			  fmt,
			  ap);
	else {
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
	}
	va_end(ap);
	exit(1);
}

noreturn void error_tok(Token *tok, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	File *f = tok_file(tok);
#ifdef PRISM_LIB_MODE
	if (lib_error_enabled()) lib_errorf(tok_line_no(tok), fmt, ap);
#endif
	verror_at(f->name, f->contents, tok_line_no(tok), tok_loc(tok), fmt, ap);
	va_end(ap);
	exit(1);
}

static void warn_tok(Token *tok, const char *fmt, ...) {
#ifdef PRISM_LIB_MODE
	(void)tok;
	(void)fmt;
	return; // Suppress warnings in library mode
#else
	va_list ap;
	va_start(ap, fmt);
	File *f = tok_file(tok);
	verror_at(f->name, f->contents, tok_line_no(tok), tok_loc(tok), fmt, ap);
	va_end(ap);
#endif
}

static inline bool equal_n(Token *tok, const char *op, size_t len) {
	char *loc = tok_loc(tok);
	return tok->len == (int)len && (uint8_t)loc[0] == (uint8_t)op[0] &&
	       !memcmp(loc + 1, op + 1, len - 1);
}

static inline bool _equal_1(Token *tok, char c) {
	return tok->len == 1 && (uint8_t)tok_loc(tok)[0] == (uint8_t)c;
}

static inline bool _equal_2(Token *tok, const char *s) {
	char *loc = tok_loc(tok);
	return tok->len == 2 && loc[0] == s[0] && loc[1] == s[1];
}

static inline uintptr_t keyword_lookup(char *key, int keylen) {
	if (keylen < 2) return 0;
	unsigned slot = KEYWORD_HASH(key, keylen);
	for (int i = 0; i < 8; i++) {
		KeywordEntry *ent = &keyword_cache[(slot + i) & 255];
		if (!ent->name) return 0;
		if (ent->len == keylen && !memcmp(ent->name, key, keylen)) return ent->value;
	}
	return 0;
}

static inline bool is_potential_func_name(Token *tok) {
	Token *next = tok_next(tok);
	return tok->kind <= TK_KEYWORD && next && tok_loc(next)[0] == '(' &&
	       (next->flags & TF_OPEN) &&
	       !(tok->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_ATTR));
}

static void init_keyword_map(void) {
#if defined(_MSC_VER)
#pragma warning(push)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
	static struct {
		char *name;
		uint32_t tag;
		bool is_kw;
		uint8_t extra_flags;
	} entries[] = {
		{"return", TT_SKIP_DECL | TT_RETURN, true},
	    {"if", TT_SKIP_DECL | TT_IF, true},
	    {"else", TT_SKIP_DECL | TT_IF, true},
	    {"for", TT_SKIP_DECL | TT_LOOP, true},
	    {"while", TT_SKIP_DECL | TT_LOOP, true},
	    {"do", TT_SKIP_DECL | TT_LOOP, true},
	    {"switch", TT_SKIP_DECL | TT_SWITCH, true},
	    {"case", TT_SKIP_DECL | TT_CASE, true},
	    {"default", TT_SKIP_DECL | TT_DEFAULT, true},
	    {"break", TT_SKIP_DECL | TT_BREAK, true},
	    {"continue", TT_SKIP_DECL | TT_CONTINUE, true},
	    {"goto", TT_SKIP_DECL | TT_GOTO, true},
	    {"sizeof", TT_SKIP_DECL, true, TF_SIZEOF},
	    {"alignof", TT_SKIP_DECL, true, TF_SIZEOF},
	    {"_Alignof", TT_SKIP_DECL, true, TF_SIZEOF},
	    {"_Generic", TT_SKIP_DECL | TT_GENERIC, true},
	    {"_Static_assert", 0, true},
	    {"struct", TT_TYPE | TT_SUE, true},
	    {"union", TT_TYPE | TT_SUE, true},
	    {"enum", TT_TYPE | TT_SUE, true},
	    {"typedef", TT_SKIP_DECL | TT_TYPEDEF, true},
	    {"static", TT_QUALIFIER | TT_SKIP_DECL | TT_STORAGE, true},
	    {"extern", TT_SKIP_DECL | TT_STORAGE, true},
	    {"inline", TT_INLINE, true},
	    {"const", TT_QUALIFIER | TT_CONST, true},
	    {"volatile", TT_QUALIFIER | TT_VOLATILE, true},
	    {"restrict", TT_QUALIFIER, true},
	    {"_Atomic", TT_QUALIFIER | TT_TYPE, true},
	    {"_Noreturn", TT_SKIP_DECL, true},
	    {"__inline", TT_INLINE, true},
	    {"__inline__", TT_INLINE, true},
	    {"_Thread_local", TT_STORAGE, true},
	    {"constexpr", TT_QUALIFIER | TT_SKIP_DECL, true},
	    {"thread_local", TT_QUALIFIER | TT_SKIP_DECL | TT_STORAGE, true},
	    {"void", TT_TYPE, true},
	    {"char", TT_TYPE, true},
	    {"short", TT_TYPE, true},
	    {"int", TT_TYPE, true},
	    {"long", TT_TYPE, true},
	    {"float", TT_TYPE, true},
	    {"double", TT_TYPE, true},
	    {"signed", TT_TYPE, true},
	    {"unsigned", TT_TYPE, true},
	    {"_Bool", TT_TYPE, true},
	    {"bool", TT_TYPE, true},
	    {"_Complex", TT_TYPE, true},
	    {"_Imaginary", TT_TYPE, true},
	    {"__int128", TT_TYPE, true},
	    {"__int128_t", TT_TYPE, true},
	    {"__uint128", TT_TYPE, true},
	    {"__uint128_t", TT_TYPE, true},
	    {"__int8", TT_TYPE, true},
	    {"__int16", TT_TYPE, true},
	    {"__int32", TT_TYPE, true},
	    {"__int64", TT_TYPE, true},
	    {"typeof_unqual", TT_TYPE | TT_TYPEOF, true},
	    {"auto", TT_QUALIFIER | TT_TYPE, true},
	    {"register", TT_QUALIFIER | TT_REGISTER, true},
	    {"_Alignas", TT_QUALIFIER | TT_ALIGNAS, true},
	    {"alignas", TT_QUALIFIER | TT_ALIGNAS, true},
	    {"typeof", TT_TYPE | TT_TYPEOF, true},
	    {"__typeof__", TT_TYPE | TT_TYPEOF, true},
	    {"__typeof", TT_TYPE | TT_TYPEOF, true},
	    {"_BitInt", TT_TYPE | TT_BITINT, true},
	    {"asm", TT_SKIP_DECL | TT_ASM, true},
	    {"__asm__", TT_SKIP_DECL | TT_ASM, true},
	    {"__asm", TT_SKIP_DECL | TT_ASM, true},
	    {"__attribute__", TT_ATTR | TT_QUALIFIER, true},
	    {"__attribute", TT_ATTR | TT_QUALIFIER, true},
	    {"__declspec", TT_ATTR | TT_QUALIFIER, true},
	    {"_Pragma", TT_ATTR, true},
	    {"__pragma", TT_ATTR, true},
	    {"__extension__", 0, true},
	    {"__builtin_va_list", 0, true},
	    {"__builtin_va_arg", 0, true},
	    {"__builtin_offsetof", 0, true, TF_SIZEOF},
	    {"offsetof", 0, true, TF_SIZEOF},
	    {"__restrict", TT_QUALIFIER, true},
	    {"__restrict__", TT_QUALIFIER, true},
	    {"__builtin_types_compatible_p", 0, true},
	    {"defer", TT_DEFER, true},
	    {"orelse", TT_ORELSE, true},
	    {"raw", 0, true, TF_RAW},
	    {"exit", TT_NORETURN_FN, false},
	    {"_Exit", TT_NORETURN_FN, false},
	    {"_exit", TT_NORETURN_FN, false},
	    {"abort", TT_NORETURN_FN, false},
	    {"quick_exit", TT_NORETURN_FN, false},
	    {"__builtin_trap", TT_NORETURN_FN, false},
	    {"__builtin_unreachable", TT_NORETURN_FN, false},
	    {"thrd_exit", TT_NORETURN_FN, false},
	    {"setjmp", TT_SPECIAL_FN, false},
	    {"longjmp", TT_SPECIAL_FN, false},
	    {"_setjmp", TT_SPECIAL_FN, false},
	    {"_longjmp", TT_SPECIAL_FN, false},
	    {"sigsetjmp", TT_SPECIAL_FN, false},
	    {"siglongjmp", TT_SPECIAL_FN, false},
	    {"pthread_exit", TT_SPECIAL_FN, false},
	    {"__builtin_setjmp", TT_SPECIAL_FN, false},
	    {"__builtin_longjmp", TT_SPECIAL_FN, false},
	    {"__builtin_setjmp_receive", TT_SPECIAL_FN, false},
	    {"savectx", TT_SPECIAL_FN, false},
	    {"vfork", TT_SPECIAL_FN, false},
	};
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

	memset(keyword_cache, 0, sizeof(keyword_cache));
	for (size_t i = 0; i < sizeof(entries) / sizeof(*entries); i++) {
		int len = strlen(entries[i].name);
		uintptr_t val = entries[i].is_kw ? (entries[i].tag | KW_MARKER) : entries[i].tag;
		val |= (uintptr_t)entries[i].extra_flags << KW_FLAGS_SHIFT;
		unsigned slot = KEYWORD_HASH(entries[i].name, len);
		while (keyword_cache[slot & 255].name) slot++;
		keyword_cache[slot & 255] = (KeywordEntry){.name = entries[i].name, .value = val, .len = len};
	}
	ctx->keyword_cache_features = ctx->features;
}

static int read_ident(char *start) {
	char *p = start;
	if ((unsigned char)*p >= 0x80) p++;
	else if (IS_ALPHA(*p))
		p++;
	else
		return 0;
	while (ident_char[(unsigned char)*p]) p++;
	return p - start;
}

static int read_punct(char *p) {
	switch (*p) {
	case '<':
		if (p[1] == '<' && p[2] == '=') return 3; // <<=
		if (p[1] == '<') return 2;		  // <<
		if (p[1] == '=') return 2;		  // <=
		if (p[1] == ':') return -2;		  // <: (digraph)
		if (p[1] == '%') return -2;		  // <% (digraph)
		return 1;
	case '>':
		if (p[1] == '>' && p[2] == '=') return 3; // >>=
		if (p[1] == '>') return 2;		  // >>
		if (p[1] == '=') return 2;		  // >=
		return 1;
	case '.':
		if (p[1] == '.' && p[2] == '.') return 3; // ...
		return 1;
	case '=': return (p[1] == '=') ? 2 : 1; // == or =
	case '!': return (p[1] == '=') ? 2 : 1; // != or !
	case '-':
		if (p[1] == '>') return 2; // ->
		if (p[1] == '=') return 2; // -=
		if (p[1] == '-') return 2; // --
		return 1;
	case '+':
		if (p[1] == '=') return 2; // +=
		if (p[1] == '+') return 2; // ++
		return 1;
	case '*': return (p[1] == '=') ? 2 : 1; // *= or *
	case '/': return (p[1] == '=') ? 2 : 1; // /= or /
	case '%':
		if (p[1] == ':' && p[2] == '%' && p[3] == ':') return -4; // %:%: (digraph ##)
		if (p[1] == ':') return -2;				  // %: (digraph #)
		if (p[1] == '>') return -2;				  // %> (digraph })
		if (p[1] == '=') return 2;				  // %=
		return 1;
	case '&':
		if (p[1] == '&') return 2; // &&
		if (p[1] == '=') return 2; // &=
		return 1;
	case '|':
		if (p[1] == '|') return 2; // ||
		if (p[1] == '=') return 2; // |=
		return 1;
	case '^': return (p[1] == '=') ? 2 : 1;	 // ^= or ^
	case '#': return (p[1] == '#') ? 2 : 1;	 // ## or #
	case ':': return (p[1] == '>') ? -2 : 1; // :> (digraph) or :
	default: return ((unsigned char)*p > 0x20 && *p != 0x7f && !IS_ALNUM(*p)) ? 1 : 0;
	}
}

static bool is_space(char c) {
	return c == ' ' || c == '\t' || c == '\f' || c == '\r' || c == '\v';
}

// SWAR: test if any byte in a 64-bit word is zero
#define SWAR_HAS_ZERO(v) (((v) - 0x0101010101010101ULL) & ~(v) & 0x8080808080808080ULL)
// SWAR: test if any byte in a 64-bit word matches 'c' (broadcast c to all bytes first)
#define SWAR_BROADCAST(c) (0x0101010101010101ULL * (uint8_t)(c))

static char *skip_line_comment(char *p) {
	// Byte-at-a-time until aligned
	while ((uintptr_t)p & 7) {
		if (*p == '\0' || *p == '\n') return p;
		p++;
	}
	// Process 8 bytes at a time
	uint64_t nl_mask = SWAR_BROADCAST('\n');
	for (;;) {
		uint64_t v;
		memcpy(&v, p, 8);
		if (SWAR_HAS_ZERO(v) || SWAR_HAS_ZERO(v ^ nl_mask)) {
			// Found a NUL or newline in this chunk — find exact position
			for (int i = 0; i < 8; i++)
				if (p[i] == '\0' || p[i] == '\n') return p + i;
		}
		p += 8;
	}
}

static char *skip_block_comment(char *p, TokState *ts) {
	// Byte-at-a-time until aligned
	while ((uintptr_t)p & 7) {
		if (*p == '\0') error_at(p, "unclosed block comment");
		if (*p == '\n') { ts->line_no++; ts->at_bol = true; }
		if (p[0] == '*' && p[1] == '/') return p + 2;
		p++;
	}
	// Process 8 bytes at a time - skip chunks with no NUL, newline, or asterisk
	uint64_t nl_mask = SWAR_BROADCAST('\n');
	uint64_t star_mask = SWAR_BROADCAST('*');
	for (;;) {
		uint64_t v;
		memcpy(&v, p, 8);
		if (SWAR_HAS_ZERO(v) || SWAR_HAS_ZERO(v ^ nl_mask) || SWAR_HAS_ZERO(v ^ star_mask)) {
			// Interesting bytes in this chunk — scan byte by byte
			for (int i = 0; i < 8; i++) {
				if (p[i] == '\0') error_at(p + i, "unclosed block comment");
				if (p[i] == '\n') { ts->line_no++; ts->at_bol = true; }
				if (p[i] == '*' && p[i + 1] == '/') return p + i + 2;
			}
		}
		p += 8;
	}
}

static char *string_literal_end(char *p) {
	for (; *p != '"'; p++) {
		if (*p == '\0') error_at(p, "unclosed string literal");
		if (*p == '\\') {
			if (p[1] == '\0') error_at(p, "unclosed string literal");
			p++;
		}
	}
	return p;
}

// Scan C++11/C23 raw string literal: R"delim(content)delim"
static char *raw_string_literal_end(char *p, TokState *ts) {
	char *delim_start = p + 1;
	char *paren = delim_start;
	while (*paren && *paren != '(' && *paren != ')' && *paren != '\\' && *paren != ' ' &&
	       *paren != '\t' && *paren != '\n' && (paren - delim_start) < 17)
		paren++;

	if (*paren != '(') return NULL;

	int delim_len = paren - delim_start;
	char *content = paren + 1;
	// Search for )delimiter"
	for (char *q = content; *q; q++) {
		if (*q == '\n') ts->line_no++;
		if (*q == ')' && (delim_len == 0 || strncmp(q + 1, delim_start, delim_len) == 0) &&
		    q[1 + delim_len] == '"') {
			return q + 1 + delim_len + 1;
		}
	}

	error_at(p, "unclosed raw string literal");
}

static inline __attribute__((always_inline)) Token *new_token(TokenKind kind, char *start, char *end, TokState *ts) {
	Token *tok = pool_alloc_token();
	tok->kind = kind;
	tok->len = end - start;
	tok->next_idx = 0;
	tok->tag = 0;
	tok->match_idx = 0;
	tok->flags = (ts->at_bol ? TF_AT_BOL : 0) | (ts->has_space ? TF_HAS_SPACE : 0);
	TokenCold *c = tok_cold(tok);
	c->loc_offset = (uint32_t)(start - ctx->current_file->contents);
	{
		long long ln = (long long)ts->line_no + ctx->current_file->line_delta;
		int clamped = ln > 0x0FFFFF ? 0x0FFFFF : (ln < -0x100000 ? -0x100000 : (int)ln);
		c->line_no = clamped;
	}
	c->file_idx = ctx->current_file->file_no;
	ts->at_bol = ts->has_space = false;
	return tok;
}

static Token *read_string_literal(char *start, char *quote, TokState *ts) {
	char *end = string_literal_end(quote + 1);
	return new_token(TK_STR, start, end + 1, ts);
}

static Token *read_raw_string_literal(char *start, char *quote, TokState *ts) {
	char *end = raw_string_literal_end(quote, ts);
	if (!end) error_at(start, "invalid raw string literal");
	return new_token(TK_STR, start, end, ts);
}

static Token *read_char_literal(char *start, char *quote, TokState *ts) {
	char *p = quote + 1;
	if (*p == '\0') error_at(start, "unclosed char literal");
	for (; *p != '\''; p++) {
		if (*p == '\n' || *p == '\0') error_at(p, "unclosed char literal");
		if (*p == '\\') {
			p++;
			if (*p == '\0') error_at(p, "unclosed char literal");
		}
	}
	return new_token(TK_NUM, start, p + 1, ts);
}

// Check for C23 extended float suffix; returns suffix length to strip (0 if none)
static int get_extended_float_suffix(const char *p, int len, const char **replacement) {
	static const struct {
		const char suffix[6];
		int slen;
		const char *repl;
	} tbl[] = {
	    {"f128x", 5, "L"},
	    {"bf16", 4, "f"},
	    {"f128", 4, "L"},
	    {"f32x", 4, NULL},
	    {"f64x", 4, "L"},
	    {"f64", 3, NULL},
	    {"f32", 3, "f"},
	    {"f16", 3, "f"},
	};

	if (replacement) *replacement = NULL;
	if (len < 3) return 0;
	const char *e = p + len;
	for (int i = 0; i < (int)(sizeof(tbl) / sizeof(*tbl)); i++) {
		int sl = tbl[i].slen;
		if (len < sl) continue;
		bool match = true;
		for (int j = 0; j < sl; j++)
			if ((e[-sl + j] | 0x20) != tbl[i].suffix[j]) {
				match = false;
				break;
			}
		if (match) {
			if (replacement) *replacement = tbl[i].repl;
			return sl;
		}
	}
	return 0;
}

static inline void classify_punct(Token *t) {
	char *loc = tok_loc(t);
	char c = loc[0];
	if (t->len == 1) {
		if (c == '=' || c == '[') t->tag = TT_ASSIGN;
		else if (c == '.')
			t->tag = TT_MEMBER;
		else if (c == '{' || c == '}' || c == ';' || c == ':')
			t->tag = TT_STRUCTURAL;
		if (c == '(' || c == '[' || c == '{') t->flags |= TF_OPEN;
		else if (c == ')' || c == ']' || c == '}')
			t->flags |= TF_CLOSE;
	} else if (t->len == 2) {
		char c2 = loc[1];
		if (c2 == '=' && c != '!' && c != '<' && c != '>' && c != '=') t->tag = TT_ASSIGN;
		else if (c == '+' && c2 == '+')
			t->tag = TT_ASSIGN;
		else if (c == '-' && c2 == '-')
			t->tag = TT_ASSIGN;
		else if (c == '-' && c2 == '>')
			t->tag = TT_MEMBER;
	} else if (t->len == 3 && loc[2] == '=' && (c == '<' || c == '>') && loc[1] == c)
		t->tag = TT_ASSIGN;
}

static inline bool delimiters_match(Token *open, Token *close) {
	char a = tok_loc(open)[0], b = tok_loc(close)[0];
	return a == '(' ? b == ')' : b == a + 2;
}

static inline bool tok_name_eq(Token *a, Token *b) {
	return a->len == b->len && memcmp(tok_loc(a), tok_loc(b), a->len) == 0;
}

static Token *find_wrapper_callee(Token *body) {
	Token *end = tok_match(body);
	if (!end) return NULL;

	Token *tok = tok_next(body);
	while (tok && tok != end && tok_loc(tok)[0] == ';') tok = tok_next(tok);
	if (tok && tok != end && (tok->tag & TT_RETURN)) tok = tok_next(tok);
	while (tok && tok != end && tok_loc(tok)[0] == ';') tok = tok_next(tok);

	if (!tok || tok == end || tok->kind != TK_IDENT) return NULL;
	Token *open = tok_next(tok);
	if (!open || tok_loc(open)[0] != '(' || !tok_match(open)) return NULL;

	Token *after = tok_next(tok_match(open));
	while (after && after != end && tok_loc(after)[0] == ';') after = tok_next(after);
	return after == end ? tok : NULL;
}

static File *new_file(char *name, int file_no, char *contents) {
	File *file = arena_alloc(&ctx->main_arena, sizeof(File));
	file->name = intern_filename(name);
	file->file_no = file_no;
	file->contents = contents;
	file->contents_len = strlen(contents);
	file->owns_contents = true;
	return file;
}

static void add_input_file(File *file) {
	ARENA_ENSURE_CAP(&ctx->main_arena,
			 ctx->input_files,
			 ctx->input_file_count + 1,
			 ctx->input_file_capacity,
			 16,
			 File *);
	ctx->input_files[ctx->input_file_count++] = file;
}

static File *
new_file_view(const char *name, File *base, int line_delta, bool is_system, bool is_include_entry) {
	File *file = arena_alloc(&ctx->main_arena, sizeof(File));
	file->name = intern_filename(name ? name : base->name);
	file->file_no = ctx->input_file_count;
	file->contents = base->contents;
	file->contents_len = base->contents_len;
	file->line_delta = line_delta;
	file->is_system = is_system;
	file->is_include_entry = is_include_entry;
	add_input_file(file);
	return file;
}

// Scan line directive; returns position after it, or NULL if not a line marker
static char *scan_line_directive(char *p, File *base_file, int *line_no, bool *in_system_include) {
	int directive_line = *line_no;
	p++; // skip '#'
	while (*p == ' ' || *p == '\t') p++;

	if (!strncmp(p, "line", 4) && (p[4] == ' ' || p[4] == '\t')) {
		p += 4;
		while (*p == ' ' || *p == '\t') p++;
	}

	if (!IS_DIGIT(*p))
		return NULL;

	unsigned long new_line = 0;
	while (IS_DIGIT(*p)) {
		unsigned int digit = *p - '0';
		if (new_line > (ULONG_MAX - digit) / 10) return NULL;
		new_line = new_line * 10 + digit;
		p++;
	}
	while (*p == ' ' || *p == '\t') p++;

	char *filename = NULL;
	if (*p == '"') {
		p++;
		char *start = p;
		while (*p && *p != '"') {
			if (*p == '\\' && p[1]) p++;
			p++;
		}
		int raw_len = p - start;
		filename = malloc(raw_len + 1);
		if (!filename) error("out of memory");
		// Unescape \\ -> \ and \" -> "; preserve single backslashes (Windows paths)
		int len = 0;
		for (char *s = start; s < start + raw_len; s++) {
			if (*s == '\\' && s + 1 < start + raw_len &&
			    (s[1] == '\\' || s[1] == '"')) {
				s++;
			}
			filename[len++] = *s;
		}
		filename[len] = '\0';
		if (*p == '"') p++;
	}

	bool is_system = false, is_entering = false, is_returning = false;
	while (*p == ' ' || *p == '\t') p++;
	while (IS_DIGIT(*p)) {
		int flag = 0;
		while (IS_DIGIT(*p)) {
			if (flag > INT_MAX / 10) {
				while (IS_DIGIT(*p)) p++; // skip remaining digits
				break;
			}
			flag = flag * 10 + (*p - '0');
			p++;
		}
		if (flag == 1) is_entering = true;
		if (flag == 2) is_returning = true;
		if (flag == 3) is_system = true;
		while (*p == ' ' || *p == '\t') p++;
	}

	if (is_entering && is_system) *in_system_include = true;
	else if (is_returning && !is_system)
		*in_system_include = false;

	if (new_line > (unsigned long)INT_MAX) {
		free(filename);
		return NULL;
	}

	long long ld = (long long)(int)new_line - ((long long)directive_line + 1);
	if (ld > INT_MAX) ld = INT_MAX;
	if (ld < INT_MIN) ld = INT_MIN;
	int line_delta = (int)ld;
	File *view = new_file_view(filename ? filename : ctx->current_file->name,
				   base_file,
				   line_delta,
				   is_system,
				   *in_system_include);
	ctx->current_file = view;
	free(filename);

	// Skip to end of directive
	while (*p && *p != '\n') p++;
	if (*p == '\n') {
		p++;
		(*line_no)++;
	}
	return p;
}

// Scan preprocessor number; sets *is_float if it contains '.', 'e/E', or 'p/P'
static char *scan_pp_number(char *p, bool *is_float) {
	bool is_hex = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'));
	*is_float = false;
	for (;;) {
		char c = *p;
		if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') && (p[1] == '+' || p[1] == '-')) {
			if (c == 'p' || c == 'P' || !is_hex) *is_float = true;
			p += 2;
		} else if (c == '.') {
			*is_float = true;
			p++;
		} else if (ident_char[(unsigned char)c]) {
			p++;
		} else if (c == '\'' && ident_char[(unsigned char)p[1]]) {
			p++;
		} else
			break;
	}
	return p;
}

static Token *tokenize(File *file) {
	File *base_file = file;
	ctx->current_file = file;
	char *p = file->contents;

	// Pre-allocate token pool based on input size
	token_pool_ensure(token_count + file->contents_len / 2 + 4096);

	uint32_t first_idx = 0;
	uint32_t cur_idx = 0;
	#define LINK(nt) do { \
		uint32_t _ni = tok_idx(nt); \
		if (cur_idx) token_pool[cur_idx].next_idx = _ni; \
		else first_idx = _ni; \
		cur_idx = _ni; \
	} while(0)
	TokState ts = {true, false, 1};

	bool in_system_include = false;

	while (*p) {
		if (ts.at_bol && *p == '#') {
			char *directive_start = p;
			char *after =
			    scan_line_directive(p, base_file, &ts.line_no, &in_system_include);
			if (after) {
				p = after;
				ts.at_bol = true;
				ts.has_space = false;
				continue;
			}

			while (*p &&
			       *p != '\n')
				p++;
			LINK(new_token(TK_PREP_DIR, directive_start, p, &ts));
			tok_set_at_bol(&token_pool[cur_idx], true);
			if (*p == '\n') {
				p++;
				ts.line_no++;
				ts.at_bol = true;
				ts.has_space = false;
			}
			continue;
		}

		if (p[0] == '/' && p[1] == '/')
		{
			p = skip_line_comment(p + 2);
			ts.has_space = true;
			continue;
		}
		if (p[0] == '/' && p[1] == '*')
		{
			p = skip_block_comment(p + 2, &ts);
			ts.has_space = true;
			continue;
		}
		if (*p == '\n')
		{
			p++;
			ts.line_no++;
			ts.at_bol = true;
			ts.has_space = false;
			continue;
		}
		if (is_space(*p))
		{
			do {
				p++;
			} while (is_space(*p));
			ts.has_space = true;
			continue;
		}
		if (IS_DIGIT(*p) || (*p == '.' && IS_DIGIT(p[1]))) {
			char *start = p;
			bool is_float;
			p = scan_pp_number(p, &is_float);
			Token *t = new_token(TK_NUM, start, p, &ts);
			LINK(t);
			if (is_float) t->flags |= TF_IS_FLOAT;
			else if (!(start[0] == '0' && (start[1] == 'x' || start[1] == 'X' ||
						       start[1] == 'b' || start[1] == 'B')) &&
				 get_extended_float_suffix(start, t->len, NULL))
				t->flags |= TF_IS_FLOAT;
			continue;
		}
		{ // Raw string literals
			int raw_pfx = (p[0] == 'R')						     ? 0
				      : (p[0] == 'u' && p[1] == '8' && p[2] == 'R')		     ? 2
				      : ((p[0] == 'L' || p[0] == 'u' || p[0] == 'U') && p[1] == 'R') ? 1
												     : -1;
			if (raw_pfx >= 0 && p[raw_pfx] == 'R' && p[raw_pfx + 1] == '"') {
				Token *nt = read_raw_string_literal(p, p + raw_pfx + 1, &ts);
				LINK(nt);
				p += nt->len;
				continue;
			}
		}
		if (*p == '"')
		{
			Token *nt = read_string_literal(p, p, &ts);
			LINK(nt);
			p += nt->len;
			continue;
		}
		if ((p[0] == 'u' && p[1] == '8' && p[2] == '"') ||
		    ((p[0] == 'u' || p[0] == 'U' || p[0] == 'L') && p[1] == '"')) {
			char *start = p;
			p += (p[0] == 'u' && p[1] == '8') ? 2 : 1;
			Token *nt = read_string_literal(start, p, &ts);
			LINK(nt);
			p = start + nt->len;
			continue;
		}
		if (*p == '\'')
		{
			Token *nt = read_char_literal(p, p, &ts);
			LINK(nt);
			p += nt->len;
			continue;
		}
		if ((p[0] == 'u' || p[0] == 'U' || p[0] == 'L') && p[1] == '\'') {
			Token *nt = read_char_literal(p, p + 1, &ts);
			LINK(nt);
			p += nt->len;
			continue;
		}
		int ident_len = read_ident(p);
		if (ident_len) {
			Token *t = new_token(TK_IDENT, p, p + ident_len, &ts);
			LINK(t);
				uintptr_t kw = keyword_lookup(p, ident_len);
			if (kw) {
				if (kw & KW_MARKER) {
					t->kind = TK_KEYWORD;
					t->tag = (uint32_t)(kw & ~KW_MARKER);
				} else
					t->tag = (uint32_t)kw;
				t->flags |= (uint8_t)(kw >> KW_FLAGS_SHIFT);
			}
			p += ident_len;
			continue;
		}
		int punct_len = read_punct(p);
		if (punct_len) {
			int abs_len = punct_len < 0 ? -punct_len : punct_len;
			Token *t = new_token(TK_PUNCT, p, p + abs_len, &ts);
			LINK(t);
			if (punct_len < 0) {
				char *norm;
				switch (abs_len == 4 ? '%' : p[0]) {
				case '<':
					norm =
					    p[1] == ':' ? digraph_norm_bracket_open : digraph_norm_brace_open;
					break;
				case ':': norm = digraph_norm_bracket_close; break;
				case '%':
					norm = abs_len == 4 ? digraph_norm_paste
							    : (p[1] == '>' ? digraph_norm_brace_close
									   : digraph_norm_hash);
					break;
				default: norm = p; break;
				}
				// Patch source buffer in place for digraph normalization
				p[0] = norm[0];
				if (abs_len == 4) p[1] = norm[1]; // %:%: -> ##
				t->len = (abs_len == 4) ? 2 : 1;
			}
			classify_punct(t);
			p += abs_len;
			continue;
		}
		error_at(p, "invalid token");
	}

	LINK(new_token(TK_EOF, p, p, &ts));
	#undef LINK

	Token *first = first_idx ? &token_pool[first_idx] : NULL;

	// Link matching delimiters: connect every TF_OPEN to its TF_CLOSE via tok->match.
	// Also detect C23 [[ ... ]] attributes and tag the first '[' with TF_C23_ATTR.
	{
		Token *stack[4096];
		int sp = 0;
		for (Token *t = first; t && t->kind != TK_EOF; t = tok_next(t)) {
			if (t->flags & TF_OPEN) {
				if (sp >= 4096) error_tok(t, "delimiter nesting exceeds 4096");
				stack[sp++] = t;
				Token *tn = tok_next(t);
				if (tok_loc(t)[0] == '[' && tn && tok_loc(tn)[0] == '[' && (tn->flags & TF_OPEN))
					t->flags |= TF_C23_ATTR;
			} else if (t->flags & TF_CLOSE) {
				if (sp == 0) error_tok(t, "unmatched closing delimiter");
				Token *open = stack[--sp];
				if (!delimiters_match(open, t))
					error_tok(t,
						  "mismatched closing delimiter '%c' for opener '%c'",
						  tok_loc(t)[0],
						  tok_loc(open)[0]);
				open->match_idx = tok_idx(t);
				t->match_idx = tok_idx(open);
			}
		}
		if (sp > 0)
			error_tok(stack[sp - 1], "unclosed delimiter '%c'", tok_loc(stack[sp - 1])[0]);

		// Pre-scan function bodies: tag '{' with TT_SPECIAL_FN / TT_ASM / TT_NORETURN_FN(=vfork).
		// Propagate special-function taint transitively through wrapper chains.
		// Function body heuristic: depth-0 '{' preceded by ')'.
		{
			typedef struct {
				Token *name;
				Token *body;
			} FunctionScan;

			FunctionScan *functions = NULL;
			int function_count = 0;
			int function_capacity = 0;
			Token *func_name = NULL;
			for (Token *t = first; t && t->kind != TK_EOF; t = tok_next(t)) {
				if (is_potential_func_name(t))
					func_name = t;
				if (tok_loc(t)[0] == '{' && (t->flags & TF_OPEN) && t->match_idx) {
					Token *end = tok_match(t);
					for (Token *b = tok_next(t); b != end; b = tok_next(b)) {
						if (b->tag & TT_SPECIAL_FN) {
							if (tok_loc(b)[0] == 'v' && b->len == 5) {
								// Only taint if vfork is actually called: vfork()
								// Also catch (vfork)() where next tok is ')' then '('.
								// A bare reference (return vfork; / &vfork) is safe.
								Token *nx = tok_next(b);
								if (nx && nx != end && tok_loc(nx)[0] == '(')
									t->tag |= TT_NORETURN_FN;
								else if (nx && nx != end && tok_loc(nx)[0] == ')') {
									Token *nx2 = tok_next(nx);
									if (nx2 && nx2 != end && tok_loc(nx2)[0] == '(')
										t->tag |= TT_NORETURN_FN;
								}
							} else
								t->tag |= TT_SPECIAL_FN;
						}
						if (b->tag & TT_ASM) t->tag |= TT_ASM;
					}
					if (func_name) {
						ARENA_ENSURE_CAP(&ctx->main_arena,
								 functions,
								 function_count + 1,
								 function_capacity,
								 32,
								 FunctionScan);
						functions[function_count++] = (FunctionScan){.name = func_name, .body = t};
					}
					func_name = NULL;
					t = end;
				}
			}

			// Precompute wrapper callee tokens and resolve to function indices.
			uint32_t *wrapper_taint = NULL;
			int *callee_idx = NULL;
			if (function_count > 0) {
				wrapper_taint = arena_alloc(&ctx->main_arena, (size_t)function_count * sizeof(*wrapper_taint));
				callee_idx = arena_alloc(&ctx->main_arena, (size_t)function_count * sizeof(*callee_idx));
				for (int i = 0; i < function_count; i++) {
					callee_idx[i] = -1;
					Token *callee = find_wrapper_callee(functions[i].body);
					if (!callee) continue;
					if (callee->tag & (TT_SPECIAL_FN | TT_NORETURN_FN)) {
						wrapper_taint[i] = callee->tag & (TT_SPECIAL_FN | TT_NORETURN_FN);
						continue;
					}
					for (int j = 0; j < function_count; j++) {
						if (tok_name_eq(callee, functions[j].name)) {
							callee_idx[i] = j;
							break;
						}
					}
				}
			}

			// Fixed-point: propagate taint through wrapper chains.
			bool changed;
			do {
				changed = false;
				for (int i = 0; i < function_count; i++) {
					if (wrapper_taint[i] || callee_idx[i] < 0) continue;
					if (wrapper_taint[callee_idx[i]]) {
						wrapper_taint[i] = wrapper_taint[callee_idx[i]];
						changed = true;
					}
				}
			} while (changed);

			// Apply taint to callers of tainted functions.
			// Propagate vfork taint (TT_NORETURN_FN) transitively through
			// call chains: if function A calls function B, and B's body
			// contains vfork (or B is a wrapper of vfork), A gets tainted.
			// Also propagate wrapper_taint for setjmp/asm wrappers (single pass).
			// Use fixed-point iteration for vfork since it must cross
			// non-wrapper boundaries (e.g. vfork returned as function pointer).
			do {
				changed = false;
				for (int i = 0; i < function_count; i++) {
					Token *body = functions[i].body;
					if (body->tag & (TT_SPECIAL_FN | TT_NORETURN_FN)) continue;
					Token *end = tok_match(body);
					for (Token *b = tok_next(body); b != end; b = tok_next(b)) {
						if (b->kind != TK_IDENT || !tok_next(b) || tok_loc(tok_next(b))[0] != '(')
							continue;
						for (int j = 0; j < function_count; j++) {
							if (!tok_name_eq(b, functions[j].name)) continue;
							// Wrapper taint (setjmp/vfork wrappers)
							if (wrapper_taint[j] && !(body->tag & wrapper_taint[j])) {
								body->tag |= wrapper_taint[j];
								changed = true;
							}
							// Direct vfork taint from body scan
							uint32_t vfork_taint = functions[j].body->tag & TT_NORETURN_FN;
							if (vfork_taint && !(body->tag & TT_NORETURN_FN)) {
								body->tag |= TT_NORETURN_FN;
								changed = true;
							}
						}
					}
				}
			} while (changed);
		}
	}

	return first;
}

static void ensure_keyword_cache(void) {
	if (!keyword_cache[0].name && !keyword_cache[1].name) init_keyword_map();
	else if (ctx->keyword_cache_features != ctx->features) init_keyword_map();
}

static Token *tokenize_buffer(char *name, char *buf) {
	if (!buf) return NULL;

	File *file = new_file(name, ctx->input_file_count, buf);
	add_input_file(file);
    
	ensure_keyword_cache();

	return tokenize(file);
}

Token *tokenize_file(char *path) {
	ensure_keyword_cache();

#ifdef _WIN32
	HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
				   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return NULL;
	DWORD file_size = GetFileSize(hFile, NULL);
	if (file_size == INVALID_FILE_SIZE || file_size == 0) {
		CloseHandle(hFile);
		if (file_size == 0) {
			char *buf = malloc(8);
			if (!buf) return NULL;
			memset(buf, 0, 8);
			File *file = new_file(path, ctx->input_file_count, buf);
			add_input_file(file);
			return tokenize(file);
		}
		return NULL;
	}
	char *buf = malloc((size_t)file_size + 8);
	if (!buf) { CloseHandle(hFile); return NULL; }
	DWORD bytes_read = 0;
	if (!ReadFile(hFile, buf, file_size, &bytes_read, NULL) || bytes_read != file_size) {
		free(buf);
		CloseHandle(hFile);
		return NULL;
	}
	CloseHandle(hFile);
	memset(buf + file_size, 0, 8);
	File *file = new_file(path, ctx->input_file_count, buf);
	add_input_file(file);
	return tokenize(file);
#else
	int fd = open(path, O_RDONLY);
	if (fd < 0) return NULL;
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
		close(fd);
		return NULL;
	}
	size_t size = (size_t)st.st_size;
	if (size == 0) {
		close(fd);
		char *buf = malloc(8);
		if (!buf) return NULL;
		memset(buf, 0, 8);
		File *file = new_file(path, ctx->input_file_count, buf);
		add_input_file(file);
		return tokenize(file);
	}
	// malloc + read with 8-byte padding for safe SWAR comment scanning
	char *buf = malloc(size + 8);
	if (!buf) { close(fd); return NULL; }
	size_t total = 0;
	while (total < size) {
		ssize_t n = read(fd, buf + total, size - total);
		if (n <= 0) { free(buf); close(fd); return NULL; }
		total += (size_t)n;
	}
	close(fd);
	memset(buf + size, 0, 8);
	File *file = new_file(path, ctx->input_file_count, buf);
	add_input_file(file);
	return tokenize(file);
#endif
}

// full=false: reset for reuse; full=true: free everything
void tokenizer_teardown(bool full) {
	if (ctx->input_files) {
		for (int i = 0; i < ctx->input_file_count; i++) free_file_contents(ctx->input_files[i]);
	}
	if (full) {
		arena_free(&ctx->main_arena);
		memset(keyword_cache, 0, sizeof(keyword_cache));
		free(token_pool);
		free(token_cold);
		token_pool = NULL;
		token_cold = NULL;
		token_count = 1;
		token_cap = 0;
	} else {
		arena_reset(&ctx->main_arena);
		token_count = 1; // Reset pool index but keep allocation
	}
	ctx->input_files = NULL;
	ctx->input_file_count = 0;
	ctx->input_file_capacity = 0;
	ctx->current_file = NULL;
}

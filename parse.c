// Prism parse - C tokenizer. Expects preprocessed input (cc -E).
//
// Thread-local storage qualifier: all per-invocation mutable state uses this
// for thread safety. Each thread gets its own copy.
//
// Originally only based on the parsing logic from https://github.com/rui314/chibicc (MIT License)
//

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
#define KW_MARKER 0x80000000ULL // Internal marker bit for keyword map: values are (tag | KW_MARKER)
#define KW_FLAGS_SHIFT 32       // Extra token flags encoded in bits 32-39 of keyword value

#if defined(_MSC_VER)
#define ARENA_ALIGN 8
#else
#define ARENA_ALIGN (__alignof__(long double))
#endif

// equal() is defined as an always_inline function after its helpers (_equal_1, _equal_2, equal_n)

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
	TT_STORAGE = 1 << 8,	  // Storage class: extern, static, _Thread_local, thread_local, __thread
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
	TT_TYPEOF = 1 << 27,	  // typeof, typeof_unqual, __typeof__, __typeof, __typeof_unqual__, __typeof_unqual
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
	uint16_t ann;         // Pass 1 annotation flags (P1_SCOPE_*, P1_OE_*, P1_IS_DECL, P1_REJECTED)
	uint8_t  ch0;         // First source byte — avoids tok_loc() indirection in hot paths
	uint8_t  _pad[3];     // Explicit padding to 24 bytes (alignment)
}; // 24 bytes

typedef struct {
	uint32_t loc_offset;  // Byte offset from File->contents
	int32_t  line_no : 18;
	uint32_t file_idx : 14;
} TokenCold; // 8 bytes — error/debug path

typedef struct {
	char *key;
	void *val;
	uint32_t hash;
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
	uint64_t value;
	uint8_t len;
} KeywordEntry;

enum // Feature flags
{
	F_DEFER = 1,
	F_ZEROINIT = 2,
	F_LINE_DIR = 4,
	F_WARN_SAFETY = 8,
	F_FLATTEN = 16,
	F_ORELSE = 32,
	F_AUTO_UNREACHABLE = 64,
	F_AUTO_STATIC = 128,
	F_BOUNDS_CHECK = 256
};

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

	// Reusable typeof/VLA memset variable queue (hoisted from process_declarators)
	Token **typeof_vars;
	int typeof_var_count;
	int typeof_var_cap;

	// Pre-hoisted plain dimension temps for VLA eval-order preservation
	unsigned *bracket_dim_ids;         // Temp IDs for non-orelse brackets (0 = not hoisted)
	int bracket_dim_count;             // Count of pre-hoisted dimension temps
	int bracket_dim_cap;
	int bracket_dim_next;              // Next dim temp to consume during emit

	// Source-file #define directives consumed by cc -E, for non-flatten reconstruction
	char **source_defines;             // Array of "NAME=VALUE" or "NAME" strings (malloc'd)
	char **source_define_guards;       // Parallel: NULL (unconditional) or condition guard text (malloc'd)
	int source_define_count;
	int source_define_cap;

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

	// Pass 1 infrastructure (scope tree + per-function metadata)
	// Stored as void* because the struct definitions live in prism.c (after #include "parse.c")
	void *p1_scope_tree;       // ScopeInfo[] — flat array indexed by scope_id
	uint16_t p1_scope_count;
	uint16_t p1_scope_cap;
	void *p1_func_meta;        // FuncMeta[] — one per function definition
	int p1_func_meta_count;
	int p1_func_meta_cap;

	// Pass 1 shadow table: variable declarations that shadow typedefs
	void *p1_shadow_entries;   // P1ShadowEntry[] — shadow chain per name
	int p1_shadow_count;
	int p1_shadow_cap;
	HashMap p1_shadow_map;     // name → (index+1) into p1_shadow_entries

	// Pass 1 per-function entries: labels, gotos, defers, decls, switches, cases
	void *p1_func_entries;     // P1FuncEntry[] — flat combined array
	int p1_func_entry_count;
	int p1_func_entry_cap;

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
	if (size > SIZE_MAX - (ARENA_ALIGN - 1)) error("arena_alloc: size overflow");
	size = (size + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
	arena_ensure(arena, size);
	void *ptr = arena->current->data + arena->current->used;
	arena->current->used += size;
	return ptr;
}

static void *arena_alloc(Arena *arena, size_t size) {
	// GCC VRP hint: size is always a valid positive allocation, never a
	// sign-extended negative int.  Suppresses -Wstringop-overflow false
	// positives that appear when arena_alloc is inlined into p1_verify_cfg
	// and GCC concludes (size_t)signed_int could wrap.
	if (size > (size_t)PTRDIFF_MAX) __builtin_unreachable();
	void *ptr = arena_alloc_uninit(arena, size);
	memset(ptr, 0, size);
	return ptr;
}

static void *arena_realloc(Arena *arena, void *old, size_t old_size, size_t new_size) {
	if (new_size <= old_size) return old;
	// In-place extension: if old is at the top of the current block, just grow it
	if (old && arena->current) {
		size_t aligned_old = (old_size + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
		if ((char *)old + aligned_old == arena->current->data + arena->current->used) {
			size_t aligned_new = (new_size + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
			size_t diff = aligned_new - aligned_old;
			if (arena->current->used + diff <= arena->current->capacity) {
				arena->current->used += diff;
				memset((char *)old + old_size, 0, new_size - old_size);
				return old;
			}
		}
	}
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

typedef struct { ArenaBlock *block; size_t used; } ArenaMark;

static ArenaMark arena_mark(Arena *arena) {
	return (ArenaMark){ arena->current, arena->current ? arena->current->used : 0 };
}

static void arena_restore(Arena *arena, ArenaMark mark) {
	// Reset all blocks after the saved block, then restore saved position.
	for (ArenaBlock *b = mark.block ? mark.block->next : arena->head; b; b = b->next)
		b->used = 0;
	arena->current = mark.block;
	if (mark.block) mark.block->used = mark.used;
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
	// Zero slot 0 on first allocation: it is the reserved NULL sentinel
	// (next_idx==0 means "no next") and must never look like a real token.
	// Without this, any loop that accidentally iterates from i=0 reads UB
	// flags/tag bits and crashes via tok_next(garbage_next_idx).
	if (token_count <= 1) {
		memset(&token_pool[0], 0, sizeof(Token));
		memset(&token_cold[0], 0, sizeof(TokenCold));
	}
}

static inline Token *pool_alloc_token(void) {
	if (token_count == UINT32_MAX)
		error("maximum token limit reached");
	if (__builtin_expect(token_count < token_cap, 1)) return &token_pool[token_count++];
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
static inline uint64_t fast_hash(char *s, uint32_t len) {
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
	uint32_t hash32 = (uint32_t)hash;
	int mask = map->capacity - 1;
	for (int i = 0; i <= mask; i++) {
		HashEntry *ent = &map->buckets[(hash + i) & mask];
		if (ent->key && ent->key != TOMBSTONE && ent->hash == hash32 &&
		    ent->key_len == (uint16_t)keylen && !memcmp(ent->key, key, keylen))
			return ent->val;
		if (!ent->key) return NULL;
	}
	return NULL;
}

static void hashmap_resize(HashMap *map, int newcap) {
	HashMap new_map = {.buckets = arena_alloc(&ctx->main_arena, (size_t)newcap * sizeof(HashEntry)), .capacity = newcap};
	int new_mask = newcap - 1;
	for (int i = 0; i < map->capacity; i++) {
		HashEntry *ent = &map->buckets[i];
		if (ent->key && ent->key != TOMBSTONE) {
			uint64_t h = ent->hash;
			int idx;
			for (int j = 0;; j++) {
				idx = (h + j) & new_mask;
				if (!new_map.buckets[idx].key) break;
			}
			new_map.buckets[idx] = *ent;
			new_map.used++;
		}
	}
	// Old buckets are abandoned on the arena — reclaimed at arena_reset.
	*map = new_map;
}

static void hashmap_put(HashMap *map, char *key, int keylen, void *val) {
	if (!map->buckets) {
		map->buckets = arena_alloc(&ctx->main_arena, 64 * sizeof(HashEntry));
		map->capacity = 64;
	} else if ((unsigned long)map->used * 100 / (unsigned long)map->capacity >= 70) {
		hashmap_resize(map, map->capacity * 2);
	}

	uint64_t hash = fast_hash(key, keylen);
	uint32_t hash32 = (uint32_t)hash;
	int mask = map->capacity - 1;
	int first_empty = -1;

	for (int i = 0; i <= mask; i++) {
		int idx = (hash + i) & mask;
		HashEntry *ent = &map->buckets[idx];

		if (ent->key && ent->key != TOMBSTONE && ent->hash == hash32 &&
		    ent->key_len == (uint16_t)keylen && !memcmp(ent->key, key, keylen)) {
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
	ent->hash = hash32;
	ent->val = val;
	map->used++;
}

// Reset a HashMap whose buckets were arena-allocated and are now dead
// (e.g. after arena_reset).  Zeroes the entire struct so the next
// hashmap_put re-allocates fresh buckets from the arena.
static void hashmap_discard(HashMap *map) {
	*map = (HashMap){0};
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
	return tok->len == (uint32_t)len && tok->ch0 == (uint8_t)op[0] &&
	       !memcmp(tok_loc(tok) + 1, op + 1, len - 1);
}

static inline bool _equal_1(Token *tok, char c) {
	return tok->len == 1 && tok->ch0 == (uint8_t)c;
}
#define match_ch _equal_1

// Branchless multi-char punctuation set test: match_set(tok, CH(';') | CH(','))
// Covers ASCII 32-95 (all C punctuation except { | } ~).
#define CH(c) (1ULL << ((c) - 32))
#define match_set(tok, mask) ((tok)->len == 1 && (unsigned)((tok)->ch0 - 32) < 64u && ((mask) & (1ULL << ((tok)->ch0 - 32))))

// Statement expression open: ({ (skipping noise between '(' and '{')
// Handles _Pragma(...), __attribute__((...)), C23 [[...]], and #pragma directives
// that may legally appear between the opening '(' and the compound statement '{'.
static inline bool is_stmt_expr_open(Token *t) {
	if (!match_ch(t, '(')) return false;
	Token *n = tok_next(t);
	while (n && n->kind != TK_EOF) {
		if (n->kind == TK_PREP_DIR) { n = tok_next(n); continue; }
		if ((n->tag & TT_ATTR) && tok_next(n) && match_ch(tok_next(n), '(') &&
		    tok_match(tok_next(n))) { n = tok_next(tok_match(tok_next(n))); continue; }
		if (n->flags & TF_C23_ATTR) {
			Token *close = tok_match(n);
			if (close) { n = tok_next(close); continue; }
		}
		break;
	}
	return n && match_ch(n, '{');
}

// 'else' keyword (TT_IF covers both if and else; 'e' distinguishes)
static inline bool is_else_kw(Token *t) { return (t->tag & TT_IF) && t->ch0 == 'e'; }
// 'do' keyword (TT_LOOP covers for/while/do; 'd' distinguishes)
static inline bool is_do_kw(Token *t) { return (t->tag & TT_LOOP) && t->ch0 == 'd'; }
// Either else or do (no-condition ctrl-flow)
static inline bool is_else_or_do(Token *t) { return is_else_kw(t) || is_do_kw(t); }

static inline bool _equal_2(Token *tok, const char *s) {
	if (tok->len != 2 || tok->ch0 != (uint8_t)s[0]) return false;
	return tok_loc(tok)[1] == s[1];
}

static inline uint64_t keyword_lookup(char *key, int keylen) {
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
	return tok->kind <= TK_KEYWORD && next && next->ch0 == '(' &&
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
	    {"_Static_assert", TT_SKIP_DECL, true},
	    {"static_assert", TT_SKIP_DECL, true},
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
	    {"_Noreturn", TT_SKIP_DECL | TT_INLINE, true},
	    {"noreturn", TT_SKIP_DECL | TT_INLINE, true},
	    {"__inline", TT_INLINE, true},
	    {"__inline__", TT_INLINE, true},
	    {"_Thread_local", TT_STORAGE, true},
	    {"__thread", TT_STORAGE, true},
	    {"constexpr", TT_QUALIFIER, true},
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
	    {"__float128", TT_TYPE, true},
	    {"__float80", TT_TYPE, true},
	    {"__fp16", TT_TYPE, true},
	    {"__bf16", TT_TYPE, true},
	    {"_Float16", TT_TYPE, true},
	    {"_Float32", TT_TYPE, true},
	    {"_Float64", TT_TYPE, true},
	    {"_Float128", TT_TYPE, true},
	    {"_Float32x", TT_TYPE, true},
	    {"_Float64x", TT_TYPE, true},
	    {"_Float128x", TT_TYPE, true},
	    {"_Decimal32", TT_TYPE, true},
	    {"_Decimal64", TT_TYPE, true},
	    {"_Decimal128", TT_TYPE, true},
	    {"typeof_unqual", TT_TYPE | TT_TYPEOF, true},
	    {"__typeof_unqual__", TT_TYPE | TT_TYPEOF, true},
	    {"__typeof_unqual", TT_TYPE | TT_TYPEOF, true},
	    {"auto", TT_QUALIFIER | TT_TYPE, true},
	    {"register", TT_QUALIFIER | TT_REGISTER, true},
	    {"_Alignas", TT_QUALIFIER | TT_ALIGNAS, true},
	    {"alignas", TT_QUALIFIER | TT_ALIGNAS, true},
	    {"typeof", TT_TYPE | TT_TYPEOF, true},
	    {"__typeof__", TT_TYPE | TT_TYPEOF, true},
	    {"__typeof", TT_TYPE | TT_TYPEOF, true},
	    {"__auto_type", TT_TYPE | TT_TYPEOF, true},
	    {"_BitInt", TT_TYPE | TT_BITINT, true},
	    {"asm", TT_SKIP_DECL | TT_ASM, true},
	    {"__asm__", TT_SKIP_DECL | TT_ASM, true},
	    {"__asm", TT_SKIP_DECL | TT_ASM, true},
	    {"__attribute__", TT_ATTR | TT_QUALIFIER, true},
	    {"__attribute", TT_ATTR | TT_QUALIFIER, true},
	    {"__declspec", TT_ATTR | TT_QUALIFIER, true},
	    {"_Pragma", TT_ATTR, true},
	    {"__pragma", TT_ATTR, true},
	    {"__extension__", TT_INLINE, true},
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
	    {"__sigsetjmp", TT_SPECIAL_FN, false},
	    {"__siglongjmp", TT_SPECIAL_FN, false},
	    {"__setjmp", TT_SPECIAL_FN, false},
	    {"__longjmp", TT_SPECIAL_FN, false},
	    {"__longjmp_chk", TT_SPECIAL_FN, false},
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
		uint64_t val = entries[i].is_kw ? (entries[i].tag | KW_MARKER) : entries[i].tag;
		val |= (uint64_t)entries[i].extra_flags << KW_FLAGS_SHIFT;
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
	tok->ann = 0;
	tok->ch0 = (uint8_t)*start;
	TokenCold *c = tok_cold(tok);
	c->loc_offset = (uint32_t)(start - ctx->current_file->contents);
	{
		long long ln = (long long)ts->line_no + ctx->current_file->line_delta;
		int clamped = ln > 0x1FFFF ? 0x1FFFF : (ln < -0x20000 ? -0x20000 : (int)ln);
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
	char a = open->ch0, b = close->ch0;
	return a == '(' ? b == ')' : b == a + 2;
}

static Token *find_wrapper_callee(Token *body) {
	Token *end = tok_match(body);
	if (!end) return NULL;

	Token *tok = tok_next(body);
	while (tok && tok != end && tok->ch0 == ';') tok = tok_next(tok);
	if (tok && tok != end && (tok->tag & TT_RETURN)) tok = tok_next(tok);
	while (tok && tok != end && tok->ch0 == ';') tok = tok_next(tok);

	if (!tok || tok == end || tok->kind != TK_IDENT) return NULL;
	Token *open = tok_next(tok);
	if (!open || open->ch0 != '(' || !tok_match(open)) return NULL;

	Token *after = tok_next(tok_match(open));
	while (after && after != end && after->ch0 == ';') after = tok_next(after);
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

	if (!IS_DIGIT(*p)) return NULL;

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

	// MSVC-style `#line N "file"` has no GCC-style flags (no 1/2/3).
	// Infer system/user status from the filename path so non-flatten mode
	// correctly suppresses content from system headers regardless of whether
	// the preprocessor was GCC (flag 3) or MSVC (no flags).
	//
	// Declare view here (before any compound statement) so prism's transpilation
	// does not wrap the later new_file_view() call in a short-lived {} scope that
	// would leave ctx->current_file = view out of scope.
	bool msvc_style = !is_entering && !is_returning;
	File *view;
	if (msvc_style && filename) {
		const char *f = filename;
		// Paths matching well-known system include roots are system headers.
		if (strncmp(f, "/usr/include/", 13) == 0 ||
		    strncmp(f, "/usr/local/include/", 19) == 0 ||
		    strncmp(f, "/Library/", 9) == 0 ||
		    strncmp(f, "/Applications/Xcode", 19) == 0 ||
		    (strstr(f, "/lib/gcc/") && strstr(f, "/include/")) ||
		    strstr(f, "Windows Kits") || strstr(f, "Program Files")) {
			is_system = true;
			*in_system_include = true;
		} else if (*f == '/' || *f == '.' ||
		           (f[0] && f[1] == ':')) {
			// Absolute non-system path, relative path, or Windows drive letter
			// path (e.g., C:\Users\...) — user file, exit system mode.
			is_system = false;
			*in_system_include = false;
		}
		// Otherwise (bare filename, unknown absolute path): preserve current state.
	}
	view = new_file_view(filename ? filename : ctx->current_file->name,
			     base_file,
			     line_delta,
			     is_system,
			     *in_system_include);
	ctx->current_file = view;
	free(filename);

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
				uint64_t kw = keyword_lookup(p, ident_len);
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
				t->ch0 = (uint8_t)norm[0];
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
		int stack_cap = 256;
		Token **stack = arena_alloc_uninit(&ctx->main_arena, stack_cap * sizeof(Token *));
		int sp = 0;
		for (Token *t = first; t && t->kind != TK_EOF; t = tok_next(t)) {
			if (t->flags & TF_OPEN) {
				if (sp >= stack_cap) {
					int old_cap = stack_cap;
					stack_cap *= 2;
					stack = arena_realloc(&ctx->main_arena, stack, old_cap * sizeof(Token *), stack_cap * sizeof(Token *));
				}
				stack[sp++] = t;
				Token *tn = tok_next(t);
				if (t->ch0 == '[' && tn && tn->ch0 == '[' && (tn->flags & TF_OPEN))
					t->flags |= TF_C23_ATTR;
			} else if (t->flags & TF_CLOSE) {
				if (sp == 0) error_tok(t, "unmatched closing delimiter");
				Token *open = stack[--sp];
				if (!delimiters_match(open, t))
					error_tok(t,
						  "mismatched closing delimiter '%c' for opener '%c'",
						  t->ch0,
						  open->ch0);
				open->match_idx = tok_idx(t);
				t->match_idx = tok_idx(open);
			}
		}
		if (sp > 0)
			error_tok(stack[sp - 1], "unclosed delimiter '%c'", stack[sp - 1]->ch0);

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
				if (t->ch0 == '{' && (t->flags & TF_OPEN) && t->match_idx) {
					Token *end = tok_match(t);
					for (Token *b = tok_next(t); b != end; b = tok_next(b)) {
						if ((b->tag & TT_SPECIAL_FN) &&
						    !(tok_idx(b) >= 1 && (token_pool[tok_idx(b) - 1].tag & TT_MEMBER))) {
							if (b->ch0 == 'v' && b->len == 5) {
								// Taint on any appearance of vfork in the
								// function body.  A bare reference like
								// `fp = vfork; fp();` bypasses the old
								// call-site-only check.  The false-positive
								// cost (can't use defer in a function that
								// merely returns &vfork) is negligible.
								t->tag |= TT_NORETURN_FN;
							} else
								t->tag |= TT_SPECIAL_FN;
						}
						if (b->tag & TT_ASM) {
							// Only taint for asm goto — can jump to labels, bypassing defer.
							// Regular asm (volatile, inline) is safe.
							for (Token *ag = tok_next(b); ag && ag != end && ag->ch0 != '('; ag = tok_next(ag))
								if (ag->tag & TT_GOTO) { t->tag |= TT_ASM; break; }
						}
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
			// Build O(1) name→index map for function lookups.
			uint32_t *wrapper_taint = NULL;
			int *callee_idx = NULL;
			HashMap func_map = {0};
			if (function_count > 0) {
				wrapper_taint = arena_alloc(&ctx->main_arena, (size_t)function_count * sizeof(*wrapper_taint));
				callee_idx = arena_alloc(&ctx->main_arena, (size_t)function_count * sizeof(*callee_idx));
				for (int i = 0; i < function_count; i++) {
					hashmap_put(&func_map, tok_loc(functions[i].name),
						    functions[i].name->len,
						    (void *)(intptr_t)(i + 1));
				}
				for (int i = 0; i < function_count; i++) {
					callee_idx[i] = -1;
					Token *callee = find_wrapper_callee(functions[i].body);
					if (!callee) continue;
					// Mirror the direct-body scan at line 1765-1777:
					// TT_SPECIAL_FN in the body taints with TT_SPECIAL_FN
					// (setjmp/longjmp/pthread_exit) or TT_NORETURN_FN if the
					// callee is vfork specifically.  Bare TT_NORETURN_FN on
					// the callee (exit/abort/_Exit/thrd_exit/quick_exit)
					// does NOT taint the body — direct-body-scan doesn't,
					// and wrapper-propagation should not either, otherwise
					// a simple `void die(void) { exit(0); }` makes every
					// caller hard-error with a misleading "vfork()" message.
					// Pass 2 still emits a per-call-site warning at the
					// direct TT_NORETURN_FN call token.
					if (callee->tag & TT_SPECIAL_FN) {
						wrapper_taint[i] = (callee->ch0 == 'v' && callee->len == 5)
							? TT_NORETURN_FN  // vfork wrapper
							: TT_SPECIAL_FN;  // setjmp/longjmp/pthread_exit wrapper
						continue;
					}
					void *v = hashmap_get(&func_map, tok_loc(callee), callee->len);
					if (v) callee_idx[i] = (int)(intptr_t)v - 1;
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
			bool has_taint = false;
			for (int i = 0; i < function_count; i++) {
				if (wrapper_taint[i] || (functions[i].body->tag & (TT_SPECIAL_FN | TT_NORETURN_FN))) {
					has_taint = true;
					break;
				}
			}
			if (has_taint) {
			// Extract function-reference edges in a single pass over tokens.
			// This avoids re-scanning all function bodies on each fixed-point
			// iteration, reducing O(D*T) to O(T + D*E) where E = edge count.
			typedef struct { int from, to; } TaintEdge;
			TaintEdge *edges = NULL;
			int edge_count = 0, edge_cap = 0;
			uint64_t fn_bloom = 0;
			for (int i = 0; i < function_count; i++) {
				Token *n = functions[i].name;
				fn_bloom |= 1ULL << (((unsigned)n->ch0 ^ n->len) & 63);
			}
			for (int i = 0; i < function_count; i++) {
				Token *body = functions[i].body;
				if (body->tag & (TT_SPECIAL_FN | TT_NORETURN_FN)) continue;
				Token *end = tok_match(body);
				for (Token *b = tok_next(body); b != end; b = tok_next(b)) {
					if (b->kind != TK_IDENT) continue;
					if (!(fn_bloom & (1ULL << (((unsigned)b->ch0 ^ b->len) & 63)))) continue;
					// Skip identifiers that are clearly variable declarations,
					// not function calls/references.  At Pass 0 we have no symbol
					// table, so use lexical heuristics:
					// (1) Preceded by type/qualifier/storage keyword → declaration
					// (2) Preceded by ',' with a type keyword before that → multi-decl
					if (tok_idx(b) > tok_idx(body) + 1) {
						Token *prev = &token_pool[tok_idx(b) - 1];
						if (prev->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_SUE))
							continue;
						// Also skip `(void)name` cast pattern: prev is ')'
						// from a cast expression (not a function call).
						// Conservative: if prev is ')' and the matching '('
						// has a type-keyword token right after it, it's a cast.
						if (match_ch(prev, ')') && (prev->flags & TF_CLOSE) && prev->match_idx) {
							Token *open = tok_match(prev);
							Token *inner = open ? tok_next(open) : NULL;
							if (inner && (inner->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE)))
								continue;
						}
					}
					void *v = hashmap_get(&func_map, tok_loc(b), b->len);
					if (!v) continue;
					int j = (int)(intptr_t)v - 1;
					ENSURE_ARRAY_CAP(edges, edge_count + 1, edge_cap, 64, TaintEdge);
					edges[edge_count++] = (TaintEdge){i, j};
				}
			}
			// Fixed-point on extracted edges (no token re-scanning).
			do {
				changed = false;
				for (int e = 0; e < edge_count; e++) {
					int i = edges[e].from, j = edges[e].to;
					Token *body = functions[i].body;
					if (body->tag & (TT_SPECIAL_FN | TT_NORETURN_FN)) continue;
					if (wrapper_taint[j] && !(body->tag & wrapper_taint[j])) {
						body->tag |= wrapper_taint[j];
						changed = true;
					}
					uint32_t vt = functions[j].body->tag & TT_NORETURN_FN;
					if (vt && !(body->tag & TT_NORETURN_FN)) {
						body->tag |= TT_NORETURN_FN;
						changed = true;
					}
				}
			} while (changed);
			free(edges);
			} // has_taint
		}
	}

	// Phase: detect user-defined noreturn functions from declarations.
	// Scan file-scope tokens for _Noreturn, noreturn, [[noreturn]],
	// or __attribute__((noreturn)) / __attribute__((__noreturn__)).
	// When a noreturn specifier is found before a function declaration,
	// collect the function name into a hashmap, then do a single O(N)
	// pass to tag all occurrences (replaces prior O(N*K) inner loop).
	{
		HashMap nr_map = {0};
		for (Token *t = first; t && t->kind != TK_EOF; t = tok_next(t)) {
			bool is_noreturn = false;
			Token *scan_start = t;
			Token *attr_origin = t; // original position for backward scan

			// _Noreturn or noreturn keyword
			if (t->kind <= TK_KEYWORD &&
			    (equal(t, "_Noreturn") || equal(t, "noreturn")))
				is_noreturn = true;

			// [[noreturn]] / [[_Noreturn]] / [[__noreturn__]] — C23 attribute
			// Also handles namespaced forms: [[gnu::noreturn]], [[gnu::__noreturn__]]
			if (t->ch0 == '[' && (t->flags & TF_C23_ATTR) && t->match_idx) {
				Token *inner = tok_next(t);
				Token *attr_end = &token_pool[t->match_idx];
				if (inner && inner->ch0 == '[') {
					for (Token *a = tok_next(inner); a && a < attr_end; a = tok_next(a)) {
						if (a->kind <= TK_KEYWORD &&
						    (equal(a, "noreturn") || equal(a, "_Noreturn") ||
						     equal(a, "__noreturn__"))) {
							is_noreturn = true;
							break;
						}
						// Skip attribute arguments to avoid matching inside them
						if (a->kind <= TK_KEYWORD && tok_next(a) && tok_next(a)->ch0 == '(' && tok_next(a)->match_idx)
							a = &token_pool[tok_next(a)->match_idx];
					}
				}
				t = attr_end;  // advance past [[ ... ]]
				scan_start = t;
			}

			// __attribute__((noreturn)) or __attribute__((__noreturn__))
			// Handles comma-separated attribute lists: __attribute__((cold, noreturn))
			if (t->kind <= TK_KEYWORD && equal(t, "__attribute__")) {
				Token *p1 = tok_next(t);
				if (p1 && p1->ch0 == '(') {
					Token *p2 = tok_next(p1);
					if (p2 && p2->ch0 == '(' && p2->match_idx) {
						Token *close = &token_pool[p2->match_idx];
						for (Token *a = tok_next(p2); a && a < close; a = tok_next(a)) {
							if (a->kind <= TK_KEYWORD &&
							    (equal(a, "noreturn") || equal(a, "__noreturn__"))) {
								is_noreturn = true;
								break;
							}
							// Skip attribute arguments to avoid matching inside them
							if (a->kind <= TK_KEYWORD && tok_next(a) && tok_next(a)->ch0 == '(' && tok_next(a)->match_idx)
								a = &token_pool[tok_next(a)->match_idx];
						}
						t = tok_match(p1);  // advance past __attribute__(( ... ))
						scan_start = t;
					}
				}
			}

			// __declspec(noreturn) or __declspec(__noreturn__) — MSVC
			if (t->kind <= TK_KEYWORD && equal(t, "__declspec")) {
				Token *p1 = tok_next(t);
				if (p1 && p1->ch0 == '(' && p1->match_idx) {
					Token *close = &token_pool[p1->match_idx];
					for (Token *a = tok_next(p1); a && a < close; a = tok_next(a)) {
						if (a->kind <= TK_KEYWORD &&
						    (equal(a, "noreturn") || equal(a, "__noreturn__"))) {
							is_noreturn = true;
							break;
						}
						// Skip attribute arguments to avoid matching inside them
						if (a->kind <= TK_KEYWORD && tok_next(a) && tok_next(a)->ch0 == '(' && tok_next(a)->match_idx)
							a = &token_pool[tok_next(a)->match_idx];
					}
					t = close;  // advance past __declspec( ... )
					scan_start = t;
				}
			}

			if (!is_noreturn) continue;

			// Found noreturn annotation.  Look forward for function name:
			// the last TK_IDENT before the first depth-0 '(' before ';' or '{'.
			Token *fn_name = NULL;
			for (Token *s = scan_start; s && s->kind != TK_EOF; s = tok_next(s)) {
				char ch = s->ch0;
				if (ch == ';' || ch == '{') break;
				if (s->kind == TK_IDENT && tok_next(s) &&
				    tok_next(s)->ch0 == '(') {
					// Skip known type/qualifier keywords
					if (s->tag & (TT_SKIP_DECL | TT_INLINE | TT_QUALIFIER |
						       TT_TYPE | TT_STORAGE))
						continue;
					fn_name = s;
					break;
				}
			}
			if (!fn_name) {
				// Backward scan: attribute placed AFTER declarator.
				// Pattern: void my_die(void) __attribute__((noreturn));
				// Find last TK_IDENT before '(' scanning backward from attr.
				for (uint32_t pi = tok_idx(attr_origin); pi > 0; pi--) {
					Token *pt = &token_pool[pi - 1];
					if (pt->kind == TK_PREP_DIR) continue;
					if (pt->ch0 == ';' || pt->ch0 == '{' || pt->ch0 == '}') break;
					if (pt->kind == TK_IDENT && tok_next(pt) &&
					    tok_next(pt)->ch0 == '(') {
						if (pt->tag & (TT_SKIP_DECL | TT_INLINE | TT_QUALIFIER |
							       TT_TYPE | TT_STORAGE))
							continue;
						fn_name = pt;
						break;
					}
				}
			}
			if (!fn_name) continue;
			hashmap_put(&nr_map, tok_loc(fn_name), fn_name->len, (void *)1);
		}

		// Single O(N) pass to tag all occurrences of noreturn function names
		if (nr_map.used > 0) {
			uint64_t nr_bloom = 0;
			for (int i = 0; i < nr_map.capacity; i++) {
				HashEntry *ent = &nr_map.buckets[i];
				if (ent->key && ent->key != TOMBSTONE)
					nr_bloom |= 1ULL << (((unsigned char)ent->key[0] ^ ent->key_len) & 63);
			}
			for (Token *s = first; s && s->kind != TK_EOF; s = tok_next(s)) {
				if (s->kind == TK_IDENT &&
				    (nr_bloom & (1ULL << (((unsigned)s->ch0 ^ s->len) & 63))) &&
				    !(tok_idx(s) >= 1 && (token_pool[tok_idx(s) - 1].tag & TT_MEMBER)) &&
				    hashmap_get(&nr_map, tok_loc(s), s->len))
					s->tag |= TT_NORETURN_FN;
			}
		}
	}

	return first;
}

static void ensure_keyword_cache(void) {
	if (!keyword_cache[0].name && !keyword_cache[1].name) init_keyword_map();
	else if (ctx->keyword_cache_features != ctx->features) init_keyword_map();
}

static inline Token *finalize_load(char *name, char *buf) {
	File *file = new_file(name, ctx->input_file_count, buf);
	add_input_file(file);
	return tokenize(file);
}

static Token *tokenize_buffer(char *name, char *buf) {
	if (!buf) return NULL;
	ensure_keyword_cache();
	return finalize_load(name, buf);
}

Token *tokenize_file(char *path) {
	ensure_keyword_cache();

#ifdef _WIN32
	wchar_t wpath[PATH_MAX];
	int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, PATH_MAX);
	HANDLE hFile;
	if (wn > 0)
		hFile = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL,
				   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	else
		hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
				   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return NULL;
	LARGE_INTEGER li_size;
	if (!GetFileSizeEx(hFile, &li_size) || li_size.QuadPart < 0) {
		CloseHandle(hFile);
		return NULL;
	}
	if (li_size.QuadPart == 0) {
		CloseHandle(hFile);
		char *buf = malloc(8);
		if (!buf) return NULL;
		memset(buf, 0, 8);
		return finalize_load(path, buf);
	}
	if (li_size.QuadPart > 512LL * 1024 * 1024) {
		CloseHandle(hFile);
		fprintf(stderr, "error: file too large: %s\n", path);
		return NULL;
	}
	DWORD file_size = (DWORD)li_size.QuadPart;
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
	return finalize_load(path, buf);
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
		return finalize_load(path, buf);
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
	return finalize_load(path, buf);
#endif
}

// Pure parsing/analysis functions with no emit/transpilation dependencies.
// Used by both Pass 1 (analysis) and Pass 2 (emission) in prism.c.

// --- Type Definitions ---

typedef struct {
	Token *end;		  // First token after the type specifier
	bool saw_type : 1;	  // True if a type was recognized
	bool is_struct : 1;
	bool is_union : 1;
	bool is_enum : 1;
	bool is_typedef : 1;
	bool is_vla : 1;
	bool has_typeof : 1;
	bool has_atomic : 1;
	bool has_register : 1;
	bool has_volatile : 1;
	bool has_const : 1;
	bool has_void : 1;	  // True if void or void typedef
	bool has_raw : 1;	  // True if 'raw' keyword was skipped in type specifier
	bool has_extern : 1;
	bool has_static : 1;
	bool has_auto : 1;	  // C23 'auto' type inference
	bool has_constexpr : 1;   // C23 'constexpr'
	bool has_thread_local : 1; // _Thread_local, thread_local, __thread
	bool has_volatile_member : 1; // Struct/union has volatile-qualified fields
	bool is_array : 1;	      // Array type from typeof()/typeof_unqual/_Atomic(...) (not declarator [])
	bool type_vm : 1;	      // Any VM dimension in typeof/_Atomic parens (incl. ptr-to-VLA)
	uint8_t type_array_rank;      // Dimension count for is_array (multi-dim typeof)
} TypeSpecResult;

// Declarator parsing result
typedef struct {
	Token *end;		  // First token after declarator
	Token *var_name;
	bool is_pointer : 1;
	bool is_array : 1;
	bool is_vla : 1;
	bool is_func_ptr : 1;
	bool has_paren : 1;
	bool paren_pointer : 1;	  // Has pointer (*) inside parenthesized declarator
	bool paren_array : 1;
	bool has_init : 1;
	bool is_const : 1;
} DeclResult;

typedef enum {
	TDK_TYPEDEF,
	TDK_SHADOW,
	TDK_ENUM_CONST,
	TDK_VLA_VAR, // VLA variable (not typedef, but actual VLA array variable)
	TDK_STRUCT_TAG // struct/union tag (for VLA/volatile member propagation)
} TypedefKind;

typedef struct {
	char *name; // Points into token stream (no alloc needed)
	int prev_index;		    // Index of previous entry with same name (-1 if none)
	uint32_t token_index;       // Token pool index of the declaration
	uint32_t scope_open_idx;    // Token index of enclosing '{' (0 for file scope)
	uint32_t scope_close_idx;   // Token index of matching '}' (UINT32_MAX for file scope)
	uint16_t len;
	uint16_t scope_depth;	    // Scope where defined (aligns with ctx->block_depth)
	bool is_vla : 1;
	bool is_void : 1;
	bool is_const : 1;
	bool is_volatile : 1;
	bool is_ptr : 1;
	bool is_array : 1;
	bool is_shadow : 1;
	bool is_enum_const : 1;
	bool is_vla_var : 1;
	bool is_aggregate : 1;
	bool is_union : 1;
	bool is_func : 1;
	bool is_param : 1;
	bool has_volatile_member : 1;
	bool is_struct_tag : 1; // struct/union tag (not a typedef name)
	bool array_dim_complete : 1; // array typedef: sizeof(T)/sizeof(T[0]) valid at uses
	uint8_t array_rank;	// # of array dimensions (0 if not array);
				// used by -fbounds-check multi-dim wrap to
				// avoid false positives on pointer-element
				// arrays like `int *p[10]`.
				// ARRAY_RANK_WRAP_ALL: rank overflow (>15); never drop
				// the peel-off guard in try_bounds_check_subscript.
} TypedefEntry; // 32 bytes — two entries per 64-byte cache line

#define ARRAY_RANK_WRAP_ALL 255

typedef struct {
	TypedefEntry *entries;
	int count;
	int capacity;
	HashMap name_map; // Maps name -> index+1 of most recent entry (0 means not found)
	uint64_t bloom;   // Bloom filter: bit (ch0 ^ len) & 63. Fast negative lookup.
} TypedefTable;

// Per-token Pass 1 annotation flags (stored in Token.ann)
enum {
	P1_IS_TYPEDEF        = 1 << 0, // Token resolves to a real typedef at this position
	P1_SCOPE_LOOP        = 1 << 1, // This '{' opens a loop body
	P1_SCOPE_SWITCH      = 1 << 2, // This '{' opens a switch body
	P1_HAS_ENTRY         = 1 << 3, // Token has any typedef-table entry (typedef/enum/shadow/VLA)
	P1_OE_BRACKET        = 1 << 4, // orelse inside array dimension brackets
	P1_OE_DECL_INIT      = 1 << 5, // orelse inside declaration initializer
	P1_IS_DECL           = 1 << 6, // Phase 1D: token starts a variable declaration
	P1_SCOPE_INIT        = 1 << 7, // This '{' opens an initializer (compound literal, = {...})
	P1_REJECTED          = 1 << 8, // Phase 1D/1F/1G rejected this token (defense-in-depth signal)
	P1_DECL_BRACKET      = 1 << 9, // '[' is an array-declarator bracket (not an expression subscript)
	P1_UNEVAL_BRACKET    = 1 << 10, // '[' is inside an unevaluated operand (sizeof/_Alignof/typeof/offsetof/etc.)
};

#define tok_ann(t) ((t)->ann)

// Typedef query flags (single lookup, check multiple properties)
enum { TDF_TYPEDEF = 1, TDF_VLA = 2, TDF_VOID = 4, TDF_ENUM_CONST = 8, TDF_CONST = 16, TDF_PTR = 32, TDF_ARRAY = 64, TDF_AGGREGATE = 128, TDF_FUNC = 256, TDF_PARAM = 512, TDF_VOLATILE = 1024, TDF_HAS_VOL_MEMBER = 2048, TDF_UNION = 4096 };

#define FEAT(f) (ctx->features & (f))

// --- Globals ---

static PRISM_THREAD_LOCAL TypedefTable typedef_table;

// Phase 3A: scope range context for typedef_add_entry.
static PRISM_THREAD_LOCAL uint32_t td_scope_open = 0;
static PRISM_THREAD_LOCAL uint32_t td_scope_close = UINT32_MAX;
static PRISM_THREAD_LOCAL bool p1_typedef_annotated; // true after p1_annotate_typedefs(); enables O(1) is_known_typedef

// Save/restore typedef scope range context (used 5+ sites).
#define TD_SCOPE_SAVE() uint32_t _tds_o = td_scope_open, _tds_c = td_scope_close
#define TD_SCOPE_RESTORE() do { td_scope_open = _tds_o; td_scope_close = _tds_c; } while(0)

// --- Utility Inlines ---

#define is_c23_attr(t) ((t) && ((t)->flags & TF_C23_ATTR))
#define is_sizeof_like(t) ((t)->flags & TF_SIZEOF)
#define is_enum_kw(t) ((t)->tag & TT_SUE && (t)->ch0 == 'e')

static inline bool is_identifier_like(Token *tok) {
	return tok->kind <= TK_KEYWORD; // TK_IDENT=0, TK_KEYWORD=1
}

// Skip balanced group: just advance past the matching close token.
static inline Token *skip_balanced_group(Token *tok) {
	Token *end = tok_match(tok);
	if (!end) return tok_next(tok);
	return tok_next(end);
}

static inline Token *skip_prep_dirs(Token *tok) {
	while (tok && tok->kind == TK_PREP_DIR) tok = tok_next(tok);
	return tok;
}

static bool is_pp_conditional(Token *s) {
	if (s->kind != TK_PREP_DIR) return false;
	const char *dp = tok_loc(s);
	if (*dp == '#') dp++;
	while (*dp == ' ' || *dp == '\t') dp++;
	return strncmp(dp, "ifdef", 5) == 0 || strncmp(dp, "ifndef", 6) == 0 ||
	       strncmp(dp, "elif",  4) == 0 || strncmp(dp, "else",   4) == 0 ||
	       strncmp(dp, "endif", 5) == 0 ||
	       (strncmp(dp, "if", 2) == 0 && (dp[2]==' '||dp[2]=='\t'||dp[2]=='('));
}

// Skip noise tokens (attributes, C23 [[...]], prep dirs) in analysis mode.
static Token *skip_noise(Token *tok) {
	while (tok && tok->kind != TK_EOF) {
		if (tok->tag & TT_ATTR) {
			tok = tok_next(tok);
			if (tok && tok->len == 1 && tok->ch0 == '(' && tok_match(tok))
				tok = tok_next(tok_match(tok));
		} else if (is_c23_attr(tok) && tok_match(tok)) {
			tok = tok_next(tok_match(tok));
		} else if (tok->kind == TK_PREP_DIR) {
			tok = tok_next(tok);
		} else break;
	}
	return tok;
}

// Check if a token is "noise" (attribute, C23 [[...]], or preprocessor directive).
// These tokens must be skipped via skip_noise() before any tag-based type checks.
static inline bool is_noise_token(Token *t) {
	return (t->tag & TT_ATTR) || is_c23_attr(t) || t->kind == TK_PREP_DIR;
}

#ifdef PRISM_DEBUG
#define ASSERT_NOT_NOISE(t) do { \
	if (is_noise_token(t)) \
		error_tok(t, "internal: tag check on noise token (skip_noise() missing)"); \
} while(0)
#else
#define ASSERT_NOT_NOISE(t) ((void)0)
#endif

#define SKIP_NOISE_CONTINUE(var) do { \
	Token *_sn = skip_noise(var); \
	if (_sn != (var)) { (var) = _sn; continue; } \
} while(0)

static Token *skip_to_semicolon(Token *tok, Token *end) {
	while (tok->kind != TK_EOF) {
		if (end && tok == end) return tok;
		if (tok->flags & TF_OPEN) { tok = tok_next(tok_match(tok)); continue; }
		if (tok->len == 1 && tok->ch0 == ';') return tok;
		if ((tok->flags & TF_CLOSE) && tok->ch0 == '}') return tok;
		tok = tok_next(tok);
	}
	return tok;
}

static Token *skip_pointers(Token *tok, bool *is_void) {
	while (tok && tok->kind != TK_EOF) {
		SKIP_NOISE_CONTINUE(tok);
		if ((tok->len == 1 && tok->ch0 == '*') || (tok->tag & TT_QUALIFIER)) {
			tok = tok_next(tok);
			if (is_void) *is_void = false;
		} else break;
	}
	return tok;
}

// --- Typedef Table ---

static void typedef_table_reset(void) {
	typedef_table.entries = NULL;
	typedef_table.count = 0;
	typedef_table.capacity = 0;
	typedef_table.bloom = 0;
	hashmap_discard(&typedef_table.name_map);
}

static int typedef_get_index(char *name, int len) {
	void *val = hashmap_get(&typedef_table.name_map, name, len);
	return val ? (int)(intptr_t)val - 1 : -1;
}

static void
typedef_add_entry(char *name, int len, int scope_depth, TypedefKind kind, bool is_vla, bool is_void) {
	// Skip duplicate re-definitions at the same scope (valid C11 §6.7/3).
	if (kind == TDK_TYPEDEF || kind == TDK_ENUM_CONST || kind == TDK_STRUCT_TAG) {
		int existing = typedef_get_index(name, len);
		if (existing >= 0) {
			TypedefEntry *prev = &typedef_table.entries[existing];
			if (prev->scope_depth == scope_depth && !prev->is_shadow &&
			    prev->scope_open_idx == td_scope_open && prev->scope_close_idx == td_scope_close &&
			    prev->is_struct_tag == (kind == TDK_STRUCT_TAG))
				return;
		}
	}
	if (kind == TDK_SHADOW || kind == TDK_VLA_VAR) {
		int existing = typedef_get_index(name, len);
		if (existing >= 0) {
			TypedefEntry *prev = &typedef_table.entries[existing];
			if (prev->scope_depth == scope_depth &&
			    prev->scope_open_idx == td_scope_open && prev->scope_close_idx == td_scope_close &&
			    prev->is_shadow == (kind == TDK_SHADOW) &&
			    prev->is_vla_var == (kind == TDK_VLA_VAR))
				return;
		}
	}

	ARENA_ENSURE_CAP(&ctx->main_arena,
			 typedef_table.entries,
			 typedef_table.count + 1,
			 typedef_table.capacity,
			 32,
			 TypedefEntry);
	int new_index = typedef_table.count++;
	TypedefEntry *e = &typedef_table.entries[new_index];
	e->name = name;
	e->len = len;
	e->scope_depth = scope_depth;
	e->is_vla = (kind == TDK_TYPEDEF || kind == TDK_VLA_VAR || kind == TDK_STRUCT_TAG) ? is_vla : false;
	e->is_void = (kind == TDK_TYPEDEF) ? is_void : false;
	e->is_const = false;
	e->is_shadow = (kind == TDK_SHADOW || kind == TDK_ENUM_CONST);
	e->is_enum_const = (kind == TDK_ENUM_CONST);
	e->is_vla_var = (kind == TDK_VLA_VAR);
	e->is_struct_tag = (kind == TDK_STRUCT_TAG);
	e->is_param = false;
	e->array_rank = 0;
	e->array_dim_complete = true;
	e->prev_index = typedef_get_index(name, len);
	e->token_index = 0;
	e->scope_open_idx = td_scope_open;
	e->scope_close_idx = td_scope_close;
	hashmap_put(&typedef_table.name_map, name, len, (void *)(intptr_t)(new_index + 1));
	typedef_table.bloom |= 1ULL << (((unsigned char)name[0] ^ len) & 63);
}

static TypedefEntry *typedef_lookup(Token *tok) {
	if (!is_identifier_like(tok)) return NULL;
	if (p1_typedef_annotated && !(tok_ann(tok) & P1_HAS_ENTRY)) return NULL;
	if (tok->kind == TK_KEYWORD && !(tok->tag & (TT_ORELSE | TT_DEFER)) && !(tok->flags & TF_RAW))
		return NULL;
	unsigned c0 = tok->ch0, tl = tok->len;
	if (!(typedef_table.bloom & (1ULL << ((c0 ^ tl) & 63)))) return NULL;
	int idx = typedef_get_index(tok_loc(tok), tok->len);
	uint32_t cur = tok_idx(tok);
	// Prefer non-struct-tag entries (ordinary identifiers) over struct tags.
	// ISO C11 §6.2.3: tag namespace is separate from ordinary identifiers.
	// When both exist at the same scope, the ordinary entry wins.
	// Fall back to struct tag only if no ordinary entry matches.
	TypedefEntry *tag_fallback = NULL;
	while (idx >= 0) {
		TypedefEntry *e = &typedef_table.entries[idx];
		if (e->token_index <= cur &&
		    cur >= e->scope_open_idx && cur < e->scope_close_idx) {
			if (!e->is_struct_tag) return e;
			if (!tag_fallback) tag_fallback = e;
		}
		idx = e->prev_index;
	}
	return tag_fallback;
}

// Lookup a struct/union tag entry, skipping ordinary identifiers/shadows.
// Enforces ISO C11 §6.2.3 namespace separation: tag names live in a
// different namespace from ordinary identifiers.
static TypedefEntry *tag_lookup(Token *tok) {
	if (!is_identifier_like(tok)) return NULL;
	unsigned c0 = tok->ch0, tl = tok->len;
	if (!(typedef_table.bloom & (1ULL << ((c0 ^ tl) & 63)))) return NULL;
	int idx = typedef_get_index(tok_loc(tok), tok->len);
	uint32_t cur = tok_idx(tok);
	while (idx >= 0) {
		TypedefEntry *e = &typedef_table.entries[idx];
		if (e->is_struct_tag && e->token_index <= cur &&
		    cur >= e->scope_open_idx && cur < e->scope_close_idx)
			return e;
		idx = e->prev_index;
	}
	return NULL;
}

static inline int typedef_flags(Token *tok) {
	TypedefEntry *e = typedef_lookup(tok);
	if (!e) return 0;
	if (e->is_enum_const) return TDF_ENUM_CONST;
	if (e->is_shadow) return 0;
	if (e->is_vla_var) return TDF_VLA | (e->is_param ? TDF_PARAM : 0) |
	       (e->has_volatile_member ? TDF_HAS_VOL_MEMBER : 0);
	if (e->is_struct_tag) return (e->is_vla ? TDF_VLA : 0) |
	       (e->has_volatile_member ? TDF_HAS_VOL_MEMBER : 0) |
	       (e->is_aggregate ? TDF_AGGREGATE : 0);
	return TDF_TYPEDEF | (e->is_vla ? TDF_VLA : 0) | (e->is_void ? TDF_VOID : 0) |
	       (e->is_const ? TDF_CONST : 0) | (e->is_volatile ? TDF_VOLATILE : 0) |
	       (e->is_ptr ? TDF_PTR : 0) |
	       (e->is_array ? TDF_ARRAY : 0) | (e->is_aggregate ? TDF_AGGREGATE : 0) |
	       (e->is_func ? TDF_FUNC : 0) | (e->has_volatile_member ? TDF_HAS_VOL_MEMBER : 0) |
	       (e->is_union ? TDF_UNION : 0);
}

// After Pass 1 annotation, is_known_typedef becomes O(1) bit check.
static inline bool _is_known_typedef(Token *tok) {
	if (__builtin_expect(p1_typedef_annotated, 1)) return tok_ann(tok) & P1_IS_TYPEDEF;
	return typedef_flags(tok) & TDF_TYPEDEF;
}
#define is_known_typedef(tok) _is_known_typedef(tok)
#define is_vla_typedef(tok) (typedef_flags(tok) & TDF_VLA)
#define is_void_typedef(tok) (typedef_flags(tok) & TDF_VOID)
#define is_known_enum_const(tok) (typedef_flags(tok) & TDF_ENUM_CONST)
#define is_const_typedef(tok) (typedef_flags(tok) & TDF_CONST)
#define is_ptr_typedef(tok) (typedef_flags(tok) & TDF_PTR)
#define is_array_typedef(tok) (typedef_flags(tok) & TDF_ARRAY)
#define is_func_typedef(tok) (typedef_flags(tok) & TDF_FUNC)
#define is_volatile_typedef(tok) (typedef_flags(tok) & TDF_VOLATILE)
#define has_volatile_member_typedef(tok) (typedef_flags(tok) & TDF_HAS_VOL_MEMBER)

// --- Type/Variable Classification ---

static bool is_type_keyword(Token *tok) {
	if (tok->tag & TT_TYPE) return true;
	if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD) return false;
	return is_known_typedef(tok);
}

static inline bool is_valid_varname(Token *tok) {
	return tok->kind == TK_IDENT || (tok->flags & TF_RAW) || (tok->tag & (TT_DEFER | TT_ORELSE));
}

// Token ends an expression (value-producing): ident, keyword, num, str, ), ].
static inline bool is_expr_ending(Token *t) {
	return (t->kind == TK_IDENT || t->kind == TK_KEYWORD ||
		t->kind == TK_NUM || t->kind == TK_STR) ||
	       match_set(t, CH(')') | CH(']'));
}

// Extended version including '}' (for compound literal / brace init contexts).
static inline bool is_expr_ending_brace(Token *t) {
	return is_expr_ending(t) || match_ch(t, '}');
}

static DeclResult parse_declarator(Token *tok, bool emit);

// Register enum constants as typedef shadows. tok points to opening '{'.
static void parse_enum_constants(Token *tok, int scope_depth) {
	if (!tok || !(tok->len == 1 && tok->ch0 == '{')) return;
	tok = tok_next(tok); // Skip '{'

	while (tok && tok->kind != TK_EOF && !(tok->len == 1 && tok->ch0 == '}')) {
		SKIP_NOISE_CONTINUE(tok);

		if (is_valid_varname(tok)) {
			int pre = typedef_table.count;
			typedef_add_entry(tok_loc(tok), tok->len, scope_depth, TDK_ENUM_CONST, false, false);
			if (typedef_table.count > pre)
				typedef_table.entries[typedef_table.count - 1].token_index = tok_idx(tok);
			tok = tok_next(tok);

			tok = skip_noise(tok); // Skip C23/GNU attributes on enumerator

			if (tok && tok->len == 1 && tok->ch0 == '=') {
				tok = tok_next(tok);
				while (tok && tok->kind != TK_EOF) {
					if (tok->flags & TF_OPEN) { tok = tok_next(tok_match(tok)); continue; }
					if (tok->len == 1 && (tok->ch0 == ',' || tok->ch0 == '}')) break;
					tok = tok_next(tok);
				}
			}

			if (tok && tok->len == 1 && tok->ch0 == ',') tok = tok_next(tok);
		} else tok = tok_next(tok);
	}
}

// Shadow-aware orelse keyword check: real typedefs suppress,
// variable/enum shadows do not.
static inline bool is_orelse_kw_shadow(Token *tok) {
	if (!(tok->tag & TT_ORELSE)) return false;
	TypedefEntry *te = typedef_lookup(tok);
	return !te || te->is_shadow;
}

// Strict orelse keyword check: any typedef table match suppresses.
static inline bool is_orelse_kw(Token *tok) {
	if (!(tok->tag & TT_ORELSE)) return false;
	return !typedef_lookup(tok);
}

// Positional orelse shadow disambiguation: only treat as keyword if the
// preceding token ends an expression (infix position).
static inline bool orelse_shadow_is_kw(Token *prev) {
	if (!prev) return false;
	// orelse is a keyword only after value-producing tokens.
	// After operators, type keywords, storage classes, control-flow
	// keywords (sizeof, return, if, ...), etc. it's a variable.
	if (prev->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_SUE |
			 TT_TYPEOF | TT_BITINT | TT_SKIP_DECL | TT_ALIGNAS |
			 TT_INLINE | TT_ATTR))
		return false;
	// Postfix ++/-- are value-producing; prefix ++/-- on a shadowed
	// variable is rare, so favour the postfix interpretation.
	if (prev->len == 2 && (prev->ch0 == '+' || prev->ch0 == '-') &&
	    tok_loc(prev)[1] == prev->ch0)
		return true;
	return is_expr_ending_brace(prev);
}

// --- Struct/VLA Analysis ---

// Find opening brace of a struct/union/enum body, or NULL if no body.
static Token *find_struct_body_brace(Token *tok) {
	Token *t = tok_next(tok);
	while (t && t->kind != TK_EOF) {
		SKIP_NOISE_CONTINUE(t);
		if (is_valid_varname(t) || (t->tag & TT_QUALIFIER) || is_type_keyword(t)) {
			t = tok_next(t);
		} else if (t->len == 1 && t->ch0 == ':') {
			// C23 enum fixed underlying type: enum E : int { ... }
			t = tok_next(t);
		} else break;
	}
	return (t && t->len == 1 && t->ch0 == '{') ? t : NULL;
}

// Walk backward from token_pool[start_idx - 1], skipping attribute noise
// (TF_CLOSE balanced groups, TT_ATTR keywords, TK_PREP_DIR, C23 [[...]]).
// Returns the effective predecessor token, or NULL if none found.
static inline Token *walk_back_past_noise(uint32_t start_idx) {
	uint32_t ti = start_idx;
	while (ti > 0) {
		Token *b = &token_pool[ti - 1];
		if (b->flags & TF_CLOSE) {
			Token *open = tok_match(b);
			if (!open) return NULL;
			ti = tok_idx(open);
			continue;
		}
		if ((b->tag & TT_ATTR) || b->kind == TK_PREP_DIR) {
			ti--;
			continue;
		}
		return b;
	}
	return NULL;
}

// Check if a token can legally precede an array bracket '[' in a type context.
static inline bool is_array_bracket_predecessor(Token *t) {
	if (is_type_keyword(t) ||
	    (t->tag & TT_QUALIFIER) ||
	    is_known_typedef(t) ||
	    (t->len == 1 && t->ch0 == '*') ||
	    (t->len == 1 && t->ch0 == '}'))
		return true;
	// struct/union/enum Tag [n]: identifier preceded by TT_SUE keyword.
	// Walk backward through token_pool, skipping attribute noise and
	// balanced groups (GNU __attribute__((...)), C23 [[...]], #pragma),
	// to find the effective predecessor.
	if (is_identifier_like(t)) {
		Token *b = walk_back_past_noise(tok_idx(t));
		return b && (b->tag & TT_SUE);
	}
	// ']' is a type predecessor only for multi-dimensional array types
	// like int[3][n], NOT for expression subscripts like arr[1][n].
	// Disambiguate by checking what precedes the matching '['.
	if (t->len == 1 && t->ch0 == ']') {
		Token *open = tok_match(t);
		if (!open) return true;
		Token *before_open = walk_back_past_noise(tok_idx(open));
		if (!before_open) return true;
		if (is_type_keyword(before_open) ||
		    (before_open->tag & (TT_TYPEOF | TT_QUALIFIER | TT_SUE)) ||
		    (before_open->len == 1 && before_open->ch0 == '*') ||
		    is_known_typedef(before_open))
			return true;
		// ']' before '[' — check if the outer ']' itself is in type context
		if (before_open->len == 1 && before_open->ch0 == ']')
			return is_array_bracket_predecessor(before_open);
		return false;
	}
	// ')' is a type predecessor only for declarator parens like int (*)[n]
	// or int (*ptr)[n], NOT for expression parens like sizeof((arr)[n]).
	// Disambiguate by checking what precedes the matching '('.
	if (t->len == 1 && t->ch0 == ')') {
		Token *open = tok_match(t);
		if (!open) return true; // no match info — conservatively assume type
		Token *before_open = walk_back_past_noise(tok_idx(open));
		if (!before_open) return true;
		// Type-producing constructs before '(' → type context (declarator)
		if (is_type_keyword(before_open) ||
		    (before_open->tag & (TT_TYPEOF | TT_QUALIFIER | TT_SUE)) ||
		    (before_open->len == 1 && before_open->ch0 == '*') ||
		    is_known_typedef(before_open))
			return true;
		// Everything else (sizeof, ident, operator, '(') → expression context
		return false;
	}
	return false;
}

// Check if array dimension contains a VLA expression (runtime variable).
static bool array_size_is_vla_impl(Token *open_bracket, int depth) {
	if (depth > 256)
		error_tok(open_bracket, "array dimension nesting depth exceeds 256");
	Token *close = tok_match(open_bracket);
	if (!close) return false;
	Token *tok = tok_next(open_bracket);

	while (tok != close) {
		if (tok->len == 1 && tok->ch0 == '[') {
			if (array_size_is_vla_impl(tok, depth + 1)) return true;
			tok = skip_balanced_group(tok);
			continue;
		}
		if (tok->len == 1 && tok->ch0 == '(' && tok_next(tok) &&
		    tok_next(tok)->len == 1 && tok_next(tok)->ch0 == '{')
			return true;
		if (tok->tag & TT_GENERIC) return true;
		SKIP_NOISE_CONTINUE(tok);

		// sizeof/alignof: skip argument but check for VLA typedef and inner VLA types.
		if (is_sizeof_like(tok)) {
			bool is_sizeof = tok->ch0 == 's';
			tok = tok_next(tok);
			if (tok != close && tok->len == 1 && tok->ch0 == '(') {
				Token *end = skip_balanced_group(tok);
				if (is_sizeof) {
					Token *prev_inner = tok;
					for (Token *inner = tok_next(tok); inner && inner != end; prev_inner = inner, inner = tok_next(inner)) {
						if (is_enum_kw(inner)) {
							Token *brace = find_struct_body_brace(inner);
							if (brace) {
								inner = skip_balanced_group(brace);
								if (inner == end) break;
								continue;
							}
						}
						int vla_fl = typedef_flags(inner) & (TDF_VLA | TDF_PARAM);
						if (vla_fl & TDF_VLA) {
							if (!(vla_fl & TDF_PARAM)) return true;
							Token *ni = tok_next(inner);
							bool has_next = ni && ni != end;
							// Look past parentheses for the real preceding/following operator
							Token *eff_prev = prev_inner;
							uint32_t pi = tok_idx(eff_prev);
							while (eff_prev->ch0 == '(' && pi > tok_idx(tok) + 1)
								eff_prev = &token_pool[--pi];
							Token *eff_next = ni;
							while (has_next && eff_next && eff_next->ch0 == ')' && eff_next != end)
								{ eff_next = tok_next(eff_next); has_next = eff_next && eff_next != end; }
							/* `+`/`-` after a decayed array *identifier* are binary
							 * pointer arithmetic — sizeof(param+5) is sizeof(void*).
							 * Unary +/- live on eff_prev before the identifier. */
							bool deref =
							    (eff_prev->len == 1 &&
							     (eff_prev->ch0 == '*' || eff_prev->ch0 == '[' ||
							      eff_prev->ch0 == '+' || eff_prev->ch0 == '-')) ||
							    (has_next && eff_next && eff_next->len == 1 &&
							     (eff_next->ch0 == '[' || eff_next->ch0 == '*' ||
							      ((eff_next->ch0 == '+' || eff_next->ch0 == '-') &&
							       !(inner->kind == TK_IDENT &&
							         (vla_fl & TDF_PARAM)))));
							if (deref) return true;
						}
						if (inner->len == 1 && inner->ch0 == '[' &&
						    is_array_bracket_predecessor(prev_inner)) {
							if (array_size_is_vla_impl(inner, depth + 1)) return true;
							inner = tok_match(inner);
							if (!inner || inner == end) break;
							continue;
						}
						if (is_valid_varname(inner) && !is_type_keyword(inner) &&
						    !is_known_typedef(inner) && !is_known_enum_const(inner) &&
						    tok_next(inner) && inner != end &&
						    tok_next(inner)->len == 1 && tok_next(inner)->ch0 == '(') {
							Token *call_end = skip_balanced_group(tok_next(inner));
							bool is_deref = (prev_inner->len == 1 && prev_inner->ch0 == '*') ||
							    (call_end && call_end != end &&
							     ((call_end->len == 1 && call_end->ch0 == '[') || (call_end->tag & TT_MEMBER)));
							if (is_deref)
								for (Token *a = tok_next(tok_next(inner)); a && a != call_end; a = tok_next(a))
									if (is_valid_varname(a) && !is_known_enum_const(a) &&
									    !is_type_keyword(a))
										return true;
							prev_inner = inner;
							inner = call_end;
							if (!inner || inner == end) break;
						}
					}
				}
				tok = end;
				if (tok != close && tok->len == 1 && tok->ch0 == '{')
					tok = skip_balanced_group(tok);
			} else if (tok != close) {
				// Unparenthesized sizeof/alignof: skip prefix ops, operand, postfix.
				while (tok != close) {
					SKIP_NOISE_CONTINUE(tok);
					if ((tok->len == 1 && (tok->ch0 == '*' || tok->ch0 == '&' || tok->ch0 == '!' ||
					    tok->ch0 == '+' || tok->ch0 == '-' || tok->ch0 == '~')) ||
					    (tok->len == 2 && (tok->ch0 == '+' || tok->ch0 == '-')) ||
					    is_sizeof_like(tok)) {
						tok = tok_next(tok);
						continue;
					}
					break;
				}
				if (tok != close) {
					if (tok->flags & TF_OPEN) {
						tok = tok_match(tok) ? tok_next(tok_match(tok)) : tok;
						if (tok != close && tok->len == 1 && tok->ch0 == '{')
							tok = tok_match(tok) ? tok_next(tok_match(tok)) : tok;
					} else {
						if (is_identifier_like(tok) && is_vla_typedef(tok)) return true;
						tok = tok_next(tok);
					}
				}
				while (tok != close) {
					if (tok->tag & TT_MEMBER) {
						tok = tok_next(tok);
						if (tok != close) tok = tok_next(tok);
					} else if (tok->flags & TF_OPEN)
						tok = tok_match(tok) ? tok_next(tok_match(tok)) : tok;
					else break;
				}
			}
			continue;
		}

		if ((tok->tag & TT_MEMBER) ||
		    (is_valid_varname(tok) && !is_known_enum_const(tok) && !is_type_keyword(tok))) return true;
		tok = tok_next(tok);
	}
	return false;
}

static inline bool array_size_is_vla(Token *open_bracket) { return array_size_is_vla_impl(open_bracket, 0); }

// Field declarator names share the member namespace — do not resolve them via
// ordinary typedef_lookup (ISO C11 §6.2.3).
static inline bool struct_body_id_is_field_name(Token *id) {
	if (!is_identifier_like(id)) return false;
	Token *nx = skip_noise(tok_next(id));
	if (!nx || nx->len != 1) return false;
	switch (nx->ch0) {
	case ';': case ',': case ':': case '[': case '=':
		return true;
	default:
		return false;
	}
}

// After struct/union/enum keyword, tag_lookup — not typedef_lookup — sees the
// tag namespace (C11 §6.2.3). Ordinary identifiers can shadow tag names.
static inline bool struct_body_field_is_vla_typedef(Token *id, Token *prev) {
	if (!is_identifier_like(id)) return false;
	if (struct_body_id_is_field_name(id)) return false;
	if (prev && (prev->tag & TT_SUE)) {
		TypedefEntry *te = tag_lookup(id);
		return te && te->is_struct_tag && te->is_vla;
	}
	return is_vla_typedef(id);
}

static bool struct_body_contains_vla(Token *brace) {
	if (!brace || !(brace->len == 1 && brace->ch0 == '{') || !tok_match(brace)) return false;
	Token *end = tok_match(brace);
	Token *prev = brace;
	for (Token *t = tok_next(brace); t && t != end; prev = t, t = tok_next(t)) {
		if (t->len == 1 && t->ch0 == '{') {
			if (struct_body_contains_vla(t)) return true;
			prev = t; t = tok_match(t); continue;
		}
		// Don't skip typeof()/_Atomic() parens — VLA dims hide inside.
		if ((t->flags & TF_OPEN) && !(t->len == 1 && t->ch0 == '[') &&
		    !(prev && ((prev->tag & TT_TYPEOF) ||
		              ((prev->tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE)))))
			{ prev = t; t = tok_match(t); continue; }
		if (t->len == 1 && t->ch0 == '[' && array_size_is_vla(t)) return true;
		if (struct_body_field_is_vla_typedef(t, prev)) return true;
	}
	return false;
}

// Scan a struct/union body for volatile-qualified fields, including nested
// struct bodies and fields whose typedef carries TDF_VOLATILE or
// TDF_HAS_VOL_MEMBER.  Used to propagate has_volatile_member to typedef
// entries and to set has_volatile in parse_type_specifier so that memset
// is replaced with a volatile-safe byte loop (memset strips volatile → UB).
static inline bool struct_body_field_volatile_member(Token *id, Token *prev) {
	if (!is_identifier_like(id)) return false;
	if (struct_body_id_is_field_name(id)) return false;
	if (prev && (prev->tag & TT_SUE)) {
		TypedefEntry *te = tag_lookup(id);
		if (te && te->has_volatile_member) return true;
	}
	return is_volatile_typedef(id) || has_volatile_member_typedef(id);
}

static bool struct_body_contains_volatile(Token *brace) {
	if (!brace || !(brace->len == 1 && brace->ch0 == '{') || !tok_match(brace)) return false;
	Token *end = tok_match(brace);
	Token *prev = brace;
	for (Token *t = tok_next(brace); t && t != end; prev = t, t = tok_next(t)) {
		if (t->len == 1 && t->ch0 == '{') {
			if (struct_body_contains_volatile(t)) return true;
			prev = t; t = tok_match(t); continue;
		}
		// Don't skip typeof()/_Atomic() parens — volatile hides inside.
		if ((t->flags & TF_OPEN) && !(t->len == 1 && t->ch0 == '{') &&
		    !(prev && ((prev->tag & TT_TYPEOF) ||
		              ((prev->tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE)))))
			{ prev = t; t = tok_match(t); continue; }
		if (t->tag & TT_VOLATILE) return true;
		if (struct_body_field_volatile_member(t, prev)) return true;
	}
	return false;
}

static bool typedef_contains_vla(Token *tok) {
	while (tok && tok->kind != TK_EOF) {
		if (tok->len == 1 && tok->ch0 == ';') break;
		if ((tok->flags & TF_OPEN) && !(tok->len == 1 && tok->ch0 == '[')) { tok = tok_next(tok_match(tok)); continue; }
		if (tok->len == 1 && tok->ch0 == '[' && array_size_is_vla(tok)) return true;
		tok = tok_next(tok);
	}
	return false;
}

// --- Find Helpers ---

static Token *find_boundary_comma(Token *tok) {
	while (tok->kind != TK_EOF) {
		if (tok->flags & TF_OPEN) { tok = tok_next(tok_match(tok)); continue; }
		if (tok->len == 1 && tok->ch0 == ';') return NULL;
		if (tok->len == 1 && tok->ch0 == ',') {
			Token *n = tok_next(tok);
			if (n) {
				if (n->len == 1 && n->ch0 == '(') {
					Token *inside = tok_next(n);
					if (inside && !(inside->tag & (TT_TYPE | TT_SUE | TT_TYPEOF | TT_QUALIFIER))
					    && !is_known_typedef(inside) && !is_c23_attr(inside))
						return tok;
				} else if ((n->len == 1 && n->ch0 == '*') || (n->tag & TT_QUALIFIER) || (n->tag & TT_ATTR) || is_c23_attr(n)) {
					return tok;
				} else if (is_valid_varname(n) && !(n->tag & (TT_TYPE | TT_SUE | TT_TYPEOF))) {
					if (tok_next(n) && tok_next(n)->len == 1 && tok_next(n)->ch0 == '(') {
						Token *inside = tok_next(tok_next(n));
						if (inside &&
						    (inside->tag & (TT_TYPE | TT_SUE | TT_TYPEOF | TT_QUALIFIER)))
							return tok;
					} else
						return tok;
				}
			}
		}
		tok = tok_next(tok);
	}
	return NULL;
}

static Token *find_init_semicolon(Token *open, Token *close) {
	int pd = 0;
	for (Token *s = tok_next(open); s && s != close; s = tok_next(s)) {
		if (s->flags & TF_OPEN) pd++;
		else if (s->flags & TF_CLOSE) pd--;
		else if (pd == 0 && s->len == 1 && s->ch0 == ';') return s;
	}
	return NULL;
}

// --- VLA Paren Scanner ---

// True if '('...')' contains only abstract-pointer tokens (*, qualifiers).
// Then a following `[n]` completes a pointer-to-array type (e.g. int (*)[n]),
// not an array-of-VLA object — must not set TypeSpecResult.is_vla / is_array.
static bool abstract_declarator_paren_is_pointer_only(Token *open_paren) {
	Token *close = tok_match(open_paren);
	if (!close || !(open_paren->len == 1 && open_paren->ch0 == '(')) return false;
	for (Token *x = tok_next(open_paren); x && x != close;) {
		x = skip_noise(x);
		if (!x || x == close) break;
		if (x->len == 1 && x->ch0 == '*') {
			x = tok_next(x);
			continue;
		}
		if (x->tag & TT_QUALIFIER) {
			x = tok_next(x);
			continue;
		}
		/* Nested abstract declarator: `(*...)` inside outer parens. */
		if (x->len == 1 && x->ch0 == '(' && (x->flags & TF_OPEN)) {
			Token *inner_close = tok_match(x);
			if (!inner_close) return false;
			if (!abstract_declarator_paren_is_pointer_only(x))
				return false;
			x = tok_next(inner_close);
			continue;
		}
		/* Concrete direct declarator `ident` + array suffixes (e.g. `*p[5]` in
		 * `int (*p[5])[10]`): `[10]` closes pointer-to-array, not another dim. */
		if (is_identifier_like(x) && x->kind == TK_IDENT) {
			x = tok_next(x);
			while (x != close && match_ch(x, '[') && (x->flags & TF_OPEN) && tok_match(x)) {
				Token *rb = tok_match(x);
				if (!rb) return false;
				x = tok_next(rb);
			}
			continue;
		}
		return false;
	}
	return true;
}

bool array_bracket_closes_ptr_to_array(Token *open_bracket, Token *prev) {
	if (!open_bracket || !prev || prev->len != 1 || prev->ch0 != ')') return false;
	Token *open = tok_match(prev);
	return open && abstract_declarator_paren_is_pointer_only(open);
}

// Scan a parenthesized type for VLA indicators and array types (typeof/_Atomic).
static void scan_paren_for_vla(Token *open, Token *end, TypeSpecResult *r, bool check_typeof) {
	Token *prev = open;
	int fn_skip = 0;
	for (Token *t = tok_next(open); t && t != end; prev = t, t = tok_next(t)) {
		// Ban control-flow keywords inside type specifier parens.
		// typeof()/Atomic() are unevaluated at runtime, but Prism's
		// emit_type_range routes stmt-exprs through walk_balanced (full
		// transpilation engine).  Type specifiers can be emitted multiple
		// times (const orelse temp, multi-declarator splits), so any
		// defer/goto/return would be processed N times at compile time,
		// corrupting the defer stack or goto_entry_cursor.
		if (t->tag & (TT_GOTO | TT_RETURN | TT_BREAK | TT_CONTINUE | TT_DEFER)) {
			if (FEAT(F_WARN_SAFETY))
				warn_tok(t, "control flow keywords inside type "
					 "specifiers (typeof() / _Atomic()) may "
					 "corrupt control-flow tracking");
			else
				error_tok(t, "control flow keywords are not "
					  "allowed inside type specifiers "
					  "(typeof() / _Atomic()); transpiler "
					  "rewrites may duplicate the type "
					  "specifier, which would corrupt "
					  "control-flow tracking");
		}
		if (check_typeof && (t->tag & TT_TYPEOF)) r->has_typeof = true;
		if (t->len == 1 && t->ch0 == '(') {
			if (fn_skip > 0) fn_skip++;
			else if (prev->len == 1 && prev->ch0 == ')') fn_skip = 1;
		} else if (t->len == 1 && t->ch0 == ')') { if (fn_skip > 0) fn_skip--; }
		if (fn_skip > 0) continue;
		if (t->len == 1 && t->ch0 == '[' && is_array_bracket_predecessor(prev)) {
			if (array_size_is_vla(t))
				r->type_vm = true;
			if (!array_bracket_closes_ptr_to_array(t, prev)) {
				r->is_array = true;
				if (r->type_array_rank < 15) r->type_array_rank++;
				if (array_size_is_vla(t)) r->is_vla = true;
			}
			continue;
		}
		if (is_identifier_like(t) && (typedef_flags(t) & TDF_VLA)) {
			r->is_vla = true;
			break;
		}
	}
}

// --- Type Specifier Parser ---

#define SKIP_RAW(after, last) do { \
	while ((after) && ((after)->flags & TF_RAW) && !is_known_typedef(after)) { \
		(last) = (after); (after) = skip_noise(tok_next(after)); \
	} \
} while (0)

static TypeSpecResult parse_type_specifier(Token *tok) {
	TypeSpecResult r = { .end = tok };

	while (tok && tok->kind != TK_EOF) {
		Token *next = skip_noise(tok);
		if (next != tok) { tok = next; r.end = tok; continue; }

		if (!r.saw_type && (tok->flags & TF_RAW) && !is_known_typedef(tok)) {
			r.has_raw = true;
			Token *after = skip_noise(tok_next(tok));
			Token *last = tok;
			SKIP_RAW(after, last);
			tok = tok_next(last);
			r.end = tok;
			continue;
		}

		uint32_t tag = tok->tag;
		bool is_type = is_type_keyword(tok);
		if (!(tag & (TT_QUALIFIER | TT_STORAGE | TT_INLINE)) && !is_type && !(tag & (TT_BITINT | TT_ALIGNAS))) break;

		// Skip inline/_Noreturn/__extension__ — valid prefix, no type info
		if ((tag & TT_INLINE) && !(tag & (TT_QUALIFIER | TT_STORAGE))) {
			tok = tok_next(tok); r.end = tok; continue;
		}

		if (equal(tok, "void") || is_void_typedef(tok)) r.has_void = true;

		bool had_type = r.saw_type;

		if ((tag & TT_STORAGE) && tok->ch0 == 'e') r.has_extern = true;
		if ((tag & TT_STORAGE) && tok->ch0 == 's') r.has_static = true;
		if ((tag & TT_STORAGE) && tok->ch0 != 'e' && tok->ch0 != 's') r.has_thread_local = true;

		if (tag & TT_QUALIFIER) {
			if (tag & TT_VOLATILE) r.has_volatile = true;
			if (tag & TT_REGISTER) r.has_register = true;
			if (tag & TT_CONST) r.has_const = true;
			if (tok->ch0 == 'c' && tok->len == 9) r.has_constexpr = true;
			if (tag & TT_TYPE) {
				if (tok->ch0 == 'a') { r.saw_type = true; r.has_auto = true; }
				else r.has_atomic = true;
			}
		}

		if (is_type && (tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE)
		    && !(tok_next(tok) && tok_next(tok)->len == 1 && tok_next(tok)->ch0 == '('))
			is_type = false;
		if (is_type) r.saw_type = true;
		is_type = false;

		// _Atomic(type) specifier form
		if ((tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE) && tok_next(tok) &&
		    tok_next(tok)->len == 1 && tok_next(tok)->ch0 == '(') {
			r.saw_type = true;
			r.has_atomic = true;
			tok = tok_next(tok);
			Token *inner_start = skip_noise(tok_next(tok));
			Token *end = skip_balanced_group(tok);
			if (inner_start && (inner_start->tag & TT_SUE)) {
				r.is_struct = true;
				if (inner_start->ch0 == 'u') r.is_union = true;
			}
			if (inner_start && is_identifier_like(inner_start) && is_known_typedef(inner_start)) {
				r.is_typedef = true;
				if (typedef_flags(inner_start) & TDF_UNION) r.is_union = true;
			}
			scan_paren_for_vla(tok, end, &r, true);
			tok = end;
			r.end = tok;
			continue;
		}

		// struct/union/enum
		if (tag & TT_SUE) {
			r.is_struct = true;
			if (tok->ch0 == 'u') r.is_union = true;
			if (tok->ch0 == 'e') r.is_enum = true;
			r.saw_type = true;
			tok = tok_next(tok);
			while (tok && tok->kind != TK_EOF) {
				SKIP_NOISE_CONTINUE(tok);
				if (tok->tag & TT_QUALIFIER) tok = tok_next(tok);
				else break;
			}
			Token *sue_tag = NULL;
			if (tok && is_valid_varname(tok)) { sue_tag = tok; tok = tok_next(tok); }
			// C23 enum fixed underlying type: enum E : int { ... }
			if (tok && tok->len == 1 && tok->ch0 == ':') {
				tok = tok_next(tok);
				while (tok && tok->kind != TK_EOF) {
					SKIP_NOISE_CONTINUE(tok);
					if (is_type_keyword(tok) || (tok->tag & TT_QUALIFIER)) tok = tok_next(tok);
					else break;
				}
			}
			if (tok && tok->len == 1 && tok->ch0 == '{') {
				if (struct_body_contains_vla(tok)) r.is_vla = true;
				if (struct_body_contains_volatile(tok)) r.has_volatile_member = true;
				tok = skip_balanced_group(tok);
			} else if (sue_tag) {
				TypedefEntry *tag_e = tag_lookup(sue_tag);
				if (tag_e) {
					if (tag_e->is_vla) r.is_vla = true;
					if (tag_e->has_volatile_member) r.has_volatile_member = true;
				}
			}
			r.end = tok;
			continue;
		}

		// typeof/typeof_unqual/__typeof__/__typeof_unqual__
		if (tag & TT_TYPEOF) {
			bool is_unqual = tok->len >= 13; // typeof_unqual(13), __typeof_unqual(15), __typeof_unqual__(17)
			r.saw_type = true;
			r.has_typeof = true;
			tok = tok_next(tok);
			if (tok && tok->len == 1 && tok->ch0 == '(') {
				Token *end = skip_balanced_group(tok);
				if (tok_next(tok) && equal(tok_next(tok), "void") && tok_next(tok_next(tok)) == tok_match(tok)) r.has_void = true;
				{
					bool saw_sue = false;
					for (Token *t = tok_next(tok); t && t != end; t = tok_next(t)) {
						// Skip attribute groups — they contain
						// identifier-like tokens (e.g. 'packed')
						// that would consume saw_sue.
						if ((t->tag & TT_ATTR) && tok_next(t) &&
						    match_ch(tok_next(t), '(') && tok_match(tok_next(t))) {
							t = tok_match(tok_next(t));
							continue;
						}
						if (is_c23_attr(t) && tok_match(t)) {
							t = tok_match(t);
							continue;
						}
						if (!is_unqual) {
							if (t->tag & TT_VOLATILE) r.has_volatile = true;
							if (t->tag & TT_CONST) r.has_const = true;
							if ((t->tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE))
								r.has_atomic = true;
						}
						if ((t->tag & TT_SUE) || (typedef_flags(t) & TDF_AGGREGATE)) r.is_struct = true;
						if ((t->tag & TT_SUE) && t->ch0 == 'u') r.is_union = true;
						if (typedef_flags(t) & TDF_UNION) r.is_union = true;
						if (t->tag & TT_SUE) { saw_sue = true; continue; }
						if (is_identifier_like(t)) {
							// After struct/union keyword, use tag_lookup for
							// ISO C11 §6.2.3 namespace separation.
							if (saw_sue) {
								TypedefEntry *tag_e = tag_lookup(t);
								if (tag_e) {
									if (tag_e->is_vla) r.is_vla = true;
									if (tag_e->has_volatile_member) r.has_volatile_member = true;
								}
								saw_sue = false;
							}
							if (!is_unqual) {
								int tf = typedef_flags(t);
								if (tf & TDF_VOLATILE) r.has_volatile = true;
								if (tf & TDF_HAS_VOL_MEMBER) r.has_volatile_member = true;
							}
						}
					}
				}
				scan_paren_for_vla(tok, end, &r, false);
				tok = end;
			}
			r.end = tok;
			continue;
		}

		if (tag & (TT_BITINT | TT_ATTR | TT_ALIGNAS)) {
			if (tag & TT_BITINT) r.saw_type = true;
			Token *kw = tok;
			tok = tok_next(tok);
			if (tok && tok->len == 1 && tok->ch0 == '(') {
				if (FEAT(F_ORELSE) && (kw->tag & (TT_BITINT | TT_ALIGNAS))) {
					Token *close = tok_match(tok);
					for (Token *s = tok_next(tok); s && s != close; s = tok_next(s))
						if (is_orelse_kw_shadow(s))
							error_tok(s, "'orelse' cannot be used inside %s "
								  "(requires a compile-time constant expression)",
								  (kw->tag & TT_BITINT) ? "_BitInt()" : "_Alignas()");
				}
				tok = skip_balanced_group(tok);
			}
			r.end = tok;
			continue;
		}

		int tflags = typedef_flags(tok);
		if (tflags & TDF_TYPEDEF) {
			if (had_type) break;
			r.is_typedef = true;
			if (tflags & TDF_VLA) r.is_vla = true;
			if (tflags & TDF_AGGREGATE) r.is_struct = true;
			if (tflags & TDF_UNION) r.is_union = true;
			if (tflags & TDF_HAS_VOL_MEMBER) r.has_volatile_member = true;
			Token *peek = tok_next(tok);
			while (peek && (peek->tag & TT_QUALIFIER)) peek = tok_next(peek);
			if (peek && is_valid_varname(peek)) {
				Token *after = tok_next(peek);
				if (after && (after->len == 1 && (after->ch0 == ';' || after->ch0 == '[' || after->ch0 == ',' || after->ch0 == '='))) {
					tok = tok_next(tok);
					r.end = tok;
					r.saw_type = true;
					return r;
				}
			}
		}

		tok = tok_next(tok);
		r.end = tok;
	}

	return r;
}

// --- Typedef Declaration Parser ---

static void parse_typedef_declaration(Token *tok, int scope_depth) {
	Token *typedef_start = tok;
	tok = tok_next(tok); // Skip 'typedef'
	Token *type_start = tok;
	TypeSpecResult type_spec = parse_type_specifier(tok);
	tok = type_spec.end;

	bool is_vla = type_spec.is_vla || typedef_contains_vla(typedef_start);

	bool base_is_const = type_spec.has_const;
	if (!base_is_const) {
		for (Token *t = type_start; t && t != tok; t = tok_next(t))
			if (is_const_typedef(t)) { base_is_const = true; break; }
	}

	bool base_is_volatile = type_spec.has_volatile;
	if (!base_is_volatile) {
		for (Token *t = type_start; t && t != tok; t = tok_next(t))
			if (is_volatile_typedef(t)) { base_is_volatile = true; break; }
	}

	bool base_has_volatile_member = type_spec.has_volatile_member;
	if (!base_has_volatile_member) {
		for (Token *t = type_start; t && t != tok; t = tok_next(t))
			if (has_volatile_member_typedef(t)) { base_has_volatile_member = true; break; }
	}

	bool base_is_void = type_spec.has_void;
	bool base_is_ptr = false;
	bool base_is_array = false;
	bool base_is_func = false;
	uint8_t base_array_rank = 0;
	for (Token *bt = type_start; bt && bt != type_spec.end; bt = tok_next(bt)) {
		if (is_ptr_typedef(bt)) { base_is_ptr = true; break; }
		if (is_array_typedef(bt)) {
			base_is_array = true;
			// Inherit rank from the resolved typedef so chained
			// multi-dim typedefs (typedef int T[3][4]; typedef T U;
			// U a;) don't lose their inner dimensions for
			// bounds-check wrapping.
			TypedefEntry *te = typedef_lookup(bt);
			if (te) base_array_rank = te->array_rank;
			break;
		}
		if (is_func_typedef(bt)) { base_is_func = true; break; }
	}

	// Register struct/union tag unconditionally so inner-scope
	// redefinitions correctly shadow outer tags (C11 §6.2.1p4).
	// Without this, a clean inner "typedef struct T { int y; } T_t"
	// fails to register, and tag_lookup finds the outer VLA/volatile
	// tag, causing false CFG verifier errors or incorrect memset.
	if (type_spec.is_struct) {
		for (Token *bt = type_start; bt && bt != type_spec.end; bt = tok_next(bt)) {
			if (bt->tag & TT_SUE) {
				Token *tag = skip_noise(tok_next(bt));
				// Skip qualifiers after struct/union keyword
				while (tag && (tag->tag & TT_QUALIFIER)) tag = skip_noise(tok_next(tag));
				if (tag && is_valid_varname(tag)) {
					int pre = typedef_table.count;
					typedef_add_entry(tok_loc(tag), tag->len, scope_depth, TDK_STRUCT_TAG, is_vla, false);
					if (typedef_table.count > pre) {
						TypedefEntry *te = &typedef_table.entries[typedef_table.count - 1];
						te->token_index = tok_idx(tag);
						te->is_aggregate = !type_spec.is_enum;
						if (base_has_volatile_member) te->has_volatile_member = true;
					}
				}
				break;
			}
		}
	}

	while (tok && !(tok->len == 1 && tok->ch0 == ';') && tok->kind != TK_EOF) {
		DeclResult decl = parse_declarator(tok, false);
		if (decl.var_name) {
			bool is_void =
			    base_is_void && !decl.is_pointer && !decl.is_array && !decl.is_func_ptr;
			bool is_const = (decl.is_pointer || decl.is_func_ptr)
			    ? decl.is_const : base_is_const;
			bool is_ptr =
			    decl.is_pointer || decl.is_func_ptr || base_is_ptr;
			int pre_count = typedef_table.count;
			typedef_add_entry(tok_loc(decl.var_name), decl.var_name->len, scope_depth, TDK_TYPEDEF, is_vla, is_void);
			if (typedef_table.count > pre_count) {
				TypedefEntry *added = &typedef_table.entries[typedef_table.count - 1];
				added->token_index = tok_idx(decl.var_name);
				if (is_const) added->is_const = true;
				bool is_vol = (decl.is_pointer || decl.is_func_ptr)
				    ? false : base_is_volatile;
				if (is_vol) added->is_volatile = true;
				if (base_has_volatile_member && !decl.is_pointer && !decl.is_func_ptr)
					added->has_volatile_member = true;
				if (is_ptr) added->is_ptr = true;
				if ((decl.is_array || base_is_array) && !decl.is_pointer && !decl.is_func_ptr)
					added->is_array = true;
				// array_rank: count declarator `[` and add the
				// base typedef's inherited rank (if any), so
				// `typedef int T[3][4]; T a;` records rank=2 and
				// `typedef int T[5]; T m[3];` records rank=2.
				// Kept as conservative sum — does not attempt to
				// detect declarator-level paren-wrapped groups
				// or func-of-array constructs.
				if ((decl.is_array || base_is_array) && !decl.is_pointer && !decl.is_func_ptr) {
					int rank = 0;
					Token *prev_dt = NULL;
					for (Token *dt = decl.var_name; dt && decl.end && dt != decl.end;) {
						if (dt->len == 1 && dt->ch0 == '[' && (dt->flags & TF_OPEN)) {
							if (!array_bracket_closes_ptr_to_array(dt, prev_dt))
								rank++;
							Token *m = tok_match(dt);
							dt = m ? tok_next(m) : tok_next(dt);
							prev_dt = m;
							continue;
						}
						prev_dt = dt;
						dt = tok_next(dt);
					}
					rank += (int)base_array_rank;
					if (rank < 1) rank = 1;
					if (rank > 15)
						rank = ARRAY_RANK_WRAP_ALL;
					added->array_rank = (uint8_t)rank;
				}
				if ((decl.is_array || base_is_array) && !decl.is_pointer && !decl.is_func_ptr) {
					bool dim_complete = false;
					if (decl.is_array) {
						for (Token *dt = decl.var_name; dt && decl.end && dt != decl.end;
						     dt = tok_next(dt)) {
							if (match_ch(dt, '[')) {
								Token *nx = tok_next(dt);
								if (nx && !match_ch(nx, ']'))
									dim_complete = true;
								break;
							}
						}
						if (!dim_complete && decl.end && match_ch(decl.end, '='))
							dim_complete = true;
					}
					if (!dim_complete && base_is_array) {
						for (Token *bt = type_start; bt && bt != type_spec.end;
						     bt = tok_next(bt)) {
							if (is_array_typedef(bt)) {
								TypedefEntry *bte = typedef_lookup(bt);
								if (bte && bte->is_array)
									dim_complete = bte->array_dim_complete;
								break;
							}
						}
					}
					added->array_dim_complete = dim_complete;
				}
				if (type_spec.is_struct && !type_spec.is_enum && !decl.is_pointer && !decl.is_func_ptr)
					added->is_aggregate = true;
				if (type_spec.is_union && !decl.is_pointer && !decl.is_func_ptr)
					added->is_union = true;
				if (!decl.end) {
					Token *after_name = skip_noise(tok_next(decl.var_name));
					if (after_name && after_name->len == 1 && after_name->ch0 == '(')
						added->is_func = true;
				}
				if (decl.is_func_ptr && !decl.paren_pointer)
					added->is_func = true;
				if (base_is_func && !decl.is_pointer && !decl.is_array && !decl.is_func_ptr)
					added->is_func = true;
			}
		}
		tok = decl.end ? decl.end : tok_next(tok);

		while (tok && !(tok->len == 1 && tok->ch0 == ',') && !(tok->len == 1 && tok->ch0 == ';') && tok->kind != TK_EOF) {
			if (tok->len == 1 && tok->ch0 == '(') tok = skip_balanced_group(tok);
			else if (tok->len == 1 && tok->ch0 == '[') tok = skip_balanced_group(tok);
			else tok = tok_next(tok);
		}

		if (tok && tok->len == 1 && tok->ch0 == ',') tok = tok_next(tok);
	}
}

// --- skip_one_stmt ---

// Limits for skip_one_stmt_impl stack arrays.
// if_depth beyond SOS_IF_MAX skips recording trail snapshots; unwind must not
// treat a missing snapshot as 0 (that would flush the skip-cache).
// do_depth is hard-capped at SOS_DO_MAX (gives up on pathological input).
#define SOS_IF_MAX    512
#define SOS_DO_MAX    128
#define SOS_SNAP_MAX  1024

static Token *skip_one_stmt_impl(Token *tok, uint32_t *cache) {
	int if_depth = 0;
	int do_depth = 0;
	int do_if_save[SOS_DO_MAX];
	int do_tn_save[SOS_DO_MAX];
	int do_snap_buf[SOS_SNAP_MAX];  // flat buffer saving if_trail_snap per do level
	int do_snap_start[SOS_DO_MAX];
	int do_snap_top = 0;
	uint32_t trail[256];
	int tn = 0;
	int if_trail_snap[SOS_IF_MAX]; // trail length snapshot at each if_depth entry
restart:
	tok = skip_prep_dirs(tok);
	tok = skip_noise(tok);
	if (!tok || tok->kind == TK_EOF) return NULL;
	if (cache) {
		uint32_t idx = tok_idx(tok);
		if (cache[idx]) {
			Token *r = &token_pool[cache[idx] - 1];
			for (int i = 0; i < tn; i++) {
				uint32_t tix = trail[i];
				cache[tix] = cache[idx];
			}
			return r;
		}
		if (tn < 256) trail[tn++] = idx;
	}

	if (tok->len == 1 && tok->ch0 == '{') { tok = tok_match(tok); goto unwind_if; }

	if (tok->tag & TT_IF) {
		if (tok->ch0 == 'e') { tok = tok_next(tok); goto restart; }
		Token *p = skip_prep_dirs(tok_next(tok));
		if (!p || !(p->len == 1 && p->ch0 == '(') || !tok_match(p)) return NULL;
		if (if_depth < SOS_IF_MAX) if_trail_snap[if_depth] = tn;
		if_depth++;
		tok = tok_next(tok_match(p)); goto restart;
	}

	if ((tok->tag & (TT_LOOP | TT_SWITCH)) && tok->ch0 != 'd') {
		Token *p = skip_prep_dirs(tok_next(tok));
		if (!p || !(p->len == 1 && p->ch0 == '(') || !tok_match(p)) return NULL;
		tok = tok_next(tok_match(p)); goto restart;
	}

	if ((tok->tag & TT_LOOP) && tok->ch0 == 'd') {
		if (do_depth >= SOS_DO_MAX) return NULL;
		if (do_snap_top + if_depth > SOS_SNAP_MAX)
			error_tok(tok,
				  "statement scan: nested do/if trail snapshot limit "
				  "exceeded; reduce nested 'do' and 'if' depth in a "
				  "single statement");
		do_if_save[do_depth] = if_depth;
		do_tn_save[do_depth] = tn;
		do_snap_start[do_depth] = do_snap_top;
		for (int i = 0; i < if_depth && i < SOS_IF_MAX; i++)
			do_snap_buf[do_snap_top++] = if_trail_snap[i];
		do_depth++;
		if_depth = 0;
		tok = tok_next(tok); goto restart;
	}

	if (is_identifier_like(tok) && !(tok->tag & (TT_CASE | TT_DEFAULT | TT_TYPE | TT_QUALIFIER | TT_STORAGE))) {
		Token *colon = skip_noise(tok_next(tok));
		if (colon && colon->len == 1 && colon->ch0 == ':' && !(tok_next(colon) && tok_next(colon)->len == 1 && tok_next(colon)->ch0 == ':')) {
			tok = tok_next(colon); goto restart;
		}
	}

	if ((tok->tag & (TT_CASE | TT_DEFAULT)) && !is_known_typedef(tok)) {
		int td = 0;
		for (Token *s = tok_next(tok); s && s->kind != TK_EOF; s = tok_next(s)) {
			if (s->flags & TF_OPEN) { s = tok_match(s) ? tok_match(s) : s; continue; }
			if (s->len == 1 && s->ch0 == '?') { td++; continue; }
			if (s->len == 1 && s->ch0 == ':') {
				if (td > 0) { td--; continue; }
				tok = tok_next(s); goto restart;
			}
		}
		return NULL;
	}

	for (Token *s = tok; s && s->kind != TK_EOF; s = tok_next(s)) {
		if (s->flags & TF_OPEN) { s = tok_match(s) ? tok_match(s) : s; continue; }
		if (s->len == 1 && s->ch0 == ';') { tok = s; goto unwind_if; }
	}
	return NULL;

unwind_if:
	while (if_depth > 0) {
		if_depth--;
		if (!tok) return NULL;
		Token *n = skip_prep_dirs(tok_next(tok));
		if (n && (n->tag & TT_IF) && n->ch0 == 'e') {
			// Only flush trail entries from THIS if level (true-branch),
			// not parent tokens that span the entire if-else.
			int snap = (if_depth < SOS_IF_MAX) ? if_trail_snap[if_depth] : tn;
			if (cache && tok) {
				uint32_t val = tok_idx(tok) + 1;
				for (int i = snap; i < tn; i++) {
					uint32_t tix = trail[i];
					cache[tix] = val;
				}
			}
			tn = snap; // keep parent tokens in trail for final resolution
			tok = tok_next(n); goto restart;
		}
	}
	if (cache && tok) {
		uint32_t val = tok_idx(tok) + 1;
		for (int i = 0; i < tn; i++) {
			uint32_t tix = trail[i];
			cache[tix] = val;
		}
	}
	if (do_depth > 0) {
		do_depth--;
		if_depth = do_if_save[do_depth];
		tn = do_tn_save[do_depth];
		int snap_start = do_snap_start[do_depth];
		int snap_count = do_snap_top - snap_start;
		for (int i = 0; i < snap_count; i++)
			if_trail_snap[i] = do_snap_buf[snap_start + i];
		do_snap_top = snap_start;
		if (!tok) goto unwind_if;
		Token *w = skip_prep_dirs(tok_next(tok));
		if (!w || !(w->tag & TT_LOOP) || w->ch0 != 'w') { tok = NULL; goto unwind_if; }
		Token *p2 = skip_prep_dirs(tok_next(w));
		if (!p2 || !(p2->len == 1 && p2->ch0 == '(') || !tok_match(p2)) { tok = NULL; goto unwind_if; }
		Token *a = skip_prep_dirs(tok_next(tok_match(p2)));
		tok = (a && a->len == 1 && a->ch0 == ';') ? a : NULL;
		goto unwind_if;
	}
	return tok;
}
static Token *skip_one_stmt(Token *tok) { return skip_one_stmt_impl(tok, NULL); }

// --- Scope Tree ---

// Scope tree: flat array indexed by scope_id, one entry per '{' in the TU.
typedef struct {
	uint16_t parent_id;    // scope_id of enclosing '{' (0 = file scope)
	uint32_t open_tok_idx; // token index of the '{' (0 for file scope)
	uint32_t close_tok_idx;// token index of the matching '}' (UINT32_MAX for file scope)
	bool is_struct : 1;
	bool is_loop : 1;
	bool is_switch : 1;
	bool is_func_body : 1;
	bool is_stmt_expr : 1;
	bool is_conditional : 1;
	bool is_init : 1;     // initializer brace: = { ... } — not a compound statement
	bool is_enum : 1;     // set when is_struct=true and the keyword is 'enum'
} ScopeInfo;

#define scope_tree       ((ScopeInfo *)ctx->p1_scope_tree)
#define scope_tree_count (ctx->p1_scope_count)
#define scope_tree_cap   (ctx->p1_scope_cap)

static bool scope_is_ancestor_or_self(uint16_t ancestor, uint16_t descendant) {
	for (uint16_t s = descendant; s != 0; s = scope_tree[s].parent_id)
		if (s == ancestor) return true;
	return ancestor == 0; // file scope is ancestor of everything
}

static int scope_tree_depth(uint16_t scope_id) {
	int depth = 0;
	for (uint16_t s = scope_id; s != 0; s = scope_tree[s].parent_id)
		depth++;
	return depth;
}

static int scope_block_exits(uint16_t goto_sid, uint16_t label_sid) {
	uint16_t a = goto_sid, b = label_sid;
	int da = scope_tree_depth(a), db = scope_tree_depth(b);
	while (da > db) { a = scope_tree[a].parent_id; da--; }
	while (db > da) { b = scope_tree[b].parent_id; db--; }
	while (a != b && a != 0) { a = scope_tree[a].parent_id; b = scope_tree[b].parent_id; }
	uint16_t lca = a;
	int exits = 0;
	for (uint16_t s = goto_sid; s != lca && s != 0; s = scope_tree[s].parent_id)
		if (!scope_tree[s].is_init)
			exits++;
	return exits;
}

static uint16_t scope_stmt_expr_ancestor(uint16_t scope_id) {
	for (uint16_t s = scope_id; s != 0; s = scope_tree[s].parent_id)
		if (s < scope_tree_count && scope_tree[s].is_stmt_expr) return s;
	return 0;
}

// Phase 1: check if a defer in scope 'sid' is inside a chain of closing braces
// leading to a statement expression.
static void p1_check_defer_stmt_expr_chain(Token *defer_tok, uint16_t sid) {
	while (sid > 0 && sid < scope_tree_count) {
		uint16_t pid = scope_tree[sid].parent_id;
		if (pid == 0 || pid >= scope_tree_count) break;
		Token *t = tok_next(&token_pool[scope_tree[sid].close_tok_idx]);
		Token *parent_close = &token_pool[scope_tree[pid].close_tok_idx];
		bool only_trivial = true;
		while (t && t != parent_close && t->kind != TK_EOF) {
			if (match_ch(t, ';') || match_ch(t, '}')) { t = tok_next(t); continue; }
			if (t->kind == TK_PREP_DIR) { t = tok_next(t); continue; }
			if (t->tag & TT_ATTR) {
				t = tok_next(t);
				if (t && match_ch(t, '(')) t = tok_match(t) ? tok_next(tok_match(t)) : tok_next(t);
				continue;
			}
			if (is_c23_attr(t)) {
				t = tok_match(t) ? tok_next(tok_match(t)) : tok_next(t);
				continue;
			}
			if ((t->kind == TK_IDENT || t->kind == TK_KEYWORD) &&
			    tok_next(t) && match_ch(tok_next(t), ':'))
				{ t = tok_next(tok_next(t)); continue; }
			only_trivial = false;
			break;
		}
		if (!only_trivial) break;
		if (scope_tree[pid].is_stmt_expr)
			error_tok(defer_tok,
				  "defer inside a block that is the last "
				  "statement of a statement expression "
				  "would corrupt the expression's return "
				  "value; ensure the last statement of the "
				  "statement expression is outside the "
				  "defer block");
		sid = pid;
	}
}

// Walk backward from before_idx skipping prep dirs, GNU attrs, C23 [[attrs]].
static Token *p1_find_prev_skipping_attrs(uint32_t before_idx) {
	for (uint32_t pi = before_idx; pi > 0; pi--) {
		Token *pt = &token_pool[pi];
		if (pt->kind == TK_PREP_DIR) continue;
		if (match_ch(pt, ']') && tok_match(pt) && (tok_match(pt)->flags & TF_C23_ATTR)) { pi = tok_idx(tok_match(pt)); continue; }
		if (match_ch(pt, ')') && tok_match(pt)) {
			Token *open = tok_match(pt);
			uint32_t oi = tok_idx(open);
			for (uint32_t bi = oi - 1; bi > 0; bi--) {
				Token *bt = &token_pool[bi];
				if (bt->kind == TK_PREP_DIR) continue;
				if (bt->tag & TT_ATTR) { pi = bi; goto next_pi; }
				break;
			}
		}
		return pt;
		next_pi:;
	}
	return NULL;
}

// --- Defer Validation (Phase 1F) ---

static void defer_scan_hidden_stmt_exprs(Token *open, bool in_loop, bool in_switch, int depth);

static inline Token *skip_defer_control_head(Token *tok, bool in_loop, bool in_switch, int depth) {
	tok = skip_noise(tok);
	if (tok && match_ch(tok, '(') && tok_match(tok)) {
		defer_scan_hidden_stmt_exprs(tok, in_loop, in_switch, depth);
		return tok_next(tok_match(tok));
	}
	return tok;
}

static void validate_defer_control_flow(Token *t, bool in_loop, bool in_switch) {
	if (!t) return;
	if (t->tag & TT_RETURN)
		error_tok(t, "'return' inside defer block bypasses remaining defers");
	if ((t->tag & TT_GOTO) && !is_known_typedef(t))
		error_tok(t, "'goto' inside defer block could bypass remaining defers");
	if ((t->tag & TT_BREAK) && !in_loop && !in_switch)
		error_tok(t, "'break' inside defer block bypasses remaining defers");
	if ((t->tag & TT_CONTINUE) && !in_loop)
		error_tok(t, "'continue' inside defer block bypasses remaining defers");
}

static Token *validate_defer_statement(Token *tok, bool in_loop, bool in_switch, int depth);

// Recursively scan a balanced (...) or [...] group for orelse keywords
// with control-flow actions.  Required because Pass 2's scan_decl_orelse
// strips parens from initializers like `int x = (0 orelse return);`,
// making the orelse action reachable even though Phase 1F would normally
// skip the balanced group.
static void defer_scan_orelse_in_group(Token *open, bool in_loop, bool in_switch, int depth) {
	Token *end = tok_match(open);
	if (!end) return;
	Token *prev = open;
	for (Token *s = tok_next(open); s && s != end && s->kind != TK_EOF; s = tok_next(s)) {
		if (s->flags & TF_OPEN) {
			if (match_ch(s, '(') || match_ch(s, '['))
				defer_scan_orelse_in_group(s, in_loop, in_switch, depth);
			prev = tok_match(s) ? tok_match(s) : s;
			s = prev;
			continue;
		}
		if (is_orelse_kw_shadow(s) && orelse_shadow_is_kw(prev)) {
			Token *act = tok_next(s);
			if (act && match_ch(act, ';'))
				error_tok(s, "expected statement after 'orelse'");
			validate_defer_control_flow(act, in_loop, in_switch);
			if (act && match_ch(act, '{')) {
				validate_defer_statement(act, in_loop, in_switch, depth + 1);
				Token *close = tok_match(act);
				if (close) { prev = close; s = close; }
			} else {
				prev = act ? act : s;
				s = prev;
			}
			continue;
		}
		prev = s;
	}
}

static void defer_scan_hidden_stmt_exprs(Token *open, bool in_loop, bool in_switch, int depth) {
	Token *end = tok_match(open);
	if (!end) return;
	for (Token *t = tok_next(open); t && t != end && t->kind != TK_EOF; ) {
		if (is_stmt_expr_open(t)) {
			validate_defer_statement(tok_next(t), in_loop, in_switch, depth + 1);
			t = tok_match(t) ? tok_next(tok_match(t)) : tok_next(t);
		} else t = tok_next(t);
	}
}

static Token *validate_defer_statement(Token *tok, bool in_loop, bool in_switch, int depth) {
	if (depth >= 4096) error_tok(tok, "braceless control flow nesting depth exceeds 4096");
	tok = skip_noise(tok);
	if (!tok || tok->kind == TK_EOF) return tok;

	if (match_ch(tok, '{')) {
		Token *end = tok_match(tok);
		uint32_t end_idx = end ? tok_idx(end) : UINT32_MAX;
		for (tok = skip_noise(tok_next(tok)); tok && tok != end && tok->kind != TK_EOF && tok_idx(tok) < end_idx; tok = skip_noise(tok)) {
			Token *next = validate_defer_statement(tok, in_loop, in_switch, depth);
			if (next == tok) break;
			tok = next;
		}
		return end ? tok_next(end) : tok;
	}

	if ((tok->tag & TT_IF) && tok->ch0 == 'i') {
		Token *after_then = validate_defer_statement(skip_defer_control_head(tok_next(tok), in_loop, in_switch, depth), in_loop, in_switch, depth + 1);
		Token *else_tok = skip_noise(after_then);
		if (else_tok && (else_tok->tag & TT_IF) && else_tok->ch0 == 'e')
			return validate_defer_statement(tok_next(else_tok), in_loop, in_switch, depth + 1);
		return after_then;
	}

	if (tok->tag & (TT_CASE | TT_DEFAULT)) {
		int td = 0;
		for (tok = tok_next(tok); tok && tok->kind != TK_EOF; tok = tok_next(tok)) {
			if ((tok->flags & TF_CLOSE) && tok->ch0 == '}') break;
			if (tok->flags & TF_OPEN) {
				if (match_ch(tok, '(') || match_ch(tok, '['))
					defer_scan_hidden_stmt_exprs(tok, in_loop, in_switch, depth);
				tok = tok_match(tok) ? tok_match(tok) : tok;
				continue;
			}
			if (match_ch(tok, '?')) { td++; continue; }
			if (match_ch(tok, ':')) { if (td > 0) { td--; continue; } break; }
		}
		return tok && match_ch(tok, ':') ? validate_defer_statement(tok_next(tok), in_loop, in_switch, depth + 1) : tok;
	}

	if (tok->tag & TT_SWITCH)
		return validate_defer_statement(skip_defer_control_head(tok_next(tok), in_loop, true, depth), in_loop, true, depth + 1);

	if (tok->tag & TT_LOOP) {
		if (tok->ch0 == 'd') {
			tok = validate_defer_statement(tok_next(tok), true, in_switch, depth + 1);
			Token *w = skip_noise(tok);
			if (w && (w->tag & TT_LOOP) && w->ch0 == 'w') {
				tok = skip_defer_control_head(tok_next(w), true, in_switch, depth);
				tok = skip_noise(tok);
				if (tok && match_ch(tok, ';')) tok = tok_next(tok);
			}
			return tok;
		}
		return validate_defer_statement(skip_defer_control_head(tok_next(tok), true, in_switch, depth), true, in_switch, depth + 1);
	}

	if (tok->flags & TF_OPEN) {
		if (is_stmt_expr_open(tok)) {
			Token *inner_brace = tok_next(tok);
			validate_defer_statement(inner_brace, in_loop, in_switch, depth + 1);
			return tok_match(tok) ? tok_next(tok_match(tok)) : tok_next(tok);
		}
	}

	if (tok->kind == TK_IDENT && tok_next(tok) && match_ch(tok_next(tok), ':'))
		error_tok(tok, "labels inside defer blocks produce duplicate labels "
			  "when the defer body is copied to multiple exit points");

	// Storage classes that create persistent per-scope state (static,
	// thread_local/_Thread_local/__thread) would be DUPLICATED when the
	// defer body is copied to multiple exit points — each copy lives in
	// its own block, so each declares a distinct object.  Users writing
	// `defer { static int once = 1; if (once) { init(); once = 0; } }`
	// would silently get N independent "once" flags instead of one.
	// `extern` is fine (binds to a single external symbol).  `typedef`
	// is compile-time only.  Scan forward through decl-specifiers.
	for (Token *s = tok; s && s->kind != TK_EOF; s = tok_next(s)) {
		if (s->tag & TT_STORAGE) {
			// "extern" is safe: binds to one external symbol regardless
			// of how many times its declaration is textually emitted.
			if (s->ch0 != 'e' || !equal(s, "extern"))
				error_tok(s, "'static' or thread-local storage inside defer block "
				             "creates duplicate state per exit path; hoist the "
				             "declaration outside the defer body");
			continue;
		}
		if (s->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_BITINT | TT_INLINE)) {
			if (s->flags & TF_OPEN) { Token *m = tok_match(s); if (m) s = m; }
			continue;
		}
		break;
	}

	if (tok->kind == TK_KEYWORD) {
		validate_defer_control_flow(tok, in_loop, in_switch);
		if ((tok->tag & TT_DEFER) && !is_known_typedef(tok) &&
		    !match_ch(tok_next(tok), ':') && !(tok_next(tok) && (tok_next(tok)->tag & TT_ASSIGN)))
			error_tok(tok, "nested defer is not supported");
	}

	if (FEAT(F_ORELSE)) {
		Token *prev_oe = NULL;
		for (Token *s = tok; s && s->kind != TK_EOF && !match_ch(s, ';') && !((s->flags & TF_CLOSE) && s->ch0 == '}'); s = tok_next(s)) {
			if (s->flags & TF_OPEN) {
				// Recurse into (...) and [...] to find orelse inside
				// paren-wrapped initializers (e.g. `int x = (0 orelse return);`).
				// Skip {...} — stmt-exprs handled by the loop below.
				if (match_ch(s, '(') || match_ch(s, '['))
					defer_scan_orelse_in_group(s, in_loop, in_switch, depth);
				prev_oe = tok_match(s) ? tok_match(s) : s;
				s = prev_oe;
				continue;
			}
			if (is_orelse_kw_shadow(s) && (!prev_oe || orelse_shadow_is_kw(prev_oe))) {
				Token *act = tok_next(s);
				if (act && match_ch(act, ';'))
					error_tok(s, "expected statement after 'orelse'");
				validate_defer_control_flow(act, in_loop, in_switch);
				if (act && match_ch(act, '{')) {
					validate_defer_statement(act, in_loop, in_switch, depth + 1);
					Token *close = tok_match(act);
					if (close) { prev_oe = close; s = close; }
				} else {
					prev_oe = act ? act : s;
					s = prev_oe;
				}
				continue;
			}
			prev_oe = s;
		}
	}

	for (Token *s = tok; s && s->kind != TK_EOF && !match_ch(s, ';') && !((s->flags & TF_CLOSE) && s->ch0 == '}'); s = tok_next(s)) {
		if (s->flags & TF_OPEN) {
			if (is_stmt_expr_open(s))
				validate_defer_statement(tok_next(s), in_loop, in_switch, depth + 1);
			else if (match_set(s, CH('(') | CH('[')) || match_ch(s, '{'))
				defer_scan_hidden_stmt_exprs(s, in_loop, in_switch, depth);
			s = tok_match(s) ? tok_match(s) : s;
			continue;
		}
	}
	Token *semi = skip_to_semicolon(tok, NULL);
	return (semi && semi->kind != TK_EOF) ? tok_next(semi) : semi;
}

// --- _Generic Analysis ---

static bool is_knr_params(Token *start, Token *brace) {
	if (!start || start == brace || match_ch(start, ';')) return false;
	bool saw_semi = false;
	for (Token *t = start; t && t != brace && t->kind != TK_EOF; t = tok_next(t)) {
		if (match_ch(t, ';')) saw_semi = true;
		if (t->flags & TF_OPEN) t = tok_match(t) ? tok_match(t) : t;
	}
	return saw_semi;
}

// Detect noreturn function call: tok(args);
static inline Token *try_detect_noreturn_call(Token *tok) {
	if (!(tok->tag & TT_NORETURN_FN)) return NULL;
	// Respect C scoping: if a local variable/parameter shadows a noreturn
	// function name (e.g. void (*exit)(void)), the call is not noreturn.
	TypedefEntry *te = typedef_lookup(tok);
	if (te && te->is_shadow) return NULL;
	if (tok_idx(tok) >= 1) {
		Token *prev = walk_back_past_noise(tok_idx(tok));
		if (prev && (prev->tag & TT_MEMBER)) return NULL;
		// Predecessor is a type keyword/qualifier/storage/_Noreturn/void/*
		// → this is a forward declaration, not a call.
		if (prev && (prev->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_INLINE | TT_SUE)))
			return NULL;
		if (prev && match_ch(prev, '*')) return NULL;
	}
	Token *call = tok_next(tok);
	if (!call || !match_ch(call, '(') || !tok_match(call)) return NULL;
	Token *after = tok_next(tok_match(call));
	return (after && match_ch(after, ';')) ? after : NULL;
}

// Find the '(' token after a keyword, skipping prep dirs.
static inline Token *p1d_find_open_paren(Token *tok) {
	for (Token *s = tok_next(tok); s && s->kind != TK_EOF; s = tok_next(s)) {
		if (s->kind == TK_PREP_DIR) continue;
		if (match_ch(s, '(')) return s;
		break;
	}
	return NULL;
}

// K&R backward walk: from a ';' token, scan backward for ')' past K&R-style
// parameter type declarations.  Returns the ')' token, or NULL.
static Token *p1_knr_find_close_paren(Token *semi_tok) {
	for (uint32_t pi = tok_idx(semi_tok); pi > 0; pi--) {
		Token *pt = &token_pool[pi - 1];
		if (pt->kind == TK_PREP_DIR) continue;
		if (match_ch(pt, '{') || match_ch(pt, '}')) return NULL;
		// For ')': check if the content is a K&R identifier list
		// (only identifiers and commas). If so, this is the function
		// parameter list. Otherwise skip the balanced group (e.g.
		// function pointer params: void (*cb)(int, int);).
		if (match_ch(pt, ')') && tok_match(pt)) {
			Token *open = tok_match(pt);
			bool is_ident_list = true;
			for (Token *t = tok_next(open); t && t != pt; t = tok_next(t)) {
				if (t->kind != TK_IDENT && !match_ch(t, ',') &&
				    t->kind != TK_PREP_DIR) {
					is_ident_list = false;
					break;
				}
			}
			if (is_ident_list) return pt;
			pi = tok_idx(open) + 1; // +1 because loop does pi--
			continue;
		}
		// Skip non-paren balanced groups (e.g. ']' from array decls).
		if ((pt->flags & TF_CLOSE) && tok_match(pt)) {
			pi = tok_idx(tok_match(pt)) + 1;
			continue;
		}
	}
	return NULL;
}

// Skip per-declarator 'raw' keywords (e.g. "int x, raw y;").
// Returns token past raw chain, or original t if not raw.
// Sets *saw_raw = true if raw was found.
// Probes past attributes/pragmas via skip_noise before checking TF_RAW,
// matching Pass 2's process_declarators logic.
static inline Token *p1_skip_decl_raw(Token *t, bool *saw_raw) {
	Token *probe = skip_noise(t);
	if ((probe->flags & TF_RAW) && !is_known_typedef(probe)) {
		Token *after = skip_noise(tok_next(probe));
		if (after && ((is_valid_varname(after) && !is_type_keyword(after) &&
			       !is_known_typedef(after) && !(after->tag & (TT_QUALIFIER | TT_SUE))) ||
			      match_ch(after, '*') || match_ch(after, '('))) {
			while ((after->flags & TF_RAW) && !is_known_typedef(after))
				after = skip_noise(tok_next(after));
			*saw_raw = true;
			return after;
		}
	}
	return t;
}

static inline bool is_assignment_operator_token(Token *tok) {
	return (tok->tag & TT_ASSIGN) && tok_loc(tok)[tok->len - 1] == '=';
}

// Returns true if 'raw' is followed by a declaration context (type keyword, typedef, *, etc.)
static bool is_raw_declaration_context(Token *after_raw) {
	after_raw = skip_noise(after_raw);
	if (!after_raw) return false;
	if (is_type_keyword(after_raw) || is_known_typedef(after_raw) ||
	    (after_raw->tag & (TT_QUALIFIER | TT_SUE | TT_STORAGE | TT_INLINE | TT_TYPEDEF)) ||
	    ((after_raw->flags & TF_RAW) && !is_known_typedef(after_raw)))
		return true;
	// Distinguish pointer declaration (raw *x) from multiplication (raw * 5).
	// In a declaration, * is followed by another *, qualifier, (, or an identifier.
	if (match_ch(after_raw, '*')) {
		Token *after_star = skip_noise(tok_next(after_raw));
		while (after_star && (match_ch(after_star, '*') ||
		       (after_star->tag & TT_QUALIFIER)))
			after_star = skip_noise(tok_next(after_star));
		return after_star && (is_valid_varname(after_star) || match_ch(after_star, '('));
	}
	return false;
}

// Extended raw context: also matches per-declarator raw after comma (int x, raw y;)
static bool is_raw_strip_context(Token *after_raw) {
	if (is_raw_declaration_context(after_raw)) return true;
	after_raw = skip_noise(after_raw);
	Token *boundary = after_raw ? skip_noise(tok_next(after_raw)) : NULL;
	return after_raw && is_valid_varname(after_raw) && !is_type_keyword(after_raw) &&
	       !is_known_typedef(after_raw) && !(after_raw->tag & (TT_QUALIFIER | TT_SUE)) &&
	       boundary &&
	       (match_ch(boundary, ',') || match_ch(boundary, ';') ||
	        match_set(boundary, CH('[') | CH('(') | CH('=') | CH(':')));
}

static bool has_effective_const_qual(Token *type_start, TypeSpecResult *type, DeclResult *decl) {
	bool has_const_qual = (type->has_const && !decl->is_func_ptr && !decl->is_pointer) || decl->is_const;
	if (type->has_constexpr) has_const_qual = true;
	if (type->has_typeof && !decl->is_func_ptr && !decl->is_pointer)
		has_const_qual = true;
	if (!has_const_qual && !decl->is_func_ptr && !decl->is_pointer) {
		for (Token *t = type_start; t && t != type->end; t = tok_next(t))
			if (is_const_typedef(t)) { has_const_qual = true; break; }
	}
	return has_const_qual;
}

static bool has_storage_in(Token *from, Token *to) {
	for (Token *s = from; s && s != to; s = tok_next(s))
		if (s->tag & TT_STORAGE) return true;
	return false;
}

static bool needs_space(Token *prev, Token *tok) {
	if (!prev || tok_at_bol(tok)) return false;
	if (tok_has_space(tok)) return true;
	if ((is_identifier_like(prev) || prev->kind == TK_NUM) &&
	    (is_identifier_like(tok) || tok->kind == TK_NUM))
		return true;
	if (prev->kind != TK_PUNCT || tok->kind != TK_PUNCT) return false;
	char a = (prev->len == 1) ? prev->ch0 : tok_loc(prev)[prev->len - 1];
	char b = tok->ch0;
	if (b == '=') return strchr("=!<>+-*/%&|^", a) != NULL;
	return (a == b && strchr("+-<>&|#", a)) || (a == '-' && b == '>') ||
	       (a == '/' && b == '*') || (a == '*' && b == '/');
}

static bool declarator_has_bracket_orelse(Token *start, Token *end) {
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = tok_next(t))
		if (tok_ann(t) & P1_OE_BRACKET) return true;
	return false;
}

static uint16_t find_body_scope_id(Token *body_start) {
	if (body_start && match_ch(body_start, '{')) {
		uint32_t idx = tok_idx(body_start);
		int low = 1, high = (int)scope_tree_count - 1;
		while (low <= high) {
			int mid = low + (high - low) / 2;
			if (scope_tree[mid].open_tok_idx == idx) return (uint16_t)mid;
			if (scope_tree[mid].open_tok_idx < idx) low = mid + 1;
			else high = mid - 1;
		}
	}
	return 0;
}

// full=false: reset for reuse; full=true: free everything
void tokenizer_teardown(bool full) {
	if (ctx->input_files) { for (int i = 0; i < ctx->input_file_count; i++) free_file_contents(ctx->input_files[i]); }
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


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

#if defined(__GNUC__) || defined(__clang__)
#define PRISM_COLD __attribute__((cold, noinline))
#define PRISM_HOT __attribute__((hot))
#define PRISM_PURE __attribute__((pure))
#define PRISM_CONST_FN __attribute__((const))
#define PRISM_ALWAYS_INLINE __attribute__((always_inline))
#define PRISM_FLATTEN __attribute__((flatten))
#define PRISM_MAYBE_UNUSED __attribute__((unused))
#define PRISM_LIKELY(x) __builtin_expect(!!(x), 1)
#define PRISM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define PRISM_COLD
#define PRISM_HOT
#define PRISM_PURE
#define PRISM_CONST_FN
#define PRISM_ALWAYS_INLINE
#define PRISM_FLATTEN
#define PRISM_MAYBE_UNUSED
#define PRISM_LIKELY(x) (x)
#define PRISM_UNLIKELY(x) (x)
#endif

#define TOMBSTONE ((char *)1)

#include "mem.c"

#define ENTRY_MATCHES(ent, k, kl)                                                                            \
	((ent)->key && (ent)->key != TOMBSTONE && (ent)->key_len == (kl) &&                                   \
	 prism_memeq_runtime_sized((ent)->key, (k), (size_t)(kl)))
#define IS_DIGIT(c) ((unsigned)(c) - '0' < 10u)
#define IS_ALPHA(c) (((unsigned)((c) | 0x20) - 'a') < 26u || (c) == '_' || (c) == '$')
#define IS_ALNUM(c) (IS_DIGIT(c) || IS_ALPHA(c))
#define IS_XDIGIT(c) (IS_DIGIT(c) || ((unsigned)((c) | 0x20) - 'a') < 6u)
#define ARENA_DEFAULT_BLOCK_SIZE (64 * 1024)
#define KW_MARKER 0x80000000ULL // Internal marker bit for keyword map: values are (tag | KW_MARKER)
#define KW_FLAGS_SHIFT 32	// Extra token flags encoded in bits 32-47 of keyword value

// Centralized diagnostic strings. Many appear at multiple Pass 2 emit /
static const char ERR_ORELSE_STMT_LEVEL[] = "'orelse' cannot be used here (it must appear at the "
					    "statement level in a declaration or bare expression)";
/* Canonical stray-'defer' diagnostic: a defer keyword in any position that is
 * not statement-level (declarator/argument/dimension interiors, sizeof and
 * _Static_assert operands, expression positions).  A single wording keeps the
 * message stable across the many Phase-1 sites that detect it.  Contains both
 * "expression context" and "parenthesized" so context-agnostic callers'
 * expectations hold. */
static const char ERR_DEFER_EXPR_CTX[] =
    "'defer' cannot be used in expression context (array dimensions, parenthesized "
    "expressions, function arguments, sizeof/_Static_assert operands, etc.); move it to "
    "statement position";
static const char ERR_BARE_ORELSE_SPANS_PP[] = "bare orelse assignment cannot be used when the "
					       "expression spans preprocessor conditionals — the "
					       "transpiler would emit tokens from all branches, "
					       "producing invalid C; use a temporary variable or "
					       "move the #ifdef outside the expression";
static const char ERR_REGISTER_ATOMIC_AGGREGATE[] = "'register _Atomic' aggregate cannot be safely "
						    "zero-initialized; remove 'register' or use 'raw' "
						    "to opt out of automatic initialization";
static const char ERR_REGISTER_UNION[] = "'register' union cannot be safely "
					 "zero-initialized (address-taking is illegal for "
					 "register, and = {0} only zeros the first member); "
					 "remove 'register' or use 'raw' to opt out of "
					 "automatic initialization";
static const char ERR_REGISTER_VLA[] = "'register' VLA cannot be safely zero-initialized "
				       "(address-taking is illegal for register, and VLAs "
				       "cannot use initializer syntax); remove 'register' "
				       "or use 'raw' to opt out of automatic initialization";
static const char ERR_REGISTER_EMPTY_AGG[] =
    "'register' empty or zero-size aggregate cannot be safely "
    "zero-initialized (address-taking is illegal for register, and "
    "`= {0}` is rejected by the backend); remove 'register' or use "
    "'raw' to opt out of automatic initialization";
static const char ERR_INIT_STMT_VLA[] = "VLA in for/if/switch init-statement cannot be "
					"safely zero-initialized; move the declaration "
					"before the statement";
static const char ERR_MULTIDECL_VM[] = "multi-declarator with variably-modified "
				       "type specifier requires declaration split which "
				       "would double-evaluate VLA size expressions; "
				       "declare each variable on a separate line";
static const char ERR_ORELSE_STATIC_THREAD[] = "'orelse' cannot be used in the initializer of a "
					       "variable with static or thread storage duration "
					       "(the runtime fallback check would re-execute on "
					       "every function entry, destroying persistence)";
static const char ERR_ORELSE_CONSTEXPR[] = "'orelse' cannot be used with 'constexpr' "
					   "(constexpr requires a compile-time constant "
					   "initializer; orelse produces runtime fallback code)";
static const char ERR_ORELSE_CONST_VM[] = "orelse on a const-qualified variably-modified type "
					  "would duplicate the type specifier, causing VLA "
					  "size expressions to be evaluated twice; hoist the "
					  "value to a non-const variable first";
static const char ERR_BRACKET_OE_VLA_INIT_STMT[] = "bracket orelse in VLA dimensions cannot be used in "
						   "control statement conditions (hoisted temps would "
						   "inject invalid syntax); move the declaration before "
						   "the statement";
static const char ERR_ORELSE_FILE_SCOPE[] = "'orelse' cannot be used in file-scope initializers "
					    "(requires runtime fallback code)";
static const char ERR_ORELSE_STRUCT_VALUE[] = "orelse on struct/union values is not supported "
					      "(memcmp cannot reliably detect zero due to padding)";
static const char ERR_BRACKET_OE_ANON_AGG[] = "bracket orelse / zero-init requiring declaration split "
					      "cannot be used with anonymous struct/union; "
					      "add a tag name or use a typedef";
static const char ERR_ORELSE_ARRAY_NEVER_NULL[] = "orelse on array variable '%.*s' will never trigger "
						  "(array address is never NULL); remove the orelse clause";
static const char ERR_CONST_UNAVOIDABLE_MEMSET[] = "'const' variable requiring unavoidable memset "
						   "(union, VLA, or _Atomic aggregate) cannot be safely "
						   "zero-initialized: modifying a const object is "
						   "undefined behavior. Remove 'const', provide an "
						   "explicit initializer, or use 'raw' to opt out.";
static const char ERR_DEFER_LAST_STMT_EXPR[] = "defer inside a block that is the last "
					       "statement of a statement expression "
					       "would corrupt the expression's return "
					       "value; ensure the last statement of the "
					       "statement expression is outside the "
					       "defer block";
static const char ERR_DEFER_SHADOW_SAME_SCOPE[] = "variable '%.*s' shadows a name captured "
						  "by a defer in the same scope; the defer "
						  "body would bind to the shadowing variable";
static const char ERR_DEFER_CTRL_PAREN[] = "defer cannot appear inside control statement parentheses";
static const char ERR_DEFER_BRACELESS_CTRL[] =
    "defer requires braces in control statements (braceless has no scope)";
static const char ERR_DEFER_STMT_EXPR_TOP[] =
    "defer cannot be at top level of statement expression; wrap in a block";
static const char ERR_DEFER_SWITCH_BRACE[] = "defer in switch case requires braces";
static const char ERR_DEFER_UNTERMINATED[] = "unterminated defer statement; expected ';'";
static const char ERR_DEFER_MISSING_SEMI[] =
    "defer statement appears to be missing ';' (found '%.*s' keyword inside)";
static const char ERR_ORELSE_TERNARY[] = "'orelse' cannot be used inside a ternary expression";
static const char ERR_BOUNDS_COMM_IDX_ARR[] = "commutative subscript 'idx[arr]' bypasses "
					      "-fbounds-check; rewrite as 'arr[idx]'";
static const char ERR_BOUNDS_DERIVED_SUB[] = "-fbounds-check: derived-pointer subscript "
					     "'(&arr[..])[i]' bypasses bounds-check; "
					     "rewrite as 'arr[idx]'";
static const char ERR_BOUNDS_COMM_DERIVED[] = "-fbounds-check: commutative derived-pointer "
					      "subscript 'idx[(&arr[..])]' bypasses "
					      "bounds-check; rewrite as 'arr[idx]'";
static const char ERR_BOUNDS_COMMA_OP[] = "-fbounds-check: comma operand subscript idx[(...,arr)] must be "
					  "rewritten as arr[idx]";
static const char ERR_BOUNDS_COMM_SCAN[] =
    "bounds-check (commutative): array name in index position cannot be verified "
    "(rewrite as array[index], or disable -fbounds-check for this expression)";

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
							       : equal_n(tok, s, (uint32_t)__builtin_strlen(s)))                   \
				 : equal_n(tok, s, (uint32_t)strlen(s)))

#define KEYWORD_HASH(key, len)                                                                               \
	((len) == 0 ? 0                                                                                      \
		    : (((unsigned)(len) * 2 + (unsigned char)(key)[0] * 99 +                                 \
			(unsigned char)((len) > 1 ? (key)[1] : (key)[0]) * 125 +                             \
			(unsigned char)((len) > 6 ? (key)[6] : (key)[(len) - 1]) * 69) &                     \
		       255))

/* Shared capacity growth: double until >= need (or init_cap when empty). */
static inline size_t vec_grow_cap(size_t cap, size_t need, size_t init_cap) {
	size_t new_cap = cap == 0 ? (init_cap > 0 ? init_cap : 1) : cap * 2;
	while (new_cap < need) new_cap *= 2;
	return new_cap;
}

/* need = minimum used count the array must hold (typically count or count+1). */
#define VEC_ENSURE_REALLOC(arr, need, cap, init_cap)                                                         \
	do {                                                                                                 \
		if ((size_t)(need) > (size_t)(cap)) {                                                        \
			size_t _new_cap = vec_grow_cap((size_t)(cap), (size_t)(need), (size_t)(init_cap));   \
			if (_new_cap > SIZE_MAX / sizeof(*(arr))) error("allocation overflow");              \
			void *_tmp = realloc((arr), sizeof(*(arr)) * _new_cap);                              \
			if (!_tmp) error("out of memory");                                                   \
			(arr) = _tmp;                                                                        \
			(cap) = _new_cap;                                                                    \
		}                                                                                            \
	} while (0)

#define ARENA_ENSURE_CAP(arena, arr, count, cap, init_cap, T)                                                \
	do {                                                                                                 \
		if ((size_t)(count) >= (size_t)(cap)) {                                                      \
			size_t old_cap = (size_t)(cap);                                                      \
			size_t new_cap = vec_grow_cap(old_cap, (size_t)(count), (size_t)(init_cap));         \
			if (new_cap > SIZE_MAX / sizeof(T)) error("allocation overflow");                    \
			(arr) = arena_realloc((arena), (arr), sizeof(T) * old_cap, sizeof(T) * new_cap);     \
			(cap) = new_cap;                                                                     \
		}                                                                                            \
	} while (0)

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

/* Forward decls — call sites in tokenize/Pass0 precede the definitions. */
enum {
	WB_SKIP_PREP = 1 << 0,
	WB_SKIP_ATTR = 1 << 1,
	WB_JUMP_GROUPS = 1 << 2,
	WB_JUMP_C23_ATTR = 1 << 3,
	WB_JUMP_ALL_PARENS = 1 << 4,
	WB_JUMP_ATTR_PARENS = 1 << 5,
	WB_FROM_PRED = 1 << 6,
};
#define WB_PAST_NOISE (WB_FROM_PRED | WB_SKIP_PREP | WB_SKIP_ATTR | WB_JUMP_GROUPS)
#define WB_ATTR_NOISE (WB_FROM_PRED | WB_SKIP_PREP | WB_SKIP_ATTR | WB_JUMP_C23_ATTR)
#define WB_SKIP_NOISE (WB_SKIP_PREP | WB_SKIP_ATTR | WB_JUMP_C23_ATTR | WB_JUMP_ATTR_PARENS)
#define WB_SKIP_ATTRS (WB_SKIP_PREP | WB_SKIP_ATTR | WB_JUMP_C23_ATTR | WB_JUMP_ALL_PARENS)
static Token *tok_walk_back(uint32_t start_idx, unsigned flags);
static bool is_raw_declaration_context(Token *raw_kw, Token *after_raw);

typedef struct {
	char *name;
	char *contents;
	size_t contents_len;
	int file_no;
	int line_delta;
	bool owns_contents;
	bool is_system;
	bool is_direct_system_include; // Entered from non-system (re-emit as #include)
	bool skip_emit; // Precomputed is_system && is_include_entry hot-path predicate
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
	TF_OPEN = 1 << 3,	   // Opening delimiter: ( [ {
	TF_CLOSE = 1 << 4,	   // Closing delimiter: ) ] }
	TF_C23_ATTR = 1 << 5,	   // First '[' of C23 [[ ... ]] attribute
	TF_RAW = 1 << 6,	   // 'raw' keyword
	TF_SIZEOF = 1 << 7,	   // sizeof, alignof, _Alignof
	TF_SOFT_KW = 1 << 8,	   // soft keyword usable as identifier (alignas, bool, …)
	TF_STATIC_ASSERT = 1 << 9, // _Static_assert / static_assert
	TF_MS_CC = 1 << 10,	   // MSVC calling-convention keyword (__cdecl, …)
	TF_SYS_SKIP = 1 << 11,	   // token belongs to a system #include entry file; in
				   // non-flatten emit it is skipped verbatim. Precomputed at
				   // tokenize from current_file so the hot emit loop tests one
				   // flag bit instead of a per-token tok_cold + file lookup.
	TF_HAS_PRISM = 1 << 12,	   // matched group contains a defer/orelse token
	TF_LINK_JUMP = 1 << 13,	   // parse_data is a non-adjacent next-token index
};

enum {
	TT_TYPE = 1 << 0, // Type keyword (int, char, void, struct, etc.)
	TT_QUALIFIER = 1 << 1,
	TT_SUE = 1 << 2, // struct/union/enum
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
	TT_CONST = 1 << 13, // const keyword

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

	TT_VOLATILE = 1 << 25, // volatile
	TT_REGISTER = 1 << 26, // register
	TT_TYPEOF =
	    1 << 27, // typeof, typeof_unqual, __typeof__, __typeof, __typeof_unqual__, __typeof_unqual
	TT_BITINT = 1 << 28,	  // _BitInt
	TT_ALIGNAS = 1 << 29,	  // _Alignas, alignas
	TT_ORELSE = 1 << 30,	  // orelse
};
#define TT_STRUCTURAL (1u << 31) // { } ; : — force slow-path dispatch

#define TT_DECL_START                                                                                        \
	(TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_INLINE | TT_ALIGNAS | TT_SKIP_DECL | TT_ATTR)

struct Token {
	uint32_t tag;	    // TT_* bitmask - token classification
	uint32_t parse_data; // Phase-local name resolution/scope ID, or TF_LINK_JUMP target
	uint32_t match_idx; // Token pool index (0 = NULL)
	uint32_t len;	    // Token length in bytes (must handle >65535 for large literals)
	uint8_t kind;
	uint8_t _flags_pad; // Align flags to 2 bytes (keeps Token at 24)
	uint16_t flags;	    // TF_* bitmask (TF_SOFT_KW needs bit 8)
	uint16_t ann;	    // Pass 1 annotation flags (P1_SCOPE_*, P1_OE_*, P1_IS_DECL, …)
	uint8_t ch0;	    // First source byte — avoids tok_loc() indirection in hot paths
	uint8_t _pad;	    // Explicit padding to 24 bytes
}; // 24 bytes

typedef char prism_assert_token_24[(sizeof(struct Token) == 24) ? 1 : -1];

typedef struct {
	uint32_t loc_offset; // Byte offset from File->contents
	int32_t line_no : 18;
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
	const char **dep_flags; // -Wp,-MMD, -MD, -MF etc. (preprocessor-only)
	int dep_flags_count;
	int scope_depth;
	int block_depth;
	int aggregate_member_nest; // `{` after struct/union/enum kw (expr/type contexts without scope push)

	bool last_system_header;
	int last_line_no;
	char *last_filename;
	bool at_stmt_start;
	int system_include_count;
	int raw_block_depth; /* Pass 2: nest depth inside `raw { ... }` suppress blocks */
	unsigned long long ret_counter;
	Token *func_ret_type_start;	   // First token of return type (after storage/function specifiers)
	Token *func_ret_type_end;	   // Function name token (exclusive end of return type range)
	Token *func_ret_type_suffix_start; // For complex declarators: closing ')' after func params
	Token *func_ret_type_suffix_end;   // For complex declarators: token after suffix (exclusive)
	unsigned *bracket_oe_ids;	   // Pre-assigned temp IDs for bracket orelse hoisting (dynamic)
	int bracket_oe_count;		   // Count of hoisted bracket orelse temps
	int bracket_oe_cap;		   // Capacity of bracket_oe_ids array
	int bracket_oe_next;		   // Next temp to consume during emit

	Token **typeof_vars;
	int typeof_var_count;
	int typeof_var_cap;
	unsigned *bracket_dim_ids; // Temp IDs for non-orelse brackets (0 = not hoisted)
	int bracket_dim_count;	   // Count of pre-hoisted dimension temps
	int bracket_dim_cap;
	int bracket_dim_next; // Next dim temp to consume during emit

	char **source_defines;	     // Array of "NAME=VALUE" or "NAME" strings (malloc'd)
	char **source_define_guards; // Parallel: NULL (unconditional) or condition guard text (malloc'd)
	int source_define_count;
	int source_define_cap;
	Token *tp_pool;	    // Hot: tag, parse_data, match_idx, len, kind, flags
	TokenCold *tp_cold; // Cold: loc_offset, line_no, file_idx
	uint32_t tp_count;  // Next free index. 0 reserved as NULL sentinel.
	uint32_t tp_cap;
	KeywordEntry kw_cache[256];
	uint32_t keyword_cache_features; // features used when keyword_cache was built

	// Digraph normalization targets (per-context for token loc comparison)
	char dg_bracket_open[2];
	char dg_bracket_close[2];
	char dg_brace_open[2];
	char dg_brace_close[2];
	char dg_hash[2];
	char dg_paste[3];
	void *p1_scope_tree; // ScopeInfo[] — flat array indexed by scope_id
	uint16_t p1_scope_count;
	/* Capacity, NOT a scope_id: scope_count maxes at 65534 (uint16) but the
	 * doubling cap reaches 65536, which must not truncate to 0 — that would
	 * pass old_size=0 to arena_realloc and drop the whole scope tree. */
	uint32_t p1_scope_cap;
	void *p1_func_meta; // FuncMeta[] — one per function definition
	int p1_func_meta_count;
	int p1_func_meta_cap;
	void *p1_func_entries; // P1FuncEntry[] — flat combined array
	int p1_func_entry_count;
	int p1_func_entry_cap;

#ifdef PRISM_LIB_MODE
	char *active_membuf; // open_memstream buffer; freed on longjmp recovery
#endif
} PrismContext;

static PRISM_THREAD_LOCAL PrismContext *ctx = NULL;

static inline bool is_digraph_loc(char *loc) {
	return loc == ctx->dg_bracket_open || loc == ctx->dg_bracket_close || loc == ctx->dg_brace_open ||
	       loc == ctx->dg_brace_close || loc == ctx->dg_hash || loc == ctx->dg_paste;
}

#define token_pool (ctx->tp_pool)
#define token_cold (ctx->tp_cold)
#define token_count (ctx->tp_count)
#define token_cap (ctx->tp_cap)
#define keyword_cache (ctx->kw_cache)
#define digraph_norm_bracket_open (ctx->dg_bracket_open)
#define digraph_norm_bracket_close (ctx->dg_bracket_close)
#define digraph_norm_brace_open (ctx->dg_brace_open)
#define digraph_norm_brace_close (ctx->dg_brace_close)
#define digraph_norm_hash (ctx->dg_hash)
#define digraph_norm_paste (ctx->dg_paste)

static PRISM_COLD noreturn void error(char *fmt, ...);
static void hashmap_put(HashMap *map, char *key, int keylen, void *val);
static void hashmap_remove(HashMap *map, char *key, int keylen);

static inline bool tok_at_bol(Token *tok) {
	return tok->flags & TF_AT_BOL;
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
	if (size > (size_t)PTRDIFF_MAX) __builtin_unreachable();
	void *ptr = arena_alloc_uninit(arena, size);
	memset(ptr, 0, size);
	return ptr;
}

static void *arena_realloc(Arena *arena, void *old, size_t old_size, size_t new_size) {
	if (new_size <= old_size) return old;
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
	if (old && old_size > 0) memcpy(p, old, old_size);
	memset((char *)p + old_size, 0, new_size - old_size);
	return p;
}

typedef struct {
	ArenaBlock *block;
	size_t used;
} ArenaMark;

static ArenaMark arena_mark(Arena *arena) {
	return (ArenaMark){arena->current, arena->current ? arena->current->used : 0};
}

static void arena_restore(Arena *arena, ArenaMark mark) {
	for (ArenaBlock *b = mark.block ? mark.block->next : arena->head; b; b = b->next) b->used = 0;
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
	size_t new_cap = vec_grow_cap(token_cap, need, 65536);
	if (new_cap > (uint32_t)-1 || new_cap > SIZE_MAX / sizeof(Token))
		error("token pool capacity exceeded");
	Token *p = realloc(token_pool, new_cap * sizeof(Token));
	if (!p) error("out of memory allocating token pool");
	token_pool = p;
	TokenCold *c = realloc(token_cold, new_cap * sizeof(TokenCold));
	if (!c) error("out of memory allocating token cold pool");
	token_cold = c;
	token_cap = (uint32_t)new_cap;
	// Pool index 0 is the NULL sentinel and must never look like a real token.
	if (token_count <= 1) {
		memset(&token_pool[0], 0, sizeof(Token));
		memset(&token_cold[0], 0, sizeof(TokenCold));
	}
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE TokenCold *tok_cold(Token *tok) {
	uintptr_t bd = (uintptr_t)tok - (uintptr_t)token_pool;
#if defined(__SIZEOF_INT128__)
	uint64_t hi = (uint64_t)(((__uint128_t)bd * 0xAAAAAAAAAAAAAAABULL) >> 64);
	uint64_t idx = hi >> 4;
#else
	uint64_t idx = (uint64_t)bd / 24u;
#endif
	return &token_cold[idx];
}

static inline PRISM_PURE char *tok_loc(Token *tok) {
	TokenCold *c = tok_cold(tok);
	return ctx->input_files[c->file_idx]->contents + c->loc_offset;
}

static inline PRISM_PURE Token *tok_next(Token *tok) {
	if (tok->kind == TK_EOF) return NULL;
	if (__builtin_expect(!(tok->flags & TF_LINK_JUMP), 1)) return tok + 1;
	return &token_pool[tok->parse_data];
}

static inline PRISM_PURE Token *tok_match(Token *tok) {
	return tok->match_idx ? &token_pool[tok->match_idx] : NULL;
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE uint32_t tok_idx(Token *tok) {
	if (!tok) return 0;
	uintptr_t bd = (uintptr_t)tok - (uintptr_t)token_pool;
#if defined(__SIZEOF_INT128__)
	uint64_t hi = (uint64_t)(((__uint128_t)bd * 0xAAAAAAAAAAAAAAABULL) >> 64);
	return (uint32_t)(hi >> 4);
#else
	return (uint32_t)((uint64_t)bd / 24u);
#endif
}

static inline PRISM_PURE uint32_t fast_hash(char *s, uint32_t len) {
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
		a = ((uint64_t)(unsigned char)s[0] << 16) | ((uint64_t)(unsigned char)s[len >> 1] << 8) |
		    (unsigned char)s[len - 1];
	}
	/* Rotate b before mixing: for len == 8 the first and last 8 bytes are the
	 * SAME, so a plain `a ^= b` cancels the content and every 8-char key hashes
	 * to a constant → one giant open-addressing cluster → O(n^2) probing. */
	a ^= (uint64_t)len * 0x9e3779b97f4a7c15ULL;
	a ^= (b << 32) | (b >> 32);
	a *= 0xbf58476d1ce4e5b9ULL;
	a ^= a >> 31;
	a *= 0x94d049bb133111ebULL;
	a ^= a >> 31;
	return (uint32_t)a;
}

static PRISM_HOT PRISM_PURE void *hashmap_get_hashed(HashMap *map, char *key, int keylen, uint32_t hash) {
	int mask = map->capacity - 1;
	for (int i = 0; i <= mask; i++) {
		HashEntry *ent = &map->buckets[(hash + i) & mask];
		if (__builtin_expect(!ent->key, 0)) return NULL;
		if (__builtin_expect(ent->key == TOMBSTONE, 0)) continue;
		if (ent->hash == hash && ent->key_len == (uint16_t)keylen &&
		    prism_memeq_runtime_sized(ent->key, key, (uint32_t)keylen))
			return ent->val;
	}
	return NULL;
}

static PRISM_PURE void *hashmap_get(HashMap *map, char *key, int keylen) {
	if (__builtin_expect(!map->buckets, 0)) return NULL;
	return hashmap_get_hashed(map, key, keylen, fast_hash(key, keylen));
}

static inline PRISM_PURE int hashmap_index_hashed(HashMap *map, char *key, int keylen, uint32_t hash) {
	if (__builtin_expect(!map->buckets, 0)) return -1;
	void *val = hashmap_get_hashed(map, key, keylen, hash);
	return val ? (int)(intptr_t)val - 1 : -1;
}

static void hashmap_remove(HashMap *map, char *key, int keylen) {
	if (!map->buckets) return;
	uint32_t hash = fast_hash(key, keylen);
	int mask = map->capacity - 1;
	for (int i = 0; i <= mask; i++) {
		HashEntry *ent = &map->buckets[(hash + i) & mask];
		if (!ent->key) return;
		if (ent->key == TOMBSTONE) continue;
		if (ent->hash == hash && ent->key_len == (uint16_t)keylen &&
		    prism_memeq_runtime_sized(ent->key, key, (uint32_t)keylen)) {
			ent->key = TOMBSTONE;
			ent->val = NULL;
			map->used--;
			return;
		}
	}
}

static void hashmap_resize(HashMap *map, int newcap) {
	HashMap new_map = {.buckets = arena_alloc(&ctx->main_arena, (size_t)newcap * sizeof(HashEntry)),
			   .capacity = newcap};
	int new_mask = newcap - 1;
	for (int i = 0; i < map->capacity; i++) {
		HashEntry *ent = &map->buckets[i];
		if (ent->key && ent->key != TOMBSTONE) {
			uint32_t h = ent->hash;
			int idx;
			for (int j = 0;; j++) {
				idx = (h + j) & new_mask;
				if (!new_map.buckets[idx].key) break;
			}
			new_map.buckets[idx] = *ent;
			new_map.used++;
		}
	}
	*map = new_map;
}

static void hashmap_put_hashed(HashMap *map, char *key, int keylen, void *val, uint32_t hash) {
	if (__builtin_expect(!map->buckets, 0)) {
		map->buckets = arena_alloc(&ctx->main_arena, 64 * sizeof(HashEntry));
		map->capacity = 64;
	} else if (__builtin_expect((uint64_t)map->used * 10 >= (uint64_t)map->capacity * 7, 0)) {
		hashmap_resize(map, map->capacity * 2);
	}

	int mask = map->capacity - 1;
	int first_empty = -1;
	for (int i = 0; i <= mask; i++) {
		int idx = (hash + i) & mask;
		HashEntry *ent = &map->buckets[idx];
		if (ent->key && ent->key != TOMBSTONE && ent->hash == hash &&
		    ent->key_len == (uint16_t)keylen && prism_memeq_runtime_sized(ent->key, key, (uint32_t)keylen)) {
			ent->val = val;
			return;
		}

		if (first_empty < 0 && (!ent->key || ent->key == TOMBSTONE)) first_empty = idx;
		if (!ent->key) break;
	}

	if (first_empty < 0) error("hashmap_put: no empty slot found (internal error)");
	HashEntry *ent = &map->buckets[first_empty];
	ent->key = key;
	ent->key_len = keylen;
	ent->hash = hash;
	ent->val = val;
	map->used++;
}

static void hashmap_put(HashMap *map, char *key, int keylen, void *val) {
	hashmap_put_hashed(map, key, keylen, val, fast_hash(key, keylen));
}

/* Tokenizer-only map access: parse_data still contains the lexer hash here.
 * Later parse passes use token_name_hash() because finalization may repurpose it. */
static inline PRISM_PURE void *lex_token_map_get(HashMap *map, Token *tok) {
	if (!map->buckets) return NULL;
	return hashmap_get_hashed(map, tok_loc(tok), tok->len, tok->parse_data);
}

static inline void lex_token_map_put(HashMap *map, Token *tok, void *value) {
	hashmap_put_hashed(map, tok_loc(tok), tok->len, value, tok->parse_data);
}

static void hashmap_discard(HashMap *map) {
	*map = (HashMap){0};
}

static void hashmap_clear(HashMap *map) {
	if (map->buckets) memset(map->buckets, 0, (size_t)map->capacity * sizeof(HashEntry));
	map->used = 0;
}

static char *intern_filename(const char *name) {
	if (!name) return NULL;
	size_t len = strlen(name) + 1;
	char *copy = arena_alloc_uninit(&ctx->main_arena, len);
	return memcpy(copy, name, len);
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

static PRISM_COLD noreturn void error(char *fmt, ...) {
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

static PRISM_COLD void
verror_at(char *filename, char *input, int line_no, char *loc, const char *severity, const char *fmt,
	  va_list ap) {
	/* GCC-style severity label so `-fno-safety` warnings are not mistaken
	 * for hard errors by humans or CI log scrapers (which key on
	 * `file:line: warning:` / `file:line: error:`).  Defaults to "error". */
	const char *sev = severity ? severity : "error";
	// Digraph locs point to static storage; avoid UB from cross-object pointer comparison
	if (!input || !loc || line_no <= 0 || is_digraph_loc(loc)) {
		fprintf(stderr, "%s:%d: %s: ", filename ? filename : "<unknown>",
			line_no > 0 ? line_no : 0, sev);
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
		return;
	}

	char *line = loc;
	while (input < line && line[-1] != '\n') line--;
	char *end = loc;
	while (*end && *end != '\n') end++;
	int indent = fprintf(stderr, "%s:%d: %s: ", filename, line_no, sev);
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

PRISM_COLD noreturn void error_at(char *loc, char *fmt, ...) {
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
			  "error",
			  fmt,
			  ap);
	else {
		fprintf(stderr, "error: ");
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
	}
	va_end(ap);
	exit(1);
}

PRISM_COLD noreturn void error_tok(Token *tok, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	File *f = tok_file(tok);
#ifdef PRISM_LIB_MODE
	if (lib_error_enabled()) lib_errorf(tok_line_no(tok), fmt, ap);
#endif
	verror_at(f->name, f->contents, tok_line_no(tok), tok_loc(tok), "error", fmt, ap);
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
	verror_at(f->name, f->contents, tok_line_no(tok), tok_loc(tok), "warning", fmt, ap);
	va_end(ap);
#endif
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool equal_n(Token *tok, const char *op, uint32_t len) {
	/* Operator/keyword spellings are short; caller proves len fits the bound. */
	return tok->len == len && tok->ch0 == (uint8_t)op[0] &&
	       (len <= 1 || prism_memeq_bounded(tok_loc(tok) + 1, op + 1, (size_t)len - 1u));
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool _equal_1(Token *tok, char c) {
	return tok->len == 1 && tok->ch0 == (uint8_t)c;
}

#define match_ch _equal_1

#define CH(c) (1ULL << ((c) - 32))
#define match_set(tok, mask)                                                                                 \
	((tok)->len == 1 && (unsigned)((tok)->ch0 - 32) < 64u && ((mask) & (1ULL << ((tok)->ch0 - 32))))

// Handles _Pragma(...), __attribute__((...)), C23 [[...]], and #pragma directives
static inline bool is_stmt_expr_open(Token *t) {
	if (!match_ch(t, '(')) return false;
	Token *n = tok_next(t);
	while (n && n->kind != TK_EOF) {
		if (n->kind == TK_PREP_DIR) {
			n = tok_next(n);
			continue;
		}
		if ((n->tag & TT_ATTR) && tok_next(n) && match_ch(tok_next(n), '(') &&
		    tok_match(tok_next(n))) {
			n = tok_next(tok_match(tok_next(n)));
			continue;
		}
		if (n->flags & TF_C23_ATTR) {
			Token *close = tok_match(n);
			if (close) {
				n = tok_next(close);
				continue;
			}
		}
		break;
	}
	return n && match_ch(n, '{');
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool is_else_kw(Token *t) {
	return (t->tag & TT_IF) && t->ch0 == 'e';
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool is_do_kw(Token *t) {
	return (t->tag & TT_LOOP) && t->ch0 == 'd';
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool is_else_or_do(Token *t) {
	return is_else_kw(t) || is_do_kw(t);
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool _equal_2(Token *tok, const char *s) {
	if (tok->len != 2 || tok->ch0 != (uint8_t)s[0]) return false;
	return tok_loc(tok)[1] == s[1];
}

static inline PRISM_PURE bool is_gnu_label_decl_head(Token *tok) {
	return tok && tok->len == 9 && tok->ch0 == '_' && prism_memeq_static(tok_loc(tok), "__label__", 9);
}

static inline PRISM_PURE uint64_t keyword_lookup(char *key, int keylen) {
	if (keylen < 2) return 0;
	unsigned slot = KEYWORD_HASH(key, keylen);
	for (int i = 0; i < 32; i++) {
		KeywordEntry *ent = &keyword_cache[(slot + i) & 255];
		if (!ent->name) return 0;
		if (ent->len == keylen && prism_memeq_runtime_sized(ent->name, key, (uint32_t)keylen)) return ent->value;
	}
	return 0;
}

static inline PRISM_PURE bool is_potential_func_name(Token *tok) {
	Token *next = tok_next(tok);
	return tok->kind <= TK_KEYWORD && next && next->ch0 == '(' && (next->flags & TF_OPEN) &&
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
		uint16_t extra_flags;
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
	    {"alignof", TT_SKIP_DECL, true, TF_SIZEOF | TF_SOFT_KW},
	    {"_Alignof", TT_SKIP_DECL, true, TF_SIZEOF},
	    {"_Generic", TT_SKIP_DECL | TT_GENERIC, true},
	    {"_Static_assert", TT_SKIP_DECL, true, TF_STATIC_ASSERT},
	    {"static_assert", TT_SKIP_DECL, true, TF_SOFT_KW | TF_STATIC_ASSERT},
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
	    {"_Nonnull", TT_QUALIFIER, true},
	    {"_Nullable", TT_QUALIFIER, true},
	    {"_Null_unspecified", TT_QUALIFIER, true},
	    {"_Noreturn", TT_SKIP_DECL | TT_INLINE, true},
	    {"noreturn", TT_SKIP_DECL | TT_INLINE, true, TF_SOFT_KW},
	    {"__inline", TT_INLINE, true},
	    {"__inline__", TT_INLINE, true},
	    {"_Thread_local", TT_STORAGE, true},
	    {"__thread", TT_STORAGE, true},
	    {"constexpr", TT_QUALIFIER, true, TF_SOFT_KW},
	    {"thread_local", TT_QUALIFIER | TT_SKIP_DECL | TT_STORAGE, true, TF_SOFT_KW},
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
	    {"bool", TT_TYPE, true, TF_SOFT_KW},
	    {"_Complex", TT_TYPE, true},
	    {"_Imaginary", TT_TYPE, true},
	    /* Extension type spellings are soft: after an established type they
	     * are ordinary declarator names (`int _Float32;`, `int __int64;`) on
	     * clang/gcc, matching `bool`. As a lone type specifier they keep
	     * TT_TYPE so `_Float32 x;` / `__int64 y;` still zero-init. */
	    {"__int128", TT_TYPE, true, TF_SOFT_KW},
	    {"__int128_t", TT_TYPE, true, TF_SOFT_KW},
	    {"__uint128", TT_TYPE, true, TF_SOFT_KW},
	    {"__uint128_t", TT_TYPE, true, TF_SOFT_KW},
	    {"__int8", TT_TYPE, true, TF_SOFT_KW},
	    {"__int16", TT_TYPE, true, TF_SOFT_KW},
	    {"__int32", TT_TYPE, true, TF_SOFT_KW},
	    {"__int64", TT_TYPE, true, TF_SOFT_KW},
	    {"__float128", TT_TYPE, true, TF_SOFT_KW},
	    {"__float80", TT_TYPE, true, TF_SOFT_KW},
	    {"__fp16", TT_TYPE, true, TF_SOFT_KW},
	    {"__bf16", TT_TYPE, true, TF_SOFT_KW},
	    {"_Float16", TT_TYPE, true, TF_SOFT_KW},
	    {"_Float32", TT_TYPE, true, TF_SOFT_KW},
	    {"_Float64", TT_TYPE, true, TF_SOFT_KW},
	    {"_Float128", TT_TYPE, true, TF_SOFT_KW},
	    {"_Float32x", TT_TYPE, true, TF_SOFT_KW},
	    {"_Float64x", TT_TYPE, true, TF_SOFT_KW},
	    {"_Float128x", TT_TYPE, true, TF_SOFT_KW},
	    {"_Decimal32", TT_TYPE, true, TF_SOFT_KW},
	    {"_Decimal64", TT_TYPE, true, TF_SOFT_KW},
	    {"_Decimal128", TT_TYPE, true, TF_SOFT_KW},
	    {"typeof_unqual", TT_TYPE | TT_TYPEOF, true, TF_SOFT_KW},
	    {"__typeof_unqual__", TT_TYPE | TT_TYPEOF, true},
	    {"__typeof_unqual", TT_TYPE | TT_TYPEOF, true},
	    {"auto", TT_QUALIFIER | TT_TYPE, true},
	    {"register", TT_QUALIFIER | TT_REGISTER, true},
	    {"_Alignas", TT_QUALIFIER | TT_ALIGNAS, true},
	    {"alignas", TT_QUALIFIER | TT_ALIGNAS, true, TF_SOFT_KW},
	    {"typeof", TT_TYPE | TT_TYPEOF, true, TF_SOFT_KW},
	    {"__typeof__", TT_TYPE | TT_TYPEOF, true},
	    {"__typeof", TT_TYPE | TT_TYPEOF, true},
	    {"__auto_type", TT_TYPE | TT_TYPEOF, true},
	    {"_BitInt", TT_TYPE | TT_BITINT, true},
	    {"asm", TT_SKIP_DECL | TT_ASM, true, TF_SOFT_KW},
	    {"__asm__", TT_SKIP_DECL | TT_ASM, true},
	    {"__asm", TT_SKIP_DECL | TT_ASM, true},
	    {"__attribute__", TT_ATTR | TT_QUALIFIER, true},
	    {"__attribute", TT_ATTR | TT_QUALIFIER, true},
	    {"__declspec", TT_ATTR | TT_QUALIFIER, true},
	    {"__cdecl", 0, true, TF_MS_CC},
	    {"__stdcall", 0, true, TF_MS_CC},
	    {"__fastcall", 0, true, TF_MS_CC},
	    {"__thiscall", 0, true, TF_MS_CC},
	    {"__vectorcall", 0, true, TF_MS_CC},
	    {"_cdecl", 0, true, TF_MS_CC},
	    {"_stdcall", 0, true, TF_MS_CC},
	    {"_fastcall", 0, true, TF_MS_CC},
	    {"cdecl", 0, true, TF_MS_CC | TF_SOFT_KW},
	    {"stdcall", 0, true, TF_MS_CC | TF_SOFT_KW},
	    {"_Pragma", TT_ATTR, true},
	    {"__pragma", TT_ATTR, true},
	    {"__extension__", TT_INLINE, true},
	    {"__builtin_va_list", 0, true},
	    {"__builtin_va_arg", 0, true},
	    {"__builtin_offsetof", 0, true, TF_SIZEOF},
	    {"offsetof", 0, true, TF_SIZEOF | TF_SOFT_KW},
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

/* C11 6.4.3: \uXXXX (4 hex) or \UXXXXXXXX (8 hex). Returns length or 0. */
static int read_ucn(char *p) {
	if (*p != '\\') return 0;
	int nhex;
	if (p[1] == 'u')
		nhex = 4;
	else if (p[1] == 'U')
		nhex = 8;
	else
		return 0;
	for (int i = 0; i < nhex; i++)
		if (!IS_XDIGIT(p[2 + i])) return 0;
	return 2 + nhex;
}

static int read_ident(char *start) {
	char *p = start;
	int ucn = read_ucn(p);
	if (ucn)
		p += ucn;
	else if ((unsigned char)*p >= 0x80)
		p++;
	else if (IS_ALPHA(*p))
		p++;
	else
		return 0;
	for (;;) {
		if (ident_char[(unsigned char)*p]) {
			p++;
			continue;
		}
		ucn = read_ucn(p);
		if (ucn) {
			p += ucn;
			continue;
		}
		break;
	}
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

#define SWAR_HAS_ZERO(v) (((v) - 0x0101010101010101ULL) & ~(v) & 0x8080808080808080ULL)
#define SWAR_BROADCAST(c) (0x0101010101010101ULL * (uint8_t)(c))

/* Phase 2: `\`+newline (and `\r\n`) inside // comments is spliced, so the
 * comment continues on the next physical line. Line numbers still advance. */
static char *skip_line_comment(char *p, TokState *ts) {
	for (;;) {
		while ((uintptr_t)p & 7) {
			if (*p == '\0') return p;
			if (*p == '\\') {
				if (p[1] == '\n') {
					p += 2;
					ts->line_no++;
					continue;
				}
				if (p[1] == '\r' && p[2] == '\n') {
					p += 3;
					ts->line_no++;
					continue;
				}
			}
			if (*p == '\n') return p;
			p++;
		}
		uint64_t nl_mask = SWAR_BROADCAST('\n');
		uint64_t bs_mask = SWAR_BROADCAST('\\');
		for (;;) {
			uint64_t v;
			memcpy(&v, p, 8);
			if (SWAR_HAS_ZERO(v) || SWAR_HAS_ZERO(v ^ nl_mask) || SWAR_HAS_ZERO(v ^ bs_mask)) {
				for (int i = 0; i < 8; i++) {
					if (p[i] == '\0') return p + i;
					if (p[i] == '\\') {
						if (p[i + 1] == '\n') {
							p += i + 2;
							ts->line_no++;
							goto realign;
						}
						if (p[i + 1] == '\r' && p[i + 2] == '\n') {
							p += i + 3;
							ts->line_no++;
							goto realign;
						}
					}
					if (p[i] == '\n') return p + i;
				}
			}
			p += 8;
		}
	realign:;
	}
}

static char *skip_block_comment(char *p, TokState *ts) {
	while ((uintptr_t)p & 7) {
		if (*p == '\0') error_at(p, "unclosed block comment");
		if (*p == '\n') {
			ts->line_no++;
			ts->at_bol = true;
		}
		if (p[0] == '*' && p[1] == '/') return p + 2;
		p++;
	}
	uint64_t nl_mask = SWAR_BROADCAST('\n');
	uint64_t star_mask = SWAR_BROADCAST('*');
	for (;;) {
		uint64_t v;
		memcpy(&v, p, 8);
		if (SWAR_HAS_ZERO(v) || SWAR_HAS_ZERO(v ^ nl_mask) || SWAR_HAS_ZERO(v ^ star_mask)) {
			for (int i = 0; i < 8; i++) {
				if (p[i] == '\0') error_at(p + i, "unclosed block comment");
				if (p[i] == '\n') {
					ts->line_no++;
					ts->at_bol = true;
				}
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
	for (char *q = content; *q; q++) {
		if (*q == '\n') ts->line_no++;
		if (*q == ')' && (delim_len == 0 || strncmp(q + 1, delim_start, delim_len) == 0) &&
		    q[1 + delim_len] == '"') {
			return q + 1 + delim_len + 1;
		}
	}

	error_at(p, "unclosed raw string literal");
}

static inline __attribute__((always_inline)) Token *
new_token(TokenKind kind, char *start, char *end, TokState *ts) {
	File *cf = ctx->current_file;
	if (token_count == UINT32_MAX) error("maximum token limit reached");
	if (__builtin_expect(token_count >= token_cap, 0)) token_pool_ensure(token_count + 1);
	uint32_t token_idx = token_count++;
	Token *tok = &token_pool[token_idx];
	tok->kind = kind;
	tok->len = end - start;
	tok->tag = 0;
	tok->match_idx = 0;
	/* file_idx below comes from this same `cf`, so TF_SYS_SKIP exactly mirrors
	 * the emit-time file predicate without a per-token cold-file dereference. */
	tok->flags = (ts->at_bol ? TF_AT_BOL : 0) | (ts->has_space ? TF_HAS_SPACE : 0) |
			     (cf->skip_emit ? TF_SYS_SKIP : 0);
	tok->ann = 0;
	tok->ch0 = (uint8_t)*start;
	TokenCold *c = &token_cold[token_idx];
	ptrdiff_t off = start - cf->contents;
	if (off < 0 || (size_t)off > (size_t)UINT32_MAX)
		error_at(start, "source file exceeds 4 GiB; cannot record token locations");
	c->loc_offset = (uint32_t)off;
	{
		long long ln = (long long)ts->line_no + cf->line_delta;
		int clamped = ln > 0x1FFFF ? 0x1FFFF : (ln < -0x20000 ? -0x20000 : (int)ln);
		c->line_no = clamped;
	}
	c->file_idx = cf->file_no;
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

static inline void classify_punct(Token *t) {
	char c = t->ch0;
	if (t->len == 1) {
		if (c == '=' || c == '[') t->tag = TT_ASSIGN;
		else if (c == '.')
			t->tag = TT_MEMBER;
		else if (c == '{' || c == '}' || c == ';' || c == ':')
			t->tag = TT_STRUCTURAL;
		if (c == '(' || c == '[' || c == '{') t->flags |= TF_OPEN;
		else if (c == ')' || c == ']' || c == '}')
			t->flags |= TF_CLOSE;
	} else {
		char *loc = tok_loc(t);
		if (t->len == 2) {
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
}

static inline bool delimiters_match(Token *open, Token *close) {
	char a = open->ch0, b = close->ch0;
	return a == '(' ? b == ')' : b == a + 2;
}

static inline bool p0_token_can_name_function(Token *tok) {
	return tok && (tok->kind == TK_IDENT || (tok->tag & (TT_DEFER | TT_ORELSE)) ||
		       (tok->flags & (TF_RAW | TF_SOFT_KW)));
}

static Token *p0_attribute_group_end(Token *tok) {
	if (!tok) return NULL;
	if ((tok->flags & TF_C23_ATTR) && tok_match(tok)) return tok_match(tok);
	if (tok->kind <= TK_KEYWORD &&
	    (equal(tok, "__attribute__") || equal(tok, "__attribute") || equal(tok, "__declspec"))) {
		Token *open = tok_next(tok);
		if (open && open->ch0 == '(' && tok_match(open)) return tok_match(open);
	}
	return NULL;
}

static Token *p0_previous_token(Token *tok) {
	for (uint32_t i = tok_idx(tok); i > 0;) {
		Token *prev = &token_pool[--i];
		if (prev->kind != TK_PREP_DIR) return prev;
	}
	return NULL;
}

static bool p0_soft_noreturn_is_decl_specifier(Token *tok) {
	Token *next = tok_next(tok);
	if (!next || next->kind == TK_EOF ||
	    match_set(next, CH('=') | CH('(') | CH('[') | CH(',') | CH(';') | CH(')')))
		return false;

	bool followed_by_decl =
	    (next->tag & (TT_TYPE | TT_STORAGE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_ATTR |
			  TT_INLINE)) ||
	    (next->flags & TF_C23_ATTR);
	if (!followed_by_decl && p0_token_can_name_function(next) && tok_next(next) &&
	    tok_next(next)->ch0 == '(')
		followed_by_decl = true;
	if (!followed_by_decl && next->kind == TK_IDENT) {
		Token *name = tok_next(next);
		if (p0_token_can_name_function(name) && tok_next(name) && tok_next(name)->ch0 == '(')
			followed_by_decl = true;
	}
	if (!followed_by_decl) return false;

	if (tok_at_bol(tok)) return true;
	Token *prev = p0_previous_token(tok);
	if (!prev) return true;
	if (prev->ch0 == ';' || prev->ch0 == '{' || prev->ch0 == '}') return true;
	if (prev->tag &
	    (TT_TYPE | TT_STORAGE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_ATTR | TT_INLINE))
		return true;
	if (prev->ch0 == ']' && tok_match(prev) && (tok_match(prev)->flags & TF_C23_ATTR)) return true;
	return false;
}

static bool p0_attribute_inside_parameter_list(Token *attr) {
	int depth = 0;
	for (uint32_t i = tok_idx(attr); i > 0; i--) {
		Token *t = &token_pool[i - 1];
		if (t->kind == TK_PREP_DIR) continue;
		if (t->flags & TF_CLOSE) {
			depth++;
			continue;
		}
		if (t->flags & TF_OPEN) {
			if (depth > 0) {
				depth--;
				continue;
			}
			if (match_ch(t, '(') && p0_token_can_name_function(p0_previous_token(t))) return true;
			continue;
		}
		if (depth == 0 && (match_ch(t, ';') || match_ch(t, '{') || match_ch(t, '}'))) break;
	}
	return false;
}

static Token *find_wrapper_callee(Token *body) {
	Token *end = tok_match(body);
	if (!end) return NULL;
	Token *tok = tok_next(body);
	while (tok && tok != end && tok->ch0 == ';') tok = tok_next(tok);
	if (tok && tok != end && (tok->tag & TT_RETURN)) tok = tok_next(tok);
	while (tok && tok != end && tok->ch0 == ';') tok = tok_next(tok);
	if (!tok || tok == end || !p0_token_can_name_function(tok)) return NULL;
	Token *open = tok_next(tok);
	if (!open || open->ch0 != '(' || !tok_match(open)) return NULL;
	Token *after = tok_next(tok_match(open));
	while (after && after != end && after->ch0 == ';') after = tok_next(after);
	return after == end ? tok : NULL;
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
	file->is_direct_system_include = false;
	file->skip_emit = is_system && is_include_entry;
	add_input_file(file);
	return file;
}

// Scan line directive; returns position after it, or NULL if not a line marker.
// Accepts `#`, digraph `%:`, and trigraph `??=` as the directive introducer.
static char *scan_line_directive(char *p, File *base_file, int *line_no, bool *in_system_include) {
	int directive_line = *line_no;
	if (p[0] == '%' && p[1] == ':')
		p += 2;
	else if (p[0] == '?' && p[1] == '?' && p[2] == '=')
		p += 3;
	else if (*p == '#')
		p++;
	else
		return NULL;
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
		int len = 0;
		for (char *s = start; s < start + raw_len; s++) {
			if (*s == '\\' && s + 1 < start + raw_len && (s[1] == '\\' || s[1] == '"')) {
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

	/* Direct system include = first entry into a system file from user code.
	 * Nested system headers (bits/…) must not be re-emitted as #include.
	 * Flag `1 3` on a non-system-looking path must not sticky-skip the TU. */
	if (is_entering && is_system && filename) {
		const char *f = filename;
		bool looks_system =
		    strncmp(f, "/usr/include/", 13) == 0 || strncmp(f, "/usr/local/include/", 19) == 0 ||
		    strncmp(f, "/Library/", 9) == 0 || strncmp(f, "/Applications/Xcode", 19) == 0 ||
		    (strstr(f, "/lib/gcc/") && strstr(f, "/include/")) ||
		    (strstr(f, "/lib/clang/") && strstr(f, "/include")) || strstr(f, "Windows Kits") ||
		    strstr(f, "Program Files");
		if (!looks_system) is_system = false;
	}
	bool direct_system = is_entering && is_system && !*in_system_include;
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
	// Flag `3` alone (`# N "sys.h" 3`) is a GCC/Clang system location update —
	// must NOT be treated as MSVC or `/usr/lib/clang/…` paths clear
	// `in_system_include` and nested headers get re-emitted.
	bool msvc_style = !is_entering && !is_returning && !is_system;
	File *view;
	if (msvc_style && filename) {
		const char *f = filename;
		if (strncmp(f, "/usr/include/", 13) == 0 || strncmp(f, "/usr/local/include/", 19) == 0 ||
		    strncmp(f, "/Library/", 9) == 0 || strncmp(f, "/Applications/Xcode", 19) == 0 ||
		    (strstr(f, "/lib/gcc/") && strstr(f, "/include/")) ||
		    (strstr(f, "/lib/clang/") && strstr(f, "/include")) || strstr(f, "Windows Kits") ||
		    strstr(f, "Program Files")) {
			direct_system = !*in_system_include;
			is_system = true;
			*in_system_include = true;
		} else if (*f == '/' || *f == '.' || (f[0] && f[1] == ':')) {
			is_system = false;
			*in_system_include = false;
		}
	}
	/* `# N "user.c" 2 3` — return with a spurious system flag on a non-system
	 * path must not sticky-skip the rest of the TU (TF_SYS_SKIP). */
	if (is_returning && is_system && filename) {
		const char *f = filename;
		bool looks_system =
		    strncmp(f, "/usr/include/", 13) == 0 || strncmp(f, "/usr/local/include/", 19) == 0 ||
		    strncmp(f, "/Library/", 9) == 0 || strncmp(f, "/Applications/Xcode", 19) == 0 ||
		    (strstr(f, "/lib/gcc/") && strstr(f, "/include/")) ||
		    (strstr(f, "/lib/clang/") && strstr(f, "/include")) || strstr(f, "Windows Kits") ||
		    strstr(f, "Program Files");
		if (!looks_system) {
			is_system = false;
			*in_system_include = false;
		}
	}
	view = new_file_view(filename ? filename : ctx->current_file->name,
			     base_file,
			     line_delta,
			     is_system,
			     *in_system_include);
	view->is_direct_system_include = direct_system;
	ctx->current_file = view;
	free(filename);
	while (*p && *p != '\n') p++;
	if (*p == '\n') {
		p++;
		(*line_no)++;
	}
	return p;
}

static char *scan_pp_number(char *p, bool *is_float) {
	bool is_hex = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'));
	bool float_lit = false;
	for (;;) {
		char c = *p;
		if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') && (p[1] == '+' || p[1] == '-')) {
			if (c == 'p' || c == 'P' || !is_hex) float_lit = true;
			p += 2;
		} else if (c == '.') {
			float_lit = true;
			p++;
		} else if (ident_char[(unsigned char)c]) {
			p++;
		} else if (c == '\'' && ident_char[(unsigned char)p[1]]) {
			p++;
		} else
			break;
	}
	if (is_float) *is_float = float_lit;
	return p;
}

static Token *tokenize(File *file) {
	File *base_file = file;
	ctx->current_file = file;
	char *p = file->contents;
	/* Skip leading UTF-8 BOM (EF BB BF). UTF-16 BOMs are rejected earlier. */
	if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
		p += 3;
	token_pool_ensure(token_count + file->contents_len / 2 + 4096);
	uint32_t first_idx = token_count;
	TokState ts = {true, false, 1};
	bool in_system_include = false;
	bool saw_taint_kw = false, saw_asm_kw = false, saw_attr_kw = false, saw_noreturn_kw = false;
	while (*p) {
		if (ts.at_bol &&
		    (*p == '#' || (p[0] == '%' && p[1] == ':') ||
		     (p[0] == '?' && p[1] == '?' && p[2] == '='))) {
			char *directive_start = p;
			char *after = scan_line_directive(p, base_file, &ts.line_no, &in_system_include);
			if (after) {
				p = after;
				ts.at_bol = true;
				ts.has_space = false;
				continue;
			}
			while (*p && *p != '\n') p++;
			new_token(TK_PREP_DIR, directive_start, p, &ts)->flags |= TF_AT_BOL;
			if (*p == '\n') {
				p++;
				ts.line_no++;
				ts.at_bol = true;
				ts.has_space = false;
			}
			continue;
		}

		if (p[0] == '/' && p[1] == '/') {
			p = skip_line_comment(p + 2, &ts);
			ts.has_space = true;
			continue;
		}
		if (p[0] == '/' && p[1] == '*') {
			p = skip_block_comment(p + 2, &ts);
			ts.has_space = true;
			continue;
		}
		if (*p == '\n' || is_space(*p)) {
			do {
				if (*p == '\n') {
					ts.line_no++;
					ts.at_bol = true;
					ts.has_space = false;
				} else
					ts.has_space = true;
				p++;
			} while (*p == '\n' || is_space(*p));
			continue;
		}
		/* Fast path: the vast majority of tokens are identifiers/keywords that
		 * do NOT start with a string/char literal prefix (u/U/L/R). Jump
		 * straight to identifier scanning, skipping ~15 literal-prefix branches.
		 * u/U/L/R starts fall through so `u8"..."`, `L'x'`, `R"..."` still work. */
		{
			unsigned char c0 = (unsigned char)*p;
			if (__builtin_expect((IS_ALPHA(c0) || c0 >= 0x80) && c0 != 'u' && c0 != 'U' &&
						 c0 != 'L' && c0 != 'R',
					     1))
				goto do_ident;
		}
		if (IS_DIGIT(*p) || (*p == '.' && IS_DIGIT(p[1]))) {
			char *start = p;
			p = scan_pp_number(p, NULL);
			new_token(TK_NUM, start, p, &ts);
			continue;
		}
		{ // Raw string literals
			int raw_pfx = (p[0] == 'R')						     ? 0
				      : (p[0] == 'u' && p[1] == '8' && p[2] == 'R')		     ? 2
				      : ((p[0] == 'L' || p[0] == 'u' || p[0] == 'U') && p[1] == 'R') ? 1
												     : -1;
			if (raw_pfx >= 0 && p[raw_pfx] == 'R' && p[raw_pfx + 1] == '"') {
				Token *nt = read_raw_string_literal(p, p + raw_pfx + 1, &ts);
				p += nt->len;
				continue;
			}
		}
		if (*p == '"') {
			Token *nt = read_string_literal(p, p, &ts);
			p += nt->len;
			continue;
		}
		if ((p[0] == 'u' && p[1] == '8' && p[2] == '"') ||
		    ((p[0] == 'u' || p[0] == 'U' || p[0] == 'L') && p[1] == '"')) {
			char *start = p;
			p += (p[0] == 'u' && p[1] == '8') ? 2 : 1;
			Token *nt = read_string_literal(start, p, &ts);
			p = start + nt->len;
			continue;
		}
		if (*p == '\'') {
			Token *nt = read_char_literal(p, p, &ts);
			p += nt->len;
			continue;
		}
		if (p[0] == 'u' && p[1] == '8' && p[2] == '\'') {
			Token *nt = read_char_literal(p, p + 2, &ts);
			p += nt->len;
			continue;
		}
		if ((p[0] == 'u' || p[0] == 'U' || p[0] == 'L') && p[1] == '\'') {
			Token *nt = read_char_literal(p, p + 1, &ts);
			p += nt->len;
			continue;
		}
	do_ident:;
		int ident_len = read_ident(p);
		if (ident_len) {
			Token *t = new_token(TK_IDENT, p, p + ident_len, &ts);
			t->parse_data = fast_hash(p, (uint32_t)ident_len);
			uint64_t kw = keyword_lookup(p, ident_len);
			if (kw) {
				if (kw & KW_MARKER) {
					t->kind = TK_KEYWORD;
					t->tag = (uint32_t)(kw & ~KW_MARKER);
				} else
					t->tag = (uint32_t)kw;
				t->flags |= (uint16_t)(kw >> KW_FLAGS_SHIFT);
				if (t->tag & (TT_SPECIAL_FN | TT_NORETURN_FN)) saw_taint_kw = true;
				if (t->tag & TT_ASM) saw_asm_kw = true;
				if (t->tag & TT_ATTR) saw_attr_kw = true;
				if ((t->tag & TT_INLINE) && (t->tag & TT_SKIP_DECL) &&
				    (equal(t, "_Noreturn") || equal(t, "noreturn")))
					saw_noreturn_kw = true;
			}
			p += ident_len;
			continue;
		}
		int punct_len = read_punct(p);
		if (punct_len) {
			int abs_len = punct_len < 0 ? -punct_len : punct_len;
			Token *t = new_token(TK_PUNCT, p, p + abs_len, &ts);
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

	new_token(TK_EOF, p, p, &ts);

	Token *first = first_idx ? &token_pool[first_idx] : NULL;
	// Also detect C23 [[ ... ]] attributes and tag the first '[' with TF_C23_ATTR.
	{
		int stack_cap = 256;
		Token **stack = arena_alloc_uninit(&ctx->main_arena, stack_cap * sizeof(Token *));
		int sp = 0;
		uint32_t last_prism_idx = 0;
		for (Token *t = first; t && t->kind != TK_EOF; t = tok_next(t)) {
			if (t->tag & (TT_DEFER | TT_ORELSE)) last_prism_idx = tok_idx(t);
			if (t->flags & TF_OPEN) {
				ARENA_ENSURE_CAP(
				    &ctx->main_arena, stack, sp + 1, stack_cap, 256, Token *);
				stack[sp++] = t;
				Token *tn = tok_next(t);
				if (t->ch0 == '[' && tn && tn->ch0 == '[' && (tn->flags & TF_OPEN)) {
					t->flags |= TF_C23_ATTR;
					saw_attr_kw = true;
				}
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
				if (last_prism_idx > tok_idx(open)) open->flags |= TF_HAS_PRISM;
			}
		}
		if (sp > 0) error_tok(stack[sp - 1], "unclosed delimiter '%c'", stack[sp - 1]->ch0);
		/* User definitions of libc noreturn/special names are ordinary unless
		 * explicitly annotated (_Noreturn / attr). Clear keyword-cache tags so
		 * taint + auto-unreachable do not treat `int exit(void){…}` as builtin. */
		{
			HashMap user_builtin = {0};
			Token *fname = NULL;
			int depth = 0;
			for (Token *t = first; t && t->kind != TK_EOF; t = tok_next(t)) {
				if ((t->flags & TF_OPEN) && match_ch(t, '{')) {
					if (depth == 0 && fname &&
					    (fname->tag & (TT_NORETURN_FN | TT_SPECIAL_FN))) {
						Token *np = tok_next(fname);
						if (np && match_ch(np, '(') && tok_match(np)) {
							Token *after = tok_next(tok_match(np));
							/* Allow noise/attrs between ) and { */
							while (after && after != t && after->kind != TK_EOF) {
								Token *ae = p0_attribute_group_end(after);
								if (ae) {
									after = tok_next(ae);
									continue;
								}
								if (after->kind == TK_PREP_DIR) {
									after = tok_next(after);
									continue;
								}
								break;
							}
							if (after == t)
								lex_token_map_put(&user_builtin, fname, (void *)1);
						}
					}
					depth++;
					fname = NULL;
					continue;
				}
				if ((t->flags & TF_CLOSE) && match_ch(t, '}')) {
					if (depth > 0) depth--;
					continue;
				}
				if (depth != 0) continue;
				if (match_ch(t, ';')) {
					fname = NULL;
					continue;
				}
				if (is_potential_func_name(t)) fname = t;
			}
			if (user_builtin.used > 0) {
				for (Token *s = first; s && s->kind != TK_EOF; s = tok_next(s)) {
					if ((s->tag & (TT_NORETURN_FN | TT_SPECIAL_FN)) &&
					    lex_token_map_get(&user_builtin, s))
						s->tag &= ~(TT_NORETURN_FN | TT_SPECIAL_FN);
				}
			}
			hashmap_discard(&user_builtin);
		}

		// Pre-scan function bodies: tag '{' with TT_SPECIAL_FN / TT_ASM / TT_NORETURN_FN(=vfork).
		// Propagate special-function taint transitively through wrapper chains.
		if (saw_taint_kw || saw_asm_kw) {
			typedef struct {
				Token *name;
				Token *body;
			} FunctionScan;

			FunctionScan *functions = NULL;
			int function_count = 0;
			int function_capacity = 0;
			Token *func_name = NULL;
			for (Token *t = first; t && t->kind != TK_EOF; t = tok_next(t)) {
				Token *attr_end = p0_attribute_group_end(t);
				if (attr_end) {
					t = attr_end;
					continue;
				}
				if (is_potential_func_name(t)) func_name = t;
				if (t->ch0 == '{' && (t->flags & TF_OPEN) && t->match_idx) {
					Token *end = tok_match(t);
					for (Token *b = tok_next(t); b != end; b = tok_next(b)) {
						if ((b->tag & TT_SPECIAL_FN) &&
						    !(tok_idx(b) >= 1 &&
						      (token_pool[tok_idx(b) - 1].tag & TT_MEMBER))) {
							/* Skip declarator shadows: `void (*setjmp)(void)` /
							 * `int vfork;` — not calls/refs. */
							Token *prev =
							    tok_idx(b) >= 1 ? &token_pool[tok_idx(b) - 1]
									    : NULL;
							Token *nxt = tok_next(b);
							bool decl_shadow =
							    prev &&
							    (match_ch(prev, '*') ||
							     (prev->tag &
							      (TT_TYPE | TT_QUALIFIER | TT_STORAGE |
							       TT_SUE))) &&
							    nxt && nxt->ch0 != '(';
							if (!decl_shadow) {
								if (b->ch0 == 'v' && b->len == 5) {
									t->tag |= TT_NORETURN_FN;
								} else
									t->tag |= TT_SPECIAL_FN;
							}
						}
						if (b->tag & TT_ASM) {
							/* GNU `asm [qualifiers] goto (` — require
							 * `goto` before the asm operand `(`, and
							 * only allow asm-header tokens in between.
							 * Stop on any other token so soft-kw
							 * identifier uses like `if (asm) goto L`
							 * are not mistaken for asm-goto. */
							bool saw_goto = false;
							for (Token *ag = tok_next(b);
							     ag && ag != end;
							     ag = tok_next(ag)) {
								if (ag->ch0 == '(') {
									if (saw_goto) t->tag |= TT_ASM;
									break;
								}
								if (ag->tag & TT_GOTO) {
									saw_goto = true;
									continue;
								}
								/* asm volatile/inline/goto attrs */
								if (ag->tag &
								    (TT_QUALIFIER | TT_INLINE | TT_ATTR))
									continue;
								if (equal(ag, "__volatile__") ||
								    equal(ag, "__inline__") ||
								    equal(ag, "volatile") ||
								    equal(ag, "inline"))
									continue;
								if (ag->flags & TF_SOFT_KW) continue;
								if ((ag->flags & TF_C23_ATTR) &&
								    tok_match(ag)) {
									ag = tok_match(ag);
									continue;
								}
								break;
							}
						}
					}
					if (func_name) {
						ARENA_ENSURE_CAP(&ctx->main_arena,
								 functions,
								 function_count + 1,
								 function_capacity,
								 32,
								 FunctionScan);
						functions[function_count++] =
						    (FunctionScan){.name = func_name, .body = t};
					}
					func_name = NULL;
					t = end;
				}
			}

			uint32_t *wrapper_taint = NULL;
			int *callee_idx = NULL;
			HashMap func_map = {0};
			if (function_count > 0) {
				wrapper_taint = arena_alloc(&ctx->main_arena,
							    (size_t)function_count * sizeof(*wrapper_taint));
				callee_idx = arena_alloc(&ctx->main_arena,
							 (size_t)function_count * sizeof(*callee_idx));
				for (int i = 0; i < function_count; i++) {
					lex_token_map_put(
					    &func_map, functions[i].name, (void *)(intptr_t)(i + 1));
				}
				for (int i = 0; i < function_count; i++) {
					callee_idx[i] = -1;
					Token *callee = find_wrapper_callee(functions[i].body);
					if (!callee) continue;
					// TT_SPECIAL_FN in the body taints with TT_SPECIAL_FN
					// Pass 2 still emits a per-call-site warning at the
					if (callee->tag & TT_SPECIAL_FN) {
						wrapper_taint[i] =
						    (callee->ch0 == 'v' && callee->len == 5)
							? TT_NORETURN_FN // vfork wrapper
							: TT_SPECIAL_FN; // setjmp/longjmp/pthread_exit wrapper
						continue;
					}
					void *v = lex_token_map_get(&func_map, callee);
					if (v) callee_idx[i] = (int)(intptr_t)v - 1;
				}

				/* Taint flows callee → caller along the wrapper chain. */
				int *caller_head = arena_alloc(&ctx->main_arena,
							       (size_t)function_count * sizeof(int));
				int *caller_next = arena_alloc(&ctx->main_arena,
							       (size_t)function_count * sizeof(int));
				int *queue = arena_alloc(&ctx->main_arena,
							 (size_t)function_count * sizeof(int));
				for (int i = 0; i < function_count; i++) caller_head[i] = -1;
				for (int i = 0; i < function_count; i++) {
					int j = callee_idx[i];
					if (j < 0) continue;
					caller_next[i] = caller_head[j];
					caller_head[j] = i;
				}
				int qh = 0, qt = 0;
				for (int i = 0; i < function_count; i++)
					if (wrapper_taint[i]) queue[qt++] = i;
				while (qh < qt) {
					int j = queue[qh++];
					for (int i = caller_head[j]; i >= 0; i = caller_next[i]) {
						if (wrapper_taint[i]) continue;
						wrapper_taint[i] = wrapper_taint[j];
						queue[qt++] = i;
					}
				}
			}

			bool has_taint = false;
			for (int i = 0; i < function_count; i++) {
				if (wrapper_taint[i] ||
				    (functions[i].body->tag & (TT_SPECIAL_FN | TT_NORETURN_FN))) {
					has_taint = true;
					break;
				}
			}
			if (has_taint) {
				typedef struct {
					int from, to;
				} TaintEdge;

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
						if (!p0_token_can_name_function(b)) continue;
						/* Skip declarator occurrences (`void (*f0)(void)`,
						 * `int f0;`) — keep bare refs so FP chains like
						 * `fp = f0; fp();` still propagate taint. */
						if (tok_idx(b) >= 1) {
							Token *prev = &token_pool[tok_idx(b) - 1];
							Token *n = tok_next(b);
							if ((match_ch(prev, '*') ||
							     (prev->tag & (TT_TYPE | TT_QUALIFIER |
									   TT_STORAGE | TT_SUE))) &&
							    n && n->ch0 != '(')
								continue;
						}
						if (!(fn_bloom &
						      (1ULL << (((unsigned)b->ch0 ^ b->len) & 63))))
							continue;
						if (tok_idx(b) > tok_idx(body) + 1) {
							Token *prev = &token_pool[tok_idx(b) - 1];
							if (prev->tag &
							    (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_SUE))
								continue;
							if (match_ch(prev, ')') && (prev->flags & TF_CLOSE) &&
							    prev->match_idx) {
								Token *open = tok_match(prev);
								Token *inner = open ? tok_next(open) : NULL;
								if (inner &&
								    (inner->tag &
								     (TT_TYPE | TT_QUALIFIER | TT_SUE)))
									continue;
							}
						}
						void *v = lex_token_map_get(&func_map, b);
						if (!v) continue;
						int j = (int)(intptr_t)v - 1;
						VEC_ENSURE_REALLOC(edges, edge_count + 1, edge_cap, 64);
						edges[edge_count++] = (TaintEdge){i, j};
					}
				}
				/* Body taint: call edge i→j propagates j's taint onto i. */
				if (edge_count > 0) {
					int *edge_head = arena_alloc(&ctx->main_arena,
								     (size_t)function_count * sizeof(int));
					int *edge_next = arena_alloc(&ctx->main_arena,
								     (size_t)edge_count * sizeof(int));
					int *queue = arena_alloc(&ctx->main_arena,
								 (size_t)function_count * sizeof(int));
					uint8_t *queued = arena_alloc(&ctx->main_arena,
								      (size_t)function_count);
					for (int i = 0; i < function_count; i++) {
						edge_head[i] = -1;
						queued[i] = 0;
					}
					for (int e = 0; e < edge_count; e++) {
						int j = edges[e].to;
						edge_next[e] = edge_head[j];
						edge_head[j] = e;
					}
					int qh = 0, qt = 0;
					for (int j = 0; j < function_count; j++) {
						if (wrapper_taint[j] ||
						    (functions[j].body->tag & TT_NORETURN_FN)) {
							queue[qt++] = j;
							queued[j] = 1;
						}
					}
					while (qh < qt) {
						int j = queue[qh++];
						for (int e = edge_head[j]; e >= 0; e = edge_next[e]) {
							int i = edges[e].from;
							Token *body = functions[i].body;
							if (body->tag & (TT_SPECIAL_FN | TT_NORETURN_FN))
								continue;
							uint32_t before = body->tag;
							if (wrapper_taint[j]) body->tag |= wrapper_taint[j];
							if (functions[j].body->tag & TT_NORETURN_FN)
								body->tag |= TT_NORETURN_FN;
							if (body->tag == before) continue;
							if (!queued[i]) {
								queue[qt++] = i;
								queued[i] = 1;
							}
						}
					}
				}
				free(edges);
			} // has_taint
		}
	}

	// When a noreturn specifier is found before a function declaration,
	if (saw_noreturn_kw || saw_attr_kw) {
		HashMap nr_map = {0};
#define SKIP_ATTR_ARGS(a)                                                                                    \
	do {                                                                                                 \
		if ((a)->kind <= TK_KEYWORD && tok_next(a) && tok_next(a)->ch0 == '(' &&                     \
		    tok_next(a)->match_idx)                                                                  \
			(a) = &token_pool[tok_next(a)->match_idx];                                           \
	} while (0)
#define IS_NORETURN_NAME(a)                                                                                  \
	((a)->kind <= TK_KEYWORD &&                                                                          \
	 (equal((a), "noreturn") || equal((a), "_Noreturn") || equal((a), "__noreturn__")))
#define ATTR_SPAN_HAS_NORETURN(start, end, out)                                                              \
	do {                                                                                                 \
		for (Token *_a = (start); _a && _a < (end); _a = tok_next(_a)) {                             \
			if (IS_NORETURN_NAME(_a)) {                                                          \
				(out) = true;                                                                \
				break;                                                                       \
			}                                                                                    \
			SKIP_ATTR_ARGS(_a);                                                                  \
		}                                                                                            \
	} while (0)
		for (Token *t = first; t && t->kind != TK_EOF; t = tok_next(t)) {
			bool is_noreturn = false;
			bool attribute_form = false;
			Token *scan_start = t;
			Token *attr_origin = t; // original position for backward scan

			if (t->kind <= TK_KEYWORD &&
			    (equal(t, "_Noreturn") ||
			     (equal(t, "noreturn") && p0_soft_noreturn_is_decl_specifier(t))))
				is_noreturn = true;
			// [[noreturn]] / [[_Noreturn]] / [[__noreturn__]] — C23 attribute
			if (t->ch0 == '[' && (t->flags & TF_C23_ATTR) && t->match_idx) {
				attribute_form = true;
				Token *inner = tok_next(t);
				Token *attr_end = &token_pool[t->match_idx];
				if (inner && inner->ch0 == '[')
					ATTR_SPAN_HAS_NORETURN(tok_next(inner), attr_end, is_noreturn);
				t = attr_end; // advance past [[ ... ]]
				scan_start = t;
			}

			if (t->kind <= TK_KEYWORD &&
			    (equal(t, "__attribute__") || equal(t, "__attribute"))) {
				attribute_form = true;
				Token *p1 = tok_next(t);
				if (p1 && p1->ch0 == '(') {
					Token *p2 = tok_next(p1);
					if (p2 && p2->ch0 == '(' && p2->match_idx) {
						Token *close = &token_pool[p2->match_idx];
						ATTR_SPAN_HAS_NORETURN(tok_next(p2), close, is_noreturn);
						t = tok_match(p1); // advance past __attribute__(( ... ))
						scan_start = t;
					}
				}
			}

			// __declspec(noreturn) or __declspec(__noreturn__) — MSVC
			if (t->kind <= TK_KEYWORD && equal(t, "__declspec")) {
				attribute_form = true;
				Token *p1 = tok_next(t);
				if (p1 && p1->ch0 == '(' && p1->match_idx) {
					Token *close = &token_pool[p1->match_idx];
					ATTR_SPAN_HAS_NORETURN(tok_next(p1), close, is_noreturn);
					t = close; // advance past __declspec( ... )
					scan_start = t;
				}
			}

			if (!is_noreturn) continue;
			if (attribute_form && p0_attribute_inside_parameter_list(attr_origin))
				continue;
			Token *fn_name = NULL;
			/* Post-declarator attrs (`void die(void) __attribute__((noreturn)), live`)
			 * must bind to the preceding name. Prefer backward when the token
			 * before the attr is `)` so a following declarator is not tagged. */
			bool post_decl_attr = false;
			if (attribute_form) {
				Token *before = tok_walk_back(tok_idx(attr_origin), WB_ATTR_NOISE);
				post_decl_attr = before && match_ch(before, ')');
			}
			if (post_decl_attr) {
				for (uint32_t pi = tok_idx(attr_origin); pi > 0; pi--) {
					Token *pt = &token_pool[pi - 1];
					if (pt->kind == TK_PREP_DIR) continue;
					if (pt->ch0 == ';' || pt->ch0 == '{' || pt->ch0 == '}') break;
					if ((pt->ch0 == ')' || pt->ch0 == ']') && tok_match(pt)) {
						pi = tok_idx(tok_match(pt)) + 1;
						continue;
					}
					if (p0_token_can_name_function(pt) && tok_next(pt) &&
					    tok_next(pt)->ch0 == '(') {
						if (pt->tag & (TT_SKIP_DECL | TT_INLINE | TT_QUALIFIER |
							       TT_TYPE | TT_STORAGE))
							continue;
						fn_name = pt;
						break;
					}
				}
			}
			if (!fn_name) {
				int fwd_depth = 0;
				for (Token *s = scan_start; s && s->kind != TK_EOF; s = tok_next(s)) {
					char ch = s->ch0;
					if (ch == ';' || ch == '{') break;
					/* Post-declarator attrs bind to the preceding name only. */
					if (fwd_depth == 0 && ch == ',' && post_decl_attr) break;
					Token *attr_end = p0_attribute_group_end(s);
					if (attr_end) {
						s = attr_end;
						continue;
					}
					if ((s->flags & TF_OPEN) && tok_match(s)) {
						fwd_depth++;
						continue;
					}
					if ((s->flags & TF_CLOSE) && fwd_depth > 0) {
						fwd_depth--;
						continue;
					}
					if (fwd_depth == 0 && p0_token_can_name_function(s) &&
					    tok_next(s) && tok_next(s)->ch0 == '(') {
						if (s->tag & (TT_SKIP_DECL | TT_INLINE | TT_QUALIFIER |
							      TT_TYPE | TT_STORAGE))
							continue;
						if (post_decl_attr) {
							fn_name = s;
							break;
						}
						/* Prefix _Noreturn / [[noreturn]] / leading attr:
						 * tag every declarator in the list. */
						lex_token_map_put(&nr_map, s, (void *)1);
						fn_name = s;
					}
				}
			}
			if (!fn_name && !post_decl_attr) {
				for (uint32_t pi = tok_idx(attr_origin); pi > 0; pi--) {
					Token *pt = &token_pool[pi - 1];
					if (pt->kind == TK_PREP_DIR) continue;
					if (pt->ch0 == ';' || pt->ch0 == '{' || pt->ch0 == '}') break;
					if ((pt->ch0 == ')' || pt->ch0 == ']') && tok_match(pt)) {
						pi = tok_idx(tok_match(pt)) + 1;
						continue;
					}
					if (p0_token_can_name_function(pt) && tok_next(pt) &&
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
			if (post_decl_attr)
				lex_token_map_put(&nr_map, fn_name, (void *)1);
			else if (!lex_token_map_get(&nr_map, fn_name))
				lex_token_map_put(&nr_map, fn_name, (void *)1);
		}

		if (nr_map.used > 0) {
			uint64_t nr_bloom = 0;
			for (int i = 0; i < nr_map.capacity; i++) {
				HashEntry *ent = &nr_map.buckets[i];
				if (ent->key && ent->key != TOMBSTONE)
					nr_bloom |= 1ULL
						    << (((unsigned char)ent->key[0] ^ ent->key_len) & 63);
			}
			for (Token *s = first; s && s->kind != TK_EOF; s = tok_next(s)) {
				if (p0_token_can_name_function(s) &&
				    (nr_bloom & (1ULL << (((unsigned)s->ch0 ^ s->len) & 63))) &&
				    !(tok_idx(s) >= 1 && (token_pool[tok_idx(s) - 1].tag & TT_MEMBER)) &&
				    lex_token_map_get(&nr_map, s))
					s->tag |= TT_NORETURN_FN;
			}
		}
#undef SKIP_ATTR_ARGS
#undef IS_NORETURN_NAME
#undef ATTR_SPAN_HAS_NORETURN
	}

	return first;
}

static void ensure_keyword_cache(void) {
	if (!keyword_cache[0].name && !keyword_cache[1].name) init_keyword_map();
	else if (ctx->keyword_cache_features != ctx->features)
		init_keyword_map();
}

static inline Token *finalize_load(char *name, char *buf) {
	File *file = arena_alloc(&ctx->main_arena, sizeof(File));
	*file = (File){.name = intern_filename(name),
		       .contents = buf,
		       .contents_len = strlen(buf),
		       .file_no = ctx->input_file_count,
		       .owns_contents = true};
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
		hFile = CreateFileW(
		    wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	else
		hFile = CreateFileA(
		    path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
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
	if (!buf) {
		CloseHandle(hFile);
		return NULL;
	}
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
	if (size > (size_t)UINT32_MAX) {
		close(fd);
		error("source file '%s' exceeds 4 GiB", path);
	}
	if (size == 0) {
		close(fd);
		char *buf = malloc(8);
		if (!buf) return NULL;
		memset(buf, 0, 8);
		return finalize_load(path, buf);
	}
	char *buf = malloc(size + 8);
	if (!buf) {
		close(fd);
		return NULL;
	}
	size_t total = 0;
	while (total < size) {
		ssize_t n = read(fd, buf + total, size - total);
		if (n <= 0) {
			free(buf);
			close(fd);
			return NULL;
		}
		total += (size_t)n;
	}
	close(fd);
	memset(buf + size, 0, 8);
	return finalize_load(path, buf);
#endif
}

// Used by both Pass 1 (analysis) and Pass 2 (emission) in prism.c.

typedef struct {
	Token *end;	   // First token after the type specifier
	bool saw_type : 1; // True if a type was recognized
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
	bool has_void : 1; // True if void or void typedef
	bool has_raw : 1;  // True if 'raw' keyword was skipped in type specifier
	bool has_extern : 1;
	bool has_static : 1;
	bool has_auto : 1;	      // C23 'auto' type inference
	bool has_constexpr : 1;	      // C23 'constexpr'
	bool has_thread_local : 1;    // _Thread_local, thread_local, __thread
	bool has_volatile_member : 1; // Struct/union has volatile-qualified fields
	bool has_alignas : 1;	      // _Alignas(...) / alignas(...) appeared in the type
	bool is_array : 1;	 // Array type from typeof()/typeof_unqual/_Atomic(...) (not declarator [])
	bool type_vm : 1;	 // Any VM dimension in typeof/_Atomic parens (incl. ptr-to-VLA)
	uint8_t type_array_rank; // Dimension count for is_array (multi-dim typeof)
} TypeSpecResult;

typedef struct {
	Token *end; // First token after declarator
	Token *var_name;
	bool is_pointer : 1;
	bool is_array : 1;
	bool is_vla : 1;
	bool is_func_ptr : 1;
	bool is_func_decl : 1;
	bool has_paren : 1;
	bool paren_pointer : 1; // Has pointer (*) inside parenthesized declarator
	bool paren_array : 1;
	bool has_init : 1;
	bool is_const : 1;
} DeclResult;

typedef enum {
	TDK_TYPEDEF,
	TDK_SHADOW,
	TDK_ENUM_CONST,
	TDK_VLA_VAR,   // VLA variable (not typedef, but actual VLA array variable)
	TDK_STRUCT_TAG // struct/union tag (for VLA/volatile member propagation)
} TypedefKind;

typedef struct {
	char *name;		  // Points into token stream (no alloc needed)
	int prev_index;		  // Index of previous entry with same name (-1 if none)
	uint32_t token_index;	  // Token pool index of the declaration
	uint32_t scope_open_idx;  // Token index of enclosing '{' (0 for file scope)
	uint32_t scope_close_idx; // Token index of matching '}' (UINT32_MAX for file scope)
	uint16_t len;
	uint16_t scope_depth; // Scope where defined (aligns with ctx->block_depth)
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
	bool is_atomic : 1;	     // _Atomic(...) spelling baked into this typedef name
	bool is_struct_tag : 1;	     // struct/union tag (not a typedef name)
	bool array_dim_complete : 1; // array typedef: sizeof(T)/sizeof(T[0]) valid at uses
	uint8_t array_rank;	     // # of array dimensions (0 if not array);
} TypedefEntry;			     // 32 bytes — two entries per 64-byte cache line

#define ARRAY_RANK_WRAP_ALL 255

typedef struct {
	TypedefEntry *entries;
	int count;
	int capacity;
	HashMap name_map; // Maps name → (entry_index + 1) as void*, 0 = absent. Chain via prev_index.
	uint64_t bloom;	  // Bloom filter: bit (ch0 ^ len) & 63. Fast negative lookup.
} TypedefTable;

enum {
	P1_IS_TYPEDEF = 1 << 0,	  // Token resolves to a real typedef at this position
	P1_SCOPE_LOOP = 1 << 1,	  // This '{' opens a loop body
	P1_SCOPE_SWITCH = 1 << 2, // This '{' opens a switch body
	P1_HAS_ENTRY = 1 << 3,	  // Token has any typedef-table entry (typedef/enum/shadow/VLA)
	P1_OE_BRACKET = 1 << 4,	  // orelse inside array dimension brackets
	P1_OE_DECL_INIT = 1 << 5, // orelse inside declaration initializer
	P1_IS_DECL = 1 << 6,	  // Phase 1D: token starts a variable declaration
	P1_SCOPE_INIT = 1 << 7,	  // This '{' opens an initializer (compound literal, = {...})
	P1_RAW_BLOCK = 1 << 8,	  // This '{' opens a `raw { ... }` suppress block
	P1_DECL_BRACKET = 1 << 9, // '[' is an array-declarator bracket (not an expression subscript)
	P1_UNEVAL_BRACKET =
	    1 << 10, // '[' is inside an unevaluated operand (sizeof/_Alignof/typeof/offsetof/etc.)
	P1_IS_ORELSE_KW = 1 << 11, // Pass 1: this orelse token is the Prism keyword (not an ident)
	P1_IN_ATTR_ARGS = 1 << 12, // token is inside a GNU __attribute__((...)) / __declspec(...) group
	/* Cache for p1_sue_body_brace_zero_unsafe on `{` (uint16_t ann: bits 13–14). */
	P1_ZUNSAFE_KNOWN = 1 << 13, // brace body zero-unsafe result is cached
	P1_ZUNSAFE = 1 << 14,	    // cached result: body rejects `= {0}`
	P1_IS_DEFER_KW = 1 << 15,   // Pass 1: this defer token is the Prism keyword
};

#define tok_ann(t) ((t)->ann)

enum {
	TDF_TYPEDEF = 1,
	TDF_VLA = 2,
	TDF_VOID = 4,
	TDF_ENUM_CONST = 8,
	TDF_CONST = 16,
	TDF_PTR = 32,
	TDF_ARRAY = 64,
	TDF_AGGREGATE = 128,
	TDF_FUNC = 256,
	TDF_PARAM = 512,
	TDF_VOLATILE = 1024,
	TDF_HAS_VOL_MEMBER = 2048,
	TDF_UNION = 4096,
	TDF_ATOMIC = 8192,
};

// Spread a typedef's TDF_* flag bag onto a TypeSpecResult.is_typedef path.
// Two parse_type_specifier sites need this; centralizing keeps them in
static inline void typedef_apply_tdf_flags(TypeSpecResult *r, int tflags) {
	r->is_typedef = true;
	if (tflags & TDF_VLA) r->is_vla = true;
	if (tflags & TDF_AGGREGATE) r->is_struct = true;
	if (tflags & TDF_UNION) r->is_union = true;
	if (tflags & TDF_VOLATILE) r->has_volatile = true;
	if (tflags & TDF_HAS_VOL_MEMBER) r->has_volatile_member = true;
	if (tflags & TDF_ATOMIC) r->has_atomic = true;
}

#define FEAT(f) (ctx->features & (f))

static PRISM_THREAD_LOCAL TypedefTable typedef_table;

static PRISM_THREAD_LOCAL uint32_t td_scope_open = 0;
static PRISM_THREAD_LOCAL uint32_t td_scope_close = UINT32_MAX;
static PRISM_THREAD_LOCAL bool
    p1_typedef_annotated; // true after p1_annotate_typedefs(); enables O(1) is_known_typedef
static PRISM_THREAD_LOCAL HashMap c_function_symbols;
/* Nonzero once any `raw {` brace was annotated this TU — gates scope walks. */
static PRISM_THREAD_LOCAL uint32_t p1_raw_block_count;

#define TD_SCOPE_SAVE() uint32_t _tds_o = td_scope_open, _tds_c = td_scope_close
#define TD_SCOPE_RESTORE()                                                                                   \
	do {                                                                                                 \
		td_scope_open = _tds_o;                                                                      \
		td_scope_close = _tds_c;                                                                     \
	} while (0)

#define is_c23_attr(t) ((t) && ((t)->flags & TF_C23_ATTR))
#define is_sizeof_like(t) ((t)->flags & TF_SIZEOF)

static inline PRISM_PURE uint32_t token_name_hash(Token *tok) {
	/* After pool annotation, parse_data holds the resolved typedef entry for
	 * P1_HAS_ENTRY tokens. The uncommon bounds/tag query recomputes its hash. */
	if (p1_typedef_annotated && (tok_ann(tok) & P1_HAS_ENTRY))
		return fast_hash(tok_loc(tok), tok->len);
	return tok->parse_data;
}

static inline PRISM_PURE void *c_function_symbol(Token *tok) {
	if (!tok || !c_function_symbols.buckets) return NULL;
	return hashmap_get_hashed(
	    &c_function_symbols, tok_loc(tok), tok->len, token_name_hash(tok));
}

static inline void c_function_symbol_put(Token *tok, void *kind) {
	hashmap_put_hashed(
	    &c_function_symbols, tok_loc(tok), tok->len, kind, token_name_hash(tok));
}

static inline void c_function_symbols_reset(void) {
	hashmap_discard(&c_function_symbols);
}
#define is_enum_kw(t) ((t)->tag & TT_SUE && (t)->ch0 == 'e')

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool is_identifier_like(Token *tok) {
	return tok->kind <= TK_KEYWORD; // TK_IDENT=0, TK_KEYWORD=1
}

static inline Token *skip_balanced_group(Token *tok) {
	Token *end = tok_match(tok);
	if (!end) return tok_next(tok);
	return tok_next(end);
}

static inline Token *skip_prep_dirs(Token *tok) {
	while (tok && tok->kind == TK_PREP_DIR) tok = tok_next(tok);
	return tok;
}

static inline Token *skip_prep_dirs_until(Token *tok, Token *end) {
	while (tok && tok != end && tok->kind == TK_PREP_DIR) tok = tok_next(tok);
	return tok;
}

static bool is_pp_conditional(Token *s) {
	if (s->kind != TK_PREP_DIR) return false;
	const char *dp = tok_loc(s);
	if (*dp == '#') dp++;
	while (*dp == ' ' || *dp == '\t') dp++;
	return strncmp(dp, "ifdef", 5) == 0 || strncmp(dp, "ifndef", 6) == 0 || strncmp(dp, "elif", 4) == 0 ||
	       strncmp(dp, "else", 4) == 0 || strncmp(dp, "endif", 5) == 0 ||
	       (strncmp(dp, "if", 2) == 0 && (dp[2] == ' ' || dp[2] == '\t' || dp[2] == '('));
}

static Token *span_find_pp_conditional(Token *start, Token *end, bool (*is_end)(Token *)) {
	int sd = 0;
	for (Token *s = start; s && s != end && s->kind != TK_EOF; s = tok_next(s)) {
		if (s->flags & TF_OPEN) sd++;
		else if (s->flags & TF_CLOSE)
			sd--;
		else if (is_end && sd == 0 && is_end(s))
			break;
		if (is_pp_conditional(s)) return s;
	}
	return NULL;
}

static bool tok_is_semicolon(Token *s) {
	return match_ch(s, ';');
}

// Skip noise tokens (attributes, C23 [[...]], prep dirs) in analysis mode.
static PRISM_PURE Token *skip_noise(Token *tok) {
	while (tok && tok->kind != TK_EOF) {
		if (tok->tag & TT_ATTR) {
			tok = tok_next(tok);
			if (tok && match_ch(tok, '(') && tok_match(tok)) tok = tok_next(tok_match(tok));
		} else if (is_c23_attr(tok) && tok_match(tok)) {
			tok = tok_next(tok_match(tok));
		} else if (tok->kind == TK_PREP_DIR) {
			tok = tok_next(tok);
		} else
			break;
	}
	return tok;
}

/* `skip_noise` already eats TT_ATTR / [[...]] / _Pragma; this adds TT_ASM. */
static Token *skip_asm_specifier_trail(Token *t) {
	t = skip_noise(t);
	while (t && (t->tag & TT_ASM)) {
		t = tok_next(t);
		if (t && match_ch(t, '(') && tok_match(t)) t = tok_next(tok_match(t));
		t = skip_noise(t);
	}
	return t;
}

// Check if a token is "noise" (attribute, C23 [[...]], or preprocessor directive).
// These tokens must be skipped via skip_noise() before any tag-based type checks.
static inline PRISM_PURE bool is_noise_token(Token *t) {
	return (t->tag & TT_ATTR) || is_c23_attr(t) || t->kind == TK_PREP_DIR;
}

#ifdef PRISM_DEBUG
#define ASSERT_NOT_NOISE(t)                                                                                  \
	do {                                                                                                 \
		if (is_noise_token(t))                                                                       \
			error_tok(t, "internal: tag check on noise token (skip_noise() missing)");           \
	} while (0)
#else
#define ASSERT_NOT_NOISE(t) ((void)0)
#endif

#define SKIP_NOISE_CONTINUE(var)                                                                             \
	do {                                                                                                 \
		Token *_sn = skip_noise(var);                                                                \
		if (_sn != (var)) {                                                                          \
			(var) = _sn;                                                                         \
			continue;                                                                            \
		}                                                                                            \
	} while (0)

static PRISM_PURE Token *skip_to_semicolon(Token *tok, Token *end) {
	while (tok->kind != TK_EOF) {
		if (end && tok == end) return tok;
		if (tok->flags & TF_OPEN) {
			tok = tok_next(tok_match(tok));
			continue;
		}
		if (match_ch(tok, ';')) return tok;
		if ((tok->flags & TF_CLOSE) && tok->ch0 == '}') return tok;
		tok = tok_next(tok);
	}
	return tok;
}

/* Long same-name chains (stress TUs) get a token_index-sorted timeline for
 * O(log n) lookup instead of newest→oldest walks. Short chains keep the walk.
 * Timelines are rebuilt during Pass 1 as chains grow (not only after prescan),
 * so bounds-check array shadows do not force O(n²) linear walks mid-TU. */
static PRISM_THREAD_LOCAL HashMap td_tl_map; /* name → packed (base<<32)|count into td_tl_idxs */
static PRISM_THREAD_LOCAL int *td_tl_idxs;
static PRISM_THREAD_LOCAL int td_tl_cap;
static PRISM_THREAD_LOCAL int td_max_chain_seen; /* capped sample; ≥8 triggers timeline build */
static PRISM_THREAD_LOCAL int td_tl_count_at_build; /* typedef_table.count when timelines last built */

/* Bounds-check array locals live here — not in the ordinary typedef/shadow
 * chain — so Pass 1/2 typedef lookups are not taxed by `int a[N]` × thousands
 * of functions. Lookups use the same mid-Pass1 timeline pattern as typedefs. */
typedef struct {
	char *name;
	int prev_index;
	uint32_t token_index;
	uint32_t scope_open_idx;
	uint32_t scope_close_idx;
	uint16_t len;
	uint8_t array_rank;
	bool array_dim_complete : 1;
	bool is_vla_var : 1;
	bool is_param : 1;
} BoundsArrayEntry;

typedef struct {
	BoundsArrayEntry *entries;
	int count;
	int capacity;
	HashMap name_map;
	uint64_t bloom;
} BoundsArrayTable;

static PRISM_THREAD_LOCAL BoundsArrayTable bounds_array_table;
static PRISM_THREAD_LOCAL HashMap ba_tl_map;
static PRISM_THREAD_LOCAL int *ba_tl_idxs;
static PRISM_THREAD_LOCAL int ba_tl_cap;
static PRISM_THREAD_LOCAL int ba_max_chain_seen;
static PRISM_THREAD_LOCAL int ba_tl_count_at_build;

static void bounds_array_table_reset(void) {
	bounds_array_table.entries = NULL;
	bounds_array_table.count = 0;
	bounds_array_table.capacity = 0;
	bounds_array_table.bloom = 0;
	hashmap_discard(&bounds_array_table.name_map);
	hashmap_discard(&ba_tl_map);
	ba_tl_idxs = NULL;
	ba_tl_cap = 0;
	ba_max_chain_seen = 0;
	ba_tl_count_at_build = 0;
}

static void typedef_table_reset(void) {
	typedef_table.entries = NULL;
	typedef_table.count = 0;
	typedef_table.capacity = 0;
	typedef_table.bloom = 0;
	hashmap_discard(&typedef_table.name_map);
	hashmap_discard(&td_tl_map);
	td_tl_idxs = NULL;
	td_tl_cap = 0;
	td_max_chain_seen = 0;
	td_tl_count_at_build = 0;
	bounds_array_table_reset();
}

static void c_parse_reset(void) {
	typedef_table_reset();
	c_function_symbols_reset();
	p1_typedef_annotated = false;
}

static PRISM_PURE int typedef_get_index(char *name, int len) {
	return hashmap_index_hashed(&typedef_table.name_map, name, len, fast_hash(name, len));
}

static int td_tl_cmp_tok(const void *pa, const void *pb) {
	uint32_t ta = typedef_table.entries[*(const int *)pa].token_index;
	uint32_t tb = typedef_table.entries[*(const int *)pb].token_index;
	return (ta > tb) - (ta < tb);
}

static void td_build_timelines(void) {
	hashmap_clear(&td_tl_map);
	td_tl_count_at_build = typedef_table.count;
	if (td_max_chain_seen < 8 || !typedef_table.name_map.buckets) return;

	/* Walk name_map heads only (one probe per unique name). */
	int cap = typedef_table.name_map.capacity;
	int pos = 0;
	for (int b = 0; b < cap; b++) {
		HashEntry *ent = &typedef_table.name_map.buckets[b];
		if (!ent->key || ent->key == TOMBSTONE) continue;
		int head = (int)(intptr_t)ent->val - 1;
		int run = 0;
		for (int j = head; j >= 0; j = typedef_table.entries[j].prev_index) run++;
		if (run < 8) continue;
		int base = pos;
		ARENA_ENSURE_CAP(&ctx->main_arena, td_tl_idxs, pos + run, td_tl_cap, 64, int);
		for (int j = head; j >= 0; j = typedef_table.entries[j].prev_index)
			td_tl_idxs[pos++] = j;
		qsort(td_tl_idxs + base, (size_t)run, sizeof(int), td_tl_cmp_tok);
		hashmap_put_hashed(&td_tl_map, ent->key, ent->key_len,
				   (void *)(((uintptr_t)(uint32_t)base << 32) | (uint32_t)run), ent->hash);
	}
}

static PRISM_PURE TypedefEntry *td_lookup_timeline(void *val, uint32_t cur, bool tags_only) {
	uint32_t base = (uint32_t)((uintptr_t)val >> 32);
	uint32_t run = (uint32_t)(uintptr_t)val;
	int lo = (int)base, hi = (int)base + (int)run;
	while (lo < hi) {
		int mid = lo + ((hi - lo) >> 1);
		if (typedef_table.entries[td_tl_idxs[mid]].token_index <= cur)
			lo = mid + 1;
		else
			hi = mid;
	}
	TypedefEntry *tag_fallback = NULL;
	for (int p = lo - 1; p >= (int)base; p--) {
		TypedefEntry *e = &typedef_table.entries[td_tl_idxs[p]];
		if (cur < e->scope_open_idx || cur >= e->scope_close_idx) continue;
		if (tags_only) {
			if (e->is_struct_tag) return e;
			continue;
		}
		if (!e->is_struct_tag) return e;
		if (!tag_fallback) tag_fallback = e;
	}
	return tag_fallback;
}

static int ba_tl_cmp_tok(const void *pa, const void *pb) {
	uint32_t ta = bounds_array_table.entries[*(const int *)pa].token_index;
	uint32_t tb = bounds_array_table.entries[*(const int *)pb].token_index;
	return (ta > tb) - (ta < tb);
}

static void ba_build_timelines(void) {
	hashmap_clear(&ba_tl_map);
	ba_tl_count_at_build = bounds_array_table.count;
	if (ba_max_chain_seen < 8 || !bounds_array_table.name_map.buckets) return;

	int cap = bounds_array_table.name_map.capacity;
	int pos = 0;
	for (int b = 0; b < cap; b++) {
		HashEntry *ent = &bounds_array_table.name_map.buckets[b];
		if (!ent->key || ent->key == TOMBSTONE) continue;
		int head = (int)(intptr_t)ent->val - 1;
		int run = 0;
		for (int j = head; j >= 0; j = bounds_array_table.entries[j].prev_index) run++;
		if (run < 8) continue;
		int base = pos;
		ARENA_ENSURE_CAP(&ctx->main_arena, ba_tl_idxs, pos + run, ba_tl_cap, 64, int);
		for (int j = head; j >= 0; j = bounds_array_table.entries[j].prev_index)
			ba_tl_idxs[pos++] = j;
		qsort(ba_tl_idxs + base, (size_t)run, sizeof(int), ba_tl_cmp_tok);
		hashmap_put_hashed(&ba_tl_map, ent->key, ent->key_len,
				   (void *)(((uintptr_t)(uint32_t)base << 32) | (uint32_t)run), ent->hash);
	}
}

static PRISM_PURE BoundsArrayEntry *ba_lookup_timeline(void *val, uint32_t cur) {
	uint32_t base = (uint32_t)((uintptr_t)val >> 32);
	uint32_t run = (uint32_t)(uintptr_t)val;
	int lo = (int)base, hi = (int)base + (int)run;
	while (lo < hi) {
		int mid = lo + ((hi - lo) >> 1);
		if (bounds_array_table.entries[ba_tl_idxs[mid]].token_index <= cur)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (int p = lo - 1; p >= (int)base; p--) {
		BoundsArrayEntry *e = &bounds_array_table.entries[ba_tl_idxs[p]];
		if (cur >= e->scope_open_idx && cur < e->scope_close_idx) return e;
	}
	return NULL;
}

static void bounds_array_add(char *name, int len, uint32_t token_index, uint8_t array_rank,
				     bool dim_complete, bool is_vla_var, bool is_param) {
	uint32_t hash = fast_hash(name, len);
	int existing = hashmap_index_hashed(&bounds_array_table.name_map, name, len, hash);
	if (existing >= 0) {
		BoundsArrayEntry *prev = &bounds_array_table.entries[existing];
		if (prev->token_index == token_index && prev->scope_open_idx == td_scope_open &&
		    prev->scope_close_idx == td_scope_close) {
			prev->array_rank = array_rank;
			prev->array_dim_complete = dim_complete;
			prev->is_vla_var = is_vla_var;
			prev->is_param = is_param;
			return;
		}
	}

	ARENA_ENSURE_CAP(&ctx->main_arena,
			 bounds_array_table.entries,
			 bounds_array_table.count + 1,
			 bounds_array_table.capacity,
			 32,
			 BoundsArrayEntry);
	int new_index = bounds_array_table.count++;
	BoundsArrayEntry *e = &bounds_array_table.entries[new_index];
	e->name = name;
	e->len = (uint16_t)len;
	e->prev_index = existing;
	e->token_index = token_index;
	e->scope_open_idx = td_scope_open;
	e->scope_close_idx = td_scope_close;
	e->array_rank = array_rank;
	e->array_dim_complete = dim_complete;
	e->is_vla_var = is_vla_var;
	e->is_param = is_param;
	hashmap_put_hashed(
	    &bounds_array_table.name_map, name, len, (void *)(intptr_t)(new_index + 1), hash);
	bounds_array_table.bloom |= 1ULL << (((unsigned char)name[0] ^ len) & 63);
	int cl = 1;
	for (int p = e->prev_index; p >= 0 && cl < 8; p = bounds_array_table.entries[p].prev_index) cl++;
	if (cl > ba_max_chain_seen) ba_max_chain_seen = cl;
	if (ba_max_chain_seen >= 8) {
		int since = bounds_array_table.count - ba_tl_count_at_build;
		if (!ba_tl_idxs || since >= 64) ba_build_timelines();
	}
}

static PRISM_PURE BoundsArrayEntry *bounds_array_lookup(Token *tok) {
	if (!is_identifier_like(tok)) return NULL;
	unsigned c0 = tok->ch0, tl = tok->len;
	if (!(bounds_array_table.bloom & (1ULL << ((c0 ^ tl) & 63)))) return NULL;
	char *name = tok_loc(tok);
	uint32_t cur = tok_idx(tok);
	if (ba_tl_idxs) {
		uint32_t hash = token_name_hash(tok);
		int idx = hashmap_index_hashed(&bounds_array_table.name_map, name, (int)tok->len, hash);
		while (idx >= ba_tl_count_at_build) {
			BoundsArrayEntry *e = &bounds_array_table.entries[idx];
			if (e->token_index <= cur && cur >= e->scope_open_idx && cur < e->scope_close_idx)
				return e;
			idx = e->prev_index;
		}
		void *tl = hashmap_get_hashed(&ba_tl_map, name, (int)tok->len, hash);
		if (tl) return ba_lookup_timeline(tl, cur);
		while (idx >= 0) {
			BoundsArrayEntry *e = &bounds_array_table.entries[idx];
			if (e->token_index <= cur && cur >= e->scope_open_idx && cur < e->scope_close_idx)
				return e;
			idx = e->prev_index;
		}
		return NULL;
	}
	int idx = hashmap_index_hashed(
	    &bounds_array_table.name_map, name, (int)tok->len, token_name_hash(tok));
	while (idx >= 0) {
		BoundsArrayEntry *e = &bounds_array_table.entries[idx];
		if (e->token_index <= cur && cur >= e->scope_open_idx && cur < e->scope_close_idx)
			return e;
		idx = e->prev_index;
	}
	return NULL;
}

static PRISM_PURE BoundsArrayEntry *bounds_array_entry_for_token(Token *t) {
	for (int ix = hashmap_index_hashed(
		 &bounds_array_table.name_map, tok_loc(t), t->len, token_name_hash(t));
	     ix >= 0;
	     ix = bounds_array_table.entries[ix].prev_index) {
		BoundsArrayEntry *e = &bounds_array_table.entries[ix];
		if (e->token_index == tok_idx(t)) return e;
	}
	return NULL;
}

static void
typedef_add_entry(char *name, int len, int scope_depth, TypedefKind kind, bool is_vla, bool is_void) {
	uint32_t hash = fast_hash(name, len);
	int existing = hashmap_index_hashed(&typedef_table.name_map, name, len, hash);
	// Skip duplicate re-definitions at the same scope (valid C11 §6.7/3).
	if (existing >= 0) {
		TypedefEntry *prev = &typedef_table.entries[existing];
		if (kind == TDK_SHADOW || kind == TDK_VLA_VAR) {
			if (prev->scope_depth == scope_depth && prev->scope_open_idx == td_scope_open &&
			    prev->scope_close_idx == td_scope_close &&
			    prev->is_shadow == (kind == TDK_SHADOW) &&
			    prev->is_vla_var == (kind == TDK_VLA_VAR))
				return;
		} else if (prev->scope_depth == scope_depth && !prev->is_shadow &&
			   prev->scope_open_idx == td_scope_open && prev->scope_close_idx == td_scope_close &&
			   prev->is_struct_tag == (kind == TDK_STRUCT_TAG))
			return;
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
	e->is_atomic = false;
	e->prev_index = existing;
	e->token_index = 0;
	e->scope_open_idx = td_scope_open;
	e->scope_close_idx = td_scope_close;
	hashmap_put_hashed(&typedef_table.name_map, name, len, (void *)(intptr_t)(new_index + 1), hash);
	typedef_table.bloom |= 1ULL << (((unsigned char)name[0] ^ len) & 63);
	int cl = 1;
	for (int p = e->prev_index; p >= 0 && cl < 8; p = typedef_table.entries[p].prev_index) cl++;
	if (cl > td_max_chain_seen) td_max_chain_seen = cl;
	/* Rebuild mid-Pass1 so long same-name chains stay O(log n).
	 * Cadence: first at chain≥8, then every 64 new table entries. */
	if (td_max_chain_seen >= 8) {
		int since = typedef_table.count - td_tl_count_at_build;
		if (!td_tl_idxs || since >= 64) td_build_timelines();
	}
}

static TypedefEntry *c_binding_entry(Token *tok, bool shadow_only) {
	for (int ix = typedef_get_index(tok_loc(tok), tok->len); ix >= 0;
	     ix = typedef_table.entries[ix].prev_index) {
		TypedefEntry *e = &typedef_table.entries[ix];
		if (e->token_index == tok_idx(tok) && (!shadow_only || e->is_shadow)) return e;
	}
	return NULL;
}

/* Register a token-owned C binding and preserve its source identity in one
 * place. Prism never needs to coordinate the typedef table's name/index API. */
static TypedefEntry *c_register_binding(Token *tok, int scope_depth, TypedefKind kind) {
	int before = typedef_table.count;
	typedef_add_entry(tok_loc(tok),
			  tok->len,
			  scope_depth,
			  kind,
			  kind == TDK_VLA_VAR,
			  false);
	if (typedef_table.count > before) {
		TypedefEntry *added = &typedef_table.entries[typedef_table.count - 1];
		added->token_index = tok_idx(tok);
		return added;
	}
	return c_binding_entry(tok, kind == TDK_SHADOW);
}

static inline TypedefEntry *c_register_shadow(Token *tok, int scope_depth) {
	return c_register_binding(tok, scope_depth, TDK_SHADOW);
}

static inline TypedefEntry *c_shadow_entry(Token *tok) {
	return c_binding_entry(tok, true);
}

static inline TypedefEntry *c_register_vla_var(Token *tok, int scope_depth) {
	return c_register_binding(tok, scope_depth, TDK_VLA_VAR);
}

static inline bool is_soft_keyword_identifier(Token *tok);

static PRISM_PURE TypedefEntry *typedef_lookup(Token *tok) {
	if (!is_identifier_like(tok)) return NULL;
	if (p1_typedef_annotated) {
		if (!(tok_ann(tok) & P1_HAS_ENTRY)) return NULL;
		return &typedef_table.entries[tok->parse_data - 1];
	}
	if (tok->kind == TK_KEYWORD && !is_soft_keyword_identifier(tok) &&
	    !(tok->tag & (TT_ORELSE | TT_DEFER)) && !(tok->flags & TF_RAW))
		return NULL;
	unsigned c0 = tok->ch0, tl = tok->len;
	if (!(typedef_table.bloom & (1ULL << ((c0 ^ tl) & 63)))) return NULL;
	char *name = tok_loc(tok);
	uint32_t cur = tok_idx(tok);
	if (td_tl_idxs) {
		uint32_t hash = token_name_hash(tok);
		TypedefEntry *tag_fallback = NULL;
		int idx = hashmap_index_hashed(&typedef_table.name_map, name, (int)tok->len, hash);
		/* Entries added since the last build are absent from the sorted
		 * timeline. Walk only that short newest prefix (cadence ≤64). */
		while (idx >= td_tl_count_at_build) {
			TypedefEntry *e = &typedef_table.entries[idx];
			if (e->token_index <= cur && cur >= e->scope_open_idx &&
			    cur < e->scope_close_idx) {
				if (!e->is_struct_tag) return e;
				if (!tag_fallback) tag_fallback = e;
			}
			idx = e->prev_index;
		}
		void *tl = hashmap_get_hashed(&td_tl_map, name, (int)tok->len, hash);
		if (tl) {
			TypedefEntry *hit = td_lookup_timeline(tl, cur, false);
			return hit ? hit : tag_fallback;
		}
		/* Short chain (no timeline): finish the ordinary walk from idx. */
		while (idx >= 0) {
			TypedefEntry *e = &typedef_table.entries[idx];
			if (e->token_index <= cur && cur >= e->scope_open_idx &&
			    cur < e->scope_close_idx) {
				if (!e->is_struct_tag) return e;
				if (!tag_fallback) tag_fallback = e;
			}
			idx = e->prev_index;
		}
		return tag_fallback;
	}
	int idx = hashmap_index_hashed(&typedef_table.name_map, name, tok->len, token_name_hash(tok));
	// ISO C11 §6.2.3: tag namespace is separate from ordinary identifiers.
	TypedefEntry *tag_fallback = NULL;
	while (idx >= 0) {
		TypedefEntry *e = &typedef_table.entries[idx];
		if (e->token_index <= cur && cur >= e->scope_open_idx && cur < e->scope_close_idx) {
			if (!e->is_struct_tag) return e;
			if (!tag_fallback) tag_fallback = e;
		}
		idx = e->prev_index;
	}
	return tag_fallback;
}

// Enforces ISO C11 §6.2.3 namespace separation: tag names live in a
static PRISM_PURE TypedefEntry *tag_lookup(Token *tok) {
	if (!is_identifier_like(tok)) return NULL;
	unsigned c0 = tok->ch0, tl = tok->len;
	if (!(typedef_table.bloom & (1ULL << ((c0 ^ tl) & 63)))) return NULL;
	char *name = tok_loc(tok);
	uint32_t cur = tok_idx(tok);
	if (td_tl_idxs) {
		uint32_t hash = token_name_hash(tok);
		int idx = hashmap_index_hashed(&typedef_table.name_map, name, (int)tok->len, hash);
		while (idx >= td_tl_count_at_build) {
			TypedefEntry *e = &typedef_table.entries[idx];
			if (e->is_struct_tag && e->token_index <= cur && cur >= e->scope_open_idx &&
			    cur < e->scope_close_idx)
				return e;
			idx = e->prev_index;
		}
		void *tl = hashmap_get_hashed(&td_tl_map, name, (int)tok->len, hash);
		if (tl) return td_lookup_timeline(tl, cur, true);
		while (idx >= 0) {
			TypedefEntry *e = &typedef_table.entries[idx];
			if (e->is_struct_tag && e->token_index <= cur && cur >= e->scope_open_idx &&
			    cur < e->scope_close_idx)
				return e;
			idx = e->prev_index;
		}
		return NULL;
	}
	int idx = hashmap_index_hashed(&typedef_table.name_map, name, tok->len, token_name_hash(tok));
	while (idx >= 0) {
		TypedefEntry *e = &typedef_table.entries[idx];
		if (e->is_struct_tag && e->token_index <= cur && cur >= e->scope_open_idx &&
		    cur < e->scope_close_idx)
			return e;
		idx = e->prev_index;
	}
	return NULL;
}

static inline PRISM_PURE int typedef_flags(Token *tok) {
	TypedefEntry *e = typedef_lookup(tok);
	if (!e) {
		BoundsArrayEntry *be = bounds_array_lookup(tok);
		if (!be || be->is_param) return 0;
		return TDF_ARRAY | (be->is_vla_var ? TDF_VLA : 0);
	}
	if (e->is_enum_const) return TDF_ENUM_CONST;
	if (e->is_shadow) {
		int fl = (e->is_volatile ? TDF_VOLATILE : 0) |
			 (e->has_volatile_member ? TDF_HAS_VOL_MEMBER : 0) |
			 (e->is_atomic ? TDF_ATOMIC : 0) | (e->is_array ? TDF_ARRAY : 0);
		/* Decayed params must not inherit TDF_ARRAY from an outer
		 * file-scope array of the same name. */
		if (!(fl & TDF_ARRAY) && !e->is_param) {
			BoundsArrayEntry *be = bounds_array_lookup(tok);
			if (be && !be->is_param) fl |= TDF_ARRAY | (be->is_vla_var ? TDF_VLA : 0);
		}
		return fl;
	}
	if (e->is_vla_var)
		return TDF_VLA | (e->is_param ? TDF_PARAM : 0) |
		       (e->has_volatile_member ? TDF_HAS_VOL_MEMBER : 0) |
		       (e->is_array ? TDF_ARRAY : 0);
	if (e->is_struct_tag)
		return (e->is_vla ? TDF_VLA : 0) | (e->has_volatile_member ? TDF_HAS_VOL_MEMBER : 0) |
		       (e->is_aggregate ? TDF_AGGREGATE : 0);
	return TDF_TYPEDEF | (e->is_vla ? TDF_VLA : 0) | (e->is_void ? TDF_VOID : 0) |
	       (e->is_const ? TDF_CONST : 0) | (e->is_volatile ? TDF_VOLATILE : 0) |
	       (e->is_ptr ? TDF_PTR : 0) | (e->is_array ? TDF_ARRAY : 0) |
	       (e->is_aggregate ? TDF_AGGREGATE : 0) | (e->is_func ? TDF_FUNC : 0) |
	       (e->has_volatile_member ? TDF_HAS_VOL_MEMBER : 0) | (e->is_union ? TDF_UNION : 0) |
	       (e->is_atomic ? TDF_ATOMIC : 0);
}

static inline PRISM_PURE bool _is_known_typedef(Token *tok) {
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

static inline bool c_token_can_name_function(Token *tok) {
	return tok &&
	       (tok->kind == TK_IDENT || (tok->tag & (TT_DEFER | TT_ORELSE)) || (tok->flags & TF_RAW));
}

static inline bool c_is_known_function_call(Token *tok) {
	if (!c_function_symbol(tok)) return false;
	Token *next = skip_noise(tok_next(tok));
	return next && match_ch(next, '(');
}

static inline bool c_is_empty_function_call(Token *tok) {
	if (!c_is_known_function_call(tok)) return false;
	Token *open = skip_noise(tok_next(tok));
	return open && match_ch(open, '(') && tok_match(open) && tok_next(open) == tok_match(open);
}

static bool c_token_can_precede_function_name(Token *tok) {
	while (tok && (match_ch(tok, '*') || (tok->tag & (TT_QUALIFIER | TT_STORAGE | TT_INLINE))))
		tok = tok_walk_back(tok_idx(tok), WB_PAST_NOISE);
	return tok && ((tok->tag & (TT_TYPE | TT_SUE | TT_TYPEOF | TT_BITINT)) || is_known_typedef(tok));
}

static bool c_token_can_start_knr_param_decl(Token *tok) {
	return tok && ((tok->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_INLINE | TT_TYPEDEF | TT_SUE |
				    TT_TYPEOF | TT_BITINT)) ||
		       is_known_typedef(tok));
}

static bool c_paren_is_function_params(Token *open) {
	if (!open || !match_ch(open, '(') || !tok_match(open)) return false;
	Token *close = tok_match(open);
	Token *after = skip_asm_specifier_trail(tok_next(close));
	if (!after ||
	    !(match_ch(after, '{') || match_ch(after, ';') || match_ch(after, ',') || match_ch(after, '=') ||
	      match_ch(after, ')') || c_token_can_start_knr_param_decl(after)))
		return false;
	/* Keep the `)` in `(*F)(...)`: jumping the pointer group would mistake
	 * typedef/function-pointer parameter dimensions for expression parens. */
	Token *prev = tok_walk_back(tok_idx(open), WB_ATTR_NOISE);
	if (prev && c_token_can_name_function(prev)) {
		Token *before = tok_walk_back(tok_idx(prev), WB_ATTR_NOISE);
		return c_token_can_precede_function_name(before);
	}
	if (prev && match_ch(prev, ')') && tok_match(prev)) {
		Token *before = tok_walk_back(tok_idx(tok_match(prev)), WB_ATTR_NOISE);
		return c_token_can_precede_function_name(before);
	}
	return false;
}

static Token *c_function_param_open(Token *tok) {
	if (!tok) return NULL;
	int depth = 0;
	for (uint32_t i = tok_idx(tok); i > 0; i--) {
		Token *t = &token_pool[i - 1];
		if (t->kind == TK_PREP_DIR) continue;
		if (t->flags & TF_CLOSE) {
			depth++;
			continue;
		}
		if (t->flags & TF_OPEN) {
			if (depth == 0 && match_ch(t, '(')) return c_paren_is_function_params(t) ? t : NULL;
			if (depth > 0) depth--;
			continue;
		}
		if (depth == 0 && (match_ch(t, ';') || match_ch(t, '{') || match_ch(t, '}'))) break;
	}
	return NULL;
}

static PRISM_PURE uint8_t array_rank_for_tok(Token *t) {
	TypedefEntry *te = typedef_lookup(t);
	if (te && te->array_rank > 0) return te->array_rank;
	BoundsArrayEntry *be = bounds_array_lookup(t);
	if (be && be->array_rank > 0) return be->array_rank;
	return 0;
}

static PRISM_PURE bool is_type_keyword(Token *tok) {
	if (tok->tag & TT_TYPE) return true;
	if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD) return false;
	return is_known_typedef(tok);
}

static inline PRISM_PURE bool is_soft_keyword_identifier(Token *tok) {
	return tok && tok->kind == TK_KEYWORD && (tok->flags & TF_SOFT_KW);
}

static inline bool soft_keyword_decl_name_boundary(Token *tok) {
	Token *after = skip_noise(tok_next(tok));
	/* Include '(' so `int orelse(int);` / `T defer(void)` treat the soft
	 * keyword as a declarator name (function prototype), not an operator. */
	uint64_t end = CH(';') | CH(',') | CH('=') | CH('[') | CH(':') | CH('(');
	return after && (match_set(after, end) || (after->tag & TT_ASM));
}

static inline PRISM_PURE bool is_valid_varname(Token *tok) {
	return tok->kind == TK_IDENT || is_soft_keyword_identifier(tok) || (tok->flags & TF_RAW) ||
	       (tok->tag & (TT_DEFER | TT_ORELSE));
}

static inline PRISM_PURE bool is_expr_ending(Token *t) {
	return (t->kind == TK_IDENT || t->kind == TK_KEYWORD || t->kind == TK_NUM || t->kind == TK_STR) ||
	       match_set(t, CH(')') | CH(']'));
}

static inline PRISM_PURE bool is_expr_ending_brace(Token *t) {
	return is_expr_ending(t) || match_ch(t, '}');
}

static void parse_enum_constants(Token *tok, int scope_depth) {
	if (!tok || !(match_ch(tok, '{'))) return;
	tok = tok_next(tok); // Skip '{'
	while (tok && tok->kind != TK_EOF && !(match_ch(tok, '}'))) {
		SKIP_NOISE_CONTINUE(tok);
		if (is_valid_varname(tok)) {
			int pre = typedef_table.count;
			typedef_add_entry(tok_loc(tok), tok->len, scope_depth, TDK_ENUM_CONST, false, false);
			if (typedef_table.count > pre)
				typedef_table.entries[typedef_table.count - 1].token_index = tok_idx(tok);
			tok = tok_next(tok);
			tok = skip_noise(tok); // Skip C23/GNU attributes on enumerator

			if (tok && match_ch(tok, '=')) {
				tok = tok_next(tok);
				while (tok && tok->kind != TK_EOF) {
					if (tok->flags & TF_OPEN) {
						tok = tok_next(tok_match(tok));
						continue;
					}
					if (tok->len == 1 && (tok->ch0 == ',' || tok->ch0 == '}')) break;
					tok = tok_next(tok);
				}
			}

			if (tok && match_ch(tok, ',')) tok = tok_next(tok);
		} else
			tok = tok_next(tok);
	}
}

static inline PRISM_PURE bool is_orelse_kw_shadow(Token *tok) {
	if (!(tok->tag & TT_ORELSE)) return false;
	TypedefEntry *te = typedef_lookup(tok);
	return !te || te->is_shadow;
}

static bool close_brace_ends_sue_body(Token *tok);
static bool token_ends_sue_type_specifier(Token *tok);
static bool close_paren_ends_cast_type_name(Token *tok);
static bool orelse_is_label_or_goto_target(Token *tok, Token *prev);
static TypeSpecResult parse_type_specifier(Token *tok);

static bool close_paren_ends_type_specifier_ctor(Token *tok);

static inline bool orelse_shadow_is_kw(Token *prev) {
	if (!prev) return false;
	if (token_ends_sue_type_specifier(prev)) return false;
	if (close_paren_ends_cast_type_name(prev)) return false;
	/* `_BitInt(N) orelse` / `typeof(T) orelse` / `_Alignas(N) int orelse`:
	 * the closing `)` ends a type-specifier constructor, not an expression. */
	if (close_paren_ends_type_specifier_ctor(prev)) return false;
	if (orelse_is_label_or_goto_target(NULL, prev)) return false;
	/* `return orelse;` — orelse is the return operand (identifier), not an
	 * operator. Real keyword form is `return x orelse fb;`.
	 * Do NOT exclude break/continue: `… orelse continue orelse …` mid-chain
	 * needs the second orelse classified as a keyword. goto is already
	 * handled by orelse_is_label_or_goto_target above. */
	if (prev->tag & TT_RETURN) return false;
	/* Soft keywords (incl. type spellings used as names: `_Float32`, `bool`,
	 * `asm`) are expression-ending. `bool orelse = 0` is handled in
	 * orelse_kw_at via soft_keyword_decl_name_boundary before this runs. */
	if (is_soft_keyword_identifier(prev)) return is_expr_ending_brace(prev);
	if (prev->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_SUE | TT_TYPEOF | TT_BITINT | TT_SKIP_DECL |
			 TT_ALIGNAS | TT_INLINE | TT_ATTR))
		return false;
	/* `T orelse = 0` where T is a typedef-name: mirror is_defer_kw. */
	if (is_known_typedef(prev)) return false;
	if (prev->len == 2 && (prev->ch0 == '+' || prev->ch0 == '-') && tok_loc(prev)[1] == prev->ch0)
		return true;
	return is_expr_ending_brace(prev);
}

static Token *find_struct_body_brace(Token *tok) {
	Token *t = tok_next(tok);
	bool saw_tag = false;
	while (t && t->kind != TK_EOF) {
		SKIP_NOISE_CONTINUE(t);
		if (!saw_tag && is_valid_varname(t)) {
			saw_tag = true;
			t = tok_next(t);
		} else if ((t->tag & TT_QUALIFIER) || is_type_keyword(t)) {
			t = tok_next(t);
		} else if (match_ch(t, ':')) {
			/* C23 enum fixed underlying type: enum E : int { ... }
			 * Also `enum E : typeof(unsigned)` / `_BitInt(N)` / `_Alignas`. */
			t = tok_next(t);
			while (t && t->kind != TK_EOF) {
				SKIP_NOISE_CONTINUE(t);
				if ((t->tag & (TT_TYPEOF | TT_BITINT | TT_ALIGNAS | TT_ATTR)) &&
				    tok_next(t) && match_ch(tok_next(t), '(') && tok_match(tok_next(t))) {
					t = tok_next(tok_match(tok_next(t)));
					continue;
				}
				/* `enum E : _Atomic(int) {` */
				if ((t->tag & TT_TYPE) && equal(t, "_Atomic") && tok_next(t) &&
				    match_ch(tok_next(t), '(') && tok_match(tok_next(t))) {
					t = tok_next(tok_match(tok_next(t)));
					continue;
				}
				if (is_c23_attr(t) && tok_match(t)) {
					t = tok_next(tok_match(t));
					continue;
				}
				if ((t->tag & TT_QUALIFIER) || is_type_keyword(t) || is_known_typedef(t)) {
					t = tok_next(t);
					continue;
				}
				if (match_ch(t, '*')) {
					t = tok_next(t);
					continue;
				}
				break;
			}
		} else
			break;
	}
	return (t && match_ch(t, '{')) ? t : NULL;
}

/* Unified backward token walker.
 *
 * Indexing conventions (preserved via flag presets above):
 *   WB_FROM_PRED — start at pool[start_idx-1] (past_noise / attr_noise)
 *   otherwise    — start at pool[start_idx]   (skip_noise / skip_attrs)
 *
 * Flag meanings:
 *   WB_SKIP_PREP       skip TK_PREP_DIR
 *   WB_SKIP_ATTR       skip TT_ATTR keywords
 *   WB_JUMP_GROUPS     jump any TF_CLOSE → matching open (unmatched → NULL)
 *   WB_JUMP_C23_ATTR   jump C23 ]] → [[
 *   WB_JUMP_ALL_PARENS jump every )
 *   WB_JUMP_ATTR_PARENS jump ) only when preceded by ATTR (attr-parens mode)
 */
static Token *tok_walk_back(uint32_t start_idx, unsigned flags) {
	/* Both conventions refuse to start at pool[0] as a candidate: FROM_PRED
	 * with start_idx==0 has no predecessor; skip_* with start_idx==0 matches
	 * the old `for (pi = 0; pi > 0;)` no-op. */
	if (start_idx == 0) return NULL;
	/* Loop invariant: pi is one past the next candidate (candidate = pi-1). */
	uint32_t pi = (flags & WB_FROM_PRED) ? start_idx : start_idx + 1;
	for (; pi > 0;) {
		pi--;
		Token *pt = &token_pool[pi];

		if ((flags & WB_SKIP_PREP) && pt->kind == TK_PREP_DIR) continue;
		if ((flags & WB_SKIP_ATTR) && (pt->tag & TT_ATTR)) continue;

		if (flags & WB_JUMP_GROUPS) {
			if (pt->flags & TF_CLOSE) {
				Token *open = tok_match(pt);
				if (!open) return NULL;
				pi = tok_idx(open); /* next iter looks at open-1 */
				continue;
			}
			return pt;
		}

		if ((flags & WB_JUMP_C23_ATTR) && match_ch(pt, ']') && tok_match(pt) &&
		    (tok_match(pt)->flags & TF_C23_ATTR)) {
			pi = tok_idx(tok_match(pt));
			continue;
		}

		if (match_ch(pt, ')') && tok_match(pt) &&
		    (flags & (WB_JUMP_ALL_PARENS | WB_JUMP_ATTR_PARENS))) {
			uint32_t open_idx = tok_idx(tok_match(pt));
			if (flags & WB_JUMP_ALL_PARENS) {
				pi = open_idx;
				continue;
			}
			/* WB_JUMP_ATTR_PARENS: jump only attr-bearing parens */
			for (uint32_t bi = open_idx; bi > 0;) {
				bi--;
				Token *bt = &token_pool[bi];
				if ((flags & WB_SKIP_PREP) && bt->kind == TK_PREP_DIR) continue;
				if (bt->tag & TT_ATTR) {
					pi = bi; /* skip ATTR on next iter's continue path */
					goto next;
				}
				break;
			}
		}
		return pt;
	next:;
	}
	return NULL;
}


/* WB_ATTR_NOISE: predecessor skipping only attributes / prep dirs — does not
 * jump TF_CLOSE→open groups, so `} (expr)` after `while (c) { }` is not
 * mistaken for closing `while (`. */

/* if/while/for/switch before a condition '(', walking past GNU/C23 attrs.
 * else/do take no condition paren — their body '(' must not match. */
static Token *ctrl_condition_kw_before_paren(Token *open) {
	if (!open || !match_ch(open, '(')) return NULL;
	Token *kw = tok_walk_back(tok_idx(open), WB_ATTR_NOISE);
	if (kw && (kw->tag & (TT_IF | TT_LOOP | TT_SWITCH)) && !is_else_or_do(kw)) return kw;
	return NULL;
}

/* C23 `enum Tag : unsigned int {` — `prev` is the type keyword before `{`.
 * `enum` carries TT_TYPE, so check is_enum_kw before skipping type keywords.
 * Also accepts `enum E : typeof(unsigned) {` where prev is `)`. */
static bool is_c23_fixed_underlying_enum(Token *type_kw_before_brace) {
	if (!type_kw_before_brace) return false;
	Token *anchor = type_kw_before_brace;
	if (match_ch(anchor, ')') && tok_match(anchor)) {
		Token *open = tok_match(anchor);
		Token *kw = tok_walk_back(tok_idx(open), WB_ATTR_NOISE);
		if (kw && ((kw->tag & (TT_TYPEOF | TT_BITINT | TT_ALIGNAS)) ||
			   ((kw->tag & TT_TYPE) && equal(kw, "_Atomic"))))
			anchor = kw;
		else
			return false;
	} else if (!is_type_keyword(anchor) && !is_known_typedef(anchor)) {
		return false;
	}
	/* si2-- form: no uint32 underflow at index 0, and pool[0] is inspected. */
	for (uint32_t si2 = tok_idx(anchor); si2-- > 0;) {
		Token *st = &token_pool[si2];
		if (st->kind == TK_PREP_DIR) continue;
		if (is_enum_kw(st)) return true;
		if (is_type_keyword(st) || (st->tag & TT_QUALIFIER) || is_known_typedef(st)) continue;
		if (match_ch(st, ':')) continue;
		if (match_ch(st, '*')) continue;
		if (match_ch(st, ']') && tok_match(st) && (tok_match(st)->flags & TF_C23_ATTR)) {
			si2 = tok_idx(tok_match(st));
			continue;
		}
		if (match_ch(st, ')') && tok_match(st)) {
			si2 = tok_idx(tok_match(st));
			continue;
		}
		if (st->tag & TT_ATTR) continue;
		if (is_valid_varname(st)) continue; // enum tag name
		break;
	}
	return false;
}

static bool close_brace_ends_sue_body(Token *tok) {
	if (!tok || !match_ch(tok, '}') || !tok_match(tok)) return false;
	Token *open = tok_match(tok);
	for (uint32_t ti = tok_idx(open); ti > 1;) {
		Token *t = &token_pool[ti - 1];
		if (t->kind == TK_PREP_DIR) {
			ti--;
			continue;
		}
		if (match_ch(t, ']') && tok_match(t) && (tok_match(t)->flags & TF_C23_ATTR)) {
			ti = tok_idx(tok_match(t));
			continue;
		}
		if (match_ch(t, ')') && tok_match(t)) {
			Token *open_paren = tok_match(t);
			Token *before = tok_idx(open_paren) > 1 ? &token_pool[tok_idx(open_paren) - 1] : NULL;
			if (before && (before->tag & (TT_ATTR | TT_ALIGNAS | TT_BITINT | TT_TYPEOF))) {
				ti = tok_idx(before);
				continue;
			}
			return false;
		}
		if (t->tag & TT_SUE) return true;
		if (match_ch(t, ';') || match_ch(t, '=') || match_ch(t, ',') || match_ch(t, '{') ||
		    match_ch(t, '}'))
			return false;
		ti--;
	}
	return false;
}

static bool token_ends_sue_type_specifier(Token *tok) {
	if (!tok) return false;
	if (close_brace_ends_sue_body(tok)) return true;
	Token *effective = tok_walk_back(tok_idx(tok) + 1, WB_PAST_NOISE);
	if (effective && effective != tok) return token_ends_sue_type_specifier(effective);
	if (tok->tag & TT_SUE) return true;
	if (is_identifier_like(tok)) {
		Token *before = tok_walk_back(tok_idx(tok), WB_PAST_NOISE);
		return before && (before->tag & TT_SUE);
	}
	return false;
}

static bool close_paren_ends_cast_type_name(Token *tok) {
	if (!tok || !match_ch(tok, ')') || !tok_match(tok)) return false;
	Token *open = tok_match(tok);
	Token *before_open = tok_walk_back(tok_idx(open), WB_PAST_NOISE);
	if (before_open &&
	    (is_sizeof_like(before_open) || (before_open->tag & (TT_TYPEOF | TT_ALIGNAS | TT_BITINT))))
		return false;
	Token *inner = skip_noise(tok_next(open));
	if (!inner || inner == tok) return false;
	TypeSpecResult type = parse_type_specifier(inner);
	if (!type.saw_type) return false;
	Token *t = type.end;
	while (t && t != tok && t->kind != TK_EOF) {
		Token *next = skip_noise(t);
		if (next != t) {
			t = next;
			continue;
		}
		if (match_ch(t, '*') || (t->tag & TT_QUALIFIER)) {
			t = tok_next(t);
			continue;
		}
		if ((match_ch(t, '(') || match_ch(t, '[')) && tok_match(t)) {
			t = tok_next(tok_match(t));
			continue;
		}
		return false;
	}
	return t == tok;
}

/* `)` that closes typeof(…), _BitInt(…), _Alignas(…), or _Atomic(…) —
 * a type-specifier constructor, so a following soft keyword is not an
 * expression operator (`typeof(_Atomic(int) orelse 0)` is type-junk). */
static bool close_paren_ends_type_specifier_ctor(Token *tok) {
	if (!tok || !match_ch(tok, ')') || !tok_match(tok)) return false;
	Token *open = tok_match(tok);
	Token *before_open = tok_walk_back(tok_idx(open), WB_PAST_NOISE);
	if (!before_open) return false;
	if (before_open->tag & (TT_TYPEOF | TT_BITINT | TT_ALIGNAS)) return true;
	return (before_open->tag & TT_TYPE) && equal(before_open, "_Atomic");
}

static bool orelse_is_label_or_goto_target(Token *tok, Token *prev) {
	if (prev && ((prev->tag & TT_GOTO) || is_gnu_label_decl_head(prev) || _equal_2(prev, "&&")))
		return true;
	if (!tok) return false;
	Token *next = skip_noise(tok_next(tok));
	return next && match_ch(next, ':') && !(tok_next(next) && match_ch(tok_next(next), ':'));
}

static inline bool decl_paren_predecessor_is_type(Token *p) {
	return p && (is_type_keyword(p) || (p->tag & (TT_TYPEOF | TT_QUALIFIER | TT_SUE)) ||
		     match_ch(p, '*') || is_known_typedef(p));
}

static inline bool is_array_bracket_predecessor(Token *t) {
	if (is_type_keyword(t) || (t->tag & TT_QUALIFIER) || is_known_typedef(t) || (match_ch(t, '*')) ||
	    (match_ch(t, '}')))
		return true;
	if (is_identifier_like(t)) {
		Token *b = tok_walk_back(tok_idx(t), WB_PAST_NOISE);
		return b && (b->tag & TT_SUE);
	}
	if (match_ch(t, ']')) {
		Token *open = tok_match(t);
		if (!open) return true;
		Token *before_open = tok_walk_back(tok_idx(open), WB_PAST_NOISE);
		if (!before_open) return true;
		if (decl_paren_predecessor_is_type(before_open)) return true;
		if (match_ch(before_open, ']')) return is_array_bracket_predecessor(before_open);
		return false;
	}
	if (match_ch(t, ')')) {
		Token *open = tok_match(t);
		if (!open) return true; // no match info — conservatively assume type
		Token *before_open = tok_walk_back(tok_idx(open), WB_PAST_NOISE);
		if (!before_open) return true;
		return decl_paren_predecessor_is_type(before_open);
	}
	return false;
}

static bool array_size_is_vla_impl(Token *open_bracket, int depth) {
	if (depth > 256) error_tok(open_bracket, "array dimension nesting depth exceeds 256");
	Token *close = tok_match(open_bracket);
	if (!close) return false;
	Token *tok = tok_next(open_bracket);
	while (tok != close) {
		if (match_ch(tok, '[')) {
			if (array_size_is_vla_impl(tok, depth + 1)) return true;
			tok = skip_balanced_group(tok);
			continue;
		}
		if (match_ch(tok, '(') && tok_next(tok) && match_ch(tok_next(tok), '{')) return true;
		if (tok->tag & TT_GENERIC) return true;
		SKIP_NOISE_CONTINUE(tok);
		if (is_sizeof_like(tok)) {
			bool is_sizeof = tok->ch0 == 's';
			tok = tok_next(tok);
			if (tok != close && match_ch(tok, '(')) {
				Token *end = skip_balanced_group(tok);
				if (is_sizeof) {
					Token *prev_inner = tok;
					for (Token *inner = tok_next(tok); inner && inner != end;
					     prev_inner = inner, inner = tok_next(inner)) {
						if (is_enum_kw(inner)) {
							Token *brace = find_struct_body_brace(inner);
							if (brace) {
								/* Register enumerators so later
								 * identifiers in the same dimension
								 * (e.g. sizeof(enum { A = 5 }) + A)
								 * are not misclassified as VLA. */
								parse_enum_constants(brace, 0);
								inner = skip_balanced_group(brace);
								if (inner == end) break;
								continue;
							}
						}
						int vla_fl = typedef_flags(inner) & (TDF_VLA | TDF_PARAM);
						if (vla_fl & TDF_VLA) {
							if (!(vla_fl & TDF_PARAM)) {
								Token *la = skip_noise(tok_next(inner));
								if (la && la != end && match_ch(la, ')'))
									return true;
							}
							Token *ni = tok_next(inner);
							bool has_next = ni && ni != end;
							Token *eff_prev = prev_inner;
							uint32_t pi = tok_idx(eff_prev);
							while (eff_prev->ch0 == '(' && pi > tok_idx(tok) + 1)
								eff_prev = &token_pool[--pi];
							Token *eff_next = ni;
							while (has_next && eff_next && eff_next->ch0 == ')' &&
							       eff_next != end) {
								eff_next = tok_next(eff_next);
								has_next = eff_next && eff_next != end;
							}
							bool deref =
							    (eff_prev->len == 1 &&
							     (eff_prev->ch0 == '*' || eff_prev->ch0 == '[' ||
							      eff_prev->ch0 == '+' ||
							      eff_prev->ch0 == '-')) ||
							    (has_next && eff_next && eff_next->len == 1 &&
							     (eff_next->ch0 == '[' || eff_next->ch0 == '*' ||
							      ((eff_next->ch0 == '+' ||
								eff_next->ch0 == '-') &&
							       !(inner->kind == TK_IDENT &&
								 (vla_fl & TDF_VLA)))));
							if (deref) return true;
						}
						if (match_ch(inner, '[') &&
						    is_array_bracket_predecessor(prev_inner)) {
							if (array_size_is_vla_impl(inner, depth + 1))
								return true;
							inner = tok_match(inner);
							if (!inner || inner == end) break;
							continue;
						}
						if (is_valid_varname(inner) && !is_type_keyword(inner) &&
						    !is_known_typedef(inner) && !is_known_enum_const(inner) &&
						    tok_next(inner) && inner != end &&
						    match_ch(tok_next(inner), '(')) {
							Token *call_end =
							    skip_balanced_group(tok_next(inner));
							bool is_deref = (match_ch(prev_inner, '*')) ||
									(call_end && call_end != end &&
									 ((match_ch(call_end, '[')) ||
									  (call_end->tag & TT_MEMBER)));
							if (is_deref)
								for (Token *a = tok_next(tok_next(inner));
								     a && a != call_end;
								     a = tok_next(a))
									if (is_valid_varname(a) &&
									    !is_known_enum_const(a) &&
									    !is_type_keyword(a))
										return true;
							prev_inner = inner;
							inner = call_end;
							if (!inner || inner == end) break;
						}
					}
				}
				tok = end;
				if (tok != close && match_ch(tok, '{')) tok = skip_balanced_group(tok);
			} else if (tok != close) {
				while (tok != close) {
					SKIP_NOISE_CONTINUE(tok);
					if ((tok->len == 1 &&
					     (tok->ch0 == '*' || tok->ch0 == '&' || tok->ch0 == '!' ||
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
						Token *m = tok_match(tok);
						if (m) tok = tok_next(m);
						if (tok != close && match_ch(tok, '{')) {
							m = tok_match(tok);
							if (m) tok = tok_next(m);
						}
					} else {
						if (is_identifier_like(tok) && is_vla_typedef(tok))
							return true;
						tok = tok_next(tok);
					}
				}
				while (tok != close) {
					if (tok->tag & TT_MEMBER) {
						tok = tok_next(tok);
						if (tok != close) tok = tok_next(tok);
					} else if (tok->flags & TF_OPEN) {
						Token *m = tok_match(tok);
						if (!m) break;
						tok = tok_next(m);
					} else
						break;
				}
			}
			continue;
		}

		if ((tok->tag & TT_MEMBER) ||
		    (is_valid_varname(tok) && !is_known_enum_const(tok) && !is_type_keyword(tok)))
			return true;
		tok = tok_next(tok);
	}
	return false;
}

static inline bool array_size_is_vla(Token *open_bracket) {
	return array_size_is_vla_impl(open_bracket, 0);
}

static Token *c_declarator_array_dims(Token *tok, bool *is_vla) {
	for (;;) {
		if (!tok || tok->kind == TK_EOF) return tok;
		if (match_ch(tok, '[')) {
			tok_ann(tok) |= P1_DECL_BRACKET;
			if (tok->flags & TF_C23_ATTR) {
				tok = skip_balanced_group(tok);
				continue;
			}
			if (array_size_is_vla(tok)) *is_vla = true;
			tok = skip_balanced_group(tok);
			continue;
		}
		/* GNU attributes and preprocessor noise may separate array dims. */
		Token *after = skip_noise(tok);
		if (after != tok && match_ch(after, '[')) {
			tok = after;
			continue;
		}
		return after;
	}
}

/* Pure C declarator analysis. Emission is intentionally outside this API so
 * parse.c can be reused without depending on Prism's output state. */
static DeclResult c_parse_declarator(Token *tok) {
	DeclResult r = {.end = tok};
	bool is_vla = false;
	int ptr_depth = 0;

#define C_DECL_EAT_PTRS(extra_ptr_action)                                                                    \
	while (tok && tok->kind != TK_EOF) {                                                                 \
		Token *_n = skip_noise(tok);                                                                  \
		if (_n != tok) {                                                                             \
			tok = _n;                                                                            \
			continue;                                                                            \
		}                                                                                            \
		if (match_ch(tok, '*')) {                                                                    \
			r.is_pointer = true;                                                                 \
			r.is_const = false;                                                                  \
			extra_ptr_action;                                                                    \
			if (++ptr_depth > 1024) {                                                            \
				warn_tok(tok, "pointer depth exceeds 1024; zero-initialization skipped");    \
				r.end = NULL;                                                                \
				return r;                                                                    \
			}                                                                                    \
			tok = tok_next(tok);                                                                 \
		} else if ((tok->tag & TT_QUALIFIER) &&                                                      \
			   !(is_soft_keyword_identifier(tok) && soft_keyword_decl_name_boundary(tok))) {     \
			if (r.is_pointer && (tok->tag & TT_CONST)) r.is_const = true;                        \
			tok = tok_next(tok);                                                                 \
		} else                                                                                       \
			break;                                                                               \
	}

	C_DECL_EAT_PTRS((void)0)

	int nested_paren = 0;
	if (match_ch(tok, '(')) {
		Token *peek = skip_noise(tok_next(tok));
		if (!match_ch(peek, '*') && !match_ch(peek, '(') && !is_valid_varname(peek)) {
			r.end = NULL;
			return r;
		}
		tok = tok_next(tok);
		nested_paren = 1;
		r.has_paren = true;
		C_DECL_EAT_PTRS(r.paren_pointer = true)
		while (match_ch(tok, '(')) {
			if (++nested_paren > 1024) {
				warn_tok(tok, "parenthesization depth exceeds 1024");
				r.end = NULL;
				return r;
			}
			tok = tok_next(tok);
			C_DECL_EAT_PTRS(r.paren_pointer = true)
		}
	}
#undef C_DECL_EAT_PTRS

	if (!is_valid_varname(tok)) {
		r.end = NULL;
		return r;
	}
	r.var_name = tok;
	tok = skip_noise(tok_next(tok));
	if (r.has_paren && match_ch(tok, '(')) r.is_func_decl = true;
	if (r.has_paren && match_ch(tok, '[')) {
		r.is_array = r.paren_array = true;
		tok = c_declarator_array_dims(tok, &is_vla);
	}
	while (r.has_paren && nested_paren > 0) {
		while (match_ch(tok, '(') || match_ch(tok, '[')) {
			if (match_ch(tok, '(')) tok = skip_balanced_group(tok);
			else {
				r.is_array = r.paren_array = true;
				tok = c_declarator_array_dims(tok, &is_vla);
			}
		}
		if (!match_ch(tok, ')')) {
			r.end = NULL;
			return r;
		}
		tok = tok_next(tok);
		nested_paren--;
	}

	if (match_ch(tok, '(')) {
		if (!r.has_paren) {
			r.end = NULL;
			return r;
		}
		r.is_func_ptr = true;
		tok = skip_balanced_group(tok);
	}
	if (match_ch(tok, '[')) {
		r.is_array = true;
		tok = c_declarator_array_dims(tok, &is_vla);
	}
	while (tok && tok->kind != TK_EOF) {
		Token *next = skip_noise(tok);
		if (next != tok) {
			tok = next;
			continue;
		}
		if (!(tok->tag & TT_ASM)) break;
		tok = tok_next(tok);
		if (tok && match_ch(tok, '(')) tok = skip_balanced_group(tok);
	}

	r.has_init = match_ch(tok, '=');
	r.is_vla = is_vla;
	r.end = tok;
	return r;
}

// Field declarator names share the member namespace — do not resolve them via
// ordinary typedef_lookup (ISO C11 §6.2.3).
static inline bool struct_body_id_is_field_name(Token *id) {
	if (!is_identifier_like(id)) return false;
	Token *nx = skip_noise(tok_next(id));
	if (!nx || nx->len != 1) return false;
	switch (nx->ch0) {
	case ';':
	case ',':
	case ':':
	case '[':
	case '=': return true;
	default: return false;
	}
}

// Ordinary identifiers can shadow tag names (C11 §6.2.3).
static inline bool struct_body_field_pred(Token *id, Token *prev, bool vla) {
	if (!is_identifier_like(id) || struct_body_id_is_field_name(id)) return false;
	if (prev && (prev->tag & TT_SUE)) {
		TypedefEntry *te = tag_lookup(id);
		if (vla) return te && te->is_struct_tag && te->is_vla;
		if (te && te->has_volatile_member) return true;
	}
	return vla ? is_vla_typedef(id) : (is_volatile_typedef(id) || has_volatile_member_typedef(id));
}

static inline bool struct_body_field_is_vla_typedef(Token *id, Token *prev) {
	return struct_body_field_pred(id, prev, true);
}

enum StructBodyScan { SBS_VLA, SBS_VOL };

static bool struct_body_scan(Token *brace, enum StructBodyScan what) {
	if (!brace || !match_ch(brace, '{') || !tok_match(brace)) return false;
	Token *end = tok_match(brace);
	Token *prev = brace;
	for (Token *t = tok_next(brace); t && t != end; prev = t, t = tok_next(t)) {
		if (match_ch(t, '{')) {
			if (struct_body_scan(t, what)) return true;
			prev = t;
			t = tok_match(t);
			continue;
		}
		// Don't skip typeof()/_Atomic() parens — VLA dims / volatile hide inside.
		if ((t->flags & TF_OPEN) && !match_ch(t, what == SBS_VLA ? '[' : '{') &&
		    !(prev && ((prev->tag & TT_TYPEOF) ||
			       ((prev->tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE))))) {
			prev = t;
			t = tok_match(t);
			continue;
		}
		if (what == SBS_VLA) {
			if (match_ch(t, '[') && array_size_is_vla(t)) return true;
			if (struct_body_field_is_vla_typedef(t, prev)) return true;
		} else {
			if (t->tag & TT_VOLATILE) return true;
			if (struct_body_field_pred(t, prev, false)) return true;
		}
	}
	return false;
}

static inline bool struct_body_contains_vla(Token *brace) {
	return struct_body_scan(brace, SBS_VLA);
}

static inline bool struct_body_contains_volatile(Token *brace) {
	return struct_body_scan(brace, SBS_VOL);
}

static bool typedef_contains_vla(Token *tok) {
	while (tok && tok->kind != TK_EOF) {
		if (match_ch(tok, ';')) break;
		if ((tok->flags & TF_OPEN) && !(match_ch(tok, '['))) {
			tok = tok_next(tok_match(tok));
			continue;
		}
		if (match_ch(tok, '[') && array_size_is_vla(tok)) return true;
		tok = tok_next(tok);
	}
	return false;
}

static Token *find_boundary_comma(Token *tok) {
	while (tok->kind != TK_EOF) {
		if (tok->flags & TF_OPEN) {
			tok = tok_next(tok_match(tok));
			continue;
		}
		if (match_ch(tok, ';')) return NULL;
		if (match_ch(tok, ',')) {
			Token *n = tok_next(tok);
			if (n) {
				if (match_ch(n, '(')) {
					Token *inside = tok_next(n);
					if (inside &&
					    !(inside->tag & (TT_TYPE | TT_SUE | TT_TYPEOF | TT_QUALIFIER)) &&
					    !is_known_typedef(inside) && !is_c23_attr(inside))
						return tok;
				} else if ((match_ch(n, '*')) || (n->tag & TT_QUALIFIER) ||
					   (n->tag & TT_ATTR) || is_c23_attr(n)) {
					return tok;
				} else if (is_valid_varname(n) &&
					   !(n->tag & (TT_TYPE | TT_SUE | TT_TYPEOF))) {
					if (tok_next(n) && match_ch(tok_next(n), '(')) {
						Token *inside = tok_next(tok_next(n));
						if (inside && (inside->tag &
							       (TT_TYPE | TT_SUE | TT_TYPEOF | TT_QUALIFIER)))
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
		else if (s->flags & TF_CLOSE)
			pd--;
		else if (pd == 0 && match_ch(s, ';'))
			return s;
	}
	return NULL;
}

// not an array-of-VLA object — must not set TypeSpecResult.is_vla / is_array.
static bool abstract_declarator_paren_is_pointer_only(Token *open_paren) {
	Token *close = tok_match(open_paren);
	if (!close || !(match_ch(open_paren, '('))) return false;
	for (Token *x = tok_next(open_paren); x && x != close;) {
		x = skip_noise(x);
		if (!x || x == close) break;
		if (match_ch(x, '*')) {
			x = tok_next(x);
			continue;
		}
		if (x->tag & TT_QUALIFIER) {
			x = tok_next(x);
			continue;
		}
		if (match_ch(x, '(') && (x->flags & TF_OPEN)) {
			Token *inner_close = tok_match(x);
			if (!inner_close) return false;
			if (!abstract_declarator_paren_is_pointer_only(x)) return false;
			x = tok_next(inner_close);
			continue;
		}
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

static void scan_paren_for_vla(Token *open, Token *end, TypeSpecResult *r, bool check_typeof) {
	Token *prev = open;
	int fn_skip = 0;
	for (Token *t = tok_next(open); t && t != end; prev = t, t = tok_next(t)) {
		if (is_c23_attr(t) && tok_match(t)) {
			prev = t;
			t = tok_match(t);
			continue;
		}
		// Ban control-flow keywords inside type specifier parens.
		// `defer` as an *identifier* (e.g. `typeof(defer)` after
		// `int defer;`) must not trip this — only block-form
		// `defer { ... }` is a real control-flow use here.
		if (t->tag & (TT_GOTO | TT_RETURN | TT_BREAK | TT_CONTINUE)) {
			if (FEAT(F_WARN_SAFETY))
				warn_tok(t,
					 "control flow keywords inside type "
					 "specifiers (typeof() / _Atomic()) may "
					 "corrupt control-flow tracking");
			else
				error_tok(t,
					  "control flow keywords are not "
					  "allowed inside type specifiers "
					  "(typeof() / _Atomic()); transpiler "
					  "rewrites may duplicate the type "
					  "specifier, which would corrupt "
					  "control-flow tracking");
		} else if ((t->tag & TT_DEFER) && !typedef_lookup(t) && tok_next(t) &&
			   (match_ch(tok_next(t), '{') || is_identifier_like(tok_next(t)))) {
			/* Statement form `defer cleanup();` and block form `defer {`
			 * are keyword uses; a bare/operator-adjacent `defer` (e.g.
			 * `typeof(defer)`, `defer + 1`) stays an identifier. */
			if (FEAT(F_WARN_SAFETY))
				warn_tok(t,
					 "control flow keywords inside type "
					 "specifiers (typeof() / _Atomic()) may "
					 "corrupt control-flow tracking");
			else
				error_tok(t,
					  "control flow keywords are not "
					  "allowed inside type specifiers "
					  "(typeof() / _Atomic()); transpiler "
					  "rewrites may duplicate the type "
					  "specifier, which would corrupt "
					  "control-flow tracking");
		}
		if (check_typeof && (t->tag & TT_TYPEOF)) r->has_typeof = true;
		if (match_ch(t, '(')) {
			if (fn_skip > 0) fn_skip++;
			else if (match_ch(prev, ')'))
				fn_skip = 1;
		} else if (match_ch(t, ')')) {
			if (fn_skip > 0) fn_skip--;
		}
		if (fn_skip > 0) continue;
		if (!check_typeof && is_sizeof_like(t)) {
			Token *nx = tok_next(t);
			if (nx && match_ch(nx, '(') && tok_match(nx)) {
				prev = t;
				t = tok_match(nx);
				continue;
			}
		}
		if (is_enum_kw(t)) {
			Token *brace = find_struct_body_brace(t);
			if (brace) {
				parse_enum_constants(brace, 0);
				prev = brace;
				t = tok_match(brace);
				continue;
			}
		}
		if (match_ch(t, '[') && !(t->flags & TF_C23_ATTR) && is_array_bracket_predecessor(prev)) {
			if (array_size_is_vla(t)) r->type_vm = true;
			if (!array_bracket_closes_ptr_to_array(t, prev)) {
				r->is_array = true;
				if (r->type_array_rank < 15) r->type_array_rank++;
				if (array_size_is_vla(t)) r->is_vla = true;
			}
			continue;
		}
		if (is_identifier_like(t)) {
			int tf = typedef_flags(t);
			if (tf & TDF_VLA) {
				r->is_vla = true;
				if (tf & TDF_ARRAY) {
					r->is_array = true;
					uint8_t rk = array_rank_for_tok(t);
					if (rk > 0 && r->type_array_rank < rk) r->type_array_rank = rk;
				}
				break;
			}
			/* `typeof(a)` / `_Atomic(a)` where `a` is a fixed array shadow. */
			if (tf & TDF_ARRAY) {
				r->is_array = true;
				uint8_t rk = array_rank_for_tok(t);
				if (rk > 0 && r->type_array_rank < rk)
					r->type_array_rank = rk;
				else if (r->type_array_rank == 0)
					r->type_array_rank = 1;
				break;
			}
		}
	}
}

// --- Type Specifier Parser ---

#define SKIP_RAW(after, last)                                                                                \
	do {                                                                                                 \
		while ((after) && ((after)->flags & TF_RAW) && !is_known_typedef(after)) {                   \
			(last) = (after);                                                                    \
			(after) = skip_noise(tok_next(after));                                               \
		}                                                                                            \
	} while (0)

static void apply_typespec_storage_quals(TypeSpecResult *r, Token *tok) {
	uint32_t tag = tok->tag;
	if ((tag & TT_STORAGE) && tok->ch0 == 'e') r->has_extern = true;
	if ((tag & TT_STORAGE) && tok->ch0 == 's') r->has_static = true;
	if ((tag & TT_STORAGE) && tok->ch0 != 'e' && tok->ch0 != 's') r->has_thread_local = true;
	if (!(tag & TT_QUALIFIER)) return;
	if (tag & TT_VOLATILE) r->has_volatile = true;
	if (tag & TT_REGISTER) r->has_register = true;
	if (tag & TT_CONST) r->has_const = true;
	if (tok->ch0 == 'c' && tok->len == 9) r->has_constexpr = true;
	if (tag & TT_TYPE) {
		if (tok->ch0 == 'a') {
			r->saw_type = true;
			r->has_auto = true;
		} else
			r->has_atomic = true;
	}
}

// Soft-kw typedefs skip soft-kw qualifiers when peeking the name; plain typedefs skip all quals.
static bool typespec_typedef_name_finishes(Token *tok, bool soft) {
	Token *peek = tok_next(tok);
	while (peek && (peek->tag & TT_QUALIFIER) && (!soft || !is_soft_keyword_identifier(peek)))
		peek = tok_next(peek);
	if (!peek || !is_valid_varname(peek)) return false;
	Token *after = tok_next(peek);
	return after && match_set(after, CH(';') | CH('[') | CH(',') | CH('='));
}

static TypeSpecResult parse_type_specifier(Token *tok) {
	TypeSpecResult r = {.end = tok};
	while (tok && tok->kind != TK_EOF) {
		Token *next = skip_noise(tok);
		if (next != tok) {
			tok = next;
			r.end = tok;
			continue;
		}

		if (!r.saw_type && (tok->flags & TF_RAW)) {
			Token *after_raw = skip_noise(tok_next(tok));
			bool typedef_kw_prefix =
			    is_known_typedef(tok) && after_raw &&
			    (is_type_keyword(after_raw) || is_known_typedef(after_raw) ||
			     (after_raw->tag &
			      (TT_QUALIFIER | TT_SUE | TT_STORAGE | TT_INLINE | TT_TYPEDEF)) ||
			     (after_raw->flags & TF_RAW));
			if (!is_known_typedef(tok) || typedef_kw_prefix) {
				r.has_raw = true;
				Token *after = after_raw;
				Token *last = tok;
				SKIP_RAW(after, last);
				tok = tok_next(last);
				r.end = tok;
				continue;
			}
		}

		uint32_t tag = tok->tag;
		bool is_type = is_type_keyword(tok);
		if (r.saw_type && is_soft_keyword_identifier(tok) && soft_keyword_decl_name_boundary(tok))
			break;
		if (!(tag & (TT_QUALIFIER | TT_STORAGE | TT_INLINE)) && !is_type &&
		    !(tag & (TT_BITINT | TT_ALIGNAS)))
			break;
		if ((tag & TT_INLINE) && !(tag & (TT_QUALIFIER | TT_STORAGE))) {
			tok = tok_next(tok);
			r.end = tok;
			continue;
		}

		if (equal(tok, "void") || is_void_typedef(tok)) r.has_void = true;
		bool had_type = r.saw_type;
		int tflags = typedef_flags(tok);
		if ((tflags & TDF_TYPEDEF) && is_soft_keyword_identifier(tok)) {
			if (had_type) break;
			typedef_apply_tdf_flags(&r, tflags);
			if (typespec_typedef_name_finishes(tok, true)) {
				tok = tok_next(tok);
				r.end = tok;
				r.saw_type = true;
				return r;
			}
			tok = tok_next(tok);
			r.end = tok;
			r.saw_type = true;
			continue;
		}

		apply_typespec_storage_quals(&r, tok);

		if (is_type && (tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE) &&
		    !(tok_next(tok) && match_ch(tok_next(tok), '(')))
			is_type = false;
		if (is_type) r.saw_type = true;
		is_type = false;
		// _Atomic(type) specifier form
		if ((tag & (TT_QUALIFIER | TT_TYPE)) == (TT_QUALIFIER | TT_TYPE) && tok_next(tok) &&
		    match_ch(tok_next(tok), '(')) {
			r.saw_type = true;
			r.has_atomic = true;
			tok = tok_next(tok);
			Token *inner_start = skip_noise(tok_next(tok));
			Token *end = skip_balanced_group(tok);
			if (inner_start && (inner_start->tag & TT_SUE)) {
				r.is_struct = true;
				if (inner_start->ch0 == 'u') r.is_union = true;
				if (inner_start->ch0 == 'e') r.is_enum = true;
			}
			if (inner_start && is_identifier_like(inner_start) && is_known_typedef(inner_start)) {
				r.is_typedef = true;
				if (typedef_flags(inner_start) & TDF_UNION) r.is_union = true;
			}
			/* `_Atomic(struct S *)` is a pointer, not a struct value. */
			{
				bool outer_ptr = false;
				int depth = 0;
				for (Token *t = tok_next(tok); t && t != end; t = tok_next(t)) {
					if (t->flags & TF_OPEN) {
						depth++;
						continue;
					}
					if (t->flags & TF_CLOSE) {
						if (depth > 0) depth--;
						continue;
					}
					if (depth == 0 && match_ch(t, '*')) outer_ptr = true;
				}
				if (outer_ptr) {
					r.is_struct = false;
					r.is_union = false;
					r.is_enum = false;
				}
			}
			scan_paren_for_vla(tok, end, &r, true);
			tok = end;
			r.end = tok;
			continue;
		}

		if (tag & TT_SUE) {
			r.is_struct = true;
			if (tok->ch0 == 'u') r.is_union = true;
			if (tok->ch0 == 'e') r.is_enum = true;
			r.saw_type = true;
			tok = tok_next(tok);
			while (tok && tok->kind != TK_EOF) {
				SKIP_NOISE_CONTINUE(tok);
				if ((tok->tag & TT_QUALIFIER) && !is_soft_keyword_identifier(tok))
					tok = tok_next(tok);
				else
					break;
			}
			Token *sue_tag = NULL;
			if (tok && is_valid_varname(tok)) {
				sue_tag = tok;
				tok = tok_next(tok);
			}
			// C23 enum fixed underlying type: enum E : int { ... }
			if (tok && match_ch(tok, ':')) {
				tok = tok_next(tok);
				while (tok && tok->kind != TK_EOF) {
					SKIP_NOISE_CONTINUE(tok);
					if (match_ch(tok, '(')) {
						tok = skip_balanced_group(tok);
						continue;
					}
					if (is_c23_attr(tok) && tok_match(tok)) {
						tok = tok_match(tok);
						continue;
					}
					if (is_type_keyword(tok) || (tok->tag & TT_QUALIFIER)) {
						tok = tok_next(tok);
						continue;
					}
					break;
				}
			}
			if (tok && match_ch(tok, '{')) {
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

		if (tag & TT_TYPEOF) {
			bool is_unqual =
			    tok->len >= 13; // typeof_unqual(13), __typeof_unqual(15), __typeof_unqual__(17)
			r.saw_type = true;
			r.has_typeof = true;
			if (equal(tok, "__auto_type")) r.has_auto = true;
			tok = tok_next(tok);
			if (tok && match_ch(tok, '(')) {
				Token *end = skip_balanced_group(tok);
				if (tok_next(tok) && equal(tok_next(tok), "void") &&
				    tok_next(tok_next(tok)) == tok_match(tok))
					r.has_void = true;
				{
					bool saw_sue = false;
					bool outer_ptr = false;
					int depth = 0;
					for (Token *t = tok_next(tok); t && t != end; t = tok_next(t)) {
						if ((t->tag & TT_ATTR) && tok_next(t) &&
						    match_ch(tok_next(t), '(') && tok_match(tok_next(t))) {
							t = tok_match(tok_next(t));
							continue;
						}
						if (is_c23_attr(t) && tok_match(t)) {
							t = tok_match(t);
							continue;
						}
						if (match_ch(t, '{') && saw_sue) {
							if (struct_body_contains_volatile(t))
								r.has_volatile_member = true;
							t = tok_match(t);
							saw_sue = false;
							continue;
						}
						if (t->flags & TF_OPEN) {
							depth++;
							continue;
						}
						if (t->flags & TF_CLOSE) {
							if (depth > 0) depth--;
							continue;
						}
						/* `typeof(struct S *)` is a pointer type — the SUE
						 * must not mark the typeof result as a struct value. */
						if (depth == 0 && match_ch(t, '*')) outer_ptr = true;
						if (!is_unqual) {
							if (t->tag & TT_VOLATILE) r.has_volatile = true;
							if (t->tag & TT_CONST) r.has_const = true;
							if ((t->tag & (TT_QUALIFIER | TT_TYPE)) ==
							    (TT_QUALIFIER | TT_TYPE))
								r.has_atomic = true;
						}
						if ((t->tag & TT_SUE) || (typedef_flags(t) & TDF_AGGREGATE))
							r.is_struct = true;
						if ((t->tag & TT_SUE) && t->ch0 == 'u') r.is_union = true;
						if ((t->tag & TT_SUE) && t->ch0 == 'e') r.is_enum = true;
						if (typedef_flags(t) & TDF_UNION) r.is_union = true;
						if (t->tag & TT_SUE) {
							saw_sue = true;
							continue;
						}
						if (is_identifier_like(t)) {
							// ISO C11 §6.2.3 namespace separation.
							if (saw_sue) {
								TypedefEntry *tag_e = tag_lookup(t);
								if (tag_e) {
									if (tag_e->is_vla) r.is_vla = true;
									if (tag_e->has_volatile_member)
										r.has_volatile_member = true;
								}
								saw_sue = false;
							}
							if (!is_unqual) {
								int tf = typedef_flags(t);
								if (tf & TDF_VOLATILE) r.has_volatile = true;
								if (tf & TDF_HAS_VOL_MEMBER)
									r.has_volatile_member = true;
							}
						}
					}
					if (outer_ptr) {
						r.is_struct = false;
						r.is_union = false;
						r.is_enum = false;
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
			if (tag & TT_ALIGNAS) r.has_alignas = true;
			Token *kw = tok;
			tok = tok_next(tok);
			if (tok && match_ch(tok, '(')) {
				if (FEAT(F_ORELSE) && (kw->tag & (TT_BITINT | TT_ALIGNAS))) {
					Token *close = tok_match(tok);
					for (Token *s = tok_next(tok); s && s != close; s = tok_next(s))
						if (is_orelse_kw_shadow(s))
							error_tok(
							    s,
							    "'orelse' cannot be used inside %s "
							    "(requires a compile-time constant expression)",
							    (kw->tag & TT_BITINT) ? "_BitInt()"
										  : "_Alignas()");
				}
				tok = skip_balanced_group(tok);
			}
			r.end = tok;
			continue;
		}

		tflags = typedef_flags(tok);
		if (tflags & TDF_TYPEDEF) {
			if (had_type) break;
			typedef_apply_tdf_flags(&r, tflags);
			if (typespec_typedef_name_finishes(tok, false)) {
				tok = tok_next(tok);
				r.end = tok;
				r.saw_type = true;
				return r;
			}
		}

		tok = tok_next(tok);
		r.end = tok;
	}

	return r;
}

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
			if (is_const_typedef(t)) {
				base_is_const = true;
				break;
			}
	}

	bool base_is_volatile = type_spec.has_volatile;
	if (!base_is_volatile) {
		for (Token *t = type_start; t && t != tok; t = tok_next(t))
			if (is_volatile_typedef(t)) {
				base_is_volatile = true;
				break;
			}
	}

	bool base_has_volatile_member = type_spec.has_volatile_member;
	if (!base_has_volatile_member) {
		for (Token *t = type_start; t && t != tok; t = tok_next(t))
			if (has_volatile_member_typedef(t)) {
				base_has_volatile_member = true;
				break;
			}
	}

	bool base_is_void = type_spec.has_void;
	bool base_is_ptr = false;
	bool base_is_array = false;
	bool base_is_func = false;
	uint8_t base_array_rank = 0;
	for (Token *bt = type_start; bt && bt != type_spec.end; bt = tok_next(bt)) {
		if (is_ptr_typedef(bt)) {
			base_is_ptr = true;
			break;
		}
		if (is_array_typedef(bt)) {
			base_is_array = true;
			base_array_rank = array_rank_for_tok(bt);
			break;
		}
		if (is_func_typedef(bt)) {
			base_is_func = true;
			break;
		}
	}

	// redefinitions correctly shadow outer tags (C11 §6.2.1p4).
	if (type_spec.is_struct) {
		for (Token *bt = type_start; bt && bt != type_spec.end; bt = tok_next(bt)) {
			if (bt->tag & TT_SUE) {
				Token *tag = skip_noise(tok_next(bt));
				while (tag && (tag->tag & TT_QUALIFIER) && !is_soft_keyword_identifier(tag))
					tag = skip_noise(tok_next(tag));
				if (tag && is_valid_varname(tag)) {
					int pre = typedef_table.count;
					typedef_add_entry(tok_loc(tag),
							  tag->len,
							  scope_depth,
							  TDK_STRUCT_TAG,
							  is_vla,
							  false);
					if (typedef_table.count > pre) {
						TypedefEntry *te =
						    &typedef_table.entries[typedef_table.count - 1];
						te->token_index = tok_idx(tag);
						te->is_aggregate = !type_spec.is_enum;
						if (base_has_volatile_member) te->has_volatile_member = true;
					}
				}
				break;
			}
		}
	}
	while (tok && !(match_ch(tok, ';')) && tok->kind != TK_EOF) {
		DeclResult decl = c_parse_declarator(tok);
		if (decl.var_name) {
			bool is_void =
			    base_is_void && !decl.is_pointer && !decl.is_array && !decl.is_func_ptr;
			bool is_const = (decl.is_pointer || decl.is_func_ptr) ? decl.is_const : base_is_const;
			bool is_ptr = decl.is_pointer || decl.is_func_ptr || base_is_ptr;
			int pre_count = typedef_table.count;
			typedef_add_entry(tok_loc(decl.var_name),
					  decl.var_name->len,
					  scope_depth,
					  TDK_TYPEDEF,
					  is_vla,
					  is_void);
			if (typedef_table.count > pre_count) {
				TypedefEntry *added = &typedef_table.entries[typedef_table.count - 1];
				added->token_index = tok_idx(decl.var_name);
				if (is_const) added->is_const = true;
				bool is_vol =
				    (decl.is_pointer || decl.is_func_ptr) ? false : base_is_volatile;
				if (is_vol) added->is_volatile = true;
				if (base_has_volatile_member && !decl.is_pointer && !decl.is_func_ptr)
					added->has_volatile_member = true;
				if (is_ptr) added->is_ptr = true;
				if ((decl.is_array || base_is_array) &&
				    (!decl.is_pointer || decl.paren_array) && !decl.is_func_ptr)
					added->is_array = true;
				if ((decl.is_array || base_is_array) &&
				    (!decl.is_pointer || decl.paren_array) && !decl.is_func_ptr) {
					int rank = 0;
					Token *prev_dt = NULL;
					for (Token *dt = decl.var_name; dt && decl.end && dt != decl.end;) {
						if (match_ch(dt, '[') && (dt->flags & TF_OPEN)) {
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
					if (rank > 15) rank = ARRAY_RANK_WRAP_ALL;
					added->array_rank = (uint8_t)rank;
				}
				if ((decl.is_array || base_is_array) &&
				    (!decl.is_pointer || decl.paren_array) && !decl.is_func_ptr) {
					bool dim_complete = false;
					if (decl.is_array) {
						for (Token *dt = decl.var_name;
						     dt && decl.end && dt != decl.end;
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
									dim_complete =
									    bte->array_dim_complete;
								break;
							}
						}
					}
					added->array_dim_complete = dim_complete;
				}
				if (type_spec.is_struct && !type_spec.is_enum && !decl.is_pointer &&
				    !decl.is_func_ptr)
					added->is_aggregate = true;
				if (type_spec.is_union && !decl.is_pointer && !decl.is_func_ptr)
					added->is_union = true;
				if (decl.is_func_decl) added->is_func = true;
				if (type_spec.has_atomic) added->is_atomic = true;
				if (!decl.end) {
					Token *after_name = skip_noise(tok_next(decl.var_name));
					if (after_name && match_ch(after_name, '(')) added->is_func = true;
				}
				if (decl.is_func_ptr && !decl.paren_pointer) added->is_func = true;
				if (base_is_func && !decl.is_pointer && !decl.is_array && !decl.is_func_ptr)
					added->is_func = true;
			}
		}
		tok = decl.end ? decl.end : tok_next(tok);
		while (tok && !(match_ch(tok, ',')) && !(match_ch(tok, ';')) && tok->kind != TK_EOF) {
			if (match_ch(tok, '(')) tok = skip_balanced_group(tok);
			else if (match_ch(tok, '['))
				tok = skip_balanced_group(tok);
			else
				tok = tok_next(tok);
		}

		if (tok && match_ch(tok, ',')) tok = tok_next(tok);
	}
}

static PRISM_THREAD_LOCAL int *sos_do_if_save = NULL;
static PRISM_THREAD_LOCAL int *sos_do_tn_save = NULL;
static PRISM_THREAD_LOCAL int *sos_do_snap_start = NULL;
static PRISM_THREAD_LOCAL int sos_do_cap = 0;
static PRISM_THREAD_LOCAL int *sos_do_snap_buf = NULL;
static PRISM_THREAD_LOCAL int sos_snap_cap = 0;
static PRISM_THREAD_LOCAL int *sos_if_trail_snap = NULL;
static PRISM_THREAD_LOCAL int sos_if_cap = 0;

static inline bool sos_ensure_intbuf(int **buf, int *cap, int need) {
	if (need <= *cap) return true;
	/* Soft-fail variant of VEC_ENSURE_REALLOC for skip_one_stmt OOM path. */
	size_t nc = vec_grow_cap((size_t)*cap, (size_t)need, 128);
	int *p = (int *)realloc(*buf, nc * sizeof(int));
	if (!p) return false;
	*buf = p;
	*cap = (int)nc;
	return true;
}

static inline bool sos_ensure_do(int need) {
	if (need <= sos_do_cap) return true;
	int nc = (int)vec_grow_cap((size_t)sos_do_cap, (size_t)need, 128);
	/* Commit each successful realloc immediately — a partial failure must
	 * not leave a TLS pointer at a block realloc already freed. */
	int *a = (int *)realloc(sos_do_if_save, (size_t)nc * sizeof(int));
	if (a) sos_do_if_save = a;
	int *b = (int *)realloc(sos_do_tn_save, (size_t)nc * sizeof(int));
	if (b) sos_do_tn_save = b;
	int *c = (int *)realloc(sos_do_snap_start, (size_t)nc * sizeof(int));
	if (c) sos_do_snap_start = c;
	if (!a || !b || !c) return false;
	sos_do_cap = nc;
	return true;
}

static inline bool sos_ensure_snap(int need) {
	return sos_ensure_intbuf(&sos_do_snap_buf, &sos_snap_cap, need);
}

static inline bool sos_ensure_if(int need) {
	return sos_ensure_intbuf(&sos_if_trail_snap, &sos_if_cap, need);
}

static Token *skip_one_stmt_impl(Token *tok, uint32_t *cache) {
	int if_depth = 0;
	int do_depth = 0;
	int do_snap_top = 0;
	uint32_t trail[256];
	int tn = 0;
	if (!sos_ensure_do(128) || !sos_ensure_snap(1024) || !sos_ensure_if(512)) return NULL;
	int *do_if_save = sos_do_if_save;
	int *do_tn_save = sos_do_tn_save;
	int *do_snap_start = sos_do_snap_start;
	int *do_snap_buf = sos_do_snap_buf;
	int *if_trail_snap = sos_if_trail_snap;
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

	if (match_ch(tok, '{')) {
		tok = tok_match(tok);
		goto unwind_if;
	}

	if (tok->tag & TT_IF) {
		if (tok->ch0 == 'e') {
			tok = tok_next(tok);
			goto restart;
		}
		Token *p = skip_prep_dirs(tok_next(tok));
		if (!p || !(match_ch(p, '(')) || !tok_match(p)) return NULL;
		if (!sos_ensure_if(if_depth + 1)) return NULL;
		if_trail_snap = sos_if_trail_snap;
		if_trail_snap[if_depth] = tn;
		if_depth++;
		tok = tok_next(tok_match(p));
		goto restart;
	}

	if ((tok->tag & (TT_LOOP | TT_SWITCH)) && tok->ch0 != 'd') {
		Token *p = skip_prep_dirs(tok_next(tok));
		if (!p || !(match_ch(p, '(')) || !tok_match(p)) return NULL;
		tok = tok_next(tok_match(p));
		goto restart;
	}

	if ((tok->tag & TT_LOOP) && tok->ch0 == 'd') {
		if (!sos_ensure_do(do_depth + 1)) return NULL;
		if (!sos_ensure_snap(do_snap_top + if_depth)) return NULL;
		do_if_save = sos_do_if_save;
		do_tn_save = sos_do_tn_save;
		do_snap_start = sos_do_snap_start;
		do_snap_buf = sos_do_snap_buf;
		do_if_save[do_depth] = if_depth;
		do_tn_save[do_depth] = tn;
		do_snap_start[do_depth] = do_snap_top;
		for (int i = 0; i < if_depth; i++) do_snap_buf[do_snap_top++] = if_trail_snap[i];
		do_depth++;
		if_depth = 0;
		tok = tok_next(tok);
		goto restart;
	}

	if (is_identifier_like(tok) &&
	    !(tok->tag & (TT_CASE | TT_DEFAULT | TT_TYPE | TT_QUALIFIER | TT_STORAGE))) {
		Token *colon = skip_noise(tok_next(tok));
		if (colon && match_ch(colon, ':') && !(tok_next(colon) && match_ch(tok_next(colon), ':'))) {
			tok = tok_next(colon);
			goto restart;
		}
	}

	if ((tok->tag & (TT_CASE | TT_DEFAULT)) && !is_known_typedef(tok)) {
		int td = 0;
		for (Token *s = tok_next(tok); s && s->kind != TK_EOF; s = tok_next(s)) {
			if (s->flags & TF_OPEN) {
				s = tok_match(s);
				continue;
			}
			if (match_ch(s, '?')) {
				td++;
				continue;
			}
			if (match_ch(s, ':')) {
				if (td > 0) {
					td--;
					continue;
				}
				tok = tok_next(s);
				goto restart;
			}
		}
		return NULL;
	}

	for (Token *s = tok; s && s->kind != TK_EOF; s = tok_next(s)) {
		if (s->flags & TF_OPEN) {
			s = tok_match(s);
			continue;
		}
		if (match_ch(s, ';')) {
			tok = s;
			goto unwind_if;
		}
	}
	return NULL;

unwind_if:
	while (if_depth > 0) {
		if_depth--;
		if (!tok) return NULL;
		Token *n = skip_prep_dirs(tok_next(tok));
		if (n && (n->tag & TT_IF) && n->ch0 == 'e') {
			int snap = if_trail_snap[if_depth];
			if (cache) {
				uint32_t val = tok_idx(tok) + 1;
				for (int i = snap; i < tn; i++) {
					uint32_t tix = trail[i];
					cache[tix] = val;
				}
			}
			tn = snap; // keep parent tokens in trail for final resolution
			tok = tok_next(n);
			goto restart;
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
		for (int i = 0; i < snap_count; i++) if_trail_snap[i] = do_snap_buf[snap_start + i];
		do_snap_top = snap_start;
		if (!tok) goto unwind_if;
		Token *w = skip_prep_dirs(tok_next(tok));
		if (!w || !(w->tag & TT_LOOP) || w->ch0 != 'w') {
			tok = NULL;
			goto unwind_if;
		}
		Token *p2 = skip_prep_dirs(tok_next(w));
		if (!p2 || !(match_ch(p2, '(')) || !tok_match(p2)) {
			tok = NULL;
			goto unwind_if;
		}
		Token *a = skip_prep_dirs(tok_next(tok_match(p2)));
		tok = (a && match_ch(a, ';')) ? a : NULL;
		goto unwind_if;
	}
	return tok;
}

static Token *skip_one_stmt(Token *tok) {
	return skip_one_stmt_impl(tok, NULL);
}

typedef struct {
	uint16_t parent_id;	// scope_id of enclosing '{' (0 = file scope)
	uint32_t open_tok_idx;	// token index of the '{' (0 for file scope)
	uint32_t close_tok_idx; // token index of the matching '}' (UINT32_MAX for file scope)
	bool is_struct : 1;
	bool is_loop : 1;
	bool is_switch : 1;
	bool is_func_body : 1;
	bool is_stmt_expr : 1;
	bool is_conditional : 1;
	bool is_init : 1; // initializer brace: = { ... } — not a compound statement
	bool is_enum : 1; // set when is_struct=true and the keyword is 'enum'
} ScopeInfo;

#define scope_tree ((ScopeInfo *)ctx->p1_scope_tree)
#define scope_tree_count (ctx->p1_scope_count)
#define scope_tree_cap (ctx->p1_scope_cap)

static bool c_is_objc_ivar_brace(uint32_t brace_idx) {
	for (uint32_t i = brace_idx - 1; i > 0; i--) {
		Token *t = &token_pool[i];
		if (t->kind == TK_PREP_DIR) continue;
		if ((t->kind == TK_IDENT && !t->tag) || match_ch(t, ':') || match_ch(t, '*') ||
		    (t->tag & (TT_QUALIFIER | TT_ATTR)))
			continue;
		if (match_ch(t, ')') && tok_match(t)) {
			i = tok_idx(tok_match(t));
			continue;
		}
		if (match_ch(t, ']') && tok_match(t) && (tok_match(t)->flags & TF_C23_ATTR)) {
			i = tok_idx(tok_match(t));
			continue;
		}
		if (match_ch(t, '>')) {
			int depth = 1;
			while (i > 1 && depth > 0) {
				Token *inner = &token_pool[--i];
				if (inner->kind == TK_PREP_DIR) continue;
				if (match_ch(inner, '>')) depth++;
				else if (match_ch(inner, '<')) depth--;
			}
			continue;
		}
		if (match_ch(t, '@')) {
			Token *kw = &token_pool[i + 1];
			if (kw->kind == TK_IDENT &&
			    ((kw->len == 9 && prism_memeq_static(tok_loc(kw), "interface", 9)) ||
			     (kw->len == 14 && prism_memeq_static(tok_loc(kw), "implementation", 14)) ||
			     (kw->len == 8 && prism_memeq_static(tok_loc(kw), "protocol", 8))))
				return true;
		}
		return false;
	}
	return false;
}

/* Build the reusable C scope index. Each `{` caches its scope ID directly,
 * making all later ownership/CFG queries O(1). Returns whether the TU contains
 * an `orelse` spelling so Prism can gate its extension-only work. */
static bool c_parse_build_scopes(Token *start) {
	scope_tree_count = 1; // 0 is file scope
	scope_tree_cap = 0;
	ctx->p1_scope_tree = NULL;
	int stack_cap = 256, depth = 0;
	uint16_t *stack = arena_alloc_uninit(&ctx->main_arena, stack_cap * sizeof(uint16_t));
	stack[0] = 0;
	bool has_orelse = false;
	for (Token *t = start; t && t->kind != TK_EOF; t = tok_next(t)) {
		if (t->tag & TT_ORELSE) has_orelse = true;
		if (match_ch(t, '}')) {
			if (depth > 0) depth--;
			continue;
		}
		if (!match_ch(t, '{')) continue;

		uint16_t sid = scope_tree_count;
		if (sid == UINT16_MAX) error_tok(t, "scope tree: too many scopes (>65534)");
		ARENA_ENSURE_CAP(
		    &ctx->main_arena, ctx->p1_scope_tree, scope_tree_count, scope_tree_cap, 256, ScopeInfo);
		ScopeInfo *si = &scope_tree[sid];
		*si = (ScopeInfo){.parent_id = stack[depth],
				  .open_tok_idx = tok_idx(t),
				  .close_tok_idx = tok_match(t) ? tok_idx(tok_match(t)) : UINT32_MAX};
		Token *prev = tok_walk_back(tok_idx(t) - 1, WB_SKIP_NOISE);
		if (prev) {
			if (is_do_kw(prev)) {
				si->is_loop = true;
			} else if (match_ch(prev, ')') && tok_match(prev)) {
				Token *open = tok_match(prev);
				Token *kw = tok_walk_back(tok_idx(open) - 1, WB_SKIP_NOISE);
				if (kw && (kw->tag & TT_ATTR)) kw = tok_walk_back(tok_idx(kw) - 1, WB_SKIP_ATTRS);
				if (kw) {
					if (kw->tag & TT_LOOP) si->is_loop = true;
					else if (kw->tag & TT_SWITCH) si->is_switch = true;
					else if (kw->tag & TT_IF) si->is_conditional = true;
					else if (kw->tag & TT_SUE) {
						si->is_struct = true;
						si->is_enum = is_enum_kw(kw);
					}
				}
				if (depth == 0 && !si->is_loop && !si->is_switch && !si->is_conditional &&
				    !si->is_struct)
					si->is_func_body = true;
				if (depth > 0 && !si->is_loop && !si->is_switch && !si->is_conditional &&
				    !si->is_struct && !si->is_func_body) {
					if (c_paren_is_function_params(open)) si->is_func_body = true;
					else si->is_init = true;
				}
			} else if (is_else_kw(prev)) {
				si->is_conditional = true;
			} else if (prev->tag & TT_SUE) {
				si->is_struct = true;
				si->is_enum = is_enum_kw(prev);
			} else if (prev->kind == TK_IDENT &&
				   !(prev->tag &
				     (TT_TYPE | TT_QUALIFIER | TT_LOOP | TT_SWITCH | TT_IF | TT_STORAGE))) {
				Token *sue = tok_walk_back(tok_idx(prev) - 1, WB_SKIP_ATTRS);
				if (sue && (sue->tag & TT_SUE)) {
					si->is_struct = true;
					si->is_enum = is_enum_kw(sue);
				}
			} else if (is_c23_fixed_underlying_enum(prev)) {
				si->is_struct = si->is_enum = true;
			} else if (depth == 0 && (match_ch(prev, ']') || match_ch(prev, ';'))) {
				si->is_func_body = true;
			}
		}

		if (!si->is_struct && !si->is_loop && !si->is_switch && !si->is_conditional &&
		    c_is_objc_ivar_brace(tok_idx(t))) {
			si->is_struct = true;
			si->is_func_body = si->is_init = false;
		}
		if (prev && match_ch(prev, '(')) si->is_stmt_expr = true;
		if (!si->is_func_body && !si->is_loop && !si->is_switch && !si->is_conditional &&
		    !si->is_struct && !si->is_stmt_expr) {
			if (prev && match_ch(prev, '=')) si->is_init = true;
			else if (depth > 0 && stack[depth] < scope_tree_count && scope_tree[stack[depth]].is_init)
				si->is_init = true;
		}

		tok_ann(t) = (si->is_loop ? P1_SCOPE_LOOP : 0) |
			     (si->is_switch ? P1_SCOPE_SWITCH : 0) |
			     (si->is_init ? P1_SCOPE_INIT : 0);
		bool reuse_parent =
		    si->is_init && depth > 0 && stack[depth] < scope_tree_count && scope_tree[stack[depth]].is_init;
		t->parse_data = reuse_parent ? stack[depth] : sid;
		if (!reuse_parent) scope_tree_count++;
		ARENA_ENSURE_CAP(&ctx->main_arena, stack, depth + 2, stack_cap, 256, uint16_t);
		depth++;
		stack[depth] = reuse_parent ? stack[depth - 1] : sid;
	}
	return has_orelse;
}

static bool c_parse_begin(Token *start) {
	c_parse_reset();
	return c_parse_build_scopes(start);
}

static bool scope_is_ancestor_or_self(uint16_t ancestor, uint16_t descendant) {
	for (uint16_t s = descendant; s != 0; s = scope_tree[s].parent_id)
		if (s == ancestor) return true;
	return ancestor == 0; // file scope is ancestor of everything
}

static int scope_tree_depth(uint16_t scope_id) {
	int depth = 0;
	for (uint16_t s = scope_id; s != 0; s = scope_tree[s].parent_id) depth++;
	return depth;
}

static int scope_block_exits(uint16_t goto_sid, uint16_t label_sid) {
	uint16_t a = goto_sid, b = label_sid;
	int da = scope_tree_depth(a), db = scope_tree_depth(b);
	while (da > db) {
		a = scope_tree[a].parent_id;
		da--;
	}
	while (db > da) {
		b = scope_tree[b].parent_id;
		db--;
	}
	while (a != b && a != 0) {
		a = scope_tree[a].parent_id;
		b = scope_tree[b].parent_id;
	}
	uint16_t lca = a;
	int exits = 0;
	for (uint16_t s = goto_sid; s != lca && s != 0; s = scope_tree[s].parent_id)
		if (!scope_tree[s].is_init) exits++;
	return exits;
}

static uint16_t scope_stmt_expr_ancestor(uint16_t scope_id) {
	for (uint16_t s = scope_id; s != 0; s = scope_tree[s].parent_id)
		if (s < scope_tree_count && scope_tree[s].is_stmt_expr) return s;
	return 0;
}

// Phase 1: check if a defer in scope 'sid' is inside a chain of closing braces
static void p1_check_defer_stmt_expr_chain(Token *defer_tok, uint16_t sid) {
	while (sid > 0 && sid < scope_tree_count) {
		uint16_t pid = scope_tree[sid].parent_id;
		if (pid == 0 || pid >= scope_tree_count) break;
		Token *t = tok_next(&token_pool[scope_tree[sid].close_tok_idx]);
		Token *parent_close = &token_pool[scope_tree[pid].close_tok_idx];
		bool only_trivial = true;
		while (t && t != parent_close && t->kind != TK_EOF) {
			if (match_ch(t, ';') || match_ch(t, '}')) {
				t = tok_next(t);
				continue;
			}
			if (t->kind == TK_PREP_DIR) {
				t = tok_next(t);
				continue;
			}
			if (t->tag & TT_ATTR) {
				t = tok_next(t);
				if (t && match_ch(t, '('))
					t = tok_match(t) ? tok_next(tok_match(t)) : tok_next(t);
				continue;
			}
			if (is_c23_attr(t)) {
				t = tok_match(t) ? tok_next(tok_match(t)) : tok_next(t);
				continue;
			}
			/* Label: ident [[attr]]...: or ident __attribute__((...)): */
			if (t->kind == TK_IDENT || t->kind == TK_KEYWORD) {
				Token *colon = skip_noise(tok_next(t));
				if (colon && match_ch(colon, ':') &&
				    !(tok_next(colon) && match_ch(tok_next(colon), ':'))) {
					t = tok_next(colon);
					continue;
				}
			}
			only_trivial = false;
			break;
		}
		if (!only_trivial) break;
		if (scope_tree[pid].is_stmt_expr) error_tok(defer_tok, ERR_DEFER_LAST_STMT_EXPR);
		sid = pid;
	}
}

// Walk backward from before_idx skipping prep dirs, GNU attrs, C23 [[attrs]].

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
	if (t->tag & TT_RETURN) error_tok(t, "'return' inside defer block bypasses remaining defers");
	if ((t->tag & TT_GOTO) && !is_known_typedef(t))
		error_tok(t, "'goto' inside defer block could bypass remaining defers");
	if ((t->tag & TT_BREAK) && !in_loop && !in_switch)
		error_tok(t, "'break' inside defer block bypasses remaining defers");
	if ((t->tag & TT_CONTINUE) && !in_loop)
		error_tok(t, "'continue' inside defer block bypasses remaining defers");
}

static Token *validate_defer_statement(Token *tok, bool in_loop, bool in_switch, int depth);

static Token *defer_walk_advance_past_orelse(Token *s, bool in_loop, bool in_switch, int depth) {
	Token *act = tok_next(s);
	if (act && match_ch(act, ';')) error_tok(s, "expected statement after 'orelse'");
	validate_defer_control_flow(act, in_loop, in_switch);
	if (act && match_ch(act, '{')) {
		validate_defer_statement(act, in_loop, in_switch, depth + 1);
		Token *close = tok_match(act);
		if (close) return close;
	}
	return act ? act : s;
}

static void defer_scan_orelse_in_group(Token *open, bool in_loop, bool in_switch, int depth) {
	Token *end = tok_match(open);
	if (!end) return;
	Token *prev = open;
	for (Token *s = tok_next(open); s && s != end && s->kind != TK_EOF; s = tok_next(s)) {
		if (s->flags & TF_OPEN) {
			if (match_ch(s, '(') || match_ch(s, '['))
				defer_scan_orelse_in_group(s, in_loop, in_switch, depth);
			prev = tok_match(s);
			s = prev;
			continue;
		}
		if (is_orelse_kw_shadow(s) && orelse_shadow_is_kw(prev)) {
			prev = defer_walk_advance_past_orelse(s, in_loop, in_switch, depth);
			s = prev;
			continue;
		}
		prev = s;
	}
}

static void defer_scan_hidden_stmt_exprs(Token *open, bool in_loop, bool in_switch, int depth) {
	Token *end = tok_match(open);
	if (!end) return;
	for (Token *t = tok_next(open); t && t != end && t->kind != TK_EOF;) {
		if (is_stmt_expr_open(t)) {
			validate_defer_statement(tok_next(t), in_loop, in_switch, depth + 1);
			t = tok_match(t) ? tok_next(tok_match(t)) : tok_next(t);
		} else
			t = tok_next(t);
	}
}

static Token *validate_defer_statement(Token *tok, bool in_loop, bool in_switch, int depth) {
	if (depth >= 4096) error_tok(tok, "braceless control flow nesting depth exceeds 4096");
	tok = skip_noise(tok);
	if (!tok || tok->kind == TK_EOF) return tok;
	if (match_ch(tok, '{')) {
		Token *end = tok_match(tok);
		uint32_t end_idx = end ? tok_idx(end) : UINT32_MAX;
		for (tok = skip_noise(tok_next(tok));
		     tok && tok != end && tok->kind != TK_EOF && tok_idx(tok) < end_idx;
		     tok = skip_noise(tok)) {
			Token *next = validate_defer_statement(tok, in_loop, in_switch, depth);
			if (next == tok) break;
			tok = next;
		}
		return end ? tok_next(end) : tok;
	}

	if ((tok->tag & TT_IF) && tok->ch0 == 'i') {
		Token *after_then = validate_defer_statement(
		    skip_defer_control_head(tok_next(tok), in_loop, in_switch, depth),
		    in_loop,
		    in_switch,
		    depth + 1);
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
				tok = tok_match(tok);
				continue;
			}
			if (match_ch(tok, '?')) {
				td++;
				continue;
			}
			if (match_ch(tok, ':')) {
				if (td > 0) {
					td--;
					continue;
				}
				break;
			}
		}
		return tok && match_ch(tok, ':')
			   ? validate_defer_statement(tok_next(tok), in_loop, in_switch, depth + 1)
			   : tok;
	}

	if (tok->tag & TT_SWITCH)
		return validate_defer_statement(
		    skip_defer_control_head(tok_next(tok), in_loop, true, depth), in_loop, true, depth + 1);
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
		return validate_defer_statement(
		    skip_defer_control_head(tok_next(tok), true, in_switch, depth),
		    true,
		    in_switch,
		    depth + 1);
	}

	if (tok->flags & TF_OPEN) {
		if (is_stmt_expr_open(tok)) {
			Token *inner_brace = tok_next(tok);
			validate_defer_statement(inner_brace, in_loop, in_switch, depth + 1);
			return tok_match(tok) ? tok_next(tok_match(tok)) : tok_next(tok);
		}
	}

	if (is_identifier_like(tok) && tok_next(tok) && match_ch(tok_next(tok), ':'))
		error_tok(tok,
			  "labels inside defer blocks produce duplicate labels "
			  "when the defer body is copied to multiple exit points");
	// is compile-time only.  Scan forward through decl-specifiers.
	for (Token *s = tok; s && s->kind != TK_EOF; s = tok_next(s)) {
		if (s->tag & TT_STORAGE) {
			if (s->ch0 != 'e' || !equal(s, "extern"))
				error_tok(s,
					  "'static' or thread-local storage inside defer block "
					  "creates duplicate state per exit path; hoist the "
					  "declaration outside the defer body");
			continue;
		}
		if (s->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_BITINT | TT_INLINE)) {
			if (s->flags & TF_OPEN) {
				Token *m = tok_match(s);
				if (m) s = m;
			}
			continue;
		}
		break;
	}

	if (tok->kind == TK_KEYWORD) {
		validate_defer_control_flow(tok, in_loop, in_switch);
		if ((tok->tag & TT_DEFER) && !is_known_typedef(tok) && !match_ch(tok_next(tok), ':') &&
		    !(tok_next(tok) && (tok_next(tok)->tag & TT_ASSIGN)))
			error_tok(tok, "nested defer is not supported");
	}

	if (FEAT(F_ORELSE)) {
		Token *prev_oe = NULL;
		for (Token *s = tok;
		     s && s->kind != TK_EOF && !match_ch(s, ';') && !((s->flags & TF_CLOSE) && s->ch0 == '}');
		     s = tok_next(s)) {
			if (s->flags & TF_OPEN) {
				if (match_ch(s, '(') || match_ch(s, '['))
					defer_scan_orelse_in_group(s, in_loop, in_switch, depth);
				prev_oe = tok_match(s);
				s = prev_oe;
				continue;
			}
			if (is_orelse_kw_shadow(s) && (!prev_oe || orelse_shadow_is_kw(prev_oe))) {
				prev_oe = defer_walk_advance_past_orelse(s, in_loop, in_switch, depth);
				s = prev_oe;
				continue;
			}
			prev_oe = s;
		}
	}

	for (Token *s = tok;
	     s && s->kind != TK_EOF && !match_ch(s, ';') && !((s->flags & TF_CLOSE) && s->ch0 == '}');
	     s = tok_next(s)) {
		if (s->flags & TF_OPEN) {
			if (is_stmt_expr_open(s))
				validate_defer_statement(tok_next(s), in_loop, in_switch, depth + 1);
			else if (match_set(s, CH('(') | CH('[')) || match_ch(s, '{'))
				defer_scan_hidden_stmt_exprs(s, in_loop, in_switch, depth);
			s = tok_match(s);
			continue;
		}
	}
	Token *semi = skip_to_semicolon(tok, NULL);
	return (semi && semi->kind != TK_EOF) ? tok_next(semi) : semi;
}

static bool is_knr_params(Token *start, Token *brace) {
	if (!start || start == brace || match_ch(start, ';')) return false;
	bool saw_semi = false;
	for (Token *t = start; t && t != brace && t->kind != TK_EOF; t = tok_next(t)) {
		if (match_ch(t, ';')) saw_semi = true;
		if (t->flags & TF_OPEN) t = tok_match(t);
	}
	return saw_semi;
}

/* --- C23 glibc _Generic-in-declarator recovery (N3322 / GCC 15+) ---
 * After cc -E, `extern void *bsearch(...)` can expand to
 * `extern void *_Generic(..., default: bsearch(...))` which is not a valid
 * declarator. When every association names the same function with the same
 * decl-shaped argument list, fold back to `(name)(params)`. Expression
 * `_Generic` is untouched: callers must only invoke this when the preceding
 * token is a declaration prefix (*, ), type/storage/…, or typedef). */

static bool params_look_like_decls(Token *open) {
	Token *close = tok_match(open);
	if (!close) return false;
	for (Token *t = tok_next(open); t && t != close; t = tok_next(t)) {
		if (t->flags & TF_OPEN) {
			if (tok_match(t)) t = tok_match(t);
			continue;
		}
		if (t->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_TYPEOF | TT_BITINT | TT_ATTR | TT_STORAGE))
			return true;
		if (is_known_typedef(t)) return true;
	}
	return false;
}

static Token *generic_find_assoc_start(Token *open) {
	Token *close = tok_match(open);
	if (!close) return NULL;
	for (Token *t = tok_next(open); t && t != close; t = tok_next(t)) {
		if (t->flags & TF_OPEN) {
			if (tok_match(t)) t = tok_match(t);
			continue;
		}
		if (match_ch(t, ',')) return tok_next(t);
	}
	return NULL;
}

static bool generic_has_distinct_targets(Token *assoc_start, Token *close) {
	const char *first_name = NULL;
	uint32_t first_len = 0;
	Token *first_args_open = NULL;
	Token *first_args_close = NULL;
	int ternary_depth = 0;
	for (Token *t = assoc_start; t && t != close; t = tok_next(t)) {
		if (t->flags & TF_OPEN) {
			if (tok_match(t)) t = tok_match(t);
			continue;
		}
		if (match_ch(t, '?')) {
			ternary_depth++;
			continue;
		}
		if (!match_ch(t, ':')) continue;
		if (ternary_depth > 0) {
			ternary_depth--;
			continue;
		}
		bool found_ident = false;
		int inner_ternary = 0;
		for (Token *b = tok_next(t); b && b != close; b = tok_next(b)) {
			if (b->flags & TF_OPEN) {
				if (tok_match(b)) b = tok_match(b);
				continue;
			}
			if (match_ch(b, ',') && inner_ternary == 0) break;
			if (match_ch(b, '?')) {
				inner_ternary++;
				continue;
			}
			if (match_ch(b, ':') && inner_ternary > 0) {
				inner_ternary--;
				continue;
			}
			if (inner_ternary > 0) continue;
			if (!is_valid_varname(b)) continue;
			{
				Token *bn = tok_next(b);
				if (bn && match_ch(bn, '?')) {
					inner_ternary++;
					b = bn;
					continue;
				}
			}
			found_ident = true;
			while (b && tok_next(b) && tok_next(b) != close && (tok_next(b)->tag & TT_MEMBER) &&
			       tok_next(tok_next(b)) && is_valid_varname(tok_next(tok_next(b)))) {
				b = tok_next(tok_next(b));
			}
			if (!first_name) {
				first_name = tok_loc(b);
				first_len = b->len;
				Token *ao = tok_next(b);
				if (ao && match_ch(ao, '(') && tok_match(ao)) {
					first_args_open = ao;
					first_args_close = tok_match(ao);
				}
			} else if (b->len != first_len ||
				   !prism_memeq_runtime_sized(tok_loc(b), first_name, first_len)) {
				return true;
			} else {
				Token *ao = tok_next(b);
				if (!ao || !match_ch(ao, '(') || !tok_match(ao)) {
					if (first_args_open) return true;
				} else {
					Token *ac = tok_match(ao);
					if (!first_args_open) return true;
					Token *a1 = tok_next(first_args_open);
					Token *a2 = tok_next(ao);
					while (a1 && a1 != first_args_close && a2 && a2 != ac) {
						if (a1->kind != a2->kind || a1->len != a2->len ||
						    !prism_memeq_runtime_sized(tok_loc(a1), tok_loc(a2), a1->len))
							return true;
						a1 = tok_next(a1);
						a2 = tok_next(a2);
					}
					if ((a1 != first_args_close) || (a2 != ac)) return true;
				}
			}
			break;
		}
		if (!found_ident) {
			bool has_real_ident = false;
			int depth = 0;
			for (Token *d = tok_next(t); d && d != close; d = tok_next(d)) {
				if (d->flags & TF_OPEN) depth++;
				else if (d->flags & TF_CLOSE)
					depth--;
				if (depth == 0 && match_ch(d, ',')) break;
				if (is_valid_varname(d) &&
				    !(d->tag & (TT_TYPE | TT_QUALIFIER | TT_SUE | TT_STORAGE | TT_ATTR | TT_TYPEOF |
						TT_BITINT))) {
					has_real_ident = true;
					break;
				}
			}
			if (!has_real_ident) return true;
		}
	}
	return false;
}

static bool generic_rewrite_preamble(Token *generic_tok,
				     Token **open_out,
				     Token **close_out,
				     Token **after_out,
				     Token **assoc_start_out) {
	Token *open = tok_next(generic_tok);
	if (!open || !match_ch(open, '(') || !tok_match(open)) return false;
	Token *close = tok_match(open);
	Token *after = skip_noise(tok_next(close));
	if (!after) return false;
	Token *assoc_start = generic_find_assoc_start(open);
	if (!assoc_start || generic_has_distinct_targets(assoc_start, close)) return false;
	*open_out = open;
	*close_out = close;
	*after_out = after;
	*assoc_start_out = assoc_start;
	return true;
}

static bool generic_decl_rewrite_target(Token *generic_tok,
					Token **name_out,
					Token **params_open_out,
					Token **params_close_out,
					Token **next_out) {
	Token *open, *close, *after, *assoc_start;
	if (!generic_rewrite_preamble(generic_tok, &open, &close, &after, &assoc_start)) return false;
	if (match_set(after, CH(';') | CH(',')) || (after->tag & TT_ATTR) || is_c23_attr(after)) {
		for (Token *t = assoc_start; t && t != close; t = tok_next(t)) {
			Token *name = t;
			Token *call_open = skip_noise(tok_next(t));
			/* Plain `name(params)` association. */
			if (is_valid_varname(t) && call_open && match_ch(call_open, '(') &&
			    tok_match(call_open) && params_look_like_decls(call_open)) {
				*name_out = name;
				*params_open_out = call_open;
				*params_close_out = tok_match(call_open);
				*next_out = after;
				return true;
			}
			/* Glibc-style parenthesized / cast-wrapped name:
			 * `(name)(params)` or `(const char *)(name)(params)`. */
			if (!match_ch(t, '(') || !tok_match(t)) continue;
			Token *inner = skip_noise(tok_next(t));
			Token *paren_close = tok_match(t);
			Token *after_paren = skip_noise(tok_next(paren_close));
			/* Peel one layer of cast-like `(type)(name)` before `(params)`. */
			if (inner && !is_valid_varname(inner) && after_paren && match_ch(after_paren, '(') &&
			    tok_match(after_paren)) {
				Token *maybe_name = skip_noise(tok_next(after_paren));
				Token *name_close = tok_match(after_paren);
				Token *params = name_close ? skip_noise(tok_next(name_close)) : NULL;
				if (maybe_name && is_valid_varname(maybe_name) &&
				    skip_noise(tok_next(maybe_name)) == name_close && params &&
				    match_ch(params, '(') && tok_match(params) &&
				    params_look_like_decls(params)) {
					*name_out = maybe_name;
					*params_open_out = params;
					*params_close_out = tok_match(params);
					*next_out = after;
					return true;
				}
			}
			/* `(name)(params)` */
			if (inner && is_valid_varname(inner) &&
			    skip_noise(tok_next(inner)) == paren_close && after_paren &&
			    match_ch(after_paren, '(') && tok_match(after_paren) &&
			    params_look_like_decls(after_paren)) {
				*name_out = inner;
				*params_open_out = after_paren;
				*params_close_out = tok_match(after_paren);
				*next_out = after;
				return true;
			}
		}
	}
	if (match_ch(after, '(') && tok_match(after) && params_look_like_decls(after)) {
		Token *ext_close = tok_match(after);
		Token *after_ext = skip_noise(tok_next(ext_close));
		if (after_ext &&
		    (match_ch(after_ext, ';') || match_ch(after_ext, ',') || (after_ext->tag & TT_ATTR) ||
		     is_c23_attr(after_ext))) {
			Token *found = NULL;
			for (Token *t = assoc_start; t && t != close; t = tok_next(t)) {
				if (is_valid_varname(t)) found = t;
			}
			if (found) {
				*name_out = found;
				*params_open_out = after;
				*params_close_out = ext_close;
				*next_out = after_ext;
				return true;
			}
		}
	}
	return false;
}

static inline Token *try_detect_noreturn_call(Token *tok) {
	if (!(tok->tag & TT_NORETURN_FN)) return NULL;
	// Respect C scoping: if a local variable/parameter shadows a noreturn
	TypedefEntry *te = typedef_lookup(tok);
	if (te && te->is_shadow) return NULL;
	/* A direct call can still be the operand of the no-paren form of sizeof:
	 *
	 *     sizeof +die();
	 *     sizeof -(int)die();
	 *
	 * In both cases the call is unevaluated even though the token immediately
	 * before the callee is not `sizeof`.  If we mistake it for a statement-level
	 * call, auto-unreachable makes the reachable continuation undefined.  Walk
	 * only cast and unary-prefix syntax; stop at a binary +/-, so
	 * `sizeof(x) + die();` still gets the optimization. */
	Token *ue = tok_walk_back(tok_idx(tok), WB_ATTR_NOISE);
	for (;;) {
		if (ue && match_ch(ue, ')') && close_paren_ends_cast_type_name(ue)) {
			Token *open = tok_match(ue);
			ue = open ? tok_walk_back(tok_idx(open), WB_PAST_NOISE) : NULL;
			continue;
		}
		if (ue && ue->kind == TK_IDENT && equal(ue, "__extension__")) {
			ue = tok_walk_back(tok_idx(ue), WB_PAST_NOISE);
			continue;
		}
		if (ue && ue->kind == TK_PUNCT && ue->len == 1 &&
		    (ue->ch0 == '+' || ue->ch0 == '-' || ue->ch0 == '!' || ue->ch0 == '~')) {
			Token *before = tok_walk_back(tok_idx(ue), WB_ATTR_NOISE);
			if ((ue->ch0 == '+' || ue->ch0 == '-') && before && is_expr_ending(before) &&
			    !is_sizeof_like(before) &&
			    !(before->kind == TK_IDENT && equal(before, "__extension__")) &&
			    !close_paren_ends_cast_type_name(before))
				break; /* binary + / - */
			ue = tok_walk_back(tok_idx(ue), WB_PAST_NOISE);
			continue;
		}
		break;
	}
	if (ue && is_sizeof_like(ue)) return NULL;
	if (tok_idx(tok) >= 1) {
		Token *prev = tok_walk_back(tok_idx(tok), WB_PAST_NOISE);
		if (prev && (prev->tag & TT_MEMBER)) return NULL;
		if (prev && (prev->tag & (TT_TYPE | TT_QUALIFIER | TT_STORAGE | TT_INLINE | TT_SUE)))
			return NULL;
		if (prev && match_ch(prev, '*')) return NULL;
		/* `sizeof die();` / `_Alignof die();` — call is unevaluated;
		 * must not inject unreachable after the statement. */
		if (prev && is_sizeof_like(prev)) return NULL;
	}
	Token *call = tok_next(tok);
	if (!call || !match_ch(call, '(') || !tok_match(call)) return NULL;
	Token *after = tok_next(tok_match(call));
	return (after && match_ch(after, ';')) ? after : NULL;
}

static inline Token *p1d_find_open_paren(Token *tok) {
	for (Token *s = tok_next(tok); s && s->kind != TK_EOF; s = tok_next(s)) {
		if (s->kind == TK_PREP_DIR) continue;
		if (match_ch(s, '(')) return s;
		break;
	}
	return NULL;
}

static Token *p1_knr_find_close_paren(Token *semi_tok) {
	for (uint32_t pi = tok_idx(semi_tok); pi > 0; pi--) {
		Token *pt = &token_pool[pi - 1];
		if (pt->kind == TK_PREP_DIR) continue;
		if (match_ch(pt, '{') || match_ch(pt, '}')) return NULL;
		if (match_ch(pt, ')') && tok_match(pt)) {
			Token *open = tok_match(pt);
			bool is_ident_list = true;
			for (Token *t = tok_next(open); t && t != pt; t = tok_next(t)) {
				if (!is_valid_varname(t) && !match_ch(t, ',') && t->kind != TK_PREP_DIR) {
					is_ident_list = false;
					break;
				}
			}
			if (is_ident_list) return pt;
			pi = tok_idx(open) + 1; // +1 because loop does pi--
			continue;
		}
		if ((pt->flags & TF_CLOSE) && tok_match(pt)) {
			pi = tok_idx(tok_match(pt)) + 1;
			continue;
		}
	}
	return NULL;
}

// Probes past attributes/pragmas via skip_noise before checking TF_RAW,
// matching Pass 2's process_declarators logic.
// Returns the token after a stripable `raw` prefix, or NULL if not a decl-strip.
static inline Token *raw_decl_strip_after(Token *probe) {
	if (!probe || !(probe->flags & TF_RAW)) return NULL;
	Token *after = skip_noise(tok_next(probe));
	if (!after) return NULL;
	/* Typedef-named `raw` is a keyword prefix only before another type
	 * (`raw raw x` / `raw int x`), never before `*` / `(`` / a declarator
	 * name where `raw` itself is the typedef type. */
	if (is_known_typedef(probe)) {
		if (is_type_keyword(after) || is_known_typedef(after) ||
		    (after->tag & (TT_QUALIFIER | TT_SUE | TT_STORAGE | TT_INLINE | TT_TYPEDEF)) ||
		    (after->flags & TF_RAW))
			return after;
		return NULL;
	}
	if ((is_valid_varname(after) && !is_type_keyword(after) && !is_known_typedef(after) &&
	     !(after->tag & (TT_QUALIFIER | TT_SUE))) ||
	    match_ch(after, '*') || match_ch(after, '('))
		return after;
	return NULL;
}

static inline Token *p1_skip_decl_raw(Token *t, bool *saw_raw) {
	Token *after = raw_decl_strip_after(skip_noise(t));
	if (!after) return t;
	while ((after->flags & TF_RAW) && !is_known_typedef(after)) after = skip_noise(tok_next(after));
	*saw_raw = true;
	return after;
}

static inline bool is_assignment_operator_token(Token *tok) {
	return (tok->tag & TT_ASSIGN) && tok_loc(tok)[tok->len - 1] == '=';
}

static bool raw_after_subscript_open_bracket(Token *raw_kw) {
	if (!raw_kw || !(raw_kw->flags & TF_RAW)) return false;
	uint32_t ri = tok_idx(raw_kw);
	if (ri == 0) return false;
	/* tok_walk_back(k, WB_PAST_NOISE) inspects pool[k-1] first — pass ri, not ri-1. */
	Token *b = tok_walk_back(ri, WB_PAST_NOISE);
	// `[` from `[[attr]]` is tagged TF_C23_ATTR — not an array subscript.
	return b && match_ch(b, '[') && !(b->flags & TF_C23_ATTR);
}

static bool is_raw_declaration_context(Token *raw_kw, Token *after_raw) {
	if (raw_after_subscript_open_bracket(raw_kw)) return false;
	after_raw = skip_noise(after_raw);
	if (!after_raw) return false;
	if (is_type_keyword(after_raw) || is_known_typedef(after_raw) ||
	    (after_raw->tag & (TT_QUALIFIER | TT_SUE | TT_STORAGE | TT_INLINE | TT_TYPEDEF)) ||
	    ((after_raw->flags & TF_RAW) && !is_known_typedef(after_raw)))
		return true;
	if (match_ch(after_raw, '*')) {
		Token *after_star = skip_noise(tok_next(after_raw));
		while (after_star && (match_ch(after_star, '*') || (after_star->tag & TT_QUALIFIER)))
			after_star = skip_noise(tok_next(after_star));
		return after_star && (is_valid_varname(after_star) || match_ch(after_star, '('));
	}
	return false;
}

static bool is_raw_strip_context(Token *raw_kw, Token *after_raw) {
	if (is_raw_declaration_context(raw_kw, after_raw)) return true;
	after_raw = skip_noise(after_raw);
	Token *boundary = after_raw ? skip_noise(tok_next(after_raw)) : NULL;
	return after_raw && is_valid_varname(after_raw) && !is_type_keyword(after_raw) &&
	       !is_known_typedef(after_raw) && !(after_raw->tag & (TT_QUALIFIER | TT_SUE)) && boundary &&
	       (match_ch(boundary, ',') || match_ch(boundary, ';') ||
		match_set(boundary, CH('[') | CH('(') | CH('=') | CH(':')));
}

static bool has_effective_const_qual(Token *type_start, TypeSpecResult *type, DeclResult *decl) {
	bool has_const_qual = (type->has_const && !decl->is_func_ptr && !decl->is_pointer) || decl->is_const;
	if (type->has_constexpr) has_const_qual = true;
	if (type->has_typeof && !decl->is_func_ptr && !decl->is_pointer) has_const_qual = true;
	if (!has_const_qual && !decl->is_func_ptr && !decl->is_pointer) {
		for (Token *t = type_start; t && t != type->end; t = tok_next(t))
			if (is_const_typedef(t)) {
				has_const_qual = true;
				break;
			}
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
	if (tok->flags & TF_HAS_SPACE) return true;
	if ((is_identifier_like(prev) || prev->kind == TK_NUM) &&
	    (is_identifier_like(tok) || tok->kind == TK_NUM))
		return true;
	if (prev->kind != TK_PUNCT || tok->kind != TK_PUNCT) return false;
	char a = (prev->len == 1) ? prev->ch0 : tok_loc(prev)[prev->len - 1];
	char b = tok->ch0;
	if (b == '=') return strchr("=!<>+-*/%&|^", a) != NULL;
	return (a == b && strchr("+-<>&|#", a)) || (a == '-' && b == '>') || (a == '/' && b == '*') ||
	       (a == '*' && b == '/');
}

static bool declarator_has_bracket_orelse(Token *start, Token *end) {
	for (Token *t = start; t && t != end && t->kind != TK_EOF; t = tok_next(t))
		if (tok_ann(t) & P1_OE_BRACKET) return true;
	return false;
}

static inline uint16_t c_scope_id(Token *body_start) {
	return body_start && match_ch(body_start, '{') ? (uint16_t)body_start->parse_data : 0;
}

/* Freeze position-dependent C name resolution into each token for O(1)
 * consumers. Optionally report whether an exact identifier exists while the
 * pool is hot, avoiding a second sentinel walk in the caller. */
static bool c_parse_finalize(const char *find_ident, uint32_t find_len) {
	td_build_timelines();
	ba_build_timelines();
	bool found = false;
	for (uint32_t i = 1; i < token_count; i++) {
		Token *t = &token_pool[i];
		if (find_ident && !found && t->kind == TK_IDENT && t->len == find_len &&
		    t->ch0 == (uint8_t)find_ident[0] &&
		    prism_memeq_runtime_sized(tok_loc(t), find_ident, find_len))
			found = true;
		if (t->tag & TT_ATTR) {
			Token *open = tok_next(t);
			if (open && match_ch(open, '(') && (open->flags & TF_OPEN) && tok_match(open)) {
				uint32_t close = tok_idx(tok_match(open));
				for (uint32_t j = tok_idx(open); j <= close; j++)
					token_pool[j].ann |= P1_IN_ATTR_ARGS;
			}
		}
		if (!is_identifier_like(t)) continue;
		TypedefEntry *e = typedef_lookup(t);
		if (!e) continue;
		tok_ann(t) |= P1_HAS_ENTRY;
		t->parse_data = (uint32_t)(e - typedef_table.entries) + 1;
		if (!e->is_enum_const && !e->is_shadow && !e->is_vla_var && !e->is_struct_tag)
			tok_ann(t) |= P1_IS_TYPEDEF;
	}
	p1_typedef_annotated = true;
	return found;
}

void tokenizer_teardown(bool full) {
	if (ctx->input_files) {
		for (int i = 0; i < ctx->input_file_count; i++) {
			File *f = ctx->input_files[i];
			if (f && f->contents && f->owns_contents) free(f->contents);
		}
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
		for (ArenaBlock *b = ctx->main_arena.head; b; b = b->next) b->used = 0;
		ctx->main_arena.current = ctx->main_arena.head;
		token_count = 1; // Reset pool index but keep allocation
	}
	ctx->input_files = NULL;
	ctx->input_file_count = 0;
	ctx->input_file_capacity = 0;
	ctx->current_file = NULL;
}

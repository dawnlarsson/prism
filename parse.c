
/*
 * Prism Parse (pparse), reusable C parse library.
   Parsing full C89/C11/C17/C23 sources and supports the Prism dialect extensions (defer, orelse, raw, etc.).
 */

#if defined(PRISM_SINGLE_THREAD)
#define PRISM_THREAD_LOCAL
#elif defined(_MSC_VER)
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
#define pparse_PRISM_LIKELY(x) __builtin_expect(!!(x), 1)
#define pparse_PRISM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define PRISM_COLD
#define PRISM_HOT
#define PRISM_PURE
#define PRISM_CONST_FN
#define PRISM_ALWAYS_INLINE
#define PRISM_FLATTEN
#define PRISM_MAYBE_UNUSED
#define pparse_PRISM_LIKELY(x) (x)
#define pparse_PRISM_UNLIKELY(x) (x)
#endif

#define PPARSE_TOMBSTONE ((char *)1)

/* These four spellings document the only distinction callers need: a runtime
 * length may be zero and therefore avoids calling memcmp with sentinel/null
 * pointers. Compilers already recognize the libc operations as builtins. */
#define prism_memeq_static(a, b, n) (memcmp((a), (b), (n)) == 0)
#define prism_memeq_runtime(a, b, n) (memcmp((a), (b), (n)) == 0)
#define prism_memeq_runtime_sized(a, b, n) ((n) == 0 || memcmp((a), (b), (n)) == 0)
#define prism_memcpy_runtime_sized(dst, src, n) memcpy((dst), (src), (n))

#define PPARSE_IS_DIGIT(c) ((unsigned)(c) - '0' < 10u)
#define PPARSE_IS_ALPHA(c) (((unsigned)((c) | 0x20) - 'a') < 26u || (c) == '_' || (c) == '$')
#define PPARSE_IS_ALNUM(c) (PPARSE_IS_DIGIT(c) || PPARSE_IS_ALPHA(c))
#define PPARSE_IS_XDIGIT(c) (PPARSE_IS_DIGIT(c) || ((unsigned)((c) | 0x20) - 'a') < 6u)
#define PPARSE_ARENA_DEFAULT_BLOCK_SIZE (64 * 1024)
#define PPARSE_KW_MARKER 0x80000000ULL // Internal marker bit for keyword map: values are (tag | PPARSE_KW_MARKER)
#define PPARSE_KW_FLAGS_SHIFT 32	// Extra token flags encoded in bits 32-47 of keyword value
#define PPARSE_KW_LEN_SHIFT 48	// Identifier length encoded in bits 48-55
/* Linear-probe bound shared by keyword-map insertion and lookup; they must
 * agree or a displaced keyword becomes unfindable. See pparse_init_keyword_map. */
#define PPARSE_KW_MAX_PROBE 32
#define PPARSE_KW_SHADOW_SHIFT 56 // Dialect macro-shadow bit encoded in bits 56-58

static inline bool pparse_is_hspace(char c) {
	return (c == ' ') | (c == '\t');
}

enum {
	PPARSE_KWSHADOW_DEFER = 1u << 0,
	PPARSE_KWSHADOW_ORELSE = 1u << 1,
	PPARSE_KWSHADOW_RAW = 1u << 2,
};

// Centralized diagnostic strings. Many appear at multiple Pass 2 emit /
static const char PPARSE_ERR_ORELSE_STMT_LEVEL[] = "'orelse' cannot be used here (it must appear at the "
					    "statement level in a declaration or bare expression)";
/* Canonical stray-defer diagnostic for every expression context. */
static const char PPARSE_ERR_DEFER_EXPR_CTX[] =
    "'defer' cannot be used in expression context (array dimensions, parenthesized "
    "expressions, function arguments, sizeof/_Static_assert operands, etc.); move it to "
    "statement position";
static const char PPARSE_ERR_BARE_ORELSE_SPANS_PP[] = "bare orelse assignment cannot be used when the "
					       "expression spans preprocessor conditionals: the "
					       "transpiler would emit tokens from all branches, "
					       "producing invalid C; use a temporary variable or "
					       "move the #ifdef outside the expression";
static const char PPARSE_ERR_REGISTER_ATOMIC_AGGREGATE[] = "'register _Atomic' aggregate cannot be safely "
						    "zero-initialized; remove 'register' or use 'raw' "
						    "to opt out of automatic initialization";
static const char PPARSE_ERR_REGISTER_UNION[] = "'register' union cannot be safely "
					 "zero-initialized (address-taking is illegal for "
					 "register, and = {0} only zeros the first member); "
					 "remove 'register' or use 'raw' to opt out of "
					 "automatic initialization";
static const char PPARSE_ERR_REGISTER_VLA[] = "'register' VLA cannot be safely zero-initialized "
				       "(address-taking is illegal for register, and VLAs "
				       "cannot use initializer syntax); remove 'register' "
				       "or use 'raw' to opt out of automatic initialization";
static const char PPARSE_ERR_REGISTER_EMPTY_AGG[] =
    "'register' empty or zero-size aggregate cannot be safely "
    "zero-initialized (address-taking is illegal for register, and "
    "`= {0}` is rejected by the backend); remove 'register' or use "
    "'raw' to opt out of automatic initialization";
static const char PPARSE_ERR_INIT_STMT_VLA[] = "VLA in for/if/switch init-statement cannot be "
					"safely zero-initialized; move the declaration "
					"before the statement";
static const char PPARSE_ERR_MULTIDECL_VM[] = "multi-declarator with variably-modified "
				       "type specifier requires declaration split which "
				       "would double-evaluate VLA size expressions; "
				       "declare each variable on a separate line";
static const char PPARSE_ERR_ORELSE_STATIC_THREAD[] = "'orelse' cannot be used in the initializer of a "
					       "variable with static or thread storage duration "
					       "(the runtime fallback check would re-execute on "
					       "every function entry, destroying persistence)";
static const char PPARSE_ERR_ORELSE_CONSTEXPR[] = "'orelse' cannot be used with 'constexpr' "
					   "(constexpr requires a compile-time constant "
					   "initializer; orelse produces runtime fallback code)";
static const char PPARSE_ERR_ORELSE_CONST_VM[] = "orelse on a const-qualified variably-modified type "
					  "would duplicate the type specifier, causing VLA "
					  "size expressions to be evaluated twice; hoist the "
					  "value to a non-const variable first";
static const char PPARSE_ERR_BRACKET_OE_VLA_INIT_STMT[] = "bracket orelse in VLA dimensions cannot be used in "
						   "control statement conditions (hoisted temps would "
						   "inject invalid syntax); move the declaration before "
						   "the statement";
static const char PPARSE_ERR_ORELSE_FILE_SCOPE[] = "'orelse' cannot be used in file-scope initializers "
					    "(requires runtime fallback code)";
static const char PPARSE_ERR_ORELSE_STRUCT_VALUE[] = "orelse on struct/union values is not supported "
					      "(memcmp cannot reliably detect zero due to padding)";
static const char PPARSE_ERR_BRACKET_OE_ANON_AGG[] = "bracket orelse / zero-init requiring declaration split "
					      "cannot be used with anonymous struct/union; "
					      "add a tag name or use a typedef";
static const char PPARSE_ERR_ORELSE_ARRAY_NEVER_NULL[] = "orelse on array variable '%.*s' will never trigger "
						  "(array address is never NULL); remove the orelse clause";
static const char PPARSE_ERR_CONST_UNAVOIDABLE_MEMSET[] = "'const' variable requiring unavoidable post-declaration memset "
						   "cannot be safely "
						   "zero-initialized: modifying a const object is "
						   "undefined behavior. Remove 'const', provide an "
						   "explicit initializer, or use 'raw' to opt out.";
static const char PPARSE_ERR_DEFER_LAST_STMT_EXPR[] = "defer inside a block that is the last "
					       "statement of a statement expression "
					       "would corrupt the expression's return "
					       "value; ensure the last statement of the "
					       "statement expression is outside the "
					       "defer block";
static const char PPARSE_ERR_DEFER_SHADOW_SAME_SCOPE[] = "variable '%.*s' shadows a name captured "
						  "by a defer in the same scope; the defer "
						  "body would bind to the shadowing variable";
static const char PPARSE_ERR_DEFER_CTRL_PAREN[] = "defer cannot appear inside control statement parentheses";
static const char PPARSE_ERR_DEFER_BRACELESS_CTRL[] =
    "defer requires braces in control statements (braceless has no scope)";
static const char PPARSE_ERR_DEFER_STMT_EXPR_TOP[] =
    "defer cannot be at top level of statement expression; wrap in a block";
static const char PPARSE_ERR_DEFER_SWITCH_BRACE[] = "defer in switch case requires braces";
static const char PPARSE_ERR_DEFER_UNTERMINATED[] = "unterminated defer statement; expected ';'";
static const char PPARSE_ERR_DEFER_MISSING_SEMI[] =
    "defer statement appears to be missing ';' (found '%.*s' keyword inside)";
static const char PPARSE_ERR_ORELSE_TERNARY[] = "'orelse' cannot be used inside a ternary expression";
static const char PPARSE_ERR_BOUNDS_COMM_IDX_ARR[] = "commutative subscript 'idx[arr]' bypasses "
					      "-fbounds-check; rewrite as 'arr[idx]'";
static const char PPARSE_ERR_BOUNDS_DERIVED_SUB[] = "-fbounds-check: derived-pointer subscript "
					     "'(&arr[..])[i]' bypasses bounds-check; "
					     "rewrite as 'arr[idx]'";
static const char PPARSE_ERR_BOUNDS_COMM_DERIVED[] = "-fbounds-check: commutative derived-pointer "
					      "subscript 'idx[(&arr[..])]' bypasses "
					      "bounds-check; rewrite as 'arr[idx]'";
static const char PPARSE_ERR_BOUNDS_COMMA_OP[] = "-fbounds-check: comma operand subscript idx[(...,arr)] must be "
					  "rewritten as arr[idx]";
static const char PPARSE_ERR_BOUNDS_COMM_SCAN[] =
    "bounds-check (commutative): array name in index position cannot be verified "
    "(rewrite as array[index], or disable -fbounds-check for this expression)";

/* Folded from duplicate spellings at multiple sites (release prep). */
static const char PPARSE_ERR_BOUNDS_PTR_ARITH_DEREF[] =
    "-fbounds-check: pointer-arithmetic dereference with tracked array base "
    "cannot be verified (rewrite as array[index])";
static const char PPARSE_ERR_BOUNDS_PTR_ARITH_SUB[] =
    "-fbounds-check: pointer-arithmetic subscript with tracked array base "
    "cannot be verified (rewrite as array[index])";
static const char PPARSE_ERR_ORELSE_EXPECT_EXPR[] =
    "expected expression before 'orelse'";
static const char PPARSE_ERR_ORELSE_EXPECT_STMT[] =
    "expected statement after 'orelse'";
static const char PPARSE_ERR_ORELSE_LHS_TWICE[] =
    "in the LHS (would be evaluated twice); hoist the expression to a "
    "variable first";
static const char PPARSE_ERR_CTRL_IN_TYPE_SPEC[] =
    "control flow keywords inside type specifiers (typeof() / _Atomic()) may "
    "corrupt control-flow tracking";
static const char PPARSE_ERR_CTRL_IN_TYPE_SPEC_HARD[] =
    "control flow keywords are not allowed inside type specifiers (typeof() / "
    "_Atomic()); transpiler rewrites may duplicate the type specifier, which "
    "would corrupt control-flow tracking";
static const char PPARSE_ERR_ORELSE_IN_PARENS[] =
    "'orelse' cannot be used inside parentheses "
    "(it must appear at the top level of a declaration)";
static const char PPARSE_ERR_ORELSE_PROTO_DIM[] =
    "'orelse' in array dimensions of a function "
    "prototype is not allowed (prototype parameter "
    "arrays are never allocated; the dimension is "
    "not evaluated at runtime)";
static const char PPARSE_ERR_ORELSE_DEFN_DIM[] =
    "'orelse' in array dimensions of a function "
    "definition parameter is not allowed (the "
    "ternary expansion would evaluate the "
    "dimension twice: undefined behavior for "
    "volatile expressions)";

#if defined(_MSC_VER)
#define PPARSE_ARENA_ALIGN 8
#else
#define PPARSE_ARENA_ALIGN (__alignof__(long double))
#endif

/* Every call site passes a string literal of at least two bytes. sizeof keeps
 * the length compile-time and lets the optimizer fold short memcmp calls. */
#define pparse_equal(tok, s) pparse_equal_n(_pc, tok, s, (uint32_t)(sizeof(s) - 1))

#define pparse_KEYWORD_HASH(key, len)                                                                               \
	((len) == 0 ? 0                                                                                      \
		    : (((unsigned)(len) * 2 + (unsigned char)(key)[0] * 99 +                                 \
			(unsigned char)((len) > 1 ? (key)[1] : (key)[0]) * 125 +                             \
			(unsigned char)((len) > 6 ? (key)[6] : (key)[(len) - 1]) * 69) &                     \
		       255))

/* Shared capacity growth: double until >= need (or init_cap when empty). */
static inline size_t pparse_vec_grow_cap(size_t cap, size_t need, size_t init_cap) {
	size_t new_cap = cap == 0 ? (init_cap > 0 ? init_cap : 1) : cap * 2;
	while (new_cap < need) new_cap *= 2;
	return new_cap;
}

static const uint8_t pparse_ident_char[256] = {
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

typedef struct PParseToken PParseToken;
typedef struct PParseArenaBlock PParseArenaBlock;

/* Forward decls — call sites in pparse_tokenize/Pass0 precede the definitions. */
enum {
	PPARSE_WB_SKIP_PREP = 1 << 0,
	PPARSE_WB_SKIP_ATTR = 1 << 1,
	PPARSE_WB_JUMP_GROUPS = 1 << 2,
	PPARSE_WB_JUMP_C23_ATTR = 1 << 3,
	PPARSE_WB_JUMP_ALL_PARENS = 1 << 4,
	PPARSE_WB_JUMP_ATTR_PARENS = 1 << 5,
	PPARSE_WB_FROM_PRED = 1 << 6,
};
#define PPARSE_WB_PAST_NOISE (PPARSE_WB_FROM_PRED | PPARSE_WB_SKIP_PREP | PPARSE_WB_SKIP_ATTR | PPARSE_WB_JUMP_GROUPS)
#define PPARSE_WB_ATTR_NOISE (PPARSE_WB_FROM_PRED | PPARSE_WB_SKIP_PREP | PPARSE_WB_SKIP_ATTR | PPARSE_WB_JUMP_C23_ATTR)
#define PPARSE_WB_SKIP_NOISE (PPARSE_WB_SKIP_PREP | PPARSE_WB_SKIP_ATTR | PPARSE_WB_JUMP_C23_ATTR | PPARSE_WB_JUMP_ATTR_PARENS)
#define PPARSE_WB_SKIP_ATTRS (PPARSE_WB_SKIP_PREP | PPARSE_WB_SKIP_ATTR | PPARSE_WB_JUMP_C23_ATTR | PPARSE_WB_JUMP_ALL_PARENS)
static PParseToken *pparse_walk_back(uint32_t start_idx, unsigned flags);
static bool pparse_is_raw_declaration_context(PParseToken *raw_kw, PParseToken *after_raw);

typedef struct {
	char *name;
	int line_delta;
	uint32_t file_no : 14; // Matches PParseToken.file_idx.
	uint32_t is_system : 1;
	uint32_t is_direct_system_include : 1; // Entered from non-system (re-emit as #include)
	uint32_t skip_emit : 1; // Precomputed is_system && is_include_entry hot-path predicate
} PParseFile;

typedef enum {
	PPARSE_TK_IDENT,
	PPARSE_TK_KEYWORD,
	PPARSE_TK_PUNCT,
	PPARSE_TK_STR,
	PPARSE_TK_NUM,
	PPARSE_TK_PREP_DIR, // Preprocessor directive (e.g., #pragma) to preserve
	PPARSE_TK_EOF,
} PParseTokenKind;

enum {
	PPARSE_TF_AT_BOL = 1 << 0,
	PPARSE_TF_HAS_SPACE = 1 << 1,
	PPARSE_TF_ALIGNOF = 1 << 2,
	PPARSE_TF_OPEN = 1 << 3,	   // Opening delimiter: ( [ {
	PPARSE_TF_CLOSE = 1 << 4,	   // Closing delimiter: ) ] }
	PPARSE_TF_C23_ATTR = 1 << 5,	   // First '[' of C23 [[ ... ]] attribute
	PPARSE_TF_RAW = 1 << 6,	   // 'raw' keyword
	PPARSE_TF_SIZEOF = 1 << 7,	   // unevaluated sizeof/alignof/offsetof family
	PPARSE_TF_SOFT_KW = 1 << 8,	   // soft keyword usable as identifier (alignas, bool, …)
	PPARSE_TF_STATIC_ASSERT = 1 << 9, // _Static_assert / static_assert
	PPARSE_TF_MS_CC = 1 << 10,	   // MSVC calling-convention keyword (__cdecl, …)
	PPARSE_TF_SYS_SKIP = 1 << 11,	   // token belongs to a system #include entry file; in
				   // non-flatten emit it is skipped verbatim. Precomputed at
				   // pparse_tokenize from current_file so the hot emit loop tests one
				   // flag bit instead of a per-token pparse_tok_cold + file lookup.
	PPARSE_TF_HAS_PRISM = 1 << 12,	   // matched group contains a defer/orelse token
	PPARSE_TF_LINK_JUMP = 1 << 13,	   // parse_data is a non-adjacent next-token index
	PPARSE_TF_OFFSETOF = 1 << 14,
	PPARSE_TF_STMT_EXPR = 1 << 15,
};

#define P1_IN_ATTR_ARGS (1u << 12) // token is inside a GNU/MS attribute group

enum {
	PPARSE_TT_TYPE = 1 << 0, // Type keyword (int, char, void, struct, etc.)
	PPARSE_TT_QUALIFIER = 1 << 1,
	PPARSE_TT_SUE = 1 << 2, // struct/union/enum
	PPARSE_TT_SKIP_DECL = 1 << 3,
	PPARSE_TT_ATTR = 1 << 4,	  // Attribute keyword (__attribute__, __attribute, __declspec)
	PPARSE_TT_ASSIGN = 1 << 5,	  // Assignment or compound assignment operator (=, +=, ++, --, [)
	PPARSE_TT_MEMBER = 1 << 6,	  // Member access operator (. or ->)
	PPARSE_TT_LOOP = 1 << 7,	  // Loop keyword (for, while, do)
	PPARSE_TT_STORAGE = 1 << 8,	  // Storage class: extern, static, _Thread_local, thread_local, __thread
	PPARSE_TT_ASM = 1 << 9,	  // Inline assembly (asm, __asm__, __asm)
	PPARSE_TT_INLINE = 1 << 10,	  // inline, __inline, __inline__
	PPARSE_TT_NORETURN_FN = 1 << 11, // Noreturn function identifier (exit, abort, etc.)
	PPARSE_TT_SPECIAL_FN = 1 << 12,
	PPARSE_TT_CONST = 1 << 13, // const keyword

	PPARSE_TT_RETURN = 1 << 14,   // return
	PPARSE_TT_BREAK = 1 << 15,    // break
	PPARSE_TT_CONTINUE = 1 << 16, // continue
	PPARSE_TT_GOTO = 1 << 17,     // goto
	PPARSE_TT_CASE = 1 << 18,     // case
	PPARSE_TT_DEFAULT = 1 << 19,  // default
	PPARSE_TT_DEFER = 1 << 20,    // defer
	PPARSE_TT_GENERIC = 1 << 21,  // _Generic
	PPARSE_TT_SWITCH = 1 << 22,   // switch
	PPARSE_TT_IF = 1 << 23,       // if, else
	PPARSE_TT_TYPEDEF = 1 << 24,  // typedef

	PPARSE_TT_VOLATILE = 1 << 25, // volatile
	PPARSE_TT_REGISTER = 1 << 26, // register
	PPARSE_TT_TYPEOF =
	    1 << 27, // typeof, typeof_unqual, __typeof__, __typeof, __typeof_unqual__, __typeof_unqual
	PPARSE_TT_BITINT = 1 << 28,	  // _BitInt
	PPARSE_TT_ALIGNAS = 1 << 29,	  // _Alignas, alignas
	PPARSE_TT_ORELSE = 1 << 30,	  // orelse
};
#define PPARSE_TT_STRUCTURAL (1u << 31) // { } ; : — force slow-path dispatch

/* Two tokens per cache line and shift-only index arithmetic. Delimiter pairing
 * and identifier binding share a word because their token kinds are disjoint;
 * that space absorbs line/file metadata and eliminates the cold side array. */
struct PParseToken {
	uint32_t tag;	    // TT_* bitmask - token classification
	uint32_t parse_data; // Token-specific parser payload, or PPARSE_TF_LINK_JUMP target
	uint32_t match_idx; // Source offset
	uint32_t len;	    // PParseToken length in bytes (must handle >65535 for large literals)
	union {
		uint32_t pair_idx; // Delimiter token: paired index (0 = none)
		uint32_t td_entry; // Identifier token: binding scratch
	};
	int32_t line_no : 18;
	uint32_t file_idx : 14;
	uint8_t kind;
	uint8_t ch0;	    // First source byte — avoids pparse_loc(_pc) indirection in hot paths
	uint16_t flags;	    // TF_* bitmask (PPARSE_TF_SOFT_KW needs bit 8)
	/* Pass 1 annotations and emitter recipes. */
	uint32_t ann;
}; // 32 bytes

typedef char prism_assert_token_32[(sizeof(struct PParseToken) == 32) ? 1 : -1];

typedef struct {
	char *key;
	void *val;
	uint32_t hash;
	uint16_t key_len;
} PParseHashEntry;

typedef struct {
	PParseHashEntry *buckets;
	int capacity;
	int used;
} PParseHashMap;

typedef struct {
	int prev_index;		  // Index of previous entry with same name (-1 if none)
	uint32_t token_index;	  // PParseToken pool index of the declaration
	uint32_t scope_close_idx; // PParseToken index of matching '}' (UINT32_MAX for file scope)
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
	bool is_long_double : 1;      // scalar long double (including _Complex)
	bool is_typeof : 1;           // typedef originated from typeof()/__typeof__
	bool is_constexpr : 1;	     // C23 'constexpr' shadow: usable as an array-dimension ICE
	bool is_struct_tag : 1;	     // struct/union tag (not a typedef name)
	bool array_dim_complete : 1; // array typedef: sizeof(T)/sizeof(T[0]) valid at uses
	uint8_t array_rank;	     // # of array dimensions (0 if not array);
} PParseTypedefEntry; // 16 bytes — four entries per 64-byte cache line

typedef char prism_assert_typedef_entry_16[(sizeof(PParseTypedefEntry) == 16) ? 1 : -1];
typedef struct PParseTimelineItem {
	int entry_idx, prev_cover;
} PParseTimelineItem;
typedef struct {
	PParseTimelineItem *timeline;
	uint64_t *tl_descs;
	int tl_cap, tl_desc_cap;
	int max_chain_seen;    /* capped sample; >=8 triggers timeline build */
	int tl_count_at_build; /* table.count when timelines were last built */
} PParseTimelineState;
typedef struct {
	PParseTypedefEntry *entries;
	int count;
	int capacity;
	PParseHashMap name_map; // Maps name → (entry_index + 1) as void*, 0 = absent. Chain via prev_index.
	uint64_t bloom;	  // Bloom filter: bit (ch0 ^ len) & 63. Fast negative lookup.
	PParseTimelineState tl;
} PParseTypedefTable;
/* Long same-name chains use sorted timelines with prev_cover skip links;
 * short chains walk. Bounds locals stay separate from typedef lookups. */
typedef struct {
	int prev_index;
	uint32_t token_index;
	uint32_t scope_close_idx;
	uint8_t array_rank;
	bool array_dim_complete : 1;
	bool is_vla_var : 1;
	bool blocks_outer : 1;
} PParseBoundsArrayEntry;
typedef struct {
	PParseBoundsArrayEntry *entries;
	int count;
	int capacity;
	PParseHashMap name_map;
	uint64_t bloom;
} PParseBoundsArrayTable;
/* One TLS lookup for the complete bounds registry. */
typedef struct {
	PParseBoundsArrayTable table;
	PParseTimelineState tl;
} PParseBoundsState;

typedef struct {
	char *name;
	uint64_t value;
} PParseKeywordEntry;

enum // Feature flags
{
	PPARSE_F_DEFER = 1,
	PPARSE_F_ZEROINIT = 2,
	PPARSE_F_LINE_DIR = 4,
	PPARSE_F_WARN_SAFETY = 8,
	PPARSE_F_FLATTEN = 16,
	PPARSE_F_ORELSE = 32,
	PPARSE_F_AUTO_UNREACHABLE = 64,
	PPARSE_F_AUTO_STATIC = 128,
	PPARSE_F_BOUNDS_CHECK = 256,
	/* Suppress Prism's own warnings. Generators that deliberately emit
	 * thousands of the same diagnostic set this; tests that assert on a
	 * warning leave it clear. */
	PPARSE_F_QUIET = 512
};

struct PParseArenaBlock {
	PParseArenaBlock *next;
	size_t used;
	size_t capacity;
#if defined(_MSC_VER)
	__declspec(align(PPARSE_ARENA_ALIGN)) char data[];
#else
	_Alignas(PPARSE_ARENA_ALIGN) char data[];
#endif
};

typedef struct {
	PParseArenaBlock *head;
	PParseArenaBlock *current;
} PParseArena;

typedef struct {
	bool at_bol;
	bool has_space;
	int line_no;
} PParseTokState;

typedef struct {
	int if_depth, trail_n, snap_start;
} PParseSosDoFrame;

typedef struct PParseContext {
	PParseArena main_arena;
	PParseFile *current_file;
	PParseFile **input_files;
	int input_file_count;
	int input_file_capacity;

#ifdef PRISM_LIB_MODE
	jmp_buf error_jmp;
	bool error_jmp_set;
	char error_msg[1024];
	int error_line;
#endif
	uint32_t features; // PPARSE_F_DEFER | PPARSE_F_ZEROINIT | PPARSE_F_LINE_DIR | PPARSE_F_WARN_SAFETY | PPARSE_F_FLATTEN | PPARSE_F_ORELSE

	PParseToken *tp_pool;	    // Hot: tag, parse_data, match_idx, len, kind, flags
	char *token_source; // Shared backing buffer for match_idx source offsets
	bool lex_only;	    // Tokenize for spellings only: do not require balanced delimiters
	uint32_t tp_count;  // Next free index. 0 reserved as NULL sentinel.
	uint32_t tp_cap;
	uint32_t pparse_token_tag_summary; // OR of PPARSE_TT_* tags in the current token stream
	PParseKeywordEntry kw_cache[256];

	void *p1_scope_tree; // PParseScopeInfo[] — flat array indexed by scope_id
	uint16_t p1_scope_count;
	void *p1_func_meta; // FuncMeta[] — one per function definition
	int p1_func_meta_count;
	int p1_func_meta_cap;
	void *p1_func_entries; // P1FuncEntry[] — flat combined array
	int p1_func_entry_count;
	int p1_func_entry_cap;

	/* Immutable position-keyed analysis: a 4-byte token index points into a
	 * sparse tagged record chain, keeping the hot 32-byte token unchanged. */
	uint32_t *analysis_index;
	void *analysis_records; // PParseAnalysisRecord[]
	uint32_t analysis_count, analysis_cap;

	/* Per-thread parser state lives inside the context rather than in separate
	 * _Thread_local slots: it is reached through the threaded `_pc`, so it costs
	 * a constant offset instead of a _tlv_get_addr per access. */
	PParseTypedefTable typedef_table;
	PParseHashMap function_symbols;
	PParseBoundsState ba;
	PParseSosDoFrame *sos_do_frames;
	int *sos_do_snap_buf, *sos_if_trail_snap;
	int sos_do_cap, sos_snap_cap, sos_if_cap;
	uint32_t td_scope_close;
	bool p1_has_raw_block;
	bool parses_frozen;
} PParseContext;

/* Group-relative token walks. Defined later, but called from Pass 0 above. */
static PParseToken *pparse_enclosing_open(PParseContext *_pc, PParseToken *from, bool stop_at_stmt);
static PParseToken *pparse_prev_sibling(PParseContext *_pc, PParseToken *from);

static PRISM_THREAD_LOCAL PParseContext *pparse_ctx = NULL;

/* Resolve TLS once at each cold boundary and thread `_pc` through hot calls. */
#define PPARSE_CTX() PParseContext *const _pc = pparse_ctx

/* Old global spellings resolve through the threaded context. */
#define pparse_typedef_table (_pc->typedef_table)
#define pparse_function_symbols (_pc->function_symbols)
#define pparse_ba (_pc->ba)
#define pparse_td_scope_close (_pc->td_scope_close)
#define pparse_p1_has_raw_block (_pc->p1_has_raw_block)
#define pparse_sos_do_frames (_pc->sos_do_frames)
#define pparse_sos_do_snap_buf (_pc->sos_do_snap_buf)
#define pparse_sos_if_trail_snap (_pc->sos_if_trail_snap)
#define pparse_sos_do_cap (_pc->sos_do_cap)
#define pparse_sos_snap_cap (_pc->sos_snap_cap)
#define pparse_sos_if_cap (_pc->sos_if_cap)

#define pparse_token_pool (_pc->tp_pool)
#define pparse_token_count (_pc->tp_count)
#define pparse_token_cap (_pc->tp_cap)
#define pparse_token_tag_summary (_pc->pparse_token_tag_summary)
#define pparse_keyword_cache (_pc->kw_cache)
static PRISM_COLD noreturn void pparse_error(char *fmt, ...);
static void pparse_hashmap_put(PParseHashMap *map, char *key, int keylen, void *val);
static void pparse_hashmap_remove(PParseHashMap *map, char *key, int keylen);

static inline bool pparse_at_bol(PParseToken *tok) {
	return tok->flags & PPARSE_TF_AT_BOL;
}

static void pparse_arena_ensure(PParseArena *arena, size_t size) {
	if (arena->current && arena->current->used + size <= arena->current->capacity) return;
	if (arena->current && arena->current->next && size <= arena->current->next->capacity) {
		arena->current = arena->current->next;
		arena->current->used = 0;
		return;
	}
	size_t capacity = size > PPARSE_ARENA_DEFAULT_BLOCK_SIZE ? size : PPARSE_ARENA_DEFAULT_BLOCK_SIZE;
	PParseArenaBlock *block = malloc(sizeof(PParseArenaBlock) + capacity);
	if (!block) pparse_error("out of memory allocating arena block");
	*block = (PParseArenaBlock){.capacity = capacity};
	if (arena->current) {
		block->next = arena->current->next;
		arena->current->next = block;
	} else
		arena->head = block;
	arena->current = block;
}
static void *pparse_arena_alloc_uninit(PParseArena *arena, size_t size) {
	size += size == 0;
	if (size > SIZE_MAX - (PPARSE_ARENA_ALIGN - 1)) pparse_error("pparse_arena_alloc: size overflow");
	size = (size + (PPARSE_ARENA_ALIGN - 1)) & ~(size_t)(PPARSE_ARENA_ALIGN - 1);
	pparse_arena_ensure(arena, size);
	void *ptr = arena->current->data + arena->current->used;
	arena->current->used += size;
	return ptr;
}

static void *pparse_arena_alloc(PParseArena *arena, size_t size) {
	// GCC VRP hint: size is always a valid positive allocation, never a
	if (size > (size_t)PTRDIFF_MAX) __builtin_unreachable();
	void *ptr = pparse_arena_alloc_uninit(arena, size);
	memset(ptr, 0, size);
	return ptr;
}

static void *pparse_arena_realloc(PParseArena *arena, void *old, size_t old_size, size_t new_size) {
	/* Arena callers currently only grow, but this primitive is also used by
	 * generic capacity helpers.  A shrink must preserve the allocation rather
	 * than underflowing the copy/zero lengths below. */
	if (!old) old_size = 0;
	if (old && new_size <= old_size) return old;
	if (old && arena->current) {
		size_t aligned_old = (old_size + (PPARSE_ARENA_ALIGN - 1)) & ~(size_t)(PPARSE_ARENA_ALIGN - 1);
		if ((char *)old + aligned_old == arena->current->data + arena->current->used) {
			size_t aligned_new = (new_size + (PPARSE_ARENA_ALIGN - 1)) & ~(size_t)(PPARSE_ARENA_ALIGN - 1);
			size_t diff = aligned_new - aligned_old;
			if (arena->current->used + diff <= arena->current->capacity) {
				arena->current->used += diff;
				memset((char *)old + old_size, 0, new_size - old_size);
				return old;
			}
		}
	}
	void *p = pparse_arena_alloc_uninit(arena, new_size);
	if (old) memcpy(p, old, old_size);
	memset((char *)p + old_size, 0, new_size - old_size);
	return p;
}

typedef struct {
	void *ptr;
	size_t capacity;
} PParseGrowResult;

/* Centralize capacity checks so generic call sites do not duplicate three
 * control-flow branches apiece. Both helpers inline in optimized builds. */
static inline PParseGrowResult
pparse_vec_ensure_realloc(void *ptr, size_t elem_size, size_t need, size_t cap, size_t init_cap) {
	if (need <= cap) return (PParseGrowResult){ptr, cap};
	size_t new_cap = pparse_vec_grow_cap(cap, need, init_cap);
	if (new_cap > SIZE_MAX / elem_size) pparse_error("allocation overflow");
	ptr = realloc(ptr, elem_size * new_cap);
	if (!ptr) pparse_error("out of memory");
	return (PParseGrowResult){ptr, new_cap};
}

static inline PParseGrowResult pparse_arena_ensure_cap(PParseArena *arena,
						       void *ptr,
						       size_t elem_size,
						       size_t count,
						       size_t cap,
						       size_t init_cap) {
	if (count < cap) return (PParseGrowResult){ptr, cap};
	size_t new_cap = pparse_vec_grow_cap(cap, count, init_cap);
	if (new_cap > SIZE_MAX / elem_size) pparse_error("allocation overflow");
	ptr = pparse_arena_realloc(arena, ptr, elem_size * cap, elem_size * new_cap);
	return (PParseGrowResult){ptr, new_cap};
}

/* need is the minimum used count (typically count or count+1). */
#define pparse_VEC_ENSURE_REALLOC(arr, need, cap, init_cap)                                             \
	do {                                                                                             \
		PParseGrowResult _pparse_grow = pparse_vec_ensure_realloc(                                \
		    (arr), sizeof(*(arr)), (size_t)(need), (size_t)(cap), (size_t)(init_cap));            \
		(arr) = _pparse_grow.ptr;                                                                \
	(cap) = _pparse_grow.capacity;                                                           \
	} while (0)

#define PPARSE_ARENA_ENSURE_CAP(arena, arr, count, cap, init_cap, T)                                    \
	do {                                                                                             \
		PParseGrowResult _pparse_grow = pparse_arena_ensure_cap(                                  \
		    (arena), (arr), sizeof(T), (size_t)(count), (size_t)(cap), (size_t)(init_cap));       \
		(arr) = _pparse_grow.ptr;                                                                \
	(cap) = _pparse_grow.capacity;                                                           \
	} while (0)

typedef struct {
	PParseArenaBlock *block;
	size_t used;
} PParseArenaMark;

static PParseArenaMark pparse_arena_mark(PParseArena *arena) {
	return (PParseArenaMark){arena->current, arena->current ? arena->current->used : 0};
}

static void pparse_arena_restore(PParseArena *arena, PParseArenaMark mark) {
	for (PParseArenaBlock *b = mark.block ? mark.block->next : arena->head; b; b = b->next) b->used = 0;
	arena->current = mark.block;
	if (mark.block) mark.block->used = mark.used;
}

static bool pparse_ctx_init(void) {
	if (pparse_ctx) return true;
	PParseContext *c = calloc(1, sizeof(PParseContext));
	if (!c) return false;
	c->features = PPARSE_F_DEFER | PPARSE_F_ZEROINIT | PPARSE_F_LINE_DIR | PPARSE_F_FLATTEN | PPARSE_F_ORELSE;
	c->tp_count = 1; // 0 reserved as NULL sentinel
	pparse_ctx = c;
	return true;
}

static void pparse_token_pool_ensure(size_t need) {
	PPARSE_CTX();
	if (need <= pparse_token_cap) return;
	size_t new_cap = pparse_vec_grow_cap(pparse_token_cap, need, 4096);
	const size_t max_cap = SIZE_MAX / sizeof(PParseToken) < UINT32_MAX
				 ? SIZE_MAX / sizeof(PParseToken)
				 : UINT32_MAX;
	if (new_cap > max_cap)
		pparse_error("token pool capacity exceeded");
	PParseToken *p = realloc(pparse_token_pool, new_cap * sizeof(PParseToken));
	if (!p) pparse_error("out of memory allocating token pool");
	pparse_token_pool = p;
	pparse_token_cap = (uint32_t)new_cap;
	// Pool index 0 is the NULL sentinel and must never look like a real token.
	if (pparse_token_count <= 1) {
		memset(&pparse_token_pool[0], 0, sizeof(PParseToken));
	}
}

/* match_idx is a source offset for every token; pair_idx stores delimiters. */
static inline PRISM_PURE char *pparse_loc(PParseContext *_pc, PParseToken *tok) {
	return _pc->token_source + tok->match_idx;
}

static inline PRISM_PURE PParseToken *pparse_next(PParseContext *_pc, PParseToken *tok) {
	if (tok->kind == PPARSE_TK_EOF) return NULL;
	if (__builtin_expect(!(tok->flags & PPARSE_TF_LINK_JUMP), 1)) return tok + 1;
	return &pparse_token_pool[tok->parse_data];
}

/* Tokenization rejects unbalanced input before analysis. Once a delimiter
 * flag has proved the token is an opener/closer, its pair index is nonzero. */
static inline PRISM_ALWAYS_INLINE PRISM_PURE PParseToken *pparse_pair_known(PParseToken *tok) {
	PPARSE_CTX();
	return &pparse_token_pool[tok->pair_idx];
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE uint32_t pparse_idx(PParseContext *_pc, PParseToken *tok) {
	if (!tok) return 0;
	uintptr_t bd = (uintptr_t)tok - (uintptr_t)pparse_token_pool;
	return (uint32_t)(bd / sizeof(PParseToken));
}

static inline PRISM_PURE uint32_t pparse_fast_hash(char *s, uint32_t len) {
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

static PRISM_HOT PRISM_PURE void *pparse_hashmap_get_hashed(PParseHashMap *map, char *key, int keylen, uint32_t hash) {
	int mask = map->capacity - 1;
	for (int i = 0; i <= mask; i++) {
		PParseHashEntry *ent = &map->buckets[(hash + i) & mask];
		if (__builtin_expect(!ent->key, 0)) return NULL;
		if (__builtin_expect(ent->key == PPARSE_TOMBSTONE, 0)) continue;
		if (ent->hash == hash && ent->key_len == (uint16_t)keylen &&
		    prism_memeq_runtime_sized(ent->key, key, (uint32_t)keylen))
			return ent->val;
	}
	return NULL;
}

static PRISM_PURE void *pparse_hashmap_get(PParseHashMap *map, char *key, int keylen) {
	if (__builtin_expect(!map->buckets, 0)) return NULL;
	return pparse_hashmap_get_hashed(map, key, keylen, pparse_fast_hash(key, keylen));
}

static inline PRISM_PURE int pparse_hashmap_index_hashed(PParseHashMap *map, char *key, int keylen, uint32_t hash) {
	if (__builtin_expect(!map->buckets, 0)) return -1;
	void *val = pparse_hashmap_get_hashed(map, key, keylen, hash);
	return val ? (int)(intptr_t)val - 1 : -1;
}

static void pparse_hashmap_resize(PParseHashMap *map, int newcap) {
	PPARSE_CTX();
	PParseHashMap new_map = {.buckets = pparse_arena_alloc(&_pc->main_arena, (size_t)newcap * sizeof(PParseHashEntry)),
			   .capacity = newcap};
	int new_mask = newcap - 1;
	for (int i = 0; i < map->capacity; i++) {
		PParseHashEntry *ent = &map->buckets[i];
		if (ent->key && ent->key != PPARSE_TOMBSTONE) {
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
static void pparse_hashmap_put_hashed(PParseHashMap *map, char *key, int keylen, void *val, uint32_t hash) {
	PPARSE_CTX();
	if (__builtin_expect(!map->buckets, 0)) {
		map->buckets = pparse_arena_alloc(&_pc->main_arena, 32 * sizeof(PParseHashEntry));
		map->capacity = 32;
	} else if (__builtin_expect((uint64_t)map->used * 10 >= (uint64_t)map->capacity * 7, 0)) {
		pparse_hashmap_resize(map, map->capacity * 2);
	}

	int mask = map->capacity - 1;
	int first_empty = -1;
	for (int i = 0; i <= mask; i++) {
		int idx = (hash + i) & mask;
		PParseHashEntry *ent = &map->buckets[idx];
		if (ent->key && ent->key != PPARSE_TOMBSTONE && ent->hash == hash &&
		    ent->key_len == (uint16_t)keylen && prism_memeq_runtime_sized(ent->key, key, (uint32_t)keylen)) {
			ent->val = val;
			return;
		}

		if (first_empty < 0 && (!ent->key || ent->key == PPARSE_TOMBSTONE)) first_empty = idx;
		if (!ent->key) break;
	}

	if (first_empty < 0) pparse_error("pparse_hashmap_put: no empty slot found (internal pparse_error)");
	PParseHashEntry *ent = &map->buckets[first_empty];
	ent->key = key;
	ent->key_len = keylen;
	ent->hash = hash;
	ent->val = val;
	map->used++;
}

static void pparse_hashmap_put(PParseHashMap *map, char *key, int keylen, void *val) {
	pparse_hashmap_put_hashed(map, key, keylen, val, pparse_fast_hash(key, keylen));
}

/* Tokenizer-only map access: parse_data still contains the lexer hash here.
 * Later parse passes use pparse_token_name_hash() because finalization may repurpose it. */
static inline PRISM_PURE void *pparse_lex_token_map_get(PParseHashMap *map, PParseToken *tok) {
	PPARSE_CTX();
	if (!map->buckets) return NULL;
	return pparse_hashmap_get_hashed(map, pparse_loc(_pc, tok), tok->len, tok->parse_data);
}

static inline void pparse_lex_token_map_put(PParseHashMap *map, PParseToken *tok, void *value) {
	PPARSE_CTX();
	pparse_hashmap_put_hashed(map, pparse_loc(_pc, tok), tok->len, value, tok->parse_data);
}


static char *pparse_intern_filename(const char *name) {
	PPARSE_CTX();
	size_t len = strlen(name) + 1;
	char *copy = pparse_arena_alloc_uninit(&_pc->main_arena, len);
	return memcpy(copy, name, len);
}

static inline PParseFile *pparse_tok_file(PParseToken *tok) {
	PPARSE_CTX();
	if (tok->file_idx >= (uint32_t)_pc->input_file_count) return _pc->current_file;
	return _pc->input_files[tok->file_idx];
}

static int pparse_tok_line_no(PParseToken *tok) {
	return tok->line_no;
}

#ifdef PRISM_LIB_MODE
static noreturn void pparse_lib_error_jump(int line) {
	PPARSE_CTX();
	_pc->error_line = line;
	longjmp(_pc->error_jmp, 1);
}

static inline bool pparse_lib_error_enabled(void) {
	PPARSE_CTX();
	return pparse_ctx && _pc->error_jmp_set;
}

static noreturn void pparse_lib_errorf(int line, const char *fmt, va_list ap) {
	PPARSE_CTX();
	vsnprintf(_pc->error_msg, sizeof(_pc->error_msg), fmt, ap);
	pparse_lib_error_jump(line);
}
#endif

static PRISM_COLD noreturn void pparse_error(char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
#ifdef PRISM_LIB_MODE
	if (pparse_lib_error_enabled()) pparse_lib_errorf(0, fmt, ap);
#endif
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static PRISM_COLD void
pparse_verror_at(char *filename, char *input, int line_no, char *loc, const char *severity, const char *fmt,
	  va_list ap) {
	/* GCC-style severity label so `-fno-safety` warnings are not mistaken
	 * for hard errors by humans or CI log scrapers (which key on
	 * `file:line: warning:` / `file:line: pparse_error:`).  Defaults to "pparse_error". */
	const char *sev = severity ? severity : "pparse_error";
	if (!input || !loc || line_no <= 0) {
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

static int pparse_count_lines(char *base, char *loc) {
	int n = 1;
	for (char *p = base; p < loc; p++)
		if (*p == '\n') n++;
	return n;
}

PRISM_COLD noreturn void pparse_error_at(char *loc, char *fmt, ...) {
	PPARSE_CTX();
	va_list ap;
	va_start(ap, fmt);
#ifdef PRISM_LIB_MODE
	int line = _pc->current_file ? pparse_count_lines(_pc->token_source, loc) : 0;
	if (pparse_lib_error_enabled()) pparse_lib_errorf(line, fmt, ap);
#endif
	if (_pc->current_file)
		pparse_verror_at(_pc->current_file->name,
			  _pc->token_source,
			  pparse_count_lines(_pc->token_source, loc),
			  loc,
			  "pparse_error",
			  fmt,
			  ap);
	else {
		fprintf(stderr, "pparse_error: ");
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
	}
	va_end(ap);
	exit(1);
}

PRISM_COLD noreturn void pparse_error_tok(PParseToken *tok, const char *fmt, ...) {
	PPARSE_CTX();
	va_list ap;
	va_start(ap, fmt);
	PParseFile *f = pparse_tok_file(tok);
#ifdef PRISM_LIB_MODE
	if (pparse_lib_error_enabled()) pparse_lib_errorf(pparse_tok_line_no(tok), fmt, ap);
#endif
	pparse_verror_at(f->name, _pc->token_source, pparse_tok_line_no(tok), pparse_loc(_pc, tok), "pparse_error", fmt, ap);
	va_end(ap);
	exit(1);
}

static PRISM_COLD void pparse_warn_tok(PParseToken *tok, const char *fmt, ...) {
#ifdef PRISM_LIB_MODE
	(void)tok;
	(void)fmt;
	return; // Suppress warnings in library mode
#else
	PPARSE_CTX();
	va_list ap;
	va_start(ap, fmt);
	PParseFile *f = pparse_tok_file(tok);
	pparse_verror_at(f->name, _pc->token_source, pparse_tok_line_no(tok), pparse_loc(_pc, tok), "warning", fmt, ap);
	va_end(ap);
#endif
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool
pparse_equal_n(PParseContext *_pc, PParseToken *tok, const char *op, uint32_t len) {
	/* Operator/keyword spellings are short and len is at least two. */
	return tok->len == len && tok->ch0 == (uint8_t)op[0] &&
	       prism_memeq_runtime(pparse_loc(_pc, tok) + 1, op + 1, (size_t)len - 1u);
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool pparse_equal_1(PParseToken *tok, char c) {
	return (tok->len == 1) & (tok->ch0 == (uint8_t)c);
}

#define pparse_match_ch pparse_equal_1

#define pparse_CH(c) (1ULL << ((c) - 32))
#define pparse_match_set(tok, mask)                                                                                 \
	(((tok)->len == 1) & ((unsigned)((tok)->ch0 - 32) < 64u) &                                              \
	 (((mask) >> (((tok)->ch0 - 32) & 63)) & 1u))

#define pparse_is_stmt_expr_open(t) ((t)->flags & PPARSE_TF_STMT_EXPR)

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool pparse_is_else_kw(PParseToken *t) {
	return ((t->tag & PPARSE_TT_IF) != 0) & (t->ch0 == 'e');
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool pparse_is_do_kw(PParseToken *t) {
	return ((t->tag & PPARSE_TT_LOOP) != 0) & (t->ch0 == 'd');
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool pparse_is_else_or_do(PParseToken *t) {
	return pparse_is_else_kw(t) | pparse_is_do_kw(t);
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool
pparse_equal_2(PParseContext *_pc, PParseToken *tok, const char *s) {
	if (tok->len != 2 || tok->ch0 != (uint8_t)s[0]) return false;
	return pparse_loc(_pc, tok)[1] == s[1];
}

static inline PRISM_PURE bool pparse_is_gnu_label_decl_head(PParseToken *tok) {
	PPARSE_CTX();
	return tok->len == 9 && tok->ch0 == '_' && prism_memeq_static(pparse_loc(_pc, tok), "__label__", 9);
}

static void pparse_init_keyword_map(void) {
	PPARSE_CTX();
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
	    {"return", PPARSE_TT_SKIP_DECL | PPARSE_TT_RETURN, true},
	    {"if", PPARSE_TT_SKIP_DECL | PPARSE_TT_IF, true},
	    {"else", PPARSE_TT_SKIP_DECL | PPARSE_TT_IF, true},
	    {"for", PPARSE_TT_SKIP_DECL | PPARSE_TT_LOOP, true},
	    {"while", PPARSE_TT_SKIP_DECL | PPARSE_TT_LOOP, true},
	    {"do", PPARSE_TT_SKIP_DECL | PPARSE_TT_LOOP, true},
	    {"switch", PPARSE_TT_SKIP_DECL | PPARSE_TT_SWITCH, true},
	    {"case", PPARSE_TT_SKIP_DECL | PPARSE_TT_CASE, true},
	    {"default", PPARSE_TT_SKIP_DECL | PPARSE_TT_DEFAULT, true},
	    {"break", PPARSE_TT_SKIP_DECL | PPARSE_TT_BREAK, true},
	    {"continue", PPARSE_TT_SKIP_DECL | PPARSE_TT_CONTINUE, true},
	    {"goto", PPARSE_TT_SKIP_DECL | PPARSE_TT_GOTO, true},
	    {"sizeof", PPARSE_TT_SKIP_DECL, true, PPARSE_TF_SIZEOF},
	    {"alignof", PPARSE_TT_SKIP_DECL, true, PPARSE_TF_SIZEOF | PPARSE_TF_ALIGNOF | PPARSE_TF_SOFT_KW},
	    {"_Alignof", PPARSE_TT_SKIP_DECL, true, PPARSE_TF_SIZEOF | PPARSE_TF_ALIGNOF},
	    {"_Generic", PPARSE_TT_SKIP_DECL | PPARSE_TT_GENERIC, true},
	    {"_Static_assert", PPARSE_TT_SKIP_DECL, true, PPARSE_TF_STATIC_ASSERT},
	    {"static_assert", PPARSE_TT_SKIP_DECL, true, PPARSE_TF_SOFT_KW | PPARSE_TF_STATIC_ASSERT},
	    {"struct", PPARSE_TT_TYPE | PPARSE_TT_SUE, true},
	    {"union", PPARSE_TT_TYPE | PPARSE_TT_SUE, true},
	    {"enum", PPARSE_TT_TYPE | PPARSE_TT_SUE, true},
	    {"typedef", PPARSE_TT_SKIP_DECL | PPARSE_TT_TYPEDEF, true},
	    {"static", PPARSE_TT_QUALIFIER | PPARSE_TT_SKIP_DECL | PPARSE_TT_STORAGE, true},
	    {"extern", PPARSE_TT_SKIP_DECL | PPARSE_TT_STORAGE, true},
	    {"inline", PPARSE_TT_INLINE, true},
	    {"const", PPARSE_TT_QUALIFIER | PPARSE_TT_CONST, true},
	    {"volatile", PPARSE_TT_QUALIFIER | PPARSE_TT_VOLATILE, true},
	    {"restrict", PPARSE_TT_QUALIFIER, true},
	    {"_Atomic", PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE, true},
	    {"_Nonnull", PPARSE_TT_QUALIFIER, true},
	    {"_Nullable", PPARSE_TT_QUALIFIER, true},
	    {"_Null_unspecified", PPARSE_TT_QUALIFIER, true},
	    {"_Noreturn", PPARSE_TT_SKIP_DECL | PPARSE_TT_INLINE, true},
	    {"noreturn", PPARSE_TT_SKIP_DECL | PPARSE_TT_INLINE, true, PPARSE_TF_SOFT_KW},
	    {"__inline", PPARSE_TT_INLINE, true},
	    {"__inline__", PPARSE_TT_INLINE, true},
	    {"_Thread_local", PPARSE_TT_STORAGE, true},
	    {"__thread", PPARSE_TT_STORAGE, true},
	    {"constexpr", PPARSE_TT_QUALIFIER, true, PPARSE_TF_SOFT_KW},
	    {"thread_local", PPARSE_TT_QUALIFIER | PPARSE_TT_SKIP_DECL | PPARSE_TT_STORAGE, true, PPARSE_TF_SOFT_KW},
	    {"void", PPARSE_TT_TYPE, true},
	    {"char", PPARSE_TT_TYPE, true},
	    {"short", PPARSE_TT_TYPE, true},
	    {"int", PPARSE_TT_TYPE, true},
	    {"long", PPARSE_TT_TYPE, true},
	    {"float", PPARSE_TT_TYPE, true},
	    {"double", PPARSE_TT_TYPE, true},
	    {"signed", PPARSE_TT_TYPE, true},
	    {"unsigned", PPARSE_TT_TYPE, true},
	    {"_Bool", PPARSE_TT_TYPE, true},
	    {"bool", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"_Complex", PPARSE_TT_TYPE, true},
	    {"_Imaginary", PPARSE_TT_TYPE, true},
	    /* Extension type spellings are soft: after an established type they
	     * are ordinary declarator names (`int _Float32;`, `int __int64;`) on
	     * clang/gcc, matching `bool`. As a lone type specifier they keep
	     * PPARSE_TT_TYPE so `_Float32 x;` / `__int64 y;` still zero-init. */
	    {"__int128", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"__int128_t", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"__uint128", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"__uint128_t", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"__int8", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"__int16", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"__int32", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"__int64", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"__float128", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"__float80", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"__fp16", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"__bf16", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"_Float16", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"_Float32", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"_Float64", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"_Float128", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"_Float32x", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"_Float64x", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"_Float128x", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"_Decimal32", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"_Decimal64", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"_Decimal128", PPARSE_TT_TYPE, true, PPARSE_TF_SOFT_KW},
	    {"typeof_unqual", PPARSE_TT_TYPE | PPARSE_TT_TYPEOF, true, PPARSE_TF_SOFT_KW},
	    {"__typeof_unqual__", PPARSE_TT_TYPE | PPARSE_TT_TYPEOF, true},
	    {"__typeof_unqual", PPARSE_TT_TYPE | PPARSE_TT_TYPEOF, true},
	    {"auto", PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE, true},
	    {"register", PPARSE_TT_QUALIFIER | PPARSE_TT_REGISTER, true},
	    {"_Alignas", PPARSE_TT_QUALIFIER | PPARSE_TT_ALIGNAS, true},
	    {"alignas", PPARSE_TT_QUALIFIER | PPARSE_TT_ALIGNAS, true, PPARSE_TF_SOFT_KW},
	    {"typeof", PPARSE_TT_TYPE | PPARSE_TT_TYPEOF, true, PPARSE_TF_SOFT_KW},
	    {"__typeof__", PPARSE_TT_TYPE | PPARSE_TT_TYPEOF, true},
	    {"__typeof", PPARSE_TT_TYPE | PPARSE_TT_TYPEOF, true},
	    {"__auto_type", PPARSE_TT_TYPE | PPARSE_TT_TYPEOF, true},
	    {"_BitInt", PPARSE_TT_TYPE | PPARSE_TT_BITINT, true},
	    {"asm", PPARSE_TT_SKIP_DECL | PPARSE_TT_ASM, true, PPARSE_TF_SOFT_KW},
	    {"__asm__", PPARSE_TT_SKIP_DECL | PPARSE_TT_ASM, true},
	    {"__asm", PPARSE_TT_SKIP_DECL | PPARSE_TT_ASM, true},
	    {"__attribute__", PPARSE_TT_ATTR | PPARSE_TT_QUALIFIER, true},
	    {"__attribute", PPARSE_TT_ATTR | PPARSE_TT_QUALIFIER, true},
	    {"__declspec", PPARSE_TT_ATTR | PPARSE_TT_QUALIFIER, true},
	    {"__cdecl", 0, true, PPARSE_TF_MS_CC},
	    {"__stdcall", 0, true, PPARSE_TF_MS_CC},
	    {"__fastcall", 0, true, PPARSE_TF_MS_CC},
	    {"__thiscall", 0, true, PPARSE_TF_MS_CC},
	    {"__vectorcall", 0, true, PPARSE_TF_MS_CC},
	    {"_cdecl", 0, true, PPARSE_TF_MS_CC},
	    {"_stdcall", 0, true, PPARSE_TF_MS_CC},
	    {"_fastcall", 0, true, PPARSE_TF_MS_CC},
	    {"cdecl", 0, true, PPARSE_TF_MS_CC | PPARSE_TF_SOFT_KW},
	    {"stdcall", 0, true, PPARSE_TF_MS_CC | PPARSE_TF_SOFT_KW},
	    {"_Pragma", PPARSE_TT_ATTR, true},
	    {"__pragma", PPARSE_TT_ATTR, true},
	    {"__extension__", PPARSE_TT_INLINE, true},
	    {"__builtin_va_list", 0, true},
	    {"__builtin_va_arg", 0, true},
	    {"__builtin_offsetof", 0, true, PPARSE_TF_SIZEOF | PPARSE_TF_OFFSETOF},
	    {"offsetof", 0, true, PPARSE_TF_SIZEOF | PPARSE_TF_OFFSETOF | PPARSE_TF_SOFT_KW},
	    {"__restrict", PPARSE_TT_QUALIFIER, true},
	    {"__restrict__", PPARSE_TT_QUALIFIER, true},
	    {"__builtin_types_compatible_p", 0, true},
	    {"defer", PPARSE_TT_DEFER, true},
	    {"orelse", PPARSE_TT_ORELSE, true},
	    {"raw", 0, true, PPARSE_TF_RAW},
	    {"exit", PPARSE_TT_NORETURN_FN, false},
	    {"_Exit", PPARSE_TT_NORETURN_FN, false},
	    {"_exit", PPARSE_TT_NORETURN_FN, false},
	    {"abort", PPARSE_TT_NORETURN_FN, false},
	    {"quick_exit", PPARSE_TT_NORETURN_FN, false},
	    {"__builtin_trap", PPARSE_TT_NORETURN_FN, false},
	    {"__builtin_unreachable", PPARSE_TT_NORETURN_FN, false},
	    {"thrd_exit", PPARSE_TT_NORETURN_FN, false},
	    {"setjmp", PPARSE_TT_SPECIAL_FN, false},
	    {"longjmp", PPARSE_TT_SPECIAL_FN, false},
	    {"_setjmp", PPARSE_TT_SPECIAL_FN, false},
	    {"_longjmp", PPARSE_TT_SPECIAL_FN, false},
	    {"sigsetjmp", PPARSE_TT_SPECIAL_FN, false},
	    {"siglongjmp", PPARSE_TT_SPECIAL_FN, false},
	    {"__sigsetjmp", PPARSE_TT_SPECIAL_FN, false},
	    {"__siglongjmp", PPARSE_TT_SPECIAL_FN, false},
	    {"__setjmp", PPARSE_TT_SPECIAL_FN, false},
	    {"__longjmp", PPARSE_TT_SPECIAL_FN, false},
	    {"__longjmp_chk", PPARSE_TT_SPECIAL_FN, false},
	    {"pthread_exit", PPARSE_TT_SPECIAL_FN, false},
	    {"__builtin_setjmp", PPARSE_TT_SPECIAL_FN, false},
	    {"__builtin_longjmp", PPARSE_TT_SPECIAL_FN, false},
	    {"__builtin_setjmp_receive", PPARSE_TT_SPECIAL_FN, false},
	    {"savectx", PPARSE_TT_SPECIAL_FN, false},
	    {"vfork", PPARSE_TT_SPECIAL_FN, false},
	};
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

	memset(pparse_keyword_cache, 0, sizeof(pparse_keyword_cache));
	for (size_t i = 0; i < sizeof(entries) / sizeof(*entries); i++) {
		int len = strlen(entries[i].name);
		uint64_t val = entries[i].is_kw ? (entries[i].tag | PPARSE_KW_MARKER) : entries[i].tag;
		val |= (uint64_t)entries[i].extra_flags << PPARSE_KW_FLAGS_SHIFT;
		val |= (uint64_t)(uint8_t)len << PPARSE_KW_LEN_SHIFT;
		unsigned shadow = entries[i].tag & PPARSE_TT_DEFER ? PPARSE_KWSHADOW_DEFER
				  : entries[i].tag & PPARSE_TT_ORELSE ? PPARSE_KWSHADOW_ORELSE
				  : entries[i].extra_flags & PPARSE_TF_RAW ? PPARSE_KWSHADOW_RAW : 0;
		val |= (uint64_t)shadow << PPARSE_KW_SHADOW_SHIFT;
		unsigned slot = pparse_KEYWORD_HASH(entries[i].name, len);
		unsigned probe = 0;
		while (pparse_keyword_cache[slot & 255].name) {
			slot++;
			probe++;
		}
		/* Insertion probes without a bound, lookup gives up after
		 * PPARSE_KW_MAX_PROBE. A keyword displaced further than that would be
		 * stored and then never found again -- silently not a keyword. At 141
		 * entries in 256 slots the worst displacement is 10, so the two agree
		 * with room to spare; this fires if a future keyword closes the gap. */
		if (probe >= PPARSE_KW_MAX_PROBE)
			pparse_error("internal: keyword '%s' displaced %u slots, past the lookup probe limit",
				     entries[i].name, probe);
		pparse_keyword_cache[slot & 255] = (PParseKeywordEntry){.name = entries[i].name, .value = val};
	}
}

/* C11 6.4.3: \uXXXX (4 hex) or \UXXXXXXXX (8 hex). Returns length or 0. */
static int pparse_read_ucn(char *p) {
	if (*p != '\\') return 0;
	int nhex;
	if (p[1] == 'u')
		nhex = 4;
	else if (p[1] == 'U')
		nhex = 8;
	else
		return 0;
	for (int i = 0; i < nhex; i++)
		if (!PPARSE_IS_XDIGIT(p[2 + i])) return 0;
	return 2 + nhex;
}


#define pparse_SWAR_HAS_ZERO(v) (((v) - 0x0101010101010101ULL) & ~(v) & 0x8080808080808080ULL)
#define pparse_SWAR_BROADCAST(c) (0x0101010101010101ULL * (uint8_t)(c))

static inline __attribute__((always_inline)) PParseToken *
pparse_new_token(PParseTokenKind kind, char *start, char *end, PParseTokState *ts) {
	PPARSE_CTX();
	PParseFile *cf = _pc->current_file;
	if (__builtin_expect(pparse_token_count >= pparse_token_cap, 0))
		pparse_token_pool_ensure((size_t)pparse_token_count + 1);
	uint32_t token_idx = pparse_token_count++;
	PParseToken *tok = &pparse_token_pool[token_idx];
	/* This mirrors the emit-time file predicate without consulting input_files
	 * on the hot suppression path. */
	*tok = (PParseToken){.len = (uint32_t)(end - start),
			     .kind = kind,
			     .flags = (ts->at_bol ? PPARSE_TF_AT_BOL : 0) |
				      (ts->has_space ? PPARSE_TF_HAS_SPACE : 0) |
				      (cf->skip_emit ? PPARSE_TF_SYS_SKIP : 0),
			     .ch0 = (uint8_t)*start};
	tok->match_idx = (uint32_t)(start - _pc->token_source);
	{
		long long ln = (long long)ts->line_no + cf->line_delta;
		int clamped = ln > 0x1FFFF ? 0x1FFFF : (ln < -0x20000 ? -0x20000 : (int)ln);
		tok->line_no = clamped;
	}
	tok->file_idx = cf->file_no;
	ts->at_bol = ts->has_space = false;
	return tok;
}

static char *pparse_string_literal_end(char *p) {
	for (; *p != '"'; p++) {
		if (*p == '\0') pparse_error_at(p, "unclosed string literal");
		if (*p == '\\') {
			if (p[1] == '\0') pparse_error_at(p, "unclosed string literal");
			p++;
		}
	}
	return p;
}
static PParseToken *pparse_read_string_literal(char *start, char *quote, PParseTokState *ts) {
	char *end = pparse_string_literal_end(quote + 1);
	return pparse_new_token(PPARSE_TK_STR, start, end + 1, ts);
}

static PParseToken *pparse_read_char_literal(char *start, char *quote, PParseTokState *ts) {
	char *p = quote + 1;
	if (*p == '\0') pparse_error_at(start, "unclosed char literal");
	for (; *p != '\''; p++) {
		if (*p == '\n' || *p == '\0') pparse_error_at(p, "unclosed char literal");
		if (*p == '\\') {
			p++;
			if (*p == '\0') pparse_error_at(p, "unclosed char literal");
		}
	}
	return pparse_new_token(PPARSE_TK_NUM, start, p + 1, ts);
}

static inline bool pparse_p0_token_can_name_function(PParseToken *tok) {
	return tok && (tok->kind == PPARSE_TK_IDENT || (tok->tag & (PPARSE_TT_DEFER | PPARSE_TT_ORELSE)) ||
		       (tok->flags & (PPARSE_TF_RAW | PPARSE_TF_SOFT_KW)));
}

/* Phase-zero scans run before Prism can splice the logical token stream. */
static inline PParseToken *pparse_p0_next(PParseToken *tok) {
	return tok + 1;
}

static PParseToken *pparse_p0_attribute_group_end(PParseToken *tok) {
	PPARSE_CTX();
	if (!tok) return NULL;
	if (tok->flags & PPARSE_TF_C23_ATTR) return pparse_pair_known(tok);
	if (tok->kind <= PPARSE_TK_KEYWORD &&
	    (pparse_equal(tok, "__attribute__") || pparse_equal(tok, "__attribute") || pparse_equal(tok, "__declspec"))) {
		PParseToken *open = pparse_p0_next(tok);
		if (open->ch0 == '(') return pparse_pair_known(open);
	}
	return NULL;
}

/* Find an attribute's function owner; +1 compensates for the loop decrement. */
static PParseToken *pparse_p0_attr_owner_backward(PParseContext *_pc, PParseToken *origin) {
	for (uint32_t pi = pparse_idx(_pc, origin); pi > 0; pi--) {
		PParseToken *pt = &pparse_token_pool[pi - 1];
		if (pt->kind == PPARSE_TK_PREP_DIR) continue;
		if (pt->ch0 == ';' || pt->ch0 == '{' || pt->ch0 == '}') break;
		if (pt->ch0 == ')' || pt->ch0 == ']') {
			pi = pparse_idx(_pc, pparse_pair_known(pt)) + 1;
			continue;
		}
		if (pparse_p0_token_can_name_function(pt) && pparse_p0_next(pt)->ch0 == '(') {
			if (pt->tag & (PPARSE_TT_SKIP_DECL | PPARSE_TT_INLINE | PPARSE_TT_QUALIFIER |
				       PPARSE_TT_TYPE | PPARSE_TT_STORAGE))
				continue;
			return pt;
		}
	}
	return NULL;
}

static PParseToken *pparse_p0_previous_token(PParseToken *tok) {
	PPARSE_CTX();
	for (uint32_t i = pparse_idx(_pc, tok); i > 0;) {
		PParseToken *prev = &pparse_token_pool[--i];
		if (prev->kind != PPARSE_TK_PREP_DIR) return prev;
	}
	return NULL;
}

static PParseFile *pparse_add_input_file(PParseFile file) {
	PPARSE_CTX();
	file.file_no = _pc->input_file_count;
	PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena,
			 _pc->input_files,
			 _pc->input_file_count + 1,
			 _pc->input_file_capacity,
			 16,
			 PParseFile *);
	PParseFile *stored = pparse_arena_alloc(&_pc->main_arena, sizeof(PParseFile));
	*stored = file;
	_pc->input_files[_pc->input_file_count++] = stored;
	return stored;
}

static void pparse_ensure_keyword_cache(void) {
	PPARSE_CTX();
	if (!pparse_keyword_cache[0].name && !pparse_keyword_cache[1].name) pparse_init_keyword_map();
}

/* Track the three dialect spellings in the directive pass the tokenizer must
 * make anyway. This preserves macro point-of-definition and #undef semantics. */
static inline unsigned pparse_update_kw_shadow(char *p, unsigned mask) {
	if (*p == '#') p++;
	else if (*p == '%' && p[1] == ':') p += 2;
	else if (*p == '?' && p[1] == '?' && p[2] == '=') p += 3;
	while (pparse_is_hspace(*p)) p++;
	bool define = prism_memeq_static(p, "define", 6) && !pparse_ident_char[(unsigned char)p[6]];
	bool undef = prism_memeq_static(p, "undef", 5) && !pparse_ident_char[(unsigned char)p[5]];
	if (!define && !undef) return mask;
	p += define ? 6 : 5;
	while (pparse_is_hspace(*p)) p++;
	char *name = p;
	while (pparse_ident_char[(unsigned char)*p]) p++;
	int len = (int)(p - name);
	unsigned bit = len == 3 && prism_memeq_static(name, "raw", 3)     ? PPARSE_KWSHADOW_RAW
		       : len == 5 && prism_memeq_static(name, "defer", 5) ? PPARSE_KWSHADOW_DEFER
		       : len == 6 && prism_memeq_static(name, "orelse", 6) ? PPARSE_KWSHADOW_ORELSE
								     : 0;
	return define ? mask | bit : mask & ~bit;
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE uint64_t
pparse_keyword_lookup(PParseContext *_pc, char *key, int keylen) {
	if (keylen < 2) return 0;
	unsigned slot = pparse_KEYWORD_HASH(key, keylen);
	for (int i = 0; i < PPARSE_KW_MAX_PROBE; i++) {
		PParseKeywordEntry *ent = &pparse_keyword_cache[(slot + i) & 255];
		if (!ent->name) return 0;
		if ((uint8_t)(ent->value >> PPARSE_KW_LEN_SHIFT) == keylen &&
		    prism_memeq_runtime_sized(ent->name, key, (uint32_t)keylen))
			return ent->value;
	}
	return 0;
}
static int pparse_read_punct(char *p) {
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
		return 1 + 2 * ((p[1] == '.') & (p[2] == '.')); // ... or .
	case '=': return 1 + (p[1] == '='); // == or =
	case '!': return 1 + (p[1] == '='); // != or !
	case '-': return 1 + ((p[1] == '>') | (p[1] == '=') | (p[1] == '-'));
	case '+': return 1 + ((p[1] == '=') | (p[1] == '+'));
	case '*': return 1 + (p[1] == '='); // *= or *
	case '/': return 1 + (p[1] == '='); // /= or /
	case '%':
		if (p[1] == ':' && p[2] == '%' && p[3] == ':') return -4; // %:%: (digraph ##)
		if (p[1] == ':') return -2;				  // %: (digraph #)
		if (p[1] == '>') return -2;				  // %> (digraph })
		if (p[1] == '=') return 2;				  // %=
		return 1;
	case '&': return 1 + ((p[1] == '&') | (p[1] == '='));
	case '|': return 1 + ((p[1] == '|') | (p[1] == '='));
	case '^': return 1 + (p[1] == '=');	 // ^= or ^
	case '#': return 1 + (p[1] == '#');	 // ## or #
	case ':': return 1 - 3 * (p[1] == '>'); // :> (digraph) or :
	default: return ((unsigned char)*p > 0x20 && *p != 0x7f && !PPARSE_IS_ALNUM(*p)) ? 1 : 0;
	}
}
/* Phase 2: `\`+newline (and `\r\n`) inside // comments is spliced, so the
 * comment continues on the next physical line. Line numbers still advance. */
static char *pparse_skip_line_comment(char *p, PParseTokState *ts) {
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
		uint64_t nl_mask = pparse_SWAR_BROADCAST('\n');
		uint64_t bs_mask = pparse_SWAR_BROADCAST('\\');
		for (;;) {
			uint64_t v;
			memcpy(&v, p, 8);
			if (pparse_SWAR_HAS_ZERO(v) || pparse_SWAR_HAS_ZERO(v ^ nl_mask) || pparse_SWAR_HAS_ZERO(v ^ bs_mask)) {
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
static char *pparse_skip_block_comment(char *p, PParseTokState *ts) {
	while ((uintptr_t)p & 7) {
		if (*p == '\0') pparse_error_at(p, "unclosed block comment");
		if (*p == '\n') {
			ts->line_no++;
			ts->at_bol = true;
		}
		if (p[0] == '*' && p[1] == '/') return p + 2;
		p++;
	}
	uint64_t nl_mask = pparse_SWAR_BROADCAST('\n');
	uint64_t star_mask = pparse_SWAR_BROADCAST('*');
	for (;;) {
		uint64_t v;
		memcpy(&v, p, 8);
		if (pparse_SWAR_HAS_ZERO(v) || pparse_SWAR_HAS_ZERO(v ^ nl_mask) || pparse_SWAR_HAS_ZERO(v ^ star_mask)) {
			for (int i = 0; i < 8; i++) {
				if (p[i] == '\0') pparse_error_at(p + i, "unclosed block comment");
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
// Scan C++11/C23 raw string literal: R"delim(content)delim"
static char *pparse_raw_string_literal_end(char *p, PParseTokState *ts) {
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

	pparse_error_at(p, "unclosed raw string literal");
}
static PParseToken *pparse_read_raw_string_literal(char *start, char *quote, PParseTokState *ts) {
	char *end = pparse_raw_string_literal_end(quote, ts);
	if (!end) pparse_error_at(start, "invalid raw string literal");
	return pparse_new_token(PPARSE_TK_STR, start, end, ts);
}
static inline void pparse_classify_punct(PParseToken *t) {
	PPARSE_CTX();
	char c = t->ch0;
	if (t->len == 1) {
		if (c == '=' || c == '[') t->tag = PPARSE_TT_ASSIGN;
		else if (c == '.')
			t->tag = PPARSE_TT_MEMBER;
		else if (c == '{' || c == '}' || c == ';' || c == ':')
			t->tag = PPARSE_TT_STRUCTURAL;
		if (c == '(' || c == '[' || c == '{') t->flags |= PPARSE_TF_OPEN;
		else if (c == ')' || c == ']' || c == '}')
			t->flags |= PPARSE_TF_CLOSE;
	} else {
		char *loc = pparse_loc(_pc, t);
		if (t->len == 2) {
			char c2 = loc[1];
			if (c2 == '=' && c != '!' && c != '<' && c != '>' && c != '=') t->tag = PPARSE_TT_ASSIGN;
			else if (c == '+' && c2 == '+')
				t->tag = PPARSE_TT_ASSIGN;
			else if (c == '-' && c2 == '-')
				t->tag = PPARSE_TT_ASSIGN;
			else if (c == '-' && c2 == '>')
				t->tag = PPARSE_TT_MEMBER;
		} else if (t->len == 3 && loc[2] == '=' && (c == '<' || c == '>') && loc[1] == c)
			t->tag = PPARSE_TT_ASSIGN;
	}
}
static bool pparse_p0_soft_noreturn_is_decl_specifier(PParseToken *tok) {
	PParseToken *next = pparse_p0_next(tok);
	if (!next || next->kind == PPARSE_TK_EOF ||
	    pparse_match_set(next, pparse_CH('=') | pparse_CH('(') | pparse_CH('[') | pparse_CH(',') | pparse_CH(';') | pparse_CH(')')))
		return false;

	bool followed_by_decl =
	    (next->tag & (PPARSE_TT_TYPE | PPARSE_TT_STORAGE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_ATTR |
			  PPARSE_TT_INLINE)) ||
	    (next->flags & PPARSE_TF_C23_ATTR);
	if (!followed_by_decl && pparse_p0_token_can_name_function(next) && pparse_p0_next(next) &&
	    pparse_p0_next(next)->ch0 == '(')
		followed_by_decl = true;
	if (!followed_by_decl && next->kind == PPARSE_TK_IDENT) {
		PParseToken *name = pparse_p0_next(next);
		if (pparse_p0_token_can_name_function(name) && pparse_p0_next(name) && pparse_p0_next(name)->ch0 == '(')
			followed_by_decl = true;
	}
	if (!followed_by_decl) return false;

	if (pparse_at_bol(tok)) return true;
	PParseToken *prev = pparse_p0_previous_token(tok);
	if (!prev) return true;
	if (prev->ch0 == ';' || prev->ch0 == '{' || prev->ch0 == '}') return true;
	if (prev->tag &
	    (PPARSE_TT_TYPE | PPARSE_TT_STORAGE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_ATTR | PPARSE_TT_INLINE))
		return true;
	if (prev->ch0 == ']' && (pparse_pair_known(prev)->flags & PPARSE_TF_C23_ATTR)) return true;
	return false;
}
static bool pparse_p0_attribute_inside_parameter_list(PParseToken *attr) {
	PPARSE_CTX();
	for (PParseToken *t = pparse_enclosing_open(_pc, attr, true); t; t = pparse_enclosing_open(_pc, t, true))
		if (pparse_match_ch(t, '(') && pparse_p0_token_can_name_function(pparse_p0_previous_token(t)))
			return true;
	return false;
}
static PParseToken *pparse_find_wrapper_callee(PParseToken *body) {
	PParseToken *end = pparse_pair_known(body);
	PParseToken *tok = pparse_p0_next(body);
	while (tok && tok != end && tok->ch0 == ';') tok = pparse_p0_next(tok);
	if (tok && tok != end && (tok->tag & PPARSE_TT_RETURN)) tok = pparse_p0_next(tok);
	while (tok && tok != end && tok->ch0 == ';') tok = pparse_p0_next(tok);
	if (!tok || tok == end || !pparse_p0_token_can_name_function(tok)) return NULL;
	PParseToken *open = pparse_p0_next(tok);
	if (open->ch0 != '(') return NULL;
	PParseToken *after = pparse_p0_next(pparse_pair_known(open));
	while (after != end && after->ch0 == ';') after = pparse_p0_next(after);
	return after == end ? tok : NULL;
}
// Scan line directive; returns position after it, or NULL if not a line marker.
// Accepts `#`, digraph `%:`, and trigraph `??=` as the directive introducer.
static char *pparse_scan_line_directive(char *p, int *line_no, bool *in_system_include) {
	PPARSE_CTX();
	int directive_line = *line_no;
	if (p[0] == '%' && p[1] == ':')
		p += 2;
	else if (p[0] == '?' && p[1] == '?' && p[2] == '=')
		p += 3;
	else if (*p == '#')
		p++;
	else
		return NULL;
	while (pparse_is_hspace(*p)) p++;
	if (!strncmp(p, "line", 4) && pparse_is_hspace(p[4])) {
		p += 4;
		while (pparse_is_hspace(*p)) p++;
	}

	if (!PPARSE_IS_DIGIT(*p)) return NULL;
	unsigned long new_line = 0;
	while (PPARSE_IS_DIGIT(*p)) {
		unsigned int digit = *p - '0';
		if (new_line > (ULONG_MAX - digit) / 10) return NULL;
		new_line = new_line * 10 + digit;
		p++;
	}
	while (pparse_is_hspace(*p)) p++;
	char *filename = NULL;
	if (*p == '"') {
		p++;
		char *start = p;
		while (*p && *p != '"') {
			if (*p == '\\' && p[1]) p++;
			p++;
		}
		int raw_len = p - start;
		filename = pparse_arena_alloc_uninit(&_pc->main_arena, (size_t)raw_len + 1);
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
	while (pparse_is_hspace(*p)) p++;
	while (PPARSE_IS_DIGIT(*p)) {
		int flag = 0;
		while (PPARSE_IS_DIGIT(*p)) {
			if (flag > INT_MAX / 10) {
				while (PPARSE_IS_DIGIT(*p)) p++; // skip remaining digits
				break;
			}
			flag = flag * 10 + (*p - '0');
			p++;
		}
		if (flag == 1) is_entering = true;
		if (flag == 2) is_returning = true;
		if (flag == 3) is_system = true;
		while (pparse_is_hspace(*p)) p++;
	}
	bool filename_is_system = false;
	if (filename) {
		const char *f = filename;
		filename_is_system =
		    strncmp(f, "/usr/include/", 13) == 0 || strncmp(f, "/usr/local/include/", 19) == 0 ||
		    strncmp(f, "/Library/", 9) == 0 || strncmp(f, "/Applications/Xcode", 19) == 0 ||
		    (strstr(f, "/lib/gcc/") && strstr(f, "/include/")) ||
		    (strstr(f, "/lib/clang/") && strstr(f, "/include")) || strstr(f, "Windows Kits") ||
		    strstr(f, "Program Files");
	}

	/* Direct system include = first entry into a system file from user code.
	 * Nested system headers (bits/…) must not be re-emitted as #include.
	 * Flag `1 3` on a non-system-looking path must not sticky-skip the TU. */
	if (is_entering && is_system && filename && !filename_is_system) is_system = false;
	bool direct_system = is_entering && is_system && !*in_system_include;
	if (is_entering && is_system) *in_system_include = true;
	else if (is_returning && !is_system)
		*in_system_include = false;
	if (new_line > (unsigned long)INT_MAX) {
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
	PParseFile *view;
	if (msvc_style && filename) {
		const char *f = filename;
		if (filename_is_system) {
			direct_system = !*in_system_include;
			is_system = true;
			*in_system_include = true;
		} else if (*f == '/' || *f == '.' || (f[0] && f[1] == ':')) {
			is_system = false;
			*in_system_include = false;
		}
	}
	/* `# N "user.c" 2 3` — return with a spurious system flag on a non-system
	 * path must not sticky-skip the rest of the TU (PPARSE_TF_SYS_SKIP). */
	if (is_returning && is_system && filename) {
		if (!filename_is_system) {
			is_system = false;
			*in_system_include = false;
		}
	}
	view = pparse_add_input_file((PParseFile){.name = filename ? filename : _pc->current_file->name,
				 .line_delta = line_delta,
				 .is_system = is_system,
				 .is_direct_system_include = direct_system,
				 .skip_emit = is_system && *in_system_include});
	_pc->current_file = view;
	while (*p && *p != '\n') p++;
	if (*p == '\n') {
		p++;
		(*line_no)++;
	}
	return p;
}
static char *pparse_scan_pp_number(char *p) {
	for (;;) {
		char c = *p;
		if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') && (p[1] == '+' || p[1] == '-')) {
			p += 2;
		} else if (c == '.') {
			p++;
		} else if (pparse_ident_char[(unsigned char)c]) {
			p++;
		} else if (c == '\'' && pparse_ident_char[(unsigned char)p[1]]) {
			p++;
		} else
			break;
	}
	return p;
}

/* One byte-class load rejects punctuation; bits retain the interesting reason. */
enum {
	PCC_SPACE = 1 << 0,   /* space \t \f \r \v                */
	PCC_ALPHA = 1 << 1,   /* PPARSE_IS_ALPHA, plus >= 0x80    */
	PCC_DIGIT = 1 << 2,   /* 0-9                              */
	PCC_LITPFX = 1 << 3,  /* u U L R - may open a literal     */
	PCC_QUOTE = 1 << 4,   /* " ', plus \\ for UCN dispatch     */
	PCC_SLASH = 1 << 5,   /* / - may open a comment           */
	PCC_DOT = 1 << 6,     /* . - may open a pp-number         */
	PCC_NEWLINE = 1 << 7, /* \n                               */
};

static inline PRISM_ALWAYS_INLINE void
pparse_push_u32(PParseContext *_pc, uint32_t **v, int *n, int *cap, uint32_t x) {
	PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena, *v, *n + 1, *cap, 16, uint32_t);
	(*v)[(*n)++] = x;
}

static const uint8_t pparse_char_class[256] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01, 0x01, 0x01, 0x00, 0x00,  /* 0x00 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 0x10 */
	0x01, 0x00, 0x10, 0x00, 0x02, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x20,  /* 0x20 */
	0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 0x30 */
	0x00, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0A, 0x02, 0x02, 0x02,  /* 0x40 */
	0x02, 0x02, 0x0A, 0x02, 0x02, 0x0A, 0x02, 0x02, 0x02, 0x02, 0x02, 0x00, 0x10, 0x00, 0x00, 0x02,  /* 0x50 */
	0x00, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,  /* 0x60 */
	0x02, 0x02, 0x02, 0x02, 0x02, 0x0A, 0x02, 0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 0x70 */
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,  /* 0x80 */
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,  /* 0x90 */
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,  /* 0xA0 */
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,  /* 0xB0 */
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,  /* 0xC0 */
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,  /* 0xD0 */
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,  /* 0xE0 */
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,  /* 0xF0 */
};

static PParseToken *pparse_tokenize(PParseFile *file, char *contents, size_t contents_len) {
	PPARSE_CTX();
	_pc->current_file = file;
	_pc->token_source = contents;
	if (contents_len > UINT32_MAX)
		pparse_error_at(contents, "source file exceeds 4 GiB; cannot record token locations");
	char *p = contents;
	/* Skip leading UTF-8 BOM (EF BB BF). UTF-16 BOMs are rejected earlier. */
	if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
		p += 3;
	pparse_token_pool_ensure(pparse_token_count + contents_len / 2 + 4096);
	uint32_t first_idx = pparse_token_count;
	uint32_t tag_summary = 0;
	PParseTokState ts = {true, false, 1};
	unsigned kw_shadow_mask = 0;
	bool in_system_include = false;
	int delimiter_cap = 64, delimiter_n = 0, builtin_n = 0, builtin_cap = 0;
	int noreturn_n = 0, noreturn_cap = 0;
	uint32_t *delimiter_stack =
	    pparse_arena_alloc_uninit(&_pc->main_arena, delimiter_cap * sizeof(*delimiter_stack));
	uint32_t *builtin_candidates = NULL;
	uint32_t *noreturn_candidates = NULL;
	uint32_t last_prism_idx = 0;
	while (*p) {
		int ident_len;
		char *ident_end;
		if (ts.at_bol &&
		    (*p == '#' || (p[0] == '%' && p[1] == ':') ||
		     (p[0] == '?' && p[1] == '?' && p[2] == '='))) {
			char *directive_start = p;
			kw_shadow_mask = pparse_update_kw_shadow(p, kw_shadow_mask);
			char *after = pparse_scan_line_directive(p, &ts.line_no, &in_system_include);
			if (after) {
				p = after;
				ts.at_bol = true;
				ts.has_space = false;
				continue;
			}
			while (*p && *p != '\n') p++;
			pparse_new_token(PPARSE_TK_PREP_DIR, directive_start, p, &ts)->flags |= PPARSE_TF_AT_BOL;
			if (*p == '\n') {
				p++;
				ts.line_no++;
				ts.at_bol = true;
				ts.has_space = false;
			}
			continue;
		}

		/* Class 0 means this byte cannot begin a comment, a run of whitespace,
		 * a number, a literal prefix or a quote, so none of the tests below can
		 * match it. Skip straight to punctuation. The '#'/'%:'/'??=' directive
		 * forms are handled above, before this dispatch, so they are unaffected. */
		unsigned cc0 = pparse_char_class[(unsigned char)*p];
		if (__builtin_expect(cc0 == 0, 1)) goto do_punct;

		switch (cc0) {
		case PCC_SLASH:
			if (p[1] == '/') {
				p = pparse_skip_line_comment(p + 2, &ts);
				ts.has_space = true;
				continue;
			}
			if (p[1] == '*') {
				p = pparse_skip_block_comment(p + 2, &ts);
				ts.has_space = true;
				continue;
			}
			goto do_punct;
		/* Whitespace is the largest single class of loop iterations on real
		 * preprocessed input: 95,924 of 288,375 on a 1.1 MB .i, 319 KB of the
		 * bytes, averaging 3.3 bytes a run. The old body wrote all three state
		 * fields on every byte of the run and re-derived "is this a space"
		 * from a five-way comparison chain, when the class table consulted one
		 * line above already answers it. Only the last byte of the run decides
		 * the flags -- `at_bol` latches on any newline and is never cleared by
		 * a following space -- so the writes hoist out and only the newline
		 * count stays per byte. Worth 1.05x on the whole transpile, 20 of 22
		 * interleaved pairs. */
		case PCC_SPACE:
		case PCC_NEWLINE: {
			int newlines = 0;
			do {
				newlines += (*p == '\n');
				p++;
			} while (pparse_char_class[(unsigned char)*p] & (PCC_SPACE | PCC_NEWLINE));
			ts.line_no += newlines;
			ts.at_bol |= newlines != 0;
			ts.has_space = (p[-1] != '\n');
			continue;
		}
		/* Fast path: the vast majority of tokens are identifiers/keywords that
		 * do NOT start with a string/char literal prefix (u/U/L/R). Jump
		 * straight to identifier scanning, skipping ~15 literal-prefix branches.
		 * u/U/L/R starts fall through so `u8"..."`, `L'x'`, `R"..."` still work. */
		case PCC_ALPHA: goto do_ident_fast;
		case PCC_DIGIT: goto do_number;
		case PCC_DOT:
			if (pparse_char_class[(unsigned char)p[1]] & PCC_DIGIT) goto do_number;
			goto do_punct;
		case PCC_ALPHA | PCC_LITPFX:
		case PCC_QUOTE: goto do_literal;
		default: goto do_punct;
		}
	do_number: {
			char *start = p;
			p = pparse_scan_pp_number(p);
			pparse_new_token(PPARSE_TK_NUM, start, p, &ts);
			continue;
		}
		/* String, character and raw-string literals differ only in the
		 * encoding prefix that may precede the quote. Six near-identical
		 * branches used to spell out every combination of {"", u8, u, U, L} x
		 * {R, ", '}; one prefix length drives all of them. */
	do_literal: {
			int pfx = (((cc0 & PCC_LITPFX) != 0) & (*p != 'R')) +
				  ((*p == 'u') & (p[1] == '8'));
			char q = p[pfx];
			if (q == 'R' && p[pfx + 1] == '"') {
				PParseToken *nt = pparse_read_raw_string_literal(p, p + pfx + 1, &ts);
				p += nt->len;
				continue;
			}
			if (q == '"' || q == '\'') {
				PParseToken *nt = q == '"' ? pparse_read_string_literal(p, p + pfx, &ts)
							   : pparse_read_char_literal(p, p + pfx, &ts);
				p += nt->len;
				continue;
			}
		}
		if (cc0 & PCC_ALPHA) goto do_ident_fast;
		if (*p != '\\') goto do_punct;
		ident_len = pparse_read_ucn(p);
		if (!ident_len) goto do_punct;
		ident_end = p + ident_len;
		goto scan_ident_tail;
	do_ident_fast:
		ident_end = p + 1;
	scan_ident_tail:
		for (;;) {
			while (pparse_ident_char[(unsigned char)*ident_end]) ident_end++;
			int ucn = pparse_read_ucn(ident_end);
			if (!ucn) break;
			ident_end += ucn;
		}
		ident_len = (int)(ident_end - p);
		{
			PParseToken *t = pparse_new_token(PPARSE_TK_IDENT, p, p + ident_len, &ts);
			t->parse_data = pparse_fast_hash(p, (uint32_t)ident_len);
			uint64_t kw = pparse_keyword_lookup(_pc, p, ident_len);
			kw *= (kw_shadow_mask & (unsigned)(kw >> PPARSE_KW_SHADOW_SHIFT)) == 0;
			if (kw) {
				bool marked_keyword = (kw & PPARSE_KW_MARKER) != 0;
				t->kind = (PParseTokenKind)(marked_keyword * PPARSE_TK_KEYWORD);
				t->tag = (uint32_t)(kw & ~PPARSE_KW_MARKER);
				t->flags |= (uint16_t)(kw >> PPARSE_KW_FLAGS_SHIFT);
				tag_summary |= t->tag;
				uint32_t ti = pparse_token_count - 1;
				if (t->tag & (PPARSE_TT_DEFER | PPARSE_TT_ORELSE)) last_prism_idx = ti;
				if (delimiter_n == 0 && (t->tag & (PPARSE_TT_NORETURN_FN | PPARSE_TT_SPECIAL_FN))) {
					pparse_push_u32(_pc, &builtin_candidates, &builtin_n, &builtin_cap, ti);
				}
				bool nr_spec = ((t->tag & (PPARSE_TT_INLINE | PPARSE_TT_SKIP_DECL)) ==
					       (PPARSE_TT_INLINE | PPARSE_TT_SKIP_DECL) &&
					       (pparse_equal(t, "_Noreturn") || pparse_equal(t, "noreturn")));
				if ((t->tag & PPARSE_TT_ATTR) || nr_spec) {
					pparse_push_u32(_pc, &noreturn_candidates, &noreturn_n, &noreturn_cap, ti);
				}
			}
			p += ident_len;
			continue;
		}
	do_punct:;
		int punct_len = pparse_read_punct(p);
		if (punct_len) {
			int abs_len = abs(punct_len);
			PParseToken *t = pparse_new_token(PPARSE_TK_PUNCT, p, p + abs_len, &ts);
			if (punct_len < 0) {
				char norm;
				switch (abs_len == 4 ? '%' : p[0]) {
				case '<': norm = p[1] == ':' ? '[' : '{'; break;
				case ':': norm = ']'; break;
				case '%': norm = abs_len == 4 || p[1] != '>' ? '#' : '}'; break;
				default: norm = *p; break;
				}
				p[0] = norm;
				if (abs_len == 4) p[1] = '#'; // %:%: -> ##
				t->len = (uint8_t)(1 + (abs_len == 4));
				t->ch0 = (uint8_t)norm;
			}
			pparse_classify_punct(t);
			/* Array-bracket grammar context is cached in parse_data after
			 * tokenization. The pool survives library-mode resets, so clear
			 * this one reused punctuation word at creation. */
			if (t->ch0 == '[') t->parse_data = 0;
			uint32_t ti = pparse_token_count - 1;
			uint16_t delimiter_flags = t->flags & (PPARSE_TF_OPEN | PPARSE_TF_CLOSE);
			if (delimiter_flags) {
				if (delimiter_flags & PPARSE_TF_OPEN) {
					pparse_push_u32(_pc, &delimiter_stack, &delimiter_n, &delimiter_cap, ti);
					if (t->ch0 == '{') {
						PParseToken *prev =
						    pparse_walk_back(ti, PPARSE_WB_ATTR_NOISE | PPARSE_WB_JUMP_ATTR_PARENS);
						if (prev && pparse_match_ch(prev, '(')) prev->flags |= PPARSE_TF_STMT_EXPR;
					}
					if (t->ch0 == '[' && ti > first_idx) {
						PParseToken *prev = &pparse_token_pool[ti - 1];
						if (prev->ch0 == '[' && (prev->flags & PPARSE_TF_OPEN)) {
							prev->flags |= PPARSE_TF_C23_ATTR;
							pparse_push_u32(_pc, &noreturn_candidates, &noreturn_n,
									 &noreturn_cap, ti - 1);
						}
					}
				} else {
					if (delimiter_n == 0) pparse_error_tok(t, "unmatched closing delimiter");
					uint32_t open_idx = delimiter_stack[--delimiter_n];
					PParseToken *open = &pparse_token_pool[open_idx];
					unsigned expected_close = open->ch0 + 2 - (open->ch0 == '(');
					if (t->ch0 != expected_close)
						pparse_error_tok(t,
								  "mismatched closing delimiter '%c' for opener '%c'",
								  t->ch0,
								  open->ch0);
					open->pair_idx = ti;
					t->pair_idx = open_idx;
					if (open > pparse_token_pool + first_idx && ((open - 1)->tag & PPARSE_TT_ATTR))
						for (PParseToken *u = open; u <= t; u++) u->ann |= P1_IN_ATTR_ARGS;
					if (last_prism_idx > t->pair_idx) open->flags |= PPARSE_TF_HAS_PRISM;
				}
			}
			p += abs_len;
			continue;
		}
		pparse_error_at(p, "invalid token");
	}

	/* `lex_only` callers want the token *spellings* out of text that has not
	 * been through cc -E yet, where a macro body may legitimately open a
	 * delimiter it never closes. Pairing is meaningless for them, so an
	 * unclosed delimiter is not an error; every consumer of pair_idx runs
	 * only on preprocessed input. */
	if (delimiter_n > 0 && !_pc->lex_only) {
		PParseToken *open = &pparse_token_pool[delimiter_stack[delimiter_n - 1]];
		pparse_error_tok(open, "unclosed delimiter '%c'", open->ch0);
	}
	pparse_new_token(PPARSE_TK_EOF, p, p, &ts);

	PParseToken *first = &pparse_token_pool[first_idx];
	pparse_token_tag_summary = tag_summary;
	if (_pc->lex_only) return first;
	{
		PParseHashMap user_builtin = {0};
		for (int i = 0; i < builtin_n; i++) {
			PParseToken *name = &pparse_token_pool[builtin_candidates[i]];
			PParseToken *open = pparse_p0_next(name);
			if (!pparse_match_ch(open, '(')) continue;
			PParseToken *after = pparse_p0_next(pparse_pair_known(open));
			while (after->kind != PPARSE_TK_EOF) {
				PParseToken *ae = pparse_p0_attribute_group_end(after);
				if (ae) {
					after = pparse_p0_next(ae);
					continue;
				}
				if (after->kind == PPARSE_TK_PREP_DIR) {
					after = pparse_p0_next(after);
					continue;
				}
				break;
			}
			if (pparse_match_ch(after, '{'))
				pparse_lex_token_map_put(&user_builtin, name, (void *)1);
		}
		/* User definitions of libc noreturn/special names are ordinary unless
		 * explicitly annotated (_Noreturn / attr). Clear keyword-cache tags so
		 * taint + auto-unreachable do not treat `int exit(void){…}` as builtin. */
		if (user_builtin.used > 0) {
			for (PParseToken *s = first; s->kind != PPARSE_TK_EOF; s = pparse_p0_next(s)) {
				if ((s->tag & (PPARSE_TT_NORETURN_FN | PPARSE_TT_SPECIAL_FN)) &&
				    pparse_lex_token_map_get(&user_builtin, s))
					s->tag &= ~(PPARSE_TT_NORETURN_FN | PPARSE_TT_SPECIAL_FN);
			}
		}

		// Pre-scan function bodies: tag '{' with PPARSE_TT_SPECIAL_FN / PPARSE_TT_ASM / PPARSE_TT_NORETURN_FN(=vfork).
		// Propagate special-function taint transitively through wrapper chains.
		if (tag_summary &
		    (PPARSE_TT_SPECIAL_FN | PPARSE_TT_NORETURN_FN | PPARSE_TT_ASM)) {
			typedef struct {
				PParseToken *name;
				PParseToken *body;
			} PParseFunctionScan;

			PParseFunctionScan *functions = NULL;
			int function_count = 0;
			int function_capacity = 0;
			PParseToken *func_name = NULL;
			for (PParseToken *t = first; t->kind != PPARSE_TK_EOF; t = pparse_p0_next(t)) {
				PParseToken *attr_end = pparse_p0_attribute_group_end(t);
				if (attr_end) {
					t = attr_end;
					continue;
				}
				PParseToken *next = pparse_p0_next(t);
				if (t->kind <= PPARSE_TK_KEYWORD && next->ch0 == '(' &&
				    !(t->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE |
					       PPARSE_TT_TYPEOF | PPARSE_TT_ATTR)))
					func_name = t;
				if (t->ch0 == '{' && (t->flags & PPARSE_TF_OPEN)) {
					PParseToken *end = pparse_pair_known(t);
					for (PParseToken *b = pparse_p0_next(t); b != end; b = pparse_p0_next(b)) {
						if ((b->tag & PPARSE_TT_SPECIAL_FN) &&
						    !((b - 1)->tag & PPARSE_TT_MEMBER)) {
							/* Skip declarator shadows: `void (*setjmp)(void)` /
							 * `int vfork;` — not calls/refs. */
							PParseToken *prev = b - 1;
							PParseToken *nxt = pparse_p0_next(b);
							bool decl_shadow =
							    (pparse_match_ch(prev, '*') ||
							     (prev->tag &
							      (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE |
							       PPARSE_TT_SUE))) &&
							    nxt->ch0 != '(';
							if (!decl_shadow) {
								if (b->ch0 == 'v' && b->len == 5) {
									t->tag |= PPARSE_TT_NORETURN_FN;
								} else
									t->tag |= PPARSE_TT_SPECIAL_FN;
							}
						}
						if (b->tag & PPARSE_TT_ASM) {
							/* GNU `asm [qualifiers] goto (` — require
							 * `goto` before the asm operand `(`, and
							 * only allow asm-header tokens in between.
							 * Stop on any other token so soft-kw
							 * identifier uses like `if (asm) goto L`
							 * are not mistaken for asm-goto. */
							bool saw_goto = false;
							for (PParseToken *ag = pparse_p0_next(b);
							     ag != end;
							     ag = pparse_p0_next(ag)) {
								if (ag->ch0 == '(') {
									if (saw_goto) t->tag |= PPARSE_TT_ASM;
									break;
								}
								if (ag->tag & PPARSE_TT_GOTO) {
									saw_goto = true;
									continue;
								}
								/* asm volatile/inline/goto attrs */
								if (ag->tag &
								    (PPARSE_TT_QUALIFIER | PPARSE_TT_INLINE | PPARSE_TT_ATTR))
									continue;
								if (pparse_equal(ag, "__volatile__") ||
								    pparse_equal(ag, "__inline__") ||
								    pparse_equal(ag, "volatile") ||
								    pparse_equal(ag, "inline"))
									continue;
								if (ag->flags & PPARSE_TF_SOFT_KW) continue;
								if (ag->flags & PPARSE_TF_C23_ATTR) {
									ag = pparse_pair_known(ag);
									continue;
								}
								break;
							}
						}
					}
					if (func_name) {
						PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena,
								 functions,
								 function_count + 1,
								 function_capacity,
								 32,
								 PParseFunctionScan);
						functions[function_count++] =
						    (PParseFunctionScan){.name = func_name, .body = t};
					}
					func_name = NULL;
					t = end;
				}
			}

			uint32_t *wrapper_taint = NULL;
			int *callee_idx = NULL;
			PParseHashMap func_map = {0};
			if (function_count > 0) {
				wrapper_taint = pparse_arena_alloc(&_pc->main_arena,
							    (size_t)function_count * sizeof(*wrapper_taint));
				callee_idx = pparse_arena_alloc(&_pc->main_arena,
							 (size_t)function_count * sizeof(*callee_idx));
				for (int i = 0; i < function_count; i++) {
					pparse_lex_token_map_put(
					    &func_map, functions[i].name, (void *)(intptr_t)(i + 1));
				}
				for (int i = 0; i < function_count; i++) {
					callee_idx[i] = -1;
					PParseToken *callee = pparse_find_wrapper_callee(functions[i].body);
					if (!callee) continue;
					// PPARSE_TT_SPECIAL_FN in the body taints with PPARSE_TT_SPECIAL_FN
					// Pass 2 still emits a per-call-site warning at the
					if (callee->tag & PPARSE_TT_SPECIAL_FN) {
						wrapper_taint[i] =
						    (callee->ch0 == 'v' && callee->len == 5)
							? PPARSE_TT_NORETURN_FN // vfork wrapper
							: PPARSE_TT_SPECIAL_FN; // setjmp/longjmp/pthread_exit wrapper
						continue;
					}
					void *v = pparse_lex_token_map_get(&func_map, callee);
					if (v) callee_idx[i] = (int)(intptr_t)v - 1;
				}

				/* Taint flows callee → caller along the wrapper chain. */
				int *caller_head = pparse_arena_alloc(&_pc->main_arena,
							       (size_t)function_count * sizeof(int));
				int *caller_next = pparse_arena_alloc(&_pc->main_arena,
							       (size_t)function_count * sizeof(int));
				int *queue = pparse_arena_alloc(&_pc->main_arena,
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
				    (functions[i].body->tag & (PPARSE_TT_SPECIAL_FN | PPARSE_TT_NORETURN_FN))) {
					has_taint = true;
					break;
				}
			}
			if (has_taint) {
				typedef struct {
					int from, to;
				} PParseTaintEdge;

				PParseTaintEdge *edges = NULL;
				int edge_count = 0, edge_cap = 0;
				uint64_t fn_bloom = 0;
				for (int i = 0; i < function_count; i++) {
					PParseToken *n = functions[i].name;
					fn_bloom |= 1ULL << (((unsigned)n->ch0 ^ n->len) & 63);
				}
				for (int i = 0; i < function_count; i++) {
					PParseToken *body = functions[i].body;
					if (body->tag & (PPARSE_TT_SPECIAL_FN | PPARSE_TT_NORETURN_FN)) continue;
					PParseToken *end = pparse_pair_known(body);
					for (PParseToken *b = pparse_p0_next(body); b != end; b = pparse_p0_next(b)) {
						if (!pparse_p0_token_can_name_function(b)) continue;
						/* Skip declarator occurrences (`void (*f0)(void)`,
						 * `int f0;`) — keep bare refs so FP chains like
						 * `fp = f0; fp();` still propagate taint. */
						PParseToken *prev = b - 1;
						PParseToken *n = pparse_p0_next(b);
						if ((pparse_match_ch(prev, '*') ||
						     (prev->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER |
								   PPARSE_TT_STORAGE | PPARSE_TT_SUE))) &&
						    n->ch0 != '(')
							continue;
						if (!(fn_bloom &
						      (1ULL << (((unsigned)b->ch0 ^ b->len) & 63))))
							continue;
						if (b > body + 1) {
							prev = b - 1;
							if (prev->tag &
							    (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_SUE))
								continue;
							/* `(void)callee()` is still a call edge, but a casted
							 * bare identifier is commonly a local-use idiom. Treating
							 * `(void)local;` as a reference to an equally named file-scope
							 * function falsely taints the containing function. */
							if (pparse_match_ch(prev, ')') && (prev->flags & PPARSE_TF_CLOSE)) {
								PParseToken *open = pparse_pair_known(prev);
								PParseToken *inner = pparse_p0_next(open);
								PParseToken *next = pparse_p0_next(b);
								if ((inner->tag &
								     (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE)) &&
								    !pparse_match_ch(next, '('))
									continue;
							}
						}
						void *v = pparse_lex_token_map_get(&func_map, b);
						if (!v) continue;
						int j = (int)(intptr_t)v - 1;
						PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena, edges, edge_count + 1,
									 edge_cap, 64, PParseTaintEdge);
						edges[edge_count++] = (PParseTaintEdge){i, j};
					}
				}
				/* Body taint: call edge i→j propagates j's taint onto i. */
				if (edge_count > 0) {
					int *edge_head = pparse_arena_alloc(&_pc->main_arena,
								     (size_t)function_count * sizeof(int));
					int *edge_next = pparse_arena_alloc(&_pc->main_arena,
								     (size_t)edge_count * sizeof(int));
					int *queue = pparse_arena_alloc(&_pc->main_arena,
								 (size_t)function_count * sizeof(int));
					uint8_t *queued = pparse_arena_alloc(&_pc->main_arena,
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
						    (functions[j].body->tag & (PPARSE_TT_NORETURN_FN | PPARSE_TT_SPECIAL_FN))) {
							queue[qt++] = j;
							queued[j] = 1;
						}
					}
					while (qh < qt) {
						int j = queue[qh++];
						for (int e = edge_head[j]; e >= 0; e = edge_next[e]) {
							int i = edges[e].from;
							PParseToken *body = functions[i].body;
							if (body->tag & (PPARSE_TT_SPECIAL_FN | PPARSE_TT_NORETURN_FN))
								continue;
							uint32_t before = body->tag;
							if (wrapper_taint[j]) body->tag |= wrapper_taint[j];
							if (functions[j].body->tag & PPARSE_TT_NORETURN_FN)
								body->tag |= PPARSE_TT_NORETURN_FN;
							/* SPECIAL_FN propagates transitively like NORETURN_FN just
							 * above, but only within the same original source file.
							 * Unlike vfork, setjmp/longjmp are how PRISM_LIB_MODE itself
							 * recovers from errors (prism_transpile_source_into calls
							 * setjmp; pparse_lib_error_jump calls longjmp), so an
							 * unguarded propagation taints every caller of the library
							 * entry points transitively -- nearly every test in the
							 * suite -- whenever prism transpiles a TU that #includes
							 * prism.c, which is exactly what the self-hosted suite
							 * build does. Restricting the relay to same-file edges
							 * still closes the real gap: a multi-statement helper that
							 * calls setjmp/longjmp directly, in the same file as its
							 * caller, now taints that caller too instead of only the
							 * one function whose body literally names the call.
							 *
							 * "Same file" is a name compare, not a file_idx compare:
							 * file_idx mints a fresh PParseFile on every system/line-
							 * marker flag toggle emitted by cc -E, including ones a
							 * glibc/macOS macro produces mid-statement (`setjmp(buf)`
							 * itself expands through a `# N "file.c" 3 4` bracket), so
							 * two functions written back-to-back in one file can carry
							 * different file_idx values with an identical name. */
							if ((functions[j].body->tag & PPARSE_TT_SPECIAL_FN) &&
							    !strcmp(pparse_tok_file(functions[j].body)->name,
								    pparse_tok_file(body)->name))
								body->tag |= PPARSE_TT_SPECIAL_FN;
							if (body->tag == before) continue;
							if (!queued[i]) {
								queue[qt++] = i;
								queued[i] = 1;
							}
						}
					}
				}
			} // has_taint
		}
	}

	// When a noreturn specifier is found before a function declaration,
		if (noreturn_n) {
			PParseHashMap nr_map = {0};
#define pparse_SKIP_ATTR_ARGS(a)                                                                                    \
	do {                                                                                                 \
		if ((a)->kind <= PPARSE_TK_KEYWORD && pparse_p0_next(a)->ch0 == '(')                              \
			(a) = &pparse_token_pool[pparse_p0_next(a)->pair_idx];                                            \
	} while (0)
#define PPARSE_IS_NORETURN_NAME(a)                                                                                  \
	((a)->kind <= PPARSE_TK_KEYWORD &&                                                                          \
	 (pparse_equal((a), "noreturn") || pparse_equal((a), "_Noreturn") || pparse_equal((a), "__noreturn__")))
#define pparse_ATTR_SPAN_HAS_NORETURN(start, end, out)                                                              \
	do {                                                                                                 \
		for (PParseToken *_a = (start); _a < (end); _a = pparse_p0_next(_a)) {                              \
			if (PPARSE_IS_NORETURN_NAME(_a)) {                                                          \
				(out) = true;                                                                \
				break;                                                                       \
			}                                                                                    \
			pparse_SKIP_ATTR_ARGS(_a);                                                                  \
		}                                                                                            \
	} while (0)
			for (int ni = 0; ni < noreturn_n; ni++) {
				PParseToken *t = &pparse_token_pool[noreturn_candidates[ni]];
			bool is_noreturn = false;
			bool attribute_form = false;
			PParseToken *scan_start = t;
			PParseToken *attr_origin = t; // original position for backward scan

			if (t->kind <= PPARSE_TK_KEYWORD &&
			    (pparse_equal(t, "_Noreturn") ||
			     (pparse_equal(t, "noreturn") && pparse_p0_soft_noreturn_is_decl_specifier(t))))
				is_noreturn = true;
			// [[noreturn]] / [[_Noreturn]] / [[__noreturn__]] — C23 attribute
			if (t->ch0 == '[' && (t->flags & PPARSE_TF_C23_ATTR)) {
				attribute_form = true;
				PParseToken *inner = pparse_p0_next(t);
				PParseToken *attr_end = &pparse_token_pool[t->pair_idx];
				if (inner->ch0 == '[')
					pparse_ATTR_SPAN_HAS_NORETURN(pparse_p0_next(inner), attr_end, is_noreturn);
				t = attr_end; // advance past [[ ... ]]
				scan_start = t;
			}

			if (t->kind <= PPARSE_TK_KEYWORD &&
			    (pparse_equal(t, "__attribute__") || pparse_equal(t, "__attribute"))) {
				attribute_form = true;
				PParseToken *p1 = pparse_p0_next(t);
				if (p1->ch0 == '(') {
					PParseToken *p2 = pparse_p0_next(p1);
					if (p2->ch0 == '(') {
						PParseToken *close = &pparse_token_pool[p2->pair_idx];
						pparse_ATTR_SPAN_HAS_NORETURN(pparse_p0_next(p2), close, is_noreturn);
						t = pparse_pair_known(p1); // advance past __attribute__(( ... ))
						scan_start = t;
					}
				}
			}

			// __declspec(noreturn) or __declspec(__noreturn__) — MSVC
			if (t->kind <= PPARSE_TK_KEYWORD && pparse_equal(t, "__declspec")) {
				attribute_form = true;
				PParseToken *p1 = pparse_p0_next(t);
				if (p1->ch0 == '(') {
					PParseToken *close = &pparse_token_pool[p1->pair_idx];
					pparse_ATTR_SPAN_HAS_NORETURN(pparse_p0_next(p1), close, is_noreturn);
					t = close; // advance past __declspec( ... )
					scan_start = t;
				}
			}

			if (!is_noreturn) continue;
			if (attribute_form && pparse_p0_attribute_inside_parameter_list(attr_origin))
				continue;
			PParseToken *fn_name = NULL;
			/* Post-declarator attrs (`void die(void) __attribute__((noreturn)), live`)
			 * must bind to the preceding name. Prefer backward when the token
			 * before the attr is `)` so a following declarator is not tagged. */
			bool post_decl_attr = false;
			if (attribute_form) {
				PParseToken *before = pparse_walk_back(pparse_idx(_pc, attr_origin), PPARSE_WB_ATTR_NOISE);
				post_decl_attr = before && pparse_match_ch(before, ')');
			}
			if (post_decl_attr) {
				fn_name = pparse_p0_attr_owner_backward(_pc, attr_origin);
			}
			if (!fn_name) {
				for (PParseToken *s = scan_start; s->kind != PPARSE_TK_EOF; s = pparse_p0_next(s)) {
					char ch = s->ch0;
					if (ch == ';' || ch == '{') break;
					/* Post-declarator attrs bind to the preceding name only. */
					if (ch == ',' && post_decl_attr) break;
					PParseToken *attr_end = pparse_p0_attribute_group_end(s);
					if (attr_end) {
						s = attr_end;
						continue;
					}
					if (s->flags & PPARSE_TF_OPEN) {
						s = pparse_pair_known(s);
						continue;
					}
					if (pparse_p0_token_can_name_function(s) &&
					    pparse_p0_next(s)->ch0 == '(') {
						if (s->tag & (PPARSE_TT_SKIP_DECL | PPARSE_TT_INLINE | PPARSE_TT_QUALIFIER |
							      PPARSE_TT_TYPE | PPARSE_TT_STORAGE))
							continue;
						if (post_decl_attr) {
							fn_name = s;
							break;
						}
						/* Prefix _Noreturn / [[noreturn]] / leading attr:
						 * tag every declarator in the list. */
						pparse_lex_token_map_put(&nr_map, s, (void *)1);
						fn_name = s;
					}
				}
			}
			if (!fn_name && !post_decl_attr) {
				fn_name = pparse_p0_attr_owner_backward(_pc, attr_origin);
			}
			if (!fn_name) continue;
			if (post_decl_attr)
				pparse_lex_token_map_put(&nr_map, fn_name, (void *)1);
			else if (!pparse_lex_token_map_get(&nr_map, fn_name))
				pparse_lex_token_map_put(&nr_map, fn_name, (void *)1);
		}

		if (nr_map.used > 0) {
			uint64_t nr_bloom = 0;
			for (int i = 0; i < nr_map.capacity; i++) {
				PParseHashEntry *ent = &nr_map.buckets[i];
				if (ent->key && ent->key != PPARSE_TOMBSTONE)
					nr_bloom |= 1ULL
						    << (((unsigned char)ent->key[0] ^ ent->key_len) & 63);
			}
			for (PParseToken *s = first; s->kind != PPARSE_TK_EOF; s = pparse_p0_next(s)) {
				if (pparse_p0_token_can_name_function(s) &&
				    (nr_bloom & (1ULL << (((unsigned)s->ch0 ^ s->len) & 63))) &&
				    !(pparse_token_pool[pparse_idx(_pc, s) - 1].tag & PPARSE_TT_MEMBER) &&
				    pparse_lex_token_map_get(&nr_map, s))
					s->tag |= PPARSE_TT_NORETURN_FN;
			}
		}
#undef pparse_SKIP_ATTR_ARGS
#undef PPARSE_IS_NORETURN_NAME
#undef pparse_ATTR_SPAN_HAS_NORETURN
	}

	return first;
}
static PParseToken *pparse_tokenize_buffer(char *name, char *buf) {
	pparse_ensure_keyword_cache();
	size_t contents_len = strlen(buf);
	PParseFile *file = pparse_add_input_file((PParseFile){.name = pparse_intern_filename(name)});
	return pparse_tokenize(file, buf, contents_len);
}

/* Count dialect keywords in a C text, lexing it the way everything else does.
 * `buf` must be writable with at least 8 NUL bytes of padding, and must stay
 * alive for the call; tokens are appended to the pool and left there for the
 * caller's reset.
 *
 * --prism-verify used to hand-roll this scan in prism.c. That scanner knew
 * about quoted strings, character constants and both comment forms, and
 * nothing about raw strings, so R"(a "b)"
 * left it one quote out of phase and every keyword after such a literal became
 * invisible -- in the one routine whose entire job is to notice a keyword that
 * leaked into the output. It also counted `defer` *inside* R"(say "defer")" as
 * real code. Both directions are gone here: the tokenizer already handles raw
 * strings, prefixes, UCNs and digraphs, and it honours #define shadowing, so a
 * `#define defer` in the emitted text no longer inflates the count. */
static void pparse_count_dialect_keywords(char *name, char *buf, long *n_defer, long *n_orelse) {
	PPARSE_CTX();
	uint32_t first = pparse_token_count;
	/* pparse_tokenize hands _pc->token_source to pparse_tokenizer_teardown to
	 * free. This helper is a borrower, not an owner: restore the pointer so
	 * teardown still frees whatever it owned before and the caller keeps
	 * `buf`. Token `loc` pointers into `buf` are never dereferenced here. */
	char *saved_source = _pc->token_source;
	bool saved_lex_only = _pc->lex_only;
	*n_defer = 0;
	*n_orelse = 0;
	/* The text has not been through cc -E: a macro body can open a delimiter
	 * it never closes, and glibc's headers do exactly that. Only the token
	 * spellings matter here, so lex without demanding balance. */
	_pc->lex_only = true;
	(void)pparse_tokenize_buffer(name, buf);
	_pc->lex_only = saved_lex_only;
	_pc->token_source = saved_source;
	for (uint32_t i = first; i < pparse_token_count; i++) {
		uint32_t tag = pparse_token_pool[i].tag;
		if (tag & PPARSE_TT_DEFER) (*n_defer)++;
		else if (tag & PPARSE_TT_ORELSE) (*n_orelse)++;
	}
}

// Used by both Pass 1 (analysis) and Pass 2 (emission) in prism.c.

typedef struct {
	PParseToken *end;	   // First token after the type specifier
	PParseToken *sue_kw;       // Outermost struct/union/enum keyword, if any
	uint32_t object_type_idx;  // Aggregate S/U or typedef source, index + 1
	bool saw_type : 1; // True if a type was recognized
	bool is_struct : 1;
	bool is_union : 1;
	bool is_enum : 1;
	bool is_typedef : 1;
	bool is_vla : 1;
	bool has_typeof : 1;
	bool has_atomic : 1;
	bool has_long_double : 1;
	bool has_register : 1;
	bool has_volatile : 1;
	bool has_const : 1;
	bool has_decl_const : 1;   // Const that applies to declaration policy
	bool has_hidden_const : 1; // Const inherited through typedef/typeof
	bool has_hidden_volatile : 1;
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
	bool is_ptr : 1; // Pointer type inherited through a typedef
	bool is_func : 1; // Function type inherited through a typedef
	bool array_dim_complete : 1;
	bool type_vm : 1;	 // Any VM dimension in typeof/_Atomic parens (incl. ptr-to-VLA)
	uint8_t type_array_rank; // Dimension count for is_array (multi-dim typeof)
} PParseTypeSpec;

typedef struct {
	PParseToken *end; // First token after declarator
	PParseToken *var_name;
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
	bool is_volatile : 1;
	bool is_atomic : 1;
	bool array_dim_complete : 1;
	bool has_bracket_orelse : 1;
	bool has_zero_dim : 1;
	uint8_t array_rank;
} PParseDecl;

/* Canonical declaration shape, stored verbatim in the Pass-2 recipe. */
enum {
	P1DS_EFF_VLA = 1 << 0,
	P1DS_AGG = 1 << 1,
	P1DS_UNION = 1 << 2,
	P1DS_FUNC = 1 << 3,
};

typedef enum {
	PPARSE_FUNC_RETURN_NONE,
	PPARSE_FUNC_RETURN_VOID,
	PPARSE_FUNC_RETURN_VALUE,
} PParseFunctionReturnKind;

typedef struct {
	PParseToken *type_start;
	PParseToken *type_end;
	PParseToken *suffix_start;
	PParseToken *suffix_end;
	PParseFunctionReturnKind kind;
} PParseFunctionReturn;

typedef enum {
	PPARSE_TDK_TYPEDEF,
	PPARSE_TDK_SHADOW,
	PPARSE_TDK_ENUM_CONST,
	PPARSE_TDK_VLA_VAR,   // VLA variable (not typedef, but actual VLA array variable)
	PPARSE_TDK_STRUCT_TAG // struct/union tag (for VLA/volatile member propagation)
} PParseTypedefKind;

#define PPARSE_ARRAY_RANK_WRAP_ALL 255

enum {
	P1_SCOPE_LOOP = 1 << 1,	  // This '{' opens a loop body
	P1_SCOPE_SWITCH = 1 << 2, // This '{' opens a switch body
	P1_OE_BRACKET = 1 << 4,	  // orelse inside array dimension brackets
	P1_OE_DECL_INIT = 1 << 5, // orelse inside declaration initializer
	P1_IS_DECL = 1 << 6,	  // Phase 1D: token starts a variable declaration
	P1_SCOPE_INIT = 1 << 7,	  // This '{' opens an initializer (compound literal, = {...})
	P1_RAW_BLOCK = 1 << 8,	  // This '{' opens a `raw { ... }` suppress block
	P1_DECL_BRACKET = 1 << 9, // '[' is an array-declarator bracket (not an expression subscript)
	P1_UNEVAL_BRACKET =
	    1 << 10, // '[' is inside an unevaluated operand (sizeof/_Alignof/typeof/offsetof/etc.)
	P1_IS_ORELSE_KW = 1 << 11, // Pass 1: this orelse token is the Prism keyword (not an ident)
	/* Cache for p1_sue_body_brace_zero_unsafe on `{` (ann bits 13–14). */
	P1_ZUNSAFE_KNOWN = 1 << 13, // brace body zero-unsafe result is cached
	P1_ZUNSAFE = 1 << 14,	    // cached result: body rejects `= {0}`
	P1_IS_DEFER_KW = 1 << 15,   // Pass 1: this defer token is the Prism keyword
	/* Local-declaration emit recipe, stored on the name token. */
	P1_DECL_SPLIT = 1 << 16,    // ',' must split the declaration during emission
	P1_DECL_CONST_ORELSE = 1 << 17, // declaration uses the const-value lowering
	P1_OE_RHS_MEMBER = 1 << 18,
	P1_DECL_ZERO_SHIFT = 19,
	P1_DECL_RECIPE = 1u << 21,
	P1_DECL_EXPLICIT_CONST = 1u << 22,
	P1_DECL_EFFECTIVE_CONST = 1u << 23,
	/* Forward-stamped context; keep above bit 23 due to low-bit uint16_t casts. */
	P1_CTX_UNEVAL = 1u << 24,	 // inside sizeof/typeof/_Generic/_Static_assert/_Alignof operand
	P1_CTX_ALIGNOF_TYPE = 1u << 25,	 // inside the type operand of _Alignof / alignof
	P1_CTX_OFFSETOF = 1u << 26,	 // inside offsetof( ... ) / __builtin_offsetof( ... )
	P1_CTX_GENERIC_ASSOC = 1u << 27, // inside a _Generic association type-name
	P1_DECL_BRACKET_OE = 1u << 28,
	P1_DECL_VOLATILE_VALUE = 1u << 29,
	/* Brackets and declaration names are disjoint token kinds, so the top two
	 * bits double as the cached VLA verdict on '[' tokens. */
	P1_VLA_KNOWN = 1u << 30,
	P1_VLA = INT_MIN,
};

/* On aggregate-body braces, the declaration-name-only bits cache a second
 * parser-owned fact pair. Token kinds make the uses disjoint. */
#define P1_AGG_CONST_KNOWN P1_DECL_BRACKET_OE
#define P1_AGG_CONST P1_DECL_VOLATILE_VALUE
#define P1_AGG_BUSY P1_VLA_KNOWN
/* Declaration-name tokens never carry aggregate-body cache state. */
#define P1_DECL_AUTO_STATIC P1_ZUNSAFE_KNOWN
#define P1_DECL_ALREADY_ZERO P1_ZUNSAFE
#define P1_STMT_COLON P1_DECL_RECIPE
#define P1_OE_BARE_RECIPE P1_DECL_SPLIT
#define P1_OE_FALLBACK_CL_COMMA P1_DECL_CONST_ORELSE
#define P1_OE_LHS_INDIRECT (1u << P1_DECL_ZERO_SHIFT)
#define P1_OE_FULL_LHS_INDIRECT (1u << (P1_DECL_ZERO_SHIFT + 1))
#define P1_OE_FALLBACK_CL_SEMI P1_DECL_RECIPE
#define P1_NORETURN_CALL_RECIPE P1_DECL_SPLIT
#define P1_OE_DECL_RECIPE P1_DECL_SPLIT
#define P1_DEFER_SHADOW_NAME P1_OE_RHS_MEMBER
#define P1_GENERIC_DECL_RECIPE P1_DECL_RECIPE
#define P1_GENERIC_DECL_TARGET (P1_DECL_RECIPE | P1_OE_RHS_MEMBER)
#define P1_SUE_BODY_RECIPE P1_DECL_RECIPE
#define P1_RAW_DECL_RECIPE (P1_DECL_SPLIT | P1_OE_RHS_MEMBER)
#define P1_DEFER_BODY_RECIPE P1_DECL_RECIPE
#define P1_SUE_SPLIT_STRIP P1_IS_DECL
/* On type-start tokens, the same kind-disjoint bits cache object traits. */
#define P1_TYPE_OBJ_KNOWN P1_VLA_KNOWN
#define P1_TYPE_OBJ_BUSY P1_VLA
#define P1_TYPE_ZERO_UNSAFE P1_DECL_BRACKET_OE
#define P1_TYPE_CONST_SUBOBJECT P1_DECL_VOLATILE_VALUE

/* Context queries are annotation reads, not token walks. */
#define pparse_token_is_in_unevaluated_operand(tok) ((tok) && ((tok)->ann & P1_CTX_UNEVAL) != 0)

#define pparse_bracket_in_alignof_type_operand(b) (((b)->ann & P1_CTX_ALIGNOF_TYPE) != 0)
#define pparse_bracket_in_offsetof_member(b) (((b)->ann & P1_CTX_OFFSETOF) != 0)
#define pparse_bracket_in_generic_association_type(b) (((b)->ann & P1_CTX_GENERIC_ASSOC) != 0)

#define pparse_ann(t) ((t)->ann)

/* Step over a group after tokenization has validated all delimiter pairs. */
#define PPARSE_SKIP_GROUP_LENIENT(t)                                                                 \
	if ((t)->flags & PPARSE_TF_OPEN) {                                                           \
		(t) = pparse_pair_known(t);                                                          \
		continue;                                                                            \
	}

/* Typedef-lookup accept rule (ISO C11 6.2.3): ordinary identifiers win
 * outright, a struct/union/enum tag is only a fallback if nothing else covers
 * this position. Used by all three of pparse_typedef_lookup's chain walks. */
#define PPARSE_ACCEPT_NONTAG_ELSE_REMEMBER                                                           \
	if (PPARSE_ENTRY_COVERS(e, cur)) {                                                           \
		if (!e->is_struct_tag) return e;                                                     \
		if (!tag_fallback) tag_fallback = e;                                                 \
	}

/* Attributes, C23 [[...]] groups and preprocessor directives can appear between
 * any two meaningful tokens. Scanners that must not see them restart the loop
 * whenever pparse_skip_noise moved the cursor. */
#define PPARSE_SKIP_NOISE_RESTART(tok)                                                               \
	do {                                                                                         \
		PParseToken *_n = pparse_skip_noise(_pc, tok);                                       \
		if (_n != (tok)) {                                                                   \
			(tok) = _n;                                                                  \
			continue;                                                                    \
		}                                                                                    \
	} while (0)

/* Warn and error wordings differ at some sites: the warning says what may
 * happen, the error says why it is refused. SAFETY_DIAG is for the common case
 * where one message serves both. */
#define SAFETY_DIAG2(t, warn_msg, err_msg)                                                           \
	do {                                                                                         \
		if (pparse_feat(PPARSE_F_WARN_SAFETY))                                               \
			pparse_warn_tok((t), warn_msg);                                              \
		else                                                                                 \
			pparse_error_tok((t), err_msg);                                              \
	} while (0)

/* -fno-safety downgrades safety errors to warnings. */
#define SAFETY_DIAG(t, msg)                                                                          \
	do {                                                                                         \
		if (pparse_feat(PPARSE_F_WARN_SAFETY))                                               \
			pparse_warn_tok((t), msg);                                                   \
		else                                                                                 \
			pparse_error_tok((t), msg);                                                  \
	} while (0)

/* Is entry `e` live at token index `cur`? */
#define PPARSE_ENTRY_COVERS(e, cur) ((e)->token_index <= (cur) && (cur) < (e)->scope_close_idx)

/* Walk a name chain newest-first; ACCEPT may return. */
#define PPARSE_CHAIN_WALK(T, ENTS, IDX, STOP, ACCEPT)                                                \
	while ((IDX) >= (STOP)) {                                                                    \
		T *e = &(ENTS)[IDX];                                                                 \
		ACCEPT                                                                               \
		(IDX) = e->prev_index;                                                               \
	}

/* Balanced-group skips for advancing and non-advancing loops. */
#define PPARSE_SKIP_GROUP_ON_CLOSE(t)                                                                \
	if ((t)->flags & PPARSE_TF_OPEN) {                                                           \
		(t) = pparse_pair_known(t);                                                          \
		continue;                                                                            \
	}

#define PPARSE_SKIP_GROUP_PAST(t)                                                                    \
	if ((t)->flags & PPARSE_TF_OPEN) {                                                           \
		(t) = pparse_next(_pc, pparse_pair_known(t));                                        \
		continue;                                                                            \
	}

/* Half-open token range. Starts are real tokens; a missing end stops at EOF. */
#define PPARSE_FOR_RANGE(t, from, to)                                                                \
	for (PParseToken *t = (from); t != (to) && t->kind != PPARSE_TK_EOF;                         \
	     t = pparse_next(_pc, t))

/* Walk from `from` to the end of the token stream. */
#define PPARSE_FOR_TAIL(t, from)                                                                     \
	for (PParseToken *t = (from); t->kind != PPARSE_TK_EOF; t = pparse_next(_pc, t))

enum {
	PPARSE_TDF_TYPEDEF = 1,
	PPARSE_TDF_VLA = 2,
	PPARSE_TDF_VOID = 4,
	PPARSE_TDF_ENUM_CONST = 8,
	PPARSE_TDF_CONST = 16,
	PPARSE_TDF_PTR = 32,
	PPARSE_TDF_ARRAY = 64,
	PPARSE_TDF_AGGREGATE = 128,
	PPARSE_TDF_FUNC = 256,
	PPARSE_TDF_PARAM = 512,
	PPARSE_TDF_VOLATILE = 1024,
	PPARSE_TDF_HAS_VOL_MEMBER = 2048,
	PPARSE_TDF_UNION = 4096,
	PPARSE_TDF_ATOMIC = 8192,
	PPARSE_TDF_CONSTEXPR = 16384, // C23 'constexpr': value is an integer constant expression
	PPARSE_TDF_LONG_DOUBLE = 32768,
	PPARSE_TDF_TYPEOF = 65536,
};

// Apply typedef traits to a parsed type.
static PParseTypedefEntry *pparse_typedef_lookup(PParseContext *_pc, PParseToken *tok);
static uint8_t pparse_array_rank_for_tok(PParseToken *t);
static inline void pparse_typedef_apply_tdf_flags(PParseTypeSpec *r, PParseToken *tok, int tflags) {
	PPARSE_CTX();
	r->is_typedef = true;
	r->is_vla |= (tflags & PPARSE_TDF_VLA) != 0;
	r->is_struct |= (tflags & PPARSE_TDF_AGGREGATE) != 0;
	if ((tflags & PPARSE_TDF_AGGREGATE) && !r->object_type_idx)
		r->object_type_idx = pparse_idx(_pc, tok) + 1;
	r->is_union |= (tflags & PPARSE_TDF_UNION) != 0;
	bool is_volatile = (tflags & PPARSE_TDF_VOLATILE) != 0;
	bool is_const = (tflags & PPARSE_TDF_CONST) != 0;
	bool has_volatile_member = (tflags & PPARSE_TDF_HAS_VOL_MEMBER) != 0;
	r->has_volatile |= is_volatile;
	r->has_hidden_volatile |= is_volatile | has_volatile_member;
	r->has_const |= is_const;
	r->has_decl_const |= is_const;
	r->has_hidden_const |= is_const;
	r->has_volatile_member |= has_volatile_member;
	r->has_atomic |= (tflags & PPARSE_TDF_ATOMIC) != 0;
	r->has_long_double |= (tflags & PPARSE_TDF_LONG_DOUBLE) != 0;
	r->has_typeof |= (tflags & PPARSE_TDF_TYPEOF) != 0;
	r->is_ptr |= (tflags & PPARSE_TDF_PTR) != 0;
	r->is_func |= (tflags & PPARSE_TDF_FUNC) != 0;
	if (tflags & PPARSE_TDF_ARRAY) {
		r->is_array = true;
		r->type_array_rank = pparse_array_rank_for_tok(tok);
		PParseTypedefEntry *e = pparse_typedef_lookup(_pc, tok);
		r->array_dim_complete = e && e->array_dim_complete;
	}
}

#define pparse_feat(f) (_pc->features & (f))

typedef enum {
	PPARSE_FS_NONE = 0,
	PPARSE_FS_FUNCTION = 1,
	PPARSE_FS_AGGREGATE_RETURN = 2,
} PParseFunctionSymbolKind;
/* Nonzero once any `raw {` brace was annotated this TU — gates scope walks. */

#define PPARSE_TD_SCOPE_SAVE() uint32_t _tds_c = pparse_td_scope_close
#define PPARSE_TD_SCOPE_RESTORE()                                                                                   \
	do {                                                                                                 \
		pparse_td_scope_close = _tds_c;                                                                     \
	} while (0)

#define pparse_is_c23_attr(t) ((t) && ((t)->flags & PPARSE_TF_C23_ATTR))
#define pparse_is_sizeof_like(t) ((t)->flags & PPARSE_TF_SIZEOF)

static inline PRISM_PURE uint32_t pparse_token_name_hash(PParseToken *tok) {
	return tok->parse_data;
}

static inline PRISM_PURE PParseFunctionSymbolKind pparse_function_symbol(PParseToken *tok) {
	PPARSE_CTX();
	if (!pparse_function_symbols.buckets) return PPARSE_FS_NONE;
	return (PParseFunctionSymbolKind)(intptr_t)pparse_hashmap_get_hashed(
	    &pparse_function_symbols, pparse_loc(_pc, tok), tok->len, pparse_token_name_hash(tok));
}

static inline void pparse_function_symbol_put(PParseToken *tok, PParseFunctionSymbolKind kind) {
	PPARSE_CTX();
	pparse_hashmap_put_hashed(
	    &pparse_function_symbols, pparse_loc(_pc, tok), tok->len, (void *)(intptr_t)kind, pparse_token_name_hash(tok));
}

#define pparse_is_enum_kw(t) ((t)->tag & PPARSE_TT_SUE && (t)->ch0 == 'e')

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool pparse_is_identifier_like(PParseToken *tok) {
	return tok->kind <= PPARSE_TK_KEYWORD; // PPARSE_TK_IDENT=0, PPARSE_TK_KEYWORD=1
}

static inline PParseToken *pparse_skip_balanced_group(PParseToken *tok) {
	PPARSE_CTX();
	return pparse_next(_pc, pparse_pair_known(tok));
}

static inline PParseToken *pparse_skip_prep_dirs_until(PParseToken *tok, PParseToken *end) {
	PPARSE_CTX();
	while (tok && tok != end && tok->kind == PPARSE_TK_PREP_DIR) tok = pparse_next(_pc, tok);
	return tok;
}

static inline PParseToken *pparse_skip_prep_dirs(PParseToken *tok) {
	return pparse_skip_prep_dirs_until(tok, NULL);
}

enum {
	PPARSE_PP_COND_NONE,
	PPARSE_PP_COND_OPEN,
	PPARSE_PP_COND_BRANCH,
	PPARSE_PP_COND_CLOSE,
};

static int pparse_pp_conditional_kind(PParseToken *s) {
	PPARSE_CTX();
	if (s->kind != PPARSE_TK_PREP_DIR) return PPARSE_PP_COND_NONE;
	const char *dp = pparse_loc(_pc, s);
	const char *end = dp + s->len;
	if (dp < end && *dp == '#') dp++;
	else if (end - dp >= 2 && dp[0] == '%' && dp[1] == ':') dp += 2;
	else if (end - dp >= 3 && dp[0] == '?' && dp[1] == '?' && dp[2] == '=') dp += 3;
	else return PPARSE_PP_COND_NONE;
	while (dp < end && pparse_is_hspace(*dp)) dp++;
	const char *word = dp;
	while (dp < end && ((*dp >= 'a' && *dp <= 'z') || (*dp >= 'A' && *dp <= 'Z') || *dp == '_')) dp++;
	size_t n = (size_t)(dp - word);
	static const struct {
		const char *word;
		uint8_t len, kind;
	} directives[] = {{"if", 2, PPARSE_PP_COND_OPEN},
			   {"ifdef", 5, PPARSE_PP_COND_OPEN},
			   {"ifndef", 6, PPARSE_PP_COND_OPEN},
			   {"elif", 4, PPARSE_PP_COND_BRANCH},
			   {"else", 4, PPARSE_PP_COND_BRANCH},
			   {"endif", 5, PPARSE_PP_COND_CLOSE}};
	for (size_t i = 0; i < sizeof(directives) / sizeof(*directives); i++)
		if (n == directives[i].len && !memcmp(word, directives[i].word, n))
			return directives[i].kind;
	return PPARSE_PP_COND_NONE;
}

static bool pparse_is_pp_conditional(PParseToken *s) {
	return pparse_pp_conditional_kind(s) != PPARSE_PP_COND_NONE;
}

static PRISM_PURE PParseToken *
pparse_skip_to_set(PParseToken *tok, PParseToken *end, uint64_t set);

static bool pparse_token_in_pp_conditional(PParseToken *tok) {
	PPARSE_CTX();
	int depth = 0;
	uint32_t end = pparse_idx(_pc, tok);
	for (uint32_t i = 1; i < end; i++) {
		PParseToken *s = &pparse_token_pool[i];
		if (s->file_idx != tok->file_idx || s->kind != PPARSE_TK_PREP_DIR) continue;
		int kind = pparse_pp_conditional_kind(s);
		if (kind == PPARSE_PP_COND_OPEN) depth++;
		else if (kind == PPARSE_PP_COND_CLOSE && depth > 0) depth--;
	}
	return depth > 0;
}

static PParseToken *pparse_span_find_pp_conditional(PParseToken *start, PParseToken *end, bool stop_at_semi) {
	PPARSE_CTX();
	PParseToken *scan_end = stop_at_semi
				? pparse_skip_to_set(start, end, pparse_CH(';'))
				: end;
	PPARSE_FOR_RANGE(s, start, scan_end) {
		if (pparse_is_pp_conditional(s)) return s;
	}
	return NULL;
}

// Skip noise tokens (attributes, C23 [[...]], prep dirs) in analysis mode.
static PRISM_PURE PParseToken *pparse_skip_noise(PParseContext *_pc, PParseToken *tok) {
	while (tok && tok->kind != PPARSE_TK_EOF) {
		if (tok->tag & PPARSE_TT_ATTR) {
			tok = pparse_next(_pc, tok);
			if (tok && pparse_match_ch(tok, '('))
				tok = pparse_next(_pc, pparse_pair_known(tok));
		} else if (pparse_is_c23_attr(tok)) {
			tok = pparse_next(_pc, pparse_pair_known(tok));
		} else if (tok->kind == PPARSE_TK_PREP_DIR) {
			tok = pparse_next(_pc, tok);
		} else
			break;
	}
	return tok;
}

/* `pparse_skip_noise` already eats PPARSE_TT_ATTR / [[...]] / _Pragma; this adds PPARSE_TT_ASM. */
static PParseToken *pparse_skip_asm_specifier_trail(PParseToken *t) {
	PPARSE_CTX();
	t = pparse_skip_noise(_pc, t);
	while (t && (t->tag & PPARSE_TT_ASM)) {
		t = pparse_next(_pc, t);
		if (t && pparse_match_ch(t, '(')) t = pparse_next(_pc, pparse_pair_known(t));
		t = pparse_skip_noise(_pc, t);
	}
	return t;
}

// Check if a token is "noise" (attribute, C23 [[...]], or preprocessor directive).
// These tokens must be skipped via pparse_skip_noise(_pc) before any tag-based type checks.
static inline PRISM_PURE bool pparse_is_noise_token(PParseToken *t) {
	return (t->tag & PPARSE_TT_ATTR) || pparse_is_c23_attr(t) || t->kind == PPARSE_TK_PREP_DIR;
}

#ifdef PRISM_DEBUG
#define pparse_ASSERT_NOT_NOISE(t)                                                                                  \
	do {                                                                                                 \
		if (pparse_is_noise_token(t))                                                                       \
			pparse_error_tok(t, "internal: tag check on noise token (pparse_skip_noise(_pc) missing)");           \
	} while (0)
#else
#define pparse_ASSERT_NOT_NOISE(t) ((void)0)
#endif

#define pparse_SKIP_NOISE_CONTINUE(var)                                                                             \
	do {                                                                                                 \
		PParseToken *_sn = pparse_skip_noise(_pc, var);                                                                \
		if (_sn != (var)) {                                                                          \
			(var) = _sn;                                                                         \
			continue;                                                                            \
		}                                                                                            \
	} while (0)

static PRISM_PURE PParseToken *pparse_skip_to_set(PParseToken *tok, PParseToken *end, uint64_t set) {
	PPARSE_CTX();
	if (!tok) return NULL;
	while (tok->kind != PPARSE_TK_EOF) {
		if (end && tok == end) return tok;
		PPARSE_SKIP_GROUP_PAST(tok)
		if (pparse_match_set(tok, set)) return tok;
		if ((tok->flags & PPARSE_TF_CLOSE) && tok->ch0 == '}') return tok;
		tok = pparse_next(_pc, tok);
	}
	return tok;
}
#define pparse_skip_to_semicolon(tok, end) pparse_skip_to_set((tok), (end), pparse_CH(';'))

#define PPARSE_TL_REBUILD_CADENCE 256

static void pparse_reset(void) {
	PPARSE_CTX();
	pparse_typedef_table = (PParseTypedefTable){0};
	pparse_ba = (PParseBoundsState){0};
	pparse_function_symbols = (PParseHashMap){0};
	_pc->parses_frozen = false;
	_pc->p1_func_meta = NULL;
	_pc->p1_func_meta_count = _pc->p1_func_meta_cap = 0;
	_pc->p1_func_entries = NULL;
	_pc->p1_func_entry_count = _pc->p1_func_entry_cap = 0;
	_pc->analysis_index = NULL;
	_pc->analysis_records = NULL;
	_pc->analysis_count = _pc->analysis_cap = 0;
}

/* O(log n) timelines for long typedef/bounds name chains. */
#define PPARSE_DEFINE_TIMELINE(PFX, ENTT, STATE, TABLE, IS_TAG)                                    \
	static int PFX##_tl_cmp_tok(const void *pa, const void *pb) {                              \
		PPARSE_CTX();                                                                      \
		uint32_t ta = (TABLE).entries[((const PParseTimelineItem *)pa)->entry_idx].token_index; \
		uint32_t tb = (TABLE).entries[((const PParseTimelineItem *)pb)->entry_idx].token_index; \
		return (ta > tb) - (ta < tb);                                                      \
	}                                                                                          \
                                                                                                   \
	static void PFX##_build_timelines(void) {                                                  \
		PPARSE_CTX();                                                                      \
		(STATE).tl_count_at_build = (TABLE).count;                                         \
		if ((STATE).max_chain_seen < 8 || !(TABLE).name_map.buckets) return;               \
		PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena, (STATE).tl_descs, (TABLE).count,         \
					(STATE).tl_desc_cap, 64, uint64_t);                        \
		memset((STATE).tl_descs, 0, (size_t)(TABLE).count * sizeof(*(STATE).tl_descs));    \
		/* Walk name_map heads only (one probe per unique name). */                        \
		int cap = (TABLE).name_map.capacity;                                               \
		int pos = 0;                                                                       \
		for (int b = 0; b < cap; b++) {                                                    \
			PParseHashEntry *ent = &(TABLE).name_map.buckets[b];                       \
			if (!ent->key || ent->key == PPARSE_TOMBSTONE) continue;                    \
			int head = (int)(intptr_t)ent->val - 1;                                    \
			int run = 0;                                                               \
			for (int j = head; j >= 0; j = (TABLE).entries[j].prev_index) run++;        \
			if (run < 8) continue;                                                     \
			int base = pos;                                                            \
			PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena, (STATE).timeline, pos + run,      \
						(STATE).tl_cap, 64, PParseTimelineItem);           \
			for (int j = head; j >= 0; j = (TABLE).entries[j].prev_index)              \
				(STATE).timeline[pos++].entry_idx = j;                              \
			qsort((STATE).timeline + base, (size_t)run, sizeof(PParseTimelineItem),     \
			      PFX##_tl_cmp_tok);                                                   \
			for (int p = base; p < pos; p++) {                                         \
				uint32_t close = (TABLE).entries[(STATE).timeline[p].entry_idx].scope_close_idx; \
				int prev = p - 1;                                                  \
				while (prev >= base &&                                             \
				       (TABLE).entries[(STATE).timeline[prev].entry_idx].scope_close_idx <= \
					   close)                                                  \
					prev = (STATE).timeline[prev].prev_cover;                   \
				(STATE).timeline[p].prev_cover = prev;                              \
			}                                                                          \
			(STATE).tl_descs[head] = ((uint64_t)(uint32_t)base << 32) | (uint32_t)run; \
		}                                                                                  \
	}                                                                                          \
                                                                                                   \
	static PRISM_PURE ENTT *PFX##_lookup_timeline(uint64_t desc, uint32_t cur, bool tags_only) { \
		PPARSE_CTX();                                                                      \
		/* Resolve the thread-locals once; the searches below would otherwise pay a        \
		 * _tlv_get_addr per iteration. */                                                 \
		ENTT *ents = (TABLE).entries;                                                      \
		const PParseTimelineItem *timeline = (STATE).timeline;                              \
		uint32_t base = (uint32_t)(desc >> 32);                                            \
		uint32_t run = (uint32_t)desc;                                                     \
		int lo = (int)base, hi = (int)base + (int)run;                                     \
		while (lo < hi) {                                                                  \
			int mid = lo + ((hi - lo) >> 1);                                           \
			if (ents[timeline[mid].entry_idx].token_index <= cur)                       \
				lo = mid + 1;                                                      \
			else                                                                       \
				hi = mid;                                                          \
		}                                                                                  \
		ENTT *tag_fallback = NULL;                                                         \
		for (int p = lo - 1; p >= (int)base;) {                                            \
			ENTT *e = &ents[timeline[p].entry_idx];                                      \
			if (cur >= e->scope_close_idx) {                                           \
				p = timeline[p].prev_cover;                                         \
				continue;                                                          \
			}                                                                          \
			p--;                                                                       \
			if (tags_only) {                                                           \
				if (IS_TAG(e)) return e;                                           \
				continue;                                                          \
			}                                                                          \
			if (!IS_TAG(e)) return e;                                                  \
			if (!tag_fallback) tag_fallback = e;                                       \
		}                                                                                  \
		return tag_fallback;                                                               \
	}

#define PPARSE_TL_IS_STRUCT_TAG(e) ((e)->is_struct_tag)
#define PPARSE_TL_NO_TAGS(e) 0

PPARSE_DEFINE_TIMELINE(pparse_td, PParseTypedefEntry, pparse_typedef_table.tl, pparse_typedef_table,
		       PPARSE_TL_IS_STRUCT_TAG)

PPARSE_DEFINE_TIMELINE(pparse_ba, PParseBoundsArrayEntry, pparse_ba.tl, pparse_ba.table,
		       PPARSE_TL_NO_TAGS)

static PRISM_PURE PParseBoundsArrayEntry *pparse_bounds_array_lookup(PParseContext *_pc, PParseToken *tok) {
	if (!pparse_is_identifier_like(tok)) return NULL;
	/* One thread-local resolution for the whole lookup, including the chain
	 * walks below — this runs for every identifier token in the TU. */
	PParseBoundsState *ba = &pparse_ba;
	unsigned c0 = tok->ch0, tl = tok->len;
	if (!(ba->table.bloom & (1ULL << ((c0 ^ tl) & 63)))) return NULL;
	char *name = pparse_loc(_pc, tok);
	uint32_t cur = pparse_idx(_pc, tok);
	PParseBoundsArrayEntry *ents = ba->table.entries;
	if (ba->tl.timeline) {
		uint32_t hash = pparse_token_name_hash(tok);
		int idx = pparse_hashmap_index_hashed(&ba->table.name_map, name, (int)tok->len, hash);
		PPARSE_CHAIN_WALK(PParseBoundsArrayEntry, ents, idx, ba->tl.tl_count_at_build,
				  if (PPARSE_ENTRY_COVERS(e, cur)) return e;)
		uint64_t desc = idx >= 0 ? ba->tl.tl_descs[idx] : 0;
		if (desc) return pparse_ba_lookup_timeline(desc, cur, false);
		PPARSE_CHAIN_WALK(PParseBoundsArrayEntry, ents, idx, 0,
				  if (PPARSE_ENTRY_COVERS(e, cur)) return e;)
		return NULL;
	}
	int idx = pparse_hashmap_index_hashed(&ba->table.name_map, name, (int)tok->len,
					      pparse_token_name_hash(tok));
	PPARSE_CHAIN_WALK(PParseBoundsArrayEntry, ents, idx, 0,
			  if (PPARSE_ENTRY_COVERS(e, cur)) return e;)
	return NULL;
}

static PParseTypedefEntry *
pparse_typedef_add_entry(PParseToken *tok, int scope_depth, PParseTypedefKind kind, bool is_vla, bool is_void) {
	PPARSE_CTX();
	char *name = pparse_loc(_pc, tok);
	int len = (int)tok->len;
	uint32_t hash = pparse_fast_hash(name, len);
	int existing = pparse_hashmap_index_hashed(&pparse_typedef_table.name_map, name, len, hash);
	// Skip duplicate re-definitions at the same scope (valid C11 §6.7/3).
	if (existing >= 0) {
		PParseTypedefEntry *prev = &pparse_typedef_table.entries[existing];
		uint16_t prev_scope_depth =
		    (uint16_t)pparse_token_pool[prev->token_index].td_entry;
		if ((kind == PPARSE_TDK_SHADOW) | (kind == PPARSE_TDK_VLA_VAR)) {
			if ((prev_scope_depth == (uint16_t)scope_depth) &
			    (prev->scope_close_idx == pparse_td_scope_close) &
			    (prev->is_shadow == (kind == PPARSE_TDK_SHADOW)) &
			    (prev->is_vla_var == (kind == PPARSE_TDK_VLA_VAR)))
				return NULL;
		} else if ((prev_scope_depth == (uint16_t)scope_depth) & !prev->is_shadow &
			   (prev->scope_close_idx == pparse_td_scope_close) &
			   (prev->is_struct_tag == (kind == PPARSE_TDK_STRUCT_TAG)))
			return NULL;
	}

	PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena,
			 pparse_typedef_table.entries,
			 pparse_typedef_table.count + 1,
			 pparse_typedef_table.capacity,
			 32,
			 PParseTypedefEntry);
	int new_index = pparse_typedef_table.count++;
	PParseTypedefEntry *e = &pparse_typedef_table.entries[new_index];
	tok->td_entry = (uint32_t)(uint16_t)scope_depth;
	*e = (PParseTypedefEntry){
	    .prev_index = existing,
	    .token_index = pparse_idx(_pc, tok),
	    .scope_close_idx = pparse_td_scope_close,
	    .is_vla = ((kind == PPARSE_TDK_TYPEDEF) | (kind == PPARSE_TDK_VLA_VAR) |
		       (kind == PPARSE_TDK_STRUCT_TAG)) & is_vla,
	    .is_void = (kind == PPARSE_TDK_TYPEDEF) & is_void,
	    .is_shadow = (kind == PPARSE_TDK_SHADOW) | (kind == PPARSE_TDK_ENUM_CONST),
	    .is_enum_const = kind == PPARSE_TDK_ENUM_CONST,
	    .is_vla_var = kind == PPARSE_TDK_VLA_VAR,
	    .is_struct_tag = kind == PPARSE_TDK_STRUCT_TAG,
	    .array_dim_complete = true,
	};
	pparse_hashmap_put_hashed(&pparse_typedef_table.name_map, name, len, (void *)(intptr_t)(new_index + 1), hash);
	pparse_typedef_table.bloom |= 1ULL << (((unsigned char)name[0] ^ len) & 63);
	int cl = 1;
	for (int p = e->prev_index; (p >= 0) & (cl < 8); p = pparse_typedef_table.entries[p].prev_index) cl++;
	if (cl > pparse_typedef_table.tl.max_chain_seen) pparse_typedef_table.tl.max_chain_seen = cl;
	/* Rebuild mid-Pass1 so long same-name chains stay O(log n), with a
	 * constant bound on the unsorted prefix between rebuilds. */
	if (pparse_typedef_table.tl.max_chain_seen >= 8) {
		int since = pparse_typedef_table.count - pparse_typedef_table.tl.tl_count_at_build;
		if ((pparse_typedef_table.tl.timeline == NULL) | (since >= PPARSE_TL_REBUILD_CADENCE))
			pparse_td_build_timelines();
	}
	return e;
}

static PRISM_PURE int pparse_typedef_get_index(char *name, int len) {
	PPARSE_CTX();
	return pparse_hashmap_index_hashed(&pparse_typedef_table.name_map, name, len, pparse_fast_hash(name, len));
}
static PParseTypedefEntry *pparse_binding_entry(PParseToken *tok, bool shadow_only) {
	PPARSE_CTX();
	for (int ix = pparse_typedef_get_index(pparse_loc(_pc, tok), tok->len); ix >= 0;
	     ix = pparse_typedef_table.entries[ix].prev_index) {
		PParseTypedefEntry *e = &pparse_typedef_table.entries[ix];
		if ((e->token_index == pparse_idx(_pc, tok)) & (!shadow_only | e->is_shadow)) return e;
	}
	return NULL;
}

/* Register a token-owned C binding and preserve its source identity in one
 * place. Prism never needs to coordinate the typedef table's name/index API. */
static PParseTypedefEntry *pparse_register_binding(PParseToken *tok, int scope_depth, PParseTypedefKind kind) {
	PParseTypedefEntry *added =
	    pparse_typedef_add_entry(tok, scope_depth, kind, kind == PPARSE_TDK_VLA_VAR, false);
	if (added) return added;
	return pparse_binding_entry(tok, kind == PPARSE_TDK_SHADOW);
}

static inline PParseTypedefEntry *pparse_register_shadow(PParseToken *tok, int scope_depth) {
	return pparse_register_binding(tok, scope_depth, PPARSE_TDK_SHADOW);
}

static inline PParseTypedefEntry *pparse_register_vla_var(PParseToken *tok, int scope_depth) {
	return pparse_register_binding(tok, scope_depth, PPARSE_TDK_VLA_VAR);
}

enum {
	PPARSE_BIND_FUNC = 1u << 0,
	PPARSE_BIND_CONST = 1u << 1,
	PPARSE_BIND_VOLATILE = 1u << 2,
	PPARSE_BIND_VOLATILE_MEMBER = 1u << 3,
	PPARSE_BIND_ATOMIC = 1u << 4,
	PPARSE_BIND_AGGREGATE = 1u << 5,
	PPARSE_BIND_CONSTEXPR = 1u << 6,
	PPARSE_BIND_LONG_DOUBLE = 1u << 7,
};

static inline void pparse_binding_apply_traits(PParseTypedefEntry *entry, unsigned traits) {
	if (!entry) return;
	entry->is_func |= (traits & PPARSE_BIND_FUNC) != 0;
	entry->is_const |= (traits & PPARSE_BIND_CONST) != 0;
	entry->is_volatile |= (traits & PPARSE_BIND_VOLATILE) != 0;
	entry->has_volatile_member |= (traits & PPARSE_BIND_VOLATILE_MEMBER) != 0;
	entry->is_atomic |= (traits & PPARSE_BIND_ATOMIC) != 0;
	entry->is_long_double |= (traits & PPARSE_BIND_LONG_DOUBLE) != 0;
	entry->is_aggregate |= (traits & PPARSE_BIND_AGGREGATE) != 0;
	entry->is_constexpr |= (traits & PPARSE_BIND_CONSTEXPR) != 0;
}

/* Register or update an ordinary identifier without exposing table insertion
 * order to consumers. Multiple traits intentionally collapse to one lookup. */
static PParseTypedefEntry *pparse_register_shadow_traits(PParseToken *tok, int scope_depth, unsigned traits) {
	PParseTypedefEntry *entry = pparse_register_shadow(tok, scope_depth);
	pparse_binding_apply_traits(entry, traits);
	return entry;
}

static PParseTypedefEntry *pparse_register_vla_binding(PParseToken *tok, int scope_depth) {
	PParseTypedefEntry *entry = pparse_register_vla_var(tok, scope_depth);
	if (entry) entry->is_param = true;
	return entry;
}

static inline bool pparse_is_soft_keyword_identifier(PParseToken *tok);

static PRISM_PURE PParseTypedefEntry *pparse_typedef_lookup(PParseContext *_pc, PParseToken *tok) {
	if (!pparse_is_identifier_like(tok)) return NULL;
	PParseTypedefTable *tbl = &pparse_typedef_table;
	PParseTimelineState *td = &tbl->tl;
	if (tok->kind == PPARSE_TK_KEYWORD && !pparse_is_soft_keyword_identifier(tok) &&
	    !(tok->tag & (PPARSE_TT_ORELSE | PPARSE_TT_DEFER)) && !(tok->flags & PPARSE_TF_RAW))
		return NULL;
	unsigned c0 = tok->ch0, tl = tok->len;
	if (!(tbl->bloom & (1ULL << ((c0 ^ tl) & 63)))) return NULL;
	char *name = pparse_loc(_pc, tok);
	uint32_t cur = pparse_idx(_pc, tok);
	if (td->timeline) {
		uint32_t hash = pparse_token_name_hash(tok);
		PParseTypedefEntry *tag_fallback = NULL;
		int idx = pparse_hashmap_index_hashed(&tbl->name_map, name, (int)tok->len, hash);
		/* Entries added since the last build are absent from the sorted
		 * timeline. Walk only that bounded newest prefix. */
		PPARSE_CHAIN_WALK(PParseTypedefEntry, tbl->entries, idx, td->tl_count_at_build,
				  PPARSE_ACCEPT_NONTAG_ELSE_REMEMBER)
		uint64_t desc = idx >= 0 ? td->tl_descs[idx] : 0;
		if (desc) {
			PParseTypedefEntry *hit = pparse_td_lookup_timeline(desc, cur, false);
			return hit ? hit : tag_fallback;
		}
		/* Short chain (no timeline): finish the ordinary walk from idx. */
		PPARSE_CHAIN_WALK(PParseTypedefEntry, tbl->entries, idx, 0,
				  PPARSE_ACCEPT_NONTAG_ELSE_REMEMBER)
		return tag_fallback;
	}
	int idx = pparse_hashmap_index_hashed(&tbl->name_map, name, tok->len, pparse_token_name_hash(tok));
	// ISO C11 §6.2.3: tag namespace is separate from ordinary identifiers.
	PParseTypedefEntry *tag_fallback = NULL;
	PPARSE_CHAIN_WALK(PParseTypedefEntry, tbl->entries, idx, 0,
			  PPARSE_ACCEPT_NONTAG_ELSE_REMEMBER)
	return tag_fallback;
}

typedef enum {
	PPARSE_BINDING_NONE,
	PPARSE_BINDING_TYPE,
	PPARSE_BINDING_VALUE,
} PParseIdentifierBindingKind;

static PRISM_PURE PParseIdentifierBindingKind pparse_identifier_binding_kind(PParseToken *tok) {
	PPARSE_CTX();
	PParseTypedefEntry *entry = pparse_typedef_lookup(_pc, tok);
	if (!entry) return PPARSE_BINDING_NONE;
	bool value = entry->is_shadow | entry->is_enum_const | entry->is_vla_var;
	return (PParseIdentifierBindingKind)(PPARSE_BINDING_TYPE + value);
}

static inline PRISM_PURE bool pparse_token_has_binding(PParseToken *tok) {
	return pparse_identifier_binding_kind(tok) != PPARSE_BINDING_NONE;
}

// Enforces ISO C11 §6.2.3 namespace separation: tag names live in a
static PRISM_PURE PParseTypedefEntry *pparse_tag_lookup(PParseToken *tok) {
	PPARSE_CTX();
	if (!pparse_is_identifier_like(tok)) return NULL;
	unsigned c0 = tok->ch0, tl = tok->len;
	if (!(pparse_typedef_table.bloom & (1ULL << ((c0 ^ tl) & 63)))) return NULL;
	char *name = pparse_loc(_pc, tok);
	uint32_t cur = pparse_idx(_pc, tok);
	if (pparse_typedef_table.tl.timeline) {
		uint32_t hash = pparse_token_name_hash(tok);
		int idx = pparse_hashmap_index_hashed(&pparse_typedef_table.name_map, name, (int)tok->len, hash);
		PPARSE_CHAIN_WALK(PParseTypedefEntry, pparse_typedef_table.entries, idx,
				  pparse_typedef_table.tl.tl_count_at_build,
				  if (e->is_struct_tag && PPARSE_ENTRY_COVERS(e, cur)) return e;)
		uint64_t desc = idx >= 0 ? pparse_typedef_table.tl.tl_descs[idx] : 0;
		if (desc) return pparse_td_lookup_timeline(desc, cur, true);
		PPARSE_CHAIN_WALK(PParseTypedefEntry, pparse_typedef_table.entries, idx, 0,
				  if (e->is_struct_tag && PPARSE_ENTRY_COVERS(e, cur)) return e;)
		return NULL;
	}
	int idx = pparse_hashmap_index_hashed(&pparse_typedef_table.name_map, name, tok->len, pparse_token_name_hash(tok));
	PPARSE_CHAIN_WALK(PParseTypedefEntry, pparse_typedef_table.entries, idx, 0,
			  if (e->is_struct_tag && PPARSE_ENTRY_COVERS(e, cur)) return e;)
	return NULL;
}

static inline PRISM_PURE int pparse_typedef_flags_(PParseToken *tok, bool include_arrays) {
	PPARSE_CTX();
	PParseTypedefEntry *e = pparse_typedef_lookup(_pc, tok);
	if (!e) {
		if (!include_arrays) return 0;
		PParseBoundsArrayEntry *be = pparse_bounds_array_lookup(_pc, tok);
		if (!be || be->blocks_outer) return 0;
		return PPARSE_TDF_ARRAY | (be->is_vla_var ? PPARSE_TDF_VLA : 0);
	}
	if (e->is_enum_const) return PPARSE_TDF_ENUM_CONST;
	if (e->is_shadow) {
		int fl = (e->is_volatile ? PPARSE_TDF_VOLATILE : 0) |
			 (e->has_volatile_member ? PPARSE_TDF_HAS_VOL_MEMBER : 0) |
			 (e->is_atomic ? PPARSE_TDF_ATOMIC : 0) |
			 (e->is_long_double ? PPARSE_TDF_LONG_DOUBLE : 0) |
			 (e->is_array ? PPARSE_TDF_ARRAY : 0) |
			 (e->is_constexpr ? PPARSE_TDF_CONSTEXPR : 0);
		/* Decayed params must not inherit PPARSE_TDF_ARRAY from an outer
		 * file-scope array of the same name. */
		if (!(fl & PPARSE_TDF_ARRAY) && !e->is_param) {
			PParseBoundsArrayEntry *be = pparse_bounds_array_lookup(_pc, tok);
			if (be && !be->blocks_outer) fl |= PPARSE_TDF_ARRAY | (be->is_vla_var ? PPARSE_TDF_VLA : 0);
		}
		return fl;
	}
	if (e->is_vla_var)
		return PPARSE_TDF_VLA | (e->is_param ? PPARSE_TDF_PARAM : 0) |
		       (e->has_volatile_member ? PPARSE_TDF_HAS_VOL_MEMBER : 0) |
		       (e->is_array ? PPARSE_TDF_ARRAY : 0);
	if (e->is_struct_tag)
		return (e->is_vla ? PPARSE_TDF_VLA : 0) | (e->has_volatile_member ? PPARSE_TDF_HAS_VOL_MEMBER : 0) |
		       (e->is_aggregate ? PPARSE_TDF_AGGREGATE : 0);
	return PPARSE_TDF_TYPEDEF | (e->is_vla ? PPARSE_TDF_VLA : 0) | (e->is_void ? PPARSE_TDF_VOID : 0) |
	       (e->is_const ? PPARSE_TDF_CONST : 0) | (e->is_volatile ? PPARSE_TDF_VOLATILE : 0) |
	       (e->is_ptr ? PPARSE_TDF_PTR : 0) | (e->is_array ? PPARSE_TDF_ARRAY : 0) |
	       (e->is_aggregate ? PPARSE_TDF_AGGREGATE : 0) | (e->is_func ? PPARSE_TDF_FUNC : 0) |
	       (e->has_volatile_member ? PPARSE_TDF_HAS_VOL_MEMBER : 0) | (e->is_union ? PPARSE_TDF_UNION : 0) |
	       (e->is_atomic ? PPARSE_TDF_ATOMIC : 0) |
	       (e->is_long_double ? PPARSE_TDF_LONG_DOUBLE : 0) |
	       (e->is_typeof ? PPARSE_TDF_TYPEOF : 0);
}

#define pparse_typedef_flags(tok) pparse_typedef_flags_((tok), true)
#define pparse_is_known_typedef(tok) (pparse_typedef_flags_((tok), false) & PPARSE_TDF_TYPEDEF)
#define pparse_is_vla_typedef(tok) (pparse_typedef_flags(tok) & PPARSE_TDF_VLA)
#define pparse_is_known_enum_const(tok) (pparse_typedef_flags_((tok), false) & PPARSE_TDF_ENUM_CONST)
#define pparse_is_constexpr_ident(tok) (pparse_typedef_flags_((tok), false) & PPARSE_TDF_CONSTEXPR)

static inline bool pparse_token_can_name_function(PParseToken *tok) {
	return tok->kind == PPARSE_TK_IDENT || (tok->tag & (PPARSE_TT_DEFER | PPARSE_TT_ORELSE)) ||
	       (tok->flags & PPARSE_TF_RAW);
}

static inline bool pparse_is_known_function_call(PParseToken *tok) {
	PPARSE_CTX();
	if (!pparse_function_symbol(tok)) return false;
	PParseToken *next = pparse_skip_noise(_pc, pparse_next(_pc, tok));
	return next && pparse_match_ch(next, '(');
}

static bool pparse_token_can_precede_function_name(PParseToken *tok) {
	PPARSE_CTX();
	while (tok && (pparse_match_ch(tok, '*') || (tok->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_INLINE))))
		tok = pparse_walk_back(pparse_idx(_pc, tok), PPARSE_WB_PAST_NOISE);
	return tok && ((tok->tag & (PPARSE_TT_TYPE | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT)) || pparse_is_known_typedef(tok));
}

static bool pparse_paren_is_function_params(PParseToken *open) {
	PPARSE_CTX();
	if (!open || !pparse_match_ch(open, '(')) return false;
	PParseToken *close = pparse_pair_known(open);
	PParseToken *after = pparse_skip_asm_specifier_trail(pparse_next(_pc, close));
	if (!after ||
	    !(pparse_match_ch(after, '{') || pparse_match_ch(after, ';') || pparse_match_ch(after, ',') || pparse_match_ch(after, '=') ||
	      pparse_match_ch(after, ')') ||
	      (after->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_INLINE | PPARSE_TT_TYPEDEF |
			     PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT)) ||
	      pparse_is_known_typedef(after)))
		return false;
	/* Keep the `)` in `(*F)(...)`: jumping the pointer group would mistake
	 * typedef/function-pointer parameter dimensions for expression parens. */
	PParseToken *prev = pparse_walk_back(pparse_idx(_pc, open), PPARSE_WB_ATTR_NOISE);
	if (prev && pparse_token_can_name_function(prev)) {
		PParseToken *before = pparse_walk_back(pparse_idx(_pc, prev), PPARSE_WB_ATTR_NOISE);
		return pparse_token_can_precede_function_name(before);
	}
	PParseToken *pointer_open = prev && pparse_match_ch(prev, ')') ? pparse_pair_known(prev) : NULL;
	if (pointer_open) {
		PParseToken *before = pparse_walk_back(pparse_idx(_pc, pointer_open), PPARSE_WB_ATTR_NOISE);
		return pparse_token_can_precede_function_name(before);
	}
	return false;
}

/* Innermost enclosing delimiter; pair jumps make cost proportional to nesting. */
static PParseToken *pparse_enclosing_open(PParseContext *_pc, PParseToken *from, bool stop_at_stmt) {
	if (!from) return NULL;
	for (uint32_t i = pparse_idx(_pc, from); i > 0; i--) {
		PParseToken *t = &pparse_token_pool[i - 1];
		if (t->kind == PPARSE_TK_PREP_DIR) continue;
		if (t->flags & PPARSE_TF_CLOSE) {
			/* +1 compensates for the loop's decrement after the jump. */
			i = pparse_idx(_pc, pparse_pair_known(t)) + 1;
			continue;
		}
		if (t->flags & PPARSE_TF_OPEN) return t;
		if (stop_at_stmt &&
		    (pparse_match_ch(t, ';') || pparse_match_ch(t, '{') || pparse_match_ch(t, '}')))
			return NULL;
	}
	return NULL;
}

/* Previous same-depth sibling, or NULL at the enclosing open. */
static PParseToken *pparse_prev_sibling(PParseContext *_pc, PParseToken *from) {
	if (!from) return NULL;
	PParseToken *t = pparse_walk_back(pparse_idx(_pc, from),
					  PPARSE_WB_FROM_PRED | PPARSE_WB_SKIP_PREP | PPARSE_WB_JUMP_GROUPS);
	return t && (t->flags & PPARSE_TF_OPEN) ? NULL : t;
}

static PParseToken *pparse_function_param_open(PParseToken *tok) {
	PPARSE_CTX();
	for (PParseToken *o = pparse_enclosing_open(_pc, tok, true); o; o = pparse_enclosing_open(_pc, o, true))
		if (pparse_match_ch(o, '(')) return pparse_paren_is_function_params(o) ? o : NULL;
	return NULL;
}

static PRISM_PURE uint8_t pparse_array_rank_for_tok(PParseToken *t) {
	PPARSE_CTX();
	PParseTypedefEntry *te = pparse_typedef_lookup(_pc, t);
	if (te && te->array_rank > 0) return te->array_rank;
	PParseBoundsArrayEntry *be = pparse_bounds_array_lookup(_pc, t);
	if (be && be->array_rank > 0) return be->array_rank;
	return 0;
}

typedef struct {
	uint8_t rank;
	bool tracked : 1;
	bool dim_complete : 1;
} PParseArrayBindingInfo;

/* Resolve the two parser-owned binding stores as one semantic array query.
 * Ordinary bindings take precedence when they explicitly block array use. */
static PRISM_PURE PParseArrayBindingInfo pparse_array_binding_info(PParseToken *tok) {
	PPARSE_CTX();
	PParseArrayBindingInfo info = {.dim_complete = true};
	PParseTypedefEntry *binding = pparse_typedef_lookup(_pc, tok);
	if (binding && (binding->is_param || binding->is_enum_const)) return info;
	PParseBoundsArrayEntry *array = pparse_bounds_array_lookup(_pc, tok);
	if (array) {
		info.rank = array->array_rank;
		info.tracked = !array->blocks_outer;
		info.dim_complete = array->array_dim_complete;
		return info;
	}
	if (binding && (binding->is_array || binding->is_vla_var)) {
		info.rank = binding->array_rank;
		info.tracked = true;
		info.dim_complete = binding->array_dim_complete;
	}
	return info;
}

static void pparse_bounds_array_add(char *name, int len, uint32_t token_index, uint8_t array_rank,
				     bool dim_complete, bool is_vla_var, bool blocks_outer) {
	PPARSE_CTX();
	uint32_t hash = pparse_fast_hash(name, len);
	int existing = pparse_hashmap_index_hashed(&pparse_ba.table.name_map, name, len, hash);
	if (existing >= 0) {
		PParseBoundsArrayEntry *prev = &pparse_ba.table.entries[existing];
		if (prev->token_index == token_index) {
			prev->array_rank = array_rank;
			prev->array_dim_complete = dim_complete;
			prev->is_vla_var = is_vla_var;
			prev->blocks_outer = blocks_outer;
			return;
		}
	}

	PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena,
			 pparse_ba.table.entries,
			 pparse_ba.table.count + 1,
			 pparse_ba.table.capacity,
			 32,
			 PParseBoundsArrayEntry);
	int new_index = pparse_ba.table.count++;
	PParseBoundsArrayEntry *e = &pparse_ba.table.entries[new_index];
	*e = (PParseBoundsArrayEntry){.prev_index = existing,
					 .token_index = token_index,
					 .scope_close_idx = pparse_td_scope_close,
					 .array_rank = array_rank,
					 .array_dim_complete = dim_complete,
					 .is_vla_var = is_vla_var,
					 .blocks_outer = blocks_outer};
	pparse_hashmap_put_hashed(
	    &pparse_ba.table.name_map, name, len, (void *)(intptr_t)(new_index + 1), hash);
	pparse_ba.table.bloom |= 1ULL << (((unsigned char)name[0] ^ len) & 63);
	int cl = 1;
	for (int p = e->prev_index; p >= 0 && cl < 8; p = pparse_ba.table.entries[p].prev_index) cl++;
	if (cl > pparse_ba.tl.max_chain_seen) pparse_ba.tl.max_chain_seen = cl;
	if (pparse_ba.tl.max_chain_seen >= 8) {
		int since = pparse_ba.table.count - pparse_ba.tl.tl_count_at_build;
		if (!pparse_ba.tl.timeline || since >= PPARSE_TL_REBUILD_CADENCE) pparse_ba_build_timelines();
	}
}
static void pparse_register_array_binding(PParseToken *tok,
				     uint8_t rank,
				     bool dim_complete,
				     bool is_vla,
				     bool blocks_outer) {
	PPARSE_CTX();
	pparse_bounds_array_add(
	    pparse_loc(_pc, tok), tok->len, pparse_idx(_pc, tok), rank, dim_complete, is_vla, blocks_outer);
}

static PRISM_PURE PParseBoundsArrayEntry *pparse_bounds_array_entry_for_token(PParseToken *t) {
	PPARSE_CTX();
	for (int ix = pparse_hashmap_index_hashed(
		 &pparse_ba.table.name_map, pparse_loc(_pc, t), t->len, pparse_token_name_hash(t));
	     ix >= 0;
	     ix = pparse_ba.table.entries[ix].prev_index) {
		PParseBoundsArrayEntry *e = &pparse_ba.table.entries[ix];
		if (e->token_index == pparse_idx(_pc, t)) return e;
	}
	return NULL;
}
/* VLA registration changes the ordinary binding kind. Preserve array facts
 * recorded earlier in the declaration without relying on table tail order. */
static void pparse_register_vla_preserving_array(PParseToken *tok, int scope_depth) {
	PParseArrayBindingInfo saved = {.dim_complete = true};
	PParseBoundsArrayEntry *array = pparse_bounds_array_entry_for_token(tok);
	if (array) {
		saved.rank = array->array_rank;
		saved.tracked = !array->blocks_outer;
		saved.dim_complete = array->array_dim_complete;
	} else {
		PParseTypedefEntry *binding = pparse_binding_entry(tok, false);
		if (binding && binding->is_shadow && binding->is_array) {
			saved.rank = binding->array_rank;
			saved.tracked = true;
			saved.dim_complete = binding->array_dim_complete;
		}
	}
	PParseTypedefEntry *vla = pparse_register_vla_var(tok, scope_depth);
	if (!saved.tracked || !vla) return;
	vla->is_array = true;
	vla->array_rank = saved.rank;
	vla->array_dim_complete = saved.dim_complete;
	pparse_register_array_binding(tok, saved.rank, saved.dim_complete, true, false);
}

static PRISM_PURE bool pparse_is_type_keyword(PParseToken *tok) {
	if (tok->tag & PPARSE_TT_TYPE) return true;
	if (tok->kind != PPARSE_TK_IDENT && tok->kind != PPARSE_TK_KEYWORD) return false;
	return pparse_is_known_typedef(tok);
}

static inline PRISM_PURE bool pparse_is_soft_keyword_identifier(PParseToken *tok) {
	return tok->kind == PPARSE_TK_KEYWORD && (tok->flags & PPARSE_TF_SOFT_KW);
}

static inline bool pparse_soft_keyword_decl_name_boundary(PParseToken *tok) {
	PPARSE_CTX();
	PParseToken *after = pparse_skip_noise(_pc, pparse_next(_pc, tok));
	/* Include '(' so `int orelse(int);` / `T defer(void)` treat the soft
	 * keyword as a declarator name (function prototype), not an operator. */
	uint64_t end = pparse_CH(';') | pparse_CH(',') | pparse_CH('=') | pparse_CH('[') | pparse_CH(':') | pparse_CH('(');
	return after && (pparse_match_set(after, end) || (after->tag & PPARSE_TT_ASM));
}

static inline PRISM_PURE bool pparse_is_valid_varname(PParseToken *tok) {
	return tok->kind == PPARSE_TK_IDENT || pparse_is_soft_keyword_identifier(tok) || (tok->flags & PPARSE_TF_RAW) ||
	       (tok->tag & (PPARSE_TT_DEFER | PPARSE_TT_ORELSE));
}

static inline bool pparse_token_is_label_name(PParseToken *tok) {
	PPARSE_CTX();
	PParseToken *colon = pparse_skip_noise(_pc, pparse_next(_pc, tok));
	return pparse_is_identifier_like(tok) && colon && pparse_match_ch(colon, ':') &&
	       !(pparse_next(_pc, colon) && pparse_match_ch(pparse_next(_pc, colon), ':')) && !(tok->tag & (PPARSE_TT_CASE | PPARSE_TT_DEFAULT));
}

static bool pparse_is_strict_bare_function_call(PParseToken *start, PParseToken *end) {
	PPARSE_CTX();
	PParseToken *tok = pparse_skip_prep_dirs_until(start, end);
	if (!tok || tok == end || !pparse_is_valid_varname(tok) || pparse_is_type_keyword(tok)) return false;
	PParseToken *function = tok;
	tok = pparse_skip_prep_dirs_until(pparse_next(_pc, function), end);
	if (!tok || tok == end || !pparse_match_ch(tok, '(') || !(tok->flags & PPARSE_TF_OPEN)) return false;
	PParseToken *close = pparse_pair_known(tok);
	return pparse_skip_prep_dirs_until(pparse_next(_pc, close), end) == end &&
	       pparse_function_symbol(function) != PPARSE_FS_NONE;
}

static inline PRISM_PURE bool pparse_is_expr_ending(PParseToken *t) {
	return (t->kind == PPARSE_TK_IDENT || t->kind == PPARSE_TK_KEYWORD || t->kind == PPARSE_TK_NUM || t->kind == PPARSE_TK_STR) ||
	       pparse_match_set(t, pparse_CH(')') | pparse_CH(']'));
}

static inline PRISM_PURE bool pparse_is_expr_ending_brace(PParseToken *t) {
	return pparse_is_expr_ending(t) || pparse_match_ch(t, '}');
}

static void pparse_enum_constants(PParseToken *tok, int scope_depth) {
	PPARSE_CTX();
	if (!tok || !(pparse_match_ch(tok, '{'))) return;
	tok = pparse_next(_pc, tok); // Skip '{'
	while (tok && tok->kind != PPARSE_TK_EOF && !(pparse_match_ch(tok, '}'))) {
		pparse_SKIP_NOISE_CONTINUE(tok);
		if (pparse_is_valid_varname(tok)) {
			pparse_ann(tok) |= P1_DEFER_SHADOW_NAME;
			pparse_typedef_add_entry(tok, scope_depth, PPARSE_TDK_ENUM_CONST, false, false);
			tok = pparse_next(_pc, tok);
			tok = pparse_skip_noise(_pc, tok); // Skip C23/GNU attributes on enumerator

			if (tok && pparse_match_ch(tok, '=')) {
				tok = pparse_next(_pc, tok);
				while (tok && tok->kind != PPARSE_TK_EOF) {
					PPARSE_SKIP_GROUP_PAST(tok)
					if (tok->len == 1 && (tok->ch0 == ',' || tok->ch0 == '}')) break;
					tok = pparse_next(_pc, tok);
				}
			}

			if (tok && pparse_match_ch(tok, ',')) tok = pparse_next(_pc, tok);
		} else
			tok = pparse_next(_pc, tok);
	}
}

static inline PRISM_PURE bool pparse_is_orelse_kw_shadow(PParseToken *tok) {
	PPARSE_CTX();
	if (!(tok->tag & PPARSE_TT_ORELSE)) return false;
	PParseTypedefEntry *te = pparse_typedef_lookup(_pc, tok);
	return !te || te->is_shadow;
}

static bool pparse_close_brace_ends_sue_body(PParseToken *tok);
static bool pparse_token_ends_sue_type_specifier(PParseToken *tok);
static bool pparse_close_paren_ends_cast_type_name(PParseToken *tok);
static bool pparse_orelse_is_label_or_goto_target(PParseToken *tok, PParseToken *prev);
static PParseTypeSpec pparse_type_specifier(PParseToken *tok);

static bool pparse_close_paren_ends_type_specifier_ctor(PParseToken *tok);

static inline bool pparse_orelse_shadow_is_kw(PParseToken *prev) {
	PPARSE_CTX();
	if (!prev) return false;
	if (pparse_token_ends_sue_type_specifier(prev)) return false;
	if (pparse_close_paren_ends_cast_type_name(prev)) return false;
	/* `_BitInt(N) orelse` / `typeof(T) orelse` / `_Alignas(N) int orelse`:
	 * the closing `)` ends a type-specifier constructor, not an expression. */
	if (pparse_close_paren_ends_type_specifier_ctor(prev)) return false;
	if (pparse_orelse_is_label_or_goto_target(NULL, prev)) return false;
	/* `return orelse;` — orelse is the return operand (identifier), not an
	 * operator. Real keyword form is `return x orelse fb;`.
	 * Do NOT exclude break/continue: `… orelse continue orelse …` mid-chain
	 * needs the second orelse classified as a keyword. goto is already
	 * handled by pparse_orelse_is_label_or_goto_target above. */
	if (prev->tag & PPARSE_TT_RETURN) return false;
	/* Soft keywords (incl. type spellings used as names: `_Float32`, `bool`,
	 * `asm`) are expression-ending. `bool orelse = 0` is handled in
	 * orelse_kw_at via pparse_soft_keyword_decl_name_boundary before this runs. */
	if (pparse_is_soft_keyword_identifier(prev)) return pparse_is_expr_ending_brace(prev);
	if (prev->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_SKIP_DECL |
			 PPARSE_TT_ALIGNAS | PPARSE_TT_INLINE | PPARSE_TT_ATTR))
		return false;
	/* `T orelse = 0` where T is a typedef-name: mirror is_defer_kw. */
	if (pparse_is_known_typedef(prev)) return false;
	if (prev->len == 2 && (prev->ch0 == '+' || prev->ch0 == '-') && pparse_loc(_pc, prev)[1] == prev->ch0)
		return true;
	return pparse_is_expr_ending_brace(prev);
}

static inline PParseToken *pparse_sue_body_recipe(PParseToken *tok);

static PParseToken *pparse_find_struct_body_brace(PParseToken *tok) {
	PPARSE_CTX();
	PParseToken *recipe = pparse_sue_body_recipe(tok);
	if (recipe) return recipe;
	PParseToken *t = pparse_next(_pc, tok);
	bool saw_tag = false;
	while (t && t->kind != PPARSE_TK_EOF) {
		pparse_SKIP_NOISE_CONTINUE(t);
		if (!saw_tag && pparse_is_valid_varname(t)) {
			saw_tag = true;
			t = pparse_next(_pc, t);
		} else if ((t->tag & PPARSE_TT_QUALIFIER) || pparse_is_type_keyword(t)) {
			t = pparse_next(_pc, t);
		} else if (pparse_match_ch(t, ':')) {
			/* C23 enum fixed underlying type: enum E : int { ... }
			 * Also `enum E : typeof(unsigned)` / `_BitInt(N)` / `_Alignas`. */
			t = pparse_next(_pc, t);
			while (t && t->kind != PPARSE_TK_EOF) {
				pparse_SKIP_NOISE_CONTINUE(t);
				if ((t->tag & (PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS | PPARSE_TT_ATTR)) &&
				    pparse_next(_pc, t) && pparse_match_ch(pparse_next(_pc, t), '(')) {
					t = pparse_next(_pc, pparse_pair_known(pparse_next(_pc, t)));
					continue;
				}
				/* `enum E : _Atomic(int) {` */
				if ((t->tag & PPARSE_TT_TYPE) && pparse_equal(t, "_Atomic") && pparse_next(_pc, t) &&
				    pparse_match_ch(pparse_next(_pc, t), '(')) {
					t = pparse_next(_pc, pparse_pair_known(pparse_next(_pc, t)));
					continue;
				}
				if (pparse_is_c23_attr(t)) {
					t = pparse_next(_pc, pparse_pair_known(t));
					continue;
				}
				if ((t->tag & PPARSE_TT_QUALIFIER) || pparse_is_type_keyword(t) || pparse_is_known_typedef(t)) {
					t = pparse_next(_pc, t);
					continue;
				}
				if (pparse_match_ch(t, '*')) {
					t = pparse_next(_pc, t);
					continue;
				}
				break;
			}
		} else
			break;
	}
	if (t && pparse_match_ch(t, '{')) {
		pparse_ann(tok) |= P1_SUE_BODY_RECIPE;
		tok->pair_idx = pparse_idx(_pc, t);
		return t;
	}
	return NULL;
}

static inline PParseToken *pparse_ann_pair_recipe(PParseToken *tok, uint32_t mask) {
	PPARSE_CTX();
	return tok && (pparse_ann(tok) & mask) == mask && tok->pair_idx
		   ? &pparse_token_pool[tok->pair_idx]
		   : NULL;
}

static inline PParseToken *pparse_sue_body_recipe(PParseToken *tok) {
	return pparse_ann_pair_recipe(tok, P1_SUE_BODY_RECIPE);
}

/* Shared backward walker; PPARSE_WB_* selects origin, noise and group jumps. */
static PParseToken *pparse_walk_back(uint32_t start_idx, unsigned flags) {
	PPARSE_CTX();
	/* Both conventions refuse to start at pool[0] as a candidate: FROM_PRED
	 * with start_idx==0 has no predecessor; skip_* with start_idx==0 matches
	 * the old `for (pi = 0; pi > 0;)` no-op. */
	if (start_idx == 0) return NULL;
	/* Loop invariant: pi is one past the next candidate (candidate = pi-1). */
	uint32_t pi = (flags & PPARSE_WB_FROM_PRED) ? start_idx : start_idx + 1;
	for (; pi > 0;) {
		pi--;
		PParseToken *pt = &pparse_token_pool[pi];

		if ((flags & PPARSE_WB_SKIP_PREP) && pt->kind == PPARSE_TK_PREP_DIR) continue;
		if ((flags & PPARSE_WB_SKIP_ATTR) && (pt->tag & PPARSE_TT_ATTR)) continue;

		if (flags & PPARSE_WB_JUMP_GROUPS) {
			if (pt->flags & PPARSE_TF_CLOSE) {
				pi = pparse_idx(_pc, pparse_pair_known(pt)); /* next iter looks at open-1 */
				continue;
			}
			return pt;
		}

		if ((flags & PPARSE_WB_JUMP_C23_ATTR) && pparse_match_ch(pt, ']') &&
		    (pparse_pair_known(pt)->flags & PPARSE_TF_C23_ATTR)) {
			pi = pparse_idx(_pc, pparse_pair_known(pt));
			continue;
		}

		if (pparse_match_ch(pt, ')') &&
		    (flags & (PPARSE_WB_JUMP_ALL_PARENS | PPARSE_WB_JUMP_ATTR_PARENS))) {
			uint32_t open_idx = pparse_idx(_pc, pparse_pair_known(pt));
			if (flags & PPARSE_WB_JUMP_ALL_PARENS) {
				pi = open_idx;
				continue;
			}
			/* PPARSE_WB_JUMP_ATTR_PARENS: jump only attr-bearing parens */
			for (uint32_t bi = open_idx; bi > 0;) {
				bi--;
				PParseToken *bt = &pparse_token_pool[bi];
				if ((flags & PPARSE_WB_SKIP_PREP) && bt->kind == PPARSE_TK_PREP_DIR) continue;
				if (bt->tag & PPARSE_TT_ATTR) {
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

static bool pparse_expr_is_aggregate_value(PParseToken *start, PParseToken *end) {
	PPARSE_CTX();
	if (!start || start == end) return false;
	PParseToken *t = start, *limit = end;
	while (pparse_match_ch(t, '(')) {
		PParseToken *close = pparse_pair_known(t);
		if (pparse_next(_pc, close) != limit) break;
		limit = close;
		t = pparse_next(_pc, t);
		if (!t || t == limit) return false;
	}
	if (t->kind == PPARSE_TK_IDENT && pparse_next(_pc, t) == limit) {
		PParseTypedefEntry *entry = pparse_typedef_lookup(_pc, t);
		return entry && entry->is_shadow && entry->is_aggregate && !entry->is_array;
	}
	if (t->kind == PPARSE_TK_IDENT) {
		PParseToken *open = pparse_next(_pc, t);
		PParseToken *close = open && pparse_match_ch(open, '(') ? pparse_pair_known(open) : NULL;
		if (close && pparse_next(_pc, close) == limit)
			return pparse_function_symbol(t) == PPARSE_FS_AGGREGATE_RETURN;
	}
	if (pparse_match_ch(t, '(')) {
		PParseToken *inner = pparse_skip_noise(_pc, pparse_next(_pc, t));
		if (!inner) return false;
		if (inner->tag & PPARSE_TT_SUE) return !pparse_is_enum_kw(inner);
		if (pparse_typedef_flags(inner) & PPARSE_TDF_AGGREGATE) return true;
	}
	return false;
}

static PParseToken *pparse_skip_function_attrs_and_cc(PParseToken *tok) {
	PPARSE_CTX();
	while (tok && tok->kind != PPARSE_TK_EOF) {
		PPARSE_SKIP_NOISE_RESTART(tok);
		if (tok->flags & PPARSE_TF_MS_CC) {
			tok = pparse_next(_pc, tok);
			continue;
		}
		break;
	}
	return tok;
}

/* Pointer stars and their qualifiers are return-type material. Function
 * attributes are intentionally not skipped here: they terminate that range. */
static PParseToken *pparse_skip_return_pointers(PParseToken *tok, bool *is_void) {
	PPARSE_CTX();
	while (tok && tok->kind != PPARSE_TK_EOF) {
		if (tok->kind == PPARSE_TK_PREP_DIR) {
			tok = pparse_next(_pc, tok);
			continue;
		}
		if (!pparse_match_ch(tok, '*') &&
		    !((tok->tag & PPARSE_TT_QUALIFIER) && !(tok->tag & PPARSE_TT_ATTR) &&
		      !pparse_is_soft_keyword_identifier(tok)))
			break;
		tok = pparse_next(_pc, tok);
		if (is_void) *is_void = false;
	}
	return tok;
}

static PParseToken *pparse_return_type_end_before_attrs(PParseToken *start, PParseToken *parsed_end) {
	PPARSE_CTX();
	for (PParseToken *t = start; t && t != parsed_end && t->kind != PPARSE_TK_EOF;) {
		if ((t->tag & PPARSE_TT_ATTR) || pparse_is_c23_attr(t) || (t->flags & PPARSE_TF_MS_CC)) {
			return pparse_skip_function_attrs_and_cc(t) == parsed_end ? t : parsed_end;
		}
		if (t->flags & PPARSE_TF_OPEN) t = pparse_pair_known(t);
		t = pparse_next(_pc, t);
	}
	return parsed_end;
}
static PParseFunctionReturn pparse_function_return(PParseToken *tok) {
	PPARSE_CTX();
	PParseFunctionReturn result = {0};
	while (tok && tok->kind != PPARSE_TK_EOF) {
		if (tok->tag & (PPARSE_TT_SKIP_DECL | PPARSE_TT_INLINE)) {
			tok = pparse_next(_pc, tok);
			continue;
		}
		/* `raw` suppresses initialization; it is not return-type material. */
		if ((tok->flags & PPARSE_TF_RAW) && !pparse_is_known_typedef(tok)) {
			PParseToken *after = pparse_skip_noise(_pc, pparse_next(_pc, tok));
			if (pparse_is_raw_declaration_context(tok, after)) {
				tok = after;
				continue;
			}
		}
		PParseToken *next = pparse_skip_noise(_pc, tok);
		if (next == tok) break;
		tok = next;
	}
	if (!tok || tok->kind == PPARSE_TK_EOF) return result;
	PParseToken *type_start = tok;
	PParseTypeSpec type = pparse_type_specifier(tok);
	if (!type.saw_type) return result;
	bool is_void = type.has_void;
	PParseToken *trimmed = pparse_return_type_end_before_attrs(type_start, type.end);
	if (type.is_struct) {
		PPARSE_FOR_RANGE(t, type_start, trimmed)
			if (pparse_match_ch(t, '{')) return result;
	}

	tok = pparse_skip_return_pointers(trimmed, &is_void);
	PParseToken *ret_end = tok;
	tok = pparse_skip_function_attrs_and_cc(tok);
	if (tok && pparse_is_valid_varname(tok) && pparse_next(_pc, tok) && pparse_match_ch(pparse_next(_pc, tok), '(')) {
		result.kind = is_void ? PPARSE_FUNC_RETURN_VOID : PPARSE_FUNC_RETURN_VALUE;
		if (!is_void) {
			result.type_start = type_start;
			result.type_end = ret_end;
		}
		return result;
	}

	if (!tok || !pparse_match_ch(tok, '(')) return result;
	PParseToken *outer_open = tok;
	PParseToken *inner = pparse_skip_return_pointers(pparse_next(_pc, tok), &is_void);
	inner = pparse_skip_function_attrs_and_cc(inner);
	while (inner && pparse_match_ch(inner, '(')) {
		inner = pparse_skip_return_pointers(pparse_next(_pc, inner), NULL);
		inner = pparse_skip_function_attrs_and_cc(inner);
	}
	if (!inner || !pparse_is_valid_varname(inner) || !pparse_next(_pc, inner)) return result;
	if (pparse_match_ch(pparse_next(_pc, inner), '(')) {
		PParseToken *after_params = pparse_skip_balanced_group(pparse_next(_pc, inner));
		if (after_params && pparse_match_ch(after_params, ')')) {
			PParseToken *decl_end = pparse_skip_balanced_group(outer_open);
			while (decl_end && (decl_end->flags & PPARSE_TF_OPEN) && !pparse_match_ch(decl_end, '{') &&
			       !pparse_match_ch(decl_end, '(') && !(decl_end->flags & PPARSE_TF_C23_ATTR))
				decl_end = pparse_skip_balanced_group(decl_end);
			result.kind = is_void ? PPARSE_FUNC_RETURN_VOID : PPARSE_FUNC_RETURN_VALUE;
			if (!is_void) {
				result.type_start = type_start;
				result.type_end = inner;
				result.suffix_start = after_params;
				result.suffix_end = decl_end;
			}
			return result;
		}
	} else if (pparse_next(_pc, inner) == pparse_pair_known(outer_open)) {
		PParseToken *params = pparse_next(_pc, pparse_next(_pc, inner));
		if (params && pparse_match_ch(params, '(')) {
			result.kind = is_void ? PPARSE_FUNC_RETURN_VOID : PPARSE_FUNC_RETURN_VALUE;
			if (!is_void) {
				result.type_start = type_start;
				result.type_end = inner;
			}
		}
	}
	return result;
}

/* Reusable classification for an array bracket's surrounding C grammar.
 * The result lives in the otherwise-unused parse_data word of an ordinary
 * '[' token. Stream-splice tokens retain their jump target and simply take
 * the uncached path. */
enum {
	PPARSE_BRACKET_COMPOUND_LITERAL = 1u << 0,
	PPARSE_BRACKET_DESIGNATOR = 1u << 1,
	PPARSE_BRACKET_GNU_RANGE = 1u << 2,
	PPARSE_BRACKET_ALIGNOF_TYPE = 1u << 3,
	PPARSE_BRACKET_GENERIC_ASSOC_TYPE = 1u << 4,
	PPARSE_BRACKET_CONTEXT_KNOWN = INT_MIN,
};


static bool pparse_expr_maybe_nonconstant(PParseToken *start, PParseToken *end) {
	PPARSE_CTX();
	PPARSE_FOR_RANGE(t, start, end) {
		PPARSE_SKIP_GROUP_ON_CLOSE(t)
		if (t->kind != PPARSE_TK_IDENT) continue;
		if (pparse_is_type_keyword(t) ||
		    (t->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_ATTR | PPARSE_TT_STORAGE |
			       PPARSE_TT_ALIGNAS | PPARSE_TT_BITINT)))
			continue;
		if (pparse_typedef_lookup(_pc, t)) continue;
		return true;
	}
	return false;
}

static PParseToken *pparse_last_comma_operand(PParseToken *open_paren, PParseToken *close_paren) {
	PPARSE_CTX();
	PParseToken *segment = pparse_next(_pc, open_paren);
	PPARSE_FOR_RANGE(t, segment, close_paren) {
		PPARSE_SKIP_GROUP_ON_CLOSE(t)
		if (pparse_match_ch(t, ',')) segment = pparse_next(_pc, t);
	}
	return segment;
}

static inline bool pparse_is_value_name_token(PParseToken *t) {
	/* Soft type spellings (`_Float32`, `bool`, …) remain valid declarator
	 * names after an established type. */
	return t && pparse_is_valid_varname(t) &&
	       (!(t->tag & PPARSE_TT_TYPE) || pparse_is_soft_keyword_identifier(t)) && !pparse_is_known_typedef(t);
}

static inline bool pparse_is_unevaluated_operand_intro(PParseToken *t) {
	return (t->flags & PPARSE_TF_SIZEOF) || (t->tag & PPARSE_TT_TYPEOF);
}

static void pparse_tag_brackets_in_range(PParseToken *open, PParseToken *close) {
	PPARSE_CTX();
	PPARSE_FOR_RANGE(t, pparse_next(_pc, open), close)
		if (t->ch0 == '[' && (t->flags & PPARSE_TF_OPEN)) pparse_ann(t) |= P1_UNEVAL_BRACKET;
}

static void pparse_tag_postfix_chain_unevaluated(PParseToken *t) {
	PPARSE_CTX();
	while (t && t->kind != PPARSE_TK_EOF) {
		if ((pparse_match_ch(t, '[') || pparse_match_ch(t, '(')) && (t->flags & PPARSE_TF_OPEN)) {
			if (pparse_match_ch(t, '[')) pparse_ann(t) |= P1_UNEVAL_BRACKET;
			PParseToken *close = pparse_pair_known(t);
			pparse_tag_brackets_in_range(t, close);
			t = pparse_next(_pc, close);
			continue;
		}
		if (pparse_match_ch(t, '.') || pparse_equal(t, "->")) {
			t = pparse_next(_pc, t);
			if (pparse_is_value_name_token(t)) t = pparse_next(_pc, t);
			continue;
		}
		if (pparse_equal(t, "++") || pparse_equal(t, "--")) {
			t = pparse_next(_pc, t);
			continue;
		}
		break;
	}
}

/* Mark brackets suppressed by one core-C unevaluated construct and return the
 * last token already covered, so the dedicated sweep skips nested ranges. */
static PParseToken *pparse_mark_unevaluated_at(PParseToken *t) {
	PPARSE_CTX();
	PParseToken *open = pparse_next(_pc, t);
	if (t->flags & PPARSE_TF_STATIC_ASSERT) {
		if (!open || open->ch0 != '(' || !(open->flags & PPARSE_TF_OPEN)) return NULL;
		PParseToken *close = pparse_pair_known(open);
		pparse_tag_brackets_in_range(open, close);
		return close;
	}
	if (t->tag & PPARSE_TT_GENERIC) {
		if (!open || open->ch0 != '(' || !(open->flags & PPARSE_TF_OPEN)) return NULL;
		PParseToken *close = pparse_pair_known(open);
		PParseToken *comma = pparse_skip_to_set(
		    pparse_next(_pc, open), close, pparse_CH(','));
		pparse_tag_brackets_in_range(open, comma);
		return comma;
	}
	if (!open) return NULL;
	if (open->ch0 == '(' && (open->flags & PPARSE_TF_OPEN)) {
		PParseToken *close = pparse_pair_known(open);
		pparse_tag_brackets_in_range(open, close);
		pparse_tag_postfix_chain_unevaluated(pparse_next(_pc, close));
		return close;
	}
	while (open && open->kind != PPARSE_TK_EOF &&
	       (pparse_match_set(open, pparse_CH('+') | pparse_CH('-') | pparse_CH('!') | pparse_CH('&') | pparse_CH('*')) ||
		pparse_match_ch(open, '~') || pparse_equal(open, "++") || pparse_equal(open, "--")))
		open = pparse_next(_pc, open);
	if (pparse_is_value_name_token(open)) pparse_tag_postfix_chain_unevaluated(pparse_next(_pc, open));
	return NULL;
}
static bool pparse_bounds_is_tracked_array(PParseToken *tok) {
	return pparse_is_value_name_token(tok) && pparse_array_binding_info(tok).tracked;
}

/* Peel a parenthesized primary. The commutative-index path asks for every
 * redundant layer; the base-expression path stops after one and preserves a
 * call's argument list. */
static PParseToken *pparse_bounds_peel_primary(PParseToken *last, bool nested) {
	PPARSE_CTX();
	PParseToken *t = last;
	for (;;) {
		if (!t || !pparse_match_ch(t, ')')) return t;
		PParseToken *open = pparse_pair_known(t);
		if (!nested && pparse_idx(_pc, open) >= 1) {
			PParseToken *before = &pparse_token_pool[pparse_idx(_pc, open) - 1];
			if (pparse_is_value_name_token(before) || before->kind == PPARSE_TK_NUM ||
			    pparse_match_ch(before, ')') || pparse_match_ch(before, ']'))
				return t;
		}
		PParseToken *inner = pparse_next(_pc, open);
		if (!inner) return t;
		if ((pparse_is_value_name_token(inner) || inner->kind == PPARSE_TK_NUM) && pparse_next(_pc, inner) == t) {
			t = inner;
			if (nested) continue;
			return t;
		}
		/* Nested `((i))`: whole body is one paren group. */
		if (nested && pparse_match_ch(inner, '(') && (inner->flags & PPARSE_TF_OPEN) &&
		    pparse_next(_pc, pparse_pair_known(inner)) == t) {
			t = pparse_pair_known(inner);
			continue;
		}
		return t;
	}
}

static bool pparse_bounds_skip_unevaluated_group(PParseToken **tok) {
	PPARSE_CTX();
	PParseToken *t = *tok;
	if (!(t->flags & PPARSE_TF_OPEN) || !pparse_match_ch(t, '('))
		return false;
	PParseToken *prev = &pparse_token_pool[pparse_idx(_pc, t) - 1];
	if (!pparse_is_unevaluated_operand_intro(prev)) return false;
	*tok = pparse_pair_known(t);
	return true;
}

static PParseToken *pparse_bounds_find_tracked_array(PParseToken *start, PParseToken *end) {
	PPARSE_CTX();
	PPARSE_FOR_RANGE(t, start, end) {
		if (pparse_bounds_skip_unevaluated_group(&t)) continue;
		if (!pparse_is_value_name_token(t) || pparse_is_known_typedef(t)) continue;
		if (pparse_token_pool[pparse_idx(_pc, t) - 1].tag & PPARSE_TT_MEMBER) continue;
		if (pparse_bounds_is_tracked_array(t)) return t;
	}
	return NULL;
}

static bool pparse_bounds_span_derives_array(PParseToken *first, PParseToken *close) {
	PPARSE_CTX();
	if (!first || !close || first == close) return false;
	PPARSE_FOR_RANGE(t, first, close) {
		if (pparse_bounds_skip_unevaluated_group(&t)) continue;
		if ((pparse_match_ch(t, '&') || pparse_match_ch(t, '*')) && !(t->flags & PPARSE_TF_OPEN)) {
			PParseToken *next = pparse_next(_pc, t);
			while (next && next != close && pparse_match_ch(next, '(') &&
			       (next->flags & PPARSE_TF_OPEN)) {
				PParseToken *inner = pparse_next(_pc, next), *ic = pparse_pair_known(next);
				if (!inner || inner == close) break;
				/* `(name)` -- bare, or `(name[...][...]...)` -- name with a
				 * trailing subscript chain that fills the rest of this
				 * paren exactly. Unparenthesized `&a[0]` is already caught
				 * below by the plain next-token check; without this, only
				 * wrapping the *whole* address-of (`(&a[0])`) was
				 * recognized and `&(a[0])` -- parens around just the
				 * element access -- silently slipped past as a "no derived
				 * form here" span, a full bypass of the commutative-
				 * subscript rejection for that one paren placement. */
				if (pparse_is_value_name_token(inner)) {
					PParseToken *after = pparse_next(_pc, inner);
					while (after && after != ic && pparse_match_ch(after, '[') &&
					       (after->flags & PPARSE_TF_OPEN))
						after = pparse_next(_pc, pparse_pair_known(after));
					if (after == ic) {
						next = inner;
						break;
					}
				}
				if (!pparse_match_ch(inner, '(')) break;
				next = inner;
			}
			if (next && next != close && pparse_is_value_name_token(next) &&
			    pparse_bounds_is_tracked_array(next))
				return true;
		}
	}
	return false;
}

static bool pparse_bounds_peel_redundant_parens(PParseToken **lhs, PParseToken **scan_end) {
	PPARSE_CTX();
	while (*lhs && pparse_match_ch(*lhs, '(') && ((*lhs)->flags & PPARSE_TF_OPEN)) {
		PParseToken *inner_close = pparse_pair_known(*lhs);
		if (pparse_next(_pc, inner_close) != *scan_end) break;
		*lhs = pparse_next(_pc, *lhs);
		*scan_end = inner_close;
		if (!*lhs || *lhs == *scan_end) return false;
	}
	return *lhs && *lhs != *scan_end;
}

static bool pparse_bounds_group_is_cast(PParseToken *open) {
	PPARSE_CTX();
	PParseToken *fi = open ? pparse_next(_pc, open) : NULL;
	return fi && (pparse_is_type_keyword(fi) ||
		      (fi->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF)));
}

static bool pparse_bounds_span_has_array_arithmetic(PParseToken *lhs, PParseToken *scan_end) {
	PPARSE_CTX();
	if (!lhs || lhs == scan_end) return false;
	PPARSE_FOR_RANGE(t, lhs, scan_end) {
		PPARSE_SKIP_GROUP_ON_CLOSE(t)
		if ((pparse_match_ch(t, '+') || pparse_match_ch(t, '-')) && !(t->flags & PPARSE_TF_OPEN))
			return pparse_bounds_find_tracked_array(lhs, scan_end) != NULL;
	}
	return false;
}

static bool pparse_bounds_paren_has_array_arithmetic(PParseToken *open) {
	PPARSE_CTX();
	if (!open || !pparse_match_ch(open, '(') || !(open->flags & PPARSE_TF_OPEN)) return false;
	PParseToken *scan_end = pparse_pair_known(open);
	PParseToken *lhs = pparse_next(_pc, open);
	if (!pparse_bounds_peel_redundant_parens(&lhs, &scan_end)) return false;
	return pparse_bounds_span_has_array_arithmetic(lhs, scan_end);
}

/* Forward-declared: definition sits near its other P1D control-flow callers
 * below, but the unary-`*`/`&` disambiguation here needs it first. A control
 * statement's condition `)` (if/while/for/switch, not else/do) is followed by
 * a new statement, not a value -- it must not be mistaken for a value-close
 * paren the way a cast or a parenthesized subexpression is. */
static PParseToken *pparse_ctrl_condition_kw_before_paren(PParseToken *open);

static bool pparse_bounds_deref_add_is_unverifiable(PParseToken *tok) {
	PPARSE_CTX();
	if (!pparse_feat(PPARSE_F_BOUNDS_CHECK)) return false;
	if (!pparse_match_ch(tok, '*') || (tok->flags & PPARSE_TF_OPEN)) return false;
	if (pparse_token_is_in_unevaluated_operand(tok)) return false;
	PParseToken *prev = &pparse_token_pool[pparse_idx(_pc, tok) - 1];
	if (prev->kind == PPARSE_TK_NUM || prev->kind == PPARSE_TK_STR) return false;
	if (prev->kind == PPARSE_TK_IDENT &&
	    !(prev->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF)) &&
	    !(prev->tag & (PPARSE_TT_RETURN | PPARSE_TT_GOTO | PPARSE_TT_DEFER)))
		return false;
	if (pparse_match_ch(prev, ']')) return false;
	if (pparse_match_ch(prev, ')') && (prev->flags & PPARSE_TF_CLOSE)) {
		PParseToken *popen = pparse_pair_known(prev);
		/* A control statement's condition close (`if (c) *(a+i) = 0;`)
		 * is not a value: the `)` ends `if (c)`, not an operand of `*`.
		 * Treating it like one let this exact pattern silently skip the
		 * pointer-arithmetic-dereference rejection below. */
		if (!pparse_bounds_group_is_cast(popen) && !pparse_ctrl_condition_kw_before_paren(popen))
			return false;
	}

	PParseToken *op = pparse_next(_pc, tok);
	if (!op || !pparse_match_ch(op, '(') || !(op->flags & PPARSE_TF_OPEN)) return false;
	while (pparse_bounds_group_is_cast(op)) {
		PParseToken *after = pparse_next(_pc, pparse_pair_known(op));
		if (!after || !pparse_match_ch(after, '(') || !(after->flags & PPARSE_TF_OPEN))
			return false;
		op = after;
	}
	PParseToken *scan_end = pparse_pair_known(op);
	PParseToken *lhs = pparse_next(_pc, op);
	if (!pparse_bounds_peel_redundant_parens(&lhs, &scan_end)) return false;
	while (lhs && pparse_match_ch(lhs, '(') && (lhs->flags & PPARSE_TF_OPEN) &&
	       pparse_bounds_group_is_cast(lhs)) {
		PParseToken *after = pparse_next(_pc, pparse_pair_known(lhs));
		if (!after || !pparse_match_ch(after, '(') || !(after->flags & PPARSE_TF_OPEN))
			return false;
		PParseToken *add_close = pparse_pair_known(after);
		if (pparse_next(_pc, add_close) != scan_end) break;
		lhs = pparse_next(_pc, after);
		scan_end = add_close;
		if (!lhs || lhs == scan_end) return false;
	}
	return pparse_bounds_span_has_array_arithmetic(lhs, scan_end);
}

static PParseToken *pparse_ctrl_condition_kw_before_paren(PParseToken *open) {
	PPARSE_CTX();
	if (!open || !pparse_match_ch(open, '(')) return NULL;
	PParseToken *kw = pparse_walk_back(pparse_idx(_pc, open), PPARSE_WB_ATTR_NOISE);
	if (kw && (kw->tag & (PPARSE_TT_IF | PPARSE_TT_LOOP | PPARSE_TT_SWITCH)) && !pparse_is_else_or_do(kw)) return kw;
	return NULL;
}

static bool pparse_close_brace_ends_sue_body(PParseToken *tok) {
	PPARSE_CTX();
	if (!tok || !pparse_match_ch(tok, '}')) return false;
	PParseToken *open = pparse_pair_known(tok);
	for (uint32_t ti = pparse_idx(_pc, open); ti > 1;) {
		PParseToken *t = &pparse_token_pool[ti - 1];
		if (t->kind == PPARSE_TK_PREP_DIR) {
			ti--;
			continue;
		}
		PParseToken *group_open = (pparse_match_ch(t, ']') || pparse_match_ch(t, ')'))
						 ? pparse_pair_known(t)
						 : NULL;
		if (group_open && pparse_match_ch(t, ']') && (group_open->flags & PPARSE_TF_C23_ATTR)) {
			ti = pparse_idx(_pc, group_open);
			continue;
		}
		if (group_open && pparse_match_ch(t, ')')) {
			uint32_t open_idx = pparse_idx(_pc, group_open);
			PParseToken *before = open_idx > 1 ? &pparse_token_pool[open_idx - 1] : NULL;
			if (before && (before->tag & (PPARSE_TT_ATTR | PPARSE_TT_ALIGNAS | PPARSE_TT_BITINT | PPARSE_TT_TYPEOF))) {
				ti = pparse_idx(_pc, before);
				continue;
			}
			return false;
		}
		if (t->tag & PPARSE_TT_SUE) return true;
		if (pparse_match_ch(t, ';') || pparse_match_ch(t, '=') || pparse_match_ch(t, ',') || pparse_match_ch(t, '{') ||
		    pparse_match_ch(t, '}'))
			return false;
		ti--;
	}
	return false;
}
static bool pparse_token_ends_sue_type_specifier(PParseToken *tok) {
	PPARSE_CTX();
	if (!tok) return false;
	if (pparse_close_brace_ends_sue_body(tok)) return true;
	PParseToken *effective = pparse_walk_back(pparse_idx(_pc, tok) + 1, PPARSE_WB_PAST_NOISE);
	if (effective && effective != tok) return pparse_token_ends_sue_type_specifier(effective);
	if (tok->tag & PPARSE_TT_SUE) return true;
	if (pparse_is_identifier_like(tok)) {
		PParseToken *before = pparse_walk_back(pparse_idx(_pc, tok), PPARSE_WB_PAST_NOISE);
		return before && (before->tag & PPARSE_TT_SUE);
	}
	return false;
}

static inline bool pparse_raw_token_is_sue_tag_name(PParseToken *tok) {
	PPARSE_CTX();
	if (!tok || !(tok->flags & PPARSE_TF_RAW)) return false;
	PParseToken *prev = pparse_walk_back(pparse_idx(_pc, tok), PPARSE_WB_PAST_NOISE);
	return (prev && (prev->tag & PPARSE_TT_SUE)) || pparse_token_ends_sue_type_specifier(tok);
}

static bool pparse_close_paren_ends_cast_type_name(PParseToken *tok) {
	PPARSE_CTX();
	if (!tok || !pparse_match_ch(tok, ')')) return false;
	PParseToken *open = pparse_pair_known(tok);
	PParseToken *before_open = pparse_walk_back(pparse_idx(_pc, open), PPARSE_WB_PAST_NOISE);
	if (before_open &&
	    (pparse_is_sizeof_like(before_open) || (before_open->tag & (PPARSE_TT_TYPEOF | PPARSE_TT_ALIGNAS | PPARSE_TT_BITINT))))
		return false;
	PParseToken *inner = pparse_skip_noise(_pc, pparse_next(_pc, open));
	if (!inner || inner == tok) return false;
	PParseTypeSpec type = pparse_type_specifier(inner);
	if (!type.saw_type) return false;
	PParseToken *t = type.end;
	while (t && t != tok && t->kind != PPARSE_TK_EOF) {
		PPARSE_SKIP_NOISE_RESTART(t);
		if (pparse_match_ch(t, '*') || (t->tag & PPARSE_TT_QUALIFIER)) {
			t = pparse_next(_pc, t);
			continue;
		}
		PParseToken *close = pparse_match_ch(t, '(') || pparse_match_ch(t, '[') ? pparse_pair_known(t) : NULL;
		if (close) {
			t = pparse_next(_pc, close);
			continue;
		}
		return false;
	}
	return t == tok;
}

typedef struct {
	PParseToken *arr;	 /* array name the bound is taken from   */
	PParseToken *close;	 /* ']' closing the subscript            */
	PParseToken *cast_close; /* ')' of outermost (T *) cast, or NULL */
	int dim_depth;		 /* preceding [i] dimensions already peeled */
} PParseBoundsPlan;

typedef enum {
	PPARSE_AR_TYPE,
	PPARSE_AR_DECL,
	PPARSE_AR_BARE_ORELSE,
	PPARSE_AR_BOUNDS,
} PParseAnalysisKind;

typedef struct {
	PParseAnalysisKind kind;
	uint32_t next;
	union {
		PParseTypeSpec type;
		PParseDecl decl;
		PParseBoundsPlan bounds;
		uint32_t token_idx;
	} as;
} PParseAnalysisRecord;

static PParseAnalysisRecord *pparse_analysis_get(PParseToken *tok, PParseAnalysisKind kind) {
	PPARSE_CTX();
	if (!tok || !_pc->analysis_index) return NULL;
	uint32_t ri = _pc->analysis_index[pparse_idx(_pc, tok)];
	PParseAnalysisRecord *records = _pc->analysis_records;
	while (ri) {
		PParseAnalysisRecord *r = &records[ri - 1];
		if (r->kind == kind) return r;
		ri = r->next;
	}
	return NULL;
}

static PParseAnalysisRecord *pparse_analysis_add(PParseToken *tok, PParseAnalysisKind kind) {
	PPARSE_CTX();
	if (!_pc->analysis_index)
		_pc->analysis_index = pparse_arena_alloc(
		    &_pc->main_arena, (size_t)pparse_token_count * sizeof(*_pc->analysis_index));
	PParseAnalysisRecord *records = _pc->analysis_records;
	PPARSE_ARENA_ENSURE_CAP(
	    &_pc->main_arena, records, _pc->analysis_count, _pc->analysis_cap, 64, PParseAnalysisRecord);
	_pc->analysis_records = records;
	PParseAnalysisRecord *r = &records[_pc->analysis_count];
	uint32_t ti = pparse_idx(_pc, tok);
	*r = (PParseAnalysisRecord){.kind = kind, .next = _pc->analysis_index[ti]};
	_pc->analysis_index[ti] = ++_pc->analysis_count;
	return r;
}

/* A warn-mode diagnostic answers "no plan"; an error-mode one does not return. */
#define PPARSE_BOUNDS_DIAG(t, msg)                                                                   \
	do {                                                                                         \
		if (pparse_feat(PPARSE_F_WARN_SAFETY)) {                                             \
			pparse_warn_tok((t), msg);                                                   \
			return false;                                                                \
		}                                                                                    \
		pparse_error_tok((t), msg);                                                          \
	} while (0)

/* Does this token end a value, so that a following `&` or `*` is the binary
 * operator rather than the unary one? Three bounds sites asked this question
 * with three separately maintained copies of the list, and every copy that
 * fell behind produced either a silent unchecked subscript or a spurious trap
 * on correct C. One list, one place to add to. A control statement's condition
 * close is deliberately not a value: in `if (c) &a[len];` the `)` ends the
 * `if`, so nothing carries into the statement that follows it. */
static bool pparse_bounds_tok_ends_value(PParseToken *pp) {
	PPARSE_CTX();
	if (!pp) return false;
	if (pparse_is_value_name_token(pp) || pp->kind == PPARSE_TK_NUM || pp->kind == PPARSE_TK_STR)
		return true;
	if (pparse_match_ch(pp, ']') || pparse_match_ch(pp, '}')) return true;
	if (pparse_equal(pp, "++") || pparse_equal(pp, "--")) return true;
	if (pparse_match_ch(pp, ')') && !pparse_close_paren_ends_cast_type_name(pp) &&
	    !pparse_ctrl_condition_kw_before_paren(pparse_pair_known(pp)))
		return true;
	return false;
}

static bool pparse_bounds_plan_subscript(PParseToken *tok,
					 PParseToken *last_emitted,
					 PParseBoundsPlan *out) {
	PPARSE_CTX();
	if (!pparse_feat(PPARSE_F_BOUNDS_CHECK)) return false;
	if (!pparse_match_ch(tok, '[') || !(tok->flags & PPARSE_TF_OPEN)) return false;
	PParseToken *rb = pparse_pair_known(tok);
	if (tok->flags & PPARSE_TF_C23_ATTR) return false;
	if (pparse_ann(tok) & (P1_DECL_BRACKET | P1_UNEVAL_BRACKET)) return false;
	{
		PParseToken *idx0 = pparse_next(_pc, tok);
		if (idx0->kind == PPARSE_TK_IDENT && idx0->len == 12 &&
		    prism_memeq_static(pparse_loc(_pc, idx0), "__prism_bchk", 12))
			return false;
	}
	if (!last_emitted) return false;
	uint32_t ti = pparse_idx(_pc, tok);
	PParseToken *rp_prev = (ti >= 1) ? &pparse_token_pool[ti - 1] : NULL;
	if (rp_prev && pparse_match_ch(rp_prev, ')')) {
		PParseToken *rp = rp_prev;
		PParseToken *op = pparse_pair_known(rp);
		{
			if (pparse_bounds_span_derives_array(pparse_next(_pc, op), rp))
				PPARSE_BOUNDS_DIAG(tok, PPARSE_ERR_BOUNDS_DERIVED_SUB);
			if (pparse_bounds_paren_has_array_arithmetic(op))
				PPARSE_BOUNDS_DIAG(tok, PPARSE_ERR_BOUNDS_PTR_ARITH_SUB);
			PParseToken *idx = pparse_next(_pc, op);
			if (pparse_is_value_name_token(idx) && pparse_next(_pc, idx) == rp) {
				PParseToken *inner = pparse_next(_pc, tok);
				if (pparse_next(_pc, inner) == rb &&
				    pparse_bounds_is_tracked_array(inner) && !pparse_bounds_is_tracked_array(idx))
					PPARSE_BOUNDS_DIAG(tok, PPARSE_ERR_BOUNDS_COMM_IDX_ARR);
			}
		}
	}

	{
		PParseToken *inner_first = pparse_next(_pc, tok);
		if (pparse_bounds_span_derives_array(inner_first, rb))
			PPARSE_BOUNDS_DIAG(tok, PPARSE_ERR_BOUNDS_COMM_DERIVED);
	}

	bool comma_resolved = false;
	{
		PParseToken *inner = pparse_next(_pc, tok);
		PParseToken *iclose = rb;
		if (pparse_match_ch(inner, '(') && (inner->flags & PPARSE_TF_OPEN)) {
			PParseToken *pclose = pparse_pair_known(inner);
			{
				PParseToken *lastop = pparse_last_comma_operand(inner, pclose);
				if (lastop != pparse_next(_pc, inner) && pparse_next(_pc, lastop) == pclose) {
					inner = lastop;
					iclose = pclose;
					if (pparse_bounds_is_tracked_array(lastop)) comma_resolved = true;
				}
			}
		}
		if (!comma_resolved) {
			bool le_is_member =
			    pparse_idx(_pc, last_emitted) >= 1 &&
			    (pparse_token_pool[pparse_idx(_pc, last_emitted) - 1].tag & PPARSE_TT_MEMBER);
			if (!le_is_member) {
				while (inner != iclose && pparse_match_ch(inner, '(') && (inner->flags & PPARSE_TF_OPEN) &&
				       pparse_next(_pc, pparse_pair_known(inner)) == iclose) {
					iclose = pparse_pair_known(inner);
					inner = pparse_next(_pc, inner);
				}
				if (inner != iclose && pparse_next(_pc, inner) == iclose &&
				    pparse_bounds_is_tracked_array(inner)) {
					PParseToken *le = pparse_bounds_peel_primary(last_emitted, false);
					PParseTypedefEntry *binding = pparse_is_value_name_token(le)
								       ? pparse_typedef_lookup(_pc, le)
								       : NULL;
					if (!pparse_bounds_is_tracked_array(le) &&
					    !(binding && !binding->is_struct_tag && binding->is_ptr))
						PPARSE_BOUNDS_DIAG(tok, PPARSE_ERR_BOUNDS_COMM_IDX_ARR);
				}
			} // !le_is_member
		} // !comma_resolved
	}
	if (comma_resolved) PPARSE_BOUNDS_DIAG(tok, PPARSE_ERR_BOUNDS_COMMA_OP);
	{
		PParseToken *le = pparse_bounds_peel_primary(last_emitted, true);
		if (pparse_is_value_name_token(le) || le->kind == PPARSE_TK_NUM ||
		    last_emitted->kind == PPARSE_TK_NUM) {
			bool memb = pparse_idx(_pc, le) >= 1 &&
				    (pparse_token_pool[pparse_idx(_pc, le) - 1].tag & PPARSE_TT_MEMBER);
			bool left_ok_scan = !memb && le->kind != PPARSE_TK_NUM && !pparse_is_known_typedef(le) &&
					    pparse_bounds_is_tracked_array(le);
			PParseToken *hit = left_ok_scan ? NULL : pparse_bounds_find_tracked_array(pparse_next(_pc, tok), rb);
			while (hit) {
				PParseToken *nx = pparse_next(_pc, hit);
				if (!(pparse_match_ch(nx, '[') && (nx->flags & PPARSE_TF_OPEN))) break;
				hit = pparse_bounds_find_tracked_array(pparse_next(_pc, nx), rb);
			}
			if (hit) PPARSE_BOUNDS_DIAG(tok, PPARSE_ERR_BOUNDS_COMM_SCAN);
			PParseToken *inner0 = pparse_next(_pc, tok);
			if (!left_ok_scan && pparse_match_ch(inner0, '(') && (inner0->flags & PPARSE_TF_OPEN) &&
			    pparse_next(_pc, pparse_pair_known(inner0)) == rb &&
			    pparse_bounds_paren_has_array_arithmetic(inner0))
				PPARSE_BOUNDS_DIAG(tok, PPARSE_ERR_BOUNDS_PTR_ARITH_SUB);
		}
	}
	PParseToken *name_tok = last_emitted;
	if (ti >= 1) {
		PParseToken *pool_prev = &pparse_token_pool[ti - 1];
		if (pool_prev != last_emitted &&
		    (pparse_match_ch(pool_prev, ']') || pparse_match_ch(pool_prev, ')') ||
		     pparse_is_value_name_token(pool_prev)))
			name_tok = pool_prev;
	}
	int dim_depth = 0;
	while (1) {
		if (pparse_match_ch(name_tok, ')')) {
			PParseToken *open = pparse_pair_known(name_tok);
			if (pparse_idx(_pc, open) >= 1) {
				PParseToken *bp = &pparse_token_pool[pparse_idx(_pc, open) - 1];
				if (pparse_is_value_name_token(bp) || bp->kind == PPARSE_TK_NUM ||
				    pparse_match_ch(bp, ')') || pparse_match_ch(bp, ']'))
					break;
			}
			PParseToken *inner = pparse_next(_pc, open);
			if (pparse_is_value_name_token(inner) && pparse_next(_pc, inner) == name_tok) {
				name_tok = inner;
				break;
			}
			if (pparse_match_ch(inner, '(') &&
			    pparse_next(_pc, pparse_pair_known(inner)) == name_tok) {
				name_tok = pparse_pair_known(inner);
				continue;
			}
			if (pparse_idx(_pc, name_tok) >= 1) {
				PParseToken *inner_last = &pparse_token_pool[pparse_idx(_pc, name_tok) - 1];
				if (pparse_match_ch(inner_last, ']')) {
					name_tok = inner_last;
					continue;
				}
			}
			break;
		}
		if (pparse_match_ch(name_tok, ']')) {
			PParseToken *open_br = pparse_pair_known(name_tok);
			if (pparse_ann(open_br) & (P1_DECL_BRACKET | P1_UNEVAL_BRACKET)) break;
			if (open_br->flags & PPARSE_TF_C23_ATTR) break;
			if (pparse_idx(_pc, open_br) < 1) break;
			PParseToken *before = &pparse_token_pool[pparse_idx(_pc, open_br) - 1];
			if (!(pparse_is_value_name_token(before) || pparse_match_ch(before, ')') ||
			      pparse_match_ch(before, ']')))
				break;
			name_tok = before;
			dim_depth++;
			continue;
		}
		break;
	}
	if (!pparse_is_value_name_token(name_tok)) {
		PParseToken *probe = last_emitted;
		while (probe && pparse_match_ch(probe, ')')) {
			PParseToken *op = pparse_pair_known(probe);
			if (pparse_bounds_paren_has_array_arithmetic(op))
				PPARSE_BOUNDS_DIAG(tok, PPARSE_ERR_BOUNDS_PTR_ARITH_SUB);
			PParseToken *hit = pparse_bounds_find_tracked_array(pparse_next(_pc, op), probe);
			if (hit && pparse_bounds_is_tracked_array(hit)) {
				name_tok = hit;
				break;
			}
			if (pparse_idx(_pc, op) < 1) break;
			probe = &pparse_token_pool[pparse_idx(_pc, op) - 1];
		}
	}
	if (!pparse_is_value_name_token(name_tok)) return false;
	if (pparse_is_known_typedef(name_tok)) return false;
	uint32_t name_idx = pparse_idx(_pc, name_tok);
	if (name_idx >= 1) {
		PParseToken *pv = &pparse_token_pool[name_idx - 1];
		if (pv->tag & PPARSE_TT_MEMBER) return false;
		if (pv->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF)) return false;
		if (pparse_match_ch(pv, '*') && !(pv->flags & PPARSE_TF_OPEN) && pparse_idx(_pc, pv) >= 1) {
			PParseToken *pp = &pparse_token_pool[pparse_idx(_pc, pv) - 1];
			bool is_decl_star =
			    (pp->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF)) ||
			    (pparse_match_ch(pp, '*') && !(pp->flags & PPARSE_TF_OPEN)) ||
			    (pparse_match_ch(pp, '(') && (pp->flags & PPARSE_TF_OPEN));
			if (is_decl_star) return false;
		}
		PParseArrayBindingInfo array = pparse_array_binding_info(name_tok);
		if (!array.tracked) return false;
		if (array.rank > 0 && array.rank != PPARSE_ARRAY_RANK_WRAP_ALL && dim_depth >= array.rank) return false;
		PParseToken *operand_start = name_tok;
		/* Include this subscript in the operand span.  Otherwise `&(a[i])`
		 * cannot peel the parentheses: the close follows `]`, not the bare
		 * name token, and the address-of suppression is lost even though it
		 * is semantically identical to `&a[i]` (including a valid one-past
		 * address at i == length). */
		PParseToken *operand_end = rb;
		PParseToken *prev = &pparse_token_pool[name_idx - 1];
		while (pparse_match_ch(prev, '(') && (prev->flags & PPARSE_TF_OPEN)) {
			PParseToken *rp = pparse_pair_known(prev);
			if (pparse_next(_pc, prev) != operand_start || pparse_next(_pc, operand_end) != rp) break;
			operand_start = prev;
			operand_end = rp;
			if (pparse_idx(_pc, prev) < 1) {
				prev = NULL;
				break;
			}
			prev = &pparse_token_pool[pparse_idx(_pc, prev) - 1];
		}
		if (prev && pparse_match_ch(prev, '&') && !(prev->flags & PPARSE_TF_OPEN)) {
			bool unary = true;
			if (pparse_idx(_pc, prev) >= 1) {
				PParseToken *pp = &pparse_token_pool[pparse_idx(_pc, prev) - 1];
				/* A control statement's condition close is not a value
				 * (`if (c) &a[len];` -- the `)` ends `if (c)`, so `&` still
				 * addresses `a[len]` unarily); mistaking it for one made
				 * this one-past-end address spuriously wrap and trap.
				 * `}` (compound-literal close, e.g. `(int){5} & a[i]`) and
				 * postfix `++`/`--` (`x++ & a[i]`) are value-producing too:
				 * without them here, `&` after either was misread as unary
				 * address-of and `a[i]` silently kept its raw, unchecked
				 * subscript instead of being wrapped. */
				if (pparse_bounds_tok_ends_value(pp)) unary = false;
			}
			/* `*&a[i]` cancels the address-of: the `&` never yields an
			 * address to the program, it is dereferenced right back into a
			 * read of `a[i]`. Suppressing the wrapper here (the one-past-end
			 * rule for a genuine `&a[len]`) left the access unchecked, so
			 * `*&a[idx]` read out of bounds with no wrapper and no
			 * diagnostic while the identical `a[idx]` trapped. Only a *unary*
			 * `*` cancels: in `x * &a[i]` the `*` multiplies and the
			 * address-of is real, so the suppression must stand. */
			if (unary && pparse_idx(_pc, prev) >= 1) {
				/* Peel `(` opens so `*(&a[i])` and `*((&a[i]))` cancel the
				 * same way `*&a[i]` does. Over-peeling cannot cause a
				 * spurious wrap: the binary-vs-unary test on the `*` below
				 * still has to pass, so `x * (&a[i])` and `f((&a[i]))` are
				 * both left alone. */
				PParseToken *deref = &pparse_token_pool[pparse_idx(_pc, prev) - 1];
				/* A paren only counts if it wraps the address-of operand
				 * exactly, i.e. it closes immediately after this subscript's
				 * `]`. `*(&a[0]+i)` also opens a paren before the `&`, but it
				 * closes after the `+i`: there the address is a real operand
				 * of pointer arithmetic and the `*` dereferences the sum, so
				 * treating it as a cancelled `&` would wrap a subscript that
				 * must stay bare under -fno-safety. */
				PParseToken *expect = pparse_next(_pc, rb);
				for (int peel = 0; peel < 8 && pparse_match_ch(deref, '(') &&
					   (deref->flags & PPARSE_TF_OPEN) && pparse_idx(_pc, deref) >= 1;
				     peel++) {
					PParseToken *rp = pparse_pair_known(deref);
					if (rp != expect) break;
					expect = pparse_next(_pc, rp);
					deref = &pparse_token_pool[pparse_idx(_pc, deref) - 1];
				}
				if (pparse_match_ch(deref, '*') && !(deref->flags & PPARSE_TF_OPEN)) {
					bool deref_unary = true;
					if (pparse_idx(_pc, deref) >= 1)
						deref_unary = !pparse_bounds_tok_ends_value(
						    &pparse_token_pool[pparse_idx(_pc, deref) - 1]);
					if (deref_unary) unary = false;
				}
			}
			if (unary) return false;
		}
	} else {
		return false;
	}

	PParseToken *cast_close = NULL; /* ')' of outermost (T *) / (T(*)[N]) */
	{
		PParseToken *probe = name_tok;
		for (int peel = 0; peel < 8 && pparse_idx(_pc, probe) >= 1; peel++) {
			PParseToken *prev = &pparse_token_pool[pparse_idx(_pc, probe) - 1];
			if (pparse_match_ch(prev, ')')) {
				PParseToken *open = pparse_pair_known(prev);
				PParseToken *last =
				    pparse_idx(_pc, prev) >= 1 ? &pparse_token_pool[pparse_idx(_pc, prev) - 1] : NULL;
				if (last && (pparse_match_ch(last, '*') || pparse_match_ch(last, ']'))) {
					PParseToken *fi = pparse_skip_noise(_pc, pparse_next(_pc, open));
					bool looks_cast =
					    fi && (pparse_is_type_keyword(fi) ||
						   (fi->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE |
							       PPARSE_TT_TYPEOF)) ||
						   pparse_is_known_typedef(fi));
					if (looks_cast) {
						cast_close = prev;
						probe = open;
						continue;
					}
				}
				if (pparse_next(_pc, open) == probe && pparse_next(_pc, probe) == prev) {
					probe = open;
					continue;
				}
			}
			break;
		}
	}
	out->arr = name_tok;
	out->close = rb;
	out->cast_close = cast_close;
	out->dim_depth = dim_depth;
	return true;
}

static inline bool pparse_cached_bounds_plan(PParseToken *tok, PParseBoundsPlan *out) {
	PParseAnalysisRecord *r = pparse_analysis_get(tok, PPARSE_AR_BOUNDS);
	if (!r) return false;
	*out = r->as.bounds;
	return true;
}

/* `)` that closes typeof(…), _BitInt(…), _Alignas(…), or _Atomic(…) —
 * a type-specifier constructor, so a following soft keyword is not an
 * expression operator (`typeof(_Atomic(int) orelse 0)` is type-junk). */
static bool pparse_close_paren_ends_type_specifier_ctor(PParseToken *tok) {
	PPARSE_CTX();
	if (!tok || !pparse_match_ch(tok, ')')) return false;
	PParseToken *open = pparse_pair_known(tok);
	PParseToken *before_open = pparse_walk_back(pparse_idx(_pc, open), PPARSE_WB_PAST_NOISE);
	if (!before_open) return false;
	if (before_open->tag & (PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS)) return true;
	return (before_open->tag & PPARSE_TT_TYPE) && pparse_equal(before_open, "_Atomic");
}

static bool pparse_orelse_is_label_or_goto_target(PParseToken *tok, PParseToken *prev) {
	PPARSE_CTX();
	if (prev && ((prev->tag & PPARSE_TT_GOTO) || pparse_is_gnu_label_decl_head(prev) || pparse_equal_2(_pc, prev, "&&")))
		return true;
	if (!tok) return false;
	PParseToken *next = pparse_skip_noise(_pc, pparse_next(_pc, tok));
	return next && pparse_match_ch(next, ':') && !(pparse_next(_pc, next) && pparse_match_ch(pparse_next(_pc, next), ':'));
}

static inline bool pparse_decl_paren_predecessor_is_type(PParseToken *p) {
	return p && (pparse_is_type_keyword(p) || (p->tag & (PPARSE_TT_TYPEOF | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE)) ||
		     pparse_match_ch(p, '*') || pparse_is_known_typedef(p));
}

static inline bool pparse_is_array_bracket_predecessor(PParseToken *t) {
	PPARSE_CTX();
	if (pparse_is_type_keyword(t) || (t->tag & PPARSE_TT_QUALIFIER) || pparse_is_known_typedef(t) || (pparse_match_ch(t, '*')) ||
	    (pparse_match_ch(t, '}')))
		return true;
	if (pparse_is_identifier_like(t)) {
		PParseToken *b = pparse_walk_back(pparse_idx(_pc, t), PPARSE_WB_PAST_NOISE);
		return b && (b->tag & PPARSE_TT_SUE);
	}
	if (pparse_match_ch(t, ']')) {
		PParseToken *open = pparse_pair_known(t);
		PParseToken *before_open = pparse_walk_back(pparse_idx(_pc, open), PPARSE_WB_PAST_NOISE);
		if (!before_open) return true;
		if (pparse_decl_paren_predecessor_is_type(before_open)) return true;
		if (pparse_match_ch(before_open, ']')) return pparse_is_array_bracket_predecessor(before_open);
		return false;
	}
	if (pparse_match_ch(t, ')')) {
		PParseToken *open = pparse_pair_known(t);
		PParseToken *before_open = pparse_walk_back(pparse_idx(_pc, open), PPARSE_WB_PAST_NOISE);
		if (!before_open) return true;
		return pparse_decl_paren_predecessor_is_type(before_open);
	}
	return false;
}

static bool pparse_array_size_is_vla_impl(PParseToken *open_bracket, int depth) {
	PPARSE_CTX();
	if (depth > 256) pparse_error_tok(open_bracket, "array dimension nesting depth exceeds 256");
	PParseToken *close = pparse_pair_known(open_bracket);
	PParseToken *tok = pparse_next(_pc, open_bracket);
	while (tok != close) {
		if (pparse_match_ch(tok, '[')) {
			if (pparse_array_size_is_vla_impl(tok, depth + 1)) return true;
			tok = pparse_skip_balanced_group(tok);
			continue;
		}
		if (pparse_match_ch(tok, '(') && pparse_match_ch(pparse_next(_pc, tok), '{')) return true;
		if (tok->tag & PPARSE_TT_GENERIC) return true;
		pparse_SKIP_NOISE_CONTINUE(tok);
		if (pparse_is_sizeof_like(tok)) {
			bool is_sizeof = tok->ch0 == 's';
			tok = pparse_next(_pc, tok);
			if (tok != close && pparse_match_ch(tok, '(')) {
				PParseToken *end = pparse_skip_balanced_group(tok);
				if (is_sizeof) {
					PParseToken *prev_inner = tok;
					for (PParseToken *inner = pparse_next(_pc, tok); inner != end;
					     prev_inner = inner, inner = pparse_next(_pc, inner)) {
						if (pparse_is_enum_kw(inner)) {
							PParseToken *brace = pparse_find_struct_body_brace(inner);
							if (brace) {
								/* Register enumerators so later
								 * identifiers in the same dimension
								 * (e.g. sizeof(enum { A = 5 }) + A)
								 * are not misclassified as VLA. */
								pparse_enum_constants(brace, 0);
								inner = pparse_skip_balanced_group(brace);
								if (inner == end) break;
								continue;
							}
						}
						int vla_fl = pparse_typedef_flags(inner) & (PPARSE_TDF_VLA | PPARSE_TDF_PARAM);
						if (vla_fl & PPARSE_TDF_VLA) {
							if (!(vla_fl & PPARSE_TDF_PARAM)) {
								PParseToken *la = pparse_skip_noise(_pc, pparse_next(_pc, inner));
								if (la != end && pparse_match_ch(la, ')'))
									return true;
							}
							PParseToken *ni = pparse_next(_pc, inner);
							bool has_next = ni && ni != end;
							PParseToken *eff_prev = prev_inner;
							uint32_t pi = pparse_idx(_pc, eff_prev);
							while (eff_prev->ch0 == '(' && pi > pparse_idx(_pc, tok) + 1)
								eff_prev = &pparse_token_pool[--pi];
							PParseToken *eff_next = ni;
							while (has_next && eff_next->ch0 == ')' &&
							       eff_next != end) {
								eff_next = pparse_next(_pc, eff_next);
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
							       !(inner->kind == PPARSE_TK_IDENT &&
								 (vla_fl & PPARSE_TDF_VLA)))));
							if (deref) return true;
						}
						if (pparse_match_ch(inner, '[') &&
						    pparse_is_array_bracket_predecessor(prev_inner)) {
							if (pparse_array_size_is_vla_impl(inner, depth + 1))
								return true;
							inner = pparse_pair_known(inner);
							if (inner == end) break;
							continue;
						}
						if (pparse_is_valid_varname(inner) && !pparse_is_type_keyword(inner) &&
						    !pparse_is_known_typedef(inner) && !pparse_is_known_enum_const(inner) &&
						    inner != end && pparse_match_ch(pparse_next(_pc, inner), '(')) {
							PParseToken *call_open = pparse_next(_pc, inner);
							PParseToken *call_end = pparse_skip_balanced_group(call_open);
							bool is_deref = (pparse_match_ch(prev_inner, '*')) ||
									(call_end != end &&
									 ((pparse_match_ch(call_end, '[')) ||
									  (call_end->tag & PPARSE_TT_MEMBER)));
							if (is_deref)
								for (PParseToken *a = pparse_next(_pc, call_open);
								     a != call_end;
								     a = pparse_next(_pc, a))
									if (pparse_is_valid_varname(a) &&
									    !pparse_is_known_enum_const(a) &&
									    !pparse_is_type_keyword(a))
										return true;
							inner = call_end;
							if (inner == end) break;
						}
					}
				}
				tok = end;
				if (tok != close && pparse_match_ch(tok, '{')) tok = pparse_skip_balanced_group(tok);
			} else if (tok != close) {
				while (tok != close) {
					pparse_SKIP_NOISE_CONTINUE(tok);
					if ((tok->len == 1 &&
					     (tok->ch0 == '*' || tok->ch0 == '&' || tok->ch0 == '!' ||
					      tok->ch0 == '+' || tok->ch0 == '-' || tok->ch0 == '~')) ||
					    (tok->len == 2 && (tok->ch0 == '+' || tok->ch0 == '-')) ||
					    pparse_is_sizeof_like(tok)) {
						tok = pparse_next(_pc, tok);
						continue;
					}
					break;
				}
				if (tok != close) {
					if (tok->flags & PPARSE_TF_OPEN) {
						tok = pparse_next(_pc, pparse_pair_known(tok));
						if (tok != close && pparse_match_ch(tok, '{')) {
							tok = pparse_next(_pc, pparse_pair_known(tok));
						}
					} else {
						if (pparse_is_identifier_like(tok) && pparse_is_vla_typedef(tok))
							return true;
						tok = pparse_next(_pc, tok);
					}
				}
				while (tok != close) {
					if (tok->tag & PPARSE_TT_MEMBER) {
						tok = pparse_next(_pc, tok);
						if (tok != close) tok = pparse_next(_pc, tok);
					} else if (tok->flags & PPARSE_TF_OPEN) {
						tok = pparse_next(_pc, pparse_pair_known(tok));
					} else
						break;
				}
			}
			continue;
		}

		/* A C23 'constexpr' object is an integer constant expression at every
		 * use (C23 6.7.1p6), same as an enum constant: naming one in a
		 * dimension does not make the array variable-length. Without this,
		 * `constexpr int N = 4; int arr[N];` was flagged as a VLA -- wrong
		 * zero-init strategy, and (with `raw`, which does not exempt real
		 * VLAs) a false "goto would skip over this VLA declaration" reject
		 * on code the user explicitly opted out of the check for. */
		if ((tok->tag & PPARSE_TT_MEMBER) ||
		    (pparse_is_valid_varname(tok) && !pparse_is_known_enum_const(tok) &&
		     !pparse_is_type_keyword(tok) && !pparse_is_constexpr_ident(tok)))
			return true;
		tok = pparse_next(_pc, tok);
	}
	return false;
}

static inline bool pparse_array_size_is_vla(PParseToken *open_bracket) {
	if (pparse_ann(open_bracket) & P1_VLA_KNOWN) return (pparse_ann(open_bracket) & P1_VLA) != 0;
	bool result = pparse_array_size_is_vla_impl(open_bracket, 0);
	pparse_ann(open_bracket) |= P1_VLA_KNOWN | (result ? P1_VLA : 0);
	return result;
}

static bool pparse_array_bracket_closes_ptr_to_array(PParseToken *open_bracket, PParseToken *prev);

static PParseToken *pparse_declarator_array_dims(PParseToken *tok, PParseDecl *decl) {
	PPARSE_CTX();
	for (;;) {
		if (!tok || tok->kind == PPARSE_TK_EOF) return tok;
		if (pparse_match_ch(tok, '[')) {
			pparse_ann(tok) |= P1_DECL_BRACKET;
			if (tok->flags & PPARSE_TF_C23_ATTR) {
				tok = pparse_skip_balanced_group(tok);
				continue;
			}
			PParseToken *close = pparse_pair_known(tok), *first = pparse_next(_pc, tok);
			PParseToken *inner = pparse_skip_noise(_pc, first);
			if (close && (!inner || inner == close ||
				      (inner->kind == PPARSE_TK_NUM && inner->len == 1 && inner->ch0 == '0' &&
				       pparse_skip_noise(_pc, pparse_next(_pc, inner)) == close)))
				decl->has_zero_dim = true;
			PParseToken *prev = pparse_walk_back(pparse_idx(_pc, tok), PPARSE_WB_ATTR_NOISE);
			if (!pparse_array_bracket_closes_ptr_to_array(tok, prev)) {
				if (!decl->array_rank) {
					decl->array_dim_complete = first && !pparse_match_ch(first, ']');
				}
				if (decl->array_rank < 15) decl->array_rank++;
				else decl->array_rank = PPARSE_ARRAY_RANK_WRAP_ALL;
			}
			if (pparse_array_size_is_vla(tok)) decl->is_vla = true;
			tok = pparse_skip_balanced_group(tok);
			continue;
		}
		/* GNU attributes and preprocessor noise may separate array dims. */
		PParseToken *after = pparse_skip_noise(_pc, tok);
		if (after != tok && pparse_match_ch(after, '[')) {
			tok = after;
			continue;
		}
		return after;
	}
}

/* Pure C declarator analysis. Emission is intentionally outside this API so
 * parse.c can be reused without depending on Prism's output state. */
static PParseDecl pparse_declarator_parse(PParseToken *tok) {
	PPARSE_CTX();
	PParseDecl r = {.end = tok};
	int ptr_depth = 0;

#define PPARSE_DECL_EAT_PTRS(extra_ptr_action)                                                                    \
	while (tok && tok->kind != PPARSE_TK_EOF) {                                                                 \
		PParseToken *_n = pparse_skip_noise(_pc, tok);                                                                  \
		if (_n != tok) {                                                                             \
			tok = _n;                                                                            \
			continue;                                                                            \
		}                                                                                            \
		if (pparse_match_ch(tok, '*')) {                                                                    \
			r.is_pointer = true;                                                                 \
			r.is_const = false;                                                                  \
			r.is_volatile = false;                                                               \
			r.is_atomic = false;                                                                 \
			extra_ptr_action;                                                                    \
			if (++ptr_depth > 1024) {                                                            \
				pparse_warn_tok(tok, "pointer depth exceeds 1024; zero-initialization skipped");    \
				r.end = NULL;                                                                \
				return r;                                                                    \
			}                                                                                    \
			tok = pparse_next(_pc, tok);                                                                 \
		} else if ((tok->tag & PPARSE_TT_QUALIFIER) &&                                                      \
			   !(pparse_is_soft_keyword_identifier(tok) && pparse_soft_keyword_decl_name_boundary(tok))) {     \
			if (r.is_pointer && (tok->tag & PPARSE_TT_CONST)) r.is_const = true;                        \
			if (r.is_pointer && (tok->tag & PPARSE_TT_VOLATILE)) r.is_volatile = true;                  \
			if (r.is_pointer && (tok->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE)) ==                  \
						      (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE)) r.is_atomic = true;        \
			tok = pparse_next(_pc, tok);                                                                 \
		} else                                                                                       \
			break;                                                                               \
	}

	PPARSE_DECL_EAT_PTRS((void)0)

	int nested_paren = 0;
	if (pparse_match_ch(tok, '(')) {
		PParseToken *peek = pparse_skip_noise(_pc, pparse_next(_pc, tok));
		if (!pparse_match_ch(peek, '*') && !pparse_match_ch(peek, '(') && !pparse_is_valid_varname(peek)) {
			r.end = NULL;
			return r;
		}
		tok = pparse_next(_pc, tok);
		nested_paren = 1;
		r.has_paren = true;
		PPARSE_DECL_EAT_PTRS(r.paren_pointer = true)
		while (pparse_match_ch(tok, '(')) {
			if (++nested_paren > 1024) {
				pparse_warn_tok(tok, "parenthesization depth exceeds 1024");
				r.end = NULL;
				return r;
			}
			tok = pparse_next(_pc, tok);
			PPARSE_DECL_EAT_PTRS(r.paren_pointer = true)
		}
	}
#undef PPARSE_DECL_EAT_PTRS

	if (!pparse_is_valid_varname(tok)) {
		r.end = NULL;
		return r;
	}
	r.var_name = tok;
	tok = pparse_skip_noise(_pc, pparse_next(_pc, tok));
	if (r.has_paren && pparse_match_ch(tok, '(')) r.is_func_decl = true;
	if (r.has_paren && pparse_match_ch(tok, '[')) {
		r.is_array = r.paren_array = true;
		tok = pparse_declarator_array_dims(tok, &r);
	}
	while (r.has_paren && nested_paren > 0) {
		while (pparse_match_ch(tok, '(') || pparse_match_ch(tok, '[')) {
			if (pparse_match_ch(tok, '(')) tok = pparse_skip_balanced_group(tok);
			else {
				r.is_array = r.paren_array = true;
				tok = pparse_declarator_array_dims(tok, &r);
			}
		}
		if (!pparse_match_ch(tok, ')')) {
			r.end = NULL;
			return r;
		}
		tok = pparse_next(_pc, tok);
		nested_paren--;
	}

	if (pparse_match_ch(tok, '(')) {
		if (!r.has_paren) {
			r.end = NULL;
			return r;
		}
		r.is_func_ptr = true;
		tok = pparse_skip_balanced_group(tok);
	}
	if (pparse_match_ch(tok, '[')) {
		r.is_array = true;
		tok = pparse_declarator_array_dims(tok, &r);
	}
	while (tok && tok->kind != PPARSE_TK_EOF) {
		PPARSE_SKIP_NOISE_RESTART(tok);
		if (!(tok->tag & PPARSE_TT_ASM)) break;
		tok = pparse_next(_pc, tok);
		if (tok && pparse_match_ch(tok, '(')) tok = pparse_skip_balanced_group(tok);
	}

	r.has_init = pparse_match_ch(tok, '=');
	r.end = tok;
	return r;
}

#define PPARSE_CACHED_PARSE(fn, parse_fn, T, record_kind, record_member)                              \
	static T fn(PParseToken *tok) {                                                                \
		PPARSE_CTX();                                                                            \
		if (!tok) return (T){0};                                                                  \
		PParseAnalysisRecord *cached = pparse_analysis_get(tok, record_kind);                     \
		if (cached) return cached->as.record_member;                                               \
		if (_pc->parses_frozen)                                                                    \
			pparse_error_tok(tok, "internal: declaration parse requested after analysis");       \
		T parsed = parse_fn(tok);                                                                  \
		pparse_analysis_add(tok, record_kind)->as.record_member = parsed;                          \
		return parsed;                                                                             \
	}

PPARSE_CACHED_PARSE(pparse_declarator,
			    pparse_declarator_parse,
			    PParseDecl,
			    PPARSE_AR_DECL,
			    decl)

enum PParseStructBodyTraits { PPARSE_SBT_VLA = 1, PPARSE_SBT_VOL = 2 };

// Ordinary identifiers can shadow tag names (C11 §6.2.3).
static unsigned pparse_struct_body_field_traits(PParseToken *id, PParseToken *prev) {
	PPARSE_CTX();
	if (!pparse_is_identifier_like(id)) return 0;
	/* Field declarators use the member namespace, not the ordinary binding
	 * table (C11 §6.2.3). */
	PParseToken *nx = pparse_skip_noise(_pc, pparse_next(_pc, id));
	if (nx && pparse_match_set(nx, pparse_CH(';') | pparse_CH(',') | pparse_CH(':') |
				      pparse_CH('[') | pparse_CH('=')))
		return 0;
	unsigned traits = 0;
	int tflags = pparse_typedef_flags(id);
	if (prev && (prev->tag & PPARSE_TT_SUE)) {
		PParseTypedefEntry *te = pparse_tag_lookup(id);
		if (te && te->is_struct_tag && te->is_vla) traits |= PPARSE_SBT_VLA;
		if (te && te->has_volatile_member) traits |= PPARSE_SBT_VOL;
	} else if (tflags & PPARSE_TDF_VLA) {
		traits |= PPARSE_SBT_VLA;
	}
	if (tflags & (PPARSE_TDF_VOLATILE | PPARSE_TDF_HAS_VOL_MEMBER))
		traits |= PPARSE_SBT_VOL;
	return traits;
}

static unsigned pparse_struct_body_traits(PParseToken *brace) {
	PPARSE_CTX();
	if (!brace || !pparse_match_ch(brace, '{')) return 0;
	PParseToken *end = pparse_pair_known(brace);
	unsigned traits = 0;
	PParseToken *prev = brace;
	for (PParseToken *t = pparse_next(_pc, brace); t && t != end; prev = t, t = pparse_next(_pc, t)) {
		if (pparse_match_ch(t, '{')) {
			traits |= pparse_struct_body_traits(t);
			if (traits == (PPARSE_SBT_VLA | PPARSE_SBT_VOL)) return traits;
			t = pparse_pair_known(t);
			continue;
		}
		if ((t->flags & PPARSE_TF_OPEN) && pparse_match_ch(t, '[')) {
			if (pparse_array_size_is_vla(t)) traits |= PPARSE_SBT_VLA;
			t = pparse_pair_known(t);
			continue;
		}
		// Don't skip typeof()/_Atomic() parens — VLA dims / volatile hide inside.
		if ((t->flags & PPARSE_TF_OPEN) &&
		    !(prev && ((prev->tag & PPARSE_TT_TYPEOF) ||
			       ((prev->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE)) == (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE))))) {
			t = pparse_pair_known(t);
			continue;
		}
		if (t->tag & PPARSE_TT_VOLATILE) traits |= PPARSE_SBT_VOL;
		traits |= pparse_struct_body_field_traits(t, prev);
		if (traits == (PPARSE_SBT_VLA | PPARSE_SBT_VOL)) return traits;
	}
	return traits;
}

static bool pparse_comma_starts_declarator(PParseToken *tok) {
	PPARSE_CTX();
	PParseToken *n = pparse_next(_pc, tok);
	if (!n) return false;
	if (pparse_match_ch(n, '(')) {
		PParseToken *inside = pparse_next(_pc, n);
		return inside &&
		       !(inside->tag & (PPARSE_TT_TYPE | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_QUALIFIER)) &&
		       !pparse_is_known_typedef(inside) && !pparse_is_c23_attr(inside);
	}
	if (pparse_match_ch(n, '*') || (n->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_ATTR)) ||
	    pparse_is_c23_attr(n))
		return true;
	if (!pparse_is_valid_varname(n) || (n->tag & (PPARSE_TT_TYPE | PPARSE_TT_SUE | PPARSE_TT_TYPEOF)))
		return false;
	PParseToken *after_name = pparse_next(_pc, n);
	if (!after_name || !pparse_match_ch(after_name, '(')) return true;
	PParseToken *inside = pparse_next(_pc, after_name);
	return inside &&
	       (inside->tag & (PPARSE_TT_TYPE | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_QUALIFIER));
}

// not an array-of-VLA object — must not set PParseTypeSpec.is_vla / is_array.
static bool pparse_abstract_declarator_paren_is_pointer_only(PParseToken *open_paren) {
	PPARSE_CTX();
	if (!pparse_match_ch(open_paren, '(')) return false;
	PParseToken *close = pparse_pair_known(open_paren);
	for (PParseToken *x = pparse_next(_pc, open_paren); x && x != close;) {
		x = pparse_skip_noise(_pc, x);
		if (!x || x == close) break;
		if (pparse_match_ch(x, '*')) {
			x = pparse_next(_pc, x);
			continue;
		}
		if (x->tag & PPARSE_TT_QUALIFIER) {
			x = pparse_next(_pc, x);
			continue;
		}
		if (pparse_match_ch(x, '(') && (x->flags & PPARSE_TF_OPEN)) {
			PParseToken *inner_close = pparse_pair_known(x);
			if (!pparse_abstract_declarator_paren_is_pointer_only(x)) return false;
			x = pparse_next(_pc, inner_close);
			continue;
		}
		if (pparse_is_identifier_like(x) && x->kind == PPARSE_TK_IDENT) {
			x = pparse_next(_pc, x);
			while (x != close && pparse_match_ch(x, '[') && (x->flags & PPARSE_TF_OPEN)) {
				PParseToken *rb = pparse_pair_known(x);
				x = pparse_next(_pc, rb);
			}
			continue;
		}
		return false;
	}
	return true;
}

static bool pparse_array_bracket_closes_ptr_to_array(PParseToken *open_bracket, PParseToken *prev) {
	if (!open_bracket || !prev || prev->len != 1 || prev->ch0 != ')') return false;
	return pparse_abstract_declarator_paren_is_pointer_only(pparse_pair_known(prev));
}

static void pparse_scan_type_constructor(PParseToken *start,
					 PParseToken *end,
					 PParseTypeSpec *r,
					 bool check_typeof,
					 bool copy_quals) {
	PPARSE_CTX();
	PParseToken *prev = NULL;
	bool saw_sue = false, outer_ptr = false, vla_done = false;
	int depth = 0, fn_skip = 0;
	uint32_t sizeof_end = 0;
	for (PParseToken *t = start; t && t != end; prev = t, t = pparse_next(_pc, t)) {
		/* sizeof/alignof/offsetof yield an unqualified size type. Qualifiers
		 * written inside their balanced operand describe that operand, not the
		 * result of an enclosing typeof. Scanning through `_Atomic int` here
		 * made `const typeof(sizeof(_Atomic int))` falsely require the atomic
		 * memset path and reject otherwise valid code. */
		if (copy_quals && pparse_is_sizeof_like(t)) {
			PParseToken *op = pparse_skip_noise(_pc, pparse_next(_pc, t));
			if (pparse_match_ch(op, '(')) {
				t = pparse_pair_known(op);
				continue;
			}
		}
		PParseToken *attr_open = (t->tag & PPARSE_TT_ATTR) ? pparse_next(_pc, t) : NULL;
		PParseToken *attr_close = attr_open && pparse_match_ch(attr_open, '(')
					   ? pparse_pair_known(attr_open)
					   : NULL;
		if (attr_close) {
			t = attr_close;
			continue;
		}
		if (pparse_is_c23_attr(t)) {
			t = pparse_pair_known(t);
			continue;
		}
		if (t->tag & (PPARSE_TT_GOTO | PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE)) {
			SAFETY_DIAG2(t, PPARSE_ERR_CTRL_IN_TYPE_SPEC, PPARSE_ERR_CTRL_IN_TYPE_SPEC_HARD);
		} else if ((t->tag & PPARSE_TT_DEFER) && !pparse_typedef_lookup(_pc, t)) {
			PParseToken *next = pparse_next(_pc, t);
			if (pparse_match_ch(next, '{') || pparse_is_identifier_like(next))
				SAFETY_DIAG2(t, PPARSE_ERR_CTRL_IN_TYPE_SPEC, PPARSE_ERR_CTRL_IN_TYPE_SPEC_HARD);
		}
		if (check_typeof && (t->tag & PPARSE_TT_TYPEOF)) r->has_typeof = true;

		if (pparse_is_enum_kw(t)) {
			PParseToken *brace = pparse_find_struct_body_brace(t);
			if (brace) {
				unsigned traits = pparse_struct_body_traits(brace);
				bool has_vla = (traits & PPARSE_SBT_VLA) != 0;
				r->is_vla |= has_vla;
				r->type_vm |= has_vla;
				r->has_volatile_member |= (traits & PPARSE_SBT_VOL) != 0;
				pparse_enum_constants(brace, 0);
				t = pparse_pair_known(brace);
				saw_sue = false;
				continue;
			}
		}
		if (pparse_match_ch(t, '{') && saw_sue) {
			unsigned traits = pparse_struct_body_traits(t);
			bool has_vla = (traits & PPARSE_SBT_VLA) != 0;
			r->is_vla |= has_vla;
			r->type_vm |= has_vla;
			r->has_volatile_member |= (traits & PPARSE_SBT_VOL) != 0;
			t = pparse_pair_known(t);
			saw_sue = false;
			continue;
		}

		if (pparse_match_ch(t, '(')) {
			if (fn_skip > 0) fn_skip++;
			else if (prev && pparse_match_ch(prev, ')'))
				fn_skip = 1;
			depth++;
			continue;
		} else if (pparse_match_ch(t, ')')) {
			if (fn_skip > 0) fn_skip--;
			if (depth > 0) depth--;
			continue;
		}

		if (!check_typeof && pparse_is_sizeof_like(t) && !vla_done) {
			PParseToken *nx = pparse_next(_pc, t);
			PParseToken *close = pparse_match_ch(nx, '(') ? pparse_pair_known(nx) : NULL;
			if (close) sizeof_end = pparse_idx(_pc, close);
		}
		bool scan_vla = !vla_done & (fn_skip == 0) &
				((sizeof_end == 0) | (pparse_idx(_pc, t) > sizeof_end));
		if (sizeof_end && pparse_idx(_pc, t) > sizeof_end) sizeof_end = 0;
		if (scan_vla && pparse_match_ch(t, '[') && !(t->flags & PPARSE_TF_C23_ATTR) &&
		    prev && pparse_is_array_bracket_predecessor(prev)) {
			bool dim_vla = pparse_array_size_is_vla(t);
			r->type_vm |= dim_vla;
			if (!pparse_array_bracket_closes_ptr_to_array(t, prev)) {
				r->is_array = true;
				if (!r->type_array_rank) {
					PParseToken *inner = pparse_next(_pc, t);
					r->array_dim_complete = !pparse_match_ch(inner, ']');
				}
				if (r->type_array_rank < 15) r->type_array_rank++;
				r->is_vla |= dim_vla;
			}
		}
		if (scan_vla && pparse_is_identifier_like(t)) {
			int tf = pparse_typedef_flags(t);
			r->is_vla |= (tf & PPARSE_TDF_VLA) != 0;
			if (tf & PPARSE_TDF_ARRAY) {
				r->is_array = true;
				uint8_t rk = pparse_array_rank_for_tok(t);
				if (rk > 0 && r->type_array_rank < rk)
					r->type_array_rank = rk;
				else if (r->type_array_rank == 0)
					r->type_array_rank = 1;
				PParseArrayBindingInfo info = pparse_array_binding_info(t);
				r->array_dim_complete |= info.tracked & info.dim_complete;
			}
			vla_done |= (tf & (PPARSE_TDF_VLA | PPARSE_TDF_ARRAY)) != 0;
		}

		outer_ptr |= (depth == 0) & pparse_match_ch(t, '*');
		if (copy_quals) {
			r->has_volatile |= (t->tag & PPARSE_TT_VOLATILE) != 0;
			bool has_const = (t->tag & PPARSE_TT_CONST) != 0;
			r->has_const |= has_const;
			r->has_decl_const |= has_const;
			r->has_atomic |= (t->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE)) ==
					 (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE);
		}
		int tf = pparse_typedef_flags(t);
		if (((t->tag & PPARSE_TT_SUE) || (tf & PPARSE_TDF_AGGREGATE)) && !r->object_type_idx)
			r->object_type_idx = pparse_idx(_pc, t) + 1;
		if (tf & PPARSE_TDF_CONST) {
			r->has_decl_const = r->has_hidden_const = true;
			if (copy_quals) r->has_const = true;
		}
		r->has_hidden_volatile |= (tf & (PPARSE_TDF_VOLATILE | PPARSE_TDF_HAS_VOL_MEMBER)) != 0;
		r->is_struct |= ((t->tag & PPARSE_TT_SUE) != 0) | ((tf & PPARSE_TDF_AGGREGATE) != 0);
		r->is_union |= ((t->tag & PPARSE_TT_SUE) != 0) & (t->ch0 == 'u');
		r->is_enum |= ((t->tag & PPARSE_TT_SUE) != 0) & (t->ch0 == 'e');
		r->is_union |= (tf & PPARSE_TDF_UNION) != 0;
		if (t->tag & PPARSE_TT_SUE) {
			saw_sue = true;
			continue;
		}
		if (!pparse_is_identifier_like(t)) continue;
		if (saw_sue) {
			PParseTypedefEntry *tag_e = pparse_tag_lookup(t);
			if (tag_e) {
				if (tag_e->is_vla) r->is_vla = true;
				if (tag_e->has_volatile_member) r->has_volatile_member = true;
			}
			saw_sue = false;
		}
		if (copy_quals) {
			r->has_volatile |= (tf & PPARSE_TDF_VOLATILE) != 0;
			r->has_volatile_member |= (tf & PPARSE_TDF_HAS_VOL_MEMBER) != 0;
		}
	}
	r->is_struct &= !outer_ptr;
	r->is_union &= !outer_ptr;
	r->is_enum &= !outer_ptr;
}

static bool pparse_typeof_operand_is_function(PParseToken *inner, PParseToken *close) {
	PPARSE_CTX();
	while (inner && inner != close && pparse_match_ch(inner, '(')) {
		PParseToken *inner_close = pparse_pair_known(inner);
		if (pparse_next(_pc, inner_close) != close) break;
		inner = pparse_next(_pc, inner);
		close = inner_close;
	}
	if (!inner || inner == close) return false;
	if (!(inner->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF))) {
		if (pparse_next(_pc, inner) != close || !pparse_is_valid_varname(inner)) return false;
		PParseTypedefEntry *binding = pparse_typedef_lookup(_pc, inner);
		return binding && binding->is_shadow ? binding->is_func
						     : pparse_function_symbol(inner) != PPARSE_FS_NONE;
	}
	PParseToken *prev = NULL;
	PPARSE_FOR_RANGE(s, inner, close) {
		if (pparse_is_c23_attr(s) || pparse_match_ch(s, '[')) {
			s = pparse_pair_known(s);
			prev = NULL;
			continue;
		}
		if (!pparse_match_ch(s, '(')) {
			prev = s;
			continue;
		}
		PParseToken *pair = pparse_pair_known(s);
		bool ctor = prev && ((prev->tag & (PPARSE_TT_TYPEOF | PPARSE_TT_BITINT |
						       PPARSE_TT_ALIGNAS | PPARSE_TT_ATTR)) ||
				     ((prev->tag & PPARSE_TT_TYPE) && pparse_equal(prev, "_Atomic")));
		if (ctor) {
			s = pair;
			prev = NULL;
			continue;
		}
		PParseToken *after = pparse_skip_noise(_pc, pparse_next(_pc, s));
		return !(after && pparse_match_ch(after, '*'));
	}
	return false;
}

// --- Type Specifier Parser ---

// Soft-kw typedefs skip soft-kw qualifiers when peeking the name; plain typedefs skip all quals.
static bool pparse_typespec_typedef_name_finishes(PParseToken *tok, bool soft) {
	PPARSE_CTX();
	PParseToken *peek = pparse_next(_pc, tok);
	while (peek && (peek->tag & PPARSE_TT_QUALIFIER) && (!soft || !pparse_is_soft_keyword_identifier(peek)))
		peek = pparse_next(_pc, peek);
	if (!peek || !pparse_is_valid_varname(peek)) return false;
	PParseToken *after = pparse_next(_pc, peek);
	return after && pparse_match_set(after, pparse_CH(';') | pparse_CH('[') | pparse_CH(',') | pparse_CH('='));
}

static PParseTypeSpec pparse_type_specifier_parse(PParseToken *tok) {
	PPARSE_CTX();
	PParseTypeSpec r = {.end = tok};
	bool saw_long = false, saw_double = false;
	while (tok && tok->kind != PPARSE_TK_EOF) {
		PParseToken *next = pparse_skip_noise(_pc, tok);
		if (next != tok) {
			tok = next;
			r.end = tok;
			continue;
		}
		int tflags = pparse_typedef_flags(tok);
		if (pparse_equal(tok, "__extension__")) {
			tok = pparse_next(_pc, tok);
			r.end = tok;
			continue;
		}

		if (!r.saw_type && (tok->flags & PPARSE_TF_RAW)) {
			PParseToken *after_raw = pparse_skip_noise(_pc, pparse_next(_pc, tok));
			bool typedef_kw_prefix =
			    (tflags & PPARSE_TDF_TYPEDEF) && after_raw &&
			    (pparse_is_type_keyword(after_raw) || pparse_is_known_typedef(after_raw) ||
			     (after_raw->tag &
			      (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_STORAGE | PPARSE_TT_INLINE | PPARSE_TT_TYPEDEF)) ||
			     (after_raw->flags & PPARSE_TF_RAW));
			if (!(tflags & PPARSE_TDF_TYPEDEF) || typedef_kw_prefix) {
				r.has_raw = true;
				PParseToken *raw = tok, *after = after_raw;
				while (after) {
					pparse_ann(raw) |= P1_RAW_DECL_RECIPE;
					raw->pair_idx = pparse_idx(_pc, after);
					if (!(after->flags & PPARSE_TF_RAW) || pparse_is_known_typedef(after))
						break;
					raw = after;
					after = pparse_skip_noise(_pc, pparse_next(_pc, raw));
				}
				tok = pparse_next(_pc, raw);
				r.end = tok;
				continue;
			}
		}

		uint32_t tag = tok->tag;
		if (tag & PPARSE_TT_TYPE) {
			saw_long |= tok->ch0 == 'l' && tok->len == 4;
			saw_double |= tok->ch0 == 'd' && tok->len == 6;
			r.has_long_double |= saw_long & saw_double;
		}
		bool is_type = (tag & PPARSE_TT_TYPE) || (tflags & PPARSE_TDF_TYPEDEF);
		if (r.saw_type && pparse_is_soft_keyword_identifier(tok) && pparse_soft_keyword_decl_name_boundary(tok))
			break;
		if (!(tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_INLINE)) && !is_type &&
		    !(tag & (PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS)))
			break;
		if ((tag & PPARSE_TT_INLINE) && !(tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE))) {
			tok = pparse_next(_pc, tok);
			r.end = tok;
			continue;
		}

		r.has_void |= pparse_equal(tok, "void") | ((tflags & PPARSE_TDF_VOID) != 0);
		bool had_type = r.saw_type;
		if ((tflags & PPARSE_TDF_TYPEDEF) && pparse_is_soft_keyword_identifier(tok)) {
			if (had_type) break;
			pparse_typedef_apply_tdf_flags(&r, tok, tflags);
			if (pparse_typespec_typedef_name_finishes(tok, true)) {
				tok = pparse_next(_pc, tok);
				r.end = tok;
				r.saw_type = true;
				return r;
			}
			tok = pparse_next(_pc, tok);
			r.end = tok;
			r.saw_type = true;
			continue;
		}

		if (tag & PPARSE_TT_STORAGE) {
			switch (tok->ch0) {
			case 'e': r.has_extern = true; break;
			case 's': r.has_static = true; break;
			default: r.has_thread_local = true; break;
			}
		}
		if (tag & PPARSE_TT_QUALIFIER) {
			r.has_volatile |= (tag & PPARSE_TT_VOLATILE) != 0;
			r.has_register |= (tag & PPARSE_TT_REGISTER) != 0;
			bool has_const = (tag & PPARSE_TT_CONST) != 0;
			r.has_const |= has_const;
			r.has_decl_const |= has_const;
			r.has_constexpr |= (tok->ch0 == 'c') & (tok->len == 9);
			if (tag & PPARSE_TT_TYPE) {
				bool auto_type = tok->ch0 == 'a';
				r.saw_type |= auto_type;
				r.has_auto |= auto_type;
				r.has_atomic |= !auto_type;
			}
		}

		bool atomic_kw = (tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE)) ==
				 (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE);
		PParseToken *atomic_open = pparse_next(_pc, tok);
		bool atomic_ctor = atomic_kw & pparse_match_ch(atomic_open, '(');
		if (is_type && atomic_kw && !atomic_ctor)
			is_type = false;
		if (is_type) r.saw_type = true;
		// _Atomic(type) specifier form
		if (atomic_ctor) {
			r.saw_type = true;
			r.has_atomic = true;
			tok = atomic_open;
			PParseToken *inner_start = pparse_skip_noise(_pc, pparse_next(_pc, tok));
			PParseToken *end = pparse_skip_balanced_group(tok);
			int inner_flags = pparse_typedef_flags(inner_start);
			r.is_typedef |= (inner_flags & PPARSE_TDF_TYPEDEF) != 0;
			PParseToken *shape_start = inner_start, *shape_end = end;
			/* Look through the nested constructor once; shape is about its
			 * operand, not the `typeof` keyword itself. */
			bool nested_typeof = (inner_start->tag & PPARSE_TT_TYPEOF) != 0;
			PParseToken *nested_open = pparse_next(_pc, inner_start);
			if (nested_typeof && pparse_match_ch(nested_open, '(')) {
				PParseToken *topen = nested_open;
				shape_start = pparse_next(_pc, topen);
				shape_end = pparse_skip_balanced_group(topen);
			}
			r.has_typeof |= nested_typeof;
			pparse_scan_type_constructor(shape_start, shape_end, &r, true, false);
			tok = end;
			r.end = tok;
			continue;
		}

		if (tag & PPARSE_TT_SUE) {
			if (!r.sue_kw) r.sue_kw = tok;
			if (!r.object_type_idx) r.object_type_idx = pparse_idx(_pc, tok) + 1;
			r.is_struct = true;
			r.is_union |= tok->ch0 == 'u';
			r.is_enum |= tok->ch0 == 'e';
			r.saw_type = true;
			tok = pparse_next(_pc, tok);
			while (tok->kind != PPARSE_TK_EOF) {
				pparse_SKIP_NOISE_CONTINUE(tok);
				if ((tok->tag & PPARSE_TT_QUALIFIER) && !pparse_is_soft_keyword_identifier(tok))
					tok = pparse_next(_pc, tok);
				else
					break;
			}
			PParseToken *sue_tag = NULL;
			if (pparse_is_valid_varname(tok)) {
				sue_tag = tok;
				tok = pparse_next(_pc, tok);
			}
			// C23 enum fixed underlying type: enum E : int { ... }
			if (pparse_match_ch(tok, ':')) {
				tok = pparse_next(_pc, tok);
				while (tok->kind != PPARSE_TK_EOF) {
					pparse_SKIP_NOISE_CONTINUE(tok);
					if (pparse_match_ch(tok, '(')) {
						tok = pparse_skip_balanced_group(tok);
						continue;
					}
					if (pparse_is_c23_attr(tok)) {
						tok = pparse_pair_known(tok);
						continue;
					}
					if (pparse_is_type_keyword(tok) || (tok->tag & PPARSE_TT_QUALIFIER)) {
						tok = pparse_next(_pc, tok);
						continue;
					}
					break;
				}
			}
			if (pparse_match_ch(tok, '{')) {
				pparse_ann(r.sue_kw) |= P1_SUE_BODY_RECIPE;
				r.sue_kw->pair_idx = pparse_idx(_pc, tok);
				if (r.is_enum || sue_tag) pparse_ann(tok) |= P1_SUE_SPLIT_STRIP;
				unsigned body_traits = pparse_struct_body_traits(tok);
				r.is_vla |= (body_traits & PPARSE_SBT_VLA) != 0;
				r.has_volatile_member |= (body_traits & PPARSE_SBT_VOL) != 0;
				tok = pparse_skip_balanced_group(tok);
			} else if (sue_tag) {
				PParseTypedefEntry *tag_e = pparse_tag_lookup(sue_tag);
				if (tag_e) {
					r.is_vla |= tag_e->is_vla;
					r.has_volatile_member |= tag_e->has_volatile_member;
				}
			}
			r.end = tok;
			continue;
		}

		if (tag & PPARSE_TT_TYPEOF) {
			bool is_unqual =
			    tok->len >= 13; // typeof_unqual(13), __typeof_unqual(15), __typeof_unqual__(17)
			r.saw_type = true;
			r.has_typeof = true;
			r.has_hidden_const |= tok->len != 11;
			r.has_auto |= pparse_equal(tok, "__auto_type");
			tok = pparse_next(_pc, tok);
			if (pparse_match_ch(tok, '(')) {
				PParseToken *close = pparse_pair_known(tok);
				PParseToken *end = pparse_next(_pc, close);
				PParseToken *inner = pparse_next(_pc, tok);
				r.has_void |= pparse_equal(inner, "void") & (pparse_next(_pc, inner) == close);
				/* Resolve the traits of a direct identifier operand. Scanning
				 * every identifier inside an arbitrary expression is unsound:
				 * `typeof(sizeof(atomic_var))` is size_t, not atomic. */
				PParseToken *operand = inner, *operand_close = close;
				while (pparse_match_ch(operand, '(') &&
				       pparse_next(_pc, pparse_pair_known(operand)) == operand_close) {
					operand_close = pparse_pair_known(operand);
					operand = pparse_next(_pc, operand);
				}
				if (pparse_next(_pc, operand) == operand_close &&
				    pparse_is_identifier_like(operand)) {
					int operand_flags = pparse_typedef_flags(operand);
					PParseTypedefEntry *operand_binding = pparse_typedef_lookup(_pc, operand);
					if (!is_unqual && operand_binding && operand_binding->is_const) {
						r.has_const = r.has_decl_const = r.has_hidden_const = true;
					}
					r.has_atomic |= !is_unqual & ((operand_flags & PPARSE_TDF_ATOMIC) != 0);
					r.has_long_double |= (operand_flags & PPARSE_TDF_LONG_DOUBLE) != 0;
				}
				pparse_scan_type_constructor(inner, end, &r, false, !is_unqual);
				r.is_func |= pparse_typeof_operand_is_function(inner, close);
				tok = end;
			}
			r.end = tok;
			continue;
		}

		if (tag & (PPARSE_TT_BITINT | PPARSE_TT_ATTR | PPARSE_TT_ALIGNAS)) {
			r.saw_type |= (tag & PPARSE_TT_BITINT) != 0;
			r.has_alignas |= (tag & PPARSE_TT_ALIGNAS) != 0;
			PParseToken *kw = tok;
			tok = pparse_next(_pc, tok);
			if (pparse_match_ch(tok, '(')) {
				if (pparse_feat(PPARSE_F_ORELSE) && (kw->tag & (PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS))) {
					PParseToken *close = pparse_pair_known(tok);
					PPARSE_FOR_RANGE(s, pparse_next(_pc, tok), close)
						if (pparse_is_orelse_kw_shadow(s))
							pparse_error_tok(
							    s,
							    "'orelse' cannot be used inside %s "
							    "(requires a compile-time constant expression)",
							    (kw->tag & PPARSE_TT_BITINT) ? "_BitInt()"
										  : "_Alignas()");
				}
				tok = pparse_skip_balanced_group(tok);
			}
			r.end = tok;
			continue;
		}

		tflags = pparse_typedef_flags(tok);
		if (tflags & PPARSE_TDF_TYPEDEF) {
			if (had_type) break;
			pparse_typedef_apply_tdf_flags(&r, tok, tflags);
			if (pparse_typespec_typedef_name_finishes(tok, false)) {
				tok = pparse_next(_pc, tok);
				r.end = tok;
				r.saw_type = true;
				return r;
			}
		}

		tok = pparse_next(_pc, tok);
		r.end = tok;
	}

	return r;
}

PPARSE_CACHED_PARSE(pparse_type_specifier,
			    pparse_type_specifier_parse,
			    PParseTypeSpec,
			    PPARSE_AR_TYPE,
			    type)
#undef PPARSE_CACHED_PARSE

static inline bool pparse_cached_type_specifier(PParseToken *tok, PParseTypeSpec *out) {
	PParseAnalysisRecord *r = pparse_analysis_get(tok, PPARSE_AR_TYPE);
	if (!r) return false;
	*out = r->as.type;
	return true;
}

static inline bool pparse_cached_declarator(PParseToken *tok, PParseDecl *out) {
	PParseAnalysisRecord *r = pparse_analysis_get(tok, PPARSE_AR_DECL);
	if (!r) return false;
	*out = r->as.decl;
	return true;
}

static inline PParseToken *pparse_cached_decl_sue_body(PParseToken *tok) {
	PPARSE_CTX();
	PParseTypeSpec type;
	tok = pparse_skip_noise(_pc, tok);
	return pparse_cached_type_specifier(tok, &type) && type.sue_kw
		   ? pparse_sue_body_recipe(type.sue_kw)
		   : NULL;
}

static inline PParseToken *pparse_bare_orelse_recipe(PParseToken *tok) {
	PPARSE_CTX();
	PParseAnalysisRecord *r = pparse_analysis_get(tok, PPARSE_AR_BARE_ORELSE);
	uint32_t oi = r ? r->as.token_idx : 0;
	return oi && oi < pparse_token_count ? &pparse_token_pool[oi] : NULL;
}

static void pparse_typedef_declaration(PParseToken *tok, int scope_depth) {
	PPARSE_CTX();
	tok = pparse_next(_pc, tok); // Skip 'typedef'
	PParseTypeSpec type_spec = pparse_type_specifier(tok);
	tok = type_spec.end;

	// redefinitions correctly shadow outer tags (C11 §6.2.1p4).
	if (type_spec.is_struct && type_spec.sue_kw) {
		PParseToken *tag = pparse_skip_noise(_pc, pparse_next(_pc, type_spec.sue_kw));
		while (tag && (tag->tag & PPARSE_TT_QUALIFIER) && !pparse_is_soft_keyword_identifier(tag))
			tag = pparse_skip_noise(_pc, pparse_next(_pc, tag));
		if (tag && pparse_is_valid_varname(tag)) {
			PParseTypedefEntry *te = pparse_typedef_add_entry(
			    tag, scope_depth, PPARSE_TDK_STRUCT_TAG, type_spec.is_vla, false);
			if (te) {
				te->is_aggregate = !type_spec.is_enum;
				if (type_spec.has_volatile_member) te->has_volatile_member = true;
			}
		}
	}
	while (tok && !(pparse_match_ch(tok, ';')) && tok->kind != PPARSE_TK_EOF) {
		PParseDecl decl = pparse_declarator(tok);
		if (decl.var_name) {
			pparse_ann(decl.var_name) |= P1_DEFER_SHADOW_NAME;
			bool is_void =
			    type_spec.has_void && !decl.is_pointer && !decl.is_array && !decl.is_func_ptr;
			bool is_const = (decl.is_pointer || decl.is_func_ptr) ? decl.is_const : type_spec.has_const;
			bool is_ptr = decl.is_pointer || decl.is_func_ptr || type_spec.is_ptr;
			bool is_array = (decl.is_array || type_spec.is_array) &&
					(!decl.is_pointer || decl.paren_array) && !decl.is_func_ptr;
			PParseTypedefEntry *added = pparse_typedef_add_entry(
			    decl.var_name, scope_depth, PPARSE_TDK_TYPEDEF, type_spec.is_vla || decl.is_vla, is_void);
			if (added) {
				added->is_const = is_const;
				added->is_volatile = (decl.is_pointer || decl.is_func_ptr)
							 ? decl.is_volatile
							 : type_spec.has_volatile;
				added->has_volatile_member = type_spec.has_volatile_member && !decl.is_pointer && !decl.is_func_ptr;
				added->is_ptr = is_ptr;
				added->is_array = is_array;
				if (is_array) {
					int rank = decl.array_rank + type_spec.type_array_rank;
					if (rank < 1) rank = 1;
					if (rank > 15) rank = PPARSE_ARRAY_RANK_WRAP_ALL;
					added->array_rank = (uint8_t)rank;
					added->array_dim_complete = decl.array_dim_complete ||
								    (decl.is_array && decl.end && pparse_match_ch(decl.end, '=')) ||
								    (type_spec.is_array && type_spec.array_dim_complete);
				}
				added->is_aggregate = type_spec.is_struct && !type_spec.is_enum &&
						      !decl.is_pointer && !decl.is_func_ptr;
				added->is_union = type_spec.is_union && !decl.is_pointer && !decl.is_func_ptr;
				added->is_long_double = type_spec.has_long_double && !is_ptr && !is_array;
				added->is_typeof = type_spec.has_typeof;
				if (decl.is_func_decl) added->is_func = true;
				added->is_atomic = (decl.is_pointer || decl.is_func_ptr)
						      ? decl.is_atomic
						      : type_spec.has_atomic;
				if (!decl.end) {
					PParseToken *after_name = pparse_skip_noise(_pc, pparse_next(_pc, decl.var_name));
					if (after_name && pparse_match_ch(after_name, '(')) added->is_func = true;
				}
				if (decl.is_func_ptr && !decl.paren_pointer) added->is_func = true;
				if (type_spec.is_func && !decl.is_pointer && !decl.is_array && !decl.is_func_ptr)
					added->is_func = true;
			}
		}
		tok = pparse_skip_to_set(decl.end ? decl.end : pparse_next(_pc, tok),
					 NULL,
					 pparse_CH(',') | pparse_CH(';'));
		if (tok && pparse_match_ch(tok, ',')) tok = pparse_next(_pc, tok);
	}
}

enum { PPARSE_DECL_CONST_EXPLICIT = 1, PPARSE_DECL_CONST_EFFECTIVE = 2 };

static unsigned pparse_decl_const_flags(const PParseTypeSpec *type, const PParseDecl *decl) {
	bool plain = !decl->is_func_ptr && !decl->is_pointer;
	bool explicit_const = (type->has_decl_const && plain) || decl->is_const;
	return (explicit_const ? PPARSE_DECL_CONST_EXPLICIT : 0) |
	       ((explicit_const || type->has_constexpr || (type->has_typeof && plain))
		    ? PPARSE_DECL_CONST_EFFECTIVE
		    : 0);
}

static PParseToken *pparse_sue_definition_body(PParseToken *sue_kw) {
	PPARSE_CTX();
	PParseToken *body = pparse_find_struct_body_brace(sue_kw);
	if (body) return body;

	PParseToken *tag = pparse_skip_noise(_pc, pparse_next(_pc, sue_kw));
	while (tag && (tag->tag & PPARSE_TT_QUALIFIER) && !pparse_is_soft_keyword_identifier(tag))
		tag = pparse_skip_noise(_pc, pparse_next(_pc, tag));
	if (!tag || !pparse_is_valid_varname(tag)) return NULL;

	PParseTypedefEntry *entry = pparse_tag_lookup(tag);
	if (!entry || !entry->is_struct_tag || entry->token_index >= pparse_token_count) return NULL;
	PParseToken *definition_tag = &pparse_token_pool[entry->token_index];
	PParseToken *definition_kw = pparse_walk_back(pparse_idx(_pc, definition_tag), PPARSE_WB_PAST_NOISE);
	if (!definition_kw || !(definition_kw->tag & PPARSE_TT_SUE) || pparse_is_enum_kw(definition_kw)) return NULL;
	return pparse_find_struct_body_brace(definition_kw);
}

static PParseToken *pparse_typedef_type_start(PParseTypedefEntry *entry) {
	PPARSE_CTX();
	if (!entry || entry->token_index >= pparse_token_count) return NULL;
	uint32_t i = entry->token_index;
	while (i > 0) {
		PParseToken *prev = &pparse_token_pool[i - 1];
		if (prev->flags & PPARSE_TF_CLOSE) {
			i = pparse_idx(_pc, pparse_pair_known(prev));
			continue;
		}
		if (pparse_match_ch(prev, ';')) return NULL;
		if (prev->tag & PPARSE_TT_TYPEDEF) return pparse_skip_noise(_pc, pparse_next(_pc, prev));
		i--;
	}
	return NULL;
}

enum { PPARSE_OBJ_ZERO_UNSAFE = 1, PPARSE_OBJ_CONST_SUBOBJECT = 2 };

static unsigned pparse_aggregate_object_traits(PParseToken *brace, int depth);

/* Follow the aggregate source retained by the type parse; no range scan. */
static unsigned pparse_type_object_traits(PParseToken *type_start, int depth) {
	PPARSE_CTX();
	if (!type_start || depth > 32) return 0;
	if (pparse_ann(type_start) & P1_TYPE_OBJ_KNOWN)
		return ((pparse_ann(type_start) & P1_TYPE_ZERO_UNSAFE) ? PPARSE_OBJ_ZERO_UNSAFE : 0) |
		       ((pparse_ann(type_start) & P1_TYPE_CONST_SUBOBJECT) ? PPARSE_OBJ_CONST_SUBOBJECT : 0);
	if (pparse_ann(type_start) & P1_TYPE_OBJ_BUSY) return 0;
	pparse_ann(type_start) |= P1_TYPE_OBJ_BUSY;
	unsigned traits = 0;
	PParseTypeSpec type = pparse_type_specifier(type_start);
	if (type.object_type_idx && type.object_type_idx <= pparse_token_count) {
		PParseToken *source = &pparse_token_pool[type.object_type_idx - 1];
		if ((source->tag & PPARSE_TT_SUE) && !pparse_is_enum_kw(source)) {
			PParseToken *body = pparse_sue_definition_body(source);
			if (body) traits = pparse_aggregate_object_traits(body, depth + 1);
		} else {
			PParseTypedefEntry *entry = pparse_typedef_lookup(_pc, source);
			PParseToken *alias = pparse_typedef_type_start(entry);
			if (alias) traits = pparse_type_object_traits(alias, depth + 1);
		}
	}
	pparse_ann(type_start) &= ~P1_TYPE_OBJ_BUSY;
	pparse_ann(type_start) |= P1_TYPE_OBJ_KNOWN;
	if (traits & PPARSE_OBJ_ZERO_UNSAFE) pparse_ann(type_start) |= P1_TYPE_ZERO_UNSAFE;
	if (traits & PPARSE_OBJ_CONST_SUBOBJECT) pparse_ann(type_start) |= P1_TYPE_CONST_SUBOBJECT;
	return traits;
}

/* Punctuation/keyword recipes live in pair_idx so stream jumps retain
 * parse_data.  Token kinds keep these payloads disjoint from delimiter pairs
 * and identifier bindings. */
#define pparse_decl_init_orelse(eq)                                                               \
	((eq) && pparse_match_ch((eq), '=') && (eq)->pair_idx && (eq)->pair_idx <= pparse_token_count \
	     ? &pparse_token_pool[(eq)->pair_idx - 1]                                                \
	     : NULL)
static inline PParseToken *pparse_orelse_payload(PParseToken *tok) {
	PPARSE_CTX();
	PParseToken *payload = pparse_ann_pair_recipe(tok, P1_IS_ORELSE_KW | P1_DECL_SPLIT);
	return payload && tok->pair_idx < pparse_token_count ? payload : NULL;
}
static inline PParseToken *pparse_bare_orelse_last_comma(PParseToken *eq) {
	PPARSE_CTX();
	return eq && pparse_match_ch(eq, '=') && eq->pair_idx && eq->pair_idx < pparse_token_count
		   ? &pparse_token_pool[eq->pair_idx]
		   : NULL;
}
static unsigned pparse_record_aggregate_traits(PParseToken *brace, unsigned traits) {
	pparse_ann(brace) &= ~P1_AGG_BUSY;
	pparse_ann(brace) |= P1_ZUNSAFE_KNOWN | P1_AGG_CONST_KNOWN;
	if (traits & PPARSE_OBJ_ZERO_UNSAFE) pparse_ann(brace) |= P1_ZUNSAFE;
	if (traits & PPARSE_OBJ_CONST_SUBOBJECT) pparse_ann(brace) |= P1_AGG_CONST;
	return traits;
}

static unsigned pparse_aggregate_object_traits(PParseToken *brace, int depth) {
	PPARSE_CTX();
	if (!brace || depth > 32) return 0;
	uint32_t known = P1_ZUNSAFE_KNOWN | P1_AGG_CONST_KNOWN;
	if ((pparse_ann(brace) & known) == known)
		return ((pparse_ann(brace) & P1_ZUNSAFE) ? PPARSE_OBJ_ZERO_UNSAFE : 0) |
		       ((pparse_ann(brace) & P1_AGG_CONST) ? PPARSE_OBJ_CONST_SUBOBJECT : 0);
	if (pparse_ann(brace) & P1_AGG_BUSY) return 0;
	pparse_ann(brace) |= P1_AGG_BUSY;

	PParseToken *end = pparse_pair_known(brace);
	bool saw_any = false, needs_full = false, maybe_const = false;
	for (PParseToken *t = pparse_next(_pc, brace); t && t != end;) {
		if (t->kind == PPARSE_TK_PREP_DIR) {
			t = pparse_next(_pc, t);
			continue;
		}
		if ((t->flags & PPARSE_TF_OPEN) && pparse_match_ch(t, '{')) {
			needs_full = saw_any = true;
			t = pparse_next(_pc, pparse_pair_known(t));
			continue;
		}
		if ((pparse_match_ch(t, '[') && !(t->flags & PPARSE_TF_C23_ATTR)) || (t->tag & PPARSE_TT_SUE))
			needs_full = true;
		int tf = pparse_is_identifier_like(t) ? pparse_typedef_flags(t) : 0;
		if ((t->tag & PPARSE_TT_CONST) || (tf & (PPARSE_TDF_CONST | PPARSE_TDF_AGGREGATE)))
			maybe_const = true;
		saw_any = true;
		t = pparse_next(_pc, t);
	}
	if (!saw_any) return pparse_record_aggregate_traits(brace, PPARSE_OBJ_ZERO_UNSAFE);
	if (!needs_full && !maybe_const) return pparse_record_aggregate_traits(brace, 0);

	bool saw_sized = false, saw_empty_or_zero = false, has_const = false;
	saw_any = false;
	for (PParseToken *stmt = pparse_skip_noise(_pc, pparse_next(_pc, brace)); stmt && stmt != end;) {
		if (stmt->kind == PPARSE_TK_PREP_DIR) {
			stmt = pparse_skip_noise(_pc, pparse_next(_pc, stmt));
			continue;
		}
		if ((stmt->flags & PPARSE_TF_STATIC_ASSERT) || (stmt->tag & PPARSE_TT_SKIP_DECL)) {
			PParseToken *n = pparse_skip_noise(_pc, pparse_next(_pc, stmt));
			stmt = n && pparse_match_ch(n, '(')
				   ? pparse_skip_noise(_pc, pparse_next(_pc, pparse_pair_known(n)))
				   : pparse_skip_noise(_pc, pparse_next(_pc, stmt));
			continue;
		}
		PParseTypeSpec member_type = pparse_type_specifier(stmt);
		if (!member_type.saw_type || !member_type.end) {
			PParseToken *next = pparse_skip_to_semicolon(stmt, end);
			stmt = next && pparse_match_ch(next, ';') ? pparse_skip_noise(_pc, pparse_next(_pc, next)) : end;
			continue;
		}

		PParseToken *decl_start = pparse_skip_noise(_pc, member_type.end);
		unsigned member_traits = pparse_type_object_traits(stmt, depth + 1);
		bool member_empty_type = (member_traits & PPARSE_OBJ_ZERO_UNSAFE) != 0;
		bool saw_decl = false, saw_named_decl = false;
		while (decl_start && decl_start != end && !pparse_match_ch(decl_start, ';')) {
			if (pparse_match_ch(decl_start, ':')) {
				saw_any = saw_sized = true;
				break;
			}
			PParseDecl member = pparse_declarator(decl_start);
			if (!member.end) break;
			saw_decl = saw_any = true;
			if (member.var_name) {
				saw_named_decl = true;
				if ((pparse_decl_const_flags(&member_type, &member) &
				     PPARSE_DECL_CONST_EXPLICIT) ||
				    (!member.is_pointer && !member.is_func_ptr &&
				     (member_traits & PPARSE_OBJ_CONST_SUBOBJECT)))
					has_const = true;
			}
			bool zero_arr = member.is_array && !member.is_pointer && member.has_zero_dim;
			if (member.is_pointer || member.is_func_ptr)
				saw_sized = true;
			else if (zero_arr || member_empty_type)
				saw_empty_or_zero = true;
			else
				saw_sized = true;

			PParseToken *next =
			    pparse_skip_to_set(member.end, end, pparse_CH(',') | pparse_CH(';'));
			if (next && pparse_match_ch(next, ',')) {
				decl_start = pparse_skip_noise(_pc, pparse_next(_pc, next));
				continue;
			}
			decl_start = next;
			break;
		}
		if (!saw_decl && member_empty_type) saw_any = saw_empty_or_zero = true;
		if (!saw_named_decl && (member_traits & PPARSE_OBJ_CONST_SUBOBJECT)) has_const = true;
		PParseToken *next_stmt = pparse_skip_to_semicolon(decl_start, end);
		stmt = next_stmt && pparse_match_ch(next_stmt, ';') ? pparse_skip_noise(_pc, pparse_next(_pc, next_stmt)) : end;
	}
	unsigned traits = (!saw_any || !saw_sized || saw_empty_or_zero) ? PPARSE_OBJ_ZERO_UNSAFE : 0;
	if (has_const) traits |= PPARSE_OBJ_CONST_SUBOBJECT;
	return pparse_record_aggregate_traits(brace, traits);
}

static bool pparse_range_has_attribute(PParseToken *start, PParseToken *end, uint32_t extra_tag) {
	PPARSE_CTX();
	PPARSE_FOR_RANGE(t, start, end) {
		if ((t->tag & (PPARSE_TT_ATTR | extra_tag)) || pparse_is_c23_attr(t)) return true;
		if (t->flags & PPARSE_TF_OPEN) t = pparse_pair_known(t);
	}
	return false;
}

static bool pparse_is_const_literal_initializer(PParseToken *eq) {
	PPARSE_CTX();
	PParseToken *t = pparse_next(_pc, eq);
	if (!t) return false;
	if (t->kind == PPARSE_TK_STR) {
		PParseToken *next = pparse_next(_pc, t);
		while (next && next->kind == PPARSE_TK_STR) next = pparse_next(_pc, next);
		return next && pparse_match_ch(next, ';');
	}
	if (!pparse_match_ch(t, '{')) return false;
	PParseToken *close = pparse_pair_known(t);
	bool prev_was_dot = false;
	for (t = pparse_next(_pc, t); t && t != close; t = pparse_next(_pc, t)) {
		if (t->kind == PPARSE_TK_NUM || t->kind == PPARSE_TK_STR) {
			prev_was_dot = false;
			continue;
		}
		if (t->kind == PPARSE_TK_PUNCT) {
			char c = t->ch0;
			if (c == ',' || c == '{' || c == '}' || c == '[' || c == ']' ||
			    (c == '=' && t->len == 1) || (c == '.' && t->len == 1) ||
			    ((c == '+' || c == '-') && t->len == 1)) {
				prev_was_dot = c == '.';
				continue;
			}
			return false;
		}
		if (t->kind == PPARSE_TK_IDENT &&
		    (prev_was_dot || pparse_is_known_enum_const(t) || pparse_equal(t, "true") || pparse_equal(t, "false"))) {
			prev_was_dot = false;
			continue;
		}
		return false;
	}
	return true;
}

static inline bool pparse_sos_ensure_intbuf(int **buf, int *cap, int need) {
	if (need <= *cap) return true;
	/* Soft-fail variant of pparse_VEC_ENSURE_REALLOC for pparse_skip_one_stmt OOM path. */
	size_t nc = pparse_vec_grow_cap((size_t)*cap, (size_t)need, 128);
	int *p = (int *)realloc(*buf, nc * sizeof(int));
	if (!p) return false;
	*buf = p;
	*cap = (int)nc;
	return true;
}

static inline bool pparse_sos_ensure_do(int need) {
	PPARSE_CTX();
	if (need <= pparse_sos_do_cap) return true;
	int nc = (int)pparse_vec_grow_cap((size_t)pparse_sos_do_cap, (size_t)need, 128);
	PParseSosDoFrame *p = realloc(pparse_sos_do_frames, (size_t)nc * sizeof(*p));
	if (!p) return false;
	pparse_sos_do_frames = p;
	pparse_sos_do_cap = nc;
	return true;
}

static PParseToken *pparse_skip_one_stmt_impl(PParseToken *tok, uint32_t *cache) {
	PPARSE_CTX();
	int if_depth = 0;
	int do_depth = 0;
	int do_snap_top = 0;
	uint32_t trail[256];
	int tn = 0;
	PParseSosDoFrame *do_frames = pparse_sos_do_frames;
	int *do_snap_buf = pparse_sos_do_snap_buf;
	int *if_trail_snap = pparse_sos_if_trail_snap;
restart:
	tok = pparse_skip_prep_dirs(tok);
	tok = pparse_skip_noise(_pc, tok);
	if (!tok || tok->kind == PPARSE_TK_EOF) return NULL;
	if (cache) {
		uint32_t idx = pparse_idx(_pc, tok);
		if (cache[idx]) {
			PParseToken *r = &pparse_token_pool[cache[idx] - 1];
			for (int i = 0; i < tn; i++) {
				uint32_t tix = trail[i];
				cache[tix] = cache[idx];
			}
			return r;
		}
		if (tn < 256) trail[tn++] = idx;
	}

	if (pparse_match_ch(tok, '{')) {
		tok = pparse_pair_known(tok);
		goto unwind_if;
	}

	if (tok->tag & PPARSE_TT_IF) {
		if (tok->ch0 == 'e') {
			tok = pparse_next(_pc, tok);
			goto restart;
		}
		PParseToken *p = pparse_skip_prep_dirs(pparse_next(_pc, tok));
		if (!p || !pparse_match_ch(p, '(')) return NULL;
		if (!pparse_sos_ensure_intbuf(&pparse_sos_if_trail_snap, &pparse_sos_if_cap, if_depth + 1))
			return NULL;
		if_trail_snap = pparse_sos_if_trail_snap;
		if_trail_snap[if_depth] = tn;
		if_depth++;
		tok = pparse_next(_pc, pparse_pair_known(p));
		goto restart;
	}

	if ((tok->tag & (PPARSE_TT_LOOP | PPARSE_TT_SWITCH)) && tok->ch0 != 'd') {
		PParseToken *p = pparse_skip_prep_dirs(pparse_next(_pc, tok));
		if (!p || !pparse_match_ch(p, '(')) return NULL;
		tok = pparse_next(_pc, pparse_pair_known(p));
		goto restart;
	}

	if ((tok->tag & PPARSE_TT_LOOP) && tok->ch0 == 'd') {
		if (!pparse_sos_ensure_do(do_depth + 1)) return NULL;
		if (!pparse_sos_ensure_intbuf(
			&pparse_sos_do_snap_buf, &pparse_sos_snap_cap, do_snap_top + if_depth))
			return NULL;
		do_frames = pparse_sos_do_frames;
		do_snap_buf = pparse_sos_do_snap_buf;
		do_frames[do_depth] = (PParseSosDoFrame){if_depth, tn, do_snap_top};
		for (int i = 0; i < if_depth; i++) do_snap_buf[do_snap_top++] = if_trail_snap[i];
		do_depth++;
		if_depth = 0;
		tok = pparse_next(_pc, tok);
		goto restart;
	}

	if (pparse_is_identifier_like(tok) &&
	    !(tok->tag & (PPARSE_TT_CASE | PPARSE_TT_DEFAULT | PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE))) {
		PParseToken *colon = pparse_skip_noise(_pc, pparse_next(_pc, tok));
		if (colon && pparse_match_ch(colon, ':') && !(pparse_next(_pc, colon) && pparse_match_ch(pparse_next(_pc, colon), ':'))) {
			tok = pparse_next(_pc, colon);
			goto restart;
		}
	}

	if ((tok->tag & (PPARSE_TT_CASE | PPARSE_TT_DEFAULT)) && !pparse_is_known_typedef(tok)) {
		int td = 0;
		PPARSE_FOR_TAIL(s, pparse_next(_pc, tok)) {
			PPARSE_SKIP_GROUP_ON_CLOSE(s)
			if (pparse_match_ch(s, '?')) {
				td++;
				continue;
			}
			if (pparse_match_ch(s, ':')) {
				if (td > 0) {
					td--;
					continue;
				}
				tok = pparse_next(_pc, s);
				goto restart;
			}
		}
		return NULL;
	}

	PPARSE_FOR_TAIL(s, tok) {
		PPARSE_SKIP_GROUP_ON_CLOSE(s)
		if (pparse_match_ch(s, ';')) {
			tok = s;
			goto unwind_if;
		}
	}
	return NULL;

unwind_if:
	while (if_depth > 0) {
		if_depth--;
		if (!tok) return NULL;
		PParseToken *n = pparse_skip_prep_dirs(pparse_next(_pc, tok));
		if (n && (n->tag & PPARSE_TT_IF) && n->ch0 == 'e') {
			int snap = if_trail_snap[if_depth];
			if (cache) {
				uint32_t val = pparse_idx(_pc, tok) + 1;
				for (int i = snap; i < tn; i++) {
					uint32_t tix = trail[i];
					cache[tix] = val;
				}
			}
			tn = snap; // keep parent tokens in trail for final resolution
			tok = pparse_next(_pc, n);
			goto restart;
		}
	}
	if (cache && tok) {
		uint32_t val = pparse_idx(_pc, tok) + 1;
		for (int i = 0; i < tn; i++) {
			uint32_t tix = trail[i];
			cache[tix] = val;
		}
	}
	if (do_depth > 0) {
		do_depth--;
		if_depth = do_frames[do_depth].if_depth;
		tn = do_frames[do_depth].trail_n;
		int snap_start = do_frames[do_depth].snap_start;
		int snap_count = do_snap_top - snap_start;
		for (int i = 0; i < snap_count; i++) if_trail_snap[i] = do_snap_buf[snap_start + i];
		do_snap_top = snap_start;
		if (!tok) goto unwind_if;
		PParseToken *w = pparse_skip_prep_dirs(pparse_next(_pc, tok));
		if (!w || !(w->tag & PPARSE_TT_LOOP) || w->ch0 != 'w') {
			tok = NULL;
			goto unwind_if;
		}
		PParseToken *p2 = pparse_skip_prep_dirs(pparse_next(_pc, w));
		if (!p2 || !pparse_match_ch(p2, '(')) {
			tok = NULL;
			goto unwind_if;
		}
		PParseToken *a = pparse_skip_prep_dirs(pparse_next(_pc, pparse_pair_known(p2)));
		tok = (a && pparse_match_ch(a, ';')) ? a : NULL;
		goto unwind_if;
	}
	return tok;
}

typedef struct {
	uint32_t open_tok_idx;	// token index of the '{' (0 for file scope)
	uint16_t parent_id;	// scope_id of enclosing '{' (0 = file scope)
	bool is_struct : 1;
	bool is_loop : 1;
	bool is_switch : 1;
	bool is_func_body : 1;
	bool is_stmt_expr : 1;
	bool is_conditional : 1;
	bool is_init : 1; // initializer brace: = { ... } — not a compound statement
} PParseScopeInfo;

#define pparse_scope_tree ((PParseScopeInfo *)_pc->p1_scope_tree)
#define pparse_scope_tree_count (_pc->p1_scope_count)
#define pparse_scope_close(si) ((si)->open_tok_idx ? pparse_token_pool[(si)->open_tok_idx].pair_idx : 0)

/* C23 `enum Tag : unsigned int {` — `prev` is the type keyword before `{`.
 * `enum` carries PPARSE_TT_TYPE, so check pparse_is_enum_kw before skipping type keywords.
 * Also accepts `enum E : typeof(unsigned) {` where prev is `)`. */
static bool pparse_is_c23_fixed_underlying_enum(PParseToken *type_kw_before_brace) {
	PPARSE_CTX();
	if (!type_kw_before_brace) return false;
	PParseToken *anchor = type_kw_before_brace;
	PParseToken *open = pparse_match_ch(anchor, ')') ? pparse_pair_known(anchor) : NULL;
	if (open) {
		PParseToken *kw = pparse_walk_back(pparse_idx(_pc, open), PPARSE_WB_ATTR_NOISE);
		if (kw && ((kw->tag & (PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS)) ||
			   ((kw->tag & PPARSE_TT_TYPE) && pparse_equal(kw, "_Atomic"))))
			anchor = kw;
		else
			return false;
	} else if (!pparse_is_type_keyword(anchor) && !pparse_is_known_typedef(anchor)) {
		return false;
	}
	/* si2-- form: no uint32 underflow at index 0, and pool[0] is inspected. */
	for (uint32_t si2 = pparse_idx(_pc, anchor); si2-- > 0;) {
		PParseToken *st = &pparse_token_pool[si2];
		if (st->kind == PPARSE_TK_PREP_DIR) continue;
		if (pparse_is_enum_kw(st)) return true;
		if (pparse_is_type_keyword(st) || (st->tag & PPARSE_TT_QUALIFIER) || pparse_is_known_typedef(st)) continue;
		if (pparse_match_ch(st, ':')) continue;
		if (pparse_match_ch(st, '*')) continue;
		PParseToken *group_open = (pparse_match_ch(st, ']') || pparse_match_ch(st, ')'))
						 ? pparse_pair_known(st)
						 : NULL;
		if (group_open && (pparse_match_ch(st, ')') || (group_open->flags & PPARSE_TF_C23_ATTR))) {
			si2 = pparse_idx(_pc, group_open);
			continue;
		}
		if (st->tag & PPARSE_TT_ATTR) continue;
		if (pparse_is_valid_varname(st)) continue; // enum tag name
		break;
	}
	return false;
}
static bool pparse_is_objc_ivar_brace(uint32_t brace_idx) {
	PPARSE_CTX();
	for (uint32_t i = brace_idx - 1; i > 0; i--) {
		PParseToken *t = &pparse_token_pool[i];
		if (t->kind == PPARSE_TK_PREP_DIR) continue;
		if ((t->kind == PPARSE_TK_IDENT && !t->tag) || pparse_match_ch(t, ':') || pparse_match_ch(t, '*') ||
		    (t->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_ATTR)))
			continue;
		PParseToken *group_open = (pparse_match_ch(t, ')') || pparse_match_ch(t, ']'))
						 ? pparse_pair_known(t)
						 : NULL;
		if (group_open && (pparse_match_ch(t, ')') || (group_open->flags & PPARSE_TF_C23_ATTR))) {
			i = pparse_idx(_pc, group_open);
			continue;
		}
		if (pparse_match_ch(t, '>')) {
			int depth = 1;
			while (i > 1 && depth > 0) {
				PParseToken *inner = &pparse_token_pool[--i];
				if (inner->kind == PPARSE_TK_PREP_DIR) continue;
				if (pparse_match_ch(inner, '>')) depth++;
				else if (pparse_match_ch(inner, '<')) depth--;
			}
			continue;
		}
		if (pparse_match_ch(t, '@')) {
			PParseToken *kw = &pparse_token_pool[i + 1];
			if (kw->kind == PPARSE_TK_IDENT &&
			    ((kw->len == 9 && prism_memeq_static(pparse_loc(_pc, kw), "interface", 9)) ||
			     (kw->len == 14 && prism_memeq_static(pparse_loc(_pc, kw), "implementation", 14)) ||
			     (kw->len == 8 && prism_memeq_static(pparse_loc(_pc, kw), "protocol", 8))))
				return true;
		}
		return false;
	}
	return false;
}
enum { PPARSE_CI_UNEVAL = 1, PPARSE_CI_ALIGNOF = 2, PPARSE_CI_OFFSETOF = 4,
	PPARSE_CI_TYPE_CTOR = 8 };
#define PPARSE_CTX_STACK_MAX 512
static unsigned pparse_context_intro(PParseToken *t);

/* One raw-token sweep stamps expression context and builds the scope index. */
static void pparse_build_scopes(PParseToken *start) {
	PPARSE_CTX();
	pparse_scope_tree_count = 1; // 0 is file scope
	_pc->p1_scope_tree = NULL;
	uint32_t scope_cap = 0;
	int stack_cap = 64, depth = 0, context_depth = 0;
	uint16_t *stack = pparse_arena_alloc_uninit(&_pc->main_arena, stack_cap * sizeof(uint16_t));
	stack[0] = 0;
	struct {
		uint32_t bits;
		bool is_generic, seen_comma, sticky_uneval;
	} context[PPARSE_CTX_STACK_MAX] = {0};
	for (PParseToken *t = start; t->kind != PPARSE_TK_EOF; t++) {
		uint32_t here = context[context_depth].bits;
		if (t->flags & PPARSE_TF_OPEN) {
			pparse_ann(t) |= here;
			if (context_depth + 1 < PPARSE_CTX_STACK_MAX) {
				bool paren = pparse_match_ch(t, '(');
				PParseToken *intro = paren
							 ? pparse_walk_back(pparse_idx(_pc, t), PPARSE_WB_ATTR_NOISE)
							 : NULL;
				unsigned prev_ci = pparse_context_intro(t - 1);
				unsigned intro_ci = pparse_context_intro(intro);
				int d = ++context_depth;
				context[d].bits = 0;
				context[d].seen_comma = false;
				context[d].sticky_uneval = false;
				/* A delimiter nested anywhere inside an unevaluated operand
				 * remains unevaluated across its own commas and child groups.
				 * Previously only a group immediately preceded by sizeof/typeof
				 * inherited the bit, so one redundant pair of parentheses made
				 * bounds checking reject dead pointer arithmetic. */
				bool inherited_uneval = ((here & P1_CTX_UNEVAL) != 0) |
							((prev_ci & PPARSE_CI_UNEVAL) != 0);
				context[d].bits |= inherited_uneval * P1_CTX_UNEVAL;
				context[d].sticky_uneval = inherited_uneval;
				context[d].is_generic = intro && (intro->tag & PPARSE_TT_GENERIC);
				if (!paren) {
					context[d].bits |= here &
						(P1_CTX_ALIGNOF_TYPE | P1_CTX_OFFSETOF | P1_CTX_GENERIC_ASSOC);
				} else {
					if ((intro_ci & PPARSE_CI_ALIGNOF) ||
					    ((intro_ci & PPARSE_CI_TYPE_CTOR) && (here & P1_CTX_ALIGNOF_TYPE)))
						context[d].bits |= P1_CTX_ALIGNOF_TYPE;
					if (intro_ci & PPARSE_CI_OFFSETOF) context[d].bits |= P1_CTX_OFFSETOF;
					if (!context[d].is_generic) context[d].bits |= here & P1_CTX_GENERIC_ASSOC;
				}
			}
		} else if (t->flags & PPARSE_TF_CLOSE) {
			if (context_depth > 0) context_depth--;
			pparse_ann(t) |= context[context_depth].bits;
		} else {
			pparse_ann(t) |= here;
			if (context[context_depth].is_generic) {
				if (pparse_match_ch(t, ',')) {
					context[context_depth].seen_comma = true;
					context[context_depth].bits |= P1_CTX_GENERIC_ASSOC;
					/* The first comma ends _Generic's unevaluated controlling
					 * expression; association expressions are ordinary evaluated
					 * expressions. Nested comma expressions are in child frames. */
					context[context_depth].bits &= ~P1_CTX_UNEVAL;
					context[context_depth].sticky_uneval = false;
				} else if (pparse_match_ch(t, ':') && context[context_depth].seen_comma) {
					context[context_depth].bits &= ~P1_CTX_GENERIC_ASSOC;
				}
			}
			if (pparse_context_intro(t) & PPARSE_CI_UNEVAL) {
				PParseToken *after = pparse_skip_noise(_pc, pparse_next(_pc, t));
				if (!pparse_match_ch(after, '(')) context[context_depth].bits |= P1_CTX_UNEVAL;
			} else if (!context[context_depth].sticky_uneval && t->len == 1 &&
				   ((t->ch0 == ';') | (t->ch0 == ',') | (t->ch0 == '{') | (t->ch0 == '}'))) {
				context[context_depth].bits &= ~P1_CTX_UNEVAL;
			}
		}
		if (t->ch0 != '{' && t->ch0 != '}') continue;
		if (t->ch0 == '}') {
			depth--;
			continue;
		}

		uint16_t sid = pparse_scope_tree_count;
		if (sid == UINT16_MAX) pparse_error_tok(t, "scope tree: too many scopes (>65534)");
		PPARSE_ARENA_ENSURE_CAP(
		    &_pc->main_arena, _pc->p1_scope_tree, pparse_scope_tree_count, scope_cap, 64, PParseScopeInfo);
		PParseScopeInfo *si = &pparse_scope_tree[sid];
		*si = (PParseScopeInfo){.parent_id = stack[depth],
				  .open_tok_idx = pparse_idx(_pc, t)};
		PParseToken *prev = pparse_walk_back(pparse_idx(_pc, t) - 1, PPARSE_WB_SKIP_NOISE);
		if (prev) {
			if (pparse_is_do_kw(prev)) {
				si->is_loop = true;
			} else if (pparse_match_ch(prev, ')')) {
				PParseToken *open = pparse_pair_known(prev);
				PParseToken *kw = pparse_walk_back(pparse_idx(_pc, open) - 1, PPARSE_WB_SKIP_NOISE);
				if (kw && (kw->tag & PPARSE_TT_ATTR)) kw = pparse_walk_back(pparse_idx(_pc, kw) - 1, PPARSE_WB_SKIP_ATTRS);
				if (kw) {
					if (kw->tag & PPARSE_TT_LOOP) si->is_loop = true;
					else if (kw->tag & PPARSE_TT_SWITCH) si->is_switch = true;
					else if (kw->tag & PPARSE_TT_IF) si->is_conditional = true;
					else if (kw->tag & PPARSE_TT_SUE) si->is_struct = true;
				}
				bool classified = si->is_loop | si->is_switch | si->is_conditional | si->is_struct;
				if (depth == 0 && !classified)
					si->is_func_body = true;
				if (depth > 0 && !classified && !si->is_func_body) {
					if (pparse_paren_is_function_params(open)) si->is_func_body = true;
					else si->is_init = true;
				}
			} else if (pparse_is_else_kw(prev)) {
				si->is_conditional = true;
			} else if (prev->tag & PPARSE_TT_SUE) {
				si->is_struct = true;
			} else if (pparse_is_valid_varname(prev) &&
				   !(prev->tag &
				     (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_LOOP | PPARSE_TT_SWITCH | PPARSE_TT_IF | PPARSE_TT_STORAGE))) {
				/* Soft keywords are legal S/U/E tag names. */
				PParseToken *sue = pparse_walk_back(pparse_idx(_pc, prev) - 1, PPARSE_WB_SKIP_ATTRS);
				if (sue && (sue->tag & PPARSE_TT_SUE)) {
					si->is_struct = true;
				}
			} else if (pparse_is_c23_fixed_underlying_enum(prev)) {
				si->is_struct = true;
			} else if (depth == 0 && (pparse_match_ch(prev, ']') || pparse_match_ch(prev, ';'))) {
				si->is_func_body = true;
			}
		}

		if (!(si->is_struct | si->is_loop | si->is_switch | si->is_conditional) &&
		    pparse_is_objc_ivar_brace(pparse_idx(_pc, t))) {
			si->is_struct = true;
			si->is_func_body = si->is_init = false;
		}
		if (prev && pparse_match_ch(prev, '(')) si->is_stmt_expr = true;
		if (!(si->is_func_body | si->is_loop | si->is_switch | si->is_conditional |
		      si->is_struct | si->is_stmt_expr)) {
			if (prev && pparse_match_ch(prev, '=')) si->is_init = true;
			else if (depth > 0 && stack[depth] < pparse_scope_tree_count && pparse_scope_tree[stack[depth]].is_init)
				si->is_init = true;
		}

		pparse_ann(t) = (uint16_t)si->is_loop * P1_SCOPE_LOOP |
			     (uint16_t)si->is_switch * P1_SCOPE_SWITCH |
			     (uint16_t)si->is_init * P1_SCOPE_INIT;
		bool reuse_parent =
		    si->is_init && depth > 0 && stack[depth] < pparse_scope_tree_count && pparse_scope_tree[stack[depth]].is_init;
		uint16_t actual_sid = reuse_parent ? stack[depth] : sid;
		t->parse_data = actual_sid;
		if (!reuse_parent) pparse_scope_tree_count++;
		PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena, stack, depth + 2, stack_cap, 256, uint16_t);
		depth++;
		stack[depth] = actual_sid;
	}
}
static bool pparse_scope_is_ancestor_or_self(uint16_t ancestor, uint16_t descendant) {
	PPARSE_CTX();
	for (uint16_t s = descendant; s != 0; s = pparse_scope_tree[s].parent_id)
		if (s == ancestor) return true;
	return ancestor == 0; // file scope is ancestor of everything
}

static int pparse_scope_tree_depth(uint16_t scope_id) {
	PPARSE_CTX();
	int depth = 0;
	for (uint16_t s = scope_id; s != 0; s = pparse_scope_tree[s].parent_id) depth++;
	return depth;
}

static int pparse_scope_block_exits(uint16_t goto_sid, uint16_t label_sid) {
	PPARSE_CTX();
	uint16_t a = goto_sid, b = label_sid;
	int da = pparse_scope_tree_depth(a), db = pparse_scope_tree_depth(b);
	while (da > db) {
		a = pparse_scope_tree[a].parent_id;
		da--;
	}
	while (db > da) {
		b = pparse_scope_tree[b].parent_id;
		db--;
	}
	while (a != b && a != 0) {
		a = pparse_scope_tree[a].parent_id;
		b = pparse_scope_tree[b].parent_id;
	}
	uint16_t lca = a;
	int exits = 0;
	for (uint16_t s = goto_sid; s != lca && s != 0; s = pparse_scope_tree[s].parent_id)
		if (!pparse_scope_tree[s].is_init) exits++;
	return exits;
}

static uint16_t pparse_scope_stmt_expr_ancestor(uint16_t scope_id) {
	PPARSE_CTX();
	for (uint16_t s = scope_id; s != 0; s = pparse_scope_tree[s].parent_id)
		if (s < pparse_scope_tree_count && pparse_scope_tree[s].is_stmt_expr) return s;
	return 0;
}

// Walk backward from before_idx skipping prep dirs, GNU attrs, C23 [[attrs]].

static void pparse_defer_scan_hidden_stmt_exprs(PParseToken *open, bool in_loop, bool in_switch, int depth);

static inline PParseToken *pparse_skip_defer_control_head(PParseToken *tok, bool in_loop, bool in_switch, int depth) {
	PPARSE_CTX();
	tok = pparse_skip_noise(_pc, tok);
	if (tok && pparse_match_ch(tok, '(')) {
		pparse_defer_scan_hidden_stmt_exprs(tok, in_loop, in_switch, depth);
		return pparse_next(_pc, pparse_pair_known(tok));
	}
	return tok;
}

static void pparse_validate_defer_control_flow(PParseToken *t, bool in_loop, bool in_switch) {
	if (!t) return;
	if (t->tag & PPARSE_TT_RETURN) pparse_error_tok(t, "'return' inside defer block bypasses remaining defers");
	if ((t->tag & PPARSE_TT_GOTO) && !pparse_is_known_typedef(t))
		pparse_error_tok(t, "'goto' inside defer block could bypass remaining defers");
	if ((t->tag & PPARSE_TT_BREAK) && !in_loop && !in_switch)
		pparse_error_tok(t, "'break' inside defer block bypasses remaining defers");
	if ((t->tag & PPARSE_TT_CONTINUE) && !in_loop)
		pparse_error_tok(t, "'continue' inside defer block bypasses remaining defers");
}

/* Shared recursion budget for every defer walker: statement nesting, hidden
 * statement expressions and parenthesised group nesting all draw on it. */
#define PPARSE_DEFER_MAX_DEPTH 4096

static PParseToken *pparse_validate_defer_statement(PParseToken *tok, bool in_loop, bool in_switch, int depth);

static PParseToken *pparse_defer_walk_advance_past_orelse(PParseToken *s, bool in_loop, bool in_switch, int depth) {
	PPARSE_CTX();
	PParseToken *act = pparse_next(_pc, s);
	if (act && pparse_match_ch(act, ';')) pparse_error_tok(s, PPARSE_ERR_ORELSE_EXPECT_STMT);
	pparse_validate_defer_control_flow(act, in_loop, in_switch);
	if (act && pparse_match_ch(act, '{')) {
		pparse_validate_defer_statement(act, in_loop, in_switch, depth + 1);
		return pparse_pair_known(act);
	}
	return act ? act : s;
}

/* `depth` is the shared defer-nesting budget enforced by
 * pparse_validate_defer_statement. This function recurses once per nested
 * `(`/`[` inside a defer statement and used to forward `depth` unchanged, so
 * it was the one defer walker with no bound at all: `defer g((((...1...))))`
 * with 100k parens segfaulted a release build, while the identical expression
 * without `defer` transpiled fine. Charging group nesting to the same budget
 * turns that into the diagnostic below. */
static void pparse_defer_scan_orelse_in_group(PParseToken *open, bool in_loop, bool in_switch, int depth) {
	PPARSE_CTX();
	if (depth >= PPARSE_DEFER_MAX_DEPTH)
		pparse_error_tok(open, "expression nesting depth inside 'defer' exceeds 4096");
	PParseToken *end = pparse_pair_known(open);
	PParseToken *prev = open;
	PPARSE_FOR_RANGE(s, pparse_next(_pc, open), end) {
		if (s->flags & PPARSE_TF_OPEN) {
			if (pparse_match_ch(s, '(') || pparse_match_ch(s, '['))
				pparse_defer_scan_orelse_in_group(s, in_loop, in_switch, depth + 1);
			prev = pparse_pair_known(s);
			s = prev;
			continue;
		}
		if (pparse_is_orelse_kw_shadow(s) && pparse_orelse_shadow_is_kw(prev)) {
			prev = pparse_defer_walk_advance_past_orelse(s, in_loop, in_switch, depth);
			s = prev;
			continue;
		}
		prev = s;
	}
}

static void pparse_defer_scan_hidden_stmt_exprs(PParseToken *open, bool in_loop, bool in_switch, int depth) {
	PPARSE_CTX();
	PParseToken *end = pparse_pair_known(open);
	for (PParseToken *t = pparse_next(_pc, open); t != end;) {
		if (pparse_is_stmt_expr_open(t)) {
			pparse_validate_defer_statement(pparse_next(_pc, t), in_loop, in_switch, depth + 1);
			t = pparse_next(_pc, pparse_pair_known(t));
		} else
			t = pparse_next(_pc, t);
	}
}

static PParseToken *pparse_validate_defer_statement(PParseToken *tok, bool in_loop, bool in_switch, int depth) {
	PPARSE_CTX();
	if (depth >= PPARSE_DEFER_MAX_DEPTH)
		pparse_error_tok(tok, "braceless control flow nesting depth exceeds 4096");
	tok = pparse_skip_noise(_pc, tok);
	if (!tok || tok->kind == PPARSE_TK_EOF) return tok;
	if (pparse_match_ch(tok, '{')) {
		PParseToken *end = pparse_pair_known(tok);
		uint32_t end_idx = pparse_idx(_pc, end);
		for (tok = pparse_skip_noise(_pc, pparse_next(_pc, tok));
		     tok && tok != end && tok->kind != PPARSE_TK_EOF && pparse_idx(_pc, tok) < end_idx;
		     tok = pparse_skip_noise(_pc, tok)) {
			PParseToken *next = pparse_validate_defer_statement(tok, in_loop, in_switch, depth);
			if (next == tok) break;
			tok = next;
		}
		return pparse_next(_pc, end);
	}

	if ((tok->tag & PPARSE_TT_IF) && tok->ch0 == 'i') {
		PParseToken *after_then = pparse_validate_defer_statement(
		    pparse_skip_defer_control_head(pparse_next(_pc, tok), in_loop, in_switch, depth),
		    in_loop,
		    in_switch,
		    depth + 1);
		PParseToken *else_tok = pparse_skip_noise(_pc, after_then);
		if (else_tok && (else_tok->tag & PPARSE_TT_IF) && else_tok->ch0 == 'e')
			return pparse_validate_defer_statement(pparse_next(_pc, else_tok), in_loop, in_switch, depth + 1);
		return after_then;
	}

	if (tok->tag & (PPARSE_TT_CASE | PPARSE_TT_DEFAULT)) {
		int td = 0;
		for (tok = pparse_next(_pc, tok); tok && tok->kind != PPARSE_TK_EOF; tok = pparse_next(_pc, tok)) {
			if ((tok->flags & PPARSE_TF_CLOSE) && tok->ch0 == '}') break;
			if (tok->flags & PPARSE_TF_OPEN) {
				if (pparse_match_ch(tok, '(') || pparse_match_ch(tok, '['))
					pparse_defer_scan_hidden_stmt_exprs(tok, in_loop, in_switch, depth);
				tok = pparse_pair_known(tok);
				continue;
			}
			if (pparse_match_ch(tok, '?')) {
				td++;
				continue;
			}
			if (pparse_match_ch(tok, ':')) {
				if (td > 0) {
					td--;
					continue;
				}
				break;
			}
		}
		return tok && pparse_match_ch(tok, ':')
			   ? pparse_validate_defer_statement(pparse_next(_pc, tok), in_loop, in_switch, depth + 1)
			   : tok;
	}

	if (tok->tag & PPARSE_TT_SWITCH)
		return pparse_validate_defer_statement(
		    pparse_skip_defer_control_head(pparse_next(_pc, tok), in_loop, true, depth), in_loop, true, depth + 1);
	if (tok->tag & PPARSE_TT_LOOP) {
		if (tok->ch0 == 'd') {
			tok = pparse_validate_defer_statement(pparse_next(_pc, tok), true, in_switch, depth + 1);
			PParseToken *w = pparse_skip_noise(_pc, tok);
			if (w && (w->tag & PPARSE_TT_LOOP) && w->ch0 == 'w') {
				tok = pparse_skip_defer_control_head(pparse_next(_pc, w), true, in_switch, depth);
				tok = pparse_skip_noise(_pc, tok);
				if (tok && pparse_match_ch(tok, ';')) tok = pparse_next(_pc, tok);
			}
			return tok;
		}
		return pparse_validate_defer_statement(
		    pparse_skip_defer_control_head(pparse_next(_pc, tok), true, in_switch, depth),
		    true,
		    in_switch,
		    depth + 1);
	}

	if (tok->flags & PPARSE_TF_OPEN) {
		if (pparse_is_stmt_expr_open(tok)) {
			PParseToken *inner_brace = pparse_next(_pc, tok);
			pparse_validate_defer_statement(inner_brace, in_loop, in_switch, depth + 1);
			return pparse_next(_pc, pparse_pair_known(tok));
		}
	}

	if (pparse_is_identifier_like(tok) && pparse_next(_pc, tok) && pparse_match_ch(pparse_next(_pc, tok), ':'))
		pparse_error_tok(tok,
			  "labels inside defer blocks produce duplicate labels "
			  "when the defer body is copied to multiple exit points");
	// is compile-time only.  Scan forward through decl-specifiers.
	PPARSE_FOR_TAIL(s, tok) {
		if (s->tag & PPARSE_TT_STORAGE) {
			if (s->ch0 != 'e' || !pparse_equal(s, "extern"))
				pparse_error_tok(s,
					  "'static' or thread-local storage inside defer block "
					  "creates duplicate state per exit path; hoist the "
					  "declaration outside the defer body");
			continue;
		}
		if (s->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_INLINE)) {
			if (s->flags & PPARSE_TF_OPEN) {
				s = pparse_pair_known(s);
			}
			continue;
		}
		break;
	}

	if (tok->kind == PPARSE_TK_KEYWORD) {
		pparse_validate_defer_control_flow(tok, in_loop, in_switch);
		PParseToken *next = pparse_next(_pc, tok);
		if ((tok->tag & PPARSE_TT_DEFER) && !pparse_is_known_typedef(tok) && !pparse_match_ch(next, ':') &&
		    !(next->tag & PPARSE_TT_ASSIGN))
			pparse_error_tok(tok, "nested defer is not supported");
	}

	if (pparse_feat(PPARSE_F_ORELSE)) {
		PParseToken *prev_oe = NULL;
		for (PParseToken *s = tok;
		     s && s->kind != PPARSE_TK_EOF && !pparse_match_ch(s, ';') && !((s->flags & PPARSE_TF_CLOSE) && s->ch0 == '}');
		     s = pparse_next(_pc, s)) {
			if (s->flags & PPARSE_TF_OPEN) {
				if (pparse_match_ch(s, '(') || pparse_match_ch(s, '['))
					pparse_defer_scan_orelse_in_group(s, in_loop, in_switch, depth);
				prev_oe = pparse_pair_known(s);
				s = prev_oe;
				continue;
			}
			if (pparse_is_orelse_kw_shadow(s) && (!prev_oe || pparse_orelse_shadow_is_kw(prev_oe))) {
				prev_oe = pparse_defer_walk_advance_past_orelse(s, in_loop, in_switch, depth);
				s = prev_oe;
				continue;
			}
			prev_oe = s;
		}
	}

	for (PParseToken *s = tok;
	     s && s->kind != PPARSE_TK_EOF && !pparse_match_ch(s, ';') && !((s->flags & PPARSE_TF_CLOSE) && s->ch0 == '}');
	     s = pparse_next(_pc, s)) {
		if (s->flags & PPARSE_TF_OPEN) {
			if (pparse_is_stmt_expr_open(s))
				pparse_validate_defer_statement(pparse_next(_pc, s), in_loop, in_switch, depth + 1);
			else if (pparse_match_set(s, pparse_CH('(') | pparse_CH('[')) || pparse_match_ch(s, '{'))
				pparse_defer_scan_hidden_stmt_exprs(s, in_loop, in_switch, depth);
			s = pparse_pair_known(s);
			continue;
		}
	}
	PParseToken *semi = pparse_skip_to_semicolon(tok, NULL);
	return semi->kind != PPARSE_TK_EOF ? pparse_next(_pc, semi) : semi;
}

static bool pparse_is_knr_params(PParseToken *start, PParseToken *brace) {
	PPARSE_CTX();
	if (!start || start == brace || pparse_match_ch(start, ';')) return false;
	PPARSE_FOR_RANGE(t, start, brace) {
		if (pparse_match_ch(t, ';')) return true;
		PPARSE_SKIP_GROUP_ON_CLOSE(t)
	}
	return false;
}

/* Fold glibc C23 _Generic function declarations back to `(name)(params)`. */

static bool pparse_params_look_like_decls(PParseToken *open) {
	PPARSE_CTX();
	PParseToken *close = pparse_pair_known(open);
	PPARSE_FOR_RANGE(t, pparse_next(_pc, open), close) {
		PPARSE_SKIP_GROUP_LENIENT(t)
		if (t->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_ATTR | PPARSE_TT_STORAGE))
			return true;
		if (pparse_is_known_typedef(t)) return true;
	}
	return false;
}

static PParseToken *pparse_generic_find_assoc_start(PParseToken *open) {
	PPARSE_CTX();
	PParseToken *close = pparse_pair_known(open);
	PPARSE_FOR_RANGE(t, pparse_next(_pc, open), close) {
		PPARSE_SKIP_GROUP_LENIENT(t)
		if (pparse_match_ch(t, ',')) return pparse_next(_pc, t);
	}
	return NULL;
}
static bool pparse_generic_has_distinct_targets(PParseToken *assoc_start, PParseToken *close) {
	PPARSE_CTX();
	const char *first_name = NULL;
	uint32_t first_len = 0;
	PParseToken *first_args_open = NULL;
	PParseToken *first_args_close = NULL;
	int ternary_depth = 0;
	PPARSE_FOR_RANGE(t, assoc_start, close) {
		PPARSE_SKIP_GROUP_LENIENT(t)
		if (pparse_match_ch(t, '?')) {
			ternary_depth++;
			continue;
		}
		if (!pparse_match_ch(t, ':')) continue;
		if (ternary_depth > 0) {
			ternary_depth--;
			continue;
		}
		bool found_ident = false;
		int inner_ternary = 0;
		PPARSE_FOR_RANGE(b, pparse_next(_pc, t), close) {
			PPARSE_SKIP_GROUP_LENIENT(b)
			if (pparse_match_ch(b, ',') && inner_ternary == 0) break;
			if (pparse_match_ch(b, '?')) {
				inner_ternary++;
				continue;
			}
			if (pparse_match_ch(b, ':') && inner_ternary > 0) {
				inner_ternary--;
				continue;
			}
			if (inner_ternary > 0) continue;
			if (!pparse_is_valid_varname(b)) continue;
			{
				PParseToken *bn = pparse_next(_pc, b);
				if (bn && pparse_match_ch(bn, '?')) {
					inner_ternary++;
					b = bn;
					continue;
				}
			}
			found_ident = true;
			while (b && pparse_next(_pc, b) && pparse_next(_pc, b) != close && (pparse_next(_pc, b)->tag & PPARSE_TT_MEMBER) &&
			       pparse_next(_pc, pparse_next(_pc, b)) && pparse_is_valid_varname(pparse_next(_pc, pparse_next(_pc, b)))) {
				b = pparse_next(_pc, pparse_next(_pc, b));
			}
			if (!first_name) {
				first_name = pparse_loc(_pc, b);
				first_len = b->len;
				PParseToken *ao = pparse_next(_pc, b);
				if (ao && pparse_match_ch(ao, '(')) {
					first_args_open = ao;
					first_args_close = pparse_pair_known(ao);
				}
			} else if (b->len != first_len ||
				   !prism_memeq_runtime_sized(pparse_loc(_pc, b), first_name, first_len)) {
				return true;
			} else {
				PParseToken *ao = pparse_next(_pc, b);
				if (!ao || !pparse_match_ch(ao, '(')) {
					if (first_args_open) return true;
				} else {
					PParseToken *ac = pparse_pair_known(ao);
					if (!first_args_open) return true;
					PParseToken *a1 = pparse_next(_pc, first_args_open);
					PParseToken *a2 = pparse_next(_pc, ao);
					while (a1 && a1 != first_args_close && a2 && a2 != ac) {
						if (a1->kind != a2->kind || a1->len != a2->len ||
						    !prism_memeq_runtime_sized(pparse_loc(_pc, a1), pparse_loc(_pc, a2), a1->len))
							return true;
						a1 = pparse_next(_pc, a1);
						a2 = pparse_next(_pc, a2);
					}
					if ((a1 != first_args_close) || (a2 != ac)) return true;
				}
			}
			break;
		}
		if (!found_ident) {
			bool has_real_ident = false;
			PParseToken *assoc_end = pparse_skip_to_set(
			    pparse_next(_pc, t), close, pparse_CH(','));
			PPARSE_FOR_RANGE(d, pparse_next(_pc, t), assoc_end) {
				if (pparse_is_valid_varname(d) &&
				    !(d->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_STORAGE | PPARSE_TT_ATTR | PPARSE_TT_TYPEOF |
						PPARSE_TT_BITINT))) {
					has_real_ident = true;
					break;
				}
			}
			if (!has_real_ident) return true;
		}
	}
	return false;
}
static bool pparse_generic_rewrite_preamble(PParseToken *generic_tok,
				     PParseToken **open_out,
				     PParseToken **close_out,
				     PParseToken **after_out,
				     PParseToken **assoc_start_out) {
	PPARSE_CTX();
	PParseToken *open = pparse_next(_pc, generic_tok);
	if (!open || !pparse_match_ch(open, '(')) return false;
	PParseToken *close = pparse_pair_known(open);
	PParseToken *after = pparse_skip_noise(_pc, pparse_next(_pc, close));
	PParseToken *assoc_start = pparse_generic_find_assoc_start(open);
	if (!assoc_start || pparse_generic_has_distinct_targets(assoc_start, close)) return false;
	*open_out = open;
	*close_out = close;
	*after_out = after;
	*assoc_start_out = assoc_start;
	return true;
}
static bool pparse_generic_decl_rewrite_target(PParseToken *generic_tok,
					PParseToken **name_out,
					PParseToken **params_open_out,
					PParseToken **params_close_out,
					PParseToken **next_out) {
	PPARSE_CTX();
	PParseToken *open, *close, *after, *assoc_start;
	if (!pparse_generic_rewrite_preamble(generic_tok, &open, &close, &after, &assoc_start)) return false;
	if (pparse_match_set(after, pparse_CH(';') | pparse_CH(',')) || (after->tag & PPARSE_TT_ATTR) || pparse_is_c23_attr(after)) {
		PPARSE_FOR_RANGE(t, assoc_start, close) {
			PParseToken *name = t;
			PParseToken *call_open = pparse_skip_noise(_pc, pparse_next(_pc, t));
			/* Plain `name(params)` association. */
			if (pparse_is_valid_varname(t) && call_open && pparse_match_ch(call_open, '(') &&
			    pparse_params_look_like_decls(call_open)) {
				*name_out = name;
				*params_open_out = call_open;
				*params_close_out = pparse_pair_known(call_open);
				*next_out = after;
				return true;
			}
			/* Glibc-style parenthesized / cast-wrapped name:
			 * `(name)(params)` or `(const char *)(name)(params)`. */
			if (!pparse_match_ch(t, '(')) continue;
			PParseToken *inner = pparse_skip_noise(_pc, pparse_next(_pc, t));
			PParseToken *paren_close = pparse_pair_known(t);
			PParseToken *after_paren = pparse_skip_noise(_pc, pparse_next(_pc, paren_close));
			/* Peel one layer of cast-like `(type)(name)` before `(params)`. */
			if (inner && !pparse_is_valid_varname(inner) && after_paren && pparse_match_ch(after_paren, '(')) {
				PParseToken *maybe_name = pparse_skip_noise(_pc, pparse_next(_pc, after_paren));
				PParseToken *name_close = pparse_pair_known(after_paren);
				PParseToken *params = pparse_skip_noise(_pc, pparse_next(_pc, name_close));
				if (maybe_name && pparse_is_valid_varname(maybe_name) &&
				    pparse_skip_noise(_pc, pparse_next(_pc, maybe_name)) == name_close &&
				    pparse_match_ch(params, '(') &&
				    pparse_params_look_like_decls(params)) {
					*name_out = maybe_name;
					*params_open_out = params;
					*params_close_out = pparse_pair_known(params);
					*next_out = after;
					return true;
				}
			}
			/* `(name)(params)` */
			if (inner && pparse_is_valid_varname(inner) &&
			    pparse_skip_noise(_pc, pparse_next(_pc, inner)) == paren_close && after_paren &&
			    pparse_match_ch(after_paren, '(') &&
			    pparse_params_look_like_decls(after_paren)) {
				*name_out = inner;
				*params_open_out = after_paren;
				*params_close_out = pparse_pair_known(after_paren);
				*next_out = after;
				return true;
			}
		}
	}
	if (pparse_match_ch(after, '(') && pparse_params_look_like_decls(after)) {
		PParseToken *ext_close = pparse_pair_known(after);
		PParseToken *after_ext = pparse_skip_noise(_pc, pparse_next(_pc, ext_close));
		if (after_ext &&
		    (pparse_match_ch(after_ext, ';') || pparse_match_ch(after_ext, ',') || (after_ext->tag & PPARSE_TT_ATTR) ||
		     pparse_is_c23_attr(after_ext))) {
			PParseToken *found = NULL;
			PPARSE_FOR_RANGE(t, assoc_start, close) {
				if (pparse_is_valid_varname(t)) found = t;
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

typedef struct {
	PParseToken *name, *params_open, *params_close, *after;
} PParseGenericDeclRecipe;

static inline bool pparse_generic_decl_context(PParseToken *tok) {
	PPARSE_CTX();
	PParseToken *prev = pparse_walk_back(pparse_idx(_pc, tok), PPARSE_WB_ATTR_NOISE);
	return prev &&
	       (pparse_match_ch(prev, '*') || pparse_match_ch(prev, ')') ||
		(prev->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE |
			      PPARSE_TT_SKIP_DECL | PPARSE_TT_ATTR | PPARSE_TT_INLINE |
			      PPARSE_TT_STORAGE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT)) ||
		pparse_is_known_typedef(prev));
}

static inline bool pparse_generic_decl_recipe(PParseToken *tok, PParseGenericDeclRecipe *r) {
	PPARSE_CTX();
	r->name = pparse_ann_pair_recipe(tok, P1_GENERIC_DECL_RECIPE);
	if (!r->name) return false;
	r->params_open = pparse_ann_pair_recipe(r->name, P1_GENERIC_DECL_TARGET);
	if (!r->params_open) return false;
	r->params_close = pparse_pair_known(r->params_open);
	PParseToken *generic_open = pparse_next(_pc, tok);
	PParseToken *generic_close = pparse_pair_known(generic_open);
	PParseToken *after_generic = pparse_skip_noise(_pc, pparse_next(_pc, generic_close));
	r->after = after_generic == r->params_open && r->params_close
			 ? pparse_skip_noise(_pc, pparse_next(_pc, r->params_close))
			 : after_generic;
	return r->params_close && r->after;
}

static bool pparse_noreturn_is_unevaluated(PParseToken *tok) {
	PPARSE_CTX();
	if (pparse_token_is_in_unevaluated_operand(tok)) return true;
	PParseToken *t = pparse_walk_back(pparse_idx(_pc, tok), PPARSE_WB_ATTR_NOISE);
	for (;;) {
		if (t && pparse_match_ch(t, ')') && pparse_close_paren_ends_cast_type_name(t))
			t = pparse_pair_known(t);
		else if (t && pparse_match_ch(t, '(') && (t->flags & PPARSE_TF_OPEN)) {
		} else if (t && t->kind == PPARSE_TK_IDENT && pparse_equal(t, "__extension__")) {
		} else if (t && t->kind == PPARSE_TK_PUNCT && t->len == 1 && strchr("+-!~", t->ch0)) {
			PParseToken *before = pparse_walk_back(pparse_idx(_pc, t), PPARSE_WB_ATTR_NOISE);
			if ((t->ch0 == '+' || t->ch0 == '-') && before && pparse_is_expr_ending(before) &&
			    !pparse_is_sizeof_like(before) &&
			    !(before->kind == PPARSE_TK_IDENT && pparse_equal(before, "__extension__")) &&
			    !pparse_close_paren_ends_cast_type_name(before))
				break;
		} else
			break;
		t = pparse_walk_back(pparse_idx(_pc, t), PPARSE_WB_PAST_NOISE);
	}
	return t && pparse_is_sizeof_like(t);
}

static inline PParseToken *pparse_try_detect_noreturn_call(PParseToken *tok) {
	PPARSE_CTX();
	if (!(tok->tag & PPARSE_TT_NORETURN_FN)) return NULL;
	// Respect C scoping: if a local variable/parameter shadows a noreturn
	PParseTypedefEntry *te = pparse_typedef_lookup(_pc, tok);
	if (te && te->is_shadow) return NULL;
	if (pparse_noreturn_is_unevaluated(tok)) return NULL;
	if (pparse_idx(_pc, tok) >= 1) {
		PParseToken *prev = pparse_walk_back(pparse_idx(_pc, tok), PPARSE_WB_PAST_NOISE);
		if (prev && (prev->tag & PPARSE_TT_MEMBER)) return NULL;
		if (prev && (prev->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_INLINE | PPARSE_TT_SUE)))
			return NULL;
		if (prev && pparse_match_ch(prev, '*')) return NULL;
		/* Short-circuit: `x && die();` / `x || die();` — die may not run. */
		if (prev && prev->kind == PPARSE_TK_PUNCT && prev->len == 2 &&
		    ((prev->ch0 == '&' && pparse_loc(_pc, prev)[1] == '&') ||
		     (prev->ch0 == '|' && pparse_loc(_pc, prev)[1] == '|')))
			return NULL;
	}
	PParseToken *call = pparse_next(_pc, tok);
	if (!call || !pparse_match_ch(call, '(')) return NULL;
	PParseToken *after = pparse_next(_pc, pparse_pair_known(call));
	/* Accept statement-final calls through grouping, casts and comma tails. */
	while (after && pparse_match_ch(after, ')')) after = pparse_next(_pc, after);
	if (after && pparse_match_ch(after, ';')) return after;
	if (after && pparse_match_ch(after, ',')) {
		PPARSE_FOR_TAIL(s, pparse_next(_pc, after)) {
			PPARSE_SKIP_GROUP_ON_CLOSE(s)
			if (s->flags & PPARSE_TF_CLOSE) break;
			if (pparse_match_ch(s, ';')) return s;
		}
	}
	return NULL;
}

static inline PParseToken *pparse_noreturn_call_end(PParseToken *tok) {
	return tok && (tok->tag & PPARSE_TT_NORETURN_FN)
		   ? pparse_ann_pair_recipe(tok, P1_NORETURN_CALL_RECIPE)
		   : NULL;
}

static inline int pparse_goto_block_exits(PParseToken *tok) {
	return (tok->tag & PPARSE_TT_GOTO) ? (int)tok->parse_data : 0;
}

static inline PParseToken *pparse_defer_body_end(PParseToken *tok) {
	return pparse_ann_pair_recipe(tok, P1_IS_DEFER_KW | P1_DEFER_BODY_RECIPE);
}

static inline PParseToken *pparse_p1d_find_open_paren(PParseToken *tok) {
	PPARSE_CTX();
	PParseToken *open = pparse_skip_prep_dirs(pparse_next(_pc, tok));
	return pparse_match_ch(open, '(') ? open : NULL;
}

static PParseToken *pparse_p1_knr_find_close_paren(PParseToken *semi_tok) {
	PPARSE_CTX();
	for (uint32_t pi = pparse_idx(_pc, semi_tok); pi > 0; pi--) {
		PParseToken *pt = &pparse_token_pool[pi - 1];
		if (pt->kind == PPARSE_TK_PREP_DIR) continue;
		if (pparse_match_ch(pt, '{') || pparse_match_ch(pt, '}')) return NULL;
		if (pparse_match_ch(pt, ')')) {
			PParseToken *open = pparse_pair_known(pt);
			bool is_ident_list = true;
			PPARSE_FOR_RANGE(t, pparse_next(_pc, open), pt) {
				if (!pparse_is_valid_varname(t) && !pparse_match_ch(t, ',') && t->kind != PPARSE_TK_PREP_DIR) {
					is_ident_list = false;
					break;
				}
			}
			if (is_ident_list) return pt;
			pi = pparse_idx(_pc, open) + 1; // +1 because loop does pi--
			continue;
		}
		if (pt->flags & PPARSE_TF_CLOSE) {
			pi = pparse_idx(_pc, pparse_pair_known(pt)) + 1;
			continue;
		}
	}
	return NULL;
}

static inline PParseToken *pparse_mark_raw_decl(PParseToken *raw_tok, PParseToken *after) {
	PPARSE_CTX();
	pparse_ann(raw_tok) |= P1_RAW_DECL_RECIPE;
	raw_tok->pair_idx = pparse_idx(_pc, after);
	return after;
}

static inline PParseToken *pparse_raw_decl_strip_after(PParseToken *probe) {
	PPARSE_CTX();
	if (!probe || !(probe->flags & PPARSE_TF_RAW)) return NULL;
	PParseToken *after = pparse_skip_noise(_pc, pparse_next(_pc, probe));
	if (!after) return NULL;
	/* Typedef-named `raw` is a keyword prefix only before another type
	 * (`raw raw x` / `raw int x`), never before `*` / `(`` / a declarator
	 * name where `raw` itself is the typedef type. */
	if (pparse_is_known_typedef(probe)) {
		if (pparse_is_type_keyword(after) || pparse_is_known_typedef(after) ||
		    (after->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_STORAGE | PPARSE_TT_INLINE | PPARSE_TT_TYPEDEF)) ||
		    (after->flags & PPARSE_TF_RAW)) {
			return pparse_mark_raw_decl(probe, after);
		}
		return NULL;
	}
	PParseToken *inner = pparse_match_ch(after, '(') ? pparse_skip_noise(_pc, pparse_next(_pc, after)) : NULL;
	bool grouped_pointer = inner && (pparse_match_ch(inner, '*') || pparse_match_ch(inner, '('));
	if ((pparse_is_valid_varname(after) && !pparse_is_type_keyword(after) && !pparse_is_known_typedef(after) &&
	     !(after->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE))) ||
	    pparse_match_ch(after, '*') || grouped_pointer) {
		return pparse_mark_raw_decl(probe, after);
	}
	return NULL;
}

static inline PParseToken *pparse_raw_strip_recipe(PParseToken *probe) {
	return probe && (probe->flags & PPARSE_TF_RAW)
		   ? pparse_ann_pair_recipe(probe, P1_RAW_DECL_RECIPE)
		   : NULL;
}

static inline PParseToken *pparse_p1_skip_decl_raw(PParseToken *t, bool *saw_raw) {
	PPARSE_CTX();
	PParseToken *probe = pparse_skip_noise(_pc, t), *after = NULL;
	bool stripped = false;
	while ((after = pparse_raw_decl_strip_after(probe))) {
		*saw_raw = stripped = true;
		probe = after;
	}
	return stripped ? probe : t;
}

static inline bool pparse_is_assignment_operator_token(PParseToken *tok) {
	PPARSE_CTX();
	return (tok->tag & PPARSE_TT_ASSIGN) && pparse_loc(_pc, tok)[tok->len - 1] == '=';
}

typedef struct {
	PParseToken *last_comma;
	PParseToken *segment_assignment;
} PParseExprTopLevel;

/* One balanced scan answers both questions needed by bare-expression
 * lowering. An assignment before the last comma belongs to an earlier comma
 * operand, so reset it when the active segment changes. */
static PParseExprTopLevel pparse_scan_expr_top_level(PParseToken *start, PParseToken *end) {
	PPARSE_CTX();
	PParseExprTopLevel result = {0};
	PPARSE_FOR_RANGE(t, start, end) {
		PPARSE_SKIP_GROUP_ON_CLOSE(t)
		if (pparse_match_ch(t, ',')) {
			result.last_comma = t;
			result.segment_assignment = NULL;
		} else if (pparse_is_assignment_operator_token(t)) {
			result.segment_assignment = t;
		}
	}
	return result;
}

static bool pparse_raw_after_subscript_open_bracket(PParseToken *raw_kw) {
	PPARSE_CTX();
	if (!raw_kw || !(raw_kw->flags & PPARSE_TF_RAW)) return false;
	uint32_t ri = pparse_idx(_pc, raw_kw);
	if (ri == 0) return false;
	/* pparse_walk_back(k, PPARSE_WB_PAST_NOISE) inspects pool[k-1] first — pass ri, not ri-1. */
	PParseToken *b = pparse_walk_back(ri, PPARSE_WB_PAST_NOISE);
	// `[` from `[[attr]]` is tagged PPARSE_TF_C23_ATTR — not an array subscript.
	return b && pparse_match_ch(b, '[') && !(b->flags & PPARSE_TF_C23_ATTR);
}

static bool pparse_is_raw_declaration_context(PParseToken *raw_kw, PParseToken *after_raw) {
	PPARSE_CTX();
	if (pparse_raw_after_subscript_open_bracket(raw_kw)) return false;
	after_raw = pparse_skip_noise(_pc, after_raw);
	if (!after_raw) return false;
	if (pparse_is_type_keyword(after_raw) || pparse_is_known_typedef(after_raw) ||
	    (after_raw->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_STORAGE | PPARSE_TT_INLINE | PPARSE_TT_TYPEDEF)) ||
	    ((after_raw->flags & PPARSE_TF_RAW) && !pparse_is_known_typedef(after_raw)))
		return true;
	if (pparse_match_ch(after_raw, '*')) {
		PParseToken *after_star = pparse_skip_noise(_pc, pparse_next(_pc, after_raw));
		while (after_star && (pparse_match_ch(after_star, '*') || (after_star->tag & PPARSE_TT_QUALIFIER)))
			after_star = pparse_skip_noise(_pc, pparse_next(_pc, after_star));
		return after_star && (pparse_is_valid_varname(after_star) || pparse_match_ch(after_star, '('));
	}
	return false;
}

static bool pparse_is_raw_strip_context(PParseToken *raw_kw, PParseToken *after_raw) {
	PPARSE_CTX();
	if (pparse_is_raw_declaration_context(raw_kw, after_raw)) return true;
	after_raw = pparse_skip_noise(_pc, after_raw);
	PParseToken *boundary = after_raw ? pparse_skip_noise(_pc, pparse_next(_pc, after_raw)) : NULL;
	return after_raw && pparse_is_valid_varname(after_raw) && !pparse_is_type_keyword(after_raw) &&
	       !pparse_is_known_typedef(after_raw) && !(after_raw->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE)) && boundary &&
	       (pparse_match_ch(boundary, ',') || pparse_match_ch(boundary, ';') ||
		pparse_match_set(boundary, pparse_CH('[') | pparse_CH('(') | pparse_CH('=') | pparse_CH(':')));
}

/* Resolve every soft `raw` spelling once, after bindings are frozen. The
 * payload is the next significant token; consecutive prefixes form a tiny
 * recipe chain and emission never repeats the typedef/value/context tests. */
static void pparse_plan_raw_strip(PParseToken *tok) {
	PPARSE_CTX();
	if (!tok || !(tok->flags & PPARSE_TF_RAW) ||
	    (pparse_ann(tok) & P1_RAW_DECL_RECIPE) == P1_RAW_DECL_RECIPE ||
	    pparse_raw_token_is_sue_tag_name(tok) || pparse_raw_after_subscript_open_bracket(tok))
		return;
	PParseToken *after = pparse_skip_noise(_pc, pparse_next(_pc, tok));
	if (!after || pparse_identifier_binding_kind(tok) == PPARSE_BINDING_VALUE) return;
	if (pparse_is_known_typedef(tok)) {
		if (!(pparse_is_type_keyword(after) || pparse_is_known_typedef(after) ||
		      (after->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_STORAGE |
				     PPARSE_TT_INLINE | PPARSE_TT_TYPEDEF)) ||
		      (after->flags & PPARSE_TF_RAW)))
			return;
	} else if (tok->td_entry) {
		return;
	}
	if (!pparse_is_raw_strip_context(tok, after)) return;
	(void)pparse_mark_raw_decl(tok, after);
}

static PRISM_PURE bool pparse_needs_space(PParseToken *prev, PParseToken *tok) {
	PPARSE_CTX();
	if (!prev) return false;
	bool prev_word = pparse_is_identifier_like(prev) | (prev->kind == PPARSE_TK_NUM);
	bool tok_word = pparse_is_identifier_like(tok) | (tok->kind == PPARSE_TK_NUM);
	if (prev_word & tok_word) return true;
	if ((prev->kind != PPARSE_TK_PUNCT) | (tok->kind != PPARSE_TK_PUNCT)) return false;
	char a = (prev->len == 1) ? prev->ch0 : pparse_loc(_pc, prev)[prev->len - 1];
	char b = tok->ch0;
	if (b == '=') return strchr("=!<>+-*/%&|^", a) != NULL;
	return ((a == b) & (strchr("+-<>&|#", a) != NULL)) |
	       ((a == '-') & (b == '>')) | ((a == '/') & (b == '*')) |
	       ((a == '*') & (b == '/'));
}

/* Freeze the remaining emission recipes and find an optional identifier while
 * the token pool is hot. */
static bool pparse_finalize(const char *find_ident) {
	PPARSE_CTX();
	pparse_td_build_timelines();
	pparse_ba_build_timelines();
	bool found = false;
	bool mark_uneval = pparse_feat(PPARSE_F_BOUNDS_CHECK);
	bool plan_noreturn = pparse_feat(PPARSE_F_AUTO_UNREACHABLE);
	bool has_defer = pparse_feat(PPARSE_F_DEFER);
	bool warn_safety = pparse_feat(PPARSE_F_WARN_SAFETY);
	int file_scope_braces = 0;
	bool file_scope_initializer = false;
	bool static_storage_pending = false;
	bool static_storage_initializer = false;
	int raw_block_depth = 0;
	uint32_t uneval_covered_until = 0;
	for (PParseToken *t = pparse_token_pool + 1; t->kind != PPARSE_TK_EOF; t++) {
		uint32_t ti = (uint32_t)(t - pparse_token_pool);
		unsigned char ch = t->ch0;
		if (raw_block_depth && ch == '}' && (pparse_ann(t) & P1_RAW_BLOCK))
			raw_block_depth--;
		if (has_defer && (t->tag & PPARSE_TT_DEFER) &&
		    !(pparse_ann(t) & P1_IS_DEFER_KW) && !pparse_is_known_function_call(t)) {
			PParseToken *prev = pparse_walk_back(pparse_idx(_pc, t), PPARSE_WB_ATTR_NOISE);
			PParseToken *next = pparse_skip_noise(_pc, pparse_next(_pc, t));
			if (!(prev && (prev->tag & (PPARSE_TT_MEMBER | PPARSE_TT_SUE))) &&
			    (!pparse_token_has_binding(t) || pparse_match_ch(next, '{')) &&
			    (pparse_is_identifier_like(next) || pparse_match_ch(next, '{')))
				pparse_error_tok(t,
					  "'defer' cannot be used in expression context "
					  "(comma expressions, return values, array dimensions, etc.)");
		}
		if (find_ident && !found && t->kind == PPARSE_TK_IDENT && t->len == 12 &&
		    t->ch0 == (uint8_t)find_ident[0] &&
		    prism_memeq_runtime_sized(pparse_loc(_pc, t), find_ident, 12))
			found = true;
		if (mark_uneval && ti > uneval_covered_until) {
			if ((t->flags & PPARSE_TF_OPEN) && t->ch0 == '{') {
				if (!file_scope_initializer && !static_storage_initializer)
					static_storage_pending = false;
				file_scope_braces++;
			} else if ((t->flags & PPARSE_TF_CLOSE) && t->ch0 == '}') {
				file_scope_braces--;
			} else if (ch == ';') {
				file_scope_initializer = false;
				static_storage_pending = false;
				static_storage_initializer = false;
			} else if ((t->tag & PPARSE_TT_STORAGE) ||
				   ((t->tag & PPARSE_TT_QUALIFIER) && t->ch0 == 'c' && t->len == 9)) {
				if (!(t->tag & PPARSE_TT_REGISTER)) static_storage_pending = true;
			} else if (ch == '=') {
				if (file_scope_braces == 0) file_scope_initializer = true;
				if (static_storage_pending) static_storage_initializer = true;
			}
			if ((file_scope_initializer || static_storage_initializer) &&
			    (t->flags & PPARSE_TF_OPEN) && t->ch0 == '[')
				pparse_ann(t) |= P1_UNEVAL_BRACKET;
			if ((t->flags & PPARSE_TF_STATIC_ASSERT) || (t->tag & PPARSE_TT_GENERIC) ||
			    pparse_is_unevaluated_operand_intro(t)) {
				PParseToken *covered = pparse_mark_unevaluated_at(t);
				if (covered) uneval_covered_until = (uint32_t)(covered - pparse_token_pool);
			}
		}
		if ((t->tag & PPARSE_TT_GENERIC) && pparse_generic_decl_context(t)) {
			PParseToken *name, *params_open, *params_close, *after;
			if (pparse_generic_decl_rewrite_target(
				t, &name, &params_open, &params_close, &after)) {
				pparse_ann(t) |= P1_GENERIC_DECL_RECIPE;
				t->pair_idx = (uint32_t)(name - pparse_token_pool);
				pparse_ann(name) |= P1_GENERIC_DECL_TARGET;
				name->pair_idx = (uint32_t)(params_open - pparse_token_pool);
			}
		}
		if (t->flags & PPARSE_TF_RAW) pparse_plan_raw_strip(t);
		if (plan_noreturn && raw_block_depth == 0 && (t->tag & PPARSE_TT_NORETURN_FN)) {
			PParseToken *end = pparse_try_detect_noreturn_call(t);
			if (end) {
				pparse_ann(t) |= P1_NORETURN_CALL_RECIPE;
				t->pair_idx = (uint32_t)(end - pparse_token_pool);
			}
		}
		if (mark_uneval) {
			if (ch == '[') {
				PParseBoundsPlan plan;
				PParseToken *prev = t > pparse_token_pool + 1 ? t - 1 : NULL;
				if (pparse_bounds_plan_subscript(t, prev, &plan))
					pparse_analysis_add(t, PPARSE_AR_BOUNDS)->as.bounds = plan;
			} else if (ch == '*' &&
				   pparse_bounds_deref_add_is_unverifiable(t)) {
				if (warn_safety)
					pparse_warn_tok(t, PPARSE_ERR_BOUNDS_PTR_ARITH_DEREF);
				else
					pparse_error_tok(t, PPARSE_ERR_BOUNDS_PTR_ARITH_DEREF);
			}
		}
		if ((pparse_ann(t) & P1_RAW_BLOCK) && ch == '{') raw_block_depth++;
	}
	return found;
}

void pparse_tokenizer_teardown(bool full) {
	PPARSE_CTX();
	free(_pc->token_source);
	if (full) {
		free(pparse_sos_do_frames);
		free(pparse_sos_do_snap_buf);
		free(pparse_sos_if_trail_snap);
		PParseArenaBlock *b = _pc->main_arena.head;
		while (b) {
			PParseArenaBlock *next = b->next;
			free(b);
			b = next;
		}
		_pc->main_arena.head = _pc->main_arena.current = NULL;
		memset(pparse_keyword_cache, 0, sizeof(pparse_keyword_cache));
		free(pparse_token_pool);
		pparse_token_pool = NULL;
		pparse_token_cap = 0;
	} else {
		for (PParseArenaBlock *b = _pc->main_arena.head; b; b = b->next) b->used = 0;
		_pc->main_arena.current = _pc->main_arena.head;
	}
	pparse_token_count = 1;
	_pc->input_files = NULL;
	_pc->input_file_count = 0;
	_pc->input_file_capacity = 0;
	_pc->current_file = NULL;
	_pc->token_source = NULL;
}

static void pparse_ctx_destroy(void) {
	if (!pparse_ctx) return;
	pparse_tokenizer_teardown(true);
	free(pparse_ctx);
	pparse_ctx = NULL;
}

/* C language analysis and lowering recipes. */
#define is_raw(t) ((t)->flags & PPARSE_TF_RAW)

typedef struct {
	PParseToken *body_open;	      // The '{' token
	PParseToken *ret_type_start;	      // First token of return type
	PParseToken *ret_type_end;	      // Function name token (exclusive)
	PParseToken *ret_type_suffix_start; // For complex declarators: closing ')'
	PParseToken *ret_type_suffix_end;   // PParseToken after suffix (exclusive)
	PParseHashMap defer_name_set;	     // Exact set of captured names (union of all defer bodies)
	int *label_hash;	     // Open-addressing hash table: name → entry index (-1=empty)
	int entry_start;	     // Start index into p1_entries[] for this function
	int entry_count;	     // Number of P1FuncEntry items for this function
	int label_hash_mask;	     // Power-of-2 mask for label_hash probing
	bool returns_void : 1;
	bool has_computed_goto : 1; // Function contains a computed goto (*ptr)
} FuncMeta;

enum { P1K_LABEL, P1K_GOTO, P1K_DEFER, P1K_DECL, P1K_SWITCH, P1K_CASE };

enum {
	P1Z_NONE = 0,
	P1Z_SCALAR = 1, /* emit " = 0" */
	P1Z_AGG = 2,	/* emit " = {0}" */
	P1Z_MEMSET = 3, /* queue delayed memset */
};

typedef struct {
	uint32_t token_index; // pparse_idx for sorting and recovering the token
	uint16_t scope_id;
	uint8_t kind;

	union {

		struct {
			char *name;
			int len;
		} label; // P1K_LABEL, P1K_GOTO

		struct {
			bool has_init;
			bool is_vla;
			bool has_raw;
			bool is_static_storage;
			uint8_t zero_kind; // P1Z_* emit recipe
			uint32_t body_close_idx;
		} decl; // P1K_DECL

		struct {
			uint16_t switch_scope_id;
		} kase; // P1K_CASE

		PParseHashMap defer_captures; // P1K_DEFER: exact free-name set
	};
} P1FuncEntry; // 24 bytes

typedef char prism_assert_p1_entry_max_24[(sizeof(P1FuncEntry) <= 24) ? 1 : -1];

#define func_meta ((FuncMeta *)_pc->p1_func_meta)
#define func_meta_count (_pc->p1_func_meta_count)
#define func_meta_cap (_pc->p1_func_meta_cap)
#define p1_entries ((P1FuncEntry *)_pc->p1_func_entries)
#define p1_entry_count (_pc->p1_func_entry_count)
#define p1_entry_cap (_pc->p1_func_entry_cap)
#define p1_tok(e) (&pparse_token_pool[(e)->token_index])

typedef struct {
	int scope_depth;
	PParseToken *tok;
} P1LabelResult;

#define PPARSE_TT_NON_EXPR_STMT                                                                                     \
	(PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE | PPARSE_TT_GOTO | PPARSE_TT_CASE | PPARSE_TT_DEFAULT | PPARSE_TT_IF | PPARSE_TT_LOOP | PPARSE_TT_SWITCH | \
	 PPARSE_TT_STORAGE | PPARSE_TT_TYPEDEF)

#define PPARSE_SID_UNKNOWN ((uint16_t)0xFFFF)

typedef struct {
	PParseToken *tok;
	PParseToken *file_scope_stmt_start;
	PParseToken *p1d_decl_start;
	uint16_t *scope_stack;
	uint16_t *p1d_switch_stack;
	uint32_t *p1d_switch_end;
	PParseToken *p1d_prev;
	uint32_t *skip_cache;
	// GNU __label__ local label declarations — allows same-named labels
	struct {
		char *name;
		int len;
		uint16_t scope_id;
		char *mangled;
		int mangled_len;
	} *local_labels;
	int brace_depth;
	int scope_depth;
	int scope_cap;
	int p1d_cur_func;
	int p1d_switch_cap;
	int p1d_switch_top;
	uint32_t p1d_braceless_next_sid;
	int p1d_init_brace_depth;
	int local_label_count;
	int local_label_cap;
	bool at_stmt_start;
	bool p1d_saw_raw;
	bool p1d_saw_static;
	bool p1d_ctrl_pending;
	bool p1d_decl_has_attr;
} P1ScanState;

static inline void p1d_stmt_reset(P1ScanState *s, bool at_start) {
	s->at_stmt_start = at_start;
	s->p1d_saw_raw = s->p1d_saw_static = s->p1d_ctrl_pending = false;
	s->p1d_decl_start = NULL;
	s->p1d_decl_has_attr = false;
}

/* Side-effect rules for orelse, one row per context. The flag triple and the
 * wording were previously passed together at every call site; they are not
 * independent, so they live here as one named rule. */
typedef enum {
	PPARSE_OE_SE_ORELSE_IN_ARRAY_DIMENSION_CHAIN,
	PPARSE_OE_SE_DIM_LHS,
	PPARSE_OE_SE_TYPEOF_LHS,
	PPARSE_OE_SE_ORELSE_FALLBACK_ON_ASSIGNM_MAIN,
	PPARSE_OE_SE_ORELSE_COMPOUND_LITERAL_FA_MAIN,
	PPARSE_OE_SE_BARE_ORELSE_WITH_INDIRECTI_TYPEOF,
	PPARSE_OE_SE_ORELSE_IN_ARRAY_DIMENSION__CHAIN,
	PPARSE_OE_SE_DIM_TYPEOF_LHS,
	PPARSE_OE_SE_BARE_ORELSE_WITH_INDIRECTI_VM,
} PParseOrelseSeKind;

static const struct {
	const char *ctx_msg;
	const char *advice;
	bool check_asm, check_volatile_deref, check_indirect_call;
} pparse_oe_se_rules[] = {
	[PPARSE_OE_SE_ORELSE_IN_ARRAY_DIMENSION_CHAIN] = {"'orelse' in array dimension", "in a chained 'orelse' (would be " "evaluated twice); hoist the " "expression to a variable first", true, true, false},
	[PPARSE_OE_SE_DIM_LHS] = {"'orelse' in array dimension", PPARSE_ERR_ORELSE_LHS_TWICE, true, true, false},
	[PPARSE_OE_SE_TYPEOF_LHS] = {"'orelse' in typeof", PPARSE_ERR_ORELSE_LHS_TWICE, true, false, false},
	[PPARSE_OE_SE_ORELSE_FALLBACK_ON_ASSIGNM_MAIN] = {"orelse fallback on assignment", "in the target expression", true, false, true},
	[PPARSE_OE_SE_ORELSE_COMPOUND_LITERAL_FA_MAIN] = {"orelse compound-literal fallback on assignment", "in the target expression (volatile double-write " "with compound literal fallback); " "use a temporary variable instead", false, true, false},
	[PPARSE_OE_SE_BARE_ORELSE_WITH_INDIRECTI_TYPEOF] = {"bare orelse with indirection in LHS", "in the RHS expression (typeof(RHS) evaluates its " "operand for variably-modified types per C11 " "\xc2\xa7" "6.7.2.4p2, causing double evaluation); " "hoist to a variable first", false, false, true},
	[PPARSE_OE_SE_ORELSE_IN_ARRAY_DIMENSION__CHAIN] = {"'orelse' in array dimension / typeof", "in a chained 'orelse' (would be evaluated twice); " "hoist the expression to a variable first", true, true, false},
	[PPARSE_OE_SE_DIM_TYPEOF_LHS] = {"'orelse' in array dimension / typeof", PPARSE_ERR_ORELSE_LHS_TWICE, true, true, false},
	[PPARSE_OE_SE_BARE_ORELSE_WITH_INDIRECTI_VM] = {"bare orelse with indirection in LHS", "in the RHS (typeof(RHS) may evaluate for VM types " "per C11 6.7.2.4p2; hoist to a variable)", false, false, true},
};
static inline bool is_orelse_value_fallback(PParseToken *after_oe);
static void __attribute__((noinline)) p1d_classify_bracket_orelse_ex(PParseToken *tok, uint16_t cur_sid, int p1d_cur_func, bool hard_ctx, bool allow_se_hoist);
static bool p1_scope_in_raw_block(uint16_t sid);

static inline P1FuncEntry *p1_alloc(int knd, uint16_t sid, PParseToken *t) {
	PPARSE_CTX();
	PPARSE_ARENA_ENSURE_CAP(
	    &_pc->main_arena, _pc->p1_func_entries, p1_entry_count, p1_entry_cap, 64, P1FuncEntry);
	P1FuncEntry *e = &p1_entries[p1_entry_count++];
	*e = (P1FuncEntry){.kind = knd, .scope_id = sid, .token_index = pparse_idx(_pc, t)};
	return e;
}

static bool p1_defer_has_capture(PParseToken *defer_kw, const char *name, int nlen) {
	PPARSE_CTX();
	uint32_t entry_idx = defer_kw->parse_data - 1;
#ifdef PRISM_DEBUG
	if (!defer_kw->parse_data || entry_idx >= (uint32_t)p1_entry_count ||
	    p1_entries[entry_idx].kind != P1K_DEFER)
		pparse_error_tok(defer_kw, "internal: defer keyword has no Phase 1 plan");
#endif
	return pparse_hashmap_get(&p1_entries[entry_idx].defer_captures, (char *)name, nlen) != NULL;
}

static inline bool is_defer_kw(PParseToken *tok, PParseToken *prev) {
	PPARSE_CTX();
	if (!(tok->tag & PPARSE_TT_DEFER)) return false;
	PParseToken *nx = pparse_next(_pc, tok);
	unsigned char nx_ch = nx->ch0;
	/* A declared function named `defer` wins for every call spelling. The
	 * previous empty-argument-only exception let `defer(1);` be silently
	 * lowered as the keyword plus a parenthesized deferred expression. */
	if (nx_ch == '(' && pparse_function_symbol(tok)) return false;
	/* Typedef *type* named `defer` stays an identifier except `defer {…}`
	 * (unambiguous keyword). Variable/param/enum-constant shadows
	 * (`is_shadow`) must still allow braceless `defer stmt;` — otherwise the
	 * keyword leaks to the backend. */
	PParseIdentifierBindingKind binding = pparse_identifier_binding_kind(tok);
	/* A value binding followed by `(` is a function-pointer call. Ordinary
	 * variables named defer must still permit the keyword in `defer cleanup()`;
	 * only the syntactically callable spelling is unambiguous. */
	if (binding == PPARSE_BINDING_VALUE && nx_ch == '(') return false;
	if (binding == PPARSE_BINDING_TYPE && nx_ch != '{') return false;
	/* `defer {…}` cannot be a parameter name — skip the param-list walk. */
	if (nx_ch != '{' && pparse_function_param_open(tok)) return false;
	if (prev) {
		/* Type/declarator, label/member, or expression predecessor: identifier. */
		if ((prev->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE |
				  PPARSE_TT_TYPEDEF | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT |
				  PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE | PPARSE_TT_GOTO |
				  PPARSE_TT_MEMBER | PPARSE_TT_ASSIGN)) ||
		    pparse_is_known_typedef(prev) || pparse_is_sizeof_like(prev) ||
		    pparse_is_gnu_label_decl_head(prev) || pparse_equal_2(_pc, prev, "&&"))
			return false;

		unsigned char prev_ch = prev->ch0;
		switch (prev_ch) {
		case '!': case '~': case '&': case '*': case '(':
		case '[': case ',': case '+': case '-': case '/':
		case '%': case '|': case '^': case '<': case '>':
		case '?': case '=':
			return false;
		case ')': {
			/* A control condition closes into statement position; casts and calls do not. */
			PParseToken *open = pparse_pair_known(prev);
			PParseToken *before =
			    pparse_walk_back(pparse_idx(_pc, open), PPARSE_WB_PAST_NOISE);
			if (!(before && (before->tag & (PPARSE_TT_IF | PPARSE_TT_LOOP | PPARSE_TT_SWITCH))))
				return false;
			break;
		}
		case ':':
			/* Ternary `: defer` is expression; label/case `:` stays statement position. */
			for (uint32_t pi = pparse_idx(_pc, prev); pi > 0;) {
				PParseToken *pt = &pparse_token_pool[--pi];
				if (pt->kind == PPARSE_TK_PREP_DIR) continue;
				if (pt->flags & PPARSE_TF_CLOSE) {
					pi = pparse_idx(_pc, pparse_pair_known(pt));
					continue;
				}
				if (pparse_match_ch(pt, ';') || pparse_match_ch(pt, '{') ||
				    pparse_match_ch(pt, '}') ||
				    (pt->tag & (PPARSE_TT_CASE | PPARSE_TT_DEFAULT)))
					break;
				if (pparse_match_ch(pt, '?')) return false;
			}
			break;
		default: break;
		}
	}
	if (nx_ch == ':' || (nx->tag & PPARSE_TT_ASSIGN)) return false;
	/* `defer;` is an empty defer statement unless a variable/enum shadow owns
	 * the name — then it is an expression-statement of that identifier. */
	if (nx_ch == ';') return binding != PPARSE_BINDING_VALUE;
	/* Call-arg / asm-goto / brace-init: `f(defer)`, `{ defer }`, `{ defer, }`. */
	if (pparse_match_set(nx, pparse_CH(')') | pparse_CH(']') | pparse_CH(',')) || nx_ch == '}')
		return false;
	/* Subscript / member on the name: `defer[i]`, `defer.x`, `defer->x`. */
	if (nx_ch == '[' || (nx->tag & PPARSE_TT_MEMBER)) return false;
	/* Postfix `defer++` / `defer--` vs braceless body `defer ++x;`. */
	if (nx->len == 2 && (nx->ch0 == '+' || nx->ch0 == '-') && pparse_loc(_pc, nx)[1] == nx->ch0) {
		PParseToken *after = pparse_next(_pc, nx);
		if (pparse_match_set(after,
				     pparse_CH(';') | pparse_CH(')') | pparse_CH(']') |
					 pparse_CH(',') | pparse_CH(':')) ||
		    (after->tag & PPARSE_TT_ASSIGN))
			return false;
	}
	/* Infix punctuators cannot start a statement: `for (; defer < 3; )`.
	 * Unary `&` `!` `*` `+` `-` `~` still can (`defer &x;`). */
	if (nx->kind == PPARSE_TK_PUNCT) {
		unsigned char c0 = (unsigned char)nx->ch0;
		if (nx->len == 1) {
			if (pparse_match_set(nx,
					     pparse_CH('<') | pparse_CH('>') | pparse_CH('?') |
						 pparse_CH('%') | pparse_CH('^') | pparse_CH('/')) ||
			    c0 == '|')
				return false;
		} else if (nx->len == 2) {
			char c1 = pparse_loc(_pc, nx)[1];
			if ((c0 == '<' && (c1 == '=' || c1 == '<')) ||
			    (c0 == '>' && (c1 == '=' || c1 == '>')) || (c0 == '=' && c1 == '=') ||
			    (c0 == '!' && c1 == '=') || (c0 == '&' && c1 == '&') || (c0 == '|' && c1 == '|'))
				return false;
		}
	}
	return true;
}

static inline bool orelse_kw_at(PParseToken *t, PParseToken *prev) {
	if (!(t->tag & PPARSE_TT_ORELSE) || (prev && (prev->tag & PPARSE_TT_MEMBER))) return false;
	if (pparse_is_known_function_call(t)) return false;
	/* Soft-keyword predecessor: may be a type specifier (`bool orelse = 0`)
	 * or a value (`_Float32 = _Float32 orelse 2`, `x = asm orelse 2`). */
	if (prev && pparse_is_soft_keyword_identifier(prev)) {
		if (pparse_soft_keyword_decl_name_boundary(t)) return false;
		if (pparse_orelse_is_label_or_goto_target(t, prev)) return false;
		return true;
	}
	PParseIdentifierBindingKind binding = pparse_identifier_binding_kind(t);
	/* Variable/param named orelse: operator only after expression-ending prev.
	 * prev==NULL at stmt-start `orelse;` is the identifier. */
	if (binding == PPARSE_BINDING_VALUE) return pparse_orelse_shadow_is_kw(prev);
	/* Typedef named orelse: dual-use — type at stmt start, operator after expr. */
	if (binding == PPARSE_BINDING_TYPE) return prev && pparse_orelse_shadow_is_kw(prev);
	/* No typedef entry. Exclude type-specifier predecessors (`T orelse`,
	 * `_BitInt(N) orelse`, `int orelse(`) and `return orelse` (operand).
	 * Keep keyword after continue/break (PPARSE_TT_SKIP_DECL makes shadow_is_kw
	 * false — mid-chain needs the keyword) and after call `)`. */
	if (prev && ((prev->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT |
				   PPARSE_TT_ALIGNAS | PPARSE_TT_INLINE | PPARSE_TT_ATTR | PPARSE_TT_RETURN)) ||
		     pparse_is_known_typedef(prev) || pparse_token_ends_sue_type_specifier(prev) ||
		     pparse_close_paren_ends_type_specifier_ctor(prev)))
		return false;
	if (!prev) return true;
	/* Prefer shadow_is_kw; also treat break/continue/goto as keyword predecessors
	 * for mid-chain (`… orelse continue orelse …`). */
	if (pparse_orelse_shadow_is_kw(prev)) return true;
	if (prev->tag & (PPARSE_TT_BREAK | PPARSE_TT_CONTINUE | PPARSE_TT_GOTO)) return true;
	return false;
}

static inline bool orelse_kw_at_bare(PParseToken *t, PParseToken *prev) {
	return orelse_kw_at(t, prev) && !pparse_orelse_is_label_or_goto_target(t, prev) &&
	       !(prev && pparse_token_ends_sue_type_specifier(prev));
}

static void reject_orelse_side_effects(PParseToken *start, PParseToken *end,
				       PParseOrelseSeKind kind) {
	PPARSE_CTX();
	const char *ctx_msg = pparse_oe_se_rules[kind].ctx_msg;
	const char *advice = pparse_oe_se_rules[kind].advice;
	const bool check_asm = pparse_oe_se_rules[kind].check_asm;
	const bool check_volatile_deref = pparse_oe_se_rules[kind].check_volatile_deref;
	const bool check_indirect_call = pparse_oe_se_rules[kind].check_indirect_call;
	int pd = 0;
	PParseToken *prev_tok = NULL;
	PPARSE_FOR_RANGE(s, start, end) {
		if (s->flags & PPARSE_TF_OPEN) {
			pd++;
			goto next_checks;
		}
		if (s->flags & PPARSE_TF_CLOSE) {
			pd--;
			goto next_checks;
		}
		if (pd == 0 && pparse_match_ch(s, ','))
			pparse_error_tok(s,
				  "%s with comma operator at top level (the "
				  "left-hand sub-expression before ',' is evaluated "
				  "twice: double evaluation of volatile reads or "
				  "other side effects) %s",
				  ctx_msg,
				  advice);
	next_checks:
		if (s->tag & (PPARSE_TT_GOTO | PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE | PPARSE_TT_DEFER))
			pparse_error_tok(s,
				  "%s containing control flow keywords "
				  "(cannot duplicate statement expressions with "
				  "goto/return/break/continue/defer) %s",
				  ctx_msg,
				  advice);
		if ((s->len == 2 &&
		     ((s->ch0 == '+' && pparse_loc(_pc, s)[1] == '+') || (s->ch0 == '-' && pparse_loc(_pc, s)[1] == '-'))) ||
		    pparse_is_assignment_operator_token(s))
			pparse_error_tok(s, "%s with side effect %s", ctx_msg, advice);
		if (check_asm && (s->tag & PPARSE_TT_ASM) && !pparse_is_soft_keyword_identifier(s))
			pparse_error_tok(s, "%s with inline asm %s", ctx_msg, advice);
		if ((pparse_is_valid_varname(s) && !pparse_is_type_keyword(s)) || pparse_match_ch(s, ']') || pparse_match_ch(s, ')')) {
			PParseToken *after_s = pparse_next(_pc, s);
			if (after_s && after_s != end && pparse_match_ch(after_s, '('))
				pparse_error_tok(s,
					  "%s with %s call %s",
					  ctx_msg,
					  check_indirect_call ? "a function" : "side effect",
					  advice);
		}
		if (check_indirect_call && pparse_match_ch(s, '(') && (s->flags & PPARSE_TF_OPEN)) {
			PParseToken *group_close = pparse_pair_known(s);
			PParseToken *after_group = pparse_next(_pc, group_close);
			if (group_close != end && after_group != end && pparse_match_ch(after_group, '('))
				pparse_error_tok(s, "%s with an indirect call %s", ctx_msg, advice);
		}
		if (check_volatile_deref && pparse_match_ch(s, '*') && pparse_next(_pc, s) && pparse_next(_pc, s) != end) {
			bool is_mul = false;
			if (prev_tok) {
				if (prev_tok->kind == PPARSE_TK_NUM || prev_tok->kind == PPARSE_TK_STR) is_mul = true;
				else if (prev_tok->kind == PPARSE_TK_IDENT && !pparse_is_type_keyword(prev_tok))
					is_mul = true;
				else if (pparse_match_ch(prev_tok, ']'))
					is_mul = true;
				else if (pparse_match_ch(prev_tok, ')') && (prev_tok->flags & PPARSE_TF_CLOSE)) {
					PParseToken *om = pparse_pair_known(prev_tok);
					{
						PParseToken *fi = pparse_next(_pc, om);
						bool looks_cast =
						    fi && (pparse_is_type_keyword(fi) ||
							   (fi->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF)));
						if (looks_cast) {
							uint32_t oi = pparse_idx(_pc, om);
							if (oi >= 2 && pparse_token_pool[oi - 1].flags & PPARSE_TF_SIZEOF)
								is_mul = true;
						} else
							is_mul = true;
					}
				}
			}
			if (!is_mul) pparse_error_tok(s, "%s with pointer dereference %s", ctx_msg, advice);
		}
		if (check_volatile_deref && (s->tag & PPARSE_TT_MEMBER))
			pparse_error_tok(s, "%s with member access operator %s", ctx_msg, advice);
		if (check_volatile_deref && pparse_match_ch(s, '[') && (s->flags & PPARSE_TF_OPEN))
			pparse_error_tok(s, "%s with array subscript %s", ctx_msg, advice);
		// shadow/typedef table populated in Phase 1D.
		if (check_volatile_deref && pparse_is_valid_varname(s) && !pparse_is_type_keyword(s)) {
			unsigned tf = pparse_typedef_flags(s);
			if (tf & (PPARSE_TDF_VOLATILE | PPARSE_TDF_HAS_VOL_MEMBER))
				pparse_error_tok(s, "%s with volatile-qualified identifier %s", ctx_msg, advice);
			if (tf & PPARSE_TDF_ATOMIC)
				pparse_error_tok(s, "%s with atomic-qualified identifier %s", ctx_msg, advice);
		}
		prev_tok = s;
	}
}

static void validate_bracket_orelse(PParseToken *oe) {
	PPARSE_CTX();
	PParseToken *act = pparse_next(_pc, oe);
	if (act && (act->tag & (PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE | PPARSE_TT_GOTO)))
		pparse_error_tok(oe,
			  "'orelse' with control flow cannot be used inside "
			  "array dimensions or typeof expressions");
	if (act && pparse_match_ch(act, '{'))
		pparse_error_tok(oe,
			  "'orelse' block form cannot be used inside "
			  "array dimensions or typeof expressions");
	/* Empty fallback (`[n orelse ]`): the ternary expansion would emit
	 * `n ? n : ()` — invalid C leaking to the backend.  The `;`/`,` empty
	 * cases are covered by the statement/init validators; the
	 * close-delimiter case belongs here.  Found by the insertion suite. */
	if (!act || (act->flags & PPARSE_TF_CLOSE) || pparse_match_ch(act, ';') || pparse_match_ch(act, ','))
		pparse_error_tok(oe, "expected expression after 'orelse' in array dimension");
}

static PParseToken *paren_scan_skip_nested(PParseToken *t) {
	PPARSE_CTX();
	if ((t->flags & PPARSE_TF_OPEN) && pparse_match_ch(t, '[')) return pparse_pair_known(t);
	if (t->tag & PPARSE_TT_TYPEOF) {
		PParseToken *open = pparse_next(_pc, t);
		if (open && pparse_match_ch(open, '(')) return pparse_pair_known(open);
	}
	if (pparse_is_stmt_expr_open(t)) return pparse_pair_known(t);
	return NULL;
}

/* Catch orelse after a type in unevaluated/type parens; preserve declarations. */
static bool orelse_after_type_in_parens(PParseToken *t, PParseToken *prev) {
	PPARSE_CTX();
	if (!(t->tag & PPARSE_TT_ORELSE) || !prev || pparse_orelse_is_label_or_goto_target(t, prev)) return false;
	if (pparse_token_has_binding(t) || pparse_is_known_typedef(t)) return false;
	PParseToken *p = prev;
	/* Peel abstract-declarator pointer stars / trailing quals: `int *`,
	 * `int *const`, `char **`. Stop before binary `*` (expr * expr). */
	while (p && (pparse_match_ch(p, '*') || (p->tag & PPARSE_TT_QUALIFIER)))
		p = pparse_walk_back(pparse_idx(_pc, p), PPARSE_WB_PAST_NOISE);
	if (!p) return false;
	if (p->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS | PPARSE_TT_TYPEOF)) return true;
	if (pparse_is_known_typedef(p) || pparse_token_ends_sue_type_specifier(p) ||
	    pparse_close_paren_ends_type_specifier_ctor(p))
		return true;
	return false;
}

static void check_paren_orelse_defer(PParseToken *open) {
	PPARSE_CTX();
	if (pparse_paren_is_function_params(open)) return;
	PParseToken *close = pparse_pair_known(open);
	if (pparse_is_stmt_expr_open(open)) return;
	for (PParseToken *pi = open, *t = pparse_next(_pc, open); t != close; pi = t, t = pparse_next(_pc, t)) {
		PParseToken *skipped = paren_scan_skip_nested(t);
		if (skipped) {
			t = skipped;
			continue;
		}
		if (orelse_kw_at(t, pi) || orelse_after_type_in_parens(t, pi)) {
			if (pparse_orelse_is_label_or_goto_target(t, pi)) continue;
			if (pparse_ann(t) & P1_OE_DECL_INIT) continue;
			pparse_error_tok(t, PPARSE_ERR_ORELSE_IN_PARENS);
		}
		/* Paren scan: do not use is_defer_kw — it treats prev=='(' as
		 * expression position (call args / declarators). Inside an
		 * already-open paren group, a statement-shaped defer is always
		 * illegal (`while ((defer …))`, `(defer (void)0, 1)`). */
		if (pparse_feat(PPARSE_F_DEFER) && (t->tag & PPARSE_TT_DEFER) && !pparse_token_has_binding(t) &&
		    !(pi && (pi->tag & (PPARSE_TT_MEMBER | PPARSE_TT_GOTO))) &&
		    !(pi && pparse_is_gnu_label_decl_head(pi)) && !(pi && pparse_equal_2(_pc, pi, "&&")) &&
		    pparse_next(_pc, t) && !pparse_match_ch(pparse_next(_pc, t), ':') &&
		    !(pparse_next(_pc, t)->tag & PPARSE_TT_ASSIGN) &&
		    !pparse_match_ch(pparse_next(_pc, t), ')') && !pparse_match_ch(pparse_next(_pc, t), ',') &&
		    !pparse_match_ch(pparse_next(_pc, t), ']'))
			pparse_error_tok(t, "defer cannot be at top level of a parenthesized expression");
	}
}

static void check_orelse_in_parens(PParseToken *open) {
	/* Several Pass-2 debug backstops are reached from generic balanced-group
	 * walks. Braces (initializer lists) and brackets have their own Phase-1
	 * classifiers; treating them as parentheses false-rejects ordinary
	 * `defer` identifiers in `{ defer }` and valid `[i orelse j]` indexes. */
	if (!open || !pparse_match_ch(open, '(')) return;
	check_paren_orelse_defer(open);
}

static inline bool is_orelse_value_fallback(PParseToken *after_oe) {
	return after_oe && !(after_oe->tag & (PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE | PPARSE_TT_GOTO)) &&
	       !pparse_match_ch(after_oe, '{') && !pparse_match_ch(after_oe, ';');
}

/* LHS of `… = expr orelse …` uses *, [], or . / -> — Pass 2 may typeof(RHS). */
static bool bare_lhs_has_indirection(PParseToken *lhs_start, PParseToken *eq_tok) {
	PPARSE_CTX();
	PPARSE_FOR_RANGE(s, lhs_start, eq_tok)
		if ((s->tag & PPARSE_TT_MEMBER) || pparse_match_ch(s, '*') ||
		    (pparse_match_ch(s, '[') && (s->flags & PPARSE_TF_OPEN)))
			return true;
	return false;
}

/* Is `brace` the body of a struct / union / enum rather than a block? */
static bool p1_brace_is_sue_body(PParseToken *brace) {
	PPARSE_CTX();
	if (!brace || !pparse_match_ch(brace, '{')) return false;
	for (PParseToken *t = pparse_prev_sibling(_pc, brace); t; t = pparse_prev_sibling(_pc, t)) {
		if (t->tag & PPARSE_TT_SUE) return true;
		/* A tag name, attributes, or a C23 `enum E : T` underlying type may
		 * sit between the keyword and the brace; anything that ends a
		 * declaration means this brace is not an SUE body. */
		if (pparse_match_ch(t, ';') || pparse_match_ch(t, '}') || pparse_match_ch(t, '{') ||
		    pparse_match_ch(t, ')') || pparse_match_ch(t, ','))
			return false;
	}
	return false;
}

/* Scan back from `from` to the enclosing declaration boundary; true when that
 * declaration carries static / extern / _Thread_local storage (whose array
 * dimensions and initializers must both be constant). */
static bool p1_decl_boundary_has_storage(PParseToken *from) {
	PPARSE_CTX();
	for (PParseToken *t = pparse_prev_sibling(_pc, from); t; t = pparse_prev_sibling(_pc, t)) {
		if (t->tag & PPARSE_TT_STORAGE) return true;
		if (pparse_match_ch(t, ';') || pparse_match_ch(t, '{') || pparse_match_ch(t, '}') ||
		    pparse_match_ch(t, ','))
			break;
	}
	return false;
}

/* Does this `[` denote a dimension/index that must be an ICE? Automatic array
 * dimensions and sizeof operands may be VLAs, so they are excluded. */
static bool p1_bracket_requires_ice(PParseToken *lb) {
	PPARSE_CTX();
	if (!lb) return false;

	/* Designated initializer index: `[ … ] =` */
	PParseToken *close = pparse_pair_known(lb);
	{
		PParseToken *nx = pparse_next(_pc, close);
		while (nx && nx->kind == PPARSE_TK_PREP_DIR) nx = pparse_next(_pc, nx);
		if (nx && pparse_match_ch(nx, '=')) return true;
	}

	PParseToken *outer = pparse_enclosing_open(_pc, lb, false);
	/* A file-scope declaration's dimension cannot be a VLA. */
	if (!outer) return true;
	/* A struct/union member's dimension cannot be a VLA. */
	if (pparse_match_ch(outer, '{') && p1_brace_is_sue_body(outer)) return true;
	/* `(int[ … ]){…}` — a compound literal cannot have VLA type. */
	if (pparse_match_ch(outer, '(')) {
		PParseToken *outer_close = pparse_pair_known(outer);
		PParseToken *after = pparse_next(_pc, outer_close);
		while (after && after->kind == PPARSE_TK_PREP_DIR) after = pparse_next(_pc, after);
		if (after && pparse_match_ch(after, '{')) return true;
	}

	/* Otherwise: a static / extern / _Thread_local declaration's dimension
	 * cannot be a VLA either. */
	return p1_decl_boundary_has_storage(lb);
}
/* Classify the context a statement expression occupies.
 * 1 = must be an ICE, 0 = need not be, -1 = inconclusive (this stmt-expr is
 * nested in another, so only an outer context can decide). */
static int p1_stmt_expr_ctx_class(PParseToken *open) {
	PPARSE_CTX();
	PParseToken *ctx = pparse_enclosing_open(_pc, open, false);
	if (!ctx) return 1; /* file-scope initializer */
	if (pparse_match_ch(ctx, '[')) return p1_bracket_requires_ice(ctx) ? 1 : 0;
	/* Bitfield width / enum constant value / member dimension. */
	if (pparse_match_ch(ctx, '{')) {
		if (p1_brace_is_sue_body(ctx)) return 1;
		/* Body of an enclosing statement expression: undecidable here.
		 * `({ int v[n]; ({ int w[n]; 1; }); })` in a member dimension must
		 * suppress BOTH declarations. */
		PParseToken *outer = pparse_enclosing_open(_pc, ctx, false);
		if (outer && pparse_is_stmt_expr_open(outer)) return -1;
	}
	/* Initializer of a static / extern / _Thread_local object. */
	return p1_decl_boundary_has_storage(open) ? 1 : 0;
}
static bool p1_decl_in_ice_stmt_expr(PParseToken *type_start, uint16_t sid) {
	PPARSE_CTX();
	if (!type_start || sid == PPARSE_SID_UNKNOWN) return false;
	for (uint16_t s = sid; s != 0 && s < pparse_scope_tree_count;
	     s = pparse_scope_tree[s].parent_id) {
		if (!pparse_scope_tree[s].is_stmt_expr) continue;
		uint32_t bi = pparse_scope_tree[s].open_tok_idx;
		if (bi == 0) continue;
		/* `({` — the '(' is physically the token before the body '{'. */
		PParseToken *paren = &pparse_token_pool[bi - 1];
		if (!pparse_is_stmt_expr_open(paren)) continue;
		int cls = p1_stmt_expr_ctx_class(paren);
		if (cls >= 0) return cls == 1;
		/* Inconclusive: let an enclosing stmt-expr scope decide. */
	}
	return false;
}

enum {
	P1DP_REGISTER = 1,
	P1DP_CONST = 2,
	P1DP_REGISTER_VLA = 4,
	P1DP_INIT_STMT = 8,
	P1DP_FOR_INIT = 16,
};

/* Classify, validate, and finalize the declaration once. */
static uint8_t p1_decl_zero_plan(PParseToken *var,
				 uint8_t shape,
				 PParseToken *type_start,
				 PParseTypeSpec *type,
				 PParseDecl *decl,
				 bool has_init,
				 bool is_raw,
				 bool storage_static,
				 uint16_t sid,
				 unsigned flags) {
	PPARSE_CTX();
	if (!pparse_feat(PPARSE_F_ZEROINIT) || has_init || is_raw) return P1Z_NONE;
	bool automatic = !storage_static && !type->has_static && !type->has_extern;
	unsigned traits = (shape & P1DS_AGG) && type_start
				      ? pparse_type_object_traits(type_start, 0)
				      : 0;
	if (automatic && type->has_register && (flags & P1DP_REGISTER)) {
		if (type->has_atomic && (shape & P1DS_AGG))
			pparse_error_tok(var, PPARSE_ERR_REGISTER_ATOMIC_AGGREGATE);
		if (shape & P1DS_UNION) pparse_error_tok(var, PPARSE_ERR_REGISTER_UNION);
		if (traits & PPARSE_OBJ_ZERO_UNSAFE)
			pparse_error_tok(var, PPARSE_ERR_REGISTER_EMPTY_AGG);
	}
	if (automatic && type->has_register && (shape & P1DS_EFF_VLA) &&
	    (flags & P1DP_REGISTER_VLA))
		pparse_error_tok(var, PPARSE_ERR_REGISTER_VLA);
	if (automatic && !type->has_register && (shape & P1DS_EFF_VLA) &&
	    (flags & P1DP_FOR_INIT) && (!decl->is_pointer || decl->is_array))
		pparse_error_tok(var, PPARSE_ERR_INIT_STMT_VLA);

	if (automatic && !type->has_register && (flags & P1DP_CONST)) {
		bool brace_unsafe = (traits & PPARSE_OBJ_ZERO_UNSAFE) != 0;
		bool needs = (!decl->is_pointer || decl->is_array) &&
			     (type->has_typeof || type->has_long_double ||
			      (type->has_atomic && (shape & P1DS_AGG)) ||
			      (shape & (P1DS_EFF_VLA | P1DS_UNION)) || brace_unsafe);
		bool explicit_const = pparse_decl_const_flags(type, decl) & PPARSE_DECL_CONST_EXPLICIT;
		bool const_member = !explicit_const && (traits & PPARSE_OBJ_CONST_SUBOBJECT);
		if (needs && (explicit_const || const_member)) {
			/* A declaration initializer may write a const object while a
			 * delayed memset may not. Prefer the form valid for both scalar
			 * and aggregate typeof objects whenever the type is brace-safe.
			 * Atomic objects and VLAs have no equally portable generic form. */
			if (!(shape & P1DS_EFF_VLA) && !brace_unsafe && !type->has_atomic &&
			    !type->has_long_double)
				return P1Z_AGG;
			if (const_member)
				pparse_error_tok(var,
					  "aggregate containing a const-qualified subobject requires unavoidable "
					  "memset zero-initialization, which would modify a const object and cause "
					  "undefined behavior. Provide an explicit initializer or use 'raw' to opt out.");
			pparse_error_tok(var, PPARSE_ERR_CONST_UNAVOIDABLE_MEMSET);
		}
	}

	if (!automatic || (shape & P1DS_FUNC)) return P1Z_NONE;
	bool memset = !type->has_register && (!decl->is_pointer || decl->is_array) &&
		      (type->has_typeof || type->has_long_double ||
		       (type->has_atomic && (shape & P1DS_AGG)) ||
		       (shape & (P1DS_EFF_VLA | P1DS_UNION)) ||
		       (traits & PPARSE_OBJ_ZERO_UNSAFE));
	if (type->has_register && (traits & PPARSE_OBJ_ZERO_UNSAFE)) return P1Z_NONE;
	uint8_t zero = memset ? P1Z_MEMSET
			      : (shape & P1DS_EFF_VLA) ? P1Z_NONE
			      : ((shape & (P1DS_AGG | P1DS_UNION)) || type->has_typeof) ? P1Z_AGG
											 : P1Z_SCALAR;
	if (zero == P1Z_MEMSET && p1_decl_in_ice_stmt_expr(type_start, sid)) return P1Z_NONE;
	if (zero == P1Z_MEMSET && (flags & P1DP_INIT_STMT)) {
		if (!(shape & P1DS_EFF_VLA)) {
			if ((type->has_atomic && (shape & P1DS_AGG)) ||
			    (traits & PPARSE_OBJ_ZERO_UNSAFE))
				pparse_error_tok(var,
					  "aggregate requiring memset cannot be zero-initialized in a "
					  "for/if/switch init-statement; move the declaration before the "
					  "statement");
			return P1Z_AGG;
		}
		if (!(flags & P1DP_FOR_INIT)) return P1Z_NONE;
	}
	return zero;
}

static void
reject_defer_context(PParseToken *tok, bool ctrl_paren, bool ctrl_pending, bool in_stmt_expr, bool in_switch) {
	if (ctrl_paren) pparse_error_tok(tok, PPARSE_ERR_DEFER_CTRL_PAREN);
	if (ctrl_pending) pparse_error_tok(tok, PPARSE_ERR_DEFER_BRACELESS_CTRL);
	if (in_stmt_expr) pparse_error_tok(tok, PPARSE_ERR_DEFER_STMT_EXPR_TOP);
	if (in_switch) pparse_error_tok(tok, PPARSE_ERR_DEFER_SWITCH_BRACE);
}

static void reject_defer_unterminated(PParseToken *tok, PParseToken *body, PParseToken *semi) {
	PPARSE_CTX();
	if (semi->kind == PPARSE_TK_EOF || !pparse_match_ch(semi, ';')) pparse_error_tok(tok, PPARSE_ERR_DEFER_UNTERMINATED);
	/* A control keyword before the body's `;` proves the `;` was missing. */
	for (PParseToken *s = body; s && s != semi && s->kind != PPARSE_TK_EOF; s = pparse_next(_pc, s)) {
		PPARSE_SKIP_GROUP_LENIENT(s)
		if (s->kind == PPARSE_TK_KEYWORD && (s->tag & (PPARSE_TT_NON_EXPR_STMT | PPARSE_TT_DEFER)))
			pparse_error_tok(tok, PPARSE_ERR_DEFER_MISSING_SEMI, s->len, pparse_loc(_pc, s));
	}
}

// Find a label in the current function's Phase 1D P1FuncEntry array.
/* GNU __label__ stores names as "ident\\0scope_id" (mangled_len > ident len).
 * Match both plain labels and that prefix form. Prefer the deepest scope when
 * several __label__ bindings share a source name. */
static bool p1_label_matches_tok(P1FuncEntry *e, PParseToken *tok) {
	PPARSE_CTX();
	if (e->kind != P1K_LABEL) return false;
	if ((uint32_t)e->label.len == tok->len && prism_memeq_runtime_sized(e->label.name, pparse_loc(_pc, tok), tok->len)) return true;
	if ((uint32_t)e->label.len > tok->len && e->label.name[tok->len] == '\0' &&
	    prism_memeq_runtime_sized(e->label.name, pparse_loc(_pc, tok), tok->len))
		return true;
	return false;
}

static P1LabelResult p1_label_find(PParseToken *tok, int func_idx) {
	PPARSE_CTX();
	if (func_idx < 0 || func_idx >= func_meta_count) return (P1LabelResult){0, NULL};
	FuncMeta *fm = &func_meta[func_idx];
	P1FuncEntry *entries = &p1_entries[fm->entry_start];
	if (fm->label_hash) {
		uint32_t h = (uint32_t)pparse_fast_hash(pparse_loc(_pc, tok), tok->len);
		for (int probe = 0; probe <= fm->label_hash_mask; probe++) {
			int slot = (h + probe) & fm->label_hash_mask;
			if (fm->label_hash[slot] < 0) break;
			P1FuncEntry *e = &entries[fm->label_hash[slot]];
			/* Exact-len hit only — mangled __label__ hashes differ. */
			if ((uint32_t)e->label.len == tok->len && p1_label_matches_tok(e, tok))
				return (P1LabelResult){pparse_scope_tree_depth(e->scope_id), p1_tok(e)};
		}
		/* Fall through: __label__ mangled names miss the unmangled hash. */
	}
	P1LabelResult best = {0, NULL};
	for (int i = 0; i < fm->entry_count; i++) {
		if (!p1_label_matches_tok(&entries[i], tok)) continue;
		int d = pparse_scope_tree_depth(entries[i].scope_id);
		if (!best.tok || d >= best.scope_depth)
			best = (P1LabelResult){d, p1_tok(&entries[i])};
	}
	return best;
}

static inline bool pparse_ternary_depth_step(PParseToken *tok, int *depth) {
	if (pparse_match_ch(tok, '?')) {
		(*depth)++;
		return true;
	}
	if (pparse_match_ch(tok, ':') && *depth > 0) {
		(*depth)--;
		return true;
	}
	return false;
}

static PParseToken *find_bare_orelse(PParseToken *tok) {
	PPARSE_CTX();
	if (!(pparse_token_tag_summary & PPARSE_TT_ORELSE)) return NULL;
	PParseToken *prev = NULL;
	int ternary = 0;
	for (PParseToken *s = tok; s->kind != PPARSE_TK_EOF; s = pparse_next(_pc, s)) {
		if (s->flags & PPARSE_TF_OPEN) {
			s = prev = pparse_pair_known(s);
			continue;
		}
		if ((s->flags & PPARSE_TF_CLOSE) || pparse_match_ch(s, ';')) return NULL;
		if (pparse_ternary_depth_step(s, &ternary)) {
			prev = s;
			continue;
		}
		/* `return c ? g() orelse 0 : 1` — orelse inside `?:` must reject.
		 * Decl-init has its own ternary tracker; bare/expr stmts relied on
		 * find_bare_orelse skipping depth>0, which silently leaked. */
		if (ternary > 0) {
			bool is_oe = (pparse_ann(s) & P1_IS_ORELSE_KW) || orelse_kw_at_bare(s, prev);
			if (is_oe && !(pparse_ann(s) & (P1_OE_DECL_INIT | P1_OE_BRACKET))) {
				pparse_error_tok(s, PPARSE_ERR_ORELSE_TERNARY);
			}
			prev = s;
			continue;
		}
		if ((pparse_ann(s) & P1_IS_ORELSE_KW) &&
		    !(pparse_ann(s) & (P1_OE_DECL_INIT | P1_OE_BRACKET)))
			return s;
		if (orelse_kw_at_bare(s, prev)) return s;
		prev = s;
	}
	return NULL;
}

/* Parameter declarations are discovered while walking an already-built scope
 * tree. Keep exact-token lookup, optional shadow creation, scope selection,
 * and array-to-pointer decay in the parser rather than its consumers. */
static void pparse_register_parameter_binding(PParseToken *tok,
					 int scope_depth,
					 uint16_t scope_id,
					 bool create,
					 unsigned traits) {
	PPARSE_CTX();
	PParseTypedefEntry *entry = pparse_binding_entry(tok, false);
	if (!entry && create) {
		PPARSE_TD_SCOPE_SAVE();
		if (scope_id > 0 && scope_id < pparse_scope_tree_count) {
			pparse_td_scope_close = pparse_scope_close(&pparse_scope_tree[scope_id]);
		}
		entry = pparse_register_shadow(tok, scope_depth);
		PPARSE_TD_SCOPE_RESTORE();
	}
	if (!entry) return;
	entry->is_param = true;
	entry->is_array = false;
	pparse_binding_apply_traits(entry, traits);
}
static inline bool pparse_param_name_candidate(PParseToken *tok) {
	const uint32_t non_name = PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE | PPARSE_TT_SUE |
				  PPARSE_TT_TYPEOF | PPARSE_TT_ATTR;
	return pparse_is_valid_varname(tok) &
	       (!(tok->tag & non_name) |
		((tok->tag & (PPARSE_TT_DEFER | PPARSE_TT_ORELSE)) != 0) |
		((tok->flags & PPARSE_TF_RAW) != 0));
}

static bool pparse_declarator_has_vla_after_first_bracket(PParseToken *start,
							 PParseToken *end,
							 PParseToken *stop,
							 bool skip_first) {
	PPARSE_CTX();
	for (PParseToken *s = start;
	     s && s != end && s != stop && s->kind != PPARSE_TK_EOF;
	     s = pparse_next(_pc, s)) {
		if (!pparse_match_ch(s, '[')) continue;
		if (skip_first) {
			skip_first = false;
			s = pparse_pair_known(s);
			continue;
		}
		if (pparse_array_size_is_vla(s)) return true;
	}
	return false;
}

static void
p1_register_param_shadows(PParseToken *open, PParseToken *close, uint16_t scope_id, int brace_depth, bool check_vla) {
	PPARSE_CTX();
	for (PParseToken *t = pparse_next(_pc, open); t && t != close && t->kind != PPARSE_TK_EOF;) {
		PParseToken *param_start = t;
		PParseToken *last_ident = NULL;
		bool scanned_inner_paren = false;
		bool ident_from_inner = false;
		while (t && t != close && !pparse_match_ch(t, ',') && t->kind != PPARSE_TK_EOF) {
			if (t->flags & PPARSE_TF_OPEN) {
				PParseToken *group_close = pparse_pair_known(t);
				if (!last_ident && !scanned_inner_paren && pparse_match_ch(t, '('))
					for (PParseToken *s = pparse_next(_pc, t); s != group_close; s = pparse_next(_pc, s)) {
						PPARSE_SKIP_GROUP_ON_CLOSE(s)
						if (pparse_param_name_candidate(s)) {
							last_ident = s;
							ident_from_inner = true;
						}
					}
				if (pparse_match_ch(t, '(')) scanned_inner_paren = true;
				t = pparse_next(_pc, group_close);
				continue;
			}
			if (pparse_param_name_candidate(t)) {
				last_ident = t;
				ident_from_inner = false;
			}
			t = pparse_next(_pc, t);
		}
		if (last_ident &&
		    (pparse_is_known_typedef(last_ident) || pparse_is_known_enum_const(last_ident) ||
		     pparse_is_constexpr_ident(last_ident) ||
		     (last_ident->tag & (PPARSE_TT_DEFER | PPARSE_TT_ORELSE | PPARSE_TT_NORETURN_FN | PPARSE_TT_SPECIAL_FN)) ||
		     (last_ident->flags & PPARSE_TF_RAW) ||
		     pparse_function_symbol(last_ident)))
			pparse_register_shadow(last_ident, brace_depth);
		if (check_vla && last_ident) {
			PParseToken *param_end = pparse_match_ch(t, ',') ? t : close;
			if (pparse_declarator_has_vla_after_first_bracket(
				    param_start, param_end, NULL, !ident_from_inner))
				pparse_register_vla_binding(last_ident, brace_depth);
		}
		if (last_ident && scope_id < pparse_scope_tree_count) {
			PParseToken *param_end = pparse_match_ch(t, ',') ? t : close;
			/* Skip definite built-in scalar params; all ambiguous/array types need entries. */
			bool param_has_bracket = false;
			PPARSE_FOR_RANGE(s, param_start, param_end)
				if (pparse_match_ch(s, '[') && (s->flags & PPARSE_TF_OPEN) && !(s->flags & PPARSE_TF_C23_ATTR)) {
					param_has_bracket = true;
					break;
				}
			bool has_vol_qual = false;
			bool has_vol_member = false;
			bool has_atomic_qual = false;
			bool saw_star = false;
			bool saw_builtin_type = false, saw_typedef_or_sue = false;
			for (PParseToken *s = param_start; s != param_end;
			     s = pparse_next(_pc, s)) {
				if (s == last_ident) break;
				saw_star |= pparse_match_ch(s, '*');
				bool tagged_indirect_type =
				    (s->tag & (PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT)) != 0;
				bool known_typedef = pparse_is_known_typedef(s);
				saw_typedef_or_sue |= tagged_indirect_type | known_typedef;
				saw_builtin_type |= !tagged_indirect_type & !known_typedef &
						    ((s->tag & PPARSE_TT_TYPE) != 0);
				has_vol_qual |= (s->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_VOLATILE)) ==
						(PPARSE_TT_QUALIFIER | PPARSE_TT_VOLATILE);
				has_atomic_qual |= s->len == 7 && s->ch0 == '_' &&
						   prism_memeq_static(pparse_loc(_pc, s), "_Atomic", 7);
				if (pparse_is_valid_varname(s) &&
				    !(s->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_ATTR))) {
					unsigned tf = pparse_typedef_flags(s);
					has_vol_qual |= (tf & PPARSE_TDF_VOLATILE) != 0;
					has_vol_member |= (tf & PPARSE_TDF_HAS_VOL_MEMBER) != 0;
					has_atomic_qual |= (tf & PPARSE_TDF_ATOMIC) != 0;
				}
			}
			bool is_const_param = false;
			bool is_vol_param = (has_vol_qual || has_vol_member) && !saw_star;
			bool is_atomic_param = has_atomic_qual && !saw_star;
			/* Recover top-level declarator qualifiers exactly. A qualifier before
			 * `*` belongs to the pointee; one after the final `*` belongs to the
			 * parameter object. Array-parameter qualifiers live inside `[]` and
			 * qualify the adjusted pointer. */
			PParseTypeSpec param_type = pparse_type_specifier(param_start);
			PParseDecl param_decl = pparse_declarator(param_type.end);
			if (param_decl.var_name == last_ident) {
				if (param_decl.is_pointer || param_decl.is_func_ptr) {
					is_const_param = param_decl.is_const;
					is_vol_param = param_decl.is_volatile;
					is_atomic_param = param_decl.is_atomic;
				} else if (param_decl.is_array) {
					is_const_param = is_vol_param = is_atomic_param = false;
					for (PParseToken *s = pparse_next(_pc, last_ident);
					     s != param_end;
					     s = pparse_next(_pc, s)) {
						if (!pparse_match_ch(s, '[') || !(s->flags & PPARSE_TF_OPEN))
							continue;
						PParseToken *bc = pparse_pair_known(s);
						PPARSE_FOR_RANGE(q, pparse_next(_pc, s), bc) {
							is_const_param |= (q->tag & PPARSE_TT_CONST) != 0;
							is_vol_param |= (q->tag & PPARSE_TT_VOLATILE) != 0;
							is_atomic_param |= (q->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE)) ==
									   (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE);
						}
						break;
					}
				} else {
					is_const_param = param_type.has_const;
					is_vol_param = param_type.has_volatile || param_type.has_volatile_member;
					is_atomic_param = param_type.has_atomic;
				}
			}
			/* Record only params that can affect array or qualified-type lookup. */
			bool hides_outer_array = pparse_bounds_is_tracked_array(last_ident);
			bool param_needs_shadow =
			    param_has_bracket || is_const_param || is_vol_param || is_atomic_param || hides_outer_array ||
			    (!saw_star && (saw_typedef_or_sue || !saw_builtin_type));
			unsigned traits = is_const_param * PPARSE_BIND_CONST |
					  is_vol_param * PPARSE_BIND_VOLATILE |
					  (has_vol_member && is_vol_param) * PPARSE_BIND_VOLATILE_MEMBER |
					  is_atomic_param * PPARSE_BIND_ATOMIC;
			pparse_register_parameter_binding(
			    last_ident, brace_depth, scope_id, param_needs_shadow, traits);
		}
		if (pparse_match_ch(t, ',')) t = pparse_next(_pc, t);
	}
}

static bool p1d_classify_decl_dims(PParseToken *start,
				   PParseToken *end,
				   uint16_t cur_sid,
				   int cur_func,
				   bool hard_ctx,
				   bool allow_se_hoist) {
	PPARSE_CTX();
	if (!pparse_feat(PPARSE_F_ORELSE)) return false;
	bool found = false;
	for (PParseToken *t = start; t && t != end;) {
		if (pparse_match_ch(t, '[') && !(t->flags & PPARSE_TF_C23_ATTR)) {
			if (!(pparse_ann(t) & P1_OE_BRACKET))
				p1d_classify_bracket_orelse_ex(t, cur_sid, cur_func, hard_ctx, allow_se_hoist);
			found |= (pparse_ann(t) & P1_OE_BRACKET) != 0;
			t = pparse_next(_pc, pparse_pair_known(t));
			continue;
		}
		/* Skip struct/enum bodies and stmt-exprs, but walk into `(...)`
		 * so dims inside typeof/_Atomic type-specifier parens are seen. */
		if (pparse_match_ch(t, '{')) {
			t = pparse_next(_pc, pparse_pair_known(t));
			continue;
		}
		t = pparse_next(_pc, t);
	}
	return found;
}

static bool pparse_canonical_zero_follows(PParseToken *var) {
	PPARSE_CTX();
	PParseToken *t = var;
	for (int n = 0; t && t->kind != PPARSE_TK_EOF && n < 4096; n++) {
		if (t->flags & PPARSE_TF_OPEN) {
			t = pparse_next(_pc, pparse_pair_known(t));
			continue;
		}
		if (pparse_match_ch(t, ';')) break;
		if (pparse_match_ch(t, '}')) return false;
		t = pparse_next(_pc, t);
	}
	if (!t || !pparse_match_ch(t, ';')) return false;
	t = pparse_next(_pc, t);
	for (int n = 0; t && t->kind != PPARSE_TK_EOF && n < 8; n++) {
		if (t->kind == PPARSE_TK_IDENT && t->len == 16 &&
		    prism_memeq_static(pparse_loc(_pc, t), "__builtin_memset", 16)) {
			PParseToken *p = pparse_next(_pc, t), *close;
			if (!p || !pparse_match_ch(p, '(')) return false;
			close = pparse_pair_known(p);
			PParseToken *a = pparse_next(_pc, p);
			if (a && pparse_match_ch(a, '('))
				a = pparse_next(_pc, pparse_pair_known(a));
			if (a && pparse_match_ch(a, '&')) a = pparse_next(_pc, a);
			if (a && a->kind == PPARSE_TK_IDENT && a->len == var->len &&
			    prism_memeq_runtime_sized(pparse_loc(_pc, a), pparse_loc(_pc, var), var->len))
				return true;
			t = pparse_next(_pc, close);
			if (t && pparse_match_ch(t, ';')) t = pparse_next(_pc, t);
			continue;
		}
		if (pparse_match_ch(t, '{')) {
			PParseToken *close = pparse_pair_known(t);
			bool has_p = false, has_var = false;
			int k = 0;
			for (PParseToken *s = pparse_next(_pc, t); s && s != close && k++ < 16;
			     s = pparse_next(_pc, s)) {
				has_p |= s->kind == PPARSE_TK_IDENT && s->len >= 10 &&
					 prism_memeq_static(pparse_loc(_pc, s), "__prism_p_", 10);
				PParseToken *name = pparse_match_ch(s, '&') ? pparse_next(_pc, s) : NULL;
				has_var |= name && name->kind == PPARSE_TK_IDENT && name->len == var->len &&
					   prism_memeq_runtime_sized(pparse_loc(_pc, name), pparse_loc(_pc, var), var->len);
			}
			if (!has_p) return false;
			if (has_var) return true;
			t = pparse_next(_pc, close);
			continue;
		}
		return false;
	}
	return false;
}

static P1FuncEntry *p1_analyze_decl(PParseToken *type_start,
				    PParseTypeSpec *type,
				    PParseDecl *decl,
				    bool is_raw,
				    int brace_depth,
				    bool track_const,
				    bool record,
				    uint16_t sid,
				    bool storage_static,
				    uint32_t body_close_idx,
				    unsigned plan_flags,
				    uint8_t *out_zero) {
	PPARSE_CTX();
	bool plain = !(decl->is_pointer | decl->is_func_ptr);
	bool type_vol = type->has_volatile | type->has_volatile_member;
	bool is_vol = (plain & type_vol) | (!plain & decl->is_volatile);
	bool is_atomic = (plain & type->has_atomic) | (!plain & decl->is_atomic);
	bool is_long_double = plain & !type->is_ptr & type->has_long_double;
	bool is_const = track_const &
			((pparse_decl_const_flags(type, decl) & PPARSE_DECL_CONST_EFFECTIVE) != 0);
	bool is_aggregate = (brace_depth > 0) & type->is_struct & !type->is_enum & plain &
			    !decl->is_array & !decl->is_func_decl;
	bool create = pparse_is_known_typedef(decl->var_name) ||
		      pparse_is_known_enum_const(decl->var_name) ||
		      (decl->var_name->tag & (PPARSE_TT_DEFER | PPARSE_TT_ORELSE |
					     PPARSE_TT_NORETURN_FN | PPARSE_TT_SPECIAL_FN)) ||
		      (decl->var_name->flags & PPARSE_TF_RAW) ||
		      pparse_function_symbol(decl->var_name) || is_vol || is_atomic || is_long_double || is_const ||
			      decl->is_func_decl || is_aggregate;
	if (create) {
		unsigned bind = decl->is_func_decl * PPARSE_BIND_FUNC |
				is_const * PPARSE_BIND_CONST |
				is_vol * PPARSE_BIND_VOLATILE |
				(type->has_volatile_member & is_vol) * PPARSE_BIND_VOLATILE_MEMBER |
				is_atomic * PPARSE_BIND_ATOMIC |
				is_long_double * PPARSE_BIND_LONG_DOUBLE |
				is_aggregate * PPARSE_BIND_AGGREGATE |
				(type->has_constexpr & plain) * PPARSE_BIND_CONSTEXPR;
		pparse_register_shadow_traits(decl->var_name, brace_depth, bind);
	}
	if (pparse_feat(PPARSE_F_BOUNDS_CHECK)) {
		bool base_array = (!is_raw) & plain & type->is_array;
		bool own_array = decl->is_array & (!decl->paren_pointer | decl->paren_array);
		bool complete = (own_array & (decl->array_dim_complete | pparse_match_ch(decl->end, '='))) |
				((!own_array) & ((!base_array) | type->array_dim_complete));
		bool register_array = (!is_raw) &
			((brace_depth > 0) | ((brace_depth == 0) & (own_array | base_array))) &
			complete & (own_array | base_array) & (!decl->is_func_ptr);
		if (!register_array && pparse_bounds_is_tracked_array(decl->var_name))
			pparse_register_array_binding(decl->var_name, 0, false, false, true);
		if (register_array) {
			int rank = decl->array_rank + type->type_array_rank;
			rank += (!rank & base_array);
			if (rank > 15) rank = PPARSE_ARRAY_RANK_WRAP_ALL;
			if (create) {
				PParseTypedefEntry *entry = pparse_binding_entry(decl->var_name, true);
				if (entry) {
					entry->is_array = true;
					entry->array_rank = (uint8_t)rank;
					entry->array_dim_complete = complete;
				}
			}
			pparse_register_array_binding(
			    decl->var_name, (uint8_t)rank, complete, false, false);
		}
	}
	if (!is_raw && (type->is_vla || decl->is_vla) && brace_depth > 0)
		pparse_register_vla_preserving_array(decl->var_name, brace_depth);
	uint8_t shape =
	    ((decl->is_vla & (!decl->paren_pointer | decl->paren_array)) |
	     (type->is_vla & !decl->is_pointer)) * P1DS_EFF_VLA |
	    ((decl->is_array & (!decl->paren_pointer | decl->paren_array)) |
	     ((type->is_struct | type->is_typedef | type->is_array) & !decl->is_pointer)) * P1DS_AGG |
	    (type->is_union & !decl->is_pointer) * P1DS_UNION |
	    (decl->is_func_decl |
	     (type->is_func & !decl->is_pointer & !decl->is_array & !decl->is_func_ptr)) * P1DS_FUNC;
	uint8_t zero = p1_decl_zero_plan(decl->var_name,
					    shape,
					    type_start,
					    type,
					    decl,
					    pparse_match_ch(decl->end, '='),
					    is_raw,
					    storage_static,
					    sid,
					    plan_flags);
	if (out_zero) *out_zero = zero;
	if (!record) return NULL;
	uint32_t recipe = P1_DECL_RECIPE | ((uint32_t)zero << P1_DECL_ZERO_SHIFT);
	unsigned cf = pparse_decl_const_flags(type, decl);
	recipe |= ((cf & PPARSE_DECL_CONST_EXPLICIT) != 0) * P1_DECL_EXPLICIT_CONST |
		  ((cf & PPARSE_DECL_CONST_EFFECTIVE) != 0) * P1_DECL_EFFECTIVE_CONST |
		  decl->has_bracket_orelse * P1_DECL_BRACKET_OE |
		  (plain & (type->has_volatile | type->has_volatile_member | type->has_hidden_volatile)) *
		      P1_DECL_VOLATILE_VALUE;
	if (zero == P1Z_MEMSET && pparse_canonical_zero_follows(decl->var_name))
		recipe |= P1_DECL_ALREADY_ZERO;
	pparse_ann(decl->var_name) |= recipe;
	if (!sid || (shape & P1DS_FUNC)) return NULL;
	P1FuncEntry *e = p1_alloc(P1K_DECL, sid, decl->var_name);
	e->decl.has_init = pparse_match_ch(decl->end, '=');
	e->decl.is_vla = type->is_vla || decl->is_vla;
	e->decl.has_raw = is_raw;
	e->decl.is_static_storage = storage_static;
	e->decl.body_close_idx = body_close_idx;
	e->decl.zero_kind = zero;
	return e;
}

static PParseToken *pparse_find_init_semicolon(PParseToken *open, PParseToken *close) {
	PPARSE_CTX();
	PParseToken *semi = pparse_skip_to_semicolon(pparse_next(_pc, open), close);
	return semi != close && semi->kind != PPARSE_TK_EOF ? semi : NULL;
}
static void p1_scan_init_shadows(PParseToken *open,
				 PParseToken *init_end,
				 uint32_t scope_close_idx,
				 uint16_t cur_sid,
				 int brace_depth,
				 uint16_t body_sid,
				 uint32_t body_close_idx,
				 bool is_for_init) {
	PPARSE_CTX();
	PParseToken *init_tok = pparse_skip_noise(_pc, pparse_next(_pc, open));
	PParseToken *decl_start = init_tok;
	bool saw_raw = false;
	while (init_tok && (init_tok->flags & PPARSE_TF_RAW) && !pparse_is_known_typedef(init_tok)) {
		PParseToken *after_raw = pparse_skip_noise(_pc, pparse_next(_pc, init_tok));
		/* `raw { … }` suppress block in for-init is never valid. */
		if (after_raw && pparse_match_ch(after_raw, '{')) {
			pparse_error_tok(after_raw,
				  "'raw { ... }' block is not allowed in for/if/switch initializer");
			return;
		}
		/* `for (raw = 0; …)` — identifier named raw, not the keyword. */
		if (!after_raw || !pparse_is_raw_declaration_context(init_tok, after_raw)) break;
		saw_raw = true;
		init_tok = after_raw;
	}
	if (saw_raw && init_tok && pparse_match_ch(init_tok, '{'))
		pparse_error_tok(init_tok, "'raw { ... }' block is not allowed in for/if/switch initializer");
	if (saw_raw && is_for_init && init_tok && init_end) {
		bool has_dim = false, has_eq = false;
		PPARSE_FOR_RANGE(t, init_tok, init_end) {
			if (pparse_match_ch(t, '[')) has_dim = true;
			if (pparse_match_ch(t, '=') && !(t->tag & PPARSE_TT_ASSIGN && t->len > 1)) has_eq = true;
			PPARSE_SKIP_GROUP_ON_CLOSE(t)
		}
		/* Allow `for (raw int arr[n];;)` (VLA suppress); reject scalar init. */
		if (has_eq && !has_dim)
			pparse_error_tok(init_tok,
				  "'raw' scalar declaration with initializer is not allowed in for-init");
	}
	if (!init_tok) return;
	pparse_ASSERT_NOT_NOISE(init_tok);
	// Skip __extension__/inline prefix — matches Phase 1D main loop
	while (init_tok && (init_tok->tag & PPARSE_TT_INLINE) && !(init_tok->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE)))
		init_tok = pparse_skip_noise(_pc, pparse_next(_pc, init_tok));
	if (!init_tok) return;
	if (init_tok->tag & PPARSE_TT_TYPEDEF) {
		PPARSE_TD_SCOPE_SAVE();
		pparse_td_scope_close = scope_close_idx;
		pparse_typedef_declaration(init_tok, brace_depth);
		for (PParseToken *tw = init_tok;
		     tw && tw != init_end && tw->kind != PPARSE_TK_EOF && !pparse_match_ch(tw, ';');) {
			if (pparse_is_enum_kw(tw)) {
				PParseToken *brace = pparse_find_struct_body_brace(tw);
				if (brace) pparse_enum_constants(brace, brace_depth);
			}
			if (tw->flags & PPARSE_TF_OPEN) {
				tw = pparse_next(_pc, pparse_pair_known(tw));
				continue;
			}
			tw = pparse_next(_pc, tw);
		}
		PPARSE_TD_SCOPE_RESTORE();
		return;
	}
	if (!(init_tok->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT |
					    PPARSE_TT_INLINE | PPARSE_TT_STORAGE) ||
			   pparse_is_known_typedef(init_tok)))
		return;
	bool saw_static = init_tok->tag & PPARSE_TT_STORAGE;
	PPARSE_TD_SCOPE_SAVE();
	pparse_td_scope_close = scope_close_idx;
	PParseTypeSpec type = pparse_type_specifier(decl_start);
	if (type.saw_type) {
		pparse_ann(decl_start) |= P1_IS_DECL;
		saw_static |= type.has_static || type.has_extern || type.has_thread_local;
		PParseToken *t = type.end;
		while (t && t != init_end && t->kind != PPARSE_TK_EOF) {
			bool decl_raw = saw_raw;
			t = pparse_p1_skip_decl_raw(t, &decl_raw);
			PParseDecl decl = pparse_declarator(t);
			if (!decl.var_name || !decl.end) break;
			// Phase 1D: reject bracket orelse in ctrl-paren declarations
			// conditions; moved from Pass 2 to satisfy the two-pass invariant)
			decl.has_bracket_orelse =
			    p1d_classify_decl_dims(t, decl.end, 0, -1, false, true);
			if (decl.has_bracket_orelse)
				pparse_error_tok(t, PPARSE_ERR_BRACKET_OE_VLA_INIT_STMT);
			// Phase 1D: register CFG entry for goto-skip-decl detection
			{
				uint16_t eff_sid = body_sid > 0 ? body_sid : cur_sid;
				bool storage_static = saw_static || type.has_static || type.has_extern;
				unsigned plan_flags = P1DP_REGISTER | P1DP_CONST | P1DP_INIT_STMT |
						      (is_for_init ? P1DP_FOR_INIT : 0);
				p1_analyze_decl(init_tok,
						&type,
						&decl,
						decl_raw,
						brace_depth,
						false,
						true,
						eff_sid,
						storage_static,
						body_sid > 0 ? 0 : body_close_idx,
						plan_flags,
						NULL);
			}

			t = decl.end;
			if (pparse_match_ch(t, '=')) {
				t = pparse_next(_pc, t);
				while (t && t != init_end && t->kind != PPARSE_TK_EOF) {
					if (t->flags & PPARSE_TF_OPEN) {
						t = pparse_next(_pc, pparse_pair_known(t));
						continue;
					}
					if (pparse_match_ch(t, ',')) break;
					t = pparse_next(_pc, t);
				}
			}
			if (t && pparse_match_ch(t, ',')) t = pparse_next(_pc, t);
			else
				break;
		}
	}
	PPARSE_TD_SCOPE_RESTORE();
}
/* Shared for/if/switch-init Phase 1D body: open-paren → init `;` → body end →
 * shadows. */
static uint32_t *p1d_ensure_skip_cache(uint32_t **cache) {
	PPARSE_CTX();
	if (!*cache) *cache = pparse_arena_alloc(&_pc->main_arena, pparse_token_count * sizeof(uint32_t));
	return *cache;
}
static void
p1d_scan_ctrl_init(PParseToken *tok, uint32_t **skip_cache, int brace_depth, uint16_t cur_sid, bool is_for) {
	PPARSE_CTX();
	PParseToken *open = pparse_p1d_find_open_paren(tok);
	if (!open) return;
	PParseToken *close = pparse_pair_known(open);
	PParseToken *init_end = pparse_find_init_semicolon(open, close);
	/* C23 if/switch: `if (int x = 1)` has no semicolon inside the parens —
	 * the declaration runs to the closing `)`. */
	if (!init_end && !is_for) init_end = close;
	if (!init_end) return;
	PParseToken *body_start = pparse_skip_prep_dirs(pparse_next(_pc, close));
	bool body_is_braced = body_start && pparse_match_ch(body_start, '{');
	uint16_t body_sid = body_is_braced ? (uint16_t)body_start->parse_data : 0;
	uint32_t body_end_idx = pparse_idx(_pc, close);
	if (is_for && body_is_braced) {
		/* C99 §6.8.5p3: for-init scope extends to the entire loop body. */
		body_end_idx = pparse_idx(_pc, pparse_pair_known(body_start));
	} else {
		uint32_t *cache = p1d_ensure_skip_cache(skip_cache);
		PParseToken *stmt_end = pparse_skip_one_stmt_impl(body_start, cache);
		if (stmt_end) body_end_idx = pparse_idx(_pc, stmt_end);
		/* C23 §6.8.4.1: if-init scope extends through the else branch. */
		if (!is_for && stmt_end && (tok->tag & PPARSE_TT_IF)) {
			PParseToken *n = pparse_skip_prep_dirs(pparse_next(_pc, stmt_end));
			if (n && pparse_is_else_kw(n)) {
				PParseToken *else_end = pparse_skip_one_stmt_impl(pparse_next(_pc, n), cache);
				if (else_end) body_end_idx = pparse_idx(_pc, else_end);
			}
		}
	}
	p1_scan_init_shadows(
	    open, init_end, body_end_idx, cur_sid, brace_depth, body_sid, body_end_idx, is_for);
}

// Phase 1D: check if a declaration shadows an identifier captured by a
// satisfy the two-pass invariant (no semantic errors during emission).
static void __attribute__((noinline))
p1_check_defer_same_block_shadow(PParseToken *var_name, uint16_t cur_sid, int p1d_cur_func) {
	PPARSE_CTX();
	if (!pparse_feat(PPARSE_F_DEFER) || p1d_cur_func < 0) return;
	char *name = pparse_loc(_pc, var_name);
	int nlen = var_name->len;
	if (!pparse_hashmap_get(&func_meta[p1d_cur_func].defer_name_set, name, nlen)) return;
	int start = func_meta[p1d_cur_func].entry_start;
	for (int i = start; i < p1_entry_count; i++) {
		P1FuncEntry *e = &p1_entries[i];
		if (e->kind != P1K_DEFER || e->scope_id != cur_sid) continue;
		PParseToken *body = pparse_next(_pc, p1_tok(e));
		if (!body) continue;
		PParseToken *body_end = pparse_match_ch(body, '{') ? pparse_pair_known(body) : NULL;
		if (!body_end) body_end = pparse_skip_to_semicolon(body, NULL);
		uint32_t var_idx = pparse_idx(_pc, var_name);
		uint32_t bi = pparse_idx(_pc, body);
		uint32_t ei = body_end ? pparse_idx(_pc, body_end) : UINT32_MAX;
		if (var_idx >= bi && var_idx < ei) continue;
		if (pparse_hashmap_get(&e->defer_captures, name, nlen))
			pparse_error_tok(var_name, PPARSE_ERR_DEFER_SHADOW_SAME_SCOPE, nlen, name);
	}
}

static void p1_check_enum_body_defer_shadow(PParseToken *brace, uint16_t cur_sid, int p1d_cur_func) {
	PPARSE_CTX();
	if (!pparse_feat(PPARSE_F_DEFER) || p1d_cur_func < 0) return;
	PParseToken *end = pparse_pair_known(brace);
	PPARSE_FOR_RANGE(t, pparse_next(_pc, brace), end)
		if (pparse_ann(t) & P1_DEFER_SHADOW_NAME)
			p1_check_defer_same_block_shadow(t, cur_sid, p1d_cur_func);
}

static void reject_defer_fn_body(PParseToken *tok, uint32_t body_tag) {
	static const struct {
		uint32_t tag;
		const char *msg;
	} tab[] = {
	    {PPARSE_TT_SPECIAL_FN,
	     "defer cannot be used in functions that call "
	     "setjmp/longjmp/pthread_exit"},
	    {PPARSE_TT_NORETURN_FN, "defer cannot be used in functions that call vfork()"},
	    {PPARSE_TT_ASM, "defer cannot be used in functions containing asm goto"},
	};

	for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
		if (body_tag & tab[i].tag) pparse_error_tok(tok, tab[i].msg);
}
static void p1_try_alloc_defer(PParseToken *tok, uint16_t cur_sid, int func_idx) {
	PPARSE_CTX();
	/* raw { … } is a suppress region — leave defer as a soft keyword. */
	if (p1_scope_in_raw_block(cur_sid)) return;
	pparse_ann(tok) |= P1_IS_DEFER_KW;
	reject_defer_fn_body(tok, func_meta[func_idx].body_open->tag);
	p1_alloc(P1K_DEFER, cur_sid, tok);
}

/* `do { ... } while (cond);` is the one C statement whose non-body syntax
 * trails the body's own `}` instead of preceding its `{` (if/for/while/switch
 * put everything before the brace). The trivial-chain scanner below treats
 * "nothing but noise between this scope's close and the next" as proof the
 * scope is the final statement; for a do-while body, that trailing
 * `while (cond);` is still part of the SAME statement, not a fresh one, so
 * without this it reads as non-trivial content and the scanner gives up,
 * silently missing `({ ...; do { defer f(); } while(0); })` (defer's cleanup
 * block genuinely is the statement expression's last statement -- GCC gives
 * the whole expression void type either way, so this was a missed diagnostic
 * rather than silent corruption, but switch/if/for/while all catch the
 * equivalent shape and do-while should too). Returns the token after the
 * tail's `;`, or NULL if `t` is not the `while` of a do-while tail closing
 * `sid`. */
static PParseToken *pparse_defer_chain_skip_do_tail(PParseToken *t, uint16_t sid) {
	PPARSE_CTX();
	if (!t || !((t->tag & PPARSE_TT_LOOP) && t->ch0 == 'w')) return NULL;
	uint32_t open_idx = pparse_scope_tree[sid].open_tok_idx;
	if (open_idx == 0) return NULL;
	PParseToken *pre = pparse_walk_back(open_idx, PPARSE_WB_PAST_NOISE);
	if (!pre || !(pre->tag & PPARSE_TT_LOOP) || pre->ch0 != 'd') return NULL;
	PParseToken *op = pparse_skip_noise(_pc, pparse_next(_pc, t));
	if (!op || !pparse_match_ch(op, '(')) return NULL;
	PParseToken *semi = pparse_skip_noise(_pc, pparse_next(_pc, pparse_pair_known(op)));
	if (!semi || !pparse_match_ch(semi, ';')) return NULL;
	return pparse_next(_pc, semi);
}
// Phase 1: check if a defer in scope 'sid' is inside a chain of closing braces
static void pparse_p1_check_defer_stmt_expr_chain(PParseToken *defer_tok, uint16_t sid) {
	PPARSE_CTX();
	while (sid > 0 && sid < pparse_scope_tree_count) {
		uint16_t pid = pparse_scope_tree[sid].parent_id;
		if (pid == 0 || pid >= pparse_scope_tree_count) break;
		PParseToken *t = pparse_next(_pc, &pparse_token_pool[pparse_scope_close(&pparse_scope_tree[sid])]);
		PParseToken *parent_close = &pparse_token_pool[pparse_scope_close(&pparse_scope_tree[pid])];
		bool only_trivial = true;
		while (t && t != parent_close && t->kind != PPARSE_TK_EOF) {
			PParseToken *clean = pparse_skip_noise(_pc, t);
			if (clean != t) {
				t = clean;
				continue;
			}
			if (pparse_match_ch(t, ';') || pparse_match_ch(t, '}')) {
				t = pparse_next(_pc, t);
				continue;
			}
			{
				PParseToken *after_do = pparse_defer_chain_skip_do_tail(t, sid);
				if (after_do) {
					t = after_do;
					continue;
				}
			}
			/* Label: ident [[attr]]...: or ident __attribute__((...)): */
			if (t->kind == PPARSE_TK_IDENT || t->kind == PPARSE_TK_KEYWORD) {
				PParseToken *colon = pparse_skip_noise(_pc, pparse_next(_pc, t));
				PParseToken *after_colon = colon && pparse_match_ch(colon, ':') ? pparse_next(_pc, colon) : NULL;
				if (after_colon && !pparse_match_ch(after_colon, ':')) {
					t = pparse_next(_pc, colon);
					continue;
				}
			}
			only_trivial = false;
			break;
		}
		if (!only_trivial) break;
		if (pparse_scope_tree[pid].is_stmt_expr) pparse_error_tok(defer_tok, PPARSE_ERR_DEFER_LAST_STMT_EXPR);
		sid = pid;
	}
}
static void pparse_hashmap_remove(PParseHashMap *map, char *key, int keylen) {
	if (!map->buckets) return;
	uint32_t hash = pparse_fast_hash(key, keylen);
	int mask = map->capacity - 1;
	for (int i = 0; i <= mask; i++) {
		PParseHashEntry *ent = &map->buckets[(hash + i) & mask];
		if (!ent->key) return;
		if (ent->key == PPARSE_TOMBSTONE) continue;
		if (ent->hash == hash && ent->key_len == (uint16_t)keylen &&
		    prism_memeq_runtime_sized(ent->key, key, (uint32_t)keylen)) {
			ent->key = PPARSE_TOMBSTONE;
			ent->val = NULL;
			map->used--;
			return;
		}
	}
}
// Phase 1 capture analysis: compute the exact set of externally-captured
static void defer_body_populate_captures(
    PParseToken *body, PParseToken *body_end, PParseHashMap *out, PParseHashMap *body_set) {
	PPARSE_CTX();
	PParseHashMap local_decls = {0};

	typedef struct {
		char *name;
		int len;
		int depth;
		void *prev_val;
		bool had_binding;
	} ScopeDecl;

	int se_cap = 32, se_count = 0;
	ScopeDecl *se_stack = pparse_arena_alloc(&_pc->main_arena, se_cap * sizeof(ScopeDecl));
	PParseHashMap for_scopes = {0};
	PParseToken *prev = NULL;
	int bd = 0, pd = 0;
	int block_base_pd[256]; // base paren depth when each block scope opened
	block_base_pd[0] = 0;
	bool in_decl = false, was_in_decl = false;
	int decl_bd = 0, for_init_pd = -1;
	PParseToken *for_header_open = NULL;
	int enum_bd = -1;	   // brace depth of enum body (-1 = not in enum)
	bool decl_is_enum = false; // current in_decl triggered by enum keyword

	for (PParseToken *t = body; t && t != body_end && t->kind != PPARSE_TK_EOF; prev = t, t = pparse_next(_pc, t)) {
		if (pparse_match_ch(t, '{')) {
			if (bd < 255) block_base_pd[bd + 1] = pd;
			if (in_decl && decl_is_enum && enum_bd < 0) enum_bd = bd + 1;
			bd++;
			continue;
		}
		if (pparse_match_ch(t, '}')) {
			bd--;
			if (enum_bd >= 0 && bd < enum_bd) enum_bd = -1;
			if (in_decl && bd < decl_bd) in_decl = false;
			while (se_count > 0 && se_stack[se_count - 1].depth > bd) {
				se_count--;
				ScopeDecl *sd = &se_stack[se_count];
				void *val = pparse_hashmap_get(&local_decls, sd->name, sd->len);
				if (val && val != (void *)1) {
					int stored = (int)((intptr_t)val - 2);
					if (stored == sd->depth) {
						if (sd->had_binding)
							pparse_hashmap_put(
							    &local_decls, sd->name, sd->len, sd->prev_val);
						else
							pparse_hashmap_remove(&local_decls, sd->name, sd->len);
					}
				}
			}
			continue;
		}
		if (pparse_match_set(t, pparse_CH('(') | pparse_CH('['))) {
			if (pparse_match_ch(t, '(') && prev && prev->kind == PPARSE_TK_KEYWORD &&
			    ((prev->tag & PPARSE_TT_LOOP) || (prev->tag & PPARSE_TT_IF) || (prev->tag & PPARSE_TT_SWITCH))) {
				for_init_pd = pd + 1;
				for_header_open = t;
			}
			pd++;
			continue;
		}
		if (pparse_match_set(t, pparse_CH(')') | pparse_CH(']'))) {
			pd--;
			if (for_init_pd >= 0 && pd < for_init_pd) for_init_pd = -1;
			continue;
		}
		if (pparse_match_ch(t, ';')) {
			in_decl = false;
			was_in_decl = false;
			decl_is_enum = false;
			if (for_init_pd >= 0 && pd == for_init_pd) for_init_pd = -1;
			continue;
		}
		int bpd = (bd > 0) ? block_base_pd[bd] : 0;
		if (pd == bpd && pparse_match_ch(t, '=')) {
			in_decl = false;
			continue;
		}
		if (pd == bpd && pparse_match_ch(t, ',') && was_in_decl &&
		    (bd == decl_bd || (enum_bd >= 0 && bd == enum_bd))) {
			in_decl = true;
			continue;
		}
		if (((bd > 0 && pd == bpd) || (for_init_pd >= 0 && pd == for_init_pd)) &&
		    (pparse_is_type_keyword(t) || (t->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_STORAGE | PPARSE_TT_TYPEDEF)))) {
			if (!in_decl) decl_is_enum = false;
			in_decl = true;
			was_in_decl = true;
			decl_bd = bd;
			if ((t->tag & PPARSE_TT_SUE) && t->ch0 == 'e') decl_is_enum = true;
			continue;
		}
		if ((t->kind == PPARSE_TK_IDENT || t->kind == PPARSE_TK_KEYWORD) && !(prev && (prev->tag & PPARSE_TT_MEMBER))) {
			char *name = pparse_loc(_pc, t);
			int nlen = t->len;
			void *val = pparse_hashmap_get(&local_decls, name, nlen);
			if (val == (void *)1) continue;
			void *fv = pparse_hashmap_get(&for_scopes, name, nlen);
			if (fv) {
				uint32_t fe = (uint32_t)(uintptr_t)fv;
				if (pparse_idx(_pc, t) <= fe) continue; // hidden by for-init
				pparse_hashmap_put(&for_scopes, name, nlen, NULL);
			}
			if (val) {
				int dd = (int)((intptr_t)val - 2);
				if (bd > 0 && dd >= 0 && bd >= dd) continue;
			}
			if (bd > 0 && in_decl && pd == bpd) {
				void *prev_val = pparse_hashmap_get(&local_decls, name, nlen);
				if (se_count >= se_cap) {
					int new_cap = se_cap * 2;
					ScopeDecl *ns =
					    pparse_arena_alloc(&_pc->main_arena, new_cap * sizeof(ScopeDecl));
					memcpy(ns, se_stack, se_count * sizeof(ScopeDecl));
					se_stack = ns;
					se_cap = new_cap;
				}
				se_stack[se_count++] =
				    (ScopeDecl){name, nlen, bd, prev_val, prev_val != NULL};
				pparse_hashmap_put(&local_decls, name, nlen, (void *)((intptr_t)(bd + 2)));
				continue;
			}
			if (for_init_pd >= 0 && in_decl) {
				uint32_t fbe = 0;
				if (for_header_open) {
					PParseToken *end =
					    pparse_skip_one_stmt_impl(pparse_next(_pc, pparse_pair_known(for_header_open)), NULL);
					fbe = end ? pparse_idx(_pc, end) : 0;
				}
				pparse_hashmap_put(&for_scopes, name, nlen, (void *)(uintptr_t)fbe);
				continue;
			}
			pparse_hashmap_put(out, name, nlen, (void *)1);
			pparse_hashmap_put(body_set, name, nlen, (void *)1);
			pparse_hashmap_put(&local_decls, name, nlen, (void *)1);
		}
	}
}
// Phase 1F: validate defer body and populate name set.
static void p1_reject_defer_in_uneval_operand(PParseToken *defer_tok, uint16_t sid) {
	PPARSE_CTX();
	for (uint16_t s = sid; s > 0 && s < pparse_scope_tree_count; s = pparse_scope_tree[s].parent_id) {
		if (!pparse_scope_tree[s].is_stmt_expr) continue;
		PParseToken *brace = &pparse_token_pool[pparse_scope_tree[s].open_tok_idx];
		if (pparse_idx(_pc, brace) == 0) break;
		PParseToken *se_open = &pparse_token_pool[pparse_idx(_pc, brace) - 1]; /* '(' of ({ */
		if (!pparse_match_ch(se_open, '(')) break;
		PParseToken *intro = pparse_walk_back(pparse_idx(_pc, se_open), PPARSE_WB_ATTR_NOISE);
		/* Peel redundant paren wrappers: sizeof((({…}))). */
		while (intro && pparse_match_ch(intro, '(') && (intro->flags & PPARSE_TF_OPEN)) {
			PParseToken *outer_close = pparse_pair_known(intro);
			PParseToken *se_close_brace = &pparse_token_pool[pparse_scope_close(&pparse_scope_tree[s])];
			PParseToken *se_paren_close = pparse_next(_pc, se_close_brace);
			if (!se_paren_close ||
			    pparse_idx(_pc, outer_close) < pparse_idx(_pc, se_paren_close))
				break;
			intro = pparse_walk_back(pparse_idx(_pc, intro), PPARSE_WB_ATTR_NOISE);
		}
		if (intro && (pparse_is_unevaluated_operand_intro(intro) || (intro->tag & PPARSE_TT_GENERIC)))
			pparse_error_tok(defer_tok,
				  "'defer' inside an unevaluated operand "
				  "(sizeof/_Alignof/typeof/_Generic) has no effect");
		break;
	}
}
static void __attribute__((noinline))
p1d_validate_defer(PParseToken *tok, int p1d_cur_func, bool p1d_ctrl_pending, uint16_t cur_sid, int brace_depth) {
	PPARSE_CTX();
	P1FuncEntry *defer_entry = NULL;
	if (p1d_cur_func >= 0 && p1_entry_count > func_meta[p1d_cur_func].entry_start) {
		P1FuncEntry *last = &p1_entries[p1_entry_count - 1];
		if (last->kind == P1K_DEFER && last->token_index == pparse_idx(_pc, tok)) defer_entry = last;
	}
	/* File-scope defer: no scope to unwind. Struct/initializer bodies are
	 * exempt — a member or initializer field spelled `defer` never reaches
	 * Pass 2's defer machinery and passes through to the backend. */
	if (p1d_cur_func < 0 &&
	    !(cur_sid > 0 && cur_sid < pparse_scope_tree_count &&
	      (pparse_scope_tree[cur_sid].is_struct || pparse_scope_tree[cur_sid].is_init)))
		pparse_error_tok(tok, "defer outside of any scope");
	/* In library mode the caller's conditionals have not been resolved by
	 * cc -E. Moving a deferred action past its closing #endif would make it
	 * unconditional. Reject that source shape rather than silently changing
	 * its runtime semantics; the CLI never sees it because preprocessing has
	 * already selected one arm. */
	if (pparse_token_in_pp_conditional(tok))
		pparse_error_tok(tok,
			  "'defer' inside an unresolved preprocessor conditional cannot be "
			  "lowered safely; preprocess the source first or move the conditional "
			  "inside the defer body");
	// Context validation (moved from Pass 2 handle_defer_keyword)
	if (p1d_cur_func >= 0) {
		reject_defer_context(tok,
				     false,
				     p1d_ctrl_pending,
				     cur_sid < pparse_scope_tree_count && pparse_scope_tree[cur_sid].is_stmt_expr,
				     cur_sid < pparse_scope_tree_count && pparse_scope_tree[cur_sid].is_switch);
		pparse_p1_check_defer_stmt_expr_chain(tok, cur_sid);
		p1_reject_defer_in_uneval_operand(tok, cur_sid);
	}
	{
		PParseToken *body = pparse_skip_noise(_pc, pparse_next(_pc, tok));
		PParseToken *body_end = body && pparse_match_ch(body, '{') ? pparse_pair_known(body) : NULL;
		if (body_end) {
			pparse_ann(tok) |= P1_DEFER_BODY_RECIPE;
			tok->pair_idx = pparse_idx(_pc, body_end);
		}
		if (body && !pparse_match_ch(body, '{')) {
			PParseToken *semi = pparse_skip_to_semicolon(body, NULL);
			reject_defer_unterminated(tok, body, semi);
			pparse_ann(tok) |= P1_DEFER_BODY_RECIPE;
			tok->pair_idx = pparse_idx(_pc, semi);
			/* Braceless defer bodies are erased as a unit; any `orelse` in
			 * them leaks as a soft keyword into the backend. Require braces. */
			if (pparse_feat(PPARSE_F_ORELSE)) {
				PPARSE_FOR_RANGE(s, body, semi) {
					PPARSE_SKIP_GROUP_ON_CLOSE(s)
					if (pparse_is_orelse_kw_shadow(s))
						pparse_error_tok(s,
							  "'orelse' inside a braceless defer body is not "
							  "supported; wrap the defer body in braces: "
							  "`defer { ... }`");
				}
			}
			/* Braceless declarations either mis-scan or create a dead object. */
			if ((body->tag &
			     (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_STORAGE)) ||
			    (body->flags & PPARSE_TF_RAW) || pparse_is_known_typedef(body)) {
				PParseToken *ts2 = pparse_skip_noise(_pc, body);
				while (ts2 && (ts2->flags & PPARSE_TF_RAW) && !pparse_is_known_typedef(ts2))
					ts2 = pparse_skip_noise(_pc, pparse_next(_pc, ts2));
				PParseTypeSpec tr2 = ts2 ? pparse_type_specifier(ts2)
							 : (PParseTypeSpec){0};
					if (tr2.saw_type)
						pparse_error_tok(tok,
							  "a declaration as a braceless defer body is not "
							  "supported (it mis-scans across the following "
							  "statements); wrap the defer body in braces: "
							  "`defer { ... }`");
			}
			/* A braceless defer body cannot escape its enclosing group. */
			PPARSE_FOR_RANGE(s, body, semi) {
				PPARSE_SKIP_GROUP_ON_CLOSE(s)
				if (s->flags & PPARSE_TF_CLOSE)
					pparse_error_tok(tok,
						  "stray 'defer' in an expression "
						  "position (body would cross the "
						  "enclosing ')' or ']')");
			}
			// Braceless defer body tokens never hit p1d_probe_declaration at
			// stmt_start; annotate P1_IS_DECL so Pass 2 zeroinit sees the decl.
			if (pparse_feat(PPARSE_F_ZEROINIT) && brace_depth > 0 &&
			    (body->tag &
			     (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_STORAGE))) {
				PParseToken *ts = pparse_skip_noise(_pc, body);
				PParseTypeSpec tr = pparse_type_specifier(ts);
				if (tr.saw_type) pparse_ann(ts) |= P1_IS_DECL;
			}
		}
	}
	pparse_validate_defer_statement(pparse_next(_pc, tok), false, false, 0);
	if (defer_entry) {
		PParseToken *body = pparse_next(_pc, tok);
		PParseToken *body_end = body && pparse_match_ch(body, '{') ? pparse_pair_known(body) : NULL;
		if (body && !body_end) body_end = pparse_skip_to_semicolon(body, NULL);
		if (body_end)
			defer_body_populate_captures(body,
						     body_end,
						     &func_meta[p1d_cur_func].defer_name_set,
						     &defer_entry->defer_captures);
	}
}

// Phase 1G: reject orelse in VLA bracket dimensions of function prototype
// the parameter names do not exist.
static void p1d_scan_param_bracket_orelse(PParseToken *open_paren, bool is_proto) {
	PPARSE_CTX();
	PParseToken *close = pparse_pair_known(open_paren);
	PPARSE_FOR_RANGE(t, pparse_next(_pc, open_paren), close) {
		if (pparse_match_ch(t, '[') && !(t->flags & PPARSE_TF_C23_ATTR)) {
			PParseToken *bc = pparse_pair_known(t);
			PPARSE_FOR_RANGE(s, pparse_next(_pc, t), bc) {
				PPARSE_SKIP_GROUP_ON_CLOSE(s)
				if (pparse_is_orelse_kw_shadow(s))
					pparse_error_tok(s,
						  is_proto ? PPARSE_ERR_ORELSE_PROTO_DIM: PPARSE_ERR_ORELSE_DEFN_DIM);
				/* Reject defer statement shapes inside parameter dimensions. */
				if (pparse_feat(PPARSE_F_DEFER) && (s->tag & PPARSE_TT_DEFER) && pparse_next(_pc, s) &&
				    (pparse_is_identifier_like(pparse_next(_pc, s)) || pparse_match_ch(pparse_next(_pc, s), '{')))
					pparse_error_tok(s, PPARSE_ERR_DEFER_EXPR_CTX);
			}
			t = bc;
			continue;
		}
		if ((t->flags & PPARSE_TF_OPEN) && pparse_match_ch(t, '(')) {
			p1d_scan_param_bracket_orelse(t, is_proto);
			t = pparse_pair_known(t);
			continue;
		}
		PPARSE_SKIP_GROUP_ON_CLOSE(t)
	}
}

static bool pparse_bracket_in_compound_literal_type(PParseToken *open_bracket) {
	PPARSE_CTX();
	PParseToken *close = pparse_pair_known(open_bracket);
	PParseToken *t = pparse_next(_pc, close);
	while (t && t->kind != PPARSE_TK_EOF) {
		if (pparse_match_ch(t, '[') && !(t->flags & PPARSE_TF_C23_ATTR)) {
			t = pparse_next(_pc, pparse_pair_known(t));
			continue;
		}
		if (t->flags & PPARSE_TF_C23_ATTR) {
			t = pparse_next(_pc, pparse_pair_known(t));
			continue;
		}
		break;
	}
	if (!t || !pparse_match_ch(t, ')')) return false;
	PParseToken *after = pparse_skip_noise(_pc, pparse_next(_pc, t));
	return after && pparse_match_ch(after, '{');
}
static bool pparse_bracket_has_leading_member_designator(PParseToken *member_name) {
	PPARSE_CTX();
	PParseToken *name = member_name;
	while (name && pparse_is_identifier_like(name)) {
		PParseToken *member = pparse_walk_back(pparse_idx(_pc, name), PPARSE_WB_PAST_NOISE);
		if (!member || !(member->tag & PPARSE_TT_MEMBER) || !pparse_match_ch(member, '.')) return false;
		PParseToken *left = pparse_walk_back(pparse_idx(_pc, member), PPARSE_WB_PAST_NOISE);
		if (!left || pparse_match_ch(left, '{') || pparse_match_ch(left, ',')) return true;
		if (!pparse_is_identifier_like(left)) return false;
		name = left;
	}
	return false;
}
static bool pparse_bracket_is_designator_index(PParseToken *open_bracket) {
	PPARSE_CTX();
	PParseToken *prev = pparse_walk_back(pparse_idx(_pc, open_bracket), PPARSE_WB_PAST_NOISE);
	if (!prev) return false;
	if (pparse_match_ch(prev, '{') || pparse_match_ch(prev, ',') || pparse_match_ch(prev, ']')) return true;
	if (!pparse_is_identifier_like(prev)) return false;
	PParseToken *before = pparse_walk_back(pparse_idx(_pc, prev), PPARSE_WB_PAST_NOISE);
	if (before && (before->tag & PPARSE_TT_MEMBER))
		return pparse_bracket_has_leading_member_designator(prev) ||
		       pparse_bracket_in_offsetof_member(open_bracket);
	return before && pparse_match_ch(before, ',') && pparse_bracket_in_offsetof_member(open_bracket);
}
static bool pparse_bracket_contains_gnu_range(PParseToken *open_bracket) {
	PPARSE_CTX();
	PParseToken *close = pparse_pair_known(open_bracket);
	PPARSE_FOR_RANGE(t, pparse_next(_pc, open_bracket), close) {
		if (t->kind == PPARSE_TK_PUNCT && t->len == 3 && t->ch0 == '.' && pparse_loc(_pc, t)[1] == '.' &&
		    pparse_loc(_pc, t)[2] == '.')
			return true;
		if (t->flags & PPARSE_TF_OPEN) t = pparse_pair_known(t);
	}
	return false;
}
static uint32_t pparse_array_bracket_context(PParseToken *open_bracket) {
	if (!(open_bracket->flags & PPARSE_TF_LINK_JUMP) &&
	    (open_bracket->parse_data & PPARSE_BRACKET_CONTEXT_KNOWN))
		return open_bracket->parse_data;
	uint32_t bits = 0;
	if (pparse_bracket_in_compound_literal_type(open_bracket)) bits |= PPARSE_BRACKET_COMPOUND_LITERAL;
	if (pparse_bracket_is_designator_index(open_bracket)) {
		bits |= PPARSE_BRACKET_DESIGNATOR;
		if (pparse_bracket_contains_gnu_range(open_bracket)) bits |= PPARSE_BRACKET_GNU_RANGE;
	}
	if (pparse_bracket_in_alignof_type_operand(open_bracket)) bits |= PPARSE_BRACKET_ALIGNOF_TYPE;
	if (pparse_bracket_in_generic_association_type(open_bracket)) bits |= PPARSE_BRACKET_GENERIC_ASSOC_TYPE;
	bits |= PPARSE_BRACKET_CONTEXT_KNOWN;
	if (!(open_bracket->flags & PPARSE_TF_LINK_JUMP)) open_bracket->parse_data = bits;
	return bits;
}

/* Bracket path: require shadow-kw prev even when orelse is not a typedef. */
static inline bool orelse_kw_at_shadow(PParseToken *t, PParseToken *prev) {
	return (t->tag & PPARSE_TT_ORELSE) && !(prev && (prev->tag & PPARSE_TT_MEMBER)) && !pparse_is_known_function_call(t) &&
	       pparse_orelse_shadow_is_kw(prev);
}

static PParseToken *p1d_reject_group_orelse(PParseToken *intro, bool skip_dims) {
	PPARSE_CTX();
	PParseToken *open = pparse_skip_noise(_pc, pparse_next(_pc, intro));
	if (!open || !pparse_match_ch(open, '(') || !(open->flags & PPARSE_TF_OPEN)) return NULL;
	PParseToken *close = pparse_pair_known(open);
	PParseToken *prev = open;
	PPARSE_FOR_RANGE(t, pparse_next(_pc, open), close) {
		if (skip_dims && pparse_match_ch(t, '[') && (t->flags & PPARSE_TF_OPEN) &&
		    !(t->flags & PPARSE_TF_C23_ATTR)) {
			t = pparse_pair_known(t);
			prev = t;
			continue;
		}
		if (pparse_feat(PPARSE_F_ORELSE) &&
		    (orelse_kw_at(t, prev) || orelse_after_type_in_parens(t, prev)))
			pparse_error_tok(t, PPARSE_ERR_ORELSE_IN_PARENS);
		prev = t;
	}
	return close;
}

static void __attribute__((noinline))
p1d_classify_bracket_orelse_ex(PParseToken *tok, uint16_t cur_sid, int p1d_cur_func, bool hard_ctx,
			       bool allow_se_hoist) {
	PPARSE_CTX();
	PParseToken *close = pparse_pair_known(tok);
	if (!(tok->flags & PPARSE_TF_HAS_PRISM)) return;
	/* Function-parameter dimensions reject orelse before annotation. */
	PParseToken *param_open = pparse_feat(PPARSE_F_ORELSE) ? pparse_function_param_open(tok) : NULL;
	if (param_open) {
		PParseToken *pc = pparse_pair_known(param_open);
		PParseToken *after = pparse_skip_asm_specifier_trail(pparse_next(_pc, pc));
		bool is_proto = !pparse_match_ch(after, '{');
		/* Walk every token — do not skip nested groups, or
		 * `int a[sizeof(0 orelse 1)]` in a prototype would lower. */
		PParseToken *prev_s = tok;
		PPARSE_FOR_RANGE(s, pparse_next(_pc, tok), close) {
			if (orelse_kw_at(s, prev_s) || orelse_after_type_in_parens(s, prev_s))
				pparse_error_tok(s,
					  is_proto ? PPARSE_ERR_ORELSE_PROTO_DIM: PPARSE_ERR_ORELSE_DEFN_DIM);
			prev_s = s;
		}
		return;
	}
	bool in_struct = cur_sid > 0 && cur_sid < pparse_scope_tree_count && pparse_scope_tree[cur_sid].is_struct;
	bool found_oe = false;
	PParseToken *prev_d0_oe = NULL;
	uint32_t bracket_context = 0;
	int oe_depth = 0;
	PParseToken *open_stack[64];
	PParseToken *prev_bracket = tok;
	int paren_depth_scan = 0;
	int brace_depth_scan = 0;
	PPARSE_FOR_RANGE(s, pparse_next(_pc, tok), close) {
		if (s->kind == PPARSE_TK_PREP_DIR) continue;
		/* Nested unevaluated/type operands must not become dimension orelse. */
		if (pparse_is_unevaluated_operand_intro(s) || (s->tag & PPARSE_TT_GENERIC) ||
		    (s->flags & PPARSE_TF_STATIC_ASSERT)) {
			PParseToken *rp = p1d_reject_group_orelse(s, false);
			if (rp) {
				s = rp;
				prev_bracket = rp;
				continue;
			}
		}
		/* Type-junk in a dimension (`int orelse`, `_BitInt(N) orelse`) is
		 * not a keyword under orelse_kw_at_shadow, so without an explicit
		 * reject the token leaks to the C backend. */
		if (pparse_feat(PPARSE_F_ORELSE) && orelse_after_type_in_parens(s, prev_bracket))
			pparse_error_tok(s,
				  "'orelse' cannot be used after a type specifier in an "
				  "array dimension");
		/* Reject defer statements, but preserve defer identifiers in dimensions. */
		if (pparse_feat(PPARSE_F_DEFER) && brace_depth_scan == 0 && (s->tag & PPARSE_TT_DEFER) &&
		    !pparse_is_known_typedef(s) && !pparse_is_known_function_call(s) &&
		    !(prev_bracket && (prev_bracket->tag & PPARSE_TT_MEMBER))) {
			PParseToken *nx = pparse_skip_noise(_pc, pparse_next(_pc, s));
			/* Expression continuation after a primary → identifier use.
			 * Anything else after the keyword is a stray defer statement. */
			bool expr_primary =
			    nx && (pparse_match_ch(nx, ']') || pparse_match_ch(nx, ')') || pparse_match_ch(nx, ',') ||
				   pparse_match_ch(nx, ';') || pparse_match_ch(nx, ':') || pparse_match_ch(nx, '?') ||
				   pparse_match_ch(nx, '.') || (nx->tag & PPARSE_TT_MEMBER) || pparse_match_ch(nx, '(') ||
				   pparse_match_ch(nx, '[') ||
				   (nx->kind == PPARSE_TK_PUNCT && !pparse_match_ch(nx, '{') && !pparse_match_ch(nx, '}')));
			if (nx && !expr_primary) pparse_error_tok(s, PPARSE_ERR_DEFER_EXPR_CTX);
		}
		if (orelse_kw_at_shadow(s, prev_bracket)) {
			/* Over-paren is always untransformable (not hoist-related). */
			if (paren_depth_scan > 1)
				pparse_error_tok(s,
					  "'orelse' inside array dimension could not be transformed; "
					  "if wrapped in outer parentheses, remove them: "
					  "use '[f() orelse 1]' not '[(f() orelse 1)]'");
			if (hard_ctx) {
				if (p1d_cur_func < 0)
					pparse_error_tok(s,
						  "orelse inside array dimension at file scope is not allowed "
						  "(cannot hoist temporary variable outside a function body)");
				if (in_struct)
					pparse_error_tok(s,
						  "orelse inside array dimension in a struct/union body "
						  "cannot be transformed (statement expressions are not "
						  "allowed in struct/union definitions)");
			}
			validate_bracket_orelse(s);
			if (oe_depth == 0) {
				if (prev_d0_oe)
					reject_orelse_side_effects(pparse_next(_pc, prev_d0_oe), s, PPARSE_OE_SE_ORELSE_IN_ARRAY_DIMENSION_CHAIN);
				else if (!allow_se_hoist)
					reject_orelse_side_effects(pparse_next(_pc, tok), s, PPARSE_OE_SE_DIM_LHS);
				if (!(bracket_context & PPARSE_BRACKET_CONTEXT_KNOWN))
					bracket_context = pparse_array_bracket_context(tok);
				uint32_t needs_ice = bracket_context &
						     (PPARSE_BRACKET_COMPOUND_LITERAL | PPARSE_BRACKET_DESIGNATOR |
						      PPARSE_BRACKET_ALIGNOF_TYPE | PPARSE_BRACKET_GENERIC_ASSOC_TYPE);
				bool lhs_nonconstant = needs_ice && pparse_expr_maybe_nonconstant(pparse_next(_pc, tok), s);
				if ((bracket_context & PPARSE_BRACKET_COMPOUND_LITERAL) && lhs_nonconstant)
					pparse_error_tok(s,
						  "'orelse' in compound-literal array dimension with "
						  "non-constant LHS (compound literals cannot be VLAs); "
						  "use a constant dimension or a named array");
				/* Designator indices must be ICEs (C11 §6.7.9). Lowering
				 * `[idx orelse 1]` to a ternary would emit illegal C. */
				if ((bracket_context & PPARSE_BRACKET_DESIGNATOR) && lhs_nonconstant)
					pparse_error_tok(s,
						  "'orelse' in a designated-initializer index requires an "
						  "integer constant expression on the left-hand side; "
						  "hoist a constant index or use a positional initializer");
				/* GNU range designators cannot be ternary-lowered. */
				if (bracket_context & PPARSE_BRACKET_GNU_RANGE)
					pparse_error_tok(s,
						  "'orelse' cannot be used in a GNU range designator "
						  "'[first ... last]' (ternary lowering would destroy "
						  "the range syntax)");
				/* `_Alignof(int[n orelse 1])` — alignof needs a non-VLA type. */
				if ((bracket_context & PPARSE_BRACKET_ALIGNOF_TYPE) && lhs_nonconstant)
					pparse_error_tok(s,
						  "'orelse' in an array dimension inside "
						  "_Alignof/alignof requires an integer constant "
						  "expression on the left-hand side");
				/* `_Generic(…, int[n orelse 1]: …)` — association types too. */
				if ((bracket_context & PPARSE_BRACKET_GENERIC_ASSOC_TYPE) && lhs_nonconstant)
					pparse_error_tok(s,
						  "'orelse' in an array dimension inside a "
						  "_Generic association type requires an integer "
						  "constant expression on the left-hand side");
				prev_d0_oe = s;
			} else {
				/* Nested orelse: check LHS of the innermost group only.
				 * For call args, include the callee so get_n(x orelse 5) rejects.
				 * Past the tracked depth, fall back to the whole dimension.
				 * `oe_depth > 0` is defensive: the delimiter-matching pass
				 * rejects unbalanced closers before Phase 1 runs, so a
				 * negative depth is unreachable today and the guard costs a
				 * compare. Without it a stray close would index
				 * open_stack[-2], and the bound that keeps it from
				 * happening lives in a different pass. */
				PParseToken *grp =
				    (oe_depth > 0 && oe_depth <= 64) ? open_stack[oe_depth - 1] : NULL;
				PParseToken *se_start = grp ? pparse_next(_pc, grp) : pparse_next(_pc, tok);
				if (grp && pparse_match_ch(grp, '(')) {
					PParseToken *before = pparse_walk_back(pparse_idx(_pc, grp), PPARSE_WB_PAST_NOISE);
					if (before && pparse_is_valid_varname(before) && !pparse_is_type_keyword(before))
						se_start = before;
				}
				reject_orelse_side_effects(se_start, s, PPARSE_OE_SE_DIM_LHS);
			}
			pparse_ann(s) |= P1_OE_BRACKET | P1_IS_ORELSE_KW;
			found_oe = true;
		}
		if (s->flags & PPARSE_TF_OPEN) {
			if (oe_depth < 64) open_stack[oe_depth] = s;
			oe_depth++;
		}
		if (s->flags & PPARSE_TF_CLOSE) oe_depth--;
		if (pparse_match_ch(s, '(')) paren_depth_scan++;
		else if (pparse_match_ch(s, ')'))
			paren_depth_scan--;
		if (pparse_match_ch(s, '{')) brace_depth_scan++;
		else if (pparse_match_ch(s, '}'))
			brace_depth_scan--;
		prev_bracket = s;
	}
	if (found_oe) {
		PParseToken *ppc = pparse_span_find_pp_conditional(pparse_next(_pc, tok), close, false);
		if (ppc)
			pparse_error_tok(ppc,
				  "'orelse' inside array dimension cannot be used when the "
				  "dimension spans preprocessor conditionals: the "
				  "transpiler would emit tokens from all branches, "
				  "producing invalid C; "
				  "use 'cc -E' preprocessing or a temporary variable");
		pparse_ann(tok) |= P1_OE_BRACKET;
	}
}

/* Annotate typeof(...) orelse for Pass 2; reject struct/file-scope when hard_ctx. */
static void p1d_annotate_typeof_orelse(PParseToken *typeof_tok, uint16_t cur_sid, int p1d_cur_func, bool hard_ctx) {
	PPARSE_CTX();
	PParseToken *paren = pparse_next(_pc, typeof_tok);
	if (!paren || !pparse_match_ch(paren, '(')) return;
	PParseToken *close = pparse_pair_known(paren);
	const char *msg = NULL;
	if (hard_ctx) {
		if (cur_sid > 0 && cur_sid < pparse_scope_tree_count && pparse_scope_tree[cur_sid].is_struct)
			msg = "'orelse' inside typeof in a struct/union body "
			      "cannot be transformed; use the resolved type directly";
		else if (p1d_cur_func < 0)
			msg = "'orelse' inside typeof at file scope is not allowed";
	}
	if (msg) {
		PPARSE_FOR_RANGE(s, pparse_next(_pc, paren), close)
			if ((s->tag & PPARSE_TT_ORELSE) && !pparse_token_has_binding(s)) pparse_error_tok(s, msg);
		return;
	}
	PParseToken *prev_typeof = paren;
	PParseToken *se_start = pparse_next(_pc, paren);
	bool typeof_has_oe = false;
	PPARSE_FOR_RANGE(s, pparse_next(_pc, paren), close) {
		/* Uneval intros nested in typeof — `typeof(sizeof(0 orelse 1))` —
		 * must reject like a bare `sizeof(… orelse …)`, not lower.
		 * Do NOT treat nested `typeof` as uneval here: expression
		 * `typeof(typeof(p orelse q))` is a supported transform. */
		if ((s->flags & (PPARSE_TF_SIZEOF | PPARSE_TF_STATIC_ASSERT)) ||
		    (s->tag & (PPARSE_TT_GENERIC | PPARSE_TT_ALIGNAS))) {
			PParseToken *rp = p1d_reject_group_orelse(s, true);
			if (rp) {
				s = rp;
				prev_typeof = rp;
				continue;
			}
		}
		if (s->tag & PPARSE_TT_ORELSE) {
			/* `typeof(int orelse 0)` is type-junk, not an expression
			 * orelse — do not lower `int` into a ternary operand. */
			if (orelse_after_type_in_parens(s, prev_typeof)) {
				pparse_error_tok(s,
					  PPARSE_ERR_ORELSE_IN_PARENS);
				prev_typeof = s;
				continue;
			}
			if (orelse_kw_at(s, prev_typeof)) {
				pparse_ann(s) |= P1_IS_ORELSE_KW;
				typeof_has_oe = true;
				reject_orelse_side_effects(se_start, s, PPARSE_OE_SE_TYPEOF_LHS);
				se_start = pparse_next(_pc, s);
				prev_typeof = s;
				continue;
			}
		}
		prev_typeof = s;
	}
	if (typeof_has_oe) pparse_ann(paren) |= P1_OE_BRACKET;
}

static void p1d_reject_orelse_chain_after_ctrl(PParseToken *oe_kw) {
	PPARSE_CTX();
	PParseToken *act = pparse_next(_pc, oe_kw);
	if (!act) return;
	PParseToken *u;
	PParseToken *sp;
	if (act->tag & (PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE | PPARSE_TT_GOTO)) {
		u = pparse_next(_pc, act);
		/* Skip optional label after break/continue/goto — but not a following
		 * `orelse` keyword (would misread `continue orelse x` as labeled). */
		if (!(act->tag & PPARSE_TT_RETURN) && u && pparse_is_identifier_like(u) && !orelse_kw_at_bare(u, act))
			u = pparse_next(_pc, u);
		sp = act;
	} else if (pparse_match_ch(act, '{') && (act->flags & PPARSE_TF_OPEN)) {
		/* Block-form action: further `orelse` after `}` cannot continue the chain. */
		sp = pparse_pair_known(act);
		u = pparse_next(_pc, sp);
	} else
		return;
	for (; u && u->kind != PPARSE_TK_EOF; u = pparse_next(_pc, u)) {
		if (u->flags & PPARSE_TF_OPEN) {
			u = sp = pparse_pair_known(u);
			continue;
		}
		if ((u->flags & PPARSE_TF_CLOSE) || pparse_match_ch(u, ';') || pparse_match_ch(u, ',')) break;
		if ((pparse_ann(u) & P1_IS_ORELSE_KW) || orelse_kw_at_bare(u, sp))
			pparse_error_tok(u,
				  "'orelse' chain cannot continue after a "
				  "control-flow action (return/goto/break/continue/block)");
		sp = u;
	}
}

static bool orelse_next_is_empty_action(PParseToken *nx) {
	PPARSE_CTX();
	if (!nx) return false;
	if (pparse_match_ch(nx, ';') || pparse_match_ch(nx, ',')) return true;
	/* Mid-chain `… orelse orelse …` is empty unless the second `orelse` is
	 * clearly an identifier use: `orelse()`, `orelse[i]`, `orelse.x`. */
	if (!pparse_is_orelse_kw_shadow(nx)) return false;
	PParseToken *a = pparse_next(_pc, nx);
	if (a && (pparse_match_ch(a, '(') || pparse_match_ch(a, '[') || (a->tag & PPARSE_TT_MEMBER))) return false;
	return true;
}

static bool p1d_lhs_is_const_shadow(PParseToken *start, PParseToken *eq_tok) {
	PPARSE_CTX();
	PParseToken *lhs_start = pparse_skip_noise(_pc, start);
	PParseToken *lhs_end = eq_tok;
	while (lhs_start && pparse_match_ch(lhs_start, '(')) {
		PParseToken *close = pparse_pair_known(lhs_start);
		if (pparse_skip_noise(_pc, pparse_next(_pc, close)) != lhs_end) break;
		lhs_start = pparse_skip_noise(_pc, pparse_next(_pc, lhs_start));
		lhs_end = close;
	}
	PParseToken *name = pparse_skip_noise(_pc, lhs_start);
	if (!name || !pparse_is_valid_varname(name)) return false;
	if (pparse_skip_noise(_pc, pparse_next(_pc, name)) != lhs_end) return false;
	PParseTypedefEntry *entry = pparse_typedef_lookup(_pc, name);
	return entry && entry->is_shadow && entry->is_const;
}

// 0 = not orelse; 1 = skip (fn/label/typedef-as-name); 2 = annotated as
// decl-init orelse
static int p1d_try_annotate_init_orelse(PParseToken *t,
					PParseToken *prev,
					bool (*ending)(PParseToken *),
					bool reject_ternary,
					int ternary_depth,
					bool *out_has_orelse,
					PParseToken **out_first_orelse) {
	if (!(t->tag & PPARSE_TT_ORELSE)) return 0;
	if (pparse_is_known_function_call(t)) return 1;
	/* `.orelse` / `->orelse` — member name, not the operator. The following
	 * `orelse` (if any) is classified on the next iteration. */
	if (prev && (prev->tag & PPARSE_TT_MEMBER)) return 1;
	if (pparse_orelse_is_label_or_goto_target(t, prev)) return 1;
	PParseIdentifierBindingKind binding = pparse_identifier_binding_kind(t);
	if (binding != PPARSE_BINDING_NONE && !(prev && ending(prev))) return 1;
	/* Shadowed `orelse` after an expression-ending token that is not a
	 * keyword context (e.g. `sizeof orelse`, `x + orelse`) stays an
	 * identifier — do not bake P1_IS_ORELSE_KW. */
	if (binding == PPARSE_BINDING_VALUE && !pparse_orelse_shadow_is_kw(prev)) return 1;
	/* `int x = int orelse 1` / `= _BitInt(8) orelse 1` — type-junk LHS. */
	if (orelse_after_type_in_parens(t, prev))
		pparse_error_tok(t,
			  "'orelse' cannot be used after a type specifier in a "
			  "declaration initializer");
	if (prev && !ending(prev)) pparse_error_tok(t, PPARSE_ERR_ORELSE_STMT_LEVEL);
	if (reject_ternary && ternary_depth > 0) pparse_error_tok(t, PPARSE_ERR_ORELSE_TERNARY);
	pparse_ann(t) |= P1_OE_DECL_INIT | P1_IS_ORELSE_KW;
	if (!*out_first_orelse) *out_first_orelse = t;
	*out_has_orelse = true;
	return 2;
}

static PParseToken *
p1d_scan_balanced_group(PParseToken *tok, int brace_depth, int cur_func, uint16_t cur_sid, PParseToken *prev_saved) {
	PPARSE_CTX();
	const bool has_orelse = pparse_feat(PPARSE_F_ORELSE);
	PParseToken *group_end = pparse_pair_known(tok);
	PParseToken *stmt_expr_open = NULL;
	PParseToken *prev_inner = NULL;
	int se_depth = 0;
	PParseToken *se_close_stack[64];
	int se_close_top = 0;
	/* Typedef / SUE walks call this on the dimension `[` itself. Classify
	 * that outer bracket as a declarator dim — otherwise
	 * `typedef int T[sizeof(int orelse 0)]` and `typedef int T[n orelse 1]`
	 * never hit p1d_classify_decl_dims and leak orelse. */
	if (has_orelse && pparse_match_ch(tok, '[') && !(tok->flags & PPARSE_TF_C23_ATTR) &&
	    !(pparse_ann(tok) & P1_OE_BRACKET))
		p1d_classify_bracket_orelse_ex(tok,
					      cur_sid,
					      cur_func,
					      /*hard_ctx=*/true,
					      /*allow_se_hoist=*/cur_func >= 0);
	PPARSE_FOR_RANGE(inner, pparse_next(_pc, tok), group_end) {
		if (inner->flags & PPARSE_TF_OPEN) {
			if (has_orelse && pparse_match_ch(inner, '[') &&
			    !(inner->flags & PPARSE_TF_C23_ATTR) && !(pparse_ann(inner) & P1_OE_BRACKET))
				p1d_classify_bracket_orelse_ex(
				    inner, cur_sid, cur_func, cur_func >= 0, /*allow_se_hoist=*/false);
			if (pparse_is_stmt_expr_open(inner)) {
				se_depth++;
				PParseToken *se_brace = pparse_skip_noise(_pc, pparse_next(_pc, inner));
				PParseToken *brace_close = se_brace ? pparse_pair_known(se_brace) : NULL;
				if (se_close_top < 64 && brace_close)
					se_close_stack[se_close_top++] = brace_close;
			}
		}
		if (has_orelse && (inner->tag & PPARSE_TT_TYPEOF))
			p1d_annotate_typeof_orelse(inner, cur_sid, cur_func, cur_func >= 0);
		if (inner->flags & PPARSE_TF_CLOSE) {
			if (se_close_top > 0 && inner == se_close_stack[se_close_top - 1]) {
				se_close_top--;
				se_depth--;
			}
			prev_inner = inner;
			continue;
		}
		if (pparse_is_enum_kw(inner)) {
			PParseToken *brace = pparse_find_struct_body_brace(inner);
			if (brace) {
				pparse_enum_constants(brace, brace_depth);
				p1_check_enum_body_defer_shadow(brace, cur_sid, cur_func);
			}
		}
		if (!stmt_expr_open && pparse_is_stmt_expr_open(inner)) stmt_expr_open = inner;
		if (se_depth == 0 && cur_func >= 0 && prev_saved &&
		    (prev_saved->tag & (PPARSE_TT_IF | PPARSE_TT_LOOP | PPARSE_TT_SWITCH)) && (inner->tag & PPARSE_TT_DEFER) &&
		    !pparse_token_has_binding(inner) && !pparse_is_known_function_call(inner) &&
		    !(prev_inner && (prev_inner->tag & PPARSE_TT_MEMBER)))
			pparse_error_tok(inner, PPARSE_ERR_DEFER_CTRL_PAREN);
		if (se_depth == 0 && prev_saved && (prev_saved->tag & (PPARSE_TT_IF | PPARSE_TT_LOOP | PPARSE_TT_SWITCH)) &&
		    (inner->tag & PPARSE_TT_ORELSE) && !pparse_token_has_binding(inner) &&
		    !(prev_inner && (prev_inner->tag & PPARSE_TT_MEMBER)) &&
		    !pparse_is_known_function_call(inner) && !pparse_orelse_is_label_or_goto_target(inner, prev_inner))
			pparse_error_tok(inner,
				  "'orelse' cannot be used inside control statement "
				  "condition parentheses");
		prev_inner = inner;
	}
	return stmt_expr_open;
}

// Read-only probe: does NOT advance the caller's token pointer.
static bool p1_scope_in_raw_block(uint16_t sid) {
	PPARSE_CTX();
	if (!pparse_p1_has_raw_block) return false;
	for (; sid != 0 && sid < pparse_scope_tree_count; sid = pparse_scope_tree[sid].parent_id) {
		uint32_t oi = pparse_scope_tree[sid].open_tok_idx;
		if (oi > 0 && oi < pparse_token_count && (pparse_token_pool[oi].ann & P1_RAW_BLOCK)) return true;
	}
	return false;
}

static char *p1d_find_local_label(P1ScanState *s, char *name, int len, uint16_t cur_sid, int *out_len) {
	for (int i = s->local_label_count - 1; i >= 0; i--) {
		if (s->local_labels[i].len != len) continue;
		if (!prism_memeq_runtime_sized(s->local_labels[i].name, name, (uint32_t)len)) continue;
		if (pparse_scope_is_ancestor_or_self(s->local_labels[i].scope_id, cur_sid)) {
			*out_len = s->local_labels[i].mangled_len;
			return s->local_labels[i].mangled;
		}
	}
	return NULL;
}
static void p1d_set_label_name(P1FuncEntry *e, P1ScanState *ps, PParseToken *name, uint16_t cur_sid) {
	PPARSE_CTX();
	int ml;
	char *mangled = p1d_find_local_label(ps, pparse_loc(_pc, name), name->len, cur_sid, &ml);
	if (mangled) {
		e->label.name = mangled;
		e->label.len = ml;
	} else {
		e->label.name = pparse_loc(_pc, name);
		e->label.len = name->len;
	}
}

static void p1d_record_goto(P1ScanState *ps, PParseToken *tok, uint16_t cur_sid, int p1d_cur_func) {
	PPARSE_CTX();
	if (!(tok->tag & PPARSE_TT_GOTO) || pparse_is_known_typedef(tok) || !pparse_next(_pc, tok)) return;
	PParseToken *target = pparse_skip_noise(_pc, pparse_next(_pc, tok));
	tok->parse_data = 0; /* CFG writes the number of lexical blocks exited. */
	if (pparse_is_identifier_like(target)) {
		P1FuncEntry *e = p1_alloc(P1K_GOTO, cur_sid, tok);
		p1d_set_label_name(e, ps, target, cur_sid);
	} else if (pparse_match_ch(target, '*'))
		func_meta[p1d_cur_func].has_computed_goto = true;
}

static void p1d_register_enum_at(PParseToken *tok, int brace_depth, uint16_t sid, int p1d_cur_func) {
	if (!pparse_is_enum_kw(tok)) return;
	PParseToken *brace = pparse_find_struct_body_brace(tok);
	if (!brace) return;
	pparse_enum_constants(brace, brace_depth);
	p1_check_enum_body_defer_shadow(brace, sid, p1d_cur_func);
}

// Handle '{' in prescan: scope tracking, Phase 1E return type capture,
static void p1d_ensure_switch_cap(P1ScanState *s) {
	PPARSE_CTX();
	if (s->p1d_switch_top < s->p1d_switch_cap) return;
	int old = s->p1d_switch_cap;
	size_t nc = pparse_vec_grow_cap((size_t)old, (size_t)s->p1d_switch_top + 1, 64);
	s->p1d_switch_stack = pparse_arena_realloc(
	    &_pc->main_arena, s->p1d_switch_stack, old * sizeof(uint16_t), nc * sizeof(uint16_t));
	s->p1d_switch_end = pparse_arena_realloc(
	    &_pc->main_arena, s->p1d_switch_end, old * sizeof(uint32_t), nc * sizeof(uint32_t));
	s->p1d_switch_cap = (int)nc;
}

static PParseTypedefEntry *pparse_register_struct_tag(PParseToken *tok,
					   int scope_depth,
					   bool has_vla,
					   bool has_volatile_member) {
	PParseTypedefEntry *entry =
	    pparse_typedef_add_entry(tok, scope_depth, PPARSE_TDK_STRUCT_TAG, has_vla, false);
	if (!entry) entry = pparse_binding_entry(tok, false);
	pparse_binding_apply_traits(entry,
			       PPARSE_BIND_AGGREGATE |
				   (has_volatile_member ? PPARSE_BIND_VOLATILE_MEMBER : 0));
	return entry;
}
static bool pparse_function_decl_returns_aggregate(PParseToken *function_name) {
	PPARSE_CTX();
	bool saw_aggregate = false, saw_pointer = false, saw_enum = false;
	for (PParseToken *t = pparse_walk_back(pparse_idx(_pc, function_name), PPARSE_WB_PAST_NOISE); t;
	     t = pparse_walk_back(pparse_idx(_pc, t), PPARSE_WB_PAST_NOISE)) {
		if (pparse_match_ch(t, ';') || pparse_match_ch(t, '{') || pparse_match_ch(t, '}') || pparse_match_ch(t, ')')) break;
		if (pparse_match_ch(t, '*')) {
			saw_pointer = true;
			continue;
		}
		if (t->tag & PPARSE_TT_SUE) {
			if (pparse_is_enum_kw(t))
				saw_enum = true;
			else
				saw_aggregate = true;
			continue;
		}
		if (pparse_typedef_flags(t) & PPARSE_TDF_AGGREGATE) {
			saw_aggregate = true;
			continue;
		}
		if (t->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_TYPEOF | PPARSE_TT_ATTR)) continue;
		if (pparse_is_known_typedef(t)) continue;
		if (t->kind == PPARSE_TK_IDENT || t->kind == PPARSE_TK_PUNCT) break;
	}
	return saw_aggregate & !saw_pointer & !saw_enum;
}
static void __attribute__((noinline)) p1d_validate_bare_orelse(PParseToken *tok, PParseToken *bare_oe) {
	PPARSE_CTX();
	pparse_analysis_add(tok, PPARSE_AR_BARE_ORELSE)->as.token_idx = pparse_idx(_pc, bare_oe);
	PParseExprTopLevel top = pparse_scan_expr_top_level(tok, bare_oe);
	PParseToken *scan_start = top.last_comma ? pparse_next(_pc, top.last_comma) : tok;
	PParseToken *eq_tok = top.segment_assignment;
	if (eq_tok && !pparse_match_ch(eq_tok, '='))
		pparse_error_tok(eq_tok,
			  "bare assignment with 'orelse' cannot use compound operators "
			  "(e.g. +=, -=); use a plain '=' assignment");
	if (tok == bare_oe) pparse_error_tok(tok, PPARSE_ERR_ORELSE_EXPECT_EXPR);
	if (eq_tok && pparse_skip_noise(_pc, pparse_next(_pc, eq_tok)) == bare_oe)
		pparse_error_tok(bare_oe, PPARSE_ERR_ORELSE_EXPECT_EXPR);
	PParseToken *after_oe = pparse_next(_pc, bare_oe);
	if (orelse_next_is_empty_action(after_oe))
		pparse_error_tok(after_oe, PPARSE_ERR_ORELSE_EXPECT_STMT);
	if (!eq_tok && is_orelse_value_fallback(after_oe))
		pparse_error_tok(after_oe,
			  "orelse fallback requires an assignment target "
			  "(use a declaration)");
	if (eq_tok) {
		if (scan_start != eq_tok && pparse_match_ch(scan_start, '(')) {
			PParseToken *inner_first = pparse_next(_pc, scan_start);
			PParseToken *pclose = pparse_pair_known(scan_start);
			if (pparse_next(_pc, pclose) != eq_tok &&
			    (inner_first->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF) ||
			     pparse_is_known_typedef(inner_first)))
				pparse_error_tok(scan_start,
					  "bare orelse assignment target is a cast expression "
					  "(not a modifiable lvalue)");
		}
		if (p1d_lhs_is_const_shadow(scan_start, eq_tok))
			pparse_error_tok(eq_tok, "orelse fallback cannot reassign a const-qualified variable");
	}

	bool value_fallback = eq_tok && is_orelse_value_fallback(after_oe);
	bool fb_cl_comma = false, fb_cl_semi = false;
	if (value_fallback) {
		reject_orelse_side_effects(scan_start, eq_tok, PPARSE_OE_SE_ORELSE_FALLBACK_ON_ASSIGNM_MAIN);
		{
			int fd = 0;
			bool after_comma = false;
			PParseToken *prev_cl = NULL;
			PPARSE_FOR_TAIL(s, after_oe) {
				if (pparse_match_ch(s, '{') && (fd == 0 || (prev_cl && pparse_match_ch(prev_cl, ')')))) {
					fb_cl_semi = true;
					fb_cl_comma = !after_comma;
					break;
				}
				if (s->flags & PPARSE_TF_OPEN) fd++;
				else if (s->flags & PPARSE_TF_CLOSE) {
					if (fd == 0) break;
					fd--;
				} else if (fd == 0 && pparse_match_ch(s, ';'))
					break;
				else if (fd == 0 && pparse_match_ch(s, ','))
					after_comma = true;
				prev_cl = s;
			}
		}
		if (fb_cl_comma)
			reject_orelse_side_effects(scan_start, eq_tok, PPARSE_OE_SE_ORELSE_COMPOUND_LITERAL_FA_MAIN);
	}

	bool lhs_indirect = value_fallback && bare_lhs_has_indirection(scan_start, eq_tok);
	if (lhs_indirect) {
			PParseToken *rhs_start = pparse_next(_pc, eq_tok);
			if (!pparse_is_strict_bare_function_call(rhs_start, bare_oe))
				reject_orelse_side_effects(rhs_start, bare_oe, PPARSE_OE_SE_BARE_ORELSE_WITH_INDIRECTI_TYPEOF);
	}

	if (eq_tok) {
		PParseToken *ppc = pparse_span_find_pp_conditional(scan_start, NULL, true);
		if (ppc) pparse_error_tok(bare_oe, PPARSE_ERR_BARE_ORELSE_SPANS_PP);
	}
	if (eq_tok) {
		PParseToken *rhs = pparse_next(_pc, eq_tok);
		if (pparse_expr_is_aggregate_value(rhs, bare_oe) || pparse_expr_is_aggregate_value(scan_start, eq_tok))
			pparse_error_tok(bare_oe, PPARSE_ERR_ORELSE_STRUCT_VALUE);
	}
	if (value_fallback) {
		uint32_t recipe = P1_OE_BARE_RECIPE |
				  fb_cl_comma * P1_OE_FALLBACK_CL_COMMA |
				  fb_cl_semi * P1_OE_FALLBACK_CL_SEMI |
				  lhs_indirect * P1_OE_LHS_INDIRECT |
				  bare_lhs_has_indirection(tok, eq_tok) * P1_OE_FULL_LHS_INDIRECT;
		PPARSE_FOR_RANGE(s, pparse_next(_pc, eq_tok), bare_oe)
			if (s->tag & PPARSE_TT_MEMBER) {
				recipe |= P1_OE_RHS_MEMBER;
				break;
			}
		bare_oe->pair_idx = pparse_idx(_pc, eq_tok);
		eq_tok->pair_idx = top.last_comma ? pparse_idx(_pc, top.last_comma) : 0;
		pparse_ann(bare_oe) |= recipe;
	}
	pparse_ann(bare_oe) |= P1_IS_ORELSE_KW;
	p1d_reject_orelse_chain_after_ctrl(bare_oe);
	{
		PParseToken *prev = bare_oe;
		PParseToken *prev_oe = bare_oe; /* most recently confirmed chain orelse */
		int ternary = 0;
		PPARSE_FOR_TAIL(s, pparse_next(_pc, bare_oe)) {
			if (s->flags & PPARSE_TF_OPEN) {
				s = prev = pparse_pair_known(s);
				continue;
			}
			if ((s->flags & PPARSE_TF_CLOSE) || pparse_match_ch(s, ';')) break;
			if (pparse_ternary_depth_step(s, &ternary)) {
				prev = s;
				continue;
			}
			if (ternary == 0 && orelse_kw_at_bare(s, prev)) {
				/* `s` existing proves the link [prev_oe+1, s) is a mid-chain
				 * fallback, not the tail: emit_bare_orelse_impl gives every
				 * non-tail link its own `typeof(...) __prism_oe_N` temp when
				 * the LHS has indirection (typeof(RHS) substitutes for
				 * typeof(LHS) there so a VM-typed RHS can't turn typeof(LHS)
				 * itself into a runtime-evaluated operand), which physically
				 * duplicates that link's tokens exactly like the first
				 * link's RHS. Only the first link's range was checked here
				 * before, so a side effect placed in a middle link of a
				 * 3+-way chain (e.g. `*p = 0 orelse vla_ptrs[f()] orelse 0;`
				 * with vla_ptrs an array of pointer-to-VLA) evaluated f()
				 * twice at runtime with no rejection. */
				if (lhs_indirect) {
					PParseToken *mid_start = pparse_next(_pc, prev_oe);
					if (!pparse_is_strict_bare_function_call(mid_start, s))
						reject_orelse_side_effects(mid_start, s, PPARSE_OE_SE_BARE_ORELSE_WITH_INDIRECTI_TYPEOF);
				}
				pparse_ann(s) |= P1_IS_ORELSE_KW;
				/* Chain tail with empty fallback: `… orelse;` / `… orelse,`
				 * would lower to an empty expression. */
				PParseToken *nx = pparse_next(_pc, s);
				if (orelse_next_is_empty_action(nx))
					pparse_error_tok(nx, PPARSE_ERR_ORELSE_EXPECT_STMT);
				p1d_reject_orelse_chain_after_ctrl(s);
				prev_oe = s;
			}
			prev = s;
		}
	}
}
static void p1d_reject_decl_orelse_pp(PParseToken *eq, PParseToken *orelse) {
	PPARSE_CTX();
	if (pparse_span_find_pp_conditional(pparse_next(_pc, eq), orelse, false))
		pparse_error_tok(orelse,
				  "'orelse' in a declaration initializer spans preprocessor "
				  "conditionals; keep the orelse within a single #if branch");
}

static PParseToken *p1d_scan_init_orelse(PParseToken *t, bool *out_has_orelse, PParseToken **out_first_orelse) {
	PPARSE_CTX();
	const bool has_flow_ext = pparse_feat(PPARSE_F_ORELSE | PPARSE_F_DEFER);
	PParseToken *prev_init_tok = NULL;
	bool init_is_first = true;
	int init_td = 0;
	PParseToken *eq = t; // chain predecessor of the first init token (for wrap-paren strip)
	t = pparse_next(_pc, t); // skip '='
	while (t && !pparse_match_ch(t, ';') && t->kind != PPARSE_TK_EOF) {
		if (init_td == 0 && pparse_match_ch(t, ',')) {
			if (!*out_has_orelse || pparse_comma_starts_declarator(t)) break;
			prev_init_tok = t;
			init_is_first = false;
			t = pparse_next(_pc, t);
			continue;
		}
		if (pparse_match_ch(t, '?')) {
			init_td++;
			init_is_first = false;
			prev_init_tok = t;
			t = pparse_next(_pc, t);
			continue;
		}
		if (pparse_match_ch(t, ':') && init_td > 0) {
			init_td--;
			init_is_first = false;
			prev_init_tok = t;
			t = pparse_next(_pc, t);
			continue;
		}
		// Phase 1G: mark orelse in decl initializer
		int oe_ann = p1d_try_annotate_init_orelse(t,
							  prev_init_tok,
							  pparse_is_expr_ending_brace,
							  true,
							  init_td,
							  out_has_orelse,
							  out_first_orelse);
		/* oe_ann==2 is the operator; reject it without a left operand. */
		if (oe_ann == 2 && init_is_first)
			pparse_error_tok(t, PPARSE_ERR_ORELSE_EXPECT_EXPR);
		if (oe_ann == 1) {
			/* A decl-init orelse cannot span conditional-preprocessor arms. */
			if (out_first_orelse && *out_first_orelse == t) p1d_reject_decl_orelse_pp(eq, t);
			prev_init_tok = t;
			t = pparse_next(_pc, t);
			init_is_first = false;
			continue;
		}
		/* Top-level decl-init orelse must be followed by an action or
		 * value (mirrors Pass 2 require_orelse_action: not ';' or ','). */
		if (oe_ann == 2) {
			PParseToken *nx = pparse_next(_pc, t);
			if (orelse_next_is_empty_action(nx))
				pparse_error_tok(nx, PPARSE_ERR_ORELSE_EXPECT_STMT);
			if (out_first_orelse && *out_first_orelse == t) p1d_reject_decl_orelse_pp(eq, t);
		}
		if (t->flags & PPARSE_TF_OPEN) {
			PParseToken *m = pparse_pair_known(t);
			if (pparse_match_ch(t, '(') && !pparse_is_stmt_expr_open(t)) {
				PParseToken *am = pparse_next(_pc, m);
				if (init_is_first &&
				    (pparse_match_ch(am, ',') || pparse_match_ch(am, ';') || am->kind == PPARSE_TK_EOF)) {
					PParseToken *prev_inner = NULL;
					bool p1d_inner_d0_comma = false;
					for (PParseToken *inner = pparse_next(_pc, t); inner != m;
					     inner = pparse_next(_pc, inner)) {
						if (pparse_match_ch(inner, ',')) p1d_inner_d0_comma = true;
						if (p1d_try_annotate_init_orelse(inner,
										 prev_inner,
										 pparse_is_expr_ending,
										 false,
										 0,
										 out_has_orelse,
										 out_first_orelse) == 1) {
							prev_inner = inner;
							continue;
						}
						if (inner->flags & PPARSE_TF_OPEN) {
							if (has_flow_ext &&
							    pparse_match_ch(inner, '(') &&
							    !(prev_inner && (prev_inner->tag & PPARSE_TT_TYPEOF)))
								check_orelse_in_parens(inner);
							inner = pparse_pair_known(inner);
							prev_inner = inner;
							continue;
						}
						prev_inner = inner;
					}
					if (p1d_inner_d0_comma && *out_has_orelse) {
						PPARSE_FOR_RANGE(u, pparse_next(_pc, t), m) {
							pparse_ann(u) &=
							    (uint16_t)~(P1_OE_DECL_INIT | P1_IS_ORELSE_KW);
							if (u->flags & PPARSE_TF_OPEN) {
								u = pparse_pair_known(u);
								continue;
							}
						}
						*out_has_orelse = false;
						*out_first_orelse = NULL;
					} else if (*out_has_orelse) {
						/* Macro-hygiene parens wrapping the whole init:
						 * unlink `(` and `)` from the chain here, once —
						 * Pass 2 emits from the stripped stream and never
						 * mutates tokens. PPARSE_TF_OPEN is cleared so index-based
						 * walkers do not treat the orphan as a group. */
						eq->parse_data = pparse_idx(_pc, t) + 1;
						eq->flags |= PPARSE_TF_LINK_JUMP;
						t->flags &= ~PPARSE_TF_OPEN;
						PParseToken *before_close = t;
						for (PParseToken *u = pparse_next(_pc, t); u != m;) {
							if (u->flags & PPARSE_TF_OPEN) {
								before_close = pparse_pair_known(u);
								u = pparse_next(_pc, before_close);
								continue;
							}
							before_close = u;
							u = pparse_next(_pc, u);
						}
						before_close->parse_data = pparse_idx(_pc, m) + 1;
						before_close->flags |= PPARSE_TF_LINK_JUMP;
					}
			} else if (has_flow_ext &&
					   !(prev_init_tok && (prev_init_tok->tag & PPARSE_TT_TYPEOF)))
					check_orelse_in_parens(t);
			}
			prev_init_tok = m;
			t = pparse_next(_pc, m);
			init_is_first = false;
			continue;
		}
		prev_init_tok = t;
		t = pparse_next(_pc, t);
		init_is_first = false;
	}
	return t;
}
static void p1d_validate_decl_orelse(PParseToken *var_name,
				     PParseTypeSpec *type,
				     PParseDecl *decl,
				     PParseToken *first_orelse,
				     bool saw_static,
				     int brace_depth) {
	PPARSE_CTX();
	if (brace_depth == 0) pparse_error_tok(var_name, PPARSE_ERR_ORELSE_FILE_SCOPE);
	if (saw_static || type->has_static || type->has_extern)
		pparse_error_tok(var_name, PPARSE_ERR_ORELSE_STATIC_THREAD);
	if (type->has_constexpr)
		pparse_error_tok(first_orelse ? first_orelse : var_name, PPARSE_ERR_ORELSE_CONSTEXPR);
	bool base_is_array = (decl->is_array && (!decl->paren_pointer || decl->paren_array)) ||
			     (type->is_array && !decl->is_pointer && !decl->paren_pointer);
	if (base_is_array)
		pparse_error_tok(var_name, PPARSE_ERR_ORELSE_ARRAY_NEVER_NULL, var_name->len, pparse_loc(_pc, var_name));
	if (type->is_struct && !type->is_enum && !decl->is_pointer && !decl->is_array)
		pparse_error_tok(var_name, PPARSE_ERR_ORELSE_STRUCT_VALUE);
	if (first_orelse && (type->is_vla || decl->is_vla || type->type_vm)) {
			if (is_orelse_value_fallback(pparse_next(_pc, first_orelse)) &&
			    (pparse_decl_const_flags(type, decl) & PPARSE_DECL_CONST_EFFECTIVE))
			pparse_error_tok(first_orelse, PPARSE_ERR_ORELSE_CONST_VM);
	}

	// Reject GNU statement expressions in orelse fallback values
	if (first_orelse) {
		if (is_orelse_value_fallback(pparse_next(_pc, first_orelse))) {
			PParseToken *fallback = pparse_next(_pc, first_orelse);
			PParseToken *fallback_end = pparse_skip_to_set(
			    fallback, NULL, pparse_CH(';') | pparse_CH(','));
			PPARSE_FOR_RANGE(s, fallback, fallback_end)
				if (pparse_is_stmt_expr_open(s))
					pparse_error_tok(s,
					  "GNU statement expressions in orelse "
					  "fallback values are not supported; "
					  "use 'orelse { ... }' block form instead");
		}
		PParseToken *prev = NULL;
		for (PParseToken *s = first_orelse; s && s->kind != PPARSE_TK_EOF && !pparse_match_ch(s, ';') && !pparse_match_ch(s, ',');
		     s = pparse_next(_pc, s)) {
			if (s->flags & PPARSE_TF_OPEN) {
				s = prev = pparse_pair_known(s);
				continue;
			}
			if ((pparse_ann(s) & P1_IS_ORELSE_KW) || (prev && orelse_kw_at_bare(s, prev)))
				p1d_reject_orelse_chain_after_ctrl(s);
			prev = s;
		}
	}
}
// break with anonymous structs or variably-modified type specifiers.
static void p1d_check_multi_decl_constraints(PParseToken *t,
					     PParseTypeSpec *type,
					     bool any_would_memset,
					     bool vm_type,
					     bool current_decl_has_orelse) {
	PPARSE_CTX();
	PParseToken *next_t = pparse_next(_pc, t);
	bool nr = false;
	next_t = pparse_p1_skip_decl_raw(next_t, &nr);
	PParseDecl nd = pparse_declarator(next_t);
	if (!nd.var_name || !nd.end) return;
	bool split = (current_decl_has_orelse && pparse_feat(PPARSE_F_ORELSE)) ||
		     (any_would_memset && (pparse_match_ch(nd.end, '=') || nd.is_vla)) ||
		     p1d_classify_decl_dims(next_t, nd.end, 0, -1, false, true);
	if (!split) return;
	pparse_ann(t) |= P1_DECL_SPLIT;
	PParseToken *after_sue =
	    type->sue_kw ? pparse_skip_noise(_pc, pparse_next(_pc, type->sue_kw)) : NULL;
	if (type->is_struct && !type->is_enum && after_sue && pparse_match_ch(after_sue, '{'))
		pparse_error_tok(next_t, PPARSE_ERR_BRACKET_OE_ANON_AGG);

	if (vm_type) pparse_error_tok(next_t, PPARSE_ERR_MULTIDECL_VM);
}

static void p1d_reject_bracket_orelse_dims(PParseToken *start,
					   PParseToken *end,
					   const char *message) {
	PPARSE_CTX();
	PPARSE_FOR_RANGE(b, start, end) {
		if (!pparse_match_ch(b, '[') || !(pparse_ann(b) & P1_OE_BRACKET)) continue;
		PParseToken *close = pparse_pair_known(b);
		PPARSE_FOR_RANGE(s, pparse_next(_pc, b), close)
			if (pparse_ann(s) & P1_IS_ORELSE_KW) pparse_error_tok(s, message);
	}
}

static void p1d_probe_declaration(PParseToken *tok,
				  PParseToken *decl_start,
				  uint16_t cur_sid,
				  int brace_depth,
				  int cur_func,
				  bool *saw_raw,
				  bool saw_static,
				  bool decl_has_leading_attr,
				  bool ctrl_pending,
				  uint32_t **skip_cache) {
	PPARSE_CTX();
	const bool has_defer = pparse_feat(PPARSE_F_DEFER);
	const bool has_orelse = pparse_feat(PPARSE_F_ORELSE);
	const bool has_zeroinit = pparse_feat(PPARSE_F_ZEROINIT);
	const bool has_auto_static = pparse_feat(PPARSE_F_AUTO_STATIC);
	pparse_ASSERT_NOT_NOISE(tok);
	if (!(tok->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT)) && !pparse_is_known_typedef(tok))
		return;
	if (p1_scope_in_raw_block(cur_sid)) *saw_raw = true;
	PParseToken *type_tok = decl_start ? decl_start : tok;
	PParseTypeSpec type = pparse_type_specifier(type_tok);
	if (type.has_raw) *saw_raw = true;
	if (type.saw_type && has_defer && cur_func >= 0) {
		int po = 0, bo = 0;
		PParseToken *sprev = NULL;
		PPARSE_FOR_TAIL(s, tok) {
			if (s->flags & PPARSE_TF_OPEN) {
				if (pparse_match_ch(s, '{')) bo++;
				else
					po++;
			} else if (s->flags & PPARSE_TF_CLOSE) {
				if (pparse_match_ch(s, '}')) {
					if (bo == 0) break;
					bo--;
				} else {
					if (po == 0 && bo == 0) break;
					if (po > 0) po--;
				}
			} else if (po == 0 && bo == 0 && (pparse_match_ch(s, ';') || pparse_match_ch(s, '{')))
				break;
			if (bo == 0 && (po > 0 || s != tok) && (s->tag & PPARSE_TT_DEFER) &&
			    !pparse_is_known_typedef(s) && !(sprev && (sprev->tag & (PPARSE_TT_SUE | PPARSE_TT_MEMBER)))) {
				PParseToken *nx = pparse_skip_noise(_pc, pparse_next(_pc, s));
				if (pparse_is_identifier_like(nx) || pparse_match_ch(nx, '{'))
					pparse_error_tok(s, PPARSE_ERR_DEFER_EXPR_CTX);
			}
			if (s->kind != PPARSE_TK_PREP_DIR) sprev = s;
		}
	}
	if (!type.saw_type && type.end && (type.end->flags & PPARSE_TF_RAW) && !pparse_is_known_typedef(type.end)) {
		PParseToken *after_raw = pparse_skip_noise(_pc, pparse_next(_pc, type.end));
		if (after_raw && pparse_is_raw_declaration_context(type.end, after_raw)) {
			*saw_raw = true;
			type_tok = after_raw;
			type = pparse_type_specifier(after_raw);
		}
	}
	if (!type.saw_type) return;
	bool annotated = false;
	uint32_t braceless_close_idx = 0;
	if (ctrl_pending) {
		PParseToken *stmt_end =
		    pparse_skip_one_stmt_impl(type_tok, p1d_ensure_skip_cache(skip_cache));
		if (stmt_end) braceless_close_idx = pparse_idx(_pc, stmt_end);
	}
	PPARSE_TD_SCOPE_SAVE();
	if (braceless_close_idx > 0) pparse_td_scope_close = braceless_close_idx;
	PParseToken *t = type.end;
	bool vm_type = (type.has_typeof || type.has_atomic) && (type.is_vla || type.type_vm);
	bool any_would_memset = false;
	bool type_has_bracket_orelse =
	    p1d_classify_decl_dims(type_tok, type.end, cur_sid, cur_func, true, false);
	while (!pparse_match_ch(t, ';') && !pparse_match_ch(t, '{') && t->kind != PPARSE_TK_EOF) {
		bool decl_raw = *saw_raw;
		t = pparse_p1_skip_decl_raw(t, &decl_raw);
		PParseDecl decl = pparse_declarator(t);
		if (!decl.var_name || !decl.end) {
			// Detect GNU nested function definitions inside outer
			if (cur_func >= 0 && brace_depth > 0 && (has_defer || has_orelse) &&
			    decl.var_name && !saw_static) {
				PParseToken *p = pparse_skip_noise(_pc, pparse_next(_pc, decl.var_name));
				if (pparse_match_ch(p, '(')) {
					PParseToken *param_close = pparse_pair_known(p);
					PParseToken *a = pparse_next(_pc, param_close);
					while (a->tag & (PPARSE_TT_ATTR | PPARSE_TT_ASM)) {
						a = (a->tag & PPARSE_TT_ASM) ? pparse_next(_pc, a) : pparse_skip_noise(_pc, a);
						if (pparse_match_ch(a, '('))
							a = pparse_next(_pc, pparse_pair_known(a));
					}
					bool nested = pparse_match_ch(a, '{');
					PParseToken *body_open = nested ? a : NULL;
					if (!nested && a) {
						PParseToken *b = a;
						while (b->kind != PPARSE_TK_EOF && !pparse_match_ch(b, '{') &&
						       !pparse_match_ch(b, '}'))
							b = (b->flags & PPARSE_TF_OPEN)
								? pparse_next(_pc, pparse_pair_known(b))
								: pparse_next(_pc, b);
						nested = pparse_match_ch(b, '{') &&
							 pparse_is_knr_params(pparse_next(_pc, param_close), b);
						if (nested) body_open = b;
					}
					if (nested) {
						bool outer_uses_defer = false;
						PParseToken *fn_open = func_meta[cur_func].body_open;
						PParseToken *fn_close = pparse_pair_known(fn_open);
						{
							PParseToken *prev_s = NULL;
							for (PParseToken *s = pparse_next(_pc, fn_open);
							     s != fn_close;
							     prev_s = s, s = pparse_next(_pc, s)) {
								/* Pass prev so a nested function *named*
								 * defer / a param named defer is not
								 * mistaken for a defer statement. */
								if (is_defer_kw(s, prev_s)) {
									outer_uses_defer = true;
									break;
								}
							}
						}
						/* Nested bodies are passed through verbatim — orelse/
						 * defer inside them would misscompile at the backend. */
						bool nested_uses_prism = false;
						PParseToken *nclose = pparse_pair_known(body_open);
						{
							PParseToken *prev_n = NULL;
							for (PParseToken *s = pparse_next(_pc, body_open);
							     s != nclose;
							     prev_n = s, s = pparse_next(_pc, s)) {
								if (is_defer_kw(s, prev_n) ||
								    (pparse_is_orelse_kw_shadow(s) &&
								     pparse_orelse_shadow_is_kw(prev_n))) {
									nested_uses_prism = true;
									break;
								}
							}
						}
						if (outer_uses_defer || nested_uses_prism)
							pparse_error_tok(decl.var_name,
								  "nested function definitions cannot use "
								  "defer/orelse (and are unsupported inside "
								  "functions using defer): move the function "
								  "outside or use a function pointer");
					}
				}
			}
			break;
		}
		if (pparse_match_ch(decl.end, '(') && brace_depth == 0) break; // func def
		if (!pparse_match_set(decl.end,
				      pparse_CH('=') | pparse_CH(',') | pparse_CH(';') | pparse_CH('[') |
					  pparse_CH('(') | pparse_CH(')') | pparse_CH(':')) &&
		    !pparse_match_ch(decl.end, '{'))
			break;
		if (!annotated && brace_depth > 0) {
			pparse_ann(type_tok) |= P1_IS_DECL;
			pparse_ann(tok) |= P1_IS_DECL;
			annotated = true;
		}
		/* Declarator dimensions may hoist when in a function. */
		decl.has_bracket_orelse =
		    p1d_classify_decl_dims(t, decl.end, cur_sid, cur_func, true, cur_func >= 0);
		/* Function parameter dims never allocate VLAs; static/extern/TLS/
		 * constexpr dims must be ICEs (C11 §6.7.6.2 /
		 * C23 constexpr). Bracket orelse lowering inserts a runtime
		 * temporary, turning even `static int a[0 orelse 1]` / 
		 * `constexpr int a[0 orelse 1]` into an illegal non-ICE dim. */
		bool proto_dims = decl.has_bracket_orelse & (decl.is_func_ptr | decl.is_func_decl);
		bool ice_dims = saw_static | type.has_static | type.has_extern |
				type.has_thread_local | type.has_constexpr;
		if (proto_dims)
			p1d_reject_bracket_orelse_dims(t, decl.end, PPARSE_ERR_ORELSE_PROTO_DIM);
		if (ice_dims & (type_has_bracket_orelse | decl.has_bracket_orelse))
			p1d_reject_bracket_orelse_dims(
			    type_tok,
			    decl.end,
			    "orelse inside array dimension of a "
			    "static/extern/_Thread_local/constexpr "
			    "declaration is not allowed (dimension "
			    "must be an integer constant expression; "
			    "orelse lowering introduces a runtime "
			    "temporary)");

		{
			bool has_init = pparse_match_ch(decl.end, '=');
			bool has_explicit_intent =
			    has_init | decl_raw | saw_static | type.is_typedef | type.is_struct |
			    type.is_enum | type.has_static | type.has_extern | type.has_thread_local |
			    type.has_register | type.has_atomic | type.has_constexpr | type.has_alignas;
			if (has_zeroinit && brace_depth > 0 && !has_explicit_intent &&
			    braceless_close_idx == 0 && cur_sid < pparse_scope_tree_count &&
			    pparse_scope_tree[cur_sid].is_switch)
				SAFETY_DIAG(
				    type_tok,
				    "variable declaration directly in switch body without braces "
				    "(zero-init may be skipped by case labels); wrap in braces or use "
				    "'raw'");
		}

		t = decl.end;
		/* Struct/union bitfield: declarator ends at `:`. Keep going so
		 * soft/Prism keyword field names (e.g. `int orelse : 3`) still
		 * get shadow registration above, then skip the width expr. */
		if (pparse_match_ch(t, ':')) {
			t = pparse_next(_pc, t);
			while (t->kind != PPARSE_TK_EOF && !pparse_match_ch(t, ';')) {
				if (t->flags & PPARSE_TF_OPEN) {
					t = pparse_next(_pc, pparse_pair_known(t));
					continue;
				}
				if (pparse_match_ch(t, ',') || pparse_match_ch(t, ';')) break;
				t = pparse_next(_pc, t);
			}
		}
		bool has_init = pparse_match_ch(t, '=');
		bool in_aggregate_body =
		    (cur_sid > 0) & (cur_sid < pparse_scope_tree_count) && pparse_scope_tree[cur_sid].is_struct;
		/* Locals inside a GNU nested function must not land on the outer
		 * function's CFG / cgoto×zeroinit gate — nested bodies are passed
		 * through; only the outer function's own decls matter. */
		bool in_nested_func = false;
		for (uint16_t s = cur_sid; s != 0 && s < pparse_scope_tree_count; s = pparse_scope_tree[s].parent_id) {
			if (pparse_scope_tree[s].is_func_body && pparse_scope_tree[s].parent_id != 0) {
				in_nested_func = true;
				break;
			}
		}
		uint16_t decl_sid = (cur_func >= 0 && !in_nested_func) ? cur_sid : 0;
		bool storage_static = saw_static | type.has_static | type.has_extern;
		unsigned plan_flags = !in_aggregate_body * P1DP_CONST |
				      ((brace_depth > 0) & !in_aggregate_body) *
					  (P1DP_REGISTER | P1DP_REGISTER_VLA);
		uint8_t zero_kind;
		P1FuncEntry *p1e = p1_analyze_decl(type_tok,
					      &type,
					      &decl,
					      decl_raw,
					      brace_depth,
					      true,
					      (brace_depth > 0) & !in_aggregate_body,
					      decl_sid,
					      storage_static,
					      braceless_close_idx,
					      plan_flags,
					      &zero_kind);
		if (p1e && !ctrl_pending)
			p1_check_defer_same_block_shadow(decl.var_name, cur_sid, cur_func);

		bool decl_has_orelse = false;
		if (has_init) {
			PParseToken *first_orelse = NULL;
			t = p1d_scan_init_orelse(t, &decl_has_orelse, &first_orelse);
			if (first_orelse) {
				decl.end->pair_idx = pparse_idx(_pc, first_orelse) + 1;
				pparse_ann(first_orelse) |= P1_OE_DECL_RECIPE;
				first_orelse->pair_idx = pparse_match_ch(t, ',') ? pparse_idx(_pc, t) : 0;
			}
			if (decl_has_orelse && has_orelse)
				p1d_validate_decl_orelse(decl.var_name,
							 &type,
							 &decl,
							 first_orelse,
							 saw_static,
							 brace_depth);
			if (first_orelse && is_orelse_value_fallback(pparse_next(_pc, first_orelse)) &&
			    (pparse_ann(decl.var_name) & P1_DECL_EFFECTIVE_CONST))
				pparse_ann(decl.var_name) |= P1_DECL_CONST_ORELSE;
		}
		if (has_auto_static && brace_depth > 0 && !decl_raw && !saw_static &&
		    !decl_has_leading_attr &&
		    (pparse_decl_const_flags(&type, &decl) & PPARSE_DECL_CONST_EXPLICIT) &&
		    !type.has_volatile && !type.has_hidden_volatile && !type.has_volatile_member && !type.has_static &&
		    !type.has_extern && !type.has_register && !type.has_auto && !type.has_constexpr &&
		    !type.has_thread_local && (decl.is_array || type.is_array) &&
		    (!decl.is_pointer || decl.is_const) && has_init && !decl.is_vla && !type.is_vla &&
		    !decl_has_orelse && !pparse_range_has_attribute(type_tok, type.end, PPARSE_TT_ASM) &&
		    !pparse_range_has_attribute(pparse_next(_pc, decl.var_name), decl.end, 0)) {
			PParseToken *init = pparse_next(_pc, decl.end);
			if ((init->kind == PPARSE_TK_STR && pparse_is_const_literal_initializer(decl.end)) ||
				     (pparse_match_ch(init, '{') &&
				      pparse_match_ch(pparse_next(_pc, pparse_pair_known(init)), ';') &&
				      pparse_is_const_literal_initializer(decl.end)))
				pparse_ann(decl.var_name) |= P1_DECL_AUTO_STATIC;
		}

		any_would_memset |= zero_kind == P1Z_MEMSET;
		if (!pparse_match_ch(t, ',')) break;
		if (brace_depth > 0)
			p1d_check_multi_decl_constraints(t, &type, any_would_memset, vm_type, decl_has_orelse);
		t = pparse_next(_pc, t);
	}
	PPARSE_TD_SCOPE_RESTORE();
}
static void p1_register_knr_param_bindings(PParseToken *rparen, PParseToken *lbrace, uint16_t sid, int brace_depth) {
	PPARSE_CTX();
	if (!rparen || !lbrace || sid == 0 || brace_depth <= 0 || sid >= pparse_scope_tree_count) return;
	if (!pparse_match_ch(rparen, ')')) return;
	PParseToken *id_list_open = pparse_pair_known(rparen);
	if (!pparse_is_knr_params(pparse_next(_pc, id_list_open), lbrace)) return;
	PPARSE_TD_SCOPE_SAVE();
	pparse_td_scope_close = pparse_scope_close(&pparse_scope_tree[sid]);
	for (PParseToken *stmt = pparse_next(_pc, rparen); stmt && stmt != lbrace && stmt->kind != PPARSE_TK_EOF;) {
		stmt = pparse_skip_prep_dirs(stmt);
		stmt = pparse_skip_noise(_pc, stmt);
		if (!stmt || stmt == lbrace) break;
		if (!(stmt->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_STORAGE)) &&
		    !pparse_is_known_typedef(stmt)) {
			stmt = pparse_next(_pc, stmt);
			continue;
		}
		PParseTypeSpec ts = pparse_type_specifier(stmt);
		if (!ts.saw_type) {
			stmt = pparse_next(_pc, stmt);
			continue;
		}
		PParseToken *semi = pparse_skip_to_semicolon(ts.end, NULL);
		if (!semi || semi->kind == PPARSE_TK_EOF || !pparse_match_ch(semi, ';')) break;
		for (PParseToken *seg = ts.end; seg && seg != semi && seg != lbrace;) {
			PParseDecl decl = pparse_declarator(seg);
			if (!decl.var_name || !decl.end) break;
			bool plain = !(decl.is_pointer | decl.is_func_ptr);
			bool is_const = (plain & ts.has_const) | (!plain & decl.is_const);
			bool is_vol = (plain & (ts.has_volatile | ts.has_volatile_member)) |
				      (!plain & decl.is_volatile);
			bool is_atomic = (plain & ts.has_atomic) | (!plain & decl.is_atomic);
			bool is_long_double = plain & !ts.is_ptr & ts.has_long_double;
			unsigned traits = is_const * PPARSE_BIND_CONST |
					  is_vol * PPARSE_BIND_VOLATILE |
					  (plain & ts.has_volatile_member) * PPARSE_BIND_VOLATILE_MEMBER |
					  is_atomic * PPARSE_BIND_ATOMIC |
					  is_long_double * PPARSE_BIND_LONG_DOUBLE;
			pparse_register_parameter_binding(
			    decl.var_name, brace_depth, sid, true, traits);
			/* The identifier-list token already owns the binding used inside
			 * the body; the following declaration repeats its name at a second
			 * token. Apply traits to whichever same-name entry covers this
			 * declaration as well as registering the declaration token. */
			pparse_binding_apply_traits(pparse_typedef_lookup(_pc, decl.var_name), traits);
			if (pparse_declarator_has_vla_after_first_bracket(seg, decl.end, semi, true))
				pparse_register_vla_binding(decl.var_name, brace_depth);
			seg = decl.end;
			if (pparse_match_ch(seg, ',')) seg = pparse_next(_pc, seg);
			else
				break;
		}
		stmt = pparse_next(_pc, semi);
	}
	PPARSE_TD_SCOPE_RESTORE();
}
static void p1d_handle_open_brace(P1ScanState *s) {
	PPARSE_CTX();
	PParseToken *tok = s->tok;
	uint16_t sid = (uint16_t)tok->parse_data;
	bool valid_sid = sid < pparse_scope_tree_count;
	bool function_body = valid_sid && pparse_scope_tree[sid].is_func_body;

	// Phase 1E: function body detection at file scope
	if (s->brace_depth == 0 && function_body) {
		PParseFunctionReturn ret = pparse_function_return(s->file_scope_stmt_start);
		if (ret.kind == PPARSE_FUNC_RETURN_NONE) {
			PParseToken *prev = pparse_walk_back(pparse_idx(_pc, tok) - 1, PPARSE_WB_SKIP_NOISE);
			if (prev && pparse_match_ch(prev, ';')) prev = pparse_p1_knr_find_close_paren(prev);
			if (prev && pparse_match_ch(prev, ')')) {
				PParseToken *open = pparse_pair_known(prev);
				for (uint32_t pi = pparse_idx(_pc, open); pi > 1; pi--) {
					PParseToken *pt = &pparse_token_pool[pi - 1];
					if (pt->kind == PPARSE_TK_PREP_DIR) continue;
					if (pparse_match_ch(pt, '{') || pparse_match_ch(pt, '}') || pparse_match_ch(pt, ';'))
						break;
					s->file_scope_stmt_start = pt;
				}
				ret = pparse_function_return(s->file_scope_stmt_start);
			}
		}
		PPARSE_ARENA_ENSURE_CAP(
		    &_pc->main_arena, _pc->p1_func_meta, func_meta_count, func_meta_cap, 64, FuncMeta);
		FuncMeta *fm = &func_meta[func_meta_count++];
		*fm = (FuncMeta){.body_open = tok,
				 .ret_type_start = ret.kind == PPARSE_FUNC_RETURN_VALUE ? ret.type_start : NULL,
				 .ret_type_end = ret.kind == PPARSE_FUNC_RETURN_VALUE ? ret.type_end : NULL,
				 .ret_type_suffix_start = ret.kind == PPARSE_FUNC_RETURN_VALUE ? ret.suffix_start : NULL,
				 .ret_type_suffix_end = ret.kind == PPARSE_FUNC_RETURN_VALUE ? ret.suffix_end : NULL,
				 .returns_void = ret.kind == PPARSE_FUNC_RETURN_VOID};
		pparse_td_scope_close = pparse_scope_close(&pparse_scope_tree[sid]);
		PParseToken *prev_tok = pparse_walk_back(pparse_idx(_pc, tok) - 1, PPARSE_WB_SKIP_NOISE);
		if (prev_tok && pparse_match_ch(prev_tok, ';')) prev_tok = pparse_p1_knr_find_close_paren(prev_tok);
		if (prev_tok && pparse_match_ch(prev_tok, ')')) {
			p1_register_param_shadows(
			    pparse_pair_known(prev_tok), prev_tok, sid, s->brace_depth + 1, true);
			p1_register_knr_param_bindings(prev_tok, tok, sid, s->brace_depth + 1);
		}
		s->p1d_cur_func = func_meta_count - 1;
		func_meta[s->p1d_cur_func].entry_start = p1_entry_count;
		func_meta[s->p1d_cur_func].entry_count = 0;
		s->p1d_switch_top = 0;
		s->p1d_prev = NULL;
	}

	if (s->p1d_cur_func >= 0 && valid_sid && pparse_scope_tree[sid].is_switch) {
		p1d_ensure_switch_cap(s);
		p1_alloc(P1K_SWITCH, sid, tok);
		s->p1d_switch_stack[s->p1d_switch_top] = sid;
		s->p1d_switch_end[s->p1d_switch_top] = 0; // braced: popped at }
		s->p1d_switch_top++;
	}

	s->brace_depth++;
	s->scope_depth++;
	PPARSE_ARENA_ENSURE_CAP(
	    &_pc->main_arena, s->scope_stack, s->scope_depth + 1, s->scope_cap, 256, uint16_t);
	s->scope_stack[s->scope_depth] = sid;
	if (valid_sid) {
		if (pparse_scope_tree[sid].is_init) s->p1d_init_brace_depth++;
		pparse_td_scope_close = pparse_scope_close(&pparse_scope_tree[sid]);
	}

	p1d_stmt_reset(s, true);
	s->p1d_prev = tok;
	s->tok = pparse_next(_pc, tok);
}
static void p1d_handle_close_brace(P1ScanState *s) {
	PPARSE_CTX();
	PParseToken *tok = s->tok;
	if (s->p1d_cur_func >= 0 && s->p1d_switch_top > 0) {
		uint16_t closing_sid = s->scope_stack[s->scope_depth];
		if (s->p1d_switch_end[s->p1d_switch_top - 1] == 0 &&
		    s->p1d_switch_stack[s->p1d_switch_top - 1] == closing_sid)
			s->p1d_switch_top--;
	}

	bool closing_non_stmt_brace = false;
	if (s->p1d_init_brace_depth > 0) {
		uint16_t csid = s->scope_stack[s->scope_depth];
		if (csid < pparse_scope_tree_count && (pparse_scope_tree[csid].is_struct || pparse_scope_tree[csid].is_init))
			closing_non_stmt_brace = true;
		if (csid < pparse_scope_tree_count && pparse_scope_tree[csid].is_init) s->p1d_init_brace_depth--;
	} else if (s->scope_depth > 0) {
		uint16_t csid = s->scope_stack[s->scope_depth];
		if (csid < pparse_scope_tree_count && pparse_scope_tree[csid].is_struct) closing_non_stmt_brace = true;
	}

	if (s->brace_depth > 0) {
		s->brace_depth--;
		if (s->scope_depth > 0) s->scope_depth--;
	}

	{
		uint16_t cur_sid = s->scope_stack[s->scope_depth];
		if (cur_sid > 0 && cur_sid < pparse_scope_tree_count) {
			pparse_td_scope_close = pparse_scope_close(&pparse_scope_tree[cur_sid]);
		} else {
			pparse_td_scope_close = UINT32_MAX;
		}
	}

	if (s->brace_depth == 0 && s->p1d_cur_func >= 0) {
		func_meta[s->p1d_cur_func].entry_count =
		    p1_entry_count - func_meta[s->p1d_cur_func].entry_start;
		s->p1d_cur_func = -1;
		s->file_scope_stmt_start = pparse_next(_pc, tok);
		s->local_label_count = 0; // Reset local labels for next function
	}

	p1d_stmt_reset(s, !closing_non_stmt_brace);
	s->p1d_prev = tok;
	s->tok = pparse_next(_pc, tok);
}
static PRISM_HOT void p1_full_depth_prescan(PParseToken *tok) {
	PPARSE_CTX();
	const bool has_orelse = pparse_feat(PPARSE_F_ORELSE);
	const bool has_defer = pparse_feat(PPARSE_F_DEFER);
	const bool has_flow_extensions = has_orelse | has_defer;
	P1ScanState _ps = {0};
	P1ScanState *ps = &_ps;
	ps->tok = tok;
	ps->at_stmt_start = true;
	ps->file_scope_stmt_start = tok;
	ps->scope_cap = 64;
	ps->scope_stack = pparse_arena_alloc_uninit(&_pc->main_arena, 64 * sizeof(uint16_t));
	ps->scope_stack[0] = 0; // file scope
	ps->p1d_cur_func = -1;
	ps->p1d_switch_cap = 64;
	ps->p1d_switch_stack = pparse_arena_alloc_uninit(&_pc->main_arena, 64 * sizeof(uint16_t));
	ps->p1d_switch_end = pparse_arena_alloc_uninit(&_pc->main_arena, 64 * sizeof(uint32_t));
	ps->p1d_braceless_next_sid = pparse_scope_tree_count;
	pparse_td_scope_close = UINT32_MAX;
	pparse_p1_has_raw_block = false;

#define CUR_SID() (ps->scope_stack[ps->scope_depth])
#ifdef PRISM_DEBUG
	uint64_t p1_wd_steps = 0;
	const uint64_t p1_wd_budget = 256ull * (uint64_t)pparse_token_count + 65536ull;
#endif
	while (ps->tok->kind != PPARSE_TK_EOF) {
#ifdef PRISM_DEBUG
		if (++p1_wd_steps > p1_wd_budget)
			pparse_error_tok(ps->tok,
				  "internal: Phase 1 progress watchdog tripped "
				  "(possible non-termination); please report");
#endif
		while (ps->p1d_switch_top > 0 && ps->p1d_switch_end[ps->p1d_switch_top - 1] > 0 &&
		       pparse_idx(_pc, ps->tok) > ps->p1d_switch_end[ps->p1d_switch_top - 1])
			ps->p1d_switch_top--;
		unsigned char tok_ch = ps->tok->ch0;
		if (pparse_token_can_name_function(ps->tok)) {
			PParseToken *nx = pparse_next(_pc, ps->tok);
			if (pparse_match_ch(nx, '(')) {
				bool is_func_decl = false;
				PParseFunctionSymbolKind proto_tag = pparse_function_decl_returns_aggregate(ps->tok)
								 ? PPARSE_FS_AGGREGATE_RETURN
								 : PPARSE_FS_FUNCTION;
				if (ps->brace_depth == 0 && pparse_paren_is_function_params(nx)) {
					pparse_function_symbol_put(ps->tok, proto_tag);
					is_func_decl = true;
				} else {
					PParseToken *prev = pparse_walk_back(pparse_idx(_pc, ps->tok), PPARSE_WB_PAST_NOISE);
					if (prev && ((prev->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE |
								   PPARSE_TT_SUE | PPARSE_TT_TYPEOF)) ||
						     pparse_is_known_typedef(prev))) {
						pparse_function_symbol_put(ps->tok, proto_tag);
						is_func_decl = true;
					}
				}
				if (is_func_decl && has_orelse) {
					/* pparse_skip_noise handles attributes but not asm specifiers. */
					PParseToken *after =
					    pparse_skip_asm_specifier_trail(pparse_next(_pc, pparse_pair_known(nx)));
					if (pparse_match_ch(after, ';') || pparse_match_ch(after, '{'))
						p1d_scan_param_bracket_orelse(nx, pparse_match_ch(after, ';'));
				}
			}
		}

		if (ps->p1d_cur_func == -1 && ps->p1d_init_brace_depth == 0 && ps->at_stmt_start &&
		    !pparse_is_known_typedef(ps->tok)) {
			if (ps->tok->tag & (PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE | PPARSE_TT_CASE | PPARSE_TT_DEFAULT))
				pparse_error_tok(ps->tok,
					  "control statement at file scope (must appear inside a "
					  "function body)");
			if (ps->tok->tag & PPARSE_TT_GOTO)
				pparse_error_tok(ps->tok, "'goto' at file scope (must appear inside a function body)");
			if ((ps->tok->tag & (PPARSE_TT_IF | PPARSE_TT_LOOP | PPARSE_TT_SWITCH)) || pparse_is_else_kw(ps->tok))
				pparse_error_tok(ps->tok,
					  "control-flow statement at file scope (must appear inside a "
					  "function body)");
		}

		if (has_orelse && tok_ch == '[' &&
		    !(ps->tok->flags & PPARSE_TF_C23_ATTR) && !(pparse_ann(ps->tok) & P1_OE_BRACKET))
			p1d_classify_bracket_orelse_ex(ps->tok, CUR_SID(), ps->p1d_cur_func, true,
						       /*allow_se_hoist=*/false);
		if (has_orelse && (ps->tok->tag & PPARSE_TT_ORELSE) && !pparse_token_has_binding(ps->tok)) {
			uint16_t cur_sid = CUR_SID();
			PParseToken *pv = pparse_walk_back(pparse_idx(_pc, ps->tok), PPARSE_WB_ATTR_NOISE);
			bool expr_ctx =
			    pv && (pv->kind == PPARSE_TK_NUM || pv->kind == PPARSE_TK_STR || pparse_match_ch(pv, ')') ||
				   pparse_match_ch(pv, ']') ||
				   (pparse_is_identifier_like(pv) &&
				    !(pv->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_SUE | PPARSE_TT_TYPEOF |
						 PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS | PPARSE_TT_INLINE | PPARSE_TT_ATTR | PPARSE_TT_RETURN |
						 PPARSE_TT_BREAK | PPARSE_TT_CONTINUE | PPARSE_TT_GOTO)) &&
				    !pparse_is_known_typedef(pv)));
			if (pparse_scope_tree && cur_sid < pparse_scope_tree_count &&
			    pparse_scope_tree[cur_sid].is_struct && expr_ctx)
				pparse_error_tok(ps->tok, PPARSE_ERR_ORELSE_STMT_LEVEL);
			if (ps->p1d_init_brace_depth > 0 && expr_ctx &&
			    !(pparse_ann(ps->tok) & P1_OE_BRACKET))
				pparse_error_tok(ps->tok,
					  "'orelse' cannot be used in a brace initializer "
					  "expression; only designator dimensions "
					  "'[idx orelse …]' are supported");
		}

		if (has_orelse && (ps->tok->tag & PPARSE_TT_TYPEOF))
			p1d_annotate_typeof_orelse(ps->tok, CUR_SID(), ps->p1d_cur_func, true);

		if (tok_ch == '{') {
			p1d_handle_open_brace(ps);
			continue;
		}
		if (tok_ch == '}') {
			p1d_handle_close_brace(ps);
			continue;
		}

		if (tok_ch == ';') {
			p1d_stmt_reset(ps, true);
			ps->p1d_prev = ps->tok;
			if (ps->brace_depth == 0) {
				// Phase 1C: C99 prototype parameter scope (§6.2.1p4).
				PParseToken *prev_tok = pparse_walk_back(pparse_idx(_pc, ps->tok) - 1, PPARSE_WB_SKIP_NOISE);
				PParseToken *open = prev_tok && pparse_match_ch(prev_tok, ')')
						    ? pparse_pair_known(prev_tok)
						    : NULL;
				if (open) {
					PPARSE_TD_SCOPE_SAVE();
					pparse_td_scope_close = pparse_idx(_pc, prev_tok);
					p1_register_param_shadows(open, prev_tok, 0, 1, false);
					PPARSE_TD_SCOPE_RESTORE();
				}

				ps->file_scope_stmt_start = pparse_next(_pc, ps->tok);
			}
			ps->tok = pparse_next(_pc, ps->tok);
			continue;
		}
		if (ps->tok->kind == PPARSE_TK_PREP_DIR) {
			ps->at_stmt_start = true;
			/* Keep ps->p1d_saw_raw: `raw _Pragma("...") int x;` must still
			 * suppress zero-init — pparse_skip_noise in Pass 2 preserves raw
			 * across prep dirs the same way. */
			ps->p1d_saw_static = false;
			if (ps->brace_depth == 0) ps->file_scope_stmt_start = pparse_next(_pc, ps->tok);
			ps->tok = pparse_next(_pc, ps->tok);
			continue;
		}

		if (!ps->at_stmt_start) {
			// Phase 1D: detect gotos and defers even in non-stmt-start
			if (ps->p1d_cur_func >= 0) {
				uint16_t cur_sid = CUR_SID();
				p1d_record_goto(ps, ps->tok, cur_sid, ps->p1d_cur_func);
				if (has_defer && is_defer_kw(ps->tok, ps->p1d_prev) &&
				    !(pparse_is_known_function_call(ps->tok) && !ps->p1d_ctrl_pending))
					p1_try_alloc_defer(ps->tok, cur_sid, ps->p1d_cur_func);
			}

			p1d_register_enum_at(ps->tok, ps->brace_depth, CUR_SID(), ps->p1d_cur_func);
			PParseToken *p1d_prev_saved = ps->p1d_prev;
			ps->p1d_prev = ps->tok;
			if (ps->tok->flags & PPARSE_TF_OPEN) {
				PParseToken *group_close = pparse_pair_known(ps->tok);
				// Do not skip GNU statement expressions — process their body normally
				if (pparse_is_stmt_expr_open(ps->tok)) {
					ps->tok = pparse_next(_pc, ps->tok); // advance past '(' to '{'
					ps->at_stmt_start = true;
					continue;
				}
				// Phase 1D: reject orelse/defer inside non-control-flow
				// parentheses (hoisted from Pass 2 check_orelse_in_parens).
				// typeof keeps expr-orelse (`typeof(p orelse 0)`) but still
				// rejects type-junk leaks (`typeof(int orelse 0)`).
				if (has_flow_extensions && tok_ch == '(' &&
				    !(p1d_prev_saved &&
				      (p1d_prev_saved->tag & (PPARSE_TT_IF | PPARSE_TT_LOOP | PPARSE_TT_SWITCH | PPARSE_TT_ATTR | PPARSE_TT_ASM)))) {
					if (p1d_prev_saved && (p1d_prev_saved->tag & PPARSE_TT_TYPEOF)) {
						/* typeof(expr orelse fb) is valid; type-junk is not. */
						if (has_orelse) {
							for (PParseToken *pi = ps->tok, *t = pparse_next(_pc, ps->tok);
							     t != group_close; pi = t, t = pparse_next(_pc, t)) {
								PParseToken *skipped = paren_scan_skip_nested(t);
								if (skipped) { t = skipped; continue; }
								if (orelse_after_type_in_parens(t, pi))
									pparse_error_tok(t, PPARSE_ERR_ORELSE_IN_PARENS);
							}
						}
					} else
						check_orelse_in_parens(ps->tok);
				}
				// Phase 1D: reject orelse/CF inside attribute and asm paren
				// groups (pre-/post-declarator attrs; __asm__(...)).
				if ((tok_ch == '(' && p1d_prev_saved &&
				      (p1d_prev_saved->tag & (PPARSE_TT_ATTR | PPARSE_TT_ASM))) ||
				    (ps->tok->flags & PPARSE_TF_C23_ATTR)) {
					int in_asm = p1d_prev_saved && (p1d_prev_saved->tag & PPARSE_TT_ASM);
					/* `asm volatile (` — walk back through asm qualifiers. */
					if (!in_asm && tok_ch == '(') {
						PParseToken *aw = pparse_walk_back(pparse_idx(_pc, ps->tok), PPARSE_WB_ATTR_NOISE);
						while (aw && (aw->tag & PPARSE_TT_QUALIFIER))
							aw = pparse_walk_back(pparse_idx(_pc, aw), PPARSE_WB_ATTR_NOISE);
						in_asm = aw && (aw->tag & PPARSE_TT_ASM);
					}
					PPARSE_FOR_RANGE(s, pparse_next(_pc, ps->tok), group_close) {
						uint32_t st = s->tag;
						if (has_orelse && (st & PPARSE_TT_ORELSE) && !pparse_token_has_binding(s)) {
							/* GNU symbolic operand name `[orelse]` is an
							 * identifier, not the Prism operator. */
							PParseToken *sp = pparse_walk_back(pparse_idx(_pc, s), PPARSE_WB_ATTR_NOISE);
							PParseToken *sn = pparse_skip_noise(_pc, pparse_next(_pc, s));
							if (sp && pparse_match_ch(sp, '[') && sn && pparse_match_ch(sn, ']'))
								continue;
							pparse_error_tok(s,
								  in_asm ? "'orelse' cannot be used inside "
									   "asm arguments"
									 : "'orelse' cannot be used inside "
									   "attribute arguments");
						}
						if (st & (PPARSE_TT_GOTO | PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE))
							pparse_error_tok(s,
								  in_asm ? "'%.*s' inside asm argument "
									   "bypasses control-flow analysis; "
									   "move it outside the asm"
									 : "'%.*s' inside attribute argument "
									   "bypasses control-flow analysis; "
									   "move it outside the attribute",
								  s->len, pparse_loc(_pc, s));
					}
				}
				if (tok_ch == '(' || tok_ch == '[') {
					PParseToken *se_open = p1d_scan_balanced_group(
					    ps->tok, ps->brace_depth, ps->p1d_cur_func, CUR_SID(), p1d_prev_saved);
					if (se_open) {
						ps->tok = se_open;
						continue;
					}
				}
				PParseToken *grp_open = ps->tok;
				ps->tok = pparse_next(_pc, group_close);
				/* After a group jump the predecessor is its close, not its open. */
				ps->p1d_prev = group_close;
				/* if/while/for/switch condition close — not else/do body '('. */
				if (pparse_match_ch(grp_open, '(') && pparse_ctrl_condition_kw_before_paren(grp_open)) {
					ps->at_stmt_start = true;
					ps->p1d_ctrl_pending = true;
				}
			} else {
				PParseToken *open = tok_ch == ')' ? pparse_pair_known(ps->tok) : NULL;
				if (open && pparse_ctrl_condition_kw_before_paren(open)) {
					/* Must not use PPARSE_WB_PAST_NOISE inside the helper: it jumps
					 * }/{ and nested (), so `} (expr)` after `while (c){}`
					 * would look like the while-condition close. */
					ps->at_stmt_start = true;
					ps->p1d_ctrl_pending = true;
				}
				ps->tok = pparse_next(_pc, ps->tok);
			}
			continue;
		}

		// Skip noise (attributes, C23 [[...]], pragmas)
		PParseToken *clean = pparse_skip_noise(_pc, ps->tok);
		if (clean != ps->tok) {
			if (pparse_range_has_attribute(ps->tok, clean, PPARSE_TT_ASM))
				ps->p1d_decl_has_attr = true;
			PPARSE_FOR_RANGE(s, ps->tok, clean) {
				uint32_t st = s->tag;
				if (st & (PPARSE_TT_GOTO | PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE))
					pparse_error_tok(s,
						  "'%.*s' inside attribute argument "
						  "bypasses control-flow analysis; "
						  "move it outside the attribute",
						  s->len,
						  pparse_loc(_pc, s));
				if ((st & PPARSE_TT_DEFER) && !pparse_token_has_binding(s))
					pparse_error_tok(s,
						  "'defer' inside attribute argument "
						  "bypasses control-flow analysis; "
						  "move it outside the attribute",
						  s->len,
						  pparse_loc(_pc, s));
				if ((st & PPARSE_TT_ORELSE) && !pparse_token_has_binding(s))
					pparse_error_tok(s,
						  "'orelse' cannot be used inside "
						  "attribute arguments");
			}
			ps->tok = clean;
			continue;
		}

		// Skip storage/inline/noreturn/extension specifiers before type
		if (((ps->tok->tag & (PPARSE_TT_STORAGE | PPARSE_TT_INLINE)) || pparse_equal(ps->tok, "__extension__")) &&
		    !(pparse_is_soft_keyword_identifier(ps->tok) && pparse_token_is_label_name(ps->tok))) {
			if (!ps->p1d_decl_start) ps->p1d_decl_start = ps->tok;
			if (ps->tok->tag & PPARSE_TT_STORAGE) ps->p1d_saw_static = true;
			ps->tok = pparse_next(_pc, ps->tok);
			continue;
		}

		if (ps->tok->flags & PPARSE_TF_RAW) {
			PParseToken *rnext = pparse_next(_pc, ps->tok);
			PParseToken *after_raw = pparse_skip_noise(_pc, rnext);
			bool typedef_kw_prefix =
			    pparse_is_known_typedef(ps->tok) &&
			    (pparse_is_type_keyword(after_raw) || pparse_is_known_typedef(after_raw) ||
			     (after_raw->tag &
			      (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_STORAGE | PPARSE_TT_INLINE | PPARSE_TT_TYPEDEF)) ||
			     (after_raw->flags & PPARSE_TF_RAW));
			if (!pparse_is_known_typedef(ps->tok) || typedef_kw_prefix) {
				PParseToken *after_colon = pparse_next(_pc, rnext);
				if (!(pparse_match_ch(rnext, ':') && !pparse_match_ch(after_colon, ':'))) {
					/* `raw { ... }` — annotate the brace so Pass 2 / decls
					 * suppress transforms for the whole block. */
					if (pparse_match_ch(after_raw, '{')) {
						after_raw->ann |= P1_RAW_BLOCK;
						pparse_pair_known(after_raw)->ann |= P1_RAW_BLOCK;
						pparse_p1_has_raw_block = true;
						ps->tok = after_raw;
						continue;
					}
					if (!ps->p1d_decl_start) ps->p1d_decl_start = ps->tok;
					ps->p1d_saw_raw = true;
					ps->tok = pparse_next(_pc, ps->tok);
					continue;
				}
			}
		}

		if (ps->tok->tag & PPARSE_TT_TYPEDEF) {
			uint32_t td_saved_close = 0;
			if (ps->p1d_ctrl_pending && ps->brace_depth > 0) {
				PParseToken *stmt_end = pparse_skip_one_stmt_impl(
				    ps->tok, p1d_ensure_skip_cache(&ps->skip_cache));
				if (stmt_end) {
					td_saved_close = pparse_td_scope_close;
					pparse_td_scope_close = pparse_idx(_pc, stmt_end);
				}
			}
			pparse_typedef_declaration(ps->tok, ps->brace_depth);
			if (td_saved_close) pparse_td_scope_close = td_saved_close;
			while (ps->tok->kind != PPARSE_TK_EOF && !pparse_match_ch(ps->tok, ';')) {
				p1d_register_enum_at(ps->tok, ps->brace_depth, CUR_SID(), ps->p1d_cur_func);
				if (pparse_match_ch(ps->tok, '{')) {
					PParseToken *close = pparse_pair_known(ps->tok);
					PPARSE_TD_SCOPE_SAVE();
					pparse_td_scope_close = pparse_idx(_pc, close);
					for (PParseToken *m = pparse_next(_pc, ps->tok);
					     m != close;) {
						if (pparse_is_enum_kw(m)) {
							uint32_t sc = pparse_td_scope_close;
							pparse_td_scope_close = _tds_c;
							p1d_register_enum_at(
							    m, ps->brace_depth, CUR_SID(), ps->p1d_cur_func);
							pparse_td_scope_close = sc;
						}
						if (m->flags & PPARSE_TF_OPEN) {
							PParseToken *mclose = pparse_pair_known(m);
							if (has_orelse &&
							    (pparse_match_ch(m, '(') || pparse_match_ch(m, '[')))
								p1d_scan_balanced_group(m,
											ps->brace_depth,
											ps->p1d_cur_func,
											CUR_SID(),
											NULL);
							m = pparse_next(_pc, mclose);
							continue;
						}
						if (pparse_is_valid_varname(m) &&
						    (pparse_is_known_typedef(m) ||
						     (m->tag & (PPARSE_TT_DEFER | PPARSE_TT_ORELSE)))) {
							PParseToken *nxt = pparse_next(_pc, m);
							if (nxt && pparse_match_set(nxt,
										      pparse_CH(';') | pparse_CH(',') |
											  pparse_CH(':') | pparse_CH('[') |
											  pparse_CH('='))) {
								// Only shadow if a type specifier precedes m in this member.
								if (pparse_match_ch(nxt, ':')) {
									bool has_type = false;
									for (uint32_t pi = pparse_idx(_pc, m);
									     pi > pparse_idx(_pc, ps->tok);
									     pi--) {
										PParseToken *pt =
										    &pparse_token_pool[pi - 1];
										if (pt->kind == PPARSE_TK_PREP_DIR)
											continue;
										if (pparse_match_set(pt,
											      pparse_CH(';') |
												  pparse_CH(',')) ||
										    pparse_match_ch(pt, '{') ||
										    pparse_match_ch(pt, '}'))
											break;
										if (pt->tag & PPARSE_TT_QUALIFIER)
											continue;
									if (pparse_match_ch(pt, ')') || pparse_match_ch(pt, ']')) {
										PParseToken *group_open = pparse_pair_known(pt);
											pi = pparse_idx(_pc, group_open) + 1;
											continue;
										}
										has_type = true;
										break;
									}
									if (!has_type) {
										m = pparse_next(_pc, m);
										continue;
									}
								}
								pparse_register_shadow(m, ps->brace_depth + 1);
							}
						}
						m = pparse_next(_pc, m);
					}
					PPARSE_TD_SCOPE_RESTORE();
					ps->tok = pparse_next(_pc, close);
					continue;
				}
				if (ps->tok->flags & PPARSE_TF_OPEN) {
					PParseToken *group_close = pparse_pair_known(ps->tok);
					if (has_orelse &&
					    (pparse_match_ch(ps->tok, '(') || pparse_match_ch(ps->tok, '[')))
						p1d_scan_balanced_group(ps->tok,
									ps->brace_depth,
									ps->p1d_cur_func,
									CUR_SID(),
									NULL);
					ps->tok = pparse_next(_pc, group_close);
				} else
					ps->tok = pparse_next(_pc, ps->tok);
			}
			if (pparse_match_ch(ps->tok, ';')) ps->tok = pparse_next(_pc, ps->tok);
			ps->at_stmt_start = true;
			if (ps->brace_depth == 0) ps->file_scope_stmt_start = ps->tok;
			continue;
		}

		// Does NOT continue — falls through to Phase 1C/1D for declaration
		// detection.
		if (ps->tok->tag & PPARSE_TT_SUE) {
			PParseToken *brace = pparse_find_struct_body_brace(ps->tok);
			if (brace) {
				if (pparse_is_enum_kw(ps->tok)) {
					p1d_register_enum_at(ps->tok, ps->brace_depth, CUR_SID(), ps->p1d_cur_func);
				} else {
					unsigned body_traits = pparse_struct_body_traits(brace);
					for (PParseToken *t = pparse_skip_noise(_pc, pparse_next(_pc, ps->tok)); t != brace;
					     t = pparse_skip_noise(_pc, pparse_next(_pc, t))) {
						if ((t->tag & PPARSE_TT_QUALIFIER) && !pparse_is_soft_keyword_identifier(t))
							continue;
						if (pparse_is_valid_varname(t)) {
							pparse_register_struct_tag(t, ps->brace_depth,
									  body_traits & PPARSE_SBT_VLA,
									  body_traits & PPARSE_SBT_VOL);
							break;
						}
						break;
					}
				}
			}
		}

		if ((ps->tok->flags & PPARSE_TF_STATIC_ASSERT) &&
		    !(pparse_is_soft_keyword_identifier(ps->tok) && pparse_token_is_label_name(ps->tok))) {
			/* Classify bracket orelse inside static assertions; reject other forms. */
				if (has_flow_extensions) {
				PParseToken *lp = pparse_skip_noise(_pc, pparse_next(_pc, ps->tok));
				if (pparse_match_ch(lp, '(')) {
					PParseToken *rp = pparse_pair_known(lp);
					/* Walk every token — do not skip nested groups or
					 * `sizeof(char[…])` brackets are never classified. */
					if (has_orelse)
						for (PParseToken *inner = pparse_next(_pc, lp); inner != rp;
						     inner = pparse_next(_pc, inner)) {
							if (pparse_match_ch(inner, '[')) {
								PParseToken *bc = pparse_pair_known(inner);
							if (!(inner->flags & PPARSE_TF_C23_ATTR) &&
							    !(pparse_ann(inner) & P1_OE_BRACKET))
								p1d_classify_bracket_orelse_ex(inner,
											     CUR_SID(),
											     ps->p1d_cur_func,
											     /*hard_ctx=*/false,
											     /*allow_se_hoist=*/false);
							/* _Static_assert needs an ICE — VLA dims from
							 * `sizeof(char[n orelse 1])` are not ICEs. */
							if (pparse_ann(inner) & P1_OE_BRACKET) {
								for (PParseToken *oe = pparse_next(_pc, inner); oe != bc;
								     oe = pparse_next(_pc, oe)) {
									if (!(pparse_ann(oe) & P1_IS_ORELSE_KW))
										continue;
									if (pparse_expr_maybe_nonconstant(pparse_next(_pc, inner), oe))
										pparse_error_tok(
										    oe,
										    "'orelse' in an array "
										    "dimension inside "
										    "_Static_assert/static_assert "
										    "requires an integer constant "
										    "expression on the left-hand "
										    "side");
								}
							}
							}
						}
					PParseToken *prev_sa = lp;
					PPARSE_FOR_RANGE(s, pparse_next(_pc, lp), rp) {
						/* Walk into nested groups — `(0 orelse 1)` and
						 * `sizeof(0 orelse 1)` / `int orelse 0` must reject, not leak. */
						if (has_orelse && !(pparse_ann(s) & P1_IS_ORELSE_KW) &&
						    (orelse_kw_at_bare(s, prev_sa) ||
						     orelse_after_type_in_parens(s, prev_sa)))
							pparse_error_tok(s,
								  "'orelse' cannot be used in "
								  "_Static_assert/static_assert except "
								  "inside an array dimension");
						/* Reject defer statements, but preserve defer identifiers. */
						PParseToken *sn = pparse_next(_pc, s);
						if (has_defer && (s->tag & PPARSE_TT_DEFER) &&
						    (pparse_is_identifier_like(sn) || pparse_match_ch(sn, '{')))
							pparse_error_tok(s,
								  "'defer' cannot be used inside "
								  "_Static_assert/static_assert");
						prev_sa = s;
					}
				}
			}
			ps->tok = pparse_skip_to_semicolon(ps->tok, NULL);
			if (ps->tok && pparse_match_ch(ps->tok, ';')) ps->tok = pparse_next(_pc, ps->tok);
			ps->at_stmt_start = true;
			if (ps->brace_depth == 0) ps->file_scope_stmt_start = ps->tok;
			continue;
		}

		if (ps->brace_depth > 0) {
			if (ps->tok->tag & PPARSE_TT_LOOP) {
				if (ps->tok->ch0 == 'f' && ps->p1d_cur_func >= 0)
					p1d_scan_ctrl_init(ps->tok,
							   &ps->skip_cache,
							   ps->brace_depth,
							   CUR_SID(),
							   /*is_for=*/true);
				/* C does not allow declaration in while (...); reject C23-looking while-init. */
				else if (ps->tok->ch0 == 'w') {
					PParseToken *wopen = pparse_p1d_find_open_paren(ps->tok);
					if (wopen) {
						PParseToken *inner =
						    pparse_skip_noise(_pc, pparse_next(_pc, wopen));
						if ((inner->tag &
						      (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER |
						       PPARSE_TT_STORAGE | PPARSE_TT_SUE |
						       PPARSE_TT_TYPEDEF)) ||
						     (inner->kind == PPARSE_TK_IDENT &&
						      pparse_is_known_typedef(inner)))
							pparse_error_tok(inner,
									  "declaration in 'while' condition is not allowed");
					}
				}
			} else if (ps->p1d_cur_func >= 0 &&
				   (ps->tok->tag & (PPARSE_TT_IF | PPARSE_TT_SWITCH)) &&
				   !pparse_is_else_kw(ps->tok)) {
				p1d_scan_ctrl_init(ps->tok,
						   &ps->skip_cache,
						   ps->brace_depth,
						   CUR_SID(),
						   /*is_for=*/false);
			}
		}
		if (ps->p1d_cur_func >= 0) {
			uint16_t cur_sid = CUR_SID();
			// GNU __label__ local label declaration: __label__ id1, id2, ...;
			if (ps->tok->kind == PPARSE_TK_IDENT && ps->tok->len == 9 &&
			    prism_memeq_static(pparse_loc(_pc, ps->tok), "__label__", 9)) {
				PParseToken *t = pparse_next(_pc, ps->tok);
			while (t->kind != PPARSE_TK_EOF && !pparse_match_ch(t, ';')) {
					if (pparse_is_identifier_like(t)) {
						PPARSE_ARENA_ENSURE_CAP(&_pc->main_arena,
								 ps->local_labels,
								 ps->local_label_count + 1,
								 ps->local_label_cap,
								 16,
								 ps->local_labels[0]);
						int name_len = t->len;
						char sid_buf[12];
						int sid_len = snprintf(
						    sid_buf, sizeof(sid_buf), "%u", (unsigned)cur_sid);
						int mangled_len = name_len + 1 + sid_len;
						char *mangled =
						    pparse_arena_alloc_uninit(&_pc->main_arena, mangled_len);
						memcpy(mangled, pparse_loc(_pc, t), name_len);
						mangled[name_len] = '\0';
						memcpy(mangled + name_len + 1, sid_buf, sid_len);
						int li = ps->local_label_count++;
						ps->local_labels[li].name = pparse_loc(_pc, t);
						ps->local_labels[li].len = name_len;
						ps->local_labels[li].scope_id = cur_sid;
						ps->local_labels[li].mangled = mangled;
						ps->local_labels[li].mangled_len = mangled_len;
					}
					t = pparse_next(_pc, t);
				}
				if (pparse_match_ch(t, ';')) t = pparse_next(_pc, t);
				ps->p1d_prev = ps->tok;
				ps->tok = t;
				ps->at_stmt_start = true;
				ps->p1d_ctrl_pending = false;
				continue;
			}

			/* `do int x=1; while(0);` — declaration is not a valid braceless do body. */
			if (ps->p1d_prev && pparse_is_do_kw(ps->p1d_prev) &&
			    ((ps->tok->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_SUE | PPARSE_TT_TYPEDEF)) ||
			     (ps->tok->kind == PPARSE_TK_IDENT && pparse_is_known_typedef(ps->tok))))
				pparse_error_tok(ps->tok, "'do' body starting with a declaration requires braces");

			if (pparse_is_identifier_like(ps->tok) &&
			    (!(ps->tok->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE)) ||
			     pparse_is_soft_keyword_identifier(ps->tok))) {
				PParseToken *colon = pparse_skip_noise(_pc, pparse_next(_pc, ps->tok));
				if (pparse_match_ch(colon, ':') &&
				    !pparse_match_ch(pparse_next(_pc, colon), ':') &&
				    !(ps->tok->tag & (PPARSE_TT_CASE | PPARSE_TT_DEFAULT)) && ps->p1d_init_brace_depth == 0) {
					pparse_ann(colon) |= P1_STMT_COLON;
					P1FuncEntry *e = p1_alloc(P1K_LABEL, cur_sid, ps->tok);
					p1d_set_label_name(e, ps, ps->tok, cur_sid);
					ps->p1d_prev = colon;
					ps->tok = pparse_next(_pc, colon);
					ps->at_stmt_start = true;
					continue;
				}
			}

			p1d_record_goto(ps, ps->tok, cur_sid, ps->p1d_cur_func);
			if (has_defer && (ps->tok->tag & PPARSE_TT_DEFER) && is_defer_kw(ps->tok, ps->p1d_prev)) {
				pparse_ann(ps->tok) |= P1_IS_DEFER_KW;
				p1_try_alloc_defer(ps->tok, cur_sid, ps->p1d_cur_func);
			}
			/* Context scanners, not a blanket rule, reject stray defer tokens. */
			if (ps->tok->tag & (PPARSE_TT_CASE | PPARSE_TT_DEFAULT)) {
				uint16_t sw_sid =
				    ps->p1d_switch_top > 0 ? ps->p1d_switch_stack[ps->p1d_switch_top - 1] : 0;
				P1FuncEntry *e = p1_alloc(P1K_CASE, cur_sid, ps->tok);
				e->kase.switch_scope_id = sw_sid;
				PParseToken *ct = pparse_next(_pc, ps->tok);
				PParseToken *cprev = ps->tok;
				int td = 0;
				while (ct->kind != PPARSE_TK_EOF) {
					if (pparse_match_ch(ct, ';') || pparse_match_ch(ct, '{')) break;
					/* case label expressions are integer constant
					 * expressions: an orelse there has no statement
					 * context to lower into and would otherwise pass
					 * through to the backend verbatim. */
					if (has_orelse && (ct->tag & PPARSE_TT_ORELSE) &&
					    !(cprev->tag & PPARSE_TT_MEMBER) && !pparse_token_has_binding(ct))
						pparse_error_tok(ct,
							  "'orelse' cannot be used inside a case "
							  "label expression");
					if (ct->flags & PPARSE_TF_OPEN) {
						PParseToken *cam = pparse_pair_known(ct);
						if (has_orelse) {
							PParseToken *cip = ct;
							for (PParseToken *s = pparse_next(_pc, ct); s != cam;
							     s = pparse_next(_pc, s)) {
								if ((s->tag & PPARSE_TT_ORELSE) &&
								    !(cip->tag & PPARSE_TT_MEMBER) &&
								    !pparse_token_has_binding(s))
									pparse_error_tok(
									    s,
									    "'orelse' cannot be used "
									    "inside a case label "
									    "expression");
								cip = s;
							}
						}
						cprev = cam;
						ct = pparse_next(_pc, cam);
						continue;
					}
					if (pparse_match_ch(ct, '?')) {
						td++;
						cprev = ct;
						ct = pparse_next(_pc, ct);
						continue;
					}
					if (pparse_match_ch(ct, ':')) {
						if (td > 0) {
							td--;
							cprev = ct;
							ct = pparse_next(_pc, ct);
							continue;
						}
						break;
					}
					cprev = ct;
					ct = pparse_next(_pc, ct);
				}
				if (pparse_match_ch(ct, ':')) {
					pparse_ann(ct) |= P1_STMT_COLON;
					ps->p1d_prev = ct;
					ps->tok = pparse_next(_pc, ct);
					p1d_stmt_reset(ps, true);
					continue;
				}
			}
		}

		if ((pparse_ann(ps->tok) & P1_IS_DEFER_KW) ||
		    (has_defer && (ps->tok->tag & PPARSE_TT_DEFER) &&
		     is_defer_kw(ps->tok, ps->p1d_prev))) {
			pparse_ann(ps->tok) |= P1_IS_DEFER_KW;
			p1d_validate_defer(ps->tok, ps->p1d_cur_func, ps->p1d_ctrl_pending, CUR_SID(),
					   ps->brace_depth);
		}
		p1d_probe_declaration(ps->tok,
				      ps->p1d_decl_start,
				      CUR_SID(),
				      ps->brace_depth,
				      ps->p1d_cur_func,
				      &ps->p1d_saw_raw,
				      ps->p1d_saw_static,
				      ps->p1d_decl_has_attr,
				      ps->p1d_ctrl_pending,
				      &ps->skip_cache);
		ps->p1d_decl_start = NULL;
		ps->p1d_decl_has_attr = false;
		if (ps->p1d_cur_func >= 0 && (ps->tok->tag & PPARSE_TT_SWITCH) && !pparse_is_known_typedef(ps->tok)) {
			PParseToken *p = pparse_skip_prep_dirs(pparse_next(_pc, ps->tok));
			if (pparse_match_ch(p, '(')) {
				PParseToken *body = pparse_skip_prep_dirs(pparse_next(_pc, pparse_pair_known(p)));
				if (!pparse_match_ch(body, '{')) {
					uint32_t synth_sid = ps->p1d_braceless_next_sid++;
					if (synth_sid > UINT16_MAX)
						pparse_error_tok(ps->tok,
							  "too many scopes + braceless switches (>65535)");
					p1_alloc(P1K_SWITCH, (uint16_t)synth_sid, ps->tok);
					p1d_ensure_switch_cap(ps);
					ps->p1d_switch_stack[ps->p1d_switch_top] = synth_sid;
					PParseToken *end = pparse_skip_one_stmt_impl(
					    body, p1d_ensure_skip_cache(&ps->skip_cache));
					ps->p1d_switch_end[ps->p1d_switch_top] = end ? pparse_idx(_pc, end) : UINT32_MAX;
					ps->p1d_switch_top++;
				}
			}
		}

		if (pparse_is_else_or_do(ps->tok)) {
			ps->p1d_prev = ps->tok;
			ps->tok = pparse_next(_pc, ps->tok);
			ps->at_stmt_start = true;
			ps->p1d_ctrl_pending = true;
			continue;
		}

		if (has_orelse && ps->p1d_cur_func >= 0 && ps->brace_depth > 0 &&
		    !(ps->tok->tag & (PPARSE_TT_IF | PPARSE_TT_LOOP | PPARSE_TT_SWITCH | PPARSE_TT_GOTO | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE | PPARSE_TT_CASE |
				  PPARSE_TT_DEFAULT | PPARSE_TT_DEFER))) {
			if (ps->tok->tag & PPARSE_TT_RETURN) {
				PParseToken *body = pparse_skip_noise(_pc, pparse_next(_pc, ps->tok));
				PParseToken *bare_oe = find_bare_orelse(body);
				if (bare_oe && !(pparse_ann(bare_oe) & (P1_OE_BRACKET | P1_OE_DECL_INIT))) {
					/* `return orelse;` — identifier operand starts the expr.
					 * Do not walk back with PPARSE_WB_PAST_NOISE: it skips `(x)`, so
					 * `return (x) orelse 1` looked like `return orelse` and
					 * leaked the keyword to the backend. */
					if (body != bare_oe)
						pparse_error_tok(bare_oe,
							  "'orelse' cannot be used in a return expression; "
							  "assign to a temporary first");
				}
			} else {
				PParseToken *bare_oe = find_bare_orelse(ps->tok);
				if (bare_oe && !(pparse_ann(bare_oe) & (P1_OE_BRACKET | P1_OE_DECL_INIT)))
					p1d_validate_bare_orelse(ps->tok, bare_oe);
			}
		}

		/* Paren-led braceless bodies: `if (c) (x = get() orelse 0);`
		 * open at stmt-start, so the !at_stmt_start paren scan misses them. */
		if (has_flow_extensions && tok_ch == '(' &&
		    !pparse_is_stmt_expr_open(ps->tok))
			check_orelse_in_parens(ps->tok);

		ps->at_stmt_start = false;
		ps->p1d_prev = ps->tok;
		ps->tok = pparse_next(_pc, ps->tok);
	}
#undef CUR_SID
}

// Report a goto-skips-defer/decl pparse_error or warning.
static void cfg_report_goto(PParseToken *bad, const char *msg, P1FuncEntry *label) {
	PPARSE_CTX();
	if (pparse_feat(PPARSE_F_WARN_SAFETY)) pparse_warn_tok(bad, msg, label->label.len, label->label.name);
	else
		pparse_error_tok(bad, msg, label->label.len, label->label.name);
}

static inline uint32_t decl_effective_close(const P1FuncEntry *d) {
	PPARSE_CTX();
	if (d->decl.body_close_idx > 0) return d->decl.body_close_idx;
	if (d->scope_id > 0 && d->scope_id < pparse_scope_tree_count)
		return pparse_scope_close(&pparse_scope_tree[d->scope_id]);
	return 0;
}

static void cfg_reject_goto_into_stmt_expr(P1FuncEntry *g, uint16_t label_scope_id) {
	PPARSE_CTX();
	uint16_t label_se = pparse_scope_stmt_expr_ancestor(label_scope_id);
	if (label_se != 0 && !pparse_scope_is_ancestor_or_self(label_se, g->scope_id))
		pparse_error_tok(p1_tok(g),
				  "goto '%.*s' jumps into a statement expression "
				  "(jumping into ({...}) is undefined behavior)",
				  g->label.len,
				  g->label.name);
}

static void cfg_error_if_looped_between(P1FuncEntry *ents,
					P1FuncEntry *g,
					P1FuncEntry *label,
					int *list,
					int lo,
					int hi,
					bool vla_only,
					const char *msg) {
	PPARSE_CTX();
	for (int di = lo; di < hi; di++) {
		P1FuncEntry *d = &ents[list[di]];
		if (vla_only && !d->decl.is_vla) continue;
		if (d->token_index >= g->token_index) continue;
		if (!pparse_scope_is_ancestor_or_self(d->scope_id, label->scope_id)) continue;
		if (!pparse_scope_is_ancestor_or_self(d->scope_id, g->scope_id)) continue;
		if (vla_only) {
			uint32_t vla_close = decl_effective_close(d);
			if (vla_close > 0 && vla_close <= g->token_index) continue;
		}
		pparse_error_tok(p1_tok(g), msg, g->label.len, g->label.name);
		break;
	}
}

static void cfg_fill_assigned_first(P1FuncEntry *ents,
				    P1FuncEntry *label,
				    int *decl_list,
				    int decl_n,
				    uint8_t *out) {
	PPARSE_CTX();
	memset(out, 0, (size_t)decl_n);
	if (!pparse_feat(PPARSE_F_ZEROINIT) || decl_n <= 0) return;
	PParseToken *t = &pparse_token_pool[label->token_index];
	while (t && t->kind != PPARSE_TK_EOF && !pparse_match_ch(t, ':')) t = pparse_next(_pc, t);
	if (t) t = pparse_next(_pc, t);
	int bd = 0;
	PParseToken *first_name = NULL;
	while (t && t->kind != PPARSE_TK_EOF) {
		if (pparse_match_ch(t, '{')) {
			bd++;
			t = pparse_next(_pc, t);
			continue;
		}
		if (pparse_match_ch(t, '}')) {
			if (bd == 0) break;
			bd--;
			t = pparse_next(_pc, t);
			continue;
		}
		if (t->kind == PPARSE_TK_IDENT) {
			PParseToken *n = pparse_next(_pc, t);
			if (n && pparse_match_ch(n, '=')) {
				first_name = t;
				/* `x = x + 1` is not a defining first assign — RHS uses x. */
				PPARSE_FOR_TAIL(s, pparse_next(_pc, n)) {
					PPARSE_SKIP_GROUP_ON_CLOSE(s)
					if (pparse_match_ch(s, ';')) break;
					if (s->kind == PPARSE_TK_IDENT && s->len == t->len &&
					    prism_memeq_runtime_sized(pparse_loc(_pc, s), pparse_loc(_pc, t), t->len)) {
						first_name = NULL;
						break;
					}
				}
			}
			break;
		}
		t = pparse_next(_pc, t);
	}
	if (!first_name) return;
	for (int di = 0; di < decl_n; di++) {
		P1FuncEntry *d = &ents[decl_list[di]];
		if (d->decl.is_vla || d->decl.has_raw || d->decl.is_static_storage || d->decl.has_init)
			continue;
		PParseToken *lname = p1_tok(d);
		if (lname->len == first_name->len &&
		    prism_memeq_runtime_sized(pparse_loc(_pc, lname), pparse_loc(_pc, first_name), lname->len))
			out[di] = 1;
	}
}

static void cfg_check_range(P1FuncEntry *ents,
			    P1FuncEntry *g,
			    P1FuncEntry *label,
			    bool is_forward,
			    int *defer_list,
			    int defer_lo,
			    int defer_hi,
			    int *decl_list,
			    int decl_lo,
			    int decl_hi,
			    const uint8_t *label_assigned_first) {
	PPARSE_CTX();
	PParseToken *bad_defer = NULL, *bad_decl = NULL;
	bool bad_decl_is_vla = false;
	if (pparse_feat(PPARSE_F_DEFER)) {
		for (int di = defer_lo; di < defer_hi; di++) {
			P1FuncEntry *d = &ents[defer_list[di]];
			// Scope must be an ancestor-or-self of the label's scope
			if (!pparse_scope_is_ancestor_or_self(d->scope_id, label->scope_id)) continue;
			if (!is_forward && pparse_scope_is_ancestor_or_self(d->scope_id, g->scope_id)) continue;
			// Defer's scope must still be open at the label position.
			if (d->scope_id > 0 && d->scope_id < pparse_scope_tree_count) {
				uint32_t close = pparse_scope_close(&pparse_scope_tree[d->scope_id]);
				if (close < label->token_index) continue;
			}
			bad_defer = p1_tok(d);
			break;
		}
	}

	// VLA skip is always a hard pparse_error (C99/C11 6.8.6.1p1) regardless of
	bool bad_decl_has_init = false;
	P1FuncEntry *first_vla = NULL, *first_init = NULL, *first_other = NULL;
	int first_other_di = -1;
	for (int di = decl_lo; di < decl_hi; di++) {
		P1FuncEntry *d = &ents[decl_list[di]];
		if (!pparse_scope_is_ancestor_or_self(d->scope_id, label->scope_id)) continue;
		if (!is_forward && pparse_scope_is_ancestor_or_self(d->scope_id, g->scope_id)) continue;
		uint32_t close = decl_effective_close(d);
		if (close > 0 && close < label->token_index) continue;
		if (d->decl.is_vla) {
			if (!first_vla) first_vla = d;
			continue;
		}
		if (d->decl.has_raw || d->decl.is_static_storage) continue;
		if (d->decl.has_init) {
			if (!first_init) first_init = d;
		} else if (!first_other) {
			first_other = d;
			first_other_di = di;
		}
	}
	if (first_vla) {
		bad_decl = p1_tok(first_vla);
		bad_decl_is_vla = true;
	} else if (first_init) {
		bad_decl = p1_tok(first_init);
		bad_decl_has_init = true;
	} else if (first_other && pparse_feat(PPARSE_F_ZEROINIT)) {
		bool assigned_first = label_assigned_first && first_other_di >= 0 &&
				      label_assigned_first[first_other_di];
		if (!assigned_first) bad_decl = p1_tok(first_other);
	}

	if (bad_defer) cfg_report_goto(bad_defer, "goto '%.*s' would skip over this defer statement", label);
	if (bad_decl) {
		const char *msg;
		if (bad_decl_is_vla) msg = "goto '%.*s' would skip over this VLA declaration";
		else if (bad_decl_has_init)
			msg = "goto '%.*s' would skip over a declaration with an initializer "
			      "(undefined behavior if jumped into)";
		else
			msg = "goto '%.*s' would skip over this variable declaration "
			      "(bypasses initialization)";
		if (bad_decl_is_vla) pparse_error_tok(bad_decl, msg, label->label.len, label->label.name);
		else
			cfg_report_goto(bad_decl, msg, label);
	}
}

static void p1_verify_cfg(void) {
	PPARSE_CTX();
	const bool has_defer = pparse_feat(PPARSE_F_DEFER);
	const bool has_zeroinit = pparse_feat(PPARSE_F_ZEROINIT);
	for (int fi = 0; fi < func_meta_count; fi++) {
		FuncMeta *fm = &func_meta[fi];
		if (fm->entry_count == 0) continue;
		if (!(has_defer | has_zeroinit)) {
			P1FuncEntry *ents = &p1_entries[fm->entry_start];
			bool has_vla = false;
			for (int i = 0; i < fm->entry_count; i++)
				if (ents[i].kind == P1K_DECL && ents[i].decl.is_vla) {
					has_vla = true;
					break;
				}
			if (!has_vla) continue;
		}

		// Computed gotos cannot be verified statically: they could jump
		bool unverifiable_jump = fm->has_computed_goto | ((fm->body_open->tag & PPARSE_TT_ASM) != 0);
		if (unverifiable_jump) {
			const char *jump_kind = fm->has_computed_goto ? "computed goto" : "asm goto";
			static const char fmt[] = "%s cannot be used in a function that "
						  "contains %s: the jump target cannot be "
						  "verified at compile time";
			P1FuncEntry *ents = &p1_entries[fm->entry_start];
			for (int i = 0; i < fm->entry_count; i++) {
				const char *blocker = NULL;
				if (ents[i].kind == P1K_DEFER && has_defer) blocker = "defer statements";
				else if (ents[i].kind == P1K_DECL && ents[i].decl.is_vla)
					blocker = "variable-length arrays";
				else if (ents[i].kind == P1K_DECL && has_zeroinit &&
					 !ents[i].decl.has_raw && !ents[i].decl.is_static_storage &&
					 !ents[i].decl.is_vla && !ents[i].decl.has_init)
					blocker = "zero-initialized declarations";
				if (blocker) pparse_error_tok(fm->body_open, fmt, jump_kind, blocker);
			}
		}

		if (has_defer && !fm->returns_void && !fm->ret_type_start) {
			P1FuncEntry *ents = &p1_entries[fm->entry_start];
			for (int i = 0; i < fm->entry_count; i++) {
				if (ents[i].kind == P1K_DEFER)
					pparse_error_tok(fm->body_open,
						  "defer in function with unresolvable return type; "
						  "use a named struct or typedef");
			}
		}

		// Allocated before arena mark so it persists in FuncMeta for Pass 2 O(1)
		// lookup.
		int cnt = fm->entry_count;
		int hash_sz = 16;
		while (hash_sz < cnt * 2) hash_sz <<= 1;
		int *label_hash = pparse_arena_alloc(&_pc->main_arena, (size_t)hash_sz * sizeof(int));
		memset(label_hash, 0xFF, (size_t)hash_sz * sizeof(int)); // -1 = empty
		int hash_mask = hash_sz - 1;
		PParseArenaMark mark = pparse_arena_mark(&_pc->main_arena);
		P1FuncEntry *ents = &p1_entries[fm->entry_start];
		int *defer_list = pparse_arena_alloc(&_pc->main_arena, (size_t)cnt * sizeof(int));
		int *decl_list = pparse_arena_alloc(&_pc->main_arena, (size_t)cnt * sizeof(int));
		int *wm_defer = pparse_arena_alloc(&_pc->main_arena, (size_t)cnt * sizeof(int));
		int *wm_decl = pparse_arena_alloc(&_pc->main_arena, (size_t)cnt * sizeof(int));
		uint8_t **af_cache = pparse_arena_alloc(&_pc->main_arena, (size_t)cnt * sizeof(uint8_t *));
		int defer_n = 0, decl_n = 0;
		for (int i = 0; i < cnt; i++) {
			if (ents[i].kind != P1K_LABEL) continue;
			uint32_t h = (uint32_t)pparse_fast_hash(ents[i].label.name, ents[i].label.len);
			for (int probe = 0; probe < hash_sz; probe++) {
				int slot = (h + probe) & hash_mask;
				if (label_hash[slot] < 0) {
					label_hash[slot] = i;
					break;
				}
				P1FuncEntry *existing = &ents[label_hash[slot]];
				if (existing->label.len == ents[i].label.len &&
				    prism_memeq_runtime_sized(existing->label.name, ents[i].label.name,
					     existing->label.len))
					pparse_error_tok(p1_tok(&ents[i]),
						  "duplicate label '%.*s'",
						  ents[i].label.len,
						  ents[i].label.name);
			}
		}

		// Persist label hash in FuncMeta for O(1) lookup in Pass 2
		fm->label_hash = label_hash;
		fm->label_hash_mask = hash_mask;

		typedef struct {
			int idx, dm, cm, next;
		} FwdGoto;

		FwdGoto *fwd = pparse_arena_alloc(&_pc->main_arena, (size_t)cnt * sizeof(FwdGoto));
		int fwd_n = 0;
		int *fwd_hash_tbl = pparse_arena_alloc(&_pc->main_arena, (size_t)hash_sz * sizeof(int));
		memset(fwd_hash_tbl, 0xFF, (size_t)hash_sz * sizeof(int)); // -1 = empty

		/* Switch watermarks indexed by (scope_id - sw_min_sid): a function's
		 * switch scope_ids span its own scope range, so sizing to the SPAN is
		 * O(function scopes). Sizing to the absolute max scope_id was O(global
		 * scope_id) per function → O(n^2) memory across the TU. */
		int sw_max_sid = 0, sw_min_sid = INT_MAX;
		for (int i = 0; i < cnt; i++)
			if (ents[i].kind == P1K_SWITCH) {
				if (ents[i].scope_id > sw_max_sid) sw_max_sid = ents[i].scope_id;
				if (ents[i].scope_id < sw_min_sid) sw_min_sid = ents[i].scope_id;
			}
		int sw_sz = sw_max_sid >= sw_min_sid ? sw_max_sid - sw_min_sid + 1 : 0;
		int *sw_defer_wm = NULL, *sw_decl_wm = NULL;
		if (sw_sz > 0 && sw_sz <= 65536) {
			sw_defer_wm = pparse_arena_alloc(&_pc->main_arena, (size_t)sw_sz * sizeof(int));
			sw_decl_wm = pparse_arena_alloc(&_pc->main_arena, (size_t)sw_sz * sizeof(int));
		}

		for (int i = 0; i < cnt; i++) {
			wm_defer[i] = defer_n;
			wm_decl[i] = decl_n;
			switch (ents[i].kind) {
			case P1K_DEFER: {
				PParseToken *defer_kw = p1_tok(&ents[i]);
				defer_kw->parse_data = (uint32_t)(fm->entry_start + i) + 1;
				defer_list[defer_n++] = i;
				break;
			}
			case P1K_DECL: decl_list[decl_n++] = i; break;
			case P1K_LABEL: {
				uint32_t lh = (uint32_t)pparse_fast_hash(ents[i].label.name, ents[i].label.len);
				int fh_slot = lh & hash_mask;
				int prev_fi = -1;
				int fi = fwd_hash_tbl[fh_slot];
				while (fi >= 0) {
					int next_fi = fwd[fi].next;
					P1FuncEntry *g = &ents[fwd[fi].idx];
					if (g->label.len == ents[i].label.len &&
					    prism_memeq_runtime_sized(g->label.name, ents[i].label.name, g->label.len)) {
							cfg_reject_goto_into_stmt_expr(g, ents[i].scope_id);
						p1_tok(g)->parse_data =
						    (uint32_t)pparse_scope_block_exits(g->scope_id, ents[i].scope_id);
						if (!af_cache[i] && decl_n > 0) {
							af_cache[i] = pparse_arena_alloc(&_pc->main_arena,
										  (size_t)decl_n);
							cfg_fill_assigned_first(
							    ents, &ents[i], decl_list, decl_n, af_cache[i]);
						}
						cfg_check_range(ents,
								g,
								&ents[i],
								/*is_forward=*/true,
								defer_list,
								fwd[fi].dm,
								defer_n,
								decl_list,
								fwd[fi].cm,
								decl_n,
								af_cache[i]);
						if (prev_fi < 0) fwd_hash_tbl[fh_slot] = next_fi;
						else
							fwd[prev_fi].next = next_fi;
						fi = next_fi;
						continue;
					}
					prev_fi = fi;
					fi = next_fi;
				}
				break;
			}
			case P1K_GOTO: {
				P1FuncEntry *g = &ents[i];
				uint32_t h = (uint32_t)pparse_fast_hash(g->label.name, g->label.len);
				int li = -1;
				for (int probe = 0; probe < hash_sz; probe++) {
					int slot = (h + probe) & hash_mask;
					if (label_hash[slot] < 0) break;
					P1FuncEntry *cand = &ents[label_hash[slot]];
					if (cand->label.len == g->label.len &&
					    prism_memeq_runtime_sized(cand->label.name, g->label.name, g->label.len)) {
						li = label_hash[slot];
						break;
					}
				}

				if (li < 0 || li > i) {
					// Forward goto (label not yet seen, or found after current pos)
					int fi = fwd_n++;
					int fh_slot = h & hash_mask;
					fwd[fi] = (FwdGoto){.idx = i,
							    .dm = defer_n,
							    .cm = decl_n,
							    .next = fwd_hash_tbl[fh_slot]};
					fwd_hash_tbl[fh_slot] = fi;
				} else {
						cfg_reject_goto_into_stmt_expr(g, ents[li].scope_id);
					p1_tok(g)->parse_data =
					    (uint32_t)pparse_scope_block_exits(g->scope_id, ents[li].scope_id);
					if (!af_cache[li] && wm_decl[li] > 0) {
						af_cache[li] =
						    pparse_arena_alloc(&_pc->main_arena, (size_t)wm_decl[li]);
						cfg_fill_assigned_first(
						    ents, &ents[li], decl_list, wm_decl[li], af_cache[li]);
					}
					cfg_check_range(ents,
							g,
							&ents[li],
							/*is_forward=*/false,
							defer_list,
							0,
							wm_defer[li],
							decl_list,
							0,
							wm_decl[li],
							af_cache[li]);
					if (has_defer)
						cfg_error_if_looped_between(
						    ents,
						    g,
						    &ents[li],
						    defer_list,
						    wm_defer[li],
						    defer_n,
						    false,
						    "goto '%.*s' loops over a defer statement; "
						    "lexical defer does not execute dynamically and "
						    "the defer body will only execute once at scope exit");
					cfg_error_if_looped_between(
					    ents,
					    g,
					    &ents[li],
					    decl_list,
					    wm_decl[li],
					    decl_n,
					    true,
					    "goto '%.*s' loops over a variable-length array "
					    "declaration; each iteration allocates a new VLA "
					    "without freeing the previous one, causing "
					    "unbounded stack growth");
				}
				break;
			}
			case P1K_SWITCH: {
				int si = (int)ents[i].scope_id - sw_min_sid;
				if (sw_defer_wm && si >= 0 && si < sw_sz) {
					sw_defer_wm[si] = defer_n;
					sw_decl_wm[si] = decl_n;
				}
				break;
			}
			case P1K_CASE: {
				uint16_t sw_sid = ents[i].kase.switch_scope_id;
				// (Phase 1D records sw_sid=0 only when p1d_switch_top==0).
				if (sw_sid == 0)
					pparse_error_tok(p1_tok(&ents[i]),
						  "case/default label outside any switch statement");
				{
					uint16_t case_se = pparse_scope_stmt_expr_ancestor(ents[i].scope_id);
					if (case_se != 0 && sw_sid < pparse_scope_tree_count &&
					    !pparse_scope_is_ancestor_or_self(case_se, sw_sid))
						pparse_error_tok(p1_tok(&ents[i]),
							  "case/default label inside a statement expression "
							  "(jumping into ({...}) is undefined behavior)");
				}

				int sw_i = (int)sw_sid - sw_min_sid;
				if (!sw_defer_wm || sw_i < 0 || sw_i >= sw_sz) break;
				int sw_dm = sw_defer_wm[sw_i];
				int sw_cm = sw_decl_wm[sw_i];
				if (has_defer) {
					for (int di = sw_dm; di < defer_n; di++) {
						P1FuncEntry *d = &ents[defer_list[di]];
						if (!pparse_scope_is_ancestor_or_self(d->scope_id, ents[i].scope_id))
							continue;
						if (d->scope_id > 0 && d->scope_id < pparse_scope_tree_count &&
						    pparse_scope_close(&pparse_scope_tree[d->scope_id]) <
							ents[i].token_index)
							continue;
						pparse_error_tok(p1_tok(d),
							  "defer skipped by switch fallthrough at %s:%d",
							  pparse_tok_file(p1_tok(&ents[i]))->name,
							  pparse_tok_line_no(p1_tok(&ents[i])));
					}
				}

				// Decl bypass: VLA skip is always fatal (C99/C11 6.8.6.1);
				for (int di = sw_cm; di < decl_n; di++) {
					P1FuncEntry *d = &ents[decl_list[di]];
					if (!pparse_scope_is_ancestor_or_self(d->scope_id, ents[i].scope_id))
						continue;
					{
						uint32_t close = decl_effective_close(d);
						if (close > 0 && close < ents[i].token_index) continue;
					}
					if (d->decl.is_vla)
						pparse_error_tok(p1_tok(&ents[i]),
							  "case/default label may bypass VLA declaration");
					if (d->decl.has_raw || d->decl.is_static_storage) continue;
					if (d->decl.has_init)
						SAFETY_DIAG(p1_tok(&ents[i]),
							    "case/default label may bypass declaration with "
							    "initializer (undefined if jumped into)");
					if (!has_zeroinit) continue;
					SAFETY_DIAG(
					    p1_tok(&ents[i]),
					    "case/default label inside a nested block within a switch "
					    "may bypass zero-initialization (move the label to the "
					    "switch body or wrap in its own block)");
				}
				break;
			}
			} // switch
		} // sweep

		{
			for (int s = 0; s <= hash_mask; s++) {
				int fi = fwd_hash_tbl[s];
				if (fi >= 0) {
					pparse_error_tok(p1_tok(&ents[fwd[fi].idx]),
						  "goto target label '%.*s' not found in scope",
						  ents[fwd[fi].idx].label.len,
						  ents[fwd[fi].idx].label.name);
					break;
				}
			}
		}

		pparse_arena_restore(&_pc->main_arena, mark);
	} // per-function
}

/* Tokenization already classified every spelling that changes context. */
static unsigned pparse_context_intro(PParseToken *t) {
	if (!t) return 0;
	uint32_t tag = t->tag;
	uint16_t flags = t->flags;
	unsigned uneval = ((flags & (PPARSE_TF_SIZEOF | PPARSE_TF_STATIC_ASSERT)) != 0) |
			   ((tag & (PPARSE_TT_TYPEOF | PPARSE_TT_GENERIC)) != 0);
	unsigned type_ctor = ((tag & (PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS)) != 0) |
			     ((tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER)) ==
			      (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER));
	return uneval * PPARSE_CI_UNEVAL |
	       (unsigned)((flags & PPARSE_TF_ALIGNOF) != 0) * PPARSE_CI_ALIGNOF |
	       (unsigned)((flags & PPARSE_TF_OFFSETOF) != 0) * PPARSE_CI_OFFSETOF |
	       type_ctor * PPARSE_CI_TYPE_CTOR;
}

static bool pparse_analyze(PParseToken *tok) {
	PPARSE_CTX();
	pparse_reset();
	pparse_build_scopes(tok);
	p1_full_depth_prescan(tok);
	bool has_bounds_helper =
	    pparse_finalize(pparse_feat(PPARSE_F_BOUNDS_CHECK) ? "__prism_bchk" : NULL);
	p1_verify_cfg();
	_pc->parses_frozen = true;
	return has_bounds_helper;
}

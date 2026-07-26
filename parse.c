
/*
 * Prism Parse (pparse), reusable C parse library.
   Parsing full C89/C11/C17/C23 sources and supports the Prism dialect extensions (defer, orelse, raw, etc.).
 */

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

/* ── memory policy (was mem.c) ──────────────────────────────────────────
 *
 * Every Prism memory operation names whether its byte count is a runtime
 * decision or a compile-time constant:
 *
 *   prism_*_runtime   unbounded runtime size; lowers to compiler builtins.
 *   prism_*_static    n must be a compile-time constant; PRISM_MEM_*_EXACT
 *                     gives exact-width inline load/store with no size ladder.
 *
 * Vague soft names (prism_eq / prism_copy / prism_zero) are intentionally not
 * the API — call sites pick the tier explicitly.
 *
 * This used to be mem.c, which also carried an opt-in "bounded" tier: no-libc
 * unrolled byte ladders behind -DPRISM_MEM_KIT=1.  A coverage audit found that
 * tier was 285 of its 297 instrumented lines dead — compiled into every build
 * and called by none of them — and that -DPRISM_MEM_KIT=1 had not compiled
 * since PRISM_LIKELY was renamed to pparse_PRISM_LIKELY, because no build
 * turns it on.  Untested, unbuildable, unreachable: removed.  What survives is
 * what the default build actually used, and its codegen is unchanged.
 */

#ifndef PRISM_HAS_BUILTIN
#ifdef __has_builtin
#define PRISM_HAS_BUILTIN(x) __has_builtin(x)
#else
#define PRISM_HAS_BUILTIN(x) 0
#endif
#endif

#ifndef PRISM_REQUIRE_CONST
#if defined(__clang__) || defined(__GNUC__)
#define PRISM_REQUIRE_CONST(n)                                                                               \
	do {                                                                                                 \
		_Static_assert(__builtin_constant_p(n), "prism_*_static requires a compile-time constant n"); \
	} while (0)
#else
#define PRISM_REQUIRE_CONST(n) ((void)0)
#endif
#endif

/* Exact-width copy/set for compile-time-constant n: inline load/store, never a
 * runtime size ladder.  Falls back to an unrolled byte loop without the
 * builtin — still no libc call. */
#if PRISM_HAS_BUILTIN(__builtin_memcpy_inline)
#define PRISM_MEM_COPY_EXACT(dst, src, n) __builtin_memcpy_inline((dst), (src), (n))
#else
#define PRISM_MEM_COPY_EXACT(dst, src, n)                                                                    \
	do {                                                                                                 \
		unsigned char *_d = (unsigned char *)(dst);                                                  \
		const unsigned char *_s = (const unsigned char *)(src);                                      \
		for (unsigned _i = 0; _i < (unsigned)(n); _i++) _d[_i] = _s[_i];                             \
	} while (0)
#endif

#if PRISM_HAS_BUILTIN(__builtin_memset_inline)
#define PRISM_MEM_SET_EXACT(dst, v, n) __builtin_memset_inline((dst), (v), (n))
#else
#define PRISM_MEM_SET_EXACT(dst, v, n)                                                                       \
	do {                                                                                                 \
		unsigned char *_d = (unsigned char *)(dst);                                                  \
		unsigned char _b = (unsigned char)(v);                                                        \
		for (unsigned _i = 0; _i < (unsigned)(n); _i++) _d[_i] = _b;                                 \
	} while (0)
#endif

static inline PRISM_ALWAYS_INLINE void *prism__mem_ret(void *p) {
	return p;
}

/* ── runtime tier (unbounded n; builtins OK) ───────────────────────── */

static inline PRISM_ALWAYS_INLINE void *prism_memcpy_runtime(void *restrict dst, const void *restrict src,
							     size_t n) {
#if defined(__clang__) || defined(__GNUC__)
	return __builtin_memcpy(dst, src, n);
#else
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	while (n--) *d++ = *s++;
	return dst;
#endif
}

static inline PRISM_ALWAYS_INLINE void *prism_memset_runtime(void *dst, int v, size_t n) {
#if defined(__clang__) || defined(__GNUC__)
	return __builtin_memset(dst, v, n);
#else
	unsigned char *d = (unsigned char *)dst;
	unsigned char b = (unsigned char)v;
	while (n--) *d++ = b;
	return dst;
#endif
}

static inline PRISM_ALWAYS_INLINE void *prism_memzero_runtime(void *dst, size_t n) {
	return prism_memset_runtime(dst, 0, n);
}

static inline PRISM_ALWAYS_INLINE int prism_memcmp_runtime(const void *a, const void *b, size_t n) {
#if defined(__clang__) || defined(__GNUC__)
	return __builtin_memcmp(a, b, n);
#else
	const unsigned char *pa = (const unsigned char *)a;
	const unsigned char *pb = (const unsigned char *)b;
	while (n--) {
		unsigned char av = *pa++, bv = *pb++;
		if (av != bv) return (int)av - (int)bv;
	}
	return 0;
#endif
}

static inline PRISM_ALWAYS_INLINE void *prism_memmove_runtime(void *dst, const void *src, size_t n) {
#if defined(__clang__) || defined(__GNUC__)
	return __builtin_memmove(dst, src, n);
#else
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	if (d == s || n == 0) return dst;
	if ((uintptr_t)d < (uintptr_t)s) {
		while (n--) *d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--) *--d = *--s;
	}
	return dst;
#endif
}

/* ── static tier (compile-time-constant n) ─────────────────────────── */

#define prism_memcpy_static(dst, src, n)                                                                     \
	__extension__({                                                                                      \
		PRISM_REQUIRE_CONST(n);                                                                      \
		void *_prism_dst = (dst);                                                                    \
		PRISM_MEM_COPY_EXACT(_prism_dst, (src), (n));                                                \
		prism__mem_ret(_prism_dst);                                                                  \
	})
#define prism_memset_static(dst, v, n)                                                                       \
	__extension__({                                                                                      \
		PRISM_REQUIRE_CONST(n);                                                                      \
		void *_prism_dst = (dst);                                                                    \
		PRISM_MEM_SET_EXACT(_prism_dst, (v), (n));                                                   \
		prism__mem_ret(_prism_dst);                                                                  \
	})
#define prism_memzero_static(dst, n)                                                                         \
	__extension__({                                                                                      \
		PRISM_REQUIRE_CONST(n);                                                                      \
		void *_prism_dst = (dst);                                                                    \
		PRISM_MEM_SET_EXACT(_prism_dst, 0, (n));                                                     \
		prism__mem_ret(_prism_dst);                                                                  \
	})
#define prism_memcmp_static(a, b, n) prism_memcmp_runtime((a), (b), (size_t)(n))

/* ── equality helpers that still name the tier ─────────────────────── */

#define prism_memeq_static(a, b, n) (prism_memcmp_static((a), (b), (n)) == 0)
#define prism_memeq_runtime(a, b, n) (prism_memcmp_runtime((a), (b), (n)) == 0)
#define prism_memeq_runtime_sized(a, b, n) ((n) == 0 || prism_memcmp_runtime((a), (b), (n)) == 0)
#define prism_memcpy_runtime_sized(dst, src, n) prism_memcpy_runtime((dst), (src), (n))

#define PPARSE_ENTRY_MATCHES(ent, k, kl)                                                                            \
	((ent)->key && (ent)->key != PPARSE_TOMBSTONE && (ent)->key_len == (kl) &&                                   \
	 prism_memeq_runtime_sized((ent)->key, (k), (size_t)(kl)))
#define PPARSE_IS_DIGIT(c) ((unsigned)(c) - '0' < 10u)
#define PPARSE_IS_ALPHA(c) (((unsigned)((c) | 0x20) - 'a') < 26u || (c) == '_' || (c) == '$')
#define PPARSE_IS_ALNUM(c) (PPARSE_IS_DIGIT(c) || PPARSE_IS_ALPHA(c))
#define PPARSE_IS_XDIGIT(c) (PPARSE_IS_DIGIT(c) || ((unsigned)((c) | 0x20) - 'a') < 6u)
#define PPARSE_ARENA_DEFAULT_BLOCK_SIZE (64 * 1024)
#define PPARSE_KW_MARKER 0x80000000ULL // Internal marker bit for keyword map: values are (tag | PPARSE_KW_MARKER)
#define PPARSE_KW_FLAGS_SHIFT 32	// Extra token flags encoded in bits 32-47 of keyword value

// Centralized diagnostic strings. Many appear at multiple Pass 2 emit /
static const char PPARSE_ERR_ORELSE_STMT_LEVEL[] = "'orelse' cannot be used here (it must appear at the "
					    "statement level in a declaration or bare expression)";
/* Canonical stray-'defer' diagnostic: a defer keyword in any position that is
 * not statement-level (declarator/argument/dimension interiors, sizeof and
 * _Static_assert operands, expression positions).  A single wording keeps the
 * message stable across the many Phase-1 sites that detect it.  Contains both
 * "expression context" and "parenthesized" so context-agnostic callers'
 * expectations hold. */
static const char PPARSE_ERR_DEFER_EXPR_CTX[] =
    "'defer' cannot be used in expression context (array dimensions, parenthesized "
    "expressions, function arguments, sizeof/_Static_assert operands, etc.); move it to "
    "statement position";
static const char PPARSE_ERR_BARE_ORELSE_SPANS_PP[] = "bare orelse assignment cannot be used when the "
					       "expression spans preprocessor conditionals — the "
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
static const char PPARSE_ERR_CONST_UNAVOIDABLE_MEMSET[] = "'const' variable requiring unavoidable memset "
						   "(union, VLA, or _Atomic aggregate) cannot be safely "
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

#if defined(_MSC_VER)
#define PPARSE_ARENA_ALIGN 8
#else
#define PPARSE_ARENA_ALIGN (__alignof__(long double))
#endif

#define pparse_equal(                                                                                                                     \
    tok,                                                                                                                           \
    s) /* known-length strings of 1/2 bytes use branchless comparisons; others use memcmp. Runtime strings fall back to strlen. */ \
	(__builtin_constant_p(s) ? (__builtin_strlen(s) == 1   ? pparse_equal_1(tok, (s)[0])                                             \
				    : __builtin_strlen(s) == 2 ? pparse_equal_2(tok, s)                                                  \
							       : pparse_equal_n(tok, s, (uint32_t)__builtin_strlen(s)))                   \
				 : pparse_equal_n(tok, s, (uint32_t)strlen(s)))

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

/* need = minimum used count the array must hold (typically count or count+1). */
#define pparse_VEC_ENSURE_REALLOC(arr, need, cap, init_cap)                                                         \
	do {                                                                                                 \
		if ((size_t)(need) > (size_t)(cap)) {                                                        \
			size_t _new_cap = pparse_vec_grow_cap((size_t)(cap), (size_t)(need), (size_t)(init_cap));   \
			if (_new_cap > SIZE_MAX / sizeof(*(arr))) pparse_error("allocation overflow");              \
			void *_tmp = realloc((arr), sizeof(*(arr)) * _new_cap);                              \
			if (!_tmp) pparse_error("out of memory");                                                   \
			(arr) = _tmp;                                                                        \
			(cap) = _new_cap;                                                                    \
		}                                                                                            \
	} while (0)

#define PPARSE_ARENA_ENSURE_CAP(arena, arr, count, cap, init_cap, T)                                                \
	do {                                                                                                 \
		if ((size_t)(count) >= (size_t)(cap)) {                                                      \
			size_t old_cap = (size_t)(cap);                                                      \
			size_t new_cap = pparse_vec_grow_cap(old_cap, (size_t)(count), (size_t)(init_cap));         \
			if (new_cap > SIZE_MAX / sizeof(T)) pparse_error("allocation overflow");                    \
			(arr) = pparse_arena_realloc((arena), (arr), sizeof(T) * old_cap, sizeof(T) * new_cap);     \
			(cap) = new_cap;                                                                     \
		}                                                                                            \
	} while (0)

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
	char *contents;
	size_t contents_len;
	int file_no;
	int line_delta;
	bool owns_contents;
	bool is_system;
	bool is_direct_system_include; // Entered from non-system (re-emit as #include)
	bool skip_emit; // Precomputed is_system && is_include_entry hot-path predicate
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
	PPARSE_TF_IS_FLOAT = 1 << 2,
	PPARSE_TF_OPEN = 1 << 3,	   // Opening delimiter: ( [ {
	PPARSE_TF_CLOSE = 1 << 4,	   // Closing delimiter: ) ] }
	PPARSE_TF_C23_ATTR = 1 << 5,	   // First '[' of C23 [[ ... ]] attribute
	PPARSE_TF_RAW = 1 << 6,	   // 'raw' keyword
	PPARSE_TF_SIZEOF = 1 << 7,	   // sizeof, alignof, _Alignof
	PPARSE_TF_SOFT_KW = 1 << 8,	   // soft keyword usable as identifier (alignas, bool, …)
	PPARSE_TF_STATIC_ASSERT = 1 << 9, // _Static_assert / static_assert
	PPARSE_TF_MS_CC = 1 << 10,	   // MSVC calling-convention keyword (__cdecl, …)
	PPARSE_TF_SYS_SKIP = 1 << 11,	   // token belongs to a system #include entry file; in
				   // non-flatten emit it is skipped verbatim. Precomputed at
				   // pparse_tokenize from current_file so the hot emit loop tests one
				   // flag bit instead of a per-token pparse_tok_cold + file lookup.
	PPARSE_TF_HAS_PRISM = 1 << 12,	   // matched group contains a defer/orelse token
	PPARSE_TF_LINK_JUMP = 1 << 13,	   // parse_data is a non-adjacent next-token index
};

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

#define PPARSE_TT_DECL_START                                                                                        \
	(PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_INLINE | PPARSE_TT_ALIGNAS | PPARSE_TT_SKIP_DECL | PPARSE_TT_ATTR)

struct PParseToken {
	uint32_t tag;	    // TT_* bitmask - token classification
	uint32_t parse_data; // Phase-local name resolution/scope ID, or PPARSE_TF_LINK_JUMP target
	uint32_t match_idx; // Paired-delimiter index, otherwise source offset
	uint32_t len;	    // PParseToken length in bytes (must handle >65535 for large literals)
	uint8_t kind;
	uint8_t _flags_pad; // Align flags to 2 bytes (keeps PParseToken at 24)
	uint16_t flags;	    // TF_* bitmask (PPARSE_TF_SOFT_KW needs bit 8)
	uint16_t ann;	    // Pass 1 annotation flags (P1_SCOPE_*, P1_OE_*, P1_IS_DECL, …)
	uint8_t ch0;	    // First source byte — avoids pparse_loc() indirection in hot paths
	uint8_t _pad;	    // Explicit padding to 24 bytes
}; // 24 bytes

typedef char prism_assert_token_24[(sizeof(struct PParseToken) == 24) ? 1 : -1];

typedef struct {
	uint32_t loc_offset; // Byte offset from PParseFile->contents
	int32_t line_no : 18;
	uint32_t file_idx : 14;
} PParseTokenCold; // 8 bytes — pparse_error/debug path

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
	char *name;
	uint64_t value;
	uint8_t len;
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
	PPARSE_F_BOUNDS_CHECK = 256
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
	size_t default_block_size;
} PParseArena;

typedef struct {
	bool at_bol;
	bool has_space;
	int line_no;
} PParseTokState;

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
	int error_col;
#endif
	uint32_t features; // PPARSE_F_DEFER | PPARSE_F_ZEROINIT | PPARSE_F_LINE_DIR | PPARSE_F_WARN_SAFETY | PPARSE_F_FLATTEN | PPARSE_F_ORELSE
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
	PParseToken *func_ret_type_start;	   // First token of return type (after storage/function specifiers)
	PParseToken *func_ret_type_end;	   // Function name token (exclusive end of return type range)
	PParseToken *func_ret_type_suffix_start; // For complex declarators: closing ')' after func params
	PParseToken *func_ret_type_suffix_end;   // For complex declarators: token after suffix (exclusive)
	unsigned *bracket_oe_ids;	   // Pre-assigned temp IDs for bracket orelse hoisting (dynamic)
	int bracket_oe_count;		   // Count of hoisted bracket orelse temps
	int bracket_oe_cap;		   // Capacity of bracket_oe_ids array
	int bracket_oe_next;		   // Next temp to consume during emit

	PParseToken **typeof_vars;
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
	PParseToken *tp_pool;	    // Hot: tag, parse_data, match_idx, len, kind, flags
	PParseTokenCold *tp_cold; // Cold: loc_offset, line_no, file_idx
	char *token_source; // Shared backing buffer for match_idx source offsets
	uint32_t tp_count;  // Next free index. 0 reserved as NULL sentinel.
	uint32_t tp_cap;
	uint32_t pparse_token_tag_summary; // OR of PPARSE_TT_* tags in the current token stream
	PParseKeywordEntry kw_cache[256];
	uint32_t keyword_cache_features; // features used when pparse_keyword_cache was built

	// Digraph normalization targets (per-context for token loc comparison)
	char dg_bracket_open[2];
	char dg_bracket_close[2];
	char dg_brace_open[2];
	char dg_brace_close[2];
	char dg_hash[2];
	char dg_paste[3];
	void *p1_scope_tree; // PParseScopeInfo[] — flat array indexed by scope_id
	uint16_t p1_scope_count;
	/* Capacity, NOT a scope_id: scope_count maxes at 65534 (uint16) but the
	 * doubling cap reaches 65536, which must not truncate to 0 — that would
	 * pass old_size=0 to pparse_arena_realloc and drop the whole scope tree. */
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
} PParseContext;

static PRISM_THREAD_LOCAL PParseContext *pparse_ctx = NULL;

static inline bool pparse_is_digraph_loc(char *loc) {
	return loc == pparse_ctx->dg_bracket_open || loc == pparse_ctx->dg_bracket_close || loc == pparse_ctx->dg_brace_open ||
	       loc == pparse_ctx->dg_brace_close || loc == pparse_ctx->dg_hash || loc == pparse_ctx->dg_paste;
}

#define pparse_token_pool (pparse_ctx->tp_pool)
#define pparse_token_cold (pparse_ctx->tp_cold)
#define pparse_token_count (pparse_ctx->tp_count)
#define pparse_token_cap (pparse_ctx->tp_cap)
#define pparse_token_tag_summary (pparse_ctx->pparse_token_tag_summary)
#define pparse_keyword_cache (pparse_ctx->kw_cache)
#define pparse_digraph_norm_bracket_open (pparse_ctx->dg_bracket_open)
#define pparse_digraph_norm_bracket_close (pparse_ctx->dg_bracket_close)
#define pparse_digraph_norm_brace_open (pparse_ctx->dg_brace_open)
#define pparse_digraph_norm_brace_close (pparse_ctx->dg_brace_close)
#define pparse_digraph_norm_hash (pparse_ctx->dg_hash)
#define pparse_digraph_norm_paste (pparse_ctx->dg_paste)

static PRISM_COLD noreturn void pparse_error(char *fmt, ...);
static void pparse_hashmap_put(PParseHashMap *map, char *key, int keylen, void *val);
static void pparse_hashmap_remove(PParseHashMap *map, char *key, int keylen);

static inline bool pparse_at_bol(PParseToken *tok) {
	return tok->flags & PPARSE_TF_AT_BOL;
}

static PParseArenaBlock *pparse_arena_new_block(size_t min_size, size_t default_size) {
	size_t capacity = default_size;
	if (min_size > capacity) capacity = min_size;
	PParseArenaBlock *block = malloc(sizeof(PParseArenaBlock) + capacity);
	if (!block) pparse_error("out of memory allocating arena block");
	block->next = NULL;
	block->used = 0;
	block->capacity = capacity;
	return block;
}

static void pparse_arena_ensure(PParseArena *arena, size_t size) {
	if (arena->current && arena->current->used + size <= arena->current->capacity) return;
	if (arena->current && arena->current->next && size <= arena->current->next->capacity) {
		arena->current = arena->current->next;
		arena->current->used = 0;
		return;
	}
	size_t block_size = arena->default_block_size ? arena->default_block_size : PPARSE_ARENA_DEFAULT_BLOCK_SIZE;
	PParseArenaBlock *block = pparse_arena_new_block(size, block_size);
	if (arena->current) {
		block->next = arena->current->next;
		arena->current->next = block;
	} else
		arena->head = block;
	arena->current = block;
}

static void *pparse_arena_alloc_uninit(PParseArena *arena, size_t size) {
	if (size == 0) size = 1;
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
	if (new_size <= old_size) return old;
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
	if (old && old_size > 0) memcpy(p, old, old_size);
	memset((char *)p + old_size, 0, new_size - old_size);
	return p;
}

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

static void pparse_arena_free(PParseArena *arena) {
	PParseArenaBlock *b = arena->head;
	while (b) {
		PParseArenaBlock *next = b->next;
		free(b);
		b = next;
	}
	arena->head = NULL;
	arena->current = NULL;
}

static void pparse_ctx_init(void) {
	if (pparse_ctx) return;
	PParseContext *c = calloc(1, sizeof(PParseContext));
	if (!c) {
		fprintf(stderr, "prism: out of memory\n");
		exit(1);
	}
	c->main_arena.default_block_size = PPARSE_ARENA_DEFAULT_BLOCK_SIZE;
	c->features = PPARSE_F_DEFER | PPARSE_F_ZEROINIT | PPARSE_F_LINE_DIR | PPARSE_F_FLATTEN | PPARSE_F_ORELSE;
	c->at_stmt_start = true;
	c->tp_count = 1; // 0 reserved as NULL sentinel

	memcpy(c->dg_bracket_open, "[", 2);
	memcpy(c->dg_bracket_close, "]", 2);
	memcpy(c->dg_brace_open, "{", 2);
	memcpy(c->dg_brace_close, "}", 2);
	memcpy(c->dg_hash, "#", 2);
	memcpy(c->dg_paste, "##", 3);
	pparse_ctx = c;
}

static void pparse_token_pool_ensure(size_t need) {
	if (need <= pparse_token_cap) return;
	size_t new_cap = pparse_vec_grow_cap(pparse_token_cap, need, 65536);
	if (new_cap > (uint32_t)-1 || new_cap > SIZE_MAX / sizeof(PParseToken))
		pparse_error("token pool capacity exceeded");
	PParseToken *p = realloc(pparse_token_pool, new_cap * sizeof(PParseToken));
	if (!p) pparse_error("out of memory allocating token pool");
	pparse_token_pool = p;
	PParseTokenCold *c = realloc(pparse_token_cold, new_cap * sizeof(PParseTokenCold));
	if (!c) pparse_error("out of memory allocating token cold pool");
	pparse_token_cold = c;
	pparse_token_cap = (uint32_t)new_cap;
	// Pool index 0 is the NULL sentinel and must never look like a real token.
	if (pparse_token_count <= 1) {
		memset(&pparse_token_pool[0], 0, sizeof(PParseToken));
		memset(&pparse_token_cold[0], 0, sizeof(PParseTokenCold));
	}
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE PParseTokenCold *pparse_tok_cold(PParseToken *tok) {
	uintptr_t bd = (uintptr_t)tok - (uintptr_t)pparse_token_pool;
#if defined(__SIZEOF_INT128__)
	uint64_t hi = (uint64_t)(((__uint128_t)bd * 0xAAAAAAAAAAAAAAABULL) >> 64);
	uint64_t idx = hi >> 4;
#else
	uint64_t idx = (uint64_t)bd / 24u;
#endif
	return &pparse_token_cold[idx];
}

static inline PRISM_PURE char *pparse_loc(PParseToken *tok) {
	if (!(tok->flags & (PPARSE_TF_OPEN | PPARSE_TF_CLOSE))) return pparse_ctx->token_source + tok->match_idx;
	PParseTokenCold *c = pparse_tok_cold(tok);
	return pparse_ctx->input_files[c->file_idx]->contents + c->loc_offset;
}

static inline PRISM_PURE PParseToken *pparse_next(PParseToken *tok) {
	if (tok->kind == PPARSE_TK_EOF) return NULL;
	if (__builtin_expect(!(tok->flags & PPARSE_TF_LINK_JUMP), 1)) return tok + 1;
	return &pparse_token_pool[tok->parse_data];
}

static inline PRISM_PURE PParseToken *pparse_pair(PParseToken *tok) {
	return tok->match_idx ? &pparse_token_pool[tok->match_idx] : NULL;
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE uint32_t pparse_idx(PParseToken *tok) {
	if (!tok) return 0;
	uintptr_t bd = (uintptr_t)tok - (uintptr_t)pparse_token_pool;
#if defined(__SIZEOF_INT128__)
	uint64_t hi = (uint64_t)(((__uint128_t)bd * 0xAAAAAAAAAAAAAAABULL) >> 64);
	return (uint32_t)(hi >> 4);
#else
	return (uint32_t)((uint64_t)bd / 24u);
#endif
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

static void pparse_hashmap_resize(PParseHashMap *map, int newcap) {
	PParseHashMap new_map = {.buckets = pparse_arena_alloc(&pparse_ctx->main_arena, (size_t)newcap * sizeof(PParseHashEntry)),
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
	if (__builtin_expect(!map->buckets, 0)) {
		map->buckets = pparse_arena_alloc(&pparse_ctx->main_arena, 64 * sizeof(PParseHashEntry));
		map->capacity = 64;
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
	if (!map->buckets) return NULL;
	return pparse_hashmap_get_hashed(map, pparse_loc(tok), tok->len, tok->parse_data);
}

static inline void pparse_lex_token_map_put(PParseHashMap *map, PParseToken *tok, void *value) {
	pparse_hashmap_put_hashed(map, pparse_loc(tok), tok->len, value, tok->parse_data);
}

static void pparse_hashmap_discard(PParseHashMap *map) {
	*map = (PParseHashMap){0};
}

static char *pparse_intern_filename(const char *name) {
	if (!name) return NULL;
	size_t len = strlen(name) + 1;
	char *copy = pparse_arena_alloc_uninit(&pparse_ctx->main_arena, len);
	return memcpy(copy, name, len);
}

static inline PParseFile *pparse_tok_file(PParseToken *tok) {
	if (!tok) return pparse_ctx->current_file;
	PParseTokenCold *c = pparse_tok_cold(tok);
	if (c->file_idx >= (uint32_t)pparse_ctx->input_file_count) return pparse_ctx->current_file;
	return pparse_ctx->input_files[c->file_idx];
}

static int pparse_tok_line_no(PParseToken *tok) {
	return pparse_tok_cold(tok)->line_no;
}

#ifdef PRISM_LIB_MODE
static noreturn void pparse_lib_error_jump(int line) {
	pparse_ctx->error_line = line;
	longjmp(pparse_ctx->error_jmp, 1);
}

static inline bool pparse_lib_error_enabled(void) {
	return pparse_ctx && pparse_ctx->error_jmp_set;
}

static noreturn void pparse_lib_errorf(int line, const char *fmt, va_list ap) {
	vsnprintf(pparse_ctx->error_msg, sizeof(pparse_ctx->error_msg), fmt, ap);
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
	// Digraph locs point to static storage; avoid UB from cross-object pointer comparison
	if (!input || !loc || line_no <= 0 || pparse_is_digraph_loc(loc)) {
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
	va_list ap;
	va_start(ap, fmt);
#ifdef PRISM_LIB_MODE
	int line = pparse_ctx->current_file && pparse_ctx->current_file->contents && !pparse_is_digraph_loc(loc)
		       ? pparse_count_lines(pparse_ctx->current_file->contents, loc)
		       : 0;
	if (pparse_lib_error_enabled()) pparse_lib_errorf(line, fmt, ap);
#endif
	if (pparse_ctx->current_file)
		pparse_verror_at(pparse_ctx->current_file->name,
			  pparse_ctx->current_file->contents,
			  pparse_is_digraph_loc(loc) ? 0 : pparse_count_lines(pparse_ctx->current_file->contents, loc),
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
	va_list ap;
	va_start(ap, fmt);
	PParseFile *f = pparse_tok_file(tok);
#ifdef PRISM_LIB_MODE
	if (pparse_lib_error_enabled()) pparse_lib_errorf(pparse_tok_line_no(tok), fmt, ap);
#endif
	pparse_verror_at(f->name, f->contents, pparse_tok_line_no(tok), pparse_loc(tok), "pparse_error", fmt, ap);
	va_end(ap);
	exit(1);
}

static void pparse_warn_tok(PParseToken *tok, const char *fmt, ...) {
#ifdef PRISM_LIB_MODE
	(void)tok;
	(void)fmt;
	return; // Suppress warnings in library mode
#else
	va_list ap;
	va_start(ap, fmt);
	PParseFile *f = pparse_tok_file(tok);
	pparse_verror_at(f->name, f->contents, pparse_tok_line_no(tok), pparse_loc(tok), "warning", fmt, ap);
	va_end(ap);
#endif
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool pparse_equal_n(PParseToken *tok, const char *op, uint32_t len) {
	/* Operator/keyword spellings are short; caller proves len fits the bound. */
	return tok->len == len && tok->ch0 == (uint8_t)op[0] &&
	       (len <= 1 || prism_memeq_runtime(pparse_loc(tok) + 1, op + 1, (size_t)len - 1u));
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool pparse_equal_1(PParseToken *tok, char c) {
	return tok->len == 1 && tok->ch0 == (uint8_t)c;
}

#define pparse_match_ch pparse_equal_1

#define pparse_CH(c) (1ULL << ((c) - 32))
#define pparse_match_set(tok, mask)                                                                                 \
	((tok)->len == 1 && (unsigned)((tok)->ch0 - 32) < 64u && ((mask) & (1ULL << ((tok)->ch0 - 32))))

// Handles _Pragma(...), __attribute__((...)), C23 [[...]], and #pragma directives
static inline bool pparse_is_stmt_expr_open(PParseToken *t) {
	if (!pparse_match_ch(t, '(')) return false;
	PParseToken *n = pparse_next(t);
	while (n && n->kind != PPARSE_TK_EOF) {
		if (n->kind == PPARSE_TK_PREP_DIR) {
			n = pparse_next(n);
			continue;
		}
		if ((n->tag & PPARSE_TT_ATTR) && pparse_next(n) && pparse_match_ch(pparse_next(n), '(') &&
		    pparse_pair(pparse_next(n))) {
			n = pparse_next(pparse_pair(pparse_next(n)));
			continue;
		}
		if (n->flags & PPARSE_TF_C23_ATTR) {
			PParseToken *close = pparse_pair(n);
			if (close) {
				n = pparse_next(close);
				continue;
			}
		}
		break;
	}
	return n && pparse_match_ch(n, '{');
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool pparse_is_else_kw(PParseToken *t) {
	return (t->tag & PPARSE_TT_IF) && t->ch0 == 'e';
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool pparse_is_do_kw(PParseToken *t) {
	return (t->tag & PPARSE_TT_LOOP) && t->ch0 == 'd';
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool pparse_is_else_or_do(PParseToken *t) {
	return pparse_is_else_kw(t) || pparse_is_do_kw(t);
}

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool pparse_equal_2(PParseToken *tok, const char *s) {
	if (tok->len != 2 || tok->ch0 != (uint8_t)s[0]) return false;
	return pparse_loc(tok)[1] == s[1];
}

static inline PRISM_PURE bool pparse_is_gnu_label_decl_head(PParseToken *tok) {
	return tok && tok->len == 9 && tok->ch0 == '_' && prism_memeq_static(pparse_loc(tok), "__label__", 9);
}

static inline PRISM_PURE uint64_t pparse_keyword_lookup(char *key, int keylen) {
	if (keylen < 2) return 0;
	unsigned slot = pparse_KEYWORD_HASH(key, keylen);
	for (int i = 0; i < 32; i++) {
		PParseKeywordEntry *ent = &pparse_keyword_cache[(slot + i) & 255];
		if (!ent->name) return 0;
		if (ent->len == keylen && prism_memeq_runtime_sized(ent->name, key, (uint32_t)keylen)) return ent->value;
	}
	return 0;
}

static inline PRISM_PURE bool pparse_is_potential_func_name(PParseToken *tok) {
	PParseToken *next = pparse_next(tok);
	return tok->kind <= PPARSE_TK_KEYWORD && next && next->ch0 == '(' && (next->flags & PPARSE_TF_OPEN) &&
	       !(tok->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_ATTR));
}

static void pparse_init_keyword_map(void) {
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
	    {"alignof", PPARSE_TT_SKIP_DECL, true, PPARSE_TF_SIZEOF | PPARSE_TF_SOFT_KW},
	    {"_Alignof", PPARSE_TT_SKIP_DECL, true, PPARSE_TF_SIZEOF},
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
	    {"__builtin_offsetof", 0, true, PPARSE_TF_SIZEOF},
	    {"offsetof", 0, true, PPARSE_TF_SIZEOF | PPARSE_TF_SOFT_KW},
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
		unsigned slot = pparse_KEYWORD_HASH(entries[i].name, len);
		while (pparse_keyword_cache[slot & 255].name) slot++;
		pparse_keyword_cache[slot & 255] = (PParseKeywordEntry){.name = entries[i].name, .value = val, .len = len};
	}
	pparse_ctx->keyword_cache_features = pparse_ctx->features;
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

static int pparse_read_ident(char *start) {
	char *p = start;
	int ucn = pparse_read_ucn(p);
	if (ucn)
		p += ucn;
	else if ((unsigned char)*p >= 0x80)
		p++;
	else if (PPARSE_IS_ALPHA(*p))
		p++;
	else
		return 0;
	for (;;) {
		if (pparse_ident_char[(unsigned char)*p]) {
			p++;
			continue;
		}
		ucn = pparse_read_ucn(p);
		if (ucn) {
			p += ucn;
			continue;
		}
		break;
	}
	return p - start;
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
	default: return ((unsigned char)*p > 0x20 && *p != 0x7f && !PPARSE_IS_ALNUM(*p)) ? 1 : 0;
	}
}

static bool pparse_is_space(char c) {
	return c == ' ' || c == '\t' || c == '\f' || c == '\r' || c == '\v';
}

#define pparse_SWAR_HAS_ZERO(v) (((v) - 0x0101010101010101ULL) & ~(v) & 0x8080808080808080ULL)
#define pparse_SWAR_BROADCAST(c) (0x0101010101010101ULL * (uint8_t)(c))

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

static inline __attribute__((always_inline)) PParseToken *
pparse_new_token(PParseTokenKind kind, char *start, char *end, PParseTokState *ts) {
	PParseFile *cf = pparse_ctx->current_file;
	if (pparse_token_count == UINT32_MAX) pparse_error("maximum token limit reached");
	if (__builtin_expect(pparse_token_count >= pparse_token_cap, 0)) pparse_token_pool_ensure(pparse_token_count + 1);
	uint32_t token_idx = pparse_token_count++;
	PParseToken *tok = &pparse_token_pool[token_idx];
	tok->kind = kind;
	tok->len = end - start;
	tok->tag = 0;
	/* file_idx below comes from this same `cf`, so PPARSE_TF_SYS_SKIP exactly mirrors
	 * the emit-time file predicate without a per-token cold-file dereference. */
	tok->flags = (ts->at_bol ? PPARSE_TF_AT_BOL : 0) | (ts->has_space ? PPARSE_TF_HAS_SPACE : 0) |
			     (cf->skip_emit ? PPARSE_TF_SYS_SKIP : 0);
	tok->ann = 0;
	tok->ch0 = (uint8_t)*start;
	PParseTokenCold *c = &pparse_token_cold[token_idx];
	ptrdiff_t off = start - cf->contents;
	if (off < 0 || (size_t)off > (size_t)UINT32_MAX)
		pparse_error_at(start, "source file exceeds 4 GiB; cannot record token locations");
	c->loc_offset = (uint32_t)off;
	tok->match_idx = (uint32_t)off;
	{
		long long ln = (long long)ts->line_no + cf->line_delta;
		int clamped = ln > 0x1FFFF ? 0x1FFFF : (ln < -0x20000 ? -0x20000 : (int)ln);
		c->line_no = clamped;
	}
	c->file_idx = cf->file_no;
	ts->at_bol = ts->has_space = false;
	return tok;
}

static PParseToken *pparse_read_string_literal(char *start, char *quote, PParseTokState *ts) {
	char *end = pparse_string_literal_end(quote + 1);
	return pparse_new_token(PPARSE_TK_STR, start, end + 1, ts);
}

static PParseToken *pparse_read_raw_string_literal(char *start, char *quote, PParseTokState *ts) {
	char *end = pparse_raw_string_literal_end(quote, ts);
	if (!end) pparse_error_at(start, "invalid raw string literal");
	return pparse_new_token(PPARSE_TK_STR, start, end, ts);
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

static inline void pparse_classify_punct(PParseToken *t) {
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
		char *loc = pparse_loc(t);
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

static inline bool pparse_delimiters_match(PParseToken *open, PParseToken *close) {
	char a = open->ch0, b = close->ch0;
	return a == '(' ? b == ')' : b == a + 2;
}

static inline bool pparse_p0_token_can_name_function(PParseToken *tok) {
	return tok && (tok->kind == PPARSE_TK_IDENT || (tok->tag & (PPARSE_TT_DEFER | PPARSE_TT_ORELSE)) ||
		       (tok->flags & (PPARSE_TF_RAW | PPARSE_TF_SOFT_KW)));
}

/* Phase-zero scans run before Prism can splice the logical token stream. */
static inline PParseToken *pparse_p0_next(PParseToken *tok) {
	return tok && tok->kind != PPARSE_TK_EOF ? tok + 1 : NULL;
}

static PParseToken *pparse_p0_attribute_group_end(PParseToken *tok) {
	if (!tok) return NULL;
	if ((tok->flags & PPARSE_TF_C23_ATTR) && pparse_pair(tok)) return pparse_pair(tok);
	if (tok->kind <= PPARSE_TK_KEYWORD &&
	    (pparse_equal(tok, "__attribute__") || pparse_equal(tok, "__attribute") || pparse_equal(tok, "__declspec"))) {
		PParseToken *open = pparse_p0_next(tok);
		if (open && open->ch0 == '(' && pparse_pair(open)) return pparse_pair(open);
	}
	return NULL;
}

static PParseToken *pparse_p0_previous_token(PParseToken *tok) {
	for (uint32_t i = pparse_idx(tok); i > 0;) {
		PParseToken *prev = &pparse_token_pool[--i];
		if (prev->kind != PPARSE_TK_PREP_DIR) return prev;
	}
	return NULL;
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
	if (prev->ch0 == ']' && pparse_pair(prev) && (pparse_pair(prev)->flags & PPARSE_TF_C23_ATTR)) return true;
	return false;
}

static bool pparse_p0_attribute_inside_parameter_list(PParseToken *attr) {
	int depth = 0;
	for (uint32_t i = pparse_idx(attr); i > 0; i--) {
		PParseToken *t = &pparse_token_pool[i - 1];
		if (t->kind == PPARSE_TK_PREP_DIR) continue;
		if (t->flags & PPARSE_TF_CLOSE) {
			depth++;
			continue;
		}
		if (t->flags & PPARSE_TF_OPEN) {
			if (depth > 0) {
				depth--;
				continue;
			}
			if (pparse_match_ch(t, '(') && pparse_p0_token_can_name_function(pparse_p0_previous_token(t))) return true;
			continue;
		}
		if (depth == 0 && (pparse_match_ch(t, ';') || pparse_match_ch(t, '{') || pparse_match_ch(t, '}'))) break;
	}
	return false;
}

static PParseToken *pparse_find_wrapper_callee(PParseToken *body) {
	PParseToken *end = pparse_pair(body);
	if (!end) return NULL;
	PParseToken *tok = pparse_p0_next(body);
	while (tok && tok != end && tok->ch0 == ';') tok = pparse_p0_next(tok);
	if (tok && tok != end && (tok->tag & PPARSE_TT_RETURN)) tok = pparse_p0_next(tok);
	while (tok && tok != end && tok->ch0 == ';') tok = pparse_p0_next(tok);
	if (!tok || tok == end || !pparse_p0_token_can_name_function(tok)) return NULL;
	PParseToken *open = pparse_p0_next(tok);
	if (!open || open->ch0 != '(' || !pparse_pair(open)) return NULL;
	PParseToken *after = pparse_p0_next(pparse_pair(open));
	while (after && after != end && after->ch0 == ';') after = pparse_p0_next(after);
	return after == end ? tok : NULL;
}

static void pparse_add_input_file(PParseFile *file) {
	PPARSE_ARENA_ENSURE_CAP(&pparse_ctx->main_arena,
			 pparse_ctx->input_files,
			 pparse_ctx->input_file_count + 1,
			 pparse_ctx->input_file_capacity,
			 16,
			 PParseFile *);
	pparse_ctx->input_files[pparse_ctx->input_file_count++] = file;
}

static PParseFile *
pparse_new_file_view(const char *name, PParseFile *base, int line_delta, bool is_system, bool is_include_entry) {
	PParseFile *file = pparse_arena_alloc(&pparse_ctx->main_arena, sizeof(PParseFile));
	file->name = pparse_intern_filename(name ? name : base->name);
	file->file_no = pparse_ctx->input_file_count;
	file->contents = base->contents;
	file->contents_len = base->contents_len;
	file->line_delta = line_delta;
	file->is_system = is_system;
	file->is_direct_system_include = false;
	file->skip_emit = is_system && is_include_entry;
	pparse_add_input_file(file);
	return file;
}

// Scan line directive; returns position after it, or NULL if not a line marker.
// Accepts `#`, digraph `%:`, and trigraph `??=` as the directive introducer.
static char *pparse_scan_line_directive(char *p, PParseFile *base_file, int *line_no, bool *in_system_include) {
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

	if (!PPARSE_IS_DIGIT(*p)) return NULL;
	unsigned long new_line = 0;
	while (PPARSE_IS_DIGIT(*p)) {
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
		if (!filename) pparse_error("out of memory");
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
	PParseFile *view;
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
	 * path must not sticky-skip the rest of the TU (PPARSE_TF_SYS_SKIP). */
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
	view = pparse_new_file_view(filename ? filename : pparse_ctx->current_file->name,
			     base_file,
			     line_delta,
			     is_system,
			     *in_system_include);
	view->is_direct_system_include = direct_system;
	pparse_ctx->current_file = view;
	free(filename);
	while (*p && *p != '\n') p++;
	if (*p == '\n') {
		p++;
		(*line_no)++;
	}
	return p;
}

static char *pparse_scan_pp_number(char *p, bool *is_float) {
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
		} else if (pparse_ident_char[(unsigned char)c]) {
			p++;
		} else if (c == '\'' && pparse_ident_char[(unsigned char)p[1]]) {
			p++;
		} else
			break;
	}
	if (is_float) *is_float = float_lit;
	return p;
}

static PParseToken *pparse_tokenize(PParseFile *file) {
	PParseFile *base_file = file;
	pparse_ctx->current_file = file;
	pparse_ctx->token_source = file->contents;
	char *p = file->contents;
	/* Skip leading UTF-8 BOM (EF BB BF). UTF-16 BOMs are rejected earlier. */
	if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
		p += 3;
	pparse_token_pool_ensure(pparse_token_count + file->contents_len / 2 + 4096);
	uint32_t first_idx = pparse_token_count;
	pparse_token_tag_summary = 0;
	PParseTokState ts = {true, false, 1};
	bool in_system_include = false;
	bool saw_taint_kw = false, saw_asm_kw = false, saw_attr_kw = false, saw_noreturn_kw = false;
	while (*p) {
		if (ts.at_bol &&
		    (*p == '#' || (p[0] == '%' && p[1] == ':') ||
		     (p[0] == '?' && p[1] == '?' && p[2] == '='))) {
			char *directive_start = p;
			char *after = pparse_scan_line_directive(p, base_file, &ts.line_no, &in_system_include);
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

		if (p[0] == '/' && p[1] == '/') {
			p = pparse_skip_line_comment(p + 2, &ts);
			ts.has_space = true;
			continue;
		}
		if (p[0] == '/' && p[1] == '*') {
			p = pparse_skip_block_comment(p + 2, &ts);
			ts.has_space = true;
			continue;
		}
		if (*p == '\n' || pparse_is_space(*p)) {
			do {
				if (*p == '\n') {
					ts.line_no++;
					ts.at_bol = true;
					ts.has_space = false;
				} else
					ts.has_space = true;
				p++;
			} while (*p == '\n' || pparse_is_space(*p));
			continue;
		}
		/* Fast path: the vast majority of tokens are identifiers/keywords that
		 * do NOT start with a string/char literal prefix (u/U/L/R). Jump
		 * straight to identifier scanning, skipping ~15 literal-prefix branches.
		 * u/U/L/R starts fall through so `u8"..."`, `L'x'`, `R"..."` still work. */
		{
			unsigned char c0 = (unsigned char)*p;
			if (__builtin_expect((PPARSE_IS_ALPHA(c0) || c0 >= 0x80) && c0 != 'u' && c0 != 'U' &&
						 c0 != 'L' && c0 != 'R',
					     1))
				goto do_ident;
		}
		if (PPARSE_IS_DIGIT(*p) || (*p == '.' && PPARSE_IS_DIGIT(p[1]))) {
			char *start = p;
			p = pparse_scan_pp_number(p, NULL);
			pparse_new_token(PPARSE_TK_NUM, start, p, &ts);
			continue;
		}
		{ // Raw string literals
			int raw_pfx = (p[0] == 'R')						     ? 0
				      : (p[0] == 'u' && p[1] == '8' && p[2] == 'R')		     ? 2
				      : ((p[0] == 'L' || p[0] == 'u' || p[0] == 'U') && p[1] == 'R') ? 1
												     : -1;
			if (raw_pfx >= 0 && p[raw_pfx] == 'R' && p[raw_pfx + 1] == '"') {
				PParseToken *nt = pparse_read_raw_string_literal(p, p + raw_pfx + 1, &ts);
				p += nt->len;
				continue;
			}
		}
		if (*p == '"') {
			PParseToken *nt = pparse_read_string_literal(p, p, &ts);
			p += nt->len;
			continue;
		}
		if ((p[0] == 'u' && p[1] == '8' && p[2] == '"') ||
		    ((p[0] == 'u' || p[0] == 'U' || p[0] == 'L') && p[1] == '"')) {
			char *start = p;
			p += (p[0] == 'u' && p[1] == '8') ? 2 : 1;
			PParseToken *nt = pparse_read_string_literal(start, p, &ts);
			p = start + nt->len;
			continue;
		}
		if (*p == '\'') {
			PParseToken *nt = pparse_read_char_literal(p, p, &ts);
			p += nt->len;
			continue;
		}
		if (p[0] == 'u' && p[1] == '8' && p[2] == '\'') {
			PParseToken *nt = pparse_read_char_literal(p, p + 2, &ts);
			p += nt->len;
			continue;
		}
		if ((p[0] == 'u' || p[0] == 'U' || p[0] == 'L') && p[1] == '\'') {
			PParseToken *nt = pparse_read_char_literal(p, p + 1, &ts);
			p += nt->len;
			continue;
		}
	do_ident:;
		int ident_len = pparse_read_ident(p);
		if (ident_len) {
			PParseToken *t = pparse_new_token(PPARSE_TK_IDENT, p, p + ident_len, &ts);
			t->parse_data = pparse_fast_hash(p, (uint32_t)ident_len);
			uint64_t kw = pparse_keyword_lookup(p, ident_len);
			if (kw) {
				if (kw & PPARSE_KW_MARKER) {
					t->kind = PPARSE_TK_KEYWORD;
					t->tag = (uint32_t)(kw & ~PPARSE_KW_MARKER);
				} else
					t->tag = (uint32_t)kw;
				t->flags |= (uint16_t)(kw >> PPARSE_KW_FLAGS_SHIFT);
				pparse_token_tag_summary |= t->tag;
				if (t->tag & (PPARSE_TT_SPECIAL_FN | PPARSE_TT_NORETURN_FN)) saw_taint_kw = true;
				if (t->tag & PPARSE_TT_ASM) saw_asm_kw = true;
				if (t->tag & PPARSE_TT_ATTR) saw_attr_kw = true;
				if ((t->tag & PPARSE_TT_INLINE) && (t->tag & PPARSE_TT_SKIP_DECL) &&
				    (pparse_equal(t, "_Noreturn") || pparse_equal(t, "noreturn")))
					saw_noreturn_kw = true;
			}
			p += ident_len;
			continue;
		}
		int punct_len = pparse_read_punct(p);
		if (punct_len) {
			int abs_len = punct_len < 0 ? -punct_len : punct_len;
			PParseToken *t = pparse_new_token(PPARSE_TK_PUNCT, p, p + abs_len, &ts);
			if (punct_len < 0) {
				char *norm;
				switch (abs_len == 4 ? '%' : p[0]) {
				case '<':
					norm =
					    p[1] == ':' ? pparse_digraph_norm_bracket_open : pparse_digraph_norm_brace_open;
					break;
				case ':': norm = pparse_digraph_norm_bracket_close; break;
				case '%':
					norm = abs_len == 4 ? pparse_digraph_norm_paste
							    : (p[1] == '>' ? pparse_digraph_norm_brace_close
									   : pparse_digraph_norm_hash);
					break;
				default: norm = p; break;
				}
				p[0] = norm[0];
				if (abs_len == 4) p[1] = norm[1]; // %:%: -> ##
				t->len = (abs_len == 4) ? 2 : 1;
				t->ch0 = (uint8_t)norm[0];
			}
			pparse_classify_punct(t);
			/* Array-bracket grammar context is cached in parse_data after
			 * tokenization. The pool survives library-mode resets, so clear
			 * this one reused punctuation word at creation. */
			if (t->ch0 == '[') t->parse_data = 0;
			p += abs_len;
			continue;
		}
		pparse_error_at(p, "invalid token");
	}

	pparse_new_token(PPARSE_TK_EOF, p, p, &ts);

	PParseToken *first = first_idx ? &pparse_token_pool[first_idx] : NULL;
	// Also detect C23 [[ ... ]] attributes and tag the first '[' with PPARSE_TF_C23_ATTR.
	{
		int stack_cap = 256;
		PParseToken **stack = pparse_arena_alloc_uninit(&pparse_ctx->main_arena, stack_cap * sizeof(PParseToken *));
		int sp = 0;
		uint32_t last_prism_idx = 0;
		for (PParseToken *t = first; t && t->kind != PPARSE_TK_EOF; t = pparse_p0_next(t)) {
			if (t->tag & (PPARSE_TT_DEFER | PPARSE_TT_ORELSE)) last_prism_idx = pparse_idx(t);
			if (t->flags & PPARSE_TF_OPEN) {
				PPARSE_ARENA_ENSURE_CAP(
				    &pparse_ctx->main_arena, stack, sp + 1, stack_cap, 256, PParseToken *);
				stack[sp++] = t;
				PParseToken *tn = pparse_p0_next(t);
				if (t->ch0 == '[' && tn && tn->ch0 == '[' && (tn->flags & PPARSE_TF_OPEN)) {
					t->flags |= PPARSE_TF_C23_ATTR;
					saw_attr_kw = true;
				}
			} else if (t->flags & PPARSE_TF_CLOSE) {
				if (sp == 0) pparse_error_tok(t, "unmatched closing delimiter");
				PParseToken *open = stack[--sp];
				if (!pparse_delimiters_match(open, t))
					pparse_error_tok(t,
						  "mismatched closing delimiter '%c' for opener '%c'",
						  t->ch0,
						  open->ch0);
				open->match_idx = pparse_idx(t);
				t->match_idx = pparse_idx(open);
				if (last_prism_idx > pparse_idx(open)) open->flags |= PPARSE_TF_HAS_PRISM;
			}
		}
		if (sp > 0) pparse_error_tok(stack[sp - 1], "unclosed delimiter '%c'", stack[sp - 1]->ch0);
		/* User definitions of libc noreturn/special names are ordinary unless
		 * explicitly annotated (_Noreturn / attr). Clear keyword-cache tags so
		 * taint + auto-unreachable do not treat `int exit(void){…}` as builtin. */
		{
			PParseHashMap user_builtin = {0};
			PParseToken *fname = NULL;
			int depth = 0;
			for (PParseToken *t = first; t && t->kind != PPARSE_TK_EOF; t = pparse_p0_next(t)) {
				if ((t->flags & PPARSE_TF_OPEN) && pparse_match_ch(t, '{')) {
					if (depth == 0 && fname &&
					    (fname->tag & (PPARSE_TT_NORETURN_FN | PPARSE_TT_SPECIAL_FN))) {
						PParseToken *np = pparse_p0_next(fname);
						if (np && pparse_match_ch(np, '(') && pparse_pair(np)) {
							PParseToken *after = pparse_p0_next(pparse_pair(np));
							/* Allow noise/attrs between ) and { */
							while (after && after != t && after->kind != PPARSE_TK_EOF) {
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
							if (after == t)
								pparse_lex_token_map_put(&user_builtin, fname, (void *)1);
						}
					}
					depth++;
					fname = NULL;
					continue;
				}
				if ((t->flags & PPARSE_TF_CLOSE) && pparse_match_ch(t, '}')) {
					if (depth > 0) depth--;
					continue;
				}
				if (depth != 0) continue;
				if (pparse_match_ch(t, ';')) {
					fname = NULL;
					continue;
				}
				if (pparse_is_potential_func_name(t)) fname = t;
			}
			if (user_builtin.used > 0) {
				for (PParseToken *s = first; s && s->kind != PPARSE_TK_EOF; s = pparse_p0_next(s)) {
					if ((s->tag & (PPARSE_TT_NORETURN_FN | PPARSE_TT_SPECIAL_FN)) &&
					    pparse_lex_token_map_get(&user_builtin, s))
						s->tag &= ~(PPARSE_TT_NORETURN_FN | PPARSE_TT_SPECIAL_FN);
				}
			}
			pparse_hashmap_discard(&user_builtin);
		}

		// Pre-scan function bodies: tag '{' with PPARSE_TT_SPECIAL_FN / PPARSE_TT_ASM / PPARSE_TT_NORETURN_FN(=vfork).
		// Propagate special-function taint transitively through wrapper chains.
		if (saw_taint_kw || saw_asm_kw) {
			typedef struct {
				PParseToken *name;
				PParseToken *body;
			} PParseFunctionScan;

			PParseFunctionScan *functions = NULL;
			int function_count = 0;
			int function_capacity = 0;
			PParseToken *func_name = NULL;
			for (PParseToken *t = first; t && t->kind != PPARSE_TK_EOF; t = pparse_p0_next(t)) {
				PParseToken *attr_end = pparse_p0_attribute_group_end(t);
				if (attr_end) {
					t = attr_end;
					continue;
				}
				if (pparse_is_potential_func_name(t)) func_name = t;
				if (t->ch0 == '{' && (t->flags & PPARSE_TF_OPEN) && t->match_idx) {
					PParseToken *end = pparse_pair(t);
					for (PParseToken *b = pparse_p0_next(t); b != end; b = pparse_p0_next(b)) {
						if ((b->tag & PPARSE_TT_SPECIAL_FN) &&
						    !(pparse_idx(b) >= 1 &&
						      (pparse_token_pool[pparse_idx(b) - 1].tag & PPARSE_TT_MEMBER))) {
							/* Skip declarator shadows: `void (*setjmp)(void)` /
							 * `int vfork;` — not calls/refs. */
							PParseToken *prev =
							    pparse_idx(b) >= 1 ? &pparse_token_pool[pparse_idx(b) - 1]
									    : NULL;
							PParseToken *nxt = pparse_p0_next(b);
							bool decl_shadow =
							    prev &&
							    (pparse_match_ch(prev, '*') ||
							     (prev->tag &
							      (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE |
							       PPARSE_TT_SUE))) &&
							    nxt && nxt->ch0 != '(';
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
							     ag && ag != end;
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
								if ((ag->flags & PPARSE_TF_C23_ATTR) &&
								    pparse_pair(ag)) {
									ag = pparse_pair(ag);
									continue;
								}
								break;
							}
						}
					}
					if (func_name) {
						PPARSE_ARENA_ENSURE_CAP(&pparse_ctx->main_arena,
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
				wrapper_taint = pparse_arena_alloc(&pparse_ctx->main_arena,
							    (size_t)function_count * sizeof(*wrapper_taint));
				callee_idx = pparse_arena_alloc(&pparse_ctx->main_arena,
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
				int *caller_head = pparse_arena_alloc(&pparse_ctx->main_arena,
							       (size_t)function_count * sizeof(int));
				int *caller_next = pparse_arena_alloc(&pparse_ctx->main_arena,
							       (size_t)function_count * sizeof(int));
				int *queue = pparse_arena_alloc(&pparse_ctx->main_arena,
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
					PParseToken *end = pparse_pair(body);
					for (PParseToken *b = pparse_p0_next(body); b != end; b = pparse_p0_next(b)) {
						if (!pparse_p0_token_can_name_function(b)) continue;
						/* Skip declarator occurrences (`void (*f0)(void)`,
						 * `int f0;`) — keep bare refs so FP chains like
						 * `fp = f0; fp();` still propagate taint. */
						if (pparse_idx(b) >= 1) {
							PParseToken *prev = &pparse_token_pool[pparse_idx(b) - 1];
							PParseToken *n = pparse_p0_next(b);
							if ((pparse_match_ch(prev, '*') ||
							     (prev->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER |
									   PPARSE_TT_STORAGE | PPARSE_TT_SUE))) &&
							    n && n->ch0 != '(')
								continue;
						}
						if (!(fn_bloom &
						      (1ULL << (((unsigned)b->ch0 ^ b->len) & 63))))
							continue;
						if (pparse_idx(b) > pparse_idx(body) + 1) {
							PParseToken *prev = &pparse_token_pool[pparse_idx(b) - 1];
							if (prev->tag &
							    (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_SUE))
								continue;
							if (pparse_match_ch(prev, ')') && (prev->flags & PPARSE_TF_CLOSE) &&
							    prev->match_idx) {
								PParseToken *open = pparse_pair(prev);
								PParseToken *inner = open ? pparse_p0_next(open) : NULL;
								if (inner &&
								    (inner->tag &
								     (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE)))
									continue;
							}
						}
						void *v = pparse_lex_token_map_get(&func_map, b);
						if (!v) continue;
						int j = (int)(intptr_t)v - 1;
						pparse_VEC_ENSURE_REALLOC(edges, edge_count + 1, edge_cap, 64);
						edges[edge_count++] = (PParseTaintEdge){i, j};
					}
				}
				/* Body taint: call edge i→j propagates j's taint onto i. */
				if (edge_count > 0) {
					int *edge_head = pparse_arena_alloc(&pparse_ctx->main_arena,
								     (size_t)function_count * sizeof(int));
					int *edge_next = pparse_arena_alloc(&pparse_ctx->main_arena,
								     (size_t)edge_count * sizeof(int));
					int *queue = pparse_arena_alloc(&pparse_ctx->main_arena,
								 (size_t)function_count * sizeof(int));
					uint8_t *queued = pparse_arena_alloc(&pparse_ctx->main_arena,
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
						    (functions[j].body->tag & PPARSE_TT_NORETURN_FN)) {
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
		PParseHashMap nr_map = {0};
#define pparse_SKIP_ATTR_ARGS(a)                                                                                    \
	do {                                                                                                 \
		if ((a)->kind <= PPARSE_TK_KEYWORD && pparse_p0_next(a) && pparse_p0_next(a)->ch0 == '(' &&                      \
		    pparse_p0_next(a)->match_idx)                                                                   \
			(a) = &pparse_token_pool[pparse_p0_next(a)->match_idx];                                            \
	} while (0)
#define PPARSE_IS_NORETURN_NAME(a)                                                                                  \
	((a)->kind <= PPARSE_TK_KEYWORD &&                                                                          \
	 (pparse_equal((a), "noreturn") || pparse_equal((a), "_Noreturn") || pparse_equal((a), "__noreturn__")))
#define pparse_ATTR_SPAN_HAS_NORETURN(start, end, out)                                                              \
	do {                                                                                                 \
		for (PParseToken *_a = (start); _a && _a < (end); _a = pparse_p0_next(_a)) {                              \
			if (PPARSE_IS_NORETURN_NAME(_a)) {                                                          \
				(out) = true;                                                                \
				break;                                                                       \
			}                                                                                    \
			pparse_SKIP_ATTR_ARGS(_a);                                                                  \
		}                                                                                            \
	} while (0)
		for (PParseToken *t = first; t && t->kind != PPARSE_TK_EOF; t = pparse_p0_next(t)) {
			bool is_noreturn = false;
			bool attribute_form = false;
			PParseToken *scan_start = t;
			PParseToken *attr_origin = t; // original position for backward scan

			if (t->kind <= PPARSE_TK_KEYWORD &&
			    (pparse_equal(t, "_Noreturn") ||
			     (pparse_equal(t, "noreturn") && pparse_p0_soft_noreturn_is_decl_specifier(t))))
				is_noreturn = true;
			// [[noreturn]] / [[_Noreturn]] / [[__noreturn__]] — C23 attribute
			if (t->ch0 == '[' && (t->flags & PPARSE_TF_C23_ATTR) && t->match_idx) {
				attribute_form = true;
				PParseToken *inner = pparse_p0_next(t);
				PParseToken *attr_end = &pparse_token_pool[t->match_idx];
				if (inner && inner->ch0 == '[')
					pparse_ATTR_SPAN_HAS_NORETURN(pparse_p0_next(inner), attr_end, is_noreturn);
				t = attr_end; // advance past [[ ... ]]
				scan_start = t;
			}

			if (t->kind <= PPARSE_TK_KEYWORD &&
			    (pparse_equal(t, "__attribute__") || pparse_equal(t, "__attribute"))) {
				attribute_form = true;
				PParseToken *p1 = pparse_p0_next(t);
				if (p1 && p1->ch0 == '(') {
					PParseToken *p2 = pparse_p0_next(p1);
					if (p2 && p2->ch0 == '(' && p2->match_idx) {
						PParseToken *close = &pparse_token_pool[p2->match_idx];
						pparse_ATTR_SPAN_HAS_NORETURN(pparse_p0_next(p2), close, is_noreturn);
						t = pparse_pair(p1); // advance past __attribute__(( ... ))
						scan_start = t;
					}
				}
			}

			// __declspec(noreturn) or __declspec(__noreturn__) — MSVC
			if (t->kind <= PPARSE_TK_KEYWORD && pparse_equal(t, "__declspec")) {
				attribute_form = true;
				PParseToken *p1 = pparse_p0_next(t);
				if (p1 && p1->ch0 == '(' && p1->match_idx) {
					PParseToken *close = &pparse_token_pool[p1->match_idx];
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
				PParseToken *before = pparse_walk_back(pparse_idx(attr_origin), PPARSE_WB_ATTR_NOISE);
				post_decl_attr = before && pparse_match_ch(before, ')');
			}
			if (post_decl_attr) {
				for (uint32_t pi = pparse_idx(attr_origin); pi > 0; pi--) {
					PParseToken *pt = &pparse_token_pool[pi - 1];
					if (pt->kind == PPARSE_TK_PREP_DIR) continue;
					if (pt->ch0 == ';' || pt->ch0 == '{' || pt->ch0 == '}') break;
					if ((pt->ch0 == ')' || pt->ch0 == ']') && pparse_pair(pt)) {
						pi = pparse_idx(pparse_pair(pt)) + 1;
						continue;
					}
					if (pparse_p0_token_can_name_function(pt) && pparse_p0_next(pt) &&
					    pparse_p0_next(pt)->ch0 == '(') {
						if (pt->tag & (PPARSE_TT_SKIP_DECL | PPARSE_TT_INLINE | PPARSE_TT_QUALIFIER |
							       PPARSE_TT_TYPE | PPARSE_TT_STORAGE))
							continue;
						fn_name = pt;
						break;
					}
				}
			}
			if (!fn_name) {
				int fwd_depth = 0;
				for (PParseToken *s = scan_start; s && s->kind != PPARSE_TK_EOF; s = pparse_p0_next(s)) {
					char ch = s->ch0;
					if (ch == ';' || ch == '{') break;
					/* Post-declarator attrs bind to the preceding name only. */
					if (fwd_depth == 0 && ch == ',' && post_decl_attr) break;
					PParseToken *attr_end = pparse_p0_attribute_group_end(s);
					if (attr_end) {
						s = attr_end;
						continue;
					}
					if ((s->flags & PPARSE_TF_OPEN) && pparse_pair(s)) {
						fwd_depth++;
						continue;
					}
					if ((s->flags & PPARSE_TF_CLOSE) && fwd_depth > 0) {
						fwd_depth--;
						continue;
					}
					if (fwd_depth == 0 && pparse_p0_token_can_name_function(s) &&
					    pparse_p0_next(s) && pparse_p0_next(s)->ch0 == '(') {
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
				for (uint32_t pi = pparse_idx(attr_origin); pi > 0; pi--) {
					PParseToken *pt = &pparse_token_pool[pi - 1];
					if (pt->kind == PPARSE_TK_PREP_DIR) continue;
					if (pt->ch0 == ';' || pt->ch0 == '{' || pt->ch0 == '}') break;
					if ((pt->ch0 == ')' || pt->ch0 == ']') && pparse_pair(pt)) {
						pi = pparse_idx(pparse_pair(pt)) + 1;
						continue;
					}
					if (pparse_p0_token_can_name_function(pt) && pparse_p0_next(pt) &&
					    pparse_p0_next(pt)->ch0 == '(') {
						if (pt->tag & (PPARSE_TT_SKIP_DECL | PPARSE_TT_INLINE | PPARSE_TT_QUALIFIER |
							       PPARSE_TT_TYPE | PPARSE_TT_STORAGE))
							continue;
						fn_name = pt;
						break;
					}
				}
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
			for (PParseToken *s = first; s && s->kind != PPARSE_TK_EOF; s = pparse_p0_next(s)) {
				if (pparse_p0_token_can_name_function(s) &&
				    (nr_bloom & (1ULL << (((unsigned)s->ch0 ^ s->len) & 63))) &&
				    !(pparse_idx(s) >= 1 && (pparse_token_pool[pparse_idx(s) - 1].tag & PPARSE_TT_MEMBER)) &&
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

static void pparse_ensure_keyword_cache(void) {
	if (!pparse_keyword_cache[0].name && !pparse_keyword_cache[1].name) pparse_init_keyword_map();
	else if (pparse_ctx->keyword_cache_features != pparse_ctx->features)
		pparse_init_keyword_map();
}

static inline PParseToken *pparse_finalize_load(char *name, char *buf) {
	PParseFile *file = pparse_arena_alloc(&pparse_ctx->main_arena, sizeof(PParseFile));
	*file = (PParseFile){.name = pparse_intern_filename(name),
		       .contents = buf,
		       .contents_len = strlen(buf),
		       .file_no = pparse_ctx->input_file_count,
		       .owns_contents = true};
	pparse_add_input_file(file);
	return pparse_tokenize(file);
}

static PParseToken *pparse_tokenize_buffer(char *name, char *buf) {
	if (!buf) return NULL;
	pparse_ensure_keyword_cache();
	return pparse_finalize_load(name, buf);
}

PParseToken *pparse_tokenize_file(char *path) {
	pparse_ensure_keyword_cache();

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
		return pparse_finalize_load(path, buf);
	}
	if (li_size.QuadPart > 512LL * 1024 * 1024) {
		CloseHandle(hFile);
		fprintf(stderr, "pparse_error: file too large: %s\n", path);
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
	return pparse_finalize_load(path, buf);
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
		pparse_error("source file '%s' exceeds 4 GiB", path);
	}
	if (size == 0) {
		close(fd);
		char *buf = malloc(8);
		if (!buf) return NULL;
		memset(buf, 0, 8);
		return pparse_finalize_load(path, buf);
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
	return pparse_finalize_load(path, buf);
#endif
}

// Used by both Pass 1 (analysis) and Pass 2 (emission) in prism.c.

typedef struct {
	PParseToken *end;	   // First token after the type specifier
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
} PParseDecl;

/* Canonical object shape derived from a C type-specifier + declarator pair.
 * Consumers decide policy (zero-init, diagnostics, emission); parse.c owns
 * the language question of what object/function shape was declared. */
typedef struct {
	bool effective_vla : 1;
	bool is_aggregate : 1;
	bool is_union_type : 1;
	bool is_func_type : 1;
} PParseDeclShape;

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

typedef struct {
	char *name;		  // Points into token stream (no alloc needed)
	int prev_index;		  // Index of previous entry with same name (-1 if none)
	uint32_t token_index;	  // PParseToken pool index of the declaration
	uint32_t scope_close_idx; // PParseToken index of matching '}' (UINT32_MAX for file scope)
	uint16_t len;
	uint16_t scope_depth; // Scope where defined (aligns with pparse_ctx->block_depth)
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
} PParseTypedefEntry;			     // 32 bytes — two entries per 64-byte cache line

#define PPARSE_ARRAY_RANK_WRAP_ALL 255

typedef struct {
	PParseTypedefEntry *entries;
	int count;
	int capacity;
	PParseHashMap name_map; // Maps name → (entry_index + 1) as void*, 0 = absent. Chain via prev_index.
	uint64_t bloom;	  // Bloom filter: bit (ch0 ^ len) & 63. Fast negative lookup.
} PParseTypedefTable;

enum {
	P1_IS_TYPEDEF = 1 << 0,	  // PParseToken resolves to a real typedef at this position
	P1_SCOPE_LOOP = 1 << 1,	  // This '{' opens a loop body
	P1_SCOPE_SWITCH = 1 << 2, // This '{' opens a switch body
	P1_HAS_ENTRY = 1 << 3,	  // PParseToken has any typedef-table entry (typedef/enum/shadow/VLA)
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

#define pparse_ann(t) ((t)->ann)

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
};

// Spread a typedef's TDF_* flag bag onto a PParseTypeSpec.is_typedef path.
// Two pparse_type_specifier sites need this; centralizing keeps them in
static inline void pparse_typedef_apply_tdf_flags(PParseTypeSpec *r, int tflags) {
	r->is_typedef = true;
	if (tflags & PPARSE_TDF_VLA) r->is_vla = true;
	if (tflags & PPARSE_TDF_AGGREGATE) r->is_struct = true;
	if (tflags & PPARSE_TDF_UNION) r->is_union = true;
	if (tflags & PPARSE_TDF_VOLATILE) r->has_volatile = true;
	if (tflags & PPARSE_TDF_HAS_VOL_MEMBER) r->has_volatile_member = true;
	if (tflags & PPARSE_TDF_ATOMIC) r->has_atomic = true;
}

#define pparse_feat(f) (pparse_ctx->features & (f))

static PRISM_THREAD_LOCAL PParseTypedefTable pparse_typedef_table;

static PRISM_THREAD_LOCAL uint32_t pparse_td_scope_close = UINT32_MAX;
static PRISM_THREAD_LOCAL bool
    pparse_p1_typedef_annotated; // true after p1_annotate_typedefs(); enables O(1) pparse_is_known_typedef
static PRISM_THREAD_LOCAL PParseHashMap pparse_function_symbols;
typedef enum {
	PPARSE_FS_NONE = 0,
	PPARSE_FS_FUNCTION = 1,
	PPARSE_FS_AGGREGATE_RETURN = 2,
} PParseFunctionSymbolKind;
/* Nonzero once any `raw {` brace was annotated this TU — gates scope walks. */
static PRISM_THREAD_LOCAL uint32_t pparse_p1_raw_block_count;

#define PPARSE_TD_SCOPE_SAVE() uint32_t _tds_c = pparse_td_scope_close
#define PPARSE_TD_SCOPE_RESTORE()                                                                                   \
	do {                                                                                                 \
		pparse_td_scope_close = _tds_c;                                                                     \
	} while (0)

#define pparse_is_c23_attr(t) ((t) && ((t)->flags & PPARSE_TF_C23_ATTR))
#define pparse_is_sizeof_like(t) ((t)->flags & PPARSE_TF_SIZEOF)

static inline PRISM_PURE uint32_t pparse_token_name_hash(PParseToken *tok) {
	/* After pool annotation, parse_data holds the resolved typedef entry for
	 * P1_HAS_ENTRY tokens. The uncommon bounds/tag query recomputes its hash. */
	if (pparse_p1_typedef_annotated && (pparse_ann(tok) & P1_HAS_ENTRY))
		return pparse_fast_hash(pparse_loc(tok), tok->len);
	return tok->parse_data;
}

static inline PRISM_PURE PParseFunctionSymbolKind pparse_function_symbol(PParseToken *tok) {
	if (!tok || !pparse_function_symbols.buckets) return PPARSE_FS_NONE;
	return (PParseFunctionSymbolKind)(intptr_t)pparse_hashmap_get_hashed(
	    &pparse_function_symbols, pparse_loc(tok), tok->len, pparse_token_name_hash(tok));
}

static inline void pparse_function_symbol_put(PParseToken *tok, PParseFunctionSymbolKind kind) {
	pparse_hashmap_put_hashed(
	    &pparse_function_symbols, pparse_loc(tok), tok->len, (void *)(intptr_t)kind, pparse_token_name_hash(tok));
}

static inline void pparse_function_symbols_reset(void) {
	pparse_hashmap_discard(&pparse_function_symbols);
}
#define pparse_is_enum_kw(t) ((t)->tag & PPARSE_TT_SUE && (t)->ch0 == 'e')

static inline PRISM_ALWAYS_INLINE PRISM_PURE bool pparse_is_identifier_like(PParseToken *tok) {
	return tok->kind <= PPARSE_TK_KEYWORD; // PPARSE_TK_IDENT=0, PPARSE_TK_KEYWORD=1
}

static inline PParseToken *pparse_skip_balanced_group(PParseToken *tok) {
	PParseToken *end = pparse_pair(tok);
	if (!end) return pparse_next(tok);
	return pparse_next(end);
}

static inline PParseToken *pparse_skip_prep_dirs(PParseToken *tok) {
	while (tok && tok->kind == PPARSE_TK_PREP_DIR) tok = pparse_next(tok);
	return tok;
}

static inline PParseToken *pparse_skip_prep_dirs_until(PParseToken *tok, PParseToken *end) {
	while (tok && tok != end && tok->kind == PPARSE_TK_PREP_DIR) tok = pparse_next(tok);
	return tok;
}

static bool pparse_is_pp_conditional(PParseToken *s) {
	if (s->kind != PPARSE_TK_PREP_DIR) return false;
	const char *dp = pparse_loc(s);
	if (*dp == '#') dp++;
	while (*dp == ' ' || *dp == '\t') dp++;
	return strncmp(dp, "ifdef", 5) == 0 || strncmp(dp, "ifndef", 6) == 0 || strncmp(dp, "elif", 4) == 0 ||
	       strncmp(dp, "else", 4) == 0 || strncmp(dp, "endif", 5) == 0 ||
	       (strncmp(dp, "if", 2) == 0 && (dp[2] == ' ' || dp[2] == '\t' || dp[2] == '('));
}

static PParseToken *pparse_span_find_pp_conditional(PParseToken *start, PParseToken *end, bool (*is_end)(PParseToken *)) {
	int sd = 0;
	for (PParseToken *s = start; s && s != end && s->kind != PPARSE_TK_EOF; s = pparse_next(s)) {
		if (s->flags & PPARSE_TF_OPEN) sd++;
		else if (s->flags & PPARSE_TF_CLOSE)
			sd--;
		else if (is_end && sd == 0 && is_end(s))
			break;
		if (pparse_is_pp_conditional(s)) return s;
	}
	return NULL;
}

static bool pparse_tok_is_semicolon(PParseToken *s) {
	return pparse_match_ch(s, ';');
}

// Skip noise tokens (attributes, C23 [[...]], prep dirs) in analysis mode.
static PRISM_PURE PParseToken *pparse_skip_noise(PParseToken *tok) {
	while (tok && tok->kind != PPARSE_TK_EOF) {
		if (tok->tag & PPARSE_TT_ATTR) {
			tok = pparse_next(tok);
			if (tok && pparse_match_ch(tok, '(') && pparse_pair(tok)) tok = pparse_next(pparse_pair(tok));
		} else if (pparse_is_c23_attr(tok) && pparse_pair(tok)) {
			tok = pparse_next(pparse_pair(tok));
		} else if (tok->kind == PPARSE_TK_PREP_DIR) {
			tok = pparse_next(tok);
		} else
			break;
	}
	return tok;
}

/* `pparse_skip_noise` already eats PPARSE_TT_ATTR / [[...]] / _Pragma; this adds PPARSE_TT_ASM. */
static PParseToken *pparse_skip_asm_specifier_trail(PParseToken *t) {
	t = pparse_skip_noise(t);
	while (t && (t->tag & PPARSE_TT_ASM)) {
		t = pparse_next(t);
		if (t && pparse_match_ch(t, '(') && pparse_pair(t)) t = pparse_next(pparse_pair(t));
		t = pparse_skip_noise(t);
	}
	return t;
}

// Check if a token is "noise" (attribute, C23 [[...]], or preprocessor directive).
// These tokens must be skipped via pparse_skip_noise() before any tag-based type checks.
static inline PRISM_PURE bool pparse_is_noise_token(PParseToken *t) {
	return (t->tag & PPARSE_TT_ATTR) || pparse_is_c23_attr(t) || t->kind == PPARSE_TK_PREP_DIR;
}

#ifdef PRISM_DEBUG
#define pparse_ASSERT_NOT_NOISE(t)                                                                                  \
	do {                                                                                                 \
		if (pparse_is_noise_token(t))                                                                       \
			pparse_error_tok(t, "internal: tag check on noise token (pparse_skip_noise() missing)");           \
	} while (0)
#else
#define pparse_ASSERT_NOT_NOISE(t) ((void)0)
#endif

#define pparse_SKIP_NOISE_CONTINUE(var)                                                                             \
	do {                                                                                                 \
		PParseToken *_sn = pparse_skip_noise(var);                                                                \
		if (_sn != (var)) {                                                                          \
			(var) = _sn;                                                                         \
			continue;                                                                            \
		}                                                                                            \
	} while (0)

static PRISM_PURE PParseToken *pparse_skip_to_semicolon(PParseToken *tok, PParseToken *end) {
	while (tok->kind != PPARSE_TK_EOF) {
		if (end && tok == end) return tok;
		if (tok->flags & PPARSE_TF_OPEN) {
			tok = pparse_next(pparse_pair(tok));
			continue;
		}
		if (pparse_match_ch(tok, ';')) return tok;
		if ((tok->flags & PPARSE_TF_CLOSE) && tok->ch0 == '}') return tok;
		tok = pparse_next(tok);
	}
	return tok;
}

/* Long same-name chains use a token_index-sorted timeline; short chains walk.
 * prev_cover is the nearest earlier entry whose scope closes later. Once cur
 * reaches one close, every entry skipped by that link is also dead, bounding
 * lookup by the post-build prefix, binary search, and lexical nesting.
 * Mid-Pass1 rebuilds keep that prefix bounded as stress-TU chains grow. */
static PRISM_THREAD_LOCAL int *pparse_td_tl_idxs;
static PRISM_THREAD_LOCAL int pparse_td_tl_cap;
static PRISM_THREAD_LOCAL int *pparse_td_tl_prev_cover;
static PRISM_THREAD_LOCAL int pparse_td_tl_prev_cover_cap;
/* Timeline descriptor indexed by the name-map head at the last build. This
 * turns long-chain resolution from two hash probes into one probe + one load. */
static PRISM_THREAD_LOCAL uint64_t *pparse_td_tl_descs;
static PRISM_THREAD_LOCAL int pparse_td_tl_desc_cap;
static PRISM_THREAD_LOCAL int pparse_td_max_chain_seen; /* capped sample; ≥8 triggers timeline build */
static PRISM_THREAD_LOCAL int pparse_td_tl_count_at_build; /* pparse_typedef_table.count when timelines last built */
#define PPARSE_TL_REBUILD_CADENCE 256

/* Bounds-check array locals live here — not in the ordinary typedef/shadow
 * chain — so Pass 1/2 typedef lookups are not taxed by `int a[N]` × thousands
 * of functions. Lookups use the same mid-Pass1 timeline pattern as typedefs. */
typedef struct {
	char *name;
	int prev_index;
	uint32_t token_index;
	uint32_t scope_close_idx;
	uint16_t len;
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

static PRISM_THREAD_LOCAL PParseBoundsArrayTable pparse_bounds_array_table;
static PRISM_THREAD_LOCAL int *pparse_ba_tl_idxs;
static PRISM_THREAD_LOCAL int pparse_ba_tl_cap;
static PRISM_THREAD_LOCAL int *pparse_ba_tl_prev_cover;
static PRISM_THREAD_LOCAL int pparse_ba_tl_prev_cover_cap;
static PRISM_THREAD_LOCAL uint64_t *pparse_ba_tl_descs;
static PRISM_THREAD_LOCAL int pparse_ba_tl_desc_cap;
static PRISM_THREAD_LOCAL int pparse_ba_max_chain_seen;
static PRISM_THREAD_LOCAL int pparse_ba_tl_count_at_build;

static void pparse_bounds_array_table_reset(void) {
	pparse_bounds_array_table.entries = NULL;
	pparse_bounds_array_table.count = 0;
	pparse_bounds_array_table.capacity = 0;
	pparse_bounds_array_table.bloom = 0;
	pparse_hashmap_discard(&pparse_bounds_array_table.name_map);
	pparse_ba_tl_idxs = NULL;
	pparse_ba_tl_cap = 0;
	pparse_ba_tl_prev_cover = NULL;
	pparse_ba_tl_prev_cover_cap = 0;
	pparse_ba_tl_descs = NULL;
	pparse_ba_tl_desc_cap = 0;
	pparse_ba_max_chain_seen = 0;
	pparse_ba_tl_count_at_build = 0;
}

static void pparse_typedef_table_reset(void) {
	pparse_typedef_table.entries = NULL;
	pparse_typedef_table.count = 0;
	pparse_typedef_table.capacity = 0;
	pparse_typedef_table.bloom = 0;
	pparse_hashmap_discard(&pparse_typedef_table.name_map);
	pparse_td_tl_idxs = NULL;
	pparse_td_tl_cap = 0;
	pparse_td_tl_prev_cover = NULL;
	pparse_td_tl_prev_cover_cap = 0;
	pparse_td_tl_descs = NULL;
	pparse_td_tl_desc_cap = 0;
	pparse_td_max_chain_seen = 0;
	pparse_td_tl_count_at_build = 0;
	pparse_bounds_array_table_reset();
}

static void pparse_reset(void) {
	pparse_typedef_table_reset();
	pparse_function_symbols_reset();
	pparse_p1_typedef_annotated = false;
}

static PRISM_PURE int pparse_typedef_get_index(char *name, int len) {
	return pparse_hashmap_index_hashed(&pparse_typedef_table.name_map, name, len, pparse_fast_hash(name, len));
}

static int pparse_td_tl_cmp_tok(const void *pa, const void *pb) {
	uint32_t ta = pparse_typedef_table.entries[*(const int *)pa].token_index;
	uint32_t tb = pparse_typedef_table.entries[*(const int *)pb].token_index;
	return (ta > tb) - (ta < tb);
}

static void pparse_td_build_timelines(void) {
	pparse_td_tl_count_at_build = pparse_typedef_table.count;
	if (pparse_td_max_chain_seen < 8 || !pparse_typedef_table.name_map.buckets) return;
	PPARSE_ARENA_ENSURE_CAP(&pparse_ctx->main_arena,
			 pparse_td_tl_descs,
			 pparse_typedef_table.count,
			 pparse_td_tl_desc_cap,
			 64,
			 uint64_t);
	memset(pparse_td_tl_descs, 0, (size_t)pparse_typedef_table.count * sizeof(*pparse_td_tl_descs));

	/* Walk name_map heads only (one probe per unique name). */
	int cap = pparse_typedef_table.name_map.capacity;
	int pos = 0;
	for (int b = 0; b < cap; b++) {
		PParseHashEntry *ent = &pparse_typedef_table.name_map.buckets[b];
		if (!ent->key || ent->key == PPARSE_TOMBSTONE) continue;
		int head = (int)(intptr_t)ent->val - 1;
		int run = 0;
		for (int j = head; j >= 0; j = pparse_typedef_table.entries[j].prev_index) run++;
		if (run < 8) continue;
		int base = pos;
		PPARSE_ARENA_ENSURE_CAP(&pparse_ctx->main_arena, pparse_td_tl_idxs, pos + run, pparse_td_tl_cap, 64, int);
		PPARSE_ARENA_ENSURE_CAP(
		    &pparse_ctx->main_arena, pparse_td_tl_prev_cover, pos + run, pparse_td_tl_prev_cover_cap, 64, int);
		for (int j = head; j >= 0; j = pparse_typedef_table.entries[j].prev_index)
			pparse_td_tl_idxs[pos++] = j;
		qsort(pparse_td_tl_idxs + base, (size_t)run, sizeof(int), pparse_td_tl_cmp_tok);
		for (int p = base; p < pos; p++) {
			uint32_t close = pparse_typedef_table.entries[pparse_td_tl_idxs[p]].scope_close_idx;
			int prev = p - 1;
			while (prev >= base &&
			       pparse_typedef_table.entries[pparse_td_tl_idxs[prev]].scope_close_idx <= close)
				prev = pparse_td_tl_prev_cover[prev];
			pparse_td_tl_prev_cover[p] = prev;
		}
		pparse_td_tl_descs[head] = ((uint64_t)(uint32_t)base << 32) | (uint32_t)run;
	}
}

static PRISM_PURE PParseTypedefEntry *pparse_td_lookup_timeline(uint64_t desc, uint32_t cur, bool tags_only) {
	uint32_t base = (uint32_t)(desc >> 32);
	uint32_t run = (uint32_t)desc;
	int lo = (int)base, hi = (int)base + (int)run;
	while (lo < hi) {
		int mid = lo + ((hi - lo) >> 1);
		if (pparse_typedef_table.entries[pparse_td_tl_idxs[mid]].token_index <= cur)
			lo = mid + 1;
		else
			hi = mid;
	}
	PParseTypedefEntry *tag_fallback = NULL;
	for (int p = lo - 1; p >= (int)base;) {
		PParseTypedefEntry *e = &pparse_typedef_table.entries[pparse_td_tl_idxs[p]];
		if (cur >= e->scope_close_idx) {
			p = pparse_td_tl_prev_cover[p];
			continue;
		}
		p--;
		if (tags_only) {
			if (e->is_struct_tag) return e;
			continue;
		}
		if (!e->is_struct_tag) return e;
		if (!tag_fallback) tag_fallback = e;
	}
	return tag_fallback;
}

static int pparse_ba_tl_cmp_tok(const void *pa, const void *pb) {
	uint32_t ta = pparse_bounds_array_table.entries[*(const int *)pa].token_index;
	uint32_t tb = pparse_bounds_array_table.entries[*(const int *)pb].token_index;
	return (ta > tb) - (ta < tb);
}

static void pparse_ba_build_timelines(void) {
	pparse_ba_tl_count_at_build = pparse_bounds_array_table.count;
	if (pparse_ba_max_chain_seen < 8 || !pparse_bounds_array_table.name_map.buckets) return;
	PPARSE_ARENA_ENSURE_CAP(&pparse_ctx->main_arena,
			 pparse_ba_tl_descs,
			 pparse_bounds_array_table.count,
			 pparse_ba_tl_desc_cap,
			 64,
			 uint64_t);
	memset(pparse_ba_tl_descs, 0, (size_t)pparse_bounds_array_table.count * sizeof(*pparse_ba_tl_descs));

	int cap = pparse_bounds_array_table.name_map.capacity;
	int pos = 0;
	for (int b = 0; b < cap; b++) {
		PParseHashEntry *ent = &pparse_bounds_array_table.name_map.buckets[b];
		if (!ent->key || ent->key == PPARSE_TOMBSTONE) continue;
		int head = (int)(intptr_t)ent->val - 1;
		int run = 0;
		for (int j = head; j >= 0; j = pparse_bounds_array_table.entries[j].prev_index) run++;
		if (run < 8) continue;
		int base = pos;
		PPARSE_ARENA_ENSURE_CAP(&pparse_ctx->main_arena, pparse_ba_tl_idxs, pos + run, pparse_ba_tl_cap, 64, int);
		PPARSE_ARENA_ENSURE_CAP(
		    &pparse_ctx->main_arena, pparse_ba_tl_prev_cover, pos + run, pparse_ba_tl_prev_cover_cap, 64, int);
		for (int j = head; j >= 0; j = pparse_bounds_array_table.entries[j].prev_index)
			pparse_ba_tl_idxs[pos++] = j;
		qsort(pparse_ba_tl_idxs + base, (size_t)run, sizeof(int), pparse_ba_tl_cmp_tok);
		for (int p = base; p < pos; p++) {
			uint32_t close = pparse_bounds_array_table.entries[pparse_ba_tl_idxs[p]].scope_close_idx;
			int prev = p - 1;
			while (prev >= base &&
			       pparse_bounds_array_table.entries[pparse_ba_tl_idxs[prev]].scope_close_idx <= close)
				prev = pparse_ba_tl_prev_cover[prev];
			pparse_ba_tl_prev_cover[p] = prev;
		}
		pparse_ba_tl_descs[head] = ((uint64_t)(uint32_t)base << 32) | (uint32_t)run;
	}
}

static PRISM_PURE PParseBoundsArrayEntry *pparse_ba_lookup_timeline(uint64_t desc, uint32_t cur) {
	uint32_t base = (uint32_t)(desc >> 32);
	uint32_t run = (uint32_t)desc;
	int lo = (int)base, hi = (int)base + (int)run;
	while (lo < hi) {
		int mid = lo + ((hi - lo) >> 1);
		if (pparse_bounds_array_table.entries[pparse_ba_tl_idxs[mid]].token_index <= cur)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (int p = lo - 1; p >= (int)base;) {
		PParseBoundsArrayEntry *e = &pparse_bounds_array_table.entries[pparse_ba_tl_idxs[p]];
		if (cur >= e->scope_close_idx) {
			p = pparse_ba_tl_prev_cover[p];
			continue;
		}
		p--;
		return e;
	}
	return NULL;
}

static void pparse_bounds_array_add(char *name, int len, uint32_t token_index, uint8_t array_rank,
				     bool dim_complete, bool is_vla_var, bool blocks_outer) {
	uint32_t hash = pparse_fast_hash(name, len);
	int existing = pparse_hashmap_index_hashed(&pparse_bounds_array_table.name_map, name, len, hash);
	if (existing >= 0) {
		PParseBoundsArrayEntry *prev = &pparse_bounds_array_table.entries[existing];
		if (prev->token_index == token_index) {
			prev->array_rank = array_rank;
			prev->array_dim_complete = dim_complete;
			prev->is_vla_var = is_vla_var;
			prev->blocks_outer = blocks_outer;
			return;
		}
	}

	PPARSE_ARENA_ENSURE_CAP(&pparse_ctx->main_arena,
			 pparse_bounds_array_table.entries,
			 pparse_bounds_array_table.count + 1,
			 pparse_bounds_array_table.capacity,
			 32,
			 PParseBoundsArrayEntry);
	int new_index = pparse_bounds_array_table.count++;
	PParseBoundsArrayEntry *e = &pparse_bounds_array_table.entries[new_index];
	e->name = name;
	e->len = (uint16_t)len;
	e->prev_index = existing;
	e->token_index = token_index;
	e->scope_close_idx = pparse_td_scope_close;
	e->array_rank = array_rank;
	e->array_dim_complete = dim_complete;
	e->is_vla_var = is_vla_var;
	e->blocks_outer = blocks_outer;
	pparse_hashmap_put_hashed(
	    &pparse_bounds_array_table.name_map, name, len, (void *)(intptr_t)(new_index + 1), hash);
	pparse_bounds_array_table.bloom |= 1ULL << (((unsigned char)name[0] ^ len) & 63);
	int cl = 1;
	for (int p = e->prev_index; p >= 0 && cl < 8; p = pparse_bounds_array_table.entries[p].prev_index) cl++;
	if (cl > pparse_ba_max_chain_seen) pparse_ba_max_chain_seen = cl;
	if (pparse_ba_max_chain_seen >= 8) {
		int since = pparse_bounds_array_table.count - pparse_ba_tl_count_at_build;
		if (!pparse_ba_tl_idxs || since >= PPARSE_TL_REBUILD_CADENCE) pparse_ba_build_timelines();
	}
}

static PRISM_PURE PParseBoundsArrayEntry *pparse_bounds_array_lookup(PParseToken *tok) {
	if (!pparse_is_identifier_like(tok)) return NULL;
	unsigned c0 = tok->ch0, tl = tok->len;
	if (!(pparse_bounds_array_table.bloom & (1ULL << ((c0 ^ tl) & 63)))) return NULL;
	char *name = pparse_loc(tok);
	uint32_t cur = pparse_idx(tok);
	if (pparse_ba_tl_idxs) {
		uint32_t hash = pparse_token_name_hash(tok);
		int idx = pparse_hashmap_index_hashed(&pparse_bounds_array_table.name_map, name, (int)tok->len, hash);
		while (idx >= pparse_ba_tl_count_at_build) {
			PParseBoundsArrayEntry *e = &pparse_bounds_array_table.entries[idx];
			if (e->token_index <= cur && cur < e->scope_close_idx)
				return e;
			idx = e->prev_index;
		}
		uint64_t desc = idx >= 0 ? pparse_ba_tl_descs[idx] : 0;
		if (desc) return pparse_ba_lookup_timeline(desc, cur);
		while (idx >= 0) {
			PParseBoundsArrayEntry *e = &pparse_bounds_array_table.entries[idx];
			if (e->token_index <= cur && cur < e->scope_close_idx)
				return e;
			idx = e->prev_index;
		}
		return NULL;
	}
	int idx = pparse_hashmap_index_hashed(
	    &pparse_bounds_array_table.name_map, name, (int)tok->len, pparse_token_name_hash(tok));
	while (idx >= 0) {
		PParseBoundsArrayEntry *e = &pparse_bounds_array_table.entries[idx];
		if (e->token_index <= cur && cur < e->scope_close_idx)
			return e;
		idx = e->prev_index;
	}
	return NULL;
}

static PRISM_PURE PParseBoundsArrayEntry *pparse_bounds_array_entry_for_token(PParseToken *t) {
	for (int ix = pparse_hashmap_index_hashed(
		 &pparse_bounds_array_table.name_map, pparse_loc(t), t->len, pparse_token_name_hash(t));
	     ix >= 0;
	     ix = pparse_bounds_array_table.entries[ix].prev_index) {
		PParseBoundsArrayEntry *e = &pparse_bounds_array_table.entries[ix];
		if (e->token_index == pparse_idx(t)) return e;
	}
	return NULL;
}

static PParseTypedefEntry *
pparse_typedef_add_entry(PParseToken *tok, int scope_depth, PParseTypedefKind kind, bool is_vla, bool is_void) {
	char *name = pparse_loc(tok);
	int len = (int)tok->len;
	uint32_t hash = pparse_fast_hash(name, len);
	int existing = pparse_hashmap_index_hashed(&pparse_typedef_table.name_map, name, len, hash);
	// Skip duplicate re-definitions at the same scope (valid C11 §6.7/3).
	if (existing >= 0) {
		PParseTypedefEntry *prev = &pparse_typedef_table.entries[existing];
		if (kind == PPARSE_TDK_SHADOW || kind == PPARSE_TDK_VLA_VAR) {
			if (prev->scope_depth == scope_depth && prev->scope_close_idx == pparse_td_scope_close &&
			    prev->is_shadow == (kind == PPARSE_TDK_SHADOW) &&
			    prev->is_vla_var == (kind == PPARSE_TDK_VLA_VAR))
				return NULL;
		} else if (prev->scope_depth == scope_depth && !prev->is_shadow &&
			   prev->scope_close_idx == pparse_td_scope_close &&
			   prev->is_struct_tag == (kind == PPARSE_TDK_STRUCT_TAG))
			return NULL;
	}

	PPARSE_ARENA_ENSURE_CAP(&pparse_ctx->main_arena,
			 pparse_typedef_table.entries,
			 pparse_typedef_table.count + 1,
			 pparse_typedef_table.capacity,
			 32,
			 PParseTypedefEntry);
	int new_index = pparse_typedef_table.count++;
	PParseTypedefEntry *e = &pparse_typedef_table.entries[new_index];
	e->name = name;
	e->len = len;
	e->scope_depth = scope_depth;
	e->is_vla = (kind == PPARSE_TDK_TYPEDEF || kind == PPARSE_TDK_VLA_VAR || kind == PPARSE_TDK_STRUCT_TAG) ? is_vla : false;
	e->is_void = (kind == PPARSE_TDK_TYPEDEF) ? is_void : false;
	e->is_const = false;
	e->is_shadow = (kind == PPARSE_TDK_SHADOW || kind == PPARSE_TDK_ENUM_CONST);
	e->is_enum_const = (kind == PPARSE_TDK_ENUM_CONST);
	e->is_vla_var = (kind == PPARSE_TDK_VLA_VAR);
	e->is_struct_tag = (kind == PPARSE_TDK_STRUCT_TAG);
	e->is_param = false;
	e->array_rank = 0;
	e->array_dim_complete = true;
	e->is_atomic = false;
	e->prev_index = existing;
	e->token_index = pparse_idx(tok);
	e->scope_close_idx = pparse_td_scope_close;
	pparse_hashmap_put_hashed(&pparse_typedef_table.name_map, name, len, (void *)(intptr_t)(new_index + 1), hash);
	pparse_typedef_table.bloom |= 1ULL << (((unsigned char)name[0] ^ len) & 63);
	int cl = 1;
	for (int p = e->prev_index; p >= 0 && cl < 8; p = pparse_typedef_table.entries[p].prev_index) cl++;
	if (cl > pparse_td_max_chain_seen) pparse_td_max_chain_seen = cl;
	/* Rebuild mid-Pass1 so long same-name chains stay O(log n), with a
	 * constant bound on the unsorted prefix between rebuilds. */
	if (pparse_td_max_chain_seen >= 8) {
		int since = pparse_typedef_table.count - pparse_td_tl_count_at_build;
		if (!pparse_td_tl_idxs || since >= PPARSE_TL_REBUILD_CADENCE) pparse_td_build_timelines();
	}
	return e;
}

static PParseTypedefEntry *pparse_binding_entry(PParseToken *tok, bool shadow_only) {
	for (int ix = pparse_typedef_get_index(pparse_loc(tok), tok->len); ix >= 0;
	     ix = pparse_typedef_table.entries[ix].prev_index) {
		PParseTypedefEntry *e = &pparse_typedef_table.entries[ix];
		if (e->token_index == pparse_idx(tok) && (!shadow_only || e->is_shadow)) return e;
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

static inline PParseTypedefEntry *pparse_shadow_entry(PParseToken *tok) {
	return pparse_binding_entry(tok, true);
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
};

static inline void pparse_binding_apply_traits(PParseTypedefEntry *entry, unsigned traits) {
	if (!entry) return;
	if (traits & PPARSE_BIND_FUNC) entry->is_func = true;
	if (traits & PPARSE_BIND_CONST) entry->is_const = true;
	if (traits & PPARSE_BIND_VOLATILE) entry->is_volatile = true;
	if (traits & PPARSE_BIND_VOLATILE_MEMBER) entry->has_volatile_member = true;
	if (traits & PPARSE_BIND_ATOMIC) entry->is_atomic = true;
	if (traits & PPARSE_BIND_AGGREGATE) entry->is_aggregate = true;
}

/* Register or update an ordinary identifier without exposing table insertion
 * order to consumers. Multiple traits intentionally collapse to one lookup. */
static PParseTypedefEntry *pparse_register_shadow_traits(PParseToken *tok, int scope_depth, unsigned traits) {
	PParseTypedefEntry *entry = pparse_register_shadow(tok, scope_depth);
	pparse_binding_apply_traits(entry, traits);
	return entry;
}

static PParseTypedefEntry *pparse_register_vla_binding(PParseToken *tok, int scope_depth, bool is_param) {
	PParseTypedefEntry *entry = pparse_register_vla_var(tok, scope_depth);
	if (entry && is_param) entry->is_param = true;
	return entry;
}

static PParseTypedefEntry *pparse_register_struct_tag(PParseToken *tok,
					   int scope_depth,
					   bool has_vla,
					   bool is_aggregate,
					   bool has_volatile_member) {
	PParseTypedefEntry *entry =
	    pparse_typedef_add_entry(tok, scope_depth, PPARSE_TDK_STRUCT_TAG, has_vla, false);
	if (!entry) entry = pparse_binding_entry(tok, false);
	pparse_binding_apply_traits(entry,
			       (is_aggregate ? PPARSE_BIND_AGGREGATE : 0) |
				   (has_volatile_member ? PPARSE_BIND_VOLATILE_MEMBER : 0));
	return entry;
}

static inline bool pparse_is_soft_keyword_identifier(PParseToken *tok);

static PRISM_PURE PParseTypedefEntry *pparse_typedef_lookup(PParseToken *tok) {
	if (!pparse_is_identifier_like(tok)) return NULL;
	if (pparse_p1_typedef_annotated) {
		if (!(pparse_ann(tok) & P1_HAS_ENTRY)) return NULL;
		return &pparse_typedef_table.entries[tok->parse_data - 1];
	}
	if (tok->kind == PPARSE_TK_KEYWORD && !pparse_is_soft_keyword_identifier(tok) &&
	    !(tok->tag & (PPARSE_TT_ORELSE | PPARSE_TT_DEFER)) && !(tok->flags & PPARSE_TF_RAW))
		return NULL;
	unsigned c0 = tok->ch0, tl = tok->len;
	if (!(pparse_typedef_table.bloom & (1ULL << ((c0 ^ tl) & 63)))) return NULL;
	char *name = pparse_loc(tok);
	uint32_t cur = pparse_idx(tok);
	if (pparse_td_tl_idxs) {
		uint32_t hash = pparse_token_name_hash(tok);
		PParseTypedefEntry *tag_fallback = NULL;
		int idx = pparse_hashmap_index_hashed(&pparse_typedef_table.name_map, name, (int)tok->len, hash);
		/* Entries added since the last build are absent from the sorted
		 * timeline. Walk only that bounded newest prefix. */
		while (idx >= pparse_td_tl_count_at_build) {
			PParseTypedefEntry *e = &pparse_typedef_table.entries[idx];
			if (e->token_index <= cur && cur < e->scope_close_idx) {
				if (!e->is_struct_tag) return e;
				if (!tag_fallback) tag_fallback = e;
			}
			idx = e->prev_index;
		}
		uint64_t desc = idx >= 0 ? pparse_td_tl_descs[idx] : 0;
		if (desc) {
			PParseTypedefEntry *hit = pparse_td_lookup_timeline(desc, cur, false);
			return hit ? hit : tag_fallback;
		}
		/* Short chain (no timeline): finish the ordinary walk from idx. */
		while (idx >= 0) {
			PParseTypedefEntry *e = &pparse_typedef_table.entries[idx];
			if (e->token_index <= cur && cur < e->scope_close_idx) {
				if (!e->is_struct_tag) return e;
				if (!tag_fallback) tag_fallback = e;
			}
			idx = e->prev_index;
		}
		return tag_fallback;
	}
	int idx = pparse_hashmap_index_hashed(&pparse_typedef_table.name_map, name, tok->len, pparse_token_name_hash(tok));
	// ISO C11 §6.2.3: tag namespace is separate from ordinary identifiers.
	PParseTypedefEntry *tag_fallback = NULL;
	while (idx >= 0) {
		PParseTypedefEntry *e = &pparse_typedef_table.entries[idx];
		if (e->token_index <= cur && cur < e->scope_close_idx) {
			if (!e->is_struct_tag) return e;
			if (!tag_fallback) tag_fallback = e;
		}
		idx = e->prev_index;
	}
	return tag_fallback;
}

typedef enum {
	PPARSE_BINDING_NONE,
	PPARSE_BINDING_TYPE,
	PPARSE_BINDING_VALUE,
} PParseIdentifierBindingKind;

static PRISM_PURE PParseIdentifierBindingKind pparse_identifier_binding_kind(PParseToken *tok) {
	PParseTypedefEntry *entry = pparse_typedef_lookup(tok);
	if (!entry) return PPARSE_BINDING_NONE;
	return entry->is_shadow || entry->is_enum_const || entry->is_vla_var ? PPARSE_BINDING_VALUE
									   : PPARSE_BINDING_TYPE;
}

static inline PRISM_PURE bool pparse_token_has_binding(PParseToken *tok) {
	return pparse_identifier_binding_kind(tok) != PPARSE_BINDING_NONE;
}

static PRISM_PURE bool pparse_token_is_const_object(PParseToken *tok) {
	PParseTypedefEntry *entry = pparse_typedef_lookup(tok);
	return entry && entry->is_shadow && entry->is_const;
}

// Enforces ISO C11 §6.2.3 namespace separation: tag names live in a
static PRISM_PURE PParseTypedefEntry *pparse_tag_lookup(PParseToken *tok) {
	if (!pparse_is_identifier_like(tok)) return NULL;
	unsigned c0 = tok->ch0, tl = tok->len;
	if (!(pparse_typedef_table.bloom & (1ULL << ((c0 ^ tl) & 63)))) return NULL;
	char *name = pparse_loc(tok);
	uint32_t cur = pparse_idx(tok);
	if (pparse_td_tl_idxs) {
		uint32_t hash = pparse_token_name_hash(tok);
		int idx = pparse_hashmap_index_hashed(&pparse_typedef_table.name_map, name, (int)tok->len, hash);
		while (idx >= pparse_td_tl_count_at_build) {
			PParseTypedefEntry *e = &pparse_typedef_table.entries[idx];
			if (e->is_struct_tag && e->token_index <= cur && cur < e->scope_close_idx)
				return e;
			idx = e->prev_index;
		}
		uint64_t desc = idx >= 0 ? pparse_td_tl_descs[idx] : 0;
		if (desc) return pparse_td_lookup_timeline(desc, cur, true);
		while (idx >= 0) {
			PParseTypedefEntry *e = &pparse_typedef_table.entries[idx];
			if (e->is_struct_tag && e->token_index <= cur && cur < e->scope_close_idx)
				return e;
			idx = e->prev_index;
		}
		return NULL;
	}
	int idx = pparse_hashmap_index_hashed(&pparse_typedef_table.name_map, name, tok->len, pparse_token_name_hash(tok));
	while (idx >= 0) {
		PParseTypedefEntry *e = &pparse_typedef_table.entries[idx];
		if (e->is_struct_tag && e->token_index <= cur && cur < e->scope_close_idx)
			return e;
		idx = e->prev_index;
	}
	return NULL;
}

static inline PRISM_PURE int pparse_typedef_flags(PParseToken *tok) {
	PParseTypedefEntry *e = pparse_typedef_lookup(tok);
	if (!e) {
		PParseBoundsArrayEntry *be = pparse_bounds_array_lookup(tok);
		if (!be || be->blocks_outer) return 0;
		return PPARSE_TDF_ARRAY | (be->is_vla_var ? PPARSE_TDF_VLA : 0);
	}
	if (e->is_enum_const) return PPARSE_TDF_ENUM_CONST;
	if (e->is_shadow) {
		int fl = (e->is_volatile ? PPARSE_TDF_VOLATILE : 0) |
			 (e->has_volatile_member ? PPARSE_TDF_HAS_VOL_MEMBER : 0) |
			 (e->is_atomic ? PPARSE_TDF_ATOMIC : 0) | (e->is_array ? PPARSE_TDF_ARRAY : 0);
		/* Decayed params must not inherit PPARSE_TDF_ARRAY from an outer
		 * file-scope array of the same name. */
		if (!(fl & PPARSE_TDF_ARRAY) && !e->is_param) {
			PParseBoundsArrayEntry *be = pparse_bounds_array_lookup(tok);
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
	       (e->is_atomic ? PPARSE_TDF_ATOMIC : 0);
}

static inline PRISM_PURE bool pparse__is_known_typedef(PParseToken *tok) {
	if (__builtin_expect(pparse_p1_typedef_annotated, 1)) return pparse_ann(tok) & P1_IS_TYPEDEF;
	return pparse_typedef_flags(tok) & PPARSE_TDF_TYPEDEF;
}

#define pparse_is_known_typedef(tok) pparse__is_known_typedef(tok)
#define pparse_is_vla_typedef(tok) (pparse_typedef_flags(tok) & PPARSE_TDF_VLA)
#define pparse_is_void_typedef(tok) (pparse_typedef_flags(tok) & PPARSE_TDF_VOID)
#define pparse_is_known_enum_const(tok) (pparse_typedef_flags(tok) & PPARSE_TDF_ENUM_CONST)
#define pparse_is_const_typedef(tok) (pparse_typedef_flags(tok) & PPARSE_TDF_CONST)
#define pparse_is_ptr_typedef(tok) (pparse_typedef_flags(tok) & PPARSE_TDF_PTR)
#define pparse_is_array_typedef(tok) (pparse_typedef_flags(tok) & PPARSE_TDF_ARRAY)
#define pparse_is_func_typedef(tok) (pparse_typedef_flags(tok) & PPARSE_TDF_FUNC)
#define pparse_is_volatile_typedef(tok) (pparse_typedef_flags(tok) & PPARSE_TDF_VOLATILE)
#define pparse_has_volatile_member_typedef(tok) (pparse_typedef_flags(tok) & PPARSE_TDF_HAS_VOL_MEMBER)

static inline bool pparse_token_can_name_function(PParseToken *tok) {
	return tok &&
	       (tok->kind == PPARSE_TK_IDENT || (tok->tag & (PPARSE_TT_DEFER | PPARSE_TT_ORELSE)) || (tok->flags & PPARSE_TF_RAW));
}

static inline bool pparse_is_known_function_call(PParseToken *tok) {
	if (!pparse_function_symbol(tok)) return false;
	PParseToken *next = pparse_skip_noise(pparse_next(tok));
	return next && pparse_match_ch(next, '(');
}

static inline bool pparse_is_empty_function_call(PParseToken *tok) {
	if (!pparse_is_known_function_call(tok)) return false;
	PParseToken *open = pparse_skip_noise(pparse_next(tok));
	return open && pparse_match_ch(open, '(') && pparse_pair(open) && pparse_next(open) == pparse_pair(open);
}

static bool pparse_token_can_precede_function_name(PParseToken *tok) {
	while (tok && (pparse_match_ch(tok, '*') || (tok->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_INLINE))))
		tok = pparse_walk_back(pparse_idx(tok), PPARSE_WB_PAST_NOISE);
	return tok && ((tok->tag & (PPARSE_TT_TYPE | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT)) || pparse_is_known_typedef(tok));
}

static bool pparse_token_can_start_knr_param_decl(PParseToken *tok) {
	return tok && ((tok->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_INLINE | PPARSE_TT_TYPEDEF | PPARSE_TT_SUE |
				    PPARSE_TT_TYPEOF | PPARSE_TT_BITINT)) ||
		       pparse_is_known_typedef(tok));
}

static bool pparse_paren_is_function_params(PParseToken *open) {
	if (!open || !pparse_match_ch(open, '(') || !pparse_pair(open)) return false;
	PParseToken *close = pparse_pair(open);
	PParseToken *after = pparse_skip_asm_specifier_trail(pparse_next(close));
	if (!after ||
	    !(pparse_match_ch(after, '{') || pparse_match_ch(after, ';') || pparse_match_ch(after, ',') || pparse_match_ch(after, '=') ||
	      pparse_match_ch(after, ')') || pparse_token_can_start_knr_param_decl(after)))
		return false;
	/* Keep the `)` in `(*F)(...)`: jumping the pointer group would mistake
	 * typedef/function-pointer parameter dimensions for expression parens. */
	PParseToken *prev = pparse_walk_back(pparse_idx(open), PPARSE_WB_ATTR_NOISE);
	if (prev && pparse_token_can_name_function(prev)) {
		PParseToken *before = pparse_walk_back(pparse_idx(prev), PPARSE_WB_ATTR_NOISE);
		return pparse_token_can_precede_function_name(before);
	}
	if (prev && pparse_match_ch(prev, ')') && pparse_pair(prev)) {
		PParseToken *before = pparse_walk_back(pparse_idx(pparse_pair(prev)), PPARSE_WB_ATTR_NOISE);
		return pparse_token_can_precede_function_name(before);
	}
	return false;
}

static PParseToken *pparse_function_param_open(PParseToken *tok) {
	if (!tok) return NULL;
	int depth = 0;
	for (uint32_t i = pparse_idx(tok); i > 0; i--) {
		PParseToken *t = &pparse_token_pool[i - 1];
		if (t->kind == PPARSE_TK_PREP_DIR) continue;
		if (t->flags & PPARSE_TF_CLOSE) {
			depth++;
			continue;
		}
		if (t->flags & PPARSE_TF_OPEN) {
			if (depth == 0 && pparse_match_ch(t, '(')) return pparse_paren_is_function_params(t) ? t : NULL;
			if (depth > 0) depth--;
			continue;
		}
		if (depth == 0 && (pparse_match_ch(t, ';') || pparse_match_ch(t, '{') || pparse_match_ch(t, '}'))) break;
	}
	return NULL;
}

static PRISM_PURE uint8_t pparse_array_rank_for_tok(PParseToken *t) {
	PParseTypedefEntry *te = pparse_typedef_lookup(t);
	if (te && te->array_rank > 0) return te->array_rank;
	PParseBoundsArrayEntry *be = pparse_bounds_array_lookup(t);
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
	PParseArrayBindingInfo info = {.dim_complete = true};
	if (!pparse_is_identifier_like(tok)) return info;
	PParseTypedefEntry *binding = pparse_typedef_lookup(tok);
	if (binding && (binding->is_param || binding->is_enum_const)) return info;
	PParseBoundsArrayEntry *array = pparse_bounds_array_lookup(tok);
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

static PRISM_PURE bool pparse_array_binding_dim_complete(PParseToken *tok) {
	PParseArrayBindingInfo info = pparse_array_binding_info(tok);
	return info.tracked && info.dim_complete;
}

static void pparse_register_array_binding(PParseToken *tok,
				     uint8_t rank,
				     bool dim_complete,
				     bool is_vla,
				     bool blocks_outer) {
	pparse_bounds_array_add(
	    pparse_loc(tok), tok->len, pparse_idx(tok), rank, dim_complete, is_vla, blocks_outer);
}

static void pparse_register_array_shadow(PParseToken *tok) {
	pparse_register_array_binding(tok, 0, false, false, true);
}

static void pparse_mark_shadow_array(PParseToken *tok, uint8_t rank, bool dim_complete) {
	PParseTypedefEntry *entry = pparse_shadow_entry(tok);
	if (!entry) return;
	entry->is_array = true;
	entry->array_rank = rank;
	entry->array_dim_complete = dim_complete;
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
	return tok && tok->kind == PPARSE_TK_KEYWORD && (tok->flags & PPARSE_TF_SOFT_KW);
}

static inline bool pparse_soft_keyword_decl_name_boundary(PParseToken *tok) {
	PParseToken *after = pparse_skip_noise(pparse_next(tok));
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
	PParseToken *colon = tok ? pparse_skip_noise(pparse_next(tok)) : NULL;
	return tok && pparse_is_identifier_like(tok) && colon && pparse_match_ch(colon, ':') &&
	       !(pparse_next(colon) && pparse_match_ch(pparse_next(colon), ':')) && !(tok->tag & (PPARSE_TT_CASE | PPARSE_TT_DEFAULT));
}

static bool pparse_is_strict_bare_function_call(PParseToken *start, PParseToken *end) {
	PParseToken *tok = pparse_skip_prep_dirs_until(start, end);
	if (!tok || tok == end || !pparse_is_valid_varname(tok) || pparse_is_type_keyword(tok)) return false;
	PParseToken *function = tok;
	tok = pparse_skip_prep_dirs_until(pparse_next(function), end);
	if (!tok || tok == end || !pparse_match_ch(tok, '(') || !(tok->flags & PPARSE_TF_OPEN)) return false;
	PParseToken *close = pparse_pair(tok);
	return close && pparse_skip_prep_dirs_until(pparse_next(close), end) == end &&
	       pparse_function_symbol(function) != PPARSE_FS_NONE;
}

static bool pparse_type_range_has_nonempty_array_dim(PParseToken *start, PParseToken *end) {
	for (PParseToken *tok = start; tok && tok != end; tok = pparse_next(tok)) {
		if (pparse_match_ch(tok, '[') && (tok->flags & PPARSE_TF_OPEN)) {
			PParseToken *next = pparse_next(tok);
			if (next && !pparse_match_ch(next, ']')) return true;
		}
	}
	return false;
}

static inline PRISM_PURE bool pparse_is_expr_ending(PParseToken *t) {
	return (t->kind == PPARSE_TK_IDENT || t->kind == PPARSE_TK_KEYWORD || t->kind == PPARSE_TK_NUM || t->kind == PPARSE_TK_STR) ||
	       pparse_match_set(t, pparse_CH(')') | pparse_CH(']'));
}

static inline PRISM_PURE bool pparse_is_expr_ending_brace(PParseToken *t) {
	return pparse_is_expr_ending(t) || pparse_match_ch(t, '}');
}

static void pparse_enum_constants(PParseToken *tok, int scope_depth) {
	if (!tok || !(pparse_match_ch(tok, '{'))) return;
	tok = pparse_next(tok); // Skip '{'
	while (tok && tok->kind != PPARSE_TK_EOF && !(pparse_match_ch(tok, '}'))) {
		pparse_SKIP_NOISE_CONTINUE(tok);
		if (pparse_is_valid_varname(tok)) {
			pparse_typedef_add_entry(tok, scope_depth, PPARSE_TDK_ENUM_CONST, false, false);
			tok = pparse_next(tok);
			tok = pparse_skip_noise(tok); // Skip C23/GNU attributes on enumerator

			if (tok && pparse_match_ch(tok, '=')) {
				tok = pparse_next(tok);
				while (tok && tok->kind != PPARSE_TK_EOF) {
					if (tok->flags & PPARSE_TF_OPEN) {
						tok = pparse_next(pparse_pair(tok));
						continue;
					}
					if (tok->len == 1 && (tok->ch0 == ',' || tok->ch0 == '}')) break;
					tok = pparse_next(tok);
				}
			}

			if (tok && pparse_match_ch(tok, ',')) tok = pparse_next(tok);
		} else
			tok = pparse_next(tok);
	}
}

static inline PRISM_PURE bool pparse_is_orelse_kw_shadow(PParseToken *tok) {
	if (!(tok->tag & PPARSE_TT_ORELSE)) return false;
	PParseTypedefEntry *te = pparse_typedef_lookup(tok);
	return !te || te->is_shadow;
}

static bool pparse_close_brace_ends_sue_body(PParseToken *tok);
static bool pparse_token_ends_sue_type_specifier(PParseToken *tok);
static bool pparse_close_paren_ends_cast_type_name(PParseToken *tok);
static bool pparse_orelse_is_label_or_goto_target(PParseToken *tok, PParseToken *prev);
static PParseTypeSpec pparse_type_specifier(PParseToken *tok);

static bool pparse_close_paren_ends_type_specifier_ctor(PParseToken *tok);

static inline bool pparse_orelse_shadow_is_kw(PParseToken *prev) {
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
	if (prev->len == 2 && (prev->ch0 == '+' || prev->ch0 == '-') && pparse_loc(prev)[1] == prev->ch0)
		return true;
	return pparse_is_expr_ending_brace(prev);
}

static PParseToken *pparse_find_struct_body_brace(PParseToken *tok) {
	PParseToken *t = pparse_next(tok);
	bool saw_tag = false;
	while (t && t->kind != PPARSE_TK_EOF) {
		pparse_SKIP_NOISE_CONTINUE(t);
		if (!saw_tag && pparse_is_valid_varname(t)) {
			saw_tag = true;
			t = pparse_next(t);
		} else if ((t->tag & PPARSE_TT_QUALIFIER) || pparse_is_type_keyword(t)) {
			t = pparse_next(t);
		} else if (pparse_match_ch(t, ':')) {
			/* C23 enum fixed underlying type: enum E : int { ... }
			 * Also `enum E : typeof(unsigned)` / `_BitInt(N)` / `_Alignas`. */
			t = pparse_next(t);
			while (t && t->kind != PPARSE_TK_EOF) {
				pparse_SKIP_NOISE_CONTINUE(t);
				if ((t->tag & (PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS | PPARSE_TT_ATTR)) &&
				    pparse_next(t) && pparse_match_ch(pparse_next(t), '(') && pparse_pair(pparse_next(t))) {
					t = pparse_next(pparse_pair(pparse_next(t)));
					continue;
				}
				/* `enum E : _Atomic(int) {` */
				if ((t->tag & PPARSE_TT_TYPE) && pparse_equal(t, "_Atomic") && pparse_next(t) &&
				    pparse_match_ch(pparse_next(t), '(') && pparse_pair(pparse_next(t))) {
					t = pparse_next(pparse_pair(pparse_next(t)));
					continue;
				}
				if (pparse_is_c23_attr(t) && pparse_pair(t)) {
					t = pparse_next(pparse_pair(t));
					continue;
				}
				if ((t->tag & PPARSE_TT_QUALIFIER) || pparse_is_type_keyword(t) || pparse_is_known_typedef(t)) {
					t = pparse_next(t);
					continue;
				}
				if (pparse_match_ch(t, '*')) {
					t = pparse_next(t);
					continue;
				}
				break;
			}
		} else
			break;
	}
	return (t && pparse_match_ch(t, '{')) ? t : NULL;
}

/* Unified backward token walker.
 *
 * Indexing conventions (preserved via flag presets above):
 *   PPARSE_WB_FROM_PRED — start at pool[start_idx-1] (past_noise / attr_noise)
 *   otherwise    — start at pool[start_idx]   (pparse_skip_noise / skip_attrs)
 *
 * Flag meanings:
 *   PPARSE_WB_SKIP_PREP       skip PPARSE_TK_PREP_DIR
 *   PPARSE_WB_SKIP_ATTR       skip PPARSE_TT_ATTR keywords
 *   PPARSE_WB_JUMP_GROUPS     jump any PPARSE_TF_CLOSE → matching open (unmatched → NULL)
 *   PPARSE_WB_JUMP_C23_ATTR   jump C23 ]] → [[
 *   PPARSE_WB_JUMP_ALL_PARENS jump every )
 *   PPARSE_WB_JUMP_ATTR_PARENS jump ) only when preceded by ATTR (attr-parens mode)
 */
static PParseToken *pparse_walk_back(uint32_t start_idx, unsigned flags) {
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
				PParseToken *open = pparse_pair(pt);
				if (!open) return NULL;
				pi = pparse_idx(open); /* next iter looks at open-1 */
				continue;
			}
			return pt;
		}

		if ((flags & PPARSE_WB_JUMP_C23_ATTR) && pparse_match_ch(pt, ']') && pparse_pair(pt) &&
		    (pparse_pair(pt)->flags & PPARSE_TF_C23_ATTR)) {
			pi = pparse_idx(pparse_pair(pt));
			continue;
		}

		if (pparse_match_ch(pt, ')') && pparse_pair(pt) &&
		    (flags & (PPARSE_WB_JUMP_ALL_PARENS | PPARSE_WB_JUMP_ATTR_PARENS))) {
			uint32_t open_idx = pparse_idx(pparse_pair(pt));
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

static bool pparse_function_decl_returns_aggregate(PParseToken *function_name) {
	bool saw_aggregate = false, saw_pointer = false, saw_enum = false;
	for (PParseToken *t = pparse_walk_back(pparse_idx(function_name), PPARSE_WB_PAST_NOISE); t;
	     t = pparse_walk_back(pparse_idx(t), PPARSE_WB_PAST_NOISE)) {
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
	return saw_aggregate && !saw_pointer && !saw_enum;
}

static bool pparse_expr_is_aggregate_value(PParseToken *start, PParseToken *end) {
	if (!start || start == end) return false;
	PParseToken *t = start, *limit = end;
	while (pparse_match_ch(t, '(') && pparse_pair(t) && pparse_next(pparse_pair(t)) == limit) {
		limit = pparse_pair(t);
		t = pparse_next(t);
		if (!t || t == limit) return false;
	}
	if (t->kind == PPARSE_TK_IDENT && pparse_next(t) == limit) {
		PParseTypedefEntry *entry = pparse_typedef_lookup(t);
		return entry && entry->is_shadow && entry->is_aggregate && !entry->is_array;
	}
	if (t->kind == PPARSE_TK_IDENT) {
		PParseToken *open = pparse_next(t);
		if (open && pparse_match_ch(open, '(') && pparse_pair(open) && pparse_next(pparse_pair(open)) == limit)
			return pparse_function_symbol(t) == PPARSE_FS_AGGREGATE_RETURN;
	}
	if (pparse_match_ch(t, '(')) {
		PParseToken *inner = pparse_skip_noise(pparse_next(t));
		if (!inner) return false;
		if (inner->tag & PPARSE_TT_SUE) return !pparse_is_enum_kw(inner);
		if (pparse_typedef_flags(inner) & PPARSE_TDF_AGGREGATE) return true;
	}
	return false;
}

static PParseToken *pparse_skip_function_attrs_and_cc(PParseToken *tok) {
	while (tok && tok->kind != PPARSE_TK_EOF) {
		PParseToken *next = pparse_skip_noise(tok);
		if (next != tok) {
			tok = next;
			continue;
		}
		if (tok->flags & PPARSE_TF_MS_CC) {
			tok = pparse_next(tok);
			continue;
		}
		break;
	}
	return tok;
}

/* Pointer stars and their qualifiers are return-type material. Function
 * attributes are intentionally not skipped here: they terminate that range. */
static PParseToken *pparse_skip_return_pointers(PParseToken *tok, bool *is_void) {
	while (tok && tok->kind != PPARSE_TK_EOF) {
		if (tok->kind == PPARSE_TK_PREP_DIR) {
			tok = pparse_next(tok);
			continue;
		}
		if (pparse_match_ch(tok, '*')) {
			tok = pparse_next(tok);
			if (is_void) *is_void = false;
			continue;
		}
		if ((tok->tag & PPARSE_TT_QUALIFIER) && !(tok->tag & PPARSE_TT_ATTR) &&
		    !pparse_is_soft_keyword_identifier(tok)) {
			tok = pparse_next(tok);
			if (is_void) *is_void = false;
			continue;
		}
		break;
	}
	return tok;
}

static PParseToken *pparse_return_type_end_before_attrs(PParseToken *start, PParseToken *parsed_end) {
	PParseToken *t = start;
	while (t && t != parsed_end && t->kind != PPARSE_TK_EOF) {
		if ((t->tag & PPARSE_TT_ATTR) || pparse_is_c23_attr(t) || (t->flags & PPARSE_TF_MS_CC)) {
			PParseToken *p = t;
			while (p && p != parsed_end) {
				if (p->tag & PPARSE_TT_ATTR) {
					PParseToken *next = pparse_next(p);
					p = next && next != parsed_end && pparse_match_ch(next, '(') && pparse_pair(next)
						? pparse_next(pparse_pair(next))
						: next;
					continue;
				}
				if (pparse_is_c23_attr(p) && pparse_pair(p)) {
					p = pparse_next(pparse_pair(p));
					continue;
				}
				if (p->flags & PPARSE_TF_MS_CC) {
					p = pparse_next(p);
					continue;
				}
				return parsed_end;
			}
			return t;
		}
		t = (t->flags & PPARSE_TF_OPEN) && pparse_pair(t) ? pparse_next(pparse_pair(t)) : pparse_next(t);
	}
	return parsed_end;
}

static PParseFunctionReturn pparse_function_return(PParseToken *tok) {
	PParseFunctionReturn result = {0};
	while (tok && tok->kind != PPARSE_TK_EOF) {
		if (tok->tag & (PPARSE_TT_SKIP_DECL | PPARSE_TT_INLINE)) {
			tok = pparse_next(tok);
			continue;
		}
		PParseToken *next = pparse_skip_noise(tok);
		if (next == tok) break;
		tok = next;
	}
	if (!tok || tok->kind == PPARSE_TK_EOF) return result;
	PParseToken *type_start = tok;
	PParseTypeSpec type = pparse_type_specifier(tok);
	if (!type.saw_type) return result;
	bool is_void = type.has_void;
	PParseToken *trimmed = pparse_return_type_end_before_attrs(type_start, type.end);
	if (is_void) {
		for (PParseToken *t = type_start; t && t != trimmed && t->kind != PPARSE_TK_EOF; t = pparse_next(t)) {
			if (pparse_equal(t, "void") || pparse_is_void_typedef(t) || (t->tag & PPARSE_TT_TYPEOF)) break;
			if ((t->tag & PPARSE_TT_TYPE) && !pparse_equal(t, "void")) {
				is_void = false;
				break;
			}
		}
	}
	if (type.is_struct) {
		for (PParseToken *t = type_start; t && t != trimmed && t->kind != PPARSE_TK_EOF; t = pparse_next(t))
			if (pparse_match_ch(t, '{')) return result;
	}

	tok = pparse_skip_return_pointers(trimmed, &is_void);
	PParseToken *ret_end = tok;
	tok = pparse_skip_function_attrs_and_cc(tok);
	if (tok && pparse_is_valid_varname(tok) && pparse_next(tok) && pparse_match_ch(pparse_next(tok), '(')) {
		result.kind = is_void ? PPARSE_FUNC_RETURN_VOID : PPARSE_FUNC_RETURN_VALUE;
		if (!is_void) {
			result.type_start = type_start;
			result.type_end = ret_end;
		}
		return result;
	}

	if (!tok || !pparse_match_ch(tok, '(')) return result;
	PParseToken *outer_open = tok;
	PParseToken *inner = pparse_skip_return_pointers(pparse_next(tok), &is_void);
	inner = pparse_skip_function_attrs_and_cc(inner);
	while (inner && pparse_match_ch(inner, '(')) {
		inner = pparse_skip_return_pointers(pparse_next(inner), NULL);
		inner = pparse_skip_function_attrs_and_cc(inner);
	}
	if (!inner || !pparse_is_valid_varname(inner) || !pparse_next(inner)) return result;
	if (pparse_match_ch(pparse_next(inner), '(')) {
		PParseToken *after_params = pparse_skip_balanced_group(pparse_next(inner));
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
	} else if (pparse_next(inner) == pparse_pair(outer_open)) {
		PParseToken *params = pparse_next(pparse_next(inner));
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
	PPARSE_BRACKET_CONTEXT_KNOWN = 1u << 31,
};

static bool pparse_bracket_in_compound_literal_type(PParseToken *open_bracket) {
	PParseToken *close = pparse_pair(open_bracket);
	if (!close) return false;
	PParseToken *t = pparse_next(close);
	while (t && t->kind != PPARSE_TK_EOF) {
		if (pparse_match_ch(t, '[') && pparse_pair(t) && !(t->flags & PPARSE_TF_C23_ATTR)) {
			t = pparse_next(pparse_pair(t));
			continue;
		}
		if ((t->flags & PPARSE_TF_C23_ATTR) && pparse_pair(t)) {
			t = pparse_next(pparse_pair(t));
			continue;
		}
		break;
	}
	if (!t || !pparse_match_ch(t, ')')) return false;
	PParseToken *after = pparse_skip_noise(pparse_next(t));
	return after && pparse_match_ch(after, '{');
}

static bool pparse_bracket_in_offsetof_member(PParseToken *open_bracket) {
	int depth = 0;
	for (uint32_t i = pparse_idx(open_bracket); i > 0; i--) {
		PParseToken *t = &pparse_token_pool[i - 1];
		if (t->kind == PPARSE_TK_PREP_DIR) continue;
		if (t->flags & PPARSE_TF_CLOSE) {
			depth++;
			continue;
		}
		if (t->flags & PPARSE_TF_OPEN) {
			if (depth == 0 && pparse_match_ch(t, '(')) {
				PParseToken *before = pparse_walk_back(pparse_idx(t), PPARSE_WB_ATTR_NOISE);
				if (!before) return false;
				return (before->kind == PPARSE_TK_IDENT || before->kind == PPARSE_TK_KEYWORD) &&
				       ((before->len == 8 &&
					 prism_memeq_static(pparse_loc(before), "offsetof", 8)) ||
					(before->len == 18 &&
					 prism_memeq_static(pparse_loc(before), "__builtin_offsetof", 18)));
			}
			if (depth > 0) depth--;
			continue;
		}
		if (depth == 0 && (pparse_match_ch(t, ';') || pparse_match_ch(t, '{') || pparse_match_ch(t, '}'))) break;
	}
	return false;
}

static bool pparse_bracket_has_leading_member_designator(PParseToken *member_name) {
	PParseToken *name = member_name;
	while (name && pparse_is_identifier_like(name)) {
		PParseToken *member = pparse_walk_back(pparse_idx(name), PPARSE_WB_PAST_NOISE);
		if (!member || !(member->tag & PPARSE_TT_MEMBER) || !pparse_match_ch(member, '.')) return false;
		PParseToken *left = pparse_walk_back(pparse_idx(member), PPARSE_WB_PAST_NOISE);
		if (!left || pparse_match_ch(left, '{') || pparse_match_ch(left, ',')) return true;
		if (!pparse_is_identifier_like(left)) return false;
		name = left;
	}
	return false;
}

static bool pparse_bracket_is_designator_index(PParseToken *open_bracket) {
	PParseToken *prev = pparse_walk_back(pparse_idx(open_bracket), PPARSE_WB_PAST_NOISE);
	if (!prev) return false;
	if (pparse_match_ch(prev, '{') || pparse_match_ch(prev, ',') || pparse_match_ch(prev, ']')) return true;
	if (!pparse_is_identifier_like(prev)) return false;
	PParseToken *before = pparse_walk_back(pparse_idx(prev), PPARSE_WB_PAST_NOISE);
	if (before && (before->tag & PPARSE_TT_MEMBER))
		return pparse_bracket_has_leading_member_designator(prev) ||
		       pparse_bracket_in_offsetof_member(open_bracket);
	return before && pparse_match_ch(before, ',') && pparse_bracket_in_offsetof_member(open_bracket);
}

static bool pparse_bracket_contains_gnu_range(PParseToken *open_bracket) {
	PParseToken *close = pparse_pair(open_bracket);
	if (!close) return false;
	for (PParseToken *t = pparse_next(open_bracket); t && t != close; t = pparse_next(t)) {
		if (t->kind == PPARSE_TK_PUNCT && t->len == 3 && t->ch0 == '.' && pparse_loc(t)[1] == '.' &&
		    pparse_loc(t)[2] == '.')
			return true;
		if ((t->flags & PPARSE_TF_OPEN) && pparse_pair(t)) t = pparse_pair(t);
	}
	return false;
}

static bool pparse_bracket_in_alignof_type_operand(PParseToken *open_bracket) {
	int depth = 0;
	for (uint32_t i = pparse_idx(open_bracket); i > 0; i--) {
		PParseToken *t = &pparse_token_pool[i - 1];
		if (t->kind == PPARSE_TK_PREP_DIR) continue;
		if (t->flags & PPARSE_TF_CLOSE) {
			depth++;
			continue;
		}
		if (t->flags & PPARSE_TF_OPEN) {
			if (depth == 0 && pparse_match_ch(t, '(')) {
				PParseToken *before = pparse_walk_back(pparse_idx(t), PPARSE_WB_ATTR_NOISE);
				if (!before) return false;
				if ((before->kind == PPARSE_TK_IDENT || before->kind == PPARSE_TK_KEYWORD) &&
				    ((before->len == 8 &&
				      prism_memeq_static(pparse_loc(before), "_Alignof", 8)) ||
				     (before->len == 7 &&
				      prism_memeq_static(pparse_loc(before), "alignof", 7))))
					return true;
				if ((before->tag & (PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS)) ||
				    ((before->tag & PPARSE_TT_TYPE) && pparse_equal(before, "_Atomic")))
					continue;
				return false;
			}
			if (depth > 0) depth--;
			continue;
		}
		if (depth == 0 && (pparse_match_ch(t, ';') || pparse_match_ch(t, '{') || pparse_match_ch(t, '}'))) break;
	}
	return false;
}

static bool pparse_bracket_in_generic_association_type(PParseToken *open_bracket) {
	PParseToken *gen_open = NULL;
	for (uint32_t i = pparse_idx(open_bracket); i > 0; i--) {
		PParseToken *t = &pparse_token_pool[i - 1];
		if (t->kind == PPARSE_TK_PREP_DIR) continue;
		if (!(t->flags & PPARSE_TF_OPEN) || !pparse_match_ch(t, '(') || !pparse_pair(t) ||
		    pparse_idx(pparse_pair(t)) <= pparse_idx(open_bracket))
			continue;
		PParseToken *before = pparse_walk_back(pparse_idx(t), PPARSE_WB_ATTR_NOISE);
		if (before && (before->tag & PPARSE_TT_GENERIC)) {
			gen_open = t;
			break;
		}
	}
	if (!gen_open || !pparse_pair(gen_open)) return false;
	PParseToken *gen_close = pparse_pair(gen_open);
	int depth = 0, commas = 0;
	for (PParseToken *t = pparse_next(gen_open); t && t != open_bracket; t = pparse_next(t)) {
		if (t->flags & PPARSE_TF_OPEN)
			depth++;
		else if (t->flags & PPARSE_TF_CLOSE) {
			if (depth > 0) depth--;
		} else if (depth == 0 && pparse_match_ch(t, ','))
			commas++;
	}
	if (commas < 1) return false;
	depth = 0;
	for (PParseToken *t = open_bracket; t && t != gen_close; t = pparse_next(t)) {
		if (t->flags & PPARSE_TF_OPEN)
			depth++;
		else if (t->flags & PPARSE_TF_CLOSE) {
			if (depth > 0) depth--;
		} else if (depth == 0 && pparse_match_ch(t, ':'))
			return true;
		else if (depth == 0 && pparse_match_ch(t, ','))
			return false;
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

static bool pparse_expr_maybe_nonconstant(PParseToken *start, PParseToken *end) {
	for (PParseToken *t = start; t && t != end && t->kind != PPARSE_TK_EOF; t = pparse_next(t)) {
		if (t->flags & PPARSE_TF_OPEN) {
			t = pparse_pair(t);
			continue;
		}
		if (t->kind != PPARSE_TK_IDENT) continue;
		if (pparse_is_type_keyword(t) ||
		    (t->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_ATTR | PPARSE_TT_STORAGE |
			       PPARSE_TT_ALIGNAS | PPARSE_TT_BITINT)))
			continue;
		if (pparse_typedef_lookup(t) || pparse_is_known_enum_const(t)) continue;
		return true;
	}
	return false;
}

static PParseToken *pparse_last_comma_operand(PParseToken *open_paren, PParseToken *close_paren) {
	PParseToken *segment = pparse_next(open_paren);
	for (PParseToken *t = segment; t && t != close_paren; t = pparse_next(t)) {
		if (t->flags & PPARSE_TF_OPEN) {
			t = pparse_pair(t);
			continue;
		}
		if (pparse_match_ch(t, ',')) segment = pparse_next(t);
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
	return (t->flags & PPARSE_TF_SIZEOF) || (t->tag & PPARSE_TT_TYPEOF) ||
	       (t->kind == PPARSE_TK_IDENT && t->len == 18 &&
		prism_memeq_static(pparse_loc(t), "__builtin_offsetof", 18));
}

static void pparse_tag_brackets_in_range(PParseToken *open, PParseToken *close) {
	for (PParseToken *t = pparse_next(open); t && t != close && t->kind != PPARSE_TK_EOF; t = pparse_next(t))
		if (t->ch0 == '[' && (t->flags & PPARSE_TF_OPEN)) pparse_ann(t) |= P1_UNEVAL_BRACKET;
}

static void pparse_tag_postfix_chain_unevaluated(PParseToken *t) {
	while (t && t->kind != PPARSE_TK_EOF) {
		if ((pparse_match_ch(t, '[') || pparse_match_ch(t, '(')) && (t->flags & PPARSE_TF_OPEN) && pparse_pair(t)) {
			if (pparse_match_ch(t, '[')) pparse_ann(t) |= P1_UNEVAL_BRACKET;
			PParseToken *close = pparse_pair(t);
			pparse_tag_brackets_in_range(t, close);
			t = pparse_next(close);
			continue;
		}
		if (pparse_match_ch(t, '.') || pparse_equal(t, "->")) {
			t = pparse_next(t);
			if (pparse_is_value_name_token(t)) t = pparse_next(t);
			continue;
		}
		if (pparse_equal(t, "++") || pparse_equal(t, "--")) {
			t = pparse_next(t);
			continue;
		}
		break;
	}
}

/* Mark brackets suppressed by one core-C unevaluated construct and return the
 * last token already covered, so the dedicated sweep skips nested ranges. */
static PParseToken *pparse_mark_unevaluated_at(PParseToken *t) {
	PParseToken *open = pparse_next(t);
	if (t->flags & PPARSE_TF_STATIC_ASSERT) {
		if (!open || open->ch0 != '(' || !(open->flags & PPARSE_TF_OPEN) || !pparse_pair(open)) return NULL;
		PParseToken *close = pparse_pair(open);
		pparse_tag_brackets_in_range(open, close);
		return close;
	}
	if (t->tag & PPARSE_TT_GENERIC) {
		if (!open || open->ch0 != '(' || !(open->flags & PPARSE_TF_OPEN) || !pparse_pair(open)) return NULL;
		PParseToken *close = pparse_pair(open), *u = pparse_next(open);
		for (int depth = 0; u != close && u->kind != PPARSE_TK_EOF; u = pparse_next(u)) {
			if (u->ch0 == '[' && (u->flags & PPARSE_TF_OPEN)) pparse_ann(u) |= P1_UNEVAL_BRACKET;
			if (u->flags & PPARSE_TF_OPEN) depth++;
			else if (u->flags & PPARSE_TF_CLOSE) depth--;
			else if (depth == 0 && u->ch0 == ',' && u->len == 1) break;
		}
		return u && u != close ? u : close;
	}
	if (!open) return NULL;
	if (open->ch0 == '(' && (open->flags & PPARSE_TF_OPEN)) {
		PParseToken *close = pparse_pair(open);
		if (!close) return NULL;
		pparse_tag_brackets_in_range(open, close);
		pparse_tag_postfix_chain_unevaluated(pparse_next(close));
		return close;
	}
	while (open && open->kind != PPARSE_TK_EOF &&
	       (pparse_match_set(open, pparse_CH('+') | pparse_CH('-') | pparse_CH('!') | pparse_CH('&') | pparse_CH('*')) ||
		pparse_match_ch(open, '~') || pparse_equal(open, "++") || pparse_equal(open, "--")))
		open = pparse_next(open);
	if (pparse_is_value_name_token(open)) pparse_tag_postfix_chain_unevaluated(pparse_next(open));
	return NULL;
}

static void pparse_mark_unevaluated_brackets(void) {
	int file_scope_braces = 0;
	bool file_scope_initializer = false;
	/* Block-scope `static`/`extern`/`_Thread_local`/`constexpr` initializers
	 * also require ICEs (C11 §6.7.9p4) — wrapping g[0] in __prism_bchk breaks
	 * the backend. Track storage/constexpr from the last `;`/`{` body entry. */
	bool static_storage_pending = false;
	bool static_storage_initializer = false;
	PParseToken *pool_end = pparse_token_pool + pparse_token_count;
	for (PParseToken *t = pparse_token_pool + 1; t < pool_end && t->kind != PPARSE_TK_EOF; t++) {
		if ((t->flags & PPARSE_TF_OPEN) && t->ch0 == '{') {
			/* Function/compound body: outer `static void f(){` must not
			 * keep pending across the body. Initializer braces (`= {`)
			 * already have static_storage_initializer set. */
			if (!file_scope_initializer && !static_storage_initializer)
				static_storage_pending = false;
			file_scope_braces++;
		} else if ((t->flags & PPARSE_TF_CLOSE) && t->ch0 == '}') {
			if (file_scope_braces) file_scope_braces--;
		} else if (t->ch0 == ';' && t->len == 1) {
			file_scope_initializer = false;
			static_storage_pending = false;
			static_storage_initializer = false;
		} else if ((t->tag & PPARSE_TT_STORAGE) ||
			   ((t->tag & PPARSE_TT_QUALIFIER) && t->ch0 == 'c' && t->len == 9)) {
			/* static / extern / thread_local / constexpr — not register. */
			if (!(t->tag & PPARSE_TT_REGISTER)) static_storage_pending = true;
		} else if (t->ch0 == '=' && t->len == 1) {
			if (file_scope_braces == 0) file_scope_initializer = true;
			if (static_storage_pending) static_storage_initializer = true;
		}
		if ((file_scope_initializer || static_storage_initializer) &&
		    (t->flags & PPARSE_TF_OPEN) && t->ch0 == '[')
			pparse_ann(t) |= P1_UNEVAL_BRACKET;
		if ((t->flags & PPARSE_TF_STATIC_ASSERT) || (t->tag & PPARSE_TT_GENERIC) ||
		    pparse_is_unevaluated_operand_intro(t)) {
			PParseToken *covered = pparse_mark_unevaluated_at(t);
			if (covered) t = covered;
		}
	}
}

static bool pparse_bounds_is_tracked_array(PParseToken *tok) {
	return pparse_is_value_name_token(tok) && pparse_array_binding_info(tok).tracked;
}

static bool pparse_bounds_expr_base_is_pointer(PParseToken *tok) {
	if (!pparse_is_value_name_token(tok)) return false;
	PParseTypedefEntry *binding = pparse_typedef_lookup(tok);
	return binding && !binding->is_struct_tag && binding->is_ptr;
}

static PParseToken *pparse_bounds_peel_paren_ident(PParseToken *last) {
	if (!pparse_match_ch(last, ')') || !pparse_pair(last)) return last;
	PParseToken *open = pparse_pair(last);
	if (pparse_idx(open) >= 1) {
		PParseToken *before = &pparse_token_pool[pparse_idx(open) - 1];
		if (pparse_is_value_name_token(before) || before->kind == PPARSE_TK_NUM || pparse_match_ch(before, ')') ||
		    pparse_match_ch(before, ']'))
			return last;
	}
	PParseToken *inner = pparse_next(open);
	return pparse_is_value_name_token(inner) && pparse_next(inner) == last ? inner : last;
}

/* Peel nested `(ident)` / `(num)` groups from a subscript's left primary.
 * Unlike pparse_bounds_peel_paren_ident (single layer, call-safe), this is for the
 * commutative brute-scan gate so `(i)[…]` / `((i))[…]` / `(2)[…]` still see
 * ternary/arith hidden arrays in the index. */
static PParseToken *pparse_bounds_peel_index_lhs(PParseToken *last) {
	PParseToken *t = last;
	for (;;) {
		if (!t || !pparse_match_ch(t, ')') || !pparse_pair(t)) return t;
		PParseToken *open = pparse_pair(t);
		PParseToken *inner = pparse_next(open);
		if (!inner) return t;
		if ((pparse_is_value_name_token(inner) || inner->kind == PPARSE_TK_NUM) && pparse_next(inner) == t) {
			t = inner;
			continue;
		}
		/* Nested `((i))`: whole body is one paren group. */
		if (pparse_match_ch(inner, '(') && (inner->flags & PPARSE_TF_OPEN) && pparse_pair(inner) &&
		    pparse_next(pparse_pair(inner)) == t) {
			t = pparse_pair(inner);
			continue;
		}
		return t;
	}
}

static PParseToken *pparse_bounds_find_tracked_array(PParseToken *start, PParseToken *end) {
	for (PParseToken *t = start; t && t != end && t->kind != PPARSE_TK_EOF; t = pparse_next(t)) {
		if ((t->flags & PPARSE_TF_OPEN) && pparse_match_ch(t, '(') && pparse_idx(t) >= 1) {
			PParseToken *prev = &pparse_token_pool[pparse_idx(t) - 1];
			if (pparse_is_unevaluated_operand_intro(prev) && pparse_pair(t)) {
				t = pparse_pair(t);
				continue;
			}
		}
		if (!pparse_is_value_name_token(t) || pparse_is_known_typedef(t)) continue;
		if (pparse_idx(t) >= 1 && (pparse_token_pool[pparse_idx(t) - 1].tag & PPARSE_TT_MEMBER)) continue;
		if (pparse_bounds_is_tracked_array(t)) return t;
	}
	return NULL;
}

/* After `&` / `*`, peel redundant `(ident)` groups: `&(a)` / `*(a)`. */
static PParseToken *pparse_bounds_peel_addr_operand(PParseToken *next, PParseToken *close) {
	while (next && next != close && pparse_match_ch(next, '(') && (next->flags & PPARSE_TF_OPEN) &&
	       pparse_pair(next)) {
		PParseToken *inner = pparse_next(next);
		PParseToken *ic = pparse_pair(next);
		if (!inner || !ic || inner == close) break;
		if (pparse_is_value_name_token(inner) && pparse_next(inner) == ic) {
			next = inner;
			break;
		}
		if (pparse_match_ch(inner, '(')) {
			next = inner;
			continue;
		}
		break;
	}
	return next;
}

/* Scan [first, close) for `&arr` / `*arr` (with redundant parens peeled).
 * Used for both `(&arr[0])[i]` bodies and bare index spans `idx[&arr[0]]`. */
static bool pparse_bounds_span_derives_array(PParseToken *first, PParseToken *close) {
	if (!first || !close || first == close) return false;
	for (PParseToken *t = first; t && t != close && t->kind != PPARSE_TK_EOF; t = pparse_next(t)) {
		if ((t->flags & PPARSE_TF_OPEN) && pparse_match_ch(t, '(') && pparse_idx(t) >= 1) {
			PParseToken *prev = &pparse_token_pool[pparse_idx(t) - 1];
			if (pparse_is_unevaluated_operand_intro(prev) && pparse_pair(t)) {
				t = pparse_pair(t);
				continue;
			}
		}
		/* `&arr` / `&(arr)` and `*arr` / `*(arr)` (row decay on multi-dim)
		 * produce derived pointers the v1 matcher cannot verify. */
		if ((pparse_match_ch(t, '&') || pparse_match_ch(t, '*')) && !(t->flags & PPARSE_TF_OPEN)) {
			PParseToken *next = pparse_bounds_peel_addr_operand(pparse_next(t), close);
			if (next && next != close && pparse_is_value_name_token(next) &&
			    pparse_bounds_is_tracked_array(next))
				return true;
		}
	}
	return false;
}

static bool pparse_bounds_paren_derives_array(PParseToken *open) {
	if (!open || !pparse_match_ch(open, '(') || !(open->flags & PPARSE_TF_OPEN) || !pparse_pair(open)) return false;
	return pparse_bounds_span_derives_array(pparse_next(open), pparse_pair(open));
}

static bool pparse_bounds_paren_has_array_arithmetic(PParseToken *open) {
	if (!open || !pparse_match_ch(open, '(') || !(open->flags & PPARSE_TF_OPEN) || !pparse_pair(open)) return false;
	PParseToken *scan_end = pparse_pair(open);
	PParseToken *lhs = pparse_next(open);
	if (!lhs || lhs == scan_end) return false;
	while (pparse_match_ch(lhs, '(') && (lhs->flags & PPARSE_TF_OPEN) && pparse_pair(lhs)) {
		PParseToken *inner_close = pparse_pair(lhs);
		if (pparse_next(inner_close) != scan_end) break;
		lhs = pparse_next(lhs);
		scan_end = inner_close;
		if (!lhs || lhs == scan_end) return false;
	}
	bool has_addsub = false;
	for (PParseToken *t = lhs; t && t != scan_end && t->kind != PPARSE_TK_EOF; t = pparse_next(t)) {
		if ((t->flags & PPARSE_TF_OPEN) && pparse_pair(t)) {
			t = pparse_pair(t);
			continue;
		}
		if ((pparse_match_ch(t, '+') || pparse_match_ch(t, '-')) && !(t->flags & PPARSE_TF_OPEN)) {
			has_addsub = true;
			break;
		}
	}
	return has_addsub && pparse_bounds_find_tracked_array(lhs, scan_end) != NULL;
}

static bool pparse_token_is_in_unevaluated_operand(PParseToken *tok) {
	int depth = 0;
	for (uint32_t i = pparse_idx(tok); i > 0; i--) {
		PParseToken *t = &pparse_token_pool[i - 1];
		if (t->flags & PPARSE_TF_CLOSE) {
			depth++;
			continue;
		}
		if (t->flags & PPARSE_TF_OPEN) {
			if (depth > 0) {
				depth--;
				continue;
			}
			PParseToken *prev = i >= 2 ? &pparse_token_pool[i - 2] : NULL;
			if (!prev) return false;
			if (pparse_is_unevaluated_operand_intro(prev) || (prev->tag & PPARSE_TT_GENERIC) ||
			    (prev->flags & PPARSE_TF_STATIC_ASSERT))
				return true;
			return prev->kind == PPARSE_TK_IDENT &&
			       ((prev->len == 8 && prism_memeq_static(pparse_loc(prev), "_Alignof", 8)) ||
				(prev->len == 7 && prism_memeq_static(pparse_loc(prev), "alignof", 7)));
		}
		if (depth != 0) continue;
		if (pparse_is_unevaluated_operand_intro(t) || (t->tag & PPARSE_TT_GENERIC) ||
		    (t->flags & PPARSE_TF_STATIC_ASSERT)) {
			/* Parenthesized operand (`sizeof(0) + die()`): we already
			 * left that group via the OPEN/CLOSE depth walk above.
			 * Seeing the intro here means a sibling, not an encloser.
			 * Bare form (`sizeof die`) has no `(` after the intro. */
			PParseToken *after = pparse_skip_noise(pparse_next(t));
			if (after && pparse_match_ch(after, '(')) continue;
			return true;
		}
		if (t->kind == PPARSE_TK_IDENT &&
		    ((t->len == 8 && prism_memeq_static(pparse_loc(t), "_Alignof", 8)) ||
		     (t->len == 7 && prism_memeq_static(pparse_loc(t), "alignof", 7)))) {
			PParseToken *after = pparse_skip_noise(pparse_next(t));
			if (after && pparse_match_ch(after, '(')) continue;
			return true;
		}
		if (pparse_match_ch(t, ';') || pparse_match_ch(t, '{') || pparse_match_ch(t, '}') || pparse_match_ch(t, ','))
			return false;
	}
	return false;
}


/* PPARSE_WB_ATTR_NOISE: predecessor skipping only attributes / prep dirs — does not
 * jump PPARSE_TF_CLOSE→open groups, so `} (expr)` after `while (c) { }` is not
 * mistaken for closing `while (`. */

/* if/while/for/switch before a condition '(', walking past GNU/C23 attrs.
 * else/do take no condition paren — their body '(' must not match. */
static PParseToken *pparse_ctrl_condition_kw_before_paren(PParseToken *open) {
	if (!open || !pparse_match_ch(open, '(')) return NULL;
	PParseToken *kw = pparse_walk_back(pparse_idx(open), PPARSE_WB_ATTR_NOISE);
	if (kw && (kw->tag & (PPARSE_TT_IF | PPARSE_TT_LOOP | PPARSE_TT_SWITCH)) && !pparse_is_else_or_do(kw)) return kw;
	return NULL;
}

/* C23 `enum Tag : unsigned int {` — `prev` is the type keyword before `{`.
 * `enum` carries PPARSE_TT_TYPE, so check pparse_is_enum_kw before skipping type keywords.
 * Also accepts `enum E : typeof(unsigned) {` where prev is `)`. */
static bool pparse_is_c23_fixed_underlying_enum(PParseToken *type_kw_before_brace) {
	if (!type_kw_before_brace) return false;
	PParseToken *anchor = type_kw_before_brace;
	if (pparse_match_ch(anchor, ')') && pparse_pair(anchor)) {
		PParseToken *open = pparse_pair(anchor);
		PParseToken *kw = pparse_walk_back(pparse_idx(open), PPARSE_WB_ATTR_NOISE);
		if (kw && ((kw->tag & (PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS)) ||
			   ((kw->tag & PPARSE_TT_TYPE) && pparse_equal(kw, "_Atomic"))))
			anchor = kw;
		else
			return false;
	} else if (!pparse_is_type_keyword(anchor) && !pparse_is_known_typedef(anchor)) {
		return false;
	}
	/* si2-- form: no uint32 underflow at index 0, and pool[0] is inspected. */
	for (uint32_t si2 = pparse_idx(anchor); si2-- > 0;) {
		PParseToken *st = &pparse_token_pool[si2];
		if (st->kind == PPARSE_TK_PREP_DIR) continue;
		if (pparse_is_enum_kw(st)) return true;
		if (pparse_is_type_keyword(st) || (st->tag & PPARSE_TT_QUALIFIER) || pparse_is_known_typedef(st)) continue;
		if (pparse_match_ch(st, ':')) continue;
		if (pparse_match_ch(st, '*')) continue;
		if (pparse_match_ch(st, ']') && pparse_pair(st) && (pparse_pair(st)->flags & PPARSE_TF_C23_ATTR)) {
			si2 = pparse_idx(pparse_pair(st));
			continue;
		}
		if (pparse_match_ch(st, ')') && pparse_pair(st)) {
			si2 = pparse_idx(pparse_pair(st));
			continue;
		}
		if (st->tag & PPARSE_TT_ATTR) continue;
		if (pparse_is_valid_varname(st)) continue; // enum tag name
		break;
	}
	return false;
}

static bool pparse_close_brace_ends_sue_body(PParseToken *tok) {
	if (!tok || !pparse_match_ch(tok, '}') || !pparse_pair(tok)) return false;
	PParseToken *open = pparse_pair(tok);
	for (uint32_t ti = pparse_idx(open); ti > 1;) {
		PParseToken *t = &pparse_token_pool[ti - 1];
		if (t->kind == PPARSE_TK_PREP_DIR) {
			ti--;
			continue;
		}
		if (pparse_match_ch(t, ']') && pparse_pair(t) && (pparse_pair(t)->flags & PPARSE_TF_C23_ATTR)) {
			ti = pparse_idx(pparse_pair(t));
			continue;
		}
		if (pparse_match_ch(t, ')') && pparse_pair(t)) {
			PParseToken *open_paren = pparse_pair(t);
			PParseToken *before = pparse_idx(open_paren) > 1 ? &pparse_token_pool[pparse_idx(open_paren) - 1] : NULL;
			if (before && (before->tag & (PPARSE_TT_ATTR | PPARSE_TT_ALIGNAS | PPARSE_TT_BITINT | PPARSE_TT_TYPEOF))) {
				ti = pparse_idx(before);
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
	if (!tok) return false;
	if (pparse_close_brace_ends_sue_body(tok)) return true;
	PParseToken *effective = pparse_walk_back(pparse_idx(tok) + 1, PPARSE_WB_PAST_NOISE);
	if (effective && effective != tok) return pparse_token_ends_sue_type_specifier(effective);
	if (tok->tag & PPARSE_TT_SUE) return true;
	if (pparse_is_identifier_like(tok)) {
		PParseToken *before = pparse_walk_back(pparse_idx(tok), PPARSE_WB_PAST_NOISE);
		return before && (before->tag & PPARSE_TT_SUE);
	}
	return false;
}

static bool pparse_close_paren_ends_cast_type_name(PParseToken *tok) {
	if (!tok || !pparse_match_ch(tok, ')') || !pparse_pair(tok)) return false;
	PParseToken *open = pparse_pair(tok);
	PParseToken *before_open = pparse_walk_back(pparse_idx(open), PPARSE_WB_PAST_NOISE);
	if (before_open &&
	    (pparse_is_sizeof_like(before_open) || (before_open->tag & (PPARSE_TT_TYPEOF | PPARSE_TT_ALIGNAS | PPARSE_TT_BITINT))))
		return false;
	PParseToken *inner = pparse_skip_noise(pparse_next(open));
	if (!inner || inner == tok) return false;
	PParseTypeSpec type = pparse_type_specifier(inner);
	if (!type.saw_type) return false;
	PParseToken *t = type.end;
	while (t && t != tok && t->kind != PPARSE_TK_EOF) {
		PParseToken *next = pparse_skip_noise(t);
		if (next != t) {
			t = next;
			continue;
		}
		if (pparse_match_ch(t, '*') || (t->tag & PPARSE_TT_QUALIFIER)) {
			t = pparse_next(t);
			continue;
		}
		if ((pparse_match_ch(t, '(') || pparse_match_ch(t, '[')) && pparse_pair(t)) {
			t = pparse_next(pparse_pair(t));
			continue;
		}
		return false;
	}
	return t == tok;
}

/* `)` that closes typeof(…), _BitInt(…), _Alignas(…), or _Atomic(…) —
 * a type-specifier constructor, so a following soft keyword is not an
 * expression operator (`typeof(_Atomic(int) orelse 0)` is type-junk). */
static bool pparse_close_paren_ends_type_specifier_ctor(PParseToken *tok) {
	if (!tok || !pparse_match_ch(tok, ')') || !pparse_pair(tok)) return false;
	PParseToken *open = pparse_pair(tok);
	PParseToken *before_open = pparse_walk_back(pparse_idx(open), PPARSE_WB_PAST_NOISE);
	if (!before_open) return false;
	if (before_open->tag & (PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS)) return true;
	return (before_open->tag & PPARSE_TT_TYPE) && pparse_equal(before_open, "_Atomic");
}

static bool pparse_orelse_is_label_or_goto_target(PParseToken *tok, PParseToken *prev) {
	if (prev && ((prev->tag & PPARSE_TT_GOTO) || pparse_is_gnu_label_decl_head(prev) || pparse_equal_2(prev, "&&")))
		return true;
	if (!tok) return false;
	PParseToken *next = pparse_skip_noise(pparse_next(tok));
	return next && pparse_match_ch(next, ':') && !(pparse_next(next) && pparse_match_ch(pparse_next(next), ':'));
}

static inline bool pparse_decl_paren_predecessor_is_type(PParseToken *p) {
	return p && (pparse_is_type_keyword(p) || (p->tag & (PPARSE_TT_TYPEOF | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE)) ||
		     pparse_match_ch(p, '*') || pparse_is_known_typedef(p));
}

static inline bool pparse_is_array_bracket_predecessor(PParseToken *t) {
	if (pparse_is_type_keyword(t) || (t->tag & PPARSE_TT_QUALIFIER) || pparse_is_known_typedef(t) || (pparse_match_ch(t, '*')) ||
	    (pparse_match_ch(t, '}')))
		return true;
	if (pparse_is_identifier_like(t)) {
		PParseToken *b = pparse_walk_back(pparse_idx(t), PPARSE_WB_PAST_NOISE);
		return b && (b->tag & PPARSE_TT_SUE);
	}
	if (pparse_match_ch(t, ']')) {
		PParseToken *open = pparse_pair(t);
		if (!open) return true;
		PParseToken *before_open = pparse_walk_back(pparse_idx(open), PPARSE_WB_PAST_NOISE);
		if (!before_open) return true;
		if (pparse_decl_paren_predecessor_is_type(before_open)) return true;
		if (pparse_match_ch(before_open, ']')) return pparse_is_array_bracket_predecessor(before_open);
		return false;
	}
	if (pparse_match_ch(t, ')')) {
		PParseToken *open = pparse_pair(t);
		if (!open) return true; // no match info — conservatively assume type
		PParseToken *before_open = pparse_walk_back(pparse_idx(open), PPARSE_WB_PAST_NOISE);
		if (!before_open) return true;
		return pparse_decl_paren_predecessor_is_type(before_open);
	}
	return false;
}

static bool pparse_array_size_is_vla_impl(PParseToken *open_bracket, int depth) {
	if (depth > 256) pparse_error_tok(open_bracket, "array dimension nesting depth exceeds 256");
	PParseToken *close = pparse_pair(open_bracket);
	if (!close) return false;
	PParseToken *tok = pparse_next(open_bracket);
	while (tok != close) {
		if (pparse_match_ch(tok, '[')) {
			if (pparse_array_size_is_vla_impl(tok, depth + 1)) return true;
			tok = pparse_skip_balanced_group(tok);
			continue;
		}
		if (pparse_match_ch(tok, '(') && pparse_next(tok) && pparse_match_ch(pparse_next(tok), '{')) return true;
		if (tok->tag & PPARSE_TT_GENERIC) return true;
		pparse_SKIP_NOISE_CONTINUE(tok);
		if (pparse_is_sizeof_like(tok)) {
			bool is_sizeof = tok->ch0 == 's';
			tok = pparse_next(tok);
			if (tok != close && pparse_match_ch(tok, '(')) {
				PParseToken *end = pparse_skip_balanced_group(tok);
				if (is_sizeof) {
					PParseToken *prev_inner = tok;
					for (PParseToken *inner = pparse_next(tok); inner && inner != end;
					     prev_inner = inner, inner = pparse_next(inner)) {
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
								PParseToken *la = pparse_skip_noise(pparse_next(inner));
								if (la && la != end && pparse_match_ch(la, ')'))
									return true;
							}
							PParseToken *ni = pparse_next(inner);
							bool has_next = ni && ni != end;
							PParseToken *eff_prev = prev_inner;
							uint32_t pi = pparse_idx(eff_prev);
							while (eff_prev->ch0 == '(' && pi > pparse_idx(tok) + 1)
								eff_prev = &pparse_token_pool[--pi];
							PParseToken *eff_next = ni;
							while (has_next && eff_next && eff_next->ch0 == ')' &&
							       eff_next != end) {
								eff_next = pparse_next(eff_next);
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
							inner = pparse_pair(inner);
							if (!inner || inner == end) break;
							continue;
						}
						if (pparse_is_valid_varname(inner) && !pparse_is_type_keyword(inner) &&
						    !pparse_is_known_typedef(inner) && !pparse_is_known_enum_const(inner) &&
						    pparse_next(inner) && inner != end &&
						    pparse_match_ch(pparse_next(inner), '(')) {
							PParseToken *call_end =
							    pparse_skip_balanced_group(pparse_next(inner));
							bool is_deref = (pparse_match_ch(prev_inner, '*')) ||
									(call_end && call_end != end &&
									 ((pparse_match_ch(call_end, '[')) ||
									  (call_end->tag & PPARSE_TT_MEMBER)));
							if (is_deref)
								for (PParseToken *a = pparse_next(pparse_next(inner));
								     a && a != call_end;
								     a = pparse_next(a))
									if (pparse_is_valid_varname(a) &&
									    !pparse_is_known_enum_const(a) &&
									    !pparse_is_type_keyword(a))
										return true;
							prev_inner = inner;
							inner = call_end;
							if (!inner || inner == end) break;
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
						tok = pparse_next(tok);
						continue;
					}
					break;
				}
				if (tok != close) {
					if (tok->flags & PPARSE_TF_OPEN) {
						PParseToken *m = pparse_pair(tok);
						if (m) tok = pparse_next(m);
						if (tok != close && pparse_match_ch(tok, '{')) {
							m = pparse_pair(tok);
							if (m) tok = pparse_next(m);
						}
					} else {
						if (pparse_is_identifier_like(tok) && pparse_is_vla_typedef(tok))
							return true;
						tok = pparse_next(tok);
					}
				}
				while (tok != close) {
					if (tok->tag & PPARSE_TT_MEMBER) {
						tok = pparse_next(tok);
						if (tok != close) tok = pparse_next(tok);
					} else if (tok->flags & PPARSE_TF_OPEN) {
						PParseToken *m = pparse_pair(tok);
						if (!m) break;
						tok = pparse_next(m);
					} else
						break;
				}
			}
			continue;
		}

		if ((tok->tag & PPARSE_TT_MEMBER) ||
		    (pparse_is_valid_varname(tok) && !pparse_is_known_enum_const(tok) && !pparse_is_type_keyword(tok)))
			return true;
		tok = pparse_next(tok);
	}
	return false;
}

static inline bool pparse_array_size_is_vla(PParseToken *open_bracket) {
	return pparse_array_size_is_vla_impl(open_bracket, 0);
}

static PParseToken *pparse_declarator_array_dims(PParseToken *tok, bool *is_vla) {
	for (;;) {
		if (!tok || tok->kind == PPARSE_TK_EOF) return tok;
		if (pparse_match_ch(tok, '[')) {
			pparse_ann(tok) |= P1_DECL_BRACKET;
			if (tok->flags & PPARSE_TF_C23_ATTR) {
				tok = pparse_skip_balanced_group(tok);
				continue;
			}
			if (pparse_array_size_is_vla(tok)) *is_vla = true;
			tok = pparse_skip_balanced_group(tok);
			continue;
		}
		/* GNU attributes and preprocessor noise may separate array dims. */
		PParseToken *after = pparse_skip_noise(tok);
		if (after != tok && pparse_match_ch(after, '[')) {
			tok = after;
			continue;
		}
		return after;
	}
}

/* Pure C declarator analysis. Emission is intentionally outside this API so
 * parse.c can be reused without depending on Prism's output state. */
static PParseDecl pparse_declarator(PParseToken *tok) {
	PParseDecl r = {.end = tok};
	bool is_vla = false;
	int ptr_depth = 0;

#define PPARSE_DECL_EAT_PTRS(extra_ptr_action)                                                                    \
	while (tok && tok->kind != PPARSE_TK_EOF) {                                                                 \
		PParseToken *_n = pparse_skip_noise(tok);                                                                  \
		if (_n != tok) {                                                                             \
			tok = _n;                                                                            \
			continue;                                                                            \
		}                                                                                            \
		if (pparse_match_ch(tok, '*')) {                                                                    \
			r.is_pointer = true;                                                                 \
			r.is_const = false;                                                                  \
			extra_ptr_action;                                                                    \
			if (++ptr_depth > 1024) {                                                            \
				pparse_warn_tok(tok, "pointer depth exceeds 1024; zero-initialization skipped");    \
				r.end = NULL;                                                                \
				return r;                                                                    \
			}                                                                                    \
			tok = pparse_next(tok);                                                                 \
		} else if ((tok->tag & PPARSE_TT_QUALIFIER) &&                                                      \
			   !(pparse_is_soft_keyword_identifier(tok) && pparse_soft_keyword_decl_name_boundary(tok))) {     \
			if (r.is_pointer && (tok->tag & PPARSE_TT_CONST)) r.is_const = true;                        \
			tok = pparse_next(tok);                                                                 \
		} else                                                                                       \
			break;                                                                               \
	}

	PPARSE_DECL_EAT_PTRS((void)0)

	int nested_paren = 0;
	if (pparse_match_ch(tok, '(')) {
		PParseToken *peek = pparse_skip_noise(pparse_next(tok));
		if (!pparse_match_ch(peek, '*') && !pparse_match_ch(peek, '(') && !pparse_is_valid_varname(peek)) {
			r.end = NULL;
			return r;
		}
		tok = pparse_next(tok);
		nested_paren = 1;
		r.has_paren = true;
		PPARSE_DECL_EAT_PTRS(r.paren_pointer = true)
		while (pparse_match_ch(tok, '(')) {
			if (++nested_paren > 1024) {
				pparse_warn_tok(tok, "parenthesization depth exceeds 1024");
				r.end = NULL;
				return r;
			}
			tok = pparse_next(tok);
			PPARSE_DECL_EAT_PTRS(r.paren_pointer = true)
		}
	}
#undef PPARSE_DECL_EAT_PTRS

	if (!pparse_is_valid_varname(tok)) {
		r.end = NULL;
		return r;
	}
	r.var_name = tok;
	tok = pparse_skip_noise(pparse_next(tok));
	if (r.has_paren && pparse_match_ch(tok, '(')) r.is_func_decl = true;
	if (r.has_paren && pparse_match_ch(tok, '[')) {
		r.is_array = r.paren_array = true;
		tok = pparse_declarator_array_dims(tok, &is_vla);
	}
	while (r.has_paren && nested_paren > 0) {
		while (pparse_match_ch(tok, '(') || pparse_match_ch(tok, '[')) {
			if (pparse_match_ch(tok, '(')) tok = pparse_skip_balanced_group(tok);
			else {
				r.is_array = r.paren_array = true;
				tok = pparse_declarator_array_dims(tok, &is_vla);
			}
		}
		if (!pparse_match_ch(tok, ')')) {
			r.end = NULL;
			return r;
		}
		tok = pparse_next(tok);
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
		tok = pparse_declarator_array_dims(tok, &is_vla);
	}
	while (tok && tok->kind != PPARSE_TK_EOF) {
		PParseToken *next = pparse_skip_noise(tok);
		if (next != tok) {
			tok = next;
			continue;
		}
		if (!(tok->tag & PPARSE_TT_ASM)) break;
		tok = pparse_next(tok);
		if (tok && pparse_match_ch(tok, '(')) tok = pparse_skip_balanced_group(tok);
	}

	r.has_init = pparse_match_ch(tok, '=');
	r.is_vla = is_vla;
	r.end = tok;
	return r;
}

// Field declarator names share the member namespace — do not resolve them via
// ordinary pparse_typedef_lookup (ISO C11 §6.2.3).
static inline bool pparse_struct_body_id_is_field_name(PParseToken *id) {
	if (!pparse_is_identifier_like(id)) return false;
	PParseToken *nx = pparse_skip_noise(pparse_next(id));
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
static inline bool pparse_struct_body_field_pred(PParseToken *id, PParseToken *prev, bool vla) {
	if (!pparse_is_identifier_like(id) || pparse_struct_body_id_is_field_name(id)) return false;
	if (prev && (prev->tag & PPARSE_TT_SUE)) {
		PParseTypedefEntry *te = pparse_tag_lookup(id);
		if (vla) return te && te->is_struct_tag && te->is_vla;
		if (te && te->has_volatile_member) return true;
	}
	return vla ? pparse_is_vla_typedef(id) : (pparse_is_volatile_typedef(id) || pparse_has_volatile_member_typedef(id));
}

static inline bool pparse_struct_body_field_is_vla_typedef(PParseToken *id, PParseToken *prev) {
	return pparse_struct_body_field_pred(id, prev, true);
}

enum PParseStructBodyScan { PPARSE_SBS_VLA, PPARSE_SBS_VOL };

static bool pparse_struct_body_scan(PParseToken *brace, enum PParseStructBodyScan what) {
	if (!brace || !pparse_match_ch(brace, '{') || !pparse_pair(brace)) return false;
	PParseToken *end = pparse_pair(brace);
	PParseToken *prev = brace;
	for (PParseToken *t = pparse_next(brace); t && t != end; prev = t, t = pparse_next(t)) {
		if (pparse_match_ch(t, '{')) {
			if (pparse_struct_body_scan(t, what)) return true;
			prev = t;
			t = pparse_pair(t);
			continue;
		}
		// Don't skip typeof()/_Atomic() parens — VLA dims / volatile hide inside.
		if ((t->flags & PPARSE_TF_OPEN) && !pparse_match_ch(t, what == PPARSE_SBS_VLA ? '[' : '{') &&
		    !(prev && ((prev->tag & PPARSE_TT_TYPEOF) ||
			       ((prev->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE)) == (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE))))) {
			prev = t;
			t = pparse_pair(t);
			continue;
		}
		if (what == PPARSE_SBS_VLA) {
			if (pparse_match_ch(t, '[') && pparse_array_size_is_vla(t)) return true;
			if (pparse_struct_body_field_is_vla_typedef(t, prev)) return true;
		} else {
			if (t->tag & PPARSE_TT_VOLATILE) return true;
			if (pparse_struct_body_field_pred(t, prev, false)) return true;
		}
	}
	return false;
}

static inline bool pparse_struct_body_contains_vla(PParseToken *brace) {
	return pparse_struct_body_scan(brace, PPARSE_SBS_VLA);
}

static inline bool pparse_struct_body_contains_volatile(PParseToken *brace) {
	return pparse_struct_body_scan(brace, PPARSE_SBS_VOL);
}

static bool pparse_typedef_contains_vla(PParseToken *tok) {
	while (tok && tok->kind != PPARSE_TK_EOF) {
		if (pparse_match_ch(tok, ';')) break;
		if ((tok->flags & PPARSE_TF_OPEN) && !(pparse_match_ch(tok, '['))) {
			tok = pparse_next(pparse_pair(tok));
			continue;
		}
		if (pparse_match_ch(tok, '[') && pparse_array_size_is_vla(tok)) return true;
		tok = pparse_next(tok);
	}
	return false;
}

static PParseToken *pparse_find_boundary_comma(PParseToken *tok) {
	while (tok->kind != PPARSE_TK_EOF) {
		if (tok->flags & PPARSE_TF_OPEN) {
			tok = pparse_next(pparse_pair(tok));
			continue;
		}
		if (pparse_match_ch(tok, ';')) return NULL;
		if (pparse_match_ch(tok, ',')) {
			PParseToken *n = pparse_next(tok);
			if (n) {
				if (pparse_match_ch(n, '(')) {
					PParseToken *inside = pparse_next(n);
					if (inside &&
					    !(inside->tag & (PPARSE_TT_TYPE | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_QUALIFIER)) &&
					    !pparse_is_known_typedef(inside) && !pparse_is_c23_attr(inside))
						return tok;
				} else if ((pparse_match_ch(n, '*')) || (n->tag & PPARSE_TT_QUALIFIER) ||
					   (n->tag & PPARSE_TT_ATTR) || pparse_is_c23_attr(n)) {
					return tok;
				} else if (pparse_is_valid_varname(n) &&
					   !(n->tag & (PPARSE_TT_TYPE | PPARSE_TT_SUE | PPARSE_TT_TYPEOF))) {
					if (pparse_next(n) && pparse_match_ch(pparse_next(n), '(')) {
						PParseToken *inside = pparse_next(pparse_next(n));
						if (inside && (inside->tag &
							       (PPARSE_TT_TYPE | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_QUALIFIER)))
							return tok;
					} else
						return tok;
				}
			}
		}
		tok = pparse_next(tok);
	}
	return NULL;
}

static PParseToken *pparse_find_init_semicolon(PParseToken *open, PParseToken *close) {
	int pd = 0;
	for (PParseToken *s = pparse_next(open); s && s != close; s = pparse_next(s)) {
		if (s->flags & PPARSE_TF_OPEN) pd++;
		else if (s->flags & PPARSE_TF_CLOSE)
			pd--;
		else if (pd == 0 && pparse_match_ch(s, ';'))
			return s;
	}
	return NULL;
}

// not an array-of-VLA object — must not set PParseTypeSpec.is_vla / is_array.
static bool pparse_abstract_declarator_paren_is_pointer_only(PParseToken *open_paren) {
	PParseToken *close = pparse_pair(open_paren);
	if (!close || !(pparse_match_ch(open_paren, '('))) return false;
	for (PParseToken *x = pparse_next(open_paren); x && x != close;) {
		x = pparse_skip_noise(x);
		if (!x || x == close) break;
		if (pparse_match_ch(x, '*')) {
			x = pparse_next(x);
			continue;
		}
		if (x->tag & PPARSE_TT_QUALIFIER) {
			x = pparse_next(x);
			continue;
		}
		if (pparse_match_ch(x, '(') && (x->flags & PPARSE_TF_OPEN)) {
			PParseToken *inner_close = pparse_pair(x);
			if (!inner_close) return false;
			if (!pparse_abstract_declarator_paren_is_pointer_only(x)) return false;
			x = pparse_next(inner_close);
			continue;
		}
		if (pparse_is_identifier_like(x) && x->kind == PPARSE_TK_IDENT) {
			x = pparse_next(x);
			while (x != close && pparse_match_ch(x, '[') && (x->flags & PPARSE_TF_OPEN) && pparse_pair(x)) {
				PParseToken *rb = pparse_pair(x);
				if (!rb) return false;
				x = pparse_next(rb);
			}
			continue;
		}
		return false;
	}
	return true;
}

bool pparse_array_bracket_closes_ptr_to_array(PParseToken *open_bracket, PParseToken *prev) {
	if (!open_bracket || !prev || prev->len != 1 || prev->ch0 != ')') return false;
	PParseToken *open = pparse_pair(prev);
	return open && pparse_abstract_declarator_paren_is_pointer_only(open);
}

static void pparse_scan_paren_for_vla(PParseToken *open, PParseToken *end, PParseTypeSpec *r, bool check_typeof) {
	PParseToken *prev = open;
	int fn_skip = 0;
	for (PParseToken *t = pparse_next(open); t && t != end; prev = t, t = pparse_next(t)) {
		if (pparse_is_c23_attr(t) && pparse_pair(t)) {
			prev = t;
			t = pparse_pair(t);
			continue;
		}
		// Ban control-flow keywords inside type specifier parens.
		// `defer` as an *identifier* (e.g. `typeof(defer)` after
		// `int defer;`) must not trip this — only block-form
		// `defer { ... }` is a real control-flow use here.
		if (t->tag & (PPARSE_TT_GOTO | PPARSE_TT_RETURN | PPARSE_TT_BREAK | PPARSE_TT_CONTINUE)) {
			if (pparse_feat(PPARSE_F_WARN_SAFETY))
				pparse_warn_tok(t,
					 "control flow keywords inside type "
					 "specifiers (typeof() / _Atomic()) may "
					 "corrupt control-flow tracking");
			else
				pparse_error_tok(t,
					  "control flow keywords are not "
					  "allowed inside type specifiers "
					  "(typeof() / _Atomic()); transpiler "
					  "rewrites may duplicate the type "
					  "specifier, which would corrupt "
					  "control-flow tracking");
		} else if ((t->tag & PPARSE_TT_DEFER) && !pparse_typedef_lookup(t) && pparse_next(t) &&
			   (pparse_match_ch(pparse_next(t), '{') || pparse_is_identifier_like(pparse_next(t)))) {
			/* Statement form `defer cleanup();` and block form `defer {`
			 * are keyword uses; a bare/operator-adjacent `defer` (e.g.
			 * `typeof(defer)`, `defer + 1`) stays an identifier. */
			if (pparse_feat(PPARSE_F_WARN_SAFETY))
				pparse_warn_tok(t,
					 "control flow keywords inside type "
					 "specifiers (typeof() / _Atomic()) may "
					 "corrupt control-flow tracking");
			else
				pparse_error_tok(t,
					  "control flow keywords are not "
					  "allowed inside type specifiers "
					  "(typeof() / _Atomic()); transpiler "
					  "rewrites may duplicate the type "
					  "specifier, which would corrupt "
					  "control-flow tracking");
		}
		if (check_typeof && (t->tag & PPARSE_TT_TYPEOF)) r->has_typeof = true;
		if (pparse_match_ch(t, '(')) {
			if (fn_skip > 0) fn_skip++;
			else if (pparse_match_ch(prev, ')'))
				fn_skip = 1;
		} else if (pparse_match_ch(t, ')')) {
			if (fn_skip > 0) fn_skip--;
		}
		if (fn_skip > 0) continue;
		if (!check_typeof && pparse_is_sizeof_like(t)) {
			PParseToken *nx = pparse_next(t);
			if (nx && pparse_match_ch(nx, '(') && pparse_pair(nx)) {
				prev = t;
				t = pparse_pair(nx);
				continue;
			}
		}
		if (pparse_is_enum_kw(t)) {
			PParseToken *brace = pparse_find_struct_body_brace(t);
			if (brace) {
				pparse_enum_constants(brace, 0);
				prev = brace;
				t = pparse_pair(brace);
				continue;
			}
		}
		if (pparse_match_ch(t, '[') && !(t->flags & PPARSE_TF_C23_ATTR) && pparse_is_array_bracket_predecessor(prev)) {
			if (pparse_array_size_is_vla(t)) r->type_vm = true;
			if (!pparse_array_bracket_closes_ptr_to_array(t, prev)) {
				r->is_array = true;
				if (r->type_array_rank < 15) r->type_array_rank++;
				if (pparse_array_size_is_vla(t)) r->is_vla = true;
			}
			continue;
		}
		if (pparse_is_identifier_like(t)) {
			int tf = pparse_typedef_flags(t);
			if (tf & PPARSE_TDF_VLA) {
				r->is_vla = true;
				if (tf & PPARSE_TDF_ARRAY) {
					r->is_array = true;
					uint8_t rk = pparse_array_rank_for_tok(t);
					if (rk > 0 && r->type_array_rank < rk) r->type_array_rank = rk;
				}
				break;
			}
			/* `typeof(a)` / `_Atomic(a)` where `a` is a fixed array shadow. */
			if (tf & PPARSE_TDF_ARRAY) {
				r->is_array = true;
				uint8_t rk = pparse_array_rank_for_tok(t);
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

#define pparse_SKIP_RAW(after, last)                                                                                \
	do {                                                                                                 \
		while ((after) && ((after)->flags & PPARSE_TF_RAW) && !pparse_is_known_typedef(after)) {                   \
			(last) = (after);                                                                    \
			(after) = pparse_skip_noise(pparse_next(after));                                               \
		}                                                                                            \
	} while (0)

static void pparse_apply_typespec_storage_quals(PParseTypeSpec *r, PParseToken *tok) {
	uint32_t tag = tok->tag;
	if ((tag & PPARSE_TT_STORAGE) && tok->ch0 == 'e') r->has_extern = true;
	if ((tag & PPARSE_TT_STORAGE) && tok->ch0 == 's') r->has_static = true;
	if ((tag & PPARSE_TT_STORAGE) && tok->ch0 != 'e' && tok->ch0 != 's') r->has_thread_local = true;
	if (!(tag & PPARSE_TT_QUALIFIER)) return;
	if (tag & PPARSE_TT_VOLATILE) r->has_volatile = true;
	if (tag & PPARSE_TT_REGISTER) r->has_register = true;
	if (tag & PPARSE_TT_CONST) r->has_const = true;
	if (tok->ch0 == 'c' && tok->len == 9) r->has_constexpr = true;
	if (tag & PPARSE_TT_TYPE) {
		if (tok->ch0 == 'a') {
			r->saw_type = true;
			r->has_auto = true;
		} else
			r->has_atomic = true;
	}
}

// Soft-kw typedefs skip soft-kw qualifiers when peeking the name; plain typedefs skip all quals.
static bool pparse_typespec_typedef_name_finishes(PParseToken *tok, bool soft) {
	PParseToken *peek = pparse_next(tok);
	while (peek && (peek->tag & PPARSE_TT_QUALIFIER) && (!soft || !pparse_is_soft_keyword_identifier(peek)))
		peek = pparse_next(peek);
	if (!peek || !pparse_is_valid_varname(peek)) return false;
	PParseToken *after = pparse_next(peek);
	return after && pparse_match_set(after, pparse_CH(';') | pparse_CH('[') | pparse_CH(',') | pparse_CH('='));
}

static PParseTypeSpec pparse_type_specifier(PParseToken *tok) {
	PParseTypeSpec r = {.end = tok};
	while (tok && tok->kind != PPARSE_TK_EOF) {
		PParseToken *next = pparse_skip_noise(tok);
		if (next != tok) {
			tok = next;
			r.end = tok;
			continue;
		}

		if (!r.saw_type && (tok->flags & PPARSE_TF_RAW)) {
			PParseToken *after_raw = pparse_skip_noise(pparse_next(tok));
			bool typedef_kw_prefix =
			    pparse_is_known_typedef(tok) && after_raw &&
			    (pparse_is_type_keyword(after_raw) || pparse_is_known_typedef(after_raw) ||
			     (after_raw->tag &
			      (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_STORAGE | PPARSE_TT_INLINE | PPARSE_TT_TYPEDEF)) ||
			     (after_raw->flags & PPARSE_TF_RAW));
			if (!pparse_is_known_typedef(tok) || typedef_kw_prefix) {
				r.has_raw = true;
				PParseToken *after = after_raw;
				PParseToken *last = tok;
				pparse_SKIP_RAW(after, last);
				tok = pparse_next(last);
				r.end = tok;
				continue;
			}
		}

		uint32_t tag = tok->tag;
		bool is_type = pparse_is_type_keyword(tok);
		if (r.saw_type && pparse_is_soft_keyword_identifier(tok) && pparse_soft_keyword_decl_name_boundary(tok))
			break;
		if (!(tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_INLINE)) && !is_type &&
		    !(tag & (PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS)))
			break;
		if ((tag & PPARSE_TT_INLINE) && !(tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE))) {
			tok = pparse_next(tok);
			r.end = tok;
			continue;
		}

		if (pparse_equal(tok, "void") || pparse_is_void_typedef(tok)) r.has_void = true;
		bool had_type = r.saw_type;
		int tflags = pparse_typedef_flags(tok);
		if ((tflags & PPARSE_TDF_TYPEDEF) && pparse_is_soft_keyword_identifier(tok)) {
			if (had_type) break;
			pparse_typedef_apply_tdf_flags(&r, tflags);
			if (pparse_typespec_typedef_name_finishes(tok, true)) {
				tok = pparse_next(tok);
				r.end = tok;
				r.saw_type = true;
				return r;
			}
			tok = pparse_next(tok);
			r.end = tok;
			r.saw_type = true;
			continue;
		}

		pparse_apply_typespec_storage_quals(&r, tok);

		if (is_type && (tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE)) == (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE) &&
		    !(pparse_next(tok) && pparse_match_ch(pparse_next(tok), '(')))
			is_type = false;
		if (is_type) r.saw_type = true;
		is_type = false;
		// _Atomic(type) specifier form
		if ((tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE)) == (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE) && pparse_next(tok) &&
		    pparse_match_ch(pparse_next(tok), '(')) {
			r.saw_type = true;
			r.has_atomic = true;
			tok = pparse_next(tok);
			PParseToken *inner_start = pparse_skip_noise(pparse_next(tok));
			PParseToken *end = pparse_skip_balanced_group(tok);
			if (inner_start && (inner_start->tag & PPARSE_TT_SUE)) {
				r.is_struct = true;
				if (inner_start->ch0 == 'u') r.is_union = true;
				if (inner_start->ch0 == 'e') r.is_enum = true;
			}
			if (inner_start && pparse_is_identifier_like(inner_start) && pparse_is_known_typedef(inner_start)) {
				r.is_typedef = true;
				if (pparse_typedef_flags(inner_start) & PPARSE_TDF_UNION) r.is_union = true;
				if (pparse_typedef_flags(inner_start) & PPARSE_TDF_AGGREGATE) r.is_struct = true;
			}
			/* `_Atomic(typeof(T))` — propagate aggregate/array from the
			 * typeof operand (bare `_Atomic(typeof(S))` otherwise leaves
			 * is_struct false and false-accepts const+memset). */
			if (inner_start && (inner_start->tag & PPARSE_TT_TYPEOF) && pparse_next(inner_start) &&
			    pparse_match_ch(pparse_next(inner_start), '(')) {
				PParseToken *topen = pparse_next(inner_start);
				PParseToken *tend = pparse_skip_balanced_group(topen);
				bool saw_sue = false;
				bool outer_ptr = false;
				int depth = 0;
				for (PParseToken *t = pparse_next(topen); t && t != tend; t = pparse_next(t)) {
					if ((t->flags & PPARSE_TF_OPEN)) {
						depth++;
						continue;
					}
					if ((t->flags & PPARSE_TF_CLOSE)) {
						if (depth > 0) depth--;
						continue;
					}
					if (depth == 0 && pparse_match_ch(t, '*')) outer_ptr = true;
					if ((t->tag & PPARSE_TT_SUE) || (pparse_typedef_flags(t) & PPARSE_TDF_AGGREGATE))
						r.is_struct = true;
					if ((t->tag & PPARSE_TT_SUE) && t->ch0 == 'u') r.is_union = true;
					if ((t->tag & PPARSE_TT_SUE) && t->ch0 == 'e') r.is_enum = true;
					if (pparse_typedef_flags(t) & PPARSE_TDF_UNION) r.is_union = true;
					if (t->tag & PPARSE_TT_SUE) saw_sue = true;
					if (pparse_is_identifier_like(t) && saw_sue) saw_sue = false;
				}
				if (outer_ptr) {
					r.is_struct = false;
					r.is_union = false;
					r.is_enum = false;
				}
			}
			/* `_Atomic(struct S *)` is a pointer, not a struct value. */
			{
				bool outer_ptr = false;
				int depth = 0;
				for (PParseToken *t = pparse_next(tok); t && t != end; t = pparse_next(t)) {
					if (t->flags & PPARSE_TF_OPEN) {
						depth++;
						continue;
					}
					if (t->flags & PPARSE_TF_CLOSE) {
						if (depth > 0) depth--;
						continue;
					}
					if (depth == 0 && pparse_match_ch(t, '*')) outer_ptr = true;
				}
				if (outer_ptr) {
					r.is_struct = false;
					r.is_union = false;
					r.is_enum = false;
				}
			}
			pparse_scan_paren_for_vla(tok, end, &r, true);
			tok = end;
			r.end = tok;
			continue;
		}

		if (tag & PPARSE_TT_SUE) {
			r.is_struct = true;
			if (tok->ch0 == 'u') r.is_union = true;
			if (tok->ch0 == 'e') r.is_enum = true;
			r.saw_type = true;
			tok = pparse_next(tok);
			while (tok && tok->kind != PPARSE_TK_EOF) {
				pparse_SKIP_NOISE_CONTINUE(tok);
				if ((tok->tag & PPARSE_TT_QUALIFIER) && !pparse_is_soft_keyword_identifier(tok))
					tok = pparse_next(tok);
				else
					break;
			}
			PParseToken *sue_tag = NULL;
			if (tok && pparse_is_valid_varname(tok)) {
				sue_tag = tok;
				tok = pparse_next(tok);
			}
			// C23 enum fixed underlying type: enum E : int { ... }
			if (tok && pparse_match_ch(tok, ':')) {
				tok = pparse_next(tok);
				while (tok && tok->kind != PPARSE_TK_EOF) {
					pparse_SKIP_NOISE_CONTINUE(tok);
					if (pparse_match_ch(tok, '(')) {
						tok = pparse_skip_balanced_group(tok);
						continue;
					}
					if (pparse_is_c23_attr(tok) && pparse_pair(tok)) {
						tok = pparse_pair(tok);
						continue;
					}
					if (pparse_is_type_keyword(tok) || (tok->tag & PPARSE_TT_QUALIFIER)) {
						tok = pparse_next(tok);
						continue;
					}
					break;
				}
			}
			if (tok && pparse_match_ch(tok, '{')) {
				if (pparse_struct_body_contains_vla(tok)) r.is_vla = true;
				if (pparse_struct_body_contains_volatile(tok)) r.has_volatile_member = true;
				tok = pparse_skip_balanced_group(tok);
			} else if (sue_tag) {
				PParseTypedefEntry *tag_e = pparse_tag_lookup(sue_tag);
				if (tag_e) {
					if (tag_e->is_vla) r.is_vla = true;
					if (tag_e->has_volatile_member) r.has_volatile_member = true;
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
			if (pparse_equal(tok, "__auto_type")) r.has_auto = true;
			tok = pparse_next(tok);
			if (tok && pparse_match_ch(tok, '(')) {
				PParseToken *end = pparse_skip_balanced_group(tok);
				if (pparse_next(tok) && pparse_equal(pparse_next(tok), "void") &&
				    pparse_next(pparse_next(tok)) == pparse_pair(tok))
					r.has_void = true;
				{
					bool saw_sue = false;
					bool outer_ptr = false;
					int depth = 0;
					for (PParseToken *t = pparse_next(tok); t && t != end; t = pparse_next(t)) {
						if ((t->tag & PPARSE_TT_ATTR) && pparse_next(t) &&
						    pparse_match_ch(pparse_next(t), '(') && pparse_pair(pparse_next(t))) {
							t = pparse_pair(pparse_next(t));
							continue;
						}
						if (pparse_is_c23_attr(t) && pparse_pair(t)) {
							t = pparse_pair(t);
							continue;
						}
						if (pparse_match_ch(t, '{') && saw_sue) {
							if (pparse_struct_body_contains_volatile(t))
								r.has_volatile_member = true;
							t = pparse_pair(t);
							saw_sue = false;
							continue;
						}
						if (t->flags & PPARSE_TF_OPEN) {
							depth++;
							continue;
						}
						if (t->flags & PPARSE_TF_CLOSE) {
							if (depth > 0) depth--;
							continue;
						}
						/* `typeof(struct S *)` is a pointer type — the SUE
						 * must not mark the typeof result as a struct value. */
						if (depth == 0 && pparse_match_ch(t, '*')) outer_ptr = true;
						if (!is_unqual) {
							if (t->tag & PPARSE_TT_VOLATILE) r.has_volatile = true;
							if (t->tag & PPARSE_TT_CONST) r.has_const = true;
							if ((t->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE)) ==
							    (PPARSE_TT_QUALIFIER | PPARSE_TT_TYPE))
								r.has_atomic = true;
						}
						if ((t->tag & PPARSE_TT_SUE) || (pparse_typedef_flags(t) & PPARSE_TDF_AGGREGATE))
							r.is_struct = true;
						if ((t->tag & PPARSE_TT_SUE) && t->ch0 == 'u') r.is_union = true;
						if ((t->tag & PPARSE_TT_SUE) && t->ch0 == 'e') r.is_enum = true;
						if (pparse_typedef_flags(t) & PPARSE_TDF_UNION) r.is_union = true;
						if (t->tag & PPARSE_TT_SUE) {
							saw_sue = true;
							continue;
						}
						if (pparse_is_identifier_like(t)) {
							// ISO C11 §6.2.3 namespace separation.
							if (saw_sue) {
								PParseTypedefEntry *tag_e = pparse_tag_lookup(t);
								if (tag_e) {
									if (tag_e->is_vla) r.is_vla = true;
									if (tag_e->has_volatile_member)
										r.has_volatile_member = true;
								}
								saw_sue = false;
							}
							if (!is_unqual) {
								int tf = pparse_typedef_flags(t);
								if (tf & PPARSE_TDF_VOLATILE) r.has_volatile = true;
								if (tf & PPARSE_TDF_HAS_VOL_MEMBER)
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
				pparse_scan_paren_for_vla(tok, end, &r, false);
				tok = end;
			}
			r.end = tok;
			continue;
		}

		if (tag & (PPARSE_TT_BITINT | PPARSE_TT_ATTR | PPARSE_TT_ALIGNAS)) {
			if (tag & PPARSE_TT_BITINT) r.saw_type = true;
			if (tag & PPARSE_TT_ALIGNAS) r.has_alignas = true;
			PParseToken *kw = tok;
			tok = pparse_next(tok);
			if (tok && pparse_match_ch(tok, '(')) {
				if (pparse_feat(PPARSE_F_ORELSE) && (kw->tag & (PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS))) {
					PParseToken *close = pparse_pair(tok);
					for (PParseToken *s = pparse_next(tok); s && s != close; s = pparse_next(s))
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
			pparse_typedef_apply_tdf_flags(&r, tflags);
			if (pparse_typespec_typedef_name_finishes(tok, false)) {
				tok = pparse_next(tok);
				r.end = tok;
				r.saw_type = true;
				return r;
			}
		}

		tok = pparse_next(tok);
		r.end = tok;
	}

	return r;
}

static void pparse_typedef_declaration(PParseToken *tok, int scope_depth) {
	PParseToken *typedef_start = tok;
	tok = pparse_next(tok); // Skip 'typedef'
	PParseToken *type_start = tok;
	PParseTypeSpec type_spec = pparse_type_specifier(tok);
	tok = type_spec.end;
	bool is_vla = type_spec.is_vla || pparse_typedef_contains_vla(typedef_start);
	bool base_is_const = type_spec.has_const;
	if (!base_is_const) {
		for (PParseToken *t = type_start; t && t != tok; t = pparse_next(t))
			if (pparse_is_const_typedef(t)) {
				base_is_const = true;
				break;
			}
	}

	bool base_is_volatile = type_spec.has_volatile;
	if (!base_is_volatile) {
		for (PParseToken *t = type_start; t && t != tok; t = pparse_next(t))
			if (pparse_is_volatile_typedef(t)) {
				base_is_volatile = true;
				break;
			}
	}

	bool base_has_volatile_member = type_spec.has_volatile_member;
	if (!base_has_volatile_member) {
		for (PParseToken *t = type_start; t && t != tok; t = pparse_next(t))
			if (pparse_has_volatile_member_typedef(t)) {
				base_has_volatile_member = true;
				break;
			}
	}

	bool base_is_void = type_spec.has_void;
	bool base_is_ptr = false;
	bool base_is_array = false;
	bool base_is_func = false;
	uint8_t base_array_rank = 0;
	for (PParseToken *bt = type_start; bt && bt != type_spec.end; bt = pparse_next(bt)) {
		if (pparse_is_ptr_typedef(bt)) {
			base_is_ptr = true;
			break;
		}
		if (pparse_is_array_typedef(bt)) {
			base_is_array = true;
			base_array_rank = pparse_array_rank_for_tok(bt);
			break;
		}
		if (pparse_is_func_typedef(bt)) {
			base_is_func = true;
			break;
		}
	}

	// redefinitions correctly shadow outer tags (C11 §6.2.1p4).
	if (type_spec.is_struct) {
		for (PParseToken *bt = type_start; bt && bt != type_spec.end; bt = pparse_next(bt)) {
			if (bt->tag & PPARSE_TT_SUE) {
				PParseToken *tag = pparse_skip_noise(pparse_next(bt));
				while (tag && (tag->tag & PPARSE_TT_QUALIFIER) && !pparse_is_soft_keyword_identifier(tag))
					tag = pparse_skip_noise(pparse_next(tag));
				if (tag && pparse_is_valid_varname(tag)) {
					PParseTypedefEntry *te = pparse_typedef_add_entry(
					    tag, scope_depth, PPARSE_TDK_STRUCT_TAG, is_vla, false);
					if (te) {
						te->is_aggregate = !type_spec.is_enum;
						if (base_has_volatile_member) te->has_volatile_member = true;
					}
				}
				break;
			}
		}
	}
	while (tok && !(pparse_match_ch(tok, ';')) && tok->kind != PPARSE_TK_EOF) {
		PParseDecl decl = pparse_declarator(tok);
		if (decl.var_name) {
			bool is_void =
			    base_is_void && !decl.is_pointer && !decl.is_array && !decl.is_func_ptr;
			bool is_const = (decl.is_pointer || decl.is_func_ptr) ? decl.is_const : base_is_const;
			bool is_ptr = decl.is_pointer || decl.is_func_ptr || base_is_ptr;
			PParseTypedefEntry *added = pparse_typedef_add_entry(
			    decl.var_name, scope_depth, PPARSE_TDK_TYPEDEF, is_vla, is_void);
			if (added) {
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
					PParseToken *prev_dt = NULL;
					for (PParseToken *dt = decl.var_name; dt && decl.end && dt != decl.end;) {
						if (pparse_match_ch(dt, '[') && (dt->flags & PPARSE_TF_OPEN)) {
							if (!pparse_array_bracket_closes_ptr_to_array(dt, prev_dt))
								rank++;
							PParseToken *m = pparse_pair(dt);
							dt = m ? pparse_next(m) : pparse_next(dt);
							prev_dt = m;
							continue;
						}
						prev_dt = dt;
						dt = pparse_next(dt);
					}
					rank += (int)base_array_rank;
					if (rank < 1) rank = 1;
					if (rank > 15) rank = PPARSE_ARRAY_RANK_WRAP_ALL;
					added->array_rank = (uint8_t)rank;
				}
				if ((decl.is_array || base_is_array) &&
				    (!decl.is_pointer || decl.paren_array) && !decl.is_func_ptr) {
					bool dim_complete = false;
					if (decl.is_array) {
						for (PParseToken *dt = decl.var_name;
						     dt && decl.end && dt != decl.end;
						     dt = pparse_next(dt)) {
							if (pparse_match_ch(dt, '[')) {
								PParseToken *nx = pparse_next(dt);
								if (nx && !pparse_match_ch(nx, ']'))
									dim_complete = true;
								break;
							}
						}
						if (!dim_complete && decl.end && pparse_match_ch(decl.end, '='))
							dim_complete = true;
					}
					if (!dim_complete && base_is_array) {
						for (PParseToken *bt = type_start; bt && bt != type_spec.end;
						     bt = pparse_next(bt)) {
							if (pparse_is_array_typedef(bt)) {
								PParseTypedefEntry *bte = pparse_typedef_lookup(bt);
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
					PParseToken *after_name = pparse_skip_noise(pparse_next(decl.var_name));
					if (after_name && pparse_match_ch(after_name, '(')) added->is_func = true;
				}
				if (decl.is_func_ptr && !decl.paren_pointer) added->is_func = true;
				if (base_is_func && !decl.is_pointer && !decl.is_array && !decl.is_func_ptr)
					added->is_func = true;
			}
		}
		tok = decl.end ? decl.end : pparse_next(tok);
		while (tok && !(pparse_match_ch(tok, ',')) && !(pparse_match_ch(tok, ';')) && tok->kind != PPARSE_TK_EOF) {
			if (pparse_match_ch(tok, '(')) tok = pparse_skip_balanced_group(tok);
			else if (pparse_match_ch(tok, '['))
				tok = pparse_skip_balanced_group(tok);
			else
				tok = pparse_next(tok);
		}

		if (tok && pparse_match_ch(tok, ',')) tok = pparse_next(tok);
	}
}

/* A declaration whose base type is a function type is not an object even
 * when the spelling arrives through typeof. Keep this recognition beside
 * the typedef/function registries instead of making transform passes infer it. */
static bool pparse_typeof_is_function_type(PParseToken *type_start,
				      const PParseTypeSpec *type,
				      const PParseDecl *decl) {
	if (decl->is_pointer || decl->is_array || decl->is_func_ptr) return false;
	for (PParseToken *t = type_start; t && t != type->end; t = pparse_next(t))
		if (pparse_is_func_typedef(t)) return true;
	if (!type->has_typeof) return false;

	for (PParseToken *t = type_start; t && t != type->end; t = pparse_next(t)) {
		if (!(t->tag & PPARSE_TT_TYPEOF)) continue;
		PParseToken *open = pparse_next(t);
		if (!open || !pparse_match_ch(open, '(') || !pparse_pair(open)) break;
		PParseToken *inner = pparse_next(open);
		PParseToken *close = pparse_pair(open);
		while (inner && inner != close && pparse_match_ch(inner, '(') && pparse_pair(inner)) {
			PParseToken *inner_close = pparse_pair(inner);
			if (inner_close && pparse_next(inner_close) == close) {
				inner = pparse_next(inner);
				close = inner_close;
			} else
				break;
		}
		if (!inner || inner == close) break;

		if (inner->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF)) {
			for (PParseToken *s = inner; s && s != close; s = pparse_next(s)) {
				if ((s->tag & PPARSE_TT_TYPEOF) && pparse_next(s) && pparse_match_ch(pparse_next(s), '(') &&
				    pparse_pair(pparse_next(s))) {
					s = pparse_pair(pparse_next(s));
					continue;
				}
				if ((s->tag & (PPARSE_TT_BITINT | PPARSE_TT_ALIGNAS)) && pparse_next(s) &&
				    pparse_match_ch(pparse_next(s), '(') && pparse_pair(pparse_next(s))) {
					s = pparse_pair(pparse_next(s));
					continue;
				}
				if ((s->tag & PPARSE_TT_TYPE) && pparse_equal(s, "_Atomic") && pparse_next(s) &&
				    pparse_match_ch(pparse_next(s), '(') && pparse_pair(pparse_next(s))) {
					s = pparse_pair(pparse_next(s));
					continue;
				}
				if ((s->tag & PPARSE_TT_ATTR) && pparse_next(s) && pparse_match_ch(pparse_next(s), '(') &&
				    pparse_pair(pparse_next(s))) {
					s = pparse_pair(pparse_next(s));
					continue;
				}
				if (pparse_is_c23_attr(s) && pparse_pair(s)) {
					s = pparse_pair(s);
					continue;
				}
				if (pparse_match_ch(s, '[') && pparse_pair(s)) {
					s = pparse_pair(s);
					continue;
				}
				if (pparse_match_ch(s, '(')) {
					PParseToken *after = pparse_skip_noise(pparse_next(s));
					if (after && pparse_match_ch(after, '*')) return false;
					return true;
				}
			}
			break;
		}

		if (pparse_next(inner) != close || !pparse_is_valid_varname(inner)) break;
		PParseTypedefEntry *binding = pparse_typedef_lookup(inner);
		if (binding && binding->is_shadow) return binding->is_func;
		return pparse_function_symbol(inner) != PPARSE_FS_NONE;
	}
	return false;
}

static PParseDeclShape
pparse_classify_decl_shape(PParseToken *type_start, const PParseTypeSpec *type, const PParseDecl *decl) {
	return (PParseDeclShape){
	    .effective_vla = (decl->is_vla && (!decl->paren_pointer || decl->paren_array)) ||
			     (type->is_vla && !decl->is_pointer),
	    .is_aggregate = (decl->is_array && (!decl->paren_pointer || decl->paren_array)) ||
			    ((type->is_struct || type->is_typedef || type->is_array) &&
			     !decl->is_pointer),
	    .is_union_type = type->is_union && !decl->is_pointer,
	    .is_func_type = decl->is_func_decl || pparse_typeof_is_function_type(type_start, type, decl),
	};
}

static bool pparse_decl_has_explicit_const(PParseToken *type_start,
				      const PParseTypeSpec *type,
				      const PParseDecl *decl) {
	if ((type->has_const && !decl->is_func_ptr && !decl->is_pointer) || decl->is_const)
		return true;
	if (decl->is_func_ptr || decl->is_pointer) return false;
	for (PParseToken *t = type_start; t && t != type->end; t = pparse_next(t))
		if (pparse_is_const_typedef(t)) return true;
	return false;
}

static PParseToken *pparse_sue_definition_body(PParseToken *sue_kw) {
	PParseToken *body = pparse_find_struct_body_brace(sue_kw);
	if (body) return body;

	PParseToken *tag = pparse_skip_noise(pparse_next(sue_kw));
	while (tag && (tag->tag & PPARSE_TT_QUALIFIER) && !pparse_is_soft_keyword_identifier(tag))
		tag = pparse_skip_noise(pparse_next(tag));
	if (!tag || !pparse_is_valid_varname(tag)) return NULL;

	PParseTypedefEntry *entry = pparse_tag_lookup(tag);
	if (!entry || !entry->is_struct_tag || entry->token_index >= pparse_token_count) return NULL;
	PParseToken *definition_tag = &pparse_token_pool[entry->token_index];
	PParseToken *definition_kw = pparse_walk_back(pparse_idx(definition_tag), PPARSE_WB_PAST_NOISE);
	if (!definition_kw || !(definition_kw->tag & PPARSE_TT_SUE) || pparse_is_enum_kw(definition_kw)) return NULL;
	return pparse_find_struct_body_brace(definition_kw);
}

static PParseToken *pparse_typedef_type_start(PParseTypedefEntry *entry) {
	if (!entry || entry->token_index >= pparse_token_count) return NULL;
	uint32_t i = entry->token_index;
	while (i > 0) {
		PParseToken *prev = &pparse_token_pool[i - 1];
		if ((prev->flags & PPARSE_TF_CLOSE) && pparse_pair(prev)) {
			i = pparse_idx(pparse_pair(prev));
			continue;
		}
		if (pparse_match_ch(prev, ';')) return NULL;
		if (prev->tag & PPARSE_TT_TYPEDEF) return pparse_skip_noise(pparse_next(prev));
		i--;
	}
	return NULL;
}

static bool pparse_array_dim_is_empty_or_zero(PParseToken *open_bracket) {
	PParseToken *close = pparse_pair(open_bracket);
	if (!open_bracket || !close) return false;
	PParseToken *t = pparse_skip_noise(pparse_next(open_bracket));
	if (!t || t == close) return true;
	return t->kind == PPARSE_TK_NUM && t->len == 1 && t->ch0 == '0' &&
	       pparse_skip_noise(pparse_next(t)) == close;
}

static bool pparse_sue_body_brace_zero_unsafe(PParseToken *brace, int depth);

static bool pparse_type_brace_zero_unsafe(PParseToken *type_start, PParseToken *type_end, int depth) {
	if (!type_start || depth > 16) return false;
	uint32_t range_lo = pparse_idx(type_start);
	uint32_t range_hi = type_end ? pparse_idx(type_end) : UINT32_MAX;
	for (PParseToken *t = type_start; t && t != type_end;) {
		if (!(t->tag & PPARSE_TT_SUE) || pparse_is_enum_kw(t)) {
			t = pparse_next(t);
			continue;
		}
		PParseToken *body = pparse_find_struct_body_brace(t);
		if (!body) body = pparse_sue_definition_body(t);
		if (body && pparse_sue_body_brace_zero_unsafe(body, depth + 1)) return true;
		if (body && pparse_pair(body)) {
			uint32_t bi = pparse_idx(body);
			if (bi >= range_lo && bi < range_hi) {
				PParseToken *after = pparse_next(pparse_pair(body));
				if (!after || pparse_idx(after) >= range_hi) break;
				t = after;
				continue;
			}
		}
		t = pparse_next(t);
	}
	for (PParseToken *t = type_start; t && t != type_end;) {
		if ((t->flags & PPARSE_TF_OPEN) && pparse_match_ch(t, '{') && pparse_pair(t)) {
			uint32_t bi = pparse_idx(t);
			if (bi >= range_lo && bi < range_hi) {
				PParseToken *after = pparse_next(pparse_pair(t));
				if (!after || pparse_idx(after) >= range_hi) break;
				t = after;
				continue;
			}
		}
		if (!pparse_is_identifier_like(t) || !pparse_is_known_typedef(t)) {
			t = pparse_next(t);
			continue;
		}
		PParseTypedefEntry *entry = pparse_typedef_lookup(t);
		if (!entry || entry->is_shadow || entry->is_ptr) {
			t = pparse_next(t);
			continue;
		}
		if (entry->token_index < pparse_token_count && t == &pparse_token_pool[entry->token_index]) {
			t = pparse_next(t);
			continue;
		}
		PParseToken *alias = pparse_typedef_type_start(entry);
		if (alias && entry->token_index < pparse_token_count) {
			PParseToken *name = &pparse_token_pool[entry->token_index];
			if (pparse_type_brace_zero_unsafe(alias, name, depth + 1)) return true;
		}
		t = pparse_next(t);
	}
	return false;
}

static bool pparse_sue_body_brace_zero_unsafe(PParseToken *brace, int depth) {
	if (!brace || !pparse_pair(brace) || depth > 16) return false;
	if (pparse_ann(brace) & P1_ZUNSAFE_KNOWN) return (pparse_ann(brace) & P1_ZUNSAFE) != 0;

	PParseToken *end = pparse_pair(brace);
	bool saw_any = false, needs_full = false;
	for (PParseToken *t = pparse_next(brace); t && t != end;) {
		if (t->kind == PPARSE_TK_PREP_DIR) {
			t = pparse_next(t);
			continue;
		}
		if ((t->flags & PPARSE_TF_OPEN) && pparse_match_ch(t, '{') && pparse_pair(t)) {
			needs_full = saw_any = true;
			t = pparse_next(pparse_pair(t));
			continue;
		}
		if ((pparse_match_ch(t, '[') && !(t->flags & PPARSE_TF_C23_ATTR)) || (t->tag & PPARSE_TT_SUE))
			needs_full = true;
		saw_any = true;
		t = pparse_next(t);
	}
	if (!saw_any) {
		brace->ann |= (uint16_t)(P1_ZUNSAFE_KNOWN | P1_ZUNSAFE);
		return true;
	}
	if (!needs_full) {
		brace->ann |= (uint16_t)P1_ZUNSAFE_KNOWN;
		return false;
	}

	bool saw_sized = false, saw_empty_or_zero = false;
	saw_any = false;
	for (PParseToken *stmt = pparse_skip_noise(pparse_next(brace)); stmt && stmt != end;) {
		if (stmt->kind == PPARSE_TK_PREP_DIR) {
			stmt = pparse_skip_noise(pparse_next(stmt));
			continue;
		}
		if ((stmt->flags & PPARSE_TF_STATIC_ASSERT) || (stmt->tag & PPARSE_TT_SKIP_DECL)) {
			PParseToken *n = pparse_skip_noise(pparse_next(stmt));
			stmt = n && pparse_match_ch(n, '(') && pparse_pair(n)
				   ? pparse_skip_noise(pparse_next(pparse_pair(n)))
				   : pparse_skip_noise(pparse_next(stmt));
			continue;
		}
		PParseTypeSpec member_type = pparse_type_specifier(stmt);
		if (!member_type.saw_type || !member_type.end) {
			PParseToken *next = stmt;
			while (next && next != end && !pparse_match_ch(next, ';'))
				next = (next->flags & PPARSE_TF_OPEN) && pparse_pair(next)
					   ? pparse_next(pparse_pair(next))
					   : pparse_next(next);
			stmt = next && pparse_match_ch(next, ';') ? pparse_skip_noise(pparse_next(next)) : end;
			continue;
		}

		PParseToken *decl_start = pparse_skip_noise(member_type.end);
		bool member_empty_type = pparse_type_brace_zero_unsafe(stmt, member_type.end, depth + 1);
		bool saw_decl = false;
		while (decl_start && decl_start != end && !pparse_match_ch(decl_start, ';')) {
			if (pparse_match_ch(decl_start, ':')) {
				saw_any = saw_sized = true;
				break;
			}
			PParseDecl member = pparse_declarator(decl_start);
			if (!member.end) break;
			saw_decl = saw_any = true;
			bool zero_arr = false;
			if (member.is_array && !member.is_pointer) {
				for (PParseToken *b = decl_start; b && b != member.end; b = pparse_next(b)) {
					if (pparse_match_ch(b, '[') && !(b->flags & PPARSE_TF_C23_ATTR) &&
					    pparse_array_dim_is_empty_or_zero(b)) {
						zero_arr = true;
						break;
					}
					if ((b->flags & PPARSE_TF_OPEN) && pparse_pair(b) && !pparse_match_ch(b, '['))
						b = pparse_pair(b);
				}
			}
			if (member.is_pointer || member.is_func_ptr)
				saw_sized = true;
			else if (zero_arr || member_empty_type)
				saw_empty_or_zero = true;
			else
				saw_sized = true;

			PParseToken *next = member.end;
			while (next && next != end && !pparse_match_ch(next, ',') && !pparse_match_ch(next, ';'))
				next = (next->flags & PPARSE_TF_OPEN) && pparse_pair(next)
					   ? pparse_next(pparse_pair(next))
					   : pparse_next(next);
			if (next && pparse_match_ch(next, ',')) {
				decl_start = pparse_skip_noise(pparse_next(next));
				continue;
			}
			decl_start = next;
			break;
		}
		if (!saw_decl && member_empty_type) saw_any = saw_empty_or_zero = true;
		PParseToken *next_stmt = decl_start;
		while (next_stmt && next_stmt != end && !pparse_match_ch(next_stmt, ';'))
			next_stmt = (next_stmt->flags & PPARSE_TF_OPEN) && pparse_pair(next_stmt)
					? pparse_next(pparse_pair(next_stmt))
					: pparse_next(next_stmt);
		stmt = next_stmt && pparse_match_ch(next_stmt, ';') ? pparse_skip_noise(pparse_next(next_stmt)) : end;
	}
	bool unsafe = !saw_any || !saw_sized || saw_empty_or_zero;
	brace->ann |= (uint16_t)P1_ZUNSAFE_KNOWN;
	if (unsafe) brace->ann |= (uint16_t)P1_ZUNSAFE;
	return unsafe;
}

static bool pparse_type_has_const_subobject(PParseToken *type_start, PParseToken *type_end, int depth);

static bool pparse_aggregate_has_const_subobject(PParseToken *brace, int depth) {
	if (!brace || !pparse_pair(brace) || depth > 32) return false;
	PParseToken *end = pparse_pair(brace);
	for (PParseToken *stmt = pparse_skip_noise(pparse_next(brace)); stmt && stmt != end;) {
		if (stmt->kind == PPARSE_TK_PREP_DIR) {
			stmt = pparse_skip_noise(pparse_next(stmt));
			continue;
		}
		PParseTypeSpec member_type = pparse_type_specifier(stmt);
		bool saw_declarator = false;
		if (member_type.saw_type && member_type.end) {
			PParseToken *decl_start = pparse_skip_noise(member_type.end);
			while (decl_start && decl_start != end && !pparse_match_ch(decl_start, ';')) {
				PParseDecl member = pparse_declarator(decl_start);
				if (!member.end || !member.var_name) break;
				saw_declarator = true;
				if (pparse_decl_has_explicit_const(stmt, &member_type, &member)) return true;
				if (!member.is_pointer && !member.is_func_ptr &&
				    pparse_type_has_const_subobject(stmt, member_type.end, depth + 1))
					return true;

				PParseToken *next = member.end;
				while (next && next != end && !pparse_match_ch(next, ',') && !pparse_match_ch(next, ';'))
					next = (next->flags & PPARSE_TF_OPEN) && pparse_pair(next)
						   ? pparse_next(pparse_pair(next))
						   : pparse_next(next);
				if (next && pparse_match_ch(next, ',')) {
					decl_start = pparse_skip_noise(pparse_next(next));
					continue;
				}
				break;
			}
			if (!saw_declarator &&
			    pparse_type_has_const_subobject(stmt, member_type.end, depth + 1))
				return true;
		}
		PParseToken *next_stmt = stmt;
		while (next_stmt && next_stmt != end && !pparse_match_ch(next_stmt, ';'))
			next_stmt = (next_stmt->flags & PPARSE_TF_OPEN) && pparse_pair(next_stmt)
					? pparse_next(pparse_pair(next_stmt))
					: pparse_next(next_stmt);
		stmt = next_stmt && next_stmt != end ? pparse_skip_noise(pparse_next(next_stmt)) : end;
	}
	return false;
}

static bool pparse_type_has_const_subobject(PParseToken *type_start, PParseToken *type_end, int depth) {
	if (!type_start || depth > 32) return false;
	for (PParseToken *t = type_start; t && t != type_end; t = pparse_next(t)) {
		if ((t->tag & PPARSE_TT_SUE) && !pparse_is_enum_kw(t)) {
			PParseToken *body = pparse_sue_definition_body(t);
			if (body && pparse_aggregate_has_const_subobject(body, depth + 1)) return true;
			continue;
		}
		if (!pparse_is_known_typedef(t)) continue;
		PParseTypedefEntry *entry = pparse_typedef_lookup(t);
		if (!entry || !entry->is_aggregate || entry->is_ptr) continue;
		PParseToken *alias_type = pparse_typedef_type_start(entry);
		if (!alias_type) continue;
		if (entry->token_index < pparse_token_count && t == &pparse_token_pool[entry->token_index]) continue;
		PParseTypeSpec alias = pparse_type_specifier(alias_type);
		if (alias.saw_type && alias.end &&
		    pparse_type_has_const_subobject(alias_type, alias.end, depth + 1))
			return true;
	}
	return false;
}

static bool pparse_type_spec_is_anonymous_sue(PParseToken *type_start, const PParseTypeSpec *type) {
	if (!type->is_struct || type->is_enum) return false;
	for (PParseToken *t = type_start; t && t != type->end; t = pparse_next(t)) {
		if (!(t->tag & PPARSE_TT_SUE)) continue;
		PParseToken *after = pparse_skip_noise(pparse_next(t));
		return after && pparse_match_ch(after, '{');
	}
	return false;
}

static bool pparse_range_has_attribute(PParseToken *start, PParseToken *end, uint32_t extra_tag) {
	for (PParseToken *t = start; t && t != end; t = pparse_next(t)) {
		if ((t->tag & (PPARSE_TT_ATTR | extra_tag)) || pparse_is_c23_attr(t)) return true;
		if ((t->flags & PPARSE_TF_OPEN) && pparse_pair(t)) t = pparse_pair(t);
	}
	return false;
}

static bool pparse_is_const_literal_initializer(PParseToken *eq) {
	PParseToken *t = pparse_next(eq);
	if (!t) return false;
	if (t->kind == PPARSE_TK_STR) {
		PParseToken *next = pparse_next(t);
		while (next && next->kind == PPARSE_TK_STR) next = pparse_next(next);
		return next && pparse_match_ch(next, ';');
	}
	if (!pparse_match_ch(t, '{') || !pparse_pair(t)) return false;
	PParseToken *close = pparse_pair(t);
	bool prev_was_dot = false;
	for (t = pparse_next(t); t && t != close; t = pparse_next(t)) {
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

static PRISM_THREAD_LOCAL int *pparse_sos_do_if_save = NULL;
static PRISM_THREAD_LOCAL int *pparse_sos_do_tn_save = NULL;
static PRISM_THREAD_LOCAL int *pparse_sos_do_snap_start = NULL;
static PRISM_THREAD_LOCAL int pparse_sos_do_cap = 0;
static PRISM_THREAD_LOCAL int *pparse_sos_do_snap_buf = NULL;
static PRISM_THREAD_LOCAL int pparse_sos_snap_cap = 0;
static PRISM_THREAD_LOCAL int *pparse_sos_if_trail_snap = NULL;
static PRISM_THREAD_LOCAL int pparse_sos_if_cap = 0;

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
	if (need <= pparse_sos_do_cap) return true;
	int nc = (int)pparse_vec_grow_cap((size_t)pparse_sos_do_cap, (size_t)need, 128);
	/* Commit each successful realloc immediately — a partial failure must
	 * not leave a TLS pointer at a block realloc already freed. */
	int *a = (int *)realloc(pparse_sos_do_if_save, (size_t)nc * sizeof(int));
	if (a) pparse_sos_do_if_save = a;
	int *b = (int *)realloc(pparse_sos_do_tn_save, (size_t)nc * sizeof(int));
	if (b) pparse_sos_do_tn_save = b;
	int *c = (int *)realloc(pparse_sos_do_snap_start, (size_t)nc * sizeof(int));
	if (c) pparse_sos_do_snap_start = c;
	if (!a || !b || !c) return false;
	pparse_sos_do_cap = nc;
	return true;
}

static inline bool pparse_sos_ensure_snap(int need) {
	return pparse_sos_ensure_intbuf(&pparse_sos_do_snap_buf, &pparse_sos_snap_cap, need);
}

static inline bool pparse_sos_ensure_if(int need) {
	return pparse_sos_ensure_intbuf(&pparse_sos_if_trail_snap, &pparse_sos_if_cap, need);
}

static PParseToken *pparse_skip_one_stmt_impl(PParseToken *tok, uint32_t *cache) {
	int if_depth = 0;
	int do_depth = 0;
	int do_snap_top = 0;
	uint32_t trail[256];
	int tn = 0;
	if (!pparse_sos_ensure_do(128) || !pparse_sos_ensure_snap(1024) || !pparse_sos_ensure_if(512)) return NULL;
	int *do_if_save = pparse_sos_do_if_save;
	int *do_tn_save = pparse_sos_do_tn_save;
	int *do_snap_start = pparse_sos_do_snap_start;
	int *do_snap_buf = pparse_sos_do_snap_buf;
	int *if_trail_snap = pparse_sos_if_trail_snap;
restart:
	tok = pparse_skip_prep_dirs(tok);
	tok = pparse_skip_noise(tok);
	if (!tok || tok->kind == PPARSE_TK_EOF) return NULL;
	if (cache) {
		uint32_t idx = pparse_idx(tok);
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
		tok = pparse_pair(tok);
		goto unwind_if;
	}

	if (tok->tag & PPARSE_TT_IF) {
		if (tok->ch0 == 'e') {
			tok = pparse_next(tok);
			goto restart;
		}
		PParseToken *p = pparse_skip_prep_dirs(pparse_next(tok));
		if (!p || !(pparse_match_ch(p, '(')) || !pparse_pair(p)) return NULL;
		if (!pparse_sos_ensure_if(if_depth + 1)) return NULL;
		if_trail_snap = pparse_sos_if_trail_snap;
		if_trail_snap[if_depth] = tn;
		if_depth++;
		tok = pparse_next(pparse_pair(p));
		goto restart;
	}

	if ((tok->tag & (PPARSE_TT_LOOP | PPARSE_TT_SWITCH)) && tok->ch0 != 'd') {
		PParseToken *p = pparse_skip_prep_dirs(pparse_next(tok));
		if (!p || !(pparse_match_ch(p, '(')) || !pparse_pair(p)) return NULL;
		tok = pparse_next(pparse_pair(p));
		goto restart;
	}

	if ((tok->tag & PPARSE_TT_LOOP) && tok->ch0 == 'd') {
		if (!pparse_sos_ensure_do(do_depth + 1)) return NULL;
		if (!pparse_sos_ensure_snap(do_snap_top + if_depth)) return NULL;
		do_if_save = pparse_sos_do_if_save;
		do_tn_save = pparse_sos_do_tn_save;
		do_snap_start = pparse_sos_do_snap_start;
		do_snap_buf = pparse_sos_do_snap_buf;
		do_if_save[do_depth] = if_depth;
		do_tn_save[do_depth] = tn;
		do_snap_start[do_depth] = do_snap_top;
		for (int i = 0; i < if_depth; i++) do_snap_buf[do_snap_top++] = if_trail_snap[i];
		do_depth++;
		if_depth = 0;
		tok = pparse_next(tok);
		goto restart;
	}

	if (pparse_is_identifier_like(tok) &&
	    !(tok->tag & (PPARSE_TT_CASE | PPARSE_TT_DEFAULT | PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE))) {
		PParseToken *colon = pparse_skip_noise(pparse_next(tok));
		if (colon && pparse_match_ch(colon, ':') && !(pparse_next(colon) && pparse_match_ch(pparse_next(colon), ':'))) {
			tok = pparse_next(colon);
			goto restart;
		}
	}

	if ((tok->tag & (PPARSE_TT_CASE | PPARSE_TT_DEFAULT)) && !pparse_is_known_typedef(tok)) {
		int td = 0;
		for (PParseToken *s = pparse_next(tok); s && s->kind != PPARSE_TK_EOF; s = pparse_next(s)) {
			if (s->flags & PPARSE_TF_OPEN) {
				s = pparse_pair(s);
				continue;
			}
			if (pparse_match_ch(s, '?')) {
				td++;
				continue;
			}
			if (pparse_match_ch(s, ':')) {
				if (td > 0) {
					td--;
					continue;
				}
				tok = pparse_next(s);
				goto restart;
			}
		}
		return NULL;
	}

	for (PParseToken *s = tok; s && s->kind != PPARSE_TK_EOF; s = pparse_next(s)) {
		if (s->flags & PPARSE_TF_OPEN) {
			s = pparse_pair(s);
			continue;
		}
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
		PParseToken *n = pparse_skip_prep_dirs(pparse_next(tok));
		if (n && (n->tag & PPARSE_TT_IF) && n->ch0 == 'e') {
			int snap = if_trail_snap[if_depth];
			if (cache) {
				uint32_t val = pparse_idx(tok) + 1;
				for (int i = snap; i < tn; i++) {
					uint32_t tix = trail[i];
					cache[tix] = val;
				}
			}
			tn = snap; // keep parent tokens in trail for final resolution
			tok = pparse_next(n);
			goto restart;
		}
	}
	if (cache && tok) {
		uint32_t val = pparse_idx(tok) + 1;
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
		PParseToken *w = pparse_skip_prep_dirs(pparse_next(tok));
		if (!w || !(w->tag & PPARSE_TT_LOOP) || w->ch0 != 'w') {
			tok = NULL;
			goto unwind_if;
		}
		PParseToken *p2 = pparse_skip_prep_dirs(pparse_next(w));
		if (!p2 || !(pparse_match_ch(p2, '(')) || !pparse_pair(p2)) {
			tok = NULL;
			goto unwind_if;
		}
		PParseToken *a = pparse_skip_prep_dirs(pparse_next(pparse_pair(p2)));
		tok = (a && pparse_match_ch(a, ';')) ? a : NULL;
		goto unwind_if;
	}
	return tok;
}

static PParseToken *pparse_skip_one_stmt(PParseToken *tok) {
	return pparse_skip_one_stmt_impl(tok, NULL);
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
} PParseScopeInfo;

#define pparse_scope_tree ((PParseScopeInfo *)pparse_ctx->p1_scope_tree)
#define pparse_scope_tree_count (pparse_ctx->p1_scope_count)
#define pparse_scope_tree_cap (pparse_ctx->p1_scope_cap)

/* Parameter declarations are discovered while walking an already-built scope
 * tree. Keep exact-token lookup, optional shadow creation, scope selection,
 * and array-to-pointer decay in the parser rather than its consumers. */
static void pparse_register_parameter_binding(PParseToken *tok,
					 int scope_depth,
					 uint16_t scope_id,
					 bool create,
					 unsigned traits) {
	PParseTypedefEntry *entry = pparse_binding_entry(tok, false);
	if (!entry && create) {
		PPARSE_TD_SCOPE_SAVE();
		if (scope_id > 0 && scope_id < pparse_scope_tree_count) {
			pparse_td_scope_close = pparse_scope_tree[scope_id].close_tok_idx;
		}
		entry = pparse_register_shadow(tok, scope_depth);
		PPARSE_TD_SCOPE_RESTORE();
	}
	if (!entry) return;
	entry->is_param = true;
	entry->is_array = false;
	pparse_binding_apply_traits(entry, traits);
}

static bool pparse_is_objc_ivar_brace(uint32_t brace_idx) {
	for (uint32_t i = brace_idx - 1; i > 0; i--) {
		PParseToken *t = &pparse_token_pool[i];
		if (t->kind == PPARSE_TK_PREP_DIR) continue;
		if ((t->kind == PPARSE_TK_IDENT && !t->tag) || pparse_match_ch(t, ':') || pparse_match_ch(t, '*') ||
		    (t->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_ATTR)))
			continue;
		if (pparse_match_ch(t, ')') && pparse_pair(t)) {
			i = pparse_idx(pparse_pair(t));
			continue;
		}
		if (pparse_match_ch(t, ']') && pparse_pair(t) && (pparse_pair(t)->flags & PPARSE_TF_C23_ATTR)) {
			i = pparse_idx(pparse_pair(t));
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
			    ((kw->len == 9 && prism_memeq_static(pparse_loc(kw), "interface", 9)) ||
			     (kw->len == 14 && prism_memeq_static(pparse_loc(kw), "implementation", 14)) ||
			     (kw->len == 8 && prism_memeq_static(pparse_loc(kw), "protocol", 8))))
				return true;
		}
		return false;
	}
	return false;
}

/* Build the reusable C scope index. Each `{` caches its scope ID directly,
 * making all later ownership/CFG queries O(1). Returns whether the TU contains
 * an `orelse` spelling so Prism can gate its extension-only work. */
static bool pparse_build_scopes(PParseToken *start) {
	pparse_scope_tree_count = 1; // 0 is file scope
	pparse_scope_tree_cap = 0;
	pparse_ctx->p1_scope_tree = NULL;
	int stack_cap = 256, depth = 0;
	uint16_t *stack = pparse_arena_alloc_uninit(&pparse_ctx->main_arena, stack_cap * sizeof(uint16_t));
	stack[0] = 0;
	for (PParseToken *t = start; t && t->kind != PPARSE_TK_EOF; t++) {
		if (t->ch0 != '{' && t->ch0 != '}') continue;
		if (t->ch0 == '}') {
			if (depth > 0) depth--;
			continue;
		}

		uint16_t sid = pparse_scope_tree_count;
		if (sid == UINT16_MAX) pparse_error_tok(t, "scope tree: too many scopes (>65534)");
		PPARSE_ARENA_ENSURE_CAP(
		    &pparse_ctx->main_arena, pparse_ctx->p1_scope_tree, pparse_scope_tree_count, pparse_scope_tree_cap, 256, PParseScopeInfo);
		PParseScopeInfo *si = &pparse_scope_tree[sid];
		*si = (PParseScopeInfo){.parent_id = stack[depth],
				  .open_tok_idx = pparse_idx(t),
				  .close_tok_idx = pparse_pair(t) ? pparse_idx(pparse_pair(t)) : UINT32_MAX};
		PParseToken *prev = pparse_walk_back(pparse_idx(t) - 1, PPARSE_WB_SKIP_NOISE);
		if (prev) {
			if (pparse_is_do_kw(prev)) {
				si->is_loop = true;
			} else if (pparse_match_ch(prev, ')') && pparse_pair(prev)) {
				PParseToken *open = pparse_pair(prev);
				PParseToken *kw = pparse_walk_back(pparse_idx(open) - 1, PPARSE_WB_SKIP_NOISE);
				if (kw && (kw->tag & PPARSE_TT_ATTR)) kw = pparse_walk_back(pparse_idx(kw) - 1, PPARSE_WB_SKIP_ATTRS);
				if (kw) {
					if (kw->tag & PPARSE_TT_LOOP) si->is_loop = true;
					else if (kw->tag & PPARSE_TT_SWITCH) si->is_switch = true;
					else if (kw->tag & PPARSE_TT_IF) si->is_conditional = true;
					else if (kw->tag & PPARSE_TT_SUE) {
						si->is_struct = true;
						si->is_enum = pparse_is_enum_kw(kw);
					}
				}
				if (depth == 0 && !si->is_loop && !si->is_switch && !si->is_conditional &&
				    !si->is_struct)
					si->is_func_body = true;
				if (depth > 0 && !si->is_loop && !si->is_switch && !si->is_conditional &&
				    !si->is_struct && !si->is_func_body) {
					if (pparse_paren_is_function_params(open)) si->is_func_body = true;
					else si->is_init = true;
				}
			} else if (pparse_is_else_kw(prev)) {
				si->is_conditional = true;
			} else if (prev->tag & PPARSE_TT_SUE) {
				si->is_struct = true;
				si->is_enum = pparse_is_enum_kw(prev);
			} else if (prev->kind == PPARSE_TK_IDENT &&
				   !(prev->tag &
				     (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_LOOP | PPARSE_TT_SWITCH | PPARSE_TT_IF | PPARSE_TT_STORAGE))) {
				PParseToken *sue = pparse_walk_back(pparse_idx(prev) - 1, PPARSE_WB_SKIP_ATTRS);
				if (sue && (sue->tag & PPARSE_TT_SUE)) {
					si->is_struct = true;
					si->is_enum = pparse_is_enum_kw(sue);
				}
			} else if (pparse_is_c23_fixed_underlying_enum(prev)) {
				si->is_struct = si->is_enum = true;
			} else if (depth == 0 && (pparse_match_ch(prev, ']') || pparse_match_ch(prev, ';'))) {
				si->is_func_body = true;
			}
		}

		if (!si->is_struct && !si->is_loop && !si->is_switch && !si->is_conditional &&
		    pparse_is_objc_ivar_brace(pparse_idx(t))) {
			si->is_struct = true;
			si->is_func_body = si->is_init = false;
		}
		if (prev && pparse_match_ch(prev, '(')) si->is_stmt_expr = true;
		if (!si->is_func_body && !si->is_loop && !si->is_switch && !si->is_conditional &&
		    !si->is_struct && !si->is_stmt_expr) {
			if (prev && pparse_match_ch(prev, '=')) si->is_init = true;
			else if (depth > 0 && stack[depth] < pparse_scope_tree_count && pparse_scope_tree[stack[depth]].is_init)
				si->is_init = true;
		}

		pparse_ann(t) = (si->is_loop ? P1_SCOPE_LOOP : 0) |
			     (si->is_switch ? P1_SCOPE_SWITCH : 0) |
			     (si->is_init ? P1_SCOPE_INIT : 0);
		bool reuse_parent =
		    si->is_init && depth > 0 && stack[depth] < pparse_scope_tree_count && pparse_scope_tree[stack[depth]].is_init;
		t->parse_data = reuse_parent ? stack[depth] : sid;
		if (!reuse_parent) pparse_scope_tree_count++;
		PPARSE_ARENA_ENSURE_CAP(&pparse_ctx->main_arena, stack, depth + 2, stack_cap, 256, uint16_t);
		depth++;
		stack[depth] = reuse_parent ? stack[depth - 1] : sid;
	}
	return pparse_token_tag_summary & PPARSE_TT_ORELSE;
}

static bool pparse_begin(PParseToken *start) {
	pparse_reset();
	return pparse_build_scopes(start);
}

static bool pparse_scope_is_ancestor_or_self(uint16_t ancestor, uint16_t descendant) {
	for (uint16_t s = descendant; s != 0; s = pparse_scope_tree[s].parent_id)
		if (s == ancestor) return true;
	return ancestor == 0; // file scope is ancestor of everything
}

static int pparse_scope_tree_depth(uint16_t scope_id) {
	int depth = 0;
	for (uint16_t s = scope_id; s != 0; s = pparse_scope_tree[s].parent_id) depth++;
	return depth;
}

static int pparse_scope_block_exits(uint16_t goto_sid, uint16_t label_sid) {
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
	for (uint16_t s = scope_id; s != 0; s = pparse_scope_tree[s].parent_id)
		if (s < pparse_scope_tree_count && pparse_scope_tree[s].is_stmt_expr) return s;
	return 0;
}

// Phase 1: check if a defer in scope 'sid' is inside a chain of closing braces
static void pparse_p1_check_defer_stmt_expr_chain(PParseToken *defer_tok, uint16_t sid) {
	while (sid > 0 && sid < pparse_scope_tree_count) {
		uint16_t pid = pparse_scope_tree[sid].parent_id;
		if (pid == 0 || pid >= pparse_scope_tree_count) break;
		PParseToken *t = pparse_next(&pparse_token_pool[pparse_scope_tree[sid].close_tok_idx]);
		PParseToken *parent_close = &pparse_token_pool[pparse_scope_tree[pid].close_tok_idx];
		bool only_trivial = true;
		while (t && t != parent_close && t->kind != PPARSE_TK_EOF) {
			if (pparse_match_ch(t, ';') || pparse_match_ch(t, '}')) {
				t = pparse_next(t);
				continue;
			}
			if (t->kind == PPARSE_TK_PREP_DIR) {
				t = pparse_next(t);
				continue;
			}
			if (t->tag & PPARSE_TT_ATTR) {
				t = pparse_next(t);
				if (t && pparse_match_ch(t, '('))
					t = pparse_pair(t) ? pparse_next(pparse_pair(t)) : pparse_next(t);
				continue;
			}
			if (pparse_is_c23_attr(t)) {
				t = pparse_pair(t) ? pparse_next(pparse_pair(t)) : pparse_next(t);
				continue;
			}
			/* Label: ident [[attr]]...: or ident __attribute__((...)): */
			if (t->kind == PPARSE_TK_IDENT || t->kind == PPARSE_TK_KEYWORD) {
				PParseToken *colon = pparse_skip_noise(pparse_next(t));
				if (colon && pparse_match_ch(colon, ':') &&
				    !(pparse_next(colon) && pparse_match_ch(pparse_next(colon), ':'))) {
					t = pparse_next(colon);
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

// Walk backward from before_idx skipping prep dirs, GNU attrs, C23 [[attrs]].

static void pparse_defer_scan_hidden_stmt_exprs(PParseToken *open, bool in_loop, bool in_switch, int depth);

static inline PParseToken *pparse_skip_defer_control_head(PParseToken *tok, bool in_loop, bool in_switch, int depth) {
	tok = pparse_skip_noise(tok);
	if (tok && pparse_match_ch(tok, '(') && pparse_pair(tok)) {
		pparse_defer_scan_hidden_stmt_exprs(tok, in_loop, in_switch, depth);
		return pparse_next(pparse_pair(tok));
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

static PParseToken *pparse_validate_defer_statement(PParseToken *tok, bool in_loop, bool in_switch, int depth);

static PParseToken *pparse_defer_walk_advance_past_orelse(PParseToken *s, bool in_loop, bool in_switch, int depth) {
	PParseToken *act = pparse_next(s);
	if (act && pparse_match_ch(act, ';')) pparse_error_tok(s, "expected statement after 'orelse'");
	pparse_validate_defer_control_flow(act, in_loop, in_switch);
	if (act && pparse_match_ch(act, '{')) {
		pparse_validate_defer_statement(act, in_loop, in_switch, depth + 1);
		PParseToken *close = pparse_pair(act);
		if (close) return close;
	}
	return act ? act : s;
}

static void pparse_defer_scan_orelse_in_group(PParseToken *open, bool in_loop, bool in_switch, int depth) {
	PParseToken *end = pparse_pair(open);
	if (!end) return;
	PParseToken *prev = open;
	for (PParseToken *s = pparse_next(open); s && s != end && s->kind != PPARSE_TK_EOF; s = pparse_next(s)) {
		if (s->flags & PPARSE_TF_OPEN) {
			if (pparse_match_ch(s, '(') || pparse_match_ch(s, '['))
				pparse_defer_scan_orelse_in_group(s, in_loop, in_switch, depth);
			prev = pparse_pair(s);
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
	PParseToken *end = pparse_pair(open);
	if (!end) return;
	for (PParseToken *t = pparse_next(open); t && t != end && t->kind != PPARSE_TK_EOF;) {
		if (pparse_is_stmt_expr_open(t)) {
			pparse_validate_defer_statement(pparse_next(t), in_loop, in_switch, depth + 1);
			t = pparse_pair(t) ? pparse_next(pparse_pair(t)) : pparse_next(t);
		} else
			t = pparse_next(t);
	}
}

static PParseToken *pparse_validate_defer_statement(PParseToken *tok, bool in_loop, bool in_switch, int depth) {
	if (depth >= 4096) pparse_error_tok(tok, "braceless control flow nesting depth exceeds 4096");
	tok = pparse_skip_noise(tok);
	if (!tok || tok->kind == PPARSE_TK_EOF) return tok;
	if (pparse_match_ch(tok, '{')) {
		PParseToken *end = pparse_pair(tok);
		uint32_t end_idx = end ? pparse_idx(end) : UINT32_MAX;
		for (tok = pparse_skip_noise(pparse_next(tok));
		     tok && tok != end && tok->kind != PPARSE_TK_EOF && pparse_idx(tok) < end_idx;
		     tok = pparse_skip_noise(tok)) {
			PParseToken *next = pparse_validate_defer_statement(tok, in_loop, in_switch, depth);
			if (next == tok) break;
			tok = next;
		}
		return end ? pparse_next(end) : tok;
	}

	if ((tok->tag & PPARSE_TT_IF) && tok->ch0 == 'i') {
		PParseToken *after_then = pparse_validate_defer_statement(
		    pparse_skip_defer_control_head(pparse_next(tok), in_loop, in_switch, depth),
		    in_loop,
		    in_switch,
		    depth + 1);
		PParseToken *else_tok = pparse_skip_noise(after_then);
		if (else_tok && (else_tok->tag & PPARSE_TT_IF) && else_tok->ch0 == 'e')
			return pparse_validate_defer_statement(pparse_next(else_tok), in_loop, in_switch, depth + 1);
		return after_then;
	}

	if (tok->tag & (PPARSE_TT_CASE | PPARSE_TT_DEFAULT)) {
		int td = 0;
		for (tok = pparse_next(tok); tok && tok->kind != PPARSE_TK_EOF; tok = pparse_next(tok)) {
			if ((tok->flags & PPARSE_TF_CLOSE) && tok->ch0 == '}') break;
			if (tok->flags & PPARSE_TF_OPEN) {
				if (pparse_match_ch(tok, '(') || pparse_match_ch(tok, '['))
					pparse_defer_scan_hidden_stmt_exprs(tok, in_loop, in_switch, depth);
				tok = pparse_pair(tok);
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
			   ? pparse_validate_defer_statement(pparse_next(tok), in_loop, in_switch, depth + 1)
			   : tok;
	}

	if (tok->tag & PPARSE_TT_SWITCH)
		return pparse_validate_defer_statement(
		    pparse_skip_defer_control_head(pparse_next(tok), in_loop, true, depth), in_loop, true, depth + 1);
	if (tok->tag & PPARSE_TT_LOOP) {
		if (tok->ch0 == 'd') {
			tok = pparse_validate_defer_statement(pparse_next(tok), true, in_switch, depth + 1);
			PParseToken *w = pparse_skip_noise(tok);
			if (w && (w->tag & PPARSE_TT_LOOP) && w->ch0 == 'w') {
				tok = pparse_skip_defer_control_head(pparse_next(w), true, in_switch, depth);
				tok = pparse_skip_noise(tok);
				if (tok && pparse_match_ch(tok, ';')) tok = pparse_next(tok);
			}
			return tok;
		}
		return pparse_validate_defer_statement(
		    pparse_skip_defer_control_head(pparse_next(tok), true, in_switch, depth),
		    true,
		    in_switch,
		    depth + 1);
	}

	if (tok->flags & PPARSE_TF_OPEN) {
		if (pparse_is_stmt_expr_open(tok)) {
			PParseToken *inner_brace = pparse_next(tok);
			pparse_validate_defer_statement(inner_brace, in_loop, in_switch, depth + 1);
			return pparse_pair(tok) ? pparse_next(pparse_pair(tok)) : pparse_next(tok);
		}
	}

	if (pparse_is_identifier_like(tok) && pparse_next(tok) && pparse_match_ch(pparse_next(tok), ':'))
		pparse_error_tok(tok,
			  "labels inside defer blocks produce duplicate labels "
			  "when the defer body is copied to multiple exit points");
	// is compile-time only.  Scan forward through decl-specifiers.
	for (PParseToken *s = tok; s && s->kind != PPARSE_TK_EOF; s = pparse_next(s)) {
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
				PParseToken *m = pparse_pair(s);
				if (m) s = m;
			}
			continue;
		}
		break;
	}

	if (tok->kind == PPARSE_TK_KEYWORD) {
		pparse_validate_defer_control_flow(tok, in_loop, in_switch);
		if ((tok->tag & PPARSE_TT_DEFER) && !pparse_is_known_typedef(tok) && !pparse_match_ch(pparse_next(tok), ':') &&
		    !(pparse_next(tok) && (pparse_next(tok)->tag & PPARSE_TT_ASSIGN)))
			pparse_error_tok(tok, "nested defer is not supported");
	}

	if (pparse_feat(PPARSE_F_ORELSE)) {
		PParseToken *prev_oe = NULL;
		for (PParseToken *s = tok;
		     s && s->kind != PPARSE_TK_EOF && !pparse_match_ch(s, ';') && !((s->flags & PPARSE_TF_CLOSE) && s->ch0 == '}');
		     s = pparse_next(s)) {
			if (s->flags & PPARSE_TF_OPEN) {
				if (pparse_match_ch(s, '(') || pparse_match_ch(s, '['))
					pparse_defer_scan_orelse_in_group(s, in_loop, in_switch, depth);
				prev_oe = pparse_pair(s);
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
	     s = pparse_next(s)) {
		if (s->flags & PPARSE_TF_OPEN) {
			if (pparse_is_stmt_expr_open(s))
				pparse_validate_defer_statement(pparse_next(s), in_loop, in_switch, depth + 1);
			else if (pparse_match_set(s, pparse_CH('(') | pparse_CH('[')) || pparse_match_ch(s, '{'))
				pparse_defer_scan_hidden_stmt_exprs(s, in_loop, in_switch, depth);
			s = pparse_pair(s);
			continue;
		}
	}
	PParseToken *semi = pparse_skip_to_semicolon(tok, NULL);
	return (semi && semi->kind != PPARSE_TK_EOF) ? pparse_next(semi) : semi;
}

static bool pparse_is_knr_params(PParseToken *start, PParseToken *brace) {
	if (!start || start == brace || pparse_match_ch(start, ';')) return false;
	bool saw_semi = false;
	for (PParseToken *t = start; t && t != brace && t->kind != PPARSE_TK_EOF; t = pparse_next(t)) {
		if (pparse_match_ch(t, ';')) saw_semi = true;
		if (t->flags & PPARSE_TF_OPEN) t = pparse_pair(t);
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

static bool pparse_params_look_like_decls(PParseToken *open) {
	PParseToken *close = pparse_pair(open);
	if (!close) return false;
	for (PParseToken *t = pparse_next(open); t && t != close; t = pparse_next(t)) {
		if (t->flags & PPARSE_TF_OPEN) {
			if (pparse_pair(t)) t = pparse_pair(t);
			continue;
		}
		if (t->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_TYPEOF | PPARSE_TT_BITINT | PPARSE_TT_ATTR | PPARSE_TT_STORAGE))
			return true;
		if (pparse_is_known_typedef(t)) return true;
	}
	return false;
}

static PParseToken *pparse_generic_find_assoc_start(PParseToken *open) {
	PParseToken *close = pparse_pair(open);
	if (!close) return NULL;
	for (PParseToken *t = pparse_next(open); t && t != close; t = pparse_next(t)) {
		if (t->flags & PPARSE_TF_OPEN) {
			if (pparse_pair(t)) t = pparse_pair(t);
			continue;
		}
		if (pparse_match_ch(t, ',')) return pparse_next(t);
	}
	return NULL;
}

static bool pparse_generic_has_distinct_targets(PParseToken *assoc_start, PParseToken *close) {
	const char *first_name = NULL;
	uint32_t first_len = 0;
	PParseToken *first_args_open = NULL;
	PParseToken *first_args_close = NULL;
	int ternary_depth = 0;
	for (PParseToken *t = assoc_start; t && t != close; t = pparse_next(t)) {
		if (t->flags & PPARSE_TF_OPEN) {
			if (pparse_pair(t)) t = pparse_pair(t);
			continue;
		}
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
		for (PParseToken *b = pparse_next(t); b && b != close; b = pparse_next(b)) {
			if (b->flags & PPARSE_TF_OPEN) {
				if (pparse_pair(b)) b = pparse_pair(b);
				continue;
			}
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
				PParseToken *bn = pparse_next(b);
				if (bn && pparse_match_ch(bn, '?')) {
					inner_ternary++;
					b = bn;
					continue;
				}
			}
			found_ident = true;
			while (b && pparse_next(b) && pparse_next(b) != close && (pparse_next(b)->tag & PPARSE_TT_MEMBER) &&
			       pparse_next(pparse_next(b)) && pparse_is_valid_varname(pparse_next(pparse_next(b)))) {
				b = pparse_next(pparse_next(b));
			}
			if (!first_name) {
				first_name = pparse_loc(b);
				first_len = b->len;
				PParseToken *ao = pparse_next(b);
				if (ao && pparse_match_ch(ao, '(') && pparse_pair(ao)) {
					first_args_open = ao;
					first_args_close = pparse_pair(ao);
				}
			} else if (b->len != first_len ||
				   !prism_memeq_runtime_sized(pparse_loc(b), first_name, first_len)) {
				return true;
			} else {
				PParseToken *ao = pparse_next(b);
				if (!ao || !pparse_match_ch(ao, '(') || !pparse_pair(ao)) {
					if (first_args_open) return true;
				} else {
					PParseToken *ac = pparse_pair(ao);
					if (!first_args_open) return true;
					PParseToken *a1 = pparse_next(first_args_open);
					PParseToken *a2 = pparse_next(ao);
					while (a1 && a1 != first_args_close && a2 && a2 != ac) {
						if (a1->kind != a2->kind || a1->len != a2->len ||
						    !prism_memeq_runtime_sized(pparse_loc(a1), pparse_loc(a2), a1->len))
							return true;
						a1 = pparse_next(a1);
						a2 = pparse_next(a2);
					}
					if ((a1 != first_args_close) || (a2 != ac)) return true;
				}
			}
			break;
		}
		if (!found_ident) {
			bool has_real_ident = false;
			int depth = 0;
			for (PParseToken *d = pparse_next(t); d && d != close; d = pparse_next(d)) {
				if (d->flags & PPARSE_TF_OPEN) depth++;
				else if (d->flags & PPARSE_TF_CLOSE)
					depth--;
				if (depth == 0 && pparse_match_ch(d, ',')) break;
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
	PParseToken *open = pparse_next(generic_tok);
	if (!open || !pparse_match_ch(open, '(') || !pparse_pair(open)) return false;
	PParseToken *close = pparse_pair(open);
	PParseToken *after = pparse_skip_noise(pparse_next(close));
	if (!after) return false;
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
	PParseToken *open, *close, *after, *assoc_start;
	if (!pparse_generic_rewrite_preamble(generic_tok, &open, &close, &after, &assoc_start)) return false;
	if (pparse_match_set(after, pparse_CH(';') | pparse_CH(',')) || (after->tag & PPARSE_TT_ATTR) || pparse_is_c23_attr(after)) {
		for (PParseToken *t = assoc_start; t && t != close; t = pparse_next(t)) {
			PParseToken *name = t;
			PParseToken *call_open = pparse_skip_noise(pparse_next(t));
			/* Plain `name(params)` association. */
			if (pparse_is_valid_varname(t) && call_open && pparse_match_ch(call_open, '(') &&
			    pparse_pair(call_open) && pparse_params_look_like_decls(call_open)) {
				*name_out = name;
				*params_open_out = call_open;
				*params_close_out = pparse_pair(call_open);
				*next_out = after;
				return true;
			}
			/* Glibc-style parenthesized / cast-wrapped name:
			 * `(name)(params)` or `(const char *)(name)(params)`. */
			if (!pparse_match_ch(t, '(') || !pparse_pair(t)) continue;
			PParseToken *inner = pparse_skip_noise(pparse_next(t));
			PParseToken *paren_close = pparse_pair(t);
			PParseToken *after_paren = pparse_skip_noise(pparse_next(paren_close));
			/* Peel one layer of cast-like `(type)(name)` before `(params)`. */
			if (inner && !pparse_is_valid_varname(inner) && after_paren && pparse_match_ch(after_paren, '(') &&
			    pparse_pair(after_paren)) {
				PParseToken *maybe_name = pparse_skip_noise(pparse_next(after_paren));
				PParseToken *name_close = pparse_pair(after_paren);
				PParseToken *params = name_close ? pparse_skip_noise(pparse_next(name_close)) : NULL;
				if (maybe_name && pparse_is_valid_varname(maybe_name) &&
				    pparse_skip_noise(pparse_next(maybe_name)) == name_close && params &&
				    pparse_match_ch(params, '(') && pparse_pair(params) &&
				    pparse_params_look_like_decls(params)) {
					*name_out = maybe_name;
					*params_open_out = params;
					*params_close_out = pparse_pair(params);
					*next_out = after;
					return true;
				}
			}
			/* `(name)(params)` */
			if (inner && pparse_is_valid_varname(inner) &&
			    pparse_skip_noise(pparse_next(inner)) == paren_close && after_paren &&
			    pparse_match_ch(after_paren, '(') && pparse_pair(after_paren) &&
			    pparse_params_look_like_decls(after_paren)) {
				*name_out = inner;
				*params_open_out = after_paren;
				*params_close_out = pparse_pair(after_paren);
				*next_out = after;
				return true;
			}
		}
	}
	if (pparse_match_ch(after, '(') && pparse_pair(after) && pparse_params_look_like_decls(after)) {
		PParseToken *ext_close = pparse_pair(after);
		PParseToken *after_ext = pparse_skip_noise(pparse_next(ext_close));
		if (after_ext &&
		    (pparse_match_ch(after_ext, ';') || pparse_match_ch(after_ext, ',') || (after_ext->tag & PPARSE_TT_ATTR) ||
		     pparse_is_c23_attr(after_ext))) {
			PParseToken *found = NULL;
			for (PParseToken *t = assoc_start; t && t != close; t = pparse_next(t)) {
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

static inline PParseToken *pparse_try_detect_noreturn_call(PParseToken *tok) {
	if (!(tok->tag & PPARSE_TT_NORETURN_FN)) return NULL;
	// Respect C scoping: if a local variable/parameter shadows a noreturn
	PParseTypedefEntry *te = pparse_typedef_lookup(tok);
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
	PParseToken *ue = pparse_walk_back(pparse_idx(tok), PPARSE_WB_ATTR_NOISE);
	for (;;) {
		if (ue && pparse_match_ch(ue, ')') && pparse_close_paren_ends_cast_type_name(ue)) {
			PParseToken *open = pparse_pair(ue);
			ue = open ? pparse_walk_back(pparse_idx(open), PPARSE_WB_PAST_NOISE) : NULL;
			continue;
		}
		/* Grouping paren we are inside: `sizeof +(+die())` — peel to the
		 * token before the open so uneval intros still win. */
		if (ue && pparse_match_ch(ue, '(') && (ue->flags & PPARSE_TF_OPEN) && pparse_pair(ue)) {
			ue = pparse_walk_back(pparse_idx(ue), PPARSE_WB_PAST_NOISE);
			continue;
		}
		if (ue && ue->kind == PPARSE_TK_IDENT && pparse_equal(ue, "__extension__")) {
			ue = pparse_walk_back(pparse_idx(ue), PPARSE_WB_PAST_NOISE);
			continue;
		}
		if (ue && ue->kind == PPARSE_TK_PUNCT && ue->len == 1 &&
		    (ue->ch0 == '+' || ue->ch0 == '-' || ue->ch0 == '!' || ue->ch0 == '~')) {
			PParseToken *before = pparse_walk_back(pparse_idx(ue), PPARSE_WB_ATTR_NOISE);
			if ((ue->ch0 == '+' || ue->ch0 == '-') && before && pparse_is_expr_ending(before) &&
			    !pparse_is_sizeof_like(before) &&
			    !(before->kind == PPARSE_TK_IDENT && pparse_equal(before, "__extension__")) &&
			    !pparse_close_paren_ends_cast_type_name(before))
				break; /* binary + / - */
			ue = pparse_walk_back(pparse_idx(ue), PPARSE_WB_PAST_NOISE);
			continue;
		}
		break;
	}
	if (ue && pparse_is_sizeof_like(ue)) return NULL;
	if (pparse_idx(tok) >= 1) {
		PParseToken *prev = pparse_walk_back(pparse_idx(tok), PPARSE_WB_PAST_NOISE);
		if (prev && (prev->tag & PPARSE_TT_MEMBER)) return NULL;
		if (prev && (prev->tag & (PPARSE_TT_TYPE | PPARSE_TT_QUALIFIER | PPARSE_TT_STORAGE | PPARSE_TT_INLINE | PPARSE_TT_SUE)))
			return NULL;
		if (prev && pparse_match_ch(prev, '*')) return NULL;
		/* `sizeof die();` / `_Alignof die();` — call is unevaluated;
		 * must not inject unreachable after the statement. */
		if (prev && pparse_is_sizeof_like(prev)) return NULL;
		/* Short-circuit: `x && die();` / `x || die();` — die may not run. */
		if (prev && prev->kind == PPARSE_TK_PUNCT && prev->len == 2 &&
		    ((prev->ch0 == '&' && pparse_loc(prev)[1] == '&') ||
		     (prev->ch0 == '|' && pparse_loc(prev)[1] == '|')))
			return NULL;
	}
	PParseToken *call = pparse_next(tok);
	if (!call || !pparse_match_ch(call, '(') || !pparse_pair(call)) return NULL;
	/* Unevaluated operands (`sizeof(die())`, `_Generic`) must not inject. */
	if (pparse_token_is_in_unevaluated_operand(tok)) return NULL;
	PParseToken *after = pparse_next(pparse_pair(call));
	/* Statement-final call through casts / grouping / comma tails:
	 *   (void)die();          — after is `;` (classic)
	 *   (void)(die());        — after is `)` then `;`
	 *   (void)(0, die());     — after is `)` then `;`
	 *   die(), 1;             — after is `,` … `;`
	 *   foo(die());           — after is `)` of args then `;`
	 * Control-paren sites (`if (die())`) are filtered by callers via
	 * in_ctrl_paren(); do not special-case here. */
	while (after && pparse_match_ch(after, ')')) after = pparse_next(after);
	if (after && pparse_match_ch(after, ';')) return after;
	if (after && pparse_match_ch(after, ',')) {
		int depth = 0;
		for (PParseToken *s = pparse_next(after); s && s->kind != PPARSE_TK_EOF; s = pparse_next(s)) {
			if (s->flags & PPARSE_TF_OPEN) {
				depth++;
				continue;
			}
			if (s->flags & PPARSE_TF_CLOSE) {
				if (depth == 0) break;
				depth--;
				continue;
			}
			if (depth == 0 && pparse_match_ch(s, ';')) return s;
		}
	}
	return NULL;
}

static inline PParseToken *pparse_p1d_find_open_paren(PParseToken *tok) {
	for (PParseToken *s = pparse_next(tok); s && s->kind != PPARSE_TK_EOF; s = pparse_next(s)) {
		if (s->kind == PPARSE_TK_PREP_DIR) continue;
		if (pparse_match_ch(s, '(')) return s;
		break;
	}
	return NULL;
}

static PParseToken *pparse_p1_knr_find_close_paren(PParseToken *semi_tok) {
	for (uint32_t pi = pparse_idx(semi_tok); pi > 0; pi--) {
		PParseToken *pt = &pparse_token_pool[pi - 1];
		if (pt->kind == PPARSE_TK_PREP_DIR) continue;
		if (pparse_match_ch(pt, '{') || pparse_match_ch(pt, '}')) return NULL;
		if (pparse_match_ch(pt, ')') && pparse_pair(pt)) {
			PParseToken *open = pparse_pair(pt);
			bool is_ident_list = true;
			for (PParseToken *t = pparse_next(open); t && t != pt; t = pparse_next(t)) {
				if (!pparse_is_valid_varname(t) && !pparse_match_ch(t, ',') && t->kind != PPARSE_TK_PREP_DIR) {
					is_ident_list = false;
					break;
				}
			}
			if (is_ident_list) return pt;
			pi = pparse_idx(open) + 1; // +1 because loop does pi--
			continue;
		}
		if ((pt->flags & PPARSE_TF_CLOSE) && pparse_pair(pt)) {
			pi = pparse_idx(pparse_pair(pt)) + 1;
			continue;
		}
	}
	return NULL;
}

// Probes past attributes/pragmas via pparse_skip_noise before checking PPARSE_TF_RAW,
// matching Pass 2's process_declarators logic.
// Returns the token after a stripable `raw` prefix, or NULL if not a decl-strip.
static inline PParseToken *pparse_raw_decl_strip_after(PParseToken *probe) {
	if (!probe || !(probe->flags & PPARSE_TF_RAW)) return NULL;
	PParseToken *after = pparse_skip_noise(pparse_next(probe));
	if (!after) return NULL;
	/* Typedef-named `raw` is a keyword prefix only before another type
	 * (`raw raw x` / `raw int x`), never before `*` / `(`` / a declarator
	 * name where `raw` itself is the typedef type. */
	if (pparse_is_known_typedef(probe)) {
		if (pparse_is_type_keyword(after) || pparse_is_known_typedef(after) ||
		    (after->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_STORAGE | PPARSE_TT_INLINE | PPARSE_TT_TYPEDEF)) ||
		    (after->flags & PPARSE_TF_RAW))
			return after;
		return NULL;
	}
	if ((pparse_is_valid_varname(after) && !pparse_is_type_keyword(after) && !pparse_is_known_typedef(after) &&
	     !(after->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE))) ||
	    pparse_match_ch(after, '*') || pparse_match_ch(after, '('))
		return after;
	return NULL;
}

static inline PParseToken *pparse_p1_skip_decl_raw(PParseToken *t, bool *saw_raw) {
	PParseToken *after = pparse_raw_decl_strip_after(pparse_skip_noise(t));
	if (!after) return t;
	while ((after->flags & PPARSE_TF_RAW) && !pparse_is_known_typedef(after)) after = pparse_skip_noise(pparse_next(after));
	*saw_raw = true;
	return after;
}

static inline bool pparse_is_assignment_operator_token(PParseToken *tok) {
	return (tok->tag & PPARSE_TT_ASSIGN) && pparse_loc(tok)[tok->len - 1] == '=';
}

typedef struct {
	PParseToken *last_comma;
	PParseToken *last_assignment;
	PParseToken *segment_assignment;
} PParseExprTopLevel;

/* One balanced scan answers both questions needed by bare-expression
 * lowering. An assignment before the last comma belongs to an earlier comma
 * operand, so reset it when the active segment changes. */
static PParseExprTopLevel pparse_scan_expr_top_level(PParseToken *start, PParseToken *end) {
	PParseExprTopLevel result = {0};
	int depth = 0;
	for (PParseToken *t = start; t && t != end; t = pparse_next(t)) {
		if (t->flags & PPARSE_TF_OPEN)
			depth++;
		else if (t->flags & PPARSE_TF_CLOSE)
			depth--;
		else if (depth == 0 && pparse_match_ch(t, ',')) {
			result.last_comma = t;
			result.segment_assignment = NULL;
		} else if (depth == 0 && pparse_is_assignment_operator_token(t)) {
			result.last_assignment = t;
			result.segment_assignment = t;
		}
	}
	return result;
}

static void pparse_for_each_enum_constant(PParseToken *brace, void (*visit)(PParseToken *, void *), void *user_data) {
	PParseToken *end = pparse_pair(brace);
	if (!end) return;
	for (PParseToken *t = pparse_next(brace); t && t != end && t->kind != PPARSE_TK_EOF;) {
		if (t->kind != PPARSE_TK_IDENT && t->kind != PPARSE_TK_KEYWORD) {
			t = pparse_next(t);
			continue;
		}
		visit(t, user_data);
		while (t && t != end && t->kind != PPARSE_TK_EOF && !pparse_match_ch(t, ',')) {
			if ((t->flags & PPARSE_TF_OPEN) && pparse_pair(t)) {
				t = pparse_next(pparse_pair(t));
				continue;
			}
			t = pparse_next(t);
		}
		if (t && pparse_match_ch(t, ',')) t = pparse_next(t);
	}
}

static bool pparse_raw_after_subscript_open_bracket(PParseToken *raw_kw) {
	if (!raw_kw || !(raw_kw->flags & PPARSE_TF_RAW)) return false;
	uint32_t ri = pparse_idx(raw_kw);
	if (ri == 0) return false;
	/* pparse_walk_back(k, PPARSE_WB_PAST_NOISE) inspects pool[k-1] first — pass ri, not ri-1. */
	PParseToken *b = pparse_walk_back(ri, PPARSE_WB_PAST_NOISE);
	// `[` from `[[attr]]` is tagged PPARSE_TF_C23_ATTR — not an array subscript.
	return b && pparse_match_ch(b, '[') && !(b->flags & PPARSE_TF_C23_ATTR);
}

static bool pparse_is_raw_declaration_context(PParseToken *raw_kw, PParseToken *after_raw) {
	if (pparse_raw_after_subscript_open_bracket(raw_kw)) return false;
	after_raw = pparse_skip_noise(after_raw);
	if (!after_raw) return false;
	if (pparse_is_type_keyword(after_raw) || pparse_is_known_typedef(after_raw) ||
	    (after_raw->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE | PPARSE_TT_STORAGE | PPARSE_TT_INLINE | PPARSE_TT_TYPEDEF)) ||
	    ((after_raw->flags & PPARSE_TF_RAW) && !pparse_is_known_typedef(after_raw)))
		return true;
	if (pparse_match_ch(after_raw, '*')) {
		PParseToken *after_star = pparse_skip_noise(pparse_next(after_raw));
		while (after_star && (pparse_match_ch(after_star, '*') || (after_star->tag & PPARSE_TT_QUALIFIER)))
			after_star = pparse_skip_noise(pparse_next(after_star));
		return after_star && (pparse_is_valid_varname(after_star) || pparse_match_ch(after_star, '('));
	}
	return false;
}

static bool pparse_is_raw_strip_context(PParseToken *raw_kw, PParseToken *after_raw) {
	if (pparse_is_raw_declaration_context(raw_kw, after_raw)) return true;
	after_raw = pparse_skip_noise(after_raw);
	PParseToken *boundary = after_raw ? pparse_skip_noise(pparse_next(after_raw)) : NULL;
	return after_raw && pparse_is_valid_varname(after_raw) && !pparse_is_type_keyword(after_raw) &&
	       !pparse_is_known_typedef(after_raw) && !(after_raw->tag & (PPARSE_TT_QUALIFIER | PPARSE_TT_SUE)) && boundary &&
	       (pparse_match_ch(boundary, ',') || pparse_match_ch(boundary, ';') ||
		pparse_match_set(boundary, pparse_CH('[') | pparse_CH('(') | pparse_CH('=') | pparse_CH(':')));
}

static bool pparse_has_effective_const_qual(PParseToken *type_start, PParseTypeSpec *type, PParseDecl *decl) {
	bool has_const_qual = (type->has_const && !decl->is_func_ptr && !decl->is_pointer) || decl->is_const;
	if (type->has_constexpr) has_const_qual = true;
	if (type->has_typeof && !decl->is_func_ptr && !decl->is_pointer) has_const_qual = true;
	if (!has_const_qual && !decl->is_func_ptr && !decl->is_pointer) {
		for (PParseToken *t = type_start; t && t != type->end; t = pparse_next(t))
			if (pparse_is_const_typedef(t)) {
				has_const_qual = true;
				break;
			}
	}
	return has_const_qual;
}

static bool pparse_has_storage_in(PParseToken *from, PParseToken *to) {
	for (PParseToken *s = from; s && s != to; s = pparse_next(s))
		if (s->tag & PPARSE_TT_STORAGE) return true;
	return false;
}

static bool pparse_needs_space(PParseToken *prev, PParseToken *tok) {
	if (!prev || pparse_at_bol(tok)) return false;
	if (tok->flags & PPARSE_TF_HAS_SPACE) return true;
	if ((pparse_is_identifier_like(prev) || prev->kind == PPARSE_TK_NUM) &&
	    (pparse_is_identifier_like(tok) || tok->kind == PPARSE_TK_NUM))
		return true;
	if (prev->kind != PPARSE_TK_PUNCT || tok->kind != PPARSE_TK_PUNCT) return false;
	char a = (prev->len == 1) ? prev->ch0 : pparse_loc(prev)[prev->len - 1];
	char b = tok->ch0;
	if (b == '=') return strchr("=!<>+-*/%&|^", a) != NULL;
	return (a == b && strchr("+-<>&|#", a)) || (a == '-' && b == '>') || (a == '/' && b == '*') ||
	       (a == '*' && b == '/');
}

static bool pparse_declarator_has_bracket_orelse(PParseToken *start, PParseToken *end) {
	for (PParseToken *t = start; t && t != end && t->kind != PPARSE_TK_EOF; t = pparse_next(t))
		if (pparse_ann(t) & P1_OE_BRACKET) return true;
	return false;
}

static inline uint16_t pparse_scope_id(PParseToken *body_start) {
	return body_start && pparse_match_ch(body_start, '{') ? (uint16_t)body_start->parse_data : 0;
}

/* Freeze position-dependent C name resolution into each token for O(1)
 * consumers. Optionally report whether an exact identifier exists while the
 * pool is hot, avoiding a second sentinel walk in the caller. */
static bool pparse_finalize(const char *find_ident, uint32_t find_len) {
	pparse_td_build_timelines();
	pparse_ba_build_timelines();
	bool found = false;
	PParseToken *pool_end = pparse_token_pool + pparse_token_count;
	for (PParseToken *t = pparse_token_pool + 1; t < pool_end && t->kind != PPARSE_TK_EOF; t++) {
		if (find_ident && !found && t->kind == PPARSE_TK_IDENT && t->len == find_len &&
		    t->ch0 == (uint8_t)find_ident[0] &&
		    prism_memeq_runtime_sized(pparse_loc(t), find_ident, find_len))
			found = true;
		if (t->tag & PPARSE_TT_ATTR) {
			PParseToken *open = pparse_next(t);
			if (open && pparse_match_ch(open, '(') && (open->flags & PPARSE_TF_OPEN) && pparse_pair(open)) {
				PParseToken *close = pparse_pair(open);
				for (PParseToken *u = open; u <= close; u++) pparse_ann(u) |= P1_IN_ATTR_ARGS;
			}
		}
		if (!pparse_is_identifier_like(t)) continue;
		PParseTypedefEntry *e = pparse_typedef_lookup(t);
		if (!e) continue;
		pparse_ann(t) |= P1_HAS_ENTRY;
		t->parse_data = (uint32_t)(e - pparse_typedef_table.entries) + 1;
		if (!e->is_enum_const && !e->is_shadow && !e->is_vla_var && !e->is_struct_tag)
			pparse_ann(t) |= P1_IS_TYPEDEF;
	}
	pparse_p1_typedef_annotated = true;
	return found;
}

void pparse_tokenizer_teardown(bool full) {
	if (pparse_ctx->input_files) {
		for (int i = 0; i < pparse_ctx->input_file_count; i++) {
			PParseFile *f = pparse_ctx->input_files[i];
			if (f && f->contents && f->owns_contents) free(f->contents);
		}
	}
	if (full) {
		pparse_arena_free(&pparse_ctx->main_arena);
		memset(pparse_keyword_cache, 0, sizeof(pparse_keyword_cache));
		free(pparse_token_pool);
		free(pparse_token_cold);
		pparse_token_pool = NULL;
		pparse_token_cold = NULL;
		pparse_token_count = 1;
		pparse_token_cap = 0;
	} else {
		for (PParseArenaBlock *b = pparse_ctx->main_arena.head; b; b = b->next) b->used = 0;
		pparse_ctx->main_arena.current = pparse_ctx->main_arena.head;
		pparse_token_count = 1; // Reset pool index but keep allocation
	}
	pparse_ctx->input_files = NULL;
	pparse_ctx->input_file_count = 0;
	pparse_ctx->input_file_capacity = 0;
	pparse_ctx->current_file = NULL;
	pparse_ctx->token_source = NULL;
	pparse_token_tag_summary = 0;
}

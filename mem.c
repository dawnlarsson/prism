/* PRISM_MEMORY_POLICY
 *
 * Every Prism memory operation name must reveal whether byte count is a
 * runtime decision or a compile-time constant:
 *
 *   prism_*_runtime
 *       Unbounded runtime size. May lower to compiler builtins; use only when
 *       the size is genuinely dynamic (arena blocks, fwrite chunks, …).
 *
 *   prism_*_bounded
 *       Runtime size, caller proves n <= PRISM_MEM_BOUNDED_BYTES. No-libc
 *       hot-path ladders: predictable, still branch on n. Token lengths,
 *       hashmap keys, and out_str spills live here.
 *
 *   prism_*_static
 *       n must be a compile-time constant. Preferred for keyword literals,
 *       fixed structs, and any site that wants exact load/store code with no
 *       runtime size ladder (__builtin_memcpy_inline when available).
 *
 * Vague soft names (prism_eq / prism_copy / prism_zero) are intentionally not
 * the API. Call sites pick the tier explicitly.
 *
 * ── Opt-in switches (A/B) ─────────────────────────────────────────
 * Custom paths default OFF so we can enable one at a time:
 *
 *   -DPRISM_MEM_STATIC=1         keyword / const-n memeq + memcpy_static
 *   -DPRISM_MEM_BOUNDED_EQ=1     bounded size-switch / word-ladder equality
 *   -DPRISM_MEM_BOUNDED_COPY=1   bounded memcpy/memset ladders + out_str
 *   -DPRISM_MEM_OUT_LIT_STATIC=1 OUT_LIT uses memcpy_static (needs STATIC)
 *
 * -DPRISM_MEM_KIT=1 enables all of the above.
 */

#ifndef PRISM_MEM_KIT
#define PRISM_MEM_KIT 0
#endif
#ifndef PRISM_MEM_STATIC
#define PRISM_MEM_STATIC PRISM_MEM_KIT
#endif
#ifndef PRISM_MEM_BOUNDED_EQ
#define PRISM_MEM_BOUNDED_EQ PRISM_MEM_KIT
#endif
#ifndef PRISM_MEM_BOUNDED_COPY
#define PRISM_MEM_BOUNDED_COPY PRISM_MEM_KIT
#endif
#ifndef PRISM_MEM_OUT_LIT_STATIC
#define PRISM_MEM_OUT_LIT_STATIC PRISM_MEM_KIT
#endif

#ifndef PRISM_MEM_BOUNDED_BYTES
/* Ladder below covers any n ≤ this with 64-byte chunks + bit tail.
 * Token/hash/out_str spills that fit stay on the no-libc path. */
#define PRISM_MEM_BOUNDED_BYTES 256u
#endif

#ifndef PRISM_HAS_BUILTIN
#if defined(__has_builtin)
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

#ifndef PRISM_ASSUME
#if PRISM_HAS_BUILTIN(__builtin_assume)
#define PRISM_ASSUME(x) __builtin_assume(x)
#elif defined(__clang__) || defined(__GNUC__)
#define PRISM_ASSUME(x)                                                                                      \
	do {                                                                                                 \
		if (!(x)) __builtin_unreachable();                                                           \
	} while (0)
#else
#define PRISM_ASSUME(x) ((void)0)
#endif
#endif

#ifndef PRISM_NOBUILTIN_MEMOPS
#if defined(__has_attribute)
#if __has_attribute(no_builtin)
#define PRISM_NOBUILTIN_MEMOPS                                                                               \
	__attribute__((no_builtin("memcpy", "memmove", "memset", "memcmp", "bzero")))
#else
#define PRISM_NOBUILTIN_MEMOPS
#endif
#else
#define PRISM_NOBUILTIN_MEMOPS
#endif
#endif

#if PRISM_HAS_BUILTIN(__builtin_memcpy_inline)
#define PRISM_MEM_COPY_EXACT(dst, src, n) __builtin_memcpy_inline((dst), (src), (n))
#else
/* Exact-width fallback: still no libc — unrolled byte copy for const n. */
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
		unsigned char _b = (unsigned char)(v);                                                       \
		for (unsigned _i = 0; _i < (unsigned)(n); _i++) _d[_i] = _b;                                 \
	} while (0)
#endif

static inline PRISM_ALWAYS_INLINE void *prism__mem_ret(void *p) {
	return p;
}

/* ── runtime (unbounded; builtins OK) ─────────────────────────────── */

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

/* ── bounded (runtime n ≤ PRISM_MEM_BOUNDED_BYTES; no libc) ───────── */

static inline PRISM_NOBUILTIN_MEMOPS void *prism_memcpy_bounded(void *restrict dst, const void *restrict src,
							       size_t n) {
	unsigned char *restrict d = (unsigned char *)dst;
	const unsigned char *restrict s = (const unsigned char *)src;
	PRISM_ASSUME(n <= (size_t)PRISM_MEM_BOUNDED_BYTES);
	while (n >= 64u) {
		PRISM_MEM_COPY_EXACT(d, s, 64);
		d += 64;
		s += 64;
		n -= 64u;
	}
	if (n & 32u) {
		PRISM_MEM_COPY_EXACT(d, s, 32);
		d += 32;
		s += 32;
	}
	if (n & 16u) {
		PRISM_MEM_COPY_EXACT(d, s, 16);
		d += 16;
		s += 16;
	}
	if (n & 8u) {
		PRISM_MEM_COPY_EXACT(d, s, 8);
		d += 8;
		s += 8;
	}
	if (n & 4u) {
		PRISM_MEM_COPY_EXACT(d, s, 4);
		d += 4;
		s += 4;
	}
	if (n & 2u) {
		PRISM_MEM_COPY_EXACT(d, s, 2);
		d += 2;
		s += 2;
	}
	if (n & 1u) *d = *s;
	return dst;
}

static inline PRISM_NOBUILTIN_MEMOPS void *prism_memset_bounded(void *dst, int v, size_t n) {
	unsigned char *d = (unsigned char *)dst;
	PRISM_ASSUME(n <= (size_t)PRISM_MEM_BOUNDED_BYTES);
	while (n >= 64u) {
		PRISM_MEM_SET_EXACT(d, v, 64);
		d += 64;
		n -= 64u;
	}
	if (n & 32u) {
		PRISM_MEM_SET_EXACT(d, v, 32);
		d += 32;
	}
	if (n & 16u) {
		PRISM_MEM_SET_EXACT(d, v, 16);
		d += 16;
	}
	if (n & 8u) {
		PRISM_MEM_SET_EXACT(d, v, 8);
		d += 8;
	}
	if (n & 4u) {
		PRISM_MEM_SET_EXACT(d, v, 4);
		d += 4;
	}
	if (n & 2u) {
		PRISM_MEM_SET_EXACT(d, v, 2);
		d += 2;
	}
	if (n & 1u) *d = (unsigned char)v;
	return dst;
}

static inline PRISM_ALWAYS_INLINE PRISM_NOBUILTIN_MEMOPS void *prism_memzero_bounded(void *dst, size_t n) {
	return prism_memset_bounded(dst, 0, n);
}

static inline PRISM_ALWAYS_INLINE PRISM_NOBUILTIN_MEMOPS int prism_memcmp_bounded(const void *a,
										 const void *b, size_t n) {
	PRISM_ASSUME(n <= (size_t)PRISM_MEM_BOUNDED_BYTES);
	const unsigned char *pa = (const unsigned char *)a;
	const unsigned char *pb = (const unsigned char *)b;
	/* Word ladder first — hashmap/token keys are short; avoid byte tax. */
	while (n >= 8u) {
		uint64_t av, bv;
		PRISM_MEM_COPY_EXACT(&av, pa, 8);
		PRISM_MEM_COPY_EXACT(&bv, pb, 8);
		if (av != bv) {
			/* Match libc memcmp ordering on little-endian hosts. */
			for (unsigned i = 0; i < 8u; i++) {
				if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
			}
		}
		pa += 8;
		pb += 8;
		n -= 8u;
	}
	if (n & 4u) {
		uint32_t av, bv;
		PRISM_MEM_COPY_EXACT(&av, pa, 4);
		PRISM_MEM_COPY_EXACT(&bv, pb, 4);
		if (av != bv) {
			for (unsigned i = 0; i < 4u; i++) {
				if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
			}
		}
		pa += 4;
		pb += 4;
	}
	if (n & 2u) {
		uint16_t av, bv;
		PRISM_MEM_COPY_EXACT(&av, pa, 2);
		PRISM_MEM_COPY_EXACT(&bv, pb, 2);
		if (av != bv) return (int)pa[0] != (int)pb[0] ? (int)pa[0] - (int)pb[0]
							       : (int)pa[1] - (int)pb[1];
		pa += 2;
		pb += 2;
	}
	if (n & 1u) {
		if (*pa != *pb) return (int)*pa - (int)*pb;
	}
	return 0;
}

/* Equality-only hot path: exact-size cases first (hashmap / tokens), then
 * word ladder. No libc. */
static inline PRISM_ALWAYS_INLINE PRISM_NOBUILTIN_MEMOPS bool prism_memeq_bounded_impl(const void *a,
										      const void *b,
										      size_t n) {
	PRISM_ASSUME(n <= (size_t)PRISM_MEM_BOUNDED_BYTES);
	const unsigned char *pa = (const unsigned char *)a;
	const unsigned char *pb = (const unsigned char *)b;
	switch (n) {
	case 0:
		return true;
	case 1:
		return pa[0] == pb[0];
	case 2: {
		uint16_t av, bv;
		PRISM_MEM_COPY_EXACT(&av, pa, 2);
		PRISM_MEM_COPY_EXACT(&bv, pb, 2);
		return av == bv;
	}
	case 3:
		return pa[0] == pb[0] && pa[1] == pb[1] && pa[2] == pb[2];
	case 4: {
		uint32_t av, bv;
		PRISM_MEM_COPY_EXACT(&av, pa, 4);
		PRISM_MEM_COPY_EXACT(&bv, pb, 4);
		return av == bv;
	}
	case 5: {
		uint32_t av, bv;
		PRISM_MEM_COPY_EXACT(&av, pa, 4);
		PRISM_MEM_COPY_EXACT(&bv, pb, 4);
		return av == bv && pa[4] == pb[4];
	}
	case 6: {
		uint32_t a0, b0;
		uint16_t a1, b1;
		PRISM_MEM_COPY_EXACT(&a0, pa, 4);
		PRISM_MEM_COPY_EXACT(&b0, pb, 4);
		PRISM_MEM_COPY_EXACT(&a1, pa + 4, 2);
		PRISM_MEM_COPY_EXACT(&b1, pb + 4, 2);
		return a0 == b0 && a1 == b1;
	}
	case 7: {
		uint32_t a0, b0;
		uint16_t a1, b1;
		PRISM_MEM_COPY_EXACT(&a0, pa, 4);
		PRISM_MEM_COPY_EXACT(&b0, pb, 4);
		PRISM_MEM_COPY_EXACT(&a1, pa + 4, 2);
		PRISM_MEM_COPY_EXACT(&b1, pb + 4, 2);
		return a0 == b0 && a1 == b1 && pa[6] == pb[6];
	}
	case 8: {
		uint64_t av, bv;
		PRISM_MEM_COPY_EXACT(&av, pa, 8);
		PRISM_MEM_COPY_EXACT(&bv, pb, 8);
		return av == bv;
	}
	case 9: {
		uint64_t av, bv;
		PRISM_MEM_COPY_EXACT(&av, pa, 8);
		PRISM_MEM_COPY_EXACT(&bv, pb, 8);
		return av == bv && pa[8] == pb[8];
	}
	case 10: {
		uint64_t a0, b0;
		uint16_t a1, b1;
		PRISM_MEM_COPY_EXACT(&a0, pa, 8);
		PRISM_MEM_COPY_EXACT(&b0, pb, 8);
		PRISM_MEM_COPY_EXACT(&a1, pa + 8, 2);
		PRISM_MEM_COPY_EXACT(&b1, pb + 8, 2);
		return a0 == b0 && a1 == b1;
	}
	case 11: {
		uint64_t a0, b0;
		uint16_t a1, b1;
		PRISM_MEM_COPY_EXACT(&a0, pa, 8);
		PRISM_MEM_COPY_EXACT(&b0, pb, 8);
		PRISM_MEM_COPY_EXACT(&a1, pa + 8, 2);
		PRISM_MEM_COPY_EXACT(&b1, pb + 8, 2);
		return a0 == b0 && a1 == b1 && pa[10] == pb[10];
	}
	case 12: {
		uint64_t a0, b0;
		uint32_t a1, b1;
		PRISM_MEM_COPY_EXACT(&a0, pa, 8);
		PRISM_MEM_COPY_EXACT(&b0, pb, 8);
		PRISM_MEM_COPY_EXACT(&a1, pa + 8, 4);
		PRISM_MEM_COPY_EXACT(&b1, pb + 8, 4);
		return a0 == b0 && a1 == b1;
	}
	case 13: {
		uint64_t a0, b0;
		uint32_t a1, b1;
		PRISM_MEM_COPY_EXACT(&a0, pa, 8);
		PRISM_MEM_COPY_EXACT(&b0, pb, 8);
		PRISM_MEM_COPY_EXACT(&a1, pa + 8, 4);
		PRISM_MEM_COPY_EXACT(&b1, pb + 8, 4);
		return a0 == b0 && a1 == b1 && pa[12] == pb[12];
	}
	case 14: {
		uint64_t a0, b0;
		uint32_t a1, b1;
		uint16_t a2, b2;
		PRISM_MEM_COPY_EXACT(&a0, pa, 8);
		PRISM_MEM_COPY_EXACT(&b0, pb, 8);
		PRISM_MEM_COPY_EXACT(&a1, pa + 8, 4);
		PRISM_MEM_COPY_EXACT(&b1, pb + 8, 4);
		PRISM_MEM_COPY_EXACT(&a2, pa + 12, 2);
		PRISM_MEM_COPY_EXACT(&b2, pb + 12, 2);
		return a0 == b0 && a1 == b1 && a2 == b2;
	}
	case 15: {
		uint64_t a0, b0;
		uint32_t a1, b1;
		uint16_t a2, b2;
		PRISM_MEM_COPY_EXACT(&a0, pa, 8);
		PRISM_MEM_COPY_EXACT(&b0, pb, 8);
		PRISM_MEM_COPY_EXACT(&a1, pa + 8, 4);
		PRISM_MEM_COPY_EXACT(&b1, pb + 8, 4);
		PRISM_MEM_COPY_EXACT(&a2, pa + 12, 2);
		PRISM_MEM_COPY_EXACT(&b2, pb + 12, 2);
		return a0 == b0 && a1 == b1 && a2 == b2 && pa[14] == pb[14];
	}
	case 16: {
		uint64_t a0, b0, a1, b1;
		PRISM_MEM_COPY_EXACT(&a0, pa, 8);
		PRISM_MEM_COPY_EXACT(&b0, pb, 8);
		PRISM_MEM_COPY_EXACT(&a1, pa + 8, 8);
		PRISM_MEM_COPY_EXACT(&b1, pb + 8, 8);
		return a0 == b0 && a1 == b1;
	}
	default:
		break;
	}
	while (n >= 8u) {
		uint64_t av, bv;
		PRISM_MEM_COPY_EXACT(&av, pa, 8);
		PRISM_MEM_COPY_EXACT(&bv, pb, 8);
		if (av != bv) return false;
		pa += 8;
		pb += 8;
		n -= 8u;
	}
	if (n & 4u) {
		uint32_t av, bv;
		PRISM_MEM_COPY_EXACT(&av, pa, 4);
		PRISM_MEM_COPY_EXACT(&bv, pb, 4);
		if (av != bv) return false;
		pa += 4;
		pb += 4;
	}
	if (n & 2u) {
		uint16_t av, bv;
		PRISM_MEM_COPY_EXACT(&av, pa, 2);
		PRISM_MEM_COPY_EXACT(&bv, pb, 2);
		if (av != bv) return false;
		pa += 2;
		pb += 2;
	}
	if (n & 1u && *pa != *pb) return false;
	return true;
}

/* ── static (compile-time n; exact inline) ────────────────────────── */

#if PRISM_MEM_STATIC && (defined(__clang__) || defined(__GNUC__))
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
#define prism_memcmp_static(a, b, n)                                                                         \
	__extension__({                                                                                      \
		PRISM_REQUIRE_CONST(n);                                                                      \
		const unsigned char *_pa = (const unsigned char *)(a);                                       \
		const unsigned char *_pb = (const unsigned char *)(b);                                       \
		int _ret = 0;                                                                                \
		for (size_t _i = 0; _i < (size_t)(n); _i++) {                                                \
			unsigned char _av = _pa[_i], _bv = _pb[_i];                                          \
			if (_av != _bv) {                                                                    \
				_ret = (int)_av - (int)_bv;                                                  \
				break;                                                                       \
			}                                                                                    \
		}                                                                                            \
		_ret;                                                                                        \
	})
#else
#define prism_memcpy_static(dst, src, n) prism_memcpy_runtime((dst), (src), (size_t)(n))
#define prism_memset_static(dst, v, n) prism_memset_runtime((dst), (v), (size_t)(n))
#define prism_memzero_static(dst, n) prism_memzero_runtime((dst), (size_t)(n))
#define prism_memcmp_static(a, b, n) prism_memcmp_runtime((a), (b), (size_t)(n))
#endif

/* Bool helpers that still name the tier. */
#define prism_memeq_static(a, b, n) (prism_memcmp_static((a), (b), (n)) == 0)
#if PRISM_MEM_BOUNDED_EQ
#define prism_memeq_bounded(a, b, n) prism_memeq_bounded_impl((a), (b), (n))
#else
#define prism_memeq_bounded(a, b, n) (prism_memcmp_runtime((a), (b), (n)) == 0)
#endif
#define prism_memeq_runtime(a, b, n) (prism_memcmp_runtime((a), (b), (n)) == 0)

/* Runtime n whose magnitude is not proven at the call site: prefer the
 * bounded no-libc ladder when n fits, else *_runtime. Prefer an explicit
 * *_static / *_bounded call when the tier is known. */
static inline PRISM_ALWAYS_INLINE PRISM_PURE bool prism_memeq_runtime_sized(const void *a, const void *b,
									   size_t n) {
#if PRISM_MEM_BOUNDED_EQ
	if (n == 0) return true;
	if (PRISM_LIKELY(n <= (size_t)PRISM_MEM_BOUNDED_BYTES)) return prism_memeq_bounded(a, b, n);
	return prism_memeq_runtime(a, b, n);
#else
	return n == 0 || prism_memcmp_runtime(a, b, n) == 0;
#endif
}

static inline PRISM_ALWAYS_INLINE void *prism_memcpy_runtime_sized(void *restrict dst,
								  const void *restrict src, size_t n) {
#if PRISM_MEM_BOUNDED_COPY
	if (n == 0) return dst;
	if (PRISM_LIKELY(n <= (size_t)PRISM_MEM_BOUNDED_BYTES)) return prism_memcpy_bounded(dst, src, n);
	return prism_memcpy_runtime(dst, src, n);
#else
	return prism_memcpy_runtime(dst, src, n);
#endif
}

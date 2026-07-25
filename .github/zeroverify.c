/* zeroverify.c — runtime proof that Prism auto zero-initialization actually
 * ZEROES the object, under a poisoned stack.
 *
 * Why this exists: the main suite's zero-init runtime checks (`int x;
 * CHECK(x==0)`) read locals on an uncontrolled stack that is often naturally
 * zero, so a *dropped* initializer passes by luck.  The strong existing guard
 * is transpile-level ("did prism emit an initializer?"); this file adds the
 * missing runtime guard ("does the emitted initializer zero the bytes?").
 *
 * Method: `dirty()` fills a large stack region with 0xAA; the next same-depth
 * call reuses that region.  Each tested function carries a `raw` pad array so
 * its frame is large and stays poisoned, guaranteeing the tested object's
 * slot lands in poisoned memory.  Two twins per shape:
 *   chk_*  : `T x;`      — prism zero-inits — must read all-zero bytes.
 *   pois_* : `raw T x;`  — suppressed       — must read POISON (nonzero).
 * The pois_ twin is the SELF-VALIDATION: if it reads nonzero, the poison
 * landed and the chk_ result is meaningful; if it reads zero, poison was
 * ineffective on this platform/opt and the shape is SKIPPED (never a false
 * pass).  Reading indeterminate bytes through `unsigned char` is well defined
 * (no trap representations), so the pois_ twin is not itself UB.
 *
 * MUST be compiled at -O0 (poison relies on predictable stack layout):
 *   prism -O0 zeroverify.c -o zv && ./zv
 * Not run on MSVC (no VLA/_Atomic/_Complex/_BitInt/raw-as-tested-here).
 */
#include <stdio.h>
#include <string.h>

typedef int T_int;
typedef struct {
	int a;
	char b;
	long c;
} T_struct;

static volatile unsigned char *g_sink;
__attribute__((noinline)) static void dirty(void) {
	volatile unsigned char big[65536];
	memset((void *)big, 0xAA, sizeof big);
	g_sink = big;
}
static int is_zero(const void *p, unsigned long n) {
	const unsigned char *b = p;
	for (unsigned long i = 0; i < n; i++)
		if (b[i]) return 0;
	return 1;
}

/* A shape defines chk_/pois_ returning "is the object all-zero bytes?". */
#define SHAPE(name, DECL, ADDR, SIZE)                                                                \
	__attribute__((noinline)) static int chk_##name(void) {                                     \
		raw unsigned char _pad[2048];                                                        \
		(void)_pad;                                                                          \
		DECL;                                                                                \
		return is_zero((const void *)(ADDR), (unsigned long)(SIZE));                         \
	}                                                                                           \
	__attribute__((noinline)) static int pois_##name(void) {                                    \
		raw unsigned char _pad[2048];                                                        \
		(void)_pad;                                                                          \
		raw DECL;                                                                            \
		return is_zero((const void *)(ADDR), (unsigned long)(SIZE));                         \
	}

/* scalars */
SHAPE(i_char, char x, &x, sizeof x)
SHAPE(i_short, short x, &x, sizeof x)
SHAPE(i_int, int x, &x, sizeof x)
SHAPE(i_long, long x, &x, sizeof x)
SHAPE(i_llong, long long x, &x, sizeof x)
SHAPE(i_float, float x, &x, sizeof x)
SHAPE(i_double, double x, &x, sizeof x)
SHAPE(i_ldouble, long double x, &x, sizeof x)
SHAPE(i_ptr, int *x, &x, sizeof x)
SHAPE(i_vptr, void *x, &x, sizeof x)
SHAPE(i_fptr, int (*x)(int), &x, sizeof x)
SHAPE(i_bool, _Bool x, &x, sizeof x)
SHAPE(i_enum, enum E_ { EA_ } x, &x, sizeof x)
SHAPE(i_cfloat, float _Complex x, &x, sizeof x)
SHAPE(i_cdouble, double _Complex x, &x, sizeof x)
/* arrays */
SHAPE(a_1d, int x[13], x, sizeof x)
SHAPE(a_2d, int x[7][5], x, sizeof x)
SHAPE(a_char, char x[37], x, sizeof x)
/* structs / unions */
SHAPE(s_flat, struct { int a; char b; double c; } x, &x, sizeof x)
SHAPE(s_nested, struct { int a; struct { char c; long d; } in; int e; } x, &x, sizeof x)
SHAPE(s_bitfield, struct { unsigned a : 3; unsigned b : 5; int c; unsigned d : 1; } x, &x, sizeof x)
SHAPE(s_arrmem, struct { int hdr; char buf[40]; int tail; } x, &x, sizeof x)
SHAPE(u_basic, union { int a; double b; char c[16]; } x, &x, sizeof x)
SHAPE(a_of_s, struct P_ { int a; char b; } x[9], x, sizeof x)
/* qualified */
SHAPE(q_atomic_int, _Atomic int x, (void *)&x, sizeof x)
SHAPE(q_atomic_ptr, _Atomic(int *) x, (void *)&x, sizeof x)
SHAPE(q_atomic_agg, _Atomic struct { int a; long b; } x, (void *)&x, sizeof x)
SHAPE(q_vol_int, volatile int x, (int *)&x, sizeof x)
SHAPE(q_vol_agg, volatile struct { int a; char b[20]; } x, (void *)&x, sizeof x)
/* typedef-hidden */
SHAPE(t_int, T_int x, &x, sizeof x)
SHAPE(t_struct, T_struct x, &x, sizeof x)
#if defined(__BITINT_MAXWIDTH__)
SHAPE(i_bitint, _BitInt(37) x, &x, sizeof x)
#endif

/* VLA shapes take a runtime n. */
#define VSHAPE(name, DECL, ADDR, SIZE)                                                               \
	__attribute__((noinline)) static int chk_##name(int n) {                                    \
		raw unsigned char _pad[2048];                                                        \
		(void)_pad;                                                                          \
		DECL;                                                                                \
		return is_zero((const void *)(ADDR), (unsigned long)(SIZE));                         \
	}                                                                                           \
	__attribute__((noinline)) static int pois_##name(int n) {                                   \
		raw unsigned char _pad[2048];                                                        \
		(void)_pad;                                                                          \
		raw DECL;                                                                            \
		return is_zero((const void *)(ADDR), (unsigned long)(SIZE));                         \
	}
VSHAPE(v_int, int x[n], x, sizeof x)
VSHAPE(v_2d, int x[n][3], x, sizeof x)
/* typedef-hidden VLA */
__attribute__((noinline)) static int chk_v_typedef(int n) {
	raw unsigned char _pad[2048];
	(void)_pad;
	typedef int TV[n];
	TV x;
	return is_zero((const void *)(x), (unsigned long)(sizeof x));
}
__attribute__((noinline)) static int pois_v_typedef(int n) {
	raw unsigned char _pad[2048];
	(void)_pad;
	typedef int TV[n];
	raw TV x;
	return is_zero((const void *)(x), (unsigned long)(sizeof x));
}

static int checked, skipped, fails;
/* VER: skip if poison ineffective for this shape; else require zeroed. */
#define VER(nm, chk, pois)                                                                           \
	do {                                                                                        \
		dirty();                                                                             \
		int landed = !(pois);                                                                \
		dirty();                                                                             \
		int zeroed = (chk);                                                                  \
		if (!landed)                                                                         \
			skipped++;                                                                   \
		else if (!zeroed) {                                                                  \
			printf("ZERO-INIT MISS: %s (poison landed, object not zeroed)\n", nm);       \
			fails++;                                                                     \
		} else                                                                              \
			checked++;                                                                   \
	} while (0)

int main(void) {
	VER("char", chk_i_char(), pois_i_char());
	VER("short", chk_i_short(), pois_i_short());
	VER("int", chk_i_int(), pois_i_int());
	VER("long", chk_i_long(), pois_i_long());
	VER("long long", chk_i_llong(), pois_i_llong());
	VER("float", chk_i_float(), pois_i_float());
	VER("double", chk_i_double(), pois_i_double());
	VER("long double", chk_i_ldouble(), pois_i_ldouble());
	VER("int*", chk_i_ptr(), pois_i_ptr());
	VER("void*", chk_i_vptr(), pois_i_vptr());
	VER("funcptr", chk_i_fptr(), pois_i_fptr());
	VER("_Bool", chk_i_bool(), pois_i_bool());
	VER("enum", chk_i_enum(), pois_i_enum());
	VER("float _Complex", chk_i_cfloat(), pois_i_cfloat());
	VER("double _Complex", chk_i_cdouble(), pois_i_cdouble());
	VER("int[13]", chk_a_1d(), pois_a_1d());
	VER("int[7][5]", chk_a_2d(), pois_a_2d());
	VER("char[37]", chk_a_char(), pois_a_char());
	VER("struct flat", chk_s_flat(), pois_s_flat());
	VER("struct nested", chk_s_nested(), pois_s_nested());
	VER("struct bitfield", chk_s_bitfield(), pois_s_bitfield());
	VER("struct w/array", chk_s_arrmem(), pois_s_arrmem());
	VER("union", chk_u_basic(), pois_u_basic());
	VER("array of struct", chk_a_of_s(), pois_a_of_s());
	VER("_Atomic int", chk_q_atomic_int(), pois_q_atomic_int());
	VER("_Atomic ptr", chk_q_atomic_ptr(), pois_q_atomic_ptr());
	VER("_Atomic aggregate", chk_q_atomic_agg(), pois_q_atomic_agg());
	VER("volatile int", chk_q_vol_int(), pois_q_vol_int());
	VER("volatile aggregate", chk_q_vol_agg(), pois_q_vol_agg());
	VER("typedef int", chk_t_int(), pois_t_int());
	VER("typedef struct", chk_t_struct(), pois_t_struct());
#if defined(__BITINT_MAXWIDTH__)
	VER("_BitInt(37)", chk_i_bitint(), pois_i_bitint());
#endif
	VER("VLA int[n]", chk_v_int(5), pois_v_int(5));
	VER("VLA int[n][3]", chk_v_2d(5), pois_v_2d(5));
	VER("typedef-hidden VLA", chk_v_typedef(5), pois_v_typedef(5));

	printf("zeroverify: %d checked, %d skipped(poison ineffective), %d MISSES\n", checked,
	       skipped, fails);
	if (checked == 0)
		printf("zeroverify: WARNING — poison ineffective for ALL shapes on this "
		       "platform/opt; result is vacuous (need -O0 and a downward stack)\n");
	return fails ? 1 : 0;
}

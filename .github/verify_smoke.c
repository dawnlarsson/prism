/* verify_smoke.c — a small, real defer/orelse/zero-init program used to
 * exercise `--prism-verify` in CI on a representative (non-self-referential)
 * input.  Unlike test.c, this does not #include the transpiler or any
 * test-data source, so its emitted C is an ordinary lowered program: a clean
 * target for the per-compile translation-validation certificate.  It must
 * transpile, verify (re-transpile leaks no operator keyword), compile, and
 * run returning 0. */
#include <stdio.h>

static volatile int z;
static int calls;
static int g(int v) {
	calls++;
	return v;
}
static void cleanup(const char *tag) {
	printf("cleanup:%s\n", tag);
}

/* defer LIFO across nested scopes + return-value capture */
static int deferred(int sel) {
	defer cleanup("outer");
	int acc = 0;
	{
		defer cleanup("inner");
		int t = g(sel) orelse 10; /* compound-assign orelse is rejected; use a temp */
		acc += t;
	}
	for (int i = 0; i < 3; i++) {
		defer cleanup("loop");
		if (i == sel) break;
	}
	return acc;
}

/* orelse forms: decl-init value, chain, control-flow payloads */
static int forms(int a, int b) {
	int x = g(a) orelse 100;
	int y = g(a) orelse g(b) orelse 200;
	int w = g(b) orelse return -1;
	(void)w;
	return x + y;
}

/* bracket-dimension orelse (fixed-size when operands are constant) */
static int dims(void) {
	int arr[(0) orelse 4];
	for (int i = 0; i < (int)(sizeof(arr) / sizeof(arr[0])); i++) arr[i] = i;
	return (int)(sizeof(arr) / sizeof(arr[0]));
}

/* zero-init interaction: auto object with no initializer */
static int zeroed(void) {
	int n;	   /* auto-zeroed by Prism */
	int m = 5; /* explicit */
	return n + m;
}

int main(void) {
	int rc = 0;
	if (deferred(1) != 1) rc |= 1;
	if (forms(3, 7) != 6) rc |= 2;
	if (dims() != 4) rc |= 4;
	if (zeroed() != 5) rc |= 8;
	(void)z;
	(void)calls;
	if (rc == 0) printf("verify_smoke: OK\n");
	else
		printf("verify_smoke: FAIL rc=%d\n", rc);
	return rc;
}

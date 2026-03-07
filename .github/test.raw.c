void test_raw_basic(void) {
	raw int x;
	x = 42;
	CHECK_EQ(x, 42, "raw int assignment");

	raw char c;
	c = 'A';
	CHECK_EQ(c, 'A', "raw char assignment");
}

void test_raw_array(void) {
	raw int arr[100];
	arr[0] = 1;
	arr[99] = 99;
	CHECK(arr[0] == 1 && arr[99] == 99, "raw array assignment");
}

void test_raw_pointer(void) {
	raw int *p;
	int val = 123;
	p = &val;
	CHECK_EQ(*p, 123, "raw pointer assignment");
}

void test_raw_struct(void) {
	raw struct {
		int a;
		int b;
	} s;

	s.a = 10;
	s.b = 20;
	CHECK(s.a == 10 && s.b == 20, "raw struct assignment");
}

void test_raw_with_qualifiers(void) {
	raw volatile int v;
	v = 100;
	CHECK_EQ(v, 100, "raw volatile int");

	raw const int *cp;
	int val = 50;
	cp = &val;
	CHECK_EQ(*cp, 50, "raw const pointer");
}

void test_raw_variable_assignment(void) {
	int raw, edit;
	raw = edit = 0;
	CHECK(raw == 0 && edit == 0, "raw = edit = 0 (bash pattern)");

	raw = 42;
	CHECK_EQ(raw, 42, "raw = 42");

	raw += 10;
	CHECK_EQ(raw, 52, "raw += 10");

	raw -= 2;
	CHECK_EQ(raw, 50, "raw -= 2");

	raw *= 2;
	CHECK_EQ(raw, 100, "raw *= 2");

	raw /= 4;
	CHECK_EQ(raw, 25, "raw /= 4");

	raw %= 10;
	CHECK_EQ(raw, 5, "raw %= 10");

	raw = 0xFF;
	raw &= 0x0F;
	CHECK_EQ(raw, 0x0F, "raw &= 0x0F");

	raw |= 0xF0;
	CHECK_EQ(raw, 0xFF, "raw |= 0xF0");

	raw ^= 0x0F;
	CHECK_EQ(raw, 0xF0, "raw ^= 0x0F");

	raw = 8;
	raw <<= 2;
	CHECK_EQ(raw, 32, "raw <<= 2");

	raw >>= 1;
	CHECK_EQ(raw, 16, "raw >>= 1");
}

void test_raw_variable_comparison(void) {
	int raw = 10;

	CHECK(raw == 10, "raw == 10");
	CHECK(raw != 5, "raw != 5");
	CHECK(raw < 20, "raw < 20");
	CHECK(raw > 5, "raw > 5");
	CHECK(raw <= 10, "raw <= 10");
	CHECK(raw >= 10, "raw >= 10");
}

void test_raw_variable_arithmetic(void) {
	int raw = 10;
	int result;

	result = raw + 5;
	CHECK_EQ(result, 15, "raw + 5");

	result = raw - 3;
	CHECK_EQ(result, 7, "raw - 3");

	result = raw * 2;
	CHECK_EQ(result, 20, "raw * 2");

	result = raw / 2;
	CHECK_EQ(result, 5, "raw / 2");

	result = raw % 3;
	CHECK_EQ(result, 1, "raw % 3");
}

void test_raw_variable_bitwise(void) {
	int raw = 0b1010;
	int result;

	result = raw & 0b1100;
	CHECK_EQ(result, 0b1000, "raw & mask");

	result = raw | 0b0101;
	CHECK_EQ(result, 0b1111, "raw | mask");

	result = raw ^ 0b1111;
	CHECK_EQ(result, 0b0101, "raw ^ mask");

	result = raw << 2;
	CHECK_EQ(result, 0b101000, "raw << 2");

	result = raw >> 1;
	CHECK_EQ(result, 0b0101, "raw >> 1");
}

void test_raw_variable_logical(void) {
	int raw = 1;
	int other = 0;

	CHECK((raw && 1), "raw && 1");
	CHECK((raw || other), "raw || other");
}

void test_raw_variable_incr_decr(void) {
	int raw = 10;

	raw++;
	CHECK_EQ(raw, 11, "raw++");

	raw--;
	CHECK_EQ(raw, 10, "raw--");

	++raw;
	CHECK_EQ(raw, 11, "++raw");

	--raw;
	CHECK_EQ(raw, 10, "--raw");
}

void test_raw_variable_array(void) {
	int arr[] = {10, 20, 30};
	int raw = 1;

	CHECK_EQ(arr[raw], 20, "arr[raw]");

	int *raw_ptr = arr;
	CHECK_EQ(raw_ptr[2], 30, "raw_ptr[2]");
}

void test_raw_variable_member(void) {
	struct point {
		int x;
		int y;
	};
	struct point raw;
	raw.x = 5;
	raw.y = 10;
	CHECK(raw.x == 5 && raw.y == 10, "raw.x and raw.y");

	struct point s = {100, 200};
	struct point *raw_ptr = &s;
	CHECK(raw_ptr->x == 100 && raw_ptr->y == 200, "raw_ptr->x and raw_ptr->y");
}

static int _raw_identity(int x) {
	return x;
}

void test_raw_variable_function_call(void) {
	int (*raw)(int) = _raw_identity;
	CHECK_EQ(raw(42), 42, "raw(42) function pointer call");
}

void test_raw_variable_comma(void) {
	int raw = 0;
	int result;
	result = (raw = 5, raw + 10);
	CHECK_EQ(result, 15, "raw in comma expression");
}

void test_raw_variable_semicolon(void) {
	int raw = 10;
	int x = raw; // raw followed by semicolon (after x = raw)
	CHECK_EQ(x, 10, "int x = raw;");

	// raw alone on statement (like last expression in void function)
	raw; // This should compile - raw is a variable
	CHECK_EQ(raw, 10, "raw; as statement");
}

void test_raw_variable_ternary(void) {
	int raw = 1;
	int result = raw ? 100 : 200;
	CHECK_EQ(result, 100, "raw ? 100 : 200");

	result = 0 ? raw : 50;
	CHECK_EQ(result, 50, "0 ? raw : 50");
}

void test_raw_keyword_static(void) {
	raw static int x = 5;
	// Just verify it compiles and has the right value
	// Don't modify x since this test might be called multiple times
	CHECK(x >= 5, "raw static int x = 5");
}

extern int some_external_var;

void test_raw_keyword_extern_decl(void) {
	PrismResult result = prism_transpile_source(
	    "raw extern int some_external_var;\n"
	    "int *addr(void) { return &some_external_var; }\n",
	    "raw_extern_decl.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "raw extern declaration transpiles");
	if (result.output) {
		CHECK(strstr(result.output, "some_external_var =") == NULL,
		      "raw extern declaration does not gain initializer");
		CHECK(strstr(result.output, "some_external_var") != NULL,
		      "raw extern declaration preserves referenced symbol");
	}
	prism_free(&result);
}

void test_raw_mixed_usage(void) {
	raw int uninitialized_var; // 'raw' as keyword
	uninitialized_var = 42;

	int raw = 100; // 'raw' as variable name

	raw = raw + uninitialized_var; // 'raw' as variable
	CHECK_EQ(raw, 142, "mixed raw keyword and variable");
}

void test_raw_multiple_variables(void) {
	int raw, cooked, done;
	raw = cooked = done = 0;

	raw = 1;
	cooked = 2;
	done = 3;

	CHECK(raw == 1 && cooked == 2 && done == 3, "multiple vars with raw");
}

static int _raw_intval_mock(int x) {
	return x * 2;
}

static int _raw_term_mock(int x) {
	return x + 1;
}

void test_raw_bash_pattern(void) {
	// Exact pattern from bash's builtins/read.def
	int raw, edit, nchars, silent;
	raw = edit = 0;
	nchars = silent = 0;

	if (1) {
		raw = _raw_intval_mock(5);
		edit = _raw_term_mock(3);
	}

	CHECK_EQ(raw, 10, "bash pattern: raw = intval(5)");
	CHECK_EQ(edit, 4, "bash pattern: edit = term(3)");
}

void test_raw_in_switch(void) {
	int raw = 2;
	int result = 0;

	switch (raw) {
	case 1: result = 10; break;
	case 2: result = 20; break;
	default: result = 30; break;
	}

	CHECK_EQ(result, 20, "switch(raw) works");
}

void test_raw_in_loops(void) {
	int raw = 3;
	int count = 0;

	while (raw > 0) {
		count++;
		raw--;
	}
	CHECK_EQ(count, 3, "while(raw > 0)");

	raw = 0;
	for (raw = 0; raw < 5; raw++) {
		count++;
	}
	CHECK_EQ(raw, 5, "for(raw = 0; raw < 5; raw++)");
}

static int _raw_func_with_raw_param(int raw) {
	return raw * 2;
}

void test_raw_as_parameter(void) {
	CHECK_EQ(_raw_func_with_raw_param(21), 42, "raw as function parameter");
}

void test_raw_in_sizeof(void) {
	int raw = 42;
	size_t s = sizeof(raw);
	CHECK_EQ(s, sizeof(int), "sizeof(raw)");
}

void test_raw_address_of(void) {
	int raw = 42;
	int *p = &raw;
	CHECK_EQ(*p, 42, "&raw works");
	*p = 100;
	CHECK_EQ(raw, 100, "*(&raw) = 100 works");
}

void test_raw_in_cast(void) {
	double raw = 3.14159;
	int truncated = (int)raw;
	CHECK_EQ(truncated, 3, "(int)raw");
}

void test_raw_multi_decl(void) {
	// "raw" should apply to all declarators in the statement
	raw int a, b;
	a = 1;
	b = 2; // Initialize to avoid UB check failures if running with sanitizers
	CHECK(a == 1 && b == 2, "raw multi-declaration compiles");
}

void test_raw_string_literals(void) {
	// Test 1: Basic raw string with backslashes
	const char *path = R"(C:\Path\To\File)";
	CHECK(strcmp(path, "C:\\Path\\To\\File") == 0, "raw string preserves backslashes");
	// Test 2: Raw string with quotes
	const char *quoted = R"("Hello" 'World')";
	CHECK(strcmp(quoted, "\"Hello\" 'World'") == 0, "raw string preserves quotes");

	// Test 3: Raw string with newlines
	const char *multiline = R"(Line 1
Line 2
Line 3)";
	CHECK(strchr(multiline, '\n') != NULL, "raw string preserves newlines");

	// Test 4: Raw string with escape-like sequences
	const char *escaped = R"(\n\t\r\0)";
	CHECK(strcmp(escaped, "\\n\\t\\r\\0") == 0, "raw string doesn't interpret escapes");
}

void test_raw_keyword_after_static(void) {
	// raw after static - this was the bug: 'raw' was not consumed
	static raw int raw_after_static;
	CHECK_EQ(raw_after_static, 0, "static raw int: raw consumed, no zero-init");
}

void test_raw_keyword_after_extern(void) {
	PrismResult result = prism_transpile_source(
	    "extern raw int test_raw_extern_var;\n"
	    "int read_it(void) { return test_raw_extern_var; }\n",
	    "raw_after_extern.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "extern raw int transpiles");
	if (result.output) {
		CHECK(strstr(result.output, "test_raw_extern_var =") == NULL,
		      "extern raw int does not gain initializer");
		CHECK(strstr(result.output, "test_raw_extern_var") != NULL,
		      "extern raw int symbol preserved");
	}
	prism_free(&result);
}

int test_raw_extern_var = 42;

void test_raw_keyword_before_static(void) {
	// raw before static - this always worked
	raw static int raw_before_static;
	CHECK_EQ(raw_before_static, 0, "raw static int: raw consumed, no zero-init");
}

void test_raw_string_basic(void) {
	// C23 raw string literal with newlines
	const char *json = R"(
{
    "key": "value"
}
)";
	CHECK(json != NULL, "raw string literal basic");
	CHECK(strlen(json) > 10, "raw string has content");
}

void test_raw_string_with_backslash(void) {
	// Raw string with backslashes (regex pattern) - should NOT be escaped
	const char *regex = R"(\d+\s*\w+)";
	CHECK(regex[0] == '\\', "raw string preserves backslash");
	CHECK(regex[1] == 'd', "raw string no escape processing");
}

void test_raw_string_with_quotes(void) {
	// Raw string containing quote characters
	const char *s = R"(He said "hello")";
	CHECK(strstr(s, "\"hello\"") != NULL, "raw string with quotes");
}

void test_raw_string_with_delimiter(void) {
	// Raw string with custom delimiter to allow )" inside
	const char *code = R"delim(
        const char *s = R"(nested)";
    )delim";
	CHECK(code != NULL, "raw string with delimiter");
}

void test_raw_string_all_escape_sequences(void) {
	// All C escape sequences should be preserved literally
	const char *s = R"(\a\b\f\n\r\t\v\\\'\"\\0\x1F\777)";
	CHECK(s[0] == '\\' && s[1] == 'a', "raw \\a preserved");
	CHECK(s[2] == '\\' && s[3] == 'b', "raw \\b preserved");
	CHECK(strstr(s, "\\n") != NULL, "raw \\n preserved");
	CHECK(strstr(s, "\\0") != NULL, "raw \\0 preserved");
	CHECK(strstr(s, "\\x1F") != NULL, "raw \\x1F preserved");
}

void test_raw_string_multiline_complex(void) {
	// Complex multiline with various special chars
	const char *sql = R"(
SELECT *
FROM users
WHERE name = 'O''Brien'
  AND email LIKE '%@example.com'
  AND data ~ '^\d{3}-\d{4}$'
ORDER BY id DESC;
)";
	CHECK(strstr(sql, "SELECT") != NULL, "raw multiline SELECT");
	CHECK(strstr(sql, "O''Brien") != NULL, "raw multiline escaped quote");
	CHECK(strstr(sql, "\\d{3}") != NULL, "raw multiline regex");
}

void test_raw_string_json_complex(void) {
	// Complex JSON with nested structures
	const char *json = R"({
    "users": [
        {"name": "Alice", "age": 30, "path": "C:\\Users\\Alice"},
        {"name": "Bob", "age": 25, "regex": "^\\w+@\\w+\\.\\w+$"}
    ],
    "config": {
        "escapes": "\t\n\r",
        "unicode": "\u0041\u0042"
    }
})";
	CHECK(strstr(json, "C:\\\\Users") != NULL, "raw JSON backslash path");
	CHECK(strstr(json, "\\\\w+") != NULL, "raw JSON regex pattern");
}

void test_raw_string_empty(void) {
	const char *empty = R"()";
	CHECK(strlen(empty) == 0, "raw empty string");
}

void test_raw_string_single_char(void) {
	const char *a = R"(a)";
	const char *bs = R"(\)";
	const char *qt = R"(")";
	CHECK(strcmp(a, "a") == 0, "raw single char a");
	CHECK(strcmp(bs, "\\") == 0, "raw single backslash");
	CHECK(strcmp(qt, "\"") == 0, "raw single quote");
}

void test_raw_string_only_special_chars(void) {
	const char *s = R"(
	)"; // newline + tab
	CHECK(s[0] == '\n', "raw starts with newline");
	CHECK(s[1] == '\t', "raw has tab");
}

void test_raw_string_parens_inside(void) {
	// Parentheses that don't end the string
	const char *s = R"(func(a, b))";
	CHECK(strcmp(s, "func(a, b)") == 0, "raw with parens inside");

	const char *nested = R"(((((deep)))))";
	CHECK(strcmp(nested, "((((deep))))") == 0, "raw deeply nested parens");
}

void test_raw_string_delimiter_edge_cases(void) {
	// Various delimiter patterns
	const char *s1 = R"x(content)x";
	CHECK(strcmp(s1, "content") == 0, "raw single char delimiter");

	const char *s2 = R"abc123(data)abc123";
	CHECK(strcmp(s2, "data") == 0, "raw alphanumeric delimiter");

	const char *s3 = R"___(underscores)___";
	CHECK(strcmp(s3, "underscores") == 0, "raw underscore delimiter");
}

void test_raw_string_false_endings(void) {
	// Content that looks like it might end the string but doesn't
	const char *s = R"foo()foo not end )foo still not end)foo";
	CHECK(strstr(s, ")foo not end") != NULL, "raw false ending 1");
	CHECK(strstr(s, ")foo still not end") != NULL, "raw false ending 2");
}

void test_raw_string_with_null_like(void) {
	// Content that looks like null but isn't
	const char *s = R"(\0 NUL \x00)";
	CHECK(strlen(s) > 10, "raw null-like not terminated");
	CHECK(strstr(s, "\\0") != NULL, "raw \\0 literal");
	CHECK(strstr(s, "\\x00") != NULL, "raw \\x00 literal");
}

void test_raw_string_wide_prefix(void) {
	// Wide/UTF raw strings
	const wchar_t *ws = LR"(wide\nstring)";
	CHECK(ws != NULL, "LR wide raw string");

	const char *u8s = u8R"(utf8\tstring)";
	CHECK(u8s != NULL, "u8R UTF-8 raw string");
	CHECK(strstr(u8s, "\\t") != NULL, "u8R preserves backslash");
}

void test_raw_string_adjacent_concat(void) {
	// Adjacent string literal concatenation
	const char *s = R"(first)"
			R"(second)";
	CHECK(strstr(s, "first") != NULL, "raw concat first");
	CHECK(strstr(s, "second") != NULL, "raw concat second");

	// Mixed raw and regular
	const char *mixed = R"(raw\n)"
			    "regular\n";
	CHECK(strstr(mixed, "raw\\n") != NULL, "mixed keeps raw backslash");
	CHECK(strchr(mixed, '\n') != NULL, "mixed has real newline");
}

void test_raw_string_in_expressions(void) {
	// Raw strings in various expression contexts
	size_t len = strlen(R"(hello)");
	CHECK(len == 5, "raw in strlen");

	int cmp = strcmp(R"(abc)", "abc");
	CHECK(cmp == 0, "raw in strcmp");

	const char *arr[] = {R"(one)", R"(two\n)", R"(three)"};
	CHECK(strcmp(arr[1], "two\\n") == 0, "raw in array init");
}

void test_raw_string_windows_paths(void) {
	const char *path = R"(C:\Program Files\App\file.txt)";
	CHECK(strstr(path, "C:\\Program") != NULL, "raw windows path");
	CHECK(strstr(path, "\\App\\") != NULL, "raw windows subdir");
}

void test_raw_string_regex_patterns(void) {
	const char *email = R"(^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$)";
	CHECK(strstr(email, "\\.[a-zA-Z]") != NULL, "raw regex dot");

	const char *ip = R"(\b(?:\d{1,3}\.){3}\d{1,3}\b)";
	CHECK(strstr(ip, "\\b") != NULL, "raw regex word boundary");
	CHECK(strstr(ip, "\\d{1,3}") != NULL, "raw regex digit");
}

void test_raw_string_code_snippets(void) {
	const char *c_code = R"(
#include <stdio.h>
int main() {
    printf("Hello, \"World\"!\n");
    return 0;
}
)";
	CHECK(strstr(c_code, "#include") != NULL, "raw C code include");
	CHECK(strstr(c_code, "\\\"World\\\"") != NULL, "raw C code quotes");
	CHECK(strstr(c_code, "\\n") != NULL, "raw C code newline escape");
}

void test_raw_string_html_template(void) {
	const char *html = R"(<!DOCTYPE html>
<html>
<head><title>Test</title></head>
<body>
<script>
    var x = "Hello \"World\"";
    if (a < b && c > d) { }
</script>
</body>
</html>)";
	CHECK(strstr(html, "<!DOCTYPE") != NULL, "raw HTML doctype");
	CHECK(strstr(html, "<script>") != NULL, "raw HTML script");
	CHECK(strstr(html, "\\\"World\\\"") != NULL, "raw HTML JS string");
}

void test_raw_as_function_pointer_var(void) {
	int (*raw)(const char *) = (int (*)(const char *))strlen;
	CHECK(raw("hello") == 5, "raw as function pointer variable");
}

void test_raw_as_loop_counter(void) {
	int sum = 0;
	for (int raw = 0; raw < 5; raw++) sum += raw;
	CHECK_EQ(sum, 10, "raw as loop counter");
}

void test_raw_struct_field_access(void) {
	struct {
		int raw;
	} s;

	s.raw = 77;
	CHECK_EQ(s.raw, 77, "raw as struct field access");
}

void test_raw_vla_skips_zeroinit(void) {
	int n = 4;
	int sum = 0;
	for (int i = 0; i < 3; i++) {
		raw int buf[n];
		buf[0] = i;
		sum += buf[0];
	}
	CHECK_EQ(sum, 3, "raw VLA in loop compiles and runs");
}

typedef int *_RawIntPtr;

void test_raw_star_ptr_decl(void) {
	int x = 42;
	{
		defer(void) 0;
		raw _RawIntPtr p;
		p = &x;
		CHECK_EQ(*p, 42, "raw typedef ptr declaration");

		raw int *q;
		q = &x;
		CHECK_EQ(*q, 42, "raw int *q declaration");
	}
}

void test_typedef_as_raw(void) {
	typedef int raw;
	raw x;
	CHECK_EQ(x, 0, "typedef raw: zero-initialized");
	raw arr[4];
	int all_zero = 1;
	for (int i = 0; i < 4; i++)
		if (arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "typedef raw array: zero-initialized");
}

void test_typedef_raw_with_pointer(void) {
	typedef int raw;
	raw *p;
	CHECK(p == NULL, "typedef raw pointer: zero to null");
}

void test_typedef_raw_multi_decl(void) {
	typedef int raw;
	raw a, b, c;
	CHECK(a == 0 && b == 0 && c == 0, "typedef raw multi-decl: all zeroed");
}

static int _raw_goto_kw_label_helper(int skip) {
	log_reset();
	defer log_append("D");
	if (skip) goto raw;
	log_append("body");
raw:
	log_append("end");
	return 0;
}

void test_goto_keyword_label_defer(void) {
	// Non-skip path: body + end + defer
	_raw_goto_kw_label_helper(0);
	CHECK_LOG("bodyendD", "goto keyword label: non-skip defer fires");
	// Skip path: goto raw jumps to raw: label, defer still fires at return
	_raw_goto_kw_label_helper(1);
	CHECK_LOG("endD", "goto keyword label: skip path defer fires");
}

// Keyword label with zero-init variable — goto does NOT skip over the decl
static int _raw_goto_kw_zeroinit_helper(int skip) {
	int x;
	if (skip) goto defer;
	x = 42;
defer:
	return x;
}

void test_goto_keyword_label_zeroinit(void) {
	CHECK_EQ(_raw_goto_kw_zeroinit_helper(0), 42, "goto keyword label zeroinit: non-skip");
	CHECK_EQ(_raw_goto_kw_zeroinit_helper(1), 0, "goto keyword label zeroinit: skip (x = 0)");
}

static void test_switch_raw_var_in_body(void) {
	int result = 0;
	int x = 2;
	switch (x) {
		raw int y;
	case 1:
		y = 10;
		result = y;
		break;
	case 2:
		y = 20;
		result = y;
		break;
	default:
		y = 30;
		result = y;
		break;
	}
	CHECK_EQ(result, 20, "switch raw var in body");
}

static void test_raw_string_max_delimiter(void) {
	const char *s = R"ABCDEFGHIJKLMNOP(hello raw)ABCDEFGHIJKLMNOP";
	CHECK(strcmp(s, "hello raw") == 0, "raw string 16-char delimiter");
}

static void test_raw_string_near_max_delimiter(void) {
	const char *s = R"ABCDEFGHIJKLMNO(near max)ABCDEFGHIJKLMNO";
	CHECK(strcmp(s, "near max") == 0, "raw string 15-char delimiter");
}

static void test_raw_string_16_char_delimiter(void) {
	const char *s = R"1234567890ABCDEF(sixteen char delim)1234567890ABCDEF";
	CHECK(strcmp(s, "sixteen char delim") == 0, "raw string exactly 16-char delimiter");
}


void test_raw_c23_attribute(void) {
    [[maybe_unused]] raw int x;
    x = 10;
    CHECK_EQ(x, 10, "raw with C23 attribute");
}

void test_raw_gnu_attribute(void) {
    __attribute__((unused)) raw int y;
    y = 20;
    CHECK_EQ(y, 20, "raw with GNU attribute");
}

void test_raw_atomic(void) {
    _Atomic raw int z;
    z = 30;
    CHECK_EQ(z, 30, "raw with _Atomic");
}

void test_raw_qualifier_order(void) {
    volatile raw int a;
    a = 40;
    CHECK_EQ(a, 40, "raw succeeding volatile");

    const raw int b = 50;
    CHECK_EQ(b, 50, "raw succeeding const");
}

void test_raw_register(void) {
    register raw int c;
    c = 60;
    CHECK_EQ(c, 60, "raw with register");
}

void test_raw_thread_local(void) {
    static _Thread_local raw int d;
    d = 70;
    CHECK_EQ(d, 70, "raw with _Thread_local");
}


static void test_raw_multi_declarator_second_var_uninitialized(void) {
	printf("\n--- raw Propagates Across Multi-Declarators ---\n");

	const char *code =
	    "int main(void) {\n"
	    "    raw int x, y;\n"
	    "    return 0;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "raw multi-decl: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "raw multi-decl: transpiles OK");
	CHECK(result.output != NULL, "raw multi-decl: output not NULL");

	bool x_has_init = strstr(result.output, "x = 0") != NULL ||
	                  strstr(result.output, "x = {0}") != NULL;
	bool y_has_init = strstr(result.output, "y = 0") != NULL ||
	                  strstr(result.output, "y = {0}") != NULL;

	CHECK(!x_has_init, "raw multi-decl: x is raw (no init) as expected");
	CHECK(y_has_init,
	      "raw multi-decl: y should be zero-initialized (raw should bind per-variable, not per-declaration)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_static_const_raw_ordering(void) {
	printf("\n--- static const raw Ordering Inconsistency ---\n");

	// "static const raw int x;" should strip 'raw' and not zero-init.
	// Currently handle_storage_raw fails to skip qualifiers, leaving 'raw' in output.
	const char *code =
	    "void f(void) {\n"
	    "    static const raw int x;\n"
	    "    (void)x;\n"
	    "}\n"
	    "int main(void) { f(); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "static const raw: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "static const raw: transpiles OK");
	CHECK(result.output != NULL, "static const raw: output not NULL");

	// 'raw' must be stripped from the output (it's not valid C).
	CHECK(strstr(result.output, " raw ") == NULL,
	      "static const raw: 'raw' keyword must be consumed, not emitted");
	// Should NOT have zero-init (raw suppresses it).
	CHECK(strstr(result.output, "= 0") == NULL && strstr(result.output, "= {0}") == NULL,
	      "static const raw: variable should NOT be zero-initialized (raw suppresses it)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_raw_typedef_blinds_type_tracker(void) {
	printf("\n--- raw typedef Blinds Type Tracker ---\n");

	const char *code =
	    "raw typedef int my_int;\n"
	    "void f(void) {\n"
	    "    my_int x;\n"
	    "    x = 42;\n"
	    "    (void)x;\n"
	    "}\n"
	    "int main(void) { f(); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "raw typedef: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "raw typedef: transpiles OK");
	CHECK(result.output != NULL, "raw typedef: output not NULL");

	// 'raw' should be stripped from the output (not valid C).
	CHECK(strstr(result.output, "raw ") == NULL,
	      "raw typedef: 'raw' keyword must be consumed, not emitted");
	// The typedef must be registered so that 'my_int x' is zero-initialized.
	CHECK(strstr(result.output, "= {0}") != NULL || strstr(result.output, "= 0") != NULL,
	      "raw typedef: variable of typedef type should be zero-initialized");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_raw_static_multi_decl_consistency(void) {
	printf("\n--- raw Static Multi-Declarator Consistency ---\n");

	// "static raw int x, y;" — static variables are auto-zero-initialized by C,
	// so raw for static/extern means "don't touch" for ALL declarators.
	// No = 0 should be added (adding = 0 to extern turns a declaration into a
	// definition, causing linker errors).
	const char *code =
	    "void test(void) {\n"
	    "    {\n"
	    "        static raw int x, y;\n"
	    "        extern raw int a, b;\n"
	    "        raw int c, d;\n"
	    "    }\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "raw static multi-decl: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "raw static multi-decl: transpiles OK");
	CHECK(result.output != NULL, "raw static multi-decl: output not NULL");

	// static: no = 0 (auto-zeroed by C standard)
	CHECK(strstr(result.output, "static int x, y;") != NULL,
	      "raw static multi-decl: static has no = 0 (auto-zeroed)");
	// extern: no = 0 (would turn declaration into definition — linker bomb)
	CHECK(strstr(result.output, "extern int a, b;") != NULL,
	      "raw extern multi-decl: extern has no = 0 (would be linker bomb)");
	// local: d gets = 0 (automatic storage needs explicit zero-init)
	CHECK(strstr(result.output, "d = 0") != NULL,
	      "raw local multi-decl: local d gets = 0 (automatic storage)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_raw_extern_no_initializer(void) {
	printf("\n--- raw extern No Initializer ---\n");

	const char *code =
	    "void test(void) {\n"
	    "    extern raw int x, y;\n"
	    "    extern raw int a;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "raw extern no init: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "raw extern no init: transpiles OK");
	CHECK(result.output != NULL, "raw extern no init: output not NULL");

	// extern declarations must NEVER have = 0 appended
	CHECK(strstr(result.output, "= 0") == NULL,
	      "raw extern no init: no = 0 anywhere (would turn declaration into definition)");
	// The raw keyword itself should be stripped
	CHECK(strstr(result.output, "raw") == NULL,
	      "raw extern no init: raw keyword stripped from output");
	// extern should survive
	CHECK(strstr(result.output, "extern int x, y;") != NULL,
	      "raw extern no init: extern int x, y; preserved");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_raw_prep_dir_pragma(void) {
	printf("\n--- raw TK_PREP_DIR Pragma ---\n");

	const char *code =
	    "void test(void) {\n"
	    "    raw _Pragma(\"pack(push,1)\") int x;\n"
	    "    (void)x;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "raw prep_dir: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "raw prep_dir: transpiles OK");
	CHECK(result.output != NULL, "raw prep_dir: output not NULL");

	// "raw" should NOT appear in the output — it must be consumed as a keyword.
	CHECK(strstr(result.output, "raw") == NULL,
	      "raw prep_dir: 'raw' keyword consumed (not emitted as identifier)");
	// int x should NOT have = 0 (raw suppresses zero-init)
	CHECK(strstr(result.output, "= 0") == NULL && strstr(result.output, "= {0}") == NULL,
	      "raw prep_dir: no zero-init (raw suppression active through pragma)");
	// The pragma should still be present
	CHECK(strstr(result.output, "#pragma pack(push,1)") != NULL,
	      "raw prep_dir: #pragma pack preserved in output");

	prism_free(&result);
	unlink(path);
	free(path);
}

void run_raw_tests(void) {
	printf("\n=== RAW KEYWORD TESTS ===\n");

	// Basic raw keyword
	test_raw_basic();
	test_raw_array();
	test_raw_pointer();
	test_raw_struct();
	test_raw_with_qualifiers();
	test_raw_multi_decl();
	test_raw_keyword_static();
	test_raw_keyword_after_static();
	test_raw_keyword_after_extern();
	test_raw_keyword_before_static();
	test_raw_mixed_usage();
	test_raw_vla_skips_zeroinit();
	test_raw_star_ptr_decl();
	test_switch_raw_var_in_body();

	// raw as variable name (disambiguation torture)
	test_raw_variable_assignment();
	test_raw_variable_comparison();
	test_raw_variable_arithmetic();
	test_raw_variable_bitwise();
	test_raw_variable_logical();
	test_raw_variable_incr_decr();
	test_raw_variable_array();
	test_raw_variable_member();
	test_raw_variable_function_call();
	test_raw_variable_comma();
	test_raw_variable_semicolon();
	test_raw_variable_ternary();
	test_raw_keyword_extern_decl();
	test_raw_multiple_variables();
	test_raw_bash_pattern();
	test_raw_in_switch();
	test_raw_in_loops();
	test_raw_as_parameter();
	test_raw_in_sizeof();
	test_raw_address_of();
	test_raw_in_cast();
	test_raw_as_function_pointer_var();
	test_raw_as_loop_counter();
	test_raw_struct_field_access();

	// typedef named 'raw'
	test_typedef_as_raw();
	test_typedef_raw_with_pointer();
	test_typedef_raw_multi_decl();

	// goto with raw/defer as labels
	test_goto_keyword_label_defer();
	test_goto_keyword_label_zeroinit();

	// Raw string literals (R"(...)")
	test_raw_string_literals();
	test_raw_string_basic();
	test_raw_string_with_backslash();
	test_raw_string_with_quotes();
	test_raw_string_with_delimiter();
	test_raw_string_all_escape_sequences();
	test_raw_string_multiline_complex();
	test_raw_string_json_complex();
	test_raw_string_empty();
	test_raw_string_single_char();
	test_raw_string_only_special_chars();
	test_raw_string_parens_inside();
	test_raw_string_delimiter_edge_cases();
	test_raw_string_false_endings();
	test_raw_string_with_null_like();
	test_raw_string_wide_prefix();
	test_raw_string_adjacent_concat();
	test_raw_string_in_expressions();
	test_raw_string_windows_paths();
	test_raw_string_regex_patterns();
	test_raw_string_code_snippets();
	test_raw_string_html_template();
	test_raw_string_max_delimiter();
	test_raw_string_near_max_delimiter();
	test_raw_string_16_char_delimiter();

	test_raw_c23_attribute();
	test_raw_gnu_attribute();
	test_raw_atomic();
	test_raw_qualifier_order();
	test_raw_register();
	test_raw_thread_local();
	test_raw_multi_declarator_second_var_uninitialized();
	test_static_const_raw_ordering();
	test_raw_typedef_blinds_type_tracker();
	test_raw_static_multi_decl_consistency();
	test_raw_extern_no_initializer();
	test_raw_prep_dir_pragma();
}

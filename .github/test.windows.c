// Windows-specific test suite for Prism C transpiler.
// Exercises defer, orelse, zero-init, and raw with Windows SDK APIs.
// Run with: prism run .github/test.windows.c
//
// Requirements: Windows, MSVC (cl.exe), Desktop development with C++ workload.

#ifdef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>

static void test_zeroinit_win_types(void) {
	printf("\n--- Zero-Init: Windows Types ---\n");

	// Basic Windows typedefs
	DWORD dw;
	CHECK_EQ(dw, 0, "DWORD zero-init");

	HANDLE h;
	CHECK(h == 0, "HANDLE zero-init (NULL)");

	BOOL b;
	CHECK_EQ(b, 0, "BOOL zero-init (FALSE)");

	BYTE by;
	CHECK_EQ(by, 0, "BYTE zero-init");

	WORD w;
	CHECK_EQ(w, 0, "WORD zero-init");

	LONG l;
	CHECK_EQ(l, 0, "LONG zero-init");

	UINT ui;
	CHECK_EQ(ui, 0, "UINT zero-init");

	LPVOID lp;
	CHECK(lp == NULL, "LPVOID zero-init");

	LPCSTR lpc;
	CHECK(lpc == NULL, "LPCSTR zero-init");

	SIZE_T sz;
	CHECK(sz == 0, "SIZE_T zero-init");

	LARGE_INTEGER li;
	CHECK(li.QuadPart == 0, "LARGE_INTEGER zero-init");

	ULARGE_INTEGER uli;
	CHECK(uli.QuadPart == 0, "ULARGE_INTEGER zero-init");

	HMODULE hm;
	CHECK(hm == NULL, "HMODULE zero-init");

	HWND hw;
	CHECK(hw == NULL, "HWND zero-init");

	HINSTANCE hi;
	CHECK(hi == NULL, "HINSTANCE zero-init");

	DWORD_PTR dp;
	CHECK(dp == 0, "DWORD_PTR zero-init");

	INT_PTR ip;
	CHECK(ip == 0, "INT_PTR zero-init");

	ULONG_PTR up;
	CHECK(up == 0, "ULONG_PTR zero-init");
}

static void test_zeroinit_win_structs(void) {
	printf("\n--- Zero-Init: Windows Structs ---\n");

	SYSTEM_INFO si;
	CHECK(si.dwPageSize == 0, "SYSTEM_INFO zero-init dwPageSize");
	CHECK(si.dwNumberOfProcessors == 0, "SYSTEM_INFO zero-init dwNumberOfProcessors");
	CHECK(si.lpMinimumApplicationAddress == NULL, "SYSTEM_INFO zero-init lpMin");

	FILETIME ft;
	CHECK(ft.dwLowDateTime == 0, "FILETIME zero-init low");
	CHECK(ft.dwHighDateTime == 0, "FILETIME zero-init high");

	SYSTEMTIME st;
	CHECK(st.wYear == 0, "SYSTEMTIME zero-init wYear");
	CHECK(st.wMonth == 0, "SYSTEMTIME zero-init wMonth");
	CHECK(st.wDay == 0, "SYSTEMTIME zero-init wDay");

	SECURITY_ATTRIBUTES sa;
	CHECK(sa.nLength == 0, "SECURITY_ATTRIBUTES zero-init nLength");
	CHECK(sa.lpSecurityDescriptor == NULL, "SECURITY_ATTRIBUTES zero-init lpSD");
	CHECK(sa.bInheritHandle == 0, "SECURITY_ATTRIBUTES zero-init bInherit");

	OVERLAPPED ov;
	CHECK(ov.Internal == 0, "OVERLAPPED zero-init Internal");
	CHECK(ov.hEvent == NULL, "OVERLAPPED zero-init hEvent");

	STARTUPINFOA sui;
	CHECK(sui.cb == 0, "STARTUPINFOA zero-init cb");
	CHECK(sui.lpDesktop == NULL, "STARTUPINFOA zero-init lpDesktop");
	CHECK(sui.dwFlags == 0, "STARTUPINFOA zero-init dwFlags");

	PROCESS_INFORMATION pi;
	CHECK(pi.hProcess == NULL, "PROCESS_INFORMATION zero-init hProcess");
	CHECK(pi.hThread == NULL, "PROCESS_INFORMATION zero-init hThread");
	CHECK(pi.dwProcessId == 0, "PROCESS_INFORMATION zero-init dwProcessId");

	CRITICAL_SECTION cs;
	CHECK(cs.LockCount == 0, "CRITICAL_SECTION zero-init LockCount");

	WIN32_FIND_DATAA fd;
	CHECK(fd.dwFileAttributes == 0, "WIN32_FIND_DATAA zero-init dwFileAttributes");
	CHECK(fd.cFileName[0] == 0, "WIN32_FIND_DATAA zero-init cFileName");

	MEMORYSTATUS ms;
	CHECK(ms.dwLength == 0, "MEMORYSTATUS zero-init dwLength");
	CHECK(ms.dwTotalPhys == 0, "MEMORYSTATUS zero-init dwTotalPhys");
}

static void test_zeroinit_win_arrays(void) {
	printf("\n--- Zero-Init: Windows Type Arrays ---\n");

	DWORD arr[16];
	int all_zero = 1;
	for (int i = 0; i < 16; i++) {
		if (arr[i] != 0) { all_zero = 0; break; }
	}
	CHECK(all_zero, "DWORD[16] array zero-init");

	HANDLE handles[8];
	all_zero = 1;
	for (int i = 0; i < 8; i++) {
		if (handles[i] != NULL) { all_zero = 0; break; }
	}
	CHECK(all_zero, "HANDLE[8] array zero-init");

	BYTE buffer[256];
	all_zero = 1;
	for (int i = 0; i < 256; i++) {
		if (buffer[i] != 0) { all_zero = 0; break; }
	}
	CHECK(all_zero, "BYTE[256] array zero-init");

	WCHAR wpath[MAX_PATH];
	CHECK(wpath[0] == 0, "WCHAR[MAX_PATH] first element zero");
	CHECK(wpath[MAX_PATH - 1] == 0, "WCHAR[MAX_PATH] last element zero");

	FILETIME timestamps[4];
	all_zero = 1;
	for (int i = 0; i < 4; i++) {
		if (timestamps[i].dwLowDateTime != 0 || timestamps[i].dwHighDateTime != 0)
			{ all_zero = 0; break; }
	}
	CHECK(all_zero, "FILETIME[4] struct array zero-init");
}

static void test_defer_heap(void) {
	printf("\n--- Defer: HeapAlloc/HeapFree ---\n");
	log_reset();

	HANDLE heap = GetProcessHeap();
	CHECK(heap != NULL, "GetProcessHeap not null");

	{
		void *mem = HeapAlloc(heap, HEAP_ZERO_MEMORY, 1024);
		CHECK(mem != NULL, "HeapAlloc succeeded");
		defer { HeapFree(heap, 0, mem); log_append("F"); }
		log_append("A");

		// Verify heap memory is usable
		memset(mem, 0xAB, 1024);
		CHECK(((unsigned char *)mem)[0] == 0xAB, "heap memory writable");
	}
	CHECK_LOG("AF", "defer HeapFree runs after scope");
}

static void test_defer_virtual_memory(void) {
	printf("\n--- Defer: VirtualAlloc/VirtualFree ---\n");
	log_reset();
	int success = 0;

	{
		void *page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		CHECK(page != NULL, "VirtualAlloc succeeded");
		defer { VirtualFree(page, 0, MEM_RELEASE); log_append("V"); }
		log_append("A");

		// Write pattern to verify page is writable
		memset(page, 0xCD, 4096);
		CHECK(((unsigned char *)page)[0] == 0xCD, "virtual page writable");
		CHECK(((unsigned char *)page)[4095] == 0xCD, "virtual page end writable");
		success = 1;
	}
	CHECK(success, "VirtualAlloc block completed");
	CHECK_LOG("AV", "defer VirtualFree runs");
}

static void test_defer_file_handle(void) {
	printf("\n--- Defer: CreateFile/CloseHandle ---\n");
	log_reset();

	char tmppath[MAX_PATH];
	GetTempPathA(MAX_PATH, tmppath);
	char tmpfile[MAX_PATH];
	GetTempFileNameA(tmppath, "prm", 0, tmpfile);

	{
		HANDLE hf = CreateFileA(tmpfile, GENERIC_WRITE, 0, NULL,
		                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		CHECK(hf != INVALID_HANDLE_VALUE, "CreateFileA write succeeded");
		defer { CloseHandle(hf); log_append("C"); }
		log_append("W");

		const char *msg = "Prism test data";
		DWORD written;
		WriteFile(hf, msg, (DWORD)strlen(msg), &written, NULL);
		CHECK(written == strlen(msg), "WriteFile correct bytes");
	}
	CHECK_LOG("WC", "defer CloseHandle runs after write");

	// Re-open and verify
	{
		HANDLE hf = CreateFileA(tmpfile, GENERIC_READ, 0, NULL,
		                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		CHECK(hf != INVALID_HANDLE_VALUE, "CreateFileA read succeeded");
		defer CloseHandle(hf);

		char buf[64];
		DWORD rd;
		ReadFile(hf, buf, 63, &rd, NULL);
		buf[rd] = '\0';
		CHECK(strcmp(buf, "Prism test data") == 0, "file content round-trip");
	}

	DeleteFileA(tmpfile);
}

static void test_defer_semaphore(void) {
	printf("\n--- Defer: CreateSemaphore/CloseHandle ---\n");
	log_reset();

	{
		HANDLE sem = CreateSemaphoreA(NULL, 1, 1, NULL);
		CHECK(sem != NULL, "CreateSemaphoreA succeeded");
		defer { CloseHandle(sem); log_append("S"); }
		log_append("O");

		DWORD wait = WaitForSingleObject(sem, 0);
		CHECK(wait == WAIT_OBJECT_0, "WaitForSingleObject acquired semaphore");
		defer { ReleaseSemaphore(sem, 1, NULL); log_append("R"); }
		log_append("H");
	}
	CHECK_LOG("OHRS", "defer semaphore release then close (LIFO)");
}

static void test_defer_mutex(void) {
	printf("\n--- Defer: CreateMutex/CloseHandle ---\n");
	log_reset();

	{
		HANDLE mtx = CreateMutexA(NULL, FALSE, NULL);
		CHECK(mtx != NULL, "CreateMutexA succeeded");
		defer { CloseHandle(mtx); log_append("M"); }
		log_append("L");

		DWORD wait = WaitForSingleObject(mtx, 0);
		CHECK(wait == WAIT_OBJECT_0, "WaitForSingleObject got mutex");
		defer { ReleaseMutex(mtx); log_append("U"); }
		log_append("H");
	}
	CHECK_LOG("LHUM", "defer mutex release then close (LIFO)");
}

static void test_defer_event(void) {
	printf("\n--- Defer: CreateEvent/CloseHandle ---\n");
	log_reset();

	{
		HANDLE evt = CreateEventA(NULL, TRUE, FALSE, NULL);
		CHECK(evt != NULL, "CreateEventA succeeded");
		defer { CloseHandle(evt); log_append("E"); }
		log_append("1");

		SetEvent(evt);
		DWORD r = WaitForSingleObject(evt, 0);
		CHECK(r == WAIT_OBJECT_0, "event signaled");
		log_append("2");
	}
	CHECK_LOG("12E", "defer closes event after signal check");
}

static void test_defer_critical_section(void) {
	printf("\n--- Defer: CriticalSection init/delete ---\n");
	log_reset();

	{
		CRITICAL_SECTION cs;
		InitializeCriticalSection(&cs);
		defer { DeleteCriticalSection(&cs); log_append("D"); }
		log_append("I");

		EnterCriticalSection(&cs);
		defer { LeaveCriticalSection(&cs); log_append("L"); }
		log_append("E");

		// Simulate work inside critical section
		volatile int counter = 0;
		counter++;
		CHECK(counter == 1, "work inside critical section");
	}
	CHECK_LOG("IELD", "defer leave then delete critical section (LIFO)");
}

static void test_defer_multi_resource(void) {
	printf("\n--- Defer: Multiple Resource Cleanup ---\n");
	log_reset();

	HANDLE heap = GetProcessHeap();
	char tmppath[MAX_PATH];
	GetTempPathA(MAX_PATH, tmppath);
	char tmpfile[MAX_PATH];
	GetTempFileNameA(tmppath, "prm", 0, tmpfile);

	{
		// Resource 1: heap memory
		void *mem = HeapAlloc(heap, 0, 512);
		CHECK(mem != NULL, "multi: HeapAlloc");
		defer { HeapFree(heap, 0, mem); log_append("1"); }

		// Resource 2: file handle
		HANDLE hf = CreateFileA(tmpfile, GENERIC_WRITE, 0, NULL,
		                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		CHECK(hf != INVALID_HANDLE_VALUE, "multi: CreateFile");
		defer { CloseHandle(hf); log_append("2"); }

		// Resource 3: event
		HANDLE evt = CreateEventA(NULL, TRUE, FALSE, NULL);
		CHECK(evt != NULL, "multi: CreateEvent");
		defer { CloseHandle(evt); log_append("3"); }

		// Resource 4: mutex
		HANDLE mtx = CreateMutexA(NULL, FALSE, NULL);
		CHECK(mtx != NULL, "multi: CreateMutex");
		defer { CloseHandle(mtx); log_append("4"); }

		log_append("W");
	}
	CHECK_LOG("W4321", "multi-resource LIFO cleanup");
	DeleteFileA(tmpfile);
}

static void test_defer_early_return(void) {
	printf("\n--- Defer: Early Return with Win Resources ---\n");
	log_reset();

	HANDLE heap = GetProcessHeap();

	// Lambda-style: use a block to test early return
	int result;
	{
		void *m1 = HeapAlloc(heap, 0, 64);
		defer { HeapFree(heap, 0, m1); log_append("1"); }

		void *m2 = HeapAlloc(heap, 0, 64);
		defer { HeapFree(heap, 0, m2); log_append("2"); }

		log_append("A");
		// Fall through end-of-scope — both defers clean up
	}
	CHECK_LOG("A21", "defer cleanup on scope exit (simulated early return)");
}

static int test_orelse_helper_getmodule(void) {
	HMODULE mod = GetModuleHandleA("kernel32.dll") orelse return -1;
	return (mod != NULL) ? 1 : 0;
}

static int test_orelse_helper_getproc(void) {
	HMODULE mod = GetModuleHandleA("kernel32.dll") orelse return -1;
	FARPROC proc = GetProcAddress(mod, "GetTickCount");
	if (!proc) return -2;
	return 1;
}

static int test_orelse_helper_loadlib_fail(void) {
	HMODULE mod = LoadLibraryA("nonexistent_prism_test_12345.dll") orelse return -99;
	FreeLibrary(mod);
	return 1;
}

static int test_orelse_helper_virtual_alloc(void) {
	void *p = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) orelse return -1;
	defer VirtualFree(p, 0, MEM_RELEASE);
	memset(p, 0xFF, 4096);
	return 1;
}

static int test_orelse_helper_heap_chain(void) {
	HANDLE heap = GetProcessHeap() orelse return -1;

	void *a = HeapAlloc(heap, 0, 128) orelse return -2;
	defer HeapFree(heap, 0, a);

	void *b = HeapAlloc(heap, 0, 256) orelse return -3;
	defer HeapFree(heap, 0, b);

	void *c = HeapAlloc(heap, 0, 512) orelse return -4;
	defer HeapFree(heap, 0, c);

	return 1;
}

static int test_orelse_helper_createfile_fail(void) {
	HANDLE hf = CreateFileA("Z:\\nonexistent\\path\\prism_test.txt",
	                        GENERIC_READ, 0, NULL, OPEN_EXISTING,
	                        FILE_ATTRIBUTE_NORMAL, NULL);
	// CreateFileA returns INVALID_HANDLE_VALUE (not NULL) on failure,
	// so we need manual check — orelse only checks for falsy
	if (hf == INVALID_HANDLE_VALUE) return -1;
	defer CloseHandle(hf);
	return 1;
}

static int test_orelse_helper_tempfile_roundtrip(void) {
	char tmppath[MAX_PATH];
	DWORD tplen = GetTempPathA(MAX_PATH, tmppath) orelse return -1;

	char tmpfile[MAX_PATH];
	UINT tfret = GetTempFileNameA(tmppath, "prm", 0, tmpfile) orelse return -2;

	HANDLE hf = CreateFileA(tmpfile, GENERIC_WRITE, 0, NULL,
	                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hf == INVALID_HANDLE_VALUE) return -3;
	defer CloseHandle(hf);

	DWORD written;
	WriteFile(hf, "test", 4, &written, NULL) orelse return -4;

	DeleteFileA(tmpfile);
	return 1;
}

static void test_orelse_win_apis(void) {
	printf("\n--- Orelse: Windows API ---\n");

	CHECK_EQ(test_orelse_helper_getmodule(), 1, "orelse GetModuleHandleA success");
	CHECK_EQ(test_orelse_helper_getproc(), 1, "orelse GetProcAddress success");
	CHECK_EQ(test_orelse_helper_loadlib_fail(), -99, "orelse LoadLibrary fail returns -99");
	CHECK_EQ(test_orelse_helper_virtual_alloc(), 1, "orelse VirtualAlloc + defer");
	CHECK_EQ(test_orelse_helper_heap_chain(), 1, "orelse heap chain 3-deep");
	CHECK_EQ(test_orelse_helper_createfile_fail(), -1, "orelse CreateFile fail path");
	CHECK_EQ(test_orelse_helper_tempfile_roundtrip(), 1, "orelse temp file round-trip");
}

static int copy_file_with_prism(const char *src, const char *dst) {
	HANDLE hs = CreateFileA(src, GENERIC_READ, FILE_SHARE_READ, NULL,
	                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hs == INVALID_HANDLE_VALUE) return -1;
	defer CloseHandle(hs);

	HANDLE hd = CreateFileA(dst, GENERIC_WRITE, 0, NULL,
	                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hd == INVALID_HANDLE_VALUE) return -2;
	defer CloseHandle(hd);

	char buf[4096];
	DWORD rd, wr;
	while (ReadFile(hs, buf, sizeof(buf), &rd, NULL) && rd > 0) {
		WriteFile(hd, buf, rd, &wr, NULL) orelse return -3;
		if (wr != rd) return -4;
	}
	return 0;
}

static int alloc_chain_with_early_exit(int fail_at) {
	HANDLE heap = GetProcessHeap() orelse return -1;

	void *a = HeapAlloc(heap, 0, 100) orelse return -10;
	defer HeapFree(heap, 0, a);
	if (fail_at == 1) return 1;

	void *b = HeapAlloc(heap, 0, 200) orelse return -20;
	defer HeapFree(heap, 0, b);
	if (fail_at == 2) return 2;

	void *c = HeapAlloc(heap, 0, 300) orelse return -30;
	defer HeapFree(heap, 0, c);
	if (fail_at == 3) return 3;

	void *d = HeapAlloc(heap, 0, 400) orelse return -40;
	defer HeapFree(heap, 0, d);
	if (fail_at == 4) return 4;

	return 0;
}

static void test_combined_patterns(void) {
	printf("\n--- Combined: defer + orelse Real Patterns ---\n");

	// File copy test
	{
		char tmppath[MAX_PATH];
		GetTempPathA(MAX_PATH, tmppath);

		char srcfile[MAX_PATH], dstfile[MAX_PATH];
		GetTempFileNameA(tmppath, "psrc", 0, srcfile);
		GetTempFileNameA(tmppath, "pdst", 0, dstfile);

		// Write source file
		{
			HANDLE hf = CreateFileA(srcfile, GENERIC_WRITE, 0, NULL,
			                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			defer CloseHandle(hf);
			const char *data = "Hello from Prism Windows test!";
			DWORD wr;
			WriteFile(hf, data, (DWORD)strlen(data), &wr, NULL);
		}

		int result = copy_file_with_prism(srcfile, dstfile);
		CHECK_EQ(result, 0, "file copy with defer+orelse");

		// Verify copy
		{
			HANDLE hf = CreateFileA(dstfile, GENERIC_READ, 0, NULL,
			                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			defer CloseHandle(hf);
			char buf[128];
			DWORD rd;
			ReadFile(hf, buf, 127, &rd, NULL);
			buf[rd] = '\0';
			CHECK(strcmp(buf, "Hello from Prism Windows test!") == 0,
			      "copied file content matches");
		}

		DeleteFileA(srcfile);
		DeleteFileA(dstfile);
	}

	// Alloc chain early exit
	CHECK_EQ(alloc_chain_with_early_exit(0), 0, "alloc chain: complete");
	CHECK_EQ(alloc_chain_with_early_exit(1), 1, "alloc chain: exit at 1");
	CHECK_EQ(alloc_chain_with_early_exit(2), 2, "alloc chain: exit at 2");
	CHECK_EQ(alloc_chain_with_early_exit(3), 3, "alloc chain: exit at 3");
	CHECK_EQ(alloc_chain_with_early_exit(4), 4, "alloc chain: exit at 4");
}

static void test_raw_win_types(void) {
	printf("\n--- Raw: Windows Types (no zero-init) ---\n");

	// These should compile without zero-init
	raw BYTE large_buf[65536];
	raw DWORD scratch[1024];
	raw WCHAR wbuf[MAX_PATH];
	raw CHAR cbuf[4096];

	// Immediately overwrite — raw is for perf-critical paths
	memset(large_buf, 0xAA, sizeof(large_buf));
	CHECK(large_buf[0] == 0xAA, "raw BYTE[] overwritten");
	CHECK(large_buf[65535] == 0xAA, "raw BYTE[] end overwritten");

	memset(scratch, 0, sizeof(scratch));
	CHECK(scratch[0] == 0, "raw DWORD[] overwritten with zero");

	wcscpy(wbuf, L"Prism");
	CHECK(wbuf[0] == L'P', "raw WCHAR[] overwritten with string");

	strcpy(cbuf, "test");
	CHECK(cbuf[0] == 't', "raw CHAR[] overwritten");
}

static void test_defer_loop_alloc(void) {
	printf("\n--- Defer: Loop with HeapAlloc ---\n");
	log_reset();

	HANDLE heap = GetProcessHeap();
	int count = 0;

	for (int i = 0; i < 5; i++) {
		void *mem = HeapAlloc(heap, 0, (i + 1) * 100);
		defer HeapFree(heap, 0, mem);

		if (mem) count++;
		memset(mem, (unsigned char)i, (i + 1) * 100);
	}
	CHECK_EQ(count, 5, "loop: all 5 HeapAllocs succeeded + deferred free");
}

static void test_defer_loop_break(void) {
	printf("\n--- Defer: Loop Break with Resources ---\n");
	log_reset();

	HANDLE heap = GetProcessHeap();
	int freed = 0;

	for (int i = 0; i < 10; i++) {
		void *mem = HeapAlloc(heap, 0, 64);
		defer { HeapFree(heap, 0, mem); freed++; }

		if (i == 3) break;
	}
	CHECK_EQ(freed, 4, "loop break: defers ran for i=0..3");
}

static void test_defer_loop_continue(void) {
	printf("\n--- Defer: Loop Continue with Resources ---\n");

	HANDLE heap = GetProcessHeap();
	int freed = 0;

	for (int i = 0; i < 6; i++) {
		void *mem = HeapAlloc(heap, 0, 64);
		defer { HeapFree(heap, 0, mem); freed++; }

		if (i % 2 == 0) continue;
		// Odd iterations do extra work
	}
	CHECK_EQ(freed, 6, "loop continue: all 6 defers ran");
}

static void test_defer_nested_scopes(void) {
	printf("\n--- Defer: Nested Scopes with Win Resources ---\n");
	log_reset();

	HANDLE heap = GetProcessHeap();

	{
		void *outer = HeapAlloc(heap, 0, 100);
		defer { HeapFree(heap, 0, outer); log_append("O"); }
		log_append("1");

		{
			void *middle = HeapAlloc(heap, 0, 200);
			defer { HeapFree(heap, 0, middle); log_append("M"); }
			log_append("2");

			{
				void *inner = HeapAlloc(heap, 0, 300);
				defer { HeapFree(heap, 0, inner); log_append("I"); }
				log_append("3");
			}
			log_append("4");
		}
		log_append("5");
	}
	CHECK_LOG("123I4M5O", "nested 3-level defer with heap");
}

static void test_win_api_queries(void) {
	printf("\n--- Windows API: System Queries ---\n");

	// GetSystemInfo — populates a struct
	{
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		CHECK(si.dwPageSize >= 4096, "GetSystemInfo: page size >= 4096");
		CHECK(si.dwNumberOfProcessors >= 1, "GetSystemInfo: CPU count >= 1");
	}

	// GetVersionExA — deprecated but should still compile
	{
		DWORD ver = GetVersion();
		DWORD major = (DWORD)(LOBYTE(LOWORD(ver)));
		CHECK(major >= 5, "GetVersion: major >= 5");
	}

	// GetComputerNameA
	{
		char name[MAX_COMPUTERNAME_LENGTH + 1];
		DWORD sz = sizeof(name);
		BOOL ok = GetComputerNameA(name, &sz);
		CHECK(ok, "GetComputerNameA succeeded");
		CHECK(sz > 0, "computer name length > 0");
	}

	// GetCurrentDirectory
	{
		char cwd[MAX_PATH];
		DWORD len = GetCurrentDirectoryA(MAX_PATH, cwd);
		CHECK(len > 0, "GetCurrentDirectoryA returned path");
		CHECK(cwd[0] != '\0', "cwd not empty");
	}

	// GetEnvironmentVariable
	{
		char val[MAX_PATH];
		DWORD len = GetEnvironmentVariableA("SYSTEMROOT", val, MAX_PATH);
		CHECK(len > 0, "GetEnvironmentVariable SYSTEMROOT");
		CHECK(strlen(val) > 0, "SYSTEMROOT not empty");
	}

	// GetTickCount
	{
		DWORD ticks = GetTickCount();
		CHECK(ticks > 0, "GetTickCount > 0");
	}

	// QueryPerformanceCounter
	{
		LARGE_INTEGER freq, counter;
		BOOL ok = QueryPerformanceFrequency(&freq);
		CHECK(ok, "QueryPerformanceFrequency succeeded");
		CHECK(freq.QuadPart > 0, "perf freq > 0");

		ok = QueryPerformanceCounter(&counter);
		CHECK(ok, "QueryPerformanceCounter succeeded");
		CHECK(counter.QuadPart > 0, "perf counter > 0");
	}

	// GetModuleFileName
	{
		char path[MAX_PATH];
		DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
		CHECK(len > 0, "GetModuleFileNameA returned path");
	}
}

static DWORD WINAPI thread_func(LPVOID param) {
	int *counter = (int *)param;
	InterlockedIncrement((volatile LONG *)counter);
	return 0;
}

static void test_defer_thread(void) {
	printf("\n--- Defer: Thread Handle Cleanup ---\n");
	log_reset();

	volatile LONG counter = 0;

	{
		HANDLE t1 = CreateThread(NULL, 0, thread_func, (LPVOID)&counter, 0, NULL);
		CHECK(t1 != NULL, "CreateThread 1 succeeded");
		defer { WaitForSingleObject(t1, INFINITE); CloseHandle(t1); log_append("1"); }

		HANDLE t2 = CreateThread(NULL, 0, thread_func, (LPVOID)&counter, 0, NULL);
		CHECK(t2 != NULL, "CreateThread 2 succeeded");
		defer { WaitForSingleObject(t2, INFINITE); CloseHandle(t2); log_append("2"); }

		log_append("S");
	}

	CHECK_LOG("S21", "defer joins+closes threads in LIFO order");
	CHECK_EQ(counter, 2, "both threads incremented counter");
}

static int extreme_combined_test(void) {
	HANDLE heap = GetProcessHeap() orelse return -1;
	DWORD page_size;  // zero-init

	{
		SYSTEM_INFO si;  // zero-init
		GetSystemInfo(&si);
		page_size = si.dwPageSize;
	}

	void *pages[4];  // zero-init (all NULL)
	int allocated = 0;

	for (int i = 0; i < 4; i++) {
		pages[i] = VirtualAlloc(NULL, page_size, MEM_COMMIT | MEM_RESERVE,
		                        PAGE_READWRITE) orelse break;
		allocated++;
	}

	// Clean up with defer in reverse
	for (int i = allocated - 1; i >= 0; i--) {
		VirtualFree(pages[i], 0, MEM_RELEASE);
	}

	DWORD err_code;  // zero-init
	CHECK_EQ(err_code, 0, "extreme: err_code zero-init");
	CHECK(page_size >= 4096, "extreme: got page size");
	CHECK(allocated == 4, "extreme: all 4 pages allocated");

	return allocated;
}

static int extreme_nested_orelse(void) {
	HMODULE k32 = GetModuleHandleA("kernel32.dll") orelse return -1;
	FARPROC gsc = GetProcAddress(k32, "GetSystemInfo");
	if (!gsc) return -2;
	HMODULE ntdll = GetModuleHandleA("ntdll.dll") orelse return -3;
	FARPROC rtl = GetProcAddress(ntdll, "RtlGetVersion");
	if (!rtl) return -4;

	return (gsc && rtl) ? 1 : 0;
}

static void test_extreme_combined(void) {
	printf("\n--- Extreme: All Features Combined ---\n");

	int pages = extreme_combined_test();
	CHECK_EQ(pages, 4, "extreme combined: 4 pages");

	int result = extreme_nested_orelse();
	CHECK_EQ(result, 1, "extreme nested orelse: all procs found");
}

static const char *error_to_string(DWORD err) {
	switch (err) {
	case ERROR_SUCCESS: {
		defer (void)0;  // noop defer in case branch
		return "success";
	}
	case ERROR_FILE_NOT_FOUND:
		return "file not found";
	case ERROR_ACCESS_DENIED:
		return "access denied";
	default:
		return "unknown";
	}
}

static void test_defer_switch(void) {
	printf("\n--- Defer: Switch with Error Codes ---\n");

	CHECK(strcmp(error_to_string(ERROR_SUCCESS), "success") == 0,
	      "switch: ERROR_SUCCESS");
	CHECK(strcmp(error_to_string(ERROR_FILE_NOT_FOUND), "file not found") == 0,
	      "switch: ERROR_FILE_NOT_FOUND");
	CHECK(strcmp(error_to_string(ERROR_ACCESS_DENIED), "access denied") == 0,
	      "switch: ERROR_ACCESS_DENIED");
	CHECK(strcmp(error_to_string(9999), "unknown") == 0,
	      "switch: unknown error");
}

static void test_defer_mmap(void) {
	printf("\n--- Defer: Memory-Mapped File ---\n");

	char tmppath[MAX_PATH];
	GetTempPathA(MAX_PATH, tmppath);
	char tmpfile[MAX_PATH];
	GetTempFileNameA(tmppath, "pmm", 0, tmpfile);

	// Write initial data
	{
		HANDLE hf = CreateFileA(tmpfile, GENERIC_WRITE, 0, NULL,
		                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		defer CloseHandle(hf);

		const char *data = "Memory mapped test data from Prism!";
		DWORD wr;
		WriteFile(hf, data, (DWORD)strlen(data), &wr, NULL);
	}

	// Memory-map and read
	{
		HANDLE hf = CreateFileA(tmpfile, GENERIC_READ, FILE_SHARE_READ, NULL,
		                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		CHECK(hf != INVALID_HANDLE_VALUE, "mmap: open file");
		defer CloseHandle(hf);

		DWORD fileSize = GetFileSize(hf, NULL);
		CHECK(fileSize > 0, "mmap: file has content");

		HANDLE hMapping = CreateFileMappingA(hf, NULL, PAGE_READONLY, 0, 0, NULL);
		CHECK(hMapping != NULL, "mmap: CreateFileMapping");
		defer CloseHandle(hMapping);

		LPVOID view = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
		CHECK(view != NULL, "mmap: MapViewOfFile");
		defer UnmapViewOfFile(view);

		// Three defers stacked: UnmapViewOfFile, CloseHandle(mapping), CloseHandle(file)
		CHECK(memcmp(view, "Memory mapped", 13) == 0, "mmap: content readable");
	}

	DeleteFileA(tmpfile);
}

static void test_wide_string_apis(void) {
	printf("\n--- Wide String: Unicode APIs ---\n");

	WCHAR sysDir[MAX_PATH];  // zero-init
	CHECK(sysDir[0] == 0, "WCHAR array zero-init");

	UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
	CHECK(len > 0, "GetSystemDirectoryW returned path");

	WCHAR winDir[MAX_PATH];  // zero-init
	len = GetWindowsDirectoryW(winDir, MAX_PATH);
	CHECK(len > 0, "GetWindowsDirectoryW returned path");

	// Combine orelse with wide API
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll") orelse {
		CHECK(0, "GetModuleHandleW failed unexpectedly");
		return;
	};
	CHECK(k32 != NULL, "GetModuleHandleW kernel32 succeeded");
}

static void test_defer_stress(void) {
	printf("\n--- Stress: Many Sequential Defers ---\n");

	HANDLE heap = GetProcessHeap();
	int count = 0;

	{
		void *p0  = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p0);  count++; }
		void *p1  = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p1);  count++; }
		void *p2  = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p2);  count++; }
		void *p3  = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p3);  count++; }
		void *p4  = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p4);  count++; }
		void *p5  = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p5);  count++; }
		void *p6  = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p6);  count++; }
		void *p7  = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p7);  count++; }
		void *p8  = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p8);  count++; }
		void *p9  = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p9);  count++; }
		void *p10 = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p10); count++; }
		void *p11 = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p11); count++; }
		void *p12 = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p12); count++; }
		void *p13 = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p13); count++; }
		void *p14 = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p14); count++; }
		void *p15 = HeapAlloc(heap, 0, 32); defer { HeapFree(heap, 0, p15); count++; }
	}
	CHECK_EQ(count, 16, "stress: 16 defers all ran");
}

static int test_goto_cleanup_pattern(void) {
	HANDLE heap = GetProcessHeap();
	int result = -1;

	void *a = HeapAlloc(heap, 0, 100);
	defer HeapFree(heap, 0, a);
	if (!a) return -1;

	void *b = HeapAlloc(heap, 0, 200);
	defer HeapFree(heap, 0, b);
	if (!b) return -2;

	// Simulate success
	result = 0;
	return result;
	// Both defers run on any return path
}

static void test_goto_defer(void) {
	printf("\n--- Goto: Defer Cleanup Pattern ---\n");
	int r = test_goto_cleanup_pattern();
	CHECK_EQ(r, 0, "goto-like cleanup pattern with defer");
}

static int interleaved_flow(int mode) {
	HANDLE heap = GetProcessHeap();
	DWORD result;  // zero-init

	void *mem = HeapAlloc(heap, 0, 256) orelse return -1;
	defer HeapFree(heap, 0, mem);

	switch (mode) {
	case 0:
		result = 100;
		break;
	case 1: {
		void *extra = HeapAlloc(heap, 0, 128) orelse return -2;
		defer HeapFree(heap, 0, extra);
		result = 200;
		break;
	}
	case 2: {
		for (int i = 0; i < 5; i++) {
			void *tmp = HeapAlloc(heap, 0, 64) orelse continue;
			defer HeapFree(heap, 0, tmp);
			result += 10;
		}
		break;
	}
	default:
		result = 999;
		break;
	}

	return (int)result;
}

static void test_interleaved_flow(void) {
	printf("\n--- Interleaved: switch + defer + orelse ---\n");

	CHECK_EQ(interleaved_flow(0), 100, "interleaved: mode 0 (simple)");
	CHECK_EQ(interleaved_flow(1), 200, "interleaved: mode 1 (nested defer)");
	CHECK_EQ(interleaved_flow(2), 50, "interleaved: mode 2 (loop defer)");
	CHECK_EQ(interleaved_flow(99), 999, "interleaved: mode default");
}

// Bug 1: Multi-word return types were truncated in defer.
// "unsigned long long foo()" with defer would capture only "long" as the
// return type, producing an incorrect _Prism_ret_t typedef.
static unsigned long long multiword_ret_with_defer(int x) {
	defer (void)0;
	return (unsigned long long)x * 1000000ULL;
}

// Bug 2: __int8/__int16/__int32/__int64 not recognized as types.
// MSVC-specific integer types need to be in the keyword table for zero-init
// and orelse to work.
static __int32 get_int32_val(void) { return 42; }
static __int64 get_int64_val(void) { return 9999999999LL; }

// Bug 3: MSVC calling conventions (__cdecl, __stdcall) leak into _Prism_ret.
// Functions declared with calling conventions and using defer get the
// calling convention keyword baked into the return type typedef.
// KNOWN BUG — not yet fixed upstream. Test omitted.

// Bug 5: __pragma not registered as TT_ATTR — MSVC preprocessor emits
// __pragma(pack(push, 8)) inline in declarations. Without TT_ATTR, skip_noise
// couldn't skip it, corrupting the prescan pass. This is implicitly tested by
// including <stdio.h> (which emits __pragma), but the typedef+orelse test below
// directly exercises the parse path that was broken.

// Bug 6: Typedef flag corruption — when typedef_add() skipped a duplicate
// re-registration during the transpile pass, the flag-setting code wrote
// is_aggregate on the wrong (last-added) entry, corrupting user typedefs.
typedef int MyTestInt;
typedef unsigned long long MyTestULL;
static MyTestInt get_myint(void) { return 77; }
static MyTestULL get_myull(void) { return 12345ULL; }

static void test_msvc_regressions(void) {
	printf("\n--- MSVC Bug Regressions ---\n");

	// Bug 1: multi-word return type + defer
	unsigned long long r1 = multiword_ret_with_defer(3);
	CHECK(r1 == 3000000ULL, "multi-word return type (unsigned long long) + defer");

	// Bug 2: __int8/__int16/__int32/__int64 zero-init
	{
		__int8  i8;
		__int16 i16;
		__int32 i32;
		__int64 i64;
		CHECK_EQ(i8,  0, "__int8 zero-init");
		CHECK_EQ(i16, 0, "__int16 zero-init");
		CHECK_EQ(i32, 0, "__int32 zero-init");
		CHECK(i64 == 0,  "__int64 zero-init");
	}

	// Bug 2b: __int32/__int64 orelse
	{
		__int32 v32 = get_int32_val() orelse return;
		CHECK_EQ(v32, 42, "__int32 orelse");

		__int64 v64 = get_int64_val() orelse return;
		CHECK(v64 == 9999999999LL, "__int64 orelse");
	}

	// Bug 3: calling convention + defer — KNOWN BUG, test omitted

	// Bug 6: typedef flag corruption — typedef'd scalars + orelse
	// Before the fix, including <stdio.h> (or any SDK header with struct
	// typedefs) would corrupt unrelated user typedefs with is_aggregate=true,
	// causing "orelse on struct/union values is not supported".
	{
		MyTestInt mi = get_myint() orelse return;
		CHECK_EQ(mi, 77, "typedef int + orelse (flag corruption regression)");

		MyTestULL mu = get_myull() orelse return;
		CHECK(mu == 12345ULL, "typedef unsigned long long + orelse (flag corruption regression)");
	}

	// Bug 5+6 combined: size_t (from <stdio.h>) + orelse
	// size_t is a typedef from system headers; this exercises both __pragma
	// parsing (needed for headers to not corrupt prescan) and typedef flag
	// integrity.
	{
		size_t sv = (size_t)strlen("hello") orelse return;
		CHECK_EQ((int)sv, 5, "size_t orelse (header typedef regression)");
	}
}

void run_windows_tests(void) {
	printf("=== PRISM WINDOWS TEST SUITE ===\n");

	// Zero-init
	test_zeroinit_win_types();
	test_zeroinit_win_structs();
	test_zeroinit_win_arrays();

	// Defer with Windows resources
	test_defer_heap();
	test_defer_virtual_memory();
	test_defer_file_handle();
	test_defer_semaphore();
	test_defer_mutex();
	test_defer_event();
	test_defer_critical_section();
	test_defer_multi_resource();
	test_defer_early_return();

	// Orelse
	test_orelse_win_apis();

	// Combined patterns
	test_combined_patterns();

	// Raw
	test_raw_win_types();

	// Defer in loops
	test_defer_loop_alloc();
	test_defer_loop_break();
	test_defer_loop_continue();

	// Nested
	test_defer_nested_scopes();

	// API queries
	test_win_api_queries();

	// Threads
	test_defer_thread();

	// Extreme combined
	test_extreme_combined();

	// Switch
	test_defer_switch();

	// Memory-mapped files
	test_defer_mmap();

	// Wide strings
	test_wide_string_apis();

	// Stress
	test_defer_stress();

	// Goto cleanup
	test_goto_defer();

	// Interleaved
	test_interleaved_flow();

	// MSVC regression tests
	test_msvc_regressions();

	printf("\n========================================\n");
	printf("TOTAL: %d tests, %d passed, %d failed\n", total, passed, failed);
	printf("========================================\n");

	return (failed == 0) ? 0 : 1;
}

#else

void run_windows_tests(void) {
	printf("Windows tests skipped: not running on Windows platform.\n");
}

#endif

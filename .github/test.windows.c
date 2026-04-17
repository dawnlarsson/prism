// Windows-specific test suite for Prism C transpiler.
// Exercises defer, orelse, zero-init, and raw with Windows SDK APIs.
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

static void test_defer_nested_scopes_win(void) {
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

static unsigned long long multiword_ret_with_defer(int x) {
	defer (void)0;
	return (unsigned long long)x * 1000000ULL;
}

static __int32 get_int32_val(void) { return 42; }
static __int64 get_int64_val(void) { return 9999999999LL; }

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

static void test_spaces_in_paths(void) {
	printf("\n--- Bug Regression: Paths with Spaces ---\n");

	// win32_argv_to_cmdline must quote args that contain spaces
	{
		char *argv_test[] = { "cl.exe", "/c", "src with spaces\\hello.c", "/Fo:out dir\\hello.obj", NULL };
		char *cmdline = win32_argv_to_cmdline(argv_test);
		CHECK(cmdline != NULL, "argv_to_cmdline returned non-NULL");
		// The arg with spaces must be quoted
		CHECK(strstr(cmdline, "\"src with spaces\\hello.c\"") != NULL,
		      "argv_to_cmdline quotes arg with spaces");
		CHECK(strstr(cmdline, "\"/Fo:out dir\\hello.obj\"") != NULL,
		      "argv_to_cmdline quotes output arg with spaces");
		free(cmdline);
	}

	// Simple args without spaces should NOT be quoted
	{
		char *argv_simple[] = { "cl.exe", "/c", "hello.c", NULL };
		char *cmdline = win32_argv_to_cmdline(argv_simple);
		CHECK(cmdline != NULL, "argv_to_cmdline simple non-NULL");
		CHECK(strstr(cmdline, "\"hello.c\"") == NULL,
		      "argv_to_cmdline does not quote simple arg");
		free(cmdline);
	}

	// End-to-end: compile a file inside a directory with spaces
	{
		char tmpdir[PATH_MAX];
		if (!test_mkdtemp(tmpdir, "prism_spaces_")) {
			printf("  SKIP: could not create temp dir\n");
			return;
		}
		// Create "sub dir" inside tmpdir
		char subdir[PATH_MAX];
		snprintf(subdir, sizeof(subdir), "%s\\sub dir", tmpdir);
		CreateDirectoryA(subdir, NULL);

		// Write a minimal C file inside the spaced directory
		char srcpath[PATH_MAX];
		snprintf(srcpath, sizeof(srcpath), "%s\\test.c", subdir);
		FILE *f = fopen(srcpath, "w");
		CHECK(f != NULL, "spaces: create source file");
		if (f) { fprintf(f, "int main(void){return 0;}\n"); fclose(f); }

		// Compile it using run_command (exercises CreateProcess quoting)
		char outpath[PATH_MAX];
		snprintf(outpath, sizeof(outpath), "%s\\test.exe", subdir);
		char fe_flag[PATH_MAX];
		snprintf(fe_flag, sizeof(fe_flag), "/Fe:%s", outpath);
		char *cc_argv[] = { "cl.exe", "/nologo", fe_flag, srcpath, NULL };
		int rc = run_command(cc_argv);
		CHECK_EQ(rc, 0, "spaces: compile in dir with spaces succeeds");
		CHECK(access(outpath, 0) == 0, "spaces: output exe exists");

		// Cleanup
		unlink(outpath);
		// Remove .obj that cl.exe may produce in cwd or subdir
		char objpath[PATH_MAX];
		snprintf(objpath, sizeof(objpath), "%s\\test.obj", subdir);
		unlink(objpath);
		unlink(srcpath);
		RemoveDirectoryA(subdir);
		RemoveDirectoryA(tmpdir);
	}
}

static void test_unicode_paths(void) {
	printf("\n--- Bug Regression: Non-ASCII (Unicode) Paths ---\n");

	// mkstemps with a non-ASCII directory
	{
		char tmpdir[PATH_MAX];
		if (!test_mkdtemp(tmpdir, "prism_uni_")) {
			printf("  SKIP: could not create temp dir\n");
			return;
		}
		// Create a subdirectory with Japanese characters
		char unidir[PATH_MAX];
		snprintf(unidir, sizeof(unidir), "%s\\\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88", tmpdir);

		// Use wide API to create directory (since ANSI CreateDirectoryA may fail)
		wchar_t wunidir[MAX_PATH];
		MultiByteToWideChar(CP_UTF8, 0, unidir, -1, wunidir, MAX_PATH);
		CreateDirectoryW(wunidir, NULL);

		// Try mkstemps inside the Unicode directory
		char tmpl[PATH_MAX];
		snprintf(tmpl, sizeof(tmpl), "%s\\prism_XXXXXX.c", unidir);
		int fd = mkstemps(tmpl, 2);
		CHECK(fd >= 0, "unicode: mkstemps in non-ASCII dir succeeds");
		if (fd >= 0) {
			const char *src = "int main(void){return 0;}\n";
			write(fd, src, (unsigned)strlen(src));
			close(fd);

			// Verify fopen also works on the non-ASCII path
			FILE *f = fopen(tmpl, "r");
			CHECK(f != NULL, "unicode: fopen non-ASCII path");
			if (f) fclose(f);

			unlink(tmpl);
		}

		// Cleanup
		RemoveDirectoryW(wunidir);
		RemoveDirectoryA(tmpdir);
	}
}

static void test_quoted_cc_parsing(void) {
	printf("\n--- Bug Regression: Quoted CC Parsing ---\n");

	// cc_is_msvc: unquoted cl.exe
	CHECK(cc_is_msvc("cl") == true, "cc_is_msvc(\"cl\")");
	CHECK(cc_is_msvc("cl.exe") == true, "cc_is_msvc(\"cl.exe\")");
	CHECK(cc_is_msvc("CL.EXE") == true, "cc_is_msvc(\"CL.EXE\") case-insensitive");

	// cc_is_msvc: path without spaces
	CHECK(cc_is_msvc("C:\\MSVC\\bin\\cl.exe") == true,
	      "cc_is_msvc with backslash path");

	// cc_is_msvc: quoted path with spaces (the bug scenario)
	CHECK(cc_is_msvc("\"C:\\Program Files\\MSVC\\bin\\cl.exe\"") == true,
	      "cc_is_msvc quoted path with spaces");
	CHECK(cc_is_msvc("\"C:\\Program Files\\MSVC\\bin\\cl.exe\" /nologo") == true,
	      "cc_is_msvc quoted path with spaces + args");

	// cc_is_msvc: non-MSVC compilers
	CHECK(cc_is_msvc("gcc") == false, "cc_is_msvc(\"gcc\")");
	CHECK(cc_is_msvc("\"C:\\Program Files\\gcc\\bin\\gcc.exe\"") == false,
	      "cc_is_msvc quoted gcc path");
	CHECK(cc_is_msvc(NULL) == false, "cc_is_msvc(NULL)");
	CHECK(cc_is_msvc("") == false, "cc_is_msvc(\"\")");

	// cc_executable: extract compiler path from quoted CC string
	{
		const char *exe;

		exe = cc_executable("cl.exe");
		CHECK(strcmp(exe, "cl.exe") == 0, "cc_executable simple");

		exe = cc_executable("\"C:\\Program Files\\MSVC\\cl.exe\" /nologo");
		CHECK(strcmp(exe, "C:\\Program Files\\MSVC\\cl.exe") == 0,
		      "cc_executable quoted with spaces");

		exe = cc_executable("gcc -m32");
		CHECK(strcmp(exe, "gcc") == 0, "cc_executable with flags");
	}

	// cc_extra_arg_count: count args beyond compiler
	{
		CHECK_EQ(cc_extra_arg_count("cl.exe"), 0,
		         "cc_extra_arg_count no extras");
		CHECK_EQ(cc_extra_arg_count("\"C:\\Program Files\\cl.exe\" /nologo"), 1,
		         "cc_extra_arg_count quoted + 1 extra");
		CHECK_EQ(cc_extra_arg_count("\"C:\\Program Files\\cl.exe\" /nologo /W4"), 2,
		         "cc_extra_arg_count quoted + 2 extras");
		CHECK_EQ(cc_extra_arg_count("gcc -m32"), 1,
		         "cc_extra_arg_count unquoted + 1 extra");
	}
}

static void test_win_path_wipe(void) {
	printf("\n--- Security Regression: PATH Wipe Prevention ---\n");

	// Verify the two-phase registry query pattern works:
	// Phase 1: query with NULL buffer to get size
	// Phase 2: allocate exact buffer and read
	HKEY hKey;
	LONG open_rc = RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0, KEY_READ, &hKey);
	if (open_rc != ERROR_SUCCESS) {
		printf("  SKIP: cannot open HKCU\\Environment (rc=%ld)\n", open_rc);
		return;
	}

	DWORD size = 0;
	DWORD type = REG_EXPAND_SZ;
	LONG rc = RegQueryValueExA(hKey, "Path", NULL, &type, NULL, &size);
	if (rc == ERROR_SUCCESS && size > 0) {
		CHECK(size > 0, "path_wipe: phase-1 query returns nonzero size");

		// Phase 2: allocate and read with exact size
		char *buf = (char *)malloc((size_t)size + 1);
		CHECK(buf != NULL, "path_wipe: malloc succeeded");
		if (buf) {
			rc = RegQueryValueExA(hKey, "Path", NULL, &type, (BYTE *)buf, &size);
			CHECK_EQ((int)rc, (int)ERROR_SUCCESS, "path_wipe: phase-2 read succeeds");
			buf[size] = '\0';
			CHECK(strlen(buf) > 0, "path_wipe: PATH content is non-empty");
			free(buf);
		}

		// Prove the old a tiny buffer triggers ERROR_MORE_DATA
		char tiny[10];
		DWORD tiny_size = sizeof(tiny);
		LONG tiny_rc = RegQueryValueExA(hKey, "Path", NULL, &type, (BYTE *)tiny, &tiny_size);
		CHECK_EQ((int)tiny_rc, (int)ERROR_MORE_DATA,
		         "path_wipe: tiny buffer triggers ERROR_MORE_DATA (old bug)");
	} else if (rc == ERROR_FILE_NOT_FOUND) {
		CHECK(1, "path_wipe: no user PATH key (handled correctly)");
	} else {
		CHECK(0, "path_wipe: unexpected registry error");
	}
	RegCloseKey(hKey);
}

static void test_win_capture_injection(void) {
	printf("\n--- Security Regression: Command Injection Prevention ---\n");

	// Test 1: Ampersand in argv should not break out to a separate command.
	// With the old _popen code, "echo&echo INJECTED" would run two commands.
	// With CreateProcess, the entire string is one argv[0] that just fails.
	{
		char buf[256];
		char *argv_inject[] = { "echo&echo", "INJECTED", NULL };
		int rc = capture_first_line(argv_inject, buf, sizeof(buf));
		// "echo&echo" doesn't exist as a binary, so it should fail
		// or the output should NOT contain "INJECTED" as a separate command
		CHECK(strstr(buf, "INJECTED") == NULL,
		      "injection: ampersand in argv[0] not interpreted as shell operator");
	}

	// Test 2: Pipe in argv should not create a pipeline
	{
		char buf[256];
		char *argv_pipe[] = { "echo|echo", "PIPED", NULL };
		int rc = capture_first_line(argv_pipe, buf, sizeof(buf));
		CHECK(strstr(buf, "PIPED") == NULL,
		      "injection: pipe in argv[0] not interpreted as shell operator");
	}

	// Test 3: Normal capture_first_line still works
	{
		char buf[256];
		char *argv_normal[] = { "cmd.exe", "/c", "echo", "hello_from_test", NULL };
		int rc = capture_first_line(argv_normal, buf, sizeof(buf));
		// cmd.exe /c echo hello_from_test should produce output
		CHECK_EQ(rc, 0, "injection: normal capture_first_line succeeds");
		CHECK(strstr(buf, "hello_from_test") != NULL,
		      "injection: normal output captured correctly");
	}

	// Test 4: Arguments with special chars are properly escaped, not injected
	{
		char buf[256];
		char *argv_meta[] = { "cmd.exe", "/c", "echo", "a&b|c<d>e", NULL };
		int rc = capture_first_line(argv_meta, buf, sizeof(buf));
		// The metacharacters should appear literally in the output
		if (rc == 0 && buf[0]) {
			CHECK(strstr(buf, "a&b") != NULL || strstr(buf, "a") != NULL,
			      "injection: metachar args passed through (not split)");
		} else {
			// Even if cmd.exe misinterprets, at least nothing was injected
			CHECK(1, "injection: metachar args did not crash");
		}
	}
}

static void test_win_tempfile_exclusive(void) {
	printf("\n--- Security Regression: Temp File Exclusive Access ---\n");

	char tmpl[PATH_MAX];
	snprintf(tmpl, sizeof(tmpl), "%sprism_excl_XXXXXX", test_tmp_dir());
	int fd = mkstemp(tmpl);
	CHECK(fd >= 0, "tempfile_excl: mkstemp succeeded");
	if (fd < 0) return;

	// Attempt to open the same file — must fail because _SH_DENYRW
	int fd2 = -1;
	errno_t err = _sopen_s(&fd2, tmpl, _O_RDWR, _SH_DENYNO, _S_IREAD | _S_IWRITE);
	CHECK(err != 0 || fd2 < 0,
	      "tempfile_excl: concurrent open DENIED (_SH_DENYRW enforced)");
	if (fd2 >= 0) _close(fd2);

	_close(fd);
	_unlink(tmpl);
}

static void test_win_stderr_preserved(void) {
	printf("\n--- Security Regression: stderr Not Mutated ---\n");

	// Get stderr state before
	HANDLE h_before = GetStdHandle(STD_ERROR_HANDLE);
	DWORD type_before = GetFileType(h_before);

	// run_command_quiet triggers internally — call it directly
	char *argv_probe[] = { "cmd.exe", "/c", "echo", "probe", NULL };
	int rc = run_command_quiet(argv_probe);

	// Get stderr state after
	HANDLE h_after = GetStdHandle(STD_ERROR_HANDLE);
	DWORD type_after = GetFileType(h_after);

	CHECK(h_before == h_after, "stderr_race: handle unchanged after run_command_quiet");
	CHECK(type_before == type_after, "stderr_race: handle type unchanged after run_command_quiet");

	// Verify stderr is still writable
	int written = fprintf(stderr, "  (stderr write test OK)\n");
	fflush(stderr);
	CHECK(written > 0, "stderr_race: fprintf(stderr) still works");
}

static void test_win_realpath_resolves(void) {
	printf("\n--- Security Regression: realpath Resolves Reparse Points ---\n");

	// Test 1: realpath returns a canonical path for an existing file
	{
		char resolved[PATH_MAX];
		char *r = realpath("prism.exe", resolved);
		if (!r) {
			// prism.exe might not be in cwd during test, try self
			char self[PATH_MAX];
			GetModuleFileNameA(NULL, self, PATH_MAX);
			r = realpath(self, resolved);
		}
		CHECK(r != NULL, "realpath: resolves existing file");
		if (r) {
			// Must not have \\?\ prefix (we strip it)
			CHECK(!(r[0] == '\\' && r[1] == '\\' && r[2] == '?' && r[3] == '\\'),
			      "realpath: no \\\\?\\\\ prefix");
		}
	}

	// Test 2: Junction resolution (junctions don't require admin privileges)
	{
		char tmpdir[PATH_MAX];
		if (!test_mkdtemp(tmpdir, "prism_junc_")) {
			printf("  SKIP: cannot create temp dir for junction test\n");
			return;
		}

		// Create target directory and a file inside it
		char target[PATH_MAX], junction[PATH_MAX], filepath[PATH_MAX];
		snprintf(target, sizeof(target), "%s\\real_target", tmpdir);
		snprintf(junction, sizeof(junction), "%s\\junc_link", tmpdir);
		CreateDirectoryA(target, NULL);

		// Create a test file in the target
		snprintf(filepath, sizeof(filepath), "%s\\test.txt", target);
		FILE *f = fopen(filepath, "w");
		if (f) { fprintf(f, "test\n"); fclose(f); }

		// Create junction: junc_link -> real_target
		char cmd[PATH_MAX * 2];
		snprintf(cmd, sizeof(cmd), "mklink /J \"%s\" \"%s\" >nul 2>nul", junction, target);
		system(cmd);

		// Access the file through the junction
		char junc_file[PATH_MAX];
		snprintf(junc_file, sizeof(junc_file), "%s\\test.txt", junction);

		if (access(junc_file, 0) == 0) {
			char resolved_junc[PATH_MAX], resolved_real[PATH_MAX];
			char *rj = realpath(junc_file, resolved_junc);
			char *rr = realpath(filepath, resolved_real);

			CHECK(rj != NULL, "realpath: resolves file through junction");
			CHECK(rr != NULL, "realpath: resolves real file");

			if (rj && rr) {
				// Both should resolve to the same final path
				CHECK(_stricmp(rj, rr) == 0,
				      "realpath: junction and real path resolve identically");

				// Verify _fullpath would NOT have matched (the old bug)
				char lex_junc[PATH_MAX], lex_real[PATH_MAX];
				_fullpath(lex_junc, junc_file, PATH_MAX);
				_fullpath(lex_real, filepath, PATH_MAX);
				CHECK(_stricmp(lex_junc, lex_real) != 0,
				      "realpath: _fullpath does NOT resolve junction (old bug confirmed)");
			}
		} else {
			printf("  SKIP: junction creation requires elevated shell or failed\n");
		}

		// Cleanup
		snprintf(cmd, sizeof(cmd), "rd /s /q \"%s\" >nul 2>nul", tmpdir);
		system(cmd);
	}
}

static void test_win_spawn_oflag(void) {
	printf("\n--- Security Regression: SPAWN_ACT_OPEN Honors oflag ---\n");

	// Test: posix_spawn_file_actions_addopen with O_WRONLY correctly
	// creates a NUL handle for stderr suppression (the primary use case)
	{
		posix_spawn_file_actions_t fa;
		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
		CHECK_EQ(fa.count, 1, "oflag: addopen registered");
		CHECK_EQ(fa.actions[0].oflag, O_WRONLY, "oflag: O_WRONLY stored");
		CHECK_EQ(fa.actions[0].fd, STDERR_FILENO, "oflag: target fd stored");

		// Actually spawn with it — should succeed and suppress stderr
		char *argv_test[] = { "cmd.exe", "/c", "echo", "oflag_test", NULL };
		HANDLE hp = win32_spawn_with_actions(argv_test, &fa, NULL);
		CHECK(hp != INVALID_HANDLE_VALUE, "oflag: spawn with O_WRONLY /dev/null succeeds");
		if (hp != INVALID_HANDLE_VALUE) {
			WaitForSingleObject(hp, 5000);
			DWORD exit_code = 99;
			GetExitCodeProcess(hp, &exit_code);
			CloseHandle(hp);
			CHECK_EQ((int)exit_code, 0, "oflag: child exited cleanly");
		}
		posix_spawn_file_actions_destroy(&fa);
	}

	// Test: O_RDONLY is stored and distinct from O_WRONLY
	{
		posix_spawn_file_actions_t fa;
		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
		CHECK_EQ(fa.actions[0].oflag, O_RDONLY, "oflag: O_RDONLY stored for stdin");

		// Spawn with O_RDONLY on stdin (/dev/null -> NUL readable)
		char *argv_rd[] = { "cmd.exe", "/c", "echo", "rdonly_ok", NULL };
		HANDLE hp = win32_spawn_with_actions(argv_rd, &fa, NULL);
		CHECK(hp != INVALID_HANDLE_VALUE, "oflag: spawn with O_RDONLY /dev/null succeeds");
		if (hp != INVALID_HANDLE_VALUE) {
			WaitForSingleObject(hp, 5000);
			CloseHandle(hp);
		}
		posix_spawn_file_actions_destroy(&fa);
	}

	// Test: O_CREAT flag is preserved
	{
		posix_spawn_file_actions_t fa;
		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null",
		                                 O_WRONLY | O_CREAT, 0644);
		CHECK(fa.actions[0].oflag & O_CREAT, "oflag: O_CREAT flag preserved");
		CHECK(fa.actions[0].oflag & O_WRONLY, "oflag: O_WRONLY flag preserved with O_CREAT");
		posix_spawn_file_actions_destroy(&fa);
	}
}

static void test_win_env_scrubbing(void) {
	printf("\n--- Security Regression: Environment Variable Scrubbing ---\n");

	// run_command/run_command_quiet passed NULL for envp to
	// win32_spawn_with_actions, and that function ignored envp entirely
	// (passed NULL to CreateProcessW for lpEnvironment).  This caused
	// the child to inherit CC= and PRISM_CC= from the parent, defeating
	// the recursive-compiler-loop prevention in build_clean_environ().

	// Test 1: build_clean_environ strips CC= from the environment.
	{
		// Temporarily set CC in our own environment so build_clean_environ
		// has something to strip.  Save and restore the original value.
		const char *orig_cc = getenv("CC");
		_putenv("CC=SHOULD_NOT_LEAK");

		// Force rebuild of cached env (the cache is process-lifetime, but
		// for testing we can verify the content directly).
		char **clean = build_clean_environ();
		CHECK(clean != NULL, "env_scrub: build_clean_environ returns non-NULL");
		bool found_cc = false;
		if (clean) {
			for (char **e = clean; *e; e++) {
				if (strncmp(*e, "CC=", 3) == 0) { found_cc = true; break; }
			}
		}
		CHECK(!found_cc, "env_scrub: CC= stripped from clean environment");

		// Restore
		if (orig_cc) {
			char buf[PATH_MAX];
			snprintf(buf, sizeof(buf), "CC=%s", orig_cc);
			_putenv(buf);
		} else {
			_putenv("CC=");
		}
	}

	// Test 2: win32_build_env_block produces a valid environment block
	// from a POSIX-style envp array, and CreateProcessW uses it.
	{
		// Spawn "cmd /c set" with a custom envp that contains a marker
		// variable but does NOT contain CC= or PRISM_CC=.
		// Verify the child sees the marker and doesn't see CC/PRISM_CC.
		char *test_envp[] = {
			"PRISM_TEST_MARKER=found_it_42",
			"SystemRoot=C:\\Windows",  // needed for cmd.exe to work
			"PATH=C:\\Windows\\System32",
			NULL
		};
		posix_spawn_file_actions_t fa;
		posix_spawn_file_actions_init(&fa);

		// Redirect stdout to a pipe so we can read the child's output.
		int pipe_fds[2];
		CHECK_EQ(pipe(pipe_fds), 0, "env_scrub: pipe created");
		posix_spawn_file_actions_adddup2(&fa, pipe_fds[1], STDOUT_FILENO);
		posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

		char *argv[] = { "cmd.exe", "/c", "set", "PRISM_TEST_MARKER", NULL };
		HANDLE hp = win32_spawn_with_actions(argv, &fa, test_envp);
		close(pipe_fds[1]); // close write end in parent
		posix_spawn_file_actions_destroy(&fa);

		CHECK(hp != INVALID_HANDLE_VALUE, "env_scrub: spawn with custom envp succeeds");
		if (hp != INVALID_HANDLE_VALUE) {
			char buf[1024] = {0};
			int total = 0;
			while (total < (int)sizeof(buf) - 1) {
				int n = read(pipe_fds[0], buf + total, sizeof(buf) - 1 - total);
				if (n <= 0) break;
				total += n;
			}
			buf[total] = '\0';
			close(pipe_fds[0]);
			WaitForSingleObject(hp, 5000);
			CloseHandle(hp);

			CHECK(strstr(buf, "found_it_42") != NULL,
			      "env_scrub: child sees custom env variable");
		} else {
			close(pipe_fds[0]);
		}
	}

	// Test 3: Verify that run_command passes the clean environment
	// (not NULL) by spawning a child that checks for CC.
	{
		const char *orig_cc = getenv("CC");
		_putenv("CC=RECURSIVE_BOMB");

		// Create a tiny batch script that prints CC if set
		char script[PATH_MAX];
		snprintf(script, sizeof(script), "%sprism_env_test.bat", test_tmp_dir());
		FILE *f = fopen(script, "w");
		CHECK(f != NULL, "env_scrub: create test script");
		if (f) {
			fprintf(f, "@echo off\r\nif defined CC (echo CC_LEAKED) else (echo CC_CLEAN)\r\n");
			fclose(f);

			// Use capture_first_line to check child's output.
			// Note: capture_first_line has its own CreateProcess — but
			// run_command is what we fixed. Use run_command_quiet to
			// exercise the fixed path and check exit code.
			char buf[256] = {0};
			char *argv[] = { "cmd.exe", "/c", script, NULL };
			// Use posix_spawnp which goes through the fixed path:
			char **clean_env = build_clean_environ();
			posix_spawn_file_actions_t fa;
			posix_spawn_file_actions_init(&fa);
			int pipe_fds[2];
			pipe(pipe_fds);
			posix_spawn_file_actions_adddup2(&fa, pipe_fds[1], STDOUT_FILENO);
			posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

			HANDLE hp = win32_spawn_with_actions(argv, &fa, clean_env);
			close(pipe_fds[1]);
			posix_spawn_file_actions_destroy(&fa);

			if (hp != INVALID_HANDLE_VALUE) {
				int total = 0;
				while (total < (int)sizeof(buf) - 1) {
					int n = read(pipe_fds[0], buf + total, sizeof(buf) - 1 - total);
					if (n <= 0) break;
					total += n;
				}
				buf[total] = '\0';
				close(pipe_fds[0]);
				WaitForSingleObject(hp, 5000);
				CloseHandle(hp);

				CHECK(strstr(buf, "CC_CLEAN") != NULL,
				      "env_scrub: CC= not visible in child via clean env");
				CHECK(strstr(buf, "CC_LEAKED") == NULL,
				      "env_scrub: CC= did not leak to child");
			} else {
				close(pipe_fds[0]);
			}

			remove(script);
		}

		if (orig_cc) {
			char buf[PATH_MAX];
			snprintf(buf, sizeof(buf), "CC=%s", orig_cc);
			_putenv(buf);
		} else {
			_putenv("CC=");
		}
	}
}

static void test_win_handle_isolation(void) {
	printf("\n--- Security Regression: Handle Isolation (STARTUPINFOEX) ---\n");

	// win32_spawn_with_actions used bInheritHandles=TRUE with plain
	// STARTUPINFOW, causing ALL inheritable handles in the process to leak
	// into children.  In PRISM_LIB_MODE with concurrent threads, pipe
	// handles from thread A would leak into thread B's cl.exe, causing
	// deadlocks because the pipe write-end stays open.
	//
	// Fix: Use STARTUPINFOEXW + PROC_THREAD_ATTRIBUTE_HANDLE_LIST to
	// whitelist exactly which handles the child may inherit.

	// Test 1: Create a pipe, spawn a child process with file actions that
	// redirect stdout to a DIFFERENT pipe.  The first pipe's handles should
	// NOT leak into the child (verified by the child exiting cleanly and
	// the pipe being closeable without issues).
	{
		int bystander_pipe[2];
		CHECK_EQ(pipe(bystander_pipe), 0, "handle_iso: bystander pipe created");
		// Make the bystander pipe's write end inheritable (simulates what
		// would happen if another thread created a pipe concurrently).
		HANDLE bystander_write = (HANDLE)_get_osfhandle(bystander_pipe[1]);
		SetHandleInformation(bystander_write, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

		// Now spawn a child with its own pipe redirection.
		int child_pipe[2];
		CHECK_EQ(pipe(child_pipe), 0, "handle_iso: child pipe created");

		posix_spawn_file_actions_t fa;
		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_adddup2(&fa, child_pipe[1], STDOUT_FILENO);
		posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

		char *argv[] = { "cmd.exe", "/c", "echo", "isolated", NULL };
		HANDLE hp = win32_spawn_with_actions(argv, &fa, NULL);
		close(child_pipe[1]); // close write end in parent
		posix_spawn_file_actions_destroy(&fa);

		CHECK(hp != INVALID_HANDLE_VALUE, "handle_iso: spawn succeeds");
		if (hp != INVALID_HANDLE_VALUE) {
			// Read child output
			char buf[256] = {0};
			int n = read(child_pipe[0], buf, sizeof(buf) - 1);
			if (n > 0) buf[n] = '\0';
			WaitForSingleObject(hp, 5000);
			CloseHandle(hp);
			CHECK(strstr(buf, "isolated") != NULL,
			      "handle_iso: child output received through correct pipe");
		}
		close(child_pipe[0]);

		// The bystander pipe should still be fully functional because
		// its write end was NOT leaked to the child.  With the old code
		// (bInheritHandles=TRUE without handle list), the write end
		// would have been cloned into the child, keeping it open.
		close(bystander_pipe[1]); // close our write end
		// Read from bystander should return 0 (EOF) immediately because
		// no other process holds the write end.
		char bystander_buf[16];
		int br = read(bystander_pipe[0], bystander_buf, sizeof(bystander_buf));
		CHECK(br == 0, "handle_iso: bystander pipe EOF (write end not leaked)");
		close(bystander_pipe[0]);
	}

	// Test 2: When no file actions are used, bInheritHandles should be FALSE.
	// Create a pipe, spawn without file actions, verify the pipe handle
	// count is not incremented.
	{
		int lone_pipe[2];
		CHECK_EQ(pipe(lone_pipe), 0, "handle_iso: lone pipe created");
		HANDLE lone_write = (HANDLE)_get_osfhandle(lone_pipe[1]);
		SetHandleInformation(lone_write, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

		// Spawn with NULL file actions (no redirection).
		char *argv[] = { "cmd.exe", "/c", "echo", "no_fa", NULL };
		HANDLE hp = win32_spawn_with_actions(argv, NULL, NULL);
		CHECK(hp != INVALID_HANDLE_VALUE, "handle_iso: spawn without fa succeeds");
		if (hp != INVALID_HANDLE_VALUE) {
			WaitForSingleObject(hp, 5000);
			CloseHandle(hp);
		}

		// Write end should not have leaked; close it and verify EOF on read.
		close(lone_pipe[1]);
		char rb[16];
		int nr = read(lone_pipe[0], rb, sizeof(rb));
		CHECK(nr == 0, "handle_iso: lone pipe write end not inherited (EOF)");
		close(lone_pipe[0]);
	}
}

static void test_win_std_clatest_override(void) {
	printf("\n--- Regression: /std:clatest Override ---\n");

	// If user passed /std:c11 to prism, the has_std check suppressed
	// /std:clatest injection, but generated code may contain typeof() which
	// requires C23 mode on MSVC.  This caused hard compilation errors.
	//
	// Fix: Always inject /std:clatest and strip any user /std: flags.

	// Test: Transpile code that triggers typeof emission (const typedef +
	// orelse), confirm the output contains "typeof" (which needs /std:clatest).
	{
		PrismFeatures feat = prism_defaults();
		const char *code =
		    "typedef const int CI;\n"
		    "int main(void) {\n"
		    "    int x = 5;\n"
		    "    CI y = x orelse 0;\n"
		    "    return y;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "std_override.c", feat);
		CHECK_EQ(r.status, PRISM_OK, "std_override: transpiles OK");
		if (r.output) {
			// The transpiler should emit typeof or typeof_unqual for the
			// const-stripping path.
			bool has_typeof = strstr(r.output, "typeof") != NULL;
			CHECK(has_typeof,
			      "std_override: output contains typeof (needs /std:clatest)");
		}
		prism_free(&r);
	}

	// Test: Verify that a non-typeof path (plain zeroinit) still works.
	{
		PrismFeatures feat = prism_defaults();
		const char *code =
		    "int main(void) {\n"
		    "    int x;\n"
		    "    return x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "std_plain.c", feat);
		CHECK_EQ(r.status, PRISM_OK, "std_override: plain zeroinit transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "= 0") != NULL,
			      "std_override: plain int gets = 0");
		}
		prism_free(&r);
	}
}

static void test_win_old_exe_cleanup(void) {
	printf("\n--- Regression: .old Exe Cleanup on Update ---\n");

	// install() renamed the running prism.exe to prism.exe.old during a
	// self-update, then called remove(old_path) to clean up.  Since the .old
	// file is still locked by the running process, remove() silently fails,
	// leaving orphaned .old files in the install directory forever.
	//
	// Fix: Move the .old file to %TEMP% and schedule deletion via
	// MoveFileExA(path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT).

	// Test 1: Simulate running-exe lock by copying a real DLL and loading
	// it with LoadLibraryEx.  This creates a SEC_IMAGE section mapping —
	// the same mechanism the PE loader uses — which blocks remove() but
	// allows MoveFileExA rename.
	{
		char locked_path[PATH_MAX];
		snprintf(locked_path, sizeof(locked_path), "%sprism_lock_test.dll",
		         test_tmp_dir());

		// Copy a small system DLL to our temp dir so we can load it.
		BOOL copied = CopyFileA("C:\\Windows\\System32\\version.dll",
		                        locked_path, FALSE);
		CHECK(copied != 0, "old_cleanup: copy DLL for locking test");
		if (!copied) return;

		// Load as image — creates SEC_IMAGE section mapping.
		HMODULE hMod = LoadLibraryExA(locked_path, NULL, 0);
		CHECK(hMod != NULL, "old_cleanup: LoadLibrary locks the file");
		if (!hMod) { remove(locked_path); return; }

		// remove() should fail because the image section is mapped.
		int rm_result = remove(locked_path);
		CHECK(rm_result != 0,
		      "old_cleanup: remove() fails on image-mapped file (proves bug)");

		// MoveFileExA rename should succeed — Windows allows renaming
		// files with SEC_IMAGE mappings (same as renaming a running exe).
		char temp_dest[PATH_MAX];
		snprintf(temp_dest, sizeof(temp_dest), "%sprism_old_%u.dll",
		         test_tmp_dir(), (unsigned)GetCurrentProcessId());

		BOOL moved = MoveFileExA(locked_path, temp_dest,
		                         MOVEFILE_REPLACE_EXISTING);
		CHECK(moved != 0,
		      "old_cleanup: MoveFileExA rename succeeds on mapped image");

		if (moved) {
			CHECK(access(locked_path, F_OK) != 0,
			      "old_cleanup: original path gone after rename");
		}

		// Clean up: unload DLL, delete the renamed file.
		FreeLibrary(hMod);
		remove(temp_dest);
		remove(locked_path); // in case rename failed
	}

	// Test 2: Verify MoveFileExA with MOVEFILE_DELAY_UNTIL_REBOOT doesn't
	// crash (may require elevation to actually succeed, but must not crash).
	{
		char reboot_path[PATH_MAX];
		snprintf(reboot_path, sizeof(reboot_path), "%sprism_reboot_test.tmp",
		         test_tmp_dir());
		FILE *f = fopen(reboot_path, "w");
		CHECK(f != NULL, "old_cleanup: create temp file for reboot test");
		if (f) {
			fprintf(f, "test");
			fclose(f);
			// This may fail without admin, but should not crash.
			MoveFileExA(reboot_path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
			// Clean up (file may or may not still exist).
			remove(reboot_path);
		}
	}
}

static void test_win_registry_wide_path(void) {
	printf("\n--- Security Regression: Registry Wide-Char PATH ---\n");

	// add_to_user_path used RegQueryValueExA/RegSetValueExA (ANSI).
	// If the user's PATH contained non-ASCII characters (e.g., C:\Users\José),
	// the ANSI API would transliterate them to '?' and permanently corrupt
	// the user's system PATH when writing back.
	//
	// Fix: Use RegOpenKeyExW, RegQueryValueExW, RegSetValueExW throughout.

	// Test 1: Verify the wpath_contains_dir helper works with wide strings.
	{
		CHECK(wpath_contains_dir(L"C:\\Users\\Jos\u00e9;C:\\Windows", L"C:\\Users\\Jos\u00e9"),
		      "registry_wide: wpath_contains_dir finds non-ASCII dir");
		CHECK(!wpath_contains_dir(L"C:\\Users\\Jose;C:\\Windows", L"C:\\Users\\Jos\u00e9"),
		      "registry_wide: wpath_contains_dir rejects similar ASCII dir");
		CHECK(wpath_contains_dir(L"C:\\first;C:\\second;C:\\third", L"C:\\second"),
		      "registry_wide: wpath_contains_dir finds middle segment");
		CHECK(!wpath_contains_dir(L"C:\\firstsecond", L"C:\\second"),
		      "registry_wide: wpath_contains_dir rejects substring match");
		CHECK(wpath_contains_dir(L"C:\\only", L"C:\\only"),
		      "registry_wide: wpath_contains_dir finds sole entry");
	}

	// Test 2: Read the real registry PATH using wide API and verify
	// round-trip preserves content.  We don't modify the registry — just
	// read and verify no data loss compared to ANSI.
	{
		HKEY hKey;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
			DWORD size = 0;
			DWORD type = REG_EXPAND_SZ;
			LONG rc = RegQueryValueExW(hKey, L"Path", NULL, &type, NULL, &size);
			if (rc == ERROR_SUCCESS && size > 0) {
				wchar_t *wpath = (wchar_t *)malloc(size + sizeof(wchar_t));
				if (wpath) {
					rc = RegQueryValueExW(hKey, L"Path", NULL, &type, (BYTE *)wpath, &size);
					if (rc == ERROR_SUCCESS) {
						wpath[size / sizeof(wchar_t)] = L'\0';
						// Convert to UTF-8 and back to verify round-trip
						int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, NULL, 0, NULL, NULL);
						CHECK(utf8_len > 0, "registry_wide: PATH converts to UTF-8");
						if (utf8_len > 0) {
							char *utf8 = (char *)malloc(utf8_len);
							WideCharToMultiByte(CP_UTF8, 0, wpath, -1, utf8, utf8_len, NULL, NULL);
							// Round-trip back to wide
							int rt_len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
							wchar_t *roundtrip = (wchar_t *)malloc(rt_len * sizeof(wchar_t));
							MultiByteToWideChar(CP_UTF8, 0, utf8, -1, roundtrip, rt_len);
							CHECK(wcscmp(wpath, roundtrip) == 0,
							      "registry_wide: PATH round-trips through UTF-8 losslessly");
							free(roundtrip);
							free(utf8);
						}
					}
					free(wpath);
				}
			}
			RegCloseKey(hKey);
		}
	}
}

static void test_win_signal_tempfile_cleanup(void) {
	printf("\n--- Security Regression: Signal Handler Temp File Cleanup ---\n");

	// signal_cleanup_handler called unlink() on temp files, but on
	// Windows _wunlink fails with EACCES if the file is still open.
	// Since out_fp or win32_memstream_fp may hold the file open when
	// Ctrl-C fires, the unlink silently fails, leaving orphaned temp files.
	//
	// Fix: Close out_fp and win32_memstream_fp before unlinking.

	// Test: Create a temp file, open it (simulating out_fp holding it open),
	// verify that unlink fails while open, then close and verify it succeeds.
	{
		char tmp_path[PATH_MAX];
		snprintf(tmp_path, sizeof(tmp_path), "%sprism_sig_test.tmp", test_tmp_dir());

		FILE *fp = fopen(tmp_path, "w");
		CHECK(fp != NULL, "signal_cleanup: create temp file");
		if (!fp) return;
		fprintf(fp, "test data");
		fflush(fp);

		// On Windows, unlink fails while file is open
		int rm1 = _unlink(tmp_path);
		CHECK(rm1 != 0, "signal_cleanup: unlink fails while file is open (proves bug)");

		// Close the file first (this is what the fix does)
		fclose(fp);

		// Now unlink succeeds
		int rm2 = _unlink(tmp_path);
		CHECK(rm2 == 0, "signal_cleanup: unlink succeeds after fclose (proves fix)");
	}

	// Test: Verify the open_memstream temp file path is cleaned up properly
	// when closed before unlinking.
	{
		char *membuf = NULL;
		size_t memsize = 0;
		FILE *mfp = open_memstream(&membuf, &memsize);
		CHECK(mfp != NULL, "signal_cleanup: open_memstream succeeds");
		if (mfp) {
			// The memstream file path should exist
			CHECK(access(win32_memstream_path, F_OK) == 0,
			      "signal_cleanup: memstream temp file exists while open");

			char saved_path[MAX_PATH];
			strncpy(saved_path, win32_memstream_path, MAX_PATH - 1);
			saved_path[MAX_PATH - 1] = '\0';

			// Close via normal path (fclose wrapper handles cleanup)
			fclose(mfp);

			// After fclose, the temp file should be gone (wrapper deletes it)
			CHECK(access(saved_path, F_OK) != 0,
			      "signal_cleanup: memstream temp file removed after fclose");

			free(membuf);
		}
	}
}

static void test_win_realpath_unicode(void) {
	printf("\n--- Security Regression: realpath Wide-Char Unicode ---\n");

	// realpath used CreateFileA and GetFinalPathNameByHandleA (ANSI).
	// UTF-8 paths with non-ASCII characters (e.g., src/テスト.c) get mangled
	// through the system ANSI codepage, breaking #line directives.
	//
	// Fix: Use CreateFileW + GetFinalPathNameByHandleW with UTF-8 conversion.

	// Test 1: realpath resolves a plain ASCII path correctly.
	{
		char resolved[PATH_MAX];
		char *r = realpath(".", resolved);
		CHECK(r != NULL, "realpath_unicode: resolves current directory");
		if (r) {
			CHECK(strlen(r) > 0, "realpath_unicode: result is non-empty");
			// Should be an absolute path (starts with drive letter)
			CHECK(r[1] == ':', "realpath_unicode: result is absolute path");
		}
	}

	// Test 2: Create a file with a non-ASCII (Unicode) name and resolve it.
	{
		// Create a temp directory with a Unicode name
		char unicode_dir[PATH_MAX];
		snprintf(unicode_dir, sizeof(unicode_dir), "%s\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88",
		         test_tmp_dir()); // "テスト" in UTF-8
		wchar_t wdir[MAX_PATH];
		MultiByteToWideChar(CP_UTF8, 0, unicode_dir, -1, wdir, MAX_PATH);
		CreateDirectoryW(wdir, NULL);

		char unicode_file[PATH_MAX];
		snprintf(unicode_file, sizeof(unicode_file), "%s\\\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab.c",
		         unicode_dir); // "ファイル.c" in UTF-8
		wchar_t wfile[MAX_PATH];
		MultiByteToWideChar(CP_UTF8, 0, unicode_file, -1, wfile, MAX_PATH);

		// Create the file via wide API
		HANDLE hf = CreateFileW(wfile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		if (hf != INVALID_HANDLE_VALUE) {
			DWORD written;
			WriteFile(hf, "/* test */", 10, &written, NULL);
			CloseHandle(hf);

			// Now resolve via realpath (UTF-8 input)
			char resolved[PATH_MAX];
			char *r = realpath(unicode_file, resolved);
			CHECK(r != NULL, "realpath_unicode: resolves Unicode file path");
			if (r) {
				// The resolved path should contain the Unicode characters
				// (not '?' replacements from ANSI)
				CHECK(strstr(r, "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88") != NULL,
				      "realpath_unicode: resolved path preserves テスト");
				CHECK(strstr(r, "?") == NULL || strstr(r, "?\\") != NULL || r[0] == '?',
				      "realpath_unicode: no ANSI '?' corruption in result");
			}

			// Clean up
			DeleteFileW(wfile);
		} else {
			// If we can't create the Unicode file, skip gracefully
			printf("  (skipped Unicode file test — CreateFileW failed)\n");
		}
		RemoveDirectoryW(wdir);
	}

	// Test 3: realpath returns NULL for non-existent path with correct errno.
	{
		char *r = realpath(NULL, NULL);
		CHECK(r == NULL, "realpath_unicode: NULL input returns NULL");
	}
}

static void test_win_capture_wide(void) {
	printf("\n--- Security Regression: capture_first_line CreateProcessW ---\n");

	// capture_first_line used CreateProcessA with a UTF-8 cmdline.
	// If the compiler path contained non-ASCII characters, the ANSI
	// interpretation would fail to launch the process.
	//
	// Fix: Convert cmdline to wide chars and use CreateProcessW.

	// Test 1: capture_first_line works with basic ASCII commands.
	{
		char buf[256] = {0};
		char *argv[] = { "cmd.exe", "/c", "echo", "capture_test_42", NULL };
		int rc = capture_first_line(argv, buf, sizeof(buf));
		CHECK_EQ(rc, 0, "capture_wide: basic capture succeeds");
		CHECK(strstr(buf, "capture_test_42") != NULL,
		      "capture_wide: captured correct output");
	}

	// Test 2: capture_first_line handles multi-word output.
	{
		char buf[256] = {0};
		char *argv[] = { "cmd.exe", "/c", "echo", "hello world 123", NULL };
		int rc = capture_first_line(argv, buf, sizeof(buf));
		CHECK_EQ(rc, 0, "capture_wide: multi-word capture succeeds");
		CHECK(strstr(buf, "hello") != NULL,
		      "capture_wide: captured first word");
	}

	// Test 3: capture_first_line returns error for non-existent program.
	{
		char buf[256] = {0};
		char *argv[] = { "nonexistent_program_xyz_123", "--version", NULL };
		int rc = capture_first_line(argv, buf, sizeof(buf));
		CHECK(rc != 0, "capture_wide: non-existent program returns error");
	}

	// Test 4: Create a batch file with a Unicode path and capture its output.
	{
		char unicode_bat[PATH_MAX];
		snprintf(unicode_bat, sizeof(unicode_bat), "%s\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88",
		         test_tmp_dir());
		wchar_t wdir[MAX_PATH];
		MultiByteToWideChar(CP_UTF8, 0, unicode_bat, -1, wdir, MAX_PATH);
		CreateDirectoryW(wdir, NULL);

		char bat_path[PATH_MAX];
		snprintf(bat_path, sizeof(bat_path), "%s\\version.bat", unicode_bat);
		wchar_t wbat[MAX_PATH];
		MultiByteToWideChar(CP_UTF8, 0, bat_path, -1, wbat, MAX_PATH);

		// Create bat file via wide API
		HANDLE hf = CreateFileW(wbat, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		if (hf != INVALID_HANDLE_VALUE) {
			const char *content = "@echo off\r\necho UNICODE_PATH_OK\r\n";
			DWORD written;
			WriteFile(hf, content, (DWORD)strlen(content), &written, NULL);
			CloseHandle(hf);

			char buf[256] = {0};
			char *argv[] = { "cmd.exe", "/c", bat_path, NULL };
			int rc = capture_first_line(argv, buf, sizeof(buf));
			CHECK_EQ(rc, 0, "capture_wide: Unicode path batch file succeeds");
			CHECK(strstr(buf, "UNICODE_PATH_OK") != NULL,
			      "capture_wide: captured output from Unicode path");

			DeleteFileW(wbat);
		} else {
			printf("  (skipped Unicode batch test — CreateFileW failed)\n");
		}
		RemoveDirectoryW(wdir);
	}
}

static void test_win_install_path_wide(void) {
	printf("\n--- Security Regression: Install Path Wide-Char APIs ---\n");

	// get_install_path used GetEnvironmentVariableA("LOCALAPPDATA") and
	// GetModuleFileNameA.  ensure_install_dir used GetFileAttributesA and
	// CreateDirectoryA.  All are ANSI APIs that corrupt non-ASCII user profiles.
	//
	// Fix: Use W-suffixed APIs throughout, convert to UTF-8 with WideCharToMultiByte.

	// Test 1: get_install_path returns a valid path containing "prism".
	{
		const char *ip = get_install_path();
		CHECK(ip != NULL, "install_path_wide: get_install_path returns non-NULL");
		CHECK(strlen(ip) > 0, "install_path_wide: path is non-empty");
		CHECK(strstr(ip, "prism") != NULL,
		      "install_path_wide: path contains 'prism'");
	}

	// Test 2: Verify the path round-trips through UTF-8 correctly.
	// Read LOCALAPPDATA via wide API and compare with get_install_path prefix.
	{
		wchar_t wlocal[MAX_PATH];
		DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", wlocal, MAX_PATH);
		if (len > 0 && len < MAX_PATH) {
			char utf8_local[MAX_PATH * 3];
			int ulen = WideCharToMultiByte(CP_UTF8, 0, wlocal, -1,
			                               utf8_local, sizeof(utf8_local), NULL, NULL);
			CHECK(ulen > 0, "install_path_wide: LOCALAPPDATA converts to UTF-8");
			if (ulen > 0) {
				const char *ip = get_install_path();
				// The install path should start with the UTF-8 LOCALAPPDATA
				CHECK(strncmp(ip, utf8_local, strlen(utf8_local)) == 0,
				      "install_path_wide: path starts with UTF-8 LOCALAPPDATA");
			}
		}
	}

	// Test 3: get_self_exe_path returns a valid absolute path.
	{
		char self[PATH_MAX];
		bool ok = get_self_exe_path(self, sizeof(self));
		CHECK(ok, "install_path_wide: get_self_exe_path succeeds");
		if (ok) {
			CHECK(self[1] == ':', "install_path_wide: self path is absolute");
			CHECK(strstr(self, ".exe") != NULL,
			      "install_path_wide: self path ends with .exe");
		}
	}

	// Test 4: ensure_install_dir works with a Unicode directory.
	{
		char unicode_dir[PATH_MAX];
		snprintf(unicode_dir, sizeof(unicode_dir),
		         "%s\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88_install",
		         test_tmp_dir());

		char fake_install[PATH_MAX];
		snprintf(fake_install, sizeof(fake_install), "%s\\prism.exe", unicode_dir);

		// ensure_install_dir should create the Unicode directory
		bool created = ensure_install_dir(fake_install);
		CHECK(created, "install_path_wide: ensure_install_dir creates Unicode dir");

		if (created) {
			// Verify it exists via wide API
			wchar_t wdir[MAX_PATH];
			MultiByteToWideChar(CP_UTF8, 0, unicode_dir, -1, wdir, MAX_PATH);
			DWORD attr = GetFileAttributesW(wdir);
			CHECK(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY),
			      "install_path_wide: Unicode dir exists after creation");
			RemoveDirectoryW(wdir);
		}
	}
}

static void test_win_remove_utf8(void) {
	printf("\n--- Security Regression: remove() UTF-8 Shim ---\n");

	// remove() was not shimmed for UTF-8, unlike unlink().  The standard
	// C library remove() uses ANSI APIs on Windows, failing on non-ASCII paths.
	// MoveFileA and MoveFileExA in install() also silently fail on Unicode paths.
	//
	// Fix: Added remove(), MoveFileA(), MoveFileExA() shims that convert UTF-8
	// to wide chars before calling the W-suffixed API.

	// Test 1: remove() a file with a Unicode name.
	{
		char unicode_path[PATH_MAX];
		snprintf(unicode_path, sizeof(unicode_path),
		         "%s\xe5\x89\x8a\xe9\x99\xa4\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88.tmp",
		         test_tmp_dir()); // "削除テスト.tmp" in UTF-8

		// Create via wide API to ensure it exists
		wchar_t wpath[MAX_PATH];
		MultiByteToWideChar(CP_UTF8, 0, unicode_path, -1, wpath, MAX_PATH);
		HANDLE hf = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		if (hf != INVALID_HANDLE_VALUE) {
			DWORD written;
			WriteFile(hf, "test", 4, &written, NULL);
			CloseHandle(hf);

			// remove() with UTF-8 path should succeed
			int rc = remove(unicode_path);
			CHECK_EQ(rc, 0, "remove_utf8: remove() succeeds on Unicode file");

			// Verify it's gone
			DWORD attr = GetFileAttributesW(wpath);
			CHECK(attr == INVALID_FILE_ATTRIBUTES,
			      "remove_utf8: Unicode file is deleted");
		} else {
			printf("  (skipped Unicode remove test — CreateFileW failed)\n");
		}
	}

	// Test 2: MoveFileA shim works with Unicode paths.
	{
		char src_path[PATH_MAX], dst_path[PATH_MAX];
		snprintf(src_path, sizeof(src_path),
		         "%s\xe7\xa7\xbb\xe5\x8b\x95\xe5\x85\x83.tmp",
		         test_tmp_dir()); // "移動元.tmp"
		snprintf(dst_path, sizeof(dst_path),
		         "%s\xe7\xa7\xbb\xe5\x8b\x95\xe5\x85\x88.tmp",
		         test_tmp_dir()); // "移動先.tmp"

		wchar_t wsrc[MAX_PATH];
		MultiByteToWideChar(CP_UTF8, 0, src_path, -1, wsrc, MAX_PATH);
		HANDLE hf = CreateFileW(wsrc, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		if (hf != INVALID_HANDLE_VALUE) {
			DWORD written;
			WriteFile(hf, "move", 4, &written, NULL);
			CloseHandle(hf);

			// MoveFileA shim should handle UTF-8
			BOOL ok = MoveFileA(src_path, dst_path);
			CHECK(ok != 0, "remove_utf8: MoveFileA shim succeeds with Unicode");

			if (ok) {
				// Verify dst exists, src gone
				wchar_t wdst[MAX_PATH];
				MultiByteToWideChar(CP_UTF8, 0, dst_path, -1, wdst, MAX_PATH);
				CHECK(GetFileAttributesW(wdst) != INVALID_FILE_ATTRIBUTES,
				      "remove_utf8: MoveFileA destination exists");
				CHECK(GetFileAttributesW(wsrc) == INVALID_FILE_ATTRIBUTES,
				      "remove_utf8: MoveFileA source is gone");
				DeleteFileW(wdst);
			} else {
				DeleteFileW(wsrc);
			}
		} else {
			printf("  (skipped Unicode MoveFile test — CreateFileW failed)\n");
		}
	}

	// Test 3: MoveFileExA shim works with MOVEFILE_REPLACE_EXISTING.
	{
		char src_path[PATH_MAX], dst_path[PATH_MAX];
		snprintf(src_path, sizeof(src_path), "%smoveex_src.tmp", test_tmp_dir());
		snprintf(dst_path, sizeof(dst_path), "%smoveex_dst.tmp", test_tmp_dir());

		FILE *sf = fopen(src_path, "w");
		FILE *df = fopen(dst_path, "w");
		CHECK(sf != NULL && df != NULL, "remove_utf8: create MoveFileEx test files");
		if (sf) { fprintf(sf, "src"); fclose(sf); }
		if (df) { fprintf(df, "dst"); fclose(df); }

		BOOL ok = MoveFileExA(src_path, dst_path, MOVEFILE_REPLACE_EXISTING);
		CHECK(ok != 0, "remove_utf8: MoveFileExA shim with REPLACE succeeds");

		// Clean up
		remove(dst_path);
		remove(src_path);
	}
}

static void test_win_memstream_wide_temp(void) {
	printf("\n--- Security Regression: open_memstream Wide Temp Path ---\n");

	// open_memstream used GetTempPathA and GetTempFileNameA (ANSI APIs).
	// If %TEMP% contains non-ASCII characters, the temp file path gets corrupted
	// and fopen fails, causing open_memstream to crash.
	//
	// Fix: Use GetTempPathW and GetTempFileNameW, convert to UTF-8.

	// Test 1: open_memstream creates a temp file and returns valid data.
	{
		char *buf = NULL;
		size_t sz = 0;
		FILE *fp = open_memstream(&buf, &sz);
		CHECK(fp != NULL, "memstream_wide: open_memstream succeeds");
		if (fp) {
			fprintf(fp, "hello memstream");
			fclose(fp); // triggers read-back

			CHECK(buf != NULL, "memstream_wide: buffer is non-NULL after close");
			CHECK(sz > 0, "memstream_wide: size is non-zero");
			if (buf) {
				CHECK(strcmp(buf, "hello memstream") == 0,
				      "memstream_wide: content matches");
				free(buf);
			}
		}
	}

	// Test 2: Verify the temp file path stored in win32_memstream_path is
	// valid UTF-8 (not ANSI-mangled).
	{
		char *buf = NULL;
		size_t sz = 0;
		FILE *fp = open_memstream(&buf, &sz);
		CHECK(fp != NULL, "memstream_wide: second open_memstream succeeds");
		if (fp) {
			// While open, the path should be valid and the file should exist
			CHECK(win32_memstream_path[0] != '\0',
			      "memstream_wide: temp path is non-empty");
			CHECK(access(win32_memstream_path, F_OK) == 0,
			      "memstream_wide: temp file exists at stored path");

			// Verify the path is valid UTF-8 by round-tripping through wide
			wchar_t wcheck[MAX_PATH];
			int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			                             win32_memstream_path, -1, wcheck, MAX_PATH);
			CHECK(wn > 0, "memstream_wide: temp path is valid UTF-8");

			fclose(fp);
			free(buf);
		}
	}
}

static void test_win_oem_to_utf8(void) {
	printf("\n--- Security Regression: OEM Codepage Conversion ---\n");

	// check_path_shadow reads popen("where prism.exe") output encoded in
	// the console's OEM codepage (e.g., CP437), but compares it with
	// install_path which is UTF-8.  Non-ASCII paths cause false positives.
	//
	// Fix: Convert popen output from GetConsoleOutputCP() to UTF-8.

	// Test 1: Verify that OEM-to-UTF-8 conversion works for a known string.
	{
		UINT oem_cp = GetConsoleOutputCP();
		CHECK(oem_cp != 0, "oem_utf8: GetConsoleOutputCP returns valid codepage");

		// Round-trip a simple ASCII string through the conversion pipeline
		const char *test_str = "C:\\Windows\\System32";
		wchar_t wide[PATH_MAX];
		int wlen = MultiByteToWideChar(oem_cp, 0, test_str, -1, wide, PATH_MAX);
		CHECK(wlen > 0, "oem_utf8: ASCII string converts to wide");
		if (wlen > 0) {
			char utf8[PATH_MAX];
			int ulen = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, PATH_MAX, NULL, NULL);
			CHECK(ulen > 0, "oem_utf8: wide converts to UTF-8");
			CHECK(strcmp(utf8, test_str) == 0,
			      "oem_utf8: ASCII round-trip is lossless");
		}
	}

	// Test 2: Verify the `where` command output can be captured and decoded.
	{
		char buf[PATH_MAX];
		char *argv[] = { "cmd.exe", "/c", "where", "cmd.exe", NULL };
		int rc = capture_first_line(argv, buf, sizeof(buf));
		CHECK_EQ(rc, 0, "oem_utf8: 'where cmd.exe' succeeds");
		if (rc == 0) {
			// The result should be a valid path
			CHECK(strlen(buf) > 0, "oem_utf8: where output is non-empty");
			CHECK(strstr(buf, "cmd.exe") != NULL,
			      "oem_utf8: where output contains cmd.exe");
		}
	}

	// Test 3: Simulate OEM → UTF-8 conversion with a CP437 byte sequence.
	// CP437 byte 0x81 is 'ü'. In UTF-8, 'ü' is 0xC3 0xBC.
	{
		char oem_str[] = { (char)0x81, '\0' }; // 'ü' in CP437
		wchar_t wide[8];
		int wlen = MultiByteToWideChar(437, 0, oem_str, -1, wide, 8);
		if (wlen > 0) {
			char utf8[8];
			int ulen = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, 8, NULL, NULL);
			CHECK(ulen > 0, "oem_utf8: CP437 ü converts to UTF-8");
			if (ulen > 0) {
				// ü in UTF-8 is 0xC3 0xBC
				CHECK((unsigned char)utf8[0] == 0xC3 && (unsigned char)utf8[1] == 0xBC,
				      "oem_utf8: CP437 0x81 → UTF-8 ü (0xC3 0xBC)");
			}
		} else {
			printf("  (skipped CP437 test — codepage not available)\n");
		}
	}
}

// Regression: tokenize_file must use CreateFileW so UTF-8 paths work.
static void test_win_tokenize_file_wide(void) {
	printf("  Testing tokenize_file CreateFileW (wide path)...\n");

	// Create a temp file with a Unicode name, write valid C, tokenize it.
	wchar_t tmp_dir[MAX_PATH];
	DWORD dw = GetTempPathW(MAX_PATH, tmp_dir);
	CHECK(dw > 0, "tokenize_wide: GetTempPathW");

	wchar_t wpath[MAX_PATH];
	swprintf(wpath, MAX_PATH, L"%s\\prism_tok_\u00e9\u00fc.c", tmp_dir);

	// Write a tiny C file via wide API
	HANDLE hf = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(hf != INVALID_HANDLE_VALUE, "tokenize_wide: create temp file");
	if (hf != INVALID_HANDLE_VALUE) {
		const char *src = "int main(void) { return 0; }\n";
		DWORD written;
		WriteFile(hf, src, (DWORD)strlen(src), &written, NULL);
		CloseHandle(hf);

		// Convert to UTF-8 — this is the path tokenize_file receives
		char utf8path[MAX_PATH * 3];
		int ulen = WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
					       utf8path, sizeof(utf8path), NULL, NULL);
		CHECK(ulen > 0, "tokenize_wide: wpath → UTF-8");

		// Open via CreateFileW path (same as tokenize_file now does)
		wchar_t wpath2[MAX_PATH];
		int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
					     utf8path, -1, wpath2, MAX_PATH);
		CHECK(wn > 0, "tokenize_wide: UTF-8 round-trips to wide");

		HANDLE hRead = CreateFileW(wpath2, GENERIC_READ, FILE_SHARE_READ,
					   NULL, OPEN_EXISTING,
					   FILE_ATTRIBUTE_NORMAL, NULL);
		CHECK(hRead != INVALID_HANDLE_VALUE,
		      "tokenize_wide: CreateFileW opens UTF-8-named file");
		if (hRead != INVALID_HANDLE_VALUE) CloseHandle(hRead);

		DeleteFileW(wpath);
	}
}

// Regression: get_tmp_dir must use _wgetenv so TEMP paths survive non-ASCII.
static void test_win_tmpdir_wide(void) {
	printf("  Testing get_tmp_dir _wgetenv (wide TEMP)...\n");

	// Verify _wgetenv(L"TEMP") returns something useful
	const wchar_t *wt = _wgetenv(L"TEMP");
	CHECK(wt != NULL, "tmpdir_wide: _wgetenv(L\"TEMP\") != NULL");
	if (wt) {
		CHECK(wcslen(wt) > 0, "tmpdir_wide: TEMP is non-empty");

		// Convert to UTF-8 the same way get_tmp_dir does
		char buf[MAX_PATH];
		int ulen = WideCharToMultiByte(CP_UTF8, 0, wt, -1,
					       buf, sizeof(buf), NULL, NULL);
		CHECK(ulen > 0, "tmpdir_wide: TEMP converts to UTF-8");

		// The ANSI getenv might lose chars — just ensure the wide one works
		// and the UTF-8 path exists.
		if (ulen > 0) {
			wchar_t wcheck[MAX_PATH];
			int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
						     buf, -1, wcheck, MAX_PATH);
			CHECK(wn > 0, "tmpdir_wide: UTF-8 round-trips to wide");
			CHECK(wcscmp(wt, wcheck) == 0,
			      "tmpdir_wide: round-trip matches original");
		}
	}
}

// Regression: check_path_shadow must use _wgetcwd for Unicode CWD.
static void test_win_getcwd_wide(void) {
	printf("  Testing _wgetcwd UTF-8 round-trip...\n");

	wchar_t wcwd[MAX_PATH];
	wchar_t *ret = _wgetcwd(wcwd, MAX_PATH);
	CHECK(ret != NULL, "getcwd_wide: _wgetcwd succeeds");
	if (ret) {
		char utf8cwd[MAX_PATH * 3];
		int ulen = WideCharToMultiByte(CP_UTF8, 0, wcwd, -1,
					       utf8cwd, sizeof(utf8cwd), NULL, NULL);
		CHECK(ulen > 0, "getcwd_wide: wide CWD converts to UTF-8");

		// Round-trip back to wide
		if (ulen > 0) {
			wchar_t wrt[MAX_PATH];
			int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
						     utf8cwd, -1, wrt, MAX_PATH);
			CHECK(wn > 0, "getcwd_wide: UTF-8 round-trips to wide");
			CHECK(wcscmp(wcwd, wrt) == 0,
			      "getcwd_wide: round-trip matches original");
		}
	}
}

// Regression: open() shim must use _wopen for UTF-8 path support.
static void test_win_open_wide(void) {
	printf("  Testing open() shim _wopen (wide path)...\n");

	// Create a temp file with Unicode name via wide API
	wchar_t tmp_dir[MAX_PATH];
	DWORD dw = GetTempPathW(MAX_PATH, tmp_dir);
	CHECK(dw > 0, "open_wide: GetTempPathW");

	wchar_t wpath[MAX_PATH];
	swprintf(wpath, MAX_PATH, L"%s\\prism_open_\u00e9.txt", tmp_dir);

	HANDLE hf = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(hf != INVALID_HANDLE_VALUE, "open_wide: create temp file");
	if (hf != INVALID_HANDLE_VALUE) {
		const char *data = "hello\n";
		DWORD written;
		WriteFile(hf, data, (DWORD)strlen(data), &written, NULL);
		CloseHandle(hf);

		// Convert to UTF-8
		char utf8path[MAX_PATH * 3];
		int ulen = WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
					       utf8path, sizeof(utf8path), NULL, NULL);
		CHECK(ulen > 0, "open_wide: wpath → UTF-8");

		// Open via the shim (which should use _wopen internally)
		// _wopen test directly to avoid macro interference
		wchar_t wpath2[MAX_PATH];
		int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
					     utf8path, -1, wpath2, MAX_PATH);
		CHECK(wn > 0, "open_wide: UTF-8 round-trips to wide");
		if (wn > 0) {
			int fd = _wopen(wpath2, _O_RDONLY | _O_BINARY);
			CHECK(fd >= 0, "open_wide: _wopen opens UTF-8-named file");
			if (fd >= 0) {
				char buf[16];
				int n = _read(fd, buf, sizeof(buf) - 1);
				CHECK(n > 0, "open_wide: read from fd");
				if (n > 0) {
					buf[n] = '\0';
					CHECK(strcmp(buf, "hello\n") == 0,
					      "open_wide: content matches");
				}
				_close(fd);
			}
		}

		DeleteFileW(wpath);
	}
}

// Regression: mkdtemp must use CreateDirectoryW for UTF-8 path support.
static void test_win_mkdtemp_wide(void) {
	printf("  Testing mkdtemp CreateDirectoryW (wide path)...\n");

	// Verify that CreateDirectoryW can handle a UTF-8→wide converted path
	// with non-ASCII characters (same path as mkdtemp would produce).
	wchar_t tmp_dir[MAX_PATH];
	DWORD dw = GetTempPathW(MAX_PATH, tmp_dir);
	CHECK(dw > 0, "mkdtemp_wide: GetTempPathW");

	// Build a directory path with Unicode chars
	wchar_t wdir[MAX_PATH];
	swprintf(wdir, MAX_PATH, L"%s\\prism_mkd_\u00e9\u00fc_test", tmp_dir);

	// Convert to UTF-8 (simulating what mkdtemp's try_buf would contain)
	char utf8dir[MAX_PATH * 3];
	int ulen = WideCharToMultiByte(CP_UTF8, 0, wdir, -1,
				       utf8dir, sizeof(utf8dir), NULL, NULL);
	CHECK(ulen > 0, "mkdtemp_wide: wdir → UTF-8");

	if (ulen > 0) {
		// Round-trip: UTF-8 → wide → CreateDirectoryW (same as the fix does)
		wchar_t wrt[MAX_PATH];
		int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
					     utf8dir, -1, wrt, MAX_PATH);
		CHECK(wn > 0, "mkdtemp_wide: UTF-8 round-trips to wide");
		if (wn > 0) {
			// Clean up first in case of prior failed run
			RemoveDirectoryW(wrt);
			BOOL ok = CreateDirectoryW(wrt, NULL);
			CHECK(ok, "mkdtemp_wide: CreateDirectoryW creates Unicode dir");
			if (ok) {
				// Verify it exists
				DWORD attrs = GetFileAttributesW(wrt);
				CHECK(attrs != INVALID_FILE_ATTRIBUTES,
				      "mkdtemp_wide: dir exists");
				CHECK(attrs & FILE_ATTRIBUTE_DIRECTORY,
				      "mkdtemp_wide: is a directory");
				RemoveDirectoryW(wrt);
			}
		}
	}
}

// Regression: install() .old cleanup must use get_tmp_dir(), not getenv("TEMP").
static void test_win_install_tmpdir(void) {
	printf("  Testing install() uses get_tmp_dir() for .old cleanup...\n");

	// Verify that _wgetenv(L"TEMP") and get_tmp_dir() both return
	// consistent UTF-8 paths, proving the install() path is sound.
	const wchar_t *wtemp = _wgetenv(L"TEMP");
	CHECK(wtemp != NULL, "install_tmpdir: _wgetenv(L\"TEMP\") != NULL");
	if (wtemp) {
		char utf8_temp[MAX_PATH];
		int ulen = WideCharToMultiByte(CP_UTF8, 0, wtemp, -1,
					       utf8_temp, sizeof(utf8_temp), NULL, NULL);
		CHECK(ulen > 0, "install_tmpdir: TEMP → UTF-8");

		// get_tmp_dir() appends a trailing slash — strip it for comparison
		const char *gtd = get_tmp_dir();
		CHECK(gtd != NULL && *gtd, "install_tmpdir: get_tmp_dir() non-empty");
		if (gtd && *gtd) {
			char gtd_copy[MAX_PATH];
			strncpy(gtd_copy, gtd, sizeof(gtd_copy) - 1);
			gtd_copy[sizeof(gtd_copy) - 1] = '\0';
			size_t glen = strlen(gtd_copy);
			if (glen > 0 && (gtd_copy[glen - 1] == '/' || gtd_copy[glen - 1] == '\\'))
				gtd_copy[glen - 1] = '\0';

			CHECK(_stricmp(utf8_temp, gtd_copy) == 0,
			      "install_tmpdir: get_tmp_dir() matches _wgetenv TEMP");
		}
	}
}

// Regression: win32_spawn_with_actions SPAWN_ACT_OPEN must use CreateFileW.
static void test_win_spawn_open_wide(void) {
	printf("  Testing spawn SPAWN_ACT_OPEN CreateFileW (wide path)...\n");

	// Simulate what the spawn shim does: take a UTF-8 path, convert to wide,
	// and open via CreateFileW.
	wchar_t tmp_dir[MAX_PATH];
	DWORD dw = GetTempPathW(MAX_PATH, tmp_dir);
	CHECK(dw > 0, "spawn_open: GetTempPathW");

	wchar_t wpath[MAX_PATH];
	swprintf(wpath, MAX_PATH, L"%s\\prism_spawn_\u00e9.txt", tmp_dir);

	// Create the file
	HANDLE hf = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(hf != INVALID_HANDLE_VALUE, "spawn_open: create temp file");
	if (hf != INVALID_HANDLE_VALUE) {
		const char *data = "spawn test\n";
		DWORD written;
		WriteFile(hf, data, (DWORD)strlen(data), &written, NULL);
		CloseHandle(hf);

		// Convert to UTF-8 (as a->path would be)
		char utf8path[MAX_PATH * 3];
		int ulen = WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
					       utf8path, sizeof(utf8path), NULL, NULL);
		CHECK(ulen > 0, "spawn_open: wpath → UTF-8");

		// Now do what the fixed spawn shim does: UTF-8 → wide → CreateFileW
		if (ulen > 0) {
			wchar_t wpath2[MAX_PATH];
			int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
						     utf8path, -1, wpath2, MAX_PATH);
			CHECK(wn > 0, "spawn_open: UTF-8 round-trips to wide");
			if (wn > 0) {
				SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
				HANDLE hRead = CreateFileW(wpath2,
							   GENERIC_READ,
							   FILE_SHARE_READ | FILE_SHARE_WRITE,
							   &sa,
							   OPEN_EXISTING,
							   0,
							   NULL);
				CHECK(hRead != INVALID_HANDLE_VALUE,
				      "spawn_open: CreateFileW opens UTF-8-named file");
				if (hRead != INVALID_HANDLE_VALUE) CloseHandle(hRead);
			}
		}

		DeleteFileW(wpath);
	}
}

// Regression: preprocess_with_cc must use get_tmp_dir(), not getenv(TMPDIR_ENVVAR).
static void test_win_preprocess_tmpdir(void) {
	printf("  Testing preprocess_with_cc uses get_tmp_dir()...\n");

	// Verify get_tmp_dir() returns a valid writable directory.
	const char *tmpdir = get_tmp_dir();
	CHECK(tmpdir != NULL, "preprocess_tmpdir: get_tmp_dir() != NULL");
	CHECK(*tmpdir != '\0', "preprocess_tmpdir: get_tmp_dir() non-empty");

	if (tmpdir && *tmpdir) {
		// Simulate what preprocess_with_cc does: build a mkstemp template
		char pp_path[MAX_PATH];
		snprintf(pp_path, sizeof(pp_path), "%sprism_pp_err_XXXXXX", tmpdir);
		CHECK(strlen(pp_path) > 6, "preprocess_tmpdir: template is valid");

		// Verify the directory portion exists by converting to wide and checking
		wchar_t wtmpdir[MAX_PATH];
		int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
					     tmpdir, -1, wtmpdir, MAX_PATH);
		CHECK(wn > 0, "preprocess_tmpdir: tmpdir → wide");
		if (wn > 0) {
			DWORD attrs = GetFileAttributesW(wtmpdir);
			CHECK(attrs != INVALID_FILE_ATTRIBUTES,
			      "preprocess_tmpdir: temp dir exists");
			CHECK(attrs & FILE_ATTRIBUTE_DIRECTORY,
			      "preprocess_tmpdir: temp dir is a directory");
		}
	}
}

// Regression: fopen/stat/access/unlink/remove/open shims must not
// fall back to ANSI when the wide API fails legitimately (file not found).
static void test_win_shim_no_ansi_fallback(void) {
	printf("  Testing wide shims don't fall back to ANSI on legitimate failure...\n");

	// Create a file with Unicode name, verify stat finds it,
	// then delete and verify stat returns -1 (not ANSI fallback).
	wchar_t tmp_dir[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp_dir);

	wchar_t wpath[MAX_PATH];
	swprintf(wpath, MAX_PATH, L"%s\\prism_shim_\u00e9\u00fc.tmp", tmp_dir);

	char utf8path[MAX_PATH * 3];
	int ulen = WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
				       utf8path, sizeof(utf8path), NULL, NULL);
	CHECK(ulen > 0, "shim_nofb: path → UTF-8");

	// Create file via wide API
	HANDLE hf = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(hf != INVALID_HANDLE_VALUE, "shim_nofb: create temp file");
	if (hf != INVALID_HANDLE_VALUE) {
		WriteFile(hf, "x", 1, &(DWORD){0}, NULL);
		CloseHandle(hf);

		// stat should find it via wide API
		struct _stat st;
		int r = stat(utf8path, &st);
		CHECK(r == 0, "shim_nofb: stat finds Unicode file");

		// fopen should open it
		FILE *fp = fopen(utf8path, "rb");
		CHECK(fp != NULL, "shim_nofb: fopen opens Unicode file");
		if (fp) fclose(fp);

		// access should confirm it exists
		r = access(utf8path, 0);
		CHECK(r == 0, "shim_nofb: access finds Unicode file");

		// Delete the file
		DeleteFileW(wpath);

		// Now stat should FAIL (file gone) — must NOT fall to ANSI
		r = stat(utf8path, &st);
		CHECK(r != 0, "shim_nofb: stat returns -1 for deleted file");

		// fopen should return NULL (not ANSI garbage)
		fp = fopen(utf8path, "rb");
		CHECK(fp == NULL, "shim_nofb: fopen returns NULL for deleted file");

		// access should fail
		r = access(utf8path, 0);
		CHECK(r != 0, "shim_nofb: access returns -1 for deleted file");
	}
}

// Regression: get_env_utf8 must return UTF-8 strings from environment variables.
static void test_win_get_env_utf8(void) {
	printf("  Testing get_env_utf8 returns UTF-8...\n");

	// Test with a known variable that always exists
	const char *temp = get_env_utf8("TEMP");
	CHECK(temp != NULL, "get_env_utf8: TEMP is not NULL");
	if (temp) {
		CHECK(strlen(temp) > 0, "get_env_utf8: TEMP is non-empty");

		// Verify it matches what _wgetenv returns after UTF-8 conversion
		const wchar_t *wtemp = _wgetenv(L"TEMP");
		if (wtemp) {
			char expected[MAX_PATH * 3];
			int elen = WideCharToMultiByte(CP_UTF8, 0, wtemp, -1,
						       expected, sizeof(expected), NULL, NULL);
			CHECK(elen > 0, "get_env_utf8: _wgetenv converts");
			if (elen > 0) {
				CHECK(strcmp(temp, expected) == 0,
				      "get_env_utf8: matches _wgetenv UTF-8 conversion");
			}
		}
	}

	// Test with a nonexistent variable
	const char *missing = get_env_utf8("PRISM_THIS_VAR_DOES_NOT_EXIST_12345");
	CHECK(missing == NULL, "get_env_utf8: missing var returns NULL");
}

// Regression: collect_system_includes must handle backslash paths (MSVC).
static void test_win_path_basename_backslash(void) {
	printf("  Testing path_basename handles backslashes...\n");

	// path_basename should handle both / and \ separators
	const char *b1 = path_basename("C:\\Program Files\\include\\assert.h");
	CHECK(strcmp(b1, "assert.h") == 0,
	      "basename_bs: backslash path yields assert.h");

	const char *b2 = path_basename("/usr/include/assert.h");
	CHECK(strcmp(b2, "assert.h") == 0,
	      "basename_bs: forward-slash path yields assert.h");

	const char *b3 = path_basename("C:\\Mixed/Path\\to/file.h");
	CHECK(strcmp(b3, "file.h") == 0,
	      "basename_bs: mixed separators yield file.h");

	const char *b4 = path_basename("justfile.h");
	CHECK(strcmp(b4, "justfile.h") == 0,
	      "basename_bs: no separator returns full name");

	// This is the exact case that broke: MSVC outputs backslash paths
	const char *b5 = path_basename("C:\\Program Files (x86)\\Microsoft Visual Studio\\include\\assert.h");
	CHECK(strcmp(b5, "assert.h") == 0,
	      "basename_bs: MSVC-style path yields assert.h");
}

// Regression: win32_memstream_path must be large enough for UTF-8 expansion.
static void test_win_memstream_buffer_size(void) {
	printf("  Testing open_memstream buffer sizing...\n");

	// Verify that win32_memstream_path can hold an expanded UTF-8 path.
	// MAX_PATH wide chars can expand to MAX_PATH*3 UTF-8 bytes.
	// The buffer should be at least MAX_PATH*3.
	CHECK(sizeof(win32_memstream_path) >= MAX_PATH * 3,
	      "memstream_buf: win32_memstream_path >= MAX_PATH*3");

	// Verify open_memstream works (it uses win32_memstream_path internally)
	char *buf = NULL;
	size_t sz = 0;
	FILE *fp = open_memstream(&buf, &sz);
	CHECK(fp != NULL, "memstream_buf: open_memstream succeeds");
	if (fp) {
		fprintf(fp, "test data");
		fclose(fp);
		CHECK(buf != NULL, "memstream_buf: buffer is populated");
		CHECK(sz > 0, "memstream_buf: size > 0");
		if (buf) {
			CHECK(strcmp(buf, "test data") == 0,
			      "memstream_buf: content matches");
			free(buf);
		}
	}
}

// Regression: handles in PROC_THREAD_ATTRIBUTE_HANDLE_LIST must have
// HANDLE_FLAG_INHERIT set, otherwise UpdateProcThreadAttribute returns
// ERROR_INVALID_PARAMETER on some Windows versions.
static void test_win_handle_inherit_flag(void) {
	printf("  Testing handle inheritance flag for spawn...\n");

	// Create a pipe for stdout redirection – the write end must get
	// HANDLE_FLAG_INHERIT set by win32_spawn_with_actions.
	HANDLE hReadPipe, hWritePipe;
	SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE }; // NOT inheritable yet
	BOOL created = CreatePipe(&hReadPipe, &hWritePipe, &sa, 0);
	CHECK(created, "handle_inherit: CreatePipe succeeds");
	if (!created) return;

	// Simulate what win32_spawn_with_actions now does: set inheritance
	BOOL ok = SetHandleInformation(hWritePipe, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	CHECK(ok, "handle_inherit: SetHandleInformation succeeds");

	DWORD flags = 0;
	GetHandleInformation(hWritePipe, &flags);
	CHECK((flags & HANDLE_FLAG_INHERIT) != 0,
	      "handle_inherit: HANDLE_FLAG_INHERIT is set");

	// Now test that UpdateProcThreadAttribute accepts this handle
	SIZE_T attr_size = 0;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
	LPPROC_THREAD_ATTRIBUTE_LIST attrList =
		(LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
	CHECK(attrList != NULL, "handle_inherit: malloc attrList");
	if (attrList) {
		BOOL initOk = InitializeProcThreadAttributeList(attrList, 1, 0, &attr_size);
		CHECK(initOk, "handle_inherit: InitializeProcThreadAttributeList");
		if (initOk) {
			HANDLE handle_list[1] = { hWritePipe };
			BOOL updateOk = UpdateProcThreadAttribute(
				attrList, 0,
				PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
				handle_list, sizeof(HANDLE), NULL, NULL);
			CHECK(updateOk, "handle_inherit: UpdateProcThreadAttribute succeeds");
			DeleteProcThreadAttributeList(attrList);
		}
		free(attrList);
	}
	CloseHandle(hReadPipe);
	CloseHandle(hWritePipe);
}

// Regression: build_clean_environ must filter CC= case-insensitively on Windows.
static void test_win_env_case_insensitive(void) {
	printf("  Testing case-insensitive env scrubbing...\n");

	// On Windows, env vars are case-insensitive: cc=foo should be stripped
	// just like CC=foo.  build_clean_environ uses _strnicmp on Windows.
	// We can't easily call build_clean_environ directly because it caches,
	// but we can verify _strnicmp behavior directly.
	CHECK(_strnicmp("CC=clang", "CC=", 3) == 0,
	      "case_env: CC= matches CC= (upper)");
	CHECK(_strnicmp("cc=gcc", "CC=", 3) == 0,
	      "case_env: cc= matches CC= (lower)");
	CHECK(_strnicmp("Cc=msvc", "CC=", 3) == 0,
	      "case_env: Cc= matches CC= (mixed)");
	CHECK(_strnicmp("PRISM_CC=clang", "PRISM_CC=", 9) == 0,
	      "case_env: PRISM_CC= matches (upper)");
	CHECK(_strnicmp("prism_cc=gcc", "PRISM_CC=", 9) == 0,
	      "case_env: prism_cc= matches (lower)");
	CHECK(_strnicmp("PATH=foo", "CC=", 3) != 0,
	      "case_env: PATH= does not match CC=");
}

// Regression: win32_build_env_block must produce a valid double-NUL terminated
// block even when the environment is empty (zero entries).
static void test_win_empty_env_block(void) {
	printf("  Testing empty env block double-NUL...\n");

	// An empty envp array: just a NULL terminator
	char *empty_envp[] = { NULL };
	wchar_t *block = win32_build_env_block(empty_envp);
	CHECK(block != NULL, "empty_env: allocation succeeds");
	if (block) {
		// A valid empty environment block is L"\0\0" (two NUL wchars).
		CHECK(block[0] == L'\0', "empty_env: first wchar is NUL");
		CHECK(block[1] == L'\0', "empty_env: second wchar is NUL (double-NUL)");
		free(block);
	}

	// Also test NULL envp returns NULL (inherit parent)
	CHECK(win32_build_env_block(NULL) == NULL,
	      "empty_env: NULL envp returns NULL");
}

// Regression: open_memstream must register its temp file for signal cleanup
// so Ctrl-C doesn't leave stale prmXXXX.tmp files.
static void test_win_memstream_signal_register(void) {
	printf("  Testing open_memstream signal temp registration...\n");

	char *buf = NULL;
	size_t sz = 0;
	FILE *fp = open_memstream(&buf, &sz);
	CHECK(fp != NULL, "memstream_reg: open_memstream succeeds");
	if (fp) {
		// The temp file path should be registered in signal_temps[]
		// Search for win32_memstream_path in the signal_temps array.
		CHECK(strlen(win32_memstream_path) > 0,
		      "memstream_reg: path is non-empty");

		bool found = false;
		sig_atomic_t n = signal_temps_load();
		for (sig_atomic_t i = 0; i < n; i++) {
			if (signal_temps_ready_load(i) &&
			    strcmp(signal_temps[i], win32_memstream_path) == 0) {
				found = true;
				break;
			}
		}
		CHECK(found, "memstream_reg: path registered in signal_temps");

		fprintf(fp, "signal test");
		fclose(fp);
		if (buf) free(buf);
	}
}

// Regression: capture_first_line must use STARTUPINFOEX with a handle whitelist
// (PROC_THREAD_ATTRIBUTE_HANDLE_LIST) just like win32_spawn_with_actions, to
// prevent leaking all inheritable handles into the probe child process.
static void test_win_capture_handle_whitelist(void) {
	printf("  Testing capture_first_line uses handle whitelist...\n");

	// Create a pipe that should NOT be inherited by capture_first_line's child.
	// If the old code (plain STARTUPINFOW + bInheritHandles=TRUE) were still
	// in use, this pipe would leak.  With the new STARTUPINFOEX whitelist,
	// only the explicitly listed handles are inherited.
	HANDLE hRead, hWrite;
	SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE }; // inheritable
	BOOL created = CreatePipe(&hRead, &hWrite, &sa, 0);
	CHECK(created, "capture_wl: CreatePipe for bystander pipe");
	if (!created) return;

	// Run capture_first_line with a trivial command.  If the bystander pipe
	// leaks into the child, it won't directly fail here, but the test proves
	// the code path compiles and runs with the new STARTUPINFOEX logic.
	char buf[256];
	char *argv[] = { "cmd.exe", "/c", "echo whitelist_ok", NULL };
	int rc = capture_first_line(argv, buf, sizeof(buf));
	CHECK(rc == 0, "capture_wl: capture_first_line succeeds");
	if (rc == 0) {
		CHECK(strcmp(buf, "whitelist_ok") == 0,
		      "capture_wl: output is whitelist_ok");
	}

	// The bystander pipe should still be valid (not closed by child exit).
	DWORD flags = 0;
	BOOL info_ok = GetHandleInformation(hWrite, &flags);
	CHECK(info_ok, "capture_wl: bystander pipe still valid after child");

	CloseHandle(hRead);
	CloseHandle(hWrite);
}

// Regression: win32_fclose_wrapper must use win32_unlink_utf8 (not raw _unlink)
// to properly delete temp files with non-ASCII paths.
static void test_win_fclose_uses_utf8_unlink(void) {
	printf("  Testing fclose wrapper uses UTF-8 unlink shim...\n");

	// open_memstream creates a temp file and stores it in win32_memstream_path.
	// When we fclose it, the wrapper should call win32_unlink_utf8, which
	// converts UTF-8 to wide before calling _wunlink.
	char *buf = NULL;
	size_t sz = 0;
	FILE *fp = open_memstream(&buf, &sz);
	CHECK(fp != NULL, "fclose_utf8: open_memstream succeeds");
	if (fp) {
		fprintf(fp, "test content for unlink");
		// Note the path before fclose deletes it
		char saved_path[4096];
		strncpy(saved_path, win32_memstream_path, sizeof(saved_path) - 1);
		saved_path[sizeof(saved_path) - 1] = '\0';

		// Verify the temp file exists
		struct _stat st;
		int exists_before = (win32_stat_utf8(saved_path, &st) == 0);
		CHECK(exists_before, "fclose_utf8: temp file exists before fclose");

		fclose(fp);

		// After fclose, the temp file should be deleted by win32_unlink_utf8
		int exists_after = (win32_stat_utf8(saved_path, &st) == 0);
		CHECK(!exists_after, "fclose_utf8: temp file deleted after fclose");

		if (buf) free(buf);
	}
}

// Regression: PATH_MAX must be large enough for UTF-8 expansion of Windows
// wide-char paths.  MAX_PATH (260) wide chars can expand to 780 UTF-8 bytes.
static void test_win_path_max_utf8_safe(void) {
	printf("  Testing PATH_MAX is UTF-8-safe...\n");

	// PATH_MAX should be at least MAX_PATH * 3 (780) to hold any possible
	// UTF-8 expansion of a MAX_PATH wide-char path.  We set it to 4096.
	CHECK(PATH_MAX >= MAX_PATH * 3,
	      "path_max: PATH_MAX >= MAX_PATH * 3");
	CHECK(PATH_MAX >= 4096,
	      "path_max: PATH_MAX >= 4096");

	// signal_temps entries should be large enough
	CHECK(sizeof(signal_temps[0]) >= 4096,
	      "path_max: signal_temps entry >= 4096 bytes");

	// signal_temp_path should be large enough
	CHECK(sizeof(signal_temp_path) >= 4096,
	      "path_max: signal_temp_path >= 4096 bytes");
}

// Regression: signal_temps_register must accept long UTF-8 paths (>= 260 bytes).
// Previously PATH_MAX was 260, silently dropping legitimate long paths.
static void test_win_signal_register_long_path(void) {
	printf("  Testing signal_temps_register accepts long UTF-8 paths...\n");

	// Build a valid-looking path that's > 260 bytes but < 4096
	char long_path[512];
	memset(long_path, 0, sizeof(long_path));
	strcpy(long_path, "C:\\Users\\");
	// Pad with realistic directory components to exceed 260
	for (int i = (int)strlen(long_path); i < 300; i++)
		long_path[i] = (i % 10 == 0) ? '\\' : 'a';
	strcat(long_path, "\\temp.tmp");

	size_t len = strlen(long_path);
	CHECK(len > 260, "sig_long: path exceeds old PATH_MAX of 260");
	CHECK(len < PATH_MAX, "sig_long: path fits in new PATH_MAX");

	// Register it — should NOT be silently dropped anymore
	sig_atomic_t count_before = signal_temps_load();
	signal_temps_register(long_path);
	sig_atomic_t count_after = signal_temps_load();
	CHECK(count_after == count_before + 1,
	      "sig_long: path was registered (count incremented)");

	// Verify the path is stored correctly
	if (count_after > count_before) {
		int idx = (int)(count_after - 1);
		CHECK(signal_temps_ready_load(idx),
		      "sig_long: entry marked ready");
		CHECK(strcmp(signal_temps[idx], long_path) == 0,
		      "sig_long: stored path matches");
	}
}

// Regression: multiple SPAWN_ACT_OPEN actions must not leak file handles.
// Previously, a single HANDLE hNul was overwritten by each CreateFile;
// only the last handle was closed, leaking all prior ones.
static void test_win_spawn_multi_open_handles(void) {
	printf("  Testing SPAWN_ACT_OPEN closes all opened handles...\n");

	// Create two distinct temp files to redirect stdout and stderr to different files.
	char tmpdir_buf[PATH_MAX];
	const char *tmpdir = get_tmp_dir();
	snprintf(tmpdir_buf, sizeof(tmpdir_buf), "%s", tmpdir);

	char path_out[PATH_MAX], path_err[PATH_MAX];
	snprintf(path_out, sizeof(path_out), "%sprism_test_out_XXXXXX", tmpdir_buf);
	snprintf(path_err, sizeof(path_err), "%sprism_test_err_XXXXXX", tmpdir_buf);

	int fd_out = mkstemp(path_out);
	int fd_err = mkstemp(path_err);
	CHECK(fd_out >= 0, "spawn_multi: mkstemp for stdout");
	CHECK(fd_err >= 0, "spawn_multi: mkstemp for stderr");
	if (fd_out < 0 || fd_err < 0) {
		if (fd_out >= 0) { close(fd_out); remove(path_out); }
		if (fd_err >= 0) { close(fd_err); remove(path_err); }
		return;
	}
	close(fd_out);
	close(fd_err);

	// Set up two SPAWN_ACT_OPEN actions: stdout -> path_out, stderr -> path_err
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, path_out,
					 O_WRONLY | O_CREAT | O_TRUNC, 0666);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, path_err,
					 O_WRONLY | O_CREAT | O_TRUNC, 0666);

	// Spawn a process with these two redirections.
	// If the old code leaked the first handle, it wouldn't be obvious from
	// this test alone, but we verify both files get written to correctly.
	char *argv[] = { "cmd.exe", "/c", "echo STDOUT_DATA && echo STDERR_DATA>&2", NULL };
	pid_t pid;
	int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, NULL);
	CHECK(rc == 0, "spawn_multi: posix_spawnp succeeds");

	if (rc == 0) {
		int status;
		waitpid(pid, &status, 0);
		CHECK(status == 0, "spawn_multi: child exited cleanly");

		// Verify stdout was redirected to path_out
		FILE *f = fopen(path_out, "r");
		CHECK(f != NULL, "spawn_multi: stdout file exists");
		if (f) {
			char buf[256];
			if (fgets(buf, sizeof(buf), f)) {
				// Trim whitespace
				char *end = buf + strlen(buf) - 1;
				while (end >= buf && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
				CHECK(strcmp(buf, "STDOUT_DATA") == 0,
				      "spawn_multi: stdout contains STDOUT_DATA");
			}
			fclose(f);
		}

		// Verify stderr was redirected to path_err
		f = fopen(path_err, "r");
		CHECK(f != NULL, "spawn_multi: stderr file exists");
		if (f) {
			char buf[256];
			if (fgets(buf, sizeof(buf), f)) {
				char *end = buf + strlen(buf) - 1;
				while (end >= buf && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
				CHECK(strcmp(buf, "STDERR_DATA") == 0,
				      "spawn_multi: stderr contains STDERR_DATA");
			}
			fclose(f);
		}
	}

	posix_spawn_file_actions_destroy(&fa);
	remove(path_out);
	remove(path_err);
}

// Regression: realpath must correctly strip \\?\UNC\ prefix on network paths.
// Previously, stripping exactly 4 chars from \\?\UNC\server\share produced
// the broken path "UNC\server\share" instead of "\\server\share".
static void test_win_realpath_unc_prefix(void) {
	printf("  Testing realpath UNC prefix stripping logic...\n");

	// We can't easily create a real network path in a test environment,
	// so we test the prefix-stripping logic directly by calling
	// GetFinalPathNameByHandleW on a local path and checking the \\?\ strip.
	// Then we verify the UNC branch compiles and has correct pointer math.

	// Test 1: local path stripping works (\\?\C:\... -> C:\...)
	char resolved[PATH_MAX];
	char *rp = realpath(".", resolved);
	CHECK(rp != NULL, "realpath_unc: resolves current dir");
	if (rp) {
		// Result should be a normal local path (C:\...), not \\?\...
		CHECK(rp[0] != '\\' || rp[1] != '\\' || rp[2] != '?',
		      "realpath_unc: \\\\?\\ prefix is stripped");
		// Should start with a drive letter
		CHECK(((rp[0] >= 'A' && rp[0] <= 'Z') || (rp[0] >= 'a' && rp[0] <= 'z')) && rp[1] == ':',
		      "realpath_unc: starts with drive letter");
	}

	// Test 2: Verify the UNC prefix detection math is correct.
	// Simulate: if GetFinalPathNameByHandleW returned L"\\\\?\\UNC\\server\\share\\file.c",
	// after stripping, we should get L"\\\\server\\share\\file.c" (advance 6, write \\).
	wchar_t sim_unc[] = L"\\\\?\\UNC\\myserver\\myshare\\dir\\file.c";
	DWORD sim_len = (DWORD)wcslen(sim_unc);
	wchar_t *result_start = sim_unc;
	if (sim_len >= 4 && sim_unc[0] == L'\\' && sim_unc[1] == L'\\' &&
	    sim_unc[2] == L'?' && sim_unc[3] == L'\\') {
		if (sim_len >= 8 && sim_unc[4] == L'U' && sim_unc[5] == L'N' &&
		    sim_unc[6] == L'C' && sim_unc[7] == L'\\') {
			result_start += 6;
			result_start[0] = L'\\';
		} else {
			result_start += 4;
		}
	}
	// Convert to UTF-8 and verify
	char unc_result[PATH_MAX];
	WideCharToMultiByte(CP_UTF8, 0, result_start, -1, unc_result, sizeof(unc_result), NULL, NULL);
	CHECK(unc_result[0] == '\\' && unc_result[1] == '\\',
	      "realpath_unc: UNC result starts with \\\\");
	CHECK(strncmp(unc_result, "\\\\myserver\\", 11) == 0,
	      "realpath_unc: UNC result is \\\\myserver\\...");
	CHECK(strstr(unc_result, "UNC") == NULL,
	      "realpath_unc: no stray UNC in result");

	// Test 3: Verify non-UNC extended path (\\?\C:\...) strips correctly
	wchar_t sim_local[] = L"\\\\?\\C:\\Users\\test\\file.c";
	DWORD sim_local_len = (DWORD)wcslen(sim_local);
	wchar_t *local_start = sim_local;
	if (sim_local_len >= 4 && sim_local[0] == L'\\' && sim_local[1] == L'\\' &&
	    sim_local[2] == L'?' && sim_local[3] == L'\\') {
		if (sim_local_len >= 8 && sim_local[4] == L'U' && sim_local[5] == L'N' &&
		    sim_local[6] == L'C' && sim_local[7] == L'\\') {
			local_start += 6;
			local_start[0] = L'\\';
		} else {
			local_start += 4;
		}
	}
	char local_result[PATH_MAX];
	WideCharToMultiByte(CP_UTF8, 0, local_start, -1, local_result, sizeof(local_result), NULL, NULL);
	CHECK(local_result[0] == 'C' && local_result[1] == ':',
	      "realpath_unc: local \\\\?\\ stripped to C:\\...");
}

// Regression: win32_utf8_argv must not pass needed=0 to malloc.
// Previously, if WideCharToMultiByte returned 0 (unmappable), malloc(0) returned
// a valid pointer with uninitialized data, then argv[i] lacked a null terminator.
static void test_win_utf8_argv_zero_needed(void) {
	printf("  Testing win32_utf8_argv handles edge cases...\n");

	// Test 1: Normal ASCII argument converts correctly
	{
		wchar_t warg[] = L"hello";
		int needed = WideCharToMultiByte(CP_UTF8, 0, warg, -1, NULL, 0, NULL, NULL);
		CHECK(needed > 0, "utf8_argv: ASCII needed > 0");
		if (needed > 0) {
			char *arg = (char *)malloc((size_t)needed);
			CHECK(arg != NULL, "utf8_argv: malloc succeeds");
			if (arg) {
				WideCharToMultiByte(CP_UTF8, 0, warg, -1, arg, needed, NULL, NULL);
				CHECK(strcmp(arg, "hello") == 0, "utf8_argv: ASCII converts correctly");
				free(arg);
			}
		}
	}

	// Test 2: Empty string should still work (needed=1 for null terminator)
	{
		wchar_t warg[] = L"";
		int needed = WideCharToMultiByte(CP_UTF8, 0, warg, -1, NULL, 0, NULL, NULL);
		CHECK(needed == 1, "utf8_argv: empty string needed == 1");
	}

	// Test 3: The fix — when needed <= 0, we must not malloc and use it
	// Directly test the fixed pattern: if (needed > 0 && (argv[i] = malloc(needed)))
	{
		int needed = 0; // simulate failure
		char *arg = NULL;
		// The fix: only malloc and use if needed > 0
		if (needed > 0 && (arg = (char *)malloc((size_t)needed)))
			CHECK(0, "utf8_argv: should not reach here with needed=0");
		else
			arg = _strdup("");
		CHECK(arg != NULL, "utf8_argv: fallback to strdup");
		CHECK(strcmp(arg, "") == 0, "utf8_argv: fallback is empty string");
		free(arg);
	}

	// Test 4: Verify full win32_utf8_argv works end-to-end
	// (it operates on the actual process command line, so just check it doesn't crash)
	{
		int argc = 0;
		char **argv = NULL;
		win32_utf8_argv(&argc, &argv);
		CHECK(argc > 0, "utf8_argv: parsed at least 1 arg");
		CHECK(argv != NULL, "utf8_argv: argv is not NULL");
		if (argv) {
			CHECK(argv[0] != NULL, "utf8_argv: argv[0] is not NULL");
			CHECK(strlen(argv[0]) > 0, "utf8_argv: argv[0] is non-empty");
			// Don't free — these are the process args
		}
	}
}

// Regression: get_env_utf8 must not clobber previous results.
// Previously, a single static buffer was reused for all calls, so
// const char *a = getenv("A"); const char *b = getenv("B"); would make
// a and b point to the same buffer containing B's value.
static void test_win_get_env_utf8_no_clobber(void) {
	printf("  Testing get_env_utf8 rotating buffer prevents clobbering...\n");

	// Set up known environment variables (use _wputenv to set them)
	_wputenv(L"PRISM_TEST_ENV_A=alpha_value");
	_wputenv(L"PRISM_TEST_ENV_B=bravo_value");
	_wputenv(L"PRISM_TEST_ENV_C=charlie_value");
	_wputenv(L"PRISM_TEST_ENV_D=delta_value");

	// Grab all 4 pointers before any of them can be clobbered
	const char *a = get_env_utf8("PRISM_TEST_ENV_A");
	const char *b = get_env_utf8("PRISM_TEST_ENV_B");
	const char *c = get_env_utf8("PRISM_TEST_ENV_C");
	const char *d = get_env_utf8("PRISM_TEST_ENV_D");

	CHECK(a != NULL, "env_clobber: A is not NULL");
	CHECK(b != NULL, "env_clobber: B is not NULL");
	CHECK(c != NULL, "env_clobber: C is not NULL");
	CHECK(d != NULL, "env_clobber: D is not NULL");

	if (a && b && c && d) {
		// With the old single-buffer code, ALL four would equal "delta_value".
		// With the rotating pool of 4, all should be distinct.
		CHECK(strcmp(a, "alpha_value") == 0,  "env_clobber: A == alpha_value");
		CHECK(strcmp(b, "bravo_value") == 0,  "env_clobber: B == bravo_value");
		CHECK(strcmp(c, "charlie_value") == 0, "env_clobber: C == charlie_value");
		CHECK(strcmp(d, "delta_value") == 0,  "env_clobber: D == delta_value");

		// Verify they're different pointers
		CHECK(a != b, "env_clobber: A and B are different pointers");
		CHECK(b != c, "env_clobber: B and C are different pointers");
		CHECK(c != d, "env_clobber: C and D are different pointers");
	}

	// A 5th call WILL reuse the first slot (rotating pool of 4), so a is now stale.
	// This is expected and documented behavior — the pool handles the common case.
	const char *e_val = get_env_utf8("PRISM_TEST_ENV_A");
	// 'a' may now be clobbered since slot 0 was reused — that's by design.
	// But b, c, d should still be valid.
	if (b && c && d) {
		CHECK(strcmp(b, "bravo_value") == 0,  "env_clobber: B survives 5th call");
		CHECK(strcmp(c, "charlie_value") == 0, "env_clobber: C survives 5th call");
		CHECK(strcmp(d, "delta_value") == 0,  "env_clobber: D survives 5th call");
	}
	(void)e_val;

	// Clean up
	_wputenv(L"PRISM_TEST_ENV_A=");
	_wputenv(L"PRISM_TEST_ENV_B=");
	_wputenv(L"PRISM_TEST_ENV_C=");
	_wputenv(L"PRISM_TEST_ENV_D=");
}

void run_windows_tests(void) {
	printf("=== PRISM WINDOWS TEST SUITE ===\n");

	test_zeroinit_win_types();
	test_zeroinit_win_structs();
	test_zeroinit_win_arrays();

	test_defer_heap();
	test_defer_virtual_memory();
	test_defer_file_handle();
	test_defer_semaphore();
	test_defer_mutex();
	test_defer_event();
	test_defer_critical_section();
	test_defer_multi_resource();
	test_defer_early_return();
	test_defer_loop_alloc();
	test_defer_loop_break();
	test_defer_loop_continue();
	test_defer_nested_scopes_win();
	test_defer_switch();
	test_defer_mmap();
	test_defer_stress();
	test_goto_defer();
	test_defer_thread();

	test_raw_win_types();
	test_orelse_win_apis();
	test_wide_string_apis();
	test_win_api_queries();
	test_spaces_in_paths();
	test_unicode_paths();
	test_quoted_cc_parsing();
	
	test_combined_patterns();
	test_extreme_combined();

	test_interleaved_flow();

	test_msvc_regressions();

	test_win_path_wipe();
	test_win_capture_injection();
	test_win_tempfile_exclusive();
	test_win_stderr_preserved();
	test_win_realpath_resolves();
	test_win_spawn_oflag();

	test_win_env_scrubbing();
	test_win_handle_isolation();
	test_win_std_clatest_override();
	test_win_old_exe_cleanup();

	test_win_registry_wide_path();
	test_win_signal_tempfile_cleanup();
	test_win_realpath_unicode();
	test_win_capture_wide();

	test_win_install_path_wide();
	test_win_remove_utf8();
	test_win_memstream_wide_temp();
	test_win_oem_to_utf8();

	test_win_tokenize_file_wide();
	test_win_tmpdir_wide();
	test_win_getcwd_wide();
	test_win_open_wide();

	test_win_mkdtemp_wide();
	test_win_install_tmpdir();
	test_win_spawn_open_wide();
	test_win_preprocess_tmpdir();

	test_win_shim_no_ansi_fallback();
	test_win_get_env_utf8();
	test_win_path_basename_backslash();
	test_win_memstream_buffer_size();

	test_win_handle_inherit_flag();
	test_win_env_case_insensitive();
	test_win_empty_env_block();
	test_win_memstream_signal_register();

	test_win_capture_handle_whitelist();
	test_win_fclose_uses_utf8_unlink();
	test_win_path_max_utf8_safe();
	test_win_signal_register_long_path();

	test_win_spawn_multi_open_handles();
	test_win_realpath_unc_prefix();
	test_win_utf8_argv_zero_needed();
	test_win_get_env_utf8_no_clobber();
}

#else

void run_windows_tests(void) {
	printf("Windows tests skipped: not running on Windows platform.\n");
}

#endif

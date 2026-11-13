#include "compat.h"
#include "nostd.h"

static int console, shortWrite, failWrite, failConvert, failAlloc;
static unsigned calls, count;
static char captured[16384];
static BOOL out_file(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL out_console(HANDLE, const VOID *, DWORD, LPDWORD, LPVOID);
static BOOL out_mode(HANDLE, LPDWORD);
static int out_convert(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);
static void *out_alloc(size_t);
#define WriteFile out_file
#define WriteConsoleW out_console
#define GetConsoleMode out_mode
#define WideCharToMultiByte out_convert
#define malloc out_alloc
#include "util.c"
#undef WriteFile
#undef WriteConsoleW
#undef GetConsoleMode
#undef WideCharToMultiByte
#undef malloc

static BOOL out_file(HANDLE h, LPCVOID data, DWORD n, LPDWORD written, LPOVERLAPPED ov)
{
    (void)h; (void)ov;
    calls++;
    if (failWrite) { *written = 0; return failWrite == 2; }
    if (shortWrite && n > 3) n = 3;
    if (count + n > sizeof(captured)) return FALSE;
    memcpy(captured + count, data, n); count += n; *written = n;
    return TRUE;
}
static BOOL out_console(HANDLE h, const VOID *data, DWORD n, LPDWORD written, LPVOID r)
{
    const wchar_t *w = data;
    DWORD i;
    (void)h; (void)r;
    calls++;
    if (failWrite) { *written = 0; return FALSE; }
    if (shortWrite && n > 2) n = 2;
    for (i = 0; i < n; i++) captured[count++] = (char)w[i];
    *written = n; return TRUE;
}
static BOOL out_mode(HANDLE h, LPDWORD mode) { (void)h; *mode = 0; return console; }
static int out_convert(UINT cp, DWORD flags, LPCWCH w, int n, LPSTR b, int cap,
                        LPCCH def, LPBOOL used)
{
    return failConvert ? 0 : WideCharToMultiByte(cp, flags, w, n, b, cap, def, used);
}
static void *out_alloc(size_t n) { return failAlloc ? NULL : malloc(n); }
static int check(int ok, int line)
{
    if (!ok) {
        char msg[] = "Output regression failed.\r\n"; DWORD written;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, sizeof(msg) - 1, &written, NULL);
        return line;
    }
    return 0;
}
#define CHECK(x) do { int error = check((x), __LINE__); if (error) return error; } while (0)
static void reset(void)
{
    console = shortWrite = failWrite = failConvert = failAlloc = 0;
    calls = count = 0; g_outputError = 0;
}
int wmain(int argc, wchar_t **argv)
{
    wchar_t longLine[6001]; unsigned i; DWORD written;
    static const char passed[] = "Output short-write/error/long-path regressions passed.\r\n";
    (void)argc; (void)argv;
    reset(); shortWrite = 1;
    uinfo(L"hello");
    CHECK(uoutput_ok() && count == 5 && !memcmp(captured, "hello", 5) && calls == 2);
    reset(); console = shortWrite = 1;
    uinfo(L"hello");
    CHECK(uoutput_ok() && count == 5 && !memcmp(captured, "hello", 5) && calls == 3);
    reset(); failWrite = 2;
    uinfo(L"hello");
    CHECK(!uoutput_ok() && calls == 2);
    reset(); failConvert = 1;
    uinfo(L"hello");
    CHECK(!uoutput_ok());
    reset(); failAlloc = 1;
    uinfo(L"hello");
    CHECK(!uoutput_ok());
    reset();
    for (i = 0; i < 6000; i++) longLine[i] = L'x';
    longLine[6000] = 0;
    uinfo(L"%ls", longLine);
    CHECK(uoutput_ok() && count == 6000);
    reset();
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), passed, sizeof(passed) - 1, &written, NULL);
    return 0;
}

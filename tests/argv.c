#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef unsigned int UINT;
typedef unsigned long DWORD;
#define HSDEFRAG_COMPAT_H
#define HEAP_ZERO_MEMORY 8
#define STD_ERROR_HANDLE ((DWORD)-12)
static int calls, failAt, live, exitCode, mainCalls, errorWrites;
static jmp_buf exited;
static wchar_t commandLine[128] =
    L"program A B C D E F G H I J K L M N O";

static void *GetProcessHeap(void) { return NULL; }
static void *HeapAlloc(void *heap, unsigned flags, size_t size)
{
    void *p;
    (void)heap;
    if (++calls == failAt)
        return NULL;
    p = flags & HEAP_ZERO_MEMORY ? calloc(1, size) : malloc(size);
    if (p)
        live++;
    return p;
}
static void *HeapReAlloc(void *heap, unsigned flags, void *ptr, size_t size)
{
    (void)heap; (void)flags;
    if (++calls == failAt)
        return NULL;
    return realloc(ptr, size);
}
static int HeapFree(void *heap, unsigned flags, void *ptr)
{
    (void)heap; (void)flags;
    if (ptr)
        live--;
    free(ptr);
    return 1;
}
static wchar_t *GetCommandLineW(void)
{
    return commandLine;
}
static void *GetStdHandle(DWORD which) { (void)which; return NULL; }
static int WriteFile(void *h, const void *p, DWORD n, DWORD *written, void *o)
{
    (void)h; (void)p; (void)o;
    errorWrites++;
    *written = n;
    return 1;
}
static void ExitProcess(UINT code) { exitCode = (int)code; longjmp(exited, 1); }
int uoutput_ok(void) { return 1; }

#define malloc hsd_malloc
#define calloc hsd_calloc
#define realloc hsd_realloc
#define free hsd_free
#define memset hsd_memset
#define memcpy hsd_memcpy
#define memcmp hsd_memcmp
#define memmove hsd_memmove
#define wcslen hsd_wcslen
#define wcscpy hsd_wcscpy
#define wcscat hsd_wcscat
#define wcscmp hsd_wcscmp
#include "nostd.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef memset
#undef memcpy
#undef memcmp
#undef memmove
#undef wcslen
#undef wcscpy
#undef wcscat
#undef wcscmp

int wmain(int argc, wchar_t **argv)
{
    assert(argc == 16 && argv[argc] == NULL);
    mainCalls++;
    return 0;
}

static int parse(void)
{
    int argc = -1;
    wchar_t **argv = build_argv(GetCommandLineW(), &argc);
    if (!argv) {
        assert(argc == 0 && live == 0);
        return 0;
    }
    assert(argc == 16 && argv[argc] == NULL);
    while (argc)
        hsd_free(argv[--argc]);
    hsd_free(argv);
    assert(live == 0);
    return 1;
}

static void quoted_parse_checks(void)
{
    int argc = 0;
    wchar_t **argv;
    wcscpy(commandLine, L"program \"C:\\Program Files\" \"a\\\"b\" \"\"");
    argv = build_argv(commandLine, &argc);
    assert(argv && argc == 4 && argv[argc] == NULL);
    assert(wcscmp(argv[0], L"program") == 0);
    assert(wcscmp(argv[1], L"C:\\Program Files") == 0);
    assert(wcscmp(argv[2], L"a\"b") == 0);
    assert(wcscmp(argv[3], L"") == 0);
    while (argc)
        hsd_free(argv[--argc]);
    hsd_free(argv);
    assert(live == 0);
    wcscpy(commandLine, L"program A B C D E F G H I J K L M N O");
}

static void memmove_checks(void)
{
    char data[] = "abcdef";
    char source[] = "xyz";
    assert(hsd_memmove(data + 1, data, 5) == data + 1);
    assert(memcmp(data, "aabcde", 6) == 0);
    assert(hsd_memmove(data, data + 1, 5) == data);
    assert(memcmp(data, "abcde", 5) == 0);
    assert(hsd_memmove(data, source, sizeof(source)) == data);
    assert(memcmp(data, "xyz", sizeof(source)) == 0);
}

int main(void)
{
    int total, i;
    quoted_parse_checks();
    memmove_checks();
    calls = 0;
    assert(parse());
    total = calls;
    for (i = 1; i <= total; i++) {
        calls = 0;
        failAt = i;
        assert(!parse());
    }
    calls = 0;
    failAt = 1;
    if (!setjmp(exited))
        wmainCRTStartup();
    assert(exitCode == 5 && !mainCalls && live == 0 && errorWrites == 1);
    failAt = calls = 0;
    if (!setjmp(exited))
        wmainCRTStartup();
    assert(exitCode == 0 && mainCalls == 1 && live == 0);
    printf("argv allocation/startup regressions: all %d failure positions passed.\n", total);
    return 0;
}

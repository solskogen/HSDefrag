/*
 * nostd.c - Minimal freestanding C-runtime replacement (see nostd.h).
 *
 * Provides heap-backed memory functions, block/string helpers, and the program
 * entry point, so the binary links with -nostdlib and imports only KERNEL32.
 *
 * The block routines are marked so the optimizer does not rewrite their loops
 * back into calls to memset/memcpy (which would recurse infinitely).
 */
#include "nostd.h"
#include "compat.h"

#define NOINLINE_PATTERN __attribute__((optimize("no-tree-loop-distribute-patterns")))

/* ------------------------------------------------------------------ memory */

void *malloc(size_t size)
{
    if (size == 0)
        size = 1;
    return HeapAlloc(GetProcessHeap(), 0, size);
}

void *calloc(size_t count, size_t size)
{
    size_t total = count * size;
    if (count != 0 && total / count != size)
        return NULL; /* overflow */
    if (total == 0)
        total = 1;
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
}

void *realloc(void *ptr, size_t size)
{
    if (ptr == NULL)
        return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    return HeapReAlloc(GetProcessHeap(), 0, ptr, size);
}

void free(void *ptr)
{
    if (ptr)
        HeapFree(GetProcessHeap(), 0, ptr);
}

/* --------------------------------------------------------------- block ops */

NOINLINE_PATTERN void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

NOINLINE_PATTERN void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

NOINLINE_PATTERN int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    while (n--) {
        if (*p != *q)
            return (int)*p - (int)*q;
        p++;
        q++;
    }
    return 0;
}

NOINLINE_PATTERN void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0)
        return dst;
    if ((size_t)d < (size_t)s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

/* ------------------------------------------------------------- wide string */

size_t wcslen(const wchar_t *s)
{
    const wchar_t *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

wchar_t *wcscpy(wchar_t *dst, const wchar_t *src)
{
    wchar_t *d = dst;
    while ((*d++ = *src++) != 0)
        ;
    return dst;
}

wchar_t *wcscat(wchar_t *dst, const wchar_t *src)
{
    wchar_t *d = dst;
    while (*d)
        d++;
    while ((*d++ = *src++) != 0)
        ;
    return dst;
}

int wcscmp(const wchar_t *a, const wchar_t *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(*a - *b);
}

/* --------------------------------------------------------- program startup */

/* GCC emits a call to __main in a function literally named "main"; our entry
 * is wmain, so this is unused, but provide a harmless stub just in case. */
void __main(void) {}

extern int wmain(int argc, wchar_t **argv);
extern int uoutput_ok(void);

/* Minimal command-line tokenizer (space separated, double-quote aware). */
static wchar_t **build_argv(wchar_t *cmd, int *argcOut)
{
    int cap = 8;
    int argc = 0;
    wchar_t **argv = (wchar_t **)malloc((size_t)cap * sizeof(wchar_t *));
    wchar_t *p = cmd;

    if (!argv) {
        *argcOut = 0;
        return NULL;
    }

    for (;;) {
        wchar_t *tok;
        int ti = 0;
        int inq = 0;

        while (*p == L' ' || *p == L'\t')
            p++;
        if (*p == 0)
            break;

        tok = (wchar_t *)malloc((wcslen(p) + 1) * sizeof(wchar_t));
        if (!tok)
            goto fail;

        while (*p) {
            if (*p == L'\\') {
                size_t slashes = 0, j;
                while (*p == L'\\') {
                    slashes++;
                    p++;
                }
                if (*p == L'"') {
                    for (j = 0; j < slashes / 2; j++)
                        tok[ti++] = L'\\';
                    if (slashes & 1) {
                        tok[ti++] = L'"';
                        p++;
                    } else {
                        inq = !inq;
                        p++;
                    }
                    continue;
                }
                for (j = 0; j < slashes; j++)
                    tok[ti++] = L'\\';
                continue;
            }
            if (*p == L'"') {
                inq = !inq;
                p++;
                continue;
            }
            if (!inq && (*p == L' ' || *p == L'\t'))
                break;
            tok[ti++] = *p;
            p++;
        }
        tok[ti] = 0;

        if (argc + 1 >= cap) {
            wchar_t **n;
            int newCap = cap * 2;
            n = (wchar_t **)realloc(argv, (size_t)newCap * sizeof(wchar_t *));
            if (!n) {
                free(tok);
                goto fail;
            }
            argv = n;
            cap = newCap;
        }
        argv[argc++] = tok;
    }

    argv[argc] = NULL;
    *argcOut = argc;
    return argv;

fail:
    while (argc)
        free(argv[--argc]);
    free(argv);
    *argcOut = 0;
    return NULL;
}

void wmainCRTStartup(void)
{
    int argc = 0;
    wchar_t *cmd = GetCommandLineW();
    wchar_t **argv = build_argv(cmd, &argc);
    int rc;
    if (!argv) {
        static const char error[] = "error: cannot allocate command-line arguments\r\n";
        DWORD written;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), error, sizeof(error) - 1,
                  &written, NULL);
        ExitProcess(5);
        return;
    }
    rc = wmain(argc, argv);
    if (!uoutput_ok())
        rc = 5;
    while (argc)
        free(argv[--argc]);
    free(argv);
    ExitProcess((UINT)rc);
}

/*
 * ntrt.c - Minimal native runtime (heap, block ops, output) for the boot app.
 *
 * All memory comes from a private native heap via RtlAllocateHeap; output goes
 * to the boot-time display via NtDisplayString. These are the only services
 * bootfiles.c and boot_native.c need from "libc".
 */
#include "ntrt.h"

#define NOINLINE_PATTERN __attribute__((optimize("no-tree-loop-distribute-patterns")))

static PVOID g_heap;
static int g_outputFailed;

int noutput_ok(void)
{
    return !g_outputFailed;
}

NTSTATUS ncomplete(HANDLE h, NTSTATUS status, IO_STATUS_BLOCK *io)
{
    if (status == STATUS_PENDING) {
        NTSTATUS wait = NtWaitForSingleObject(h, 0, NULL);
        return wait == STATUS_SUCCESS ? io->Status : wait;
    }
    return status;
}

int nrt_init(void)
{
    g_outputFailed = 0;
    g_heap = RtlCreateHeap(2, NULL, 0, 0, NULL, NULL);
    return g_heap != NULL;
}

void *malloc(size_t size)
{
    if (size == 0)
        size = 1;
    return RtlAllocateHeap(g_heap, 0, size);
}

void free(void *ptr)
{
    if (ptr)
        RtlFreeHeap(g_heap, 0, ptr);
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr)
        return malloc(size);
    if (!size) {
        free(ptr);
        return NULL;
    }
    return RtlReAllocateHeap(g_heap, 0, ptr, size);
}

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

NOINLINE_PATTERN void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0)
        return dst;
    if (d < s)
        while (n--)
            *d++ = *s++;
    else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
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

size_t wcslen(const wchar_t *s)
{
    const wchar_t *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (int)(*a - *b) : 0;
}

void nout(const wchar_t *s)
{
    static wchar_t line[300];
    UNICODE_STRING u;
    size_t n = wcslen(s);
    size_t i;

    if (n > 296)
        n = 296;
    for (i = 0; i < n; i++)
        line[i] = s[i];
    line[n++] = L'\r';
    line[n++] = L'\n';
    u.Buffer = line;
    u.Length = (USHORT)(n * sizeof(wchar_t));
    u.MaximumLength = (USHORT)(sizeof(line));
    if (NtDisplayString(&u) != STATUS_SUCCESS)
        g_outputFailed = 1;
}

void nout_ascii(const char *s)
{
    wchar_t w[300];
    size_t i = 0;
    while (s[i] && i < 298) {
        w[i] = (wchar_t)(unsigned char)s[i];
        i++;
    }
    w[i] = 0;
    nout(w);
}

void nout_status(const wchar_t *label, NTSTATUS status)
{
    static const wchar_t digits[] = L"0123456789ABCDEF";
    wchar_t line[200];
    size_t i = 0, k;
    while (label[i] && i < 180) {
        line[i] = label[i];
        i++;
    }
    line[i++] = L' ';
    line[i++] = L'0';
    line[i++] = L'x';
    for (k = 0; k < 8; k++)
        line[i++] = digits[((ULONG)status >> (28 - 4 * k)) & 15];
    line[i] = 0;
    nout(line);
}

void nout_count(const wchar_t *label, unsigned long long count)
{
    wchar_t line[200], number[20];
    size_t i = 0, n = 0;
    while (label[i] && i < 175) {
        line[i] = label[i];
        i++;
    }
    line[i++] = L' ';
    do {
        number[n++] = (wchar_t)(L'0' + count % 10);
        count /= 10;
    } while (count);
    while (n)
        line[i++] = number[--n];
    line[i] = 0;
    nout(line);
}

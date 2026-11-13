/*
 * nostd.h - Minimal freestanding C-runtime replacement.
 *
 * This tool is linked with -nostdlib so that the resulting binary depends only
 * on KERNEL32.dll (plus libgcc helpers). That keeps it genuinely portable and
 * runnable on Windows NT 4.0, whose system libraries do not provide the modern
 * Universal CRT (api-ms-win-crt-*.dll) that the host MinGW toolchain links by
 * default. The small set of C-library functions the code relies on is provided
 * here on top of the Win32 heap and simple hand-written routines.
 */
#ifndef HSDEFRAG_NOSTD_H
#define HSDEFRAG_NOSTD_H

#include <stddef.h>

/* Memory (backed by the process heap). */
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

/* Block operations (also used implicitly by the compiler). */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memmove(void *dst, const void *src, size_t n);

/* Wide-string helpers. */
size_t   wcslen(const wchar_t *s);
wchar_t *wcscpy(wchar_t *dst, const wchar_t *src);
wchar_t *wcscat(wchar_t *dst, const wchar_t *src);
int      wcscmp(const wchar_t *a, const wchar_t *b);

#endif /* HSDEFRAG_NOSTD_H */

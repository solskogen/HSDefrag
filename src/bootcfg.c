/*
 * bootcfg.c - Parse and edit the BootExecute MULTI_SZ for the native helper.
 *
 * Shared by the scheduler (Win32) and the native helper. BootExecute also
 * holds "autocheck autochk *", which must survive every edit, so only
 * entries whose program is hsdefrgb[.exe] (bare, quoted or path-qualified)
 * and whose drive matches are removed; everything else is copied byte for
 * byte. A malformed list is refused instead of rewritten, because rewriting
 * a value we do not understand could drop a command the system needs.
 */
#include "bootcfg.h"

void *malloc(size_t);
void free(void *);

static wchar_t lower(wchar_t c)
{
    return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c;
}

static int space(wchar_t c)
{
    return c == L' ' || c == L'\t';
}

static size_t token(const wchar_t *s, size_t n, size_t *at, size_t *start)
{
    while (*at < n && space(s[*at]))
        ++*at;
    *start = *at;
    while (*at < n && !space(s[*at]) && s[*at])
        ++*at;
    return *at - *start;
}

static int program(const wchar_t *s, size_t n)
{
    static const wchar_t exe[] = L"hsdefrgb.exe";
    size_t i;
    if (n != 8 && n != 12)
        return 0;
    for (i = 0; i < n; i++)
        if (lower(s[i]) != exe[i])
            return 0;
    return 1;
}

static wchar_t drive_token(const wchar_t *s, size_t n, size_t *at)
{
    size_t start, len;
    len = token(s, n, at, &start);
    if (len != 2 || s[start + 1] != L':' ||
        lower(s[start]) < L'a' || lower(s[start]) > L'z')
        return 0;
    return lower(s[start]) - L'a' + L'A';
}

static int native_program(const wchar_t *s, size_t n, size_t *at);

static wchar_t entry_drive(const wchar_t *s, size_t n, size_t *at)
{
    *at = 0;
    return native_program(s, n, at) ? drive_token(s, n, at) : 0;
}

static int native_program(const wchar_t *s, size_t n, size_t *at)
{
    size_t start, end, base, i;
    while (*at < n && space(s[*at]))
        ++*at;
    if (*at == n)
        return 0;
    if (s[*at] == L'"') {
        start = ++*at;
        while (*at < n && s[*at] && s[*at] != L'"')
            ++*at;
        end = *at;
        if (*at == n || s[*at] != L'"')
            return 0;
        ++*at;
        if (*at < n && !space(s[*at]))
            return 0;
    } else {
        token(s, n, at, &start);
        end = *at;
    }
    base = start;
    for (i = start; i < end; i++)
        if (s[i] == L'\\' || s[i] == L'/')
            base = i + 1;
    return program(s + base, end - base);
}

int bootcfg_parse(const wchar_t *s, size_t n, wchar_t *drive,
                  int *dryRun, int *writeAuthorized, int *full)
{
    size_t at = 0, start, len;
    if (!drive || !dryRun || !writeAuthorized || !full)
        return 0;
    *drive = 0;
    *dryRun = *writeAuthorized = *full = 0;
    if (!s || !native_program(s, n, &at))
        return 0;
    *drive = drive_token(s, n, &at);
    if (!*drive)
        return 0;
    while ((len = token(s, n, &at, &start)) != 0) {
        if (len != 2 || (s[start] != L'/' && s[start] != L'-'))
            return 0;
        if (lower(s[start + 1]) == L'n' && !*dryRun)
            *dryRun = 1;
        else if (lower(s[start + 1]) == L'x' && !*writeAuthorized)
            *writeAuthorized = 1;
        else if (lower(s[start + 1]) == L'f' && !*full)
            *full = 1;
        else
            return 0;
    }
    return at == n;
}

wchar_t *bootcfg_edit(const wchar_t *data, size_t chars, wchar_t drive,
                      const wchar_t *add, size_t *outChars)
{
    size_t i = 0, used = 0, extra = 0;
    wchar_t *out;
    if (!outChars)
        return NULL;
    *outChars = 0;
    if (drive >= L'a' && drive <= L'z')
        drive -= L'a' - L'A';
    if (drive < L'A' || drive > L'Z' || !data || chars < 2 || chars > 524288 ||
        data[chars - 1] || data[chars - 2])
        return NULL;
    while (add && add[extra]) {
        if (++extra > 1024)
            return NULL;
    }
    out = malloc((chars + extra + 2) * sizeof(*out));
    if (!out)
        return NULL;
    while (i < chars && data[i]) {
        size_t start = i, at, k;
        while (i < chars && data[i])
            i++;
        if (i == chars) {
            free(out);
            return NULL;
        }
        if (entry_drive(data + start, i - start, &at) != drive)
            for (k = start; k <= i; k++)
                out[used++] = data[k];
        i++;
    }
    while (i < chars)
        if (data[i++]) {
            free(out);
            return NULL;
        }
    if (extra) {
        for (i = 0; i < extra; i++)
            out[used++] = add[i];
        out[used++] = 0;
    }
    if (!used)
        out[used++] = 0;
    out[used++] = 0;
    *outChars = used;
    return out;
}

/*
 * util.c - Error reporting, console output, and a small wide-printf.
 *
 * Output goes to the Win32 console via WriteConsoleW; when stdout/stderr are
 * redirected to a file or pipe, it falls back to UTF-8 via WriteFile. No C
 * stdio is used, keeping the binary free of any CRT DLL dependency.
 */
#include "util.h"

int g_verbose = 0;

DWORD   g_osMajor = 0;
DWORD   g_osMinor = 0;
DWORD   g_osBuild = 0;
wchar_t g_osCsd[128];

/* ------------------------------------------------------------- formatting */

static void put_ch(wchar_t *buf, size_t cap, size_t *pos, wchar_t c)
{
    if (*pos + 1 < cap)
        buf[*pos] = c;
    (*pos)++;
}

static void put_padded(wchar_t *buf, size_t cap, size_t *pos,
                       const wchar_t *s, size_t len, int width, int leftAlign,
                       int zeroPad)
{
    int pad = (width > (int)len) ? (width - (int)len) : 0;
    int i;
    if (!leftAlign)
        for (i = 0; i < pad; i++)
            put_ch(buf, cap, pos, zeroPad ? L'0' : L' ');
    {
        size_t k;
        for (k = 0; k < len; k++)
            put_ch(buf, cap, pos, s[k]);
    }
    if (leftAlign)
        for (i = 0; i < pad; i++)
            put_ch(buf, cap, pos, L' ');
}

static int utoa_w(unsigned long long v, wchar_t *tmp)
{
    wchar_t rev[32];
    int n = 0, i;
    if (v == 0)
        rev[n++] = L'0';
    while (v > 0) {
        rev[n++] = (wchar_t)(L'0' + (int)(v % 10));
        v /= 10;
    }
    for (i = 0; i < n; i++)
        tmp[i] = rev[n - 1 - i];
    tmp[n] = 0;
    return n;
}

static int xtoa_w(unsigned long long v, wchar_t *tmp, int upper)
{
    const wchar_t *digits = upper ? L"0123456789ABCDEF" : L"0123456789abcdef";
    wchar_t rev[32];
    int n = 0, i;
    if (v == 0)
        rev[n++] = L'0';
    while (v > 0) {
        rev[n++] = digits[v & 15];
        v >>= 4;
    }
    for (i = 0; i < n; i++)
        tmp[i] = rev[n - 1 - i];
    tmp[n] = 0;
    return n;
}

static int itoa_w(long long v, wchar_t *tmp)
{
    int n = 0;
    unsigned long long mag;
    if (v < 0) {
        tmp[n++] = L'-';
        mag = (unsigned long long)(-(v + 1)) + 1ULL;
    } else {
        mag = (unsigned long long)v;
    }
    n += utoa_w(mag, tmp + n);
    return n;
}


int u_vsnwprintf(wchar_t *buf, size_t cap, const wchar_t *fmt, va_list ap)
{
    size_t pos = 0;
    const wchar_t *f = fmt;

    while (*f) {
        if (*f != L'%') {
            put_ch(buf, cap, &pos, *f++);
            continue;
        }
        f++; /* skip '%' */

        {
            int leftAlign = 0;
            int zeroPad = 0;
            int width = 0;
            int lcount = 0;
            wchar_t spec;
            wchar_t tmp[64];

            while (*f == L'-' || *f == L'0') {
                if (*f == L'-')
                    leftAlign = 1;
                else
                    zeroPad = 1;
                f++;
            }
            while (*f >= L'0' && *f <= L'9') {
                width = width * 10 + (*f - L'0');
                f++;
            }
            if (*f == L'.') { /* a precision is accepted and ignored */
                f++;
                while (*f >= L'0' && *f <= L'9')
                    f++;
            }
            while (*f == L'l' || *f == L'h') {
                if (*f == L'l')
                    lcount++;
                f++;
            }
            spec = *f ? *f++ : 0;

            switch (spec) {
            case L's': {
                const wchar_t *s = va_arg(ap, const wchar_t *);
                if (!s)
                    s = L"(null)";
                put_padded(buf, cap, &pos, s, wcslen(s), width, leftAlign, 0);
                break;
            }
            case L'c': {
                wchar_t c = (wchar_t)va_arg(ap, int);
                tmp[0] = c;
                put_padded(buf, cap, &pos, tmp, 1, width, leftAlign, 0);
                break;
            }
            case L'd':
            case L'i': {
                long long v;
                if (lcount >= 2)
                    v = va_arg(ap, long long);
                else if (lcount == 1)
                    v = va_arg(ap, long);
                else
                    v = va_arg(ap, int);
                itoa_w(v, tmp);
                put_padded(buf, cap, &pos, tmp, wcslen(tmp), width, leftAlign,
                           zeroPad && v >= 0);
                break;
            }
            case L'u':
            case L'x':
            case L'X': {
                unsigned long long v;
                if (lcount >= 2)
                    v = va_arg(ap, unsigned long long);
                else if (lcount == 1)
                    v = va_arg(ap, unsigned long);
                else
                    v = va_arg(ap, unsigned int);
                if (spec == L'u')
                    utoa_w(v, tmp);
                else
                    xtoa_w(v, tmp, spec == L'X');
                put_padded(buf, cap, &pos, tmp, wcslen(tmp), width, leftAlign,
                           zeroPad);
                break;
            }
            case L'%':
                put_ch(buf, cap, &pos, L'%');
                break;
            case 0:
                break;
            default:
                put_ch(buf, cap, &pos, L'%');
                put_ch(buf, cap, &pos, spec);
                break;
            }
        }
    }

    if (cap > 0)
        buf[(pos < cap) ? pos : cap - 1] = 0;
    return (int)pos;
}

int u_snwprintf(wchar_t *buf, size_t cap, const wchar_t *fmt, ...)
{
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = u_vsnwprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

/* --------------------------------------------------------------- output */

static int g_outputError;

int uoutput_ok(void)
{
    return !g_outputError;
}

static void output_error(void)
{
    static const char msg[] = "error: diagnostic output failed; stopping\r\n";
    DWORD written;
    if (!g_outputError) {
        g_outputError = 1;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, sizeof(msg) - 1,
                  &written, NULL);
    }
}

static void write_std(DWORD stdHandle, const wchar_t *s, int len)
{
    HANDLE h = GetStdHandle(stdHandle);
    DWORD written = 0, mode;
    int bytes, done;
    char *tmp;
    UINT cp;

    if (len <= 0)
        return;
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
        output_error();
        return;
    }
    if (GetConsoleMode(h, &mode) && !uos_is_9x()) {
        for (done = 0; done < len; done += (int)written) {
            DWORD want = (DWORD)(len - done);
            if (want > 4096)
                want = 4096;
            if (!WriteConsoleW(h, s + done, want, &written, NULL) ||
                !written || written > want) {
                output_error();
                return;
            }
        }
        return;
    }
    /* Windows 95's WideCharToMultiByte does not support CP_UTF8; use the OEM
     * code page there (all of this tool's output is ASCII, so it is lossless).
     * On NT, UTF-8 is used so redirected output keeps any non-ASCII text. */
    cp = uos_is_9x() ? CP_OEMCP : CP_UTF8;
    bytes = WideCharToMultiByte(cp, 0, s, len, NULL, 0, NULL, NULL);
    if (bytes <= 0) {
        output_error();
        return;
    }
    tmp = (char *)malloc((size_t)bytes);
    if (!tmp) {
        output_error();
        return;
    }
    if (WideCharToMultiByte(cp, 0, s, len, tmp, bytes, NULL, NULL) != bytes) {
        output_error();
        free(tmp);
        return;
    }
    for (done = 0; done < bytes; done += (int)written) {
        if (!WriteFile(h, tmp + done, (DWORD)(bytes - done), &written, NULL) ||
            !written || written > (DWORD)(bytes - done)) {
            output_error();
            break;
        }
    }
    free(tmp);
}

static void vout(DWORD stdHandle, const wchar_t *fmt, va_list ap)
{
    wchar_t buf[4096], *large;
    va_list copy;
    int n;
    va_copy(copy, ap);
    n = u_vsnwprintf(buf, 4096, fmt, copy);
    va_end(copy);
    if (n < 4096) {
        write_std(stdHandle, buf, n);
        return;
    }
    large = malloc(((size_t)n + 1) * sizeof(*large));
    if (!large) {
        output_error();
        return;
    }
    u_vsnwprintf(large, (size_t)n + 1, fmt, ap);
    write_std(stdHandle, large, n);
    free(large);
}

void uinfo(const wchar_t *fmt, ...)
{
    va_list ap;
    uprogress_clear();
    va_start(ap, fmt);
    vout(STD_OUTPUT_HANDLE, fmt, ap);
    va_end(ap);
}

void uverbose(const wchar_t *fmt, ...)
{
    va_list ap;
    if (!g_verbose)
        return;
    uprogress_clear();
    va_start(ap, fmt);
    vout(STD_OUTPUT_HANDLE, fmt, ap);
    va_end(ap);
}

void uerror(const wchar_t *fmt, ...)
{
    va_list ap;
    uprogress_clear();
    write_std(STD_ERROR_HANDLE, L"error: ", 7);
    va_start(ap, fmt);
    vout(STD_ERROR_HANDLE, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------- progress */

static int    g_progressShown;
static DWORD  g_progressTick;

static int stdout_is_console(void)
{
    static int known = 0, isCon = 0;
    if (!known) {
        DWORD mode;
        isCon = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
        known = 1;
    }
    return isCon;
}

void uprogress(const wchar_t *fmt, ...)
{
    wchar_t line[96];
    va_list ap;
    DWORD now;
    int n;

    if (g_verbose || !stdout_is_console())
        return;
    now = GetTickCount();
    if (g_progressShown && now - g_progressTick < 250)
        return;
    g_progressTick = now;

    line[0] = L'\r';
    va_start(ap, fmt);
    n = 1 + u_vsnwprintf(line + 1, 78, fmt, ap);
    va_end(ap);
    if (n > 78)
        n = 78;
    while (n < 78)
        line[n++] = L' ';
    write_std(STD_OUTPUT_HANDLE, line, n);
    g_progressShown = 1;
}

void uprogress_clear(void)
{
    wchar_t line[80];
    int n = 0;

    if (!g_progressShown)
        return;
    line[n++] = L'\r';
    while (n < 78)
        line[n++] = L' ';
    line[n++] = L'\r';
    write_std(STD_OUTPUT_HANDLE, line, n);
    g_progressShown = 0;
}

/* ------------------------------------------------------------ OS version */

int uos_is_9x(void)
{
    static int known = 0, is9x = 0;

    if (!known) {
        /* GetVersion works on both NT and 9x; the high bit is set on the
         * non-NT (Win9x / Win32s) platforms. Unlike GetVersionExW, this never
         * hits a missing-stub on Windows 95. */
        is9x = (GetVersion() & 0x80000000u) != 0;
        known = 1;
    }
    return is9x;
}

void uos_detect(void)
{
    g_osCsd[0] = 0;

    if (uos_is_9x()) {
        /* The wide GetVersionExW is a no-op stub on Windows 9x; use the ANSI
         * entry point and widen the (ASCII) service-pack text ourselves. */
        OSVERSIONINFOA vi;
        int i;

        memset(&vi, 0, sizeof(vi));
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (!GetVersionExA(&vi))
            return;
        g_osMajor = vi.dwMajorVersion;
        g_osMinor = vi.dwMinorVersion;
        g_osBuild = vi.dwBuildNumber & 0xFFFF;
        vi.szCSDVersion[sizeof(vi.szCSDVersion) - 1] = 0;
        for (i = 0; vi.szCSDVersion[i] && i < 127; i++)
            g_osCsd[i] = (wchar_t)(unsigned char)vi.szCSDVersion[i];
        g_osCsd[i] = 0;
    } else {
        OSVERSIONINFOW vi;

        memset(&vi, 0, sizeof(vi));
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (!GetVersionExW(&vi))
            return;
        g_osMajor = vi.dwMajorVersion;
        g_osMinor = vi.dwMinorVersion;
        g_osBuild = vi.dwBuildNumber & 0xFFFF;
        vi.szCSDVersion[127] = 0;
        wcscpy(g_osCsd, vi.szCSDVersion);
    }
}

void uos_print(void)
{
    if (g_osMajor == 0) {
        uinfo(L"Windows version unknown\n");
        return;
    }
    if (uos_is_9x()) {
        uinfo(L"Windows %lu.%lu (9x)%ls%ls\n", (unsigned long)g_osMajor,
              (unsigned long)g_osMinor, g_osCsd[0] ? L", " : L"", g_osCsd);
        return;
    }
    uinfo(L"Windows NT %lu.%lu build %lu%ls%ls\n", (unsigned long)g_osMajor,
          (unsigned long)g_osMinor, (unsigned long)g_osBuild,
          g_osCsd[0] ? L", " : L"", g_osCsd);
}

void uwin32_error(const wchar_t *ctx, DWORD err)
{
    wchar_t msg[256];

    uwin32_msg(err, msg, 256);
    if (msg[0])
        uerror(L"%ls: %ls (code %lu)\n", ctx, msg, (unsigned long)err);
    else
        uerror(L"%ls (code %lu)\n", ctx, (unsigned long)err);
}

const wchar_t *uwin32_msg(DWORD err, wchar_t *buf, size_t cap)
{
    wchar_t *msg = NULL;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, err, 0, (LPWSTR)&msg, 0, NULL);

    if (cap == 0)
        return buf;
    buf[0] = 0;
    if (n && msg) {
        while (n > 0 && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n' ||
                         msg[n - 1] == L'.' || msg[n - 1] == L' ')) {
            msg[--n] = L'\0';
        }
        u_snwprintf(buf, cap, L"%ls", msg);
    }
    if (msg)
        LocalFree(msg);
    return buf;
}

const wchar_t *uwin32_str(DWORD err, wchar_t *buf, size_t cap)
{
    wchar_t msg[200];

    uwin32_msg(err, msg, 200);
    if (msg[0])
        u_snwprintf(buf, cap, L"%ls (code %lu)", msg, (unsigned long)err);
    else
        u_snwprintf(buf, cap, L"code %lu", (unsigned long)err);
    return buf;
}

int uos_at_least(DWORD major, DWORD minor)
{
    return g_osMajor > major || (g_osMajor == major && g_osMinor >= minor);
}

int uos_nt4(void)
{
    return !uos_at_least(5, 0);
}

const wchar_t *uformat_bytes(unsigned long long bytes, wchar_t *buf, size_t cap)
{
    /* "12.3 MB" in integer arithmetic, so the program needs no FPU (a 386
     * or 486SX without one runs it too); the same rounding as the FAT
     * report (fatreport.c). */
    static const wchar_t *const unit[] = { L"B", L"KB", L"MB", L"GB" };
    unsigned long long div = 1, tenths;
    int u = 0;

    while (u < 3 && bytes >= div * 1024) {
        div *= 1024;
        u++;
    }
    tenths = (bytes * 10 + div / 2) / div;
    u_snwprintf(buf, cap, L"%llu.%llu %ls", tenths / 10, tenths % 10, unit[u]);
    return buf;
}

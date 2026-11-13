/*
 * util.h - Error reporting, console output, and a small wide-printf.
 */
#ifndef HSDEFRAG_UTIL_H
#define HSDEFRAG_UTIL_H

#include "compat.h"
#include "nostd.h"
#include <stdarg.h>

/* Global verbose flag, set from the command line. */
extern int g_verbose;

/* False after the first failed output operation; callers must stop new moves. */
int uoutput_ok(void);

/* Windows version, filled in by uos_detect() (all zero if unknown). */
extern DWORD   g_osMajor;
extern DWORD   g_osMinor;
extern DWORD   g_osBuild;
extern wchar_t g_osCsd[128];   /* e.g. "Service Pack 6" */

/* Detect the running Windows version into the g_os* globals. */
void uos_detect(void);

/* TRUE on Windows 95/98/Me (the non-NT, FAT-only platforms). Safe to call
 * before uos_detect(); the result is cached. */
int uos_is_9x(void);

/* Print a one-line description of the running Windows version. */
void uos_print(void);

/*
 * Show a transient one-line status ("\r"-overwritten) on an interactive
 * console. Throttled; does nothing in verbose mode or when stdout is
 * redirected. uprogress_clear() erases it before normal output resumes.
 */
void uprogress(const wchar_t *fmt, ...);
void uprogress_clear(void);

/* Print a formatted message to stdout. */
void uinfo(const wchar_t *fmt, ...);

/* Print a formatted message to stdout only when verbose mode is enabled. */
void uverbose(const wchar_t *fmt, ...);

/* Print a formatted message to stderr, prefixed with "error: ". */
void uerror(const wchar_t *fmt, ...);

/*
 * Print "<ctx>: <system message> (code N)" to stderr for the given Win32 error
 * code (as returned by GetLastError()).
 */
void uwin32_error(const wchar_t *ctx, DWORD err);

/* Copy the system message text for a Win32 error into buf ("" if none). */
const wchar_t *uwin32_msg(DWORD err, wchar_t *buf, size_t cap);

/* Format "<system message> (code N)", or just "code N", into buf. */
const wchar_t *uwin32_str(DWORD err, wchar_t *buf, size_t cap);

/* TRUE if the running Windows version is at least major.minor. */
int uos_at_least(DWORD major, DWORD minor);

/*
 * TRUE on NT 4.0 or older, or when the version is unknown. Its NTFS is not
 * trusted with handles to metadata files or to files held open exclusively by
 * the system, so such opens are not attempted there.
 */
int uos_nt4(void);

/*
 * Minimal snprintf-style formatter for wide strings. Supports the subset of
 * conversions used by this program: %ls %lc %c %d %i %u %x %X (with l/ll
 * length modifiers) %.Nf %%, with optional '-' / '0' flags and field width.
 * Always NUL-terminates when cap > 0. Returns the number of characters that
 * would have been written.
 */
int u_snwprintf(wchar_t *buf, size_t cap, const wchar_t *fmt, ...);
int u_vsnwprintf(wchar_t *buf, size_t cap, const wchar_t *fmt, va_list ap);

/* Format a byte count into a human-friendly string (KB/MB/GB). */
const wchar_t *uformat_bytes(unsigned long long bytes, wchar_t *buf, size_t cap);

#endif /* HSDEFRAG_UTIL_H */

/*
 * boot9x.c - The boot-time pass on Windows 95/98.
 *
 * While Windows runs, its swap file, its registry and the programs and DLLs
 * it keeps open cannot move, so the live pass leaves them in place. "hsdefrag
 * C: --boot-defrag" schedules hsdefrag itself (hsdefrag.exe is an MS-DOS
 * program too, with the same FAT engine) to run from AUTOEXEC.BAT before
 * Windows starts:
 *
 *   - a copy of hsdefrag.exe goes into the Windows folder, or into the root of
 *     the boot drive if Windows lives on another drive;
 *   - the drive and its volume serial number go into HSDEFRAG.JOB in the root
 *     of the boot drive;
 *   - AUTOEXEC.BAT gets the line that starts hsdefrag while the job exists.
 *
 * The MS-DOS side deletes the job file before it begins, so the pass runs
 * once and the line stays idle afterwards; its report goes to HSDEFRAG.LOG
 * next to the job. It must not edit AUTOEXEC.BAT itself: COMMAND.COM is
 * reading it.
 * Windows Me does not run real-mode programs from AUTOEXEC.BAT, so scheduling
 * is refused there. Only the ANSI functions of KERNEL32 are used, as the wide
 * ones are stubs on Windows 9x.
 */
#include "compat.h"
#include "util.h"
#include "bootjob.h"
#include "boot9x.h"
#include "fatwin9x.h"

#define MAX_TEXT (256u * 1024u)       /* AUTOEXEC.BAT, the job and the log */
#define MAX_EXE  (16u * 1024u * 1024u) /* hsdefrag.exe                     */
#define NO_ATTR  ((DWORD)0xFFFFFFFFu)
#define JOB_CAP  1024                 /* a job with all 26 drives fits     */

typedef struct {
    char boot;                        /* the drive Windows starts from     */
    char job[16];                     /* C:\HSDEFRAG.JOB                   */
    char log[16];                     /* C:\HSDEFRAG.LOG                   */
    char autoexec[16];                /* C:\AUTOEXEC.BAT                   */
    char temp[16];                    /* C:\AUTOEXEC.$HS                   */
    char backup[16];                  /* C:\AUTOEXEC.HS0                   */
    char helper[MAX_PATH];            /* where hsdefrag.exe is copied      */
    char line[BJ_MAX_LINE + 1];       /* the AUTOEXEC.BAT line             */
} paths9x;

static int up(int c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

/* out = "X:\" name */
static void at_root(char *out, int drive, const char *name)
{
    out[0] = (char)up(drive);
    out[1] = ':';
    out[2] = '\\';
    lstrcpyA(out + 3, name);
}

static const wchar_t *wide(const char *s, wchar_t *buf, int cap)
{
    if (!MultiByteToWideChar(CP_ACP, 0, s, -1, buf, cap))
        buf[0] = 0;
    return buf;
}

/* FormatMessageW is a stub on Windows 9x: name the usual errors here. */
static const wchar_t *err_text(DWORD e, wchar_t *buf, size_t cap)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND:      return L"file not found";
    case ERROR_PATH_NOT_FOUND:      return L"path not found";
    case ERROR_ACCESS_DENIED:       return L"access denied";
    case ERROR_WRITE_PROTECT:       return L"write-protected";
    case ERROR_SHARING_VIOLATION:   return L"in use by another program";
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_DISK_FULL:           return L"disk full";
    default:                        return uwin32_str(e, buf, cap);
    }
}

static void file_error(const wchar_t *what, const char *path, DWORD e)
{
    wchar_t wp[MAX_PATH], msg[160];

    uerror(L"cannot %ls %ls: %ls\n", what, wide(path, wp, MAX_PATH),
           err_text(e, msg, 160));
}

static int exists(const char *path)
{
    return GetFileAttributesA(path) != NO_ATTR;
}

/*
 * Read a whole file of at most cap bytes into a new NUL-terminated buffer.
 * Returns 1, 0 if the file does not exist, or -1 after printing why it could
 * not be read.
 */
static int read_file(const char *path, DWORD cap, char **data, DWORD *len)
{
    HANDLE h;
    DWORD size, high = 0, got = 0, e;
    char *buf;

    *data = NULL;
    *len = 0;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            return 0;
        file_error(L"open", path, e);
        return -1;
    }
    size = GetFileSize(h, &high);
    if (size == NO_ATTR || high || size > cap) {
        e = size == NO_ATTR ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
        CloseHandle(h);
        file_error(L"read", path, e);
        return -1;
    }
    buf = (char *)malloc(size + 1);
    if (!buf) {
        CloseHandle(h);
        file_error(L"read", path, ERROR_NOT_ENOUGH_MEMORY);
        return -1;
    }
    if (size && (!ReadFile(h, buf, size, &got, NULL) || got != size)) {
        e = GetLastError();
        CloseHandle(h);
        free(buf);
        file_error(L"read", path, e ? e : ERROR_HANDLE_EOF);
        return -1;
    }
    if (!CloseHandle(h)) {
        e = GetLastError();
        free(buf);
        file_error(L"close", path, e);
        return -1;
    }
    buf[size] = 0;
    *data = buf;
    *len = size;
    return 1;
}

static int read_all(const char *path, char **data, DWORD *len)
{
    return read_file(path, MAX_TEXT, data, len);
}

/* The running program, which is the MS-DOS program as well. Returns 1, or 0
 * after printing why it cannot be read. */
static int load_self(BYTE **data, DWORD *len)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    int r;

    *data = NULL;
    *len = 0;
    if (n == 0 || n >= MAX_PATH) {
        uerror(L"cannot find the file hsdefrag runs from\n");
        return 0;
    }
    r = read_file(path, MAX_EXE, (char **)data, len);
    if (r == 0)
        file_error(L"open", path, ERROR_FILE_NOT_FOUND);
    return r > 0;
}

/* Never truncate an existing file while preparing a replacement. */
static int write_new(const char *path, const void *data, DWORD len)
{
    HANDLE h;
    DWORD put = 0, e = 0;
    int ok;

    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        file_error(L"create", path, GetLastError());
        return 0;
    }
    ok = len == 0 || (WriteFile(h, data, len, &put, NULL) && put == len);
    if (!ok)
        e = GetLastError();
    if (ok && !FlushFileBuffers(h)) {
        ok = 0;
        e = GetLastError();
    }
    if (!CloseHandle(h) && ok) {
        ok = 0;
        e = GetLastError();
    }
    if (!ok) {
        file_error(L"write", path, e ? e : ERROR_HANDLE_DISK_FULL);
        if (!DeleteFileA(path))
            file_error(L"remove incomplete file", path, GetLastError());
    }
    return ok;
}

/* 1 if the file holds exactly data[len]. */
static int same_file(const char *path, const BYTE *data, DWORD len)
{
    BYTE buf[4096];
    DWORD pos = 0, got, want, high = 0;
    HANDLE h;
    int same;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    same = GetFileSize(h, &high) == len && high == 0;
    while (same && pos < len) {
        want = len - pos < sizeof(buf) ? len - pos : (DWORD)sizeof(buf);
        if (!ReadFile(h, buf, want, &got, NULL) || got != want ||
            memcmp(buf, data + pos, want) != 0)
            same = 0;
        pos += want;
    }
    if (!CloseHandle(h)) {
        file_error(L"close", path, GetLastError());
        return 0;
    }
    return same;
}

/* Win9x has no atomic replace. Keep the old file under a recovery name until
 * the flushed, verified replacement has reached its final name. */
static int replace_file(const char *path, const char *temp, const char *backup,
                         const void *data, DWORD len)
{
    DWORD attr = GetFileAttributesA(path);
    DWORD e = GetLastError();
    int had = attr != NO_ATTR;
    wchar_t wp[MAX_PATH];

    if ((had && (attr & FILE_ATTRIBUTE_DIRECTORY)) ||
        (!had && e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) ||
        exists(temp) || exists(backup)) {
        uerror(L"cannot replace %ls: check for an existing temporary or "
               L"recovery file before retrying\n", wide(path, wp, MAX_PATH));
        return 0;
    }
    if (!write_new(temp, data, len))
        return 0;
    if (!same_file(temp, (const BYTE *)data, len)) {
        file_error(L"verify", temp, ERROR_CRC);
        DeleteFileA(temp);
        return 0;
    }
    if (had) {
        if (!SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL) ||
            !MoveFileA(path, backup)) {
            e = GetLastError();
            SetFileAttributesA(path, attr);
            file_error(L"preserve", path, e);
            DeleteFileA(temp);
            return 0;
        }
    }
    if (!MoveFileA(temp, path)) {
        e = GetLastError();
        file_error(L"install replacement for", path, e);
        if (had) {
            if (!MoveFileA(backup, path)) {
                file_error(L"restore original from", backup, GetLastError());
                uerror(L"Restore the recovery file before restarting.\n");
                return 0;
            }
            SetFileAttributesA(path, attr);
        }
        DeleteFileA(temp);
        return 0;
    }
    if (had && !SetFileAttributesA(path, attr)) {
        file_error(L"restore attributes of", path, GetLastError());
        uerror(L"The previous copy was kept at %ls.\n",
               wide(backup, wp, MAX_PATH));
        return 0;
    }
    if (had && !DeleteFileA(backup)) {
        file_error(L"remove recovery file", backup, GetLastError());
        uerror(L"Remove %ls before retrying.\n",
               wide(backup, wp, MAX_PATH));
        return 0;
    }
    return 1;
}

static int write_all(const char *path, const void *data, DWORD len)
{
    char temp[MAX_PATH], backup[MAX_PATH];
    size_t n = (size_t)lstrlenA(path);

    if (n + 5 > sizeof(temp)) {
        file_error(L"replace", path, ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    memcpy(temp, path, n);
    memcpy(backup, path, n);
    memcpy(temp + n, ".HST", 5);
    memcpy(backup + n, ".HSB", 5);
    return replace_file(path, temp, backup, data, len);
}

/* The drive Windows starts from: the first fixed drive with IO.SYS and
 * MSDOS.SYS in its root (C: but for unusual setups). */
static int boot_drive(void)
{
    char p[16];
    int d;

    for (d = 'C'; d <= 'Z'; d++) {
        at_root(p, d, "");
        if (GetDriveTypeA(p) != DRIVE_FIXED)
            continue;
        at_root(p, d, "IO.SYS");
        if (!exists(p))
            continue;
        at_root(p, d, "MSDOS.SYS");
        if (exists(p))
            return d;
    }
    return 0;
}

/* Returns 0, or an exit code. */
static int get_paths(paths9x *p)
{
    char win[MAX_PATH], dos[MAX_PATH];
    UINT n;
    DWORD k;

    memset(p, 0, sizeof(*p));
    p->boot = (char)boot_drive();
    if (!p->boot) {
        uerror(L"cannot find the drive Windows starts from (no fixed drive "
               L"has IO.SYS and MSDOS.SYS)\n");
        return 2;
    }
    at_root(p->job, p->boot, "HSDEFRAG.JOB");
    at_root(p->log, p->boot, "HSDEFRAG.LOG");
    at_root(p->autoexec, p->boot, "AUTOEXEC.BAT");
    at_root(p->temp, p->boot, "AUTOEXEC.$HS");
    at_root(p->backup, p->boot, "AUTOEXEC.HS0");

    /* The Windows folder if it is on the boot drive, which MS-DOS surely
     * sees under the same letter; AUTOEXEC.BAT gets its short name. */
    n = GetWindowsDirectoryA(win, MAX_PATH);
    if (n > 3 && n < MAX_PATH - 16 && up(win[0]) == p->boot &&
        win[1] == ':') {
        if (win[n - 1] == '\\')
            win[n - 1] = 0;
        k = GetShortPathNameA(win, dos, MAX_PATH);
        if (k && k < MAX_PATH - 16) {
            lstrcatA(dos, "\\HSDEFRAG.EXE");
            if (bj_line(dos, p->job, p->line, sizeof(p->line)) != BJ_ERR) {
                lstrcpyA(p->helper, win);
                lstrcatA(p->helper, "\\HSDEFRAG.EXE");
                return 0;
            }
        }
    }
    at_root(p->helper, p->boot, "HSDEFRAG.EXE");
    if (bj_line(p->helper, p->job, p->line, sizeof(p->line)) == BJ_ERR) {
        uerror(L"the AUTOEXEC.BAT line is too long\n");
        return 5;
    }
    return 0;
}

/*
 * Read the job into e[]. Returns 1 if it exists, 0 if not, 2 if it was
 * damaged (then *n is 0), or -1 if it could not be read.
 */
static int load_job(const paths9x *p, bj_entry *e, int *n, char **text,
                    DWORD *len)
{
    int r = read_all(p->job, text, len);

    *n = 0;
    if (r <= 0)
        return r;
    *n = bj_parse(*text, *len, e, BJ_MAX_DRIVES);
    if (*n < 0) {
        *n = 0;
        return 2;
    }
    return 1;
}

static int save_job(const paths9x *p, const bj_entry *e, int n)
{
    char text[JOB_CAP];
    size_t len = bj_format(e, n, text, sizeof(text));

    return len != BJ_ERR && write_all(p->job, text, (DWORD)len);
}

/* Puts the job file back as load_job found it (r: its result). */
static void restore_job(const paths9x *p, int r, const char *old, DWORD len)
{
    if (r == 1 || r == 2) {
        if (!same_file(p->job, (const BYTE *)old, len) &&
            !write_all(p->job, old, len))
            uerror(L"The previous job could not be restored. Inspect the job "
                   L"and recovery files before restarting.\n");
    } else if (exists(p->job) && !DeleteFileA(p->job)) {
        file_error(L"remove incomplete job", p->job, GetLastError());
        uerror(L"Inspect --boot-status before restarting.\n");
    }
}

/* "C: (1A2B-3C4D) D:" */
static void list_drives(const bj_entry *e, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        uinfo(L"%ls%lc:", i ? L", " : L"", (wchar_t)e[i].drive);
        if (e[i].hasSerial)
            uinfo(L" (%04lX-%04lX)", (e[i].serial >> 16) & 0xFFFFu,
                  e[i].serial & 0xFFFFu);
    }
    if (!n)
        uinfo(L"nothing");
    uinfo(L"\n");
}

/*
 * Put our line into AUTOEXEC.BAT, or take it out if line is NULL. The new
 * text goes to a temporary file that then replaces the old one (Windows 9x
 * has no MoveFileEx), keeping its attributes. AUTOEXEC.BAT is created if
 * needed but never deleted: MS-DOS asks for the date and time without one.
 * Returns 1 if the file changed, 0 if it was right already, -1 on error.
 */
static int edit_autoexec(const paths9x *p, const char *line)
{
    char *old, *text;
    DWORD len;
    size_t n, cap;
    int r;

    r = read_all(p->autoexec, &old, &len);
    if (r < 0)
        return -1;
    if (r == 0 && !line)
        return 0;
    cap = (size_t)len + BJ_MAX_LINE + 8;
    text = (char *)malloc(cap);
    if (!text) {
        free(old);
        file_error(L"edit", p->autoexec, ERROR_NOT_ENOUGH_MEMORY);
        return -1;
    }
    n = bj_autoexec_edit(old ? old : "", len, line, text, cap);
    if (n == BJ_ERR || (r && n == len && memcmp(old, text, n) == 0)) {
        free(old);
        free(text);
        if (n == BJ_ERR)
            file_error(L"edit", p->autoexec, ERROR_NOT_ENOUGH_MEMORY);
        return n == BJ_ERR ? -1 : 0;
    }
    free(old);
    if (!replace_file(p->autoexec, p->temp, p->backup, text, (DWORD)n)) {
        free(text);
        return -1;
    }
    free(text);
    return 1;
}

/* Returns 0, or an exit code; *serial is set if the boot sector has one. */
static int check_drive(wchar_t drive, bj_entry *x, wchar_t *fs, int fsCap)
{
    unsigned char bs[512];
    char root[4], name[16];
    DWORD serial, maxName, flags;
    UINT type;
    int rc;

    at_root(root, drive, "");
    type = GetDriveTypeA(root);
    if (type != DRIVE_FIXED) {
        uerror(L"%lc: is not a fixed disk; only fixed disks can be "
               L"defragmented at boot time\n", drive);
        return type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN ? 2 : 3;
    }
    if (!GetVolumeInformationA(root, NULL, 0, &serial, &maxName, &flags, name,
                               sizeof(name))) {
        file_error(L"read the volume information of", root, GetLastError());
        return 2;
    }
    name[sizeof(name) - 1] = 0;
    wide(name, fs, fsCap);
    if (up(name[0]) != 'F' || up(name[1]) != 'A' || up(name[2]) != 'T') {
        uerror(L"%lc: is a %ls volume; only FAT12/16/32 can be defragmented "
               L"at boot time\n", drive, fs);
        return 3;
    }
    rc = fat9x_boot_sector(drive, bs);
    if (rc)
        return rc;
    x->drive = (char)up(drive);
    x->hasSerial = bj_boot_serial(bs, &x->serial);
    return 0;
}

int cmd_boot9x_schedule(wchar_t drive, int dryRun)
{
    wchar_t fs[16], w1[MAX_PATH], w2[MAX_PATH];
    bj_entry e[BJ_MAX_DRIVES], x;
    paths9x p;
    char *old;
    BYTE *self;
    DWORD oldLen, size;
    int n, r, rc;

    drive = (wchar_t)up(drive);
#ifdef HSD_WINDOWS_ONLY
    /* The pass runs hsdefrag.exe itself as an MS-DOS program; a build
     * without the MS-DOS part (make windows) would install a program that
     * cannot run there. */
    (void)dryRun;
    uerror(L"This hsdefrag.exe was built for Windows only (make windows) and "
           L"has no MS-DOS part,\nso it cannot run the boot-time pass. Use "
           L"the full hsdefrag.exe to schedule it.\n");
    return 3;
#endif
    if (g_osMajor > 4 || (g_osMajor == 4 && g_osMinor >= 90)) {
        uerror(L"Windows Me does not run MS-DOS programs before it starts, "
               L"so the boot-time pass\ncannot be scheduled. Start the "
               L"computer from a Windows Me startup disk and run\n"
               L"hsdefrag.exe from it instead.\n");
        return 3;
    }
    rc = check_drive(drive, &x, fs, 16);
    if (!rc)
        rc = get_paths(&p);
    if (rc)
        return rc;
    r = load_job(&p, e, &n, &old, &oldLen);
    if (r < 0)
        return 2;
    if (r == 2)
        uinfo(L"warning: %ls is damaged and is replaced\n",
              wide(p.job, w1, MAX_PATH));
    n = bj_set(e, n, &x);

    uinfo(L"\nBoot-time pass for %lc: (%ls", (wchar_t)x.drive, fs);
    if (x.hasSerial)
        uinfo(L", serial %04lX-%04lX", (x.serial >> 16) & 0xFFFFu,
              x.serial & 0xFFFFu);
    uinfo(L")\n  program        : %ls\n", wide(p.helper, w1, MAX_PATH));
    uinfo(L"  job file       : %ls, drives ", wide(p.job, w1, MAX_PATH));
    list_drives(e, n);
    uinfo(L"  AUTOEXEC.BAT   : %ls\n  report         : %ls\n",
          wide(p.line, w1, MAX_PATH), wide(p.log, w2, MAX_PATH));
    if (dryRun) {
        free(old);
        uinfo(L"\nDry run: nothing was written. Run it without --dry-run to "
              L"schedule the pass.\n");
        return uoutput_ok() ? 0 : 5;
    }

    /* The program first and AUTOEXEC.BAT last, so the line never starts a
     * missing program. */
    if (!load_self(&self, &size)) {
        free(old);
        return 2;
    }
    if (same_file(p.helper, self, size)) {
        uverbose(L"  %ls is up to date\n", wide(p.helper, w1, MAX_PATH));
    } else if (!write_all(p.helper, self, size) ||
               !same_file(p.helper, self, size)) {
        uerror(L"%ls could not be written; %lc: is not scheduled\n",
               wide(p.helper, w1, MAX_PATH), (wchar_t)x.drive);
        free(self);
        free(old);
        return 2;
    }
    free(self);
    if (!save_job(&p, e, n)) {
        restore_job(&p, r, old, oldLen);
        free(old);
        uerror(L"the job file could not be updated; scheduling did not "
               L"complete.\nInspect --boot-status and recovery files before "
               L"restarting.\n");
        return 2;
    }
    if (edit_autoexec(&p, p.line) < 0) {
        restore_job(&p, r, old, oldLen);
        free(old);
        uerror(L"AUTOEXEC.BAT could not be updated; scheduling did not "
               L"complete.\nInspect --boot-status and recovery files before "
               L"restarting.\n");
        return 2;
    }
    free(old);
    uinfo(L"\n%lc: is defragmented the next time the computer starts, before "
          L"Windows. Hidden,\nsystem and read-only files, the swap file, the "
          L"registry and the files Windows\nkeeps open move too; only boot "
          L"files such as IO.SYS and the start of the root\nfolder stay in "
          L"place. Press ESC at the countdown to skip the pass.\n"
          L"\"hsdefrag %lc: --boot-status\" shows its report afterwards.\n",
          (wchar_t)x.drive, (wchar_t)x.drive);
    return uoutput_ok() ? 0 : 5;
}

int cmd_boot9x_cancel(wchar_t drive)
{
    wchar_t w1[MAX_PATH];
    bj_entry e[BJ_MAX_DRIVES];
    paths9x p;
    char *text;
    DWORD len;
    int n, r, had, rc;

    drive = (wchar_t)up(drive);
    rc = get_paths(&p);
    if (rc)
        return rc;
    r = load_job(&p, e, &n, &text, &len);
    free(text);
    if (r < 0)
        return 2;
    had = bj_find(e, n, (char)drive) >= 0;
    n = bj_remove(e, n, (char)drive);
    if (n > 0) {
        if (had && !save_job(&p, e, n))
            return 2;
        uinfo(had ? L"%lc: is no longer scheduled. " :
                    L"%lc: is not scheduled. ", drive);
        uinfo(L"Still scheduled: ");
        list_drives(e, n);
        return uoutput_ok() ? 0 : 5;
    }

    /* Nothing is left: remove the job and the line. */
    if (r == 2)
        uinfo(L"%ls was damaged.\n", wide(p.job, w1, MAX_PATH));
    if (r > 0) {
        SetFileAttributesA(p.job, FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileA(p.job)) {
            file_error(L"delete", p.job, GetLastError());
            return 2;
        }
    }
    r = edit_autoexec(&p, NULL);
    if (r < 0)
        return 2;
    uinfo(had ? L"The boot-time pass of %lc: is cancelled.\n" :
                L"%lc: is not scheduled.\n", drive);
    if (r > 0)
        uinfo(L"Its line was removed from %ls.\n",
              wide(p.autoexec, w1, MAX_PATH));
    if (exists(p.helper))
        uverbose(L"%ls stays; it also runs in MS-DOS mode.\n",
                 wide(p.helper, w1, MAX_PATH));
    return uoutput_ok() ? 0 : 5;
}

/* Print the report the pass left (MS-DOS text: OEM code page, CR LF). */
static void print_log(const char *text, DWORD len)
{
    wchar_t *w = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
    int n, i, end, start = 0;

    if (!w)
        return;
    n = len ? MultiByteToWideChar(CP_OEMCP, 0, text, (int)len, w, (int)len)
            : 0;
    for (i = 0; i < n && w[i] != 0x1A; i++)
        ;
    n = i;
    while (start < n) {
        for (end = start; end < n && w[end] != L'\n'; end++)
            ;
        i = end;
        if (end > start && w[end - 1] == L'\r')
            end--;
        w[end] = 0;
        uinfo(L"%ls\n", w + start);
        start = i + 1;
    }
    free(w);
}

static void show_recovery_files(const paths9x *p)
{
    char jobT[MAX_PATH], jobB[MAX_PATH], exeT[MAX_PATH], exeB[MAX_PATH];
    const char *cand[6];
    wchar_t w[MAX_PATH];
    size_t jn = (size_t)lstrlenA(p->job), hn = (size_t)lstrlenA(p->helper);
    int i, found = 0;

    cand[0] = p->temp;
    cand[1] = p->backup;
    cand[2] = cand[3] = cand[4] = cand[5] = NULL;
    if (jn + 5 <= sizeof(jobT)) {
        memcpy(jobT, p->job, jn);
        memcpy(jobB, p->job, jn);
        memcpy(jobT + jn, ".HST", 5);
        memcpy(jobB + jn, ".HSB", 5);
        cand[2] = jobT;
        cand[3] = jobB;
    }
    if (hn + 5 <= sizeof(exeT)) {
        memcpy(exeT, p->helper, hn);
        memcpy(exeB, p->helper, hn);
        memcpy(exeT + hn, ".HST", 5);
        memcpy(exeB + hn, ".HSB", 5);
        cand[4] = exeT;
        cand[5] = exeB;
    }
    for (i = 0; i < 6; i++) {
        if (!cand[i] || !exists(cand[i]))
            continue;
        if (!found)
            uinfo(L"  recovery files : ");
        uinfo(L"%ls%ls", found ? L", " : L"", wide(cand[i], w, MAX_PATH));
        found++;
    }
    if (found) {
        uinfo(L"\nwarning: temporary or recovery files remain on disk; "
              L"inspect and remove or restore them before retrying\n");
    }
}

int cmd_boot9x_status(wchar_t drive)
{
    wchar_t w1[MAX_PATH];
    bj_entry e[BJ_MAX_DRIVES];
    paths9x p;
    char *text;
    DWORD len;
    int n, r, lines, rc;

    drive = (wchar_t)up(drive);
    rc = get_paths(&p);
    if (rc)
        return rc;
    r = load_job(&p, e, &n, &text, &len);
    free(text);
    if (r < 0)
        return 2;
    if (read_all(p.autoexec, &text, &len) < 0)
        return 2;
    lines = text ? bj_autoexec_count(text, len) : 0;
    free(text);

    uinfo(L"\nBoot-time pass (started from %ls)\n",
          wide(p.autoexec, w1, MAX_PATH));
    uinfo(L"  scheduled      : ");
    if (r == 2)
        uinfo(L"nothing (%ls is damaged)\n", wide(p.job, w1, MAX_PATH));
    else
        list_drives(e, n);
    uinfo(L"  AUTOEXEC.BAT   : %ls\n",
          !lines ? L"no line for the pass" :
          n ? L"starts the pass" : L"idle line (--boot-cancel removes it)");
    uinfo(L"  program        : %ls%ls\n", wide(p.helper, w1, MAX_PATH),
          exists(p.helper) ? L"" : L" (missing)");
    show_recovery_files(&p);
    if (bj_find(e, n, (char)drive) >= 0)
        uinfo(L"%lc: is defragmented the next time the computer starts.\n",
              drive);
    else
        uinfo(L"%lc: is not scheduled.\n", drive);
    if (n && (!lines || !exists(p.helper)))
        uinfo(L"warning: the pass will not run; schedule it again with "
              L"\"hsdefrag %lc: --boot-defrag\"\n", (wchar_t)e[0].drive);

    r = read_all(p.log, &text, &len);
    if (r < 0)
        return 2;
    if (r == 0) {
        uinfo(L"No boot-time pass has run yet (%ls does not exist).\n",
              wide(p.log, w1, MAX_PATH));
    } else {
        uinfo(L"\nReport of the last pass (%ls):\n\n",
              wide(p.log, w1, MAX_PATH));
        print_log(text, len);
        free(text);
    }
    return uoutput_ok() ? 0 : 5;
}

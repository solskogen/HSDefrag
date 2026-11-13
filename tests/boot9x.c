/*
 * Windows 95/98 boot-time pass scheduling (boot9x.c) against an in-memory
 * file system: the copy of hsdefrag.exe, the job file and the AUTOEXEC.BAT
 * line, dry runs, merging drives, cancelling, the report of the last pass,
 * and the rollback when a write fails.
 */
#include "compat.h"
#include "nostd.h"

static BOOL t_write(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL t_mode(HANDLE, LPDWORD);
#define WriteFile t_write
#define GetConsoleMode t_mode
#include "util.c"
#undef WriteFile
#undef GetConsoleMode

#include "bootjob.h"
#include "fatwin9x.h"

static UINT t_drivetype(LPCSTR);
static DWORD t_getattr(LPCSTR);
static BOOL t_setattr(LPCSTR, DWORD);
static HANDLE t_create(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD,
                       DWORD, HANDLE);
static BOOL t_read(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL t_fwrite(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL t_close(HANDLE);
static BOOL t_flush(HANDLE);
static DWORD t_size(HANDLE, LPDWORD);
static BOOL t_delete(LPCSTR);
static BOOL t_move(LPCSTR, LPCSTR);
static BOOL t_volinfo(LPCSTR, LPSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPSTR,
                      DWORD);
static UINT t_windir(LPSTR, UINT);
static DWORD t_short(LPCSTR, LPSTR, DWORD);
static DWORD t_modname(HMODULE, LPSTR, DWORD);
static int t_bootsec(wchar_t, unsigned char *);
#define GetDriveTypeA t_drivetype
#define GetFileAttributesA t_getattr
#define SetFileAttributesA t_setattr
#define CreateFileA t_create
#define ReadFile t_read
#define WriteFile t_fwrite
#define CloseHandle t_close
#define FlushFileBuffers t_flush
#define GetFileSize t_size
#define DeleteFileA t_delete
#define MoveFileA t_move
#define GetVolumeInformationA t_volinfo
#define GetWindowsDirectoryA t_windir
#define GetShortPathNameA t_short
#define GetModuleFileNameA t_modname
#define fat9x_boot_sector t_bootsec
#include "../src/boot9x.c"
#undef GetDriveTypeA
#undef GetFileAttributesA
#undef SetFileAttributesA
#undef CreateFileA
#undef ReadFile
#undef WriteFile
#undef CloseHandle
#undef FlushFileBuffers
#undef GetFileSize
#undef DeleteFileA
#undef MoveFileA
#undef GetVolumeInformationA
#undef GetWindowsDirectoryA
#undef GetShortPathNameA
#undef GetModuleFileNameA
#undef fat9x_boot_sector

/* The running hsdefrag.exe: more than one 4 KB compare block. */
#define IMG_SIZE 9005u
static BYTE img[IMG_SIZE];

/* ------------------------------------------------------- the file system */

#define NF 24
#define NH 8
#define RO FILE_ATTRIBUTE_READONLY
#define HSR (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | RO)
#define ARC FILE_ATTRIBUTE_ARCHIVE

typedef struct {
    int      used;
    char     name[MAX_PATH];
    BYTE    *data;
    DWORD    len, attr;
    unsigned creates;              /* times created or truncated */
} tfile;

typedef struct {
    int   used, f, canWrite;
    DWORD pos;
} thandle;

static tfile   files[NF];
static thandle hs[NH];
static char    out[65536];
static unsigned outLen;
static char    winDir[MAX_PATH], winShort[MAX_PATH];
static char    selfPath[MAX_PATH];   /* "" : GetModuleFileNameA fails    */
static char    failCreate[MAX_PATH]; /* CREATE_ALWAYS of it: access denied */
static char    failWrite[MAX_PATH];  /* writing to it: disk full           */
static char    failMove[MAX_PATH];
static char    failDelete[MAX_PATH], failAttr[MAX_PATH];
static int     failRestore;
static int     failFlush;
static int     failOnce;             /* the failures above happen once    */
static int     noSerial, checks, failed;
static unsigned tempCreates;         /* AUTOEXEC.BAT rewrites            */

static int up_c(int c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static int same_ci(const char *a, const char *b)
{
    while (*a && up_c((unsigned char)*a) == up_c((unsigned char)*b)) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static size_t cstrlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

static int find(const char *name)
{
    int i;

    for (i = 0; i < NF; i++)
        if (files[i].used && same_ci(files[i].name, name))
            return i;
    return -1;
}

/* Create or replace (truncate) a file. */
static int put_file(const char *name, const void *data, DWORD len, DWORD attr)
{
    int i = find(name);

    if (i < 0) {
        for (i = 0; i < NF && files[i].used; i++)
            ;
        if (i == NF)
            return -1;
        memset(&files[i], 0, sizeof(files[i]));
        files[i].used = 1;
        lstrcpyA(files[i].name, name);
    }
    free(files[i].data);
    files[i].data = (BYTE *)malloc(len ? len : 1);
    if (len)
        memcpy(files[i].data, data, len);
    files[i].len = len;
    files[i].attr = attr;
    files[i].creates++;
    return i;
}

static void drop(int i)
{
    free(files[i].data);
    memset(&files[i], 0, sizeof(files[i]));
}

static int open_handles(void)
{
    int k, n = 0;

    for (k = 0; k < NH; k++)
        n += hs[k].used;
    return n;
}

static thandle *handle(HANDLE h)
{
    DWORD_PTR k = (DWORD_PTR)h - 0x100;

    return k < NH && hs[k].used ? &hs[k] : NULL;
}

static UINT t_drivetype(LPCSTR root)
{
    if (cstrlen(root) != 3 || root[1] != ':' || root[2] != '\\')
        return DRIVE_NO_ROOT_DIR;
    switch (up_c((unsigned char)root[0])) {
    case 'C': case 'D': case 'F':
        return DRIVE_FIXED;
    case 'A':
        return DRIVE_REMOVABLE;
    case 'E':
        return DRIVE_CDROM;
    default:
        return DRIVE_NO_ROOT_DIR;
    }
}

static DWORD t_getattr(LPCSTR name)
{
    int i = find(name);

    if (i < 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return 0xFFFFFFFFu;
    }
    return files[i].attr;
}

static BOOL t_setattr(LPCSTR name, DWORD attr)
{
    int i = find(name);

    if (attr != FILE_ATTRIBUTE_NORMAL && failAttr[0] &&
        same_ci(name, failAttr)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (i < 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    files[i].attr = attr;
    return TRUE;
}

static HANDLE t_create(LPCSTR name, DWORD access, DWORD share,
                       LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                       HANDLE tmpl)
{
    int f = find(name), k;

    (void)share; (void)sa; (void)flags; (void)tmpl;
    for (k = 0; k < NH && hs[k].used; k++)
        ;
    if (k == NH) {
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return INVALID_HANDLE_VALUE;
    }
    if (disp == OPEN_EXISTING) {
        if (f < 0) {
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
    } else if (disp == CREATE_ALWAYS || disp == CREATE_NEW) {
        if (disp == CREATE_NEW && f >= 0) {
            SetLastError(ERROR_FILE_EXISTS);
            return INVALID_HANDLE_VALUE;
        }
        if ((f >= 0 && (files[f].attr & RO)) ||
            (failCreate[0] && same_ci(name, failCreate))) {
            if (failOnce && failCreate[0] && same_ci(name, failCreate))
                failCreate[0] = 0;
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        f = put_file(name, NULL, 0, ARC);
        tempCreates += same_ci(name, "C:\\AUTOEXEC.$HS");
    } else {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    hs[k].used = 1;
    hs[k].f = f;
    hs[k].pos = 0;
    hs[k].canWrite = (access & GENERIC_WRITE) != 0;
    return (HANDLE)(DWORD_PTR)(0x100 + k);
}

static BOOL t_read(HANDLE h, LPVOID buf, DWORD n, LPDWORD got,
                   LPOVERLAPPED ov)
{
    thandle *t = handle(h);
    tfile *f;

    (void)ov;
    if (!t) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    f = &files[t->f];
    if (n > f->len - t->pos)
        n = f->len - t->pos;
    memcpy(buf, f->data + t->pos, n);
    t->pos += n;
    *got = n;
    return TRUE;
}

static BOOL t_fwrite(HANDLE h, LPCVOID data, DWORD n, LPDWORD put,
                     LPOVERLAPPED ov)
{
    thandle *t = handle(h);
    tfile *f;
    BYTE *grown;

    (void)ov;
    *put = 0;
    if (!t || !t->canWrite) {
        SetLastError(t ? ERROR_ACCESS_DENIED : ERROR_INVALID_HANDLE);
        return FALSE;
    }
    f = &files[t->f];
    if (failWrite[0] && same_ci(f->name, failWrite)) {
        if (failOnce)
            failWrite[0] = 0;
        SetLastError(ERROR_HANDLE_DISK_FULL);
        return FALSE;
    }
    grown = (BYTE *)realloc(f->data, t->pos + n + 1);
    if (!grown) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    f->data = grown;
    memcpy(f->data + t->pos, data, n);
    t->pos += n;
    if (t->pos > f->len)
        f->len = t->pos;
    *put = n;
    return TRUE;
}

static BOOL t_close(HANDLE h)
{
    thandle *t = handle(h);

    if (!t) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    t->used = 0;
    return TRUE;
}

static BOOL t_flush(HANDLE h)
{
    if (!handle(h) || failFlush) {
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return TRUE;
}

static DWORD t_size(HANDLE h, LPDWORD high)
{
    thandle *t = handle(h);

    if (high)
        *high = 0;
    return t ? files[t->f].len : 0xFFFFFFFFu;
}

static BOOL t_delete(LPCSTR name)
{
    int i = find(name);

    if (failDelete[0] && same_ci(name, failDelete)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (i < 0 || (files[i].attr & RO)) {
        SetLastError(i < 0 ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
        return FALSE;
    }
    drop(i);
    return TRUE;
}

static BOOL t_move(LPCSTR from, LPCSTR to)
{
    int i = find(from);

    if ((failMove[0] && same_ci(from, failMove)) ||
        (failRestore && same_ci(from, "C:\\AUTOEXEC.HS0"))) {
        if (failOnce)
            failMove[0] = 0;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (i < 0 || find(to) >= 0) {
        SetLastError(i < 0 ? ERROR_FILE_NOT_FOUND : ERROR_ALREADY_EXISTS);
        return FALSE;
    }
    lstrcpyA(files[i].name, to);
    return TRUE;
}

static BOOL t_volinfo(LPCSTR root, LPSTR label, DWORD labelCap,
                      LPDWORD serial, LPDWORD maxName, LPDWORD flags,
                      LPSTR fs, DWORD fsCap)
{
    const char *name;

    (void)label; (void)labelCap;
    switch (up_c((unsigned char)root[0])) {
    case 'C': name = "FAT32"; *serial = 0x1A2B3C4Du; break;
    case 'D': name = "FAT";   *serial = 0x5E6F7A8Bu; break;
    case 'F': name = "XFS";   *serial = 1; break;
    default:
        SetLastError(ERROR_NOT_READY);
        return FALSE;
    }
    *maxName = 255;
    *flags = 0;
    if (fsCap > cstrlen(name))
        lstrcpyA(fs, name);
    return TRUE;
}

static UINT t_windir(LPSTR buf, UINT cap)
{
    UINT n = (UINT)cstrlen(winDir);

    if (n + 1 > cap)
        return n + 1;
    lstrcpyA(buf, winDir);
    return n;
}

static DWORD t_short(LPCSTR in, LPSTR buf, DWORD cap)
{
    const char *s = same_ci(in, winDir) ? winShort : in;
    DWORD n = (DWORD)cstrlen(s);

    if (n + 1 > cap)
        return n + 1;
    lstrcpyA(buf, s);
    return n;
}

static DWORD t_modname(HMODULE m, LPSTR buf, DWORD cap)
{
    DWORD n = (DWORD)cstrlen(selfPath);

    if (m || !n) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    if (n + 1 > cap)
        return cap;
    lstrcpyA(buf, selfPath);
    return n;
}

/* C: FAT32, D: FAT16 (serial in the extended BPB unless noSerial). */
static int t_bootsec(wchar_t drive, unsigned char *bs)
{
    unsigned long serial = drive == L'C' ? 0x1A2B3C4Du : 0x5E6F7A8Bu;
    int ext = drive == L'C' ? 0x42 : 0x26;

    memset(bs, 0, 512);
    if (drive != L'C')
        bs[0x16] = 0x20;
    if (!noSerial) {
        bs[ext] = 0x29;
        bs[ext + 1] = (unsigned char)serial;
        bs[ext + 2] = (unsigned char)(serial >> 8);
        bs[ext + 3] = (unsigned char)(serial >> 16);
        bs[ext + 4] = (unsigned char)(serial >> 24);
    }
    bs[510] = 0x55;
    bs[511] = 0xAA;
    return 0;
}

/* -------------------------------------------------------------- output */

static BOOL t_write(HANDLE h, LPCVOID data, DWORD n, LPDWORD written,
                    LPOVERLAPPED ov)
{
    const char *s = (const char *)data;
    DWORD i;

    (void)h; (void)ov;
    for (i = 0; i < n && outLen + 1 < sizeof(out); i++)
        out[outLen++] = s[i];
    out[outLen] = 0;
    *written = n;
    return TRUE;
}

static BOOL t_mode(HANDLE h, LPDWORD mode)
{
    (void)h;
    *mode = 0;
    return FALSE; /* redirected output */
}

static int out_has(const char *s)
{
    size_t n = cstrlen(s), i;

    for (i = 0; i + n <= outLen; i++)
        if (memcmp(out + i, s, n) == 0)
            return 1;
    return 0;
}

/* ------------------------------------------------------------- checks */

static int put_num(char *num, int v)
{
    int n = 0, k;

    for (k = v; k; k /= 10)
        n++;
    if (!n)
        n = 1;
    num[n] = 0;
    for (k = n - 1; k >= 0; k--, v /= 10)
        num[k] = (char)('0' + v % 10);
    return n;
}

static void report(int line)
{
    static const char head[] = "FAIL boot9x line ";
    char num[16];
    DWORD written;
    int n = put_num(num, line);

    failed++;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), head, sizeof(head) - 1,
              &written, NULL);
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), num, (DWORD)n, &written, NULL);
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), "\r\n--- output:\r\n", 15,
              &written, NULL);
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), out, outLen, &written, NULL);
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), "---\r\n", 5, &written, NULL);
}

#define CHECK(c) do { checks++; if (!(c)) report(__LINE__); } while (0)

static int file_is(const char *name, const char *text)
{
    int i = find(name);
    size_t n = cstrlen(text);

    return i >= 0 && files[i].len == n && memcmp(files[i].data, text, n) == 0;
}

static int helper_ok(const char *name)
{
    int i = find(name);

    return i >= 0 && files[i].len == IMG_SIZE &&
           memcmp(files[i].data, img, IMG_SIZE) == 0;
}

static unsigned creates(const char *name)
{
    int i = find(name);

    return i < 0 ? 0 : files[i].creates;
}

/* The job file bj_format writes for drives "C", "CD", ... */
static const char *job_for(const char *drives, int serials)
{
    static char text[JOB_CAP];
    bj_entry e[BJ_MAX_DRIVES];
    int n;

    for (n = 0; drives[n]; n++) {
        e[n].drive = drives[n];
        e[n].hasSerial = serials;
        e[n].serial = drives[n] == 'C' ? 0x1A2B3C4Du : 0x5E6F7A8Bu;
    }
    if (bj_format(e, n, text, sizeof(text)) == BJ_ERR)
        text[0] = 0;
    return text;
}

#define LINE_WIN "@IF EXIST C:\\HSDEFRAG.JOB C:\\WINDOWS\\HSDEFRAG.EXE " \
                 "--boot C:\\HSDEFRAG.JOB"
#define LINE_ROOT "@IF EXIST C:\\HSDEFRAG.JOB C:\\HSDEFRAG.EXE --boot " \
                  "C:\\HSDEFRAG.JOB"
#define SELF "D:\\TOOLS\\HSDEFRAG.EXE"
#define AUTOEXEC0 "@echo off\r\nPATH C:\\WINDOWS;C:\\DOS\r\n"
#define AUTOEXEC1 "@echo off\r\n" LINE_WIN "\r\nPATH C:\\WINDOWS;C:\\DOS\r\n"

static void setup(void)
{
    int i;

    for (i = 0; i < NF; i++)
        if (files[i].used)
            drop(i);
    memset(hs, 0, sizeof(hs));
    img[0] = 'M';
    img[1] = 'Z';
    memset(img + 2, 'A', IMG_SIZE - 5);
    memcpy(img + IMG_SIZE - 3, "END", 3);
    lstrcpyA(selfPath, SELF);
    put_file(SELF, img, IMG_SIZE, ARC);
    put_file("C:\\IO.SYS", "IO", 2, HSR);
    put_file("C:\\MSDOS.SYS", "[Paths]\r\n", 9, HSR);
    put_file("C:\\AUTOEXEC.BAT", AUTOEXEC0, sizeof(AUTOEXEC0) - 1, ARC | RO);
    lstrcpyA(winDir, "C:\\WINDOWS");
    lstrcpyA(winShort, "C:\\WINDOWS");
    failCreate[0] = failWrite[0] = failMove[0] = 0;
    failDelete[0] = failAttr[0] = 0;
    failRestore = 0;
    failFlush = 0;
    failOnce = 0;
    noSerial = 0;
    tempCreates = 0;
    g_osMajor = 4;
    g_osMinor = 0;
    g_verbose = 0;
    outLen = 0;
    out[0] = 0;
}

static int nfiles(void)
{
    int i, n = 0;

    for (i = 0; i < NF; i++)
        n += files[i].used;
    return n;
}

/* ----------------------------------------------------------- scenarios */

static void test_refusals(void)
{
    setup();
    g_osMinor = 90;                            /* Windows Me */
    CHECK(cmd_boot9x_schedule(L'C', 0) == 3);
    CHECK(out_has("Windows Me") && nfiles() == 4);
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));

    setup();
    CHECK(cmd_boot9x_schedule(L'E', 0) == 3);  /* CD-ROM */
    CHECK(out_has("not a fixed disk"));
    CHECK(cmd_boot9x_schedule(L'A', 0) == 3);  /* floppy */
    CHECK(cmd_boot9x_schedule(L'Q', 0) == 2);  /* no such drive */
    CHECK(cmd_boot9x_schedule(L'F', 0) == 3);  /* not FAT */
    CHECK(out_has("XFS volume"));
    CHECK(nfiles() == 4 && open_handles() == 0);

    setup();
    drop(find("C:\\IO.SYS"));                  /* no boot drive */
    CHECK(cmd_boot9x_schedule(L'C', 0) == 2);
    CHECK(cmd_boot9x_status(L'C') == 2);
    CHECK(cmd_boot9x_cancel(L'C') == 2);
    CHECK(out_has("cannot find the drive Windows starts from"));
    CHECK(nfiles() == 3 && file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));
}

static void test_dry_run(void)
{
    setup();
    CHECK(cmd_boot9x_schedule(L'C', 1) == 0);
    CHECK(out_has("Dry run: nothing was written"));
    CHECK(out_has(LINE_WIN) && out_has("C: (1A2B-3C4D)"));
    CHECK(out_has("FAT32, serial 1A2B-3C4D"));
    CHECK(nfiles() == 4 && creates("C:\\AUTOEXEC.BAT") == 1);
    CHECK(open_handles() == 0);
}

static void test_schedule_cancel(void)
{
    static const char logText[] =
        "Boot-time pass, 2024-05-01 08:00\r\n\r\nDefragmenting C:\r\n"
        "  files defragmented   : 42\r\n\x1A";

    setup();
    CHECK(cmd_boot9x_schedule(L'c', 0) == 0);
    CHECK(helper_ok("C:\\WINDOWS\\HSDEFRAG.EXE"));
    CHECK(file_is("C:\\HSDEFRAG.JOB", job_for("C", 1)));
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC1) && tempCreates == 1);
    CHECK(t_getattr("C:\\AUTOEXEC.BAT") == (ARC | RO));  /* attributes kept */
    CHECK(find("C:\\AUTOEXEC.$HS") < 0 && open_handles() == 0);
    CHECK(out_has("C: is defragmented the next time the computer starts"));

    /* A second drive joins the job; the rest is already right. */
    outLen = 0;
    CHECK(cmd_boot9x_schedule(L'D', 0) == 0);
    CHECK(file_is("C:\\HSDEFRAG.JOB", job_for("CD", 1)));
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC1) && tempCreates == 1);
    CHECK(creates("C:\\WINDOWS\\HSDEFRAG.EXE") == 1);
    CHECK(out_has("C: (1A2B-3C4D), D: (5E6F-7A8B)"));

    /* Scheduling a drive again changes nothing. */
    CHECK(cmd_boot9x_schedule(L'C', 0) == 0);
    CHECK(file_is("C:\\HSDEFRAG.JOB", job_for("CD", 1)));

    /* Status, without and with a report. */
    outLen = 0;
    CHECK(cmd_boot9x_status(L'D') == 0);
    CHECK(out_has("D: is defragmented the next time the computer starts"));
    CHECK(out_has("starts the pass") && out_has("No boot-time pass has run"));
    CHECK(!out_has("warning"));
    put_file("C:\\HSDEFRAG.LOG", logText, sizeof(logText) - 1, ARC);
    outLen = 0;
    CHECK(cmd_boot9x_status(L'C') == 0);
    CHECK(out_has("Report of the last pass (C:\\HSDEFRAG.LOG)"));
    CHECK(out_has("\nDefragmenting C:\n  files defragmented   : 42\n"));
    CHECK(!out_has("\r") && !out_has("\x1A"));

    /* Cancelling one drive keeps the other. */
    outLen = 0;
    CHECK(cmd_boot9x_cancel(L'C') == 0);
    CHECK(file_is("C:\\HSDEFRAG.JOB", job_for("D", 1)));
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC1));
    CHECK(out_has("C: is no longer scheduled") && out_has("D: (5E6F-7A8B)"));
    outLen = 0;
    CHECK(cmd_boot9x_cancel(L'C') == 0);
    CHECK(out_has("C: is not scheduled") && out_has("Still scheduled"));

    /* The last one removes the job and the line; hsdefrag.exe stays. */
    outLen = 0;
    CHECK(cmd_boot9x_cancel(L'D') == 0);
    CHECK(find("C:\\HSDEFRAG.JOB") < 0);
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));
    CHECK(t_getattr("C:\\AUTOEXEC.BAT") == (ARC | RO));
    CHECK(helper_ok("C:\\WINDOWS\\HSDEFRAG.EXE"));
    CHECK(out_has("The boot-time pass of D: is cancelled"));
    CHECK(out_has("removed from C:\\AUTOEXEC.BAT"));
    outLen = 0;
    CHECK(cmd_boot9x_cancel(L'D') == 0);
    CHECK(out_has("D: is not scheduled") && !out_has("removed"));
    CHECK(open_handles() == 0 && find("C:\\AUTOEXEC.$HS") < 0);

    /* After a pass: the job is gone but the idle line remains. */
    CHECK(cmd_boot9x_schedule(L'C', 0) == 0);
    t_delete("C:\\HSDEFRAG.JOB");
    outLen = 0;
    CHECK(cmd_boot9x_status(L'C') == 0);
    CHECK(out_has("idle line") && out_has("C: is not scheduled"));
    CHECK(cmd_boot9x_cancel(L'C') == 0);
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));
}

static void test_no_autoexec(void)
{
    setup();
    drop(find("C:\\AUTOEXEC.BAT"));
    noSerial = 1;
    CHECK(cmd_boot9x_schedule(L'D', 0) == 0);
    CHECK(file_is("C:\\AUTOEXEC.BAT", LINE_WIN "\r\n"));
    CHECK(file_is("C:\\HSDEFRAG.JOB", job_for("D", 0)));
    CHECK(cmd_boot9x_cancel(L'D') == 0);
    CHECK(file_is("C:\\AUTOEXEC.BAT", ""));      /* emptied, never deleted */
    CHECK(open_handles() == 0);
}

static void test_helper_place(void)
{
    /* Windows on another drive: hsdefrag.exe goes to the boot drive. */
    setup();
    lstrcpyA(winDir, "D:\\WIN95");
    lstrcpyA(winShort, "D:\\WIN95");
    CHECK(cmd_boot9x_schedule(L'D', 0) == 0);
    CHECK(helper_ok("C:\\HSDEFRAG.EXE") && find("D:\\WIN95\\HSDEFRAG.EXE") < 0);
    CHECK(file_is("C:\\AUTOEXEC.BAT", "@echo off\r\n" LINE_ROOT
                  "\r\nPATH C:\\WINDOWS;C:\\DOS\r\n"));

    /* A long Windows folder: the line uses its short name. */
    setup();
    lstrcpyA(winDir, "C:\\Program Files\\Windows 95");
    lstrcpyA(winShort, "C:\\PROGRA~1\\WINDOW~1");
    CHECK(cmd_boot9x_schedule(L'C', 0) == 0);
    CHECK(helper_ok("C:\\Program Files\\Windows 95\\HSDEFRAG.EXE"));
    CHECK(out_has("@IF EXIST C:\\HSDEFRAG.JOB C:\\PROGRA~1\\WINDOW~1\\"
                  "HSDEFRAG.EXE --boot C:\\HSDEFRAG.JOB"));

    /* A short name too long for the line: the root of the boot drive. */
    setup();
    lstrcpyA(winDir, "C:\\AAAAAAAA\\BBBBBBBB\\CCCCCCCC\\DDDDDDDD\\EEEEEEEE\\"
                     "FFFFFFFF\\GGGGGGGG\\HHHHHHHH\\IIIIIIII");
    lstrcpyA(winShort, winDir);
    CHECK(cmd_boot9x_schedule(L'C', 0) == 0);
    CHECK(helper_ok("C:\\HSDEFRAG.EXE"));
    CHECK(out_has(LINE_ROOT));
}

static void test_rollback(void)
{
    /* AUTOEXEC.BAT cannot be replaced: the new job is taken back. */
    setup();
    lstrcpyA(failWrite, "C:\\AUTOEXEC.$HS");
    CHECK(cmd_boot9x_schedule(L'C', 0) == 2);
    CHECK(find("C:\\HSDEFRAG.JOB") < 0 && find("C:\\AUTOEXEC.$HS") < 0);
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));
    CHECK(out_has("scheduling did not complete") && out_has("disk full"));

    /* ... and an existing job is put back as it was. */
    setup();
    CHECK(cmd_boot9x_schedule(L'C', 0) == 0);
    t_setattr("C:\\AUTOEXEC.BAT", ARC);
    put_file("C:\\AUTOEXEC.BAT", AUTOEXEC0, sizeof(AUTOEXEC0) - 1, ARC);
    lstrcpyA(failCreate, "C:\\AUTOEXEC.$HS");
    CHECK(cmd_boot9x_schedule(L'D', 0) == 2);
    CHECK(file_is("C:\\HSDEFRAG.JOB", job_for("C", 1)));
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));
    CHECK(out_has("access denied") && out_has("scheduling did not complete"));

    /* The job file cannot be written: a new one is removed again ... */
    setup();
    lstrcpyA(failWrite, "C:\\HSDEFRAG.JOB.HST");
    CHECK(cmd_boot9x_schedule(L'C', 0) == 2);
    CHECK(find("C:\\HSDEFRAG.JOB") < 0);
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0) && tempCreates == 0);
    CHECK(out_has("C:\\HSDEFRAG.JOB") && out_has("disk full"));
    CHECK(out_has("scheduling did not complete") && open_handles() == 0);

    /* ... and an existing one keeps the drives it had. */
    setup();
    CHECK(cmd_boot9x_schedule(L'C', 0) == 0);
    lstrcpyA(failWrite, "C:\\HSDEFRAG.JOB.HST");
    failOnce = 1;
    outLen = 0;
    CHECK(cmd_boot9x_schedule(L'D', 0) == 2);
    CHECK(file_is("C:\\HSDEFRAG.JOB", job_for("C", 1)));
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC1) && tempCreates == 1);
    CHECK(out_has("scheduling did not complete") && open_handles() == 0);

    /* The copy cannot be written: nothing else is touched. */
    setup();
    lstrcpyA(failWrite, "C:\\WINDOWS\\HSDEFRAG.EXE.HST");
    CHECK(cmd_boot9x_schedule(L'C', 0) == 2);
    CHECK(find("C:\\WINDOWS\\HSDEFRAG.EXE") < 0);
    CHECK(find("C:\\HSDEFRAG.JOB") < 0);
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));
    CHECK(out_has("C:\\WINDOWS\\HSDEFRAG.EXE could not be written"));
    CHECK(open_handles() == 0);

    /* The running program cannot be found or read: nothing is touched. */
    setup();
    selfPath[0] = 0;
    CHECK(cmd_boot9x_schedule(L'C', 0) == 2);
    CHECK(out_has("cannot find the file hsdefrag runs from"));
    CHECK(nfiles() == 4 && file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));
    setup();
    drop(find(SELF));
    outLen = 0;
    CHECK(cmd_boot9x_schedule(L'C', 0) == 2);
    CHECK(out_has("cannot open D:\\TOOLS\\HSDEFRAG.EXE: file not found"));
    CHECK(nfiles() == 3 && file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));
    CHECK(open_handles() == 0);

    /* A stale or damaged hsdefrag.exe and job are replaced. */
    setup();
    put_file("C:\\WINDOWS\\HSDEFRAG.EXE", "old", 3, ARC | RO);
    put_file("C:\\HSDEFRAG.JOB", "garbage\r\n", 9, ARC);
    CHECK(cmd_boot9x_schedule(L'C', 0) == 0);
    CHECK(helper_ok("C:\\WINDOWS\\HSDEFRAG.EXE"));
    CHECK(file_is("C:\\HSDEFRAG.JOB", job_for("C", 1)));
    CHECK(out_has("damaged"));
}

static void test_self(void)
{
    /* Run from the place of the copy: nothing to copy. */
    setup();
    lstrcpyA(selfPath, "C:\\WINDOWS\\HSDEFRAG.EXE");
    put_file(selfPath, img, IMG_SIZE, ARC);
    g_verbose = 1;
    CHECK(cmd_boot9x_schedule(L'C', 0) == 0);
    CHECK(helper_ok(selfPath) && creates(selfPath) == 1);
    CHECK(out_has("C:\\WINDOWS\\HSDEFRAG.EXE is up to date"));
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC1) && open_handles() == 0);
}

static void test_replacement_failures(void)
{
    static const char *const reserved[] = {
        "C:\\AUTOEXEC.$HS", "C:\\AUTOEXEC.HS0"
    };
    int i;

    for (i = 0; i < 2; i++) {
        setup();
        put_file(reserved[i], "keep", 4, ARC);
        CHECK(cmd_boot9x_schedule(L'C', 0) == 2);
        CHECK(file_is(reserved[i], "keep"));
        CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));
        CHECK(out_has("temporary or recovery file"));
        CHECK(open_handles() == 0);
    }

    setup();
    lstrcpyA(failMove, "C:\\AUTOEXEC.$HS");
    CHECK(cmd_boot9x_schedule(L'C', 0) == 2);
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));
    CHECK(t_getattr("C:\\AUTOEXEC.BAT") == (ARC | RO));
    CHECK(find("C:\\AUTOEXEC.HS0") < 0);
    CHECK(find("C:\\HSDEFRAG.JOB") < 0 && open_handles() == 0);

    setup();
    lstrcpyA(failMove, "C:\\AUTOEXEC.BAT");
    CHECK(cmd_boot9x_schedule(L'C', 0) == 2);
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));
    CHECK(find("C:\\AUTOEXEC.$HS") < 0);
    CHECK(find("C:\\AUTOEXEC.HS0") < 0 && open_handles() == 0);

    setup();
    put_file("C:\\WINDOWS\\HSDEFRAG.EXE", "old", 3, HSR);
    lstrcpyA(failWrite, "C:\\WINDOWS\\HSDEFRAG.EXE.HST");
    CHECK(cmd_boot9x_schedule(L'C', 0) == 2);
    CHECK(file_is("C:\\WINDOWS\\HSDEFRAG.EXE", "old"));
    CHECK(t_getattr("C:\\WINDOWS\\HSDEFRAG.EXE") == HSR);
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));
    CHECK(open_handles() == 0);

    setup();
    CHECK(cmd_boot9x_schedule(L'C', 0) == 0);
    lstrcpyA(failWrite, "C:\\HSDEFRAG.JOB.HST");
    CHECK(cmd_boot9x_schedule(L'D', 0) == 2);
    CHECK(file_is("C:\\HSDEFRAG.JOB", job_for("C", 1)));
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC1));
    CHECK(open_handles() == 0);

    setup();
    put_file("C:\\WINDOWS\\HSDEFRAG.EXE", "old", 3, ARC);
    failFlush = 1;
    CHECK(cmd_boot9x_schedule(L'C', 0) == 2);
    CHECK(file_is("C:\\WINDOWS\\HSDEFRAG.EXE", "old"));
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));
    CHECK(find("C:\\WINDOWS\\HSDEFRAG.EXE.HST") < 0);
    CHECK(open_handles() == 0);

    setup();
    lstrcpyA(failMove, "C:\\AUTOEXEC.$HS");
    failRestore = 1;
    CHECK(cmd_boot9x_schedule(L'C', 0) == 2);
    CHECK(find("C:\\AUTOEXEC.BAT") < 0);
    CHECK(file_is("C:\\AUTOEXEC.HS0", AUTOEXEC0));
    CHECK(file_is("C:\\AUTOEXEC.$HS", AUTOEXEC1));
    CHECK(out_has("Restore the recovery file before restarting"));
    CHECK(find("C:\\HSDEFRAG.JOB") < 0 && open_handles() == 0);

    setup();
    lstrcpyA(failDelete, "C:\\AUTOEXEC.HS0");
    CHECK(cmd_boot9x_schedule(L'C', 0) == 2);
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC1));
    CHECK(file_is("C:\\AUTOEXEC.HS0", AUTOEXEC0));
    CHECK(out_has("remove recovery file"));
    CHECK(find("C:\\HSDEFRAG.JOB") < 0 && open_handles() == 0);

    setup();
    lstrcpyA(failAttr, "C:\\AUTOEXEC.BAT");
    CHECK(cmd_boot9x_schedule(L'C', 0) == 2);
    CHECK(file_is("C:\\AUTOEXEC.BAT", AUTOEXEC1));
    CHECK(file_is("C:\\AUTOEXEC.HS0", AUTOEXEC0));
    CHECK(out_has("restore attributes"));
    CHECK(find("C:\\HSDEFRAG.JOB") < 0 && open_handles() == 0);

    /* An installed job whose recovery cleanup fails must not be reported
     * as successfully scheduled or silently discarded. */
    setup();
    CHECK(cmd_boot9x_schedule(L'C', 0) == 0);
    lstrcpyA(failDelete, "C:\\HSDEFRAG.JOB.HSB");
    outLen = 0;
    CHECK(cmd_boot9x_schedule(L'D', 0) == 2);
    CHECK(file_is("C:\\HSDEFRAG.JOB.HSB", job_for("C", 1)));
    CHECK(file_is("C:\\HSDEFRAG.JOB", job_for("CD", 1)));
    CHECK(out_has("Remove C:\\HSDEFRAG.JOB.HSB before retrying"));
    CHECK(out_has("previous job could not be restored"));
    CHECK(out_has("scheduling did not complete") && open_handles() == 0);
    outLen = 0;
    CHECK(cmd_boot9x_status(L'C') == 0);
    CHECK(out_has("recovery files : C:\\HSDEFRAG.JOB.HSB"));
    CHECK(out_has("warning: temporary or recovery files remain on disk"));
}

#ifdef HSD_WINDOWS_ONLY
/* make windows: no MS-DOS part, so the pass must never be scheduled. */
static void test_windows_only(void)
{
    setup();
    CHECK(cmd_boot9x_schedule(L'C', 0) == 3);
    CHECK(cmd_boot9x_schedule(L'C', 1) == 3);
    CHECK(out_has("built for Windows only"));
    CHECK(nfiles() == 4 && file_is("C:\\AUTOEXEC.BAT", AUTOEXEC0));
    CHECK(find("C:\\HSDEFRAG.JOB") < 0 && find("C:\\WINDOWS\\HSDEFRAG.EXE") < 0);
    CHECK(open_handles() == 0);
}
#endif

int wmain(int argc, wchar_t **argv)
{
    static const char ok[] = " checks: Windows 9x boot-time scheduling "
                             "regressions passed.\r\n";
    char num[16];
    DWORD written;
    int n;

    (void)argc; (void)argv;
#ifdef HSD_WINDOWS_ONLY
    test_windows_only();
#else
    test_refusals();
    test_dry_run();
    test_schedule_cancel();
    test_no_autoexec();
    test_helper_place();
    test_rollback();
    test_replacement_failures();
    test_self();
#endif
    if (failed)
        return 1;
    n = put_num(num, checks);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), num, (DWORD)n, &written, NULL);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), ok, sizeof(ok) - 1, &written,
              NULL);
    return 0;
}

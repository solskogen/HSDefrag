/*
 * Windows 9x layer regressions (fatwin9x.c) against a simulated VWIN32: a
 * FAT32 (or FAT16) volume in memory, the Int 21h 440Dh volume locks and the
 * in-use file calls (6Ch, 6Dh, 6Eh, 71h). The simulation enforces Microsoft's
 * rules (locks taken in order with category 48h on FAT32 and 08h on FAT12/16;
 * direct writes, Enumerate Open Files and Get First Cluster only under level
 * 3), and the test checks
 * that nothing is printed and nobody waits while level 3 is held, that every
 * file Windows reports in use keeps its clusters, and that all data survives.
 */
#include "compat.h"
#include "nostd.h"

static BOOL t_write(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL t_console(HANDLE, const VOID *, DWORD, LPDWORD, LPVOID);
static BOOL t_mode(HANDLE, LPDWORD);
#define WriteFile t_write
#define WriteConsoleW t_console
#define GetConsoleMode t_mode
#include "util.c"
#undef WriteFile
#undef WriteConsoleW
#undef GetConsoleMode

static BOOL t_ioctl(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD,
                    LPOVERLAPPED);
static HANDLE t_create(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD,
                       DWORD, HANDLE);
static BOOL t_close(HANDLE);
static DWORD t_module(HMODULE, LPSTR, DWORD);
static UINT t_sysdir(LPSTR, UINT);
static DWORD t_short(LPCSTR, LPSTR, DWORD);
static void t_sleep(DWORD);
#define DeviceIoControl t_ioctl
#define CreateFileA t_create
#define CloseHandle t_close
#define GetModuleFileNameA t_module
#define GetSystemDirectoryA t_sysdir
#define GetShortPathNameA t_short
#define Sleep t_sleep
#include "fatwin9x.c"
#undef DeviceIoControl
#undef CreateFileA
#undef CloseHandle
#undef GetModuleFileNameA
#undef GetSystemDirectoryA
#undef GetShortPathNameA
#undef Sleep

/* ------------------------------------------------------------ the volume */

#define BPS   512u
#define TOTAL 70000u                 /* 68888 clusters of one sector: FAT32 */
#define RESV  32u
#define FATSZ 540u
#define EOC   0x0FFFFFFFu
/* The FAT16 layout: 39647 clusters of one sector, a 512-entry root folder. */
#define TOTAL16 40000u
#define RESV16  1u
#define FATSZ16 160u
#define ROOT16  (RESV16 + 2u * FATSZ16)
#define FAKE_VWIN32 ((HANDLE)(DWORD_PTR)0x5150)

typedef struct {
    const char *name11;
    const char *path;
    fat_u32     cl[3];
    int         n;
} tfile;

enum { F_SELF, F_KERNEL, F_MAPPED, F_OPEN, F_FRAG, F_LONG, F_SWAP, F_NODRIVE,
       F_BADCL, NFILES };

static const tfile files[NFILES] = {
    { "HSDEFRAGEXE", "C:\\HSDEFRAG.EXE",  { 300, 301, 302 }, 3 },
    { "KERNEL32DLL", "C:\\KERNEL32.DLL", { 400, 401 }, 2 },
    { "MAPPED  DAT", "C:\\MAPPED.DAT",   { 500, 501 }, 2 },
    { "OPEN    LOG", "C:\\OPEN.LOG",     { 600 }, 1 },
    { "FRAG    TXT", "C:\\FRAG.TXT",     { 10, 12, 14 }, 3 },
    { "LONGFI~1TXT", "C:\\LONGFI~1.TXT", { 700 }, 1 },
    { "WIN386  SWP", "C:\\WIN386.SWP",   { 800, 801 }, 2 },
    { "NODRIVE DLL", "C:\\NODRIVE.DLL",  { 900 }, 1 },
    { "BADCL   DLL", "C:\\BADCL.DLL",    { 950 }, 1 },
};

static fat_u8 *disk, *snap;
static int fat16;                    /* build the FAT16 volume instead      */
static fat_u32 totalSecs, data0;     /* of the volume built; data0 is the
                                        sector of cluster 2                 */

static void put16(fat_u8 *p, fat_u32 v)
{
    p[0] = (fat_u8)v;
    p[1] = (fat_u8)(v >> 8);
}

static void put32(fat_u8 *p, fat_u32 v)
{
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

static fat_u8 *sec(fat_u32 s)
{
    return disk + (size_t)s * BPS;
}

static fat_u8 *clus(fat_u32 c)
{
    return sec(data0 + c - 2);
}

static fat_u8 pattern(int f, int k, fat_u32 i)
{
    return (fat_u8)(f * 37 + k * 11 + (int)i * 3 + 1);
}

static void set_fat(fat_u32 c, fat_u32 v)
{
    if (fat16) {
        put16(sec(RESV16) + c * 2, v & 0xFFFF);
        put16(sec(RESV16 + FATSZ16) + c * 2, v & 0xFFFF);
        return;
    }
    put32(sec(RESV) + c * 4, v);
    put32(sec(RESV + FATSZ) + c * 4, v);
}

static void build16(void)
{
    fat_u8 *b = sec(0);

    put16(b + 0x11, 512);
    put16(b + 0x16, FATSZ16);
    put16(b + 0x0E, RESV16);
    put32(b + 0x20, TOTAL16);
    put32(b + 0x24, 0);
    put32(b + 0x2C, 0);
    put16(b + 0x30, 0);
    put16(b + 0x32, 0);
    memset(b + 0x40, 0, 0x1A);
    b[0x24] = 0x80;
    b[0x26] = 0x29;
    memcpy(b + 0x2B, "NO NAME    FAT16   ", 19);
    memset(sec(1), 0, BPS);
}

static void build(void)
{
    fat_u8 *b = sec(0), *fsi = sec(1);
    fat_u32 i;
    int f, k;

    memset(disk, 0, (size_t)TOTAL * BPS);
    totalSecs = fat16 ? TOTAL16 : TOTAL;
    data0 = fat16 ? ROOT16 + 32u : RESV + 2u * FATSZ;
    b[0] = 0xEB;
    b[1] = 0x58;
    b[2] = 0x90;
    memcpy(b + 3, "MSWIN4.1", 8);
    put16(b + 0x0B, BPS);
    b[0x0D] = 1;
    put16(b + 0x0E, RESV);
    b[0x10] = 2;
    b[0x15] = 0xF8;
    put16(b + 0x18, 63);
    put16(b + 0x1A, 255);
    put32(b + 0x20, TOTAL);
    put32(b + 0x24, FATSZ);
    put32(b + 0x2C, 2);
    put16(b + 0x30, 1);
    put16(b + 0x32, 6);
    b[0x40] = 0x80;
    b[0x42] = 0x29;
    memcpy(b + 0x47, "NO NAME    FAT32   ", 19);
    b[510] = 0x55;
    b[511] = 0xAA;
    put32(fsi, 0x41615252);
    put32(fsi + 484, 0x61417272);
    put32(fsi + 488, 0xFFFFFFFF);
    put32(fsi + 492, 0xFFFFFFFF);
    put32(fsi + 508, 0xAA550000);
    if (fat16)
        build16();
    set_fat(0, 0x0FFFFFF8);
    set_fat(1, EOC);
    if (!fat16)
        set_fat(2, EOC); /* the FAT32 root folder */
    for (f = 0; f < NFILES; f++) {
        const tfile *t = &files[f];
        fat_u8 *e = (fat16 ? sec(ROOT16) : clus(2)) + f * 32;

        memcpy(e, t->name11, 11);
        e[11] = 0x20;
        put16(e + 20, t->cl[0] >> 16);
        put16(e + 26, t->cl[0] & 0xFFFF);
        put32(e + 28, (fat_u32)t->n * BPS);
        for (k = 0; k < t->n; k++) {
            set_fat(t->cl[k], k + 1 < t->n ? t->cl[k + 1] : EOC);
            for (i = 0; i < BPS; i++)
                clus(t->cl[k])[i] = pattern(f, k, i);
        }
    }
}

/* ------------------------------------------------------- simulated VWIN32 */

typedef struct {
    const char *path;
    WORD        cx;   /* 0 normal, 1 mapped, 2 32-bit program or DLL */
} tent;

static const tent std1[] = {          /* Enumerate Open Files, DI = 1 */
    { "C:\\HSDEFRAG.EXE", 2 },
    { "C:\\KERNEL32.DLL", 2 },
    { "C:\\Long File Name.txt", 2 },  /* only its short name resolves */
    { "\\NODRIVE.DLL", 2 },           /* reported without a drive letter */
    { "C:\\BADCL.DLL", 2 },           /* VFAT returns a bad cluster for it */
    { "D:\\OTHER.DLL", 2 },           /* another drive */
    { "C:\\GONE.DLL", 2 },            /* deleted before it is resolved */
    { NULL, 0 }
};
static const tent ghost1[] = {        /* stays listed but is never found */
    { "C:\\HSDEFRAG.EXE", 2 },
    { "C:\\KERNEL32.DLL", 2 },
    { "C:\\GHOST.DLL", 2 },
    { NULL, 0 }
};
static const tent std0[] = {          /* DI = 0: every open file */
    { "C:\\OPEN.LOG", 0 },            /* a normal file stays movable */
    { "C:\\MAPPED.DAT", 1 },          /* missing from the DI = 1 list */
    { "C:\\HSDEFRAG.EXE", 2 },
    { "C:\\KERNEL32.DLL", 2 },
    { NULL, 0 }
};
static const tent mapped1[] = {       /* after OPEN.LOG gets mapped */
    { "C:\\HSDEFRAG.EXE", 2 },
    { "C:\\KERNEL32.DLL", 2 },
    { "C:\\Long File Name.txt", 2 },
    { "\\NODRIVE.DLL", 2 },
    { "C:\\BADCL.DLL", 2 },
    { "C:\\OPEN.LOG", 1 },
    { NULL, 0 }
};
static const tent mapped0[] = {
    { "C:\\OPEN.LOG", 1 },
    { "C:\\MAPPED.DAT", 1 },
    { NULL, 0 }
};
static const tent noself[] = {
    { "C:\\KERNEL32.DLL", 2 },
    { NULL, 0 }
};
static const tent none[] = { { NULL, 0 } };
static const tent every[] = {         /* for allIndex: every open file */
    { "C:\\OPEN.LOG", 0 },
    { "C:\\HSDEFRAG.EXE", 2 },
    { "C:\\MAPPED.DAT", 1 },
    { "C:\\Long File Name.txt", 2 },
    { "C:\\KERNEL32.DLL", 2 },
    { "\\NODRIVE.DLL", 2 },
    { "D:\\OTHER.DLL", 2 },
    { "C:\\BADCL.DLL", 2 },
    { "C:\\GONE.DLL", 2 },
    { NULL, 0 }
};

static const tent *list1, *list0, *list1b, *list0b;
static WORD endCode;         /* answer after the last file */
static WORD enumFail;        /* a real failure of Enumerate Open Files */
static int enum48Fails;      /* the 48h form of 6Dh is not supported */
static int allIndex;         /* FileIndex of the unmovable list counts every
                                open file, and CX of the full list is 0 */
static int inUse;            /* files are open: no level 0 lock */
static int secondLockFails, unlockFails, endlessList;
static int flag2Session;     /* level 2/3 session in which a file is mapped */
static int flagValue;        /* the lock flag it then reports (1 or 2) */
static int flagFired;
static int goneDeleted;      /* GONE.DLL is deleted: no longer listed */
static int unlock3Session;   /* session whose unlocks from level 3 fail */
static int unlock3Fails;     /* ... this many times in a row */
static int unlock3Writes;    /* sectors written before the first of them */
static int cat48Calls;       /* IOCTLs made with category 48h */
static int allowFrag;        /* FRAG.TXT may stay fragmented (run stopped) */
static const char *selfPath, *sysDir;

static int lv[8], nlv, sessions, protoLine, writes, cat08Enum, shortCalls;
static int noDos7305;      /* the 7305h call fails: the Int 25h/26h is used */
static int int25Calls, int26Calls;
static char out[16384];
static unsigned outLen;

static int top(void)
{
    return nlv ? lv[nlv - 1] : -1;
}

static void proto(int line)
{
    if (!protoLine)
        protoLine = line;
}

static void copy_str(char *dst, const char *src)
{
    while ((*dst++ = *src++) != 0)
        ;
}

static DWORD str_len(const char *s)
{
    DWORD n = 0;

    while (s[n])
        n++;
    return n;
}

static BOOL answer(DIOC_REGISTERS *r, WORD err)
{
    if (err) {
        r->reg_EAX = err;
        r->reg_Flags |= 1;
    } else {
        r->reg_Flags &= ~1u;
    }
    return TRUE;
}

/* Entry i of a list of open files; GONE.DLL is no longer open once it was
 * deleted. NULL past the end. */
static const tent *nth(const tent *L, DWORD i)
{
    for (; L->path; L++) {
        if (goneDeleted && same_path(L->path, "C:\\GONE.DLL"))
            continue;
        if (i-- == 0)
            return L;
    }
    return NULL;
}

static BOOL sim_disk_io(DIOC_REGISTERS *r)
{
    DISKIO *d = (DISKIO *)(DWORD_PTR)r->reg_EBX;
    int write = (int)(r->reg_ESI & 1);

    if (noDos7305) {
        /* A pre-OSR2 system: the DOS 7.1 absolute disk I/O is not there.
         * This is not a protocol error: the layer falls back to Int 25h/26h. */
        return answer(r, 1);
    }
    if ((r->reg_EAX & 0xFFFF) != 0x7305 || (r->reg_ECX & 0xFFFF) != 0xFFFF ||
        r->reg_EDX != 3 || (r->reg_ESI & ~1u) != 0 || !d) {
        proto(__LINE__);
        return answer(r, 1);
    }
    if (d->StartSector + d->nSectors > totalSecs || !d->nSectors) {
        proto(__LINE__);
        return answer(r, 0x1B);
    }
    if (write) {
        /* Direct writes need a level 3 lock (or the exclusive level 0). */
        if (top() != 3 && !(nlv && lv[0] == 0)) {
            proto(__LINE__);
            return answer(r, 0x13);
        }
        writes++;
        memcpy(sec(d->StartSector), d->Buffer, (size_t)d->nSectors * BPS);
    } else {
        memcpy(d->Buffer, sec(d->StartSector), (size_t)d->nSectors * BPS);
    }
    return answer(r, 0);
}

/* The classic Int 25h/26h: the DIOC code carries the operation, the register
 * image only names the drive (AL the 0-based number, DX the 1-based one). */
static BOOL sim_disk_io_int(DIOC_REGISTERS *r, int write)
{
    DISKIO *d = (DISKIO *)(DWORD_PTR)r->reg_EBX;

    if ((r->reg_EAX & 0xFF) != 2 || (r->reg_ECX & 0xFFFF) != 0xFFFF ||
        r->reg_EDX != 3 || !d) {
        proto(__LINE__);
        return answer(r, 1);
    }
    if (d->StartSector + d->nSectors > totalSecs || !d->nSectors) {
        proto(__LINE__);
        return answer(r, 0x1B);
    }
    if (write) {
        if (top() != 3 && !(nlv && lv[0] == 0)) {
            proto(__LINE__);
            return answer(r, 0x13);
        }
        writes++;
        memcpy(sec(d->StartSector), d->Buffer, (size_t)d->nSectors * BPS);
    } else {
        memcpy(d->Buffer, sec(d->StartSector), (size_t)d->nSectors * BPS);
    }
    if (write)
        int26Calls++;
    else
        int25Calls++;
    return answer(r, 0);
}

static BOOL sim_ioctl(DIOC_REGISTERS *r)
{
    static const struct { const char *path; fat_u32 cl; } firstCl[] = {
        { "C:\\HSDEFRAG.EXE", 300 },  { "C:\\KERNEL32.DLL", 400 },
        { "C:\\MAPPED.DAT", 500 },   { "C:\\OPEN.LOG", 600 },
        { "C:\\WIN386.SWP", 800 },   { "C:\\NODRIVE.DLL", 900 },
        { "C:\\BADCL.DLL", 0x7FFFFFFu },
    };
    int cat = (int)((r->reg_ECX >> 8) & 0xFF), minor = (int)(r->reg_ECX & 0xFF);
    int lockCat = fat16 ? 0x08 : 0x48; /* the only one locks accept */
    char *buf = (char *)(DWORD_PTR)r->reg_EDX;
    const tent *e;
    unsigned k;

    if ((r->reg_EAX & 0xFFFF) != 0x440D || (cat != 0x48 && cat != 0x08)) {
        proto(__LINE__);
        return answer(r, 1);
    }
    if (cat == 0x48)
        cat48Calls++;
    switch (minor) {
    case 0x4A: { /* lock logical volume */
        int level = (int)((r->reg_EBX >> 8) & 0xFF);

        if (cat != lockCat || (r->reg_EBX & 0xFF) != 3 || nlv >= 8) {
            proto(__LINE__);
            return answer(r, 1);
        }
        if (level == 0) {
            if (nlv && top() != 0) {
                proto(__LINE__);
                return answer(r, 5);
            }
            if (inUse)
                return answer(r, 5);
            if (nlv && secondLockFails)
                return answer(r, 5);
        } else if (nlv == 0 ? level != 1 : top() != level - 1) {
            proto(__LINE__);
            return answer(r, 5);
        }
        lv[nlv++] = level;
        return answer(r, 0);
    }
    case 0x6A: /* unlock logical volume */
        if (cat != lockCat || nlv == 0 || (r->reg_EBX & 0xFF) != 3) {
            proto(__LINE__);
            return answer(r, 1);
        }
        if (unlockFails) {
            unlockFails--;
            return answer(r, 5);
        }
        if (unlock3Fails && top() == 3 && sessions == unlock3Session) {
            if (!unlock3Writes)
                unlock3Writes = writes;
            unlock3Fails--;
            return answer(r, 5);
        }
        if (--nlv == 1 && lv[0] == 1)
            sessions++; /* back at level 1: a write session ended */
        return answer(r, 0);
    case 0x6C: /* get lock flag state (the lock owner only) */
        if (nlv == 0 || lv[0] != 1)
            proto(__LINE__);
        r->reg_EAX = 0;
        if (flag2Session && !flagFired && sessions == flag2Session &&
            top() == 3) {
            flagFired = 1; /* a file was mapped while we held level 1 */
            list1 = list1b;
            list0 = list0b;
            r->reg_EAX = (DWORD)flagValue;
        }
        return answer(r, 0);
    case 0x6D: { /* enumerate open files */
        const tent *L = (r->reg_EDI & 0xFFFF) ? list1 : list0;
        DWORD i = r->reg_ESI & 0xFFFF;

        if (top() != 3 || (r->reg_EBX & 0xFF) != 3 || r->reg_EDI > 1 || !buf)
            proto(__LINE__);
        if (cat == 0x08)
            cat08Enum++;
        if (cat == 0x48 && enum48Fails)
            return answer(r, 1);
        if (enumFail)
            return answer(r, enumFail);
        if (endlessList) {
            copy_str(buf, "C:\\HSDEFRAG.EXE");
            r->reg_ECX = 2;
            return answer(r, 0);
        }
        if (allIndex) {
            int unmovable = (r->reg_EDI & 0xFFFF) != 0;

            e = nth(every, i);
            if (!e || (unmovable && e->cx == 0))
                return answer(r, endCode);
            copy_str(buf, e->path);
            r->reg_EAX = 0x0040;
            r->reg_ECX = unmovable ? e->cx : 0;
            return answer(r, 0);
        }
        e = nth(L, i);
        if (!e)
            return answer(r, endCode);
        copy_str(buf, e->path);
        r->reg_EAX = 0x0040;
        r->reg_ECX = e->cx;
        return answer(r, 0);
    }
    case 0x6E: /* find swap file */
        if (!buf) {
            proto(__LINE__);
            return answer(r, 1);
        }
        copy_str(buf, "C:\\WIN386.SWP");
        r->reg_EAX = 3;
        r->reg_EBX = 2;
        r->reg_ECX = 0;
        return answer(r, 0);
    case 0x71: /* get first cluster: level 3 only */
        if (top() != 3) {
            proto(__LINE__);
            return answer(r, 5);
        }
        if (r->reg_EBX > 2 || !buf)
            proto(__LINE__);
        if (buf[0] && buf[1] != ':') {
            /* Resolved on the current drive (A:): a file of another disk. */
            r->reg_EAX = 1234;
            r->reg_EDX = 0;
            return answer(r, 0);
        }
        for (k = 0; k < sizeof(firstCl) / sizeof(firstCl[0]); k++)
            if (same_path(firstCl[k].path, buf)) {
                r->reg_EAX = firstCl[k].cl & 0xFFFF;
                r->reg_EDX = firstCl[k].cl >> 16;
                return answer(r, 0);
            }
        return answer(r, 5);
    default:
        proto(__LINE__);
        return answer(r, 1);
    }
}

static BOOL t_ioctl(HANDLE h, DWORD code, LPVOID in, DWORD inSize,
                    LPVOID outBuf, DWORD outSize, LPDWORD ret,
                    LPOVERLAPPED ov)
{
    DIOC_REGISTERS *r = (DIOC_REGISTERS *)in;

    (void)ov;
    if (h != FAKE_VWIN32 || !r || in != outBuf || inSize != sizeof(*r) ||
        outSize != sizeof(*r)) {
        proto(__LINE__);
        return FALSE;
    }
    if (ret)
        *ret = sizeof(*r);
    if (code == VWIN32_DIOC_DOS_DRIVEINFO)
        return sim_disk_io(r);
    if (code == VWIN32_DIOC_DOS_IOCTL)
        return sim_ioctl(r);
    if (code == VWIN32_DIOC_DOS_INT25)
        return sim_disk_io_int(r, 0);
    if (code == VWIN32_DIOC_DOS_INT26)
        return sim_disk_io_int(r, 1);
    proto(__LINE__);
    return FALSE;
}

static HANDLE t_create(LPCSTR name, DWORD access, DWORD share,
                       LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                       HANDLE tmpl)
{
    (void)access; (void)share; (void)sa; (void)disp; (void)flags; (void)tmpl;
    return same_path(name, "\\\\.\\vwin32") ? FAKE_VWIN32
                                             : INVALID_HANDLE_VALUE;
}

static BOOL t_close(HANDLE h)
{
    if (h != FAKE_VWIN32)
        proto(__LINE__);
    return TRUE;
}

static DWORD t_module(HMODULE m, LPSTR buf, DWORD cap)
{
    DWORD n = str_len(selfPath);

    if (m != NULL || cap <= n)
        return 0;
    copy_str(buf, selfPath);
    return n;
}

static UINT t_sysdir(LPSTR buf, UINT cap)
{
    DWORD n = str_len(sysDir);

    if (cap <= n)
        return n + 1;
    copy_str(buf, sysDir);
    return n;
}

static DWORD t_short(LPCSTR in, LPSTR buf, DWORD cap)
{
    const char *res = in;
    DWORD n;

    if (top() == 3)
        proto(__LINE__); /* goes through the file system */
    shortCalls++;
    if (same_path(in, "C:\\Long File Name.txt"))
        res = "C:\\LONGFI~1.TXT";
    else if (same_path(in, "C:\\GONE.DLL")) {
        goneDeleted = 1; /* closed and deleted: gone from the next list */
        SetLastError(ERROR_FILE_NOT_FOUND);
        return 0;
    } else if (same_path(in, "C:\\GHOST.DLL")) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return 0;
    }
    n = str_len(res);
    if (cap <= n)
        return n + 1;
    copy_str(buf, res);
    return n;
}

static void t_sleep(DWORD ms)
{
    (void)ms;
    if (top() == 3)
        proto(__LINE__);
}

static void capture(const char *s, DWORD n)
{
    if (top() == 3)
        proto(__LINE__); /* console output while level 3 is held */
    while (n-- && outLen + 1 < sizeof(out))
        out[outLen++] = *s++;
}

static BOOL t_write(HANDLE h, LPCVOID data, DWORD n, LPDWORD written,
                    LPOVERLAPPED ov)
{
    (void)h; (void)ov;
    capture((const char *)data, n);
    *written = n;
    return TRUE;
}

static BOOL t_console(HANDLE h, const VOID *data, DWORD n, LPDWORD written,
                      LPVOID r)
{
    const wchar_t *w = (const wchar_t *)data;
    DWORD i;

    (void)h; (void)r;
    for (i = 0; i < n; i++) {
        char ch = (char)w[i];

        capture(&ch, 1);
    }
    *written = n;
    return TRUE;
}

static BOOL t_mode(HANDLE h, LPDWORD mode)
{
    (void)h;
    *mode = 0;
    return FALSE; /* redirected output */
}

/* ----------------------------------------------------------------- checks */

static int mem_read(void *ctx, fat_u32 s, fat_u32 n, void *buf)
{
    (void)ctx;
    if (s + n > totalSecs)
        return 0;
    memcpy(buf, sec(s), (size_t)n * BPS);
    return 1;
}

static int mem_write(void *ctx, fat_u32 s, fat_u32 n, const void *buf)
{
    (void)ctx; (void)s; (void)n; (void)buf;
    return 0;
}

/* Every file intact, FRAG.TXT contiguous, kept files where they were, and no
 * cluster lost or leaked. Returns 0 or the failing line. */
static int check_files(const int *keep)
{
    fat_volume fv;
    fat_dev dev;
    fat_u32 used = 0, want = fat16 ? 0 : 1, c; /* FAT32: the root folder */
    int f, k, line = 0;

    memset(&dev, 0, sizeof(dev));
    dev.read = mem_read;
    dev.write = mem_write;
    dev.bytesPerSector = BPS;
    if (!fat_mount(&fv, &dev))
        return __LINE__;
    for (f = 0; f < NFILES && !line; f++) {
        fat_u32 cl, i;

        want += (fat_u32)files[f].n;
        if (!fat_lookup(&fv, files[f].path, &cl)) {
            line = __LINE__;
            break;
        }
        for (k = 0; k < files[f].n && !line; k++) {
            fat_u32 nx;

            if (!fat_cluster_valid(&fv, cl) ||
                (keep[f] && cl != files[f].cl[k])) {
                line = __LINE__;
                break;
            }
            for (i = 0; i < BPS; i++)
                if (clus(cl)[i] != pattern(f, k, i)) {
                    line = __LINE__;
                    break;
                }
            nx = fat_get(&fv, cl);
            if (k + 1 == files[f].n ? !fat_is_eoc(&fv, nx)
                                    : (f == F_FRAG && !allowFrag &&
                                       nx != cl + 1))
                line = __LINE__;
            cl = nx;
        }
    }
    for (c = 2; !line && c < fv.countOfClusters + 2; c++)
        if (fat_get(&fv, c) != 0)
            used++;
    if (!line && used != want)
        line = __LINE__;
    fat_unmount(&fv);
    return line;
}

static int wcontains(const wchar_t *s, const wchar_t *part)
{
    size_t n = wcslen(part);

    for (; s && *s; s++)
        if (!memcmp(s, part, n * sizeof(wchar_t)))
            return 1;
    return 0;
}

static void reset(void)
{
    build();
    list1 = list1b = std1;
    list0 = list0b = std0;
    endCode = ERR_ACCESS_DENIED;
    enumFail = 0;
    enum48Fails = 0;
    allIndex = 0;
    inUse = 1;
    secondLockFails = unlockFails = endlessList = 0;
    flag2Session = 0;
    flagValue = 2;
    flagFired = 0;
    goneDeleted = 0;
    unlock3Session = unlock3Fails = unlock3Writes = 0;
    cat48Calls = 0;
    allowFrag = 0;
    selfPath = "C:\\HSDEFRAG.EXE";
    sysDir = "C:\\WINDOWS\\SYSTEM";
    nlv = sessions = protoLine = writes = cat08Enum = shortCalls = 0;
    noDos7305 = 0;
    int25Calls = int26Calls = 0;
    outLen = 0;
    g_verbose = 0;
}

static const int keepInUse[NFILES] = { 1, 1, 1, 0, 0, 1, 1, 1, 1 };
static const int keepMapped[NFILES] = { 1, 1, 1, 1, 0, 1, 1, 1, 1 };
static const int keepSwap[NFILES] = { 0, 0, 0, 0, 0, 0, 1, 0, 0 };
static const int keepNone[NFILES] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };

/* A run that must defragment; returns 0 or the failing line. */
static int expect_done(const int *keep)
{
    int rc = cmd_fat_defrag(L'C');

    if (protoLine)
        return protoLine;
    if (rc != 0 || nlv != 0 || writes == 0)
        return __LINE__;
    return check_files(keep);
}

/* A run that must stop before writing anything. */
static int expect_refused(const wchar_t *why)
{
    int rc;

    memcpy(snap, disk, (size_t)TOTAL * BPS);
    rc = cmd_fat_defrag(L'C');
    if (protoLine)
        return protoLine;
    if (rc != 2 || nlv != 0 || writes != 0 || !wcontains(g_w.failWhat, why))
        return __LINE__;
    if (memcmp(snap, disk, (size_t)TOTAL * BPS))
        return __LINE__;
    return 0;
}

/* The in-use set of the last run: exactly the expected first clusters. */
static int pins_are(const int *pinned)
{
    fat_u32 n = 0;
    int f;

    for (f = 0; f < NFILES; f++) {
        if (hook_unmovable(&g_w, files[f].cl[0]) != pinned[f])
            return 0;
        n += (fat_u32)pinned[f];
    }
    return g_w.nPins == n;
}

static int put_num(char *p, int v)
{
    int n = 0, d = 1;

    while (v / d >= 10)
        d *= 10;
    for (; d; d /= 10)
        p[n++] = (char)('0' + v / d % 10);
    return n;
}

static int report(int scenario, int line)
{
    static const char head[] = "Windows 9x layer regression failed: scenario ";
    char msg[64];
    DWORD written;
    int n = 0;

    n += put_num(msg + n, scenario);
    copy_str(msg + n, ", line ");
    n += 7;
    n += put_num(msg + n, line);
    msg[n++] = '\r';
    msg[n++] = '\n';
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), head, sizeof(head) - 1,
              &written, NULL);
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, (DWORD)n, &written, NULL);
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), out, outLen, &written, NULL);
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    static const char ok[] = "Windows 9x layer regressions passed (22 "
                             "scenarios on a simulated VWIN32).\r\n";
    DWORD written;
    int line;

    (void)argc; (void)argv;
    disk = (fat_u8 *)malloc((size_t)TOTAL * BPS);
    snap = (fat_u8 *)malloc((size_t)TOTAL * BPS);
    if (!disk || !snap)
        return report(0, __LINE__);

    /* 1. The list of open files ends with ERROR_ACCESS_DENIED, as Microsoft
     *    documents: everything in use is pinned, the rest is defragmented. */
    reset();
    g_verbose = 1;
    line = expect_done(keepInUse);
    if (!line && (!pins_are(keepInUse) || cat08Enum || !shortCalls ||
                  sessions < 2))
        line = __LINE__;
    if (line)
        return report(1, line);

    /* 2. ... or with ERROR_NO_MORE_FILES. */
    reset();
    endCode = ERR_NO_MORE_FILES;
    line = expect_done(keepInUse);
    if (!line && (!pins_are(keepInUse) || cat08Enum))
        line = __LINE__;
    if (line)
        return report(2, line);

    /* 3. The 48h form of Enumerate Open Files fails: the 08h form is used. */
    reset();
    enum48Fails = 1;
    line = expect_done(keepInUse);
    if (!line && (!pins_are(keepInUse) || !cat08Enum))
        line = __LINE__;
    if (line)
        return report(3, line);

    /* 4. A program maps OPEN.LOG during the run: it is listed again and
     *    pinned before anything else is written. (GONE.DLL is already gone,
     *    so session 1 is the first one of the defragmentation itself.) */
    reset();
    goneDeleted = 1;
    flag2Session = 1;
    list1b = mapped1;
    list0b = mapped0;
    line = expect_done(keepMapped);
    if (!line && (!flagFired || !hook_unmovable(&g_w, files[F_OPEN].cl[0])))
        line = __LINE__;
    if (line)
        return report(4, line);

    /* 5. hsdefrag.exe runs from this drive but is not on the list: the list
     *    is incomplete, so nothing is moved. */
    reset();
    list1 = list0 = noself;
    line = expect_refused(L"hsdefrag.exe");
    if (line)
        return report(5, line);

    /* 6. Nothing listed on the system drive: refused too. */
    reset();
    selfPath = "A:\\HSDEFRAG.EXE";
    list1 = list0 = none;
    line = expect_refused(L"system drive");
    if (line)
        return report(6, line);

    /* 7. ... but an idle data drive may have nothing open. */
    reset();
    selfPath = "A:\\HSDEFRAG.EXE";
    sysDir = "A:\\WINDOWS\\SYSTEM";
    list1 = list0 = none;
    line = expect_done(keepSwap);
    if (line)
        return report(7, line);

    /* 8. A real failure of Enumerate Open Files stops the run. */
    reset();
    enumFail = 0x1F;
    line = expect_refused(L"cannot list the files in use");
    if (!line && g_w.failCode != 0x1F)
        line = __LINE__;
    if (line)
        return report(8, line);

    /* 9. Nothing open on the volume: the exclusive level 0 lock is used and
     *    every file may move. */
    reset();
    inUse = 0;
    line = expect_done(keepNone);
    if (!line && sessions)
        line = __LINE__;
    if (line)
        return report(9, line);

    /* 10. FileIndex of the unmovable list counts every open file (it answers
     *     "end" at a movable one) and CX of the full list is always 0: the
     *     files in use are still all found. */
    reset();
    allIndex = 1;
    line = expect_done(keepInUse);
    if (!line && !pins_are(keepInUse))
        line = __LINE__;
    if (line)
        return report(10, line);

    reset();
    inUse = 0;
    secondLockFails = 1;
    if (cmd_fat_defrag(L'C') != 2 || writes || nlv || protoLine)
        return report(11, __LINE__);

    reset();
    selfPath = "";
    line = expect_refused(L"identify the running program");
    if (line)
        return report(12, line);

    reset();
    selfPath = "C:\\MISSING.EXE";
    line = expect_refused(L"locate the running program");
    if (line)
        return report(13, line);

    reset();
    sysDir = "";
    line = expect_refused(L"identify the Windows system drive");
    if (line)
        return report(14, line);

    reset();
    endlessList = 1;
    line = expect_refused(L"did not terminate");
    if (line)
        return report(15, line);

    /* 16. The unlock keeps failing (a single refusal is just tried again). */
    reset();
    unlockFails = MAX_UNLOCK;
    line = expect_refused(L"release the volume lock");
    if (line)
        return report(16, line);

    /* 17. A pre-OSR2 system without the DOS 7.1 absolute disk I/O: the
     *     classic Int 25h/26h does the work instead, and the run ends
     *     just as it does with the 7305h call. */
    reset();
    noDos7305 = 1;
    line = expect_done(keepInUse);
    if (!line && (!int25Calls || !int26Calls))
        line = __LINE__;
    if (line)
        return report(17, line);

    /* 18. A file Windows lists in use cannot be found (a name we fail to
     *     translate looks just like a deleted file): it stays listed, so the
     *     run stops instead of leaving that file unprotected. */
    reset();
    list1 = list1b = ghost1;
    line = expect_refused(L"cannot find the file in use");
    if (!line && !same_path(g_w.failPath, "C:\\GHOST.DLL"))
        line = __LINE__;
    if (line)
        return report(18, line);

    /* 19. Like 4, but the lock flag reports 1 ("written"), not 2, after the
     *     mapping: OPEN.LOG must be listed again and pinned all the same. */
    reset();
    goneDeleted = 1;
    flag2Session = 2;
    flagValue = 1;
    list1b = mapped1;
    list0b = mapped0;
    line = expect_done(keepMapped);
    if (!line && (!flagFired || !hook_unmovable(&g_w, files[F_OPEN].cl[0])))
        line = __LINE__;
    if (line)
        return report(19, line);

    /* 20. Mid-run, the unlock from level 3 is refused twice: tried again at
     *     once, it succeeds, and the run goes on with nothing printed under
     *     level 3 (verbose output is on). */
    reset();
    goneDeleted = 1;
    g_verbose = 1;
    unlock3Session = 2;
    unlock3Fails = MAX_UNLOCK - 1;
    line = expect_done(keepInUse);
    if (!line && (unlock3Fails || !unlock3Writes))
        line = __LINE__;
    if (line)
        return report(20, line);

    /* 21. ... refused every time: level 3 stays held, nothing is printed
     *     until it is released at the end, and the run stops. */
    reset();
    goneDeleted = 1;
    g_verbose = 1;
    unlock3Session = 2;
    unlock3Fails = MAX_UNLOCK;
    allowFrag = 1;
    line = cmd_fat_defrag(L'C') != 4 ? __LINE__ : 0;
    if (protoLine)
        line = protoLine;
    if (!line && (nlv || unlock3Fails || !unlock3Writes ||
                  !wcontains(g_w.failWhat, L"release the volume lock")))
        line = __LINE__;
    if (!line)
        line = check_files(keepInUse);
    if (line)
        return report(21, line);

    /* 22. A FAT16 volume: every lock and unlock takes category 08h (the
     *     simulation refuses 48h there), and so does everything else. */
    reset();
    fat16 = 1;
    build();
    g_verbose = 1;
    line = expect_done(keepInUse);
    if (!line && (!pins_are(keepInUse) || cat48Calls || !cat08Enum))
        line = __LINE__;
    fat16 = 0;
    if (line)
        return report(22, line);

    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), ok, sizeof(ok) - 1, &written,
              NULL);
    return 0;
}

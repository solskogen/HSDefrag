/*
 * fatwin9x.c - Windows 9x FAT analyze/defrag over VWIN32 raw sector I/O.
 *
 * Windows 95/98/Me expose no NT-style defragmentation IOCTLs, so we move
 * clusters ourselves: read and write absolute logical sectors through the
 * VWIN32 VxD and edit the FAT and directory entries directly (see fatfs.c /
 * fatdefrag.c for the portable engine this layer feeds).
 *
 * Sector access uses the DOS 7.1 absolute read/write (Int 21h AX=7305h), which
 * understands FAT32, and falls back to the classic Int 25h/26h for FAT12/16 on
 * pre-OSR2 systems.
 *
 * Locking follows Microsoft's rules for disk utilities ("Exclusive Volume
 * Locking"): direct writes are made only under a level 0 lock (granted only
 * when no file on the volume is open) or a level 3 lock. The system drive
 * always has open files, so there a level 1 lock is held for the whole run
 * and, for every short burst of writes, raised to level 2 and then 3. Taking
 * level 3 makes Windows flush its caches and set aside its open files; the
 * lock flag then tells whether anyone changed the volume since the last burst
 * (the FAT is then re-read). After writing, the lock drops back to level 1 so
 * Windows keeps running in between, like it does for the Windows 95 Disk
 * Defragmenter. Files Windows must keep in place (the swap file, mapped files,
 * running programs and DLLs) are listed inside level 3 and never moved.
 */
#include "compat.h"
#include "util.h"
#include "fatfs.h"
#include "fatdefrag.h"
#include "fatreport.h"
#include "fatwin9x.h"

/* VWIN32 DeviceIoControl function codes. */
#define VWIN32_DIOC_DOS_IOCTL     1   /* Int 21h AX=4400h..4411h          */
#define VWIN32_DIOC_DOS_INT25     2   /* absolute disk read  (FAT12/16)   */
#define VWIN32_DIOC_DOS_INT26     3   /* absolute disk write (FAT12/16)   */
#define VWIN32_DIOC_DOS_DRIVEINFO 6   /* Int 21h AX=7305h (FAT32-aware)   */

/* Int 21h AX=440Dh minor codes (category 08h, or 48h on FAT32). */
#define IOCTL_LOCK     0x4A
#define IOCTL_UNLOCK   0x6A
#define IOCTL_LOCKFLAG 0x6C   /* get lock flag state                     */
#define IOCTL_ENUMOPEN 0x6D   /* enumerate open files                    */
#define IOCTL_SWAPFILE 0x6E   /* find swap file                          */
#define IOCTL_FIRSTCL  0x71   /* get first cluster (OSR2 and later)      */

#define ERR_ACCESS_DENIED 0x05
#define ERR_NO_MORE_FILES 0x12

#define MAX_PINS  1024        /* in-use files that must not move         */
#define MAX_UNRES 128         /* paths resolved outside level 3          */
#define MAX_MISS  3           /* bursts in a row with an unfindable file */
#define MAX_UNLOCK 3          /* tries of one unlock call                */

/* Register image exchanged with VWIN32 (fixed field order). */
typedef struct {
    DWORD reg_EBX;
    DWORD reg_EDX;
    DWORD reg_ECX;
    DWORD reg_EAX;
    DWORD reg_EDI;
    DWORD reg_ESI;
    DWORD reg_Flags;
} DIOC_REGISTERS;

/* DOS absolute-I/O parameter block (must be byte-packed). */
#pragma pack(push, 1)
typedef struct {
    DWORD StartSector;
    WORD  nSectors;
    void *Buffer;
} DISKIO;
#pragma pack(pop)

typedef struct {
    HANDLE         h;          /* \\.\vwin32 device                       */
    int            driveNum;   /* 1-based (A=1, C=3)                      */
    wchar_t        drive;
    int            lockCat;    /* 0x08 (FAT12/16) or 0x48 (FAT32)         */
    int            levels;     /* nested locks held                       */
    int            unlockFailed;
    int            exclusive;  /* level 0: nobody else uses the volume    */
    int            needEnum;   /* list the in-use files again             */
    int            forceReload;/* re-read the FAT in the next section     */
    fat_u32        nPins;
    fat_u32        pins[MAX_PINS]; /* sorted first clusters of in-use files */
    fat_u32        listed;     /* in-use files Windows reported here      */
    fat_u32        selfCl;     /* hsdefrag.exe when it is on this drive   */
    int            sysDrive;   /* the Windows system folder is here       */
    int            nUnres;
    int            unresMiss;  /* bursts in a row that listed a file which
                                  could not be found afterwards           */
    char           unres[MAX_UNRES][MAX_PATH];
    char           path[MAX_PATH + 4];
    const wchar_t *failWhat;   /* why a write section failed              */
    char           failPath[MAX_PATH];
    DWORD          failCode;
    fat_dev        dev;
    fat_volume     fv;
} fat_win9x;

static fat_win9x    g_w;
static volatile int g_cancel;

/* --------------------------------------------------------- raw sector I/O */

static int vwin32_call(fat_win9x *w, DWORD code, DIOC_REGISTERS *reg)
{
    DWORD ret = 0;

    /* Carry preset: a call that VWIN32 does not carry out reads as failed. */
    reg->reg_Flags = 1;
    return DeviceIoControl(w->h, code, reg, sizeof(*reg), reg, sizeof(*reg),
                           &ret, NULL) &&
           !(reg->reg_Flags & 1);
}

static int vwin32_io_once(fat_win9x *w, fat_u32 sector, fat_u32 count,
                          void *buf, int write)
{
    DISKIO dio;
    DIOC_REGISTERS reg;

    dio.StartSector = sector;
    dio.nSectors = (WORD)count;
    dio.Buffer = buf;

    /* Preferred path: DOS 7.1 absolute read/write (handles FAT32). */
    memset(&reg, 0, sizeof(reg));
    reg.reg_EAX = 0x7305;
    reg.reg_EBX = (DWORD)(DWORD_PTR)&dio;
    reg.reg_ECX = 0xFFFF;
    reg.reg_EDX = (DWORD)w->driveNum;          /* 1-based */
    reg.reg_ESI = write ? 0x0001 : 0x0000;
    if (vwin32_call(w, VWIN32_DIOC_DOS_DRIVEINFO, &reg))
        return 1;

    /* Fallback: classic Int 25h/26h for FAT12/16 on pre-OSR2 systems. The
     * DIOC code carries the operation, so the register image only names the
     * drive: AL the 0-based number, DX the 1-based one. */
    memset(&reg, 0, sizeof(reg));
    reg.reg_EAX = (DWORD)(w->driveNum - 1);    /* 0-based */
    reg.reg_EBX = (DWORD)(DWORD_PTR)&dio;
    reg.reg_ECX = 0xFFFF;
    reg.reg_EDX = (DWORD)w->driveNum;          /* 1-based */
    return vwin32_call(w, write ? VWIN32_DIOC_DOS_INT26 : VWIN32_DIOC_DOS_INT25,
                       &reg);
}

/* On a live volume a transfer can transiently fail; retry a few times like
 * the Windows disk utilities do. (No waiting: this may run under level 3.) */
static int vwin32_io(fat_win9x *w, fat_u32 sector, fat_u32 count, void *buf,
                     int write)
{
    int tries;

    for (tries = 0; tries < 5; tries++)
        if (vwin32_io_once(w, sector, count, buf, write))
            return 1;
    return 0;
}

static int w9x_read(void *ctx, fat_u32 sector, fat_u32 count, void *buf)
{
    return vwin32_io((fat_win9x *)ctx, sector, count, buf, 0);
}

static int w9x_write(void *ctx, fat_u32 sector, fat_u32 count, const void *buf)
{
    return vwin32_io((fat_win9x *)ctx, sector, count, (void *)buf, 1);
}

/* ------------------------------------------------------------ IOCTL, locks */

/* Enumerate Open Files reports the end of the list as ERROR_ACCESS_DENIED
 * ("FileIndex exceeds the number of open files"), or ERROR_NO_MORE_FILES. */
static int is_list_end(int minor, DWORD ax)
{
    ax &= 0xFFFF;
    return minor == IOCTL_ENUMOPEN &&
           (ax == ERR_ACCESS_DENIED || ax == ERR_NO_MORE_FILES);
}

/*
 * Generic IOCTL (Int 21h AX=440Dh) on our drive. Lock and unlock use the
 * category the volume requires (48h on FAT32); the other calls fall back to
 * 08h, the category of the original Windows 95 interface, unless the answer
 * was a normal end of a list.
 */
static int dos_ioctl(fat_win9x *w, int minor, DWORD ebx, DWORD edx, DWORD esi,
                     DWORD edi, DIOC_REGISTERS *out)
{
    DIOC_REGISTERS reg;
    int cat = w->lockCat, ok;

    for (;;) {
        memset(&reg, 0, sizeof(reg));
        reg.reg_EAX = 0x440D;
        reg.reg_EBX = ebx;
        reg.reg_ECX = (DWORD)((cat << 8) | minor);
        reg.reg_EDX = edx;
        reg.reg_ESI = esi;
        reg.reg_EDI = edi;
        ok = vwin32_call(w, VWIN32_DIOC_DOS_IOCTL, &reg);
        if (ok || cat == 0x08 || minor == IOCTL_LOCK ||
            minor == IOCTL_UNLOCK || is_list_end(minor, reg.reg_EAX))
            break;
        cat = 0x08;
    }
    if (out)
        *out = reg;
    return ok;
}

/* Take one more lock level (each must be released with unlock_level). */
static int lock_level(fat_win9x *w, int level, DWORD perm)
{
    if (!dos_ioctl(w, IOCTL_LOCK, (DWORD)(w->driveNum | (level << 8)), perm, 0,
                   0, NULL))
        return 0;
    w->levels++;
    return 1;
}

/* A failed unlock from level 3 leaves Windows unable to touch the disk, and
 * nothing may be printed until it is released, so a refused unlock is tried
 * again a few times at once (no waiting: this may run under level 3). */
static int unlock_level(fat_win9x *w)
{
    int tries;

    if (w->levels <= 0)
        return 1;
    for (tries = 0; tries < MAX_UNLOCK; tries++)
        if (dos_ioctl(w, IOCTL_UNLOCK, (DWORD)w->driveNum, 0, 0, 0, NULL)) {
            w->levels--;
            return 1;
        }
    w->unlockFailed = 1;
    w->failWhat = L"cannot release the volume lock";
    return 0;
}

static void unlock_all(fat_win9x *w)
{
    while (w->levels > 0 && unlock_level(w))
        ;
}

/* Get Lock Flag State, which also resets it: 0 nothing happened since the
 * last call, 1 something was written, 2 a file was mapped or a 32-bit program
 * or DLL was opened; -1 when unknown. */
static int lock_flag(fat_win9x *w)
{
    DIOC_REGISTERS r;

    if (!dos_ioctl(w, IOCTL_LOCKFLAG, (DWORD)w->driveNum, 0, 0, 0, &r))
        return -1;
    return (int)(r.reg_EAX & 0xFFFF);
}

/*
 * Lock the volume for the run: the exclusive level 0 lock when nothing on it
 * is open (an idle or removable drive; a second level 0 lock carrying the
 * write permission then authorizes direct writes), otherwise level 1 with
 * "others may still write" (DX bit 0) for the per-write protocol.
 */
static int lock_volume(fat_win9x *w)
{
    int i;

    if (lock_level(w, 0, 0)) {
        if (!lock_level(w, 0, 0x0001)) {
            unlock_all(w);
            return 0;
        }
        w->exclusive = 1;
        return 1;
    }
    for (i = 0; i < 20; i++) {
        if (lock_level(w, 1, 0x0001))
            return 1;
        Sleep(100);
    }
    return 0;
}

/* ----------------------------------------------------------- in-use files */

static void set_fail(fat_win9x *w, const wchar_t *what, const char *path,
                     DWORD code)
{
    w->failWhat = what;
    w->failCode = code;
    w->failPath[0] = 0;
    if (path) {
        memcpy(w->failPath, path, MAX_PATH);
        w->failPath[MAX_PATH - 1] = 0;
    }
}

static int pin_add(fat_win9x *w, fat_u32 cl)
{
    fat_u32 i, j;

    if (cl == 0)
        return 1;
    for (i = 0; i < w->nPins && w->pins[i] < cl; i++)
        ;
    if (i < w->nPins && w->pins[i] == cl)
        return 1;
    if (w->nPins >= MAX_PINS)
        return 0;
    for (j = w->nPins; j > i; j--)
        w->pins[j] = w->pins[j - 1];
    w->pins[i] = cl;
    w->nPins++;
    return 1;
}

static int hook_unmovable(void *ctx, fat_u32 start)
{
    const fat_win9x *w = (const fat_win9x *)ctx;
    fat_u32 lo = 0, hi = w->nPins;

    while (lo < hi) {
        fat_u32 mid = lo + (hi - lo) / 2;

        if (w->pins[mid] < start)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < w->nPins && w->pins[lo] == start;
}

/* First cluster of a file: ask VFAT (Get First Cluster, OEM then ANSI
 * names), else walk the directories ourselves. A cluster number from VFAT is
 * checked against the volume, as its documentation asks. */
static int resolve_path(fat_win9x *w, const char *path, fat_u32 *cl)
{
    DIOC_REGISTERS r;
    DWORD cs;

    for (cs = 1;; cs--) {
        if (dos_ioctl(w, IOCTL_FIRSTCL, cs, (DWORD)(DWORD_PTR)path, 0, 0, &r)) {
            fat_u32 c = ((r.reg_EDX & 0xFFFF) << 16) | (r.reg_EAX & 0xFFFF);

            if (fat_cluster_valid(&w->fv, c)) {
                *cl = c;
                return 1;
            }
        }
        if (cs == 0)
            break;
    }
    return fat_lookup(&w->fv, path, cl);
}

static int same_path(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/*
 * Pin the file named in w->path, or leave it for resolve_unres(). Returns 1
 * when done, 0 when the file is not on this drive, -1 on failure.
 */
static int pin_path(fat_win9x *w)
{
    char *p = w->path;
    fat_u32 cl;
    int i;

    p[MAX_PATH - 1] = 0;
    if (!p[0])
        return 0;
    if (p[1] == ':') {
        if ((p[0] & ~0x20) != (int)w->drive)
            return 0; /* on another drive */
    } else {
        /* No drive letter: the file is on the drive being listed. */
        int n, lead = p[0] == '\\' ? 2 : 3;

        for (n = 0; p[n]; n++)
            ;
        if (n + lead >= MAX_PATH) {
            set_fail(w, L"the name of a file in use is too long", p, 0);
            return -1;
        }
        memmove(p + lead, p, (size_t)n + 1);
        p[0] = (char)w->drive;
        p[1] = ':';
        if (lead == 3)
            p[2] = '\\';
    }
    if (resolve_path(w, p, &cl)) {
        if (pin_add(w, cl))
            return 1;
    } else {
        for (i = 0; i < w->nUnres; i++)
            if (same_path(w->unres[i], p))
                return 1;
        if (w->nUnres < MAX_UNRES) {
            memcpy(w->unres[w->nUnres++], p, MAX_PATH);
            return 1;
        }
    }
    set_fail(w, L"too many files in use", NULL, 0);
    return -1;
}

/*
 * One pass of Enumerate Open Files. Type 0 lists every open file with its
 * kind in CX (0 normal, 1 mapped, 2 32-bit program or DLL, 4 swap file): all
 * but the normal ones are pinned. Type 1 lists the unmovable files
 * (memory-mapped files, running 32-bit programs and DLLs): all are pinned.
 * Microsoft does not say whether FileIndex then counts only the unmovable
 * files or every open file, so below "skip" (the number of open files) an
 * end-of-list answer is passed over. *end receives where the list ended.
 */
static int enum_pass(fat_win9x *w, DWORD type, DWORD skip, DWORD *end)
{
    DIOC_REGISTERS r;
    DWORD i;

    for (i = 0; i < 0x10000; i++) {
        int got;

        memset(w->path, 0, sizeof(w->path));
        if (!dos_ioctl(w, IOCTL_ENUMOPEN, (DWORD)w->driveNum,
                       (DWORD)(DWORD_PTR)w->path, i, type, &r)) {
            if (!is_list_end(IOCTL_ENUMOPEN, r.reg_EAX)) {
                set_fail(w, L"cannot list the files in use", NULL,
                         r.reg_EAX & 0xFFFF);
                return 0;
            }
            if (i < skip)
                continue;
            break;
        }
        if (type == 0 && (r.reg_ECX & 0xFFFF) == 0)
            continue; /* a normal file: movable under level 3 */
        got = pin_path(w);
        if (got < 0)
            return 0;
        w->listed += (fat_u32)got;
    }
    if (i == 0x10000) {
        set_fail(w, L"the list of files in use did not terminate", NULL, 0);
        return 0;
    }
    if (end)
        *end = i;
    return 1;
}

/*
 * Under level 3: pin the swap file and every open file that Windows needs to
 * keep in place. Returns 1 when done, 0 when some paths are left for
 * resolve_unres(), and -1 on failure.
 */
static int enum_pins(fat_win9x *w)
{
    DIOC_REGISTERS r;
    DWORD open = 0;

    w->nPins = 0;
    w->nUnres = 0;
    w->listed = 0;
    memset(w->path, 0, sizeof(w->path));
    if (dos_ioctl(w, IOCTL_SWAPFILE, (DWORD)w->driveNum,
                  (DWORD)(DWORD_PTR)w->path, 0, 0, &r) &&
        (r.reg_EAX & 0xFFFF) != 1 && pin_path(w) < 0)
        return -1;
    if (!enum_pass(w, 0, 0, &open) || !enum_pass(w, 1, open, NULL))
        return -1;
    return w->nUnres ? 0 : 1;
}

/*
 * First cluster of the file at an ANSI path, through its short name (this
 * goes through the file system, so never under level 3). Returns 1 when
 * found, 0 when the file does not exist, -1 on failure (*err may be set).
 */
static int resolve_ansi(fat_win9x *w, const char *path, fat_u32 *cl,
                        DWORD *err)
{
    char shortName[MAX_PATH], oem[MAX_PATH];
    wchar_t wide[MAX_PATH];
    DWORD n = GetShortPathNameA(path, shortName, MAX_PATH);

    *err = 0;
    if (n == 0) {
        *err = GetLastError();
        return (*err == ERROR_FILE_NOT_FOUND || *err == ERROR_PATH_NOT_FOUND)
                   ? 0 : -1;
    }
    if (n >= MAX_PATH ||
        !MultiByteToWideChar(CP_ACP, 0, shortName, -1, wide, MAX_PATH) ||
        !WideCharToMultiByte(CP_OEMCP, 0, wide, -1, oem, MAX_PATH, NULL,
                             NULL) ||
        !fat_lookup(&w->fv, oem, cl))
        return -1;
    return 1;
}

/*
 * A name from Enumerate Open Files is OEM text, while GetShortPathNameA wants
 * ANSI: convert it first. A character with no ANSI form would turn into a
 * default character that might name another file, so that conversion is not
 * used then. The name exactly as listed (taken as ANSI) is the fallback.
 */
static int resolve_listed(fat_win9x *w, const char *path, fat_u32 *cl,
                          DWORD *err)
{
    char ansi[MAX_PATH];
    wchar_t wide[MAX_PATH];
    BOOL lossy = FALSE;
    int r;

    if (MultiByteToWideChar(CP_OEMCP, 0, path, -1, wide, MAX_PATH) &&
        WideCharToMultiByte(CP_ACP, 0, wide, -1, ansi, MAX_PATH, NULL,
                            &lossy) &&
        !lossy && !same_path(ansi, path)) {
        r = resolve_ansi(w, ansi, cl, err);
        if (r != 0)
            return r;
    }
    return resolve_ansi(w, path, cl, err);
}

/*
 * Under level 2: resolve the remaining paths through their short names.
 * Returns 1 when done, 0 on failure, and -1 when a listed file was not found.
 * That file may have been closed and deleted in the meantime, but a name we
 * merely failed to translate looks the same, and dropping it would leave a
 * file in use unprotected. So the list is taken again in the next burst (a
 * deleted file is gone from it by then), and a file that stays listed but
 * cannot be found stops the run.
 */
static int resolve_unres(fat_win9x *w)
{
    int i, missing = -1;

    for (i = 0; i < w->nUnres; i++) {
        fat_u32 cl;
        DWORD e;
        int r = resolve_listed(w, w->unres[i], &cl, &e);

        if (r == 0) {
            if (missing < 0)
                missing = i;
            continue;
        }
        if (r < 0) {
            set_fail(w, L"cannot find the file in use", w->unres[i], e);
            return 0;
        }
        if (!pin_add(w, cl)) {
            set_fail(w, L"too many files in use", NULL, 0);
            return 0;
        }
    }
    if (missing >= 0) {
        if (++w->unresMiss >= MAX_MISS) {
            set_fail(w, L"cannot find the file in use", w->unres[missing],
                     ERROR_FILE_NOT_FOUND);
            return 0;
        }
        return -1;
    }
    w->nUnres = 0;
    w->unresMiss = 0;
    return 1;
}

/*
 * Cross-check the list from Windows before trusting it: hsdefrag.exe is a
 * running 32-bit program, so it must be on the list when it lives on this
 * drive, and the drive with the Windows system folder always has programs
 * and DLLs in use. With an incomplete list a DLL in use could be moved.
 */
static int pins_plausible(fat_win9x *w)
{
    if (w->selfCl && !hook_unmovable(w, w->selfCl)) {
        set_fail(w, L"Windows did not list hsdefrag.exe among the files in "
                    L"use", NULL, 0);
        return 0;
    }
    if (w->sysDrive && w->listed == 0) {
        set_fail(w, L"Windows listed no files in use on the system drive",
                 NULL, 0);
        return 0;
    }
    return 1;
}

/* Find what pins_plausible() checks (before the first write section). */
static int find_selfcheck(fat_win9x *w)
{
    char path[MAX_PATH];
    fat_u32 cl;
    DWORD n, e;

    w->selfCl = 0;
    n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n <= 2 || n >= MAX_PATH || path[1] != ':') {
        set_fail(w, L"cannot identify the running program", NULL, 0);
        return 0;
    }
    if ((path[0] & ~0x20) == (int)w->drive) {
        if (resolve_ansi(w, path, &cl, &e) <= 0 ||
            !fat_cluster_valid(&w->fv, cl)) {
            set_fail(w, L"cannot locate the running program", path, e);
            return 0;
        }
        w->selfCl = cl;
    }
    n = GetSystemDirectoryA(path, MAX_PATH);
    if (n <= 2 || n >= MAX_PATH || path[1] != ':') {
        set_fail(w, L"cannot identify the Windows system drive", NULL, 0);
        return 0;
    }
    w->sysDrive = (path[0] & ~0x20) == (int)w->drive;
    return 1;
}

/* ---------------------------------------------------- write-section hooks */

/*
 * Raise the lock to level 3 for a burst of writes. Nothing but sector I/O and
 * IOCTLs happens while level 3 is held: no output, no memory allocation, no
 * waiting.
 */
static int hook_begin(void *ctx)
{
    fat_win9x *w = (fat_win9x *)ctx;
    int changed = 0, tries, r;

    if (w->unlockFailed)
        return FAT_WR_FAIL;
    if (g_cancel) {
        set_fail(w, L"cancelled", NULL, 0);
        return FAT_WR_FAIL;
    }
    if (!lock_level(w, 2, 0)) {
        Sleep(250);
        return FAT_WR_BUSY;
    }
    for (tries = 0; tries < 8; tries++) {
        int flag;

        if (!lock_level(w, 3, 0))
            break;
        flag = lock_flag(w);
        if (flag != 0 || w->forceReload) {
            if (!fat_reload(&w->fv)) {
                w->forceReload = 1;
                unlock_level(w);
                unlock_level(w);
                set_fail(w, L"cannot read the FAT again", NULL, 0);
                return FAT_WR_FAIL;
            }
            changed = 1;
            w->forceReload = 0;
            /* Any change may have mapped a file or started a program: a flag
             * of 1 ("written") does not rule that out, so list again. */
            w->needEnum = 1;
        }
        if (!w->needEnum)
            return changed ? FAT_WR_CHANGED : FAT_WR_OK;
        r = enum_pins(w);
        if (r > 0) {
            if (!pins_plausible(w)) {
                unlock_level(w);
                unlock_level(w);
                return FAT_WR_FAIL;
            }
            w->needEnum = 0;
            w->unresMiss = 0;
            return FAT_WR_CHANGED;
        }
        if (!unlock_level(w)) /* back to level 2 */
            return FAT_WR_FAIL;
        if (r < 0) {
            unlock_level(w);
            return FAT_WR_FAIL;
        }
        r = resolve_unres(w);
        if (r < 0)
            break; /* a listed file was not found: list again later */
        if (r == 0 || !pins_plausible(w)) {
            unlock_level(w);
            return FAT_WR_FAIL;
        }
        w->needEnum = 0;
        changed = 1; /* the pins changed: the engine must look again */
    }
    /* The engine learns of a change only from FAT_WR_CHANGED, so one seen
     * here must still be reported by the next section. */
    if (changed)
        w->forceReload = 1;
    unlock_level(w);
    Sleep(250);
    return FAT_WR_BUSY;
}

static void hook_end(void *ctx)
{
    fat_win9x *w = (fat_win9x *)ctx;

    lock_flag(w); /* our own writes must not count as a change next time */
    if (unlock_level(w))
        unlock_level(w);
}

/* ------------------------------------------------------------- reporting */

static const wchar_t *fat_type_name(const fat_volume *fv)
{
    switch (fv->fatType) {
    case FAT_TYPE_12: return L"FAT12";
    case FAT_TYPE_16: return L"FAT16";
    case FAT_TYPE_32: return L"FAT32";
    default:          return L"FAT";
    }
}

/* Widen a path in code page cp for output. */
static const wchar_t *widen(const char *s, UINT cp, wchar_t *buf, int cap)
{
    if (!MultiByteToWideChar(cp, 0, s, -1, buf, cap)) {
        int i;

        for (i = 0; s[i] && i + 1 < cap; i++)
            buf[i] = (wchar_t)(unsigned char)s[i];
        buf[i] = 0;
    }
    return buf;
}

/*
 * Console output while level 3 is held can wait on the disk Windows may not
 * touch, so progress and events are dropped then. That only happens when an
 * unlock failed and the engine goes on to its next step. (The count of locks
 * reaches 3 only with the level 1-2-3 chain: the exclusive mode holds two.)
 */
static int muted(void *ctx)
{
    return ctx && ((const fat_win9x *)ctx)->levels >= 3;
}

/* Report lines are 8-bit text with names as stored on disk (OEM). */
static void report_out(void *ctx, int level, const char *line)
{
    wchar_t w[420];

    (void)ctx;
    widen(line, CP_OEMCP, w, 420);
    if (level == FAT_OUT_VERBOSE)
        uverbose(L"%ls", w);
    else
        uinfo(L"%ls", w);
}

static fat_report report_of(const fat_volume *fv)
{
    fat_report r;

    r.out = report_out;
    r.ctx = NULL;
    r.inUse = "in use";
    r.lostGap = "lost clusters, or a file being written";
    r.bytesPerCluster = fv ? fv->bytesPerCluster : 0;
    return r;
}

static void on_event(void *ctx, int ev, const char *path, int frags,
                     int newFrags)
{
    fat_report r = report_of(NULL); /* events carry no sizes */

    if (muted(ctx))  /* ctx is NULL when analyzing */
        return;
    fat_report_event(&r, ev, path, frags, newFrags);
}

static void print_kept_notes(const fat_volume *fv, const fat_stats *st)
{
    fat_report r = report_of(fv);

    fat_report_kept(&r, st);
}

static void print_free_space(const fat_volume *fv, const fat_stats *st)
{
    fat_report r = report_of(fv);

    fat_report_free_space(&r, st);
}

static void print_gap_notes(const fat_volume *fv, const fat_stats *st)
{
    fat_report r = report_of(fv);

    fat_report_gaps(&r, st);
}

static void on_progress(void *ctx, int phase, fat_u64 done, fat_u64 total)
{
    if (muted(ctx))
        return;
    if (phase == FAT_PH_SCAN)
        uprogress(L"  scanning: %llu files and folders", done);
    else if (phase == FAT_PH_DEFRAG)
        uprogress(L"  defragmenting: %llu of %llu", done, total);
    else
        uprogress(L"  consolidating free space: %llu moved", done);
}




/* The largest gaps left in the free space and what keeps each one open. */

static void print_reason(const fat_win9x *w)
{
    wchar_t wp[MAX_PATH], msg[160];

    if (!w->failWhat)
        return;
    uinfo(L"  reason: %ls", w->failWhat);
    if (w->failPath[0])
        uinfo(L" \"%ls\"", widen(w->failPath, CP_ACP, wp, MAX_PATH));
    if (w->failCode)
        uinfo(L" (%ls)", uwin32_str(w->failCode, msg, 160));
    uinfo(L"\n");
}

/* ---------------------------------------------------------- open / close */

/* Returns 0, or a tool exit code. */
static int open_volume(fat_win9x *w, wchar_t drive)
{
    fat_u8 probe[512];

    memset(w, 0, sizeof(*w));
    if (drive >= L'a' && drive <= L'z')
        drive = (wchar_t)(drive - L'a' + L'A');
    w->drive = drive;
    w->driveNum = (int)(drive - L'A' + 1);
    w->h = CreateFileA("\\\\.\\vwin32", 0, 0, NULL, 0,
                       FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (w->h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();

        uwin32_error(L"cannot open the VWIN32 device", err);
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            uinfo(L"The VWIN32 driver is not present: direct FAT access "
                  L"requires Windows 95/98/Me with 32-bit file system "
                  L"support.\n");
        else if (err == ERROR_ACCESS_DENIED)
            uinfo(L"Access to the VWIN32 device was denied; run with the "
                  L"rights of an administrator.\n");
        else
            uinfo(L"Direct FAT access requires Windows 95/98/Me.\n");
        return 2;
    }
    if (!vwin32_io(w, 0, 1, probe, 0)) {
        uerror(L"cannot read %lc: (drive not present or not ready)\n", drive);
        CloseHandle(w->h);
        return 2;
    }
    /* BPB_FATSz16 (offset 0x16) is zero only on FAT32, whose lock calls
     * take category 48h instead of 08h. */
    w->lockCat = (probe[0x16] == 0 && probe[0x17] == 0) ? 0x48 : 0x08;
    w->dev.ctx = w;
    w->dev.read = w9x_read;
    w->dev.write = w9x_write;
    w->dev.bytesPerSector = 512;
    return 0;
}

static void close_volume(fat_win9x *w)
{
    unlock_all(w);
    fat_unmount(&w->fv);
    if (w->h != INVALID_HANDLE_VALUE && w->h != NULL)
        CloseHandle(w->h);
    w->h = INVALID_HANDLE_VALUE;
}

static int mount_volume(fat_win9x *w)
{
    if (fat_mount(&w->fv, &w->dev))
        return 0;
    if (w->fv.fat == NULL && w->fv.fatBytes != 0 &&
        w->fv.fatBytes <= 64u * 1024u * 1024u) {
        /* The boot sector was fine: the FAT itself could not be loaded. */
        uerror(L"cannot read the FAT of %lc: (%lu KB)\n", w->drive,
               (unsigned long)(w->fv.fatBytes / 1024));
        close_volume(w);
        return 2;
    }
    uerror(L"%lc: is not a FAT volume (only FAT12/16/32 are supported on "
           L"Windows 9x)\n", w->drive);
    close_volume(w);
    return 3;
}

int fat9x_boot_sector(wchar_t drive, unsigned char *bs)
{
    fat_win9x *w = &g_w;
    int rc = open_volume(w, drive);

    if (rc)
        return rc;
    if (!vwin32_io(w, 0, 1, bs, 0)) {
        uerror(L"cannot read %lc: (drive not present or not ready)\n",
               w->drive);
        rc = 2;
    }
    close_volume(w);
    return rc;
}

static BOOL WINAPI on_ctrl(DWORD type)
{
    (void)type;
    g_cancel = 1; /* stop before the next burst of writes */
    return TRUE;
}

/* ----------------------------------------------------------- commands */

int cmd_fat_analyze(wchar_t drive)
{
    fat_win9x *w = &g_w;
    fat_defrag_cfg cfg;
    fat_stats st;
    int rc;

    rc = open_volume(w, drive);
    if (!rc)
        rc = mount_volume(w);
    if (rc)
        return rc;

    uinfo(L"Analyzing %lc: (%ls)...\n\n", w->drive, fat_type_name(&w->fv));
    memset(&cfg, 0, sizeof(cfg));
    cfg.doMove = 0;
    cfg.compact = 1; /* plan a full defragmentation */
    cfg.progress = on_progress;
    cfg.event = on_event;
    rc = fat_run(&w->fv, &cfg, &st);
    uprogress_clear();
    if (rc < 0) {
        close_volume(w);
        uerror(L"out of memory\n");
        return 5;
    }

    uinfo(L"Analysis of %lc: complete\n", w->drive);
    uinfo(L"  files examined       : %llu\n", st.filesExamined);
    uinfo(L"  folders examined     : %llu\n", st.dirsExamined);
    uinfo(L"  fragmented files     : %llu (%llu fragments)\n", st.fragmented,
          st.totalFragments);
    uinfo(L"  fragmented folders   : %llu\n", st.dirsFragmented);
    if (st.unmovable) {
        uinfo(L"  system files         : %llu (never moved)\n", st.unmovable);
        print_kept_notes(&w->fv, &st);
    }
    if (st.unmovableFragmented)
        uinfo(L"  unmovable fragmented : %llu (root folder or system files)\n",
              st.unmovableFragmented);
    if (st.chainErrors)
        uinfo(L"  bad cluster chains   : %llu\n", st.chainErrors);
    if (st.crossLinked)
        uinfo(L"  cross-linked         : %llu\n", st.crossLinked);
    if (st.ioErrors)
        uinfo(L"  I/O errors           : %llu\n", st.ioErrors);
    if (st.dotdotErrors)
        uinfo(L"  folders with a kept old copy: %llu\n", st.dotdotErrors);
    print_free_space(&w->fv, &st);

    if (st.chainErrors || st.crossLinked || st.ioErrors) {
        uinfo(L"\nThe file system has errors. Run ScanDisk to repair them "
              L"before defragmenting.\n");
    } else if (st.planDefrag || st.compactable) {
        const wchar_t *runs = st.planFreeRuns == 1 ? L"" : L"s";

        if (st.planDefrag && st.compactable)
            uinfo(L"\nA full defragmentation would make %llu files and "
                  L"folders contiguous and\nmove %llu others to consolidate "
                  L"the free space (leaving %lu free run%ls).\n",
                  st.planDefrag, st.compactable,
                  (unsigned long)st.planFreeRuns, runs);
        else if (st.planDefrag)
            uinfo(L"\nA full defragmentation would make %llu files and "
                  L"folders contiguous\n(leaving %lu free run%ls).\n",
                  st.planDefrag, (unsigned long)st.planFreeRuns, runs);
        else
            uinfo(L"\nA full defragmentation would move %llu files and "
                  L"folders to consolidate the\nfree space (leaving %lu free "
                  L"run%ls).\n", st.compactable,
                  (unsigned long)st.planFreeRuns, runs);
        if (st.gapNotes) {
            uinfo(L"Gaps it would leave in the free space:\n");
            print_gap_notes(&w->fv, &st);
        }
        if (st.noSpace)
            uinfo(L"%llu fragmented files or folders would not fit into any "
                  L"free run.\n", st.noSpace);
        if (st.planDefrag)
            uinfo(L"Defragmentation is recommended: hsdefrag %lc: -d\n",
                  w->drive);
        else
            uinfo(L"To consolidate the free space: hsdefrag %lc: -d\n",
                  w->drive);
    } else if (st.gapNotes) {
        uinfo(L"\nNo defragmentation needed: the files and folders are "
              L"contiguous and the\nfree space is consolidated as far as "
              L"possible. Gaps that stay:\n");
        print_gap_notes(&w->fv, &st);
    } else {
        uinfo(L"\nNo defragmentation needed: the files and folders are "
              L"contiguous and the\nfree space is consolidated.\n");
    }
    close_volume(w);
    if (!uoutput_ok())
        return 5;
    return rc ? 4 : 0;
}

int cmd_fat_defrag(wchar_t drive)
{
    fat_win9x *w = &g_w;
    fat_defrag_cfg cfg;
    fat_stats st;
    wchar_t a[32];
    DWORD start, secs;
    int rc, i, r = FAT_WR_OK;

    g_cancel = 0;
    rc = open_volume(w, drive);
    if (rc)
        return rc;
    if (!lock_volume(w)) {
        uerror(L"cannot lock %lc: for writing\n", w->drive);
        uinfo(L"Close other disk utilities (ScanDisk, Disk Defragmenter, "
              L"backup programs)\nand try again.\n");
        close_volume(w);
        return 2;
    }
    rc = mount_volume(w);
    if (rc)
        return rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.doMove = 1;
    cfg.verify = 1;
    cfg.compact = 1;
    cfg.cbctx = w;
    cfg.progress = on_progress;
    cfg.event = on_event;
    if (!w->exclusive) {
        /* Volume in use: write only under level 3, keep in-use files put. */
        cfg.begin_write = hook_begin;
        cfg.end_write = hook_end;
        cfg.is_unmovable = hook_unmovable;
        /* Start from the FAT as Windows has really written it. */
        w->forceReload = w->needEnum = 1;
        if (!find_selfcheck(w)) {
            print_reason(w);
            close_volume(w);
            return 2;
        }
        for (i = 0; i < 20; i++) {
            r = hook_begin(w);
            if (r != FAT_WR_BUSY)
                break;
        }
        if (r != FAT_WR_OK && r != FAT_WR_CHANGED) {
            close_volume(w);
            uerror(L"cannot lock %lc: for writing\n", w->drive);
            print_reason(w);
            return 2;
        }
        hook_end(w);
        if (w->unlockFailed) {
            close_volume(w);
            print_reason(w);
            return 2;
        }
    }

    start = GetTickCount();
    uinfo(L"Defragmenting %lc: (%ls)...\n", w->drive, fat_type_name(&w->fv));
    if (!w->exclusive)
        uverbose(L"  %lu file%ls in use stay%ls in place\n",
                 (unsigned long)w->nPins, w->nPins == 1 ? L"" : L"s",
                 w->nPins == 1 ? L"s" : L"");
    g_cancel = 0;
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    rc = fat_run(&w->fv, &cfg, &st);
    SetConsoleCtrlHandler(on_ctrl, FALSE);
    unlock_all(w);
    if (w->unlockFailed) {
        close_volume(w);
        uerror(L"cannot release the volume lock on %lc:\n", w->drive);
        if (rc == 0)
            rc = 1;
    }
    uprogress_clear();
    if (rc < 0) {
        close_volume(w);
        uerror(L"out of memory\n");
        return 5;
    }

    secs = (GetTickCount() - start) / 1000;
    uinfo(L"\nDefragmentation of %lc: %ls in %lu:%02lu\n", w->drive,
          rc ? L"incomplete" : L"finished", (unsigned long)(secs / 60),
          (unsigned long)(secs % 60));
    uinfo(L"  files examined       : %llu\n", st.filesExamined);
    uinfo(L"  folders examined     : %llu\n", st.dirsExamined);
    uinfo(L"  files defragmented   : %llu\n", st.defragged);
    uinfo(L"  folders defragmented : %llu\n", st.dirsDefragged);
    uinfo(L"  moved to consolidate : %llu %ls\n", st.compacted,
          st.compacted == 1 ? L"file or folder" : L"files and folders");
    uinfo(L"  data moved           : %ls\n",
          uformat_bytes(st.clustersMoved * w->fv.bytesPerCluster, a, 32));
    if (st.unmovable) {
        uinfo(L"  kept in place        : %llu system or in-use file%ls\n",
              st.unmovable, st.unmovable == 1 ? L"" : L"s");
        print_kept_notes(&w->fv, &st);
    }
    if (st.unmovableFragmented)
        uinfo(L"  unmovable fragmented : %llu (root folder, system or in-use "
              L"files)\n", st.unmovableFragmented);
    print_free_space(&w->fv, &st);
    print_gap_notes(&w->fv, &st);

    if (st.noSpace)
        uinfo(L"%llu fragmented files or folders did not fit into any free "
              L"run.\n", st.noSpace);
    if (st.busy)
        uinfo(L"%llu files or folders kept changing and were skipped; run "
              L"again when the\ncomputer is idle.\n", st.busy);
    if (st.ioErrors)
        uinfo(L"%llu I/O errors occurred; run ScanDisk with a thorough "
              L"(surface) scan.\n", st.ioErrors);
    if (st.chainErrors || st.crossLinked)
        uinfo(L"The file system has errors (%llu bad chains, %llu "
              L"cross-linked); run ScanDisk\nto repair them, then run "
              L"hsdefrag again.%ls\n", st.chainErrors, st.crossLinked,
              st.crossLinked ? L" Nothing was moved." : L"");
    if (st.aborted) {
        uinfo(L"The run was stopped early.\n");
        if (w->failWhat)
            print_reason(w);
        else if (!st.ioErrors)
            uinfo(L"  reason: the volume stayed busy\n");
    }
    close_volume(w);
    if (!uoutput_ok())
        return 5;
    return rc ? 4 : 0;
}

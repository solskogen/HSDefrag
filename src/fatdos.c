/*
 * fatdos.c - The MS-DOS side of hsdefrag.exe, the FAT defragmenter for DOS.
 *
 * Windows 95/98 never lets a program move the files it keeps open (its swap
 * file, the registry, running programs and DLLs), so a pass under Windows
 * leaves them where they are. The MS-DOS side runs the same engine (fatfs.c
 * and fatdefrag.c) before Windows starts, when nothing holds those files:
 *
 *   hsdefrag C: -a|-d [-v]           in "Command prompt only" or MS-DOS mode
 *   hsdefrag --boot C:\HSDEFRAG.JOB  from AUTOEXEC.BAT, as scheduled by
 *                                    "hsdefrag C: --boot-defrag" on Windows
 *
 * It is a 32-bit DJGPP program on a CWSDPMI stub that never swaps to disk;
 * tools/mkdual.c puts the stub and the program behind the MS-DOS header of
 * the Windows program, so one hsdefrag.exe runs in both. Sectors are read and
 * written with the DOS absolute disk calls (INT 21h AX=7305h, which knows
 * FAT32, else INT 25h/26h, first in the form of MS-DOS 3.31 and later and
 * then in the old one) through a buffer in conventional memory, in the same
 * 32 KB transfers as on Windows.
 *
 * DOS keeps FAT and directory sectors in its own buffers, and an absolute
 * write does not invalidate them (MS-DOS 4.0 source, ABSDWRT), so:
 *   - the disk is reset (INT 21h AH=0Dh writes the dirty buffers and then
 *     invalidates all of them) before the run and again after it, before
 *     any file is written;
 *   - no DOS file I/O touches the volume during the run: progress goes to
 *     the screen, the boot log is kept in memory, and the current directory
 *     is moved to the root (which never moves) and back afterwards;
 *   - files DOS has open there (its system file table) stay in place, and a
 *     file open for writing (e.g. redirected output) refuses the run;
 *   - the volume is locked where DOS supports it (MS-DOS 7), and SMARTDRV
 *     commits its cache and stops caching writes to the drive meanwhile.
 * Only IO.SYS and the other hidden system files in the root folder that boot
 * code may find by position (see is_system in fatdefrag.c), LDLINUX.SYS and
 * the first cluster of the FAT32 root folder stay in place; the swap file,
 * the registry, the logs and backups in the root folder and hidden, system
 * and read-only files elsewhere move like any other file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <crt0.h>
#include <sys/farptr.h>
#include <sys/movedata.h>
#include <sys/exceptn.h>

#include "fatfs.h"
#include "fatdisk.h"
#include "fatdefrag.h"
#include "fatreport.h"
#include "bootjob.h"
#include "version.h"

/* Exit codes, as for hsdefrag.exe. */
enum { RC_OK, RC_USAGE, RC_ACCESS, RC_UNSUPPORTED, RC_INCOMPLETE, RC_ERROR };

/* No response files, wildcard expansion, DJGPP.ENV or long file names: the
 * program opens no file but the ones it is told about. */
int _crt0_startup_flags = _CRT0_FLAG_DISALLOW_RESPONSE_FILES |
                          _CRT0_FLAG_NO_LFN;
unsigned _stklen = 1048576;

char **__crt0_glob_function(char *arg)
{
    (void)arg;
    return 0;
}

void __crt0_load_environment_file(char *app)
{
    (void)app;
}

/*
 * crt0 registers the program's unwind tables with libgcc at startup and
 * deregisters them at exit. A C program never unwinds, so these empty
 * versions keep libgcc's exception unwinder out of the image: in an i686
 * DJGPP it is built with cmov/fcmov, which a 486 does not have. With them
 * the image holds only i486 code whichever DJGPP builds it (make
 * check-dos-insns verifies this).
 */
struct object;
void __register_frame_info(const void *begin, struct object *ob);
void *__deregister_frame_info(const void *begin);

void __register_frame_info(const void *begin, struct object *ob)
{
    (void)begin;
    (void)ob;
}

void *__deregister_frame_info(const void *begin)
{
    (void)begin;
    return 0;
}

#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))

/* ------------------------------------------------------------- output */

#define LOG_CAP 32768

static int           g_verbose;
static int           g_logOn;      /* --boot: keep a copy for HSDEFRAG.LOG */
static char          g_log[LOG_CAP];
static size_t        g_logLen;
static int           g_logCut;
static int           g_tty = -1;
static int           g_progressShown;
static unsigned long g_progressTick;
static volatile int  g_cancel;

static unsigned long bios_ticks(void)
{
    return _farpeekl(_dos_ds, 0x46C);
}

/* BIOS timer ticks (18.2 a second) since start, across midnight. */
static unsigned long ticks_since(unsigned long start)
{
    unsigned long now = bios_ticks();

    if (now < start)
        now += 0x1800B0UL;          /* ticks per day */
    return now - start;
}

static void log_text(const char *s)
{
    if (!g_logOn || g_logCut)
        return;
    for (; *s; s++) {
        if (g_logLen + 64 > LOG_CAP) { /* room for the closing note */
            g_logCut = 1;
            return;
        }
        if (*s == '\n')
            g_log[g_logLen++] = '\r';
        g_log[g_logLen++] = *s;
    }
}

static int stdout_tty(void)
{
    if (g_tty < 0)
        g_tty = isatty(1) ? 1 : 0;
    return g_tty;
}

static void progress_clear(void)
{
    if (!g_progressShown)
        return;
    g_progressShown = 0;
    printf("\r%77s\r", "");
    fflush(stdout);
}

/* A status line that rewrites itself: screen only, about 3 times a second. */
static PRINTF_LIKE(1, 2) void progress(const char *fmt, ...)
{
    char line[96];
    va_list ap;
    unsigned long now = bios_ticks();
    int n;

    if (g_verbose || !stdout_tty())
        return;
    if (g_progressShown && now - g_progressTick < 5)
        return;
    g_progressTick = now;
    line[0] = '\r';
    va_start(ap, fmt);
    n = 1 + vsnprintf(line + 1, 78, fmt, ap);
    va_end(ap);
    if (n < 1)
        n = 1;
    if (n > 78)
        n = 78;
    while (n < 78)
        line[n++] = ' ';
    fwrite(line, 1, (size_t)n, stdout);
    fflush(stdout);
    g_progressShown = 1;
}

static void out_v(FILE *f, const char *prefix, const char *fmt, va_list ap)
{
    char small[512], *buf = small;
    va_list aq;
    int n;

    progress_clear();
    va_copy(aq, ap);
    n = vsnprintf(small, sizeof(small), fmt, ap);
    if (n >= (int)sizeof(small)) {
        buf = (char *)malloc((size_t)n + 1);
        if (buf)
            vsnprintf(buf, (size_t)n + 1, fmt, aq);
        else
            buf = small;            /* print what fits */
    }
    va_end(aq);
    if (prefix) {
        fputs(prefix, f);
        log_text(prefix);
    }
    fputs(buf, f);
    fflush(f);
    log_text(buf);
    if (buf != small)
        free(buf);
}

static PRINTF_LIKE(1, 2) void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    out_v(stdout, NULL, fmt, ap);
    va_end(ap);
}

static PRINTF_LIKE(1, 2) void verbose(const char *fmt, ...)
{
    va_list ap;

    if (!g_verbose)
        return;
    va_start(ap, fmt);
    out_v(stdout, NULL, fmt, ap);
    va_end(ap);
}

static PRINTF_LIKE(1, 2) void fail(const char *fmt, ...)
{
    va_list ap;

    fflush(stdout);
    va_start(ap, fmt);
    out_v(stderr, "error: ", fmt, ap);
    va_end(ap);
}


/* ------------------------------------------------- conventional memory */

/*
 * One block below 1 MB: a 32 KB transfer window that does not cross a 64 KB
 * boundary (DMA), and past the first 64 KB a small area with the request
 * packet, thunks for INT 25h/26h (which return with the flags still on the
 * stack), our INT 23h/24h handlers, a path buffer and a real-mode stack.
 */
#define DOS_PARAS   (4096 + 256)
#define WIN_BYTES   32768u
#define WIN_SECTORS (WIN_BYTES / 512u)
#define M_PACKET    0x00    /* 10 bytes: the disk I/O packet               */
#define M_INT25     0x10    /* int 25h; inc sp; inc sp; retf              */
#define M_INT26     0x18    /* int 26h; inc sp; inc sp; retf              */
#define M_INT23     0x20    /* mov byte [cs:M_BREAK],1; iret              */
#define M_INT24     0x28    /* mov al,3 (fail the call); iret             */
#define M_BREAK     0x2C    /* set when DOS saw Ctrl+C                    */
#define M_PATH      0x30    /* 128 bytes                                  */
#define M_STACK     0x1000  /* top of the real-mode stack                 */

static int           g_dosSel = -1;
static unsigned long g_winLin;      /* linear address of the window */
static unsigned      g_winSeg;
static unsigned      g_miscSeg;
static unsigned long g_miscLin;
static unsigned long g_dosLimit;    /* last byte _dos_ds reaches */
static int           g_vectors;
static __dpmi_raddr  g_old23, g_old24;

static int           g_letter;      /* 'A'..'Z' */
static int           g_drive;       /* 1-based (A: = 1) */
static int           g_method;      /* one of the FATDISK_* of fatdisk.h   */
static int           g_lockCat;     /* 08h (FAT12/16) or 48h (FAT32) */
static int           g_lockUsed;    /* category the lock was taken in */
static int           g_levels;      /* lock levels held */
static unsigned char g_bs[512];     /* boot sector */

/* Real-mode interrupt on our own stack. The carry flag is preset, so a call
 * that does not exist on this DOS reads as failed. */
static int rm_int(int vec, __dpmi_regs *r)
{
    r->x.flags = 0x0201;
    r->x.ss = (unsigned short)g_miscSeg;
    r->x.sp = M_STACK;
    r->x.cs = r->x.ip = 0;
    if (__dpmi_simulate_real_mode_interrupt(vec, r) != 0)
        return 0;
    return !(r->x.flags & 1);
}

static int rm_thunk(unsigned ip, __dpmi_regs *r)
{
    r->x.flags = 0x0201;
    r->x.ss = (unsigned short)g_miscSeg;
    r->x.sp = M_STACK;
    r->x.cs = (unsigned short)g_miscSeg;
    r->x.ip = (unsigned short)ip;
    if (__dpmi_simulate_real_mode_procedure_retf(r) != 0)
        return 0;
    return !(r->x.flags & 1);
}

static int dos_call(__dpmi_regs *r)
{
    return rm_int(0x21, r);
}

/* INT 21h AH=0Dh: write DOS's dirty buffers, then forget all of them. */
static void disk_reset(void)
{
    __dpmi_regs r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0x0D;
    r.h.dl = (unsigned char)(g_drive ? g_drive : 0);
    dos_call(&r);
}

static void unlock_all(void)
{
    __dpmi_regs r;

    while (g_levels > 0) {
        memset(&r, 0, sizeof(r));
        r.x.ax = 0x440D;
        r.h.bl = (unsigned char)g_drive;
        r.h.ch = (unsigned char)g_lockUsed;
        r.h.cl = 0x6A;
        dos_call(&r);
        g_levels--;
    }
}

static void dos_done(void)
{
    if (g_dosSel == -1)
        return;
    unlock_all();
    if (g_vectors) {
        __dpmi_set_real_mode_interrupt_vector(0x23, &g_old23);
        __dpmi_set_real_mode_interrupt_vector(0x24, &g_old24);
        g_vectors = 0;
    }
    __dpmi_free_dos_memory(g_dosSel);
    g_dosSel = -1;
}

static int dos_init(void)
{
    static const unsigned char code[] = {
        0xCD, 0x25, 0x44, 0x44, 0xCB, 0x90, 0x90, 0x90,         /* M_INT25 */
        0xCD, 0x26, 0x44, 0x44, 0xCB, 0x90, 0x90, 0x90,         /* M_INT26 */
        0x2E, 0xC6, 0x06, M_BREAK, 0x00, 0x01, 0xCF, 0x90,      /* M_INT23 */
        0xB0, 0x03, 0xCF                                        /* M_INT24 */
    };
    static const unsigned char zero[256];
    unsigned long base, next;
    __dpmi_raddr a;
    int seg, i;

    seg = __dpmi_allocate_dos_memory(DOS_PARAS, &g_dosSel);
    if (seg == -1) {
        g_dosSel = -1;
        return 0;
    }
    base = (unsigned long)seg << 4;
    next = (base + 0x10000UL) & ~0xFFFFUL;
    g_winLin = next - base >= WIN_BYTES ? base : next;
    g_winSeg = (unsigned)(g_winLin >> 4);
    g_miscSeg = (unsigned)seg + 4096;
    g_miscLin = (unsigned long)g_miscSeg << 4;
    g_dosLimit = __dpmi_get_segment_limit(_dos_ds);
    for (i = 0; i < M_STACK; i += (int)sizeof(zero))
        dosmemput(zero, sizeof(zero), g_miscLin + (unsigned long)i);
    dosmemput(code, sizeof(code), g_miscLin + M_INT25);

    __dpmi_get_real_mode_interrupt_vector(0x23, &g_old23);
    __dpmi_get_real_mode_interrupt_vector(0x24, &g_old24);
    a.segment = (unsigned short)g_miscSeg;
    a.offset16 = M_INT23;
    __dpmi_set_real_mode_interrupt_vector(0x23, &a);
    a.offset16 = M_INT24;
    __dpmi_set_real_mode_interrupt_vector(0x24, &a);
    g_vectors = 1;
    atexit(dos_done);
    return 1;
}

/* A crash must not leave the volume locked. */
static void on_fatal(int sig)
{
    signal(sig, SIG_DFL);
    unlock_all();
    disk_reset();
    raise(sig);
}

/* ------------------------------------------------------------ keyboard */

static void on_sigint(int sig)
{
    g_cancel = 1;
    signal(sig, on_sigint);
}

/* ESC, Ctrl+C or Ctrl+Break: stop at the next safe point. */
static int poll_keys(void)
{
    if (_go32_was_ctrl_break_hit())
        g_cancel = 1;
    if (_farpeekb(_dos_ds, g_miscLin + M_BREAK)) {
        _farpokeb(_dos_ds, g_miscLin + M_BREAK, 0);
        g_cancel = 1;
    }
    while (kbhit()) {
        int k = getkey();

        if (k == 27 || k == 3)
            g_cancel = 1;
    }
    return g_cancel;
}

static void reset_keys(void)
{
    while (kbhit())
        getkey();
    _go32_was_ctrl_break_hit();
    _farpokeb(_dos_ds, g_miscLin + M_BREAK, 0);
    g_cancel = 0;
}

/* ---------------------------------------------------------- sector I/O */

/*
 * Absolute sector access.
 *
 * Method 1 is INT 21h AX=7305h, the extended absolute disk read/write of
 * MS-DOS 7.0A and later (Windows 95 and 98). It takes the 1-based drive
 * number in DL, the mode flags in SI and the same disk I/O packet as
 * method 2 at DS:BX, and it knows FAT32 volumes.
 *
 * Method 2 is INT 25h/26h in the form of MS-DOS 3.31 and later, so MS-DOS
 * 6.2 has it: CX=FFFFh selects the form, AL holds the 0-based drive number
 * and DS:BX points at the disk I/O packet, which holds the 32-bit sector
 * number, the number of sectors and the far address of the transfer
 * window. INT 25h reads and INT 26h writes.
 *
 * Method 3 is the same call in the old form of DOS 1 and 2: the 0-based
 * drive number in AL, the number of sectors in CX, the first sector in DX
 * and the transfer window at DS:BX. It reaches only the first 64K sectors
 * of a drive; DOS refuses it with AX=0207h on anything bigger, which is
 * how the probe tells the two forms apart.
 *
 * The disk I/O packet is the one of the extended calls: the sector number
 * (DWORD), the number of sectors (WORD) and the transfer address (DWORD),
 * which is a far pointer with the offset in the low word and the segment
 * in the high word.
 */
static int abs_once(int method, fat_u32 sector, unsigned count, int write)
{
    fat_disk_req req;
    __dpmi_regs r;

    fat_disk_request(method, g_drive, sector, count, g_winSeg, 0, &req);
    if (req.use_packet) {
        dosmemput(req.packet, FATDISK_PACKET_SIZE, g_miscLin + M_PACKET);
        req.bx = M_PACKET;
        req.ds = (unsigned short)g_miscSeg;
    }
    if (method == FATDISK_7305)
        req.si = write ? 1 : 0;                   /* bit 0: 1 is a write */

    memset(&r, 0, sizeof(r));
    r.x.ax = (unsigned short)req.ax;
    r.x.bx = (unsigned short)req.bx;
    r.x.cx = (unsigned short)req.cx;
    r.x.dx = (unsigned short)req.dx;
    r.x.si = (unsigned short)req.si;
    r.x.ds = (unsigned short)req.ds;
    r.x.es = (unsigned short)req.es;
    if (method == FATDISK_7305)
        return dos_call(&r);
    return rm_thunk(write ? M_INT26 : M_INT25, &r);
}

/* Whole sectors through the window; in is read into, out is written. */
static int abs_io(fat_u32 sector, fat_u32 count, unsigned char *in,
                  const unsigned char *out)
{
    while (count > 0) {
        unsigned n = count > WIN_SECTORS ? WIN_SECTORS : (unsigned)count;
        int tries = 0;

        if (out)
            dosmemput(out, (int)(n * 512), g_winLin);
        while (!abs_once(g_method, sector, n, out != NULL))
            if (++tries == 3)
                return 0;
        if (in) {
            dosmemget(g_winLin, (int)(n * 512), in);
            in += n * 512;
        } else {
            out += n * 512;
        }
        sector += n;
        count -= n;
    }
    return 1;
}

static int dev_read(void *ctx, fat_u32 sector, fat_u32 count, void *buf)
{
    (void)ctx;
    return abs_io(sector, count, (unsigned char *)buf, NULL);
}

static int dev_write(void *ctx, fat_u32 sector, fat_u32 count,
                     const void *buf)
{
    (void)ctx;
    return abs_io(sector, count, NULL, (const unsigned char *)buf);
}

/*
 * A call that only looks like it works must not be taken for one: a boot
 * sector ends with the 55AA signature, and a call that transfers nothing
 * leaves the 0xE7 fill in place.
 */
static int probe_boot(int method)
{
    int t;

    for (t = 0; t < 3; t++) {
        memset(g_bs, 0xE7, sizeof(g_bs));
        dosmemput(g_bs, sizeof(g_bs), g_winLin);
        if (!abs_once(method, 0, 1, 0))
            continue;
        dosmemget(g_winLin, sizeof(g_bs), g_bs);
        if (g_bs[510] == 0x55 && g_bs[511] == 0xAA)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------- environment checks */

/* Returns 0, or an exit code after explaining why hsdefrag cannot run. */
static int check_dos(void)
{
    __dpmi_regs r;
    unsigned major, minor, oem;
    const char *name;
    int windows, ax160a, al1600, ax4680;

    memset(&r, 0, sizeof(r));
    r.x.ax = 0x3000;
    dos_call(&r);
    major = r.h.al;
    minor = r.h.ah;
    oem = r.h.bh;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x3306;                /* true version, despite SETVER */
    dos_call(&r);
    if (r.h.al != 0xFF && r.h.bl != 0) {
        major = r.h.bl;
        minor = r.h.bh;
    }
    switch (oem) {
    case 0xFF: name = "MS-DOS"; break;
    case 0xFD: name = "FreeDOS"; break;
    case 0x00: name = "PC DOS"; break;
    case 0xEE:
    case 0xEF: name = "DR-DOS"; break;
    default:   name = "DOS"; break;
    }
    if (major == 5 && minor == 50) {
        fail("this is the DOS box of Windows NT; run hsdefrag from a Windows "
             "command prompt\n");
        return RC_UNSUPPORTED;
    }
    if (major >= 10) {
        fail("this is the DOS box of OS/2, which does not allow direct disk "
             "writes\n");
        return RC_UNSUPPORTED;
    }
    if (major < 4) {
        fail("%s %u.%02u is too old; DOS 4.0 or later is needed\n", name,
             major, minor);
        return RC_UNSUPPORTED;
    }

    /*
     * Is Windows running?  INT 2Fh AX=160Ah is the check that answers in
     * every mode of Windows 3.1 and later (Windows 95 and 98 included);
     * AX=1600h catches Windows/386 2.x and Windows 3.0 in enhanced mode.
     * AX=4680h must not be used on its own: DOS 5 and later answer it the
     * same way when their own shell is on the screen, which is the normal
     * case on a plain DOS system, so it would keep hsdefrag from running
     * there at all.  DOS 4 has no shell of its own, and there an answer of
     * zero means Windows 3.0 in real or standard mode.
     */
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x160A;
    rm_int(0x2F, &r);
    ax160a = r.x.ax;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x1600;
    rm_int(0x2F, &r);
    al1600 = r.h.al;
    ax4680 = 1;
    if (major < 5) {
        memset(&r, 0, sizeof(r));
        r.x.ax = 0x4680;
        rm_int(0x2F, &r);
        ax4680 = r.x.ax;
    }
    windows = fatdisk_windows_running(ax160a, al1600, ax4680, major < 5);
    if (!windows)
        return RC_OK;
    fail("hsdefrag cannot run in MS-DOS while Windows is running\n");
    say("Run it from a Windows command prompt, or run \"hsdefrag C: "
        "--boot-defrag\" there to\ndefragment the drive the next time the "
        "computer starts.\n");
    return RC_UNSUPPORTED;
}

static int check_drive(void)
{
    __dpmi_regs r;

    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4409;                /* is the block device remote? */
    r.h.bl = (unsigned char)g_drive;
    if (!dos_call(&r)) {
        fail("%c: is not a valid drive\n", g_letter);
        return RC_ACCESS;
    }
    if (r.x.dx & 0x1000) {
        fail("%c: is a network or CD-ROM drive\n", g_letter);
        return RC_UNSUPPORTED;
    }
    if (r.x.dx & 0x8000) {
        fail("%c: is a SUBST drive; defragment the drive it points to\n",
             g_letter);
        return RC_UNSUPPORTED;
    }

    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4A11;                /* DoubleSpace/DriveSpace loaded? */
    r.x.bx = 0x0000;
    rm_int(0x2F, &r);
    if (r.x.bx == 0x444D) {
        memset(&r, 0, sizeof(r));
        r.x.ax = 0x4A11;
        r.x.bx = 0x0001;            /* get the drive map */
        r.h.dl = (unsigned char)(g_drive - 1);
        rm_int(0x2F, &r);
        if (r.x.ax == 0 && (r.h.bl & 0x80)) {
            fail("%c: is a compressed DriveSpace/DoubleSpace drive\n",
                 g_letter);
            say("Defragment it with the DEFRAG of DriveSpace.\n");
            return RC_UNSUPPORTED;
        }
    }
    return RC_OK;
}

/* The name of the disk call that was chosen, for the verbose output. */
static const char *method_name(int method)
{
    switch (method) {
    case FATDISK_7305:      return "INT 21h AX=7305h";
    case FATDISK_INT25_NEW: return "INT 25h/26h, CX=FFFFh";
    case FATDISK_INT25_OLD: return "INT 25h/26h, the old form";
    default:                return "no disk call";
    }
}

/* Probe the drive: which call reads it, and is it a FAT volume? */
static int open_drive(int letter)
{
    int rc;

    g_letter = letter;
    g_drive = letter - 'A' + 1;
    rc = check_drive();
    if (rc)
        return rc;
    {
        /* The extended calls come first: INT 21h AX=7305h knows FAT32 and
         * is the only one that does, and the CX=FFFFh form of INT 25h/26h
         * is what MS-DOS 3.31 and later have.  The old form of INT 25h/26h
         * is the last resort: it reaches only the first 64K sectors of a
         * drive, and DOS refuses it with AX=0207h on anything bigger. */
        static const int methods[3] = { FATDISK_7305, FATDISK_INT25_NEW,
                                       FATDISK_INT25_OLD };
        int m;

        g_method = 0;
        for (m = 0; m < 3 && !g_method; m++)
            if (probe_boot(methods[m]))
                g_method = methods[m];
    }
    if (!g_method) {
        fail("cannot read %c: (drive not ready, or not a FAT drive)\n",
             letter);
        return RC_ACCESS;
    }
    verbose("  %c: is read with %s\n", letter, method_name(g_method));
    if (g_bs[11] != 0x00 || g_bs[12] != 0x02) {
        fail("%c: is not a FAT volume with 512-byte sectors\n", letter);
        return RC_UNSUPPORTED;
    }
    if (g_method == FATDISK_INT25_OLD) {
        /* The old form of INT 25h/26h addresses 64K sectors at most. DOS
         * refuses it on a bigger drive, but the boot sector is checked as
         * well, so that no sector of a larger volume is ever touched. */
        unsigned long sectors = g_bs[0x13] | (unsigned)g_bs[0x14] << 8;

        if (!sectors)
            sectors = (unsigned long)g_bs[0x20] | (unsigned long)g_bs[0x21] << 8 |
                      (unsigned long)g_bs[0x22] << 16 |
                      (unsigned long)g_bs[0x23] << 24;
        if (sectors > 0xFFFFUL) {
            fail("%c: is too large for this DOS to reach with the old disk "
                 "call\n", letter);
            return RC_UNSUPPORTED;
        }
    }
    /* BPB_FATSz16 is zero only on FAT32, whose locks use category 48h. */
    g_lockCat = (g_bs[0x16] == 0 && g_bs[0x17] == 0) ? 0x48 : 0x08;
    return RC_OK;
}

/* ---------------------------------------------------- locks, SMARTDRV */

/* INT 21h AX=440Dh CL=4Ah: one more lock level. 0, or the DOS error. */
static int lock_level(int cat, int level, unsigned perm)
{
    __dpmi_regs r;

    memset(&r, 0, sizeof(r));
    r.x.ax = 0x440D;
    r.h.bl = (unsigned char)g_drive;
    r.h.bh = (unsigned char)level;
    r.h.ch = (unsigned char)cat;
    r.h.cl = 0x4A;
    r.x.dx = (unsigned short)perm;
    if (dos_call(&r)) {
        g_lockUsed = cat;
        g_levels++;
        return 0;
    }
    return r.x.ax ? r.x.ax : -1;
}

/*
 * MS-DOS 7 wants a lock for direct disk writes even without Windows: level 0
 * when no file on the volume is open, otherwise levels 1, 2 and 3. Older DOS
 * versions know no locks (the call is invalid in both categories) and need
 * none. Returns 0 when the volume may be written, else the DOS error.
 */
static int lock_drive(void)
{
    int cat = g_lockCat, e, level;

    e = lock_level(cat, 0, 0);
    if (e == 1) {
        cat = cat == 0x48 ? 0x08 : 0x48;
        e = lock_level(cat, 0, 0);
        if (e == 1)
            return 0;
    }
    if (e == 0) {
        e = lock_level(cat, 0, 1); /* with the write permission */
        if (e)
            unlock_all();
        return e;
    }
    for (level = 1; level <= 3; level++) {
        e = lock_level(cat, level, level == 1 ? 1 : 0);
        if (e) {
            unlock_all();
            return e;
        }
    }
    return 0;
}

static int g_sdrv;                  /* SMARTDRV is loaded */
static int g_sdrvWrite;             /* we turned off its write caching */

static void smartdrv(unsigned bx, unsigned bp, unsigned dl, __dpmi_regs *r)
{
    memset(r, 0, sizeof(*r));
    r->x.ax = 0x4A10;
    r->x.bx = (unsigned short)bx;
    r->x.bp = (unsigned short)bp;
    r->x.dx = (unsigned short)dl;
    if (bx == 0)
        r->x.cx = 0xEBAB;
    rm_int(0x2F, r);
}

/* Commit SMARTDRV's write-behind cache and stop caching writes to the
 * drive, so the engine's write order reaches the disk as it is made. */
static void smartdrv_begin(void)
{
    __dpmi_regs r;

    smartdrv(0, 0, 0, &r);          /* installation check */
    g_sdrv = r.x.ax == 0xBABE;
    if (!g_sdrv)
        return;
    smartdrv(1, 0, 0, &r);          /* commit */
    smartdrv(3, (unsigned)(g_drive - 1), 0, &r); /* drive status */
    if (r.h.dl != 0xFF && !(r.h.dl & 0x40)) {
        smartdrv(3, (unsigned)(g_drive - 1), 4, &r); /* write cache off */
        g_sdrvWrite = 1;
    }
}

static void smartdrv_end(void)
{
    __dpmi_regs r;

    if (!g_sdrv)
        return;
    if (g_sdrvWrite)
        smartdrv(3, (unsigned)(g_drive - 1), 3, &r); /* write cache on */
    smartdrv(1, 0, 0, &r);
    g_sdrv = g_sdrvWrite = 0;
}

/* ------------------------------------------------------ DOS structures */

#define MAX_PINS 256

static fat_volume g_fv;
static fat_u32    g_pins[MAX_PINS];  /* sorted first clusters of open files */
static int        g_nPins;
static char       g_cwd[68];         /* current directory of the drive */
static int        g_cwdSaved;

static int upper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

/* Does [lin, lin+len) lie inside what _dos_ds reaches? */
static int lin_ok(unsigned long lin, unsigned long len)
{
    return len > 0 && lin + len > lin && lin + len - 1 <= g_dosLimit;
}

/* INT 21h AH=52h: DOS's List of Lists (0 if unknown). */
static unsigned long lol_addr(void)
{
    __dpmi_regs r;
    unsigned long lol;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0x52;
    dos_call(&r);
    lol = ((unsigned long)r.x.es << 4) + r.x.bx;
    return lol && lin_ok(lol, 0x22) ? lol : 0;
}

/*
 * A SUBST or JOIN drive that leads into the volume keeps the first cluster
 * of its current directory in DOS's current directory structure (CDS), which
 * a move would make stale. Entries are 58h bytes from DOS 4 on; if the table
 * does not look like that, there is nothing reliable to check.
 */
static int check_cds(void)
{
    unsigned long lol = lol_addr(), cds;
    unsigned char e[FATDISK_CDS_SIZE];
    unsigned last, i, off, seg;

    if (!lol)
        return RC_OK;
    last = _farpeekb(_dos_ds, lol + 0x21);
    off = _farpeekw(_dos_ds, lol + 0x16);
    seg = _farpeekw(_dos_ds, lol + 0x18);
    cds = ((unsigned long)seg << 4) + off;
    if (last == 0 || last > 32 || off == 0xFFFF || cds == 0 ||
        !lin_ok(cds, last * 0x58UL))
        return RC_OK;
    for (i = 0; i < last; i++) {
        int kind;

        dosmemget(cds + i * 0x58UL, (int)sizeof(e), e);
        kind = fatdisk_cds_entry(e, i, g_letter, g_drive - 1);
        if (kind == FATDISK_CDS_UNKNOWN)
            return RC_OK;           /* not the layout we know */
        if (kind == FATDISK_CDS_OK)
            continue;
        e[0x42] = 0;                /* end the path for the message */
        fail("%c: is %s %s\n", (int)('A' + i),
             kind == FATDISK_CDS_JOIN ? "JOINed to" : "a SUBST of", (char *)e);
        say("Remove it first (%s %c: /D).\n",
            kind == FATDISK_CDS_JOIN ? "JOIN" : "SUBST", (int)('A' + i));
        return RC_UNSUPPORTED;
    }
    return RC_OK;
}

static int pin_add(fat_u32 cl)
{
    int i, j;

    if (cl == 0)
        return 1;
    for (i = 0; i < g_nPins && g_pins[i] < cl; i++)
        ;
    if (i < g_nPins && g_pins[i] == cl)
        return 1;
    if (g_nPins >= MAX_PINS)
        return 0;
    for (j = g_nPins; j > i; j--)
        g_pins[j] = g_pins[j - 1];
    g_pins[i] = cl;
    g_nPins++;
    return 1;
}

static int hook_unmovable(void *ctx, fat_u32 start)
{
    int lo = 0, hi = g_nPins;

    (void)ctx;
    while (lo < hi) {
        int mid = (lo + hi) / 2;

        if (g_pins[mid] == start)
            return 1;
        if (g_pins[mid] < start)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

/* One system file table entry: refuse, pin, or ignore it. */
static int check_open(unsigned long at, int fcbTable)
{
    unsigned char e[FATDISK_SFT_SIZE], sec[512];
    fat_sft_info info;
    fat_u32 cl;
    int kind;

    dosmemget(at, (int)sizeof(e), e);
    kind = fatdisk_sft_entry(e, g_drive - 1, fcbTable, &info);
    if (kind == FATDISK_SFT_IGNORE)
        return RC_OK;               /* free, a device, or another drive */
    if (kind == FATDISK_SFT_WRITE) {
        fail("%s on %c: is open for writing\n"
             "If the output is redirected to a file on %c:, write it to "
             "another drive.\n", info.name, g_letter, g_letter);
        return RC_UNSUPPORTED;
    }
    if (info.dirIndex < 16 && info.dirSector < g_fv.totalSectors &&
        fat_read_sectors(&g_fv, info.dirSector, 1, sec) &&
        fatdisk_sft_dir_match(e, sec + info.dirIndex * 32,
                              g_fv.fatType == FAT_TYPE_32, &cl)) {
        if (pin_add(cl))
            return RC_OK;
        fail("too many files are open on %c:\n", g_letter);
        return RC_UNSUPPORTED;
    }
    if (info.fcb)
        return RC_OK;
    fail("cannot locate %s, which is open on %c:\n", info.name, g_letter);
    return RC_UNSUPPORTED;
}

/*
 * Walk DOS's system file tables (and the FCB table). A file open for
 * writing on the volume refuses the run, as DOS would write its clusters and
 * directory entry behind the engine's back; files open for reading stay in
 * place. Each one is looked up through the directory sector DOS keeps for it,
 * which also gives the full first cluster on FAT32.
 */
static int scan_sft(void)
{
    unsigned long lol = lol_addr();
    int table;

    g_nPins = 0;
    if (!lol)
        goto unknown;
    for (table = 0; table < 2; table++) {
        unsigned long at = lol + (table ? 0x1A : 0x04);
        int blocks = 0;

        for (;;) {
            unsigned off = _farpeekw(_dos_ds, at);
            unsigned seg = _farpeekw(_dos_ds, at + 2);
            unsigned long blk = ((unsigned long)seg << 4) + off;
            unsigned count, i;
            int rc;

            if (off == 0xFFFF || blk == 0)
                break;
            if (++blocks > 64 || !lin_ok(blk, 6))
                goto unknown;
            count = _farpeekw(_dos_ds, blk + 4);
            if (count > 255 || (count && !lin_ok(blk + 6, count * 0x3BUL)))
                goto unknown;
            for (i = 0; i < count; i++) {
                rc = check_open(blk + 6 + i * 0x3BUL, table);
                if (rc)
                    return rc;
            }
            at = blk;               /* the next block's address comes first */
        }
    }
    return RC_OK;
unknown:
    fail("cannot check which files are open on %c:\n", g_letter);
    return RC_UNSUPPORTED;
}

static int set_dir(const char *path)
{
    __dpmi_regs r;

    dosmemput(path, (int)strlen(path) + 1, g_miscLin + M_PATH);
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x3B;
    r.x.ds = (unsigned short)g_miscSeg;
    r.x.dx = M_PATH;
    return dos_call(&r);
}

/* DOS keeps the first cluster of each drive's current directory; make it
 * the root, which never moves, and remember the old one. */
static int cwd_to_root(void)
{
    __dpmi_regs r;
    char root[4];

    g_cwdSaved = 0;
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x47;
    r.h.dl = (unsigned char)g_drive;
    r.x.ds = (unsigned short)g_miscSeg;
    r.x.si = M_PATH;
    if (dos_call(&r)) {
        dosmemget(g_miscLin + M_PATH, 64, g_cwd);
        g_cwd[64] = 0;
        g_cwdSaved = g_cwd[0] != 0;
    }
    root[0] = (char)g_letter;
    root[1] = ':';
    root[2] = '\\';
    root[3] = 0;
    if (set_dir(root))
        return 1;
    g_cwdSaved = 0;
    return 0;
}

static void cwd_restore(void)
{
    char path[72];

    if (!g_cwdSaved)
        return;
    g_cwdSaved = 0;
    snprintf(path, sizeof(path), "%c:\\%s", g_letter, g_cwd);
    /* CHDIR (AH=3Bh) never changes the current drive, so there is no
     * drive to put back. */
    set_dir(path);
}

/* ------------------------------------------------------------ reporting */

static void report_out(void *ctx, int level, const char *line)
{
    (void)ctx;
    if (level == FAT_OUT_VERBOSE)
        verbose("%s", line);
    else
        say("%s", line);
}

static fat_report report_of(void)
{
    fat_report r;

    r.out = report_out;
    r.ctx = NULL;
    r.inUse = "open file";
    r.lostGap = "lost clusters (run ScanDisk)";
    r.bytesPerCluster = g_fv.bytesPerCluster;
    return r;
}

static void on_event(void *ctx, int ev, const char *path, int frags,
                     int newFrags)
{
    fat_report r = report_of();

    (void)ctx;
    fat_report_event(&r, ev, path, frags, newFrags);
}

static void print_kept_notes(const fat_stats *st)
{
    fat_report r = report_of();

    fat_report_kept(&r, st);
}

static void print_free_space(const fat_stats *st)
{
    fat_report r = report_of();

    fat_report_free_space(&r, st);
}

static void print_gap_notes(const fat_stats *st)
{
    fat_report r = report_of();

    fat_report_gaps(&r, st);
}

static void print_lost(const fat_stats *st)
{
    fat_report r = report_of();

    fat_report_lost(&r, st);
}

static const char *g_failWhat;      /* why the run was stopped */
static int         g_analyzing;

static const char *fat_type_name(void)
{
    switch (g_fv.fatType) {
    case FAT_TYPE_12: return "FAT12";
    case FAT_TYPE_16: return "FAT16";
    case FAT_TYPE_32: return "FAT32";
    default:          return "FAT";
    }
}

static unsigned long ul(fat_u64 v)
{
    return v > 0xFFFFFFFFull ? 0xFFFFFFFFul : (unsigned long)v;
}

static void on_progress(void *ctx, int phase, fat_u64 done, fat_u64 total)
{
    (void)ctx;
    if (poll_keys() && g_analyzing) {
        say("\nAnalysis stopped.\n");
        exit(RC_INCOMPLETE);        /* nothing was written */
    }
    if (phase == FAT_PH_SCAN)
        progress("  scanning: %lu files and folders", ul(done));
    else if (phase == FAT_PH_DEFRAG)
        progress("  defragmenting: %lu of %lu", ul(done), ul(total));
    else
        progress("  consolidating free space: %lu moved", ul(done));
}




/* The largest gaps left in the free space and what keeps each one open. */


/* The engine calls this before every burst of writes: stop on request. */
static int hook_begin(void *ctx)
{
    (void)ctx;
    if (poll_keys()) {
        g_failWhat = "stopped with ESC";
        return FAT_WR_FAIL;
    }
    return FAT_WR_OK;
}

static int mount_volume(int writable)
{
    fat_dev dev;

    memset(&dev, 0, sizeof(dev));
    dev.read = dev_read;
    dev.write = writable ? dev_write : NULL;
    dev.bytesPerSector = 512;
    memset(&g_fv, 0, sizeof(g_fv));
    if (fat_mount(&g_fv, &dev))
        return RC_OK;
    if (g_fv.fat == NULL && g_fv.fatBytes != 0 &&
        g_fv.fatBytes <= 64u * 1024u * 1024u) {
        fail("cannot load the FAT of %c: (%lu KB): not enough memory, or a "
             "read error\n", g_letter, (unsigned long)(g_fv.fatBytes / 1024));
        return RC_ACCESS;
    }
    fail("%c: is not a FAT volume\n", g_letter);
    return RC_UNSUPPORTED;
}

/* ------------------------------------------------------------ commands */

static int cmd_analyze(int letter)
{
    fat_defrag_cfg cfg;
    fat_stats st;
    int rc;

    rc = open_drive(letter);
    if (rc)
        return rc;
    disk_reset();                   /* DOS writes what it still holds */
    rc = mount_volume(0);
    if (rc)
        return rc;

    say("Analyzing %c: (%s)...\n\n", letter, fat_type_name());
    memset(&cfg, 0, sizeof(cfg));
    cfg.compact = 1;                /* plan a full defragmentation */
    cfg.offline = 1;
    cfg.progress = on_progress;
    cfg.event = on_event;
    g_analyzing = 1;
    rc = fat_run(&g_fv, &cfg, &st);
    g_analyzing = 0;
    progress_clear();
    if (rc < 0) {
        fat_unmount(&g_fv);
        fail("out of memory\n");
        return RC_ERROR;
    }

    say("Analysis of %c: complete\n", letter);
    say("  files examined       : %lu\n", ul(st.filesExamined));
    say("  folders examined     : %lu\n", ul(st.dirsExamined));
    say("  fragmented files     : %lu (%lu fragments)\n", ul(st.fragmented),
        ul(st.totalFragments));
    say("  fragmented folders   : %lu\n", ul(st.dirsFragmented));
    if (st.unmovable) {
        say("  system files         : %lu (never moved)\n", ul(st.unmovable));
        print_kept_notes(&st);
    }
    if (st.unmovableFragmented)
        say("  unmovable fragmented : %lu (root folder or system files)\n",
            ul(st.unmovableFragmented));
    if (st.chainErrors)
        say("  bad cluster chains   : %lu\n", ul(st.chainErrors));
    if (st.crossLinked)
        say("  cross-linked         : %lu\n", ul(st.crossLinked));
    if (st.ioErrors)
        say("  I/O errors           : %lu\n", ul(st.ioErrors));
    if (st.dotdotErrors)
        say("  folders with a kept old copy: %lu\n", ul(st.dotdotErrors));
    print_lost(&st);
    print_free_space(&st);

    if (st.chainErrors || st.crossLinked || st.ioErrors) {
        say("\nThe file system has errors. Run ScanDisk to repair them "
            "before defragmenting.\n");
    } else if (st.planDefrag || st.compactable) {
        const char *runs = st.planFreeRuns == 1 ? "" : "s";

        if (st.planDefrag && st.compactable)
            say("\nA full defragmentation would make %lu files and folders "
                "contiguous and\nmove %lu others to consolidate the free "
                "space (leaving %lu free run%s).\n", ul(st.planDefrag),
                ul(st.compactable), (unsigned long)st.planFreeRuns, runs);
        else if (st.planDefrag)
            say("\nA full defragmentation would make %lu files and folders "
                "contiguous\n(leaving %lu free run%s).\n", ul(st.planDefrag),
                (unsigned long)st.planFreeRuns, runs);
        else
            say("\nA full defragmentation would move %lu files and folders "
                "to consolidate the\nfree space (leaving %lu free run%s).\n",
                ul(st.compactable), (unsigned long)st.planFreeRuns, runs);
        if (st.gapNotes) {
            say("Gaps it would leave in the free space:\n");
            print_gap_notes(&st);
        }
        if (st.noSpace)
            say("%lu fragmented files or folders would not fit into any "
                "free run.\n", ul(st.noSpace));
        if (st.planDefrag)
            say("Defragmentation is recommended: hsdefrag %c: -d\n", letter);
        else
            say("To consolidate the free space: hsdefrag %c: -d\n", letter);
    } else if (st.gapNotes) {
        say("\nNo defragmentation needed: the files and folders are "
            "contiguous and the\nfree space is consolidated as far as "
            "possible. Gaps that stay:\n");
        print_gap_notes(&st);
    } else {
        say("\nNo defragmentation needed: the files and folders are "
            "contiguous and the\nfree space is consolidated.\n");
    }
    fat_unmount(&g_fv);
    return rc ? RC_INCOMPLETE : RC_OK;
}

/* Defragment one drive. job (boot pass) names the volume it must be. */
static int cmd_defrag(int letter, const bj_entry *job)
{
    fat_defrag_cfg cfg;
    fat_stats st;
    unsigned long start, secs = 0, serial;
    char a[32];
    int rc, e, run = -2;
    __dpmi_regs r;

    rc = open_drive(letter);
    if (rc)
        return rc;
    if (job && job->hasSerial &&
        (!bj_boot_serial(g_bs, &serial) || serial != job->serial)) {
        say("%c: is not the volume that was scheduled (serial %04lX-%04lX); "
            "skipped.\n", letter, (job->serial >> 16) & 0xFFFF,
            job->serial & 0xFFFF);
        return RC_ACCESS;
    }
    if (!cwd_to_root()) {
        fail("cannot change to the root folder of %c:\n", letter);
        return RC_ACCESS;
    }
    smartdrv_begin();
    disk_reset();
    e = lock_drive();
    if (e) {
        fail("cannot lock %c: (DOS error %d)\n", letter, e);
        rc = RC_ACCESS;
        goto out;
    }
    rc = mount_volume(1);
    if (!rc)
        rc = check_cds();
    if (!rc)
        rc = scan_sft();
    if (rc)
        goto out;

    memset(&cfg, 0, sizeof(cfg));
    cfg.doMove = 1;
    cfg.verify = 1;
    cfg.compact = 1;
    cfg.offline = 1;                /* Windows is not running */
    cfg.progress = on_progress;
    cfg.event = on_event;
    cfg.begin_write = hook_begin;
    cfg.is_unmovable = g_nPins ? hook_unmovable : NULL;
    say("Defragmenting %c: (%s)...\n", letter, fat_type_name());
    if (g_nPins)
        verbose("  %d open file%s in place\n", g_nPins,
                g_nPins == 1 ? " stays" : "s stay");
    g_failWhat = NULL;
    start = bios_ticks();
    run = fat_run(&g_fv, &cfg, &st);
    secs = ticks_since(start) * 10 / 182;

out:
    unlock_all();
    disk_reset();                   /* forget DOS's stale FAT/dir buffers */
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x710D;                /* MS-DOS 7: flush and invalidate */
    r.x.cx = 1;
    r.x.dx = (unsigned short)g_drive;
    dos_call(&r);
    smartdrv_end();
    cwd_restore();
    progress_clear();
    if (run == -2) {
        fat_unmount(&g_fv);
        return rc;
    }
    if (run < 0) {
        fat_unmount(&g_fv);
        fail("out of memory\n");
        return RC_ERROR;
    }

    say("\nDefragmentation of %c: %s in %lu:%02lu\n", letter,
        run ? "incomplete" : "finished", secs / 60, secs % 60);
    say("  files examined       : %lu\n", ul(st.filesExamined));
    say("  folders examined     : %lu\n", ul(st.dirsExamined));
    say("  files defragmented   : %lu\n", ul(st.defragged));
    say("  folders defragmented : %lu\n", ul(st.dirsDefragged));
    say("  moved to consolidate : %lu %s\n", ul(st.compacted),
        st.compacted == 1 ? "file or folder" : "files and folders");
    say("  data moved           : %s\n",
        fat_format_bytes(st.clustersMoved * g_fv.bytesPerCluster, a, sizeof(a)));
    if (st.unmovable) {
        say("  kept in place        : %lu system or open file%s\n",
            ul(st.unmovable), st.unmovable == 1 ? "" : "s");
        print_kept_notes(&st);
    }
    if (st.unmovableFragmented)
        say("  unmovable fragmented : %lu (root folder, system or open "
            "files)\n", ul(st.unmovableFragmented));
    print_lost(&st);
    print_free_space(&st);
    print_gap_notes(&st);

    if (st.noSpace)
        say("%lu fragmented files or folders did not fit into any free "
            "run.\n", ul(st.noSpace));
    if (st.busy)
        say("%lu files or folders kept changing and were skipped.\n",
            ul(st.busy));
    if (st.ioErrors)
        say("%lu I/O errors occurred; run ScanDisk with a thorough (surface) "
            "scan.\n", ul(st.ioErrors));
    if (st.chainErrors || st.crossLinked)
        say("The file system has errors (%lu bad chains, %lu cross-linked); "
            "run ScanDisk\nto repair them, then run hsdefrag again.%s\n",
            ul(st.chainErrors), ul(st.crossLinked),
            st.crossLinked ? " Nothing was moved." : "");
    if (st.aborted) {
        say("The run was stopped early.\n");
        if (g_failWhat)
            say("  reason: %s\n", g_failWhat);
    }
    fat_unmount(&g_fv);
    return run ? RC_INCOMPLETE : RC_OK;
}

/* ----------------------------------------------------------- boot pass */

/* "1997-03-14 07:05" from the DOS clock. */
static void now_text(char *buf, size_t cap)
{
    __dpmi_regs r;
    unsigned y, mo, d;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0x2A;
    dos_call(&r);
    y = r.x.cx;
    mo = r.h.dh;
    d = r.h.dl;
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x2C;
    dos_call(&r);
    snprintf(buf, cap, "%04u-%02u-%02u %02u:%02u", y, mo, d,
             (unsigned)r.h.ch, (unsigned)r.h.cl);
}

/*
 * Wait up to secs seconds for a key, counting down after prompt. Returns 0
 * on timeout, 27 for ESC, Ctrl+C or Ctrl+Break, else the key.
 */
static int wait_key(unsigned secs, const char *prompt)
{
    unsigned long start = bios_ticks(), limit = secs * 182UL / 10;
    unsigned shown = 0;
    int key = 0;

    for (;;) {
        unsigned long t = ticks_since(start);
        unsigned left;

        if (_go32_was_ctrl_break_hit() ||
            _farpeekb(_dos_ds, g_miscLin + M_BREAK)) {
            _farpokeb(_dos_ds, g_miscLin + M_BREAK, 0);
            key = 27;
            break;
        }
        if (kbhit()) {
            key = getkey();
            if (key == 3)
                key = 27;
            break;
        }
        if (t >= limit)
            break;
        left = (unsigned)((limit - t) * 10 / 182) + 1;
        if (left != shown) {
            shown = left;
            printf("\r%s %u ", prompt, left);
            fflush(stdout);
        }
        __dpmi_yield();
    }
    printf("\r%78s\r", "");
    fflush(stdout);
    return key;
}

/* HSDEFRAG.LOG next to the job file. */
static int write_log(const char *job)
{
    static const char cut[] = "(The log was cut short here.)\r\n";
    char path[160];
    size_t dir = 0, i;
    FILE *f;
    int ok;

    for (i = 0; job[i]; i++)
        if (job[i] == '\\' || job[i] == '/' || job[i] == ':')
            dir = i + 1;
    if (dir + sizeof("HSDEFRAG.LOG") > sizeof(path))
        return 0;
    memcpy(path, job, dir);
    strcpy(path + dir, "HSDEFRAG.LOG");
    f = fopen(path, "wb");
    if (!f)
        return 0;
    ok = fwrite(g_log, 1, g_logLen, f) == g_logLen;
    if (ok && g_logCut)
        ok = fwrite(cut, 1, sizeof(cut) - 1, f) == sizeof(cut) - 1;
    if (fclose(f) != 0)
        ok = 0;
    if (ok)
        say("\nThe report was saved to %s.\n", path);
    else
        fail("cannot write %s\n", path);
    return ok;
}

/* Set the text mode again, which also takes down the Windows 9x start-up
 * logo: it would hide the countdown and the progress. */
static void text_mode(void)
{
    __dpmi_regs r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0x0F;                       /* get the video mode */
    __dpmi_int(0x10, &r);
    r.x.ax = (r.h.al & 0x7F) == 7 ? 0x0007 : 0x0003;
    __dpmi_int(0x10, &r);
}

/*
 * The pass AUTOEXEC.BAT starts while the job file exists. The job file is
 * deleted first, so whatever happens the pass runs only once.
 */
static int cmd_boot(const char *job)
{
    static char text[4097];
    bj_entry e[BJ_MAX_DRIVES];
    char when[32];
    size_t len;
    FILE *f;
    int n, i, r, rc = RC_OK;

    text_mode();
    now_text(when, sizeof(when));
    say("Boot-time pass, %s\n", when);
    f = fopen(job, "rb");
    if (!f) {
        if (errno != ENOENT) {
            fail("cannot read the boot job %s\n", job);
            return RC_ACCESS;
        }
        say("Nothing is scheduled (%s not found).\n", job);
        g_logOn = 0;
        return RC_OK;
    }
    len = fread(text, 1, sizeof(text) - 1, f);
    r = ferror(f) || (len == sizeof(text) - 1 && fgetc(f) != EOF);
    if (ferror(f))
        r = 1;
    if (fclose(f) != 0)
        r = 1;
    if (r) {
        fail("cannot read the complete boot job %s; nothing was "
             "defragmented\n", job);
        rc = RC_ERROR;
        goto done;
    }
    text[len] = 0;
    n = bj_parse(text, len, e, BJ_MAX_DRIVES);
    if (remove(job) != 0) {
        fail("cannot delete %s, so nothing is defragmented (the pass would "
             "repeat at every start)\n", job);
        rc = RC_ACCESS;
        goto done;
    }
    disk_reset();
    if (n < 0) {
        fail("%s is damaged; nothing was defragmented\n", job);
        rc = RC_ERROR;
        goto done;
    }
    if (n == 0) {
        say("No drives are scheduled.\n");
        goto done;
    }

    say("Drives to defragment:");
    for (i = 0; i < n; i++)
        say(" %c:", e[i].drive);
    say("\n\n");
    if (wait_key(5, "Press ESC to skip, any other key to start now...")
        == 27) {
        say("Skipped (ESC).\n");
        goto done;
    }
    reset_keys();
    for (i = 0; i < n; i++) {
        r = cmd_defrag(e[i].drive, &e[i]);
        if (r > rc)
            rc = r;
        say("\n");
        if (g_cancel) {
            if (i + 1 < n)
                say("The other drives were skipped (ESC).\n");
            break;
        }
    }

done:
    if (!write_log(job) && rc == RC_OK)
        rc = RC_ERROR;
    g_logOn = 0;
    reset_keys();
    say("\n");
    wait_key(rc ? 30 : 5, rc ? "Please read the messages above. Press any "
             "key to continue..." : "Press any key to continue...");
    return rc;
}

/* --------------------------------------------------------------- main */

static void banner(void)
{
    say("House Soft Defrag (DOS) " HSD_VERSION
        " - portable console defragmenter\n");
}

static void usage(void)
{
    say("usage: hsdefrag <drive:> [options]\n"
        "       hsdefrag --boot <job file>\n\n"
        "  -a, --analyze   analyze only; report fragmentation (default)\n"
        "  -d, --defrag    defragment the drive\n"
        "  -v, --verbose   per-file output\n"
        "  -h, --help      show this help\n"
        "  --boot FILE     the boot-time pass AUTOEXEC.BAT runs, as scheduled "
        "by\n"
        "                  \"hsdefrag C: --boot-defrag\" on Windows 95/98\n\n"
        "examples:\n"
        "  hsdefrag C: -a        analyze drive C:\n"
        "  hsdefrag C: -d -v     defragment drive C:, list every file\n\n"
        "This is the MS-DOS side of hsdefrag.exe, for \"Command prompt "
        "only\", MS-DOS mode\nor a boot floppy; in Windows it runs as a "
        "Windows program. Everything moves,\nhidden, system and read-only "
        "files, the Windows swap file and registry too,\nexcept IO.SYS and "
        "the other hidden system files in the root folder that boot\ncode "
        "may find by position, LDLINUX.SYS, files DOS has open and the "
        "first\ncluster of the FAT32 root folder. ESC stops safely at any "
        "time.\n\n"
        "Exit codes: 0 ok, 1 usage, 2 drive not accessible, 3 unsupported, "
        "4 incomplete,\n5 runtime/output error.\n\n"
        "Built with DJGPP (https://www.delorie.com/djgpp/). It runs on the "
        "CWSDPMI DPMI\nhost, whose source is at\n"
        "  https://www.delorie.com/pub/djgpp/current/v2misc/csdpmi7s.zip\n");
}

/* Case-insensitive ASCII string equality. */
static int same(const char *a, const char *b)
{
    while (*a && upper((unsigned char)*a) == upper((unsigned char)*b)) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

/* -x, /x or --long, in any case. */
static int opt_is(const char *a, const char *s, const char *l)
{
    if ((a[0] == '-' || a[0] == '/') && same(a + 1, s))
        return 1;
    return a[0] == '-' && a[1] == '-' && same(a + 2, l);
}

/* "C", "C:" or "C:\"; returns the upper-case letter or 0. */
static int parse_drive(const char *a)
{
    int d = upper((unsigned char)a[0]);

    if (d < 'A' || d > 'Z')
        return 0;
    if (a[1] == 0 || (a[1] == ':' && (a[2] == 0 ||
                                      ((a[2] == '\\' || a[2] == '/') &&
                                       a[3] == 0))))
        return d;
    return 0;
}

static int usage_error(const char *fmt, const char *arg)
{
    banner();
    fail(fmt, arg);
    say("Run \"hsdefrag -h\" for help.\n");
    return RC_USAGE;
}

int main(int argc, char **argv)
{
    const char *job = NULL;
    int letter = 0, action = 0, i, rc;

    __djgpp_set_ctrl_c(0);
    _go32_want_ctrl_break(1);
    signal(SIGINT, on_sigint);

    if (argc < 2) {
        banner();
        usage();
        return RC_USAGE;
    }
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        int want = 0;

        if (opt_is(a, "h", "help") || opt_is(a, "?", "?")) {
            banner();
            usage();
            return RC_OK;
        } else if (opt_is(a, "a", "analyze")) {
            want = 1;
        } else if (opt_is(a, "d", "defrag")) {
            want = 2;
        } else if (opt_is(a, "boot", "boot")) {
            if (i + 1 >= argc)
                return usage_error("%s needs the job file\n", a);
            want = 3;
            job = argv[++i];
        } else if (opt_is(a, "v", "verbose")) {
            g_verbose = 1;
        } else if (a[0] == '-' || a[0] == '/') {
            return usage_error("unknown option: %s\n", a);
        } else if (!letter && parse_drive(a)) {
            letter = parse_drive(a);
        } else {
            return usage_error("unexpected argument: %s\n", a);
        }
        if (want) {
            if (action && action != want)
                return usage_error("%s: choose only one of -a, -d and "
                                   "--boot\n", a);
            action = want;
        }
    }
    if (action == 3 && letter)
        return usage_error("--boot takes no drive (%s lists them)\n", job);
    if (action != 3 && !letter)
        return usage_error("no drive specified%s\n", "");

    g_logOn = action == 3;
    banner();
    if (!dos_init()) {
        fail("not enough conventional memory\n");
        return RC_ERROR;
    }
    signal(SIGSEGV, on_fatal);
    signal(SIGFPE, on_fatal);
    signal(SIGILL, on_fatal);
    signal(SIGABRT, on_fatal);

    rc = check_dos();
    if (rc == RC_OK) {
        if (action == 3)
            rc = cmd_boot(job);
        else if (action == 2)
            rc = cmd_defrag(letter, NULL);
        else
            rc = cmd_analyze(letter);
    }
    fflush(stdout);
    if (ferror(stdout) && rc == RC_OK)
        rc = RC_ERROR;
    return rc;
}

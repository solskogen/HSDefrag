/*
 * main.c - Command-line entry point (Windows NT 4.0 and Windows 95/98/Me).
 */
#include "compat.h"
#include "util.h"
#include "volume.h"
#include "analyze.h"
#include "defrag.h"
#include "mft.h"
#include "schedule.h"
#include "fatwin9x.h"
#include "boot9x.h"
#include "fileenum.h"
#include "version.h"

/* The program name without its directory, for the usage text. */
static const wchar_t *base_name(const wchar_t *path)
{
    const wchar_t *p, *b = path;

    if (!path)
        return L"hsdefrag";
    for (p = path; *p; p++)
        if (*p == L'\\' || *p == L'/' || *p == L':')
            b = p + 1;
    return *b ? b : L"hsdefrag";
}

static void usage(const wchar_t *prog)
{
    prog = base_name(prog);
    uinfo(L"usage: %ls <drive:> [options]\n\n", prog);
    uinfo(L"  -a, --analyze   analyze only; report fragmentation (default)\n");
    uinfo(L"  -d, --defrag    defragment the volume\n");
    uinfo(L"  -m, --mft       report NTFS $MFT fragmentation; never move metadata\n");
    uinfo(L"  -v, --verbose   per-file output\n");
    uinfo(L"  -h, --help      show this help\n\n");
    uinfo(L"boot-time defragmentation (NTFS, verified backup required):\n"
          L"  --boot-defrag              native whole-volume file-data pass\n"
          L"  --boot-defrag --dry-run    read-only eligibility pre-check\n"
          L"  --boot-status              show the saved boot phase/result\n"
          L"  --boot-cancel              remove this drive's scheduled boot entry\n"
          L"On Windows 95/98 (FAT), --boot-defrag makes AUTOEXEC.BAT run "
          L"hsdefrag in MS-DOS\nat the next start, which also moves the swap "
          L"file, the registry and the files\nWindows keeps open; "
          L"--boot-status shows its report. hsdefrag.exe also runs\nin MS-DOS "
          L"mode.\n\n");
    uinfo(L"troubleshooting (analyze mode only):\n");
    uinfo(L"  --no-bitmap     do not read the volume cluster bitmap\n");
    uinfo(L"  --no-files      do not open or map individual files\n");
    uinfo(L"  --no-meta       do not read the NTFS metadata records\n\n");
    uinfo(L"examples:\n");
    uinfo(L"  %ls C: -a           analyze drive C:\n", prog);
    uinfo(L"  %ls C: -d -m -v     files + read-only MFT report, verbose\n", prog);
    uinfo(L"\nAdministrator rights are required for raw volume access.\n");
    uinfo(L"Exit codes: 0 ok, 1 usage, 2 volume not accessible, "
          L"3 unsupported, 4 incomplete, 5 runtime/output error.\n");
}

static int opt_is(const wchar_t *a, const wchar_t *s, const wchar_t *l)
{
    return (wcscmp(a, s) == 0) || (wcscmp(a, l) == 0);
}

/* Accept "C", "C:" or "C:\"; returns the upper-case letter or 0. */
static wchar_t parse_drive(const wchar_t *a)
{
    wchar_t d = a[0];

    if (d >= L'a' && d <= L'z')
        d = (wchar_t)(d - L'a' + L'A');
    if (d < L'A' || d > L'Z')
        return 0;
    if (a[1] == 0 || (a[1] == L':' && (a[2] == 0 ||
                                       (a[2] == L'\\' && a[3] == 0))))
        return d;
    return 0;
}

/* Explain a failed FSCTL_GET_VOLUME_BITMAP in terms of likely causes. */
static void explain_bitmap_failure(const VOLUME *v, HSD_NTSTATUS st)
{
    if (g_osMajor != 0 && g_osMajor < 4) {
        uinfo(L"Windows NT %lu.%lu lacks the defragmentation interface; "
              L"NT 4.0 or later is required.\n",
              (unsigned long)g_osMajor, (unsigned long)g_osMinor);
        return;
    }
    switch (st) {
    case HSD_STATUS_ACCESS_DENIED:
        uinfo(L"Run the program from an account with administrator rights.\n");
        break;
    case HSD_STATUS_INVALID_DEVICE_REQUEST:
    case HSD_STATUS_NOT_IMPLEMENTED:
    case HSD_STATUS_NOT_SUPPORTED:
        if (lstrcmpiW(v->fsName, L"FAT32") == 0 && !uos_at_least(5, 0))
            uinfo(L"FAT32 is not native to NT 4.0; the add-on FAT32 driver "
                  L"serving this volume does\nnot implement the "
                  L"defragmentation interface.\n");
        else if (!v->isNtfs && !v->isFat)
            uinfo(L"The %ls file system driver does not implement the "
                  L"defragmentation interface.\n",
                  v->fsName);
        else
            uinfo(L"The %ls driver of this volume rejected the request. A "
                  L"third-party file system\ndriver or a filter driver "
                  L"(antivirus, encryption, disk tools) may be intercepting\n"
                  L"it. Try another drive, or temporarily disable such "
                  L"software.\n",
                  v->fsName);
        break;
    default:
        break;
    }
}

/* Combine two exit codes, keeping the more serious one. A runtime failure
 * (5) outranks "unsupported" (3), which outranks an inaccessible volume (2),
 * which outranks "incomplete" (4): a later step that only finished partly
 * must not hide an earlier hard failure, and a hard failure in a later step
 * (the read-only $MFT report, say) must not be hidden by an earlier 4. */
static int exit_rank(int rc)
{
    switch (rc) {
    case 0: return 0;
    case 4: return 1;
    case 2: return 2;
    case 3: return 3;
    default: return 4; /* 5, and anything unexpected */
    }
}

static int worse(int a, int b)
{
    return exit_rank(b) > exit_rank(a) ? b : a;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t drive = 0;
    int doDefrag = 0;
    int action = 0, boot = 0, cancel = 0, bootStatus = 0, dryRun = 0;
    int reportMft = 0;
    int noBitmap = 0;
    unsigned aflags = 0;
    int i, rc;
    HSD_NTSTATUS st;
    VOLUME vol;

    uinfo(L"House Soft Defrag (%ls) %ls - portable console defragmenter\n",
          uos_is_9x() ? L"9x" : L"NT4", HSD_WVERSION);
    if (argc < 2) {
        usage(argc && argv ? argv[0] : NULL);
        return 1;
    }

    for (i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];

        if (opt_is(a, L"-h", L"--help") || wcscmp(a, L"/?") == 0) {
            usage(argv[0]);
            return 0;
        } else if (opt_is(a, L"-a", L"--analyze")) {
            if (action && action != 1)
                goto conflicting;
            action = 1;
            doDefrag = 0;
        } else if (opt_is(a, L"-d", L"--defrag")) {
            if (action && action != 2)
                goto conflicting;
            action = 2;
            doDefrag = 1;
        } else if (wcscmp(a, L"--boot-defrag") == 0) {
            if (action && action != 3)
                goto conflicting;
            action = 3;
            boot = 1;
        } else if (wcscmp(a, L"--boot-cancel") == 0) {
            if (action && action != 4)
                goto conflicting;
            action = 4;
            cancel = 1;
        } else if (wcscmp(a, L"--boot-status") == 0) {
            if (action && action != 5)
                goto conflicting;
            action = 5;
            bootStatus = 1;
        } else if (wcscmp(a, L"--dry-run") == 0) {
            dryRun = 1;
        } else if (opt_is(a, L"-m", L"--mft")) {
            reportMft = 1;
        } else if (opt_is(a, L"-v", L"--verbose")) {
            g_verbose = 1;
        } else if (wcscmp(a, L"--no-bitmap") == 0) {
            noBitmap = 1;
        } else if (wcscmp(a, L"--no-files") == 0) {
            aflags |= ANALYZE_NO_FILES;
        } else if (wcscmp(a, L"--no-meta") == 0) {
            aflags |= ANALYZE_NO_META;
        } else if (a[0] == L'-') {
            uerror(L"unknown option: %ls\n", a);
            return 1;
        } else if (!drive && parse_drive(a)) {
            drive = parse_drive(a);
        } else {
            uerror(L"unexpected argument: %ls\n", a);
            return 1;
        }
    }

    if (!drive) {
        uerror(L"no drive specified\n");
        usage(argv[0]);
        return 1;
    }
    if (doDefrag && (noBitmap || aflags)) {
        uerror(L"--no-bitmap, --no-files and --no-meta only work in analyze "
               L"mode (-a)\n");
        return 1;
    }
    if ((!boot && dryRun) ||
        ((boot || cancel || bootStatus) && (reportMft || noBitmap || aflags))) {
        uerror(L"boot options cannot be combined with online actions; "
               L"--dry-run requires --boot-defrag\n");
        return 1;
    }
    if (reportMft && (aflags & ANALYZE_NO_META)) {
        uerror(L"-m/--mft reads the NTFS metadata; it cannot be combined with "
               L"--no-meta\n");
        return 1;
    }
    if (!uoutput_ok())
        return 5;

    /* Windows 95/98/Me: no NT defragmentation interface exists, so use the
     * direct FAT engine, and the MS-DOS side of hsdefrag.exe for the
     * boot-time pass. The NT-only actions below are rejected here. */
    if (uos_is_9x()) {
        if (reportMft || noBitmap || aflags) {
            uerror(L"-m/--mft and --no-bitmap/--no-files/--no-meta are only "
                   L"available on Windows NT\n");
            return 1;
        }
        uos_detect();
        uos_print();
        if (boot)
            return cmd_boot9x_schedule(drive, dryRun);
        if (cancel)
            return cmd_boot9x_cancel(drive);
        if (bootStatus)
            return cmd_boot9x_status(drive);
        return doDefrag ? cmd_fat_defrag(drive) : cmd_fat_analyze(drive);
    }

    if (cancel)
        return cmd_boot_cancel(drive);
    if (bootStatus)
        return cmd_boot_status(drive);

    uos_detect();
    uos_print();
    if (g_osMajor != 0 && g_osMajor < 4)
        uinfo(L"warning: NT 4.0 or later is required for defragmentation.\n");

    if (!volume_open(&vol, drive)) {
        volume_close(&vol);
        return 2;
    }
    volume_print_info(&vol);
    if (boot) {
        rc = cmd_boot_schedule(&vol, dryRun);
        volume_close(&vol);
        return rc;
    }
    uverbose(L"  FSCTL interface    : %ls\n",
             hsd_fsctl_native() ? L"NtFsControlFile (ntdll)"
                               : L"DeviceIoControl");

    rc = 0;
    if (noBitmap) {
        uinfo(L"Cluster bitmap not read (--no-bitmap).\n");
    } else {
        st = volume_load_bitmap(&vol);
        if (!HSD_NT_SUCCESS(st)) {
            hsd_status_error(L"FSCTL_GET_VOLUME_BITMAP", st);
            explain_bitmap_failure(&vol, st);
            if (doDefrag) {
                volume_close(&vol);
                return 2;
            }
            uinfo(L"Continuing the analysis without the cluster bitmap.\n");
            rc = 4;
        }
    }
    uinfo(L"\n");

    /* Ctrl+C stops the file walk between files (a move in progress is one
     * FSCTL_MOVE_FILE call, which the filesystem completes or refuses as a
     * whole). Only here, on NT: the 9x engine installs its own handler. */
    enum_catch_ctrl_c();
    if (doDefrag) {
        rc = worse(rc, cmd_defrag(&vol));
    } else {
        unsigned analysisFlags = aflags;
        if (reportMft)
            analysisFlags |= ANALYZE_MFT_ELSEWHERE;
        rc = worse(rc, cmd_analyze(&vol, analysisFlags));
    }
    if (reportMft && uoutput_ok() && !enum_interrupted())
        rc = worse(rc, mft_report(&vol));

    volume_close(&vol);
    return rc;
conflicting:
    uerror(L"choose only one action: analyze, defrag, boot-defrag, "
           L"boot-status or boot-cancel\n");
    return 1;
}

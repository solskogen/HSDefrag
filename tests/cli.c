#include "compat.h"
#include "util.h"
#include "volume.h"
#include "schedule.h"
#include "fatwin9x.h"
#include "boot9x.h"

static int fake_open(VOLUME *, wchar_t);
static void fake_close(VOLUME *);
static void fake_info(const VOLUME *);
static int fake_schedule(VOLUME *, int);
static int fake_cancel(wchar_t);
static int fake_status(wchar_t);
static int fake_is_9x(void);
static int fake_schedule9x(wchar_t, int);
static int fake_cancel9x(wchar_t);
static int fake_status9x(wchar_t);
static int fake_fat_analyze(wchar_t);
static int fake_fat_defrag(wchar_t);

#define volume_open fake_open
#define volume_close fake_close
#define volume_print_info fake_info
#define cmd_boot_schedule fake_schedule
#define cmd_boot_cancel fake_cancel
#define cmd_boot_status fake_status
#define uos_is_9x fake_is_9x
#define cmd_boot9x_schedule fake_schedule9x
#define cmd_boot9x_cancel fake_cancel9x
#define cmd_boot9x_status fake_status9x
#define cmd_fat_analyze fake_fat_analyze
#define cmd_fat_defrag fake_fat_defrag
#define wmain dispatch
#include "../src/main.c"
#undef wmain

static int opens, closes, schedules, cancels, statuses, dry, checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    uinfo(L"FAIL CLI line %d\n", __LINE__); } } while (0)

static int fake_open(VOLUME *v, wchar_t drive)
{
    CHECK(drive == L'C');
    opens++;
    memset(v, 0, sizeof(*v));
    v->drive = drive;
    return 1;
}
static void fake_close(VOLUME *v) { (void)v; closes++; }
static void fake_info(const VOLUME *v) { (void)v; }
static int fake_schedule(VOLUME *v, int n)
{
    CHECK(v->drive == L'C');
    schedules++;
    dry = n;
    return 4;
}
static int fake_cancel(wchar_t drive) { CHECK(drive == L'C'); cancels++; return 5; }
static int fake_status(wchar_t drive) { CHECK(drive == L'C'); statuses++; return 4; }

/* Windows 9x: distinct return codes show which command ran. */
static int is9x, schedules9x, dry9x, cancels9x, statuses9x, analyses, defrags;
static int fake_is_9x(void) { return is9x; }
static int fake_schedule9x(wchar_t drive, int n)
{
    CHECK(drive == L'C');
    schedules9x++;
    dry9x = n;
    return 6;
}
static int fake_cancel9x(wchar_t drive) { CHECK(drive == L'C'); cancels9x++; return 7; }
static int fake_status9x(wchar_t drive) { CHECK(drive == L'C'); statuses9x++; return 8; }
static int fake_fat_analyze(wchar_t drive) { CHECK(drive == L'C'); analyses++; return 9; }
static int fake_fat_defrag(wchar_t drive) { CHECK(drive == L'C'); defrags++; return 10; }

int wmain(int argc, wchar_t **argv)
{
    wchar_t *args[] = {L"hsdefrag", L"C:", L"--boot-defrag", NULL, NULL};
    (void)argc; (void)argv;
    CHECK(dispatch(3, args) == 4 && schedules == 1 && !dry && opens == 1 && closes == 1);
    args[3] = L"--dry-run";
    CHECK(dispatch(4, args) == 4 && schedules == 2 && dry && opens == 2 && closes == 2);
    args[3] = L"--experimental";
    CHECK(dispatch(4, args) == 1 && schedules == 2 && opens == 2);
    args[2] = L"--boot-status";
    CHECK(dispatch(3, args) == 4 && statuses == 1 && opens == 2);
    args[2] = L"--boot-cancel";
    CHECK(dispatch(3, args) == 5 && cancels == 1 && opens == 2);
    args[3] = L"--boot-status";
    CHECK(dispatch(4, args) == 1 && statuses == 1 && cancels == 1 && opens == 2);
    args[2] = L"--boot-status"; args[3] = L"--dry-run";
    CHECK(dispatch(4, args) == 1 && statuses == 1 && opens == 2);
    args[2] = L"--boot-mft"; args[3] = NULL;
    CHECK(dispatch(3, args) == 1 && schedules == 2 && opens == 2);
    args[2] = L"--boot-defrag"; args[3] = NULL;
    CHECK(dispatch(3, args) == 4 && schedules == 3 && !dry && opens == 3 && closes == 3);
    args[3] = L"--dry-run";
    CHECK(dispatch(4, args) == 4 && schedules == 4 && dry && opens == 4 && closes == 4);
    args[3] = L"--boot-mft";
    CHECK(dispatch(4, args) == 1 && schedules == 4 && opens == 4);
    args[3] = L"-d";
    CHECK(dispatch(4, args) == 1 && schedules == 4 && opens == 4);
    args[3] = L"--boot-status";
    CHECK(dispatch(4, args) == 1 && schedules == 4 && opens == 4);

    /* Results combine by severity: 5 > 3 > 2 > 4 > 0, in either order. */
    CHECK(worse(0, 4) == 4 && worse(4, 0) == 4);
    CHECK(worse(4, 5) == 5 && worse(5, 4) == 5);
    CHECK(worse(4, 2) == 2 && worse(2, 4) == 2);
    CHECK(worse(2, 3) == 3 && worse(3, 2) == 3);
    CHECK(worse(3, 5) == 5 && worse(5, 3) == 5);
    CHECK(worse(0, 0) == 0 && worse(4, 4) == 4);
    /* -m reads the metadata that --no-meta says not to read. */
    args[2] = L"-m"; args[3] = L"--no-meta";
    CHECK(dispatch(4, args) == 1 && opens == 4);
    args[2] = L"--no-meta"; args[3] = L"--mft";
    CHECK(dispatch(4, args) == 1 && opens == 4);
    args[2] = L"-a"; args[3] = L"-m";
    {
        wchar_t *more[] = {L"hsdefrag", L"C:", L"-a", L"-m", L"--no-meta", NULL};
        CHECK(dispatch(5, more) == 1 && opens == 4);
    }

    /* Windows 9x: the FAT engine and the MS-DOS pass, never the NT volume. */
    is9x = 1;
    args[1] = L"c:"; args[2] = L"--boot-defrag"; args[3] = NULL;
    CHECK(dispatch(3, args) == 6 && schedules9x == 1 && !dry9x);
    args[3] = L"--dry-run";
    CHECK(dispatch(4, args) == 6 && schedules9x == 2 && dry9x);
    args[2] = L"--boot-cancel"; args[3] = NULL;
    CHECK(dispatch(3, args) == 7 && cancels9x == 1);
    args[2] = L"--boot-status";
    CHECK(dispatch(3, args) == 8 && statuses9x == 1);
    args[3] = L"--dry-run";
    CHECK(dispatch(4, args) == 1 && statuses9x == 1);
    args[2] = L"-d"; args[3] = NULL;
    CHECK(dispatch(3, args) == 10 && defrags == 1);
    args[2] = L"-a";
    CHECK(dispatch(3, args) == 9 && analyses == 1);
    CHECK(dispatch(2, args) == 9 && analyses == 2);
    args[3] = L"-m";
    CHECK(dispatch(4, args) == 1 && analyses == 2);
    args[3] = L"--no-files";
    CHECK(dispatch(4, args) == 1 && analyses == 2);
    args[2] = L"--boot-defrag"; args[3] = L"-m";
    CHECK(dispatch(4, args) == 1 && schedules9x == 2);
    CHECK(opens == 4 && closes == 4 && schedules == 4 && cancels == 1 &&
          statuses == 1);
    uinfo(L"Boot CLI dispatch regressions: %d checks, %d failures.\n", checks, failures);
    return failures ? 1 : 0;
}

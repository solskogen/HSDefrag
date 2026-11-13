/*
 * report.c - Regressions for the FAT engine's report text (fatreport.c).
 *
 * The Windows 9x and the MS-DOS front ends print these lines, so their
 * wording, number formatting and line-length rule are pinned down here,
 * including the words that differ between the two platforms.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fatreport.h"

static int checks;
static char got[4096];
static int levels[64], nlines;

static void ok(int cond, const char *what)
{
    if (!cond) {
        printf("FAIL: %s\ngot:\n%s", what, got);
        exit(1);
    }
    checks++;
}

static void capture(void *ctx, int level, const char *line)
{
    (void)ctx;
    if (strlen(got) + strlen(line) < sizeof(got))
        strcat(got, line);
    if (nlines < 64)
        levels[nlines++] = level;
}

static void reset(void)
{
    got[0] = 0;
    nlines = 0;
}

static fat_report win(void)
{
    fat_report r;

    r.out = capture;
    r.ctx = NULL;
    r.inUse = "in use";
    r.lostGap = "lost clusters, or a file being written";
    r.bytesPerCluster = 4096;
    return r;
}

static void bytes(void)
{
    char b[32];

    ok(!strcmp(fat_format_bytes(0, b, sizeof(b)), "0.0 B"), "0 B");
    ok(!strcmp(fat_format_bytes(1023, b, sizeof(b)), "1023.0 B"), "1023 B");
    ok(!strcmp(fat_format_bytes(1024, b, sizeof(b)), "1.0 KB"), "1 KB");
    ok(!strcmp(fat_format_bytes(1536, b, sizeof(b)), "1.5 KB"), "1.5 KB");
    ok(!strcmp(fat_format_bytes(1048575, b, sizeof(b)), "1024.0 KB"),
       "rounding below 1 MB");
    ok(!strcmp(fat_format_bytes(1474560, b, sizeof(b)), "1.4 MB"), "floppy");
    ok(!strcmp(fat_format_bytes(5ULL << 40, b, sizeof(b)), "5120.0 GB"),
       "GB is the largest unit");
    ok(!strcmp(fat_format_bytes(1536, b, 4), "1.5"), "cut to cap");
}

static void events(void)
{
    fat_report r = win(), d = win();

    d.inUse = "open file";
    reset();
    fat_report_event(&r, FAT_EV_CONTIGUOUS, "\\A.TXT", 1, 3);
    fat_report_event(&r, FAT_EV_FRAGMENTED, "\\B.TXT", 4, 9);
    fat_report_event(&r, FAT_EV_DEFRAGGED, "\\C.TXT", 2, 1);
    fat_report_event(&r, FAT_EV_MOVED, "\\D.TXT", 7, 0);
    fat_report_event(&r, FAT_EV_NOSPACE, "\\E.TXT", 5, 0);
    ok(!strcmp(got,
               "  contiguous (3 clusters): \\A.TXT\n"
               "  fragmented (4 fragments, 9 clusters): \\B.TXT\n"
               "  defragmented (2 fragments): \\C.TXT\n"
               "  moved (7 clusters): \\D.TXT\n"
               "  no contiguous free space yet, retried later "
               "(5 fragments): \\E.TXT\n"), "verbose events");
    ok(nlines == 5 && levels[0] == FAT_OUT_VERBOSE &&
       levels[4] == FAT_OUT_VERBOSE, "verbose level");

    reset();
    fat_report_event(&r, FAT_EV_UNMOVABLE, "\\WIN386.SWP", 3, FAT_GAP_SYSTEM);
    fat_report_event(&r, FAT_EV_UNMOVABLE, "\\X.DLL", 1, FAT_GAP_INUSE);
    fat_report_event(&d, FAT_EV_UNMOVABLE, "\\X.DLL", 1, FAT_GAP_INUSE);
    ok(!strcmp(got,
               "  kept in place, system file (3 fragments): \\WIN386.SWP\n"
               "  kept in place, in use: \\X.DLL\n"
               "  kept in place, open file: \\X.DLL\n"), "kept in place");

    reset();
    fat_report_event(&r, FAT_EV_BUSY, "\\F", 0, 0);
    fat_report_event(&r, FAT_EV_IOERROR, "\\G", 0, 0);
    fat_report_event(&r, FAT_EV_CHAINERROR, "\\H", 0, 0);
    fat_report_event(&r, FAT_EV_CROSSLINKED, "\\I", 0, 0);
    fat_report_event(&r, FAT_EV_DOTDOT, "\\J", 0, 0);
    ok(!strcmp(got,
               "  skipped, kept changing: \\F\n"
               "  I/O error: \\G\n"
               "  bad cluster chain: \\H\n"
               "  cross-linked: \\I\n"
               "  moved, but the old copy was kept: some \"..\" entries of "
               "its subfolders still point there: \\J\n"), "info events");
    ok(nlines == 5 && levels[0] == FAT_OUT_INFO && levels[4] == FAT_OUT_INFO,
       "info level");

    reset();
    fat_report_event(&r, 9999, "\\K", 0, 0);
    ok(nlines == 0, "unknown event prints nothing");
}

static void summaries(void)
{
    static fat_stats st;
    fat_report r = win(), d = win();
    char longp[64];

    d.inUse = "open file";
    d.lostGap = "lost clusters (run ScanDisk)";
    memset(&st, 0, sizeof(st));

    st.unmovable = 10;
    st.keptNotes = 2;
    st.keptNote[0].clusters = 256;
    st.keptNote[0].frags = 3;
    st.keptNote[0].why = FAT_GAP_SYSTEM;
    strcpy(st.keptNote[0].path, "\\WIN386.SWP");
    st.keptNote[1].clusters = 1;
    st.keptNote[1].frags = 1;
    st.keptNote[1].why = FAT_GAP_INUSE;
    strcpy(st.keptNote[1].path, "\\WINDOWS\\X.DLL");
    reset();
    fat_report_kept(&r, &st);
    ok(!strcmp(got,
               "    \\WIN386.SWP (system file, 1.0 MB, 3 fragments)\n"
               "    \\WINDOWS\\X.DLL (in use, 4.0 KB)\n"
               "    ... and 8 more files\n"), "kept notes");
    st.unmovable = 3;
    reset();
    fat_report_kept(&d, &st);
    ok(strstr(got, "(open file, 4.0 KB)") &&
       strstr(got, "... and 1 more file\n"), "kept notes, DOS words");

    st.freeClusters = 300;
    st.freeRuns = 1;
    st.largestFreeRun = 300;
    reset();
    fat_report_free_space(&r, &st);
    ok(!strcmp(got,
               "  free space           : 1.2 MB in 1 run (largest 1.2 MB)\n"),
       "free space, no gaps");
    st.freeRuns = 3;
    st.freeGaps = 2;
    st.gapClusters = 20;
    reset();
    fat_report_free_space(&r, &st);
    ok(!strcmp(got,
               "  free space           : 1.2 MB in 3 runs (largest 1.2 MB)\n"
               "  free between files   : 80.0 KB in 2 gaps\n"),
       "free space with gaps");

    st.gapsLeft = 6;
    st.gapNotes = 4;
    st.gapNote[0].clusters = 10;
    st.gapNote[0].why = FAT_GAP_INUSE;
    strcpy(st.gapNote[0].path, "\\X.DLL");
    st.gapNote[1].clusters = 2;
    st.gapNote[1].why = FAT_GAP_ROOT;
    st.gapNote[2].clusters = 1;
    st.gapNote[2].why = FAT_GAP_LOST;
    st.gapNote[3].clusters = 1;
    st.gapNote[3].why = FAT_GAP_BAD;
    reset();
    fat_report_gaps(&r, &st);
    ok(!strcmp(got,
               "    40.0 KB before \\X.DLL (in use)\n"
               "    8.0 KB before the root folder\n"
               "    4.0 KB before lost clusters, or a file being written\n"
               "    4.0 KB before bad clusters\n"
               "    ... and 2 more gaps\n"), "gap notes");
    reset();
    fat_report_gaps(&d, &st);
    ok(strstr(got, "before \\X.DLL (open file)\n") &&
       strstr(got, "before lost clusters (run ScanDisk)\n"),
       "gap notes, DOS words");

    /* A long path keeps its end, and the line stays within 79 columns. */
    memset(longp, 'P', sizeof(longp) - 1);
    longp[0] = '\\';
    longp[sizeof(longp) - 1] = 0;
    longp[sizeof(longp) - 2] = 'Z';
    st.gapsLeft = st.gapNotes = 1;
    st.gapNote[0].why = FAT_GAP_STUCK;
    strcpy(st.gapNote[0].path, longp);
    reset();
    fat_report_gaps(&r, &st);
    ok(strlen(got) - 1 <= 79 && strstr(got, "before ...PPP") &&
       strstr(got, "PZ (not moved)\n"), "long path cut at the start");

    st.lostClusters = 0;
    reset();
    fat_report_lost(&d, &st);
    ok(nlines == 0, "no lost clusters, no line");
    st.lostClusters = 3;
    reset();
    fat_report_lost(&d, &st);
    ok(!strcmp(got, "  lost clusters        : 3 (12.0 KB; run ScanDisk to "
                    "free them)\n"), "lost clusters");
}

int main(void)
{
    bytes();
    events();
    summaries();
    printf("report: %d checks passed\n", checks);
    return 0;
}

/*
 * Host tests for bootjob.c: the job file and the AUTOEXEC.BAT line of the
 * Windows 95/98 boot-time pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bootjob.h"

static unsigned checks;

#define CHECK(c)                                                              \
    do {                                                                      \
        checks++;                                                             \
        if (!(c)) {                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,  \
                    #c);                                                      \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

#define LINE "@IF EXIST C:\\HSDEFRAG.JOB C:\\WINDOWS\\HSDEFRAG.EXE --boot " \
             "C:\\HSDEFRAG.JOB"

static int parse(const char *s, bj_entry *e)
{
    return bj_parse(s, strlen(s), e, BJ_MAX_DRIVES);
}

/* Edit text and compare with want; also with every smaller buffer. */
static void edit_is(const char *text, const char *line, const char *want)
{
    char out[1024];
    size_t n = bj_autoexec_edit(text, strlen(text), line, out, sizeof(out));
    size_t cap;

    if (n == BJ_ERR || strcmp(out, want) != 0) {
        fprintf(stderr, "edit of [%s]:\n got [%s]\nwant [%s]\n", text,
                n == BJ_ERR ? "(error)" : out, want);
        CHECK(0);
    }
    CHECK(n == strlen(want));
    for (cap = 0; cap <= n; cap++) {
        char *small = malloc(cap ? cap : 1);

        CHECK(bj_autoexec_edit(text, strlen(text), line, small, cap) == BJ_ERR);
        free(small);
    }
}

static void test_serial(void)
{
    unsigned char bs[512];
    unsigned long s = 0;

    memset(bs, 0, sizeof(bs));
    bs[0x16] = 0x20; /* FAT16: extended BPB at 26h */
    CHECK(!bj_boot_serial(bs, &s));
    bs[0x26] = 0x29;
    bs[0x27] = 0x4D;
    bs[0x28] = 0x3C;
    bs[0x29] = 0x2B;
    bs[0x2A] = 0x1A;
    CHECK(bj_boot_serial(bs, &s) && s == 0x1A2B3C4DUL);
    memset(bs, 0, sizeof(bs)); /* FAT32: extended BPB at 42h */
    bs[0x26] = 0x29;
    CHECK(!bj_boot_serial(bs, &s));
    bs[0x42] = 0x29;
    bs[0x43] = 0xEF;
    bs[0x44] = 0xBE;
    bs[0x45] = 0xAD;
    bs[0x46] = 0xDE;
    CHECK(bj_boot_serial(bs, &s) && s == 0xDEADBEEFUL);
}

static void test_job(void)
{
    static const char *const bad[] = {
        "C\r\n", "C:x\r\n", "1:\r\n", "C: 1A2B3C4D\r\n", "C: 1A2B-3C4G\r\n",
        "C: 1A2B-3C4D x\r\n", "C: 1A2B-3C4\r\n", ":\r\n", "C;\r\n",
    };
    bj_entry e[BJ_MAX_DRIVES], f[BJ_MAX_DRIVES], x;
    char buf[1024];
    size_t n, k;
    int i, m;

    CHECK(parse("", e) == 0);
    CHECK(parse("; only a comment\r\n\r\n   \r\n", e) == 0);
    m = parse("; head\r\nC: 1A2B-3C4D\r\n  d:\n\te:  0000-00ff  \r\nc:\r\n", e);
    CHECK(m == 3);
    CHECK(e[0].drive == 'C' && e[0].hasSerial && e[0].serial == 0x1A2B3C4DUL);
    CHECK(e[1].drive == 'D' && !e[1].hasSerial);
    CHECK(e[2].drive == 'E' && e[2].hasSerial && e[2].serial == 0xFFUL);
    CHECK(parse("C:\r\n\x1A" "garbage", e) == 1);
    CHECK(parse("C:", e) == 1 && e[0].drive == 'C');
    for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
        CHECK(parse(bad[k], e) == -1);
    CHECK(bj_parse("C:\r\nD:\r\n", 8, e, 1) == -1);

    m = parse("C: 1A2B-3C4D\r\nD:\r\n", e);
    n = bj_format(e, m, buf, sizeof(buf));
    CHECK(n != BJ_ERR && n == strlen(buf) && buf[0] == ';');
    CHECK(strstr(buf, "\r\nC: 1A2B-3C4D\r\nD:\r\n") != NULL);
    CHECK(parse(buf, f) == 2 && memcmp(e, f, 2 * sizeof(bj_entry)) == 0);
    CHECK(bj_format(e, m, buf, 20) == BJ_ERR);

    x.drive = 'd';
    x.hasSerial = 1;
    x.serial = 0x12345678UL;
    m = bj_set(e, m, &x);
    CHECK(m == 2 && e[1].drive == 'D' && e[1].serial == 0x12345678UL);
    x.drive = 'F';
    m = bj_set(e, m, &x);
    CHECK(m == 3 && bj_find(e, m, 'f') == 2);
    m = bj_remove(e, m, 'c');
    CHECK(m == 2 && e[0].drive == 'D' && e[1].drive == 'F');
    CHECK(bj_remove(e, m, 'Z') == 2 && bj_find(e, m, 'C') == -1);
    for (i = 0, m = 0; i < 30; i++) {
        x.drive = (char)('A' + i % 26);
        m = bj_set(e, m, &x);
    }
    CHECK(m == 26);
    n = bj_format(e, m, buf, sizeof(buf));
    CHECK(n != BJ_ERR && parse(buf, f) == 26);
}

static void test_line(void)
{
    char buf[200], longp[120];

    CHECK(bj_line("C:\\WINDOWS\\HSDEFRAG.EXE", "C:\\HSDEFRAG.JOB", buf,
                  sizeof(buf)) == strlen(LINE));
    CHECK(strcmp(buf, LINE) == 0);
    CHECK(bj_is_ours(buf, strlen(buf)));
    CHECK(bj_line("C:\\WINDOWS\\HSDEFRAG.EXE", "C:\\HSDEFRAG.JOB", buf,
                  strlen(LINE)) == BJ_ERR);
    memset(longp, 'A', sizeof(longp) - 1);
    longp[sizeof(longp) - 1] = 0;
    CHECK(bj_line(longp, "C:\\HSDEFRAG.JOB", buf, sizeof(buf)) == BJ_ERR);
    CHECK(!bj_is_ours("C:\\HSDEFRAG.EXE C:", sizeof("C:\\HSDEFRAG.EXE C:") - 1));
    CHECK(!bj_is_ours("HSDEFRAG C: --boot-defrag",
                      sizeof("HSDEFRAG C: --boot-defrag") - 1));
    CHECK(!bj_is_ours("REM --boot here", 15));
    CHECK(!bj_is_ours("if exist x d:\\hsdefragd --BOOT y",
                     sizeof("if exist x d:\\hsdefragd --BOOT y") - 1));
    CHECK(bj_is_ours("if exist x d:\\hsdefrag.exe --BOOT y",
                    sizeof("if exist x d:\\hsdefrag.exe --BOOT y") - 1));
    CHECK(!bj_is_ours("ECHO HSDEFRAG --boot x", sizeof("ECHO HSDEFRAG --boot x") - 1));
    CHECK(!bj_is_ours("REM " LINE, sizeof("REM " LINE) - 1));
    CHECK(!bj_is_ours("if exist x d:\\myhsdefrag.exe --boot y",
                     sizeof("if exist x d:\\myhsdefrag.exe --boot y") - 1));
    CHECK(!bj_is_ours(LINE " > C:\\KEEP.TXT", sizeof(LINE " > C:\\KEEP.TXT") - 1));
}

static void test_autoexec(void)
{
    const char *const L = LINE "\r\n";
    char want[1024], out[1024];
    const char *t;

    edit_is("", LINE, L);
    edit_is("", NULL, "");
    snprintf(want, sizeof(want), "@ECHO OFF\r\n%sPATH C:\\DOS\r\n", L);
    edit_is("@ECHO OFF\r\nPATH C:\\DOS\r\n", LINE, want);
    snprintf(want, sizeof(want), " @echo \t off \r\n%s", L);
    edit_is(" @echo \t off \r\n", LINE, want);
    snprintf(want, sizeof(want), "@echo off\r\n%s", L);
    edit_is("@echo off", LINE, want);
    snprintf(want, sizeof(want), "%sPATH X\r\n", L);
    edit_is("PATH X\r\n", LINE, want);
    snprintf(want, sizeof(want), "%s@ECHO OFFX\r\n", L);
    edit_is("@ECHO OFFX\r\n", LINE, want);
    snprintf(want, sizeof(want), "%sECHO OFF\nPATH X", L);
    edit_is("ECHO OFF\nPATH X", LINE, want);
    snprintf(want, sizeof(want), "%s\x1A", L);
    edit_is("\x1A", LINE, want);

    /* A line with another path is replaced; removal keeps the rest. */
    t = "@ECHO OFF\r\nSET A=1\r\n@if exist D:\\HSDEFRAG.JOB D:\\HSDEFRAG.EXE "
        "--boot D:\\HSDEFRAG.JOB\r\nSET B=2\r\n";
    CHECK(bj_autoexec_count(t, strlen(t)) == 1);
    snprintf(want, sizeof(want), "@ECHO OFF\r\n%sSET A=1\r\nSET B=2\r\n", L);
    edit_is(t, LINE, want);
    edit_is(t, NULL, "@ECHO OFF\r\nSET A=1\r\nSET B=2\r\n");
    edit_is(want, NULL, "@ECHO OFF\r\nSET A=1\r\nSET B=2\r\n");
    edit_is(want, LINE, want);
    CHECK(bj_autoexec_count(want, strlen(want)) == 1);
    edit_is(L, NULL, "");
    edit_is(LINE, NULL, "");
    edit_is("REM " LINE "\r\nECHO HSDEFRAG --boot x\r\n", NULL,
            "REM " LINE "\r\nECHO HSDEFRAG --boot x\r\n");
    snprintf(want, sizeof(want), "%s%sPATH X\r\n%s", L, L, LINE);
    CHECK(bj_autoexec_count(want, strlen(want)) == 3);
    edit_is(want, NULL, "PATH X\r\n");
    CHECK(bj_autoexec_edit(want, strlen(want), LINE, out, sizeof(out)) !=
          BJ_ERR);
    CHECK(bj_autoexec_count(out, strlen(out)) == 1);
    CHECK(strncmp(out, L, strlen(L)) == 0);
}

int main(void)
{
    test_serial();
    test_job();
    test_line();
    test_autoexec();
    printf("bootjob: %u checks passed\n", checks);
    return 0;
}

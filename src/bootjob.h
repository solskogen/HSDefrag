/*
 * bootjob.h - Text handling for the Windows 95/98 boot-time pass.
 *
 * "hsdefrag C: --boot-defrag" on Windows 95/98 writes a job file listing the
 * drives to defragment and a line in AUTOEXEC.BAT that runs hsdefrag.exe, which
 * is an MS-DOS program too, before Windows starts:
 *
 *   @IF EXIST C:\HSDEFRAG.JOB C:\WINDOWS\HSDEFRAG.EXE --boot C:\HSDEFRAG.JOB
 *
 * The MS-DOS side deletes the job file before it starts, so the line stays
 * dormant until the next pass is scheduled. Each job line carries the volume
 * serial number, so a drive letter that means another volume under MS-DOS is
 * skipped.
 *
 * Text is only edited in memory here: the module is shared by the Windows and
 * the MS-DOS side of hsdefrag.exe and tested on the host.
 */
#ifndef HSDEFRAG_BOOTJOB_H
#define HSDEFRAG_BOOTJOB_H

#include <stddef.h>

#define BJ_MAX_DRIVES 26
#define BJ_MAX_LINE   127        /* longest line COMMAND.COM runs        */
#define BJ_ERR        ((size_t)-1)

typedef struct {
    char          drive;         /* 'A'..'Z'                             */
    int           hasSerial;
    unsigned long serial;        /* volume serial number (32 bits)        */
} bj_entry;

/* Volume serial number from a FAT12/16/32 boot sector (512 bytes). Returns
 * 0 if the boot sector has no extended boot signature. */
int bj_boot_serial(const unsigned char *bs, unsigned long *serial);

/* Parse a job file into e[cap]. Blank lines and lines starting with ';' are
 * ignored; every other line is "X:" with an optional serial "1A2B-3C4D".
 * Returns the number of drives, or -1 if a line is malformed. */
int bj_parse(const char *text, size_t len, bj_entry *e, int cap);

/* Format a job file. Returns its length, or BJ_ERR if cap is too small. */
size_t bj_format(const bj_entry *e, int n, char *out, size_t cap);

/* Index of drive in e[n], or -1. */
int bj_find(const bj_entry *e, int n, char drive);

/* Add or replace a drive (e has room for BJ_MAX_DRIVES); returns the new n. */
int bj_set(bj_entry *e, int n, const bj_entry *x);

/* Remove a drive; returns the new n. */
int bj_remove(bj_entry *e, int n, char drive);

/* The AUTOEXEC.BAT line for exePath and jobPath (short MS-DOS paths).
 * Returns its length, or BJ_ERR if longer than BJ_MAX_LINE or cap. */
size_t bj_line(const char *exePath, const char *jobPath, char *out, size_t cap);

/* True if an AUTOEXEC.BAT line (without its line end) is ours. */
int bj_is_ours(const char *line, size_t len);

/* Number of our lines in AUTOEXEC.BAT text. */
int bj_autoexec_count(const char *text, size_t len);

/*
 * Copy AUTOEXEC.BAT text to out without our lines and, if line is not NULL,
 * with line inserted after a leading "@ECHO OFF" (else first). Line ends are
 * CR LF. Returns the new length, or BJ_ERR if cap is too small.
 */
size_t bj_autoexec_edit(const char *text, size_t len, const char *line,
                        char *out, size_t cap);

#endif /* HSDEFRAG_BOOTJOB_H */

/*
 * fatreport.h - The FAT engine's report text, shared by the Windows 9x and
 * the MS-DOS front ends.
 *
 * Both front ends print the same events and summaries (files kept in
 * place, free space, the gaps left and what keeps them open). Writing that
 * text once keeps the two from drifting apart and lets the host tests check
 * it. Lines are built as 8-bit text, with file names exactly as stored on
 * disk (OEM code page), and handed to the front end, which prints them in
 * its own way (the Windows side converts them to UTF-16 first). No C
 * library and no floating point: the same code runs in the freestanding
 * Win32 program and under DJGPP.
 */
#ifndef FATREPORT_H
#define FATREPORT_H

#include "fatfs.h"
#include "fatdefrag.h"

enum { FAT_OUT_INFO, FAT_OUT_VERBOSE };

typedef struct {
    /* Prints one line (it ends in '\n'); level is FAT_OUT_INFO or
     * FAT_OUT_VERBOSE. */
    void (*out)(void *ctx, int level, const char *line);
    void *ctx;
    /* The words that differ between the platforms. */
    const char *inUse;      /* a file kept in place because it is in use:
                             * "in use" (Windows), "open file" (MS-DOS) */
    const char *lostGap;    /* what a gap before unowned clusters is:
                             * "lost clusters, or a file being written" or
                             * "lost clusters (run ScanDisk)" */
    fat_u32 bytesPerCluster;
} fat_report;

/* "12.3 MB": integer arithmetic only. cap must be at least 16. */
const char *fat_format_bytes(fat_u64 bytes, char *buf, unsigned cap);

/* One engine event (the fat_cfg.on_event callback). */
void fat_report_event(const fat_report *r, int ev, const char *path,
                      int frags, int newFrags);

void fat_report_kept(const fat_report *r, const fat_stats *st);
void fat_report_free_space(const fat_report *r, const fat_stats *st);
void fat_report_gaps(const fat_report *r, const fat_stats *st);
void fat_report_lost(const fat_report *r, const fat_stats *st);

#endif

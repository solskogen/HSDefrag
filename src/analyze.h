/*
 * analyze.h - Fragmentation analysis and reporting.
 */
#ifndef HSDEFRAG_ANALYZE_H
#define HSDEFRAG_ANALYZE_H

#include "volume.h"

/* cmd_analyze() flags that skip a pass (to bisect kernel-level trouble). */
#define ANALYZE_NO_FILES 0x1 /* do not open and map individual files */
#define ANALYZE_NO_META  0x2 /* do not read the NTFS metadata records */
/* The caller prints the $MFT report itself (-a -m): skip it here quietly. */
#define ANALYZE_MFT_ELSEWHERE 0x4

/* Analyze fragmentation on the volume and print a report. Returns 0 on
 * success, 4 when the report is incomplete (or Ctrl+C stopped it), 5 on a
 * runtime or output failure. */
int cmd_analyze(VOLUME *v, unsigned flags);

#endif /* HSDEFRAG_ANALYZE_H */

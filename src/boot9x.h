/*
 * boot9x.h - The boot-time pass on Windows 95/98 (see boot9x.c).
 *
 * Exit codes match the rest of the tool: 0 ok, 2 not accessible,
 * 3 unsupported, 5 runtime/output error.
 */
#ifndef HSDEFRAG_BOOT9X_H
#define HSDEFRAG_BOOT9X_H

/* Schedule a pass over drive at the next start (dryRun: change nothing). */
int cmd_boot9x_schedule(wchar_t drive, int dryRun);

/* Take drive out of the job; with no drive left, remove everything. */
int cmd_boot9x_cancel(wchar_t drive);

/* Show what is scheduled and the report of the last pass. */
int cmd_boot9x_status(wchar_t drive);

#endif /* HSDEFRAG_BOOT9X_H */

/*
 * defrag.h - Cluster relocation primitives and the volume defrag driver.
 */
#ifndef HSDEFRAG_DEFRAG_H
#define HSDEFRAG_DEFRAG_H

#include "compat.h"
#include "volume.h"
#include "retrieval.h"

/*
 * Open an ordinary file for NT 4.0's data-move interface.
 * Returns INVALID_HANDLE_VALUE on failure.
 */
HANDLE defrag_open(const wchar_t *path);

/*
 * Move the file described by 'm' (read from hFile) into one contiguous free
 * run, or consolidate complete fragments if only smaller holes exist.
 * RELOC_PARTIAL means a verified reduction, not a contiguous file.
 * The result is verified by re-reading the map into 'm' (FAT reports
 * success even when it does not move anything). Transient NTFS refusals are
 * retried. *status receives the NTSTATUS behind RELOC_FAILED/UNSUPPORTED.
 */
reloc_result defrag_relocate(VOLUME *v, HANDLE hFile, EXTMAP *m,
                             HSD_NTSTATUS *status);

/* Explain why the file system refused to move clusters on this volume. */
void defrag_explain_unsupported(const VOLUME *v, HSD_NTSTATUS st);

/*
 * Defragment every eligible ordinary file on the volume.
 * Returns 0 on success, 3 if the file system does not support moves.
 */
int cmd_defrag(VOLUME *v);

#endif /* HSDEFRAG_DEFRAG_H */

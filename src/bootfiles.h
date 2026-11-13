#ifndef HSDEFRAG_BOOTFILES_H
#define HSDEFRAG_BOOTFILES_H

#include "ntrt.h"
#include "bootlog.h"

/* All traversal/data handles are closed before returning. Files move only
 * through mounted NTFS; this routine never writes raw metadata. */
int boot_files_run(HANDLE volume, wchar_t drive, ntfs_u64 resultReference,
                    ULONG sector, ntfs_u64 volumeBytes,
                    int dryRun, boot_file_stats *stats,
                    int (*progress)(void *), void *progressContext);

#endif

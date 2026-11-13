/*
 * compat.h - Target/compat definitions for the NT 4.0 defragmenter.
 *
 * We target Windows NT 4.0, so we pin _WIN32_WINNT before any system header is
 * pulled in. The defrag IOCTLs (FSCTL_GET_VOLUME_BITMAP,
 * FSCTL_GET_RETRIEVAL_POINTERS, FSCTL_MOVE_FILE) and their structures were
 * introduced in NT 4.0 and are provided by <winioctl.h>. Fallback definitions
 * are guarded so the tool still builds on a toolchain whose headers predate
 * them.
 */
#ifndef HSDEFRAG_COMPAT_H
#define HSDEFRAG_COMPAT_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0400
#endif
#ifndef WINVER
#define WINVER 0x0400
#endif

#include <windows.h>
#include <winioctl.h>

/* Virtual LCN marker used in retrieval pointers for sparse/compressed runs. */
#ifndef LCN_VIRTUAL
#define LCN_VIRTUAL ((LONGLONG)-1)
#endif

#ifndef FSCTL_ALLOW_EXTENDED_DASD_IO
#define FSCTL_ALLOW_EXTENDED_DASD_IO \
    CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 32, METHOD_NEITHER, FILE_ANY_ACCESS)
#endif

#ifndef FSCTL_MOVE_FILE
#define FSCTL_MOVE_FILE \
    CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 29, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)

typedef struct {
    HANDLE        FileHandle;
    LARGE_INTEGER StartingVcn;
    LARGE_INTEGER StartingLcn;
    DWORD         ClusterCount;
} MOVE_FILE_DATA, *PMOVE_FILE_DATA;
#endif /* FSCTL_MOVE_FILE */

#endif /* HSDEFRAG_COMPAT_H */

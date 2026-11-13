/*
 * retrieval.h - File extent (VCN -> LCN) maps via FSCTL_GET_RETRIEVAL_POINTERS.
 */
#ifndef HSDEFRAG_RETRIEVAL_H
#define HSDEFRAG_RETRIEVAL_H

#include "compat.h"
#include "ntfsctl.h"
#include "filemove.h"
#include "volume.h"

fm_status win_file_control(void *ctx, void *handle, unsigned code, void *in,
                           unsigned inBytes, void *out, unsigned outBytes,
                           unsigned *returned);

/* Initialize an empty extent map. */
void extmap_init(EXTMAP *m);

/* Reset an extent map for reuse (keeps the allocation). */
void extmap_reset(EXTMAP *m);

/* Release an extent map's memory. */
void extmap_free(EXTMAP *m);

/* Append one run (lcn may be LCN_VIRTUAL). Returns FALSE when out of memory. */
int extmap_add(EXTMAP *m, long long vcn, long long lcn, long long count);

/*
 * Read the retrieval pointers for an open file/dir handle into the map.
 * Returns HSD_STATUS_SUCCESS, or the NTSTATUS of the failing
 * FSCTL_GET_RETRIEVAL_POINTERS call. A file without allocated clusters
 * (STATUS_END_OF_FILE, e.g. NTFS-resident data) succeeds with n == 0.
 */
HSD_NTSTATUS extmap_get(VOLUME *volume, HANDLE hFile, EXTMAP *m);

/*
 * Number of physical fragments: runs with a real LCN, where runs that continue
 * exactly where the previous one ended on disk count as one fragment.
 */
int extmap_fragments(const EXTMAP *m);

#endif /* HSDEFRAG_RETRIEVAL_H */

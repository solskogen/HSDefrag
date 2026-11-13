/*
 * Read-only NTFS $MFT inspection.
 *
 * The analyzer validates NTFS geometry, the MFT mapping pairs, $MFTMirr,
 * $Volume's dirty flag and the allocation bitmap. All volume access is through
 * the read-only callback below.
 */
#ifndef HSDEFRAG_NTFSMFT_H
#define HSDEFRAG_NTFSMFT_H

typedef unsigned char      ntfs_u8;
typedef unsigned short     ntfs_u16;
typedef unsigned int       ntfs_u32;
typedef unsigned long long ntfs_u64;

typedef struct ntfsmft_io {
    void *ctx;
    int (*read)(void *ctx, ntfs_u64 off, void *buf, ntfs_u32 len);
    ntfs_u32 sectorSize;
    ntfs_u64 length;
} ntfsmft_io;

typedef void (*ntfsmft_log)(void *logctx, const char *line);

typedef enum {
    MFT_OK = 0,
    MFT_READ_ERROR,
    MFT_NOT_NTFS,
    MFT_BAD_GEOMETRY,
    MFT_BAD_RECORD,
    MFT_HAS_ATTRLIST,
    MFT_SPARSE,
    MFT_VOLUME_DIRTY,
    MFT_MIRROR_MISMATCH,
    MFT_NOMEM
} mft_status;

typedef struct {
    ntfs_u64 mftLcn;
    ntfs_u32 recSize;
    ntfs_u64 bytesPerCluster;
    ntfs_u64 totalClusters;
    ntfs_u64 mftClusters;
    ntfs_u64 mftBytes;
    int fragments;
} ntfsmft_info;

mft_status ntfsmft_analyze(ntfsmft_io *io, ntfsmft_info *out,
                           ntfsmft_log log, void *logctx);
const char *ntfsmft_strstatus(mft_status st);

#endif

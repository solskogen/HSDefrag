/*
 * volume.h - Volume handling: open, cluster geometry, and the free/used
 * cluster bitmap.
 */
#ifndef HSDEFRAG_VOLUME_H
#define HSDEFRAG_VOLUME_H

#include "compat.h"
#include "ntfsctl.h"

/* A cluster range that must never be used as a move target. */
typedef struct {
    unsigned long long lcn;
    unsigned long long count;
} LCN_RANGE;

typedef struct {
    wchar_t            drive;             /* e.g. L'C' */
    wchar_t            root[8];           /* e.g. "C:\\"  */
    wchar_t            fsName[32];        /* e.g. "NTFS", "FAT" */
    UINT               driveType;         /* GetDriveTypeW() result */
    HANDLE             hVol;              /* \\.\C: volume handle */
    DWORD              bytesPerSector;
    DWORD              sectorsPerCluster;
    unsigned long long bytesPerCluster;
    unsigned long long totalClusters;     /* from GetDiskFreeSpaceW */
    unsigned long long freeClusters;      /* from GetDiskFreeSpaceW */
    int                isNtfs;
    int                isFat;

    /*
     * NT 4.0 / Windows 2000 NTFS moves data in 16-cluster units and cannot
     * move at all on volumes with clusters larger than 4 KB.
     */
    unsigned           moveAlign;         /* VCN granularity of a move */
    int                movesUnsupported;  /* known not to support moves */

    /* NTFS volume data (FSCTL_GET_NTFS_VOLUME_DATA), when available. */
    int                haveNtfsData;
    HSD_NTSTATUS        ntfsDataStatus;    /* result of that request */
    unsigned long long mftStartLcn;
    unsigned long long mft2StartLcn;
    unsigned long long mftValidDataLength;
    DWORD              mftRecordSize;     /* bytes per MFT file record */
    unsigned long long mftZoneStart;
    unsigned long long mftZoneEnd;

    /* 1 bit per cluster, 1 = used. Length = (bitmapClusters + 7) / 8. */
    BYTE              *bitmap;
    unsigned long long bitmapClusters;    /* clusters covered by the bitmap */
    int                haveBitmap;
    DWORD              bitmapChunk;       /* bitmap bytes per FSCTL that worked */

    /* Ranges excluded from move targets (MFT zone, rejected targets). */
    LCN_RANGE         *excl;
    int                nExcl;
    int                capExcl;
    int                exclLost;          /* an exclusion could not be kept:
                                             no more moves (see volume_exclude) */
} VOLUME;

/* Open the volume for the given drive letter. Returns TRUE on success. */
int volume_open(VOLUME *v, wchar_t drive);

/* Print geometry / file system details for diagnostics. */
void volume_print_info(const VOLUME *v);

/*
 * Load the full cluster allocation bitmap into memory. Returns the NTSTATUS of
 * the failing FSCTL_GET_VOLUME_BITMAP call, or HSD_STATUS_SUCCESS.
 */
HSD_NTSTATUS volume_load_bitmap(VOLUME *v);

/*
 * Re-read the bitmap from the file system (keeping exclusions). On failure the
 * previous in-memory bitmap is kept. Returns the NTSTATUS.
 */
HSD_NTSTATUS volume_refresh_bitmap(VOLUME *v);

/* Query whether a single cluster is marked used. */
int volume_cluster_used(const VOLUME *v, unsigned long long lcn);

/* Mark a run of clusters used (used != 0) or free (used == 0). */
void volume_mark(VOLUME *v, unsigned long long lcn, unsigned long long count,
                 int used);

/*
 * Permanently exclude a cluster range from move targets. Returns 0 if the
 * range could not be recorded (out of memory): it is still marked in the
 * current bitmap, but the next refresh would hand it out again, so
 * v->exclLost is set and the defrag driver stops moving.
 */
int volume_exclude(VOLUME *v, unsigned long long lcn,
                   unsigned long long count);

/* Largest contiguous free run, in clusters. */
unsigned long long volume_largest_free_run(const VOLUME *v);

/*
 * Read raw bytes from the volume. offset and len must be multiples of the
 * sector size and buf should be page aligned (VirtualAlloc). Returns TRUE on
 * success; on failure GetLastError() describes the problem.
 */
int volume_read_raw(VOLUME *v, unsigned long long offset, void *buf, DWORD len);

/* Close handles and release the bitmap. */
void volume_close(VOLUME *v);

#endif /* HSDEFRAG_VOLUME_H */

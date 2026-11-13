/*
 * fatfs.h - Portable FAT12/16/32 reader/writer used by the Windows 9x path.
 *
 * This module is deliberately free of any Win32 dependency: it operates on an
 * abstract sector device (fat_dev) so the same logic drives both the Windows 9x
 * raw-sector layer (VWIN32 absolute disk I/O) and the host unit tests, which
 * back it with an in-memory disk image. All disk access goes through the
 * device's read/write callbacks, in whole sectors.
 */
#ifndef HSDEFRAG_FATFS_H
#define HSDEFRAG_FATFS_H

#include <stddef.h>

typedef unsigned char      fat_u8;
typedef unsigned short     fat_u16;
typedef unsigned int       fat_u32;   /* 32-bit on win32 and the LP64 host */
typedef unsigned long long fat_u64;

/*
 * Abstract logical-sector device. read/write transfer "count" whole sectors
 * starting at logical sector "sector" and return 1 on success, 0 on failure.
 * write may be NULL for a read-only device. bytesPerSector is filled in by the
 * caller before fat_mount().
 */
typedef struct fat_dev {
    void   *ctx;
    int   (*read)(void *ctx, fat_u32 sector, fat_u32 count, void *buf);
    int   (*write)(void *ctx, fat_u32 sector, fat_u32 count, const void *buf);
    fat_u32 bytesPerSector;
} fat_dev;

enum { FAT_TYPE_12 = 12, FAT_TYPE_16 = 16, FAT_TYPE_32 = 32 };

typedef struct {
    fat_dev            dev;

    /* Geometry taken from the BIOS parameter block. */
    fat_u32            bytesPerSector;
    fat_u32            sectorsPerCluster;
    fat_u32            reservedSectors;
    fat_u32            numFats;
    fat_u32            rootEntryCount;    /* FAT12/16 only */
    fat_u32            fatSize;           /* sectors per FAT */
    fat_u64            totalSectors;
    fat_u32            rootCluster;       /* FAT32 only */
    fat_u32            fsInfoSector;      /* FAT32 only (0 if none) */

    /* Derived layout. */
    int                fatType;           /* FAT_TYPE_12/16/32 */
    fat_u32            firstDataSector;
    fat_u32            fatStartSector;    /* start of the ACTIVE FAT */
    fat_u32            rootDirSectors;    /* FAT12/16 fixed root */
    fat_u32            firstRootDirSector;
    fat_u32            countOfClusters;   /* data clusters (indices 2..n+1) */
    fat_u32            bytesPerCluster;

    /* FAT32 mirroring (BPB_ExtFlags). When mirroring is disabled only one FAT
     * is live; we must read from and write to that copy, not FAT #0. */
    int                fatMirrored;       /* 1 = all FATs kept identical */
    fat_u32            activeFat;         /* index of the live FAT (0-based) */

    /* Active FAT loaded into memory; written back on flush (to every copy when
     * mirroring is enabled, otherwise only to the active copy). Only the FAT
     * sectors changed since the last flush are written: on a large FAT32
     * volume rewriting the whole table for every moved file would cost
     * megabytes of I/O per move. */
    fat_u8            *fat;
    fat_u32            fatBytes;
    int                fatDirty;
    fat_u8            *dirtyMap;          /* one bit per FAT sector */
} fat_volume;

/* End-of-chain / bad / free sentinels for the mounted FAT type. */
int     fat_is_eoc(const fat_volume *fv, fat_u32 value);
int     fat_is_bad(const fat_volume *fv, fat_u32 value);
int     fat_is_free(fat_u32 value);

/*
 * Mount the volume: read and validate the boot sector, compute the layout and
 * load the first FAT into memory. dev (with ctx/read/write/bytesPerSector set)
 * is copied into the volume. Returns 1 on success, 0 on failure.
 */
int     fat_mount(fat_volume *fv, const fat_dev *dev);

/* Release the in-memory FAT. */
void    fat_unmount(fat_volume *fv);

/* Read/write a FAT entry in memory (set marks the FAT dirty). */
fat_u32 fat_get(const fat_volume *fv, fat_u32 cluster);
void    fat_set(fat_volume *fv, fat_u32 cluster, fat_u32 value);

/* End-of-chain marker value to store for the mounted FAT type. */
fat_u32 fat_eoc_value(const fat_volume *fv);

/* First logical sector of a data cluster (cluster >= 2). */
fat_u32 fat_cluster_sector(const fat_volume *fv, fat_u32 cluster);

/* Whole-sector device I/O, chunked into Win9x-friendly transfers. 1 on ok. */
int     fat_read_sectors(fat_volume *fv, fat_u32 sector, fat_u32 count,
                         void *buf);
int     fat_write_sectors(fat_volume *fv, fat_u32 sector, fat_u32 count,
                          const void *buf);

/* Metadata writes must be read back successfully before old storage is freed. */
int     fat_write_verified(fat_volume *fv, fat_u32 sector, fat_u32 count,
                           const void *buf);

/*
 * Invalidate the FAT32 FSInfo free-cluster hints (set to "unknown") so the OS
 * recomputes them. No-op on FAT12/16. Returns 1 on success.
 */
int     fat_fsinfo_invalidate(fat_volume *fv);

/* Write the FAT sectors changed since the last flush back to every on-disk
 * copy (or only to the active copy when mirroring is off). 1 on success. */
int     fat_flush(fat_volume *fv);

/*
 * Discard the in-memory FAT and read the active FAT again from disk, e.g. after
 * another process changed the live volume. Pending unflushed changes are lost.
 * Returns 1 on success; on failure the in-memory FAT must not be trusted.
 */
int     fat_reload(fat_volume *fv);
/* Check mirrored FATs against the loaded active FAT. No-op when not mirrored. */
int     fat_mirrors_match(fat_volume *fv);
/*
 * Undo the unflushed changes to the in-memory FAT by reading just the changed
 * sectors again from disk. Returns 1 on success; on failure the in-memory FAT
 * must not be trusted.
 */
int     fat_revert(fat_volume *fv);

/* TRUE if cluster is a valid data-cluster index (2 .. countOfClusters+1). */
int     fat_cluster_valid(const fat_volume *fv, fat_u32 cluster);

/*
 * Find the first run of "need" consecutive free clusters. Returns the starting
 * cluster (>= 2) or 0 if no such run exists.
 */
fat_u32 fat_find_free_run(const fat_volume *fv, fat_u32 need);

/* Count free clusters, free runs, and the largest free run (any may be NULL). */
void    fat_free_space(const fat_volume *fv, fat_u32 *freeClusters,
                       fat_u32 *freeRuns, fat_u32 *largestRun);

#endif /* HSDEFRAG_FATFS_H */

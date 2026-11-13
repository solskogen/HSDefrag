#ifndef HSDEFRAG_BOOTLOG_H
#define HSDEFRAG_BOOTLOG_H

#include "ntfsmft.h"

#define BOOTLOG_BYTES 4096U
typedef struct {
    void *ctx;
    int (*read)(void *, ntfs_u64, void *, ntfs_u32);
    int (*write)(void *, ntfs_u64, const void *, ntfs_u32);
    int (*flush)(void *);
    ntfs_u64 length;
} bootlog_io;

typedef enum {
    BOOT_PREPARE, BOOT_OPEN_VOLUME, BOOT_GEOMETRY,
    BOOT_REMOVE_ENTRY, BOOT_FILES, BOOT_FINISH
} boot_phase;

typedef enum {
    BOOT_PENDING, BOOT_RUNNING, BOOT_SUCCESS, BOOT_FAILED
} boot_result;

typedef enum {
    FILES_PENDING, FILES_RUNNING, FILES_COMPLETE, FILES_PARTIAL,
    FILES_UNSUPPORTED, FILES_ERROR
} boot_files_result;

typedef struct {
    ntfs_u64 files, streams, fragmented, moved, contiguous, busy, special;
    ntfs_u64 noSpace, errors, directories, metadata, refused;
    ntfs_u64 reduced;
    ntfs_u64 partial;   /* improved, but still fragmented */
    ntfs_u32 ntstatus;
    boot_files_result result;
    char errorContext[128];
    ntfs_u32 controlCode, controlCapacity, controlReturned;
    ntfs_u64 retrieval[4];
    ntfs_u64 allocationBytes, dataBytes;
    ntfs_u32 standardStatus, standardReturned;
} boot_file_stats;

typedef struct {
    ntfs_u64 jobId;
    unsigned drive;
    boot_phase phase;
    boot_result result;
    ntfs_u32 ntstatus;
    boot_file_stats files;
} boot_record;

int bootlog_create(bootlog_io *io, unsigned drive, ntfs_u64 jobId);
int bootlog_read(bootlog_io *io, boot_record *record);
int bootlog_write(bootlog_io *io, const boot_record *record);
const char *bootlog_phase_name(boot_phase phase);
const char *bootlog_files_result_name(boot_files_result result);

#endif

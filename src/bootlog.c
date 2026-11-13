/*
 * bootlog.c - The 4-KiB checksummed boot result record (C:\hsdefrag.boot).
 *
 * The native helper records its job state here (pending, running, success,
 * failed) plus coverage counters, and --boot-status reads it back. The
 * record is written whole, flushed and read back; a bad size, checksum or
 * field makes it invalid, never "probably fine". It is a status record,
 * not an undo journal: nothing is ever recovered from it. Shared by the
 * scheduler and the native helper through the bootlog_io callbacks, so it
 * is host-testable.
 */
#include <string.h>
#include "bootlog.h"
#include "ntfsrecord.h"

static ntfs_u32 get32(const ntfs_u8 *p)
{
    return (ntfs_u32)p[0] | ((ntfs_u32)p[1] << 8) |
           ((ntfs_u32)p[2] << 16) | ((ntfs_u32)p[3] << 24);
}

static ntfs_u64 get64(const ntfs_u8 *p)
{
    return get32(p) | ((ntfs_u64)get32(p + 4) << 32);
}

static void put32(ntfs_u8 *p, ntfs_u32 v)
{
    unsigned i;
    for (i = 0; i < 4; i++)
        p[i] = (ntfs_u8)(v >> (8 * i));
}

static void put64(ntfs_u8 *p, ntfs_u64 v)
{
    put32(p, (ntfs_u32)v);
    put32(p + 4, (ntfs_u32)(v >> 32));
}

static int bounds(bootlog_io *io)
{
    return io && io->read && io->length == BOOTLOG_BYTES;
}

static int valid(const boot_record *r)
{
    return r->jobId &&
           r->drive >= 'A' && r->drive <= 'Z' &&
           (unsigned)r->phase <= BOOT_FINISH &&
           (unsigned)r->result <= BOOT_FAILED &&
           (unsigned)r->files.result <= FILES_ERROR &&
           !r->files.errorContext[sizeof(r->files.errorContext) - 1] &&
           (r->result != BOOT_SUCCESS ||
            (r->phase == BOOT_FINISH && r->files.result == FILES_COMPLETE &&
             !r->ntstatus && !r->files.errors && !r->files.noSpace &&
             !r->files.partial && !r->files.refused));
}

int bootlog_read(bootlog_io *io, boot_record *r)
{
    ntfs_u8 data[BOOTLOG_BYTES];
    ntfs_u32 sum;
    if (!r || !bounds(io) ||
        !io->read(io->ctx, io->length - BOOTLOG_BYTES, data, sizeof(data)))
        return 0;
    sum = get32(data + 12);
    put32(data + 12, 0);
    if (memcmp(data, "UDBOOT17", 8) || get32(data + 8) != 1 ||
        sum != ~ntfs_crc32_update(~0U, data, sizeof(data)))
        return 0;
    memset(r, 0, sizeof(*r));
    r->jobId = get64(data + 32);
    r->drive = get32(data + 56);
    r->phase = (boot_phase)get32(data + 60);
    r->result = (boot_result)get32(data + 64);
    r->ntstatus = get32(data + 68);
    r->files.files = get64(data + 336);
    r->files.streams = get64(data + 344);
    r->files.fragmented = get64(data + 352);
    r->files.moved = get64(data + 360);
    r->files.contiguous = get64(data + 368);
    r->files.busy = get64(data + 376);
    r->files.special = get64(data + 384);
    r->files.noSpace = get64(data + 392);
    r->files.errors = get64(data + 400);
    r->files.directories = get64(data + 408);
    r->files.metadata = get64(data + 416);
    r->files.refused = get64(data + 424);
    r->files.reduced = get64(data + 648);
    r->files.partial = get64(data + 656);
    r->files.ntstatus = get32(data + 432);
    r->files.result = (boot_files_result)get32(data + 436);
    r->files.allocationBytes = get64(data + 624);
    r->files.dataBytes = get64(data + 632);
    r->files.standardStatus = get32(data + 640);
    r->files.standardReturned = get32(data + 644);
    memcpy(r->files.errorContext, data + 448, sizeof(r->files.errorContext));
    r->files.controlCode = get32(data + 576);
    r->files.controlCapacity = get32(data + 580);
    r->files.controlReturned = get32(data + 584);
    {
        unsigned i;
        for (i = 0; i < 4; i++)
            r->files.retrieval[i] = get64(data + 592 + i * 8);
    }
    return valid(r);
}

int bootlog_write(bootlog_io *io, const boot_record *r)
{
    ntfs_u8 data[BOOTLOG_BYTES], check[BOOTLOG_BYTES];
    if (!r || !bounds(io) || !io->write || !io->flush || !valid(r))
        return 0;
    memset(data, 0, sizeof(data));
    memcpy(data, "UDBOOT17", 8);
    put32(data + 8, 1);
    put64(data + 32, r->jobId);
    put32(data + 56, r->drive);
    put32(data + 60, r->phase);
    put32(data + 64, r->result);
    put32(data + 68, r->ntstatus);
    put64(data + 336, r->files.files);
    put64(data + 344, r->files.streams);
    put64(data + 352, r->files.fragmented);
    put64(data + 360, r->files.moved);
    put64(data + 368, r->files.contiguous);
    put64(data + 376, r->files.busy);
    put64(data + 384, r->files.special);
    put64(data + 392, r->files.noSpace);
    put64(data + 400, r->files.errors);
    put64(data + 408, r->files.directories);
    put64(data + 416, r->files.metadata);
    put64(data + 424, r->files.refused);
    put64(data + 648, r->files.reduced);
    put64(data + 656, r->files.partial);
    put32(data + 432, r->files.ntstatus);
    put32(data + 436, r->files.result);
    put64(data + 624, r->files.allocationBytes);
    put64(data + 632, r->files.dataBytes);
    put32(data + 640, r->files.standardStatus);
    put32(data + 644, r->files.standardReturned);
    memcpy(data + 448, r->files.errorContext, sizeof(r->files.errorContext));
    put32(data + 576, r->files.controlCode);
    put32(data + 580, r->files.controlCapacity);
    put32(data + 584, r->files.controlReturned);
    {
        unsigned i;
        for (i = 0; i < 4; i++)
            put64(data + 592 + i * 8, r->files.retrieval[i]);
    }
    put32(data + 12, ~ntfs_crc32_update(~0U, data, sizeof(data)));
    return io->write(io->ctx, io->length - BOOTLOG_BYTES, data, sizeof(data)) &&
           io->flush(io->ctx) &&
           io->read(io->ctx, io->length - BOOTLOG_BYTES, check, sizeof(check)) &&
           !memcmp(data, check, sizeof(data));
}

int bootlog_create(bootlog_io *io, unsigned drive, ntfs_u64 jobId)
{
    ntfs_u8 data[BOOTLOG_BYTES];
    boot_record r;
    unsigned i;
    if (!bounds(io) ||
        !io->read(io->ctx, io->length - BOOTLOG_BYTES, data, sizeof(data)))
        return 0;
    for (i = 0; i < sizeof(data); i++)
        if (data[i])
            return 0;
    memset(&r, 0, sizeof(r));
    r.jobId = jobId;
    r.drive = drive;
    return bootlog_write(io, &r);
}

const char *bootlog_phase_name(boot_phase phase)
{
    switch (phase) {
    case BOOT_PREPARE: return "prepared; waiting for boot";
    case BOOT_OPEN_VOLUME: return "opening the volume";
    case BOOT_GEOMETRY: return "querying disk geometry";
    case BOOT_REMOVE_ENTRY: return "removing the BootExecute entry";
    case BOOT_FINISH: return "finished";
    case BOOT_FILES: return "defragmenting the volume before Win32 starts";
    default: return "invalid boot phase";
    }
}

const char *bootlog_files_result_name(boot_files_result result)
{
    switch (result) {
    case FILES_PENDING: return "not started";
    case FILES_RUNNING: return "interrupted/in progress";
    case FILES_COMPLETE: return "completed (reported exclusions apply)";
    case FILES_PARTIAL: return "incomplete";
    case FILES_UNSUPPORTED: return "unsupported by this NTFS driver/geometry";
    case FILES_ERROR: return "stopped by a runtime/I/O failure";
    default: return "invalid file-pass result";
    }
}

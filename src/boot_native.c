/*
 * boot_native.c - Entry point of hsdefrgb.exe, the NTDLL-only helper that
 * Session Manager runs from BootExecute before Win32 starts.
 *
 * The helper is a small state machine driven by the result record
 * (X:\hsdefrag.boot, bootlog.c) that --boot-defrag wrote. Only a valid
 * PENDING record for the drive named on the command line starts a pass.
 * The record is marked RUNNING and the BootExecute entry is removed before
 * the volume is touched, so a crash or power loss during the pass can never
 * make it run again at the next boot; the record then still says RUNNING
 * and --boot-status reports the pass as interrupted. Each phase is saved
 * before it starts, and the file pass itself (bootfiles.c) moves data only
 * through FSCTL_MOVE_FILE. The volume is not locked or dismounted.
 *
 * Output goes through NtDisplayString: nothing here may import KERNEL32.
 */
#include "ntrt.h"
#include "bootcfg.h"
#include "bootfiles.h"
#include "version.h"

typedef struct { HANDLE handle; } result_context;
static boot_record job;
static bootlog_io resultIo;

static int result_read(void *ctx, ntfs_u64 off, void *buf, ntfs_u32 len)
{
    result_context *c = ctx;
    LARGE_INTEGER offset;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    offset.QuadPart = (long long)off;
    memset(&io, 0, sizeof(io));
    status = NtReadFile(c->handle, NULL, NULL, NULL, &io, buf, len, &offset, NULL);
    return ncomplete(c->handle, status, &io) == STATUS_SUCCESS && io.Information == len;
}

static int result_write(void *ctx, ntfs_u64 off, const void *buf, ntfs_u32 len)
{
    result_context *c = ctx;
    LARGE_INTEGER offset;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    offset.QuadPart = (long long)off;
    memset(&io, 0, sizeof(io));
    status = NtWriteFile(c->handle, NULL, NULL, NULL, &io, (void *)buf, len, &offset, NULL);
    return ncomplete(c->handle, status, &io) == STATUS_SUCCESS && io.Information == len;
}

static int result_flush(void *ctx)
{
    result_context *c = ctx;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    memset(&io, 0, sizeof(io));
    status = NtFlushBuffersFile(c->handle, &io);
    return ncomplete(c->handle, status, &io) == STATUS_SUCCESS;
}

static int save_job(void *ctx)
{
    (void)ctx;
    if (!bootlog_write(&resultIo, &job)) {
        nout(L"Cannot save the boot result; stopping new moves.");
        return 0;
    }
    return noutput_ok();
}

static HANDLE open_path(wchar_t *path, ULONG access, ULONG options, NTSTATUS *status)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    HANDLE handle = NULL;
    RtlInitUnicodeString(&name, path);
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    memset(&io, 0, sizeof(io));
    *status = NtOpenFile(&handle, SYNCHRONIZE | access, &attributes, &io,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         FILE_SYNCHRONOUS_IO_NONALERT | options);
    *status = ncomplete(handle, *status, &io);
    if (*status == STATUS_SUCCESS && !handle)
        *status = (NTSTATUS)0xE0000002L;
    return *status == STATUS_SUCCESS ? handle : NULL;
}

static NTSTATUS remove_bootexecute(wchar_t drive)
{
    UNICODE_STRING name, value;
    OBJECT_ATTRIBUTES attributes;
    KEY_VALUE_PARTIAL_INFORMATION *data = NULL;
    wchar_t *edited = NULL;
    HANDLE key = NULL;
    ULONG need = 0, got = 0;
    size_t count = 0;
    NTSTATUS status;
    RtlInitUnicodeString(&name,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Session Manager");
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = NtOpenKey(&key, KEY_QUERY_VALUE | KEY_SET_VALUE, &attributes);
    if (status != STATUS_SUCCESS)
        goto done;
    RtlInitUnicodeString(&value, L"BootExecute");
    status = NtQueryValueKey(key, &value, KeyValuePartialInformation, NULL, 0, &need);
    if ((status != STATUS_BUFFER_TOO_SMALL && status != STATUS_BUFFER_OVERFLOW) ||
        need < 16 || need > 1024 * 1024) {
        if (status == STATUS_SUCCESS || status == STATUS_BUFFER_TOO_SMALL ||
            status == STATUS_BUFFER_OVERFLOW)
            status = (NTSTATUS)0xE0000002L;
        goto done;
    }
    data = malloc(need);
    if (!data) {
        status = (NTSTATUS)0xC000009AL;
        goto done;
    }
    status = NtQueryValueKey(key, &value, KeyValuePartialInformation, data, need, &got);
    if (status != STATUS_SUCCESS)
        goto done;
    if (got > need || got < 12 || data->Type != REG_MULTI_SZ ||
        data->DataLength > got - 12 || data->DataLength % sizeof(wchar_t)) {
        status = (NTSTATUS)0xE0000002L;
        goto done;
    }
    edited = bootcfg_edit((const wchar_t *)data->Data,
                          data->DataLength / sizeof(wchar_t), drive, NULL, &count);
    if (!edited) {
        status = (NTSTATUS)0xE0000002L;
        goto done;
    }
    status = NtSetValueKey(key, &value, 0, REG_MULTI_SZ, edited,
                           (ULONG)(count * sizeof(wchar_t)));
    if (status == STATUS_SUCCESS) {
        NTSTATUS flushed = NtFlushKey(key);
        if (flushed != STATUS_SUCCESS)
            nout_status(L"Boot entry flush failed; persistent result prevents a retry:", flushed);
    }
done:
    free(edited);
    free(data);
    if (key) {
        NTSTATUS closed = NtClose(key);
        if (status == STATUS_SUCCESS)
            status = closed;
    }
    return status;
}

/* The result file is the only thing that authorizes a pass. When it cannot
 * be opened or is not a valid record for this drive, no pass can ever run
 * from this entry, and leaving it in BootExecute would fail the same way at
 * every boot (and could not always be cancelled from Windows). Remove only
 * this drive's hsdefrgb entries; the drive comes from a command line that
 * bootcfg_parse() has already accepted. The result file is left exactly as
 * it is so --boot-status can still show what was wrong with it. */
static void drop_unusable_entry(wchar_t drive)
{
    NTSTATUS removed = remove_bootexecute(drive);
    if (removed == STATUS_SUCCESS)
        nout(L"Boot result file unusable; removed this drive's boot entry.");
    else
        nout_status(L"Boot result file unusable; removing the boot entry failed:", removed);
}

void NTAPI NtProcessStartup(PVOID peb)
{
    wchar_t volumePath[] = L"\\??\\C:";
    wchar_t resultPath[] = L"\\??\\C:\\hsdefrag.boot";
    wchar_t drive;
    int dryRun, authorized, full, rc = 5, started = 0;
    PVOID parameters;
    UNICODE_STRING *command;
    HANDLE volume = NULL, result = NULL;
    result_context context;
    FILE_STANDARD_INFORMATION standard;
    LARGE_INTEGER reference;
    DISK_GEOMETRY geometry;
    PARTITION_INFORMATION partition;
    IO_STATUS_BLOCK io;
    NTSTATUS status = (NTSTATUS)0xC000000DL;
    memset(&job, 0, sizeof(job));
    memset(&resultIo, 0, sizeof(resultIo));
    if (!nrt_init())
        goto done;
    nout(L"House Soft Defrag " HSD_WVERSION L" - native volume defragmentation");
    if (!peb || !(parameters = *(PVOID *)((BYTE *)peb + 0x10)))
        goto done;
    command = (UNICODE_STRING *)((BYTE *)parameters + 0x40);
    if (!command->Buffer || command->Length % sizeof(wchar_t) ||
        !bootcfg_parse(command->Buffer, command->Length / sizeof(wchar_t),
                       &drive, &dryRun, &authorized, &full) ||
        dryRun || !authorized || !full)
        goto done;
    volumePath[4] = resultPath[4] = drive;
    result = open_path(resultPath, GENERIC_READ_NT | GENERIC_WRITE_NT,
                        FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH, &status);
    if (!result) {
        drop_unusable_entry(drive);
        goto done;
    }
    context.handle = result;
    resultIo.ctx = &context; resultIo.read = result_read; resultIo.write = result_write;
    resultIo.flush = result_flush; resultIo.length = BOOTLOG_BYTES;
    memset(&io, 0, sizeof(io));
    status = NtQueryInformationFile(result, &io, &standard, sizeof(standard), 5);
    status = ncomplete(result, status, &io);
    if (status != STATUS_SUCCESS) {
        drop_unusable_entry(drive);
        goto done;
    }
    if (io.Information != sizeof(standard) || standard.Directory ||
        standard.DeletePending || standard.EndOfFile.QuadPart != BOOTLOG_BYTES ||
        !bootlog_read(&resultIo, &job) || job.drive != (unsigned)drive) {
        status = (NTSTATUS)0xE0000002L;
        drop_unusable_entry(drive);
        goto done;
    }
    if (job.result != BOOT_PENDING) {
        status = remove_bootexecute(drive);
        rc = status == STATUS_SUCCESS ? 0 : 5;
        goto done;
    }
    job.result = BOOT_RUNNING;
    started = 1;
    if (!save_job(NULL))
        goto done;
    job.phase = BOOT_REMOVE_ENTRY;
    status = remove_bootexecute(drive);
    if (status != STATUS_SUCCESS || !save_job(NULL))
        goto done;
    job.phase = BOOT_OPEN_VOLUME;
    if (!save_job(NULL))
        goto done;
    /* NtFlushBuffersFile requires write access, even after a read-only scan. */
    volume = open_path(volumePath, GENERIC_READ_NT | GENERIC_WRITE_NT, 0, &status);
    if (!volume)
        goto done;
    job.phase = BOOT_GEOMETRY;
    if (!save_job(NULL))
        goto done;
    memset(&io, 0, sizeof(io));
    status = NtDeviceIoControlFile(volume, NULL, NULL, NULL, &io,
        IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &geometry, sizeof(geometry));
    status = ncomplete(volume, status, &io);
    if (status != STATUS_SUCCESS)
        goto done;
    if (io.Information != sizeof(geometry)) {
        status = (NTSTATUS)0xE0000002L;
        goto done;
    }
    memset(&io, 0, sizeof(io));
    status = NtDeviceIoControlFile(volume, NULL, NULL, NULL, &io,
        IOCTL_DISK_GET_PARTITION_INFO, NULL, 0, &partition, sizeof(partition));
    status = ncomplete(volume, status, &io);
    if (status != STATUS_SUCCESS)
        goto done;
    if (io.Information != sizeof(partition) || partition.PartitionLength.QuadPart <= 0) {
        status = (NTSTATUS)0xE0000002L;
        goto done;
    }
    memset(&io, 0, sizeof(io));
    status = NtQueryInformationFile(result, &io, &reference, sizeof(reference), 6);
    status = ncomplete(result, status, &io);
    if (status != STATUS_SUCCESS)
        goto done;
    if (io.Information != sizeof(reference)) {
        status = (NTSTATUS)0xE0000002L;
        goto done;
    }
    job.phase = BOOT_FILES;
    if (!save_job(NULL))
        goto done;
    rc = boot_files_run(volume, drive, (ntfs_u64)reference.QuadPart,
                         geometry.BytesPerSector, (ntfs_u64)partition.PartitionLength.QuadPart,
                         0, &job.files, save_job, NULL);
    status = (NTSTATUS)job.files.ntstatus;
    if (!rc)
        job.phase = BOOT_FINISH;
done:
    if (volume) {
        NTSTATUS closed = NtClose(volume);
        if (closed != STATUS_SUCCESS) { status = closed; rc = 5; }
    }
    if (started) {
        if (!noutput_ok()) rc = 5;
        job.result = rc ? BOOT_FAILED : BOOT_SUCCESS;
        job.ntstatus = (ntfs_u32)(status == STATUS_SUCCESS && rc == 5 ?
                                 (NTSTATUS)0xE0000003L : status);
        if (!save_job(NULL)) rc = 5;
    }
    if (result && NtClose(result) != STATUS_SUCCESS) rc = 5;
    if (rc) {
        if (status == STATUS_SUCCESS)
            nout(L"Boot volume pass incomplete; inspect the saved coverage counters.");
        else
            nout_status(L"Boot volume pass incomplete:", status);
    }
    if (!noutput_ok()) rc = 5;
    NtTerminateProcess(NtCurrentProcess(), rc);
}

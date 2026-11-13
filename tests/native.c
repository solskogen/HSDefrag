#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#define malloc native_malloc
#define realloc native_realloc
#define free native_free
#define memcpy native_memcpy
#define memset native_memset
#define memmove native_memmove
#define memcmp native_memcmp
#define wcslen native_wcslen
#define wcsncmp native_wcsncmp
#define NtDisplayString mock_display
#define NtOpenFile mock_open
#define NtClose mock_close
#define NtReadFile mock_read
#define NtWriteFile mock_write
#define NtDeviceIoControlFile mock_ioctl
#define NtFlushBuffersFile mock_flush
#define NtQueryInformationFile mock_query_file
#define NtTerminateProcess mock_terminate
#define NtOpenKey mock_open_key
#define NtQueryValueKey mock_query_key
#define NtSetValueKey mock_set_key
#define NtFlushKey mock_flush_key
#define boot_files_run fake_files
#define NtProcessStartup boot_start
#include "ntrt.c"
#include "boot_native.c"
#undef malloc
#undef realloc
#undef free
#undef memcpy
#undef memset
#undef memmove
#undef memcmp
#undef wcslen
#undef wcsncmp

static jmp_buf finished;
static BYTE data[BOOTLOG_BYTES];
static int scenario, writes, fileCalls, regWrites, closes, exitCode;
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL native line %d scenario %d: %s\n", __LINE__, scenario, #x); exit(1); \
} } while (0)
#define RESULT ((HANDLE)1)
#define VOLUME ((HANDLE)2)
#define KEY ((HANDLE)3)
#define FAILURE ((NTSTATUS)0xC0000022L)

static NTSTATUS complete(IO_STATUS_BLOCK *io, ULONG bytes)
{
    io->Status = STATUS_SUCCESS; io->Information = bytes;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI mock_display(PUNICODE_STRING string)
{
    CHECK(string->Length <= string->MaximumLength);
    return scenario == 8 ? FAILURE : STATUS_SUCCESS;
}
NTSTATUS NTAPI mock_open(HANDLE *h, ULONG access, POBJECT_ATTRIBUTES oa,
                         PIO_STATUS_BLOCK io, ULONG share, ULONG options)
{
    CHECK(share == 3 && (options & FILE_SYNCHRONOUS_IO_NONALERT));
    CHECK(oa->ObjectName->Buffer[4] == L'C');
    *h = oa->ObjectName->Length == 12 ? VOLUME : RESULT;
    if (*h == VOLUME) {
        CHECK(access == (SYNCHRONIZE | GENERIC_READ_NT | GENERIC_WRITE_NT) &&
              !(options & FILE_WRITE_THROUGH));
        if (scenario == 3) { *h = NULL; return FAILURE; }
    } else {
        CHECK(access == (SYNCHRONIZE | GENERIC_READ_NT | GENERIC_WRITE_NT));
        if (scenario == 2) { *h = NULL; return FAILURE; }
    }
    return complete(io, 0);
}
NTSTATUS NTAPI mock_close(HANDLE h)
{
    CHECK(h == RESULT || h == VOLUME || h == KEY);
    closes++;
    return scenario == 10 && h == VOLUME ? FAILURE : STATUS_SUCCESS;
}
NTSTATUS NTAPI mock_read(HANDLE h, HANDLE event, PVOID apc, PVOID ctx,
                         PIO_STATUS_BLOCK io, PVOID buf, ULONG len,
                         PLARGE_INTEGER off, ULONG *key)
{
    (void)event; (void)apc; (void)ctx; (void)key;
    CHECK(h == RESULT && !off->QuadPart && len == sizeof(data));
    memcpy(buf, data, len);
    return complete(io, len);
}
NTSTATUS NTAPI mock_write(HANDLE h, HANDLE event, PVOID apc, PVOID ctx,
                          PIO_STATUS_BLOCK io, PVOID buf, ULONG len,
                          PLARGE_INTEGER off, ULONG *key)
{
    (void)event; (void)apc; (void)ctx; (void)key;
    CHECK(h == RESULT && !off->QuadPart && len == sizeof(data));
    writes++;
    if (scenario == 7) return FAILURE;
    memcpy(data, buf, len);
    return complete(io, len);
}
NTSTATUS NTAPI mock_flush(HANDLE h, PIO_STATUS_BLOCK io)
{
    CHECK(h == RESULT);
    return complete(io, 0);
}
NTSTATUS NTAPI mock_query_file(HANDLE h, PIO_STATUS_BLOCK io, PVOID out,
                               ULONG len, ULONG cls)
{
    CHECK(h == RESULT);
    memset(out, 0, len);
    if (cls == 5) ((FILE_STANDARD_INFORMATION *)out)->EndOfFile.QuadPart = sizeof(data);
    else { CHECK(cls == 6); ((LARGE_INTEGER *)out)->QuadPart = (1ULL << 48) | 100; }
    return complete(io, scenario == 9 ? len - 1 : len);
}
NTSTATUS NTAPI mock_ioctl(HANDLE h, HANDLE event, PVOID apc, PVOID ctx,
                          PIO_STATUS_BLOCK io, ULONG code, PVOID in,
                          ULONG inLen, PVOID out, ULONG outLen)
{
    (void)event; (void)apc; (void)ctx;
    CHECK(h == VOLUME && !in && !inLen);
    memset(out, 0, outLen);
    if (code == IOCTL_DISK_GET_DRIVE_GEOMETRY)
        ((DISK_GEOMETRY *)out)->BytesPerSector = 512;
    else { CHECK(code == IOCTL_DISK_GET_PARTITION_INFO);
        ((PARTITION_INFORMATION *)out)->PartitionLength.QuadPart = 1024 * 1024; }
    return complete(io, outLen);
}
NTSTATUS NTAPI mock_terminate(HANDLE process, NTSTATUS status)
{
    CHECK(process == NtCurrentProcess());
    exitCode = status;
    longjmp(finished, 1);
}
NTSTATUS NTAPI mock_open_key(HANDLE *key, ULONG access, POBJECT_ATTRIBUTES oa)
{
    (void)access; (void)oa; *key = KEY; return STATUS_SUCCESS;
}
NTSTATUS NTAPI mock_query_key(HANDLE key, PUNICODE_STRING name, int cls,
                              PVOID out, ULONG len, ULONG *need)
{
    static const wchar_t entries[] = L"autocheck autochk *\0hsdefrgb C: /x /f\0";
    KEY_VALUE_PARTIAL_INFORMATION *p = out;
    (void)key; (void)name; (void)cls;
    *need = 12 + sizeof(entries);
    if (!len) return scenario == 4 ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
    CHECK(len >= *need);
    p->Type = REG_MULTI_SZ; p->DataLength = sizeof(entries);
    memcpy(p->Data, entries, sizeof(entries));
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI mock_set_key(HANDLE key, PUNICODE_STRING name, ULONG title,
                            ULONG type, PVOID value, ULONG bytes)
{
    static const wchar_t expected[] = L"autocheck autochk *\0";
    (void)key; (void)name; (void)title;
    CHECK(type == REG_MULTI_SZ && bytes == sizeof(expected) && !memcmp(value, expected, bytes));
    regWrites++; return STATUS_SUCCESS;
}
NTSTATUS NTAPI mock_flush_key(HANDLE key) { CHECK(key == KEY); return STATUS_SUCCESS; }
int fake_files(HANDLE volume, wchar_t drive, ntfs_u64 ref, ULONG sector,
                 ntfs_u64 bytes, int dry, boot_file_stats *stats,
                 int (*progress)(void *), void *ctx)
{
    CHECK(volume == VOLUME && drive == L'C' && sector == 512 &&
          ref == ((1ULL << 48) | 100) && bytes == 1024 * 1024 && !dry && regWrites == 1);
    fileCalls++;
    stats->result = FILES_RUNNING;
    CHECK(progress(ctx));
    stats->files = stats->streams = 10; stats->moved = 1;
    stats->result = scenario == 5 ? FILES_PARTIAL : FILES_COMPLETE;
    stats->errors = scenario == 5;
    stats->ntstatus = scenario == 5 ? (ntfs_u32)FAILURE : 0;
    return scenario == 5 ? 4 : 0;
}
int wmain(void)
{
    BYTE peb[64], parameters[128];
    UNICODE_STRING *cmd = (UNICODE_STRING *)(parameters + 0x40);
    volatile int i;
    for (i = 0; i <= 12; i++) {
        result_context memory = {RESULT};
        bootlog_io log = { &memory, result_read, result_write, result_flush, BOOTLOG_BYTES };
        boot_record record;
        scenario = 0; writes = fileCalls = regWrites = closes = 0;
        memset(data, 0, sizeof(data));
        CHECK(bootlog_create(&log, i == 12 ? 'D' : 'C', 123));
        if (i == 6) {
            CHECK(bootlog_read(&log, &record));
            record.result = BOOT_RUNNING;
            CHECK(bootlog_write(&log, &record));
        }
        if (i == 1) data[12] ^= 1;
        scenario = i; writes = 0;
        memset(peb, 0, sizeof(peb)); memset(parameters, 0, sizeof(parameters));
        *(PVOID *)(peb + 0x10) = parameters;
        RtlInitUnicodeString(cmd, i == 11 ? L"hsdefrgb C: /x" : L"hsdefrgb C: /x /f");
        if (!setjmp(finished)) boot_start(peb);
        CHECK(exitCode == (i == 0 || i == 6 ? 0 : i == 5 ? 4 : 5));
        CHECK(fileCalls == (i == 0 || i == 5 || i == 10));
        if (i == 0 || i == 5 || i == 10) {
            CHECK(bootlog_read(&log, &record));
            CHECK(record.result == (i == 0 ? BOOT_SUCCESS : BOOT_FAILED));
            CHECK(record.files.moved == 1);
        }
        if (i == 1 || i == 2 || i == 9 || i == 11 || i == 12) CHECK(!writes);
        /* An unusable result file (bad checksum, unopenable, wrong size,
         * another drive) cannot authorize a pass: the entry goes, the file
         * stays untouched. A rejected command line touches nothing. */
        if (i == 1 || i == 2 || i == 9 || i == 12) CHECK(regWrites == 1);
        if (i == 11) CHECK(!regWrites);
        if (i == 6) CHECK(!writes && regWrites == 1);
    }
    printf("Native volume-only boot regressions: %u checks.\n", checks);
    return 0;
}

/*
 * ntrt.h - Minimal native (ntdll-only) runtime for the boot-time application.
 *
 * The boot-time volume defragmenter runs from the registry BootExecute list,
 * before the Win32 subsystem exists, so it cannot use KERNEL32. It is a native
 * NT application: its entry point is NtProcessStartup and it may only call
 * ntdll exports. This header declares the small set of NT types, the ntdll
 * functions we use, and the tiny runtime (heap, block ops, console output,
 * file and registry helpers) implemented in ntrt.c.
 *
 * No <windows.h>: pulling it in would declare KERNEL32 APIs and invite
 * accidental use. Everything needed is defined here from scratch.
 */
#ifndef HSDEFRAG_NTRT_H
#define HSDEFRAG_NTRT_H

#include <stddef.h>

#define NTAPI __stdcall

typedef long               NTSTATUS;
typedef unsigned char      BYTE;
typedef unsigned short     USHORT;
typedef unsigned long      ULONG;
typedef long               LONG;
typedef unsigned long      ULONG_PTR;
typedef unsigned char      BOOLEAN;
typedef void              *PVOID;
typedef void              *HANDLE;
typedef wchar_t            WCHAR;
typedef wchar_t           *PWSTR;
typedef const wchar_t     *PCWSTR;

#define NtCurrentProcess() ((HANDLE)(ULONG_PTR)-1)

#define STATUS_SUCCESS            ((NTSTATUS)0x00000000L)
#define STATUS_PENDING            ((NTSTATUS)0x00000103L)
#define STATUS_BUFFER_TOO_SMALL   ((NTSTATUS)0xC0000023L)
#define STATUS_BUFFER_OVERFLOW    ((NTSTATUS)0x80000005L)
#define STATUS_NO_MORE_FILES      ((NTSTATUS)0x80000006L)

#define OBJ_CASE_INSENSITIVE      0x00000040UL

#define SYNCHRONIZE               0x00100000UL
#define GENERIC_READ_NT           0x80000000UL
#define GENERIC_WRITE_NT          0x40000000UL

#define FILE_SHARE_READ           0x0001UL
#define FILE_SHARE_WRITE          0x0002UL
#define FILE_SHARE_DELETE         0x0004UL

#define FILE_NON_DIRECTORY_FILE        0x00000040UL
#define FILE_DIRECTORY_FILE            0x00000001UL
#define FILE_SYNCHRONOUS_IO_NONALERT   0x00000020UL
#define FILE_WRITE_THROUGH              0x00000002UL
#define FILE_NO_INTERMEDIATE_BUFFERING  0x00000008UL

#define IOCTL_DISK_GET_DRIVE_GEOMETRY 0x00070000UL
#define IOCTL_DISK_GET_PARTITION_INFO 0x00074004UL

#define KEY_QUERY_VALUE           0x0001UL
#define KEY_SET_VALUE             0x0002UL

#define REG_MULTI_SZ              7UL
#define KeyValuePartialInformation 2

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef union _LARGE_INTEGER {
    long long QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _IO_STATUS_BLOCK {
    NTSTATUS  Status;
    ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _OBJECT_ATTRIBUTES {
    ULONG           Length;
    HANDLE          RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG           Attributes;
    PVOID           SecurityDescriptor;
    PVOID           SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

#define InitializeObjectAttributes(p, n, a, r, s) \
    do {                                          \
        (p)->Length = sizeof(OBJECT_ATTRIBUTES);  \
        (p)->RootDirectory = (r);                 \
        (p)->ObjectName = (n);                    \
        (p)->Attributes = (a);                    \
        (p)->SecurityDescriptor = (s);            \
        (p)->SecurityQualityOfService = NULL;     \
    } while (0)

typedef struct _KEY_VALUE_PARTIAL_INFORMATION {
    ULONG TitleIndex;
    ULONG Type;
    ULONG DataLength;
    BYTE  Data[1];
} KEY_VALUE_PARTIAL_INFORMATION;

typedef struct {
    LARGE_INTEGER Cylinders;
    ULONG MediaType, TracksPerCylinder, SectorsPerTrack, BytesPerSector;
} DISK_GEOMETRY;

typedef struct {
    LARGE_INTEGER StartingOffset, PartitionLength;
    ULONG HiddenSectors, PartitionNumber;
    BYTE PartitionType, BootIndicator, RecognizedPartition, RewritePartition;
} PARTITION_INFORMATION;

typedef struct {
    LARGE_INTEGER AllocationSize, EndOfFile;
    ULONG NumberOfLinks;
    BOOLEAN DeletePending, Directory;
} FILE_STANDARD_INFORMATION;

typedef struct {
    ULONG NextEntryOffset, FileIndex;
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    LARGE_INTEGER EndOfFile, AllocationSize;
    ULONG FileAttributes, FileNameLength;
    wchar_t FileName[1];
} NATIVE_DIRECTORY_INFORMATION;

typedef struct {
    ULONG NextEntryOffset, StreamNameLength;
    LARGE_INTEGER StreamSize, StreamAllocationSize;
    wchar_t StreamName[1];
} NATIVE_STREAM_INFORMATION;

typedef struct {
    LARGE_INTEGER TotalAllocationUnits, AvailableAllocationUnits;
    ULONG SectorsPerAllocationUnit, BytesPerSector;
} NATIVE_FS_SIZE_INFORMATION;

typedef struct {
    HANDLE FileHandle;
    LARGE_INTEGER StartingVcn, StartingLcn;
    ULONG ClusterCount;
} NATIVE_MOVE_FILE_DATA;

typedef char ntrt_boolean_size[sizeof(BOOLEAN) == 1 ? 1 : -1];
typedef char ntrt_wchar_size[sizeof(wchar_t) == 2 ? 1 : -1];
typedef char ntrt_unicode_size[sizeof(UNICODE_STRING) == 8 ? 1 : -1];
typedef char ntrt_iosb_size[sizeof(IO_STATUS_BLOCK) == 8 ? 1 : -1];
typedef char ntrt_oa_size[sizeof(OBJECT_ATTRIBUTES) == 24 ? 1 : -1];
typedef char ntrt_geometry_size[sizeof(DISK_GEOMETRY) == 24 ? 1 : -1];
typedef char ntrt_partition_size[sizeof(PARTITION_INFORMATION) == 32 ? 1 : -1];
typedef char ntrt_standard_size[sizeof(FILE_STANDARD_INFORMATION) == 24 ? 1 : -1];
typedef char ntrt_directory_name[offsetof(NATIVE_DIRECTORY_INFORMATION, FileName) == 64 ? 1 : -1];
typedef char ntrt_stream_name[offsetof(NATIVE_STREAM_INFORMATION, StreamName) == 24 ? 1 : -1];
typedef char ntrt_fs_size[sizeof(NATIVE_FS_SIZE_INFORMATION) == 24 ? 1 : -1];
typedef char ntrt_move_size[sizeof(NATIVE_MOVE_FILE_DATA) == 32 ? 1 : -1];
typedef char ntrt_move_vcn[offsetof(NATIVE_MOVE_FILE_DATA, StartingVcn) == 8 ? 1 : -1];
typedef char ntrt_move_lcn[offsetof(NATIVE_MOVE_FILE_DATA, StartingLcn) == 16 ? 1 : -1];
typedef char ntrt_move_count[offsetof(NATIVE_MOVE_FILE_DATA, ClusterCount) == 24 ? 1 : -1];

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ----------------------------------------------------------- ntdll imports */

NTSTATUS NTAPI NtDisplayString(PUNICODE_STRING String);
NTSTATUS NTAPI NtTerminateProcess(HANDLE Process, NTSTATUS Status);
void     NTAPI RtlInitUnicodeString(PUNICODE_STRING Dst, PCWSTR Src);
PVOID    NTAPI RtlAllocateHeap(PVOID Heap, ULONG Flags, size_t Size);
PVOID    NTAPI RtlReAllocateHeap(PVOID Heap, ULONG Flags, PVOID Mem, size_t Size);
BOOLEAN  NTAPI RtlFreeHeap(PVOID Heap, ULONG Flags, PVOID Mem);
PVOID    NTAPI RtlCreateHeap(ULONG Flags, PVOID Base, size_t Reserve,
                             size_t Commit, PVOID Lock, PVOID Parameters);

NTSTATUS NTAPI NtOpenFile(HANDLE *Handle, ULONG Access, POBJECT_ATTRIBUTES Oa,
                          PIO_STATUS_BLOCK Iosb, ULONG Share, ULONG Options);
NTSTATUS NTAPI NtReadFile(HANDLE Handle, HANDLE Event, PVOID Apc, PVOID Ctx,
                          PIO_STATUS_BLOCK Iosb, PVOID Buffer, ULONG Length,
                          PLARGE_INTEGER Offset, ULONG *Key);
NTSTATUS NTAPI NtWriteFile(HANDLE Handle, HANDLE Event, PVOID Apc, PVOID Ctx,
                           PIO_STATUS_BLOCK Iosb, PVOID Buffer, ULONG Length,
                           PLARGE_INTEGER Offset, ULONG *Key);
NTSTATUS NTAPI NtFsControlFile(HANDLE Handle, HANDLE Event, PVOID Apc,
                               PVOID Ctx, PIO_STATUS_BLOCK Iosb, ULONG Code,
                               PVOID In, ULONG InLen, PVOID Out, ULONG OutLen);
NTSTATUS NTAPI NtDeviceIoControlFile(HANDLE Handle, HANDLE Event, PVOID Apc,
                                     PVOID Ctx, PIO_STATUS_BLOCK Iosb, ULONG Code,
                                     PVOID In, ULONG InLen, PVOID Out, ULONG OutLen);
NTSTATUS NTAPI NtFlushBuffersFile(HANDLE Handle, PIO_STATUS_BLOCK Iosb);
NTSTATUS NTAPI NtQueryInformationFile(HANDLE Handle, PIO_STATUS_BLOCK Iosb,
                                      PVOID Info, ULONG Length, ULONG Class);
NTSTATUS NTAPI NtQueryDirectoryFile(HANDLE Handle, HANDLE Event, PVOID Apc,
                                     PVOID Ctx, PIO_STATUS_BLOCK Iosb,
                                     PVOID Info, ULONG Length, ULONG Class,
                                     BOOLEAN Single, PUNICODE_STRING Pattern,
                                     BOOLEAN Restart);
NTSTATUS NTAPI NtQueryVolumeInformationFile(HANDLE Handle, PIO_STATUS_BLOCK Iosb,
                                            PVOID Info, ULONG Length, ULONG Class);
NTSTATUS NTAPI NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable,
                                     PLARGE_INTEGER Timeout);
NTSTATUS NTAPI NtDelayExecution(BOOLEAN Alertable, PLARGE_INTEGER Interval);
NTSTATUS NTAPI NtClose(HANDLE Handle);

NTSTATUS NTAPI NtOpenKey(HANDLE *Key, ULONG Access, POBJECT_ATTRIBUTES Oa);
NTSTATUS NTAPI NtQueryValueKey(HANDLE Key, PUNICODE_STRING Name, int Class,
                               PVOID Info, ULONG Length, ULONG *ResultLength);
NTSTATUS NTAPI NtSetValueKey(HANDLE Key, PUNICODE_STRING Name, ULONG TitleIndex,
                             ULONG Type, PVOID Data, ULONG DataSize);
NTSTATUS NTAPI NtFlushKey(HANDLE Key);

/* ------------------------------------------------------------- runtime */

/* Native applications cannot assume a Win32-initialized PEB process heap. */
int nrt_init(void);

/* Block + wide-string helpers (also satisfy the compiler's implicit calls). */
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t wcslen(const wchar_t *s);
int    wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);

/* Heap (used by the shared core via malloc/free). */
void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

/* Write a line (a trailing CR/LF is added) to the boot display. */
void nout(const wchar_t *s);
/* Write a plain ASCII line to the boot display (for the core's logger). */
void nout_ascii(const char *s);
void nout_status(const wchar_t *label, NTSTATUS status);
void nout_count(const wchar_t *label, unsigned long long count);
int noutput_ok(void);
NTSTATUS ncomplete(HANDLE h, NTSTATUS status, IO_STATUS_BLOCK *io);

#endif /* HSDEFRAG_NTRT_H */

/*
 * ntfsctl.h - File-system control requests through the native NtFsControlFile.
 *
 * This is how the NT 4.0 defragmentation interface was originally driven
 * (Russinovich, "Inside Windows NT Disk Defragmenting", 1997). Calling the
 * native API directly yields the exact NTSTATUS, which tells apart results
 * that collapse into the same Win32 error (e.g. STATUS_ALREADY_COMMITTED and
 * STATUS_ACCESS_DENIED are both ERROR_ACCESS_DENIED) and makes failures on real
 * NT 4.0 machines diagnosable. ntdll.dll is resolved at run time, so the binary
 * still imports only KERNEL32.dll.
 */
#ifndef HSDEFRAG_NTFSCTL_H
#define HSDEFRAG_NTFSCTL_H

#include "compat.h"

typedef LONG HSD_NTSTATUS;

#define HSD_NT_SUCCESS(s) ((HSD_NTSTATUS)(s) >= 0)

#define HSD_STATUS_SUCCESS                ((HSD_NTSTATUS)0x00000000L)
#define HSD_STATUS_PENDING                ((HSD_NTSTATUS)0x00000103L)
#define HSD_STATUS_BUFFER_OVERFLOW        ((HSD_NTSTATUS)0x80000005L)
#define HSD_STATUS_NOT_IMPLEMENTED        ((HSD_NTSTATUS)0xC0000002L)
#define HSD_STATUS_INVALID_HANDLE         ((HSD_NTSTATUS)0xC0000008L)
#define HSD_STATUS_INVALID_PARAMETER      ((HSD_NTSTATUS)0xC000000DL)
#define HSD_STATUS_INVALID_DEVICE_REQUEST ((HSD_NTSTATUS)0xC0000010L)
#define HSD_STATUS_END_OF_FILE            ((HSD_NTSTATUS)0xC0000011L)
#define HSD_STATUS_ALREADY_COMMITTED      ((HSD_NTSTATUS)0xC0000021L)
#define HSD_STATUS_ACCESS_DENIED          ((HSD_NTSTATUS)0xC0000022L)
#define HSD_STATUS_BUFFER_TOO_SMALL       ((HSD_NTSTATUS)0xC0000023L)
#define HSD_STATUS_SHARING_VIOLATION      ((HSD_NTSTATUS)0xC0000043L)
#define HSD_STATUS_DISK_FULL              ((HSD_NTSTATUS)0xC000007FL)
#define HSD_STATUS_INSUFFICIENT_RESOURCES ((HSD_NTSTATUS)0xC000009AL)
#define HSD_STATUS_NOT_SUPPORTED          ((HSD_NTSTATUS)0xC00000BBL)

/*
 * Private status (customer bit set, never returned by NT): the file system
 * reported success but re-reading the file's map showed nothing moved. FAT
 * always returns STATUS_SUCCESS from FSCTL_MOVE_FILE, even when it refuses.
 */
#define HSD_STATUS_NOT_MOVED              ((HSD_NTSTATUS)0xE0000001L)

/* Private status: the driver's FSCTL reply is inconsistent (e.g. a stub). */
#define HSD_STATUS_BAD_REPLY              ((HSD_NTSTATUS)0xE0000002L)

/*
 * Send an FSCTL to a file or volume handle and wait for it to finish. Returns
 * the final NTSTATUS; *bytesReturned (optional) receives the I/O status block
 * Information field.
 */
HSD_NTSTATUS hsd_fsctl(HANDLE h, DWORD code, void *in, DWORD inLen, void *out,
                     DWORD outLen, DWORD *bytesReturned);

/* TRUE when the native NtFsControlFile entry point is in use. */
int hsd_fsctl_native(void);

/* Symbolic name of a common NTSTATUS, or NULL if unknown. */
const wchar_t *hsd_status_name(HSD_NTSTATUS st);

/* Format "STATUS_NAME (0xXXXXXXXX)" (or just the hex value) into buf. */
const wchar_t *hsd_status_str(HSD_NTSTATUS st, wchar_t *buf, size_t cap);

/* Print "error: <ctx>: STATUS_NAME (0xXXXXXXXX) - <description>". */
void hsd_status_error(const wchar_t *ctx, HSD_NTSTATUS st);

#endif /* HSDEFRAG_NTFSCTL_H */

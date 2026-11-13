/*
 * ntfsctl.c - File-system control requests through the native NtFsControlFile.
 */
#include "ntfsctl.h"
#include "util.h"

typedef struct {
    HSD_NTSTATUS Status;
    ULONG_PTR   Information;
} HSD_IO_STATUS_BLOCK;

typedef HSD_NTSTATUS(WINAPI *NtFsControlFile_t)(
    HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
    HSD_IO_STATUS_BLOCK *IoStatusBlock, ULONG FsControlCode, PVOID InputBuffer,
    ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength);

typedef ULONG(WINAPI *RtlNtStatusToDosError_t)(HSD_NTSTATUS Status);

static NtFsControlFile_t       pNtFsControlFile;
static RtlNtStatusToDosError_t pRtlNtStatusToDosError;
static int                     g_resolved;

static void resolve_ntdll(void)
{
    HMODULE ntdll;

    if (g_resolved)
        return;
    g_resolved = 1;

    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return;
    /* The void(*)(void) hop silences -Wcast-function-type for FARPROC. */
    pNtFsControlFile = (NtFsControlFile_t)(void (*)(void))GetProcAddress(
        ntdll, "NtFsControlFile");
    pRtlNtStatusToDosError =
        (RtlNtStatusToDosError_t)(void (*)(void))GetProcAddress(
            ntdll, "RtlNtStatusToDosError");
}

int hsd_fsctl_native(void)
{
    resolve_ntdll();
    return pNtFsControlFile != NULL;
}

/* Best-effort reverse mapping, only used when ntdll is unavailable. */
static HSD_NTSTATUS win32_to_status(DWORD err)
{
    switch (err) {
    case ERROR_MORE_DATA:          return HSD_STATUS_BUFFER_OVERFLOW;
    case ERROR_HANDLE_EOF:         return HSD_STATUS_END_OF_FILE;
    case ERROR_INVALID_FUNCTION:   return HSD_STATUS_INVALID_DEVICE_REQUEST;
    case ERROR_INVALID_PARAMETER:  return HSD_STATUS_INVALID_PARAMETER;
    case ERROR_ACCESS_DENIED:      return HSD_STATUS_ACCESS_DENIED;
    case ERROR_INSUFFICIENT_BUFFER:return HSD_STATUS_BUFFER_TOO_SMALL;
    case ERROR_INVALID_HANDLE:     return HSD_STATUS_INVALID_HANDLE;
    case ERROR_NOT_SUPPORTED:      return HSD_STATUS_NOT_SUPPORTED;
    default:
        return (HSD_NTSTATUS)(0xC0070000UL | (err & 0xFFFFUL));
    }
}

HSD_NTSTATUS hsd_fsctl(HANDLE h, DWORD code, void *in, DWORD inLen, void *out,
                     DWORD outLen, DWORD *bytesReturned)
{
    resolve_ntdll();

    if (pNtFsControlFile) {
        HSD_IO_STATUS_BLOCK iosb;
        HSD_NTSTATUS st;

        iosb.Status = HSD_STATUS_SUCCESS;
        iosb.Information = 0;
        st = pNtFsControlFile(h, NULL, NULL, NULL, &iosb, code, in, inLen, out,
                              outLen);
        if (st == HSD_STATUS_PENDING) {
            DWORD wait = WaitForSingleObject(h, INFINITE);
            if (wait != WAIT_OBJECT_0)
                return wait == WAIT_FAILED ? win32_to_status(GetLastError())
                                           : HSD_STATUS_BAD_REPLY;
            st = iosb.Status;
            if (st == HSD_STATUS_PENDING)
                return HSD_STATUS_BAD_REPLY;
        }
        if (out && iosb.Information > outLen)
            return HSD_STATUS_BAD_REPLY;
        if (bytesReturned)
            *bytesReturned = (DWORD)iosb.Information;
        return st;
    } else {
        DWORD n = 0;
        if (DeviceIoControl(h, code, in, inLen, out, outLen, &n, NULL)) {
            if (bytesReturned)
                *bytesReturned = n;
            return HSD_STATUS_SUCCESS;
        }
        if (bytesReturned)
            *bytesReturned = n;
        return win32_to_status(GetLastError());
    }
}

const wchar_t *hsd_status_name(HSD_NTSTATUS st)
{
    switch (st) {
    case HSD_STATUS_SUCCESS:                return L"STATUS_SUCCESS";
    case HSD_STATUS_PENDING:                return L"STATUS_PENDING";
    case HSD_STATUS_BUFFER_OVERFLOW:        return L"STATUS_BUFFER_OVERFLOW";
    case HSD_STATUS_NOT_IMPLEMENTED:        return L"STATUS_NOT_IMPLEMENTED";
    case HSD_STATUS_INVALID_HANDLE:         return L"STATUS_INVALID_HANDLE";
    case HSD_STATUS_INVALID_PARAMETER:      return L"STATUS_INVALID_PARAMETER";
    case HSD_STATUS_INVALID_DEVICE_REQUEST: return L"STATUS_INVALID_DEVICE_REQUEST";
    case HSD_STATUS_END_OF_FILE:            return L"STATUS_END_OF_FILE";
    case HSD_STATUS_ALREADY_COMMITTED:      return L"STATUS_ALREADY_COMMITTED";
    case HSD_STATUS_ACCESS_DENIED:          return L"STATUS_ACCESS_DENIED";
    case HSD_STATUS_BUFFER_TOO_SMALL:       return L"STATUS_BUFFER_TOO_SMALL";
    case HSD_STATUS_SHARING_VIOLATION:      return L"STATUS_SHARING_VIOLATION";
    case HSD_STATUS_DISK_FULL:              return L"STATUS_DISK_FULL";
    case HSD_STATUS_INSUFFICIENT_RESOURCES: return L"STATUS_INSUFFICIENT_RESOURCES";
    case HSD_STATUS_NOT_SUPPORTED:          return L"STATUS_NOT_SUPPORTED";
    case HSD_STATUS_NOT_MOVED:              return L"NOT_MOVED";
    case HSD_STATUS_BAD_REPLY:              return L"BAD_REPLY";
    default:                               return NULL;
    }
}

const wchar_t *hsd_status_str(HSD_NTSTATUS st, wchar_t *buf, size_t cap)
{
    const wchar_t *name = hsd_status_name(st);
    if (name)
        u_snwprintf(buf, cap, L"%ls (0x%08lX)", name, (unsigned long)st);
    else
        u_snwprintf(buf, cap, L"NTSTATUS 0x%08lX", (unsigned long)st);
    return buf;
}

/* Defrag-specific meaning for statuses whose generic text is misleading. */
static const wchar_t *defrag_meaning(HSD_NTSTATUS st)
{
    switch (st) {
    case HSD_STATUS_INVALID_DEVICE_REQUEST:
        return L"the file system driver does not support this request on this "
               L"volume";
    case HSD_STATUS_NOT_IMPLEMENTED:
        return L"the file system driver does not implement this request";
    case HSD_STATUS_ALREADY_COMMITTED:
        return L"target clusters are in use, or were freed but not yet "
               L"checkpointed by NTFS";
    case HSD_STATUS_INVALID_PARAMETER:
        return L"invalid parameter (on NTFS the target may lie in a reserved "
               L"zone, or the file cannot be moved)";
    case HSD_STATUS_END_OF_FILE:
        return L"the file has no allocated clusters";
    case HSD_STATUS_NOT_MOVED:
        return L"the file system reported success but the clusters did not "
               L"move (the file may be in use)";
    case HSD_STATUS_BAD_REPLY:
        return L"the file system driver returned an inconsistent reply";
    default:
        return NULL;
    }
}

void hsd_status_error(const wchar_t *ctx, HSD_NTSTATUS st)
{
    wchar_t code[64];
    const wchar_t *meaning = defrag_meaning(st);
    wchar_t *msg = NULL;
    DWORD n = 0;

    hsd_status_str(st, code, 64);
    if (meaning) {
        uerror(L"%ls: %ls - %ls\n", ctx, code, meaning);
        return;
    }

    resolve_ntdll();
    if (pRtlNtStatusToDosError) {
        DWORD err = (DWORD)pRtlNtStatusToDosError(st);
        n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                               FORMAT_MESSAGE_FROM_SYSTEM |
                               FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, err, 0, (LPWSTR)&msg, 0, NULL);
    }
    if (n && msg) {
        while (n > 0 && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n' ||
                         msg[n - 1] == L'.' || msg[n - 1] == L' '))
            msg[--n] = L'\0';
        uerror(L"%ls: %ls - %ls\n", ctx, code, msg);
        LocalFree(msg);
    } else {
        uerror(L"%ls: %ls\n", ctx, code);
    }
}

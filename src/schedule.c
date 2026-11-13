/*
 * schedule.c - NT boot-time scheduling: --boot-defrag, --boot-status and
 * --boot-cancel.
 *
 * Scheduling writes three things, in this order: a fresh 4-KiB result
 * record (X:\hsdefrag.boot, bootlog.c), the native helper hsdefrgb.exe in
 * System32 (staged as .tmp and renamed into place), and finally the
 * "hsdefrgb X: /x /f" BootExecute entry. The entry goes last so that a
 * failure part-way never leaves Windows running a helper without a valid
 * record; the helper itself refuses to run without one.
 *
 * An existing record is overwritten only when it is a completed job for
 * this drive. Pending, running, failed or unreadable records are kept as
 * they are, because they are the only evidence of what an interrupted pass
 * did. BootExecute is edited through bootcfg.c, which keeps every unrelated
 * entry byte-for-byte. ADVAPI32 is loaded at run time so the image keeps
 * importing KERNEL32 only.
 */
#include "schedule.h"
#include "util.h"
#include "bootcfg.h"
#include "bootlog.h"
#include "filemove.h"

extern const BYTE hsd_native_image[], hsd_native_image_end[];

typedef struct {
    HANDLE h;
    ntfs_u64 length;
} win_io;

static int win_transfer(win_io *c, ntfs_u64 off, void *buf, DWORD len, int write)
{
    LONG high = (LONG)(off >> 32);
    DWORD low, done = 0, error = NO_ERROR;
    int ok;
    if (!len || off > c->length || len > c->length - off ||
        off > 0x7FFFFFFFFFFFFFFFULL) {
        error = ERROR_INVALID_PARAMETER;
        goto done;
    }
    SetLastError(NO_ERROR);
    low = SetFilePointer(c->h, (LONG)(DWORD)off, &high, FILE_BEGIN);
    if (low == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) {
        error = GetLastError();
        goto done;
    }
    ok = write ? WriteFile(c->h, buf, len, &done, NULL) != 0 :
                 ReadFile(c->h, buf, len, &done, NULL) != 0;
    if (!ok)
        error = GetLastError();
    if (!error && (!ok || done != len))
        error = write ? ERROR_WRITE_FAULT : ERROR_HANDLE_EOF;
done:
    if (error) {
        uerror(L"boot-result %ls failed at byte offset %llu, requested %lu bytes\n",
               write ? L"write" : L"read", off, (unsigned long)len);
        uwin32_error(L"I/O error", error);
    }
    SetLastError(error);
    return error == NO_ERROR;
}

static int win_read(void *ctx, ntfs_u64 off, void *buf, ntfs_u32 len)
{
    return win_transfer(ctx, off, buf, len, 0);
}

static int win_write(void *ctx, ntfs_u64 off, const void *buf, ntfs_u32 len)
{
    return win_transfer(ctx, off, (void *)buf, len, 1);
}

static int win_flush(void *ctx)
{
    if (FlushFileBuffers(((win_io *)ctx)->h))
        return 1;
    uwin32_error(L"flushing boot-result file", GetLastError());
    return 0;
}

static void print_ascii(const char *line)
{
    wchar_t w[256];
    size_t i = 0;
    while (line[i] && i + 1 < sizeof(w) / sizeof(w[0])) {
        w[i] = (unsigned char)line[i];
        i++;
    }
    w[i] = 0;
    uinfo(L"  %ls\n", w);
}

typedef struct {
    HMODULE dll;
    LONG (WINAPI *open)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
    LONG (WINAPI *query)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
    LONG (WINAPI *set)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE *, DWORD);
    LONG (WINAPI *flush)(HKEY);
    LONG (WINAPI *close)(HKEY);
} registry_api;

static int edit_bootexecute(wchar_t drive, const wchar_t *add)
{
    registry_api a;
    HKEY key = NULL;
    wchar_t *data = NULL, *edited = NULL;
    DWORD type = 0, bytes = 0, capacity;
    size_t chars;
    LONG error = ERROR_PROC_NOT_FOUND;
    int ok = 0;
    memset(&a, 0, sizeof(a));
    a.dll = LoadLibraryW(L"ADVAPI32.DLL");
    if (!a.dll) {
        uwin32_error(L"loading registry API", GetLastError());
        return 0;
    }
    a.open = (void *)GetProcAddress(a.dll, "RegOpenKeyExW");
    a.query = (void *)GetProcAddress(a.dll, "RegQueryValueExW");
    a.set = (void *)GetProcAddress(a.dll, "RegSetValueExW");
    a.flush = (void *)GetProcAddress(a.dll, "RegFlushKey");
    a.close = (void *)GetProcAddress(a.dll, "RegCloseKey");
    if (!a.open || !a.query || !a.set || !a.flush || !a.close)
        goto done;
    error = a.open(HKEY_LOCAL_MACHINE,
                   L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0,
                   KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (error)
        goto done;
    error = a.query(key, L"BootExecute", NULL, &type, NULL, &bytes);
    if (error)
        goto done;
    if (type != REG_MULTI_SZ || bytes < 4 || bytes % 2 || bytes > 1024 * 1024) {
        error = ERROR_INVALID_DATA;
        goto done;
    }
    capacity = bytes;
    data = malloc(bytes);
    if (!data) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    error = a.query(key, L"BootExecute", NULL, &type, (BYTE *)data, &bytes);
    if (error)
        goto done;
    if (type != REG_MULTI_SZ || bytes > capacity || bytes < 4 || bytes % 2) {
        error = ERROR_INVALID_DATA;
        goto done;
    }
    edited = bootcfg_edit(data, bytes / sizeof(wchar_t), drive, add, &chars);
    if (!edited) {
        error = ERROR_INVALID_DATA;
        goto done;
    }
    error = a.set(key, L"BootExecute", 0, REG_MULTI_SZ, (BYTE *)edited,
                  (DWORD)(chars * sizeof(wchar_t)));
    if (!error)
        error = a.flush(key);
    ok = error == ERROR_SUCCESS;
done:
    if (key) {
        LONG closed = a.close(key);
        if (closed) {
            ok = 0;
            error = closed;
        }
    }
    if (!ok)
        uwin32_error(L"updating BootExecute (scheduling may be incomplete)",
                     (DWORD)error);
    free(data);
    free(edited);
    FreeLibrary(a.dll);
    return ok;
}

static int deploy_native(void)
{
    wchar_t path[MAX_PATH], temp[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    HANDLE h;
    DWORD size = (DWORD)((ULONG_PTR)hsd_native_image_end - (ULONG_PTR)hsd_native_image);
    DWORD written = 0;
    int ok;
    if (!n || n >= MAX_PATH - 17) {
        uerror(L"cannot resolve the System32 helper path\n");
        return 0;
    }
    wcscat(path, L"\\hsdefrgb.exe");
    u_snwprintf(temp, MAX_PATH, L"%ls.tmp", path);
    h = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        uwin32_error(L"creating native helper staging file", GetLastError());
        return 0;
    }
    ok = WriteFile(h, hsd_native_image, size, &written, NULL) != 0;
    if (ok && written != size) {
        SetLastError(ERROR_WRITE_FAULT);
        ok = 0;
    }
    if (ok)
        ok = FlushFileBuffers(h) != 0;
    if (!ok)
        uwin32_error(L"writing native helper", GetLastError());
    if (!CloseHandle(h)) {
        uwin32_error(L"closing native helper", GetLastError());
        ok = 0;
    }
    if (ok && !MoveFileExW(temp, path,
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        uwin32_error(L"installing native helper", GetLastError());
        ok = 0;
    }
    if (!ok && !DeleteFileW(temp))
        uwin32_error(L"removing incomplete helper staging file", GetLastError());
    return ok;
}

int cmd_boot_schedule(VOLUME *v, int dryRun)
{
    wchar_t path[32], entry[40];
    BYTE zero[BOOTLOG_BYTES];
    FILETIME now;
    win_io file;
    bootlog_io io;
    boot_record previous;
    DWORD opened, low, high = 0;
    int rc = 5;
    if (!v->isNtfs) {
        uerror(L"boot-time defragmentation requires NTFS\n");
        return 3;
    }
    if (v->bytesPerSector < 512 || v->bytesPerSector > 4096 ||
        (v->bytesPerSector & (v->bytesPerSector - 1)) ||
        !v->sectorsPerCluster || (v->sectorsPerCluster & (v->sectorsPerCluster - 1)) ||
        v->bytesPerCluster != (ntfs_u64)v->bytesPerSector * v->sectorsPerCluster ||
        !v->totalClusters || v->freeClusters > v->totalClusters ||
        v->totalClusters > 0x7FFFFFFFFFFFFFFFULL / v->bytesPerCluster) {
        uerror(L"invalid volume geometry; scheduling refused\n");
        return 4;
    }
    if (v->bytesPerCluster > 4096) {
        uerror(L"NT4 cannot move file data on volumes with clusters larger than 4 KB\n");
        return 3;
    }
    if (dryRun) {
        uinfo(L"Read-only eligibility check passed: NTFS, %llu-byte clusters.\n"
              L"Files/streams are enumerated and assessed only by the boot helper.\n"
              L"No helper, result file or registry entry was written.\n",
              v->bytesPerCluster);
        return uoutput_ok() ? 0 : 5;
    }
    if (!uoutput_ok())
        return 5;
    u_snwprintf(path, 32, L"%lc:\\hsdefrag.boot", v->drive);
    file.h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    opened = GetLastError();
    if (file.h == INVALID_HANDLE_VALUE) {
        uwin32_error(L"opening/creating boot-result file", opened);
        return 5;
    }
    file.length = BOOTLOG_BYTES;
    memset(&io, 0, sizeof(io));
    io.ctx = &file;
    io.read = win_read;
    io.write = win_write;
    io.flush = win_flush;
    io.length = file.length;
    if (opened == ERROR_ALREADY_EXISTS) {
        SetLastError(NO_ERROR);
        low = GetFileSize(file.h, &high);
        if ((low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) ||
            high || low != BOOTLOG_BYTES ||
            !bootlog_read(&io, &previous) ||
            previous.drive != (unsigned)v->drive ||
            previous.result != BOOT_SUCCESS) {
            uerror(L"Existing boot result is not a valid completed job for this drive.\n"
                   L"Pending, interrupted, failed and unreadable results are retained.\n"
                   L"Inspect --boot-status and cancel/archive explicitly if appropriate.\n");
            if (!CloseHandle(file.h))
                uwin32_error(L"closing protected boot result", GetLastError());
            return 5;
        }
    }
    memset(zero, 0, sizeof(zero));
    GetSystemTimeAsFileTime(&now);
    rc = 5;
    if (win_write(&file, 0, zero, sizeof(zero)) &&
        bootlog_create(&io, (unsigned)v->drive,
                       ((ntfs_u64)now.dwHighDateTime << 32) | now.dwLowDateTime))
        rc = 0;
    if (!CloseHandle(file.h)) {
        uwin32_error(L"closing boot-result file", GetLastError());
        rc = 5;
    }
    if (!rc && (!deploy_native() || !uoutput_ok()))
        rc = 5;
    if (!rc) {
        u_snwprintf(entry, 40, L"hsdefrgb %lc: /x /f", v->drive);
        if (!edit_bootexecute(v->drive, entry))
            rc = 5;
    }
    if (rc) {
        uerror(L"Boot-result file retained at %ls. Scheduling may be incomplete;\n"
               L"inspect/cancel the boot entry before rebooting or rescheduling.\n", path);
    } else {
        uinfo(L"Scheduled one NT4 native NTFS file pass for the next boot.\n"
              L"Directory indices, reserved metadata, busy and special files are excluded.\n"
              L"Saved result: %ls (%u bytes).\n"
              L"After boot, use --boot-status. Cancel with --boot-cancel.\n",
              path, BOOTLOG_BYTES);
    }
    return uoutput_ok() ? rc : 5;
}

int cmd_boot_status(wchar_t drive)
{
    wchar_t path[32];
    win_io file;
    bootlog_io io;
    boot_record record;
    DWORD low, high = 0;
    int rc = 0;
    u_snwprintf(path, 32, L"%lc:\\hsdefrag.boot", drive);
    file.h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file.h == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            uinfo(L"No saved boot result exists at %ls.\n", path);
            return uoutput_ok() ? 0 : 5;
        }
        uwin32_error(L"opening saved boot result", error);
        return 5;
    }
    SetLastError(NO_ERROR);
    low = GetFileSize(file.h, &high);
    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
        uwin32_error(L"reading boot-result file size", GetLastError());
        rc = 5;
        goto done;
    }
    file.length = ((ntfs_u64)high << 32) | low;
    memset(&io, 0, sizeof(io));
    io.ctx = &file;
    io.read = win_read;
    io.length = file.length;
    if (!bootlog_read(&io, &record) || record.drive != (unsigned)drive) {
        uerror(L"invalid/unreadable boot result or drive mismatch; retain the file\n");
        rc = 4;
        goto done;
    }
    uinfo(L"Saved boot result: %ls (job %llu)\n",
          record.result == BOOT_PENDING ? L"pending" :
          record.result == BOOT_RUNNING ? L"interrupted/in progress" :
          record.result == BOOT_SUCCESS ? L"completed" : L"failed", record.jobId);
    print_ascii(bootlog_phase_name(record.phase));
    uinfo(L"  NTSTATUS: 0x%08lX\n", (unsigned long)record.ntstatus);
    print_ascii(bootlog_files_result_name(record.files.result));
    uinfo(L"  Files visited: %llu; data streams examined: %llu\n"
          L"  Fragmented: %llu; made contiguous: %llu; already contiguous/resident: %llu\n"
          L"  In-use: %llu; special: %llu; no space: %llu; errors: %llu\n"
          L"  Directories visited (indices excluded): %llu; reserved/internal: %llu; move refusals: %llu\n"
          L"  File-pass NTSTATUS: 0x%08lX\n",
          record.files.files, record.files.streams, record.files.fragmented,
          record.files.moved, record.files.contiguous, record.files.busy,
          record.files.special, record.files.noSpace, record.files.errors,
          record.files.directories, record.files.metadata, record.files.refused,
          (unsigned long)record.files.ntstatus);
    uinfo(L"  Partially improved (still fragmented): %llu\n", record.files.reduced);
    if (record.files.errorContext[0]) {
        print_ascii(record.files.errorContext);
        if (record.files.controlCode) {
            uinfo(L"  Native control: 0x%08lX; buffer: %lu bytes; returned: %lu bytes\n",
                  (unsigned long)record.files.controlCode,
                  (unsigned long)record.files.controlCapacity,
                  (unsigned long)record.files.controlReturned);
            if (record.files.controlCode == FM_RETRIEVAL) {
                uinfo(L"  Retrieval header: extents=%llu; VCN=%llu; next=%llu; LCN=%llu\n"
                      L"  Standard info: status=0x%08lX; returned=%lu; allocation=%llu; data=%llu\n",
                      record.files.retrieval[0], record.files.retrieval[1],
                      record.files.retrieval[2], record.files.retrieval[3],
                      (unsigned long)record.files.standardStatus,
                      (unsigned long)record.files.standardReturned,
                      record.files.allocationBytes, record.files.dataBytes);
            }
        }
    }
    if (record.result == BOOT_RUNNING || record.result == BOOT_FAILED ||
        (record.result == BOOT_SUCCESS && record.files.result != FILES_COMPLETE))
        rc = 4;
done:
    if (!CloseHandle(file.h)) {
        uwin32_error(L"closing boot-result file", GetLastError());
        rc = 5;
    }
    return uoutput_ok() ? rc : 5;
}

/* Cancelling only edits BootExecute and never touches the drive, so it
 * works even when the drive cannot be reached (removed, not ready, letter
 * gone): otherwise an entry for a drive that has disappeared could never be
 * removed and the helper would run (and fail) at every boot. */
int cmd_boot_cancel(wchar_t drive)
{
    if (!edit_bootexecute(drive, NULL))
        return 5;
    uinfo(L"Removed only hsdefrgb entries for %lc:; unrelated boot commands\n"
          L"were preserved. The helper and saved result were retained.\n", drive);
    return uoutput_ok() ? 0 : 5;
}

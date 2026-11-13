#include "compat.h"
#include "util.h"
#include "bootlog.h"

static UINT system_directory(LPWSTR, UINT);
static HANDLE file_open(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static BOOL file_write(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL file_read(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL file_flush(HANDLE);
static BOOL file_close(HANDLE);
static BOOL file_move(LPCWSTR, LPCWSTR, DWORD);
static DWORD file_seek(HANDLE, LONG, PLONG, DWORD);
static HMODULE registry_library(LPCWSTR);
static FARPROC registry_function(HMODULE, LPCSTR);
static BOOL registry_free(HMODULE);

#define GetSystemDirectoryW system_directory
#define CreateFileW file_open
#define WriteFile file_write
#define ReadFile file_read
#define FlushFileBuffers file_flush
#define CloseHandle file_close
#define MoveFileExW file_move
#define SetFilePointer file_seek
#define LoadLibraryW registry_library
#define GetProcAddress registry_function
#define FreeLibrary registry_free
#include "../src/schedule.c"
#undef GetSystemDirectoryW
#undef CreateFileW
#undef WriteFile
#undef ReadFile
#undef FlushFileBuffers
#undef CloseHandle
#undef MoveFileExW
#undef SetFilePointer
#undef LoadLibraryW
#undef GetProcAddress
#undef FreeLibrary

static wchar_t directory[MAX_PATH], resultPath[MAX_PATH];
static wchar_t helperPath[MAX_PATH], stagingPath[MAX_PATH], regData[512];
static const wchar_t original[] =
    L"autocheck autochk *\0otherhelper C:\0hsdefrgb D: /x /f\0";
static const wchar_t scheduled[] =
    L"autocheck autochk *\0otherhelper C:\0hsdefrgb D: /x /f\0hsdefrgb T: /x /f\0";
static DWORD regBytes;
static HANDLE resultHandle, helperHandle;
static int checks, failures, fault, creates, sets, queries;
enum {
    NORMAL, RESULT_OPEN, RESULT_WRITE, RESULT_READ, RESULT_FLUSH,
    HELPER_WRITE, HELPER_FLUSH, HELPER_CLOSE, INSTALL, SEEK,
    LONG_PATH, REG_TYPE, REG_GROW, REG_ZERO,
    REG_OPEN, REG_SET, REG_FLUSH, REG_CLOSE, REG_LOAD, REG_PROC,
    GONE_PATH, GONE_READY, GONE_DRIVE
};
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    uinfo(L"FAIL schedule line %d fault %d\n", __LINE__, fault); } } while (0)

static UINT system_directory(LPWSTR out, UINT size)
{
    if (fault == LONG_PATH)
        return MAX_PATH;
    CHECK(wcslen(directory) + 1 < size);
    wcscpy(out, directory);
    return (UINT)wcslen(out);
}

static HANDLE file_open(LPCWSTR path, DWORD access, DWORD share,
                         LPSECURITY_ATTRIBUTES security, DWORD disposition,
                         DWORD flags, HANDLE template)
{
    HANDLE h;
    CHECK(memcmp(path, L"\\\\.\\", 4 * sizeof(wchar_t)) != 0);
    if (!wcscmp(path, L"T:\\hsdefrag.boot")) {
        CHECK(disposition == OPEN_ALWAYS || disposition == OPEN_EXISTING);
        path = resultPath;
        if (fault == RESULT_OPEN) {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (fault >= GONE_PATH) {
            SetLastError(fault == GONE_PATH ? ERROR_PATH_NOT_FOUND :
                         fault == GONE_READY ? ERROR_NOT_READY : ERROR_INVALID_DRIVE);
            return INVALID_HANDLE_VALUE;
        }
    } else
        CHECK(!wcscmp(path, stagingPath));
    if (disposition == CREATE_NEW || disposition == OPEN_ALWAYS)
        creates++;
    h = CreateFileW(path, access, share, security, disposition, flags, template);
    if (!wcscmp(path, resultPath))
        resultHandle = h;
    else
        helperHandle = h;
    return h;
}

static BOOL file_write(HANDLE h, LPCVOID data, DWORD bytes, LPDWORD done, LPOVERLAPPED ov)
{
    if ((h == resultHandle && fault == RESULT_WRITE) ||
        (h == helperHandle && fault == HELPER_WRITE))
        bytes--;
    return WriteFile(h, data, bytes, done, ov);
}

static BOOL file_read(HANDLE h, LPVOID data, DWORD bytes, LPDWORD done, LPOVERLAPPED ov)
{
    if (h == resultHandle && fault == RESULT_READ)
        bytes--;
    return ReadFile(h, data, bytes, done, ov);
}

static BOOL file_flush(HANDLE h)
{
    if ((h == resultHandle && fault == RESULT_FLUSH) ||
        (h == helperHandle && fault == HELPER_FLUSH)) {
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return FlushFileBuffers(h);
}

static BOOL file_close(HANDLE h)
{
    int helper = h == helperHandle;
    BOOL ok = CloseHandle(h);
    if (h == helperHandle) helperHandle = INVALID_HANDLE_VALUE;
    if (h == resultHandle) resultHandle = INVALID_HANDLE_VALUE;
    if (helper && fault == HELPER_CLOSE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return ok;
}

static DWORD file_seek(HANDLE h, LONG low, PLONG high, DWORD method)
{
    if (fault == SEEK) {
        SetLastError(ERROR_SEEK);
        return INVALID_SET_FILE_POINTER;
    }
    return SetFilePointer(h, low, high, method);
}

static BOOL file_move(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    CHECK(!wcscmp(from, stagingPath) && !wcscmp(to, helperPath) &&
          flags == (MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    if (fault == INSTALL) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return MoveFileExW(from, to, flags);
}

static LONG WINAPI reg_open(HKEY root, LPCWSTR path, DWORD reserved,
                             REGSAM access, PHKEY key)
{
    CHECK(root == HKEY_LOCAL_MACHINE && !reserved &&
          !wcscmp(path, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager") &&
          access == (KEY_QUERY_VALUE | KEY_SET_VALUE));
    if (fault == REG_OPEN)
        return ERROR_ACCESS_DENIED;
    *key = (HKEY)1;
    return ERROR_SUCCESS;
}

static LONG WINAPI reg_query(HKEY key, LPCWSTR name, LPDWORD reserved,
                              LPDWORD type, LPBYTE data, LPDWORD bytes)
{
    CHECK(key == (HKEY)1 && !wcscmp(name, L"BootExecute") && !reserved);
    queries++;
    *type = fault == REG_TYPE ? REG_SZ : REG_MULTI_SZ;
    if (data) {
        CHECK(*bytes >= regBytes);
        memcpy(data, regData, regBytes);
    }
    *bytes = fault == REG_ZERO ? 0 : regBytes + (fault == REG_GROW && data ? 2 : 0);
    return ERROR_SUCCESS;
}

static LONG WINAPI reg_set(HKEY key, LPCWSTR name, DWORD reserved, DWORD type,
                            const BYTE *data, DWORD bytes)
{
    CHECK(key == (HKEY)1 && !wcscmp(name, L"BootExecute") && !reserved &&
          type == REG_MULTI_SZ && bytes <= sizeof(regData));
    if (fault == REG_SET)
        return ERROR_ACCESS_DENIED;
    memcpy(regData, data, bytes);
    regBytes = bytes;
    sets++;
    return ERROR_SUCCESS;
}
static LONG WINAPI reg_flush(HKEY key)
{
    CHECK(key == (HKEY)1);
    return fault == REG_FLUSH ? ERROR_WRITE_FAULT : ERROR_SUCCESS;
}
static LONG WINAPI reg_close(HKEY key)
{
    CHECK(key == (HKEY)1);
    return fault == REG_CLOSE ? ERROR_INVALID_HANDLE : ERROR_SUCCESS;
}
static HMODULE registry_library(LPCWSTR name)
{
    CHECK(!wcscmp(name, L"ADVAPI32.DLL"));
    if (fault == REG_LOAD) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return NULL;
    }
    return (HMODULE)1;
}
static int equal(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static FARPROC registry_function(HMODULE dll, LPCSTR name)
{
    CHECK(dll == (HMODULE)1);
    if (fault == REG_PROC)
        return NULL;
    if (equal(name, "RegOpenKeyExW")) return (FARPROC)(void (*)(void))reg_open;
    if (equal(name, "RegQueryValueExW")) return (FARPROC)(void (*)(void))reg_query;
    if (equal(name, "RegSetValueExW")) return (FARPROC)(void (*)(void))reg_set;
    if (equal(name, "RegFlushKey")) return (FARPROC)(void (*)(void))reg_flush;
    if (equal(name, "RegCloseKey")) return (FARPROC)(void (*)(void))reg_close;
    CHECK(0);
    return NULL;
}
static BOOL registry_free(HMODULE dll) { CHECK(dll == (HMODULE)1); return TRUE; }

static void remove_if_present(const wchar_t *path)
{
    CHECK(DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND);
}
static void reset(void)
{
    fault = NORMAL;
    remove_if_present(resultPath);
    remove_if_present(stagingPath);
    remove_if_present(helperPath);
    memcpy(regData, original, sizeof(original));
    regBytes = sizeof(original);
    creates = sets = queries = 0;
    resultHandle = helperHandle = INVALID_HANDLE_VALUE;
}
static void touch(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(CloseHandle(h));
}
static void read_record(boot_record *record, int write)
{
    win_io file;
    bootlog_io io;
    file.h = CreateFileW(resultPath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file.h != INVALID_HANDLE_VALUE && GetFileSize(file.h, NULL) == BOOTLOG_BYTES);
    file.length = BOOTLOG_BYTES;
    io.ctx = &file; io.read = win_read; io.write = win_write;
    io.flush = win_flush; io.length = file.length;
    CHECK(write ? bootlog_write(&io, record) : bootlog_read(&io, record));
    CHECK(CloseHandle(file.h));
}

int wmain(void)
{
    wchar_t temp[MAX_PATH];
    VOLUME v;
    boot_record record;
    int i;
    CHECK(GetTempPathW(MAX_PATH, temp) > 0);
    CHECK(GetTempFileNameW(temp, L"hsd", 0, directory));
    CHECK(DeleteFileW(directory) && CreateDirectoryW(directory, NULL));
    u_snwprintf(resultPath, MAX_PATH, L"%ls\\result", directory);
    u_snwprintf(helperPath, MAX_PATH, L"%ls\\hsdefrgb.exe", directory);
    u_snwprintf(stagingPath, MAX_PATH, L"%ls.tmp", helperPath);
    memset(&v, 0, sizeof(v));
    v.drive = L'T'; v.isNtfs = 1; v.bytesPerSector = 512;
    v.sectorsPerCluster = 1; v.bytesPerCluster = 512;
    v.totalClusters = 4096; v.freeClusters = 1024;

    reset();
    CHECK(cmd_boot_schedule(&v, 1) == 0 && !creates && !sets);
    CHECK(cmd_boot_schedule(&v, 0) == 0 && creates == 2 && sets == 1);
    CHECK(regBytes == sizeof(scheduled) && !memcmp(regData, scheduled, regBytes));
    CHECK(GetFileAttributesW(helperPath) != INVALID_FILE_ATTRIBUTES);
    read_record(&record, 0);
    CHECK(record.drive == 'T' && record.jobId && record.result == BOOT_PENDING);
    CHECK(cmd_boot_status('T') == 0);
    CHECK(cmd_boot_schedule(&v, 0) == 5 && sets == 1);
    CHECK(cmd_boot_cancel('T') == 0);
    CHECK(regBytes == sizeof(original) && !memcmp(regData, original, regBytes));
    CHECK(GetFileAttributesW(resultPath) != INVALID_FILE_ATTRIBUTES);
    record.result = BOOT_RUNNING;
    read_record(&record, 1);
    CHECK(cmd_boot_status('T') == 4);
    CHECK(cmd_boot_schedule(&v, 0) == 5);
    read_record(&record, 0);
    CHECK(record.result == BOOT_RUNNING);
    record.result = BOOT_FAILED;
    read_record(&record, 1);
    CHECK(cmd_boot_status('T') == 4);
    CHECK(cmd_boot_schedule(&v, 0) == 5);
    read_record(&record, 0);
    CHECK(record.result == BOOT_FAILED);
    record.result = BOOT_SUCCESS; record.phase = BOOT_FINISH;
    record.files.result = FILES_COMPLETE;
    read_record(&record, 1);
    CHECK(cmd_boot_status('T') == 0);
    {
        BYTE saved[BOOTLOG_BYTES];
        HANDLE h;
        DWORD got;
        CHECK(cmd_boot_schedule(&v, 1) == 0);
        read_record(&record, 0);
        CHECK(record.result == BOOT_SUCCESS);
        CHECK(cmd_boot_schedule(&v, 0) == 0);
        CHECK(regBytes == sizeof(scheduled) && !memcmp(regData, scheduled, regBytes));
        read_record(&record, 0);
        CHECK(record.result == BOOT_PENDING && record.phase == BOOT_PREPARE &&
              !record.files.streams && !record.files.moved);
        CHECK(cmd_boot_schedule(&v, 0) == 5);
        /* A corrupt result must remain byte-for-byte intact. */
        h = CreateFileW(resultPath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(h != INVALID_HANDLE_VALUE);
        CHECK(ReadFile(h, saved, sizeof(saved), &got, NULL) && got == sizeof(saved));
        saved[12] ^= 1;
        CHECK(SetFilePointer(h, 0, NULL, FILE_BEGIN) == 0);
        CHECK(WriteFile(h, saved, sizeof(saved), &got, NULL) && got == sizeof(saved));
        CHECK(CloseHandle(h));
        CHECK(cmd_boot_schedule(&v, 0) == 5);
        h = CreateFileW(resultPath, GENERIC_READ, 0, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        {
            BYTE check[BOOTLOG_BYTES];
            CHECK(ReadFile(h, check, sizeof(check), &got, NULL) &&
                  got == sizeof(check) && !memcmp(saved, check, sizeof(check)));
        }
        CHECK(CloseHandle(h));
    }
    record.result = BOOT_SUCCESS; record.phase = BOOT_FINISH;
    record.files.result = FILES_COMPLETE;
    record.drive = 'D';
    read_record(&record, 1);
    CHECK(cmd_boot_status('T') == 4);
    CHECK(cmd_boot_schedule(&v, 0) == 5);
    reset();
    CHECK(cmd_boot_status('T') == 0);
    touch(resultPath);
    CHECK(cmd_boot_status('T') == 4);
    CHECK(cmd_boot_schedule(&v, 0) == 5);

    reset();
    v.isNtfs = 0;
    CHECK(cmd_boot_schedule(&v, 0) == 3);
    v.isNtfs = 1; v.bytesPerCluster = 8192; v.sectorsPerCluster = 16;
    CHECK(cmd_boot_schedule(&v, 0) == 3);
    v.bytesPerCluster = 513; v.sectorsPerCluster = 1;
    CHECK(cmd_boot_schedule(&v, 0) == 4);
    v.bytesPerCluster = 512;

    for (i = RESULT_OPEN; i <= REG_PROC; i++) {
        reset();
        fault = i;
        CHECK(cmd_boot_schedule(&v, 0) == 5);
        CHECK(sets == (i == REG_FLUSH || i == REG_CLOSE ? 1 : 0));
        if (i != RESULT_OPEN)
            CHECK(GetFileAttributesW(resultPath) != INVALID_FILE_ATTRIBUTES);
        CHECK(GetFileAttributesW(stagingPath) == INVALID_FILE_ATTRIBUTES);
    }
    /* A drive that can no longer be reached must not pin its boot entry:
     * cancelling only edits BootExecute, so it removes the entry without
     * touching the drive; scheduling still refuses because it has to write
     * the result file there. */
    for (i = GONE_PATH; i <= GONE_DRIVE; i++) {
        reset();
        CHECK(cmd_boot_schedule(&v, 0) == 0 && sets == 1);
        fault = i;
        CHECK(cmd_boot_schedule(&v, 0) == 5 && sets == 1);
        CHECK(cmd_boot_cancel('T') == 0 && sets == 2 && creates == 2);
        CHECK(regBytes == sizeof(original) && !memcmp(regData, original, regBytes));
    }
    reset();
    CHECK(RemoveDirectoryW(directory));
    uinfo(L"Standalone scheduling/status/cancellation regressions: %d checks, %d failures.\n",
          checks, failures);
    return failures != 0;
}

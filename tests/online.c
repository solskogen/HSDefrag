#include "compat.h"
#include "util.h"
#include "ntfsctl.h"
#include "volume.h"

static HSD_NTSTATUS test_fsctl(HANDLE, DWORD, void *, DWORD, void *, DWORD, DWORD *);
static HANDLE test_first(LPCWSTR, LPWIN32_FIND_DATAW);
static BOOL test_next(HANDLE, LPWIN32_FIND_DATAW);
static BOOL test_find_close(HANDLE);
static DWORD test_attributes(LPCWSTR);
static HANDLE test_open(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static BOOL test_close(HANDLE);
static void *test_alloc(size_t);
static void *test_realloc(void *, size_t);
static LPVOID WINAPI test_valloc(LPVOID, SIZE_T, DWORD, DWORD);
static int test_read_raw(VOLUME *, unsigned long long, void *, DWORD);
static BOOL test_device(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static FARPROC test_query_proc(HMODULE, LPCSTR);

#define hsd_fsctl test_fsctl
#define FindFirstFileW test_first
#define FindNextFileW test_next
#define FindClose test_find_close
#define GetFileAttributesW test_attributes
#define CreateFileW test_open
#define CloseHandle test_close
#define malloc test_alloc
#include "filemove.c"
#define realloc test_realloc
#include "volume.c"
#undef realloc
#define GetProcAddress test_query_proc
#include "retrieval.c"
#undef GetProcAddress
#include "fileenum.c"
#include "defrag.c"
#define volume_read_raw test_read_raw
#define DeviceIoControl test_device
#define VirtualAlloc test_valloc
#include "mft.c"
#undef volume_read_raw
#undef DeviceIoControl
#undef VirtualAlloc
#include "analyze.c"
#undef hsd_fsctl
#undef FindFirstFileW
#undef FindNextFileW
#undef FindClose
#undef GetFileAttributesW
#undef CreateFileW
#undef CloseHandle
#undef malloc

static int checks, failures, bitmapMode, retrievalMode, rpCalls, moves, moved;
static int enumMode, handles, depthLimit, longest, failAlloc = -1, named;
static int rawReads, failRealloc, failVAlloc, vallocs;
static DWORD fileAttributes, openError;
static HSD_NTSTATUS mapError, moveError;
static long long movedLcn;
static wchar_t fileName[32] = L"file";

#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    uinfo(L"FAIL online line %d\n", __LINE__); } } while (0)

static HSD_NTSTATUS WINAPI test_standard(HANDLE file, retrieval_io *io,
                                        void *out, ULONG size, ULONG cls)
{
    retrieval_standard *s = out;
    CHECK(file == (HANDLE)123 && size == sizeof(*s) && cls == 5);
    memset(s, 0, size);
    s->allocation.QuadPart = retrievalMode == 6 ? 0 :
                            retrievalMode == 7 ? 296 :
                            retrievalMode == 8 ? 512 : 8192;
    s->data.QuadPart = retrievalMode == 7 ? 290 : s->allocation.QuadPart;
    if (retrievalMode == 10) s->allocation.QuadPart = -1;
    if (retrievalMode == 11) s->directory = 1;
    if (retrievalMode == 12) s->data.QuadPart++;
    io->status = HSD_STATUS_SUCCESS;
    io->information = retrievalMode == 13 ? size - 1 : size;
    return retrievalMode == 14 ? HSD_STATUS_ACCESS_DENIED : HSD_STATUS_SUCCESS;
}

static FARPROC test_query_proc(HMODULE module, LPCSTR name)
{
    CHECK(module && !memcmp(name, "NtQueryInformationFile", sizeof("NtQueryInformationFile")));
    return retrievalMode == 15 ? NULL : (FARPROC)(void (*)(void))test_standard;
}

static void *test_alloc(size_t n)
{
    if (failAlloc == 0)
        return NULL;
    if (failAlloc > 0)
        failAlloc--;
    return malloc(n);
}

static void *test_realloc(void *p, size_t n)
{
    if (failRealloc)
        return NULL;
    return realloc(p, n);
}

static LPVOID WINAPI test_valloc(LPVOID p, SIZE_T n, DWORD type, DWORD protect)
{
    vallocs++;
    if (failVAlloc) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return VirtualAlloc(p, n, type, protect);
}

static int test_read_raw(VOLUME *v, unsigned long long offset, void *buf,
                         DWORD len)
{
    CHECK(v && !(offset % 512) && !(len % 512) && !((ULONG_PTR)buf & 4095));
    memset(buf, 0, len);
    rawReads++;
    return 1;
}

static BOOL test_device(HANDLE handle, DWORD code, LPVOID in, DWORD inBytes,
                         LPVOID out, DWORD outBytes, LPDWORD got, LPOVERLAPPED ov)
{
    PARTITION_INFORMATION *partition = out;
    (void)handle;
    CHECK(code == IOCTL_DISK_GET_PARTITION_INFO && !in && !inBytes && !ov &&
          outBytes == sizeof(*partition));
    memset(out, 0, outBytes);
    partition->PartitionLength.QuadPart = 8192 * 512;
    *got = outBytes;
    return TRUE;
}

static HSD_NTSTATUS test_fsctl(HANDLE h, DWORD code, void *in, DWORD inLen,
                              void *out, DWORD outLen, DWORD *got)
{
    (void)h; (void)inLen;
    if (got)
        *got = 0;
    if (code == FSCTL_GET_VOLUME_BITMAP) {
        VOLUME_BITMAP_BUFFER *b = out;
        DWORD hdr = FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer);
        CHECK(outLen >= hdr + 1024);
        b->StartingLcn.QuadPart = bitmapMode == 4 ? -1 : 0;
        b->BitmapSize.QuadPart = bitmapMode == 5 ? -1 : 8192;
        memset(b->Buffer, 0, 1024);
        b->Buffer[0] = 0xFF;
        if (got)
            *got = bitmapMode == 1 ? hdr : bitmapMode == 2 ? hdr + 1 : hdr + 1024;
        return HSD_STATUS_SUCCESS;
    }
    if (code == FSCTL_GET_RETRIEVAL_POINTERS) {
        RETRIEVAL_POINTERS_BUFFER *r = out;
        DWORD hdr = FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents);
        long long requested = ((STARTING_VCN_INPUT_BUFFER *)in)->StartingVcn.QuadPart;
        rpCalls++;
        if (mapError)
            return mapError;
        if (retrievalMode >= 16 && retrievalMode <= 18) {
            /* NT 4.0 overflow reply at VCN 0 without a length: a full
             * buffer of increasing extents (16), one slot left unwritten
             * (17), or a count short of the buffer (18). */
            unsigned n = (outLen - hdr) / 16, k;
            if (requested) {
                CHECK(retrievalMode == 16 && requested == (long long)n);
                r->ExtentCount = 1;
                r->StartingVcn.QuadPart = requested;
                r->Extents[0].NextVcn.QuadPart = requested + 6;
                r->Extents[0].Lcn.QuadPart = 1000 + requested;
                if (got)
                    *got = hdr + 16;
                return HSD_STATUS_SUCCESS;
            }
            r->ExtentCount = retrievalMode == 18 ? n - 1 : n;
            r->StartingVcn.QuadPart = 0;
            for (k = 0; k < n - (retrievalMode == 16 ? 0 : 1); k++) {
                r->Extents[k].NextVcn.QuadPart = k + 1;
                r->Extents[k].Lcn.QuadPart = 1000 + k;
            }
            return HSD_STATUS_BUFFER_OVERFLOW;
        }
        if (retrievalMode >= 6 && retrievalMode != 9) {
            memset(out, 0, outLen);
            return HSD_STATUS_SUCCESS;
        }
        r->ExtentCount = moved ? 1 : 2;
        r->StartingVcn.QuadPart = 0;
        r->Extents[0].NextVcn.QuadPart = moved ? 16 : 8;
        r->Extents[0].Lcn.QuadPart = moved ? movedLcn : 100;
        r->Extents[1].NextVcn.QuadPart = 16;
        r->Extents[1].Lcn.QuadPart = 200;
        if (retrievalMode == 3)
            r->Extents[1].Lcn.QuadPart = -2;
        if (retrievalMode == 4 || retrievalMode == 5) {
            r->ExtentCount = 1;
            r->StartingVcn.QuadPart = requested ? 5 : 0;
            r->Extents[0].NextVcn.QuadPart = requested && retrievalMode == 4 ? 15 : 10;
            r->Extents[0].Lcn.QuadPart = requested ? 105 : 100;
        }
        if (got)
            *got = retrievalMode == 9 ? 0 : retrievalMode == 1 ? hdr :
                   retrievalMode == 2 ? hdr + 16 : hdr + 16 * r->ExtentCount;
        return (retrievalMode == 4 || retrievalMode == 5) && !requested
                   ? HSD_STATUS_BUFFER_OVERFLOW : HSD_STATUS_SUCCESS;
    }
    if (code == FSCTL_MOVE_FILE) {
        MOVE_FILE_DATA *m = in;
        moves++;
        if (moveError)
            return moveError;
        if (enumMode != 6) {
            moved = 1;
            movedLcn = m->StartingLcn.QuadPart - m->StartingVcn.QuadPart;
        }
        return HSD_STATUS_SUCCESS;
    }
    return HSD_STATUS_INVALID_DEVICE_REQUEST;
}

typedef struct { int depth, next; } finder;

static void find_data(finder *f, WIN32_FIND_DATAW *fd)
{
    memset(fd, 0, sizeof(*fd));
    fd->nFileSizeLow = 1;
    if (enumMode == 5 && f->depth < depthLimit) {
        wcscpy(fd->cFileName, L"d");
        fd->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    } else {
        wcscpy(fd->cFileName, fileName);
        fd->dwFileAttributes = fileAttributes;
    }
}

static HANDLE test_first(LPCWSTR path, LPWIN32_FIND_DATAW fd)
{
    finder *f;
    size_t i, len = wcslen(path);
    CHECK(!memcmp(path, L"\\\\?\\T:\\", 7 * sizeof(wchar_t)));
    if ((int)len > longest)
        longest = (int)len;
    if (enumMode == 1 || enumMode == 2) {
        SetLastError(enumMode == 1 ? ERROR_PATH_NOT_FOUND : ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    f = malloc(sizeof(*f));
    f->depth = f->next = 0;
    for (i = 7; i + 1 < len; i++)
        if (path[i] == L'd' && path[i + 1] == L'\\')
            f->depth++;
    handles++;
    find_data(f, fd);
    return f;
}

static BOOL test_next(HANDLE h, LPWIN32_FIND_DATAW fd)
{
    (void)h; (void)fd;
    SetLastError(enumMode == 3 ? ERROR_ACCESS_DENIED : ERROR_NO_MORE_FILES);
    return FALSE;
}

static BOOL test_find_close(HANDLE h)
{
    handles--;
    free(h);
    return TRUE;
}

static DWORD test_attributes(LPCWSTR path)
{
    (void)path;
    return enumMode == 1 ? INVALID_FILE_ATTRIBUTES : FILE_ATTRIBUTE_DIRECTORY;
}

static HANDLE test_open(LPCWSTR path, DWORD access, DWORD share,
                         LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
                         HANDLE template)
{
    size_t i;
    (void)access; (void)share; (void)sa; (void)disposition; (void)flags; (void)template;
    for (i = 0; path[i]; i++)
        if (path[i] == L'$')
            named++;
    if (openError) {
        SetLastError(openError);
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)123;
}

static BOOL test_close(HANDLE h) { (void)h; return TRUE; }

static void init(VOLUME *v)
{
    memset(v, 0, sizeof(*v));
    v->drive = L'T'; wcscpy(v->root, L"T:\\"); wcscpy(v->fsName, L"FAT");
    v->hVol = (HANDLE)456;
    v->bytesPerSector = 512; v->bytesPerCluster = 4096;
    v->totalClusters = 8192; v->isFat = 1; v->moveAlign = 1;
    bitmapMode = retrievalMode = enumMode = moved = moves = rpCalls = 0;
    fileAttributes = openError = 0;
    wcscpy(fileName, L"file");
    mapError = moveError = 0;
    failAlloc = -1;
}

static int visit(const wchar_t *path, const WIN32_FIND_DATAW *fd, int dir, void *ctx)
{
    (void)path; (void)fd; (void)dir;
    (*(int *)ctx)++;
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    VOLUME v;
    EXTMAP m;
    ENUM_RESULT scan;
    HSD_NTSTATUS status;
    int i, entries = 0;
    (void)argc; (void)argv;
    g_osMajor = 4; g_osMinor = 0; g_verbose = 0;
    init(&v);
    extmap_init(&m);
    for (i = 1; i <= 5; i++) {
        bitmapMode = i;
        if (i == 3)
            continue;
        CHECK(volume_load_bitmap(&v) == HSD_STATUS_BAD_REPLY && !v.haveBitmap);
    }
    bitmapMode = 0;
    /* GetDiskFreeSpace can report fewer clusters than the volume has (disk
     * quotas on Windows 2000 and later): the bitmap is still accepted. */
    v.totalClusters = 1024;
    CHECK(volume_load_bitmap(&v) == HSD_STATUS_SUCCESS && v.bitmapClusters == 8192);
    v.totalClusters = 8192;
    CHECK(volume_load_bitmap(&v) == HSD_STATUS_SUCCESS && v.bitmapClusters == 8192);
    CHECK(!volume_cluster_used(&v, 8191));
    volume_mark(&v, ~0ULL, ~0ULL, 1);
    CHECK(!volume_cluster_used(&v, 8191));
    volume_mark(&v, 8191, ~0ULL, 1);
    CHECK(volume_cluster_used(&v, 8191));
    volume_mark(&v, 8191, ~0ULL, 0);
    CHECK(!volume_cluster_used(&v, 8191));
    for (i = 1; i <= 3; i++) {
        retrievalMode = i;
        CHECK(extmap_get(&v, (HANDLE)123, &m) == HSD_STATUS_BAD_REPLY && m.n == 0);
    }
    retrievalMode = 4; rpCalls = 0;
    CHECK(extmap_get(&v, (HANDLE)123, &m) == HSD_STATUS_SUCCESS);
    CHECK(m.n == 2 && m.ext[1].vcn == 10 && m.ext[1].lcn == 110 &&
          m.ext[1].count == 5 && m.realClusters == 15 && extmap_fragments(&m) == 1);
    retrievalMode = 5;
    CHECK(extmap_get(&v, (HANDLE)123, &m) == HSD_STATUS_BAD_REPLY && m.n == 0);
    retrievalMode = 0;
    CHECK(extmap_get(&v, (HANDLE)123, &m) == HSD_STATUS_SUCCESS);
    v.isNtfs = 1; v.bytesPerCluster = 512; v.mftRecordSize = 1024;
    for (i = 6; i <= 15; i++) {
        retrievalMode = i;
        status = extmap_get(&v, (HANDLE)123, &m);
        if (i <= 8)
            CHECK(status == HSD_STATUS_SUCCESS && !m.n);
        else if (i == 9)
            CHECK(status == HSD_STATUS_SUCCESS && m.n == 2 && m.realClusters == 16);
        else
            CHECK(status == (i == 14 ? HSD_STATUS_ACCESS_DENIED :
                             i == 15 ? HSD_STATUS_NOT_SUPPORTED : HSD_STATUS_BAD_REPLY) && !m.n);
    }
    retrievalMode = 16;
    status = extmap_get(&v, (HANDLE)123, &m);
    CHECK(status == HSD_STATUS_SUCCESS && m.realClusters == 1030 &&
          extmap_fragments(&m) == 1);
    for (i = 17; i <= 18; i++) {
        retrievalMode = i;
        CHECK(extmap_get(&v, (HANDLE)123, &m) == HSD_STATUS_BAD_REPLY && !m.n);
    }
    retrievalMode = 15;
    v.isNtfs = 0;
    CHECK(extmap_get(&v, (HANDLE)123, &m) == HSD_STATUS_BAD_REPLY);
    retrievalMode = 0;
    CHECK(extmap_get(&v, (HANDLE)123, &m) == HSD_STATUS_SUCCESS);
    moveError = HSD_STATUS_NOT_SUPPORTED;
    CHECK(defrag_relocate(&v, (HANDLE)123, &m, &status) == RELOC_UNSUPPORTED);
    CHECK(status == HSD_STATUS_NOT_SUPPORTED && moves == 1);
    moveError = 0;
    CHECK(defrag_relocate(&v, (HANDLE)123, &m, &status) == RELOC_MOVED);
    CHECK(extmap_fragments(&m) == 1);
    enumMode = 1;
    CHECK(cmd_analyze(&v, ANALYZE_NO_META) == 4);
    CHECK(cmd_defrag(&v) == 4);
    enumMode = 2;
    scan = enum_tree(v.root, visit, &entries);
    CHECK(!scan.errors && !scan.entries && !scan.aborted);
    enumMode = 3;
    scan = enum_tree(v.root, visit, &entries);
    CHECK(scan.errors == 1 && scan.entries == 1 && !handles);
    enumMode = 5; depthLimit = 400;
    scan = enum_tree(v.root, visit, &entries);
    CHECK(!scan.errors && scan.entries == 401 && longest > 260 && !handles);
    failAlloc = 0;
    scan = enum_tree(v.root, visit, &entries);
    CHECK(scan.errors && scan.aborted && !handles);
    failAlloc = -1;
    enumMode = 0; moved = 0; moves = 0;
    mapError = HSD_STATUS_INVALID_PARAMETER;
    CHECK(cmd_analyze(&v, ANALYZE_NO_META) == 4);
    CHECK(cmd_defrag(&v) == 4);
    mapError = 0; moveError = HSD_STATUS_NOT_SUPPORTED;
    CHECK(cmd_defrag(&v) == 3);
    v.movesUnsupported = 1; moves = 0;
    CHECK(cmd_defrag(&v) == 3 && !moves);
    v.movesUnsupported = 0; moveError = 0; fileAttributes = FILE_ATTRIBUTE_COMPRESSED;
    CHECK(cmd_defrag(&v) == 0 && !moves);
    /* Encrypted, reparse and device streams are as special as compressed. */
    fileAttributes = 0x4000; /* FILE_ATTRIBUTE_ENCRYPTED */
    CHECK(cmd_defrag(&v) == 0 && !moves);
    fileAttributes = 0x0400; /* FILE_ATTRIBUTE_REPARSE_POINT */
    CHECK(cmd_defrag(&v) == 0 && !moves);
    fileAttributes = 0; enumMode = 6;
    CHECK(cmd_defrag(&v) == 4 && moves > 0);
    enumMode = 0; openError = ERROR_SHARING_VIOLATION;
    CHECK(defrag_open(L"T:\\pagefile.sys") == INVALID_HANDLE_VALUE);
    v.isNtfs = 1;
    openError = 0; moves = 0; enumMode = 6; wcscpy(fileName, L"$MFT");
    CHECK(cmd_defrag(&v) == 0 && !moves);
    enumMode = 0; wcscpy(fileName, L"file");
    rawReads = 0;
    CHECK(mft_report(&v) == 4 && rawReads > 0 && !named);

    /* -a -m: the caller prints the $MFT report, so analysis skips it; a
     * runtime failure (5) in the report is passed through, not lowered. */
    vallocs = 0;
    CHECK(cmd_analyze(&v, ANALYZE_MFT_ELSEWHERE) == 0 && !vallocs);
    CHECK(cmd_analyze(&v, 0) == 4 && vallocs == 1);
    failVAlloc = 1;
    CHECK(cmd_analyze(&v, 0) == 5);
    failVAlloc = 0;

    /* Ctrl+C stops the walk before the next file: nothing is moved, the
     * result is incomplete (4), and the $MFT report is not started. */
    CHECK(on_ctrl(CTRL_C_EVENT) && enum_interrupted());
    scan = enum_tree(v.root, visit, &entries);
    CHECK(scan.interrupted && scan.aborted && !scan.entries && !handles);
    moves = 0; moved = 0; vallocs = 0;
    CHECK(cmd_defrag(&v) == 4 && !moves);
    CHECK(cmd_analyze(&v, 0) == 4 && !vallocs);
    g_interrupt = 0;
    CHECK(!on_ctrl(CTRL_CLOSE_EVENT) && !enum_interrupted());

    /* A refused target that cannot be recorded stops all moving (5): a
     * later bitmap refresh would offer the same place again. */
    moves = 0; moved = 0; moveError = HSD_STATUS_INVALID_PARAMETER;
    free(v.excl); v.excl = NULL; v.nExcl = v.capExcl = 0;
    failRealloc = 1;
    CHECK(cmd_defrag(&v) == 5 && moves == 1 && v.exclLost);
    failRealloc = 0; moveError = 0; moves = 0;
    CHECK(cmd_defrag(&v) == 5 && !moves);
    v.exclLost = 0;
    extmap_free(&m);
    volume_close(&v);
    uinfo(L"Online regressions: %d checks, %d failures.\n", checks, failures);
    return failures ? 1 : 0;
}

/*
 * bootfiles.c - boot-time file pass of the native helper (hsdefrgb.exe)
 *
 * Runs from BootExecute before Win32 starts, on NTDLL only. It walks the
 * mounted NTFS volume with NtQueryDirectoryFile, one entry at a time and
 * without recursion (explicit heap frames, depth bound, cycle check), and
 * hands every fragmented data stream to the shared engine in filemove.c.
 * Data moves only through FSCTL_MOVE_FILE on the mounted filesystem: no
 * raw metadata writes, no locks or dismounts, no MFT relocation.
 *
 * Left alone: NTFS metafiles and the first 16 MFT records, this tool's own
 * result file, directories' index streams, and compressed, sparse,
 * encrypted, reparse and device streams. Files that are in use are counted
 * and skipped. The MFT zone reported by the filesystem is kept out of the
 * cached bitmap so no target lands in it.
 *
 * Every reply is validated; a bad reply, an I/O failure or a display
 * failure stops the pass (exit 5). Ranges the driver refused as move
 * targets are remembered for the whole pass and marked again on every
 * bitmap re-read; if that table cannot grow, moves stop and the result is
 * incomplete (exit 4). Moves use a 16-cluster alignment, which NT4 needs
 * for ordinary file data with clusters of up to 4 KiB.
 */
#include "bootfiles.h"
#include "filemove.h"

#define BF_BAD_REPLY ((NTSTATUS)0xE0000002L)
#define BF_OUTPUT_ERROR ((NTSTATUS)0xE0000003L)
#define BF_SHARING ((NTSTATUS)0xC0000043L)
#define BF_SPECIAL FM_SPECIAL_ATTRS /* shared with the online pass */
#define BF_DIRECTORY 0x00000010UL
#define BF_NTFS_DATA 0x00090064UL
#define BF_COMPRESSION 0x0009003CUL
#define BF_MAX_DEPTH 512U
/* Target ranges the driver refused. Each refusal is remembered on its own:
 * only the range that was actually refused is given up. The table starts
 * with BF_EXCL_INITIAL entries and doubles on the heap; BF_EXCL_LIMIT only
 * keeps the byte count far from overflow. */
#define BF_EXCL_INITIAL 64
#define BF_EXCL_LIMIT 0x100000

typedef struct bf_frame {
    struct bf_frame *parent;
    HANDLE handle;
    ntfs_u64 id;
    unsigned depth;
    int first;
} bf_frame;

typedef struct {
    ntfs_u64 start, count;
} bf_range;

typedef struct {
    HANDLE volume;
    BYTE *bitmap;
    ntfs_u64 maximum, clusters, resultReference;
    ntfs_u64 zoneStart, zoneCount;
    bf_range *excl;
    int nExcl, capExcl, exclNoted;
    unsigned chunk, bytesPerCluster, bytesPerRecord, sinceProgress;
    /* noMoves: a refused range could not be remembered, so no new move
     * target may be chosen for the rest of the pass. */
    int dryRun, stopped, noMoves;
    unsigned controlCode, controlCapacity, controlReturned;
    ntfs_u64 retrieval[4];
    ntfs_u64 allocationBytes, dataBytes;
    unsigned standardStatus, standardReturned;
    EXTMAP map;
    boot_file_stats *stats;
    int (*progress)(void *);
    void *progressContext;
} bf_context;

static void bf_error(bf_context *c, const wchar_t *label, NTSTATUS status)
{
    c->stats->errors++;
    if (!c->stats->ntstatus) {
        unsigned i = 0;
        c->stats->ntstatus = (ntfs_u32)status;
        while (label[i] && i + 1 < sizeof(c->stats->errorContext)) {
            c->stats->errorContext[i] = label[i] < 128 ? (char)label[i] : '?';
            i++;
        }
        c->stats->errorContext[i] = 0;
        c->stats->controlCode = c->controlCode;
        c->stats->controlCapacity = c->controlCapacity;
        c->stats->controlReturned = c->controlReturned;
        memcpy(c->stats->retrieval, c->retrieval, sizeof(c->retrieval));
        c->stats->allocationBytes = c->allocationBytes;
        c->stats->dataBytes = c->dataBytes;
        c->stats->standardStatus = c->standardStatus;
        c->stats->standardReturned = c->standardReturned;
    }
    nout_status(label, status);
    if (status == BF_BAD_REPLY || status == (NTSTATUS)FM_NOMEM ||
        status == (NTSTATUS)0xC0000185L || status == (NTSTATUS)0xC00000A3L ||
        status == (NTSTATUS)0xC000000EL || status == (NTSTATUS)0xC0000032L ||
        status == (NTSTATUS)0xC0000102L)
        c->stopped = 5;
}

static int bf_close(bf_context *c, HANDLE handle)
{
    NTSTATUS status = NtClose(handle);
    if (status == STATUS_SUCCESS)
        return 1;
    bf_error(c, L"Cannot close a file-pass handle:", status);
    c->stopped = 5;
    return 0;
}

static int bf_progress(bf_context *c)
{
    c->sinceProgress = 0;
    if (!noutput_ok() || (c->progress && !c->progress(c->progressContext))) {
        bf_error(c, L"File-pass diagnostics failed:", BF_OUTPUT_ERROR);
        c->stopped = 5;
        return 0;
    }
    return 1;
}

static fm_status bf_control(void *ctx, void *handle, unsigned code, void *in,
                             unsigned inBytes, void *out, unsigned outBytes,
                             unsigned *returned)
{
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    bf_context *c = ctx;
    memset(&io, 0, sizeof(io));
    if (code == FM_RETRIEVAL) {
        memset(out, 0xFF, outBytes);
        memset(out, 0, outBytes < 16 ? outBytes : 16);
    }
    status = NtFsControlFile(handle, NULL, NULL, NULL, &io, code, in, inBytes, out, outBytes);
    status = ncomplete(handle, status, &io);
    c->controlCode = code;
    c->controlCapacity = outBytes;
    c->controlReturned = io.Information;
    memset(c->retrieval, 0, sizeof(c->retrieval));
    c->allocationBytes = c->dataBytes = 0;
    c->standardStatus = c->standardReturned = 0;
    if (code == FM_RETRIEVAL && outBytes >= 32 && io.Information <= outBytes) {
        unsigned count;
        memcpy(&count, out, sizeof(count));
        c->retrieval[0] = count;
        memcpy(c->retrieval + 1, (BYTE *)out + 8, 24);
    }
    if (code == FM_RETRIEVAL && status == STATUS_SUCCESS && !io.Information &&
        inBytes == sizeof(long long) && !*(const long long *)in) {
        FILE_STANDARD_INFORMATION standard;
        IO_STATUS_BLOCK query;
        NTSTATUS queried;
        memset(&standard, 0, sizeof(standard));
        memset(&query, 0, sizeof(query));
        queried = NtQueryInformationFile(handle, &query, &standard, sizeof(standard), 5);
        queried = ncomplete(handle, queried, &query);
        c->standardStatus = (unsigned)queried;
        c->standardReturned = query.Information;
        c->allocationBytes = (ntfs_u64)standard.AllocationSize.QuadPart;
        c->dataBytes = (ntfs_u64)standard.EndOfFile.QuadPart;
        if (queried != STATUS_SUCCESS)
            return (fm_status)queried;
        if (query.Information != sizeof(standard) || standard.Directory ||
            standard.AllocationSize.QuadPart < 0 || standard.EndOfFile.QuadPart < 0)
            return FM_BAD_REPLY;
        if (!standard.AllocationSize.QuadPart)
            return c->retrieval[0] ? FM_BAD_REPLY : FM_EOF;
        /* A resident stream rounded to one cluster and an actual single-cluster
         * stream are both inherently contiguous. Neither needs a move or LCN. */
        if (!c->retrieval[0] && !c->retrieval[1] && c->bytesPerCluster &&
            c->allocationBytes == c->bytesPerCluster &&
            c->dataBytes <= c->allocationBytes)
            return FM_EOF;
        /* NT4 reports resident attribute storage rounded to eight bytes, not
         * necessarily zero. Nonresident allocation must use whole clusters. */
        if (!c->retrieval[0] && !c->retrieval[1] && c->bytesPerCluster &&
            c->allocationBytes % c->bytesPerCluster &&
            c->allocationBytes < (c->bytesPerRecord ? c->bytesPerRecord : c->bytesPerCluster) &&
            c->dataBytes <= c->allocationBytes &&
            c->allocationBytes == ((c->dataBytes + 7) & ~7ULL))
            return FM_EOF;
        /* Some legacy replies omit Information even when the buffer is filled.
         * Infer a length only for a complete initial map whose final VCN agrees
         * with independently queried allocation. Shared parsing still validates
         * every extent and the caller checks physical volume bounds. */
        if (outBytes >= 32 && c->bytesPerCluster &&
            !(c->allocationBytes % c->bytesPerCluster) &&
            c->dataBytes <= c->allocationBytes &&
            c->retrieval[0] && c->retrieval[0] <= (outBytes - 16) / 16 &&
            !c->retrieval[1]) {
            ntfs_u64 last;
            unsigned bytes = 16 + (unsigned)c->retrieval[0] * 16;
            memcpy(&last, (BYTE *)out + bytes - 16, sizeof(last));
            if (last == c->allocationBytes / c->bytesPerCluster) {
                if (returned)
                    *returned = bytes;
                return FM_SUCCESS;
            }
        }
        return FM_BAD_REPLY;
    }
    if (returned)
        *returned = io.Information;
    return (fm_status)status;
}

static HANDLE bf_open(bf_context *c, HANDLE parent, wchar_t *name, unsigned chars,
                        int directory, NTSTATUS *status)
{
    UNICODE_STRING string;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    HANDLE handle = NULL;
    string.Buffer = name;
    string.Length = (USHORT)(chars * sizeof(wchar_t));
    string.MaximumLength = string.Length;
    InitializeObjectAttributes(&attributes, &string, parent ? 0 : OBJ_CASE_INSENSITIVE, parent, NULL);
    memset(&io, 0, sizeof(io));
    *status = NtOpenFile(&handle, SYNCHRONIZE | GENERIC_READ_NT, &attributes, &io,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         FILE_SYNCHRONOUS_IO_NONALERT |
                         (directory ? FILE_DIRECTORY_FILE :
                          FILE_NON_DIRECTORY_FILE | FILE_NO_INTERMEDIATE_BUFFERING));
    *status = ncomplete(handle, *status, &io);
    if (*status == STATUS_SUCCESS && !handle)
        *status = BF_BAD_REPLY;
    if (*status != STATUS_SUCCESS && handle) {
        bf_close(c, handle);
        handle = NULL;
    }
    return handle;
}

static int bf_equal(const wchar_t *name, const wchar_t *expected)
{
    while (*name && *expected) {
        wchar_t a = *name++, b = *expected++;
        if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
        if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
        if (a != b)
            return 0;
    }
    return !*name && !*expected;
}

static int bf_reserved(const wchar_t *name)
{
    static const wchar_t *const reserved[] = {
        L"$MFT", L"$MFTMirr", L"$LogFile", L"$Volume", L"$AttrDef",
        L"$Bitmap", L"$Boot", L"$BadClus", L"$Secure", L"$UpCase",
        L"$Extend", L"$Quota", L"$ObjId", L"$Reparse", L"$UsnJrnl",
        L"hsdefrag.boot"
    };
    unsigned i;
    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
        if (bf_equal(name, reserved[i]))
            return 1;
    return 0;
}

static fm_status bf_refresh(void *ctx)
{
    bf_context *c = ctx;
    BYTE *bitmap = NULL;
    ntfs_u64 clusters = 0;
    fm_status status = fm_get_bitmap(bf_control, c, c->volume, c->chunk,
                                     c->maximum, &bitmap, &clusters);
    int i;
    if (status != FM_SUCCESS)
        return status;
    if (c->clusters && c->clusters != clusters) {
        free(bitmap);
        return FM_BAD_REPLY;
    }
    free(c->bitmap);
    c->bitmap = bitmap; c->clusters = clusters;
    fm_mark(bitmap, clusters, c->zoneStart, c->zoneCount, 1);
    for (i = 0; i < c->nExcl; i++)
        fm_mark(bitmap, clusters, c->excl[i].start, c->excl[i].count, 1);
    return FM_SUCCESS;
}

static int bf_find(void *ctx, ntfs_u64 need, ntfs_u64 minimum,
                    ntfs_u64 *target, ntfs_u64 *available)
{
    bf_context *c = ctx;
    if (c->noMoves)
        return 0;
    return fm_find_best(c->bitmap, c->clusters, need, minimum, target, available);
}

static void bf_mark(void *ctx, ntfs_u64 start, ntfs_u64 count)
{
    bf_context *c = ctx;
    fm_mark(c->bitmap, c->clusters, start, count, 1);
}

/* The clusters a move vacated are free again. */
static void bf_unmark(void *ctx, ntfs_u64 start, ntfs_u64 count)
{
    bf_context *c = ctx;
    fm_mark(c->bitmap, c->clusters, start, count, 0);
}

/* Make room for one more refused range. */
static int bf_excl_grow(bf_context *c)
{
    bf_range *next;
    int capacity;

    if (c->nExcl < c->capExcl)
        return 1;
    if (c->capExcl >= BF_EXCL_LIMIT)
        return 0;
    capacity = c->capExcl ? c->capExcl * 2 : BF_EXCL_INITIAL;
    next = realloc(c->excl, (size_t)capacity * sizeof(*next));
    if (!next)
        return 0;
    c->excl = next;
    c->capExcl = capacity;
    return 1;
}

/*
 * The refused range is marked used in the current bitmap and remembered, so
 * that every later re-read of the volume bitmap marks it again. If it cannot
 * be remembered, merging it into a neighbour could give up a huge span of
 * good free space, and forgetting it would offer it again after the next
 * re-read. Instead no new move target is chosen for the rest of the pass;
 * the pass reports an error, so the result is incomplete (exit 4).
 */
static void bf_exclude(void *ctx, ntfs_u64 start, ntfs_u64 count)
{
    bf_context *c = ctx;
    bf_mark(c, start, count);
    if (bf_excl_grow(c)) {
        c->excl[c->nExcl].start = start;
        c->excl[c->nExcl].count = count;
        c->nExcl++;
    } else if (!c->noMoves) {
        int stopped = c->stopped;

        c->noMoves = 1;
        bf_error(c, L"Cannot remember a refused move target; no further moves this pass:",
                 (NTSTATUS)FM_NOMEM);
        /* Not a reason to abandon the pass: only moves stop. */
        c->stopped = stopped;
    }
    if (!c->exclNoted) {
        c->exclNoted = 1;
        nout(L"A move target area refused by the driver has been excluded.");
    }
}

static fm_status bf_map(void *ctx, void *handle, EXTMAP *map)
{
    bf_context *c = ctx;
    fm_status status = fm_get_extents(bf_control, c, handle, map);
    int i;
    if (status != FM_SUCCESS)
        return status;
    for (i = 0; i < map->n; i++)
        if (map->ext[i].lcn >= 0 &&
            ((ntfs_u64)map->ext[i].lcn >= c->clusters ||
             (ntfs_u64)map->ext[i].count > c->clusters - (ntfs_u64)map->ext[i].lcn))
            return FM_BAD_REPLY;
    return FM_SUCCESS;
}

static fm_status bf_move(void *ctx, void *handle, long long vcn, ntfs_u64 lcn,
                          unsigned count)
{
    bf_context *c = ctx;
    NATIVE_MOVE_FILE_DATA move;
    memset(&move, 0, sizeof(move));
    move.FileHandle = handle; move.StartingVcn.QuadPart = vcn;
    move.StartingLcn.QuadPart = (long long)lcn; move.ClusterCount = count;
    return bf_control(c, c->volume, FM_MOVE, &move, sizeof(move), NULL, 0, NULL);
}

static void bf_pause(void *ctx, unsigned milliseconds)
{
    bf_context *c = ctx;
    LARGE_INTEGER delay;
    NTSTATUS status;
    delay.QuadPart = -(long long)milliseconds * 10000;
    status = NtDelayExecution(0, &delay);
    if (status != STATUS_SUCCESS) {
        bf_error(c, L"Cannot wait for the NTFS checkpoint:", status);
        c->stopped = 5;
    }
}

static int bf_alive(void *ctx)
{
    bf_context *c = ctx;
    return !c->stopped && noutput_ok();
}

static void bf_stream(bf_context *c, HANDLE handle, const wchar_t *name, ULONG attributes)
{
    USHORT compression = 0;
    unsigned got = 0;
    fm_status status;
    fm_volume volume;
    reloc_result result;
    c->stats->streams++;
    if (attributes & BF_SPECIAL) {
        c->stats->special++;
        return;
    }
    status = bf_control(c, handle, BF_COMPRESSION, NULL, 0, &compression, sizeof(compression), &got);
    if (status != FM_SUCCESS || got != sizeof(compression)) {
        bf_error(c, L"Cannot determine stream compression:", status == FM_SUCCESS ? BF_BAD_REPLY : (NTSTATUS)status);
        return;
    }
    if (compression) {
        c->stats->special++;
        return;
    }
    status = bf_map(c, handle, &c->map);
    if (status != FM_SUCCESS) {
        bf_error(c, L"Cannot map a data stream:", (NTSTATUS)status);
        if (status == FM_INVALID_REQUEST || status == FM_NOT_SUPPORTED || status == FM_NOT_IMPLEMENTED)
            c->stopped = 3;
        return;
    }
    if (c->map.hasVirtual) {
        c->stats->special++;
        return;
    }
    if (extmap_fragments(&c->map) <= 1) {
        c->stats->contiguous++;
        return;
    }
    c->stats->fragmented++;
    if (c->dryRun || c->noMoves)
        return;
    nout(L"Defragmenting data stream:");
    nout(name);
    if (!noutput_ok()) {
        bf_error(c, L"Boot display failed before a move:", BF_OUTPUT_ERROR);
        c->stopped = 5;
        return;
    }
    memset(&volume, 0, sizeof(volume));
    volume.ctx = c; volume.bytesPerCluster = c->bytesPerCluster;
    volume.moveAlign = 16; volume.clusters = c->clusters; volume.haveBitmap = 1;
    volume.find = bf_find; volume.mark = bf_mark; volume.unmark = bf_unmark;
    volume.exclude = bf_exclude;
    volume.refresh = bf_refresh; volume.map = bf_map; volume.move = bf_move;
    volume.pause = bf_pause; volume.alive = bf_alive;
    result = fm_relocate(&volume, handle, &c->map, &status);
    if (result == RELOC_MOVED)
        c->stats->moved++;
    else if (result == RELOC_PARTIAL) {
        /* An improvement, not a refusal: it is counted on its own so that
         * the no-space counter keeps counting the streams that did not fit. */
        c->stats->reduced++;
        c->stats->partial++;
        nout(L"Fragmentation reduced; no full-size free run (stream still fragmented).");
    }
    else if (result == RELOC_SKIPPED)
        c->stats->special++;
    else if (result == RELOC_NO_SPACE) {
        c->stats->noSpace++;
        nout(L"No sufficiently large free run for this stream.");
    } else {
        c->stats->refused++;
        bf_error(c, L"Data-stream relocation refused/unverified:", (NTSTATUS)status);
        if (result == RELOC_UNSUPPORTED)
            c->stopped = 3;
    }
}

/* 1: data, 2: non-data metadata, 0: malformed. */
static int bf_stream_kind(const wchar_t *name, unsigned chars)
{
    unsigned i, separator;
    if (chars < 4 || chars > 289 || name[0] != L':')
        return 0;
    for (separator = 1; separator < chars && name[separator] != L':'; separator++)
        if (!name[separator] || name[separator] == L'/' || name[separator] == L'\\')
            return 0;
    if (separator > 256 || chars - separator < 3 || chars - separator > 33 ||
        name[separator + 1] != L'$')
        return 0;
    for (i = separator + 2; i < chars; i++) {
        wchar_t ch = name[i];
        if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
        if ((ch < L'A' || ch > L'Z') && (ch < L'0' || ch > L'9') && ch != L'_')
            return 0;
    }
    if (chars - separator == 6) {
        static const wchar_t data[] = L"$DATA";
        for (i = 0; i < 5; i++) {
            wchar_t ch = name[separator + 1 + i];
            if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
            if (ch != data[i])
                return 2;
        }
        return 1;
    }
    return 2;
}

static int bf_file(bf_context *c, HANDLE parent, HANDLE handle, wchar_t *name,
                   ULONG attributes, int directory, ntfs_u64 *referenceOut)
{
    IO_STATUS_BLOCK io;
    LARGE_INTEGER reference;
    BYTE *streams = NULL;
    unsigned capacity = 4096, offset = 0, nameChars = (unsigned)wcslen(name);
    NTSTATUS status;
    if (referenceOut)
        *referenceOut = 0;
    c->controlCode = c->controlCapacity = c->controlReturned = 0;
    memset(c->retrieval, 0, sizeof(c->retrieval));
    c->allocationBytes = c->dataBytes = 0;
    c->standardStatus = c->standardReturned = 0;
    memset(&io, 0, sizeof(io));
    status = NtQueryInformationFile(handle, &io, &reference, sizeof(reference), 6);
    status = ncomplete(handle, status, &io);
    if (status != STATUS_SUCCESS || io.Information != sizeof(reference)) {
        bf_error(c, L"Cannot verify file identity:", status == STATUS_SUCCESS ? BF_BAD_REPLY : status);
        return 0;
    }
    if (referenceOut)
        *referenceOut = (ntfs_u64)reference.QuadPart;
    if ((!directory && ((ntfs_u64)reference.QuadPart & 0xFFFFFFFFFFFFULL) < 16) ||
        (ntfs_u64)reference.QuadPart == c->resultReference) {
        c->stats->metadata++;
        return 1;
    }
    for (;;) {
        BYTE *next = realloc(streams, capacity);
        if (!next) {
            bf_error(c, L"Cannot allocate data-stream enumeration:", (NTSTATUS)FM_NOMEM);
            goto done;
        }
        streams = next;
        memset(&io, 0, sizeof(io));
        status = NtQueryInformationFile(handle, &io, streams, capacity, 22);
        status = ncomplete(handle, status, &io);
        if (status != STATUS_BUFFER_OVERFLOW && status != STATUS_BUFFER_TOO_SMALL)
            break;
        if (capacity == 1024 * 1024) {
            bf_error(c, L"Data-stream list exceeds the supported 1 MiB bound:", (NTSTATUS)FM_NOMEM);
            goto done;
        }
        capacity *= 2;
    }
    if (status != STATUS_SUCCESS) {
        bf_error(c, L"Cannot enumerate all data streams:", status);
        if (!directory && !c->stopped)
            bf_stream(c, handle, name, attributes);
        goto done;
    }
    if (io.Information > capacity || (!directory && !io.Information)) {
        bf_error(c, L"Invalid data-stream enumeration:", BF_BAD_REPLY);
        goto done;
    }
    while (offset < io.Information && !c->stopped) {
        NATIVE_STREAM_INFORMATION *entry = (NATIVE_STREAM_INFORMATION *)(streams + offset);
        unsigned remaining = io.Information - offset, chars, span;
        int kind;
        wchar_t relative[518];
        HANDLE stream;
        if (remaining < 24 || !entry->StreamNameLength || entry->StreamNameLength % 2 ||
            entry->StreamNameLength > remaining - 24 || entry->StreamSize.QuadPart < 0 ||
            entry->StreamAllocationSize.QuadPart < 0) {
            bf_error(c, L"Invalid data-stream record:", BF_BAD_REPLY);
            break;
        }
        chars = entry->StreamNameLength / 2;
        span = entry->NextEntryOffset;
        kind = bf_stream_kind(entry->StreamName, chars);
        if ((span && ((span & 7) || span < 24 + entry->StreamNameLength ||
                      span > remaining || remaining - span < 24)) ||
            (!span && remaining > ((24 + entry->StreamNameLength + 7) & ~7U)) ||
            !kind || (kind == 1 && nameChars + chars >= 518)) {
            bf_error(c, L"Invalid data-stream name/offset:", BF_BAD_REPLY);
            break;
        }
        if (kind == 2)
            c->stats->metadata++;
        else if (chars == 7) {
            if (!directory)
                bf_stream(c, handle, name, attributes);
            else
                c->stats->metadata++;
        } else {
            memcpy(relative, name, nameChars * sizeof(wchar_t));
            memcpy(relative + nameChars, entry->StreamName, entry->StreamNameLength);
            relative[nameChars + chars] = 0;
            stream = bf_open(c, parent, relative, nameChars + chars, 0, &status);
            if (stream) {
                bf_stream(c, stream, relative, attributes);
                bf_close(c, stream);
            } else if (status == BF_SHARING)
                c->stats->busy++;
            else
                bf_error(c, L"Cannot open a named data stream:", status);
        }
        if (!span)
            break;
        offset += span;
    }
done:
    free(streams);
    return 1;
}

static int bf_geometry(bf_context *c, ULONG sector, ntfs_u64 bytes)
{
    NATIVE_FS_SIZE_INFORMATION size;
    struct { ULONG attributes; LONG maximum; ULONG bytes; wchar_t name[16]; } fs;
    struct {
        LARGE_INTEGER serial, sectors, clusters, free, reserved;
        ULONG sector, cluster, record, clustersPerRecord;
        LARGE_INTEGER mftBytes, mft, mirror, zoneStart, zoneEnd;
    } data;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    unsigned got;
    if (sector < 512 || sector > 4096 || (sector & (sector - 1))) {
        bf_error(c, L"Invalid file-pass sector geometry:", BF_BAD_REPLY);
        return 0;
    }
    memset(&io, 0, sizeof(io));
    memset(&fs, 0, sizeof(fs));
    status = NtQueryVolumeInformationFile(c->volume, &io, &fs, sizeof(fs), 5);
    status = ncomplete(c->volume, status, &io);
    if (status != STATUS_SUCCESS || io.Information < 20 || io.Information > sizeof(fs) ||
        fs.bytes != 8 || fs.maximum <= 0 || fs.maximum > 255 ||
        wcsncmp(fs.name, L"NTFS", 4)) {
        bf_error(c, L"Cannot verify the NTFS filesystem:", status == STATUS_SUCCESS ? BF_BAD_REPLY : status);
        return 0;
    }
    memset(&io, 0, sizeof(io));
    status = NtQueryVolumeInformationFile(c->volume, &io, &size, sizeof(size), 3);
    status = ncomplete(c->volume, status, &io);
    if (status != STATUS_SUCCESS || io.Information != sizeof(size) ||
        size.TotalAllocationUnits.QuadPart <= 0 || size.AvailableAllocationUnits.QuadPart < 0 ||
        size.AvailableAllocationUnits.QuadPart > size.TotalAllocationUnits.QuadPart ||
        size.BytesPerSector != sector || !size.SectorsPerAllocationUnit ||
        size.SectorsPerAllocationUnit > 128 ||
        (size.SectorsPerAllocationUnit & (size.SectorsPerAllocationUnit - 1))) {
        bf_error(c, L"Invalid filesystem allocation geometry:", status == STATUS_SUCCESS ? BF_BAD_REPLY : status);
        return 0;
    }
    c->bytesPerCluster = size.BytesPerSector * size.SectorsPerAllocationUnit;
    c->maximum = bytes / c->bytesPerCluster;
    if ((ntfs_u64)size.TotalAllocationUnits.QuadPart > c->maximum) {
        bf_error(c, L"Filesystem exceeds the partition bounds:", BF_BAD_REPLY);
        return 0;
    }
    if (c->bytesPerCluster > 4096) {
        nout(L"NT4 cannot move ordinary file data with clusters larger than 4 KiB.");
        c->stats->ntstatus = (ntfs_u32)FM_NOT_SUPPORTED;
        c->stopped = 3;
        return 0;
    }
    memset(&data, 0, sizeof(data));
    status = (NTSTATUS)bf_control(c, c->volume, BF_NTFS_DATA, NULL, 0, &data, sizeof(data), &got);
    if (status == STATUS_SUCCESS && got == sizeof(data) &&
        data.cluster == c->bytesPerCluster && data.sector == sector &&
        data.record >= 512 && data.record <= 65536 && !(data.record & (data.record - 1)))
        c->bytesPerRecord = data.record;
    if (status == STATUS_SUCCESS && got == sizeof(data) &&
        data.cluster == c->bytesPerCluster && data.sector == sector &&
        data.clusters.QuadPart > 0 && (ntfs_u64)data.clusters.QuadPart <= c->maximum &&
        data.zoneStart.QuadPart >= 0 && data.zoneStart.QuadPart < data.zoneEnd.QuadPart &&
        data.zoneEnd.QuadPart <= data.clusters.QuadPart) {
        c->zoneStart = (ntfs_u64)data.zoneStart.QuadPart;
        c->zoneCount = (ntfs_u64)data.zoneEnd.QuadPart - c->zoneStart;
    } else {
        nout_status(L"NTFS MFT-zone query unavailable/invalid; targets remain driver-checked:",
                     status == STATUS_SUCCESS ? BF_BAD_REPLY : status);
    }
    return 1;
}

int boot_files_run(HANDLE volume, wchar_t drive, ntfs_u64 resultReference,
                    ULONG sector, ntfs_u64 volumeBytes,
                    int dryRun, boot_file_stats *stats,
                    int (*progress)(void *), void *progressContext)
{
    bf_context c;
    bf_frame *frame = NULL;
    wchar_t root[] = L"\\??\\C:\\";
    union { ntfs_u64 align; BYTE data[64 + 512]; } buffer;
    NTSTATUS status;
    HANDLE handle;
    memset(&c, 0, sizeof(c)); memset(stats, 0, sizeof(*stats));
    c.volume = volume; c.stats = stats; c.dryRun = dryRun;
    c.resultReference = resultReference; c.chunk = 65536;
    c.progress = progress; c.progressContext = progressContext;
    stats->result = FILES_RUNNING; root[4] = drive;
    extmap_init(&c.map);
    if (!bf_progress(&c))
        goto done;
    if (!bf_geometry(&c, sector, volumeBytes)) {
        if (!c.stopped)
            c.stopped = 5;
        goto done;
    }
    if (!bf_excl_grow(&c)) {
        bf_error(&c, L"Cannot allocate the refused-target table:", (NTSTATUS)FM_NOMEM);
        goto done;
    }
    status = (NTSTATUS)bf_refresh(&c);
    if (status != STATUS_SUCCESS && status != (NTSTATUS)FM_NOMEM) {
        c.chunk = 4096;
        status = (NTSTATUS)bf_refresh(&c);
    }
    if (status != STATUS_SUCCESS) {
        bf_error(&c, L"Cannot load the file-pass bitmap:", status);
        c.stopped = status == (NTSTATUS)FM_NOT_SUPPORTED ||
                    status == (NTSTATUS)FM_NOT_IMPLEMENTED ||
                    status == (NTSTATUS)FM_INVALID_REQUEST ? 3 : 5;
        goto done;
    }
    handle = bf_open(&c, NULL, root, 7, 1, &status);
    if (!handle) {
        bf_error(&c, L"Cannot open the volume's root directory:", status);
        c.stopped = 5;
        goto done;
    }
    frame = malloc(sizeof(*frame));
    if (!frame) {
        bf_error(&c, L"Cannot allocate the root traversal frame:", (NTSTATUS)FM_NOMEM);
        bf_close(&c, handle);
        goto done;
    }
    frame->parent = NULL; frame->handle = handle;
    frame->id = 0; frame->depth = 0; frame->first = 1;
    stats->directories++;
    bf_file(&c, handle, handle, L"", BF_DIRECTORY, 1, &frame->id);
    while (frame && !c.stopped) {
        IO_STATUS_BLOCK io;
        NATIVE_DIRECTORY_INFORMATION *entry = (NATIVE_DIRECTORY_INFORMATION *)buffer.data;
        wchar_t name[256];
        unsigned chars, i;
        ntfs_u64 previouslyMoved = stats->moved, childId = 0;
        int directory, validName = 1, identified;
        if (!bf_alive(&c)) {
            c.stopped = 5;
            break;
        }
        memset(&io, 0, sizeof(io));
        status = NtQueryDirectoryFile(frame->handle, NULL, NULL, NULL, &io, buffer.data,
                                       sizeof(buffer.data), 1, 1, NULL, (BOOLEAN)frame->first);
        status = ncomplete(frame->handle, status, &io);
        frame->first = 0;
        if (status == STATUS_NO_MORE_FILES) {
            bf_frame *parent = frame->parent;
            bf_close(&c, frame->handle);
            free(frame); frame = parent;
            continue;
        }
        if (status != STATUS_SUCCESS) {
            bf_frame *parent = frame->parent;
            bf_error(&c, L"Directory enumeration failed:", status);
            if (c.stopped)
                break;
            bf_close(&c, frame->handle);
            free(frame); frame = parent;
            continue;
        }
        if (io.Information < 64 || io.Information > sizeof(buffer.data) ||
            entry->NextEntryOffset || !entry->FileNameLength || entry->FileNameLength > 510 ||
            entry->FileNameLength % 2 || entry->FileNameLength > io.Information - 64) {
            bf_error(&c, L"Invalid native directory reply:", BF_BAD_REPLY);
            break;
        }
        chars = entry->FileNameLength / 2;
        for (i = 0; i < chars; i++) {
            name[i] = entry->FileName[i];
            if (!name[i] || name[i] == L'\\' || name[i] == L'/' || name[i] == L':')
                validName = 0;
        }
        name[chars] = 0;
        if (!validName) {
            bf_error(&c, L"Invalid directory-entry name:", BF_BAD_REPLY);
            break;
        }
        if (bf_equal(name, L".") || bf_equal(name, L".."))
            continue;
        if (!frame->parent && bf_reserved(name)) {
            stats->metadata++;
            continue;
        }
        directory = !!(entry->FileAttributes & BF_DIRECTORY);
        if (!directory)
            stats->files++;
        if (entry->FileAttributes & 0x440UL) {
            stats->special++;
            continue;
        }
        handle = bf_open(&c, frame->handle, name, chars, directory, &status);
        if (!handle) {
            if (status == BF_SHARING)
                stats->busy++;
            else {
                nout(name);
                bf_error(&c, L"Cannot open a file/directory:", status);
            }
            continue;
        }
        identified = bf_file(&c, frame->handle, handle, name, entry->FileAttributes,
                             directory, &childId);
        if (directory && identified && !c.stopped) {
            bf_frame *ancestor, *child;
            for (ancestor = frame; ancestor; ancestor = ancestor->parent)
                if (ancestor->id == childId) {
                    bf_error(&c, L"Directory cycle detected:", BF_BAD_REPLY);
                    bf_close(&c, handle);
                    break;
                }
            if (c.stopped)
                break;
            if (frame->depth >= BF_MAX_DEPTH) {
                bf_error(&c, L"Directory tree exceeds the supported depth bound:", BF_BAD_REPLY);
                bf_close(&c, handle);
                break;
            }
            child = malloc(sizeof(*child));
            if (!child) {
                bf_error(&c, L"Cannot allocate a directory frame:", (NTSTATUS)FM_NOMEM);
                bf_close(&c, handle);
                break;
            }
            child->parent = frame; child->handle = handle;
            child->id = childId; child->depth = frame->depth + 1;
            child->first = 1;
            frame = child; stats->directories++;
        } else
            bf_close(&c, handle);
        if (++c.sinceProgress >= 128 || previouslyMoved != stats->moved)
            bf_progress(&c);
    }
done:
    while (frame) {
        bf_frame *parent = frame->parent;
        bf_close(&c, frame->handle);
        free(frame); frame = parent;
    }
    if (c.bitmap) {
        IO_STATUS_BLOCK io;
        c.controlCode = c.controlCapacity = c.controlReturned = 0;
        memset(c.retrieval, 0, sizeof(c.retrieval));
        c.allocationBytes = c.dataBytes = 0;
        c.standardStatus = c.standardReturned = 0;
        memset(&io, 0, sizeof(io));
        status = NtFlushBuffersFile(volume, &io);
        status = ncomplete(volume, status, &io);
        if (status != STATUS_SUCCESS) {
            bf_error(&c, L"Cannot flush the mounted file pass:", status);
            c.stopped = 5;
        }
    }
    extmap_free(&c.map); free(c.bitmap); free(c.excl);
    if (!noutput_ok()) {
        if (!stats->ntstatus)
            bf_error(&c, L"Boot display failed during the file pass:", BF_OUTPUT_ERROR);
        c.stopped = 5;
    }
    stats->result = c.stopped == 5 ? FILES_ERROR : c.stopped == 3 ? FILES_UNSUPPORTED :
                    stats->errors || stats->noSpace || stats->partial ?
                    FILES_PARTIAL : FILES_COMPLETE;
    nout_ascii(bootlog_files_result_name(stats->result));
    nout_count(L"Files visited:", stats->files);
    nout_count(L"Data streams examined:", stats->streams);
    nout_count(L"Initially fragmented streams:", stats->fragmented);
    nout_count(L"Streams made contiguous:", stats->moved);
    nout_count(L"Partially improved (still fragmented):", stats->reduced);
    nout_count(L"Already contiguous/resident:", stats->contiguous);
    nout_count(L"In-use exclusions:", stats->busy);
    nout_count(L"Compressed/sparse/reparse/device/encrypted exclusions:", stats->special);
    nout_count(L"No-space refusals:", stats->noSpace);
    nout_count(L"Errors:", stats->errors);
    nout_count(L"Directories visited (indices not moved):", stats->directories);
    nout_count(L"Reserved/internal exclusions:", stats->metadata);
    nout_count(L"Move refusals:", stats->refused);
    if (!noutput_ok()) {
        if (stats->result != FILES_ERROR)
            stats->errors++;
        if (!stats->ntstatus)
            stats->ntstatus = (ntfs_u32)BF_OUTPUT_ERROR;
        stats->result = FILES_ERROR;
    }
    return stats->result == FILES_COMPLETE ? 0 :
           stats->result == FILES_UNSUPPORTED ? 3 : stats->result == FILES_ERROR ? 5 : 4;
}

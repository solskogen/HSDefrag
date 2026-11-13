#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define malloc native_malloc
#define realloc native_realloc
#define free native_free
#define memset native_memset
#define memcpy native_memcpy
#define memmove native_memmove
#define memcmp native_memcmp
#define wcslen native_wcslen
#define wcsncmp native_wcsncmp
#define RtlCreateHeap mock_heap
#define RtlAllocateHeap mock_alloc
#define RtlReAllocateHeap mock_realloc
#define RtlFreeHeap mock_free
#define NtDisplayString mock_display
#define NtOpenFile mock_open
#define NtClose mock_close
#define NtQueryDirectoryFile mock_directory
#define NtQueryInformationFile mock_information
#define NtQueryVolumeInformationFile mock_volume
#define NtFsControlFile mock_control
#define NtFlushBuffersFile mock_flush
#define NtWaitForSingleObject mock_wait
#define NtDelayExecution mock_delay
#include "ntrt.c"
#include "filemove.c"
#include "../src/bootfiles.c"
#undef malloc
#undef realloc
#undef free
#undef memset
#undef memcpy
#undef memmove
#undef memcmp
#undef wcslen
#undef wcsncmp
#undef RtlAllocateHeap
#undef RtlReAllocateHeap
#undef RtlFreeHeap

__declspec(dllimport) PVOID NTAPI GetProcessHeap(void);
PVOID NTAPI RtlAllocateHeap(PVOID, ULONG, size_t);
PVOID NTAPI RtlReAllocateHeap(PVOID, ULONG, PVOID, size_t);
BOOLEAN NTAPI RtlFreeHeap(PVOID, ULONG, PVOID);

#define CLUSTERS 32768
#define MAX_STREAMS 64
#define MAX_NODES 600
#define MAX_HANDLES 600
#define VOLUME ((HANDLE)1)
#define RESULT_REFERENCE ((1ULL << 48) | 20)
#define FAILURE ((NTSTATUS)0xC0000001L)
#define DENIED ((NTSTATUS)0xC0000022L)
#define IO_FAILURE ((NTSTATUS)0xC0000185L)

typedef struct { const wchar_t *name; int parent; ULONG attributes; ntfs_u64 id; } model_node;
typedef struct {
    const wchar_t *name;
    int node, count, compression, lcn[4096];
} model_stream;
typedef struct { int live, node, stream, cursor, directory; } model_handle;
static model_node nodes[MAX_NODES];
static model_stream streams[MAX_STREAMS];
static model_handle handles[MAX_HANDLES];
static ULONG owners[CLUSTERS];
static wchar_t longName[256], streamNames[16][263];
static int nodeCount, streamCount, scenario, moves, opens, liveHandles, peakHandles;
static int allocations, failAllocation, liveAllocations, waits, progressCalls, flushes;
static int busyNode, aliasNode, allocationFailed, sourceQueries, bitmapQueries;
static ntfs_u64 protectedStart, protectedCount;
static PVOID allocated[2048];
static unsigned checks;

#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL boot files line %d, scenario %d, allocation %d/%d: %s\n", \
            __LINE__, scenario, allocations, failAllocation, #x); exit(1); } } while (0)

static NTSTATUS complete(IO_STATUS_BLOCK *io, NTSTATUS status, ULONG bytes)
{
    io->Status = status; io->Information = bytes;
    return scenario == 17 || scenario == 38 ? STATUS_PENDING : status;
}

static int allocation_slot(PVOID pointer)
{
    int i;
    for (i = 0; i < 2048; i++)
        if (allocated[i] == pointer)
            return i;
    CHECK(0);
    return 0;
}

PVOID NTAPI mock_heap(ULONG flags, PVOID base, size_t reserve, size_t commit,
                       PVOID lock, PVOID parameters)
{
    CHECK(flags == 2 && !base && !reserve && !commit && !lock && !parameters);
    return GetProcessHeap();
}

PVOID NTAPI mock_alloc(PVOID heap, ULONG flags, size_t size)
{
    PVOID pointer;
    CHECK(heap == GetProcessHeap() && !flags && size);
    if (++allocations == failAllocation) {
        allocationFailed = 1;
        return NULL;
    }
    pointer = RtlAllocateHeap(heap, flags, size);
    CHECK(pointer);
    allocated[allocation_slot(NULL)] = pointer;
    liveAllocations++;
    return pointer;
}

PVOID NTAPI mock_realloc(PVOID heap, ULONG flags, PVOID old, size_t size)
{
    int slot = allocation_slot(old);
    PVOID pointer;
    CHECK(heap == GetProcessHeap() && !flags && old && size);
    if (++allocations == failAllocation) {
        allocationFailed = 1;
        return NULL;
    }
    pointer = RtlReAllocateHeap(heap, flags, old, size);
    CHECK(pointer);
    allocated[slot] = pointer;
    return pointer;
}

BOOLEAN NTAPI mock_free(PVOID heap, ULONG flags, PVOID pointer)
{
    CHECK(heap == GetProcessHeap() && !flags && pointer);
    allocated[allocation_slot(pointer)] = NULL;
    liveAllocations--;
    return RtlFreeHeap(heap, flags, pointer);
}

NTSTATUS NTAPI mock_display(PUNICODE_STRING string)
{
    CHECK(string->Length && string->Length <= string->MaximumLength);
    return scenario == 30 && moves ? FAILURE : STATUS_SUCCESS;
}

static model_handle *file_handle(HANDLE handle)
{
    model_handle *h = handle;
    CHECK(h >= handles && h < handles + MAX_HANDLES && h->live);
    return h;
}

NTSTATUS NTAPI mock_open(HANDLE *out, ULONG access, POBJECT_ATTRIBUTES attributes,
                          PIO_STATUS_BLOCK io, ULONG share, ULONG options)
{
    wchar_t name[520], base[256];
    unsigned chars = attributes->ObjectName->Length / 2, split = 0;
    int node = -1, stream = -1, i, directory = !!(options & FILE_DIRECTORY_FILE);
    model_handle *h = NULL;
    CHECK(access == (SYNCHRONIZE | GENERIC_READ_NT) && share == 7 &&
          (options & FILE_SYNCHRONOUS_IO_NONALERT) && chars < 520);
    CHECK(attributes->Attributes == (attributes->RootDirectory ? 0 : OBJ_CASE_INSENSITIVE));
    CHECK(!(options & FILE_WRITE_THROUGH));
    memcpy(name, attributes->ObjectName->Buffer, chars * 2); name[chars] = 0;
    *out = NULL; opens++;
    if (!attributes->RootDirectory) {
        CHECK(!wcscmp(name, L"\\??\\C:\\") && directory);
        node = 0;
    } else {
        model_handle *parent = file_handle(attributes->RootDirectory);
        CHECK(parent->directory);
        while (split < chars && name[split] != L':')
            split++;
        CHECK(split <= 255);
        memcpy(base, name, split * 2); base[split] = 0;
        if (!split)
            node = parent->node;
        for (i = 1; i < nodeCount; i++)
            if (nodes[i].parent == parent->node && !wcscmp(nodes[i].name, base))
                node = i;
        CHECK(node >= 0);
        CHECK(!(nodes[node].attributes & 0x400));
        if (parent->node == 0)
            CHECK(wcscmp(base, L"$MFT") && wcscmp(base, L"hsdefrag.boot"));
        if (node == busyNode)
            return scenario == 15 ? DENIED : BF_SHARING;
        for (i = 0; i < streamCount; i++)
            if (streams[i].node == node &&
                !wcscmp(streams[i].name, split < chars ? name + split : L"::$DATA"))
                stream = i;
        if (!directory)
            CHECK(stream >= 0 || node == aliasNode);
        CHECK(directory == !!(nodes[node].attributes & BF_DIRECTORY) || split < chars);
    }
    if (!directory)
        CHECK((options & (FILE_NON_DIRECTORY_FILE | FILE_NO_INTERMEDIATE_BUFFERING)) ==
              (FILE_NON_DIRECTORY_FILE | FILE_NO_INTERMEDIATE_BUFFERING));
    for (i = 0; i < MAX_HANDLES; i++)
        if (!handles[i].live) { h = handles + i; break; }
    CHECK(h);
    h->live = 1; h->node = node; h->stream = stream; h->cursor = 1; h->directory = directory;
    *out = h;
    if (++liveHandles > peakHandles)
        peakHandles = liveHandles;
    return complete(io, STATUS_SUCCESS, 0);
}

NTSTATUS NTAPI mock_close(HANDLE handle)
{
    model_handle *h = file_handle(handle);
    h->live = 0; liveHandles--;
    return scenario == 16 && moves == 1 ? FAILURE : STATUS_SUCCESS;
}

NTSTATUS NTAPI mock_directory(HANDLE handle, HANDLE event, PVOID apc, PVOID context,
                               PIO_STATUS_BLOCK io, PVOID out, ULONG length,
                               ULONG cls, BOOLEAN single, PUNICODE_STRING pattern,
                               BOOLEAN restart)
{
    model_handle *h = file_handle(handle);
    NATIVE_DIRECTORY_INFORMATION *entry = out;
    unsigned chars;
    int node;
    CHECK(!event && !apc && !context && !pattern && cls == 1 && single &&
          h->directory && length == 576);
    CHECK(!!restart == (h->cursor == 1));
    if (scenario == 7 || (scenario == 36 && h->node != 0))
        return complete(io, DENIED, 0);
    for (node = h->cursor; node < nodeCount && nodes[node].parent != h->node; node++) {}
    h->cursor = node + 1;
    if (node == nodeCount)
        return complete(io, STATUS_NO_MORE_FILES, 0);
    memset(out, 0, length);
    chars = (unsigned)wcslen(nodes[node].name);
    CHECK(chars <= 255);
    entry->FileAttributes = nodes[node].attributes;
    entry->FileNameLength = chars * 2;
    memcpy(entry->FileName, nodes[node].name, chars * 2);
    if (scenario == 6) entry->NextEntryOffset = 64;
    if (scenario == 24) entry->FileNameLength = 511;
    if (scenario == 25) entry->FileName[0] = L'\\';
    if (scenario == 26) entry->FileNameLength = 512;
    return complete(io, STATUS_SUCCESS, 64 + (scenario == 24 ? 511 : scenario == 26 ? 512 : chars * 2));
}

NTSTATUS NTAPI mock_information(HANDLE handle, PIO_STATUS_BLOCK io, PVOID out,
                                 ULONG length, ULONG cls)
{
    model_handle *h = file_handle(handle);
    unsigned bytes = 0;
    int i;
    if (cls == 5) {
        FILE_STANDARD_INFORMATION *standard = out;
        CHECK(length == sizeof(*standard) && h->stream >= 0 && !h->directory);
        CHECK(scenario >= 44 && scenario <= 87 && scenario != 51);
        memset(standard, 0, sizeof(*standard));
        standard->AllocationSize.QuadPart = streams[h->stream].count * 512LL;
        standard->EndOfFile.QuadPart = scenario == 45 ? 100 : standard->AllocationSize.QuadPart;
        if (scenario == 49) standard->EndOfFile.QuadPart = -1;
        if (scenario == 50) standard->Directory = 1;
        if (scenario == 60) standard->AllocationSize.QuadPart += 512;
        if (scenario == 63) standard->AllocationSize.QuadPart = -1;
        if (scenario == 64) standard->EndOfFile.QuadPart++;
        if (scenario >= 66 && !streams[h->stream].count) {
            standard->AllocationSize.QuadPart = 296;
            standard->EndOfFile.QuadPart = 289;
            if (scenario == 67 || scenario == 74) {
                standard->AllocationSize.QuadPart = 600;
                standard->EndOfFile.QuadPart = 599;
            }
            if (scenario == 68) standard->EndOfFile.QuadPart = 296;
            if (scenario == 69) {
                standard->AllocationSize.QuadPart = 512;
                standard->EndOfFile.QuadPart = 505;
            }
            if (scenario == 70) standard->AllocationSize.QuadPart = 295;
            if (scenario == 71) standard->AllocationSize.QuadPart = 304;
            if (scenario == 72) standard->AllocationSize.QuadPart = 288;
            if (scenario == 73) {
                standard->AllocationSize.QuadPart = 1032;
                standard->EndOfFile.QuadPart = 1025;
            }
        }
        if (scenario >= 78 && h->stream == 0) {
            standard->EndOfFile.QuadPart = 508;
            if (scenario == 79) standard->EndOfFile.QuadPart = 512;
            if (scenario == 80) standard->EndOfFile.QuadPart = 513;
            if (scenario == 84) standard->Directory = 1;
        }
        complete(io, scenario == 47 || scenario == 62 || scenario == 82 ? DENIED : STATUS_SUCCESS,
                 scenario == 48 || scenario == 61 || scenario == 83 ?
                 sizeof(*standard) - 1 : sizeof(*standard));
        return scenario == 52 ? STATUS_PENDING : io->Status;
    }
    if (cls == 6) {
        CHECK(length == 8);
        ((LARGE_INTEGER *)out)->QuadPart = (long long)nodes[h->node].id;
        return complete(io, STATUS_SUCCESS, 8);
    }
    CHECK(cls == 22 && h->node != aliasNode);
    sourceQueries++;
    if (scenario == 40)
        return complete(io, STATUS_BUFFER_OVERFLOW, 0);
    if (scenario == 18)
        return complete(io, (NTSTATUS)0xC0000003L, 0);
    for (i = 0; i < streamCount; i++)
        if (streams[i].node == h->node)
            bytes += (24 + (unsigned)wcslen(streams[i].name) * 2 + 7) & ~7U;
    if (bytes > length)
        return complete(io, STATUS_BUFFER_OVERFLOW, 0);
    memset(out, 0, length);
    bytes = 0;
    for (i = 0; i < streamCount; i++) {
        NATIVE_STREAM_INFORMATION *entry;
        unsigned nameBytes, span;
        int next;
        if (streams[i].node != h->node)
            continue;
        nameBytes = (unsigned)wcslen(streams[i].name) * 2;
        span = (24 + nameBytes + 7) & ~7U;
        CHECK(bytes + span <= length);
        entry = (NATIVE_STREAM_INFORMATION *)((BYTE *)out + bytes);
        entry->StreamNameLength = nameBytes;
        entry->StreamSize.QuadPart = entry->StreamAllocationSize.QuadPart = streams[i].count * 512;
        memcpy(entry->StreamName, streams[i].name, nameBytes);
        if (scenario == 8) entry->StreamName[0] = L'\\';
        if (scenario == 32) entry->NextEntryOffset = 1;
        else {
            for (next = i + 1; next < streamCount && streams[next].node != h->node; next++) {}
            entry->NextEntryOffset = next < streamCount ? span : 0;
        }
        bytes += span;
    }
    return complete(io, STATUS_SUCCESS, scenario == 9 ? length + 1 :
                     scenario == 34 && bytes ? bytes + 8 : bytes);
}

NTSTATUS NTAPI mock_volume(HANDLE handle, PIO_STATUS_BLOCK io, PVOID out, ULONG length,
                            ULONG cls)
{
    CHECK(handle == VOLUME);
    memset(out, 0, length);
    if (scenario == 37)
        return complete(io, DENIED, 0);
    if (cls == 5) {
        CHECK(length >= 20);
        ((ULONG *)out)[1] = 255; ((ULONG *)out)[2] = 8;
        memcpy((BYTE *)out + 12, scenario == 12 ? L"FAT " : L"NTFS", 8);
        return complete(io, STATUS_SUCCESS, 20);
    }
    CHECK(cls == 3 && length == sizeof(NATIVE_FS_SIZE_INFORMATION));
    ((NATIVE_FS_SIZE_INFORMATION *)out)->TotalAllocationUnits.QuadPart = CLUSTERS;
    ((NATIVE_FS_SIZE_INFORMATION *)out)->AvailableAllocationUnits.QuadPart = 1000;
    ((NATIVE_FS_SIZE_INFORMATION *)out)->BytesPerSector = 512;
    ((NATIVE_FS_SIZE_INFORMATION *)out)->SectorsPerAllocationUnit = scenario == 2 ? 16 : 1;
    if (scenario == 2)
        ((NATIVE_FS_SIZE_INFORMATION *)out)->TotalAllocationUnits.QuadPart /= 16;
    return complete(io, STATUS_SUCCESS, length);
}

NTSTATUS NTAPI mock_control(HANDLE handle, HANDLE event, PVOID apc, PVOID context,
                             PIO_STATUS_BLOCK io, ULONG code, PVOID in, ULONG inBytes,
                             PVOID out, ULONG outBytes)
{
    CHECK(!event && !apc && !context);
    if (code == BF_NTFS_DATA) {
        CHECK(handle == VOLUME && outBytes == 96);
        if (scenario == 31 || scenario == 74 || scenario == 75 || scenario == 87)
            return complete(io, (NTSTATUS)FM_NOT_SUPPORTED, 0);
        memset(out, 0, outBytes);
        ((long long *)out)[2] = CLUSTERS;
        ((ULONG *)out)[10] = ((ULONG *)out)[11] = 512;
        ((ULONG *)out)[12] = 1024;
        ((long long *)out)[10] = 16; ((long long *)out)[11] = 64;
        return complete(io, STATUS_SUCCESS, outBytes);
    }
    if (code == FM_BITMAP) {
        unsigned i, bytes = CLUSTERS / 8;
        CHECK(handle == VOLUME && inBytes == 8 && !*(ntfs_u64 *)in && outBytes >= 16 + bytes);
        bitmapQueries++;
        if (scenario == 35)
            return complete(io, (NTSTATUS)FM_NOT_SUPPORTED, 0);
        memset(out, 0, outBytes);
        ((ntfs_u64 *)out)[1] = scenario == 19 ? CLUSTERS + 1 : CLUSTERS;
        for (i = 0; i < CLUSTERS; i++)
            if (owners[i] || scenario == 13)
                ((BYTE *)out)[16 + i / 8] |= (BYTE)(1U << (i & 7));
        return complete(io, STATUS_SUCCESS, scenario == 11 ? 16 : 16 + bytes);
    }
    if (code == BF_COMPRESSION) {
        model_handle *h = file_handle(handle);
        CHECK(outBytes == 2 && h->stream >= 0 && !h->directory);
        if (scenario == 20)
            return complete(io, (NTSTATUS)FM_INVALID_REQUEST, 0);
        *(USHORT *)out = scenario == 14 && streams[h->stream].name[1] != L':' ? 2 : 0;
        return complete(io, STATUS_SUCCESS, scenario == 43 ? 0 : 2);
    }
    if (code == FM_RETRIEVAL) {
        model_handle *h = file_handle(handle);
        model_stream *s;
        unsigned count = 0;
        int vcn;
        CHECK(h->stream >= 0 && !h->directory && inBytes == 8 && outBytes == 16 + 1024 * 16);
        s = streams + h->stream;
        if ((scenario >= 66 && !s->count) || (scenario >= 78 && h->stream == 0)) {
            if (scenario == 76 || scenario == 86) ((long long *)out)[1] = 1;
            if (scenario == 77 || scenario == 85) *(unsigned *)out = 1;
            return complete(io, STATUS_SUCCESS, 0);
        }
        if ((scenario >= 44 && scenario <= 53 && !s->count) || scenario == 46 ||
            (scenario == 51 && *(long long *)in))
            return complete(io, STATUS_SUCCESS, 0);
        if (!s->count)
            return complete(io, (NTSTATUS)FM_EOF, 0);
        vcn = (int)*(long long *)in;
        memset(out, 0, outBytes); ((long long *)out)[1] = vcn;
        while (vcn < s->count && count < 1024) {
            int begin = vcn++;
            while (vcn < s->count && s->lcn[vcn] == s->lcn[vcn - 1] + 1)
                vcn++;
            ((long long *)((BYTE *)out + 16))[count * 2] = vcn;
            ((long long *)((BYTE *)out + 16))[count * 2 + 1] = s->lcn[begin];
            count++;
        }
        *(unsigned *)out = count;
        if (scenario == 29 && h->stream == 0)
            ((long long *)((BYTE *)out + 16))[1] = -1;
        if (scenario >= 54) {
            if (scenario == 55) memset((BYTE *)out + 16 + (count - 1) * 16, 0, 16);
            if (scenario == 56) *(unsigned *)out = 1025;
            if (scenario == 57) ((long long *)((BYTE *)out + 16))[0] = -1;
            if (scenario == 58) ((long long *)((BYTE *)out + 16))[1] = CLUSTERS;
            if (scenario == 59) ((long long *)out)[1] = 1;
            if (scenario == 65) memset((BYTE *)out + 16, 0, count * 16);
        }
        return complete(io, vcn < s->count ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS,
                         scenario >= 54 ? 0 : scenario == 10 ? 16 : 16 + count * 16);
    }
    if (code == FM_MOVE) {
        NATIVE_MOVE_FILE_DATA *move = in;
        model_handle *h;
        model_stream *s;
        unsigned i, take;
        ntfs_u64 target = (ntfs_u64)move->StartingLcn.QuadPart;
        CHECK(handle == VOLUME && inBytes == 32 && !out && !outBytes &&
              !allocationFailed && !(move->StartingVcn.QuadPart & 15) &&
              move->ClusterCount && move->ClusterCount <= 2048);
        h = file_handle(move->FileHandle); CHECK(h->stream >= 0 && !h->directory);
        s = streams + h->stream;
        CHECK(move->StartingVcn.QuadPart >= 0 &&
              move->StartingVcn.QuadPart + move->ClusterCount <= s->count &&
              target + move->ClusterCount <= CLUSTERS);
        CHECK(!protectedCount || target + move->ClusterCount <= protectedStart ||
              target >= protectedStart + protectedCount);
        moves++;
        if (scenario == 3)
            return complete(io, (NTSTATUS)FM_NOT_SUPPORTED, 0);
        if (target < 64 && target + move->ClusterCount > 16)
            return complete(io, (NTSTATUS)FM_INVALID_PARAMETER, 0);
        if (scenario == 4)
            return complete(io, STATUS_SUCCESS, 0);
        take = scenario == 5 ? move->ClusterCount / 2 : move->ClusterCount;
        for (i = 0; i < take; i++) {
            int vcn = (int)move->StartingVcn.QuadPart + (int)i;
            ULONG owner = ((ULONG)h->stream + 1) * 65536 + (ULONG)vcn + 1;
            CHECK(!owners[target + i] && owners[s->lcn[vcn]] == owner);
            owners[target + i] = owner; owners[s->lcn[vcn]] = 0;
            s->lcn[vcn] = (int)target + (int)i;
        }
        return complete(io, scenario == 5 ? FAILURE : STATUS_SUCCESS, 0);
    }
    CHECK(0);
    return FAILURE;
}

NTSTATUS NTAPI mock_flush(HANDLE handle, PIO_STATUS_BLOCK io)
{
    CHECK(handle == VOLUME && !liveHandles);
    flushes++;
    return complete(io, scenario == 21 ? IO_FAILURE : STATUS_SUCCESS, 0);
}
NTSTATUS NTAPI mock_wait(HANDLE handle, BOOLEAN alert, PLARGE_INTEGER timeout)
{
    (void)handle; CHECK(!alert && !timeout); waits++;
    return scenario == 38 ? IO_FAILURE : STATUS_SUCCESS;
}
NTSTATUS NTAPI mock_delay(BOOLEAN alert, PLARGE_INTEGER delay)
{
    CHECK(!alert && delay->QuadPart == -30000000LL); return STATUS_SUCCESS;
}
static int progress(void *ctx)
{
    CHECK(ctx == &checks);
    progressCalls++;
    return scenario != 33 || progressCalls < 2;
}

static int add_node(int parent, const wchar_t *name, ULONG attributes)
{
    int node = nodeCount++;
    CHECK(node < MAX_NODES);
    nodes[node].parent = parent; nodes[node].name = name; nodes[node].attributes = attributes;
    nodes[node].id = (1ULL << 48) | (ntfs_u64)(node + 30);
    return node;
}
static void add_stream(int node, const wchar_t *name, int count)
{
    int stream = streamCount++, i, base = 100 + stream * 64;
    model_stream *s = streams + stream;
    CHECK(stream < MAX_STREAMS && count <= 4096);
    s->node = node; s->name = name; s->count = count;
    for (i = 0; i < count; i++) {
        int lcn = count > 32 && (scenario == 39 || scenario == 51) ? 6000 + i * 2 :
                  count > 32 ? (i < count / 2 ? 6000 + i : 12000 + i - count / 2) :
                  base + i + (i >= count / 2 ? 16 : 0);
        CHECK(lcn < CLUSTERS && !owners[lcn]);
        s->lcn[i] = lcn; owners[lcn] = ((ULONG)stream + 1) * 65536 + (ULONG)i + 1;
    }
}

static void setup(int mode, int fail)
{
    int node, directory, i;
    CHECK(!liveHandles && !liveAllocations);
    scenario = mode; failAllocation = fail;
    allocations = allocationFailed = peakHandles = moves = opens = waits = 0;
    progressCalls = flushes = sourceQueries = bitmapQueries = 0;
    memset(owners, 0, sizeof(owners)); memset(nodes, 0, sizeof(nodes));
    memset(streams, 0, sizeof(streams)); memset(handles, 0, sizeof(handles));
    nodeCount = streamCount = 0; protectedStart = protectedCount = 0;
    busyNode = aliasNode = -1;
    for (i = 0; i < 8; i++) owners[i] = ~0U;
    node = add_node(-1, L"", BF_DIRECTORY); CHECK(node == 0); nodes[0].id = 5;
    if (mode == 41)
        add_stream(0, L":$I30:$INDEX_ALLOCATION", 0);
    if (mode == 22 || mode == 89) {
        int depth = mode == 89 ? (int)BF_MAX_DEPTH + 1 : 400;
        directory = 0;
        for (i = 0; i < depth; i++)
            directory = add_node(directory, L"d", BF_DIRECTORY);
        node = add_node(directory, L"\x00F8.dat", 0);
        add_stream(node, L"::$DATA", 32);
    } else if (mode == 88) {
        directory = add_node(0, L"loop", BF_DIRECTORY);
        node = add_node(directory, L"again", BF_DIRECTORY);
        nodes[node].id = nodes[directory].id;
    } else {
        if (mode == 27) {
            for (i = 0; i < 16; i++) {
                unsigned j;
                streamNames[i][0] = L':';
                for (j = 1; j < 242; j++) streamNames[i][j] = L'x';
                streamNames[i][242] = (wchar_t)(L'A' + i);
                memcpy(streamNames[i] + 243, L":$DATA", 7 * sizeof(wchar_t));
                add_stream(0, streamNames[i], 32);
            }
        } else if (mode != 53)
            add_stream(0, L":root-side:$DATA", mode >= 78 ? (mode == 81 ? 2 : 1) : mode >= 66 ? 0 :
                       mode == 28 || mode == 39 || mode == 51 ? 4096 : 32);
        add_node(0, L"$MFT", 0);
        add_node(0, L"hsdefrag.boot", 0);
        node = add_node(0, L"data.bin", 0);
        add_stream(node, L"::$DATA", mode == 53 || mode >= 66 ? 0 : 32);
        add_stream(node, L":side:$DATA", mode >= 66 ? 0 : 32);
        directory = add_node(0, L"folder", BF_DIRECTORY);
        if (mode == 41)
            add_stream(directory, L"::$BITMAP", 0);
        add_stream(directory, L":directory-side:$DATA", 32);
        for (i = 0; i < 255; i++) longName[i] = L'\x00F8';
        longName[255] = 0;
        node = add_node(directory, mode == 23 ? longName : L"\x00E6.txt", 0);
        add_stream(node, L"::$DATA", 32);
        node = add_node(directory, L"$MFT", 0); add_stream(node, L"::$DATA", 32);
        node = add_node(0, L"resident", 0); add_stream(node, L"::$DATA", 0);
        busyNode = add_node(0, L"busy", 0);
        node = add_node(0, L"sparse", 0x200); add_stream(node, L"::$DATA", 32);
        node = add_node(0, L"compressed", 0x800); add_stream(node, L"::$DATA", 32);
        add_node(0, L"junction", BF_DIRECTORY | 0x400);
        aliasNode = add_node(0, L"result-alias", 0); nodes[aliasNode].id = RESULT_REFERENCE;
        node = add_node(0, L"$notes", 0); add_stream(node, L"::$DATA", 32);
        if (mode == 42) {
            node = add_node(0, L"CASE", 0); add_stream(node, L"::$DATA", 32);
            node = add_node(0, L"case", 0); add_stream(node, L"::$DATA", 32);
        }
    }
    CHECK(nrt_init());
}

static int run(int mode, int fail, boot_file_stats *stats)
{
    int rc, i, j;
    setup(mode, fail);
    rc = boot_files_run(VOLUME, L'C', RESULT_REFERENCE, 512, CLUSTERS * 512ULL,
                         mode == 1, stats, progress, &checks);
    CHECK(!liveHandles && !liveAllocations);
    for (i = 0; i < streamCount; i++)
        for (j = 0; j < streams[i].count; j++)
            CHECK(owners[streams[i].lcn[j]] == ((ULONG)i + 1) * 65536 + (ULONG)j + 1);
    return rc;
}

static void partial_stream_check(void)
{
    bf_context context;
    boot_file_stats stats;
    model_handle *handle = handles;
    int i;
    setup(0, 0);
    for (i = 0; i < streams[0].count; i++)
        owners[streams[0].lcn[i]] = 0;
    for (i = 0; i < CLUSTERS; i++)
        if (!owners[i]) owners[i] = ~0U;
    streams[0].count = 64;
    for (i = 0; i < 64; i++) {
        streams[0].lcn[i] = 6000 + (i / 16) * 32 + i % 16;
        owners[streams[0].lcn[i]] = 65536 + (ULONG)i + 1;
    }
    for (i = 1000; i < 1032; i++) owners[i] = 0;
    memset(&context, 0, sizeof(context));
    memset(&stats, 0, sizeof(stats));
    context.volume = VOLUME; context.bytesPerCluster = 512;
    context.maximum = CLUSTERS; context.chunk = 65536; context.stats = &stats;
    CHECK(bf_refresh(&context) == FM_SUCCESS);
    handle->live = 1; handle->node = 0; handle->stream = 0;
    liveHandles = 1;
    bf_stream(&context, handle, L":partial:$DATA", 0);
    /* A stream that only partly fits is an improvement, not a refusal: it is
     * counted on its own, and the no-space counter stays at zero. */
    CHECK(stats.fragmented == 1 && stats.reduced == 1 && stats.partial == 1 &&
          !stats.noSpace && !stats.moved && !stats.refused && !stats.errors &&
          extmap_fragments(&context.map) == 3);
    for (i = 0; i < 64; i++)
        CHECK(owners[streams[0].lcn[i]] == 65536 + (ULONG)i + 1);
    CHECK(bf_close(&context, handle));
    extmap_free(&context.map);
    native_free(context.bitmap);
    CHECK(!liveHandles && !liveAllocations);
}

static int bit_set(const bf_context *c, ntfs_u64 lcn)
{
    return (c->bitmap[lcn / 8] >> (lcn & 7)) & 1;
}

/* More refusals than the old fixed table held, at far-apart places: each
 * range stays separate, and the free space between them stays usable even
 * after the bitmap is read again. */
static void exclusion_checks(void)
{
    bf_context context;
    boot_file_stats stats;
    ntfs_u64 i, lcn, target = 0, available = 0;
    setup(0, 0);
    memset(owners, 0, sizeof(owners));
    memset(&context, 0, sizeof(context));
    memset(&stats, 0, sizeof(stats));
    context.volume = VOLUME; context.bytesPerCluster = 512;
    context.maximum = CLUSTERS; context.chunk = 65536; context.stats = &stats;
    CHECK(bf_refresh(&context) == FM_SUCCESS);
    for (i = 0; i < 80; i++)
        bf_exclude(&context, 300 + i * 400, 16);
    CHECK(!stats.errors && !context.stopped);
    CHECK(bf_refresh(&context) == FM_SUCCESS);
    for (i = 0; i < 80; i++)
        for (lcn = 300 + i * 400; lcn < 700 + i * 400 && lcn < CLUSTERS; lcn++)
            CHECK(bit_set(&context, lcn) == (lcn < 316 + i * 400));
    CHECK(bf_find(&context, 384, 26000, &target, &available) &&
          available == 384 && target >= 26000);
    native_free(context.excl);
    native_free(context.bitmap);
    CHECK(!liveAllocations);
}

/* If the table cannot grow, the refused range is not merged into another
 * one: moves stop for the rest of the pass and the result is incomplete. */
static void exclusion_failure_checks(void)
{
    bf_context context;
    boot_file_stats stats;
    model_handle *handle = handles;
    ntfs_u64 i, target = 0, available = 0;
    setup(0, 0);
    memset(&context, 0, sizeof(context));
    memset(&stats, 0, sizeof(stats));
    context.volume = VOLUME; context.bytesPerCluster = 512;
    context.maximum = CLUSTERS; context.chunk = 65536; context.stats = &stats;
    CHECK(bf_refresh(&context) == FM_SUCCESS);
    for (i = 0; i < 64; i++)
        bf_exclude(&context, 300 + i * 400, 16);
    CHECK(context.nExcl == 64 && !context.noMoves);
    failAllocation = allocations + 1;
    bf_exclude(&context, 300 + 64 * 400, 16);
    CHECK(allocationFailed && context.noMoves && !context.stopped);
    CHECK(stats.errors == 1 && stats.ntstatus == (ntfs_u32)FM_NOMEM);
    CHECK(context.nExcl == 64 && context.excl[63].start == 300 + 63 * 400 &&
          context.excl[63].count == 16);
    for (i = 300 + 64 * 400; i < 316 + 64 * 400; i++)
        CHECK(bit_set(&context, i));
    CHECK(!bit_set(&context, 316 + 63 * 400));
    CHECK(!bf_find(&context, 16, 0, &target, &available));
    /* a fragmented stream is counted, not moved */
    handle->live = 1; handle->node = 0; handle->stream = 0;
    liveHandles = 1; moves = 0;
    bf_stream(&context, handle, L":root-side:$DATA", 0);
    CHECK(!moves && stats.fragmented == 1 && !stats.moved && !context.stopped);
    CHECK(bf_close(&context, handle));
    extmap_free(&context.map);
    native_free(context.excl);
    native_free(context.bitmap);
    CHECK(!liveHandles && !liveAllocations);
}

int wmain(int argc, wchar_t **argv)
{
    boot_file_stats stats;
    int mode, count, at, pattern, boundaries = 0;
    static const int allocationPatterns[] = {0, 27, 39};
    (void)argc; (void)argv;
    partial_stream_check();
    exclusion_checks();
    exclusion_failure_checks();
    CHECK(run(0, 0, &stats) == 0);
    count = allocations;
    CHECK(stats.files == 9 && stats.streams == 10 && stats.moved == 7 &&
          stats.fragmented == 7 && stats.contiguous == 1 && stats.busy == 1 &&
          stats.special == 3 && stats.metadata == 3 && stats.directories == 2 &&
          !stats.errors && !stats.noSpace && stats.result == FILES_COMPLETE);
    for (mode = 1; mode <= 89; mode++) {
        int expected = mode == 2 || mode == 3 || mode == 35 ? 3 :
                       mode == 4 || mode == 5 || mode == 7 || mode == 13 ||
                       mode == 15 || mode == 18 || mode == 20 || mode == 36 ||
                       mode == 47 || mode == 62 || mode == 82 ? 4 :
                       mode == 6 || (mode >= 8 && mode <= 12) || mode == 16 ||
                       mode == 19 || mode == 21 || (mode >= 24 && mode <= 26) ||
                       mode == 30 || mode == 32 || mode == 33 || mode == 34 ||
                       mode == 37 || mode == 38 || mode == 40 || mode == 43 ||
                       mode == 46 || mode == 48 || mode == 49 || mode == 50 ||
                       mode == 51 || (mode >= 55 && mode <= 65 && mode != 62) ||
                       (mode >= 70 && mode <= 74) || mode == 76 || mode == 77 ||
                       mode == 80 || mode == 81 || (mode >= 83 && mode <= 86) ||
                       mode == 88 || mode == 89 ? 5 : 0;
        CHECK(run(mode, 0, &stats) == expected);
        if (mode == 1) CHECK(!moves && stats.fragmented == 7);
        if (mode == 2) CHECK(!moves && !opens && stats.result == FILES_UNSUPPORTED);
        if (mode == 3) CHECK(moves == 1 && stats.result == FILES_UNSUPPORTED);
        if (mode == 4) CHECK(moves == 28 && !stats.moved && stats.refused == 7);
        if (mode == 5) CHECK(moves == 7 && !stats.moved && stats.refused == 7);
        if (mode == 13) CHECK(!moves && stats.noSpace == 7);
        if (mode == 17) CHECK(waits > 30 && stats.moved == 7);
        if (mode == 21)
            CHECK(stats.ntstatus == (ntfs_u32)IO_FAILURE &&
                  !stats.controlCode && !stats.controlCapacity && !stats.controlReturned &&
                  !stats.retrieval[0] && !stats.retrieval[1] &&
                  !stats.allocationBytes && !stats.dataBytes &&
                  !strcmp(stats.errorContext, "Cannot flush the mounted file pass:"));
        if (mode == 22) CHECK(peakHandles >= 401 && stats.directories == 401 && stats.moved == 1);
        if (mode == 27) CHECK(stats.moved == 22 && sourceQueries > 10);
        if (mode == 28) CHECK(moves == 8 && stats.moved == 7);
        if (mode == 29) CHECK(stats.moved == 6 && stats.special == 4);
        if (mode == 35) CHECK(!moves && !opens && stats.result == FILES_UNSUPPORTED);
        if (mode == 36) CHECK(stats.errors == 1 && stats.moved == 5 && stats.files == 7);
        if (mode == 37) CHECK(!moves && !opens && stats.result == FILES_ERROR);
        if (mode == 38) CHECK(!moves && !opens && waits && stats.ntstatus == (ntfs_u32)IO_FAILURE);
        if (mode == 39) CHECK(moves == 8 && stats.moved == 7);
        if (mode == 40) CHECK(!moves && stats.result == FILES_ERROR);
        if (mode == 41) CHECK(stats.metadata == 5 && stats.moved == 7 && stats.streams == 10);
        if (mode == 42) CHECK(stats.files == 11 && stats.moved == 9);
        if (mode == 10)
            CHECK(stats.controlCode == FM_RETRIEVAL && stats.controlReturned == 16 &&
                  !strcmp(stats.errorContext, "Cannot map a data stream:"));
        if (mode == 43)
            CHECK(!moves && stats.controlCode == BF_COMPRESSION &&
                  stats.controlCapacity == 2 && !stats.controlReturned &&
                  !strcmp(stats.errorContext, "Cannot determine stream compression:"));
        if (mode == 44 || mode == 45)
            CHECK(stats.moved == 7 && stats.contiguous == 1 && !stats.errors);
        if (mode == 46)
            CHECK(!moves && stats.controlCode == FM_RETRIEVAL && !stats.controlReturned &&
                  stats.ntstatus == (ntfs_u32)FM_BAD_REPLY);
        if (mode >= 47 && mode <= 50)
            CHECK(stats.errors == 1 && stats.ntstatus ==
                  (ntfs_u32)(mode == 47 ? DENIED : FM_BAD_REPLY));
        if (mode == 51)
            CHECK(!moves && stats.ntstatus == (ntfs_u32)FM_BAD_REPLY);
        if (mode == 53)
            CHECK(stats.moved == 5 && stats.contiguous == 2 && !stats.errors);
        if (mode == 52)
            CHECK(waits == 1 && stats.moved == 7 && stats.contiguous == 1 && !stats.errors);
        if (mode == 54)
            CHECK(stats.moved == 7 && stats.contiguous == 1 && !stats.errors);
        if (mode >= 55 && mode <= 65 && mode != 62)
            CHECK(!moves && stats.ntstatus == (ntfs_u32)FM_BAD_REPLY);
        if (mode == 66 || mode == 67 || mode == 68 || mode == 69 || mode == 75 ||
            mode == 78 || mode == 79 || mode == 87)
            CHECK(stats.moved == 4 && stats.contiguous == 4 && !stats.errors);
        if ((mode >= 70 && mode <= 74) || mode == 76 || mode == 77 ||
            mode == 80 || mode == 81 || (mode >= 83 && mode <= 86) ||
            mode == 88 || mode == 89)
            CHECK(!moves && stats.ntstatus == (ntfs_u32)FM_BAD_REPLY);
        if (mode == 88)
            CHECK(!strcmp(stats.errorContext, "Directory cycle detected:"));
        if (mode == 89)
            CHECK(!strcmp(stats.errorContext, "Directory tree exceeds the supported depth bound:"));
        if (mode == 46)
            CHECK(stats.allocationBytes == 32 * 512 && stats.dataBytes == 32 * 512 &&
                  stats.standardReturned == sizeof(FILE_STANDARD_INFORMATION));
        if (!expected) CHECK(stats.result == FILES_COMPLETE);
    }
    for (pattern = 0; pattern < 3; pattern++) {
        CHECK(run(allocationPatterns[pattern], 0, &stats) == 0);
        count = allocations; boundaries += count;
        for (at = 1; at <= count; at++) {
            CHECK(run(allocationPatterns[pattern], at, &stats) == 5 && allocationFailed);
            CHECK(stats.result == FILES_ERROR && stats.ntstatus == (ntfs_u32)FM_NOMEM);
        }
    }
    printf("Native file traversal/streams/FSCTL/heap-fault regressions: %u checks, %d allocation boundaries.\n",
           checks, boundaries);
    return 0;
}

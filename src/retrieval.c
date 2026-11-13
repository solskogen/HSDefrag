/*
 * retrieval.c - VCN->LCN extent maps of open files (online NT path).
 *
 * The map comes from FSCTL_GET_RETRIEVAL_POINTERS; filemove.c's shared
 * fm_get_extents() asks for it piece by piece and validates every reply
 * (extent count, VCN continuity, LCN range). This file only supplies the
 * Win32 transport and works around the NT 4.0 replies that report a
 * length of zero although the call did something (see legacy_retrieval()
 * and win_file_control()). A workaround accepts a reply only when it is
 * consistent in itself and, where the reply is meant to be complete, with
 * the separately queried stream size; otherwise it stays an error, because
 * a wrong map would make a later move verify against the wrong clusters.
 */
#include "retrieval.h"

typedef struct {
    HSD_NTSTATUS status;
    ULONG_PTR information;
} retrieval_io;
typedef struct {
    LARGE_INTEGER allocation, data;
    ULONG links;
    BYTE deletePending, directory;
} retrieval_standard;
typedef HSD_NTSTATUS (WINAPI *query_information)(
    HANDLE, retrieval_io *, void *, ULONG, ULONG);
typedef char retrieval_standard_size[sizeof(retrieval_standard) == 24 ? 1 : -1];

/*
 * NT 4.0 sometimes completes FSCTL_GET_RETRIEVAL_POINTERS at VCN 0 with
 * success but an Information (bytes returned) of zero. That happens both
 * for streams that have no extents (resident data, stored inside the MFT
 * record) and for ordinary maps whose length it simply did not report. The
 * reply alone cannot tell these apart, so:
 *
 * - win_file_control() fills the buffer with 0xFF before the call and zeroes
 *   only the 16-byte header (extent count and starting VCN). A header still
 *   zero afterwards means "no extents written". An extent slot the driver
 *   did not write reads as NextVcn = -1, which can never pass the shared
 *   parser's increasing-VCN check, so leftover bytes cannot pose as extents.
 *
 * - The stream's size is then asked for independently, with
 *   NtQueryInformationFile class 5 (FileStandardInformation: allocation
 *   size, end of file). ntdll is looked up at run time, as the Win32 image
 *   imports nothing but KERNEL32.
 *
 * - With no extents, the reply is "nothing to map" (FM_EOF) only for sizes a
 *   resident stream can have: no allocation at all; exactly one cluster
 *   (NT 4.0 rounds a resident stream's allocation up to one cluster, and a
 *   real one-cluster stream is contiguous anyway, so neither needs a move);
 *   or an allocation below the MFT record size that is not a whole number
 *   of clusters and equals the data size rounded up to 8 bytes (how resident
 *   attribute storage is sized).
 *
 * - With extents, the length is inferred (16 + 16 per extent) only for a
 *   complete map starting at VCN 0 whose last NextVcn equals the allocation
 *   in clusters. Anything else is a bad reply.
 */
static fm_status legacy_retrieval(VOLUME *v, HANDLE file, void *out,
                                  unsigned capacity, unsigned *got)
{
    retrieval_standard standard;
    retrieval_io io;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    query_information query;
    HSD_NTSTATUS status;
    unsigned count;
    unsigned long long start, last, allocation, data;
    if (!ntdll)
        return FM_NOT_SUPPORTED;
    query = (query_information)(void (*)(void))GetProcAddress(ntdll, "NtQueryInformationFile");
    if (!query)
        return FM_NOT_SUPPORTED;
    memset(&standard, 0, sizeof(standard));
    memset(&io, 0, sizeof(io));
    status = query(file, &io, &standard, sizeof(standard), 5);
    if (status == HSD_STATUS_PENDING) {
        if (WaitForSingleObject(file, INFINITE) != WAIT_OBJECT_0)
            return FM_BAD_REPLY;
        status = io.status;
    }
    if (status != HSD_STATUS_SUCCESS)
        return status == HSD_STATUS_PENDING ? FM_BAD_REPLY : (fm_status)status;
    if (io.information != sizeof(standard) || standard.directory ||
        standard.allocation.QuadPart < 0 || standard.data.QuadPart < 0)
        return FM_BAD_REPLY;
    allocation = (unsigned long long)standard.allocation.QuadPart;
    data = (unsigned long long)standard.data.QuadPart;
    memcpy(&count, out, sizeof(count));
    memcpy(&start, (BYTE *)out + 8, sizeof(start));
    if (!allocation)
        return count ? FM_BAD_REPLY : FM_EOF;
    if (!count && !start && data <= allocation &&
        (allocation == v->bytesPerCluster ||
         (allocation % v->bytesPerCluster &&
          allocation < (v->mftRecordSize ? v->mftRecordSize : v->bytesPerCluster) &&
          allocation == ((data + 7) & ~7ULL))))
        return FM_EOF;
    if (!start && count && count <= (capacity - 16) / 16 &&
        !(allocation % v->bytesPerCluster) && data <= allocation) {
        unsigned bytes = 16 + count * 16;
        memcpy(&last, (BYTE *)out + bytes - 16, sizeof(last));
        if (last == allocation / v->bytesPerCluster) {
            *got = bytes;
            return FM_SUCCESS;
        }
    }
    return FM_BAD_REPLY;
}

/*
 * The same zero-length report on an overflow reply at VCN 0: the buffer was
 * filled and more extents follow. Its length is inferred only if the header
 * says the buffer is exactly full and, starting at VCN 0, every slot's
 * NextVcn is larger than the one before. A slot the driver did not write
 * holds the 0xFF prefill (NextVcn -1) and fails that test. Returns the
 * inferred length, or 0 to leave it a bad reply.
 */
static unsigned legacy_overflow(const void *out, unsigned capacity)
{
    const BYTE *p = out;
    unsigned count, i, slots = (capacity - 16) / 16;
    long long start, previous, next;
    memcpy(&count, p, sizeof(count));
    memcpy(&start, p + 8, sizeof(start));
    if (start || !count || count != slots)
        return 0;
    previous = 0;
    for (i = 0; i < count; i++) {
        memcpy(&next, p + 16 + i * 16, sizeof(next));
        if (next <= previous)
            return 0;
        previous = next;
    }
    return 16 + count * 16;
}

fm_status win_file_control(void *ctx, void *handle, unsigned code, void *in,
                           unsigned inBytes, void *out, unsigned outBytes,
                           unsigned *returned)
{
    DWORD got = 0;
    HSD_NTSTATUS status;
    VOLUME *volume = ctx;
    if (volume && code == FM_RETRIEVAL && outBytes >= 32) {
        memset(out, 0xFF, outBytes);
        memset(out, 0, 16);
    }
    status = hsd_fsctl(handle, code, in, inBytes, out, outBytes, &got);
    if (volume && volume->isNtfs && volume->bytesPerCluster &&
        code == FM_RETRIEVAL && status == HSD_STATUS_SUCCESS && !got &&
        outBytes >= 32 && inBytes == sizeof(long long) && !*(long long *)in) {
        unsigned legacyBytes = 0;
        status = (HSD_NTSTATUS)legacy_retrieval(volume, handle, out, outBytes, &legacyBytes);
        got = legacyBytes;
    } else if (volume && volume->isNtfs && code == FM_RETRIEVAL &&
               status == HSD_STATUS_BUFFER_OVERFLOW && !got && outBytes >= 32 &&
               inBytes == sizeof(long long) && !*(long long *)in) {
        got = legacy_overflow(out, outBytes);
    }
    if (returned)
        *returned = got;
    return (fm_status)status;
}

HSD_NTSTATUS extmap_get(VOLUME *volume, HANDLE file, EXTMAP *map)
{
    return (HSD_NTSTATUS)fm_get_extents(win_file_control, volume, file, map);
}

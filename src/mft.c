/*
 * mft.c - Read-only NTFS metadata fragmentation reporting.
 */
#include "mft.h"
#include "ntfsmft.h"
#include "util.h"

typedef struct { VOLUME *volume; BYTE *buffer; } raw_view;

static int read_volume(void *ctx, ntfs_u64 offset, void *buf, ntfs_u32 len)
{
    raw_view *view = ctx;
    ntfs_u32 done = 0;
    while (done < len) {
        DWORD take = len - done > 65536 ? 65536 : len - done;
        if (!volume_read_raw(view->volume, offset + done, view->buffer, take)) {
            uwin32_error(L"reading NTFS metadata", GetLastError());
            uerror(L"metadata read offset: %llu; requested: %lu bytes\n",
                   offset + done, (unsigned long)take);
            return 0;
        }
        memcpy((BYTE *)buf + done, view->buffer, take);
        done += take;
    }
    return 1;
}

int mft_report(VOLUME *v)
{
    ntfsmft_io io;
    ntfsmft_info info;
    mft_status status;
    raw_view view;
    PARTITION_INFORMATION partition;
    DWORD got = 0;

    if (!v->isNtfs)
        return 0;
    if (!v->bytesPerSector || !v->bytesPerCluster ||
        v->totalClusters > 0xFFFFFFFFFFFFFFFFULL / v->bytesPerCluster) {
        uinfo(L"\n  NTFS metadata: unavailable (invalid volume geometry)\n");
        return 4;
    }

    memset(&partition, 0, sizeof(partition));
    if (!DeviceIoControl(v->hVol, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0,
                          &partition, sizeof(partition), &got, NULL)) {
        uwin32_error(L"querying NTFS partition length", GetLastError());
        return 4;
    }
    if (got != sizeof(partition) || partition.PartitionLength.QuadPart <= 0) {
        uerror(L"invalid NTFS partition length reply\n");
        return 4;
    }
    view.volume = v;
    view.buffer = VirtualAlloc(NULL, 65536, MEM_COMMIT, PAGE_READWRITE);
    if (!view.buffer) {
        uwin32_error(L"allocating aligned metadata buffer", GetLastError());
        return 5;
    }
    memset(&io, 0, sizeof(io));
    io.ctx = &view;
    io.read = read_volume;
    io.sectorSize = v->bytesPerSector;
    io.length = (ntfs_u64)partition.PartitionLength.QuadPart;

    status = ntfsmft_analyze(&io, &info, NULL, NULL);
    if (!VirtualFree(view.buffer, 0, MEM_RELEASE)) {
        uwin32_error(L"releasing metadata buffer", GetLastError());
        return 5;
    }
    uinfo(L"\n  NTFS metadata:\n");
    if (status != MFT_OK) {
        wchar_t message[128];
        const char *text = ntfsmft_strstatus(status);
        size_t i = 0;
        while (text[i] && i + 1 < sizeof(message) / sizeof(*message)) {
            message[i] = (unsigned char)text[i];
            i++;
        }
        message[i] = 0;
        uinfo(L"    $MFT : unavailable (%ls)\n", message);
        return uoutput_ok() ? 4 : 5;
    }

    uinfo(L"    $MFT : %d fragment(s), %llu clusters, starts at LCN %llu\n",
          info.fragments, info.mftClusters, info.mftLcn);
    return uoutput_ok() ? 0 : 5;
}

/*
 * volume.c - Volume handling: open, cluster geometry, and the free/used
 * cluster bitmap (via FSCTL_GET_VOLUME_BITMAP).
 */
#include "volume.h"
#include "util.h"
#include "retrieval.h"

static int bit_get(const BYTE *bitmap, unsigned long long idx)
{
    return (bitmap[idx >> 3] >> (idx & 7)) & 1;
}

static int starts_with_fat(const wchar_t *s)
{
    return (s[0] == L'F' || s[0] == L'f') && (s[1] == L'A' || s[1] == L'a') &&
           (s[2] == L'T' || s[2] == L't');
}

/* NTFS on NT 4.0 / Windows 2000 (before 5.1) has the legacy move limits. */
static int legacy_ntfs_moves(void)
{
    return !uos_at_least(5, 1);
}

static const wchar_t *drive_type_name(UINT t)
{
    switch (t) {
    case DRIVE_NO_ROOT_DIR: return L"not present";
    case DRIVE_REMOVABLE:   return L"removable";
    case DRIVE_FIXED:       return L"fixed";
    case DRIVE_REMOTE:      return L"network";
    case DRIVE_CDROM:       return L"CD-ROM";
    case DRIVE_RAMDISK:     return L"RAM disk";
    default:                return L"unknown";
    }
}

static void query_ntfs_data(VOLUME *v)
{
    NTFS_VOLUME_DATA_BUFFER nd;
    DWORD got = 0;
    HSD_NTSTATUS st;
    unsigned long long total, zs, ze;

    memset(&nd, 0, sizeof(nd));
    st = hsd_fsctl(v->hVol, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &nd,
                  (DWORD)sizeof(nd), &got);
    v->ntfsDataStatus = st;
    if (!HSD_NT_SUCCESS(st))
        return;
    if (got != sizeof(nd)) {
        v->ntfsDataStatus = HSD_STATUS_BAD_REPLY;
        return;
    }

    if (nd.TotalClusters.QuadPart <= 0 || nd.MftStartLcn.QuadPart < 0 ||
        nd.Mft2StartLcn.QuadPart < 0 || nd.MftValidDataLength.QuadPart < 0 ||
        nd.MftZoneStart.QuadPart < 0 || nd.MftZoneEnd.QuadPart < 0 ||
        nd.BytesPerCluster != (DWORD)v->bytesPerCluster)
        return; /* unexpected layout or invalid signed NTFS geometry */

    total = (unsigned long long)nd.TotalClusters.QuadPart;
    if (total == 0 ||
        (unsigned long long)nd.MftStartLcn.QuadPart >= total ||
        (unsigned long long)nd.MftStartLcn.QuadPart >= v->totalClusters ||
        (nd.Mft2StartLcn.QuadPart != 0 &&
         ((unsigned long long)nd.Mft2StartLcn.QuadPart >= total ||
          (unsigned long long)nd.Mft2StartLcn.QuadPart >= v->totalClusters)))
        return; /* unexpected layout: ignore rather than trust garbage */

    v->haveNtfsData = 1;
    v->mftStartLcn = (unsigned long long)nd.MftStartLcn.QuadPart;
    v->mft2StartLcn = (unsigned long long)nd.Mft2StartLcn.QuadPart;
    v->mftValidDataLength = (unsigned long long)nd.MftValidDataLength.QuadPart;
    v->mftRecordSize = nd.BytesPerFileRecordSegment;

    zs = (unsigned long long)nd.MftZoneStart.QuadPart;
    ze = (unsigned long long)nd.MftZoneEnd.QuadPart;
    if (zs < ze && ze <= total) {
        v->mftZoneStart = zs;
        v->mftZoneEnd = ze;
        /* NT 4.0 NTFS refuses to move data into the MFT zone. */
        /* On failure exclLost is set and cmd_defrag() refuses to move. */
        (void)volume_exclude(v, zs, ze - zs);
    }
}

int volume_open(VOLUME *v, wchar_t drive)
{
    wchar_t volPath[8];
    DWORD sectorsPerCluster = 0, bytesPerSector = 0;
    DWORD freeClusters = 0, totalClusters = 0;

    memset(v, 0, sizeof(*v));
    v->drive = drive;
    v->hVol = INVALID_HANDLE_VALUE;
    v->moveAlign = 1;
    wcscpy(v->fsName, L"unknown");

    u_snwprintf(v->root, 8, L"%lc:\\", drive);

    v->driveType = GetDriveTypeW(v->root);
    if (v->driveType == DRIVE_NO_ROOT_DIR || v->driveType == DRIVE_REMOTE ||
        v->driveType == DRIVE_CDROM) {
        uerror(L"drive %lc: is %ls; only local disk volumes can be "
               L"defragmented\n",
               drive, drive_type_name(v->driveType));
        return 0;
    }

    if (!GetDiskFreeSpaceW(v->root, &sectorsPerCluster, &bytesPerSector,
                           &freeClusters, &totalClusters)) {
        uwin32_error(L"GetDiskFreeSpace", GetLastError());
        return 0;
    }
    v->sectorsPerCluster = sectorsPerCluster;
    v->bytesPerSector = bytesPerSector;
    v->bytesPerCluster =
        (unsigned long long)sectorsPerCluster * bytesPerSector;
    v->totalClusters = (unsigned long long)totalClusters;
    v->freeClusters = (unsigned long long)freeClusters;
    if (!sectorsPerCluster || bytesPerSector < 512 ||
        bytesPerSector > 4096 || (bytesPerSector & (bytesPerSector - 1)) ||
        v->bytesPerCluster > 65536 || !totalClusters || freeClusters > totalClusters) {
        uerror(L"GetDiskFreeSpace returned invalid geometry\n");
        return 0;
    }

    if (GetVolumeInformationW(v->root, NULL, 0, NULL, NULL, NULL, v->fsName,
                              (DWORD)(sizeof(v->fsName) / sizeof(v->fsName[0])))) {
        v->isNtfs = (lstrcmpiW(v->fsName, L"NTFS") == 0);
        v->isFat = starts_with_fat(v->fsName);
    } else {
        uwin32_error(L"GetVolumeInformation", GetLastError());
    }

    if (v->isNtfs && legacy_ntfs_moves()) {
        v->moveAlign = 16;
        if (v->bytesPerCluster > 4096)
            v->movesUnsupported = 1;
    }

    u_snwprintf(volPath, 8, L"\\\\.\\%lc:", drive);

    /* GENERIC_READ only, as the NT 4.0 defrag FSCTLs are documented to need. */
    v->hVol = CreateFileW(volPath, GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                          OPEN_EXISTING, 0, NULL);
    if (v->hVol == INVALID_HANDLE_VALUE) {
        uwin32_error(L"opening volume (administrator rights required)",
                     GetLastError());
        return 0;
    }

    if (v->isNtfs)
        query_ntfs_data(v);
    return 1;
}

void volume_print_info(const VOLUME *v)
{
    wchar_t b1[32], b2[32], code[64];

    uinfo(L"Volume %lc:\n", v->drive);
    uinfo(L"  drive type         : %ls\n", drive_type_name(v->driveType));
    uinfo(L"  file system        : %ls\n", v->fsName);
    uinfo(L"  sector size        : %lu bytes\n",
          (unsigned long)v->bytesPerSector);
    uinfo(L"  cluster size       : %llu bytes (%lu sectors)\n",
          v->bytesPerCluster, (unsigned long)v->sectorsPerCluster);
    uinfo(L"  total clusters     : %llu (%ls)\n", v->totalClusters,
          uformat_bytes(v->totalClusters * v->bytesPerCluster, b1, 32));
    uinfo(L"  free clusters      : %llu (%ls)\n", v->freeClusters,
          uformat_bytes(v->freeClusters * v->bytesPerCluster, b2, 32));
    if (v->haveNtfsData) {
        uinfo(L"  MFT start LCN      : %llu\n", v->mftStartLcn);
        uinfo(L"  MFT size           : %ls\n",
              uformat_bytes(v->mftValidDataLength, b1, 32));
        if (v->mftZoneEnd > v->mftZoneStart)
            uinfo(L"  MFT zone           : LCN %llu - %llu (not used as a "
                  L"move target)\n",
                  v->mftZoneStart, v->mftZoneEnd - 1);
    } else if (v->isNtfs) {
        if (HSD_NT_SUCCESS(v->ntfsDataStatus))
            uinfo(L"  NTFS volume data   : unexpected reply (ignored)\n");
        else
            uinfo(L"  NTFS volume data   : unavailable, %ls\n",
                  hsd_status_str(v->ntfsDataStatus, code, 64));
    }
    if (v->moveAlign > 1)
        uinfo(L"  move granularity   : %u clusters (NTFS before Windows XP)\n",
              v->moveAlign);
    if (v->movesUnsupported)
        uinfo(L"  note               : NTFS before Windows XP cannot move "
              L"clusters larger than 4 KB\n");
}

/* Fetch the whole bitmap using chunkBytes-sized FSCTL replies.
 *
 * The first reply says how many clusters the bitmap covers, and that size
 * is allocated before any data is checked. A broken driver or filter could
 * claim an absurd size (up to 2^63 clusters), so the size is capped at
 * 2^32 clusters, the most NTFS and FAT can address (a 512 MiB bitmap).
 * The cap deliberately does not depend on GetDiskFreeSpace: with disk
 * quotas on Windows 2000 and later its total can be smaller than the
 * volume, and a valid bitmap must not be refused because of that. */
static HSD_NTSTATUS fetch_bitmap(VOLUME *v, DWORD chunkBytes)
{
    BYTE *bm = NULL;
    unsigned long long total = 0;
    const unsigned long long cap = 1ULL << 32;
    HSD_NTSTATUS st = (HSD_NTSTATUS)fm_get_bitmap(win_file_control, NULL,
                            v->hVol, chunkBytes, cap, &bm, &total);
    if (!HSD_NT_SUCCESS(st))
        return st;
    free(v->bitmap);
    v->bitmap = bm;
    v->bitmapClusters = total;
    v->bitmapChunk = chunkBytes;
    return HSD_STATUS_SUCCESS;
}

static void apply_exclusions(VOLUME *v)
{
    int i;
    for (i = 0; i < v->nExcl; i++)
        volume_mark(v, v->excl[i].lcn, v->excl[i].count, 1);
}

HSD_NTSTATUS volume_load_bitmap(VOLUME *v)
{
    HSD_NTSTATUS st = fetch_bitmap(v, 64 * 1024);

    if (!HSD_NT_SUCCESS(st) && st != HSD_STATUS_INSUFFICIENT_RESOURCES) {
        /* Retry once with the buffer size used by the original NT 4.0 sample. */
        HSD_NTSTATUS st2 = fetch_bitmap(v, 4096);
        if (HSD_NT_SUCCESS(st2))
            st = st2;
    }
    if (HSD_NT_SUCCESS(st)) {
        v->haveBitmap = 1;
        apply_exclusions(v);
        if (v->bitmapClusters != v->totalClusters)
            uverbose(L"  note: bitmap covers %llu clusters, GetDiskFreeSpace "
                     L"reported %llu\n",
                     v->bitmapClusters, v->totalClusters);
    }
    return st;
}

HSD_NTSTATUS volume_refresh_bitmap(VOLUME *v)
{
    HSD_NTSTATUS st;

    if (!v->haveBitmap)
        return volume_load_bitmap(v);
    st = fetch_bitmap(v, v->bitmapChunk ? v->bitmapChunk : 64 * 1024);
    if (HSD_NT_SUCCESS(st))
        apply_exclusions(v);
    return st;
}

int volume_cluster_used(const VOLUME *v, unsigned long long lcn)
{
    if (!v->bitmap || lcn >= v->bitmapClusters)
        return 1; /* out of range treated as used/unavailable */
    return bit_get(v->bitmap, lcn);
}

void volume_mark(VOLUME *v, unsigned long long lcn, unsigned long long count,
                 int used)
{
    if (!v->bitmap || lcn >= v->bitmapClusters)
        return;
    fm_mark(v->bitmap, v->bitmapClusters, lcn, count, used);
}

int volume_exclude(VOLUME *v, unsigned long long lcn,
                   unsigned long long count)
{
    if (count == 0)
        return 1;
    /* Mark it now in any case: the current bitmap at least avoids it. */
    volume_mark(v, lcn, count, 1);
    if (v->nExcl == v->capExcl) {
        int newCap = v->capExcl ? v->capExcl * 2 : 8;
        LCN_RANGE *p = NULL;
        if (v->capExcl < 0x1000000) /* keeps the byte count far from overflow */
            p = (LCN_RANGE *)realloc(v->excl, (size_t)newCap * sizeof(LCN_RANGE));
        if (!p) {
            v->exclLost = 1;
            return 0;
        }
        v->excl = p;
        v->capExcl = newCap;
    }
    v->excl[v->nExcl].lcn = lcn;
    v->excl[v->nExcl].count = count;
    v->nExcl++;
    return 1;
}

unsigned long long volume_largest_free_run(const VOLUME *v)
{
    unsigned long long target = 0, best = 0;

    if (!v->bitmap)
        return 0;
    fm_find_best(v->bitmap, v->bitmapClusters, ~0ULL, 0, &target, &best);
    return best;
}

int volume_read_raw(VOLUME *v, unsigned long long offset, void *buf, DWORD len)
{
    LONG hi = (LONG)(offset >> 32);
    DWORD lo, got = 0;

    SetLastError(NO_ERROR);
    lo = SetFilePointer(v->hVol, (LONG)(DWORD)(offset & 0xFFFFFFFFULL), &hi,
                        FILE_BEGIN);
    if (lo == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
        return 0;
    if (!ReadFile(v->hVol, buf, len, &got, NULL))
        return 0;
    if (got != len) {
        SetLastError(ERROR_HANDLE_EOF);
        return 0;
    }
    return 1;
}

void volume_close(VOLUME *v)
{
    if (v->hVol != INVALID_HANDLE_VALUE) {
        CloseHandle(v->hVol);
        v->hVol = INVALID_HANDLE_VALUE;
    }
    free(v->bitmap);
    v->bitmap = NULL;
    free(v->excl);
    v->excl = NULL;
    v->nExcl = v->capExcl = 0;
}

/*
 * fatfs.c - Portable FAT12/16/32 reader/writer (see fatfs.h).
 *
 * No Win32 dependency: all disk access goes through the abstract sector device.
 */
#include "fatfs.h"
#include "nostd.h"

#define FAT_IO_CHUNK 64u   /* max sectors per device call (Win9x-friendly) */
#define FAT_MAX_FAT_BYTES (64u * 1024u * 1024u)

/* --------------------------------------------------------- little-endian */

static fat_u16 rd16(const fat_u8 *p)
{
    return (fat_u16)(p[0] | ((fat_u16)p[1] << 8));
}

static fat_u32 rd32(const fat_u8 *p)
{
    return (fat_u32)p[0] | ((fat_u32)p[1] << 8) | ((fat_u32)p[2] << 16) |
           ((fat_u32)p[3] << 24);
}

static void wr16(fat_u8 *p, fat_u16 v)
{
    p[0] = (fat_u8)v;
    p[1] = (fat_u8)(v >> 8);
}

/* --------------------------------------------------------- chunked device */

static int dev_read(fat_volume *fv, fat_u32 sector, fat_u32 count, void *buf)
{
    fat_u8 *p = (fat_u8 *)buf;
    fat_u32 done = 0;

    if ((fat_u64)sector + count > 0x100000000ULL ||
        (fv->totalSectors && (fat_u64)sector + count > fv->totalSectors))
        return 0;
    while (done < count) {
        fat_u32 n = count - done;
        if (n > FAT_IO_CHUNK)
            n = FAT_IO_CHUNK;
        if (!fv->dev.read(fv->dev.ctx, sector + done, n, p))
            return 0;
        p += (size_t)n * fv->bytesPerSector;
        done += n;
    }
    return 1;
}

static int dev_write(fat_volume *fv, fat_u32 sector, fat_u32 count,
                     const void *buf)
{
    const fat_u8 *p = (const fat_u8 *)buf;
    fat_u32 done = 0;

    if (!fv->dev.write)
        return 0;
    if ((fat_u64)sector + count > 0x100000000ULL ||
        (fv->totalSectors && (fat_u64)sector + count > fv->totalSectors))
        return 0;
    while (done < count) {
        fat_u32 n = count - done;
        if (n > FAT_IO_CHUNK)
            n = FAT_IO_CHUNK;
        if (!fv->dev.write(fv->dev.ctx, sector + done, n, p))
            return 0;
        p += (size_t)n * fv->bytesPerSector;
        done += n;
    }
    return 1;
}

int fat_read_sectors(fat_volume *fv, fat_u32 sector, fat_u32 count, void *buf)
{
    return dev_read(fv, sector, count, buf);
}

int fat_write_sectors(fat_volume *fv, fat_u32 sector, fat_u32 count,
                      const void *buf)
{
    return dev_write(fv, sector, count, buf);
}

int fat_write_verified(fat_volume *fv, fat_u32 sector, fat_u32 count,
                        const void *buf)
{
    fat_u8 check[4096];
    const fat_u8 *p = (const fat_u8 *)buf;
    fat_u32 done = 0;

    if (!dev_write(fv, sector, count, buf))
        return 0;
    while (done < count) {
        fat_u32 n = (fat_u32)sizeof(check) / fv->bytesPerSector;

        if (n > count - done)
            n = count - done;
        if (!dev_read(fv, sector + done, n, check) ||
            memcmp(check, p, (size_t)n * fv->bytesPerSector) != 0)
            return 0;
        p += (size_t)n * fv->bytesPerSector;
        done += n;
    }
    return 1;
}

/* --------------------------------------------------------------- FAT type */

static fat_u32 type_mask(const fat_volume *fv)
{
    if (fv->fatType == FAT_TYPE_12)
        return 0x00000FFFu;
    if (fv->fatType == FAT_TYPE_16)
        return 0x0000FFFFu;
    return 0x0FFFFFFFu;
}

int fat_is_free(fat_u32 value)
{
    return value == 0;
}

int fat_is_eoc(const fat_volume *fv, fat_u32 value)
{
    value &= type_mask(fv);
    if (fv->fatType == FAT_TYPE_12)
        return value >= 0x0FF8u;
    if (fv->fatType == FAT_TYPE_16)
        return value >= 0xFFF8u;
    return value >= 0x0FFFFFF8u;
}

int fat_is_bad(const fat_volume *fv, fat_u32 value)
{
    value &= type_mask(fv);
    if (fv->fatType == FAT_TYPE_12)
        return value == 0x0FF7u;
    if (fv->fatType == FAT_TYPE_16)
        return value == 0xFFF7u;
    return value == 0x0FFFFFF7u;
}

fat_u32 fat_eoc_value(const fat_volume *fv)
{
    if (fv->fatType == FAT_TYPE_12)
        return 0x0FFFu;
    if (fv->fatType == FAT_TYPE_16)
        return 0xFFFFu;
    return 0x0FFFFFFFu;
}

/* --------------------------------------------------------- FAT entry I/O */

/* Record that the FAT bytes [off, off+len) changed (FAT12 entries may straddle
 * a sector boundary, so both sectors are marked). */
static void mark_dirty(fat_volume *fv, fat_u32 off, fat_u32 len)
{
    fat_u32 s = off / fv->bytesPerSector;
    fat_u32 e = (off + len - 1) / fv->bytesPerSector;

    for (; s <= e; s++)
        fv->dirtyMap[s >> 3] |= (fat_u8)(1u << (s & 7));
    fv->fatDirty = 1;
}

fat_u32 fat_get(const fat_volume *fv, fat_u32 cluster)
{
    fat_u32 off;

    if (cluster > fv->countOfClusters + 1)
        return fat_eoc_value(fv);
    if (fv->fatType == FAT_TYPE_12) {
        off = cluster + (cluster / 2);
        if (off + 1 >= fv->fatBytes)
            return fat_eoc_value(fv);
        {
            fat_u16 v = rd16(fv->fat + off);
            return (cluster & 1) ? (fat_u32)(v >> 4) : (fat_u32)(v & 0x0FFF);
        }
    }
    if (fv->fatType == FAT_TYPE_16) {
        off = cluster * 2;
        if (off + 1 >= fv->fatBytes)
            return fat_eoc_value(fv);
        return rd16(fv->fat + off);
    }
    off = cluster * 4;
    if (off + 3 >= fv->fatBytes)
        return fat_eoc_value(fv);
    return rd32(fv->fat + off) & 0x0FFFFFFFu;
}

void fat_set(fat_volume *fv, fat_u32 cluster, fat_u32 value)
{
    fat_u32 off;

    if (cluster > fv->countOfClusters + 1)
        return;
    if (fv->fatType == FAT_TYPE_12) {
        off = cluster + (cluster / 2);
        if (off + 1 >= fv->fatBytes)
            return;
        {
            fat_u16 cur = rd16(fv->fat + off);
            if (cluster & 1)
                cur = (fat_u16)((cur & 0x000F) | ((value & 0x0FFF) << 4));
            else
                cur = (fat_u16)((cur & 0xF000) | (value & 0x0FFF));
            wr16(fv->fat + off, cur);
        }
        mark_dirty(fv, off, 2);
    } else if (fv->fatType == FAT_TYPE_16) {
        off = cluster * 2;
        if (off + 1 >= fv->fatBytes)
            return;
        wr16(fv->fat + off, (fat_u16)value);
        mark_dirty(fv, off, 2);
    } else {
        off = cluster * 4;
        if (off + 3 >= fv->fatBytes)
            return;
        /* Preserve the top 4 reserved bits of the 32-bit FAT entry. */
        {
            fat_u32 cur = rd32(fv->fat + off);
            fat_u32 nv = (cur & 0xF0000000u) | (value & 0x0FFFFFFFu);
            fv->fat[off + 0] = (fat_u8)nv;
            fv->fat[off + 1] = (fat_u8)(nv >> 8);
            fv->fat[off + 2] = (fat_u8)(nv >> 16);
            fv->fat[off + 3] = (fat_u8)(nv >> 24);
        }
        mark_dirty(fv, off, 4);
    }
}

/* --------------------------------------------------------------- geometry */

int fat_cluster_valid(const fat_volume *fv, fat_u32 cluster)
{
    return cluster >= 2 && cluster <= fv->countOfClusters + 1;
}

fat_u32 fat_cluster_sector(const fat_volume *fv, fat_u32 cluster)
{
    return fv->firstDataSector + (cluster - 2) * fv->sectorsPerCluster;
}

/* ----------------------------------------------------------------- mount */

static int is_pow2(fat_u32 v)
{
    return v && (v & (v - 1)) == 0;
}

int fat_mount(fat_volume *fv, const fat_dev *dev)
{
    fat_u8 boot[4096];
    fat_u32 bps, totSec16, totSec32, fatSz16, fatSz32, dataSectors;
    fat_u32 rootDirBytes;
    fat_u64 layout, fatBytes, entries, needed;

    memset(fv, 0, sizeof(*fv));
    if (!dev || !dev->read)
        return 0;
    fv->dev = *dev;
    if (fv->dev.bytesPerSector == 0)
        fv->dev.bytesPerSector = 512;
    if (!is_pow2(fv->dev.bytesPerSector) ||
        fv->dev.bytesPerSector < 512 || fv->dev.bytesPerSector > sizeof(boot))
        return 0;
    fv->bytesPerSector = fv->dev.bytesPerSector;

    if (!dev_read(fv, 0, 1, boot))
        return 0;

    bps = rd16(boot + 0x0B);
    if (bps != fv->bytesPerSector || rd16(boot + 510) != 0xAA55)
        return 0;

    fv->sectorsPerCluster = boot[0x0D];
    fv->reservedSectors = rd16(boot + 0x0E);
    fv->numFats = boot[0x10];
    fv->rootEntryCount = rd16(boot + 0x11);
    totSec16 = rd16(boot + 0x13);
    fatSz16 = rd16(boot + 0x16);
    totSec32 = rd32(boot + 0x20);

    if (!is_pow2(fv->sectorsPerCluster) || fv->sectorsPerCluster > 128 ||
        fv->numFats == 0 || fv->reservedSectors == 0)
        return 0;

    fatSz32 = rd32(boot + 0x24);
    fv->fatSize = fatSz16 ? fatSz16 : fatSz32;
    fv->totalSectors = totSec16 ? totSec16 : totSec32;
    if (fv->fatSize == 0 || fv->totalSectors == 0)
        return 0;

    rootDirBytes = (fat_u32)fv->rootEntryCount * 32u;
    fv->rootDirSectors = (rootDirBytes + (bps - 1)) / bps;

    layout = fv->reservedSectors + (fat_u64)fv->numFats * fv->fatSize;
    if (layout + fv->rootDirSectors >= fv->totalSectors)
        return 0;
    fv->firstRootDirSector = (fat_u32)layout;
    fv->firstDataSector = (fat_u32)(layout + fv->rootDirSectors);

    /* Default: all FATs mirrored, FAT #0 is authoritative. FAT32 may disable
     * mirroring via BPB_ExtFlags, in which case a single FAT is live. */
    fv->fatMirrored = 1;
    fv->activeFat = 0;

    if (fv->firstDataSector >= fv->totalSectors)
        return 0;
    dataSectors = (fat_u32)(fv->totalSectors - fv->firstDataSector);
    fv->countOfClusters = dataSectors / fv->sectorsPerCluster;

    if (fv->countOfClusters < 4085)
        fv->fatType = FAT_TYPE_12;
    else if (fv->countOfClusters < 65525)
        fv->fatType = FAT_TYPE_16;
    else
        fv->fatType = FAT_TYPE_32;

    fatBytes = (fat_u64)fv->fatSize * bps;
    entries = (fat_u64)fv->countOfClusters + 2;
    needed = fv->fatType == FAT_TYPE_12 ? (entries * 3 + 1) / 2 :
             entries * (fv->fatType == FAT_TYPE_16 ? 2 : 4);
    if (!fv->countOfClusters || fatBytes > FAT_MAX_FAT_BYTES ||
        needed > fatBytes)
        return 0;

    if (fv->fatType == FAT_TYPE_32) {
        fat_u16 extFlags = rd16(boot + 0x28);
        fv->rootCluster = rd32(boot + 0x2C);
        fv->fsInfoSector = rd16(boot + 0x30);
        if (fatSz16 != 0 || rd16(boot + 0x2A) != 0 ||
            fv->rootEntryCount != 0 || !fat_cluster_valid(fv, fv->rootCluster))
            return 0;
        /* BPB_ExtFlags bit 7 set => mirroring disabled; bits 0-3 select the one
         * live FAT. Otherwise all FATs are kept identical and FAT #0 is read. */
        if (extFlags & 0x0080) {
            fv->fatMirrored = 0;
            fv->activeFat = (fat_u32)(extFlags & 0x000F);
            if (fv->activeFat >= fv->numFats)
                return 0;
        }
    } else {
        if (!fatSz16 || fv->rootEntryCount == 0 || rootDirBytes % bps != 0)
            return 0;
    }

    fv->fatStartSector =
        fv->reservedSectors + fv->activeFat * fv->fatSize;

    fv->bytesPerCluster = fv->sectorsPerCluster * fv->bytesPerSector;

    fv->fatBytes = (fat_u32)fatBytes;
    fv->fat = (fat_u8 *)malloc(fv->fatBytes);
    fv->dirtyMap = (fat_u8 *)calloc((fv->fatSize + 7) / 8, 1);
    if (!fv->fat || !fv->dirtyMap ||
        !dev_read(fv, fv->fatStartSector, fv->fatSize, fv->fat)) {
        fat_unmount(fv);
        return 0;
    }
    fv->fatDirty = 0;
    return 1;
}

void fat_unmount(fat_volume *fv)
{
    free(fv->fat);
    free(fv->dirtyMap);
    fv->fat = NULL;
    fv->dirtyMap = NULL;
}

int fat_reload(fat_volume *fv)
{
    memset(fv->dirtyMap, 0, (fv->fatSize + 7) / 8);
    fv->fatDirty = 0;
    return dev_read(fv, fv->fatStartSector, fv->fatSize, fv->fat) &&
           fat_mirrors_match(fv);
}

int fat_mirrors_match(fat_volume *fv)
{
    fat_u8 buf[4096];
    fat_u32 copy, s;

    if (!fv->fatMirrored)
        return 1;
    for (copy = 1; copy < fv->numFats; copy++) {
        for (s = 0; s < fv->fatSize;) {
            fat_u32 n = (fat_u32)sizeof(buf) / fv->bytesPerSector;

            if (n > fv->fatSize - s)
                n = fv->fatSize - s;
            if (!dev_read(fv, fv->reservedSectors + copy * fv->fatSize + s,
                          n, buf) ||
                memcmp(buf, fv->fat + (size_t)s * fv->bytesPerSector,
                       (size_t)n * fv->bytesPerSector) != 0)
                return 0;
            s += n;
        }
    }
    return 1;
}

static int is_dirty(const fat_volume *fv, fat_u32 s)
{
    return (fv->dirtyMap[s >> 3] >> (s & 7)) & 1;
}

int fat_flush(fat_volume *fv)
{
    fat_u32 s = 0, e, c;

    if (!fv->fatDirty)
        return 1;
    while (s < fv->fatSize) {
        if (!is_dirty(fv, s)) {
            s++;
            continue;
        }
        for (e = s + 1; e < fv->fatSize && is_dirty(fv, e); e++)
            ;
        /* Mirrored: keep every copy identical. Otherwise only the active FAT
         * is live; the stale copies are left untouched. */
        for (c = 0; c < fv->numFats; c++) {
            if (!fv->fatMirrored && c != fv->activeFat)
                continue;
            if (!fat_write_verified(fv,
                           fv->reservedSectors + c * fv->fatSize + s, e - s,
                           fv->fat + (size_t)s * fv->bytesPerSector))
                return 0;
        }
        for (; s < e; s++)
            fv->dirtyMap[s >> 3] &= (fat_u8)~(1u << (s & 7));
    }
    fv->fatDirty = 0;
    return 1;
}

int fat_revert(fat_volume *fv)
{
    fat_u32 s = 0, e;
    int ok = 1;

    while (fv->fatDirty && s < fv->fatSize) {
        if (!is_dirty(fv, s)) {
            s++;
            continue;
        }
        for (e = s + 1; e < fv->fatSize && is_dirty(fv, e); e++)
            ;
        if (!dev_read(fv, fv->fatStartSector + s, e - s,
                      fv->fat + (size_t)s * fv->bytesPerSector))
            ok = 0;
        for (; s < e; s++)
            fv->dirtyMap[s >> 3] &= (fat_u8)~(1u << (s & 7));
    }
    fv->fatDirty = 0;
    return ok;
}

/* ------------------------------------------------------------ free space */

fat_u32 fat_find_free_run(const fat_volume *fv, fat_u32 need)
{
    fat_u32 c, run = 0, start = 0, last = fv->countOfClusters + 1;

    if (need == 0)
        return 0;
    for (c = 2; c <= last; c++) {
        if (fat_is_free(fat_get(fv, c))) {
            if (run == 0)
                start = c;
            if (++run >= need)
                return start;
        } else {
            run = 0;
        }
    }
    return 0;
}

void fat_free_space(const fat_volume *fv, fat_u32 *freeClusters,
                    fat_u32 *freeRuns, fat_u32 *largestRun)
{
    fat_u32 c, last = fv->countOfClusters + 1;
    fat_u32 nFree = 0, runs = 0, run = 0, best = 0;

    for (c = 2; c <= last; c++) {
        if (fat_is_free(fat_get(fv, c))) {
            nFree++;
            if (run == 0)
                runs++;
            if (++run > best)
                best = run;
        } else {
            run = 0;
        }
    }
    if (freeClusters)
        *freeClusters = nFree;
    if (freeRuns)
        *freeRuns = runs;
    if (largestRun)
        *largestRun = best;
}

int fat_fsinfo_invalidate(fat_volume *fv)
{
    /* Stack buffer: this runs inside a write section, where the caller may
     * not allocate memory. */
    fat_u8 sec[4096];

    if (fv->fatType != FAT_TYPE_32 || fv->fsInfoSector == 0 ||
        fv->fsInfoSector >= fv->reservedSectors ||
        fv->bytesPerSector > sizeof(sec))
        return 1;
    if (!dev_read(fv, fv->fsInfoSector, 1, sec))
        return 0;
    /* Validate the FSInfo signatures before touching it. */
    if (rd32(sec + 0) == 0x41615252u && rd32(sec + 484) == 0x61417272u &&
        rd16(sec + 510) == 0xAA55u) {
        sec[488] = sec[489] = sec[490] = sec[491] = 0xFF; /* free count */
        sec[492] = sec[493] = sec[494] = sec[495] = 0xFF; /* next free   */
        return fat_write_verified(fv, fv->fsInfoSector, 1, sec);
    }
    return 1;
}

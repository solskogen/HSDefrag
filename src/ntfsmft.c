/*
 * Read-only NTFS $MFT inspection.
 *
 * This code validates on-disk metadata without exposing any raw metadata write,
 * relocation, journal, or recovery operation.
 */
#include "ntfsmft.h"
#include "ntfsrecord.h"

void *malloc(size_t);
void free(void *);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);

#define MAX_RUNS 8192

typedef struct {
    ntfs_u64 lcn;
    ntfs_u64 len;
} ntfs_run;

typedef struct {
    ntfsmft_io *io;
    ntfs_u64 bytesPerCluster;
    ntfs_u64 totalClusters;
    ntfs_u64 totalSectors;
    ntfs_u32 recSize;
    ntfs_u64 mftLcn;
    ntfs_u64 mirrorLcn;
    ntfs_u8 boot[512];
} volume;

typedef struct {
    ntfs_run *runs;
    int count;
    ntfs_u64 clusters;
    ntfs_u64 allocatedBytes;
    ntfs_u64 dataBytes;
    ntfs_u64 validBytes;
} run_map;

typedef struct {
    ntfs_run *runs;
    int count;
    ntfs_u64 bytes;
    ntfs_u8 *bits;
} bitmap;

static ntfs_u16 rd16(const ntfs_u8 *p)
{
    return (ntfs_u16)(p[0] | ((ntfs_u16)p[1] << 8));
}

static ntfs_u32 rd32(const ntfs_u8 *p)
{
    return (ntfs_u32)p[0] | ((ntfs_u32)p[1] << 8) |
           ((ntfs_u32)p[2] << 16) | ((ntfs_u32)p[3] << 24);
}

static ntfs_u64 rd64(const ntfs_u8 *p)
{
    return (ntfs_u64)rd32(p) | ((ntfs_u64)rd32(p + 4) << 32);
}

static void append(char *out, size_t cap, size_t *used, const char *s)
{
    while (*s && *used + 1 < cap)
        out[(*used)++] = *s++;
}

static void decimal(ntfs_u64 value, char *out)
{
    char reverse[24];
    size_t n = 0, i;
    do {
        reverse[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    for (i = 0; i < n; i++)
        out[i] = reverse[n - 1 - i];
    out[n] = 0;
}

static void log_analysis(ntfsmft_log log, void *ctx, ntfs_u64 clusters,
                         int fragments, ntfs_u64 lcn)
{
    char line[160], number[24];
    size_t used = 0;
    if (!log)
        return;
    append(line, sizeof(line), &used, "$MFT: ");
    decimal(clusters, number);
    append(line, sizeof(line), &used, number);
    append(line, sizeof(line), &used, " clusters in ");
    decimal((ntfs_u64)fragments, number);
    append(line, sizeof(line), &used, number);
    append(line, sizeof(line), &used, " fragment(s), starts at LCN ");
    decimal(lcn, number);
    append(line, sizeof(line), &used, number);
    line[used] = 0;
    log(ctx, line);
}

static void log_error(ntfsmft_log log, void *ctx, mft_status st)
{
    char line[160];
    const char *name = ntfsmft_strstatus(st);
    size_t used = 0;
    if (!log)
        return;
    append(line, sizeof(line), &used, "Analyze failed: ");
    append(line, sizeof(line), &used, name);
    line[used] = 0;
    log(ctx, line);
}

/* Read arbitrary bytes while keeping backend requests sector-aligned. */
static int read_bytes(ntfsmft_io *io, ntfs_u64 off, void *buf, ntfs_u32 len)
{
    ntfs_u64 start, end;
    ntfs_u32 span, sector;
    ntfs_u8 *temp;
    int ok;

    if (!io || !io->read || !buf || !len)
        return 0;
    sector = io->sectorSize;
    if (sector < 512 || sector > 4096 || (sector & (sector - 1)) ||
        off > io->length || len > io->length - off ||
        off > ~0ULL - len - sector)
        return 0;
    start = off / sector * sector;
    end = (off + len + sector - 1) / sector * sector;
    if (end > io->length || end - start > 0xFFFFFFFFU)
        return 0;
    span = (ntfs_u32)(end - start);
    if (start == off && span == len)
        return io->read(io->ctx, off, buf, len);
    temp = (ntfs_u8 *)malloc(span);
    if (!temp)
        return 0;
    ok = io->read(io->ctx, start, temp, span);
    if (ok)
        memcpy(buf, temp + (off - start), len);
    free(temp);
    return ok;
}

static mft_status parse_boot(volume *v, ntfsmft_io *io)
{
    ntfs_u32 bps, spc;
    int recordCode;

    if (memcmp(v->boot + 3, "NTFS    ", 8) || rd16(v->boot + 510) != 0xAA55)
        return MFT_NOT_NTFS;
    bps = rd16(v->boot + 0x0B);
    spc = v->boot[0x0D];
    if (bps != io->sectorSize || !spc || (spc & (spc - 1)))
        return MFT_BAD_GEOMETRY;
    v->bytesPerCluster = (ntfs_u64)bps * spc;
    if (v->bytesPerCluster > 65536)
        return MFT_BAD_GEOMETRY;
    v->totalSectors = rd64(v->boot + 0x28);
    v->mftLcn = rd64(v->boot + 0x30);
    v->mirrorLcn = rd64(v->boot + 0x38);
    if (!v->totalSectors || io->length < bps ||
        v->totalSectors > (io->length - bps) / bps ||
        io->length > 0x7FFFFFFFFFFFFFFFULL)
        return MFT_BAD_GEOMETRY;
    v->totalClusters = v->totalSectors / spc;
    recordCode = (signed char)v->boot[0x40];
    if (recordCode > 0 && v->bytesPerCluster <= 65536 / (unsigned)recordCode)
        v->recSize = (ntfs_u32)recordCode * (ntfs_u32)v->bytesPerCluster;
    else if (recordCode <= -9 && recordCode >= -16)
        v->recSize = (ntfs_u32)1 << -recordCode;
    else
        return MFT_BAD_GEOMETRY;
    if (v->recSize < 512 || v->recSize > 65536 ||
        (v->recSize & (v->recSize - 1)) ||
        !v->mftLcn || v->mftLcn >= v->totalClusters ||
        !v->mirrorLcn || v->mirrorLcn >= v->totalClusters ||
        (ntfs_u64)16 * v->recSize >
            (v->totalClusters - v->mftLcn) * v->bytesPerCluster ||
        v->recSize > (v->totalClusters - v->mirrorLcn) * v->bytesPerCluster)
        return MFT_BAD_GEOMETRY;
    return MFT_OK;
}

static mft_status read_boot(volume *v, ntfsmft_io *io)
{
    if (!io || !io->read || !io->sectorSize || io->length < 512)
        return MFT_BAD_GEOMETRY;
    if (!read_bytes(io, 0, v->boot, sizeof(v->boot)))
        return MFT_READ_ERROR;
    v->io = io;
    return parse_boot(v, io);
}

static mft_status read_record(volume *v, ntfs_u64 offset, ntfs_u8 *record)
{
    if (!read_bytes(v->io, offset, record, v->recSize))
        return MFT_READ_ERROR;
    if (memcmp(record, "FILE", 4) || !ntfs_record_unprotect(record, v->recSize) ||
        !ntfs_record_valid(record, v->recSize) ||
        !(rd16(record + 0x16) & 1) || rd64(record + 0x20))
        return MFT_BAD_RECORD;
    return MFT_OK;
}

static mft_status read_mft_record(volume *v, ntfs_u32 index, ntfs_u8 *record)
{
    return read_record(v, v->mftLcn * v->bytesPerCluster +
                            (ntfs_u64)index * v->recSize, record);
}

static int valid_runs(volume *v, const ntfs_run *runs, int count,
                      ntfs_u64 clusterCount)
{
    ntfs_u64 sum = 0;
    int i, j;
    if (!runs || count < 1 || count > MAX_RUNS)
        return 0;
    for (i = 0; i < count; i++) {
        if (!runs[i].lcn || runs[i].lcn >= v->totalClusters ||
            !runs[i].len || runs[i].len > v->totalClusters - runs[i].lcn ||
            sum > clusterCount || runs[i].len > clusterCount - sum)
            return 0;
        sum += runs[i].len;
        for (j = 0; j < i; j++)
            if (runs[i].lcn < runs[j].lcn + runs[j].len &&
                runs[j].lcn < runs[i].lcn + runs[i].len)
                return 0;
    }
    return sum == clusterCount;
}

static mft_status decode_runs(const ntfs_u8 *record, ntfs_u32 recordSize,
                              long attrOffset, ntfs_run *runs, int *countOut,
                              ntfs_u64 *clustersOut)
{
    ntfs_u32 offset, length, pairs;
    const ntfs_u8 *p, *end;
    ntfs_u64 lcn = 0, clusters = 0;
    int count = 0;

    *countOut = 0;
    *clustersOut = 0;
    if (attrOffset < 0 || (ntfs_u64)attrOffset > recordSize ||
        recordSize - (ntfs_u32)attrOffset < 64)
        return MFT_BAD_RECORD;
    offset = (ntfs_u32)attrOffset;
    length = rd32(record + offset + 4);
    pairs = rd16(record + offset + 0x20);
    if (length < 64 || pairs < 0x40 || pairs >= length ||
        length > recordSize - offset || rd64(record + offset + 0x10) != 0)
        return MFT_BAD_RECORD;
    p = record + offset + pairs;
    end = record + offset + length;
    while (p < end && *p) {
        ntfs_u32 lengthBytes = *p & 0x0F, offsetBytes = *p >> 4, i;
        ntfs_u64 run = 0, delta = 0;
        p++;
        if (!lengthBytes || lengthBytes > 8 || offsetBytes > 8 ||
            (ntfs_u32)(end - p) < lengthBytes + offsetBytes)
            return MFT_BAD_RECORD;
        for (i = 0; i < lengthBytes; i++)
            run |= (ntfs_u64)p[i] << (8 * i);
        if (p[lengthBytes - 1] & 0x80)
            return MFT_BAD_RECORD;
        p += lengthBytes;
        if (!run || count >= MAX_RUNS)
            return count >= MAX_RUNS ? MFT_HAS_ATTRLIST : MFT_BAD_RECORD;
        if (offsetBytes) {
            for (i = 0; i < offsetBytes; i++)
                delta |= (ntfs_u64)p[i] << (8 * i);
            if (offsetBytes < 8 && (p[offsetBytes - 1] & 0x80))
                delta |= ~0ULL << (8 * offsetBytes);
            p += offsetBytes;
            if (delta & (1ULL << 63)) {
                ntfs_u64 magnitude = 0 - delta;
                if (magnitude > lcn)
                    return MFT_BAD_RECORD;
                lcn -= magnitude;
            } else {
                if (delta > 0x7FFFFFFFFFFFFFFFULL - lcn)
                    return MFT_BAD_RECORD;
                lcn += delta;
            }
            runs[count].lcn = lcn;
        } else {
            return MFT_SPARSE;
        }
        if (run > 0x7FFFFFFFFFFFFFFFULL - clusters)
            return MFT_BAD_RECORD;
        runs[count].len = run;
        clusters += run;
        count++;
    }
    if (p == end || !count || rd64(record + offset + 0x18) != clusters - 1)
        return MFT_BAD_RECORD;
    *countOut = count;
    *clustersOut = clusters;
    return MFT_OK;
}

static mft_status parse_data_attr(volume *v, const ntfs_u8 *record,
                                  ntfs_run *runs, run_map *map)
{
    int attrList = 0;
    long attr = ntfs_record_attr(record, v->recSize, 0x80, 1, 1, &attrList);
    mft_status st;
    if (attrList)
        return MFT_HAS_ATTRLIST;
    if (attr < 0)
        return MFT_BAD_RECORD;
    if (record[attr + 9] || rd16(record + attr + 12) ||
        rd16(record + attr + 34))
        return MFT_SPARSE;
    st = decode_runs(record, v->recSize, attr, runs, &map->count, &map->clusters);
    if (st != MFT_OK)
        return st;
    map->runs = runs;
    map->allocatedBytes = rd64(record + attr + 0x28);
    map->dataBytes = rd64(record + attr + 0x30);
    map->validBytes = rd64(record + attr + 0x38);
    if (!valid_runs(v, runs, map->count, map->clusters) ||
        map->allocatedBytes != map->clusters * v->bytesPerCluster ||
        map->dataBytes > map->allocatedBytes ||
        map->validBytes > map->dataBytes)
        return MFT_BAD_RECORD;
    return MFT_OK;
}

static int runs_overlap(const ntfs_run *a, int na, const ntfs_run *b, int nb)
{
    int i, j;
    for (i = 0; i < na; i++)
        for (j = 0; j < nb; j++)
            if (a[i].lcn < b[j].lcn + b[j].len &&
                b[j].lcn < a[i].lcn + a[i].len)
                return 1;
    return 0;
}

static ntfs_u64 run_lcn(const ntfs_run *runs, int count, ntfs_u64 vcn,
                        ntfs_u64 *left)
{
    ntfs_u64 base = 0;
    int i;
    for (i = 0; i < count; i++) {
        if (vcn < base + runs[i].len) {
            *left = base + runs[i].len - vcn;
            return runs[i].lcn + vcn - base;
        }
        base += runs[i].len;
    }
    *left = 0;
    return ~0ULL;
}

static int run_read(volume *v, const run_map *map, ntfs_u8 *out,
                    ntfs_u64 bytes)
{
    ntfs_u64 done = 0;
    while (done < bytes) {
        ntfs_u64 left, vcn = done / v->bytesPerCluster;
        ntfs_u64 lcn = run_lcn(map->runs, map->count, vcn, &left);
        ntfs_u64 inCluster = done % v->bytesPerCluster;
        ntfs_u64 take = v->bytesPerCluster - inCluster;
        if (lcn == ~0ULL || !left)
            return 0;
        if (take > bytes - done)
            take = bytes - done;
        if (!read_bytes(v->io, lcn * v->bytesPerCluster + inCluster,
                        out + done, (ntfs_u32)take))
            return 0;
        done += take;
    }
    return 1;
}

static int bitmap_get(const bitmap *b, ntfs_u64 lcn)
{
    ntfs_u64 byte = lcn >> 3;
    return byte >= b->bytes || (b->bits[byte] & (1u << (lcn & 7))) != 0;
}

static int map_allocated(const bitmap *b, const ntfs_run *runs, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        ntfs_u64 lcn;
        for (lcn = runs[i].lcn; lcn < runs[i].lcn + runs[i].len; lcn++)
            if (!bitmap_get(b, lcn))
                return 0;
    }
    return 1;
}

static mft_status load_bitmap(volume *v, bitmap *bm, ntfs_run *runs,
                              const ntfs_u8 *record)
{
    ntfs_u64 required = (v->totalClusters + 7) / 8;
    run_map map;
    mft_status st;
    long attr;
    int attrList = 0;

    memset(&map, 0, sizeof(map));
    attr = ntfs_record_attr(record, v->recSize, 0x80, 1, 1, &attrList);
    if (attrList)
        return MFT_HAS_ATTRLIST;
    if (attr < 0 || rd16(record + attr + 12) || rd16(record + attr + 34))
        return MFT_BAD_RECORD;
    st = decode_runs(record, v->recSize, attr, runs, &map.count, &map.clusters);
    if (st != MFT_OK)
        return st;
    map.runs = runs;
    map.allocatedBytes = rd64(record + attr + 0x28);
    map.dataBytes = rd64(record + attr + 0x30);
    map.validBytes = rd64(record + attr + 0x38);
    if (!valid_runs(v, runs, map.count, map.clusters) ||
        map.allocatedBytes != map.clusters * v->bytesPerCluster ||
        map.dataBytes < required || map.dataBytes > map.allocatedBytes ||
        map.validBytes < required || map.validBytes > map.dataBytes)
        return MFT_BAD_RECORD;
    if (required > (size_t)-1)
        return MFT_NOMEM;
    bm->bits = (ntfs_u8 *)malloc((size_t)required);
    if (!bm->bits)
        return MFT_NOMEM;
    bm->bytes = required;
    bm->runs = runs;
    bm->count = map.count;
    if (!run_read(v, &map, bm->bits, required)) {
        free(bm->bits);
        bm->bits = NULL;
        return MFT_READ_ERROR;
    }
    return MFT_OK;
}

static mft_status check_volume_clean(volume *v, const ntfs_u8 *record)
{
    int attrList = 0;
    long attr = ntfs_record_attr(record, v->recSize, 0x70, 0, 1, &attrList);
    ntfs_u32 attrLength, valueLength, valueOffset;
    ntfs_u8 major, minor;
    if (attrList || attr < 0 || record[attr + 8] != 0)
        return MFT_BAD_RECORD;
    attrLength = rd32(record + attr + 4);
    valueLength = rd32(record + attr + 0x10);
    valueOffset = rd16(record + attr + 0x14);
    if (valueOffset > attrLength || 12 > attrLength - valueOffset ||
        valueLength < 12)
        return MFT_BAD_RECORD;
    major = record[attr + valueOffset + 8];
    minor = record[attr + valueOffset + 9];
    if (!((major == 1 && minor == 2) || (major == 3 && minor <= 1)))
        return MFT_BAD_RECORD;
    if (rd16(record + attr + valueOffset + 0x0A) & ~0x0008U)
        return MFT_VOLUME_DIRTY;
    return MFT_OK;
}

static mft_status analyze(volume *v, ntfsmft_io *io, ntfsmft_info *out)
{
    ntfs_u8 *record = NULL, *mirrorRecord = NULL;
    ntfs_run *mftRuns = NULL, *mirrorRuns = NULL, *bitmapRuns = NULL;
    bitmap bm;
    run_map mftMap, mirrorMap;
    ntfs_run mirrorRun;
    mft_status st;
    int i;
    ntfs_u64 mirrorClusters;

    memset(&bm, 0, sizeof(bm));
    memset(&mftMap, 0, sizeof(mftMap));
    memset(&mirrorMap, 0, sizeof(mirrorMap));
    st = read_boot(v, io);
    if (st != MFT_OK)
        return st;
    record = (ntfs_u8 *)malloc(v->recSize);
    mirrorRecord = (ntfs_u8 *)malloc(v->recSize);
    mftRuns = (ntfs_run *)malloc(MAX_RUNS * sizeof(*mftRuns));
    mirrorRuns = (ntfs_run *)malloc(MAX_RUNS * sizeof(*mirrorRuns));
    bitmapRuns = (ntfs_run *)malloc(MAX_RUNS * sizeof(*bitmapRuns));
    if (!record || !mirrorRecord || !mftRuns || !mirrorRuns || !bitmapRuns) {
        st = MFT_NOMEM;
        goto done;
    }

    st = read_mft_record(v, 0, record);
    if (st != MFT_OK)
        goto done;
    st = parse_data_attr(v, record, mftRuns, &mftMap);
    if (st != MFT_OK)
        goto done;
    if (mftRuns[0].lcn != v->mftLcn ||
        mftRuns[0].len * v->bytesPerCluster < (ntfs_u64)16 * v->recSize ||
        mftMap.allocatedBytes != mftMap.clusters * v->bytesPerCluster ||
        mftMap.dataBytes > mftMap.allocatedBytes ||
        mftMap.validBytes < (ntfs_u64)16 * v->recSize) {
        st = MFT_BAD_RECORD;
        goto done;
    }

    st = read_mft_record(v, 3, record);
    if (st != MFT_OK)
        goto done;
    st = check_volume_clean(v, record);
    if (st != MFT_OK)
        goto done;
    st = read_mft_record(v, 6, record);
    if (st != MFT_OK)
        goto done;
    st = load_bitmap(v, &bm, bitmapRuns, record);
    if (st != MFT_OK)
        goto done;

    st = read_record(v, v->mirrorLcn * v->bytesPerCluster, mirrorRecord);
    if (st != MFT_OK) {
        if (st == MFT_BAD_RECORD)
            st = MFT_MIRROR_MISMATCH;
        goto done;
    }
    st = parse_data_attr(v, mirrorRecord, mirrorRuns, &mirrorMap);
    if (st != MFT_OK) {
        st = MFT_MIRROR_MISMATCH;
        goto done;
    }
    if (mftMap.count != mirrorMap.count) {
        st = MFT_MIRROR_MISMATCH;
        goto done;
    }
    for (i = 0; i < mftMap.count; i++)
        if (mftRuns[i].lcn != mirrorRuns[i].lcn ||
            mftRuns[i].len != mirrorRuns[i].len)
            break;
    if (i != mftMap.count) {
        st = MFT_MIRROR_MISMATCH;
        goto done;
    }

    mirrorClusters = (v->recSize + v->bytesPerCluster - 1) /
                     v->bytesPerCluster;
    mirrorRun.lcn = v->mirrorLcn;
    mirrorRun.len = mirrorClusters;
    if (!bitmap_get(&bm, 0) || !map_allocated(&bm, mftRuns, mftMap.count) ||
        !map_allocated(&bm, bitmapRuns, bm.count) ||
        !map_allocated(&bm, &mirrorRun, 1) ||
        runs_overlap(mftRuns, mftMap.count, bitmapRuns, bm.count) ||
        runs_overlap(mftRuns, mftMap.count, &mirrorRun, 1) ||
        runs_overlap(bitmapRuns, bm.count, &mirrorRun, 1)) {
        st = MFT_BAD_RECORD;
        goto done;
    }

    out->mftLcn = v->mftLcn;
    out->recSize = v->recSize;
    out->bytesPerCluster = v->bytesPerCluster;
    out->totalClusters = v->totalClusters;
    out->mftClusters = mftMap.clusters;
    out->mftBytes = mftMap.allocatedBytes;
    out->fragments = mftMap.count;
    st = MFT_OK;
done:
    free(bm.bits);
    free(bitmapRuns);
    free(mirrorRuns);
    free(mftRuns);
    free(mirrorRecord);
    free(record);
    return st;
}

mft_status ntfsmft_analyze(ntfsmft_io *io, ntfsmft_info *out,
                           ntfsmft_log log, void *logctx)
{
    volume v;
    ntfsmft_info local;
    mft_status st;
    if (!out)
        out = &local;
    memset(out, 0, sizeof(*out));
    memset(&v, 0, sizeof(v));
    st = analyze(&v, io, out);
    if (st == MFT_OK)
        log_analysis(log, logctx, out->mftClusters, out->fragments, out->mftLcn);
    else
        log_error(log, logctx, st);
    return st;
}

const char *ntfsmft_strstatus(mft_status st)
{
    switch (st) {
    case MFT_OK:               return "ok";
    case MFT_READ_ERROR:       return "read error";
    case MFT_NOT_NTFS:         return "not an NTFS volume";
    case MFT_BAD_GEOMETRY:     return "bad NTFS geometry";
    case MFT_BAD_RECORD:       return "corrupt NTFS metadata";
    case MFT_HAS_ATTRLIST:     return "metadata uses an attribute list";
    case MFT_SPARSE:           return "metadata contains sparse/compressed data";
    case MFT_VOLUME_DIRTY:     return "volume is marked dirty (run chkdsk)";
    case MFT_MIRROR_MISMATCH:  return "$MFTMirr mismatch";
    case MFT_NOMEM:            return "out of memory";
    }
    return "unknown error";
}

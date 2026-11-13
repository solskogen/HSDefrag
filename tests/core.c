#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "ntfsmft.c"
#include "bootcfg.h"
#include "ntfsrecord.h"

#define SECTOR 512U
#define DISK_BYTES (16U * 1024U * 1024U)
#define MFT_LCN 32U
#define MFT_CLUSTERS 64U
#define MIRROR_LCN 1024U
#define BITMAP_LCN 1000U
#define BITMAP_CLUSTERS 8U

typedef struct {
    ntfs_u8 *bytes;
    size_t length;
    unsigned reads;
    int failRead;
    int denyTail;
} memory_disk;

static unsigned checks;

#define CHECK(test) do { \
    checks++; \
    if (!(test)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

static void put16(ntfs_u8 *p, unsigned value)
{
    p[0] = (ntfs_u8)value;
    p[1] = (ntfs_u8)(value >> 8);
}

static void put32(ntfs_u8 *p, ntfs_u32 value)
{
    unsigned i;
    for (i = 0; i < 4; i++)
        p[i] = (ntfs_u8)(value >> (8 * i));
}

static void put64(ntfs_u8 *p, ntfs_u64 value)
{
    put32(p, (ntfs_u32)value);
    put32(p + 4, (ntfs_u32)(value >> 32));
}

static void set_record_header(ntfs_u8 *record)
{
    memset(record, 0, 1024);
    memcpy(record, "FILE", 4);
    put16(record + 4, 0x2A);
    put16(record + 6, 3);
    put16(record + 0x10, 1);
    put16(record + 0x14, 0x30);
    put16(record + 0x16, 1);
    put32(record + 0x1C, 1024);
}

static void finish_record(ntfs_u8 *record, ntfs_u32 end)
{
    put32(record + end, 0xFFFFFFFFU);
    put32(record + 0x18, end + 8);
    CHECK(ntfs_record_protect(record, 1024));
}

static void mapping_attr(ntfs_u8 *record, ntfs_u32 attr, ntfs_u64 lcn,
                         ntfs_u64 clusters, ntfs_u64 bytes)
{
    ntfs_u8 *a = record + attr;
    ntfs_u32 at = attr + 64, end;
    unsigned lenBytes = clusters <= 0xFF ? 1 : 2;
    unsigned lcnBytes = lcn <= 0x7F ? 1 : lcn <= 0x7FFF ? 2 : 4;
    a[0] = 0x80;
    put32(a + 4, 72);
    a[8] = 1;
    put64(a + 0x18, clusters - 1);
    put16(a + 0x20, 64);
    put64(a + 0x28, clusters * SECTOR);
    put64(a + 0x30, bytes);
    put64(a + 0x38, bytes);
    record[at++] = (ntfs_u8)((lcnBytes << 4) | lenBytes);
    {
        unsigned i;
        for (i = 0; i < lenBytes; i++)
            record[at++] = (ntfs_u8)(clusters >> (8 * i));
        for (i = 0; i < lcnBytes; i++)
            record[at++] = (ntfs_u8)(lcn >> (8 * i));
    }
    record[at++] = 0;
    end = attr + 72;
    finish_record(record, end);
}

static void mft_record(ntfs_u8 *record, int fragmented)
{
    ntfs_u8 *a;
    ntfs_u32 at;
    set_record_header(record);
    a = record + 0x30;
    a[0] = 0x80;
    put32(a + 4, fragmented ? 80 : 72);
    a[8] = 1;
    put64(a + 0x18, MFT_CLUSTERS - 1);
    put16(a + 0x20, 64);
    put64(a + 0x28, MFT_CLUSTERS * SECTOR);
    put64(a + 0x30, MFT_CLUSTERS * SECTOR);
    put64(a + 0x38, MFT_CLUSTERS * SECTOR);
    at = 0x30 + 64;
    record[at++] = 0x11;
    record[at++] = fragmented ? 32 : MFT_CLUSTERS;
    record[at++] = MFT_LCN;
    if (fragmented) {
        record[at++] = 0x21;
        record[at++] = 32;
        record[at++] = (ntfs_u8)(2000 - MFT_LCN);
        record[at++] = (ntfs_u8)((2000 - MFT_LCN) >> 8);
    }
    record[at++] = 0;
    finish_record(record, 0x30 + (fragmented ? 80 : 72));
}

static void volume_record(ntfs_u8 *record, int dirty)
{
    ntfs_u8 *a;
    set_record_header(record);
    a = record + 0x30;
    a[0] = 0x70;
    put32(a + 4, 40);
    put32(a + 0x10, 12);
    put16(a + 0x14, 24);
    a[0x20] = 3;
    a[0x21] = 1;
    put16(a + 0x22, dirty ? 1 : 8);
    finish_record(record, 0x30 + 40);
}

static int disk_read(void *ctx, ntfs_u64 off, void *buf, ntfs_u32 len)
{
    memory_disk *disk = (memory_disk *)ctx;
    disk->reads++;
    if (disk->denyTail && off + len > disk->length - SECTOR)
        return 0;
    if (disk->failRead) {
        disk->failRead = 0;
        disk->denyTail = 0;
        return 0;
    }
    if (off % SECTOR || len % SECTOR || off > disk->length ||
        len > disk->length - off)
        return 0;
    memcpy(buf, disk->bytes + off, len);
    return 1;
}

static ntfsmft_io disk_io(memory_disk *disk)
{
    ntfsmft_io io;
    io.ctx = disk;
    io.read = disk_read;
    io.sectorSize = SECTOR;
    io.length = disk->length;
    return io;
}

static void mark_used(ntfs_u8 *bitmap, ntfs_u64 lcn, ntfs_u64 length)
{
    ntfs_u64 i;
    for (i = lcn; i < lcn + length; i++)
        bitmap[i >> 3] |= (ntfs_u8)(1u << (i & 7));
}

static void fixture(memory_disk *disk, int fragmented, int dirty)
{
    ntfs_u8 *boot = disk->bytes;
    ntfs_u8 *mft = boot + MFT_LCN * SECTOR;
    ntfs_u8 *bitmap = boot + BITMAP_LCN * SECTOR;
    ntfs_u64 totalSectors = disk->length / SECTOR - 1;
    unsigned i;

    memset(disk->bytes, 0, disk->length);
    memcpy(boot + 3, "NTFS    ", 8);
    put16(boot + 11, SECTOR);
    boot[13] = 1;
    put64(boot + 0x28, totalSectors);
    put64(boot + 0x30, MFT_LCN);
    put64(boot + 0x38, MIRROR_LCN);
    boot[0x40] = (ntfs_u8)-10;
    put16(boot + 510, 0xAA55);
    memcpy(boot + totalSectors * SECTOR, boot, SECTOR);

    for (i = 0; i < MFT_CLUSTERS * SECTOR; i++)
        mft[i] = (ntfs_u8)(i * 7);
    mft_record(mft, fragmented);
    volume_record(mft + 3 * 1024, dirty);
    set_record_header(mft + 6 * 1024);
    mapping_attr(mft + 6 * 1024, 0x30, BITMAP_LCN,
                 BITMAP_CLUSTERS, BITMAP_CLUSTERS * SECTOR);
    memcpy(boot + MIRROR_LCN * SECTOR, mft, 1024);

    mark_used(bitmap, 0, 1);
    if (fragmented) {
        mark_used(bitmap, MFT_LCN, 32);
        mark_used(bitmap, 2000, 32);
    } else {
        mark_used(bitmap, MFT_LCN, MFT_CLUSTERS);
    }
    mark_used(bitmap, BITMAP_LCN, BITMAP_CLUSTERS);
    mark_used(bitmap, MIRROR_LCN, 1024 / SECTOR);
    disk->reads = 0;
    disk->failRead = 0;
}

static void record_tests(void)
{
    ntfs_u8 record[1024];
    set_record_header(record);
    put32(record + 0x30, 0xFFFFFFFFU);
    put32(record + 0x18, 0x38);
    CHECK(ntfs_record_protect(record, sizeof(record)));
    CHECK(ntfs_record_unprotect(record, sizeof(record)));
    CHECK(ntfs_record_valid(record, sizeof(record)) &&
          record[510] == 0 && record[1022] == 0);
    CHECK(ntfs_record_protect(record, sizeof(record)));
    record[1022] ^= 1;
    CHECK(!ntfs_record_unprotect(record, sizeof(record)));
}

static void analyze_tests(void)
{
    memory_disk disk;
    ntfsmft_io io;
    ntfsmft_info info;
    disk.length = DISK_BYTES;
    disk.bytes = (ntfs_u8 *)malloc(disk.length);
    CHECK(disk.bytes != NULL);

    fixture(&disk, 0, 0);
    io = disk_io(&disk);
    CHECK(ntfsmft_analyze(&io, &info, NULL, NULL) == MFT_OK);
    CHECK(info.mftLcn == MFT_LCN && info.recSize == 1024 &&
          info.bytesPerCluster == SECTOR && info.mftClusters == MFT_CLUSTERS &&
          info.fragments == 1 && info.totalClusters == DISK_BYTES / SECTOR - 1);
    CHECK(disk.reads > 0);
    disk.denyTail = 1;
    CHECK(ntfsmft_analyze(&io, &info, NULL, NULL) == MFT_OK);
    CHECK(info.fragments == 1);

    fixture(&disk, 1, 0);
    io = disk_io(&disk);
    CHECK(ntfsmft_analyze(&io, &info, NULL, NULL) == MFT_OK);
    CHECK(info.fragments == 2 && info.mftClusters == MFT_CLUSTERS);

    fixture(&disk, 0, 1);
    io = disk_io(&disk);
    CHECK(ntfsmft_analyze(&io, &info, NULL, NULL) == MFT_VOLUME_DIRTY);

    fixture(&disk, 0, 0);
    CHECK(ntfs_record_unprotect(disk.bytes + MIRROR_LCN * SECTOR, 1024));
    disk.bytes[MIRROR_LCN * SECTOR + 0x30 + 66] ^= 1;
    CHECK(ntfs_record_protect(disk.bytes + MIRROR_LCN * SECTOR, 1024));
    io = disk_io(&disk);
    CHECK(ntfsmft_analyze(&io, &info, NULL, NULL) == MFT_MIRROR_MISMATCH);

    fixture(&disk, 0, 0);
    disk.bytes[3] = 'F';
    io = disk_io(&disk);
    CHECK(ntfsmft_analyze(&io, &info, NULL, NULL) == MFT_NOT_NTFS);

    fixture(&disk, 0, 0);
    io = disk_io(&disk);
    disk.failRead = 1;
    CHECK(ntfsmft_analyze(&io, &info, NULL, NULL) == MFT_READ_ERROR);

    free(disk.bytes);
}

static void bootcfg_tests(void)
{
    static const wchar_t entries[] =
        L"autocheck autochk *\0hsdefrgb C: /x /f\0hsdefrgb D: /x\0";
    static const wchar_t expected[] =
        L"autocheck autochk *\0hsdefrgb D: /x\0";
    wchar_t drive, *edited;
    int dryRun, writeAuthorized, full;
    size_t chars;
    CHECK(bootcfg_parse(L"hsdefrgb C: /x /f", 17, &drive, &dryRun,
                        &writeAuthorized, &full));
    CHECK(drive == L'C' && !dryRun && writeAuthorized && full);
    CHECK(!bootcfg_parse(L"hsdefrgb C: /x /x", 17, &drive, &dryRun,
                         &writeAuthorized, &full));
    CHECK(!bootcfg_parse(L"hsdefrgb C: /unknown", 20, &drive, &dryRun,
                         &writeAuthorized, &full));
    edited = bootcfg_edit(entries, sizeof(entries) / sizeof(*entries),
                          L'C', NULL, &chars);
    CHECK(edited && chars == sizeof(expected) / sizeof(*expected) &&
          !memcmp(edited, expected, sizeof(expected)));
    free(edited);

    /* Quoted, path-qualified and ".exe" forms of the helper are ours too;
     * a lower-case drive letter matches; other programs that merely
     * mention the drive stay. */
    {
        static const wchar_t in[] =
            L"autocheck autochk *\0"
            L"\"C:\\WINNT\\System32\\hsdefrgb.exe\" c: /x /f\0"
            L"System32\\HSDEFRGB.EXE C: /x\0"
            L"hsdefrgbx C: /x\0"
            L"other C: /x\0";
        static const wchar_t out[] =
            L"autocheck autochk *\0hsdefrgbx C: /x\0other C: /x\0";
        edited = bootcfg_edit(in, sizeof(in) / sizeof(*in), L'c', NULL, &chars);
        CHECK(edited && chars == sizeof(out) / sizeof(*out) &&
              !memcmp(edited, out, sizeof(out)));
        free(edited);
    }
    /* Adding replaces this drive's entry and appends the new one last. */
    {
        static const wchar_t in[] = L"hsdefrgb C: /x\0autocheck autochk *\0";
        static const wchar_t out[] =
            L"autocheck autochk *\0hsdefrgb C: /x /f\0";
        edited = bootcfg_edit(in, sizeof(in) / sizeof(*in), L'C',
                              L"hsdefrgb C: /x /f", &chars);
        CHECK(edited && chars == sizeof(out) / sizeof(*out) &&
              !memcmp(edited, out, sizeof(out)));
        free(edited);
    }
    /* Removing the only entry leaves a valid empty MULTI_SZ. */
    {
        static const wchar_t in[] = L"hsdefrgb C: /x\0";
        edited = bootcfg_edit(in, sizeof(in) / sizeof(*in), L'C', NULL, &chars);
        CHECK(edited && chars == 2 && !edited[0] && !edited[1]);
        free(edited);
    }
    /* Malformed lists are refused rather than rewritten: a single final
     * NUL, an empty string followed by more data, an invalid drive. */
    {
        static const wchar_t one[] = { L'a', L'u', L't', L'o', 0 };
        static const wchar_t gap[] = L"autocheck autochk *\0\0hsdefrgb C: /x\0";
        CHECK(!bootcfg_edit(one, 5, L'C', NULL, &chars) && chars == 0);
        CHECK(!bootcfg_edit(gap, sizeof(gap) / sizeof(*gap), L'C', NULL,
                            &chars));
        CHECK(!bootcfg_edit(entries, sizeof(entries) / sizeof(*entries),
                            L'1', NULL, &chars));
    }
}

int main(void)
{
    record_tests();
    analyze_tests();
    bootcfg_tests();
    printf("Read-only NTFS/core parser regressions: %u checks.\n", checks);
    return 0;
}

/*
 * fat.c - Host unit tests for the portable FAT engine (fatfs.c / fatdefrag.c).
 *
 * Builds small in-memory FAT16 and FAT32 images, fragments a file, then checks
 * that analysis measures the fragmentation and that the defragmenter relocates
 * the file into a single contiguous run without altering its data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fatfs.h"
#include "fatdefrag.h"

static unsigned checks;

#define CHECK(test) do { \
    checks++; \
    if (!(test)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

/* ---------------------------------------------------------- memory device */

typedef struct {
    unsigned char *bytes;
    fat_u32        sectors;
    fat_u32        bps;
    /* Optional checks and fault injection (off while zero). */
    const int     *inSec;        /* every write must happen while set     */
    unsigned long  outside;      /* writes made outside a write section   */
    unsigned long  reads, writes;
    unsigned long  failWriteAt;  /* fail this write (1-based) ...         */
    int            failWriteAll; /* ... and every write after it          */
    unsigned       readFailPct;  /* fail this share of reads (percent)    */
    fat_u32        failReadSector; /* sector + 1, or zero when disabled   */
    unsigned long  failReadAfter;  /* ... only once this many writes ran  */
    fat_u32        failWriteSector; /* sector + 1: fail the next write that
                                       covers it, once                    */
    int            dropWrite;   /* acknowledge failed writes unchanged  */
    int            lostWrite;   /* a dropped write would change bytes  */
    unsigned       rng;
} mem_disk;

static unsigned rnd(unsigned *s)
{
    unsigned x = *s ? *s : 0x9E3779B9u;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static int mem_read(void *ctx, fat_u32 sector, fat_u32 count, void *buf)
{
    mem_disk *d = (mem_disk *)ctx;

    d->reads++;
    if (d->failReadSector && d->writes >= d->failReadAfter &&
        sector < d->failReadSector &&
        (fat_u64)sector + count >= d->failReadSector)
        return 0;
    if (d->readFailPct && rnd(&d->rng) % 100 < d->readFailPct)
        return 0;
    if ((fat_u64)sector + count > d->sectors)
        return 0;
    memcpy(buf, d->bytes + (fat_u64)sector * d->bps, (size_t)count * d->bps);
    return 1;
}

static int mem_write(void *ctx, fat_u32 sector, fat_u32 count, const void *buf)
{
    mem_disk *d = (mem_disk *)ctx;

    d->writes++;
    if (d->inSec && !*d->inSec)
        d->outside++;
    if ((fat_u64)sector + count > d->sectors)
        return 0;
    if (d->failWriteSector && sector < d->failWriteSector &&
        (fat_u64)sector + count >= d->failWriteSector) {
        d->failWriteSector = 0;
        return 0;
    }
    if (d->failWriteAt && (d->writes == d->failWriteAt ||
                           (d->failWriteAll && d->writes > d->failWriteAt))) {
        if (d->dropWrite &&
            memcmp(d->bytes + (fat_u64)sector * d->bps, buf,
                   (size_t)count * d->bps) != 0)
            d->lostWrite = 1;
        return d->dropWrite;
    }
    memcpy(d->bytes + (fat_u64)sector * d->bps, buf, (size_t)count * d->bps);
    return 1;
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, fat_u32 v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* Raw FAT entry poke directly in the image (test setup helper). */
static void img_set_fat(mem_disk *d, int type, fat_u32 fatStart, fat_u32 cl,
                        fat_u32 val)
{
    unsigned char *fat = d->bytes + (fat_u64)fatStart * d->bps;
    if (type == 16) {
        put16(fat + cl * 2, val & 0xFFFF);
    } else { /* 32 */
        fat_u32 cur = fat[cl * 4] | (fat[cl * 4 + 1] << 8) |
                      (fat[cl * 4 + 2] << 16) | (fat[cl * 4 + 3] << 24);
        fat_u32 nv = (cur & 0xF0000000u) | (val & 0x0FFFFFFFu);
        put32(fat + cl * 4, nv);
    }
}

static void fill_cluster(mem_disk *d, fat_u32 firstDataSector, fat_u32 spc,
                         fat_u32 cl, unsigned char pattern)
{
    fat_u32 s = firstDataSector + (cl - 2) * spc;
    memset(d->bytes + (fat_u64)s * d->bps, pattern, (size_t)spc * d->bps);
}

static int cluster_is(mem_disk *d, fat_u32 firstDataSector, fat_u32 spc,
                      fat_u32 cl, unsigned char pattern)
{
    fat_u32 s = firstDataSector + (cl - 2) * spc;
    size_t i, n = (size_t)spc * d->bps;
    unsigned char *p = d->bytes + (fat_u64)s * d->bps;
    for (i = 0; i < n; i++)
        if (p[i] != pattern)
            return 0;
    return 1;
}

static void write_name(unsigned char *e, const char *n83)
{
    int i;
    memset(e, ' ', 11);
    for (i = 0; i < 11 && n83[i]; i++)
        e[i] = (unsigned char)n83[i];
}

/* ------------------------------------------------------------- FAT16 test */

static void test_fat16(void)
{
    mem_disk d;
    fat_volume fv;
    fat_dev dev;
    fat_defrag_cfg cfg;
    fat_stats st;
    const fat_u32 bps = 512, spc = 1, reserved = 1, numFats = 2;
    const fat_u32 rootEnt = 512, fatSize = 20, countClusters = 5000;
    fat_u32 rootDirSectors = (rootEnt * 32 + bps - 1) / bps;
    fat_u32 firstDataSector = reserved + numFats * fatSize + rootDirSectors;
    fat_u32 totalSectors = firstDataSector + countClusters * spc;
    fat_u32 fatStart = reserved, rootStart = reserved + numFats * fatSize;
    unsigned char *boot, *e;
    fat_u32 fragClusters[4] = {10, 20, 11, 30};
    unsigned char pat[4] = {0xA0, 0xB1, 0xC2, 0xD3};
    int i;

    memset(&d, 0, sizeof(d));
    d.bps = bps;
    d.sectors = totalSectors;
    d.bytes = (unsigned char *)calloc((size_t)totalSectors, bps);
    CHECK(d.bytes != NULL);

    /* Boot sector / BPB. */
    boot = d.bytes;
    put16(boot + 0x0B, bps);
    boot[0x0D] = (unsigned char)spc;
    put16(boot + 0x0E, reserved);
    boot[0x10] = (unsigned char)numFats;
    put16(boot + 0x11, rootEnt);
    put16(boot + 0x13, totalSectors);
    put16(boot + 0x16, fatSize);
    put16(boot + 510, 0xAA55);

    /* Reserved FAT entries + fragmented chain 10->20->11->30. */
    img_set_fat(&d, 16, fatStart, 0, 0xFFF8);
    img_set_fat(&d, 16, fatStart, 1, 0xFFFF);
    img_set_fat(&d, 16, fatStart, 10, 20);
    img_set_fat(&d, 16, fatStart, 20, 11);
    img_set_fat(&d, 16, fatStart, 11, 30);
    img_set_fat(&d, 16, fatStart, 30, 0xFFFF);
    for (i = 0; i < 4; i++)
        fill_cluster(&d, firstDataSector, spc, fragClusters[i], pat[i]);
    memcpy(d.bytes + (fat_u64)(fatStart + fatSize) * bps,
           d.bytes + (fat_u64)fatStart * bps, (size_t)fatSize * bps);

    /* Root directory entry for TEST.BIN. */
    e = d.bytes + (fat_u64)rootStart * bps;
    write_name(e, "TEST    BIN");
    e[0x0B] = 0x20;
    put16(e + 0x1A, 10);
    put32(e + 0x1C, 4 * bps);

    /* Mount and analyze. */
    dev.ctx = &d;
    dev.read = mem_read;
    dev.write = mem_write;
    dev.bytesPerSector = bps;
    CHECK(fat_mount(&fv, &dev));
    CHECK(fv.fatType == FAT_TYPE_16);
    CHECK(fv.countOfClusters == countClusters);
    CHECK(fv.firstDataSector == firstDataSector);

    memset(&cfg, 0, sizeof(cfg));
    cfg.doMove = 0;
    CHECK(fat_run(&fv, &cfg, &st) == 0); /* analyze only: no errors */
    CHECK(st.filesExamined == 1);
    CHECK(st.fragmented == 1);
    CHECK(st.totalFragments == 4);
    CHECK(st.defragged == 0);

    /* Defragment. */
    cfg.doMove = 1;
    cfg.verify = 1;
    CHECK(fat_run(&fv, &cfg, &st) == 0);
    CHECK(st.fragmented == 1);
    CHECK(st.defragged == 1);
    CHECK(st.clustersMoved == 4);

    /* New chain should be contiguous 2,3,4,5 with original data in order. */
    CHECK(fat_get(&fv, 2) == 3);
    CHECK(fat_get(&fv, 3) == 4);
    CHECK(fat_get(&fv, 4) == 5);
    CHECK(fat_is_eoc(&fv, fat_get(&fv, 5)));
    CHECK(fat_is_free(fat_get(&fv, 10)));
    CHECK(fat_is_free(fat_get(&fv, 20)));
    for (i = 0; i < 4; i++)
        CHECK(cluster_is(&d, firstDataSector, spc, 2 + i, pat[i]));

    /* Directory entry now points at cluster 2. */
    e = d.bytes + (fat_u64)rootStart * bps;
    CHECK((e[0x1A] | (e[0x1B] << 8)) == 2);

    /* Re-analyze: fully contiguous now. */
    cfg.doMove = 0;
    CHECK(fat_run(&fv, &cfg, &st) == 0);
    CHECK(st.fragmented == 0);
    CHECK(st.contiguous == 1);

    fat_unmount(&fv);
    free(d.bytes);
}

/* ------------------------------------------------------------- FAT32 test */

static void test_fat32(void)
{
    mem_disk d;
    fat_volume fv;
    fat_dev dev;
    fat_defrag_cfg cfg;
    fat_stats st;
    const fat_u32 bps = 512, spc = 1, reserved = 32, numFats = 2;
    const fat_u32 fatSize = 520, countClusters = 65530, rootClus = 2;
    fat_u32 firstDataSector = reserved + numFats * fatSize;
    fat_u32 totalSectors = firstDataSector + countClusters * spc;
    fat_u32 fatStart = reserved;
    unsigned char *boot, *e;
    fat_u32 fragClusters[4] = {10, 20, 11, 30};
    unsigned char pat[4] = {0x11, 0x22, 0x33, 0x44};
    int i;

    memset(&d, 0, sizeof(d));
    d.bps = bps;
    d.sectors = totalSectors;
    d.bytes = (unsigned char *)calloc((size_t)totalSectors, bps);
    CHECK(d.bytes != NULL);

    boot = d.bytes;
    put16(boot + 0x0B, bps);
    boot[0x0D] = (unsigned char)spc;
    put16(boot + 0x0E, reserved);
    boot[0x10] = (unsigned char)numFats;
    put16(boot + 0x11, 0);            /* rootEntCnt = 0 for FAT32 */
    put16(boot + 0x13, 0);            /* totSec16 = 0 */
    put16(boot + 0x16, 0);            /* fatSz16 = 0 */
    put32(boot + 0x20, totalSectors); /* totSec32 */
    put32(boot + 0x24, fatSize);      /* fatSz32 */
    put32(boot + 0x2C, rootClus);     /* root cluster */
    put16(boot + 0x30, 1);            /* FSInfo sector */
    put16(boot + 510, 0xAA55);

    /* FSInfo signatures so invalidation path is exercised. */
    put32(d.bytes + 1 * bps + 0, 0x41615252u);
    put32(d.bytes + 1 * bps + 484, 0x61417272u);
    put16(d.bytes + 1 * bps + 510, 0xAA55);

    img_set_fat(&d, 32, fatStart, 0, 0x0FFFFFF8);
    img_set_fat(&d, 32, fatStart, 1, 0x0FFFFFFF);
    img_set_fat(&d, 32, fatStart, rootClus, 0x0FFFFFFF); /* root: 1 cluster */
    img_set_fat(&d, 32, fatStart, 10, 20);
    img_set_fat(&d, 32, fatStart, 20, 11);
    img_set_fat(&d, 32, fatStart, 11, 30);
    img_set_fat(&d, 32, fatStart, 30, 0x0FFFFFFF);
    for (i = 0; i < 4; i++)
        fill_cluster(&d, firstDataSector, spc, fragClusters[i], pat[i]);
    memcpy(d.bytes + (fat_u64)(fatStart + fatSize) * bps,
           d.bytes + (fat_u64)fatStart * bps, (size_t)fatSize * bps);

    /* File entry lives in the root cluster (cluster 2). */
    e = d.bytes + (fat_u64)firstDataSector * bps;
    write_name(e, "FRAG    DAT");
    e[0x0B] = 0x20;
    put16(e + 0x1A, 10);
    put16(e + 0x14, 0);
    put32(e + 0x1C, 4 * bps);

    dev.ctx = &d;
    dev.read = mem_read;
    dev.write = mem_write;
    dev.bytesPerSector = bps;
    CHECK(fat_mount(&fv, &dev));
    CHECK(fv.fatType == FAT_TYPE_32);
    CHECK(fv.rootCluster == rootClus);

    memset(&cfg, 0, sizeof(cfg));
    cfg.doMove = 1;
    cfg.verify = 1;
    CHECK(fat_run(&fv, &cfg, &st) == 0);
    CHECK(st.filesExamined == 1);
    CHECK(st.defragged == 1);
    CHECK(st.clustersMoved == 4);

    /* First-fit skips the used root cluster 2, so the file lands at 3,4,5,6. */
    CHECK(fat_get(&fv, 3) == 4);
    CHECK(fat_get(&fv, 4) == 5);
    CHECK(fat_get(&fv, 5) == 6);
    CHECK(fat_is_eoc(&fv, fat_get(&fv, 6)));
    for (i = 0; i < 4; i++)
        CHECK(cluster_is(&d, firstDataSector, spc, 3 + i, pat[i]));

    e = d.bytes + (fat_u64)firstDataSector * bps;
    CHECK((e[0x1A] | (e[0x1B] << 8)) == 3);
    CHECK((e[0x14] | (e[0x15] << 8)) == 0);

    /* FSInfo free count should have been invalidated. */
    CHECK(d.bytes[1 * bps + 488] == 0xFF && d.bytes[1 * bps + 491] == 0xFF);

    fat_unmount(&fv);
    free(d.bytes);
}

/* ----------------------------- FAT32 with mirroring disabled (active FAT) */

/* When BPB_ExtFlags disables mirroring, a single FAT is live. The engine must
 * read and write that active FAT, not the (possibly stale) FAT #0. Here FAT #0
 * is left describing a contiguous file while the active FAT #1 describes the
 * same file as fragmented; analysis must see the fragmentation. */
static void test_fat32_active_fat(void)
{
    mem_disk d;
    fat_volume fv;
    fat_dev dev;
    fat_defrag_cfg cfg;
    fat_stats st;
    const fat_u32 bps = 512, spc = 1, reserved = 32, numFats = 2;
    const fat_u32 fatSize = 520, countClusters = 65530, rootClus = 2;
    fat_u32 firstDataSector = reserved + numFats * fatSize;
    fat_u32 totalSectors = firstDataSector + countClusters * spc;
    fat_u32 fat0 = reserved, fat1 = reserved + fatSize;
    unsigned char *boot, *e;
    fat_u32 fragClusters[4] = {10, 20, 11, 30};
    unsigned char pat[4] = {0x55, 0x66, 0x77, 0x88};
    int i;

    memset(&d, 0, sizeof(d));
    d.bps = bps;
    d.sectors = totalSectors;
    d.bytes = (unsigned char *)calloc((size_t)totalSectors, bps);
    CHECK(d.bytes != NULL);

    boot = d.bytes;
    put16(boot + 0x0B, bps);
    boot[0x0D] = (unsigned char)spc;
    put16(boot + 0x0E, reserved);
    boot[0x10] = (unsigned char)numFats;
    put16(boot + 0x11, 0);
    put16(boot + 0x13, 0);
    put16(boot + 0x16, 0);
    put32(boot + 0x20, totalSectors);
    put32(boot + 0x24, fatSize);
    put16(boot + 0x28, 0x0081);        /* ExtFlags: no mirror, active FAT #1 */
    put32(boot + 0x2C, rootClus);
    put16(boot + 0x30, 0);             /* no FSInfo */
    put16(boot + 510, 0xAA55);

    /* Stale FAT #0: same file described as a single contiguous cluster. */
    img_set_fat(&d, 32, fat0, 0, 0x0FFFFFF8);
    img_set_fat(&d, 32, fat0, 1, 0x0FFFFFFF);
    img_set_fat(&d, 32, fat0, rootClus, 0x0FFFFFFF);
    img_set_fat(&d, 32, fat0, 10, 0x0FFFFFFF);

    /* Active FAT #1: the real, fragmented chain 10->20->11->30. */
    img_set_fat(&d, 32, fat1, 0, 0x0FFFFFF8);
    img_set_fat(&d, 32, fat1, 1, 0x0FFFFFFF);
    img_set_fat(&d, 32, fat1, rootClus, 0x0FFFFFFF);
    img_set_fat(&d, 32, fat1, 10, 20);
    img_set_fat(&d, 32, fat1, 20, 11);
    img_set_fat(&d, 32, fat1, 11, 30);
    img_set_fat(&d, 32, fat1, 30, 0x0FFFFFFF);
    for (i = 0; i < 4; i++)
        fill_cluster(&d, firstDataSector, spc, fragClusters[i], pat[i]);

    e = d.bytes + (fat_u64)firstDataSector * bps;
    write_name(e, "FRAG    DAT");
    e[0x0B] = 0x20;
    put16(e + 0x1A, 10);
    put16(e + 0x14, 0);
    put32(e + 0x1C, 4 * bps);

    dev.ctx = &d;
    dev.read = mem_read;
    dev.write = mem_write;
    dev.bytesPerSector = bps;
    CHECK(fat_mount(&fv, &dev));
    CHECK(fv.fatType == FAT_TYPE_32);
    CHECK(fv.fatMirrored == 0);
    CHECK(fv.activeFat == 1);
    CHECK(fv.fatStartSector == fat1);

    /* Analysis must read the ACTIVE FAT and see the fragmentation. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.doMove = 0;
    CHECK(fat_run(&fv, &cfg, &st) == 0);
    CHECK(st.filesExamined == 1);
    CHECK(st.fragmented == 1);
    CHECK(st.totalFragments == 4);

    /* Defragment, then confirm the active FAT was rewritten contiguously while
     * the stale FAT #0 is left untouched. */
    cfg.doMove = 1;
    cfg.verify = 1;
    CHECK(fat_run(&fv, &cfg, &st) == 0);
    CHECK(st.defragged == 1);
    CHECK(st.clustersMoved == 4);
    CHECK(fat_get(&fv, 3) == 4); /* active FAT now contiguous 3,4,5,6 */
    CHECK(fat_is_eoc(&fv, fat_get(&fv, 6)));

    {
        /* FAT #0 (stale) must still show cluster 10 as a lone EOC. */
        unsigned char *f0 = d.bytes + (fat_u64)fat0 * bps;
        fat_u32 v = (f0[10 * 4] | (f0[10 * 4 + 1] << 8) |
                     (f0[10 * 4 + 2] << 16) | (f0[10 * 4 + 3] << 24)) &
                    0x0FFFFFFFu;
        CHECK(v == 0x0FFFFFFFu);
    }

    fat_unmount(&fv);
    free(d.bytes);
}

/* ================================================================ builder */

/*
 * The tests below build whole volumes: a tree of directories (with long
 * names, deleted entries and a volume label mixed in) and files whose
 * clusters carry a pattern unique to (file, cluster index), scattered and
 * fragmented over the volume. An independent checker then verifies the
 * on-disk result against the expected tree.
 */

#define TV_MAXN  700
#define TV_LFN   48

static const unsigned char t_lfn_off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20,
                                            22, 24, 28, 30};

typedef struct {
    unsigned char name[11]; /* 8.3 name as stored                        */
    char     lfn[TV_LFN];   /* long name, or ""                          */
    int      parent;        /* parent directory (root: -1)               */
    int      isDir, gone, found;
    unsigned char attr;
    fat_u32  size;          /* files: bytes                              */
    fat_u32  id;            /* data pattern of a file                    */
    fat_u32  nCl;           /* clusters                                  */
    fat_u32 *cls;           /* where the builder put them                */
    fat_u32  start, frags;  /* found on disk by the checker              */
    fat_u64  eoff;          /* ... with the image offsets of its entry   */
    fat_u64  lfnOff[4];     /* ... and of its long-name parts            */
} tnode;

typedef struct {
    mem_disk d;
    fat_dev  dev;
    int      type;
    fat_u32  spc, bpc, reserved, fatSize, rootEnt, rootDirSectors;
    fat_u32  fatStart, rootSector, firstDataSector, nClusters, totalSectors;
    unsigned char *used;    /* builder: clusters handed out              */
    unsigned char *claim;   /* checker: clusters reached                 */
    fat_u32 *scratch;
    fat_u32  nextId;
    int      nn;
    tnode    n[TV_MAXN];
} tvol;

static fat_u32 get16(const unsigned char *p)
{
    return (fat_u32)p[0] | ((fat_u32)p[1] << 8);
}

static fat_u32 get32(const unsigned char *p)
{
    return get16(p) | (get16(p + 2) << 16);
}

static fat_u32 tv_eoc(const tvol *v)
{
    return v->type == 12 ? 0xFFFu : v->type == 16 ? 0xFFFFu : 0x0FFFFFFFu;
}

static int tv_is_eoc(const tvol *v, fat_u32 x)
{
    return x >= (tv_eoc(v) & ~7u);
}

static fat_u32 tv_bad(const tvol *v)
{
    return tv_eoc(v) - 8;
}

static unsigned char *tv_fat(const tvol *v, fat_u32 copy)
{
    return v->d.bytes + (fat_u64)(v->fatStart + copy * v->fatSize) * 512;
}

static fat_u32 tv_get(const tvol *v, fat_u32 copy, fat_u32 cl)
{
    const unsigned char *f = tv_fat(v, copy);

    if (v->type == 12) {
        fat_u32 w = get16(f + cl + cl / 2);
        return (cl & 1) ? w >> 4 : w & 0xFFFu;
    }
    if (v->type == 16)
        return get16(f + cl * 2);
    return get32(f + cl * 4) & 0x0FFFFFFFu;
}

/* Set a FAT entry in both copies, as the file system would. */
static void tv_set(tvol *v, fat_u32 cl, fat_u32 val)
{
    fat_u32 k;

    for (k = 0; k < 2; k++) {
        unsigned char *f = tv_fat(v, k);

        if (v->type == 12) {
            unsigned char *p = f + cl + cl / 2;

            if (cl & 1) {
                p[0] = (unsigned char)((p[0] & 0x0F) | ((val << 4) & 0xF0));
                p[1] = (unsigned char)(val >> 4);
            } else {
                p[0] = (unsigned char)val;
                p[1] = (unsigned char)((p[1] & 0xF0) | ((val >> 8) & 0x0F));
            }
        } else if (v->type == 16) {
            put16(f + cl * 2, val & 0xFFFFu);
        } else {
            put32(f + cl * 4, (get32(f + cl * 4) & 0xF0000000u) |
                                  (val & 0x0FFFFFFFu));
        }
    }
}

static unsigned char *tv_cluster(const tvol *v, fat_u32 cl)
{
    return v->d.bytes +
           (fat_u64)(v->firstDataSector + (cl - 2) * v->spc) * 512;
}

/* Files whose id has this bit hold printable text. */
#define TEXT_ID 0x40000000u

static unsigned char pat(fat_u32 id, fat_u32 k, fat_u32 j)
{
    fat_u32 x = id * 2654435761u + k * 40503u + (j >> 5) * 2246822519u;

    if (id & TEXT_ID)
        return (unsigned char)(0x20 + (((x >> 13) ^ j) & 0xFFu) % 0x5F);
    return (unsigned char)((x >> 13) ^ j);
}

static void tv_fill(tvol *v, fat_u32 cl, fat_u32 id, fat_u32 k)
{
    unsigned char *p = tv_cluster(v, cl);
    fat_u32 j;

    for (j = 0; j < v->bpc; j++)
        p[j] = pat(id, k, j);
}

static int tv_data_ok(const tvol *v, fat_u32 cl, fat_u32 id, fat_u32 k)
{
    const unsigned char *p = tv_cluster(v, cl);
    fat_u32 j;

    for (j = 0; j < v->bpc; j++)
        if (p[j] != pat(id, k, j))
            return 0;
    return 1;
}

static fat_u32 ent_start(const tvol *v, const unsigned char *e)
{
    fat_u32 s = get16(e + 0x1A);

    if (v->type == 32)
        s |= get16(e + 0x14) << 16;
    return s;
}

static void set_ent_start(const tvol *v, unsigned char *e, fat_u32 s)
{
    put16(e + 0x1A, s & 0xFFFFu);
    if (v->type == 32)
        put16(e + 0x14, s >> 16);
}

static fat_u32 count_frags(const fat_u32 *a, fat_u32 n)
{
    fat_u32 i, f = n ? 1 : 0;

    for (i = 0; i + 1 < n; i++)
        if (a[i + 1] != a[i] + 1)
            f++;
    return f;
}

static tvol *tv_new(int type, fat_u32 nClusters, fat_u32 spc)
{
    tvol *v = (tvol *)calloc(1, sizeof(tvol));
    fat_u32 bits = type == 12 ? 12 : type == 16 ? 16 : 32;
    unsigned char *b;

    CHECK(v != NULL);
    v->type = type;
    v->spc = spc;
    v->bpc = 512 * spc;
    v->reserved = type == 32 ? 32 : 1;
    v->rootEnt = type == 32 ? 0 : 512;
    v->nClusters = nClusters;
    v->fatSize = (fat_u32)((((fat_u64)(nClusters + 2) * bits + 7) / 8 + 511) /
                           512);
    v->rootDirSectors = v->rootEnt / 16;
    v->fatStart = v->reserved;
    v->rootSector = v->reserved + 2 * v->fatSize;
    v->firstDataSector = v->rootSector + v->rootDirSectors;
    v->totalSectors = v->firstDataSector + nClusters * spc;
    v->nextId = 50000;
    v->d.bps = 512;
    v->d.sectors = v->totalSectors;
    v->d.bytes = (unsigned char *)calloc(v->totalSectors, 512);
    v->used = (unsigned char *)calloc(nClusters + 2, 1);
    v->claim = (unsigned char *)calloc(nClusters + 2, 1);
    v->scratch = (fat_u32 *)calloc(nClusters + 2, sizeof(fat_u32));
    CHECK(v->d.bytes && v->used && v->claim && v->scratch);
    v->dev.ctx = &v->d;
    v->dev.read = mem_read;
    v->dev.write = mem_write;
    v->dev.bytesPerSector = 512;

    b = v->d.bytes;
    b[0] = 0xEB;
    b[1] = 0x58;
    b[2] = 0x90;
    memcpy(b + 3, "MSWIN4.1", 8);
    put16(b + 0x0B, 512);
    b[0x0D] = (unsigned char)spc;
    put16(b + 0x0E, v->reserved);
    b[0x10] = 2;
    put16(b + 0x11, v->rootEnt);
    if (type != 32 && v->totalSectors < 65536)
        put16(b + 0x13, v->totalSectors);
    else
        put32(b + 0x20, v->totalSectors);
    b[0x15] = 0xF8;
    if (type != 32) {
        put16(b + 0x16, v->fatSize);
    } else {
        unsigned char *fs = b + 512;

        put32(b + 0x24, v->fatSize);
        put32(b + 0x2C, 2);
        put16(b + 0x30, 1);
        put16(b + 0x32, 6);
        put32(fs, 0x41615252u);
        put32(fs + 484, 0x61417272u);
        put32(fs + 488, 1234);
        put32(fs + 492, 99);
        put32(fs + 508, 0xAA550000u);
    }
    put16(b + 510, 0xAA55);
    tv_set(v, 0, tv_eoc(v) & ~7u);
    tv_set(v, 1, tv_eoc(v));
    v->used[0] = v->used[1] = 1;

    v->n[0].parent = -1;
    v->n[0].isDir = 1;
    v->n[0].attr = 0x10;
    v->nn = 1;
    return v;
}

static void tv_free(tvol *v)
{
    int i;

    for (i = 0; i < v->nn; i++)
        free(v->n[i].cls);
    free(v->d.bytes);
    free(v->used);
    free(v->claim);
    free(v->scratch);
    free(v);
}

static int tv_node(tvol *v, int parent, int isDir, const char *n83,
                   const char *lfn)
{
    int i = v->nn++;
    tnode *t = &v->n[i];

    CHECK(i < TV_MAXN);
    memset(t, 0, sizeof(*t));
    write_name(t->name, n83);
    if (lfn)
        strncpy(t->lfn, lfn, TV_LFN - 1);
    t->parent = parent;
    t->isDir = isDir;
    t->attr = isDir ? 0x10 : 0x20;
    t->id = 1000 + (fat_u32)i;
    return i;
}

static fat_u32 lfn_parts(const tnode *t)
{
    return t->lfn[0] ? (fat_u32)(strlen(t->lfn) + 12) / 13 : 0;
}

/* Entries in use: "." and ".." (root: the label), a deleted entry, and the
 * children with their long-name parts. */
static fat_u32 tv_dir_slots(const tvol *v, int dir)
{
    fat_u32 s = dir == 0 ? 2 : 3;
    int j;

    for (j = 1; j < v->nn; j++)
        if (v->n[j].parent == dir && !v->n[j].gone)
            s += 1 + lfn_parts(&v->n[j]);
    return s;
}

static fat_u32 tv_dir_clusters(const tvol *v, int dir, fat_u32 spare)
{
    return ((tv_dir_slots(v, dir) + spare) * 32 + v->bpc - 1) / v->bpc;
}

/* Hand out n clusters for node i in about "pieces" fragments, each starting
 * at a random place up to cluster "limit". */
static void tv_alloc(tvol *v, int i, fat_u32 n, fat_u32 pieces, fat_u32 limit,
                     unsigned *rng)
{
    tnode *t = &v->n[i];
    fat_u32 got = 0, last = v->nClusters + 1;

    t->nCl = n;
    t->cls = (fat_u32 *)malloc((n + 1) * sizeof(fat_u32));
    CHECK(t->cls != NULL);
    if (limit < 3 || limit > last)
        limit = last;
    if (pieces < 1)
        pieces = 1;
    while (got < n) {
        fat_u32 want = (n - got + pieces - 1) / pieces, tries = 0;
        fat_u32 cl = 2 + rnd(rng) % (limit - 1);

        if (pieces > 1)
            pieces--;
        while (v->used[cl]) {
            cl = cl >= last ? 2 : cl + 1;
            CHECK(++tries <= last);
        }
        while (want > 0 && got < n && cl <= last && !v->used[cl]) {
            v->used[cl] = 1;
            t->cls[got++] = cl++;
            want--;
        }
    }
}

static void tv_place(tvol *v, int i, const fat_u32 *cls, fat_u32 n)
{
    tnode *t = &v->n[i];
    fat_u32 k;

    t->nCl = n;
    t->cls = (fat_u32 *)malloc((n + 1) * sizeof(fat_u32));
    CHECK(t->cls != NULL);
    for (k = 0; k < n; k++) {
        CHECK(cls[k] >= 2 && cls[k] <= v->nClusters + 1 && !v->used[cls[k]]);
        v->used[cls[k]] = 1;
        t->cls[k] = cls[k];
    }
}

/* FAT32: the root directory starts at cluster 2 (as the boot sector says). */
static void tv_root32(tvol *v, fat_u32 n, int frag, fat_u32 limit,
                      unsigned *rng)
{
    tnode *r = &v->n[0];
    fat_u32 k = 1, cl = 3;

    CHECK(v->type == 32 && !v->used[2]);
    if (limit < 100 || limit > v->nClusters)
        limit = v->nClusters;
    r->nCl = n;
    r->cls = (fat_u32 *)malloc((n + 1) * sizeof(fat_u32));
    CHECK(r->cls != NULL);
    r->cls[0] = 2;
    v->used[2] = 1;
    while (k < n) {
        if (frag && rnd(rng) % 2)
            cl = 3 + rnd(rng) % (limit - 3);
        while (v->used[cl])
            cl++;
        v->used[cl] = 1;
        r->cls[k++] = cl++;
    }
}

static fat_u32 node_start(const tnode *t)
{
    return t->nCl ? t->cls[0] : 0;
}

static void put_entry(const tvol *v, unsigned char *e,
                      const unsigned char *name, unsigned char attr,
                      fat_u32 start, fat_u32 size)
{
    memset(e, 0, 32);
    memcpy(e, name, 11);
    e[0x0B] = attr;
    set_ent_start(v, e, start);
    put32(e + 0x1C, size);
}

static unsigned char sfn_sum(const unsigned char *e)
{
    unsigned char s = 0;
    int i;

    for (i = 0; i < 11; i++)
        s = (unsigned char)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

static fat_u32 put_lfn(unsigned char *dir, fat_u32 slot, const tnode *t)
{
    int len = (int)strlen(t->lfn), parts = (len + 12) / 13, p, i;
    unsigned char sum = sfn_sum(t->name);

    for (p = parts; p >= 1; p--) {
        unsigned char *e = dir + (size_t)slot++ * 32;

        memset(e, 0, 32);
        e[0] = (unsigned char)(p | (p == parts ? 0x40 : 0));
        e[0x0B] = 0x0F;
        e[0x0D] = sum;
        for (i = 0; i < 13; i++) {
            int ci = (p - 1) * 13 + i;

            put16(e + t_lfn_off[i], ci < len ? (unsigned char)t->lfn[ci]
                                    : ci == len ? 0 : 0xFFFF);
        }
    }
    return slot;
}

static void tv_write_dir(tvol *v, int dir)
{
    const tnode *d = &v->n[dir];
    int fixed = dir == 0 && v->type != 32, j;
    fat_u32 bytes = fixed ? v->rootDirSectors * 512 : d->nCl * v->bpc;
    unsigned char *buf = (unsigned char *)calloc(bytes, 1), nm[11];
    fat_u32 slot, k;

    CHECK(buf != NULL && bytes > 0);
    if (dir == 0) {
        write_name(nm, "TESTVOL    ");
        put_entry(v, buf, nm, 0x08, 0, 0);
        slot = 1;
    } else {
        write_name(nm, ".          ");
        put_entry(v, buf, nm, 0x10, node_start(d), 0);
        write_name(nm, "..         ");
        put_entry(v, buf + 32, nm, 0x10,
                  d->parent == 0 ? 0 : node_start(&v->n[d->parent]), 0);
        slot = 2;
    }
    write_name(nm, "DELETED TXT");
    put_entry(v, buf + slot * 32, nm, 0x20, 0, 0);
    buf[slot * 32] = 0xE5;
    slot++;
    for (j = 1; j < v->nn; j++) {
        const tnode *t = &v->n[j];

        if (t->parent != dir || t->gone)
            continue;
        if (t->lfn[0])
            slot = put_lfn(buf, slot, t);
        CHECK(((size_t)slot + 1) * 32 <= bytes);
        put_entry(v, buf + (size_t)slot++ * 32, t->name, t->attr,
                  node_start(t), t->isDir ? 0 : t->size);
    }
    if (fixed) {
        memcpy(v->d.bytes + (fat_u64)v->rootSector * 512, buf, bytes);
    } else {
        for (k = 0; k < d->nCl; k++)
            memcpy(tv_cluster(v, d->cls[k]), buf + (size_t)k * v->bpc,
                   v->bpc);
    }
    free(buf);
}

/* Write the chains, file data and directories of the whole tree. */
static void tv_commit(tvol *v)
{
    int i;

    for (i = 0; i < v->nn; i++) {
        const tnode *t = &v->n[i];
        fat_u32 k;

        for (k = 0; k < t->nCl; k++) {
            tv_set(v, t->cls[k], k + 1 < t->nCl ? t->cls[k + 1] : tv_eoc(v));
            if (!t->isDir)
                tv_fill(v, t->cls[k], t->id, k);
        }
    }
    for (i = 0; i < v->nn; i++)
        if (v->n[i].isDir)
            tv_write_dir(v, i);
    if (v->type == 32 && v->n[0].nCl)
        put32(v->d.bytes + 0x2C, v->n[0].cls[0]);
}

/* Give a file n clusters' worth of data (the last cluster partly used). */
static void tv_size(tvol *v, int i, fat_u32 n, unsigned *rng)
{
    v->n[i].nCl = n;
    v->n[i].size = n ? (n - 1) * v->bpc + 1 + rnd(rng) % v->bpc : 0;
}

typedef struct {
    int     type;
    fat_u32 nClusters, spc;
    int     maxNodes;
    fat_u32 budgetPct;  /* share of the clusters to use                  */
    fat_u32 limitPct;   /* place objects in this leading share           */
    int     fragPct;    /* objects split into several pieces             */
    int     sysFiles;   /* add hidden system files and a swap file       */
    int     fragRoot;   /* FAT32: fragmented root directory              */
} tgen;

static tvol *tv_random(const tgen *g, unsigned *rng)
{
    tvol *v = tv_new(g->type, g->nClusters, g->spc);
    fat_u32 budget = g->nClusters * g->budgetPct / 100, spent = 0;
    fat_u32 limit = 2 + g->nClusters * g->limitPct / 100;
    int dirs[TV_MAXN], order[TV_MAXN], nDirs = 1, i, guard = 0;
    char n83[32], lfn[TV_LFN];

    dirs[0] = 0;
    while (v->nn < g->maxNodes && spent < budget && guard++ < 20000) {
        int parent = rnd(rng) % 3 ? dirs[rnd(rng) % nDirs] : 0;
        int isDir = rnd(rng) % 100 < 16;
        unsigned r = rnd(rng) % 100;
        fat_u32 cap = parent == 0 && g->type != 32 ? 500 : 200;

        if (tv_dir_slots(v, parent) + 8 > cap)
            continue;
        snprintf(n83, sizeof(n83), "%c%07d%s", isDir ? 'D' : 'F', v->nn,
                 isDir ? "   " : (r & 1) ? "DAT" : "EXE");
        if (rnd(rng) % 100 < 30)
            snprintf(lfn, sizeof(lfn), "Long %s Name %d.%s",
                     isDir ? "Folder" : "file", v->nn, isDir ? "d" : "txt");
        else
            lfn[0] = 0;
        i = tv_node(v, parent, isDir, n83, lfn[0] ? lfn : NULL);
        if (isDir) {
            dirs[nDirs++] = i;
            continue;
        }
        tv_size(v, i, r < 8 ? 0 : r < 70 ? 1 + rnd(rng) % 4
                      : r < 93 ? 5 + rnd(rng) % 28 : 33 + rnd(rng) % 170, rng);
        spent += v->n[i].nCl;
        if (g->sysFiles && rnd(rng) % 100 < 5)
            v->n[i].attr |= 0x06;
    }
    if (g->sysFiles) {
        i = tv_node(v, 0, 0, "WIN386  SWP", NULL);
        tv_size(v, i, 40 + rnd(rng) % 80, rng);
    }
    for (i = 1; i < v->nn; i++)
        if (v->n[i].isDir)
            v->n[i].nCl = tv_dir_clusters(v, i, 6 + rnd(rng) % 20);
    if (g->type == 32)
        tv_root32(v, tv_dir_clusters(v, 0, 6 + rnd(rng) % 20), g->fragRoot,
                  limit, rng);
    for (i = 1; i < v->nn; i++) {
        int j = 1 + (int)(rnd(rng) % (unsigned)i), t;

        order[i] = i;
        t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (i = 1; i < v->nn; i++) {
        int k = order[i];
        fat_u32 n = v->n[k].nCl;

        tv_alloc(v, k, n,
                 n > 1 && (int)(rnd(rng) % 100) < g->fragPct ? 2 + rnd(rng) % 5
                                                             : 1,
                 limit, rng);
    }
    tv_commit(v);
    return v;
}

/* ================================================================ checker */

typedef struct {
    int allowLost;  /* tolerate unreferenced clusters, FAT copy differences */
    int noDotDot;   /* do not check ".." entries                            */
    int noData;     /* check the structure only, not the file contents      */
    int intoLive;   /* tolerate lost clusters that lead into a live chain.
                       Only a FAT write that failed between the switch and
                       the freeing of the old clusters can leave those: the
                       old clusters still lead on to the rest of the object
                       until they are freed, and no write order avoids that
                       window. Everything else must keep lost clusters in
                       chains of their own.                                 */
} fsck_opt;

/* Image offset of a directory's 32-byte slot. */
static fat_u64 slot_off(const tvol *v, int fixed, const fat_u32 *cls,
                        fat_u32 slot)
{
    fat_u64 b = (fat_u64)slot * 32;

    if (fixed)
        return (fat_u64)v->rootSector * 512 + b;
    return (fat_u64)(v->firstDataSector + (cls[b / v->bpc] - 2) * v->spc) *
               512 + b % v->bpc;
}

static int fsck_bad(const char *what, const tvol *v, int i)
{
    fprintf(stderr, "fsck: %s (node %d %.11s)\n", what, i,
            i >= 0 ? (const char *)v->n[i].name : "-");
    return 0;
}

/* Follow a chain in FAT #0 into v->scratch, claiming its clusters. Returns
 * its length, or 0 when it is invalid or shares clusters. */
static fat_u32 fsck_chain(tvol *v, fat_u32 start)
{
    fat_u32 n = 0, cl = start, last = v->nClusters + 1;

    for (;;) {
        fat_u32 nx;

        if (cl < 2 || cl > last || v->claim[cl])
            return 0;
        v->claim[cl] = 1;
        v->scratch[n++] = cl;
        nx = tv_get(v, 0, cl);
        if (tv_is_eoc(v, nx))
            return n;
        if (nx == 0 || nx == tv_bad(v))
            return 0;
        cl = nx;
    }
}

static int lfn_ok(const unsigned char *buf, fat_u32 slot, const tnode *t)
{
    int len = (int)strlen(t->lfn), parts = (len + 12) / 13, p, i;
    unsigned char sum = sfn_sum(buf + (size_t)slot * 32);

    if ((int)slot < parts)
        return 0;
    for (p = 1; p <= parts; p++) {
        const unsigned char *e = buf + (size_t)(slot - p) * 32;

        if ((e[0x0B] & 0x3F) != 0x0F || e[0x0D] != sum ||
            (e[0] & 0x1F) != p || ((e[0] & 0x40) != 0) != (p == parts))
            return 0;
        for (i = 0; i < 13; i++) {
            int ci = (p - 1) * 13 + i;
            fat_u32 want = ci < len ? (unsigned char)t->lfn[ci]
                         : ci == len ? 0 : 0xFFFF;

            if (get16(e + t_lfn_off[i]) != want)
                return 0;
        }
    }
    return 1;
}

static int fsck_file(tvol *v, int i, const unsigned char *e,
                     const fsck_opt *o)
{
    tnode *t = &v->n[i];
    fat_u32 start = ent_start(v, e), n, k;

    t->start = start;
    t->frags = 0;
    if (get32(e + 0x1C) != t->size)
        return fsck_bad("size changed", v, i);
    if (t->size == 0)
        return start == 0 ? 1 : fsck_bad("empty file has clusters", v, i);
    n = fsck_chain(v, start);
    if (!n)
        return fsck_bad("bad or cross-linked chain", v, i);
    if (n != (t->size + v->bpc - 1) / v->bpc)
        return fsck_bad("chain length does not match the size", v, i);
    t->frags = count_frags(v->scratch, n);
    for (k = 0; k < n && !o->noData; k++)
        if (!tv_data_ok(v, v->scratch[k], t->id, k))
            return fsck_bad("data damaged", v, i);
    return 1;
}

static int fsck_dir(tvol *v, int dir, fat_u32 start, fat_u32 parentStart,
                    const fsck_opt *o)
{
    int fixed = dir == 0 && v->type != 32, ok = 1, expect = 0, matched = 0, j;
    fat_u32 bytes, n, k, slot = 0, nSlots, *cls = NULL;
    unsigned char *buf;

    if (fixed) {
        bytes = v->rootDirSectors * 512;
        buf = (unsigned char *)malloc(bytes);
        CHECK(buf != NULL);
        memcpy(buf, v->d.bytes + (fat_u64)v->rootSector * 512, bytes);
        v->n[dir].start = 0;
        v->n[dir].frags = 1;
    } else {
        n = fsck_chain(v, start);
        if (!n)
            return fsck_bad("bad or cross-linked directory chain", v, dir);
        v->n[dir].start = start;
        v->n[dir].frags = count_frags(v->scratch, n);
        bytes = n * v->bpc;
        buf = (unsigned char *)malloc(bytes);
        cls = (fat_u32 *)malloc(n * sizeof(fat_u32));
        CHECK(buf != NULL && cls != NULL);
        memcpy(cls, v->scratch, n * sizeof(fat_u32));
        for (k = 0; k < n; k++)
            memcpy(buf + (size_t)k * v->bpc, tv_cluster(v, v->scratch[k]),
                   v->bpc);
    }
    nSlots = bytes / 32;
    if (dir != 0) {
        if (memcmp(buf, ".          ", 11) != 0 || !(buf[0x0B] & 0x10) ||
            ent_start(v, buf) != start)
            ok = fsck_bad("bad \".\" entry", v, dir);
        else if (!o->noDotDot &&
                 (memcmp(buf + 32, "..         ", 11) != 0 ||
                  ent_start(v, buf + 32) !=
                      (v->n[dir].parent == 0 ? 0 : parentStart)))
            ok = fsck_bad("bad \"..\" entry", v, dir);
        slot = 2;
    }
    for (j = 1; j < v->nn; j++)
        if (v->n[j].parent == dir && !v->n[j].gone)
            expect++;
    for (; ok && slot < nSlots; slot++) {
        const unsigned char *e = buf + (size_t)slot * 32;
        tnode *t = NULL;

        if (e[0] == 0)
            break;
        if (e[0] == 0xE5 || (e[0x0B] & 0x0F) == 0x0F || (e[0x0B] & 0x08))
            continue;
        if (e[0] == '.') {
            ok = fsck_bad("stray dot entry", v, dir);
            break;
        }
        for (j = 1; j < v->nn; j++) {
            if (v->n[j].parent == dir && !v->n[j].gone &&
                memcmp(v->n[j].name, e, 11) == 0) {
                t = &v->n[j];
                break;
            }
        }
        if (!t) {
            fprintf(stderr, "fsck: unexpected entry %.11s\n",
                    (const char *)e);
            ok = fsck_bad("unexpected entry in directory", v, dir);
        } else if (t->found) {
            ok = fsck_bad("duplicate entry", v, j);
        } else if ((e[0x0B] & 0x3F) != t->attr) {
            ok = fsck_bad("attributes changed", v, j);
        } else if (t->lfn[0] && !lfn_ok(buf, slot, t)) {
            ok = fsck_bad("long name damaged", v, j);
        } else {
            fat_u32 p;

            t->found = 1;
            matched++;
            t->eoff = slot_off(v, fixed, cls, slot);
            for (p = 0; p < lfn_parts(t) && p < 4; p++)
                t->lfnOff[p] = slot_off(v, fixed, cls, slot - 1 - p);
            ok = t->isDir ? fsck_dir(v, j, ent_start(v, e), start, o)
                          : fsck_file(v, j, e, o);
        }
    }
    if (ok && matched != expect)
        ok = fsck_bad("entries missing", v, dir);
    free(cls);
    free(buf);
    return ok;
}

/* Check the whole volume against the expected tree. Leaves every node's
 * current start and fragment count in tnode.start / tnode.frags. */
static int tv_fsck(tvol *v, const fsck_opt *o)
{
    static const fsck_opt strict = {0, 0, 0, 0};
    fat_u32 cl, last = v->nClusters + 1;
    unsigned long lost = 0, diff = 0, into = 0;
    int i;

    if (!o)
        o = &strict;
    memset(v->claim, 0, last + 1);
    for (i = 0; i < v->nn; i++)
        v->n[i].found = 0;
    if (!fsck_dir(v, 0, v->type == 32 ? get32(v->d.bytes + 0x2C) : 0, 0, o))
        return 0;
    for (cl = 2; cl <= last; cl++) {
        fat_u32 x = tv_get(v, 0, cl);

        if (x != 0 && x != tv_bad(v) && !v->claim[cl]) {
            lost++;
            /* Lost clusters may stay behind after a failed write, but they
             * must form chains of their own: one that runs into a live
             * chain looks cross-linked to ScanDisk, and freeing it, or
             * reusing the live clusters, damages the other owner. */
            if (!o->intoLive && x >= 2 && x <= last && v->claim[x])
                into++;
        }
        if (x != tv_get(v, 1, cl))
            diff++;
    }
    if (!o->allowLost && (lost || diff)) {
        fprintf(stderr, "fsck: %lu lost clusters, %lu FAT copy differences\n",
                lost, diff);
        return 0;
    }
    if (into) {
        fprintf(stderr, "fsck: %lu lost clusters lead into live chains\n",
                into);
        return 0;
    }
    return 1;
}

/* Files and folders that are not where they belong: fragmented ones. */
static int tv_fragmented(const tvol *v, int (*skip)(const tvol *, int))
{
    int i, n = 0;

    for (i = 1; i < v->nn; i++)
        if (!v->n[i].gone && v->n[i].frags > 1 && !(skip && skip(v, i)))
            n++;
    return n;
}

/* MS-DOS mode run (fat_defrag_cfg.offline): the swap file and the registry
 * may move. */
static int g_offline;

/* Mirrors root_movable in fatdefrag.c. */
static const char t_root_movable[][12] = {
    "BOOTLOG TXT", "BOOTLOG PRV", "DETLOG  TXT", "DETLOG  OLD", "DETCRASHLOG",
    "SETUPLOGTXT", "SETUPLOGOLD", "SETUPXLGTXT", "NETLOG  TXT", "FRUNLOG TXT",
    "ASD     LOG", "SCANDISKLOG", "SUHDLOG DAT", "SUHDLOG BAK", "SYSTEM  1ST",
    "MSDOS   ---", "BOOTSECTDOS", "CONFIG  WOS", "AUTOEXECWOS", "W95UNDO DAT",
    "W95UNDO INI", "W98UNDO DAT", "W98UNDO INI", "DRVSPACEBIN", "DBLSPACEBIN",
    "DRVSPACEINI", "DBLSPACEINI", "WINBOOT INI", "BOOT    INI", "CHKLIST MS ",
    "FFASTUN FFA", "FFASTUN FFL", "FFASTUN FFO", "FFASTUN0FFX", "WIN386  SWP"};

#define N_ROOT_MOVABLE ((int)(sizeof(t_root_movable) / sizeof(t_root_movable[0])))

/* Mirrors is_system in fatdefrag.c. */
static int is_system(const tvol *v, int i)
{
    static const char live[4][12] = {"WIN386  SWP", "SYSTEM  DAT",
                                     "USER    DAT", "CLASSES DAT"};
    static const char boot[][12] = {"IO      SYS", "WINBOOT SYS",
                                    "IBMBIO  COM", "IBMDOS  COM",
                                    "NTLDR      "};
    const tnode *t = &v->n[i];
    const unsigned char *name = t->name;
    int k;

    if (t->isDir)
        return 0;
    if (memcmp(name, "LDLINUX SYS", 11) == 0)
        return 1;
    for (k = 0; k < 4 && !g_offline; k++)
        if (memcmp(name, live[k], 11) == 0)
            return 1;
    if (t->parent == 0) {
        for (k = 0; k < (int)(sizeof(boot) / sizeof(boot[0])); k++)
            if (memcmp(name, boot[k], 11) == 0)
                return 1;
        if (memcmp(name, "MSDOS   SYS", 11) == 0)
            return !(t->id & TEXT_ID) || t->size < 1 || t->size > 16384;
    }
    if (t->parent != 0 || (t->attr & 0x06) != 0x06)
        return 0;
    for (k = 0; k < N_ROOT_MOVABLE; k++)
        if (memcmp(name, t_root_movable[k], 11) == 0)
            return 0;
    return 1;
}

/* ========================================================== write hooks */

/*
 * The write-section hooks stand in for the Windows 95 volume locks. They
 * check that the engine writes only inside sections, never allocates memory
 * or calls back while in one, and they inject what a live volume does: busy
 * answers, failures (Ctrl+C), and other programs changing files in between
 * sections, after which the FAT is re-read and FAT_WR_CHANGED returned.
 */

#ifdef __SANITIZE_ADDRESS__
int __sanitizer_install_malloc_and_free_hooks(
    void (*malloc_hook)(const volatile void *, size_t),
    void (*free_hook)(const volatile void *));
#endif

static int g_inSec;            /* mirrors hctx.inSec for the malloc hooks */
static unsigned long g_allocs; /* allocations and frees inside sections   */

#ifdef __SANITIZE_ADDRESS__
static void on_malloc(const volatile void *p, size_t n)
{
    (void)p;
    (void)n;
    if (g_inSec)
        g_allocs++;
}

static void on_free(const volatile void *p)
{
    (void)p;
    if (g_inSec)
        g_allocs++;
}
#endif

enum { MU_DELETE, MU_EXTEND, MU_TRUNCATE, MU_REWRITE, MU_CREATE, MU_RENAME,
       MU_KINDS };

typedef struct {
    tvol         *v;
    fat_volume   *fv;
    int           inSec;
    unsigned      rng;
    unsigned long begins, opened, ends;
    unsigned      busyPct;   /* answer "busy" this often (percent)          */
    unsigned long failFrom;  /* answer "fail" from this begin on            */
    unsigned      mutPct;    /* another program writes before a section     */
    unsigned long mutAt;     /* ... or exactly before this begin            */
    int           mutKind;   /* -1: any kind of change                      */
    int           mutTarget; /* file to change (0: a random one)            */
    int           pending;   /* report a change made before the run         */
    unsigned long mutations;
    unsigned      fsckPct;   /* check the volume before this share of begins */
    int           pinNode[8];
    fat_u32       pinStart[8];
    int           nPins;
    unsigned long badCb, badPath, ev[16], progressCalls;
    unsigned long movedInPieces; /* MOVED events that left the object split */
    unsigned long unmSystem, unmInUse, unmBad; /* UNMOVABLE events by kind  */
    unsigned long runMuts;       /* changes other programs made in the run  */
    unsigned long rootMoves;     /* MOVED events of the root folder         */
    int           stopOnDotdot;  /* Ctrl+C right after a failed ".." fixup  */
} hctx;

static int is_pinned(const hctx *h, int i)
{
    int k;

    for (k = 0; k < h->nPins; k++)
        if (h->pinNode[k] == i)
            return 1;
    return 0;
}

static void h_pin(hctx *h, int i)
{
    CHECK(h->nPins < 8);
    h->pinNode[h->nPins] = i;
    h->pinStart[h->nPins++] = node_start(&h->v->n[i]);
}

static int h_unmovable(void *ctx, fat_u32 start)
{
    const hctx *h = (const hctx *)ctx;
    int k;

    for (k = 0; k < h->nPins; k++)
        if (h->pinStart[k] == start)
            return 1;
    return 0;
}

static void h_progress(void *ctx, int phase, fat_u64 done, fat_u64 total)
{
    hctx *h = (hctx *)ctx;

    (void)phase;
    (void)done;
    (void)total;
    h->progressCalls++;
    if (h->inSec)
        h->badCb++;
}

static void h_event(void *ctx, int ev, const char *path, int frags,
                    int newFrags)
{
    hctx *h = (hctx *)ctx;

    (void)frags;
    if (ev == FAT_EV_MOVED && newFrags > 1)
        h->movedInPieces++;
    if (ev == FAT_EV_MOVED && path && strcmp(path, "\\") == 0)
        h->rootMoves++;
    if (ev == FAT_EV_UNMOVABLE) {
        if (newFrags == FAT_GAP_SYSTEM)
            h->unmSystem++;
        else if (newFrags == FAT_GAP_INUSE)
            h->unmInUse++;
        else
            h->unmBad++;
    }
    if (h->inSec)
        h->badCb++;
    if (!path || path[0] != '\\')
        h->badPath++;
    if (ev >= 0 && ev < 16)
        h->ev[ev]++;
    if (ev == FAT_EV_DOTDOT && h->stopOnDotdot)
        h->failFrom = h->begins + 1;
}

/* ---------------------------------------------- other programs' changes */

static fat_u32 file_clusters(const tvol *v, const tnode *t)
{
    return (t->size + v->bpc - 1) / v->bpc;
}

static fat_u32 rnd_size(tvol *v, fat_u32 n, unsigned *rng)
{
    return n ? (n - 1) * v->bpc + 1 + rnd(rng) % v->bpc : 0;
}

/* Take n free clusters (runs from random places) and fill them with the
 * data of file t starting at cluster index k0. They are chained in order. */
static int take_clusters(hctx *h, tnode *t, fat_u32 k0, fat_u32 n,
                         fat_u32 *out)
{
    tvol *v = h->v;
    fat_u32 got = 0, last = v->nClusters + 1, k;

    while (got < n) {
        fat_u32 cl = 2 + rnd(&h->rng) % v->nClusters, tries = 0;

        while (tv_get(v, 0, cl) != 0) {
            cl = cl >= last ? 2 : cl + 1;
            if (++tries > v->nClusters) {
                for (k = 0; k < got; k++)
                    tv_set(v, out[k], 0);
                return 0;
            }
        }
        do {
            tv_set(v, cl, tv_eoc(v));
            out[got++] = cl++;
        } while (got < n && cl <= last && tv_get(v, 0, cl) == 0 &&
                 rnd(&h->rng) % 4);
    }
    for (k = 0; k < n; k++) {
        if (k + 1 < n)
            tv_set(v, out[k], out[k + 1]);
        tv_fill(v, out[k], t->id, k0 + k);
    }
    return 1;
}

static fat_u32 disk_chain(const tvol *v, fat_u32 start, fat_u32 *out)
{
    fat_u32 n = 0, cl = start;

    while (cl >= 2 && cl <= v->nClusters + 1) {
        out[n++] = cl;
        cl = tv_get(v, 0, cl);
        if (tv_is_eoc(v, cl))
            break;
    }
    return n;
}

/* A free directory slot of node dir (image offset), or 0. */
static fat_u64 free_slot(const tvol *v, int dir)
{
    const tnode *d = &v->n[dir];
    int fixed = dir == 0 && v->type != 32;
    fat_u32 n = fixed ? 0 : disk_chain(v, d->start, v->scratch), s, nSlots;

    nSlots = fixed ? v->rootEnt : n * v->bpc / 32;
    for (s = dir == 0 ? 1 : 2; s < nSlots; s++) {
        fat_u64 off = slot_off(v, fixed, v->scratch, s);

        if (v->d.bytes[off] == 0x00 || v->d.bytes[off] == 0xE5)
            return off;
    }
    return 0;
}

static int pick_file(hctx *h)
{
    tvol *v = h->v;
    int tries;

    if (h->mutTarget)
        return h->mutTarget;
    for (tries = 0; tries < 50; tries++) {
        int i = 1 + (int)(rnd(&h->rng) % (unsigned)(v->nn - 1));

        if (!v->n[i].gone && !v->n[i].isDir && !is_system(v, i) &&
            !is_pinned(h, i))
            return i;
    }
    return 0;
}

/* Change the volume like another program would. Returns 1 if it did. */
static int mutate(hctx *h)
{
    static const fsck_opt structure = {0, 0, 1, 0};
    tvol *v = h->v;
    int kind = h->mutKind >= 0 ? h->mutKind : (int)(rnd(&h->rng) % MU_KINDS);
    int i = pick_file(h);
    tnode *t;
    unsigned char *e;
    fat_u32 n, m, k, *cls;

    /* Locate everything; between sections the volume must be consistent. */
    CHECK(tv_fsck(v, &structure));
    if (!i)
        return 0;
    t = &v->n[i];
    e = v->d.bytes + t->eoff;
    n = file_clusters(v, t);
    cls = (fat_u32 *)malloc((n + 64) * sizeof(fat_u32));
    CHECK(cls != NULL);
    CHECK(disk_chain(v, t->start, cls) == n);
    switch (kind) {
    case MU_DELETE:
        for (k = 0; k < n; k++)
            tv_set(v, cls[k], 0);
        e[0] = 0xE5;
        for (k = 0; k < lfn_parts(t); k++)
            v->d.bytes[t->lfnOff[k]] = 0xE5;
        t->gone = 1;
        break;
    case MU_EXTEND:
        m = 1 + rnd(&h->rng) % 8;
        if (!take_clusters(h, t, n, m, cls + n))
            goto none;
        if (n)
            tv_set(v, cls[n - 1], cls[n]);
        else
            set_ent_start(v, e, cls[0]);
        t->size = rnd_size(v, n + m, &h->rng);
        put32(e + 0x1C, t->size);
        break;
    case MU_TRUNCATE:
        if (!n)
            goto none;
        m = rnd(&h->rng) % n;
        for (k = m; k < n; k++)
            tv_set(v, cls[k], 0);
        if (m)
            tv_set(v, cls[m - 1], tv_eoc(v));
        else
            set_ent_start(v, e, 0);
        t->size = rnd_size(v, m, &h->rng);
        put32(e + 0x1C, t->size);
        break;
    case MU_REWRITE:
        if (!n)
            goto none;
        t->id = v->nextId++;
        for (k = 0; k < n; k++)
            tv_fill(v, cls[k], t->id, k);
        break;
    case MU_CREATE: {
        int dir = 0, tries;
        fat_u64 off;
        char nm[32];

        if (v->nn >= TV_MAXN)
            goto none;
        for (tries = 0; tries < 20; tries++) {
            dir = (int)(rnd(&h->rng) % (unsigned)v->nn);
            if (v->n[dir].isDir && !v->n[dir].gone)
                break;
        }
        if (!v->n[dir].isDir || !(off = free_slot(v, dir)))
            goto none;
        snprintf(nm, sizeof(nm), "N%07dNEW", v->nn);
        i = tv_node(v, dir, 0, nm, NULL);
        t = &v->n[i];
        m = rnd(&h->rng) % 24;
        t->size = rnd_size(v, m, &h->rng);
        if (m && !take_clusters(h, t, 0, m, cls)) {
            v->nn--;
            goto none;
        }
        put_entry(v, v->d.bytes + off, t->name, t->attr, m ? cls[0] : 0,
                  t->size);
        break;
    }
    default: /* MU_RENAME */
        t->name[0] = t->name[0] == 'R' ? 'Q' : 'R';
        memcpy(e, t->name, 11);
        for (k = 0; k < lfn_parts(t); k++)
            v->d.bytes[t->lfnOff[k] + 0x0D] = sfn_sum(t->name);
        break;
    }
    free(cls);
    h->mutations++;
    return 1;
none:
    free(cls);
    return 0;
}

static int h_begin(void *ctx)
{
    hctx *h = (hctx *)ctx;
    int changed = h->pending;

    CHECK(!h->inSec);
    h->begins++;
    h->pending = 0;
    if (h->failFrom && h->begins >= h->failFrom)
        return FAT_WR_FAIL;
    if (h->busyPct && rnd(&h->rng) % 100 < h->busyPct) {
        h->pending = changed;
        return FAT_WR_BUSY;
    }
    if (h->fsckPct && rnd(&h->rng) % 100 < h->fsckPct)
        CHECK(tv_fsck(h->v, NULL));
    if ((h->mutPct && rnd(&h->rng) % 100 < h->mutPct) ||
        h->begins == h->mutAt)
        changed |= mutate(h);
    if (changed)
        CHECK(fat_reload(h->fv));
    h->opened++;
    h->inSec = g_inSec = 1;
    return changed ? FAT_WR_CHANGED : FAT_WR_OK;
}

static void h_end(void *ctx)
{
    hctx *h = (hctx *)ctx;

    CHECK(h->inSec);
    h->inSec = g_inSec = 0;
    h->ends++;
}

static void h_init(hctx *h, tvol *v, fat_volume *fv, unsigned seed)
{
    memset(h, 0, sizeof(*h));
    h->v = v;
    h->fv = fv;
    h->rng = seed * 2654435761u + 1;
    h->mutKind = -1;
}

static void tv_mount(tvol *v, fat_volume *fv)
{
    memset(fv, 0, sizeof(*fv));
    CHECK(fat_mount(fv, &v->dev));
}

/* Run the engine and check the rules every run must keep. */
static int run(hctx *h, int doMove, int hooks, fat_stats *st)
{
    tvol *v = h->v;
    fat_defrag_cfg cfg;
    unsigned long writes = v->d.writes, allocs = g_allocs;
    unsigned long muts = h->mutations;
    unsigned char boot[512];
    fat_u32 k;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.doMove = doMove;
    cfg.verify = 1;
    cfg.compact = 1;
    cfg.offline = g_offline;
    cfg.cbctx = h;
    cfg.progress = h_progress;
    cfg.event = h_event;
    cfg.is_unmovable = h_unmovable;
    if (hooks) {
        cfg.begin_write = h_begin;
        cfg.end_write = h_end;
    }
    memset(h->ev, 0, sizeof(h->ev));
    h->movedInPieces = h->rootMoves = 0;
    h->unmSystem = h->unmInUse = h->unmBad = 0;
    v->d.inSec = hooks ? &h->inSec : NULL;
    v->d.outside = 0;
    memcpy(boot, v->d.bytes, sizeof(boot));
    rc = fat_run(h->fv, &cfg, st);
    v->d.inSec = NULL;
    CHECK(rc >= 0);
    /* The boot sector, which names the FAT32 root folder's first cluster,
     * is never written. */
    CHECK(memcmp(boot, v->d.bytes, sizeof(boot)) == 0);
    CHECK(v->type == 32 || h->rootMoves == 0);
    CHECK(!h->inSec && h->opened == h->ends);
    CHECK(h->badCb == 0 && h->badPath == 0);
    CHECK(g_allocs == allocs);
    CHECK(v->d.outside == 0);
    CHECK(h->progressCalls > 0);
    if (!doMove)
        CHECK(v->d.writes == writes &&
              h->ev[FAT_EV_FRAGMENTED] == st->fragmented + st->dirsFragmented);
    h->runMuts = h->mutations - muts;
    CHECK(h->unmBad == 0);
    CHECK(h->unmSystem + h->unmInUse == (doMove ? st->unmovable : 0));
    CHECK(st->gapNotes == (st->gapsLeft < FAT_GAP_NOTES ? st->gapsLeft
                                                        : FAT_GAP_NOTES));
    CHECK(st->keptNotes == (st->unmovable < FAT_KEPT_NOTES
                                ? (fat_u32)st->unmovable
                                : FAT_KEPT_NOTES));
    for (k = 0; k < st->keptNotes; k++) {
        const fat_kept_note *n = &st->keptNote[k];

        CHECK(n->clusters > 0 && n->frags > 0);
        CHECK(n->why == FAT_GAP_SYSTEM || n->why == FAT_GAP_INUSE);
        CHECK(memchr(n->path, 0, sizeof(n->path)) != NULL);
        CHECK(n->path[0] == '\\' || n->path[0] == '.');
    }
    CHECK(g_offline || st->lostClusters == 0);
    for (k = 0; k < st->gapNotes; k++) {
        const fat_gap_note *g = &st->gapNote[k];

        CHECK(g->clusters > 0 && g->why >= 0 && g->why <= FAT_GAP_BAD);
        CHECK(k == 0 || st->gapNote[k - 1].clusters >= g->clusters);
        CHECK(memchr(g->path, 0, sizeof(g->path)) != NULL);
        if (g->why == FAT_GAP_LOST || g->why == FAT_GAP_BAD)
            CHECK(g->path[0] == 0);
        else
            CHECK(g->path[0] == '\\' || g->path[0] == '.');
    }
    if (doMove && !st->crossLinked && !st->chainErrors && !st->ioErrors &&
        !h->runMuts)
        CHECK(st->gapsLeft == st->freeGaps);
    return rc;
}

/* The analysis must measure exactly what the checker found. */
static void check_measure(hctx *h, const fat_stats *st)
{
    const tvol *v = h->v;
    fat_u64 rootFrag = v->n[0].frags > 1, dfrag = rootFrag;
    fat_u64 files = 0, dirs = 0, cont = 0, frag = 0, tf = 0;
    fat_u64 unm = 0, unmFrag = 0;
    int i;

    for (i = 1; i < v->nn; i++) {
        const tnode *t = &v->n[i];

        if (t->gone)
            continue;
        if (t->isDir) {
            dirs++;
            dfrag += t->frags > 1;
            continue;
        }
        files++;
        if (t->frags > 1) {
            frag++;
            tf += t->frags;
        } else {
            cont++;
        }
        if ((is_system(v, i) || is_pinned(h, i)) && t->start) {
            unm++;
            unmFrag += t->frags > 1;
        }
    }
    CHECK(st->filesExamined == files && st->dirsExamined == dirs);
    CHECK(st->contiguous == cont && st->fragmented == frag);
    CHECK(st->totalFragments == tf && st->dirsFragmented == dfrag);
    CHECK(st->unmovable == unm);
    /* The FAT32 root folder counts while it stays in pieces, which an
     * analysis may plan to join. */
    if (rootFrag && (st->planDefrag || st->compactable))
        CHECK(st->unmovableFragmented == unmFrag ||
              st->unmovableFragmented == unmFrag + 1);
    else
        CHECK(st->unmovableFragmented == unmFrag + rootFrag);
    CHECK(st->chainErrors == 0 && st->crossLinked == 0 && st->ioErrors == 0);
}

/* The path the engine reports for node i ("\\DIR\\FILE.EXT"; folders end in
 * a backslash), cut like a gap note's. */
static void tv_note_path(const tvol *v, int i, char *out)
{
    char full[300];
    int stack[66], n = 0, j = i;
    size_t len = 0, cap = sizeof(((fat_gap_note *)0)->path), k;

    while (j > 0 && n < 66) {
        stack[n++] = j;
        j = v->n[j].parent;
    }
    while (n-- > 0) {
        const unsigned char *e = v->n[stack[n]].name;
        char nm[16];
        int a, b = 0;

        for (a = 0; a < 8 && e[a] != ' '; a++)
            nm[b++] = (char)e[a];
        if (e[8] != ' ') {
            nm[b++] = '.';
            for (a = 8; a < 11 && e[a] != ' '; a++)
                nm[b++] = (char)e[a];
        }
        nm[b] = 0;
        if (len + 1 < sizeof(full))
            full[len++] = '\\';
        for (a = 0; nm[a] && len + 1 < sizeof(full); a++)
            full[len++] = nm[a];
    }
    if ((i == 0 || v->n[i].isDir) && len + 1 < sizeof(full))
        full[len++] = '\\';
    full[len] = 0;
    if (len < cap) {
        memcpy(out, full, len + 1);
        return;
    }
    memcpy(out, "...", 3);
    for (k = 0; k < cap - 4; k++)
        out[3 + k] = full[len - (cap - 4) + k];
    out[cap - 1] = 0;
}

/* After a defrag run the gap notes must describe the free space on disk:
 * the largest gaps, largest first (the earlier of equal ones), each with
 * what follows it; offline, the lost clusters are counted. Needs the starts
 * found by tv_fsck and a run no other program wrote during. */
static void check_gap_notes(const hctx *h, const fat_stats *st)
{
    const tvol *v = h->v;
    fat_u32 top = v->nClusters + 1, last = 1, cl, run = 0, nGaps = 0;
    fat_u32 lost = 0, k, g, *gSize, *gAt;
    int *own, i;

    if (h->runMuts)
        return;
    own = (int *)malloc((top + 1) * sizeof(int));
    gSize = (fat_u32 *)malloc((top / 2 + 2) * sizeof(fat_u32));
    gAt = (fat_u32 *)malloc((top / 2 + 2) * sizeof(fat_u32));
    CHECK(own && gSize && gAt);
    for (cl = 0; cl <= top; cl++)
        own[cl] = -1;
    for (i = 0; i < v->nn; i++) {
        fat_u32 n;

        if (v->n[i].gone || !v->n[i].start)
            continue;
        n = disk_chain(v, v->n[i].start, v->scratch);
        for (k = 0; k < n; k++)
            own[v->scratch[k]] = i;
    }
    for (cl = top; cl >= 2; cl--)
        if (tv_get(v, 0, cl) != 0) {
            last = cl;
            break;
        }
    for (cl = 2; cl <= last; cl++) {
        if (tv_get(v, 0, cl) == 0) {
            run++;
        } else if (run) {
            gSize[nGaps] = run;
            gAt[nGaps++] = cl;
            run = 0;
        }
    }
    CHECK(st->gapsLeft == nGaps);
    CHECK(st->gapNotes == (nGaps < FAT_GAP_NOTES ? nGaps : FAT_GAP_NOTES));
    for (k = 0; k < st->gapNotes; k++) {
        const fat_gap_note *note = &st->gapNote[k];
        fat_u32 best = 0;
        int o, why;
        char path[sizeof(note->path)];

        for (g = 0; g < nGaps; g++)
            if (gSize[g] > gSize[best])
                best = g;
        o = own[gAt[best]];
        if (o < 0)
            why = tv_get(v, 0, gAt[best]) == tv_bad(v) ? FAT_GAP_BAD
                                                       : FAT_GAP_LOST;
        else if (o == 0)
            why = FAT_GAP_ROOT;
        else if (is_system(v, o))
            why = FAT_GAP_SYSTEM;
        else if (is_pinned(h, o))
            why = FAT_GAP_INUSE;
        else
            why = FAT_GAP_STUCK;
        CHECK(note->clusters == gSize[best] && note->why == why);
        if (o < 0) {
            CHECK(note->path[0] == 0);
        } else {
            tv_note_path(v, o, path);
            CHECK(strcmp(note->path, path) == 0);
        }
        gSize[best] = 0;
    }
    for (cl = 2; cl <= top; cl++) {
        fat_u32 x = tv_get(v, 0, cl);

        if (x != 0 && x != tv_bad(v) && own[cl] < 0)
            lost++;
    }
    CHECK(st->lostClusters == (g_offline ? lost : 0));
    free(own);
    free(gSize);
    free(gAt);
}

/* An analysis must foresee the gaps the defragmentation leaves. */
static void same_notes(const fat_stats *a, const fat_stats *b)
{
    fat_u32 k;

    CHECK(a->gapsLeft == b->gapsLeft && a->gapNotes == b->gapNotes);
    CHECK(a->lostClusters == b->lostClusters);
    for (k = 0; k < a->gapNotes && k < FAT_GAP_NOTES; k++) {
        CHECK(a->gapNote[k].clusters == b->gapNote[k].clusters);
        CHECK(a->gapNote[k].why == b->gapNote[k].why);
        CHECK(strcmp(a->gapNote[k].path, b->gapNote[k].path) == 0);
    }
}

static int movable_node(const hctx *h, int i)
{
    return !is_system(h->v, i) && !is_pinned(h, i);
}

static const hctx *g_skipH;

static int skip_unmovable(const tvol *v, int i)
{
    (void)v;
    return !movable_node(g_skipH, i);
}

static int frag_movable(const hctx *h)
{
    g_skipH = h;
    return tv_fragmented(h->v, skip_unmovable);
}

static fat_u32 fsinfo_free(const tvol *v)
{
    return get32(v->d.bytes + 512 + 488);
}

/* =================================================== deterministic tests */

static void place(tvol *v, int i, const fat_u32 *cls, fat_u32 n, unsigned *rng)
{
    tv_place(v, i, cls, n);
    if (!v->n[i].isDir)
        v->n[i].size = rnd_size(v, n, rng);
}

typedef struct {
    int a, b, dir1, sub, s1, s2, d1, d2, empty;
} dm_nodes;

/* A fragmented folder with a long name, a subfolder and files. */
static tvol *build_dirmove(int type, dm_nodes *x, unsigned *rng)
{
    static const fat_u32 a[] = {10, 11, 12}, b[] = {20, 21, 40, 41, 60, 61};
    static const fat_u32 d1[] = {30, 50, 70}, sub[] = {80}, s1[] = {90, 95};
    static const fat_u32 s2[] = {100}, dd1[] = {110, 111, 112, 113};
    static const fat_u32 emp[] = {120};
    tvol *v = tv_new(type, type == 32 ? 66000 : type == 16 ? 4200 : 3000, 1);

    if (type == 32)
        tv_root32(v, 2, 0, 0, rng);
    x->a = tv_node(v, 0, 0, "A       TXT", NULL);
    x->b = tv_node(v, 0, 0, "B       DAT", "Bee data file.dat");
    x->dir1 = tv_node(v, 0, 1, "DIR1       ", "My Long Folder Name");
    x->sub = tv_node(v, x->dir1, 1, "SUB        ", NULL);
    x->s1 = tv_node(v, x->sub, 0, "S1      TXT", NULL);
    x->s2 = tv_node(v, x->sub, 0, "S2      TXT", NULL);
    x->d1 = tv_node(v, x->dir1, 0, "D1      TXT", "Document one.txt");
    x->d2 = tv_node(v, x->dir1, 0, "D2      TXT", NULL);
    x->empty = tv_node(v, 0, 1, "EMPTY      ", NULL);
    place(v, x->a, a, 3, rng);
    place(v, x->b, b, 6, rng);
    place(v, x->dir1, d1, 3, rng);
    place(v, x->sub, sub, 1, rng);
    place(v, x->s1, s1, 2, rng);
    place(v, x->s2, s2, 1, rng);
    place(v, x->d1, dd1, 4, rng);
    place(v, x->empty, emp, 1, rng);
    tv_commit(v);
    return v;
}

static void check_lookup(fat_volume *fv, const char *path, int expect,
                         fat_u32 start)
{
    fat_u32 s = 0xDEADBEEFu;
    int r = fat_lookup(fv, path, &s);

    if (r != expect || (r && s != start))
        fprintf(stderr, "lookup \"%s\": %d %u (expected %d %u)\n", path, r,
                s, expect, start);
    CHECK(r == expect && (!r || s == start));
}

static void lookups(tvol *v, fat_volume *fv, const dm_nodes *x)
{
    CHECK(tv_fsck(v, NULL));
    check_lookup(fv, "\\A.TXT", 1, v->n[x->a].start);
    check_lookup(fv, "C:\\DIR1\\SUB\\S1.TXT", 1, v->n[x->s1].start);
    check_lookup(fv, "c:/my long folder name/sub/s2.txt", 1,
                 v->n[x->s2].start);
    check_lookup(fv, "DIR1\\Document One.TXT", 1, v->n[x->d1].start);
    check_lookup(fv, "\\DIR1\\D1.TXT", 1, v->n[x->d1].start);
    check_lookup(fv, "\\dir1\\d2.txt", 1, 0);
    check_lookup(fv, "\\DIR1\\", 1, v->n[x->dir1].start);
    check_lookup(fv, "\\DIR1\\SUB", 1, v->n[x->sub].start);
    check_lookup(fv, "\\Bee Data File.DAT", 1, v->n[x->b].start);
    check_lookup(fv, "\\BEE DATA FILE.DA", 0, 0);
    check_lookup(fv, "\\NOPE.TXT", 0, 0);
    check_lookup(fv, "\\A.TXT\\X", 0, 0);
    check_lookup(fv, "\\DIR1\\SUB\\NOPE", 0, 0);
    check_lookup(fv, "\\EMPTY\\X", 0, 0);
    check_lookup(fv, "C:\\", 0, 0);
    check_lookup(fv, "", 0, 0);
}

static void test_bad_geometry(int type)
{
    tvol *v = tv_new(type, type == 12 ? 3000 : type == 16 ? 5000 : 66000, 1);
    unsigned char boot[512], buf[512];
    fat_volume fv;
    fat_dev dev;
    unsigned long reads, writes;
    int mode;

    memcpy(boot, v->d.bytes, sizeof(boot));
    for (mode = 0; mode < 9; mode++) {
        memcpy(v->d.bytes, boot, sizeof(boot));
        dev = v->dev;
        switch (mode) {
        case 0: put16(v->d.bytes + 510, 0); break;
        case 1: dev.bytesPerSector = 1; break;
        case 2: dev.bytesPerSector = 8192; break;
        case 3:
            if (type == 32)
                put32(v->d.bytes + 0x24, 1);
            else
                put16(v->d.bytes + 0x16, 1);
            break;
        case 4:
            put16(v->d.bytes + 0x13, 0);
            put16(v->d.bytes + 0x16, 0);
            put32(v->d.bytes + 0x20, 0x10000000);
            put32(v->d.bytes + 0x24, 0x80000001);
            break;
        case 5:
            put16(v->d.bytes + 0x13, 0);
            put16(v->d.bytes + 0x16, 0);
            put32(v->d.bytes + 0x20, 0x10000000);
            put32(v->d.bytes + 0x24, 0x00800001);
            break;
        case 6:
            if (type == 32)
                put16(v->d.bytes + 0x2A, 1);
            else
                put16(v->d.bytes + 0x11, 511);
            break;
        case 7: v->d.bytes[0x0D] = 0; break;
        case 8: dev.read = NULL; break;
        }
        reads = v->d.reads;
        CHECK(!fat_mount(&fv, &dev));
        CHECK(fv.fat == NULL && fv.dirtyMap == NULL);
        CHECK(v->d.reads <= reads + 1 && v->d.writes == 0);
    }
    memcpy(v->d.bytes, boot, sizeof(boot));
    CHECK(fat_mount(&fv, &v->dev));
    reads = v->d.reads;
    writes = v->d.writes;
    CHECK(!fat_read_sectors(&fv, 0xFFFFFFFFu, 2, buf));
    CHECK(!fat_write_sectors(&fv, 0xFFFFFFFFu, 2, buf));
    CHECK(!fat_read_sectors(&fv, v->totalSectors, 1, buf));
    CHECK(!fat_write_sectors(&fv, v->totalSectors, 1, buf));
    CHECK(v->d.reads == reads && v->d.writes == writes);
    CHECK(fat_is_eoc(&fv, fat_get(&fv, 0x40000000u)));
    fat_set(&fv, 0x40000000u, 0);
    CHECK(!fv.fatDirty);
    fat_unmount(&fv);
    tv_free(v);
}

static void test_scan_refusals(int type)
{
    int mode;

    for (mode = 0; mode < 7; mode++) {
        unsigned rng = 81;
        dm_nodes x;
        tvol *v = build_dirmove(type, &x, &rng);
        fat_volume fv;
        fat_defrag_cfg cfg;
        fat_stats st;
        unsigned char *e, *snapshot;
        size_t bytes = (size_t)v->totalSectors * 512;

        CHECK(tv_fsck(v, NULL));
        e = v->d.bytes + v->n[x.s1].eoff;
        switch (mode) {
        case 0: set_ent_start(v, e, 0); break;
        case 1: put32(e + 28, 0xFFFFFFFFu); break;
        case 2: tv_set(v, v->n[x.s1].start, 0); break;
        case 3: tv_fat(v, 1)[100] ^= 1; break;
        case 4:
            v->d.failReadSector = v->firstDataSector +
                                  (v->n[x.sub].start - 2) * v->spc + 1;
            break;
        case 5: set_ent_start(v, e, v->n[x.s2].start); break;
        case 6:
            set_ent_start(v, tv_cluster(v, v->n[x.sub].start) + 32, 0);
            break;
        }
        snapshot = (unsigned char *)malloc(bytes);
        CHECK(snapshot != NULL);
        memcpy(snapshot, v->d.bytes, bytes);
        CHECK(fat_mount(&fv, &v->dev));
        memset(&cfg, 0, sizeof(cfg));
        cfg.doMove = cfg.compact = cfg.verify = 1;
        CHECK(fat_run(&fv, &cfg, &st) == 1);
        CHECK(st.chainErrors || st.crossLinked || st.ioErrors);
        CHECK(st.clustersMoved == 0 && v->d.writes == 0);
        CHECK(memcmp(snapshot, v->d.bytes, bytes) == 0);
        fat_unmount(&fv);
        free(snapshot);
        tv_free(v);
    }
}

static void test_bad_lfn(int type)
{
    unsigned rng = 82;
    dm_nodes x;
    tvol *v = build_dirmove(type, &x, &rng);
    fat_volume fv;
    unsigned char *e, saved[32];
    int ordinal;

    CHECK(tv_fsck(v, NULL));
    e = v->d.bytes + v->n[x.b].lfnOff[0];
    memcpy(saved, e, sizeof(saved));
    CHECK(saved[0] == 1);
    CHECK(fat_mount(&fv, &v->dev));
    for (ordinal = 0; ordinal < 256; ordinal++) {
        if (ordinal == 1)
            continue;
        memcpy(e, saved, sizeof(saved));
        e[0] = (unsigned char)ordinal;
        check_lookup(&fv, "\\Bee data file.dat", 0, 0);
        check_lookup(&fv, "\\B.DAT", ordinal != 0, v->n[x.b].start);
    }
    memcpy(e, saved, sizeof(saved));
    e[12] = 1;
    check_lookup(&fv, "\\Bee data file.dat", 0, 0);
    memcpy(e, saved, sizeof(saved));
    put16(e + 26, 1);
    check_lookup(&fv, "\\Bee data file.dat", 0, 0);
    memcpy(e, saved, sizeof(saved));
    check_lookup(&fv, "\\Bee data file.dat", 1, v->n[x.b].start);
    fat_unmount(&fv);
    tv_free(v);
}

static void test_deep_tree(void)
{
    unsigned rng = 83;
    tvol *v = tv_new(16, 5000, 1);
    fat_volume fv;
    fat_defrag_cfg cfg;
    fat_stats st;
    int parent = 0, i;
    fat_u32 cl;

    for (i = 0; i < 65; i++) {
        parent = tv_node(v, parent, 1, "DIR        ", NULL);
        cl = 100 + (fat_u32)i;
        place(v, parent, &cl, 1, &rng);
    }
    cl = 400;
    place(v, tv_node(v, 0, 0, "FILE    DAT", NULL), &cl, 1, &rng);
    tv_commit(v);
    CHECK(tv_fsck(v, NULL));
    CHECK(fat_mount(&fv, &v->dev));
    memset(&cfg, 0, sizeof(cfg));
    cfg.doMove = cfg.compact = cfg.verify = 1;
    CHECK(fat_run(&fv, &cfg, &st) == 1);
    CHECK(st.chainErrors > 0 && st.clustersMoved == 0 && v->d.writes == 0);
    CHECK(tv_fsck(v, NULL));
    fat_unmount(&fv);
    tv_free(v);
}

static void test_dir_move(int type)
{
    unsigned rng = 11u + (unsigned)type;
    dm_nodes x;
    tvol *v = build_dirmove(type, &x, &rng);
    fat_volume fv;
    hctx h;
    fat_stats an, st, re;
    fat_u32 oldDir;

    tv_mount(v, &fv);
    h_init(&h, v, &fv, 1);
    CHECK(tv_fsck(v, NULL));
    CHECK(v->n[x.dir1].frags == 3 && v->n[x.b].frags == 3);
    oldDir = v->n[x.dir1].start;
    lookups(v, &fv, &x);

    CHECK(run(&h, 0, 0, &an) == 0);
    check_measure(&h, &an);
    CHECK(an.fragmented == 2 && an.dirsFragmented == 1);
    CHECK(an.planDefrag == 3 && an.compactable > 0 && an.planFreeRuns == 1);
    CHECK(h.ev[FAT_EV_FRAGMENTED] == 3);
    CHECK(h.ev[FAT_EV_CONTIGUOUS] == an.contiguous - 1); /* empty: silent */

    CHECK(run(&h, 1, 1, &st) == 0);
    CHECK(tv_fsck(v, NULL));
    CHECK(st.defragged == 2 && st.dirsDefragged == 1);
    CHECK(st.compacted == an.compactable && st.freeRuns == 1);
    CHECK(st.freeGaps == 0 && frag_movable(&h) == 0);
    CHECK(h.ev[FAT_EV_DIRDEFRAGGED] == 1 && h.ev[FAT_EV_DEFRAGGED] == 2);
    CHECK(v->n[x.dir1].start != oldDir);
    CHECK(type != 32 || fsinfo_free(v) == 0xFFFFFFFFu);
    lookups(v, &fv, &x);

    CHECK(run(&h, 0, 0, &re) == 0);
    check_measure(&h, &re);
    CHECK(re.planDefrag == 0 && re.compactable == 0 && re.planFreeRuns == 1);
    fat_unmount(&fv);
    tv_free(v);
}

/*
 * Boot files, the swap file, the registry and files in use stay put. In
 * MS-DOS mode (dos) nothing is in use and the swap file and the registry are
 * ordinary files: only IO.SYS and LDLINUX.SYS stay. Hidden, system and
 * read-only files elsewhere always move.
 */
static void test_unmovable(int type, int dos)
{
    static const fat_u32 io[] = {40, 41, 90, 91}, swp[] = {60, 61, 62, 130};
    static const fat_u32 dll[] = {100, 101, 150}, f1[] = {20, 21, 170};
    static const fat_u32 f2[] = {5, 6, 7}, f3[] = {180, 8}, win[] = {200};
    static const fat_u32 reg[] = {210, 211, 230}, idx[] = {220, 240, 241};
    static const fat_u32 ldl[] = {250, 251, 260}, hid[] = {270, 280};
    unsigned rng = 21u + (unsigned)type;
    tvol *v = tv_new(type, type == 32 ? 66000 : type == 16 ? 4200 : 3000,
                     type == 12 ? 2 : 1);
    fat_volume fv;
    hctx h;
    fat_stats an, st;
    int nio, nswp, ndll, n1, n2, n3, nwin, nreg, nidx, nldl, nhid;
    fat_u32 sIo, sSwp, sDll, sReg, sLdl;

    if (type == 32)
        tv_root32(v, 2, 0, 0, &rng);
    nio = tv_node(v, 0, 0, "IO      SYS", NULL);
    v->n[nio].attr = 0x27;
    nswp = tv_node(v, 0, 0, "WIN386  SWP", NULL);
    ndll = tv_node(v, 0, 0, "KERNEL32DLL", "Kernel32.dll");
    n1 = tv_node(v, 0, 0, "F1      TXT", NULL);
    n2 = tv_node(v, 0, 0, "F2      TXT", NULL);
    n3 = tv_node(v, 0, 0, "F3      TXT", NULL);
    nhid = tv_node(v, 0, 0, "HIDDEN  TXT", NULL);   /* read-only, hidden */
    v->n[nhid].attr = 0x23;
    nwin = tv_node(v, 0, 1, "WINDOWS    ", NULL);
    nreg = tv_node(v, nwin, 0, "SYSTEM  DAT", NULL);
    v->n[nreg].attr = 0x27;
    nidx = tv_node(v, nwin, 0, "INDEX   DAT", NULL);
    v->n[nidx].attr = 0x26;
    nldl = tv_node(v, nwin, 0, "LDLINUX SYS", NULL);
    v->n[nldl].attr = 0x27;
    place(v, nio, io, 4, &rng);
    place(v, nswp, swp, 4, &rng);
    place(v, ndll, dll, 3, &rng);
    place(v, n1, f1, 3, &rng);
    place(v, n2, f2, 3, &rng);
    place(v, n3, f3, 2, &rng);
    place(v, nhid, hid, 2, &rng);
    place(v, nwin, win, 1, &rng);
    place(v, nreg, reg, 3, &rng);
    place(v, nidx, idx, 3, &rng);
    place(v, nldl, ldl, 3, &rng);
    tv_commit(v);
    tv_mount(v, &fv);
    h_init(&h, v, &fv, 2);
    if (!dos)
        h_pin(&h, ndll);
    g_offline = dos;
    CHECK(tv_fsck(v, NULL));
    sIo = v->n[nio].start;
    sSwp = v->n[nswp].start;
    sDll = v->n[ndll].start;
    sReg = v->n[nreg].start;
    sLdl = v->n[nldl].start;

    CHECK(run(&h, 0, 0, &an) == 0);
    check_measure(&h, &an);
    CHECK(an.unmovable == (dos ? 2u : 5u));
    CHECK(an.unmovableFragmented == an.unmovable);
    CHECK(an.planDefrag == (dos ? 7u : 4u));

    CHECK(run(&h, 1, 1, &st) == 0);
    CHECK(tv_fsck(v, NULL));
    CHECK(st.defragged == an.planDefrag && st.unmovable == an.unmovable);
    CHECK(st.compacted == an.compactable && st.freeRuns == an.planFreeRuns);
    CHECK(h.ev[FAT_EV_UNMOVABLE] == an.unmovable);
    CHECK(v->n[nio].start == sIo && v->n[nio].frags == 2);
    CHECK(v->n[nldl].start == sLdl && v->n[nldl].frags == 2);
    if (dos) {
        CHECK(v->n[nswp].frags == 1 && v->n[ndll].frags == 1);
        CHECK(v->n[nreg].frags == 1);
    } else {
        CHECK(v->n[nswp].start == sSwp && v->n[nswp].frags == 2);
        CHECK(v->n[ndll].start == sDll && v->n[ndll].frags == 2);
        CHECK(v->n[nreg].start == sReg && v->n[nreg].frags == 2);
    }
    CHECK(v->n[n1].frags == 1 && v->n[n2].frags == 1 && v->n[n3].frags == 1);
    CHECK(v->n[nidx].frags == 1 && v->n[nhid].frags == 1);
    CHECK(h.unmInUse == (dos ? 0u : 1u));
    check_gap_notes(&h, &st);
    same_notes(&an, &st);
    g_offline = 0;
    fat_unmount(&fv);
    tv_free(v);
}

/*
 * Hidden system files in the root folder stay, except the logs, backups,
 * drivers and settings MS-DOS and Windows open by name and the text MSDOS.SYS
 * of Windows 95/98 (msdos 0). An MS-DOS 6 MSDOS.SYS (1: not text) or a text
 * one too big to be that (2) stays. Elsewhere such names move; empty files
 * are not counted. Boot-loader protection must not depend on attributes.
 */
static void test_root_files(int type, int dos, int msdos, int attr)
{
    static const char extra[4][12] = {"IO      SYS", "DRVSPACE000",
                                      "MSDOS   SYS", "EMPTY   SYS"};
    enum { N_ALL = N_ROOT_MOVABLE + 4 };
    unsigned rng = 41u + (unsigned)type * 3u + (unsigned)msdos;
    tvol *v = tv_new(type, type == 32 ? 66000 : type == 16 ? 4200 : 3000, 1);
    fat_volume fv;
    hctx h;
    fat_stats an, st;
    int node[N_ALL], keep[N_ALL], k, kept = 0, sub, subIo, plan;
    fat_u32 start[N_ALL], cls[40], j;

    for (k = 0; k < N_ALL; k++) {
        node[k] = tv_node(v, 0, 0, k < N_ROOT_MOVABLE ? t_root_movable[k]
                                   : extra[k - N_ROOT_MOVABLE], NULL);
        v->n[node[k]].attr = 0x27;
        if (k == N_ROOT_MOVABLE || k == N_ROOT_MOVABLE + 2)
            v->n[node[k]].attr = 0x20 | attr;
    }
    sub = tv_node(v, 0, 1, "SUB        ", NULL);
    subIo = tv_node(v, sub, 0, "IO      SYS", NULL);
    v->n[subIo].attr = 0x27;
    if (type == 32)
        tv_root32(v, tv_dir_clusters(v, 0, 4), 0, 0, &rng);
    for (k = 0; k < N_ALL - 1; k++) { /* the last one stays empty */
        int ms = k == N_ROOT_MOVABLE + 2;
        fat_u32 len = ms ? (msdos == 2 ? 33u : 4u) : 2u + (fat_u32)k % 3u;

        for (j = 0; j + 1 < len; j++)
            cls[j] = 200 + 50 * (fat_u32)k + j;
        cls[len - 1] = 2200 + 20 * (fat_u32)k;
        place(v, node[k], cls, len, &rng);
        if (ms) {
            if (msdos != 1)
                v->n[node[k]].id |= TEXT_ID;
            if (msdos == 2)
                v->n[node[k]].size = 16385;
        }
    }
    cls[0] = 150;
    place(v, sub, cls, 1, &rng);
    cls[0] = 160;
    cls[1] = 2195;
    place(v, subIo, cls, 2, &rng);
    tv_commit(v);
    tv_mount(v, &fv);
    h_init(&h, v, &fv, 4);
    g_offline = dos;
    CHECK(tv_fsck(v, NULL));
    for (k = 0; k < N_ALL; k++) {
        const unsigned char *nm = v->n[node[k]].name;

        start[k] = v->n[node[k]].start;
        keep[k] = memcmp(nm, "IO      SYS", 11) == 0 ||
                  memcmp(nm, "DRVSPACE000", 11) == 0 ||
                  memcmp(nm, "EMPTY   SYS", 11) == 0 ||
                  (msdos && memcmp(nm, "MSDOS   SYS", 11) == 0) ||
                  (!dos && memcmp(nm, "WIN386  SWP", 11) == 0);
        CHECK(is_system(v, node[k]) == keep[k]);
        kept += keep[k] && start[k];
    }
    CHECK(!is_system(v, subIo));
    CHECK(kept == (dos ? 2 : 3) + (msdos != 0));
    plan = frag_movable(&h);
    CHECK(plan == N_ALL - 1 - kept + 1);

    CHECK(run(&h, 0, 0, &an) == 0);
    check_measure(&h, &an);
    CHECK(an.unmovable == (fat_u64)kept && an.unmovableFragmented == an.unmovable);
    CHECK(an.planDefrag == (fat_u64)plan);

    CHECK(run(&h, 1, 1, &st) == 0);
    CHECK(tv_fsck(v, NULL));
    CHECK(st.defragged == an.planDefrag && st.unmovable == an.unmovable);
    CHECK(h.ev[FAT_EV_UNMOVABLE] == st.unmovable);
    CHECK(h.unmSystem == st.unmovable && h.unmInUse == 0);
    for (k = 0; k < N_ALL - 1; k++) {
        if (keep[k])
            CHECK(v->n[node[k]].start == start[k] && v->n[node[k]].frags == 2);
        else
            CHECK(v->n[node[k]].frags == 1);
    }
    CHECK(v->n[subIo].frags == 1 && frag_movable(&h) == 0);
    check_gap_notes(&h, &st);
    same_notes(&an, &st);
    g_offline = 0;
    fat_unmount(&fv);
    tv_free(v);
}

/*
 * The report names what keeps each of the largest gaps in the free space
 * open: a system file, a file in use, the FAT32 root folder, lost or bad
 * clusters, or a file that was not moved. Lost clusters are counted in
 * MS-DOS mode only.
 */
static void test_gap_notes(int type, int offline)
{
    static const fsck_opt lostOk = {1, 0, 0, 0};
    static const fat_u32 a[] = {10, 11, 12, 13, 14}, b[] = {20, 21, 22};
    static const fat_u32 c1[] = {30}, io[] = {400, 401}, pin[] = {850, 851};
    static const fat_u32 z[] = {1500, 1501, 1502};
    unsigned rng = 51u + (unsigned)type + (unsigned)offline;
    tvol *v = tv_new(type, type == 32 ? 66000 : type == 16 ? 4200 : 3000, 1);
    fat_volume fv;
    hctx h;
    fat_stats stuck, an, st, re;
    int na, nb, nc, nio, npin, nz, nemp;
    fat_u32 first = type == 32 ? 3 : 2, k;

    if (type == 32) {
        /* A root folder that starts at 1000 and goes on at 2: its first
         * cluster stays, and nothing lies below the rest. */
        tnode *r = &v->n[0];

        r->nCl = 2;
        r->cls = (fat_u32 *)malloc(3 * sizeof(fat_u32));
        CHECK(r->cls != NULL);
        r->cls[0] = 1000;
        r->cls[1] = 2;
        v->used[2] = v->used[1000] = 1;
    }
    na = tv_node(v, 0, 0, "A       DAT", NULL);
    nb = tv_node(v, 0, 0, "B       DAT", "Bee file.dat");
    nc = tv_node(v, 0, 0, "C       DAT", NULL);
    nio = tv_node(v, 0, 0, "IO      SYS", NULL);
    v->n[nio].attr = 0x27;
    npin = tv_node(v, 0, 0, "PINNED  DAT", NULL);
    nz = tv_node(v, 0, 0, "Z       DAT", NULL);
    nemp = tv_node(v, 0, 0, "EMPTY   SYS", NULL);
    v->n[nemp].attr = 0x27;
    place(v, na, a, 5, &rng);
    place(v, nb, b, 3, &rng);
    place(v, nc, c1, 1, &rng);
    place(v, nio, io, 2, &rng);
    place(v, npin, pin, 2, &rng);
    place(v, nz, z, 3, &rng);
    tv_commit(v);
    for (k = 600; k < 603; k++) /* lost clusters */
        tv_set(v, k, k + 1);
    tv_set(v, 603, tv_eoc(v));
    tv_set(v, 750, tv_bad(v));
    tv_mount(v, &fv);
    h_init(&h, v, &fv, 5);
    h_pin(&h, npin);
    h_pin(&h, nio); /* open too: still reported as a system file */
    g_offline = offline;
    CHECK(tv_fsck(v, &lostOk));

    /* Nothing can be written: every gap stays, the largest before Z.DAT. */
    h.failFrom = 1;
    run(&h, 1, 1, &stuck);
    h.failFrom = 0;
    CHECK(stuck.clustersMoved == 0);
    CHECK(tv_fsck(v, &lostOk));
    check_gap_notes(&h, &stuck);
    CHECK(stuck.gapsLeft == (type == 32 ? 9u : 8u));
    CHECK(stuck.gapNote[0].why == FAT_GAP_STUCK);
    CHECK(strcmp(stuck.gapNote[0].path, "\\Z.DAT") == 0);
    CHECK(stuck.gapNote[0].clusters == (type == 32 ? 499u : 648u));

    CHECK(run(&h, 0, 0, &an) == 0);
    check_measure(&h, &an);
    CHECK(an.unmovable == 2);
    CHECK(run(&h, 1, 1, &st) == 0);
    CHECK(tv_fsck(v, &lostOk));
    check_gap_notes(&h, &st);
    same_notes(&an, &st);
    CHECK(h.unmSystem == 1 && h.unmInUse == 1 && st.unmovable == 2);
    CHECK(st.lostClusters == (offline ? 4u : 0u));
    CHECK(st.gapsLeft == (type == 32 ? 5u : 4u) && st.gapNotes == 4);
    CHECK(st.gapNote[0].why == FAT_GAP_SYSTEM);
    CHECK(st.gapNote[0].clusters == 400 - first - 12);
    CHECK(strcmp(st.gapNote[0].path, "\\IO.SYS") == 0);
    CHECK(st.gapNote[1].why == FAT_GAP_LOST && st.gapNote[1].clusters == 198);
    if (type == 32) {
        CHECK(st.gapNote[2].why == FAT_GAP_ROOT);
        CHECK(st.gapNote[2].clusters == 148);
        CHECK(strcmp(st.gapNote[2].path, "\\") == 0);
        CHECK(st.gapNote[3].why == FAT_GAP_BAD);
    } else {
        CHECK(st.gapNote[2].why == FAT_GAP_BAD);
        CHECK(st.gapNote[2].clusters == 146);
        CHECK(st.gapNote[3].why == FAT_GAP_INUSE);
        CHECK(st.gapNote[3].clusters == 99);
        CHECK(strcmp(st.gapNote[3].path, "\\PINNED.DAT") == 0);
    }
    CHECK(run(&h, 0, 0, &re) == 0);
    CHECK(re.planDefrag == 0 && re.compactable == 0);
    same_notes(&re, &st);
    g_offline = 0;
    fat_unmount(&fv);
    tv_free(v);
}

/* A file too big for every gap, whose own pieces split the free space: its
 * pieces are moved together, each switched over through the FAT entry before
 * it. On FAT16/32 a big file fills the rest of the volume. */
static void test_piece_compact(int type)
{
    unsigned rng = 31u + (unsigned)type;
    fat_u32 n = type == 32 ? 66000 : type == 16 ? 4200 : 60;
    fat_u32 b = type == 32 ? 4 : 2, cls[30], k;
    tvol *v = tv_new(type, n, 1);
    fat_volume fv;
    hctx h;
    fat_stats an, st, re;
    int a, f = -1;

    if (type == 32)
        tv_root32(v, 2, 0, 0, &rng);
    a = tv_node(v, 0, 0, "A       DAT", NULL);
    for (k = 0; k < 30; k++) /* b..b+9, b+20..b+29, b+40..b+49 */
        cls[k] = b + k + k / 10 * 10;
    place(v, a, cls, 30, &rng);
    if (b + 60 <= n + 1) {
        fat_u32 nf = n + 1 - (b + 60) + 1;
        fat_u32 *fc = (fat_u32 *)malloc(nf * sizeof(fat_u32));

        CHECK(fc != NULL);
        for (k = 0; k < nf; k++)
            fc[k] = b + 60 + k;
        f = tv_node(v, 0, 0, "FILL    DAT", NULL);
        place(v, f, fc, nf, &rng);
        free(fc);
    }
    tv_commit(v);
    tv_mount(v, &fv);
    h_init(&h, v, &fv, 3);
    CHECK(tv_fsck(v, NULL));
    CHECK(v->n[a].frags == 3);

    CHECK(run(&h, 0, 0, &an) == 0);
    check_measure(&h, &an);
    CHECK(an.planDefrag == 1 && an.compactable == 0 && an.planFreeRuns == 1);

    CHECK(run(&h, 1, 1, &st) == 0);
    CHECK(tv_fsck(v, NULL));
    CHECK(v->n[a].frags == 1 && v->n[a].start == b);
    CHECK(st.defragged == 1 && st.noSpace == 0 && st.compacted == 0);
    CHECK(st.clustersMoved == 20 && st.freeRuns == 1);
    CHECK(h.ev[FAT_EV_NOSPACE] == 1 && h.ev[FAT_EV_DEFRAGGED] == 1);
    CHECK(h.ev[FAT_EV_MOVED] == 2);
    CHECK(f < 0 || v->n[f].start == b + 60);

    CHECK(run(&h, 0, 0, &re) == 0);
    check_measure(&h, &re);
    CHECK(re.planDefrag == 0 && re.compactable == 0);
    fat_unmount(&fv);
    tv_free(v);
}

/* Nothing fits in the gap in front of the last file: the file slides down
 * through it in steps of the gap or, when that would take more than 64
 * steps, moves up out of the way and comes back down whole. */
static void test_slide(fat_u32 gap, fat_u32 len)
{
    unsigned rng = 51u + gap;
    tvol *v = tv_new(16, 4200, 1);
    fat_u32 *cls = (fat_u32 *)malloc(len * sizeof(fat_u32)), k, one = 2;
    fat_volume fv;
    hctx h;
    fat_stats an, st, re;
    int a, b, trip = (len - 1) / gap + 1 > 64;

    CHECK(cls != NULL);
    a = tv_node(v, 0, 0, "A       DAT", NULL);
    place(v, a, &one, 1, &rng);
    b = tv_node(v, 0, 0, "BIG     DAT", NULL);
    for (k = 0; k < len; k++)
        cls[k] = 3 + gap + k;
    place(v, b, cls, len, &rng);
    free(cls);
    tv_commit(v);
    tv_mount(v, &fv);
    h_init(&h, v, &fv, 5);
    CHECK(tv_fsck(v, NULL));

    CHECK(run(&h, 0, 0, &an) == 0);
    check_measure(&h, &an);
    CHECK(an.planDefrag == 0 && an.compactable == 1 && an.planFreeRuns == 1);

    CHECK(run(&h, 1, 1, &st) == 0);
    CHECK(tv_fsck(v, NULL));
    CHECK(st.compacted == 1 && st.freeRuns == 1 && st.freeGaps == 0);
    CHECK(v->n[a].start == 2 && v->n[b].start == 3 && v->n[b].frags == 1);
    CHECK(st.clustersMoved == (trip ? 2 * len : len));
    CHECK(h.ev[FAT_EV_MOVED] == (trip ? 2u : 1u) && h.movedInPieces == 0);

    CHECK(run(&h, 0, 0, &re) == 0);
    CHECK(re.compactable == 0 && re.planFreeRuns == 1);
    fat_unmount(&fv);
    tv_free(v);
}

/* A fragmented folder that fits nowhere: its first piece moves first, which
 * updates its "." and the ".." of its subfolder while the folder is still in
 * pieces, then the whole folder. Empty files with long names put the
 * subfolder's entry in the folder's third cluster, which is not next to the
 * moved first piece. The file at 14 is in the subfolder, or with fileInSub
 * zero in the root folder, so that moving it does not touch the subfolder. */
static tvol *build_piece_dir(int fileInSub, int *ndOut)
{
    static const fat_u32 d[] = {10, 11, 3, 4}, sub[] = {7}, f[] = {14};
    unsigned rng = 41u;
    tvol *v = tv_new(12, 16, 1);
    char n83[12], lfn[48];
    int nd, ns, nf, k;

    nd = tv_node(v, 0, 1, "DIR        ", "Fragmented folder");
    for (k = 0; k < 6; k++) { /* 6 x (4 long-name parts + 1) = slots 3..32 */
        sprintf(n83, "EMPTY%d  TXT", k);
        sprintf(lfn, "An empty file with a long name, number %d.txt", k);
        CHECK(lfn_parts(&v->n[tv_node(v, nd, 0, n83, lfn)]) == 4);
    }
    ns = tv_node(v, nd, 1, "SUB        ", NULL);
    CHECK(tv_dir_slots(v, nd) == 34);
    nf = tv_node(v, fileInSub ? ns : 0, 0, "F       TXT", NULL);
    place(v, nd, d, 4, &rng);
    place(v, ns, sub, 1, &rng);
    place(v, nf, f, 1, &rng);
    tv_commit(v);
    *ndOut = nd;
    return v;
}

static void test_piece_dir(void)
{
    int nd;
    tvol *v = build_piece_dir(1, &nd);
    fat_volume fv;
    hctx h;
    fat_stats an, st, re;

    tv_mount(v, &fv);
    h_init(&h, v, &fv, 4);
    CHECK(tv_fsck(v, NULL));
    CHECK(v->n[nd].frags == 2);

    CHECK(run(&h, 0, 0, &an) == 0);
    check_measure(&h, &an);
    CHECK(an.planDefrag == 1 && an.compactable == 1 && an.planFreeRuns == 1);

    CHECK(run(&h, 1, 1, &st) == 0);
    CHECK(tv_fsck(v, NULL));
    CHECK(st.dirsDefragged == 1 && st.noSpace == 0 && st.compacted == 1);
    CHECK(st.freeRuns == 1 && st.freeGaps == 0 && frag_movable(&h) == 0);
    CHECK(h.ev[FAT_EV_NOSPACE] == 1 && h.ev[FAT_EV_DIRDEFRAGGED] == 1);
    CHECK(h.movedInPieces == 1);
    CHECK(v->n[nd].frags == 1);

    CHECK(run(&h, 0, 0, &re) == 0);
    check_measure(&h, &re);
    CHECK(re.planDefrag == 0 && re.compactable == 0);
    fat_unmount(&fv);
    tv_free(v);
}

/*
 * The folder of test_piece_dir (10, 11, 3, 4, subfolder at 7) whose first
 * piece moves, while the subfolder's ".." cannot be fixed. If the subfolder
 * cannot even be read once writing has started, the move must not happen
 * at all: the volume stays exactly as it was. If only writing its ".."
 * fails, the old first piece (10, 11) is kept for the stale ".." but must
 * end at 11 instead of still leading into the folder's live cluster 3. The
 * run is stopped right after that, so the check sees the volume as later
 * moves (or other programs) would find it.
 */
static void test_piece_dir_faults(void)
{
    static const fsck_opt loose = {1, 1, 0, 0};
    int mode;

    for (mode = 0; mode < 2; mode++) {
        int nd;
        tvol *v = build_piece_dir(0, &nd);
        fat_volume fv;
        hctx h;
        fat_stats st;
        fat_u32 subSector = v->firstDataSector + (7 - 2) * v->spc;
        int rc;

        tv_mount(v, &fv);
        h_init(&h, v, &fv, 4);
        CHECK(tv_fsck(v, NULL));
        if (mode == 0) {
            v->d.failReadSector = subSector + 1;
            v->d.failReadAfter = 1;
        } else {
            v->d.failWriteSector = subSector + 1;
            h.stopOnDotdot = 1;
        }
        rc = run(&h, 1, 1, &st);
        v->d.failReadSector = v->d.failWriteSector = 0;
        CHECK(rc == 1 && st.ioErrors > 0);
        if (mode == 0) {
            /* F.DAT moved first (14 -> 2); the folder's piece did not. */
            CHECK(st.clustersMoved == 1 && st.dotdotErrors == 0);
            CHECK(tv_fsck(v, NULL));
            CHECK(v->n[nd].start == 10 && v->n[nd].frags == 2);
        } else {
            CHECK(st.dotdotErrors == 1 && st.aborted);
            CHECK(tv_fsck(v, &loose));
            CHECK(tv_get(v, 0, 10) == 11 && tv_is_eoc(v, tv_get(v, 0, 11)));
            CHECK(ent_start(v, tv_cluster(v, 7) + 32) == 10);
            CHECK(v->n[nd].start != 10 && v->n[nd].frags == 2);
        }
        fat_unmount(&fv);
        tv_free(v);
    }
}

/*
 * The first switch of the run fails after the copy's chain was published:
 * the move is undone and nothing moved, but the FAT was written, so the
 * FSInfo free count must not be trusted any more.
 */
static void test_fsinfo_first_switch(void)
{
    unsigned rng = 61u;
    dm_nodes x;
    tvol *v = build_dirmove(32, &x, &rng);
    fat_volume fv;
    hctx h;
    fat_stats st;
    int rc;

    tv_mount(v, &fv);
    h_init(&h, v, &fv, 6);
    CHECK(tv_fsck(v, NULL));
    CHECK(fsinfo_free(v) != 0xFFFFFFFFu);
    /* B.DAT is the first fragmented object: fail writing its entry. */
    v->d.failWriteSector = (fat_u32)(v->n[x.b].eoff / 512) + 1;
    rc = run(&h, 1, 1, &st);
    CHECK(v->d.failWriteSector == 0);
    CHECK(rc == 1 && st.ioErrors > 0 && st.clustersMoved == 0);
    CHECK(tv_fsck(v, NULL));
    CHECK(v->n[x.b].frags == 3);
    CHECK(fsinfo_free(v) == 0xFFFFFFFFu);
    fat_unmount(&fv);
    tv_free(v);
}

/* A 1.3 MB file is copied across several write sections. */
static tvol *build_big(unsigned *rng, int *big)
{
    tvol *v = tv_new(16, 16000, 1);
    fat_u32 *cls = (fat_u32 *)malloc(2600 * sizeof(fat_u32)), k;
    int i;

    CHECK(cls != NULL);
    for (k = 0; k < 2600; k++)
        cls[k] = 1000 * (1 + k / 650) + k % 650;
    *big = tv_node(v, 0, 0, "BIG     DAT", "A big file.dat");
    place(v, *big, cls, 2600, rng);
    for (i = 0; i < 20; i++) {
        char nm[32];
        int f;

        snprintf(nm, sizeof(nm), "S%07dTXT", i);
        f = tv_node(v, 0, 0, nm, NULL);
        for (k = 0; k < 5; k++)
            cls[k] = 100 + (fat_u32)i * 30 + k;
        place(v, f, cls, 5, rng);
    }
    free(cls);
    tv_commit(v);
    return v;
}

static void test_big_file(void)
{
    int mode;

    for (mode = 0; mode < 3; mode++) {
        unsigned rng = 31;
        int big;
        tvol *v = build_big(&rng, &big);
        fat_volume fv;
        hctx h;
        fat_stats st;
        fat_u32 id0 = v->n[big].id;
        int rc;

        tv_mount(v, &fv);
        h_init(&h, v, &fv, 3);
        h.mutKind = MU_REWRITE;
        h.mutTarget = big;
        if (mode == 0)
            h.mutAt = 2;   /* rewritten while its copy is half done      */
        else if (mode == 1)
            h.mutPct = 100; /* rewritten all the time                    */
        else
            h.busyPct = 40; /* lock often unavailable                    */
        rc = run(&h, 1, 1, &st);
        CHECK(tv_fsck(v, NULL));
        if (mode == 0) {
            CHECK(rc == 0 && h.mutations == 1 && v->n[big].id != id0);
            CHECK(st.defragged == 1 && v->n[big].frags == 1);
        } else if (mode == 1) {
            CHECK(rc == 1 && st.busy == 1 && h.ev[FAT_EV_BUSY] == 1);
            CHECK(st.defragged == 0 && v->n[big].frags == 4);
        } else {
            CHECK(rc == 0 && st.defragged == 1 && v->n[big].frags == 1);
            CHECK(st.freeRuns == 1);
        }
        fat_unmount(&fv);
        tv_free(v);
    }
}

static const tgen g_small16 = {16, 4200, 1, 160, 55, 90, 50, 0, 0};

/* Ctrl+C (begin fails), a lock that is never available, a stale FAT. */
static void test_hook_faults(void)
{
    int mode;

    for (mode = 0; mode < 4; mode++) {
        unsigned rng = 41;
        tvol *v = tv_random(&g_small16, &rng);
        fat_volume fv;
        hctx h;
        fat_stats an, st;
        int rc, k;

        tv_mount(v, &fv);
        h_init(&h, v, &fv, 4);
        CHECK(run(&h, 0, 0, &an) == 0);
        CHECK(an.planDefrag > 5);
        if (mode == 0) {
            h.failFrom = 3;
        } else if (mode == 1) {
            h.busyPct = 100;
        } else if (mode == 2) {
            h.busyPct = 25;
        } else {
            /* Other programs wrote after the FAT was read at mount time. */
            h.mutKind = MU_CREATE;
            for (k = 0; k < 6; k++)
                mutate(&h);
            h.mutKind = MU_EXTEND;
            for (k = 0; k < 6; k++)
                mutate(&h);
            h.mutKind = -1;
            h.pending = 1;
        }
        rc = run(&h, 1, 1, &st);
        CHECK(tv_fsck(v, NULL));
        if (mode == 0) {
            CHECK(rc == 1 && st.aborted && h.begins >= 3);
        } else if (mode == 1) {
            CHECK(rc == 1 && st.aborted && st.clustersMoved == 0);
            CHECK(h.opened == 0 && v->d.writes == 0);
        } else {
            CHECK(rc == 0 && frag_movable(&h) == 0 && st.freeRuns == 1);
            if (mode == 2)
                CHECK(st.defragged + st.dirsDefragged == an.planDefrag &&
                      st.compacted == an.compactable);
            else
                CHECK(h.ev[FAT_EV_CHAINERROR] > 0 && st.chainErrors == 0);
        }
        if (mode < 2) {
            /* A later undisturbed run finishes the job. */
            hctx h2;

            h_init(&h2, v, &fv, 5);
            CHECK(run(&h2, 1, 1, &st) == 0);
            CHECK(tv_fsck(v, NULL));
            CHECK(frag_movable(&h2) == 0 && st.freeRuns == 1);
        }
        fat_unmount(&fv);
        tv_free(v);
    }
}

/* ============================================================ stress tests */

static const tgen g_cases[] = {
    /* type  clusters spc nodes budget% limit% frag% sys fragRoot */
    {12, 3000,  2, 220, 60, 92, 50, 0, 0},
    {12, 4000,  1, 300, 70, 95, 60, 1, 0},
    {16, 4200,  4, 260, 55, 90, 50, 0, 0},
    {16, 12000, 1, 600, 65, 93, 60, 1, 0},
    {32, 66000, 1, 650, 40, 95, 50, 0, 0},
    {32, 66000, 1, 650, 45, 60, 60, 1, 1},
    {32, 66000, 1, 650, 45, 60, 60, 0, 1},
};
#define N_CASES ((int)(sizeof(g_cases) / sizeof(g_cases[0])))

static double g_maxRatio;
static fat_u64 g_moved, g_used;

static void pin_some(hctx *h, unsigned *rng, int n)
{
    tvol *v = h->v;
    int tries;

    for (tries = 0; tries < 500 && n > 0; tries++) {
        int i = 1 + (int)(rnd(rng) % (unsigned)(v->nn - 1));

        if (!v->n[i].isDir && v->n[i].nCl && !is_system(v, i) &&
            !is_pinned(h, i)) {
            h_pin(h, i);
            n--;
        }
    }
}

/* Only system and pinned files can keep the free space split: the pieces of
 * a fragmented FAT32 root folder move, all but the first. */
static int has_fixed(const tgen *g)
{
    return g->sysFiles;
}

static void dump_layout(const hctx *h)
{
    const tvol *v = h->v;
    fat_u32 cl, last = v->nClusters + 1;
    int i;

    for (cl = 2; cl <= last; cl++) {
        if (tv_get(v, 0, cl) == 0) {
            fat_u32 e = cl;

            while (e <= last && tv_get(v, 0, e) == 0)
                e++;
            fprintf(stderr, "  FREE %u..%u (%u)\n", cl, e - 1, e - cl);
            cl = e - 1;
            continue;
        }
        for (i = 0; i < v->nn; i++) {
            const tnode *t = &v->n[i];
            if (!t->gone && t->start == cl && (t->isDir || t->size))
                fprintf(stderr, "  %4u: node %d %.11s %s n=%u frags=%u%s\n",
                        cl, i, (const char *)t->name, t->isDir ? "DIR" : "",
                        t->isDir ? 0 : file_clusters(v, t), t->frags,
                        movable_node(h, i) ? "" : " FIXED");
        }
    }
}

/* After a complete run: everything movable is in one piece, and without
 * unmovable objects the free space is one run at the end. */
static void check_done(hctx *h, const tgen *g, int rc, const fat_stats *st)
{
    CHECK(tv_fsck(h->v, NULL));
    check_gap_notes(h, st);
    CHECK(st->ioErrors == 0 && st->busy == 0 && !st->aborted);
    CHECK(st->chainErrors == 0 && st->crossLinked == 0);
    CHECK(rc == (st->noSpace ? 1 : 0));
    CHECK((fat_u64)frag_movable(h) == st->noSpace);
    if (!has_fixed(g) &&
        !(st->noSpace == 0 && st->freeRuns <= 1 && st->freeGaps == 0)) {
        fprintf(stderr, "type %d: noSpace %llu freeRuns %u gaps %u (%u "
                "clusters) free %u largest %u defragged %llu+%llu "
                "compacted %llu moved %llu\n", g->type,
                (unsigned long long)st->noSpace, st->freeRuns, st->freeGaps,
                st->gapClusters, st->freeClusters, st->largestFreeRun,
                (unsigned long long)st->defragged,
                (unsigned long long)st->dirsDefragged,
                (unsigned long long)st->compacted,
                (unsigned long long)st->clustersMoved);
        dump_layout(h);
        CHECK(0);
    }
}

/*
 * FAT32: the pieces of the root folder after its first cluster move into the
 * gaps like those of any folder, while the first cluster, which the boot
 * sector names, stays. kind 0: the root folder is in three pieces; 1: the
 * space right after its first cluster is free, and the rest joins it there;
 * 2: it is in one piece far up, which stays, and so does the gap below it.
 */
static void test_root_tail(int kind, int hooks)
{
    static const fat_u32 rp[] = {2, 40, 41, 300}, rj[] = {2, 600, 601};
    static const fat_u32 rh[] = {500, 501};
    static const fat_u32 pa[] = {3, 4, 5, 6, 7, 8}, ja[] = {10, 11, 12, 13,
                                                            14, 15};
    static const fat_u32 b[] = {20, 21, 22, 23};
    static const fat_u32 pc[] = {50, 51, 52, 53, 54, 55, 56, 57};
    static const fat_u32 jc[] = {30, 31, 32, 33, 700, 701, 702, 703};
    static const fat_u32 pd[] = {301, 302, 900, 901};
    static const tgen plainRoot = {32, 66000, 1, 0, 0, 0, 0, 0, 1};
    static const tgen highRoot = {32, 66000, 1, 0, 0, 0, 0, 1, 1};
    unsigned rng = 61u + (unsigned)kind * 2u + (unsigned)hooks;
    tvol *v = tv_new(32, 66000, 1);
    tnode *r = &v->n[0];
    int join = kind == 1, high = kind == 2;
    const fat_u32 *rc = join ? rj : high ? rh : rp;
    fat_u32 nr = join ? 3 : high ? 2 : 4, first = rc[0], k;
    fat_volume fv;
    hctx h;
    fat_stats an, st, re;
    int na, nc, nd = 0;

    r->nCl = nr;
    r->cls = (fat_u32 *)malloc((nr + 1) * sizeof(fat_u32));
    CHECK(r->cls != NULL);
    for (k = 0; k < nr; k++) {
        r->cls[k] = rc[k];
        v->used[rc[k]] = 1;
    }
    na = tv_node(v, 0, 0, "A       DAT", "First file.dat");
    place(v, na, join ? ja : pa, 6, &rng);
    place(v, tv_node(v, 0, 0, "B       DAT", NULL), b, 4, &rng);
    nc = tv_node(v, 0, 0, "C       DAT", "Third file.dat");
    place(v, nc, join ? jc : pc, 8, &rng);
    if (!join) {
        nd = tv_node(v, 0, 0, "D       DAT", NULL);
        place(v, nd, pd, 4, &rng);
    }
    tv_commit(v);
    tv_mount(v, &fv);
    h_init(&h, v, &fv, 6);
    CHECK(tv_fsck(v, NULL));
    CHECK(v->n[0].start == first);
    CHECK(v->n[0].frags == (join ? 2u : high ? 1u : 3u));

    CHECK(run(&h, 0, 0, &an) == 0);
    check_measure(&h, &an);
    CHECK(an.dirsFragmented == (high ? 0u : 1u));
    CHECK(an.planFreeRuns == (high ? 2u : 1u));
    CHECK(an.planDefrag == (join ? 2u : 1u));
    CHECK(an.unmovableFragmented == (kind == 0 ? 1u : 0u));
    CHECK(an.compactable > 0 && h.rootMoves == 0); /* plans report no moves */

    CHECK(run(&h, 1, hooks, &st) == 0);
    check_done(&h, high ? &highRoot : &plainRoot, 0, &st);
    CHECK(v->n[0].start == first);
    CHECK(st.defragged + st.dirsDefragged == an.planDefrag);
    CHECK(st.unmovableFragmented == an.unmovableFragmented);
    CHECK(st.compacted > 0 && (h.rootMoves > 0) == !high);
    if (high) {
        /* Everything else is packed below it. */
        CHECK(v->n[0].frags == 1 && st.freeRuns == 2 && st.gapsLeft == 1);
        CHECK(st.gapNote[0].why == FAT_GAP_ROOT);
        CHECK(st.gapNote[0].clusters == first - 2 - 22);
        CHECK(strcmp(st.gapNote[0].path, "\\") == 0);
    } else {
        CHECK(st.freeRuns == 1 && st.freeGaps == 0 && st.gapsLeft == 0);
    }
    if (join) {
        CHECK(v->n[0].frags == 1 && st.dirsDefragged == 1);
        CHECK(h.ev[FAT_EV_DIRDEFRAGGED] == 1);
        CHECK(v->n[nc].frags == 1 && st.defragged == 1);
    } else if (!high) {
        /* A stays right after it: the root folder stays in pieces. */
        CHECK(v->n[0].frags > 1 && st.dirsDefragged == 0);
        CHECK(v->n[na].start == 3 && v->n[nd].frags == 1);
    }
    same_notes(&an, &st);

    CHECK(run(&h, 0, 0, &re) == 0);
    check_measure(&h, &re);
    CHECK(re.planDefrag == 0 && re.compactable == 0 && h.rootMoves == 0);
    CHECK(re.unmovableFragmented == st.unmovableFragmented);
    same_notes(&re, &st);
    fat_unmount(&fv);
    tv_free(v);
}

static void stress_plain(const tgen *g, unsigned seed, int hooks)
{
    unsigned rng = seed;
    tvol *v = tv_random(g, &rng);
    fat_volume fv;
    hctx h;
    fat_stats an, st, re;
    double ratio;
    int rc;

    tv_mount(v, &fv);
    h_init(&h, v, &fv, seed);
    if (g->sysFiles)
        pin_some(&h, &rng, 2);
    CHECK(tv_fsck(v, NULL));
    CHECK(run(&h, 0, 0, &an) == 0);
    check_measure(&h, &an);
    CHECK(an.planDefrag > 0);
    h.fsckPct = hooks ? 3 : 0;
    rc = run(&h, 1, hooks, &st);
    check_done(&h, g, rc, &st);
    CHECK(st.defragged + st.dirsDefragged == an.planDefrag);
    CHECK(st.compacted == an.compactable);
    CHECK(st.freeRuns == an.planFreeRuns);
    CHECK(h.ev[FAT_EV_DEFRAGGED] == st.defragged);
    CHECK(h.ev[FAT_EV_DIRDEFRAGGED] == st.dirsDefragged);
    CHECK(h.ev[FAT_EV_MOVED] >= st.compacted);
    CHECK(g->type != 32 || fsinfo_free(v) == 0xFFFFFFFFu);
    CHECK(run(&h, 0, 0, &re) == 0);
    check_measure(&h, &re);
    CHECK(re.planDefrag == 0 && re.compactable == 0);
    CHECK(re.planFreeRuns == st.freeRuns);
    CHECK(an.unmovableFragmented == st.unmovableFragmented &&
          re.unmovableFragmented == st.unmovableFragmented);
    same_notes(&an, &st);
    same_notes(&re, &st);
    g_used += v->nClusters - st.freeClusters;
    g_moved += st.clustersMoved;
    ratio = (double)st.clustersMoved / (double)(v->nClusters - st.freeClusters);
    if (ratio > g_maxRatio)
        g_maxRatio = ratio;
    fat_unmount(&fv);
    tv_free(v);
}

/* Other programs write and the lock is often taken; a quiet run afterwards
 * completes the job. */
static void stress_mutate(const tgen *g, unsigned seed)
{
    unsigned rng = seed;
    tvol *v = tv_random(g, &rng);
    fat_volume fv;
    hctx h, h2;
    fat_stats st;
    int rc, k;

    tv_mount(v, &fv);
    h_init(&h, v, &fv, seed);
    if (g->sysFiles)
        pin_some(&h, &rng, 2);
    h.mutPct = 25;
    h.busyPct = 10;
    h.fsckPct = 3;
    rc = run(&h, 1, 1, &st);
    CHECK(rc == 0 || rc == 1);
    CHECK(h.mutations > 0);
    CHECK(tv_fsck(v, NULL));
    CHECK(st.ioErrors == 0 && st.chainErrors == 0 && st.crossLinked == 0);
    h_init(&h2, v, &fv, seed + 1);
    for (k = 0; k < h.nPins; k++)
        h_pin(&h2, h.pinNode[k]);
    rc = run(&h2, 1, 1, &st);
    check_done(&h2, g, rc, &st);
    fat_unmount(&fv);
    tv_free(v);
}

/* Writes that fail (once or for good) at a given point, or random read
 * errors: no data may be lost. ScanDisk-level damage (lost clusters, an
 * outdated "..") is allowed. After a remount and simulated ScanDisk repair
 * of inconsistent metadata, a clean run completes. */
static void fault_case(const tgen *g, unsigned seed, unsigned long failAt,
                       int all, unsigned readPct, int hooks, int drop)
{
    static const fsck_opt looseRead = {1, 1, 0, 0}, looseCut = {1, 1, 0, 1};
    const fsck_opt *loose = failAt ? &looseCut : &looseRead;
    unsigned rng = seed;
    tvol *v = tv_random(g, &rng);
    fat_volume fv;
    hctx h, h2;
    fat_stats st;
    int rc;

    tv_mount(v, &fv);
    h_init(&h, v, &fv, seed);
    v->d.failWriteAt = failAt;
    v->d.failWriteAll = all;
    v->d.dropWrite = drop;
    v->d.readFailPct = readPct;
    v->d.rng = seed;
    rc = run(&h, 1, hooks, &st);
    if (failAt && v->d.writes >= failAt && (!drop || v->d.lostWrite))
        CHECK(rc == 1 && st.ioErrors > 0);
    v->d.failWriteAt = 0;
    v->d.failWriteAll = 0;
    v->d.readFailPct = 0;
    CHECK(tv_fsck(v, loose));
    fat_unmount(&fv);
    tv_mount(v, &fv);
    h_init(&h2, v, &fv, seed + 1);
    {
        int repair = !fat_mirrors_match(&fv), i;

        for (i = 1; i < v->nn; i++)
            if (v->n[i].isDir &&
                ent_start(v, tv_cluster(v, v->n[i].start) + 32) !=
                (v->n[i].parent ? v->n[v->n[i].parent].start : 0))
                repair = 1;
        if (repair) {
            unsigned long writes = v->d.writes;

            CHECK(run(&h2, 1, hooks, &st) == 1);
            CHECK((st.ioErrors || st.chainErrors) && v->d.writes == writes);
            /* Simulate ScanDisk repairing the FAT mirrors and stale "..". */
            memcpy(v->d.bytes + (size_t)(fv.reservedSectors + fv.fatSize) *
                                fv.bytesPerSector,
                   v->d.bytes + (size_t)fv.reservedSectors * fv.bytesPerSector,
                   fv.fatBytes);
            for (i = 1; i < v->nn; i++)
                if (v->n[i].isDir)
                    set_ent_start(v, tv_cluster(v, v->n[i].start) + 32,
                                  v->n[i].parent ?
                                      v->n[v->n[i].parent].start : 0);
            CHECK(fat_reload(&fv));
            h_init(&h2, v, &fv, seed + 1);
        }
    }
    rc = run(&h2, 1, hooks, &st);
    CHECK(tv_fsck(v, loose));
    CHECK(st.ioErrors == 0 && st.busy == 0 && !st.aborted);
    CHECK(rc == (st.noSpace ? 1 : 0) && (fat_u64)frag_movable(&h2) == st.noSpace);
    fat_unmount(&fv);
    tv_free(v);
}

static void test_move_cuts(int type)
{
    static const fsck_opt loose = {1, 1, 0, 1}, clean = {1, 1, 0, 0};
    unsigned long cut = 0, writes = 0;
    int mode;

    for (mode = 0; mode < 4; mode++) {
        for (cut = mode ? 1 : 0; cut <= writes; cut++) {
            unsigned rng = 84;
            dm_nodes x;
            tvol *v = build_dirmove(type, &x, &rng);
            fat_volume fv;
            fat_defrag_cfg cfg;
            fat_stats st;
            unsigned char boot[512];
            int rc;

            memcpy(boot, v->d.bytes, sizeof(boot));
            CHECK(fat_mount(&fv, &v->dev));
            memset(&cfg, 0, sizeof(cfg));
            cfg.doMove = cfg.verify = cfg.compact = 1;
            v->d.failWriteAt = cut;
            v->d.failWriteAll = mode & 1;
            v->d.dropWrite = (mode & 2) != 0;
            rc = fat_run(&fv, &cfg, &st);
            if (cut && (!(mode & 2) || v->d.lostWrite)) {
                if (rc != 1 || !st.ioErrors)
                    fprintf(stderr, "write cut: FAT%d mode %d at %lu/%lu, "
                            "writes %lu rc %d errors %llu\n", type, mode,
                            cut, writes, v->d.writes, rc,
                            (unsigned long long)st.ioErrors);
                CHECK(rc == 1 && st.ioErrors > 0);
            } else if (!cut) {
                CHECK(rc == 0 && st.clustersMoved > 0);
                writes = v->d.writes;
                CHECK(writes > 20);
            } else {
                CHECK(rc == 0 && st.ioErrors == 0);
            }
            v->d.failWriteAt = 0;
            CHECK(tv_fsck(v, cut ? &loose : &clean));
            CHECK(memcmp(boot, v->d.bytes, sizeof(boot)) == 0);
            fat_unmount(&fv);
            tv_free(v);
        }
    }
}

static unsigned long count_writes(const tgen *g, unsigned seed, int hooks)
{
    unsigned rng = seed;
    tvol *v = tv_random(g, &rng);
    fat_volume fv;
    hctx h;
    fat_stats st;
    unsigned long w;

    tv_mount(v, &fv);
    h_init(&h, v, &fv, seed);
    run(&h, 1, hooks, &st);
    CHECK(!g->fragRoot || h.rootMoves > 0);
    w = v->d.writes;
    fat_unmount(&fv);
    tv_free(v);
    return w;
}

static void test_stress(void)
{
    int c, s;

    for (c = 0; c < N_CASES; c++) {
        int seeds = g_cases[c].type == 32 ? 2 : 4;

        for (s = 0; s < seeds; s++) {
            unsigned seed = 1000u * (unsigned)c + (unsigned)s + 1;

            stress_plain(&g_cases[c], seed, s & 1);
            stress_mutate(&g_cases[c], seed + 500);
        }
        if (g_cases[c].sysFiles) { /* MS-DOS mode: the swap file moves too */
            g_offline = 1;
            stress_plain(&g_cases[c], 1000u * (unsigned)c + 900, c & 1);
            g_offline = 0;
        }
    }
    printf("fat: compaction moved %.2fx the used clusters (worst %.2fx)\n",
           g_used ? (double)g_moved / (double)g_used : 0.0, g_maxRatio);
}

/* The FAT12/16 cases, and FAT32 with the root folder in pieces. */
static void test_faults(void)
{
    static const int cases[] = {0, 1, 2, 3, 6};
    int i, k;

    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        int c = cases[i];
        const tgen *g = &g_cases[c];
        unsigned seed = 7000u + (unsigned)c;
        int hooks = c & 1;
        unsigned long w = count_writes(g, seed, hooks);

        CHECK(w > 20);
        for (k = 0; k < 16; k++)
            fault_case(g, seed, 1 + (w - 1) * (unsigned long)k / 15, k & 1,
                       0, hooks, 0);
        for (k = 0; k < 16; k++)
            fault_case(g, seed, 1 + (w - 1) * (unsigned long)k / 15, k & 1,
                       0, hooks, 1);
        for (k = 0; k < 4; k++)
            fault_case(g, seed, 0, 0, 3, hooks, 0);
    }
}

int main(void)
{
    int k, attr;

#ifdef __SANITIZE_ADDRESS__
    __sanitizer_install_malloc_and_free_hooks(on_malloc, on_free);
#endif
    test_fat16();
    test_fat32();
    test_fat32_active_fat();
    test_bad_geometry(12);
    test_bad_geometry(16);
    test_bad_geometry(32);
    test_scan_refusals(12);
    test_scan_refusals(16);
    test_scan_refusals(32);
    test_bad_lfn(12);
    test_bad_lfn(16);
    test_bad_lfn(32);
    test_deep_tree();
    test_dir_move(12);
    test_dir_move(16);
    test_dir_move(32);
    test_unmovable(12, 0);
    test_unmovable(16, 0);
    test_unmovable(32, 0);
    test_unmovable(12, 1);
    test_unmovable(16, 1);
    test_unmovable(32, 1);
    for (k = 0; k < 3; k++) {
        for (attr = 0; attr < 8; attr++) {
            test_root_files(12, k & 1, k, attr);
            test_root_files(16, !(k & 1), k, attr);
            test_root_files(32, k & 1, k, attr);
            test_root_files(32, !(k & 1), k, attr);
        }
    }
    test_gap_notes(12, 0);
    test_gap_notes(16, 1);
    test_gap_notes(32, 0);
    test_gap_notes(32, 1);
    for (k = 0; k < 6; k++)
        test_root_tail(k >> 1, k & 1);
    test_piece_compact(12);
    test_piece_compact(16);
    test_piece_compact(32);
    test_piece_dir();
    test_piece_dir_faults();
    test_fsinfo_first_switch();
    test_slide(4, 200);
    test_slide(1, 64);
    test_slide(1, 65);
    test_slide(3, 1000);
    test_big_file();
    test_hook_faults();
    test_stress();
    test_move_cuts(12);
    test_move_cuts(16);
    test_move_cuts(32);
    test_faults();
    printf("fat: %u checks passed\n", checks);
    return 0;
}

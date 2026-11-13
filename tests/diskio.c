/*
 * diskio.c - Regressions for the request format of the MS-DOS absolute
 * disk calls.
 *
 * The MS-DOS side of hsdefrag.exe reads and writes sectors with INT 21h
 * AX=7305h and INT 25h/26h. Nothing checks those calls at run time except
 * the boot sector that the drive probe reads, so the layout of the request
 * is pinned down here: a mistake in it would read the wrong sectors and
 * write them back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fatdisk.h"

static int checks;

static void ok(int cond, const char *what)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        exit(1);
    }
    checks++;
}

static unsigned le16(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static unsigned long le32(const unsigned char *p)
{
    return (unsigned long)p[0] | (unsigned long)p[1] << 8 |
           (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

/* An SFT entry for NAME.EXT on the 0-based drive with the given mode. */
static void sft(unsigned char *e, unsigned handles, unsigned mode,
                unsigned dev, unsigned long dirSec, unsigned idx)
{
    memset(e, 0, FATDISK_SFT_SIZE);
    e[0] = (unsigned char)handles;
    e[1] = (unsigned char)(handles >> 8);
    e[2] = (unsigned char)mode;
    e[3] = (unsigned char)(mode >> 8);
    e[5] = (unsigned char)dev;
    e[6] = (unsigned char)(dev >> 8);
    e[0x1B] = (unsigned char)dirSec;
    e[0x1C] = (unsigned char)(dirSec >> 8);
    e[0x1D] = (unsigned char)(dirSec >> 16);
    e[0x1E] = (unsigned char)(dirSec >> 24);
    e[0x1F] = (unsigned char)idx;
    memcpy(e + 0x20, "NAME    EXT", 11);
}

static void cds(unsigned char *e, const char *path, unsigned flags)
{
    memset(e, 0, FATDISK_CDS_SIZE);
    memcpy(e, path, strlen(path));
    e[0x43] = (unsigned char)flags;
    e[0x44] = (unsigned char)(flags >> 8);
}

static void tables(void)
{
    unsigned char e[FATDISK_SFT_SIZE], d[32], c[FATDISK_CDS_SIZE];
    fat_sft_info in;
    fat_u32 cl = 0;

    /* SFT: free, device, remote and other-drive entries are ignored. */
    sft(e, 0, 0, 2, 100, 3);
    ok(fatdisk_sft_entry(e, 2, 0, &in) == FATDISK_SFT_IGNORE, "free SFT");
    sft(e, 0xFFFF, 0, 2, 100, 3);
    ok(fatdisk_sft_entry(e, 2, 0, &in) == FATDISK_SFT_IGNORE, "SFT FFFF");
    sft(e, 1, 0, 0x80 | 2, 100, 3);
    ok(fatdisk_sft_entry(e, 2, 0, &in) == FATDISK_SFT_IGNORE, "device SFT");
    sft(e, 1, 0, 0x8000 | 2, 100, 3);
    ok(fatdisk_sft_entry(e, 2, 0, &in) == FATDISK_SFT_IGNORE, "remote SFT");
    sft(e, 1, 0, 3, 100, 3);
    ok(fatdisk_sft_entry(e, 2, 0, &in) == FATDISK_SFT_IGNORE, "other drive");

    /* Read (0) and read-no-date (4) are looked up; others refuse. */
    sft(e, 1, 0x40, 2, 0x01020304UL, 3);
    ok(fatdisk_sft_entry(e, 2, 0, &in) == FATDISK_SFT_LOOKUP &&
       in.dirSector == 0x01020304UL && in.dirIndex == 3 && !in.fcb &&
       strcmp(in.name, "NAME.EXT") == 0, "SFT read lookup");
    sft(e, 1, 4, 2, 100, 3);
    ok(fatdisk_sft_entry(e, 2, 0, &in) == FATDISK_SFT_LOOKUP, "SFT mode 4");
    /* An entry without a name (as emulators fake them) still gets a name
     * for the message, and is still refused when open for writing. */
    sft(e, 1, 1, 0, 0, 0);
    memset(e + 0x20, ' ', 11);
    ok(fatdisk_sft_entry(e, 0, 0, &in) == FATDISK_SFT_WRITE &&
       strcmp(in.name, "(no name)") == 0, "SFT without a name");
    memset(e + 0x20, 0, 11);
    ok(fatdisk_sft_entry(e, 0, 0, &in) == FATDISK_SFT_WRITE &&
       strcmp(in.name, "(no name)") == 0, "SFT with a zeroed name");
    sft(e, 1, 1, 2, 100, 3);
    ok(fatdisk_sft_entry(e, 2, 0, &in) == FATDISK_SFT_WRITE, "SFT write");
    sft(e, 1, 2, 2, 100, 3);
    ok(fatdisk_sft_entry(e, 2, 0, &in) == FATDISK_SFT_WRITE, "SFT r/w");
    /* FCBs (by table or by mode bit 15) are only looked up. */
    sft(e, 1, 2, 2, 100, 3);
    ok(fatdisk_sft_entry(e, 2, 1, &in) == FATDISK_SFT_LOOKUP && in.fcb,
       "FCB table");
    sft(e, 1, 0x8002, 2, 100, 3);
    ok(fatdisk_sft_entry(e, 2, 0, &in) == FATDISK_SFT_LOOKUP && in.fcb,
       "FCB mode bit");

    /* Directory match, 05h for E5h, the FAT32 high word, and refusals. */
    sft(e, 1, 0, 2, 100, 3);
    memset(d, 0, sizeof(d));
    memcpy(d, "NAME    EXT", 11);
    d[0x1A] = 0x34; d[0x1B] = 0x12; d[0x14] = 0x02; d[0x15] = 0x00;
    ok(fatdisk_sft_dir_match(e, d, 0, &cl) && cl == 0x1234, "dir FAT16");
    ok(fatdisk_sft_dir_match(e, d, 1, &cl) && cl == 0x21234UL, "dir FAT32");
    d[0] = 'X';
    ok(!fatdisk_sft_dir_match(e, d, 0, &cl), "dir other name");
    d[0] = 'N'; d[11] = 0x10;
    ok(!fatdisk_sft_dir_match(e, d, 0, &cl), "dir folder");
    d[11] = 0x08;
    ok(!fatdisk_sft_dir_match(e, d, 0, &cl), "dir label");
    d[11] = 0; d[0] = 0xE5;
    ok(!fatdisk_sft_dir_match(e, d, 0, &cl), "dir deleted");
    d[0] = 0x05; e[0x20] = 0x05;
    ok(fatdisk_sft_dir_match(e, d, 0, &cl), "dir 05h");

    /* CDS: unused, network and plain local drives are fine. */
    cds(c, "C:\\", 0);
    ok(fatdisk_cds_entry(c, 2, 'C', 2) == FATDISK_CDS_OK, "CDS unused");
    cds(c, "\\\\SRV\\X", 0xC000);
    ok(fatdisk_cds_entry(c, 2, 'C', 2) == FATDISK_CDS_OK, "CDS network");
    cds(c, "C:\\DOS", 0x4000);
    ok(fatdisk_cds_entry(c, 2, 'C', 2) == FATDISK_CDS_OK, "CDS local");
    /* A local entry with another letter, or a strange path, is unknown. */
    cds(c, "D:\\", 0x4000);
    ok(fatdisk_cds_entry(c, 2, 'C', 2) == FATDISK_CDS_UNKNOWN, "CDS letter");
    cds(c, "C-\\", 0x4000);
    ok(fatdisk_cds_entry(c, 2, 'C', 2) == FATDISK_CDS_UNKNOWN, "CDS path");
    /* SUBST into the volume, from either side, and JOIN. */
    cds(c, "c:\\WORK", 0x5000);
    ok(fatdisk_cds_entry(c, 5, 'C', 2) == FATDISK_CDS_SUBST, "SUBST into");
    cds(c, "E:\\X", 0x5000);
    ok(fatdisk_cds_entry(c, 2, 'C', 2) == FATDISK_CDS_SUBST, "SUBST over");
    cds(c, "C:\\MNT", 0x6000);
    ok(fatdisk_cds_entry(c, 5, 'C', 2) == FATDISK_CDS_JOIN, "JOIN");
    cds(c, "E:\\X", 0x5000);
    ok(fatdisk_cds_entry(c, 5, 'C', 2) == FATDISK_CDS_OK, "SUBST elsewhere");
}

int main(void)
{
    fat_disk_req r;

    /* INT 21h AX=7305h: the 1-based drive in DL, the mode flags in SI and
     * the packet at DS:BX. */
    fat_disk_request(FATDISK_7305, 3, 0, 1, 0x1234, 0, &r);
    ok(r.use_packet, "the 7305h call uses a packet");
    ok(r.ax == 0x7305, "the 7305h call has AX=7305h");
    ok(r.cx == 0xFFFF, "the 7305h call has CX=FFFFh");
    ok(r.dx == 3, "the 7305h call has the 1-based drive in DL");
    ok(r.si == 0, "the 7305h call reads with SI=0");
    ok(r.bx == 0, "the packet is at the start of the segment");
    ok(r.packet[0] == 0 && r.packet[1] == 0 && r.packet[2] == 0 &&
       r.packet[3] == 0, "sector 0 is in the packet");
    ok(r.packet[4] == 1 && r.packet[5] == 0, "the sector count is in the "
       "packet");
    ok(r.packet[6] == 0 && r.packet[7] == 0, "the transfer offset is in the "
       "low word of the address");
    ok(r.packet[8] == 0x34 && r.packet[9] == 0x12, "the transfer segment is "
       "in the high word of the address");

    /* The sector number is 32 bits, so that a volume of more than 64K
     * sectors can be reached. */
    fat_disk_request(FATDISK_7305, 1, 0x12345678UL, 64, 0xABCD, 0, &r);
    ok(le32(r.packet) == 0x12345678UL, "the sector number is a DWORD");
    ok(le16(r.packet + 4) == 64, "the sector count is a WORD");
    ok(le16(r.packet + 6) == 0, "the transfer offset is the low word");
    ok(le16(r.packet + 8) == 0xABCD, "the transfer segment is the high "
       "word");

    /* The offset of the window is in the low word, the segment in the high
     * one: a far pointer the other way round reads foreign memory. */
    fat_disk_request(FATDISK_7305, 1, 0, 1, 0xABCD, 0x1234, &r);
    ok(le16(r.packet + 6) == 0x1234, "the offset of the window is in the "
       "low word");
    ok(le16(r.packet + 8) == 0xABCD, "the segment of the window is in the "
       "high word");

    /* INT 25h/26h in the form of MS-DOS 3.31 and later: the 0-based drive
     * in AL, CX=FFFFh and the same packet at DS:BX. */
    fat_disk_request(FATDISK_INT25_NEW, 3, 100, 8, 0x1000, 0, &r);
    ok(r.use_packet, "the new INT 25h call uses a packet");
    ok(r.ax == 2, "the new INT 25h call has the 0-based drive in AL");
    ok(r.cx == 0xFFFF, "the new INT 25h call has CX=FFFFh");
    ok(r.dx == 0, "the new INT 25h call does not use DX");
    ok(le32(r.packet) == 100, "the new INT 25h call has the sector number");
    ok(le16(r.packet + 4) == 8, "the new INT 25h call has the sector "
       "count");
    ok(le16(r.packet + 8) == 0x1000, "the new INT 25h call has the transfer "
       "address");

    /* The old form of INT 25h/26h: the 0-based drive in AL, the number of
     * sectors in CX, the first sector in DX and the window at DS:BX. */
    fat_disk_request(FATDISK_INT25_OLD, 2, 500, 4, 0x2000, 0, &r);
    ok(!r.use_packet, "the old INT 25h call uses no packet");
    ok(r.ax == 1, "the old INT 25h call has the 0-based drive in AL");
    ok(r.cx == 4, "the old INT 25h call has the sector count in CX");
    ok(r.dx == 500, "the old INT 25h call has the first sector in DX");
    ok(r.ds == 0x2000, "the old INT 25h call has the window in DS");
    ok(r.bx == 0, "the old INT 25h call has the window at offset 0");

    /* The Windows installation checks.  A plain DOS system must not be
     * taken for a running Windows: DOS 5 and later answer INT 2Fh
     * AX=4680h the same way when their own shell is on the screen. */
    ok(!fatdisk_windows_running(0x160A, 0x00, 0x0000, 0),
       "a plain DOS system is not a running Windows");
    ok(!fatdisk_windows_running(0x160A, 0x80, 0x0000, 0),
       "an XMS driver is not a running Windows");
    ok(fatdisk_windows_running(0x0000, 0x00, 0x0001, 0),
       "Windows 3.1 and later answer AX=160Ah with zero");
    ok(fatdisk_windows_running(0x160A, 0x04, 0x0001, 0),
       "Windows 95 reports its version in AX=1600h");
    ok(fatdisk_windows_running(0x160A, 0x01, 0x0001, 0),
       "Windows/386 2.x answers AX=1600h with 01h");
    ok(fatdisk_windows_running(0x160A, 0xFF, 0x0001, 0),
       "Windows/386 2.x answers AX=1600h with FFh");
    ok(fatdisk_windows_running(0x160A, 0x00, 0x0000, 1),
       "Windows 3.0 in real mode answers AX=4680h with zero");
    ok(!fatdisk_windows_running(0x160A, 0x00, 0x0001, 1),
       "DOS 4 without Windows is not a running Windows");

    tables();
    printf("diskio: %d checks passed\n", checks);
    return 0;
}

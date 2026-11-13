/*
 * fatdisk.h - The MS-DOS interfaces the MS-DOS side relies on.
 *
 * The MS-DOS side of hsdefrag.exe reads and writes sectors with INT 21h
 * AX=7305h and INT 25h/26h. What those calls expect is not obvious, and
 * getting it wrong corrupts a volume, so the layout of the request lives
 * here, apart from the DPMI code that hands it over. The rule that tells
 * whether Windows is running lives here as well, for the same reason.
 */
#ifndef FATDISK_H
#define FATDISK_H

#include "fatfs.h"

/* The ways a sector transfer can be asked for. 0 means "no method". */
enum {
    FATDISK_7305 = 1,    /* INT 21h AX=7305h: DOS 7.0A and later, the only
                          * one that knows FAT32                       */
    FATDISK_INT25_NEW,   /* INT 25h/26h with CX=FFFFh: MS-DOS 3.31 and
                          * later, so MS-DOS 6.2 has it                */
    FATDISK_INT25_OLD    /* INT 25h/26h in the old form: every DOS, but
                          * only the first 64K sectors of a drive     */
};

/* The disk I/O packet of the extended calls: the sector number (DWORD),
 * the number of sectors (WORD) and the transfer address (DWORD), which is
 * a far pointer with the offset in the low word and the segment in the
 * high word. */
#define FATDISK_PACKET_SIZE 10

/* One transfer request, in the form the DOS call expects it. */
typedef struct {
    int      method;                    /* one of the FATDISK_* above   */
    unsigned ax, bx, cx, dx, si;        /* the registers of the call    */
    unsigned ds, es;                    /* the segments of the call     */
    int      use_packet;                /* 1 if the packet is used      */
    unsigned char packet[FATDISK_PACKET_SIZE];
} fat_disk_req;

/*
 * Fill in the request for one transfer of count sectors, starting at
 * sector, into or out of the window at win_seg:win_off. drive is the
 * 1-based DOS drive number.
 */
void fat_disk_request(int method, int drive, fat_u32 sector, unsigned count,
                      unsigned win_seg, unsigned win_off, fat_disk_req *req);

/*
 * Whether the answers of the Windows installation checks mean that Windows
 * is running.  ax_160a is the answer to INT 2Fh AX=160Ah (zero when the
 * call is supported, which is the case in every mode of Windows 3.1 and
 * later, Windows 95 and 98 included), al_1600 the answer to INT 2Fh
 * AX=1600h and ax_4680 the answer to INT 2Fh AX=4680h.  That last answer
 * is only asked for when use_4680 is set: DOS 5 and later answer it the
 * same way when their own shell is on the screen, so it says nothing
 * there, and only DOS 4 and older, which have no shell of their own, can
 * read it as "Windows 3.0 in real or standard mode".
 */
int fatdisk_windows_running(int ax_160a, int al_1600, int ax_4680,
                             int use_4680);

/*
 * DOS's own tables, decoded from the bytes the MS-DOS side copies out of
 * conventional memory. They decide whether a run may go ahead (a file open
 * for writing, a SUBST or JOIN into the volume) and which files must stay
 * in place, so their layout is pinned down here and tested on the host.
 */

/* One system file table entry (3Bh bytes, DOS 4 and later). */
#define FATDISK_SFT_SIZE 0x3B

enum {
    FATDISK_SFT_IGNORE,  /* free, a device, or another drive */
    FATDISK_SFT_WRITE,   /* open for writing by handle: refuse the run */
    FATDISK_SFT_LOOKUP   /* open for reading (or an FCB): find and pin it */
};

typedef struct {
    int      fcb;        /* opened through an FCB (missing is tolerated) */
    fat_u32  dirSector;  /* sector holding its directory entry */
    unsigned dirIndex;   /* entry within that sector (< 16 to be usable) */
    char     name[13];   /* "NAME.EXT" for messages */
} fat_sft_info;

/* drive0 is the 0-based drive; fcbTable is set for the FCB table. */
int fatdisk_sft_entry(const unsigned char *e, int drive0, int fcbTable,
                      fat_sft_info *info);

/* Does the 32-byte directory entry d belong to SFT entry e? If so, put its
 * first cluster (with the FAT32 high word when fat32) in *cluster. */
int fatdisk_sft_dir_match(const unsigned char *e, const unsigned char *d,
                          int fat32, fat_u32 *cluster);

/* One current directory structure entry (58h bytes, DOS 4 and later). */
#define FATDISK_CDS_SIZE 0x58

enum {
    FATDISK_CDS_OK,      /* unused, network, or a plain local drive */
    FATDISK_CDS_UNKNOWN, /* not the layout we know: stop checking */
    FATDISK_CDS_SUBST,   /* a SUBST that leads into the volume */
    FATDISK_CDS_JOIN     /* a JOIN that leads into the volume */
};

/* index is the entry's 0-based drive, letter the volume's ('A'..'Z') and
 * drive0 its 0-based drive. */
int fatdisk_cds_entry(const unsigned char *e, unsigned index, int letter,
                      int drive0);

#endif

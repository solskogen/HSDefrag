/*
 * fatdisk.c - The request format of the MS-DOS absolute disk calls.
 *
 * INT 21h AX=7305h (the extended absolute disk read/write of MS-DOS 7.0A
 * and later, so Windows 95 and 98 have it) takes the 1-based drive number
 * in DL, the mode flags in SI and the disk I/O packet at DS:BX. Bit 0 of
 * the mode flags is the direction: 0 reads, 1 writes.
 *
 * INT 25h/26h in the form of MS-DOS 3.31 and later (so MS-DOS 6.2 has it)
 * takes the 0-based drive number in AL and the same packet at DS:BX, with
 * CX=FFFFh telling DOS that the packet is there. INT 25h reads and INT
 * 26h writes.
 *
 * INT 25h/26h in the old form takes the 0-based drive number in AL, the
 * number of sectors in CX, the first sector in DX and the transfer window
 * at DS:BX. DOS refuses that form with AX=0207h on a drive of more than
 * 64K sectors.
 */
#include <string.h>

#include "fatdisk.h"

int fatdisk_windows_running(int ax_160a, int al_1600, int ax_4680,
                             int use_4680)
{
    /* INT 2Fh AX=160Ah: the answer is zero when the call is supported. */
    if (ax_160a == 0)
        return 1;
    /* INT 2Fh AX=1600h: 01h and FFh mean Windows/386 2.x, any value from
     * 3 up is the major version of Windows 3.x or later, and 80h is only
     * an XMS driver. */
    if (al_1600 == 0x01 || al_1600 == 0xFF ||
        (al_1600 >= 0x03 && al_1600 != 0x80))
        return 1;
    /* INT 2Fh AX=4680h: zero means Windows 3.0 in real or standard mode,
     * or the shell of DOS 5 and later on the screen. */
    if (use_4680 && ax_4680 == 0)
        return 1;
    return 0;
}

void fat_disk_request(int method, int drive, fat_u32 sector, unsigned count,
                      unsigned win_seg, unsigned win_off, fat_disk_req *req)
{
    memset(req, 0, sizeof(*req));
    req->method = method;

    if (method == FATDISK_INT25_OLD) {
        req->ax = (unsigned)(drive - 1);  /* AL is the 0-based drive */
        req->cx = count;
        req->dx = (unsigned)sector;
        req->ds = win_seg;                /* the window is the buffer */
        req->bx = 0;
        return;
    }

    req->use_packet = 1;
    req->packet[0] = (unsigned char)sector;
    req->packet[1] = (unsigned char)(sector >> 8);
    req->packet[2] = (unsigned char)(sector >> 16);
    req->packet[3] = (unsigned char)(sector >> 24);
    req->packet[4] = (unsigned char)count;
    req->packet[5] = (unsigned char)(count >> 8);
    req->packet[6] = (unsigned char)win_off;  /* the offset of the window */
    req->packet[7] = (unsigned char)(win_off >> 8);
    req->packet[8] = (unsigned char)win_seg;   /* its segment */
    req->packet[9] = (unsigned char)(win_seg >> 8);

    req->bx = 0;                         /* the offset of the packet */
    req->cx = 0xFFFF;                    /* the form of the call */
    req->ds = 0;                         /* set by the caller */
    if (method == FATDISK_7305) {
        req->ax = 0x7305;
        req->dx = (unsigned)drive;       /* DL is the 1-based drive */
        req->si = 0;                     /* bit 0: 0 reads, 1 writes */
    } else {
        req->ax = (unsigned)(drive - 1); /* AL is the 0-based drive */
    }
}

/* ---------------------------------------------------- DOS's own tables */

static unsigned fd_le16(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

/* "NAME    EXT" -> "NAME.EXT" */
static void fd_name83(const unsigned char *n, char *out)
{
    int i, k = 0;

    for (i = 0; i < 8 && n[i] != ' '; i++)
        out[k++] = (char)n[i];
    if (n[8] != ' ') {
        out[k++] = '.';
        for (i = 8; i < 11 && n[i] != ' '; i++)
            out[k++] = (char)n[i];
    }
    out[k] = 0;
}

/*
 * SFT entry: 00h handle count, 02h open mode, 05h device information word
 * (bit 7 = a device, bit 15 = remote, bits 0-5 = the 0-based drive),
 * 1Bh the sector of the directory entry, 1Fh its index in that sector,
 * 20h the 8.3 name as stored in the directory.
 */
int fatdisk_sft_entry(const unsigned char *e, int drive0, int fcbTable,
                      fat_sft_info *info)
{
    unsigned handles = fd_le16(e), mode = fd_le16(e + 2), dev = fd_le16(e + 5);

    memset(info, 0, sizeof(*info));
    if (handles == 0 || handles == 0xFFFF || (dev & 0x8080) ||
        (dev & 0x3F) != (unsigned)drive0)
        return FATDISK_SFT_IGNORE;
    info->fcb = fcbTable || (mode & 0x8000);
    fd_name83(e + 0x20, info->name);
    if (!info->name[0] || info->name[0] == '.')  /* no name in the table */
        memcpy(info->name, "(no name)", 10);
    info->dirSector = e[0x1B] | (fat_u32)e[0x1C] << 8 |
                      (fat_u32)e[0x1D] << 16 | (fat_u32)e[0x1E] << 24;
    info->dirIndex = e[0x1F];
    /* Access 0 = read, 4 = read without updating the access date (DOS 7). */
    if (!info->fcb && (mode & 7) != 0 && (mode & 7) != 4)
        return FATDISK_SFT_WRITE;
    return FATDISK_SFT_LOOKUP;
}

int fatdisk_sft_dir_match(const unsigned char *e, const unsigned char *d,
                          int fat32, fat_u32 *cluster)
{
    const unsigned char *n = e + 0x20;
    int i;

    if (d[0] == 0 || d[0] == 0xE5 || (d[11] & 0x18))
        return 0;                   /* free, deleted, label or folder */
    for (i = 0; i < 11; i++) {
        unsigned a = d[i], b = n[i];

        if (i == 0 && a == 0x05)    /* 05h stands for a leading E5h */
            a = 0xE5;
        if (i == 0 && b == 0x05)
            b = 0xE5;
        if (a != b)
            return 0;
    }
    *cluster = fd_le16(d + 0x1A);
    if (fat32)
        *cluster |= (fat_u32)fd_le16(d + 0x14) << 16;
    return 1;
}

/*
 * CDS entry: 00h the current path ("X:\..."), 43h the flags word
 * (bit 15 network, bit 14 physical, bit 13 JOIN, bit 12 SUBST). A SUBST or
 * JOIN drive keeps the first cluster of a directory on the volume, which a
 * move would make stale.
 */
int fatdisk_cds_entry(const unsigned char *e, unsigned index, int letter,
                      int drive0)
{
    unsigned flags = e[0x43] | (unsigned)e[0x44] << 8;
    int c = e[0];

    if (!(flags & 0xC000) || (flags & 0x8000))
        return FATDISK_CDS_OK;      /* unused, or a network drive */
    if (e[1] != ':' || e[2] != '\\')
        return FATDISK_CDS_UNKNOWN;
    if (c >= 'a' && c <= 'z')
        c = c - 'a' + 'A';
    if (!(flags & 0x3000))
        return c == (int)('A' + index) ? FATDISK_CDS_OK : FATDISK_CDS_UNKNOWN;
    if (c == letter || (int)index == drive0)
        return (flags & 0x2000) ? FATDISK_CDS_JOIN : FATDISK_CDS_SUBST;
    return FATDISK_CDS_OK;
}

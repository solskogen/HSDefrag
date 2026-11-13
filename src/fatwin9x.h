/*
 * fatwin9x.h - Windows 9x FAT analyze/defrag command layer.
 *
 * These drive the portable FAT engine (fatfs.c / fatdefrag.c) over Windows 9x
 * raw absolute-sector I/O through the VWIN32 VxD. They are only meaningful on
 * Windows 95/98/Me; the NT path never calls them (VWIN32 does not exist there).
 *
 * Exit codes match the rest of the tool: 0 ok, 2 volume not accessible,
 * 3 unsupported, 4 incomplete, 5 runtime/output error.
 */
#ifndef HSDEFRAG_FATWIN9X_H
#define HSDEFRAG_FATWIN9X_H

/* Analyze a FAT12/16/32 volume (read-only, no volume lock). */
int cmd_fat_analyze(wchar_t drive);

/* Defragment a FAT12/16/32 volume (requires an exclusive volume lock). */
int cmd_fat_defrag(wchar_t drive);

/* Read the boot sector (512 bytes) of a drive. Returns 0, or an exit code
 * after printing why it failed. */
int fat9x_boot_sector(wchar_t drive, unsigned char *bs);

#endif /* HSDEFRAG_FATWIN9X_H */

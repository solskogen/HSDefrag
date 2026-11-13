/*
 * fatdefrag.h - FAT fragmentation analysis and whole-volume defragmenter.
 *
 * Operates purely on a mounted fat_volume (see fatfs.h), so it is exercised by
 * the host unit tests with an in-memory disk image. The Windows 9x command
 * layer supplies progress/verbose callbacks, the write-section hooks that map
 * onto the Windows 95 volume locks, and prints the resulting stats.
 *
 * A full run makes every movable file and directory contiguous and then
 * consolidates the free space (like the "Full defragmentation" of the Windows
 * 95 Disk Defragmenter). Consolidation sweeps up the volume and closes each
 * gap with the rest of the file just before it, else with the largest file
 * (or piece of a file that did not fit anywhere) from above that fits, else
 * by sliding the files that follow down; files that still did not fit are
 * then made contiguous, and this repeats until nothing more can be fixed.
 * Every file moves at most once per sweep, so the data moved stays close to
 * the size of what is out of place. Data is copied into free clusters and
 * verified, the new
 * clusters are linked in the FAT, the directory entry (or, for a later piece,
 * the FAT entry before it) is switched over, and only then are the old
 * clusters freed. Metadata writes are read back too. Interrupted moves may
 * need ScanDisk to repair lost clusters, FAT mirrors or directory links.
 * This ordering cannot protect against torn sectors or volatile drive caches.
 */
#ifndef HSDEFRAG_FATDEFRAG_H
#define HSDEFRAG_FATDEFRAG_H

#include "fatfs.h"

/* Per-object events reported through fat_defrag_cfg.event. Paths of
 * directories end in a backslash. */
enum {
    FAT_EV_CONTIGUOUS = 0,  /* analyze: file already in one piece           */
    FAT_EV_DEFRAGGED,       /* file moved into a single contiguous run      */
    FAT_EV_NOSPACE,         /* no contiguous free run large enough          */
    FAT_EV_IOERROR,         /* a read/write/flush failed while moving       */
    FAT_EV_CHAINERROR,      /* the cluster chain (or directory) was invalid */
    FAT_EV_FRAGMENTED,      /* analyze: file or directory is fragmented     */
    FAT_EV_DIRDEFRAGGED,    /* directory moved into one contiguous run      */
    FAT_EV_MOVED,           /* object, or a piece of one, moved to
                               consolidate free space (frags = clusters
                               moved, newFrags = fragments it now has)      */
    FAT_EV_UNMOVABLE,       /* defrag: kept in place, sent once for every
                               non-empty system or in-use file at the end
                               (frags = its fragments, newFrags =
                               FAT_GAP_SYSTEM or FAT_GAP_INUSE)             */
    FAT_EV_BUSY,            /* kept changing while being moved; skipped     */
    FAT_EV_CROSSLINKED,     /* shares clusters with another object          */
    FAT_EV_DOTDOT           /* a moved directory kept its old copy: some
                               ".." entries still point there              */
};

/* What follows a gap that is left in the free space (fat_gap_note.why). */
enum {
    FAT_GAP_SYSTEM = 0,     /* a system file that stays (see fat_run)       */
    FAT_GAP_INUSE,          /* a file reported by cfg->is_unmovable         */
    FAT_GAP_ROOT,           /* the first cluster of the FAT32 root folder   */
    FAT_GAP_STUCK,          /* a file or folder that was not moved          */
    FAT_GAP_LOST,           /* clusters no file or folder owns: lost
                               clusters, or data written during the run     */
    FAT_GAP_BAD             /* clusters marked bad                          */
};

#define FAT_GAP_NOTES 4
#define FAT_KEPT_NOTES 8

typedef struct {
    fat_u32 clusters;       /* size of the gap                              */
    int     why;            /* FAT_GAP_*                                    */
    char    path[64];       /* what follows it ("" for LOST and BAD); a
                               longer path keeps its end after "..."        */
} fat_gap_note;

typedef struct {
    fat_u32 clusters;       /* size of the file                             */
    fat_u32 frags;          /* fragments it has                             */
    int     why;            /* FAT_GAP_SYSTEM or FAT_GAP_INUSE              */
    char    path[64];       /* path (its end kept after "..." if longer)    */
} fat_kept_note;

/* Phases reported through fat_defrag_cfg.progress. */
enum {
    FAT_PH_SCAN = 0,        /* done = objects found, total = 0              */
    FAT_PH_DEFRAG,          /* done/total = fragmented objects processed    */
    FAT_PH_COMPACT          /* done = objects moved so far, total = 0       */
};

/* Results of fat_defrag_cfg.begin_write. */
enum {
    FAT_WR_FAIL = 0,        /* cannot write at all: stop the run            */
    FAT_WR_OK,              /* nothing changed since the last section       */
    FAT_WR_CHANGED,         /* someone else changed the volume (or the set
                               of unmovable files); the hook has already
                               re-read the FAT with fat_reload()            */
    FAT_WR_BUSY             /* volume too busy right now; try again later   */
};

typedef struct {
    int   doMove;   /* 0 = analyze only, 1 = defragment                      */
    int   verify;   /* read moved clusters back and compare                  */
    int   compact;  /* consolidate free space (analyze: plan it in memory)   */
    int   offline;  /* Windows is not running (MS-DOS): its swap file and
                       registry are ordinary files and may be moved          */
    void *cbctx;
    void (*progress)(void *ctx, int phase, fat_u64 done, fat_u64 total);
    void (*event)(void *ctx, int ev, const char *path, int frags,
                  int newFrags);

    /*
     * Optional write-section hooks for a live volume (NULL = always writable).
     * All disk writes happen between begin_write and end_write. Inside a
     * section the engine performs only sector I/O: it allocates no memory and
     * makes no progress/event callbacks, and it leaves the file system
     * consistent before calling end_write. Sections are kept short (about
     * 1 MB of copied data); big files are copied across several sections and
     * the copy is restarted if the volume changed in between.
     */
    int  (*begin_write)(void *ctx);
    void (*end_write)(void *ctx);

    /* Optional: TRUE if the file starting at this cluster must not be moved
     * (e.g. in use by the system). Consulted when planning and again right
     * before every move. */
    int  (*is_unmovable)(void *ctx, fat_u32 startCluster);
} fat_defrag_cfg;

typedef struct {
    fat_u64 filesExamined;
    fat_u64 dirsExamined;      /* subdirectories (the root is not counted)  */
    fat_u64 contiguous;        /* files found in one piece                  */
    fat_u64 fragmented;        /* fragmented files found                    */
    fat_u64 totalFragments;    /* summed fragments over fragmented files    */
    fat_u64 dirsFragmented;    /* fragmented directories found (incl. root) */
    fat_u64 defragged;         /* files made contiguous                     */
    fat_u64 dirsDefragged;     /* directories made contiguous               */
    fat_u64 compacted;         /* other objects moved to consolidate free   */
                               /* space (not counted as defragmented)       */
    fat_u64 clustersMoved;
    fat_u64 unmovable;         /* system or in-use files (never moved)      */
    fat_u64 unmovableFragmented; /* ... of which fragmented (incl. root)    */
    fat_u64 noSpace;           /* fragmented but no room to fix             */
    fat_u64 busy;              /* skipped because they kept changing        */
    fat_u64 ioErrors;
    fat_u64 chainErrors;
    fat_u64 dotdotErrors;   /* a moved directory kept its old copy: some
                               ".." entries still point there              */
    fat_u64 crossLinked;       /* objects sharing clusters: nothing moved   */
    int     aborted;           /* the run was stopped early                 */

    /* Analyze only: what a full defragmentation would do. */
    fat_u64 planDefrag;        /* fragmented objects it would fix           */
    fat_u64 compactable;       /* other objects it would move to consolidate */
    fat_u32 planFreeRuns;      /* free runs it would leave                  */

    /* Free-space picture at the end of the run. A "gap" is a free run that
     * lies before the last allocated cluster. */
    fat_u32 freeClusters;
    fat_u32 freeRuns;
    fat_u32 largestFreeRun;
    fat_u32 freeGaps;
    fat_u32 gapClusters;

    /* The largest gaps left at the end of the run (analyze: the gaps the
     * planned defragmentation would leave), largest first, with what
     * follows each one. */
    fat_u32       gapsLeft;     /* all gaps left                            */
    fat_u32       gapNotes;     /* entries used in gapNote                  */
    fat_gap_note  gapNote[FAT_GAP_NOTES];
    fat_u32       keptNotes;    /* entries used in keptNote                 */
    fat_kept_note keptNote[FAT_KEPT_NOTES];
    fat_u32       lostClusters; /* cfg->offline only: allocated clusters no
                                   file or folder owns (ScanDisk frees them) */
} fat_stats;

/*
 * Walk the whole volume. In analyze mode it only measures fragmentation (and,
 * with cfg->compact, plans a full defragmentation in memory). In defrag mode it
 * makes fragmented files and directories contiguous and, with cfg->compact,
 * consolidates free space. The first cluster of the FAT32 root directory,
 * the system files (hidden system files in the root folder other than the
 * logs, backups, drivers and settings that MS-DOS and Windows open by name,
 * LDLINUX.SYS and, unless cfg->offline, the Windows swap file and registry;
 * see is_system in fatdefrag.c) and files reported by cfg->is_unmovable are
 * never moved. Other hidden, system and read-only files, all subdirectories
 * and the rest of the FAT32 root directory may move.
 * Returns 0 when the work completed cleanly, 1 when something was left undone
 * (no space, I/O error, bad or cross-linked chains, busy files, aborted), and
 * -1 when out of memory.
 */
int fat_run(fat_volume *fv, const fat_defrag_cfg *cfg, fat_stats *st);

/*
 * Resolve a path ("C:\DIR\FILE.EXT", "\DIR\FILE" or "DIR/FILE"; 8.3 or long
 * names, ASCII case-insensitive) to the first cluster of the file or
 * directory by reading the on-disk directories. Uses no heap memory, so it is
 * safe inside a write section. Returns 1 and sets *start when found.
 */
int fat_lookup(fat_volume *fv, const char *path, fat_u32 *start);

#endif /* HSDEFRAG_FATDEFRAG_H */

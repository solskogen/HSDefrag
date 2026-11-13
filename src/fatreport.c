/*
 * fatreport.c - The FAT engine's report text (see fatreport.h).
 *
 * A line is assembled in a fixed buffer with a few append helpers instead
 * of printf: the Win32 program has no C library, and every value printed
 * here is a string or an unsigned number. Lines longer than the buffer are
 * cut, never overrun.
 */
#include "fatreport.h"

#define LINE_CAP 400

typedef struct {
    char     s[LINE_CAP];
    unsigned n;
} line_t;

static unsigned slen(const char *s)
{
    unsigned n = 0;

    while (s[n])
        n++;
    return n;
}

static void put(line_t *l, const char *s)
{
    while (*s && l->n + 2 < LINE_CAP)  /* room for "\n" and the NUL */
        l->s[l->n++] = *s++;
}

static void put_ul(line_t *l, unsigned long v)
{
    char d[24];
    int i = 0;

    do {
        d[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i && l->n + 2 < LINE_CAP)
        l->s[l->n++] = d[--i];
}

static void put_int(line_t *l, int v)
{
    if (v < 0) {
        put(l, "-");
        put_ul(l, (unsigned long)-(long)v);
    } else {
        put_ul(l, (unsigned long)v);
    }
}

static void emit(const fat_report *r, int level, line_t *l)
{
    l->s[l->n++] = '\n';
    l->s[l->n] = 0;
    r->out(r->ctx, level, l->s);
    l->n = 0;
}

static const char *plural(unsigned long n)
{
    return n == 1 ? "" : "s";
}

const char *fat_format_bytes(fat_u64 bytes, char *buf, unsigned cap)
{
    static const char *const unit[] = { "B", "KB", "MB", "GB" };
    fat_u64 div = 1, tenths;
    line_t l;
    int u = 0;

    while (u < 3 && bytes >= div * 1024) {
        div *= 1024;
        u++;
    }
    /* Rounded to tenths; bytes stays far below the overflow of *10. */
    tenths = (bytes * 10 + div / 2) / div;
    l.n = 0;
    put_ul(&l, (unsigned long)(tenths / 10));
    put(&l, ".");
    put_ul(&l, (unsigned long)(tenths % 10));
    put(&l, " ");
    put(&l, unit[u]);
    if (cap) {
        unsigned n = l.n < cap - 1 ? l.n : cap - 1, i;

        for (i = 0; i < n; i++)
            buf[i] = l.s[i];
        buf[n] = 0;
    }
    return buf;
}

static void put_bytes(line_t *l, fat_u64 bytes)
{
    char a[32];

    put(l, fat_format_bytes(bytes, a, sizeof(a)));
}

void fat_report_event(const fat_report *r, int ev, const char *path,
                      int frags, int newFrags)
{
    int level = FAT_OUT_VERBOSE;
    line_t l;

    l.n = 0;
    switch (ev) {
    case FAT_EV_CONTIGUOUS:
        put(&l, "  contiguous (");
        put_int(&l, newFrags);
        put(&l, " clusters): ");
        break;
    case FAT_EV_FRAGMENTED:
        put(&l, "  fragmented (");
        put_int(&l, frags);
        put(&l, " fragments, ");
        put_int(&l, newFrags);
        put(&l, " clusters): ");
        break;
    case FAT_EV_DEFRAGGED:
    case FAT_EV_DIRDEFRAGGED:
        put(&l, "  defragmented (");
        put_int(&l, frags);
        put(&l, " fragments): ");
        break;
    case FAT_EV_MOVED:
        put(&l, "  moved (");
        put_int(&l, frags);
        put(&l, " clusters): ");
        break;
    case FAT_EV_NOSPACE:
        put(&l, "  no contiguous free space yet, retried later (");
        put_int(&l, frags);
        put(&l, " fragments): ");
        break;
    case FAT_EV_UNMOVABLE:
        put(&l, "  kept in place, ");
        put(&l, newFrags == FAT_GAP_SYSTEM ? "system file" : r->inUse);
        if (frags > 1) {
            put(&l, " (");
            put_int(&l, frags);
            put(&l, " fragments)");
        }
        put(&l, ": ");
        break;
    case FAT_EV_BUSY:
        level = FAT_OUT_INFO;
        put(&l, "  skipped, kept changing: ");
        break;
    case FAT_EV_IOERROR:
        level = FAT_OUT_INFO;
        put(&l, "  I/O error: ");
        break;
    case FAT_EV_CHAINERROR:
        level = FAT_OUT_INFO;
        put(&l, "  bad cluster chain: ");
        break;
    case FAT_EV_CROSSLINKED:
        level = FAT_OUT_INFO;
        put(&l, "  cross-linked: ");
        break;
    case FAT_EV_DOTDOT:
        level = FAT_OUT_INFO;
        put(&l, "  moved, but the old copy was kept: some \"..\" entries of "
                "its subfolders still point there: ");
        break;
    default:
        return;
    }
    put(&l, path ? path : "");
    emit(r, level, &l);
}

void fat_report_kept(const fat_report *r, const fat_stats *st)
{
    fat_u32 k, more = (fat_u32)st->unmovable - st->keptNotes;
    line_t l;

    l.n = 0;
    for (k = 0; k < st->keptNotes; k++) {
        const fat_kept_note *n = &st->keptNote[k];

        put(&l, "    ");
        put(&l, n->path);
        put(&l, " (");
        put(&l, n->why == FAT_GAP_SYSTEM ? "system file" : r->inUse);
        put(&l, ", ");
        put_bytes(&l, (fat_u64)n->clusters * r->bytesPerCluster);
        if (n->frags > 1) {
            put(&l, ", ");
            put_ul(&l, (unsigned long)n->frags);
            put(&l, " fragments");
        }
        put(&l, ")");
        emit(r, FAT_OUT_INFO, &l);
    }
    if (more) {
        put(&l, "    ... and ");
        put_ul(&l, (unsigned long)more);
        put(&l, " more file");
        put(&l, plural(more));
        emit(r, FAT_OUT_INFO, &l);
    }
}

void fat_report_free_space(const fat_report *r, const fat_stats *st)
{
    fat_u64 bpc = r->bytesPerCluster;
    line_t l;

    l.n = 0;
    put(&l, "  free space           : ");
    put_bytes(&l, st->freeClusters * bpc);
    put(&l, " in ");
    put_ul(&l, (unsigned long)st->freeRuns);
    put(&l, " run");
    put(&l, plural((unsigned long)st->freeRuns));
    put(&l, " (largest ");
    put_bytes(&l, st->largestFreeRun * bpc);
    put(&l, ")");
    emit(r, FAT_OUT_INFO, &l);
    if (st->freeGaps) {
        put(&l, "  free between files   : ");
        put_bytes(&l, st->gapClusters * bpc);
        put(&l, " in ");
        put_ul(&l, (unsigned long)st->freeGaps);
        put(&l, " gap");
        put(&l, plural((unsigned long)st->freeGaps));
        emit(r, FAT_OUT_INFO, &l);
    }
}

void fat_report_gaps(const fat_report *r, const fat_stats *st)
{
    fat_u32 k, more = st->gapsLeft - st->gapNotes;
    line_t l;

    l.n = 0;
    for (k = 0; k < st->gapNotes; k++) {
        const fat_gap_note *g = &st->gapNote[k];
        const char *why = g->why == FAT_GAP_SYSTEM ? "system file"
                        : g->why == FAT_GAP_INUSE  ? r->inUse
                        : g->why == FAT_GAP_STUCK  ? "not moved"
                                                   : 0;
        char a[32];
        int room, len;

        fat_format_bytes((fat_u64)g->clusters * r->bytesPerCluster, a,
                         sizeof(a));
        put(&l, "    ");
        put(&l, a);
        put(&l, " before ");
        if (!why) {
            put(&l, g->why == FAT_GAP_ROOT  ? "the root folder"
                  : g->why == FAT_GAP_BAD ? "bad clusters"
                                          : r->lostGap);
            emit(r, FAT_OUT_INFO, &l);
            continue;
        }
        /* Keep the line within 79 columns: long paths lose their start,
         * since the end names the file. " (" + why + ")" follows. */
        len = (int)slen(g->path);
        room = 79 - 12 - (int)slen(a) - (int)slen(why) - 3;
        if (room < 8)
            room = 8;
        if (len > room) {
            put(&l, "...");
            put(&l, g->path + len - (room - 3));
        } else {
            put(&l, g->path);
        }
        put(&l, " (");
        put(&l, why);
        put(&l, ")");
        emit(r, FAT_OUT_INFO, &l);
    }
    if (more) {
        put(&l, "    ... and ");
        put_ul(&l, (unsigned long)more);
        put(&l, " more gap");
        put(&l, plural(more));
        emit(r, FAT_OUT_INFO, &l);
    }
}

void fat_report_lost(const fat_report *r, const fat_stats *st)
{
    line_t l;

    if (!st->lostClusters)
        return;
    l.n = 0;
    put(&l, "  lost clusters        : ");
    put_ul(&l, (unsigned long)st->lostClusters);
    put(&l, " (");
    put_bytes(&l, (fat_u64)st->lostClusters * r->bytesPerCluster);
    put(&l, "; run ScanDisk to free them)");
    emit(r, FAT_OUT_INFO, &l);
}

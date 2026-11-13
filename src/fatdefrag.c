/*
 * fatdefrag.c - FAT fragmentation analysis and defragmenter (see fatdefrag.h).
 *
 * No Win32 dependency; drives the abstract fat_volume device only.
 *
 * The volume is scanned once into a table of objects (files and directories;
 * a parent always precedes its children). The moves then work from that table
 * and a sorted list of free cluster runs, and every move is re-validated
 * against the disk right before it is made, so changes made meanwhile by other
 * programs on a live volume are detected instead of overwritten.
 */
#include "fatdefrag.h"
#include "nostd.h"

#define MAX_DEPTH    64               /* directory nesting followed         */
#define PATH_CAP     300
#define COPY_BYTES   (64u * 1024u)    /* copy buffer (at least one cluster) */
#define SEC_BYTES    (1024u * 1024u)  /* data copied per write section      */
#define SEC_MOVES    8                /* moves per write section            */
#define QUEUE_CAP    32               /* events held back inside a section  */
#define MAX_ATTEMPTS 8                /* tries per move                     */
#define MAX_RETRIES  2                /* restarts of an interrupted copy    */
#define MAX_BUSY     20               /* "busy" answers in a row            */
#define MAX_RESCANS  2
#define MAX_PASSES   1000
#define SLIDE_STEPS  64               /* longer slides take a round trip    */
#define TRIP_PASSES  4                /* sweeps that may move a unit up     */
#define MAX_ROUNDS   64               /* compact/defragment rounds          */
#define EXT_SLACK    64

/* Object flags. */
#define OF_DIR       0x0001u
#define OF_ROOT      0x0002u
#define OF_FIXEDROOT 0x0004u  /* FAT12/16 root: fixed sectors, no chain   */
#define OF_SYSTEM    0x0008u  /* system file that stays (is_system)       */
#define OF_PINNED    0x0010u  /* reported unmovable by the caller         */
#define OF_BAD       0x0020u  /* invalid chain or directory               */
#define OF_CROSS     0x0040u  /* shares clusters with another object      */
#define OF_NOSPACE   0x0080u
#define OF_BUSY      0x0100u
#define OF_IOERR     0x0200u
#define OF_GONE      0x0400u  /* deleted, renamed or emptied meanwhile    */
#define OF_DEFRAGGED 0x0800u
#define OF_COMPACTED 0x1000u
#define OF_MOVED     0x2000u

#define OF_NOMOVE (OF_ROOT | OF_SYSTEM | OF_PINNED | OF_BAD | OF_CROSS | \
                   OF_BUSY | OF_IOERR | OF_GONE)

typedef struct {
    fat_u32 start;     /* first cluster (0: empty file or fixed root)    */
    fat_u32 clusters;  /* length of the chain                            */
    fat_u32 frags;     /* number of fragments (1 = contiguous)           */
    fat_u32 parent;    /* index of the parent directory                  */
    fat_u32 entry;     /* index of its 32-byte entry within the parent   */
    fat_u16 flags;
    fat_u8  retries;
    fat_u8  name[11];  /* raw 8.3 name: re-identifies the entry          */
} fobj;

typedef struct {
    fat_u32 start, len;
} fext;

typedef struct {
    fat_u32 idx;
    int     ev, frags, newFrags;
} fevent;

typedef struct {
    fat_u32 idx;       /* object                                         */
    fat_u32 k0;        /* index of its first cluster within the chain    */
    fat_u32 start, len;
    fat_u32 reach;     /* end of the units that follow it without a gap  */
} funit;

typedef struct {
    fat_volume           *fv;
    const fat_defrag_cfg *cfg;
    fat_stats            *st;
    int                   sim;         /* plan in memory, write nothing     */
    int                   stop, oom;
    fat_u32               lastCluster; /* highest valid cluster index       */

    fobj                 *obj;
    fat_u32               nObj, capObj;

    fat_u32              *chain;       /* chain of the current object       */
    fat_u32               chainCap, chainNeed;
    fat_u8               *seen;        /* scan: clusters already claimed    */

    fext                 *ext;         /* free runs: sorted, never adjacent */
    fat_u32              *pmax;        /* running maximum of ext[].len      */
    fat_u32               nExt, capExt, extNeed;
    int                   pmaxDirty, extStale, pinStale;
    int                   fsinfoCleared; /* FSInfo count cleared this run */

    funit                *unit;        /* compaction: what may move         */
    fat_u32               nUnit, capUnit;
    fat_u32              *bySize;      /* units ordered by length, start    */
    fat_u32              *rank;        /* unit -> its place in bySize       */
    long                 *todo;        /* bySize places of the units still to
                                          place: todo[p] == p, else a lower
                                          place to look at instead          */
    fat_u32               capSize, nOpen;

    fat_u8               *buf, *vbuf;  /* copy and verify buffers           */
    fat_u32               bufClusters;
    fat_u8               *sec, *sec2;  /* one sector each                   */

    int                   inSec;       /* inside a write section            */
    fat_u64               secBytes;
    fat_u32               secMoves;
    int                   busyRun;
    fevent                queue[QUEUE_CAP];
    int                   nQueue;

    int                   phase;
    fat_u64               phaseDone, phaseTotal;
    char                  path[PATH_CAP];
} ctx_t;

/* ---------------------------------------------------------------- helpers */

static fat_u16 rd16le(const fat_u8 *p)
{
    return (fat_u16)(p[0] | ((fat_u16)p[1] << 8));
}

static fat_u32 rd32le(const fat_u8 *p)
{
    return rd16le(p) | ((fat_u32)rd16le(p + 2) << 16);
}

static void wr16le(fat_u8 *p, fat_u16 v)
{
    p[0] = (fat_u8)v;
    p[1] = (fat_u8)(v >> 8);
}

static fat_u32 entry_start(const fat_volume *fv, const fat_u8 *e)
{
    fat_u32 s = rd16le(e + 0x1A);

    if (fv->fatType == FAT_TYPE_32)
        s |= (fat_u32)rd16le(e + 0x14) << 16;
    return s;
}

/* FAT12/16 leave offset 0x14 alone: it is not part of the cluster number. */
static void set_entry_start(const fat_volume *fv, fat_u8 *e, fat_u32 start)
{
    wr16le(e + 0x1A, (fat_u16)(start & 0xFFFFu));
    if (fv->fatType == FAT_TYPE_32)
        wr16le(e + 0x14, (fat_u16)(start >> 16));
}

/* A file or directory entry (not deleted, a long-name part, the volume
 * label, "." or ".."). */
static int is_object_entry(const fat_u8 *e)
{
    return e[0] != 0xE5 && e[0] != '.' && (e[0x0B] & 0x0F) != 0x0F &&
           !(e[0x0B] & 0x08);
}

static int is_dot_entry(const fat_u8 *e, const char *name)
{
    return memcmp(e, name, 11) == 0 && (e[0x0B] & 0x10);
}

/* Format an 8.3 directory name into "NAME.EXT". */
static void fmt_name(const fat_u8 *e, char *out)
{
    int i, k = 0;

    for (i = 0; i < 8 && e[i] != ' '; i++)
        out[k++] = (char)e[i];
    if (e[8] != ' ') {
        out[k++] = '.';
        for (i = 8; i < 11 && e[i] != ' '; i++)
            out[k++] = (char)e[i];
    }
    out[k] = 0;
    if ((fat_u8)out[0] == 0x05)
        out[0] = (char)0xE5;
}

static int movable(const fobj *o)
{
    return !(o->flags & OF_NOMOVE) && o->clusters > 0;
}

/* The FAT32 root folder: the boot sector and the system's drive tables hold
 * its first cluster, which stays, but the rest of its chain moves like the
 * pieces of any folder. */
static int root_tail(const fobj *o)
{
    return (o->flags & (OF_NOMOVE | OF_FIXEDROOT)) == OF_ROOT &&
           o->clusters > 1;
}

static void stop_run(ctx_t *c)
{
    c->st->aborted = 1;
    c->stop = 1;
}

/* ------------------------------------------------------------------ chains */

static int chain_reserve(ctx_t *c, fat_u32 need)
{
    fat_u32 nc = c->chainCap ? c->chainCap : 64;
    fat_u32 *p;

    if (need <= c->chainCap)
        return 1;
    while (nc < need)
        nc *= 2;
    p = (fat_u32 *)realloc(c->chain, (size_t)nc * sizeof(fat_u32));
    if (!p) {
        c->oom = c->stop = 1;
        return 0;
    }
    c->chain = p;
    c->chainCap = nc;
    return 1;
}

/*
 * Follow the chain from "start" into c->chain. Returns 1 (*outN = length), 0
 * for an invalid chain, or -1 when out of memory or when, inside a write
 * section (where nothing may be allocated), the chain is longer than the
 * buffer; c->chainNeed then holds the length needed.
 */
static int build_chain(ctx_t *c, fat_u32 start, fat_u32 *outN)
{
    fat_volume *fv = c->fv;
    fat_u32 n = 0, cl = start;
    int over = 0;

    *outN = 0;
    if (!fat_cluster_valid(fv, start))
        return 0;
    for (;;) {
        fat_u32 nx;

        if (n >= c->chainCap && !over) {
            if (c->inSec)
                over = 1;
            else if (!chain_reserve(c, n + 1))
                return -1;
        }
        if (!over)
            c->chain[n] = cl;
        if (++n > fv->countOfClusters)
            return 0; /* cyclic */
        nx = fat_get(fv, cl);
        if (fat_is_eoc(fv, nx))
            break;
        if (!fat_cluster_valid(fv, nx) || fat_is_bad(fv, nx))
            return 0;
        cl = nx;
    }
    if (over) {
        c->chainNeed = n;
        return -1;
    }
    *outN = n;
    return 1;
}

static fat_u32 count_frag(const fat_u32 *a, fat_u32 n)
{
    fat_u32 i, f = n ? 1 : 0;

    for (i = 0; i + 1 < n; i++)
        if (a[i + 1] != a[i] + 1)
            f++;
    return f;
}

/* --------------------------------------------------------- paths, events */

/* Path of object idx ("\DIR\FILE.EXT"; directories end in a backslash). */
static const char *obj_path(ctx_t *c, fat_u32 idx)
{
    fat_u32 stack[MAX_DEPTH + 2], i = idx;
    int n = 0;
    size_t len = 0;

    while (i != 0 && n < MAX_DEPTH + 2) {
        stack[n++] = i;
        i = c->obj[i].parent;
    }
    while (n-- > 0) {
        char nm[16];
        const char *p = nm;

        fmt_name(c->obj[stack[n]].name, nm);
        if (len + 1 < PATH_CAP)
            c->path[len++] = '\\';
        while (*p && len + 1 < PATH_CAP)
            c->path[len++] = *p++;
    }
    if ((idx == 0 || (c->obj[idx].flags & OF_DIR)) && len + 1 < PATH_CAP)
        c->path[len++] = '\\';
    c->path[len] = 0;
    return c->path;
}

/* Report an event; inside a write section it is held back until the end. */
static void emit(ctx_t *c, fat_u32 idx, int ev, fat_u32 frags,
                 fat_u32 newFrags)
{
    const fat_defrag_cfg *cfg = c->cfg;

    if (!cfg->event || c->sim)
        return;
    if (c->inSec) {
        if (c->nQueue < QUEUE_CAP) {
            fevent *q = &c->queue[c->nQueue++];
            q->idx = idx;
            q->ev = ev;
            q->frags = (int)frags;
            q->newFrags = (int)newFrags;
        }
        return;
    }
    cfg->event(cfg->cbctx, ev, obj_path(c, idx), (int)frags, (int)newFrags);
}

static void progress(ctx_t *c)
{
    if (c->cfg->progress && !c->inSec && !c->sim)
        c->cfg->progress(c->cfg->cbctx, c->phase, c->phaseDone,
                         c->phaseTotal);
}

/* -------------------------------------------------------------- free runs */

static int ext_reserve(ctx_t *c, fat_u32 need)
{
    fat_u32 nc, *pm;
    fext *e;

    if (need <= c->capExt)
        return 1;
    nc = need + need / 2 + EXT_SLACK;
    e = (fext *)realloc(c->ext, (size_t)nc * sizeof(fext));
    if (!e)
        goto oom;
    c->ext = e;
    pm = (fat_u32 *)realloc(c->pmax, (size_t)nc * sizeof(fat_u32));
    if (!pm)
        goto oom;
    c->pmax = pm;
    c->capExt = nc;
    return 1;
oom:
    c->oom = c->stop = 1;
    return 0;
}

/*
 * Rebuild the free list from the FAT in one pass (it may run while the volume
 * is locked). Returns 0 when the list needs more room first: c->extNeed then
 * holds the capacity to reserve. Two spare slots are always kept.
 */
static int ext_build(ctx_t *c)
{
    fat_u32 cl, n = 0, cap = c->capExt;
    int run = 0;

    for (cl = 2; cl <= c->lastCluster; cl++) {
        if (!fat_is_free(fat_get(c->fv, cl))) {
            run = 0;
        } else if (run) {
            if (n + 1 <= cap) /* run n-1 was stored */
                c->ext[n - 1].len++;
        } else {
            run = 1;
            if (n + 2 <= cap) {
                c->ext[n].start = cl;
                c->ext[n].len = 1;
            }
            n++;
        }
    }
    if (n + 2 > cap) {
        c->extNeed = n + 2;
        return 0;
    }
    c->nExt = n;
    c->pmaxDirty = 1;
    c->extStale = 0;
    return 1;
}

/* Index of the last free run starting at or before cl, or -1. */
static int ext_find(const ctx_t *c, fat_u32 cl)
{
    int lo = 0, hi = (int)c->nExt - 1, r = -1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;

        if (c->ext[mid].start <= cl) {
            r = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return r;
}

/* Remove [start, start+len) from the free list. It must lie within one run;
 * otherwise the list no longer matches the FAT and is marked stale. */
static void ext_take(ctx_t *c, fat_u32 start, fat_u32 len)
{
    int i = ext_find(c, start), n = (int)c->nExt;
    fat_u32 end = start + len, eEnd;
    fext *e;

    if (i < 0)
        goto stale;
    e = &c->ext[i];
    eEnd = e->start + e->len;
    if (end > eEnd)
        goto stale;
    if (start == e->start && end == eEnd) {
        memmove(e, e + 1, (size_t)(n - i - 1) * sizeof(fext));
        c->nExt--;
    } else if (start == e->start) {
        e->start = end;
        e->len = eEnd - end;
    } else if (end == eEnd) {
        e->len = start - e->start;
    } else {
        if (c->nExt >= c->capExt)
            goto stale;
        memmove(e + 2, e + 1, (size_t)(n - i - 1) * sizeof(fext));
        e->len = start - e->start;
        e[1].start = end;
        e[1].len = eEnd - end;
        c->nExt++;
    }
    c->pmaxDirty = 1;
    return;
stale:
    c->extStale = 1;
}

/* Return [start, start+len) to the free list, merging with neighbours. */
static void ext_give(ctx_t *c, fat_u32 start, fat_u32 len)
{
    int i = ext_find(c, start) + 1, n = (int)c->nExt, prev = 0, next = 0;
    fat_u32 end = start + len;

    if (i > 0) {
        fat_u32 pEnd = c->ext[i - 1].start + c->ext[i - 1].len;

        if (pEnd > start)
            goto stale;
        prev = pEnd == start;
    }
    if (i < n) {
        if (end > c->ext[i].start)
            goto stale;
        next = end == c->ext[i].start;
    }
    if (prev && next) {
        c->ext[i - 1].len += len + c->ext[i].len;
        memmove(&c->ext[i], &c->ext[i + 1], (size_t)(n - i - 1) * sizeof(fext));
        c->nExt--;
    } else if (prev) {
        c->ext[i - 1].len += len;
    } else if (next) {
        c->ext[i].start = start;
        c->ext[i].len += len;
    } else {
        if (c->nExt >= c->capExt)
            goto stale;
        memmove(&c->ext[i + 1], &c->ext[i], (size_t)(n - i) * sizeof(fext));
        c->ext[i].start = start;
        c->ext[i].len = len;
        c->nExt++;
    }
    c->pmaxDirty = 1;
    return;
stale:
    c->extStale = 1;
}

/* Lowest start of "need" free clusters within [lo, hi), or 0. */
static fat_u32 ext_first_fit(ctx_t *c, fat_u32 need, fat_u32 lo, fat_u32 hi)
{
    int a = 0, b = (int)c->nExt, i;

    if (c->pmaxDirty) {
        fat_u32 m = 0;

        for (i = 0; i < (int)c->nExt; i++) {
            if (c->ext[i].len > m)
                m = c->ext[i].len;
            c->pmax[i] = m;
        }
        c->pmaxDirty = 0;
    }
    while (a < b) { /* first run that is long enough on its own */
        int mid = a + (b - a) / 2;

        if (c->pmax[mid] >= need)
            b = mid;
        else
            a = mid + 1;
    }
    i = ext_find(c, lo);
    if (i < a)
        i = a;
    for (; i < (int)c->nExt; i++) {
        fat_u32 s = c->ext[i].start, e = s + c->ext[i].len;

        if (s >= hi)
            break;
        if (s < lo)
            s = lo;
        if (e > hi)
            e = hi;
        if (e > s && e - s >= need)
            return s;
    }
    return 0;
}

static int range_free(const ctx_t *c, fat_u32 start, fat_u32 len)
{
    fat_u32 i;

    if (start < 2 || start > c->lastCluster ||
        len > c->lastCluster - start + 1)
        return 0;
    for (i = 0; i < len; i++)
        if (!fat_is_free(fat_get(c->fv, start + i)))
            return 0;
    return 1;
}

/* ------------------------------------------------------- write sections */

static void refresh_pins(ctx_t *c)
{
    const fat_defrag_cfg *cfg = c->cfg;
    fat_u32 i;

    c->pinStale = 0;
    if (!cfg->is_unmovable)
        return;
    for (i = 1; i < c->nObj; i++) {
        fobj *o = &c->obj[i];

        if (o->start && cfg->is_unmovable(cfg->cbctx, o->start))
            o->flags |= OF_PINNED;
        else
            o->flags &= (fat_u16)~OF_PINNED;
    }
}

static void sec_leave(ctx_t *c)
{
    int i;

    if (!c->inSec)
        return;
    if (c->fv->fatDirty && !fat_flush(c->fv)) {
        c->st->ioErrors++;
        stop_run(c);
    }
    if (c->cfg->end_write)
        c->cfg->end_write(c->cfg->cbctx);
    c->inSec = 0;
    for (i = 0; i < c->nQueue; i++) {
        const fevent *q = &c->queue[i];

        c->cfg->event(c->cfg->cbctx, q->ev, obj_path(c, q->idx), q->frags,
                      q->newFrags);
    }
    c->nQueue = 0;
    progress(c);
}

/*
 * Open a write section. Returns FAT_WR_OK, FAT_WR_CHANGED (the FAT was re-read,
 * so anything planned before must be planned again), FAT_WR_BUSY, or
 * FAT_WR_FAIL (the run is stopped).
 */
static int sec_enter(ctx_t *c)
{
    const fat_defrag_cfg *cfg = c->cfg;
    int tries, changed = 0;

    if (c->inSec)
        return FAT_WR_OK;
    for (tries = 0; tries < 4 && !c->stop; tries++) {
        int w = cfg->begin_write ? cfg->begin_write(cfg->cbctx) : FAT_WR_OK;

        if (w == FAT_WR_BUSY) {
            if (++c->busyRun >= MAX_BUSY)
                stop_run(c);
            return FAT_WR_BUSY;
        }
        if (w != FAT_WR_OK && w != FAT_WR_CHANGED) {
            stop_run(c);
            return FAT_WR_FAIL;
        }
        c->busyRun = 0;
        c->inSec = 1;
        c->secBytes = 0;
        c->secMoves = 0;
        if (w == FAT_WR_CHANGED) {
            changed = 1;
            c->extStale = c->pinStale = 1;
        }
        if (c->extStale && !ext_build(c)) {
            sec_leave(c);
            if (!ext_reserve(c, c->extNeed + EXT_SLACK))
                return FAT_WR_FAIL;
            continue;
        }
        if (c->pinStale)
            refresh_pins(c);
        return changed ? FAT_WR_CHANGED : FAT_WR_OK;
    }
    return c->stop ? FAT_WR_FAIL : FAT_WR_BUSY;
}

/* Leave the section once it has done enough work or events pile up. */
static void sec_check(ctx_t *c)
{
    if (c->inSec && (c->secBytes >= SEC_BYTES || c->secMoves >= SEC_MOVES ||
                     c->nQueue + 4 > QUEUE_CAP))
        sec_leave(c);
}

/* ------------------------------------------------------------------ moves */

/* Sector and byte offset of object idx's directory entry, found through the
 * parent's current chain. */
static int locate(ctx_t *c, fat_u32 idx, fat_u32 *sector, fat_u32 *off)
{
    fat_volume *fv = c->fv;
    const fobj *o = &c->obj[idx];
    const fobj *p = &c->obj[o->parent];
    fat_u32 eps = fv->bytesPerSector / 32;

    if (p->flags & OF_FIXEDROOT) {
        if (o->entry >= fv->rootEntryCount)
            return 0;
        *sector = fv->firstRootDirSector + o->entry / eps;
    } else {
        fat_u32 k = o->entry / (eps * fv->sectorsPerCluster), cl = p->start;

        while (k-- > 0) {
            if (!fat_cluster_valid(fv, cl))
                return 0;
            cl = fat_get(fv, cl);
        }
        if (!fat_cluster_valid(fv, cl))
            return 0;
        *sector = fat_cluster_sector(fv, cl) +
                  (o->entry / eps) % fv->sectorsPerCluster;
    }
    *off = (o->entry % eps) * 32;
    return 1;
}

enum { V_OK, V_REPLAN, V_GONE, V_IOERR, V_GROW, V_PINNED };

/*
 * Inside a section: check that object idx still is what the table says by
 * reading its directory entry and following its chain in the current FAT.
 * Leaves the chain in c->chain. V_REPLAN: it changed (the table is updated).
 */
static int validate(ctx_t *c, fat_u32 idx)
{
    fat_volume *fv = c->fv;
    const fat_defrag_cfg *cfg = c->cfg;
    fobj *o = &c->obj[idx];
    fat_u32 sector, off, start = o->start, n = 0, frags, size = 0;
    const fat_u8 *e;
    int r, pinned, isDir = (o->flags & OF_DIR) != 0;

    if (idx != 0) { /* the root folder has no entry; its start never moves */
        if (!locate(c, idx, &sector, &off))
            return V_GONE;
        if (!fat_read_sectors(fv, sector, 1, c->sec))
            return V_IOERR;
        e = c->sec + off;
        isDir = (e[0x0B] & 0x10) != 0;
        if (memcmp(e, o->name, 11) != 0 || !is_object_entry(e) ||
            isDir != ((o->flags & OF_DIR) != 0))
            return V_GONE;
        start = entry_start(fv, e);
        size = rd32le(e + 0x1C);
    }
    r = build_chain(c, start, &n);
    if (r < 0)
        return c->oom ? V_GONE : V_GROW;
    if (r == 0)
        return V_GONE;
    if (!isDir &&
        ((fat_u64)size + fv->bytesPerCluster - 1) / fv->bytesPerCluster != n)
        return V_GONE;
    frags = count_frag(c->chain, n);
    pinned = idx && cfg->is_unmovable && cfg->is_unmovable(cfg->cbctx, start);
    if (pinned)
        o->flags |= OF_PINNED;
    else
        o->flags &= (fat_u16)~OF_PINNED;
    if (start != o->start || n != o->clusters || frags != o->frags) {
        o->start = start;
        o->clusters = n;
        o->frags = frags;
        return V_REPLAN;
    }
    return pinned ? V_PINNED : V_OK;
}

/* Cluster k of the current chain once chain[k0 .. k0+len) lives at dst. */
static fat_u32 cl_at(const ctx_t *c, fat_u32 k, fat_u32 k0, fat_u32 len,
                     fat_u32 dst)
{
    return (k >= k0 && k - k0 < len) ? dst + (k - k0) : c->chain[k];
}

/* chain[0..n) still holds chain[k0 .. k0+len) in one run that starts at
 * cluster "at" and, if "whole", is a whole fragment on its own. */
static int piece_ok(const ctx_t *c, fat_u32 n, fat_u32 k0, fat_u32 len,
                    fat_u32 at, int whole)
{
    fat_u32 i;

    if (len == 0 || k0 + len > n)
        return 0;
    for (i = 0; i < len; i++)
        if (c->chain[k0 + i] != at + i)
            return 0;
    return !whole || ((k0 == 0 || c->chain[k0 - 1] + 1 != at) &&
                      (k0 + len == n || c->chain[k0 + len] != at + len));
}

enum { C_OK, C_CHANGED, C_SRCERR, C_DSTERR, C_BADDIR };

/* Copy chain[k0 .. k0+len) of object idx to the free clusters at dst. */
static int copy_obj(ctx_t *c, fat_u32 idx, fat_u32 k0, fat_u32 len,
                    fat_u32 dst)
{
    fat_volume *fv = c->fv;
    const fat_u32 *ch = c->chain + k0;
    fat_u32 i = 0, spc = fv->sectorsPerCluster;
    int isDir = (c->obj[idx].flags & OF_DIR) != 0;

    while (i < len) {
        fat_u32 run = 1, cnt;
        size_t bytes;

        while (i + run < len && run < c->bufClusters &&
               ch[i + run] == ch[i] + run)
            run++;
        cnt = run * spc;
        bytes = (size_t)run * fv->bytesPerCluster;
        if (!fat_read_sectors(fv, fat_cluster_sector(fv, ch[i]), cnt, c->buf))
            return C_SRCERR;
        if (isDir && k0 + i == 0) {
            /* A directory starts with "." (itself) and "..". */
            if (!is_dot_entry(c->buf, ".          ") ||
                !is_dot_entry(c->buf + 32, "..         "))
                return C_BADDIR;
            set_entry_start(fv, c->buf, dst);
        }
        if (!fat_write_sectors(fv, fat_cluster_sector(fv, dst + i), cnt,
                               c->buf))
            return C_DSTERR;
        if (c->cfg->verify &&
            (!fat_read_sectors(fv, fat_cluster_sector(fv, dst + i), cnt,
                               c->vbuf) ||
             memcmp(c->buf, c->vbuf, bytes) != 0))
            return C_DSTERR;
        i += run;
        c->secBytes += bytes;
        /* Big files are copied across several short sections; if the volume
         * changed in between, the copy starts over. */
        if (i < len && !isDir && c->cfg->begin_write &&
            c->secBytes >= SEC_BYTES) {
            int w, tries = 0;

            sec_leave(c);
            /* "Busy" only means "not yet": what was copied stays valid as
             * long as the next section reports no change. */
            do
                w = sec_enter(c);
            while (w == FAT_WR_BUSY && !c->stop && ++tries < MAX_BUSY);
            if (w != FAT_WR_OK)
                return C_CHANGED;
        }
    }
    return C_OK;
}

/* Read-only first pass of fix_children: every subdirectory of the directory
 * (c->chain[0..n), chain[k0 .. k0+len) copied to dst) can be read and has a
 * ".." that leads to "old". Run before the switch, so that a subdirectory
 * that cannot be fixed afterwards stops the move while nothing points at the
 * copy yet. Otherwise the old first clusters would have to be kept for the
 * stale ".." entries, and in a folder moved in pieces they still lead on to
 * the live rest of the folder. Returns 0 if one would fail. */
static int probe_children(ctx_t *c, fat_u32 n, fat_u32 k0, fat_u32 len,
                          fat_u32 dst, fat_u32 old)
{
    fat_volume *fv = c->fv;
    fat_u32 eps = fv->bytesPerSector / 32, k, s, i;

    for (k = 0; k < n; k++) {
        fat_u32 first = fat_cluster_sector(fv, cl_at(c, k, k0, len, dst));

        for (s = 0; s < fv->sectorsPerCluster; s++) {
            if (!fat_read_sectors(fv, first + s, 1, c->sec))
                return 0;
            for (i = 0; i < eps; i++) {
                const fat_u8 *e = c->sec + i * 32;
                fat_u32 cs;

                if (e[0] == 0x00)
                    return 1;
                if (!is_object_entry(e) || !(e[0x0B] & 0x10))
                    continue;
                cs = entry_start(fv, e);
                if (!fat_cluster_valid(fv, cs))
                    continue;
                if (!fat_read_sectors(fv, fat_cluster_sector(fv, cs), 1,
                                      c->sec2) ||
                    !is_dot_entry(c->sec2 + 32, "..         ") ||
                    entry_start(fv, c->sec2 + 32) != old)
                    return 0;
            }
        }
    }
    return 1;
}

/* Point the ".." of every subdirectory of a directory whose first cluster
 * moved from "old" to dst at its new place. The directory's chain is
 * c->chain[0..n) with chain[k0 .. k0+len) now at dst. Returns 0 if one could
 * not be updated. */
static int fix_children(ctx_t *c, fat_u32 n, fat_u32 k0, fat_u32 len,
                        fat_u32 dst, fat_u32 old)
{
    fat_volume *fv = c->fv;
    fat_u32 eps = fv->bytesPerSector / 32, k, s, i;
    int ok = 1;

    for (k = 0; k < n; k++) {
        fat_u32 first = fat_cluster_sector(fv, cl_at(c, k, k0, len, dst));

        for (s = 0; s < fv->sectorsPerCluster; s++) {
            if (!fat_read_sectors(fv, first + s, 1, c->sec))
                return 0;
            for (i = 0; i < eps; i++) {
                const fat_u8 *e = c->sec + i * 32;
                fat_u32 cs, sector;

                if (e[0] == 0x00)
                    return ok;
                if (!is_object_entry(e) || !(e[0x0B] & 0x10))
                    continue;
                cs = entry_start(fv, e);
                if (!fat_cluster_valid(fv, cs))
                    continue;
                sector = fat_cluster_sector(fv, cs);
                if (!fat_read_sectors(fv, sector, 1, c->sec2)) {
                    ok = 0;
                    continue;
                }
                if (!is_dot_entry(c->sec2 + 32, "..         ") ||
                    entry_start(fv, c->sec2 + 32) != old)
                    return 0;
                set_entry_start(fv, c->sec2 + 32, dst);
                if (!fat_write_verified(fv, sector, 1, c->sec2))
                    ok = 0;
            }
        }
    }
    return ok;
}

/* Free a just-published chain again after the move could not complete. */
static void unpublish(ctx_t *c, fat_u32 dst, fat_u32 n)
{
    fat_u32 i;

    for (i = 0; i < n; i++)
        fat_set(c->fv, dst + i, 0);
    fat_flush(c->fv);
}

/*
 * Switch chain[k0 .. k0+len) of object idx (c->chain) over to its copy at
 * dst: publish the copy's chain in the FAT, point whatever led to the old
 * clusters at it (the directory entry, or the FAT entry of the cluster
 * before), fix the ".." of the subdirectories when a directory's first
 * cluster moved, then free the old clusters. A failed write can leave
 * lost clusters, FAT mirror differences or stale ".." entries
 * when the device preserves sector writes and their ordering.
 * When analyzing only the in-memory FAT is changed, so
 * that it shows the planned layout. Returns 0 on an I/O error.
 */
static int commit_move(ctx_t *c, fat_u32 idx, fat_u32 k0, fat_u32 len,
                       fat_u32 dst)
{
    fat_volume *fv = c->fv;
    fobj *o = &c->obj[idx];
    fat_u32 n = o->clusters, end = k0 + len, old = c->chain[0];
    fat_u32 next = end < n ? c->chain[end] : fat_eoc_value(fv);
    fat_u32 i, frags = 1, sector, off;
    int live = !c->sim;
    fat_u8 *e;

    /* Nothing is published yet, so a subdirectory whose ".." could not be
     * fixed only costs this move. */
    if (live && k0 == 0 && (o->flags & OF_DIR) &&
        !probe_children(c, n, k0, len, dst, old))
        return 0;
    /* The free-space count in FSInfo goes stale with the first FAT change,
     * so it is cleared before that change rather than at the end of the run:
     * a run that stops early, or whose first switch fails after its chain
     * was published, would otherwise leave Windows trusting a wrong count.
     * This runs inside the section, like every other write. */
    if (live && !c->fsinfoCleared) {
        if (!fat_fsinfo_invalidate(fv))
            return 0;
        c->fsinfoCleared = 1;
    }
    for (i = 0; i < len; i++)
        fat_set(fv, dst + i, i + 1 < len ? dst + i + 1 : next);
    if (live && !fat_flush(fv))
        goto undo;
    if (k0 > 0) {
        fat_set(fv, c->chain[k0 - 1], dst);
        /* The old clusters lead on to the same cluster as the copy, so the
         * file is whole whichever version of this entry reached the disk. */
        if (live && !fat_flush(fv)) {
            c->extStale = 1;
            return 0;
        }
    } else if (live) {
        if (!locate(c, idx, &sector, &off) ||
            !fat_read_sectors(fv, sector, 1, c->sec))
            goto undo;
        e = c->sec + off;
        if (memcmp(e, o->name, 11) != 0 || entry_start(fv, e) != old)
            goto undo;
        set_entry_start(fv, e, dst);
        if (!fat_write_verified(fv, sector, 1, c->sec)) {
            /* Undo only if the entry certainly still has the old start. */
            if (fat_read_sectors(fv, sector, 1, c->sec) &&
                entry_start(fv, c->sec + off) == old)
                goto undo;
            c->extStale = 1;
            return 0;
        }
    }

    /* The clusters now live at dst. */
    ext_take(c, dst, len);
    for (i = 0; i + 1 < n; i++)
        if (cl_at(c, i + 1, k0, len, dst) != cl_at(c, i, k0, len, dst) + 1)
            frags++;
    o->start = cl_at(c, 0, k0, len, dst);
    o->frags = frags;
    o->flags |= OF_MOVED;
    if (live) {
        c->st->clustersMoved += len;
        c->secMoves++;
        if (k0 == 0 && (o->flags & OF_DIR) &&
            !fix_children(c, n, k0, len, dst, old)) {
            /* The directory's own data and its entry are already consistent
             * at dst; only some ".." entries still point to the old copy
             * (probe_children passed, so a write failed). Keeping the old
             * clusters allocated is what makes those ".." entries keep
             * resolving, so the run can go on instead of stopping over a
             * failed fixup. The old clusters stay lost (they are reported
             * as lost clusters when offline). When only the first piece
             * moved, the last old cluster still leads on to the live rest
             * of the folder: end the kept copy there, so that it is a chain
             * of its own that nothing frees or reuses with the folder. */
            c->extStale = 1;
            c->st->dotdotErrors++;
            c->st->ioErrors++;
            emit(c, idx, FAT_EV_DOTDOT, o->frags, o->frags);
            if (end < n) {
                fat_set(fv, c->chain[end - 1], fat_eoc_value(fv));
                if (!fat_flush(fv)) {
                    /* The folder itself is consistent at dst; only the kept
                     * copy may still run into it. Write nothing more. */
                    c->st->ioErrors++;
                    stop_run(c);
                }
            }
            for (i = k0; i < end; i++)
                c->chain[i] = dst + (i - k0);
            return 1;
        }
    }
    for (i = k0; i < end; i++)
        fat_set(fv, c->chain[i], 0);
    if (live && !fat_flush(fv)) {
        c->extStale = 1;
        return 0;
    }
    for (i = k0; i < end;) {
        fat_u32 run = 1;

        while (i + run < end && c->chain[i + run] == c->chain[i] + run)
            run++;
        ext_give(c, c->chain[i], run);
        i += run;
    }
    for (i = k0; i < end; i++)
        c->chain[i] = dst + (i - k0);
    return 1;
undo:
    unpublish(c, dst, len);
    return 0;
}

static void fail_io(ctx_t *c, fat_u32 idx, int fatal)
{
    fobj *o = &c->obj[idx];

    o->flags |= OF_IOERR;
    c->st->ioErrors++;
    emit(c, idx, FAT_EV_IOERROR, o->frags, o->frags);
    if (fatal)
        stop_run(c);
}

enum { M_DEFRAG, M_PLACE, M_SLIDE };
enum { R_OK, R_NOSPACE, R_SKIP };

/*
 * Move clusters of object idx into one contiguous free run. M_DEFRAG moves a
 * whole fragmented object anywhere (k0, len, at and to are ignored). M_PLACE
 * and M_SLIDE move chain[k0 .. k0+len), which starts at cluster "at", to the
 * free clusters at "to": M_PLACE a whole fragment (a contiguous object or one
 * piece of a fragmented one), M_SLIDE any part of one. If the clusters are no
 * longer there, or "to" is no longer free, nothing is moved.
 */
static int relocate(ctx_t *c, fat_u32 idx, int mode, fat_u32 k0, fat_u32 len,
                    fat_u32 at, fat_u32 to)
{
    fobj *o = &c->obj[idx];
    int attempt;

    for (attempt = 0; attempt < MAX_ATTEMPTS && !c->stop; attempt++) {
        fat_u32 dst, lo = 2, hi = c->lastCluster + 1, n = 0;
        int w;

        if (!movable(o) && !(root_tail(o) && mode != M_DEFRAG && k0 > 0))
            return R_SKIP;
        if (mode == M_DEFRAG) {
            if (o->frags < 2)
                return R_SKIP;
            k0 = 0;
            len = o->clusters;
        } else if (k0 + len > o->clusters) {
            return R_SKIP;
        } else {
            lo = to;
            hi = to + len;
        }

        /* Bookkeeping room must be allocated outside a write section. */
        if (c->nExt + o->frags + 2 > c->capExt || o->clusters > c->chainCap) {
            sec_leave(c);
            if (!ext_reserve(c, c->nExt + o->frags + 2 + EXT_SLACK) ||
                !chain_reserve(c, o->clusters))
                return R_SKIP;
        }
        if (c->extStale && !c->sim) {
            /* Rebuilt from the FAT, which is only current inside a section. */
            if (!c->inSec) {
                if (sec_enter(c) == FAT_WR_FAIL)
                    return R_SKIP;
                continue;
            }
            if (!ext_build(c)) {
                sec_leave(c);
                if (!ext_reserve(c, c->extNeed + EXT_SLACK))
                    return R_SKIP;
                continue;
            }
        }

        dst = ext_first_fit(c, len, lo, hi);
        if (!dst)
            return R_NOSPACE;
        if (c->sim) {
            /* The in-memory FAT holds the layout planned so far. */
            if (build_chain(c, o->start, &n) != 1 || n != o->clusters ||
                (mode != M_DEFRAG &&
                 !piece_ok(c, n, k0, len, at, mode == M_PLACE)))
                return R_SKIP;
            commit_move(c, idx, k0, len, dst);
            return R_OK;
        }
        if (!c->inSec) {
            w = sec_enter(c);
            if (w == FAT_WR_FAIL)
                return R_SKIP;
            if (w != FAT_WR_OK)
                continue; /* busy: try again; changed: plan again */
        }
        switch (validate(c, idx)) {
        case V_OK:
            break;
        case V_REPLAN:
            if (mode != M_DEFRAG)
                return R_SKIP; /* compaction finds it again */
            continue;
        case V_GROW:
            sec_leave(c);
            if (!chain_reserve(c, c->chainNeed))
                return R_SKIP;
            continue;
        case V_PINNED:
            return R_SKIP;
        case V_GONE:
            o->flags |= OF_GONE;
            return R_SKIP;
        default:
            fail_io(c, idx, 0);
            return R_SKIP;
        }
        if (mode != M_DEFRAG &&
            !piece_ok(c, o->clusters, k0, len, at, mode == M_PLACE))
            return R_SKIP;
        if (!range_free(c, dst, len)) {
            c->extStale = 1;
            continue;
        }
        switch (copy_obj(c, idx, k0, len, dst)) {
        case C_OK:
            break;
        case C_CHANGED:
            if (c->stop)
                return R_SKIP;
            if (++o->retries > MAX_RETRIES)
                goto busy;
            continue;
        case C_BADDIR:
            o->flags |= OF_BAD;
            c->st->chainErrors++;
            emit(c, idx, FAT_EV_CHAINERROR, o->frags, o->frags);
            return R_SKIP;
        case C_SRCERR:
            fail_io(c, idx, 0);
            return R_SKIP;
        default: /* writing failed: stop before more damage is done */
            fail_io(c, idx, 1);
            return R_SKIP;
        }
        if (!commit_move(c, idx, k0, len, dst)) {
            fail_io(c, idx, 1);
            return R_SKIP;
        }
        return R_OK;
    }
    if (c->stop)
        return R_SKIP;
busy:
    o->flags |= OF_BUSY;
    emit(c, idx, FAT_EV_BUSY, o->frags, o->frags);
    return R_SKIP;
}

/* ----------------------------------------------------------------- phases */

/* Phase 1: make every fragmented file and directory contiguous. Returns the
 * number of objects fixed. */
static fat_u32 phase_defrag(ctx_t *c)
{
    fat_u32 i, moved = 0;

    c->phase = FAT_PH_DEFRAG;
    c->phaseDone = c->phaseTotal = 0;
    for (i = 1; i < c->nObj; i++)
        if (c->obj[i].frags > 1 &&
            !(c->obj[i].flags & (OF_BAD | OF_CROSS | OF_GONE)))
            c->phaseTotal++;
    progress(c);
    for (i = 1; i < c->nObj && !c->stop; i++) {
        fobj *o = &c->obj[i];
        fat_u32 frags = o->frags;

        if (frags < 2 || (o->flags & (OF_BAD | OF_CROSS | OF_GONE)))
            continue;
        sec_check(c);
        c->phaseDone++;
        progress(c);
        if (movable(o)) {
            int r = relocate(c, i, M_DEFRAG, 0, 0, 0, 0);

            if (r == R_OK) {
                o->flags = (fat_u16)((o->flags | OF_DEFRAGGED) & ~OF_NOSPACE);
                moved++;
                emit(c, i, (o->flags & OF_DIR) ? FAT_EV_DIRDEFRAGGED
                                               : FAT_EV_DEFRAGGED,
                     frags, 1);
                continue;
            }
            if (r == R_NOSPACE) {
                if (!(o->flags & OF_NOSPACE))
                    emit(c, i, FAT_EV_NOSPACE, frags, frags);
                o->flags |= OF_NOSPACE;
                continue;
            }
        }
    }
    sec_check(c);
    return moved;
}

static int add_unit(ctx_t *c, fat_u32 idx, fat_u32 k0, fat_u32 start,
                    fat_u32 len)
{
    funit *u;

    if (c->nUnit == c->capUnit) {
        fat_u32 nc = c->capUnit ? c->capUnit * 2 : 1024;
        funit *p = (funit *)realloc(c->unit, (size_t)nc * sizeof(funit));

        if (!p) {
            c->oom = c->stop = 1;
            return 0;
        }
        c->unit = p;
        c->capUnit = nc;
    }
    u = &c->unit[c->nUnit++];
    u->idx = idx;
    u->k0 = k0;
    u->start = start;
    u->len = len;
    return 1;
}

/*
 * Outside a section: list what compaction may move, sorted by position:
 * contiguous movable objects, each fragment of the movable objects that are
 * still in pieces (they did not fit anywhere and would otherwise split the
 * free space for good), and the fragments of the FAT32 root folder after its
 * first. Also order them by length for the sweep, all still to be placed.
 */
static int build_units(ctx_t *c)
{
    fat_u32 k, i, n, h = 1;

    c->nUnit = 0;
    for (k = 0; k < c->nObj; k++) {
        const fobj *o = &c->obj[k];
        int whole = movable(o);

        if (!whole && !root_tail(o))
            continue;
        if (o->frags == 1) {
            if (whole && !add_unit(c, k, 0, o->start, o->clusters))
                return 0;
            continue;
        }
        if (build_chain(c, o->start, &n) != 1 || n != o->clusters) {
            if (c->oom)
                return 0;
            continue; /* out of date: checked when moved */
        }
        for (i = 0; i < n;) {
            fat_u32 run = 1;

            while (i + run < n && c->chain[i + run] == c->chain[i] + run)
                run++;
            if ((whole || i > 0) && !add_unit(c, k, i, c->chain[i], run))
                return 0;
            i += run;
        }
    }
    n = c->nUnit;
    while (h < n / 3)
        h = 3 * h + 1;
    for (; h > 0; h /= 3) {
        for (i = h; i < n; i++) {
            funit t = c->unit[i];

            for (k = i; k >= h && c->unit[k - h].start > t.start; k -= h)
                c->unit[k] = c->unit[k - h];
            c->unit[k] = t;
        }
    }
    for (i = n; i-- > 0;) {
        fat_u32 end = c->unit[i].start + c->unit[i].len;

        c->unit[i].reach = (i + 1 < n && c->unit[i + 1].start == end)
                               ? c->unit[i + 1].reach : end;
    }

    if (c->capUnit > c->capSize) {
        size_t cap = c->capUnit;
        fat_u32 *bs = (fat_u32 *)realloc(c->bySize, cap * sizeof(fat_u32));
        fat_u32 *rk = NULL;
        long *td = NULL;

        if (bs) {
            c->bySize = bs;
            rk = (fat_u32 *)realloc(c->rank, cap * sizeof(fat_u32));
        }
        if (rk) {
            c->rank = rk;
            td = (long *)realloc(c->todo, cap * sizeof(long));
        }
        if (!td) {
            c->oom = c->stop = 1;
            return 0;
        }
        c->todo = td;
        c->capSize = c->capUnit;
    }
    for (i = 0; i < n; i++)
        c->bySize[i] = i;
    for (h = 1; h < n / 3;)
        h = 3 * h + 1;
    for (; h > 0; h /= 3) {
        for (i = h; i < n; i++) {
            fat_u32 t = c->bySize[i];
            const funit *ut = &c->unit[t];

            for (k = i; k >= h; k -= h) {
                const funit *uk = &c->unit[c->bySize[k - h]];

                if (uk->len < ut->len ||
                    (uk->len == ut->len && uk->start < ut->start))
                    break;
                c->bySize[k] = c->bySize[k - h];
            }
            c->bySize[k] = t;
        }
    }
    for (i = 0; i < n; i++) {
        c->rank[c->bySize[i]] = i;
        c->todo[i] = (long)i;
    }
    c->nOpen = n;
    return 1;
}

/* The unit that starts at cluster cl, or -1. */
static long unit_at(const ctx_t *c, fat_u32 cl)
{
    fat_u32 lo = 0, hi = c->nUnit;

    while (lo < hi) {
        fat_u32 mid = lo + (hi - lo) / 2;

        if (c->unit[mid].start < cl)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < c->nUnit && c->unit[lo].start == cl) ? (long)lo : -1;
}

static int unit_open(const ctx_t *c, fat_u32 u)
{
    return c->todo[c->rank[u]] == (long)c->rank[u];
}

/* The sweep has passed or moved unit u: it is not moved again. */
static void unit_close(ctx_t *c, fat_u32 u)
{
    long p = (long)c->rank[u];

    if (c->todo[p] == p) {
        c->todo[p] = p - 1;
        c->nOpen--;
    }
}

/* The open unit with the greatest length up to "room" (the highest one of
 * equal length), or -1. */
static long best_fit(ctx_t *c, fat_u32 room)
{
    long lo = 0, hi = (long)c->nUnit, p, r;

    while (lo < hi) {
        long mid = lo + (hi - lo) / 2;

        if (c->unit[c->bySize[mid]].len <= room)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (r = lo - 1; r >= 0 && c->todo[r] != r;)
        r = c->todo[r];
    for (p = lo - 1; p > r;) { /* shorten the way for the next search */
        long next = c->todo[p];

        c->todo[p] = r;
        p = next;
    }
    return r < 0 ? -1 : (long)c->bySize[r];
}

/*
 * Move unit u to the free clusters at "to". When they lie below it and
 * overlap its place, it slides there in steps of the distance. Returns the
 * clusters moved.
 */
static fat_u32 place_unit(ctx_t *c, fat_u32 u, fat_u32 to)
{
    funit x = c->unit[u];
    fobj *o = &c->obj[x.idx];
    fat_u32 frags = o->frags, done = 0, step = x.start - to;

    if (to + x.len <= x.start || to >= x.start + x.len) {
        if (relocate(c, x.idx, M_PLACE, x.k0, x.len, x.start, to) == R_OK)
            done = x.len;
    } else if (to < x.start) {
        while (done < x.len && !c->stop) {
            fat_u32 n = x.len - done < step ? x.len - done : step;

            sec_check(c);
            if (relocate(c, x.idx, M_SLIDE, x.k0 + done, n, x.start + done,
                         to + done) != R_OK)
                break;
            done += n;
        }
    }
    if (!done)
        return 0;
    o->flags |= OF_COMPACTED;
    c->phaseDone++;
    emit(c, x.idx, FAT_EV_MOVED, done, o->frags);
    if (frags > 1 && o->frags == 1) {
        /* Its fragments came together. */
        o->flags = (fat_u16)((o->flags | OF_DEFRAGGED) & ~OF_NOSPACE);
        emit(c, x.idx, (o->flags & OF_DIR) ? FAT_EV_DIRDEFRAGGED
                                           : FAT_EV_DEFRAGGED,
             frags, 1);
    }
    return done;
}

/* The start of the highest free run from cluster "from" on with room for
 * len clusters, or 0. */
static fat_u32 top_fit(const ctx_t *c, fat_u32 len, fat_u32 from)
{
    fat_u32 i = c->nExt;

    while (i-- > 0 && c->ext[i].start >= from)
        if (c->ext[i].len >= len)
            return c->ext[i].start;
    return 0;
}

/*
 * One sweep up the volume. At each gap it moves in the rest of the file that
 * ends right before the gap if that fits, or else the longest unit from
 * above that fits. When none does, and the units that follow the gap reach
 * the next free run, it slides them down one by one, so that the gap moves
 * up and joins that run. A unit that would take many small steps is moved
 * up out of the way instead (only if "trip"); a later sweep brings it back
 * down whole. A gap in front of something unmovable stays when nothing
 * fits. Every unit moves at most once. Returns the number of moves.
 */
static fat_u32 sweep(ctx_t *c, int trip)
{
    fat_u32 moved = 0, cur = 2, low = 0;

    while (!c->stop && c->nOpen) {
        fat_u32 w, e, gap, done;
        int i;
        long u = -1;

        sec_check(c);
        i = ext_find(c, cur);
        if (i < 0 || c->ext[i].start + c->ext[i].len <= cur)
            i++;
        if (i >= (int)c->nExt)
            break;
        w = c->ext[i].start > cur ? c->ext[i].start : cur;
        e = c->ext[i].start + c->ext[i].len;
        gap = e - w;
        /* What lies below the gap stays where it is. */
        while (low < c->nUnit && c->unit[low].start < w)
            unit_close(c, low++);
        if (!c->nOpen)
            break;
        if (w > 2) {
            fat_u32 v = fat_get(c->fv, w - 1);

            if (v >= e && v <= c->lastCluster) {
                u = unit_at(c, v);
                if (u >= 0 && (!unit_open(c, (fat_u32)u) ||
                               c->unit[u].len > gap))
                    u = -1;
            }
        }
        if (u < 0)
            u = best_fit(c, gap);
        if (u >= 0) {
            unit_close(c, (fat_u32)u);
            if (place_unit(c, (fat_u32)u, w))
                moved++;
            continue;
        }
        u = unit_at(c, e);
        if (u < 0 || !unit_open(c, (fat_u32)u) || i + 1 >= (int)c->nExt ||
            c->unit[u].reach < c->ext[i + 1].start) {
            cur = e; /* leave the gap */
            continue;
        }
        unit_close(c, (fat_u32)u);
        if (trip && (c->unit[u].len - 1) / gap >= SLIDE_STEPS) {
            fat_u32 to = top_fit(c, c->unit[u].len, e + c->unit[u].len);

            if (to && place_unit(c, (fat_u32)u, to)) {
                moved++;
                continue; /* its place joined the gap */
            }
        }
        done = place_unit(c, (fat_u32)u, w);
        if (done)
            moved++;
        cur = done ? w + done : e;
    }
    return moved;
}

/* Phase 2: close the gaps between the files and folders, sweep after sweep,
 * until the free space forms as few runs as the unmovable objects allow. */
static void phase_compact(ctx_t *c)
{
    fat_u32 pass;

    sec_leave(c);
    c->phase = FAT_PH_COMPACT;
    c->phaseDone = c->phaseTotal = 0;
    progress(c);
    for (pass = 0; pass < MAX_PASSES && !c->stop; pass++) {
        sec_leave(c);
        if (!build_units(c) || !sweep(c, pass < TRIP_PASSES))
            break;
    }
    sec_leave(c);
}

static int any_nospace(const ctx_t *c)
{
    fat_u32 i;

    for (i = 1; i < c->nObj; i++)
        if ((c->obj[i].flags & OF_NOSPACE) && c->obj[i].frags > 1 &&
            movable(&c->obj[i]))
            return 1;
    return 0;
}

static void work(ctx_t *c)
{
    int round;

    phase_defrag(c);
    if (!c->cfg->compact || c->stop)
        return;
    /* Consolidating makes room for objects that did not fit, and moving
     * those leaves gaps to consolidate in turn. */
    for (round = 0; round < MAX_ROUNDS && !c->stop; round++) {
        phase_compact(c);
        if (c->stop || !any_nospace(c) || !phase_defrag(c))
            break;
    }
}

/* ------------------------------------------------------------------- scan */

static fobj *add_obj(ctx_t *c, fat_u32 *idx)
{
    if (c->nObj == c->capObj) {
        fat_u32 nc;
        fobj *p;

        if (c->capObj > 0x7FFFFFFFu / sizeof(fobj)) {
            c->oom = c->stop = 1;
            return NULL;
        }
        nc = c->capObj ? c->capObj * 2 : 1024;
        p = (fobj *)realloc(c->obj, (size_t)nc * sizeof(fobj));

        if (!p) {
            c->oom = c->stop = 1;
            return NULL;
        }
        c->obj = p;
        c->capObj = nc;
    }
    *idx = c->nObj++;
    memset(&c->obj[*idx], 0, sizeof(fobj));
    return &c->obj[*idx];
}

/* Mark the chain in c->chain as claimed; TRUE if another object had any of
 * its clusters already. */
static int mark_seen(ctx_t *c, fat_u32 n)
{
    fat_u32 i;
    int dup = 0;

    for (i = 0; i < n; i++) {
        fat_u32 cl = c->chain[i];
        fat_u8 bit = (fat_u8)(1u << (cl & 7));

        if (c->seen[cl >> 3] & bit)
            dup = 1;
        c->seen[cl >> 3] |= bit;
    }
    return dup;
}

/* Measure the chain of object idx (start already set). 0 when unusable. */
static int scan_chain(ctx_t *c, fat_u32 idx)
{
    fat_u32 n = 0;
    int r = build_chain(c, c->obj[idx].start, &n);
    fobj *o = &c->obj[idx];

    if (r < 0)
        return 0;
    if (r == 0) {
        o->flags |= OF_BAD;
        c->st->chainErrors++;
        emit(c, idx, FAT_EV_CHAINERROR, 0, 0);
        return 0;
    }
    o->clusters = n;
    o->frags = count_frag(c->chain, n);
    if (mark_seen(c, n)) {
        o->flags |= OF_CROSS;
        c->st->crossLinked++;
        emit(c, idx, FAT_EV_CROSSLINKED, o->frags, o->frags);
    }
    return 1;
}

static void scan_dir(ctx_t *c, fat_u32 dirIdx, int depth);

/* Hidden system files in the root folder that MS-DOS, Windows and their
 * setup and uninstall programs open by name only: logs, backups, uninstall
 * data, the DriveSpace drivers, boot menus and settings, and the Windows
 * swap file. They move. */
static const char root_movable[][12] = {
    "BOOTLOG TXT", "BOOTLOG PRV", "DETLOG  TXT", "DETLOG  OLD", "DETCRASHLOG",
    "SETUPLOGTXT", "SETUPLOGOLD", "SETUPXLGTXT", "NETLOG  TXT", "FRUNLOG TXT",
    "ASD     LOG", "SCANDISKLOG", "SUHDLOG DAT", "SUHDLOG BAK", "SYSTEM  1ST",
    "MSDOS   ---", "BOOTSECTDOS", "CONFIG  WOS", "AUTOEXECWOS", "W95UNDO DAT",
    "W95UNDO INI", "W98UNDO DAT", "W98UNDO INI", "DRVSPACEBIN", "DBLSPACEBIN",
    "DRVSPACEINI", "DBLSPACEINI", "WINBOOT INI", "BOOT    INI", "CHKLIST MS ",
    "FFASTUN FFA", "FFASTUN FFL", "FFASTUN FFO", "FFASTUN0FFX", "WIN386  SWP"};

/* The MSDOS.SYS of Windows 95/98 is a short text file that IO.SYS opens by
 * name; that of MS-DOS 6 and earlier is the DOS kernel, which boot code
 * finds by position. */
static int text_msdos_sys(ctx_t *c, fat_u32 idx, const fat_u8 *e)
{
    fat_volume *fv = c->fv;
    fat_u32 size = rd32le(e + 0x1C), start = entry_start(fv, e), n, i;

    if (size == 0 || size > 16384 || !fat_cluster_valid(fv, start))
        return 0;
    if (!fat_read_sectors(fv, fat_cluster_sector(fv, start), 1, c->sec2)) {
        fail_io(c, idx, 0);
        return 0;
    }
    n = size < fv->bytesPerSector ? size : fv->bytesPerSector;
    for (i = 0; i < n; i++) {
        fat_u8 b = c->sec2[i];

        if ((b < 0x20 && b != '\t' && b != '\r' && b != '\n' && b != 0x1A) ||
            b == 0x7F)
            return 0;
    }
    return 1;
}

/*
 * The files that stay where they are, whatever their size:
 *   - known root boot loaders and a binary MSDOS.SYS, even without hidden
 *     or system attributes;
 *   - hidden system files in the root folder, except those root_movable
 *     lists and a Windows 95/98 MSDOS.SYS: the boot files of MS-DOS, Windows
 *     and other systems (IO.SYS, an MS-DOS 6 MSDOS.SYS, NTLDR), compressed
 *     volume files (DRVSPACE.nnn, DBLSPACE.nnn, STACVOL.DSK), the swap or
 *     hibernation files that other systems or the BIOS find by position, and
 *     any other file not known to be safe to move;
 *   - LDLINUX.SYS, wherever it is: the SYSLINUX boot sector lists its
 *     sectors;
 *   - while Windows runs (!cfg->offline), its swap file and its registry,
 *     which the system keeps open below the level the volume locks cover.
 * Hidden, system and read-only files elsewhere move like any other file.
 */
static int is_system(ctx_t *c, fat_u32 dirIdx, fat_u32 idx, const fat_u8 *e)
{
    static const char live[4][12] = {"WIN386  SWP", "SYSTEM  DAT",
                                     "USER    DAT", "CLASSES DAT"};
    static const char boot[][12] = {"IO      SYS", "WINBOOT SYS",
                                    "IBMBIO  COM", "IBMDOS  COM",
                                    "NTLDR      "};
    fat_u32 i;

    if (memcmp(e, "LDLINUX SYS", 11) == 0)
        return 1;
    for (i = 0; i < 4 && !c->cfg->offline; i++)
        if (memcmp(e, live[i], 11) == 0)
            return 1;
    if (dirIdx == 0) {
        for (i = 0; i < sizeof(boot) / sizeof(boot[0]); i++)
            if (memcmp(e, boot[i], 11) == 0)
                return 1;
        if (memcmp(e, "MSDOS   SYS", 11) == 0)
            return !text_msdos_sys(c, idx, e);
    }
    if (dirIdx != 0 || (e[0x0B] & 0x06) != 0x06)
        return 0;
    for (i = 0; i < sizeof(root_movable) / sizeof(root_movable[0]); i++)
        if (memcmp(e, root_movable[i], 11) == 0)
            return 0;
    return 1;
}

static void scan_entry(ctx_t *c, fat_u32 dirIdx, fat_u32 slot,
                       const fat_u8 *e, int depth)
{
    fat_stats *st = c->st;
    int analyze = !c->cfg->doMove;
    fat_u32 idx;
    fobj *o = add_obj(c, &idx);

    if (!o)
        return;
    memcpy(o->name, e, 11);
    o->parent = dirIdx;
    o->entry = slot;
    o->start = entry_start(c->fv, e);
    c->phaseDone++;
    progress(c);
    if (e[0x0B] & 0x10) {
        o->flags = OF_DIR;
        st->dirsExamined++;
        if (!scan_chain(c, idx))
            return;
        o = &c->obj[idx];
        if (o->frags > 1) {
            st->dirsFragmented++;
            if (analyze)
                emit(c, idx, FAT_EV_FRAGMENTED, o->frags, o->clusters);
        }
        if (!(o->flags & OF_CROSS)) {
            if (depth + 1 < MAX_DEPTH) {
                scan_dir(c, idx, depth + 1);
            } else {
                o->flags |= OF_BAD;
                st->chainErrors++;
                emit(c, idx, FAT_EV_CHAINERROR, o->frags, o->frags);
            }
        }
        return;
    }
    st->filesExamined++;
    if (is_system(c, dirIdx, idx, e))
        o->flags |= OF_SYSTEM;
    if (o->start == 0) { /* empty file */
        if (rd32le(e + 0x1C) != 0) {
            o->flags |= OF_BAD;
            st->chainErrors++;
            emit(c, idx, FAT_EV_CHAINERROR, 0, 0);
            return;
        }
        st->contiguous++;
        return;
    }
    if (!scan_chain(c, idx))
        return;
    o = &c->obj[idx];
    if (((fat_u64)rd32le(e + 0x1C) + c->fv->bytesPerCluster - 1) /
        c->fv->bytesPerCluster != o->clusters) {
        o->flags |= OF_BAD;
        st->chainErrors++;
        emit(c, idx, FAT_EV_CHAINERROR, o->frags, o->frags);
        return;
    }
    if (o->frags > 1) {
        st->fragmented++;
        st->totalFragments += o->frags;
        if (analyze)
            emit(c, idx, FAT_EV_FRAGMENTED, o->frags, o->clusters);
    } else {
        st->contiguous++;
        if (analyze)
            emit(c, idx, FAT_EV_CONTIGUOUS, 1, o->clusters);
    }
}

static void scan_dir(ctx_t *c, fat_u32 dirIdx, int depth)
{
    fat_volume *fv = c->fv;
    fat_u32 eps = fv->bytesPerSector / 32, slot = 0, k = 0, inCl = 0;
    fat_u32 guard = 0, cl = c->obj[dirIdx].start, i;
    int fixed = (c->obj[dirIdx].flags & OF_FIXEDROOT) != 0;
    fat_u8 *buf = (fat_u8 *)malloc(fv->bytesPerSector);

    if (!buf) {
        c->oom = c->stop = 1;
        return;
    }
    while (!c->stop) {
        fat_u32 sector;

        if (fixed) {
            if (k >= fv->rootDirSectors)
                break;
            sector = fv->firstRootDirSector + k++;
        } else {
            if (inCl == fv->sectorsPerCluster) {
                fat_u32 nx = fat_get(fv, cl);

                if (fat_is_eoc(fv, nx) || !fat_cluster_valid(fv, nx) ||
                    ++guard > fv->countOfClusters)
                    break;
                cl = nx;
                inCl = 0;
            }
            sector = fat_cluster_sector(fv, cl) + inCl++;
        }
        if (!fat_read_sectors(fv, sector, 1, buf)) {
            c->st->ioErrors++;
            break;
        }
        if (dirIdx && slot == 0) {
            const fobj *o = &c->obj[dirIdx];
            fat_u32 parent = o->parent ? c->obj[o->parent].start : 0;

            if (!is_dot_entry(buf, ".          ") ||
                entry_start(fv, buf) != o->start ||
                !is_dot_entry(buf + 32, "..         ") ||
                entry_start(fv, buf + 32) != parent) {
                c->st->chainErrors++;
                emit(c, dirIdx, FAT_EV_CHAINERROR, o->frags, o->frags);
                break;
            }
        }
        for (i = 0; i < eps && !c->stop; i++, slot++) {
            const fat_u8 *e = buf + i * 32;

            if (e[0] == 0x00)
                goto done;
            if (is_object_entry(e))
                scan_entry(c, dirIdx, slot, e, depth);
        }
    }
done:
    free(buf);
}

static void scan(ctx_t *c)
{
    fat_volume *fv = c->fv;
    fat_u32 idx;
    fobj *root;

    c->phase = FAT_PH_SCAN;
    c->phaseDone = c->phaseTotal = 0;
    root = add_obj(c, &idx);
    if (!root)
        return;
    root->flags = OF_ROOT | OF_DIR;
    if (fv->fatType != FAT_TYPE_32) {
        root->flags |= OF_FIXEDROOT;
    } else {
        root->start = fv->rootCluster;
        if (!scan_chain(c, 0))
            return;
        if (c->obj[0].frags > 1) {
            c->st->dirsFragmented++;
            if (!c->cfg->doMove)
                emit(c, 0, FAT_EV_FRAGMENTED, c->obj[0].frags,
                     c->obj[0].clusters);
        }
    }
    progress(c);
    scan_dir(c, 0, 0);
}

/* ------------------------------------------------------------------ stats */

#define NO_OWNER 0xFFFFFFFFu

static int seen_bit(const ctx_t *c, fat_u32 cl)
{
    return (c->seen[cl >> 3] >> (cl & 7)) & 1;
}

/* Copy a path, keeping its end after "..." when it does not fit. */
static void note_path(char *dst, fat_u32 cap, const char *src)
{
    fat_u32 n = 0, i;

    while (src[n])
        n++;
    if (n >= cap) {
        dst[0] = dst[1] = dst[2] = '.';
        src += n - (cap - 4);
        for (i = 0; i < cap - 4; i++)
            dst[3 + i] = src[i];
        dst[cap - 1] = 0;
        return;
    }
    for (i = 0; i <= n; i++)
        dst[i] = src[i];
}

/*
 * Note the largest gaps left in the free space and what follows each one,
 * which is why it could not be closed. Works on the in-memory FAT and the
 * object table, which hold the final layout (analyze: the planned one).
 * Offline, also count the allocated clusters that no file or folder owns.
 */
static void note_gaps(ctx_t *c)
{
    fat_volume *fv = c->fv;
    fat_stats *st = c->st;
    fat_u32 at[FAT_GAP_NOTES], owner[FAT_GAP_NOTES];
    fat_u32 cl, last = 1, run = 0, i, j, k, n = 0;

    for (cl = c->lastCluster; cl >= 2; cl--) {
        if (!fat_is_free(fat_get(fv, cl))) {
            last = cl;
            break;
        }
    }
    for (cl = 2; cl <= last; cl++) {
        if (fat_is_free(fat_get(fv, cl))) {
            run++;
            continue;
        }
        if (!run)
            continue;
        st->gapsLeft++;
        for (k = n; k > 0 && st->gapNote[k - 1].clusters < run; k--)
            ;
        if (k < FAT_GAP_NOTES) {
            if (n < FAT_GAP_NOTES)
                n++;
            for (j = n - 1; j > k; j--) {
                st->gapNote[j] = st->gapNote[j - 1];
                at[j] = at[j - 1];
            }
            st->gapNote[k].clusters = run;
            at[k] = cl;
        }
        run = 0;
    }
    st->gapNotes = n;

    /* Who owns the cluster after each gap. */
    for (k = 0; k < n; k++)
        owner[k] = NO_OWNER;
    memset(c->seen, 0, c->lastCluster / 8 + 1);
    for (i = 0; i < c->nObj; i++) {
        if (c->obj[i].flags & OF_GONE)
            continue;
        for (cl = c->obj[i].start;
             fat_cluster_valid(fv, cl) && !seen_bit(c, cl);) {
            fat_u32 nx = fat_get(fv, cl);

            if (fat_is_free(nx) || fat_is_bad(fv, nx))
                break;
            c->seen[cl >> 3] |= (fat_u8)(1u << (cl & 7));
            for (k = 0; k < n; k++)
                if (at[k] == cl)
                    owner[k] = i;
            if (fat_is_eoc(fv, nx))
                break;
            cl = nx;
        }
    }
    for (k = 0; k < n; k++) {
        fat_gap_note *g = &st->gapNote[k];
        const fobj *o;

        g->path[0] = 0;
        if (owner[k] == NO_OWNER) {
            g->why = fat_is_bad(fv, fat_get(fv, at[k])) ? FAT_GAP_BAD
                                                        : FAT_GAP_LOST;
            continue;
        }
        o = &c->obj[owner[k]];
        g->why = owner[k] == 0             ? FAT_GAP_ROOT
               : (o->flags & OF_SYSTEM)    ? FAT_GAP_SYSTEM
               : (o->flags & OF_PINNED)    ? FAT_GAP_INUSE
                                           : FAT_GAP_STUCK;
        note_path(g->path, sizeof(g->path), obj_path(c, owner[k]));
    }

    if (!c->cfg->offline)
        return;
    for (cl = 2; cl <= c->lastCluster; cl++) {
        fat_u32 v = fat_get(fv, cl);

        if (!fat_is_free(v) && !fat_is_bad(fv, v) && !seen_bit(c, cl))
            st->lostClusters++;
    }
}

static void final_stats(ctx_t *c)
{
    fat_stats *st = c->st;
    fat_volume *fv = c->fv;
    int doMove = c->cfg->doMove;
    fat_u32 i, cl, last = 1;
    int run = 0;

    for (i = 0; i < c->nObj; i++) {
        const fobj *o = &c->obj[i];
        int fixed = (o->flags & (OF_SYSTEM | OF_PINNED)) != 0;

        if (i && fixed && o->clusters && !(o->flags & OF_GONE)) {
            int why = (o->flags & OF_SYSTEM) ? FAT_GAP_SYSTEM : FAT_GAP_INUSE;

            st->unmovable++;
            if (st->keptNotes < FAT_KEPT_NOTES) {
                fat_kept_note *k = &st->keptNote[st->keptNotes++];

                k->clusters = o->clusters;
                k->frags = o->frags;
                k->why = why;
                note_path(k->path, sizeof(k->path), obj_path(c, i));
            }
            if (doMove)
                emit(c, i, FAT_EV_UNMOVABLE, o->frags, why);
        }
        if ((fixed || i == 0) && o->frags > 1)
            st->unmovableFragmented++;
        if ((o->flags & OF_NOSPACE) && o->frags > 1)
            st->noSpace++;
        if (o->flags & OF_BUSY)
            st->busy++;
        if (o->flags & OF_DEFRAGGED) {
            if (!doMove)
                st->planDefrag++;
            else if (o->flags & OF_DIR)
                st->dirsDefragged++;
            else
                st->defragged++;
        } else if (o->flags & OF_COMPACTED) {
            if (doMove)
                st->compacted++;
            else
                st->compactable++;
        }
    }
    if (!doMove)
        st->planFreeRuns = c->nExt;

    fat_free_space(fv, &st->freeClusters, &st->freeRuns, &st->largestFreeRun);
    for (cl = c->lastCluster; cl >= 2; cl--) {
        if (!fat_is_free(fat_get(fv, cl))) {
            last = cl;
            break;
        }
    }
    for (cl = 2; cl < last; cl++) {
        if (fat_is_free(fat_get(fv, cl))) {
            if (!run)
                st->freeGaps++;
            run = 1;
            st->gapClusters++;
        } else {
            run = 0;
        }
    }
}

/* -------------------------------------------------------------------- run */

int fat_run(fat_volume *fv, const fat_defrag_cfg *cfg, fat_stats *st)
{
    ctx_t c;
    fat_u32 seenBytes;
    size_t bufBytes;
    int rescans;

    memset(&c, 0, sizeof(c));
    memset(st, 0, sizeof(*st));
    c.fv = fv;
    c.cfg = cfg;
    c.st = st;
    c.lastCluster = fv->countOfClusters + 1;
    c.bufClusters = COPY_BYTES / fv->bytesPerCluster;
    if (c.bufClusters == 0)
        c.bufClusters = 1;
    bufBytes = (size_t)c.bufClusters * fv->bytesPerCluster;
    seenBytes = c.lastCluster / 8 + 1;
    c.seen = (fat_u8 *)calloc(seenBytes, 1);
    c.buf = (fat_u8 *)malloc(bufBytes);
    c.vbuf = (fat_u8 *)malloc(bufBytes);
    c.sec = (fat_u8 *)malloc(fv->bytesPerSector);
    c.sec2 = (fat_u8 *)malloc(fv->bytesPerSector);
    if (!c.seen || !c.buf || !c.vbuf || !c.sec || !c.sec2) {
        c.oom = 1;
        goto out;
    }

    for (rescans = 0;; rescans++) {
        int w;

        scan(&c);
        if (!fat_mirrors_match(fv))
            st->ioErrors++;
        if (c.stop || !cfg->doMove || !cfg->begin_write ||
            rescans >= MAX_RESCANS ||
            (!st->chainErrors && !st->crossLinked && !st->ioErrors))
            break;
        /* On a live volume the errors may only mean that the FAT read at the
         * start is out of date: look again if the volume changed since. */
        w = sec_enter(&c);
        sec_leave(&c);
        if (w != FAT_WR_CHANGED || c.stop)
            break;
        memset(st, 0, sizeof(*st));
        memset(c.seen, 0, seenBytes);
        c.nObj = 0;
    }
    if (c.stop)
        goto out;
    refresh_pins(&c);
    if (!ext_build(&c) &&
        (!ext_reserve(&c, c.extNeed + EXT_SLACK) || !ext_build(&c)))
        goto out;
    if (!st->crossLinked && !st->chainErrors && !st->ioErrors) {
        c.sim = !cfg->doMove;
        work(&c);
        sec_leave(&c);
        if (!c.oom)
            note_gaps(&c);
        /* The plan was made in the in-memory FAT: back to what is on disk. */
        if (c.sim && !fat_revert(fv)) {
            st->ioErrors++;
            goto out;
        }
    }
    if (cfg->doMove && st->clustersMoved && !c.fsinfoCleared) {
        /* Let Windows recount the free space. commit_move already does
         * this before the first FAT change; this is only a fallback. */
        int saved = c.stop, w;

        c.stop = 0;
        w = sec_enter(&c);
        if (w == FAT_WR_OK || w == FAT_WR_CHANGED) {
            if (!fat_fsinfo_invalidate(fv))
                st->ioErrors++;
            sec_leave(&c);
        }
        c.stop |= saved;
    }
    if (!c.oom)
        final_stats(&c);
out:
    sec_leave(&c);
    free(c.obj);
    free(c.chain);
    free(c.seen);
    free(c.ext);
    free(c.pmax);
    free(c.unit);
    free(c.bySize);
    free(c.rank);
    free(c.todo);
    free(c.buf);
    free(c.vbuf);
    free(c.sec);
    free(c.sec2);
    if (c.oom)
        return -1;
    if (cfg->doMove)
        return (st->noSpace || st->ioErrors || st->chainErrors || st->busy ||
                st->aborted || st->crossLinked) ? 1 : 0;
    return (st->ioErrors || st->chainErrors || st->crossLinked) ? 1 : 0;
}

/* ----------------------------------------------------------------- lookup */

typedef struct {
    fat_u16 name[261]; /* up to 20 parts of 13 characters, 0-terminated   */
    int     next;      /* sequence number expected next; 0 = complete     */
    int     valid;
    fat_u8  sum;       /* checksum of the 8.3 name it belongs to          */
} lfn_state;

static const fat_u8 lfn_off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24,
                                   28, 30};

static fat_u8 sfn_checksum(const fat_u8 *e)
{
    fat_u8 sum = 0;
    int i;

    for (i = 0; i < 11; i++)
        sum = (fat_u8)(((sum & 1) << 7) + (sum >> 1) + e[i]);
    return sum;
}

/*
 * Long-name parts are stored in reverse order: the entry with the highest
 * sequence number comes first and holds the last characters of the name,
 * the entry with the number 1 comes last, right before the 8.3 entry, and
 * holds the first characters. The first entry of the chain is the one
 * flagged 0x40; the unused tail of the name is padded with 0x0000 and
 * 0xFFFF. Every part carries the checksum of the 8.3 name it belongs to.
 */
static void lfn_add(lfn_state *s, const fat_u8 *e)
{
    int seq = e[0] & 0x1F, i;

    if (seq < 1 || seq > 20 || (e[0] & 0xA0) || e[12] != 0 ||
        rd16le(e + 26) != 0) {
        s->valid = 0;
        return;
    }
    if (!s->valid) {
        /* The first part of a chain is the one flagged 0x40; it names the
         * checksum of the 8.3 entry and the highest sequence number of
         * the chain. The parts follow in descending order. */
        if (!(e[0] & 0x40)) {
            s->valid = 0;
            return;
        }
        s->valid = 1;
        s->sum = e[13];
        s->next = seq - 1;
        memset(s->name, 0, sizeof(s->name));
    } else if (seq != s->next || e[13] != s->sum || (e[0] & 0x40)) {
        s->valid = 0; /* the chain is broken */
        return;
    }
    for (i = 0; i < 13; i++)
        s->name[(seq - 1) * 13 + i] = rd16le(e + lfn_off[i]);
    s->next = seq - 1;
}

static int ascii_lower(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch;
}

/* Non-ASCII characters only match when numerically equal (Latin-1). */
static int name_eq_lfn(const lfn_state *s, const char *comp, int len)
{
    int i;

    if (len > 260)
        return 0;
    for (i = 0; i < len; i++) {
        int w = s->name[i], ch = (fat_u8)comp[i];

        if (w == 0 || w == 0xFFFF)
            return 0;
        if (w < 0x80 && ch < 0x80 ? ascii_lower(w) != ascii_lower(ch)
                                  : w != ch)
            return 0;
    }
    return s->name[i] == 0 || s->name[i] == 0xFFFF;
}

static int name_eq_short(const fat_u8 *e, const char *comp, int len)
{
    char nm[16];
    int i;

    fmt_name(e, nm);
    for (i = 0; i < len; i++)
        if (!nm[i] || ascii_lower((fat_u8)nm[i]) != ascii_lower((fat_u8)comp[i]))
            return 0;
    return nm[i] == 0;
}

/* Find comp[0..len) in a directory (the fixed root, or the chain at cl). */
static int dir_find(fat_volume *fv, int fixed, fat_u32 cl, const char *comp,
                    int len, fat_u8 *sec, fat_u32 *outStart, int *outDir)
{
    fat_u32 eps = fv->bytesPerSector / 32, k = 0, inCl = 0, guard = 0, i;
    lfn_state s;

    s.valid = s.next = 0;
    s.sum = 0;
    for (;;) {
        fat_u32 sector;

        if (fixed) {
            if (k >= fv->rootDirSectors)
                return 0;
            sector = fv->firstRootDirSector + k++;
        } else {
            if (inCl == fv->sectorsPerCluster) {
                fat_u32 nx = fat_get(fv, cl);

                if (fat_is_eoc(fv, nx) || !fat_cluster_valid(fv, nx) ||
                    ++guard > fv->countOfClusters)
                    return 0;
                cl = nx;
                inCl = 0;
            }
            sector = fat_cluster_sector(fv, cl) + inCl++;
        }
        if (!fat_read_sectors(fv, sector, 1, sec))
            return 0;
        for (i = 0; i < eps; i++) {
            const fat_u8 *e = sec + i * 32;
            int match;

            if (e[0] == 0x00)
                return 0;
            if (e[0] != 0xE5 && (e[0x0B] & 0x0F) == 0x0F) {
                lfn_add(&s, e);
                continue;
            }
            if (!is_object_entry(e)) {
                s.valid = 0;
                continue;
            }
            match = (s.valid && s.next == 0 && s.sum == sfn_checksum(e) &&
                     name_eq_lfn(&s, comp, len)) ||
                    name_eq_short(e, comp, len);
            s.valid = 0;
            if (match) {
                *outStart = entry_start(fv, e);
                *outDir = (e[0x0B] & 0x10) != 0;
                return 1;
            }
        }
    }
}

int fat_lookup(fat_volume *fv, const char *path, fat_u32 *start)
{
    fat_u8 sec[4096];
    fat_u32 cl = 0;
    int fixed = fv->fatType != FAT_TYPE_32, isDir = 1, found = 0;
    const char *p = path;

    if (fv->bytesPerSector > sizeof(sec) || !p)
        return 0;
    if (p[0] && p[1] == ':')
        p += 2;
    if (!fixed)
        cl = fv->rootCluster;
    for (;;) {
        const char *q;

        while (*p == '\\' || *p == '/')
            p++;
        if (!*p)
            break;
        if (!isDir || (!fixed && !fat_cluster_valid(fv, cl)))
            return 0;
        for (q = p; *q && *q != '\\' && *q != '/'; q++)
            ;
        if (!dir_find(fv, fixed, cl, p, (int)(q - p), sec, &cl, &isDir))
            return 0;
        fixed = 0;
        found = 1;
        p = q;
    }
    if (!found)
        return 0;
    *start = cl;
    return 1;
}

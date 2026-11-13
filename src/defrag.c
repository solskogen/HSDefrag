/*
 * defrag.c - Cluster relocation primitives and the volume defrag driver.
 */
#include "defrag.h"
#include "fileenum.h"
#include "util.h"

HANDLE defrag_open(const wchar_t *path)
{
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    return CreateFileW(path, GENERIC_READ, share, NULL, OPEN_EXISTING,
                        FILE_FLAG_NO_BUFFERING, NULL);
}

static fm_status move_run(void *ctx, void *file, long long vcn,
                           unsigned long long lcn, unsigned count)
{
    VOLUME *v = ctx;
    MOVE_FILE_DATA mfd;
    memset(&mfd, 0, sizeof(mfd));
    mfd.FileHandle = file;
    mfd.StartingVcn.QuadPart = vcn;
    mfd.StartingLcn.QuadPart = (LONGLONG)lcn;
    mfd.ClusterCount = count;
    return (fm_status)hsd_fsctl(v->hVol, FSCTL_MOVE_FILE, &mfd,
                               (DWORD)sizeof(mfd), NULL, 0, NULL);
}

static int find_target(void *ctx, unsigned long long need,
                        unsigned long long minimum, unsigned long long *target,
                        unsigned long long *available)
{
    VOLUME *v = ctx;
    return fm_find_best(v->bitmap, v->bitmapClusters, need, minimum, target, available);
}

static void mark_target(void *ctx, unsigned long long start, unsigned long long count)
{
    volume_mark(ctx, start, count, 1);
}

/* The clusters a move vacated are free again. */
static void unmark_target(void *ctx, unsigned long long start,
                          unsigned long long count)
{
    volume_mark(ctx, start, count, 0);
}

static void exclude_target(void *ctx, unsigned long long start, unsigned long long count)
{
    static int noted;

    if (!volume_exclude(ctx, start, count))
        return; /* exclLost is set; move_output_ok() now stops the moves */
    if (!noted) {
        noted = 1;
        uinfo(L"  note: the file system refused a move target (LCN %llu and "
              L"the following %llu clusters);\n"
              L"such places are avoided from now on.\n",
              start, count);
    }
}

static fm_status refresh_targets(void *ctx)
{
    return (fm_status)volume_refresh_bitmap(ctx);
}

static fm_status refresh_map(void *ctx, void *file, EXTMAP *map)
{
    return (fm_status)extmap_get(ctx, file, map);
}

static void checkpoint_pause(void *ctx, unsigned milliseconds)
{
    (void)ctx;
    Sleep(milliseconds);
}

/* Also stops the move engine once an excluded range could not be recorded:
 * a later bitmap refresh would offer the refused place again. */
static int move_output_ok(void *ctx)
{
    const VOLUME *v = ctx;
    return !v->exclLost && uoutput_ok();
}

reloc_result defrag_relocate(VOLUME *v, HANDLE file, EXTMAP *map, HSD_NTSTATUS *status)
{
    fm_volume moves;
    fm_status result;
    reloc_result relocated;
    memset(&moves, 0, sizeof(moves));
    moves.ctx = v;
    moves.bytesPerCluster = v->bytesPerCluster;
    moves.moveAlign = v->moveAlign;
    moves.clusters = v->bitmapClusters;
    moves.haveBitmap = v->haveBitmap;
    moves.unsupported = v->movesUnsupported;
    moves.find = find_target;
    moves.mark = mark_target;
    moves.unmark = unmark_target;
    moves.exclude = exclude_target;
    moves.refresh = refresh_targets;
    moves.map = refresh_map;
    moves.move = move_run;
    moves.pause = checkpoint_pause;
    moves.alive = move_output_ok;
    relocated = fm_relocate(&moves, file, map, &result);
    *status = (HSD_NTSTATUS)result;
    return relocated;
}

void defrag_explain_unsupported(const VOLUME *v, HSD_NTSTATUS st)
{
    wchar_t code[64];

    uerror(L"the file system on %lc: refused to move clusters: %ls\n", v->drive,
           hsd_status_str(st, code, 64));
    if (v->isNtfs && v->bytesPerCluster > 4096 && !uos_at_least(5, 1)) {
        uinfo(L"NTFS before Windows XP cannot move clusters on volumes with "
              L"clusters larger\nthan 4 KB; this volume uses %llu-byte "
              L"clusters. FORMAT picks such sizes for\nNTFS volumes over 4 GB. "
              L"Only reformatting with 4 KB or smaller clusters\n(FORMAT "
              L"/A:4096) makes it defragmentable.\n",
              v->bytesPerCluster);
    } else if (!v->isNtfs && !v->isFat) {
        uinfo(L"The %ls file system driver does not implement the "
              L"defragmentation interface.\n",
              v->fsName);
    } else {
        uinfo(L"The driver handling this %ls volume does not support "
              L"FSCTL_MOVE_FILE\n(a third-party file system driver or "
              L"filter may be in the way).\n",
              v->fsName);
    }
}

typedef struct {
    VOLUME            *v;
    EXTMAP             map;
    unsigned long long examined;
    unsigned long long contiguous;
    unsigned long long fragmented;
    unsigned long long moved;
    unsigned long long reduced;
    unsigned long long partial;   /* improved, but still fragmented */
    unsigned long long busy;      /* in use: could not even be opened */
    unsigned long long special;
    unsigned long long noSpace;
    unsigned long long refused;
    unsigned long long unopenable;
    unsigned long long unmappable;
    HSD_NTSTATUS        firstRefusal;
    HSD_NTSTATUS        firstMapError;
    DWORD              firstOpenError;
    int                aborted;
    HSD_NTSTATUS        abortStatus;
} defrag_ctx;

static int ntfs_system_name(const wchar_t *name)
{
    return name[0] == L'$';
}

static int defrag_cb(const wchar_t *path, const WIN32_FIND_DATAW *fd, int isDir,
                     void *ctxp)
{
    defrag_ctx *c = (defrag_ctx *)ctxp;
    wchar_t code[64];
    HANDLE h;
    int frags;
    HSD_NTSTATUS st;
    reloc_result r;

    /* Move ordinary files only; NT 4.0 cannot move directory indexes. */
    if (isDir || (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (c->v->isNtfs && ntfs_system_name(fd->cFileName)))
        return 1;

    uprogress(L"  %llu files examined, %llu defragmented", c->examined,
              c->moved);

    h = defrag_open(path);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION) {
            c->busy++;
            uverbose(L"  skipped, in use: %ls\n", path);
            return 1;
        }
        if (c->unopenable++ == 0)
            c->firstOpenError = err;
        uverbose(L"  cannot open (error %lu): %ls\n", (unsigned long)err, path);
        return 1;
    }

    st = extmap_get(c->v, h, &c->map);
    if (!HSD_NT_SUCCESS(st)) {
        CloseHandle(h);
        if (c->unmappable++ == 0)
            c->firstMapError = st;
        uverbose(L"  cannot map (%ls): %ls\n", hsd_status_str(st, code, 64),
                 path);
        return 1;
    }

    c->examined++;
    frags = extmap_fragments(&c->map);
    if (frags <= 1) {
        c->contiguous++;
        CloseHandle(h);
        return 1;
    }
    c->fragmented++;

    if (c->map.hasVirtual || (fd->dwFileAttributes & FM_SPECIAL_ATTRS)) {
        c->special++;
        uverbose(L"  skipped, special (compressed/sparse/encrypted/device/reparse)"
                 L" (%d fragments): %ls\n", frags, path);
        CloseHandle(h);
        return 1;
    }
    r = defrag_relocate(c->v, h, &c->map, &st);
    CloseHandle(h);
    if (c->v->exclLost)
        return 0; /* no further file is moved; cmd_defrag returns 5 */

    switch (r) {
    case RELOC_MOVED:
        c->moved++;
        uverbose(L"  defragmented (%d fragments): %ls\n", frags, path);
        break;
    case RELOC_PARTIAL:
        /* An improvement, not a refusal: it is counted on its own so that
         * "no full-size run" keeps counting the files that did not fit. */
        c->reduced++;
        c->partial++;
        uverbose(L"  reduced fragmentation (%d -> %d fragments): %ls\n",
                 frags, extmap_fragments(&c->map), path);
        break;
    case RELOC_SKIPPED:
        c->special++;
        break;
    case RELOC_NO_SPACE:
        c->noSpace++;
        uverbose(L"  no free run of %lld clusters (%d fragments): %ls\n",
                 c->map.realClusters, frags, path);
        break;
    case RELOC_FAILED:
        if (c->refused++ == 0)
            c->firstRefusal = st;
        uverbose(L"  move refused (%ls): %ls\n", hsd_status_str(st, code, 64),
                 path);
        break;
    case RELOC_UNSUPPORTED:
        c->aborted = 1;
        c->abortStatus = st;
        return 0;
    }
    return 1;
}

static void print_count_status(const wchar_t *label, unsigned long long n,
                               HSD_NTSTATUS first)
{
    wchar_t code[64];
    if (n)
        uinfo(L"%ls: %llu  (first: %ls)\n", label, n,
              hsd_status_str(first, code, 64));
    else
        uinfo(L"%ls: 0\n", label);
}

int cmd_defrag(VOLUME *v)
{
    defrag_ctx c;
    DWORD start, secs;
    unsigned long long notDone;
    ENUM_RESULT scan;
    int rc = 0;

    memset(&c, 0, sizeof(c));
    c.v = v;
    extmap_init(&c.map);
    start = GetTickCount();

    uinfo(L"Defragmenting %lc: (%ls)...\n", v->drive, v->fsName);
    if (v->movesUnsupported) {
        defrag_explain_unsupported(v, HSD_STATUS_NOT_SUPPORTED);
        return 3;
    }
    if (v->exclLost) {
        uerror(L"out of memory recording the clusters that must not be used "
               L"as move targets;\nnothing was moved\n");
        return 5;
    }

    scan = enum_tree(v->root, defrag_cb, &c);
    uprogress_clear();
    extmap_free(&c.map);

    if (c.aborted) {
        defrag_explain_unsupported(v, c.abortStatus);
        return 3;
    }

    secs = (GetTickCount() - start) / 1000;
    notDone = c.fragmented - c.moved;
    if (scan.errors || scan.aborted || c.refused || c.unopenable ||
        c.unmappable || c.noSpace || c.partial)
        rc = 4;
    if (v->exclLost) {
        uerror(L"out of memory recording a refused move target; moving "
               L"stopped\n");
        rc = 5;
    } else if (scan.interrupted) {
        uinfo(L"\nInterrupted by Ctrl+C; files already moved stay moved.\n");
    }
    uinfo(L"\nDefragmentation of %lc: %ls in %lu:%02lu\n", v->drive,
          rc ? L"incomplete" : L"finished",
          (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    uinfo(L"  files examined     : %llu\n", c.examined);
    uinfo(L"  already contiguous : %llu\n", c.contiguous);
    uinfo(L"  fragmented         : %llu\n", c.fragmented);
    uinfo(L"  defragmented       : %llu\n", c.moved);
    uinfo(L"  partially improved : %llu (still fragmented)\n", c.reduced);
    uinfo(L"  not defragmented   : %llu\n", notDone);
    if (notDone) {
        uinfo(L"    special          : %llu (compressed/sparse/encrypted/device/reparse)\n",
              c.special);
        uinfo(L"    still fragmented : %llu\n", c.partial);
        uinfo(L"    no full-size run : %llu\n", c.noSpace);
        print_count_status(L"    move refused     ", c.refused,
                           c.firstRefusal);
    }
    if (c.busy)
        uinfo(L"  in use (skipped)   : %llu\n", c.busy);
    if (c.unopenable) {
        wchar_t msg[200];
        uinfo(L"  could not open     : %llu  (first: %ls)\n", c.unopenable,
              uwin32_str(c.firstOpenError, msg, 200));
    }
    if (c.unmappable)
        print_count_status(L"  could not map      ", c.unmappable,
                           c.firstMapError);
    if (c.noSpace && c.moved)
        uinfo(L"Some files did not fit into a contiguous free run; running "
              L"again may help.\n");
    if (scan.errors)
        uinfo(L"  traversal errors   : %llu\n", scan.errors);

    return uoutput_ok() ? rc : 5;
}

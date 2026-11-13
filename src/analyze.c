/*
 * analyze.c - Fragmentation analysis and reporting.
 */
#include "analyze.h"
#include "defrag.h"
#include "fileenum.h"
#include "retrieval.h"
#include "util.h"
#include "mft.h"

typedef struct {
    VOLUME            *volume;
    EXTMAP             map;
    unsigned long long files;
    unsigned long long dirs;
    unsigned long long fragmentedFiles;
    unsigned long long totalFragments;   /* summed physical fragments */
    unsigned long long excessFragments;  /* fragments beyond 1 per file */
    unsigned long long fileClusters;     /* clusters occupied by files */
    unsigned long long inUse;            /* held open exclusively elsewhere */
    unsigned long long unopenable;
    unsigned long long unmappable;
    DWORD              firstOpenError;
    HSD_NTSTATUS        firstMapError;
} analyze_ctx;

static int analyze_cb(const wchar_t *path, const WIN32_FIND_DATAW *fd,
                      int isDir, void *ctxp)
{
    analyze_ctx *c = (analyze_ctx *)ctxp;
    wchar_t code[64];
    HSD_NTSTATUS st;
    HANDLE h;
    int frags;

    if (isDir) {
        c->dirs++;
        return 1;
    }
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return 1;

    uprogress(L"  %llu files examined", c->files);

    h = defrag_open(path);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION) {
            c->inUse++;
            uverbose(L"  in use, skipped: %ls\n", path);
            return 1;
        }
        if (c->unopenable++ == 0)
            c->firstOpenError = err;
        uverbose(L"  cannot open (error %lu): %ls\n", (unsigned long)err, path);
        return 1;
    }

    st = extmap_get(c->volume, h, &c->map);
    CloseHandle(h);
    if (!HSD_NT_SUCCESS(st)) {
        if (c->unmappable++ == 0)
            c->firstMapError = st;
        uverbose(L"  cannot map (%ls): %ls\n", hsd_status_str(st, code, 64),
                 path);
        return 1;
    }

    c->files++;
    c->fileClusters += (unsigned long long)c->map.realClusters;

    frags = extmap_fragments(&c->map);
    if (frags > 0)
        c->totalFragments += (unsigned long long)frags;
    if (frags > 1) {
        c->fragmentedFiles++;
        c->excessFragments += (unsigned long long)(frags - 1);
        uverbose(L"  %6d frags: %ls\n", frags, path);
    }
    return 1;
}

int cmd_analyze(VOLUME *v, unsigned flags)
{
    analyze_ctx c;
    unsigned long long pct; /* tenths of a percent */
    wchar_t b1[32], b2[32], code[64];
    ENUM_RESULT scan;
    int rc = 0;

    memset(&c, 0, sizeof(c));
    c.volume = v;
    memset(&scan, 0, sizeof(scan));
    extmap_init(&c.map);

    uinfo(L"Analyzing %lc: (%ls)...\n", v->drive, v->fsName);

    if (!(flags & ANALYZE_NO_FILES)) {
        scan = enum_tree(v->root, analyze_cb, &c);
        uprogress_clear();
    }

    /* Integer tenths of a percent: no floating point anywhere. */
    pct = (c.files > 0)
              ? (c.fragmentedFiles * 1000 + c.files / 2) / c.files
              : 0;

    uinfo(L"\nAnalysis of %lc:\n", v->drive);
    if (flags & ANALYZE_NO_FILES) {
        uinfo(L"  files              : not examined (--no-files)\n");
    } else {
        uinfo(L"  files              : %llu\n", c.files);
        uinfo(L"  directories        : %llu\n", c.dirs);
        uinfo(L"  fragmented files   : %llu (%llu.%llu%%)\n", c.fragmentedFiles,
              pct / 10, pct % 10);
        uinfo(L"  total fragments    : %llu\n", c.totalFragments);
        uinfo(L"  excess fragments   : %llu\n", c.excessFragments);
        uinfo(L"  data in files      : %ls\n",
              uformat_bytes(c.fileClusters * v->bytesPerCluster, b1, 32));
    }
    uinfo(L"  free space         : %ls\n",
          uformat_bytes(v->freeClusters * v->bytesPerCluster, b2, 32));
    if (v->haveBitmap) {
        unsigned long long largest = volume_largest_free_run(v);
        uinfo(L"  largest free run   : %llu clusters (%ls)%ls\n", largest,
              uformat_bytes(largest * v->bytesPerCluster, b1, 32),
              (v->mftZoneEnd > v->mftZoneStart) ? L", outside the MFT zone"
                                                 : L"");
    } else {
        uinfo(L"  largest free run   : n/a (cluster bitmap unavailable)\n");
    }
    if (c.inUse)
        uinfo(L"  in use (skipped)   : %llu\n", c.inUse);
    if (c.unopenable) {
        wchar_t msg[200];
        uinfo(L"  could not open     : %llu  (first: %ls)\n", c.unopenable,
              uwin32_str(c.firstOpenError, msg, 200));
    }
    if (c.unmappable)
        uinfo(L"  could not map      : %llu  (first: %ls)\n", c.unmappable,
              hsd_status_str(c.firstMapError, code, 64));
    if (c.unmappable && c.files == 0)
        hsd_status_error(L"FSCTL_GET_RETRIEVAL_POINTERS failed for every file",
                        c.firstMapError);
    if (scan.interrupted) {
        uinfo(L"  coverage           : INCOMPLETE (interrupted by Ctrl+C)\n");
        rc = 4;
    } else if (scan.errors || scan.aborted || c.unopenable || c.unmappable) {
        uinfo(L"  coverage           : INCOMPLETE (%llu traversal errors)\n",
              scan.errors);
        rc = 4;
    } else if (c.inUse || (flags & ANALYZE_NO_FILES)) {
        uinfo(L"  coverage           : excludes skipped/unexamined files\n");
    }

    /* Report metadata fragmentation (NTFS only), without moving anything. */
    if (v->isNtfs && !scan.interrupted && !(flags & ANALYZE_MFT_ELSEWHERE)) {
        if (flags & ANALYZE_NO_META) {
            uinfo(L"\n  NTFS metadata: not read (--no-meta)\n");
        } else {
            /* A runtime failure (5) stays a failure; anything else from the
             * report only makes the analysis incomplete. */
            int meta = mft_report(v);
            if (meta == 5)
                rc = 5;
            else if (meta)
                rc = 4;
        }
    }

    extmap_free(&c.map);
    return uoutput_ok() ? rc : 5;
}

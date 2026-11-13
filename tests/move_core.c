#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "filemove.h"

static unsigned checks;
static int mode, calls;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL shared file moves line %d mode %d: %s\n", __LINE__, mode, #x); \
    exit(1); } } while (0)

static void put64(unsigned char *p, unsigned long long value)
{
    unsigned i;
    for (i = 0; i < 8; i++)
        p[i] = (unsigned char)(value >> (i * 8));
}

static fm_status control(void *ctx, void *handle, unsigned code, void *in,
                          unsigned inBytes, void *out, unsigned outBytes, unsigned *got)
{
    unsigned char *p = out;
    (void)ctx; (void)handle;
    calls++;
    CHECK(inBytes == 8);
    memset(out, 0, outBytes);
    if (code == FM_RETRIEVAL) {
        long long requested;
        memcpy(&requested, in, 8);
        CHECK(outBytes == 16 + 1024 * 16);
        p[0] = 1;
        if (mode == 1) {
            const long long maximum = 0x7FFFFFFFFFFFFFFFLL;
            put64(p + 8, requested ? (unsigned long long)(maximum - 20) : 0);
            put64(p + 16, (unsigned long long)(maximum - (requested ? 12 : 16)));
            put64(p + 24, requested ? (unsigned long long)(maximum - 12) : 0);
        } else {
            put64(p + 8, requested ? 4 : 0);
            put64(p + 16, requested ? 12 : 8);
            put64(p + 24, requested ? 100 : 40);
        }
        *got = mode == 2 ? 16 : mode == 3 ? outBytes + 1 : 32;
        if (mode == 4) p[0] = 0;
        if (mode == 5) put64(p + 24, ~1ULL);
        if (mode == 6 && requested) put64(p + 16, 8);
        if (mode == 7) put64(p + 24, 0x7FFFFFFFFFFFFFFFULL);
        if (mode == 8) return FM_EOF;
        return requested ? FM_SUCCESS : FM_OVERFLOW;
    }
    CHECK(code == FM_BITMAP && outBytes == 32);
    {
        unsigned long long requested;
        memcpy(&requested, in, 8);
        put64(p, mode == 12 ? 1 : requested);
        put64(p + 8, mode == 13 ? 22 : mode == 14 ? 0 : 21 - requested);
        p[16] = requested ? 0x10 : 1;
        *got = mode == 10 ? 16 : mode == 11 ? outBytes + 1 : requested ? 17 : 18;
        return requested ? FM_SUCCESS : FM_OVERFLOW;
    }
}

static void parser_checks(void)
{
    EXTMAP map;
    unsigned char *bitmap = NULL;
    unsigned long long clusters = 0;
    extmap_init(&map);
    for (mode = 0; mode <= 8; mode++) {
        fm_status status;
        calls = 0;
        status = fm_get_extents(control, NULL, NULL, &map);
        if (mode == 0) {
            CHECK(status == FM_SUCCESS && calls == 2 && map.n == 2);
            CHECK(map.ext[1].vcn == 8 && map.ext[1].lcn == 104 && map.ext[1].count == 4);
        } else if (mode == 1) {
            CHECK(status == FM_SUCCESS && calls == 2 && map.n == 2);
            CHECK(map.ext[1].vcn == 0x7FFFFFFFFFFFFFEFLL &&
                  map.ext[1].lcn == 0x7FFFFFFFFFFFFFF7LL && map.ext[1].count == 4);
        } else if (mode == 8)
            CHECK(status == FM_SUCCESS && !map.n);
        else
            CHECK(status == FM_BAD_REPLY && !map.n);
    }
    extmap_free(&map);
    for (mode = 9; mode <= 14; mode++) {
        fm_status status;
        calls = 0;
        status = fm_get_bitmap(control, NULL, NULL, 16, 21, &bitmap, &clusters);
        if (mode == 9) {
            CHECK(status == FM_SUCCESS && calls == 2 && clusters == 21 &&
                  bitmap[0] == 1 && !bitmap[1] && bitmap[2] == 0xF0);
            free(bitmap); bitmap = NULL;
        } else
            CHECK(status == FM_BAD_REPLY && !bitmap);
    }
}

static void bitmap_checks(void)
{
    unsigned seed = 1, trial;
    for (trial = 0; trial < 128; trial++) {
        unsigned char bitmap[41], oracle[41];
        unsigned i, need, minimum;
        for (i = 0; i < sizeof(bitmap); i++) {
            seed = seed * 1664525U + 1013904223U;
            bitmap[i] = (unsigned char)(seed >> 24);
        }
        memcpy(oracle, bitmap, sizeof(bitmap));
        for (i = 0; i < 4; i++) {
            unsigned pos, len, used, k;
            seed = seed * 1664525U + 1013904223U;
            pos = (seed >> 16) % 323;
            seed = seed * 1664525U + 1013904223U;
            len = 1 + ((seed >> 16) % 160);
            used = seed & 1;
            fm_mark(bitmap, 323, pos, len, (int)used);
            for (k = 0; k < len && pos + k < 323; k++) {
                unsigned bit = pos + k;
                if (used)
                    oracle[bit / 8] |= (unsigned char)(1U << (bit & 7));
                else
                    oracle[bit / 8] &= (unsigned char)~(1U << (bit & 7));
            }
        }
        CHECK(!memcmp(bitmap, oracle, sizeof(bitmap)));
        for (need = 1; need <= 140; need += (need < 20 ? 1 : 17))
            for (minimum = 0; minimum <= 323; minimum += 19) {
                int actual;
                unsigned long long target = ~0ULL;
                {
                    unsigned best = 0, start = 0, at, run = 0;
                    unsigned long long available = 0;
                    for (at = minimum; at <= 323; at++) {
                        if (at < 323 && !(bitmap[at / 8] & (1U << (at & 7)))) {
                            run++;
                            continue;
                        }
                        if (run && (!best ||
                            (run >= need && (best < need || run < best)) ||
                            (run < need && best < need && run > best))) {
                            best = run;
                            start = at - run;
                        }
                        run = 0;
                    }
                    actual = fm_find_best(bitmap, 323, need, minimum, &target, &available);
                    CHECK(actual == (best != 0) && available == best &&
                          (!actual || target == start));
                }
            }
        fm_mark(bitmap, 323, 322, ~0ULL, 1);
        CHECK(bitmap[40] & 4);
        fm_mark(bitmap, 323, ~0ULL, ~0ULL, 0);
        CHECK(bitmap[40] & 4);
    }
}

typedef struct { int moves, refreshes, pauses; fm_status moveStatus; } relocation;
static int find(void *ctx, unsigned long long count, unsigned long long minimum,
                  unsigned long long *target, unsigned long long *available)
{
    relocation *r = ctx;
    CHECK(count == 2 && !minimum); *target = 10 + (unsigned)r->moves * 2;
    *available = 2; return 1;
}
static void mark(void *ctx, unsigned long long target, unsigned long long count)
{
    (void)ctx; CHECK(target >= 10 && count == 2);
}
static void unmark(void *ctx, unsigned long long target, unsigned long long count)
{
    (void)ctx; CHECK(target >= 10 && count == 2);
}
static void exclude(void *ctx, unsigned long long target, unsigned long long count)
{
    (void)ctx; (void)target; (void)count; CHECK(0);
}
static fm_status refresh(void *ctx)
{
    relocation *r = ctx; r->refreshes++; return FM_SUCCESS;
}
static fm_status mapped(void *ctx, void *file, EXTMAP *map)
{
    (void)ctx; (void)file;
    extmap_reset(map);
    CHECK(extmap_add(map, 0, 60, 1) && extmap_add(map, 1, 62, 1));
    return FM_SUCCESS;
}
static fm_status move(void *ctx, void *file, long long vcn, unsigned long long lcn, unsigned count)
{
    relocation *r = ctx; (void)file;
    CHECK(!vcn && lcn >= 10 && count == 2); r->moves++; return r->moveStatus;
}
static void pause_checkpoint(void *ctx, unsigned milliseconds)
{
    relocation *r = ctx; CHECK(milliseconds == 3000); r->pauses++;
}
static int alive(void *ctx) { (void)ctx; return 1; }
static void relocation_checks(void)
{
    EXTMAP map;
    relocation r;
    fm_volume volume;
    fm_status status;
    int test;
    extmap_init(&map);
    memset(&volume, 0, sizeof(volume));
    volume.ctx = &r; volume.bytesPerCluster = 4096; volume.moveAlign = 1;
    volume.clusters = 128; volume.haveBitmap = 1;
    volume.find = find; volume.mark = mark; volume.unmark = unmark;
    volume.exclude = exclude;
    volume.refresh = refresh; volume.map = mapped; volume.move = move;
    volume.pause = pause_checkpoint; volume.alive = alive;
    for (test = 0; test < 2; test++) {
        memset(&r, 0, sizeof(r));
        r.moveStatus = test ? FM_ALREADY_COMMITTED : FM_SUCCESS;
        CHECK(mapped(&r, NULL, &map) == FM_SUCCESS);
        CHECK(fm_relocate(&volume, NULL, &map, &status) == RELOC_FAILED);
        CHECK(r.moves == 4 && r.refreshes == 2 && r.pauses == (test ? 2 : 0));
        CHECK(status == (test ? FM_ALREADY_COMMITTED : FM_NOT_MOVED));
    }
    extmap_free(&map);
}

typedef struct {
    unsigned char bitmap[128];
    int owner[1024], lcn[128], length, moves, maps, refreshes, pauses;
    int noMove, failMove, failMap, failRefresh, checkpoint, reject, stopped, badMap;
    int excludes, rejectAll;
    unsigned long long exclStart[8], exclCount[8];
    fm_status failure;
} model;

static fm_status model_map(void *ctx, void *file, EXTMAP *map)
{
    model *m = ctx;
    int i = 0;
    (void)file;
    if (++m->maps == m->failMap)
        return FM_BAD_REPLY;
    extmap_reset(map);
    while (i < m->length) {
        int begin = i++;
        while (i < m->length && m->lcn[i] == m->lcn[i - 1] + 1)
            i++;
        CHECK(extmap_add(map, begin, m->lcn[begin], i - begin));
    }
    if (m->badMap)
        map->ext[0].lcn = 1024;
    return FM_SUCCESS;
}
static fm_status model_refresh(void *ctx)
{
    model *m = ctx;
    int i;
    if (++m->refreshes == m->failRefresh)
        return FM_NOMEM;
    memset(m->bitmap, 0, sizeof(m->bitmap));
    for (i = 0; i < 1024; i++)
        if (m->owner[i])
            fm_mark(m->bitmap, 1024, (unsigned)i, 1, 1);
    return FM_SUCCESS;
}
static int model_find(void *ctx, unsigned long long need,
                       unsigned long long minimum, unsigned long long *target,
                       unsigned long long *available)
{
    model *m = ctx;
    return fm_find_best(m->bitmap, 1024, need, minimum, target, available);
}
static void model_mark(void *ctx, unsigned long long start, unsigned long long count)
{
    model *m = ctx;
    CHECK(start < 1024 && count <= 1024 - start);
    fm_mark(m->bitmap, 1024, start, count, 1);
}
static void model_unmark(void *ctx, unsigned long long start, unsigned long long count)
{
    model *m = ctx;
    CHECK(start < 1024 && count <= 1024 - start);
    fm_mark(m->bitmap, 1024, start, count, 0);
}
static void model_exclude(void *ctx, unsigned long long start, unsigned long long count)
{
    model *m = ctx;
    CHECK(m->excludes < 8);
    m->exclStart[m->excludes] = start;
    m->exclCount[m->excludes] = count;
    m->excludes++;
    model_mark(ctx, start, count);
}
static void model_pause(void *ctx, unsigned ms)
{
    model *m = ctx;
    CHECK(ms == 3000); m->pauses++;
}
static int model_alive(void *ctx) { return !((model *)ctx)->stopped; }
static fm_status model_move(void *ctx, void *file, long long vcn,
                             unsigned long long target, unsigned count)
{
    model *m = ctx;
    unsigned i;
    (void)file;
    CHECK(vcn >= 0 && !(vcn % 16) && count && count <= 16 &&
          vcn + count <= m->length && target + count <= 1024);
    m->moves++;
    if (m->checkpoint && m->moves <= 2)
        return FM_ALREADY_COMMITTED;
    if ((m->reject && m->moves == 1) || m->rejectAll)
        return FM_INVALID_PARAMETER;
    if (m->moves == m->failMove)
        return m->failure;
    if (m->noMove)
        return FM_SUCCESS;
    for (i = 0; i < count; i++) {
        int source = m->lcn[vcn + i];
        CHECK(!m->owner[target + i] && m->owner[source] == vcn + (int)i + 1);
        m->owner[target + i] = m->owner[source]; m->owner[source] = 0;
        m->lcn[vcn + i] = (int)target + (int)i;
    }
    return FM_SUCCESS;
}
static void model_setup(model *m, fm_volume *v, EXTMAP *map, int hole)
{
    int i;
    memset(m, 0, sizeof(*m));
    m->length = 64;
    for (i = 0; i < 1024; i++)
        m->owner[i] = -1;
    for (i = 0; i < m->length; i++) {
        m->lcn[i] = 16 + (i / 16) * 48 + i % 16;
        m->owner[m->lcn[i]] = i + 1;
    }
    for (i = 256; i < 256 + hole; i++)
        m->owner[i] = 0;
    memset(v, 0, sizeof(*v));
    v->ctx = m; v->bytesPerCluster = 65536; v->moveAlign = 16;
    v->clusters = 1024; v->haveBitmap = 1;
    v->find = model_find; v->mark = model_mark; v->unmark = model_unmark;
    v->exclude = model_exclude;
    v->refresh = model_refresh; v->map = model_map; v->move = model_move;
    v->pause = model_pause; v->alive = model_alive;
    CHECK(model_map(m, NULL, map) == FM_SUCCESS);
    CHECK(model_refresh(m) == FM_SUCCESS);
    m->maps = m->refreshes = 0;
}

static void placement_checks(void)
{
    model m;
    fm_volume v;
    EXTMAP map;
    fm_status status;
    int test, i;
    extmap_init(&map);
    for (test = 0; test < 18; test++) {
        int hole = test == 1 || test == 3 || test == 10 || test == 12 || test == 14 ?
                   32 : test == 2 ? 16 : 64;
        reloc_result result;
        model_setup(&m, &v, &map, hole);
        if (test == 3) m.noMove = 1;
        if (test == 4) { m.failMove = 2; m.failure = FM_INVALID_REQUEST; }
        if (test == 5) m.failMap = 1;
        if (test == 6) { m.failMove = 2; m.failure = FM_NOMEM; }
        if (test == 7) m.checkpoint = 1;
        if (test == 8) { m.noMove = 1; m.failRefresh = 1; }
        if (test == 9) {
            m.reject = 1;
            for (i = 512; i < 576; i++) m.owner[i] = 0;
            CHECK(model_refresh(&m) == FM_SUCCESS);
        }
        if (test == 10) {
            /* An unaligned boundary cannot be used to merge two fragments. */
            m.lcn[15] = 210; m.owner[31] = 0; m.owner[210] = 16;
            CHECK(model_map(&m, NULL, &map) == FM_SUCCESS);
        }
        if (test == 11) m.stopped = 1;
        if (test == 12) { m.failMove = 2; m.failure = FM_NOMEM; }
        if (test == 13) m.badMap = 1;
        if (test == 14) {
            extmap_reset(&map);
            for (i = 0; i < 64; i += 8)
                CHECK(extmap_add(&map, i, m.lcn[i], 8));
        }
        if (test == 15) map.ext[0].vcn = 1;
        if (test == 16) map.ext[0].lcn = 1024;
        if (test == 17) v.moveAlign = 65537;
        result = fm_relocate(&v, NULL, &map, &status);
        if (test == 0 || test == 7 || test == 9) {
            CHECK(result == RELOC_MOVED && status == FM_SUCCESS &&
                  extmap_fragments(&map) == 1);
            /* One pause, and no re-read of the volume: the clusters a
             * failed move reserved are given back as soon as the map
             * says the data is not there. */
            if (test == 7) CHECK(m.pauses == 1 && m.refreshes == 1);
        } else if (test == 1 || test == 10 || test == 14) {
            CHECK(result == RELOC_PARTIAL && status == FM_SUCCESS &&
                  extmap_fragments(&map) == (test == 10 ? 4 : 3));
        } else if (test == 2) {
            CHECK(result == RELOC_NO_SPACE && !m.moves);
        } else if (test == 4)
            CHECK(result == RELOC_UNSUPPORTED && status == FM_INVALID_REQUEST);
        else {
            CHECK(result == RELOC_FAILED && status != FM_SUCCESS);
            if (test == 3) CHECK(extmap_fragments(&map) == 4);
            if (test == 13) CHECK(status == FM_BAD_REPLY && m.moves == 4);
            if (test >= 15) CHECK(!m.moves && status ==
                                 (test == 17 ? FM_INVALID_PARAMETER : FM_BAD_REPLY));
        }
        for (i = 0; i < m.length; i++)
            CHECK(m.owner[m.lcn[i]] == i + 1);
    }
    extmap_free(&map);
}

/* A move must give the clusters it left behind back: the cached bitmap has
 * to show them as free, or the rest of the run thinks the volume is full. */
static void vacated_checks(void)
{
    model m;
    fm_volume v;
    EXTMAP map;
    fm_status status;
    unsigned char before[128];
    int i;
    extmap_init(&map);
    model_setup(&m, &v, &map, 64);
    memcpy(before, m.bitmap, sizeof(before));
    CHECK(fm_relocate(&v, NULL, &map, &status) == RELOC_MOVED);
    CHECK(status == FM_SUCCESS && extmap_fragments(&map) == 1);
    for (i = 0; i < 1024; i++) {
        int expected = (before[i / 8] >> (i & 7)) & 1;

        if (i >= 16 && i < 32) expected = 0;        /* the first fragment */
        if (i >= 64 && i < 80) expected = 0;        /* the second one */
        if (i >= 112 && i < 128) expected = 0;      /* the third one */
        if (i >= 160 && i < 176) expected = 0;      /* the fourth one */
        if (i >= 256 && i < 320) expected = 1;      /* where the data went */
        CHECK(((m.bitmap[i / 8] >> (i & 7)) & 1) == expected);
    }
    extmap_free(&map);
}

/* A refusal gives up the place the driver refused and nothing else: the
 * free space around it has to stay available. */
static void refusal_checks(void)
{
    model m;
    fm_volume v;
    EXTMAP map;
    fm_status status;
    int i;
    extmap_init(&map);
    model_setup(&m, &v, &map, 64);
    for (i = 512; i < 576; i++)
        m.owner[i] = 0;
    CHECK(model_refresh(&m) == FM_SUCCESS);
    m.reject = 1; /* the driver refuses the first target */
    CHECK(fm_relocate(&v, NULL, &map, &status) == RELOC_MOVED);
    /* exactly one place was given up: the refused one */
    CHECK(m.excludes == 1);
    CHECK(m.exclStart[0] == 256 && m.exclCount[0] == 64);
    /* it is not offered again ... */
    for (i = 256; i < 320; i++)
        CHECK(m.bitmap[i / 8] & (1U << (i & 7)));
    /* ... but the second free run was used, and the file is whole */
    for (i = 0; i < 64; i++)
        CHECK(m.lcn[i] == 512 + i);
    extmap_free(&map);
}

/* The data has to land on an aligned place: NTFS refuses a move whose
 * destination LCN is not a multiple of the record size. */
static void alignment_checks(void)
{
    model m;
    fm_volume v;
    EXTMAP map;
    fm_status status;
    int i;
    extmap_init(&map);
    model_setup(&m, &v, &map, 64);
    /* a free run that starts unaligned, but is long enough to hold the
     * file at the first aligned place in it */
    for (i = 256; i < 260; i++)
        m.owner[i] = -1;
    for (i = 320; i < 340; i++)
        m.owner[i] = 0;
    CHECK(model_refresh(&m) == FM_SUCCESS);
    CHECK(fm_relocate(&v, NULL, &map, &status) == RELOC_MOVED);
    for (i = 0; i < 64; i++)
        CHECK(m.lcn[i] == 272 + i);
    extmap_free(&map);
}

/* Four refusals in a row: the result must carry the refusal, not success. */
static void refused_status_checks(void)
{
    model m;
    fm_volume v;
    EXTMAP map;
    fm_status status = FM_SUCCESS;
    int i;
    extmap_init(&map);
    model_setup(&m, &v, &map, 64);
    for (i = 512; i < 576; i++) m.owner[i] = 0;
    for (i = 640; i < 704; i++) m.owner[i] = 0;
    for (i = 768; i < 832; i++) m.owner[i] = 0;
    CHECK(model_refresh(&m) == FM_SUCCESS);
    m.rejectAll = 1;
    CHECK(fm_relocate(&v, NULL, &map, &status) == RELOC_FAILED);
    CHECK(m.excludes == 4 && m.moves == 4);
    CHECK(status == FM_INVALID_PARAMETER);
    for (i = 0; i < m.length; i++)
        CHECK(m.owner[m.lcn[i]] == i + 1);
    extmap_free(&map);
}

/*
 * A driver that moves data and then reports failure leaves the read-back map
 * as the only truth. Here it scatters the first 40 clusters of a 64-cluster
 * stream over the reserved range 200..263 in 20 two-cluster extents. Every
 * cluster the stream occupies must stay marked in the cached bitmap.
 */
typedef struct {
    unsigned char bitmap[128];
    int lcn[64], moves, layout;
} scatter_model;

static fm_status scatter_map(void *ctx, void *file, EXTMAP *map)
{
    scatter_model *m = ctx;
    int i;
    (void)file;
    extmap_reset(map);
    for (i = 0; i < 64; i++) {
        if (map->n && map->ext[map->n - 1].lcn + map->ext[map->n - 1].count == m->lcn[i])
            map->ext[map->n - 1].count++, map->realClusters++;
        else
            CHECK(extmap_add(map, i, m->lcn[i], 1));
    }
    return FM_SUCCESS;
}
static fm_status scatter_refresh(void *ctx) { (void)ctx; return FM_SUCCESS; }
static int scatter_find(void *ctx, unsigned long long need,
                         unsigned long long minimum, unsigned long long *target,
                         unsigned long long *available)
{
    scatter_model *m = ctx;
    return fm_find_best(m->bitmap, 1024, need, minimum, target, available);
}
static void scatter_mark(void *ctx, unsigned long long start, unsigned long long count)
{
    scatter_model *m = ctx;
    CHECK(start < 1024 && count <= 1024 - start);
    fm_mark(m->bitmap, 1024, start, count, 1);
}
static void scatter_unmark(void *ctx, unsigned long long start, unsigned long long count)
{
    scatter_model *m = ctx;
    CHECK(start < 1024 && count <= 1024 - start);
    fm_mark(m->bitmap, 1024, start, count, 0);
}
static void scatter_pause(void *ctx, unsigned ms) { (void)ctx; (void)ms; }
static int scatter_alive(void *ctx) { (void)ctx; return 1; }
static fm_status scatter_move(void *ctx, void *file, long long vcn,
                               unsigned long long target, unsigned count)
{
    scatter_model *m = ctx;
    int k;
    (void)file; (void)vcn; (void)count;
    CHECK(target == 200);
    m->moves++;
    for (k = 0; k < 20; k++) {
        /* layout 0: separate pieces 200, 203, ...; layout 1: touching
         * pieces in reverse order, 238, 236, ..., 200 */
        int at = m->layout ? 200 + 2 * (19 - k) : 200 + 3 * k;
        m->lcn[2 * k] = at;
        m->lcn[2 * k + 1] = at + 1;
    }
    return (fm_status)0xC0000001U;
}

static void scatter_checks(void)
{
    scatter_model m;
    fm_volume v;
    EXTMAP map;
    fm_status status;
    int layout, i;
    extmap_init(&map);
    for (layout = 0; layout < 2; layout++) {
        memset(&m, 0, sizeof(m));
        m.layout = layout;
        memset(m.bitmap, 0xFF, sizeof(m.bitmap));
        fm_mark(m.bitmap, 1024, 200, 64, 0);
        for (i = 0; i < 64; i++)
            m.lcn[i] = i < 32 ? 400 + i : 500 + i - 32;
        memset(&v, 0, sizeof(v));
        v.ctx = &m; v.bytesPerCluster = 65536; v.moveAlign = 1;
        v.clusters = 1024; v.haveBitmap = 1;
        v.find = scatter_find; v.mark = scatter_mark; v.unmark = scatter_unmark;
        v.refresh = scatter_refresh; v.map = scatter_map; v.move = scatter_move;
        v.pause = scatter_pause; v.alive = scatter_alive;
        CHECK(scatter_map(&m, NULL, &map) == FM_SUCCESS && extmap_fragments(&map) == 2);
        CHECK(fm_relocate(&v, NULL, &map, &status) == RELOC_FAILED);
        CHECK(m.moves == 1 && status == (fm_status)0xC0000001U);
        CHECK(map.n == 21);
        for (i = 0; i < 64; i++)
            CHECK(m.bitmap[m.lcn[i] / 8] & (1U << (m.lcn[i] & 7)));
        /* the touching layout is one piece: the rest of the range is free */
        if (layout)
            for (i = 240; i < 264; i++)
                CHECK(!(m.bitmap[i / 8] & (1U << (i & 7))));
    }
    extmap_free(&map);
}

int main(void)
{
    parser_checks(); bitmap_checks(); relocation_checks(); placement_checks();
    vacated_checks(); refusal_checks(); alignment_checks();
    refused_status_checks(); scatter_checks();
    printf("Shared file-move parser/bitmap/retry regressions: %u checks.\n", checks);
    return 0;
}

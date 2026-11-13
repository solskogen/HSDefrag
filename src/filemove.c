/*
 * filemove.c - shared NT move engine (online defrag and boot-time helper)
 *
 * Data moves only through FSCTL_MOVE_FILE. The filesystem copies the
 * clusters and switches its own metadata, so an interrupted or refused move
 * leaves the file at its old place or its new one, never in between. This
 * file never writes to the disk itself; the callers supply the FSCTLs and
 * the cached volume bitmap through the fm_volume ops table, which also lets
 * the host tests drive it without Windows.
 *
 * Every FSCTL reply (retrieval pointers, bitmap) is checked for length, run
 * count and VCN continuity before use. A target is chosen best-fit: the
 * smallest free run that holds the whole stream, otherwise the largest run,
 * into which only whole physical fragments are consolidated. Existing runs
 * are never split to fill a smaller hole.
 *
 * NT4 moves file data in units of the compression/record granularity
 * (moveAlign): source VCN boundaries and the destination LCN are aligned to
 * it, or NTFS refuses the move. Moves are issued in chunks of about 1 MiB.
 *
 * After each attempt a fresh extent map decides what happened, never the
 * return code alone: vacated clusters are given back to the cached bitmap,
 * the target clusters the data really occupies stay marked, and when in
 * doubt (map unreadable) the reserved range stays reserved. A target the
 * driver refuses (STATUS_INVALID_PARAMETER) is excluded through the ops
 * table and the search continues above it, for at most four attempts.
 * A checkpoint (STATUS_ALREADY_COMMITTED) gets one pause and a bitmap
 * re-read before the next try.
 */
#include <stdlib.h>
#include <string.h>
#include "filemove.h"

static unsigned fm_u32(const unsigned char *p)
{
    return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned long long fm_u64(const unsigned char *p)
{
    return fm_u32(p) | ((unsigned long long)fm_u32(p + 4) << 32);
}

void extmap_init(EXTMAP *m) { memset(m, 0, sizeof(*m)); }
void extmap_reset(EXTMAP *m) { m->n = 0; m->realClusters = 0; m->hasVirtual = 0; }
void extmap_free(EXTMAP *m) { free(m->ext); memset(m, 0, sizeof(*m)); }

int extmap_add(EXTMAP *m, long long vcn, long long lcn, long long count)
{
    if (vcn < 0 || lcn < -1 || count <= 0 ||
        count > 0x7FFFFFFFFFFFFFFFLL - vcn ||
        (lcn >= 0 && (count > 0x7FFFFFFFFFFFFFFFLL - lcn ||
                     count > 0x7FFFFFFFFFFFFFFFLL - m->realClusters)))
        return 0;
    if (m->n == m->cap) {
        int capacity;
        EXTENT *p;
        if (m->cap > 0x3FFFFFFF / (int)sizeof(*p))
            return 0;
        capacity = m->cap ? m->cap * 2 : 16;
        p = realloc(m->ext, (size_t)capacity * sizeof(*p));
        if (!p)
            return 0;
        m->ext = p; m->cap = capacity;
    }
    m->ext[m->n].vcn = vcn; m->ext[m->n].lcn = lcn;
    m->ext[m->n++].count = count;
    if (lcn < 0)
        m->hasVirtual = 1;
    else
        m->realClusters += count;
    return 1;
}

int extmap_fragments(const EXTMAP *m)
{
    int i, fragments = 0;
    long long next = -2;
    for (i = 0; i < m->n; i++) {
        if (m->ext[i].lcn < 0)
            continue;
        if (m->ext[i].lcn != next)
            fragments++;
        next = m->ext[i].lcn + m->ext[i].count;
    }
    return fragments;
}

/* Independent copy of a layout, used to compare before and after a move. */
static int extmap_copy(EXTMAP *dst, const EXTMAP *src)
{
    int i;

    extmap_reset(dst);
    for (i = 0; i < src->n; i++)
        if (!extmap_add(dst, src->ext[i].vcn, src->ext[i].lcn,
                        src->ext[i].count))
            return 0;
    return 1;
}

fm_status fm_get_extents(fm_control control, void *ctx, void *file, EXTMAP *m)
{
    const unsigned size = 16 + 1024 * 16;
    unsigned char *out = malloc(size);
    long long requested = 0;
    fm_status result = FM_SUCCESS;
    extmap_reset(m);
    if (!out)
        return FM_NOMEM;
    for (;;) {
        unsigned got = 0, count, i;
        long long previous;
        fm_status status;
        memset(out, 0, 32);
        status = control(ctx, file, FM_RETRIEVAL, &requested, sizeof(requested),
                          out, size, &got);
        if (status == FM_EOF)
            break;
        if (status != FM_SUCCESS && status != FM_OVERFLOW) {
            result = status;
            break;
        }
        count = fm_u32(out);
        previous = (long long)fm_u64(out + 8);
        if (got < 16 || got > size || !count || count > 1024 ||
            count > (got - 16) / 16 || previous < 0 || previous > requested) {
            result = FM_BAD_REPLY;
            break;
        }
        for (i = 0; i < count; i++) {
            long long next = (long long)fm_u64(out + 16 + i * 16);
            long long lcn = (long long)fm_u64(out + 24 + i * 16);
            long long begin = previous < requested ? requested : previous;
            if (next <= previous || lcn < -1 ||
                (lcn >= 0 && next - previous > 0x7FFFFFFFFFFFFFFFLL - lcn)) {
                result = FM_BAD_REPLY;
                goto done;
            }
            if (next > requested &&
                !extmap_add(m, begin, lcn < 0 ? -1 : lcn + (begin - previous), next - begin)) {
                result = FM_NOMEM;
                goto done;
            }
            previous = next;
        }
        if (previous <= requested) {
            result = FM_BAD_REPLY;
            break;
        }
        if (status == FM_SUCCESS)
            break;
        requested = previous;
    }
done:
    free(out);
    if (result != FM_SUCCESS)
        extmap_reset(m);
    return result;
}

fm_status fm_get_bitmap(fm_control control, void *ctx, void *volume,
                        unsigned chunk, unsigned long long maximum,
                        unsigned char **bitmap, unsigned long long *clusters)
{
    unsigned char *out, *map = NULL;
    unsigned long long total = 0, next = 0;
    fm_status status = FM_BAD_REPLY;
    if (!chunk || chunk > 1024 * 1024)
        return FM_INVALID_PARAMETER;
    out = malloc(16 + chunk);
    if (!out)
        return FM_NOMEM;
    for (;;) {
        unsigned got = 0;
        unsigned long long start, remaining, bits;
        memset(out, 0, 16);
        status = control(ctx, volume, FM_BITMAP, &next, sizeof(next), out, 16 + chunk, &got);
        if (status != FM_SUCCESS && status != FM_OVERFLOW)
            break;
        start = fm_u64(out);
        remaining = fm_u64(out + 8);
        if (got <= 16 || got > 16 + chunk || !remaining ||
            remaining > 0x7FFFFFFFFFFFFFFFULL || start > 0x7FFFFFFFFFFFFFFFULL) {
            status = FM_BAD_REPLY;
            break;
        }
        if (!map) {
            total = remaining;
            if ((maximum && total > maximum) || total > ~0ULL - 7 ||
                (total + 7) / 8 > (size_t)-1) {
                status = FM_BAD_REPLY;
                break;
            }
            map = malloc((size_t)((total + 7) / 8));
            if (!map) {
                status = FM_NOMEM;
                break;
            }
            memset(map, 0, (size_t)((total + 7) / 8));
        }
        if (start != (next & ~7ULL) || start >= total || remaining != total - start) {
            status = FM_BAD_REPLY;
            break;
        }
        bits = (unsigned long long)(got - 16) * 8;
        if (bits > total - start)
            bits = total - start;
        memcpy(map + start / 8, out + 16, (size_t)((bits + 7) / 8));
        next = start + bits;
        if (status == FM_SUCCESS && next < total) {
            status = FM_BAD_REPLY;
            break;
        }
        if (next >= total) {
            unsigned long long bit;
            for (bit = total; bit < ((total + 7) & ~7ULL); bit++)
                map[bit / 8] |= (unsigned char)(1U << (bit & 7));
            *bitmap = map; *clusters = total;
            free(out);
            return FM_SUCCESS;
        }
    }
    free(map); free(out);
    return status;
}

void fm_mark(unsigned char *map, unsigned long long total,
              unsigned long long start, unsigned long long count, int used)
{
    unsigned long long i, end;
    if (!map || start >= total)
        return;
    if (count > total - start)
        count = total - start;
    end = start + count;
    i = start;
    while (i < end && (i & 7)) {
        unsigned char bit = (unsigned char)(1U << (i & 7));
        if (used)
            map[i / 8] |= bit;
        else
            map[i / 8] &= (unsigned char)~bit;
        i++;
    }
    if (end - i >= 8) {
        unsigned long long bytes = (end - i) / 8;
        memset(map + i / 8, used ? 0xFF : 0x00, (size_t)bytes);
        i += bytes * 8;
    }
    while (i < end) {
        unsigned char bit = (unsigned char)(1U << (i & 7));
        if (used)
            map[i / 8] |= bit;
        else
            map[i / 8] &= (unsigned char)~bit;
        i++;
    }
}

int fm_find_best(const unsigned char *map, unsigned long long total,
                 unsigned long long need, unsigned long long minimum,
                 unsigned long long *target, unsigned long long *available)
{
    unsigned long long i = minimum, begin = 0, length = 0, best = 0;
    if (!map || !need || minimum >= total)
        return 0;
    for (;;) {
        if (i < total && !(map[i / 8] & (1U << (i & 7)))) {
            if (!length)
                begin = i;
            if (!(i & 7)) {
                if (total - i >= 64 && !fm_u64(map + i / 8)) {
                    length += 64;
                    i += 64;
                    continue;
                }
                if (total - i >= 32 && !fm_u32(map + i / 8)) {
                    length += 32;
                    i += 32;
                    continue;
                }
                if (total - i >= 8 && !map[i / 8]) {
                    length += 8;
                    i += 8;
                    continue;
                }
            }
            length++;
            i++;
            continue;
        }
        if (length && ((!best) ||
            (length >= need && (best < need || length < best)) ||
            (length < need && best < need && length > best))) {
            best = length;
            *target = begin;
        }
        length = 0;
        if (best == need || i == total)
            break;
        if (!(i & 7)) {
            if (total - i >= 64 && fm_u64(map + i / 8) == ~0ULL) {
                i += 64;
                continue;
            }
            if (total - i >= 32 && fm_u32(map + i / 8) == 0xFFFFFFFFU) {
                i += 32;
                continue;
            }
            if (total - i >= 8 && map[i / 8] == 0xFF) {
                i += 8;
                continue;
            }
        }
        i++;
    }
    *available = best;
    return best != 0;
}

/* Consolidate complete physical fragments only: never split an existing run
 * merely to fit a smaller hole. Internal boundaries must obey NT4 granularity. */
static int partial_range(const EXTMAP *m, unsigned long long align,
                          unsigned long long available,
                          long long *base, long long *end)
{
    int i, first = 0, fragments = 0;
    for (i = 0; i < m->n; i++) {
        long long finish = m->ext[i].vcn + m->ext[i].count;
        int boundary = i + 1 == m->n || m->ext[i + 1].lcn != m->ext[i].lcn + m->ext[i].count;
        if (!boundary)
            continue;
        fragments++;
        while (first <= i &&
               ((unsigned long long)(finish - m->ext[first].vcn) > available ||
                (unsigned long long)m->ext[first].vcn % align)) {
            int last = first++;
            while (first <= i &&
                   m->ext[first].lcn == m->ext[last].lcn + m->ext[last].count)
                last = first++;
            fragments--;
        }
        if (first <= i && fragments >= 2 &&
            (i + 1 == m->n || !((unsigned long long)finish % align))) {
            *base = m->ext[first].vcn;
            *end = finish;
            return 1;
        }
    }
    return 0;
}

static int valid_move_map(const EXTMAP *m, unsigned long long clusters)
{
    long long next = 0;
    int i;
    for (i = 0; i < m->n; i++) {
        const EXTENT *e = m->ext + i;
        if (e->vcn != next || e->lcn < 0 || e->count <= 0 ||
            e->count > 0x7FFFFFFFFFFFFFFFLL - next ||
            (unsigned long long)e->lcn >= clusters ||
            (unsigned long long)e->count > clusters - (unsigned long long)e->lcn)
            return 0;
        next += e->count;
    }
    return 1;
}

/*
 * A move frees the clusters the data left behind. Compare the layout from
 * before the move with the layout read back afterwards and release every
 * cluster whose LCN changed; clusters that stayed put are still owned by
 * the file. The new location is marked used, so the cached bitmap keeps up
 * with the volume instead of counting the vacated space as occupied for
 * the rest of the run.
 */
static void release_vacated(fm_volume *v, const EXTMAP *before,
                             const EXTMAP *after, long long base, long long end)
{
    long long vcn = base;
    int i = 0, j = 0;

    while (vcn < end) {
        const EXTENT *o, *w;
        long long stop, oldLcn, newLcn;

        while (i < before->n &&
               before->ext[i].vcn + before->ext[i].count <= vcn)
            i++;
        while (j < after->n &&
               after->ext[j].vcn + after->ext[j].count <= vcn)
            j++;
        if (i >= before->n || j >= after->n)
            break;
        o = &before->ext[i];
        w = &after->ext[j];
        stop = end;
        if (o->vcn + o->count < stop)
            stop = o->vcn + o->count;
        if (w->vcn + w->count < stop)
            stop = w->vcn + w->count;
        oldLcn = o->lcn + (vcn - o->vcn);
        newLcn = w->lcn + (vcn - w->vcn);
        if (oldLcn >= 0 && newLcn >= 0 && oldLcn != newLcn) {
            unsigned long long count = (unsigned long long)(stop - vcn);

            if (v->unmark)
                v->unmark(v->ctx, (unsigned long long)oldLcn, count);
            v->mark(v->ctx, (unsigned long long)newLcn, count);
        }
        vcn = stop;
    }
}

/* Give back the part of the reserved range that the move never reached: the
 * data was not written there, so those clusters are still free. */
static void release_tail(fm_volume *v, unsigned long long target,
                          unsigned long long written,
                          unsigned long long reserved)
{
    if (written >= reserved)
        return;
    if (v->unmark)
        v->unmark(v->ctx, target + written, reserved - written);
}

/*
 * The reserved range may hold less data than it was given: the move can
 * fail half way through, and a driver that commits a move without saying
 * so leaves the map as the only truth. The map read back afterwards shows
 * where the data really is; every cluster of the reserved range the file
 * does not occupy is free again.
 *
 * The pieces inside the range are kept sorted and merged as they are
 * collected, so touching extents count as one piece. If more than
 * FM_PIECES separate pieces remain, nothing is given back: the whole range
 * stays reserved. Releasing only part of it would risk handing out a
 * cluster that holds data, and a range kept reserved for the rest of the
 * run only costs free space.
 */
#define FM_PIECES 16

static void release_unwritten(fm_volume *v, const EXTMAP *m,
                               unsigned long long target,
                               unsigned long long reserved)
{
    unsigned long long end = target + reserved, lcn = target;
    unsigned long long lo[FM_PIECES], hi[FM_PIECES];
    int i, j, k, n = 0;

    if (!v->unmark || !reserved)
        return;
    for (i = 0; i < m->n; i++) {
        long long begin = m->ext[i].lcn, finish = m->ext[i].lcn + m->ext[i].count;
        unsigned long long a, b;

        if (begin < 0 || finish <= (long long)target ||
            begin >= (long long)end)
            continue;
        a = (unsigned long long)(begin > (long long)target ? begin
                                                           : (long long)target);
        b = (unsigned long long)(finish > (long long)end ? (long long)end
                                                         : finish);
        /* lo/hi are sorted, separate and not touching. Find the first
         * piece that touches or follows [a, b), then fold in every piece
         * that touches or overlaps it. */
        for (j = 0; j < n && hi[j] < a; j++)
            ;
        for (k = j; k < n && lo[k] <= b; k++) {
            if (lo[k] < a)
                a = lo[k];
            if (hi[k] > b)
                b = hi[k];
        }
        if (k == j) {
            if (n == FM_PIECES)
                return; /* too many pieces: keep the whole range reserved */
            for (k = n; k > j; k--) {
                lo[k] = lo[k - 1];
                hi[k] = hi[k - 1];
            }
            n++;
        } else if (k > j + 1) {
            int gone = k - j - 1;

            for (k = j + 1; k + gone < n; k++) {
                lo[k] = lo[k + gone];
                hi[k] = hi[k + gone];
            }
            n -= gone;
        }
        lo[j] = a;
        hi[j] = b;
    }
    for (i = 0; i < n; i++) {
        if (lo[i] > lcn)
            v->unmark(v->ctx, lcn, lo[i] - lcn);
        lcn = hi[i];
    }
    if (lcn < end)
        v->unmark(v->ctx, lcn, end - lcn);
}

reloc_result fm_relocate(fm_volume *v, void *file, EXTMAP *m, fm_status *status)
{
    unsigned long long align = v->moveAlign ? v->moveAlign : 1;
    unsigned long long chunk = v->bytesPerCluster ? 1048576 / v->bytesPerCluster : align;
    unsigned long long minimum = 0;
    int attempt, retries = 0, original = extmap_fragments(m), refused = 0;
    EXTMAP before;
    reloc_result outcome = RELOC_FAILED;

    extmap_init(&before);
    *status = FM_SUCCESS;
    if (v->unsupported) { *status = FM_NOT_SUPPORTED; outcome = RELOC_UNSUPPORTED; goto done; }
    if (!v->alive(v->ctx)) { *status = FM_NOMEM; goto done; }
    if (!m->n || m->realClusters <= 0 || m->hasVirtual) { outcome = RELOC_SKIPPED; goto done; }
    if (!v->bytesPerCluster || v->bytesPerCluster > 65536 ||
        align > 65536 || (align & (align - 1)) ||
        !v->clusters || v->clusters > 0x7FFFFFFFFFFFFFFFULL) {
        *status = FM_INVALID_PARAMETER;
        goto done;
    }
    if (!valid_move_map(m, v->clusters)) {
        *status = FM_BAD_REPLY;
        goto done;
    }
    if (!v->haveBitmap) { outcome = RELOC_NO_SPACE; goto done; }
    if (chunk < align)
        chunk = align;
    chunk -= chunk % align;
    for (attempt = 0; attempt < 4; attempt++) {
        long long first = m->ext[0].vcn, base = first - first % (long long)align;
        long long end = m->ext[m->n - 1].vcn + m->ext[m->n - 1].count, vcn;
        unsigned long long dataLen = (unsigned long long)(end - base);
        unsigned long long need = (dataLen + align - 1) / align * align;
        unsigned long long target = 0, available = 0, written = 0;
        int partial = 0;
        fm_status moved = FM_SUCCESS, mapped;
        if (!v->find(v->ctx, need, minimum, &target, &available) || available < need) {
            mapped = v->refresh(v->ctx);
            if (mapped != FM_SUCCESS) {
                *status = mapped;
                goto done;
            }
            if (!v->find(v->ctx, need, minimum, &target, &available)) {
                if (refused) { *status = FM_INVALID_PARAMETER; goto done; }
                outcome = RELOC_NO_SPACE;
                goto done;
            }
            if (available < need) {
                if (refused) { *status = FM_INVALID_PARAMETER; goto done; }
                if (!partial_range(m, align, available, &base, &end)) {
                    outcome = RELOC_NO_SPACE;
                    goto done;
                }
                dataLen = (unsigned long long)(end - base);
                need = (dataLen + align - 1) / align * align;
                if (dataLen > available) { outcome = RELOC_NO_SPACE; goto done; }
                partial = 1;
            }
        }
        if (target >= v->clusters || dataLen > v->clusters - target) {
            *status = FM_BAD_REPLY;
            goto done;
        }
        /* The source VCN is aligned to the record size; the destination LCN
         * has to be aligned as well, or NTFS refuses the move. Use the first
         * aligned position in the free run when it still holds the data;
         * otherwise keep the start of the run, which NTFS may still take. */
        if (align > 1 && target % align) {
            unsigned long long aligned =
                (target + align - 1) / align * align;

            if (aligned < v->clusters && dataLen <= v->clusters - aligned &&
                dataLen <= target + available - aligned)
                target = aligned;
        }
        if (target >= v->clusters || dataLen > v->clusters - target) {
            *status = FM_BAD_REPLY;
            goto done;
        }
        if (!extmap_copy(&before, m)) { *status = FM_NOMEM; goto done; }
        /* Only the clusters that will really hold data are reserved: the
         * range is rounded up to the alignment, but the tail of it stays
         * free and can serve another file. */
        v->mark(v->ctx, target, dataLen);
        for (vcn = base; vcn < end;) {
            unsigned long long take = (unsigned long long)(end - vcn);
            if (take > chunk)
                take = chunk;
            if (!v->alive(v->ctx)) {
                release_tail(v, target, written, dataLen);
                *status = FM_NOMEM;
                goto done;
            }
            moved = v->move(v->ctx, file, vcn, target + (unsigned long long)(vcn - base), (unsigned)take);
            if (moved != FM_SUCCESS)
                break;
            vcn += (long long)take;
            written += take;
        }
        if (moved == FM_INVALID_REQUEST || moved == FM_NOT_IMPLEMENTED || moved == FM_NOT_SUPPORTED) {
            release_tail(v, target, written, dataLen);
            *status = moved; outcome = RELOC_UNSUPPORTED; goto done;
        }
        mapped = v->map(v->ctx, file, m);
        if (mapped != FM_SUCCESS) {
            /* What the driver did is unknown: keep the range reserved
             * rather than hand out clusters that may hold data. */
            *status = mapped;
            goto done;
        }
        if (!m->n) {
            release_unwritten(v, m, target, dataLen);
            outcome = RELOC_SKIPPED;
            goto done;
        }
        if (m->hasVirtual) {
            release_unwritten(v, m, target, dataLen);
            outcome = RELOC_SKIPPED;
            goto done;
        }
        if (!valid_move_map(m, v->clusters)) {
            *status = FM_BAD_REPLY;
            goto done;
        }
        release_vacated(v, &before, m, base, end);
        if (moved == FM_INVALID_PARAMETER) {
            /* The place is given up: it stays reserved. */
            refused = 1;
            if (v->exclude)
                v->exclude(v->ctx, target, need);
            minimum = (target + need + align - 1) / align * align;
            if (minimum <= target)
                minimum = target + need;
            /* If this was the last attempt, the refusal is the result. */
            *status = moved;
            continue;
        }
        release_unwritten(v, m, target, dataLen);
        if (extmap_fragments(m) <= 1) {
            *status = FM_SUCCESS;
            outcome = RELOC_MOVED;
            goto done;
        }
        if (partial && moved == FM_SUCCESS && extmap_fragments(m) < original) {
            *status = FM_SUCCESS;
            outcome = RELOC_PARTIAL;
            goto done;
        }
        if (moved == FM_SUCCESS)
            moved = FM_NOT_MOVED;
        *status = moved;
        if (attempt == 3)
            break;
        if (moved == FM_ALREADY_COMMITTED || moved == FM_NOT_MOVED) {
            if (++retries >= 2) {
                if (moved == FM_ALREADY_COMMITTED)
                    v->pause(v->ctx, 3000);
                mapped = v->refresh(v->ctx);
                if (mapped != FM_SUCCESS) {
                    *status = mapped;
                    goto done;
                }
            }
            continue;
        }
        break;
    }
done:
    extmap_free(&before);
    return outcome;
}

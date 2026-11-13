/*
 * ntfsrecord.c - Validation of untrusted NTFS FILE records.
 *
 * Applies and checks the update-sequence (fixup) array, validates record
 * and attribute headers and finds attributes, including through an
 * attribute list. Every offset and length read from disk is bounds-checked
 * before use and every attribute walk advances, so a corrupt record is
 * reported as malformed instead of being read past its end. Also provides
 * the CRC-32 used by the boot result record. No OS calls: shared by the
 * Win32 side, the native helper and the host tests.
 */
#include "ntfsrecord.h"

typedef unsigned char u8;

unsigned ntfs_crc32_update(unsigned crc, const void *data, size_t size)
{
    static unsigned table[256];
    static int initialized;
    const u8 *p = data;
    unsigned i, j;
    if (!initialized) {
        for (i = 0; i < 256; i++) {
            unsigned c = i;
            for (j = 0; j < 8; j++)
                c = (c >> 1) ^ ((c & 1) ? 0xEDB88320U : 0);
            table[i] = c;
        }
        initialized = 1;
    }
    while (size--)
        crc = table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return crc;
}

static unsigned r16(const u8 *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned r32(const u8 *p)
{
    return r16(p) | (r16(p + 2) << 16);
}

static void w16(u8 *p, unsigned value)
{
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
}

static int usa_valid(const u8 *p, size_t size)
{
    unsigned offset, count, first;
    if (!p || size < 512 || size > 65536 || size % 512)
        return 0;
    offset = r16(p + 4);
    count = r16(p + 6);
    first = r16(p + 0x14);
    return offset >= 0x2A && !(offset & 1) &&
           count == size / 512 + 1 &&
           offset + count * 2 <= 510 &&
           offset + count * 2 <= first && first <= size;
}

int ntfs_record_unprotect(void *record, size_t size)
{
    u8 *p = record;
    unsigned offset, count, i;
    if (!usa_valid(p, size))
        return 0;
    offset = r16(p + 4);
    count = r16(p + 6);
    for (i = 1; i < count; i++)
        if (r16(p + i * 512 - 2) != r16(p + offset))
            return 0;
    for (i = 1; i < count; i++)
        w16(p + i * 512 - 2, r16(p + offset + i * 2));
    return 1;
}

int ntfs_record_protect(void *record, size_t size)
{
    u8 *p = record;
    unsigned offset, count, i, usn;
    if (!usa_valid(p, size))
        return 0;
    offset = r16(p + 4);
    count = r16(p + 6);
    usn = (r16(p + offset) + 1) & 0xFFFF;
    if (!usn || usn == 0xFFFF)
        usn = 1;
    for (i = 1; i < count; i++) {
        w16(p + offset + i * 2, r16(p + i * 512 - 2));
        w16(p + i * 512 - 2, usn);
    }
    w16(p + offset, usn);
    return 1;
}

long ntfs_record_attr(const void *record, size_t size, unsigned wanted,
                     int nonresident, int unnamed, int *hasAttrList)
{
    const u8 *p = record;
    unsigned offset, used;
    long found = -1;
    if (hasAttrList)
        *hasAttrList = 0;
    if (!usa_valid(p, size) || p[0] != 'F' || p[1] != 'I' ||
        p[2] != 'L' || p[3] != 'E' || r32(p + 0x1C) != size)
        return -2;
    offset = r16(p + 0x14);
    used = r32(p + 0x18);
    if (offset < 0x30 || (offset & 7) || used > size ||
        offset > used || used - offset < 4)
        return -2;
    while (used - offset >= 4) {
        unsigned type = r32(p + offset), length, header, name, namesize;
        int nr;
        if (type == 0xFFFFFFFFU)
            return found;
        if (used - offset < 16)
            return -2;
        length = r32(p + offset + 4);
        nr = p[offset + 8];
        header = nr ? 64 : 24;
        if (nr > 1 || length < header || (length & 7) || length > used - offset)
            return -2;
        if (nr && ((r16(p + offset + 12) & 0x8001) || r16(p + offset + 34))) {
            header = 72;
            if (length < header)
                return -2;
        }
        name = r16(p + offset + 10);
        namesize = (unsigned)p[offset + 9] * 2;
        if (namesize && (name < header || name > length ||
                         namesize > length - name))
            return -2;
        if (nr) {
            unsigned mp = r16(p + offset + 32);
            if (mp < header || mp >= length ||
                (namesize && name + namesize > mp))
                return -2;
        } else {
            unsigned value = r16(p + offset + 20);
            unsigned bytes = r32(p + offset + 16);
            if (value < header || value > length || bytes > length - value ||
                (namesize && name + namesize > value))
                return -2;
        }
        if (type == 0x20 && hasAttrList)
            *hasAttrList = 1;
        if (type == wanted && found < 0 && (!nonresident || nr) &&
            (!unnamed || !namesize))
            found = (long)offset;
        offset += length;
    }
    return -2;
}

int ntfs_record_valid(const void *record, size_t size)
{
    return ntfs_record_attr(record, size, 0, 0, 0, NULL) != -2;
}

#ifndef HSDEFRAG_NTFSRECORD_H
#define HSDEFRAG_NTFSRECORD_H

#include <stddef.h>

unsigned ntfs_crc32_update(unsigned crc, const void *data, size_t size);
int ntfs_record_unprotect(void *record, size_t size);
int ntfs_record_protect(void *record, size_t size);
int ntfs_record_valid(const void *record, size_t size);
/* -1: absent, -2: malformed record. Searches the entire attribute list. */
long ntfs_record_attr(const void *record, size_t size, unsigned type,
                     int nonresident, int unnamed, int *hasAttrList);

#endif

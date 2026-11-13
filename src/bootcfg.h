#ifndef HSDEFRAG_BOOTCFG_H
#define HSDEFRAG_BOOTCFG_H

#include <stddef.h>

int bootcfg_parse(const wchar_t *command, size_t chars, wchar_t *drive,
                  int *dryRun, int *writeAuthorized, int *full);
/* Validates a complete MULTI_SZ; preserves unrelated entries byte-for-byte.
 * Removes entries for drive, then appends add (NULL means cancellation).
 * Returns an allocated result, or NULL for malformed input/allocation failure.
 */
wchar_t *bootcfg_edit(const wchar_t *data, size_t chars, wchar_t drive,
                      const wchar_t *add, size_t *outChars);

#endif

#ifndef HSDEFRAG_FILEMOVE_H
#define HSDEFRAG_FILEMOVE_H

#include <stddef.h>

typedef int fm_status;
typedef char fm_status_size[sizeof(fm_status) == 4 ? 1 : -1];
#define FM_SUCCESS        ((fm_status)0)
#define FM_OVERFLOW       ((fm_status)0x80000005U)
#define FM_NOT_IMPLEMENTED ((fm_status)0xC0000002U)
#define FM_INVALID_PARAMETER ((fm_status)0xC000000DU)
#define FM_INVALID_REQUEST ((fm_status)0xC0000010U)
#define FM_EOF            ((fm_status)0xC0000011U)
#define FM_ALREADY_COMMITTED ((fm_status)0xC0000021U)
#define FM_NOMEM          ((fm_status)0xC000009AU)
#define FM_NOT_SUPPORTED  ((fm_status)0xC00000BBU)
#define FM_NOT_MOVED      ((fm_status)0xE0000001U)
#define FM_BAD_REPLY      ((fm_status)0xE0000002U)
#define FM_BITMAP         0x0009006FU
#define FM_RETRIEVAL      0x00090073U
#define FM_MOVE           0x00090074U
/* Streams the move engine leaves alone (FILE_ATTRIBUTE_* bits): NT 4.0's
 * FSCTL_MOVE_FILE does not handle these reliably, or they are not data. */
#define FM_SPECIAL_ATTRS  0x00004E40UL /* device, sparse, reparse, compressed, encrypted */

typedef struct {
    long long vcn, lcn, count;
} EXTENT;

typedef struct {
    EXTENT *ext;
    int n, cap;
    long long realClusters;
    int hasVirtual;
} EXTMAP;

typedef fm_status (*fm_control)(void *ctx, void *handle, unsigned code,
                               void *in, unsigned inBytes, void *out,
                               unsigned outBytes, unsigned *returned);

void extmap_init(EXTMAP *map);
void extmap_reset(EXTMAP *map);
void extmap_free(EXTMAP *map);
int extmap_add(EXTMAP *map, long long vcn, long long lcn, long long count);
int extmap_fragments(const EXTMAP *map);
fm_status fm_get_extents(fm_control control, void *ctx, void *file, EXTMAP *map);
fm_status fm_get_bitmap(fm_control control, void *ctx, void *volume,
                        unsigned chunkBytes, unsigned long long maxClusters,
                        unsigned char **bitmap, unsigned long long *clusters);
/* Smallest sufficient free run, otherwise the largest available run.
 * A successful search can return *available < need. */
int fm_find_best(const unsigned char *bitmap, unsigned long long clusters,
                 unsigned long long need, unsigned long long minimum,
                 unsigned long long *target, unsigned long long *available);
void fm_mark(unsigned char *bitmap, unsigned long long clusters,
              unsigned long long start, unsigned long long count, int used);

typedef enum {
    RELOC_MOVED, RELOC_PARTIAL, RELOC_SKIPPED, RELOC_NO_SPACE,
    RELOC_FAILED, RELOC_UNSUPPORTED
} reloc_result;

typedef struct {
    void *ctx;
    unsigned long long bytesPerCluster, moveAlign, clusters;
    int haveBitmap, unsupported;
    int (*find)(void *, unsigned long long, unsigned long long,
                unsigned long long *, unsigned long long *);
    void (*mark)(void *, unsigned long long, unsigned long long);
    void (*unmark)(void *, unsigned long long, unsigned long long);
    void (*exclude)(void *, unsigned long long, unsigned long long);
    fm_status (*refresh)(void *);
    fm_status (*map)(void *, void *, EXTMAP *);
    fm_status (*move)(void *, void *, long long, unsigned long long, unsigned);
    void (*pause)(void *, unsigned);
    int (*alive)(void *);
} fm_volume;

reloc_result fm_relocate(fm_volume *volume, void *file, EXTMAP *map, fm_status *status);

#endif

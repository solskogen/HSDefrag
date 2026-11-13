/* mft.h - Read-only NTFS metadata fragmentation reporting. */
#ifndef HSDEFRAG_MFT_H
#define HSDEFRAG_MFT_H

#include "volume.h"

/* Analyze the MFT's fragmentation using ntfsmft_analyze. Never moves data. */
int mft_report(VOLUME *v);

#endif /* HSDEFRAG_MFT_H */

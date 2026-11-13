#ifndef HSDEFRAG_SCHEDULE_H
#define HSDEFRAG_SCHEDULE_H

#include "volume.h"

int cmd_boot_schedule(VOLUME *volume, int dryRun);
int cmd_boot_cancel(wchar_t drive);
int cmd_boot_status(wchar_t drive);

#endif

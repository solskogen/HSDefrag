#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bootlog.h"

static unsigned char data[BOOTLOG_BYTES];
static int failWrite, failFlush, failRead, torn;
static int rd(void *ctx, ntfs_u64 off, void *buf, ntfs_u32 len)
{
    (void)ctx;
    assert(!off && len == sizeof(data));
    memcpy(buf, data, len);
    return !failRead;
}
static int wr(void *ctx, ntfs_u64 off, const void *buf, ntfs_u32 len)
{
    (void)ctx;
    assert(!off && len == sizeof(data));
    if (failWrite) return 0;
    memcpy(data, buf, torn ? len / 8 : len);
    return 1;
}
static int flush(void *ctx) { (void)ctx; return !failFlush; }
int main(void)
{
    bootlog_io io = {NULL, rd, wr, flush, BOOTLOG_BYTES};
    boot_record r;
    assert(bootlog_create(&io, 'C', 42));
    assert(!bootlog_create(&io, 'C', 43));
    assert(bootlog_read(&io, &r) && r.result == BOOT_PENDING && r.jobId == 42);
    r.result = BOOT_SUCCESS;
    assert(!bootlog_write(&io, &r));
    r.phase = BOOT_FINISH; r.files.result = FILES_COMPLETE;
    r.files.files = 0x100000001ULL; r.files.dataBytes = 123;
    assert(bootlog_write(&io, &r));
    assert(bootlog_read(&io, &r) && r.files.files == 0x100000001ULL);
    r.result = BOOT_FAILED; r.files.result = FILES_PARTIAL;
    r.files.reduced = r.files.noSpace = r.files.fragmented = 5;
    assert(bootlog_write(&io, &r));
    assert(bootlog_read(&io, &r) && r.files.reduced == 5 &&
           r.files.noSpace == 5 && r.files.result == FILES_PARTIAL);
    data[12] ^= 1; assert(!bootlog_read(&io, &r)); data[12] ^= 1;
    assert(bootlog_read(&io, &r));
    failWrite = 1; assert(!bootlog_write(&io, &r)); failWrite = 0;
    failFlush = 1; assert(!bootlog_write(&io, &r)); failFlush = 0;
    failRead = 1; assert(!bootlog_read(&io, &r)); failRead = 0;
    r.files.dataBytes = 456; torn = 1;
    assert(!bootlog_write(&io, &r)); torn = 0;
    assert(!bootlog_read(&io, &r));
    io.length++; assert(!bootlog_read(&io, &r));
    puts("Standalone boot-result CRC/bounds/I/O regressions passed.");
    return 0;
}

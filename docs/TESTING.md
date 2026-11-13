# Automated tests

```sh
make test            # test-core + test-wine
make test-core       # host tests (ASan/UBSan); no DJGPP or Wine needed
make test-wine       # Windows tests under Wine; builds everything (needs DJGPP)
make test-floppy     # build/test-floppy.img, a fragmented floppy for trying hsdefrag by hand
make check-imports   # also run by test-wine
make check-dos-insns # also run by test-wine
make test-mkdual     # also run by test-wine
```

None of them moves real disk data or edits the real registry, System32 or
AUTOEXEC.BAT. They do not certify NT4 storage-driver durability, real boot
behaviour or real DOS machines, and the MS-DOS side itself is not run: that is what [QUALIFY.md](QUALIFY.md) is
for.

## Host tests (`make test-core`)

Built with the host compiler under AddressSanitizer and UBSan:

- **FAT engine** (`tests/fat.c`): random FAT12/16/32 images with fragmented
  files and folders are defragmented and checked by an independent fsck (data
  intact, `.`/`..` correct, free space consolidated, no lost chain leading
  into a live one, analysis predictions equal to the results), also with
  injected I/O failures and with files changed by others between write bursts.
  Malformed BPBs and long-name entries, incomplete scans, mismatched FAT
  mirrors, and explicit failures or silently dropped writes at every write
  boundary of folder moves are covered. These simulate whole-sector writes,
  not torn sectors, cache reordering or power loss.
- **Report text** (`tests/report.c`): the FAT report lines of both front ends.
- **NTFS parsing** (`tests/core.c`): record validation, `$MFT` analysis and
  BootExecute editing.
- **Move engine** (`tests/move_core.c`): vacated clusters are given back, a
  refused target is given up on its own while the free space around it stays
  available, data lands on an aligned place, clusters a moved file occupies
  are never released.
- **Boot result record** (`tests/bootlog.c`): CRC, bounds and I/O failures.
- **Command line** (`tests/argv.c`): argument splitting and allocation
  failures.
- **9x job file and AUTOEXEC.BAT editing** (`tests/bootjob.c`), shared with
  the MS-DOS side.
- **MS-DOS interfaces** (`tests/diskio.c`): the disk I/O packet, drive
  numbers and the form-selecting `CX=FFFFh`, and the decoding of DOS's system
  file table and current directory structure entries (files open for writing,
  files to keep in place, SUBST and JOIN drives), since nothing else checks
  them.

## Wine tests (`make test-wine`)

- NT online pass: volume traversal, stream mapping, move verification,
  allocation and I/O failures, exit codes, Ctrl+C.
- NT scheduling and the native helper's startup and boot traversal, with
  mocked registry, files and FSCTLs.
- The Windows 9x layer against a simulated VWIN32: volume locks (categories
  48h and 08h), files in use, the lock flag, nothing printed and nobody waiting
  under a level 3 lock, failed unlocks, unresolvable files in use, and the
  fallback to the classic `Int 25h/26h`.
- 9x boot scheduling against an in-memory file system: copying `hsdefrag.exe`
  into place, the job file and the AUTOEXEC.BAT line, cancelling, the
  report, rollback when a write fails, retained recovery files, failed
  rollback and preservation of unrelated AUTOEXEC.BAT lines.
- Command lines that must be rejected (`tests/wine.sh`).
- `check-imports`: the console program imports KERNEL32 only, the native
  helper NTDLL only, and neither contains `cmov`, SSE or x87 instructions.
- `check-dos-insns`: the MS-DOS image holds only i486 code and no libgcc
  unwinder.
- `test-mkdual`: merges the Windows image and the stub with a stand-in COFF
  image and checks the layout of the result.

## A test floppy for trying hsdefrag by hand (`make test-floppy`)

`make test-floppy` writes `build/test-floppy.img`: the same badly fragmented
1.44 MB FAT12 floppy (dummy files, made with mtools only: 400 one-cluster
spacer files, every other one deleted, then files and a folder written into
the holes, about 200 fragments). Write it to a real floppy and run
`hsdefrag a: -a` and `hsdefrag a: -d` on a DOS or Windows 9x machine (see
[QUALIFY.md](QUALIFY.md)).

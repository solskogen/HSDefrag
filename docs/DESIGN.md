# How House Soft Defrag works

This is the background to [README.md](../README.md): how each engine moves
data, the rules that keep a volume consistent, and what was deliberately
left out. Developers: `.claude/skills/hsdefrag-dev/SKILL.md` has the source
map and the coding rules.

## One file for three systems

`hsdefrag.exe` is a PE32 console program for Windows (subsystem 4.0, imports
KERNEL32 only) with an MS-DOS program behind its MS-DOS header.
`tools/mkdual.c` (built and run by `make`) merges the Windows image
`build/hsdefrag-win.exe` with the CWSDPMI stub in `dos/` and the DJGPP-built
COFF image `build/dos/hsdefrag`. Both go between the PE headers and the first
PE section: MS-DOS runs the stub, which loads the COFF image from the same
file, while Windows sees an ordinary PE file.

The linker writes the COFF image directly (`--oformat coff-go32`); a linker
that does not know the option needs `exe2coff`, and the build falls back to
it. Any DJGPP works, i386 to i686: `fatdos.c` replaces the two functions
through which `crt0` would pull libgcc's exception unwinder (built with
Pentium Pro instructions in an i686 DJGPP) into the image, and
`make check-dos-insns` verifies that the MS-DOS image holds only i486 code.
The stub (`dos/cwsdstub.exe`) is CWSDPMI's official `CWSDSTUB.EXE` with its
swap file disabled, so CWSDPMI never writes a swap file on the drive being
defragmented (see `dos/README`).

The Windows program has its own minimal C runtime (`nostd.c`, `util.c`); it
needs no CRT DLL. The NT scheduler loads ADVAPI32 at run time. The native
boot helper `hsdefrgb.exe` imports NTDLL only and is embedded in the Windows
program. No part of the program uses floating point.

At startup the Windows side checks the platform (`GetVersion`) and runs the
NT engine or the FAT engine.

## Windows NT: filesystem-managed moves

The NT engine never writes to the disk itself. It reads the volume bitmap
(`FSCTL_GET_VOLUME_BITMAP`) and each file's extents
(`FSCTL_GET_RETRIEVAL_POINTERS`) and asks the filesystem to move clusters
(`FSCTL_MOVE_FILE`), calling `NtFsControlFile` directly so that the exact
NTSTATUS is known. The same engine (`filemove.c`) serves the online pass and
the boot pass.

Static inspection of the supplied Contig 1.83 informed this independently
implemented allocator; no Contig code or binary is included. Its useful
behaviours are best-fit free-run selection, fallback to smaller holes, and
filesystem-managed moves rather than raw disk rewriting. Contig's modern
`OpenFileById` metadata path is not imported or emulated: opening metadata
differently does not give NT4's NTFS driver the ability to relocate it.

- **Placement.** The engine first tries the smallest free run that fits the
  whole stream. If that is impossible even after refreshing the bitmap, it can
  consolidate two or more complete physical fragments into a smaller hole. It
  never splits an existing fragment just to fill a hole. Internal VCN
  boundaries obey NT4's move granularity. Sparse and compressed streams are
  excluded.
- **Verification.** Every attempted placement is checked against a fresh
  extent map. A verified reduction that leaves several fragments is
  **partially improved**, not defragmented, and the pass is incomplete (exit
  4). One such consolidation is attempted per eligible stream per pass, and
  partial improvements are saved in the boot result. A partially improved
  stream is counted on its own; the "no full-size run" counter keeps counting
  the streams that did not fit at all.
- **Refusals and retries.** Transient allocation collisions get bounded
  retries. When the driver refuses a target outright (an invalid parameter),
  only that place is given up and marked unusable; the free space around it
  stays available and the run continues. Clusters a failed move had reserved
  are given back as soon as the fresh extent map shows the data is not there,
  so a refusal cannot make the volume look full.
- **Replies are untrusted.** Bitmap and retrieval replies are validated
  (sizes, run counts, VCN continuity). NT4 can answer a resident stream with
  a zero-length reply; it is accepted only after checking the stream's
  allocation through the native file information, and a filled map whose
  reply length was left out must match that allocation. Malformed replies
  remain errors.
- **`-m`** validates NTFS geometry, the `$MFT` mapping pairs, `$MFTMirr` and
  the allocation bitmap, read-only. It does not need the backup boot sector
  past the mounted filesystem's readable boundary. It is not a disk integrity
  check and never moves metadata.

### The native boot pass

`--boot-defrag` installs `hsdefrgb.exe` in System32 (written to a staging
file, then moved into place write-through) and adds
`hsdefrgb C: /x /f`-style entries to `BootExecute`, removing only this
drive's earlier `hsdefrgb` entries and keeping everything else byte for byte
(`bootcfg.c`; malformed lists are refused rather than rewritten). The
result file `C:\hsdefrag.boot` is a standalone 4 KiB checksummed record
(`bootlog.c`); it need not be contiguous.

At boot the helper validates the pending record, saves a RUNNING marker,
removes its own entry, then runs the pass, so a crash at any step never runs
the pass twice. If the record is missing, the wrong size, fails its checksum
or names another drive, the helper removes its own entry for that drive and
leaves the file untouched. If saving the diagnostics fails, new moves stop.
The pass enumerates unnamed and named data streams, including named streams
on directories. It traverses ordinary system files, but does not force
handles to paging files or registry hives; a zero in-use counter alone does
not prove a particular system file was present or moved. The result file is
excluded by root name and file identity.

The final flush needs write access on the volume handle, so the helper opens
the mounted volume for read/write access; it still performs no raw writes,
locking or dismounting. Flush errors are fatal.

An earlier native pass completed on the NT4 target: 1,541 streams examined,
one fragmented stream made contiguous, no file-pass errors. The current
allocation changes still require qualification on the NT4 target
([QUALIFY.md](QUALIFY.md)).

### Removed functionality

There is **no raw MFT relocation, MFT-only scheduling, raw undo/recovery
engine, offline image utility, or newer-Windows metadata-move path**. The
native helper never requests an exclusive volume lock or dismount, writes raw
metadata, or forces a reboot. `--boot-mft` and `--experimental` are gone.

NTFS has already mounted the system volume when BootExecute runs. The former
MFT stage failed with `STATUS_ACCESS_DENIED` (`0xC0000022`) when it tried to
lock C:; it was removed, not disguised as a successful move. `$MFT`,
`$MFTMirr`, `$LogFile`, directory indices and other reserved metadata are not
defragmented. (Ordinary `.evt` event logs are files, unlike `$LogFile`.)

## The FAT engine (Windows 9x and MS-DOS)

`fatfs.c` reads and writes FAT12/16/32 through an abstract sector device;
`fatdefrag.c` plans and performs the moves. Both are free of OS calls and are
tested on the host. `fatwin9x.c` (Windows 9x) and `fatdos.c` (MS-DOS) supply
the sector I/O, the locking and the list of files to keep in place;
`fatreport.c` writes the report for both.

### What a full defragmentation does

- Fragmented files and folders move into the first free run that holds them
  whole. Moving a folder also updates its `.` entry and the `..` entries of
  its subfolders.
- The free space is then consolidated in sweeps from the start of the drive.
  Each gap is filled with the rest of the file just before it if that fits,
  else with the largest file or folder from further up that fits; when
  nothing fits, the files after the gap slide down so that it joins the next
  free space (a long file that would need many small steps moves up out of
  the way and comes back down whole). A file or folder too large for any gap
  is moved piece by piece, so the gaps between its pieces merge, and is then
  made contiguous. Every file moves at most once per sweep, so the data moved
  stays close to the size of what is out of place; sweeps repeat until
  nothing improves.
- `-a` runs the same plan on the in-memory FAT and reverts it, so the
  prediction and the real run agree.

### What stays in place, and why

- The first cluster of the FAT32 root folder: the boot sector points at it.
  The rest of the root folder moves like any folder; on FAT12/16 the root
  folder lies outside the file area.
- Hidden system files in the root folder that boot code may expect at a fixed
  place: `IO.SYS`, an MS-DOS 6 `MSDOS.SYS`, `NTLDR`, DriveSpace volumes such
  as `DRVSPACE.000`, and any other hidden system root file not known to be
  safe to move. Known boot loaders (`IO.SYS`, `WINBOOT.SYS`, `IBMBIO.COM`,
  `IBMDOS.COM`, `NTLDR`) and a binary `MSDOS.SYS` stay even if their
  attributes were cleared.
- `LDLINUX.SYS` wherever it is: the SYSLINUX boot sector points at it.
- Under Windows only: the swap file, the registry (`SYSTEM.DAT`, `USER.DAT`,
  `CLASSES.DAT`) and the files Windows reports it must keep in place.
- Hidden root files that DOS and Windows open by name do move: the Windows
  95/98 text `MSDOS.SYS`, logs and backups (`BOOTLOG.TXT`, `DETLOG.TXT`,
  `SETUPLOG.TXT`, `SUHDLOG.DAT`, `SYSTEM.1ST`, `W95UNDO.DAT`, ...) and the
  DriveSpace drivers (`DRVSPACE.BIN`, `DBLSPACE.BIN`).

### Keeping the volume consistent

- **Copy before switch.** The data is copied into free clusters and read back,
  the new clusters are linked in the FAT, the directory entry (or, for a later
  piece of a file, the FAT entry before it) is switched over, and only then
  are the old clusters freed. FAT and directory writes are read back too. An
  interrupted operation can leave lost clusters, differing FAT copies or
  stale `..` entries for ScanDisk, but never a file pointing at someone else's
  data.
- **Folders.** Before a folder's first cluster moves, the first sector of every
  subfolder is read and its `..` checked; if any cannot be read or does not
  point at the folder, the folder is not moved and the run stops. If a `..`
  then cannot be written, the move is kept and the run goes on: the old copy
  stays allocated so the stale `..` entries still resolve, and its chain is
  ended at its own last cluster so it never leads into live data. ScanDisk
  frees it as lost clusters.
- **FSInfo.** The FAT32 free-cluster hint is invalidated before the first FAT
  change of a run, so an interrupted run never leaves a free count that is too
  high, and Windows recounts it.
- **Refusal.** Nothing is moved if the first scan finds cross-linked files,
  malformed chains or directory links, size/allocation mismatches, unreadable
  folders or differing mirrored FATs. Invalid or overflowing BPB geometry is
  rejected before the FAT is loaded. Folder nesting of 64 levels or more is
  refused rather than silently skipping a subtree.
- **The active FAT.** FAT32 may disable mirroring (`BPB_ExtFlags` bit 7) and
  select one live FAT (bits 0-3); the engine reads and writes that copy. With
  mirroring on, every copy is kept identical.
- Read-back verification does not prove that a drive or controller has
  persisted its cache, or that a sector cannot be torn during power loss.

### Windows 9x locking

Locking follows Microsoft's documented protocol for disk utilities
("Exclusive Volume Locking", `Int 21h AX=440Dh`, category `48h` on FAT32,
`08h` otherwise). Sectors are read and written through the VWIN32 VxD
(`Int 21h AX=7305h`, falling back to `Int 25h/26h`).

- An idle or removable drive gets the exclusive level 0 lock for the whole run.
- The live system drive always has open files, so the tool holds a level 1
  lock and raises it to level 2 and then 3 for each short burst of writes, as
  the Windows 95 Disk Defragmenter does. At level 3 Windows flushes its
  caches; the lock flag tells whether anything changed the drive since the
  last burst, in which case the FAT is read again and the list of files in use
  is taken again. Between bursts the lock drops back to level 1. Nothing is
  printed, nothing allocates and nothing waits while level 3 is held; a failed
  release of level 3 is retried at once.
- The list of files in use comes from Windows (`Enumerate Open Files`, the
  unmovable and the full list, plus `Find Swap File`) and is cross-checked
  before anything is written: when `hsdefrag.exe` runs from the drive it must
  be on the list, and the drive with the Windows folder must have files in
  use. Otherwise nothing is written ("Windows did not list hsdefrag.exe among
  the files in use" / "Windows listed no files in use on the system drive").
  A listed file whose name cannot be found on the drive is never assumed to be
  gone: the list is taken again, and after three misses the run stops ("cannot
  find the file in use").
- If the lock cannot be taken (e.g. another disk utility holds it), nothing
  is written. The NT-only options are rejected on 9x.

### Windows 9x boot scheduling

`--boot-defrag` copies `hsdefrag.exe` to the Windows folder as `HSDEFRAG.EXE`
(or to the root of the boot drive if Windows is elsewhere), writes
`C:\HSDEFRAG.JOB` and adds one line to `C:\AUTOEXEC.BAT` after a leading
`@ECHO OFF`, e.g.
`@IF EXIST C:\HSDEFRAG.JOB C:\WINDOWS\HSDEFRAG.EXE --boot C:\HSDEFRAG.JOB`.

- AUTOEXEC.BAT keeps its attributes; a replacement is written, flushed and
  read back as `AUTOEXEC.$HS` before the original is renamed to
  `AUTOEXEC.HS0`. The program and the job use `.HST` staging and `.HSB`
  recovery files. A failed installation restores the original where possible;
  if that fails too, the recovery files are kept and an error is reported. The
  scheduler refuses to overwrite leftover staging or recovery files.
- An existing line that runs the program from another path is replaced.
- After a pass the line stays but does nothing without a job;
  `--boot-cancel` removes it. The copied `HSDEFRAG.EXE` stays for use in
  MS-DOS mode.
- After a failed or interrupted update, inspect `--boot-status` and the
  recovery files before rebooting. If `AUTOEXEC.BAT` is missing, restore
  `AUTOEXEC.HS0` to that name; a `.HSB` file holds the previous program or
  job. Preserve any existing final file separately before restoring its
  recovery copy, and keep recovery files until the final files and scheduled
  drives have been checked. These steps do not guarantee power-loss
  durability.

### MS-DOS disk access

The MS-DOS side picks the absolute disk call the running DOS has:

| Call | Form | DOS |
| --- | --- | --- |
| `Int 21h` `AX=7305h` | the disk I/O packet at `DS:BX` | 7.0A and later (Windows 95/98); the only one that knows FAT32 |
| `Int 25h`/`26h`, `CX=FFFFh` | the same packet at `DS:BX` | 3.31 and later, so MS-DOS 6.2 has it |
| `Int 25h`/`26h`, the old form | the sector count in `CX`, the first sector in `DX`, the buffer at `DS:BX` | every DOS, but only the first 64K sectors of a drive |

The packet holds the 32-bit sector number, the number of sectors and the far
address of the transfer window (offset in the low word, segment in the high
word). `Int 25h` and `26h` return with the flags still on the stack, which the
real-mode thunks remove. The call is chosen by reading the boot sector and
keeping the first one that returns a sector ending in the `55AA` signature,
so a call that only looks like it works is never used.

Before writing, the MS-DOS side walks DOS's system file tables: a file open
for writing on the drive refuses the run, and files open for reading are kept
in place. It also checks DOS's current directory structures for SUBST and
JOIN drives that lead into the volume. The layout of both tables is decoded
in `fatdisk.c` and tested on the host. DOS's buffers are flushed and SMARTDRV's
write cache is committed and switched off for the run.

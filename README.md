# House Soft Defrag 1.0

A console **whole-volume defragmenter for Windows NT 4.0, Windows 95/98/Me
and MS-DOS**. You select a drive, not individual files. One file,
`hsdefrag.exe`, runs on all of them and picks the right engine at startup:

| Started on | Filesystems | How data is moved |
|---|---|---|
| Windows NT 4.0 | NTFS, FAT | by NTFS/FAT itself, through NT's defragmentation interface |
| Windows NT 4.0, at boot | NTFS | the same, by a native helper that runs before Windows starts |
| Windows 95/98/Me | FAT12/16/32 | by House Soft Defrag's own FAT engine, with Windows' volume locks |
| MS-DOS 6.2 and later | FAT12/16 (FAT32 on MS-DOS 7.1) | by the same FAT engine |

It needs no installation, no kernel driver and no FPU (a 386 or 486SX
without one is fine; Windows NT 4.0 itself needs a 486).

> **Keep a verified backup before defragmenting.** Moves are verified, but
> this is not a journaled filesystem: a power loss in the middle of a move
> can leave work for ScanDisk or chkdsk. Do not interrupt the power.

## Quick start

```bat
hsdefrag C: -a         analyze drive C: (read-only; the default)
hsdefrag C: -d         defragment drive C:
hsdefrag C: -d -v      ... and list every file moved or kept in place
```

| Option | Meaning |
|---|---|
| `-a`, `--analyze` | analyze only: report fragmentation and what `-d` would do (default) |
| `-d`, `--defrag` | defragment the drive |
| `-v`, `--verbose` | per-file output |
| `-m`, `--mft` | NT only: also report NTFS `$MFT` fragmentation (read-only) |
| `--boot-defrag` | defragment at the next start (NT: native NTFS pass; 9x: MS-DOS pass) |
| `--boot-defrag --dry-run` | check that scheduling would work, writing nothing |
| `--boot-status` | show the scheduled job and the result of the last boot pass |
| `--boot-cancel` | remove this drive's scheduled boot pass |
| `--no-bitmap`, `--no-files`, `--no-meta` | NT only, with `-a`: skip a part of the analysis (troubleshooting) |
| `-h`, `--help` | show the help |

Only one action can be given at a time. `-m` cannot be combined with
`--no-meta`. Administrator rights are required on Windows NT.

To keep complete diagnostics, redirect the output to a file on **another
drive**:

```bat
hsdefrag C: -d -v > D:\defrag.txt 2>&1
```

## Windows NT 4.0

`hsdefrag C: -d` defragments the ordinary file data on the drive. NTFS (or
FAT) moves every cluster itself; House Soft Defrag only decides where each file
should go and checks the result:

- It looks for the smallest free run that holds a whole file, so large free
  areas stay large. If none fits, it can still join two or more of the
  file's fragments into a smaller free run.
- Every move is checked against a fresh map of the file. A file that got
  fewer but still several fragments is reported as **partially improved**,
  not as defragmented, and the run ends with exit code 4.
- Files that are in use, compressed, sparse, encrypted, device or reparse
  data are skipped ("special" in the report). Reparse points are not
  followed. `$MFT`, `$MFTMirr`, `$LogFile`, directory indices and other NTFS
  metadata are never moved; `-m` only reports how fragmented `$MFT` is.
- A file may stay fragmented when there is no big enough free space or NTFS
  refuses a move. There is no promise of free-space consolidation or optimal
  placement.
- Ctrl+C stops before the next file and exits with 4; each move is one
  filesystem request, so the volume stays consistent.

### At boot (NTFS)

```bat
hsdefrag C: --boot-defrag
hsdefrag C: --boot-status
hsdefrag C: --boot-cancel
```

Files Windows keeps open cannot move while it runs. `--boot-defrag`
installs a native helper (`%SystemRoot%\System32\hsdefrgb.exe`) and adds a
one-shot command to the registry's `BootExecute` list, keeping the entries
that are already there. **Reboot yourself** when it succeeds. At the next
start the helper defragments the drive before Windows starts, then removes
its own entry; a job is never retried automatically.

The result is saved in `C:\hsdefrag.boot` (a 4 KiB checksummed status record,
not an undo journal) and shown by `--boot-status`: pending, running or
interrupted, completed or failed, with counts of fragmented files, moves and
exclusions. Scheduling again reuses a successfully completed result; a
pending, failed, interrupted or damaged result is protected until you have
looked at it and cancelled the job. `--boot-cancel` removes only this
drive's helper command and keeps the result file; it also works when the
drive can no longer be reached.

The boot pass is NTFS-only, with clusters up to 4 KiB, and NTFS stays in
charge of every move: there is no raw disk writing, volume locking or
forced reboot.

## Windows 95/98/Me

Windows 9x has no defragmentation interface, so House Soft Defrag uses its own
FAT12/16/32 engine there and runs in a console window. `-d` does what the
Disk Defragmenter's "Full defragmentation (both files and free space)" does:

- Fragmented files **and folders** are made contiguous, and then the free
  space is consolidated, in sweeps from the start of the drive.
- Hidden, system and read-only files move like any other file.
- **Left in place**: the boot files in the root folder that boot code may
  expect at a fixed place (`IO.SYS`, an MS-DOS 6 `MSDOS.SYS`, `NTLDR`,
  `WINBOOT.SYS`, `IBMBIO.COM`, `IBMDOS.COM`, DriveSpace volumes such as
  `DRVSPACE.000`), `LDLINUX.SYS`, the first cluster of the FAT32 root folder,
  and the files Windows keeps open: the swap file (`WIN386.SWP`), the
  registry and running programs and DLLs. The boot-time pass below moves
  those too. Free space next to them can stay split; the report lists the
  largest gaps left and what keeps each one open.
- Each move copies the data, reads it back, links the new clusters and only
  then switches the file over and frees the old clusters.
- Nothing is moved if the first scan finds any inconsistency (cross-linked
  files, broken chains, size mismatches, unreadable folders, differing FAT
  copies). **Run ScanDisk first** after filesystem errors or an interrupted
  run.

`-a` takes no lock and writes nothing. It plans the full defragmentation in
memory with the same code `-d` uses and reports what it would do ("would
make N files and folders contiguous and move M others to consolidate the
free space"). It recommends `-d` only when that would improve something.

The drive is locked the way Windows' own Disk Defragmenter does it, so
Windows and your programs keep running; other disk activity slows the run
down, so close programs for the fastest result. If the lock cannot be
taken, or Windows' list of files in use does not look right, nothing is
written. Ctrl+C stops between write bursts, leaving a consistent drive.

### Boot-time pass (Windows 95/98)

```bat
hsdefrag C: --boot-defrag
hsdefrag C: --boot-status
hsdefrag C: --boot-cancel
```

`--boot-defrag` schedules House Soft Defrag in MS-DOS for the next start,
before Windows loads, like ScanDisk after an improper shutdown. At that
point the swap file, the registry and Windows' own files are ordinary files
and move too.

- `hsdefrag.exe` copies itself to the Windows folder as `HSDEFRAG.EXE`, writes
  the job file `C:\HSDEFRAG.JOB` (the drives, with their serial numbers) and
  adds one line to `C:\AUTOEXEC.BAT`. `--dry-run` shows the plan without
  writing anything.
- At the next start the screen shows a 5-second countdown: ESC skips the
  pass, any other key starts it at once, and ESC during the pass stops it
  safely. The job file is deleted first, so the pass runs only once even if
  the computer is reset. The report goes to `C:\HSDEFRAG.LOG`; then Windows
  starts as usual.
- `--boot-status` shows the scheduled drives, whether everything is in place
  and the report of the last pass. `--boot-cancel` removes the drive from the
  job, and with the last drive removes the job and the AUTOEXEC.BAT line.
- Windows Me does not run MS-DOS programs from AUTOEXEC.BAT, so scheduling is
  refused there; run `hsdefrag` from a Windows Me startup disk instead.

Changing three files cannot be one atomic step. If scheduling fails or is
interrupted, check `--boot-status` **before rebooting**: if `AUTOEXEC.BAT` is
missing, rename `AUTOEXEC.HS0` back to it; a `.HSB` file holds the previous
copy of the program or job file. The details are in
[docs/DESIGN.md](docs/DESIGN.md#windows-9x-boot-scheduling).

## MS-DOS

```bat
hsdefrag C: -a
hsdefrag C: -d -v
```

Started in MS-DOS (a "Command prompt only" start, MS-DOS mode or a startup
floppy), `hsdefrag.exe` runs the same FAT engine with the same options and
output. In a DOS box under Windows, Windows starts the Windows side instead.

- MS-DOS 6.2 and later are supported. FAT32 needs MS-DOS 7.1 (Windows 95
  OSR2 or 98).
- It locks the drive (MS-DOS 7), keeps DOS's buffers and SMARTDRV in step
  with its writes, and leaves files DOS has open in place.
- A file open for writing on the drive refuses the run, so **do not redirect
  the output to the drive you defragment**.
- Everything moves except the boot files listed above, `LDLINUX.SYS` and the
  first cluster of the FAT32 root folder; the swap file and the registry
  move too. Lost clusters (allocated, but part of no file) are counted;
  ScanDisk frees them.
- ESC stops the run safely.

SUBST, JOIN, network, CD-ROM and DriveSpace drives are refused.

## Exit codes

| Exit | Meaning |
|------|---------|
| 0 | Completed within the reported coverage, or scheduling/status/cancel succeeded |
| 1 | Invalid command line |
| 2 | Drive not accessible |
| 3 | Unsupported filesystem, driver or geometry, or the run was refused for safety |
| 4 | Incomplete: some work was left undone, stopped by the user, or a failed/interrupted boot pass |
| 5 | Runtime, scheduling, allocation, I/O or output failure |

Files that are skipped by design do not by themselves make a run
incomplete. When several results apply (for example `-d -m`), the most
serious one is returned, in the order 5, 3, 2, 4, 0.

## Building

```sh
make          # dist/hsdefrag.exe (Windows and MS-DOS in one file)
make windows  # dist/win/hsdefrag.exe: Windows only, needs only MinGW
make dos      # dist/dos/hsdefrag.exe: MS-DOS only, needs only DJGPP
make dist     # + dist/hsdefrag-1.0.zip and dist/hsdefrag-1.0.img (1.44 MB floppy)
make test     # host and Wine tests
```

The Windows-only build has no MS-DOS part, so it cannot schedule the
Windows 95/98 boot-time pass (which runs `hsdefrag.exe` as an MS-DOS
program) and says so; everything else works as in the full build. The
MS-DOS-only build refuses to run in a DOS box under Windows, like the full
one. The release zip and floppy always hold the full `hsdefrag.exe`.

You need `i686-w64-mingw32-gcc`, a DJGPP cross compiler
(`i586-` or `i686-pc-msdosdjgpp-gcc`, found in `~/.local/djgpp/bin` or on
the `PATH`; `make DJCC=...` names one), a host C compiler, and for the tests
Wine. `make dist` uses mtools for the floppy and
7-Zip (or zip, bsdtar or Python) for the archive. The release zip and the
floppy hold `HSDEFRAG.EXE`, this README, `LICENSE` and the CWSDPMI/DJGPP
terms in `DOS\`.

More:

- [docs/DESIGN.md](docs/DESIGN.md): how the engines work, the safety rules
  and what was removed.
- [docs/TESTING.md](docs/TESTING.md): what the automated tests cover.
- [docs/QUALIFY.md](docs/QUALIFY.md): the checklist for testing on real
  machines.

## Licence

House Soft Defrag is released under the MIT licence; see [LICENSE](LICENSE).
`hsdefrag.exe` also contains the CWSDPMI DPMI host (GPL with an exception for
DJGPP programs) and the DJGPP C library, under their own terms in
[dos/](dos/); `dos/README` says where to get their source code. Ship
`dos/README` and `dos/COPYING*` with `hsdefrag.exe`; `make dist` does.

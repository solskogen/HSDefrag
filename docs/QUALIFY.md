# Qualifying House Soft Defrag on real machines

The automated tests (`make test`) run the engines against in-memory
images and mocked Windows calls. They cannot show
that a real disk, controller, BIOS, DOS cache or NT driver behaves as
expected. Before a release, or after a change to anything that moves data,
run the checks below on real hardware.

Use machines and disks whose contents you can lose. Keep a verified backup
anyway.

## What to record

For every run, keep:

- the machine (CPU, RAM, disk/controller), the OS and version, and the drive
  size and filesystem;
- the exact command line and the full output (`> X:\run.txt` on a drive
  other than the one being defragmented; under MS-DOS the redirected file
  must not be on that drive);
- the exit code (`echo %errorlevel%` on NT; on DOS/9x a batch file with
  `if errorlevel` lines);
- the integrity checks below, before and after.

## Integrity checks (before and after every run)

1. **Filesystem check**: `chkdsk X:` (NT), `scandisk X: /checkonly` (9x)
   or `scandisk X: /checkonly` / `chkdsk X:` (MS-DOS). It must report no
   errors after the run that were not there before.
2. **File contents**: checksum every file before and after and compare.
   On another machine: image the disk and compare with `md5sum` of each file
   (e.g. `mtools`/`mtype` for FAT images), or copy the drive's files to a
   second drive before the run and compare with `fc /b` afterwards.
3. **Analysis**: `hsdefrag X: -a` before and after. After `-d`, the
   fragmented-file count should drop, and on FAT the "would make N files
   contiguous" plan of the first analysis should match what `-d` did.

## Test matrix

Run the rows that apply to the change. A full release runs all of them.

| # | Platform | Drive | Command | Expect |
|---|---|---|---|---|
| 1 | MS-DOS 6.22 | FAT16 hard disk, fragmented | `hsdefrag C: -a`, then `-d -v` | exit 0, checks clean |
| 2 | MS-DOS 6.22 | 1.44 MB floppy (FAT12) | `hsdefrag A: -d` | exit 0, checks clean |
| 3 | MS-DOS 7.x (Win9x "command prompt only") | FAT16 and FAT32 | `hsdefrag C: -d -v` | exit 0, swap file and registry moved |
| 4 | MS-DOS, SMARTDRV loaded with write caching | FAT16 | `hsdefrag C: -d` | exit 0, checks clean after a normal reboot |
| 5 | MS-DOS | output redirected to the same drive | `hsdefrag C: -d > C:\x.txt` | refused (file open for writing), nothing changed |
| 6 | Windows 95/98, idle | second FAT drive | `hsdefrag D: -d -v` | exit 0 |
| 7 | Windows 95/98 | system drive with programs running | `hsdefrag C: -d -v` | exit 0 or 4, files in use kept in place |
| 8 | Windows 95/98 | system drive | `hsdefrag C: --boot-defrag`, reboot | the pass runs before Windows, `C:\HSDEFRAG.LOG` written, Windows starts |
| 9 | Windows 95/98 | — | `--boot-status`, `--boot-cancel` | the AUTOEXEC.BAT line and job are removed |
| 10 | Windows NT 4.0 | NTFS data drive | `hsdefrag D: -a`, `-d -v`, `-d -m` | exit 0 or 4, chkdsk clean |
| 11 | Windows NT 4.0 | NTFS system drive | `hsdefrag C: --boot-defrag`, reboot, `--boot-status` | completed, chkdsk clean |
| 12 | Windows NT 4.0 | — | `--boot-cancel` after scheduling | BootExecute keeps `autocheck autochk *` |
| 13 | 386 or 486SX without FPU | MS-DOS, FAT16 | `hsdefrag C: -d` | runs (no FPU needed) |

## Interruption tests

A defragmenter must leave a consistent filesystem when it is stopped.

- **ESC / Ctrl+C**: start `-d` on a fragmented drive and stop it after some
  moves (ESC under MS-DOS and in the boot pass, Ctrl+C under Windows). The
  integrity checks must be clean; the exit code is 4.
- **Power loss** (only on a disk you can lose): cut power during `-d`. After
  restarting, run ScanDisk/chkdsk first. Lost clusters, differing FAT copies
  or stale `..` entries may be reported and fixed; file contents of files
  that were not being moved must be intact. Record exactly what was found.
- **Reset during the 9x boot pass**: press reset during the MS-DOS pass at
  startup. The job file is deleted before the pass starts, so it must not
  run again on the next start.

## Reporting

A failure report is most useful with: the output, the exit code, the
before/after checks, and, if possible, an image of the drive before the run.
